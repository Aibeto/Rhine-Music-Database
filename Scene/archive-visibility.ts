import * as THREE from 'three';
import { COLUMN_SPACING, ROW_SPACING, type ArchiveCell } from './archive-loop.ts';
const edges = [
    [0, 1],
    [0, 2],
    [0, 4],
    [1, 3],
    [1, 5],
    [2, 3],
    [2, 6],
    [3, 7],
    [4, 5],
    [4, 6],
    [5, 7],
    [6, 7],
];
/** 实例池与视图对齐，并裁剪到阵列可能占据的高度范围。 */
export class ArchiveVisibility {
    private camera = new THREE.PerspectiveCamera();
    private frustum = new THREE.Frustum();
    private matrix = new THREE.Matrix4();
    private box = new THREE.Box3();
    candidates = 0;
    private previous: number[] = [];
    private cachedCells: ArchiveCell[] = [];
    update(
        source: THREE.PerspectiveCamera,
        far: number,
        trackX: number,
        trackZ: number,
        extra: boolean,
    ): ArchiveCell[] {
        const inputs = [
            ...source.projectionMatrix.elements,
            ...source.matrixWorldInverse.elements,
            source.near,
            source.far,
            far,
            trackX,
            trackZ,
            Number(extra),
        ];
        if (inputs.every((value, i) => value === this.previous[i])) {
            return this.cachedCells;
        }
        this.previous = inputs;
        this.camera.copy(source, false);
        this.camera.far = Math.min(source.far, Math.max(source.near + 1, far + 8));
        this.camera.updateProjectionMatrix();
        // 屏幕余量和完整档案边界保证轮廓及屏幕外的投影物体
        // 在到达窗口边缘之前仍然存在。
        const margin = extra ? 1.5 : 1.18;
        this.camera.projectionMatrix.elements[0] /= margin;
        this.camera.projectionMatrix.elements[5] /= margin;
        this.camera.projectionMatrixInverse.copy(this.camera.projectionMatrix).invert();
        this.matrix.multiplyMatrices(this.camera.projectionMatrix, source.matrixWorldInverse);
        this.frustum.setFromProjectionMatrix(this.matrix);
        const inverse = this.matrix.clone().invert();
        const vertices = Array.from(
            {
                length: 8,
            },
            (_, i) => new THREE.Vector3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1).applyMatrix4(inverse),
        );
        const slab = new THREE.Box3();
        const bottom = -6.5;
        const top = 6.5;
        for (const point of vertices) {
            if (point.y >= bottom && point.y <= top) {
                slab.expandByPoint(point);
            }
        }
        for (const [a, b] of edges) {
            for (const y of [bottom, top]) {
                const start = vertices[a];
                const end = vertices[b];
                const dy = end.y - start.y;
                if (Math.abs(dy) < 1e-9) {
                    continue;
                }
                const t = (y - start.y) / dy;
                if (t >= 0 && t <= 1) {
                    slab.expandByPoint(start.clone().lerp(end, t));
                }
            }
        }
        if (slab.isEmpty()) {
            this.candidates = 0;
            return (this.cachedCells = []);
        }
        const minLane = Math.floor((slab.min.x - 2.8 + trackX) / COLUMN_SPACING + 2) - 1;
        const maxLane = Math.ceil((slab.max.x + 2.8 + trackX) / COLUMN_SPACING + 2) + 1;
        const minRow = Math.floor((slab.min.z - 0.6 - trackZ) / ROW_SPACING + 15.5) - 2;
        const maxRow = Math.ceil((slab.max.z + 0.6 - trackZ) / ROW_SPACING + 15.5) + 2;
        const cells: ArchiveCell[] = [];
        for (let lane = minLane; lane <= maxLane; lane++) {
            for (let row = minRow; row <= maxRow; row++) {
                const x = (lane - 2) * COLUMN_SPACING - trackX;
                const z = (row - 15.5) * ROW_SPACING + trackZ;
                this.box.min.set(x - 2.8, bottom, z - 1.2);
                this.box.max.set(x + 2.8, top, z + 1.2);
                if (this.frustum.intersectsBox(this.box)) {
                    cells.push({
                        lane,
                        row,
                    });
                }
            }
        }
        this.candidates = cells.length;
        return (this.cachedCells = cells);
    }

    intersects(x: number, y: number, z: number) {
        // 边界保守包含轻微的 X 轴倾斜和正常波动幅度。
        // 额外深度使相邻的屏幕外投影物体仍包含在实例集合中。
        this.box.min.set(x - 2.8, y - 0.3, z - 1.2);
        this.box.max.set(x + 2.8, y + 4.1, z + 1.2);
        return this.frustum.intersectsBox(this.box);
    }
}
