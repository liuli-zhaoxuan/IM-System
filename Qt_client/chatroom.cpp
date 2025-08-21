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

// ===== ���ߣ����س�ʱ��Qt5 ȫ���ݣ� =====
// ===== ���ߣ����س�ʱ��Qt5 ȫ���ݣ� =====
static QTimer* attachTimeoutToReplyDL(QNetworkReply* reply, int timeoutMs)
{
    if (!reply || timeoutMs <= 0) return nullptr;
    reply->setProperty("timedOut", false); // ��ֵ

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

// ���ļ�·����Ϊ key��ȷ�� filessendList ����һ�� item�����û���򴴽�
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

// ɾ��һ��������
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

    // ��ʼ���û��б�ģ��
    m_usersModel = new QStringListModel(this);
    ui->usersList->setModel(m_usersModel);

    // ��ʼ���ļ��б�ģ�Ͳ��󶨵� QListView
    m_filesModel = new QStringListModel(this);
    ui->filesList->setModel(m_filesModel);
    m_transfersModel = new QStandardItemModel(this);
    ui->filessendList->setModel(m_transfersModel);

    // ˫������
    connect(ui->filesList, &QListView::doubleClicked, this, &ChatRoom::on_filesList_doubleClicked);

    // �̶����ڴ�С ȡ���߿�
    setWindowFlag(Qt::FramelessWindowHint);
    setFixedSize(width(), height());
    ui->sendEdit->installEventFilter(this);
    ui->sendEdit->setAcceptRichText(false); // ��ѡ�����ⷢ�����ı� HTML

    // ����д���socket����������źŲ�
    if (m_socket) {
        // �Ȱ� socket �����е� readyRead / error / disconnected ����ȫ�����
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

    // ���������ң���������һ��������Ϣ(��Ҫ���̷��ͣ���Ȼ�ͻᴥ���˳�)
    QTimer::singleShot(0, this, &ChatRoom::requestOnlineInfo);
    m_onlineTimer = new QTimer(this);
    m_onlineTimer->setInterval(1000);
    connect(m_onlineTimer, &QTimer::timeout, this, &ChatRoom::requestOnlineInfo);
    m_onlineTimer->start();
}

ChatRoom::~ChatRoom()
{
    if (m_socket) {
        m_socket->disconnect(); // �Ͽ������źŲ�����
        m_socket->deleteLater(); // �ӳ�ɾ��
        m_socket = nullptr;
    }
    delete ui;
}

/* �����ƶ�,TODO:��ʾ���ڿ����ƶ�������ѡ�� */
void ChatRoom::mousePressEvent(QMouseEvent *event)
{
    mPos = event->globalPos() - this->pos();
}
void ChatRoom::mouseMoveEvent(QMouseEvent *event)
{
    this->move(event->globalPos() - mPos);
}

/* ===================== ���������ַ� ===================== */

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
    } else if (action == "file_meta") {          // �� ����
        handleFileMeta(obj);
    } else {
        qDebug() << "Unknown action:" << action << obj;
    }
}

/* ===================== �ı���Ϣ ===================== */
void ChatRoom::on_sendBt_clicked() {
    //1. ��ȡ����
    QString text = ui->sendEdit->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
        //�����Ϳ���Ϣ
    }
    //2. ���� JSON
    QJsonObject msg;
    msg["action"] = "chat";
    msg["text"] = text;
    msg["from"] = m_username;
    QJsonDocument doc(msg);
    //3. ���ͣ������з���������������н�����
    m_socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
    m_socket->flush();
    //4. �� messageBrowser ����ʾ�Լ���Ϣ����ɫ��
    QString timeStr = QDateTime::currentDateTime().toString("MM-dd HH:mm:ss");
    ui->messageBrowser->append(QString(u8"<span style='color:orange;'>[%1] %2(��)��%3</span>")
                               .arg(timeStr)
                               .arg(m_username)
                               .arg(text.toHtmlEscaped()));
    //5. ��������
    ui->sendEdit->clear();

}


void ChatRoom::on_sendFileBt_clicked() {
    const QString filePath = QFileDialog::getOpenFileName(this, u8"ѡ��Ҫ�ϴ��Ĵ��ļ�");
    if (filePath.isEmpty()) return;

    // Ϊ����ϴ��� filessendList ��/ȡһ����Ŀ��key ���ļ�����·�����ı��ȷš�׼���С���
    const QString key = filePath; // Ψһ��
    QStandardItem* item = ensureTransferItem(m_transfersModel, m_transferIndex, key,
                                             QString::fromUtf8("�� %1  ׼����...")
                                             .arg(QFileInfo(filePath).fileName()));

    // ������������ϴ��� FileUploader����ȡ����������ѡ��
    if (m_uploader) { m_uploader->cancel(); m_uploader->deleteLater(); m_uploader = nullptr; }

    m_uploader = new FileUploader(this);
    m_uploader->setProperty("filePath", filePath); // ����ص���������
    const QString baseName = QFileInfo(filePath).fileName();

    // ���ȣ�������Ŀ�ı�
    connect(m_uploader, &FileUploader::progress, this,
            [this, key, baseName](qint64 sent, qint64 total, double speed, int pct, int /*seq*/) {
                const QString sp = (speed > 0)
                                   ? QString::number(speed/1024.0, 'f', 1) + " KB/s"
                                   : QStringLiteral("- KB/s");
                QStandardItem* it = ensureTransferItem(m_transfersModel, m_transferIndex, key, QString());
                it->setText(QString::fromUtf8("�� %1  %2%%  (%3 / %4)  %5")
                                .arg(baseName)
                                .arg(pct)
                                .arg(QString::number(sent))
                                .arg(QString::number(total))
                                .arg(sp));
            });

    // �ɹ������б��Ƴ�
    connect(m_uploader, &FileUploader::finished, this,
            [this, key]() {
                removeTransferItem(m_transfersModel, m_transferIndex, key);
                // �ɹ���Ҫˢ messageBrowser��filesList ���ɷ���˹㲥�� file_meta ���
            });

    // ʧ�ܣ���Ŀ��Ϊʧ�ܣ�Ȼ�� 3 ����Զ��Ƴ�
    connect(m_uploader, &FileUploader::error, this,
            [this, key, baseName](const QString& msg, int code, int /*seq*/) {
                QStandardItem* it = ensureTransferItem(m_transfersModel, m_transferIndex, key, QString());
                it->setText(QString::fromUtf8("�� %1  ʧ�ܣ�%2 (HTTP %3)")
                                .arg(baseName, msg)
                                .arg(code));
                // 3 ����Ƴ�ʧ����
                QTimer::singleShot(3000, this, [this, key]() {
                    removeTransferItem(m_transfersModel, m_transferIndex, key);
                });
                QMessageBox::critical(this, u8"�ϴ�ʧ��",
                                      QString(u8"����%1��HTTP %2��").arg(msg).arg(code));
            });

    // ��ʼ�ϴ�
    m_uploader->start(m_httpHost, m_httpPort, filePath, m_username);
}

void ChatRoom::handleChatMessage(const QJsonObject& obj)
{
    const QString from = obj.value("from").toString();
    const QString text = obj.value("text").toString();

    // �������˻�����Լ���Ϣ���ɺ���
    if (!from.isEmpty() && from == m_username) return;

    const QString ts = QDateTime::currentDateTime().toString("MM-dd HH:mm:ss");
    ui->messageBrowser->append(
        QString(u8"<span style='color:#007bff;'>[%1] %2��%3</span>")
            .arg(ts)
            .arg(from.isEmpty() ? QStringLiteral("�Է�") : from.toHtmlEscaped())
            .arg(text.toHtmlEscaped())
    );
}

/* ===================== ������Ϣ ===================== */
void ChatRoom::requestOnlineInfo()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) return;

    QJsonObject req;
    req["action"] = "online_list";  // �ͷ����Լ��
    m_socket->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    m_socket->flush();
}

void ChatRoom::handleOnlineInfo(const QJsonObject& obj)
{
    // Լ��ʾ����
    // { "action":"online_info", "count":3, "users":["����","����","����"] }
    int count = obj.value("count").toInt();

    QStringList users;
    QJsonArray arr = obj.value("users").toArray();
    users.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        users << v.toString();
    }

    // �������ޱ仯�Ͳ�ˢ�� UI
    if (count == m_lastCount && users == m_lastUsers) {
        return;
    }
    m_lastCount = count;
    m_lastUsers = users;
    ui->userNumLb->setText(QString::number(count));

    m_usersModel->setStringList(users);

    // Ҳ������Ϣ������ʾһ�£���ѡ��
    // ui->messageBrowser->append(QString("<i>����������%1</i>").arg(count));
}

void ChatRoom::on_filesList_doubleClicked(const QModelIndex &index) {
    if (!index.isValid()) return;
    const QString name = m_filesModel->data(index, Qt::DisplayRole).toString();
    const QString savePath = QFileDialog::getSaveFileName(this, u8"���Ϊ", name);
    if (savePath.isEmpty()) return;

    startDownload_(name, savePath);
}

/* ===================== ����/����/�˳� ===================== */

void ChatRoom::handleSocketDisconnected()
{
    if (m_onlineTimer) m_onlineTimer->stop();
    QMessageBox::warning(this, u8"���ӶϿ�", u8"��������������ѶϿ�");
    emit back(); // �������ص�¼������ź�
}

void ChatRoom::handleSocketError(QAbstractSocket::SocketError)
{
    QMessageBox::critical(this, u8"�������",QString(u8"�����������: %1").arg(m_socket->errorString()));
}

void ChatRoom::on_exitRoomBt_clicked()
{
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    qApp->quit(); // ֱ���˳�����
}

bool ChatRoom::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->sendEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent*>(event);
        const bool isEnter = (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter);

        if (isEnter) {
            if (ke->modifiers() == Qt::NoModifier) {
                // ���س� -> ����
                on_sendBt_clicked();
                return true; // �Ѵ�����ֹ QTextEdit �Լ�����
            }
            if (ke->modifiers() & Qt::ShiftModifier) {
                // Shift+Enter -> ���У�����Ĭ����Ϊ��
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
    // const QString from = obj.value("from").toString();   // ��� UI ��չʾ�Ͳ���
    // const qint64  size = obj.value("size").toVariant().toLongLong();
    // const QString urlPath = obj.value("url").toString();
    if (name.isEmpty()) return;

    // �����Ҳࡰ�ļ��б�����˫�����أ�
    QStringList list = m_filesModel->stringList();
    if (!list.contains(name)) {
        list << name;
        m_filesModel->setStringList(list);
    }

    // ������ messageBrowser д�κ���ʾ������ˢ��
}



void ChatRoom::startDownload_(const QString& name, const QString& savePath) {
    // URL ����
    QUrl url;
    url.setScheme("http");
    url.setHost(m_httpHost);
    url.setPort(m_httpPort);
    url.setPath("/download");
    QUrlQuery q; q.addQueryItem("name", name);
    url.setQuery(q);

    QNetworkRequest req(url);
    // Qt5 û�� TransferTimeoutAttribute�������� QTimer ���ף�5 ���ӣ�
    m_dlReply = m_http.get(req);

    // ���浽�ļ�
    QFile* out = new QFile(savePath, m_dlReply);
    if (!out->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, u8"����ʧ��", u8"�޷�д�룺" + savePath);
        m_dlReply->abort();
        return;
    }

    // �󶨳�ʱ���������¼��
    attachTimeoutToReplyDL(m_dlReply, 300000);

    connect(m_dlReply, &QIODevice::readyRead, this, [this, out]() {
        out->write(m_dlReply->readAll());
    });

    connect(m_dlReply, &QNetworkReply::finished, this, [this, out, savePath]() {
        out->write(m_dlReply->readAll());
        out->close();

        const bool isTimeout = m_dlReply->property("timedOut").toBool();

        if (m_dlReply->error() == QNetworkReply::NoError) {
            QMessageBox::information(this, u8"�������", u8"�ѱ��浽�� " + savePath);
        } else {
            QFile::remove(savePath);
            const QString msg = isTimeout ? QStringLiteral("���س�ʱ") : m_dlReply->errorString();
            QMessageBox::critical(this, u8"����ʧ��", msg);
        }
        m_dlReply->deleteLater();
    });
}

