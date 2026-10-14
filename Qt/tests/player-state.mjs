import assert from 'node:assert/strict';
import { Player } from '../logic/player.js';
const calls = [];
const pending = [];
let native = {
    playing: false,
    position: 0,
    duration: 10000,
    seekable: true,
    volume: 0.7,
    ended: 0,
};
const host = {
    mediaControl: (...v) => calls.push(v),
    mediaLoad: (url) => {
        calls.push(['load', url]);
        return true;
    },
    mediaState: () => native,
};
const api = {
    request: (route, values) =>
        new Promise((resolve, reject) =>
            pending.push({
                route,
                values,
                resolve,
                reject,
            }),
        ),
};
const p = new Player(host, api);
const song = (id) => ({
    id: String(id),
    kind: 'song',
    title: 'Song ' + id,
});
const a = song(1);
const b = song(2);
const one = p.play(a, [a, b, a]);
const two = p.play(b);
assert.equal(p.queue.length, 2);
pending
    .find((v) => v.route === '/song/url/v1' && v.values.id === '2')
    .resolve({
        data: [
            {
                id: 2,
                url: 'https://m.music.126.net/two',
            },
        ],
    });
await two;
pending
    .find((v) => v.route === '/song/url/v1' && v.values.id === '1')
    .resolve({
        data: [
            {
                id: 1,
                url: 'https://m.music.126.net/one',
            },
        ],
    });
await one;
assert.deepEqual(
    calls.filter((v) => v[0] === 'load'),
    [['load', 'https://m.music.126.net/two']],
);
assert(pending.every((v) => v.route === '/song/url/v1'));
p.tick(1);
p.seek(2);
p.volume(-1);
assert.deepEqual(calls.slice(-2), [
    ['seek', 10000],
    ['volume', 0],
]);
native = {
    ...native,
    position: 1500,
};
p.tick(2);
assert.equal(p.state.position, 1500);
p.mode = 'off';
native = {
    ...native,
    ended: 1,
};
p.tick(3);
assert.equal(p.status, '队列已播放完');
const count = pending.length;
p.tick(4);
assert.equal(pending.length, count);
p.enqueue(song(3), true);
assert.deepEqual(
    p.queue.map((v) => v.id),
    ['1', '2', '3'],
);
p.remove(0);
assert.equal(p.index, 0);
p.remove(0);
assert.equal(p.queue.length, 2);
p.clearUpcoming();
assert.deepEqual(p.queue, [b]);
p.mode = 'one';
native = {
    ...native,
    ended: 2,
};
p.tick(5);
assert(p.loading);
assert.equal(p.current.id, '2');
p.stop();
for (const request of pending.filter((v) => v.values.id === '2')) {
    request.resolve({
        data: [
            {
                id: 2,
                url: 'https://m.music.126.net/stale',
            },
        ],
    });
}
await Promise.resolve();
assert.equal(p.current, null);
assert.equal(p.queue.length, 0);
assert.equal(p.loading, false);
console.log('Stale URL cancellation, ordered queue, bounds, end events and logout cancellation passed');
// 完整歌单保留未加载曲目的 ID，切到该曲目时补全信息；切歌后的迟到信息不能覆盖新曲目。
const metadata = [];
api.songs = (ids, valid) =>
    new Promise((resolve) =>
        metadata.push({
            ids,
            valid,
            resolve,
        }),
    );
const deferred = {
    ...song(4),
    deferred: true,
};
const waiting = p.play(deferred, [a, deferred]);
assert.deepEqual(metadata[0].ids, ['4']);
assert.equal(p.queue.length, 2);
metadata[0].resolve([
    {
        ...song(4),
        title: '完整信息',
    },
]);
await Promise.resolve();
pending
    .find((v) => v.route === '/song/url/v1' && v.values.id === '4')
    .resolve({
        data: [
            {
                id: 4,
                url: 'https://m.music.126.net/four',
            },
        ],
    });
await waiting;
assert.equal(p.current.title, '完整信息');
assert.equal(p.queue[1].deferred, undefined);
const stale = p.play({
    ...song(5),
    deferred: true,
});
p.stop();
metadata[1].resolve([song(5)]);
await stale;
assert.equal(p.current, null);
assert.equal(metadata[1].valid(), false);
console.log('Deferred full-playlist metadata loads on demand and cannot revive playback after stop');

// 已有曲目重新排序、当前曲目再次排入待播区，都不能中断当前音源。
const ordered = new Player(host, {
    playback: async () => ({ url: 'https://m.music.126.net/local' }),
});
const c = song(3);
const d = song(4);
await ordered.play(b, [a, b, c, d]);
const generation = ordered.generation;
ordered.enqueue(d, true);
assert.deepEqual(
    ordered.queue.map((value) => value.id),
    ['1', '2', '4', '3'],
);
ordered.enqueue(a);
assert.deepEqual(
    ordered.queue.map((value) => value.id),
    ['2', '4', '3', '1'],
);
assert.equal(ordered.index, 0);
assert.equal(ordered.current, b);
assert.equal(ordered.generation, generation);
ordered.enqueue(b, true);
assert.deepEqual(
    ordered.queue.map((value) => value.id),
    ['2', '2', '4', '3', '1'],
);
ordered.next();
assert.equal(ordered.index, 1);
ordered.next();
assert.equal(ordered.current, d);
assert.equal(ordered.index, 2);
ordered.enqueue(b);
assert.equal(ordered.current, d);
assert.equal(ordered.queue[ordered.index], d);
console.log('Existing queue items move, current tracks can repeat, and progression preserves queue indices');
