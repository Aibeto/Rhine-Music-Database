import bpy, os
sc = bpy.context.scene
sc.render.resolution_x, sc.render.resolution_y = 480, 270
try: sc.eevee.taa_render_samples = 6
except Exception: pass
os.makedirs("/tmp/strip", exist_ok=True)
mk = sorted(sc.timeline_markers, key=lambda m: m.frame)
for i, m in enumerate(mk):
    mid = int((m.frame + (mk[i+1].frame-1 if i+1 < len(mk) else sc.frame_end))) // 2
    sc.frame_set(mid)
    sc.render.filepath = "/tmp/strip/s_%02d.png" % (i+1)
    bpy.ops.render.render(write_still=True)
print("STRIP", len(mk))
