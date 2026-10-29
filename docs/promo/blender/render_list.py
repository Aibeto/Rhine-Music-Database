import bpy, os, sys, time
a = sys.argv[sys.argv.index("--")+1:]
outdir = a[0]; samples = int(a[1]); w = int(a[2]); h = int(a[3])
frames = [int(x) for x in a[4:]]
os.makedirs(outdir, exist_ok=True)
sc = bpy.context.scene
sc.render.resolution_x, sc.render.resolution_y = w, h
sc.render.resolution_percentage = 100
sc.render.fps = 12
sc.render.image_settings.file_format = 'PNG'
sc.render.image_settings.color_mode = 'RGB'
try:
    sc.eevee.taa_render_samples = samples
except Exception:
    pass
sc.view_settings.view_transform = 'AgX'
t = time.time()
for f in frames:
    sc.frame_set(f)
    sc.render.filepath = os.path.join(outdir, "f_%04d.png" % f)
    bpy.ops.render.render(write_still=True)
print("LIST_DONE", len(frames), round(time.time()-t, 1))
