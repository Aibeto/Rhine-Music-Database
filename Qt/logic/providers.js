// 平台入口共用显示名称和能力说明，协议仍由各平台适配层负责。
export const PROVIDERS = Object.freeze([
    {
        id: 'netease',
        name: '网易云音乐',
        label: 'NETEASE',
        detail: '搜索 · 个人音乐 · 推荐 · 播放 · 歌单编辑',
    },
    { id: 'qq', name: 'QQ 音乐', label: 'QQ MUSIC', detail: '歌曲搜索 · 个人音乐 · 首页推荐 · 播放' },
    {
        id: 'kugou',
        name: '酷狗音乐',
        label: 'KUGOU',
        detail: '标准版 / 概念版 · 个人歌单 · 推荐与榜单 · 播放',
    },
]);
export function providerName(id) {
    return PROVIDERS.find((entry) => entry.id === id)?.name || '网易云音乐';
}
