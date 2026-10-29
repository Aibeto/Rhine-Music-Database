import bpy, os, sys, time

argv = sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else []
start = int(argv[0]); end = int(argv[1]); outdir = argv[2]; samples = int(argv[3]) if len(argv) > 3 else 8
os.makedirs(outdir, exist_ok=True)
sc = bpy.context.scene
sc.render.resolution_x, sc.render.resolution_y = 1280, 720
sc.render.resolution_percentage = 100
sc.render.fps = 12
sc.render.image_settings.file_format = 'PNG'
sc.render.image_settings.color_mode = 'RGB'
try:
    sc.eevee.taa_render_samples = samples
except Exception:
    pass
sc.view_settings.view_transform = 'AgX'
sc.frame_start = start
sc.frame_end = end
sc.render.filepath = os.path.join(outdir, "f_")
t = time.time()
bpy.ops.render.render(animation=True)
print("SLICE_DONE", start, end, round(time.time()-t, 1))
