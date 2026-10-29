#include "NativeHud.h"
#include "MusicDiscover.h"
#include <algorithm>
#include <cmath>

/** 滚轮优先交给当前面板；歌词使用连续距离，资料库保留合并后的整数步数。 */
void NativeHud::wheel(double x, double y, double delta, std::optional<int> steps,
                      std::optional<double> distance) {
    if (frame.isEmpty() || !std::isfinite(delta)) {
        return;
    }
    if (playerBounds && playerBounds->contains(x, y)) {
        return;
    }
    const auto step = steps.value_or(delta < 0 ? 1 : -1);
    // 鼠标滚轮按帧平滑时角度增量只在首帧出现，后续只补送连续距离。
    if (delta == 0 && step == 0 && distance.value_or(0.) == 0) {
        return;
    }
    auto &music = *m_music;
    if (modal == "settings") {
        const auto bounds = modalBounds(width);
        const auto localY = y - motion.offset;
        if (!closing && x >= bounds.left() && x <= bounds.right() &&
            localY >= bounds.top() + SettingsViewportTop && localY <= bounds.top() + SettingsViewportBottom) {
            settingsScroll =
                std::clamp(settingsScroll + distance.value_or(step * 50.), 0., settingsScrollMaximum());
        }
        return;
    }
    if (frame["viewer"].toBool()) {
        if (delta != 0) {
            m_scene.pointer("wheel", x, y, -delta * 8);
        }
        return;
    }
    if (!modal.isEmpty()) {
        if (modal == "playlist-picker") {
            listOffset = std::clamp(listOffset + step, 0, std::max(0, int(music.lists[2].size()) - 7));
            return;
        }
        if (modal == "discover") {
            listOffset = std::clamp(listOffset + step, 0, std::max(0, int(music.discover->items.size()) - 6));
            return;
        }
        if (modal == "player") {
            queueOffset = std::clamp(queueOffset + step, 0, std::max(0, int(music.player.queue.size()) - 7));
            return;
        }
        if (closing || !QStringList{"search", "saved"}.contains(modal)) {
            return;
        }
        const auto bounds = modalBounds(width);
        y -= motion.offset;
        if (x < bounds.x() + 53 || x > bounds.right() - 53 || y < 475 || y > 867) {
            return;
        }
        const auto lane = modal == "saved" ? musicCollection : 0;
        const auto values = rows(lane);
        listOffset = std::clamp(listOffset + step, 0, std::max(0, int(values.size()) - 5));
        if (step > 0 && listOffset + 5 >= values.size() && !music.sourceLoading() && music.hasMore[lane]) {
            music.loadMoreSource(lane);
        }
        return;
    }
    if (lyricsMode && music.current.value("kind") == "song" && x >= width * 0.56 && y >= 360 && y < 889) {
        const auto lastTop = m_lyricsLayout.isEmpty() ? 0 : m_lyricsLayout.constLast().top;
        const auto movement = distance.value_or((-delta / 120) * 65);
        m_lyricsBrowse = std::clamp(m_lyricsBrowse.value_or(m_lyricsScroll) + movement, 0.0, lastTop);
        m_lyricsPlainOffset = std::max(0, m_lyricsPlainOffset + step);
        return;
    }
    if (frame["mode"] == "detail" && music.current.value("kind") != "song") {
        // 平滑帧只补送距离，不改曲目行偏移，也不重复触发分页加载。
        if (step == 0) {
            return;
        }
        const auto layout = containerLayout(width);
        if (x < layout.x || x > layout.x + layout.width || y < layout.rowsY ||
            y > layout.rowsY + layout.rowCount * layout.rowHeight) {
            return;
        }
        const bool albums =
            music.directory && music.directory->item["kind"] == "artist" && artistTab == "albums";
        int count = music.tracks.size();
        if (music.directory) {
            count = albums ? music.directory->albums.size() : music.directory->songs.size();
        }
        trackOffset = std::clamp(trackOffset + step, 0, std::max(0, count - layout.rowCount));
        if (trackOffset + layout.rowCount >= count) {
            if (albums) {
                music.loadArtistAlbums();
            } else if (music.directory) {
                music.loadDirectoryPage();
            } else {
                music.loadTrackPage(trackOffset);
            }
        }
        return;
    }
    if (frame["mode"] == "detail") {
        // 详情正文与历史列表只按行进，平滑帧不重复请求曲目页。
        if (step == 0) {
            return;
        }
        if (tab == "history") {
            trackOffset =
                std::clamp(trackOffset + step, 0, std::max(0, int(music.tracks.size()) - AlbumTrackRowCount));
            music.loadTrackPage(trackOffset + AlbumTrackRowCount - 1);
        } else {
            const auto count = m_glyphs->wrap(body(), width * 0.44 - 59, 16).size();
            bodyOffset = std::clamp(bodyOffset + step, 0, std::max(0, int(count) - 3));
        }
        return;
    }
    if (frame["mode"] == "archive") {
        if (step != 0) {
            music.next(step);
        }
    } else if (delta != 0) {
        m_scene.pointer("wheel", x, y, -delta * 8);
    }
}
