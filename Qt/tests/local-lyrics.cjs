const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const { spawn } = require('node:child_process');
const { createInterface } = require('node:readline');
const { bridgeRuntime } = require('./fixtures/bridge-runtime.cjs');

/** 检查本地歌词归一化 */
async function main() {
    const { bridge, runtime } = bridgeRuntime();
    const cases = [
        ['timed', '[00:01.00]第一句\n[00:03.50]第二句'],
        ['plain', '本地纯文本歌词'],
        ['invalid', 123],
        ['korean', '[00:01.00]별빛을 따라 걷는 밤\n[00:02.00]우리의 노래'],
    ];
    const encodingCases = require('./fixtures/lyrics-encodings.json');
    for (const sample of encodingCases) {
        cases.push([sample.name, { contentBase64: sample.base64 }]);
    }
    cases.push(['bad-base64', { contentBase64: '!!!!' }]);
    cases.push(['utf8-fallback', { content: '[00:01.00]内嵌歌词', contentBase64: '//4AAAE=' }]);
    const optionalCases = [];
    if (process.argv[3]) {
        const root = path.resolve(process.argv[3]);
        for (const file of fs.readdirSync(root, { recursive: true })) {
            if (path.extname(file).toLowerCase() === '.lrc') {
                const request = { contentBase64: fs.readFileSync(path.join(root, file)).toString('base64') };
                cases.push([file, request]);
                optionalCases.push(file);
            }
        }
    }
    const child = spawn(runtime, [path.join(bridge, 'worker.cjs')], {
        cwd: bridge,
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    let errors = '';
    child.stderr.on('data', (chunk) => {
        errors += chunk;
    });
    const timeout = setTimeout(() => child.kill(), 10000);
    const responses = new Map();
    const exited = new Promise((resolve, reject) => {
        child.on('error', reject);
        child.stdin.on('error', reject);
        // close 在 stderr 读完后触发，保留模块加载失败的原始原因。
        child.on('close', (code, signal) => {
            clearTimeout(timeout);
            if (code === 0) {
                resolve();
            } else {
                reject(new Error(`Local lyrics worker failed (${code ?? signal}): ${errors}`));
            }
        });
    });
    createInterface({ input: child.stdout }).on('line', (line) => {
        const response = JSON.parse(line);
        responses.set(response.id, response.body);
        if (responses.size === cases.length) {
            child.stdin.end();
        }
    });
    for (const [id, content] of cases) {
        child.stdin.write(
            JSON.stringify({
                id,
                route: '/local/lyrics',
                params: typeof content === 'object' ? content : { content },
                cookies: {},
            }) + '\n',
        );
    }
    try {
        await exited;
    } finally {
        clearTimeout(timeout);
        if (child.exitCode === null) {
            child.kill();
        }
    }
    const timed = responses.get('timed');
    assert.equal(timed.code, 200);
    assert.equal(timed.document.source, 'local');
    assert.equal(timed.document.timing, 'line');
    assert.deepEqual(
        timed.document.lines.map((line) => line.startMs),
        [1000, 3500],
    );
    assert.equal(responses.get('plain').document.plainText, '本地纯文本歌词');
    assert.equal(responses.get('invalid').code, 400);
    assert.equal(responses.get('korean').document.lines[0].text, '별빛을 따라 걷는 밤');
    for (const sample of encodingCases) {
        const result = responses.get(sample.name);
        assert.equal(result.code, 200, sample.name);
        assert.equal(
            result.document.lines.length
                ? result.document.lines.map((line) => line.text).join('\n')
                : result.document.plainText,
            sample.text.replace(/\[\d+:\d+\.\d+\]/g, '').trim(),
            sample.name,
        );
    }
    assert.equal(responses.get('bad-base64').code, 400);
    assert.equal(responses.get('utf8-fallback').document.plainText, '内嵌歌词');
    for (const id of optionalCases) {
        const result = responses.get(id);
        assert.equal(result.code, 200, id);
        assert.ok(result.document.lines.length || result.document.plainText, id);
    }
    console.log(
        'Local lyrics private IPC passed; encoding fixtures:',
        encodingCases.length,
        'real files:',
        optionalCases.length,
    );
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
