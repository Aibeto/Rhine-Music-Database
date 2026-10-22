#include "HttpAudioStream.h"
#include <QDir>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QWaitCondition>
#include <algorithm>

namespace {
constexpr qint64 MAX_STREAM_BYTES = 1024LL * 1024 * 1024;
constexpr qint64 SEEK_AHEAD_BYTES = 256 * 1024;
constexpr int MAX_READ_RETRIES = 2;
constexpr int NETWORK_READ_TIMEOUT_MS = 3000;
constexpr qint64 NETWORK_BUFFER_BYTES = 256 * 1024;
constexpr qint64 CACHE_WRITE_BYTES = 64 * 1024;
}

struct HttpAudioStream::State {
    mutable QMutex mutex;
    QWaitCondition arrived;
    QMap<qint64, qint64> ranges;
    qint64 total = -1;
    qint64 received = 0;
    qint64 activeStart = 0;
    qint64 activeEnd = 0;
    qint64 wanted = -1;
    bool cancelled = false;
    bool broken = false;
    bool transferring = false;
    bool announced = false;
    int httpStatus = 0;
    int networkError = 0;
    int reconnects = 0;

    qint64 available(qint64 offset) const {
        auto entry = ranges.upperBound(offset);
        if (entry == ranges.begin()) {
            return 0;
        }
        --entry;
        return std::max<qint64>(0, entry.value() - offset);
    }

    void remember(qint64 start, qint64 end) {
        auto entry = ranges.lowerBound(start);
        if (entry != ranges.begin()) {
            const auto previous = std::prev(entry);
            if (previous.value() >= start) {
                entry = previous;
            }
        }
        while (entry != ranges.end() && entry.key() <= end) {
            start = std::min(start, entry.key());
            end = std::max(end, entry.value());
            entry = ranges.erase(entry);
        }
        ranges.insert(start, end);
    }
};

class HttpAudioStream::Worker : public QObject {
  public:
    Worker(HttpAudioStream *stream, QString path)
        : owner(stream), shared(*stream->state), writer(path, this) {
    }

    void begin() {
        if (!writer.open(QIODevice::ReadWrite | QIODevice::Unbuffered)) {
            fail();
            return;
        }
        network = new QNetworkAccessManager(this);
        request(0);
    }

    void reposition() {
        qint64 offset = -1;
        {
            QMutexLocker lock(&shared.mutex);
            offset = shared.wanted;
            shared.wanted = -1;
            if (shared.cancelled || shared.broken || offset < 0 || shared.available(offset) > 0) {
                return;
            }
            if (shared.transferring && offset >= shared.activeStart &&
                offset <= shared.activeEnd + SEEK_AHEAD_BYTES) {
                return;
            }
        }
        retries = 0;
        request(offset);
    }

  private:
    HttpAudioStream *owner;
    State &shared;
    QFile writer;
    QNetworkAccessManager *network = nullptr;
    QPointer<QNetworkReply> reply;
    qint64 requested = 0;
    qint64 writeOffset = 0;
    qint64 responseEnd = -1;
    bool headersAccepted = false;
    int retries = 0;
    quint64 generation = 0;
    QByteArray validator;

    void notify(bool first = false, bool failure = false) {
        QMetaObject::invokeMethod(
            owner,
            [guard = QPointer(owner), first, failure] {
                if (!guard) {
                    return;
                }
                {
                    QMutexLocker lock(&guard->state->mutex);
                    if (guard->state->cancelled) {
                        return;
                    }
                }
                if (guard->transportChanged) {
                    guard->transportChanged();
                }
                if (first && guard->ready) {
                    guard->ready();
                }
                if (failure && guard->failed) {
                    guard->failed();
                }
            },
            Qt::QueuedConnection);
    }

    void fail() {
        {
            QMutexLocker lock(&shared.mutex);
            shared.broken = true;
            shared.transferring = false;
            shared.arrived.wakeAll();
        }
        ++generation;
        if (reply) {
            QObject::disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
            reply = nullptr;
        }
        notify(false, true);
    }

    void request(qint64 offset) {
        ++generation;
        if (reply) {
            QObject::disconnect(reply, nullptr, this, nullptr);
            reply->abort();
            reply->deleteLater();
        }
        {
            QMutexLocker lock(&shared.mutex);
            if (shared.cancelled) {
                return;
            }
            shared.activeStart = offset;
            shared.activeEnd = offset;
            shared.transferring = true;
        }
        requested = offset;
        writeOffset = offset;
        responseEnd = -1;
        headersAccepted = false;
        QNetworkRequest request(owner->source);
        request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
        request.setRawHeader("Accept-Encoding", "identity");
        if (!validator.isEmpty() && offset > 0) {
            request.setRawHeader("If-Range", validator);
        }
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(NETWORK_READ_TIMEOUT_MS);
        reply = network->get(request);
        reply->setReadBufferSize(NETWORK_BUFFER_BYTES);
        const auto token = generation;
        QObject::connect(reply, &QNetworkReply::metaDataChanged, this, [this, token] {
            if (generation == token) {
                acceptHeaders();
            }
        });
        QObject::connect(reply, &QNetworkReply::readyRead, this, [this, token] {
            if (generation == token) {
                consume();
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, this, [this, token] {
            if (generation == token) {
                finish();
            }
        });
    }

    void acceptHeaders() {
        if (headersAccepted) {
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        {
            QMutexLocker lock(&shared.mutex);
            shared.httpStatus = status;
        }
        // 重定向由 Qt Network 处理
        if (status >= 300 && status < 400) {
            return;
        }
        if (status != 200 && status != 206) {
            if (status >= 400) {
                fail();
            }
            return;
        }
        qint64 total = -1;
        bool hasLength = false;
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&hasLength);
        const auto contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        const auto etag = reply->rawHeader("ETag");
        if (contentType.startsWith("text/") || contentType.contains("json") ||
            (!validator.isEmpty() && !etag.isEmpty() && etag != validator)) {
            fail();
            return;
        }
        if (status == 206) {
            const QRegularExpression expression(QStringLiteral("^bytes ([0-9]+)-([0-9]+)/([0-9]+)$"));
            const auto match = expression.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
            const auto start = match.captured(1).toLongLong();
            responseEnd = match.captured(2).toLongLong() + 1;
            total = match.captured(3).toLongLong();
            if (!match.hasMatch() || start != requested || responseEnd <= start || total < responseEnd ||
                (hasLength && length != responseEnd - start)) {
                fail();
                return;
            }
        } else {
            // 服务端忽略 Range 时从零写入缓存
            writeOffset = 0;
            total = hasLength ? length : -1;
            responseEnd = total;
        }
        bool invalid = false;
        {
            QMutexLocker lock(&shared.mutex);
            invalid = total > MAX_STREAM_BYTES || (total >= 0 && shared.total >= 0 && total != shared.total);
            if (!invalid) {
                if (total >= 0) {
                    shared.total = total;
                }
                shared.activeStart = writeOffset;
                shared.activeEnd = writeOffset;
            }
        }
        if (invalid) {
            fail();
            return;
        }
        if (validator.isEmpty() && !etag.startsWith("W/")) {
            validator = etag;
        }
        headersAccepted = true;
        notify();
    }

    void consume() {
        if (!headersAccepted) {
            return;
        }
        while (reply && reply->bytesAvailable() > 0) {
            const auto bytes = reply->read(CACHE_WRITE_BYTES);
            if (bytes.isEmpty()) {
                break;
            }
            const auto start = writeOffset;
            if (start + bytes.size() > MAX_STREAM_BYTES ||
                (responseEnd >= 0 && start + bytes.size() > responseEnd) || !writer.seek(start) ||
                writer.write(bytes) != bytes.size()) {
                fail();
                return;
            }
            writeOffset += bytes.size();
            bool first = false;
            {
                QMutexLocker lock(&shared.mutex);
                shared.remember(start, writeOffset);
                shared.received += bytes.size();
                shared.activeEnd = writeOffset;
                if (!shared.announced) {
                    shared.announced = true;
                    first = true;
                }
                shared.arrived.wakeAll();
            }
            if (first) {
                notify(true);
            }
        }
    }

    /** 完成传输 */
    void finish() {
        consume();
        if (!reply) {
            return;
        }
        const auto error = reply->error();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        reply = nullptr;
        qint64 total = -1;
        bool cancelled = false;
        {
            QMutexLocker lock(&shared.mutex);
            shared.networkError = int(error);
            shared.transferring = false;
            total = shared.total;
            cancelled = shared.cancelled;
            if (error == QNetworkReply::NoError && headersAccepted && responseEnd < 0) {
                shared.total = writeOffset;
                total = writeOffset;
            }
            shared.arrived.wakeAll();
        }
        if (cancelled) {
            return;
        }
        if (total == 0) {
            fail();
            return;
        }
        const bool complete = headersAccepted && error == QNetworkReply::NoError &&
                              (responseEnd < 0 || writeOffset == responseEnd);
        if (complete && (total < 0 || writeOffset >= total)) {
            notify();
            return;
        }
        if (complete && total > writeOffset) {
            request(writeOffset);
            return;
        }
        const bool retryable = status == 0 || status == 200 || status == 206 || status >= 500;
        if (!retryable || retries >= MAX_READ_RETRIES) {
            fail();
            return;
        }
        ++retries;
        {
            QMutexLocker lock(&shared.mutex);
            ++shared.reconnects;
        }
        notify();
        // 报错缓冲时间
        request(headersAccepted ? writeOffset : requested);
    }
};

HttpAudioStream::HttpAudioStream(QUrl url, QObject *parent)
    : QIODevice(parent), state(std::make_unique<State>()), cache(QDir::tempPath() + "/rhine-stream-XXXXXX"),
      source(std::move(url)) {
}

HttpAudioStream::~HttpAudioStream() {
    cancel();
    networkThread.quit();
    networkThread.wait();
    reader.close();
}

/** 建立会话缓存与网络线程 */
bool HttpAudioStream::start() {
    if (!cache.open()) {
        return false;
    }
    reader.setFileName(cache.fileName());
    if (!reader.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        return false;
    }
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    worker = new Worker(this, cache.fileName());
    worker->moveToThread(&networkThread);
    QObject::connect(&networkThread, &QThread::started, worker, [this] { worker->begin(); });
    QObject::connect(&networkThread, &QThread::finished, worker, &QObject::deleteLater);
    networkThread.start();
    return true;
}

void HttpAudioStream::cancel() {
    QMutexLocker lock(&state->mutex);
    state->cancelled = true;
    state->arrived.wakeAll();
}

bool HttpAudioStream::isSequential() const {
    QMutexLocker lock(&state->mutex);
    return state->total < 0;
}

qint64 HttpAudioStream::size() const {
    QMutexLocker lock(&state->mutex);
    return std::max<qint64>(0, state->total);
}

qint64 HttpAudioStream::pos() const {
    QMutexLocker lock(&state->mutex);
    return cursor;
}

bool HttpAudioStream::seek(qint64 position) {
    {
        QMutexLocker lock(&state->mutex);
        if (position < 0 || state->cancelled || (state->total >= 0 && position > state->total)) {
            return false;
        }
    }
    if (!QIODevice::seek(position)) {
        return false;
    }
    QMutexLocker lock(&state->mutex);
    cursor = position;
    return true;
}

bool HttpAudioStream::atEnd() const {
    QMutexLocker lock(&state->mutex);
    return state->total >= 0 && cursor >= state->total;
}

qint64 HttpAudioStream::bytesAvailable() const {
    QMutexLocker lock(&state->mutex);
    return state->available(cursor);
}

/** 仅在解码工作线程等待需要的范围 */
qint64 HttpAudioStream::readData(char *data, qint64 maximum) {
    if (maximum <= 0) {
        return 0;
    }
    QMutexLocker lock(&state->mutex);
    while (!state->cancelled && !state->broken) {
        const auto available = state->available(cursor);
        if (available > 0) {
            const auto count = std::min(available, maximum);
            if (!reader.seek(cursor)) {
                return -1;
            }
            const auto read = reader.read(data, count);
            if (read > 0) {
                cursor += read;
            }
            return read;
        }
        if (state->total >= 0 && cursor >= state->total) {
            return 0;
        }
        const bool arriving = state->transferring && cursor >= state->activeStart &&
                              cursor <= state->activeEnd + SEEK_AHEAD_BYTES;
        if (!arriving && state->wanted != cursor) {
            state->wanted = cursor;
            QMetaObject::invokeMethod(
                worker, [worker = worker] { worker->reposition(); }, Qt::QueuedConnection);
        }
        state->arrived.wait(&state->mutex);
    }
    return -1;
}

qint64 HttpAudioStream::writeData(const char *, qint64) {
    return -1;
}

QJsonObject HttpAudioStream::diagnosticState() const {
    QMutexLocker lock(&state->mutex);
    return {{"mediaHttpStatus", state->httpStatus},          {"mediaNetworkError", state->networkError},
            {"mediaTransferBytes", double(state->received)}, {"mediaExpectedBytes", double(state->total)},
            {"mediaReconnects", state->reconnects},          {"mediaTransferActive", state->transferring},
            {"mediaTransferFailed", state->broken}};
}
