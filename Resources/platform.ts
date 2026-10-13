import * as T from '../node_modules/three/build/three.module.js';
export * from '../node_modules/three/build/three.module.js';
export class NativeElement {
    width = 1920;
    height = 1080;
    clientWidth = 1920;
    clientHeight = 1080;
    style: any = {
        setProperty(k: string, v: any) {
            this[k] = v;
        },
        removeProperty(k: string) {
            delete this[k];
        },
    };
    dataset: any = {
        layout: 'desktop',
    };
    listeners = new Map<string, Function[]>();
    parentElement: any = null;
    attributes = new Map<string, string>();
    children: any[] = [];
    selectors = new Map<string, NativeElement>();
    hidden = false;
    inert = false;
    disabled = false;
    tabIndex = 0;
    className = '';
    innerHTML = '';
    textContent = '';
    ownerDocument: any;
    classList = {
        add() {},
        remove() {},
        toggle() {},
    };
    constructor() {
        this.ownerDocument = (globalThis as any).document;
    }

    addEventListener(name: string, fn: Function) {
        this.listeners.set(name, [...(this.listeners.get(name) || []), fn]);
    }

    removeEventListener(name: string, fn: Function) {
        this.listeners.set(
            name,
            (this.listeners.get(name) || []).filter((f) => f !== fn),
        );
    }

    setAttribute(name: string, value: string) {
        this.attributes.set(name, String(value));
        if (name === 'inert') {
            this.inert = true;
        }
    }

    appendChild(child: any) {
        this.children.push(child);
        child.parentElement = this;
    }

    remove() {}
    setPointerCapture() {}
    releasePointerCapture() {}
    hasPointerCapture() {
        return true;
    }

    querySelector(s: string) {
        if (!this.selectors.has(s)) {
            const e = new NativeElement();
            e.parentElement = this;
            e.ownerDocument = (globalThis as any).document;
            this.selectors.set(s, e);
        }
        return this.selectors.get(s)!;
    }

    querySelectorAll(s: string) {
        return [...this.selectors.values()];
    }

    focus() {}
    getRootNode() {
        return (globalThis as any).document;
    }

    matches(selector: string): boolean {
        return selector.split(',').some((s) => {
            s = s.trim();
            if (s === '[inert]') {
                return this.inert;
            }
            if (s.startsWith('.')) {
                return this.className.split(/\s+/).includes(s.slice(1));
            }
            if (s.startsWith('#')) {
                return this.attributes.get('id') === s.slice(1);
            }
            const match = s.match(/^\[([^=\]]+)(?:=['"]?([^'"\]]+)['"]?)?\]$/);
            if (match) {
                const name = match[1];
                const value = name.startsWith('data-')
                    ? this.dataset[name.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase())]
                    : this.attributes.get(name);
                return value !== undefined && (match[2] === undefined || String(value) === match[2]);
            }
            return false;
        });
    }

    getClientRects() {
        return [this.getBoundingClientRect()];
    }

    animate(frames: any[], options: any) {
        let resolve: any;
        let reject: any;
        const a: any = {
            frames,
            options,
            start: (globalThis as any).performance.now(),
            target: this,
            finished: new Promise((yes, no) => {
                resolve = yes;
                reject = no;
            }),
            cancel() {
                a.cancelled = true;
                reject(new Error('cancelled'));
            },
            finish() {
                resolve();
            },
        };
        ((globalThis as any).nativeAnimations ??= []).push(a);
        return a;
    }

    getBoundingClientRect() {
        return {
            left: 0,
            top: 0,
            width: this.clientWidth,
            height: this.clientHeight,
        };
    }

    closest(selector: string) {
        let current: NativeElement | null = this;
        const visited = new Set<NativeElement>();
        while (current && !visited.has(current)) {
            visited.add(current);
            if (current.matches(selector)) {
                return current;
            }
            current = current.parentElement;
        }
        return null;
    }

    dispatch(name: string, event: any) {
        for (const fn of this.listeners.get(name) || []) {
            fn({
                ...event,
                preventDefault() {},
                stopPropagation() {},
            });
        }
    }
    commands: any[] = [];
    getContext() {
        const canvas = this;
        let state: any = {
            fillStyle: '#000',
            font: '12px MiSans',
        };
        return new Proxy(state, {
            get(t, k) {
                if (k in t) {
                    return t[k];
                }
                return (...a: any[]) => {
                    if (k === 'drawImage' && a[0]?.commands) {
                        canvas.commands = JSON.parse(JSON.stringify(a[0].commands));
                        return;
                    }
                    if (k === 'clearRect') {
                        canvas.commands = [];
                    }
                    if (k === 'fillRect' && a[0] === 0 && a[1] === 0 && a[2] === 1024) {
                        canvas.commands = [];
                    }
                    canvas.commands.push({
                        op: k,
                        args: a.map((x) =>
                            typeof x === 'object'
                                ? {
                                      image: true,
                                  }
                                : x,
                        ),
                        ...t,
                    });
                    if (k === 'measureText') {
                        return {
                            width: String(a[0]).length * 12,
                        };
                    }
                };
            },
            set(t, k, v) {
                t[k] = v;
                return true;
            },
        });
    }
}

export class WebGLRenderer {
    domElement = new NativeElement();
    toneMappingExposure = 1.05;
    shadowMap: any = {};
    transmissionResolutionScale = 1;
    capabilities = {
        maxTextureSize: 8192,
        getMaxAnisotropy: () => 16,
    };
    info = {
        autoReset: false,
        render: {
            calls: 0,
            triangles: 0,
        },
        memory: {
            geometries: 0,
            textures: 0,
        },
        reset() {},
    };
    ratio = 1;
    setPixelRatio(v: number) {
        this.ratio = v;
    }

    getPixelRatio() {
        return this.ratio;
    }

    setSize(w: number, h: number) {
        this.domElement.clientWidth = w;
        this.domElement.clientHeight = h;
    }

    getSize(v: any) {
        return v.set(this.domElement.clientWidth, this.domElement.clientHeight);
    }

    render() {}
    dispose() {}
    forceContextLoss() {}
}

export class EffectComposer {
    passes: any[] = [];
    constructor(...args: any[]) {}
    addPass(p: any) {
        this.passes.push(p);
    }

    insertPass(p: any, i: number) {
        this.passes.splice(i, 0, p);
    }

    removePass(p: any) {
        this.passes = this.passes.filter((x) => x !== p);
    }

    setSize() {}
    setPixelRatio() {}
    render() {}
    dispose() {}
}

export class NativePass {
    enabled = true;
    kernelRadius = 0.38;
    minDistance = 0.001;
    maxDistance = 0.09;
    uniforms: any = {
        focus: {
            value: 25,
        },
        aperture: {
            value: 0.0018,
        },
        maxblur: {
            value: 0.011,
        },
    };
    camera: any;
    projection: any;
    constructor(...args: any[]) {
        this.camera = args[1];
    }

    setSize() {
        if (this.camera?.projectionMatrix) {
            this.projection = this.camera.projectionMatrix.clone();
        }
    }

    setSharing() {}
    dispose() {}
}
export {
    NativePass as RenderPass,
    NativePass as OutputPass,
    NativePass as SMAAPass,
    NativePass as BokehPass,
    NativePass as SharedDepthAO,
    NativePass as SharedDepthBokeh,
};
export class GLTFLoader {
    async loadAsync(url: string) {
        const key = url.includes('assembly') ? 'assembly' : 'cassette';
        const group = new T.Group();
        for (const [i, part] of (globalThis as any).nativeGeometry[key].entries()) {
            const geo = new T.BufferGeometry();
            geo.setAttribute('position', new T.Float32BufferAttribute(part.positions, 3));
            geo.setAttribute('normal', new T.Float32BufferAttribute(part.normals, 3));
            geo.setIndex(part.indices);
            geo.computeBoundingBox();
            geo.computeBoundingSphere();
            geo.userData.nativeMesh = i;
            geo.userData.asset = key;
            const p = part.material.pbrMetallicRoughness || {};
            const m = new T.MeshPhysicalMaterial({
                color: new T.Color().fromArray(p.baseColorFactor || [1, 1, 1]),
                metalness: p.metallicFactor ?? 1,
                roughness: p.roughnessFactor ?? 1,
            });
            m.name = part.name;
            for (const [ext, val] of Object.entries(part.material.extensions || {})) {
                if (ext === 'KHR_materials_transmission') {
                    m.transmission = (val as any).transmissionFactor ?? 0;
                }
                if (ext === 'KHR_materials_ior') {
                    m.ior = (val as any).ior ?? 1.5;
                }
                if (ext === 'KHR_materials_volume') {
                    m.thickness = (val as any).thicknessFactor ?? 0;
                    m.attenuationDistance = (val as any).attenuationDistance ?? Infinity;
                    m.attenuationColor.fromArray((val as any).attenuationColor ?? [1, 1, 1]);
                }
                if (ext === 'KHR_materials_clearcoat') {
                    m.clearcoat = (val as any).clearcoatFactor ?? 0;
                    m.clearcoatRoughness = (val as any).clearcoatRoughnessFactor ?? 0;
                }
            }
            m.opacity = p.baseColorFactor?.[3] ?? 1;
            m.transparent = part.material.alphaMode === 'BLEND';
            m.side = part.material.doubleSided ? T.DoubleSide : T.FrontSide;
            const mesh = new T.Mesh(geo, m);
            mesh.userData.assemblyPart = part.assemblyPart;
            group.add(mesh);
        }
        return {
            scene: group,
        };
    }
}

export function createArchiveLighting(renderer: any, scene: any) {
    scene.environmentIntensity = 0.48;
    scene.add(new T.HemisphereLight('#fffaf5', '#b4a18c', 0.65));
    let key = new T.DirectionalLight('#fff7ed', 1.4);
    key.position.set(-6, 14, -5);
    let fill = new T.DirectionalLight('#ffffff', 0.6);
    fill.position.set(7, 8, -10);
    scene.add(key, fill);
    return key;
}

export function resizeQuality(renderer: any, composer: any, host: any, quality: any) {
    renderer.setSize(host.clientWidth, host.clientHeight);
    host.dataset.renderQuality = '{}';
    return {
        width: host.clientWidth,
        height: host.clientHeight,
        ratio: 1,
    };
}

export function applyTextureQuality() {}
export function createViewerPipeline() {
    return {
        composer: new EffectComposer(),
        smaa: new NativePass(),
    };
}
