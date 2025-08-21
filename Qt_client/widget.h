#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include "chatroom.h"
#include <QMouseEvent>
#include <QPushButton>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QDebug>
#include <QMessageBox>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    void saveLoginConfig(const QString &ip, const QString &port, const QString &username, const QString &passwd);
    void loadLoginConfig();


    ChatRoom *pcharroom = nullptr;  //保存聊天页面的实例化对象地址

private slots:
    void on_registerBt_clicked();
    void sendRegisterRequest(const QString &ip, quint16 port, const QString &username, const QString &password);

    void on_loginBt_clicked();
    void sendLoginRequest(const QString &ip, quint16 port, const QString &username, const QString &password, bool remember);

protected:
    void mouseMoveEvent(QMouseEvent *event);
    void mousePressEvent(QMouseEvent *event);

private:
    Ui::Widget *ui;
    QPoint mPos;
    QSettings *loginSettings;

};
#endif // WIDGET_H
