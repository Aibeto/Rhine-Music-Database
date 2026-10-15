import * as THREE from 'three';
import type { NativeSceneOutput } from '../Resources/platform';

/** 创建 Qt 原生着色器读取的主题状态，不再安装 WebGL 编译回调。 */
export function themeMaterial() {
    return { value: 0 };
}
type Baseline = {
    background: THREE.Color;
    fog?: THREE.Color;
    intensity: number;
    exposure: number;
    lights: {
        light: THREE.Light;
        intensity: number;
    }[];
    floor?: {
        material: THREE.MeshStandardMaterial;
        color: THREE.Color;
    };
};
const scenes = new WeakMap<THREE.Scene, Baseline>();
const background = new THREE.Color('#11181b');
const floorColor = new THREE.Color('#192125');
const mistColor = new THREE.Color('#263136');
export function themeEnvironment(scene: THREE.Scene, renderer: NativeSceneOutput, amount: number) {
    let baseline = scenes.get(scene);
    if (!baseline) {
        const lights: Baseline['lights'] = [];
        scene.traverse((object) => {
            if (object instanceof THREE.Light) {
                lights.push({
                    light: object,
                    intensity: object.intensity,
                });
            }
        });
        const floor = scene.getObjectByName('archive-floor') as THREE.Mesh | undefined;
        const material = floor?.material as THREE.MeshStandardMaterial | undefined;
        baseline = {
            background: (scene.background as THREE.Color).clone(),
            fog: scene.fog?.color.clone(),
            intensity: scene.environmentIntensity,
            exposure: renderer.toneMappingExposure,
            lights,
            floor: material
                ? {
                      material,
                      color: material.color.clone(),
                  }
                : undefined,
        };
        scenes.set(scene, baseline);
    }
    (scene.background as THREE.Color).copy(baseline.background).lerp(background, amount);
    if (scene.fog && baseline.fog) {
        scene.fog.color.copy(baseline.fog).lerp(mistColor, amount);
    }
    if (baseline.floor) {
        baseline.floor.material.color.copy(baseline.floor.color).lerp(floorColor, amount);
    }
    scene.environmentIntensity = THREE.MathUtils.lerp(baseline.intensity, 0.32, amount);
    renderer.toneMappingExposure = THREE.MathUtils.lerp(baseline.exposure, 0.98, amount);
    for (const { light, intensity } of baseline.lights) {
        light.intensity = intensity * (1 - 0.35 * amount);
    }
}
