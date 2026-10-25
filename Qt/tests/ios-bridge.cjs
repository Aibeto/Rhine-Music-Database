// 验证 iOS 核心传输及会话重建
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const readline = require('node:readline');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { bridgeRuntime, bridgeSourceDirectory } = require('./fixtures/bridge-runtime.cjs');

async function main() {
    if (process.platform === 'darwin') {
        await require('./ios-system-trust.cjs').runSystemTrustChecks();
    }
    const { bridge, runtime } = bridgeRuntime();
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-ios-bridge-'));
    const server = net.createServer();
    let child;
    let socket;
    const messages = [];
    const responses = new Map();
    const buffers = new Map();
    let failure;
    const timeout = setTimeout(() => {
        failure = new Error('iOS bridge regression timed out');
    }, 25000);
    async function waitFor(predicate) {
        while (!predicate()) {
            if (failure) {
                throw failure;
            }
            await new Promise((resolve) => setTimeout(resolve, 10));
        }
    }
    function send(message) {
        socket.write(JSON.stringify(message) + '\n');
    }
    try {
        server.listen(0, '127.0.0.1');
        await once(server, 'listening');
        const connection = once(server, 'connection', { signal: AbortSignal.timeout(25000) });
        child = spawn(
            runtime,
            [
                '--require',
                path.join(bridgeSourceDirectory(), 'ios-web-apis.cjs'),
                path.resolve('MusicBridge/ios-runtime.cjs'),
                String(server.address().port),
                'fixture-token',
            ],
            { stdio: ['ignore', 'ignore', 'pipe'] },
        );
        child.on('error', (error) => {
            failure = error;
        });
        child.on('exit', () => {
            failure = new Error('Embedded controller exited unexpectedly');
        });
        [socket] = await connection;
        readline.createInterface({ input: socket }).on('line', (line) => {
            const message = JSON.parse(line);
            messages.push(message);
            if (message.type === 'system-trust') {
                failure = new Error('Offline fixture unexpectedly requested TLS: ' + message.hostname);
                send({ ...message, type: 'system-trust-result', trusted: false });
            }
            if (message.type === 'error') {
                failure = new Error('iOS Worker failed');
            }
            if (message.type !== 'output') {
                return;
            }
            let buffer = (buffers.get(message.session) || '') + Buffer.from(message.data, 'base64');
            let end;
            while ((end = buffer.indexOf('\n')) >= 0) {
                const response = JSON.parse(buffer.slice(0, end));
                responses.set(message.session + ':' + response.id, response);
                buffer = buffer.slice(end + 1);
            }
            buffers.set(message.session, buffer);
        });
        await waitFor(() => messages.some((message) => message.token === 'fixture-token'));
        for (const session of ['first', 'second']) {
            const directory = path.join(temporary, session);
            fs.mkdirSync(directory);
            send({
                type: 'start',
                session,
                script: path.join(bridge, 'worker.cjs'),
                env: {
                    TMPDIR: directory,
                },
            });
            await waitFor(() =>
                messages.some((message) => message.session === session && message.type === 'started'),
            );
            send({
                type: 'input',
                session,
                data: Buffer.from(
                    JSON.stringify({ id: session, route: '/local/lyrics', params: { content: session } }) +
                        '\n',
                ).toString('base64'),
            });
            await waitFor(() => responses.has(session + ':' + session));
            const response = responses.get(session + ':' + session);
            assert.equal(response.body.code, 200);
            assert.equal(response.body.document.plainText, session);
            send({ type: 'stop', session });
            // 不等待退出就发送下一次 start，控制器必须等待旧 Worker 销毁。
        }
        await waitFor(() =>
            messages.some((message) => message.session === 'second' && message.type === 'finished'),
        );
        const firstExit = messages.findIndex(
            (message) => message.session === 'first' && message.type === 'finished',
        );
        const secondStart = messages.findIndex(
            (message) => message.session === 'second' && message.type === 'started',
        );
        assert(firstExit >= 0 && firstExit < secondStart);
        console.log('iOS Worker transport and session replacement passed');
    } finally {
        clearTimeout(timeout);
        child?.kill();
        socket?.destroy();
        server.close();
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
