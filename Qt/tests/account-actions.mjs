import assert from 'node:assert/strict';
import fs from 'node:fs';
import { MusicStore, parse } from '../logic/music.js';
const content = JSON.parse(fs.readFileSync('Resources/slots.json'));
let sent = [];
let invalidations = 0;
const host = {
    delay() {},
    cover() {},
    mediaControl() {},
    logout() {},
    invalidateMetadata() {
        invalidations++;
    },
    saveCredential: () => '',
    acceptCookies() {},
    request(route, p, cb) {
        sent.push([route, p]);
        cb(
            JSON.stringify({
                body: {
                    code: 200,
                    profile: {
                        userId: 42,
                        nickname: '本地用户',
                    },
                },
            }),
            '',
        );
    },
};
const store = new MusicStore(
    host,
    {
        musicIdentity() {},
        select() {},
        setMode() {},
    },
    content,
);
const song = parse(
    {
        id: 1,
        name: '歌曲',
        ar: [],
        al: {
            id: 2,
            name: '专辑',
        },
    },
    'song',
);
store.current = song;
await store.like();
assert.equal(sent.length, 0);
store.uid = '42';
store.likedIDs = [];
await store.like();
assert(store.likedIDs.includes('1'));
await store.like();
assert(!store.likedIDs.includes('1'));
assert.equal(invalidations, 2);
const album = parse(
    {
        id: 2,
        name: '专辑',
    },
    'album',
);
await store.subscribe(album);
assert.equal(store.lists[4].length, 1);
await store.subscribe(album);
assert.equal(store.lists[4].length, 0);
store.lists[2] = [
    parse(
        {
            id: 10,
            name: '我的歌单',
            creator: {
                userId: 42,
            },
        },
        'playlist',
    ),
];
await store.playlistForm('rename', '新名称', '10');
assert.equal(store.lists[2][0].title, '新名称');
const n = sent.length;
await store.playlistForm('rename', '非法', '11');
assert.equal(sent.length, n);
await store.playlistTrack('10', song);
assert.equal(sent.at(-1)[0], '/playlist/tracks');
assert.equal(sent.at(-1)[1].op, 'add');
store.loadMoreSource = () => {};
await store.passwordLogin('phone', '13800000000', 'fixture-password');
assert.equal(store.uid, '42');
assert.equal(store.identityName, '本地用户');
assert.equal(sent.find((v) => v[0] === '/login/cellphone')[1].phone, '13800000000');
let release;
store.api.request = () => new Promise((resolve) => (release = resolve));
const late = store.passwordLogin('email', 'fixture@example.test', 'fixture-password');
store.logout();
release({
    code: 200,
});
await late;
assert.equal(store.uid, '');
assert.equal(store.writeLoading, false);
assert.equal(store.subscriptions.size, 0);
console.log(
    'Likes/subscriptions, owned playlist edits, phone login and stale login after logout passed with local responses',
);

// 历史用户名按来源持久保存，退出和切换来源不清除身份确认行记录。
const preferences = new Map();
host.preference = (key) => preferences.get(key) || '';
host.setPreference = (key, value) => preferences.set(key, value);
const identityStore = new MusicStore(host, store.runtime, content);
identityStore.rememberIdentityName('qq', 'QQ 用户');
assert.equal(identityStore.identityName, 'QQ 用户');
identityStore.rememberIdentityName('netease', '网易云用户');
assert.equal(identityStore.identityName, '网易云用户');
identityStore.logout();
assert.equal(identityStore.identityName, '网易云用户');
const restored = new MusicStore(host, {}, content);
assert.equal(restored.identityName, '网易云用户');
preferences.clear();
const legacy = new MusicStore(host, {}, content);
assert.equal(legacy.identityName, '');
legacy.provider = 'qq';
host.hasCredential = (provider) => provider === 'netease';
await legacy.restoreIdentityName();
assert.equal(legacy.identityName, '本地用户');
assert.equal(legacy.provider, 'qq');
assert.equal(legacy.uid, '');
assert.equal(preferences.get('welcome-name-netease'), '本地用户');
console.log('Identity names persist across restart and logout, with NetEase priority and legacy backfill');

// 身份确认行补齐请求不得在账号已变化后写回历史用户名。
preferences.clear();
const staleIdentity = new MusicStore(host, {}, content);
staleIdentity.provider = 'qq';
let finishAccount;
const originalRequest = host.request;
host.request = (_route, _params, callback) => {
    finishAccount = callback;
};
const pendingIdentity = staleIdentity.restoreIdentityName();
++staleIdentity.accountGeneration;
finishAccount(JSON.stringify({ body: { code: 200, profile: { userId: 7, nickname: '迟到用户' } } }), '');
await pendingIdentity;
host.request = originalRequest;
assert.equal(staleIdentity.identityName, '');
assert.equal(preferences.size, 0);
