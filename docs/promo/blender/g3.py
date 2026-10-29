# -*- coding: utf-8 -*-
"""G3 — 09 档案通道穿行 / 10 盒侧掠过 / 11 盒面光幕 / 12 数据柱同心环.

风格纪律：只有平面自发光（无灯，K.key_at/K.area_at 是空函数）。
调色板锁定 paper / paper2 / ink / moss / mint / white，镜头 09–12 不出现橙色。
画面里没有可读文字：盒侧"微缩字"是三到四条细长 ink 条纹，远看是字、近看是噪声。
背景纯黑，每帧一个主体。
"""
import bpy, random
from math import radians as rd, sin, cos, pi
import shotkit as K
from shotkit import V, M, cam_at, move, box, disc, points_mesh, dust, flat

LIFT = 9.0          # 每个镜头把自己的布景抬到独立层，互不穿帮（成片看不出来）

# ---------------------------------------------------------------- palette
PAL = {
    'paper':  (0.906, 0.906, 0.882),
    'paper2': (0.851, 0.847, 0.808),
    'ink':    (0.126, 0.133, 0.122),
    'moss':   (0.467, 0.482, 0.376),
    'mint':   (0.561, 0.890, 0.733),
    'white':  (1.0, 1.0, 1.0),
}


def _o(off, i):
    """镜头 i 的布景整体上移到 z = 9*i 层。相机与布景一起移，构图不变。"""
    return off + V((0.0, 0.0, LIFT * i))


def F(name, key, fac=0.35, strength=1.0):
    """Palette-locked flat emission (the only "light source" in these shots)."""
    return flat("G3_" + name, PAL[key], fac=fac, strength=strength)


def setmat(ob, mat):
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    return ob


def bx(C, name, c, dim, mat, off, bevel=0.0, base='paper'):
    """box() with a custom (usually brighter) flat material."""
    return setmat(box(C, name, c, dim, base, off, bevel=bevel), mat)


def grid(ob, ny, nz, py, pz):
    """Two Array modifiers (constant offset must be divided by object scale)."""
    sx, sy, sz = ob.scale
    if ny > 1:
        m = ob.modifiers.new("aY", 'ARRAY')
        m.count = ny
        m.use_relative_offset = False
        m.use_constant_offset = True
        m.constant_offset_displace = (0, py / sy, 0)
    if nz > 1:
        m = ob.modifiers.new("aZ", 'ARRAY')
        m.count = nz
        m.use_relative_offset = False
        m.use_constant_offset = True
        m.constant_offset_displace = (0, 0, pz / sz)
    return ob


def lerp(a, b, t):
    return V(a) + (V(b) - V(a)) * t


def aim_key(cam, f, loc, tgt):
    """Bake one camera pose (location + look-at rotation)."""
    cam.location = V(loc)
    cam.keyframe_insert("location", frame=f)
    cam.rotation_euler = (V(tgt) - V(loc)).to_track_quat('-Z', 'Y').to_euler()
    cam.keyframe_insert("rotation_euler", frame=f)


# ================================================================ archive wall
def wall(C, off, tag, side=1, A=1.30, y0=-3.0, ny=56, nz=11, py=0.44, pz=0.42,
         dim=(0.34, 0.30, 0.30), slot=None, label=True, slab=True, bays=5, strip=False,
         body_mat=None):
    """One archive wall running along +Y. side=+1 -> wall on +X, its face looks to -X.

    Returns the list of objects (so a shot can slide the whole rack).
    """
    hx, hy, hz = dim
    xc = side * (A + hx * 0.5)
    z0 = hz * 0.5
    obs = []
    body = bx(C, C + "_w" + tag, (xc, y0, z0), dim,
              body_mat or F("pbox", 'paper', 0.24, 0.88), off, bevel=0.004)
    grid(body, ny, nz, py, pz)
    obs.append(body)

    depth = ny * py                              # rack length along Y
    ymid = y0 + (ny - 1) * py * 0.5              # 板长=盒阵实长，别探出端头
    plen = (ny - 1) * py + hy                    # 层板长度：正好覆盖盒阵
    if slab:                                     # shelf plates: the "架" in 档案架
        for k in range(nz + 1):
            s = bx(C, C + "_sh%s%d" % (tag, k), (xc - side * 0.008, ymid, k * pz - 0.006),
                   (hx + 0.014, plen, 0.014), F("shelf", 'paper', 0.30, 0.55), off)
            obs.append(s)
    if bays:                                     # vertical uprights every N boxes
        for k in range(1, int(ny / bays) + 1):
            u = bx(C, C + "_up%s%d" % (tag, k), (xc - side * 0.012, y0 + (k * bays - 0.5) * py, nz * pz * 0.5),
                   (hx + 0.022, 0.022, nz * pz), F("up", 'paper', 0.30, 0.42), off)
            obs.append(u)

    fx = side * (A - 0.0018)                     # a hair proud of the face
    if label:
        # pseudo-glyphs: 4 thin ink bars, never readable
        for k, (dz, ln, th) in enumerate(((0.062, 0.116, 0.0075), (0.042, 0.070, 0.0055),
                                          (0.024, 0.096, 0.0075), (0.006, 0.038, 0.0040))):
            t = box(C, C + "_tx%s%d" % (tag, k), (fx, y0 + 0.004, z0 + dz),
                    (0.003, ln, th), 'ink', off)
            grid(t, ny, nz, py, pz)
            obs.append(t)
        # colour codes: moss and mint ticks on alternate boxes (double pitch, staggered)
        for k, (key, dy, ln) in enumerate((('moss', 0.0, 0.020), ('mint', py, 0.014))):
            t = bx(C, C + "_tk%s%d" % (tag, k), (fx, y0 - 0.088 + dy, z0 - 0.040),
                   (0.0025, ln, ln), F("tick" + key, key, 0.55, 1.5), off)
            grid(t, max(1, ny // 2), nz, py * 2.0, pz)
            obs.append(t)

    if slot:                                      # light leaking out of every box
        sw, sh, dz = slot
        s = bx(C, C + "_sl%s" % tag, (fx - side * 0.0006, y0, z0 + dz),
               (0.0025, sw, sh), F("seam", 'mint', 0.10, 2.4), off)
        grid(s, ny, nz, py, pz)
        obs.append(s)
        e = bx(C, C + "_sm%s" % tag, (fx - side * 0.0006, y0, z0 - hz * 0.5 + 0.020),
               (0.0025, sw * 0.94, 0.0045), F("seam2", 'mint', 0.10, 3.4), off)
        grid(e, ny, nz, py, pz)
        obs.append(e)

    if strip:                                     # 层板底下一条薄荷光带（透视里是长线）
        for k in range(nz + 1):
            st = bx(C, C + "_lt%s%d" % (tag, k),
                    (fx - side * 0.0012, ymid, k * pz - 0.026),
                    (0.0025, plen * 0.99, 0.0120), F("strip", 'mint', 0.12, 3.2), off)
            obs.append(st)
    return obs


# ================================================================ 09
def _frame(C, off, tag, y, ztop, W=2.60, bar=0.16, mat=None, z0=0.0):
    """A glowing rectangular frame = the corridor cross-section, seen edge-on."""
    o = []
    x = W * 0.5 - bar * 0.5
    for sx, nm in ((1, "r"), (-1, "l")):
        o.append(bx(C, C + "_fr" + tag + nm, (sx * x, y, (z0 + ztop) * 0.5),
                    (bar, bar, ztop - z0), mat, off))
    for sz, nm in ((1, "t"), (-1, "b")):
        o.append(bx(C, C + "_fr" + tag + nm, (0, y, (z0 + ztop) * 0.5 + sz * ((ztop - z0) * 0.5 - bar * 0.5)),
                    (W, bar, bar), mat, off))
    return o


def s09(off, C, f0, f1):
    """25mm 大全景：通道内 6m/s 穿行，等距盒子延伸到无穷远，尽头白光洗掉一切。"""
    off = _o(off, 0)
    c, s = (0.0, 30.0, 1.55), 2.0
    Y0, NY, PZ, NZ, A0 = -3.0, 58, 0.42, 11, 1.30
    for sd in (1, -1):
        wall(C, off, "p" if sd > 0 else "m", side=sd, A=A0, y0=Y0, ny=NY, nz=NZ,
             py=0.44, pz=PZ, dim=(0.34, 0.30, 0.30), slot=(0.20, 0.0048, -0.112),
             label=True, strip=True)

    # 尽头：不是贴一块白方片，而是把最后十几排盒子"本身"换成过曝的白 ——
    # 透视结构不中断，只是越往里越被光洗掉（层板仍是暗线，架子还在）。
    top = NZ * PZ
    for k, (r0, nyk, st) in enumerate(((42, 5, 2.2), (47, 5, 3.2), (52, 5, 4.6), (57, 2, 6.6))):
        for sd in (1, -1):
            wall(C, off, "%s%d" % ("p" if sd > 0 else "m", k), side=sd, A=A0 - 0.0025,
                 y0=Y0 + r0 * 0.44, ny=nyk, nz=NZ, py=0.44, pz=PZ, dim=(0.34, 0.30, 0.30),
                 slot=None, label=False, slab=False, bays=0,
                 body_mat=F("hot%d" % k, 'white', 1.0, st))
    bx(C, C + "_end", (0, Y0 + NY * 0.44 + 1.4, top * 0.5), (2.60, 0.05, top),
       F("end", 'white', 1.0, 7.0), off)
    dust(C, off, (0, 6.0, 1.7), 2.6, n=30, r_k=0.0030, seed=31)

    cam = cam_at(C, off, c, s, az=0, el=0.8, fill=0.1597, lens=25, fstop=8.0)
    # dist 31.0 -> 15.0 m in 2.58 s : 6.2 m/s
    move(cam, f0, f1, c, s, az=0, el=0.8, fill=0.1597, kind='push',
         fill2=0.3300, ease='LIN')
    return cam


# ================================================================ 10
def s10(off, C, f0, f1):
    """35mm 中景：贴盒侧掠过，每个盒子漏出一点微光；相机几乎不动，整架侧向平移。"""
    off = _o(off, 1)
    c, s = (0.17, -0.45, 0.54), 1.06
    obs = wall(C, off, "L", side=1, A=0.0, y0=-1.40, ny=6, nz=3, py=0.42, pz=0.40,
               dim=(0.34, 0.30, 0.28), slot=(0.19, 0.046, 0.050), label=True,
               slab=True, bays=3, strip=True)
    cam = cam_at(C, off, c, s, az=-52, el=8, fill=1.0, lens=35, dof_k=1.0, fstop=5.6)

    # 侧向平移：机位沿盒子长边走 (Y)，视线也轻微前送 —— 近处盒子出画、远处补位，才有视差
    base, dist0 = V(c), s * (35.0 / 10.1)

    def pose(d, a, e, dy, dty):
        dv = V((sin(rd(a)) * cos(rd(e)), -cos(rd(a)) * cos(rd(e)), sin(rd(e))))
        return off + base + dv * d + V((0, dy, 0)), off + base + V((0, dty, 0))
    for f in range(f0, f1 + 1):
        u = (f - f0) / float(max(1, f1 - f0))
        aim_key(cam, f, *pose(dist0 * (1.0 + 0.02 * u), -52 - 5 * u, 8 + u, 0.95 * u, 0.16 * u))
    return cam


# ================================================================ 11
def _win(obs, fa, fb):
    """Visible only from fa..fb. Bools are stepped, but before the first key the
    fcurve returns that key's value -> every window needs a hidden key up front."""
    for ob in obs:
        ob.hide_render = True
        ob.keyframe_insert("hide_render", frame=1)
        ob.hide_render = False
        ob.keyframe_insert("hide_render", frame=fa)
        ob.hide_render = True
        ob.keyframe_insert("hide_render", frame=fb + 1)
        ob.hide_render = True


WN = 0.280                                       # world image area (window is 0.30)


def _plate(C, name, off, y, w, h, dz, mat, x=0.0):
    return bx(C, name, (x, y, dz), (w, 0.0016, h), mat, off)


def world_sea(C, off, y):
    o = [_plate(C, C + "_sea_sky", off, y, WN, 0.190, 0.045, F("sky", 'paper2', 0.45, 0.55)),
         _plate(C, C + "_sea_wat", off, y, WN, 0.135, -0.0725, F("wat", 'paper', 0.30, 0.30)),
         _plate(C, C + "_sea_hz", off, y - 0.001, WN, 0.0040, -0.008, F("hz", 'white', 1.0, 5.5))]
    rr = random.Random(7)
    for k in range(5):                            # glints on the water
        w = 0.045 + 0.105 * rr.random()
        o.append(_plate(C, C + "_sea_g%d" % k, off, y - 0.002,
                        w, 0.0028, -0.030 - k * 0.021, F("glint", 'white', 1.0, 3.2),
                        x=rr.uniform(-0.085, 0.085)))
    return o


def world_dune(C, off, y):
    o = [_plate(C, C + "_du_sky", off, y, WN, WN, 0.0, F("dsky", 'paper', 0.45, 0.85)),
         _plate(C, C + "_du_gr", off, y - 0.001, WN, 0.062, -0.108, F("dgr", 'white', 0.45, 1.55))]
    cr = _plate(C, C + "_du_cr", off, y - 0.002, WN - 0.004, 0.0035, -0.077,
                F("dcr", 'white', 1.0, 2.6))          # 沙脊：一条微微倾斜的高光线
    cr.rotation_euler = (0, rd(-7), 0)
    o.append(cr)
    for k, (dx, r, dz) in enumerate(((-0.062, 0.042, -0.086), (0.004, 0.050, -0.082),
                                     (0.070, 0.038, -0.090))):
        d = disc(C, C + "_du%d" % k, (dx, y - 0.003, dz), r, off, 'paper2', thick=0.0014)
        d.rotation_euler = (rd(90), 0, 0)
        setmat(d, F("dune", 'white', 0.35, 1.75))
        o.append(d)
    sn = disc(C, C + "_du_sun", (0.070, y - 0.004, 0.058), 0.030, off, 'white_e', thick=0.0014)
    sn.rotation_euler = (rd(90), 0, 0)
    setmat(sn, F("sun", 'white', 1.0, 6.0))
    o.append(sn)
    return o


def world_city(C, off, y):
    rr = random.Random(11)
    o = [_plate(C, C + "_ci_sky", off, y, WN, WN, 0.0, F("csky", 'paper2', 0.55, 0.75)),
         # 一条压在天际线后面的高温光带（不是橙色，是过曝的白）
         _plate(C, C + "_ci_fire", off, y - 0.001, WN, 0.070, -0.085, F("fire", 'white', 0.95, 3.6))]
    for k in range(7):
        x = -0.117 + k * 0.039 + rr.uniform(-0.003, 0.003)
        h = 0.055 + 0.105 * min(1.0, abs(rr.gauss(0.55, 0.45)))
        ztop = -0.140 + h
        o.append(_plate(C, C + "_ci_b%d" % k, off, y - 0.003, 0.027, h, -0.140 + h * 0.5,
                        F("cblk", 'ink', 1.0, 0.30), x=x))
        o.append(_plate(C, C + "_ci_r%d" % k, off, y - 0.004, 0.027, 0.0038, ztop,
                        F("roof", 'white', 1.0, 5.0), x=x))
    pts = []
    rr2 = random.Random(12)
    for _ in range(24):
        pts.append(V((rr2.uniform(-0.13, 0.13), y - 0.004, rr2.uniform(-0.085, 0.105))))
    sp = points_mesh(C, C + "_ci_sp", pts, size=0.0028, mat='white_e', sub=1)
    setmat(sp, F("spark", 'white', 1.0, 4.6))
    o.append(sp)
    return o


def world_snow(C, off, y):
    o = [_plate(C, C + "_sn_sky", off, y, WN, WN, 0.0, F("ssky", 'paper2', 0.30, 0.34)),
         _plate(C, C + "_sn_gr", off, y - 0.001, WN, 0.104, -0.088, F("sgr", 'white', 0.40, 1.95)),
         _plate(C, C + "_sn_hz", off, y - 0.002, WN, 0.0026, -0.039, F("shz", 'ink', 1.0, 0.85))]
    rr = random.Random(23)
    for k in range(16):
        x, z = rr.uniform(-0.13, 0.13), rr.uniform(-0.135, -0.045)
        o.append(_plate(C, C + "_sn_d%d" % k, off, y - 0.003, 0.0040, 0.0040, z,
                        F("sdark", 'ink', 1.0, 0.7), x=x))
    return o


def s11(off, C, f0, f1):
    """50mm：一个盒子正面化成薄光幕，幕里闪过 海 / 沙丘 / 燃烧的城市 / 雪原。机身定格。"""
    off = _o(off, 2)
    c, s = (0.0, 0.0, 0.0), 0.215
    bx(C, C + "_box", (0, 0, 0), (0.34, 0.34, 0.34), F("pbox", 'paper', 0.24, 1.00), off, bevel=0.004)
    # 盒面：纸边 + 里面那块光幕
    for tag, dim, cen in (("t", (0.340, 0.010, 0.020), (0.0, 0.160)),
                          ("b", (0.340, 0.010, 0.020), (0.0, -0.160)),
                          ("l", (0.020, 0.010, 0.340), (0.160, 0.0)),
                          ("r", (0.020, 0.010, 0.340), (-0.160, 0.0))):
        bx(C, C + "_fr" + tag, (cen[0], -0.172, cen[1]), dim, F("frim", 'paper', 0.28, 1.35), off)
    for k, (dz, ln) in enumerate(((0.070, 0.110), (0.048, 0.072), (0.026, 0.094))):
        bx(C, C + "_tx%d" % k, (0, -0.1730, dz), (ln, 0.003, 0.006), F("gtx", 'ink', 1.0, 0.35), off)
    bx(C, C + "_tk", (0, -0.1730, -0.046), (0.014, 0.003, 0.016), F("gtk", 'moss', 0.55, 1.5), off)
    scr = [bx(C, C + "_scr", (0, -0.1765, 0), (0.300, 0.003, 0.300),
              F("scr", 'white', 0.90, 2.55), off)]
    # 光幕内缘的高光边（在世界图像之前，形成一圈亮框）
    for tag, dim, cen in (("t", (0.312, 0.004, 0.005), (0, 0.152)),
                          ("b", (0.312, 0.004, 0.005), (0, -0.152)),
                          ("l", (0.005, 0.004, 0.312), (0.152, 0)),
                          ("r", (0.005, 0.004, 0.312), (-0.152, 0))):
        scr.append(bx(C, C + "_eg" + tag, (cen[0], -0.193, cen[1]), dim,
                      F("edge", 'white', 1.0, 5.0), off))

    # 先 3 格"还是一个普普通通的盒子"，正面才化成光幕；幕里四个世界闪切，末尾烧白。
    _win(scr, f0 + 3, f1)
    w = [world_sea(C, off, -0.181), world_dune(C, off, -0.183),
         world_city(C, off, -0.185), world_snow(C, off, -0.187)]
    for i, obs in enumerate(w):
        fa = f0 + 3 + i * 4                       # 每个世界 4 帧，收尾停在雪原
        _win(obs, fa, fa + 3)
    cam = cam_at(C, off, c, s, az=11, el=6, fill=0.95, lens=50, dof_k=1.0, fstop=5.6)
    return cam


# ================================================================ 12
def s12(off, C, f0, f1):
    """25mm 仰拍：盒边拉长成数据柱，柱顶浮起光点，光点排成同心的环。升镜 + 上摇。"""
    off = _o(off, 3)
    NB, SH = 12, 0.10                      # 12 段，每段 10cm
    P0, P1 = 0.058, 0.175                  # 压缩 -> 拉开
    ZB = 0.18                              # 底下那个盒子（柱子的"出处"，也给尺度）
    bx(C, C + "_src", (0, 0, ZB * 0.5), (0.34, 0.34, ZB),
       F("src", 'paper', 0.28, 1.00), off, bevel=0.004)

    def zc(i, p):
        return ZB + SH * 0.5 + i * p

    for i in range(NB):
        z0, z1 = zc(i, P0), zc(i, P1)
        cmat = F("col", 'paper', 0.26, 1.25) if i % 2 == 0 else F("colm", 'mint', 0.30, 1.45)
        ob = bx(C, C + "_seg%02d" % i, (0, 0, z1), (0.205, 0.205, SH), cmat, off, bevel=0.002)
        ob.location = off + V((0, 0, z0))
        ob.keyframe_insert("location", frame=f0)
        ob.location = off + V((0, 0, z1))
        ob.keyframe_insert("location", frame=f1)
        bd = bx(C, C + "_bd%02d" % i, (0, 0, z1 - SH * 0.5 + 0.007), (0.221, 0.221, 0.013),
                F("band", 'mint', 0.12, 2.8), off)
        bd.location = off + V((0, 0, z0 - SH * 0.5 + 0.007))
        bd.keyframe_insert("location", frame=f0)
        bd.location = off + V((0, 0, z1 - SH * 0.5 + 0.007))
        bd.keyframe_insert("location", frame=f1)

    top0, top1 = zc(NB - 1, P0) + SH * 0.5, zc(NB - 1, P1) + SH * 0.5   # 0.918 -> 2.205
    cap = bx(C, C + "_cap", (0, 0, top1 + 0.007), (0.227, 0.227, 0.014),
             F("cap", 'white', 0.95, 3.6), off)
    cap.location = off + V((0, 0, top0 + 0.007))
    cap.keyframe_insert("location", frame=f0)
    cap.location = off + V((0, 0, top1 + 0.007))
    cap.keyframe_insert("location", frame=f1)

    # 光点：从柱顶冒出来、边升边张开，最后排成同心的环
    rings = ((0.30, 16), (0.52, 22), (0.78, 28), (1.06, 34), (1.36, 40))
    n = f1 - f0
    for i, (r, cnt) in enumerate(rings):
        pts = [V((r * cos(2 * pi * k / cnt), r * sin(2 * pi * k / cnt), 0.0))
               for k in range(cnt)]
        ob = setmat(points_mesh(C, C + "_ring%d" % i, pts, size=0.028, mat='white_e', sub=1),
                    F("ring", 'white', 1.0, 4.2))
        fa = f0 + int(n * (0.14 + 0.12 * i))
        fb = f1 - int(n * 0.10)

        def topat(fr):
            return top0 + (top1 - top0) * ((fr - f0) / float(max(1, f1 - f0)))
        for fr, dz, sc in ((f0, 0.03 + 0.02 * i, 0.02), (fa, 0.11 + 0.07 * i, 0.34),
                           (fb, 0.24 + 0.12 * i, 1.00), (f1, 0.29 + 0.12 * i, 1.05)):
            ob.location = off + V((0, 0, topat(fr) + dz))
            ob.scale = (sc, sc, sc)
            ob.keyframe_insert("location", frame=fr)
            ob.keyframe_insert("scale", frame=fr)
        ob.rotation_euler = (0, 0, 0)
        ob.keyframe_insert("rotation_euler", frame=fa)
        ob.rotation_euler = (0, 0, rd(16 + 11 * i))
        ob.keyframe_insert("rotation_euler", frame=f1)
    dust(C, off, (0, 0, 1.5), 1.2, n=26, r_k=0.0032, seed=41)

    # 机身：贴地低机位起，升 + 后退 + 视线顺着柱子往上摇
    p0 = (1.85, -30.0, -7.0, 0.40)      # dist, az, el, aim z
    p1 = (3.40, -22.0, -24.0, 1.70)

    def pose(p):
        d, a, e, tz = p
        loc = off + V((0, 0, tz)) + V((sin(rd(a)) * cos(rd(e)), -cos(rd(a)) * cos(rd(e)),
                                       sin(rd(e)))) * d
        return loc, off + V((0, 0, tz))

    l0, t0v = pose(p0)
    l1, t1v = pose(p1)
    cam = cam_at(C, off, (0, 0, p0[3]), p0[0] * (10.1 / 25.0), az=p0[1], el=p0[2],
                 fill=1.0, lens=25, fstop=8.0)
    cam.data.clip_start = 0.02
    for f in range(f0, f1 + 1):
        u = (f - f0) / float(max(1, f1 - f0))
        wgt = u * u * (3 - 2 * u)
        aim_key(cam, f, lerp(l0, l1, wgt), lerp(t0v, t1v, wgt))
    return cam


SHOTS = [
    ("09", 12.86, 15.54, s09),
    ("10", 15.54, 17.14, s10),
    ("11", 17.14, 18.75, s11),
    ("12", 18.75, 20.36, s12),
]
