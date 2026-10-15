import * as THREE from '../node_modules/three/build/three.core.js';
import { NATIVE_HIDDEN_SURFACES } from './platform';

function materialData(material: any) {
    return {
        color: material.color.toArray(),
        roughness: material.roughness ?? 1,
        metalness: material.metalness ?? 0,
        transmission: material.transmission ?? 0,
        thickness: material.thickness ?? 0,
        ior: material.ior ?? 1.5,
        attenuation: material.attenuationColor?.toArray() ?? [1, 1, 1],
        attenuationDistance: Number.isFinite(material.attenuationDistance)
            ? material.attenuationDistance
            : 1e9,
        opacity: material.opacity,
        clearcoat: material.clearcoat ?? 0,
        clearcoatRoughness: material.clearcoatRoughness ?? 0,
    };
}

/**
 * 一套静态卡片模板加每张卡片 20 个数值替代逐部件的完整绘制描述。
 * 模型、材质插值端点和局部变换只在场景重建时发送，归位副本不持有部件树。
 */
export class NativeCardPackets {
    private sourceID = -1;
    private values = new Float64Array(0);
    private count = 0;
    cards: any[] = [];

    begin(model: THREE.Group, palettes: Map<string, any>) {
        this.count = 0;
        this.cards = [];
        if (this.sourceID === model.id) {
            return undefined;
        }
        this.sourceID = model.id;
        const template: any[] = [];
        for (let index = 0; index < model.children.length; index++) {
            const mesh = model.children[index] as THREE.Mesh;
            const material = mesh.material as THREE.Material;
            const name = mesh.userData.surface ?? material.name;
            if (!mesh.isMesh || NATIVE_HIDDEN_SURFACES.has(name)) {
                continue;
            }
            const palette = palettes.get(mesh.userData.surface);
            template.push({
                partIndex: index,
                mesh: mesh.geometry.userData.nativeMesh ?? -1,
                asset: mesh.geometry.userData.asset ?? 'cassette',
                label: mesh.geometry.userData.nativeMesh === undefined,
                name,
                transparent: material.transparent,
                depthWrite: material.depthWrite,
                order: mesh.renderOrder,
                side: material.side,
                castShadow: mesh.castShadow,
                localMatrix: mesh.matrix.elements,
                base: materialData(material),
                high: palette ? materialData(palette.high) : null,
                low: palette?.low ? materialData(palette.low) : null,
                hasMap: !!(material as THREE.MeshBasicMaterial).map,
                interpolateAttenuation:
                    !!palette?.low &&
                    Number.isFinite(palette.low.attenuationDistance) &&
                    Number.isFinite(palette.high.attenuationDistance),
            });
        }
        return template;
    }

    append(group: THREE.Object3D, state: any, selected: boolean, visibility: number) {
        const offset = this.count * 20;
        if (offset + 20 > this.values.length) {
            const expanded = new Float64Array(Math.max(20 * 32, this.values.length * 2));
            expanded.set(this.values);
            this.values = expanded;
        }
        this.values.set(group.matrixWorld.elements, offset);
        this.values[offset + 16] = state.quality;
        this.values[offset + 17] = state.clarity;
        this.values[offset + 18] = state.theme;
        this.values[offset + 19] = Number(state.subdued);
        this.cards.push({
            id: group.id,
            musicKey: group.userData.musicKey ?? null,
            selected,
            shadowOnly: visibility === 2,
        });
        this.count++;
    }

    bytes() {
        return this.values.buffer.slice(0, this.count * 20 * 8);
    }
}
