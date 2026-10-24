// iOS 的 Node 主线程只负责传输；每个音乐会话使用独立 Worker，注销时彻底销毁。
const net = require('node:net');
const readline = require('node:readline');
const { Worker } = require('node:worker_threads');
const socket = net.connect(Number(process.argv[2]), '127.0.0.1');
let worker;
let session;
let commands = Promise.resolve();

function send(message) {
    socket.write(JSON.stringify(message) + '\n');
}

socket.on('connect', () => send({ token: process.argv[3] }));
// Qt 结束时保持运行时存活到进程终止，禁止 Node 的退出路径结束整个应用。
socket.on('error', () => {
    // Qt 已退出时保持静默，由宿主进程统一回收运行时。
});
setInterval(() => {}, 60000);

/** 串行销毁旧 Worker 后才建立新会话，旧连接和模块全局状态不会进入新账号。 */
async function execute(message) {
    if (message.type === 'start') {
        if (worker) {
            await worker.terminate();
        }
        session = message.session;
        const current = session;
        worker = new Worker(message.script, {
            env: { ...process.env, ...message.env },
            stdin: true,
            stdout: true,
            stderr: true,
        });
        const instance = worker;
        instance.on('message', (message) => {
            if (message?.type === 'system-trust') {
                send({ ...message, session: current });
            }
        });
        instance.stdout.on('data', (bytes) => {
            send({ type: 'output', session: current, data: bytes.toString('base64') });
        });
        // 上游日志可能包含账号数据，沿用桌面端的静默策略。
        instance.stderr.resume();
        instance.on('online', () => send({ type: 'started', session: current }));
        instance.on('error', () => send({ type: 'error', session: current }));
        instance.on('exit', () => {
            send({ type: 'finished', session: current });
            if (worker === instance) {
                worker = undefined;
            }
        });
    } else if (message.session === session && worker) {
        if (message.type === 'system-trust-result') {
            worker.postMessage(message);
        } else if (message.type === 'input') {
            worker.stdin.write(Buffer.from(message.data, 'base64'));
        } else if (message.type === 'stop') {
            await worker.terminate();
        }
    }
}

readline.createInterface({ input: socket }).on('line', (line) => {
    commands = commands
        .then(() => execute(JSON.parse(line)))
        .catch(() => {
            send({ type: 'error', session });
        });
});
