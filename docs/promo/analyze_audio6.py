#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第六遍：脉冲峰的线性回归（到底有没有稳定网格）+ 音色随时间怎么变。"""
import subprocess
import sys

import numpy as np

SR = 48000
HOP = 96          # 2 ms


def load(p):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-ac", "1", "-ar", str(SR),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-9))


x = load(sys.argv[1])
dur = len(x) / SR
env = np.sqrt(np.convolve(x ** 2, np.ones(HOP) / HOP, "same")[::HOP])
k = np.ones(15) / 15          # 平滑 ~30ms，去掉噪声导致的假峰
env = np.convolve(env, k, "same")
edb = db(env)

# ---------- 脉冲峰 ----------
prom = 6.0
pk = []
for i in range(31, len(edb) - 31):
    if edb[i] == edb[i - 30:i + 31].max():
        lo = max(0, i - int(0.30 / (HOP / SR)))
        hi = min(len(edb), i + int(0.30 / (HOP / SR)))
        if edb[i] - np.percentile(edb[lo:hi], 20) > prom:
            pk.append(i)
pk = np.array(pk)
t = pk * HOP / SR
print("脉冲峰 %d 个；前 12 个时刻：%s" % (len(pk), np.round(t[:12], 3)))

iv = np.diff(t)
iv = iv[(iv > 0.2) & (iv < 1.2)]
print("峰间距：中位 %.4f s  均值 %.4f  std %.4f  (n=%d)"
      % (np.median(iv), iv.mean(), iv.std(), len(iv)))
print("直方图：", np.histogram(iv, bins=10, range=(0.35, 0.75))[0])

# 回归：第 k 个峰 ~ a + b*k
k = np.arange(len(t))
A = np.vstack([np.ones_like(k), k]).T
coef, res, *_ = np.linalg.lstsq(A, t, rcond=None)
pred = A @ coef
print("线性网格拟合：首峰 %.4fs，周期 %.5fs  →  %.3f BPM（四分音符）  残差 RMS %.1f ms  max %.1f ms"
      % (coef[0], coef[1], 60 / coef[1], np.sqrt(((t - pred) ** 2).mean()) * 1000,
         np.abs(t - pred).max() * 1000))

# ---------- 音色随时间 ----------
print("\n每 2s 的音色（1s 窗口，dB 相对总和；centroid）")
N = 48000
fr = np.fft.rfftfreq(N, 1 / SR)
edges = [(20, 60), (60, 200), (200, 800), (800, 2500), (2500, 8000), (8000, 16000)]
print("   t     rms   sub    lo    lomid  mid   hi    air   cent  flat")
for t0 in np.arange(0, dur - 2, 2.0):
    a = int(t0 * SR)
    seg = x[a:a + SR]
    S = np.abs(np.fft.rfft(seg[:N] * np.hanning(N)))
    tot = S.sum() + 1e-9
    rel = [db(S[(fr >= lo) & (fr < hi)].sum() / tot) for lo, hi in edges]
    cent = (S * fr).sum() / tot
    gm = np.exp(np.log(S[(fr > 200) & (fr < 8000)] + 1e-9).mean())
    am = S[(fr > 200) & (fr < 8000)].mean()
    print("  %5.1f  %5.1f  %5.1f %5.1f %5.1f %5.1f %5.1f %5.1f %5.0f  %4.1f"
          % (t0, db(np.sqrt((seg ** 2).mean())), *rel, cent, gm / am))
