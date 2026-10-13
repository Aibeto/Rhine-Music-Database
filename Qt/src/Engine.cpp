#include "Engine.h"
#include "DiscGeometry.h"
#include "RhiScene.h"
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QThread>
#include <cmath>
#include <cstring>
/**
 * 建立原生 Host、场景和 JS 状态层，并保持对象所有权边界。
 * 先注入桥接对象，再执行脚本初始化；定时器只负责已就绪场景。
 */
Engine::Engine(QString path, bool offline, QObject *parent)
    : QObject(parent), host(path, offline), scene(path, &js), m_glyphs(path) {
    QJSEngine::setObjectOwnership(&host, QJSEngine::CppOwnership);
    QJSEngine::setObjectOwnership(&m_glyphs, QJSEngine::CppOwnership);
    js.globalObject().setProperty("Host", js.newQObject(&host));
    js.globalObject().setProperty("Glyphs", js.newQObject(&m_glyphs));
    evaluate("var globalThis=this;Array.prototype.at=function(i){i=Math.trunc(i)||0;return "
             "this[i<0?this.length+i:i];};");
    QJSEngine::setObjectOwnership(&scene, QJSEngine::CppOwnership);
    js.globalObject().setProperty("RhineNative", js.newQObject(&scene));
    evaluate(host.read("qt-logic.js"), "qt-logic.js");
    app = evaluate("RhineQt.create(Host,Glyphs,RhineNative," + host.read("slots.json") + ")", "create");
    js.globalObject().setProperty("App", app);
    evaluate("App.init().catch(function(e){throw e;});");
    scene.diagnostics(offline);
    connect(&host, &Host::invalidated, this, [this] { wake(false); });
    connect(&host, &Host::mediaChanged, this, &Engine::pollMedia, Qt::QueuedConnection);
    connect(&host, &Host::mediaAction, this, [this](QString value) {
        call("mediaAction", {value});
        wake();
    });
    clock.start();
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &Engine::tick);
    mediaTimer.setInterval(1000);
    connect(&mediaTimer, &QTimer::timeout, this, &Engine::pollMedia);
    if (!offline) {
        start();
    }
}
Engine::~Engine() {
    timer.stop();
    mediaTimer.stop();
    host.shutdown();
    app = QJSValue();
    js.globalObject().setProperty("App", QJSValue());
}
QJSValue Engine::evaluate(QString code, QString name) {
    auto result = js.evaluate(code, name);
    if (result.isError()) {
        failure = result.toString() + "\n" + result.property("stack").toString();
        qWarning().noquote() << failure;
    }
    return result;
}
QJSValue Engine::call(QString method, QJSValueList args) {
    auto result = app.property(method).callWithInstance(app, args);
    if (result.isError()) {
        failure = result.toString() + "\n" + result.property("stack").toString();
        qWarning().noquote() << failure;
    }
    return result;
}
bool Engine::ready() const {
    return scene.ready();
}
namespace {
// 只有低于实际显示精度的浮点尾差才可视为收束，时间和诊断计数不参与画面失效。
bool equivalent(const QJsonValue &a, const QJsonValue &b) {
    if (a.type() != b.type()) {
        return false;
    }
    if (a.isDouble()) {
        return qAbs(a.toDouble() - b.toDouble()) <= 0.00001;
    }
    if (a.isArray()) {
        auto x = a.toArray();
        auto y = b.toArray();
        if (x.size() != y.size()) {
            return false;
        }
        for (int i = 0; i < x.size(); ++i) {
            if (!equivalent(x[i], y[i])) {
                return false;
            }
        }
        return true;
    }
    if (a.isObject()) {
        auto x = a.toObject();
        auto y = b.toObject();
        if (x.keys() != y.keys()) {
            return false;
        }
        for (auto it = x.begin(); it != x.end(); ++it) {
            if (!equivalent(it.value(), y[it.key()])) {
                return false;
            }
        }
        return true;
    }
    return a == b;
}
bool equivalent(const QByteArray &a, const QByteArray &b) {
    if (a == b) {
        return true;
    }
    if (a.size() != b.size()) {
        return false;
    }
    for (qsizetype i = 0; i + 4 <= a.size(); i += 4) {
        float x;
        float y;
        memcpy(&x, a.constData() + i, 4);
        memcpy(&y, b.constData() + i, 4);
        if (qAbs(x - y) > 0.00001f) {
            return false;
        }
    }
    return true;
}
}
bool Engine::eventFilter(QObject *watched, QEvent *event) {
    // 原生文本框和滑块也属于用户输入，不能只依赖资料库 MouseArea 的事件。
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        m_pressed = true;
        wake();
        break;
    case QEvent::MouseButtonRelease:
        m_pressed = false;
        wake();
        break;
    case QEvent::MouseMove:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        wake();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
void Engine::start() {
    m_started = true;
    wake(false);
}
QVariantMap Engine::performance() const {
    return {{"idle", m_sleeping},
            {"sceneSleeping", m_sceneSleeping},
            {"visible", m_visible},
            {"steps", qulonglong(m_steps)},
            {"sceneUpdates", qulonglong(m_sceneUpdates)},
            {"hudUpdates", qulonglong(m_hudUpdates)},
            {"renderFrames", qulonglong(m_renderFrames.load(std::memory_order_relaxed))},
            {"gpuResources", qulonglong(m_gpuResources.load(std::memory_order_relaxed))},
            {"timerActive", timer.isActive()}};
}
void Engine::wake(bool input) {
    const bool wasSleeping = m_sleeping;
    const auto now = clock.elapsed();
    if (input) {
        m_lastInput = now;
        m_sceneSleeping = false;
        m_stableFrames = 0;
        scene.energyIdle(false);
    } else if (scene.changed()) {
        m_sceneSleeping = false;
        m_stableFrames = 0;
    }
    m_sleeping = !m_visible;
    if (wasSleeping && !m_sleeping) {
        emit stateChanged();
    }
    if (m_started && m_visible && !timer.isActive()) {
        // 恢复时接续视觉时钟，不能把整段待机时间推进到弹簧和拖动惯性中。
        m_lastTick = now;
        timer.start(16);
    }
}
void Engine::setVisible(bool value) {
    if (m_visible == value) {
        return;
    }
    m_visible = value;
    if (!value) {
        flushPointer();
        call("pointer", {"pointercancel", 0, 0});
        m_pressed = m_wheelPending = false;
        m_wheelDelta = m_wheelSteps = 0;
        timer.stop();
        m_sleeping = true;
    } else {
        wake(false);
    }
    emit stateChanged();
}
void Engine::pollMedia() {
    call("poll", {clock.elapsed() / 1000., true});
    auto player = app.property("music").property("player");
    const bool active =
        player.property("loading").toBool() || player.property("state").property("playing").toBool();
    if (active && !mediaTimer.isActive()) {
        mediaTimer.start();
    }
    if (!active) {
        mediaTimer.stop();
    }
}
void Engine::flushPointer() {
    if (!m_pointerPending) {
        return;
    }
    m_pointerPending = false;
    call("pointer", {"pointermove", m_pointerX, m_pointerY});
}
/**
 * 推进本帧动画并决定是否继续调度。
 * 状态收束后允许待机，输入和后台数据失效会重新唤醒。
 */
void Engine::tick() {
    const auto now = clock.elapsed();
    m_animationTime += qMin<qint64>(now - m_lastTick, 50) / 1000.;
    m_lastTick = now;
    flushPointer();
    if (m_wheelPending) {
        const int steps = int(m_wheelSteps);
        call("wheel", {m_wheelX, m_wheelY, m_wheelDelta, steps});
        m_wheelSteps -= steps;
        m_wheelPending = false;
        m_wheelDelta = 0;
    }
    if (now - m_lastInput >= 750 && !m_pressed) {
        scene.energyIdle(true);
    }
    step(m_animationTime);
    if (now - m_lastInput >= 750 && !m_pressed && m_sceneSleeping && !scene.changed() &&
        !call("animating", {m_animationTime}).toBool() && m_frame["mode"] != "boot") {
        timer.stop();
        m_sleeping = true;
        emit stateChanged();
    }
}
void Engine::resize(double width, double height) {
    if (height <= 0 || (m_width == width && m_height == height)) {
        return;
    }
    m_width = width;
    m_height = height;
    scene.resize(width / (height / 1080), 1080);
    wake();
}
void Engine::action(QString value) {
    // 窗口模式交给 QML 原生窗口处理，不进入音乐状态或场景动画。
    if (value == "window:fullscreen") {
        emit fullscreenRequested();
        return;
    }
    call("action", {value});
    pollMedia();
    wake();
}
void Engine::form(QString value) {
    call("form", {value});
    wake();
}
void Engine::query(QString value) {
    call("query", {value});
    wake();
}
void Engine::key(QString value, bool command) {
    call("key", {value, command});
    wake();
}
void Engine::pointer(QString type, double x, double y) {
    if (type == "pointermove") {
        if (m_pointerX == x && m_pointerY == y && !m_pointerPending) {
            return;
        }
        m_pointerX = x;
        m_pointerY = y;
        m_pointerPending = true;
    } else {
        flushPointer();
        call("pointer", {type, x, y});
        if (type == "pointerdown") {
            m_pressed = true;
        }
        if (type == "pointerup" || type == "pointercancel" || type == "pointerleave") {
            m_pressed = false;
        }
    }
    wake();
}
void Engine::wheel(double x, double y, double delta, bool pixels) {
    if (!std::isfinite(delta) || delta == 0) {
        return;
    }
    m_wheelX = x;
    m_wheelY = y;
    m_wheelDelta += delta;
    m_wheelSteps += pixels ? -delta / 15. : (delta < 0 ? 1 : -1);
    m_wheelPending = true;
    wake();
}
/**
 * 按顺序同步场景、音乐和 HUD 帧数据，保持绘制与命中区域一致。
 */
void Engine::step(double time) {
    if (!ready()) {
        return;
    }
    ++m_steps;
    const bool reuse = m_sceneSleeping && !scene.changed();
    auto result = call("step", {time, m_width / (m_height / 1080), reuse});
    if (result.isError() || result.isNull() || result.isUndefined()) {
        return;
    }
    const auto object = QJsonObject::fromVariantMap(result.toVariant(QJSValue::ConvertJSObjects).toMap());
    m_frame = scene.frame;
    auto signature = m_frame;
    for (const auto &key : {"time", "stats", "labels", "boot", "pointerCursor"}) {
        signature.remove(key);
    }
    bool stable =
        equivalent(signature, m_sceneSignature) && m_previousBuffers.size() == scene.drawBuffers.size();
    for (auto it = scene.drawBuffers.begin(); stable && it != scene.drawBuffers.end(); ++it) {
        const auto old = m_previousBuffers.value(it.key());
        stable = equivalent(it->matrices, old.matrices) && equivalent(it->themes, old.themes);
    }
    m_stableFrames = stable ? m_stableFrames + 1 : 0;
    if (!stable || scene.changed()) {
        m_sceneSleeping = false;
    }
    m_sceneSignature = signature;
    m_previousBuffers = scene.drawBuffers;
    if (m_stableFrames >= 4 && !scene.changed() && m_frame["mode"] != "boot" &&
        !m_frame["pendingAnimations"].toBool()) {
        m_sceneSleeping = true;
    }
    const auto current = object["current"].toObject();
    const auto cover = object["cover"].toString();
    const auto coverRevision = host.image(cover).cacheKey();
    const bool visualChanged =
        !stable || current != m_current || cover != m_cover || coverRevision != m_coverRevision;
    m_coverRevision = coverRevision;
    m_current = current;
    m_cover = object["cover"].toString();
    auto commands = object["commands"].toArray();
    if (commands != m_commands) {
        m_commands = commands;
        ++m_hudUpdates;
        emit hudUpdated();
    }
    if (visualChanged) {
        ++m_sceneUpdates;
        emit sceneUpdated();
    }
    auto state = object["state"].toObject().toVariantMap();
    if (state != m_state) {
        m_state = state;
        emit stateChanged();
    }
    auto hits = object["hits"].toArray().toVariantList();
    if (hits != m_hits) {
        m_hits = hits;
        emit hitsChanged();
    }
    emit updated();
}
void Engine::snapshot(QString path) {
    if (m_frame.isEmpty()) {
        return;
    }
    QDir().mkpath(path);
    const QSize size(1920, 1080);
    auto metal = renderReference(root(), m_frame, m_current, cover(), size, scene.drawBuffers);
    saveImageNative(metal, path + "/qt-metal.png");
    QImage hud(size, QImage::Format_RGBA8888_Premultiplied);
    hud.fill(Qt::transparent);
    QPainter hudPainter(&hud);
    m_glyphs.commands(hudPainter, m_commands, [this](QString key) { return host.image(key); });
    hudPainter.end();
    saveImageNative(hud, path + "/qt-panel.png");
    QPainter painter(&metal);
    painter.drawImage(0, 0, hud);
    painter.end();
    metal.save(path + "/qt-composite.png");
    QFile file(path + "/qt-state.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(m_frame).toJson());
    }
}
int Engine::selfTest() {
    qInfo() << "[qt_offline] Waiting for runtime";
    QElapsedTimer wait;
    wait.start();
    while (!ready() && wait.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    if (!ready() || !failure.isEmpty()) {
        qCritical() << "Runtime failed" << failure;
        return 1;
    }
    qInfo() << "[qt_offline] Runtime ready; checking local search and cache";
    auto check = [this](QString expression) {
        auto r = evaluate(expression, "offline-regression");
        if (r.isError() || !r.toBool()) {
            qCritical() << "Failed:" << expression << r.toString();
            return false;
        }
        return true;
    };
    int requests = 0;
    host.fixture = [&](QString route, QJsonObject) {
        ++requests;
        if (route == "/search") {
            return QJsonDocument::fromJson(
                       R"({"body":{"code":200,"result":{"songCount":2,"songs":[{"id":1,"name":"本地歌曲","dt":180000,"ar":[{"name":"测试歌手"}],"al":{"id":7,"name":"测试专辑"}}]}}})")
                .object();
        }
        return QJsonDocument::fromJson(R"({"body":{"code":200,"songs":[]}})").object();
    };
    evaluate("App.runtime.setMode('archive');App.music.search('本地');");
    wait.restart();
    while (app.property("music").property("searchLoading").toBool() && wait.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    step(2.1);
    bool ok =
        !m_frame.isEmpty() && scene.failure.isEmpty() && DiscGeometry::validate() &&
        check("App.music.current.title==='本地歌曲' && App.music.totals[0]===2 && "
              "App.music.searchCursor===1") &&
        check("App.music.physicalSlots.length===5 && App.music.physicalSlots.every(function(x){return "
              "x.length===8;})") &&
        check(
            "App.parse({id:1,name:'x',ar:[{name:'A'}],al:{id:2,name:'B'},dt:125000},'song').artist==='A'") &&
        check("Host.fromLink('https://music.163.com/#/song?id=123')==='song:123' && "
              "Host.fromLink('https://evil.test/song?id=123')===''") &&
        check("new App.ReelCurve(0,1,0,0).sample(.46)[0]===1");
    // 覆盖启动背景、页脚和弹窗使用的四项 RGBA，避免离线测试漏掉真实 HUD 绘制路径。
    const QJsonArray swatches{
        QJsonArray{"fill", QJsonArray{119. / 255, 123. / 255, 96. / 255, 1}, 0, 0, 1, 1},
        QJsonArray{"fill", QJsonArray{.5, .5, .5, 1, "calibratedWhite"}, 1, 0, 1, 1},
        QJsonArray{"fill", QJsonArray{0, 0, 1, .5}, 2, 0, 1, 1}};
    QImage hudColors(3, 1, QImage::Format_RGBA8888_Premultiplied);
    hudColors.fill(Qt::transparent);
    {
        QPainter painter(&hudColors);
        m_glyphs.commands(painter, swatches, [](QString) { return QImage(); });
    }
    ok &= hudColors.pixelColor(0, 0) == QColor(119, 123, 96) &&
          qAbs(hudColors.pixelColor(1, 0).red() - 146) <= 1 &&
          qAbs(hudColors.pixelColor(2, 0).alpha() - 128) <= 1;
    qInfo() << "[qt_offline] HUD RGBA, calibrated color and alpha checks completed";
    // 同查询合并、稳定参数缓存、退出账号后 Cookie 不复活。
    const auto before = requests;
    evaluate("var "
             "cacheDone=0;App.music.api.search('本地','song').then(function(){cacheDone++;});App.music.api."
             "search('本地','song').then(function(){cacheDone++;});");
    wait.restart();
    while (js.globalObject().property("cacheDone").toInt() < 2 && wait.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    ok &= requests == before && check("cacheDone===2");
    ok &= Host::lifetime("/search") == 900 && Host::lifetime("/login/qr/check") == 0 &&
          Host::cacheKey("guest", "/search", {{"keywords", "x"}, {"timestamp", 1}}) ==
              Host::cacheKey("guest", "/search", {{"timestamp", 2}, {"keywords", "x"}});
    // 本地凭据样本验证两个平台互不退出，原始 QQ 记录不会进入脚本回调。
    host.cookies = {{"MUSIC_U", "fixture-netease"},
                    {"qqmusic_session", "fixture-qq"},
                    {"__qqSessions", QJsonArray{QJsonObject{{"credential", "fixture-secret"}}}}};
    host.setProvider("qq");
    ok &= host.authenticated();
    host.setProvider("netease");
    host.logout();
    ok &= !host.authenticated() && host.cookies["qqmusic_session"].toString() == "fixture-qq" &&
          host.cookies["__qqSessions"].isArray();
    host.setProvider("qq");
    ok &= host.authenticated();
    host.logout();
    ok &= !host.authenticated() && !host.cookies.contains("__qqSessions");
    host.setProvider("netease");
    const auto originalFixture = host.fixture;
    host.fixture = [](QString, QJsonObject) {
        return QJsonObject{{"body", QJsonObject{{"code", 200}}},
                           {"qqSessions", QJsonArray{QJsonObject{{"credential", "fixture-secret"}}}}};
    };
    evaluate("var qqPrivateResult=false;Host.request('/qq/user/detail',{},function(raw){"
             "var r=JSON.parse(raw);qqPrivateResult=!('qqSessions' in r)&&"
             "raw.indexOf('fixture-secret')<0;});");
    wait.restart();
    while (!js.globalObject().property("qqPrivateResult").toBool() && wait.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    ok &= check("qqPrivateResult");
    host.fixture = originalFixture;
    host.cookies = {};
    QFile fixtureFile(root() + "/scene-fixture.json");
    if (!fixtureFile.open(QIODevice::ReadOnly)) {
        qCritical() << "Missing offline scene fixture:" << fixtureFile.fileName()
                    << fixtureFile.errorString();
        return 1;
    }
    QJsonParseError fixtureError;
    auto fixture = QJsonDocument::fromJson(fixtureFile.readAll(), &fixtureError).object();
    if (fixtureError.error != QJsonParseError::NoError || !fixture["frame"].isObject() ||
        !fixture["rays"].isArray()) {
        qCritical() << "Invalid offline scene fixture:" << fixtureFile.fileName()
                    << fixtureError.errorString();
        return 1;
    }
    qInfo() << "[qt_offline] Checking scene matrices, picking and rebuild";
    scene.review("archive");
    scene.frameFast(50);
    auto expected = fixture["frame"].toObject();
    auto close = [](QJsonArray a, QJsonArray b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (int i = 0; i < a.size(); ++i) {
            if (qAbs(a[i].toDouble() - b[i].toDouble()) > 1e-5) {
                return false;
            }
        }
        return true;
    };
    ok &= close(scene.frame["view"].toArray(), expected["view"].toArray()) &&
          close(scene.frame["projection"].toArray(), expected["projection"].toArray());
    auto actualDraws = scene.frame["draws"].toArray();
    auto expectedDraws = expected["draws"].toArray();
    ok &= actualDraws.size() == expectedDraws.size();
    for (int i = 0; i < qMin(actualDraws.size(), expectedDraws.size()); ++i) {
        auto a = actualDraws[i].toObject();
        auto b = expectedDraws[i].toObject();
        ok &= a["name"] == b["name"] && a["count"] == b["count"] &&
              close(a["matrices"].toArray(), b["matrices"].toArray()) &&
              close(a["themes"].toArray(), b["themes"].toArray());
    }
    for (auto ray : fixture["rays"].toArray()) {
        auto r = ray.toObject();
        auto hit = scene.pick(r["x"].toDouble(), r["y"].toDouble());
        const auto value = QJsonValue::fromVariant(hit.toVariant(QJSValue::ConvertJSObjects));
        ok &= value == r["hit"];
    }
    ok &= scene.failure.isEmpty();
    // 再次重建，覆盖旧几何释放与缓存回收路径。
    ok &= scene.review("archive");
    scene.frameFast(50);
    ok &= close(scene.frame["view"].toArray(), expected["view"].toArray());
    if (qEnvironmentVariableIntValue("RHINE_GPU_TEST") == 1) {
        qInfo() << "[qt_offline] Starting GPU render; software renderer:"
                << qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU");
        auto pixels = renderReference(root(), scene.frame, m_current, {}, QSize(640, 360));
        qInfo() << "[qt_offline] GPU readback completed" << pixels.size();
        ok &= pixels.size() == QSize(640, 360);
        int contrast = 0;
        auto first = pixels.pixelColor(0, 0);
        for (int y = 0; y < pixels.height(); y += 12) {
            for (int x = 0; x < pixels.width(); x += 12) {
                if (pixels.pixelColor(x, y) != first) {
                    ++contrast;
                }
            }
        }
        ok &= contrast > 20;
        // 资料库详情的象牙色外壳不能被内部光照层染红；非空读回无法覆盖此回归。
        ok &= scene.review("detail");
        scene.frameFast(50);
        auto detail = renderReference(root(), scene.frame, m_current, {}, QSize(640, 360));
        double redBias = 0;
        int samples = 0;
        for (int y = 150; y < 250; y += 4) {
            for (int x = 84; x < 116; x += 4) {
                const auto color = detail.pixelColor(x, y);
                redBias += color.red() - color.green();
                ++samples;
            }
        }
        redBias /= samples;
        qInfo() << "[qt_offline] Library shell red bias:" << redBias;
        ok &= redBias < 20;
    }
    qInfo() << "Qt offline scene, parsing, search, cache, link and motion checks:"
            << (ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

int Engine::renderFixtures(QString source, QString output) {
    for (const auto &name : QDir(source).entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile input(source + "/" + name);
        if (!input.open(QIODevice::ReadOnly)) {
            return 1;
        }
        const auto fixture = QJsonDocument::fromJson(input.readAll()).object();
        js.globalObject().setProperty("Fixture", js.toScriptValue(fixture.toVariantMap()));
        auto result = evaluate(R"(
            Object.assign(App.music, Fixture.store);
            if (!App.music.current.id) {
                App.music.current = null;
            }
            App.music.directory = Fixture.store.directory || null;
            App.hud.rolling = {};
            App.hud.trackOffset = 0;
            App.hud.bodyOffset = 0;
            App.hud.listOffset = 0;
            Object.assign(App.hud, Fixture.hud);
            App.hud.modal = Fixture.hud.modal || null;
            App.hud.motion.set(true, 0, true);
            App.hud.draw(Fixture.frame, 1920, 50);
        )",
                               "visual-fixture");
        if (result.isError()) {
            return 1;
        }
        m_frame = fixture["frame"].toObject();
        m_current = fixture["store"].toObject()["current"].toObject();
        m_commands = QJsonArray::fromVariantList(result.property("commands").toVariant().toList());
        m_cover.clear();
        auto folder = output + "/" + fixture["name"].toString();
        snapshot(folder);
        QFile hitsFile(folder + "/qt-hits.json");
        if (hitsFile.open(QIODevice::WriteOnly)) {
            hitsFile.write(
                QJsonDocument(QJsonArray::fromVariantList(result.property("hits").toVariant().toList()))
                    .toJson());
        }
        qInfo().noquote() << "Qt visual" << fixture["name"].toString();
    }
    return failure.isEmpty() ? 0 : 1;
}
