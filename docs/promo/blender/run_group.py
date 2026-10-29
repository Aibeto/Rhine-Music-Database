# -*- coding: utf-8 -*-
"""Render one frame per shot of a group file.
blender -b --factory-startup -P run_group.py -- g1 /tmp/g1 640 360 6
"""
import bpy, sys, os, importlib, time
BASE = "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender"
sys.path.insert(0, BASE)
a = sys.argv[sys.argv.index("--") + 1:]
mod, outdir = a[0], a[1]
W, H, S = int(a[2]), int(a[3]), int(a[4])
os.makedirs(outdir, exist_ok=True)
import shotkit as K
m = importlib.import_module(mod)
sc = K.setup(W, H, S)
cams = []
for i, (key, t0, t1, fn) in enumerate(m.SHOTS):
    off = K.V((i * 6.0, 0, 0))
    fa, fb = K.frames_of(t0, t1)
    r = fn(off, "G" + key.replace('.', ''), fa, fb)
    segs = r if isinstance(r, list) else [(r, fa, fb)]
    for cam, cfa, cfb in segs:
        cams.append((key, cam, cfa, cfb))
sc.timeline_markers.clear()
for key, cam, cfa, cfb in cams:
    mk = sc.timeline_markers.new(str(key), frame=cfa)
    mk.camera = cam
sc.camera = cams[0][1]
t = time.time()
for key, cam, cfa, cfb in cams:
    f = int(cfa + (cfb - cfa) * 0.55)
    sc.frame_set(f)
    sc.camera = cam
    sc.render.filepath = os.path.join(outdir, "%s_f%03d.png" % (key, f))
    bpy.ops.render.render(write_still=True)
print("GROUP_DONE", mod, len(cams), round(time.time() - t, 1))
