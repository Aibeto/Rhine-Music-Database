# 07 · 实施记录：全链路优化 A01/A05/A06/A09/A10（呈现层与渲染线程）

> 本文落实 [全链路性能报告](../vulkan-3d-fullchain-performance-report.md)（下称“报告”）A 表中的五项：**A01**（GPU 计时可信度与帧 ID 贯穿）、**A05**（运动矢量 MRT 按需）、**A06**（`blur==0` 盘面单采样）、**A09**（绘制状态去重全平台化）、**A10**（swapchain resize 去重与退休证明）。
> 证据等级沿用全套约定：〔规范〕Vulkan 规范/Khronos ·〔源码〕本仓库代码事实 ·〔算术推导〕由公开规格或代码路径换算 ·〔实测〕本机可复现测量（命令附在文末）·〔未验证〕尚未测量的推断。
> **本文不含 GPU 实测数字**：本轮只做到“代码级 + 构建/测试级”验证，运行时收益需要用 §1.3、§5.3 列出的四态与三个计数器在一次真人操作里采集。报告里“不把未测当已测”的要求同样适用于本文。

---

## 0. 一页结论

| ID | 改动 | 状态 | 验证 | 代价 / 风险 |
|---|---|---|---|---|
| A01 | 受控呈现路径开启 Vulkan 时间戳查询；GPU 计时按“能力→是否请求→有无样本”四态上报；`presentMs` 与 `endMs` 的重复计量换成整帧 `frameMs` + `frameId` | 已实施 | 构建通过；`qt_render_policy_regression` 覆盖四态分类与状态名；面板与 CSV 已接线〔源码〕 | 驱动不支持时间戳时状态显示 `unsupported`，不伪造数值；`presentMs` 字段保留但标弃用（`FgCapabilityChecks.cpp` 仍在赋值） |
| A05 | 运动矢量 MRT 与逐实例历史只在 `taa`/`fsr` 需要时分配，并把“是否需要”纳入 resize 早退比较 | 已实施 | 构建通过；`RHINE_AA_MODE=off/fxaa/msaa/fsr1` 不再分配目标〔源码〕 | 切换 AA 模式会触发一次资源重建（已被早退吸收，不再每帧重建）；`fsr3` 运行期不可用时仍按配置分配（空写一次 16 B/实例） |
| A06 | 盘面片元着色器在 `blur==0` 时走单次采样快路径（两个变体同步改） | 已实施 | 数学恒等 + float32 模拟偏差 ≤5 ULP〔算术推导〕 | 仅影响“清晰档”（`clarity==1` 或非 viewer）；模糊档路径与画质不变 |
| A09 | 绘制状态去重（排序 + 只在管线/绑定变化时下发）从 Linux 专用分支推广到全部后端，并修掉 `#ifdef` 包住函数闭合括号的结构隐患 | 已实施 | 构建通过；透明段提交顺序不变〔源码〕 | 唯一画面差异：`blend=true` 但非 transparent 的 hashed 材质改为按管线分组提交，需真人目检 |
| A10 | 后端 `resize()` 增加尺寸早退（不再每帧 `m_rhi->finish()`），并新增 `swapchainRecreates`/`deviceIdleWaits`/`resizeSkips` 三个计数器贯穿到面板与 CSV | 已实施 | 构建通过；计数器接线到 `RhineFg::Stats`/CSV/reportStats〔源码〕 | 早退依赖“已生效交换链尺寸”这一新口径；运行期收益待用 `fg_resize_skips` 与 `fg_device_idle_waits` 的比值证明〔未验证〕 |

一句话：**这一轮把“看不见”变成“看得见”，把“每帧白等 GPU”从代码路径上删掉**；真正的帧时间收益仍需一次真人操作把新计数器采出来。

---

## 1. A01：GPU 计时可信度与帧 ID 贯穿

### 1.1 问题（三个独立的缺口）

1. **受控呈现路径从来没请求过时间戳。** `VulkanPresentationBackend::createDevice()` 建 QRhi 时只带 `QRhi::EnablePipelineCacheDataSave`，而 `RhiScene` 的 GPU 帧时间读的是 `cb->lastCompletedGpuTime()`，前置条件是 `rhi()->isFeatureSupported(QRhi::Timestamps)`。〔源码〕默认 Qt Quick 路径在 `main.cpp` 里有 `graphics.setTimestamps(true)`，于是行为分裂：默认路径能报数，呈现层接管后恒为 0。
2. **0 的含义被复用。** 面板把 `gpuTimeMs` 为 0 一律当作“没有测量”，而三种截然不同的原因（不支持 / 没请求 / 这一帧还没完成）挤在同一个 0 里，无法判断是平台问题还是配置问题。〔源码〕
3. **计时重复且缺帧 ID。** `PresentationShell::presentOne()` 里 `endFrame()` 之后重新起表再记 `presentMs`，量到的只是几条记账语句（恒≈0），与 `endMs` 是同一段；同时 CSV 的每一行没有与呈现帧对齐的标识，“帧间隔 30 ms 但分项都是 1 ms”的缺口无法追到具体帧。〔源码〕

### 1.2 改法

- `Qt/src/render/VulkanPresentationBackend.cpp`：`QRhi::create(..., QRhi::EnablePipelineCacheDataSave | QRhi::EnableTimestamps)`；建好后若 `isFeatureSupported(QRhi::Timestamps)` 则 `RhineRender::requestTimestamps()` 登记。离屏的两个 `QQuickWindow` 通过 `QQuickGraphicsDevice::fromRhi()` 共用同一个 QRhi（`SceneLayerHost` 里有 `m_scene.control->rhi() == rhi` 的校验），所以这一处开关同时覆盖场景与图层两条绘制路径。
- `Qt/src/main.cpp`：默认 Qt Quick 路径同样 `RhineRender::requestTimestamps()`，与既有 `setTimestamps(true)` 对齐——否则呈现层没接管时面板会把“没请求”报成“没样本”。
- 新增 `Qt/src/render/RenderPolicy.h`（header-only，只依赖 `QString`，可离线测）：
  - `enum class GpuTimingStatus { Unsupported, Disabled, NoSample, Ok }`；
  - `classifyGpuTiming(featureSupported, timestampsRequested, hasSample)`——`!supported` 优先判 `Unsupported`，其次 `!requested → Disabled`，最后 `hasSample ? Ok : NoSample`；
  - `gpuTimingStatusName()` 给出 `"unsupported" / "disabled" / "noSample" / "ok"`。
- `Engine::recordRender()` 追加第 7 参 `RhineRender::GpuTimingStatus gpuTiming`，`performance()["gpuTimingStatus"]` **无条件**输出；`gpuTimeMs` 仍只在 `> 0` 时输出（0 继续表示“没有数值”，数值与状态分开）。
- `Qt/src/RhiScene.cpp` 每帧：`classifyGpuTiming(rhi()->isFeatureSupported(QRhi::Timestamps), RhineRender::timestampsRequested(), gpuTimeMs > 0)`。
- `PresentationShell`：新增 `m_frameProbe`，从 `beginFrame()` 之前起表到 `endFrame()` 返回后停表，得到**整帧** `frameMs`；`presented` 时 `++frameId`。原“上屏耗时”列改名为“整帧”，`reportStats()` 一行里可直接对照“取图/场景/光流/插值/合成/整帧”。
- `FrameProfiler`：CSV 在**末尾**追加 7 列（`gpu_timing_status, render_frames, fg_frame_id, fg_frame_ms, fg_swapchain_recreates, fg_device_idle_waits, fg_resize_skips`），旧 14 列顺序不动，历史分析脚本不受影响；面板详情新增一行“GPU 时间 …”。

### 1.3 验收口径（怎么读这四态）

| 状态 | 含义 | 现场该查什么 |
|---|---|---|
| `ok` | 有真实样本 | `gpuTimeMs` 可直接引用 |
| `disabled` | 驱动支持，但这条路径没请求 | 呈现层 `EnableTimestamps`、`main.cpp` 的 `setTimestamps(true)` 是否被别处覆盖 |
| `noSample` | 已请求，本帧没有已完成样本 | 首帧、刚重建资源、或提交队列长期未完成 |
| `unsupported` | 后端/设备不支持 | 软件光栅化（`RHINE_SOFTWARE_GPU`）、部分 GL 实现 |

### 1.4 未做

- 多帧平均与百分位：`frameMs`/`gpuTimeMs` 仍是 `blend()` 滑动平均（与既有分项一致），尖峰看 `gapP99/over33Ms` 直方图；**逐帧最慢样本**没有单独落盘。
- 每 pass 的 GPU 时间戳（`QRhi::newTextureReadback` + 多处 `setTimestamps`）没有做，属于 A11 的账本范畴。
- **没有 GPU 实测**：本机 GUI 运行会在启动时恢复网络播放（对用户有副作用），已放弃该验证方式；四态在构建期与离线用例里覆盖。

---

## 2. A05：运动矢量 MRT 按需分配

### 2.1 问题

`RhiScene::resize()` 里 motion 目标的分配条件只有 `sampleCount <= 1`：于是 `RHINE_AA_MODE=off/fxaa/msaa/fsr1` 也会分配一张 `RGBA16F` 颜色附件、维护两张历史纹理、并给每个实例多写 16 B 的 `previousModel`——这些数据只有 TAA/FSR 的时域路径会读。〔源码〕〔未验证〕多一次全屏附件写入在核显上是实打实的带宽。

### 2.2 改法

- `ScenePostProcess` 新增 `bool motionConfigured() const` = `RhineRender::modeNeedsMotion(qEnvironmentVariable("RHINE_AA_MODE")) || RhineFg::isEnabled()`。**分配看配置，消费看运行期能力**：`needsMotion()` 语义保持不变（`temporal()` 还要看 FSR 引擎是否 `available()`），避免“配置说要、运行期拿不到”时把 MRT 删掉。
- `RhiScene::resize()`：`const bool wantMotion = nextSamples <= 1 && (postProcess->motionConfigured() || qEnvironmentVariableIsSet("RHINE_WARP_PROBE"));`，并把 `motionTarget == wantMotion` 加进早退条件；切换时打一行 `运动矢量目标: 分配/不分配 | AA <mode> | samples <n>`。

| `RHINE_AA_MODE` | 运动矢量目标 | 说明 |
|---|---|---|
| `off` / `fxaa` | 不分配 | 没有时域重投影 |
| `msaa` | 不分配 | 采样数 > 1，MRT 与 resolve 冲突（既有约束） |
| `fsr1` | 不分配 | 只有空间升采样 |
| `taa` / `fsr3` | 分配 | 需要逐像素运动矢量 |
| 任意 + `RHINE_FG_LAYER=on` | 分配 | 呈现层插值要 depth/motion |
| 任意 + `RHINE_WARP_PROBE` | 分配 | 探针需要写 warp |

### 2.3 代价与风险

- 模式切换（例如 `off → taa`）会触发一次资源重建；这条路径本来就要重建，早退比较里加上 `motionTarget` 只是保证“配置变了能重建”，不会引入每帧重建。
- `fsr3` 在运行期 `available()` 为假时仍会分配：这是“按配置口径”而不是“按当帧能力口径”的取舍——当帧能力会在运行中翻转，跟着它分配/释放会得到抖动。
- 非 Linux 平台只多一个 `bool` 成员，不改变它们的实例布局。

---

## 3. A06：`blur == 0` 的盘面单采样

### 3.1 位置

`Qt/shaders/enhancement/opticalSurface.frag` 与 `opticalSurfaceNoMrt.frag` 各有一份盘面（`flags.x == 6.0`）采样块，必须同步改，否则 OpenGL 后端与 Vulkan 后端画质不一致。

### 3.2 等价性

原代码在 `blur == 0` 时：中心采样 1 次（权重 4）+ 两环各 8 次（每环 4 个方向各正负，共 16 次），全部落在同一个 `discUv`；权重和 `total = 4 + 16 = 20`。因此

```
(4·S + 16·S) / 20 = S
```

与快路径 `linearColor(texture(cover, discUv).rgb) * 0.8` 在数学上恒等，`keyLight()` 的作用顺序也一致（两路都是先乘 0.8 再 keyLight）。〔算术推导〕float32 逐项模拟两次求和的相对偏差 ≤ 2.98e-7（≤5 ULP），远低于 8 bit sRGB 的一个台阶（≈2.46e-3 线性）。

阈值用 `blur <= 1e-6` 而不是 `blur == 0.0`：`blur = viewer ? (1.0 - clamp(v_clarity,0,1)) : 0.0`，非 viewer 时精确 0，viewer 且 `clarity == 1.0` 时 `1.0 - 1.0` 也精确 0；用 epsilon 只为避开浮点相等比较，并且不会把“几乎清晰”的实例误放进快路径。

### 3.3 收益与代价

- 清晰档（默认 `clarity = 1.0`，且非 viewer）盘面像素的纹理采样从 17 次降到 1 次：报告里写的“20”是权重和，实际 texture 指令是 17 条。
- 模糊档路径完全没动，画质不受影响；分支是 uniform 风格的逐实例标量，不会造成 warp 内发散放大。
- 同一片元着色器里另外两处“看起来可以省”的采样（`opticalFinalFragment.frag` 的九点柔化在 `soften=0` 时仍是 ±1 texel、`taa.frag` 的 3×3）**本轮没改**：前者属于 A07 的质量档设计，后者是时域积分的固有成本。

---

## 4. A09：绘制状态去重全平台化

### 4.1 问题

`RhiScene::geometry()` 里“按管线/绑定排序后只在变化时下发”的去重路径整段被 `#ifdef Q_OS_LINUX` 包住，其他后端每 draw 无条件 `setGraphicsPipeline()` + `setShaderResources()` + `setViewport()`。〔源码〕同一份数据，Linux 上 3 次状态切换，Windows/macOS 上可能是 3×N。

### 4.2 改法

- 去掉 `geometry()` 内的 `#ifdef Q_OS_LINUX / #else / #endif`，排序 + 去重路径全平台生效；`RHINE_DRAW_PROBE` 的探针统计保留（它统计的正是“实际下发次数”，是这条路径是否生效的直接证据）。
- 同时修掉一个结构隐患：HEAD 里 `geometry()` 末尾的 `#ifdef Q_OS_LINUX` 还包住了**函数自身的闭合花括号**和整个 `warpFrame()`，导致任何按预处理条件改这段代码的操作都容易把大括号弄丢。现在 `geometry()` 的 `}` 在 `#ifdef` 之外，`warpFrame()` 单独被 `#ifdef` 圈住。

### 4.3 一致性

- 透明段顺序不变：`render()` 本来就分别调用 `geometry(opaque)` 与 `geometry(transparent)`，而不透明/透明的划分来自 `data["transparent"]`；`geometry()` 内的 `stable_sort` 对透明项恒返回 `false`，保持原序。
- 唯一可见差异：`blend = true` 但 `data["transparent"] = false` 的 hashed 材质（磨砂/半透明封面一类），现在会按管线分组提交，而不是按 draw 顺序。这类材质如果依赖“按绘制顺序混合”，理论上可能出现差异——**需要一次真人目检**（放大封面、切到磨砂卡面）确认无可见变化。

---

## 5. A10：swapchain resize 去重

### 5.1 根因

`PresentationShell::syncSizes()` 每帧把窗口尺寸推给后端；`VulkanPresentationBackend::resize()` 无条件执行 `m_rhi->finish()`（`vkDeviceWaitIdle`）→ `releaseTargets()` → 重建交换链目标 → `ensureTargets()`。`VulkanPresenter::resize()` 内部本来有“尺寸没变就返回”的早退，但它在 `finish()` 之后才被调用，等于每帧一次设备空闲等待＋一次全量资源重建。〔源码〕这正是报告里“呈现层最贵的一行”和反模式清单第 11 条（每帧 `vkDeviceWaitIdle`）。

### 5.2 改法

- 后端 `resize()` 顶部：`if (!force && m_presenter && !m_swapTargets.empty() && size == m_size && m_swapSize == m_presenter->extent()) { ++m_resizeSkips; return true; }`——只做尺寸比较，不 `finish()`、不重建。
- `m_size` 口径从“上次请求的逻辑尺寸”改为“**已生效的交换链尺寸**”：`initialize()` 末尾与每次重建成功后都取 `m_presenter->extent()`。否则 HiDPI 下“请求的逻辑像素 ≠ 交换链物理像素”，同尺寸判断会永久失败，早退形同虚设。
- `PresentationShell::syncSizes()` 末尾新增受保护接线：`if (m_presenter && !m_needRecreate && m_presenter->extent() == pixelSize()) m_presenter->resize(pixelSize(), false);`——只确认、不重建；真正重建仍只走 `recreateSwapchain()`（`force = true`），那里要作废跨尺寸的插值历史并强制重画一帧。
- 三个计数器：`swapchainRecreates`（重建成功次数）、`deviceIdleWaits`（真正发生 `m_rhi->finish()` 的次数）、`resizeSkips`（早退次数）。它们经 `RhineFg::Stats` 每帧发布，`reportStats()` 一行打印“重建 N 等待 M 尺寸跳过 K”，并各占 CSV 一列。

### 5.3 验收口径

| 现象 | 结论 |
|---|---|
| `fg_resize_skips` 随帧数增长，`fg_device_idle_waits` 不增长 | 静止窗口下不再每帧等 GPU 空闲（A10 生效） |
| 拖动/改窗口时 `fg_swapchain_recreates` 与 `fg_device_idle_waits` 同步 +1，然后停住 | 重建只发生在尺寸真的变化时 |
| 两者都持续增长 | 早退条件没命中：查 `m_size` 与 `presenter->extent()` 的口径差（DPI 缩放、presenter 内部 clamp） |

### 5.4 风险

- 早退只在 `force == false` 生效：`OutOfDate` 与窗口 `Resize/Expose` 仍强制重建，不会把“能呈现但已失效”的交换链留下。
- 若 `presenter->extent()` 与请求尺寸长期不一致，会退化成“每帧一次重建”以外的路径：第一帧重建后 `m_size = extent`，之后即命中早退（自愈，不会回到旧行为）。
- 本项**没有 GPU 实测**：收益的上界是“每帧一次 `vkDeviceWaitIdle`”，实际节省取决于该 API 在本机的耗时〔未验证〕。

---

## 6. 验证记录

| 项 | 命令 / 方法 | 结果 |
|---|---|---|
| 增量构建 | `cmake --build output/qt-build -j$(nproc)` | 通过：首次全量 180 步，改动单元重编 10 步，无新增警告；还原构建噪声后再增量 18 步 |
| 回归测试 | `ctest --test-dir output/qt-build --output-on-failure` | **12/12 通过（40.02 s）**，含新增 `qt_render_policy_regression`（0.01 s）、`qt_offline`（7.01 s）、`qt_audio_stream_regression`（26.69 s） |
| 纯策略用例 | `./output/qt-build/RhineRenderPolicyRegression` | 29 条断言：四态分类 8 格、状态名逐字、`modeNeedsMotion` 10 例、时间戳登记单调 |
| 工程门禁 | `python3 scripts/check-qt-project.py` | `Qt source tree and build commands: no legacy language dependencies` |
| 上游完整性 | `node scripts/check-music-upstream.cjs` | 通过（API 哈希、Enhanced/QQ 锁定与协议拷贝移除） |
| 打包审计 | `python3 scripts/audit-qt.py output/RhineMusicDatabase.AppDir` | `{"qt_deployed": true, "swift_runtime_required": false, "locked_music_bridge": true}` |
| 打包回归 | `node scripts/check-packaged-app.cjs output/RhineMusicDatabase.AppDir` | Session / Native music 44 项 / Qt 场景与离屏包检查全部 passed |

**关于 `npm test`**：它在第一步 `scripts/check-repository.cjs` 就中止，报 `Unexpected public documentation`——实际文件集多出 `docs/vulkan-igpu/0{1..6}-*.md`、本报告与 `.mnemon/runtime/*.md`，而白名单只有 `README.md`、`tools/perf/README.md`。`docs/vulkan-igpu/*` 是分支上已提交的文件（`512e064 docs(vulkan-igpu): 添加实施记录文档与目录项`），白名单没同步，**属分支既有问题，与本轮改动无关**；上表把该步骤之后的门禁逐条单独跑过。桥接测试（`local-lyrics` / `ios-bridge` / `netease-client` / `qq-client` / `kugou-client`）本轮未单跑。

---

## 7. 本轮未落地的 A 项与理由

| ID | 动作 | 为什么这轮不做 |
|---|---|---|
| A02 | 捕获多离屏提交、帧槽等待 | 要改呈现层“谁拥有提交”的结构；在 A01（可信计时）与 A10（退休证明）落地前无法判断收益，属“先测量后架构”的第二阶段 |
| A03 | 图层 dirty / resource revision 统一 | 涉及 `SceneLayerHost` 与图层生命周期，回归面覆盖整个 UI，超出本轮风险预算 |
| A04 | 稳定实例 ID 维护 previous transform | 上游 `NativeScene` 的 `SceneDrawData` 目前只有 `matrices`/`themes`，没有逐实例稳定逻辑 ID；现有“按平移列匹配”的启发式只能靠画面验证，改了无法自动测 |
| A07 | 复用最终颜色纹理、九点柔化质量档 | 需要先定质量档位与 `soften` 阈值，并且必须做画面对比；与 A06 同一片元着色器，分两轮改更好审查 |
| A08 | 图片与 mip 预处理、上传预算 | 动资源上传与 `musicTextureBytes` 口径，需要单独的带宽测量 |
| A11 | 局部预算升级为整帧资源账本 | 依赖 A01 的四态口径稳定后再统一记账；本轮只加了 7 列计数器 |
| A12 | LOD / 深度预通过 / indirect / async | 专项实验，需独立基线与开关矩阵，不适合与可观测性改动混在一个提交里 |

---

## 8. 构建噪声（提交前需还原）

FFX 着色器排列生成器在构建时会重写 `generated/ffx_shaders/vk/*.h` 共 14 个文件：内容集合等价，但 include 顺序与 `IndirectionTable` 索引按哈希顺序重排。这不是本轮的改动，也不会影响功能，但会把 diff 弄脏：

```bash
git restore --source=HEAD -- generated/
```

建议后续把这个生成器的输出固定成确定性顺序，或者把它挪到 `output/` 下（本轮不动构建系统）。

---

## 9. 复现命令

```bash
# 构建（Release，已有 output/qt-build 缓存）
cd /home/aibeto/code/Rhine-Music-Database && cmake --build output/qt-build -j"$(nproc)"

# 回归（12 条，含渲染策略用例与 qt_offline 自检）
ctest --test-dir output/qt-build --output-on-failure

# 只跑新增用例
./output/qt-build/RhineRenderPolicyRegression

# 采集 A01/A10 的现场数字（GUI，会恢复上次播放的网络音频，注意副作用）
QT_FORCE_STDERR_LOGGING=1 RHINE_FG_LAYER=on RHINE_FG_DIAG=1 \
  ./output/qt-build/RhineMusicDatabase --frame-profile /tmp/fg.csv
# 关注列：gpu_timing_status / fg_frame_id / fg_frame_ms /
#         fg_swapchain_recreates / fg_device_idle_waits / fg_resize_skips

# 构建噪声还原
git restore --source=HEAD -- generated/
```

**下一轮优先级建议**：先用上面的 CSV 采一组稳定窗口 + 拖动窗口的对照数据，确认 `fg_resize_skips` 与 `fg_device_idle_waits` 的比值；再决定 A02（提交结构）是否值得动。A04 需要上游补逐实例 ID，是唯一一个“必须改数据源才能做对”的项。
