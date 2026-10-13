import { build } from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const root = fs.realpathSync(process.cwd());
const result = await build({
    entryPoints: ['Qt/logic/app.js'],
    outfile: 'output/resources/qt-logic.js',
    bundle: true,
    format: 'iife',
    target: 'es2016',
    metafile: true,
});
for (const name of Object.keys(result.metafile.inputs)) {
    if (!name.startsWith('<')) {
        assert(fs.realpathSync(name).startsWith(root + path.sep), 'Input outside standalone project');
    }
}
