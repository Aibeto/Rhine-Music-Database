#include "PresentationShell.h"
#ifdef Q_OS_DARWIN
#include "MetalFxBackend.h"
#endif

#include "../fg/FgBackend.h"
#include "../fg/FgCapability.h"
#include "../fg/FgConfig.h"
#include "../fg/FgFrameTap.h"
#include "PresentationWindow.h"
#include "SceneLayerHost.h"

#include "../Engine.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QList>
#include <QMatrix4x4>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QWindow>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// 合成通道的 uniform：clipSpaceCorrMatrix（64）+ params vec4（16）。
constexpr int kFgUniformSize = 64 + 16;

QShader loadShader(const QString &name) {
    QFile f(":/shaders/" + name + ".qsb");
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "FG: 缺少着色器" << name;
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

quint64 bindingKey(const QRhiTexture *a, const QRhiTexture *b) {
    return (quint64(quintptr(a)) * 0x9E3779B97F4A7C15ull) ^ quint64(quintptr(b));
}

} // namespace

PresentationShell::PresentationShell(QQmlEngine &engine, Engine &backend, QString backendName,
                                     QObject *parent)
    : QObject(parent), m_quitTransition(
                           [this] {
                               shutdown();
                               QCoreApplication::quit();
                           },
                           this),
      m_engine(&engine), m_backend(&backend), m_backendName(std::move(backendName)) {
}

/** 关闭请求先停止呈现；Windows 立即隐藏但保留 HWND，待输入回调退出后再清理 GPU。 */
void PresentationShell::requestQuit() {
    if (m_quitRequested || m_stopped) {
        return;
    }
    m_quitRequested = true;
    if (m_timer) {
        m_timer->stop();
    }
    if (m_releaseTimer) {
        m_releaseTimer->stop();
    }
#ifdef Q_OS_WIN
    // 隐藏只改变可见性，原生窗口与 Vulkan surface 必须保留到交换链清理完成。
    if (m_window) {
        m_window->hide();
    }
#endif
    m_quitTransition.request();
}

/** 停调度并按 GPU 引用顺序释放资源；原生窗口留到析构，避免在自身事件处理中销毁。 */
void PresentationShell::shutdown() {
    m_quitTransition.cancel();
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    if (m_timer) {
        m_timer->stop();
    }
    if (m_releaseTimer) {
        m_releaseTimer->stop();
    }
    RhineFg::setEnabled(false);
#ifdef Q_OS_DARWIN
    m_backend->metalFxInputs = {};
#endif
    if (m_rhi) {
        m_rhi->finish();
    }
    releaseSwapchainResources();
    m_fg.reset();
    // 离屏 QML 即将销毁，先断开更新与输入法焦点，避免清理期间重新访问旧宿主。
    m_backend->disconnect(this);
    if (auto *inputWindow = qobject_cast<RhineRender::PresentationWindow *>(m_window)) {
        inputWindow->setInputWindow(nullptr);
    }
    m_host.reset();
    delete m_pipelineBindings;
    m_pipelineBindings = nullptr;
    for (auto *resource : std::as_const(m_ownedResources)) {
        delete resource;
    }
    m_ownedResources.clear();
    if (m_presenter) {
        m_presenter->release();
    }
    RhineFg::clear();
    m_backend->setWindow(nullptr);
    m_engine->rootContext()->setContextProperty(QStringLiteral("rhineHostWindow"),
                                                QVariant::fromValue(static_cast<QWindow *>(nullptr)));
}

PresentationShell::~PresentationShell() {
    shutdown();
    m_presenter.reset();
}

bool PresentationShell::compiledIn() {
#if defined(Q_OS_DARWIN) && __has_include(<MetalFX/MTLFXFrameInterpolator.h>)
    return true;
#elif defined(RHINE_FSR3_VULKAN) && RHINE_FSR3_VULKAN
    return FgBackend::compiledIn();
#else
    return false;
#endif
}

bool PresentationShell::supportsBackend(const QString &name) {
#if QT_CONFIG(metal)
    if (name == QLatin1String("metal")) {
        return true;
    }
#endif
#if defined(RHINE_VULKAN_PRESENTATION) && RHINE_VULKAN_PRESENTATION
    return name == QLatin1String("vulkan");
#else
    Q_UNUSED(name);
    return false;
#endif
}

bool PresentationShell::requested() const {
#ifdef Q_OS_DARWIN
    return m_backend->renderConfiguration.snapshot().interpolation;
#else
    return FgConfig::fromEnvironment().enabled;
#endif
}

QWindow *PresentationShell::window() const {
    return m_window;
}

QString PresentationShell::resolveQmlUrl(const QString &file) const {
    // qt_add_qml_module 的默认资源前缀是 /qt/qml；也兼容直接以 URI 命名的老布局。
    const QStringList candidates{
        QStringLiteral("qrc:/qt/qml/RhineMusic/") + file,
        QStringLiteral("qrc:/RhineMusic/") + file,
        QStringLiteral("qrc:/") + file,
    };
    for (const auto &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return candidates.first();
}

bool PresentationShell::initializeBackend() {
    m_presenter = RhineRender::createPresentationBackend(m_backendName);
    if (!m_presenter || !m_presenter->initialize(m_size, FgConfig::fromEnvironment().vsync)) {
        m_error = m_presenter ? m_presenter->lastError() : QStringLiteral("当前呈现后端未编入");
        return false;
    }
    m_window = m_presenter->window();
    m_rhi = m_presenter->rhi();
    m_window->installEventFilter(this);
    m_size = m_window->size();
    return true;
}

bool PresentationShell::initializeLayers() {
    // Overlay.qml 的窗口行为（拖动/最大化/关闭/全屏）原来由 Main.qml 的 Window 提供；
    // FG 路径没有那个 Window，改为把真实的 QWindow 注入上下文。
    m_engine->rootContext()->setContextProperty(QStringLiteral("rhineHostWindow"),
                                                QVariant::fromValue(m_window));

    m_host = std::make_unique<SceneLayerHost>();
    m_host->requestRender = [this] { scheduleNext(); };
    if (!m_host->start(*m_engine, QUrl(resolveQmlUrl(QStringLiteral("Scene3D.qml"))),
                       QUrl(resolveQmlUrl(QStringLiteral("Overlay.qml"))), m_rhi,
                       m_presenter->vulkanInstance(), m_window)) {
        m_error = QStringLiteral("离屏场景建立失败：%1").arg(m_host->lastError());
        return false;
    }
    m_host->resize(m_size);
    // 纹理按物理尺寸分配；Qt Quick 的排版和输入始终保持逻辑像素。
    m_dpr = m_window->devicePixelRatio();
    m_windowDpr = m_dpr;
    m_host->setPixelRatio(m_dpr);
    // 键盘链路的第一环：焦点。
    //
    // 默认路径靠 Main.qml 的 Component.onCompleted → overlay.focusInputLayer() 把焦点
    // 交给 Overlay 里的 inputLayer；FG 路径不加载 Main.qml，没人做这件事。而
    // QQuickRenderControl 的离屏窗口没有平台窗口、永远不会被激活，Qt 也不会像真实
    // 窗口那样在激活时自动传播 activeFocus——实测此时 window.activeFocusItem() 为
    // null，QQuickWindow 收到 KeyPress 后无处投递，按键被直接丢弃。
    // 显式恢复当前面板的焦点；重建后可能仍在搜索框，不能统一交回快捷键层。
    if (QQuickItem *overlayRoot = m_host->overlayRootItem()) {
        const bool focused =
            QMetaObject::invokeMethod(overlayRoot, "restoreInputFocus", Qt::DirectConnection);
        QQuickItem *focusItem = m_host->overlayWindow()->activeFocusItem();
        qInfo().noquote() << "FG: 覆盖层输入焦点" << (focused && focusItem ? "已建立" : "未建立")
                          << "activeFocusItem:"
                          << (focusItem ? focusItem->metaObject()->className() : "null");
    }
    // 把平台输入法接到离屏输入项上
    if (auto *inputWindow = qobject_cast<RhineRender::PresentationWindow *>(m_window)) {
        inputWindow->setInputWindow(m_host->overlayWindow());
    }
    // 只在这里打开每帧输入通道：RhiScene 的渲染器只在 tap 打开时才发布。
#ifdef Q_OS_DARWIN
    // Main.qml 未加载时仍需接上原窗口的资源统计清理，否则隐藏后会残留旧计数。
    QObject::connect(
        m_host->sceneWindow(), &QQuickWindow::sceneGraphInvalidated, m_backend,
        [this] { m_backend->recordResourceRelease(); }, Qt::DirectConnection);
    const bool temporalAvailable = RhineRender::MetalFxTemporal::supported(m_rhi);
    m_backend->renderConfiguration.setNativeTemporalAvailable(temporalAvailable);
    RhineFg::capabilityMutable().temporalAvailable = temporalAvailable;
    m_fg = RhineRender::createMetalFxFrameGenerator(m_rhi);
    m_fgAvailable = bool(m_fg);
#else
    m_fgAvailable = FgBackend::supported(m_rhi);
    if (m_fgAvailable) {
        m_fg = std::make_unique<FgBackend>();
    }
#endif
#ifdef Q_OS_DARWIN
    RhineFg::setEnabled(false);
    m_backend->metalFxInputs = {};
#else
    RhineFg::setEnabled(m_fgAvailable);
#endif
    return true;
}

QSize PresentationShell::pixelSize() const {
#ifdef Q_OS_LINUX
    if (m_backendName == "vulkan" && m_presenter && !m_presenter->extent().isEmpty()) {
        return m_presenter->extent();
    }
#endif
    return QSize(qMax(1, qRound(m_size.width() * m_dpr)), qMax(1, qRound(m_size.height() * m_dpr)));
}

bool PresentationShell::ensureCompositeResources() {
    if (m_compositeUbo && m_sampler) {
        return true;
    }

    if (!loadShader(QStringLiteral("fgComposite.vert")).isValid() ||
        !loadShader(QStringLiteral("fgComposite.frag")).isValid()) {
        m_error = QStringLiteral("fgComposite 着色器缺失");
        return false;
    }

    m_compositeUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kFgUniformSize);
    if (!m_compositeUbo->create()) {
        m_error = QStringLiteral("合成 uniform 缓冲创建失败");
        return false;
    }
    m_ownedResources.append(m_compositeUbo);

    m_sampler = m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!m_sampler->create()) {
        m_error = QStringLiteral("合成采样器创建失败");
        return false;
    }
    m_ownedResources.append(m_sampler);

    // 管线只需要一份"形状正确"的 SRB 表达绑定布局；逐帧真正用的 SRB 由
    // bindingsFor() 按输入纹理缓存，布局一致所以共用同一条管线。
    // 1×1 占位纹理：管线必须在"布局已确定"的 SRB 上创建，而布局要求绑定真实资源。
    m_pipelineBase = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1));
    if (!m_pipelineBase->create()) {
        m_error = QStringLiteral("合成占位纹理创建失败");
        return false;
    }
    m_ownedResources.append(m_pipelineBase);

    m_pipelineBindings = m_rhi->newShaderResourceBindings();
    m_pipelineBindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_compositeUbo),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_pipelineBase,
                                                  m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, m_pipelineBase,
                                                  m_sampler),
    });
    if (!m_pipelineBindings->create()) {
        m_error = QStringLiteral("合成模板 SRB 创建失败");
        return false;
    }
    return true;
}

QRhiShaderResourceBindings *PresentationShell::bindingsFor(QRhiTexture *base, QRhiTexture *overlay) {
    const quint64 key = bindingKey(base, overlay);
    if (auto it = m_bindingCache.constFind(key); it != m_bindingCache.constEnd()) {
        return *it;
    }

    auto *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_compositeUbo),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, base,
                                                  m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, overlay,
                                                  m_sampler),
    });
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    m_bindingCache.insert(key, srb);
    return srb;
}

bool PresentationShell::ensureSwapchainResources() {
    return m_presenter && m_presenter->ready();
}

void PresentationShell::releaseSwapchainResources() {
    delete m_pipeline;
    m_pipeline = nullptr;
    for (auto *binding : std::as_const(m_bindingCache)) {
        delete binding;
    }
    m_bindingCache.clear();
}

bool PresentationShell::recreateSwapchain() {
    // 强制重建：OutOfDate 的交换链即使窗口尺寸没变也已经不可用，不能走 resize() 里
    // "尺寸没变就返回"的捷径——以前就是被这条捷径吞掉，acquire 于是永远 OutOfDate。
    const QSize requestedPixels(qMax(1, qRound(m_size.width() * m_dpr)),
                                qMax(1, qRound(m_size.height() * m_dpr)));
    if (!m_presenter->resize(requestedPixels, true)) {
        m_error = QStringLiteral("交换链重建失败：%1").arg(m_presenter->lastError());
        return false;
    }
    // 重建能救活的情况：重建后马上有新帧上屏（presentOne 成功会清零这个计数）。
    // 救不活的情况：连续重建却一帧都上不去——那就是设备根本不能向这个 surface
    // 呈现（典型是没有直连显示输出的独显），继续重建只会白等 GPU 空闲，直接判死。
    if (++m_recreateStreak > 5) {
        m_failed = true;
        m_error =
            QStringLiteral("呈现层连续 %1 次重建都没有新帧上屏，判定这台设备无法呈现").arg(m_recreateStreak);
        return false;
    }
    m_needRecreate = false;
    // 重建后必须把"跨尺寸的旧状态"全部作废，否则第一帧就是垃圾：
    //   · m_pendingRealFrame：排队的那帧是按**旧尺寸**插出来的，贴到新交换链上
    //     就是满屏错色（实测每次 resize 后第一帧亮度从 71 跳到 131/136 再回落，
    //     用户看到的就是"改窗口大小闪一下"）；
    //   · 插值历史：上一对帧属于旧尺寸，跨尺寸插值没有意义，先复位成纯真实帧。
    m_pendingRealFrame = false;
    m_forcePresent = true;
    m_framesSinceRecreate = 0;
    if (m_fg) {
        m_fg->requestReset();
    }
    releaseSwapchainResources();
    return ensureSwapchainResources();
}

void PresentationShell::syncSizes() {
    // 设备像素比会在窗口被拖到另一块屏幕、或系统缩放变化时改变。
    // 它一变，交换链和离屏纹理的目标尺寸都变了，必须重建，否则会出现
    // "画面只占左上角一块"（纹理偏大）或"整体糊一层"（纹理偏小）。
    if (m_host && m_window) {
        const qreal windowDpr = m_window->devicePixelRatio();
        if (!qFuzzyCompare(windowDpr, m_windowDpr)) {
            m_windowDpr = windowDpr;
            m_dpr = windowDpr;
            m_host->setPixelRatio(m_dpr);
            m_needRecreate = true;
            m_forcePresent = true;
            qInfo().noquote() << "FG: 窗口设备像素比" << windowDpr << "离屏层实际比例" << m_dpr << "物理像素"
                              << pixelSize();
        }
    }
    const QSize windowSize = m_window->size();
    if (windowSize.isEmpty()) {
        return;
    }
    if (windowSize != m_size) {
        m_size = windowSize;
        m_host->resize(m_size);
        // 交换链是应用自己持有的，尺寸变了必须显式重建。
        m_needRecreate = true;
    }
    // 每帧都把真实窗口尺寸推给 Engine，不能只在"尺寸变化"时推。
    //
    // 为什么：FG 路径没有 Main.qml 的 Window（它的 onWidthChanged → rhine.resize()），
    // 而 Engine 默认停在 1280x720，HUD 舞台宽度是 width/(height/1080)=1920。而
    // initializeBackend() 在 show() 之后就已经把 m_size 读成真实尺寸（如 1262x1370）
    // 了，于是第一次 step() 时 windowSize == m_size，"尺寸没变"——同步永远不发生，
    // Engine 一直按 1920 的逻辑宽度布局 HUD：导航、歌曲列表、Tab Mode/COLLECTION、
    // 弹窗右列、播放条右半边这些按 width 相对定位的内容全被画到离屏层外面，看起来
    // 就是"UI 少了一整块"；同时 fit() 不再截断，文字也变长。
    // Engine::resize() 对相同尺寸会早退，所以这里每帧调用没有额外开销。
    m_backend->resize(m_size.width(), m_size.height());
}

const RhineFg::FrameInputs &PresentationShell::currentFrameInputs() const {
#ifdef Q_OS_DARWIN
    return m_backend->metalFxInputs;
#else
    return RhineFg::latest();
#endif
}

bool PresentationShell::armFrameGeneration() {
    if (!m_fg) {
        return false;
    }
    const RhineFg::FrameInputs &inputs = currentFrameInputs();
    if (!inputs.depth || !inputs.motionVectors || inputs.renderSize.isEmpty() ||
        inputs.displaySize.isEmpty()) {
        return false;
    }
    // 只有场景真的重渲了才有新的光流输入；相机参数（列表页与详情页 near/far 不同）
    // 每帧都要写，纹理新鲜度用 serial 判断。
    if (inputs.serial == m_lastTapSerial && m_fg->available()) {
        return m_fgArmed;
    }
    m_lastTapSerial = inputs.serial;

    // 注意：resize() 会销毁/重建 GPU 资源并等设备空闲，必须在任何 QRhi 帧之外调用，
    // 不能在 beginOffscreenFrame 之后——那样会卡死在设备等待上。
    // SceneLayerHost 的场景纹理是 RGBA8（Qt 的渲染目标格式），FG 的三张主纹理格式必须一致，
    // 所以这里显式传 RGBA8，而不是用 FgBackend 的默认 RGBA16F。
    // displaySize 必须是**物理像素**：插帧的输出要直接铺满交换链。若沿用逻辑尺寸，
    // 高 DPI 下 FG 输出小于合成目标，画面会被拉伸而发糊。renderSize 保持原值——
    // 那是运动矢量/深度的分辨率，跟着 3D 渲染尺寸走，与设备像素比无关。
    const QSize fgDisplaySize = pixelSize().isEmpty() ? inputs.displaySize : pixelSize();
    if (!m_fg->resize(m_rhi, inputs.renderSize, fgDisplaySize, QRhiTexture::RGBA8)) {
        return false;
    }
    m_fg->setCamera(inputs.nearPlane, inputs.farPlane, inputs.fovYRadians, inputs.frameTimeMs);
    m_fg->setJitter(float(inputs.jitter.x()), float(inputs.jitter.y()));
    if (inputs.reset) {
        m_fg->requestReset();
    }
    m_fgArmed = true;
    RhineFg::publishCapability(true, true, m_fg->available());
    return true;
}

bool PresentationShell::recordFrameGeneration(QRhiCommandBuffer *cb) {
    if (!m_fg || !m_fgArmed) {
        return false;
    }

    QRhiTexture *color = m_host->sceneTexture();
    cb->beginExternal();
    m_probe.restart();
    // prepare 里跑的是光流（ffxOpticalflowContextDispatch）与深度重建，
    // dispatch 里跑的是帧插值本身——两者的成本差一个数量级，分开计才有诊断价值。
    const bool prepared =
        m_fg->prepare(m_rhi, cb, color, currentFrameInputs().depth, currentFrameInputs().motionVectors);
    m_stats.prepareMs = blend(m_stats.prepareMs, tookMs());
    bool dispatched = false;
    if (prepared) {
        // 3D 层不带 UI，所以"含 UI 的颜色"与"无 UI 的颜色"是同一张：UI 由合成通道
        // 在插值之后单独叠加（UI 不该跟着插值动，否则文字会重影）。
        dispatched = m_fg->dispatch(m_rhi, cb, color, color, nullptr);
        m_stats.dispatchMs = blend(m_stats.dispatchMs, tookMs());
    }
    cb->endExternal();

    if (!dispatched) {
        qWarning().noquote() << "FG:" << m_fg->lastError();
        return false;
    }
    // reset 帧的插值结果不可用（SDK 跳过了准备 pass），这一帧直接呈现当前帧。
    return !m_fg->lastFrameReset();
}

double PresentationShell::tookMs() {
    if (!m_probe.isValid()) {
        return 0.0;
    }
    const double ms = double(m_probe.nsecsElapsed()) / 1e6;
    m_probe.restart();
    return ms;
}

double PresentationShell::blend(double previous, double sample) {
    // 指数滑动平均：单帧耗时会被排队与调度拉出几倍抖动，面板要的是趋势不是尖峰。
    // 前 1/0.15 ≈ 7 帧就能跟上真实水平，切换插值开关后不到 0.15 秒就稳定下来。
    constexpr double kAlpha = 0.15;
    if (previous <= 0.0) {
        return sample;
    }
    return previous * (1.0 - kAlpha) + sample * kAlpha;
}

void PresentationShell::recordGap(double gapMs) {
    m_gapRing[size_t(m_gapCursor)] = gapMs;
    m_gapCursor = (m_gapCursor + 1) % kGapCapacity;
    if (m_gapFilled < kGapCapacity) {
        m_gapFilled++;
    }
    // 每次呈现都排序 600 个数没必要，但要够新：面板每 500ms 采一次样，
    // 每 8 帧刷一次（60Hz 下约 130ms）既保证新鲜，成本也可以忽略。
    if (m_presentedFrames % 8 == 0) {
        refreshGapStats();
    }
}

void PresentationShell::refreshGapStats() {
    if (m_gapFilled < 2) {
        return;
    }
    QList<double> sorted;
    sorted.reserve(m_gapFilled);
    for (int index = 0; index < m_gapFilled; ++index) {
        sorted.append(m_gapRing[size_t(index)]);
    }
    std::sort(sorted.begin(), sorted.end());
    const auto pick = [&sorted](double quantile) {
        const int last = sorted.size() - 1;
        const int index = std::clamp(int(std::lround(quantile * last)), 0, last);
        return sorted.at(index);
    };
    m_stats.gapP50Ms = pick(0.50);
    m_stats.gapP95Ms = pick(0.95);
    m_stats.gapP99Ms = pick(0.99);
    m_stats.gapMaxMs = sorted.last();
    const auto over = [&sorted](double limit) {
        return int(
            std::count_if(sorted.cbegin(), sorted.cend(), [limit](double gap) { return gap > limit; }));
    };
    m_stats.over33 = over(33.0);
    m_stats.over50 = over(50.0);
    m_stats.over100 = over(100.0);
    m_stats.sampleCount = int(sorted.size());
}

void PresentationShell::publishStats(bool interpolatedThisStep) {
    const RhineFg::FrameInputs &inputs = currentFrameInputs();
    m_stats.layerActive = m_started && m_window && m_window->isVisible();
    m_stats.interpolationAvailable = m_fgAvailable;
    m_stats.interpolationWanted = requested();
    m_stats.interpolationActive = interpolatedThisStep;
    m_stats.depthReady = inputs.depth != nullptr;
    m_stats.motionVectorsReady = inputs.motionVectors != nullptr;
    m_stats.presented = m_presentedFrames;
    m_stats.interpolated = m_interpolatedFrames;
    m_stats.rendered = m_renderedFrames;
    m_stats.renderSize = inputs.renderSize;
#ifdef Q_OS_DARWIN
    if (m_stats.renderSize.isEmpty()) {
        const auto telemetry = m_backend->renderTelemetry.snapshot();
        m_stats.renderSize = {telemetry.value("renderWidth").toInt(),
                              telemetry.value("renderHeight").toInt()};
    }
#endif
    m_stats.displaySize = m_presenter ? m_presenter->extent() : QSize();
    ++m_stats.serial;
    RhineFg::publish(m_stats);
}

bool PresentationShell::composite(QRhiCommandBuffer *cb, QRhiTexture *base, QRhiTexture *overlay,
                                  quint32 imageIndex, QRhiRenderTarget *overrideTarget) {
    const RhineRender::PresentationBackend::Frame frame{imageIndex};
    QRhiRenderTarget *target = overrideTarget ? overrideTarget : m_presenter->target(frame);
    if (!target) {
        return false;
    }
    QRhiShaderResourceBindings *srb = bindingsFor(base, overlay);
    if (!srb) {
        return false;
    }

    if (!m_pipeline) {
        m_pipeline = m_rhi->newGraphicsPipeline();
        m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader("fgComposite.vert")},
                                     {QRhiShaderStage::Fragment, loadShader("fgComposite.frag")}});
        m_pipeline->setVertexInputLayout({});
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_pipeline->setDepthTest(false);
        m_pipeline->setShaderResourceBindings(m_pipelineBindings);
        m_pipeline->setRenderPassDescriptor(target->renderPassDescriptor());
        if (!m_pipeline->create()) {
            m_error = QStringLiteral("合成管线创建失败");
            return false;
        }
    }

    QMatrix4x4 correction = m_rhi->clipSpaceCorrMatrix();
    float uniforms[kFgUniformSize / 4] = {};
    std::memcpy(uniforms, correction.constData(), 64);
    // 与 common.glsl 的 rtUV() 同口径：帧缓冲原点在上方时纹理坐标要翻转 v。
    uniforms[16] = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;

    QRhiResourceUpdateBatch *batch = m_rhi->nextResourceUpdateBatch();
    batch->updateDynamicBuffer(m_compositeUbo, 0, kFgUniformSize, uniforms);

    const QSize pixelSize = target->pixelSize();
    cb->beginPass(target, Qt::black, {1.0f, 0}, batch);
    cb->setGraphicsPipeline(m_pipeline);
    cb->setShaderResources(srb);
    cb->setViewport({0, 0, float(pixelSize.width()), float(pixelSize.height())});
    cb->draw(3);
    cb->endPass();

    return overrideTarget || m_presenter->finishTarget(cb, frame);
}

bool PresentationShell::presentOne(const RhineRender::PresentationBackend::Frame &frame,
                                   bool allowInterpolation, bool *interpolatedOut) {
    if (interpolatedOut) {
        *interpolatedOut = false;
    }

    QRhiCommandBuffer *cb = nullptr;
    m_probe.restart();
    const QRhi::FrameOpResult begun = m_presenter->beginFrame(frame, &cb);
    m_stats.beginMs = blend(m_stats.beginMs, tookMs());
    if (begun != QRhi::FrameOpSuccess) {
        m_error = QStringLiteral("beginOffscreenFrame 失败（%1）").arg(int(begun));
        return false;
    }

    // FG 的算法命令必须和合成落在同一个离屏帧里：先插值、再合成、最后注入信号量，
    // 三者由同一次提交保证顺序与可见性。
    const bool interpolated = allowInterpolation && recordFrameGeneration(cb);
    QRhiTexture *base = interpolated ? m_fg->output() : m_host->sceneTexture();
    m_probe.restart();
    const bool composed = composite(cb, base, m_host->overlayTexture(), frame.imageIndex);
    m_stats.compositeMs = blend(m_stats.compositeMs, tookMs());
    m_probe.restart();
    const bool presented = m_presenter->endFrame(frame, composed);
    m_stats.endMs = blend(m_stats.endMs, tookMs());
    if (!presented) {
        m_error = m_presenter->lastError();
        return false;
    }
    ++m_presentedFrames;
    if (interpolated) {
        ++m_interpolatedFrames;
    }
    if (interpolatedOut) {
        *interpolatedOut = interpolated;
    }
    // 呈现间隔与分项耗时都按"真的上屏了"来记，失败的那次不计入，免得把
    // 交换链重建的停顿算成 FG 的成本。
    if (presented) {
        m_stats.presentMs = blend(m_stats.presentMs, tookMs());
        if (m_presentClock.isValid()) {
            recordGap(double(m_presentClock.nsecsElapsed()) / 1e6);
        }
        m_presentClock.restart();
        // 真的上屏了，说明重建是有效的：清掉"连续重建次数"，别让偶发的
        // 一两次 OutOfDate 累积成误判。
        m_recreateStreak = 0;
        if (m_framesSinceRecreate < 1000) {
            ++m_framesSinceRecreate;
        }
    }
    return presented;
}

void PresentationShell::step() {
#ifdef Q_OS_IOS
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        return;
    }
#endif
    if (m_stopped || m_quitRequested || m_failed || !m_window || !m_host) {
        return;
    }
    if (!m_frameClock.isValid()) {
        m_frameClock.start();
    }
    // 两步之间的空档：定时器排队 + 事件循环。和渲染成本分开统计，否则"各项都 <1ms、
    // 帧间隔 30ms+"根本看不出钱花在哪。
    if (!m_stepClock.isValid()) {
        m_stepClock.start();
    } else if (m_stepEndNs > 0) {
        m_stats.idleGapMs = blend(m_stats.idleGapMs, double(m_stepClock.nsecsElapsed() - m_stepEndNs) / 1e6);
    }
    if (!m_window->isVisible() || !m_window->isExposed() || m_window->visibility() == QWindow::Minimized) {
        return;
    }
    const bool idle = m_backend->performance().value(QStringLiteral("idle")).toBool();
    const bool interpolationWanted = requested();
    syncSizes();

    if (!m_host->resumeGraphics()) {
        m_error = m_host->lastError();
        m_failed = true;
        return;
    }
    if (m_needRecreate && !recreateSwapchain()) {
        qWarning().noquote() << "FG:" << m_error;
        m_needRecreate = false;
        scheduleNext();
        return;
    }
#ifdef Q_OS_LINUX
    if (m_backendName == "vulkan") {
        // WSI 可以覆盖请求尺寸；离屏目标和 3D 内部纹理必须采用实际 surface 的像素数。
        m_host->resize(m_size, m_presenter->extent());
    }
#endif
    if (!ensureSwapchainResources() || !ensureCompositeResources()) {
        qWarning().noquote() << "FG:" << m_error;
        scheduleNext();
        return;
    }

    // 引擎计时推进不等于画面改变。两层都无更新且没有待呈现帧时让事件循环休眠。
    const auto engineSteps = m_backend->performance().value(QStringLiteral("steps")).toULongLong();
    const bool engineAdvanced = m_backendName == "vulkan" && engineSteps != m_lastEngineSteps;
    if (!m_host->contentDirty() && !engineAdvanced && !m_forcePresent && !m_pendingRealFrame) {
        ++m_skippedSteps;
        publishStats(false);
        if (m_backendName == "vulkan") {
            scheduleNext(idle ? 100 : 4);
        }
        return;
    }
    m_lastEngineSteps = engineSteps;

    // 上一步留下的真实帧还没上屏：这一步只做它的呈现。
    // 插值帧与真实帧因此拆进相邻的两个调度步——一步一次 present，事件循环在两者
    // 之间能拿回执行权；以前两步的活在一步里做完，输入和窗口事件会被整段吞掉。
    if (m_pendingRealFrame) {
        m_pendingRealFrame = false;
        RhineRender::PresentationBackend::Frame frame;
        m_probe.start();
        const auto acquired = m_presenter->acquire(&frame);
        m_stats.acquireMs = blend(m_stats.acquireMs, tookMs());
        if (acquired == RhineRender::PresentationBackend::AcquireResult::OutOfDate) {
            m_needRecreate = true;
            scheduleNext();
            return;
        }
        if (acquired == RhineRender::PresentationBackend::AcquireResult::Ok ||
            acquired == RhineRender::PresentationBackend::AcquireResult::Suboptimal) {
            if (!presentOne(frame, false, nullptr)) {
                qWarning().noquote() << "FG: present 失败" << m_presenter->lastError();
                m_needRecreate = true;
            }
        }
        // Timeout/Error：插值帧已经上屏，少补一帧即可，不必重建整条交换链。
        publishStats(false);
        reportStats();
        scheduleNext(0);
        return;
    }

    if (m_backendName == "vulkan") {
        armFrameGeneration();
    }
    RhineRender::PresentationBackend::Frame frame;
    m_probe.start();
    const auto acquired = m_presenter->acquire(&frame);
    m_stats.acquireMs = blend(m_stats.acquireMs, tookMs());
    // Suboptimal 只表示"这张图能呈现，但交换链不再最优"。以前这里把图一起丢掉并整链重建
    // （deviceWaitIdle + 5 张 image/view/semaphore + 全部纹理与管线），代价是掉几帧、
    // 画面还会闪一下。重建只由 OutOfDate 与窗口 Resize/Expose 驱动。
    if (acquired == RhineRender::PresentationBackend::AcquireResult::OutOfDate) {
        m_needRecreate = true;
        if (++m_outOfDateCount % 100 == 1) {
            qWarning().noquote() << "FG: acquire 返回 OutOfDate，累计" << m_outOfDateCount;
        }
        scheduleNext();
        return;
    }
    if (acquired == RhineRender::PresentationBackend::AcquireResult::Suboptimal) {
        static bool warnedSuboptimal = false;
        if (!warnedSuboptimal) {
            warnedSuboptimal = true;
            qWarning().noquote() << "FG: 交换链 suboptimal，继续使用（等 OutOfDate 或窗口事件再重建）";
        }
    } else if (acquired != RhineRender::PresentationBackend::AcquireResult::Ok) {
        // Timeout：窗口最小化或设备忙，跳过这一帧即可，重建没有意义。
        if (++m_acquireTimeoutCount % 100 == 1) {
            qWarning().noquote() << "FG: acquire 超时（窗口不可见/合成器未取图），累计"
                                 << m_acquireTimeoutCount;
        }
        scheduleNext();
        return;
    }

    m_probe.restart();
    const auto previousSceneFrames = m_host->sceneRenderCount();
    const bool sceneRendered = m_host->renderFrame();
    m_stats.sceneMs = blend(m_stats.sceneMs, tookMs());
    if (!sceneRendered) {
        qWarning().noquote() << "FG: 离屏渲染失败" << m_host->lastError();
        scheduleNext();
        return;
    }
    const bool newScene = m_host->sceneRenderCount() != previousSceneFrames;
    m_renderedFrames +=
        m_host->sceneRenderCount() - previousSceneFrames; // 场景真的重渲了一帧（FG 的输入新鲜度就看它）

    bool interpolated = false;
    // 重建后的前两帧只呈现真实帧：那时 QML 可能还在按旧尺寸排版、交换链也是全新的，
    // 跨尺寸插出来的帧就是满屏错色（改窗口大小时"闪一下"的来源）。
    // 在离屏提交结束后读取这一帧输入；UI 单独更新不能再次插值同一帧场景。
    const bool interpolationAllowed = newScene && !idle && interpolationWanted &&
                                      m_framesSinceRecreate >= 2 &&
                                      (m_backendName == "vulkan" || armFrameGeneration());
    if (!presentOne(frame, interpolationAllowed, &interpolated)) {
        qWarning().noquote() << "FG: present 失败" << m_presenter->lastError();
        m_needRecreate = true;
        scheduleNext();
        return;
    }

    // 插值帧已经上屏，同一帧的真实版本留到下一步再上屏（见上面的 pendingRealFrame）。
    m_pendingRealFrame = interpolated;
    publishStats(interpolated);
    m_forcePresent = false;
    reportStats();
    // 新更新和待呈现的真实帧继续排队；其余情况等待下一次内容更新。
    if (m_backendName == "vulkan") {
        scheduleNext(idle ? 100 : 0);
    } else if (m_pendingRealFrame || m_host->contentDirty()) {
        scheduleNext();
    }
}

void PresentationShell::reportStats() {
    // 第一行要在启动后很快出现：空闲时呈现节奏会降到 10Hz，等"每 300 帧一行"
    // 要半分钟才看得到第一条，冒烟测试和现场排障都等不起。
    const bool firstWindow = m_presentedFrames == 60;
    if (!firstWindow && (m_presentedFrames < 60 || m_presentedFrames % 300 != 0)) {
        return;
    }
    const RhineFg::FrameInputs &inputs = currentFrameInputs();
    qWarning().noquote()
        << "FG stats: presented" << m_presentedFrames << "interpolated" << m_interpolatedFrames << "tapSerial"
        << inputs.serial << "depth" << (inputs.depth ? "ok" : "null") << "mv"
        << (inputs.motionVectors ? "ok" : "null") << "render" << inputs.renderSize << "display"
        << inputs.displaySize << "gapP50" << QString::number(m_stats.gapP50Ms, 'f', 1)
        << "ms/frame"
        // 分项：取图 / 场景 / 合成 / 上屏（上屏在 FIFO 下会阻塞到刷新点，
        // 它就是"为什么每帧都掉到 20ms 档"的直接证据，必须一起打出来）
        << "取图" << QString::number(m_stats.acquireMs, 'f', 2) << "场景"
        << QString::number(m_stats.sceneMs, 'f', 2)
        // 光流+深度重建（prepare）与插值本身（dispatch）必须分开看：
        // 两者的优化手段完全不同，混在一起就不知道该调哪个。
        << "光流" << QString::number(m_stats.prepareMs, 'f', 2) << "插值"
        << QString::number(m_stats.dispatchMs, 'f', 2) << "合成"
        << QString::number(m_stats.compositeMs, 'f', 2) << "上屏"
        << QString::number(m_stats.presentMs, 'f', 2) << "起始" << QString::number(m_stats.beginMs, 'f', 2)
        << "收尾" << QString::number(m_stats.endMs, 'f', 2) << "空档"
        << QString::number(m_stats.idleGapMs, 'f', 2) << "跳过" << m_skippedSteps
        << (m_frameClock.isValid() ? m_frameClock.restart() / double(m_presentedFrames - m_statsBase) : 0.0);
    m_statsBase = m_presentedFrames;
}

void PresentationShell::scheduleNext(int delayMs) {
    // 单次定时器串起来而不是固定间隔：FIFO present 自带节流，让事件循环在两次
    // 呈现之间拿到执行权（输入、窗口事件），比追着刷新率跑更稳。
#ifdef Q_OS_IOS
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        return;
    }
#endif
    if (!m_timer || m_stopped || m_quitRequested || m_failed) {
        return;
    }
    if (m_stepClock.isValid()) {
        m_stepEndNs = m_stepClock.nsecsElapsed(); // 空档计时的起点
    }
    m_timer->setSingleShot(true);
    m_timer->start(delayMs);
}

bool PresentationShell::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window) {
        return QObject::eventFilter(watched, event);
    }

    const bool visible = m_window->isVisible() && m_window->visibility() != QWindow::Minimized
#ifdef Q_OS_IOS
                         && QGuiApplication::applicationState() == Qt::ApplicationActive
#endif
        ;
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
        event->type() == QEvent::WindowStateChange || event->type() == QEvent::Expose) {
        m_backend->setVisible(visible);
        if (visible) {
            if (m_releaseTimer) {
                m_releaseTimer->stop();
            }
            m_forcePresent = true;
            scheduleNext();
        } else {
            m_startupProbe.reset();
            if (m_timer) {
                m_timer->stop();
            }
            if (m_releaseTimer) {
                m_releaseTimer->start(2000);
            }
        }
    }
    switch (event->type()) {
    case QEvent::Close:
        // 消费关闭事件以保留原生窗口；GPU 清理必须等待发起关闭的 QML 回调返回。
        event->ignore();
        requestQuit();
        return true;
    case QEvent::Expose:
        if (m_window->isExposed()) {
            m_needRecreate = true;
            m_forcePresent = true;
            scheduleNext();
        }
        break;
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
    case QEvent::Resize:
        m_needRecreate = true;
        m_forcePresent = true;
        scheduleNext();
        break;
    default:
        break;
    }

    // 探针（RHINE_KEY_PROBE=1）：打印真正到达可见窗口的按键。
    // 被 Qt 快捷键地图（QShortcutMap）消费掉的按键不会出现在这里——用它区分
    // "快捷键没匹配上"与"快捷键已触发、但动作没生效"。
    if (qEnvironmentVariableIntValue("RHINE_KEY_PROBE") == 1 &&
        (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto *key = static_cast<QKeyEvent *>(event);
        qInfo().noquote() << "按键探针:"
                          << (event->type() == QEvent::KeyPress ? "KeyPress" : "ShortcutOverride")
                          << QKeySequence(key->keyCombination()).toString()
                          << "修饰键=" << int(key->modifiers());
    }

    // QQuickRenderControl 没有自己的事件来源：可见窗口收到的输入必须原样转发给
    // 离屏的 UI 窗口，否则按钮、滚轮、快捷键全部失效。
    if (QQuickWindow *overlay = m_host ? m_host->overlayWindow() : nullptr) {
        switch (event->type()) {
        case QEvent::KeyPress:
            // 兜底且是真正的保障：按键到达时离屏窗口若没有焦点项，KeyPress 会被
            // QQuickWindow 直接丢弃（F 键不受影响是因为 Shortcut 走 QShortcutMap，
            // 与焦点项无关）。事件循环的顺序不可控，所以这里同步补一次，绝不依赖
            // FocusIn 的排队恢复。
            ensureOverlayFocus(overlay);
            QCoreApplication::sendEvent(overlay, event);
            break;
        case QEvent::KeyRelease:
            QCoreApplication::sendEvent(overlay, event);
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::HoverEnter:
        case QEvent::HoverLeave:
        case QEvent::HoverMove:
            // 可见窗口和离屏窗口都使用逻辑坐标；像素密度只作用于渲染目标。
            QCoreApplication::sendEvent(overlay, event);
            break;
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            QCoreApplication::sendEvent(overlay, event);
            break;
        case QEvent::FocusOut:
        case QEvent::FocusIn:
            // 故意不转发 FocusIn/FocusOut。
            //
            // 为什么：离屏窗口没有平台窗口，isActive() 恒为 false，靠 Qt 自己的
            // 激活流程永远拿不回焦点。而 QQuickWindow::focusOutEvent 会立刻把
            // activeFocusItem 清空——转发出去就等于亲手把 QML 焦点打掉，且再也
            // 恢复不了（实测切走再切回后 activeFocusItem 恒为 null，于是 KeyPress
            // 到了窗口却被 QQuickWindow 丢弃：ESC/方向键全部失效，而 F 键因为是
            // Shortcut、不经过焦点项，所以照常好用）。
            // 焦点由"可见窗口"这唯一真实来源决定：启动时建立一次，按键到达时兜底。
            break;
        case QEvent::ShortcutOverride:
            QCoreApplication::sendEvent(overlay, event);
            break;
        default:
            break;
        }
    }
    if (m_host && (event->isInputEvent() || event->type() == QEvent::InputMethod)) {
        m_host->markOverlayDirty();
        scheduleNext();
    }
    return QObject::eventFilter(watched, event);
}

// 同步版：立刻把 QML 焦点补回来（排队恢复会被后续事件搅掉，实测重新聚焦后仍为 null）。
void PresentationShell::ensureOverlayFocus(QQuickWindow *overlay) {
    if (!overlay || !m_host) {
        return;
    }
    auto *focusItem = overlay->activeFocusItem();
    if (focusItem && focusItem != overlay->contentItem() && focusItem != m_host->overlayRootItem()) {
        return;
    }
    if (QQuickItem *root = m_host->overlayRootItem()) {
        QMetaObject::invokeMethod(root, "restoreInputFocus", Qt::DirectConnection);
    }
}

bool PresentationShell::start() {
    if (!supportsBackend(m_backendName)) {
        m_error = QStringLiteral("这个构建未编入所选呈现后端");
        return false;
    }
    m_size = QSize(1280, 720);
    if (!initializeBackend()) {
        return false;
    }
    if (!initializeLayers()) {
        return false;
    }

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &PresentationShell::step);
    connect(m_backend, &Engine::sceneUpdated, this, [this] {
        m_host->markSceneDirty();
        scheduleNext();
    });
    connect(m_backend, &Engine::hudUpdated, this, [this] {
        m_host->markOverlayDirty();
        scheduleNext();
    });

#ifdef Q_OS_DARWIN
    m_releaseTimer = new QTimer(this);
    m_releaseTimer->setSingleShot(true);
    const auto releaseBackgroundGraphics = [this] {
        if (m_rhi && (!m_window->isVisible() || m_window->visibility() == QWindow::Minimized
#ifdef Q_OS_IOS
                      || QGuiApplication::applicationState() != Qt::ApplicationActive
#endif
                      )) {
            if (m_backend) {
                m_backend->metalFxInputs = {};
            }
            m_rhi->finish();
            if (m_fg) {
                m_fg->release();
            }
            releaseSwapchainResources();
            if (m_host) {
                m_host->releaseGraphics();
            }
            if (m_backend) {
                m_backend->trimMemory();
            }
        }
    };
    connect(m_releaseTimer, &QTimer::timeout, this, releaseBackgroundGraphics);
#endif
#ifdef Q_OS_IOS
    // iOS 切后台时窗口可以仍是 visible；必须按应用状态停掉 GPU 提交并释放离屏资源。
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this, releaseBackgroundGraphics](Qt::ApplicationState state) {
                const bool active = state == Qt::ApplicationActive;
                if (m_backend) {
                    m_backend->setVisible(active);
                }
                m_pendingRealFrame = false;
                if (active) {
                    m_releaseTimer->stop();
                    m_needRecreate = true;
                    m_forcePresent = true;
                    scheduleNext();
                } else {
                    // 后台可能立即冻结定时器；不能等下一次自检回调才暂停计时。
                    m_startupProbe.reset();
                    m_timer->stop();
                    // 同步释放，不能依赖进入后台后可能不再执行的定时器。
                    releaseBackgroundGraphics();
                }
            });
#endif
    // 预检：有些设备能建出交换链，却撑不住持续上屏（典型是"没有直连显示输出"的独显
    // ——surface 由跑在集显上的合成器管理）。实测独显上会先成功呈现 4 帧，随后
    // OutOfDate 一路涨到几万次、再也没出过一帧（白窗 + 空转）。
    // 判据要贴着"真坏了"写，不能贴"慢了"：首帧编译着色器本来就是慢的。
    //   · OutOfDate 超过 200 次 → 交换链撑不住，判死；
    //   · 3 秒过去一帧都没上屏 → 判死；
    //   · 按适配器要求检查连续呈现；预检结束后恢复按需调度。
    // 判死后由 main.cpp 干净回退到默认 QQuickWindow 路径。这比
    // vkGetPhysicalDeviceSurfaceSupportKHR 靠谱：后者对这类设备仍返回支持。
#ifndef Q_OS_IOS
    {
        QElapsedTimer probeClock;
        probeClock.start();
        const auto probeFrames = quint64(qMax(1, m_presenter->startupProbeFrames()));
        while (probeClock.elapsed() < 3000 && m_presentedFrames < probeFrames && !m_failed &&
               m_outOfDateCount <= 200) {
            m_forcePresent = true;
            step();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        if (m_failed || m_outOfDateCount > 200 || m_presentedFrames == 0) {
            m_error = QStringLiteral("呈现层自检失败：接管窗口后只上屏 %1 帧"
                                     "（acquire 超时 %2 次、OutOfDate %3 次、重建 %4 次），"
                                     "这台设备撑不住持续呈现，回退默认呈现路径。")
                          .arg(m_presentedFrames)
                          .arg(m_acquireTimeoutCount)
                          .arg(m_outOfDateCount)
                          .arg(m_recreateStreak);
            return false;
        }
    }
#endif

    // 排障：RHINE_FG_DIAG=1 时每秒打一行真实计数。presented 只在每 300 帧打印，
    // 现场判断"到底是在跑还是在空转"时等不起，这个小计时器直接给答案。
    if (qEnvironmentVariableIsSet("RHINE_FG_DIAG")) {
        auto *diag = new QTimer(this);
        diag->setInterval(1000);
        connect(diag, &QTimer::timeout, this, [this] {
            qWarning().noquote() << "FG diag: presented" << m_presentedFrames << "rendered"
                                 << m_renderedFrames << "skipped" << m_skippedSteps << "acqTimeout"
                                 << m_acquireTimeoutCount << "outOfDate" << m_outOfDateCount << "visible"
                                 << (m_window && m_window->isVisible()) << "pendingReal" << m_pendingRealFrame
                                 << "gapP50" << QString::number(m_stats.gapP50Ms, 'f', 1) << "dirty"
                                 << (m_host ? m_host->contentDirty() : false) << "force" << m_forcePresent;
        });
        diag->start();
    }

    if (m_backend) {
        m_backend->setWindow(m_window);
        m_window->installEventFilter(m_backend);
    }
    m_started = true;
    publishStats(false);
    // 交换链与设备信息整个生命周期不变，这里写一次就够，之后 publishStats 带出去。
    m_stats.swapchainImages = int(m_presenter->imageCount());
    m_stats.swapchainFormat = m_presenter->formatName();
    m_stats.presentMode = m_presenter->presentModeName();
    m_stats.adapterName = QString::fromUtf8(m_rhi->driverInfo().deviceName);
    m_stats.backendName = m_presenter->backendName();
    RhineFg::publishCapability(compiledIn(), true, m_fgAvailable,
                               m_backendName == "metal" && !m_fgAvailable
                                   ? QStringLiteral("当前构建、系统或设备不支持 MetalFX 帧生成")
                                   : QString());
    qWarning().noquote() << "FG: 呈现层已启动；插值" << (m_fgAvailable ? "可用" : "不可用") << "，"
                         << m_presenter->imageCount() << "张交换链图像" << m_presenter->extent()
                         << "，呈现模式" << m_presenter->presentModeName();
    return true;
}

int PresentationShell::run() {
    if (!m_started) {
        return 1;
    }
#ifdef Q_OS_IOS
    // UIKit 必须先从 Qt 的主事件循环取得执行权，才能完成启动、激活和窗口暴露。
    // processEvents 的同步轮询无法替代这一过程，不能在 start() 中等待首帧。
    auto *probeTimer = new QTimer(this);
    probeTimer->setInterval(16);
    QElapsedTimer clock;
    clock.start();
    connect(probeTimer, &QTimer::timeout, this, [this, probeTimer, clock] {
        if (m_stopped) {
            probeTimer->stop();
            return;
        }
        const bool ready = QGuiApplication::applicationState() == Qt::ApplicationActive &&
                           m_window->isVisible() && m_window->isExposed() &&
                           m_window->visibility() != QWindow::Minimized;
        const auto result = m_startupProbe.update(clock.elapsed(), ready, m_presentedFrames,
                                                  m_failed || m_outOfDateCount > 200);
        using Result = RhineRender::PresentationStartupProbe::Result;
        if (result == Result::Passed || result == Result::Failed) {
            probeTimer->stop();
            probeTimer->deleteLater();
            if (result == Result::Failed) {
                m_failed = true;
                m_error = QStringLiteral("呈现层自检失败：前台窗口就绪后只上屏 %1 帧"
                                         "（acquire 超时 %2 次、OutOfDate %3 次、重建 %4 次）")
                              .arg(m_presentedFrames)
                              .arg(m_acquireTimeoutCount)
                              .arg(m_outOfDateCount)
                              .arg(m_recreateStreak);
                emit startupFailed();
            } else {
                qInfo() << "FG: iOS 呈现层首帧自检通过";
            }
            return;
        }
        if (result == Result::Running) {
            m_forcePresent = true;
            scheduleNext();
        }
    });
    probeTimer->start();
#endif
    scheduleNext();
    return 0;
}

QQuickWindow *PresentationShell::inputWindow() const {
    return m_host ? m_host->overlayWindow() : nullptr;
}

QQuickWindow *PresentationShell::sceneWindow() const {
    return m_host ? m_host->sceneWindow() : nullptr;
}

QVariantMap PresentationShell::renderState() const {
    return {{"sceneFrames", qulonglong(m_host ? m_host->sceneRenderCount() : 0)},
            {"overlayFrames", qulonglong(m_host ? m_host->overlayRenderCount() : 0)},
            {"presented", qulonglong(m_presentedFrames)},
            {"interpolated", qulonglong(m_interpolatedFrames)},
            {"timerActive", m_timer && m_timer->isActive()},
            {"graphicsDevice", m_rhi != nullptr},
#ifdef Q_OS_DARWIN
            {"renderer", m_backend ? m_backend->renderTelemetry.snapshot() : QVariantMap{}},
#endif
            {"windowDpr", m_window ? m_window->devicePixelRatio() : 0.0},
            {"sceneDpr", sceneWindow() ? sceneWindow()->effectiveDevicePixelRatio() : 0.0},
            {"overlayDpr", inputWindow() ? inputWindow()->effectiveDevicePixelRatio() : 0.0},
            {"logicalWidth", m_size.width()},
            {"logicalHeight", m_size.height()},
            {"targetWidth", m_host ? m_host->pixelSize().width() : 0},
            {"targetHeight", m_host ? m_host->pixelSize().height() : 0},
            {"swapchainWidth", m_presenter ? m_presenter->extent().width() : 0},
            {"swapchainHeight", m_presenter ? m_presenter->extent().height() : 0},
            {"backend", m_backendName}};
}

/** 从现有两层纹理抓取同一合成结果，不推进动画或重画场景。 */
QImage PresentationShell::grabFrame(bool interpolated) {
    if (!m_rhi || !m_host || !m_host->sceneTexture() || !m_host->overlayTexture()) {
        return {};
    }
    std::unique_ptr<QRhiTexture> texture(m_rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize(), 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        return {};
    }
    std::unique_ptr<QRhiTextureRenderTarget> target(
        m_rhi->newTextureRenderTarget({QRhiColorAttachment(texture.get())}));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    if (!target->create()) {
        return {};
    }
    QRhiCommandBuffer *commands = nullptr;
    if (m_rhi->beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess) {
        return {};
    }
    // 读回目标为 RGBA8，交换链可能是 BGRA8，不能复用两者的 render pass 管线。
    releaseSwapchainResources();
    auto base = interpolated && m_fg && !m_fg->lastFrameReset() ? m_fg->output() : m_host->sceneTexture();
    const bool composed = composite(commands, base, m_host->overlayTexture(), 0, target.get());
    QRhiReadbackResult result;
    auto updates = m_rhi->nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(texture.get()), &result);
    commands->resourceUpdate(updates);
    m_rhi->endOffscreenFrame();
    m_rhi->finish();
    releaseSwapchainResources();
    if (!composed || result.data.isEmpty()) {
        return {};
    }
    QImage image(reinterpret_cast<const uchar *>(result.data.constData()), result.pixelSize.width(),
                 result.pixelSize.height(), QImage::Format_RGBA8888);
    image = image.copy();
    return m_rhi->isYUpInFramebuffer() ? image.flipped(Qt::Vertical) : image;
}
