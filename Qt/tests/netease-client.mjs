import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { API } from '../../MusicBridge/client/netease.mjs';
import { dispatch } from '../../MusicBridge/client/dispatch.mjs';
import format from '../../MusicBridge/lyrics-format.cjs';
import { createClientHarness } from './fixtures/client-harness.mjs';

const require = createRequire(import.meta.url);
const { bridgeRuntime } = require('./fixtures/bridge-runtime.cjs');
const directory = path.dirname(fileURLToPath(import.meta.url));
const hook = path.resolve(directory, 'fixtures/bridge-http.cjs');
const bridgeDirectory = process.argv[2];

const message = (operation, args, cookies = {}) => ({
    route: '/native/netease/' + operation,
    params: { args },
    cookies,
});

/** 为每条消息建立真实桥接进程，验证完成后立即清理临时目录和子进程。 */
async function withNeteaseWorker(environment, verify) {
    const { bridge, runtime } = bridgeRuntime(bridgeDirectory);
    const temporaryDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-netease-check-'));
    const child = spawn(runtime, ['--require', hook, path.join(bridge, 'worker.cjs')], {
        cwd: bridge,
        env: {
            ...process.env,
            RHINE_TEST_XEAPI_EXPIRE_FIRST: '',
            RHINE_TEST_XEAPI_FAIL_FIRST: '',
            ...environment,
            TMPDIR: temporaryDirectory,
            TEMP: temporaryDirectory,
            TMP: temporaryDirectory,
            RHINE_TEST_BRIDGE: bridge,
        },
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    let buffer = '';
    let errors = '';
    let sequence = 0;
    const pending = new Map();
    const rejectPending = (error) => {
        for (const request of pending.values()) {
            clearTimeout(request.timer);
            request.reject(error);
        }
        pending.clear();
    };
    child.on('error', rejectPending);
    child.stdin.on('error', rejectPending);
    const exit = new Promise((resolve) => {
        child.once('close', (code, signal) => {
            rejectPending(new Error(`XEAPI worker exited (${code ?? signal}): ${errors}`));
            resolve(code);
        });
    });
    child.stderr.on('data', (chunk) => {
        errors += chunk;
    });
    child.stdout.on('data', (chunk) => {
        buffer += chunk;
        let end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const response = JSON.parse(buffer.slice(0, end));
            buffer = buffer.slice(end + 1);
            const request = pending.get(response.id);
            if (request) {
                clearTimeout(request.timer);
                pending.delete(response.id);
                request.resolve(response);
            }
        }
    });
    const send = (payload) =>
        new Promise((resolve, reject) => {
            const id = String(++sequence);
            const timer = setTimeout(() => reject(new Error('桥接 IPC 超时：' + errors)), 15000);
            pending.set(id, { resolve, reject, timer });
            child.stdin.write(JSON.stringify({ id, ...payload }) + '\n');
        });
    try {
        await verify(send, temporaryDirectory);
        child.stdin.end();
        assert.equal(await exit, 0);
        assert.equal(errors, '');
    } finally {
        for (const request of pending.values()) {
            clearTimeout(request.timer);
        }
        if (child.exitCode === null) {
            child.kill();
            await exit;
        }
        fs.rmSync(temporaryDirectory, { recursive: true, force: true });
    }
}

test('失效请求不会交付旧 Cookie，只有有效响应才写入账号状态', async () => {
    let respond;
    const acceptedCookies = [];
    const api = new API({
        request(route, parameters, callback) {
            assert.equal(route, '/search');
            respond = callback;
        },
        acceptCookies(cookies, epoch) {
            acceptedCookies.push({ cookies, epoch });
        },
    });
    const response = {
        body: { code: 200, result: { songs: [] } },
        cookies: ['MUSIC_U=local-fixture'],
        _epoch: 3,
    };

    // 响应到达前作废请求，旧 Cookie 不得进入当前账号状态。
    let valid = true;
    const staleRequest = api.request('/search', {}, [200], () => valid);
    valid = false;
    respond(JSON.stringify(response), '');
    await assert.rejects(staleRequest, /Stale operation/);
    assert.deepEqual(acceptedCookies, []);

    const validRequest = api.request('/search');
    respond(JSON.stringify(response), '');
    assert.deepEqual(await validRequest, response.body);
    assert.deepEqual(acceptedCookies, [{ cookies: JSON.stringify(response.cookies), epoch: 3 }]);

    // 状态码失败和格式错误的响应都不交付 Cookie。
    const deniedRequest = api.request('/search');
    respond(JSON.stringify({ ...response, body: { code: 403, message: '本地权限拒绝' } }), '');
    await assert.rejects(deniedRequest, /403/);
    assert.equal(acceptedCookies.length, 1);

    const malformedRequest = api.request('/search');
    respond('{', '');
    await assert.rejects(malformedRequest, /元数据格式无效/);
    assert.equal(acceptedCookies.length, 1);
});

test('喜欢歌曲沿用本人喜欢歌单的顺序', async () => {
    const harness = createClientHarness({
        responses: {
            '/user/playlist': {
                code: 200,
                playlist: [
                    { id: 7, specialType: 0, creator: { userId: 42 } },
                    { id: 8, specialType: 5, creator: { userId: 99 } },
                    { id: 9, specialType: 5, creator: { userId: 42 } },
                ],
            },
            '/playlist/detail': (params) => {
                assert.equal(params.id, 9);
                return { code: 200, playlist: { trackIds: [{ id: 30 }, { id: 10 }, { id: 20 }] } };
            },
        },
    });
    const api = new API(harness.host);

    assert.deepEqual(await api.likedIDs('42'), ['30', '10', '20']);
    assert.deepEqual(
        harness.calls.map((call) => call.route),
        ['/user/playlist', '/playlist/detail'],
    );
});

test('歌词解析偏移、重复副歌与逐词时标', () => {
    // 真实平台形态包含重复副歌、多精度时间、偏移、实体、翻译与间奏。
    const lrc = '[offset:200]\r\n[00:01.5][00:04.500]第一句 &amp; 第二声\r\n[00:03.00]\r\n[00:05]结束';
    const document = format.normalizeLyrics('netease', { lrc: { lyric: lrc } }).document;
    assert.deepEqual(
        document.lines.map((line) => line.startMs),
        [1500, 3000, 4500, 5000],
    );
    assert.equal(document.lines[0].text, '第一句 & 第二声');
    assert.equal(document.lines[1].text, '');

    const yrc = format.normalizeLyrics('netease', {
        yrc: { lyric: '[1000,2000](1000,500,0)第一(1500,1500,0)句\n[4000,1000](4000,1000,0)第二句' },
        ytlrc: { lyric: '[00:01.00]译文一\n[00:04.00]译文二' },
    }).document;
    assert.equal(yrc.timing, 'word');
    assert.equal(yrc.lines[0].endMs, 3000);
    assert.equal(yrc.lines[0].translation, '译文一');
});

test('原生调度归一化搜索结果并交付 Cookie', async () => {
    const netease = await dispatch(message('search', ['本地', 'song', 0]), async (request) => {
        assert.equal(request.route, '/search');
        assert.equal(request.params.type, 1);
        return {
            body: {
                code: 200,
                result: { songCount: 1, songs: [{ id: 42, name: '本地歌曲', ar: [{ name: '测试' }] }] },
            },
            cookies: ['MUSIC_U=fixture-native; Path=/'],
        };
    });
    assert.equal(netease.body.value.items[0].id, '42');
    assert.equal(netease.body.value.items[0].artist, '测试');
    assert.equal(netease.body.value.total, 1);
    assert.deepEqual(netease.cookies, ['MUSIC_U=fixture-native; Path=/']);
});

test('替代音源不回填母带等级', async () => {
    for (const alternative of [false, true]) {
        const playback = await dispatch(
            message('playback', [{ id: '42' }, 'jymaster', alternative]),
            async (request) => {
                assert.equal(request.params.level, 'jymaster');
                assert.equal(request.params.unblock, alternative);
                return {
                    body: {
                        code: 200,
                        msg: alternative
                            ? 'Warning: Customizing unblock sources is not supported on this endpoint.'
                            : '',
                        data: [{ id: 42, url: 'https://example.com/audio', level: 'jymaster', type: 'flac' }],
                    },
                };
            },
        );
        assert.equal(playback.body.code, 200);
        assert.equal(playback.body.value.level, alternative ? undefined : 'jymaster');
        assert.equal(playback.body.value.type, alternative ? undefined : 'flac');
    }
});

test('并发请求保持各自的账号 Cookie', async () => {
    const parallel = await Promise.all(
        ['A', 'B'].map((token) =>
            dispatch(message('search', [token, 'song', 0], { MUSIC_U: token }), async (request) => {
                await new Promise((resolve) => setTimeout(resolve, token === 'A' ? 10 : 0));
                assert.equal(request.cookies.MUSIC_U, token);
                return {
                    body: {
                        code: 200,
                        result: { songCount: 1, songs: [{ id: token === 'A' ? 1 : 2, name: token }] },
                    },
                };
            }),
        ),
    );
    assert.deepEqual(
        parallel.map((result) => result.body.value.items[0].title),
        ['A', 'B'],
    );
});

test('未支持的操作不会进入 SDK', async () => {
    const unsupported = await dispatch(message('constructor', []), async () => {
        throw new Error('Unsupported operations must not reach the SDK');
    });
    assert.equal(unsupported.body.code, 400);
});

test('XEAPI 冷启动、并发初始化、刷新与失败重试', async () => {
    const requestSong = (send, level, id = '1') =>
        send({
            route: '/song/url/v1',
            params: { id, level, unblock: false },
            cookies: { MUSIC_U: 'fixture-account' },
        });

    await withNeteaseWorker({}, async (send, temporaryDirectory) => {
        const levels = ['lossless', 'hires', 'jymaster'];
        const responses = await Promise.all(levels.map((level) => requestSong(send, level)));
        for (const [index, response] of responses.entries()) {
            assert.equal(response.body.code, 200);
            assert.equal(response.body.data[0].level, levels[index]);
            assert.equal(response.body.data[0].authenticated, true);
            assert.equal(response.body.data[0].keyRegistrations, 1);
        }
        const keyPath = path.join(temporaryDirectory, 'xeapi_public_key');
        assert(fs.existsSync(keyPath));
        if (process.platform !== 'win32') {
            assert.equal(fs.statSync(keyPath).mode & 0o777, 0o600);
        }
        assert.equal((await requestSong(send, 'jymaster', '2')).body.code, 403);
        assert.equal((await requestSong(send, 'jymaster')).body.data[0].keyRegistrations, 1);
    });

    await withNeteaseWorker({ RHINE_TEST_XEAPI_EXPIRE_FIRST: '1' }, async (send) => {
        assert.equal((await requestSong(send, 'jymaster')).body.data[0].keyVersion, '1');
        const responses = await Promise.all([requestSong(send, 'hires'), requestSong(send, 'jymaster')]);
        for (const response of responses) {
            assert.equal(response.body.code, 200);
            assert.equal(response.body.data[0].keyRegistrations, 2);
            assert.equal(response.body.data[0].keyVersion, '2');
        }
    });

    await withNeteaseWorker({ RHINE_TEST_XEAPI_FAIL_FIRST: '1' }, async (send, temporaryDirectory) => {
        const failed = await Promise.all([requestSong(send, 'hires'), requestSong(send, 'jymaster')]);
        for (const response of failed) {
            assert.equal(response.body.code, 503);
            assert.match(response.body.message, /音源初始化失败/);
            assert.equal(response.body.data, undefined);
        }
        assert.equal(fs.existsSync(path.join(temporaryDirectory, 'xeapi_public_key')), false);
        const retried = await requestSong(send, 'jymaster');
        assert.equal(retried.body.code, 200);
        assert.equal(retried.body.data[0].level, 'jymaster');
        assert.equal(retried.body.data[0].keyRegistrations, 2);
    });
});

test('播放诊断只收集白名单字段且不泄露业务数据', async () => {
    await withNeteaseWorker({}, async (send) => {
        const request = (id, enabled) =>
            send({
                route: '/native/netease/playback',
                params: { args: [{ id: '1', title: 'PRIVATE_DIAGNOSTIC_SENTINEL' }, 'lossless', false] },
                cookies: { MUSIC_U: 'fixture-account', ignored: 'PRIVATE_DIAGNOSTIC_SENTINEL' },
                _collectPlaybackDiagnostics: enabled,
            });
        const [recorded, ordinary] = await Promise.all([
            request('recorded', true),
            request('ordinary', false),
        ]);
        assert.equal(recorded.body.code, 200);
        assert.equal(ordinary.body.code, 200);
        assert.equal(ordinary._playbackDiagnostics, undefined);
        const events = recorded._playbackDiagnostics;
        assert.ok(events.some((event) => event.operation === 5 && event.upstreamHttpStatus === 200));
        assert.ok(events.some((event) => event.operation === 4 && event.serviceCode === 200));
        assert.ok(events.length <= 64);
        const allowed = [
            'operation',
            'workerMs',
            'transportFailed',
            'httpPhase',
            'serviceCode',
            'upstreamHttpStatus',
            'upstreamCode',
            'networkError',
            'workerOffsetMs',
        ];
        for (const event of events) {
            for (const [key, value] of Object.entries(event)) {
                assert.ok(allowed.includes(key));
                assert.ok(
                    typeof value === 'boolean' || (typeof value === 'number' && Number.isFinite(value)),
                );
            }
        }
        assert.ok(!JSON.stringify(events).includes('PRIVATE_DIAGNOSTIC_SENTINEL'));
    });
});
