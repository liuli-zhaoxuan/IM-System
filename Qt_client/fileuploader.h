#pragma once
#include <QObject>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <QPointer>

class FileUploader : public QObject {
    Q_OBJECT
public:
    explicit FileUploader(QObject* parent = nullptr);

    // host: �� "127.0.0.1", port: 9080
    // filePath: �����ļ�·��, from: �����û��������� /upload/complete��
    void start(const QString& host, int port,
               const QString& filePath, const QString& from);

    void cancel(); // ȡ����ǰ�ϴ������� abort��

signals:
    void progress(qint64 sentBytes, qint64 totalBytes, double speedBps, int percent, int seq);
    void finished(QString downloadUrl);  // ���� "/download?name=xxx"
    void error(QString message, int httpCode, int seq);

private slots:
    void onInitFinished();
    void onChunkFinished();
    void onCompleteFinished();

private:
    void postInit_();
    void putNextChunk_();
    void postComplete_();
    void fail_(const QString& msg, int httpCode = 0);

private:
    QNetworkAccessManager nam_;
    QPointer<QNetworkReply> reply_;

    QFile file_;
    QString filePath_;
    QString baseName_;
    QString from_;
    QString host_;
    int     port_ = 9080;

    QString id_;
    int     chunkSize_ = 256 * 1024; // ����˻��·�����
    qint64  size_ = 0;
    qint64  offset_ = 0;
    int     seq_ = 0;

    int     retries_ = 0;
    const   int kMaxRetries_ = 3;

    bool    cancel_ = false;

    QElapsedTimer speedClock_;
    qint64  lastOffset_ = 0;

    // ��ʱ��Qt5 ȫ���ݣ�
    int  timeoutMs_ = 30000;   // ÿ������ 30s
    bool timedOut_  = false;
};
