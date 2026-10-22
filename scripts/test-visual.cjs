const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));

// 冻结动画与场景基准对照的是特定采样，UI 或动画调参后需要重新录制。
// 这些检查不进入默认 npm test，只在需要确认视觉曲线未漂移时手动运行。
const executable = (name) =>
    path.join('output', 'qt-build', process.platform === 'win32' ? name + '.exe' : name);
const cases = [
    {
        name: '场景动画曲线',
        command: executable('RhineSceneMotionRegression'),
        args: [path.join('Qt', 'tests', 'fixtures', 'scene-motion.qz')],
    },
    {
        name: '界面动效曲线',
        command: executable('RhineUiMotionRegression'),
        args: [path.join('Qt', 'tests', 'fixtures', 'ui-motion.qz')],
    },
    {
        name: '原生场景帧',
        command: executable('RhineNativeSceneRegression'),
        args: [path.join('output', 'resources'), path.join('Qt', 'tests', 'fixtures', 'scene-frame.qz')],
    },
];

for (const item of cases) {
    if (!fs.existsSync(item.command)) {
        console.error(`缺少 ${item.command}，请先运行 npm run build。`);
        process.exit(1);
    }
}

for (const item of cases) {
    console.log(`运行 ${item.name}…`);
    const result = spawnSync(item.command, item.args, { stdio: 'inherit' });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        process.exit(result.status || 1);
    }
}

console.log('Frozen animation and scene snapshot regressions passed');
