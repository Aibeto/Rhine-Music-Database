import { parse } from './music-items.js';
const rows = (data, kind) => (data || []).map((v) => parse(v, kind)).filter(Boolean);
// 同一入口按平台选择推荐来源；切换平台与退出账号都会撤销迟到结果。
export class Discover {
    constructor(store) {
        this.s = store;
        this.tab = 'playlists';
        this.items = [];
        this.loading = false;
        this.status = '';
        this.generation = 0;
        this.offset = 0;
        this.more = false;
        this.category = '全部';
        this.categories = [];
    }

    async load(tab = this.tab, more = false) {
        if (more && (this.loading || !this.more)) {
            return;
        }
        this.tab = tab;
        const token = ++this.generation;
        const account = this.s.accountGeneration;
        const offset = more ? this.offset : 0;
        this.loading = true;
        this.status = '正在读取…';
        if (!more) {
            this.items = [];
        }
        this.s.changed();
        try {
            const qq = this.s.provider === 'qq';
            const external = this.s.provider !== 'netease';
            const valid = () => token === this.generation && account === this.s.accountGeneration;
            if (tab === 'daily' && !qq && !this.s.uid) {
                throw new Error('登录后查看每日推荐');
            }
            if (tab === 'artists' && (!this.s.uid || external)) {
                throw new Error('登录网易云后查看收藏歌手');
            }
            let next;
            let rawCount = 0;
            let hasMore = false;
            if (tab === 'history') {
                next = this.s.player.history.filter(
                    (v) => (v.provider || 'netease') === (this.s.provider || 'netease'),
                );
            } else if (external) {
                const p = await this.s.api.discover(tab, offset, this.category, valid);
                next = p.items;
                rawCount = p.rawCount;
                hasMore = p.more;
            } else {
                let b;
                let data;
                let kind = 'playlist';
                switch (tab) {
                    case 'playlists':
                        b = await this.s.api.request(
                            '/personalized',
                            {
                                limit: 30,
                            },
                            [200],
                            valid,
                        );
                        data = b.result || [];
                        break;
                    case 'daily':
                        b = await this.s.api.request('/recommend/songs', {}, [200], valid);
                        data = b.data?.dailySongs || b.recommend || [];
                        kind = 'song';
                        break;
                    case 'charts':
                        b = await this.s.api.request('/toplist', {}, [200], valid);
                        data = b.list || [];
                        break;
                    case 'albums':
                        b = await this.s.api.request('/album/newest', {}, [200], valid);
                        data = b.albums || [];
                        kind = 'album';
                        break;
                    case 'categories':
                        b = await this.s.api.request(
                            '/top/playlist',
                            {
                                cat: this.category,
                                limit: 30,
                                offset,
                            },
                            [200],
                            valid,
                        );
                        data = b.playlists || [];
                        hasMore = !!b.more;
                        break;
                    case 'artists':
                        b = await this.s.api.request(
                            '/artist/sublist',
                            {
                                limit: 30,
                                offset,
                            },
                            [200],
                            valid,
                        );
                        data = b.data || [];
                        kind = 'artist';
                        hasMore = !!b.hasMore;
                        break;
                    default:
                        throw new Error('不支持的发现类别');
                }
                next = rows(data, kind);
                rawCount = data.length;
            }
            if (!valid()) {
                return;
            }
            const keys = new Set(this.items.map((v) => v.key));
            this.items = more
                ? this.items.concat(
                      next.filter((value) => {
                          if (keys.has(value.key)) {
                              return false;
                          }
                          keys.add(value.key);
                          return true;
                      }),
                  )
                : next;
            this.offset = offset + rawCount;
            if (tab === 'artists') {
                for (const value of this.items) {
                    this.s.subscriptions?.add(value.key);
                }
            }
            this.more = hasMore;
            this.status = this.items.length ? '已载入 ' + this.items.length + ' 条' : '暂无内容';
        } catch (error) {
            if (token === this.generation) {
                this.status = error.message;
            }
        } finally {
            if (token === this.generation) {
                this.loading = false;
                this.s.changed();
            }
        }
    }

    async loadCategories() {
        const token = this.generation;
        const account = this.s.accountGeneration;
        try {
            let values;
            if (this.s.provider !== 'netease') {
                values = await this.s.api.categories(
                    () => token === this.generation && account === this.s.accountGeneration,
                );
            } else {
                const b = await this.s.api.request('/playlist/catlist');
                values = (b.sub || []).map((v) => ({
                    id: v.name,
                    name: v.name,
                }));
            }
            if (token !== this.generation || account !== this.s.accountGeneration) {
                return;
            }
            this.categories = values;
            this.s.changed();
        } catch (error) {
            if (token === this.generation) {
                this.status = error.message;
            }
        }
    }

    cancel() {
        ++this.generation;
        this.loading = false;
        this.items = [];
        this.more = false;
        this.offset = 0;
        this.categories = [];
        this.category = '全部';
        this.tab = 'playlists';
    }

    open(index) {
        const value = this.items[index];
        if (!value) {
            return;
        }
        this.s.privateSelection = ['daily', 'artists'].includes(this.tab);
        if (value.kind === 'song') {
            this.s.player.play(value, this.items);
            this.s.choose(value);
            this.s.runtime.setMode('detail');
        } else {
            this.s.enterDirectory(value);
        }
    }
}
