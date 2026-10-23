const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { findCompiler, stageApplication } = require('../../scripts/installer-windows.cjs');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rhine-installer-check-'));
try {
    const source = path.join(temporary, 'source');
    const destination = path.join(temporary, 'staged');
    for (const name of [
        'RHINE MUSIC DATABASE.exe',
        'data/account.dpapi',
        'cache/private.bin',
        'temp/private.bin',
        'resources/sdk/data/required.bin',
        'resources/sdk/cache/required.bin',
        'resources/MusicBridge/worker.cjs',
    ]) {
        const file = path.join(source, name);
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, name);
    }
    stageApplication(source, destination);
    for (const name of ['data', 'cache', 'temp']) {
        assert(!fs.existsSync(path.join(destination, name)), `Private ${name} must not enter the installer`);
    }
    for (const name of [
        'RHINE MUSIC DATABASE.exe',
        'resources/sdk/data/required.bin',
        'resources/sdk/cache/required.bin',
        'resources/MusicBridge/worker.cjs',
    ]) {
        assert.equal(fs.readFileSync(path.join(destination, name), 'utf8'), name);
    }
    assert(fs.existsSync(path.join(source, 'data/account.dpapi')), 'Source account must remain untouched');
    const compiler = path.join(temporary, 'Inno Setup 6/ISCC.exe');
    fs.mkdirSync(path.dirname(compiler));
    fs.writeFileSync(compiler, 'fixture');
    assert.equal(findCompiler({ ISCC: 'custom compiler.exe' }), 'custom compiler.exe');
    assert.equal(findCompiler({ 'ProgramFiles(x86)': temporary }), compiler);
    const compiler7 = path.join(temporary, 'Inno Setup 7/ISCC.exe');
    fs.mkdirSync(path.dirname(compiler7));
    fs.writeFileSync(compiler7, 'fixture');
    assert.equal(findCompiler({ ProgramFiles: temporary }), compiler7);
    const userRoot = path.join(temporary, 'user');
    const userCompiler = path.join(userRoot, 'Programs/Inno Setup 7/ISCC.exe');
    fs.mkdirSync(path.dirname(userCompiler), { recursive: true });
    fs.writeFileSync(userCompiler, 'fixture');
    assert.equal(findCompiler({ LOCALAPPDATA: userRoot }), userCompiler);
    assert.equal(findCompiler({}), 'ISCC.exe');
    console.log('Windows installer staging preserves runtime resources and excludes private root data');
} finally {
    fs.rmSync(temporary, { recursive: true, force: true });
}
