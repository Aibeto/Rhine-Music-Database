#include "NativeApplication.h"
#include "AppStorage.h"
#include "MusicDiscover.h"
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace {
// 会话保留元数据与逻辑索引；播放时重新获取有时效的在线音源。
QJsonObject directoryState(const MusicDirectory &folder) {
    return {{"item", folder.item},
            {"songs", folder.songs},
            {"ids", folder.ids},
            {"albums", folder.albums},
            {"cursor", folder.cursor},
            {"index", folder.index},
            {"anchor", folder.anchor},
            {"page", folder.page},
            {"parentPhysical", folder.parentPhysical},
            {"parentLane", folder.parentLane},
            {"parentItem", folder.parentItem},
            {"albumCursor", folder.albumCursor},
            {"albumsMore", folder.albumsMore}};
}
int nonnegative(const QJsonValue &value) {
    return std::max(0, value.toInt());
}
}

QJsonObject MusicPlayer::sessionState() const {
    const auto live = m_host->mediaState();
    const auto position =
        m_resumePosition.value_or(live.value("position", state["position"].toVariant()).toDouble());
    const bool requested = m_resumePosition
                               ? !m_resumePaused
                               : loading || live.value("requested", state["playing"].toVariant()).toBool();
    return {{"current", current},
            {"queue", queue},
            {"index", index},
            {"mode", mode},
            {"position", position},
            {"requested", requested},
            {"preview", previewRequested}};
}

void MusicPlayer::restoreSessionState(const QJsonObject &saved) {
    const auto position = saved["position"].toDouble(-1);
    if (!std::isfinite(position) || position < 0 || position > 1e12) {
        return;
    }
    const auto savedMode = saved["mode"].toString();
    mode = QStringList{"all", "one", "off", "shuffle"}.contains(savedMode) ? savedMode : "all";
    const auto track = saved["current"].toObject();
    auto source = saved["queue"].toArray();
    if (track.isEmpty()) {
        queue = source;
        index = -1;
        emit changed();
        return;
    }
    if (track["kind"] != "song" || track["provider"].toString(m_api->provider) != m_api->provider) {
        return;
    }
    // 用歌曲身份重建索引，损坏的旧索引不能使下一首跳到无关位置。
    play(track, source, std::nullopt, saved["preview"].toBool(), saved);
}

QJsonObject MusicLibrary::sessionState() const {
    QJsonArray sources;
    for (int lane = 0; lane < 5; ++lane) {
        sources.append(QJsonObject{{"items", lists[lane]},
                                   {"total", totals[lane]},
                                   {"offset", offsets[lane]},
                                   {"more", hasMore[lane]},
                                   {"row", rowMemory[lane]}});
    }
    QJsonArray folders;
    for (auto folder = directory; folder && folders.size() < 64; folder = folder->parent) {
        folders.prepend(directoryState(*folder));
    }
    QJsonArray indices;
    for (auto index : groupIndices) {
        indices.append(index);
    }
    return {{"provider", provider},
            {"edition", kugouEdition},
            {"uid", uid},
            {"sources", sources},
            {"directories", folders},
            {"current", current},
            {"selectedPhysical", selectedPhysical},
            {"selectedLane", selectedLane},
            {"keyword", keyword},
            {"searchKind", searchKind},
            {"status", status},
            {"searchLoading", searchLoading},
            {"directoryLoading", directoryLoading},
            {"detailLoading", detailLoading},
            {"privateSelection", privateSelection},
            {"searchCursor", searchCursor},
            {"playlistCursor", playlistCursor},
            {"albumCursor", albumCursor},
            {"likedCursor", likedCursor},
            {"likedIDs", likedIDs ? QJsonValue(*likedIDs) : QJsonValue()},
            {"groupName", groupName},
            {"groupIndices", indices},
            {"groupLane", groupLane},
            {"tracks", tracks},
            {"trackIDs", trackIDs},
            {"trackCursor", trackCursor},
            {"albumLoaded", albumLoaded},
            {"playContext", playContext},
            {"directoryInfo", directoryInfo},
            {"wantsSongDetails", wantsSongDetails},
            {"discover", QJsonObject{{"tab", discover->tab},
                                     {"items", discover->items},
                                     {"offset", discover->offset},
                                     {"more", discover->more},
                                     {"category", discover->category},
                                     {"newSongArea", discover->newSongArea}}}};
}

bool MusicLibrary::restoreSessionState(const QJsonObject &saved) {
    if (saved["provider"] != provider || saved["edition"] != kugouEdition || saved["uid"].toString() != uid) {
        return false;
    }
    const auto sources = saved["sources"].toArray();
    const int lane = saved["selectedLane"].toInt(-1);
    const int physical = saved["selectedPhysical"].toInt(-1);
    if (sources.size() != 5 || lane < 0 || lane >= 5 || physical < -1 || physical >= 40 ||
        (physical >= 0 && !physicalSlots[lane].contains(physical))) {
        return false;
    }
    const auto folders = saved["directories"].toArray();
    if (folders.size() > 64) {
        return false;
    }
    ++m_selectionGeneration;
    ++m_directoryGeneration;
    ++m_wikiGeneration;
    for (int i = 0; i < 5; ++i) {
        const auto source = sources[i].toObject();
        lists[i] = source["items"].toArray();
        totals[i] = std::max(int(lists[i].size()), nonnegative(source["total"]));
        offsets[i] = std::clamp(source["offset"].toInt(), 0, std::max(0, int(lists[i].size()) - 1));
        rowMemory[i] = std::clamp(source["row"].toInt(), 0, 7);
        hasMore[i] = source["more"].toBool();
    }
    directory.reset();
    for (const auto &entry : folders) {
        const auto value = entry.toObject();
        auto folder = std::make_shared<MusicDirectory>();
        folder->parent = directory;
        folder->item = value["item"].toObject();
        folder->songs = value["songs"].toArray();
        folder->ids = value["ids"].toArray();
        folder->albums = value["albums"].toArray();
        folder->cursor = std::clamp(value["cursor"].toInt(), 0, int(folder->ids.size()));
        folder->index = std::clamp(value["index"].toInt(), 0, std::max(0, int(folder->songs.size()) - 1));
        folder->anchor = std::clamp(value["anchor"].toInt(), 0, 39);
        folder->page = folder->index / 40 * 40;
        folder->parentPhysical = std::clamp(value["parentPhysical"].toInt(), 0, 39);
        folder->parentLane = std::clamp(value["parentLane"].toInt(), 0, 4);
        folder->parentItem = value["parentItem"].toObject();
        folder->albumCursor = nonnegative(value["albumCursor"]);
        folder->albumsMore = value["albumsMore"].toBool();
        directory = folder;
    }
    selectedLane = lane;
    selectedPhysical = physical;
    keyword = saved["keyword"].toString();
    searchKind = saved["searchKind"].toString("song");
    status = saved["status"].toString();
    searchCursor = nonnegative(saved["searchCursor"]);
    playlistCursor = nonnegative(saved["playlistCursor"]);
    albumCursor = nonnegative(saved["albumCursor"]);
    likedCursor = nonnegative(saved["likedCursor"]);
    likedIDs =
        saved["likedIDs"].isArray() ? std::optional<QJsonArray>(saved["likedIDs"].toArray()) : std::nullopt;
    groupName = saved["groupName"].toString();
    groupLane = std::clamp(saved["groupLane"].toInt(-1), -1, 4);
    groupIndices.clear();
    for (const auto &value : saved["groupIndices"].toArray()) {
        groupIndices.append(nonnegative(value));
    }
    current = saved["current"].toObject();
    tracks = saved["tracks"].toArray();
    trackIDs = saved["trackIDs"].toArray();
    trackCursor = nonnegative(saved["trackCursor"]);
    albumLoaded = saved["albumLoaded"].toBool();
    playContext = saved["playContext"].toArray();
    privateSelection = saved["privateSelection"].toBool();
    directoryInfo = saved["directoryInfo"].toBool();
    wantsSongDetails = saved["wantsSongDetails"].toBool();
    detailLoading = directoryLoading = trackLoad = false;
    const auto discovery = saved["discover"].toObject();
    discover->tab = discovery["tab"].toString("playlists");
    discover->items = discovery["items"].toArray();
    discover->offset = nonnegative(discovery["offset"]);
    discover->more = discovery["more"].toBool();
    discover->category = discovery["category"].toString("全部");
    discover->newSongArea = discovery["newSongArea"].toString("5");
    coverImage.clear();
    coverKey.clear();
    emit musicIdentityRequested(current["key"].toString());
    loadCover(current);
    changed();
    if (saved["searchLoading"].toBool() && !keyword.isEmpty()) {
        search(keyword, searchKind, !lists[0].isEmpty());
    }
    if (saved["directoryLoading"].toBool() && directory && directory->songs.isEmpty()) {
        const auto item = directory->item;
        directory = directory->parent;
        enterDirectory(item);
    } else if (saved["detailLoading"].toBool() && !current.isEmpty()) {
        choose(current, directoryInfo, wantsSongDetails);
    }
    if (!directory && selectedLane > 0 && lists[selectedLane].isEmpty()) {
        m_pendingLane = selectedLane;
    }
    return true;
}

QJsonObject NativeApplication::sessionState() const {
    return {{"version", 1},
            {"library", music.sessionState()},
            {"player", music.player.sessionState()},
            {"view", QJsonObject{{"mode", m_scene->mode()},
                                 {"selected", m_scene->selection()},
                                 {"tab", hud.tab},
                                 {"modal", hud.closing ? QString() : hud.modal},
                                 {"queueOffset", hud.queueOffset},
                                 {"settingsScroll", hud.settingsScroll},
                                 {"query", hud.query},
                                 {"groupBy", hud.groupBy},
                                 {"activeGroup", hud.activeGroup},
                                 {"artistTab", hud.artistTab},
                                 {"listOffset", hud.listOffset},
                                 {"trackOffset", hud.trackOffset},
                                 {"bodyOffset", hud.bodyOffset},
                                 {"musicCollection", hud.musicCollection},
                                 {"trackListMode", hud.trackListMode},
                                 {"lyricsMode", hud.lyricsMode}}}};
}

bool NativeApplication::restoreSessionState(const QJsonObject &saved) {
    if (saved["version"] != 1 || !music.restoreSessionState(saved["library"].toObject())) {
        return false;
    }
    const auto view = saved["view"].toObject();
    const auto modal = view["modal"].toString();
    if (QStringList{"saved", "search", "discover", "queue", "settings", "player", "local"}.contains(modal)) {
        hud.openModal(modal);
    }
    hud.queueOffset = nonnegative(view["queueOffset"]);
    hud.settingsScroll = std::clamp(view["settingsScroll"].toDouble(), 0., hud.settingsScrollMaximum());
    const auto tab = view["tab"].toString();
    hud.tab = QStringList{"overview", "notes", "history"}.contains(tab) ? tab : "overview";
    hud.query = view["query"].toString();
    hud.groupBy = view["groupBy"].toString("none");
    hud.activeGroup = view["activeGroup"].toString();
    hud.artistTab = view["artistTab"].toString("songs");
    hud.listOffset = nonnegative(view["listOffset"]);
    hud.trackOffset = nonnegative(view["trackOffset"]);
    hud.bodyOffset = nonnegative(view["bodyOffset"]);
    hud.musicCollection = std::clamp(view["musicCollection"].toInt(1), 1, 4);
    hud.trackListMode = view["trackListMode"].toBool();
    hud.lyricsMode = view["lyricsMode"].toBool();
    const auto selected = std::clamp(view["selected"].toInt(), 0, 39);
    m_scene->select(selected);
    m_scene->setMode(view["mode"] == "detail" && !music.current.isEmpty() ? "detail" : "archive");
    music.player.restoreSessionState(saved["player"].toObject());
    return true;
}

void NativeApplication::saveSession() {
    if (m_host->offline) {
        return;
    }
    const auto directory = AppStorage::dataDirectory();
    QSaveFile file(directory + "/session.json");
    const auto bytes = QJsonDocument(sessionState()).toJson(QJsonDocument::Compact);
    if (bytes.size() > 64 * 1024 * 1024) {
        qWarning("会话过大，已保留原有存档");
        return;
    }
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        qWarning("无法保存上次会话");
    }
}
