import assert from 'node:assert/strict';
import { HUD } from '../logic/hud.js';

// 验证不同窗口比例、署名揭示阶段下，入口仍独立可见并保持可点击。
for (const width of [960, 1440, 1920, 2560]) {
    for (const poweredLetters of [0, 5, 19]) {
        let mode;
        const hud = new HUD(
            {},
            {},
            {},
            {
                setMode(value) {
                    mode = value;
                },
            },
        );
        const drawing = hud.draw(
            {
                mode: 'boot',
                boot: {
                    poweredLetters,
                },
            },
            width,
            0,
        );
        const entry = drawing.hits.find((hit) => hit.action === 'skip');
        assert(entry && entry.x >= 0 && entry.x + entry.width <= width);
        assert(entry.y >= 0 && entry.y + entry.height <= 1080);
        const label = drawing.commands.find((op) => op[0] === 'text' && op[1] === 'ENTER SYSTEM ↗');
        if (poweredLetters) {
            for (const op of drawing.commands.filter(
                (op) => op[0] === 'text' && /POWERED BY|RHINE LAB/.test(op[1]),
            )) {
                assert(entry.y > op[3] + op[4], 'Entry hit region overlaps the attribution');
                assert(label[3] > op[3] + op[4], 'Entry label overlaps the attribution');
            }
        }
        hud.action('skip');
        assert.equal(mode, 'archive');
    }
}
console.log('Boot entry and attribution remain separate across viewport sizes and reveal stages');

// 长标题、窄窗口和弹窗切换都不能把按钮或固定播放栏挤进内容区。
const glyphs = {
    width: (value, size) => String(value).length * size * 0.55,
    inset: () => 0,
    wrap: (value, width, size) => {
        const n = Math.max(1, Math.floor(width / (size * 0.55)));
        return String(value).match(new RegExp('.{1,' + n + '}', 'g')) || [];
    },
};
for (const width of [1440, 1728, 1920, 2560]) {
    const song = {
        kind: 'song',
        id: '1',
        title: '长歌曲标题 / Brand New Sky（新世界的天空）',
        artist: '测试歌手',
        album: '测试专辑',
        artists: [
            {
                id: '2',
                name: '测试歌手',
            },
        ],
        mvID: '3',
        wiki: '本地百科',
        duration: 180000,
    };
    const rows = Array.from(
        {
            length: 40,
        },
        (_, i) => ({
            ...song,
            id: String(i + 1),
        }),
    );
    const store = {
        current: {
            kind: 'playlist',
            id: '8',
            title: '《鸣潮》巡回演唱会「致予新世界」上海歌单 / 长标题',
            artist: '测试歌手',
        },
        tracks: rows,
        trackIDs: rows.map((v) => v.id),
        libraryTotal: 48,
        libraryName: '本地资料库',
        status: '本地测试',
        lists: [rows, [], [], [], []],
        searchKind: 'song',
        columns: ['搜索', '喜欢', '歌单', '专辑', '歌手'],
        hasMore: [false, false, false, false, false],
        player: {
            mode: 'all',
            current: song,
            state: {
                duration: 180000,
                position: 5000,
                volume: 0.7,
            },
            queue: [],
        },
    };
    const hud = new HUD({}, glyphs, store, {});
    hud.w = width;
    hud.navigation(width);
    const discovery = hud.hits.find((hit) => hit.action === 'discover');
    const search = hud.hits.find((hit) => hit.action === 'search');
    assert.equal(discovery.y, search.y);
    assert.equal(discovery.height, search.height);
    assert(discovery.x + discovery.width <= search.x);
    assert(!hud.hits.some((hit) => hit.action === 'provider'));
    hud.hits = [];
    hud.frame = {
        mode: 'detail',
    };
    hud.container(width, 1);
    const play = hud.hits.find((v) => v.action === 'music:play-loaded');
    const track = hud.hits.find((v) => v.action === 'music:track:0');
    assert(play.height >= 44 && play.width >= 180);
    assert(play.y + play.height < track.y);
    assert(hud.hits.filter((v) => v.action.startsWith('music:track:')).every((v) => v.y + v.height < 956));
    store.current = song;
    hud.commands = [];
    hud.hits = [];
    hud.detail(width, 1);
    const actions = hud.hits.filter((v) =>
        [
            'music:like',
            'music:add-to-playlist',
            'music:artist:0',
            'music:play',
            'music:enqueue',
            'music:play-next',
            'music:official',
        ].includes(v.action),
    );
    assert(actions.every((v) => v.height >= 44 && v.x + v.width <= width - 58));
    for (let i = 0; i < actions.length; i++) {
        for (let j = i + 1; j < actions.length; j++) {
            const a = actions[i];
            const b = actions[j];
            assert(
                a.x + a.width <= b.x ||
                    b.x + b.width <= a.x ||
                    a.y + a.height <= b.y ||
                    b.y + b.height <= a.y,
                'Detail action regions overlap',
            );
        }
    }
    for (const kind of ['song', 'album', 'playlist', 'artist']) {
        store.searchKind = kind;
        store.lists[0] = rows.map((v) => ({
            ...v,
            kind,
        }));
        store.suggestions = ['不应显示的提示'];
        store.keyword = '本地';
        hud.modal = 'search';
        hud.query = '本地';
        hud.reduced = true;
        const draw = hud.draw(
            {
                mode: 'detail',
                detail: 1,
            },
            width,
            1,
        );
        assert(
            !draw.hits.some((v) => v.action === 'music:play-loaded' || v.action.startsWith('music:hint:')),
        );
        assert(!draw.commands.some((v) => v[0] === 'text' && v[1] === '不应显示的提示'));
        const dock = draw.hits.filter((v) => v.fixed);
        assert(dock.length >= 6 && dock.every((v) => v.y >= 984 && v.y + v.height <= 1080));
        assert.equal(hud.playerSeekRect.x, 0);
        assert.equal(hud.playerSeekRect.width, width);
    }
}
console.log(
    'Long titles, spaced detail actions, search-only surfaces and window-bottom playback pass at four aspect ratios',
);

// 身份确认行逐字显示用户名或五个方块；welcome 恢复原 company 图形文字。
for (const identityName of ['', '网易云昵称', '很长的昵称'.repeat(30)]) {
    const hud = new HUD(
        {},
        { ...glyphs, artWidth: () => 160, bounds: (_value, size) => ({ y: 0, height: size }) },
        { identityName },
        {},
    );
    const drawing = hud.draw(
        {
            mode: 'boot',
            boot: {
                auth: 'ID CONFIRMED : ',
                authOpacity: 1,
                f: 339,
                welcomeVisible: true,
                welcomeOpacity: 1,
                welcomeScale: 1,
                companyVisible: true,
            },
        },
        1920,
        0,
    );
    assert(drawing.commands.some((op) => op[0] === 'art' && op[1] === 'company'));
    const prefix = drawing.commands.find((op) => op[0] === 'art' && op[1] === 'identity');
    assert.equal(prefix[5], 'ID CONFIRMED : '.length);
    if (identityName) {
        const name = drawing.commands.find((op) => op[0] === 'text' && op[1] === identityName);
        assert(name && name[2] > 931 && Number.isFinite(name[3]));
        assert(glyphs.width(identityName, name[4]) <= 650 + 0.0001);
    } else {
        const squares = drawing.commands.filter((op) => op[0] === 'fill' && op[3] === 530 && op[4] === 14);
        assert.equal(squares.length, 5);
    }
    const early = hud.draw(
        { mode: 'boot', boot: { auth: 'ID CONFIRMED : ', authOpacity: 1, f: 320 } },
        1920,
        0,
    );
    assert(!early.commands.some((op) => op[0] === 'fill'));
    assert(!early.commands.some((op) => op[0] === 'text' && op[1] === identityName && identityName));
}
console.log('Identity confirmation shows saved names or five squares; welcome retains original lettering');
