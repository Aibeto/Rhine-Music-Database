Rhine-Music-Database 渲染性能：Vulkan 呈现层（PresentationShell）把两层离屏录制+合成+present 全压 GUI 线程，fg=off 时纯负担；改为启动时按 RHINE_FG 决定是否创建呈现层（Qt 默认路径）后内容帧 20.3→27.2/s（+34%）。代价：切换插帧需重启，设置页已标注「· 重启生效」。
§
Rhine 插帧设置项死锁（已修复，Qt/src/fg/FgCapability.h）：fg=off 不再创建呈现层后 presentLayerActive 恒 false，frameGenerationUi() 原本对它返回 {false,"呈现层未接管"} 置灰，导致开关永远打不开。修复为：未接管且偏好为关→{true,"关闭"}；偏好已开→{true,"已开启 · 重启生效"}；真正失败（reason 非空）仍置灰。回归用例见 Qt/tests/FgCapabilityChecks.cpp 的 settingsRowStates() 第 5/5b 条。
§
Rhine 帧率测量口径：以「内容帧/s」（节拍诊断的 render/s = Engine::m_renderFrames）为主，presented 仅作辅；fg=off 时 Vulkan presented≈内容帧×1.86（SceneLayerHost::renderFrame 非 Metal 每帧强制置脏，同帧重渲重上屏），用 presented 当帧率会高估 Vulkan。测法：全屏 2560x1440、主界面收藏专辑列表、uinput 按住 KEY_DOWN 20s，按 ↓ 键码 16777237 首末时间戳取窗口。
§
Rhine 构建/磁盘陷阱：./build.sh 每次都会附带 AppImage 打包（appimagetool），反复构建会堆满磁盘导致 ENOSPC、工具全部无法执行。日常验证改用 cmake --build output/qt-build --target RhineMusicDatabase -j8。清理：rm -rf /tmp/appimage_extracted_* output/RhineMusicDatabase.AppDir output/*.AppImage。
§
Rhine 性能工作产物的存放位置：脚本在仓库 tools/perf/（bench2.sh、press.py、holdkey.py、ts.py、analyze2.py、make_baseline.py、README.md）；诊断结论见 Mnemon 文档《Rhine 渲染性能诊断与改动记录（perf/render-precision）》，脚手架说明见《Rhine 帧率测量脚手架》。/tmp/mb 下的旧副本可弃。
