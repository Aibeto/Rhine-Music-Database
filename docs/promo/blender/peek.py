# -*- coding: utf-8 -*-
"""Render one frame per shot at a chosen fraction through the shot (default .92 = steady state).
usage: blender -b --factory-startup -P peek.py -- g1 /tmp/g1 640 360 6 0.92
"""
import bpy, sys, os, importlib

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
gname = argv[0] if argv else "g1"
outdir = argv[1] if len(argv) > 1 else "/tmp/peek"
W = int(argv[2]) if len(argv) > 2 else 640
H = int(argv[3]) if len(argv) > 3 else 360
SAMPLES = int(argv[4]) if len(argv) > 4 else 6
FRAC = float(argv[5]) if len(argv) > 5 else 0.92

BASE = "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender"
sys.path.insert(0, BASE)
import shotkit as K
from shotkit import frames_of
G = importlib.import_module(gname)

sc = K.setup(W, H, SAMPLES)
sc.render.image_settings.file_format = 'PNG'
os.makedirs(outdir, exist_ok=True)
n = 0
for i, (key, t0, t1, builder) in enumerate(G.SHOTS):
    off = K.V((i * 6.0, 0, 0))
    C = "COL_" + key
    f0, f1 = frames_of(t0, t1)
    cams = builder(off, C, f0, f1)
    if isinstance(cams, (list, tuple)):
        for cam, fa, fb in cams:
            sc.camera = cam
            sc.frame_set(int(fa + (fb - fa) * FRAC))
            sc.render.filepath = os.path.join(outdir, "%s_%s.png" % (key, cam.name))
            bpy.ops.render.render(write_still=True)
            n += 1
    else:
        sc.camera = cams
        sc.frame_set(int(f0 + (f1 - f0) * FRAC))
        sc.render.filepath = os.path.join(outdir, "%s_f%03d.png" % (key, sc.frame_current))
        bpy.ops.render.render(write_still=True)
        n += 1
print("PEEK_DONE %s %d" % (gname, n))
