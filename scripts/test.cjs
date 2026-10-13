const path = require('node:path');
const { spawnSync } = require('node:child_process');
process.chdir(path.resolve(__dirname, '..'));
const mac = process.platform === 'darwin';
const python = process.env.PYTHON || (mac ? 'python3' : 'python');
function run(command, args, env = {}) {
    const result = spawnSync(command, args, {
        stdio: 'inherit',
        env: {
            ...process.env,
            ...env,
        },
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        process.exit(result.status || 1);
    }
}
run(process.execPath, ['scripts/check-repository.cjs']);
run(python, ['scripts/check-qt-project.py']);
for (const script of [
    'scripts/check-music-upstream.cjs',
    'scripts/check-runtime.mjs',
    'Qt/tests/api-contract.mjs',
    'Qt/tests/music-state.mjs',
    'Qt/tests/hud-layout.mjs',
    'Qt/tests/list-navigation.mjs',
    'Qt/tests/search-groups.mjs',
    'Qt/tests/player-state.mjs',
    'Qt/tests/discover-state.mjs',
    'Qt/tests/account-actions.mjs',
    'Qt/tests/qq-state.mjs',
    'Qt/tests/performance.mjs',
]) {
    run(process.execPath, [script]);
}
run('ctest', ['--test-dir', 'output/qt-build', '--output-on-failure']);
const app = mac ? 'output/RHINE MUSIC DATABASE.app' : 'output/windows/RHINE MUSIC DATABASE';
const resources = mac ? app + '/Contents/Resources' : app + '/resources';
run(process.execPath, ['scripts/check-packaged-bridge.cjs', resources + '/MusicBridge']);
if (mac) {
    run('codesign', ['--verify', '--deep', '--strict', app]);
}
run(python, ['scripts/audit-qt.py', app]);
run(process.execPath, ['scripts/check-packaged-app.cjs', app]);
console.log('Cross-platform offline regressions and application package audit passed');
