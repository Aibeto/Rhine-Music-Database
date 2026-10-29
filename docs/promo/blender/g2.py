# -*- coding: utf-8 -*-
"""GROUP 2 — shots 06a / 06b / 06c / 07 / 08  (needle lands / groove = canyon / light grid /
chladni sand / dive into the hole).

Flat emission only, no lights, palette paper-ink-moss-alu-vinyl-mint, black surround,
one subject per frame, no orange, no text.

Two rig notes for whoever reads this next:
  * stages.move() used to drop the collection offset; we keep our own offset-safe `mv()`
    (per-frame keys, so the 12 fps stepping is explicit).
  * `box()` stores the dimension in obj.scale, so animating "scale" on a box destroys its size.
    Anything that must grow is built with L.prim and keyed with the full dimension baseline.
"""
import math
import random

import shotkit as K
from shotkit import V, M, cam_at, move, disc, box, torus, points_mesh, dust  # noqa: F401

L = K.L
R = math.radians
PI = math.pi


# ---------------------------------------------------------------- helpers
def _kf(ob, path, f, val):
    setattr(ob, path, val)
    ob.keyframe_insert(path, frame=f)
    return ob


def _ease(kind, u):
    if kind == 'IO':
        return u * u * (3 - 2 * u)
    if kind == 'IN':
        return u ** 2.6
    if kind == 'OUT':
        return 1 - (1 - u) ** 3
    return u


def mv(cam, off, f0, f1, c, size, az=0.0, el=0.0, fill=1.0,
       az2=None, el2=None, fill2=None, ease='IO'):
    """Offset-safe camera move, one key per integer frame. frame half-height = size/fill."""
    lens = cam.data.lens
    az2 = az if az2 is None else az2
    el2 = el if el2 is None else el2
    fill2 = fill if fill2 is None else fill2
    base = off + V(c)
    d0 = size * (lens / K.SENSOR_HALF) / max(fill, 0.05)
    d1 = size * (lens / K.SENSOR_HALF) / max(fill2, 0.05)
    for f in range(f0, f1 + 1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        uu = _ease(ease, u)
        a, e = R(az + (az2 - az) * uu), R(el + (el2 - el) * uu)
        dd = d0 + (d1 - d0) * uu
        p = base + V((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e),
                      math.sin(e))) * dd
        L.key_cam(cam, f, loc=p, target=base)
    return cam


def plate(C, off, name, r, z=0.0, thick=0.007):
    """Near-black platter with a thin lit rim (flat emission, no light)."""
    d = disc(C, C + name, (0, 0, z - thick * 0.5), r, off, 'ink', thick=thick)
    d.data.materials.clear()
    d.data.materials.append(K.flat("F_PLATE", (0.016, 0.018, 0.017), fac=0.45))
    torus(C, C + name + "_rim", (0, 0, z - 0.0006), r - 0.0015, 0.0017, off, 'alu2', mv=160)
    return d


def steps(f0, f1):
    return list(range(f0, f1 + 1))


# ---------------------------------------------------------------- 06a
def s06a(off, C, f0, f1):
    """100 mm macro. Stylus drops onto the turning platter, one pose per frame = 12 fps steps."""
    c, s = (0.030, 0.0, 0.0075), 0.021
    plate(C, off, "_pl", 0.175)

    # grooves = hairlines of geometry (a texture cannot carry this scale)
    for k in range(23):
        rr = 0.011 + k * 0.0042
        mat = 'alu2' if k % 4 == 2 else 'alu'
        torus(C, C + "_g%02d" % k, (0, 0, 0.0002), rr, 0.00021 if k % 4 == 2 else 0.00013,
              off, mat, mv=160)
    torus(C, C + "_sh", (0, 0, 0.0003), 0.0985, 0.00030, off, 'paper2', mv=160)

    # a hairline scratch that jumps every frame -> the plate turns
    scr = L.prim('cube', C, C + "_scr", loc=off + V((0.084, 0, 0.00016)),
                 scale=(0.00030, 0.070, 0.00010), material=M('alu'))

    tip, hgt = 0.00030, 0.0125
    nd = L.prim('cone', C, C + "_nd", loc=off + V((c[0], 0, tip + hgt * 0.5)),
                r1=0.0022, r2=0.00008, d=hgt, v=32, material=M('paper'))
    nd.rotation_euler = (R(180), 0, R(0))
    cant = L.prim('cube', C, C + "_cant", loc=off + V((c[0] + 0.0012, 0.0035, tip + hgt + 0.0085)),
                  scale=(0.0016, 0.0016, 0.020), material=M('alu2'))
    cant.rotation_euler = (R(-26), 0, 0)
    head = L.prim('cube', C, C + "_head", loc=off + V((c[0] + 0.0012, 0.0116, tip + hgt + 0.0175)),
                  scale=(0.0060, 0.0075, 0.0055), material=M('paper2'))
    head.rotation_euler = (R(-26), 0, 0)
    spark = L.prim('ico', C, C + "_spark", loc=off + V((c[0], 0, 0.00035)),
                   r=0.00080, sub=2, material=K.flat("F_HOT", (1, 1, 1), fac=1.0, strength=5.0))

    for f in steps(f0, f1):
        u = f - f0
        dz = 0.0064 * math.exp(-1.45 * u)          # free fall, hard stop on frame 3
        if u >= 3:
            dz = 0.0
        _kf(nd, "location", f, off + V((c[0], 0, dz + tip + hgt * 0.5)))
        _kf(cant, "location", f, off + V((c[0] + 0.0012, 0.0035, dz + tip + hgt + 0.0085)))
        _kf(head, "location", f, off + V((c[0] + 0.0012, 0.0116, dz + tip + hgt + 0.0175)))
        _kf(spark, "scale", f, (1.0, 1.0, 1.0) if u >= 3 else (0.001, 0.001, 0.001))
        th = R(24.0 + 5.2 * u)
        _kf(scr, "location", f, off + V((0.084 * math.cos(th), 0.084 * math.sin(th), 0.00016)))
        _kf(scr, "rotation_euler", f, (0, 0, th + R(90)))

    cam = cam_at(C, off, c, s, az=12, el=23, fill=1.0, lens=100, dof_k=1.0, fstop=3.2)
    mv(cam, off, f0, f1, c, s, az=12, el=23, fill=1.02, az2=16, el2=22, fill2=0.95)
    return cam


# ---------------------------------------------------------------- 06b
def s06b(off, C, f0, f1):
    """100 mm macro. One scratch read as a canyon; a blade of light runs up the valley floor."""
    c, s = (0.0, 0.038, 0.0070), 0.017
    hw, y0, y1 = 0.016, -0.085, 0.162
    yl, yc = y1 - y0, 0.5 * (y0 + y1)
    wh, wt = 0.0105, 0.0020

    fl = box(C, C + "_floor", (0, yc, -0.0009), (2 * hw, yl, 0.0018), 'ink', off)
    fl.data.materials.clear()
    fl.data.materials.append(K.flat("F_VALLEY", (0.030, 0.034, 0.031), fac=0.55))
    for sx, nm in ((-1, "L"), (1, "R")):
        box(C, C + "_w" + nm, (sx * (hw + wt * 0.5), yc, wh * 0.5 - 0.0009),
            (wt, yl, wh), 'alu', off)
        box(C, C + "_rim" + nm, (sx * (hw + wt * 0.5), yc, wh - 0.0005),
            (wt * 1.5, yl, 0.00040), 'alu2', off)
        tp = box(C, C + "_top" + nm, (sx * (hw + wt + 0.022), yc, wh - 0.0005),
                 (0.044, yl, 0.0006), 'ink', off)
        tp.data.materials.clear()
        tp.data.materials.append(K.flat("F_DECK", (0.014, 0.016, 0.015), fac=0.30))
        for si, hz in enumerate((0.34, 0.60, 0.82)):
            box(C, C + "_st%d%s" % (si, nm), (sx * (hw - 0.0003), yc, -0.0009 + wh * hz),
                (0.0009, yl, 0.00055), 'moss', off)
    rock = box(C, C + "_rock", (0.052, -0.048, 0.012), (0.055, 0.065, 0.042), 'ink', off)
    rock.data.materials.clear()
    rock.data.materials.append(K.flat("F_ROCK", (0.022, 0.024, 0.023), fac=0.40))
    rr = random.Random(9)
    for i in range(7):
        px = rr.choice((-1, 1)) * rr.uniform(hw * 0.62, hw * 0.88)
        py = rr.uniform(-0.05, 0.130)
        h = rr.uniform(0.0012, 0.0026)
        box(C, C + "_pb%d" % i, (px, py, h * 0.5 - 0.0009),
            (h * rr.uniform(0.8, 1.5), h * rr.uniform(0.8, 1.4), h), 'alu2', off)

    sl = box(C, C + "_sl", (0, 0.30, -0.0004), (3.4 * hw, 0.0011, 0.0011), 'white_e', off)
    sl.data.materials.clear()
    sl.data.materials.append(K.flat("F_BLADE", (1.0, 1.0, 0.99), fac=1.0, strength=7.0))
    pool = box(C, C + "_pool", (0, 0.30, -0.0006), (3.2 * hw, 0.055, 0.0005), 'moss', off)
    pool.data.materials.clear()
    pool.data.materials.append(K.flat("F_SPILL", (0.62, 0.62, 0.58), fac=1.0, strength=0.42))
    for f in steps(f0, f1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        yy = 0.138 - 0.108 * (u ** 0.80)
        _kf(sl, "location", f, off + V((0, yy, -0.0004)))
        _kf(pool, "location", f, off + V((0, yy + 0.026, -0.0006)))

    cam = cam_at(C, off, c, s, az=0, el=17, fill=1.0, lens=100, dof_k=1.25, fstop=9.0)
    mv(cam, off, f0, f1, c, s, az=0, el=17, fill=1.0, el2=15, fill2=0.90)
    return cam


# ---------------------------------------------------------------- 06c
def s06c(off, C, f0, f1):
    """25 mm. A blade of light crosses the platter; a mint lattice is printed in its wake."""
    c, s = (0, 0, 0.002), 0.170
    plate(C, off, "_pl", 0.168, thick=0.008)

    Rg, n, pitch = 0.150, 13, 0.0240
    base = -0.5 * (n - 1) * pitch
    lw, lh = 0.00062, 0.00050
    mint = K.flat('F_GRID', (0.24, 0.80, 0.56), fac=0.90, strength=0.85)

    def front(u):
        uu = min(u, 0.86) / 0.86
        return -0.178 + 0.328 * (uu ** 1.15)

    objs = []
    for i in range(n):
        x = base + i * pitch
        half = math.sqrt(max(Rg * Rg - x * x, 0.0025))
        ob = L.prim('cube', C, C + "_gx%02d" % i, loc=off + V((x, 0, 0.0012)),
                    scale=(lw, 2 * half, lh), material=mint)
        objs.append((ob, 'x', x, 2 * half))
    for j in range(n):
        y = base + j * pitch
        half = math.sqrt(max(Rg * Rg - y * y, 0.0025))
        ob = L.prim('cube', C, C + "_gy%02d" % j, loc=off + V((0, y, 0.0012)),
                    scale=(2 * half, lw, lh), material=mint)
        objs.append((ob, 'y', y, 2 * half))

    for f in steps(f0, f1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        fr = front(u)
        for ob, ax, v, ln in objs:
            if ax == 'x':
                k = max(0.0008, min(1.0, (fr - v + 0.02) / 0.085))
                _kf(ob, "scale", f, (lw, ln * k, lh))
            else:
                k = max(0.0008, min(1.0, (fr + ln * 0.5) / ln))
                _kf(ob, "scale", f, (ln * k, lw, lh))

    bar = L.prim('cube', C, C + "_bar", loc=off + V((0, 0, 0.0026)),
                 scale=(0.0026, 0.320, 0.0022),
                 material=K.flat("F_BAR", (1, 1, 1), fac=1.0, strength=4.6))
    for f in steps(f0, f1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        x = front(u)
        chord = 2.0 * math.sqrt(max(Rg * Rg - x * x, 0.0)) + 0.004
        fade = 1.0 if u < 0.86 else max(0.0008, 1.0 - (u - 0.86) / 0.14)
        _kf(bar, "location", f, off + V((x, 0, 0.0026)))
        _kf(bar, "scale", f, (0.0026, chord * fade, 0.0022 * fade))

    cam = cam_at(C, off, c, s, az=-34, el=27, fill=1.0, lens=25, dof_k=None, fstop=4.0)
    mv(cam, off, f0, f1, c, s, az=-34, el=27, fill=1.02, az2=-8, el2=24, fill2=0.94, ease='OUT')
    return cam


# ---------------------------------------------------------------- 07
def _sand_figure(seed=5):
    """Nodal lines of a (6,4) circular-plate mode: six spokes + scalloped node rings."""
    rr = random.Random(seed)
    pts = []
    lobe = 6                               # six petals
    rings = (0.038, 0.072, 0.104, 0.130)
    for ri, R0 in enumerate(rings):
        amp = 0.0060 + 0.0130 * (ri / 3.0)
        n = int(2 * PI * R0 / 0.0050)
        ph = 0.0 if ri % 2 == 0 else PI / 6.0
        for i in range(n):
            th = 2 * PI * i / n
            rad = R0 + amp * math.cos(lobe * th + ph)
            rr2 = rad + rr.gauss(0, 0.0014)
            pts.append((rr2 * math.cos(th), rr2 * math.sin(th), 0.0))
    for s_i in range(6):
        th = 2 * PI * s_i / 6.0 + 0.10
        n = 52
        for i in range(n):
            rad = 0.018 + (0.140 - 0.018) * (i / float(n - 1))
            jj = rr.gauss(0, 0.0011)
            th2 = th + jj / max(rad, 0.02)
            pts.append((rad * math.cos(th2), rad * math.sin(th2), 0.0))
    for i in range(26):                     # a little heap where nothing moves
        th, rad = rr.uniform(0, 2 * PI), 0.014 * math.sqrt(rr.random())
        pts.append((rad * math.cos(th), rad * math.sin(th), 0.0))
    return pts


def s07(off, C, f0, f1):
    """100 mm straight down. Dry sand on a turning platter jumps into a petal node figure."""
    c, s = (0, 0, 0.005), 0.150
    plate(C, off, "_pl", 0.156, thick=0.008)

    sand = points_mesh(C, C + "_sand", _sand_figure(), size=0.0050, mat='paper', sub=1)
    sand.data.materials.clear()
    sand.data.materials.append(K.flat("F_SAND", (0.97, 0.96, 0.92), fac=0.65, strength=1.6))
    sand.location = off + V((0, 0, 0.0016))

    # grains in the air: three phase groups, they hop and fall back
    rr = random.Random(77)
    groups = []
    for gi in range(3):
        pts = []
        for _ in range(16):
            th = rr.uniform(0, 2 * PI)
            rad = 0.025 + 0.115 * math.sqrt(rr.random())
            pts.append((rad * math.cos(th), rad * math.sin(th), rr.uniform(0.000, 0.022)))
        ob = points_mesh(C, C + "_air%d" % gi, pts, size=0.0038, mat='paper', sub=1)
        ob.data.materials.clear()
        ob.data.materials.append(K.flat("F_SAND", (0.97, 0.96, 0.92), fac=0.65, strength=1.6))
        ob.location = off + V((0, 0, 0.0022))
        groups.append(ob)

    for f in steps(f0, f1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        for gi, ob in enumerate(groups):
            ph = 2 * PI * (2.4 * u + gi / 3.0)
            hop = 0.0115 * (0.5 + 0.5 * math.sin(ph))
            _kf(ob, "location", f, off + V((0, 0, 0.0022 + hop)))
        _kf(sand, "location", f, off + V((0, 0, 0.0016 + 0.00035 * math.sin(2 * PI * 4.8 * u))))

    cam = cam_at(C, off, c, s, az=6, el=88, fill=0.78, lens=100, dof_k=None, fstop=8.0)
    mv(cam, off, f0, f1, c, s, az=6, el=88, fill=0.78, az2=16, el2=85, fill2=0.87)
    return cam


# ---------------------------------------------------------------- 08
def s08(off, C, f0, f1):
    """25 mm. The innermost Chladni ring is a hole: the camera falls in, white leaks through."""
    n, gap = 17, 0.145
    for i in range(n):
        z = -i * gap
        rr = 0.160 * (1.0 - (i / float(n)) ** 1.55)
        torus(C, C + "_r%02d" % i, (0, 0, z), max(rr, 0.009),
              0.0014 + 0.0028 * (i / float(n)), off,
              'paper2' if i < 6 else ('moss' if i < 12 else 'paper'), mv=64)

    # the light inside the hole: it opens up as the camera commits
    zc = -(n - 1) * gap - 0.105
    hot = L.prim('cyl', C, C + "_core", loc=off + V((0, 0, zc)), r=0.050, d=0.004,
                 material=K.flat("F_CORE", (1.0, 1.0, 0.97), fac=1.0, strength=9.0))

    z0, z1 = 0.52, zc + 0.055
    cam = cam_at(C, off, (0, 0, 0.0), 0.16, az=0, el=90, fill=1.0, lens=25)
    cam.data.clip_start = 0.002

    kk, e = 2.4, math.exp(2.4) - 1.0
    p0 = V((0.095, -0.115, z0))
    for f in steps(f0, f1):
        u = (f - f0) / max(1.0, float(f1 - f0))
        sp = (math.exp(kk * u) - 1.0) / e
        p = p0 * (1.0 - sp) + V((0, 0, z1)) * sp
        k = 1.0 + 1.9 * (u ** 2.2)
        _kf(hot, "scale", f, (k, k, 1.0))
        L.key_cam(cam, f, loc=off + p, target=off + V((0, 0, p.z - 1.10)))
    return cam


SHOTS = [
    ("06a", 7.50, 8.04, s06a),
    ("06b", 8.04, 8.57, s06b),
    ("06c", 8.57, 9.11, s06c),
    ("07", 9.11, 10.71, s07),
    ("08", 10.71, 12.86, s08),
]
