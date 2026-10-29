#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 build_board.py 从 v3（96BPM×28小节/70.0s，凭感觉）改成 v4（112BPM×33小节/70.71s，跟着音轨走）。
一次性补丁，跑完即弃。"""
import ast
import re
import sys

SRC = "build_board.py"
s = open(SRC, encoding="utf-8").read()
orig = s

BPM = 112.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT
NBARS = 33
TOTAL = NBARS * BAR

# ---------- 1. 取旧 SHOTS ----------
m = re.search(r"^SHOTS = \[\n(.*?)^\]\n", s, re.S | re.M)
old_shots = ast.literal_eval("[\n" + m.group(1) + "\n]")
assert len(old_shots) == 34, len(old_shots)

# ---------- 2. 新拍格 ----------
BEATS = {
    "1": (0, 3), "2": (3, 5), "3": (5, 8),
    "4": (8, 11), "5": (11, 14), "6a": (14, 15), "6b": (15, 16), "6c": (16, 17),
    "7": (17, 20), "8": (20, 24),
    "9": (24, 29), "10": (29, 32), "11": (32, 35), "12": (35, 38), "13": (38, 43),
    "14": (43, 45), "15": (45, 48),
    "16": (48, 51), "17": (51, 56), "18": (56, 60), "19": (60, 68), "20": (68, 74),
    "21": (74, 79), "22": (79, 84), "23": (84, 89), "24": (89, 94), "25": (94, 99),
    "26": (99, 105),
    "27": (105, 110), "28": (110, 115), "29": (115, 120),
    "30": (120, 125), "31": (125, 130), "32": (130, 132),
}
# 连续性自检
prev = 0
for sid, _, _, *_ in old_shots:
    b0, b1 = BEATS[sid]
    assert b0 == prev, (sid, b0, prev)
    prev = b1
assert prev == NBARS * 4, prev

lines = []
for sid, t0, t1, lens, sc, mv, tr, sd, tick in old_shots:
    b0, b1 = BEATS[sid]
    lines.append("    (%r, %8.3f, %8.3f, %r, %r, %r, %r, %r, %r)," % (
        sid, b0 * BEAT, b1 * BEAT, lens, sc, mv, tr, sd, tick))
new_shots_block = "SHOTS = [\n" + "\n".join(lines) + "\n]\n"
s = s[:m.start()] + new_shots_block + s[m.end():]

# ---------- 3. 常量与幕 ----------
s = s.replace("TOTAL = 70.0\n", "BPM = 112.0\nBEAT = 60.0 / BPM          # 0.5357s · 音轨实测心跳周期\nBAR = 4 * BEAT               # 2.1429s\nNBARS = 33\nTOTAL = NBARS * BAR          # 70.714s\n")


def bt(b):
    return b * BEAT


ACTS_NEW = """ACTS = [
    ("0", "尘", 0.000, %.3f, 0.06),
    ("1", "生", %.3f, %.3f, 0.34),
    ("2", "藏", %.3f, %.3f, 0.56),
    ("3", "溯", %.3f, %.3f, 0.78),
    ("4", "鸣", %.3f, %.3f, 1.00),
    ("5", "寂", %.3f, %.3f, 0.17),
    ("6", "铭", %.3f, %.3f, 0.05),
]
""" % (bt(8), bt(8), bt(24), bt(24), bt(48), bt(48), bt(74), bt(74), bt(105),
       bt(105), bt(120), bt(120), bt(132))
a = re.search(r"^ACTS = \[\n.*?^\]\n", s, re.S | re.M)
s = s[:a.start()] + ACTS_NEW + s[a.end():]

# ---------- 4. MORPHS 时间 ----------
for old, new in [("12.0s", "10.7s"), ("15.0s", "12.9s"), ("24.0s", "20.4s"),
                 ("45.0s", "42.3s"), ("55.0s", "53.0s"), ("62.5s", "61.6s")]:
    assert ('"%s",' % old) in s, old
s = s.replace('("M1", "12.0s"', '("M1", "10.7s"')
s = s.replace('("M2", "15.0s"', '("M2", "12.9s"')
s = s.replace('("M3", "24.0s"', '("M3", "20.4s"')
s = s.replace('("M4", "45.0s"', '("M4", "42.3s"')
s = s.replace('("M5", "55.0s"', '("M5", "53.0s"')
s = s.replace('("M6", "62.5s"', '("M6", "61.6s"')

# ---------- 5. 小节网格 ----------
s = s.replace("""    for i in range(28):
        t = i * 2.5
        w = 100.0 / 28""", """    for i in range(NBARS):
        t = i * BAR
        w = 100.0 / NBARS""")

# ---------- 6. 音频轨道 ----------
rms_d = open("_rms_path.txt").read().strip()
air_d = open("_air_path.txt").read().strip()
anchor = "PALETTE = ["
ins = '''# 实测包络（0–100 归一，120 点；来自 analyze_audio*.py + ffmpeg 解码）
AUDIO_RMS = ("%s")
AUDIO_AIR = ("%s")

PALETTE = [''' % (rms_d, air_d)
s = s.replace(anchor, ins, 1)

# ---------- 7. 新渲染函数 ----------
funcs = '''

def svg_aud_fill():
    pts = AUDIO_RMS[2:].split(" L ")
    d = ["M " + pts[0]]
    d += ["L " + p for p in pts[1:]]
    d += ["L 100 100", "L 0 100", "Z"]
    return " ".join(d)


def aud_marks():
    out = []
    for i in range(NBARS):
        major = " major" if i % 4 == 0 else ""
        out.append('<i class="am%s" style="left:%.4f%%"></i>' % (major, 100.0 * i / NBARS))
    out.append('<i class="am hinge" style="left:%.4f%%"></i>' % (100.0 * 12 / NBARS))
    return "".join(out)


def aud_facts():
    rows = [
        ("112.0 BPM", "一小节 2.143s / 一拍 0.536s（= 音轨的心跳周期）"),
        ("35 小节 = 75.0s", "成品取 33 小节 = 70.71s，删掉开头 2 小节 —— 那两小节是静止织体，删了听不出来"),
        ("没有小节感", "2 / 4 / 8 / 16 / 32 倍脉冲处起伏 &lt;1 dB —— 它没有重拍，所以画面别指望它给重拍"),
        ("RMS −11.4 dB", "0→74.5s 浮动 ±0.6 dB。没有前奏推进，没有 drop，没有副歌"),
        ("亮度 875→2576 Hz", "25.7s 处音色一次性打开，此后不再关 —— 全曲唯一的结构事件"),
        ("零音高材料", "长时平均谱无谐波峰。没有旋律、没有和声、没有一个能对上剪切的音符"),
        ("结尾 2.5s 淡出", "74.5s 起指数衰减到 −39 dB。这是它唯一给你的礼物"),
    ]
    return "".join("<div><b>%s</b><span>%s</span></div>" % (a_, b_) for a_, b_ in rows)

'''
s = s.replace("\ndef scales():", funcs + "\ndef scales():", 1)

# ---------- 8. 模板 ----------
s = s.replace("<div>总时长 <b>70.0s</b></div>", "<div>总时长 <b>70.71s</b></div>")
s = s.replace("<div>BPM <b>96</b></div>", "<div>BPM <b>112</b></div>")
s = s.replace("<div>小节 <b>28</b></div>", "<div>小节 <b>33</b></div>")
s = s.replace("Concept Promotional Film · Storyboard Board · v3 · 方向 A 档案 · 物性",
              "Concept Promotional Film · Storyboard Board · v4 · 方向 A · 跟着音轨重排")
s = s.replace('<div class="peak" style="left:82.1429%%"></div>',
              '<div class="peak" style="left:%.4f%%"></div>' % (100 * 99 * BEAT / TOTAL))
s = s.replace("""<div class="cap">34 个镜头块 · 苔绿 = 形变转场 · 橙 = 机械交互帧 · 虚线 = 57.5s 峰值，之后硬切</div>""",
              """<div class="cap">34 个镜头块 · 苔绿 = 形变转场 · 橙 = 机械交互帧 · 虚线 = 峰值（53.0s），之后画面进入凝止</div>""")
s = s.replace("<h2>第 19 镜 · 闪切十刀（每刀 0.3s / 12fps 步进）</h2>",
              "<h2>第 19 镜 · 闪切十刀（每刀 0.43s / 12fps 步进）</h2>")
s = s.replace("""  <span>96 BPM × 28 小节 = 70.0s · 能量断崖在 57.5s · 橙色仅属于机械交互</span>
  <span>RHINE MUSIC DATABASE / STORYBOARD v3</span>""",
              """  <span>112 BPM × 33 小节 = 70.71s · 音轨实测：无副歌、无音高、唯一转折 25.7s · 橙只属于机械交互</span>
  <span>RHINE MUSIC DATABASE / STORYBOARD v4</span>""")

# 插一页音频实测
aud_html = """
<h2>音频实测 · 这条音轨没有副歌</h2>
<div class="audbox">
  <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
    <defs><linearGradient id="ag" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#F2F7F4" stop-opacity=".20"/>
      <stop offset="1" stop-color="#F2F7F4" stop-opacity="0"/></linearGradient></defs>
    <path d="%(audfill)s" fill="url(#ag)"/>
    <path d="%(audrms)s" fill="none" stroke="#F2F7F4" stroke-opacity=".55"
          stroke-width=".6" vector-effect="non-scaling-stroke"/>
    <path d="%(audair)s" fill="none" stroke="#777B60" stroke-width="1.1"
          vector-effect="non-scaling-stroke"/>
    <path d="%(curve)s" fill="none" stroke="#E8622A" stroke-width="1.1"
          stroke-dasharray="2.5 1.8" vector-effect="non-scaling-stroke"/>
  </svg>
  %(audmarks)s
</div>
<div class="keyrow">
  <span><i class="k1"></i>实测 RMS 包络（能看到 0.536s 的心跳）</span>
  <span><i class="k2"></i>实测亮度 7–16 kHz 占比</span>
  <span><i class="k3"></i>画面能量（设计值）</span>
</div>
<div class="facts">%(audfacts)s</div>
<div class="cap moss">白线是这条音轨真正做的事：一条平的、密的、不变脸色的线。橙线是画面。两条线之间的全部距离，就是这支宣传片的作者性 —— 声音不给你情绪，画面必须自己长出来。</div>

<hr>
"""
s = s.replace('<h2>分镜 · 34 镜 / 42 剪辑点</h2>', aud_html + '\n<h2>分镜 · 34 镜 / 42 剪辑点</h2>')

# ---------- 9. CSS ----------
css = """
/* audio panel */
.audbox{position:relative;height:150px;background:#0A1016;
  border:1px solid rgba(231,231,225,.10);overflow:hidden}
.audbox svg{position:absolute;inset:0;width:100%;height:100%%;display:block}
.audbox .am{position:absolute;top:0;bottom:0;width:1px;background:rgba(231,231,225,.07)}
.audbox .am.major{background:rgba(231,231,225,.20)}
.audbox .am.hinge{background:#E8622A;opacity:.85;width:1.5px}
.keyrow{display:flex;gap:26px;flex-wrap:wrap;margin-top:9px;
  font:10px ui-monospace,Menlo,monospace;color:#8D9083;letter-spacing:.03em}
.keyrow i{display:inline-block;width:16px;height:0;border-top:2px solid;margin-right:7px;
  vertical-align:middle}
.keyrow .k1{border-color:#F2F7F4}
.keyrow .k2{border-color:#777B60}
.keyrow .k3{border-color:#E8622A;border-top-style:dashed}
.facts{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;margin-top:22px;
  background:rgba(231,231,225,.09);border:1px solid rgba(231,231,225,.09)}
.facts>div{background:#0C1319;padding:14px 15px;display:flex;flex-direction:column;gap:5px}
.facts b{font:11.5px ui-monospace,Menlo,monospace;color:#E7E7E1;letter-spacing:.02em}
.facts span{font-size:10.5px;line-height:1.55;color:#8D9083}
@media (max-width:1000px){.facts{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media (max-width:600px){.facts{grid-template-columns:1fr}}
"""
s = s.replace("@media (max-width:1280px){", css + "\n@media (max-width:1280px){", 1)

# ---------- 10. 渲染字典 ----------
s = s.replace('    "palette": palette(),', '    "audfill": svg_aud_fill(),\n'
              '    "audrms": AUDIO_RMS,\n'
              '    "audair": AUDIO_AIR,\n'
              '    "audmarks": aud_marks(),\n'
              '    "audfacts": aud_facts(),\n'
              '    "palette": palette(),')

assert s != orig
open(SRC, "w", encoding="utf-8").write(s)
print("patched build_board.py  (%.1f KB -> %.1f KB)" % (len(orig) / 1024, len(s) / 1024))
print("TOTAL = %.4f  BPM=%.1f  bars=%d" % (TOTAL, BPM, NBARS))
