#include "AudioPlayer.h"
#include <QUrl>
#include <algorithm>
#include <cmath>

AudioPlayer::AudioPlayer(std::function<void()> changed) {
    if (changed) {
        // 播放状态事件立即唤醒；进度无需依附 60Hz 的场景循环。
        QObject::connect(&player, &QMediaPlayer::playbackStateChanged, &player, [changed] { changed(); });
        QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &player, [changed] { changed(); });
        QObject::connect(&player, &QMediaPlayer::errorOccurred, &player, [changed] { changed(); });
        QObject::connect(&player, &QMediaPlayer::durationChanged, &player, [changed] { changed(); });
    }
    player.setAudioOutput(&output);
    output.setVolume(.7f);
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &player, [this](auto status) {
        if (status == QMediaPlayer::EndOfMedia) {
            ++ended;
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &player, [this](auto, const QString &) {
        // 不回显底层错误中的临时签名链接；只向界面反馈可操作的错误。
        error = QStringLiteral("音频读取失败，请重试或打开来源页面");
    });
}
/**
 * 校验音源并交给系统解码播放，沿用原生状态和错误反馈。
 */
bool AudioPlayer::load(QString input, bool offline, bool alternative, QString provider) {
    QUrl url(input);
    const auto host = url.host();
    const bool qq = provider == "qq" && (host == "qqmusic.qq.com" || host.endsWith(".qqmusic.qq.com"));
    const bool trusted =
        qq || (provider == "netease" && (host == "music.126.net" || host.endsWith(".music.126.net") ||
                                         host == "music.163.com" || host.endsWith(".music.163.com")));
    const QStringList providers{"qq.com", "gtimg.com", "kuwo.cn", "kugou.com", "migu.cn"};
    const bool alternateHost =
        alternative && std::any_of(providers.begin(), providers.end(), [&](const auto &domain) {
            return host == domain || host.endsWith("." + domain);
        });
    if (offline ? !url.isLocalFile()
                : (!(trusted || alternateHost) || !QStringList{"http", "https"}.contains(url.scheme()))) {
        return false;
    }
    if (url.scheme() == "http") {
        url.setScheme("https");
    }
    player.stop();
    error.clear();
    player.setSource(url);
    player.play();
    return true;
}
void AudioPlayer::control(QString action, double value) {
    if (!std::isfinite(value)) {
        return;
    }
    if (action == "pause") {
        player.pause();
    } else if (action == "resume") {
        player.play();
    } else if (action == "stop") {
        player.stop();
        player.setSource({});
        error.clear();
    } else if (action == "seek" && player.isSeekable()) {
        player.setPosition(qint64(std::clamp(value, 0., double(player.duration()))));
    } else if (action == "volume") {
        output.setVolume(float(std::clamp(value, 0., 1.)));
    }
}
QVariantMap AudioPlayer::state() const {
    return {{"playing", player.isPlaying()},
            {"position", player.position()},
            {"duration", player.duration()},
            {"seekable", player.isSeekable()},
            {"volume", output.volume()},
            {"ended", ended},
            {"error", error},
            {"buffering", player.mediaStatus() == QMediaPlayer::LoadingMedia ||
                              player.mediaStatus() == QMediaPlayer::StalledMedia}};
}
