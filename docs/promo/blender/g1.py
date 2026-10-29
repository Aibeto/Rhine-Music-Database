# -*- coding: utf-8 -*-
"""Group 1 — style anchor. Shots 01,02,03,04,05,29,30,31,32."""
import bpy
import shotkit as K
from shotkit import V, M, cam_at, move, disc, box, torus, points_mesh, dust
from math import radians as rd, sin, cos, pi
import random


def vinyl(R, N=140, dark=(0.100, 0.106, 0.102), light=(0.215, 0.232, 0.222), name=None):
    """Concentric grooves that exist: Scale = N / R, so N rings fit across the radius."""
    nm = name or ("VIN_%d_%d" % (int(R * 1000), N))
    m = bpy.data.materials.get(nm)
    if m:
        return m
    m = bpy.data.materials.new(nm)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    wv = nt.nodes.new("ShaderNodeTexWave")
    wv.wave_type = 'RINGS'
    wv.bands_direction = 'DIAGONAL'
    wv.inputs["Scale"].default_value = float(N) / max(R, 1e-4)
    wv.inputs["Distortion"].default_value = 0.0
    wv.inputs["Detail"].default_value = 0.0
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.30
    ramp.color_ramp.elements[0].color = (*dark, 1)
    ramp.color_ramp.elements[1].position = 0.86
    ramp.color_ramp.elements[1].color = (*light, 1)
    nt.links.new(tc.outputs["Object"], wv.inputs["Vector"])
    nt.links.new(wv.outputs["Fac"], ramp.inputs["Fac"])
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Strength"].default_value = 1.0
    nt.links.new(ramp.outputs["Color"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    return m



def grooves(C, off, c, R, n=28, r0=0.022, th=0.00055, mat='alu2', z=0.0, name="g"):
    """Real grooves: thin toruses. Reads at any scale, unlike a wave texture."""
    for i in range(n):
        rr = r0 + (R - r0) * i / max(1, n - 1)
        torus(C, C + "_%s%d" % (name, i), (c[0], c[1], c[2] + z), rr, th, off, mat, mv=96)


def focus(cam, k=1.0, fstop=2.8):
    cam.data.dof.use_dof = True
    cam.data.dof.focus_distance = cam["dist"] * k
    cam.data.dof.aperture_fstop = fstop
    return cam


def paint(ob, mat):
    ob.data.materials.clear()
    ob.data.materials.append(mat)
    return ob


# ---------------- 01 : a narrow blade of light, three motes, one in focus ----------------
def s01(off, C, f0, f1):
    c, s = (0.0, 0.0, 0.048), 0.052
    blade = box(C, C + "_blade", (0.004, 0.0, 0.050), (0.0020, 0.0004, 0.125), 'white_e', off)
    blade.rotation_euler = (0, rd(-16), 0)
    blade.data.materials.clear()
    blade.data.materials.append(K.L.mat_shaft("SH_BLADE", (0.96, 0.98, 1.0), strength=2.6, alpha=0.90))
    box(C, C + "_spill", (0.017, 0.0, 0.0007), (0.055, 0.0011, 0.0011), 'alu2', off)
    for i, (p, r) in enumerate([((0.006, 0.0, 0.050), 0.0024), ((0.030, 0.003, 0.078), 0.0015),
                                ((-0.018, -0.002, 0.031), 0.0016)]):
        K.L.prim('ico', C, C + "_m%d" % i, loc=off + V(p), r=r, sub=2, material=M('white_e'))
    cam = cam_at(C, off, c, s, az=33, el=6, fill=0.58, lens=100)
    focus(cam, 1.0, 4.0)
    move(cam, f0, f1, c, s, az=33, el=6, fill=0.58, kind='slide', az2=39, fill2=0.56, ease='LIN')
    return cam


# ---------------- 02 : rack focus — a corner of something enormous, still unreadable ----------------
def s02(off, C, f0, f1):
    R, cx = 0.30, 0.075
    disc(C, C + "_disc", (cx, 0, 0), R, off, mat='vinyl', thick=0.014)
    grooves(C, off, (cx, 0, 0.0072), R, n=30, r0=0.02, th=0.0006, mat='alu2', z=0.0)
    torus(C, C + "_rim", (cx, 0, 0.0072), R, 0.0022, off, 'paper')
    c, s = (cx - R * 0.55, -R * 0.52, 0.004), 0.055
    cam = cam_at(C, off, c, s, az=16, el=9, fill=3.40, lens=100)
    dd = cam["dist"]
    cam.data.dof.use_dof = True
    cam.data.dof.aperture_fstop = 2.0
    cam.data.dof.focus_distance = dd * 0.35
    cam.data.dof.keyframe_insert("focus_distance", frame=f0)
    cam.data.dof.focus_distance = dd * 1.00
    cam.data.dof.keyframe_insert("focus_distance", frame=f1)
    move(cam, f0, f1, c, s, az=16, el=7, fill=2.30, kind='slide', az2=19, fill2=3.15)
    return cam


# ---------------- 03 : the first groove appears, the disc turns, scratches catch the light ----------------
def s03(off, C, f0, f1):
    c, s = (0.052, 0.0, 0.0035), 0.019
    d = disc(C, C + "_disc", (0, 0, 0), 0.13, off, mat='vinyl')
    grooves(C, off, (0, 0, 0.0030), 0.128, n=44, r0=0.020, th=0.00045, mat='alu2', z=0.0)
    for i, ang in enumerate((14, 47, 128)):
        a = rd(ang)
        b = box(C, C + "_scr%d" % i, (0.058 * cos(a), 0.058 * sin(a), 0.0042),
                (0.075, 0.0008, 0.0002), 'alu', off)
        b.rotation_euler = (0, 0, a)
    d.keyframe_insert("rotation_euler", frame=f0)
    d.rotation_euler = (0, 0, rd(90))
    d.keyframe_insert("rotation_euler", frame=f1)
    cam = cam_at(C, off, c, s, az=8, el=54, fill=1.0, lens=100)
    focus(cam, 1.0, 5.6)
    move(cam, f0, f1, c, s, az=8, el=54, fill=1.0, kind='orbit', az2=-2, el2=48, ease='LIN')
    return cam


# ---------------- 04 : a hexagonal prism growing in an electric field ----------------
def s04(off, C, f0, f1):
    c, s = (0.0, 0.0, 0.010), 0.030
    hexp = K.L.prim('cyl', C, C + "_hex", loc=off + V(c), r=0.0115, d=0.032, v=6, material=M('paper'))
    hexp.scale = (0.18, 0.18, 0.04)
    hexp.keyframe_insert("scale", frame=f0)
    hexp.scale = (1.0, 1.0, 1.0)
    hexp.keyframe_insert("scale", frame=f1)
    # six bare edges, so the prism reads as a crystal instead of a soft blob
    for i in range(6):
        a = rd(i * 60 + 30)
        e = box(C, C + "_edge%d" % i, (0.0118 * cos(a), 0.0118 * sin(a), 0.010),
                (0.0011, 0.0011, 0.032), 'ink', off)
    # the field: two vertical hairlines either side
    for sx in (-1, 1):
        box(C, C + "_f%d" % sx, (sx * 0.026, 0, 0.010), (0.0005, 0.0005, 0.075), 'mint', off)
    # base glow pad
    K.L.prim('cyl', C, C + "_pad", loc=off + V((0, 0, -0.0062)), r=0.020, d=0.0012,
             material=K.flat("F_PAD", (0.56, 0.89, 0.73), fac=0.2, strength=0.7, alpha=0.22))
    cam = cam_at(C, off, c, s, az=24, el=10, fill=1.0, lens=100)
    focus(cam, 1.0, 5.6)
    move(cam, f0, f1, c, s, az=24, el=10, fill=1.0, kind='slide', az2=-16, el2=6, fill2=0.92)
    return cam


# ---------------- 05 : pull back — it was never a crystal, it is a column of records ----------------
def s05(off, C, f0, f1):
    n = 20
    rr = random.Random(7)
    for i in range(n):
        z = 0.010 + i * 0.0120
        d = disc(C, C + "_d%d" % i, (0, 0, z), 0.155, off, mat='vinyl')
        d.rotation_euler = (rd(rr.uniform(-1.2, 1.2)), rd(rr.uniform(-1.2, 1.2)), rd(rr.uniform(0, 360)))
        torus(C, C + "_r%d" % i, (0, 0, z + 0.0035), 0.152, 0.0012, off, 'alu2', mv=96)
    dust(C, off, (0, 0, 0.13), 0.14, n=36, r_k=0.007, seed=9)
    c = (0.0, 0.0, 0.125)
    size = 0.24
    cam = cam_at(C, off, c, size, az=18, el=74, fill=0.95, lens=50)
    move(cam, f0, f1, c, size, az=18, el=74, fill=0.95, kind='pull',
         az2=34, el2=8, fill2=0.90, ease='OUT')
    return cam


# ---------------- 29 : retreat, the disc shrinks to a point ----------------
def s29(off, C, f0, f1):
    c, s = (0.0, 0.0, 0.0), 0.16
    disc(C, C + "_disc", (0, 0, 0), 0.16, off, mat='vinyl')
    grooves(C, off, (0, 0, 0.0030), 0.158, n=34, r0=0.02, th=0.0007, mat='alu2', z=0.0)
    torus(C, C + "_rim", (0, 0, 0.0030), 0.158, 0.0016, off, 'paper', mv=96)
    K.L.prim('cyl', C, C + "_hub", loc=off + V((0, 0, 0.0024)), r=0.010, d=0.004, material=M('paper'))
    cam = cam_at(C, off, c, s, az=8, el=24, fill=0.80, lens=50)
    move(cam, f0, f1, c, s, az=8, el=24, fill=0.80, kind='pull', az2=-6, el2=36, fill2=0.09, ease='LIN')
    return cam


# ---------------- 30 : the point unfolds into a line (the 7th tick), orange marker at its left ----------------
def s30(off, C, f0, f1):
    line = box(C, C + "_line", (0, 0, 0), (0.62, 0.0010, 0.0010), 'paper', off)
    line.scale = (0.0008, 0.0010, 0.0010)
    line.keyframe_insert("scale", frame=f0)
    line.scale = (0.62, 0.0010, 0.0010)
    line.keyframe_insert("scale", frame=f0 + 8)
    sq = box(C, C + "_sq", (-0.31, 0, 0), (0.011, 0.011, 0.0014), 'orange', off)
    sq.scale = (0.0001, 0.0001, 0.0001)
    sq.keyframe_insert("scale", frame=f0 + 7)
    sq.scale = (0.011, 0.011, 0.0014)
    sq.keyframe_insert("scale", frame=f0 + 11)
    c, s = (0.0, 0.0, 0.0), 0.06
    return cam_at(C, off, c, s, az=0, el=0, fill=1.0, lens=200, ortho=0.70)


# ---------------- 31 : the wordmark appears, no animation, +0.3em ----------------
def s31(off, C, f0, f1):
    cu = bpy.data.curves.new("wm31", type='FONT')
    cu.body = "RHINE  MUSIC  DATABASE"
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    cu.size = 0.046
    cu.space_character = 1.30
    ob = bpy.data.objects.new("wm31", cu)
    ob.rotation_euler = (rd(90), 0, 0)
    ob.location = off + V((0.0, 0.0, 0.036))
    ob.data.materials.append(M('paper'))
    K.L.link(ob, C)
    box(C, C + "_line", (0, 0, 0), (0.62, 0.0010, 0.0010), 'paper', off)
    box(C, C + "_sq", (-0.31, 0, 0), (0.011, 0.011, 0.0014), 'orange', off)
    c, s = (0.0, 0.0, 0.012), 0.06
    return cam_at(C, off, c, s, az=0, el=0, fill=1.0, lens=200, ortho=0.70)


# ---------------- 32 : black, only the line left; then even that goes ----------------
def s32(off, C, f0, f1):
    line = box(C, C + "_line", (0, 0, 0), (0.62, 0.0010, 0.0010), 'paper', off)
    line.keyframe_insert("scale", frame=f0)
    line.scale = (0.62, 0.0010, 0.0010)
    line.keyframe_insert("scale", frame=f0 + 2)
    line.scale = (0.0001, 0.0001, 0.0001)
    line.keyframe_insert("scale", frame=f0 + 4)
    c, s = (0.0, 0.0, 0.0), 0.06
    return cam_at(C, off, c, s, az=0, el=0, fill=1.0, lens=200, ortho=0.70)


SHOTS = [
    ("01", 0.0000, 1.6071, s01),
    ("02", 1.6071, 2.6786, s02),
    ("03", 2.6786, 4.2857, s03),
    ("04", 4.2857, 5.8929, s04),
    ("05", 5.8929, 7.5000, s05),
    ("29", 61.6071, 64.2857, s29),
    ("30", 64.2857, 67.0000, s30),
    ("31", 67.0000, 69.6071, s31),
    ("32", 69.6071, 70.7143, s32),
]
