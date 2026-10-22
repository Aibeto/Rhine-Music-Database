#include "NativeHud.h"
#include "MusicData.h"
#include "MusicDiscover.h"
#include <QDebug>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

namespace {
QJsonObject song(int index) {
    auto value =
        MusicData::item("song", QString::number(index), "本地曲目 " + QString::number(index), "测试歌手");
    value["album"] = "测试专辑";
    value["albumID"] = "42";
    value["duration"] = 180000;
    value["wiki"] = "第一段百科\n第二段百科\n第三段百科\n第四段百科";
    value["artists"] = QJsonArray{QJsonObject{{"id", "7"}, {"name", "测试歌手"}}};
    return value;
}

// 辅助函数：检查 hits 中是否存在指定 action
bool hasAction(const QJsonArray &hits, const QString &action) {
    for (const auto &value : hits) {
        if (value.toObject()["action"].toString() == action) {
            return true;
        }
    }
    return false;
}

// 辅助函数：获取指定 action 的 hit 对象
QJsonObject getHit(const QJsonArray &hits, const QString &action) {
    for (const auto &value : hits) {
        auto hit = value.toObject();
        if (hit["action"].toString() == action) {
            return hit;
        }
    }
    return QJsonObject();
}

// 辅助函数：统计指定类型的 hits 数量
int countHitsByPrefix(const QJsonArray &hits, const QString &prefix) {
    int count = 0;
    for (const auto &value : hits) {
        if (value.toObject()["action"].toString().startsWith(prefix)) {
            ++count;
        }
    }
    return count;
}

// 创建测试用的 MusicLibrary
MusicLibrary *createTestMusicLibrary(Host *host, const QString &provider, const QJsonObject &content) {
    MusicLibrary *music = new MusicLibrary(host, content);
    music->provider = provider;
    music->api.provider = provider;
    music->uid = "42";
    music->nickname = "测试账号";
    music->status = "离线固定时间校验";
    music->keyword = "本地";
    music->loginStatus = "等待确认";
    music->coverImage.clear();
    music->likedIDs = QJsonArray{"1"};

    QJsonArray songs;
    for (int index = 1; index <= 20; ++index) {
        songs.append(song(index));
    }
    music->current = songs.first().toObject();
    music->tracks = songs;
    for (int lane = 0; lane < 5; ++lane) {
        music->lists[lane] = songs;
        music->totals[lane] = songs.size();
        music->hasMore[lane] = false;
    }
    music->player.queue = songs;
    music->player.current = music->current;
    music->player.index = 2;
    music->player.state = {{"playing", true},
                           {"position", 45678},
                           {"duration", 180000},
                           {"volume", 0.7},
                           {"seekable", true}};
    music->player.status = "原生播放器";
    music->player.quality = "exhigh";
    music->player.alternative = false;
    music->discover->items = songs;
    music->discover->status = "本地发现";
    music->discover->more = true;
    return music;
}
}

/**
 * Native HUD 核心功能回归测试
 *
 * 验证关键交互逻辑、状态管理和面板渲染，不依赖像素精确对比。
 */
bool runNativeHudChecks(const QString &root, Glyphs &glyphs) {
    Host host(root, true);
    const auto content = QJsonDocument::fromJson(host.read("slots.json").toUtf8()).object();
    bool ok = true;
    int testCount = 0;

    // 测试所有平台和主题组合
    for (const auto &provider : {QString("netease"), QString("qq"), QString("kugou")}) {
        for (const bool dark : {false, true}) {
            testCount++;

            MusicLibrary *music = createTestMusicLibrary(&host, provider, content);
            HudSceneActions scene;
            scene.theme = [](bool) {};
            NativeHud hud(&host, &glyphs, music, scene);
            hud.dark = dark;
            hud.themeMode = dark ? "dark" : "light";
            hud.motion.set(true, 0, true);

            // === 测试 1: 基础面板渲染 ===
            QJsonObject archiveFrame{{"mode", "archive"}, {"selected", 0}, {"detail", 1}, {"viewer", false}};
            hud.draw(archiveFrame, 1920, 50);

            if (hud.commands.isEmpty()) {
                qCritical() << "Archive panel commands empty" << provider << (dark ? "dark" : "light");
                ok = false;
            }
            if (hud.hits.isEmpty()) {
                qCritical() << "Archive panel hits empty" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // === 测试 2: Settings 面板功能 ===
            hud.openModal("settings");
            hud.draw(archiveFrame, 1920, 50);

            // 验证 settings 面板渲染
            if (!hasAction(hud.hits, "close-modal")) {
                qCritical() << "Settings panel missing close button" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // 验证关键设置项存在（顶部配置区域）
            const QStringList requiredSettings = {"theme", "motion", "startup-animation", "detail-autoplay",
                                                  "scene-enhancement", "quality"};
            for (const auto &setting : requiredSettings) {
                if (!hasAction(hud.hits, setting)) {
                    qCritical() << "Missing setting toggle:" << setting << provider
                                << (dark ? "dark" : "light");
                    ok = false;
                }
            }

            // 滚动到底部以验证性能相关设置项
            {
                const auto bounds = hud.modalBounds(1920);
                hud.wheel(bounds.center().x(), bounds.top() + 500, -120, 1, 10000.);
                hud.draw(archiveFrame, 1920, 50);
            }

            // 验证性能相关设置项存在（下方性能区域）
            const QStringList performanceSettings = {"disc-rotation", "performance:toggle"};
            for (const auto &setting : performanceSettings) {
                if (!hasAction(hud.hits, setting)) {
                    qCritical() << "Missing performance toggle:" << setting << provider
                                << (dark ? "dark" : "light");
                    ok = false;
                }
            }

            // 滚动回顶部继续其他测试
            {
                const auto bounds = hud.modalBounds(1920);
                hud.wheel(bounds.center().x(), bounds.top() + 500, 120, -1, -10000.);
                hud.draw(archiveFrame, 1920, 50);
            }

            // === 测试 3: 设置项切换功能 ===
            const bool initialStartupAnim = hud.startupAnimation;
            hud.action("startup-animation");
            const bool toggledStartupAnim = hud.startupAnimation;
            hud.action("startup-animation");
            const bool restoredStartupAnim = hud.startupAnimation;

            if (initialStartupAnim == toggledStartupAnim || restoredStartupAnim != initialStartupAnim) {
                qCritical() << "Startup animation toggle failed" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // === 测试 4: 设置面板滚动功能 ===
            const auto bounds = hud.modalBounds(1920);

            // 向下滚动到底部
            hud.wheel(bounds.center().x(), bounds.top() + 500, -120, 1, 10000.);
            hud.draw(archiveFrame, 1920, 50);

            // 验证滚动到最大值
            const bool atEnd = hud.settingsScroll == hud.settingsScrollMaximum();
            if (!atEnd) {
                qCritical() << "Settings scroll to bottom failed" << provider << (dark ? "dark" : "light")
                            << "scroll:" << hud.settingsScroll << "max:" << hud.settingsScrollMaximum();
                ok = false;
            }

            // 验证标题固定、内容裁剪
            bool headerFixed = false;
            bool contentClipped = true;
            for (const auto &value : hud.hits) {
                const auto hit = value.toObject();
                const auto action = hit["action"].toString();
                if (action == "close-modal") {
                    headerFixed = hit["y"].toDouble() == bounds.top() + 18;
                } else if (!action.startsWith("settings:scroll-") && !action.startsWith("player:")) {
                    const double hitTop = hit["y"].toDouble();
                    const double hitBottom = hitTop + hit["height"].toDouble();
                    const double viewportTop = bounds.top() + NativeHud::SettingsViewportTop;
                    const double viewportBottom = bounds.top() + NativeHud::SettingsViewportBottom;
                    contentClipped &= (hitTop >= viewportTop && hitBottom <= viewportBottom);
                }
            }

            if (!headerFixed) {
                qCritical() << "Settings header not fixed" << provider << (dark ? "dark" : "light");
                ok = false;
            }
            if (!contentClipped) {
                qCritical() << "Settings content not clipped" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // 向上滚动回顶部
            hud.wheel(bounds.center().x(), bounds.top() + 500, 120, -1, -10000.);
            const bool atStart = hud.settingsScroll == 0;
            if (!atStart) {
                qCritical() << "Settings scroll to top failed" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // 测试滚动按钮
            hud.action("settings:scroll-down");
            const bool scrolledDown = hud.settingsScroll > 0;
            if (!scrolledDown) {
                qCritical() << "Settings scroll-down action failed" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // 重新打开 settings 应该重置滚动位置
            hud.openModal("settings");
            const bool reset = hud.settingsScroll == 0;
            if (!reset) {
                qCritical() << "Settings scroll reset failed" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // === 测试 5: Detail 面板 ===
            QJsonObject detailFrame{{"mode", "detail"}, {"selected", 1}, {"detail", 1}, {"viewer", false}};
            hud.modal.clear();
            hud.draw(detailFrame, 1920, 50);

            if (hud.commands.isEmpty()) {
                qCritical() << "Detail panel commands empty" << provider << (dark ? "dark" : "light");
                ok = false;
            }

            // === 测试 6: 其他关键面板存在性检查 ===
            const QStringList criticalPanels = {"search", "login", "player", "confirm"};
            for (const auto &panel : criticalPanels) {
                hud.openModal(panel);
                hud.draw(archiveFrame, 1920, 50);

                if (hud.commands.isEmpty()) {
                    qCritical() << "Panel commands empty:" << panel << provider << (dark ? "dark" : "light");
                    ok = false;
                }
                if (!hasAction(hud.hits, "close-modal") && panel != "player") {
                    qCritical() << "Panel missing close button:" << panel << provider
                                << (dark ? "dark" : "light");
                    ok = false;
                }
            }

            delete music;
        }
    }

    qInfo() << "Native HUD core functionality tests:" << testCount << (ok ? "passed" : "FAILED");
    return ok;
}

#ifdef RHINE_HUD_REGRESSION_TEST_MAIN
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const auto root = application.arguments().value(1);
    Glyphs glyphs(root);
    return runNativeHudChecks(root, glyphs) ? 0 : 1;
}
#endif
