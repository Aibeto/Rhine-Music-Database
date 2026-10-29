# RHINE MUSIC DATABASE · 70s 概念 PV — Blender 分镜 previz

`rhine-pv-previz.blend` 是把 `../rhine-concept-pv-70s-storyboard.md`（v4，112 BPM × 33 小节 = 70.714 s）逐镜搭出来的可编辑场景。

## 打开就会看到什么

| 项 | 值 |
|---|---|
| 时长 | 70.714 s |
| 帧率 / 帧数 | 12 fps / 849 帧（1–849） |
| 分辨率 | 1280×720（渲染检查用；正式出片自行改） |
| 镜头 | 34 个 builder → 43 个相机（第 19 镜是十刀，各一台） |
| 切镜方式 | **timeline marker 绑相机**，不是剪出来的 |
| 物体 | 1664 个，按 `COL_<镜号>` 分集合，每条"跑道"间隔 80 m |
| 材质 | 全片纯 Emission，**没有任何灯光** |

渲染：`Render ▸ Render Animation`，输出到任意目录即可。想只出关键帧检查，用下面的 `check_scene.py`。

## 为什么没有灯

三次 PBR 打灯尝试都失败了：被摄体从 2 cm 的唱针尖到 6 m 的档案架通道，同一套灯功率无法通用，结果一半全黑、一半过曝。
改成**纯发光平面材质**后，明暗只由「面法线相对主光方向」决定，任何尺度都稳定，也更贴近原设计（纸白 + 发丝线 + 黑底，本来就不是写实渲染）。

主光方向、环境项、菲涅尔轮廓的系数抄自程序自己的着色器 `Qt/shaders/enhancement/opticalSurface.frag`：

```glsl
const vec3 KEY_LIGHT = normalize(vec3(0.0, 0.55, 0.84));
rim   = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.30;
color = tint * (0.12 + 0.30 * ndl) + vec3(spec + rim);
```

`applook.py` 把这条公式做成了节点组，导入的 `Assets/models/*.glb` 全部走它 —— 所以**程序内的模型在分镜里和程序里是同一族长相**。

## 文件

| 文件 | 作用 |
|---|---|
| `rhine-pv-previz.blend` | **交付物**：34 镜 + 43 相机 + marker |
| `checks/pv_34_shots.png` | 34 镜联络表（每镜 55% 帧，带镜号） |
| `shotkit.py` | 冻结的公共 API：`setup()` / `M()` / `flat()` / `cam_at()` / `move()` / `disc()` … |
| `stages.py` | 底层几何与材质、`cam_at/move` 的机位数学 |
| `pvlib.py` | 更底层的 Blender 封装（prim / cam / key / bloom） |
| `applook.py` | 程序着色器公式的复刻（发光版），给导入模型用 |
| `models.py` | 导入 `Assets/models/*.glb`、归一化、批量换皮、`place()` 实例化 |
| `g1.py … g7.py` | 34 个镜头，按作者分组；每镜一个 builder |
| `assemble.py` | 把 g1–g7 拼成单一 .blend：跑道间隔、marker 绑相机、**逐物体 hide_render 隔离** |
| `check_scene.py` | `python3 check_scene.py <blend> <outdir> [W H SAMPLES JOBS]` —— 每镜取一帧、6 路并行、输出联络表 |
| `run_group.py` | `blender -b --factory-startup -P run_group.py -- gN OUTDIR W H SAMPLES` 单独渲一组 |
| `peek.py` | 同上但可指定取帧比例（默认 0.92），用来判断镜头末态 |

## 三个必须知道的坑

1. **跑道隔离**：34 镜共用一条时间轴，几何摆在各自的 80 m 跑道上。不同跑道的物体仍会闯进彼此的画面（超广角尤其），所以 `assemble.py` 给每个物体打了 `hide_render` 关键帧（CONSTANT），只在自己的镜头区间里可见。**新增镜头必须重跑 `assemble.py`，不要手改 .blend。**
2. **`fill` 的语义**：`dist = size × (lens / SENSOR_HALF) / fill`，也就是 **fill 越大 = 相机越近**。和直觉相反，改机位时先看这条。
3. **`box()` 把尺寸存在 `obj.scale` 里**：给 box 打 scale 关键帧会让它变成错误尺寸。要"长出来"就用 `L.prim('cube', size=1)` 再 key scale，或 key 父空物体。

## 已知弱镜

| 镜 | 问题 |
|---|---|
| 06b | 平面发光没有衰减，"光从刻痕峡谷底掠过"只能靠一条白实体表达，读成"暗渠里的光带" |
| 14 | 全片最平的一张，靠 132 条梯形条带做透光渐变，静帧仍像一块灰色椭圆 |
| 15 | 55% 帧跌落已结束，静帧读不出"多米诺"，动作要到动画里才成立 |
| 16 | 灰白雾靠相机前的噪声 alpha 卡，静帧偏"糊" |
| 27 | 单独看只是一道柔光带 + 三颗尘，价值全在"和开场 01 逐像素一致" |

橙色严格 3 帧（25 镜 613–615），全片无旁白、无可读 UI 文字（片尾 `RHINE MUSIC DATABASE` 是字标，不算介绍文字）。
