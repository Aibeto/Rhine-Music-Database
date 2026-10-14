import assert from 'node:assert/strict';
import format from '../../MusicBridge/lyrics-format.cjs';
import { Lyrics, activeLineIndex, isSameRecording } from '../logic/lyrics.js';
import { HUD } from '../logic/hud.js';

// 真实平台形态包含重复副歌、多精度时间、偏移、实体、翻译与间奏。
const lrc = '[offset:200]\r\n[00:01.5][00:04.500]第一句 &amp; 第二声\r\n[00:03.00]\r\n[00:05]结束';
const document = format.normalizeLyrics('netease', { lrc: { lyric: lrc } }).document;
assert.deepEqual(
    document.lines.map((line) => line.startMs),
    [1500, 3000, 4500, 5000],
);
assert.equal(document.lines[0].text, '第一句 & 第二声');
assert.equal(activeLineIndex(document, 1299), -1);
assert.equal(activeLineIndex(document, 1300), 0);
assert.equal(activeLineIndex(document, 2800), 1);
assert.equal(document.lines[1].text, '');
assert.equal(activeLineIndex(document, 4800), 3);
assert.equal(activeLineIndex(document, 1300), 0);
assert.equal(activeLineIndex(document, 1100, 200), 0);

const yrc = format.normalizeLyrics('netease', {
    yrc: { lyric: '[1000,2000](1000,500,0)第一(1500,1500,0)句\n[4000,1000](4000,1000,0)第二句' },
    ytlrc: { lyric: '[00:01.00]译文一\n[00:04.00]译文二' },
}).document;
assert.equal(yrc.timing, 'word');
assert.equal(yrc.lines[0].endMs, 3000);
assert.equal(yrc.lines[0].translation, '译文一');
const krc = format.normalizeLyrics('kugou', {
    decodeContent: '[1000,2000]<0,500,0>第一<500,1500,0>句\n[4000,1000]<0,1000,0>第二句',
}).document;
assert.deepEqual(
    krc.lines.map((line) => line.words),
    yrc.lines.map((line) => line.words),
);
const qq = format.normalizeLyrics('qq', {
    response: {
        lyric: '[00:01.0]原文\n[00:02.00]第二句',
        trans: Buffer.from('[00:01.0]翻译').toString('base64'),
    },
}).document;
assert.equal(qq.lines[0].translation, '翻译');
assert.equal(format.normalizeLyrics('qq', { lyric: '只有文字\n没有时间' }).document.timing, 'none');
assert.equal(format.normalizeLyrics('qq', { lyric: '[00:00.00]一\n[00:00.00]二' }).document.timing, 'none');
const credits = format.normalizeLyrics('qq', {
    lyric:
        '[ti:回家的路]\n[ar:HOYO-MiX]\n[00:00.07]回家的路 - HOYO-MiX\n' +
        '[00:00.29]作词 Lyricist：木木\n[00:00.67]当你走上回家的路\n[00:04.03]星星发着光',
}).document;
assert.deepEqual(
    credits.lines.map((line) => line.startMs),
    [670, 4030],
);

const track = { provider: 'netease', id: '1', title: '曲目', artist: '歌手', duration: 120000 };
assert(isSameRecording(track, { ...track, provider: 'qq', id: 'mid' }));
assert(!isSameRecording(track, { ...track, title: '曲目 (Live)' }));
assert(!isSameRecording(track, { ...track, artist: '翻唱者' }));
assert(!isSameRecording(track, { ...track, duration: 130000 }));
assert(!isSameRecording(track, { ...track, duration: 0 }));

// 迟到歌词不能覆盖新歌；成功缓存离线复用，网络失败不成为永久无歌词缓存。
const pending = [];
const saved = {};
const host = {
    acceptCookies() {},
    request(route, params, callback) {
        pending.push({ route, params, callback });
    },
    preference: (key) => saved[key] || '',
    setPreference: (key, value) => {
        saved[key] = value;
    },
};
const lyrics = new Lyrics(host, () => {});
const first = lyrics.load(track);
const second = lyrics.load({ ...track, id: '2' });
pending[1].callback(JSON.stringify({ body: { code: 200, document } }), '');
await second;
lyrics.sync(4800);
assert.equal(lyrics.index, 3);
pending[0].callback(JSON.stringify({ body: { code: 200, document: qq } }), '');
await first;
assert.equal(lyrics.currentKey, 'netease:2');
assert.equal(lyrics.document.source, 'netease');
lyrics.adjustOffset(200);
const offline = new Lyrics(host, () => {});
await offline.load({ ...track, id: '2' });
assert.equal(pending.length, 2);
assert.equal(offline.offsetMs, 0);
assert(JSON.parse(saved['lyrics-cache']).every((entry) => !Object.hasOwn(entry, 'offsetMs')));
offline.adjustOffset(-400);
offline.sync(1500);
assert.equal(offline.offsetMs, -400);
await offline.load({ ...track, id: '2' });
assert.equal(offline.offsetMs, 0);
offline.clear();
assert.equal(offline.document, null);

// 按钮立即进入当前歌曲；歌词模式不绘制原卡片，点击行按偏移校正后 seek。
let selected;
let mode;
let seek;
const song = { ...track, key: 'netease:song:1', kind: 'song' };
const store = {
    observe() {},
    current: song,
    player: { current: song, lyrics, state: { seekable: true, duration: 10000 } },
    followPlayback: (value) => {
        selected = value;
    },
};
const glyphs = { wrap: (text) => [text], width: (text, size) => text.length * size * 0.5, inset: () => 0 };
const hud = new HUD(
    {
        mediaControl: (action, position) => {
            seek = [action, position];
        },
    },
    glyphs,
    store,
    {
        setMode: (value) => {
            mode = value;
        },
    },
);
hud.frame = { mode: 'archive' };
hud.action('player:lyrics');
assert.equal(selected, song);
assert.equal(mode, 'detail');
assert.equal(hud.lyricsMode, true);
hud.detail(1728, 1);
assert(!hud.commands.some((command) => command[0] === 'fill' && command[3] === 260));
assert(!hud.commands.some((command) => command[0] === 'text' && command[1] === '加入歌单'));
assert(hud.hits.some((hit) => hit.action.startsWith('player:lyric-line:')));
hud.action('player:lyric-line:0');
assert.deepEqual(seek, ['seek', 1100]);
hud.action('player:lyrics-close');
assert.equal(hud.lyricsMode, false);
hud.action('player:lyrics');
hud.action('back');
assert.equal(hud.lyricsMode, false);

// 连续小幅触控板输入不被整数步长吞掉，接续滚动速度，浏览时播放换句不会拉动视口。
lyrics.sync(0);
hud.action('player:lyrics');
hud.w = 1728;
hud.lastTime = 0;
hud.lyricsLayoutDocument = null;
hud.detail(1728, 1);
for (let i = 1; i <= 40; i++) {
    hud.wheel(1100, 500, -2, 0, 2);
    hud.lastTime = i / 60;
    hud.detail(1728, 1);
}
assert.equal(hud.lyricsBrowse, 80);
assert(hud.lyricsScroll > 60 && hud.lyricsScroll < 80);
const browsePosition = hud.lyricsBrowse;
lyrics.sync(4800);
hud.lastTime += 0.02;
hud.detail(1728, 1);
assert.equal(hud.lyricsTarget, browsePosition);
assert(hud.lyricsAnimating(hud.lastTime));
hud.lastTime += 1;
hud.detail(1728, 1);
assert.equal(hud.lyricsScroll, browsePosition);
assert(!hud.lyricsAnimating(hud.lastTime));
hud.wheel(1100, 500, -1000, 1, 10000);
assert.equal(hud.lyricsBrowse, hud.lyricsLayout.at(-1).top);
hud.wheel(1100, 500, 1000, -1, -10000);
assert.equal(hud.lyricsBrowse, 0);
hud.action('player:lyrics-follow');
assert.equal(hud.lyricsBrowse, null);
const opened = [];
hud.host.open = (url) => opened.push(url);
for (const link of hud.links.filter((link) => /project-(lyricsx|lyricskit|lyrimuse)/.test(link[1]))) {
    hud.action('music:' + link[1]);
    assert.equal(opened.at(-1), link[2]);
}
console.log('三平台格式、时间偏移、间奏、版本拒绝、迟到隔离、离线缓存及歌词导航通过');
