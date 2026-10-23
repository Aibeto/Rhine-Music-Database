const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');
const { buildVersion } = require('./release-version.cjs');

/** 优先使用显式编译器路径，再查找 Inno Setup 7/6 的系统及当前用户安装目录和 PATH。 */
function findCompiler(environment = process.env) {
    if (environment.ISCC) {
        return environment.ISCC;
    }
    const roots = [environment.ProgramFiles, environment['ProgramFiles(x86)']];
    if (environment.LOCALAPPDATA) {
        roots.push(path.join(environment.LOCALAPPDATA, 'Programs'), environment.LOCALAPPDATA);
    }
    for (const version of ['7', '6']) {
        for (const root of roots) {
            if (!root) {
                continue;
            }
            const compiler = path.join(root, `Inno Setup ${version}`, 'ISCC.exe');
            if (fs.existsSync(compiler)) {
                return compiler;
            }
        }
    }
    return 'ISCC.exe';
}

/** 仅过滤部署根目录的运行数据，保留 SDK 内同名的资源。 */
function stageApplication(source, destination) {
    const excluded = new Set(['data', 'cache', 'temp']);
    fs.cpSync(source, destination, {
        recursive: true,
        filter: (file) => !excluded.has(path.relative(source, file).split(path.sep)[0].toLowerCase()),
    });
}

function run(command, argumentsList) {
    const result = spawnSync(command, argumentsList, { stdio: 'inherit' });
    if (result.error) {
        if (result.error.code === 'ENOENT' && command === 'ISCC.exe') {
            throw new Error('Install Inno Setup 6.3+ or 7, or set ISCC to the full path of ISCC.exe.');
        }
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`${command} exited with ${result.status}`);
    }
}

/**
 * 将已部署的 Windows 应用审计、暂存并交给 ISCC，失败不保留半成品。
 * 不重新构建，不读取或修改原部署目录中的账号、缓存及临时文件。
 */
function createInstaller() {
    if (process.platform !== 'win32') {
        throw new Error('Windows installer compilation must run on Windows.');
    }
    const root = path.resolve(__dirname, '..');
    const source = path.join(root, 'output/windows/RHINE MUSIC DATABASE');
    if (!fs.existsSync(path.join(source, 'RHINE MUSIC DATABASE.exe'))) {
        throw new Error('Run npm run build on Windows before creating the installer.');
    }
    const { version } = buildVersion();
    const name = `RhineMusicDatabase-v${version}-Windows-x64-Setup`;
    const output = path.join(root, 'output');
    const installer = path.join(output, name + '.exe');
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-installer-'));
    try {
        const staged = path.join(temporary, 'app');
        stageApplication(source, staged);
        run(process.env.PYTHON || 'python', [path.join(__dirname, 'audit-qt.py'), staged]);
        fs.rmSync(installer, { force: true });
        run(findCompiler(), [
            '/Qp',
            `/DAppSource=${staged}`,
            `/DAppVersion=${version}`,
            `/O${output}`,
            `/F${name}`,
            path.join(__dirname, 'windows-installer.iss'),
        ]);
        if (!fs.existsSync(installer) || fs.statSync(installer).size === 0) {
            throw new Error('ISCC did not create the expected installer.');
        }
    } catch (error) {
        fs.rmSync(installer, { force: true });
        throw error;
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
    console.log(installer);
}

module.exports = { createInstaller, findCompiler, stageApplication };
