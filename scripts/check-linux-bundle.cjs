#!/usr/bin/env node
/**
 * 检查 Linux AppDir 里随包的 FFmpeg 与视频编码库。
 *
 * 用途：确认随包的是自编 libavcodec（只含音频解码器），还是回退到了发行版库。
 * 发行版 libavcodec 通过 DT_NEEDED 硬链接 libx264/libx265/libaom/libvpx/libdav1d/
 * librav1e/libSvtAv1Enc，这七个库合计约 50 MB，且全部用不到。
 *
 * 用法：node scripts/check-linux-bundle.cjs [AppDir 路径]
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const app = path.resolve(process.argv[2] || 'output/RhineMusicDatabase.AppDir');
const libraries = path.join(app, 'usr/lib');
const selfBuilt = path.resolve('output/dependencies/ffmpeg-linux/install/lib');
const encoderPattern = /^lib(?:aom|x264|x265|vpx|dav1d|rav1e|SvtAv1Enc)\.so/;
const ffmpegPattern = /^lib(?:avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale)\.so\.\d+/;

if (!fs.existsSync(libraries)) {
    console.error(`找不到 ${libraries}，先构建或传入正确的 AppDir 路径`);
    process.exit(1);
}

function size(file) {
    try {
        return fs.statSync(file).size;
    } catch {
        return -1;
    }
}

function megabytes(bytes) {
    return `${(bytes / 1048576).toFixed(1)} MB`;
}

const entries = fs.readdirSync(libraries);
const ffmpeg = entries.filter((name) => ffmpegPattern.test(name)).sort();
const encoders = entries.filter((name) => encoderPattern.test(name)).sort();

console.log(`AppDir: ${app}`);
console.log(`自编 FFmpeg 目录: ${fs.existsSync(selfBuilt) ? selfBuilt : '(不存在，必然回退发行版库)'}\n`);

console.log('随包的 FFmpeg 库：');
let fallback = 0;
for (const name of ffmpeg) {
    const packaged = size(path.join(libraries, name));
    const reference = size(path.join(selfBuilt, name));
    const built = reference > 0 && reference === packaged;
    if (!built) {
        fallback += 1;
    }
    console.log(`  ${name.padEnd(24)} ${megabytes(packaged).padStart(9)}  ${built ? '自编' : '发行版'}`);
}
if (!ffmpeg.length) {
    console.log('  (未找到 FFmpeg 库，Qt 可能不使用 FFmpeg 后端)');
}

if (encoders.length) {
    const total = encoders.reduce((sum, name) => sum + size(path.join(libraries, name)), 0);
    console.log(`\n发现编码器库 ${encoders.length} 个，合计 ${megabytes(total)}：`);
    for (const name of encoders) {
        const dependents = entries.filter((candidate) => {
            if (candidate === name || !candidate.endsWith('.so')) {
                return false;
            }
            const result = spawnSync('readelf', ['-d', path.join(libraries, candidate)], {
                encoding: 'utf8',
            });
            return (result.stdout || '').includes(`[${name}]`);
        });
        console.log(`  ${name.padEnd(24)} 依赖方: ${dependents.join(', ') || '(无，可直接删除)'}`);
    }
} else {
    console.log('\n未发现视频编码器库。');
}

console.log('');
if (fallback) {
    console.log(`结论：${fallback} 个 FFmpeg 库来自发行版，编码器库因此被拖进包。`);
    console.log(
        '自编库未能匹配 Qt 需要的 SONAME，构建日志里应有 "Self-built FFmpeg does not provide" 告警。',
    );
    process.exitCode = 1;
} else if (ffmpeg.length) {
    console.log('结论：随包的是自编 FFmpeg，未引入视频编码器库。');
} else {
    console.log('结论：没有可判定的 FFmpeg 库。');
}
