#include "chatroom.h"
#include "ui_chatroom.h"
#include "fileuploader.h"
#include <QPushButton>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QFileInfo>
#include <QFileDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QJsonArray>
#include <QtConcurrent>
#include <QLineEdit>

#include <QDataStream>
#include <QtEndian>
#include <QElapsedTimer>
#include <QDir>
#include <QHostAddress>
#include <QUrl>
#include <QUrlQuery>

// ===== 工具：下载超时（Qt5 全兼容） =====
// ===== 工具：下载超时（Qt5 全兼容） =====
static QTimer* attachTimeoutToReplyDL(QNetworkReply* reply, int timeoutMs)
{
    if (!reply || timeoutMs <= 0) return nullptr;
    reply->setProperty("timedOut", false); // 初值

    QTimer* timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("timedOut", true);
        if (!reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, timer, &QObject::deleteLater);
    timer->start(timeoutMs);
    return timer;
}

// 以文件路径作为 key，确保 filessendList 里有一条 item；如果没有则创建
static QStandardItem* ensureTransferItem(QStandardItemModel* model,
                                         QHash<QString, QStandardItem*>& index,
                                         const QString& key, const QString& initialText)
{
    if (index.contains(key)) return index[key];
    auto* it = new QStandardItem(initialText);
    it->setEditable(false);
    model->appendRow(it);
    index.insert(key, it);
    return it;
}

// 删除一条传输项
static void removeTransferItem(QStandardItemModel* model,
                               QHash<QString, QStandardItem*>& index,
                               const QString& key)
{
    auto it = index.find(key);
    if (it == index.end()) return;
    QStandardItem* item = it.value();
    const int row = item->row();
    model->removeRow(row);
    index.remove(key);
}

ChatRoom::ChatRoom(QTcpSocket *socket, const QString &username, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChatRoom),
    m_socket(socket),
    m_username(username)
{
    ui->setupUi(this);

    // 初始化用户列表模型
    m_usersModel = new QStringListModel(this);
    ui->usersList->setModel(m_usersModel);

    // 初始化文件列表模型并绑定到 QListView
    m_filesModel = new QStringListModel(this);
    ui->filesList->setModel(m_filesModel);
    m_transfersModel = new QStandardItemModel(this);
    ui->filessendList->setModel(m_transfersModel);

    // 双击保存
    connect(ui->filesList, &QListView::doubleClicked, this, &ChatRoom::on_filesList_doubleClicked);

    // 固定窗口大小 取消边框
    setWindowFlag(Qt::FramelessWindowHint);
    setFixedSize(width(), height());
    ui->sendEdit->installEventFilter(this);
    ui->sendEdit->setAcceptRichText(false); // 可选：避免发出富文本 HTML

    // 如果有传入socket，设置相关信号槽
    if (m_socket) {
        // 先把 socket 上现有的 readyRead / error / disconnected 连接全部拆掉
        QObject::disconnect(m_socket, &QTcpSocket::readyRead,    nullptr, nullptr);
        QObject::disconnect(m_socket, &QTcpSocket::disconnected, nullptr, nullptr);
        QObject::disconnect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                            nullptr, nullptr);

        qDebug() << "ChatRoom ctor socket:" << m_socket;
        connect(m_socket, &QTcpSocket::readyRead, this, &ChatRoom::handleSocketReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &ChatRoom::handleSocketDisconnected);
        connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, &ChatRoom::handleSocketError);

        const QHostAddress addr = m_socket->peerAddress();
        if (!addr.isNull()) m_httpHost = addr.toString();
    }
    if (m_httpHost.isEmpty()) m_httpHost = QStringLiteral("127.0.0.1");
    m_httpPort = 9080;

    // 进入聊天室，主动请求一次在线信息(不要立刻发送，不然就会触发退出)
    QTimer::singleShot(0, this, &ChatRoom::requestOnlineInfo);
    m_onlineTimer = new QTimer(this);
    m_onlineTimer->setInterval(1000);
    connect(m_onlineTimer, &QTimer::timeout, this, &ChatRoom::requestOnlineInfo);
    m_onlineTimer->start();
}

ChatRoom::~ChatRoom()
{
    if (m_socket) {
        m_socket->disconnect(); // 断开所有信号槽连接
        m_socket->deleteLater(); // 延迟删除
        m_socket = nullptr;
    }
    delete ui;
}

/* 窗口移动,TODO:显示窗口可以移动的区域选择 */
void ChatRoom::mousePressEvent(QMouseEvent *event)
{
    mPos = event->globalPos() - this->pos();
}
void ChatRoom::mouseMoveEvent(QMouseEvent *event)
{
    this->move(event->globalPos() - mPos);
}

/* ===================== 网络接收与分发 ===================== */

void ChatRoom::handleSocketReadyRead()
{
    if (!m_socket) return;

    const QByteArray chunk = m_socket->readAll();
    if (chunk.isEmpty()) return;
    m_recvBuf.append(chunk);

    for (;;) {
        int nl = m_recvBuf.indexOf('\n');
        if (nl < 0) break;

        QByteArray line = m_recvBuf.left(nl);
        m_recvBuf.remove(0, nl + 1);

        line = line.trimmed();
        if (line.isEmpty()) continue;

        if (!line.startsWith('{')) {
            qWarning() << "[recv] non-json line ignored:" << line.left(128);
            continue;
        }

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            dispatchMessage(doc.object());
        } else {
            qWarning() << "[recv] json parse error:" << err.errorString()
                       << " line=" << line.left(256);
        }
    }

    const int kMaxBuf = 4 * 1024 * 1024;
    if (m_recvBuf.size() > kMaxBuf) {
        qWarning() << "[recv] buffer overflow, clearing" << m_recvBuf.size();
        m_recvBuf.clear();
    }
}


void ChatRoom::dispatchMessage(const QJsonObject& obj)
{
    const QString action = obj.value("action").toString();
    if (action == "chat") {
        handleChatMessage(obj);
    } else if (action == "online_info") {
        handleOnlineInfo(obj);
    } else if (action == "file_meta") {          // ★ 新增
        handleFileMeta(obj);
    } else {
        qDebug() << "Unknown action:" << action << obj;
    }
}

/* ===================== 文本消息 ===================== */
void ChatRoom::on_sendBt_clicked() {
    //1. 获取输入
    QString text = ui->sendEdit->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
        //不发送空消息
    }
    //2. 构造 JSON
    QJsonObject msg;
    msg["action"] = "chat";
    msg["text"] = text;
    msg["from"] = m_username;
    QJsonDocument doc(msg);
    //3. 发送（带换行符，方便服务器按行解析）
    m_socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
    m_socket->flush();
    //4. 在 messageBrowser 上显示自己消息（橙色）
    QString timeStr = QDateTime::currentDateTime().toString("MM-dd HH:mm:ss");
    ui->messageBrowser->append(QString(u8"<span style='color:orange;'>[%1] %2(我)：%3</span>")
                               .arg(timeStr)
                               .arg(m_username)
                               .arg(text.toHtmlEscaped()));
    //5. 清空输入框
    ui->sendEdit->clear();

}


void ChatRoom::on_sendFileBt_clicked() {
    const QString filePath = QFileDialog::getOpenFileName(this, u8"选择要上传的大文件");
    if (filePath.isEmpty()) return;

    // 为这个上传在 filessendList 建/取一个条目（key 用文件完整路径，文本先放“准备中”）
    const QString key = filePath; // 唯一键
    QStandardItem* item = ensureTransferItem(m_transfersModel, m_transferIndex, key,
                                             QString::fromUtf8("↑ %1  准备中...")
                                             .arg(QFileInfo(filePath).fileName()));

    // 如果已有正在上传的 FileUploader，先取消并清理（可选）
    if (m_uploader) { m_uploader->cancel(); m_uploader->deleteLater(); m_uploader = nullptr; }

    m_uploader = new FileUploader(this);
    m_uploader->setProperty("filePath", filePath); // 方便回调里拿名字
    const QString baseName = QFileInfo(filePath).fileName();

    // 进度：更新条目文本
    connect(m_uploader, &FileUploader::progress, this,
            [this, key, baseName](qint64 sent, qint64 total, double speed, int pct, int /*seq*/) {
                const QString sp = (speed > 0)
                                   ? QString::number(speed/1024.0, 'f', 1) + " KB/s"
                                   : QStringLiteral("- KB/s");
                QStandardItem* it = ensureTransferItem(m_transfersModel, m_transferIndex, key, QString());
                it->setText(QString::fromUtf8("↑ %1  %2%%  (%3 / %4)  %5")
                                .arg(baseName)
                                .arg(pct)
                                .arg(QString::number(sent))
                                .arg(QString::number(total))
                                .arg(sp));
            });

    // 成功：从列表移除
    connect(m_uploader, &FileUploader::finished, this,
            [this, key]() {
                removeTransferItem(m_transfersModel, m_transferIndex, key);
                // 成功后不要刷 messageBrowser；filesList 会由服务端广播的 file_meta 填充
            });

    // 失败：条目改为失败，然后 3 秒后自动移除
    connect(m_uploader, &FileUploader::error, this,
            [this, key, baseName](const QString& msg, int code, int /*seq*/) {
                QStandardItem* it = ensureTransferItem(m_transfersModel, m_transferIndex, key, QString());
                it->setText(QString::fromUtf8("↑ %1  失败：%2 (HTTP %3)")
                                .arg(baseName, msg)
                                .arg(code));
                // 3 秒后移除失败项
                QTimer::singleShot(3000, this, [this, key]() {
                    removeTransferItem(m_transfersModel, m_transferIndex, key);
                });
                QMessageBox::critical(this, u8"上传失败",
                                      QString(u8"错误：%1（HTTP %2）").arg(msg).arg(code));
            });

    // 开始上传
    m_uploader->start(m_httpHost, m_httpPort, filePath, m_username);
}

void ChatRoom::handleChatMessage(const QJsonObject& obj)
{
    const QString from = obj.value("from").toString();
    const QString text = obj.value("text").toString();

    // 如果服务端会回显自己消息，可忽略
    if (!from.isEmpty() && from == m_username) return;

    const QString ts = QDateTime::currentDateTime().toString("MM-dd HH:mm:ss");
    ui->messageBrowser->append(
        QString(u8"<span style='color:#007bff;'>[%1] %2：%3</span>")
            .arg(ts)
            .arg(from.isEmpty() ? QStringLiteral("对方") : from.toHtmlEscaped())
            .arg(text.toHtmlEscaped())
    );
}

/* ===================== 在线信息 ===================== */
void ChatRoom::requestOnlineInfo()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return;

    QJsonObject req;
    req["action"] = "online_list";  // 和服务端约定
    m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    m_socket->flush();
}

void ChatRoom::handleOnlineInfo(const QJsonObject& obj)
{
    // 约定示例：
    // { "action":"online_info", "count":3, "users":["张三","李四","王五"] }
    int count = obj.value("count").toInt();

    QStringList users;
    QJsonArray arr = obj.value("users").toArray();
    users.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        users << v.toString();
    }

    // 防抖：无变化就不刷新 UI
    if (count == m_lastCount && users == m_lastUsers) {
        return;
    }
    m_lastCount = count;
    m_lastUsers = users;
    ui->userNumLb->setText(QString::number(count));

    m_usersModel->setStringList(users);

    // 也可在消息框里提示一下（可选）
    // ui->messageBrowser->append(QString("<i>在线人数：%1</i>").arg(count));
}

void ChatRoom::on_filesList_doubleClicked(const QModelIndex &index) {
    if (!index.isValid()) return;
    const QString name = m_filesModel->data(index, Qt::DisplayRole).toString();
    const QString savePath = QFileDialog::getSaveFileName(this, u8"另存为", name);
    if (savePath.isEmpty()) return;

    startDownload_(name, savePath);
}

/* ===================== 断线/错误/退出 ===================== */

void ChatRoom::handleSocketDisconnected()
{
    if (m_onlineTimer) m_onlineTimer->stop();
    QMessageBox::warning(this, u8"连接断开", u8"与服务器的连接已断开");
    emit back(); // 触发返回登录界面的信号
}

void ChatRoom::handleSocketError(QAbstractSocket::SocketError)
{
    QMessageBox::critical(this, u8"网络错误",QString(u8"发生网络错误: %1").arg(m_socket->errorString()));
}

void ChatRoom::on_exitRoomBt_clicked()
{
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    qApp->quit(); // 直接退出程序
}

bool ChatRoom::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->sendEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent*>(event);
        const bool isEnter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter);

        if (isEnter) {
            if (ke->modifiers() == Qt::NoModifier) {
                // 纯回车 -> 发送
                on_sendBt_clicked();
                return true; // 已处理，阻止 QTextEdit 自己换行
            }
            if (ke->modifiers() & Qt::ShiftModifier) {
                // Shift+Enter -> 换行（保留默认行为）
                return false;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

QString ChatRoom::detectHttpHost_() const {
    if (m_socket) {
        const QHostAddress addr = m_socket->peerAddress();
        if (!addr.isNull()) return addr.toString();
    }
    return QStringLiteral("127.0.0.1");
}

void ChatRoom::handleFileMeta(const QJsonObject& obj) {
    const QString name = obj.value("name").toString();
    // const QString from = obj.value("from").toString();   // 如果 UI 不展示就不用
    // const qint64  size = obj.value("size").toVariant().toLongLong();
    // const QString urlPath = obj.value("url").toString();
    if (name.isEmpty()) return;

    // 更新右侧“文件列表”（可双击下载）
    QStringList list = m_filesModel->stringList();
    if (!list.contains(name)) {
        list << name;
        m_filesModel->setStringList(list);
    }

    // 不再向 messageBrowser 写任何提示，避免刷屏
}



void ChatRoom::startDownload_(const QString& name, const QString& savePath) {
    // URL 构造
    QUrl url;
    url.setScheme("http");
    url.setHost(m_httpHost);
    url.setPort(m_httpPort);
    url.setPath("/download");
    QUrlQuery q; q.addQueryItem("name", name);
    url.setQuery(q);

    QNetworkRequest req(url);
    // Qt5 没有 TransferTimeoutAttribute，这里用 QTimer 兜底（5 分钟）
    m_dlReply = m_http.get(req);

    // 保存到文件
    QFile* out = new QFile(savePath, m_dlReply);
    if (!out->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, u8"保存失败", u8"无法写入：" + savePath);
        m_dlReply->abort();
        return;
    }

    // 绑定超时（属性里记录）
    attachTimeoutToReplyDL(m_dlReply, 300000);

    connect(m_dlReply, &QIODevice::readyRead, this, [this, out]() {
        out->write(m_dlReply->readAll());
    });

    connect(m_dlReply, &QNetworkReply::finished, this, [this, out, savePath]() {
        out->write(m_dlReply->readAll());
        out->close();

        const bool isTimeout = m_dlReply->property("timedOut").toBool();

        if (m_dlReply->error() == QNetworkReply::NoError) {
            QMessageBox::information(this, u8"下载完成", u8"已保存到： " + savePath);
        } else {
            QFile::remove(savePath);
            const QString msg = isTimeout ? QStringLiteral("下载超时") : m_dlReply->errorString();
            QMessageBox::critical(this, u8"下载失败", msg);
        }
        m_dlReply->deleteLater();
    });
}

