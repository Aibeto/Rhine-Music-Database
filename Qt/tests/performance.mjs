import assert from 'node:assert/strict';
import { MusicStore } from '../logic/music.js';
import { HUD } from '../logic/hud.js';
import { RollingLabel } from '../logic/motion.js';
let measurements = 0;
let navigated = 0;
let nativeWheel = 0;
const songs = Array.from(
    {
        length: 2000,
    },
    (_, i) => ({
        key: 'song:' + i,
        title: '歌曲 ' + i,
        artist: '测试歌手',
        kind: 'song',
    }),
);
const music = {
    lists: [songs],
    revision: 0,
    next: (step) => (navigated += step),
};
const glyphs = {
    width: (text, size) => {
        measurements++;
        return (text.length * size) / 2;
    },
};
const hud = new HUD({}, glyphs, music, {
    pointer: () => nativeWheel++,
});
const title = '用于验证长标题缓存以及截断边界的曲目名称'.repeat(100);
const truncated = hud.fit(title, 16, 120);
assert(truncated.endsWith('…'));
assert(measurements < 20, 'long-title fit must avoid linear rescanning');
const measured = measurements;
for (let i = 0; i < 200; i++) {
    assert.equal(hud.fit(title, 16, 120), truncated);
}
assert.equal(measurements, measured, 'unchanged layouts must reuse measurements');
for (let i = 0; i < 600; i++) {
    hud.fit('title ' + i, 16, 120);
}
assert(hud.fitCache.size <= 512, 'layout cache stays bounded');
hud.modal = 'saved';
hud.query = '歌曲 199';
const rows = hud.rows(0);
assert.equal(rows.length, 11);
assert.equal(hud.rows(0), rows, 'unchanged large lists reuse filtering');
songs[199].title = '已修改';
music.revision++;
assert.equal(hud.rows(0).length, 10, 'in-place metadata edits invalidate cached filtering');
hud.query = '已修改';
assert.equal(hud.rows(0).length, 1, 'query changes invalidate filtering');
hud.frame = {
    mode: 'archive',
};
hud.modal = null;
hud.wheel(0, 0, -2, 0);
assert.equal(navigated, 0);
assert.equal(nativeWheel, 0, 'sub-threshold wheel must not rotate archive');
hud.wheel(0, 0, -45, 3);
assert.equal(navigated, 3, 'merged wheel preserves final selection');
const label = new RollingLabel();
hud.commands = [];
hud.ink;
label.render(hud, '123', 0, 0, 16, 'regular', 0, 0, true, true);
label.render(hud, '456', 0, 0, 16, 'regular', 0, 0.1, true, true);
assert(label.active(0.2), 'interrupted numeric animation remains active');
hud.commands = [];
label.render(hud, '456', 0, 0, 16, 'regular', 0, 1, true, true);
assert.deepEqual(
    hud.commands.map((command) => command[0]),
    ['text'],
    'settled rolling text renders as one whole line',
);
console.log('Bounded text cache, list invalidation, coalesced wheel and settled rolling text passed');
const mergedStore = {
    physicalSlots: [
        Array.from(
            {
                length: 8,
            },
            (_, i) => i,
        ),
    ],
    selectedLane: 0,
    selectedPhysical: 1,
    offsets: [0],
    hasMore: [false],
    sourceItems: () => songs,
    changed() {},
    call(axis, kind, direction) {
        assert.equal(direction, -25);
    },
};
MusicStore.prototype.next.call(mergedStore, -25);
assert.equal(
    mergedStore.offsets[0],
    1976,
    'large negative merged deltas wrap the source and physical row correctly',
);
MusicStore.prototype.navigateDirectory.call(
    {
        directory: {
            songs,
            ids: songs,
            index: 2,
            cursor: songs.length,
        },
        selectDirectory(index, mode, axis, direction) {
            assert.deepEqual([index, mode, axis, direction], [1977, 'archive', 'row', -25]);
        },
    },
    -25,
    'row',
);
