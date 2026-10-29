# -*- coding: utf-8 -*-
"""Scale-aware stage builders. Every shot: subject center + size, rig computes camera distance and light power."""
import bpy, bmesh, math, random
from mathutils import Vector, Euler, Matrix
from math import radians as rd, sin, cos, pi
import pvlib as L

V = Vector
SENSOR_HALF = 10.1          # mm, vertical half-sensor (16:9) -> 'size' fills frame height
MAT = {}


def M(key):
    if key in MAT:
        return MAT[key]
    C = {
        'paper':  (0.906, 0.906, 0.882),
        'paper2': (0.851, 0.847, 0.808),
        'ink':    (0.126, 0.133, 0.122),
        'moss':   (0.467, 0.482, 0.376),
        'alu':    (0.725, 0.725, 0.706),
        'alu2':   (0.541, 0.541, 0.525),
        'mint':   (0.561, 0.890, 0.733),
        'orange': (0.910, 0.384, 0.165),
        'white':  (1.0, 1.0, 1.0),
        'water':  (0.812, 0.910, 0.878),
    }
    if key == 'vinyl':
        m = flat_rings("F_VINYL", (0.020, 0.024, 0.022), (0.086, 0.098, 0.090), rings=170)
    elif key == 'vinyl2':
        m = flat_rings("F_VINYL2", (0.020, 0.024, 0.022), (0.070, 0.080, 0.074), rings=80)
    elif key == 'glass':
        m = flat("F_GLASS", C['paper'], fac=0.85, alpha=0.28)
    elif key == 'water':
        m = flat("F_WATER", C['water'], fac=0.9, alpha=0.55)
    else:
        m = flat("F_" + key.upper(), C['white' if key == 'white_e' else key], fac=0.30 if key not in ('mint', 'orange', 'white_e') else 0.75)
    MAT[key] = m
    return m


def _emis_node(nt, color, strength=1.0):
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1.0)
    em.inputs["Strength"].default_value = strength
    return em


def flat(name, color, fac=0.55, strength=1.0, alpha=1.0):
    """Flat 'paper-cut' emission material: colour + a soft facing term for form. No lights needed."""
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    lw = nt.nodes.new("ShaderNodeLayerWeight")
    lw.inputs["Blend"].default_value = 0.35
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (*color, 1)
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = (*[c * fac for c in color], 1)
    nt.links.new(lw.outputs["Facing"], ramp.inputs["Fac"])
    em = _emis_node(nt, (1, 1, 1), strength)
    nt.links.new(ramp.outputs["Color"], em.inputs["Color"])
    if alpha < 1.0:
        mix = nt.nodes.new("ShaderNodeMixShader")
        tr = nt.nodes.new("ShaderNodeBsdfTransparent")
        mix.inputs["Fac"].default_value = alpha
        nt.links.new(tr.outputs["BSDF"], mix.inputs[1])
        nt.links.new(em.outputs["Emission"], mix.inputs[2])
        nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])
        m.surface_render_method = 'BLENDED'
    else:
        nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return m


def flat_rings(name, dark, light, rings=150.0, fac=0.5):
    """Vinyl: flat emission whose colour is modulated by concentric rings."""
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    wv = nt.nodes.new("ShaderNodeTexWave")
    wv.wave_type = 'RINGS'
    wv.bands_direction = 'DIAGONAL'
    wv.inputs["Scale"].default_value = rings / 100.0
    wv.inputs["Distortion"].default_value = 0.0
    wv.inputs["Detail"].default_value = 0.0
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.30
    ramp.color_ramp.elements[0].color = (*dark, 1)
    ramp.color_ramp.elements[1].position = 0.62
    ramp.color_ramp.elements[1].color = (*light, 1)
    nt.links.new(tc.outputs["Object"], wv.inputs["Vector"])
    nt.links.new(wv.outputs["Fac"], ramp.inputs["Fac"])
    em = _emis_node(nt, (1, 1, 1), 1.0)
    nt.links.new(ramp.outputs["Color"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return m


# ---------- rig ----------

def key_at(C, off, c, size, **kw):
    return None



def area_at(C, off, c, size, **kw):
    return None


def cam_at(C, off, c, size, az=28.0, el=16.0, fill=0.85, lens=50.0, dof_k=None, fstop=2.8,
           roll=0.0, ortho=None, clip=None):
    dist = size * (lens / SENSOR_HALF) / max(fill, 0.05)
    a, e = rd(az), rd(el)
    p = off + V(c) + V((sin(a) * cos(e), -cos(a) * cos(e), sin(e))) * dist
    dof = None if dof_k is None else dof_k * dist
    cam = L.cam(C + "_cam", C, p, off + V(c), lens=lens, dof=dof, fstop=fstop, roll=roll, ortho=ortho)
    cam.data.clip_start = clip if clip else max(0.0015, dist * 0.02)
    cam.data.clip_end = 800.0
    cam["dist"] = dist
    cam["off"] = list(off)
    cam["center"] = list(c)
    cam["size"] = size
    return cam


def _pos(c, size, az, el, dist):
    a, e = rd(az), rd(el)
    return V(c) + V((sin(a) * cos(e), -cos(a) * cos(e), sin(e))) * dist


def move(cam, f0, f1, c, size, az=28.0, el=16.0, fill=0.85, kind='static',
         az2=None, el2=None, fill2=None, lens_follow=True, ease='IO'):
    """kind: static / push / pull / slide / orbit / rise / fall / roll"""
    off = V(cam.get("off", (0.0, 0.0, 0.0)))
    d0 = cam.get("dist")
    az2 = az if az2 is None else az2
    el2 = el if el2 is None else el2
    fill2 = fill if fill2 is None else fill2
    d1 = size * (cam.data.lens / SENSOR_HALF) / max(fill2, 0.05)
    for f, u in ((f0, 0.0), (f1, 1.0)):
        if ease == 'IO':
            uu = u * u * (3 - 2 * u)
        elif ease == 'IN':
            uu = u * u * u
        elif ease == 'OUT':
            uu = 1 - (1 - u) ** 3
        else:
            uu = u
        a = az + (az2 - az) * uu
        e = el + (el2 - el) * uu
        dd = d0 + (d1 - d0) * uu
        p = off + _pos(V(c), size, a, e, dd)
        roll = 0.0
        if kind == 'roll':
            roll = rd(-12 + 24 * uu)
        L.key_cam(cam, f, loc=p, target=off + V(c), roll=roll)
    return cam


def dust(C, off, c, size, n=26, spread_k=0.9, r_k=0.010, mat='white_e', seed=1):
    rr = random.Random(seed)
    pts = []
    for _ in range(n):
        pts.append(off + V(c) + V((rr.gauss(0, size * spread_k), rr.gauss(0, size * spread_k),
                                   rr.gauss(0, size * spread_k))))
    return points_mesh(C, C + "_dust", pts, size=max(0.0006, size * r_k), mat=mat, sub=1)


def points_mesh(C, name, pts, size=0.002, mat='white_e', sub=1):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for p in pts:
        bmesh.ops.create_icosphere(bm, subdivisions=sub, radius=size, matrix=Matrix.Translation(p))
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(M(mat))
    L.link(ob, C)
    return ob


def box(C, name, c, dim, mat='paper', off=None, bevel=0.0, rot=None):
    o = off if off is not None else V((0, 0, 0))
    b = L.prim('cube', C, name, loc=o + V(c), scale=tuple(dim), material=M(mat))
    if rot:
        b.rotation_euler = rot
    if bevel:
        L.bevel(b, bevel, 2)
    return b


def disc(C, name, c, r, off, mat='vinyl', thick=None, tilt=None, spin=None):
    t = thick if thick else r * 0.022
    d = L.prim('cyl', C, name, loc=off + V(c), r=r, d=t, material=M(mat))
    L.smooth(d)
    L.bevel(d, min(0.0015, t * 0.3), 2)
    if tilt:
        d.rotation_euler = (rd(tilt), 0, 0)
    return d


def torus(C, name, c, R_, r_, off, mat='mint', rot=None, mv=128):
    return L.prim('torus', C, name, loc=off + V(c), R=R_, r=r_, material=M(mat), rot=rot or (0, 0, 0), mv=mv, nv=6)


# ================= 01 =================
def s01(off, C, f0, f1):
    c, s = (0, 0, 0.055), 0.075
    sh = L.prim('cube', C, C + "_shaft", loc=off + V((0, 0, 0.075)), scale=(0.0016, 0.0016, 0.10),
                material=L.mat_shaft("SH_W", (0.88, 0.93, 1.0), strength=3.0, alpha=0.55))
    sh.rotation_euler = (rd(6), 0, rd(-3))
    L.prim('cyl', C, C + "_pool", loc=off + V((0.004, 0.001, 0.0008)), r=0.016, d=0.0012,
           material=M('white_e'))
    dust(C, off, (0, 0, 0.05), s, n=14, r_k=0.006, seed=3)
    L.prim('ico', C, C + "_hero", loc=off + V((0.003, -0.004, 0.040)), r=0.0009, sub=2, material=M('white_e'))
    key_at(C, off, c, s, mult=0.5)
    cam = cam_at(C, off, c, s, az=26, el=7, fill=0.85, lens=100, dof_k=0.55, fstop=2.4)
    move(cam, f0, f1, c, s, az=26, el=7, fill=0.85, kind='slide', az2=34, fill2=0.80)
    return cam


# ================= 02 =================
def s02(off, C, f0, f1):
    c, s = (0.10, 0, 0.0), 0.085
    disc(C, C + "_disc", (0, 0, 0), 0.16, off)
    torus(C, C + "_rim", (0, 0, 0.0006), 0.159, 0.0016, off, 'alu')
    area_at(C, off, c, s, dirv=(-0.5, -1.0, 0.55), mult=2.2, sx=0.06, ratio=2.2)
    dust(C, off, c, s, n=18, r_k=0.010, seed=11)
    cam = cam_at(C, off, c, s, az=-6, el=34, fill=1.05, lens=100, dof_k=0.45, fstop=2.8)
    cam.data.dof.focus_distance = 0.30 * 4
    move(cam, f0, f1, c, s, az=-6, el=34, fill=1.05, kind='slide', az2=4, el2=30)
    return cam


# ================= 03 =================
def s03(off, C, f0, f1):
    c, s = (0.0, 0.0, 0.0), 0.048
    d = disc(C, C + "_disc", (0, 0, 0), 0.16, off)
    L.prim('cube', C, C + "_scr", loc=off + V((-0.02, 0.015, 0.0024)), scale=(0.09, 0.0005, 0.0003),
           material=M('alu2'), rot=(0, 0, rd(24)))
    area_at(C, off, c, s, dirv=(0.4, -0.7, 0.7), mult=1.6, sx=1.6, ratio=2.4)
    cam = cam_at(C, off, c, s, az=0, el=56, fill=1.0, lens=100, dof_k=1.0, fstop=2.2)
    d.rotation_euler = (0, 0, 0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(90))
    d.keyframe_insert("rotation_euler", frame=f1)
    move(cam, f0, f1, c, s, az=0, el=56, fill=1.0, kind='orbit', az2=-10, el2=50)
    return cam


# ================= 04 =================
def s04(off, C, f0, f1):
    c, s = (0, 0, 0.05), 0.055
    cr = L.prim('cyl', C, C + "_hex", loc=off + V(c), r=0.028, d=0.075, v=6, material=M('glass'))
    L.smooth(cr)
    L.bevel(cr, 0.0018, 3)
    for i, (dx, dy, m) in enumerate(((-0.045, 0.010, 'mint'), (0.050, -0.005, 'orange'), (0.0, 0.055, 'moss'))):
        L.prim('plane', C, C + "_bg%d" % i, loc=off + V((dx, dy, 0.05)), rot=(rd(90), 0, rd(30 * i)),
               size=0.02, material=M(m))
    key_at(C, off, c, s, dirv=(-0.9, -1.0, 0.8), mult=0.8)
    dust(C, off, c, s, n=14, r_k=0.006, seed=5)
    cam = cam_at(C, off, c, s, az=22, el=12, fill=0.85, lens=100, dof_k=0.9, fstop=2.6)
    move(cam, f0, f1, c, s, az=22, el=12, fill=0.85, kind='slide', az2=6, el2=18)
    return cam


# ================= 05 =================
def s05(off, C, f0, f1):
    c, s = (0, 0, 0.09), 0.24
    for i in range(15):
        disc(C, C + "_st%02d" % i, (0, 0, i * 0.0105), 0.150, off, 'vinyl' if i % 2 else 'vinyl2')
    dust(C, off, c, s, n=60, r_k=0.008, seed=9)
    key_at(C, off, c, s, dirv=(-1.0, -1.1, 1.3), mult=1.1, cone=34)
    cam = cam_at(C, off, c, s, az=128, el=9, fill=1.15, lens=50, dof_k=0.7, fstop=4.0)
    move(cam, f0, f1, c, s, az=128, el=9, fill=1.15, kind='pull', fill2=2.6, ease='IN')
    return cam


# ================= 06a =================
def s06a(off, C, f0, f1):
    c, s = (0.018, 0.0, 0.006), 0.020
    disc(C, C + "_disc", (0, 0, 0), 0.16, off)
    nd = L.prim('cone', C, C + "_nd", loc=off + V((0.018, 0, 0.012)), r1=0.0016, r2=0.00012, d=0.016, v=20,
                material=M('alu'))
    nd.rotation_euler = (rd(16), 0, 0)
    key_at(C, off, c, s, dirv=(-0.9, -1.0, 1.2), mult=0.9, cone=40)
    cam = cam_at(C, off, c, s, az=8, el=14, fill=0.8, lens=100, dof_k=0.9, fstop=2.0)
    nd.location = off + V((0.018, 0, 0.013))
    nd.keyframe_insert("location", frame=f0)
    nd.location = off + V((0.018, 0, 0.0028))
    nd.keyframe_insert("location", frame=f1)
    return cam


# ================= 06b =================
def s06b(off, C, f0, f1):
    c, s = (0, 0, 0.010), 0.022
    box(C, C + "_plate", (0, 0, 0), (0.5, 0.5, 0.008), 'vinyl2', off, bevel=0.002)
    for i in range(3):
        box(C, C + "_gr%d" % i, (-0.03 + i * 0.03, 0, 0.0045), (0.0009, 0.4, 0.0012), 'alu2', off)
    area_at(C, off, c, s, dirv=(-0.9, -0.2, 0.5), mult=2.4, sx=0.05, ratio=2.0)
    cam = cam_at(C, off, c, s, az=-4, el=16, fill=0.9, lens=100, dof_k=1.4, fstop=2.0)
    move(cam, f0, f1, c, s, az=-4, el=16, fill=0.9, kind='slide', az2=6)
    return cam


# ================= 06c =================
def s06c(off, C, f0, f1):
    c, s = (0, 0, 0), 0.17
    disc(C, C + "_disc", (0, 0, 0), 0.16, off)
    for i in range(9):
        box(C, C + "_gx%d" % i, (0, -0.10 + i * 0.025, 0.0024), (0.26, 0.0006, 0.00035), 'mint', off)
        box(C, C + "_gy%d" % i, (-0.10 + i * 0.025, 0, 0.0024), (0.0006, 0.26, 0.00035), 'mint', off)
    area_at(C, off, c, s, dirv=(-0.9, -0.4, 0.55), mult=1.6, sx=0.10, ratio=2.0)
    cam = cam_at(C, off, c, s, az=-24, el=36, fill=1.15, lens=25, dof_k=1.0, fstop=4.0)
    move(cam, f0, f1, c, s, az=-24, el=36, fill=1.15, kind='push', fill2=0.7, ease='OUT')
    return cam


# ================= 07 =================
def chladni_pts(m, n, count, Rmax=0.145, tol=0.06, seed=2):
    rr = random.Random(seed)
    pts = []
    guard = 0
    while len(pts) < count and guard < count * 500:
        guard += 1
        r = Rmax * math.sqrt(rr.random())
        th = rr.uniform(0, 2 * pi)
        v = cos(n * th) * cos(m * pi * r / Rmax) - cos(m * th) * cos(n * pi * r / Rmax)
        if abs(v) < tol:
            pts.append((r * cos(th), r * sin(th)))
    return pts


def s07(off, C, f0, f1):
    c, s = (0, 0, 0.004), 0.155
    d = disc(C, C + "_disc", (0, 0, 0), 0.15, off)
    pts = chladni_pts(3, 5, 300)
    points_mesh(C, C + "_sand", [off + V((x, y, 0.0034)) for (x, y) in pts], size=0.0011, mat='paper', sub=1)
    key_at(C, off, c, s, dirv=(-0.4, -0.6, 1.4), mult=1.3, cone=42)
    cam = cam_at(C, off, c, s, az=0, el=62, fill=1.05, lens=100, dof_k=1.2, fstop=4.0)
    move(cam, f0, f1, c, s, az=0, el=62, fill=1.05, kind='push', fill2=0.92)
    d.rotation_euler = (0, 0, 0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(22))
    d.keyframe_insert("rotation_euler", frame=f1)
    return cam


# ================= 08 =================
def s08(off, C, f0, f1):
    c, s = (0, 0, 0.55), 0.60
    for i in range(18):
        rr = 0.150 * (1 - i / 19.0)
        torus(C, C + "_r%02d" % i, (0, 0, i * 0.10), rr, 0.0006 + 0.0004 * i, off,
              'paper2' if i < 7 else 'moss', mv=64)
    L.prim('cyl', C, C + "_hole", loc=off + V((0, 0, 1.85)), r=0.02, d=0.004, material=M('white_e'))
    key_at(C, off, c, s, dirv=(-0.5, -0.8, 1.2), mult=1.0, cone=45)
    cam = cam_at(C, off, c, s, az=0, el=6, fill=1.5, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=0, el=6, fill=1.5, kind='push', fill2=0.30, ease='IN')
    return cam


# ================= 09 =================
def archive(C, off, nx=9, ny=16, pitch=0.42, h=0.30):
    x0 = -(nx - 1) * pitch / 2
    bx = box(C, C + "_boxes", (x0, 0, 0), (0.30, 0.30, h), 'paper', off, bevel=0.004)
    def arr(ob):
        sx, sy = ob.scale[0], ob.scale[1]
        for axis, cnt, sc in (('X', nx, sx), ('Y', ny, sy)):
            m = ob.modifiers.new("a" + axis, 'ARRAY')
            m.count = cnt
            m.use_relative_offset = False
            m.use_constant_offset = True
            m.constant_offset_displace = (pitch / sc, 0, 0) if axis == 'X' else (0, pitch / sc, 0)
    arr(bx)
    lab = box(C, C + "_labels", (x0, -0.151, 0.02), (0.10, 0.0008, 0.012), 'ink', off)
    arr(lab)
    tick = box(C, C + "_ticks", (x0 + 0.10, -0.151, 0.02), (0.012, 0.0008, 0.012), 'moss', off)
    arr(tick)
    slit = box(C, C + "_slits", (x0, -0.152, -0.035), (0.16, 0.0006, 0.0035), 'mint', off)
    arr(slit)
    return bx


def s09(off, C, f0, f1):
    c, s = (0, 3.0, 0.15), 2.4
    archive(C, off)
    area_at(C, off, c, s, dirv=(0, -1.0, 1.6), mult=1.5, sx=2.0, ratio=2.2)
    cam = cam_at(C, off, c, s, az=-2, el=7, fill=1.25, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-2, el=7, fill=1.25, kind='push', fill2=0.16, ease='LIN')
    return cam


# ================= 10 =================
def s10(off, C, f0, f1):
    c, s = (0, 0.9, 0.03), 0.9
    archive(C, off, nx=6, ny=8)
    area_at(C, off, c, s, dirv=(1.2, -1.0, 1.4), mult=1.4, sx=1.6, ratio=2.4)
    cam = cam_at(C, off, c, s, az=-78, el=6, fill=1.1, lens=35, dof_k=1.2, fstop=4.0)
    move(cam, f0, f1, c, s, az=-78, el=6, fill=1.1, kind='slide', az2=-104)
    return cam


# ================= 11 =================
def s11(off, C, f0, f1):
    c, s = (0, 0, 0), 0.17
    box(C, C + "_box", (0, 0, 0), (0.34, 0.34, 0.34), 'paper', off, bevel=0.004)
    for i, m in enumerate(('water', 'mint', 'orange', 'paper')):
        p = L.prim('plane', C, C + "_scr%d" % i, loc=off + V((0, -0.1715, 0)), rot=(rd(90), 0, 0),
                   size=0.30, material=M(m))
    key_at(C, off, c, s, dirv=(-0.8, -1.2, 0.7), mult=1.1, cone=36)
    cam = cam_at(C, off, c, s, az=6, el=9, fill=0.95, lens=50, dof_k=1.2, fstop=4.0)
    n = f1 - f0
    for i in range(4):
        fa = f0 + int(n * i / 4)
        fb = f0 + int(n * (i + 1) / 4) - 1
        ob = bpy.data.objects[C + "_scr%d" % i]
        for f, vis in ((fa, False), (fb, True)):
            ob.hide_render = vis
            ob.keyframe_insert("hide_render", frame=f)
    return cam


# ================= 12 =================
def s12(off, C, f0, f1):
    c, s = (0, 0, 1.05), 1.5
    col = box(C, C + "_col", (0, 0, 0.9), (0.34, 0.34, 1.7), 'paper', off, bevel=0.006)
    for k in range(7):
        torus(C, C + "_r%02d" % k, (0, 0, 1.80 + k * 0.018), 0.09 + 0.03 * k, 0.003, off, 'mint', mv=96)
    for k in range(5):
        L.prim('ico', C, C + "_p%d" % k, loc=off + V((0.05 * cos(k * 1.7), 0.05 * sin(k * 1.7), 1.86)),
               r=0.010, sub=2, material=M('white_e'))
    key_at(C, off, c, s, dirv=(-1.0, -1.0, 0.6), mult=1.0, cone=40)
    dust(C, off, (0, 0, 1.3), s * 0.5, n=40, r_k=0.004, seed=13)
    cam = cam_at(C, off, c, s, az=-32, el=-6, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-32, el=-6, fill=1.0, kind='rise', el2=12, fill2=0.8)
    return cam


# ================= 13 =================
def s13(off, C, f0, f1):
    c, s = (0, 0, 0), 3.2
    for k in range(10):
        torus(C, C + "_r%02d" % k, (0, 0, 0), 0.65 + k * 0.55, 0.05, off, 'paper2', mv=128)
    L.prim('cyl', C, C + "_hub", loc=off + V((0, 0, 0.02)), r=0.60, d=0.06, material=M('paper'))
    torus(C, C + "_hp", (0, 0, 0.05), 0.66, 0.014, off, 'mint', mv=128)
    area_at(C, off, c, s, dirv=(0.2, -1.0, 3.2), mult=1.6, sx=3.0, ratio=2.2)
    cam = cam_at(C, off, c, s, az=-90, el=74, fill=1.1, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-90, el=74, fill=1.1, kind='orbit', az2=-62, el2=68)
    return cam


# ================= 14 =================
def s14(off, C, f0, f1):
    c, s = (0, 0, 0.16), 0.155
    d = disc(C, C + "_disc", (0, 0, 0.16), 0.15, off, 'paper2', thick=0.003)
    d.rotation_euler = (rd(90), 0, 0)
    torus(C, C + "_rim", (0, 0.0018, 0.16), 0.150, 0.0011, off, 'mint', rot=(rd(90), 0, 0))
    L.area(C + "_key", C, loc=off + V((-0.75, 0.5, 0.28)), target=off + V((0, 0, 0.16)), size=0.5,
           power=90.0 * 0.7, color=(1, 1, 1))
    L.area(C + "_rim", C, loc=off + V((0.55, 0.60, 0.16)), target=off + V((0, 0, 0.16)), size=0.4,
           power=50.0, color=(0.56, 0.89, 0.73))
    cam = cam_at(C, off, c, s, az=0, el=2, fill=0.95, lens=100, dof_k=1.1, fstop=5.6)
    return cam


# ================= 15 =================
def s15(off, C, f0, f1):
    c, s = (0, 0, 0.10), 0.22
    d = disc(C, C + "_disc", (0, 0, 0.152), 0.15, off)
    d.rotation_euler = (rd(90), 0, 0)
    d.location = off + V((0, 0, 0.152))
    d.keyframe_insert("location", frame=f0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (rd(88), 0, 0)
    d.location = off + V((0, 0.03, 0.004))
    d.keyframe_insert("location", frame=f1)
    d.keyframe_insert("rotation_euler", frame=f1)
    box(C, C + "_floor", (0, 0, -0.002), (3, 3, 0.004), 'paper2', off)
    for i in range(24):
        a = 2 * pi * i / 24
        L.prim('ico', C, C + "_p%02d" % i, loc=off + V((0.20 * cos(a), 0.20 * sin(a), 0.012)),
               r=0.003, sub=1, material=M('paper'))
    key_at(C, off, c, s, dirv=(-1.0, -1.0, 1.2), mult=1.2, cone=36)
    cam = cam_at(C, off, c, s, az=34, el=10, fill=1.25, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=34, el=10, fill=1.25, kind='fall', el2=-4, fill2=1.05)
    return cam


# ================= 16 =================
def s16(off, C, f0, f1):
    c, s = (0, 0, 0.02), 0.20
    rr = random.Random(21)
    pts = [off + V((rr.gauss(0, s * 0.9), rr.gauss(0, s * 0.9), rr.gauss(0, s * 0.5))) for _ in range(260)]
    points_mesh(C, C + "_cloud", pts, size=0.0022, mat='paper', sub=1)
    area_at(C, off, c, s, dirv=(-1.2, -0.8, 1.4), mult=1.6, sx=1.6, ratio=2.4)
    cam = cam_at(C, off, c, s, az=10, el=4, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=10, el=4, fill=1.0, kind='roll', az2=-16, el2=8)
    return cam


# ================= 17 =================
def s17(off, C, f0, f1):
    c, s = (0, 0, 0), 0.055
    e = L.prim('sphere', C, C + "_eye", loc=off, r=0.055, material=M('paper'))
    L.smooth(e)
    for k in range(6):
        torus(C, C + "_ir%d" % k, (0, -0.0505, 0), 0.004 + k * 0.0032, 0.0006, off, 'ink', rot=(rd(90), 0, 0), mv=96)
    L.prim('cyl', C, C + "_pupil", r=0.0075, d=0.002, loc=off + V((0, -0.0525, 0)), rot=(rd(90), 0, 0),
           material=M('ink'))
    key_at(C, off, c, s, dirv=(-0.6, -1.2, 0.7), mult=0.9, cone=34)
    cam = cam_at(C, off, c, s, az=0, el=3, fill=0.95, lens=100, dof_k=1.2, fstop=2.8)
    move(cam, f0, f1, c, s, az=0, el=3, fill=0.95, kind='push', fill2=0.22, ease='IN')
    return cam


# ================= 18 =================
def s18(off, C, f0, f1):
    c, s = (0, 0, 1.7), 1.7
    for k in range(30):
        a = 2 * pi * k / 30
        z = k * 0.24
        h = 0.10 + 0.26 * abs(sin(k * 1.7))
        box(C, C + "_b%02d" % k, (0.90 * cos(a), 0.90 * sin(a), z), (0.012, 0.012, h), 'paper', off)
        box(C, C + "_c%02d" % k, (1.02 * cos(a), 1.02 * sin(a), z), (0.008, 0.008, h * 0.7), 'ink', off)
    area_at(C, off, c, s, dirv=(0, 0, 1.0), mult=1.2, sx=1.5, ratio=1.6)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=1.4, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=0, el=0, fill=1.4, kind='push', fill2=0.22, ease='LIN')
    return cam


# ================= 19 montage =================
def _m_glass(off, C, f0, f1):
    c, s = (0, 0, 0.055), 0.045
    L.prim('cyl', C, C + "_g", loc=off + V((0, 0, 0.045)), r=0.032, d=0.090, material=M('glass'))
    L.prim('cyl', C, C + "_w", loc=off + V((0, 0, 0.058)), r=0.030, d=0.040, material=M('water'))
    for k in range(6):
        torus(C, C + "_r%d" % k, (0, 0, 0.0795), 0.004 + k * 0.0042, 0.0005, off, 'paper', mv=64)
    area_at(C, off, c, s, dirv=(0.2, -1.0, 1.2), mult=1.4, sx=1.2)
    return cam_at(C, off, c, s, az=4, el=22, fill=1.0, lens=100, dof_k=1.0, fstop=2.4)


def _m_needle(off, C, f0, f1):
    c, s = (0, 0, 0.010), 0.020
    L.prim('cone', C, C + "_n", loc=off + V((0, 0, 0.012)), r1=0.0011, r2=0.00008, d=0.024, v=24, material=M('alu'))
    L.prim('cyl', C, C + "_s", loc=off + V((0, 0, 0.028)), r=0.0030, d=0.012, material=M('alu2'))
    area_at(C, off, c, s, dirv=(-1.0, -0.3, 0.5), mult=2.6, sx=0.05, ratio=2.0)
    return cam_at(C, off, c, s, az=-8, el=10, fill=0.85, lens=100, dof_k=1.2, fstop=2.0)


def _m_hand(off, C, f0, f1):
    c, s = (0, 0, 0.010), 0.10
    disc(C, C + "_d", (0, 0, 0.004), 0.11, off)
    for k in range(3):
        f = box(C, C + "_f%d" % k, (-0.02 + k * 0.028, -0.040, 0.014), (0.016, 0.100, 0.009), 'paper', off, bevel=0.003)
        f.rotation_euler = (0, 0, rd(-10 + k * 7))
    area_at(C, off, c, s, dirv=(0.8, 0.6, 1.2), mult=1.8, sx=1.2)
    return cam_at(C, off, c, s, az=22, el=40, fill=1.0, lens=100, dof_k=1.0, fstop=2.8)


def _m_scope(off, C, f0, f1):
    c, s = (0, 0, 0), 0.16
    box(C, C + "_scr", (0, 0, 0), (0.24, 0.0016, 0.16), 'ink', off)
    for i in range(110):
        x = -0.10 + 0.20 * i / 109.0
        z = 0.028 * sin(i * 0.50) * sin(i * 0.13)
        box(C, C + "_p%03d" % i, (x, -0.0018, z), (0.0018, 0.0006, 0.0018), 'mint', off)
    area_at(C, off, c, s, dirv=(0, -1.0, 0.2), mult=1.2, sx=1.4)
    return cam_at(C, off, c, s, az=0, el=2, fill=1.0, lens=50, dof_k=1.2, fstop=4.0)


def _m_blink(off, C, f0, f1):
    c, s = (0, 0, 0), 0.050
    e = L.prim('sphere', C, C + "_e", loc=off, r=0.050, material=M('paper'))
    L.smooth(e)
    lid = L.prim('sphere', C, C + "_l", loc=off + V((0, -0.002, 0.030)), r=0.0505, material=M('paper2'))
    L.smooth(lid)
    key_at(C, off, c, s, dirv=(-0.5, -1.2, 0.6), mult=0.9)
    return cam_at(C, off, c, s, az=0, el=4, fill=0.95, lens=100, dof_k=1.1, fstop=2.8)


def _m_tape(off, C, f0, f1):
    c, s = (0, 0, 0.03), 0.045
    rr = random.Random(4)
    for k in range(5):
        torus(C, C + "_t%d" % k, (rr.uniform(-0.01, 0.01), rr.uniform(-0.01, 0.01), 0.03 + rr.uniform(-0.01, 0.01)),
              0.026 + 0.005 * k, 0.0018, off, 'ink',
              rot=(rd(50 + 40 * k), rd(25 * k), rd(35 * k)), mv=64)
    area_at(C, off, c, s, dirv=(-0.6, -1.0, 0.8), mult=1.6, sx=1.0)
    return cam_at(C, off, c, s, az=14, el=16, fill=0.95, lens=100, dof_k=1.0, fstop=2.8)


def _m_city(off, C, f0, f1):
    c, s = (0, 0, 0.05), 0.16
    rr = random.Random(5)
    for i in range(24):
        x = (i % 12 - 5.5) * 0.028
        y = (i // 12) * 0.040
        h = 0.025 + 0.095 * abs(rr.gauss(0, 1))
        box(C, C + "_c%d" % i, (x, y, h * 0.5), (0.020, 0.020, h), 'ink', off)
        box(C, C + "_t%d" % i, (x, y, h + 0.0015), (0.017, 0.017, 0.0028), 'mint', off)
    area_at(C, off, c, s, dirv=(0, -1.0, 1.4), mult=1.6, sx=1.4)
    return cam_at(C, off, c, s, az=6, el=8, fill=1.05, lens=50, dof_k=None, fstop=8.0)


def _m_drop(off, C, f0, f1):
    c, s = (0, 0, 0.012), 0.07
    disc(C, C + "_d", (0, 0, 0), 0.10, off)
    L.prim('ico', C, C + "_w", loc=off + V((0, 0, 0.026)), r=0.0035, sub=3, material=M('water'))
    for k in range(7):
        torus(C, C + "_r%d" % k, (0, 0, 0.0016), 0.004 + k * 0.006, 0.0005, off, 'mint', mv=64)
    key_at(C, off, c, s, dirv=(-0.5, -1.0, 1.6), mult=1.1, cone=38)
    return cam_at(C, off, c, s, az=0, el=34, fill=1.0, lens=100, dof_k=1.1, fstop=2.8)


def _m_open(off, C, f0, f1):
    c, s = (0, 0, 0.03), 0.19
    for i, x in enumerate((-0.16, 0.0, 0.16)):
        box(C, C + "_b%d" % i, (x, 0, 0.03), (0.13, 0.13, 0.055), 'paper', off)
        lid = box(C, C + "_l%d" % i, (x, -0.062, 0.070), (0.13, 0.13, 0.007), 'paper2', off)
        lid.rotation_euler = (rd(-52), 0, 0)
        box(C, C + "_g%d" % i, (x, 0, 0.052), (0.10, 0.10, 0.0018), 'mint', off)
    area_at(C, off, c, s, dirv=(0, -0.5, 1.4), mult=1.3, sx=1.6)
    return cam_at(C, off, c, s, az=0, el=56, fill=1.15, lens=25, dof_k=None, fstop=8.0)


def _m_hole(off, C, f0, f1):
    c, s = (0, 0, 0.022), 0.05
    disc(C, C + "_d1", (0, 0, 0.018), 0.150, off)
    disc(C, C + "_d2", (0, 0, 0.0235), 0.150, off)
    L.prim('cyl', C, C + "_ring", r=0.008, d=0.008, loc=off + V((0, 0, 0.022)), material=M('alu'))
    rr = random.Random(31)
    pts = [off + V((rr.gauss(0, 0.010), rr.gauss(0, 0.010), rr.uniform(-0.22, -0.004))) for _ in range(200)]
    points_mesh(C, C + "_stars", pts, size=0.0006, mat='white_e', sub=1)
    area_at(C, off, c, s, dirv=(0.3, -1.0, 1.2), mult=1.6, sx=1.4)
    return cam_at(C, off, c, s, az=0, el=26, fill=0.9, lens=100, dof_k=1.2, fstop=2.2)


MONTAGE = [_m_glass, _m_needle, _m_hand, _m_scope, _m_blink, _m_tape, _m_city, _m_drop, _m_open, _m_hole]


def s19(off, C, f0, f1):
    out = []
    n = 10
    span = (f1 - f0 + 1) / float(n)
    for i, fn in enumerate(MONTAGE):
        sub = off + V((i * 12.0, 0, 0))
        cam = fn(sub, C + "_m%d" % i, f0, f1)
        fa = int(f0 + i * span)
        fb = int(f0 + (i + 1) * span) - 1
        L.key_cam(cam, fa, loc=cam.location.copy(),
                  target=cam.location + (cam.matrix_world.to_quaternion() @ V((0, 0, -1))))
        out.append((cam, fa, fb))
    return out


# ================= 20 =================
def s20(off, C, f0, f1):
    c, s = (0, 0, 0.30), 0.17
    d = disc(C, C + "_disc", (0, 0, 0.30), 0.15, off)
    torus(C, C + "_rim", (0, 0, 0.30), 0.150, 0.0014, off, 'mint')
    box(C, C + "_floor", (0, 0, -0.004), (6, 6, 0.004), 'ink', off)
    dust(C, off, c, s, n=50, r_k=0.010, seed=17)
    key_at(C, off, c, s, dirv=(-1.0, -1.0, 1.2), mult=1.2, cone=26)
    cam = cam_at(C, off, c, s, az=-72, el=8, fill=1.0, lens=50, dof_k=0.9, fstop=2.8)
    move(cam, f0, f1, c, s, az=-72, el=8, fill=1.0, kind='orbit', az2=-52, el2=6)
    return cam


# ================= 21 =================
def s21(off, C, f0, f1):
    c, s = (0, 0, 0.20), 0.55
    d = disc(C, C + "_disc", (0, 0, 0.20), 0.15, off)
    torus(C, C + "_rim", (0, 0, 0.20), 0.150, 0.0018, off, 'mint')
    for k in range(5):
        torus(C, C + "_b%d" % k, (0, 0, 0.20), 0.30 + k * 0.36, 0.0013 - 0.00015 * k, off, 'paper', mv=160)
    for i in range(9):
        box(C, C + "_gx%d" % i, (0, -1.4 + i * 0.35, 0.20), (2.6, 0.0010, 0.0010), 'mint', off)
        box(C, C + "_gy%d" % i, (-1.4 + i * 0.35, 0, 0.20), (0.0010, 2.6, 0.0010), 'mint', off)
    key_at(C, off, c, s, dirv=(-0.8, -1.2, 1.2), mult=1.1, cone=34)
    d.rotation_euler = (0, 0, 0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(140))
    d.keyframe_insert("rotation_euler", frame=f1)
    cam = cam_at(C, off, c, s, az=0, el=10, fill=1.0, lens=50, dof_k=1.0, fstop=2.8)
    move(cam, f0, f1, c, s, az=0, el=10, fill=1.0, kind='pull', fill2=3.4)
    return cam


# ================= 22 / 23 UI =================
def ui_panel(C, off, w=1.60, h=0.90, z=0.30):
    def bar(x, y, sx, sy, m='paper'):
        return box(C, C + "_u%d" % (int(abs(x * 1e4 + y * 977)) % 99991), (x, 0, z + y), (sx, 0.0014, sy), m, off)
    bar(0.0, h / 2, w, 0.0016)
    bar(0.0, h / 2 - 0.055, w, 0.0005, 'moss')
    bar(0.0, -h / 2, w, 0.0016)
    bar(-w / 2, 0, 0.0016, h)
    bar(w / 2, 0, 0.0016, h)
    for i in range(1, 4):
        bar(0.0, h / 2 - 0.055 - i * 0.20, w, 0.0005, 'moss')
    for i in range(4):
        bar(-w / 2 + 0.13, h / 2 - 0.13 - i * 0.16, 0.20, 0.0026)
    for r in range(4):
        for k in range(6):
            x = -0.08 + k * 0.27
            bar(x, h / 2 - 0.30 - r * 0.15, 0.19 * (0.45 + 0.55 * ((k * 7 + r * 3) % 5) / 4.0), 0.0060,
                'moss' if (k + r) % 3 == 0 else 'ink')


def s22(off, C, f0, f1):
    c, s = (0, 0, 0.30), 0.85
    ui_panel(C, off)
    box(C, C + "_floor", (0, 0, -0.02), (6, 6, 0.004), 'ink', off)
    area_at(C, off, c, s, dirv=(0, -1.0, 0.6), mult=1.3, sx=1.6)
    cam = cam_at(C, off, c, s, az=-4, el=2, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-4, el=2, fill=1.0, kind='slide', az2=6, ease='LIN')
    return cam


def s23(off, C, f0, f1):
    c, s = (0, 0, 0.30), 0.85
    ui_panel(C, off)
    box(C, C + "_floor", (0, 0, -0.02), (6, 6, 0.004), 'ink', off)
    area_at(C, off, c, s, dirv=(0, -1.0, 0.6), mult=1.5, sx=1.6)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=1.55, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=0, el=0, fill=1.55, kind='push', fill2=0.06, ease='LIN')
    return cam


# ================= 24 =================
def s24(off, C, f0, f1):
    c, s = (0, 2.0, 0.55), 2.6
    archive(C, off, nx=7, ny=13)
    disc(C, C + "_disc", (0, 3.4, 1.35), 0.15, off, 'mint')
    L.prim('cyl', C, C + "_hole", loc=off + V((0, 1.4, 0.62)), r=0.30, d=0.010, material=M('ink'))
    tri = L.prim('cone', C, C + "_play", loc=off + V((0.03, 1.36, 0.62)), r1=0.11, d=0.012, v=3, material=M('ink'))
    tri.rotation_euler = (rd(90), 0, 0)
    area_at(C, off, c, s, dirv=(0, -1.0, 1.8), mult=1.5, sx=2.2)
    cam = cam_at(C, off, c, s, az=-42, el=10, fill=1.15, lens=18, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=-42, el=10, fill=1.15, kind='slide', az2=42, ease='LIN')
    return cam


# ================= 25 =================
def s25(off, C, f0, f1):
    c, s = (0, 0, 0.32), 0.50
    L.prim('cyl', C, C + "_hole", loc=off, r=0.26, d=0.008, material=M('ink'))
    tri = L.prim('cone', C, C + "_play", loc=off + V((0.03, 0, 0)), r1=0.10, d=0.010, v=3, material=M('ink'))
    tri.rotation_euler = (rd(90), 0, 0)
    disc(C, C + "_disc", (0, 0, 0.55), 0.15, off)
    torus(C, C + "_rim", (0, 0, 0.55), 0.150, 0.0018, off, 'mint')
    L.prim('cube', C, C + "_shaft", loc=off + V((0, 0, 0.62)), scale=(0.004, 0.004, 0.30),
           material=L.mat_shaft("SH_P", (1.0, 0.95, 0.9), strength=3.2, alpha=0.6))
    for k in range(5):
        torus(C, C + "_w%d" % k, (0, 0, 0.02), 0.32 + k * 0.16, 0.0011, off, 'mint', mv=160)
    key_at(C, off, c, s, dirv=(-0.9, -1.0, 1.4), mult=1.2, cone=30)
    cam = cam_at(C, off, c, s, az=0, el=12, fill=1.1, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=0, el=12, fill=1.1, kind='pull', fill2=3.0, ease='OUT')
    return cam


# ================= 26 =================
def s26(off, C, f0, f1):
    c, s = (0, 0.6, 0), 0.9
    L.prim('plane', C, C + "_white", loc=off + V((0, 0.9, 0)), rot=(rd(90), 0, 0), size=8.0,
           material=M('white_e'))
    dot = L.prim('cyl', C, C + "_dot", loc=off + V((0, 0.80, 0)), rot=(rd(90), 0, 0), r=0.008, d=0.003,
                 material=M('ink'))
    L.area(C + "_key", C, loc=off + V((0, -1.2, 0.4)), target=off + V((0, 0.9, 0)), size=3.0, power=2000.0)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=6.0, lens=50, ortho=1.6)
    L.key_cam(cam, f0, loc=cam.location.copy(), target=off + V(c))
    dot.keyframe_insert("scale", frame=f0)
    dot.scale = (0.12, 0.12, 1)
    dot.keyframe_insert("scale", frame=f1)
    return cam


# ================= 27 =================
def s27(off, C, f0, f1):
    c, s = (0, 0, 0.055), 0.075
    sh = L.prim('cube', C, C + "_shaft", loc=off + V((0, 0, 0.075)), scale=(0.0016, 0.0016, 0.10),
                material=L.mat_shaft("SH_S", (0.88, 0.93, 1.0), strength=1.6, alpha=0.40))
    sh.rotation_euler = (rd(6), 0, rd(-3))
    L.prim('cyl', C, C + "_pool", loc=off + V((0.004, 0.001, 0.0008)), r=0.014, d=0.0012, material=M('white_e'))
    dust(C, off, (0, 0, 0.05), s, n=12, r_k=0.005, seed=3)
    key_at(C, off, c, s, mult=0.4)
    return cam_at(C, off, c, s, az=32, el=7, fill=0.82, lens=100, dof_k=0.55, fstop=2.4)


# ================= 28 =================
def s28(off, C, f0, f1):
    c, s = (0, -0.6, 0.30), 0.85
    archive(C, off, nx=6, ny=9)
    sh = L.prim('cube', C, C + "_shaft", loc=off + V((0, -0.6, 0.42)), scale=(0.006, 0.006, 0.16),
                material=L.mat_shaft("SH_F", (0.9, 0.94, 1.0), strength=2.0, alpha=0.45))
    d = disc(C, C + "_disc", (0, -0.6, 0.28), 0.15, off)
    torus(C, C + "_rim", (0, -0.6, 0.28), 0.150, 0.0014, off, 'mint')
    area_at(C, off, c, s, dirv=(0, -1.0, 1.2), mult=1.4, sx=1.8, ratio=2.6)
    cam = cam_at(C, off, c, s, az=-84, el=6, fill=1.0, lens=50, dof_k=0.5, fstop=2.0)
    move(cam, f0, f1, c, s, az=-84, el=6, fill=1.0, kind='orbit', az2=-64, el2=8)
    return cam


# ================= 29 =================
def s29(off, C, f0, f1):
    c, s = (0, 0, 0.30), 0.16
    disc(C, C + "_disc", (0, 0, 0.30), 0.15, off)
    torus(C, C + "_rim", (0, 0, 0.30), 0.150, 0.0016, off, 'mint')
    key_at(C, off, c, s, dirv=(-0.8, -1.0, 1.2), mult=1.2, cone=28)
    cam = cam_at(C, off, c, s, az=0, el=4, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    move(cam, f0, f1, c, s, az=0, el=4, fill=1.0, kind='pull', fill2=30.0, ease='LIN')
    return cam


# ================= 30 / 31 / 32 =================
PX = 1.2 / 720.0


def s30(off, C, f0, f1):
    c, s = (0, 0, -0.10), 0.5
    box(C, C + "_line", (0, 0, -0.10), (0.50, PX * 0.85, PX * 0.85), 'paper', off)
    blk = box(C, C + "_blk", (-0.505, 0, -0.10 - 0.022), (0.018, 0.0014, 0.018), 'orange', off)
    L.area(C + "_key", C, loc=off + V((0, -1.0, 0.4)), target=off, size=2.0, power=400.0)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=3.2, lens=50, ortho=1.2)
    L.key_cam(cam, f0, loc=cam.location.copy(), target=off + V(c))
    blk.scale = (0.15, 1, 1)
    blk.keyframe_insert("scale", frame=f0)
    blk.scale = (1, 1, 1)
    blk.keyframe_insert("scale", frame=f0 + 3)
    return cam


def s31(off, C, f0, f1):
    c, s = (0, 0, -0.10), 0.5
    box(C, C + "_line", (0, 0, -0.10), (0.50, PX * 0.85, PX * 0.85), 'paper', off)
    box(C, C + "_blk", (-0.505, 0, -0.122), (0.018, 0.0018, 0.018), 'orange', off)
    for i in range(9):
        w = 0.005 + 0.004 * ((i * 5) % 3)
        box(C, C + "_g%d" % i, (-0.26 + i * 0.030, 0, -0.020), (w, 0.0018, 0.010 + 0.004 * (i % 2)), 'paper', off)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=3.2, lens=50, ortho=1.2)
    L.key_cam(cam, f0, loc=cam.location.copy(), target=off + V(c))
    return cam


def s32(off, C, f0, f1):
    c, s = (0, 0, -0.10), 0.5
    line = box(C, C + "_line", (0, 0, -0.10), (0.50, PX * 0.85, PX * 0.85), 'paper', off)
    cam = cam_at(C, off, c, s, az=0, el=0, fill=3.2, lens=50, ortho=1.2)
    L.key_cam(cam, f0, loc=cam.location.copy(), target=off + V(c))
    line.keyframe_insert("scale", frame=f0 + 2)
    line.scale = (0.0, PX * 0.85, PX * 0.85)
    line.keyframe_insert("scale", frame=f1)
    return cam
