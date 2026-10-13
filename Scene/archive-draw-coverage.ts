import * as THREE from 'three';
import { InstanceUpdates } from './instance-updates.ts';

/** 颜色、透射和法线通道只提交实际相机视锥内的几何。
 * 原有较宽的资料库实例池保留，用于屏幕外的投影物体。
 */
export class ArchiveDrawCoverage {
    private frustum = new THREE.Frustum();
    private matrix = new THREE.Matrix4();
    private box = new THREE.Box3();
    update(camera: THREE.PerspectiveCamera) {
        this.matrix.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
        this.frustum.setFromProjectionMatrix(this.matrix);
    }

    contains(x: number, y: number, z: number) {
        // 沿用原实例池的保守档案边界，包含倾斜幅度。
        this.box.min.set(x - 2.8, y - 0.3, z - 1.2);
        this.box.max.set(x + 2.8, y + 4.1, z + 1.2);
        return this.frustum.intersectsBox(this.box);
    }
}

export class ArchiveShadowCoverage {
    readonly mesh: THREE.InstancedMesh;
    private updates: InstanceUpdates;
    private count = 0;
    constructor(source: THREE.InstancedMesh) {
        this.mesh = new THREE.InstancedMesh(source.geometry, source.material, source.instanceMatrix.count);
        this.mesh.name = 'archive-shadow-coverage';
        this.mesh.frustumCulled = false;
        this.mesh.castShadow = true;
        this.mesh.userData.excludeFromAO = true;
        this.mesh.userData.shadowOnly = true;
        // Three 分别调用阴影和绘制钩子；保持完整阴影覆盖，
        // 颜色通道提交零个三角形后，再恢复公开的实例数量。
        this.mesh.onBeforeRender = () => {
            this.mesh.count = 0;
        };
        this.mesh.onAfterRender = () => {
            this.mesh.count = this.count;
        };
        this.mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
        this.updates = new InstanceUpdates(this.mesh.instanceMatrix);
        source.castShadow = false;
    }

    begin() {
        this.count = 0;
    }

    add(matrix: THREE.Matrix4) {
        if (this.count === this.mesh.instanceMatrix.count) {
            const old = this.mesh.instanceMatrix;
            this.mesh.dispose();
            this.mesh.instanceMatrix = new THREE.InstancedBufferAttribute(
                new Float32Array(old.array.length * 2),
                16,
            ).setUsage(THREE.DynamicDrawUsage);
            this.mesh.instanceMatrix.array.set(old.array);
            this.updates = new InstanceUpdates(this.mesh.instanceMatrix);
        }
        this.updates.set(this.count++ * 16, matrix.elements);
    }

    commit() {
        const countChanged = this.mesh.count !== this.count;
        this.mesh.count = this.count;
        return this.updates.commit() || countChanged;
    }
}
