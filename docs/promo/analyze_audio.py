#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
分析 docs/promo 下的 Rhine Lab.LLC 音轨，输出可供分镜使用的时间网格。
只依赖 numpy + ffmpeg（无 librosa / scipy）。

用法：
    python3 analyze_audio.py <audio> [out.json]
"""
import json
import subprocess
import sys

import numpy as np

SR = 48000
HOP = 480          # 10.0 ms / frame  → 100 fps，时间轴直接可读
WIN = 2048


def load_mono(path, sr=SR):
    """ffmpeg 解码为单声道 float32。"""
    cmd = [
        "ffmpeg", "-v", "error", "-i", path,
        "-ac", "1", "-ar", str(sr), "-f", "f32le", "-",
    ]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()


def stft_mag(x, win=WIN, hop=HOP):
    n = 1 + (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    frames = x[idx] * np.hanning(win).astype(np.float32)
    return np.abs(np.fft.rfft(frames, axis=1)).astype(np.float32)   # (n, win/2+1)


def db(x):
    return 20.0 * np.log10(np.maximum(x, 1e-9))


def band_energy(S, sr, lo, hi):
    f = np.fft.rfftfreq(WIN, 1.0 / sr)
    m = (f >= lo) & (f < hi)
    return S[:, m].sum(axis=1)


def onset_env(S):
    """对数幅度谱的半波整流谱通量。"""
    L = np.log1p(S * 10.0)
    d = np.diff(L, axis=0)
    d[d < 0] = 0.0
    o = d.sum(axis=1)
    o = np.concatenate([[0.0], o])
    # 局部归一化，避免大音量段独霸
    k = 25
    pad = np.pad(o, (k, k), mode="edge")
    loc = np.array([pad[i:i + 2 * k + 1].mean() for i in range(len(o))])
    return o / (loc + 1e-6)


def tempo(env, fps=100.0):
    """自相关 + 谐波加权，返回 (bpm, 置信度, 相位帧)。"""
    e = env - env.mean()
    n = len(e)
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    ac = np.fft.irfft(np.abs(np.fft.rfft(e, nfft)) ** 2)[:n]
    ac /= (ac[0] + 1e-12)

    best = []
    for bpm in np.arange(60.0, 180.01, 0.05):
        lag = fps * 60.0 / bpm
        i = int(round(lag))
        if i < 3 or i >= n - 1:
            continue
        frac = lag - i
        v = ac[i] * (1 - frac) + ac[i + 1] * frac
        i2 = int(round(2 * lag))
        v2 = ac[i2] if 0 < i2 < n else 0.0
        i3 = int(round(0.5 * lag))
        v3 = ac[i3] if 0 < i3 < n else 0.0
        best.append((v + 0.45 * v2 - 0.15 * v3, bpm))
    best.sort(reverse=True)
    bpm = best[0][1]

    # 相位：把 env 累加到网格上，取最大值
    lag = fps * 60.0 / bpm
    ph_best, ph_val = 0.0, -1e9
    for ph in np.arange(0, lag, 0.5):
        pos = ph + lag * np.arange(int((n - ph) / lag) + 1)
        pos = pos[pos < n - 1].astype(int)
        v = env[pos].sum() / max(len(pos), 1)
        if v > ph_val:
            ph_val, ph_best = v, ph
    return float(bpm), float(best[0][0]), float(ph_best % lag)


def novelty(rms_db, flux, fps=100.0, bar=2.0):
    """按小节做能量/通量差分，给小节级结构变化打分。"""
    step = bar * fps
    nb = int(len(rms_db) / step)
    rows = []
    for b in range(nb):
        a, z = int(b * step), int((b + 1) * step)
        rows.append((rms_db[a:z].mean(), flux[a:z].mean()))
    out = []
    for b, (r, f) in enumerate(rows):
        if b == 0:
            out.append(0.0)
            continue
        pr, pf = rows[b - 1]
        out.append(abs(r - pr) * 1.0 + abs(f - pf) * 12.0)
    return rows, out


def main():
    path = sys.argv[1]
    out_json = sys.argv[2] if len(sys.argv) > 2 else None

    x = load_mono(path)
    dur = len(x) / SR
    S = stft_mag(x)
    fps = SR / HOP

    rms = np.sqrt((np.r_[0.0, np.cumsum(x * x)][WIN:] - np.r_[0.0, np.cumsum(x * x)][:-WIN])
                  / WIN) if False else None
    # 逐帧 RMS（与 STFT 帧对齐）
    n = S.shape[0]
    idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
    frames = x[idx]
    rms = np.sqrt((frames ** 2).mean(axis=1) + 1e-12)
    rdb = db(rms)

    flux = onset_env(S)
    sub = db(band_energy(S, SR, 20, 60) + 1e-9)
    low = db(band_energy(S, SR, 60, 250) + 1e-9)
    mid = db(band_energy(S, SR, 250, 2000) + 1e-9)
    hi = db(band_energy(S, SR, 2000, 8000) + 1e-9)
    air = db(band_energy(S, SR, 8000, 16000) + 1e-9)

    bpm, conf, phase = tempo(flux, fps)

    print("=" * 62)
    print("RHINE LAB.LLC  ·  音频结构分析")
    print("=" * 62)
    print("时长            %.3f s   (样本 %d @ %d Hz)" % (dur, len(x), SR))
    print("峰值            %.1f dBFS" % db(np.abs(x).max()))
    print("整体 RMS        %.1f dBFS" % db(np.sqrt((x ** 2).mean())))
    print("估计 BPM        %.2f   (自相关置信 %.3f)" % (bpm, conf))
    print("一拍            %.4f s     一小节(4/4) %.4f s" % (60 / bpm, 240 / bpm))
    print("相位偏移        %.3f s（第一拍落在该时刻 + k*拍长）" % (phase / fps))
    print("全曲小节数      %.2f" % (dur / (240 / bpm)))

    # ---- 首尾 ----
    thr = db(np.percentile(np.abs(x), 99.0)) - 40
    env_abs = np.abs(x)
    k = 4800
    sm = np.convolve(env_abs, np.ones(k, dtype=np.float32) / k, mode="same")
    above = sm > 10 ** (-55 / 20)
    t_first = float(np.argmax(above) / SR)
    t_last = float(len(above) - 1 - np.argmax(above[::-1])) / SR
    print("首有声          %.3f s" % t_first)
    print("末有声(-55dB)   %.3f s   → 尾部静音 %.2f s" % (t_last, dur - t_last))

    # ---- 最响的位置 ----
    sm2 = np.convolve(rms, np.ones(13) / 13, mode="same")
    peak_t = float(np.argmax(sm2) / fps)
    print("RMS 峰值        %.2f s  (%.1f dBFS)" % (peak_t, rdb.max()))

    # ---- 静音间隙 ----
    quiet = rdb < (np.percentile(rdb, 95) - 18)
    gaps, st = [], None
    for i, q in enumerate(quiet):
        if q and st is None:
            st = i
        elif not q and st is not None:
            if (i - st) / fps > 0.20:
                gaps.append((st / fps, i / fps))
            st = None
    print("静音/低能间隙   " + (", ".join("%.2f–%.2f" % g for g in gaps) if gaps else "无"))

    # ---- 小节表 ----
    bar = 240.0 / bpm
    nb = int(dur / bar)
    print("\n小节表（每小节 RMS / 低频 / 高频 / 通量）")
    print("  bar   t0      t1      RMS    sub    hi     flux")
    for b in range(nb):
        a, z = int(b * bar * fps), int((b + 1) * bar * fps)
        if z > len(rdb):
            z = len(rdb)
        print("  %2d  %6.2f  %6.2f  %6.1f  %6.1f  %6.1f  %6.2f"
              % (b + 1, a / fps, z / fps,
                 rdb[a:z].mean(), sub[a:z].mean(), hi[a:z].mean(), flux[a:z].mean()))

    data = {
        "duration": dur, "bpm": bpm, "conf": conf,
        "beat": 60 / bpm, "bar": bar, "phase": phase / fps,
        "first": t_first, "last": t_last, "peak_t": peak_t,
        "gaps": gaps,
        "rms_db": rdb.tolist(), "flux": flux.tolist(),
        "bands": {"sub": sub.tolist(), "low": low.tolist(),
                  "mid": mid.tolist(), "hi": hi.tolist(), "air": air.tolist()},
        "fps": fps,
    }
    if out_json:
        with open(out_json, "w") as f:
            json.dump(data, f)
        print("\nfeatures -> %s" % out_json)


if __name__ == "__main__":
    main()
