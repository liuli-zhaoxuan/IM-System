#include "widget.h"
#include "ui_widget.h"
#include "chatroom.h"
#include <QFormLayout>
#include <QDialogButtonBox>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    // 设置窗口无边框 固定窗口大小
    setWindowFlag(Qt::FramelessWindowHint);
    setFixedSize(width(), height());

    // 载入配置
    loadLoginConfig();

    connect(ui->exitBt, &QPushButton::clicked, this, &QWidget::close);
}

Widget::~Widget()
{
    delete ui;
}


/* 窗口移动 */
void Widget::mousePressEvent(QMouseEvent *event)
{
    mPos = event->globalPos() - this->pos();
}
void Widget::mouseMoveEvent(QMouseEvent *event)
{
    this->move(event->globalPos() - mPos);
}

/* 保存登录配置 */
void Widget::saveLoginConfig(const QString &ip, const QString &port, const QString &username, const QString &passwd)
{
    loginSettings->beginGroup("LoginConfig");
    loginSettings->setValue("IP", ip);
    loginSettings->setValue("Port", port);
    loginSettings->setValue("Username", username);
    loginSettings->setValue("Passwd", passwd);
    loginSettings->endGroup();
}

/* 载入登录配置 */
void Widget::loadLoginConfig()
{
    // 导入上一次的配置文件
    QString configFile = QCoreApplication::applicationDirPath() + "/config.ini";
    qDebug() << "Configuration file:" << configFile;

    loginSettings = new QSettings(configFile, QSettings::IniFormat, this);

    // 检查是否是首次运行（配置文件是否为空）
    if (loginSettings->allKeys().isEmpty()) {
        // 初始化默认配置
        loginSettings->beginGroup("LoginConfig");
        loginSettings->setValue("IP", "");
        loginSettings->setValue("Port", "");
        loginSettings->setValue("Username", "");
        loginSettings->setValue("Passwd", "");
        loginSettings->endGroup();
        loginSettings->sync(); // 立即写入文件
    }
    else {
        // 将之前的配置显示到登录界面
        loginSettings->beginGroup("LoginConfig");
        ui->ipLine->setText(loginSettings->value("IP").toString());
        ui->portLine->setText(loginSettings->value("Port").toString());
        ui->userNameLine->setText(loginSettings->value("Username").toString());
        ui->userPasswdLine->setText(loginSettings->value("Passwd").toString());
        loginSettings->endGroup();
        //qDebug() << "IP:" << ui->ipLine->text() << "Port:" << ui->portLine->text() << "Username:" << ui->userNameLine->text()  << "Passwd:" << ui->userPasswdLine->text();
    }

}

// 注册功能
void Widget::on_registerBt_clicked()
{
    // 创建输入对话框
    QDialog dialog(this);
    QFormLayout form(&dialog);

    // 添加输入框
    QLineEdit ipInput(u8"127.0.0.1", &dialog);
    QLineEdit portInput(u8"9000", &dialog);
    QLineEdit usernameInput(&dialog);
    QLineEdit passwordInput(&dialog);
    passwordInput.setEchoMode(QLineEdit::Password);

    form.addRow(u8"服务器IP:", &ipInput);
    form.addRow(u8"端口:", &portInput);
    form.addRow(u8"用户名:", &usernameInput);
    form.addRow(u8"密码:", &passwordInput);

    // 添加按钮
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);

    QObject::connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // 显示对话框
    if (dialog.exec() == QDialog::Accepted) {
        bool ok;
        quint16 port = portInput.text().toUShort(&ok);

        if (!ok || port == 0) {
            QMessageBox::warning(this, u8"错误", u8"端口号必须是0-65535之间的数字");
            return;
        }

        if (usernameInput.text().isEmpty() || passwordInput.text().isEmpty()) {
            QMessageBox::warning(this, u8"错误", u8"用户名和密码不能为空");
            return;
        }

        sendRegisterRequest(ipInput.text(), port,
                          usernameInput.text(), passwordInput.text());
    }
}

void Widget::sendRegisterRequest(const QString &ip, quint16 port, const QString &username, const QString &password)
{
    QTcpSocket *socket = new QTcpSocket(this);

    // === 1) 连接阶段：所有状态/错误都打日志 ===
    qDebug() << "[register] dial" << ip << port
             << "user=" << username;

    connect(socket, &QTcpSocket::stateChanged, this, [=](QAbstractSocket::SocketState s){
        qDebug() << "[register][state]" << s;
    });
    connect(socket, &QTcpSocket::connected, this, [=](){
        qDebug() << "[register] connected";
    });
    connect(socket, &QTcpSocket::bytesWritten, this, [=](qint64 n){
        qDebug() << "[register][bytesWritten]" << n;
    });

    socket->connectToHost(ip, port);
    if (!socket->waitForConnected(3000)) {
        QMessageBox::critical(this, u8"错误",
                              u8"连接服务器失败: " + socket->errorString());
        qDebug() << "[register] waitForConnected FAILED";
        socket->deleteLater();
        return;
    }

    // === 2) 预先绑定 readyRead（避免竞态） ===
    QMetaObject::Connection rrConn;
    rrConn = connect(socket, &QTcpSocket::readyRead, this, [=]() mutable {
        qint64 avail = socket->bytesAvailable();
        qDebug() << "[register][readyRead] bytesAvailable=" << avail;
        QByteArray response = socket->readAll();
        qDebug() << "[register][raw size]" << response.size();
        qDebug().noquote() << "[register][raw head]"
                           << QString::fromUtf8(response.left(200));

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(response, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            qDebug() << "[register][json error]" << error.errorString();
            // 不立刻提示，让同步兜底再试一轮
            return;
        }

        QJsonObject obj = doc.object();
        qDebug() << "[register][json]" << obj;

        if (obj["status"].toString() == u8"success") {
            QMessageBox::information(this, u8"成功", u8"注册成功！");
        } else {
            QMessageBox::warning(this, u8"注册失败", obj["reason"].toString());
        }
        QObject::disconnect(rrConn);
        socket->disconnectFromHost();
    });

    // === 3) 发送注册 JSON（结尾必须有 '\n'） ===
    QJsonObject registerMsg{
        {"action","register"},
        {"username",username},
        {"password",password}
    };
    const QByteArray payload =
        QJsonDocument(registerMsg).toJson(QJsonDocument::Compact) + "\n";
    qDebug().noquote() << "[register][tx]" << QString::fromUtf8(payload);
    qint64 n = socket->write(payload);
    socket->flush();
    socket->waitForBytesWritten(1000);
    qDebug() << "[register] write ret=" << n;

    // === 4) 5秒超时兜底：同步按行读取一次 ===
    QTimer *timer = new QTimer(socket);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [=]() mutable {
        qDebug() << "[register] timeout -> fallback to sync read";
        bool handled = false;

        if (socket->waitForReadyRead(2000)) {
            while (socket->bytesAvailable() > 0) {
                QByteArray line = socket->readLine().trimmed();
                qDebug().noquote() << "[register][line]" << QString::fromUtf8(line);
                if (line.isEmpty()) continue;

                QJsonParseError err{};
                QJsonDocument j = QJsonDocument::fromJson(line, &err);
                if (err.error != QJsonParseError::NoError || !j.isObject()) continue;
                QJsonObject obj = j.object();
                if (!obj.contains("status")) continue;

                handled = true;
                if (obj["status"].toString() == u8"success") {
                    QMessageBox::information(this, u8"成功", u8"注册成功！");
                } else {
                    QMessageBox::warning(this, u8"注册失败", obj["reason"].toString());
                }
                break;
            }
        }
        if (!handled) {
            QMessageBox::warning(this, u8"错误",
                                 u8"等待注册结果超时或数据无效");
        }
        socket->disconnectFromHost();
    });
    timer->start(5000);

    // === 5) 收尾 ===
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

// 登录功能
void Widget::on_loginBt_clicked()
{
    // 1. 获取界面输入
    QString ip = ui->ipLine->text().trimmed();
    QString portStr = ui->portLine->text().trimmed();
    QString username = ui->userNameLine->text().trimmed();
    QString password = ui->userPasswdLine->text();
    bool remember = ui->remenberCb->isChecked();

    qDebug() << "IP:" << ip << "Port:" << portStr << "Username:" << username  << "Passwd:" << password;

    // 2. 验证输入
    if (ip.isEmpty() || portStr.isEmpty() || username.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, u8"错误", u8"请填写完整登录信息");
        return;
    }

    bool ok;
    quint16 port = portStr.toUShort(&ok);
    if (!ok || port == 0) {
        QMessageBox::warning(this, u8"错误", u8"端口号必须是0-65535之间的数字");
        return;
    }

    // 3. 发送登录请求
    sendLoginRequest(ip, port, username, password, remember);
}

// 发送登录请求函数
void Widget::sendLoginRequest(const QString &ip, quint16 port, const QString &username, const QString &password, bool remember)
{
    // 创建临时socket
    QTcpSocket *socket = new QTcpSocket(this);
    socket->connectToHost(ip, port);

    if (!socket->waitForConnected(3000)) {
        QMessageBox::critical(this, u8"错误", u8"连接服务器失败: " + socket->errorString());
        socket->deleteLater();
        return;
    }

    if (remember) {
        saveLoginConfig(ip, QString::number(port), username, password);
    }

    // 构造登录请求 JSON
    QJsonObject loginMsg;
    loginMsg["action"] = "login";
    loginMsg["username"] = username;
    loginMsg["password"] = password;

    QJsonDocument doc(loginMsg);
    socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
    socket->flush();

    // 处理响应
    // 只处理一次“登录响应”
    QMetaObject::Connection loginConn;
    loginConn = connect(socket, &QTcpSocket::readyRead, [=]() mutable {

        // 只按行读取，避免一次 readAll 读进多条
        while (socket->canReadLine())
        {
            QByteArray one = socket->readLine();
            if (one.endsWith('\n')) one.chop(1);
            if (one.endsWith('\r')) one.chop(1);
            if (one.trimmed().isEmpty()) continue;

            QJsonParseError error{};
            QJsonDocument doc = QJsonDocument::fromJson(one, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                // 不是有效 JSON……继续等下一行
                continue;
            }
            QJsonObject obj = doc.object();
            // 只响应“登录结果”这一类消息
            if (obj.contains("status"))
            {
                // 解绑登录阶段的 readyRead
                QObject::disconnect(loginConn);

                if (obj["status"].toString() == u8"success") {
                    // 交接给 ChatRoom（后续消息都由 ChatRoom 处理）
                    QString uname = obj["username"].toString(username);
                    socket->disconnect(this);
                    if (!pcharroom) {
                        pcharroom = new ChatRoom(socket, uname, nullptr);
                        socket->setParent(pcharroom);               // ChatRoom 管理生命周期
                        connect(pcharroom, &ChatRoom::back, this, &Widget::show);
                    }
                    this->hide();
                    pcharroom->show();
                } else {
                    // 登录失败才断开
                    socket->disconnectFromHost();
                }
                // 处理完登录结果就 return，不再在这个 lambda 里处理其它消息
                return;
            }
        }
    });

    // 错误处理
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                [this](QAbstractSocket::SocketError error) {
            QMessageBox::critical(this, u8"错误", u8"网络错误: " + QString::number(error));
        });
}

