// HUD 文案选择与绘制分离；分支顺序和文字保持原样。
// 按既有优先级选择界面文案，不修改音乐状态。
export function sessionLabel(music) {
    if (music.uid) {
        if (music.provider === 'qq') {
            return 'QQ SESSION';
        } else {
            return 'NETEASE SESSION';
        }
    } else {
        return 'GUEST SESSION';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function libraryTitle(music) {
    if (music.directory) {
        return music.libraryName;
    } else if (!music.uid && music.selectedLane > 0) {
        return '登录后查看';
    } else {
        return '音乐资料库';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function libraryActionLabel(music) {
    if (music.directory) {
        return '查看音乐详情 →';
    } else if (music.selectedLane > 0) {
        return '登录' + (music.providerName || '网易云') + ' →';
    } else {
        return '检索音乐 →';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function libraryAction(music) {
    if (music.directory) {
        return 'music:directory-info';
    } else if (music.selectedLane > 0) {
        return 'music:login';
    } else {
        return 'search';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function archiveActionLabel(item) {
    if (item.kind === 'song') {
        return '查看歌曲详情 →';
    } else if (item.kind === 'album') {
        return '打开专辑资料库 →';
    } else if (item.kind === 'artist') {
        return '浏览歌手作品 →';
    } else {
        return '打开歌单资料库 →';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function collectionLabel(item) {
    if (item.kind === 'album') {
        return 'ALBUM / 专辑';
    } else if (item.kind === 'artist') {
        return 'ARTIST / 歌手';
    } else {
        return 'PLAYLIST / 歌单';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function modalCategory(modal) {
    if (modal === 'settings') {
        return 'SYSTEM PREFERENCES';
    } else if (modal === 'player') {
        return 'AUDIO TERMINAL';
    } else {
        return 'MUSIC DIRECTORY';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function modalTitle(modal, music) {
    if (modal === 'settings') {
        return 'SYSTEM SETTINGS';
    } else if (modal === 'saved') {
        return 'MY MUSIC';
    } else if (modal === 'login') {
        if (music.provider === 'qq') {
            return 'QQ MUSIC LOGIN';
        } else {
            return 'NETEASE LOGIN';
        }
    } else if (modal === 'player') {
        return 'NOW PLAYING';
    } else if (modal === 'discover') {
        return 'DISCOVER MUSIC';
    } else if (modal === 'playlist-form') {
        return 'PLAYLIST EDITOR';
    } else if (modal === 'playlist-picker') {
        return 'PLAYLIST DIRECTORY';
    } else if (modal === 'confirm') {
        return 'CONFIRM DELETE';
    } else {
        return 'MUSIC INDEX';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function modalSubtitle(modal, music) {
    if (modal === 'saved') {
        return '我的音乐 / ' + music.nickname;
    } else if (modal === 'settings') {
        return '终端偏好设置';
    } else if (modal === 'login') {
        return '可选登录，公开搜索不需要账号';
    } else if (modal === 'player') {
        return '播放队列 / QUEUE';
    } else if (modal === 'discover') {
        return '推荐 / 排行榜 / 新专辑';
    } else if (modal === 'playlist-form') {
        return '编辑我的歌单';
    } else if (modal === 'playlist-picker') {
        return '添加到我的歌单';
    } else if (modal === 'confirm') {
        return '删除确认';
    } else {
        return '音乐检索';
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function emptyLibraryMessage(music, lane) {
    if (music.sourceLoading) {
        return '正在读取音乐…';
    } else if (lane > 0 && !music.uid) {
        return '登录后查看个人音乐';
    } else {
        return music.status;
    }
}
// 按既有优先级选择界面文案，不修改音乐状态。
export function themeLabel(themeMode, dark) {
    if (themeMode === 'system') {
        return '跟随系统';
    } else if (dark) {
        return '暗色';
    } else {
        return '亮色';
    }
}
