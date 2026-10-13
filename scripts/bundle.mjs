import { build } from 'esbuild';
import path from 'node:path';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const root = fs.realpathSync(process.cwd());
const platform = path.resolve('Resources/platform.ts');
const plugin = {
    name: 'native-state-only',
    setup(b) {
        b.onResolve(
            {
                filter: /^three$|three\/addons\/(postprocessing\/|loaders\/GLTFLoader)/,
            },
            () => ({
                path: platform,
            }),
        );
        b.onResolve(
            {
                filter: /shared-depth$|archive-lighting$|quality-renderer$/,
            },
            () => ({
                path: platform,
            }),
        );
    },
};
const inputs = new Set();
for (const file of ['timeline', 'runtime']) {
    const result = await build({
        entryPoints: [`Resources/${file}.ts`],
        outfile: `output/resources/${file}.js`,
        bundle: true,
        format: 'iife',
        target: 'es2016',
        plugins: file === 'runtime' ? [plugin] : [],
        define: {
            'import.meta.env': '{"PROD":false,"BASE_URL":"/"}',
        },
        metafile: true,
        logLevel: 'warning',
    });
    for (const name of Object.keys(result.metafile.inputs)) {
        if (name.startsWith('<')) {
            continue;
        }
        const absolute = fs.realpathSync(path.resolve(name));
        assert(absolute.startsWith(root + path.sep), `Build input outside independent project: ${name}`);
        inputs.add(path.relative(root, absolute));
    }
}
fs.writeFileSync('output/scene-build-inputs.json', JSON.stringify([...inputs].sort(), null, 2) + '\n');
console.log('Scene dependency audit passed: every input is inside this project');
