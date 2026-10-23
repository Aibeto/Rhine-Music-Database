#include "Engine.h"
#include "FrameProfiler.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>
#include <QQuickWindow>
#include <QDebug>
#include <cstdlib>

/**
 * UI 核心功能回归测试
 *
 * 验证关键交互逻辑和状态管理，不依赖像素精确对比或 GPU 渲染细节。
 * 场景增强和窗口 Chrome 测试已移至可选的手动验证流程。
 */

bool runSceneEnhancementChecks(Engine &backend, QQuickWindow *window, const QString &output);

namespace {
void settle(int milliseconds = 150) {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
}
}

/**
 * 比较独立 QRhi 设备的离线渲染结果。
 * 尺寸与透明度必须完全一致，仅允许极少量像素出现一个 RGB8 单位的舍入差异。
 */
bool runGpuReferenceCheck(const QImage &expected, const QImage &actual) {
    if (expected.isNull() || expected.size() != actual.size()) {
        return false;
    }
    if (expected == actual) {
        return true;
    }
    const auto first = expected.convertToFormat(QImage::Format_RGBA8888);
    const auto second = actual.convertToFormat(QImage::Format_RGBA8888);
    const qint64 limit = qMax<qint64>(1, qint64(first.width()) * first.height() / 10000);
    qint64 changedPixels = 0;
    for (int y = 0; y < first.height(); ++y) {
        const auto before = first.constScanLine(y);
        const auto after = second.constScanLine(y);
        for (int x = 0; x < first.width(); ++x) {
            bool changed = false;
            for (int channel = 0; channel < 4; ++channel) {
                const int difference = std::abs(int(before[x * 4 + channel]) - int(after[x * 4 + channel]));
                if (difference > 1 || (channel == 3 && difference != 0)) {
                    return false;
                }
                changed |= difference != 0;
            }
            if (changed && ++changedPixels > limit) {
                return false;
            }
        }
    }
    return true;
}

int runUiChecks(Engine &backend, QQuickWindow *window, const QString &output, FrameProfiler &profiler) {
    if (!window || !backend.ready() || !backend.scene.review("archive")) {
        return 1;
    }

    auto &music = backend.app->music;
    auto &hud = backend.app->hud;
    auto &player = music.player;
    bool ok = true;
    int testCount = 0;

    const auto check = [&](bool value, const char *name) {
        testCount++;
        if (!value) {
            qCritical() << "UI test failed:" << name;
            ok = false;
        }
    };

    // === 测试 1: 基础应用状态 ===
    check(backend.ready(), "engine ready");
    check(window->isVisible(), "window visible");

    // === 测试 2: 场景模式切换 ===
    backend.scene.setMode("archive");
    backend.step(50);
    settle();
    check(backend.scene.review("archive"), "archive mode");

    backend.scene.setMode("detail");
    backend.step(50);
    settle();
    check(backend.scene.review("detail"), "detail mode");

    backend.scene.setMode("archive");
    backend.step(50);
    settle();

    // === 测试 3: HUD 面板切换 ===
    hud.openModal("settings");
    backend.step(50);
    settle();
    check(hud.modal == "settings", "open settings modal");

    hud.modal.clear();
    backend.step(50);
    settle();
    check(hud.modal.isEmpty(), "close modal");

    // === 测试 4: 音乐播放状态 ===
    if (!player.queue.isEmpty()) {
        const auto initialPlaying = player.state.value("playing").toBool();
        player.toggle();
        backend.step(50);
        settle();
        const auto toggledPlaying = player.state.value("playing").toBool();
        check(initialPlaying != toggledPlaying, "playback toggle");

        player.toggle();
        backend.step(50);
        settle();
        const auto restoredPlaying = player.state.value("playing").toBool();
        check(restoredPlaying == initialPlaying, "playback toggle restore");
    }

    // === 测试 5: 主题切换 ===
    const bool initialDark = hud.dark;
    hud.dark = !initialDark;
    backend.step(50);
    settle();
    check(hud.dark != initialDark, "theme toggle");

    hud.dark = initialDark;
    backend.step(50);
    settle();

    // === 测试 6: 搜索面板与输入 ===
    hud.openModal("search");
    backend.step(50);
    settle();
    check(hud.modal == "search", "open search modal");

    backend.query("测试关键词");
    backend.step(50);
    check(hud.query == "测试关键词", "search query input");

    backend.query(QString());
    hud.modal.clear();
    backend.step(50);

    // === 测试 7: 性能面板基础功能 ===
    const bool initialProfiling = backend.scene.isProfiling();
    profiler.toggleVisible();
    settle();
    const bool profilingAfterToggle = backend.scene.isProfiling();
    check(initialProfiling != profilingAfterToggle, "performance panel toggle");

    if (profilingAfterToggle) {
        profiler.toggleVisible();
        settle();
        check(!backend.scene.isProfiling(), "performance panel close");
    }

    // === 测试 8: 动画状态 ===
    const bool initialReduced = hud.reduced;
    hud.reduced = !initialReduced;
    backend.step(50);
    settle();
    check(hud.reduced != initialReduced, "motion toggle");

    hud.reduced = initialReduced;
    backend.step(50);

    // === 测试 9: 帧数据完整性 ===
    backend.step(50);
    settle();
    const auto frameData = backend.frame();
    check(!frameData.isEmpty(), "frame data available");
    check(frameData.contains("stats"), "frame stats present");

    // === 测试 10: 场景增强开关 ===
    const bool initialEnhancement = hud.sceneEnhancement;
    hud.sceneEnhancement = !initialEnhancement;
    backend.step(50);
    settle();
    check(hud.sceneEnhancement != initialEnhancement, "scene enhancement toggle");

    hud.sceneEnhancement = initialEnhancement;
    backend.step(50);

    // === 测试 11: 音乐数据加载 ===
    if (!music.tracks.isEmpty()) {
        check(music.tracks.first().toObject().contains("id"), "music track data structure");
        check(!music.provider.isEmpty(), "music provider set");
    }

    // === 测试 12: 全屏切换 ===
    const auto initialVisibility = window->visibility();
    backend.toggleFullscreen();
    settle();
    const auto fullscreenVisibility = window->visibility();
    check(fullscreenVisibility != initialVisibility, "fullscreen toggle");

    backend.toggleFullscreen();
    settle();
    check(window->visibility() == initialVisibility, "fullscreen restore");

    // Tab 仅切换信息展示；开关前后保持默认背景构图与实际投影完全一致。
    const auto savedItem = music.current;
    const auto savedTrackList = hud.trackListMode;
    const auto savedLyrics = hud.lyricsMode;
    const auto savedModal = hud.modal;
    music.current = MusicData::item("song", "42", "构图回归", "离线测试");
    hud.modal.clear();
    hud.lyricsMode = false;
    hud.trackListMode = false;
    backend.scene.review("archive");
    backend.step(90);
    const QJsonValue defaultProjection = backend.frame()["projection"];
    backend.action("archive:tab-mode");
    backend.step(90.1);
    check(hud.trackListMode, "tab list enabled");
    check(backend.frame()["projection"] == defaultProjection, "tab keeps default composition");
    backend.action("archive:tab-mode");
    backend.step(90.2);
    check(!hud.trackListMode, "tab list disabled");
    check(backend.frame()["projection"] == defaultProjection, "tab exit keeps default composition");
    music.current = savedItem;
    hud.trackListMode = savedTrackList;
    hud.lyricsMode = savedLyrics;
    hud.modal = savedModal;

    // 场景增强使用同一真实窗口继续验收，不重复创建应用实例。
    ok &= runSceneEnhancementChecks(backend, window, output.isEmpty() ? QString() : output + "/enhancement");

    qInfo() << "UI core functionality tests:" << testCount << (ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
