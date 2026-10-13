// 分组只覆盖已载入结果；保留原始索引，合唱曲目可以出现在多个歌手组中。
export function groupRows(rows, kind) {
    const groups = new Map();
    for (const entry of rows) {
        const value = entry[1];
        let labels;
        if (kind === 'artist') {
            if (value.artists?.length) {
                labels = value.artists.map((a) => [a.id ? 'artist:' + a.id : 'name:' + a.name, a.name]);
            } else {
                labels = [['name:' + value.artist, value.artist || '未知歌手']];
            }
        } else {
            labels = [
                [
                    value.albumID ? 'album:' + value.albumID : 'album-name:' + value.album,
                    value.album || '未知专辑',
                ],
            ];
        }
        const seen = new Set();
        for (const [key, title] of labels) {
            if (seen.has(key)) {
                continue;
            }
            seen.add(key);
            if (!groups.has(key)) {
                groups.set(key, {
                    key,
                    title,
                    entries: [],
                });
            }
            groups.get(key).entries.push(entry);
        }
    }
    return Array.from(groups.values()).map((group) => [
        -1,
        {
            kind: 'group',
            id: String(group.entries.length),
            title: group.title,
            artist: group.entries.length + ' 首已载入',
            album: '点击展开分组',
            ...group,
        },
    ]);
}
