# RHINE MUSIC DATABASE

**基于莱茵生命终端的第三方网易云 / QQ / 酷狗音乐面板**

这是对《明日方舟》特别映像「莱茵生命：访问」终端界面的非官方复刻

使用 **Qt 6 / QML / C++** 构建

面板支持检索、分组浏览、账号登录与 **播放**，同时保留网易云音乐官方客户端播放入口

![面板总览](Assets/previews/panel.png)

![曲目纵列（暗色）](Assets/previews/tabmode_dark.png)

![歌曲详情与光盘盒（暗色）](Assets/previews/detail_dark.png)

![歌手页与曲目列表](Assets/previews/list.png)

![同步歌词](Assets/previews/lyrics.png)

![播放队列](Assets/previews/queue.png)

![面板设置](Assets/previews/settings.png)

## 快速运行

构建产物位于：

| 平台    | 程序                                                           |
| ------- | -------------------------------------------------------------- |
| macOS   | `output/RHINE MUSIC DATABASE.app`                              |
| Windows | `output/RhineMusicDatabase-<构建标识>-Windows-x64-Setup.exe`       |
| Linux   | `output/RhineMusicDatabase-<构建标识>-Linux-x86_64.AppImage`              |

**macOS 若提示应用已损坏，需要执行**

`xattr -r -d com.apple.quarantine '/Applications/RHINE MUSIC DATABASE.app'`

并且在设置-隐私中允许运行；

Windows 运行 `RhineMusicDatabase-<构建标识>-Windows-x64-Setup.exe` 安装程序。
Qt、MSVC 运行库和 Node 桥接均已随包提供，无需另行安装。

Linux 赋予执行权限后直接运行 AppImage，Qt 运行时、QML 模块、FFmpeg 与 Node 桥接都已随包提供：

```sh
chmod +x 'RhineMusicDatabase-<构建标识>-Linux-x86_64.AppImage'
./'RhineMusicDatabase-<构建标识>-Linux-x86_64.AppImage'
```

没有 FUSE 的环境（容器、部分滚动发行版）加上 `--appimage-extract-and-run` 参数同样可以启动。

macOS、Linux 和 Windows 默认使用 FFmpeg 音频后端。Windows 设置 `QT_MEDIA_BACKEND=windows` 可临时对照原生后端。
各平台播放都会跟随系统默认输出设备

**面板内播放支持网易云、QQ 和酷狗音乐返回的可用音源。网易云官方客户端入口需要安装并注册网易云音乐；QQ 提供来源网页入口，酷狗提供官方网站入口。**

## 功能列表

勾选表示已实现；未勾选表示当前未实现，不代表承诺开发。

### 网易云音乐

- [x] 歌曲、专辑、歌单、歌手搜索与分页
- [x] 歌手热门歌曲与发行专辑
- [x] 扫码、手机密码、邮箱密码登录
- [x] 喜欢歌曲与收藏歌单、专辑、歌手
- [x] 创建、重命名、删除自己的歌单
- [x] 歌曲加入或移出歌单
- [x] 每日推荐、推荐歌单、排行榜、新专辑、分类歌单
- [x] 面板内播放与网易云官方客户端入口
- [x] 同步歌词与逐句滚动
- [ ] MV、评论社交、云盘、电台
- [ ] 音乐下载

### QQ 音乐

- [x] 关键字搜索
- [x] 从歌曲浏览专辑与歌手作品
- [x] 手机 QQ、微信扫码登录
- [x] 喜欢歌曲、自建及收藏歌单、收藏专辑
- [x] 自建非公开歌单读取
- [x] 每日 30 首、猜你喜欢、推荐歌单、榜单、新专辑、分类歌单、新歌速递
- [x] 面板内播放与来源网页入口
- [x] 歌曲、专辑、歌单、歌手独立搜索与分页
- [x] 红心与取消红心，收藏及取消收藏歌单、专辑
- [x] 创建、改名、删除本人普通歌单，普通曲库歌曲加歌、移除
- [x] 同步歌词与逐句滚动
- [ ] MV、评论社交、电台
- [ ] 音乐下载

### 酷狗音乐（标准版 / 概念版）

- [x] 选择标准版或概念版 App 扫码登录，固定设备身份与两个版本独立的本机加密凭据保存
- [x] 登录后的歌曲、专辑、歌单、歌手搜索
- [x] 自建及收藏歌单、默认喜欢歌单读取，歌单、榜单及专辑曲目播放
- [x] 推荐歌单、每日推荐、排行榜、新专辑与分类歌单
- [x] 歌手单曲和发行专辑浏览
- [x] 按文档注册设备取得 dfid，再调用 `/song/url`；高音质不可用时回退标准音质
- [ ] 收藏专辑读取与账号内容编辑
- [x] 同步歌词与逐句滚动
- [ ] MV、评论社交、云盘、电台、下载

### 本地音乐

- [x] 导入文件或递归导入文件夹并建立索引，支持 MP3、FLAC、M4A、MP4、AAC、OGG、Opus、APE、WAV、AIFF、WV、WMA、MPC
- [x] 读取标题、歌手、专辑、年份、时长、采样率与码率；无标签时从“歌手 - 歌名”文件名推断检索词
- [x] 同目录 `cover`、`folder` 或 `front` 图片优先，其次读取内嵌封面，封面缓存到应用数据目录
- [x] 同名 `.lrc` 歌词优先，其次内嵌歌词；本地歌词来源可选自动、本地或指定平台
- [x] 缺少歌词或封面时复用跨平台录音匹配补充，不改写音频标签或音乐文件
- [x] 按歌曲、歌手、专辑与文件夹分组浏览，支持本地检索
- [x] 面板内播放、播放队列与同步歌词
- [x] 按文件名、编码格式、采样率与码率近似匹配网易云音质标识
- [x] `.rhineignore` 或 `.foliaignore` 排除规则，支持 `*`、`?`、`**`、目录末尾 `/`、根路径前导 `/` 与 `!` 否定
- [x] 重新扫描、取消扫描；移除导入位置只删除索引，不删除磁盘文件
- [x] 离线时已导入的曲目、封面与本地歌词仍可播放

### 面板本身

- [x] 三维光盘盒、唱片与资料库动画
- [x] 原生全屏与窗口模式切换
- [x] 按歌手或专辑分组浏览已载入结果
- [x] 网易云 / QQ / 酷狗直接选择与账号、请求、缓存隔离
- [x] 播放、暂停、切歌、进度与音量控制
- [x] 播放栏歌词入口、当前歌曲歌词页面、点击歌词跳转、单曲时间校准与离线歌词缓存

- [x] 播放队列、删除待播曲目、循环与随机播放
- [x] 搜索历史、播放历史与本机偏好保存
- [x] 亮色、暗色、跟随系统与减少动效
- [x] 音质选择、试听提示与可选替代音源
- [x] macOS 系统媒体按钮
- [x] macOS Apple Silicon、Linux x86_64 与 Windows x64 构建和打包
- [ ] MV 播放

**音质和完整播放取决于账号权限与音源可用性；返回试听时会明确提示。替代音源默认关闭，开启时使用锁定上游的匹配实现，音质由匹配来源决定，失败时可使用官方入口。本项目不提供音乐下载。**

## 从源码构建

需要包含 Shader Tools 和 Multimedia 的 Qt **6.12+** desktop kit、CMake 3.24+、Ninja、Python 3、Node.js / npm，以及平台 C++ 编译器。本地与三平台自动构建统一使用 Qt 6.12.0，自动构建使用 Node.js 22.14.0。Linux 还需要 Qt 的私有开发头文件（发行版包名通常是 `qt6-base-private-dev`、`qt6-multimedia-private-dev`）。

`QT_ROOT` 指向 Qt kit 根目录。`RHINE_NODE_BINARY` 和 `RHINE_NODE_LICENSE` 指向官方独立 Node 分发包中的可执行文件与对应许可证；该运行时会随应用打包，不提交到源码仓库。

### macOS

安装 Xcode Command Line Tools 和匹配处理器的 Qt kit，在项目目录执行：

```sh
export QT_ROOT="$HOME/Qt/6.12.0/macos"
export RHINE_NODE_BINARY="/absolute/path/to/node/bin/node"
export RHINE_NODE_LICENSE="/absolute/path/to/node/LICENSE"
npm ci
npm run build
npm test
npm run share
```

本机已有 `vendor/node-arm64/node` 与许可证时可作为默认运行时。`QT_ROOT` 未设置时使用 `~/Qt/6.12.0/macos`。

### Windows x64

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”组件、Windows SDK 和 Qt MSVC 2022 64 位 kit。在 **Developer PowerShell for VS 2022** 中进入项目目录：

```powershell
$env:QT_ROOT = 'C:\Qt\6.12.0\msvc2022_64'
$env:RHINE_NODE_BINARY = 'C:\tools\node-v22.14.0-win-x64\node.exe'
$env:RHINE_NODE_LICENSE = 'C:\tools\node-v22.14.0-win-x64\LICENSE'
npm ci
npm run build
npm test
npm run share
```

Windows 的 `npm run share` 使用 [Inno Setup 6.3+ 或 7](https://jrsoftware.org/isdl.php)，
只生成 `output/RhineMusicDatabase-<构建标识>-Windows-x64-Setup.exe` 安装程序。
安装器默认安装到 `%LOCALAPPDATA%\Programs\RhineMusicDatabase`，无需管理员权限，提供开始菜单快捷方式、可选桌面快捷方式和卸载入口。
应用在安装目录的 `data` 中保存账号、设置和缓存，请选择当前用户可写的目录。安装包排除构建机上的运行数据，覆盖安装和卸载保留用户的 `data`；彻底清理时可手动删除。

### Linux x86_64（AppImage）

安装 Qt 6.12.0 开发包（含 Shader Tools、Multimedia 与私有头文件）、CMake、Ninja、GCC、Python 3 和 Node.js 22；若发行版尚未提供此版本，使用官方 desktop kit 并设置 `QT_ROOT`。Qt 由系统提供时 `QT_ROOT` 保持默认的 `/usr` 即可：

```sh
export RHINE_NODE_BINARY="/absolute/path/to/node-v22.14.0-linux-x64/bin/node"
export RHINE_NODE_LICENSE="/absolute/path/to/node-v22.14.0-linux-x64/LICENSE"
npm ci
npm run build
npm test
npm run share
```

`npm run build` 先按依赖闭包收集 Qt 库、平台插件、图像与 TLS 插件和 QML 模块到 `output/RhineMusicDatabase.AppDir`，再用 `appimagetool` 生成 `output/RhineMusicDatabase-<构建标识>-Linux-x86_64.AppImage`。`appimagetool` 需自行安装并出现在 `PATH` 中，也可用 `APPIMAGE_TOOL` 指定路径；没有 FUSE 时脚本自动改用 `--appimage-extract-and-run`。

随包的 Node 必须是官方独立分发包：构建脚本会检查 `ldd` 结果，只依赖 glibc 运行库的构建才会被打进 AppImage，发行版自己的 `/usr/bin/node` 动态链接了系统 ICU 等库，会被拒绝。

### 开发与测试

默认构建类型为 `Release`。设置 `RHINE_BUILD_TYPE=Debug` 可构建调试版本；`PYTHON` 可覆盖 Python 命令。Qt 6.12+ 安装包含官方 QML Live Preview 工具时，可使用 `npm run preview` 进行热预览。

设置 `RHINE_VERSION=vX.Y.Z` 时构建正式版，分发包的构建标识为 `vX.Y.Z`。
未指定版本时为开发版：设置中显示最后一次提交标题、12 位短 SHA 和“开发版”，三平台分发包均以短 SHA 作为构建标识。
检查更新仅获取发布信息；点击“前往更新”会打开 [GitHub releases 页面](https://github.com/MikeQu223/Rhine-Music-Database/releases)，由浏览器下载，不在应用内下载文件。

`npm test` 使用本地响应验证场景、拾取、音乐状态、账号隔离、锁定上游、打包通信和部署资源。设置 `RHINE_GPU_TEST=1` 可额外执行真实 GPU 离线渲染检查；Windows 设置 `RHINE_SOFTWARE_GPU=1` 可使用软件 GPU。离线测试不替代真实账号、在线音源、显示缩放与系统集成的实机检查。

Qt RHI 使用私有头文件，升级 Qt 后需重新编译并验证。

## iOS / iPadOS 构建

需要 macOS、完整 Xcode、Qt 6.12.0 的 `ios_device` kit，以及已安装的桌面构建依赖。
执行 `npm run build:ios`，生成 `output/RhineMusicDatabase-ios-unsigned.ipa`；该文件需自行签名后安装。
`RHINE_IOS_QT` 可以指定其他同版本 kit 路径。构建自动校验 NodeMobile 下载哈希，并检查包内架构、资源和动态依赖。

## 本机数据与隐私

搜索、歌曲资料、扫码与个人音乐库请求发送至所选音乐平台，封面从网易云、QQ 或酷狗音乐的图片域名加载。原始认证记录仅由 C++ 和 Node 桥处理；macOS 保存在钥匙串，Windows 通过 DPAPI 加密，Linux 保存在 `~/.local/share/RhineMusicDatabase/rhine.music.database/account.credential`（权限 `0600`）。本项目没有自建的数据收集服务器、分析 SDK 或自动上传诊断功能。

登录为可选功能。应用不读取浏览器或官方客户端的登录信息。

## 性能面板与诊断

在「设置」中向下滚动至「性能与诊断」，点击「显示 / 隐藏 · F10」，即可打开性能面板；
也可以按 F10（部分笔记本需 Fn+F10）。设置内容支持滚轮、触控板、上下方向键与侧边箭头滚动，
原有菜单尺寸和间距保持不变。性能面板支持精简与展开。
面板显示实际 Qt 呈现帧率、场景提交帧率、帧间隔 P50/P95/P99 与峰值、逻辑/同步/提交 CPU 耗时、
主进程常驻内存及渲染资源。程序采用按需绘制，静止时低帧率和较长帧间隔属于正常休眠；
Qt 帧率包含面板刷新。CPU 百分比以单个逻辑核心为 100%，内存和 CPU 不包含 Node 音乐桥接子进程。
GPU 耗时/占用未采集，同步和提交的 CPU 时间不代表 GPU 执行时间。

点击「开始收集」，复现卡顿后点击「停止并导出」，会在系统文稿目录下的 `RHINE Diagnostics`
生成 JSON 诊断文件；点击「诊断目录」可打开所在文件夹。每半秒汇总一次，另保留最多 36,000 帧的逐帧呈现/CPU 计时（超出数量在文件中注明），
最长十分钟自动导出，
退出程序时也会保存进行中的录制。导出失败可重试。文件只包含性能数值和系统/图形后端信息，
不包含账号、Cookie、搜索词、曲目、请求内容或截图。关闭面板且未录制时停止性能采样。

## 参考项目

- **[NeteaseCloudMusicApiEnhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced)**: 网易云接入
- **[MakcRe/KuGouMusicApi](https://github.com/MakcRe/KuGouMusicApi)**: 酷狗音乐标准版及概念版接入
- **[yakult-green-tea/qq-music-api](https://github.com/yakult-green-tea/qq-music-api)**: QQ 音乐接入
- **[LBEILC / RhineLabUI](https://github.com/LBEILC/RhineLabUI)**：特别感谢原界面、模型与场景作者，版权声明随项目保留。
- **[LyricsX](https://github.com/MxIris-LyricsX-Project/LyricsX)**：歌词获取、解析与显示分层参考。
- **[LyricsKit](https://github.com/MxIris-LyricsX-Project/LyricsKit)**：LRC、YRC、KRC 格式解析移植，
  `MusicBridge/lyrics-format.cjs` 单独采用 MPL-2.0，固定版本与许可随包保留。
- **[Lyrimuse](https://github.com/Yudaotor/lyrimuse)**：统一时间轴、歌词同步与录音版本匹配思路参考。


## 开源许可

本项目保留 [MIT License](LICENSE) 与原作者版权声明。第三方字体、Qt 和依赖分别遵循各自许可证；相关许可随部署包保留。

《明日方舟》、莱茵生命及其名称、标志与原作视觉设计的相关权利归各自权利人所有。本项目为非官方衍生作品，项目的 MIT 许可不代替第三方内容的授权。
