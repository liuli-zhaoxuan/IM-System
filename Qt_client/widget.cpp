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

    // ���ô����ޱ߿� �̶����ڴ�С
    setWindowFlag(Qt::FramelessWindowHint);
    setFixedSize(width(), height());

    // ��������
    loadLoginConfig();

    connect(ui->exitBt, &QPushButton::clicked, this, &QWidget::close);
}

Widget::~Widget()
{
    delete ui;
}


/* �����ƶ� */
void Widget::mousePressEvent(QMouseEvent *event)
{
    mPos = event->globalPos() - this->pos();
}
void Widget::mouseMoveEvent(QMouseEvent *event)
{
    this->move(event->globalPos() - mPos);
}

/* �����¼���� */
void Widget::saveLoginConfig(const QString &ip, const QString &port, const QString &username, const QString &passwd)
{
    loginSettings->beginGroup("LoginConfig");
    loginSettings->setValue("IP", ip);
    loginSettings->setValue("Port", port);
    loginSettings->setValue("Username", username);
    loginSettings->setValue("Passwd", passwd);
    loginSettings->endGroup();
}

/* �����¼���� */
void Widget::loadLoginConfig()
{
    // ������һ�ε������ļ�
    QString configFile = QCoreApplication::applicationDirPath() + "/config.ini";
    qDebug() << "Configuration file:" << configFile;

    loginSettings = new QSettings(configFile, QSettings::IniFormat, this);

    // ����Ƿ����״����У������ļ��Ƿ�Ϊ�գ�
    if (loginSettings->allKeys().isEmpty()) {
        // ��ʼ��Ĭ������
        loginSettings->beginGroup("LoginConfig");
        loginSettings->setValue("IP", "");
        loginSettings->setValue("Port", "");
        loginSettings->setValue("Username", "");
        loginSettings->setValue("Passwd", "");
        loginSettings->endGroup();
        loginSettings->sync(); // ����д���ļ�
    }
    else {
        // ��֮ǰ��������ʾ����¼����
        loginSettings->beginGroup("LoginConfig");
        ui->ipLine->setText(loginSettings->value("IP").toString());
        ui->portLine->setText(loginSettings->value("Port").toString());
        ui->userNameLine->setText(loginSettings->value("Username").toString());
        ui->userPasswdLine->setText(loginSettings->value("Passwd").toString());
        loginSettings->endGroup();
        //qDebug() << "IP:" << ui->ipLine->text() << "Port:" << ui->portLine->text() << "Username:" << ui->userNameLine->text()  << "Passwd:" << ui->userPasswdLine->text();
    }

}

// ע�Ṧ��
void Widget::on_registerBt_clicked()
{
    // ��������Ի���
    QDialog dialog(this);
    QFormLayout form(&dialog);

    // ��������
    QLineEdit ipInput(u8"127.0.0.1", &dialog);
    QLineEdit portInput(u8"9000", &dialog);
    QLineEdit usernameInput(&dialog);
    QLineEdit passwordInput(&dialog);
    passwordInput.setEchoMode(QLineEdit::Password);

    form.addRow(u8"������IP:", &ipInput);
    form.addRow(u8"�˿�:", &portInput);
    form.addRow(u8"�û���:", &usernameInput);
    form.addRow(u8"����:", &passwordInput);

    // ��Ӱ�ť
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);

    QObject::connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // ��ʾ�Ի���
    if (dialog.exec() == QDialog::Accepted) {
        bool ok;
        quint16 port = portInput.text().toUShort(&ok);

        if (!ok || port == 0) {
            QMessageBox::warning(this, u8"����", u8"�˿ںű�����0-65535֮�������");
            return;
        }

        if (usernameInput.text().isEmpty() || passwordInput.text().isEmpty()) {
            QMessageBox::warning(this, u8"����", u8"�û��������벻��Ϊ��");
            return;
        }

        sendRegisterRequest(ipInput.text(), port,
                          usernameInput.text(), passwordInput.text());
    }
}

void Widget::sendRegisterRequest(const QString &ip, quint16 port, const QString &username, const QString &password)
{
    QTcpSocket *socket = new QTcpSocket(this);

    // === 1) ���ӽ׶Σ�����״̬/���󶼴���־ ===
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
        QMessageBox::critical(this, u8"����",
                              u8"���ӷ�����ʧ��: " + socket->errorString());
        qDebug() << "[register] waitForConnected FAILED";
        socket->deleteLater();
        return;
    }

    // === 2) Ԥ�Ȱ� readyRead�����⾺̬�� ===
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
            // ��������ʾ����ͬ����������һ��
            return;
        }

        QJsonObject obj = doc.object();
        qDebug() << "[register][json]" << obj;

        if (obj["status"].toString() == u8"success") {
            QMessageBox::information(this, u8"�ɹ�", u8"ע��ɹ���");
        } else {
            QMessageBox::warning(this, u8"ע��ʧ��", obj["reason"].toString());
        }
        QObject::disconnect(rrConn);
        socket->disconnectFromHost();
    });

    // === 3) ����ע�� JSON����β������ '\n'�� ===
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

    // === 4) 5�볬ʱ���ף�ͬ�����ж�ȡһ�� ===
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
                    QMessageBox::information(this, u8"�ɹ�", u8"ע��ɹ���");
                } else {
                    QMessageBox::warning(this, u8"ע��ʧ��", obj["reason"].toString());
                }
                break;
            }
        }
        if (!handled) {
            QMessageBox::warning(this, u8"����",
                                 u8"�ȴ�ע������ʱ��������Ч");
        }
        socket->disconnectFromHost();
    });
    timer->start(5000);

    // === 5) ��β ===
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

// ��¼����
void Widget::on_loginBt_clicked()
{
    // 1. ��ȡ��������
    QString ip = ui->ipLine->text().trimmed();
    QString portStr = ui->portLine->text().trimmed();
    QString username = ui->userNameLine->text().trimmed();
    QString password = ui->userPasswdLine->text();
    bool remember = ui->remenberCb->isChecked();

    qDebug() << "IP:" << ip << "Port:" << portStr << "Username:" << username  << "Passwd:" << password;

    // 2. ��֤����
    if (ip.isEmpty() || portStr.isEmpty() || username.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, u8"����", u8"����д������¼��Ϣ");
        return;
    }

    bool ok;
    quint16 port = portStr.toUShort(&ok);
    if (!ok || port == 0) {
        QMessageBox::warning(this, u8"����", u8"�˿ںű�����0-65535֮�������");
        return;
    }

    // 3. ���͵�¼����
    sendLoginRequest(ip, port, username, password, remember);
}

// ���͵�¼������
void Widget::sendLoginRequest(const QString &ip, quint16 port, const QString &username, const QString &password, bool remember)
{
    // ������ʱsocket
    QTcpSocket *socket = new QTcpSocket(this);
    socket->connectToHost(ip, port);

    if (!socket->waitForConnected(3000)) {
        QMessageBox::critical(this, u8"����", u8"���ӷ�����ʧ��: " + socket->errorString());
        socket->deleteLater();
        return;
    }

    if (remember) {
        saveLoginConfig(ip, QString::number(port), username, password);
    }

    // �����¼���� JSON
    QJsonObject loginMsg;
    loginMsg["action"] = "login";
    loginMsg["username"] = username;
    loginMsg["password"] = password;

    QJsonDocument doc(loginMsg);
    socket->write(doc.toJson(QJsonDocument::Compact) + "\n");
    socket->flush();

    // ������Ӧ
    // ֻ����һ�Ρ���¼��Ӧ��
    QMetaObject::Connection loginConn;
    loginConn = connect(socket, &QTcpSocket::readyRead, [=]() mutable {

        // ֻ���ж�ȡ������һ�� readAll ��������
        while (socket->canReadLine())
        {
            QByteArray one = socket->readLine();
            if (one.endsWith('\n')) one.chop(1);
            if (one.endsWith('\r')) one.chop(1);
            if (one.trimmed().isEmpty()) continue;

            QJsonParseError error{};
            QJsonDocument doc = QJsonDocument::fromJson(one, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                // ������Ч JSON������������һ��
                continue;
            }
            QJsonObject obj = doc.object();
            // ֻ��Ӧ����¼�������һ����Ϣ
            if (obj.contains("status"))
            {
                // ����¼�׶ε� readyRead
                QObject::disconnect(loginConn);

                if (obj["status"].toString() == u8"success") {
                    // ���Ӹ� ChatRoom��������Ϣ���� ChatRoom ����
                    QString uname = obj["username"].toString(username);
                    socket->disconnect(this);
                    if (!pcharroom) {
                        pcharroom = new ChatRoom(socket, uname, nullptr);
                        socket->setParent(pcharroom);               // ChatRoom ������������
                        connect(pcharroom, &ChatRoom::back, this, &Widget::show);
                    }
                    this->hide();
                    pcharroom->show();
                } else {
                    // ��¼ʧ�ܲŶϿ�
                    socket->disconnectFromHost();
                }
                // �������¼����� return����������� lambda �ﴦ��������Ϣ
                return;
            }
        }
    });

    // ������
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                [this](QAbstractSocket::SocketError error) {
            QMessageBox::critical(this, u8"����", u8"�������: " + QString::number(error));
        });
}

