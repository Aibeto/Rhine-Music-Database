"""models.py — 直接用程序内的 3D 模型（Assets/models/*.glb）。

`archive-assembly.glb` = 档案盒（5.01 × 0.376 × 3.70，长边横置、面朝 ±Y）
`archive-cassette.glb` = 磁带盒（同尺寸谱系，正面印 RHINE LAB. LLC. INFO / NO.001）

导入后：
  1. 归一到「宽 = 1.0」，中心在原点；
  2. 全部材质换成 applook 的发光着色（不吃灯，和全片一致）；
  3. 源集合 hide_render，只有 place() 出来的实例参与渲染（共享 mesh，不复制几何）。

    import models
    models.place('case', C, C+"_case0", off + V((1.2, 0, 0.30)), w=0.44, rz=0.25)
"""
import bpy
from mathutils import Vector, Matrix
import applook

BASE = "/home/aibeto/code/Rhine-Music-Database"
GLB = {
    "case": BASE + "/Assets/models/archive-assembly.glb",
    "cassette": BASE + "/Assets/models/archive-cassette.glb",
}
_cache = {}


def _bbox(objs):
    mn = Vector((1e9, 1e9, 1e9))
    mx = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            mn = Vector((min(mn[i], w[i]) for i in range(3)))
            mx = Vector((max(mx[i], w[i]) for i in range(3)))
    return mn, mx


def archetype(kind, **kw):
    """导入一次，归一到宽=1，返回 (collection, (w,h,d))。"""
    if kind in _cache:
        col, size = _cache[kind]
        if col.name in bpy.data.collections:
            return _cache[kind]
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=GLB[kind])
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    mn, mx = _bbox(meshes)
    size = mx - mn
    s = 1.0 / size.x
    ctr = (mn + mx) * 0.5
    for o in new:
        if o.parent is None:
            o.location = (o.location - ctr) * s
            o.scale = o.scale * s
    applook.flatify_collection(meshes, **kw)

    col = bpy.data.collections.new("ARCH_%s" % kind)
    bpy.context.scene.collection.children.link(col)
    for o in new:
        for pc in list(o.users_collection):
            pc.objects.unlink(o)
        col.objects.link(o)
    col.hide_render = True
    col.hide_viewport = True
    size = (1.0, size.y * s, size.z * s)
    _cache[kind] = (col, size)
    return _cache[kind]


def place(kind, C, name, loc, w=0.44, rz=0.0, rx=0.0, ry=0.0, **kw):
    """在 C 集合里放一个实例。w = 宽（米）。返回新对象的列表。"""
    col, size = archetype(kind, **kw)
    M = (Matrix.Translation(Vector(loc))
         @ Matrix.Rotation(rz, 4, 'Z')
         @ Matrix.Rotation(ry, 4, 'Y')
         @ Matrix.Rotation(rx, 4, 'X')
         @ Matrix.Diagonal((w, w, w, 1.0)))
    out = []
    for src in col.objects:
        if src.type != 'MESH':
            continue
        ob = src.copy()
        ob.data = src.data
        ob.name = "%s_%s" % (name, src.name)
        bpy.context.scene.collection.objects.link(ob)
        ob.matrix_world = M @ src.matrix_world
        for c in list(ob.users_collection):
            if c.name != C:
                c.objects.unlink(ob)
        tgt = bpy.data.collections.get(C)
        if tgt and ob.name not in tgt.objects:
            tgt.objects.link(ob)
        out.append(ob)
    return out


def size_of(kind, w=1.0):
    col, size = archetype(kind)
    return (size[0] * w, size[1] * w, size[2] * w)
