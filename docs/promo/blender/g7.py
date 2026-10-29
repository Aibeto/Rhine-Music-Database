# -*- coding: utf-8 -*-
"""g7 — 25 / 26 / 27 / 28. 峰值与骤停。

25  25mm  一束光自己去按播放洞（没有手）→ 唱片起转 / 球面声波 / 粒子被推开 / 冲击波把镜头推开
26  —    白光满屏（正交），白里淡出一个极小黑点，不淡出
27  100mm 回到开场的光柱与尘埃，一模一样、完全静止
28  50mm  光柱里悬着唱片缓慢自转，背后是虚焦的米白档案架

纪律：全部平发光材质（K.flat / M），场景里没有灯。
橙色只在 25 镜 613–615 三帧存在（hide_render 常量插值门）。
"""
import bpy
from math import radians as rd, sin, cos
from mathutils import Vector, Quaternion
import shotkit as K
from shotkit import V, M, flat, flat_rings, cam_at, move, disc, box, torus, points_mesh, dust, SENSOR_HALF

L = K.L                                   # pvlib: primitives + emissive shaft material

FPS = K.FPS

VOID = (0.004, 0.004, 0.004)          # 纯黑的洞
COLD = (0.88, 0.93, 1.00)             # 唯一的光：冷白
ORANGE = (0.910, 0.384, 0.165)        # 莱茵橙（本组限 3 帧）


# ---------------------------------------------------------------- helpers
def _flat(name, rgb, fac=1.0, strength=1.0, alpha=1.0):
    return flat(name, rgb, fac=fac, strength=strength, alpha=alpha)


def _grooves(name, R, n=26, dark=(0.010, 0.012, 0.011), light=(0.105, 0.118, 0.109)):
    """Real concentric grooves: flat_rings takes rings/100 as wave Scale, i.e. Scale*R = ring count."""
    return flat_rings(name, dark, light, rings=100.0 * n / max(R, 1e-4))


def _setm(ob, mat):
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    return ob


def _ball(C, off, name, p, r, mat, sub=2):
    """One sphere whose object origin sits ON the sphere — so scale grows it in place."""
    return L.prim('ico', C, name, loc=off + V(p), r=r, sub=sub, material=M(mat))


def _curves(ob):
    """fcurve list for Blender 5 slotted actions, with legacy fallback."""
    ad = ob.animation_data
    if not ad or not ad.action:
        return []
    try:
        return list(ad.action.fcurves)
    except Exception:
        pass
    out = []
    try:
        for lay in ad.action.layers:
            for st in lay.strips:
                for cb in st.channelbags:
                    out.extend(cb.fcurves)
    except Exception:
        pass
    return out


def _const(ob):
    for fc in _curves(ob):
        for kp in fc.keyframe_points:
            kp.interpolation = 'CONSTANT'


def _gate(obs, f_on, n):
    """Visible on exactly n frames starting at f_on (constant interpolation, no half-frames)."""
    for ob in obs:
        ob.hide_render = True
        ob.keyframe_insert("hide_render", frame=f_on - 1)
        ob.hide_render = False
        ob.keyframe_insert("hide_render", frame=f_on)
        ob.hide_render = True
        ob.keyframe_insert("hide_render", frame=f_on + n)
        _const(ob)


def _view_q(az, el):
    """Quaternion putting a disc's +Z straight at a camera sitting at (az, el)."""
    a, e = rd(az), rd(el)
    return V((sin(a) * cos(e), -cos(a) * cos(e), sin(e))).to_track_quat('Z', 'Y')


def _spin(q0, deg):
    return q0 @ Quaternion((0, 0, 1), rd(deg))


def _rig(ob, cw, local, q):
    ob.rotation_mode = 'QUATERNION'
    ob.rotation_quaternion = q
    ob.location = V(cw) + q @ V(local)


def _spin_keys(objs, cw, locals_, q0, keys):
    """Rigid spin about the disc normal: object orbit + own rotation, keyed together.
    q0 may be one quaternion or one per object (ticks sit at their own azimuth)."""
    q0s = q0 if isinstance(q0, (list, tuple)) else [q0] * len(objs)
    for ob, loc, bq in zip(objs, locals_, q0s):
        ob.rotation_mode = 'QUATERNION'
        for f, deg in keys:
            q = _spin(bq, deg)
            ob.rotation_quaternion = q
            ob.location = V(cw) + q @ V(loc)
            ob.keyframe_insert("rotation_quaternion", frame=f)
            ob.keyframe_insert("location", frame=f)
    return objs


def _cam_at(cam, cw, size, az, el, fill):
    d = size * (cam.data.lens / SENSOR_HALF) / max(fill, 0.05)
    a, e = rd(az), rd(el)
    p = V(cw) + V((sin(a) * cos(e), -cos(a) * cos(e), sin(e))) * d
    return p


def _cam_keys(cam, cw, size, keys):
    """Explicit dolly keys. Only used where the kick has to be an impulse (25) —
    move() writes two keys, so its bezier can only ease in and out, never shove."""
    for f, fill, az, el in keys:
        p = _cam_at(cam, cw, size, az, el, fill)
        cam.location = p
        cam.rotation_euler = (V(cw) - p).to_track_quat('-Z', 'Y').to_euler()
        cam.keyframe_insert("location", frame=f)
        cam.keyframe_insert("rotation_euler", frame=f)
    return cam


def _col(C, off, name, c, dim, mat, rot=None):
    return box(C, name, c, dim, mat, off, rot=rot)


# ================================================================ 25
def s25(off, C, f0, f1):
    """一束光自己去按那个洞。没有手 —— 光柱下压、点亮、唱片起转、冲击波把镜头推开。"""
    c, s = (0, 0, 0.30), 0.46
    cw = off + V(c)
    fc = f0 + 8                              # 接触帧 613

    base_q = _view_q(-4, 30)                 # 盘面略朝上斜对镜头：一点椭圆才像实物
    TOP = 0.0040                             # 盘面上表面（局部 z）

    rec = disc(C, C + "_rec", c, 0.300, off, 'vinyl', thick=0.008)
    _setm(rec, _grooves("F_REC", 0.300, 22, dark=(0.0045, 0.0055, 0.0050),
                        light=(0.026, 0.030, 0.028)))
    label = disc(C, C + "_lab", c, 0.098, off, 'paper', thick=0.0026)
    _setm(label, _flat("F_LAB", (0.90, 0.91, 0.88), fac=0.60))
    rim = torus(C, C + "_rim", c, 0.299, 0.0032, off, 'mint', mv=200)
    _setm(rim, _flat("F_RIM", (0.40, 0.60, 0.50), strength=1.0))
    # ▶ 形的洞：单个三棱柱（v=3 的圆柱），顶点朝局部 +x，干净不锯齿
    hole = [L.prim('cyl', C, C + "_play", loc=off + cw, v=3, r=0.042, d=0.0030,
                   material=M('ink'))]
    _setm(hole[0], _flat("F_HOLE", VOID, strength=0.5))

    lab_l = (0.0, 0.0, TOP + 0.0009)
    rim_l = [(0.0, 0.0, 0.0)]
    bar_l = [(0.0, 0.0, TOP + 0.0030)]

    spin = [(f0, 0), (fc, 0), (fc + 5, 18), (fc + 12, 110), (f1, 300)]
    _spin_keys([rec], cw, [(0, 0, 0)], base_q, spin)
    _spin_keys([label], cw, [lab_l], base_q, spin)
    _spin_keys([rim], cw, rim_l, base_q, spin)
    _spin_keys(hole, cw, bar_l, base_q, spin)

    # ---- 光柱：自己从上方压下来，指尖正好落在洞上
    G = cw + base_q @ V((0.0, 0.0, TOP + 0.008))
    A = V((0.30, -0.34, 1.0)).normalized()
    for nm, r_, a_, st in (("_b0", 0.034, 0.022, 1.0), ("_b1", 0.021, 0.05, 1.4),
                           ("_b2", 0.0125, 0.11, 2.2), ("_b3", 0.0072, 0.42, 3.4),
                           ("_b4", 0.0038, 0.95, 7.0)):
        ob = _col(C, off, C + nm, (0, 0, 0), (2 * r_, 2 * r_, 1.7), 'white_e',
                  rot=A.to_track_quat('Z', 'Y').to_euler())
        _setm(ob, _flat("F_ROD" + nm, COLD, strength=st, alpha=a_))
        for f, back in ((f0, 0.34), (fc - 1, 0.29), (fc, 0.0), (fc + 2, 0.0),
                        (f1 - 1, 0.92), (f1, 1.02)):
            ob.location = G + A * (back + 0.85)
            ob.keyframe_insert("location", frame=f)


    # ---- 接触：白闪 + 橙闪（严格 3 帧）
    hit = _ball(C, off, C + "_hit", G, 0.012, 'white_e')
    _setm(hit, _flat("F_HIT", COLD, strength=6.0))
    for f, sc in ((f0, 0.02), (fc - 1, 0.02), (fc, 1.6), (fc + 3, 3.4), (fc + 9, 0.85), (f1, 0.8)):
        hit.scale = (sc, sc, sc)
        hit.keyframe_insert("scale", frame=f)


    o_hit = _ball(C, off, C + "_o1", cw + base_q @ V((0, 0, TOP + 0.010)), 0.027, 'orange')
    _setm(o_hit, _flat("F_O1", ORANGE, strength=1.5))
    o2 = torus(C, C + "_o2", c, 0.104, 0.0036, off, 'orange', mv=160)
    o2.rotation_mode = 'QUATERNION'
    o2.rotation_quaternion = base_q
    o2.location = cw + base_q @ V((0, 0, TOP + 0.0016))
    _setm(o2, _flat("F_O2", ORANGE, strength=1.25))
    o3 = disc(C, C + "_o3", c, 0.152, off, 'orange', thick=0.0008)   # 橙闪：整块盘面闪一下
    _setm(o3, _flat("F_O3", ORANGE, strength=0.85, alpha=0.30))
    o3.rotation_mode = 'QUATERNION'
    o3.rotation_quaternion = base_q
    o3.location = cw + base_q @ V((0, 0, TOP + 0.0005))
    _gate([o_hit, o2, o3], fc, 3)

    # ---- 声波：两圈环 + 一层朝镜头压过来的波面
    for k, d0 in ((0, 0), (1, 3)):
        w = torus(C, C + "_w%d" % k, c, 0.24, 0.0022, off, 'mint', mv=224)
        _setm(w, _flat("F_W%d" % k, (0.52, 0.86, 0.70), strength=1.6))
        w.rotation_mode = 'QUATERNION'
        w.rotation_quaternion = base_q
        for f, sc in ((f0, 0.02), (fc + d0 - 1, 0.02), (fc + d0, 0.62),
                      (fc + d0 + 4, 1.15), (f1, 5.6 - 0.9 * k)):
            w.scale = (sc, sc, sc)
            w.location = cw + base_q @ V((0, 0, TOP + 0.0018))
            w.keyframe_insert("scale", frame=f)
            w.keyframe_insert("location", frame=f)

    # ---- 被推开的粒子
    rr = [(-0.30, 0.22), (0.31, -0.14), (-0.26, -0.25), (0.28, 0.27), (0.02, 0.34),
          (-0.35, -0.02), (0.36, 0.05), (-0.12, -0.36), (0.16, 0.33), (-0.33, 0.14)]
    for i, (dx, dy) in enumerate(rr):
        d = V((dx, dy, 0)).normalized()
        ob = _ball(C, off, C + "_p%d" % i, cw + base_q @ V((dx * 0.94, dy * 0.94, TOP + 0.006)),
                   0.0070 + 0.0035 * ((i * 5) % 3), 'white_e')
        _setm(ob, _flat("F_P%d" % i, COLD, strength=2.4))
        fs = fc + 1 + (i % 3)
        for f, u in ((fs, 0.0), (fs + 4, 0.60), (f1, 1.0)):
            ob.location = cw + base_q @ V((d.x * (0.30 + 1.20 * u), d.y * (0.30 + 1.20 * u),
                                           TOP + 0.006 - 0.10 * u))
            ob.keyframe_insert("location", frame=f)
    dust(C, off, c, s, n=24, spread_k=1.0, r_k=0.005, seed=41)

    cam = cam_at(C, off, c, s, az=0, el=8, fill=1.0, lens=25, dof_k=None, fstop=8.0)
    keys = [(f0, 1.0, 0, 8), (fc, 1.0, 0, 8)]
    span = float(f1 - fc)
    for f in range(fc + 1, f1 + 1):           # 冲击：一上来就推，t**0.45 衰减
        u = ((f - fc) / span) ** 0.45
        keys.append((f, 1.0 - 0.44 * u, 2.5 * u, 8 + 7 * u))
    _cam_keys(cam, cw, s, keys)
    return cam


# ================================================================ 26
def s26(off, C, f0, f1):
    """白光溢出满屏 = 峰值。白里淡出一个极小的黑点。不淡出 —— 硬切给 27。"""
    c, s = (0, 0, 0), 0.80
    sc = bpy.context.scene
    # 白板只做到刚好盖住正交画幅：曾做到 9x9，边框漏进第 27 镜的取景，把整帧糊成一片白
    field = _col(C, off, C + "_field", (0, 0.90, 0.03), (2.2, 0.02, 2.2), 'white_e')
    _setm(field, _flat("F_BLOWN", (1.0, 1.0, 1.0), strength=1.1))

    dot = disc(C, C + "_dot", (0, 0.0, 0.0), 0.0125, off, 'ink', thick=0.0012, tilt=90)
    _setm(dot, _flat("F_VOIDX", VOID, strength=0.0))
    for f, scv in ((f0 + 7, 0.03), (f0 + 17, 1.0), (f1, 1.0)):
        dot.scale = (scv, scv, scv)
        dot.keyframe_insert("scale", frame=f)

    # 白靠曝光提上去、不靠高发光：白板压在 Glare 阈值 1.2 以下，黑点才不会被辉光糊成灰点
    for f, e in ((f0 - 2, 0.0), (f0, 3.2), (f1, 3.2), (f1 + 1, 0.0)):
        sc.view_settings.exposure = e
        sc.view_settings.keyframe_insert("exposure", frame=f)
    for fc_ in _curves(sc):
        for kp in fc_.keyframe_points:
            kp.interpolation = 'CONSTANT'

    cam = cam_at(C, off, c, s, az=0, el=0, fill=4.0, lens=50, ortho=1.6)
    cam.data.dof.use_dof = False
    return cam



# ================================================================ 27
def s27(off, C, f0, f1):
    """开场的那束光、那批尘埃 —— 同一个位置，这次一粒都不动，一点声音也没有。"""
    c, s = (0.0, 0.0, 0.048), 0.052
    blade = box(C, C + "_blade", (0.004, 0.0, 0.050), (0.0020, 0.0004, 0.125), 'white_e', off)
    blade.rotation_euler = (0, rd(-16), 0)
    _setm(blade, K.L.mat_shaft("G7_BLADE", (0.96, 0.98, 1.0), strength=2.6, alpha=0.90))
    spill = box(C, C + "_spill", (0.017, 0.0, 0.0007), (0.055, 0.0011, 0.0011), 'alu2', off)
    _setm(spill, _flat("F_SPILL", (0.62, 0.64, 0.62), strength=1.2))
    for i, (p, r) in enumerate([((0.006, 0.0, 0.050), 0.0024), ((0.030, 0.003, 0.078), 0.0015),
                                ((-0.018, -0.002, 0.031), 0.0016)]):
        _ball(C, off, C + "_m%d" % i, p, r, 'white_e')

    # 与 g1 的 s01 同机位同尺寸，只是这一次一粒尘埃都不动、一条曲线都没有
    cam = cam_at(C, off, c, s, az=33, el=6, fill=1.25, lens=100)
    cam.data.dof.use_dof = True
    cam.data.dof.focus_distance = cam["dist"]
    cam.data.dof.aperture_fstop = 4.0
    return cam


# ================================================================ 28
def s28(off, C, f0, f1):
    """光柱里悬着唱片，缓慢自转；背后是虚焦的米白档案架。极慢环绕。"""
    c, s = (0, 0.0, 0.22), 0.62
    cw = off + V(c)
    az0, el0 = 14.0, 7.0
    cam = cam_at(C, off, c, s, az=az0, el=el0, fill=1.30, lens=50, dof_k=1.0, fstop=0.9)
    cam.data.dof.use_dof = True
    cam.data.dof.focus_distance = cam["dist"]
    cam.data.dof.aperture_fstop = 0.9
    base_q = _view_q(20.0, 7.0)

    # ---- 背景：米白档案架。压暗 + 大光圈 + 两层薄纱把对比压平 = 虚焦的"地方"，不是主光
    wall_y = 3.20
    shelfA = _flat("F_SHA", (0.092, 0.086, 0.071), fac=0.55)
    shelfB = _flat("F_SHB", (0.071, 0.066, 0.054), fac=0.55)
    gapm = _flat("F_SHG", (0.022, 0.023, 0.021), fac=0.9)
    labm = _flat("F_SHL", (0.011, 0.012, 0.010), fac=0.9)
    for ix in range(14):
        x = -3.575 + ix * 0.55
        for iz in range(9):
            z = -1.60 + iz * 0.45
            ob = _col(C, off, C + "_s%d%d" % (ix, iz), (x, wall_y, z), (0.50, 0.18, 0.41), 'paper')
            _setm(ob, shelfA if (ix + iz) % 2 else shelfB)
            lb = _col(C, off, C + "_l%d%d" % (ix, iz), (x - 0.03, wall_y - 0.092, z + 0.05),
                      (0.20, 0.002, 0.046), 'ink')
            _setm(lb, labm)
    for iz in range(9):                                    # 层板：横向暗缝
        sb = _col(C, off, C + "_g%d" % iz, (0, wall_y - 0.02, -1.82 + iz * 0.45),
                  (7.8, 0.24, 0.028), 'ink')
        _setm(sb, gapm)

    # ---- 光柱：唱片背后一根，上亮下淡，从上方斜切下来
    shaft_q = (rd(-9), 0, rd(6))
    for nm, r_, a_, st in (("_k0", 0.160, 0.05, 1.2), ("_k1", 0.095, 0.09, 1.8),
                           ("_k2", 0.048, 0.16, 2.6), ("_k3", 0.018, 0.40, 2.8),
                           ("_kc", 0.0050, 0.92, 4.0)):
        L.prim('cyl', C, C + nm, loc=off + V((-0.10, 0.62, 0.30)), r=r_, d=6.0, v=48,
               material=K.L.mat_shaft("G7_SH" + nm, (0.94, 0.97, 1.0), strength=st, alpha=a_),
               rot=shaft_q)

    # ---- 唱片：暗盘面 + 薄荷边 + 白标 + 五道刻线（自己慢慢转）
    rec = disc(C, C + "_rec", c, 0.145, off, 'vinyl', thick=0.006)
    _setm(rec, _grooves("F_REC2", 0.145, 14, dark=(0.006, 0.007, 0.007),
                        light=(0.030, 0.033, 0.031)))
    rim = torus(C, C + "_rim", c, 0.1445, 0.0019, off, 'mint', mv=176)
    _setm(rim, _flat("F_RIM2", (0.30, 0.74, 0.52), strength=1.9))
    label = disc(C, C + "_lab", c, 0.046, off, 'paper', thick=0.0016)
    _setm(label, _flat("F_LAB2", (0.62, 0.63, 0.60), fac=0.6))
    tick_az = (26, 98, 170, 242, 314)
    ticks = []
    for k, ang in enumerate(tick_az):
        tb = box(C, C + "_t%d" % k, (0, 0, 0), (0.026, 0.0028, 0.0024), 'mint', off)
        _setm(tb, _flat("F_TK%d" % k, (0.34, 0.74, 0.54), strength=1.6))
        ticks.append(tb)
    spin = [(f0, 0), (f1, 40)]
    _spin_keys([rec], cw, [(0, 0, 0)], base_q, spin)
    _spin_keys([rim], cw, [(0, 0, 0)], base_q, spin)
    _spin_keys([label], cw, [(0, 0, 0.0044)], base_q, spin)
    _spin_keys(ticks, cw, [(cos(rd(a)) * 0.104, sin(rd(a)) * 0.104, 0.0040) for a in tick_az],
               [base_q @ Quaternion((0, 0, 1), rd(a)) for a in tick_az], spin)
    sh = disc(C, C + "_sh", c, 0.144, off, 'paper', thick=0.0004)
    _setm(sh, _flat("F_SHN", COLD, strength=1.0, alpha=0.02))
    sh.rotation_mode = 'QUATERNION'
    sh.rotation_quaternion = base_q
    sh.location = cw + base_q @ V((0, 0, 0.0048))
    dust(C, off, (0, 0, 0.22), 0.40, n=16, spread_k=0.50, r_k=0.0022, seed=23)
    dust(C, off, (0, -0.55, 0.22), 0.30, n=10, spread_k=0.65, r_k=0.0020, seed=7)

    move(cam, f0, f1, c, s, az=az0, el=el0, fill=1.30, kind='orbit', az2=26.0, el2=8.0)
    return cam


SHOTS = [
    ("25", 50.36, 53.04, s25),
    ("26", 53.04, 56.25, s26),
    ("27", 56.25, 58.93, s27),
    ("28", 58.93, 61.61, s28),
]
