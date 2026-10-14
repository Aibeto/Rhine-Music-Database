import assert from 'node:assert/strict';
import fs from 'node:fs';
import { MusicStore, API } from '../logic/music.js';
import { HUD } from '../logic/hud.js';
import { QQ_DEVICE_NOTICE, loginFailureMessage } from '../logic/login-state.js';

const content = JSON.parse(fs.readFileSync('Resources/slots.json'));
const preferences = new Map([['provider', 'qq']]);
let provider = 'qq';
let unavailable = false;
let qrReply = {
    code: 800,
    failureStage: 'credential-validation',
    failureReason: 'upstream-rejected',
    upstreamCode: 1000,
    retryAfterMs: 30000,
};
const calls = [];
const delays = [];
const credentials = { qq: true, netease: true };
const host = {
    preference: (key) => preferences.get(key) || '',
    setPreference: (key, value) => preferences.set(key, value),
    setProvider: (value) => {
        provider = value;
    },
    authenticated: () => credentials[provider],
    acceptCookies() {},
    fromLink: () => '',
    qrImage: () => 'local-qr',
    delay: (milliseconds, done) => {
        delays.push(done);
    },
    mediaControl() {},
    logout: () => {
        credentials[provider] = false;
    },
    saveCredential: () => '',
    request(route, params, done) {
        calls.push(route);
        let body;
        if (route.endsWith('/login/qr/key')) {
            body = { code: 200, data: { unikey: 'fixture-key' } };
        } else if (route.endsWith('/login/qr/create')) {
            body = { code: 200, data: { qrimg: 'data:image/png;base64,fixture' } };
        } else if (route.endsWith('/login/qr/check')) {
            body = qrReply;
        } else if (route.endsWith('/login/qr/cancel')) {
            body = { code: 200 };
        } else if (unavailable) {
            body = { code: 502, message: '临时网关失败' };
        } else if (route === '/qq/user/detail') {
            body = { code: 200, profile: { str_musicid: '42', info: { nick: 'QQ 用户' } } };
        } else if (route === '/user/account') {
            body = { code: 200, profile: { userId: 7, nickname: '网易用户' } };
        } else {
            body = { code: 200 };
        }
        done(JSON.stringify({ body }), '');
    },
};
const runtime = { musicClear() {}, musicIdentity() {}, setMode() {}, select() {} };
const flush = async () => {
    for (let i = 0; i < 12; i++) {
        await Promise.resolve();
    }
};
const store = new MusicStore(host, runtime, content);
await store.restoreAccount();
assert.equal(store.uid, '42');
await store.switchProvider('netease');
assert.equal(store.uid, '7');
for (let i = 0; i < 4; i++) {
    await store.switchProvider('qq');
    assert.equal(store.uid, '42');
    await store.switchProvider('netease');
    assert.equal(store.uid, '7');
}
// 验证临时网关失败时重启与切换仍恢复本机账号，但明确过期时撤销显示。
unavailable = true;
const restarted = new MusicStore(host, runtime, content);
assert.equal(restarted.uid, '7');
await restarted.restoreAccount();
assert.equal(restarted.uid, '7');
await restarted.switchProvider('qq');
assert.equal(restarted.uid, '42');
restarted.api.account = async () => {
    throw Object.assign(new Error('已失效'), { code: 401 });
};
await restarted.restoreAccount();
assert.equal(restarted.uid, '');
assert.equal(preferences.get('account-id-netease'), '7');
await restarted.switchProvider('netease');
restarted.api.account = async () => {
    throw Object.assign(new Error('需要登录'), { code: 301 });
};
await restarted.restoreAccount();
assert.equal(restarted.uid, '');
unavailable = false;
await store.switchProvider('qq');
let releaseAccount;
store.api.account = () =>
    new Promise((resolve) => {
        releaseAccount = resolve;
    });
const lateAccount = store.restoreAccount();
await store.switchProvider('netease');
releaseAccount(['999', '旧账号响应']);
await lateAccount;
assert.equal(store.uid, '7');
await store.switchProvider('qq');

// 不记录凭据或任意上游字段，只保留安全诊断和准确的冷却期限。
const api = new API({
    request(route, params, done) {
        done(
            JSON.stringify({
                body: {
                    code: 429,
                    retryAfterMs: 10000,
                    lastFailure: {
                        failureStage: 'credential-exchange',
                        upstreamCode: 20279,
                        musickey: 'forbidden',
                    },
                    musickey: 'forbidden',
                },
            }),
            '',
        );
    },
});
await assert.rejects(api.request('/qq/login/qr/key'), (error) => {
    assert.equal(error.retryAfterMs, 10000);
    assert.equal(error.lastFailure.upstreamCode, 20279);
    assert.equal(error.lastFailure.musickey, undefined);
    assert.equal(error.musickey, undefined);
    return true;
});
assert.match(loginFailureMessage({ code: 800, failureReason: 'qr-timeout' }), /已过期/);
assert.match(loginFailureMessage({ code: 800, failureStage: 'mqtt-listener' }), /连接中断/);
let nowMs = 100000;
const originalNow = Date.now;
Date.now = () => nowMs;
try {
    const pending = store.startLogin();
    await flush();
    const keyRequests = calls.filter((route) => route.endsWith('/login/qr/key')).length;
    delays.shift()();
    await pending;
    assert.match(store.loginStatus, /凭证验证失败/);
    assert.equal(store.loginRetrySeconds, 30);
    await store.startLogin();
    assert.equal(calls.filter((route) => route.endsWith('/login/qr/key')).length, keyRequests);
    store.cancelLogin();
    await store.startLogin();
    assert.equal(store.loginRetrySeconds, 30);
    nowMs += 30000;
    delays.shift()();
    await flush();
    assert.equal(store.loginRetrySeconds, 0);
    assert.equal(store.canRegenerateLogin, true);
    qrReply = { code: 802 };
    const waiting = store.startLogin();
    await flush();
    delays.shift()();
    await flush();
    assert.equal(store.loginPhase, 'confirming');
    const beforeDuplicate = calls.length;
    await store.startLogin();
    assert.equal(calls.length, beforeDuplicate);
    store.cancelLogin();
    delays.shift()();
    await waiting;
    assert.equal(store.loginPhase, 'idle');
} finally {
    Date.now = originalNow;
}

// 切换扫码方式会先取消、再启动；第二次清理仍必须等待同一份取消响应。
const originalRequest = host.request;
let finishCancellation;
host.request = (route, params, done) => {
    if (route.endsWith('/login/qr/cancel')) {
        finishCancellation = () => done(JSON.stringify({ body: { code: 200 } }), '');
    } else {
        originalRequest(route, params, done);
    }
};
store.qrKey = 'previous-qr';
store.cancelLogin();
const keyCount = calls.filter((route) => route.endsWith('/login/qr/key')).length;
const replacement = store.startLogin();
await flush();
assert.equal(calls.filter((route) => route.endsWith('/login/qr/key')).length, keyCount);
finishCancellation();
await flush();
assert.equal(calls.filter((route) => route.endsWith('/login/qr/key')).length, keyCount + 1);
host.request = originalRequest;
store.cancelLogin();
delays.shift()();
await replacement;

const commands = [];
const glyphs = { wrap: (text) => [text.slice(0, 35), text.slice(35)] };
const hud = new HUD({}, glyphs, store, runtime);
hud.image = () => {};
hud.text = (text, x, y) => commands.push({ text, x, y });
hud.limited = () => {};
hud.button = () => {};
hud.loginPanel(0, 0, 600);
const notice = commands.filter((command) => command.y >= 520 && command.y < 600);
assert.equal(notice.map((command) => command.text).join(''), QQ_DEVICE_NOTICE);
console.log('账号重启、重复平台切换、真实过期、扫码失败分类、冷却与确认隔离、设备提示布局通过');
