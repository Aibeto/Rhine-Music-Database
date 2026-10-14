// 歌词获取与同步共用毫秒模型，来源适配和显示状态不进入播放协议。
import { API } from './music-api.js';
import { QQAPI } from './qq.js';
import { KugouAPI } from './kugou.js';

export const trackKey = (track) => (track ? (track.provider || 'netease') + ':' + track.id : '');
const CACHE_LIMIT = 12;

function comparable(value) {
    return String(value || '')
        .normalize('NFKC')
        .toLowerCase()
        .replace(/[\s\p{P}\p{S}]/gu, '');
}

function recordingVersion(track) {
    const text = (track.title || '') + ' ' + (track.album || '');
    return ['live', 'remix', 'demo', 'acoustic', 'instrumental', '伴奏', '翻唱', '现场', '重录']
        .filter((version) => text.toLowerCase().includes(version))
        .sort()
        .join('/');
}

/**
 * 跨来源补词只接受同名、同歌手、同版本且时长接近的录音。
 * 保守拒绝不确定候选，不能让有逐字数据的错误版本覆盖当前录音。
 */
export function isSameRecording(track, candidate) {
    const artists = (value) =>
        (value.artists?.length
            ? value.artists.map((artist) => artist.name)
            : String(value.artist || '').split(/\s*\/\s*/)
        )
            .map(comparable)
            .filter(Boolean)
            .sort();
    const sourceArtists = artists(track);
    const targetArtists = artists(candidate);
    const durationKnown = track.duration > 0 && candidate.duration > 0;
    return (
        comparable(track.title) === comparable(candidate.title) &&
        sourceArtists.length > 0 &&
        sourceArtists.join('/') === targetArtists.join('/') &&
        recordingVersion(track) === recordingVersion(candidate) &&
        durationKnown &&
        Math.abs(track.duration - candidate.duration) <= 2000
    );
}

function validDocument(document) {
    return (
        document?.version === 1 &&
        ['line', 'word', 'none'].includes(document.timing) &&
        Array.isArray(document.lines) &&
        document.lines.length <= 5000 &&
        document.lines.every((line) => Number.isFinite(line.startMs) && typeof line.text === 'string') &&
        typeof document.plainText === 'string' &&
        Number.isFinite(document.offsetMs)
    );
}

/**
 * 共用同步引擎通过二分查询定位当前句，支持暂停、快进和向后跳转。
 * 空白时间行保留为间奏，重叠起点不合并，所有显示出口使用同一索引。
 */
export function activeLineIndex(document, positionMs, userOffsetMs = 0) {
    const lines = document?.lines || [];
    const position = positionMs + (document?.offsetMs || 0) + userOffsetMs;
    let lower = 0;
    let upper = lines.length;
    while (lower < upper) {
        const middle = Math.floor((lower + upper) / 2);
        if (lines[middle].startMs <= position) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    return lower - 1;
}

export class Lyrics {
    constructor(host, changed) {
        this.host = host;
        this.changed = changed;
        this.generation = 0;
        this.cache = [];
        this.currentKey = '';
        this.document = null;
        this.status = 'idle';
        this.message = '';
        this.index = -1;
        this.offsetMs = 0;
        this.positionMs = 0;
        try {
            const saved = JSON.parse(host.preference?.('lyrics-cache') || '[]');
            this.cache = Array.isArray(saved)
                ? saved
                      .filter((entry) => typeof entry.key === 'string' && validDocument(entry.document))
                      .map((entry) => ({ key: entry.key, document: entry.document }))
                      .slice(0, CACHE_LIMIT)
                : [];
        } catch (error) {
            // 损坏缓存从空列表恢复，不影响音源和播放历史。
        }
    }

    clear() {
        ++this.generation;
        this.currentKey = '';
        this.document = null;
        this.status = 'idle';
        this.message = '';
        this.index = -1;
        this.offsetMs = 0;
        this.positionMs = 0;
    }

    api(provider) {
        const base = new API(this.host);
        if (provider === 'qq') {
            return new QQAPI(base);
        }
        if (provider === 'kugou') {
            return new KugouAPI(base);
        }
        return base;
    }

    async fetch(track, valid) {
        const provider = track.provider || 'netease';
        const api = this.api(provider);
        const metadata = { title: track.title, artist: track.artist };
        let body;
        if (provider === 'qq') {
            body = await api.request(
                '/getLyric',
                { songmid: track.id, isFormat: false, ...metadata },
                [200],
                valid,
            );
        } else if (provider === 'kugou') {
            const search = await api.request(
                '/search/lyric',
                {
                    hash: track.id,
                    keywords: track.title,
                    duration: track.duration || 0,
                    album_audio_id: track.albumAudioID || 0,
                },
                [200],
                valid,
            );
            const candidates = search.candidates || search.data?.candidates || [];
            const match = candidates.find((candidate) => candidate.id && candidate.accesskey);
            if (!match) {
                return null;
            }
            body = await api.request(
                '/lyric',
                {
                    id: match.id,
                    accesskey: match.accesskey,
                    fmt: 'krc',
                    decode: true,
                    ...metadata,
                },
                [200],
                valid,
            );
            if (!body.document?.lines?.length) {
                body = await api.request(
                    '/lyric',
                    {
                        id: match.id,
                        accesskey: match.accesskey,
                        fmt: 'lrc',
                        decode: true,
                        ...metadata,
                    },
                    [200],
                    valid,
                );
            }
        } else {
            try {
                body = await api.request('/lyric/new', { id: track.id, ...metadata }, [200], valid);
            } catch (error) {
                if (!valid()) {
                    throw error;
                }
                // 新接口不可用时回退既有逐句接口，保留锁定上游协议。
                body = await api.request('/lyric', { id: track.id, ...metadata }, [200], valid);
            }
        }
        return validDocument(body?.document) ? body.document : null;
    }

    /**
     * 切歌立即清空旧歌词，先读当前平台精确 ID，再尝试其他平台的同录音候选。
     * 获取独立于音源加载；任何歌词失败都不阻止播放，迟到结果由代数隔离。
     */
    async load(track, retry = false) {
        const key = trackKey(track);
        const positionMs = key === this.currentKey ? this.positionMs : 0;
        this.clear();
        const token = this.generation;
        this.currentKey = key;
        // 当前曲目重查歌词时保留暂停位置；真正切歌由 Player 先 clear()，从零重新定位。
        this.positionMs = positionMs;
        const valid = () => token === this.generation;
        const cached = !retry && this.cache.find((entry) => entry.key === this.currentKey);
        if (cached) {
            this.accept(cached.document, track);
            return;
        }
        this.status = 'loading';
        this.message = '正在读取歌词…';
        this.changed();
        let native = null;
        let networkFailed = false;
        try {
            native = await this.fetch(track, valid);
        } catch (error) {
            networkFailed = true;
        }
        if (!valid()) {
            return;
        }
        if (native?.lines.length) {
            this.accept(native, track);
            return;
        }
        // 原来源没有可靠时间歌词时再补缺，避免每首歌都访问全部平台。
        for (const provider of ['netease', 'qq', 'kugou']) {
            if (provider === (track.provider || 'netease')) {
                continue;
            }
            try {
                // 部分平台的搜索提示不接受长关键词；短标题重试仍经过同一录音身份校验。
                const queries = [...new Set([track.title + ' ' + track.artist, track.title])];
                for (const query of queries) {
                    const result = await this.api(provider).search(query, 'song', 0, valid);
                    if (!valid()) {
                        return;
                    }
                    const candidates = result.items
                        .filter((candidate) => isSameRecording(track, candidate))
                        .slice(0, 2);
                    for (const candidate of candidates) {
                        const document = await this.fetch(candidate, valid);
                        if (!valid()) {
                            return;
                        }
                        if (document?.lines.length) {
                            this.accept({ ...document, sourceTrackId: candidate.id }, track);
                            return;
                        }
                    }
                }
            } catch (error) {
                networkFailed = true;
            }
            if (!valid()) {
                return;
            }
        }
        if (native?.plainText) {
            this.accept(native, track);
        } else {
            this.status = networkFailed ? 'error' : 'empty';
            this.message = networkFailed ? '歌词暂不可用，点击重试' : '暂无歌词 / 纯音乐';
            this.changed();
        }
    }

    accept(document, track) {
        this.document = { ...document, trackKey: trackKey(track) };
        this.status = document.lines.length ? 'ready' : 'plain';
        this.message = document.lines.length ? '' : '此歌词没有时间信息';
        this.index = activeLineIndex(document, this.positionMs, this.offsetMs);
        // 仅缓存有效歌词，失败结果可在下次播放自动重试。
        this.cache = [
            { key: this.currentKey, document: this.document },
            ...this.cache.filter((entry) => entry.key !== this.currentKey),
        ].slice(0, CACHE_LIMIT);
        this.persist();
        this.changed();
    }

    persist() {
        let serialized = JSON.stringify(this.cache);
        while (serialized.length >= 240000 && this.cache.length) {
            this.cache.pop();
            serialized = JSON.stringify(this.cache);
        }
        this.host.setPreference?.('lyrics-cache', serialized);
    }

    sync(positionMs) {
        this.positionMs = positionMs;
        const index = activeLineIndex(this.document, positionMs, this.offsetMs);
        if (index !== this.index) {
            this.index = index;
            this.changed();
        }
    }

    adjustOffset(deltaMs) {
        this.offsetMs = Math.max(-10000, Math.min(10000, this.offsetMs + deltaMs));
        // 人工校准仅对当前播放有效，暂停保留，切歌由 clear() 归零，禁止写入歌词缓存。
        this.sync(this.positionMs);
        this.changed();
    }
}
