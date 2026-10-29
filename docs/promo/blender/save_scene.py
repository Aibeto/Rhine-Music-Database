"""保存可再渲的 Blender 场景（真 glb 档案墙 + 程序机位）。不渲图。"""
import bpy, math, sys
from mathutils import Vector
BASE = "/home/aibeto/code/Rhine-Music-Database"
GLB = BASE + "/Assets/models/archive-cassette.glb"
OUT = BASE + "/docs/promo/blender/rhine-pv-archive.blend"

for c in list(bpy.data.collections):
    bpy.data.collections.remove(c)
for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)

bpy.ops.import_scene.gltf(filepath=GLB)
objs = [o for o in bpy.context.selected_objects if o.type == "MESH"]
bpy.context.view_layer.objects.active = objs[0]
for o in objs:
    o.select_set(True)
bpy.ops.object.join()
tpl = bpy.context.active_object
bb = [tpl.matrix_world @ Vector(c) for c in tpl.bound_box]
mn = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
mx = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
s = Vector((5.0104 / (mx.x - mn.x), 0.3764 / (mx.y - mn.y), 3.7 / (mx.z - mn.z)))
tpl.scale = s
bpy.context.view_layer.update()
bb = [tpl.matrix_world @ Vector(c) for c in tpl.bound_box]
ctr = sum(bb, Vector()) / 8.0
tpl.location -= ctr
tpl.location.z += 1.85
bpy.context.view_layer.update()

LANES, ROWS, PITCH, LANEP = 5, 40, 0.62, 5.2
col = bpy.data.collections.new("ARCHIVE")
bpy.context.scene.collection.children.link(col)
for i in range(LANES):
    for k in range(ROWS):
        o = bpy.data.objects.new("C_%d_%02d" % (i, k), tpl.data)
        o.location = ((i - 2) * LANEP, (k - 19.5) * PITCH, 0.0)
        for m in tpl.data.materials:
            pass
        o.data = tpl.data
        col.objects.link(o)
        for m in tpl.data.materials:
            o.data = tpl.data
tpl.hide_render = True

sc = bpy.context.scene
sc.render.engine = "BLENDER_EEVEE"
sc.render.resolution_x, sc.render.resolution_y = 1280, 720
sc.render.fps = 12
sc.frame_start, sc.frame_end = 1, 849
try:
    sc.view_settings.view_transform = "AgX"
except Exception:
    pass

w = bpy.data.worlds.new("W")
sc.world = w
w.use_nodes = True
w.node_tree.nodes["Background"].inputs[0].default_value = (0.5, 0.5, 0.49, 1)
w.node_tree.nodes["Background"].inputs[1].default_value = 0.45
l = bpy.data.lights.new("KEY", "SUN")
l.energy = 2.2
l.angle = math.radians(10)
lo = bpy.data.objects.new("KEY", l)
sc.collection.objects.link(lo)
lo.rotation_euler = (math.radians(58), 0, math.radians(26))

cam = bpy.data.cameras.new("CAM")
cam.lens = 18 / math.tan(math.radians(21.85) / 2)
cam.shift_x, cam.shift_y = 0.16, 0.20
co = bpy.data.objects.new("CAM", cam)
sc.collection.objects.link(co)
sc.camera = co
co.location = (21.0, -54.0, 8.0)
d = Vector((3.0, -1.5, 1.6)) - co.location
co.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()

sc.frame_set(1)
bpy.ops.wm.save_as_mainfile(filepath=OUT)
print("SAVED", OUT, len(bpy.data.objects))
