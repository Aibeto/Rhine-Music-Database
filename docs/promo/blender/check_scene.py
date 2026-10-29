# -*- coding: utf-8 -*-
"""One frame per shot (92% through), rendered 6-way parallel, then stitched into a contact sheet.
usage: python3 check_scene.py <scene.blend> <outdir> [W H SAMPLES JOBS]
"""
import sys, os, subprocess, math, json

BL = "/usr/bin/blender"
HERE = os.path.dirname(os.path.abspath(__file__))


def shot_frames(blend):
    """Ask Blender for the marker table: (name, frame)."""
    code = ("import bpy,json;sc=bpy.context.scene;"
            "print('MARKERS'+json.dumps([[m.name,m.frame] for m in sc.timeline_markers]))")
    out = subprocess.run([BL, "-b", blend, "--factory-startup", "-P", "/dev/stdin"],
                         input=code, capture_output=True, text=True).stdout
    for ln in out.splitlines():
        if ln.startswith("MARKERS"):
            return json.loads(ln[7:])
    return []


def main():
    blend = os.path.abspath(sys.argv[1])
    outdir = os.path.abspath(sys.argv[2])
    W = int(sys.argv[3]) if len(sys.argv) > 3 else 640
    H = int(sys.argv[4]) if len(sys.argv) > 4 else 360
    S = int(sys.argv[5]) if len(sys.argv) > 5 else 6
    J = int(sys.argv[6]) if len(sys.argv) > 6 else 6
    os.makedirs(outdir, exist_ok=True)

    marks = shot_frames(blend)
    marks.sort(key=lambda m: m[1])
    frames = []
    for i, (name, f) in enumerate(marks):
        nxt = marks[i + 1][1] if i + 1 < len(marks) else f + 12
        frames.append(int(f + max(0, (nxt - f - 1)) * 0.55))
    frames = sorted(set(frames))
    print("CHECK frames=%d jobs=%d" % (len(frames), J))

    chunks = [frames[i::J] for i in range(J)]
    procs = []
    for j, ch in enumerate(chunks):
        if not ch:
            continue
        cmd = [BL, "-b", blend, "--factory-startup", "-P", os.path.join(HERE, "render_list.py"),
               "--", outdir, str(S), str(W), str(H)] + [str(f) for f in ch]
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    for p in procs:
        out = p.communicate()[0]
        for ln in out.splitlines():
            if "LIST_DONE" in ln or "Error" in ln:
                print("   ", ln.strip())
    made = sorted(f for f in os.listdir(outdir) if f.endswith(".png"))
    print("CHECK_DONE %d png" % len(made))


main()
