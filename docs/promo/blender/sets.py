# -*- coding: utf-8 -*-
"""Set builders for RHINE PV 70s previz. Each builder returns its camera and keys its move."""
import bpy, bmesh, math, random
from mathutils import Vector, Euler, Matrix
from math import radians as rd, sin, cos, pi
import pvlib as L

V = Vector
R = random.Random(7)

# ---------------- shared materials ----------------
MAT = {}


def M(key):
    if key in MAT:
        return MAT[key]
    m = None
    if key == 'paper':
        m = L.mat("PAPER", L.srgb("#E7E7E1"), rough=0.62, spec=0.25)
    elif key == 'paper2':
        m = L.mat("PAPER2", L.srgb("#D9D8CE"), rough=0.7, spec=0.2)
    elif key == 'ink':
        m = L.mat("INK", L.srgb("#141613"), rough=0.45, spec=0.18)
    elif key == 'moss':
        m = L.mat("MOSS", L.srgb("#777B60"), rough=0.55, spec=0.3)
    elif key == 'alu':
        m = L.mat("ALU", L.srgb("#B9B9B4"), rough=0.26, metal=1.0, aniso=0.30)
    elif key == 'alu2':
        m = L.mat("ALU2", L.srgb("#8A8A86"), rough=0.34, metal=1.0, aniso=0.24)
    elif key == 'vinyl':
        m = L.mat_vinyl("VINYL", grooves=140.0)
    elif key == 'vinyl2':
        m = L.mat_vinyl("VINYL2", grooves=60.0)
    elif key == 'mint':
        m = L.mat("MINT_E", L.srgb("#8FE3BB"), emit=L.srgb("#8FE3BB"), estr=2.4)
    elif key == 'orange':
        m = L.mat("ORG_E", L.srgb("#E8622A"), emit=L.srgb("#E8622A"), estr=2.2)
    elif key == 'white_e':
        m = L.mat("WHITE_E", L.srgb("#FFFFFF"), emit=L.srgb("#FFFFFF"), estr=1.4)
    elif key == 'glass':
        m = L.mat("GLASS", L.srgb("#E7E7E1"), rough=0.10, alpha=0.22, transmission=0.85, ior=1.45)
    elif key == 'water':
        m = L.mat("WATER", L.srgb("#CFE8E0"), rough=0.05, alpha=0.35, transmission=0.9, ior=1.33)
    MAT[key] = m
    return m


def shaft(name, off, color=(0.86, 0.91, 1.0), strength=2.0, w=0.016, h=1.30, y=0.0, tilt=7.0, alpha=0.38):
    """A light shaft: thin emissive stick, bright at the top, gone at the bottom."""
    m = L.mat_shaft("SHAFT_" + name, color, strength=strength, alpha=alpha)
    b = L.prim('cube', 'S_' + name, name + "_shaft", loc=off + V((0.0, y, h * 0.5)),
               scale=(w, w, h), material=m)
    b.rotation_euler = (rd(tilt), 0, rd(-4))
    return b


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


def dust(C, off, n=40, spread=0.10, center=(0, 0, 0.05), mat='white_e', r=0.0016, seed=1):
    rr = random.Random(seed)
    out = []
    for i in range(n):
        p = V(center) + V((rr.uniform(-spread, spread), rr.uniform(-spread, spread), rr.uniform(-spread * 0.6, spread * 0.6)))
        out.append(L.prim('ico', C, C + "_d%02d" % i, loc=off + p, r=r, sub=1, material=M(mat)))
    return out


def key_spot(C, off, rel=(-0.45, -0.40, 0.75), tgt=(0, 0, 0.05), power=35.0, size=24.0, color=(1, 1, 1), blend=0.25):
    return L.spot(C + "_key", C, loc=off + V(rel), target=off + V(tgt), size=size, power=power, color=color, blend=blend)


def key_area(C, off, rel=(-0.3, -0.4, 0.5), tgt=(0, 0, 0.03), power=12.0, sx=0.5, sy=None, color=(1, 1, 1)):
    return L.area(C + "_key", C, loc=off + V(rel), target=off + V(tgt), size=sx, size_y=sy or sx, power=power, color=color)


def mkc(C, off, loc, tgt, lens=50.0, dof=None, fstop=4.0, ortho=None, clip=0.005):
    c = L.cam(C + "_cam", C, loc=off + V(loc), target=off + V(tgt), lens=lens, dof=dof, fstop=fstop, ortho=ortho)
    c.data.clip_start = clip
    c.data.clip_end = 400.0
    return c


def ease(u, kind='IO'):
    u = max(0.0, min(1.0, u))
    if kind == 'IN':
        return u * u * u
    if kind == 'OUT':
        return 1 - (1 - u) ** 3
    if kind == 'LIN':
        return u
    return u * u * (3 - 2 * u)


def move_lerp(c, f0, f1, l0, l1, tgt, kind='IO', roll=0.0):
    l0 = V(l0); l1 = V(l1)
    for f, a in ((f0, 0.0), (f1, 1.0)):
        u = ease(a, kind)
        L.key_cam(c, f, loc=l0.lerp(l1, u), target=tgt, roll=roll)
    return c


def move_orbit(c, f0, f1, center, radius, a0, a1, z0, z1, tgt=None, kind='IO'):
    ctr = V(center)
    tg = V(tgt) if tgt is not None else ctr
    for f, a in ((f0, 0.0), (f1, 1.0)):
        u = ease(a, kind)
        ang = rd(a0 + (a1 - a0) * u)
        z = z0 + (z1 - z0) * u
        p = ctr + V((radius * cos(ang), radius * sin(ang), z))
        L.key_cam(c, f, loc=p, target=tg)
    return c


# ---------------- 01 / 27  beam + dust ----------------
def s01(off, C, f0, f1):
    shaft(C, off, strength=2.4, w=0.022, h=1.4)
    dust(C, off, n=9, spread=0.05, center=(0, 0, 0.06), r=0.0014, seed=3)
    # one grain on the focal plane, two far out
    L.prim('ico', C, C + "_g0", loc=off + V((0.004, -0.010, 0.030)), r=0.0021, sub=2, material=M('white_e'))
    key_spot(C, off, rel=(-0.30, -0.50, 0.85), power=26.0, size=18)
    c = mkc(C, off, (0.06, -0.20, 0.075), (0.0, -0.005, 0.035), lens=100, dof=0.205, fstop=2.0)
    move_lerp(c, f0, f1, off + V((0.075, -0.20, 0.078)), off + V((0.055, -0.195, 0.073)), (0, -0.005, 0.035), 'LIN')
    return c


def s27(off, C, f0, f1):
    shaft(C, off, strength=1.5, w=0.022, h=1.4)
    dust(C, off, n=9, spread=0.05, center=(0, 0, 0.06), r=0.0014, seed=3)
    L.prim('ico', C, C + "_g0", loc=off + V((0.004, -0.010, 0.030)), r=0.0021, sub=2, material=M('white_e'))
    key_spot(C, off, rel=(-0.30, -0.50, 0.85), power=20.0, size=18)
    c = mkc(C, off, (0.055, -0.195, 0.073), (0.0, -0.005, 0.035), lens=100, dof=0.205, fstop=2.0)
    L.key_cam(c, f0, loc=off + V((0.055, -0.195, 0.073)), target=off + V((0, -0.005, 0.035)))
    return c


# ---------------- 02  disc corner, specular streak ----------------
def s02(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off, r=0.16, d=0.004, material=M('vinyl'))
    L.smooth(d); L.bevel(d, 0.0012, 2)
    L.prim('torus', C, C + "_rim", R=0.159, r=0.0015, material=M('alu'), loc=off + V((0, 0, 0.0005)))
    key_area(C, off, rel=(-0.10, -0.10, 0.14), tgt=(0.05, 0, 0), power=6.0, sx=0.02, sy=1.2)
    dust(C, off, n=14, spread=0.09, center=(0, 0, 0.02), r=0.0012, seed=11)
    c = mkc(C, off, (0.02, -0.075, 0.016), (0.045, 0.01, 0.0), lens=100, dof=0.075, fstop=2.8)
    for f, z in ((f0, None), (f1, None)):
        pass
    L.key_cam(c, f0, loc=off + V((0.02, -0.075, 0.016)), target=off + V((0.050, 0.012, 0.0)))
    L.key_cam(c, f1, loc=off + V((0.028, -0.075, 0.016)), target=off + V((0.045, 0.010, 0.0)))
    # focus pull: from dust plane to groove
    cd = c.data
    cd.dof.focus_distance = 0.075
    cd.dof.keyframe_insert("focus_distance", frame=f0)
    cd.dof.focus_distance = 0.115
    cd.dof.keyframe_insert("focus_distance", frame=f1)
    return c


# ---------------- 03 / 28  disc macro grooves ----------------
def s03(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off, r=0.16, d=0.004, material=M('vinyl'))
    L.smooth(d); L.bevel(d, 0.0012, 2)
    # a scratch: thin bright groove
    sc = L.prim('cube', C, C + "_scratch", loc=off + V((-0.03, 0.02, 0.0023)), scale=(0.09, 0.0006, 0.0004), material=M('alu2'))
    sc.rotation_euler = (0, 0, rd(28))
    key_area(C, off, rel=(0.10, -0.16, 0.10), tgt=(0, 0, 0), power=5.0, sx=0.28)
    c = mkc(C, off, (0.045, -0.10, 0.035), (0.0, 0.0, 0.0), lens=100, dof=0.11, fstop=2.2)
    L.key_cam(c, f0, loc=off + V((0.050, -0.105, 0.036)), target=off + V((-0.01, 0.0, 0.0)))
    L.key_cam(c, f1, loc=off + V((0.040, -0.100, 0.034)), target=off + V((0.005, 0.0, 0.0)))
    for ob in (d, sc):
        pass
    d.rotation_euler = (0, 0, 0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(90))
    d.keyframe_insert("rotation_euler", frame=f1)
    return c


# ---------------- 04  hex crystal ----------------
def s04(off, C, f0, f1):
    cr = L.prim('cyl', C, C + "_hex", loc=off + V((0, 0, 0.05)), r=0.030, d=0.075, v=6, material=M('glass'))
    L.smooth(cr); L.bevel(cr, 0.002, 3)
    for i, (dx, dy) in enumerate(((-0.05, -0.02), (0.05, 0.01), (0.0, 0.06))):
        L.prim('plane', C, C + "_bg%d" % i, loc=off + V((dx, dy, 0.05)), rot=(rd(90), 0, rd(20 * i)),
               size=0.03, material=M('mint' if i != 1 else 'orange'))
    key_spot(C, off, rel=(-0.18, -0.30, 0.45), power=14.0, size=14)
    dust(C, off, n=16, spread=0.05, center=(0, 0, 0.05), r=0.0010, seed=5)
    c = mkc(C, off, (0.02, -0.11, 0.085), (0.0, 0.0, 0.048), lens=100, dof=0.10, fstop=2.4)
    move_lerp(c, f0, f1, off + V((0.030, -0.115, 0.075)), off + V((0.010, -0.105, 0.070)), (0, 0, 0.048), 'IO')
    return c


# ---------------- 05  column of records rising ----------------


def s05(off, C, f0, f1):
    for i in range(16):
        z = i * 0.011
        d = L.prim('cyl', C, C + "_st%02d" % i, loc=off + V((R.uniform(-0.002, 0.002), R.uniform(-0.002, 0.002), z)),
                   r=0.150, d=0.004, material=M('vinyl') if i % 2 else M('vinyl2'))
        L.smooth(d)
    rr = random.Random(9)
    pts = [V((rr.gauss(0, 0.11), rr.gauss(0, 0.11), 0.09 + rr.gauss(0, 0.06))) for _ in range(70)]
    points_mesh(C, C + "_dust", [off + p for p in pts], size=0.0018, mat='white_e', sub=1)
    key_spot(C, off, rel=(-0.55, -0.65, 1.10), power=120.0, size=26)
    c = mkc(C, off, (0.30, -0.42, 0.045), (0.0, 0.0, 0.09), lens=50, dof=0.55, fstop=5.6)
    move_lerp(c, f0, f1, off + V((0.28, -0.40, 0.040)), off + V((1.05, -1.55, 0.30)), (0, 0, 0.10), 'IN')
    return c

def s06a(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off, r=0.16, d=0.004, material=M('vinyl'))
    L.smooth(d); L.bevel(d, 0.0012, 2)
    needle = L.prim('cone', C, C + "_needle", loc=off + V((0.02, 0.0, 0.010)), r1=0.0018, r2=0.0002, d=0.018, v=16,
                    material=M('alu'))
    needle.rotation_euler = (rd(18), 0, 0)
    L.area(C + "_key", C, loc=off + V((0.10, -0.12, 0.09)), target=off + V((0.02, 0, 0)), size=0.20, power=4.0)
    c = mkc(C, off, (0.052, -0.048, 0.020), (0.020, 0.0, 0.001), lens=100, dof=0.062, fstop=2.0)
    L.key_cam(c, f0, loc=off + V((0.052, -0.048, 0.020)), target=off + V((0.020, 0.0, 0.001)))
    needle.location = off + V((0.020, 0.0, 0.012))
    needle.keyframe_insert("location", frame=f0)
    needle.location = off + V((0.020, 0.0, 0.0026))
    needle.keyframe_insert("location", frame=f1 - 1)
    return c


def s06b(off, C, f0, f1):
    # valley of a scratch
    d = L.prim('cube', C, C + "_plate", loc=off, scale=(0.6, 0.6, 0.02), material=M('vinyl2'))
    L.bevel(d, 0.004, 2)
    for i in range(3):
        L.prim('cube', C, C + "_gr%d" % i, loc=off + V((-0.10 + i * 0.10, 0.0, 0.0102)),
               scale=(0.0012, 0.5, 0.0016), material=M('alu2'))
    key_area(C, off, rel=(-0.15, -0.05, 0.035), tgt=(0, 0, 0.01), power=3.0, sx=0.01, sy=0.6)
    c = mkc(C, off, (-0.05, -0.06, 0.028), (0.0, 0.0, 0.012), lens=100, dof=0.09, fstop=2.0)
    move_lerp(c, f0, f1, off + V((-0.06, -0.065, 0.030)), off + V((0.02, -0.065, 0.026)), (0, 0, 0.012), 'LIN')
    return c


def s06c(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off, r=0.16, d=0.004, material=M('vinyl'))
    L.smooth(d)
    # grid revealed by a sweeping light
    for i in range(9):
        L.prim('cube', C, C + "_gx%d" % i, loc=off + V((0, -0.14 + i * 0.035, 0.0023)), scale=(0.30, 0.0007, 0.0004), material=M('mint'))
        L.prim('cube', C, C + "_gy%d" % i, loc=off + V((-0.14 + i * 0.035, 0, 0.0023)), scale=(0.0007, 0.30, 0.0004), material=M('mint'))
    key_area(C, off, rel=(-0.20, -0.06, 0.05), tgt=(0, 0, 0), power=9.0, sx=0.02, sy=0.4)
    c = mkc(C, off, (0.16, -0.16, 0.10), (0.0, 0.0, 0.0), lens=25, dof=0.25, fstop=4.0)
    move_lerp(c, f0, f1, off + V((0.17, -0.17, 0.11)), off + V((0.02, -0.10, 0.09)), (0, 0, 0.0), 'OUT')
    return c


# ---------------- 07  Chladni ----------------
def chladni_pts(m, n, count, Rmax=0.148, tol=0.055, seed=2):
    rr = random.Random(seed)
    pts = []
    guard = 0
    while len(pts) < count and guard < count * 400:
        guard += 1
        r = Rmax * math.sqrt(rr.random())
        th = rr.uniform(0, 2 * pi)
        c = cos(n * th) * cos(m * pi * r / Rmax) - cos(m * th) * cos(n * pi * r / Rmax)
        if abs(c) < tol:
            pts.append((r * cos(th), r * sin(th)))
    return pts




def s07(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off, r=0.15, d=0.005, material=M('vinyl'))
    L.smooth(d)
    pts = chladni_pts(3, 5, 260)
    points_mesh(C, C + "_sand", [off + V((x, y, 0.0032)) for (x, y) in pts], size=0.0016, mat='paper', sub=1)
    key_spot(C, off, rel=(-0.10, -0.12, 0.42), tgt=(0, 0, 0), power=26.0, size=30, blend=0.4)
    c = mkc(C, off, (0.0, -0.26, 0.30), (0, 0, 0), lens=100, dof=0.38, fstop=4.0)
    move_lerp(c, f0, f1, off + V((0.0, -0.27, 0.30)), off + V((0.0, -0.235, 0.285)), (0, 0, 0), 'LIN')
    d.rotation_euler = (0, 0, 0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(24))
    d.keyframe_insert("rotation_euler", frame=f1)
    return c

def s08(off, C, f0, f1):
    # rings that become a tunnel
    for i in range(16):
        rr = 0.150 * (1 - i / 16.5)
        z = i * 0.16
        L.prim('torus', C, C + "_ring%02d" % i, R=rr, r=0.0012 + 0.0009 * i,
               loc=off + V((0, 0, z)), material=M('paper') if i < 6 else M('mint'), mv=64, nv=6)
    L.prim('cyl', C, C + "_hole", loc=off + V((0, 0, 2.62)), r=0.02, d=0.004, material=M('white_e'))
    key_spot(C, off, rel=(-0.4, -0.5, 1.2), power=40.0, size=40)
    c = mkc(C, off, (0.0, -0.30, 0.18), (0, 0, 0.4), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -0.30, 0.18)), target=off + V((0.0, 0, 0.55)))
    L.key_cam(c, f1, loc=off + V((0.0, -0.02, 1.05)), target=off + V((0.0, 0, 1.9)))
    L.interp(c, 'BEZIER')
    return c


# ---------------- 09 / 10 / 13  archive ----------------


def array2d(ob, nx, ny, pitch):
    sx, sy = ob.scale[0], ob.scale[1]
    ax = ob.modifiers.new("ax", 'ARRAY')
    ax.count = nx
    ax.use_relative_offset = False
    ax.use_constant_offset = True
    ax.constant_offset_displace = (pitch / sx, 0, 0)
    ay = ob.modifiers.new("ay", 'ARRAY')
    ay.count = ny
    ay.use_relative_offset = False
    ay.use_constant_offset = True
    ay.constant_offset_displace = (0, pitch / sy, 0)
    return ob


def archive_grid(C, off, nx=9, ny=14, pitch=0.42, h=0.30):
    x0 = -(nx - 1) * pitch / 2
    box = L.prim('cube', C, C + "_boxes", loc=off + V((x0, 0, 0)),
                 scale=(0.30, 0.30, h), material=M('paper'))
    L.bevel(box, 0.004, 2)
    array2d(box, nx, ny, pitch)
    lab = L.prim('cube', C, C + "_labels", loc=off + V((x0, -0.151, 0.02)),
                 scale=(0.10, 0.001, 0.012), material=M('ink'))
    array2d(lab, nx, ny, pitch)
    tick = L.prim('cube', C, C + "_ticks", loc=off + V((x0 + 0.10, -0.151, 0.02)),
                  scale=(0.012, 0.001, 0.012), material=M('moss'))
    array2d(tick, nx, ny, pitch)
    slit = L.prim('cube', C, C + "_slits", loc=off + V((x0, -0.152, -0.03)),
                  scale=(0.16, 0.0008, 0.004), material=M('mint'))
    array2d(slit, nx, ny, pitch)
    return box

def s09(off, C, f0, f1):
    archive_grid(C, off)
    L.area(C + "_key", C, loc=off + V((0, 2.0, 3.2)), target=off + V((0, 3.0, 0)), size=4.0, power=900.0)
    c = mkc(C, off, (0.0, -1.2, 0.16), (0.0, 6.0, 0.10), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -1.2, 0.16)), target=off + V((0, 6.0, 0.10)))
    L.key_cam(c, f1, loc=off + V((0.0, 4.6, 0.14)), target=off + V((0, 12.0, 0.10)))
    L.interp(c, 'LINEAR')
    return c


def s10(off, C, f0, f1):
    archive_grid(C, off, nx=6, ny=8, pitch=0.42)
    L.area(C + "_key", C, loc=off + V((2.2, 1.8, 2.2)), target=off + V((0, 1.6, 0)), size=3.0, power=700.0)
    c = mkc(C, off, (-1.05, 0.2, 0.02), (0.6, 0.9, 0.02), lens=35, dof=1.4, fstop=4.0)
    L.key_cam(c, f0, loc=off + V((-1.05, 0.20, 0.02)), target=off + V((1.0, 0.55, 0.02)))
    L.key_cam(c, f1, loc=off + V((-1.05, 1.25, 0.02)), target=off + V((1.0, 1.60, 0.02)))
    L.interp(c, 'LINEAR')
    return c


def s13(off, C, f0, f1):
    # top-down: archive = concentric grooves of a giant record
    for k in range(11):
        rr = 0.6 + k * 0.55
        L.prim('torus', C, C + "_r%02d" % k, R=rr, r=0.035, loc=off, material=M('paper'), mv=128, nv=8)
    L.prim('cyl', C, C + "_hub", r=0.55, d=0.06, material=M('paper2'))
    L.prim('torus', C, C + "_hp", R=0.62, r=0.02, loc=off + V((0, 0, 0.03)), material=M('mint'), mv=128, nv=6)
    L.area(C + "_key", C, loc=off + V((0.5, -1.5, 8.0)), target=off, size=6.0, power=2600.0)
    c = mkc(C, off, (0, -0.9, 7.4), (0, 0, 0), lens=25, ortho=None, clip=0.01)
    move_orbit(c, f0, f1, off, 7.6, -95, -65, 0.2, 0.2, tgt=off + V((0, 0, 0)))
    return c


# ---------------- 11  box face as screen ----------------
def s11(off, C, f0, f1):
    L.prim('cube', C, C + "_box", loc=off + V((0, 0, 0)), scale=(0.34, 0.34, 0.34), material=M('paper'))
    for i, m in enumerate(('water', 'mint', 'orange', 'paper')):
        p = L.prim('plane', C, C + "_scr%d" % i, loc=off + V((0, -0.1715, 0)), rot=(rd(90), 0, 0), size=0.30,
                   material=M(m))
        p.hide_render = (i != 0)
        # visibility flags animated later
    key_area(C, off, rel=(0.3, -0.8, 0.5), tgt=(0, 0, 0), power=30.0, sx=0.6)
    c = mkc(C, off, (0.0, -1.05, 0.05), (0, 0, 0), lens=50, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -1.05, 0.05)), target=off + V((0, 0, 0)))
    n = f1 - f0
    for i in range(4):
        fa = f0 + int(n * i / 4)
        fb = f0 + int(n * (i + 1) / 4) - 1
        ob = bpy.data.objects[C + "_scr%d" % i]
        for f, vis in ((fa, False), (fb, True)):
            ob.hide_render = vis
            ob.keyframe_insert("hide_render", frame=f)
    return c


# ---------------- 12  data column ----------------
def s12(off, C, f0, f1):
    col = L.prim('cube', C, C + "_col", loc=off + V((0, 0, 0.9)), scale=(0.34, 0.34, 1.8), material=M('paper'))
    L.bevel(col, 0.006, 2)
    for k in range(7):
        L.prim('torus', C, C + "_r%02d" % k, R=0.10 + 0.03 * k, r=0.004, loc=off + V((0, 0, 1.85 + k * 0.02)),
               material=M('mint'), mv=96, nv=6)
    for k in range(4):
        L.prim('ico', C, C + "_p%d" % k, loc=off + V((0.06 * cos(k * 1.6), 0.06 * sin(k * 1.6), 1.9)),
               r=0.012, sub=2, material=M('white_e'))
    L.area(C + "_key", C, loc=off + V((-1.4, -1.8, 2.6)), target=off + V((0, 0, 1.2)), size=2.4, power=700.0)
    dust(C, off, n=40, spread=0.5, center=(0, 0, 1.5), r=0.002, seed=13)
    c = mkc(C, off, (0.9, -1.3, 0.55), (0, 0, 1.3), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.90, -1.30, 0.55)), target=off + V((0, 0, 1.30)))
    L.key_cam(c, f1, loc=off + V((0.55, -0.85, 1.55)), target=off + V((0, 0, 2.00)))
    return c


# ---------------- 14  thin disc, side lit ----------------
def s14(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.16)), r=0.15, d=0.0035, material=M('paper2'))
    L.smooth(d)
    d.rotation_euler = (rd(90), 0, 0)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0012, loc=off + V((0, 0.0018, 0.16)), rot=(rd(90), 0, 0),
           material=M('mint'), mv=128, nv=6)
    L.area(C + "_key", C, loc=off + V((-0.9, 0.30, 0.20)), target=off + V((0, 0, 0.16)), size=0.6, power=28.0)
    L.area(C + "_rim", C, loc=off + V((0.55, 0.55, 0.16)), target=off + V((0, 0, 0.16)), size=0.4, power=18.0,
           color=(0.56, 0.89, 0.73))
    c = mkc(C, off, (0.0, -0.62, 0.175), (0, 0, 0.16), lens=100, dof=0.62, fstop=5.6)
    L.key_cam(c, f0, loc=off + V((0.0, -0.62, 0.175)), target=off + V((0, 0, 0.16)))
    return c


# ---------------- 15  domino fall ----------------
def s15(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.152)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    d.rotation_euler = (rd(90), 0, 0)
    d.location = off + V((0, 0, 0.152))
    d.keyframe_insert("location", frame=f0)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (rd(45 + 45), 0, 0)
    d.location = off + V((0, 0.02, 0.010))
    d.keyframe_insert("location", frame=f1)
    d.keyframe_insert("rotation_euler", frame=f1)
    L.prim('plane', C, C + "_floor", loc=off + V((0, 0, 0)), size=3.0, material=M('paper2'))
    for i in range(26):
        a = 2 * pi * i / 26
        L.prim('ico', C, C + "_p%02d" % i, loc=off + V((0.24 * cos(a), 0.24 * sin(a), 0.02)),
               r=0.004, sub=1, material=M('paper'))
    key_spot(C, off, rel=(-0.5, -0.6, 1.0), power=90.0, size=30)
    c = mkc(C, off, (0.28, -0.62, 0.12), (0, 0, 0.05), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.05, -0.70, 0.55)), target=off + V((0, 0.02, 0.14)))
    L.key_cam(c, f1, loc=off + V((0.06, -0.62, 0.10)), target=off + V((0, 0.02, 0.02)))
    return c


# ---------------- 16  dust cloud ----------------


def s16(off, C, f0, f1):
    rr = random.Random(21)
    pts = [V((rr.gauss(0, 0.16), rr.gauss(0, 0.16), rr.gauss(0.02, 0.10))) for _ in range(320)]
    points_mesh(C, C + "_cloud", [off + p for p in pts], size=0.0032, mat='paper', sub=1)
    L.area(C + "_key", C, loc=off + V((-1.0, -1.2, 1.4)), target=off, size=2.0, power=180.0)
    c = mkc(C, off, (0.10, -0.45, 0.10), (0, 0, 0.02), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.10, -0.45, 0.10)), target=off + V((0, 0, 0.02)))
    L.key_cam(c, f1, loc=off + V((-0.05, -0.20, 0.06)), target=off + V((0.05, 0.05, 0.03)))
    return c

def s17(off, C, f0, f1):
    eye = L.prim('sphere', C, C + "_eye", loc=off + V((0, 0, 0)), r=0.055, material=M('paper'))
    L.smooth(eye)
    L.prim('torus', C, C + "_iris", R=0.020, r=0.0022, loc=off + V((0, -0.050, 0)), rot=(rd(90), 0, 0),
           material=M('moss'), mv=96, nv=6)
    for k in range(5):
        L.prim('torus', C, C + "_ir%d" % k, R=0.006 + k * 0.0035, r=0.0008,
               loc=off + V((0, -0.0515, 0)), rot=(rd(90), 0, 0), material=M('ink'), mv=96, nv=4)
    L.prim('cyl', C, C + "_pupil", r=0.0075, d=0.002, loc=off + V((0, -0.0525, 0)), rot=(rd(90), 0, 0),
           material=M('ink'))
    key_spot(C, off, rel=(-0.16, -0.20, 0.14), power=10.0, size=26)
    c = mkc(C, off, (0.0, -0.16, 0.005), (0, 0, 0), lens=100, dof=0.14, fstop=2.8)
    L.key_cam(c, f0, loc=off + V((0.0, -0.155, 0.006)), target=off + V((0, 0, 0)))
    L.key_cam(c, f1, loc=off + V((0.0, -0.058, 0.001)), target=off + V((0, 0, 0)))
    return c


# ---------------- 18  spectrum tunnel ----------------
def s18(off, C, f0, f1):
    for k in range(34):
        a = 2 * pi * k / 34
        z = k * 0.34
        h = 0.10 + 0.30 * abs(sin(k * 1.7)) * (1 - k / 40.0)
        L.prim('cube', C, C + "_bar%02d" % k, loc=off + V((0.95 * cos(a), 0.95 * sin(a), z + h * 0.5)),
               scale=(0.014, 0.014, h), material=M('paper') if k % 3 else M('mint'))
        L.prim('cube', C, C + "_bar2%02d" % k, loc=off + V((1.05 * cos(a), 1.05 * sin(a), z + h * 0.4)),
               scale=(0.008, 0.008, h * 0.7), material=M('ink'))
    L.area(C + "_key", C, loc=off + V((0, 0, 6.0)), target=off + V((0, 0, 3.0)), size=3.0, power=900.0)
    c = mkc(C, off, (0.0, 0.0, 0.2), (0, 0, 3.0), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, 0.0, 0.25)), target=off + V((0, 0, 3.2)))
    L.key_cam(c, f1, loc=off + V((0.0, 0.0, 4.60)), target=off + V((0, 0, 7.6)))
    L.interp(c, 'LINEAR')
    return c


# ---------------- 20  floating disc in black room ----------------
def s20(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.30)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0016, loc=off + V((0, 0, 0.30)), material=M('mint'), mv=128, nv=6)
    L.prim('cube', C, C + "_floor", loc=off + V((0, 0, -0.002)), scale=(6, 6, 0.004), material=M('ink'))
    dust(C, off, n=60, spread=0.35, center=(0, 0, 0.30), r=0.0018, seed=17)
    key_spot(C, off, rel=(-0.7, -0.9, 1.1), power=60.0, size=22, blend=0.2)
    c = mkc(C, off, (0.0, -0.85, 0.36), (0, 0, 0.30), lens=50, dof=0.85, fstop=2.8)
    move_orbit(c, f0, f1, off + V((0, 0, 0.30)), 0.86, -90, -75, 0.06, 0.05, tgt=off + V((0, 0, 0.30)))
    return c
