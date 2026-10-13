import assert from 'node:assert/strict';
import { HUD } from '../logic/hud.js';
import { MusicStore } from '../logic/music.js';
const songs = (count) =>
    Array.from(
        {
            length: count,
        },
        (_, i) => ({
            id: String(i),
            title: '本地曲目 ' + i,
            artist: '测试歌手',
            kind: 'song',
            album: '测试专辑',
        }),
    );
const store = {
    lists: [songs(20), songs(20), [], [], []],
    tracks: songs(40),
    columns: ['搜索', '歌单', '专辑', '歌手', '收藏'],
    hasMore: [false, false, false, false, false],
    searchKind: 'song',
    keyword: '本地',
    status: '测试',
    loadMoreSource() {},
    loadTrackPage() {},
    search() {},
};
let opened;
const hud = new HUD(
    {
        open: (url) => (opened = url),
    },
    {
        width: (value, size) => (String(value).length * size) / 2,
    },
    store,
    {},
);
hud.w = 1920;
hud.frame = {
    mode: 'detail',
};
hud.motion.offset = 0;
store.current = {
    kind: 'playlist',
};
for (let i = 0; i < 50; i++) {
    hud.wheel(1300, 500, -15);
}
assert.equal(hud.trackOffset, 31);
hud.wheel(1300, 500, 15);
assert.equal(hud.trackOffset, 30);
hud.wheel(100, 500, -15);
assert.equal(hud.trackOffset, 30, 'Playlist sidebar must not scroll the track list');
for (const modal of ['search', 'saved']) {
    hud.modal = modal;
    hud.listOffset = 0;
    hud.wheel(1300, 500, -15);
    assert.equal(hud.listOffset, 1);
    hud.wheel(1300, 300, -15);
    hud.wheel(1300, 500, 0);
    assert.equal(hud.listOffset, 1);
    await hud.pageResults(1);
    assert.equal(hud.listOffset, 6);
    await hud.pageResults(-1);
    assert.equal(hud.listOffset, 1);
    for (let i = 0; i < 50; i++) {
        hud.wheel(1300, 500, -15);
    }
    assert.equal(hud.listOffset, 15);
    hud.commands = [];
    hud.hits = [];
    hud.modalDraw(1920);
    assert(hud.hits.some((h) => h.action === 'music:page-prev'));
    assert(!hud.hits.some((h) => h.action === 'music:page-next'));
}
hud.modal = 'settings';
hud.wheel(1300, 500, -15);
assert.equal(hud.listOffset, 15);
hud.action('music:project-home');
assert.equal(opened, 'https://github.com/MikeQu223/Rhine-Music-Database');
hud.action('music:project-author');
assert.equal(opened, 'https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced');

// 翻页必须等待实际补页完成，并忽略已切换的查询或类别。
hud.modal = 'search';
hud.listOffset = 15;
store.hasMore[0] = true;
let finish;
store.loadMoreSource = () =>
    new Promise((resolve) => {
        finish = () => {
            store.lists[0].push(...songs(10));
            resolve();
        };
    });
const next = hud.pageResults(1);
assert.equal(hud.listOffset, 15);
finish();
await next;
assert.equal(hud.listOffset, 20);
hud.listOffset = 25;
const stale = hud.pageResults(1);
let searchedKind;
store.search = (query, kind) => {
    searchedKind = kind;
    store.searchKind = kind;
};
hud.action('music:kind:1');
assert.equal(searchedKind, 'album');
finish();
await stale;
assert.equal(hud.listOffset, 0);
hud.listOffset = store.lists[0].length - 5;
let fetched = 0;
store.loadMoreSource = () => {
    fetched++;
};
hud.wheel(1300, 500, -15);
assert.equal(fetched, 1);
store.sourceLoading = true;
hud.wheel(1300, 500, -15);
assert.equal(fetched, 1);

// 搜索来源补页向调用方传递 Promise，避免按钮先滚动再收到数据。
let resolveSearch;
const promise = new Promise((resolve) => (resolveSearch = resolve));
const more = MusicStore.prototype.loadMoreSource.call(
    {
        keyword: '本地',
        hasMore: [true],
        search: () => promise,
    },
    0,
);
let complete = false;
more.then(() => (complete = true));
await Promise.resolve();
assert.equal(complete, false);
resolveSearch();
await more;
assert.equal(complete, true);
console.log('Playlist scrolling, result paging, delayed loading and project link regressions passed');
