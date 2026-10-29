#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第三遍：定小节网格、切段落、找结尾。输出可直接抄进分镜表的段落图。"""
import subprocess
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SR, HOP, WIN = 48000, 480, 2048
FPS = SR / HOP
BANDS = [(20, 60, "sub"), (60, 250, "low"), (250, 800, "lomid"),
         (800, 2500, "mid"), (2500, 8000, "hi"), (8000, 16000, "air")]


def load(p):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-ac", "1", "-ar", str(SR),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-9))


path = sys.argv[1]
x = load(path)
dur = len(x) / SR
n = 1 + (len(x) - WIN) // HOP
idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
fr = x[idx]
S = np.abs(np.fft.rfft(fr * np.hanning(WIN).astype(np.float32), axis=1)).astype(np.float32)
f = np.fft.rfftfreq(WIN, 1.0 / SR)
rms = np.sqrt((fr ** 2).mean(1) + 1e-12)
rdb = db(rms)
L = np.log1p(S * 10)
d = np.diff(L, axis=0)
d[d < 0] = 0
flux = np.r_[0, d.sum(1)]
cent = (S @ f) / (S.sum(1) + 1e-9)
band = {name: db(S[:, (f >= lo) & (f < hi)].sum(1)) for lo, hi, name in BANDS}

# ---------- 1. 用「小节级自相似度」检验候选速度 ----------
print("=" * 72)
print("1. 小节网格检验（把特征按拍/小节折叠，看重复度）")
print("=" * 72)
names = ["rms", "sub", "low", "lomid", "mid", "hi", "air", "cent"]
F = np.stack([rdb] + [band[k] for k, _, _ in [(b[2], 0, 0) for b in BANDS]] + [cent], 1)
F = (F - F.mean(0)) / (F.std(0) + 1e-9)

for bpm in [50.0, 53.9, 56.2, 64.15, 74.5, 80.5, 89.85, 107.85, 112.1, 128.3, 150.9]:
    beat = 60 / bpm
    lb = int(round(4 * beat * FPS))
    if lb < 20 or lb >= len(F) - 1:
        continue
    # 整段按小节对齐后做逐帧方差压缩：真正的拍长会让每帧的「同相位」差异最小
    a = F[: len(F) // lb * lb].reshape(-1, lb, F.shape[1])
    within = a.std(0).mean()          # 小节内同相位一致性
    across = F.std(0).mean()
    print("   %7.2f BPM  拍 %6.4fs  小节 %6.4fs  同相位差异 %6.3f / 整体 %6.3f  → %s"
          % (bpm, beat, 4 * beat, within, across,
             "★" if within < across * 0.75 else ""))

# ---------- 2. 选一个网格做段落 ----------
BPM = float(sys.argv[2]) if len(sys.argv) > 2 else 112.1
BAR = 240.0 / BPM
nb = int(dur / BAR)
cols = ["rms"] + [b[2] for b in BANDS] + ["cent", "flux"]
mat = []
for b in range(nb):
    a, z = int(b * BAR * FPS), int((b + 1) * BAR * FPS)
    mat.append([rdb[a:z].mean()] + [band[k][a:z].mean() for _, _, k in BANDS]
               + [cent[a:z].mean(), flux[a:z].mean()])
mat = np.array(mat)
Z = (mat - mat.mean(0)) / (mat.std(0) + 1e-9)

print("\n" + "=" * 72)
print("2. 段落切分（BPM %.2f，小节 %.4fs，共 %d 小节）" % (BPM, BAR, nb))
print("=" * 72)


def sse(a):
    return ((a - a.mean(0)) ** 2).sum() if len(a) > 1 else 0.0


def best_split(a):
    base = sse(a)
    bi, bg = -1, 0.0
    for k in range(2, len(a) - 1):
        g = base - sse(a[:k]) - sse(a[k:])
        if g > bg:
            bi, bg = k, g
    return bi, bg


segs = [(0, nb)]
while True:
    gain, pick = 0.0, None
    for i, (a, z) in enumerate(segs):
        k, g = best_split(Z[a:z])
        if k > 0 and g > gain:
            gain, pick = g, (i, a + k)
    if pick is None or gain < 6.0 or len(segs) > 11:
        break
    i, k = pick
    a, z = segs[i]
    segs[i:i + 1] = [(a, k), (k, z)]

segs.sort()
print("  seg  bars      t0      t1     dur   rms   hi    mid   air   cent  角色")
for j, (a, z) in enumerate(segs):
    m = mat[a:z].mean(0)
    print("   S%d  %2d-%-2d  %6.2f  %6.2f  %5.2f  %5.1f %5.1f %5.1f %5.1f %5.0f"
          % (j + 1, a + 1, z, a * BAR, z * BAR, (z - a) * BAR,
             m[0], m[5], m[4], m[6], m[7]))

# ---------- 3. 结尾 ----------
print("\n" + "=" * 72)
print("3. 结尾")
print("=" * 72)
thr = np.percentile(flux, 40)
live = flux > thr
last = np.argmax(live[::-1])
print("  最后一个明显起始点      %.3f s" % (last / FPS))
for t in np.arange(dur - 8, dur, 0.125):
    a, z = int(t * FPS), int((t + 0.125) * FPS)
    if z > n:
        break
    tag = ""
    if rdb[a:z].mean() < -45:
        tag = "  ← 静"
    print("   %6.2f  RMS %6.1f  hi %6.1f  flux %6.2f%s"
          % (t, rdb[a:z].mean(), band["hi"][a:z].mean(), flux[a:z].mean(), tag))

# ---------- 4. 图 ----------
fig, ax = plt.subplots(3, 1, figsize=(19, 9), sharex=True,
                       gridspec_kw={"height_ratios": [1.0, 1.8, 1.0]})
tt = np.arange(n) / FPS
ax[0].plot(tt, rdb, lw=.6, color="#141613")
ax[0].set_ylabel("RMS dBFS")
ax[0].grid(alpha=.15)
ax[1].imshow(db(S.T), aspect="auto", origin="lower", cmap="magma",
             extent=[0, n / FPS, 0, SR / 2], vmin=-88, vmax=-22)
ax[1].set_yscale("symlog", linthresh=300)
ax[1].set_ylim(40, 16000)
ax[1].set_ylabel("Hz")
ax[2].plot(tt, band["hi"], lw=.5, color="#E8622A", alpha=.85, label="hi 2.5-8k")
ax[2].plot(tt, band["mid"], lw=.5, color="#777B60", label="mid .8-2.5k")
ax[2].plot(tt, band["sub"], lw=.5, color="#141613", label="sub 20-60")
ax[2].legend(fontsize=7, loc="upper right")
ax[2].grid(alpha=.15)
for a_ in ax:
    for k in np.arange(0, dur, BAR):
        a_.axvline(k, color="#3a6ea5", lw=.35, alpha=.4)
    for k in np.arange(0, dur, BAR * 4):
        a_.axvline(k, color="#0a4d8c", lw=.9, alpha=.65)
    for s, (a, z) in enumerate(segs):
        a_.axvline(a * BAR, color="#b02a2a", lw=1.4, alpha=.85)
ax[0].set_title("Rhine Lab.LLC  %.2fs  grid %.2fBPM bar=%.3fs  red=section  blue=4bar"
                % (dur, BPM, BAR), fontsize=10)
plt.tight_layout()
plt.savefig("audio_sections.png", dpi=105)
print("\nfigure -> audio_sections.png")
