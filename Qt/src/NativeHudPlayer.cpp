#include "NativeHud.h"
#include "MusicData.h"
#include "PlayerBar.h"
#include <algorithm>

/** 适配播放业务快照；状态栏组件不感知账号、歌词页面或播放器生命周期。 */
void NativeHud::playerBar(double x, double y, double width) {
    const auto &player = m_music->player;
    PlayerBar::State state;
    state.title = player.current.value("title").toString();
    state.artist = player.current.value("artist").toString();
    state.album = player.current.value("album").toString();
    state.cover = player.coverImage;
    state.status = player.status;
    state.mode = player.mode;
    state.positionMs = player.state["position"].toDouble();
    state.durationMs = player.state["duration"].toDouble();
    state.volume = player.state["volume"].toDouble();
    state.playing = player.state["playing"].toBool();
    state.loading = player.loading;
    state.failed = player.failed;
    state.hasTrack = !player.current.isEmpty();
    const auto liked = m_music->songLiked(player.current);
    state.liked = liked.value_or(false);
    state.likeKnown = liked.has_value();
    state.canLike = state.hasTrack && (m_music->isSiren() || !m_music->uid.isEmpty()) &&
                    !m_music->writeLoading && m_music->canMutate("/like") &&
                    player.current.value("provider").toString(m_music->provider) == m_music->provider;
    state.seekable = player.state["seekable"].toBool();
    state.applicationVolume = PlatformFeatures::ApplicationVolume;
    const auto drawing = PlayerBar::render(state, {x, y, width, PlayerBar::RowHeight}, dark, *m_glyphs);
    for (const auto &command : drawing.commands) {
        commands.append(command);
    }
    for (const auto &target : drawing.hits) {
        hits.append(target);
    }
    playerBounds = drawing.inputBounds;
    playerSeekRect = drawing.seek;
    playerVolumeRect = drawing.volume;
}

void NativeHud::playerPanel(double x, double y, double width) {
    const auto &player = m_music->player;
    const auto left = x + 53;
    const auto usable = width - 106;
    rule(left, y + 216, usable);
    auto title = player.current.value("title").toString();
    if (title.isEmpty()) {
        title = "选择歌曲开始播放";
    }
    limited(title, left, y + 240, 25, usable, "bold");
    limited(player.current.value("artist").toString(), left, y + 278, 15, usable);
    text("QUEUE / " + QString::number(player.queue.size()) + " 首", left, y + 326, 15);
    button("清空待播", left + usable - 140, y + 311, 140, 44, "player:clear", 16);
    queueOffset = std::min(queueOffset, std::max(0, int(player.queue.size()) - 7));
    for (int row = 0; row < 7 && queueOffset + row < player.queue.size(); ++row) {
        const auto index = queueOffset + row;
        const auto song = player.queue.at(index);
        const auto yy = y + 371 + row * 48;
        text(index == player.queue.index() ? "▶" : MusicData::pad(index + 1), left, yy, 15);
        button(fit(song["title"].toString(), 18, usable - 110), left + 44, yy - 7, usable - 110, 40,
               "player:queue:" + QString::number(index), 18);
        if (index != player.queue.index()) {
            button("×", left + usable - 40, yy - 7, 40, 40, "player:remove:" + QString::number(index), 20);
        }
        rule(left, yy + 34, usable, 0.15);
    }
    limited(player.status, left, y + 730, 14, usable);
    static const QHash<QString, QString> modes{
        {"off", "顺序播放"}, {"all", "列表循环"}, {"one", "单曲循环"}, {"shuffle", "随机播放"}};
    button(modes.value(player.mode), left, y + 761, 140, 44, "player:mode", 16);
    if (PlatformFeatures::OfficialPlayback && !m_music->isLocal()) {
        button(m_music->provider != "netease" ? m_music->providerName() + " ↗" : "官方客户端 ↗", left + 165,
               y + 761, 175, 44, "player:official", 16);
    }
    if (m_music->canMutate("/like")) {
        button(m_music->provider == "qq" ? "红心 / 切换" : "♡ 喜欢此曲", left + 365, y + 761, 140, 44,
               "music:like-current", 16);
    }
}
