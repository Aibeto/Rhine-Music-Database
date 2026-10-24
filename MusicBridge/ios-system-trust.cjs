const https = require('node:https');
const tls = require('node:tls');
const { parentPort } = require('node:worker_threads');

/** 通过现有私有通道请求 Apple 信任评估，超时和失联均拒绝连接。 */
function createSystemVerifier(port) {
    let sequence = 0;
    const pending = new Map();
    port.on('message', (message) => {
        if (message?.type !== 'system-trust-result') {
            return;
        }
        pending.get(message.id)?.(message.trusted === true);
    });
    return (hostname, certificates) =>
        new Promise((resolve) => {
            const id = String(++sequence);
            const timeout = setTimeout(() => finish(false), 25000);
            timeout.unref();
            function finish(trusted) {
                clearTimeout(timeout);
                pending.delete(id);
                resolve(trusted);
            }
            pending.set(id, finish);
            port.postMessage({ type: 'system-trust', id, hostname, certificates });
        });
}

function createSystemConnection(verify) {
    return function (options, callback) {
        const hostname = String(options.servername || options.hostname || options.host || '').replace(
            /^\[|\]$/g,
            '',
        );
        const socket = tls.connect({ ...options, rejectUnauthorized: false });
        let completed = false;
        const fail = (error) => {
            if (completed) {
                return;
            }
            completed = true;
            socket.destroy();
            callback(error);
        };
        const timeout = () => {
            const error = new Error('iOS server trust evaluation timed out');
            error.code = 'ETIMEDOUT';
            fail(error);
        };
        socket.once('error', fail);
        socket.setTimeout(options.timeout || 25000, timeout);
        socket.once('secureConnect', async () => {
            try {
                const leaf = socket.getPeerCertificate(true);
                const identityError = tls.checkServerIdentity(hostname, leaf);
                if (identityError) {
                    throw identityError;
                }
                // 显式 CA 和上游额外的身份约束继续生效，不能被系统信任结果覆盖。
                if (options.ca !== undefined && !socket.authorized) {
                    throw new Error('Explicit CA validation failed');
                }
                const customError = options.checkServerIdentity?.(hostname, leaf);
                if (customError) {
                    throw customError;
                }
                const certificates = [];
                const seen = new Set();
                let certificate = leaf;
                while (certificate?.raw && certificates.length < 16) {
                    const encoded = certificate.raw.toString('base64');
                    if (seen.has(encoded)) {
                        break;
                    }
                    seen.add(encoded);
                    certificates.push(encoded);
                    certificate = certificate.issuerCertificate;
                }
                if (!(await verify(hostname, certificates))) {
                    const error = new Error('iOS does not trust the server certificate');
                    error.code = 'CERT_SYSTEM_TRUST_REJECTED';
                    throw error;
                }
                if (completed || socket.destroyed) {
                    return;
                }
                completed = true;
                socket.removeListener('error', fail);
                socket.removeListener('timeout', timeout);
                socket.setTimeout(0);
                socket.authorized = true;
                socket.authorizationError = null;
                callback(null, socket);
            } catch (error) {
                fail(error);
            }
        });
        // 必须使用异步 Agent 回调；返回握手中的 Socket 会提前写入登录请求。
        return undefined;
    };
}

if (parentPort) {
    https.Agent.prototype.createConnection = createSystemConnection(createSystemVerifier(parentPort));
}

module.exports = { createSystemConnection };
