// 资料库状态编排：选择、分页、账号和播放联动；请求及条目解析位于独立模块。
import { QQAPI } from './qq.js';
import { Player } from './player.js';
import { Discover } from './discover.js';
import { API } from './music-api.js';
import { safeLoginDetails, loginFailureMessage } from './login-state.js';
import { item, parse } from './music-items.js';
export { API } from './music-api.js';
export { item, parse, pad, time } from './music-items.js';
const unique = (existing, values) => {
    const keys = new Set(existing.map((x) => x.key));
    return existing.concat(
        values.filter((value) => {
            if (keys.has(value.key)) {
                return false;
            }
            keys.add(value.key);
            return true;
        }),
    );
};
const ordered = (ids, songs) => {
    const map = {};
    for (const s of songs) {
        if (!map[s.id]) {
            map[s.id] = s;
        }
    }
    return ids.map((id) => map[id]).filter(Boolean);
};
const cursor = (songs, ids) => {
    const i = songs.length ? ids.indexOf(songs[songs.length - 1].id) : -1;
    return i < 0 ? songs.length : i + 1;
};
export class MusicStore {
    /**
     * 建立资料库状态、播放器和各类请求的失效计数。
     * 初始化顺序保持一致；账号凭据和系统播放仍由 Host 处理。
     */
    constructor(host, runtime, content) {
        this.host = host;
        this.provider = host.preference?.('provider') === 'qq' ? 'qq' : 'netease';
        const base = new API(host);
        this.api = this.provider === 'qq' ? new QQAPI(base) : base;
        host.setProvider?.(this.provider);
        this.player = new Player(host, this.api, () => this.changed());
        this.runtime = runtime;
        this.content = content;
        this.player.onTrack = (value) => this.followPlayback(value);
        // 每类资料库独立保存列表、分页位置和选中行。
        this.columns = ['搜索结果', '喜欢的歌曲', '我的歌单', '收藏歌单', '收藏专辑'];
        this.lists = [[], [], [], [], []];
        this.totals = [0, 0, 0, 0, 0];
        this.offsets = [0, 0, 0, 0, 0];
        this.hasMore = [true, true, true, true, true];
        this.rowMemory = [0, 0, 0, 0, 0];
        this.current = null;
        this.tracks = [];
        this.trackIDs = [];
        this.details = {};
        this.searchKind = 'song';
        this.keyword = '';
        this.status = '输入关键词检索音乐';
        this.uid = '';
        this.nickname = '未登录';
        this.identityNames = {
            netease: host.preference?.('welcome-name-netease') || '',
            qq: host.preference?.('welcome-name-qq') || '',
        };
        this.selectedPhysical = -1;
        this.selectedLane = 0;
        this.detailLoading = false;
        this.coverImage = '';
        this.coverKey = '';
        this.qr = '';
        this.qrKey = '';
        this.loginStatus = '';
        this.accountLoading = false;
        this.loginGeneration = 0;
        this.loginPhase = 'idle';
        this.loginCooldowns = {};
        this.loginCancellations = {};
        this.restoreCachedAccount();
        // 不同操作分别推进代数，防止搜索、账号或目录的迟到响应互相覆盖。
        this.wantsSongDetails = false;
        this.directory = null;
        this.directoryLoading = false;
        this.directoryInfo = false;
        this.directoryGeneration = 0;
        this.generation = 0;
        this.selectionGeneration = 0;
        this.accountGeneration = 0;
        this.searchLoading = false;
        this.personalLoading = false;
        this.searchCursor = 0;
        this.playlistCursor = 0;
        this.albumCursor = 0;
        this.likedCursor = 0;
        this.trackCursor = 0;
        this.likedIDs = null;
        this.albumLoaded = false;
        this.trackLoad = false;
        this.wikiGeneration = 0;
        this.physicalSlots = content.columns.map((category) =>
            content.records.map((r, i) => (r.category === category ? i : -1)).filter((i) => i >= 0),
        );
        // 本地偏好只恢复界面历史，账号凭据仍由 Host 管理。
        this.writeLoading = false;
        this.loginMethod = 'qr';
        this.subscriptions = new Set();
        this.searchHistory = [];
        this.suggestions = [];
        this.suggestGeneration = 0;
        try {
            this.searchHistory = JSON.parse(host.preference?.('search-history') || '[]')
                .filter((v) => typeof v === 'string')
                .slice(0, 12);
        } catch (error) {
            // 本地搜索历史无效时保持空列表，不影响音乐检索。
        }
        this.discover = new Discover(this);
        this.groupView = null;
        this.onChange = () => {};
    }
    // 播放切歌复用资料库选中动画：旧唱片插回、资料库换位、新唱片取出。
    /**
     * 让播放切歌同步到资料库选中项和详情。
     * 保留原目录返回路径，并复用插回、换位和取出的动画。
     */
    followPlayback(value) {
        if (this.current?.key === value.key && this.current?.id === value.id) {
            return;
        }
        const previous = this.current;
        const physical =
            this.selectedPhysical >= 0
                ? this.selectedPhysical
                : (this.frame?.selected ?? this.physicalSlots[this.selectedLane][0]);
        let directory = this.directory;
        let index = directory?.songs.findIndex((song) => song.id === value.id) ?? -1;
        if (index < 0) {
            ++this.directoryGeneration;
            this.directoryLoading = false;
            const songs = this.player.queue.slice();
            const row = this.physicalSlots[this.selectedLane].indexOf(physical);
            index = songs.findIndex((song) => song.id === value.id);
            directory = this.directory = {
                parentDirectory: this.directory,
                item: {
                    kind: 'playlist',
                    key: 'playback:queue',
                    id: '',
                    title: '播放队列',
                    trackCount: songs.length,
                },
                songs,
                ids: songs.map((song) => song.id),
                cursor: songs.length,
                index,
                anchor: this.selectedLane * 8 + Math.max(0, row),
                page: 0,
                parentPhysical: physical,
                parentLane: this.selectedLane,
                parentItem: previous,
                albums: [],
            };
        }
        directory.songs[index] = value;
        const cell = (index + directory.anchor) % 40;
        const target = this.physicalSlots[Math.floor(cell / 8)][cell % 8];
        this.selectDirectory(
            index,
            'detail',
            target === physical ? 'row' : null,
            target === physical ? 8 : 1,
        );
        if (this.frame) {
            this.frame.mode = 'detail';
        }
    }

    get identityName() {
        return this.identityNames.netease || this.identityNames.qq;
    }

    rememberIdentityName(provider, name) {
        const nickname = String(name || '').trim();
        if (!nickname) {
            return;
        }
        this.identityNames[provider] = nickname;
        this.host.setPreference?.('welcome-name-' + provider, nickname);
        if (provider === this.provider && this.uid) {
            this.host.setPreference?.('account-id-' + provider, this.uid);
        }
    }

    /**
     * 为旧版保存的另一来源凭据补齐身份确认行用户名，不切换当前音乐来源。
     * 只在缺少历史用户名时查询；来源或账号变化后丢弃迟到结果。
     */
    async restoreIdentityName() {
        const provider = this.provider === 'netease' ? 'qq' : 'netease';
        if (this.identityNames[provider] || !this.host.hasCredential?.(provider)) {
            return;
        }
        const token = this.accountGeneration;
        const valid = () => token === this.accountGeneration;
        const base = new API(this.host);
        const api = provider === 'qq' ? new QQAPI(base) : base;
        try {
            const [, name] = await api.account(valid);
            if (!valid()) {
                return;
            }
            this.rememberIdentityName(provider, name);
            this.changed();
        } catch (error) {
            // 旧凭据过期或网络不可用时保留已有记录，缺少记录时仍显示方块。
        }
    }

    // 只恢复具有本机凭据的账号显示，服务器验证仍由 restoreAccount 完成。
    restoreCachedAccount() {
        const hasCredential = this.host.authenticated?.() || false;
        this.uid = hasCredential ? this.host.preference?.('account-id-' + this.provider) || '' : '';
        this.nickname = hasCredential ? this.identityNames[this.provider] || '正在恢复登录…' : '未登录';
    }

    get loginRetrySeconds() {
        const deadline = this.loginCooldowns[this.provider]?.deadline || 0;
        return Math.max(0, Math.ceil((deadline - Date.now()) / 1000));
    }

    get canRegenerateLogin() {
        return this.loginRetrySeconds === 0 && !['preparing', 'confirming'].includes(this.loginPhase);
    }

    /**
     * 保存当前来源的安全失败摘要，并遵守服务给出的冷却期限。
     * 倒计时只更新失败提示，不允许旧计时器覆盖新登录或另一来源的状态。
     */
    reportLoginFailure(details) {
        this.loginDiagnostics = safeLoginDetails(details);
        const message = loginFailureMessage(details);
        const retryAfterMs = Math.max(0, Number(this.loginDiagnostics.retryAfterMs) || 0);
        const provider = this.provider;
        this.loginPhase = 'failed';
        this.loginStatus = message;
        if (retryAfterMs > 0) {
            const cooldown = { deadline: Date.now() + retryAfterMs, message };
            this.loginCooldowns[provider] = cooldown;
            this.updateLoginCooldown(provider, cooldown);
        }
        this.changed();
    }

    async updateLoginCooldown(provider, cooldown) {
        while (this.loginCooldowns[provider] === cooldown) {
            const seconds = Math.max(0, Math.ceil((cooldown.deadline - Date.now()) / 1000));
            if (provider === this.provider && this.loginPhase === 'failed') {
                this.loginStatus =
                    cooldown.message + (seconds ? '（请等待 ' + seconds + ' 秒）' : '，可重新生成');
                this.changed();
            }
            if (!seconds) {
                return;
            }
            await this.api.delay(1000);
        }
    }

    get providerName() {
        return this.provider === 'qq' ? 'QQ 音乐' : '网易云';
    }

    get editable() {
        return this.provider === 'netease';
    }
    /**
     * 切换音乐来源并作废旧来源尚未完成的请求。
     * 先停止登录与播放，再清理状态并建立新接口，避免迟到回调恢复旧内容。
     */
    async switchProvider(provider) {
        if (!['netease', 'qq'].includes(provider) || provider === this.provider) {
            return;
        }
        this.cancelLogin();
        ++this.generation;
        ++this.accountGeneration;
        ++this.selectionGeneration;
        ++this.directoryGeneration;
        ++this.wikiGeneration;
        ++this.suggestGeneration;
        this.discover.cancel();
        this.player.stop();
        this.runtime.musicClear?.();
        this.provider = provider;
        this.host.setProvider?.(provider);
        this.host.setPreference?.('provider', provider);
        this.api = provider === 'qq' ? new QQAPI(new API(this.host)) : new API(this.host);
        this.player.api = this.api;
        this.restoreCachedAccount();
        this.loginStatus = '';
        this.loginDiagnostics = {};
        this.current = null;
        this.directory = null;
        this.groupView = null;
        this.details = {};
        this.tracks = [];
        this.trackIDs = [];
        this.coverImage = '';
        this.coverKey = '';
        this.lists = [[], [], [], [], []];
        this.totals = [0, 0, 0, 0, 0];
        this.offsets = [0, 0, 0, 0, 0];
        this.hasMore = [true, true, true, true, true];
        this.rowMemory = [0, 0, 0, 0, 0];
        this.likedIDs = null;
        this.likedCursor = 0;
        this.albumCursor = 0;
        this.playlistCursor = 0;
        this.searchCursor = 0;
        this.searchLoading = false;
        this.personalLoading = false;
        this.detailLoading = false;
        this.directoryLoading = false;
        this.trackLoad = false;
        this.writeLoading = false;
        this.subscriptions.clear();
        this.searchKind = 'song';
        this.keyword = '';
        this.suggestions = [];
        this.selectedPhysical = -1;
        this.selectedLane = 0;
        this.privateSelection = false;
        this.loginMethod = 'qr';
        this.status = '已切换至' + this.providerName;
        this.changed();
        await this.restoreAccount();
    }

    call(name, ...args) {
        return this.runtime[name](...args);
    }

    changed() {
        this.revision = (this.revision ?? 0) + 1;
        this.onChange();
    }

    get sourceLoading() {
        return this.searchLoading || this.personalLoading;
    }

    get libraryName() {
        return (
            this.directory?.item.title ??
            (this.groupView?.lane === this.selectedLane
                ? this.groupView.name
                : this.columns[this.selectedLane])
        );
    }

    sourceItems(lane) {
        return this.groupView?.lane === lane
            ? this.groupView.indices.map((i) => this.lists[lane][i]).filter(Boolean)
            : this.lists[lane];
    }

    setGroupView(name, indices = [], lane = 0) {
        this.groupView = name
            ? {
                  name,
                  indices,
                  lane,
              }
            : null;
        this.offsets[lane] = 0;
        this.rowMemory[lane] = 0;
        this.showSource(lane);
    }

    get libraryItems() {
        return this.directory?.songs ?? this.sourceItems(this.selectedLane);
    }

    get libraryTotal() {
        const directory = this.directory;
        if (directory) {
            return Math.max(directory.ids.length, directory.item.trackCount, directory.songs.length);
        } else if (this.groupView?.lane === this.selectedLane) {
            return this.sourceItems(this.selectedLane).length;
        } else {
            return this.totals[this.selectedLane];
        }
    }

    get libraryIndex() {
        if (this.directory) {
            return this.directory.index;
        }
        const row = Math.max(0, this.physicalSlots[this.selectedLane].indexOf(this.selectedPhysical));
        const n = Math.max(1, this.sourceItems(this.selectedLane).length);
        return (((this.offsets[this.selectedLane] + row) % n) + n) % n;
    }

    observe(selected) {
        if (this.directory) {
            this.observeDirectory(selected);
            return;
        }
        if (this.selectedPhysical === selected) {
            return;
        }
        this.selectedPhysical = selected;
        const lane = this.physicalSlots.findIndex((a) => a.includes(selected));
        if (lane < 0) {
            return;
        }
        const row = this.physicalSlots[lane].indexOf(selected);
        this.selectedLane = lane;
        this.rowMemory[lane] = row;
        const values = this.sourceItems(lane);
        const n = values.length;
        const index = n ? (((this.offsets[lane] + row) % n) + n) % n : 0;
        this.choose(n ? values[index] : null);
    }

    async choose(input, containerDetails = false, showDetails = undefined) {
        const token = ++this.selectionGeneration;
        ++this.wikiGeneration;
        this.wantsSongDetails = showDetails ?? this.frame?.mode === 'detail';
        this.albumLoaded = false;
        this.trackCursor = 0;
        this.trackLoad = false;
        this.current = input;
        this.call('musicIdentity', input?.key ?? '');
        this.tracks = [];
        this.trackIDs = [];
        if (this.coverKey !== input?.key) {
            this.coverImage = '';
            this.coverKey = '';
        }
        this.detailLoading = input !== null;
        this.changed();
        if (!input) {
            return;
        }
        this.current = {
            ...(this.details[input.key] ?? input),
        };
        this.loadCover(this.current);
        this.detailLoading = true;
        if (input.kind !== 'song' && !containerDetails) {
            this.detailLoading = false;
            this.changed();
            return;
        }
        // 搜索接口可能只给专辑 ID 和时长；缺少封面时仍需补全歌曲详情。
        // 已请求过的详情允许没有封面，避免无封面歌曲被反复请求。
        if (
            input.kind === 'song' &&
            this.current.duration > 0 &&
            this.current.albumID &&
            (this.current.cover || this.details[input.key])
        ) {
            this.details[input.key] = {
                ...this.current,
            };
            this.detailLoading = false;
            if (this.wantsSongDetails) {
                this.loadSongWiki();
            }
            this.changed();
            return;
        }
        try {
            await this.api.delay(300);
            if (token !== this.selectionGeneration) {
                return;
            }
            const [fetched, list, ids] = await this.api.detail(
                input,
                () => token === this.selectionGeneration,
            );
            if (token !== this.selectionGeneration) {
                return;
            }
            const value = {
                ...fetched,
            };
            if (value.kind === 'song') {
                value.description = this.current?.description ?? value.description;
                value.wiki = this.current?.wiki ?? '';
            }
            value.date = this.current?.date || value.date;
            this.details[input.key] = {
                ...value,
            };
            const index = this.lists[0].findIndex((x) => x.key === input.key);
            if (index >= 0) {
                this.lists[0][index] = value;
            }
            this.current = value;
            if (value.kind !== 'song' || !this.albumLoaded) {
                this.tracks = list;
            }
            this.trackIDs = ids;
            this.trackCursor = cursor(list, ids);
            this.detailLoading = false;
            if (this.status === '正在读取链接…') {
                this.status = '已读取：' + value.title;
            }
            this.loadCover(value);
            this.changed();
            if (value.kind === 'song' && !value.wiki && this.wantsSongDetails) {
                try {
                    const wiki = await this.api.wiki(value.id, () => token === this.selectionGeneration);
                    if (token !== this.selectionGeneration) {
                        return;
                    }
                    this.current.wiki = wiki;
                    this.details[input.key] = {
                        ...this.current,
                    };
                    this.changed();
                } catch (error) {
                    if (token !== this.selectionGeneration) {
                        return;
                    }
                    this.current.wiki = '百科暂时无法读取：' + error.message;
                    this.changed();
                }
            }
        } catch (error) {
            if (token !== this.selectionGeneration) {
                return;
            }
            this.detailLoading = false;
            this.status = '读取失败：' + error.message;
            this.changed();
        }
    }

    async loadCover(value) {
        const token = this.selectionGeneration;
        await this.api.delay(200);
        if (token !== this.selectionGeneration) {
            return;
        }
        this.host.cover(value.cover, (key) => {
            if (token !== this.selectionGeneration) {
                return;
            }
            this.coverImage = key;
            this.coverKey = value.key;
            this.changed();
        });
    }

    async search(text, kind = null, more = false) {
        const link = this.provider === 'netease' && !more && this.host.fromLink(text);
        if (link) {
            const [type, id] = link.split(':');
            const value = item(type, id);
            ++this.generation;
            this.searchLoading = false;
            this.keyword = text;
            this.searchKind = type;
            this.lists[0] = [value];
            this.totals[0] = 1;
            this.offsets[0] = 0;
            this.hasMore[0] = false;
            this.rowMemory[0] = 0;
            this.status = '正在读取链接…';
            this.showSource(0);
            this.wantsSongDetails = type === 'song';
            this.call('setMode', type === 'song' ? 'detail' : 'archive');
            return;
        }
        const q = text.trim();
        if (
            !more &&
            q === this.keyword &&
            (kind === null || kind === this.searchKind) &&
            (this.lists[0].length || this.searchLoading)
        ) {
            return;
        }
        if (!q || Array.from(q).length > 100) {
            ++this.generation;
            this.searchLoading = false;
            this.status = '请输入 1–100 字关键词';
            this.changed();
            return;
        }
        if (more && (this.searchLoading || !this.hasMore[0])) {
            return;
        }
        if (kind) {
            this.searchKind = kind;
        }
        this.keyword = q;
        const token = ++this.generation;
        const type = this.searchKind;
        const start = more ? this.searchCursor : 0;
        if (!more) {
            this.groupView = null;
            this.lists[0] = [];
            this.totals[0] = 0;
            this.offsets[0] = 0;
            this.searchCursor = 0;
            this.hasMore[0] = false;
        }
        if (!more) {
            this.searchHistory = [q, ...this.searchHistory.filter((v) => v !== q)].slice(0, 12);
            this.host.setPreference?.('search-history', JSON.stringify(this.searchHistory));
        }
        this.searchLoading = true;
        this.status = '正在检索…';
        this.changed();
        try {
            const page = await this.api.search(q, type, start, () => token === this.generation);
            if (token !== this.generation) {
                return;
            }
            if (more) {
                this.lists[0] = unique(this.lists[0], page.items);
            } else {
                this.lists[0] = page.items;
                this.offsets[0] = 0;
            }
            this.searchCursor = start + page.rawCount;
            this.hasMore[0] = page.more;
            this.totals[0] = Math.max(page.total, this.lists[0].length);
            this.searchLoading = false;
            this.status =
                page.notice || (page.items.length ? '找到 ' + this.totals[0] + ' 条结果' : '没有匹配的音乐');
            if (!more) {
                this.showSource(0);
                this.call('setMode', 'archive');
                this.frame = {
                    ...this.frame,
                    mode: 'archive',
                };
            }
            this.changed();
        } catch (error) {
            if (token !== this.generation) {
                return;
            }
            this.searchLoading = false;
            this.status = '检索失败：' + error.message;
            this.changed();
        }
    }

    async suggest(text) {
        const token = ++this.suggestGeneration;
        this.suggestions = [];
        if (!text.trim()) {
            return;
        }
        try {
            const responseBody = await this.api.request(
                this.provider === 'qq' ? '/getSmartbox' : '/search/suggest',
                this.provider === 'qq'
                    ? {
                          key: text,
                      }
                    : {
                          keywords: text,
                          type: 'mobile',
                      },
                [200],
                () => token === this.suggestGeneration,
            );
            if (token === this.suggestGeneration) {
                this.suggestions = (
                    this.provider === 'qq'
                        ? (responseBody.response?.data?.song?.itemlist || []).map((v) => ({
                              keyword: v.name,
                          }))
                        : responseBody.result?.allMatch || []
                )
                    .map((v) => v.keyword)
                    .filter((v) => typeof v === 'string')
                    .slice(0, 4);
            }
        } catch (error) {
            // 联想请求失败时保持已有结果，不打断当前输入。
        }
    }

    showSource(lane) {
        this.privateSelection = lane > 0;
        if (this.directory) {
            this.leaveDirectory(false);
        }
        if (lane < 0 || lane >= 5) {
            return;
        }
        this.selectedLane = lane;
        this.selectedPhysical = -1;
        const row = this.rowMemory[lane];
        this.call('select', this.physicalSlots[lane][row]);
        this.observe(this.physicalSlots[lane][row]);
        if (lane > 0) {
            if (!this.uid) {
                this.status = '登录后查看' + this.columns[lane];
            } else if (!this.lists[lane].length) {
                this.loadMoreSource(lane);
            }
        }
        this.changed();
    }

    selectResult(index, lane, open = true) {
        if (this.directory) {
            this.leaveDirectory(false);
        }
        if (!this.lists[lane]?.[index]) {
            return;
        }
        let viewIndex = index;
        if (this.groupView?.lane === lane) {
            viewIndex = this.groupView.indices.indexOf(index);
            if (viewIndex < 0) {
                this.groupView = null;
                viewIndex = index;
            }
        }
        this.offsets[lane] = Math.floor(viewIndex / 8) * 8;
        this.selectedPhysical = -1;
        this.call('select', this.physicalSlots[lane][viewIndex % 8]);
        this.call('setMode', open && this.lists[lane][index].kind === 'song' ? 'detail' : 'archive');
        this.frame = {
            ...this.frame,
            mode: open && this.lists[lane][index].kind === 'song' ? 'detail' : 'archive',
        };
        this.observe(this.physicalSlots[lane][viewIndex % 8]);
        if (open && this.current?.kind !== 'song') {
            this.enterDirectory(this.current);
        }
    }

    next(direction) {
        if (this.directory) {
            this.navigateDirectory(direction, 'row');
            return;
        }
        const row = this.physicalSlots[this.selectedLane].indexOf(this.selectedPhysical);
        const n = this.sourceItems(this.selectedLane).length;
        if (row < 0 || !n) {
            return;
        }
        const absolute = (((this.offsets[this.selectedLane] + row) % n) + n) % n;
        const raw = absolute + direction;
        if (raw >= n && this.hasMore[this.selectedLane] && this.groupView?.lane !== this.selectedLane) {
            this.loadMoreSource(this.selectedLane);
            return;
        }
        const next = ((raw % n) + n) % n;
        const target = (((row + direction) % 8) + 8) % 8;
        this.offsets[this.selectedLane] = next - target;
        this.call('navigate', 'row', direction);
        this.selectedPhysical = -1;
        this.changed();
    }

    async loadMoreSource(lane) {
        if (lane < 0 || lane >= 5 || this.personalLoading || !this.hasMore[lane]) {
            return;
        }
        if (lane === 0) {
            return this.search(this.keyword, null, true);
        }
        if (!this.uid) {
            this.status = '请登录' + this.providerName;
            this.changed();
            return;
        }
        this.personalLoading = true;
        const token = this.accountGeneration;
        const user = this.uid;
        this.changed();
        try {
            if (lane === 1 && this.provider === 'qq') {
                const page = await this.api.likedSongs(
                    this.likedCursor,
                    () => token === this.accountGeneration,
                );
                if (token !== this.accountGeneration || user !== this.uid) {
                    return;
                }
                this.likedCursor += page.rawCount;
                this.lists[1] = unique(this.lists[1], page.items);
                this.likedIDs = this.lists[1].map((v) => v.id);
                this.totals[1] = page.total;
                this.hasMore[1] = page.more;
            } else if (lane === 1) {
                const ids =
                    this.likedIDs ?? (await this.api.likedIDs(user, () => token === this.accountGeneration));
                if (token !== this.accountGeneration || user !== this.uid) {
                    return;
                }
                const chunk = ids.slice(this.likedCursor, this.likedCursor + 100);
                const songs = await this.api.songs(chunk, () => token === this.accountGeneration);
                if (token !== this.accountGeneration || user !== this.uid) {
                    return;
                }
                this.likedIDs = ids;
                this.likedCursor += chunk.length;
                this.lists[1] = unique(this.lists[1], ordered(chunk, songs));
                this.totals[1] = ids.length;
                this.hasMore[1] = this.likedCursor < ids.length;
            } else if (lane === 4) {
                const page = await this.api.likedAlbums(
                    this.albumCursor,
                    () => token === this.accountGeneration,
                );
                if (token !== this.accountGeneration || user !== this.uid) {
                    return;
                }
                this.albumCursor += page.rawCount;
                this.lists[4] = unique(this.lists[4], page.items);
                this.totals[4] = Math.max(page.total, this.lists[4].length);
                this.hasMore[4] = page.more;
            } else {
                const page = await this.api.userPlaylists(user, () => token === this.accountGeneration);
                if (token !== this.accountGeneration || user !== this.uid) {
                    return;
                }
                this.playlistCursor += page.rawCount;
                this.lists[2] = unique(
                    this.lists[2],
                    page.items.filter((x) => x.creatorID === user),
                );
                this.lists[3] = unique(
                    this.lists[3],
                    page.items.filter((x) => x.creatorID !== user),
                );
                for (const i of [2, 3]) {
                    this.totals[i] = this.lists[i].length;
                    this.hasMore[i] = page.more;
                }
            }
            this.personalLoading = false;
            this.status = '已读取个人音乐';
            if (this.selectedLane === lane && !this.current) {
                this.selectedPhysical = -1;
                this.observe(this.frame?.selected ?? this.physicalSlots[lane][0]);
            }
            this.changed();
        } catch (error) {
            if (token !== this.accountGeneration) {
                return;
            }
            this.personalLoading = false;
            this.status = error.message;
            this.changed();
        }
    }

    async loadTrackPage(start) {
        if (
            !this.current ||
            this.trackLoad ||
            start + 3 < this.tracks.length ||
            this.trackCursor >= this.trackIDs.length
        ) {
            return;
        }
        this.trackLoad = true;
        const token = this.selectionGeneration;
        const ids = this.trackIDs.slice(this.trackCursor, this.trackCursor + 100);
        try {
            const songs = await this.api.songs(ids, () => token === this.selectionGeneration);
            if (token !== this.selectionGeneration) {
                return;
            }
            this.trackCursor += ids.length;
            this.tracks = unique(this.tracks, ordered(ids, songs));
            this.changed();
        } catch (error) {
            if (token === this.selectionGeneration) {
                this.status = error.message;
                this.changed();
            }
        } finally {
            if (token === this.selectionGeneration) {
                this.trackLoad = false;
            }
        }
    }

    async sameAlbum() {
        const song = this.current;
        if (!song || song.kind !== 'song' || this.detailLoading || !song.albumID || this.albumLoaded) {
            return;
        }
        this.albumLoaded = true;
        const token = this.selectionGeneration;
        try {
            const [album, list] = await this.api.detail(
                {
                    ...item('album', song.albumID, song.album, song.artist),
                    provider: this.provider,
                    key: this.provider + ':album:' + song.albumID,
                },
                () => token === this.selectionGeneration,
            );
            if (token !== this.selectionGeneration) {
                return;
            }
            this.current.description = album.description;
            this.current.date = album.date;
            this.tracks = list;
            this.changed();
        } catch (error) {
            if (token !== this.selectionGeneration) {
                return;
            }
            this.albumLoaded = false;
            this.status = error.message;
            this.changed();
        }
    }

    play() {
        if (this.current?.kind !== 'song') {
            this.status = '请先选择一首歌曲';
            this.changed();
            return;
        }
        return this.player.play(this.current, this.directoryQueue());
    }

    directoryQueue() {
        const directory = this.directory;
        if (!directory?.ids.length) {
            return this.libraryItems;
        }
        const loaded = new Map(directory.songs.map((value) => [value.id, value]));
        return directory.ids.map(
            (id) =>
                loaded.get(id) || {
                    ...item('song', id, '待读取曲目'),
                    provider: this.provider,
                    key: this.provider + ':song:' + id,
                    deferred: true,
                },
        );
    }

    playDirectory() {
        const directory = this.directory;
        if (!directory?.songs.length) {
            return;
        }
        return this.player.play(directory.songs[0], this.directoryQueue());
    }

    playOfficial(value = this.current) {
        if (value?.kind !== 'song') {
            return;
        }
        if ((value.provider || this.provider) === 'qq') {
            this.host.open(this.sourceURL(value));
            return;
        }
        this.host.play(value.id, (message) => {
            this.status = message;
            this.changed();
        });
    }

    sourceURL(value = this.current) {
        if (!value) {
            return '';
        }
        if ((value.provider || this.provider) === 'netease') {
            return 'https://music.163.com/#/' + value.kind + '?id=' + value.id;
        }
        const paths = {
            song: 'songDetail',
            album: 'albumDetail',
            artist: 'singer',
            playlist: value.chart ? 'toplist' : 'playlist',
        };
        return 'https://y.qq.com/n/ryqq/' + paths[value.kind] + '/' + encodeURIComponent(value.id);
    }

    async mutate(route, values, success) {
        if (!this.editable) {
            this.status = 'QQ 音乐当前支持读取与播放';
            this.changed();
            return false;
        }
        if (this.writeLoading) {
            return false;
        }
        if (!this.uid) {
            this.status = '请先登录网易云';
            this.changed();
            return false;
        }
        const token = this.accountGeneration;
        this.writeLoading = true;
        this.changed();
        try {
            const responseBody = await this.api.request(
                route,
                values,
                [200],
                () => token === this.accountGeneration,
            );
            if (token !== this.accountGeneration) {
                return false;
            }
            if (responseBody.body?.code && responseBody.body.code !== 200) {
                throw new Error('网易云返回 ' + responseBody.body.code);
            }
            this.host.invalidateMetadata?.();
            success?.(responseBody);
            this.status = '已同步至网易云';
            return true;
        } catch (error) {
            if (token === this.accountGeneration) {
                this.status = error.message;
            }
            return false;
        } finally {
            if (token === this.accountGeneration) {
                this.writeLoading = false;
                this.changed();
            }
        }
    }

    async like(value = this.current) {
        if (value?.kind !== 'song') {
            return;
        }
        if (!this.uid) {
            this.status = '请先登录网易云';
            this.changed();
            return;
        }
        if (this.likedIDs === null) {
            const token = this.accountGeneration;
            try {
                this.likedIDs = await this.api.likedIDs(this.uid, () => token === this.accountGeneration);
                if (token !== this.accountGeneration) {
                    return;
                }
            } catch (error) {
                this.status = error.message;
                this.changed();
                return;
            }
        }
        const liked =
            (this.likedIDs || []).includes(value.id) || this.lists[1].some((v) => v.id === value.id);
        return this.mutate(
            '/like',
            {
                id: value.id,
                like: !liked,
            },
            () => {
                if (liked) {
                    this.lists[1] = this.lists[1].filter((v) => v.id !== value.id);
                    if (this.likedIDs) {
                        this.likedIDs = this.likedIDs.filter((v) => v !== value.id);
                    }
                } else {
                    this.lists[1] = unique(this.lists[1], [value]);
                    if (this.likedIDs) {
                        this.likedIDs.push(value.id);
                    }
                }
                this.totals[1] = this.likedIDs?.length ?? this.lists[1].length;
            },
        );
    }

    subscribe(value = this.current) {
        if (!value || !['album', 'playlist', 'artist'].includes(value.kind)) {
            return;
        }
        const lane = value.kind === 'album' ? 4 : 3;
        const subscribed =
            this.subscriptions.has(value.key) || this.lists[lane].some((v) => v.key === value.key);
        return this.mutate(
            value.kind === 'playlist' ? '/playlist/subscribe' : '/' + value.kind + '/sub',
            {
                id: value.id,
                t: subscribed ? 2 : 1,
            },
            () => {
                if (subscribed) {
                    this.subscriptions.delete(value.key);
                    if (value.kind !== 'artist') {
                        this.lists[lane] = this.lists[lane].filter((v) => v.key !== value.key);
                    }
                } else {
                    this.subscriptions.add(value.key);
                    if (value.kind !== 'artist') {
                        this.lists[lane] = unique(this.lists[lane], [value]);
                    }
                }
                if (value.kind !== 'artist') {
                    this.totals[lane] = this.lists[lane].length;
                }
            },
        );
    }
    /**
     * 提交当前来源的账号登录请求。
     * 通过登录代数隔离迟到响应，不持久化明文密码。
     */
    async passwordLogin(method, account, password) {
        if (this.accountLoading && this.loginMethod === method) {
            return;
        }
        if (!account.trim() || !password) {
            this.loginStatus = '请输入账号和密码';
            this.changed();
            return;
        }
        this.cancelLogin();
        const token = ++this.loginGeneration;
        this.loginMethod = method;
        this.loginStatus = '正在登录…';
        this.accountLoading = true;
        this.changed();
        try {
            const values =
                method === 'phone'
                    ? {
                          phone: account,
                          password,
                      }
                    : {
                          email: account,
                          password,
                      };
            await this.api.request(
                method === 'phone' ? '/login/cellphone' : '/login',
                values,
                [200],
                () => token === this.loginGeneration,
            );
            if (token !== this.loginGeneration) {
                return;
            }
            const [id, name] = await this.api.account(() => token === this.loginGeneration);
            if (token !== this.loginGeneration) {
                return;
            }
            const error = this.host.saveCredential();
            if (error) {
                throw new Error(error);
            }
            ++this.accountGeneration;
            this.uid = id;
            this.nickname = name;
            this.rememberIdentityName(this.provider, name);
            this.loginStatus = '已登录：' + name;
            this.status = this.loginStatus;
            this.loadMoreSource(2);
        } catch (error) {
            if (token === this.loginGeneration) {
                this.loginStatus = error.message;
            }
        } finally {
            if (token === this.loginGeneration) {
                this.accountLoading = false;
                this.changed();
            }
        }
    }

    playlistForm(kind, name, pid = '') {
        name = name.trim();
        if (!name || Array.from(name).length > 100) {
            this.status = '请输入 1–100 字歌单名称';
            this.changed();
            return;
        }
        if (
            kind === 'rename' &&
            !(
                this.lists[2].some((v) => v.id === pid) ||
                (this.directory?.item.id === pid && this.directory.item.creatorID === this.uid)
            )
        ) {
            this.status = '只能编辑自己创建的歌单';
            this.changed();
            return;
        }
        return this.mutate(
            kind === 'create' ? '/playlist/create' : '/playlist/name/update',
            kind === 'create'
                ? {
                      name,
                  }
                : {
                      id: pid,
                      name,
                  },
            (responseBody) => {
                if (kind === 'create' && responseBody.playlist) {
                    const value = parse(responseBody.playlist, 'playlist');
                    if (value) {
                        this.lists[2] = unique(this.lists[2], [value]);
                    }
                }
                if (kind === 'rename') {
                    for (const value of this.lists[2]) {
                        if (value.id === pid) {
                            value.title = name;
                        }
                    }
                    if (this.directory?.item.id === pid) {
                        this.directory.item.title = name;
                    }
                }
                this.totals[2] = this.lists[2].length;
            },
        );
    }

    playlistTrack(pid, value = this.current, remove = false) {
        if (value?.kind !== 'song') {
            return;
        }
        if (!(
            this.lists[2].some((v) => v.id === pid) ||
            (this.directory?.item.id === pid && this.directory.item.creatorID === this.uid)
        )) {
            this.status = '请选择自己创建的歌单';
            this.changed();
            return;
        }
        return this.mutate(
            '/playlist/tracks',
            {
                pid,
                tracks: value.id,
                op: remove ? 'del' : 'add',
            },
            () => {
                if (this.directory?.item.id === pid) {
                    this.directory.songs = remove
                        ? this.directory.songs.filter((v) => v.id !== value.id)
                        : unique(this.directory.songs, [value]);
                    if (remove) {
                        const directory = this.directory;
                        const old = directory.ids.indexOf(value.id);
                        if (old >= 0 && old < directory.cursor) {
                            directory.cursor = Math.max(0, directory.cursor - 1);
                        }
                        directory.ids = directory.ids.filter((id) => id !== value.id);
                        directory.item.trackCount = Math.max(0, directory.item.trackCount - 1);
                        directory.index = Math.min(directory.index, Math.max(0, directory.songs.length - 1));
                    }
                }
            },
        );
    }

    async restoreAccount() {
        if (!this.host.authenticated()) {
            return;
        }
        const token = this.accountGeneration;
        try {
            const [id, name] = await this.api.account(() => token === this.accountGeneration);
            if (token !== this.accountGeneration) {
                return;
            }
            this.uid = id;
            this.nickname = name;
            this.rememberIdentityName(this.provider, name);
            this.changed();
        } catch (error) {
            if (token !== this.accountGeneration) {
                return;
            }
            const isExpired =
                error.code === 401 ||
                error.authExpired ||
                (this.provider === 'netease' && error.code === 301);
            if (isExpired) {
                this.uid = '';
                this.nickname = '登录已失效';
                this.host.setPreference?.('account-id-' + this.provider, '');
                this.status = '登录已失效，请重新登录';
            } else {
                // 临时网络失败不删除账号和凭据，切换回来仍可继续验证。
                this.status = '账号验证暂不可用，已保留本机登录记录';
            }
            this.changed();
        }
    }
    /**
     * 发起扫码登录并轮询确认状态。
     * 每轮核对登录代数，已关闭或被替换的流程不能接受旧 Cookie。
     */
    async startLogin() {
        if (!this.canRegenerateLogin) {
            if (this.loginRetrySeconds > 0) {
                this.loginPhase = 'failed';
                this.loginStatus =
                    this.loginCooldowns[this.provider].message +
                    '（请等待 ' +
                    this.loginRetrySeconds +
                    ' 秒）';
                this.changed();
            }
            return;
        }
        const cancellation = this.cancelLogin();
        const token = ++this.loginGeneration;
        this.loginPhase = 'preparing';
        this.loginDiagnostics = {};
        this.loginStatus = '准备二维码…';
        this.qr = '';
        this.accountLoading = true;
        this.changed();
        try {
            if ((await cancellation) === false) {
                throw new Error('旧二维码取消失败，请稍后重试');
            }
            if (token !== this.loginGeneration) {
                return;
            }
            const responseBody = await this.api.request(
                '/login/qr/key',
                {
                    timestamp: Date.now(),
                    ...(this.provider === 'qq'
                        ? {
                              channel: this.loginMethod === 'wechat' ? 'wechat' : 'qq',
                          }
                        : {}),
                },
                [200],
                () => token === this.loginGeneration,
            );
            if (token !== this.loginGeneration) {
                return;
            }
            const key = responseBody.unikey ?? responseBody.data?.unikey;
            if (typeof key !== 'string') {
                throw new Error('二维码生成失败');
            }
            this.qrKey = key;
            if (this.provider === 'qq') {
                const image = await this.api.request(
                    '/login/qr/create',
                    {
                        key,
                    },
                    [200],
                    () => token === this.loginGeneration,
                );
                if (token !== this.loginGeneration) {
                    return;
                }
                const data = image.data?.qrimg;
                if (typeof data !== 'string' || !/^data:image\/(png|jpeg|gif|webp);base64,/.test(data)) {
                    throw new Error('QQ 二维码格式无效');
                }
                this.qr = this.host.qrImage(data);
                if (!this.qr) {
                    throw new Error('QQ 二维码读取失败');
                }
            } else {
                this.qr = this.host.qr(key);
            }
            // 保持原有判断顺序，仅计算当前需要的分支结果。
            // 按当前来源和登录方式选择扫码客户端。
            const selectLoginScanner = () => {
                if (this.provider === 'qq') {
                    if (this.loginMethod === 'wechat') {
                        return '微信';
                    } else {
                        return '手机 QQ';
                    }
                } else {
                    return '网易云';
                }
            };
            this.loginPhase = 'waiting';
            this.loginStatus = '请用' + selectLoginScanner() + '扫描并确认登录';
            this.changed();
            for (let i = 0; i < 300; i++) {
                await this.api.delay(1000);
                if (token !== this.loginGeneration) {
                    return;
                }
                const r = await this.api.request(
                    '/login/qr/check',
                    {
                        key,
                        timestamp: Date.now(),
                    },
                    [800, 801, 802, 803],
                    () => token === this.loginGeneration,
                );
                if (token !== this.loginGeneration) {
                    return;
                }
                const code = r.code ?? 800;
                if (code === 800) {
                    this.qr = '';
                    if (this.provider === 'qq') {
                        this.reportLoginFailure(r);
                    } else {
                        this.loginPhase = 'failed';
                        this.loginStatus = '二维码已过期，请重新生成';
                        this.changed();
                    }
                    return;
                }
                if (code === 802) {
                    this.loginPhase = 'confirming';
                    this.loginStatus = '已扫描，请在手机上确认';
                    this.changed();
                }
                if (code === 803) {
                    this.loginPhase = 'confirming';
                    const [id, name] = await this.api.account(() => token === this.loginGeneration);
                    if (token !== this.loginGeneration) {
                        return;
                    }
                    const error = this.host.saveCredential();
                    if (error) {
                        throw new Error(error);
                    }
                    ++this.accountGeneration;
                    this.uid = id;
                    this.nickname = name;
                    this.rememberIdentityName(this.provider, name);
                    this.qr = '';
                    this.qrKey = '';
                    this.loginPhase = 'complete';
                    delete this.loginCooldowns[this.provider];
                    this.loginStatus = '已登录：' + name;
                    this.status = '已登录，可读取我的音乐';
                    this.loadMoreSource(2);
                    this.changed();
                    return;
                }
            }
            this.loginPhase = 'failed';
            this.loginStatus = '二维码超时，请重新生成';
            this.changed();
        } catch (error) {
            if (token !== this.loginGeneration) {
                return;
            }
            this.loginPhase = 'failed';
            if (this.provider === 'qq' && error.code) {
                this.reportLoginFailure(error);
            } else {
                this.loginStatus = error.message;
                this.changed();
            }
        } finally {
            if (token === this.loginGeneration) {
                this.accountLoading = false;
                this.changed();
            }
        }
    }

    cancelLogin() {
        const previousKey = this.qrKey;
        const previousAPI = this.api;
        ++this.loginGeneration;
        this.qrKey = '';
        this.qr = '';
        this.loginPhase = 'idle';
        this.accountLoading = false;
        if (this.provider === 'qq' && previousKey) {
            const provider = this.provider;
            const cancellation = previousAPI.request('/login/qr/cancel', { key: previousKey }).then(
                () => true,
                () => {
                    // 关闭时先在本地隔离旧回调；重新生成时由调用方报告取消失败。
                    return false;
                },
            );
            this.loginCancellations[provider] = cancellation;
            cancellation.then(() => {
                if (this.loginCancellations[provider] === cancellation) {
                    delete this.loginCancellations[provider];
                }
            });
            return cancellation;
        }
        return this.loginCancellations[this.provider] || Promise.resolve(true);
    }
    /**
     * 退出当前账号并撤销其登录、资料和播放请求。
     * 清理当前来源状态，持久凭据操作交给 Host。
     */
    logout() {
        const clearSelection = this.selectedLane > 0 || this.privateSelection;
        this.writeLoading = false;
        this.subscriptions.clear();
        this.discover.cancel();
        this.player.stop();
        this.player.history = [];
        this.host.setPreference?.('play-history', '[]');
        while (this.directory) {
            this.leaveDirectory();
        }
        this.groupView = null;
        this.cancelLogin();
        ++this.accountGeneration;
        this.host.logout();
        this.host.setPreference?.('account-id-' + this.provider, '');
        this.uid = '';
        this.nickname = '未登录';
        this.likedIDs = null;
        this.likedCursor = 0;
        this.playlistCursor = 0;
        this.albumCursor = 0;
        this.personalLoading = false;
        this.details = {};
        for (let lane = 1; lane < 5; lane++) {
            this.lists[lane] = [];
            this.totals[lane] = 0;
            this.offsets[lane] = 0;
            this.hasMore[lane] = true;
        }
        this.runtime.musicClear?.();
        this.choose(clearSelection ? null : this.current);
        this.privateSelection = false;
        this.status = '已退出登录';
        this.changed();
    }
    /**
     * 读取条目并建立可导航目录。
     * 目录代数隔离迟到响应，成功后按原顺序更新条目与选中动画。
     */
    async enterDirectory(value) {
        if (!value) {
            return;
        }
        if (value.kind === 'song') {
            this.choose(value);
            return;
        }
        const token = ++this.directoryGeneration;
        ++this.selectionGeneration;
        ++this.wikiGeneration;
        const parent =
            this.selectedPhysical >= 0
                ? this.selectedPhysical
                : (this.frame?.selected ?? this.physicalSlots[this.selectedLane][0]);
        const parentDirectory = this.directory;
        this.directory = {
            parentDirectory,
            albums: [],
            albumCursor: 0,
            albumsMore: value.kind === 'artist',
            albumsLoading: false,
            item: value,
            songs: [],
            ids: [],
            cursor: 0,
            index: 0,
            anchor: 0,
            page: 0,
            parentPhysical: parent,
            parentLane: this.selectedLane,
            parentItem: value,
        };
        this.directoryInfo = false;
        this.directoryLoading = true;
        this.wantsSongDetails = false;
        this.current = null;
        this.tracks = [];
        this.trackIDs = [];
        this.coverImage = '';
        this.coverKey = '';
        this.detailLoading = false;
        this.call('setMode', 'archive');
        this.call('navigate', 'lane', 1);
        const dest = (this.selectedLane + 1) % 5;
        this.directory.anchor = dest * 8 + this.rowMemory[dest];
        this.selectedPhysical = -1;
        this.status = '正在打开：' + value.title;
        this.changed();
        try {
            const [updated, songs, ids] = await this.api.detail(
                value,
                () => token === this.directoryGeneration,
            );
            if (token !== this.directoryGeneration || !this.directory) {
                return;
            }
            Object.assign(this.directory, {
                item: updated,
                songs,
                ids,
                cursor: cursor(songs, ids),
            });
            this.details[updated.key] = updated;
            this.directoryLoading = false;
            this.status = '已打开：' + updated.title;
            if (value.kind === 'artist') {
                this.showDirectoryInfo();
            } else if (songs.length) {
                this.directory.index = 0;
                this.choose(songs[0]);
            } else {
                this.changed();
            }
        } catch (error) {
            if (token !== this.directoryGeneration) {
                return;
            }
            this.directoryLoading = false;
            this.status = '读取失败：' + error.message;
            this.changed();
        }
    }

    async loadArtistAlbums() {
        const directory = this.directory;
        if (
            !directory ||
            directory.item.kind !== 'artist' ||
            directory.albumsLoading ||
            !directory.albumsMore
        ) {
            return;
        }
        const token = this.directoryGeneration;
        directory.albumsLoading = true;
        this.changed();
        try {
            const page = await this.api.artistAlbums(
                directory.item.id,
                directory.albumCursor,
                () => token === this.directoryGeneration,
            );
            if (token !== this.directoryGeneration) {
                return;
            }
            directory.albums = unique(directory.albums, page.items);
            directory.albumCursor += page.rawCount;
            directory.albumsMore = page.more;
        } catch (error) {
            if (token !== this.directoryGeneration) {
                return;
            }
            this.status = '专辑读取失败：' + error.message;
        } finally {
            if (token === this.directoryGeneration) {
                directory.albumsLoading = false;
                this.changed();
            }
        }
    }

    openSongArtist(index = 0) {
        const a = this.current?.artists?.[index];
        if (a && /^[a-zA-Z0-9_-]{1,80}$/.test(a.id)) {
            this.enterDirectory({
                ...item('artist', a.id, a.name),
                provider: this.provider,
                key: this.provider + ':artist:' + a.id,
            });
        }
    }

    observeDirectory(physical) {
        const directory = this.directory;
        if (!directory || this.directoryInfo || this.selectedPhysical === physical) {
            return;
        }
        const lane = this.physicalSlots.findIndex((x) => x.includes(physical));
        if (lane < 0) {
            return;
        }
        const row = this.physicalSlots[lane].indexOf(physical);
        this.selectedPhysical = physical;
        this.selectedLane = lane;
        if (!directory.songs.length) {
            directory.anchor = lane * 8 + row;
            return;
        }
        const index =
            (directory.page + ((lane * 8 + row - directory.anchor + 40) % 40)) % directory.songs.length;
        directory.index = index;
        this.choose(directory.songs[index]);
    }

    selectDirectory(index, mode = 'archive', axis = 'row', direction = 1) {
        const directory = this.directory;
        if (!directory?.songs[index]) {
            return;
        }
        this.directoryInfo = false;
        directory.index = index;
        directory.page = Math.floor(index / 40) * 40;
        const cell = (index + directory.anchor) % 40;
        this.selectedLane = Math.floor(cell / 8);
        const physical = this.physicalSlots[this.selectedLane][cell % 8];
        if (axis) {
            this.call('select', physical, {
                axis,
                direction,
            });
        } else {
            this.call('select', physical);
        }
        this.selectedPhysical = physical;
        this.choose(directory.songs[index], false, mode === 'detail');
        this.call('setMode', mode);
        this.changed();
    }

    navigateDirectory(step, axis = null) {
        const directory = this.directory;
        if (!directory?.songs.length) {
            return;
        }
        const next = directory.index + step;
        if (next >= directory.songs.length && directory.cursor < directory.ids.length) {
            this.loadDirectoryPage(next);
            return;
        }
        // 保持原有判断顺序，仅计算当前需要的分支结果。
        // 行移动保留步长，列移动只取原有方向。
        const selectNavigationStep = () => {
            if (axis === 'row') {
                return step;
            } else if (step > 0) {
                return 1;
            } else {
                return -1;
            }
        };
        this.selectDirectory(
            ((next % directory.songs.length) + directory.songs.length) % directory.songs.length,
            'archive',
            axis ?? (Math.abs(step) > 1 ? 'lane' : 'row'),
            selectNavigationStep(),
        );
    }
    /**
     * 按完整曲目 ID 补齐当前目录的一页元数据。
     * 目录被关闭或切换后丢弃迟到结果，保留既有分页游标和选择顺序。
     */
    async loadDirectoryPage(selectAfter = null) {
        const directory = this.directory;
        if (!directory || this.directoryLoading || directory.cursor >= directory.ids.length) {
            return;
        }
        this.directoryLoading = true;
        const token = this.directoryGeneration;
        const ids = directory.ids.slice(directory.cursor, directory.cursor + 100);
        this.changed();
        try {
            const songs = await this.api.songs(ids);
            if (token !== this.directoryGeneration || !this.directory) {
                return;
            }
            directory.songs = unique(directory.songs, ordered(ids, songs));
            directory.cursor += ids.length;
            this.directoryLoading = false;
            if (selectAfter !== null && directory.songs[selectAfter]) {
                this.selectDirectory(selectAfter);
            } else {
                this.changed();
            }
        } catch (error) {
            if (token !== this.directoryGeneration) {
                return;
            }
            this.directoryLoading = false;
            this.status = error.message;
            this.changed();
        }
    }

    showDirectoryInfo() {
        const directory = this.directory;
        if (!directory) {
            return;
        }
        ++this.selectionGeneration;
        this.directoryInfo = true;
        this.current = {
            ...directory.item,
        };
        this.detailLoading = false;
        this.tracks = directory.songs;
        this.trackIDs = directory.ids;
        this.coverImage = '';
        this.coverKey = '';
        this.call('musicIdentity', directory.item.key);
        this.loadCover(directory.item);
        this.call('setMode', 'detail');
        this.changed();
    }

    returnToDirectory() {
        if (this.directory) {
            this.selectDirectory(this.directory.index);
        }
        this.changed();
    }

    leaveDirectory(restore = true) {
        const directory = this.directory;
        if (!directory) {
            return;
        }
        ++this.directoryGeneration;
        this.directory = restore ? (directory.parentDirectory ?? null) : null;
        this.directoryLoading = false;
        this.directoryInfo = false;
        ++this.selectionGeneration;
        ++this.wikiGeneration;
        this.tracks = [];
        this.trackIDs = [];
        this.coverImage = '';
        this.coverKey = '';
        this.detailLoading = false;
        if (restore) {
            this.selectedLane = directory.parentLane;
            this.selectedPhysical = directory.parentPhysical;
            this.current = this.details[directory.item.key] ?? directory.parentItem;
            this.call('select', directory.parentPhysical, {
                axis: 'lane',
                direction: -1,
            });
            this.call('setMode', 'archive');
            if (this.directory) {
                this.selectDirectory(this.directory.index);
                this.status = '已返回：' + this.directory.item.title;
                this.changed();
                return;
            }
            if (this.current) {
                this.call('musicIdentity', this.current.key);
                this.loadCover(this.current);
            }
        }
        this.status = '已返回：' + this.columns[this.selectedLane];
        this.changed();
    }

    async loadSongWiki() {
        this.wantsSongDetails = true;
        const value = this.current;
        if (!value || value.kind !== 'song' || this.detailLoading || value.wiki) {
            return;
        }
        const token = this.selectionGeneration;
        const wikiToken = ++this.wikiGeneration;
        try {
            const wiki = await this.api.wiki(value.id, () => token === this.selectionGeneration);
            if (token !== this.selectionGeneration || wikiToken !== this.wikiGeneration) {
                return;
            }
            this.current.wiki = wiki;
            this.details[value.key] = {
                ...this.current,
            };
            this.changed();
        } catch (error) {
            if (token !== this.selectionGeneration || wikiToken !== this.wikiGeneration) {
                return;
            }
            this.status = '百科暂不可读取：' + error.message;
            this.changed();
        }
    }
}
