// 使用发布包与本地 HTTP 响应，验证并发诊断隔离、实际 HTTP 状态及隐私白名单。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { bridgeRuntime } = require('./fixtures/bridge-runtime.cjs');
const { bridge, runtime } = bridgeRuntime();
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-playback-diagnostic-'));
const child = spawn(
    runtime,
    ['--require', path.resolve(__dirname, 'fixtures/bridge-http.cjs'), path.join(bridge, 'worker.cjs')],
    {
        cwd: bridge,
        env: {
            ...process.env,
            RHINE_TEST_BRIDGE: bridge,
            TMPDIR: temporary,
            TEMP: temporary,
            TMP: temporary,
        },
        stdio: ['pipe', 'pipe', 'pipe'],
    },
);
const pending = new Map();
let buffer = '';
let stderr = '';
child.stderr.on('data', (bytes) => {
    stderr += bytes;
});
child.stdout.on('data', (bytes) => {
    buffer += bytes;
    let end;
    while ((end = buffer.indexOf('\n')) >= 0) {
        const response = JSON.parse(buffer.slice(0, end));
        buffer = buffer.slice(end + 1);
        pending.get(response.id)?.resolve(response);
        pending.delete(response.id);
    }
});
const exited = new Promise((resolve) => {
    child.once('close', (code) => {
        for (const request of pending.values()) {
            request.reject(new Error('Diagnostic worker exited before responding'));
        }
        resolve(code);
    });
});
child.on('error', (error) => {
    for (const request of pending.values()) {
        request.reject(error);
    }
});
child.stdin.on('error', (error) => {
    for (const request of pending.values()) {
        request.reject(error);
    }
});
function request(id, enabled) {
    return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        child.stdin.write(
            JSON.stringify({
                id,
                route: '/native/netease/playback',
                params: { args: [{ id: '1', title: 'PRIVATE_DIAGNOSTIC_SENTINEL' }, 'lossless', false] },
                cookies: { MUSIC_U: 'fixture-account', ignored: 'PRIVATE_DIAGNOSTIC_SENTINEL' },
                _collectPlaybackDiagnostics: enabled,
            }) + '\n',
        );
    });
}
async function main() {
    const timeout = setTimeout(() => {
        child.kill();
    }, 15000);
    try {
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
        for (const event of events) {
            for (const [key, value] of Object.entries(event)) {
                assert.ok(
                    [
                        'operation',
                        'workerMs',
                        'transportFailed',
                        'httpPhase',
                        'serviceCode',
                        'upstreamHttpStatus',
                        'upstreamCode',
                        'networkError',
                        'workerOffsetMs',
                    ].includes(key),
                );
                assert.ok(
                    typeof value === 'boolean' || (typeof value === 'number' && Number.isFinite(value)),
                );
            }
        }
        assert.ok(!JSON.stringify(events).includes('PRIVATE_DIAGNOSTIC_SENTINEL'));
        child.stdin.end();
        assert.equal(await exited, 0);
        assert.equal(stderr, '');
        console.log('Packaged playback HTTP diagnostics, opt-in isolation and privacy checks passed');
    } finally {
        clearTimeout(timeout);
        child.kill();
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
