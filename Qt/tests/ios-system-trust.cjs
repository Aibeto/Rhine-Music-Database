const assert = require('node:assert/strict');
const fs = require('node:fs');
const https = require('node:https');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { once } = require('node:events');
const { createSystemConnection } = require('../../MusicBridge/ios-system-trust.cjs');

/** 检查信任门控真实的 TLS 连接，不允许证书批准前发送 HTTP 请求。 */
async function runSystemTrustChecks() {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-system-trust-'));
    const key = path.join(directory, 'key.pem');
    const cert = path.join(directory, 'cert.pem');
    const config = path.join(directory, 'openssl.cnf');
    fs.writeFileSync(
        config,
        '[req]\ndistinguished_name=dn\nx509_extensions=server\n[dn]\n[server]\nsubjectAltName=DNS:localhost\n',
    );
    const generated = spawnSync(
        'openssl',
        [
            'req',
            '-x509',
            '-newkey',
            'rsa:2048',
            '-nodes',
            '-keyout',
            key,
            '-out',
            cert,
            '-days',
            '1',
            '-subj',
            '/CN=localhost',
            '-config',
            config,
        ],
        { stdio: 'ignore' },
    );
    assert.equal(generated.status, 0, 'Local TLS fixture certificate generation failed');
    let requests = 0;
    const server = https.createServer(
        { key: fs.readFileSync(key), cert: fs.readFileSync(cert) },
        (_, res) => {
            ++requests;
            res.end('trusted');
        },
    );
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const agents = [];
    function request(verify, extra = {}) {
        const agent = new https.Agent();
        agents.push(agent);
        agent.createConnection = createSystemConnection(verify);
        return new Promise((resolve, reject) => {
            const operation = https.get(
                { host: '127.0.0.1', servername: 'localhost', port: server.address().port, agent, ...extra },
                (response) => {
                    let body = '';
                    response.on('data', (bytes) => {
                        body += bytes;
                    });
                    response.on('end', () => resolve(body));
                },
            );
            operation.on('error', reject);
        });
    }
    try {
        const accepted = request(async (hostname, chain) => {
            assert.equal(hostname, 'localhost');
            assert(chain.length > 0);
            await new Promise((resolve) => setTimeout(resolve, 25));
            assert.equal(requests, 0, 'HTTP data escaped before trust approval');
            return true;
        });
        assert.equal(await accepted, 'trusted');
        assert.equal(requests, 1);
        await assert.rejects(
            request(async () => false),
            { code: 'CERT_SYSTEM_TRUST_REJECTED' },
        );
        await assert.rejects(
            request(async () => true, { servername: 'wrong.invalid' }),
            {
                code: 'ERR_TLS_CERT_ALTNAME_INVALID',
            },
        );
        await assert.rejects(
            request(async () => {
                throw new Error('Native verifier unavailable');
            }),
            /Native verifier unavailable/,
        );
        assert.equal(requests, 1, 'Rejected TLS connections sent HTTP requests');
        console.log('iOS TLS trust gating, hostname checks and fail-closed requests passed');
    } finally {
        for (const agent of agents) {
            agent.destroy();
        }
        server.close();
        fs.rmSync(directory, { recursive: true, force: true });
    }
}

module.exports = { runSystemTrustChecks };
