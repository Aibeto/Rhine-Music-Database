// 同一资源与构建流程；平台分支仅处理 Qt 部署和系统签名。
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
process.chdir(root);
const win = process.platform === 'win32';
const mac = process.platform === 'darwin';
if (!win && !mac) {
    throw Error('Supported platforms: macOS and Windows');
}
const python = process.env.PYTHON || (win ? 'python' : 'python3');
const nodeName = win ? 'node.exe' : 'node';
/**
 * 串行执行构建或部署命令，失败立即终止。
 * 命令和参数分开传递，不经过 Shell 解释。
 */
function run(command, args = [], env = {}) {
    const result = spawnSync(command, args, {
        stdio: 'inherit',
        env: {
            ...process.env,
            ...env,
        },
        shell: false,
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw Error(`${command} exited with ${result.status}`);
    }
}

function npmCi(folder) {
    run(process.execPath, [
        process.env.npm_execpath,
        'ci',
        '--prefix',
        folder,
        '--omit=dev',
        '--ignore-scripts',
        '--no-audit',
        '--no-fund',
    ]);
}
/**
 * 准备字体、模型、锁定桥接依赖和运行时资源；保持原有导出与检查顺序。
 */
function prepareResources() {
    fs.mkdirSync('output', {
        recursive: true,
    });
    run(python, ['scripts/check-qt-project.py']);
    run(python, ['scripts/export-assets.py']);
    for (const file of fs.readdirSync('Assets/fonts')) {
        fs.copyFileSync(path.join('Assets/fonts', file), path.join('output/resources', file));
    }

    run(process.execPath, ['scripts/check-music-upstream.cjs']);
    const bridge = 'output/resources/MusicBridge';
    fs.mkdirSync(bridge, {
        recursive: true,
    });
    for (const file of ['worker.cjs', 'qq.cjs', 'package.json', 'package-lock.json']) {
        fs.copyFileSync(path.join('MusicBridge', file), path.join(bridge, file));
    }
    fs.cpSync('MusicBridge/upstream', path.join(bridge, 'upstream'), {
        recursive: true,
    });
    const bridgePackages = {
        '@neteasecloudmusicapienhanced/api': '4.41.1',
        '@yakult-green-tea/qq-music-api': '3.1.5',
    };
    if (
        Object.entries(bridgePackages).some(([name, version]) => {
            try {
                return (
                    JSON.parse(fs.readFileSync(path.join(bridge, 'node_modules', name, 'package.json')))
                        .version !== version
                );
            } catch {
                return true;
            }
        })
    ) {
        npmCi(bridge);
    }
    const binary =
        process.env.RHINE_NODE_BINARY ||
        (mac && process.arch === 'arm64' ? path.resolve('vendor/node-arm64/node') : process.execPath);
    const license = process.env.RHINE_NODE_LICENSE || path.join(path.dirname(binary), 'LICENSE');
    if (!fs.existsSync(binary) || !fs.existsSync(license)) {
        throw Error('Provide RHINE_NODE_BINARY and its matching RHINE_NODE_LICENSE');
    }
    if (mac) {
        const deps = spawnSync('otool', ['-L', binary], {
            encoding: 'utf8',
        });
        if (
            deps.status !== 0 ||
            /\/opt\/|\/Users\/|@rpath/.test(deps.stdout.split('\n').slice(1).join('\n'))
        ) {
            throw Error('Node must be an official standalone distribution');
        }
    }
    fs.copyFileSync(binary, path.join(bridge, nodeName));
    fs.copyFileSync(license, path.join(bridge, 'NODE-LICENSE'));
    fs.chmodSync(path.join(bridge, nodeName), 0o755);
    run(process.execPath, ['scripts/export-lighting.mjs']);
    run(process.execPath, ['scripts/bundle.mjs']);
    run(process.execPath, ['scripts/check-runtime.mjs']);
    run(process.execPath, ['scripts/bundle-qt.mjs']);
    run(process.execPath, ['scripts/export-scene-fixture.mjs']);
    fs.copyFileSync('Assets/room-pmrem.bin', 'output/resources/room-pmrem.bin');
}
/**
 * 验证 Qt kit 后串行配置和编译，返回部署所需的路径与构建类型。
 */
function buildQt() {
    const qt = process.env.QT_ROOT || (mac ? path.join(os.homedir(), 'Qt/6.12.0/macos') : '');
    if (!qt || !fs.existsSync(path.join(qt, 'lib/cmake/Qt6/Qt6Config.cmake'))) {
        throw Error('Set QT_ROOT to a Qt desktop kit with Shader Tools and private headers');
    }
    const config = process.env.RHINE_BUILD_TYPE || 'Release';
    run('cmake', [
        '-S',
        '.',
        '-B',
        'output/qt-build',
        '-G',
        'Ninja',
        `-DCMAKE_PREFIX_PATH=${qt}`,
        `-DCMAKE_BUILD_TYPE=${config}`,
        '-DRHINE_PACKAGE_APP=OFF',
    ]);
    run('cmake', ['--build', 'output/qt-build']);
    return {
        qt,
        config,
    };
}
/**
 * 组装独立应用并执行平台部署和签名，所有文件沿用原资源清单。
 */
function deployApp(qt, config, existingBundle = '') {
    const app = path.resolve(
        existingBundle || (mac ? 'output/RHINE MUSIC DATABASE.app' : 'output/windows/RHINE MUSIC DATABASE'),
    );
    if (!existingBundle && fs.existsSync(app)) {
        fs.renameSync(app, app + '.previous-' + Date.now());
    }
    const resources = mac ? path.join(app, 'Contents/Resources') : path.join(app, 'resources');
    if (mac && !existingBundle) {
        fs.cpSync('output/qt-build/RhineMusicDatabase.app', app, {
            recursive: true,
        });
    } else if (!mac) {
        fs.mkdirSync(app, {
            recursive: true,
        });
        fs.copyFileSync('output/qt-build/RhineMusicDatabase.exe', path.join(app, 'RHINE MUSIC DATABASE.exe'));
    }

    run(process.execPath, ['scripts/package-resources.cjs', resources]);
    fs.copyFileSync('output/resources/qt-logic.js', path.join(resources, 'qt-logic.js'));
    fs.copyFileSync('LICENSE', path.join(resources, 'LICENSE'));
    fs.copyFileSync('vendor/qrcodegen/qrcodegen.hpp', path.join(resources, 'qrcodegen-LICENSE.hpp'));
    fs.cpSync('licenses/Qt', path.join(resources, 'Qt-Licenses'), {
        recursive: true,
    });
    const qtLicenses = path.resolve(qt, '../../Licenses');
    if (fs.existsSync(qtLicenses)) {
        fs.cpSync(qtLicenses, path.join(resources, 'Qt-Licenses'), {
            recursive: true,
        });
    }
    if (mac) {
        fs.mkdirSync('output/qt-icon', {
            recursive: true,
        });
        run(path.join(app, 'Contents/MacOS/RhineMusicDatabase'), ['--make-icon', 'output/qt-icon'], {
            QT_QPA_PLATFORM: 'offscreen',
        });
        run(python, ['scripts/package-icon.py', 'output/qt-icon', path.join(resources, 'AppIcon.icns')]);
        for (const plugin of [
            'platforms/libqcocoa.dylib',
            'platforms/libqoffscreen.dylib',
            'imageformats/libqjpeg.dylib',
            'imageformats/libqgif.dylib',
            'imageformats/libqico.dylib',
            'tls/libqsecuretransportbackend.dylib',
            'multimedia/libdarwinmediaplugin.dylib',
        ]) {
            const dest = path.join(app, 'Contents/PlugIns', plugin);
            fs.mkdirSync(path.dirname(dest), {
                recursive: true,
            });
            fs.copyFileSync(path.join(qt, 'plugins', plugin), dest);
        }
        if (config === 'Debug') {
            for (const plugin of ['local', 'server', 'preview', 'quickprofiler', 'quickeventreplay']) {
                const dest = path.join(app, 'Contents/PlugIns/qmltooling', `libqmldbg_${plugin}.dylib`);
                fs.mkdirSync(path.dirname(dest), {
                    recursive: true,
                });
                fs.copyFileSync(path.join(qt, 'plugins/qmltooling', path.basename(dest)), dest);
            }
        }
        run(path.join(qt, 'bin/macdeployqt'), [
            app,
            '-qmldir=' + path.join(root, 'Qt/qml'),
            '-always-overwrite',
            '-no-plugins',
        ]);
        fs.writeFileSync(
            path.join(resources, 'qt.conf'),
            '[Paths]\nPrefix=.\nPlugins=PlugIns\nQmlImports=Resources/qml\n',
        );
        fs.rmSync(path.join(app, 'Contents/PlugIns/sqldrivers'), {
            recursive: true,
            force: true,
        });
        run('codesign', ['--force', '--sign', '-', path.join(resources, 'MusicBridge/node')]);
        run('codesign', ['--force', '--deep', '--sign', '-', app]);
    } else {
        run(path.join(qt, 'bin/windeployqt.exe'), [
            '--' + config.toLowerCase(),
            '--qmldir',
            path.join(root, 'Qt/qml'),
            '--no-translations',
            '--no-system-d3d-compiler',
            path.join(app, 'RHINE MUSIC DATABASE.exe'),
        ]);
        // windeployqt 默认只带 Windows 平台插件；离线发布包测试也需要 offscreen 插件。
        const offscreen = config === 'Debug' ? 'qoffscreend.dll' : 'qoffscreen.dll';
        fs.mkdirSync(path.join(app, 'platforms'), {
            recursive: true,
        });
        fs.copyFileSync(
            path.join(qt, 'plugins/platforms', offscreen),
            path.join(app, 'platforms', offscreen),
        );
        fs.writeFileSync(path.join(app, 'qt.conf'), '[Paths]\nPlugins=.\nQmlImports=qml\n');
        fs.rmSync(path.join(app, 'sqldrivers'), {
            recursive: true,
            force: true,
        });
    }
    return app;
}
prepareResources();
if (process.argv[2] === '--cmake-package') {
    // Qt Creator 的链接产物原地打包，不再次启动 CMake 或覆盖其他构建目录。
    const [, , , bundle, qt, config] = process.argv;
    if (!mac || !bundle || !qt || !config) {
        throw new Error('CMake packaging requires a macOS bundle, Qt kit and build configuration');
    }
    console.log(deployApp(qt, config, path.resolve(bundle)));
} else {
    const { qt, config } = buildQt();
    console.log(deployApp(qt, config));
}
