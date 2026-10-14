const identity = (v) => (v.provider || 'netease') + ':' + v.id;
// 播放元数据确认后通知资料库，迟到请求不能覆盖当前曲目。
export class Player {
    constructor(host, api, changed = () => {}) {
        this.host = host;
        this.api = api;
        this.changed = changed;
        this.queue = [];
        this.index = -1;
        this.current = null;
        this.coverImage = '';
        this.generation = 0;
        this.loading = false;
        this.mode = 'all';
        this.onTrack = null;
        this.state = {
            playing: false,
            position: 0,
            duration: 0,
            volume: 0.7,
            seekable: false,
        };
        this.endSerial = 0;
        this.lastPoll = -1;
        this.status = '';
        this.history = [];
        this.onEnd = null;
        this.quality = host.preference?.('quality') || 'exhigh';
        if (!['standard', 'higher', 'exhigh', 'lossless', 'hires', 'jymaster'].includes(this.quality)) {
            this.quality = 'exhigh';
        }
        this.alternative = host.preference?.('alternate-source') === 'on';
        try {
            this.history = JSON.parse(host.preference?.('play-history') || '[]')
                .filter((v) => v.kind === 'song' && /^[a-zA-Z0-9_-]{1,80}$/.test(v.id))
                .slice(0, 200);
        } catch (error) {
            // 本地播放历史无效时使用空历史，不阻止正常播放。
        }
    }

    enqueue(value, next = false) {
        if (value?.kind !== 'song') {
            return;
        }
        const existing = this.queue.findIndex((s) => identity(s) === identity(value));
        if (existing >= 0) {
            return;
        }
        this.queue.splice(next ? Math.max(0, this.index + 1) : this.queue.length, 0, value);
        this.changed();
    }
    /**
     * 替换或选择播放队列中的曲目，异步读取详情和音源。
     * 每次播放推进代数，停止或切歌后的迟到结果不能恢复旧曲目。
     */
    async play(value, source = null) {
        if (value?.kind !== 'song') {
            return;
        }
        if (source) {
            const seen = new Set();
            this.queue = source.filter((song) => {
                if (song.kind !== 'song' || seen.has(identity(song))) {
                    return false;
                }
                seen.add(identity(song));
                return true;
            });
        }
        if (!this.queue.some((s) => identity(s) === identity(value))) {
            this.queue.push(value);
        }
        this.index = this.queue.findIndex((s) => identity(s) === identity(value));
        const token = ++this.generation;
        this.host.mediaControl?.('stop');
        this.current = value;
        this.coverImage = '';
        this.loading = true;
        this.failed = false;
        this.trial = false;
        this.status = '正在准备：' + value.title;
        this.changed();
        try {
            if (value.deferred) {
                const fetched = (await this.api.songs([value.id], () => token === this.generation))[0];
                if (token !== this.generation) {
                    return false;
                }
                if (!fetched) {
                    throw new Error('此曲目暂不可读取');
                }
                value = fetched;
                this.current = value;
                this.queue[this.index] = value;
                this.changed();
            }
            if (value.cover) {
                this.host.cover?.(value.cover, (key) => {
                    if (token === this.generation) {
                        this.coverImage = key;
                        this.changed();
                    }
                });
            }
            // 先同步资料库选中项，再准备当前曲目的音源。
            this.onTrack?.(value);
            const track = this.api.playback
                ? await this.api.playback(
                      value,
                      this.quality,
                      this.alternative,
                      () => token === this.generation,
                  )
                : (
                      await this.api.request(
                          '/song/url/v1',
                          {
                              id: value.id,
                              level: this.quality,
                              unblock: this.alternative,
                          },
                          [200],
                          () => token === this.generation,
                      )
                  ).data?.find((s) => String(s.id) === value.id);
            if (token !== this.generation) {
                return;
            }
            if (!track?.url) {
                throw new Error('此歌曲暂无可用音源，请检查登录和歌曲权限');
            }
            this.host.mediaMetadata?.(value.title, value.artist || '', value.album || '');
            if (!this.host.mediaLoad(track.url, this.api.provider === 'netease' && this.alternative)) {
                throw new Error('音频地址不可用');
            }
            this.endSerial = this.host.mediaState?.().ended ?? 0;
            this.history = [
                {
                    ...value,
                    wiki: '',
                    description: '',
                },
                ...this.history.filter((v) => identity(v) !== identity(value)),
            ].slice(0, 200);
            this.host.setPreference?.('play-history', JSON.stringify(this.history));
            this.trial = !!(track.freeTrialInfo && track.freeTrialInfo !== 'null');
            this.status = this.trial ? '当前音源为试听片段' : '正在播放：' + value.title;
            if (track.qualityFallback) {
                this.status += '（已切换为' + track.qualityFallback + '音质）';
            }
            return true;
        } catch (error) {
            if (token !== this.generation) {
                return;
            }
            this.failed = true;
            this.status = error.message;
            return false;
        } finally {
            if (token === this.generation) {
                this.loading = false;
                this.changed();
            }
        }
    }
    /**
     * 读取原生播放状态并处理结束事件。
     * 轮询节流和队列推进保持原规则，状态变化时才通知界面。
     */
    tick(time, force = false) {
        if (!force && time - this.lastPoll < 0.1) {
            return;
        }
        this.lastPoll = time;
        if (!this.host.mediaState) {
            return;
        }
        const previous = this.state;
        this.state = {
            ...this.state,
            ...this.host.mediaState(),
        };
        if (Object.keys(this.state).some((key) => this.state[key] !== previous[key])) {
            this.changed();
        }
        if (this.state.error) {
            this.failed = true;
            this.status = this.state.error;
        }
        if (
            this.current &&
            !this.loading &&
            this.state.ended !== undefined &&
            this.state.ended !== this.endSerial
        ) {
            this.endSerial = this.state.ended;
            if (this.onEnd) {
                this.onEnd();
            } else {
                this.next(1, true);
            }
        }
    }

    toggle() {
        if (this.loading) {
            return;
        }
        if (!this.current) {
            return;
        }
        this.host.mediaControl(this.state.playing ? 'pause' : 'resume');
    }

    seek(fraction) {
        if (this.state.seekable) {
            this.host.mediaControl('seek', Math.max(0, Math.min(1, fraction)) * this.state.duration);
        }
    }

    volume(value) {
        const volume = Math.max(0, Math.min(1, value));
        this.host.mediaControl('volume', volume);
        this.host.setPreference?.('volume', String(volume));
    }

    next(direction = 1, ended = false) {
        if (!this.queue.length) {
            return;
        }
        if (ended && this.mode === 'one') {
            this.play(this.current);
            return;
        }
        let index = this.index + direction;
        if (this.mode === 'shuffle' && this.queue.length > 1) {
            index =
                (this.index + 1 + Math.floor(Math.random() * (this.queue.length - 1))) % this.queue.length;
        }
        if (index >= this.queue.length && this.mode === 'off') {
            this.status = '队列已播放完';
            this.changed();
            return;
        }
        index = ((index % this.queue.length) + this.queue.length) % this.queue.length;
        this.play(this.queue[index]);
    }

    remove(index) {
        if (index < 0 || index >= this.queue.length || index === this.index) {
            return;
        }
        this.queue.splice(index, 1);
        if (index < this.index) {
            --this.index;
        }
        this.changed();
    }

    clearUpcoming() {
        this.queue = this.current ? [this.current] : [];
        this.index = this.current ? 0 : -1;
        this.changed();
    }
    /**
     * 停止系统播放并作废等待中的音源与元数据请求。
     */
    stop() {
        ++this.generation;
        this.loading = false;
        this.failed = false;
        this.status = '';
        this.coverImage = '';
        this.current = null;
        this.queue = [];
        this.index = -1;
        this.host.mediaControl?.('stop');
        this.host.mediaMetadata?.('', '', '');
        this.state = {
            ...this.state,
            playing: false,
            position: 0,
            duration: 0,
        };
        this.changed();
    }
}
