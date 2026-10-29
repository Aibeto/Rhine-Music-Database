# -*- coding: utf-8 -*-
"""RHINE PV 70s — shared library: palette, materials, primitives, camera moves, render setup."""
import bpy, bmesh, math, random
from mathutils import Vector, Euler, Quaternion

FPS = 12
SENSOR = 36.0

# ---------- palette (docs/promo storyboard §2.1) ----------
PAPER  = (0.796, 0.796, 0.749)   # #E7E7E1  linear-ish approximation
INK    = (0.021, 0.023, 0.020)   # #141613
MOSS   = (0.166, 0.174, 0.116)   # #777B60
MINT   = (0.253, 0.771, 0.474)   # #8FE3BB
ORANGE = (0.784, 0.117, 0.024)   # #E8622A
CABIN  = (0.029, 0.051, 0.070)   # #121A20
BG     = (0.006, 0.010, 0.014)   # #080C10


def srgb(hexstr):
    h = hexstr.lstrip('#')
    c = [int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)]
    return tuple((v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4) for v in c)


# ---------- scene bootstrap ----------
def purge():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for c in list(bpy.data.collections):
        bpy.data.collections.remove(c)
    for m in list(bpy.data.materials):
        bpy.data.materials.remove(m)
    for c in list(bpy.data.cameras):
        bpy.data.cameras.remove(c)
    for l in list(bpy.data.lights):
        bpy.data.lights.remove(l)


def col(name):
    c = bpy.data.collections.get(name)
    if not c:
        c = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(c)
    return c


def link(ob, cname):
    c = col(cname)
    for cc in list(ob.users_collection):
        cc.objects.unlink(ob)
    c.objects.link(ob)
    return ob


# ---------- materials ----------
def _p(mat):
    return mat.node_tree.nodes["Principled BSDF"]


def mat(name, color, rough=0.5, metal=0.0, aniso=0.0, emit=None, estr=0.0, alpha=1.0,
        transmission=0.0, ior=1.45, spec=0.5):
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = _p(m)
    b.inputs["Base Color"].default_value = (*color, 1.0)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    b.inputs["Anisotropic"].default_value = aniso
    b.inputs["IOR"].default_value = ior
    b.inputs["Specular IOR Level"].default_value = spec
    if transmission:
        b.inputs["Transmission Weight"].default_value = transmission
    if emit is not None:
        b.inputs["Emission Color"].default_value = (*emit, 1.0)
        b.inputs["Emission Strength"].default_value = estr
    if alpha < 1.0:
        b.inputs["Alpha"].default_value = alpha
        m.surface_render_method = 'BLENDED'
    return m


def mat_vinyl(name, color=None, grooves=180.0, strength=1.0):
    """Black vinyl: wave-texture concentric grooves + anisotropy. No geometry needed."""
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    tc = nt.nodes.new("ShaderNodeTexCoord")
    wave = nt.nodes.new("ShaderNodeTexWave")
    wave.wave_type = 'RINGS'
    wave.rings_direction = 'SPHERICAL'
    wave.inputs["Scale"].default_value = grooves
    wave.inputs["Distortion"].default_value = 0.6
    wave.inputs["Detail"].default_value = 2.0
    coord = nt.nodes.new("ShaderNodeMapping")
    coord.inputs["Scale"].default_value = (1, 1, 1)
    nt.links.new(tc.outputs["Object"], coord.inputs["Vector"])
    nt.links.new(coord.outputs["Vector"], wave.inputs["Vector"])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.30
    ramp.color_ramp.elements[0].color = (0.008, 0.008, 0.009, 1)
    ramp.color_ramp.elements[1].position = 0.72
    ramp.color_ramp.elements[1].color = (0.055, 0.056, 0.060, 1)
    nt.links.new(wave.outputs["Fac"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
    b.inputs["Roughness"].default_value = 0.28
    b.inputs["Metallic"].default_value = 0.0
    b.inputs["Anisotropic"].default_value = 0.35
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.10 * strength
    nt.links.new(wave.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


def mat_shaft(name, color, strength=2.4, alpha=0.42):
    """Light shaft: emissive stick, alpha fades from the top (Generated Z=1) to the bottom."""
    m = bpy.data.materials.get(name)
    if m:
        return m
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    mix = nt.nodes.new("ShaderNodeMixShader")
    tr = nt.nodes.new("ShaderNodeBsdfTransparent")
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = (*color, 1)
    em.inputs["Strength"].default_value = strength
    tc = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0, 0, 0, 1)
    ramp.color_ramp.elements[1].position = 0.92
    ramp.color_ramp.elements[1].color = (1, 1, 1, 1)
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = 'MULTIPLY'
    mul.inputs[1].default_value = alpha
    nt.links.new(tc.outputs["Generated"], sep.inputs["Vector"])
    nt.links.new(sep.outputs["Z"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], mul.inputs[0])
    nt.links.new(mul.outputs["Value"], mix.inputs["Fac"])
    nt.links.new(tr.outputs["BSDF"], mix.inputs[1])
    nt.links.new(em.outputs["Emission"], mix.inputs[2])
    nt.links.new(mix.outputs["Shader"], out.inputs["Surface"])
    m.surface_render_method = 'BLENDED'
    return m

# ---------- primitives ----------
def prim(kind, cname, name, loc=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1), material=None, **kw):
    if kind == 'cube':
        bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    elif kind == 'cyl':
        bpy.ops.mesh.primitive_cylinder_add(vertices=kw.get('v', 64), radius=kw.get('r', 0.5),
                                            depth=kw.get('d', 0.01), location=loc, rotation=rot)
    elif kind == 'cone':
        bpy.ops.mesh.primitive_cone_add(vertices=kw.get('v', 48), radius1=kw.get('r1', 0.5),
                                        radius2=kw.get('r2', 0.0), depth=kw.get('d', 1.0),
                                        location=loc, rotation=rot)
    elif kind == 'sphere':
        bpy.ops.mesh.primitive_uv_sphere_add(segments=kw.get('seg', 48), ring_count=kw.get('ring', 24),
                                             radius=kw.get('r', 0.5), location=loc, rotation=rot)
    elif kind == 'ico':
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=kw.get('sub', 2), radius=kw.get('r', 0.5),
                                              location=loc, rotation=rot)
    elif kind == 'plane':
        bpy.ops.mesh.primitive_plane_add(size=kw.get('size', 1.0), location=loc, rotation=rot)
    elif kind == 'torus':
        bpy.ops.mesh.primitive_torus_add(location=loc, rotation=rot,
                                         major_radius=kw.get('R', 0.5), minor_radius=kw.get('r', 0.02),
                                         major_segments=kw.get('mv', 96), minor_segments=kw.get('nv', 8))
    elif kind == 'grid':
        bpy.ops.mesh.primitive_grid_add(x_subdivisions=kw.get('nx', 20), y_subdivisions=kw.get('ny', 20),
                                        size=kw.get('size', 1.0), location=loc, rotation=rot)
    else:
        raise ValueError(kind)
    ob = bpy.context.object
    ob.name = name
    ob.scale = scale
    if material:
        ob.data.materials.append(material)
    link(ob, cname)
    return ob


def smooth(ob):
    for p in ob.data.polygons:
        p.use_smooth = True
    return ob


def bevel(ob, w=0.004, seg=2, angle=0.7):
    m = ob.modifiers.new("bv", 'BEVEL')
    m.width = w
    m.segments = seg
    m.limit_method = 'ANGLE'
    m.angle_limit = angle
    return ob


# ---------- camera ----------
def look_rot(loc, target, roll=0.0):
    d = (Vector(target) - Vector(loc))
    if d.length < 1e-9:
        d = Vector((0, 1, 0))
    q = d.to_track_quat('-Z', 'Y')
    e = q.to_euler()
    if roll:
        e.rotate_axis('Z', roll)
    return e


def cam(name, cname, loc, target, lens=50.0, dof=None, fstop=2.8, roll=0.0, ortho=None):
    cd = bpy.data.cameras.new(name)
    cd.lens = lens
    cd.sensor_width = SENSOR
    cd.sensor_fit = 'HORIZONTAL'
    if ortho:
        cd.type = 'ORTHO'
        cd.ortho_scale = ortho
    if dof is not None:
        cd.dof.use_dof = True
        cd.dof.focus_distance = dof
        cd.dof.aperture_fstop = fstop
    ob = bpy.data.objects.new(name, cd)
    ob.location = loc
    ob.rotation_euler = look_rot(loc, target, roll)
    col(cname).objects.link(ob)
    return ob


def key_cam(c, frame, loc=None, target=None, roll=None):
    if loc is not None:
        c.location = loc
        c.keyframe_insert("location", frame=frame)
    if target is not None:
        r = look_rot(c.location, target, roll or 0.0)
        c.rotation_euler = r
        c.keyframe_insert("rotation_euler", frame=frame)
    elif roll is not None:
        r = look_rot(c.location, Vector(c.location) + (c.matrix_world.to_quaternion() @ Vector((0, 0, -1))), roll)
        c.rotation_euler = r
        c.keyframe_insert("rotation_euler", frame=frame)


def key(obj, path, frame, value=None):
    if value is not None:
        setattr(obj, path, value)
    obj.keyframe_insert(path, frame=frame)


def interp(c, kind='BEZIER', easing='AUTO'):
    ad = c.animation_data
    if not ad or not ad.action:
        return
    for fc in ad.action.fcurves if hasattr(ad.action, 'fcurves') else []:
        for kp in fc.keyframe_points:
            kp.interpolation = kind
            kp.easing = easing


# ---------- lights ----------
def area(name, cname, loc, target, size=1.0, power=200.0, color=(1, 1, 1), shape='RECTANGLE', size_y=None):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.energy = power
    ld.color = color
    ld.shape = shape
    ld.size = size
    if size_y:
        ld.size_y = size_y
    ob = bpy.data.objects.new(name, ld)
    ob.location = loc
    ob.rotation_euler = look_rot(loc, target)
    col(cname).objects.link(ob)
    return ob


def spot(name, cname, loc, target, size=30.0, power=500.0, color=(1, 1, 1), blend=0.3):
    ld = bpy.data.lights.new(name, 'SPOT')
    ld.energy = power
    ld.color = color
    ld.spot_size = math.radians(size)
    ld.spot_blend = blend
    ob = bpy.data.objects.new(name, ld)
    ob.location = loc
    ob.rotation_euler = look_rot(loc, target)
    col(cname).objects.link(ob)
    return ob


# ---------- world & render ----------
def world(bg=BG, strength=1.0):
    w = bpy.data.worlds.get("W") or bpy.data.worlds.new("W")
    w.use_nodes = True
    b = w.node_tree.nodes["Background"]
    b.inputs["Color"].default_value = (*bg, 1)
    b.inputs["Strength"].default_value = strength
    bpy.context.scene.world = w
    return w


def render_setup(res=(1280, 720), samples=16, fps=FPS, frame_end=849, engine='BLENDER_EEVEE'):
    sc = bpy.context.scene
    sc.render.engine = engine
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.resolution_percentage = 100
    sc.render.fps = fps
    sc.frame_start = 1
    sc.frame_end = frame_end
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGB'
    sc.render.film_transparent = False
    try:
        sc.view_settings.view_transform = 'Standard'
    except Exception:
        pass
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    if hasattr(sc, 'eevee'):
        sc.eevee.taa_render_samples = samples
        try:
            sc.eevee.use_raytracing = False
        except Exception:
            pass
    if engine == 'CYCLES':
        sc.cycles.samples = samples
        sc.cycles.use_denoising = True
    return sc


def compositor_bloom(threshold=1.0, size=8.0, strength=0.5):
    sc = bpy.context.scene
    ng = bpy.data.node_groups.get("Comp") or bpy.data.node_groups.new("Comp", "CompositorNodeTree")
    sc.compositing_node_group = ng
    ng.nodes.clear()
    ng.interface.new_socket(name="Image", in_out='OUTPUT', socket_type='NodeSocketColor')
    rl = ng.nodes.new('CompositorNodeRLayers')
    gl = ng.nodes.new('CompositorNodeGlare')
    gl.inputs['Type'].default_value = 'Fog Glow'
    gl.inputs['Quality'].default_value = 'High'
    gl.inputs['Threshold'].default_value = threshold
    gl.inputs['Size'].default_value = size
    gl.inputs['Strength'].default_value = strength
    out = ng.nodes.new('NodeGroupOutput')
    ng.links.new(rl.outputs['Image'], gl.inputs['Image'])
    ng.links.new(gl.outputs['Image'], out.inputs['Image'])
    sc.render.use_compositing = True
    return ng
