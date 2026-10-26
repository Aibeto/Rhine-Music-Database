#include "Engine.h"
#include "fg/FgFrameTap.h"
#include "RhiScene.h"
#include "render/PresentationShell.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QThread>
#include <QWindow>

namespace {
void settle(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}
quint64 counter(PresentationShell &shell, const char *name) {
    return shell.renderState().value(QLatin1String(name)).toULongLong();
}
double sceneDifference(const QImage &reference, const QImage &image) {
    if (reference.isNull() || image.size() != reference.size()) {
        return 255;
    }
    double difference = 0;
    int samples = 0;
    // 左侧唱片区域避开 HUD；场景丢失时不能让仍正常绘制的 UI 掩盖失败。
    for (int y = image.height() * 3 / 10; y < image.height() * 7 / 10; y += 8) {
        for (int x = image.width() / 5; x < image.width() / 2; x += 8) {
            const auto before = reference.pixelColor(x, y);
            const auto after = image.pixelColor(x, y);
            difference += qAbs(before.red() - after.red()) + qAbs(before.green() - after.green()) +
                          qAbs(before.blue() - after.blue());
            samples += 3;
        }
    }
    return samples ? difference / samples : 255;
}
} // namespace

/** 在真实呈现窗口验证按需调度、输入坐标、热切换和隐藏恢复，不访问音乐服务。 */
int runPresentationChecks(PresentationShell &shell, Engine &backend, const QString &directory) {
    QDir().mkpath(directory);
    QJsonArray checks;
    bool passed = true;
    const auto check = [&](bool result, const QString &name) {
        checks.append(QJsonObject{{"name", name}, {"passed", result}});
        passed &= result;
        qInfo().noquote() << "Presentation check:" << name << result;
    };
    const auto configure = [&](const QString &key, const QString &value) {
    // 通过正式配置通知链修改运行状态；离线模式不落盘用户偏好。
#ifdef Q_OS_MACOS
        backend.host.preferenceChanged(key, value);
#else
        const QHash<QString, QByteArray> variables{{"aa-mode", "RHINE_AA_MODE"},
                                                   {"render-scale", "RHINE_RENDER_SCALE"},
                                                   {"supersample", "RHINE_SUPERSAMPLE"}};
        qputenv(variables.value(key).constData(), value.toUtf8());
        backend.resetTemporalState();
#endif
        settle(400);
    };
#ifdef Q_OS_MACOS
    configure("fg", "off");
#endif
    configure("aa-mode", "off");
    configure("render-scale", "100");
    configure("supersample", "1");
    settle(1000);
    const auto initial = shell.grabFrame();
    check(!initial.isNull() && initial.save(directory + "/window.png"), "真实交换链的合成结果可读回");
    const auto rendered = counter(shell, "sceneFrames");
    const auto presented = counter(shell, "presented");
    settle(300);
    check(counter(shell, "presented") == presented && counter(shell, "sceneFrames") == rendered &&
              !shell.renderState().value("timerActive").toBool(),
          "静止时停止场景绘制与呈现定时器");
    const auto uiFrames = counter(shell, "overlayFrames");
    backend.hudUpdated();
    settle(150);
    check(counter(shell, "overlayFrames") > uiFrames && counter(shell, "sceneFrames") == rendered,
          "只更新 UI 时复用场景纹理");
    const auto uiAfter = counter(shell, "overlayFrames");
    backend.opticalSceneUpdated();
    settle(150);
    check(counter(shell, "sceneFrames") > rendered && counter(shell, "overlayFrames") == uiAfter,
          "只更新场景时复用 UI 纹理");

    bool settingsClicked = false;
    for (const auto &entry : backend.hits()) {
        const auto hit = entry.toMap();
        if (hit.value("action") != "settings") {
            continue;
        }
        const qreal scale = shell.window()->height() / 1080.0;
        const QPointF position((hit.value("x").toDouble() + hit.value("width").toDouble() / 2) * scale,
                               (hit.value("y").toDouble() + hit.value("height").toDouble() / 2) * scale);
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent click(type, position, shell.window()->mapToGlobal(position), Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(shell.window(), &click);
        }
        settle(250);
        settingsClicked = backend.app->hud.modal == "settings";
        break;
    }
    check(settingsClicked, "高分屏真实坐标点击打开设置");
    backend.action("close-modal");
    backend.pointer("pointerleave", 0, 0);
    settle(800);

    configure("aa-mode", "off");
    configure("render-scale", "50");
#ifdef Q_OS_MACOS
    auto actual = backend.renderTelemetry.snapshot();
    check(actual.value("renderWidth").toInt() * 2 == actual.value("displayWidth").toInt(),
          "增强场景实际按半分辨率渲染");
    configure("aa-mode", "metalfx");
    settle(800);
    actual = backend.renderTelemetry.snapshot();
    const auto scaled = shell.grabFrame();
    check(actual.value("implementation") == "metalfx-temporal", "实际执行 MetalFX 时域超分");
    check(sceneDifference(initial, scaled) < 10, "MetalFX 超分保留增强场景内容");
    check(backend.renderConfiguration.snapshot().temporal() && actual.value("samples").toInt() == 1,
          "MetalFX 使用时域输入而不创建 MSAA 附件");
    scaled.save(directory + "/metalfx-temporal-50.png");
    configure("render-scale", "100");
    check(backend.renderTelemetry.snapshot().value("implementation") == "metalfx-temporal",
          "原生分辨率下执行 MetalFX 时域抗锯齿");
    const auto native = shell.grabFrame();
    const auto fxSize = shell.window()->size();
    shell.window()->resize(1100, 660);
    settle(700);
    check(backend.renderTelemetry.snapshot().value("implementation") == "metalfx-temporal" &&
              !shell.grabFrame().isNull(),
          "窗口缩放后重建 MetalFX 时域上下文");
    shell.window()->resize(fxSize);
    settle(700);
    shell.window()->hide();
    settle(2300);
    shell.window()->show();
    settle(1000);
    check(backend.renderTelemetry.snapshot().value("implementation") == "metalfx-temporal" &&
              sceneDifference(native, shell.grabFrame()) < 1,
          "隐藏释放后恢复 MetalFX 上下文与场景内容");
    const auto temporalFrames = counter(shell, "sceneFrames");
    settle(500);
    check(counter(shell, "sceneFrames") == temporalFrames, "时域采样收束后停止场景绘制");
    configure("aa-mode", "off");
#endif
    configure("render-scale", "100");
#ifdef Q_OS_MACOS
    // 在时域累积中途关闭，不能留下阻止引擎休眠的计数。
    backend.host.preferenceChanged("aa-mode", "metalfx");
    settle(20);
    backend.host.preferenceChanged("aa-mode", "off");
    settle(1000);
    if (backend.performance().value("timerActive").toBool()) {
        qInfo() << "MetalFX close diagnostic" << backend.performance()
                << "mode" << backend.frame().value("mode") << "time" << backend.frame().value("time")
                << "pendingAnimations" << backend.frame().value("pendingAnimations");
    }
    check(!backend.performance().value("timerActive").toBool(), "中途关闭 MetalFX 后恢复引擎休眠");
#endif
    const auto restored = shell.grabFrame();
    check(restored == initial, "关闭后处理后恢复相同画面");
    const auto oldSize = shell.window()->size();
    shell.window()->resize(1100, 660);
    settle(500);
    check(shell.grabFrame().size() == QSize(qRound(1100 * shell.window()->devicePixelRatio()),
                                            qRound(660 * shell.window()->devicePixelRatio())),
          "改变窗口尺寸后同步离屏目标与交换链");
    shell.window()->resize(oldSize);
    settle(400);
    shell.window()->hide();
    settle(2300);
    const auto hiddenFrames = counter(shell, "presented");
    settle(200);
    check(counter(shell, "presented") == hiddenFrames && !backend.performance().value("visible").toBool(),
          "隐藏时引擎和呈现停止并释放场景图");
#ifdef Q_OS_MACOS
    check(backend.performance().value("gpuResources").toULongLong() == 0,
          "隐藏释放场景图后清空 GPU 资源计数");
#endif
    shell.window()->show();
    settle(800);
    check(!shell.grabFrame().isNull() && counter(shell, "presented") > hiddenFrames,
          "释放图形资源后恢复呈现");
    check(shell.grabFrame() == restored, "隐藏恢复后画面相同");
#ifdef Q_OS_MACOS
    configure("aa-mode", "metalfx");
    configure("render-scale", "50");
    configure("fg", "on");
    const auto before = RhineFg::stats().interpolated;
    for (int frame = 0; frame < 24; ++frame) {
        backend.key(frame % 2 ? "ArrowLeft" : "ArrowRight", false);
        settle(80);
    }
    settle(250);
    check(RhineFg::stats().interpolated > before, "MetalFX 生成的插值帧实际进入呈现计数");
    const auto inputs = backend.metalFxInputs;
    check(RhineFg::stats().depthReady && RhineFg::stats().motionVectorsReady && inputs.frameTimeMs > 0 &&
              inputs.frameTimeMs < 500,
          "MetalFX 帧生成使用当前深度、运动矢量及毫秒帧间隔");
    const auto generated = shell.grabFrame(true);
    check(sceneDifference(shell.grabFrame(), generated) < 20, "MetalFX 插值纹理包含有效场景内容");
    generated.save(directory + "/metalfx-generated.png");
    const auto inputSerial = backend.metalFxInputs.serial;
    configure("fg", "off");
    configure("aa-mode", "off");
    configure("fg", "on");
    check(backend.metalFxInputs.serial > inputSerial, "关闭后重新开启 MetalFX 时保持帧输入序号递增");
    configure("fg", "off");
#endif
    QFile report(directory + "/state.json");
    if (report.open(QIODevice::WriteOnly)) {
        auto state = QJsonObject::fromVariantMap(shell.renderState());
        state["checks"] = checks;
        state["passed"] = passed;
        report.write(QJsonDocument(state).toJson());
    }
    return passed ? 0 : 1;
}
