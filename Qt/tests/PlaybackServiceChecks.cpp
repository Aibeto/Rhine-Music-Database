#include "PlaybackConnection.h"
#include "DesktopPlaybackControls.h"
#include "Engine.h"
#include "render/PresentationShell.h"
#include "DesktopVisibility.h"
#include <QAudioDevice>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QMediaDevices>
#include <QProcess>
#include <QQuickItem>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <memory>

bool runWindowTransitionChecks();
bool runEngineResumeChecks(const QString &resources);

namespace {
bool waitFor(const std::function<bool()> &ready, int timeoutMs = 10000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return ready();
}

/** 静音 PCM 避免测试打扰用户，真实时长与 EOF 仍经过 FFmpeg 和输出设备。 */
bool writeWave(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    // 给真实桌面动画和前台切换留足时间，
    // 不能在 EOF 用例开始前自然播完整个队列。
    constexpr int sampleBytes = 8000 * 2 * 120;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + sampleBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2)
           << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(sampleBytes);
    return file.write(QByteArray(sampleBytes, '\0')) == sampleBytes;
}
}

/** 调用实际窗口按钮的 QML activate()，覆盖信号回调仍在栈上的最小化/关闭路径。 */
bool triggerWindowChromeAction(QQuickItem *item, const QString &action) {
    if (item->objectName() == QStringLiteral("windowButton_") + action) {
        return QMetaObject::invokeMethod(item, "activate", Qt::DirectConnection);
    }
    for (auto child : item->childItems()) {
        if (triggerWindowChromeAction(child, action)) {
            return true;
        }
    }
    return false;
}

/** 重建后从真实宿主窗口点击搜索并输入文字，覆盖只恢复图片却丢失输入层的回归。 */
void verifyRestoredWindowInput(PresentationShell *shell, Engine *engine) {
    QTimer::singleShot(500, engine, [shell, engine] {
        if (shell->suspended() || !shell->inputWindow()) {
            return;
        }
        for (const auto &entry : engine->hits()) {
            const auto hit = entry.toMap();
            if (hit["action"] != "search") {
                continue;
            }
            const auto scale = shell->window()->height() / 1080.0;
            const QPointF point((hit["x"].toDouble() + hit["width"].toDouble() / 2) * scale,
                                (hit["y"].toDouble() + hit["height"].toDouble() / 2) * scale);
            for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QMouseEvent event(type, point, shell->window()->mapToGlobal(point), Qt::LeftButton,
                                  type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                                  Qt::NoModifier);
                QCoreApplication::sendEvent(shell->window(), &event);
            }
            break;
        }
        QTimer::singleShot(500, engine, [shell, engine] {
            if (shell->suspended() || !shell->inputWindow()) {
                return;
            }
            const auto before = engine->app->hud.query;
            QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
            QCoreApplication::sendEvent(shell->window(), &key);
            QInputMethodEvent commit;
            commit.setCommitString(QStringLiteral("测试"));
            QCoreApplication::sendEvent(shell->window(), &commit);
            const bool passed = engine->app->hud.modal == "search" &&
                                engine->app->hud.query == before + QStringLiteral("a测试");
            qInfo() << "PLAYBACK_GUI_RESTORED_INPUT" << passed;
        });
    });
}

/**
 * 用真实服务与 GUI 进程验证生命周期；
 * 所有数据和套接字均限定在本轮临时目录。
 */
bool runPlaybackServiceChecks(const QString &sourceResources, const QString &executable, const QString &gui) {
    if (!runWindowTransitionChecks()) {
        return false;
    }
    if (!runEngineResumeChecks(sourceResources)) {
        return false;
    }
    auto desktopFixture = qEnvironmentVariable("RHINE_DESKTOP_FIXTURE");
    if (desktopFixture.isEmpty()) {
#ifdef Q_OS_WIN
        desktopFixture = QDir(QFileInfo(gui).absolutePath()).filePath("RhineDesktopFixture.exe");
#else
        desktopFixture =
            QDir(QFileInfo(gui).absolutePath())
                .absoluteFilePath("../../../RhineDesktopFixture.app/Contents/MacOS/RhineDesktopFixture");
#endif
    }
    if ((qEnvironmentVariable("QT_QPA_PLATFORM") == "cocoa" ||
         qEnvironmentVariable("QT_QPA_PLATFORM") == "windows") &&
        !QFileInfo::exists(desktopFixture)) {
        qCritical() << "Native desktop verification requires RHINE_DESKTOP_FIXTURE:" << desktopFixture;
        return false;
    }
    QTemporaryDir directory;
    if (!directory.isValid() || !writeWave(directory.path() + "/silence.wav")) {
        return false;
    }
    // 恢复与跨进程夹具只读取自身临时资源，不依赖终端的用户目录访问身份。
    const auto resources = directory.path() + "/resources";
    QDir().mkpath(resources);
    for (const auto &file : QDir(sourceResources).entryInfoList(QDir::Files)) {
        if (!QFile::copy(file.absoluteFilePath(), resources + '/' + file.fileName())) {
            return false;
        }
    }
    QDirIterator badges(sourceResources + "/quality", QDir::Files, QDirIterator::Subdirectories);
    while (badges.hasNext()) {
        const auto source = badges.next();
        const auto target =
            resources + "/quality/" + QDir(sourceResources + "/quality").relativeFilePath(source);
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (!QFile::copy(source, target)) {
            return false;
        }
    }
    bool ok = true;
    auto check = [&](bool condition, const char *message) {
        if (!condition) {
            qCritical() << "FAIL:" << message;
            ok = false;
        }
    };
    const auto scope = QDir(directory.path()).absoluteFilePath("data").toUtf8();
    auto endpoint =
        "rhine-playback-" +
        QString::fromLatin1(QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex().left(24));
#ifndef Q_OS_WIN
    endpoint.prepend("/tmp/");
#endif
    QProcess service;
    service.setProcessChannelMode(QProcess::MergedChannels);
    service.start(executable, {"--resources", resources, "--gui", gui, "--offline-test", directory.path()});
    if (!service.waitForStarted()) {
        qCritical() << service.errorString();
        return false;
    }
    auto connection = std::make_unique<PlaybackConnection>();
    if (!waitFor([&] { return connection->connectToService(endpoint, 50); })) {
        qCritical() << "Playback service did not listen:" << service.readAll();
        service.kill();
        service.waitForFinished();
        return false;
    }
    check(connection->call("hello")["protocol"] == PlaybackConnection::ProtocolVersion,
          "real helper accepts IPC handshake");
    const auto servicePid = connection->snapshot()["servicePid"].toInteger();
    check(servicePid == service.processId(), "playback owner is the separate helper process");
    connection->call("host.setProvider", {{"provider", "local"}});
    connection->call("player.volume", {{"value", 0}});
    connection->call("host.diagnostics", {{"enabled", true}});
    QJsonArray queue;
    for (int index = 0; index < 3; ++index) {
        queue.append(QJsonObject{{"kind", "song"},
                                 {"provider", "local"},
                                 {"id", QString::number(index)},
                                 {"title", "IPC fixture"},
                                 {"path", directory.path() + "/silence.wav"},
                                 {"url", QUrl::fromLocalFile(directory.path() + "/silence.wav").toString()}});
    }
    connection->call("player.play", {{"track", queue[0]}, {"source", queue}});
    connection->call("player.mode", {{"value", "off"}});
    const bool output = !QMediaDevices::defaultAudioOutput().isNull();
    if (output) {
        check(waitFor([&] {
                  connection->call("hello");
                  return connection->snapshot()["state"].toObject()["position"].toDouble() > 0;
              }),
              "service consumes real audio before GUI starts");
    }
    auto launchGui = [&](QProcess &process, const QStringList &extra) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("RHINE_RESOURCES", resources);
#ifdef Q_OS_MACOS
        // 测试控制器保持辅助身份，正常 GUI 仍沿用用户启动时的应用身份。
        environment.remove("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM");
#endif
        // 无显示服务器的默认测试不需要图形设备；真实 Metal 验收另外运行。
        if (environment.value("QT_QPA_PLATFORM") == "offscreen") {
            environment.insert("QT_QUICK_BACKEND", "software");
        }
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(gui, QStringList{"--playback-gui-test", directory.path()} + extra);
    };
    const auto launchDesktop = [&](QProcess &process, const QString &mode) {
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(desktopFixture, {mode});
    };
    const QJsonValue sourceSerial = connection->snapshot()["diagnostics"].toObject()["sourceSerial"];
    QProcess panel;
    launchGui(panel, {});
    QByteArray panelLog;
    check(panel.waitForStarted() && waitFor([&] {
              panelLog += panel.readAll();
              return panelLog.contains("PLAYBACK_GUI_READY");
          }),
          "real persistent GUI starts");
    const auto inspect = [&] {
        connection->call("hello");
        return connection->snapshot()["interfaceRendering"].toObject();
    };
    const auto waitRendering = [&](bool suspended) {
        return waitFor([&] {
            const auto report = inspect();
            return report["suspended"].toBool() == suspended && report["liveContent"].toBool() != suspended &&
                   (!suspended ||
                    (!report["timerActive"].toBool() && report["gpuResources"].toDouble() == 0));
        });
    };
    // 静态页挂起只在受控呈现层接管的平台上存在；离屏/回退路径跳过窗口验收，
    // 服务端音频、队列与进程生命周期仍按同一套用例验证。
    const bool shellActive = waitFor([&] { return inspect()["shell"].toBool(); }, 3000);
    if (!shellActive) {
        qWarning("PresentationShell 未接管：本平台跳过窗口挂起与遮挡验收");
    }
    if (shellActive) {
        check(waitFor([&] { return !inspect()["windowId"].toString().isEmpty(); }),
              "persistent window publishes its native identity");
        const QJsonValue windowId = inspect()["windowId"];
        const auto panelPid = panel.processId();
        const auto verifyIdentity = [&] {
            check(panel.state() == QProcess::Running &&
                      connection->snapshot()["interfacePid"].toInteger() == panelPid &&
                      inspect()["windowId"] == windowId,
                  "suspension and restoration preserve both process and native window handle");
            check(connection->snapshot()["diagnostics"].toObject()["sourceSerial"] == sourceSerial,
                  "render lifecycle never recreates the audio source");
        };
        QProcess duplicate;
        launchGui(duplicate, {});
        check(duplicate.waitForStarted() && duplicate.waitForFinished(10000) && duplicate.exitCode() == 0,
              "second launch activates the original window");
        if (duplicate.state() != QProcess::NotRunning) {
            duplicate.kill();
            duplicate.waitForFinished();
        }
        for (int cycle = 0; cycle < 3; ++cycle) {
            connection->call("test.coverage", {{"covered", true}});
            check(waitRendering(true), "coverage unloads the real render subtree and stops visual timers");
            if (cycle == 0) {
                // 静态页至少要真正上屏一帧，否则窗口停在挂起前的旧内容。
                check(waitFor([&] { return inspect()["windowFrames"].toDouble() >= 1.0; }),
                      "suspended window still paints the static page frame");
            }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
            check(!inspect()["graphicsDevice"].toBool() && inspect()["nativeStaticImage"].toBool(),
                  "native static page releases the entire graphics device");
#endif
            const auto before = inspect();
            QElapsedTimer idle;
            idle.start();
            waitFor([&] { return idle.elapsed() >= 1800; }, 2500);
            const auto after = inspect();
            check(before["steps"] == after["steps"] && before["renderFrames"] == after["renderFrames"],
                  "static background page performs no scene simulation or 3D rendering");
            verifyIdentity();
            connection->call("test.coverage", {{"covered", false}});
            check(waitRendering(false), "uncovering reloads content in the same original window");
            verifyIdentity();
        }
        struct PresenceCase {
            const char *name;
            QJsonObject suspended;
            bool requiresUser;
        };
        const QList<PresenceCase> presenceCases{
            {"other Space", {{"workspaceActive", false}}, false},
            {"inactive Stage Manager set", {{"stagedAway", true}, {"automaticRestoreAllowed", false}}, true},
            {"covered Stage Manager set", {{"covered", true}, {"automaticRestoreAllowed", false}}, true}};
        for (const auto &scenario : presenceCases) {
            connection->call("test.presence", scenario.suspended);
            check(waitRendering(true), "desktop or stage transition unloads render content");
            verifyIdentity();
            connection->call("test.presence");
            if (scenario.requiresUser) {
                check(connection->snapshot()["interfaceSuspended"].toBool(),
                      "Stage Manager does not force a window activation while switching sets");
                connection->call("show");
            }
            check(waitRendering(false), "desktop return or explicit opening restores the original content");
            verifyIdentity();
            qInfo() << "Desktop lifecycle verified:" << scenario.name;
        }
        connection->call("test.closeWindow");
        check(waitRendering(true), "close-to-menu hides the existing window and unloads render content");
        connection->call("test.coverage", {{"covered", true}});
        connection->call("test.coverage", {{"covered", false}});
        check(!connection->snapshot()["interfaceWanted"].toBool(), "manual close prevents passive reopening");
        connection->call("show");
        check(waitRendering(false), "menu action restores the hidden original window");
        verifyIdentity();
        if (qEnvironmentVariable("QT_QPA_PLATFORM") == "cocoa") {
            const auto nativeIdentity =
                connection->call("test.nativeCoverage")["backgroundIdentity"].toObject();
            check(nativeIdentity["identifier"].toString().endsWith(".playback") &&
                      nativeIdentity["agent"].toBool() && nativeIdentity["accessory"].toBool(),
                  "playback helper retains its separate Dock-hidden identity");
        }
        if (qEnvironmentVariable("QT_QPA_PLATFORM") == "cocoa" ||
            qEnvironmentVariable("QT_QPA_PLATFORM") == "windows") {
            for (const auto &mode : {"normal", "maximized", "fullscreen", "maximized-with-normal"}) {
                connection->call("test.presence");
                connection->call("show");
                check(waitRendering(false), "native classification starts with live content");
                QProcess cover;
                launchDesktop(cover, QString::fromLatin1(mode));
                QByteArray coverLog;
                check(cover.waitForStarted() && waitFor([&] {
                          coverLog += cover.readAll();
                          return coverLog.contains("COVERAGE_FIXTURE_READY");
                      }),
                      "native external window fixture starts");
                const bool shouldCover = QString::fromLatin1(mode) != "normal";
                QJsonObject native;
                check(waitFor([&] {
                          native = connection->call("test.nativeCoverage");
                          return native["foregroundPid"].toInteger() == cover.processId() &&
                                 native["covered"].toBool() == shouldCover;
                      }),
                      "native z-order and monitor coverage classification");
                if (native["covered"].toBool() != shouldCover) {
                    qCritical() << mode << native;
                }
                if (shouldCover) {
                    connection->call("test.presence", {{"native", true}});
                    check(waitRendering(true), "native maximize or fullscreen unloads rendering");
                    verifyIdentity();
                }
                cover.kill();
                cover.waitForFinished();
                connection->call("test.presence");
                connection->call("show");
                check(waitRendering(false), "native overlay removal restores content");
                verifyIdentity();
                qInfo() << "Native desktop coverage verified:" << mode;
            }
        }
    }
    // 模拟 GUI 崩溃仍只结束本轮启动的界面，服务和音频应继续。
    panel.kill();
    panel.waitForFinished();
    check(waitFor([&] {
              connection->call("hello");
              return !connection->snapshot()["interfaceConnected"].toBool();
          }),
          "audio service observes GUI crash without exiting");
    connection->call("player.pause", {{"paused", true}});
    check(!connection->snapshot()["state"].toObject()["requested"].toBool(),
          "pause remains available after GUI crash");
    connection->call("player.pause", {{"paused", false}});
    for (const auto &action : {"--playback-test-minimize", "--playback-test-close"}) {
        if (!shellActive) {
            qWarning("PresentationShell 未接管：跳过关闭/最小化释放验收");
            break;
        }
        QProcess retained;
        launchGui(retained, {QString::fromLatin1(action)});
        QByteArray retainedLog;
        check(retained.waitForStarted() && waitFor([&] {
                  retainedLog += retained.readAll();
                  return retainedLog.contains("PLAYBACK_GUI_RELEASE_REQUESTED");
              }),
              "real close or minimize action is delivered");
        QElapsedTimer immediate;
        immediate.start();
        check(waitRendering(true), "manual action immediately unloads rendering");
        check(immediate.elapsed() < 1000, "manual release has no two-second retention");
        const QJsonValue originalId = inspect()["windowId"];
        connection->call("show");
        check(waitRendering(false) && inspect()["windowId"] == originalId &&
                  retained.state() == QProcess::Running,
              "minimized or closed window restores using the same native handle");
        check(waitFor([&] {
                  retainedLog += retained.readAll();
                  return retainedLog.contains("PLAYBACK_GUI_RESTORED_INPUT true");
              }),
              "restored window accepts real mouse, keyboard and input-method events");
        retained.kill();
        retained.waitForFinished();
        check(waitFor([&] {
                  connection->call("hello");
                  return !connection->snapshot()["interfaceConnected"].toBool();
              }),
              "test window cleanup is observed");
    }
    if (output) {
        // 这里没有存活的 GUI 引擎，控制模型仍应直接驱动服务。
        DesktopPlaybackControls controls(resources, connection.get());
        check(waitFor([&] { return controls.state()["canPlay"].toBool(); }),
              "persistent taskbar model remains enabled without the GUI engine");
        controls.trigger("toggle");
        check(waitFor([&] { return !connection->snapshot()["state"].toObject()["requested"].toBool(); }),
              "taskbar pause reaches the service while rendering is absent");
        controls.trigger("toggle");
        check(waitFor([&] { return connection->snapshot()["state"].toObject()["requested"].toBool(); }),
              "taskbar resume reaches the service while rendering is absent");
        controls.trigger("next");
        check(waitFor([&] { return connection->snapshot()["current"].toObject()["id"] == "1"; }),
              "taskbar next reaches the service without creating an engine");
        controls.trigger("prev");
        check(waitFor([&] { return connection->snapshot()["current"].toObject()["id"] == "0"; }),
              "taskbar previous reaches the service without creating an engine");
        check(!controls.state()["canLike"].toBool(), "local track never exposes an online mutation");
    }
    if (output) {
        check(waitFor([&] {
                  const auto snapshot = connection->snapshot();
                  return !snapshot["loading"].toBool() &&
                         snapshot["state"].toObject()["position"].toDouble() > 0;
              }),
              "taskbar track switch finishes loading before the EOF seek");
        connection->call("player.seek", {{"value", .99}});
        connection.reset();
        QElapsedTimer elapsed;
        elapsed.start();
        waitFor([&] { return elapsed.elapsed() >= 1800; }, 2500);
        connection = std::make_unique<PlaybackConnection>();
        check(connection->connectToService(endpoint), "service survives with no GUI or IPC clients");
        check(waitFor([&] {
                  connection->call("hello");
                  return connection->snapshot()["current"].toObject()["id"] == "1";
              }),
              "EOF advances the queue while every GUI client is absent");
        connection->call("host.diagnostics", {{"enabled", true}});
        check(waitFor([&] {
                  connection->call("hello");
                  return connection->snapshot()["diagnostics"].toObject()["decodedFrames"].toDouble() > 0;
              }),
              "background playback has decoded PCM evidence");
    } else {
        qWarning("No audio device: background PCM consumption and real EOF remain unverified");
        connection->call("player.next", {{"direction", 1}});
        check(connection->snapshot()["current"].toObject()["id"] == "1",
              "service owns queue advancement without an audio device");
    }
    QProcess exitInterface;
    launchGui(exitInterface, {"--playback-test-quit"});
    check(exitInterface.waitForStarted() && exitInterface.waitForFinished(10000) &&
              exitInterface.exitCode() == 0,
          "explicit GUI quit exits the interface");
    if (exitInterface.state() != QProcess::NotRunning) {
        exitInterface.kill();
        exitInterface.waitForFinished();
    }
    check(service.waitForFinished(10000) && service.exitCode() == 0,
          "explicit quit stops helper and playback");
    if (service.state() != QProcess::NotRunning) {
        service.kill();
        service.waitForFinished();
    }
    if (!ok) {
        qCritical().noquote() << service.readAll();
    }
    qInfo() << "Independent playback service and persistent window:" << ok;
    return ok;
}
