// Node 与 Qt 执行同一固定时间线；比较相机、实例与拾取结果，不访问网络。
import fs from 'node:fs';
import vm from 'node:vm';
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
await ctx.RhineNative.review('archive');
const frame = ctx.RhineNative.frameFast(50);
for (const d of frame.draws) {
    d.matrices = Array.from(ctx.RhineNative.buffer(d.matrixBuffer)).slice(0, d.count * 16);
    d.themes = d.themeBuffer < 0 ? [] : Array.from(ctx.RhineNative.buffer(d.themeBuffer)).slice(0, d.count);
}
const rays = [
    [180, 180],
    [700, 500],
    [1100, 700],
    [1700, 900],
];
fs.writeFileSync(
    root + 'scene-fixture.json',
    JSON.stringify({
        frame,
        rays: rays.map(([x, y]) => ({
            x,
            y,
            hit: ctx.RhineNative.pick(x, y),
        })),
    }),
);
console.log('Frozen scene fixture generated for Qt engine regression');
