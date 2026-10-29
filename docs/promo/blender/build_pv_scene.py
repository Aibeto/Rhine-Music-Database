"""重建：真 glb 档案墙 + 21 镜机位 + 程序动作关键帧，存成单一 .blend。"""
import bpy, math, os, numpy as np
from mathutils import Vector

BASE = "/home/aibeto/code/Rhine-Music-Database"
GLB = BASE + "/Assets/models/archive-cassette.glb"
OUT = BASE + "/docs/promo/blender/RHINE-pv-scene.blend"

def wipe():
    for c in list(bpy.data.collections):
        bpy.data.collections.remove(c)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)

try:
    bpy.context.preferences.edit.keyframe_new_interpolation_type = "LINEAR"
except Exception:
    pass
wipe()
bpy.ops.import_scene.gltf(filepath=GLB)
objs = [o for o in bpy.context.selected_objects if o.type == "MESH"]
bpy.context.view_layer.objects.active = objs[0]
for o in objs:
    o.select_set(True)
bpy.ops.object.join()
tpl = bpy.context.active_object
tpl.name = "CARD_TEMPLATE"

# --- 把变换烘进顶点：目标 5.0104(宽X) × 0.3764(厚Y) × 3.7(高Z)，居中于原点、底面 z=0 ---
me = tpl.data
co = np.array([v.co[:] for v in me.vertices], dtype=np.float64)
dims = co.max(0) - co.min(0)
order = list(np.argsort(dims))       # thin, mid, wide
thin, mid, wide = order[0], order[1], order[2]
out = np.zeros_like(co)
out[:, 0] = co[:, wide] * (5.0104 / dims[wide])
out[:, 1] = co[:, thin] * (0.3764 / dims[thin])
out[:, 2] = co[:, mid] * (3.7 / dims[mid])
out[:, 0] -= (out[:, 0].max() + out[:, 0].min()) / 2
out[:, 1] -= (out[:, 1].max() + out[:, 1].min()) / 2
out[:, 2] -= out[:, 2].min()
for i, v in enumerate(me.vertices):
    v.co = out[i].tolist()
me.update()
tpl.location = (0, 0, 0)
tpl.rotation_euler = (0, 0, 0)
tpl.scale = (1, 1, 1)
print("TPL dims", [round(float(x), 3) for x in (out.max(0) - out.min(0))])

LANES, ROWS, PITCH, LANEP = 5, 40, 0.62, 5.2
col = bpy.data.collections.new("ARCHIVE")
bpy.context.scene.collection.children.link(col)
cards = {}
for i in range(LANES):
    for k in range(ROWS):
        o = bpy.data.objects.new("C_%d_%02d" % (i, k), me)
        o.location = ((i - 2) * LANEP, (k - 19.5) * PITCH, 0.0)
        col.objects.link(o)
        cards[(i, k)] = o
tpl.hide_render = True
tpl.hide_viewport = True
print("CARDS", len(cards))

sc = bpy.context.scene
sc.render.engine = "BLENDER_EEVEE"
sc.render.resolution_x, sc.render.resolution_y = 1280, 720
sc.render.fps = 12
sc.frame_start, sc.frame_end = 1, 849
try:
    sc.view_settings.view_transform = "AgX"
except Exception:
    pass
for ob in list(bpy.data.objects):
    ob.select_set(False)

# 世界 + 灯
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

# 机位：墙外视点（plates.py 验证过的距离），21 镜各有起止位置
SHOTS = [
    # (x0,-D0,h0,  x1,-D1,h1,  aim_y, f0, f1)
    (  4.0,24, 3.2,   3.2,20, 2.9,  0.0,   0,  38),
    ( -9.0,19, 2.0,  -7.4,15, 1.8,  0.0,  38,  68),
    (  0.0,22, 0.9,   0.0,18, 1.1,  0.0,  68,  92),
    (  6.0,26, 4.6,   5.0,21, 3.9,  0.0,  92, 116),
    (-12.0,20, 1.6, -10.0,16, 1.4,  0.0, 116, 150),
    (  2.0,17, 1.4,   2.0,13, 1.4,  0.0, 150, 180),
    ( 10.0,23, 2.8,   8.4,18, 2.4,  0.0, 180, 204),
    ( -6.0,18, 3.8,  -5.0,14, 3.2,  0.0, 204, 234),
    (  0.0,26, 5.0,   0.0,20, 4.0,  0.0, 234, 258),
    (  3.0,15, 1.2,   3.0,11, 1.3,  0.0, 258, 280),
    ( -4.0,21, 1.9,  -3.3,16, 1.8,  0.0, 280, 314),
    (  8.0,23, 2.2,   6.4,18, 2.0,  0.0, 314, 337),
    (  0.0,12, 1.5,   0.0, 9, 1.6,  0.0, 337, 345),
    ( -2.0,20, 2.6,  -2.0,15, 2.3,  0.0, 345, 365),
    (  0.0,27, 6.0,   0.0,21, 4.8,  0.0, 365, 379),
    ( 14.0,24, 3.4,  11.6,18, 2.8,  0.0, 379, 406),
    ( -9.0,18, 2.4,  -7.0,13, 2.0,  0.0, 406, 435),
    (  5.0,16, 3.2,   4.0,12, 2.6,  0.0, 435, 462),
    (  0.0,30, 7.0,   0.0,23, 5.6,  0.0, 462, 492),
    ( 11.0,22, 1.4,   9.0,16, 1.3,  0.0, 492, 529),
    (  9.0,25, 3.0,   5.0,19, 2.4,  0.0, 529, 849),
]


def make_cam(name, x0, d0, h0, x1, d1, h1, ay, f0, f1):
    c = bpy.data.cameras.new(name)
    c.lens = 18 / math.tan(math.radians(21.85) / 2)
    c.shift_x, c.shift_y = 0.16, 0.20
    c.clip_start, c.clip_end = 0.05, 600
    o = bpy.data.objects.new(name, c)
    sc.collection.objects.link(o)
    for fr, (x, d, h) in ((f0 + 1, (x0, -d0, h0)), (f1, (x1, -d1, h1))):
        o.location = Vector((x, d, h))
        aim = Vector((x * 0.25, ay, 1.75))
        o.rotation_euler = (aim - o.location).to_track_quat("-Z", "Y").to_euler()
        o.keyframe_insert("location", frame=fr)
        o.keyframe_insert("rotation_euler", frame=fr)
    return o


for idx, (x0, d0, h0, x1, d1, h1, ay, f0, f1) in enumerate(SHOTS):
    cam = make_cam("CAM_%02d" % (idx + 1), x0, d0, h0, x1, d1, h1, ay, f0, f1)
    mk = sc.timeline_markers.new("SHOT_%02d" % (idx + 1), frame=f0 + 1)
    mk.camera = cam
sc.camera = bpy.data.objects["CAM_01"]

# 程序动作：抽出 / 悬停 / 归位（单卡），涟漪 + 扫描（整墙）
def key_y(ob, frame, val):
    ob.location.y = val
    ob.keyframe_insert("location", index=1, frame=frame)

tgt = cards[(2, 12)]
# a_extract 38..68 (30f)
for f in range(30):
    u = f / 29.0
    sm = lambda a, b: max(0.0, min(1.0, (u - a) / (b - a))) ** 2 * (3 - 2 * max(0.0, min(1.0, (u - a) / (b - a))))
    key_y(tgt, 39 + f, -(0.4 * sm(0, 0.30) + 2.95 * sm(0.30, 0.75) + 0.7 * sm(0.75, 1.0)))
# a_hover 92..116 (22f)
for f in range(22):
    t = f / 12.0 * 1.8
    key_y(tgt, 93 + f, -0.28 * (1 - math.exp(-14 * t)))
# a_return 258..280 (22f)
for f in range(22):
    t = f / 12.0 * 1.1
    key_y(tgt, 259 + f, -3.3 * math.exp(-7 * t))
# a_ripple 204..234
for f in range(30):
    t = f / 12.0
    for (i, k), ob in cards.items():
        d = math.hypot((i - 2) * LANEP, (k - 12) * PITCH)
        age = t - d / 8.0
        if age > 0:
            key_y(ob, 205 + f, -0.55 * math.cos((age % 3.2) / 3.2 * 2 * math.pi) * math.exp(-1.15 * age))
# a_scan 150..180
for f in range(30):
    head = 3 + (f / 12.0) * 19.0
    for (i, k), ob in cards.items():
        g = max(0.0, 1 - abs(k - head) / 1.6)
        g2 = max(0.0, 1 - abs(k - (head - 6)) / 1.6)
        key_y(ob, 151 + f, -0.42 * (g - 0.58 * g2))
print("KEYS ok")


os.makedirs("/tmp/verify", exist_ok=True)
sc.frame_set(1)
bpy.ops.wm.save_as_mainfile(filepath=OUT)
print("SAVED", OUT)

sc.render.resolution_x, sc.render.resolution_y = 640, 360
try:
    sc.eevee.taa_render_samples = 8
except Exception:
    pass
for fr in (20, 160, 300, 500):
    sc.frame_set(fr)
    sc.render.filepath = "/tmp/verify/v_%04d.png" % fr
    bpy.ops.render.render(write_still=True)
print("VERIFY done")
