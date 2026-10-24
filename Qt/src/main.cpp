#include "AppIdentity.h"
#include "AppStorage.h"
#include "Engine.h"
#include "FrameProfiler.h"
#include "GraphicsBackend.h"
#include "Hud.h"
#include "RhiScene.h"
#include "OpticalScene.h"
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QSettings>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <cstring>
/* Qt 的 Vulkan 类型只在「Qt 带 Vulkan 特性」且「应用侧能看到 vulkan.h」时声明：
 *   qvulkaninstance.h:  #if QT_CONFIG(vulkan) || defined(Q_QDOC)
 *                      #if __has_include(<vulkan/vulkan.h>) || defined(Q_QDOC)
 *   qrhi_platform.h:    #if (QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)) || defined(Q_QDOC)
 * 只判 QT_CONFIG(vulkan) 会在装了 Qt 但没装 Vulkan SDK 的机器上
 * （如 Windows runner：Qt 自带 Vulkan 特性、系统无 vulkan.h）编译出使用未声明类型的代码，
 * 表现为 QVulkanInstance 不完整类型与 VK_SUCCESS 未定义。守卫必须同时覆盖两半。 */
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && !defined(Q_OS_MACOS)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#define RHINE_HAVE_VULKAN_TYPES 1
#else
#define RHINE_HAVE_VULKAN_TYPES 0
#endif
int makeIcon(const QString &directory);
int runUiChecks(Engine &backend, QQuickWindow *window, const QString &output, FrameProfiler &profiler);
#ifdef Q_OS_WIN
void initializeWindowsGraphics();
#endif
#if !defined(Q_OS_MACOS)
/**
 * 探测是否存在可用的 Vulkan 物理设备。
 * Qt 的 QSG_RHI_BACKEND 没有后端回退列表，强制指定失败会直接终止，因此必须先探测。
 */
static bool vulkanDeviceAvailable() {
#if RHINE_HAVE_VULKAN_TYPES
    QVulkanInstance probe;
    probe.setApiVersion(QVersionNumber(1, 0));
    if (!probe.create()) {
        return false;
    }
    // QVulkanInstance 不公开设备列表，直接按 Vulkan API 枚举，避免依赖 QVulkanWindow。
    quint32 devices = 0;
    return probe.functions()->vkEnumeratePhysicalDevices(probe.vkInstance(), &devices, nullptr) ==
               VK_SUCCESS &&
           devices > 0;
#else
    return false;
#endif
}
#endif
/**
 * 选择场景图后端：显式 RHINE_GRAPHICS 优先，否则可用时使用 Vulkan。
 * 3D 场景与全屏后处理链在 Vulkan 上可省去驱动侧的隐式状态验证，Windows 与 Linux 均适用。
 */
/** 图形设置的持久化位置：与 Host::preference 相同（Windows 便携 data，其余平台为应用配置）。
 *  设置页经 Host::preference 写入 panel/graphics-backend，这里必须读同一个文件，
 *  否则设置页的切换在下次启动时读不到——上一轮正是败在这里。 */
static QSettings graphicsSettings() {
    return AppStorage::settings();
}
/** 应用命名后端；Vulkan 先探测物理设备，不可用时返回 false 由调用方回退。
 *  仅在非 macOS 下编译：macOS 只提供 Metal，本函数的两个调用点都在 #else 分支里，
 *  而 vulkanDeviceAvailable() 同样只在非 macOS 下定义。 */
#if !defined(Q_OS_MACOS)
static bool applyNamedBackend(const QString &requested) {
#if defined(Q_OS_WIN)
    if (requested == "d3d12") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
        RhineGraphics::setActiveBackend(QStringLiteral("d3d12"));
        return true;
    }
#endif
    if (requested == "opengl") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
        RhineGraphics::setActiveBackend(QStringLiteral("opengl"));
        return true;
    }
    if (requested == "vulkan") {
        if (!vulkanDeviceAvailable()) {
            qWarning() << "Vulkan is unavailable; falling back to the platform default backend.";
            return false;
        }
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        RhineGraphics::setActiveBackend(QStringLiteral("vulkan"));
        return true;
    }
    return false;
}
#endif
static void selectGraphicsBackend() {
    // Qt 自己的变量优先级最高，保留它才能用 QSG_RHI_BACKEND 做后端对照。
    if (!qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        qInfo() << "QSG_RHI_BACKEND is set; keeping" << qEnvironmentVariable("QSG_RHI_BACKEND");
        return;
    }
#if defined(Q_OS_MACOS)
    // Qt 在 macOS 只提供 Metal，Vulkan 需经 MoltenVK 转换层，性能低于原生 Metal。
    const auto requested = qEnvironmentVariable("RHINE_GRAPHICS").toLower();
    if (!requested.isEmpty() && requested != "auto" && requested != "metal") {
        qWarning() << "RHINE_GRAPHICS" << requested << "is unavailable on macOS; using Metal.";
    }
#else
    auto settings = graphicsSettings();
    const auto requested = qEnvironmentVariable("RHINE_GRAPHICS").toLower();
    // 显式设置的环境变量只对本次启动生效：写回配置会把一次诊断运行变成用户偏好，
    // 设置页随后就会显示成后端"自己改回去了"。
    if (!requested.isEmpty() && requested != "auto") {
        if (applyNamedBackend(requested)) {
            qInfo() << "RHINE_GRAPHICS" << requested << "applied for this launch only.";
            return;
        }
        // 已移除的 d3d11 或探测失败的 Vulkan 不能就此返回：Qt 在 Windows 的平台默认
        // 后端是 D3D11，返回等于把它又请回来，所以继续走下面的默认选择。
        qWarning() << "RHINE_GRAPHICS" << requested << "is unavailable; using the default backend.";
    }
    // 其次是设置页/上次启动保存的选择（与 Host::preference 的 "panel/" 前缀一致）。
    const auto saved = settings.value("panel/graphics-backend").toString().toLower();
    if (!saved.isEmpty()) {
        if (applyNamedBackend(saved)) {
            qInfo() << "Using saved graphics backend:" << saved;
            return;
        }
        // 旧配置里可能还留着已移除的 d3d11；同样不能直接返回，否则会落回 D3D11。
        qWarning() << "Saved graphics backend" << saved << "is unavailable; using the default backend.";
    }
    // 都没有：Windows 上固定 D3D12，其余平台可用时默认 Vulkan。
#if defined(Q_OS_WIN)
    // Windows 选 D3D12 而不是 Vulkan：QT_D3D_ADAPTER_INDEX（默认独显）只在 D3D 系生效，
    // 而且 FSR3 在 D3D12 上走厂商原生路径。本项目已移除 D3D11，必须显式指定，
    // 否则 Qt 的平台默认后端会把它请回来。
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);
    RhineGraphics::setActiveBackend(QStringLiteral("d3d12"));
    qInfo() << "No backend configured; using Direct3D 12 (default on Windows).";
#else
    if (vulkanDeviceAvailable()) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        RhineGraphics::setActiveBackend(QStringLiteral("vulkan"));
        qInfo() << "No backend configured; Vulkan detected and selected as default.";
    } else {
        qInfo() << "No backend configured; Vulkan unavailable, using the platform default.";
    }
#endif
#endif
}
/**
 * 垂直同步：关闭时把交换链的 swapInterval 设为 0，允许帧率超过屏幕刷新率。
 * 必须在创建任何 QQuickWindow 之前写入默认格式，Qt 的 RHI 交换链会据此选择呈现模式。
 */
static void applyVSyncSetting() {
    if (!qEnvironmentVariableIsEmpty("RHINE_VSYNC")) {
        return;
    }
    if (graphicsSettings().value("panel/vsync").toString() != "off") {
        return;
    }
    auto format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);
}
/** 超采样倍率持久化：配置里设 graphics/supersample=2 即渲染 2 倍分辨率再缩回窗口。 */
static void applySupersampleSetting() {
    if (!qEnvironmentVariableIsEmpty("RHINE_SUPERSAMPLE")) {
        return;
    }
    const auto sample = graphicsSettings().value("panel/supersample", 1).toInt();
    if (sample > 1) {
        qputenv("RHINE_SUPERSAMPLE", QByteArray::number(qMin(sample, 2)));
    }
}
/**
 * 抗锯齿模式：panel/aa-mode = off/fxaa/ssaa，默认关闭且互斥单选。
 * 倍率 panel/aa-scale 只对 SSAA 生效，与渲染分辨率相乘（75% × 125% = 93.75%）。
 */
static void applyAntialiasingSetting() {
    if (qEnvironmentVariableIsEmpty("RHINE_AA_MODE")) {
        qputenv("RHINE_AA_MODE", graphicsSettings().value("panel/aa-mode", "off").toString().toUtf8());
    }
    if (qEnvironmentVariableIsEmpty("RHINE_AA_SCALE")) {
        qputenv("RHINE_AA_SCALE",
                QByteArray::number(graphicsSettings().value("panel/aa-scale", 125).toInt()));
    }
    // FSR 专属的两档设置：超分档位决定内部渲染分辨率，锐化强度决定 RCAS 强度。
    // 两者都只在 FSR 模式下被渲染器读取，非 FSR 模式留着也不影响。
    if (qEnvironmentVariableIsEmpty("RHINE_FSR_QUALITY")) {
        qputenv("RHINE_FSR_QUALITY",
                graphicsSettings().value("panel/fsr-quality", "native").toString().toUtf8());
    }
    if (qEnvironmentVariableIsEmpty("RHINE_FSR_SHARPNESS")) {
        qputenv("RHINE_FSR_SHARPNESS",
                QByteArray::number(graphicsSettings().value("panel/fsr-sharpness", 50).toInt()));
    }
    // MSAA 采样数：只在设置里选过才写入，未选过则保持空，让渲染器走"目标 4x + 设备能力下取"。
    if (qEnvironmentVariableIsEmpty("RHINE_MSAA")) {
        const auto samples = graphicsSettings().value("panel/msaa", 0).toInt();
        if (samples > 1) {
            qputenv("RHINE_MSAA", QByteArray::number(samples));
        }
    }
}
/**
 * 帧生成：panel/fg = on 时开启。渲染器每帧读 RHINE_FG，与抗锯齿模式互相独立，
 * 所以单独持久化、单独下发，切换时不需要重启。
 */
static void applyFrameGenerationSetting() {
    if (qEnvironmentVariableIsEmpty("RHINE_FG")) {
        qputenv("RHINE_FG", graphicsSettings().value("panel/fg", "off").toString().toUtf8());
    }
}
/**
 * 渲染分辨率百分比：panel/render-scale=75 表示按窗口的 75% 渲染，再由最终合成通道放大。
 * 集成显卡上这是最直接的一档性能/画质交换，50 大致相当于像素数降到 1/4。
 */
static void applyRenderScaleSetting() {
    if (!qEnvironmentVariableIsEmpty("RHINE_RENDER_SCALE")) {
        return;
    }
    const auto percent = graphicsSettings().value("panel/render-scale", 100).toInt();
    if (percent > 0 && percent != 100) {
        qputenv("RHINE_RENDER_SCALE", QByteArray::number(qBound(50, percent, 100)));
    }
}
/**
 * 建立应用身份、Qt 引擎和原生窗口。
 * 离线测试与正式播放沿用同一入口，资源只从独立应用读取。
 */
int main(int argc, char **argv) {
#ifdef Q_OS_WIN
    const auto storageError = AppStorage::initializePortable(AppStorage::executableDirectory());
    if (!storageError.isEmpty()) {
        std::fprintf(stderr, "%s\n", storageError.toUtf8().constData());
        return 1;
    }
#endif
    // 两个平台都明确使用 FFmpeg，设备切换和重连由播放器独立处理。
    // 必须在 Qt 初始化多媒体前选择后端，显式环境变量仍可用于兼容性对照。
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND")) {
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    }
    // Windows GUI 子系统默认可能将 Qt 日志送到调试器；测试必须写入 CTest 的标准错误管道。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0 || std::strcmp(argv[i], "--ui-self-test") == 0) {
            qInstallMessageHandler(
                [](QtMsgType type, const QMessageLogContext &context, const QString &message) {
                    const auto text = qFormatLogMessage(type, context, message).toUtf8();
                    std::fprintf(stderr, "%s\n", text.constData());
                    std::fflush(stderr);
                });
            break;
        }
    }
    QLoggingCategory::setFilterRules("qt.qml.usedbeforedeclared=false");
#ifdef Q_OS_MACOS
    // Qt 需在创建 QGuiApplication 前找到包内平台插件；某些 kit 会忽略资源目录里的 qt.conf。
    const QDir bundledPlugins(QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath() + "/../PlugIns");
    if (bundledPlugins.exists("platforms/libqcocoa.dylib")) {
        qputenv("QT_PLUGIN_PATH", bundledPlugins.absolutePath().toUtf8());
    }
#endif
#ifdef Q_OS_WIN
    initializeWindowsGraphics();
#endif
    QGuiApplication application(argc, argv);
    application.setWindowIcon(QIcon(":/icons/icon-256.png"));
    QCoreApplication::setApplicationVersion(AppUpdater::currentVersion());
    QCoreApplication::setOrganizationName(AppIdentity::Organization);
    QCoreApplication::setOrganizationDomain(AppIdentity::Id);
    QCoreApplication::setApplicationName(AppIdentity::Id);
    QGuiApplication::setApplicationDisplayName(AppIdentity::DisplayName);
    // 必须在创建任何 QQuickWindow 之前确定后端；组织名先落位，配置路径才能落到应用数据目录。
    selectGraphicsBackend();
    applyVSyncSetting();
    applySupersampleSetting();
    applyRenderScaleSetting();
    applyAntialiasingSetting();
    applyFrameGenerationSetting();
    const auto args = application.arguments();
    if (args.contains("--make-icon")) {
        return makeIcon(args.value(args.indexOf("--make-icon") + 1));
    }
    QString root = qEnvironmentVariable("RHINE_RESOURCES");
    if (root.isEmpty()) {
        // macOS 使用 Resources ；其他平台使用 resources
        const auto base = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
        const QStringList candidates{base + "/../Resources"};
#else
        const QStringList candidates{base + "/resources", base + "/../resources"};
#endif
        for (const auto &candidate : candidates) {
            if (QFileInfo(candidate + "/slots.json").exists()) {
                root = QDir(candidate).absolutePath();
                break;
            }
        }
        if (root.isEmpty()) {
            root = QDir::currentPath() + "/output/resources";
        }
    }
    const bool test = args.contains("--self-test");
    const bool visual = args.contains("--visual-fixtures");
    const bool offline = test || visual || args.contains("--ui-self-test") || args.contains("--offline") ||
                         qEnvironmentVariableIntValue("RHINE_OFFLINE") == 1;
    Engine backend(root, offline);
    if (test) {
        return backend.selfTest();
    }
    if (visual) {
        const int i = args.indexOf("--visual-fixtures");
        return backend.renderFixtures(args.value(i + 1), args.value(i + 2));
    }
    QString profilePath;
    if (args.contains("--frame-profile")) {
        profilePath = args.value(args.indexOf("--frame-profile") + 1);
        if (profilePath.isEmpty() || profilePath.startsWith("--")) {
            qWarning() << "--frame-profile requires an output CSV path";
            return 1;
        }
    }
    FrameProfiler profiler(&backend, profilePath);
    QObject::connect(&backend, &Engine::performanceRequested, &profiler, &FrameProfiler::toggleVisible);
    backend.start();
    qmlRegisterType<RhiScene>("Rhine.Native", 1, 0, "RhiScene");
    qmlRegisterType<OpticalScene>("Rhine.Native", 1, 0, "OpticalScene");
    qmlRegisterType<Hud>("Rhine.Native", 1, 0, "Hud");
    qmlRegisterUncreatableType<Engine>("Rhine.Native", 1, 0, "Engine", "Provided by application");
    QQmlApplicationEngine engine;
    // 无边框
    if (qEnvironmentVariableIntValue("RHINE_FRAMELESS_PREVIEW") == 1) {
        engine.setInitialProperties({{"windowsFrameless", true}});
    }
#ifdef Q_OS_WIN
    // 创建窗口时先保持隐藏，配置管线缓存后再显示，避免首次渲染写到系统缓存。
    engine.setInitialProperties({{"visible", false}, {"windowsFrameless", true}});
#endif
    const auto fontId = QFontDatabase::addApplicationFont(root + "/RhineMiSans-regular.otf");
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    engine.rootContext()->setContextProperty("rhineFontFamily",
                                             families.isEmpty() ? QString("sans-serif") : families.first());
    engine.rootContext()->setContextProperty("rhine", &backend);
    engine.rootContext()->setContextProperty("profiler", &profiler);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("RhineMusic", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
#ifdef Q_OS_WIN
    QQuickGraphicsConfiguration graphics;
    graphics.setAutomaticPipelineCache(false);
    const auto pipelineCache = AppStorage::cacheDirectory() + "/shaders/pipeline.cache";
    if (QFileInfo::exists(pipelineCache)) {
        graphics.setPipelineCacheLoadFile(pipelineCache);
    }
    graphics.setPipelineCacheSaveFile(pipelineCache);
    window->setGraphicsConfiguration(graphics);
    window->show();
#endif
    backend.setWindow(window);
    if (!offline && RHINE_RELEASE_BUILD) {
        QTimer::singleShot(8000, &backend.host, [&backend] {
            if (backend.host.preference("auto-update") != "off") {
                backend.host.updater.check();
            }
        });
    }
    window->installEventFilter(&backend);
    profiler.attach(window);
    // 短暂切换窗口继续复用 GPU；隐藏或最小化超过两秒才释放图形设备和可重建缓存。
    auto backgroundRelease = new QTimer(window);
    backgroundRelease->setSingleShot(true);
    backgroundRelease->setInterval(2000);
    QObject::connect(backgroundRelease, &QTimer::timeout, window, [&backend, window] {
        if (window->isVisible() && window->visibility() != QWindow::Minimized) {
            return;
        }
        backend.trimMemory();
        window->setPersistentSceneGraph(false);
        window->setPersistentGraphics(false);
        window->releaseResources();
    });
    QObject::connect(window, &QWindow::visibilityChanged, window,
                     [window, backgroundRelease](QWindow::Visibility visibility) {
                         if (visibility == QWindow::Hidden || visibility == QWindow::Minimized) {
                             backgroundRelease->start();
                         } else {
                             backgroundRelease->stop();
                             window->setPersistentSceneGraph(true);
                             window->setPersistentGraphics(true);
                         }
                     });
    QObject::connect(
        window, &QQuickWindow::sceneGraphInvalidated, &backend,
        [&backend] { backend.recordResourceRelease(); }, Qt::DirectConnection);
    if (args.contains("--ui-self-test")) {
        return runUiChecks(backend, qobject_cast<QQuickWindow *>(engine.rootObjects().first()),
                           args.value(args.indexOf("--ui-self-test") + 1), profiler);
    }
    if (args.contains("--capture")) {
        const auto index = args.indexOf("--capture");
        const auto path = args.value(index + 1);
        // 第三个参数可选场景模式（archive / detail），便于直接验证某个状态的渲染输出。
        const auto captureMode = args.value(index + 2);
        if (path.isEmpty()) {
            return 1;
        }
        auto poll = new QTimer(&backend);
        poll->setInterval(200);
        bool *reviewed = new bool(false);
        QObject::connect(poll, &QTimer::timeout, &backend, [&, path, captureMode, poll, reviewed] {
            if (!backend.ready()) {
                return;
            }
            if (!*reviewed) {
                *reviewed = true;
                backend.scene.review(captureMode.isEmpty() || captureMode.startsWith("--")
                                         ? QStringLiteral("archive")
                                         : captureMode);
                return;
            }
            backend.step(50);
            if (backend.frame().isEmpty()) {
                return;
            }
            poll->stop();
            delete reviewed;
            backend.snapshot(path);
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (window) {
                window->grabWindow().save(path + "/qt-window.png");
            }
            application.quit();
        });
        poll->start();
    }
    return application.exec();
}
