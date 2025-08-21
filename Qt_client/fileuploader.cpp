#include "fileuploader.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QScopedPointer>
#include <QTimer>
#include <QFileInfo>

#ifndef HAS_QNETWORKREQUEST_TIMEOUT
  #if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    #define HAS_QNETWORKREQUEST_TIMEOUT 1
  #else
    #define HAS_QNETWORKREQUEST_TIMEOUT 0
  #endif
#endif

namespace {

// 给任意 reply 附加超时（Qt 5.15 以下用 QTimer 兜底；5.15+ 用官方属性即可）
QTimer* attachTimeoutToReply(QNetworkReply* reply, int timeoutMs, bool* timedOutFlag)
{
    if (!reply || timeoutMs <= 0) return nullptr;

#if HAS_QNETWORKREQUEST_TIMEOUT
    Q_UNUSED(timedOutFlag);
    return nullptr; // 5.15+ 用 request 属性，不需要定时器
#else
    QTimer* timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply, timedOutFlag]() {
        if (timedOutFlag) *timedOutFlag = true;
        if (reply && !reply->isFinished()) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, timer, &QObject::deleteLater);
    timer->start(timeoutMs);
    return timer;
#endif
}

static inline QNetworkRequest makeJsonRequest(const QUrl& url, int timeoutMs) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
#if HAS_QNETWORKREQUEST_TIMEOUT
    req.setAttribute(QNetworkRequest::TransferTimeoutAttribute, timeoutMs);
#endif
    return req;
}

static inline QNetworkRequest makeBinaryRequest(const QUrl& url, qint64 contentLen, int timeoutMs) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setHeader(QNetworkRequest::ContentLengthHeader, contentLen);
#if HAS_QNETWORKREQUEST_TIMEOUT
    req.setAttribute(QNetworkRequest::TransferTimeoutAttribute, timeoutMs);
#endif
    return req;
}

} // namespace

FileUploader::FileUploader(QObject* parent) : QObject(parent) {}

void FileUploader::start(const QString& host, int port,
                         const QString& filePath, const QString& from) {
    cancel_ = false;
    host_ = host;
    port_ = port;
    filePath_ = filePath;
    from_ = from;

    file_.setFileName(filePath_);
    if (!file_.open(QIODevice::ReadOnly)) {
        fail_(QStringLiteral("无法打开文件: %1").arg(filePath_));
        return;
    }
    size_ = file_.size();
    baseName_ = QFileInfo(file_).fileName();
    offset_ = 0;
    seq_ = 0;
    retries_ = 0;
    lastOffset_ = 0;
    speedClock_.restart();

    postInit_();
}

void FileUploader::cancel() {
    cancel_ = true;
    if (reply_) reply_->abort();
}

void FileUploader::postInit_() {
    timedOut_ = false;

    QUrl url;
    url.setScheme("http");
    url.setHost(host_);
    url.setPort(port_);
    url.setPath("/upload/init");

    QJsonObject j{{"name", baseName_}, {"size", size_}};
    const QByteArray body = QJsonDocument(j).toJson(QJsonDocument::Compact);

    reply_ = nam_.post(makeJsonRequest(url, timeoutMs_), body);
    attachTimeoutToReply(reply_, timeoutMs_, &timedOut_);
    connect(reply_, &QNetworkReply::finished, this, &FileUploader::onInitFinished);
}

void FileUploader::onInitFinished() {
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply_);
    if (cancel_) { fail_(QStringLiteral("已取消")); return; }
    if (timedOut_) { timedOut_ = false; fail_(QStringLiteral("请求超时"), 408); return; }

    if (guard->error() != QNetworkReply::NoError) {
        const int http = guard->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        fail_(guard->errorString(), http);
        return;
    }

    const QByteArray resp = guard->readAll();
    QJsonParseError pe{};
    QJsonDocument doc = QJsonDocument::fromJson(resp, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        fail_(QStringLiteral("init 响应非 JSON"));
        return;
    }
    const auto obj = doc.object();
    id_ = obj.value("id").toString();
    chunkSize_ = obj.value("chunk_size").toInt(256 * 1024);
    if (id_.isEmpty() || chunkSize_ <= 0) {
        fail_(QStringLiteral("init 响应缺少 id/chunk_size"));
        return;
    }
    putNextChunk_();
}

void FileUploader::putNextChunk_() {
    if (cancel_) { fail_(QStringLiteral("已取消")); return; }
    if (offset_ >= size_) {
        postComplete_();
        return;
    }

    const qint64 count = qMin<qint64>(chunkSize_, size_ - offset_);
    if (!file_.seek(offset_)) {
        fail_(QStringLiteral("文件定位失败 offset=%1").arg(offset_));
        return;
    }
    QByteArray block = file_.read(count);
    if (block.size() != count) {
        fail_(QStringLiteral("读取分片失败 期望=%1 实得=%2").arg(count).arg(block.size()));
        return;
    }

    QUrl url;
    url.setScheme("http");
    url.setHost(host_);
    url.setPort(port_);
    url.setPath("/upload/chunk");
    QUrlQuery q; q.addQueryItem("id", id_); q.addQueryItem("seq", QString::number(seq_));
    url.setQuery(q);

    timedOut_ = false;
    reply_ = nam_.put(makeBinaryRequest(url, block.size(), timeoutMs_), block);
    attachTimeoutToReply(reply_, timeoutMs_, &timedOut_);
    connect(reply_, &QNetworkReply::finished, this, &FileUploader::onChunkFinished);
}

void FileUploader::onChunkFinished() {
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply_);
    if (cancel_) { fail_(QStringLiteral("已取消")); return; }
    if (timedOut_) { timedOut_ = false; fail_(QStringLiteral("请求超时"), 408); return; }

    const int http = guard->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (guard->error() != QNetworkReply::NoError) {
        if (retries_ < kMaxRetries_) {
            retries_++;
            putNextChunk_(); // 重发当前分片
            return;
        }
        fail_(guard->errorString(), http);
        return;
    }

    const qint64 count = qMin<qint64>(chunkSize_, size_ - offset_);
    offset_ += count;
    seq_ += 1;
    retries_ = 0;

    double speed = 0.0;
    if (speedClock_.elapsed() > 0) {
        const qint64 delta = offset_ - lastOffset_;
        speed = (double)delta / (double)speedClock_.elapsed() * 1000.0; // B/s
        speedClock_.restart();
        lastOffset_ = offset_;
    }
    const int pct = size_ > 0 ? int((offset_ * 100) / size_) : 0;
    emit progress(offset_, size_, speed, pct, seq_ - 1);

    putNextChunk_();
}

void FileUploader::postComplete_() {
    timedOut_ = false;

    QUrl url;
    url.setScheme("http");
    url.setHost(host_);
    url.setPort(port_);
    url.setPath("/upload/complete");

    QJsonObject j{
        {"id", id_},
        {"name", baseName_},
        {"size", size_},
        {"from", from_}
    };
    const QByteArray body = QJsonDocument(j).toJson(QJsonDocument::Compact);

    reply_ = nam_.post(makeJsonRequest(url, timeoutMs_), body);
    attachTimeoutToReply(reply_, timeoutMs_, &timedOut_);
    connect(reply_, &QNetworkReply::finished, this, &FileUploader::onCompleteFinished);
}

void FileUploader::onCompleteFinished() {
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply_);
    if (cancel_) { fail_(QStringLiteral("已取消")); return; }
    if (timedOut_) { timedOut_ = false; fail_(QStringLiteral("请求超时"), 408); return; }

    const int http = guard->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (guard->error() != QNetworkReply::NoError) {
        fail_(guard->errorString(), http);
        return;
    }

    // 服务端会通过聊天广播 file_meta；这里本地返回一个下载路径给 UI 用
    const QString urlPath = QString("/download?name=%1").arg(baseName_);
    emit finished(urlPath);
}

void FileUploader::fail_(const QString& msg, int httpCode) {
    emit error(msg, httpCode, seq_);
}
