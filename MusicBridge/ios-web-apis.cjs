require('./ios-system-trust.cjs');
const fetch = require('node-fetch');

// 保留 QQ 回环响应的多个 Set-Cookie
fetch.Headers.prototype.getSetCookie = function () {
    return [...(this.raw()['set-cookie'] || [])];
};
fetch.Response.json = function (body, init = {}) {
    const headers = new fetch.Headers(init.headers);
    if (!headers.has('content-type')) {
        headers.set('content-type', 'application/json');
    }
    return new fetch.Response(JSON.stringify(body), { ...init, headers });
};

for (const [name, value] of Object.entries({
    fetch,
    Headers: fetch.Headers,
    Request: fetch.Request,
    Response: fetch.Response,
    FormData: require('form-data'),
    // 为 QQ 音乐暴露 Web Crypto
    crypto: require('node:crypto').webcrypto,
})) {
    Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
}
