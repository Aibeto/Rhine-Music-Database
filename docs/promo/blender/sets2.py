# -*- coding: utf-8 -*-
"""Second half of the set builders (19 montage, 21-26, 28-32)."""
import bpy, bmesh, math, random
from mathutils import Vector, Euler, Matrix
from math import radians as rd, sin, cos, pi
import pvlib as L
from sets import M, V, mkc, key_spot, key_area, dust, points_mesh, shaft, ease, move_lerp, move_orbit, archive_grid

R = random.Random(11)


# ---------------- 19  ten cuts ----------------
def _glass_ripple(off, C):
    L.prim('cyl', C, C + "_glass", loc=off + V((0, 0, 0.045)), r=0.032, d=0.090, material=M('glass'))
    L.prim('cyl', C, C + "_water", loc=off + V((0, 0, 0.060)), r=0.030, d=0.040, material=M('water'))
    for k in range(6):
        L.prim('torus', C, C + "_rip%d" % k, R=0.004 + k * 0.0045, r=0.0006,
               loc=off + V((0, 0, 0.0805)), material=M('paper'), mv=48, nv=4)
    key_area(C, off, rel=(0.05, -0.10, 0.14), tgt=(0, 0, 0.07), power=3.0, sx=0.20)
    c = mkc(C, off, (0.0, -0.085, 0.088), (0, 0, 0.078), lens=100, dof=0.085, fstop=2.4)
    L.key_cam(c, 0, loc=off + V((0.0, -0.085, 0.088)), target=off + V((0, 0, 0.078)))
    return c


def _needle(off, C):
    L.prim('cone', C, C + "_nd", loc=off + V((0, 0, 0.012)), r1=0.0012, r2=0.00008, d=0.026, v=24, material=M('alu'))
    L.prim('cyl', C, C + "_sh", loc=off + V((0, 0, 0.030)), r=0.0035, d=0.014, material=M('alu2'))
    key_area(C, off, rel=(-0.10, -0.06, 0.10), tgt=(0, 0, 0.01), power=2.4, sx=0.015, sy=0.30)
    c = mkc(C, off, (0.010, -0.030, 0.022), (0, 0, 0.012), lens=100, dof=0.036, fstop=2.0)
    L.key_cam(c, 0, loc=off + V((0.010, -0.030, 0.022)), target=off + V((0, 0, 0.012)))
    return c


def _hand(off, C):
    L.prim('cyl', C, C + "_d", loc=off + V((0, 0, 0.004)), r=0.11, d=0.004, material=M('vinyl'))
    for k in range(3):
        f = L.prim('cube', C, C + "_f%d" % k, loc=off + V((-0.02 + k * 0.030, -0.045, 0.008)),
                   scale=(0.018, 0.120, 0.010), material=M('paper'))
        L.bevel(f, 0.004, 3)
        f.rotation_euler = (0, 0, rd(-8 + k * 6))
    key_area(C, off, rel=(0.10, 0.10, 0.14), tgt=(0, 0, 0), power=4.0, sx=0.3)
    c = mkc(C, off, (0.06, -0.13, 0.10), (0, -0.02, 0.005), lens=100, dof=0.16, fstop=2.8)
    L.key_cam(c, 0, loc=off + V((0.06, -0.13, 0.10)), target=off + V((0, -0.02, 0.005)))
    return c


def _scopes(off, C):
    L.prim('cube', C, C + "_scr", loc=off + V((0, 0, 0)), scale=(0.30, 0.002, 0.20), material=M('ink'))
    pts = []
    for i in range(120):
        x = -0.13 + 0.26 * i / 119.0
        y = 0.03 * sin(i * 0.55) * sin(i * 0.11)
        pts.append((x, -0.0018, y))
    for i, (x, y, z) in enumerate(pts):
        L.prim('cube', C, C + "_p%03d" % i, loc=off + V((x, y, z)), scale=(0.0022, 0.0006, 0.0022), material=M('mint'))
    key_area(C, off, rel=(0, -0.3, 0.1), tgt=(0, 0, 0), power=6.0, sx=0.5)
    c = mkc(C, off, (0.0, -0.16, 0.0), (0, 0, 0), lens=50, dof=None, fstop=8.0)
    L.key_cam(c, 0, loc=off + V((0.0, -0.16, 0.0)), target=off + V((0, 0, 0)))
    return c


def _blink(off, C):
    e = L.prim('sphere', C, C + "_eye", r=0.05, loc=off, material=M('paper'))
    L.smooth(e)
    lid = L.prim('sphere', C, C + "_lid", r=0.0505, loc=off + V((0, -0.004, 0.052)), material=M('paper2'))
    L.smooth(lid)
    key_spot(C, off, rel=(-0.1, -0.16, 0.10), power=6.0, size=30)
    c = mkc(C, off, (0, -0.14, 0.0), (0, 0, 0), lens=100, dof=0.14, fstop=2.8)
    L.key_cam(c, 0, loc=off + V((0, -0.14, 0)), target=off + V((0, 0, 0)))
    return c


def _tape(off, C):
    for k in range(5):
        t = L.prim('torus', C, C + "_t%d" % k, R=0.030 + 0.006 * k, r=0.0022,
                   loc=off + V((R.uniform(-0.01, 0.01), 0, 0.03 + R.uniform(-0.01, 0.01))),
                   rot=(rd(60 + 40 * k), rd(20 * k), rd(30 * k)), material=M('ink'), mv=64, nv=6)
    key_area(C, off, rel=(-0.1, -0.2, 0.2), tgt=(0, 0, 0.03), power=5.0, sx=0.3)
    c = mkc(C, off, (0.02, -0.14, 0.06), (0, 0, 0.03), lens=100, dof=0.14, fstop=2.8)
    L.key_cam(c, 0, loc=off + V((0.02, -0.14, 0.06)), target=off + V((0, 0, 0.03)))
    return c


def _city(off, C):
    rr = random.Random(5)
    for i in range(26):
        x = (i % 13 - 6) * 0.030
        y = (i // 13) * 0.045
        h = 0.03 + 0.10 * abs(rr.gauss(0, 1))
        L.prim('cube', C, C + "_c%d" % i, loc=off + V((x, y, h * 0.5)), scale=(0.022, 0.022, h), material=M('ink'))
        L.prim('cube', C, C + "_ct%d" % i, loc=off + V((x, y, h + 0.002)), scale=(0.018, 0.018, 0.003), material=M('mint'))
    key_area(C, off, rel=(0, -0.4, 0.5), tgt=(0, 0, 0.06), power=40.0, sx=0.6)
    c = mkc(C, off, (0.0, -0.34, 0.10), (0, 0, 0.07), lens=50, dof=None, fstop=8.0)
    L.key_cam(c, 0, loc=off + V((0.0, -0.34, 0.10)), target=off + V((0, 0, 0.07)))
    return c


def _drop(off, C):
    L.prim('cyl', C, C + "_d", loc=off, r=0.12, d=0.004, material=M('vinyl'))
    L.prim('ico', C, C + "_w", loc=off + V((0, 0, 0.030)), r=0.004, sub=3, material=M('water'))
    for k in range(7):
        L.prim('torus', C, C + "_r%d" % k, R=0.004 + k * 0.006, r=0.0007, loc=off + V((0, 0, 0.021)),
               material=M('mint'), mv=64, nv=4)
    key_spot(C, off, rel=(-0.1, -0.15, 0.30), power=20.0, size=26)
    c = mkc(C, off, (0.03, -0.10, 0.08), (0, 0, 0.01), lens=100, dof=0.13, fstop=2.8)
    L.key_cam(c, 0, loc=off + V((0.03, -0.10, 0.08)), target=off + V((0, 0, 0.01)))
    return c


def _boxes_open(off, C):
    for i, x in enumerate((-0.16, 0.0, 0.16)):
        L.prim('cube', C, C + "_b%d" % i, loc=off + V((x, 0, 0.03)), scale=(0.13, 0.13, 0.06), material=M('paper'))
        lid = L.prim('cube', C, C + "_l%d" % i, loc=off + V((x, -0.065, 0.075)),
                     scale=(0.13, 0.13, 0.008), material=M('paper2'))
        lid.rotation_euler = (rd(-55), 0, 0)
        L.prim('cube', C, C + "_g%d" % i, loc=off + V((x, 0, 0.055)), scale=(0.10, 0.10, 0.002), material=M('mint'))
    key_area(C, off, rel=(0, 0, 0.6), tgt=(0, 0, 0), power=30.0, sx=0.8)
    c = mkc(C, off, (0, -0.42, 0.52), (0, 0, 0.02), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, 0, loc=off + V((0, -0.42, 0.52)), target=off + V((0, 0, 0.02)))
    return c


def _starpinhole(off, C):
    L.prim('cyl', C, C + "_d1", loc=off + V((0, 0, 0.020)), r=0.150, d=0.004, material=M('vinyl'))
    L.prim('cyl', C, C + "_d2", loc=off + V((0, 0, 0.026)), r=0.150, d=0.004, material=M('vinyl'))
    L.prim('cyl', C, C + "_ring", r=0.008, d=0.010, loc=off + V((0, 0, 0.024)), material=M('alu'))
    rr = random.Random(31)
    pts = [V((rr.gauss(0, 0.012), rr.gauss(0, 0.012), rr.uniform(-0.30, -0.001))) for _ in range(240)]
    points_mesh(C, C + "_stars", [off + p for p in pts], size=0.0007, mat='white_e', sub=1)
    key_area(C, off, rel=(0.2, -0.2, 0.3), tgt=(0, 0, 0.02), power=12.0, sx=0.4)
    c = mkc(C, off, (0.0, -0.055, 0.024), (0, 0, 0.023), lens=100, dof=0.055, fstop=2.0)
    L.key_cam(c, 0, loc=off + V((0.0, -0.055, 0.024)), target=off + V((0, 0, 0.023)))
    return c


MONTAGE = [_glass_ripple, _needle, _hand, _scopes, _blink, _tape, _city, _drop, _boxes_open, _starpinhole]


def s19(off, C, f0, f1):
    out = []
    n = 10
    span = (f1 - f0 + 1) / float(n)
    for i, fn in enumerate(MONTAGE):
        sub = off + V((i * 8.0, 0, 0))
        cc = C + "_m%d" % i
        cam = fn(sub, cc)
        fa = int(f0 + i * span)
        fb = int(f0 + (i + 1) * span) - 1
        L.key_cam(cam, fa, loc=cam.location.copy(), target=cam.location + (cam.matrix_world.to_quaternion() @ V((0, 0, -1))))
        out.append((cam, fa, fb))
    return out


# ---------------- 21  light band -> grid ----------------
def s21(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.20)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0020, loc=off + V((0, 0, 0.20)), material=M('mint'), mv=128, nv=6)
    for k in range(6):
        L.prim('torus', C, C + "_band%d" % k, R=0.30 + k * 0.34, r=0.0016 - 0.00018 * k,
               loc=off + V((0, 0, 0.20)), material=M('paper'), mv=160, nv=4)
    for i in range(9):
        L.prim('cube', C, C + "_gx%d" % i, loc=off + V((0, -1.4 + i * 0.35, 0.20)), scale=(2.6, 0.0012, 0.0012),
               material=M('mint'))
        L.prim('cube', C, C + "_gy%d" % i, loc=off + V((-1.4 + i * 0.35, 0, 0.20)), scale=(0.0012, 2.6, 0.0012),
               material=M('mint'))
    key_spot(C, off, rel=(-0.5, -0.8, 1.2), power=70.0, size=30)
    c = mkc(C, off, (0.0, -0.42, 0.26), (0, 0, 0.20), lens=50, dof=0.45, fstop=2.8)
    L.key_cam(c, f0, loc=off + V((0.0, -0.42, 0.26)), target=off + V((0, 0, 0.20)))
    L.key_cam(c, f1, loc=off + V((0.0, -3.10, 0.55)), target=off + V((0, 0, 0.20)))
    return c


# ---------------- 22 / 23  holographic UI ----------------
def ui_panel(off, C):
    w, h = 1.60, 0.90
    z = 0.30
    def bar(x, y, sx, sy, m='paper'):
        return L.prim('cube', C, C + "_u%d" % R.randint(0, 99999), loc=off + V((x, 0, z + y)),
                      scale=(sx, 0.0016, sy), material=M(m))
    bar(0.0, h / 2, w, 0.0018)
    bar(0.0, h / 2 - 0.06, w, 0.0006, 'moss')
    bar(0.0, -h / 2, w, 0.0018)
    bar(-w / 2, 0, 0.0018, h)
    bar(w / 2, 0, 0.0018, h)
    for i in range(1, 4):
        bar(0.0, h / 2 - 0.06 - i * 0.20, w, 0.0006, 'moss')
    for i in range(4):
        bar(-w / 2 + 0.12, h / 2 - 0.14 - i * 0.16, 0.20, 0.0030)
    for r in range(4):
        for k in range(6):
            x = -0.05 + k * 0.26
            bar(x, h / 2 - 0.30 - r * 0.15, 0.18 * (0.5 + 0.5 * ((k * 7 + r * 3) % 5) / 4.0), 0.0075, 'ink' if (k + r) % 3 else 'moss')


def s22(off, C, f0, f1):
    ui_panel(off, C)
    L.prim('cube', C, C + "_floor", loc=off + V((0, 0, -0.02)), scale=(6, 6, 0.004), material=M('ink'))
    key_area(C, off, rel=(0, -1.2, 1.0), tgt=(0, 0, 0.30), power=120.0, sx=2.0)
    c = mkc(C, off, (0.0, -2.20, 0.30), (0, 0, 0.30), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((-0.55, -2.10, 0.34)), target=off + V((0, 0, 0.30)))
    L.key_cam(c, f1, loc=off + V((0.55, -2.10, 0.34)), target=off + V((0, 0, 0.30)))
    L.interp(c, 'LINEAR')
    return c


def s23(off, C, f0, f1):
    ui_panel(off, C)
    L.prim('cube', C, C + "_floor", loc=off + V((0, 0, -0.02)), scale=(6, 6, 0.004), material=M('ink'))
    key_area(C, off, rel=(0, -1.2, 1.0), tgt=(0, 0, 0.30), power=140.0, sx=2.0)
    c = mkc(C, off, (0.0, -1.6, 0.30), (0, 0, 0.30), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -1.30, 0.30)), target=off + V((0, 0, 0.30)))
    L.key_cam(c, f1, loc=off + V((0.0, 0.90, 0.30)), target=off + V((0, 0, 0.30)))
    L.interp(c, 'LINEAR')
    return c


# ---------------- 24  UI as architecture ----------------
def s24(off, C, f0, f1):
    archive_grid(C, off, nx=7, ny=12, pitch=0.42)
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 2.2, 1.30)), r=0.15, d=0.004, material=M('mint'))
    L.smooth(d)
    # the play-button shaped hole
    L.prim('cyl', C, C + "_hole", loc=off + V((0, 1.2, 0.62)), r=0.30, d=0.010, material=M('ink'))
    tri = L.prim('cone', C, C + "_play", loc=off + V((0.03, 1.16, 0.62)), r1=0.11, d=0.012, v=3,
                 material=M('ink'))
    tri.rotation_euler = (rd(90), 0, 0)
    key_area(C, off, rel=(0, -2.0, 4.0), tgt=(0, 2.0, 0.5), power=1400.0, sx=5.0)
    c = mkc(C, off, (-2.6, -1.6, 1.10), (0, 2.4, 0.55), lens=18, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((-2.6, -1.6, 1.10)), target=off + V((0, 2.4, 0.55)))
    L.key_cam(c, f1, loc=off + V((2.6, -1.6, 1.10)), target=off + V((0, 2.4, 0.55)))
    L.interp(c, 'LINEAR')
    return c


# ---------------- 25  the beam presses ----------------
def s25(off, C, f0, f1):
    L.prim('cyl', C, C + "_hole", loc=off + V((0, 0, 0.0)), r=0.26, d=0.008, material=M('ink'))
    tri = L.prim('cone', C, C + "_play", loc=off + V((0.03, 0, 0.0)), r1=0.10, d=0.010, v=3, material=M('ink'))
    tri.rotation_euler = (rd(90), 0, 0)
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.55)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0020, loc=off + V((0, 0, 0.55)), material=M('mint'), mv=128, nv=6)
    shaft(C, off, strength=3.4, w=0.016, h=1.2, y=0)
    for k in range(5):
        L.prim('torus', C, C + "_w%d" % k, R=0.32 + k * 0.16, r=0.0012, loc=off + V((0, 0, 0.02)),
               material=M('mint'), mv=160, nv=4)
    key_spot(C, off, rel=(-0.6, -0.7, 1.4), power=110.0, size=30)
    c = mkc(C, off, (0.0, -0.55, 0.62), (0, 0, 0.45), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -0.50, 0.70)), target=off + V((0, 0, 0.45)))
    L.key_cam(c, f1, loc=off + V((0.0, -1.75, 0.95)), target=off + V((0, 0, 0.34)))
    L.interp(c, 'OUT')
    return c


# ---------------- 26  whiteout ----------------
def s26(off, C, f0, f1):
    p = L.prim('plane', C, C + "_white", loc=off + V((0, 0, 1.0)), rot=(rd(90), 0, 0), size=6.0,
               material=M('white_e'))
    dot = L.prim('cyl', C, C + "_dot", loc=off + V((0.0, 0.9, 1.0)), rot=(rd(90), 0, 0), r=0.010, d=0.004,
                 material=M('ink'))
    L.area(C + "_key", C, loc=off + V((0, -2, 1.4)), target=off + V((0, 0, 1.0)), size=3.0, power=200.0)
    c = mkc(C, off, (0, -2.2, 1.0), (0, 0, 1.0), lens=50, ortho=1.6)
    L.key_cam(c, f0, loc=off + V((0, -2.2, 1.0)), target=off + V((0, 0, 1.0)))
    dot.scale = (1, 1, 1)
    dot.keyframe_insert("scale", frame=f0)
    dot.scale = (0.1, 0.1, 1)
    dot.keyframe_insert("scale", frame=f1)
    return c


# ---------------- 28  disc in the shaft, archive behind ----------------
def s28(off, C, f0, f1):
    archive_grid(C, off, nx=6, ny=8, pitch=0.42)
    shaft(C, off, strength=1.8, w=0.05, h=1.4, y=-0.6)
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, -0.6, 0.30)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0016, loc=off + V((0, -0.6, 0.30)), material=M('mint'), mv=128, nv=6)
    key_area(C, off, rel=(0, -3.6, 2.4), tgt=(0, 0.6, 0.2), power=900.0, sx=4.0)
    c = mkc(C, off, (0.0, -1.30, 0.36), (0, -0.6, 0.30), lens=50, dof=0.70, fstop=2.0)
    move_orbit(c, f0, f1, off + V((0, -0.6, 0.30)), 0.72, -90, -78, 0.06, 0.05, tgt=off + V((0, -0.6, 0.30)))
    return c


# ---------------- 29  pull back to a point ----------------
def s29(off, C, f0, f1):
    d = L.prim('cyl', C, C + "_disc", loc=off + V((0, 0, 0.30)), r=0.15, d=0.004, material=M('vinyl'))
    L.smooth(d)
    L.prim('torus', C, C + "_rim", R=0.150, r=0.0018, loc=off + V((0, 0, 0.30)), material=M('mint'), mv=128, nv=6)
    key_spot(C, off, rel=(-0.4, -0.6, 1.0), power=40.0, size=24)
    c = mkc(C, off, (0.0, -0.45, 0.34), (0, 0, 0.30), lens=25, dof=None, fstop=8.0)
    L.key_cam(c, f0, loc=off + V((0.0, -0.45, 0.34)), target=off + V((0, 0, 0.30)))
    L.key_cam(c, f1, loc=off + V((0.0, -5.20, 0.10)), target=off + V((0, 0, 0.32)))
    L.interp(c, 'LINEAR')
    return c


# ---------------- 30/31/32  the ruler line ----------------
def _ortho(off, C, f0, f1, scale=1.6):
    c = mkc(C, off, (0, -2.0, 0.0), (0, 0, 0), lens=50, ortho=scale)
    c.data.clip_start = 0.01
    return c


def s30(off, C, f0, f1):
    px = 1.6 / 720.0
    line = L.prim('cube', C, C + "_line", loc=off + V((0.0, 0, -0.18)), scale=(0.52, px * 0.9, px * 0.9),
                  material=M('paper'))
    blk = L.prim('cube', C, C + "_blk", loc=off + V((-0.52, 0, -0.18 - 0.026)), scale=(0.020, 0.0016, 0.020),
                 material=M('orange'))
    L.area(C + "_key", C, loc=off + V((0, -1, 0.5)), target=off, size=2.0, power=60.0)
    c = _ortho(off, C, f0, f1)
    L.key_cam(c, f0, loc=off + V((0, -2.0, 0)), target=off + V((0, 0, 0)))
    blk.scale = (0.25, 1, 1)
    blk.keyframe_insert("scale", frame=f0)
    blk.scale = (1, 1, 1)
    blk.keyframe_insert("scale", frame=f0 + 3)
    return c


def s31(off, C, f0, f1):
    px = 1.6 / 720.0
    L.prim('cube', C, C + "_line", loc=off + V((0.0, 0, -0.18)), scale=(0.52, px * 0.9, px * 0.9),
           material=M('paper'))
    L.prim('cube', C, C + "_blk", loc=off + V((-0.52, 0, -0.206)), scale=(0.020, 0.0022, 0.020), material=M('orange'))
    # wordmark as unreadable glyph blocks, letterspaced
    for i in range(9):
        w = 0.006 + 0.004 * ((i * 5) % 3)
        L.prim('cube', C, C + "_g%d" % i, loc=off + V((-0.28 + i * 0.030, 0, -0.085)),
               scale=(w, 0.0020, 0.011 + 0.004 * (i % 2)), material=M('paper'))
    c = _ortho(off, C, f0, f1)
    L.key_cam(c, f0, loc=off + V((0, -2.0, 0)), target=off + V((0, 0, 0)))
    return c


def s32(off, C, f0, f1):
    px = 1.6 / 720.0
    line = L.prim('cube', C, C + "_line", loc=off + V((0.0, 0, -0.18)), scale=(0.52, px * 0.9, px * 0.9),
                  material=M('paper'))
    c = _ortho(off, C, f0, f1)
    L.key_cam(c, f0, loc=off + V((0, -2.0, 0)), target=off + V((0, 0, 0)))
    line.scale = (0.52, px * 0.9, px * 0.9)
    line.keyframe_insert("scale", frame=f0)
    line.scale = (0.52, px * 0.9, px * 0.9)
    line.keyframe_insert("scale", frame=f0 + 2)
    line.scale = (0.0, px * 0.9, px * 0.9)
    line.keyframe_insert("scale", frame=f1)
    return c
