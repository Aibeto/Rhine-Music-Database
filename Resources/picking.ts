import * as T from '../node_modules/three/build/three.core.js';
// 碰撞加速结构不改变渲染网格及其命中三角形。
type Node = {
    box: T.Box3;
    left?: Node;
    right?: Node;
    triangles?: number[];
};
// 显式释放几何缓存，避免 Qt JS 引擎在场景重建时依赖弱引用回收顺序。
const trees = new Map<T.BufferGeometry, Node>();
let enabled = true;
const original = T.Mesh.prototype.raycast;
function tree(geometry: T.BufferGeometry): Node {
    let root = trees.get(geometry);
    if (root) {
        return root;
    }
    const p = geometry.attributes.position;
    const index = geometry.index;
    const ids = Array.from(
        {
            length: (index?.count ?? p.count) / 3,
        },
        (_, i) => i * 3,
    );
    const boxes = ids.map((i) => {
        const box = new T.Box3();
        for (let j = 0; j < 3; j++) {
            const v = index ? index.getX(i + j) : i + j;
            box.expandByPoint(new T.Vector3(p.getX(v), p.getY(v), p.getZ(v)));
        }
        return box;
    });
    function build(ids: number[]): Node {
        const box = new T.Box3();
        for (const i of ids) {
            box.union(boxes[i / 3]);
        }
        if (ids.length <= 8) {
            return {
                box,
                triangles: ids,
            };
        }
        const size = box.getSize(new T.Vector3());
        let axis: 'x' | 'y' | 'z';
        if (size.x >= size.y && size.x >= size.z) {
            axis = 'x';
        } else if (size.y >= size.z) {
            axis = 'y';
        } else {
            axis = 'z';
        }
        ids.sort(
            (a, b) =>
                boxes[a / 3].min[axis] +
                boxes[a / 3].max[axis] -
                (boxes[b / 3].min[axis] + boxes[b / 3].max[axis]),
        );
        const split = ids.length >> 1;
        return {
            box,
            left: build(ids.slice(0, split)),
            right: build(ids.slice(split)),
        };
    }
    root = build(ids);
    trees.set(geometry, root);
    geometry.addEventListener('dispose', () => trees.delete(geometry));
    return root;
}
const inverse = new T.Matrix4();
const ray = new T.Ray();
const a = new T.Vector3();
const b = new T.Vector3();
const c = new T.Vector3();
const hit = new T.Vector3();
const boxHit = new T.Vector3();
function usesNativePicking(geometry: T.BufferGeometry) {
    const assets = (globalThis as any).NativeAssets;
    return (
        geometry.userData.asset === 'cassette' &&
        geometry.userData.nativeMesh >= 0 &&
        typeof assets?.raycastMesh === 'function'
    );
}
T.Mesh.prototype.raycast = function (raycaster, intersects) {
    const g = this.geometry;
    const m = this.material as T.Material;
    if (
        !enabled ||
        Array.isArray(m) ||
        !g.attributes.position ||
        g.drawRange.start !== 0 ||
        Number.isFinite(g.drawRange.count)
    ) {
        original.call(this, raycaster, intersects);
        return;
    }
    inverse.copy(this.matrixWorld).invert();
    ray.copy(raycaster.ray).applyMatrix4(inverse);
    if (usesNativePicking(g)) {
        // 只有已烘焙、不可变的原生卡片模型使用 C++ BVH；标签及查看器保留原路径。
        const result = (globalThis as any).NativeAssets.raycastMesh(
            g.userData.nativeMesh,
            ray.origin.toArray(),
            ray.direction.toArray(),
            m.side,
        );
        if (typeof result.x !== 'number') {
            return;
        }
        const point = new T.Vector3(result.x, result.y, result.z).applyMatrix4(this.matrixWorld);
        const distance = raycaster.ray.origin.distanceTo(point);
        if (distance >= raycaster.near && distance <= raycaster.far) {
            intersects.push({ distance, point, object: this });
        }
        return;
    }
    const root = tree(g);
    if (!ray.intersectsBox(root.box)) {
        return;
    }
    let best = Infinity;
    let bestPoint: T.Vector3 | undefined;
    const p = g.attributes.position;
    const index = g.index;
    function visit(node: Node) {
        if (!ray.intersectBox(node.box, boxHit)) {
            return;
        }
        const lower = ray.origin.distanceToSquared(boxHit);
        if (!node.box.containsPoint(ray.origin) && lower > best) {
            return;
        }
        if (node.triangles) {
            for (const i of node.triangles) {
                a.fromBufferAttribute(p, index ? index.getX(i) : i);
                b.fromBufferAttribute(p, index ? index.getX(i + 1) : i + 1);
                c.fromBufferAttribute(p, index ? index.getX(i + 2) : i + 2);
                const point =
                    m.side === T.BackSide
                        ? ray.intersectTriangle(c, b, a, true, hit)
                        : ray.intersectTriangle(a, b, c, m.side !== T.DoubleSide, hit);
                if (point) {
                    const distance = ray.origin.distanceToSquared(point);
                    if (distance < best) {
                        best = distance;
                        bestPoint = point.clone();
                    }
                }
            }
        } else {
            visit(node.left!);
            visit(node.right!);
        }
    }

    visit(root);
    if (!bestPoint) {
        return;
    }
    bestPoint.applyMatrix4(this.matrixWorld);
    const distance = raycaster.ray.origin.distanceTo(bestPoint);
    if (distance < raycaster.near || distance > raycaster.far) {
        return;
    }
    intersects.push({
        distance,
        point: bestPoint,
        object: this,
    });
};
export function preparePicking(scene: T.Object3D) {
    scene.traverse((o: any) => {
        if (o.isMesh && o.geometry.attributes.position) {
            if (usesNativePicking(o.geometry)) {
                (globalThis as any).NativeAssets.preparePicking();
            } else {
                tree(o.geometry);
            }
        }
    });
}

export function setPicking(value: boolean) {
    enabled = value;
}
