#include "Engine.h"
#include "RhiScene.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QWheelEvent>
#include <functional>

int runUiChecks(Engine &backend, QQuickWindow *window, const QString &output) {
    if (!window) {
        return 1;
    }
    auto settle = [] {
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 150) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
    };
    QElapsedTimer wait;
    wait.start();
    while (!backend.ready() && wait.elapsed() < 10000) {
        settle();
    }
    if (!backend.ready() || !backend.scene.review("archive")) {
        return 1;
    }
    auto evaluate = [&](const QString &source) {
        auto result = backend.js.evaluate(source, "ui-regression");
        if (result.isError()) {
            qCritical() << result.toString();
        }
        return result;
    };
    bool ok = true;
    auto check = [&](bool value, const char *message) {
        if (!value) {
            qCritical() << "UI regression:" << message;
        }
        ok &= value;
    };
    // 本地数据隔离账号和网络；真实 QML 窗口仍负责事件命中、坐标映射和绘制。
    evaluate(R"(
        var uiSongs = Array.from(
            {
                length: 40,
            },
            function (_, i) {
                return {
                    id: String(i + 1),
                    title: '本地测试曲目 ' + (i + 1),
                    artist: '测试歌手',
                    kind: 'song',
                    album: '测试专辑',
                    duration: 180000,
                };
            },
        );
        App.music.observe = function () {
            // 离线测试禁用此回调，避免触发外部请求或刷新。
        };
        App.music.loadMoreSource = function () {
            // 离线测试禁用此回调，避免触发外部请求或刷新。
        };
        App.music.loadTrackPage = function () {
            // 离线测试禁用此回调，避免触发外部请求或刷新。
        };
        App.music.lists[0] = uiSongs;
        App.music.lists[1] = uiSongs;
        App.music.hasMore = [false, false, false, false, false];
        App.music.keyword = '本地测试';
        App.music.status = '离线界面验证';
        App.hud.reduced = true;
        App.hud.openModal('search');
    )");
    auto refresh = [&] {
        backend.step(50);
        settle();
    };
    // 从终端直接启动时 macOS 可能把窗口留在后台；焦点验收先申请激活。
    window->requestActivate();
    refresh();
    const double scale = window->height() / 1080.;
    const double width = window->width() / scale;
    auto wheel = [&](double x, double y, bool pixels) {
        const QPointF position(x * scale, y * scale);
        QWheelEvent event(position, window->mapToGlobal(position), pixels ? QPoint(0, -15) : QPoint(),
                          pixels ? QPoint() : QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(window, &event);
        refresh();
    };
    wheel(width * .7, 500, false);
    check(evaluate("App.hud.listOffset===1").toBool(), "search row wheel routing");
    wheel(width * .7, 500, true);
    check(evaluate("App.hud.listOffset===2").toBool(), "trackpad pixel wheel routing");
    std::function<QQuickItem *(QQuickItem *)> findInput = [&](QQuickItem *item) -> QQuickItem * {
        if (item->objectName() == "musicSearchInput") {
            return item;
        }
        for (auto child : item->childItems()) {
            if (auto result = findInput(child)) {
                return result;
            }
        }
        return nullptr;
    };
    auto input = findInput(window->contentItem());
    const double modalWidth = qMin(width - 100., qMax(760., qMin(1000., width * .5 - 59)));
    check(input && qAbs(input->x() / scale - (width - modalWidth - 59 + 53)) < .01 &&
              qAbs(input->width() / scale - (modalWidth - 106)) < .01 && input->hasActiveFocus(),
          "search input alignment and keyboard focus");
    auto click = [&](const QString &action) {
        for (const auto &value : backend.hits()) {
            const auto hit = value.toMap();
            if (hit.value("action").toString() != action) {
                continue;
            }
            const QPointF position((hit.value("x").toDouble() + hit.value("width").toDouble() / 2) * scale,
                                   (hit.value("y").toDouble() + hit.value("height").toDouble() / 2) * scale);
            for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, position, position, window->mapToGlobal(position), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(window, &event);
            }
            refresh();
            return true;
        }
        return false;
    };
    check(click("music:page-next"), "next page button is visible");
    check(evaluate("App.hud.listOffset===7").toBool(), "native next page click");
    check(click("music:page-prev"), "previous page button is visible");
    check(evaluate("App.hud.listOffset===2").toBool(), "native previous page click");
    if (!output.isEmpty()) {
        QDir().mkpath(output);
        window->grabWindow().save(output + "/search.png");
    }
    evaluate("App.hud.openModal('saved'); App.hud.musicCollection=1;");
    refresh();
    wheel(width / 2, 500, false);
    check(evaluate("App.hud.listOffset===1").toBool(), "saved music row scrolling");
    // 生成本地封面验证圆形印刷、中心孔、透明壳以及真实窗口中的选曲封面更新。
    QImage cover(512, 512, QImage::Format_RGBA8888_Premultiplied);
    cover.fill(QColor(78, 153, 197));
    QPainter coverPainter(&cover);
    coverPainter.setRenderHint(QPainter::Antialiasing);
    coverPainter.setPen(QPen(QColor(213, 233, 231), 7));
    coverPainter.drawEllipse(QRectF(52, 52, 408, 408));
    backend.glyphs()->text(coverPainter, "RHINE LAB", 85, 112, 45, "bold", 1, Qt::white);
    backend.glyphs()->text(coverPainter, "MUSIC ARCHIVE", 75, 355, 31, "demibold", 1, Qt::white);
    coverPainter.end();
    backend.host.fixtureImage("fixture-disc", cover);
    evaluate(R"(
        App.hud.modal = null;
        App.hud.trackOffset = 0;
        App.music.current = {
            kind: 'playlist',
            id: '1',
            title: '《鸣潮》巡回演唱会「致予新世界」上海歌单 / 长标题布局验收',
            artist: '测试歌手',
        };
        App.music.tracks = uiSongs;
        App.music.coverImage = 'fixture-disc';
        App.runtime.setMode('detail');
    )");
    check(backend.scene.review("detail"), "playlist detail scene readiness");
    refresh();
    wheel(width * .5, 500, false);
    check(evaluate("App.hud.trackOffset===1").toBool(), "playlist track row scrolling");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/playlist.png");
    }
    const QSize originalWindowSize = window->size();
    window->resize(1152, 720);
    refresh();
    refresh();
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/playlist-16x10.png");
    }
    window->resize(originalWindowSize);
    refresh();
    refresh();
    evaluate(R"(
        App.music.current = App.parse(
            {
                id: 1,
                name: '本地光盘模型验收',
                ar: [
                    {
                        id: 10,
                        name: '测试歌手',
                    },
                ],
                al: {
                    id: 7,
                    name: '测试专辑',
                },
                dt: 180000,
            },
            'song',
        );
        App.music.coverImage = 'fixture-disc';
        App.music.detailLoading = false;
        App.hud.tab = 'overview';
        App.music.current.wiki = '本地生成封面 / 无网络访问';
    )");
    refresh();
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/disc-case.png");
    }
    evaluate("App.runtime.reduced(true);App.runtime.openViewer();App.runtime.coverFront();");
    refresh();
    // 比较封面中心区域，避开 HUD；真实窗口不能继续采样资料库的旧景深纹理。
    const auto actualViewer = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    const auto expectedViewer = renderReference(backend.root(), backend.frame(), backend.current(),
                                                backend.cover(), actualViewer.size(), backend.drawBuffers())
                                    .convertToFormat(QImage::Format_RGB32);
    int matchingPixels = 0;
    int sampledPixels = 0;
    if (actualViewer.size() == expectedViewer.size()) {
        for (int y = actualViewer.height() / 4; y < actualViewer.height() * .72; y += 8) {
            for (int x = actualViewer.width() / 5; x < actualViewer.width() * .8; x += 8) {
                const auto actualColor = actualViewer.pixelColor(x, y);
                const auto expectedColor = expectedViewer.pixelColor(x, y);
                const int difference = qAbs(actualColor.red() - expectedColor.red()) +
                                       qAbs(actualColor.green() - expectedColor.green()) +
                                       qAbs(actualColor.blue() - expectedColor.blue());
                matchingPixels += difference <= 24;
                ++sampledPixels;
            }
        }
    }
    if (!sampledPixels || matchingPixels <= sampledPixels * .95) {
        qWarning() << "Viewer GPU comparison:" << actualViewer.size() << expectedViewer.size()
                   << matchingPixels << sampledPixels;
        if (!output.isEmpty()) {
            actualViewer.save(output + "/viewer-actual.png");
            expectedViewer.save(output + "/viewer-expected.png");
        }
    }
    check(sampledPixels > 0 && matchingPixels > sampledPixels * .95,
          "viewer samples current scene texture after archive transition");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/viewer-front.png");
    }
    evaluate("App.runtime.coverBack();");
    refresh();
    check(evaluate("App.hud.frame.eye[2]<0").toBool(), "disc rear captured from back camera");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/disc-back.png");
    }
    evaluate("App.runtime.viewerAction('close');");
    refresh();
    // 本地切歌验证：详情、唱片身份和原插回动画同步；不读取真实账号音源。
    evaluate(R"(
        App.hud.modal = null;
        App.runtime.reduced(false);
        var followSongs = [501, 502].map(function (id) {
            var v = App.parse(
                {
                    id: id,
                    name: '同步切歌 ' + id,
                    ar: [
                        {
                            id: 10,
                            name: '测试歌手',
                        },
                    ],
                    al: {
                        id: 7,
                        name: '测试专辑',
                    },
                    dt: 180000,
                },
                'song',
            );
            v.wiki = '本地验证';
            App.music.details[v.key] = v;
            return v;
        });
        App.music.player.queue = followSongs;
        App.music.selectedPhysical = 0;
        App.music.selectedLane = 0;
        App.music.player.current = followSongs[0];
        App.music.followPlayback(followSongs[0]);
    )");
    for (int i = 1; i <= 240; ++i) {
        backend.step(50 + i / 60.);
    }
    evaluate("App.music.player.current=followSongs[1];App.music.followPlayback(followSongs[1]);");
    backend.step(54.01);
    check(
        evaluate("App.music.current.id==='502'&&App.music.directory.index===1&&App.hud.frame.mode==='detail'")
            .toBool(),
        "song change synchronizes upper detail and archive selection");
    check(evaluate("App.hud.frame.stats.returningFiles>0").toBool(), "song change returns the outgoing disc");
    for (int i = 1; i <= 240; ++i) {
        backend.step(54.01 + i / 60.);
    }
    settle();
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/song-switch.png");
    }
    evaluate("App.music.directory=null;App.music.player.current=null;App.runtime.reduced(true);");
    // 即使底层保留详情状态，四类检索也只能显示检索面板，不能叠出资料页。
    for (const auto &kind : {QString("album"), QString("playlist"), QString("artist")}) {
        evaluate("App.hud.openModal('search');App.music.searchKind='" + kind + "';App.music.current={kind:'" +
                 kind +
                 "',id:'8',title:'本地分类检索',artist:'测试歌手'};"
                 "App.music.lists[0]=uiSongs.slice(0,6).map(function(v){return Object.assign({},v,{kind:'" +
                 kind + "'});});");
        refresh();
        check(evaluate("!App.hud.hits.some(function(h){return h.action==='music:play-loaded'||"
                       "h.action.indexOf('music:hint:')===0;})")
                  .toBool(),
              "search surface excludes background directory and suggestions");
        if (!output.isEmpty()) {
            window->grabWindow().save(output + "/search-" + kind + ".png");
        }
    }
    evaluate(R"(
        App.hud.openModal('search');
        App.hud.action('music:group:1');
        App.music.lists[0] = uiSongs.map(function (song, i) {
            return Object.assign({}, song, {
                artists: [
                    {
                        id: String(Math.floor(i / 10) + 10),
                        name: '测试歌手 ' + Math.floor(i / 10),
                    },
                ],
            });
        });
    )");
    refresh();
    check(click("music:expand:artist%3A10"), "artist group row opens with native click");
    check(evaluate("App.hud.activeGroup==='artist:10' && App.hud.rows(0).length===10").toBool(),
          "group expands to original song indices");
    check(click("music:group-back"), "artist group return button");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/artist-groups.png");
    }
    // 静音 PCM 文件验证真实系统解码，不访问网络也不打扰用户。
    QTemporaryDir audioFixture;
    QFile audioFile(audioFixture.filePath("silent.wav"));
    check(audioFile.open(QIODevice::WriteOnly), "local audio fixture file");
    QDataStream wav(&audioFile);
    wav.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 bytes = 8000 * 2 * 4;
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + bytes);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2)
        << quint16(16);
    wav.writeRawData("data", 4);
    wav << bytes;
    wav.writeRawData(QByteArray(bytes, '\0').constData(), bytes);
    audioFile.close();
    backend.host.mediaControl("volume", 0);
    check(backend.host.mediaLoad(QUrl::fromLocalFile(audioFile.fileName()).toString()),
          "local audio source accepted");
    wait.restart();
    while (backend.host.mediaState().value("duration").toLongLong() == 0 && wait.elapsed() < 5000) {
        settle();
    }
    const auto decoded = backend.host.mediaState();
    check(decoded.value("duration").toLongLong() >= 3900 && decoded.value("error").toString().isEmpty(),
          "native PCM decode duration");
    backend.host.mediaControl("pause");
    settle();
    check(!backend.host.mediaState().value("playing").toBool(), "native pause");
    backend.host.mediaControl("seek", 1500);
    // 系统音频后端异步完成跳转；等待目标位置，不能用固定的一帧延迟判断。
    wait.restart();
    while (qAbs(backend.host.mediaState().value("position").toLongLong() - 1500) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    const auto seekState = backend.host.mediaState();
    if (qAbs(seekState.value("position").toLongLong() - 1500) >= 100) {
        qWarning() << "Native seek state:" << seekState;
    }
    check(qAbs(seekState.value("position").toLongLong() - 1500) < 100, "native seek");
    backend.host.mediaControl("volume", .4);
    check(qAbs(backend.host.mediaState().value("volume").toDouble() - .4) < .01, "native volume");
    evaluate(R"(
        App.music.player.current = uiSongs[0];
        App.music.player.queue = uiSongs.slice(0, 10);
        App.music.player.index = 0;
        App.music.player.lastPoll = -1;
        App.hud.openModal('player');
    )");
    refresh();
    check(click("player:mode"), "playback mode native click");
    check(evaluate("App.music.player.mode==='one'").toBool(), "playback mode changes");
    check(click("player:remove:1"), "queue removal native click");
    check(evaluate("App.music.player.queue.length===9").toBool(), "queue removal changes data");
    auto dragControl = [&](const QString &name, double fraction) {
        const auto bounds = evaluate("App.hud." + name).toVariant().toMap();
        const QPointF start((bounds.value("x").toDouble() + bounds.value("width").toDouble() * .2) * scale,
                            (bounds.value("y").toDouble() + bounds.value("height").toDouble() / 2) * scale);
        const QPointF end(
            (bounds.value("x").toDouble() + bounds.value("width").toDouble() * fraction) * scale, start.y());
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const auto position = type == QEvent::MouseButtonPress ? start : end;
            QMouseEvent event(type, position, position, window->mapToGlobal(position),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(window, &event);
            settle();
        }
    };
    dragControl("playerSeekRect", .5);
    wait.restart();
    while (qAbs(backend.host.mediaState().value("position").toLongLong() - 2000) >= 100 &&
           wait.elapsed() < 3000) {
        settle();
    }
    check(qAbs(backend.host.mediaState().value("position").toLongLong() - 2000) < 100,
          "QML seek drag reaches native player");
    dragControl("playerVolumeRect", .6);
    check(qAbs(backend.host.mediaState().value("volume").toDouble() - .6) < .01,
          "QML volume drag reaches native output");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/player.png");
    }
    // 不同字号的文字与音量条必须以真实可见轮廓对齐，而非文本框名义高度。
    check(evaluate(R"(
        (function () {
            var cy = 984 + 42;
            var labels = ['单曲', '音量', '队列'];
            return labels.every(function (label) {
                var c = App.hud.commands.find(function (c) {
                    return c[0] === 'text' && c[1] === label;
                });
                if (!c) {
                    return false;
                }
                var b = Glyphs.bounds(c[1], c[4], c[5]);
                return Math.abs(c[3] + b.y + b.height / 2 - cy) < 0.01;
            });
        })();
    )")
              .toBool(),
          "playback text visible bounds share the icon centerline");
    backend.host.mediaControl("stop");
    evaluate("App.music.player.stop();App.hud.openModal('discover');App.music.discover.items=uiSongs.slice(0,"
             "10);App.music.discover.status='本地发现页面验收';");
    refresh();
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/discover.png");
    }
    evaluate("App.hud.openModal('login');App.music.loginMethod='email';");
    refresh();
    auto accountField = window->findChild<QQuickItem *>("accountField");
    auto passwordField = window->findChild<QQuickItem *>("passwordField");
    check(accountField && accountField->isVisible() && passwordField && passwordField->isVisible() &&
              passwordField->property("echoMode").toInt() == 2,
          "native login form and masked password");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/login.png");
    }
    evaluate("App.hud.openModal('settings');");
    refresh();
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/settings.png");
    }
    evaluate("App.music.switchProvider('qq');App.hud.openModal('discover');"
             "App.music.discover.items=uiSongs.slice(0,6).map(function(s){return Object.assign({},s,{"
             "provider:'qq',key:'qq:song:'+s.id});});App.music.discover.status='QQ 本地推荐验收';");
    refresh();
    check(evaluate("App.hud.hits.some(function(h){return h.label==='首页推荐';}) && "
                   "App.hud.hits.some(function(h){return h.label==='榜单列表';}) && "
                   "!App.hud.hits.some(function(h){return /云盘|FM|电台/.test(h.label);})")
              .toBool(),
          "QQ discovery mapping and core-only navigation");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/qq-discover.png");
    }
    evaluate("App.hud.openModal('login');App.music.loginMethod='wechat';");
    refresh();
    check(!accountField->isVisible() && !passwordField->isVisible(), "QQ QR hides password form");
    if (!output.isEmpty()) {
        window->grabWindow().save(output + "/qq-login.png");
    }
    const auto previousVisibility = window->visibility();
    backend.action("window:fullscreen");
    wait.restart();
    while (window->visibility() != QWindow::FullScreen && wait.elapsed() < 10000) {
        settle();
    }
    check(window->visibility() == QWindow::FullScreen, "settings enters native fullscreen");
    backend.action("window:fullscreen");
    wait.restart();
    while (window->visibility() != previousVisibility && wait.elapsed() < 10000) {
        settle();
    }
    check(window->visibility() == previousVisibility, "fullscreen restores previous window mode");
    window->resize(originalWindowSize);
    refresh();
    // 待机验收使用真实定时器，不手工推进帧；静止后输入、后台数据和窗口恢复必须唤醒。
    backend.host.mediaControl("stop");
    evaluate("App.music.player.stop();App.hud.modal=null;App.hud.rolling={};App.hud.resultStart=-100;App.hud."
             "motion.set(true,0,true);");
    check(backend.scene.review("archive"), "idle scene readiness");
    backend.scene.setMode("archive");
    backend.wake();
    wait.restart();
    while (!backend.performance().value("idle").toBool() && wait.elapsed() < 15000) {
        settle();
    }
    qInfo() << "Idle scheduler:" << backend.performance();
    check(backend.performance().value("idle").toBool(), "live scene converges into idle");
    settle();
    const auto idleRenderFrames = backend.performance().value("renderFrames").toULongLong();
    const auto idleSteps = backend.performance().value("steps").toULongLong();
    for (int i = 0; i < 8; ++i) {
        settle();
    }
    check(backend.performance().value("steps").toULongLong() == idleSteps, "idle performs zero visual steps");
    check(backend.performance().value("renderFrames").toULongLong() == idleRenderFrames,
          "idle performs zero GPU scene renders");
    // HUD 绘制过程中产生主题等场景变化，必须继续下一帧，不能带着失效标记入睡。
    evaluate("var idleDraw=App.hud.draw;App.hud.draw=function(){var result=idleDraw.apply(this,arguments);"
             "App.hud.draw=idleDraw;App.runtime.theme(true);return result;};");
    backend.host.invalidate();
    bool themeAdvanced = false;
    wait.restart();
    while (!themeAdvanced && wait.elapsed() < 3000) {
        settle();
        for (const auto &value : backend.frame()["draws"].toArray()) {
            themeAdvanced |= value.toObject()["theme"].toDouble() > .01;
        }
    }
    check(themeAdvanced, "late scene invalidation continues rendering after background wake");
    wait.restart();
    while (!backend.performance().value("idle").toBool() && wait.elapsed() < 15000) {
        settle();
    }
    bool themeFinished = false;
    for (const auto &value : backend.frame()["draws"].toArray()) {
        themeFinished |= value.toObject()["theme"].toDouble() > .99;
    }
    check(themeFinished && backend.performance().value("idle").toBool(),
          "background theme animation completes before returning to idle");
    evaluate("App.runtime.theme(false);");
    backend.pointer("pointermove", 20, 20);
    check(backend.performance().value("timerActive").toBool(), "mouse input wakes scheduler");
    settle();
    check(backend.performance().value("steps").toULongLong() > idleSteps, "mouse wake renders again");
    window->hide();
    settle();
    const auto hiddenSteps = backend.performance().value("steps").toULongLong();
    backend.host.invalidate();
    for (int i = 0; i < 4; ++i) {
        settle();
    }
    check(!backend.performance().value("timerActive").toBool() &&
              backend.performance().value("steps").toULongLong() == hiddenSteps,
          "hidden window stays asleep on background invalidation");
    window->show();
    settle();
    check(backend.performance().value("steps").toULongLong() > hiddenSteps,
          "restored window renders latest state");
    wait.restart();
    while (!backend.performance().value("idle").toBool() && wait.elapsed() < 15000) {
        settle();
    }
    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &keyEvent);
    check(backend.performance().value("timerActive").toBool(), "native keyboard wakes scheduler");
    // 搜索框仍持有焦点时也应待机，光标停止闪烁，避免仅光标就持续触发 Qt 合成。
    evaluate("App.hud.openModal('search');App.hud.resultStart=-100;");
    backend.wake();
    wait.restart();
    while (!backend.performance().value("idle").toBool() && wait.elapsed() < 15000) {
        settle();
    }
    check(backend.performance().value("idle").toBool() && !input->property("cursorVisible").toBool(),
          "idle search field pauses blinking cursor");
    QKeyEvent cursorWake(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &cursorWake);
    settle();
    check(backend.performance().value("timerActive").toBool() && input->property("cursorVisible").toBool(),
          "native text input restores cursor on keyboard wake");
    // A→B→A 重复命中时 GPU 对象数必须保持稳定，不能每次覆盖缓存而遗留旧纹理。
    backend.setVisible(false);
    evaluate("App.hud.modal=null;App.hud.rolling={};App.hud.resultStart=-100;");
    backend.scene.review("detail");
    backend.setVisible(true);
    auto cachedSelection = [&](int index) {
        evaluate(QString("App.music.current={key:'cache-%1',kind:'song',id:'%1',title:'缓存回选验收',artist:'"
                         "本地'};App.music.coverImage='fixture-disc';")
                     .arg(index));
        backend.step(70);
        settle();
    };
    cachedSelection(0);
    cachedSelection(1);
    cachedSelection(0);
    const auto warmedResources = backend.performance().value("gpuResources").toULongLong();
    for (int i = 0; i < 20; ++i) {
        cachedSelection(i % 2);
    }
    check(warmedResources > 0 && backend.performance().value("gpuResources").toULongLong() == warmedResources,
          "repeated cover cache hits keep GPU resource count stable");
    if (!output.isEmpty()) {
        QFile metrics(output + "/performance.json");
        if (metrics.open(QIODevice::WriteOnly)) {
            metrics.write(QJsonDocument::fromVariant(backend.performance()).toJson());
        }
    }
    if (ok) {
        qInfo() << "Native QML scrolling, paging and search input checks passed";
    }
    return ok ? 0 : 1;
}
