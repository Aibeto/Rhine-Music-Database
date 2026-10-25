#!/usr/bin/env node
// 帧生成（FG）的真实冒烟测试。
//
// 为什么单独一个脚本、不并进 `npm test`：FG 需要"真的能建 Vulkan 交换链"的机器——
// 有 GPU、有驱动、有一个能 present 的显示。CI 的 runner 三者都没有，把它塞进
// 默认测试只会让每一次构建都红。所以这里默认"跑不了就明确跳过并说明原因"，
// 只有在显式传 --require 时才把"跳不过"当作失败（给本地/专用 GPU 机用）。
//
// 断言的东西就是 FG 整条链路的关键节点，缺一不可：
//   1. 呈现层接管窗口（自建 VkSwapchainKHR）；
//   2. 交换链真的建起来了（图像数量 + 格式）；
//   3. FFX 帧生成上下文建起来了（光流 + 插值）；
//   4. 稳定跑一段之后确实产出了插值帧，而不是每帧都退回"重投影当前帧"；
//   5. 呈现间隔统计在推进——性能面板的帧生成字段读的就是它，它不更新面板就是空的。
const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

process.chdir(path.resolve(__dirname, '..'));

// 冒烟测试跑的是 CMake 直接产出的可执行文件（不是打包后的 AppDir）：
// 只有它才带完整调试信息，也省掉一次打包。
const executable = path.join(
    'output',
    'qt-build',
    process.platform === 'win32' ? 'RhineMusicDatabase.exe' : 'RhineMusicDatabase',
);
const requireGpu = process.argv.includes('--require');
const secondsArg = process.argv.find((arg) => arg.startsWith('--seconds='));
const runSeconds = secondsArg ? Number(secondsArg.split('=')[1]) : 25;

function skip(reason) {
    if (requireGpu) {
        console.error(`FG 冒烟测试：${reason}（--require 要求必须真跑）`);
        process.exit(1);
    }
    console.log(`FG 冒烟测试：跳过——${reason}`);
    process.exit(0);
}

if (process.env.RHINE_FG_SMOKE === 'off') {
    skip('RHINE_FG_SMOKE=off');
}
if (process.platform !== 'linux' && process.platform !== 'win32' && process.platform !== 'darwin') {
    skip(`不支持的平台 ${process.platform}`);
}
if (!fs.existsSync(executable)) {
    skip(`还没构建：找不到 ${executable}`);
}
// 没有显示服务就没人 present，交换链根本建立不起来。
if (process.platform === 'linux' && !process.env.WAYLAND_DISPLAY && !process.env.DISPLAY) {
    skip('没有 WAYLAND_DISPLAY / DISPLAY');
}
// 驱动层面先探一次：vulkaninfo 不存在或枚举不到设备就直接跳过，
// 免得把"这台机器没有 Vulkan 驱动"误报成 FG 的缺陷。
const vulkanInfo = spawnSync('vulkaninfo', ['--summary'], { encoding: 'utf8' });
if (vulkanInfo.error || vulkanInfo.status !== 0 || !/deviceName/i.test(vulkanInfo.stdout || '')) {
    skip('没有可用的 Vulkan 设备');
}

// 这里刻意不隔离配置目录：3D 场景要有曲库才渲染得起来，空配置下场景不重绘、
// 帧输入不投递，FG 上下文根本不会被创建——那测的就不是 FG 了。
// 所以脚本只注入 RHINE_FG=on，其余沿用当前用户的配置与曲库。
//
// 注意 RHINE_FG 现在只决定"要不要插补偿帧"，不再决定"要不要建呈现层"：
// 呈现层在能力可用时启动即接管（这样设置里的开关才能即时生效）。这里仍然显式
// 注入 on，是为了不受当前用户偏好影响——否则偏好是 off 的机器上永远测不到插值。
const env = {
    ...process.env,
    QT_FORCE_STDERR_LOGGING: '1',
    RHINE_FG: 'on',
};

const child = spawn(executable, [], { env, stdio: ['ignore', 'pipe', 'pipe'] });
let output = '';
// 判定成功用"抢跑"：只要在输出里看到一帧被真正插值出来就立刻收工，
// 不用等固定时长——固定时长在不同机器上不是太短（没跨到统计点）就是太慢。
const SUCCESS = /FG stats: presented (\d+) interpolated ([1-9]\d*)/;
let settle;
const settled = new Promise((resolve) => {
    settle = resolve;
});
const collect = (chunk) => {
    output += chunk.toString();
    const hit = output.match(SUCCESS);
    if (hit) {
        settle({ proven: true, presented: Number(hit[1]), interpolated: Number(hit[2]) });
    }
};
child.stdout.on('data', collect);
child.stderr.on('data', collect);
child.on('exit', (code, signal) => settle({ code, signal }));

const timeout = new Promise((resolve) =>
    setTimeout(() => resolve({ timedOut: true }), runSeconds * 1000),
);

function fail(message) {
    if (!child.killed) {
        child.kill('SIGKILL');
    }
    console.error(`FG 冒烟测试：失败——${message}`);
    const tail = output.split('\n').filter((line) => /FG|FgHost|error|Error/.test(line)).slice(-12);
    if (tail.length) {
        console.error('相关输出：\n' + tail.join('\n'));
    }
    process.exit(1);
}

(async () => {
    const result = await Promise.race([settled, timeout]);
    // 超时不算失败：判定靠下面的断言，超时只是"没抢跑到成功"。
    if (!result.timedOut && result.code !== undefined && result.code !== 0) {
        fail(`进程提前退出（code=${result.code} signal=${result.signal}）`);
    }
    if (!child.killed) {
        child.kill('SIGTERM');
    }

    // 呈现层与交换链必须先成立，否则后面的插值统计没有意义。
    for (const [pattern, message] of [
        [/FG: 呈现层已启动/, '呈现层没有接管窗口'],
        [/张交换链图像/, '交换链没有建立'],
        [/帧生成上下文/, 'FFX 帧生成上下文没有建立'],
    ]) {
        if (!pattern.test(output)) {
            fail(message);
        }
    }
    if (result.proven) {
        const ratio = (result.presented / result.interpolated).toFixed(1);
        console.log(
            `FG 冒烟测试：通过——presented ${result.presented}，interpolated ${result.interpolated}（约 1:${ratio}）`,
        );
        process.exit(0);
    }
    // 没抢跑到成功：把"为什么"说清楚，别只报一个数字。
    if (!/FG stats:/.test(output)) {
        fail(`${runSeconds}s 内没有输出帧统计（可用 --seconds=N 放长）`);
    }
    const lastStats = [...output.matchAll(/FG stats:[^\n]*/g)].pop()?.[0] ?? '';
    // 呈现间隔统计是性能面板里"呈现/渲染 FPS""帧间隔 P50/P95"的唯一来源，
    // 它不出数面板就是一片 0，所以这里把它当成冒烟条件之一。
    if (!/gapP50 \d/.test(lastStats)) {
        fail(`呈现间隔统计没有出数：${lastStats}`);
    }
    if (/interpolated 0 /.test(lastStats)) {
        fail(`插值一帧都没产出：${lastStats}`);
    }
    // 深度与运动矢量是 FG 的输入：缺其中一张，SDK 只能退化成纯光流，
    // 画面看着还在动，画质却已经塌了，日志之外没有别的证据。
    if (lastStats.includes('depth')
        && (!lastStats.includes('depth ok') || !lastStats.includes('mv ok'))) {
        fail(`深度或运动矢量没有就绪：${lastStats}`);
    }
    fail(`没有观察到插值产出：${lastStats || '无统计行'}`);
})();
