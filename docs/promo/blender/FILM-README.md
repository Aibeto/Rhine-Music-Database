# 全片 previz v1 —— 交付说明

## 交付物
| 文件 | 内容 |
|---|---|
| `docs/promo/rhine-pv-film.mp4` | 70.71 s / 1280×720 / 12 fps / H.264，全片 |
| `docs/promo/blender/checks/film_contact_v1.png` | 36 格联络表（每 2 s 一格） |
| `docs/promo/blender/checks/m1_animatic_v0.mp4` | M1「访问」单独成片（25.7 s） |
| `docs/promo/程序视觉规格书.md` | 从程序源码挖出的全部实测数值（镜头/雾/着色/纸洗/排印/开场时间线） |
| `docs/promo/改进计划书-v2.md` | 32 镜改进计划（本片是它的简化版） |

## 片子怎么生成的（重要：不是 Blender 时间线）
三段拼起来，全部由 Python 逐帧合成：

1. **M1 访问（0 – 25.71 s）** —— `m1.py` 纯 2D 纸张层。逐字打字机、日志行、一点透视栅格、缓推 3%。公式照抄 `Qt/src/BootTimeline.cpp:9`，字表照抄 `boot-lettering-art.json`。
2. **M2/M3（25.71 – 70.71 s）** —— `film.py`。底板是 `plates.py` 用真 glb `Assets/models/archive-cassette.glb`（17 mesh → 合并 → 按 5.0104×0.3764×3.7 重排）实例化的 5 车道 × 40 行，渲成 RGBA 底板；合成时逐帧做 **ACES → 纸色 → 纸洗**，再叠 HUD。
3. **界面层** —— `hud.py`，按 `Qt/src/NativeHud.h:Layout`（left 40 / sidebar 250 / x 326 / top 260 / rowsY 434 / rowHeight 54 / 9 行）等比缩放。

关键常数：`PAPER.light #DEDBD7`、`PAPER.dark #11181B`、ACES 曝光 0.92、纸洗四层（顶 0.72 / 底 0.72 / 椭圆 (0.42,0.47) r(.58,.53) edge .19 / 右缘 x>0.59）。
橙色全片 3 帧（M3 线左端，`0.62≤u<0.66`）。

## v1.1 改了什么（21 镜）
1. **补了 5 个程序动作镜头**，参数照抄 `Qt/src/SceneMotion.cpp`，由 `acts.py` 用真几何逐帧渲出：
   - `a_extract` 抽出：`0.4·smooth((t−0.82)/…) + 2.95·smooth + 0.7·smooth` 三段（30 帧）
   - `a_hover` 悬停抬起 0.28，速率 `1−exp(−14t)`（22 帧）
   - `a_return` 归位 `3.3·exp(−7t)`（22 帧）
   - `a_ripple` 涟漪：波速 8 单位/s、周期 3.2 s、衰减 `exp(−1.15t)`、`cos(phase)`（30 帧）
   - `a_scan` 扫描波：19 行/s 从 row 3 扫到 32，后 6 行带 −0.58 负瓣（30 帧）
2. **对比度**：ACES 后加 `(x−0.5)·1.16+0.48` 的 S 形压制，纸洗强度从 1.0 降到 0.50，卡片不再糊成白片。
3. **镜头表**从 19 条重排为 **21 条**，动作镜头插在 M2 前段与暗段之间。

## 仍然弱（下一版）
1. 底板仍是"整块矩形卡片"的剪影边缘，缺柔边遮罩，远景有硬边。
2. M2 仍以同一堵墙为主，缺"昼夜扩散"（半墙变色 + 按 `|Δrow|·0.034+|Δlane|·0.11` 延迟）与"解密"的真实几何。
3. **无 Blender 时间线**：本片是 `plates.py`/`acts.py` 渲底板 + `film.py` 合成，不是 `.blend` 逐镜渲。旧 34 镜 `rhine-pv-previz.blend` 与本片不一致。

## 重跑
```bash
cd docs/promo/blender
python3 m1.py        # → /tmp/m1/frames（309 帧）
blender -b --factory-startup -P plates.py    # → /tmp/plates
python3 film.py      # → /tmp/film/frames（849 帧，约 4 min）
ffmpeg -y -framerate 12 -i /tmp/film/frames/f_%04d.png -c:v libx264 -pix_fmt yuv420p -crf 18 film.mp4
```

## `RHINE-pv-scene.blend`（已验证）
单一 Blender 场景，打开即可看/渲：
- 200 张真卡片，共享同一个 mesh（`Assets/models/archive-cassette.glb` 合并后 95,865 顶点 / 86,154 面），尺寸 5.01 × 0.38 × 3.70，5 车道 × 40 行，`LANEP 5.2 / PITCH 0.62`
- 21 台相机 + 21 个 timeline marker，按镜头自动切机位（帧 1–849，12 fps，1280×720，EEVEE）
- 程序动作已 key 成关键帧：抽出（38–68）/ 悬停（92–116）/ 扫描波（150–180）/ 涟漪（204–234）/ 归位（258–280）
- 18 个材质（来自 glb 原始分件），AgX，太阳光 2.2 + 世界 0.45，`shift_x 0.16 / shift_y 0.20`、`lens ≈ 93 mm`

**没有的东西**：纸洗层、HUD 排印、M1 开场（这三样在 `film.py` / `m1.py` 里合成，不进 .blend）。
