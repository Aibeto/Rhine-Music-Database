#include "FgShell.h"

#include "FgBackend.h"
#include "FgCapability.h"
#include "FgConfig.h"
#include "FgFrameTap.h"
#include "FgHost.h"

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
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QWindow>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// 合成通道的 uniform：clipSpaceCorrMatrix（64）+ params vec4（16）。
constexpr int kFgUniformSize = 64 + 16;

QShader loadShader(const QString &name)
{
    QFile f(":/shaders/" + name + ".qsb");
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "FG: 缺少着色器" << name;
        return {};
    }
    return QShader::fromSerialized(f.readAll());
}

quint64 bindingKey(const QRhiTexture *a, const QRhiTexture *b)
{
    return (quint64(quintptr(a)) * 0x9E3779B97F4A7C15ull) ^ quint64(quintptr(b));
}

}  // namespace

FgShell::FgShell(QQmlEngine &engine, Engine &backend, QObject *parent)
    : QObject(parent), m_engine(&engine), m_backend(&backend)
{
}

void FgShell::shutdown()
{
    if (m_timer)
        m_timer->stop();
    releaseSwapchainResources();
    if (m_presenter)
        m_presenter->release();
    RhineFg::setEnabled(false);
}

FgShell::~FgShell()
{
    shutdown();
    if (m_rhi) {
        // 管线与 SRB 都建在 QRhi 上，必须先于 QRhi 销毁。
        delete m_pipeline;
        m_pipeline = nullptr;
        delete m_pipelineBindings;
        m_pipelineBindings = nullptr;
        for (auto *srb : std::as_const(m_bindingCache))
            delete srb;
        m_bindingCache.clear();
        for (auto *resource : std::as_const(m_ownedResources))
            delete resource;
        m_ownedResources.clear();
        m_compositeUbo = nullptr;
        m_sampler = nullptr;
        m_pipelineBase = nullptr;
    }
    // 退出路径实测崩溃在 ffxFrameInterpolationContextDestroy → DestroyBackendContextVK
    // 内的 free/munmap。先把 GPU 队列排空，避免 FFX 在还有队列引用时释放描述符池与
    // uniform 内存（驱动侧会踩到已失效的池）。排空本身无害，只在退出时多等一帧。
    if (m_rhi)
        m_rhi->finish();
    m_fg.reset();
    m_host.reset();
    m_presenter.reset();
    // QRhi（及其背后的 VkDevice/VkInstance）必须先于 QWindow 销毁：QWindow 一拆，
    // 平台层的 wl_display/wl_surface 就没了，而 Wayland 客户端线程仍在读事件队列，
    // 实测会在 wl_display_read_events 里 abort()。顺序错了就是退出即崩。
    m_rhi.reset();
    m_window.reset();
    m_instance.reset();
    // 呈现层没了，面板不该继续显示上一轮的旧数字。
    RhineFg::clear();
}

bool FgShell::compiledIn()
{
#if defined(RHINE_FSR3_VULKAN) && RHINE_FSR3_VULKAN
    return FgBackend::compiledIn();
#else
    return false;
#endif
}

bool FgShell::requested()
{
    return FgConfig::fromEnvironment().enabled;
}

QWindow *FgShell::window() const
{
    return m_window.get();
}

QString FgShell::resolveQmlUrl(const QString &file) const
{
    // qt_add_qml_module 的默认资源前缀是 /qt/qml；也兼容直接以 URI 命名的老布局。
    const QStringList candidates{
        QStringLiteral("qrc:/qt/qml/RhineMusic/") + file,
        QStringLiteral("qrc:/RhineMusic/") + file,
        QStringLiteral("qrc:/") + file,
    };
    for (const auto &candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return candidates.first();
}

bool FgShell::initializeVulkan()
{
    m_instance = std::make_unique<QVulkanInstance>();
    // 交换链与 present 只要 1.1 就够；调高只会让旧驱动在设备创建时更容易失败。
    m_instance->setApiVersion(QVersionNumber(1, 1));
    if (!m_instance->create()) {
        m_error = QStringLiteral("QVulkanInstance 创建失败（errorCode=%1）")
                      .arg(int(m_instance->errorCode()));
        return false;
    }

    m_window = std::make_unique<QWindow>();
    // 必须声明成 Vulkan surface：平台窗口默认按 raster/OpenGL 建，
    // 那样 surfaceForWindow() 永远返回 VK_NULL_HANDLE。
    m_window->setSurfaceType(QSurface::VulkanSurface);
    m_window->setVulkanInstance(m_instance.get());
    m_window->setTitle(QStringLiteral("Rhine Music Database"));
    m_window->resize(m_size.isEmpty() ? QSize(1280, 720) : m_size);
    m_window->create();
    if (!m_window->handle()) {
        m_error = QStringLiteral("QWindow 没有创建出平台窗口");
        return false;
    }
    m_window->installEventFilter(this);

    QRhiVulkanInitParams params;
    params.inst = m_instance.get();
    // 自建交换链需要设备支持该扩展；QRhi 默认不请求它。
    params.deviceExtensions = {QByteArrayLiteral("VK_KHR_swapchain")};
    m_rhi.reset(QRhi::create(QRhi::Vulkan, &params, {}));
    if (!m_rhi) {
        m_error = QStringLiteral("QRhi(Vulkan) 创建失败");
        return false;
    }

    m_device = static_cast<const QRhiVulkanNativeHandles *>(m_rhi->nativeHandles())->dev;
    if (m_device == VK_NULL_HANDLE) {
        m_error = QStringLiteral("拿不到 QRhi 的 VkDevice");
        return false;
    }

    m_window->show();
    // Wayland 下 surface 要等窗口真正 expose 之后才存在，只 create() 会拿到空句柄。
    QElapsedTimer exposeClock;
    exposeClock.start();
    while (!m_window->isExposed() && exposeClock.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(m_window.get());
    if (surface == VK_NULL_HANDLE) {
        m_error = QStringLiteral("拿不到 QWindow 的 VkSurfaceKHR");
        return false;
    }

    m_size = m_window->size();
    const auto config = FgConfig::fromEnvironment();
    m_presenter = std::make_unique<VulkanPresenter>();
    if (!m_presenter->init(m_rhi.get(), surface, m_size, config.vsync)) {
        m_error = QStringLiteral("交换链创建失败：%1").arg(m_presenter->lastError());
        return false;
    }
    return true;
}

bool FgShell::initializeLayers()
{
    // Overlay.qml 的窗口行为（拖动/最大化/关闭/全屏）原来由 Main.qml 的 Window 提供；
    // FG 路径没有那个 Window，改为把真实的 QWindow 注入上下文。
    m_engine->rootContext()->setContextProperty(QStringLiteral("rhineHostWindow"),
                                               QVariant::fromValue(m_window.get()));

    m_host = std::make_unique<FgHost>();
    if (!m_host->start(*m_engine, QUrl(resolveQmlUrl(QStringLiteral("Scene3D.qml"))),
                       QUrl(resolveQmlUrl(QStringLiteral("Overlay.qml"))), m_rhi.get(),
                       m_instance.get())) {
        m_error = QStringLiteral("离屏场景建立失败：%1").arg(m_host->lastError());
        return false;
    }
    m_host->resize(m_size);
    // 键盘链路的第一环：焦点。
    //
    // 默认路径靠 Main.qml 的 Component.onCompleted → overlay.focusInputLayer() 把焦点
    // 交给 Overlay 里的 inputLayer；FG 路径不加载 Main.qml，没人做这件事。而
    // QQuickRenderControl 的离屏窗口没有平台窗口、永远不会被激活，Qt 也不会像真实
    // 窗口那样在激活时自动传播 activeFocus——实测此时 window.activeFocusItem() 为
    // null，QQuickWindow 收到 KeyPress 后无处投递，按键被直接丢弃。
    // 这里显式调一次 Overlay.qml 导出的 focusInputLayer()（内部是 forceActiveFocus，
    // 对未激活的离屏窗口同样有效），与默认路径对齐。
    if (QQuickItem *overlayRoot = m_host->overlayRootItem()) {
        const bool focused =
            QMetaObject::invokeMethod(overlayRoot, "focusInputLayer", Qt::DirectConnection);
        QQuickItem *focusItem = m_host->overlayWindow()->activeFocusItem();
        qInfo().noquote() << "FG: 覆盖层输入焦点"
                          << (focused && focusItem ? "已建立" : "未建立") << "activeFocusItem:"
                          << (focusItem ? focusItem->metaObject()->className() : "null");
    }
    // 只在这里打开每帧输入通道：RhiScene 的渲染器只在 tap 打开时才发布。
    RhineFg::setEnabled(true);
    m_fgAvailable = FgBackend::supported(m_rhi.get());
    if (m_fgAvailable)
        m_fg = std::make_unique<FgBackend>();
    return true;
}

bool FgShell::ensureCompositeResources()
{
    if (m_compositeUbo && m_sampler)
        return true;

    if (!loadShader(QStringLiteral("fgComposite.vert")).isValid()
        || !loadShader(QStringLiteral("fgComposite.frag")).isValid()) {
        m_error = QStringLiteral("fgComposite 着色器缺失");
        return false;
    }

    m_compositeUbo = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                      kFgUniformSize);
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
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  m_pipelineBase, m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  m_pipelineBase, m_sampler),
    });
    if (!m_pipelineBindings->create()) {
        m_error = QStringLiteral("合成模板 SRB 创建失败");
        return false;
    }
    return true;
}

QRhiShaderResourceBindings *FgShell::bindingsFor(QRhiTexture *base, QRhiTexture *overlay)
{
    const quint64 key = bindingKey(base, overlay);
    if (auto it = m_bindingCache.constFind(key); it != m_bindingCache.constEnd())
        return *it;

    auto *srb = m_rhi->newShaderResourceBindings();
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_compositeUbo),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, base,
                                                  m_sampler),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  overlay, m_sampler),
    });
    if (!srb->create()) {
        delete srb;
        return nullptr;
    }
    m_bindingCache.insert(key, srb);
    return srb;
}

bool FgShell::ensureSwapchainResources()
{
    if (!m_presenter || !m_presenter->ready())
        return false;
    const quint32 count = m_presenter->imageCount();
    const QSize extent = m_presenter->extent();
    if (count == 0 || extent.isEmpty())
        return false;
    if (!m_swapTextures.empty() && m_swapSize == extent
        && qint32(m_swapTextures.size()) == qint32(count)) {
        return true;
    }
    releaseSwapchainResources();
    m_swapSize = extent;

    for (quint32 i = 0; i < count; ++i) {
        const VkImage image = m_presenter->imageAt(i);
        if (image == VK_NULL_HANDLE) {
            m_error = QStringLiteral("交换链第 %1 张图像为空").arg(i);
            return false;
        }
        // 包装格式必须与交换链图像的真实通道序一致。QRhi 里 RGBA8 与 BGRA8 是两个
        // 不同的格式：按 RGBA8 去解释一张 B8G8R8A8 图像，着色器写进第 0 字节的 R
        // 会被合成器当成 B 读出来——整屏 R/B 互换（暖色场景整体发蓝，AA 边缘出现
        // 彩色光点）。presenter 只会选 B8G8R8A8_UNORM(44) 或 R8G8B8A8_UNORM(37)。
        const int swapFormat = int(m_presenter->colorFormat());
        const QRhiTexture::Format textureFormat =
            (swapFormat == 44 || swapFormat == 50) ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8;
        auto texture = std::unique_ptr<QRhiTexture>(m_rhi->newTexture(textureFormat, extent));
        texture->setFlags(QRhiTexture::RenderTarget);
        // 布局不在这里固定：每帧合成前用 setNativeLayout(UNDEFINED) 声明"内容作废"，
        // 因此不需要跟踪 present 之后的真实布局。
        if (!texture->createFrom(
                {quint64(quintptr(image)), int(VK_IMAGE_LAYOUT_UNDEFINED)})) {
            m_error = QStringLiteral("包装交换链图像失败（第 %1 张）").arg(i);
            return false;
        }
        auto target = std::unique_ptr<QRhiTextureRenderTarget>(
            m_rhi->newTextureRenderTarget({QRhiColorAttachment(texture.get())}));
        auto pass =
            std::unique_ptr<QRhiRenderPassDescriptor>(target->newCompatibleRenderPassDescriptor());
        if (!pass) {
            m_error = QStringLiteral("交换链渲染通道描述符创建失败");
            return false;
        }
        target->setRenderPassDescriptor(pass.get());
        if (!target->create()) {
            m_error = QStringLiteral("交换链渲染目标创建失败");
            return false;
        }
        m_swapTextures.push_back(std::move(texture));
        m_swapPasses.push_back(std::move(pass));
        m_swapTargets.push_back(std::move(target));
    }
    return true;
}

void FgShell::releaseSwapchainResources()
{
    m_swapTargets.clear();
    m_swapPasses.clear();
    m_swapTextures.clear();
    // 管线绑定了旧渲染通道描述符，交换链一换就必须重建。
    delete m_pipeline;
    m_pipeline = nullptr;
    for (auto *srb : std::as_const(m_bindingCache))
        delete srb;
    m_bindingCache.clear();
}

bool FgShell::recreateSwapchain()
{
    // 强制重建：OutOfDate 的交换链即使窗口尺寸没变也已经不可用，不能走 resize() 里
    // "尺寸没变就返回"的捷径——以前就是被这条捷径吞掉，acquire 于是永远 OutOfDate。
    if (!m_presenter->resize(m_size, true)) {
        m_error = QStringLiteral("交换链重建失败：%1").arg(m_presenter->lastError());
        return false;
    }
    // 重建能救活的情况：重建后马上有新帧上屏（presentOne 成功会清零这个计数）。
    // 救不活的情况：连续重建却一帧都上不去——那就是设备根本不能向这个 surface
    // 呈现（典型是没有直连显示输出的独显），继续重建只会白等 GPU 空闲，直接判死。
    if (++m_recreateStreak > 5) {
        m_failed = true;
        m_error = QStringLiteral("呈现层连续 %1 次重建都没有新帧上屏，判定这台设备无法呈现")
                      .arg(m_recreateStreak);
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
    if (m_fg)
        m_fg->requestReset();
    releaseSwapchainResources();
    return ensureSwapchainResources();
}

void FgShell::syncSizes()
{
    const QSize windowSize = m_window->size();
    if (windowSize.isEmpty())
        return;
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
    // initializeVulkan() 在 show() 之后就已经把 m_size 读成真实尺寸（如 1262x1370）
    // 了，于是第一次 step() 时 windowSize == m_size，"尺寸没变"——同步永远不发生，
    // Engine 一直按 1920 的逻辑宽度布局 HUD：导航、歌曲列表、Tab Mode/COLLECTION、
    // 弹窗右列、播放条右半边这些按 width 相对定位的内容全被画到离屏层外面，看起来
    // 就是"UI 少了一整块"；同时 fit() 不再截断，文字也变长。
    // Engine::resize() 对相同尺寸会早退，所以这里每帧调用没有额外开销。
    m_backend->resize(m_size.width(), m_size.height());
    m_backend->setVisible(m_window->isVisible() && m_window->visibility() != QWindow::Minimized);
}

bool FgShell::armFrameGeneration()
{
    if (!m_fg)
        return false;
    const RhineFg::FrameInputs &inputs = RhineFg::latest();
    if (!inputs.depth || !inputs.motionVectors || inputs.renderSize.isEmpty()
        || inputs.displaySize.isEmpty()) {
        return false;
    }
    // 只有场景真的重渲了才有新的光流输入；相机参数（列表页与详情页 near/far 不同）
    // 每帧都要写，纹理新鲜度用 serial 判断。
    if (inputs.serial == m_lastTapSerial && m_fg->available())
        return m_fgArmed;
    m_lastTapSerial = inputs.serial;

    // 注意：resize() 会销毁/重建 GPU 资源并等设备空闲，必须在任何 QRhi 帧之外调用，
    // 不能在 beginOffscreenFrame 之后——那样会卡死在设备等待上。
    // FgHost 的场景纹理是 RGBA8（Qt 的渲染目标格式），FG 的三张主纹理格式必须一致，
    // 所以这里显式传 RGBA8，而不是用 FgBackend 的默认 RGBA16F。
    if (!m_fg->resize(m_rhi.get(), inputs.renderSize, inputs.displaySize, QRhiTexture::RGBA8))
        return false;
    m_fg->setCamera(inputs.nearPlane, inputs.farPlane, inputs.fovYRadians, inputs.frameTimeMs);
    m_fg->setJitter(float(inputs.jitter.x()), float(inputs.jitter.y()));
    if (inputs.reset)
        m_fg->requestReset();
    m_fgArmed = true;
    RhineFg::publishActive(m_fg->available());
    return true;
}

bool FgShell::recordFrameGeneration(QRhiCommandBuffer *cb)
{
    if (!m_fg || !m_fgArmed)
        return false;

    QRhiTexture *color = m_host->sceneTexture();
    cb->beginExternal();
    m_probe.restart();
    // prepare 里跑的是光流（ffxOpticalflowContextDispatch）与深度重建，
    // dispatch 里跑的是帧插值本身——两者的成本差一个数量级，分开计才有诊断价值。
    const bool prepared = m_fg->prepare(m_rhi.get(), cb, color, RhineFg::latest().depth,
                                       RhineFg::latest().motionVectors);
    m_stats.prepareMs = blend(m_stats.prepareMs, tookMs());
    bool dispatched = false;
    if (prepared) {
        // 3D 层不带 UI，所以"含 UI 的颜色"与"无 UI 的颜色"是同一张：UI 由合成通道
        // 在插值之后单独叠加（UI 不该跟着插值动，否则文字会重影）。
        dispatched = m_fg->dispatch(m_rhi.get(), cb, color, color, nullptr);
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

double FgShell::tookMs()
{
    if (!m_probe.isValid())
        return 0.0;
    const double ms = double(m_probe.nsecsElapsed()) / 1e6;
    m_probe.restart();
    return ms;
}

double FgShell::blend(double previous, double sample)
{
    // 指数滑动平均：单帧耗时会被排队与调度拉出几倍抖动，面板要的是趋势不是尖峰。
    // 前 1/0.15 ≈ 7 帧就能跟上真实水平，切换插值开关后不到 0.15 秒就稳定下来。
    constexpr double kAlpha = 0.15;
    if (previous <= 0.0)
        return sample;
    return previous * (1.0 - kAlpha) + sample * kAlpha;
}

void FgShell::recordGap(double gapMs)
{
    m_gapRing[size_t(m_gapCursor)] = gapMs;
    m_gapCursor = (m_gapCursor + 1) % kGapCapacity;
    if (m_gapFilled < kGapCapacity)
        m_gapFilled++;
    // 每次呈现都排序 600 个数没必要，但要够新：面板每 500ms 采一次样，
    // 每 8 帧刷一次（60Hz 下约 130ms）既保证新鲜，成本也可以忽略。
    if (m_presentedFrames % 8 == 0)
        refreshGapStats();
}

void FgShell::refreshGapStats()
{
    if (m_gapFilled < 2)
        return;
    QList<double> sorted;
    sorted.reserve(m_gapFilled);
    for (int index = 0; index < m_gapFilled; ++index)
        sorted.append(m_gapRing[size_t(index)]);
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
        return int(std::count_if(sorted.cbegin(), sorted.cend(),
                                 [limit](double gap) { return gap > limit; }));
    };
    m_stats.over33 = over(33.0);
    m_stats.over50 = over(50.0);
    m_stats.over100 = over(100.0);
    m_stats.sampleCount = int(sorted.size());
}

QString FgShell::swapchainFormatName(int format)
{
    // 直接写数值而不引用 VK_FORMAT_* 枚举：这里只是给面板看的可读名，
    // 而枚举值的稳定性由 Vulkan 规范保证。未知格式回退成数字，不隐藏信息。
    switch (format) {
    case 37:
        return QStringLiteral("R8G8B8A8_UNORM");
    case 43:
        return QStringLiteral("R8G8B8A8_SRGB");
    case 44:
        return QStringLiteral("B8G8R8A8_UNORM");
    case 50:
        return QStringLiteral("B8G8R8A8_SRGB");
    case 58:
        return QStringLiteral("A2B10G10R10_UNORM_PACK32");
    case 97:
        return QStringLiteral("R16G16B16A16_SFLOAT");
    default:
        return QStringLiteral("VkFormat %1").arg(format);
    }
}

void FgShell::publishStats(bool interpolatedThisStep)
{
    const RhineFg::FrameInputs &inputs = RhineFg::latest();
    m_stats.layerActive = m_started && m_window && m_window->isVisible();
    m_stats.interpolationAvailable = m_fgAvailable;
    m_stats.interpolationWanted = FgConfig::fromEnvironment().enabled;
    m_stats.interpolationActive = interpolatedThisStep;
    m_stats.depthReady = inputs.depth != nullptr;
    m_stats.motionVectorsReady = inputs.motionVectors != nullptr;
    m_stats.presented = m_presentedFrames;
    m_stats.interpolated = m_interpolatedFrames;
    m_stats.rendered = m_renderedFrames;
    m_stats.renderSize = inputs.renderSize;
    m_stats.displaySize = m_presenter ? m_presenter->extent() : QSize();
    RhineFg::publish(m_stats);
}

bool FgShell::composite(QRhiCommandBuffer *cb, QRhiTexture *base, QRhiTexture *overlay,
                        quint32 imageIndex)
{
    if (imageIndex >= m_swapTargets.size())
        return false;
    QRhiTexture *target = m_swapTextures[imageIndex].get();
    // 这一帧要整屏重写，所以声明旧布局为 UNDEFINED：QRhi 每次都会发一次
    // UNDEFINED → COLOR_ATTACHMENT 的转换，不必跟踪 present 之后的真实布局。
    target->setNativeLayout(int(VK_IMAGE_LAYOUT_UNDEFINED));

    // 排障：合成源与交换链尺寸不一致时，画面会被拉伸或错位——改窗口大小"闪一下"
    // 如果还有残留，这一行会直接指出来。只在尺寸变化时打，避免刷屏。
    if (qEnvironmentVariableIsSet("RHINE_FG_DIAG")) {
        static QSize reported;
        const QSize source = base ? base->pixelSize() : QSize();
        if (source != target->pixelSize() && source != reported) {
            reported = source;
            qWarning().noquote() << "FG 合成: 尺寸不一致 源" << source << "目标"
                                 << target->pixelSize();
        }
    }

    QRhiShaderResourceBindings *srb = bindingsFor(base, overlay);
    if (!srb)
        return false;

    if (!m_pipeline) {
        m_pipeline = m_rhi->newGraphicsPipeline();
        m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader("fgComposite.vert")},
                                     {QRhiShaderStage::Fragment, loadShader("fgComposite.frag")}});
        m_pipeline->setVertexInputLayout({});
        m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        m_pipeline->setCullMode(QRhiGraphicsPipeline::None);
        m_pipeline->setDepthTest(false);
        m_pipeline->setShaderResourceBindings(m_pipelineBindings);
        m_pipeline->setRenderPassDescriptor(m_swapPasses[imageIndex].get());
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
    cb->beginPass(m_swapTargets[imageIndex].get(), Qt::black, {1.0f, 0}, batch);
    cb->setGraphicsPipeline(m_pipeline);
    cb->setShaderResources(srb);
    cb->setViewport({0, 0, float(pixelSize.width()), float(pixelSize.height())});
    cb->draw(3);
    cb->endPass();

    // 呈现引擎要求图像处于 PRESENT_SRC_KHR。QRhi 不知道这张图会被 present，
    // 所以这一步只能自己发屏障。
    const auto *handles =
        static_cast<const QRhiVulkanCommandBufferNativeHandles *>(cb->nativeHandles());
    if (!handles || handles->commandBuffer == VK_NULL_HANDLE)
        return false;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_presenter->imageAt(imageIndex);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    QVulkanDeviceFunctions *deviceFunctions = m_instance->deviceFunctions(m_device);
    if (!deviceFunctions)
        return false;

    cb->beginExternal();
    deviceFunctions->vkCmdPipelineBarrier(handles->commandBuffer,
                                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, //
                                                  0, 0, nullptr, 0, nullptr, 1, &barrier);
    cb->endExternal();
    return true;
}

bool FgShell::presentOne(const VulkanPresenter::Frame &frame, bool allowInterpolation,
                         bool *interpolatedOut)
{
    if (interpolatedOut)
        *interpolatedOut = false;

    QRhiCommandBuffer *cb = nullptr;
    m_probe.restart();
    const QRhi::FrameOpResult begun = m_rhi->beginOffscreenFrame(&cb);
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
    if (composed) {
        // 注入失败不能静默：present 会退回 vkDeviceWaitIdle 硬同步（单次 50ms 级），
        // 帧率会掉到同步节奏而日志里什么都看不到。这里直接把原因写进 m_error 并放弃这一帧。
        if (!m_presenter->installSubmitParams(m_rhi.get(), frame)) {
            m_error = m_presenter->lastError();
            m_rhi->endOffscreenFrame();
            return false;
        }
        m_presentedFrames++;
        if (interpolated)
            m_interpolatedFrames++;
    }
    m_probe.restart();
    m_rhi->endOffscreenFrame();
    m_stats.endMs = blend(m_stats.endMs, tookMs());
    if (!composed)
        return false;
    if (interpolatedOut)
        *interpolatedOut = interpolated;

    m_probe.restart();
    const bool presented = m_presenter->present(frame, nullptr);
    // 呈现间隔与分项耗时都按"真的上屏了"来记，失败的那次不计入，免得把
    // 交换链重建的停顿算成 FG 的成本。
    if (presented) {
        m_stats.presentMs = blend(m_stats.presentMs, tookMs());
        if (m_presentClock.isValid())
            recordGap(double(m_presentClock.nsecsElapsed()) / 1e6);
        m_presentClock.restart();
        // 真的上屏了，说明重建是有效的：清掉"连续重建次数"，别让偶发的
        // 一两次 OutOfDate 累积成误判。
        m_recreateStreak = 0;
        if (m_framesSinceRecreate < 1000)
            ++m_framesSinceRecreate;
    }
    return presented;
}

void FgShell::step()
{
    if (!m_frameClock.isValid())
        m_frameClock.start();
    // 两步之间的空档：定时器排队 + 事件循环。和渲染成本分开统计，否则"各项都 <1ms、
    // 帧间隔 30ms+"根本看不出钱花在哪。
    if (!m_stepClock.isValid()) {
        m_stepClock.start();
    } else if (m_stepEndNs > 0) {
        m_stats.idleGapMs = blend(m_stats.idleGapMs,
                                  double(m_stepClock.nsecsElapsed() - m_stepEndNs) / 1e6);
    }
    if (!m_window->isVisible()) {
        scheduleNext();
        return;
    }
    // 引擎空闲（没有动画也没有输入）时默认路径是完全停渲的。这里不能停——
    // 自建呈现层停掉就没人 present，窗口会被合成器判成卡死——所以降频到 10Hz，
    // 并且跳过高成本的光流/插值，只用已经渲染好的场景纹理重新合成。
    const bool idle = m_backend->performance().value(QStringLiteral("idle")).toBool();
    // 呈现层一旦接管窗口，设置页的开关就能立刻生效：和渲染器读 RHINE_FG 一样，
    // 这里每帧重新解析一次（Host::setPreference 写偏好时会同步刷新这个变量）。
    // 关掉只是不再跑光流/插值；呈现层本身要重启才能退回 QQuickWindow 路径。
    const bool interpolationWanted = FgConfig::fromEnvironment().enabled;
    syncSizes();
    if (m_needRecreate && !recreateSwapchain()) {
        qWarning().noquote() << "FG:" << m_error;
        m_needRecreate = false;
        scheduleNext();
        return;
    }
    if (!ensureSwapchainResources() || !ensureCompositeResources()) {
        qWarning().noquote() << "FG:" << m_error;
        scheduleNext();
        return;
    }

    // 内容没变就整步跳过：不渲染、不插值、不合成。
    // 以前这里每步盲渲盲呈现，静止画面上反复拿同一对旧帧插值——纯浪费 GPU，
    // 静止时的掉帧就是这么来的。判定信号：
    //   a) 引擎步进计数变了 → 场景状态推进了，必须重画；
    //   b) 离屏窗口收到过 UpdateRequest → UI 自己有变化（悬停、提示、时钟）；
    //   c) 交换链重建/尺寸变化（m_forcePresent）→ 纹理内容已失效。
    // 实测 UpdateRequest 在离屏 QQuickWindow 上并不可靠，所以 a) 是主信号。
    const auto perf = m_backend->performance();
    const quint64 engineSteps = perf.value(QStringLiteral("steps")).toULongLong();
    const bool engineAdvanced = engineSteps != m_lastEngineSteps;
    if (!m_host->contentDirty() && !engineAdvanced && !m_forcePresent) {
        ++m_skippedSteps;
        publishStats(false);
        scheduleNext(perf.value(QStringLiteral("idle")).toBool() ? 100 : 4);
        return;
    }
    m_lastEngineSteps = engineSteps;

    // 上一步留下的真实帧还没上屏：这一步只做它的呈现。
    // 插值帧与真实帧因此拆进相邻的两个调度步——一步一次 present，事件循环在两者
    // 之间能拿回执行权；以前两步的活在一步里做完，输入和窗口事件会被整段吞掉。
    if (m_pendingRealFrame) {
        m_pendingRealFrame = false;
        VulkanPresenter::Frame frame;
        m_probe.start();
        const auto acquired = m_presenter->acquire(&frame);
        m_stats.acquireMs = blend(m_stats.acquireMs, tookMs());
        if (acquired == VulkanPresenter::AcquireResult::OutOfDate) {
            m_needRecreate = true;
            scheduleNext();
            return;
        }
        if (acquired == VulkanPresenter::AcquireResult::Ok
            || acquired == VulkanPresenter::AcquireResult::Suboptimal) {
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

    armFrameGeneration();
    VulkanPresenter::Frame frame;
    m_probe.start();
    const auto acquired = m_presenter->acquire(&frame);
    m_stats.acquireMs = blend(m_stats.acquireMs, tookMs());
    // Suboptimal 只表示"这张图能呈现，但交换链不再最优"。以前这里把图一起丢掉并整链重建
    // （deviceWaitIdle + 5 张 image/view/semaphore + 全部纹理与管线），代价是掉几帧、
    // 画面还会闪一下。重建只由 OutOfDate 与窗口 Resize/Expose 驱动。
    if (acquired == VulkanPresenter::AcquireResult::OutOfDate) {
        m_needRecreate = true;
        if (++m_outOfDateCount % 100 == 1)
            qWarning().noquote() << "FG: acquire 返回 OutOfDate，累计" << m_outOfDateCount;
        scheduleNext();
        return;
    }
    if (acquired == VulkanPresenter::AcquireResult::Suboptimal) {
        static bool warnedSuboptimal = false;
        if (!warnedSuboptimal) {
            warnedSuboptimal = true;
            qWarning().noquote()
                << "FG: 交换链 suboptimal，继续使用（等 OutOfDate 或窗口事件再重建）";
        }
    } else if (acquired != VulkanPresenter::AcquireResult::Ok) {
        // Timeout：窗口最小化或设备忙，跳过这一帧即可，重建没有意义。
        if (++m_acquireTimeoutCount % 100 == 1)
            qWarning().noquote() << "FG: acquire 超时（窗口不可见/合成器未取图），累计"
                                 << m_acquireTimeoutCount;
        scheduleNext();
        return;
    }

    m_probe.restart();
    const bool sceneRendered = m_host->renderFrame();
    m_stats.sceneMs = blend(m_stats.sceneMs, tookMs());
    if (!sceneRendered) {
        qWarning().noquote() << "FG: 离屏渲染失败" << m_host->lastError();
        scheduleNext();
        return;
    }
    m_renderedFrames++;  // 场景真的重渲了一帧（FG 的输入新鲜度就看它）

    bool interpolated = false;
    // 重建后的前两帧只呈现真实帧：那时 QML 可能还在按旧尺寸排版、交换链也是全新的，
    // 跨尺寸插出来的帧就是满屏错色（改窗口大小时"闪一下"的来源）。
    const bool interpolationAllowed = !idle && interpolationWanted && m_framesSinceRecreate >= 2;
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
    // 空闲时把节奏压到 10Hz：既让 UI 的偶发变化（悬停、提示）仍能在 100ms 内上屏，
    // 又不用为一张静止画面持续跑光流。
    scheduleNext(idle ? 100 : 0);
}

void FgShell::reportStats()
{
    // 第一行要在启动后很快出现：空闲时呈现节奏会降到 10Hz，等"每 300 帧一行"
    // 要半分钟才看得到第一条，冒烟测试和现场排障都等不起。
    const bool firstWindow = m_presentedFrames == 60;
    if (!firstWindow && (m_presentedFrames < 60 || m_presentedFrames % 300 != 0))
        return;
    const RhineFg::FrameInputs &inputs = RhineFg::latest();
    qWarning().noquote() << "FG stats: presented" << m_presentedFrames << "interpolated"
                         << m_interpolatedFrames << "tapSerial" << inputs.serial << "depth"
                         << (inputs.depth ? "ok" : "null") << "mv"
                         << (inputs.motionVectors ? "ok" : "null") << "render"
                         << inputs.renderSize << "display" << inputs.displaySize << "gapP50"
                         << QString::number(m_stats.gapP50Ms, 'f', 1) << "ms/frame"
                         // 分项：取图 / 场景 / 合成 / 上屏（上屏在 FIFO 下会阻塞到刷新点，
                         // 它就是"为什么每帧都掉到 20ms 档"的直接证据，必须一起打出来）
                         << "取图" << QString::number(m_stats.acquireMs, 'f', 2)
                         << "场景" << QString::number(m_stats.sceneMs, 'f', 2)
                         // 光流+深度重建（prepare）与插值本身（dispatch）必须分开看：
                         // 两者的优化手段完全不同，混在一起就不知道该调哪个。
                         << "光流" << QString::number(m_stats.prepareMs, 'f', 2)
                         << "插值" << QString::number(m_stats.dispatchMs, 'f', 2)
                         << "合成" << QString::number(m_stats.compositeMs, 'f', 2)
                         << "上屏" << QString::number(m_stats.presentMs, 'f', 2)
                         << "起始" << QString::number(m_stats.beginMs, 'f', 2)
                         << "收尾" << QString::number(m_stats.endMs, 'f', 2)
                         << "空档" << QString::number(m_stats.idleGapMs, 'f', 2)
                         << "跳过" << m_skippedSteps
                         << (m_frameClock.isValid()
                                 ? m_frameClock.restart() / double(m_presentedFrames - m_statsBase)
                                 : 0.0);
    m_statsBase = m_presentedFrames;
}

void FgShell::scheduleNext(int delayMs)
{
    // 单次定时器串起来而不是固定间隔：FIFO present 自带节流，让事件循环在两次
    // 呈现之间拿到执行权（输入、窗口事件），比追着刷新率跑更稳。
    if (!m_timer)
        return;
    if (m_stepClock.isValid())
        m_stepEndNs = m_stepClock.nsecsElapsed();  // 空档计时的起点
    m_timer->setSingleShot(true);
    m_timer->start(delayMs);
}

bool FgShell::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_window.get())
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Close:
        // 与 Ctrl+Q 同一条退出顺序：先把交换链/呈现层拆掉，再让事件循环退出。
        // 反过来（只 quit()）实测进程根本不退：呈现层还活着，析构里的排空卡死。
        qWarning().noquote() << "FG: 窗口关闭 → 先拆呈现层再退出事件循环";
        shutdown();
        QCoreApplication::quit();
        return false;
    case QEvent::Expose:
        if (m_window->isExposed()) {
            m_needRecreate = true;
            m_forcePresent = true;
        }
        break;
    case QEvent::Resize:
        m_needRecreate = true;
        m_forcePresent = true;
        break;
    default:
        break;
    }

    // 探针（RHINE_KEY_PROBE=1）：打印真正到达可见窗口的按键。
    // 被 Qt 快捷键地图（QShortcutMap）消费掉的按键不会出现在这里——用它区分
    // "快捷键没匹配上"与"快捷键已触发、但动作没生效"。
    if (qEnvironmentVariableIntValue("RHINE_KEY_PROBE") == 1
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
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
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::HoverEnter:
        case QEvent::HoverLeave:
        case QEvent::HoverMove:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
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
        case QEvent::InputMethod:
            QCoreApplication::sendEvent(overlay, event);
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

// 同步版：立刻把 QML 焦点补回来（排队恢复会被后续事件搅掉，实测重新聚焦后仍为 null）。
void FgShell::ensureOverlayFocus(QQuickWindow *overlay)
{
    if (!overlay || !m_host || overlay->activeFocusItem())
        return;
    if (QQuickItem *root = m_host->overlayRootItem())
        QMetaObject::invokeMethod(root, "focusInputLayer", Qt::DirectConnection);
}


bool FgShell::start()
{
    if (!compiledIn()) {
        m_error = QStringLiteral("这个构建没有编入 FG（需要 Vulkan + FFX 着色器）");
        return false;
    }
    m_size = QSize(1280, 720);
    if (!initializeVulkan())
        return false;
    if (!initializeLayers())
        return false;

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &FgShell::step);

    // 预检：有些设备能建出交换链，却撑不住持续上屏（典型是"没有直连显示输出"的独显
    // ——surface 由跑在集显上的合成器管理）。实测独显上会先成功呈现 4 帧，随后
    // OutOfDate 一路涨到几万次、再也没出过一帧（白窗 + 空转）。
    // 判据要贴着"真坏了"写，不能贴"慢了"：首帧编译着色器本来就是慢的。
    //   · OutOfDate 超过 200 次 → 交换链撑不住，判死；
    //   · 3 秒过去一帧都没上屏 → 判死；
    //   · 其余情况放行（能出 20 帧就提前结束预检，不拖慢启动）。
    // 判死后由 main.cpp 干净回退到默认 QQuickWindow 路径。这比
    // vkGetPhysicalDeviceSurfaceSupportKHR 靠谱：后者对这类设备仍返回支持。
    {
        QElapsedTimer probeClock;
        probeClock.start();
        while (probeClock.elapsed() < 3000 && m_presentedFrames < 20 && !m_failed
               && m_outOfDateCount <= 200) {
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

    // 排障：RHINE_FG_DIAG=1 时每秒打一行真实计数。presented 只在每 300 帧打印，
    // 现场判断"到底是在跑还是在空转"时等不起，这个小计时器直接给答案。
    if (qEnvironmentVariableIsSet("RHINE_FG_DIAG")) {
        auto *diag = new QTimer(this);
        diag->setInterval(1000);
        connect(diag, &QTimer::timeout, this, [this] {
            qWarning().noquote() << "FG diag: presented" << m_presentedFrames << "rendered"
                                 << m_renderedFrames << "skipped" << m_skippedSteps << "acqTimeout"
                                 << m_acquireTimeoutCount << "outOfDate" << m_outOfDateCount
                                 << "visible" << (m_window && m_window->isVisible()) << "pendingReal"
                                 << m_pendingRealFrame << "gapP50"
                                 << QString::number(m_stats.gapP50Ms, 'f', 1);
        });
        diag->start();
    }

    m_backend->setWindow(m_window.get());
    m_window->installEventFilter(m_backend);
    m_started = true;
    // 交换链与设备信息整个生命周期不变，这里写一次就够，之后 publishStats 带出去。
    m_stats.swapchainImages = int(m_presenter->imageCount());
    m_stats.swapchainFormat = swapchainFormatName(int(m_presenter->colorFormat()));
    m_stats.presentMode = m_presenter->presentModeName();
    m_stats.adapterName = QString::fromUtf8(m_rhi->driverInfo().deviceName);
    m_stats.backendName = QStringLiteral("Vulkan");
    RhineFg::publishActive(m_fgAvailable);
    qWarning().noquote() << "FG: 呈现层已启动；插值" << (m_fgAvailable ? "可用" : "不可用") << "，"
                         << m_presenter->imageCount() << "张交换链图像" << m_presenter->extent()
                         << "，呈现模式" << m_presenter->presentModeName();
    return true;
}

int FgShell::run()
{
    if (!m_started)
        return 1;
    m_timer->start(0);
    return 0;
}
