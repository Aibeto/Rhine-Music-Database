import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const root = 'output/resources/';
const ctx = vm.createContext({
    console,
    nativeGeometry: {
        cassette: JSON.parse(fs.readFileSync(root + 'archive-cassette-geometry.json')),
        assembly: JSON.parse(fs.readFileSync(root + 'archive-assembly-geometry.json')),
    },
});
vm.runInContext(fs.readFileSync(root + 'runtime.js', 'utf8'), ctx);
await ctx.RhineNative.init();
ctx.RhineNative.setMode('archive');
for (let i = 1; i < 121; i++) {
    ctx.RhineNative.frame(i / 60);
}
const f = ctx.RhineNative.frame(2.1);
assert(f.draws.length > 16);
assert.equal(f.selected, 0);
assert(f.stats.extraction > 0.39);
console.log('Native state:', f.stats.cameraPosition, f.stats.modelPosition, f.draws.length);
ctx.RhineNative.setMode('detail');
for (let i = 127; i < 550; i++) {
    ctx.RhineNative.frame(i / 60);
}
let d = ctx.RhineNative.frame(10);
assert(d.stats.extraction > 4);
assert(d.stats.decryption.clarity > 0.99);
ctx.RhineNative.navigate('row', 1);
for (let i = 601; i < 800; i++) {
    ctx.RhineNative.frame(i / 60);
}
let n = ctx.RhineNative.frame(14);
assert.equal(n.selected, 5);
console.log('Original lift, decryption and navigation passed');
// 切歌使用原选中流程，插回中的唱片保留旧封面身份，新唱片完成取出。
ctx.RhineNative.musicIdentity('song:old');
ctx.RhineNative.select(10);
ctx.RhineNative.musicIdentity('song:new');
const swapping = ctx.RhineNative.frame(14.01);
assert(swapping.stats.returningFiles > 0);
assert(swapping.draws.some((draw) => draw.musicKey === 'song:old' && !draw.musicSelected));
assert(swapping.draws.some((draw) => draw.musicKey === 'song:new' && draw.musicSelected));
for (let i = 842; i < 1080; i++) {
    ctx.RhineNative.frame(i / 60);
}
const synchronized = ctx.RhineNative.frame(18);
assert.equal(synchronized.selected, 10);
assert(synchronized.stats.extraction > 4);
console.log(
    'Song identity preserves outgoing cover and uses original return, reposition and extraction motion',
);
ctx.RhineNative.openViewer();
await new Promise((resolve) => setTimeout(resolve, 0));
for (let i = 1081; i < 1140; i++) {
    ctx.RhineNative.frame(i / 60);
}
const viewer = ctx.RhineNative.frame(19);
assert(viewer.viewer);
assert(viewer.draws.some((d) => d.asset === 'assembly'));
assert.equal(viewer.near, 0.3);
ctx.RhineNative.viewerAction('explode');
for (let i = 1141; i < 1260; i++) {
    ctx.RhineNative.frame(i / 60);
}
assert.equal(ctx.RhineNative.frame(21).viewerState.exploded, 'true');
ctx.RhineNative.pointer('pointerdown', 500, 400);
ctx.RhineNative.pointer('pointermove', 550, 420);
ctx.RhineNative.pointer('pointerup', 550, 420);
ctx.RhineNative.viewerAction('close');
for (let i = 1261; i < 1380; i++) {
    ctx.RhineNative.frame(i / 60);
}
await new Promise((resolve) => setTimeout(resolve, 0));
assert.equal(ctx.RhineNative.frame(24).viewer, false);
console.log('Assembly, explode, pointer rotation and close passed');
await ctx.RhineNative.review('archive');
const frozen = ctx.RhineNative.frame(30);
const later = ctx.RhineNative.frame(40);
assert.deepEqual(later.stats.modelPosition, frozen.stats.modelPosition);
assert.deepEqual(later.view, frozen.view);
console.log('Comparison frame remains physically frozen');

// 验证原有射线拾取和事件处理，包含惰性祖先元素的防护。
ctx.RhineNative.setMode('archive');
for (let i = 2401; i < 2520; i++) {
    ctx.RhineNative.frame(i / 60);
}
const selectedBefore = ctx.RhineNative.frame(42).stats.selectedCell;
let hit;
for (let y = 180; y < 780 && !hit; y += 60) {
    for (let x = 180; x < 1400 && !hit; x += 80) {
        ctx.RhineNative.pointer('pointermove', x, y);
        const f = ctx.RhineNative.frame(42.01);
        const cell = f.stats.hoverCell;
        if (cell && (cell.lane !== selectedBefore.lane || cell.row !== selectedBefore.row)) {
            hit = {
                x,
                y,
                cell,
            };
        }
    }
}
assert(hit, 'Mouse hover must reach the actual original raycaster');
for (let i = 1; i <= 20; i++) {
    ctx.RhineNative.frame(42.01 + i / 60);
}
assert(Object.keys(ctx.RhineNative.frame(42.4).stats.hoverLifts).length > 0, 'Hovered archive must lift');
ctx.RhineNative.pointer('pointerdown', hit.x, hit.y);
ctx.RhineNative.pointer('pointerup', hit.x, hit.y);
assert.deepEqual(
    ctx.RhineNative.frame(42.5).stats.selectedCell,
    hit.cell,
    'Click selects the hovered physical archive',
);
ctx.RhineNative.pointer('pointerleave', 0, 0);
assert.equal(ctx.RhineNative.frame(42.6).stats.hoverCell, null);
ctx.RhineNative.pointer('pointerdown', 700, 500);
ctx.RhineNative.frame(42.61);
ctx.RhineNative.pointer('pointermove', 850, 560);
assert(ctx.RhineNative.frame(42.63).stats.dragTrack, 'Drag continues through original plane mapping');
ctx.RhineNative.pointer('pointerup', 850, 560);
ctx.RhineNative.frame(42.65);
ctx.RhineNative.pointer('wheel', 700, 500, 120);
ctx.RhineNative.frame(42.7);
console.log('Mouse hover, lift, click selection, leave, drag and wheel passed', hit);

// 二进制桥必须保留原帧的全部浮点值和可见属性。
await ctx.RhineNative.review('archive');
const legacy = ctx.RhineNative.frame(50);
const binary = ctx.RhineNative.frameFast(50);
for (const draw of binary.draws) {
    draw.matrices = Array.from(ctx.RhineNative.buffer(draw.matrixBuffer)).slice(0, draw.count * 16);
    draw.themes =
        draw.themeBuffer < 0 ? [] : Array.from(ctx.RhineNative.buffer(draw.themeBuffer)).slice(0, draw.count);
    const original = legacy.draws.find((d) => d.id === draw.id);
    for (const key of ['matrixBuffer', 'themeBuffer']) {
        delete draw[key];
        delete original[key];
    }
    original.matrices = Array.from(new Float32Array(original.matrices));
    assert.deepEqual(JSON.parse(JSON.stringify(draw)), JSON.parse(JSON.stringify(original)));
}
assert.deepEqual(binary.view, legacy.view);
assert.deepEqual(binary.stats, legacy.stats);
console.log('Binary bridge preserves all render data; shared matrix buffers verified');

// BVH 必须与原三角形射线拾取命中同一个物理格。
await ctx.RhineNative.review('archive');
let checked = 0;
for (let y = 80; y < 1020; y += 47) {
    for (let x = 40; x < 1900; x += 47) {
        ctx.RhineNative.picking(false);
        const original = ctx.RhineNative.pick(x, y);
        ctx.RhineNative.picking(true);
        const accelerated = ctx.RhineNative.pick(x, y);
        assert.deepEqual(accelerated, original, `Picking mismatch at ${x},${y}`);
        checked++;
    }
}
console.log(`BVH and original picker agree for ${checked} screen rays`);
ctx.RhineNative.setMode('archive');
ctx.RhineNative.navigate('row', 1);
for (let i = 1; i <= 20; i++) {
    ctx.RhineNative.frame(50 + i / 60);
}
for (let y = 180; y < 840; y += 80) {
    for (let x = 160; x < 1500; x += 90) {
        ctx.RhineNative.picking(false);
        const original = ctx.RhineNative.pick(x, y);
        ctx.RhineNative.picking(true);
        assert.deepEqual(ctx.RhineNative.pick(x, y), original, `Moving picker mismatch at ${x},${y}`);
    }
}
console.log('BVH preserves picking during extraction and return motion');

// 合并触控板事件后方向可能超过一圈，负方向也必须保持合法物理索引。
const beforeMerged = ctx.RhineNative.selection();
ctx.RhineNative.navigate('row', -25);
assert(Number.isInteger(ctx.RhineNative.selection()));
ctx.RhineNative.navigate('row', 25);
assert.equal(ctx.RhineNative.selection(), beforeMerged);
console.log('Large merged positive and negative wheel navigation preserves physical selection');
