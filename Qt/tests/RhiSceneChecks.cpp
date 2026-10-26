#include "Engine.h"
#include "RhiScene.h"
#include "fsr/FsrSharpness.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QDebug>

#ifdef Q_OS_MACOS
bool runScenePostProcessChecks();
#endif

namespace {
void settle(int milliseconds = 200) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
}
}

/** 验证唯一渲染入口、离线读回和窗口恢复，避免旧开关重新引入双管线。 */
bool runRhiSceneChecks(Engine &backend, QQuickWindow *window, const QString &) {
    auto &hud = backend.app->hud;
    auto &music = backend.app->music;
    bool ok = true;
    int testCount = 0;

    const auto check = [&](bool value, const char *name) {
        testCount++;
        if (!value) {
            qCritical() << "RhiScene test failed:" << name;
            ok = false;
        }
    };

    const auto refresh = [&] {
        music.selectedPhysical = backend.scene.selection();
        backend.step(90);
        settle();
    };

    auto *scene = window->findChild<RhiScene *>("rhiScene");
    check(scene != nullptr, "single scene renderer exists");
    check(window->findChildren<RhiScene *>().size() == 1, "exactly one scene renderer");
    check(!window->findChild<QQuickItem *>("sceneEnhancementLoader"), "no enhancement loader");
    if (!scene) {
        return false;
    }
    // 旧偏好和已移除的动作不能切回另一条渲染管线。
    backend.action("scene-enhancement");
    refresh();
    check(scene->engine() == &backend && scene->isVisible(), "legacy action cannot disable renderer");
    backend.scene.setMode("archive");
    refresh();
    check(!window->grabWindow().isNull(), "archive renders");
    backend.scene.setMode("detail");
    refresh();
    check(!window->grabWindow().isNull(), "detail renders without another scene");
    const bool initialDark = hud.dark;
    hud.dark = !initialDark;
    refresh();
    check(scene->isVisible(), "scene survives theme change");
    hud.dark = initialDark;
    refresh();
    const auto before = window->grabWindow();
    window->hide();
    settle();
    window->show();
    settle();
    refresh();
    check(!window->grabWindow().isNull(), "scene restores after hide/show");
    // 离线截图入口必须使用同一烘焙渲染器，而非残留的旧版材质路径。
    const auto rendered = renderReference(backend.root(), backend.frame(), backend.current(), backend.cover(),
                                          before.size(), backend.drawBuffers());
    check(!rendered.isNull() && rendered.size() == before.size(), "reference renderer migrated");
    check(!backend.frame().contains("originalActorBlend"), "frame has no original actor dependency");

    // === 测试 11: FSR 锐化随放大倍数收敛 ===
    // 用户设置页显示的是原始百分比，运行时按 1/√放大倍数 衰减，压掉暗色封面上的边缘亮环。
    // 4 倍放大（316 → 1262）时 80% 收敛到 40%，3 倍（421 → 1262）收敛到 46%。
    check(RhineFsr::effectiveSharpnessPercent(80, QSize(316, 343), QSize(1262, 1370)) == 40,
          "sharpness converges to 40% at 4x upscale");
    check(RhineFsr::effectiveSharpnessPercent(80, QSize(421, 457), QSize(1262, 1370)) == 46,
          "sharpness converges to 46% at 3x upscale");
    check(RhineFsr::effectiveSharpnessPercent(80, QSize(1262, 1370), QSize(1262, 1370)) == 80,
          "sharpness unchanged at native scale");
    check(RhineFsr::effectiveSharpnessPercent(80, QSize(1262, 1370), QSize(631, 685)) == 80,
          "sharpness unchanged when the target is larger than the display");
    check(RhineFsr::effectiveSharpnessPercent(0, QSize(316, 343), QSize(1262, 1370)) == 0,
          "zero sharpness stays zero");

#ifdef Q_OS_MACOS
    ok &= runScenePostProcessChecks();
#endif
    qInfo() << "RhiScene core tests:" << testCount << (ok ? "passed" : "FAILED");
    return ok;
}
