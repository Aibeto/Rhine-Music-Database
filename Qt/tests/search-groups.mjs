import assert from 'node:assert/strict';
import { groupRows } from '../logic/groups.js';
import { API, parse, MusicStore, item } from '../logic/music.js';
import { HUD } from '../logic/hud.js';
import fs from 'node:fs';
const rows = [
    [
        7,
        parse(
            {
                id: 1,
                name: '合唱',
                ar: [
                    {
                        id: 10,
                        name: '同名',
                    },
                    {
                        id: 20,
                        name: '同名',
                    },
                    {
                        id: 10,
                        name: '同名',
                    },
                ],
                al: {
                    id: 30,
                    name: '专辑',
                },
            },
            'song',
        ),
    ],
    [
        9,
        parse(
            {
                id: 2,
                name: '独唱',
                ar: [
                    {
                        id: 10,
                        name: '同名',
                    },
                ],
                al: {
                    id: 40,
                    name: '专辑',
                },
            },
            'song',
        ),
    ],
];
const groups = groupRows(rows, 'artist');
assert.equal(groups.length, 2);
assert.deepEqual(
    groups[0][1].entries.map((e) => e[0]),
    [7, 9],
);
assert.deepEqual(
    groups[1][1].entries.map((e) => e[0]),
    [7],
);
assert.equal(groupRows(rows, 'album').length, 2, 'Identical album names must retain separate IDs');
assert.equal(
    groupRows(
        [
            [
                0,
                {
                    artist: '__proto__',
                },
            ],
        ],
        'artist',
    )[0][1].title,
    '__proto__',
);
let type;
const host = {
    acceptCookies() {},
    request(route, p, done) {
        type = p.type;
        done(
            JSON.stringify({
                body: {
                    code: 200,
                    result: {
                        artistCount: 31,
                        artists: [
                            {
                                id: 10,
                                name: '歌手',
                                picUrl: 'cover',
                            },
                        ],
                    },
                },
            }),
            '',
        );
    },
    delay(_, done) {
        done();
    },
    cover() {},
};
const api = new API(host);
const result = await api.search('歌手', 'artist', 30);
assert.equal(type, 100);
assert.equal(result.items[0].kind, 'artist');
assert.equal(result.more, false);
assert.equal(result.items[0].cover, 'cover');
const content = JSON.parse(fs.readFileSync('Resources/slots.json'));
const store = new MusicStore(
    host,
    {
        setMode() {},
        navigate() {},
        select() {},
        musicIdentity() {},
    },
    content,
);
let release;
store.api.detail = () => new Promise((resolve) => (release = resolve));
const opening = store.enterDirectory(item('artist', 10, '歌手'));
store.leaveDirectory();
release([item('artist', 10, '歌手'), [rows[0][1]], []]);
await opening;
assert.equal(store.directory, null, 'Stale artist response cannot reopen its directory');
store.api.detail = async (value) => [value, [rows[0][1]], []];
await store.enterDirectory(item('artist', 10, '歌手'));
const parent = store.directory;
await store.enterDirectory(item('album', 30, '专辑'));
store.leaveDirectory();
assert.equal(store.directory, parent);
assert.equal(store.current.id, '1');
store.api.artistAlbums = async () => ({
    items: [item('album', 30, '专辑')],
    rawCount: 1,
    more: false,
});
await store.loadArtistAlbums();
assert.equal(store.directory.albums.length, 1);
assert.equal(store.directory.albumsMore, false);
const hud = new HUD(
    {},
    {},
    {
        lists: [rows.map((e) => e[1])],
    },
    {},
);
hud.modal = 'search';
hud.groupBy = 'artist';
assert.equal(hud.rows(0).length, 2);
hud.action('music:expand:artist%3A10');
assert.equal(hud.rows(0).length, 2);
hud.action('music:group-back');
assert.equal(hud.activeGroup, null);
hud.reduced = true;
assert.deepEqual(hud.rowMotion('reduced', 4), {
    opacity: 1,
    offset: 0,
});
console.log(
    'Artist identity, coartist grouping, original result indices, album identity, artist routes, nested return and reduced motion passed',
);
store.leaveDirectory(false);
store.lists[0] = Array.from(
    {
        length: 24,
    },
    (_, i) =>
        parse(
            {
                id: i + 100,
                name: '歌曲 ' + i,
                ar: [
                    {
                        id: 10,
                        name: '歌手',
                    },
                ],
                al: {
                    id: 7,
                    name: '专辑',
                    picUrl: 'local',
                },
                dt: 180000,
            },
            'song',
        ),
);
store.setGroupView('过滤歌手', [1, 5, 9, 13, 17, 21], 0);
assert.equal(store.current.id, '101');
assert.equal(store.libraryTotal, 6);
store.selectResult(17, 0, false);
assert.equal(store.current.id, '117');
assert.equal(store.libraryIndex, 4);
store.next(1);
store.observe(store.physicalSlots[0][5]);
assert.equal(store.current.id, '121');
store.next(1);
store.observe(store.physicalSlots[0][6]);
assert.equal(store.current.id, '101', 'Filtered navigation wraps within the group');
store.setGroupView(null, [], 0);
assert.equal(store.libraryItems.length, 24);
assert.equal(store.current.id, '100');
console.log('Grouped physical slots preserve original identities and wrap only within the artist group');
