#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第二遍：验证速度、找段落变化、看结尾，并出一张结构图。"""
import subprocess
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

SR, HOP, WIN = 48000, 480, 2048
FPS = SR / HOP


def load(path):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
        capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def stft(x):
    n = 1 + (len(x) - WIN) // HOP
    idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
    fr = x[idx] * np.hanning(WIN).astype(np.float32)
    return np.abs(np.fft.rfft(fr, axis=1)).astype(np.float32)


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-9))


path = sys.argv[1]
outpng = sys.argv[2] if len(sys.argv) > 2 else "audio_structure.png"
x = load(path)
dur = len(x) / SR
S = stft(x)
f = np.fft.rfftfreq(WIN, 1.0 / SR)
n = S.shape[0]

# ---- 每帧特征 ----
frames = x[np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]]
rms = np.sqrt((frames ** 2).mean(1) + 1e-12)
rdb = db(rms)
L = np.log1p(S * 10.0)
d = np.diff(L, axis=0)
d[d < 0] = 0
flux = np.r_[0, d.sum(1)]

cent = (S @ f) / (S.sum(1) + 1e-9)
sub = db(S[:, (f >= 20) & (f < 60)].sum(1))
hi = db(S[:, (f >= 4000) & (f < 12000)].sum(1))

# ---- 速度：只靠自相关，列出前几名 ----
e = flux - flux.mean()
nfft = 1 << int(np.ceil(np.log2(2 * len(e))))
ac = np.fft.irfft(np.abs(np.fft.rfft(e, nfft)) ** 2)[:len(e)]
ac /= ac[0]

cands = []
for bpm in np.arange(50, 200.01, 0.05):
    lag = FPS * 60 / bpm
    i = int(round(lag))
    if i < 3 or i >= len(ac) - 1:
        continue
    frac = lag - i
    v = ac[i] * (1 - frac) + ac[i + 1] * frac
    i2 = int(round(2 * lag))
    v2 = ac[i2] if 0 < i2 < len(ac) else 0
    i3 = int(round(0.5 * lag))
    v3 = ac[i3] if 0 < i3 < len(ac) else 0
    cands.append((v + 0.45 * v2 - 0.15 * v3, bpm, v))
cands.sort(reverse=True)
print("速度候选（谐波加权 / 原始自相关）：")
seen = []
for sc, bpm, raw in cands:
    if any(abs(bpm - s) < 2.0 for s in seen):
        continue
    seen.append(bpm)
    print("   %7.2f BPM   score %+.3f   raw %+.3f   拍长 %.4fs" % (bpm, sc, raw, 60 / bpm))
    if len(seen) >= 8:
        break

# ---- 直接用强起始点的间隔直方图 ----
thr = np.percentile(flux, 92)
pk = []
i = 1
while i < len(flux) - 1:
    if flux[i] >= thr and flux[i] == flux[max(0, i - 3):i + 4].max():
        pk.append(i / FPS)
        i += 20
    else:
        i += 1
pk = np.array(pk)
ioi = np.diff(pk)
ioi = ioi[(ioi > 0.15) & (ioi < 2.2)]
print("\n强起始点 %d 个；间隔直方图（前 8）：" % len(pk))
h, edges = np.histogram(ioi, bins=120, range=(0.15, 2.2))
order = np.argsort(h)[::-1][:8]
for k in order:
    print("   %.3f s   ×%d   (≈%.1f BPM)" % ((edges[k] + edges[k + 1]) / 2, h[k],
                                             60 / ((edges[k] + edges[k + 1]) / 2)))

# ---- 0.5s 分辨率时间线 ----
step = int(0.5 * FPS)
rows = []
for t in np.arange(0, dur - 0.5, 0.5):
    a, z = int(t * FPS), int((t + 0.5) * FPS)
    rows.append((t, rdb[a:z].mean(), sub[a:z].mean(), hi[a:z].mean(),
                 cent[a:z].mean(), flux[a:z].mean()))

print("\n0.5s 时间线（RMS dB / sub / hi / centroid Hz / onset）")


def bar(v, lo, hi_, w=22):
    k = int(np.clip((v - lo) / (hi_ - lo + 1e-9), 0, 1) * w)
    return "█" * k + "·" * (w - k)


for t, r, s, hh, c, fl in rows:
    print("  %5.1f  %6.1f  %5.1f  %5.1f  %6.0f  %5.2f  |%s|"
          % (t, r, s, hh, c, fl, bar(c, 300, 2200)))

# ---- 结尾 ----
print("\n结尾细看（最后 6s，0.25s 步长）")
for t in np.arange(dur - 6, dur, 0.25):
    a, z = int(t * FPS), int((t + 0.25) * FPS)
    print("  %6.2f  RMS %6.1f  sub %5.1f  hi %5.1f  onset %5.2f"
          % (t, rdb[a:z].mean(), sub[a:z].mean(), hi[a:z].mean(), flux[a:z].mean()))

# ---- 图 ----
bpm_ref = float(sys.argv[3]) if len(sys.argv) > 3 else 112.1
bar_len = 240 / bpm_ref
fig, ax = plt.subplots(4, 1, figsize=(18, 11), sharex=True,
                       gridspec_kw={"height_ratios": [1.2, 1.6, 1.0, 0.9]})
tt = np.arange(n) / FPS
ax[0].plot(tt, db(np.abs(x)[: 1]) if False else np.zeros(1), alpha=0)
ax[0].plot(tt, rdb, lw=0.7, color="#141613")
ax[0].set_ylabel("RMS dBFS")
ax[0].grid(alpha=.15)

spec = db(S.T)
ax[1].imshow(spec, aspect="auto", origin="lower", cmap="magma",
             extent=[0, n / FPS, 0, SR / 2], vmin=-90, vmax=-10)
ax[1].set_yscale("symlog", linthresh=200)
ax[1].set_ylim(30, 16000)
ax[1].set_ylabel("Hz")

ax[2].plot(tt, flux, lw=0.6, color="#777B60")
ax[2].set_ylabel("onset")

ax[3].plot(tt, sub, lw=.7, color="#141613", label="sub 20-60")
ax[3].plot(tt, hi, lw=.7, color="#777B60", label="hi 4-12k")
ax[3].plot(tt, cent / 10, lw=.7, color="#E8622A", alpha=.8, label="centroid/10")
ax[3].legend(loc="upper right", fontsize=7)
ax[3].grid(alpha=.15)
ax[3].set_xlabel("seconds")

for a_ in ax:
    for k in np.arange(0, dur, bar_len):
        a_.axvline(k, color="#3a6ea5", lw=.4, alpha=.45)
    for k in np.arange(0, dur, bar_len * 8):
        a_.axvline(k, color="#0a4d8c", lw=1.0, alpha=.7)
ax[0].set_title("Rhine Lab.LLC · %.2f s · grid = %.2f BPM (bar=%.3f s), heavy line every 8 bars"
                % (dur, bpm_ref, bar_len), fontsize=10)
plt.tight_layout()
plt.savefig(outpng, dpi=110)
print("\nfigure -> " + outpng)
