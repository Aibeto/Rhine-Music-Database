# -*- coding: utf-8 -*-
"""Group 5 — 17 (eye / dolly-zoom), 18 (spectrum tunnel), 19 (10-cut flash montage).

Frozen API: shotkit.py / stages.py are imported, never edited.  Flat emission only,
no lights, no orange (reserved for shot 25), no readable text anywhere.
Builder contract: builder(off, C, f0, f1) -> camera | [(cam, fa, fb), ...]
"""
import bpy, bmesh, math, random
from mathutils import Vector, Matrix
from math import radians as rd, sin, cos, pi
import pvlib as L
import shotkit as K
import stages as _S
from shotkit import V, M, flat, SENSOR_HALF, cam_at, move, disc, box, torus, points_mesh, dust


# ======================================================================
# local materials.  stages.M('ink') is emission .126 which renders mid-grey,
# so near-black / dim surfaces get their own keys registered in stages.MAT
# (registered there so M(), box() and torus() all accept the string key).
# ======================================================================
def _mats():
    if 'blk' in _S.MAT:
        return
    _S.MAT['blk'] = flat("F_BLK5", (0.012, 0.013, 0.011), fac=0.25)
    _S.MAT['scl'] = flat("F_SCL5", (0.960, 0.960, 0.940), fac=0.13)
    _S.MAT['iris'] = flat("F_IRIS5", (0.078, 0.094, 0.062), fac=0.55)
    _S.MAT['glow'] = flat("F_GLOW5", (1.0, 1.0, 1.0), fac=0.75, strength=3.2)
    _S.MAT['dim'] = flat("F_DIM5", (0.235, 0.255, 0.235), fac=0.30)
    _S.MAT['grat'] = flat("F_GRAT5", (0.048, 0.078, 0.058), fac=0.40)
    _S.MAT['liq'] = flat("F_LIQ5", (0.330, 0.400, 0.380), fac=0.50)
    _S.MAT['discm'] = flat("F_DISC5", (0.150, 0.160, 0.150), fac=0.35)
    _S.MAT['mintd'] = flat("F_MINTD5", (0.150, 0.440, 0.330), fac=0.55)
    _S.MAT['irh'] = flat("F_IRH5", (0.172, 0.190, 0.140), fac=0.60)
    _S.MAT['barb'] = flat("F_BARB5", (0.048, 0.053, 0.047), fac=0.40)


def _blk():
    _mats(); return _S.MAT['blk']


def _scl():
    _mats(); return _S.MAT['scl']


def _iris():
    _mats(); return _S.MAT['iris']


def _glow():
    _mats(); return _S.MAT['glow']


def _dim():
    _mats(); return _S.MAT['dim']


def _grat():
    _mats(); return _S.MAT['grat']


def _liq():
    _mats(); return _S.MAT['liq']


def _discmat():
    _mats(); return _S.MAT['discm']


def _mintd():
    _mats(); return _S.MAT['mintd']


def _res(mat):
    """Accept a stages.MAT key or a Material object."""
    return _S.M(mat) if isinstance(mat, str) else mat


def _box(C, name, c, dim, mat, off, bevel=0.0, rot=None):
    _mats()
    b = L.prim('cube', C, name, loc=off + V(c), scale=tuple(dim), material=_res(mat))
    if rot:
        b.rotation_euler = rot
    if bevel:
        L.bevel(b, bevel, 2)
    return b


def _tor(C, name, c, R_, r_, off, mat, rot=(rd(90), 0, 0), mv=128):
    _mats()
    ob = L.prim('torus', C, name, loc=off + V(c), R=R_, r=r_, mv=mv, nv=8,
                material=_res(mat))
    ob.rotation_euler = rot
    L.smooth(ob)
    return ob


# ======================================================================
# helpers
# ======================================================================

def _cam_keys(cam, off, c, size, fa, fb, keys, focus=True):
    """keys = [(u, lens, fill, az, el), ...] -> location + focal length (+ focus pull)."""
    for (u, lens, fill, az, el) in keys:
        f = int(round(fa + (fb - fa) * u))
        d = size * (lens / SENSOR_HALF) / max(fill, 0.02)
        a, e = rd(az), rd(el)
        p = off + V(c) + V((sin(a) * cos(e), -cos(a) * cos(e), sin(e))) * d
        L.key_cam(cam, f, loc=p, target=off + V(c))
        cam.data.lens = lens
        cam.data.keyframe_insert("lens", frame=f)
        if focus and cam.data.dof.use_dof:
            cam.data.dof.focus_distance = max(0.002, d)
            cam.data.keyframe_insert("dof.focus_distance", frame=f)
    return cam


def _hold(cam, sub, c, fa, fb):
    """Static camera: orientation keys inside its own cut."""
    L.key_cam(cam, fa, loc=cam.location.copy(), target=sub + V(c))
    L.key_cam(cam, fb, loc=cam.location.copy(), target=sub + V(c))
    return cam


def _ring(C, name, c, R_, minor, mat, sub, rot=(rd(90), 0, 0), mv=128):
    return _tor(C, name, c, R_, minor, sub, mat, rot=rot, mv=mv)


def _grooves(C, name, sub, c, radii, minor, mat, rot=(rd(90), 0, 0), mv=96):
    for i, r in enumerate(radii):
        _ring(C, "%s%02d" % (name, i), c, r, minor, mat, sub, rot=rot, mv=mv)


def _annulus(C, name, c, r_in, r_out, thick, mat, sub, seg=120):
    """Flat ring with a real hole, local XY, stood up to face -Y."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    ti, to, bi, bo = [], [], [], []
    for i in range(seg):
        a = 2 * pi * i / seg
        ca, sa = cos(a), sin(a)
        ti.append(bm.verts.new((r_in * ca, r_in * sa, thick * 0.5)))
        to.append(bm.verts.new((r_out * ca, r_out * sa, thick * 0.5)))
        bi.append(bm.verts.new((r_in * ca, r_in * sa, -thick * 0.5)))
        bo.append(bm.verts.new((r_out * ca, r_out * sa, -thick * 0.5)))
    for i in range(seg):
        j = (i + 1) % seg
        bm.faces.new((ti[i], to[i], to[j], ti[j]))
        bm.faces.new((bo[j], bi[j], bi[i], bo[i]))
        bm.faces.new((to[i], bo[i], bo[j], to[j]))
        bm.faces.new((bi[j], ti[j], ti[i], bi[i]))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(_res(mat))
    ob.location = sub + V(c)
    ob.rotation_euler = (rd(90), 0, 0)
    L.link(ob, C)
    return ob


def _bars(C, name, sub, spec, base_z, depth=0.055, down=False, cap=0.0034):
    """spec = [(x, y, h, w, mat_slot)]; the object origin sits on base_z, so an
    animated scale.z reads as a spectrum rise.  Bodies are near-black and only a
    thin slab at the far end of each bar is lit."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()

    def slab(x, y, w, zc, hh, mi):
        Mx = Matrix.Translation((x, y, zc)) @ Matrix.Diagonal((w, depth, hh, 1.0))
        bm.faces.ensure_lookup_table()
        n0 = len(bm.faces)
        bmesh.ops.create_cube(bm, size=1.0, matrix=Mx)
        bm.faces.ensure_lookup_table()
        for f in bm.faces[n0:]:
            f.material_index = mi

    for (x, y, h, w, mi) in spec:
        zc = -h * 0.5 if down else h * 0.5
        slab(x, y, w, zc, h, mi)
        if cap > 0.0:
            zc2 = (-h + cap * 0.5) if down else (h - cap * 0.5)
            slab(x, y, w * 1.16, zc2, cap, 3)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    ob.data.materials.append(M('barb'))
    ob.data.materials.append(M('mint'))
    ob.data.materials.append(_dim())
    ob.data.materials.append(M('paper'))
    ob.location = sub + V((0, 0, base_z))
    L.link(ob, C)
    return ob


# ======================================================================
# 17 · the eye surfaces from the particle cloud · 100mm -> 25mm · into the pupil
# ======================================================================
EYE_R = 0.050
EYE_FY = 0.62


def _eye(C, sub, c, k=1.0, lid_z=None, lid=False, animate=True, fa=0, fb=1):
    """Paper eye facing -Y: facing-graded eyeball, iris fibres as record grooves,
    near-black pupil with cold light inside.  Returns (pupil, ring, lid)."""
    x, y, z = c
    R = EYE_R * k
    fy = EYE_FY * k
    pole = y - fy * R
    # eyeball: facing gradient = the sphere cue
    L.prim('sphere', C, C + "_scl", loc=sub + V(c), r=R, scale=(1.0, EYE_FY, 1.0),
           seg=64, ring=40, material=_scl())
    L.smooth(bpy.context.object)
    # cornea grooves (only the outer latitudes)
    for i in range(3):
        rho = (0.0370 + i * 0.0053) * k
        yy = pole + fy * R * (1.0 - math.sqrt(max(0.0, 1.0 - (rho / R) ** 2)))
        _ring(C, C + "_g%d" % i, (x, yy - 0.0006 * k, z), rho, 0.00045 * k, 'paper2', sub)
    # iris + limbus + fibres
    L.prim('cyl', C, C + "_iris", loc=sub + V((x, pole - 0.0018, z)), r=0.0238 * k, d=0.0016,
           rot=(rd(90), 0, 0), v=96, material=_iris())
    _ring(C, C + "_limb", (x, pole - 0.0030, z), 0.0246 * k, 0.0017 * k, 'blk', sub, mv=128)
    for i in range(6):
        _ring(C, C + "_ir%d" % i, (x, pole - 0.0032, z), (0.0222 - i * 0.0019) * k,
              0.00052 * k, 'irh', sub, mv=96)
    # pupil = the aperture
    pup = L.prim('cyl', C, C + "_pup", loc=sub + V((x, pole - 0.0044, z)), r=0.0096 * k,
                 d=0.0016, rot=(rd(90), 0, 0), v=64, material=_blk())
    pr = _ring(C, C + "_pring", (x, pole - 0.0056, z), 0.0104 * k, 0.00075 * k, 'white_e',
               sub, mv=96)
    L.prim('cyl', C, C + "_plight", loc=sub + V((x, pole - 0.0054, z)), r=0.0029 * k,
           d=0.0014, rot=(rd(90), 0, 0), v=48, material=M('white_e'))
    # wet specular on the cornea, upper left
    sp = L.prim('ico', C, C + "_spec", loc=sub + V((x - 0.0166, pole - 0.0016, z + 0.0198)),
                r=0.0056 * k, sub=2, material=M('white_e'))
    sp.scale = (1.0, 0.42, 1.0)
    # upper lid band (its edge crosses the eyeball as a shallow arc)
    lidob = None
    if lid_z is not None:
        L.prim('cyl', C, C + "_lidb", loc=sub + V((x, pole - 0.0072, z + 0.30 + lid_z)),
               r=0.30, d=0.004, rot=(rd(90), 0, 0), v=96, material=M('paper2'))
        _box(C, C + "_lash", (x, pole - 0.0078, z + lid_z), (0.150, 0.0016, 0.0014),
             'ink', sub)
    if lid:
        lidob = L.prim('cyl', C, C + "_lid", loc=sub + V((x, pole - 0.0100, z + 0.36)),
                       r=0.30 * k, d=0.004, rot=(rd(90), 0, 0), v=96, material=M('moss'))
        _ring(C, C + "_lidr", (x, pole - 0.0140, z + 0.36), 0.2992 * k, 0.0024 * k, 'blk',
              sub, mv=160)
    if animate:
        for ob, s0, s1 in ((pup, 1.0, 0.62), (pr, 1.0, 0.64)):
            ob.keyframe_insert("scale", frame=fa)
            ob.scale = (s1, s1, 1.0)
            ob.keyframe_insert("scale", frame=fb)
    return pup, pr, lidob


def s17(off, C, f0, f1):
    _eye(C, off, (0, 0, 0), fa=f0, fb=f1)
    # particle cloud: grit around and behind the cornea
    rr = random.Random(1717)
    near = [off + V((rr.uniform(-0.30, 0.30), rr.uniform(-0.135, -0.040), rr.uniform(-0.26, 0.26)))
            for _ in range(46)]
    d1 = points_mesh(C, C + "_dust_n", near, size=0.0010, mat='paper2', sub=1)
    far = [off + V((rr.uniform(-0.55, 0.55), rr.uniform(0.020, 0.34), rr.uniform(-0.45, 0.45)))
           for _ in range(110)]
    d2 = points_mesh(C, C + "_dust_f", far, size=0.0020, mat='paper', sub=1)
    for ob, dx, dz in ((d1, 0.004, 0.020), (d2, -0.004, -0.012)):
        ob.keyframe_insert("location", frame=f0)
        ob.location = ob.location + V((dx, 0, dz))
        ob.keyframe_insert("location", frame=f1)
    c, s = (0.004, 0, -0.0025), 0.056
    cam = cam_at(C, off, c, s, az=1.5, el=3.0, fill=0.95, lens=100, dof_k=1.0, fstop=4.0)
    _cam_keys(cam, off, c, s, f0, f1, [
        (0.00, 100.0, 0.95, 1.5, 3.0),
        (0.30, 70.0, 0.95, 1.5, 3.0),
        (0.55, 42.0, 0.97, 1.5, 3.0),
        (0.76, 30.0, 1.12, 1.5, 3.0),
        (1.00, 25.0, 2.70, 1.5, 3.0),
    ])
    return cam


# ======================================================================
# 18 · 25mm tunnel: vertical spectrum ranks, fast forward
# ======================================================================
FLOOR, CEIL, WALL = -0.62, 1.02, 1.20


def _rank(C, name, sub, x, down, seed, y0=-0.6, y1=11.2, pitch=0.195, w=0.044,
          lo=0.06, span=0.62, spike=1, base_z=None):
    rr = random.Random(seed)
    spec = []
    n = int((y1 - y0) / pitch)
    for j in range(n):
        y = y0 + j * pitch
        r = rr.random()
        h = lo + span * (r ** 2)
        if spike and j % 7 == 3:
            h = 0.80 + 0.55 * rr.random()
        spec.append((x, y, h, w, 1 if j % 11 == 5 else (2 if j % 4 == 1 else 0)))
    return _bars(C, name, sub, spec, FLOOR if base_z is None else base_z, down=down)


def s18(off, C, f0, f1):
    bands = [
        _rank(C, C + "_wl", off, -1.05, False, 1811),
        _rank(C, C + "_wr", off, 1.05, False, 1812),
        _rank(C, C + "_c0", off, 0.0, True, 1813, lo=0.10, span=0.34, spike=0,
              base_z=CEIL, w=0.064),
        _rank(C, C + "_c1", off, -0.58, True, 1814, lo=0.05, span=0.26, spike=0,
              base_z=CEIL, w=0.055),
        _rank(C, C + "_c2", off, 0.58, True, 1815, lo=0.05, span=0.26, spike=0,
              base_z=CEIL, w=0.055),
    ]
    for i, ob in enumerate(bands):
        ob.keyframe_insert("scale", frame=f0)
        ob.scale = (1, 1, 1.16)
        ob.keyframe_insert("scale", frame=f0 + 7 + i * 5)
        ob.scale = (1, 1, 0.88)
        ob.keyframe_insert("scale", frame=f1)
    # the light at the end
    L.prim('cyl', C, C + "_mouth", loc=off + V((0, 10.3, 0.20)), r=0.62, d=0.02,
           rot=(rd(90), 0, 0), v=96, material=_glow())
    _ring(C, C + "_mouth_r", (0, 10.28, 0.20), 0.65, 0.008, 'mint', off, mv=128)
    # floor: two long rails + one bright centre stripe (speed cue)
    _box(C, C + "_rail0", (0, 5.2, FLOOR + 0.003), (0.014, 11.8, 0.004), 'dim', off)
    for side in (-1, 1):
        _box(C, C + "_rail%d" % side, (side * 0.24, 5.2, FLOOR + 0.002),
             (0.008, 11.8, 0.003), 'barb', off)

    cam = L.cam(C + "_cam", C, off + V((0, -0.30, 0.30)), off + V((0, 9.0, 0.30)), lens=25.0)
    cam.data.clip_start = 0.02
    cam.data.clip_end = 800.0
    for (u, y) in ((0.0, -0.30), (0.42, 1.15), (0.78, 3.75), (1.0, 6.20)):
        f = int(round(f0 + (f1 - f0) * u))
        L.key_cam(cam, f, loc=off + V((0, y, 0.30)), target=off + V((0, y + 9.0, 0.30)))
    return cam


# ======================================================================
# 19 · ten cuts
# ======================================================================

# ---- 19a · a glass of water rippling -------------------------------------------------
def _m_glass(sub, C, fa, fb):
    # rings in black: a black dish, three bright rings, one meniscus line at the rim
    c, s = (0.0, 0.0, 0.0012), 0.0232
    L.prim('cyl', C, C + "_dish", loc=sub + V((0, 0, 0.0)), r=0.0342, d=0.0032, v=96,
           material=_blk())
    for k, rr_ in enumerate((0.0108, 0.0204, 0.0294)):
        ob = _ring(C, C + "_r%d" % k, (0, 0, 0.0024), rr_, 0.00105, 'white_e', sub,
                   rot=(0, 0, 0), mv=128)
        ob.keyframe_insert("scale", frame=fa)
        ob.scale = (1.10, 1.10, 1.0)
        ob.keyframe_insert("scale", frame=fb)
    _ring(C, C + "_men", (0, 0, 0.0026), 0.0334, 0.00130, 'white_e', sub, rot=(0, 0, 0),
          mv=160)
    cam = cam_at(C, sub, c, s, az=10, el=42, fill=1.0, lens=100, dof_k=1.0, fstop=4.0)
    return _hold(cam, sub, c, fa, fb)


# ---- 19b · stylus tip catching the light ---------------------------------------------
def _m_needle(sub, C, fa, fb):
    tip = V((0.086, 0.0, 0.0030))
    c, s = (0.078, 0.0, 0.025), 0.062
    L.prim('cyl', C, C + "_rec", loc=sub + V((0, 0, -0.004)), r=0.175, d=0.008, v=96,
           material=_discmat())
    _grooves(C, C + "_gr", sub, (0, 0, 0.0005), [0.030 + i * 0.0060 for i in range(23)],
             0.00034, 'paper2', rot=(0, 0, 0), mv=96)
    ang = rd(30)
    d_ = V((-sin(ang), 0, cos(ang)))
    ctr = tip + d_ * 0.026
    nd = L.prim('cone', C, C + "_nd", loc=sub + ctr, r1=0.0006, r2=0.0017, d=0.052, v=20,
                material=M('alu'))
    nd.rotation_euler = (rd(180), -ang, 0)
    _box(C, C + "_shell", tuple(ctr + d_ * 0.030), (0.019, 0.008, 0.011), 'alu2', sub,
         bevel=0.0010, rot=(0, -ang, 0))
    L.prim('ico', C, C + "_glint", loc=sub + tip + V((0, -0.0014, 0.0004)), r=0.0022, sub=2,
           material=M('white_e'))
    _box(C, C + "_refl", tuple(tip + V((0, -0.016, 0.0002))), (0.0016, 0.026, 0.0005),
         'dim', sub, rot=(0, 0, rd(-5)))
    cam = cam_at(C, sub, c, s, az=-11, el=20, fill=1.0, lens=100, dof_k=1.0, fstop=2.2)
    return _hold(cam, sub, c, fa, fb)


# ---- 19c · two hands turning a record (the only human in the film) -------------------
def _hand(C, name, sub, base, ang, side=1.0):
    ca, sa = cos(ang), sin(ang)

    def part(nm, lx, lz, dim, mat, extra=0.0, bevel=0.0016):
        p = V(base) + V((lx * ca + lz * sa, 0.0, -lx * sa + lz * ca))
        return _box(C, name + "_" + nm, tuple(p), dim, mat, sub, bevel=bevel,
                    rot=(0, ang + extra, 0))

    part("palm", -0.020, 0.0, (0.046, 0.019, 0.054), 'paper')
    for k in range(4):
        ln = 0.052 - 0.0034 * k
        lz = 0.0198 - k * 0.0132
        part("f%d" % k, 0.004 + ln * 0.5, lz, (ln, 0.0165, 0.0118), 'paper',
             extra=side * (0.13 - 0.058 * k))
        if k < 3:
            part("gap%d" % k, 0.022, lz - 0.0066, (0.034, 0.0205, 0.0026), 'ink', bevel=0.0)
    part("thumb", -0.030, -0.022, (0.036, 0.018, 0.015), 'paper', extra=-0.55 * side)


def _m_hand(sub, C, fa, fb):
    c, s = (0.0, 0.0, 0.026), 0.108
    rot_t = (rd(68), 0, rd(9))
    rc = L.prim('cyl', C, C + "_rec", loc=sub + V((0, 0, 0.018)), r=0.113, d=0.004, v=96,
                material=M('vinyl'))
    rc.rotation_euler = rot_t
    _grooves(C, C + "_gr", sub, (0, -0.0018, 0.018), [0.026 + i * 0.0064 for i in range(13)],
             0.00045, 'paper2', rot=rot_t, mv=80)
    _ring(C, C + "_hole", (0, -0.0022, 0.018), 0.0138, 0.0013, 'white_e', sub, rot=rot_t,
          mv=96)
    _ring(C, C + "_rim", (0, -0.0014, 0.018), 0.1122, 0.0012, 'white_e', sub, rot=rot_t,
          mv=128)
    _hand(C, C + "_hl", sub, (-0.082, -0.022, 0.010), 0.26, side=1.0)
    _hand(C, C + "_hr", sub, (0.080, -0.006, -0.004), pi - 0.22, side=-1.0)
    cam = cam_at(C, sub, c, s, az=-7, el=17, fill=1.02, lens=100, dof_k=1.05, fstop=2.8)
    return _hold(cam, sub, c, fa, fb)


# ---- 19d · old oscilloscope, green trace ---------------------------------------------
def _m_scope(sub, C, fa, fb):
    c, s = (0.0, 0.0, 0.0), 0.104
    _box(C, C + "_bez", (0, 0.012, 0), (0.400, 0.020, 0.274), 'paper2', sub, bevel=0.005)
    _box(C, C + "_scr", (0, 0.0, 0.010), (0.286, 0.010, 0.168), 'blk', sub)
    for i in range(4):
        _box(C, C + "_gh%d" % i, (0, -0.0056, -0.048 + i * 0.040), (0.272, 0.0008, 0.0007),
             'grat', sub)
    for i in range(5):
        _box(C, C + "_gv%d" % i, (-0.108 + i * 0.054, -0.0056, 0.010), (0.0007, 0.0008, 0.160),
             'grat', sub)
    n = 96
    pts = []
    for i in range(n):
        u = i / (n - 1.0)
        x = -0.120 + 0.240 * u
        z = 0.010 + 0.042 * sin(u * 10.4) * (1.0 - 0.42 * u)
        z += 0.030 * math.exp(-((u - 0.30) ** 2) / 0.0018)
        z -= 0.032 * math.exp(-((u - 0.63) ** 2) / 0.0024)
        pts.append((x, z))
    for i in range(n - 1):
        (x0, z0), (x1, z1) = pts[i], pts[i + 1]
        _box(C, C + "_tr%03d" % i, ((x0 + x1) * 0.5, -0.0066, (z0 + z1) * 0.5),
             (0.0034, 0.0016, abs(z1 - z0) + 0.0028), 'mint', sub, bevel=0.0)
    L.prim('ico', C, C + "_dot", loc=sub + V((pts[-1][0], -0.0072, pts[-1][1])), r=0.0026,
           sub=2, material=M('white_e'))
    _box(C, C + "_led0", (0.104, -0.002, -0.098), (0.052, 0.004, 0.004), 'mint', sub)
    _box(C, C + "_led1", (0.150, -0.002, -0.098), (0.014, 0.004, 0.004), 'blk', sub)
    cam = cam_at(C, sub, c, s, az=0, el=0, fill=1.04, lens=50, dof_k=None, fstop=4.0)
    return _hold(cam, sub, c, fa, fb)


# ---- 19e · the eye closes ------------------------------------------------------------
def _m_blink(sub, C, fa, fb):
    # the same eye as 17, the aperture closing to a hard-edged mint slit
    _eye(C, sub, (0, 0, 0), animate=False)
    pole = -EYE_FY * EYE_R
    sl = L.prim('cube', C, C + "_slit", loc=sub + V((0, pole - 0.0072, 0)),
                scale=(0.0300, 0.0014, 1.0), material=M('mint'))
    for f, hh in ((fa, 0.0132), (fb, 0.0006)):
        sl.scale = (0.0300, 0.0014, hh)
        sl.keyframe_insert("scale", frame=f)
    cam = cam_at(C, sub, (0, 0, 0), 0.050, az=0, el=2.0, fill=0.98, lens=100, dof_k=1.0,
                 fstop=4.5)
    return _hold(cam, sub, (0, 0, 0), fa, fb)


# ---- 19f · tape pulled out and tangled ----------------------------------------------
def _m_tape(sub, C, fa, fb):
    c, s = (0.010, 0.0, 0.030), 0.062
    _box(C, C + "_shell", (-0.016, 0.0, 0.030), (0.116, 0.028, 0.074), 'alu2', sub, bevel=0.003)
    _box(C, C + "_win", (-0.016, -0.0148, 0.032), (0.078, 0.002, 0.032), 'blk', sub)
    for k in range(2):
        _ring(C, C + "_hub%d" % k, (-0.043 + k * 0.054, -0.0160, 0.032), 0.0110, 0.0022,
              'mint', sub, mv=64)
    _box(C, C + "_lead", (0.050, -0.008, 0.010), (0.060, 0.0012, 0.009), 'paper2', sub,
         rot=(0, rd(14), rd(-22)))
    rr = random.Random(46)
    for k in range(5):
        R_ = 0.012 + 0.0060 * k
        _ring(C, C + "_lp%d" % k, (0.088 + rr.uniform(-0.008, 0.014), rr.uniform(-0.008, 0.008),
                                   0.006 + rr.uniform(-0.010, 0.016)), R_, 0.0018, 'paper2', sub,
              rot=(rd(90) + rd(rr.uniform(-45, 45)), rd(rr.uniform(-25, 25)), rd(k * 41)), mv=64)
    for k in range(3):
        _box(C, C + "_tp%d" % k, (0.074 + k * 0.013, rr.uniform(-0.006, 0.006), 0.014 + k * 0.009),
             (0.040, 0.0008, 0.004), 'paper2', sub,
             rot=(rd(rr.uniform(-35, 35)), rd(rr.uniform(-45, 45)), rd(k * 57)))
    cam = cam_at(C, sub, c, s, az=17, el=13, fill=0.94, lens=100, dof_k=1.0, fstop=2.8)
    return _hold(cam, sub, c, fa, fb)


# ---- 19g · the city rises like an equaliser ----------------------------------------
def _m_city(sub, C, fa, fb):
    c, s = (0.0, 0.02, 0.052), 0.098
    cols, rows = 13, 3
    for r in range(rows):
        for i in range(cols):
            x = (i - (cols - 1) / 2.0) * 0.0244
            y = r * 0.040
            h = (0.026 + 0.094 * (0.5 + 0.5 * sin(i * 0.92 + r * 1.9 + 0.6)) ** 1.2)
            h *= 1.0 - 0.10 * r
            _box(C, C + "_b%02d%d" % (i, r), (x, y, h * 0.5), (0.0180, 0.0180, h), 'dim', sub)
            _box(C, C + "_w%02d%d" % (i, r), (x + 0.0034, y - 0.0092, h * 0.44),
                 (0.0026, 0.0020, h * 0.62), 'mint', sub)
            _box(C, C + "_s%02d%d" % (i, r), (x - 0.0030, y - 0.0092, h * 0.70),
                 (0.0090, 0.0020, 0.0020), 'mint', sub)
            _box(C, C + "_c%02d%d" % (i, r), (x, y, h + 0.0014), (0.0192, 0.0192, 0.0028),
                 'paper', sub)
    cam = cam_at(C, sub, c, s, az=11, el=9, fill=1.02, lens=50, dof_k=None, fstop=5.6)
    return _hold(cam, sub, c, fa, fb)


# ---- 19h · one drop, ripples into concentric rings ---------------------------------
def _m_drop(sub, C, fa, fb):
    c, s = (0.012, 0.0, 0.014), 0.060
    ix, iy = 0.014, -0.008
    L.prim('cyl', C, C + "_d", loc=sub + V((0, 0, 0.007)), r=0.104, d=0.005, v=96,
           material=_discmat())
    L.prim('cyl', C, C + "_edge", loc=sub + V((0, 0, 0.0042)), r=0.1046, d=0.0022, v=96,
           material=M('paper2'))
    _grooves(C, C + "_gr", sub, (0, 0, 0.0098), [0.022 + i * 0.0092 for i in range(9)],
             0.00035, 'dim', rot=(0, 0, 0), mv=96)
    for k in range(6):
        rr_ = 0.009 + k * 0.0126
        ob = _ring(C, C + "_r%d" % k, (ix, iy, 0.0106), rr_, 0.00074, 'white_e', sub,
                   rot=(0, 0, 0), mv=112)
        ob.keyframe_insert("scale", frame=fa)
        ob.scale = (1.14, 1.14, 1.0)
        ob.keyframe_insert("scale", frame=fb)
    drop = L.prim('ico', C, C + "_w", loc=sub + V((ix, iy, 0.062)), r=0.0054, sub=3,
                  material=M('white_e'))
    drop.keyframe_insert("location", frame=fa)
    drop.location = sub + V((ix, iy, 0.0112))
    drop.keyframe_insert("location", frame=fb)
    cam = cam_at(C, sub, c, s, az=-7, el=15, fill=1.0, lens=100, dof_k=1.0, fstop=3.5)
    return _hold(cam, sub, c, fa, fb)


# ---- 19i · archive boxes popping open, top-down ------------------------------------
def _m_open(sub, C, fa, fb):
    c, s = (0.0, 0.0, 0.030), 0.148
    pitch, boxw, bh, lidw, lt = 0.077, 0.056, 0.030, 0.060, 0.0045
    lids = []
    k = 0
    for j in range(3):
        for i in range(3):
            x = (i - 1) * pitch
            y = (j - 1) * pitch
            _box(C, C + "_b%d%d" % (i, j), (x, y, 0.015), (boxw, boxw, bh), 'paper', sub,
                 bevel=0.002)
            _box(C, C + "_rim%d%d" % (i, j), (x, y, 0.0302), (boxw + 0.0012, boxw + 0.0012, 0.0016),
                 'ink', sub)
            _box(C, C + "_in%d%d" % (i, j), (x, y, 0.0314), (boxw - 0.010, boxw - 0.010, 0.0012),
                 'moss', sub)
            if (i, j) == (1, 1):
                _box(C, C + "_rule", (x, y - boxw * 0.5 + 0.0030, 0.0195),
                     (boxw - 0.014, 0.0016, 0.0016), 'mint', sub)
            hy, hz = y + boxw * 0.5, bh
            lo = _box(C, C + "_l%d%d" % (i, j), (x, hy - lidw * 0.5, hz + lt * 0.5),
                      (lidw, lidw, lt), 'paper2', sub, bevel=0.0012)
            lids.append((lo, x, hy, hz, k))
            k += 1
    for (lo, x, hy, hz, k) in lids:
        for f, th in ((fa, rd(-9.0 - 7.0 * (k % 3))), (fb, rd(-66.0 - 12.0 * (k % 4)))):
            cy = hy - (lidw * 0.5) * cos(th) - (lt * 0.5) * sin(th)
            cz = hz + (lt * 0.5) * cos(th) - (lidw * 0.5) * sin(th)
            lo.location = sub + V((x, cy, cz))
            lo.rotation_euler = (th, 0, 0)
            lo.keyframe_insert("location", frame=f)
            lo.keyframe_insert("rotation_euler", frame=f)
    cam = cam_at(C, sub, c, s, az=0, el=62, fill=1.26, lens=28, dof_k=None, fstop=8.0)
    return _hold(cam, sub, c, fa, fb)


# ---- 19j · the hole in the record, and the stars inside ----------------------------
def _m_hole(sub, C, fa, fb):
    c, s = (0.0, 0.0, 0.020), 0.112
    _annulus(C, C + "_rec", c, 0.030, 0.106, 0.005, 'vinyl', sub)
    _ring(C, C + "_rim", c, 0.1055, 0.0014, 'paper2', sub, mv=128)
    _grooves(C, C + "_gr", sub, (0, -0.0034, 0.020), [0.036 + i * 0.0042 for i in range(16)],
             0.00045, 'moss', mv=96)
    _ring(C, C + "_hole", (0, -0.0038, 0.020), 0.0302, 0.0016, 'white_e', sub, mv=128)
    rr = random.Random(1902)
    stars = [sub + V((rr.gauss(0, 0.019), rr.uniform(0.03, 0.55), rr.gauss(0, 0.019) + 0.020))
             for _ in range(240)]
    points_mesh(C, C + "_stars", stars, size=0.0012, mat='white_e', sub=1)
    cam = cam_at(C, sub, c, s, az=0, el=7, fill=1.04, lens=100, dof_k=None, fstop=2.4)
    return _hold(cam, sub, c, fa, fb)


MONTAGE = [_m_glass, _m_needle, _m_hand, _m_scope, _m_blink,
           _m_tape, _m_city, _m_drop, _m_open, _m_hole]


def s19(off, C, f0, f1):
    out = []
    n = len(MONTAGE)
    span = (f1 - f0 + 1) / float(n)
    for i, fn in enumerate(MONTAGE):
        sub = off + V((i * 4.0, 0, 0))
        fa = int(f0 + i * span)
        fb = int(f0 + (i + 1) * span) - 1
        out.append((fn(sub, C + "_m%d" % i, fa, fb), fa, fb))
    return out


SHOTS = [
    ("17", 27.32, 30.00, s17),
    ("18", 30.00, 32.14, s18),
    ("19", 32.14, 36.43, s19),
]
