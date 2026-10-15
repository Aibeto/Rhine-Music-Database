import * as THREE from 'three';
import { themeMaterial } from './theme-material';
type Surface = THREE.MeshPhysicalMaterial;
type Palette = {
    high: Surface;
    low?: Surface;
};

// 阵列和选中档案共用几何体，在同一个网格上渐变表面属性，
// 避免品质切换时透明外壳重叠。
export class CardAppearance {
    private palettes = new Map<string, Palette>();
    disposeSources() {
        for (const palette of this.palettes.values()) {
            palette.high.dispose();
            palette.low?.dispose();
        }
        this.palettes.clear();
    }

    register(name: string, high: Surface, low?: Surface) {
        this.palettes.set(name, {
            high,
            low,
        });
    }

    prepare(group: THREE.Group) {
        for (const child of group.children) {
            const mesh = child as THREE.Mesh;
            // 原生隐藏部件只承担拾取，不为每张归位卡片复制材质和着色器闭包。
            if (mesh.userData.nativeHidden) {
                continue;
            }
            const name = mesh.userData.surface as string;
            const palette = this.palettes.get(name);
            if (!palette) {
                mesh.userData.themeAmount = themeMaterial();
                continue;
            }
            const mat = palette.high.clone();
            const amount = {
                value: 0,
            };
            const clarity = {
                value: 0,
            };
            mesh.material = mat;
            if (mat.userData.opticalOrder) {
                mesh.renderOrder = mat.userData.opticalOrder;
            }
            mesh.userData.appearance = amount;
            mesh.userData.glassClarity = clarity;
            // 表面覆盖、解密和主题由 Qt 原生着色器读取这些状态。
            mesh.userData.subduedIndex = {
                value: 0,
            };
            mesh.userData.themeAmount = themeMaterial();
        }
    }

    setClarity(group: THREE.Group, value: number) {
        const clarity = THREE.MathUtils.clamp(value, 0, 1);
        // 查看器将网格移入零件分组后，遍历仍需找到原有部件。
        group.traverse((child) => {
            if (
                !(child instanceof THREE.Mesh) ||
                child.userData.nativeHidden ||
                !child.userData.glassClarity
            ) {
                return;
            }
            child.userData.glassClarity.value = clarity;
            if (child.userData.surface !== 'Frosted_Polymer') {
                return;
            }
            const mat = child.material as Surface;
            const palette = this.palettes.get('Frosted_Polymer')!;
            const quality = child.userData.appearance.value as number;
            const baseline = (key: 'thickness' | 'transmission' | 'attenuationDistance') =>
                THREE.MathUtils.lerp(palette.low?.[key] ?? palette.high[key], palette.high[key], quality);
            mat.thickness = THREE.MathUtils.lerp(baseline('thickness'), 0.018, clarity);
            mat.transmission = THREE.MathUtils.lerp(baseline('transmission'), 0.985, clarity);
            mat.attenuationDistance = THREE.MathUtils.lerp(baseline('attenuationDistance'), 8, clarity);
        });
    }

    setTheme(group: THREE.Group, value: number, subduedIndex: boolean | number = false) {
        group.traverse((child) => {
            if (child.userData.nativeHidden) {
                return;
            }
            if (child.userData.themeAmount) {
                child.userData.themeAmount.value = value;
            }
            if (child.userData.subduedIndex) {
                child.userData.subduedIndex.value = THREE.MathUtils.clamp(Number(subduedIndex), 0, 1);
            }
        });
    }

    apply(group: THREE.Group, value: number) {
        for (const child of group.children) {
            const mesh = child as THREE.Mesh;
            if (mesh.userData.nativeHidden) {
                continue;
            }
            const palette = this.palettes.get(mesh.userData.surface);
            if (!palette) {
                // 印刷画布属于当前档案，返回中的副本也使用同一份画布。
                (mesh.material as THREE.MeshBasicMaterial).opacity = value;
                continue;
            }
            mesh.userData.appearance.value = value;
            const { high, low } = palette;
            if (!low) {
                continue;
            }
            const mat = mesh.material as Surface;
            mat.color.copy(low.color).lerp(high.color, value);
            if (mat.attenuationColor && low.attenuationColor && high.attenuationColor) {
                mat.attenuationColor.copy(low.attenuationColor).lerp(high.attenuationColor, value);
                mat.attenuationDistance =
                    Number.isFinite(low.attenuationDistance) && Number.isFinite(high.attenuationDistance)
                        ? THREE.MathUtils.lerp(low.attenuationDistance, high.attenuationDistance, value)
                        : high.attenuationDistance;
            }
            for (const key of [
                'roughness',
                'metalness',
                'transmission',
                'thickness',
                'clearcoat',
                'clearcoatRoughness',
            ] as const) {
                mat[key] = THREE.MathUtils.lerp(low[key] ?? 0, high[key] ?? 0, value);
            }
            // 整个过渡过程保持同一套透射着色器和渲染通道。
            if (high.transmission > 0) {
                mat.transmission = Math.max(0.000001, mat.transmission);
            }
        }
    }

    dispose(group: THREE.Group) {
        for (const child of group.children) {
            const mesh = child as THREE.Mesh;
            // 拾取代理共享源材质；副本退出时不能销毁其他卡片仍引用的源对象。
            if (mesh.userData.nativeHidden) {
                continue;
            }
            const mat = mesh.material as THREE.MeshBasicMaterial;
            if (!mesh.userData.surface) {
                mat.map?.dispose();
            }
            mat.dispose();
        }
    }
}
