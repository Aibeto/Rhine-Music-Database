#include "Engine.h"
#include "fsr/FsrSharpness.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QDebug>

/**
 * 场景增强核心功能测试
 *
 * 验证场景增强开关、状态管理和基础渲染逻辑，不依赖像素对比。
 * 视觉效果的详细验证应通过手动测试进行。
 */

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

bool runSceneEnhancementChecks(Engine &backend, QQuickWindow *window, const QString &) {
    auto &hud = backend.app->hud;
    auto &music = backend.app->music;
    bool ok = true;
    int testCount = 0;

    const auto check = [&](bool value, const char *name) {
        testCount++;
        if (!value) {
            qCritical() << "Scene enhancement test failed:" << name;
            ok = false;
        }
    };

    const auto refresh = [&] {
        music.selectedPhysical = backend.scene.selection();
        backend.step(90);
        settle();
    };

    // === 测试 1: 默认状态 ===
    NativeHud defaultHud(&backend.host, backend.glyphs(), &music, {});
    check(defaultHud.sceneEnhancement, "scene enhancement defaults to enabled");

    // === 测试 2: Loader 组件存在 ===
    auto *loader = window->findChild<QQuickItem *>("sceneEnhancementLoader");
    check(loader != nullptr, "scene enhancement loader exists");

    if (!loader) {
        qInfo() << "Scene enhancement core tests:" << testCount << "FAILED (loader not found)";
        return false;
    }

    // === 测试 3: 增强关闭时 Loader 不活动 ===
    if (hud.sceneEnhancement) {
        backend.action("scene-enhancement");
        refresh();
    }
    check(!hud.sceneEnhancement, "enhancement disabled");
    check(!loader->property("active").toBool(), "loader inactive when disabled");
    check(loader->property("item").value<QObject *>() == nullptr, "loader item null when disabled");

    // === 测试 4: 增强开启时 Loader 活动 ===
    backend.action("scene-enhancement");
    refresh();
    check(hud.sceneEnhancement, "enhancement enabled");
    check(loader->property("active").toBool(), "loader active when enabled");
    check(loader->property("item").value<QObject *>() != nullptr, "loader item created when enabled");

    // === 测试 5: 状态持久化 ===
    const bool currentState = hud.sceneEnhancement;
    backend.action("scene-enhancement");
    refresh();
    check(hud.sceneEnhancement != currentState, "enhancement state toggles");
    backend.action("scene-enhancement");
    refresh();
    check(hud.sceneEnhancement == currentState, "enhancement state restores");

    // === 测试 6: 不同场景模式下的增强 ===
    hud.sceneEnhancement = true;
    backend.scene.setMode("archive");
    refresh();
    check(loader->property("active").toBool(), "enhancement works in archive mode");

    backend.scene.setMode("detail");
    refresh();
    check(loader->property("active").toBool(), "enhancement works in detail mode");

    // === 测试 7: 主题切换兼容性 ===
    hud.sceneEnhancement = true;
    const bool initialDark = hud.dark;
    hud.dark = !initialDark;
    refresh();
    check(loader->property("active").toBool(), "enhancement survives theme change");
    hud.dark = initialDark;
    refresh();

    // === 测试 8: 窗口隐藏/显示后状态保持 ===
    hud.sceneEnhancement = true;
    window->hide();
    settle();
    window->show();
    settle();
    refresh();
    check(hud.sceneEnhancement, "enhancement state persists after hide/show");
    check(loader->property("active").toBool(), "loader remains active after hide/show");

    // === 测试 9: 增强关闭时资源释放 ===
    hud.sceneEnhancement = true;
    refresh();
    auto *item = loader->property("item").value<QObject *>();
    check(item != nullptr, "enhancement item exists before disable");

    backend.action("scene-enhancement");
    refresh();
    check(!hud.sceneEnhancement, "enhancement disabled for cleanup test");
    check(loader->property("item").value<QObject *>() == nullptr, "enhancement item released after disable");

    // === 测试 10: 渲染数据完整性 ===
    hud.sceneEnhancement = true;
    refresh();
    const auto frameData = backend.frame();
    check(!frameData.isEmpty(), "frame data available with enhancement");
    check(frameData.contains("stats"), "frame stats present with enhancement");

    // 恢复初始状态
    hud.sceneEnhancement = false;
    refresh();

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

    qInfo() << "Scene enhancement core tests:" << testCount << (ok ? "passed" : "FAILED");
    return ok;
}
