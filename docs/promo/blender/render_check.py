# -*- coding: utf-8 -*-
"""Render one check frame per shot (720p, 12 fps, low samples)."""
import bpy, os, time, sys

OUT = "/tmp/pv"
os.makedirs(OUT, exist_ok=True)


def segments():
    sc = bpy.context.scene
    ms = sorted(sc.timeline_markers, key=lambda m: m.frame)
    out = []
    for i, m in enumerate(ms):
        f0 = m.frame
        f1 = ms[i + 1].frame - 1 if i + 1 < len(ms) else sc.frame_end
        out.append((m.name.replace(' ', '_').replace('/', '-'), f0, f1, m.camera))
    return out


def chk(i0=0, i1=None, frac=0.55, res=(1280, 720), samples=8):
    sc = bpy.context.scene
    segs = segments()
    i1 = len(segs) if i1 is None else i1
    sc.render.resolution_x, sc.render.resolution_y = res
    sc.render.resolution_percentage = 100
    sc.render.fps = 12
    try:
        sc.eevee.taa_render_samples = samples
    except Exception:
        pass
    sc.view_settings.view_transform = 'AgX'
    t0 = time.time()
    done = []
    for name, f0, f1, cam in segs[i0:i1]:
        sc.camera = cam
        f = int(f0 + (f1 - f0) * frac)
        sc.frame_set(f)
        p = os.path.join(OUT, "%s_f%03d.png" % (name, f))
        sc.render.filepath = p
        t = time.time()
        bpy.ops.render.render(write_still=True)
        done.append((name, f, round(time.time() - t, 2)))
    print("CHK", i0, i1, "total", round(time.time() - t0, 1), "n", len(done))
    for d in done:
        print("  ", d)
    return done
