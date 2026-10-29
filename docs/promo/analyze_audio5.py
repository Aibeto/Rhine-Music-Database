#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第五遍：搞清这条音轨到底是什么材料 —— 有音高吗？脉冲周期多少？有重音吗？"""
import subprocess
import sys

import numpy as np

SR = 48000


def load(p):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-ac", "1", "-ar", str(SR),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-9))


x = load(sys.argv[1])
dur = len(x) / SR

# ---------- 1. 长期平均谱：找谐波峰 ----------
seg = x[15 * SR:45 * SR]
N = 1 << 18
acc = np.zeros(N // 2 + 1)
cnt = 0
for i in range(0, len(seg) - N, N // 2):
    acc += np.abs(np.fft.rfft(seg[i:i + N] * np.hanning(N)))
    cnt += 1
lts = acc / cnt
fr = np.fft.rfftfreq(N, 1 / SR)
lt_db = db(lts)
print("=" * 70)
print("1. 长期平均谱（15–45s）—— 有谐波峰=有音高材料；平滑=纯噪声")
print("=" * 70)
ref = lt_db[(fr > 200) & (fr < 8000)].max()
pk_idx = [i for i in range(2, len(lt_db) - 2)
          if lt_db[i] == lt_db[max(0, i - 8):i + 9].max() and lt_db[i] > ref - 22 and 25 < fr[i] < 12000]
pk_idx = sorted(pk_idx, key=lambda i: -lt_db[i])[:18]
pk_idx.sort(key=lambda i: fr[i])
for i in pk_idx:
    print("   %8.1f Hz   %6.1f dB(rel %+.1f)   note≈%s"
          % (fr[i], lt_db[i], lt_db[i] - ref,
             ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"][
                 int(round(12 * np.log2(fr[i] / 440.0))) % 12]))
flat = np.percentile(lt_db[(fr > 200) & (fr < 8000)], 90) - np.percentile(
    lt_db[(fr > 200) & (fr < 8000)], 10)
print("   200–8k 谱平坦度(p90-p10) = %.1f dB  →  %s" % (flat, "接近白噪" if flat < 12 else "有明显结构"))
print("   20–60Hz : %.1f dB    60–250: %.1f    250–2k: %.1f   2k–8k: %.1f   8k–16k: %.1f"
      % tuple(db(lts[(fr >= a) & (fr < b)].mean()) for a, b in
              [(20, 60), (60, 250), (250, 2000), (2000, 8000), (8000, 16000)]))

# ---------- 2. RMS 包络的脉冲周期 ----------
hop = 96                      # 2 ms
n = len(x) // hop
e = np.sqrt(np.convolve(x ** 2, np.ones(hop) / hop, "same")[::hop][:n])
edb = db(e)
print("\n" + "=" * 70)
print("2. 包络脉冲（2ms 分辨率）")
print("=" * 70)
core = edb[int(5 / 0.002):int(65 / 0.002)]
z = core - core.mean()
nn = 1 << int(np.ceil(np.log2(2 * len(z))))
ac = np.fft.irfft(np.abs(np.fft.rfft(z, nn)) ** 2)[:len(z)]
ac /= ac[0]
lags = np.arange(20, 400)
best = lags[np.argsort(ac[lags])[::-1][:6]]
print("   包络自相关前 6 个峰（ms / Hz / 折合 BPM 若为 1/8 音符）:")
for l in sorted(best):
    print("      %5.1f ms   %6.3f Hz   → %s" % (l * 2, 1000 / (l * 2), ""))

# 用最大峰做门控折叠
P = int(best[0])
fold = core[:len(core) // P * P].reshape(-1, P)
shape = fold.mean(0)
sh = shape - shape.min()
sh /= sh.max()
print("   门控形状（每格 %.1f ms）：" % (P * 2))
print("   " + " ".join("%.2f" % v for v in sh[::max(1, P // 24)]))

# ---------- 3. 重音：按 N 个脉冲折叠 ----------
print("\n" + "=" * 70)
print("3. 有没有每 2/4/8/16 个脉冲的重音（= 是否真有节拍）")
print("=" * 70)
for k in [2, 3, 4, 6, 8, 12, 16, 32]:
    nb = len(core) // (P * k)
    if nb < 3:
        continue
    g = core[:nb * P * k].reshape(nb, k, P).mean(2)   # (nb, k)
    m = g.mean(0)
    amp = (m.max() - m.min()) / (m.mean() + 1e-9) * 100
    print("   每 %2d 个脉冲折叠：组内起伏 %5.1f%%  （%s）"
          % (k, amp, "有重音" if amp > 6 else "无"))

# ---------- 4. 结尾 ----------
print("\n" + "=" * 70)
print("4. 结尾 0.5s 粒度")
print("=" * 70)
for t in np.arange(68, dur - 0.5, 0.5):
    a, b = int(t * SR), int((t + 0.5) * SR)
    s = x[a:b]
    print("   %6.2f  RMS %6.1f  dB   crest %5.1f dB"
          % (t, db(np.sqrt((s ** 2).mean() + 1e-12)),
             db(np.abs(s).max()) - db(np.sqrt((s ** 2).mean() + 1e-12))))
