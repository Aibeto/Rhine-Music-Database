import bpy, math, os
from mathutils import Vector
sc = bpy.context.scene
sc.render.resolution_x, sc.render.resolution_y = 480, 270
try: sc.eevee.taa_render_samples = 6
except Exception: pass
CAND = [
 ("A", (28,0,2.2), (0,0,1.7)),
 ("B", (30,8,4.0), (0,0,1.4)),
 ("C", (26,-6,1.2), (0,0,1.9)),
 ("D", (34,4,6.0), (0,0,1.0)),
 ("E", (24,10,2.6), (0,2,1.6)),
 ("F", (40,0,3.0), (0,0,1.5)),
]
os.makedirs("/tmp/probe", exist_ok=True)
sc.timeline_markers.clear()
for name, eye, aim in CAND:
    c = bpy.data.cameras.new("P_"+name)
    c.lens = 18/math.tan(math.radians(21.85)/2)
    c.shift_x, c.shift_y = 0.16, 0.20
    c.clip_start, c.clip_end = 0.05, 400
    o = bpy.data.objects.new("P_"+name, c)
    sc.collection.objects.link(o)
    o.location = Vector(eye)
    o.rotation_euler = (Vector(aim)-Vector(eye)).to_track_quat("-Z","Y").to_euler()
    sc.camera = o
    sc.frame_set(1)
    sc.render.filepath = "/tmp/probe/%s.png" % name
    bpy.ops.render.render(write_still=True)
print("PROBE ok")
