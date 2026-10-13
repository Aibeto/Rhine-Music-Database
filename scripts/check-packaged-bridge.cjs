// 发布包内执行真实协议模块，只替换上游 HTTP 响应；不访问实际音乐 API。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const bridge = path.resolve(process.argv[2]);
const temp = fs.mkdtempSync(path.join(require('node:os').tmpdir(), 'rhine-package-check-'));
const hook = path.resolve(__dirname, '../Qt/tests/fixtures/bridge-http.cjs');
async function main() {
    let child;
    try {
        child = spawn(
            path.join(bridge, process.platform === 'win32' ? 'node.exe' : 'node'),
            ['--require', hook, path.join(bridge, 'worker.cjs')],
            {
                cwd: bridge,
                env: {
                    ...process.env,
                    TMPDIR: temp,
                    TEMP: temp,
                    LANG: 'en_US.UTF-8',
                    RHINE_TEST_BRIDGE: bridge,
                },
                stdio: ['pipe', 'pipe', 'pipe'],
            },
        );
        let errors = '';
        let buffer = '';
        const pending = new Map();
        child.stderr.on('data', (d) => (errors += d));
        child.stdout.on('data', (data) => {
            buffer += data;
            let end;
            while ((end = buffer.indexOf('\n')) >= 0) {
                const line = buffer.slice(0, end);
                buffer = buffer.slice(end + 1);
                let result;
                try {
                    result = JSON.parse(line);
                } catch (error) {
                    for (const p of pending.values()) {
                        p.reject(error);
                    }
                    return;
                }
                const p = pending.get(result.id);
                if (p) {
                    clearTimeout(p.timer);
                    pending.delete(result.id);
                    p.resolve(result);
                }
            }
        });
        function request(id, route, params = {}, cookies = {}) {
            return new Promise((resolve, reject) => {
                const timer = setTimeout(() => {
                    pending.delete(id);
                    reject(new Error('IPC timeout: ' + errors));
                }, 15000);
                pending.set(id, {
                    resolve,
                    reject,
                    timer,
                });
                child.stdin.write(
                    JSON.stringify({
                        id,
                        route,
                        params,
                        cookies,
                    }) + '\n',
                );
            });
        }
        const search = await request('search', '/search', {
            keywords: 'fixture',
            type: 1,
        });
        assert.equal(search.body.code, 200);
        const audio = await request('audio', '/song/url/v1', {
            id: '1',
            level: 'lossless',
            unblock: false,
        });
        assert.equal(audio.body.code, 200);
        assert(audio.body.data[0].url);
        const standard = await request('standard', '/song/url/v1', {
            id: '1',
            level: 'standard',
            unblock: false,
        });
        assert.equal(standard.body.code, 200);
        assert(standard.body.data[0].url);
        assert.equal(audio.body.data[0].requestedBr, 999000);
        assert.equal(standard.body.data[0].requestedBr, 128000);
        const authenticated = await request(
            'authenticated',
            '/song/url/v1',
            {
                id: '1',
                level: 'exhigh',
                unblock: false,
            },
            {
                MUSIC_U: 'fixture-account',
            },
        );
        assert.equal(authenticated.body.data[0].authenticated, true);
        assert.equal(authenticated.body.data[0].requestedBr, 320000);
        const denied = await request('denied', '/song/url/v1', {
            id: '2',
            level: 'standard',
            unblock: false,
        });
        assert.equal(denied.body.code, 403);
        const qq = await request('qq', '/qq/getSearchByKey', {
            key: 'fixture',
        });
        assert.equal(qq.body.response.data.song.list[0].mid, 'fixtureMID');
        const charts = await request('charts', '/qq/getTopLists');
        assert.equal(charts.body.response.data.topList[0].id, 4);
        const chart = await request('chart', '/qq/getRanks', {
            topId: 4,
            period: charts.body.response.data.topList[0].period,
            limit: 100,
            page: 0,
        });
        assert.equal(chart.body.response.req_1.data.songInfoList[0].mid, 'fixtureMID');
        for (const route of ['/cloud', '/mv/url', '/personal_fm', '/qq/getLyric', '/qq/getComments']) {
            const rejected = await request(route, route);
            assert.equal(rejected.body.code, 503);
        }
        child.stdin.end();
        await new Promise((resolve) => child.once('exit', resolve));
        assert.equal(errors, '');
        console.log(
            'Packaged NetEase Enhanced and QQ IPC, real dependency loading, local responses and core route allowlists passed',
        );
    } finally {
        if (child && child.exitCode === null) {
            child.kill();
        }
        fs.rmSync(temp, {
            recursive: true,
            force: true,
        });
    }
}
main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
