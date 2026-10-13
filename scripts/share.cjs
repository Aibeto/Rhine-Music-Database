const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));
const mac = process.platform === 'darwin';
const python = process.env.PYTHON || (mac ? 'python3' : 'python');
const app = mac ? 'output/RHINE MUSIC DATABASE.app' : 'output/windows/RHINE MUSIC DATABASE';
if (!fs.existsSync(app)) {
    throw Error('Run npm run build first');
}

function run(cmd, args) {
    const r = spawnSync(cmd, args, {
        stdio: 'inherit',
    });
    if (r.error) {
        throw r.error;
    }
    if (r.status !== 0) {
        process.exit(r.status || 1);
    }
}
run(python, ['scripts/audit-qt.py', app]);
const archive = path.resolve(`output/RhineMusicDatabase-${mac ? 'macOS' : 'Windows'}-${Date.now()}.zip`);
if (mac) {
    run('codesign', ['--verify', '--deep', '--strict', app]);
    run('ditto', ['-c', '-k', '--sequesterRsrc', '--keepParent', app, archive]);
} else {
    run(python, ['-m', 'zipfile', '-c', archive, app]);
}
console.log(archive);
