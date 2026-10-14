import assert from 'node:assert/strict';
import fs from 'node:fs';
import { MusicStore, item, API, parse, time } from '../logic/music.js';
import { ReelCurve, SurfaceMotion } from '../logic/motion.js';
const pause = (ms) => new Promise((r) => setTimeout(r, ms));
const content = JSON.parse(fs.readFileSync('Resources/slots.json'));
const song = (id) => ({
    id,
    name: '歌曲 ' + id,
    ar: [
        {
            name: '歌手',
        },
    ],
    al: {
        id: 100,
        name: '专辑',
    },
    dt: 180000,
});
function fixture() {
    const host = {
        epoch: 0,
        cookies: {},
        saved: 0,
        acceptCookies(raw, epoch) {
            if (epoch === this.epoch) {
                for (const line of JSON.parse(raw)) {
                    const [key, ...rest] = line.split(';')[0].split('=');
                    this.cookies[key] = rest.join('=');
                }
            }
        },
        request(route, params, done) {
            const epoch = this.epoch;
            const r = this.respond(route, params);
            setTimeout(
                () =>
                    done(
                        JSON.stringify({
                            _epoch: epoch,
                            ...(r.body
                                ? {
                                      body: r.body,
                                      cookies: r.cookies || [],
                                  }
                                : {
                                      body: r,
                                  }),
                        }),
                        '',
                    ),
                r.delay || 0,
            );
        },
        delay(ms, done) {
            setTimeout(done, ms === 1000 ? 5 : ms);
        },
        authenticated() {
            return !!this.cookies.MUSIC_U;
        },
        saveCredential() {
            this.saved++;
            return '';
        },
        logout() {
            this.epoch++;
            this.cookies = {};
        },
        cover() {},
        qr(key) {
            return 'qr:' + key;
        },
        play(id, done) {
            done('OFFLINE_PLAY:' + id);
        },
        fromLink(input) {
            try {
                let u = new URL(input.replaceAll('\\', '').trim());
                if (
                    !['music.163.com', 'y.music.163.com'].includes(u.hostname) ||
                    !['http:', 'https:'].includes(u.protocol)
                ) {
                    return '';
                }
                if (u.hash) {
                    u = new URL('https://music.163.com/' + u.hash.slice(1).replace(/^\//, ''));
                }
                const kind = u.pathname.split('/').at(-1);
                const id = u.searchParams.get('id');
                return ['song', 'album', 'playlist'].includes(kind) && /^[0-9]{1,20}$/.test(id)
                    ? kind + ':' + id
                    : '';
            } catch {
                return '';
            }
        },
    };
    let selected = 0;
    let mode = 'archive';
    const runtime = {
        select(i) {
            selected = i;
        },
        setMode(value) {
            mode = value;
        },
        musicIdentity() {},
        navigate(axis, dir) {
            const lane = content.columns.indexOf(content.records[selected].category);
            const slots = content.records
                .map((r, i) => (r.category === content.columns[lane] ? i : -1))
                .filter((i) => i >= 0);
            const row = slots.indexOf(selected);
            if (axis === 'row') {
                selected = slots[(row + dir + 8) % 8];
            } else {
                selected = content.records.findIndex(
                    (r) => r.category === content.columns[(lane + dir + 5) % 5],
                );
            }
        },
        get selected() {
            return selected;
        },
        get mode() {
            return mode;
        },
    };
    const store = new MusicStore(host, runtime, content);
    store.frame = {
        selected,
        mode,
    };
    host.respond = () => ({
        code: 200,
    });
    return {
        host,
        runtime,
        store,
    };
}
let f = fixture();
let searches = 0;
{
    const t = fixture();
    const loaded = parse(song(41), 'song');
    t.store.directory = {
        ids: ['41', '42', '43'],
        songs: [loaded],
    };
    const queue = t.store.directoryQueue();
    assert.deepEqual(
        queue.map((value) => value.id),
        ['41', '42', '43'],
    );
    assert.equal(queue[0], loaded);
    assert.equal(queue[1].deferred, true);
    const plays = [];
    t.store.player.play = (...args) => plays.push(args);
    t.store.current = loaded;
    t.store.play();
    assert.deepEqual(plays[0], [loaded]);
    t.store.playDirectory();
    assert.deepEqual(plays[1], [loaded, queue]);
    console.log('Full playlist queues retain all IDs before paged metadata is loaded');
}
f.host.respond = (route) =>
    route === '/search'
        ? {
              delay: ++searches === 1 ? 100 : 1,
              body: {
                  code: 200,
                  result: {
                      songCount: 1,
                      songs: [song(searches === 1 ? 42 : 43)],
                  },
              },
          }
        : {
              code: 200,
          };
f.store.search('旧搜索');
await pause(5);
f.store.search('新搜索');
await pause(120);
assert.equal(f.store.current.id, '43');
assert.equal(f.store.keyword, '新搜索');
assert.equal(f.store.sourceLoading, false);
f.store.search('未完成');
f.store.search('');
await pause(10);
assert.equal(f.store.sourceLoading, false);
console.log('Slow searches and invalidated empty searches preserve latest state');
f.host.respond = (route) =>
    route === '/user/playlist'
        ? {
              delay: 10,
              body: {
                  code: 200,
                  playlist: [
                      {
                          id: 1,
                          name: '自己创建',
                          creator: {
                              userId: 7,
                              nickname: '同名',
                          },
                      },
                      {
                          id: 2,
                          name: '收藏',
                          creator: {
                              userId: 8,
                              nickname: '同名',
                          },
                      },
                  ],
              },
          }
        : {
              code: 200,
          };
f.store.uid = '7';
f.store.nickname = '同名';
await f.store.loadMoreSource(2);
assert.deepEqual(
    f.store.lists[2].map((x) => x.id),
    ['1'],
);
assert.deepEqual(
    f.store.lists[3].map((x) => x.id),
    ['2'],
);
f.store.hasMore[2] = true;
f.store.loadMoreSource(2);
await pause(1);
f.store.logout();
await pause(20);
assert.equal(f.store.uid, '');
assert(f.store.lists.slice(1).every((x) => !x.length));
assert.equal(f.store.sourceLoading, false);
console.log('Personal playlists use creator UID and remain cleared after logout');
f = fixture();
f.host.respond = (route, params) =>
    route === '/song/detail'
        ? {
              delay: params.ids === '42' ? 100 : 1,
              body: {
                  code: 200,
                  songs: [song(Number(params.ids))],
              },
          }
        : {
              code: 200,
              data: {
                  blocks: [],
              },
          };
f.store.choose(item('song', '42', '旧歌曲', '歌手'));
await pause(320);
f.store.choose(item('song', '43', '新歌曲', '歌手'));
await pause(420);
assert.equal(f.store.current.id, '43');
assert.equal(f.store.current.title, '歌曲 43');
assert.equal(f.store.detailLoading, false);
console.log('Rapid song changes discard stale details');
{
    const t = fixture();
    let details = 0;
    t.host.delay = (ms, done) => setTimeout(done, 0);
    t.host.respond = (route) => {
        if (route === '/song/detail') {
            {
                details++;
                return {
                    code: 200,
                    songs: [
                        {
                            ...song(42),
                            al: {
                                id: 100,
                                name: '专辑',
                                picUrl: 'https://p1.music.126.net/local-cover.jpg',
                            },
                        },
                    ],
                };
            }
        } else {
            return {
                code: 200,
            };
        }
    };
    t.host.cover = (url, done) => {
        if (url) {
            done(url);
        }
    };
    const sparse = parse(song(42), 'song');
    await t.store.choose(sparse);
    await pause(10);
    assert.equal(details, 1);
    assert.equal(t.store.coverImage, 'https://p1.music.126.net/local-cover.jpg');
    await t.store.choose(sparse);
    await pause(10);
    assert.equal(details, 1);
    t.host.respond = () => {
        details++;
        return {
            code: 200,
            songs: [song(43)],
        };
    };
    await t.store.choose(parse(song(43), 'song'));
    await t.store.choose(parse(song(43), 'song'));
    assert.equal(details, 2);
    console.log('Sparse search songs fetch artwork once; cached details avoid repeat requests');
}
f.store.lists[0] = Array.from(
    {
        length: 20,
    },
    (_, i) => ({
        ...item('song', 100 + i, '曲目', ''),
        duration: 1,
        albumID: '100',
    }),
);
f.store.offsets[0] = 0;
f.store.hasMore[0] = false;
f.store.selectedPhysical = -1;
f.runtime.select(f.store.physicalSlots[0][0]);
f.store.observe(f.store.physicalSlots[0][0]);
for (let i = 0; i < 9; i++) {
    f.store.next(1);
    f.store.observe(f.runtime.selected);
}
assert.equal(f.store.current.id, '109');
assert.equal(f.store.offsets[0], 8);
f.store.selectResult(0, 0);
f.store.next(-1);
f.store.observe(f.runtime.selected);
assert.equal(f.store.current.id, '119');
f.store.next(1);
f.store.observe(f.runtime.selected);
assert.equal(f.store.current.id, '100');
console.log('Eight physical slots page and wrap through 20 distinct songs');
f = fixture();
let missing = 0;
f.host.respond = (route) => {
    if (route === '/playlist/detail') {
        return {
            code: 200,
            playlist: {
                id: 900,
                name: '部分曲目',
                tracks: [song(1)],
                trackIds: [
                    {
                        id: 1,
                    },
                    {
                        id: 2,
                    },
                    {
                        id: 3,
                    },
                ],
            },
        };
    } else if (route === '/song/detail') {
        {
            missing++;
            return {
                code: 200,
                songs: [song(3)],
            };
        }
    } else {
        return {
            code: 200,
        };
    }
};
await f.store.choose(item('playlist', 900, '部分曲目', ''), true);
await f.store.loadTrackPage(0);
assert.equal(f.store.trackCursor, 3);
assert.deepEqual(
    f.store.tracks.map((x) => x.id),
    ['1', '3'],
);
await f.store.loadTrackPage(0);
assert.equal(missing, 1);
console.log('Unavailable tracks consume IDs once and never cause retry loops');
f = fixture();
f.host.respond = (route) => {
    if (route === '/playlist/detail') {
        return {
            code: 200,
            playlist: {
                id: 900,
                name: '目录',
                trackCount: 60,
                tracks: Array.from(
                    {
                        length: 45,
                    },
                    (_, i) => song(i + 1),
                ),
                trackIds: Array.from(
                    {
                        length: 60,
                    },
                    (_, i) => ({
                        id: i + 1,
                    }),
                ),
            },
        };
    } else if (route === '/song/detail') {
        return {
            code: 200,
            songs: Array.from(
                {
                    length: 15,
                },
                (_, i) => song(46 + i),
            ),
        };
    } else {
        return {
            code: 200,
        };
    }
};
f.store.selectedPhysical = f.store.physicalSlots[0][3];
f.store.selectedLane = 0;
await f.store.enterDirectory(item('playlist', 900, '目录', ''));
assert.equal(f.store.directory.songs.length, 45);
f.store.selectDirectory(44);
assert.equal(f.store.current.id, '45');
assert.equal(f.store.directory.page, 40);
await f.store.loadDirectoryPage(45);
assert.equal(f.store.directory.songs.length, 60);
assert.equal(f.store.directory.cursor, 60);
assert.equal(f.store.current.id, '46');
f.store.showDirectoryInfo();
assert.equal(f.store.current.kind, 'playlist');
assert.equal(f.store.tracks.length, 60);
f.store.returnToDirectory();
assert.equal(f.store.current.id, '46');
f.store.leaveDirectory();
assert.equal(f.store.directory, null);
assert.equal(f.store.selectedPhysical, f.store.physicalSlots[0][3]);
assert.equal(f.runtime.mode, 'archive');
console.log('Directory paging, container details and parent restoration preserve physical identity');
f = fixture();
f.host.respond = (route) =>
    route === '/playlist/detail'
        ? {
              delay: 50,
              body: {
                  code: 200,
                  playlist: {
                      id: 900,
                      name: '旧目录',
                      tracks: [song(1)],
                  },
              },
          }
        : {
              code: 200,
          };
f.store.enterDirectory(item('playlist', 900, '旧目录', ''));
await pause(1);
f.store.leaveDirectory();
await pause(70);
assert.equal(f.store.directory, null);
assert.equal(f.store.directoryLoading, false);
console.log('Late directory responses cannot reopen a closed directory');
f = fixture();
let polls = 0;
f.host.respond = (route) => {
    if (route === '/login/qr/key') {
        return {
            code: 200,
            data: {
                unikey: 'local-key',
            },
        };
    } else if (route === '/login/qr/check') {
        return {
            body: {
                code: ++polls === 1 ? 802 : 803,
            },
            cookies: ['MUSIC_U=local-auth; Path=/', 'DEVICE=local; Path=/'],
        };
    } else if (route === '/user/account') {
        return {
            code: 200,
            profile: {
                userId: 7,
                nickname: '本地用户',
            },
        };
    } else {
        return {
            code: 200,
            playlist: [],
        };
    }
};
await f.store.startLogin();
assert.equal(f.store.uid, '7');
assert.equal(f.store.nickname, '本地用户');
assert.equal(f.host.saved, 1);
assert.equal(f.host.cookies.MUSIC_U, 'local-auth');
assert.equal(f.host.cookies.DEVICE, 'local');
assert.equal(f.store.qr, '');
console.log('801/802/803 QR flow keeps all cookies and saves only confirmed credentials');
f = fixture();
f.host.respond = (route) => {
    if (route === '/login/qr/key') {
        return {
            code: 200,
            data: {
                unikey: 'local-key',
            },
        };
    } else if (route === '/login/qr/check') {
        return {
            delay: 25,
            body: {
                code: 803,
            },
            cookies: ['MUSIC_U=stale; Path=/'],
        };
    } else {
        return {
            code: 200,
            profile: {
                userId: 7,
                nickname: '旧账号',
            },
        };
    }
};
f.store.startLogin();
await pause(10);
f.store.cancelLogin();
await pause(40);
assert.equal(f.store.uid, '');
assert.equal(f.host.saved, 0);
assert.equal(f.host.authenticated(), false);
console.log('Closing QR login rejects stale cookies before they reach account state');
f = fixture();
f.host.respond = () => ({
    delay: 20,
    body: {
        code: 200,
    },
    cookies: ['MUSIC_U=old; Path=/'],
});
const request = f.store.api.request('/user/account');
f.host.logout();
await request;
assert.equal(f.host.authenticated(), false);
assert.equal(f.host.fromLink('https://music.163.com/#/album?id=398969728'), 'album:398969728');
assert.equal(f.host.fromLink('https://music.163.com.example.org/song?id=1'), '');
assert.equal(time(parse(song(1), 'song')), '03:00');
assert.equal(
    parse(
        {
            id: 'bad',
            name: 'x',
        },
        'song',
    ),
    null,
);
const reel = new ReelCurve(0, 1, 0, 0);
assert.deepEqual(reel.sample(0.46), [1, 0]);
const motion = new SurfaceMotion();
motion.set(true, 0);
motion.sample(0.15);
const before = motion.opacity;
motion.set(false, 0.15);
assert.equal(motion.opacity, before);
motion.sample(0.351);
assert.equal(motion.finished, true);
motion.set(true, 1, true);
assert.equal(motion.opacity, 1);
console.log('Cookie epochs, exact host validation, duration and interrupted motion passed');
// 分类搜索只更新结果与档案选中项，不能沿用旧的详情模式打开空白资料页。
for (const kind of ['album', 'playlist', 'artist']) {
    const t = fixture();
    t.runtime.setMode('detail');
    t.store.frame = {
        mode: 'detail',
    };
    const fields = {
        album: 'albums',
        playlist: 'playlists',
        artist: 'artists',
    };
    t.host.respond = (route) =>
        route === '/search'
            ? {
                  code: 200,
                  result: {
                      [fields[kind]]: [
                          {
                              id: 82,
                              name: '本地分类结果',
                          },
                      ],
                      [kind + 'Count']: 1,
                  },
              }
            : {
                  code: 200,
              };
    await t.store.search('分类验收', kind);
    assert.equal(t.runtime.mode, 'archive');
    assert.equal(t.store.frame.mode, 'archive');
    assert.equal(t.store.directory, null);
}
console.log('Album, playlist and artist searches reset stale detail mode without opening directories');

// 下一首、队列跳播和跨资料库播放都更新详情，同时保留原资料库返回路径。
{
    const t = fixture();
    const values = [parse(song(501), 'song'), parse(song(502), 'song')];
    const selections = [];
    const identities = [];
    t.runtime.select = (physical, navigation) =>
        selections.push({
            physical,
            navigation,
        });
    t.runtime.musicIdentity = (key) => identities.push(key);
    t.host.mediaLoad = () => true;
    t.host.mediaControl = () => {};
    t.host.mediaState = () => ({
        ended: 0,
    });
    t.host.respond = (route) =>
        route === '/song/url/v1'
            ? {
                  code: 200,
                  data: values.map((value) => ({
                      id: Number(value.id),
                      url: 'https://m.music.126.net/local',
                  })),
              }
            : {
                  code: 200,
              };
    t.store.current = parse(song(500), 'song');
    t.store.selectedPhysical = t.store.physicalSlots[0][0];
    await t.store.player.play(values[0], values);
    assert.equal(t.store.current.id, '501');
    assert.equal(t.runtime.mode, 'detail');
    assert.equal(t.store.frame.mode, 'detail');
    assert.equal(t.store.directory.parentItem.id, '500');
    t.store.player.next(1);
    await pause(10);
    assert.equal(t.store.current.id, '502');
    assert.equal(t.store.player.current.id, '502');
    assert.equal(t.store.directory.index, 1);
    assert.equal(identities.at(-1), 'netease:song:502');
    assert.notEqual(selections[0].physical, selections[1].physical);
    const count = selections.length;
    await t.store.player.play(values[1]);
    assert.equal(selections.length, count, 'Same-song replay should not return the disc');
    await t.store.player.play(values[0]);
    assert.equal(t.store.current.id, '501');
    assert.equal(t.store.directory.index, 0);
    t.store.player.stop();
}
console.log('Playback changes synchronize detail, model identity and archive selection in both directions');
