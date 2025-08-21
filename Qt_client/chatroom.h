#ifndef CHATROOM_H
#define CHATROOM_H

#include <QWidget>
#include <QMouseEvent>
#include <QTcpSocket>
#include <QStringListModel>
#include <QMap>
#include <QTimer>
#include <QKeyEvent>
#include <QUuid>
#include <QElapsedTimer>
#include <QSharedPointer>
#include <QVector>
#include <QBitArray>
#include <QScopedValueRollback>  // 在 QtCore 里
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include "fileuploader.h"
#include <QPointer>
#include <QStandardItemModel>
#include <QHash>

namespace Ui {
class ChatRoom;
}

class ChatRoom : public QWidget
{
    Q_OBJECT

public:
    explicit ChatRoom(QTcpSocket *socket = nullptr, const QString &username = "", QWidget *parent = nullptr);
    ~ChatRoom();

signals:
    void back();//定义一个信号，返回登录界面

protected:
    void mouseMoveEvent(QMouseEvent *event);
    void mousePressEvent(QMouseEvent *event);
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    // 网络事件
    void handleSocketDisconnected();
    void handleSocketError(QAbstractSocket::SocketError error);
    void handleSocketReadyRead();

    // UI事件
    void on_exitRoomBt_clicked();
    void on_sendBt_clicked();
    void on_sendFileBt_clicked();
    void on_filesList_doubleClicked(const QModelIndex &index);

private:
    // 分发器
    void dispatchMessage(const QJsonObject& obj);

    // 消息接收
    void handleChatMessage(const QJsonObject& obj);

    // 在线信息
    void requestOnlineInfo();   // 主动向服务器请求在线人数/列表
    void handleOnlineInfo(const QJsonObject& obj); // 处理服务器推送/应答的在线信息

    // 新增：处理服务端广播的文件元信息
    void handleFileMeta(const QJsonObject& obj);

    // 新增：辅助
    QString detectHttpHost_() const; // 从 socket 推断 HTTP host（与 TCP 同机）
    void startDownload_(const QString& name, const QString& savePath);

private:
    Ui::ChatRoom *ui;
    QPoint mPos;

    QTcpSocket *m_socket;
    QString m_username;

    QByteArray m_recvBuf;  // 接收缓冲：按行解析
    QStringListModel *m_usersModel = nullptr;
    QStringListModel *m_filesModel = nullptr;       // filesList 的模型

    QTimer* m_onlineTimer = nullptr;// 定时获取在线列表
    QStringList m_lastUsers;    // 防抖
    int m_lastCount = -1;

    // 新增：HTTP
    QNetworkAccessManager m_http;
    QPointer<QNetworkReply> m_dlReply;
    QString m_httpHost;
    int     m_httpPort = 9080;

    // 新增：上传器（每次选择文件就建一个；也可做成列表）
    FileUploader* m_uploader = nullptr;
    QStandardItemModel *m_transfersModel = nullptr; // filessendList 的模型
    QHash<QString, QStandardItem*> m_transferIndex; // filePath -> item
};

#endif // CHATROOM_H
