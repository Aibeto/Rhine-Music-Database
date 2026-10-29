#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a single-file HTML storyboard board from the storyboard data.
Run: python3 build_board.py > rhine-concept-pv-70s-board.html
"""
import html

BPM = 112.0
BEAT = 60.0 / BPM          # 0.5357s · 音轨实测心跳周期
BAR = 4 * BEAT               # 2.1429s
NBARS = 33
TOTAL = NBARS * BAR          # 70.714s

ACTS = [
    ("0", "尘", 0.000, 4.286, 0.06),
    ("1", "生", 4.286, 12.857, 0.34),
    ("2", "藏", 12.857, 25.714, 0.56),
    ("3", "溯", 25.714, 39.643, 0.78),
    ("4", "鸣", 39.643, 56.250, 1.00),
    ("5", "寂", 56.250, 64.286, 0.17),
    ("6", "铭", 64.286, 70.714, 0.05),
]

# id, t0, t1, lens, scene, move, transition, sound, tick
SHOTS = [
    ('1',    0.000,    1.607, '100mm 微距', '全黑。一束极窄光柱斜切画面，三粒尘埃漂浮，只有一粒在焦平面上', '0.5cm/s 横移', '黑场起', '磁带底噪、空气声', None),
    ('2',    1.607,    2.679, '100mm 拉焦', '气流吹散尘埃，露出一个巨大圆盘的一角——它在反光，但反的是什么看不清', '焦点由前拉后', '硬切', '40Hz 心跳，3s 混响', None),
    ('3',    2.679,    4.286, '100mm 大特写', '盘面浮出第一道同心刻痕，慢转；表面有划痕、指纹残留', '盘转 90°', '硬切', '针尖悬停的金属「嘶」', None),
    ('4',    4.286,    5.893, '100mm 微距', '一块六棱柱晶体在电场中析出，棱面各映着不同的模糊之物', '贴面滑行', '硬切', '高频电流 + 生长「叮」', None),
    ('5',    5.893,    7.500, '50mm 中景', '拉远——那不是晶体，是一摞唱片堆成的柱体，正在黑暗里升起，尘埃环绕', '快速拉远 + 仰角', '硬切', '低频进入，脉冲起', 1),
    ('6a',    7.500,    8.036, '微距', '针尖落到旋转盘面', '12fps 步进', '橙闪 1 帧', '咔', None),
    ('6b',    8.036,    8.571, '微距', '盘面划痕像峡谷，光从谷底掠过', '12fps 步进', '硬切', '咔', None),
    ('6c',    8.571,    9.107, '25mm', '一道光扫过盘面，反出网格', '急摇', '硬切', '碎裂声', None),
    ('7',    9.107,   10.714, '100mm 俯拍', '克拉尼：细沙撒在转动的盘面上，随低频跳成花瓣状驻波；图形缓慢与刻痕重合', '极慢推', 'M1 起', '沙粒滚动的细响', None),
    ('8',   10.714,   12.857, '25mm 推镜', '摄影机坠入克拉尼图形最内圈，圈即孔；黑吞掉画面，同时透出白光', '指数加速推进', 'M1 环→隧道', '反向混响渐强，过曝前 0.2s 全静', None),
    ('9',   12.857,   15.536, '25mm 大全景', '白光洗出米白档案架，等距，延伸到无穷远；盒侧有不可读微缩字与色标', '通道内 6m/s 穿行', 'M2 白→纸白', '弦乐进入', 2),
    ('10',   15.536,   17.143, '35mm 中景', '掠过盒侧，图案一闪而过；每个盒子内部有微光', '侧向平移', '硬切', '纸摩擦 + 机械咔哒 0.5s/次', None),
    ('11',   17.143,   18.750, '50mm', '一个盒子正面化成薄光幕，幕里闪过四个世界：海面 / 沙丘 / 燃烧的城市 / 雪原', '机身定格，幕内闪切', '硬切', '四下短促的「呼」', None),
    ('12',   18.750,   20.357, '25mm 仰拍', '盒边拉长成数据柱，柱顶浮起光点，光点排成同心的环', '上摇 + 升镜', 'M3 起', '低频上扬', None),
    ('13',   20.357,   23.036, '25mm 俯拍大全景', '档案库显形为巨大唱盘的同心圆阵列，货架即纹路；一道光环沿纹路走一圈', '缓慢旋转 30°', 'M3 柱→纹', '全频铺开', 3),
    ('14',   23.036,   24.107, '100mm 侧光', '一张唱片侧立，薄如纸，光从侧面穿透，边缘发绿，内部纤维可见', '静止', '硬切', '单音', None),
    ('15',   24.107,   25.714, '25mm 低角度', '唱片以 45° 倒下，像第一张多米诺，拍在地面激起一圈尘浪', '快速下摇跟随', '硬切', '金属落地 + 3s 混响', None),
    ('16',   25.714,   27.321, '25mm', '尘浪卷起，摄影机被卷入粒子云，画面变成流动的灰白', '失重翻滚', '硬切', '噪声墙', None),
    ('17',   27.321,   30.000, '100mm→25mm', '粒子云里浮出一只眼睛，角膜上是唱片纹路；瞳孔收缩，像镜头光圈', '变焦+推轨，推进瞳孔', '硬切', '心跳 + 极细高频', 4),
    ('18',   30.000,   32.143, '25mm 隧道', '穿过瞳孔是一条隧道，四壁是竖直的频谱柱，随音乐起伏', '高速前进，透视拉长', '硬切', '律动全开', None),
    ('19',   32.143,   36.429, '混剪 10 刀', '见闪切表 19a–19j', '12fps 步进', '10 次硬切', '每刀一记打击，共 10 记', None),
    ('20',   36.429,   39.643, '50mm 环摇', '骤停：纯黑房间，中央悬浮一张发光唱片，尘土慢速飘', '缓慢环绕 15°', '硬切（静）', '近乎静默，只有耳机漏音的低频', None),
    ('21',   39.643,   42.321, '50mm', '唱片猛然加速，甩出一圈光带；光带在空中展开成白色细线网格', '快速拉远露出网格', 'M4 起', '加速的嗡鸣', 5),
    ('22',   42.321,   45.000, '25mm', '细线开始搭建 UI：标题栏、分割线、列、伪字形文本块，像全息在空间里组装', '缓慢横移穿过结构', 'M4 带→格', '每落位一声「嗒」（共 12 声）', None),
    ('23',   45.000,   47.679, '25mm 穿行', '摄影机穿过全息 UI，穿过处被切开，剖面发橄榄绿', '直线穿行', '硬切', '低频扫频', None),
    ('24',   47.679,   50.357, '18mm 广角全景', 'UI 与建筑混合：书架即列表，列表即书架；中央悬唱片，下方是一个纯黑的、播放键形状的洞', '大范围横移', '硬切', '全频', None),
    ('25',   50.357,   53.036, '25mm', '一束光自己去按那个洞（没有手）；唱片起转，声波以球面扩散，粒子被推开', '冲击波把镜头推开', 'M5', '峰值 · 橙闪 3 帧', None),
    ('26',   53.036,   56.250, '—', '白光溢出全屏，峰值；白中淡出一个极小的黑点', '过曝', 'M5 → 硬切（不淡出）', '峰值 → 静', None),
    ('27',   56.250,   58.929, '100mm 微距', '回到开场的光柱与尘埃，但这次尘埃是静止的', '完全静止', '硬切', '全静默，只剩耳鸣般高频', None),
    ('28',   58.929,   61.607, '50mm', '光柱里悬着唱片，缓慢自转；背景是虚焦的米白档案架', '极慢环绕', '硬切', '单音（正弦/钢琴）', 6),
    ('29',   61.607,   64.286, '25mm', '摄影机后退，唱片缩小成一个点，落在画面正中偏下', '匀速后退', 'M6 盘→点', '混响尾巴', None),
    ('30',   64.286,   66.964, '图形', '黑场。那个点展开成一条 1px 横线——它就是刻度尺的第 7 格；左端出现方块标记', '—', '硬切 · 橙标记', '一声极轻的「叮」', 7),
    ('31',   66.964,   69.643, '图形', '横线上方浮出字标，无动画，直接出现，字距 +0.3em', '无', '—', '静', None),
    ('32',   69.643,   70.714, '图形', '黑场，只剩那条横线；0.2s 后线也消失', '—', '黑场', '干净地断掉，不淡出', None),
]

MORPHS = [
    ("M1", "10.7s", "克拉尼内圈 → 隧道入口", "圆的边缘向纵深拉伸成管壁；内圈半径不变，外圈被拉长", "3.0s"),
    ("M2", "12.9s", "过曝白 → 纸白档案库", "曝光回落时，白色里先浮雕出盒子的边缘阴影", "2.0s"),
    ("M3", "20.4s", "数据柱 → 唱片纹路", "升到俯视角，柱顶光点连成同心环，柱退化为平面上的线", "2.5s"),
    ("M4", "42.3s", "光带 → UI 网格", "光带展开时不是散开，是被拉直；直线交点先出现，再长出细线", "2.5s"),
    ("M5", "53.0s", "声波球面 → 全白", "球面推到镜头位置时不是穿过，是贴满整个画面，然后过曝", "2.5s"),
    ("M6", "61.6s", "盘 → 点", "唱片不是缩小，是翻到侧面成一条线，再缩成一个点", "2.5s"),
]

FRAMES = [
    ("K1", "第 3 镜 · 刻痕", "圆盘边缘在左下占 1/4，其余是黑。100mm T2.0 俯角 15°。拉丝铝 + 同心车纹，留 3 处指纹、2 条发丝划痕——全片唯一一次「人」的痕迹。", "它已经在这里很久了。"),
    ("K2", "第 7 镜 · 克拉尼", "俯拍全圆，沙在低频下跳成六瓣/八瓣驻波，边缘的沙在「走」。干燥颗粒，每颗都在跳——不能做成流体噪声，这是最容易翻车的一镜。", "声音有了形状。"),
    ("K3", "第 13 镜 · 档案库即唱盘", "俯拍 90°，同心圆阵列充满画面，圆心偏右下。25mm T5.6 高度 40m。聚酯白盒阵列，每 12 个盒子一次色差。", "货架就是纹路。"),
    ("K4", "第 17 镜 · 眼", "眼占 80%，瞳孔略偏。100mm 变焦+推轨切 25mm 制造透视畸变。角膜叠加唱片同心纹路（法线位移，不是贴图）。", "它在看你。"),
    ("K5", "第 22 镜 · 界面组装", "三层景深：前景线框虚焦、中景组装中的 UI、背景唱片。氧化玻璃厚 2mm，有气泡与污渍。每元素 0.4s 缓入落位，落位瞬间 0.06s 过曝。", "它在被搭建出来。"),
    ("K6", "第 24 镜 · 书架即列表", "18mm 广角，透视线从四角收向中心唱片。T5.6 高度 1.6m（人眼）。全片唯一「两种物质同时出现」的镜头。", "这不是软件，这是一个地方。"),
    ("K7", "第 28 镜 · 光柱里的唱片", "唱片占 12%，光柱从上方斜切。50mm T2.0 环绕 5°/s。喷砂铝 + 薄尘。尘埃静止，和开场形成镜像。", "它一直在那儿。"),
    ("K8", "第 30–31 镜 · 落款", "横线在垂直中线略下，长 18% 画面宽；左端方块 8px 橙色。极细 grotesque，字距 +0.3em。无动画，直接 cut in。", "报告写完了。"),
]

FLASH = [
    ("19a", "玻璃杯里的水因声波起涟漪", "实拍微距"),
    ("19b", "唱针针尖，反着光", "实拍微距"),
    ("19c", "黑暗中一双手翻动唱片，只见手与光", "全片唯一一次出现「人」"),
    ("19d", "老式示波器的绿线", "实拍 CRT，呼应苔绿"),
    ("19e", "那只眼睛闭上", "与第 17 镜同一只眼"),
    ("19f", "磁带被拉出一截，绞成一团", "磁带盒未来主义"),
    ("19g", "城市楼群像均衡器一样起伏", "3D"),
    ("19h", "一滴水落在光盘上，涟漪扩散成同心圆", "和克拉尼同构"),
    ("19i", "档案盒一个个自动弹开（俯拍）", "3D"),
    ("19j", "唱片中心孔正对镜头，孔里是星空", "与第 2 镜呼应"),
]

# 实测包络（0–100 归一，120 点；来自 analyze_audio*.py + ffmpeg 解码）
AUDIO_RMS = ("M 0.00 87.31 L 0.38 100.00 L 0.76 87.00 L 1.14 99.96 L 1.52 87.10 L 1.90 100.00 L 2.28 86.46 L 2.66 99.81 L 3.04 87.03 L 3.42 100.00 L 3.80 86.82 L 4.18 100.00 L 4.56 86.80 L 4.94 99.93 L 5.32 87.28 L 5.70 100.00 L 6.08 87.01 L 6.46 99.86 L 6.84 87.02 L 7.22 99.93 L 7.60 86.83 L 7.98 99.94 L 8.37 86.80 L 8.75 100.00 L 9.13 87.09 L 9.51 99.99 L 9.89 86.93 L 10.27 100.00 L 10.65 87.17 L 11.03 100.00 L 11.41 86.70 L 11.79 99.92 L 12.17 87.26 L 12.55 100.00 L 12.93 87.13 L 13.31 100.00 L 13.69 86.97 L 14.07 100.00 L 14.45 87.16 L 14.83 100.00 L 15.21 86.72 L 15.59 100.00 L 15.97 87.02 L 16.35 100.00 L 16.73 86.76 L 17.11 99.70 L 17.49 87.06 L 17.87 100.00 L 18.25 86.99 L 18.63 99.69 L 19.01 86.70 L 19.39 99.53 L 19.77 86.83 L 20.15 99.93 L 20.53 86.65 L 20.91 99.99 L 21.29 87.31 L 21.67 100.00 L 22.05 87.09 L 22.43 100.00 L 22.81 86.92 L 23.19 99.84 L 23.57 87.07 L 23.95 99.89 L 24.33 87.00 L 24.71 100.00 L 25.10 86.72 L 25.48 100.00 L 25.86 86.86 L 26.24 100.00 L 26.62 86.70 L 27.00 100.00 L 27.38 86.62 L 27.76 99.99 L 28.14 86.97 L 28.52 100.00 L 28.90 87.02 L 29.28 100.00 L 29.66 87.13 L 30.04 100.00 L 30.42 86.77 L 30.80 100.00 L 31.18 86.88 L 31.56 99.99 L 31.94 86.71 L 32.32 100.00 L 32.70 86.68 L 33.08 100.00 L 33.46 87.30 L 33.84 100.00 L 34.22 86.73 L 34.60 100.00 L 34.98 86.80 L 35.36 99.61 L 35.74 86.56 L 36.12 99.94 L 36.50 86.97 L 36.88 100.00 L 37.26 87.24 L 37.64 99.92 L 38.02 86.74 L 38.40 100.00 L 38.78 86.93 L 39.16 100.00 L 39.54 86.83 L 39.92 100.00 L 40.30 87.11 L 40.68 100.00 L 41.06 86.60 L 41.44 100.00 L 41.83 86.99 L 42.21 100.00 L 42.59 86.76 L 42.97 99.82 L 43.35 86.64 L 43.73 99.89 L 44.11 87.04 L 44.49 99.83 L 44.87 86.78 L 45.25 99.92 L 45.63 86.89 L 46.01 100.00 L 46.39 87.18 L 46.77 100.00 L 47.15 86.66 L 47.53 100.00 L 47.91 87.24 L 48.29 99.72 L 48.67 86.61 L 49.05 100.00 L 49.43 86.92 L 49.81 100.00 L 50.19 86.55 L 50.57 100.00 L 50.95 86.85 L 51.33 99.91 L 51.71 86.49 L 52.09 100.00 L 52.47 86.88 L 52.85 99.69 L 53.23 87.14 L 53.61 99.93 L 53.99 86.72 L 54.37 99.90 L 54.75 86.97 L 55.13 100.00 L 55.51 87.15 L 55.89 100.00 L 56.27 87.02 L 56.65 100.00 L 57.03 86.83 L 57.41 100.00 L 57.79 86.61 L 58.17 99.84 L 58.56 87.39 L 58.94 99.93 L 59.32 86.88 L 59.70 99.98 L 60.08 87.03 L 60.46 99.88 L 60.84 86.92 L 61.22 100.00 L 61.60 86.84 L 61.98 100.00 L 62.36 87.15 L 62.74 100.00 L 63.12 86.70 L 63.50 100.00 L 63.88 86.89 L 64.26 100.00 L 64.64 87.04 L 65.02 100.00 L 65.40 87.12 L 65.78 100.00 L 66.16 86.85 L 66.54 100.00 L 66.92 87.08 L 67.30 99.99 L 67.68 86.84 L 68.06 99.63 L 68.44 86.79 L 68.82 99.47 L 69.20 86.79 L 69.58 99.98 L 69.96 86.85 L 70.34 99.58 L 70.72 86.64 L 71.10 99.94 L 71.48 86.93 L 71.86 99.87 L 72.24 87.02 L 72.62 100.00 L 73.00 86.07 L 73.38 99.98 L 73.76 86.96 L 74.14 99.92 L 74.52 86.94 L 74.90 99.89 L 75.29 87.19 L 75.67 100.00 L 76.05 87.49 L 76.43 99.90 L 76.81 87.48 L 77.19 99.89 L 77.57 86.59 L 77.95 99.92 L 78.33 86.92 L 78.71 100.00 L 79.09 86.70 L 79.47 99.67 L 79.85 87.49 L 80.23 99.63 L 80.61 86.45 L 80.99 99.84 L 81.37 87.35 L 81.75 100.00 L 82.13 87.19 L 82.51 99.43 L 82.89 87.49 L 83.27 99.66 L 83.65 86.41 L 84.03 100.00 L 84.41 86.38 L 84.79 100.00 L 85.17 87.81 L 85.55 100.00 L 85.93 87.09 L 86.31 99.81 L 86.69 86.68 L 87.07 99.62 L 87.45 86.83 L 87.83 99.98 L 88.21 88.76 L 88.59 100.00 L 88.97 87.60 L 89.35 99.69 L 89.73 86.00 L 90.11 100.00 L 90.49 86.79 L 90.87 100.00 L 91.25 86.99 L 91.63 99.44 L 92.02 87.40 L 92.40 99.65 L 92.78 86.11 L 93.16 99.75 L 93.54 86.64 L 93.92 99.87 L 94.30 87.31 L 94.68 99.45 L 95.06 86.74 L 95.44 99.52 L 95.82 87.65 L 96.20 100.00 L 96.58 87.01 L 96.96 99.81 L 97.34 87.83 L 97.72 100.00 L 98.10 86.41 L 98.48 99.41 L 98.86 86.82 L 99.24 99.59 L 99.62 86.51 L 100.00 99.79")
AUDIO_AIR = ("M 0.00 67.80 L 0.38 67.80 L 0.76 67.04 L 1.14 67.50 L 1.52 67.45 L 1.90 68.47 L 2.28 69.69 L 2.66 71.06 L 3.04 70.93 L 3.42 71.29 L 3.80 71.45 L 4.18 72.23 L 4.56 71.57 L 4.94 70.91 L 5.32 70.37 L 5.70 70.80 L 6.08 70.76 L 6.46 71.06 L 6.84 71.59 L 7.22 72.46 L 7.60 72.28 L 7.98 72.05 L 8.37 72.27 L 8.75 73.45 L 9.13 73.24 L 9.51 73.17 L 9.89 73.93 L 10.27 76.00 L 10.65 76.00 L 11.03 76.00 L 11.41 76.00 L 11.79 76.00 L 12.17 76.00 L 12.55 75.60 L 12.93 72.36 L 13.31 71.75 L 13.69 71.59 L 14.07 71.52 L 14.45 71.97 L 14.83 72.94 L 15.21 72.48 L 15.59 72.24 L 15.97 71.49 L 16.35 68.30 L 16.73 60.86 L 17.11 51.05 L 17.49 41.26 L 17.87 37.59 L 18.25 41.82 L 18.63 47.69 L 19.01 54.27 L 19.39 60.81 L 19.77 63.13 L 20.15 65.65 L 20.53 68.14 L 20.91 69.72 L 21.29 69.56 L 21.67 69.01 L 22.05 68.50 L 22.43 69.01 L 22.81 68.94 L 23.19 68.56 L 23.57 67.99 L 23.95 67.99 L 24.33 67.46 L 24.71 66.35 L 25.10 64.83 L 25.48 64.50 L 25.86 64.29 L 26.24 64.64 L 26.62 65.36 L 27.00 66.41 L 27.38 66.03 L 27.76 64.92 L 28.14 63.93 L 28.52 63.92 L 28.90 63.54 L 29.28 62.39 L 29.66 61.20 L 30.04 61.07 L 30.42 60.56 L 30.80 60.37 L 31.18 58.99 L 31.56 58.15 L 31.94 57.84 L 32.32 56.61 L 32.70 54.74 L 33.08 54.25 L 33.46 53.72 L 33.84 53.14 L 34.22 52.09 L 34.60 51.42 L 34.98 50.81 L 35.36 50.16 L 35.74 48.67 L 36.12 48.27 L 36.50 47.28 L 36.88 46.94 L 37.26 45.70 L 37.64 44.98 L 38.02 44.20 L 38.40 43.35 L 38.78 41.82 L 39.16 40.82 L 39.54 39.89 L 39.92 39.58 L 40.30 39.44 L 40.68 39.39 L 41.06 35.13 L 41.44 32.52 L 41.83 29.34 L 42.21 27.60 L 42.59 29.24 L 42.97 29.91 L 43.35 29.70 L 43.73 29.51 L 44.11 28.69 L 44.49 28.08 L 44.87 27.53 L 45.25 28.32 L 45.63 29.07 L 46.01 30.73 L 46.39 32.55 L 46.77 34.00 L 47.15 35.27 L 47.53 36.62 L 47.91 37.19 L 48.29 38.30 L 48.67 38.21 L 49.05 39.13 L 49.43 39.40 L 49.81 39.97 L 50.19 40.11 L 50.57 40.69 L 50.95 40.49 L 51.33 39.97 L 51.71 36.07 L 52.09 31.95 L 52.47 26.92 L 52.85 23.85 L 53.23 23.61 L 53.61 24.36 L 53.99 25.36 L 54.37 26.80 L 54.75 28.06 L 55.13 29.80 L 55.51 31.44 L 55.89 33.22 L 56.27 34.93 L 56.65 36.45 L 57.03 37.35 L 57.41 38.20 L 57.79 35.47 L 58.17 32.50 L 58.56 28.27 L 58.94 25.48 L 59.32 24.61 L 59.70 25.47 L 60.08 26.57 L 60.46 27.95 L 60.84 27.67 L 61.22 27.32 L 61.60 25.61 L 61.98 24.41 L 62.36 23.84 L 62.74 24.09 L 63.12 24.38 L 63.50 25.28 L 63.88 26.50 L 64.26 28.13 L 64.64 30.06 L 65.02 31.48 L 65.40 30.16 L 65.78 29.21 L 66.16 27.61 L 66.54 25.52 L 66.92 24.55 L 67.30 23.45 L 67.68 21.89 L 68.06 21.56 L 68.44 21.08 L 68.82 21.31 L 69.20 21.29 L 69.58 22.31 L 69.96 24.43 L 70.34 26.43 L 70.72 27.41 L 71.10 29.19 L 71.48 29.41 L 71.86 29.55 L 72.24 28.42 L 72.62 26.93 L 73.00 25.06 L 73.38 24.55 L 73.76 25.19 L 74.14 25.77 L 74.52 25.60 L 74.90 26.60 L 75.29 26.44 L 75.67 26.84 L 76.05 26.60 L 76.43 24.94 L 76.81 22.98 L 77.19 21.39 L 77.57 20.29 L 77.95 20.51 L 78.33 20.45 L 78.71 21.26 L 79.09 21.62 L 79.47 22.67 L 79.85 23.95 L 80.23 24.12 L 80.61 23.99 L 80.99 24.53 L 81.37 24.52 L 81.75 25.83 L 82.13 26.56 L 82.51 26.29 L 82.89 25.38 L 83.27 24.43 L 83.65 23.24 L 84.03 23.06 L 84.41 22.50 L 84.79 22.89 L 85.17 22.26 L 85.55 22.31 L 85.93 22.38 L 86.31 21.84 L 86.69 21.56 L 87.07 21.94 L 87.45 21.82 L 87.83 22.94 L 88.21 24.53 L 88.59 26.04 L 88.97 26.95 L 89.35 27.53 L 89.73 25.91 L 90.11 24.64 L 90.49 23.34 L 90.87 22.47 L 91.25 22.35 L 91.63 21.90 L 92.02 21.62 L 92.40 20.69 L 92.78 19.68 L 93.16 19.45 L 93.54 18.65 L 93.92 18.72 L 94.30 18.45 L 94.68 18.36 L 95.06 17.98 L 95.44 17.32 L 95.82 16.28 L 96.20 15.29 L 96.58 14.02 L 96.96 14.04 L 97.34 14.00 L 97.72 14.42 L 98.10 15.15 L 98.48 15.04 L 98.86 14.29 L 99.24 14.00 L 99.62 14.00 L 100.00 14.00")

PALETTE = [
    ("#E7E7E1", "纸白", "档案库 / 光 / 片尾", "全片"),
    ("#141613", "墨", "字 / 线 / 剪影", "全片"),
    ("#777B60", "苔绿", "只做暗部反射", "全片"),
    ("#F2F7F4", "冷白", "唯一发光体", "全片"),
    ("#E8622A", "莱茵橙", "只属于机械交互", "仅 3 次 · 每次 ≤3 帧"),
]


def esc(s):
    return html.escape(str(s), quote=True)


def x(t):
    return 100.0 * t / TOTAL


def energy_path():
    pts = []
    for _, _, a, b, e in ACTS:
        pts.append((x(a), 100 - e * 100))
        pts.append((x(b), 100 - e * 100))
    return pts


def svg_curve():
    pts = energy_path()
    d = ["M %.3f %.3f" % pts[0]]
    for px, py in pts[1:]:
        d.append("L %.3f %.3f" % (px, py))
    return " ".join(d)


def svg_fill():
    pts = energy_path()
    d = ["M %.3f 100" % pts[0][0]]
    for px, py in pts:
        d.append("L %.3f %.3f" % (px, py))
    d.append("L %.3f 100 Z" % pts[-1][0])
    return " ".join(d)


def bars():
    out = []
    for i in range(NBARS):
        t = i * BAR
        w = 100.0 / NBARS
        cls = "bar whole" if i % 4 == 0 else "bar"
        out.append('<i class="%s" style="left:%.4f%%;width:%.4f%%"></i>' % (cls, x(t), w))
    return "".join(out)


def act_spans():
    out = []
    for code, name, a, b, e in ACTS:
        out.append(
            '<div class="act" style="left:%.4f%%;width:%.4f%%">'
            '<div class="actName"><b>%s</b><span>%s</span></div>'
            '<div class="actMeta">%.1fs</div></div>' % (x(a), x(b - a), code, name, b - a)
        )
    return "".join(out)


def act_band():
    out = []
    for code, name, a, b, e in ACTS:
        out.append('<i class="band b%d" style="left:%.4f%%;width:%.4f%%"></i>' % (int(code), x(a), x(b - a)))
    return "".join(out)


def shot_strip():
    out = []
    for sid, t0, t1, lens, sc, mv, tr, sd, tick in SHOTS:
        e = "M" in tr
        o = "橙" in tr or "橙" in sd
        cls = "cell" + (" morph" if e and "M" in tr and tr.startswith("M") else "") + (" orange" if o else "")
        out.append(
            '<div class="%s" style="left:%.4f%%;width:%.4f%%" title="镜%s · %.1f–%.1fs · %s">'
            '<span>%s</span></div>' % (cls, x(t0), x(t1 - t0), sid, t0, t1, esc(lens), esc(sid))
        )
    return "".join(out)


def ruler():
    """The 1px scale bar that recurs 7 times, one notch higher each time.
    Tick 7 IS the end-card horizontal rule."""
    out = []
    ticks = [(sid, (t0 + t1) / 2.0, n) for sid, t0, t1, *_r, n in SHOTS if n]
    for sid, mid, n in ticks:
        h = 5 + n * 4.2
        out.append(
            '<div class="rk" style="left:%.4f%%;height:%.1fpx"><b>%d</b></div>'
            % (x(mid), h, n)
        )
    return "".join(out)


def cards():
    out = []
    for sid, t0, t1, lens, sc, mv, tr, sd, tick in SHOTS:
        dur = t1 - t0
        trans_cls = "tv-cut"
        if tr.startswith("M"):
            trans_cls = "tv-morph"
        if "橙" in tr or "橙" in sd:
            trans_cls = "tv-orange"
        if "黑场" in tr:
            trans_cls = "tv-black"
        tickbadge = ('<span class="tick">刻度 %d</span>' % tick) if tick else ""
        out.append("""
<article class="card">
  <header>
    <span class="num">%s</span>
    <span class="tc">%s – %s</span>
    <span class="dur">%.1fs</span>
    %s
  </header>
  <div class="lens">%s</div>
  <p class="scene">%s</p>
  <div class="rows">
    <div><b>运动</b><span>%s</span></div>
    <div><b>转出</b><span class="tw %s">%s</span></div>
    <div><b>声音</b><span>%s</span></div>
  </div>
</article>""" % (
            esc(sid), "%.1fs" % t0, "%.1fs" % t1, dur, tickbadge,
            esc(lens), esc(sc), esc(mv), trans_cls, esc(tr), esc(sd),
        ))
    return "".join(out)


def morphs():
    out = []
    for code, at, fr, how, dur in MORPHS:
        out.append("""
<div class="morph">
  <div class="mh"><b>%s</b><span>%s</span></div>
  <div class="mt">%s</div>
  <p>%s</p>
  <div class="md">%s</div>
</div>""" % (code, at, esc(fr), esc(how), dur))
    return "".join(out)


def frames():
    out = []
    for code, title, spec, mood in FRAMES:
        out.append("""
<div class="frame">
  <div class="fh"><b>%s</b><span>%s</span></div>
  <p>%s</p>
  <q>%s</q>
</div>""" % (code, esc(title), esc(spec), esc(mood)))
    return "".join(out)


def flash():
    out = []
    for code, what, note in FLASH:
        out.append('<li><b>%s</b><span>%s</span><i>%s</i></li>' % (code, esc(what), esc(note)))
    return "".join(out)


def palette():
    out = []
    for hexv, name, use, freq in PALETTE:
        out.append(
            '<div class="sw"><i style="background:%s"></i><div><b>%s</b><span>%s</span><small>%s</small></div><code>%s</code></div>'
            % (hexv, esc(name), esc(use), esc(freq), hexv)
        )
    return "".join(out)



MATERIALS = [
    ("M1", "喷砂铝", "唱盘 · 针 · 机械件", [
        ("base color", "#B8BCB6 · linear 0.52–0.58，微冷"),
        ("roughness", "0.42–0.50　亚光"),
        ("anisotropic", "0.30，沿车削同心圆"),
        ("normal", "车纹 0.08 + 细颗粒 0.03"),
        ("edge wear", "0.5–1.5mm，曲率遮罩，不许均匀"),
        ("指纹", "3 处，roughness 降到 0.25 的油脂层"),
    ], "镜面反射 / clearcoat / 任何镀铬感"),
    ("M2", "聚酯白", "档案盒外壳", [
        ("base color", "#E7E7E1 · linear ≈ 0.79"),
        ("roughness", "0.55，均匀，不要噪声"),
        ("coat", "0.15 / coat-rough 0.35　薄涂层"),
        ("transmission", "0.02，仅边缘 0.3mm 透光"),
        ("积灰", "底部 15% 高度，AO 驱动"),
    ], "白值过曝 —— 暗环境锁 0.72–0.80"),
    ("M3", "纸浆", "盒内 · 标签", [
        ("roughness", "0.85 · #D9D8CE"),
        ("normal", "纤维各向异性 0.06"),
        ("碎边", "0.2mm displacement"),
        ("备注", "起毛必须靠位移，贴图做不出来"),
    ], "用贴图假装纸纤维"),
    ("M4", "半透树脂", "晶体", [
        ("IOR / trans", "1.49 / 0.85"),
        ("roughness", "0.12"),
        ("色散", "0.02　宁少勿多"),
        ("体积散射", "density 0.02，色微暖"),
        ("内含", "气泡 3–5 个 + 悬浮杂质"),
    ], "色散 >0.1 就变成水晶球"),
    ("M5", "氧化玻璃", "全息 UI", [
        ("厚度", "2mm，边缘染苔绿"),
        ("亮度", "0.3 —— 靠环境反射才看得见"),
        ("表面", "气泡 3–5 个，污渍 2 处"),
        ("备注", "全片唯一的「高科技」来源"),
    ], "自发光拉满 = 网页 / PPT"),
    ("M6", "尘埃", "全片的时间", [
        ("粒径", "0.3–2mm，不均匀分布"),
        ("景深", "仅 3% 在焦平面"),
        ("运动", "布朗 + 0.02m/s 上升气流"),
        ("备注", "第 27 镜静止，且必须是同一批粒子"),
    ], "换了粒子 = 换了时间线"),
]


def materials():
    out = []
    for code, name, use, rows, ban in MATERIALS:
        body = "".join(
            "<div><dt>%s</dt><dd>%s</dd></div>" % (esc(k), esc(v)) for k, v in rows
        )
        out.append(
            '<div class="mat"><div class="mh"><b>%s</b><span>%s</span><i>%s</i></div>'
            "<dl>%s</dl><p class=\"ban\"><s>\u2715</s>%s</p></div>"
            % (code, esc(name), esc(use), body, esc(ban))
        )
    return "".join(out)


CSS = """
*{box-sizing:border-box;margin:0;padding:0}
html{background:#080C10;color:#E7E7E1}
body{font:14px/1.65 system-ui,"Noto Sans SC","PingFang SC","Microsoft YaHei",sans-serif;
  background:#080C10;padding:48px 40px 96px;max-width:1560px;margin:0 auto;
  -webkit-font-smoothing:antialiased}
code,.mono,header .tc,header .num,.lens,.actMeta,.md,.sw code{
  font-family:ui-monospace,"Cascadia Mono",Menlo,Consolas,monospace}
hr{border:0;border-top:1px solid rgba(231,231,225,.10);margin:56px 0}
h1{font-size:15px;font-weight:500;letter-spacing:.34em;text-transform:uppercase;
  color:#E7E7E1;font-family:ui-monospace,Menlo,Consolas,monospace}
h1 small{display:block;letter-spacing:.18em;font-size:10px;color:#777B60;margin-top:10px}
h2{font-size:10px;letter-spacing:.30em;text-transform:uppercase;color:#777B60;font-weight:500;
  margin-bottom:22px;font-family:ui-monospace,Menlo,Consolas,monospace}
h2:before{content:"";display:inline-block;width:22px;height:1px;background:#777B60;
  vertical-align:middle;margin-right:12px;transform:translateY(-1px)}

/* masthead */
.mast{display:flex;justify-content:space-between;align-items:flex-end;gap:40px;
  padding-bottom:26px;border-bottom:1px solid rgba(231,231,225,.16);margin-bottom:34px}
.spec{display:grid;grid-template-columns:repeat(3,auto);gap:6px 34px;font-size:11px;
  font-family:ui-monospace,Menlo,Consolas,monospace;color:#9A9C90;text-align:right}
.spec b{color:#E7E7E1;font-weight:500}
.spec em{color:#E8622A;font-style:normal}

/* timeline */
.tl{position:relative;height:150px;margin-bottom:14px;
  border-top:1px solid rgba(231,231,225,.10);border-bottom:1px solid rgba(231,231,225,.10)}
.tl .bar{position:absolute;top:0;bottom:0;border-left:1px solid rgba(231,231,225,.055)}
.tl .bar.whole{border-left-color:rgba(231,231,225,.16)}
.band{position:absolute;height:34px;top:0;background:rgba(231,231,225,.035);
  border-right:1px solid rgba(231,231,225,.10)}
.band.b4{background:rgba(242,247,244,.09)}
.band.b5{background:rgba(119,123,96,.12)}
.tl svg{position:absolute;left:0;top:46px;width:100%;height:calc(100% - 46px);overflow:visible}
.actName{display:flex;align-items:baseline;gap:7px;font-size:10px;
  font-family:ui-monospace,Menlo,Consolas,monospace;color:#777B60;
  position:absolute;top:9px;left:9px;white-space:nowrap;overflow:hidden}
.actName b{color:#E7E7E1;font-weight:500}
.actName span{letter-spacing:.2em}
.actMeta{position:absolute;top:26px;left:9px;font-size:9px;color:#5C6155}
.act{position:absolute;top:0;height:52px}
.peak{position:absolute;top:0;bottom:0;width:0;
  border-left:1px dashed rgba(232,98,42,.40);pointer-events:none}
.strip{position:relative;height:34px;margin-bottom:4px}
.cell{position:absolute;top:0;height:22px;background:rgba(231,231,225,.22);
  border-right:2px solid #080C10;display:flex;align-items:center;justify-content:center;overflow:hidden}
.cell span{font:9px/1 ui-monospace,Menlo,Consolas,monospace;color:#0A0F14;opacity:.85}
.cell.morph{background:#777B60}
.cell.morph span{color:#F2F7F4}
.cell.orange{background:#E8622A}
.cell.orange span{color:#fff}
.ruler{position:relative;height:44px;margin:18px 0 8px;
  border-bottom:1px solid rgba(231,231,225,.07)}
.rk{position:absolute;bottom:0;width:1px;background:#9BA082;transform:translateX(-50%)}
.rk b{position:absolute;top:-13px;left:50%;transform:translateX(-50%);
  font:8px ui-monospace,Menlo,monospace;color:#777B60;font-weight:400}
.endline{position:absolute;height:1px;background:#E8622A;opacity:.75}
.endoflabel{position:absolute;top:2px;transform:translateX(-100%);
  font:8.5px ui-monospace,Menlo,monospace;color:#E8622A;opacity:.8;
  white-space:nowrap;letter-spacing:.06em;text-align:right}
.cap{font:8.5px ui-monospace,Menlo,monospace;color:#4B504A;letter-spacing:.06em;margin-top:9px}
.cap.moss{color:#777B60}
.scale{position:relative;height:18px;font:9px ui-monospace,Menlo,monospace;color:#5C6155}
.scale span{position:absolute;transform:translateX(-50%)}

/* cards */
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(268px,1fr));gap:14px}
.card{background:#0C1319;border:1px solid rgba(231,231,225,.08);padding:15px 16px 12px;
  position:relative;display:flex;flex-direction:column;gap:9px}
.card:hover{border-color:rgba(231,231,225,.24)}
.card header{display:flex;align-items:baseline;gap:9px;flex-wrap:wrap}
.num{font-size:17px;color:#F2F7F4;letter-spacing:.02em}
.tc{font-size:9.5px;color:#5C6155}
.dur{margin-left:auto;font:9.5px ui-monospace,Menlo,monospace;color:#777B60}
.tick{font:8.5px ui-monospace,Menlo,monospace;color:#0A0F14;background:#777B60;
  padding:1px 5px;letter-spacing:.08em}
.lens{font-size:9.5px;color:#777B60;letter-spacing:.04em}
.scene{font-size:12.5px;line-height:1.72;color:#D3D3C9}
.rows{display:grid;grid-template-columns:30px minmax(0,1fr);gap:4px 8px;
  align-items:baseline;font-size:10.5px;color:#8D9083}
.rows>div{display:contents}
.rows b{color:#5C6155;font-weight:400;font-family:ui-monospace,Menlo,monospace}
.rows span{min-width:0}
.tw{justify-self:start;white-space:nowrap;padding:0 7px;font-size:9.5px;
  line-height:1.6;border:1px solid rgba(231,231,225,.16)}
.tw.tv-cut{border-color:transparent;padding-left:0;color:#6C7067}
.tw.tv-morph{border-color:rgba(119,123,96,.85);color:#A8AC92}
.tw.tv-orange{border-color:#E8622A;color:#E8622A}
.tw.tv-black{border-color:rgba(231,231,225,.34);color:#E7E7E1}


/* morphs */
.mgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:1px;
  background:rgba(231,231,225,.08)}
.morph{background:#0C1319;padding:18px 20px}
.mh{display:flex;align-items:baseline;gap:10px;margin-bottom:10px}
.morph .mh b{font:11px ui-monospace,Menlo,monospace;color:#0A0F14;background:#777B60;padding:1px 7px}
.mh span{font:10px ui-monospace,Menlo,monospace;color:#5C6155}
.mt{font-size:13px;color:#F2F7F4;margin-bottom:8px}
.morph p{font-size:11.5px;color:#8D9083;line-height:1.7}
.md{margin-top:9px;font-size:9.5px;color:#5C6155}

/* key frames */
.fgrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;
  background:rgba(231,231,225,.08)}
.frame{background:#0C1319;padding:20px 22px 18px}
.fh{display:flex;align-items:baseline;gap:11px;margin-bottom:12px;
  padding-bottom:9px;border-bottom:1px solid rgba(231,231,225,.09)}
.fh b{font:11px ui-monospace,Menlo,monospace;color:#777B60;letter-spacing:.1em}
.fh span{font-size:12.5px;color:#F2F7F4}
.frame p{font-size:11.5px;line-height:1.75;color:#9A9C90}
.frame q{display:block;margin-top:12px;font-size:11.5px;color:#777B60;font-style:normal}
.frame q:before{content:"— ";}

/* flash list */
.flash{list-style:none;display:grid;grid-template-columns:1fr;gap:0}
.flash li{display:grid;grid-template-columns:40px minmax(0,1fr) 220px;gap:16px;
  align-items:baseline;padding:8px 0;border-bottom:1px solid rgba(231,231,225,.06);
  font-size:11.5px}
.flash b{font:10px ui-monospace,Menlo,monospace;color:#777B60}
.flash span{color:#D3D3C9}
.flash i{font-style:normal;color:#5C6155;font-size:10px;text-align:right}

/* palette */
.pal{display:grid;grid-template-columns:repeat(auto-fit,minmax(238px,1fr));gap:1px;
  background:rgba(231,231,225,.08)}
.sw{background:#0C1319;padding:0;display:flex;align-items:stretch}
.sw i{flex:0 0 54px}
.sw div{padding:11px 13px;flex:1;display:flex;flex-direction:column;gap:2px}
.sw b{font-size:11.5px;font-weight:500;color:#E7E7E1}
.sw span{font-size:10px;color:#8D9083}
.sw small{font-size:9px;color:#5C6155;letter-spacing:.04em}
.sw code{align-self:center;padding-right:13px;font-size:9.5px;color:#5C6155}

footer{margin-top:64px;padding-top:18px;border-top:1px solid rgba(231,231,225,.10);
  font:10px ui-monospace,Menlo,monospace;color:#4B504A;display:flex;
  justify-content:space-between;gap:20px;flex-wrap:wrap;letter-spacing:.06em}

/* materials */
.matgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:1px;
  background:rgba(231,231,225,.10);border:1px solid rgba(231,231,225,.10)}
.mat{background:#0C1319;padding:18px 20px 16px;display:flex;flex-direction:column}
.mat .mh{display:flex;align-items:baseline;gap:8px;margin-bottom:13px;
  padding-bottom:10px;border-bottom:1px solid rgba(231,231,225,.08)}
.mat .mh b{font:10px ui-monospace,Menlo,monospace;color:#0A0F14;background:#777B60;padding:1px 6px;letter-spacing:.08em}
.mat .mh span{font-size:13.5px;color:#E7E7E1;letter-spacing:.02em}
.mat .mh i{font-size:9px;color:#5C6155;font-style:normal;margin-left:auto;letter-spacing:.04em}
.mat dl{display:grid;gap:4px}
.mat dl>div{display:grid;grid-template-columns:92px 1fr;gap:10px;align-items:baseline}
.mat dt{font:9px ui-monospace,Menlo,monospace;color:#5C6155;letter-spacing:.03em;text-align:right}
.mat dd{font-size:10.5px;color:#9A9C90;line-height:1.55}
.mat .ban{margin-top:auto;padding-top:9px;border-top:1px dashed rgba(231,231,225,.12);
  font-size:10px;color:#8D9083;display:flex;gap:6px;align-items:baseline}
.mat .ban s{color:#777B60;text-decoration:none;font-size:10px}


/* audio panel */
.audbox{position:relative;height:150px;background:#0A1016;
  border:1px solid rgba(231,231,225,.10);overflow:hidden}
.audbox svg{position:absolute;inset:0;width:100%;height:100%;display:block}
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

@media (max-width:1280px){
  .fgrid{grid-template-columns:repeat(2,minmax(0,1fr))}
}
@media (max-width:1000px){
  .mgrid{grid-template-columns:repeat(2,minmax(0,1fr))}
  .flash li{grid-template-columns:36px minmax(0,1fr)}
  .flash i{display:none}
}
@media (max-width:820px){
  body{padding:28px 18px 64px}
  .mast{flex-direction:column;align-items:flex-start;gap:20px}
  .spec{text-align:left}
  .fgrid,.mgrid,.matgrid{grid-template-columns:1fr}
}
@media print{
  html,body{background:#fff;color:#111}
  .card,.morph,.frame,.sw{background:#fff;border-color:#ddd}
  h1{color:#111}
}
"""

HTML = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RHINE MUSIC DATABASE · 概念宣传影像 70″ · 分镜板</title>
<style>%(css)s</style>
</head>
<body>

<div class="mast">
  <h1>Rhine Music Database<small>Concept Promotional Film · Storyboard Board · v4 · 方向 A · 跟着音轨重排</small></h1>
  <div class="spec">
    <div>总时长 <b>70.71s</b></div>
    <div>镜头 <b>34</b></div>
    <div>剪辑点 <b>42</b></div>
    <div>BPM <b>112</b></div>
    <div>小节 <b>33</b></div>
    <div>形变转场 <b>6</b></div>
    <div>旁白 <b>0</b></div>
    <div>介绍文字 <b>0</b></div>
    <div>饱和色 <b><em>3 帧组</em></b></div>
  </div>
</div>

<h2>时间结构</h2>
<div class="tl">
  %(bands)s
  %(actspans)s
  <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
    <path d="%(fill)s" fill="rgba(242,247,244,.07)"/>
    <path d="%(curve)s" fill="none" stroke="#F2F7F4" stroke-opacity=".85"
          stroke-width="1.6" vector-effect="non-scaling-stroke"
          stroke-linejoin="miter"/>
  </svg>
  %(bars)s
  <div class="peak" style="left:75.0000%%"></div>
</div>

<div class="scale">
  %(scales)s
</div>

<div class="strip">%(strip)s</div>
<div class="cap">34 个镜头块 · 苔绿 = 形变转场 · 橙 = 机械交互帧 · 虚线 = 峰值（53.0s），之后画面进入凝止</div>
<div class="ruler">%(ruler)s</div>
<div class="cap moss">刻度尺 · 全片出现 7 次，每次高一格，从不解释 —— 第 7 格就是片尾那条 1px 横线</div>

<hr>


<h2>音频实测 · 这条音轨没有副歌</h2>
<div class="audbox">
  <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
    <defs><linearGradient id="ag" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#F2F7F4" stop-opacity=".20"/>
      <stop offset="1" stop-color="#F2F7F4" stop-opacity="0"/></linearGradient></defs>
    <path d="%(audfill)s" fill="url(#ag)"/>
    <path d="%(audrms)s" fill="none" stroke="#F2F7F4" stroke-opacity=".55"
          stroke-width=".6" vector-effect="non-scaling-stroke"/>
    <path d="%(audair)s" fill="none" stroke="#A8AC92" stroke-width="1.15"
          vector-effect="non-scaling-stroke"/>
  </svg>
  %(audmarks)s
</div>
<div class="keyrow">
  <span><i class="k1"></i>实测脉冲：132 颗齿，齿齿一样高</span>
  <span><i class="k2"></i>实测亮度（7–16 kHz 占比）—— 唯一一次开口</span>
</div>
<div class="facts">%(audfacts)s</div>
<div class="cap moss">白线是这条音轨真正做的事：一条平的、密的、不变脸色的线 —— 132 个齿，齿齿一样高。它不给你情绪、不给你重拍、不给你副歌。全片 34 个镜头、42 刀，全靠画面自己把戏撑起来：声音只负责不让它掉下来。</div>

<hr>

<h2>分镜 · 34 镜 / 42 剪辑点</h2>
<div class="grid">%(cards)s</div>

<hr>

<h2>材质圣经 · A 方向的赌注全在这里</h2>
<div class="matgrid">%(materials)s</div>
<div class="cap moss">贴图糙一点，整片立刻掉进学生作业 —— 这六张表是这一版的主体，不是附录</div>

<hr>

<h2>形变链 · 六处不做剪切</h2>
<div class="mgrid">%(morphs)s</div>

<hr>

<h2>关键帧 · 先做这 8 张</h2>
<div class="fgrid">%(frames)s</div>

<hr>

<h2>第 19 镜 · 闪切十刀（每刀 0.429s / 12fps 步进）</h2>
<ul class="flash">%(flash)s</ul>

<hr>

<h2>调色板与纪律</h2>
<div class="pal">%(palette)s</div>

<footer>
  <span>112 BPM × 33 小节 = 70.71s · 音轨实测：无副歌、无音高、唯一转折 25.7s · 橙只属于机械交互</span>
  <span>RHINE MUSIC DATABASE / STORYBOARD v4</span>
</footer>

</body>
</html>
"""



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
        ("10 刀脱格", "42 个剪辑点里 32 个落在 0.536s 拍格上；只有第 19 镜那 10 刀是 0.429s/刀 —— 全片唯一一次失速"),
        ("结尾 2.5s 淡出", "74.5s 起指数衰减到 −39 dB。这是它唯一给你的礼物"),
    ]
    return "".join("<div><b>%s</b><span>%s</span></div>" % (a_, b_) for a_, b_ in rows)


def scales():
    out = []
    for i in range(0, 71, 5):
        out.append('<span style="left:%.4f%%">%ds</span>' % (x(i), i))
    return "".join(out)


print(HTML % {
    "css": CSS,
    "bands": act_band(),
    "actspans": act_spans(),
    "fill": svg_fill(),
    "curve": svg_curve(),
    "bars": bars(),
    "scales": scales(),
    "ruler": ruler(),
    "strip": shot_strip(),
    "cards": cards(),
    "morphs": morphs(),
    "frames": frames(),
    "flash": flash(),
    "materials": materials(),
    "audfill": svg_aud_fill(),
    "audrms": AUDIO_RMS,
    "audair": AUDIO_AIR,
    "audmarks": aud_marks(),
    "audfacts": aud_facts(),
    "palette": palette(),
})
