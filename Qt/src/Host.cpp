#include "Host.h"
#include "AppStorage.h"
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QImageReader>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStyleHints>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>

static QString hash(QByteArray value) {
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex();
}
Host::Host(QString path, bool test, QObject *parent) : QObject(parent), root(path), offline(test) {
    if (!offline) {
        cookies = QJsonDocument::fromJson(platformCredential()).object();
    } else {
        interval = 0;
    }
    if (!offline) {
        platformMediaCommands([this](QString action) {
            QMetaObject::invokeMethod(
                this, [this, action] { emit mediaAction(action); }, Qt::QueuedConnection);
        });
    }
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { emit invalidated(); });
    connect(&child, &QProcess::started, this, [this] {
        const auto keys = pending.values();
        pending.clear();
        for (const auto &key : keys) {
            send(key);
        }
    });
    connect(&child, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return;
        }
        const auto keys = pending.values();
        pending.clear();
        for (const auto &key : keys) {
            finish(key, {}, QStringLiteral("音乐服务组件缺失，请重新构建应用"));
        }
    });
    connect(&child, &QProcess::readyReadStandardOutput, this, [this] {
        buffer += child.readAllStandardOutput();
        if (buffer.size() >= 32 * 1024 * 1024) {
            child.kill();
            buffer.clear();
            return;
        }
        qsizetype end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const auto line = buffer.left(end);
            buffer.remove(0, end + 1);
            const auto response = QJsonDocument::fromJson(line).object();
            const auto id = response["id"].toString();
            if (pending.contains(id)) {
                finish(pending.take(id), response);
            }
        }
    });
    connect(&child, &QProcess::finished, this, [this] {
        const auto keys = pending.values();
        pending.clear();
        buffer.clear();
        for (const auto &key : keys) {
            finish(key, {}, QStringLiteral("音乐服务已退出，请重试"));
        }
    });
}
Host::~Host() {
    shutdown();
}
int Host::lifetime(const QString &route) {
    if (route.startsWith("/qq/")) {
        if (route.startsWith("/qq/login/") || route == "/qq/logout" || route == "/qq/getMusicPlay") {
            return 0;
        }
        return route.startsWith("/qq/user/") ? 60 : 900;
    }
    if (route == "/song/detail" || route == "/song/wiki/summary") {
        return 86400;
    }
    if (route == "/album" || route == "/artists" || route == "/artist/album") {
        return 21600;
    }
    if (QStringList{"/playlist/detail", "/user/playlist", "/likelist", "/album/sublist", "/artist/sublist"}
            .contains(route)) {
        return 600;
    }
    if (route == "/user/account") {
        return 1800;
    }
    if (QStringList{"/personalized", "/toplist", "/album/newest", "/top/playlist", "/playlist/catlist"}
            .contains(route)) {
        return 900;
    }
    if (route == "/recommend/songs") {
        return 60;
    }
    if (route == "/search" || route == "/search/suggest") {
        return 900;
    }
    return 0;
}
QString Host::cacheKey(QString scope, QString route, QJsonObject params) {
    params.remove("timestamp");
    return hash((scope + route).toUtf8() + QJsonDocument(params).toJson(QJsonDocument::Compact));
}
QString Host::diskPath(QString key) const {
    return AppStorage::cacheDirectory() + "/metadata/" + key + ".json";
}
/**
 * 接收脚本层请求，在协议外层执行缓存、合并和限流。
 * 记录当前账号代数，异步结果只能交付给仍有效的请求。
 */
void Host::request(QString route, QJSValue params, QJSValue callback) {
    if (stopped) {
        return;
    }
    auto values = QJsonObject::fromVariantMap(params.toVariant().toMap());
    const auto token = cookies[route.startsWith("/qq/") ? "qqmusic_session" : "MUSIC_U"].toString();
    const auto scope = token.isEmpty() ? "guest" : hash(token.toUtf8());
    const bool cached = lifetime(route) > 0;
    const auto key =
        cached
            ? cacheKey("enhanced-4.41.1/qq-3.1.5/" + scope + QString::number(metadataRevision), route, values)
            : QUuid::createUuid().toString();
    const bool disk = !offline && scope == "guest" &&
                      QStringList{"/song/detail",  "/song/wiki/summary", "/album", "/artists",
                                  "/artist/album", "/playlist/detail",   "/search"}
                          .contains(route);
    Waiter waiter{callback, epoch};
    if (disk && !cache.contains(key)) {
        QFile f(diskPath(key));
        if (f.open(QIODevice::ReadOnly)) {
            auto obj = QJsonDocument::fromJson(f.readAll()).object();
            cache.insert(key, {qint64(obj["expiry"].toDouble()), obj["body"].toObject()});
        }
    }
    if (cached && cache.contains(key) && cache[key].expiry > QDateTime::currentMSecsSinceEpoch()) {
        auto body = cache[key].body;
        QTimer::singleShot(0, this, [=, this] { respond(waiter, {{"body", body}}, {}); });
        return;
    }
    if (flights.contains(key)) {
        flights[key].waiters.append(waiter);
        return;
    }
    flights.insert(
        key, {QUuid::createUuid().toString(), route, key, values, cookies, {waiter}, disk, metadataRevision});
    if (cached || route == "/song/url" || route == "/song/url/v1") {
        queue.enqueue(key);
        pump();
    } else {
        send(key);
    }
}
void Host::pump() {
    if (stopped || !active.isEmpty() || queue.isEmpty()) {
        return;
    }
    active = queue.dequeue();
    const auto service = flights[active].route.startsWith("/qq/") ? "qq" : "netease";
    if (blockedUntil[service] > QDateTime::currentMSecsSinceEpoch()) {
        const auto key = active;
        QTimer::singleShot(0, this, [=, this] {
            finish(key, {}, QStringLiteral("音乐服务提示访问频繁，已暂停该平台的新请求；缓存仍可浏览"));
        });
        return;
    }
    auto key = active;
    int wait = int(qMax<qint64>(0, interval - (QDateTime::currentMSecsSinceEpoch() - lastStart)));
    QTimer::singleShot(wait, this, [=, this] {
        if (!stopped) {
            lastStart = QDateTime::currentMSecsSinceEpoch();
            send(key);
        }
    });
}
void Host::send(QString key) {
    if (!flights.contains(key) || stopped) {
        return;
    }
    auto work = flights[key];
    if (offline) {
        QTimer::singleShot(0, this, [=, this] {
            if (fixture) {
                finish(key, fixture(work.route, work.params));
            } else {
                finish(key, {}, "Offline: no local fixture for " + work.route);
            }
        });
        return;
    }
    if (child.state() == QProcess::NotRunning) {
        temporary = std::make_unique<QTemporaryDir>(AppStorage::temporaryDirectory() + "/rhine-music-XXXXXX");
        if (!temporary->isValid()) {
            finish(key, {}, "无法创建音乐服务临时目录");
            return;
        }
        QProcessEnvironment env;
#ifdef Q_OS_WIN
        const auto system = QProcessEnvironment::systemEnvironment();
        for (const auto &name : {"SystemRoot", "WINDIR"}) {
            if (system.contains(name)) {
                env.insert(name, system.value(name));
            }
        }
        env.insert("PATH", system.value("SystemRoot") + "/System32");
        // 上游运行时的用户目录和临时目录也限定在绿色版目录，不继承旧版缓存位置。
        env.insert("USERPROFILE", AppStorage::dataDirectory());
        env.insert("APPDATA", AppStorage::dataDirectory());
        env.insert("LOCALAPPDATA", AppStorage::dataDirectory());
        env.insert("TEMP", temporary->path());
        env.insert("TMP", temporary->path());
#else
        env.insert("PATH", "/usr/bin:/bin");
#endif
        env.insert("TMPDIR", temporary->path());
        env.insert("LANG", "en_US.UTF-8");
        child.setProcessEnvironment(env);
        child.setWorkingDirectory(root + "/MusicBridge");
        child.setStandardErrorFile(QProcess::nullDevice());
        // 平台选择放在宏调用外，避免 MSVC 将条件编译指令当作宏参数。
#ifdef Q_OS_WIN
        const auto nodeName = QStringLiteral("node.exe");
#else
        const auto nodeName = QStringLiteral("node");
#endif
        pending.insert(work.id, key);
        child.start(root + "/MusicBridge/" + nodeName, {root + "/MusicBridge/worker.cjs"});
        return;
    }
    if (child.state() == QProcess::Starting) {
        pending.insert(work.id, key);
        return;
    }
    pending.insert(work.id, key);
    QJsonObject envelope{
        {"id", work.id}, {"route", work.route}, {"params", work.params}, {"cookies", work.cookies}};
    child.write(QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n');
    QTimer::singleShot(30000, this, [=, this] {
        if (pending.remove(work.id)) {
            finish(key, {}, "音乐请求超时，请重试");
        }
    });
}
void Host::finish(QString key, QJsonObject response, QString error) {
    if (!flights.contains(key)) {
        return;
    }
    const auto work = flights.take(key);
    // QQ 原始认证记录只经过 C++ 与子进程，禁止出现在脚本回调中。
    if (response["qqSessions"].isArray() && response["qqSessions"] != cookies["__qqSessions"] &&
        !work.waiters.isEmpty() && work.waiters.first().epoch == epoch) {
        cookies["__qqSessions"] = response["qqSessions"];
        if (!cookies["qqmusic_session"].toString().isEmpty()) {
            saveCredential();
        }
    }
    response.remove("qqSessions");
    const auto body = response["body"].toObject();
    const auto code = body["code"].toInt(200);
    if (lifetime(work.route) || work.route == "/song/url" || work.route == "/song/url/v1") {
        const auto message = body["message"].toString(body["msg"].toString());
        if (QList<int>{405, 429, 509}.contains(code) || message.contains("频繁")) {
            blockedUntil[work.route.startsWith("/qq/") ? "qq" : "netease"] =
                QDateTime::currentMSecsSinceEpoch() + qMax(120000LL, qint64(body["retryAfterMs"].toDouble()));
        }
        if (work.cacheRevision == metadataRevision && lifetime(work.route) && error.isEmpty() &&
            response["body"].isObject() && code == 200) {
            Entry entry{QDateTime::currentMSecsSinceEpoch() + lifetime(work.route) * 1000LL, body};
            cache.insert(key, entry);
            if (cache.size() > 256) {
                for (auto it = cache.begin(); it != cache.end();) {
                    if (it->expiry <= QDateTime::currentMSecsSinceEpoch()) {
                        it = cache.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (cache.size() > 256) {
                    cache.erase(cache.begin());
                }
            }
            if (work.disk) {
                auto path = diskPath(key);
                QDir().mkpath(QFileInfo(path).path());
                QFile::setPermissions(QFileInfo(path).path(),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
                QSaveFile f(path);
                if (f.open(QIODevice::WriteOnly)) {
                    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
                    f.write(QJsonDocument(QJsonObject{{"expiry", double(entry.expiry)}, {"body", body}})
                                .toJson(QJsonDocument::Compact));
                    f.commit();
                }
            }
        }
    }
    for (auto waiter : work.waiters) {
        respond(waiter, response, error);
    }
    if (active == key) {
        active.clear();
        pump();
    }
}
void Host::respond(const Waiter &waiter, QJsonObject response, QString error) {
    if (stopped) {
        return;
    }
    response["_epoch"] = waiter.epoch;
    auto callback = waiter.callback;
    callback.call({QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)), error});
}
/**
 * 只接受当前账号代数对应的 Cookie，避免过期请求覆盖新登录。
 */
void Host::acceptCookies(QString json, int responseEpoch) {
    if (responseEpoch != epoch || stopped) {
        return;
    }
    for (auto line : QJsonDocument::fromJson(json.toUtf8()).array()) {
        for (auto cookie : QNetworkCookie::parseCookies(line.toString().toUtf8())) {
            cookies[QString::fromUtf8(cookie.name())] = QString::fromUtf8(cookie.value());
        }
    }
}
void Host::delay(int ms, QJSValue callback) {
    QTimer::singleShot(qMax(0, ms), this, [this, callback]() mutable {
        if (!stopped) {
            callback.call();
        }
    });
}
bool Host::authenticated() const {
    return hasCredential(provider);
}
bool Host::hasCredential(QString value) const {
    if (value != "qq" && value != "netease") {
        return false;
    }
    return !cookies[value == "qq" ? "qqmusic_session" : "MUSIC_U"].toString().isEmpty();
}
/**
 * 切换音乐来源，保持两种来源的凭据与缓存隔离。
 */
void Host::setProvider(QString value) {
    if (value != "qq" && value != "netease") {
        return;
    }
    if (provider == value) {
        return;
    }
    provider = value;
    ++epoch;
    ++metadataRevision;
    cache.clear();
    images.clear();
    coverFlights.clear();
}
QString Host::saveCredential() {
    return offline ? QString()
                   : platformSaveCredential(QJsonDocument(cookies).toJson(QJsonDocument::Compact));
}
/**
 * 作废当前来源的请求并清理凭据，另一来源的账号保持独立。
 */
void Host::logout() {
    mediaControl("stop");
    if (provider == "qq") {
        cookies.remove("qqmusic_session");
        cookies.remove("__qqSessions");
    } else {
        const auto token = cookies.value("qqmusic_session");
        const auto sessions = cookies.value("__qqSessions");
        cookies = {};
        if (!token.isUndefined()) {
            cookies["qqmusic_session"] = token;
        }
        if (!sessions.isUndefined()) {
            cookies["__qqSessions"] = sessions;
        }
    }
    ++epoch;
    // 结束进程同时关闭 QQ 的扫码长连接；另一平台的持久凭据仍保留。
    if (child.state() != QProcess::NotRunning) {
        child.kill();
        child.waitForFinished(1000);
    }
    cache.clear();
    images.clear();
    coverFlights.clear();
    if (!offline) {
        if (cookies.isEmpty()) {
            platformDeleteCredential();
        } else {
            saveCredential();
        }
    }
}
QString Host::fromLink(QString input) const {
    input = input.remove('\\').trimmed();
    QUrl outer(input);
    if (!QStringList{"music.163.com", "y.music.163.com"}.contains(outer.host()) ||
        !QStringList{"http", "https"}.contains(outer.scheme())) {
        return {};
    }
    QUrl page = outer;
    if (outer.hasFragment()) {
        auto part = outer.fragment();
        page = QUrl("https://music.163.com" + (part.startsWith('/') ? part : "/" + part));
    }
    auto kind = page.path().split('/', Qt::SkipEmptyParts).last();
    auto id = QUrlQuery(page).queryItemValue("id");
    if (!QStringList{"song", "album", "playlist"}.contains(kind) ||
        !QRegularExpression("^[0-9]{1,20}$").match(id).hasMatch()) {
        return {};
    }
    return kind + ":" + id;
}
void Host::cover(QString input, QJSValue callback) {
    QUrl url(input.replace("http://", "https://"));
    const auto domain = url.host();
    if (!(domain == "music.126.net" || domain.endsWith(".music.126.net") || domain == "music.163.com" ||
          domain.endsWith(".music.163.com") || domain == "y.gtimg.cn" || domain == "qpic.y.qq.com" ||
          domain == "y.qq.com")) {
        return;
    }
    QUrlQuery query(url);
    query.removeAllQueryItems("param");
    query.addQueryItem("param", "512y512");
    url.setQuery(query);
    auto key = url.toString();
    if (images.contains(key)) {
        callback.call({key});
        return;
    }
    if (offline) {
        return;
    }
    if (coverFlights.contains(key)) {
        coverFlights[key].append({callback, epoch});
        return;
    }
    coverFlights[key] = {{callback, epoch}};
    const auto imageEpoch = epoch;
    QNetworkRequest request(url);
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    auto reply = network.get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 size, qint64) {
        if (size >= 8 * 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        const auto bytes = reply->readAll();
        if (imageEpoch != epoch || stopped) {
            reply->deleteLater();
            return;
        }
        const bool valid = reply->error() == QNetworkReply::NoError &&
                           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                           bytes.size() < 8 * 1024 * 1024;
        reply->deleteLater();
        if (!valid) {
            coverFlights.remove(key);
            return;
        }
        // 工作者只接触不可变字节和 QImage；JS 回调与缓存仍在 Host 所在线程执行。
        auto watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key, imageEpoch] {
            const auto image = watcher->result();
            watcher->deleteLater();
            if (imageEpoch != epoch || stopped) {
                return;
            }
            const auto waiters = coverFlights.take(key);
            bool current = false;
            for (const auto &waiter : waiters) {
                current |= waiter.epoch == epoch;
            }
            if (stopped || !current || image.isNull()) {
                return;
            }
            // QCache 按内存预算逐项淘汰，不再在第 32 张封面清空所有命中。
            images.insert(key, new QImage(image), qMax(1, int(image.sizeInBytes() / 1024)));
            for (const auto &waiter : waiters) {
                if (waiter.epoch == epoch) {
                    auto callback = waiter.callback;
                    callback.call({key});
                }
            }
        });
        watcher->setFuture(QtConcurrent::run([bytes] {
            QBuffer buffer;
            buffer.setData(bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const auto size = reader.size();
            if (!size.isValid() || size.width() > 8192 || size.height() > 8192) {
                return QImage();
            }
            reader.setScaledSize(size.scaled(512, 512, Qt::KeepAspectRatio));
            return reader.read();
        }));
    });
}

QString Host::qrImage(QString data) {
    if (data.size() > 8 * 1024 * 1024 ||
        !QRegularExpression("^data:image/(png|jpeg|gif|webp);base64,").match(data).hasMatch()) {
        return {};
    }
    const auto bytes = QByteArray::fromBase64(data.mid(data.indexOf(',') + 1).toLatin1());
    auto value = QImage::fromData(bytes);
    if (value.isNull() || value.width() > 4096 || value.height() > 4096) {
        return {};
    }
    const auto key = "qq-qr:" + hash(bytes);
    images.insert(key, new QImage(value), int(value.sizeInBytes() / 1024));
    return key;
}
QString Host::qr(QString key) {
    auto name = "qr:" + key;
    images.insert(name, new QImage(platformQR("https://music.163.com/login?codekey=" + key)), 1024);
    return name;
}
QImage Host::image(const QString &key) const {
    auto p = images.object(key);
    return p ? *p : QImage();
}
void Host::fixtureImage(const QString &key, const QImage &image) {
    // 仅离线窗口验收可注入本地封面，不读取账号或访问封面服务器。
    if (offline && !image.isNull()) {
        images.insert(key, new QImage(image), int(image.sizeInBytes() / 1024));
    }
}
void Host::play(QString id, QJSValue callback) {
    if (!QRegularExpression("^[0-9]{1,20}$").match(id).hasMatch()) {
        callback.call({"请先选择一首歌曲"});
        return;
    }
    if (offline) {
        callback.call({"OFFLINE_PLAY:" + id});
        return;
    }
    QPointer<Host> guarded(this);
    platformPlay(id, activates(), [guarded, callback](QString message) mutable {
        if (guarded && !guarded->stopped) {
            callback.call({message});
        }
    });
}
bool Host::mediaLoad(QString url, bool alternative) {
    if (!audio) {
        audio = std::make_unique<AudioPlayer>([this] { emit mediaChanged(); });
    }
    return audio->load(url, offline, alternative, provider);
}
void Host::mediaControl(QString action, double value) {
    if (!audio && action == "volume") {
        audio = std::make_unique<AudioPlayer>([this] { emit mediaChanged(); });
    }
    if (audio) {
        audio->control(action, value);
    }
    emit mediaChanged();
}
void Host::mediaMetadata(QString title, QString artist, QString album) {
    mediaTitle = title.left(256);
    mediaArtist = artist.left(256);
    mediaAlbum = album.left(256);
    lastMediaUpdate = 0;
}
QVariantMap Host::mediaState() const {
    auto state = audio ? audio->state() : QVariantMap();
    auto now = QDateTime::currentMSecsSinceEpoch();
    if (!offline && now - lastMediaUpdate >= 1000) {
        lastMediaUpdate = now;
        platformMediaInfo(mediaTitle, mediaArtist, mediaAlbum, state);
    }
    return state;
}
void Host::copy(QString text) {
    QGuiApplication::clipboard()->setText(text);
}
void Host::open(QString url) {
    if (!offline && QUrl(url).scheme() == "https") {
        QDesktopServices::openUrl(QUrl(url));
    }
}
bool Host::systemDark() const {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
void Host::invalidateMetadata() {
    ++metadataRevision;
    cache.clear();
    if (offline) {
        return;
    }
    QDir metadata(AppStorage::cacheDirectory() + "/metadata");
    for (const auto &name : metadata.entryList({"*.json"}, QDir::Files)) {
        metadata.remove(name);
    }
}
QString Host::preference(QString key) const {
    if (offline || !QStringList{"search-history", "play-history", "theme", "motion", "volume", "quality",
                                "alternate-source", "provider", "welcome-name-netease", "welcome-name-qq"}
                        .contains(key)) {
        return {};
    }
    return AppStorage::settings().value("panel/" + key).toString();
}
void Host::setPreference(QString key, QString value) {
    if (!offline && value.size() < 262144 &&
        QStringList{"search-history", "play-history", "theme", "motion", "volume", "quality",
                    "alternate-source", "provider", "welcome-name-netease", "welcome-name-qq"}
            .contains(key)) {
        AppStorage::settings().setValue("panel/" + key, value);
    }
}
bool Host::activates() const {
    return offline ? false : platformActivates();
}
void Host::setActivates(bool value) {
    if (!offline) {
        platformSetActivates(value);
    }
}
QString Host::asset(QString name) const {
    return QUrl::fromLocalFile(root + "/" + name).toString();
}
QString Host::read(QString name) const {
    QFile f(root + "/" + name);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(f.readAll());
}
/**
 * 停止桥接子进程并释放等待请求，避免退出后仍有回调。
 */
void Host::shutdown() {
    if (stopped) {
        return;
    }
    stopped = true;
    coverFlights.clear();
    if (!offline) {
        platformMediaCommands({});
        platformMediaInfo({}, {}, {}, {});
    }
    audio.reset();
    disconnect(&child, nullptr, this, nullptr);
    child.closeWriteChannel();
    if (child.state() != QProcess::NotRunning) {
        child.terminate();
        if (!child.waitForFinished(2000)) {
            child.kill();
            child.waitForFinished(2000);
        }
    }
    flights.clear();
    pending.clear();
    queue.clear();
    temporary.reset();
}
