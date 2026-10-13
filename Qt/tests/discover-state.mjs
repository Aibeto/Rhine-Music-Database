import assert from 'node:assert/strict';
import { Discover } from '../logic/discover.js';
const song = (id) => ({
    id,
    name: '歌曲 ' + id,
    ar: [],
    al: {
        id: 1,
        name: '专辑',
    },
});
let route;
let release;
const s = {
    provider: 'netease',
    uid: '',
    accountGeneration: 0,
    changed() {},
    player: {
        history: [],
    },
    api: {
        async request(r) {
            route = r;
            return {
                data: {
                    dailySongs: [song(1)],
                },
            };
        },
    },
};
const d = new Discover(s);
await d.load('daily');
assert.match(d.status, /登录/);
s.uid = '1';
await d.load('daily');
assert.equal(d.items[0].kind, 'song');
assert.equal(route, '/recommend/songs');
s.api.request = () => new Promise((resolve) => (release = resolve));
const late = d.load('charts');
d.cancel();
release({
    list: [
        {
            id: 4,
            name: '旧榜单',
        },
    ],
});
await late;
assert.equal(d.items.length, 0);
s.provider = 'qq';
let tab;
s.api.discover = async (value) => {
    tab = value;
    return {
        items: [
            {
                provider: 'qq',
                kind: 'playlist',
                id: '4',
                key: 'qq:playlist:4',
            },
        ],
        rawCount: 1,
        more: false,
    };
};
await d.load('playlists');
assert.equal(tab, 'playlists');
assert.equal(d.items[0].provider, 'qq');
s.api.discover = () => new Promise((resolve) => (release = resolve));
const old = d.load('daily');
d.cancel();
release({
    items: [
        {
            id: 'stale',
        },
    ],
    rawCount: 1,
});
await old;
assert.equal(d.items.length, 0);
console.log('Platform discovery, authenticated daily recommendations and stale cancellation passed offline');
