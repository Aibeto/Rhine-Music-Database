#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第四遍：用「梳状相位检验」把速度和相位钉死，并精确定位最后一击。"""
import subprocess
import sys

import numpy as np

SR, HOP, WIN = 48000, 480, 2048
FPS = SR / HOP


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
L = np.log1p(S * 10)
d = np.diff(L, axis=0)
d[d < 0] = 0
flux = np.r_[0, d.sum(1)]
np.savez("/tmp/feat.npz", flux=flux, dur=dur, fps=FPS)

# 高频段通量（打点/踩镲更锐利）
hs = S[:, (f >= 2000)].sum(1)
L2 = np.log1p(hs[:, None] * 10)
d2 = np.diff(L2, axis=0)
d2[d2 < 0] = 0
flux_hi = np.r_[0, d2[:, 0]]

def comb_score(env, period, phase, spp=1.0):
    pos = phase + period * np.arange(int((len(env) - phase) / period) + 1)
    pos = pos[pos < len(env) - 1]
    return env[np.round(pos).astype(int)].mean() / (env.mean() + 1e-9)

print("=" * 74)
print("A. 梳状相位检验：哪个「小节长度」能让重击最好地落在网格上")
print("=" * 74)
print("  BPM     拍(s)    小节(s)   拍网格分  小节网格分  最佳小节相位(s)")
res = []
for bpm in np.arange(104, 120.01, 0.25):
    beat = 60 / bpm
    bb, bp = -1, 0
    for ph in np.arange(0, beat, 0.01):
        s = comb_score(flux_hi, beat, ph)
        if s > bb:
            bb, bp = s, ph
    bar = 4 * beat
    sb, sp = -1, 0
    for ph in np.arange(0, bar, 0.01):
        s = comb_score(flux_hi, bar, ph)
        if s > sb:
            sb, sp = s, ph
    res.append((bpm, beat, bar, bb, sb, sp))

res.sort(key=lambda r: -r[4])
for bpm, beat, bar, bb, sb, sp in res[:6]:
    print("  %6.2f  %7.4f  %7.4f   %6.3f    %6.3f     %6.3f"
          % (bpm, beat, bar, bb, sb, sp))

BPM = res[0][0]
BEAT = 60 / BPM
BAR = 4 * BEAT
# 相位用全频通量再确认一次
best = (-1, 0)
for ph in np.arange(0, BEAT, 0.002):
    s = comb_score(flux, BEAT, ph)
    if s > best[0]:
        best = (s, ph)
PH = best[1]
print("\n  选中  BPM %.2f   拍 %.4fs   小节 %.4fs   首拍相位 %.3fs" % (BPM, BEAT, BAR, PH))
print("  起拍序列（前 8 个）: " + ", ".join("%.3f" % (PH + k * BEAT) for k in range(8)))

print("\n" + "=" * 74)
print("B. 最后一击 / 全曲收尾")
print("=" * 74)
sm = np.convolve(np.abs(x), np.ones(240) / 240, mode="same")
thi = db(np.percentile(sm, 90)) - 30
alive = sm > 10 ** (thi / 20)
i_last = len(alive) - 1 - np.argmax(alive[::-1])
print("  能量降到 -30dB(相对p90) 以下的时刻   %.3f s" % (i_last / SR))
print("  该时刻 ÷ 小节 = %.3f 小节" % (i_last / SR / BAR))

pk = np.argsort(flux)[-400:]
pk = np.sort([p for i, p in enumerate(pk) if i == 0 or p - pk[i - 1] > 15])
print("  最后 6 个强起始点: " + ", ".join("%.3f" % (p / FPS) for p in pk[-6:]))
print("  最后一个强起始点 ÷ 拍 = %.3f" % (pk[-1] / FPS / BEAT))
for t in pk[-3:]:
    print("     t=%7.3f  → %.3f 拍 / %.3f 小节" % (t / FPS, t / FPS / BEAT, t / FPS / BAR))

print("\n" + "=" * 74)
print("C. 小节级能量表（%.4fs 一小节）" % BAR)
print("=" * 74)
bands = {"sub": (20, 60), "low": (60, 250), "mid": (250, 2500), "hi": (2500, 8000), "air": (8000, 16000)}
bv = {k: db(S[:, (f >= a) & (f < b)].sum(1)) for k, (a, b) in bands.items()}
has = len(x) - WIN
nb = int((dur - 0.01) / BAR)
print("  bar   t0      RMS    sub    low    mid    hi     air    cent   hits/s")
for b in range(nb):
    a0, z0 = int(b * BAR * FPS), min(int((b + 1) * BAR * FPS), n)
    k0, k1 = int(b * BAR * SR), min(int((b + 1) * BAR * SR), len(x))
    seg = x[k0:k1]
    r = db(np.sqrt((seg ** 2).mean() + 1e-12))
    c = float((S[a0:z0] @ f).sum() / (S[a0:z0].sum() + 1e-9))
    hits = int(flux[k0 and a0:z0].__len__() and (flux[a0:z0] > np.percentile(flux, 88)).sum())
    print("  %3d  %6.2f  %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.0f  %4d"
          % (b + 1, b * BAR, r, bv["sub"][a0:z0].mean(), bv["low"][a0:z0].mean(),
             bv["mid"][a0:z0].mean(), bv["hi"][a0:z0].mean(), bv["air"][a0:z0].mean(),
             c, hits))
