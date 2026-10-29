"""程序动作镜头：用真实参数动画化档案墙，逐帧渲透明底板。"""
import bpy, math, os, sys
from mathutils import Vector

BASE = "/home/aibeto/code/Rhine-Music-Database"
GLB = BASE + "/Assets/models/archive-cassette.glb"
OUT = "/tmp/acts"
FPS = 12
LANEP, PITCH = 5.2, 0.62
CARD = (5.0104, 0.3764, 3.7000)  # x=宽 y=厚 z=高


def clean():
    for c in list(bpy.data.collections):
        bpy.data.collections.remove(c)
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def import_card():
    bpy.ops.import_scene.gltf(filepath=GLB)
    obs = [o for o in bpy.context.selected_objects if o.type == "MESH"]
    bpy.context.view_layer.objects.active = obs[0]
    for o in obs:
        o.select_set(True)
    bpy.ops.object.join()
    ob = bpy.context.active_object
    bb = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    mn = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
    mx = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    s = Vector((CARD[0] / max(1e-6, mx.x - mn.x), CARD[1] / max(1e-6, mx.y - mn.y), CARD[2] / max(1e-6, mx.z - mn.z)))
    ob.scale = s
    bpy.context.view_layer.update()
    bb = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    ctr = sum(bb, Vector()) / 8.0
    ob.location -= ctr
    bpy.context.view_layer.update()
    return ob


def wall(src, lanes=5, rows=24):
    col = bpy.data.collections.new("WALL")
    bpy.context.scene.collection.children.link(col)
    cards = []
    for i in range(lanes):
        for k in range(rows):
            ob = src.copy()
            ob.data = src.data
            col.objects.link(ob)
            ob.location = ((i - 2) * LANEP, (k - rows / 2) * PITCH, 0.0)
            cards.append((i, k, ob))
    src.hide_render = True
    return cards


def setup(W=960, H=540, samples=12):
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x, sc.render.resolution_y = W, H
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    try:
        sc.eevee.taa_render_samples = samples
    except Exception:
        pass
    sc.view_settings.view_transform = "Standard"
    w = bpy.data.worlds.new("W")
    sc.world = w
    w.use_nodes = True
    bg = w.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (1, 1, 1, 1)
    bg.inputs[1].default_value = 0.55
    l = bpy.data.lights.new("SUN", "SUN")
    l.energy = 2.0
    lo = bpy.data.objects.new("SUN", l)
    sc.collection.objects.link(lo)
    lo.rotation_euler = (math.radians(62), 0, math.radians(28))
    cam = bpy.data.cameras.new("C")
    cam.lens = 18 / math.tan(math.radians(21.85) / 2)
    cam.shift_x, cam.shift_y = 0.16, 0.20
    co = bpy.data.objects.new("C", cam)
    sc.collection.objects.link(co)
    sc.camera = co
    return sc, co


def look(cam, eye, aim):
    cam.location = Vector(eye)
    d = Vector(aim) - Vector(eye)
    rot = d.to_track_quat("-Z", "Y").to_euler()
    cam.rotation_euler = rot


def render(sc, name, n):
    os.makedirs(OUT + "/" + name, exist_ok=True)
    for f in range(n):
        sc.frame_set(f + 1)
        sc.render.filepath = "%s/%s/f_%03d" % (OUT, name, f)
        bpy.ops.render.render(write_still=True)


def main():
    clean()
    src = import_card()
    cards = wall(src, 5, 24)
    sc, cam = setup()

    for f in range(1, 121):
        sc.frame_set(f)

    # ---- 1. extract：卡片沿 -Y 抽出（extraction 分段）----
    tgt = [c for c in cards if c[0] == 2 and c[1] == 12][0][2]
    N = 30
    for f in range(N):
        u = f / (N - 1.0)
        sm = lambda a, b: max(0.0, min(1.0, (u - a) / (b - a))) ** 2 * (3 - 2 * max(0.0, min(1.0, (u - a) / (b - a))))
        tgt.location.y = -(0.4 * sm(0.00, 0.30) + 2.95 * sm(0.30, 0.75) + 0.7 * sm(0.75, 1.0))
        look(cam, (21.0, -54.0, 8.0), (3.0, -1.5, 1.6))
        sc.frame_set(f + 1)
        sc.render.filepath = "%s/a_extract/f_%03d" % (OUT, f)
        os.makedirs(OUT + "/a_extract", exist_ok=True)
        bpy.ops.render.render(write_still=True)
    tgt.location.y = 0

    # ---- 2. hover：悬停抬起 0.28，速率 1-exp(-14t) ----
    N = 22
    for f in range(N):
        u = f / (N - 1.0) * 1.8
        tgt.location.y = -0.28 * (1 - math.exp(-14 * u))
        look(cam, (16.0, -48.0, 7.0), (3.0, -1.0, 1.5))
        sc.frame_set(f + 1)
        os.makedirs(OUT + "/a_hover", exist_ok=True)
        sc.render.filepath = "%s/a_hover/f_%03d" % (OUT, f)
        bpy.ops.render.render(write_still=True)

    # ---- 3. return：exp(-7t) 归位 ----
    N = 22
    for f in range(N):
        u = f / (N - 1.0) * 1.1
        tgt.location.y = -3.3 * math.exp(-7 * u)
        look(cam, (21.0, -54.0, 8.0), (3.0, -1.5, 1.6))
        sc.frame_set(f + 1)
        os.makedirs(OUT + "/a_return", exist_ok=True)
        sc.render.filepath = "%s/a_return/f_%03d" % (OUT, f)
        bpy.ops.render.render(write_still=True)
    tgt.location.y = 0

    # ---- 4. ripple：波速 8/s，衰减 exp(-1.15t)，周期 3.2 ----
    N = 30
    for f in range(N):
        t = f / FPS
        for i, k, ob in cards:
            d = math.hypot((i - 2) * LANEP, (k - 12) * PITCH)
            age = t - d / 8.0
            if age <= 0:
                ob.location.y = 0
                continue
            ph = (age % 3.2) / 3.2 * 2 * math.pi
            ob.location.y = -0.55 * math.cos(ph) * math.exp(-1.15 * age)
        look(cam, (26.0, -58.0, 9.0), (0.0, -1.0, 1.4))
        sc.frame_set(f + 1)
        os.makedirs(OUT + "/a_ripple", exist_ok=True)
        sc.render.filepath = "%s/a_ripple/f_%03d" % (OUT, f)
        bpy.ops.render.render(write_still=True)

    # ---- 5. scan：row 3→32，19 行/s，后 6 行带 -0.58 负瓣 ----
    N = 30
    for f in range(N):
        t = f / FPS
        head = 3 + t * 19.0
        for i, k, ob in cards:
            g = max(0.0, 1.0 - abs(k - head) / 1.6)
            g2 = max(0.0, 1.0 - abs(k - (head - 6)) / 1.6)
            ob.location.y = -0.42 * (g - 0.58 * g2)
        look(cam, (29.0, -60.0, 8.4), (0.0, 0.0, 1.3))
        sc.frame_set(f + 1)
        os.makedirs(OUT + "/a_scan", exist_ok=True)
        sc.render.filepath = "%s/a_scan/f_%03d" % (OUT, f)
        bpy.ops.render.render(write_still=True)

    print("ACTS_DONE")


if __name__ == "__main__":
    main()
