import {
    sessionLabel,
    libraryTitle,
    libraryActionLabel,
    libraryAction,
    archiveActionLabel,
    collectionLabel,
    modalCategory,
    modalTitle,
    modalSubtitle,
    emptyLibraryMessage,
    themeLabel,
} from './hud-labels.js';
import { groupRows } from './groups.js';
import { pad, time } from './music-items.js';
import { RollingLabel, SurfaceMotion } from './motion.js';
import { QQ_DEVICE_NOTICE } from './login-state.js';
// 所有坐标沿用 1080 高的原舞台；文字仍绘制授权字体的原始轮廓。
export class HUD {
    constructor(host, glyphs, music, runtime) {
        this.host = host;
        this.g = glyphs;
        this.s = music;
        this.r = runtime;
        this.tab = 'overview';
        this.modal = null;
        this.query = '';
        this.listOffset = 0;
        this.musicCollection = 1;
        this.trackOffset = 0;
        this.bodyOffset = 0;
        this.dark = false;
        this.reduced = false;
        this.motion = new SurfaceMotion();
        this.closing = false;
        this.lastTime = 0;
        this.commands = [];
        this.hits = [];
        this.rolling = {};
        this.searchGeneration = 0;
        this.groupBy = 'none';
        this.activeGroup = null;
        this.resultSignature = '';
        this.resultStart = 0;
        this.artistTab = 'songs';
        this.queueOffset = 0;
        this.themeMode = host.preference?.('theme') || 'light';
        this.dark = this.themeMode === 'system' ? !!host.systemDark?.() : this.themeMode === 'dark';
        this.reduced = host.preference?.('motion') === 'reduced';
    }

    get ink() {
        return this.dark
            ? [0.92, 0.92, 0.92, 1, 'calibratedWhite']
            : [0.08, 0.085, 0.075, 1, 'calibratedRGB'];
    }

    op(...v) {
        this.commands.push(v.map((x) => (Array.isArray(x) ? x.slice() : x)));
    }

    save() {
        this.op('save');
    }

    restore() {
        this.op('restore');
    }

    alpha(v) {
        this.op('alpha', v);
    }

    translate(x, y) {
        this.op('translate', x, y);
    }

    scale(x, y) {
        this.op('scale', x, y);
    }

    clip(x, y, w, h) {
        this.op('clip', x, y, w, h);
    }

    fill(x, y, w, h, color = this.ink) {
        this.op('fill', color, x, y, w, h);
    }

    text(value, x, y, size, weight = 'regular', tracking = 0, color = this.ink) {
        this.op('text', String(value), x, y, size, weight, tracking, color);
    }

    aligned(value, x, y, size, weight = 'regular', tracking = 0) {
        this.text(value, x - this.g.inset(value, size, weight), y, size, weight, tracking);
    }

    fit(value, size, width, weight = 'regular') {
        const text = String(value);
        const key = JSON.stringify([text, size, width, weight]);
        this.fitCache ??= new Map();
        if (this.fitCache.has(key)) {
            return this.fitCache.get(key);
        }
        let length = text.length;
        if (this.g.width(text, size, weight) > width) {
            let lo = 1;
            let hi = length;
            while (lo < hi) {
                const mid = Math.ceil((lo + hi) / 2);
                if (this.g.width(text.slice(0, mid), size, weight) <= width) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            length = lo;
        }
        const result = length === text.length ? text : text.slice(0, Math.max(0, length - 1)) + '…';
        if (this.fitCache.size >= 512) {
            this.fitCache.delete(this.fitCache.keys().next().value);
        }
        this.fitCache.set(key, result);
        return result;
    }

    limited(value, x, y, size, width, weight = 'regular') {
        this.text(this.fit(value, size, width, weight), x, y, size, weight);
    }
    // 播放栏按实际文字轮廓对齐，字号不同也共用同一视觉中心线。
    centeredText(value, x, cy, size, weight = 'regular', horizontal = false) {
        const b = this.g.bounds?.(String(value), size, weight) ?? {
            x: 0,
            y: 0,
            width: this.g.width(value, size, weight),
            height: size,
        };
        this.text(value, x - b.x - (horizontal ? b.width / 2 : 0), cy - b.y - b.height / 2, size, weight);
    }

    rollingText(key, value, x, y, size, weight = 'regular', tracking = 0, numeric = false) {
        const label = this.rolling[key] || (this.rolling[key] = new RollingLabel());
        label.render(
            this,
            value,
            x,
            y,
            size,
            weight,
            tracking,
            this.lastTime,
            !this.reduced && this.frame.mode === 'archive',
            numeric,
        );
    }

    roll(key, value, x, y, size, width, weight = 'regular', numeric = false) {
        const shown = this.fit(value, size, width, weight);
        this.rollingText(key, shown, x - this.g.inset(shown, size, weight), y, size, weight, 0, numeric);
    }

    hit(x, y, w, h, action, label = action) {
        this.hits.push({
            x,
            y,
            width: w,
            height: h,
            action,
            label,
        });
    }

    button(label, x, y, w, h, action, size, weight = 'regular') {
        this.hit(x, y, w, h, action, label);
        this.text(label, x, y + (h - size) / 2 - 4, size, weight);
    }

    control(label, x, y, w, action) {
        this.hit(x - 10, y, w + 20, 54, action, label);
        this.aligned(label, x, y + 14, 20, 'demibold');
    }

    rule(x, y, w, alpha = 0.4) {
        const c = this.ink.slice();
        c[3] = alpha;
        this.fill(x, y, w, 1, c);
    }

    image(key, x, y, w, h) {
        if (key) {
            this.op('image', key, x, y, w, h);
        }
    }

    art(key, x, y, size, count = -1, tracking = 0, color = this.ink) {
        this.op('art', key, x, y, size, count, tracking, color);
    }
    /**
     * 按原舞台坐标生成本帧绘制命令和命中区域。
     * 动画状态、绘制顺序、文字轮廓与命中坐标保持一致。
     */
    draw(frame, w, t) {
        if (this.themeMode === 'system' && this.dark !== !!this.host.systemDark?.()) {
            this.dark = !!this.host.systemDark?.();
            this.r.theme(this.dark);
        }
        this.frame = frame;
        this.w = w;
        this.lastTime = t;
        this.commands = [];
        this.hits = [];
        this.playerSeekRect = null;
        this.playerVolumeRect = null;
        this.motion.sample(t);
        if (this.closing && this.motion.finished) {
            this.modal = null;
            this.closing = false;
        }
        if (frame.viewer) {
            this.viewer(w);
        } else if (frame.mode === 'boot') {
            this.boot(w);
        } else {
            this.brand();
            this.navigation(w);
            this.footer(w);
            if (this.modal !== 'search') {
                if (frame.mode === 'detail') {
                    this.detail(w, frame.detail);
                } else {
                    this.archive(w);
                }
            }
        }
        if (this.modal) {
            this.modalDraw(w);
        }
        if (frame.mode !== 'boot') {
            this.playerBar(0, 984, w);
        }
        return {
            commands: this.commands,
            hits: this.hits,
        };
    }

    brand() {
        this.roll('music-brand', this.s.libraryName, 59, 115.425, 44, Math.min(720, this.w * 0.45), 'bold');
        this.aligned('RHINE LAB / MUSIC DATABASE', 59, 173, 14, 'demibold', 1);
    }

    brandLine(i) {
        if (i === 0) {
            this.text('RHINE LAB', 59, 115.425, 50.75, 'bold', 1);
        } else if (i === 1) {
            this.text('MUSIC DATABASE', 59, 169, 22, 'demibold', 1.7);
        } else {
            this.rule(59, 212, 300, 0.65);
            this.text('INDEX / COLLECTION / TRACK', 59, 228, 11, 'regular', 1.2);
        }
    }

    navigation(w) {
        this.button('发现音乐 ↗', 59, 211, 180, 35, 'discover', 14);
        this.button((this.s.providerName || '网易云') + ' ⇄', 259, 211, 180, 35, 'provider', 14);
        const x = w - 579;
        this.control('音乐检索  /', x, 118, 184, 'search');
        this.control('＋ 我的音乐', x + 200, 118, 184, 'saved');
        this.control('设置', x + 400, 118, 120, 'settings');
    }

    footer(w) {
        this.fill(59, 971, 5, 5, [119 / 255, 123 / 255, 96 / 255, 1]);
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.text(sessionLabel(this.s), 75, 967, 10, 'regular', 0.9);
        this.limited(this.s.status, w - 361.203, 967, 10, 195);
        this.button('REINITIALIZE ↗', w - 145.57, 965, 86.57, 20, 'replay', 10);
        this.text('POWERED BY RHINE LAB', w / 2 - 85, 967, 10, 'demibold');
    }

    empty(w) {
        const x = w * 0.50520833;
        const music = this.s;
        if (music.directory) {
            this.control('← 返回上一级', 59, 281, 280, 'music:parent');
        }
        this.text('MUSIC DATABASE / ' + music.libraryName, x, 471, 12);
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.limited(libraryTitle(this.s), x, 509, 28, w - x - 59, 'bold');
        this.rule(x, 601, w - x - 59);
        this.limited(music.status, x, 624, 16, w - x - 117);
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.button(libraryActionLabel(this.s), x, 703, 300, 32, libraryAction(this.s), 14);
    }

    archive(w) {
        const music = this.s;
        const item = music.current;
        if (item) {
            const x = w * 0.50520833;
            if (music.directory) {
                this.control('← 返回上一级', 59, 281, 280, 'music:parent');
                this.control('查看音乐详情 ↗', 59, 347, 310, 'music:directory-info');
            }
            this.roll('music-source', 'MUSIC DATABASE / ' + music.libraryName, x, 471, 12, w - x - 59);
            this.roll('music-title', item.title, x, 509, 28, w - x - 59, 'bold');
            this.hit(x, 509, 414, 37, 'open', item.title);
            this.rule(x, 601, w - x - 59);
            this.fill(x - 12, 598, 6, 6);
            this.roll(
                'music-id',
                item.kind.toUpperCase() + ' ID: ' + item.id + ' / ' + item.artist,
                x,
                624,
                16,
                w - x - 210,
            );
            this.text(item.kind.toUpperCase(), w - 170, 627, 11);
            // 保持原有判断顺序，仅计算当前需要的分支结果。

            this.control(archiveActionLabel(item), x, 703, 300, 'open');
            this.aligned('点击打开 · ENTER 键也可打开', x, 771, 14);
            this.text('TRACK / SELECT', 60, 865, 11);
            this.rollingText('music-number', pad(music.libraryIndex + 1), 60, 901.2, 58, 'regular', 0, true);
            this.text('/', 141, 925, 32, 'light');
            this.text(String(music.libraryTotal), 172, 932, 22);
        } else {
            this.empty(w);
        }
        const nav = w * 0.2625;
        this.button('↑', nav, 903, 46, 46, 'prev', 28);
        for (let i = 0; i < 8; i++) {
            const x = nav + 85 + i * 26;
            const row = Math.max(0, music.physicalSlots[music.selectedLane].indexOf(this.frame.selected));
            const c = this.ink.slice();
            c[3] = i === row ? 1 : 0.45;
            this.fill(x, i === row ? 909 : 920, 2, i === row ? 33 : 12, c);
            if (music.libraryItems.length) {
                this.hit(x - 6, 900, 26, 50, 'music:slot:' + i, '选曲 ' + (i + 1));
            }
        }
        this.button('↓', nav + 320, 903, 46, 46, 'next', 28);
        const col = w * 0.5234375;
        this.button('←', col, 903, 46, 46, 'column-prev', 28);
        if (!music.directory) {
            this.text('COLLECTION ' + pad(music.selectedLane + 1) + ' / 05', col + 70, 904, 10);
            this.roll('music-column', music.libraryName, col + 70, 925, 15, w - col - 360);
        } else {
            const label = 'TRACK COLUMN ' + pad(music.selectedLane + 1) + ' / 05';
            this.text(label, col + 62 + (171 - this.g.width(label, 15)) / 2, 916, 15);
        }
        this.button('→', col + 233, 903, 46, 46, 'column-next', 28);
        this.text(
            music.directory
                ? '← → 浏览曲目列 / ↑ ↓ 选曲 / ENTER 查看 / ESC 上一级'
                : '← → 切换来源 / ↑ ↓ 选曲 / ENTER 查看 / 检索',
            nav,
            850,
            12,
        );
    }

    actionButton(label, x, y, width, action, primary = false) {
        const c = primary ? [119 / 255, 123 / 255, 96 / 255, 1] : this.ink.slice();
        if (!primary) {
            c[3] = 0.075;
        }
        this.fill(x, y, width, 44, c);
        this.hit(x, y, width, 44, action, label);
        this.text(label, x + 14, y + 12, 15, 'demibold', 0, primary ? [0.96, 0.95, 0.91, 1] : this.ink);
    }

    detail(w, opacity) {
        const music = this.s;
        const item = music.current;
        if (!item) {
            this.archive(w);
            return;
        }
        if (item.kind !== 'song') {
            this.container(w, opacity);
            return;
        }
        const x = w * 0.56;
        const width = w - x - 59;
        const gap = 12;
        const buttonWidth = (width - gap * 2) / 3;
        this.save();
        this.alpha(opacity);
        this.fill(
            x - 24,
            260,
            width + 48,
            696,
            this.dark ? [0.08, 0.095, 0.09, 0.96] : [0.929, 0.922, 0.894, 0.96],
        );
        this.button('← 返回资料库   ESC', 59, 281, 360, 54, 'back', 20, 'demibold');
        this.limited(item.id, 60, 845, 37, 330);
        this.text('MUSIC DATABASE', 60, 893, 10);
        this.text('DRAG TO INSPECT', 60, 917, 10);
        this.button('查看封面 ↗', 60, 944, 260, 35, 'viewer', 14);
        this.text('SONG ID / ' + item.id, x, 278, 12);
        const titleLines = this.g.wrap(item.title, width, 32);
        titleLines
            .slice(0, 2)
            .forEach((line, i) =>
                this.limited(
                    i === 1 && titleLines.length > 2 ? line + '…' : line,
                    x,
                    305 + i * 40,
                    32,
                    width,
                    'bold',
                ),
            );
        this.limited(item.artist, x, 394, 19, width, 'demibold');
        if (music.editable !== false) {
            this.actionButton(
                (music.likedIDs || []).includes(item.id) || music.lists[1].some((v) => v.id === item.id)
                    ? '♥ 已喜欢'
                    : '♡ 喜欢',
                x,
                436,
                buttonWidth,
                'music:like',
            );
        }
        if (music.editable !== false) {
            this.actionButton('加入歌单', x + buttonWidth + gap, 436, buttonWidth, 'music:add-to-playlist');
        }
        if (item.artists?.length) {
            this.actionButton('歌手作品 ↗', x + (buttonWidth + gap) * 2, 436, buttonWidth, 'music:artist:0');
        }
        this.actionButton('▶ 面板内播放', x, 498, buttonWidth, 'music:play', true);
        this.actionButton('加入队列', x + buttonWidth + gap, 498, buttonWidth, 'music:enqueue');
        this.actionButton('下一首播放', x + (buttonWidth + gap) * 2, 498, buttonWidth, 'music:play-next');
        this.rule(x, 565, width, 0.4);
        const column = (width - gap) / 2;
        const meta = [
            ['ARTIST / 歌手', item.artist],
            ['ALBUM / 专辑', item.album],
            ['DURATION / 时长', time(item)],
            ['PLAYBACK / 播放方式', '面板内播放 / 官方入口'],
        ];
        meta.forEach((p, i) => {
            const xx = x + (i % 2) * (column + gap);
            const yy = 584 + Math.floor(i / 2) * 64;
            this.text(p[0], xx, yy, 11);
            this.limited(p[1] || '暂无', xx, yy + 23, 16, column - 12);
        });
        ['overview', 'notes', 'history'].forEach((key, i) => {
            const xx = x + (i * width) / 3;
            this.button(['歌曲百科', '专辑信息', '同专辑'][i], xx, 720, width / 3 - 12, 38, 'tab:' + key, 15);
            if (this.tab === key) {
                this.rule(xx, 760, width / 3 - 24, 1);
            }
        });
        this.rule(x, 762, width, 0.2);
        if (this.tab === 'history') {
            music.tracks.slice(this.trackOffset, this.trackOffset + 2).forEach((song, i) => {
                const yy = 790 + i * 48;
                this.text(pad(this.trackOffset + i + 1), x, yy, 11);
                this.limited(song.title + ' / ' + song.artist, x + 35, yy, 16, width - 115);
                this.text(time(song), x + width - 65, yy, 12);
                this.rule(x, yy + 36, width, 0.2);
                this.hit(x, yy - 4, width, 42, 'music:track:' + (this.trackOffset + i), song.title);
            });
            if (!music.tracks.length) {
                this.text(music.detailLoading ? '正在载入曲目…' : '暂无曲目信息', x, 790, 17);
            }
        } else {
            const lines = this.g.wrap(this.body(), width, 16);
            this.bodyOffset = Math.min(this.bodyOffset, Math.max(0, lines.length - 3));
            lines
                .slice(this.bodyOffset, this.bodyOffset + 3)
                .forEach((line, i) => this.text(line, x, 790 + i * 31, 16));
        }
        this.actionButton(
            music.provider === 'qq' ? 'QQ 来源页面 ↗' : '官方客户端 ↗',
            x,
            910,
            buttonWidth,
            'music:official',
        );
        if (music.editable !== false && music.directory?.item.creatorID === music.uid && music.uid) {
            this.actionButton(
                '从歌单移除',
                x + (buttonWidth + gap) * 2,
                910,
                buttonWidth,
                'music:remove-track',
            );
        }
        this.restore();
    }

    body() {
        const item = this.s.current;
        if (!item) {
            return '';
        }
        if (item.kind === 'song') {
            return this.tab === 'overview'
                ? item.wiki || (this.s.detailLoading ? '正在读取歌曲百科…' : '暂无歌曲百科')
                : '专辑：' +
                      item.album +
                      '\n歌手：' +
                      item.artist +
                      '\n发行日期：' +
                      (item.date || '暂无') +
                      '\n' +
                      (item.description || '暂无专辑介绍');
        }
        return this.tab === 'overview'
            ? item.description || '暂无简介'
            : '编号：' +
                  item.id +
                  '\n名称：' +
                  item.title +
                  '\n' +
                  item.artist +
                  '\n已载入 ' +
                  this.s.tracks.length +
                  ' 首';
    }

    containerLayout(w = this.w) {
        const left = 40;
        const sidebar = Math.min(340, Math.max(250, w * 0.23 - 40));
        const x = left + sidebar + 36;
        const width = w - x - 40;
        return {
            left,
            sidebar,
            x,
            width,
            top: 260,
            rowsY: 434,
            rowHeight: 54,
            rowCount: 9,
            bottom: 956,
        };
    }

    container(w, opacity) {
        const music = this.s;
        const item = music.current;
        const l = this.containerLayout(w);
        const { left, sidebar, x, width, rowsY, rowHeight, rowCount } = l;
        const albums = item.kind === 'artist' && this.artistTab === 'albums';
        const songs = albums ? (music.directory?.albums ?? []) : (music.directory?.songs ?? music.tracks);
        const sx = left + 19;
        const sideWidth = sidebar - 38;
        const coverSize = Math.min(220, sideWidth);
        this.trackOffset = Math.min(this.trackOffset, Math.max(0, songs.length - rowCount));
        this.save();
        this.alpha(opacity);
        this.fill(
            left,
            l.top,
            w - left * 2,
            l.bottom - l.top,
            this.dark ? [0.08, 0.095, 0.09, 1] : [0.929, 0.922, 0.894, 1],
        );
        this.rule(x - 18, 278, 1, 0.12);
        this.button('← 返回曲目资料库   ESC', sx, 274, sideWidth, 54, 'music:directory', 20, 'demibold');
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.text(collectionLabel(item), sx, 325, 11);
        this.image(music.coverImage, sx, 355, coverSize, coverSize);
        this.limited(item.title, sx, coverSize + 374, 20, sideWidth, 'bold');
        this.limited(item.artist, sx, coverSize + 410, 14, sideWidth);
        this.text(
            (music.provider === 'qq' ? 'QQ MID / ' : 'NETEASE ID / ') + item.id,
            sx,
            coverSize + 449,
            11,
        );
        this.text(
            Math.max(item.trackCount || 0, music.directory?.ids.length || 0, songs.length) + ' TRACKS / 曲目',
            sx,
            coverSize + 479,
            12,
        );
        this.limited(item.date ? '发行日期：' + item.date : '', sx, coverSize + 510, 12, sideWidth);
        this.g
            .wrap(item.description || '暂无简介', sideWidth, 12)
            .slice(0, 3)
            .forEach((line, i) => this.text(line, sx, coverSize + 550 + i * 22, 12));
        if (
            music.editable !== false &&
            item.kind === 'playlist' &&
            item.creatorID === music.uid &&
            music.uid
        ) {
            this.button('修改名称', sx, 866, sideWidth / 2, 30, 'music:rename-playlist', 12);
            this.button('删除歌单', sx + sideWidth / 2, 866, sideWidth / 2, 30, 'music:delete-playlist', 12);
        }
        if (music.editable !== false) {
            this.button('收藏 / 取消', sx, 913, sideWidth / 2, 32, 'music:subscribe', 12);
        }
        this.button(
            (music.providerName || '网易云') + ' ↗',
            sx + sideWidth / 2,
            913,
            sideWidth / 2,
            32,
            'music:source',
            12,
        );
        this.limited(item.title, x, 279, 30, width - 155, 'bold');
        this.text(
            songs.length +
                ' / ' +
                Math.max(
                    item.trackCount || 0,
                    music.directory?.ids.length ?? music.trackIDs.length,
                    songs.length,
                ) +
                ' 首',
            x + width - 130,
            289,
            12,
        );
        if (!albums) {
            this.fill(x, 339, 180, 44, [119 / 255, 123 / 255, 96 / 255, 1]);
            this.hit(x, 339, 180, 44, 'music:play-loaded', '播放全部');
            this.text('▶  播放全部', x + 22, 351, 16, 'demibold', 0, [0.96, 0.95, 0.91, 1]);
        }
        if (item.kind === 'artist') {
            this.button('热门歌曲', x + 210, 345, 110, 32, 'music:artist-tab:songs', 13);
            this.button('发行专辑', x + 335, 345, 110, 32, 'music:artist-tab:albums', 13);
        }
        this.rule(x, 399, width, 0.4);
        const titleX = x + 49;
        const artistX = x + width * 0.52;
        const albumX = x + width * 0.75;
        const timeX = x + width - 65;
        [
            ['#', x],
            ['TITLE / 歌曲', titleX],
            ['ARTIST / 歌手', artistX],
            ['ALBUM / 专辑', albumX],
            ['TIME', timeX],
        ].forEach(([label, xx]) => this.text(label, xx, 412, 11));
        songs.slice(this.trackOffset, this.trackOffset + rowCount).forEach((song, i) => {
            const index = this.trackOffset + i;
            const yy = rowsY + i * rowHeight;
            if (music.directory?.index === index) {
                const c = this.ink.slice();
                c[3] = 0.055;
                this.fill(x - 8, yy, width + 16, rowHeight - 2, c);
            }
            this.text(pad(index + 1), x, yy + 16, 12);
            this.limited(song.title, titleX, yy + 12, 18, artistX - titleX - 22, 'demibold');
            this.limited(song.artist, artistX, yy + 16, 12, albumX - artistX - 20);
            this.limited(song.album, albumX, yy + 16, 12, timeX - albumX - 18);
            this.limited(albums ? song.date || '专辑' : time(song), timeX, yy + 16, 12, 65);
            this.rule(x, yy + rowHeight - 1, width, 0.15);
            this.hit(
                x - 8,
                yy,
                width + 16,
                rowHeight - 2,
                (albums ? 'music:artist-album:' : 'music:track:') + index,
                song.title,
            );
        });
        if (!songs.length) {
            this.limited(
                music.directoryLoading || music.detailLoading ? '正在载入曲目…' : '暂无可读取曲目',
                titleX,
                490,
                22,
                width - 100,
            );
        }
        this.text('滚轮浏览曲目 · 点击查看歌曲', x, 934, 11);
        const total = music.directory?.ids.length ?? music.trackIDs.length;
        if (albums && music.directory?.albumsMore) {
            this.button(
                music.directory.albumsLoading ? '载入中…' : '更多专辑 ↓',
                x + width - 150,
                922,
                150,
                32,
                'music:artist-more',
                12,
            );
        }
        if (!albums && songs.length < total) {
            this.button(
                music.directoryLoading ? '载入中…' : '加载更多 ↓',
                x + width - 150,
                922,
                150,
                32,
                'music:directory-more',
                12,
            );
        }
        if (songs.length > rowCount) {
            const height = rowCount * rowHeight - 2;
            const c = this.ink.slice();
            c[3] = 0.12;
            this.fill(x + width + 14, rowsY, 2, height, c);
            const h = Math.max(28, (height * rowCount) / songs.length);
            const t = this.trackOffset / Math.max(1, songs.length - rowCount);
            c[3] = 0.6;
            this.fill(x + width + 14, rowsY + t * (height - h), 2, h, c);
        }
        this.restore();
    }

    viewer(w) {
        const item = this.s.current;
        if (!item) {
            return;
        }
        this.button('← 返回音乐   ESC', 59, 69, 300, 54, 'viewer:close', 20, 'demibold');
        this.text('RHINE LAB / MUSIC DATABASE / COVER', w / 2 - 145, 70, 11, 'regular', 1.5);
        this.limited(item.title + ' / 封面', w / 2 - 145, 95, 28, w / 2 - 250, 'demibold');
        this.text(item.id, w / 2 - 145, 139, 12);
        this.text('360°', w - 185, 72, 55, 'light');
        this.button('清晰', w - 247, 189, 84, 38, 'viewer:clear', 14);
        this.button('磨砂', w - 147, 189, 84, 38, 'viewer:frosted', 14);
        this.text('拖动旋转     ↑ ↓ ← → 平移     滚轮缩放', 59, 965, 13);
        this.button('原始视角', w / 2 - 165, 914, 154, 46, 'viewer:reset', 15);
        this.button('封面正面', w / 2 + 18, 914, 154, 46, 'music:cover-front', 15);
        this.button('复位视角 ↗', w - 194, 914, 135, 46, 'viewer:reset', 14);
    }

    rows(lane) {
        const source = this.s.lists[lane];
        const revision = this.s.revision ?? 0;
        const query = this.modal === 'saved' ? this.query.toLocaleLowerCase() : '';
        const key = JSON.stringify([lane, revision, source.length, query, this.groupBy, this.activeGroup]);
        if (this.rowsCache?.key === key && this.rowsCache.source === source) {
            return this.rowsCache.rows;
        }
        let rows = source
            .map((value, index) => [index, value])
            .filter(
                (e) => !query || (e[1].title + e[1].artist + e[1].id).toLocaleLowerCase().includes(query),
            );
        if (this.groupBy !== 'none' && rows.some((e) => e[1].kind === 'song')) {
            const groups = groupRows(
                rows.filter((e) => e[1].kind === 'song'),
                this.groupBy,
            );
            rows = this.activeGroup
                ? (groups.find((e) => e[1].key === this.activeGroup)?.[1].entries ?? [])
                : groups;
            if (this.activeGroup && this.s.groupView?.lane === lane) {
                this.s.groupView.indices = rows.map((e) => e[0]);
            }
        }
        rows.signature = rows.map((e) => e[1].key ?? e[1].id).join('|');
        this.rowsCache = {
            key,
            source,
            rows,
        };
        return rows;
    }

    rowMotion(signature, index) {
        if (signature !== this.resultSignature) {
            this.resultSignature = signature;
            this.resultStart = this.lastTime;
        }
        const p = this.reduced
            ? 1
            : Math.max(0, Math.min(1, (this.lastTime - this.resultStart - index * 0.025) / 0.24));
        return {
            opacity: 1 - Math.pow(1 - p, 3),
            offset: 10 * Math.pow(1 - p, 3),
        };
    }

    modalBounds(w = this.w) {
        const width =
            this.modal === 'search'
                ? Math.min(w - 100, Math.max(760, Math.min(1000, w * 0.5 - 59)))
                : Math.min(1260, w - 100);
        return {
            width,
            x: this.modal === 'search' ? w - width - 59 : (w - width) / 2,
            y: 122,
        };
    }
    /**
     * 绘制弹窗外壳与标题，再分派对应面板。
     * 各面板共用外层透明度和绘制栈，结束时统一恢复。
     */
    modalDraw(w) {
        const music = this.s;
        this.save();
        this.alpha(this.motion.opacity);
        this.fill(
            0,
            0,
            w,
            1080,
            this.dark
                ? [17 / 255, 24 / 255, 27 / 255, this.modal === 'search' ? 0.15 : 0.6]
                : [227 / 255, 224 / 255, 215 / 255, this.modal === 'search' ? 0.12 : 0.45],
        );
        this.translate(0, this.motion.offset);
        const { width, x, y } = this.modalBounds(w);
        this.fill(
            x,
            y,
            width,
            836,
            this.dark ? [0.15, 0.15, 0.15, 1, 'calibratedWhite'] : [0.929, 0.922, 0.894, 1],
        );
        this.hits = [];
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.text('RHINE LAB / MUSIC DATABASE / ' + modalCategory(this.modal), x + 53, y + 37, 11);
        this.button('← 返回', x + width - 220, y + 18, 167, 54, 'close-modal', 20, 'demibold');
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.text(modalTitle(this.modal, this.s), x + 53, y + 91, 38, 'demibold');
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        this.text(modalSubtitle(this.modal, this.s), x + 53, y + 147, 15);
        if (this.modal === 'confirm') {
            this.limited(this.confirmTitle || '', x + 53, y + 260, 24, width - 106, 'demibold');
            this.text('此操作将同步至网易云。', x + 53, y + 330, 16);
            this.button('确认删除', x + 53, y + 450, 180, 45, 'confirm:yes', 16);
            this.button('取消', x + 270, y + 450, 150, 45, 'close-modal', 16);
        } else if (this.modal === 'playlist-picker') {
            this.text('选择我的歌单', x + 53, y + 240, 18);
            music.lists[2]
                .slice(this.listOffset, this.listOffset + 7)
                .forEach((value, i) =>
                    this.button(
                        this.fit(value.title, 18, width - 106),
                        x + 53,
                        y + 290 + i * 60,
                        width - 106,
                        52,
                        'music:playlist-pick:' + value.id,
                        18,
                    ),
                );
            this.button('↑ 上一页', x + width - 330, y + 758, 120, 32, 'music:picker-prev', 12);
            this.button('下一页 ↓', x + width - 190, y + 758, 120, 32, 'music:picker-next', 12);
            this.limited(music.status, x + 53, y + 775, 12, width - 420);
        } else if (this.modal === 'playlist-form') {
            this.text(this.formKind === 'create' ? '歌单名称' : '新歌单名称', x + 53, y + 240, 15);
            this.text('按 ENTER 提交', x + 53, y + 420, 12);
            this.limited(music.status, x + 53, y + 775, 12, width - 106);
        } else if (this.modal === 'discover') {
            this.discoverPanel(x, y, width);
        } else if (this.modal === 'player') {
            this.playerPanel(x, y, width);
        } else if (this.modal === 'settings') {
            this.settings(x, y, width);
        } else if (this.modal === 'login') {
            this.loginPanel(x, y, width);
        } else {
            this.libraryPanel(x, y, width);
        }
        this.restore();
    }
    /**
     * 绘制当前来源支持的登录方式及状态。
     * 只生成按钮和输入位置，账号操作由音乐状态层执行。
     */
    loginPanel(x, y, width) {
        const music = this.s;
        (music.provider === 'qq' ? ['qr', 'wechat'] : ['qr', 'phone', 'email']).forEach((method, i) =>
            this.button(
                {
                    qr: music.provider === 'qq' ? '手机 QQ 扫码' : '扫码登录',
                    wechat: '微信扫码',
                    phone: '手机登录',
                    email: '邮箱登录',
                }[method],
                x + 53 + i * 145,
                y + 207,
                140,
                35,
                'music:login-method:' + method,
                13,
            ),
        );
        if (['qr', 'wechat'].includes(music.loginMethod)) {
            this.image(music.qr, x + width / 2 - 120, y + 260, 240, 240);
            if (music.provider === 'qq') {
                const lines = this.g.wrap(QQ_DEVICE_NOTICE, width - 106, 13);
                lines.forEach((line, index) => this.text(line, x + 53, y + 520 + index * 18, 13));
            }
            if (music.canRegenerateLogin !== false) {
                this.button('重新生成二维码', x + 53, y + 626, 260, 40, 'music:qr', 15);
            } else {
                const label =
                    music.loginRetrySeconds > 0
                        ? '请等待 ' + music.loginRetrySeconds + ' 秒后重新生成'
                        : '正在处理扫码登录…';
                this.text(label, x + 53, y + 636, 15);
            }
        } else {
            this.text(
                music.loginMethod === 'phone' ? '手机号码（中国大陆）' : '邮箱地址',
                x + 53,
                y + 278,
                14,
            );
            this.text('密码', x + 53, y + 391, 14);
            this.text('按 ENTER 登录', x + 53, y + 532, 12);
        }
        this.limited(music.loginStatus, x + 53, y + 600, 15, width - 106);
        this.button('本轮跳过账号验证', x + 53, y + 709, 350, 45, 'close-modal', 16);
        this.text('凭据仅保存至本机钥匙串，不读取官方 App 的登录信息。', x + 53, y + 785, 11);
    }
    /**
     * 绘制搜索或个人音乐列表，保持分页、筛选和命中区域同步。
     */
    libraryPanel(x, y, width) {
        const music = this.s;
        if (this.modal === 'saved' && music.editable !== false) {
            this.button('＋ 创建歌单', x + width - 220, y + 147, 167, 33, 'music:create-playlist', 12);
        }
        this.rule(x + 53, y + 248, width - 106);
        let filters;
        if (this.modal === 'saved') {
            filters = music.columns.slice(1);
        } else if (music.provider === 'qq') {
            filters = ['歌曲'];
        } else {
            filters = ['歌曲', '专辑', '歌单', '歌手'];
        }
        const selected =
            this.modal === 'saved'
                ? this.musicCollection - 1
                : ['song', 'album', 'playlist', 'artist'].indexOf(music.searchKind);
        filters.forEach((name, i) => {
            const spacing = Math.min(145, (width - 106) / 8);
            const xx = x + 53 + i * spacing;
            this.button(
                name,
                xx,
                y + 266,
                spacing - 5,
                34,
                this.modal === 'saved' ? 'music:collection:' + (i + 1) : 'music:kind:' + i,
                12,
            );
            if (i === selected) {
                this.rule(xx, y + 303, Math.min(110, (width - 106) / 8 - 10), 1);
            }
        });
        const lane = this.modal === 'saved' ? this.musicCollection : 0;
        if (music.lists[lane].some((v) => v.kind === 'song')) {
            ['原序', '歌手分组', '专辑分组'].forEach((label, i) => {
                const xx = x + width - 340 + i * 96;
                this.button(label, xx, y + 266, 94, 34, 'music:group:' + i, 12);
                if (this.groupBy === ['none', 'artist', 'album'][i]) {
                    this.rule(xx, y + 303, 85, 1);
                }
            });
        }
        if (this.activeGroup) {
            this.button('← 全部分组', x + 53, y + 315, 230, 54, 'music:group-back', 20, 'demibold');
        } else {
            this.text('ID / 编号', x + 53, y + 329, 10);
        }
        if (!this.activeGroup) {
            this.text('TITLE / 音乐', x + 160, y + 329, 10);
        }
        this.text('OPEN', x + width - 136, y + 329, 10);
        const rows = this.rows(lane);
        this.listOffset = Math.min(this.listOffset, Math.max(0, rows.length - 5));
        const signature = [
            this.modal,
            lane,
            music.keyword,
            music.searchKind,
            this.query,
            this.groupBy,
            this.activeGroup,
            this.listOffset,
            rows.signature,
        ].join('|');
        rows.slice(this.listOffset, this.listOffset + 5).forEach((entry, i) => {
            const motion = this.rowMotion(signature, i);
            const yy = y + (this.activeGroup ? 385 : 365) + i * 76 + motion.offset;
            this.save();
            this.alpha(motion.opacity);
            this.rule(x + 53, yy - 12, width - 106, 0.2);
            this.limited(entry[1].id, x + 53, yy, 12, 100);
            this.limited(entry[1].title, x + 160, yy, 20, width - 350, 'demibold');
            this.limited(
                (entry[1].kind === 'group' ? 'GROUP' : entry[1].kind.toUpperCase()) +
                    ' / ' +
                    (entry[1].artist || entry[1].album || ''),
                x + 160,
                yy + 30,
                11,
                width - 350,
            );
            this.text('查看 ↗', x + width - 136, yy + 8, 12);
            if (motion.opacity > 0.1) {
                this.hit(
                    x + 53,
                    yy - 10,
                    width - 106,
                    70,
                    entry[1].kind === 'group'
                        ? 'music:expand:' + encodeURIComponent(entry[1].key)
                        : 'music:result:' + lane + ':' + entry[0],
                    entry[1].title,
                );
            }
            this.restore();
        });
        if (!rows.length) {
            // 保持原有判断顺序，仅计算当前需要的分支结果。

            this.limited(emptyLibraryMessage(this.s, lane), x + 53, y + 501, 22, width - 106);
            if (lane > 0 && !music.uid) {
                this.button(
                    '登录' + (music.providerName || '网易云') + ' →',
                    x + 53,
                    y + 561,
                    300,
                    40,
                    'music:login',
                    16,
                );
            }
        }
        this.limited(music.status, x + 53, y + 785, 11, Math.max(100, width - 650));
        if (rows.length > 5) {
            this.button('上一页 ↑', x + width - 585, y + 775, 100, 35, 'music:page-prev', 12);
            this.text(
                this.listOffset + 1 + '–' + Math.min(rows.length, this.listOffset + 5) + ' / ' + rows.length,
                x + width - 475,
                y + 785,
                11,
            );
            if (this.listOffset + 5 < rows.length || music.hasMore[lane]) {
                this.button('下一页 ↓', x + width - 350, y + 775, 100, 35, 'music:page-next', 12);
            }
        }
        if (!music.sourceLoading && music.hasMore[lane]) {
            this.button('加载更多 ↓', x + width - 200, y + 775, 147, 35, 'music:more:' + lane, 12);
        }
    }

    discoverPanel(x, y, width) {
        const music = this.s;
        const discover = music.discover;
        const left = x + 53;
        const usable = width - 106;
        const tabs = [
            ['playlists', music.provider === 'qq' ? '榜单列表' : '推荐歌单'],
            ['daily', music.provider === 'qq' ? '首页推荐' : '每日推荐'],
            ['charts', '排行榜'],
            ['albums', '新专辑'],
            ['categories', '分类歌单'],
            ...(music.provider === 'qq' ? [] : [['artists', '收藏歌手']]),
            ['history', '历史'],
        ];
        tabs.forEach(([key, label], i) => {
            const xx = left + (i * usable) / tabs.length;
            this.button(label, xx, y + 195, usable / tabs.length - 4, 33, 'discover:tab:' + key, 12);
            if (discover.tab === key) {
                this.rule(xx, y + 231, usable / tabs.length - 15, 1);
            }
        });
        if (discover.tab === 'categories') {
            const cats = [
                {
                    id: '全部',
                    name: '全部',
                },
                ...discover.categories,
            ];
            const selected = cats.findIndex((v) => v.id === discover.category);
            const next = cats[(selected + 1) % cats.length];
            this.button(
                '分类：' + (cats[selected]?.name || '全部') + ' →',
                left,
                y + 244,
                usable,
                33,
                'discover:category:' + encodeURIComponent(next.id),
                12,
            );
        }
        this.rule(left, y + 286, usable, 0.5);
        this.listOffset = Math.min(this.listOffset, Math.max(0, discover.items.length - 6));
        discover.items.slice(this.listOffset, this.listOffset + 6).forEach((value, i) => {
            const index = this.listOffset + i;
            const yy = y + 307 + i * 67;
            this.text(pad(index + 1), left, yy + 8, 11);
            this.limited(value.title, left + 54, yy + 4, 20, usable * 0.6 - 65, 'demibold');
            this.limited(value.artist, left + usable * 0.64, yy + 8, 13, usable * 0.3 - 25);
            this.button(
                value.kind === 'song' ? '播放 ▶' : '打开 ↗',
                left + usable - 68,
                yy - 3,
                68,
                35,
                'discover:item:' + index,
                11,
            );
            this.rule(left, yy + 53, usable, 0.15);
            this.hit(left, yy - 4, usable, 60, 'discover:item:' + index, value.title);
        });
        if (!discover.items.length) {
            this.limited(discover.loading ? '正在读取音乐…' : discover.status, left, y + 434, 22, usable);
        }
        this.limited(discover.status, left, y + 755, 12, usable - 350);
        this.button('↑ 上一页', x + width - 340, y + 748, 95, 32, 'discover:prev', 12);
        this.button('下一页 ↓', x + width - 225, y + 748, 95, 32, 'discover:next', 12);
        if (discover.more) {
            this.button(
                discover.loading ? '载入中…' : '加载更多',
                x + width - 130,
                y + 748,
                80,
                32,
                'discover:more',
                12,
            );
        }
    }
    // 播放器固定在舞台下缘，使用几何图标保证视觉居中。
    playerBar(x, y, width) {
        const p = this.s.player;
        if (!p) {
            return;
        }
        const first = this.hits.length;
        const center = width / 2;
        const shade = this.ink.slice();
        shade[3] = 0.12;
        this.fill(0, y, width, 1080 - y, this.dark ? [0.075, 0.085, 0.075, 1] : [0.929, 0.922, 0.894, 1]);
        const pos = p.state.duration ? Math.min(1, p.state.position / p.state.duration) : 0;
        this.fill(0, y, width, 3, shade);
        this.fill(0, y, width * pos, 3, [119 / 255, 123 / 255, 96 / 255, 1]);
        this.playerSeekRect = {
            x: 0,
            y,
            width,
            height: 12,
        };
        this.hit(0, y, width, 12, 'player:open', '播放进度');
        const coverX = 28;
        const coverY = y + 17;
        if (p.coverImage) {
            this.image(p.coverImage, coverX, coverY, 64, 64);
        } else {
            this.op('ellipse', shade, coverX, coverY, 64, 64);
            this.op('ellipse', this.ink, coverX + 28, coverY + 28, 8, 8);
        }
        const infoWidth = Math.max(120, center - 280 - 116);
        this.centeredText(
            this.fit(p.failed ? p.status : p.current?.title || '尚未播放', 20, infoWidth, 'demibold'),
            116,
            y + 34,
            20,
            'demibold',
        );
        this.centeredText(
            this.fit(p.current?.artist || 'RHINE LAB / MUSIC DATABASE', 15, infoWidth),
            116,
            y + 64,
            15,
        );
        this.hit(28, y + 17, Math.max(200, center - 280 - 28), 64, 'player:open', '当前歌曲 / 播放队列');
        this.hit(center - 187, y + 20, 70, 44, 'player:mode', '播放模式');
        this.centeredText(
            {
                off: '顺序',
                all: '循环',
                one: '单曲',
                shuffle: '随机',
            }[p.mode],
            center - 152,
            y + 42,
            16,
            'regular',
            true,
        );
        this.hit(center - 99, y + 14, 44, 56, 'player:prev', '上一首');
        const cy = y + 42;
        const icon = [0.96, 0.95, 0.91, 1];
        this.op(
            'path',
            `M ${center - 72} ${cy - 10} L ${center - 82} ${cy} L ${center - 72} ${cy + 10}`,
            3,
            this.ink,
            [],
            0,
        );
        this.op(
            'path',
            `M ${center + 72} ${cy - 10} L ${center + 82} ${cy} L ${center + 72} ${cy + 10}`,
            3,
            this.ink,
            [],
            0,
        );
        this.op('ellipse', [119 / 255, 123 / 255, 96 / 255, 1], center - 29, cy - 29, 58, 58);
        this.hit(center - 29, cy - 29, 58, 58, 'player:toggle', '播放 / 暂停');
        if (p.loading) {
            for (let i = -1; i <= 1; i++) {
                this.op('ellipse', icon, center + i * 9 - 2, cy - 2, 4, 4);
            }
        } else if (p.state.playing) {
            this.fill(center - 9, cy - 11, 6, 22, icon);
            this.fill(center + 3, cy - 11, 6, 22, icon);
        } else {
            this.op(
                'fill-path',
                `M ${center - 10} ${cy - 12} L ${center + 10} ${cy} L ${center - 10} ${cy + 12} Z`,
                icon,
            );
        }
        this.hit(center + 55, y + 14, 44, 56, 'player:next', '下一首');
        const stamp = this.clock(p.state.position) + ' / ' + this.clock(p.state.duration);
        this.centeredText(stamp, center, y + 84, 13, 'regular', true);
        const vx = width - 239;
        this.playerVolumeRect = {
            x: vx,
            y: cy - 24,
            width: 86,
            height: 48,
        };
        this.centeredText('音量', vx - 54, cy, 15);
        for (let i = 0; i < 10; i++) {
            const height = 6 + i;
            this.fill(vx + i * 9, cy - height / 2, 4, height, i / 10 < p.state.volume ? this.ink : shade);
        }
        this.hit(width - 103, cy - 24, 70, 48, 'player:open', '播放队列');
        this.centeredText('队列', width - 68, cy, 18, 'regular', true);
        for (let i = first; i < this.hits.length; i++) {
            this.hits[i].fixed = true;
        }
    }

    clock(ms) {
        return pad(Math.floor((ms || 0) / 60000)) + ':' + pad(Math.floor((ms || 0) / 1000) % 60);
    }

    playerPanel(x, y, width) {
        const p = this.s.player;
        const left = x + 53;
        const rw = width - 106;
        this.rule(left, y + 216, rw);
        this.limited(p.current?.title || '选择歌曲开始播放', left, y + 240, 25, rw, 'bold');
        this.limited(p.current?.artist || '', left, y + 278, 15, rw);
        this.text('QUEUE / ' + p.queue.length + ' 首', left, y + 326, 15);
        this.button('清空待播', left + rw - 140, y + 311, 140, 44, 'player:clear', 16);
        this.queueOffset = Math.min(this.queueOffset, Math.max(0, p.queue.length - 7));
        p.queue.slice(this.queueOffset, this.queueOffset + 7).forEach((song, i) => {
            const index = this.queueOffset + i;
            const yy = y + 371 + i * 48;
            this.text(index === p.index ? '▶' : pad(index + 1), left, yy, 15);
            this.button(
                this.fit(song.title, 18, rw - 110),
                left + 44,
                yy - 7,
                rw - 110,
                40,
                'player:queue:' + index,
                18,
            );
            if (index !== p.index) {
                this.button('×', left + rw - 40, yy - 7, 40, 40, 'player:remove:' + index, 20);
            }
            this.rule(left, yy + 34, rw, 0.15);
        });
        this.limited(p.status, left, y + 730, 14, rw);
        this.button(
            {
                off: '顺序播放',
                all: '列表循环',
                one: '单曲循环',
                shuffle: '随机播放',
            }[p.mode],
            left,
            y + 761,
            140,
            44,
            'player:mode',
            16,
        );
        this.button(
            this.s.provider === 'qq' ? 'QQ 来源页面 ↗' : '官方客户端 ↗',
            left + 165,
            y + 761,
            175,
            44,
            'player:official',
            16,
        );
        if (this.s.editable !== false) {
            this.button('♡ 喜欢此曲', left + 365, y + 761, 140, 44, 'music:like-current', 16);
        }
    }

    get links() {
        return [
            [
                '本项目 / Rhine Music Database ↗',
                'project-home',
                'https://github.com/MikeQu223/Rhine-Music-Database',
            ],
            [
                '网易云音乐 API / api-enhanced ↗',
                'project-author',
                'https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced',
            ],
            [
                'QQ 音乐 API / qq-music-api ↗',
                'project-player',
                'https://github.com/yakult-green-tea/qq-music-api',
            ],
            ['界面原项目 / RhineLabUI ↗', 'project-rhine', 'https://github.com/LBEILC/RhineLabUI'],
        ];
    }

    settings(x, y, width) {
        const music = this.s;
        this.button(
            '平台：' + (music.providerName || '网易云') + ' ⇄',
            x + width - 300,
            y + 182,
            247,
            35,
            'provider',
            14,
        );
        this.button(
            music.uid ? '退出登录：' + music.nickname : '登录' + (music.providerName || '网易云') + ' →',
            x + 53,
            y + 182,
            400,
            35,
            music.uid ? 'music:logout' : 'music:login',
            12,
        );
        // 保持原有判断顺序，仅计算当前需要的分支结果。

        const rows = [
            ['COLOR THEME / 界面配色', themeLabel(this.themeMode, this.dark), 'theme'],
            ['REDUCED MOTION / 动态效果', this.reduced ? '减少动态效果' : '完整动效', 'motion'],
            [
                'AUDIO QUALITY / 音质',
                {
                    standard: '标准',
                    higher: '较高',
                    exhigh: '极高',
                    lossless: '无损',
                    hires: 'Hi-Res',
                    jymaster: '母带',
                }[music.player.quality],
                'quality',
            ],
            ...(music.provider === 'qq'
                ? []
                : [
                      [
                          'ALTERNATE SOURCE / 替代音源',
                          music.player.alternative ? '开启（音质由来源决定）' : '关闭',
                          'alternative',
                      ],
                  ]),
            [
                'OFFICIAL PLAYER / 官方窗口',
                this.host.activates() ? '播放时显示' : '尽量保持后台',
                'music:activate',
            ],
        ];
        rows.forEach((row, i) => {
            const yy = y + 264 + i * 72;
            this.rule(x + 53, yy - 13, width - 106, 0.2);
            this.text(row[0], x + 53, yy, 16, 'demibold');
            this.button(row[1], x + width - 252, yy - 1, 200, 35, row[2], 14);
        });
        this.text(
            '← → 切换来源 / ↑ ↓ 选曲 / ENTER 查看 / ' +
                (this.host.modifierKey ? this.host.modifierKey() : '⌘') +
                ' ENTER 播放 / ESC 返回',
            x + 53,
            y + 670,
            14,
        );
        this.text('PROJECT / 项目与开源参考', x + 53, y + 707, 11);
        this.links.forEach((link, i) =>
            this.button(link[0], x + 53, y + 734 + i * 26, 580, 26, 'music:' + link[1], 16),
        );
        this.button('切换全屏 / F11', x + width - 330, y + 653, 277, 45, 'window:fullscreen', 16);
        this.button('REINITIALIZE SYSTEM ↻', x + width - 330, y + 709, 277, 45, 'replay', 16);
        this.limited(music.status, x + 53, y + 846, 11, width - 106);
    }

    openModal(kind) {
        ++this.searchGeneration;
        this.r.pointer('pointercancel', 0, 0);
        this.r.pointer('pointerleave', 0, 0);
        this.modal = kind;
        this.closing = false;
        this.motion.set(true, this.lastTime, this.reduced);
        this.query = '';
        this.listOffset = 0;
        this.activeGroup = null;
        this.artistTab = 'songs';
        if (kind === 'saved') {
            this.musicCollection = Math.max(1, this.s.selectedLane);
            if (!this.s.lists[this.musicCollection].length) {
                this.s.loadMoreSource(this.musicCollection);
            }
        }
        if (kind === 'search') {
            this.query = this.s.keyword;
        }
    }

    closeModal() {
        ++this.searchGeneration;
        if (this.modal === 'login') {
            this.s.cancelLogin();
        }
        this.closing = true;
        this.motion.set(false, this.lastTime, this.reduced);
    }

    setQuery(value) {
        this.query = value;
        this.listOffset = 0;
        this.activeGroup = null;
        const token = ++this.searchGeneration;
        if (this.modal === 'search') {
            this.host.delay(700, () => {
                if (token !== this.searchGeneration) {
                    return;
                }
                this.s.search(value);
                if (this.s.provider === 'netease' && this.host.fromLink(value)) {
                    this.closeModal();
                }
            });
        }
    }
    /**
     * 将界面动作分派到音乐、播放和场景操作。
     * 保留原动作字符串和执行顺序，不在绘制过程中提交账号操作。
     */
    action(action) {
        const music = this.s;
        if (action.startsWith('tab:')) {
            this.trackOffset = 0;
            this.bodyOffset = 0;
            this.tab = action.slice(4);
            if (music.current?.kind === 'song' && this.tab !== 'overview') {
                music.sameAlbum();
            }
            return;
        }
        if (action === 'provider') {
            ++this.searchGeneration;
            this.activeGroup = null;
            this.query = '';
            this.listOffset = 0;
            this.trackOffset = 0;
            const provider = this.s.provider === 'qq' ? 'netease' : 'qq';
            this.s.switchProvider(provider).then(() => {
                if (this.s.provider !== provider) {
                    return;
                }
                if (this.modal === 'discover') {
                    this.s.discover.load();
                } else if (this.modal === 'saved') {
                    this.s.loadMoreSource(this.musicCollection);
                }
            });
            return;
        }
        if (action === 'confirm:yes') {
            const run = this.confirmAction;
            this.confirmAction = null;
            this.closeModal();
            run?.();
            return;
        }
        if (action.startsWith('discover:')) {
            const discover = music.discover;
            const parts = action.split(':');
            switch (parts[1]) {
                case 'tab':
                    this.listOffset = 0;
                    discover.load(parts[2]);
                    if (parts[2] === 'categories' && !discover.categories.length) {
                        discover.loadCategories();
                    }
                    break;
                case 'item':
                    discover.open(Number(parts[2]));
                    this.closeModal();
                    break;
                case 'category':
                    this.listOffset = 0;
                    discover.category = decodeURIComponent(parts[2]);
                    discover.load('categories');
                    break;
                case 'more':
                    discover.load(discover.tab, true);
                    break;
                case 'prev':
                    this.listOffset = Math.max(0, this.listOffset - 6);
                    break;
                case 'next':
                    this.listOffset = Math.min(Math.max(0, discover.items.length - 6), this.listOffset + 6);
                    break;
            }
            return;
        }
        if (action.startsWith('player:')) {
            const parts = action.split(':');
            const p = music.player;
            const n = Number(parts[2]);
            switch (parts[1]) {
                case 'open':
                    this.openModal('player');
                    break;
                case 'toggle':
                    p.toggle();
                    break;
                case 'prev':
                    p.next(-1);
                    break;
                case 'next':
                    p.next(1);
                    break;
                case 'seek':
                    p.seek(n / 20);
                    break;
                case 'seek-fraction':
                    p.seek(n);
                    break;
                case 'volume-fraction':
                    p.volume(n);
                    break;
                case 'volume':
                    p.volume(n / 10);
                    break;
                case 'mode':
                    p.mode = ['off', 'all', 'one', 'shuffle'][
                        (['off', 'all', 'one', 'shuffle'].indexOf(p.mode) + 1) % 4
                    ];
                    break;
                case 'queue':
                    if (p.queue[n]) {
                        p.play(p.queue[n]);
                    }
                    break;
                case 'remove':
                    p.remove(n);
                    break;
                case 'clear':
                    p.clearUpcoming();
                    break;
                case 'official':
                    music.playOfficial(p.current);
                    break;
            }
            return;
        }
        if (action === 'open') {
            if (music.current) {
                if (music.current.kind !== 'song') {
                    music.enterDirectory(music.current);
                } else {
                    this.r.setMode('detail');
                    music.loadSongWiki();
                }
            }
            return;
        }
        if (action === 'back' && music.directory) {
            if (this.frame.mode === 'detail') {
                music.returnToDirectory();
            } else {
                music.leaveDirectory();
            }
            return;
        }
        if ((action === 'column-prev' || action === 'column-next') && music.directory) {
            music.navigateDirectory(action === 'column-next' ? 8 : -8);
            return;
        }
        if (action === 'prev' || action === 'next') {
            music.next(action === 'next' ? 1 : -1);
            return;
        }
        if (action === 'column-prev' || action === 'column-next') {
            music.showSource((music.selectedLane + (action === 'column-next' ? 1 : 4)) % 5);
            return;
        }
        if (action.startsWith('music:')) {
            const parts = action.split(':');
            const last = Number(parts[parts.length - 1]);
            switch (parts[1]) {
                case 'slot': {
                    const discover = music.directory;
                    if (discover?.songs.length) {
                        music.selectDirectory(
                            (discover.page + ((music.selectedLane * 8 + last - discover.anchor + 40) % 40)) %
                                discover.songs.length,
                        );
                        return;
                    }
                    const n = music.libraryItems.length;
                    if (n) {
                        const index = (((music.offsets[music.selectedLane] + last) % n) + n) % n;
                        const raw =
                            music.groupView?.lane === music.selectedLane
                                ? music.groupView.indices[index]
                                : index;
                        music.selectResult(raw, music.selectedLane, false);
                        this.r.setMode('archive');
                    }
                    break;
                }
                case 'result':
                    music.selectResult(Number(parts[3]), Number(parts[2]));
                    this.tab = 'overview';
                    this.trackOffset = 0;
                    this.bodyOffset = 0;
                    this.closeModal();
                    break;
                case 'track':
                    if (music.directory?.songs[last]) {
                        music.selectDirectory(last, 'detail');
                    } else if (music.tracks[last]) {
                        music.choose(music.tracks[last]);
                    }
                    this.tab = 'overview';
                    this.trackOffset = 0;
                    this.bodyOffset = 0;
                    break;
                case 'parent':
                    music.leaveDirectory();
                    break;
                case 'directory-more':
                    if (music.directory) {
                        music.loadDirectoryPage();
                    } else {
                        music.loadTrackPage(this.trackOffset);
                    }
                    break;
                case 'directory-info':
                    music.showDirectoryInfo();
                    this.trackOffset = 0;
                    break;
                case 'directory':
                    if (music.directory) {
                        music.returnToDirectory();
                    } else if (music.current) {
                        music.enterDirectory(music.current);
                    }
                    break;
                case 'first-track':
                    if (music.tracks[this.trackOffset]) {
                        music.choose(music.tracks[this.trackOffset]);
                    }
                    this.tab = 'overview';
                    this.trackOffset = 0;
                    this.bodyOffset = 0;
                    break;
                case 'hint':
                    this.query = decodeURIComponent(action.slice('music:hint:'.length));
                    this.listOffset = 0;
                    music.search(this.query);
                    break;
                case 'kind':
                    ++this.searchGeneration;
                    this.listOffset = 0;
                    this.activeGroup = null;
                    {
                        const kind = ['song', 'album', 'playlist', 'artist'][last] || 'song';
                        const q = this.query || music.keyword;
                        if (q) {
                            music.search(q, kind);
                        } else {
                            music.searchKind = kind;
                        }
                    }
                    break;
                case 'group':
                    music.setGroupView?.(null, [], this.modal === 'saved' ? this.musicCollection : 0);
                    ++this.searchGeneration;
                    this.groupBy = ['none', 'artist', 'album'][last] || 'none';
                    this.activeGroup = null;
                    this.listOffset = 0;
                    break;
                case 'expand': {
                    ++this.searchGeneration;
                    const lane = this.modal === 'saved' ? this.musicCollection : 0;
                    const key = decodeURIComponent(action.slice('music:expand:'.length));
                    const group = this.rows(lane).find((e) => e[1].key === key)?.[1];
                    this.activeGroup = key;
                    this.listOffset = 0;
                    if (group) {
                        music.setGroupView?.(
                            group.title,
                            group.entries.map((e) => e[0]),
                            lane,
                        );
                    }
                    break;
                }
                case 'group-back':
                    music.setGroupView?.(null, [], this.modal === 'saved' ? this.musicCollection : 0);
                    ++this.searchGeneration;
                    this.activeGroup = null;
                    this.listOffset = 0;
                    break;
                case 'artist':
                    music.openSongArtist(last);
                    this.artistTab = 'songs';
                    break;
                case 'artist-tab':
                    this.artistTab = parts[2];
                    this.trackOffset = 0;
                    if (this.artistTab === 'albums') {
                        music.loadArtistAlbums();
                    }
                    break;
                case 'artist-album':
                    if (music.directory?.albums[last]) {
                        music.enterDirectory(music.directory.albums[last]);
                    }
                    this.artistTab = 'songs';
                    break;
                case 'artist-more':
                    music.loadArtistAlbums();
                    break;
                case 'collection':
                    this.activeGroup = null;
                    ++this.searchGeneration;
                    this.musicCollection = last;
                    this.listOffset = 0;
                    if (!music.lists[last].length) {
                        music.loadMoreSource(last);
                    }
                    break;
                case 'more':
                    music.loadMoreSource(last);
                    break;
                case 'page-prev':
                    this.pageResults(-1);
                    break;
                case 'page-next':
                    this.pageResults(1);
                    break;
                case 'play':
                    music.play();
                    break;
                case 'play-next':
                    music.player.enqueue(music.current, true);
                    break;
                case 'play-loaded':
                    music.playDirectory();
                    break;
                case 'remove-track':
                    if (music.directory) {
                        music.playlistTrack(music.directory.item.id, music.current, true);
                    }
                    break;
                case 'picker-prev':
                    this.listOffset = Math.max(0, this.listOffset - 7);
                    break;
                case 'picker-next':
                    this.listOffset = Math.min(Math.max(0, music.lists[2].length - 7), this.listOffset + 7);
                    break;
                case 'official':
                    music.playOfficial();
                    break;
                case 'enqueue':
                    music.player.enqueue(music.current);
                    break;
                case 'copy':
                    if (music.current) {
                        this.host.copy(music.current.id);
                        music.status = '已复制' + (music.providerName || '网易云') + ' ID';
                        music.changed();
                    }
                    break;
                case 'source':
                    if (music.current) {
                        this.host.open(music.sourceURL(music.current));
                    }
                    break;
                case 'project-home':
                case 'project-author':
                case 'project-player':
                case 'project-rhine': {
                    const link = this.links.find((l) => l[1] === parts[1]);
                    if (link) {
                        this.host.open(link[2]);
                    }
                    break;
                }
                case 'login':
                    this.openModal('login');
                    music.loginMethod = 'qr';
                    music.startLogin();
                    break;
                case 'login-method':
                    music.cancelLogin();
                    music.loginMethod = parts[2];
                    music.loginStatus = '';
                    if (['qr', 'wechat'].includes(music.loginMethod)) {
                        music.startLogin();
                    }
                    break;
                case 'like':
                    music.like();
                    break;
                case 'like-current':
                    music.like(music.player.current);
                    break;
                case 'subscribe':
                    music.subscribe();
                    break;
                case 'create-playlist':
                    this.formKind = 'create';
                    this.formPID = '';
                    this.openModal('playlist-form');
                    break;
                case 'delete-playlist': {
                    const value = music.directory?.item;
                    if (value?.creatorID === music.uid && music.uid) {
                        this.confirmTitle = '删除歌单：' + value.title;
                        this.confirmAction = () =>
                            music.mutate(
                                '/playlist/delete',
                                {
                                    id: value.id,
                                },
                                () => {
                                    music.lists[2] = music.lists[2].filter((v) => v.id !== value.id);
                                    music.totals[2] = music.lists[2].length;
                                    music.leaveDirectory();
                                },
                            );
                        this.openModal('confirm');
                    }
                    break;
                }
                case 'rename-playlist':
                    if (music.directory?.item.creatorID === music.uid) {
                        this.formKind = 'rename';
                        this.formPID = music.directory.item.id;
                        this.openModal('playlist-form');
                    }
                    break;
                case 'add-to-playlist':
                    this.openModal('playlist-picker');
                    if (!music.lists[2].length) {
                        music.loadMoreSource(2);
                    }
                    break;
                case 'playlist-pick':
                    Promise.resolve(music.playlistTrack(parts[2])).then((ok) => {
                        if (ok) {
                            this.closeModal();
                        }
                    });
                    break;
                case 'qr':
                    music.startLogin();
                    break;
                case 'logout':
                    music.logout();
                    break;
                case 'cover-front':
                    this.r.coverFront();
                    break;
                case 'cover-back':
                    this.r.coverBack();
                    break;
                case 'activate':
                    this.host.setActivates(!this.host.activates());
                    break;
            }
            return;
        }
        if (action === 'back') {
            music.observe(-1);
            music.observe(this.frame.selected);
        }
        if (action.startsWith('viewer:')) {
            this.r.viewerAction(action.slice(7));
            return;
        }
        switch (action) {
            case 'discover':
                this.openModal('discover');
                music.discover.load();
                break;
            case 'skip':
                this.r.setMode('archive');
                break;
            case 'back':
                this.r.setMode('archive');
                break;
            case 'search':
            case 'saved':
            case 'settings':
                this.openModal(action);
                break;
            case 'close-modal':
                this.closeModal();
                break;
            case 'viewer':
                this.r.openViewer();
                break;
            case 'replay':
                if (this.modal) {
                    this.closeModal();
                }
                this.r.setMode('boot');
                break;
            case 'alternative':
                music.player.alternative = !music.player.alternative;
                this.host.setPreference?.('alternate-source', music.player.alternative ? 'on' : 'off');
                break;
            case 'quality':
                music.player.quality = ['standard', 'higher', 'exhigh', 'lossless', 'hires', 'jymaster'][
                    (['standard', 'higher', 'exhigh', 'lossless', 'hires', 'jymaster'].indexOf(
                        music.player.quality,
                    ) +
                        1) %
                        6
                ];
                this.host.setPreference?.('quality', String(music.player.quality));
                break;
            case 'theme':
                this.themeMode = ['light', 'dark', 'system'][
                    (['light', 'dark', 'system'].indexOf(this.themeMode) + 1) % 3
                ];
                this.dark =
                    this.themeMode === 'system' ? !!this.host.systemDark?.() : this.themeMode === 'dark';
                this.r.theme(this.dark);
                this.host.setPreference?.('theme', this.themeMode);
                break;
            case 'motion':
                this.reduced = !this.reduced;
                this.r.reduced(this.reduced);
                this.host.setPreference?.('motion', this.reduced ? 'reduced' : 'full');
                break;
        }
    }

    key(key, command) {
        if (key === 'Enter' && command) {
            this.s.play();
            return;
        }
        if (this.frame.viewer) {
            this.r.viewerKey(key);
            return;
        }
        if (key === 'Escape') {
            this.action(this.modal ? 'close-modal' : 'back');
            return;
        }
        if (this.modal) {
            return;
        }
        if (key === ' ') {
            this.s.player.toggle();
            return;
        }
        const actions = {
            ArrowLeft: 'column-prev',
            ArrowRight: 'column-next',
            ArrowUp: 'prev',
            ArrowDown: 'next',
            Enter: 'open',
            '/': 'search',
        };
        if (actions[key]) {
            this.action(actions[key]);
        }
    }

    async pageResults(direction) {
        if (this.closing || !['search', 'saved'].includes(this.modal)) {
            return;
        }
        const modal = this.modal;
        const lane = modal === 'saved' ? this.musicCollection : 0;
        const token = this.searchGeneration;
        const desired = Math.max(0, this.listOffset + direction * 5);
        // 下一页在末尾等待补页；查询、类别或弹窗变化后不再移动旧列表。
        if (
            direction > 0 &&
            desired > Math.max(0, this.rows(lane).length - 5) &&
            this.s.hasMore[lane] &&
            !this.s.sourceLoading
        ) {
            await this.s.loadMoreSource(lane);
        }
        if (
            this.modal !== modal ||
            this.closing ||
            this.searchGeneration !== token ||
            (modal === 'saved' && lane !== this.musicCollection)
        ) {
            return;
        }
        this.listOffset = Math.min(desired, Math.max(0, this.rows(lane).length - 5));
    }

    wheel(x, y, delta, steps = undefined) {
        if (!Number.isFinite(delta) || delta === 0) {
            return;
        }
        const step = steps ?? (delta < 0 ? 1 : -1);
        const music = this.s;
        if (this.frame.viewer) {
            this.r.pointer('wheel', x, y, -delta * 8);
            return;
        }
        if (this.modal) {
            if (this.modal === 'playlist-picker') {
                this.listOffset = Math.max(
                    0,
                    Math.min(Math.max(0, music.lists[2].length - 7), this.listOffset + step),
                );
                return;
            }
            if (this.modal === 'discover') {
                this.listOffset = Math.max(
                    0,
                    Math.min(Math.max(0, music.discover.items.length - 6), this.listOffset + step),
                );
                return;
            }
            if (this.modal === 'player') {
                this.queueOffset = Math.max(
                    0,
                    Math.min(Math.max(0, music.player.queue.length - 7), this.queueOffset + step),
                );
                return;
            }
            if (this.closing || !['search', 'saved'].includes(this.modal)) {
                return;
            }
            const { width, x: left } = this.modalBounds();
            y -= this.motion.offset;
            if (x < left + 53 || x > left + width - 53 || y < 475 || y > 867) {
                return;
            }
            const lane = this.modal === 'saved' ? this.musicCollection : 0;
            const rows = this.rows(lane);
            this.listOffset = Math.max(0, Math.min(Math.max(0, rows.length - 5), this.listOffset + step));
            if (
                step > 0 &&
                this.listOffset + 5 >= rows.length &&
                !music.sourceLoading &&
                music.hasMore[lane]
            ) {
                music.loadMoreSource(lane);
            }
            return;
        }
        if (this.frame.mode === 'detail' && music.current?.kind !== 'song') {
            const l = this.containerLayout();
            if (x < l.x || x > l.x + l.width || y < l.rowsY || y > l.rowsY + l.rowCount * l.rowHeight) {
                return;
            }
            const albums = music.directory?.item.kind === 'artist' && this.artistTab === 'albums';
            const count = albums
                ? music.directory.albums.length
                : (music.directory?.songs.length ?? music.tracks.length);
            this.trackOffset = Math.max(
                0,
                Math.min(Math.max(0, count - l.rowCount), this.trackOffset + step),
            );
            if (this.trackOffset + l.rowCount >= count) {
                if (albums) {
                    music.loadArtistAlbums();
                } else if (music.directory) {
                    music.loadDirectoryPage();
                } else {
                    music.loadTrackPage(this.trackOffset);
                }
            }
            return;
        }
        if (this.frame.mode === 'detail') {
            if (this.tab === 'history') {
                this.trackOffset = Math.max(
                    0,
                    Math.min(Math.max(0, music.tracks.length - 2), this.trackOffset + step),
                );
                music.loadTrackPage(this.trackOffset);
            } else {
                const width = this.w * 0.44 - 59;
                this.bodyOffset = Math.max(
                    0,
                    Math.min(
                        Math.max(0, this.g.wrap(this.body(), width, 16).length - 3),
                        this.bodyOffset + step,
                    ),
                );
            }
            return;
        }
        if (this.frame.mode === 'archive') {
            if (step) {
                music.next(step);
            }
        } else {
            this.r.pointer('wheel', x, y, -delta * 8);
        }
    }

    logo(x, y, width) {
        this.save();
        this.translate(x, y);
        this.scale(width / 310, width / 310);
        this.op(
            'path',
            'M156 75C127 48 103 15 70 15C37 15 15 39 15 70C15 101 38 128 70 128C103 128 127 96 176 52M155 75C182 99 208 128 240 128C273 128 295 105 295 73C295 41 273 15 240 15C221 15 207 23 192 38',
            26,
            this.ink,
            [],
            0,
        );
        this.op('path', 'M44 70H94M69 45V95M219 70H263', 15, this.ink, [], 0);
        this.restore();
    }

    boot(w) {
        const b = this.frame.boot;
        if (!b) {
            return;
        }
        const num = (key) => Number(b[key] || 0);
        const offset = (w - 1920) / 2;
        this.save();
        this.translate(offset, 0);
        if (num('backgroundOpacity') > 0) {
            this.save();
            this.alpha(num('backgroundOpacity'));
            this.op('gradient');
            this.restore();
            this.fill(-offset, 0, w, 1080, [1, 1, 1, num('white')]);
        }
        if (num('accessOpacity') > 0) {
            this.save();
            this.alpha(num('accessOpacity'));
            this.art('access', 800, 532, 18, (b.access || '').length);
            this.restore();
        }
        if (num('logoOpacity') > 0 && b.logo) {
            const l = b.logo;
            this.save();
            this.alpha(num('logoOpacity'));
            this.translate(518 + l.offsetX, 463);
            this.scale(296 / 310, 296 / 310);
            const length = BOOT_LENGTH;
            const fraction = Math.max(0.000001, Math.min(1, l.length));
            this.op(
                'path',
                BOOT_CONTOUR,
                l.strokeWidth,
                this.ink,
                [fraction * length, (1 - fraction) * length],
                -l.start * length,
            );
            if (l.symbolScale > 0) {
                this.save();
                this.translate(l.plusX, 70);
                this.op('rotate', (l.plusAngle * Math.PI) / 180);
                this.scale(l.symbolScale, l.symbolScale);
                this.translate(-69, -70);
                this.op('path', 'M44 70H94M69 45V95', 15, this.ink, [], 0);
                this.restore();
                this.save();
                this.translate(l.minusX, 70);
                this.scale(l.symbolScale, l.symbolScale);
                this.op('path', 'M' + -l.minusWidth / 2 + ' 0H' + l.minusWidth / 2, 15, this.ink, [], 0);
                this.restore();
            }
            this.text(b.logoLetters || '', 20, 174 - 16 * 1.044, 16, 'bold', 22);
            this.restore();
        }
        if (num('authOpacity') > 0) {
            this.save();
            this.alpha(num('authOpacity'));
            this.text('▪', 907, 528, 14);
            const auth = b.auth || '';
            let key;
            if (auth.startsWith('ID')) {
                key = 'identity';
            } else if (auth.startsWith('REQUEST')) {
                key = 'request';
            } else if (auth.startsWith(' ')) {
                key = 'processingGlitch';
            } else {
                key = 'processing';
            }
            const size = 21.35;
            const tracking = -0.00909 * size;
            const prefix = 'ID CONFIRMED : ';
            const count = key === 'identity' ? Math.min(auth.length, prefix.length) : auth.length;
            this.art(key, 931, 527, size, count, tracking);
            if (key === 'identity' && auth.length >= prefix.length) {
                // 用户名沿用原姓名逐字出现的帧区间；无记录时绘制五个实心方块。
                const nickname = this.s.identityName || '';
                const characters = Array.from(nickname);
                const length = characters.length || 5;
                const frame = Number(b.f || 0);
                const visible =
                    frame < 321 ? 0 : Math.min(length, 1 + Math.floor(((frame - 321) * (length - 1)) / 18));
                const x = 931 + this.g.artWidth('identity', size, tracking, prefix.length) + tracking;
                if (nickname) {
                    const bounds = this.g.bounds(nickname, size, 'regular');
                    const scale = Math.min(
                        1,
                        650 / this.g.width(nickname, size),
                        (size * 0.7) / bounds.height,
                    );
                    const nameSize = size * scale;
                    // 按实际字体轮廓对齐到原英文字母的中线，避免中文昵称基线偏低。
                    const y = 527 + size * 0.45 - (bounds.y + bounds.height / 2) * scale;
                    this.text(characters.slice(0, visible).join(''), x, y, nameSize);
                } else {
                    for (let i = 0; i < visible; i++) {
                        this.fill(x + i * 18, 530, 14, 14);
                    }
                }
            }
            this.restore();
        }
        if (b.scanVisible) {
            this.scan(b);
        }
        if (b.welcomeVisible) {
            this.save();
            this.alpha(num('welcomeOpacity'));
            this.translate(960, 552.6);
            this.scale(num('welcomeScale'), num('welcomeScale'));
            this.translate(-960, -552.6);
            const ink = this.ink.slice();
            ink[3] = num('welcomePanel');
            this.fill(725, 344, 470, 392, ink);
            const white = 1 - num('welcomeInk');
            this.art('welcome', 960 - this.g.artWidth('welcome', 55) / 2, 403, 55, -1, 0, [
                white,
                white,
                white,
                1,
                'calibratedWhite',
            ]);
            if (b.companyVisible) {
                const mask = b.companyMask;
                this.save();
                this.alpha(mask ? 0.65 : 1);
                const companyWidth = this.g.artWidth('company', 48, -0.5);
                this.art('company', 960 - companyWidth / 2, 475, 48, -1, -0.5);
                this.save();
                this.clip(755, 470, 410 * num('highlight'), 60);
                this.fill(755, 470, 410, 60);
                this.art('company', 960 - companyWidth / 2, 475, 48, -1, -0.5, [
                    238 / 255,
                    237 / 255,
                    232 / 255,
                    mask ? 0.06 : 1,
                ]);
                this.restore();
                this.restore();
            }
            this.save();
            this.alpha(num('databaseOpacity'));
            this.art('database', 960 - this.g.artWidth('database', 36) / 2, 537, 36);
            this.restore();
            if (b.welcomeLogo) {
                this.logo(858.5, 590, 203);
            }
            this.restore();
        }
        this.restore();
        for (let i = 0; i < (b.brand || []).length; i++) {
            const line = b.brand[i];
            this.save();
            this.alpha(line.opacity || 0);
            this.translate(line.x || 0, 0);
            this.brandLine(i);
            this.restore();
        }
        if (num('poweredLetters') > 0) {
            this.save();
            this.clip(w - 354, 945, (295 * num('poweredLetters')) / 19, 35);
            this.text('POWERED BY', w - 354, 947, 19);
            this.text('RHINE LAB', w - 218, 947, 19, 'bold');
            this.fill(w - 84, 955, 25, 6);
            this.restore();
        } // 入口独占下一行，避免与逐字出现的 POWERED BY 署名重叠；点击区域同步下移。
        this.button('ENTER SYSTEM ↗', w - 261, 994, 202, 40, 'skip', 14);
    }

    scan(b) {
        const music = b.scan;
        const o = b.scanOrbit;
        if (!music || !o) {
            return;
        }
        const arc = (r, start, sweep, x = 960, y = 540, width = 2, color = this.ink) => {
            if (r > 0 && sweep > 0) {
                this.op('arc', x, y, r, start, sweep, width, color);
            }
        };
        const dot = (x, y, r, color = this.ink) => {
            if (r > 0) {
                this.op('ellipse', color, x - r, y - r, r * 2, r * 2);
            }
        };
        this.save();
        this.translate(960, 540);
        this.scale(b.ringScale, b.ringScale);
        this.translate(-960, -540);
        this.alpha(b.ringOpacity);
        arc(music.radius, music.outerStart, music.outerSweep, 960, 540, 2.4);
        arc(music.whiteRadius, music.whiteStart, music.whiteSweep, 960, 540, 4, [1, 1, 1, 1]);
        arc(music.innerRadius, music.innerStart, music.innerSweep);
        arc(music.innerRadius, music.innerStart + Math.PI, music.innerSweep);
        if (o.sideVisible) {
            for (const side of o.sides || []) {
                arc(side.radius, side.start, side.sweep, side.x, side.y);
            }
        }
        for (const p of o.satellites || []) {
            dot(p.x, p.y, p.radius);
        }
        if (b.ornament) {
            dot(959.5, 539.5, b.coreRadius);
        }
        for (let i = 0; i < 2; i++) {
            const angle = music.orbit + i * Math.PI;
            dot(
                960 + Math.cos(angle) * music.orbitRadius,
                540 + Math.sin(angle) * music.orbitRadius,
                music.dotRadius,
                [237 / 255, 130 / 255, 27 / 255, 1],
            );
        }
        const black = music.outerStart + music.outerSweep;
        const white = music.whiteStart;
        dot(960 + Math.cos(black) * music.radius, 540 + Math.sin(black) * music.radius, music.blackCap);
        dot(
            960 + Math.cos(white) * music.whiteRadius,
            540 + Math.sin(white) * music.whiteRadius,
            music.whiteCap,
            [1, 1, 1, 1],
        );
        this.restore();
        this.save();
        this.alpha(b.permissionOpacity);
        this.art(
            'permission',
            960 - this.g.artWidth('permission', b.scanFont, b.scanTracking) / 2,
            540 - b.scanFont / 2,
            b.scanFont,
            -1,
            b.scanTracking,
        );
        this.restore();
    }
}
const BOOT_CONTOUR =
    'M295 73C295 41 273 15 240 15C221 15 207 23 192 38C186 43 181 47 176 52C127 96 103 128 70 128C38 128 15 101 15 70C15 39 37 15 70 15C103 15 127 48 156 75C182 99 208 128 240 128C273 128 295 105 295 73Z';
// 原实现逐曲线 512 次采样的轮廓长度，避免不同绘图库的长度近似改变开场描边。
function contourLength() {
    const nums = BOOT_CONTOUR.match(/[-+]?[0-9]*\.?[0-9]+/g).map(Number);
    let point = [nums[0], nums[1]];
    let total = 0;
    for (let i = 2; i < nums.length; i += 6) {
        const p = point.slice();
        const a = nums.slice(i, i + 2);
        const b = nums.slice(i + 2, i + 4);
        const discover = nums.slice(i + 4, i + 6);
        for (let j = 1; j <= 512; j++) {
            const t = j / 512;
            const music = 1 - t;
            const next = [0, 1].map(
                (k) =>
                    music * music * music * p[k] +
                    3 * music * music * t * a[k] +
                    3 * music * t * t * b[k] +
                    t * t * t * discover[k],
            );
            total += Math.hypot(next[0] - point[0], next[1] - point[1]);
            point = next;
        }
    }
    total += Math.hypot(point[0] - nums[0], point[1] - nums[1]);
    return total;
}
const BOOT_LENGTH = contourLength();
