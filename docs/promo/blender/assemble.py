# -*- coding: utf-8 -*-
"""Assemble every group's shots into one .blend.
usage: blender -b --factory-startup -P assemble.py -- out.blend [g1,g2,...]
Every group exports SHOTS = [(key, t0, t1, builder)]; builders get their own lane (off = i*80).
"""
import bpy, sys, os, importlib, traceback

BASE = "/home/aibeto/code/Rhine-Music-Database/docs/promo/blender"
sys.path.insert(0, BASE)
import shotkit as K
from shotkit import frames_of, V


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = argv[0] if argv else os.path.join(BASE, "rhine-pv-previz.blend")
    groups = (argv[1].split(",") if len(argv) > 1 else ["g1", "g2", "g3", "g4", "g5", "g6", "g7"])

    sc = K.setup(1280, 720, 12)
    sc.render.fps = K.FPS
    sc.frame_start = 1
    sc.frame_end = int(round(70.7143 * K.FPS))

    rows = []
    for gname in groups:
        try:
            G = importlib.import_module(gname)
        except Exception as e:
            print("GROUP_IMPORT_FAIL %s %s" % (gname, e))
            continue
        for i, (key, t0, t1, builder) in enumerate(G.SHOTS):
            rows.append((t0, t1, key, builder, (i + 1) * 80.0, gname))
    rows.sort(key=lambda r: r[0])

    ok, fail = [], []
    for t0, t1, key, builder, ox, gname in rows:
        off = V((ox, 0, 0))
        C = "COL_" + key.replace(".", "_")
        try:
            f0, f1 = frames_of(t0, t1)
            res = builder(off, C, f0, f1)
            cams = res if isinstance(res, (list, tuple)) else [(res, f0, f1)]
            for cam, fa, fb in cams:
                m = sc.timeline_markers.new("%s_%d" % (key, fa), frame=int(fa))
                m.camera = cam
            ok.append((key, t0, t1, len(cams), gname))
        except Exception:
            fail.append((key, gname, traceback.format_exc().splitlines()[-1]))

    # ---- lane isolation: 每一帧只渲染自己那一镜的物体 ----
    # 34 个镜头共用一条时间轴，几何都摆在各自 80m 的"跑道"上；不同跑道的物体
    # 会互相出现在彼此的镜头里（尤其超广角），所以给每个物体打 hide_render 关键帧。
    iso = 0
    for t0, t1, key, builder, ox, gname in rows:
        C = "COL_" + key.replace(".", "_")
        col = bpy.data.collections.get(C)
        if not col:
            continue
        f0, f1 = frames_of(t0, t1)
        objs = list(col.all_objects)
        for ob in objs:
            if f0 > 1:
                ob.hide_render = True
                ob.keyframe_insert("hide_render", frame=1)
            ob.hide_render = False
            ob.keyframe_insert("hide_render", frame=int(f0))
            ob.hide_render = True
            ob.keyframe_insert("hide_render", frame=int(f1) + 1)
        iso += len(objs)
    for act in (bpy.data.actions if hasattr(bpy.data, "actions") else []):
        for fc in (act.fcurves if hasattr(act, "fcurves") else []):
            if "hide_render" in fc.data_path:
                for kp in fc.keyframe_points:
                    kp.interpolation = 'CONSTANT'
    print("ISO objects=%d" % iso)

    # a black world, no lights: every shot is emissive by design
    sc.camera = bpy.data.objects.get("01_cam") or (bpy.data.objects.get(ok[0][0] + "_cam") if ok else None)
    bpy.ops.wm.save_as_mainfile(filepath=out)

    print("ASSEMBLE shots=%d ok=%d fail=%d objs=%d cams=%d markers=%d frames=%d..%d" % (
        len(rows), len(ok), len(fail), len(bpy.data.objects),
        len([o for o in bpy.data.objects if o.type == 'CAMERA']),
        len(sc.timeline_markers), sc.frame_start, sc.frame_end))
    for k, t0, t1, n, g in ok:
        print("  OK   %-4s %6.2f-%6.2f n=%d %s" % (k, t0, t1, n, g))
    for k, g, err in fail:
        print("  FAIL %-4s %s :: %s" % (k, g, err))
    print("SAVED " + out)


main()
