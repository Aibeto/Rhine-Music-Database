// Linux 随包提供自编的 FFmpeg：发行版 libavcodec 动态依赖 x265/aom/SvtAv1Enc 等
// 编码器库（合计约 55 MB），而应用只用 Qt 的 FFmpeg 后端解码音频，不需要任何编码器。
// 裁剪口径与 scripts/prepare-ios-ffmpeg.cjs 保持一致：保留全部解码与解复用能力，
// 关闭编码器、复用器、avdevice、avfilter 与命令行工具，且不自动探测外部库，
// 因此不会链接 x264/x265/aom/dav1d 等 GPL 或体积巨大的第三方库。
//
// 许可：不启用 --enable-gpl、不启用任何非自由组件时，FFmpeg 以 LGPL-2.1-or-later 构建。
// 脚本会在 configure 之后断言这一点，构建产物随包分发时同时保留许可与来源记录。
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createHash } = require('node:crypto');
const { spawnSync } = require('node:child_process');
const source = require('../vendor/ffmpeg/source.json');
const root = path.resolve(__dirname, '..');
const directory = path.join(root, 'output/dependencies/ffmpeg-linux');
const install = path.join(directory, 'install');
const build = path.join(directory, 'build');
// 仓库以 submodule 方式纳入 FFmpeg 时直接就地构建，省去下载且源码随仓库可得。
const submodule = path.join(root, 'third_party/ffmpeg');

function run(command, args, cwd = directory) {
    const result = spawnSync(command, args, { cwd, stdio: 'inherit' });
    if (result.error || result.status !== 0) {
        throw result.error || new Error(`${command} exited with ${result.status}`);
    }
}

function capture(command, args, cwd = directory) {
    const result = spawnSync(command, args, { cwd, encoding: 'utf8' });
    if (result.error || result.status !== 0) {
        throw result.error || new Error(`${command} exited with ${result.status}`);
    }
    return result.stdout;
}

/** 取出源码：优先 submodule，其次按锁定校验和下载发布包。 */
function prepareSourceTree() {
    // submodule 只作为快速克隆使用，源码保持上游原样：提交与锁定值不一致或工作树有改动时，
    // 一律回退到发布包，保证构建产物可复现。
    if (source.submodule && fs.existsSync(path.join(submodule, 'configure'))) {
        const git = (args) => spawnSync('git', ['-C', submodule, ...args], { encoding: 'utf8' });
        const revision = git(['rev-parse', 'HEAD']).stdout.trim();
        const dirty = git(['status', '--porcelain']).stdout.trim();
        if (dirty) {
            console.warn('third_party/ffmpeg has local modifications; use the release tarball');
        } else if (revision !== source.submodule.commit) {
            console.warn(
                `third_party/ffmpeg is at ${revision}, expected ${source.submodule.commit}; use the release tarball`,
            );
        } else {
            return {
                tree: submodule,
                provenance: { kind: 'submodule', tag: source.submodule.tag, revision },
            };
        }
    }
    const archive = path.join(directory, `ffmpeg-${source.version}.tar.xz`);
    fs.mkdirSync(directory, { recursive: true });
    if (!fs.existsSync(archive)) {
        run('curl', ['-L', '--fail', '--retry', '3', source.download, '-o', archive]);
    }
    const hash = createHash('sha256').update(fs.readFileSync(archive)).digest('hex');
    if (hash !== source.sha256) {
        throw new Error('FFmpeg source checksum mismatch');
    }
    const tree = path.join(directory, `ffmpeg-${source.version}`);
    if (!fs.existsSync(tree)) {
        run('tar', ['-xf', archive]);
    }
    return {
        tree,
        provenance: { kind: 'release', download: source.download, sha256: hash },
    };
}

// 构建参数：shared 便于随包替换；关闭一切与音频解码无关的部分。
// --disable-x86asm 免去构建机需要 nasm，音频解码不依赖汇编加速。
const configureArguments = [
    '--enable-shared',
    '--disable-static',
    '--disable-programs',
    '--disable-doc',
    '--disable-debug',
    '--disable-autodetect',
    '--disable-encoders',
    '--disable-muxers',
    '--disable-avdevice',
    '--disable-avfilter',
    '--disable-x86asm',
    // 库都在 AppDir 的 usr/lib，运行时按 $ORIGIN 相互查找，不落到构建机的安装目录。
    // 写成 $$ORIGIN：configure 原样写入 config.mak，由 make 展开成 $ORIGIN。
    '--extra-ldflags=-Wl,-rpath,$$ORIGIN',
];

/**
 * 断言构建结果是 LGPL
 * 配成 GPL 或非自由组件后随包分发会改变整个应用的许可，必须在构建期拦住。
 */
function assertLgpl() {
    const markers = ['config.h', 'config_components.h', 'ffbuild/config.mak'];
    let inspected = false;
    for (const marker of markers) {
        const file = path.join(build, marker);
        if (!fs.existsSync(file)) {
            continue;
        }
        const content = fs.readFileSync(file, 'utf8');
        if (/CONFIG_(?:GPLV3|GPL|NONFREE)\s*(?:1|yes)/.test(content)) {
            throw new Error(`FFmpeg was configured with GPL or nonfree components (${marker})`);
        }
        if (/CONFIG_(?:GPL|NONFREE)/.test(content)) {
            inspected = true;
        }
    }
    if (!inspected) {
        console.warn('Cannot confirm the FFmpeg license from generated config; check configure output');
    }
}

function main() {
    const { tree, provenance } = prepareSourceTree();
    const stamp = path.join(install, '.build.json');
    const fingerprint =
        JSON.stringify(provenance) +
        '\n' +
        configureArguments.join(' ') +
        '\n' +
        createHash('sha256').update(fs.readFileSync(__filename)).digest('hex');
    if (fs.existsSync(stamp) && JSON.parse(fs.readFileSync(stamp, 'utf8')).fingerprint === fingerprint) {
        return;
    }
    fs.mkdirSync(build, { recursive: true });
    fs.rmSync(build, { recursive: true, force: true });
    fs.mkdirSync(build, { recursive: true });
    run(path.join(tree, 'configure'), [`--prefix=${install}`, ...configureArguments], build);
    assertLgpl();
    run('make', ['-j', process.env.RHINE_BUILD_JOBS || String(os.cpus().length)], build);
    run('make', ['install'], build);
    // 随包保留来源与构建参数，满足 LGPL 的源码可得性要求。
    fs.writeFileSync(
        stamp,
        JSON.stringify(
            {
                fingerprint,
                project: source.name,
                version: source.version,
                license: 'LGPL-2.1-or-later',
                licenseFile: source.license,
                ...provenance,
                configure: configureArguments,
            },
            null,
            4,
        ) + '\n',
    );
    fs.copyFileSync(stamp, path.join(install, 'build.json'));
}
main();
