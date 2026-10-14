import { preparePicking, setPicking } from './picking';
import { NativeElement, compactNativeGeometry, NATIVE_HIDDEN_SURFACES } from './platform';
import * as THREE from '../node_modules/three/build/three.module.js';
import { ModelViewer } from '../Scene/model-viewer';
import { ArchiveScene } from '../Scene/scene';
import { bootMotion } from '../Scene/boot-motion';
import { columnFiles, records, archiveColumns } from '../Scene/data';
const host = new NativeElement();
let clock = 0;
let mode = 'boot';
let selected = 0;
let bootStart = 0;
let freeze: number | null = null;
let sceneFreeze: number | null = null;
(globalThis as any).performance = {
    now: () => clock * 1000,
};
(globalThis as any).devicePixelRatio = 1;
(globalThis as any).innerWidth = 1920;
(globalThis as any).innerHeight = 1080;
(globalThis as any).document = {
    createElement: () => new NativeElement(),
    listeners: new Map(),
    addEventListener: NativeElement.prototype.addEventListener,
    removeEventListener: NativeElement.prototype.removeEventListener,
    dispatch: NativeElement.prototype.dispatch,
    hidden: false,
    activeElement: null,
    body: host,
};
(globalThis as any).window = {
    addEventListener() {},
    removeEventListener() {},
};
(globalThis as any).Image = class {
    src = '';
    async decode() {}
};
(globalThis as any).HTMLElement = NativeElement;
(globalThis as any).matchMedia = () => ({
    matches: false,
});
(globalThis as any).getComputedStyle = (e: any) => ({
    opacity: e.style.opacity ?? 1,
    transform: e.style.transform ?? 'none',
});
(globalThis as any).AbortController = class {
    signal = {};
    abort() {}
};
let scene = new ArchiveScene(host as any);
const viewer = new ModelViewer(
    host as any,
    () => {},
    () => {},
);
const memories = archiveColumns.map((_, i) => columnFiles(i)[0]);
let ready = false;
function select(index: number, navigation?: any) {
    selected = index;
    scene.select(index, navigation);
    const lane = archiveColumns.indexOf(records[index].category);
    memories[lane] = index;
}
scene.onSelect = (index, cell) =>
    select(
        index,
        cell
            ? {
                  cell,
              }
            : undefined,
    );
scene.onNavigate = (axis, dir) => navigate(axis, dir);
function navigate(axis: string, dir: number) {
    const lane = archiveColumns.indexOf(records[selected].category);
    const files = columnFiles(lane);
    if (axis === 'lane') {
        select(memories[(((lane + dir) % 5) + 5) % 5], {
            axis,
            direction: dir,
        });
    } else {
        select(files[(((files.indexOf(selected) + dir) % files.length) + files.length) % files.length], {
            axis,
            direction: dir,
        });
    }
}

function setMode(value: string) {
    sceneFreeze = null;
    mode = value;
    scene.setMode(value === 'boot' ? 'hidden' : (value as any));
    if (value === 'boot') {
        bootStart = clock - 1.76;
    }
}
async function init() {
    compactNativeGeometry();
    await scene.load();
    preparePicking(scene.scene);
    scene.resize();
    scene.select(0);
    ready = true;
    setMode('boot');
}
let frameBuffers: Float32Array[] = [];
const IDENTITY_MATRIX = new THREE.Matrix4();
const PACKET_STRIDE = 23;
const DEFINITION_FIELDS = [
    'musicKey',
    'musicSelected',
    'shadowOnly',
    'mesh',
    'asset',
    'id',
    'instanced',
    'label',
    'floor',
    'transparent',
    'depthWrite',
    'order',
    'side',
    'name',
    'castShadow',
] as const;
const drawDefinitions = new Map<number, Record<(typeof DEFINITION_FIELDS)[number], unknown>>();
let packetData = new Float64Array(0);
let packetLength = 0;
/**
 * 生成当前场景的可序列化帧，保持矩阵、实例索引和拾取数据一致。
 */
function frame(time: number, binary = false, diagnostics = true, packed = false, advance = true) {
    const buffers: Float32Array[] = [];
    const bufferIDs = new Map<any, number>();
    function bufferID(array: any) {
        if (!array.length) {
            return -1;
        }
        let id = bufferIDs.get(array);
        if (id === undefined) {
            id = buffers.length;
            bufferIDs.set(array, id);
            buffers.push(array instanceof Float32Array ? array : new Float32Array(array));
        }
        return id;
    }
    clock = time;
    if (!ready) {
        return null;
    }
    const t = freeze ?? clock - bootStart;
    const cinema =
        mode === 'boot'
            ? {
                  time: t,
                  reveal: ease((t - 22) / 0.4),
                  lift: ease((t - 26) / 1.8),
                  zoom: 0.55 * ease((t - 27.3) / 1.65) + 0.45 * ease((t - 29) / 5),
              }
            : undefined;
    if (mode === 'boot' && t >= 35) {
        setMode('detail');
    }
    if (advance && sceneFreeze === null && (!cinema || t >= 21.9)) {
        scene.update(time, cinema);
    }
    const nativeAnimations = (globalThis as any).nativeAnimations ?? [];
    for (const a of advance ? nativeAnimations : []) {
        if (
            !a.cancelled &&
            !a.done &&
            time * 1000 - a.start >= (a.options.duration ?? 0) + (a.options.delay ?? 0)
        ) {
            a.done = true;
            Object.assign(a.target.style, a.frames.at(-1));
            a.finish();
        }
    }
    // 完成和取消的动画不得持续持有查看器元素、回调与 Promise。
    (globalThis as any).nativeAnimations = nativeAnimations.filter((a: any) => !a.cancelled && !a.done);
    viewer.setTheme(scene.themeAmount);
    if (advance) {
        viewer.update(time);
    }
    const renderScene = viewer.isOpen ? (viewer as any).scene : scene.scene;
    const camera = viewer.isOpen ? (viewer as any).camera : scene.camera;
    // 资料库 update 已同步世界矩阵；只有查看器需要在这里同步，避免每帧重复遍历全部归位副本。
    if (viewer.isOpen) {
        renderScene.updateMatrixWorld(true);
    }
    camera.updateMatrixWorld(true);
    const draws: any[] = [];
    const definitions: any[] = [];
    const activeIDs = new Set<number>();
    packetLength = 0;
    renderScene.traverseVisible((o: any) => {
        if (!o.isMesh) {
            return;
        }
        let owner = o;
        while (owner.parent && owner.parent !== renderScene) {
            owner = owner.parent;
        }
        const g = o.geometry;
        const m = o.material;
        const surfaceName = o.userData.surface ?? m.name;
        if (NATIVE_HIDDEN_SURFACES.has(surfaceName)) {
            return;
        }
        let sourceMatrices = o.isInstancedMesh ? o.instanceMatrix.array : o.matrixWorld.elements;
        // 同一卡片的无局部变换部件共享父矩阵，一次跨引擎传输即可覆盖全部表面。
        // 查看器爆炸、独立部件变换与实例矩阵仍读取各自原始数据。
        if (binary && !o.isInstancedMesh && o.parent === owner && o.matrix.equals(IDENTITY_MATRIX)) {
            sourceMatrices = owner.matrixWorld.elements;
        }
        const sourceThemes =
            o.isInstancedMesh && g.attributes.archiveTheme ? g.attributes.archiveTheme.array : [];
        let matrices;
        if (binary) {
            matrices = [];
        } else if (o.isInstancedMesh) {
            matrices = Array.from(sourceMatrices.slice(0, o.count * 16));
        } else {
            matrices = sourceMatrices;
        }
        let definition = drawDefinitions.get(o.id);
        const musicKey = owner.userData.musicKey ?? null;
        const musicSelected = owner === (scene as any).model || viewer.isOpen;
        const shadowOnly = !!o.userData.shadowOnly;
        const meshIndex = g.userData.nativeMesh ?? -1;
        const asset = g.userData.asset ?? 'cassette';
        const instanced = !!o.isInstancedMesh;
        const label = g.userData.nativeMesh === undefined && o.name !== 'archive-floor';
        const floor = o.name === 'archive-floor';
        const definitionChanged =
            !definition ||
            definition.musicKey !== musicKey ||
            definition.musicSelected !== musicSelected ||
            definition.shadowOnly !== shadowOnly ||
            definition.mesh !== meshIndex ||
            definition.asset !== asset ||
            definition.instanced !== instanced ||
            definition.label !== label ||
            definition.floor !== floor ||
            definition.transparent !== m.transparent ||
            definition.depthWrite !== m.depthWrite ||
            definition.order !== o.renderOrder ||
            definition.side !== m.side ||
            definition.name !== surfaceName ||
            definition.castShadow !== o.castShadow;
        if (!packed || definitionChanged) {
            definition = {
                musicKey,
                musicSelected,
                shadowOnly,
                mesh: meshIndex,
                asset,
                id: o.id,
                instanced,
                label,
                floor,
                transparent: m.transparent,
                depthWrite: m.depthWrite,
                order: o.renderOrder,
                side: m.side,
                name: surfaceName,
                castShadow: o.castShadow,
            };
            if (packed) {
                drawDefinitions.set(o.id, definition);
                definitions.push(definition);
            }
        }
        const quality = o.userData.appearance?.value ?? (m.map ? 1 : 0);
        const clarity = o.userData.glassClarity?.value ?? 0;
        const subdued = o.userData.subduedIndex?.value ?? (scene as any).subduedIndex?.value ?? 0;
        const theme = o.userData.themeAmount?.value ?? scene.themeAmount;
        const count = o.isInstancedMesh ? o.count : 1;
        const matrixBuffer = binary ? bufferID(sourceMatrices) : -1;
        const themeBuffer = binary ? bufferID(sourceThemes) : -1;
        const attenuationDistance = Number.isFinite(m.attenuationDistance) ? m.attenuationDistance : 1e9;
        if (packed) {
            activeIDs.add(o.id);
            if (packetLength + PACKET_STRIDE > packetData.length) {
                const expanded = new Float64Array(Math.max(PACKET_STRIDE * 64, packetData.length * 2));
                expanded.set(packetData);
                packetData = expanded;
            }
            // 静态定义只在变化时传 JSON；动态材质和缓冲索引使用双精度字节包，保持原数值。
            packetData.set(
                [
                    o.id,
                    count,
                    matrixBuffer,
                    themeBuffer,
                    m.color.r,
                    m.color.g,
                    m.color.b,
                    m.roughness ?? 1,
                    m.metalness ?? 0,
                    m.transmission ?? 0,
                    m.thickness ?? 0,
                    m.ior ?? 1.5,
                    m.attenuationColor?.r ?? 1,
                    m.attenuationColor?.g ?? 1,
                    m.attenuationColor?.b ?? 1,
                    attenuationDistance,
                    m.opacity,
                    m.clearcoat ?? 0,
                    m.clearcoatRoughness ?? 0,
                    quality,
                    clarity,
                    subdued,
                    theme,
                ],
                packetLength,
            );
            packetLength += PACKET_STRIDE;
        } else {
            draws.push({
                ...definition,
                themes: binary ? [] : Array.from(sourceThemes.slice(0, o.count)),
                matrixBuffer,
                themeBuffer,
                matrices,
                count,
                color: m.color.toArray(),
                roughness: m.roughness ?? 1,
                metalness: m.metalness ?? 0,
                transmission: m.transmission ?? 0,
                thickness: m.thickness ?? 0,
                ior: m.ior ?? 1.5,
                attenuation: m.attenuationColor?.toArray() ?? [1, 1, 1],
                attenuationDistance,
                opacity: m.opacity,
                clearcoat: m.clearcoat ?? 0,
                clearcoatRoughness: m.clearcoatRoughness ?? 0,
                quality,
                clarity,
                subdued,
                theme,
            });
        }
    });
    if (packed) {
        // 先保存键列表，再删除失效定义，避免迭代器在删除过程中跳过复用对象。
        for (const id of Array.from(drawDefinitions.keys())) {
            if (!activeIDs.has(id)) {
                drawDefinitions.delete(id);
            }
        }
    }
    for (const o of (scene as any).instances) {
        o.instanceMatrix.clearUpdateRanges();
        o.geometry.attributes.archiveTheme?.clearUpdateRanges();
    }
    frameBuffers = buffers;
    return {
        pendingAnimations: ((globalThis as any).nativeAnimations ?? []).some(
            (a: any) => !a.cancelled && !a.done,
        ),
        pointerCursor: scene.renderer.domElement.style.cursor ?? 'default',
        aoProjection: ((scene as any).ao.projection ?? camera.projectionMatrix).elements,
        reference: sceneFreeze !== null,
        near: camera.near,
        far: camera.far,
        viewer: viewer.isOpen,
        viewerState: viewer.root.dataset,
        mode,
        selected,
        time: t,
        draws: packed ? undefined : draws,
        drawDefinitions: packed ? definitions : undefined,
        packetVersion: packed ? 1 : undefined,
        packetCount: packed ? packetLength / PACKET_STRIDE : undefined,
        view: camera.matrixWorldInverse.elements,
        projection: camera.projectionMatrix.elements,
        eye: camera.position.toArray(),
        background: (renderScene.background as THREE.Color).toArray(),
        fogColor: renderScene.fog!.color.toArray(),
        fogNear: (renderScene.fog as THREE.Fog).near,
        fogFar: (renderScene.fog as THREE.Fog).far,
        detail: scene.detailVisibility,
        stats: diagnostics ? scene.getStats() : undefined,
        boot: bootMotion(t),
        labels: diagnostics ? (scene as any).labelCanvas.commands : undefined,
        focus: (scene as any).bokeh.uniforms.focus.value,
        aperture: (scene as any).bokeh.uniforms.aperture.value,
    };
}

function ease(v: number) {
    v = Math.min(1, Math.max(0, v));
    return v * v * (3 - 2 * v);
}
(globalThis as any).RhineNative = {
    init,
    frame,
    selection: () => selected,
    musicIdentity: (key: string) => {
        (scene as any).model.userData.musicKey = key;
    },
    musicClear: () => {
        for (const root of [scene.scene, (viewer as any).scene]) {
            root.traverse((object: any) => {
                delete object.userData.musicKey;
            });
        }
    },
    picking: setPicking,
    pick: (x: number, y: number) => (scene as any).pickCell(x, y),
    frameFast: (time: number, diagnostics = true) => frame(time, true, diagnostics),
    framePacked: (time: number, advance = true) => frame(time, true, false, true, advance),
    packetBytes: () => packetData.buffer.slice(0, packetLength * 8),
    bufferBytes: (index: number, count: number) => {
        const v = frameBuffers[index];
        return v
            ? v.buffer.slice(v.byteOffset, v.byteOffset + Math.min(count, v.length) * 4)
            : new ArrayBuffer(0);
    },
    energyIdle: (value: boolean) => {
        scene.energyIdle = value;
    },
    trimMemory: () => {
        scene.trimMemory(!viewer.isOpen);
        if (!viewer.isOpen) {
            delete (globalThis as any).nativeGeometry.assembly;
        }
        (globalThis as any).nativeAnimations = ((globalThis as any).nativeAnimations ?? []).filter(
            (animation: any) => !animation.cancelled && !animation.done,
        );
    },
    buffer: (index: number) => frameBuffers[index],
    setMode,
    navigate,
    select,
    async review(value: string) {
        ready = false;
        scene.dispose();
        scene = new ArchiveScene(host as any);
        scene.onSelect = (i, cell) =>
            select(
                i,
                cell
                    ? {
                          cell,
                      }
                    : undefined,
            );
        scene.onNavigate = (axis, dir) => navigate(axis, dir);
        await scene.load();
        preparePicking(scene.scene);
        scene.resize();
        selected = 0;
        scene.select(0);
        setMode(value);
        for (let i = 1; i <= 120; i++) {
            scene.update(i / 60);
        }
        scene.update(2.1);
        scene.update(2.101);
        sceneFreeze = 2.101;
        ready = true;
    },
    openViewer() {
        const r = records[selected];
        viewer.open(r.id, r.title, () => scene.createAssemblyModel(), false);
    },
    coverFront() {
        const v = viewer as any;
        v.interruptReset();
        v.controlCamera.position.set(0, 0, v.initialCamera.length());
        v.controls.target.set(0, 0, 0);
        v.controls.update();
        v.cameraMotion.snap(v.controlCamera, v.controls.target);
    },
    coverBack() {
        const v = viewer as any;
        v.interruptReset();
        v.controlCamera.position.set(0, 0, -v.initialCamera.length());
        v.controls.target.set(0, 0, 0);
        v.controls.update();
        v.cameraMotion.snap(v.controlCamera, v.controls.target);
    },
    viewerAction(action: string) {
        viewer.root.dispatch('click', {
            target: {
                closest: () => ({
                    dataset: {
                        viewer: action,
                    },
                }),
            },
        });
    },
    viewerKey(key: string) {
        viewer.root.dispatch('keydown', {
            key,
            target: viewer.root,
        });
    },
    theme: (v: boolean) => scene.setTheme(v),
    reduced: (v: boolean) => scene.setReduced(v),
    resize(w: number, h: number) {
        host.clientWidth = w;
        host.clientHeight = h;
        if (w / h < 1.05) {
            host.dataset.layout = 'portrait';
        } else if (w < 1100) {
            host.dataset.layout = 'compact';
        } else {
            host.dataset.layout = 'desktop';
        }
        scene.resize();
        (viewer as any).canvasHost.clientWidth = w;
        (viewer as any).canvasHost.clientHeight = h;
        viewer.resize();
    },
    freeze(t: number | null) {
        freeze = t;
    },
    pointer(type: string, x: number, y: number, delta = 0) {
        const target = viewer.isOpen ? (viewer as any).renderer.domElement : scene.renderer.domElement;
        const event = {
            clientX: x,
            clientY: y,
            pointerId: 1,
            pointerType: 'mouse',
            button: 0,
            timeStamp: clock * 1000,
            deltaX: 0,
            deltaY: delta,
            deltaMode: 0,
            target,
            pageX: x,
            pageY: y,
            ctrlKey: false,
            metaKey: false,
            shiftKey: false,
        };
        target.dispatch(type, event);
        if (viewer.isOpen && (type === 'pointermove' || type === 'pointerup')) {
            (globalThis as any).document.dispatch(type, event);
        }
    },
    get ready() {
        return ready;
    },
};
