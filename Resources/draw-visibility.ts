import * as THREE from '../node_modules/three/build/three.core.js';

/**
 * 在生成部件绘制包前剔除屏幕外且不会进入阴影图的归位卡片。
 * 只跳过绘制提交，不改变物理运动、对象身份、拾取或归位完成条件。
 */
export class NativeDrawVisibility {
    private cameraFrustum = new THREE.Frustum();
    private projectionView = new THREE.Matrix4();
    private lightView = new THREE.Matrix4();
    private worldSphere = new THREE.Sphere();
    private lightCenter = new THREE.Vector3();

    constructor() {
        // 与 RhiScene::globals 的固定阴影相机一致；深度方向保守保留，
        // 只剔除完全落在 32 × 30 阴影覆盖范围之外的投影物体。
        const light = new THREE.PerspectiveCamera();
        light.position.set(-6, 14, -5);
        light.lookAt(0, 0, 0);
        light.updateMatrixWorld(true);
        this.lightView.copy(light.matrixWorldInverse);
    }

    update(camera: THREE.Camera) {
        this.projectionView.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
        this.cameraFrustum.setFromProjectionMatrix(this.projectionView);
    }

    // 1 表示完整绘制，2 表示只提交阴影投影部件，0 表示无需提交。
    classify(group: THREE.Object3D) {
        // 边界跟随对象及其复用池释放，不在独立缓存中持有副本引用。
        let local = group.userData.nativeDrawBounds as THREE.Sphere | undefined;
        if (!local) {
            const box = new THREE.Box3();
            const partBox = new THREE.Box3();
            // 归位副本沿用选中卡片的固定局部部件变换；查看器不走这条路径。
            for (const child of group.children) {
                const mesh = child as THREE.Mesh;
                if (!mesh.isMesh || mesh.userData.nativeHidden) {
                    continue;
                }
                if (!mesh.geometry.boundingBox) {
                    mesh.geometry.computeBoundingBox();
                }
                partBox.copy(mesh.geometry.boundingBox!).applyMatrix4(mesh.matrix);
                box.union(partBox);
            }
            if (box.isEmpty()) {
                return 1;
            }
            local = box.getBoundingSphere(new THREE.Sphere());
            group.userData.nativeDrawBounds = local;
        }
        this.worldSphere.copy(local).applyMatrix4(group.matrixWorld);
        // 额外覆盖阴影法线偏移及边界浮点误差，避免临界位置闪烁。
        this.worldSphere.radius += 0.1;
        if (this.cameraFrustum.intersectsSphere(this.worldSphere)) {
            return 1;
        }
        this.lightCenter.copy(this.worldSphere.center).applyMatrix4(this.lightView);
        const castsVisibleShadow =
            Math.abs(this.lightCenter.x) <= 16.1 + this.worldSphere.radius &&
            Math.abs(this.lightCenter.y) <= 15.1 + this.worldSphere.radius;
        return castsVisibleShadow ? 2 : 0;
    }
}
