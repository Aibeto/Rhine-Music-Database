#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从音轨生成两条可直接塞进 SVG 的 polyline 路径（0–100 坐标）：
  docs/promo/_rms_path.txt  —— 实测 RMS 包络，一拍一点（132 点），脉冲列整齐可见
  docs/promo/_air_path.txt  —— 实测高频占比（8–16 kHz / 总能量），半拍一点
替换 build_board.py 里的 AUDIO_RMS / AUDIO_AIR 两个常量。
"""
import subprocess

import numpy as np

SR, HOP, WIN = 48000, 480, 2048
AUDIO = "塞壬唱片-MSR,Erik Castro - Rhine Lab.LLC.flac"
BEAT = 60.0 / 112.0
OFF = 8 * BEAT               # 删掉的开头 2 小节
DUR = 132 * BEAT             # 70.714s

raw = subprocess.run(["ffmpeg", "-v", "error", "-i", AUDIO, "-ac", "1", "-ar", str(SR),
                      "-f", "f32le", "-"], capture_output=True, check=True).stdout
x = np.frombuffer(raw, dtype=np.float32).copy()[int(OFF * SR): int((OFF + DUR) * SR)]

n = 1 + (len(x) - WIN) // HOP
idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
fr = x[idx] * np.hanning(WIN).astype(np.float32)
rms = np.sqrt((fr ** 2).mean(1) + 1e-12)
S = np.abs(np.fft.rfft(fr, axis=1))
f = np.fft.rfftfreq(WIN, 1.0 / SR)
air = 20 * np.log10(S[:, (f >= 8000) & (f < 16000)].sum(1) / (S.sum(1) + 1e-9) + 1e-9)
air = np.convolve(air, np.ones(105) / 105, mode="same")   # ≈1.05s 平滑
air[:52] = air[52]; air[-53:] = air[-54]   # 修掉卷积边缘塌陷


def path(v, npts, lo, hi_, pool="max", ymin=4.0, ymax=96.0):
    bins = np.linspace(0, len(v), npts + 1).astype(int)
    out = []
    for i in range(npts):
        a, z = bins[i], max(bins[i + 1], bins[i] + 1)
        m = v[a:z].max() if pool == "max" else v[a:z].mean()
        yy = ymax - np.clip((m - lo) / (hi_ - lo), 0, 1) * (ymax - ymin)
        out.append("%.2f %.2f" % (100.0 * i / (npts - 1), yy))
    return "M " + " L ".join(out)


# 每拍两点：峰 + 谷 → 132 颗高低一致、齿形一致的「梳齿」
NFR = len(rms)
edges = np.linspace(0, NFR, 133).astype(int)
pk, tr = [], []
for b in range(132):
    a = int(edges[b]); w = int(min(edges[b + 1], NFR))
    a = min(a, NFR - 2); w = max(w, a + 2)
    z = a + max(1, int((w - a) * 0.7))
    pk.append(rms[a:z].max() if z > a else rms[a])
    tr.append(rms[z:w].min() if w > z else rms[a])
pk = np.array(pk); tr = np.array(tr)
comb = np.empty(264)
comb[0::2] = pk
comb[1::2] = tr
lo = float(np.percentile(tr, 50))
hi_ = float(np.percentile(pk, 99.4))


def combpath(v, lo, hi_, ymin=86.0, ymax=100.0):
    out = []
    for i, m in enumerate(v):
        yy = ymax - np.clip((m - lo) / (hi_ - lo), 0, 1) * (ymax - ymin)
        out.append("%.2f %.2f" % (100.0 * i / (len(v) - 1), yy))
    return "M " + " L ".join(out)


p1 = combpath(comb, lo, hi_)
print("tooth: peak med %.3f  floor med %.3f  -> heights %.0f..%.0f" %
      (np.median(pk), np.median(tr), lo, hi_))
p2 = path(air, 264, np.percentile(air, 2), np.percentile(air, 98), "mean", ymin=14.0, ymax=76.0)
open("_rms_path.txt", "w").write(p1)
open("_air_path.txt", "w").write(p2)
print("rms %d chars / air %d chars" % (len(p1), len(p2)))
print("rms head:", p1[:90])
