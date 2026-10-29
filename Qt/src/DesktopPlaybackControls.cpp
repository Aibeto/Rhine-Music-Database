#include "DesktopPlaybackControls.h"
#include "MusicData.h"
#include "MusicPlayer.h"
#include <QJsonDocument>
#include <QDebug>

DesktopPlaybackControls::DesktopPlaybackControls(QString root, PlaybackConnection *connection,
                                                 QObject *parent)
    : QObject(parent), m_connection(connection), m_host(std::move(root), false, nullptr, false, connection),
      m_api(&m_host) {
    connect(connection, &PlaybackConnection::snapshotChanged, this, &DesktopPlaybackControls::refresh,
            Qt::QueuedConnection);
    connect(connection, &PlaybackConnection::disconnected, this, [this] {
        m_connected = false;
        ++m_generation;
        emit changed();
    });
    refresh();
}

QJsonObject DesktopPlaybackControls::state() const {
    const bool hasTrack = m_connected && !m_snapshot["current"].toObject().isEmpty();
    return {{"playing", m_snapshot["state"].toObject()["playing"]},
            {"canPlay", hasTrack && !m_snapshot["loading"].toBool()},
            {"canSkip", hasTrack && !m_snapshot["queue"].toArray().isEmpty()},
            {"canLike", hasTrack && m_likeKnown && !m_writing},
            {"liked", m_liked}};
}

QImage DesktopPlaybackControls::cover() const {
    return m_cover;
}

MusicApi::Valid DesktopPlaybackControls::valid(quint64 generation) {
    return [owner = QPointer(this), generation] {
        return owner && owner->m_connected && owner->m_generation == generation;
    };
}

/** 快照变化只更新轻量控制状态；封面最多保留当前一张，迟到响应不能覆盖新歌曲。 */
void DesktopPlaybackControls::refresh() {
    if (!m_connection || !m_connected) {
        return;
    }
    m_snapshot = m_connection->snapshot();
    const auto track = m_snapshot["current"].toObject();
    const auto provider = m_snapshot["provider"].toString(track["provider"].toString());
    const auto url = track["cover"].toString();
    if (url != m_coverUrl) {
        m_coverUrl = url;
        m_cover = {};
        m_host.trimMemory({});
        const auto generation = ++m_coverGeneration;
        if (!url.isEmpty()) {
            m_host.loadCover(url, [this, generation](QString key) {
                if (generation == m_coverGeneration) {
                    m_cover = m_host.image(key);
                    m_host.trimMemory({key});
                    emit changed();
                }
            });
        }
    }
    const auto scope =
        QJsonDocument(QJsonArray{provider, MusicPlayer::identity(track), m_snapshot["accountEpoch"],
                                 m_snapshot["metadataRevision"], m_snapshot["credentials"]})
            .toJson(QJsonDocument::Compact);
    if (scope != m_scope) {
        m_scope = scope;
        ++m_generation;
        m_api.provider = provider;
        m_host.clearPreferenceCache();
        m_likeKnown = false;
        m_liked = false;
        m_writing = false;
        refreshLike(m_generation);
    }
    emit changed();
}

/** 沿用锁定上游的红心读取契约；账号和歌曲的代数共同隔离异步结果。 */
void DesktopPlaybackControls::refreshLike(quint64 generation) {
    const auto track = m_snapshot["current"].toObject();
    const auto provider = m_api.provider;
    if (track["kind"] != "song" || track["provider"].toString(provider) != provider ||
        !m_snapshot["credentials"].toObject()[provider].toBool()) {
        return;
    }
    if (provider == "netease") {
        const auto uid = m_host.preference("account-id-netease");
        if (uid.isEmpty()) {
            return;
        }
        m_api.call(
            "likedIDs", {uid},
            [this, track](MusicReply result) {
                if (result.ok()) {
                    m_liked = MusicData::indexOf(result.value.toArray(), track["id"]) >= 0;
                    m_likeKnown = true;
                    emit changed();
                }
            },
            valid(generation));
    } else if (provider == "qq") {
        m_api.call(
            "capabilities", {},
            [this, track, generation](MusicReply result) {
                if (!result.ok() || !result.value.toObject()["mutations"].toObject()["likeSong"].toBool()) {
                    return;
                }
                m_api.call(
                    "subscriptionStatus", {"song", track["id"]},
                    [this](MusicReply status) {
                        if (status.ok()) {
                            const auto value = status.value.toString();
                            m_likeKnown = value == "subscribed" || value == "not-subscribed";
                            m_liked = value == "subscribed";
                            emit changed();
                        }
                    },
                    valid(generation));
            },
            valid(generation));
    }
}

void DesktopPlaybackControls::trigger(const QString &action) {
    if (!m_connection || !m_connected) {
        return;
    }
    const auto completed = [](QJsonObject, QString error) {
        if (!error.isEmpty()) {
            qWarning().noquote() << "桌面播放控制失败：" << error;
        }
    };
    const auto controls = state();
    if (action == "like" && controls["canLike"].toBool()) {
        toggleLike();
    } else if (action == "toggle" && controls["canPlay"].toBool()) {
        m_connection->request("player.toggle", {}, completed);
    } else if ((action == "prev" || action == "next") && controls["canSkip"].toBool()) {
        m_connection->request("player.next", {{"direction", action == "next" ? 1 : -1}}, completed);
    }
}

/** 写入只发起一次，结果确认后清缓存；不因界面挂起或恢复重复提交。 */
void DesktopPlaybackControls::toggleLike() {
    const auto generation = m_generation;
    const auto track = m_snapshot["current"].toObject();
    const bool liked = !m_liked;
    m_writing = true;
    emit changed();
    const auto completed = [this, liked](MusicReply result) {
        m_writing = false;
        if (result.ok() && result.value.toObject()["body"].toObject()["code"].toInt(200) == 200) {
            m_liked = m_api.provider == "qq" ? result.value.toObject()["liked"].toBool() : liked;
            m_host.invalidateMetadata();
        } else {
            m_likeKnown = false;
        }
        emit changed();
    };
    if (m_api.provider == "qq") {
        m_api.call("mutate", {"/like", QJsonObject{{"id", track["id"]}}}, completed, valid(generation));
    } else {
        m_api.request("/like", {{"id", track["id"]}, {"like", liked}}, completed, valid(generation));
    }
}
