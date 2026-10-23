const fs = require('node:fs');
const path = require('node:path');
const { buildVersion } = require('./release-version.cjs');
const { app } = require('./app-layout.cjs');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));
const win = process.platform === 'win32';
const linux = process.platform === 'linux';
// 统一使用 python3：多数发行版不再提供 python 命令。
const python = process.env.PYTHON || (win ? 'python' : 'python3');
if (!fs.existsSync(app)) {
    throw Error('Run npm run build first');
}

/** 发布步骤失败时抛出错误，使临时目录始终能够清理。 */
function run(cmd, args) {
    const result = spawnSync(cmd, args, {
        stdio: 'inherit',
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`${cmd} exited with ${result.status}`);
    }
}
if (win) {
    // Windows 
    require('./installer-windows.cjs').createInstaller();
    process.exit(0);
}
run(python, ['scripts/audit-qt.py', app]);
if (linux) {
    // Linux 直接交付已构建的 AppImage，不再额外压缩，避免二次解包目录。
    const image = path.resolve('output/RhineMusicDatabase-Linux-x86_64.AppImage');
    if (!fs.existsSync(image)) {
        throw Error('AppImage was not built; run npm run build first');
    }
    console.log(image);
    process.exit(0);
}
const version = buildVersion();
const name = version.release
    ? `RhineMusicDatabase-v${version.version}-macOS-arm64`
    : `RhineMusicDatabase-macOS-${Date.now()}`;
const archive = path.resolve(`output/${name}.zip`);
run('codesign', ['--verify', '--deep', '--strict', app]);
run('ditto', ['-c', '-k', '--sequesterRsrc', '--keepParent', app, archive]);
console.log(archive);
