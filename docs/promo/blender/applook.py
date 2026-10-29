"""applook.py — 把「程序内的 3D 效果」搬进 Blender，并按分镜需要调参。

依据：`Qt/shaders/enhancement/opticalSurface.frag`

    const vec3 KEY_LIGHT = normalize(vec3(0.0, 0.55, 0.84));
    rim   = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.30;
    spec  = pow(max(dot(n, h), 0.0), P);
    color = tint * (0.12 + 0.30 * ndl) + vec3(spec + rim);

程序里没有灯：固定主光、0.12 环境、0.30 漫反射上限、一圈 0.30 菲涅尔、一道高光。
分镜里要把系数放大一些（否则 5cm 的倒角在 720p 下全是平的），但形状公式照抄，
所以模型在分镜里的"长相"和程序里是同一族。

    import applook
    applook.shade(mat, tint=(...), diff=1.8, spec=0.9, rim=0.55, power=42)
    applook.flatify_collection(objs, ...)   # 导入的 glb 批量换皮
"""
import bpy
from mathutils import Vector

KEY_LIGHT = Vector((0.0, 0.84, 0.54)).normalized()   # gl 的 (0,0.55,0.84) → Z-up
GROUP_NAME = "APPLOOK2"


def group():
    if GROUP_NAME in bpy.data.node_groups:
        return bpy.data.node_groups[GROUP_NAME]

    ng = bpy.data.node_groups.new(GROUP_NAME, 'ShaderNodeTree')
    for nm, st in (("Tint", 'NodeSocketColor'), ("Diff", 'NodeSocketFloat'),
                   ("Spec", 'NodeSocketFloat'), ("Rim", 'NodeSocketFloat'),
                   ("Power", 'NodeSocketFloat'), ("Amb", 'NodeSocketFloat')):
        ng.interface.new_socket(nm, in_out='INPUT', socket_type=st)
    ng.interface.new_socket("Emission", in_out='OUTPUT', socket_type='NodeSocketShader')

    n, L = ng.nodes, ng.links
    gi = n.new('NodeGroupInput')
    go = n.new('NodeGroupOutput')
    geo = n.new('ShaderNodeNewGeometry')

    # ndl
    d1 = n.new('ShaderNodeVectorMath'); d1.operation = 'DOT_PRODUCT'
    d1.inputs[1].default_value = KEY_LIGHT
    L.new(geo.outputs['Normal'], d1.inputs[0])
    c1 = n.new('ShaderNodeMath'); c1.operation = 'MAXIMUM'; c1.inputs[1].default_value = 0.0
    L.new(d1.outputs['Value'], c1.inputs[0])

    # diffuse = amb + diff*ndl
    m1 = n.new('ShaderNodeMath'); m1.operation = 'MULTIPLY'; m1.name = "ndl"
    L.new(c1.outputs['Value'], m1.inputs[0])
    L.new(gi.outputs['Diff'], m1.inputs[1])
    a1 = n.new('ShaderNodeMath'); a1.operation = 'ADD'
    L.new(m1.outputs['Value'], a1.inputs[0])
    L.new(gi.outputs['Amb'], a1.inputs[1])

    # half vector for spec: H = normalize(L + V); use N·H ≈ N·L + N·V 近似
    dv = n.new('ShaderNodeVectorMath'); dv.operation = 'DOT_PRODUCT'
    L.new(geo.outputs['Normal'], dv.inputs[0])
    L.new(geo.outputs['Incoming'], dv.inputs[1])
    hsum = n.new('ShaderNodeMath'); hsum.operation = 'ADD'
    L.new(c1.outputs['Value'], hsum.inputs[0])
    aabs = n.new('ShaderNodeMath'); aabs.operation = 'ABSOLUTE'
    L.new(dv.outputs['Value'], aabs.inputs[0])
    L.new(aabs.outputs['Value'], hsum.inputs[1])
    half = n.new('ShaderNodeMath'); half.operation = 'MULTIPLY'; half.inputs[1].default_value = 0.5
    L.new(hsum.outputs['Value'], half.inputs[0])
    sp = n.new('ShaderNodeMath'); sp.operation = 'POWER'
    L.new(half.outputs['Value'], sp.inputs[0])
    L.new(gi.outputs['Power'], sp.inputs[1])
    spm = n.new('ShaderNodeMath'); spm.operation = 'MULTIPLY'
    L.new(sp.outputs['Value'], spm.inputs[0])
    L.new(gi.outputs['Spec'], spm.inputs[1])

    # rim = pow(1-|N·V|, 3) * rim
    one = n.new('ShaderNodeMath'); one.operation = 'SUBTRACT'; one.inputs[0].default_value = 1.0
    L.new(aabs.outputs['Value'], one.inputs[1])
    pw = n.new('ShaderNodeMath'); pw.operation = 'POWER'; pw.inputs[1].default_value = 3.0
    L.new(one.outputs['Value'], pw.inputs[0])
    rm = n.new('ShaderNodeMath'); rm.operation = 'MULTIPLY'
    L.new(pw.outputs['Value'], rm.inputs[0])
    L.new(gi.outputs['Rim'], rm.inputs[1])

    # light = diffuse + spec + rim
    s1 = n.new('ShaderNodeMath'); s1.operation = 'ADD'
    L.new(a1.outputs['Value'], s1.inputs[0])
    L.new(spm.outputs['Value'], s1.inputs[1])
    s2 = n.new('ShaderNodeMath'); s2.operation = 'ADD'
    L.new(s1.outputs['Value'], s2.inputs[0])
    L.new(rm.outputs['Value'], s2.inputs[1])

    # tint * light
    sc = n.new('ShaderNodeVectorMath'); sc.operation = 'SCALE'
    L.new(gi.outputs['Tint'], sc.inputs[0])
    L.new(s2.outputs['Value'], sc.inputs['Scale'])

    em = n.new('ShaderNodeEmission')
    L.new(sc.outputs['Vector'], em.inputs['Color'])
    em.inputs['Strength'].default_value = 1.0
    L.new(em.outputs['Emission'], go.inputs['Emission'])
    return ng


def shade(mat, tint=None, diff=1.8, spec=0.9, rim=0.55, power=42.0, amb=0.12):
    ng = group()
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    g = nt.nodes.new('ShaderNodeGroup')
    g.node_tree = ng
    if tint is not None:
        g.inputs['Tint'].default_value = (tint[0], tint[1], tint[2], 1.0)
    g.inputs['Diff'].default_value = diff
    g.inputs['Spec'].default_value = spec
    g.inputs['Rim'].default_value = rim
    g.inputs['Power'].default_value = power
    g.inputs['Amb'].default_value = amb
    nt.links.new(g.outputs['Emission'], out.inputs['Surface'])
    mat.surface_render_method = 'DITHERED'
    return mat


def flatify_collection(objs, boost=1.0, tint_override=None, **kw):
    seen = {}
    for ob in objs:
        if ob.type != 'MESH':
            continue
        for slot in ob.material_slots:
            m = slot.material
            if not m:
                continue
            if m.name not in seen:
                tint = tint_override
                if tint is None:
                    b = next((x for x in m.node_tree.nodes if x.type == 'BSDF_PRINCIPLED'), None)
                    if b is not None:
                        c = b.inputs['Base Color'].default_value
                        tint = (c[0] * boost, c[1] * boost, c[2] * boost)
                    else:
                        tint = (0.8, 0.8, 0.78)
                seen[m.name] = shade(m, tint=tint, **kw)
            slot.material = seen[m.name]
    return objs
