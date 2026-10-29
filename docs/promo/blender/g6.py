# -*- coding: utf-8 -*-
"""Group 6 · 第 20–24 镜（骤停 → 光带 → UI 组装 → 穿行 → 书架即列表）。

硬规则（对齐分镜 §2）：
  · 只有平面发光材质，没有灯（K.key_at / K.area_at 是空函数，这里一次都不调）。
  · 调色板只用 纸白 / 灰纸 / 墨 / 苔绿 / 薄荷 —— 本组**不出现橙**（橙色留给第 25 镜）。
  · 画面里没有可读文字：所有"字"都是宽度不等、基线对齐的实心小方块。
  · 背景纯黑（shotkit.setup 的 world strength = 0）。
  · 每镜只有一个主体（21 的"带 + 网格"算同一个主体的两次变形）。

本组的一条暗线：21 甩出来的细线网格 = 22 的 UI 面板 = 23 被切开的那层壳 = 24 的书架立面，
所以三镜共用同一套 PANEL 尺寸（2.00 × 1.10）与同一个中心点，切换时不会"跳"。
"""
import bpy, bmesh, random
from math import radians as RD, sin, cos, pi
from mathutils import Vector, Matrix

import pvlib as L
import models
import shotkit as K
from shotkit import V, M, flat, cam_at, move, disc, box, torus, points_mesh, SENSOR_HALF


# ---------------------------------------------------------------- helpers

def slab(C, name, rects, mat, off, origin=(0.0, 0.0, 0.0)):
    """把一堆盒子并成一个网格物体（省对象、也能整组做落位动画）。

    rects = [(x, y, z, w, t, h)]，全部是镜头局部米制；origin = 物体原点（缩放入点）。
    """
    og = Vector(origin)
    o = Vector(off)
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for (x, y, z, w, t, h) in rects:
        p = Vector((x, y, z)) - og
        mtx = Matrix.Translation(p) @ Matrix.Diagonal((max(w, 1e-4), max(t, 1e-4), max(h, 1e-4), 1.0))
        bmesh.ops.create_cube(bm, size=1.0, matrix=mtx)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.location = o + og
    ob.data.materials.append(M(mat) if isinstance(mat, str) else mat)
    L.link(ob, C)
    return ob


def ring(C, name, off, c, R, r, mat, rot=(RD(90), 0.0, 0.0), mv=192):
    """torus 的 mat 参数只收调色板 key，这里放开成"任意材质对象"。"""
    return L.prim('torus', C, name, loc=Vector(off) + Vector(c), R=R, r=r,
                  material=M(mat) if isinstance(mat, str) else mat, rot=rot, mv=mv, nv=6)


_MC = {}


def GMat(name, color, fac=0.55, strength=1.0):
    """带内存缓存的 flat 材质；一定要在 setup() 之后才被调用（purge 会清材质）。"""
    m = _MC.get(name)
    try:
        m.name
    except Exception:
        m = None
    if m is None:
        m = K.flat(name, color, fac=fac, strength=strength)
        _MC[name] = m
    return m


def BLACK():
    return GMat("G6_BLACK", (0.0045, 0.0050, 0.0045), 1.0, 1.0)


def GROOVE():
    return GMat("G6_GRV", (0.33, 0.335, 0.320), 0.60, 0.95)


def GROOVE_M():
    return GMat("G6_GRV_M", (0.30, 0.52, 0.40), 0.60, 0.95)


def MINT_D():
    return GMat("G6_MINT_D", (0.235, 0.560, 0.395), 0.70, 1.0)


def MOSS_D():
    return GMat("G6_MOSS_D", (0.205, 0.245, 0.105), 0.72, 0.70)


def PAPER_D():
    return GMat("G6_PAPER_D", (0.700, 0.698, 0.660), 0.55, 1.0)


def MOSS_G():
    """孔的剖切内壁：橄榄绿 + 一点自发光，让 23 也有一处吹到 0.9 的高光。"""
    return GMat("G6_MOSS_G", (0.290, 0.330, 0.150), 0.75, 1.60)


def BAYMAT():
    """中央开间的纸白墙：比纸白暗一档，否则 18mm 广角里它把整帧的中间调抬到 0.67（和别人的黑场对不上）。"""
    return GMat("G6_BAY", (0.560, 0.562, 0.530), 0.75, 0.85)


def FLOOR():
    return GMat("G6_FLOOR", (0.006, 0.006, 0.005), 1.0, 1.0)


def GM(mint=False):
    """发光材质必须在 setup() 之后建：run_group 先 import 再 setup，模块级的材质会被 purge 掉。"""
    k = 1 if mint else 0
    m = _MC.get(k)
    try:
        m.name
    except Exception:
        m = None
    if m is None:
        m = (K.flat("G6_GLOW_M", (0.561, 0.890, 0.733), fac=0.85, strength=2.50) if mint
             else K.flat("G6_GLOW_W", (1.0, 1.0, 0.98), fac=0.85, strength=3.60))
        _MC[k] = m
    return m


def glyph_row(x0, x1, z_base, y, seed, t=0.008, hmin=0.013, hmax=0.021, gap=0.007,
              wmin=0.010, wmax=0.028, avoid=None):
    """伪字形：宽度不等、底边对齐的实心小方块。永远读不出任何字。

    avoid = (cx, cz, r) 时把落进去的方块挖掉（给中间的"洞"让位）。
    """
    rr = random.Random(seed)
    out, x = [], x0
    while x < x1 - wmin:
        w = min(rr.uniform(wmin, wmax), x1 - x)
        h = rr.uniform(hmin, hmax)
        if rr.random() < 0.10:
            h = hmax * rr.uniform(1.15, 1.45)          # 偶发"升部"，破掉等高的机器感
        cxx = x + w / 2.0
        if avoid is None or (cxx - avoid[0]) ** 2 + (z_base - avoid[1]) ** 2 > avoid[2] ** 2:
            out.append((cxx, y, z_base + h / 2.0, w, t, h))
        x += w + rr.uniform(gap * 0.55, gap * 1.7)
    return out


def motes(C, off, c, s, f0, f1, n=18, seed=3, rx=(0.0009, 0.0024), spread=(0.85, 0.55, 0.85),
          drift=(0.004, 0.010), front=0.35):
    """会动的尘埃：每粒单独两帧位移（匀速漂移），近处几粒更大 = 焦外光点。"""
    rr = random.Random(seed)
    for i in range(n):
        near = (i % 3 == 0)
        p = Vector(c) + Vector((rr.gauss(0, s * spread[0]),
                                rr.gauss(0, s * 0.35) - (front if near else 0.0),
                                rr.gauss(0, s * spread[1]) if i % 5 else rr.gauss(0, s * 0.9)))
        p.z += (s * spread[2]) * rr.uniform(-0.15, 1.0) if i % 4 else 0.0
        rad = rr.uniform(*rx) * (2.1 if near else 1.0)
        ob = L.prim('ico', C, "%s_mt%02d" % (C, i), loc=Vector(off) + p, r=rad, sub=1,
                    material=(GM() if near else M('paper2' if i % 2 else 'paper')))
        d = Vector((rr.uniform(-1, 1) * drift[0], 0.0, rr.uniform(0.4, 1.6) * drift[1]))
        ob.keyframe_insert("location", frame=f0)
        ob.location = Vector(off) + p + d
        ob.keyframe_insert("location", frame=f1)


def arrive(ob, f_in, dur=5, offset=(0.0, -0.28, 0.0)):
    """全息落位：先隐藏，从空中偏移位置缓入到最终位置（每落一次 = 一声"嗒"）。"""
    b = Vector(ob.location)
    ob.hide_render = True
    ob.keyframe_insert("hide_render", frame=max(1, f_in - 1))
    ob.hide_render = False
    ob.keyframe_insert("hide_render", frame=f_in)
    ob.location = b + Vector(offset)
    ob.keyframe_insert("location", frame=f_in)
    ob.location = b
    ob.keyframe_insert("location", frame=f_in + dur)
    return ob


# ---------------------------------------------------------------- 唱片（20/21/24 共用）

def spinner(C, off, c, R, spokes=1, spin=(0.0, 0.0), f=(None, None)):
    """径向标记：原点在盘心，绕世界 Y 自转 => 一帧里唯一能看出"唱片在转"的东西。"""
    rects = []
    for k in range(spokes):
        a = 2 * pi * k / spokes
        for j in range(9):                                    # 沿半径采样的小方块链 = 任意角度的细杠
            u = 0.52 + j * (0.46 / 8.0)
            rects.append((c[0] + R * u * cos(a), c[1], c[2] + R * u * sin(a),
                          R * 0.050, R * 0.018, R * 0.050))
    ob = slab(C, C + "_mark", rects, 'paper', off, origin=c)
    ob.location = Vector(off) + V((c[0], c[1] - R * 0.034, c[2]))
    if f[0] is not None:
        ob.rotation_euler = (0.0, spin[0], 0.0)
        ob.keyframe_insert("rotation_euler", frame=f[0])
        ob.rotation_euler = (0.0, spin[1], 0.0)
        ob.keyframe_insert("rotation_euler", frame=f[1])
    return ob


def record(C, off, c, R, spin=(0.0, 0.0), f=(None, None), spokes=1, mint_groove=True):
    """竖立的唱片：近黑盘体 + 一圈圈细纹 + 发光盘缘 + 纸白标签 + 会转的径向标记。"""
    o = Vector(off)
    body = L.prim('cyl', C, C + "_vinyl", loc=o + V(c), r=R * 0.995, d=R * 0.030,
                  material=BLACK(), rot=(RD(90), 0, 0))
    L.smooth(body)
    L.bevel(body, R * 0.004, 2)
    # 纹路：薄圆环（M('vinyl') 的 rings 尺度以物体米计，在大盘上完全看不出来）
    y = c[1] - R * 0.0175
    for k in range(7):
        u = 0.42 + k * 0.082
        mat = GROOVE_M() if (mint_groove and k == 2) else GROOVE()
        ring(C, "%s_grv%02d" % (C, k), off, (c[0], y, c[2]), R * u, R * 0.0032, mat, mv=128)
    ring(C, C + "_rim", off, (c[0], y, c[2]), R * 1.002, R * 0.013, GM(), mv=224)
    ring(C, C + "_rim2", off, (c[0], y, c[2]), R * 0.955, R * 0.0035, GM(), mv=192)
    # 标签 + 中心孔（都往前挪一点，否则埋进盘体）
    lab = L.prim('cyl', C, C + "_label", loc=o + V((c[0], c[1] - R * 0.020, c[2])), r=R * 0.30,
                 d=R * 0.010, material=M('paper'), rot=(RD(90), 0, 0))
    L.smooth(lab)
    L.prim('cyl', C, C + "_spindle", loc=o + V((c[0], c[1] - R * 0.026, c[2])), r=R * 0.045,
           d=R * 0.010, material=BLACK(), rot=(RD(90), 0, 0))
    # 标签上的"字"：宽度不等的小方块（读不出来）
    rr = random.Random(int(R * 977) + 5)
    gl = [(c[0] + R * (-0.17 + 0.028 * i), c[1] - R * 0.0285, c[2] + R * (0.10 if i < 4 else -0.13),
           rr.uniform(0.012, 0.030) * R * 3.0, R * 0.005, R * 0.045) for i in range(8)]
    slab(C, C + "_labtxt", gl, 'ink', off)
    spinner(C, off, c, R, spokes=spokes, spin=spin, f=f)
    return body


def chain_move(cam, segs, c, s, az=0.0, el=0.0, kind='push', az2=None):
    """把 move() 串起来：move 只读 cam["dist"] 当起点，所以每段前面手动写回去。

    az2 给了就把整条链的方位角按帧线性摊到各段上（用于"边拉远边侧移"）。
    """
    fa0, fb1 = float(segs[0][0]), float(segs[-1][1])
    for (a, b, f_from, f_to) in segs:
        cam["dist"] = s * (cam.data.lens / SENSOR_HALF) / max(f_from, 0.05)
        if az2 is None:
            move(cam, a, b, c, s, az=az, el=el, fill=f_from, kind=kind, fill2=f_to)
        else:
            u0, u1 = (a - fa0) / (fb1 - fa0), (b - fa0) / (fb1 - fa0)
            move(cam, a, b, c, s, az=az + (az2 - az) * u0, el=el, fill=f_from, kind=kind,
                 az2=az + (az2 - az) * u1, fill2=f_to)
    return cam


def dolly(cam, off, P0, P1, f0, f1, target, n=14, power=1.0):
    """直线路径逐点落关键帧（多帧 ≈ 匀速）。power>1 => 起手慢、后面加速穿过去。"""
    P0, P1, T = Vector(P0), Vector(P1), Vector(target)
    for i in range(n):
        u = i / float(n - 1)
        L.key_cam(cam, int(round(f0 + (f1 - f0) * u)),
                  loc=off + P0 + (P1 - P0) * (u ** power), target=off + T, roll=0.0)
    return cam


# ---------------------------------------------------------------- 20 · 骤停

def s20(off, C, f0, f1):
    """36.43–39.64 · 50mm 环摇。纯黑房间里悬浮一张发光唱片，尘土慢速飘，缓环绕 15°。"""
    c, s, R = (0.0, 0.0, 0.34), 0.255, 0.200
    record(C, off, c, R, spin=(0.0, RD(38)), f=(f0, f1))
    motes(C, off, c, s, f0, f1, n=26, seed=17, drift=(0.005, 0.013))
    cam = cam_at(C, off, c, s, az=-17, el=7, fill=1.04, lens=50, dof_k=1.0, fstop=2.8)
    move(cam, f0, f1, c, s, az=-17, el=7, fill=1.04, kind='orbit', az2=-2, el2=6)
    return cam


# ---------------------------------------------------------------- 21 · 光带 → 网格

GRID_W, GRID_H = 2.00, 1.10
GRID_C = (0.0, 0.0, 0.30)


def s21(off, C, f0, f1):
    """39.64–42.32 · 50mm。唱片猛然加速甩出一圈光带，光带被"拉直"成白色细线网格，快速拉远。"""
    s, c, R = 0.55, GRID_C, 0.185
    record(C, off, c, R, spin=(0.0, RD(520)), f=(f0, f1), spokes=3)
    # 甩出的光带：4 圈由小到大冲出画外
    for k, (R0, kk) in enumerate(((0.30, 6.2), (0.42, 4.6), (0.58, 3.4), (0.78, 2.5))):
        t = ring(C, "%s_band%d" % (C, k), off, c, R0, 0.0018 + 0.0006 * k,
                 GM() if k % 2 == 0 else GM(True), mv=192)
        t.hide_render = True
        t.keyframe_insert("hide_render", frame=f0)
        fa = f0 + 1 + k
        t.hide_render = False
        t.keyframe_insert("hide_render", frame=fa)
        t.scale = (0.22, 0.22, 0.22)
        t.keyframe_insert("scale", frame=fa)
        t.scale = (1.0, 1.0, 1.0)
        t.keyframe_insert("scale", frame=fa + 6)
        t.scale = (kk * 1.55, kk * 1.55, kk * 1.55)
        t.keyframe_insert("scale", frame=fa + 11)
        if k == 0:                      # 最外圈留着：光带最终变成网格外面那一圈
            t.location = Vector(off) + V((c[0], c[1] - 0.03, c[2]))
            t.hide_render = True
            t.keyframe_insert("hide_render", frame=fa + 17)
            ring2 = ring(C, C + "_halo", off, (c[0], c[1] - 0.03, c[2]), GRID_W * 0.55,
                         0.0022, GM(True), mv=192)
            ring2.hide_render = True
            ring2.keyframe_insert("hide_render", frame=fa + 12)
            ring2.hide_render = False
            ring2.keyframe_insert("hide_render", frame=fa + 20)
    # 网格：先出交点（每根线从中点向两头长），先竖后横，交错生长 —— "被拉直"而不是散开
    lines = []
    for i in range(9):
        x = -GRID_W / 2 + i * GRID_W / 8.0
        m = 'paper' if i % 3 else 'paper2'          # 每三根一组，网格才有主次、不像窗棂
        ln = GRID_H * (1.0 - 0.05 * abs(i - 4) / 4.0)   # 越靠外越短：网是被拉出来的，不是画好的框
        lines.append((box(C, "%s_gx%02d" % (C, i), (x, 0.0, c[2]), (0.011, 0.010, ln), m, off), True, i))
    for j in range(6):
        z = c[2] - GRID_H / 2 + j * GRID_H / 5.0
        m = 'paper' if j % 2 else 'paper2'
        ln = GRID_W * (1.0 - 0.04 * abs(j - 2.5) / 2.5)
        lines.append((box(C, "%s_gz%02d" % (C, j), (0.0, 0.0, z), (ln, 0.010, 0.011), m, off), False, j))
    for (ob, vert, idx) in lines:
        s0 = ob.scale.copy()                       # box 的 dim 就是 scale，必须按比例动
        fa = f0 + 2 + idx * 1.05 + (0.0 if vert else 2.10)
        fa = int(round(fa))
        ob.hide_render = True
        ob.keyframe_insert("hide_render", frame=max(1, fa - 1))
        ob.hide_render = False
        ob.keyframe_insert("hide_render", frame=fa)
        ob.scale = (s0.x, s0.y, s0.z * 0.03) if vert else (s0.x * 0.03, s0.y, s0.z)
        ob.keyframe_insert("scale", frame=fa)
        ob.scale = s0
        ob.keyframe_insert("scale", frame=fa + 8)
    motes(C, off, c, s, f0, f1, n=14, seed=23, drift=(0.004, 0.009))
    cam = cam_at(C, off, c, s, az=-3, el=8, fill=2.50, lens=50, dof_k=None, fstop=8.0)
    chain_move(cam, [(f0, f0 + 9, 2.50, 1.95), (f0 + 9, f1, 1.95, 0.46)], c, s, az=-3, el=8, kind='pull', az2=5)
    return cam


# ---------------------------------------------------------------- 22/23 · UI 面板

PANEL_C = (0.0, 0.0, 0.30)
PW, PH = 2.00, 1.10
HOLE_R = 0.27
UI_T = 0.012          # 面板厚度（23 里它是被切开的"壳"）
SEC_T = 0.090         # 23 里结构件向后加厚的橄榄绿剖面


def panel_parts(thin=UI_T, hole=HOLE_R):
    """UI 的几何：返回 [(name, rects, mat, slot)]。slot = 第几声"嗒"。"""
    cx, cy, cz = PANEL_C
    hw, hh = PW / 2.0, PH / 2.0
    zt, zb = cz + hh, cz - hh
    out = []
    out.append(("u_frame", [(cx - hw, cy, cz, 0.009, thin, PH), (cx + hw, cy, cz, 0.009, thin, PH),
                            (cx, cy, zt, PW, thin, 0.009), (cx, cy, zb, PW, thin, 0.009)], 'paper2', 0))
    out.append(("u_title", [(cx, cy, zt - 0.056, PW - 0.024, thin * 1.3, 0.062)], 'paper', 1))
    out.append(("u_logo", [(cx - hw + 0.075, cy - 0.012, zt - 0.056, 0.044, 0.010, 0.038)], 'ink', 2))
    out.append(("u_nav", [(cx + 0.26 + k * 0.155, cy - 0.012, zt - 0.056, 0.084, 0.008, 0.014)
                          for k in range(4)], 'ink', 3))
    out.append(("u_rule", [(cx, cy, zt - 0.098, PW - 0.008, thin, 0.006)], 'moss', 4))
    colL, colR = cx - hw + 0.045, cx + hw - 0.045
    out.append(("u_headL", [(colL + 0.10, cy, zt - 0.135, 0.20, thin, 0.021)], 'paper', 5))
    out.append(("u_headR", [(colR - 0.10, cy, zt - 0.135, 0.20, thin, 0.021)], 'paper', 6))
    rows = [zt - 0.185 - i * 0.108 for i in range(6)]
    for i, z in enumerate(rows):
        out.append(("u_rowL%d" % i, glyph_row(colL, cx - 0.33, z, cy, 100 + i, t=thin * 0.8), 'paper2', 7 + (i % 3)))
        out.append(("u_rowR%d" % i, glyph_row(cx + 0.33, colR, z, cy, 200 + i, t=thin * 0.8), 'paper2', 7 + (i % 3)))
    for i, z in enumerate((zb + 0.085, zb + 0.030)):
        out.append(("u_bot%d" % i, glyph_row(colL, colR, z, cy, 300 + i, t=thin * 0.8,
                                             avoid=(cx, cz, hole + 0.05)), 'paper2', 10 + i))
    out.append(("u_ribL", [(cx - hole - 0.045, cy, cz, 0.008, thin, PH - 0.22)], 'moss', 11))
    out.append(("u_ribR", [(cx + hole + 0.045, cy, cz, 0.008, thin, PH - 0.22)], 'moss', 11))
    return out


def section_parts(thin=UI_T, hole=HOLE_R):
    """23 专用：结构件向后加厚成橄榄绿实体 —— 侧看就是"被切开的剖面"。"""
    cx, cy, cz = PANEL_C
    hw, hh = PW / 2.0, PH / 2.0
    zt, zb = cz + hh, cz - hh
    t2, y2 = SEC_T, cy + thin * 0.5 + SEC_T * 0.5
    return [(cx - hw, y2, cz, 0.009, t2, PH), (cx + hw, y2, cz, 0.009, t2, PH),
            (cx, y2, zt, PW, t2, 0.009), (cx, y2, zb, PW, t2, 0.009),
            (cx, y2, zt - 0.056, PW - 0.024, t2, 0.062),
            (cx - hole - 0.045, y2, cz, 0.009, t2, PH - 0.22),
            (cx + hole + 0.045, y2, cz, 0.009, t2, PH - 0.22)]


def build_panel(C, off, f_build=None, dur=5, section=False, thin=UI_T, hole=HOLE_R, jitter=0.30):
    parts = panel_parts(thin=thin, hole=hole)
    rr = random.Random(7)
    objs = []
    for (name, rects, mat, slot) in parts:
        ob = slab(C, C + "_" + name, rects, mat, off)
        if f_build is not None:
            arrive(ob, f_build + int(round(slot * 1.15)), dur,
                   offset=(rr.uniform(-0.18, 0.18), rr.uniform(-0.34, -0.10), rr.uniform(-0.14, 0.14)))
        objs.append(ob)
    if section:
        ob = slab(C, C + "_section", section_parts(thin=thin, hole=hole), MOSS_D(), off)
        if f_build is not None:
            arrive(ob, f_build + 1)
        objs.append(ob)
    # 中间的洞：一圈苔绿 + 一圈薄荷（已"溶解"的唱片）
    for (R, r_, mat) in ((hole, 0.0055, MOSS_D()), (hole * 1.10, 0.0016, 'paper2'),
                         (hole * 0.56, 0.0022, MINT_D())):
        t = ring(C, "%s_hole%.2f" % (C, R), off, (PANEL_C[0], PANEL_C[1], PANEL_C[2]), R, r_, mat, mv=160)
        if f_build is not None:
            arrive(t, f_build + 8, dur, offset=(0, -0.20, 0))
        objs.append(t)
    return objs


def s22(off, C, f0, f1):
    """42.32–45.00 · 25mm。细线开始搭 UI（标题栏 / 分割线 / 列 / 伪字形块），缓慢横移穿过结构。"""
    s, c = 0.62, PANEL_C
    build_panel(C, off, f_build=f0 + 1)
    # 前景线框（虚焦，贴着镜头掠过）—— K5 的三层景深里的最前层
    motes(C, off, c, s, f0, f1, n=18, seed=31, rx=(0.0022, 0.0050),
          drift=(0.004, 0.008), front=1.10)     # 贴镜头的焦外光斑 = 三层景深的最前层
    cam = cam_at(C, off, c, s, az=-9, el=3, fill=0.86, lens=25, dof_k=1.05, fstop=2.2)
    move(cam, f0, f1, c, s, az=-9, el=3, fill=0.86, kind='slide', az2=7, el2=1)
    return cam


def s23(off, C, f0, f1):
    """45.00–47.68 · 25mm 穿行。镜头直线穿过全息 UI，穿过处被切开，剖面发橄榄绿。

    路径是一条直线（故事板"直线穿行"）：起点在面板左前方 3.6m、终点在面板背后 1.0m，
    中途从洞口正中穿过（切线距洞壁 ~0.06m）。时间用 power=1.6 让起手慢、后段加速穿过去，
    所以 run_group 的 55% 那一帧正好停在洞口前 1.4m 的斜侧位 —— 能看到 UI 正面的伪字形、
    洞口的橄榄绿剖切厚度、以及洞里 16 根肋条。
    """
    s = 0.85
    build_panel(C, off, section=True, thin=UI_T)
    # 洞口的"壳厚"：内外两圈 + 16 根纵向肋条（从侧面看 = 被钻穿的橄榄绿剖切面）
    cz = PANEL_C[2]
    for (dy, R, r_, mat) in ((-0.004, HOLE_R, 0.0072, MOSS_G()), (UI_T + SEC_T, HOLE_R, 0.0072, MOSS_G())):
        ring(C, "%s_cut%.3f" % (C, R + dy), off, (0.0, dy, cz), R, r_, mat, mv=160)
    ribs = []
    for k in range(16):
        a = 2 * pi * k / 16.0
        ribs.append((HOLE_R * cos(a), UI_T * 0.5 + SEC_T * 0.5, cz + HOLE_R * sin(a), 0.0065, SEC_T, 0.0065))
    slab(C, C + "_cutribs", ribs, MOSS_G(), off)
    cam = cam_at(C, off, (0.0, 0.9, cz), s, az=-14, el=6, fill=0.55, lens=25, dof_k=None, fstop=8.0)
    dolly(cam, off, (-0.894, -2.100, 0.452), (0.10, 1.00, 0.19), f0 + 9, f1, (0.0, 1.20, 0.28), n=16, power=2.0)
    return cam


# ---------------------------------------------------------------- 24 · 书架即列表

def s24(off, C, f0, f1):
    """47.68–50.36 · 18mm 广角全景。UI 与建筑混合：书架即列表；中央悬唱片，下方纯黑的播放键洞。"""
    s = 1.55
    c = (0.0, 0.10, 1.42)
    PK = [0.26 + k * 0.62 for k in range(5)]        # 五层搁板
    BAY = 0.95
    planks, divs, boxes, spines, moss, glyphs, lists = [], [], [], [], [], [], []
    for z in PK:
        for (x0, x1) in ((-3.30, -BAY), (BAY, 3.30)):
            planks.append(((x0 + x1) / 2.0, 0.0, z, x1 - x0, 0.50, 0.050))
            # 搁板前立面 = 一行伪字形 + 一条苔绿细线 —— 书架边就是列表行
            glyphs += glyph_row(x0 + 0.05, x1 - 0.05, z - 0.012, -0.268, int(z * 1000) % 97,
                                t=0.010, hmin=0.011, hmax=0.017, gap=0.010, wmin=0.012, wmax=0.030)
            moss.append(((x0 + x1) / 2.0, -0.262, z - 0.030, x1 - x0, 0.008, 0.005))
            # 单元格内:一半放档案盒/书脊(建筑),一半放三行"列表"(UI) —— 同一件事的两种物质
            for k in range(4):
                cx = x0 + (k + 0.5) * (x1 - x0) / 4.0
                zi = PK.index(z)
                if zi >= len(PK) - 1:
                    continue
                if (k + zi) % 2 == 0:
                    for r in range(3):
                        lists += glyph_row(cx - 0.16, cx + 0.16, z + 0.055 + r * 0.075, 0.12,
                                           900 + zi * 10 + k * 3 + r, t=0.008,
                                           hmin=0.010, hmax=0.016, gap=0.008)
                else:
                    w = 0.26 if (k % 2) else 0.30
                    boxes.append((cx, 0.02, z + 0.120, w, 0.30, 0.19))
                    boxes.append((cx + 0.02, 0.03, z + 0.375, w * 0.86, 0.28, 0.32))
    for k in range(10):
        x = -3.15 + k * 0.70
        if abs(x) < BAY + 0.06:
            continue
        for z in PK[:-1]:
            divs.append((x, 0.0, z + 0.335, 0.045, 0.42, 0.57))
    rr = random.Random(41)
    for k in range(44):
        x = rr.choice([-1.0, 1.0]) * rr.uniform(BAY + 0.12, 3.15)
        zi = rr.randrange(len(PK) - 2)
        z = PK[zi] + 0.025
        if rr.random() < 0.55:
            w, h = rr.uniform(0.028, 0.060), rr.uniform(0.28, 0.44)
            spines.append((x, -0.06, z + h / 2.0, w, 0.10, h))
    # 程序内的档案盒（Assets/models/archive-assembly.glb）：每层两侧各两只，几何与程序里同一份
    for zi, z in enumerate(PK[:-1]):
        for sx in (-1.0, 1.0):
            for k in range(2):
                cx = sx * (BAY + 0.44 + k * 0.92)
                models.place('case', C, C + "_arch%d%d%d" % (zi, int(sx), k),
                             Vector(off) + V((cx, -0.085, z + 0.195)),
                             w=0.40, rz=0.07 * sx * (k + 1))
    for (x0, x1) in ((-3.30, -BAY), (BAY, 3.30)):
        planks.append(((x0 + x1) / 2.0, 0.0, 0.115, x1 - x0, 0.52, 0.230))   # 底座
    slab(C, C + "_planks", planks, PAPER_D(), off)
    slab(C, C + "_divs", divs, 'paper2', off)
    slab(C, C + "_boxes", boxes, 'paper2', off)
    slab(C, C + "_spines", spines, 'moss', off)
    slab(C, C + "_glyphs", glyphs, 'paper2', off)
    slab(C, C + "_moss", moss, 'moss', off)
    slab(C, C + "_lists", lists, 'paper2', off)
    # 中央开间：退进去的纸白墙（最亮的一层）+ 苔绿侧返
    slab(C, C + "_bay", [(0.0, 0.62, 1.46, BAY * 2.0 - 0.02, 0.04, 2.60)], BAYMAT(), off)
    for sx in (-1.0, 1.0):
        slab(C, C + "_bayr%d" % sx, [(sx * BAY, 0.30, 1.46, 0.032, 0.66, 2.60)], MOSS_D(), off)
    slab(C, C + "_baytop", [(0.0, 0.30, 2.77, BAY * 2.0, 0.66, 0.030)], 'moss', off)
    # 纯黑的播放键形状的洞（贴在纸白墙前一点，读作"洞"）
    L.prim('cone', C, C + "_playwall", loc=Vector(off) + V((0.045, 0.598, 1.045)), r1=0.505, r2=0.505,
           d=0.010, v=3, rot=(RD(90), RD(90), 0), material=MOSS_D())   # 洞的内壁（露一条橄榄绿）
    hole = L.prim('cone', C, C + "_play", loc=Vector(off) + V((0.02, 0.578, 1.06)), r1=0.46, r2=0.46,
                  d=0.010, v=3, rot=(RD(90), RD(90), 0), material=BLACK())   # 尖端指右 = 播放键
    L.bevel(hole, 0.006, 2)
    # 中央悬挂的唱片 + 吊线
    record(C, off, (0.0, 0.16, 2.02), 0.42, spokes=1)
    slab(C, C + "_wire", [(0.0, 0.16, 2.47, 0.005, 0.005, 0.62)], 'paper2', off)
    # 地坪 + 踢脚线（给"建筑"一个落地的水平面）
    slab(C, C + "_floor", [(0.0, 0.6, -0.026, 9.0, 3.4, 0.052)], FLOOR(), off)
    slab(C, C + "_skirt", [(0.0, -0.25, 0.028, 6.9, 0.012, 0.020)], 'moss', off)
    motes(C, off, c, s, f0, f1, n=16, seed=53, drift=(0.006, 0.012), rx=(0.002, 0.005))
    cam = cam_at(C, off, c, s, az=-48, el=6, fill=1.02, lens=18, dof_k=None, fstop=8.0)
    move(cam, f0, f0 + 18, c, s, az=-48, el=6, fill=1.02, kind='slide', az2=-14, el2=5, fill2=1.02)
    cam["dist"] = s * (cam.data.lens / SENSOR_HALF) / 1.02
    move(cam, f0 + 18, f1, c, s, az=-14, el=5, fill=1.02, kind='slide', az2=34, el2=6, fill2=1.10)
    return cam


SHOTS = [
    ("20", 36.43, 39.64, s20),
    ("21", 39.64, 42.32, s21),
    ("22", 42.32, 45.00, s22),
    ("23", 45.00, 47.68, s23),
    ("24", 47.68, 50.36, s24),
]
