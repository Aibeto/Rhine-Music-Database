# -*- coding: utf-8 -*-
"""g4 — 手写镜头 13–16（档案库=唱盘 / 侧立唱片 / 多米诺倒下 / 尘浪）。
SHOTS = [(key, t0, t1, builder)], builder(off, C, f0, f1) -> camera。
平面发光材质，不用灯；13–16 不出现橙色。
"""
import bpy, bmesh, math, random
from math import radians as rd, sin, cos, sqrt, pi
import shotkit as K
from shotkit import V, M, flat, cam_at, move, disc, box, torus, points_mesh, dust
import pvlib as L
from mathutils import Matrix, Vector

PAPER = (0.906, 0.906, 0.882)
PAPER2 = (0.851, 0.847, 0.808)
INK = (0.126, 0.133, 0.122)
MOSS = (0.467, 0.482, 0.376)
MINT = (0.561, 0.890, 0.733)

SHOTS = [
    ("13", 20.36, 23.04, lambda off, C, f0, f1: s13(off, C, f0, f1)),
    ("14", 23.04, 24.11, lambda off, C, f0, f1: s14(off, C, f0, f1)),
    ("15", 24.11, 25.71, lambda off, C, f0, f1: s15(off, C, f0, f1)),
    ("16", 25.71, 27.32, lambda off, C, f0, f1: s16(off, C, f0, f1)),
]


# ---------------------------------------------------------------- helpers
def mt(name, color, strength=1.0, alpha=1.0):
    return K.flat("G4_" + name, color, fac=1.0, strength=strength, alpha=alpha)


GRV = (0.185, 0.193, 0.176)      # groove highlight on black vinyl (below alu2)


def fcurves_of(ob):
    ad = ob.animation_data
    if not ad or not ad.action:
        return []
    act = ad.action
    try:
        if len(act.fcurves):
            return list(act.fcurves)
    except Exception:
        pass
    out = []
    try:
        for lay in act.layers:
            for st in lay.strips:
                for slot in act.slots:
                    cb = st.channelbag(slot)
                    if cb:
                        out.extend(cb.fcurves)
    except Exception:
        pass
    return out


def linear(ob, ease=None):
    for fc in fcurves_of(ob):
        for kp in fc.keyframe_points:
            kp.interpolation = ease or 'LINEAR'


def bind(ob, piv, piv_world):
    """geometry authored in world space; pivot at piv_world with no scale."""
    ob.parent = piv
    ob.matrix_parent_inverse = Matrix.Translation(V(piv_world)).inverted()
    return ob


def cube_bm(bm, center, dim, rz=0.0):
    m = (Matrix.Translation(V(center)) @ Matrix.Rotation(rz, 4, 'Z') @
         Matrix.Diagonal((dim[0], dim[1], dim[2], 1.0)))
    bmesh.ops.create_cube(bm, size=1.0, matrix=m)


def mesh_from_bm(C, name, bm, mat):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    return ob


def arc_ribbon(C, name, loc, r, th0, th1, w, z, mat, segs=48, plane='xy'):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    ia, oa = [], []
    for i in range(segs + 1):
        th = th0 + (th1 - th0) * i / segs
        a = ((r - w * 0.5) * cos(th), (r - w * 0.5) * sin(th), z)
        b = ((r + w * 0.5) * cos(th), (r + w * 0.5) * sin(th), z)
        ia.append(bm.verts.new(a))
        oa.append(bm.verts.new(b))
    bm.verts.ensure_lookup_table()
    for i in range(segs):
        bm.faces.new((ia[i], ia[i + 1], oa[i + 1], oa[i]))
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.data.materials.append(mat)
    L.link(ob, C)
    return ob


def cloud(C, name, items, mat, sub=2, smooth=True):
    """items = [(world pos, radius), ...] -> one mesh object, origin at world 0."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for p, rad in items:
        bmesh.ops.create_icosphere(bm, subdivisions=sub, radius=rad,
                                   matrix=Matrix.Translation(V(p)))
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    if smooth:
        L.smooth(ob)
    return ob


def streaks(C, name, items, mat):
    """items = [(world pos, length, width, angle_z)] -> flat ribbon splinters (motion streaks)."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for p, ln, wd, az in items:
        cube_bm(bm, p, (ln, wd, wd * 0.8), az)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    return ob


def splinters(C, name, items, mat):
    """items = [(pos, length, width, axis(Vector), angle)] -> tiny randomly-tumbled chips."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for p, ln, wd, ax, ang in items:
        m = (Matrix.Translation(V(p)) @ Matrix.Rotation(ang, 4, ax) @
             Matrix.Diagonal((ln, wd, wd, 1.0)))
        bmesh.ops.create_cube(bm, size=1.0, matrix=m)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    return ob



def place(ob, piv, W):
    """Attach ob to piv so that it sits exactly at world matrix W now, then rides piv."""
    bpy.context.view_layer.update()
    pm = piv.matrix_world.copy()
    ob.parent = piv
    ob.matrix_parent_inverse = Matrix.Identity(4)
    ob.matrix_basis = pm.inverted() @ W
    return ob


def puff_mat(name, color, strength=1.0, a_max=0.5, r_out=0.50):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    sub = nt.nodes.new("ShaderNodeVectorMath")
    sub.operation = 'SUBTRACT'
    sub.inputs[1].default_value = (0.5, 0.5, 0.0)
    ln = nt.nodes.new("ShaderNodeVectorMath")
    ln.operation = 'LENGTH'
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = 0.04
    mr.inputs["From Max"].default_value = r_out
    mr.inputs["To Min"].default_value = a_max
    mr.inputs["To Max"].default_value = 0.0
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1)
    em.inputs["Strength"].default_value = strength
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    mx = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(tc.outputs["UV"], sub.inputs[0])
    nt.links.new(sub.outputs["Vector"], ln.inputs[0])
    nt.links.new(ln.outputs["Value"], mr.inputs["Value"])
    nt.links.new(mr.outputs["Result"], mx.inputs["Fac"])
    nt.links.new(tr.outputs["BSDF"], mx.inputs[1])
    nt.links.new(em.outputs["Emission"], mx.inputs[2])
    nt.links.new(mx.outputs["Shader"], out.inputs["Surface"])
    m.surface_render_method = 'BLENDED'
    m.use_backface_culling = False
    return m


def puff_quad(C, name, pos, size, roll, mat, toward, piv=None, piv_world=None, sy=None,
              flow=None):
    sy = size if sy is None else sy
    me = bpy.data.meshes.new(name)
    me.from_pydata([(-0.5, 0, -0.5), (0.5, 0, -0.5), (0.5, 0, 0.5), (-0.5, 0, 0.5)],
                   [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for i, (a, b) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[i].uv = (a, b)
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    d = V(toward) - V(pos)
    n = d.normalized() if d.length > 1e-6 else V((0, 1, 0))
    if flow is not None:
        x = V(flow) - n * V(flow).dot(n)
        x = x.normalized() if x.length > 1e-6 else V((1, 0, 0))
    else:
        up = V((0, 0, 1))
        x = up.cross(n)
        x = x.normalized() if x.length > 1e-6 else V((1, 0, 0))
    z = x.cross(n).normalized()
    q = Matrix((x, n, z)).transposed().to_4x4()
    W = (Matrix.Translation(V(pos)) @ q @ Matrix.Rotation(roll, 4, 'Y') @
         Matrix.Diagonal((size, 1.0, sy, 1.0)))
    if piv is not None:
        place(ob, piv, W)
    else:
        ob.matrix_world = W
    return ob



def fog_mat(name, color, strength=1.0, amax=0.7, cx=0.02, cy=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Location"].default_value = (cx, cy, 0.0)
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 4.2
    noise.inputs["Detail"].default_value = 4.0
    noise.inputs["Roughness"].default_value = 0.62
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.30
    ramp.color_ramp.elements[0].color = (amax * 0.22, amax * 0.22, amax * 0.22, 1)
    ramp.color_ramp.elements[1].position = 0.70
    ramp.color_ramp.elements[1].color = (amax, amax, amax, 1)
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1)
    em.inputs["Strength"].default_value = strength
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    mx = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], noise.inputs["Vector"])
    nt.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], mx.inputs["Fac"])
    nt.links.new(tr.outputs["BSDF"], mx.inputs[1])
    nt.links.new(em.outputs["Emission"], mx.inputs[2])
    nt.links.new(mx.outputs["Shader"], out.inputs["Surface"])
    m.surface_render_method = 'BLENDED'
    return m, mp


def fog_card(cam, C, name, dist, size, color, strength, f0, f1, amax=0.7):
    """Noise-alpha fog sheet parented to the camera; grows from nothing to a full-frame wash."""
    mat, mp = fog_mat("G4_" + name, color, strength, amax)
    me = bpy.data.meshes.new(name)
    me.from_pydata([(-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0)],
                   [], [(0, 1, 2, 3)])
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(mat)
    L.link(ob, C)
    ob.parent = cam
    ob.matrix_parent_inverse = Matrix.Identity(4)
    ob.location = (0.0, 0.0, -dist)
    ob.scale = (size * 0.04, size * 0.04, 1.0)
    ob.keyframe_insert("scale", frame=f0)
    ob.scale = (size, size, 1.0)
    ob.keyframe_insert("scale", frame=f1)
    for fc in fcurves_of(ob):
        for kp in fc.keyframe_points:
            kp.interpolation = 'BEZIER'
    loc = mp.inputs["Location"]
    loc.default_value = (0.0, 0.0, 0.0)
    loc.keyframe_insert("default_value", index=0, frame=f0)
    loc.default_value = (3.4, 0.9, 0.0)
    loc.keyframe_insert("default_value", index=0, frame=f1)
    return ob


def rotz_keys(ob, f0, f1, a0, a1, ease=None):
    ob.rotation_euler = (0, 0, a0)
    ob.keyframe_insert("rotation_euler", frame=f0)
    ob.rotation_euler = (0, 0, a1)
    ob.keyframe_insert("rotation_euler", frame=f1)
    linear(ob, ease)


# ================================================================ 13
def s13(off, C, f0, f1):
    """档案库 = 巨大唱盘：同心货架阵列 + 一道光环沿纹路走一圈 + 缓转 30°。"""
    rc = off + V((-1.20, -0.80, 0.0))          # record centre (kept clear of the next lane)
    c, s = (0.25, 0.50, 0.0), 2.25             # aim = rc + (1.45, 1.30) -> centre low-right
    piv = bpy.data.objects.new(C + "_piv", None)
    piv.location = rc
    L.link(piv, C)

    def par(ob):
        return bind(ob, piv, rc)

    bms = {'paper': bmesh.new(), 'paper2': bmesh.new(), 'moss': bmesh.new()}
    for bi in range(12):
        rad = 1.02 + 0.36 * bi
        w = 0.26
        n = max(9, int(2 * pi * rad / (0.46 + 0.02 * bi)))
        step = 2 * pi / n
        # size from the OUTER arc so neighbours abut there and the ring reads as a solid groove
        tang = 2.0 * (rad + w * 0.5) * sin(step * 0.5) * 0.94
        for i in range(n):
            th = (i + 0.5 * (bi % 2)) * step
            deg = math.degrees(th) % 120.0
            if deg < 2.0 or deg > 118.0:              # three radial aisles
                continue
            h = 0.100 + 0.055 * (((i * 5 + bi * 7) % 4) / 3.0)
            tone = 'paper2'
            if (i + bi * 3) % 9 == 0:
                tone = 'paper'
            elif (i + bi * 5) % 23 == 0:
                tone = 'moss'
            cube_bm(bms[tone], rc + V((rad * cos(th), rad * sin(th), h * 0.5)),
                    (tang, w, h), th + pi * 0.5)
    for t in ('paper', 'paper2', 'moss'):
        par(mesh_from_bm(C, C + "_rings_" + t, bms[t], M(t)))

    # hub = label area of the record
    hub = [par(disc(C, C + "_hub", (-1.20, -0.80, 0.030), 0.800, off, 'vinyl', thick=0.06)),
           par(torus(C, C + "_hr", (-1.20, -0.80, 0.062), 0.804, 0.011, off, 'mint', mv=180))]
    for k in range(9):
        hub.append(par(torus(C, C + "_g%d" % k, (-1.20, -0.80, 0.070),
                             0.43 + k * 0.036, 0.0036, off, 'alu2', mv=96)))
    hub += [par(torus(C, C + "_hr2", (-1.20, -0.80, 0.064), 0.375, 0.0045, off, 'paper2', mv=160)),
            par(L.prim('cyl', C, C + "_lab",
                       loc=off + V((-1.20, -0.80, 0.066)), r=0.220, d=0.012,
                       material=mt("S13_LAB", PAPER, 1.0))),
            par(L.prim('cyl', C, C + "_hole",
                       loc=off + V((-1.20, -0.80, 0.074)), r=0.074, d=0.010,
                       material=mt("S13_HOLE", INK, 0.7)))]

    # one light ring walking a groove
    RA = 2.70
    arcp = bpy.data.objects.new(C + "_arcp", None)
    arcp.location = rc
    L.link(arcp, C)
    head = rd(223)
    for w, nm, col, st, span, z in ((0.048, "S13_CORE", (0.82, 1.0, 0.90), 5.5, 8.0, 0.206),
                                    (0.130, "S13_MID", MINT, 2.4, 46.0, 0.199),
                                    (0.320, "S13_TAIL", MOSS, 1.2, 162.0, 0.192)):
        a = arc_ribbon(C, C + "_a" + nm, rc, RA, head - rd(span), head, w, z,
                       mt(nm, col, st), segs=64)
        bind(a, arcp, rc)
    par(L.prim('torus', C, C + "_track", loc=off + V((-1.20, -0.80, 0.168)), R=RA,
               r=0.016, material=mt("S13_TRACK", (0.055, 0.062, 0.053), 1.0), mv=220, nv=8))
    bind(L.prim('ico', C, C + "_spark",
                loc=rc + V((RA * cos(head), RA * sin(head), 0.216)),
                r=0.038, sub=2, material=mt("S13_SPARK", (1.0, 1.0, 0.97), 5.0)), arcp, rc)

    piv.rotation_euler = (0, 0, 0)
    piv.keyframe_insert("rotation_euler", frame=f0)
    piv.rotation_euler = (0, 0, rd(15))
    piv.keyframe_insert("rotation_euler", frame=(f0 + f1) // 2)
    piv.rotation_euler = (0, 0, rd(30))
    piv.keyframe_insert("rotation_euler", frame=f1)
    linear(piv)
    rotz_keys(arcp, f0, f1, 0.0, 2 * pi)

    cam = cam_at(C, off, c, s, az=-96, el=78, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-96, el=78, fill=1.0, kind='orbit',
         az2=-88, el2=82, ease='LIN')
    return cam


# ================================================================ 14
def _s14_face(C, off, cy, R=0.150):
    """paper-thin disc face lit from the left edge: geometry gradient, not textures."""
    N = 132
    ladder = [3.20, 2.55, 1.95, 1.50, 1.15, 0.94, 0.79, 0.67, 0.57, 0.48,
              0.405, 0.340, 0.280, 0.225, 0.175, 0.132, 0.098, 0.072, 0.052, 0.036]
    cream = V((1.0, 0.982, 0.94))
    olive = V((0.72, 0.75, 0.62))
    per = N // len(ladder)
    bms = [bmesh.new() for _ in ladder]
    for i in range(N):
        x0 = -R + 2 * R * i / N
        x1 = -R + 2 * R * (i + 1) / N
        h0 = sqrt(max(R * R - x0 * x0, 0.0))
        h1 = sqrt(max(R * R - x1 * x1, 0.0))
        idx = min(len(ladder) - 1, i // per)
        bm = bms[idx]
        v = [bm.verts.new((x0, -h0, 0.0)), bm.verts.new((x1, -h1, 0.0)),
             bm.verts.new((x1, h1, 0.0)), bm.verts.new((x0, h0, 0.0))]
        bm.faces.new(v)
    objs = []
    for k, bm in enumerate(bms):
        b = ladder[k]
        base = cream.lerp(olive, min(1.0, max(0.0, 1.0 - b / 1.1)))
        col = (base[0] * b, base[1] * b, base[2] * b)
        ob = mesh_from_bm(C, C + "_fp%d" % k, bm, mt("S14F%d" % k, col, 1.0))
        ob.location = off + V((0, cy, 0.152))
        ob.rotation_euler = (rd(90), 0, 0)
        objs.append(ob)
    # fibres: thin strands, brighter or darker than their surround
    bmf = [bmesh.new() for _ in ladder]
    rr = random.Random(1414)
    for i in range(78):
        t = rr.uniform(0.03, 0.95)
        x = -R + 2 * R * t
        h = sqrt(max(R * R - x * x, 0.0)) * 0.94
        zz = rr.uniform(-h, h)
        ln = rr.uniform(0.012, 0.075)
        wd = rr.uniform(0.0007, 0.0016)
        ang = rr.uniform(-0.45, 0.45)
        b = ladder[min(len(ladder) - 1, int(t * len(ladder) * 0.9))]
        lvl = min(len(ladder) - 1, max(0, int(t * len(ladder)) + (2 if i % 3 else -1)))
        bm = bmf[lvl]
        ca, sa = cos(ang), sin(ang)
        cx, cz = x, zz
        pts = [(-ln * .5, -wd * .5), (ln * .5, -wd * .5), (ln * .5, wd * .5), (-ln * .5, wd * .5)]
        v = [bm.verts.new((cx + p[0] * ca - p[1] * sa, cz + p[0] * sa + p[1] * ca, 0.0)) for p in pts]
        bm.faces.new(v)
    for k, bm in enumerate(bmf):
        if not bm.verts:
            bm.free()
            continue
        b = ladder[k]
        base = cream.lerp(olive, min(1.0, max(0.0, 1.0 - b / 1.1)))
        col = (base[0] * b * 1.22, base[1] * b * 1.22, base[2] * b * 1.22)
        ob = mesh_from_bm(C, C + "_ff%d" % k, bm, mt("S14X%d" % k, col, 1.0))
        ob.location = off + V((0, cy - 0.00007, 0.152))
        ob.rotation_euler = (rd(90), 0, 0)
        objs.append(ob)
    return objs


def s14(off, C, f0, f1):
    """一张唱片侧立，薄如纸：侧光穿透，边缘发绿，内部纤维可见；完全静止。"""
    c, s = (0.0, 0.0, 0.152), 0.170
    R = 0.150
    body = L.prim('cyl', C, C + "_body", loc=off + V((0, 0, 0.152)), r=R, d=0.0011,
                  material=mt("S14BODY", (0.055, 0.062, 0.052), 1.0))
    body.rotation_euler = (rd(90), 0, 0)
    _s14_face(C, off, -0.00075)
    # green transmission rim
    rim = torus(C, C + "_rim", (0, 0, 0.152), 0.1502, 0.0013, off, 'mint',
                rot=(rd(90), 0, 0), mv=192)
    rim.data.materials.clear()
    rim.data.materials.append(mt("S14_MINT", MINT, 1.0))
    back = torus(C, C + "_rim2", (0, 0.0011, 0.152), 0.1496, 0.0022, off, 'moss',
                 rot=(rd(90), 0, 0), mv=192)
    back.data.materials.clear()
    back.data.materials.append(mt("S14_BACK", (0.30, 0.46, 0.36), 0.45))
    # where the light enters, the rim burns white-hot
    hot = arc_ribbon(C, C + "_hot", off + V((0, -0.0010, 0.152)), 0.1499,
                     rd(118), rd(242), 0.0038, 0.0,
                     mt("S14_HOT", (1.0, 0.95, 0.78), 2.6), segs=48)
    hot.rotation_euler = (rd(90), 0, 0)
    # label + spindle hole
    lab = L.prim('cyl', C, C + "_lab", loc=off + V((0, -0.0012, 0.152)), r=0.045, d=0.0004,
                 material=mt("S14_LAB", (0.62, 0.63, 0.575), 1.0))
    lab.rotation_euler = (rd(90), 0, 0)
    hol = L.prim('cyl', C, C + "_hol", loc=off + V((0, -0.0015, 0.152)), r=0.0100, d=0.0004,
                 material=mt("S14_HOL", INK, 1.0))
    hol.rotation_euler = (rd(90), 0, 0)

    cam = cam_at(C, off, c, s, az=33, el=6, fill=1.0, lens=100, dof_k=1.1, fstop=5.6)
    return cam


# ================================================================ 15
def _s15_record(C, off, R):
    hero = bpy.data.objects.new(C + "_rec", None)
    hp = off + V((0, 0, R))
    hero.location = hp
    L.link(hero, C)

    def par(ob):
        ob.parent = hero
        ob.matrix_parent_inverse = Matrix.Translation(hp).inverted()
        return ob

    par(L.prim('cyl', C, C + "_disc", loc=hp, r=R, d=0.0038, material=M('vinyl')))
    par(L.prim('cyl', C, C + "_face", loc=hp + V((0, 0, 0.00195)), r=R - 0.0009, d=0.0002,
               material=mt("S15_FACE", (0.050, 0.056, 0.050), 1.0)))
    grv = mt("S15_GRV", GRV, 1.0)
    for k in range(19):
        rk = 0.048 + k * 0.0053
        par(L.prim('torus', C, C + "_gr%d" % k, loc=hp + V((0, 0, 0.0021)),
                   R=rk, r=0.00050, material=grv, mv=96, nv=6))
    par(L.prim('torus', C, C + "_rim", loc=hp, R=R, r=0.0016,
               material=mt("S15_MINT", MINT, 1.3), mv=192, nv=6))
    par(L.prim('cyl', C, C + "_lab", loc=hp + V((0, 0, 0.0022)), r=0.044, d=0.0006,
               material=mt("S15_LAB", (0.72, 0.72, 0.675), 1.0)))
    par(L.prim('cyl', C, C + "_hole", loc=hp + V((0, 0, 0.0026)), r=0.0062, d=0.0004,
               material=mt("S15_HOLE", INK, 1.0)))
    return hero, hp


def s15(off, C, f0, f1):
    """唱片以 45° 倒下，像第一张多米诺，拍地激起尘浪；快速下摇跟随。"""
    R = 0.150
    c0 = (0.0, 0.0, 0.165)
    hero, hp = _s15_record(C, off, R)

    fi = f0 + 8
    lean = rd(10)
    for f, rot, y, z in ((f0, rd(90) - lean, R * sin(lean), R * cos(lean)),
                         (f0 + 4, rd(45), R * sin(rd(45)), R * cos(rd(45))),
                         (fi, 0.0, R, 0.0022),
                         (f1, 0.0, R, 0.0022)):
        hero.rotation_euler = (rot, 0, 0)
        hero.location = off + V((0, y, z))
        hero.keyframe_insert("rotation_euler", frame=f)
        hero.keyframe_insert("location", frame=f)
    for fc in fcurves_of(hero):
        for kp in fc.keyframe_points:
            kp.interpolation = 'SINE'
            kp.easing = 'EASE_IN'

    # dust burst — geometry authored around its own centre
    cen = off + V((0, 0.150, 0.0))
    rr = random.Random(1515)
    fine, mid, chunk, puff = [], [], [], []
    for i in range(1500):
        th = rr.uniform(0, 2 * pi)
        rad = 0.148 + abs(rr.gauss(0, 0.048)) + rr.uniform(0.0, 0.075)
        p = cen + V((rad * cos(th), rad * sin(th),
                     0.001 + abs(rr.gauss(0, 0.009))))
        fine.append((p, rr.uniform(0.0006, 0.0016)))
    for i in range(420):
        th = rr.uniform(0, 2 * pi)
        rad = 0.160 + rr.uniform(0.0, 0.125)
        mid.append((cen + V((rad * cos(th), rad * sin(th),
                             0.001 + abs(rr.gauss(0, 0.020)) + rr.uniform(0.0, 0.022))),
                    rr.uniform(0.0014, 0.0034)))
    for i in range(110):
        th = rr.uniform(0, 2 * pi)
        rad = 0.20 + rr.uniform(0.0, 0.14)
        chunk.append((cen + V((rad * cos(th), rad * sin(th), rr.uniform(0.004, 0.062))),
                      rr.uniform(0.0022, 0.0050)))
    for i in range(30):
        th = rr.uniform(0, 2 * pi)
        rad = rr.uniform(0.22, 0.36)
        puff.append((cen + V((rad * cos(th), rad * sin(th), rr.uniform(0.008, 0.085))),
                     rr.uniform(0.0065, 0.012)))
    dx = bpy.data.objects.new(C + "_dust", None)
    dx.location = cen
    L.link(dx, C)
    # grain: tumbled chips, 1-3 px on screen
    chips = {'a': [], 'b': [], 'c': []}
    for i in range(3400):
        th = rr.uniform(0, 2 * pi)
        rad = 0.146 + abs(rr.gauss(0, 0.050)) + rr.uniform(0.0, 0.085)
        h = 0.001 + abs(rr.gauss(0, 0.010)) * (1.0 + 1.6 * abs(rr.gauss(0, 1)))
        p = cen + V((rad * cos(th), rad * sin(th), h))
        ax = V((rr.gauss(0, 1), rr.gauss(0, 1), rr.gauss(0, 1))).normalized()
        k = 'a' if i % 3 else ('b' if i % 7 else 'c')
        chips[k].append((p, rr.uniform(0.0012, 0.0055), rr.uniform(0.0005, 0.0013),
                         ax, rr.uniform(0, pi)))
    for k, mat in (('a', mt("S15_GA", (0.86, 0.86, 0.82), 1.0)),
                   ('b', mt("S15_GB", (1.0, 1.0, 0.96), 1.5)),
                   ('c', mt("S15_GC", (0.52, 0.52, 0.49), 1.0))):
        bind(splinters(C, C + "_dsp" + k, chips[k], mat), dx, cen)
    for f, k in ((fi - 1, 0.06), (fi, 0.30), (fi + 1, 0.62), (fi + 2, 0.84),
                 (fi + 5, 1.02), (f1, 1.18)):
        dx.scale = (k, k, k * 0.78)
        dx.keyframe_insert("scale", frame=f)
    for f, z in ((fi, 0.0), (f1, 0.075)):
        dx.location = cen + V((0, 0, z))
        dx.keyframe_insert("location", frame=f)

    cam = cam_at(C, off, c0, 0.185, az=52, el=22, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    p0 = cam.location.copy()
    keys = [(f0, (0.0, 0.0, 0.172), (0.0, 0.0, 0.0)),
            (f0 + 4, (0.0, 0.010, 0.150), (0.004, -0.003, -0.003)),
            (fi, (0.0, 0.085, 0.075), (0.012, 0.010, -0.014)),
            (fi + 3, (0.0, 0.150, 0.032), (0.020, 0.026, -0.024)),
            (f1, (0.0, 0.172, 0.022), (0.024, 0.038, -0.030))]
    for f, tgt, dp in keys:
        L.key_cam(cam, f, loc=p0 + V(dp), target=off + V(tgt), roll=0.0)
    return cam


# ================================================================ 16
def s16(off, C, f0, f1):
    """尘浪卷起，摄影机被卷入粒子云；失重翻滚，画面变成流动的灰白。"""
    c, s = (0.0, 0.0, 0.26), 0.46
    cen = off + V((0.0, 0.0, 0.30))
    sp = bpy.data.objects.new(C + "_spin", None)
    sp.location = cen
    sp.rotation_euler = (rd(-22), 0, 0)
    L.link(sp, C)

    rr = random.Random(1601)
    hot, soft, dark, puff = [], [], [], []

    def curl(t):
        return 3.4 * pi * t

    for a in range(3):
        for i in range(320):
            t = i / 319.0
            th = a * 2 * pi / 3 + curl(t)
            rad = 0.040 + 0.55 * t
            j = 0.018 + 0.070 * t
            p = cen + V((rad * cos(th) + rr.gauss(0, j), rad * sin(th) + rr.gauss(0, j),
                         rr.gauss(0, 0.030 + 0.190 * t)))
            u = rr.random()
            if u < 0.24:
                hot.append((p, rr.uniform(0.0010, 0.0026)))
            elif u < 0.72:
                soft.append((p, rr.uniform(0.0014, 0.0042)))
            else:
                dark.append((p, rr.uniform(0.0018, 0.0058)))
    # the wave: a rising curtain just outside the camera path
    for i in range(420):
        th = rr.uniform(0, 2 * pi)
        rad = 0.40 + rr.uniform(0.0, 0.30)
        p = cen + V((rad * cos(th) * 1.10, rad * sin(th) * 0.88, rr.uniform(-0.34, 0.28)))
        (soft if i % 2 else dark).append((p, rr.uniform(0.0016, 0.0044)))
    # near-camera puffs: read as out-of-focus bokeh, very dim
    for i in range(90):
        th = rr.uniform(0, 2 * pi)
        rad = rr.uniform(0.14, 0.62)
        p = cen + V((rad * cos(th), rad * sin(th), rr.uniform(-0.24, 0.26)))
        puff.append((p, rr.uniform(0.006, 0.015)))
    # flowing streams: dense dotted spirals read as motion at 12 fps
    for k in range(12):
        th0 = k * 2 * pi / 12 + rr.uniform(-0.22, 0.22)
        for i in range(70):
            t = i / 69.0
            th = th0 + 2.3 * pi * t
            rad = 0.09 + 0.50 * t
            p = cen + V((rad * cos(th), rad * sin(th), rr.gauss(0, 0.010)))
            hot.append((p, rr.uniform(0.0012, 0.0030)))

    grains = {'h': [], 's': [], 'd': []}
    for k, items in (('h', hot), ('s', soft), ('d', dark)):
        for p, rad in items:
            ax = V((rr.gauss(0, 1), rr.gauss(0, 1), rr.gauss(0, 1))).normalized()
            grains[k].append((p, rad * rr.uniform(1.2, 3.4), rad * 0.5, ax, rr.uniform(0, pi)))
    for k, mat in (('h', mt("S16_HOT", (1.0, 0.99, 0.96), 2.4)),
                   ('s', mt("S16_SOFT", (0.80, 0.80, 0.755), 1.0)),
                   ('d', mt("S16_DARK", (0.24, 0.24, 0.225), 0.95))):
        bind(splinters(C, C + "_gs" + k, grains[k], mat), sp, cen)
    cam_end = V((18.30, -0.30, 0.34))
    pmd = puff_mat("G4_S16_PD", (0.50, 0.50, 0.48), 1.0, 0.20, 0.50)
    pmb = puff_mat("G4_S16_PB", (0.92, 0.92, 0.885), 1.0, 0.50, 0.50)
    pmh = puff_mat("G4_S16_PH", (1.0, 1.0, 0.98), 2.2, 0.55, 0.34)
    ax = V((0, -0.375, 0.927))
    for i in range(4200):
        th = rr.uniform(0, 2 * pi)
        rad = 0.06 + 0.82 * rr.random() ** 0.55
        pz = cam_end + V((rad * cos(th), rad * sin(th), rr.gauss(0, 0.30)))
        u = rr.random()
        sz = rr.uniform(0.0007, 0.0026)
        if u < 0.30:
            hot.append((pz, sz * 0.8))
        elif u < 0.74:
            soft.append((pz, sz))
        else:
            dark.append((pz, sz * 1.4))
    for i in range(10):
        d = V((rr.gauss(0, 1), rr.gauss(0, 1), rr.gauss(0, 1))).normalized()
        p = cam_end + d * rr.uniform(0.30, 0.80)
        puff_quad(C, C + "_ph%d" % i, p, rr.uniform(0.05, 0.13), rr.uniform(0, pi),
                  pmh, cam_end, sp, cen)
    pmv = puff_mat("G4_S16_PV", (0.60, 0.60, 0.575), 1.0, 0.34, 0.50)
    for i, (dz, sx) in enumerate(((0.42, 1.70), (0.95, 2.60))):
        pv = cam_end + V((0.06 * (i + 1), -dz, 0.05 * (i - 1)))
        puff_quad(C, C + "_pv%d" % i, pv, sx, 0.0, pmv, pv + V((0, 3.0, 0)))

    # streaks = motion-blurred dust, the "flowing grey-white"
    stk = []
    for k in range(300):
        t = rr.random() ** 0.8
        th = rr.uniform(0, 2 * pi)
        rad = 0.06 + 0.56 * t
        th += 0.7 * pi * t                       # sheared along the swirl
        stk.append((cen + V((rad * cos(th), rad * sin(th), rr.gauss(0, 0.05 + 0.06 * t))),
                    rr.uniform(0.012, 0.060), rr.uniform(0.0004, 0.0011),
                    th + pi * 0.5))
    bind(streaks(C, C + "_streak", stk, mt("S16_STRK", (0.62, 0.62, 0.59), 1.0)), sp, cen)

    sp.keyframe_insert("rotation_euler", frame=f0)
    sp.rotation_euler = (rd(-22), 0, rd(26))
    sp.keyframe_insert("rotation_euler", frame=(f0 + f1) // 2)
    sp.rotation_euler = (rd(-22), 0, rd(54))
    sp.keyframe_insert("rotation_euler", frame=f1)
    sp.scale = (0.84, 0.84, 0.84)
    sp.keyframe_insert("scale", frame=f0)
    sp.scale = (1.10, 1.10, 1.10)
    sp.keyframe_insert("scale", frame=f1)
    for fc in fcurves_of(sp):
        for kp in fc.keyframe_points:
            kp.interpolation = 'LINEAR'

    cam = cam_at(C, off, c, s, az=26, el=24, fill=0.85, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=26, el=24, fill=0.85, kind='roll',
         az2=40, el2=5, fill2=3.0, ease='IO')
    fog_card(cam, C, "COL_16_FOG", 0.26, 1.05, (0.72, 0.72, 0.695), 0.95, f0, f1,
             amax=0.60)
    return cam
