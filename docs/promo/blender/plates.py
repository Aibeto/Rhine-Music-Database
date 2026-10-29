"""生成 M2/M3 的 3D 底板（真实 glb 卡匣 + 程序几何），透明背景，供 film.py 合成。"""
import bpy, math, os, sys
import numpy as np
from mathutils import Vector

BASE = "/home/aibeto/code/Rhine-Music-Database"
GLB = BASE + "/Assets/models/archive-cassette.glb"
OUT = "/tmp/plates"
os.makedirs(OUT, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=GLB)
objs = [o for o in sc.objects if o.type == 'MESH']
bpy.ops.object.select_all(action='DESELECT')
for o in objs: o.select_set(True)
bpy.context.view_layer.objects.active = objs[0]
if len(objs) > 1: bpy.ops.object.join()
tpl = bpy.context.view_layer.objects.active
me = tpl.data

co = np.array([v.co[:] for v in me.vertices], dtype=np.float64)
dims = co.max(0) - co.min(0)
order = list(np.argsort(dims))
thin, mid, wide = order[0], order[1], order[2]
out = np.zeros_like(co)
out[:, 0] = co[:, wide] * (5.0104 / dims[wide])
out[:, 1] = co[:, thin] * (0.3764 / dims[thin])
out[:, 2] = co[:, mid] * (3.7 / dims[mid])
out[:, 0] -= (out[:, 0].max() + out[:, 0].min()) / 2
out[:, 1] -= (out[:, 1].max() + out[:, 1].min()) / 2
out[:, 2] -= out[:, 2].min()
for i in range(len(me.vertices)): me.vertices[i].co = out[i].tolist()
me.update()

LANES, ROWS, PITCH, LANEP = 5, 40, 0.62, 5.2
cards = []
for lane in range(LANES):
    for row in range(ROWS):
        o = bpy.data.objects.new("C_%d_%d" % (lane, row), me)
        o.location = ((lane - 2) * LANEP, (row - 19.5) * PITCH, 0.0)
        sc.collection.objects.link(o)
        cards.append(o)
print("CARDS", len(cards), flush=True)

# 光照：程序 KEY_LIGHT(0,0.55,0.84) + 55% 环境
sun_d = bpy.data.lights.new("KEY", 'SUN'); sun_d.energy = 2.2; sun_d.angle = math.radians(10)
sun = bpy.data.objects.new("KEY", sun_d); sc.collection.objects.link(sun)
k = Vector((0.0, 0.86, 0.52)).normalized()
sun.rotation_euler = (-k).to_track_quat('-Z', 'Y').to_euler()
w = bpy.data.worlds.new("W"); sc.world = w; w.use_nodes = True
w.node_tree.nodes["Background"].inputs[0].default_value = (0.5, 0.5, 0.49, 1)
w.node_tree.nodes["Background"].inputs[1].default_value = 0.45

RES_W, RES_H = 1920, 1080
sc.render.engine = 'BLENDER_EEVEE'
sc.eevee.taa_render_samples = 24
sc.render.resolution_x, sc.render.resolution_y = RES_W, RES_H
sc.render.film_transparent = True
sc.view_settings.view_transform = 'Standard'
sc.render.image_settings.file_format = 'PNG'
sc.render.image_settings.color_mode = 'RGBA'

cam_d = bpy.data.cameras.new("CAM"); cam_d.sensor_width = 36
cam = bpy.data.objects.new("CAM", cam_d); sc.collection.objects.link(cam); sc.camera = cam

def vfov_lens(deg):
    return 18.0 / math.tan(math.radians(deg / 2))

def shot(name, eye, aim, fov=21.85, sx=0.16, sy=0.20):
    cam_d.lens = vfov_lens(fov)
    cam_d.shift_x, cam_d.shift_y = sx, sy
    cam.location = Vector(eye)
    d = Vector(aim) - Vector(eye)
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = "%s/%s.png" % (OUT, name)
    bpy.ops.render.render(write_still=True)
    print("PLATE", name, flush=True)

# P1 走廊全景（浏览视）
shot("p1_corridor", (12.5, -26.0, 5.6), (-1.0, 1.0, 1.3), 21.85)
# P2 低机位掠射（叠压/板缝）
shot("p2_rake", (9.0, -18.0, 1.5), (-1.5, 3.0, 1.5), 21.85)
# P3 单片正面特写（11 部件）
shot("p3_card", (0.35, 6.4, 1.95), (0.0, -0.2, 1.85), 21.85, 0.0, 0.0)
# P4 竖排板缝（俯视）
shot("p4_top", (2.0, -16.0, 11.0), (0.0, 2.0, 1.0), 28.0, 0.0, 0.0)
# P5 单车道纵深（扫描波用）
shot("p5_lane", (0.2, -30.0, 3.4), (0.0, 8.0, 1.7), 16.0, 0.0, 0.10)
# P6 远距全灭（成点）
shot("p6_far", (34.0, -70.0, 16.0), (-1.0, -4.0, 1.4), 21.85, 0.0, 0.06)
# P7 背光侧影（暗舱）
shot("p7_dark", (16.0, -30.0, 4.0), (-1.0, 4.0, 1.5), 16.0, 0.0, 0.08)
print("DONE", flush=True)
