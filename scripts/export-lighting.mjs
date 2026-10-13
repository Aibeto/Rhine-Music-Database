import fs from 'node:fs';
import { getDFGLUT } from '../node_modules/three/src/renderers/shaders/DFGLUTData.js';
const lut = getDFGLUT();
fs.writeFileSync('output/resources/dfg-lut.bin', Buffer.from(lut.image.data.buffer));
import { SSAOPass } from '../node_modules/three/examples/jsm/postprocessing/SSAOPass.js';
import { Scene, PerspectiveCamera } from '../node_modules/three/build/three.module.js';
const savedRandom = Math.random;
let seed = 0x5248494e;
Math.random = () => {
    seed = (Math.imul(1664525, seed) + 1013904223) >>> 0;
    return seed / 0xffffffff;
};
const ao = new SSAOPass(new Scene(), new PerspectiveCamera(), 1, 1, 32);
Math.random = savedRandom;
fs.writeFileSync(
    'output/resources/ao-kernel.bin',
    Buffer.from(new Float32Array(ao.kernel.flatMap((v) => [v.x, v.y, v.z, 0])).buffer),
);
fs.writeFileSync('output/resources/ao-noise.bin', Buffer.from(ao.noiseTexture.image.data.buffer));
ao.dispose();
