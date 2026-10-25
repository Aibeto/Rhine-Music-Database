#pragma once

#include "FgStats.h"
#include "VulkanPresenter.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <array>
#include <memory>

QT_BEGIN_NAMESPACE
class QQmlEngine;
class QEvent;
class QRhi;
class QQuickItem;
class QQuickWindow;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiSampler;
class QRhiResource;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QTimer;
class QVulkanInstance;
class QWindow;
QT_END_NAMESPACE

class Engine;
class FgBackend;
class FgHost;

/**
 * FG 呈现层的总装：自建 QVulkanInstance / QWindow / VkSwapchainKHR，自己驱动渲染循环。
 *
 * 为什么需要这一层：FG 必须在"显示分辨率的一帧"上插入一张插值帧，而 Qt 的
 * QQuickWindow 自己拥有并呈现交换链，应用插不进去。所以 FG 路径改用
 * QQuickRenderControl 把 3D 与 UI 渲到离屏纹理（FgHost），由本类接管交换链与 present。
 *
 * 一帧的实际顺序（每次 step()）：
 *   1. acquire 一张交换链图像；
 *   2. FgHost::renderFrame() 把 3D 与 UI 两个场景各渲一遍；
 *   3. 起一个离屏帧：FgBackend 跑光流 + 帧插值，然后合成通道把
 *      (插值帧 或 当前帧) 与 UI 叠在一起，写进交换链图像；
 *   4. installSubmitParams 注入"等图像可用 / 通知渲染完成"两个信号量，endOffscreenFrame；
 *   5. present。若本帧产出了插值帧，再 acquire 一张图、只做合成与 present，
 *      于是每个渲染帧呈现两次——这就是 FG 的收益来源。
 *
 * 失败策略：start() 任何一步失败都返回 false 并留下可读原因，调用方回退到默认的
 * QQuickWindow 路径。运行期 FG 算法失败则退化成"每帧直接呈现当前帧"，画面继续动。
 */
class FgShell : public QObject
{
    Q_OBJECT

public:
    FgShell(QQmlEngine &engine, Engine &backend, QObject *parent = nullptr);
    ~FgShell() override;

    /// 编译期是否可能支持（Vulkan 后端 + FFX 已编入）。
    static bool compiledIn();

    /// 用户是否要求开启（RHINE_FG=on 或设置页 panel/fg）。
    static bool requested();

    /// 建立实例/设备/窗口/离屏层/交换链并打开渲染循环。失败时 lastError() 给出原因。
    /**
     * @brief 退出前先拆呈现层：停掉呈现定时器、释放交换链资源与 VkSwapchainKHR。
     *
     * 必须先于窗口销毁调用。交换链还活着时窗口先没了，驱动的 WSI 会拿已销毁的
     * wl_surface（Windows 上是已销毁的 HWND）去做 roundtrip，退出即崩。
     * 幂等，析构路径会再调用一次。
     */
    void shutdown();

    bool start();

    /// 启动后的收尾：返回给 main() 的退出码。渲染由事件循环里的定时器驱动。
    int run();

    QString lastError() const { return m_error; }
    bool frameGenerationActive() const { return m_fgAvailable; }
    QWindow *window() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 同步补焦点：按键到达时若没有焦点项就当场重建。
    void ensureOverlayFocus(QQuickWindow *overlay);
    void step();
    void scheduleNext(int delayMs = 0);
    void reportStats();
    /// 上一次统计时的帧数，用来把 ms/frame 算成"这一窗口内"的均值。
    int m_statsBase = 0;
    bool initializeVulkan();
    bool initializeLayers();
    bool ensureSwapchainResources();
    void releaseSwapchainResources();
    bool ensureCompositeResources();
    QRhiShaderResourceBindings *bindingsFor(QRhiTexture *base, QRhiTexture *overlay);
    bool recreateSwapchain();
    void syncSizes();
    /// 帧外准备：按最新输入重建上下文并写入相机参数。必须在 beginOffscreenFrame 之前。
    bool armFrameGeneration();
    /// 记录 FG 的命令，返回"本帧是否有可用的插值帧"。
    bool recordFrameGeneration(QRhiCommandBuffer *cb);
    /// 把 base（插值帧或当前帧）与 UI 合成到交换链图像，并转到 PRESENT_SRC。
    bool composite(QRhiCommandBuffer *cb, QRhiTexture *base, QRhiTexture *overlay,
                   quint32 imageIndex);
    bool presentOne(const VulkanPresenter::Frame &frame, bool allowInterpolation,
                    bool *interpolatedOut);
    QString resolveQmlUrl(const QString &file) const;

    /// 读走 m_probe 的耗时并重新起表（毫秒）。
    double tookMs();
    /// 把一次测量并入滑动平均：单帧抖动大，面板要的是可读的趋势。
    static double blend(double previous, double sample);
    /// 记一次呈现间隔，并按需重算分位（呈现层的帧率统计全靠它，FG 路径没有 frameSwapped）。
    void recordGap(double gapMs);
    void refreshGapStats();
    /// 把 m_stats 的最新内容发给性能面板。
    void publishStats(bool interpolatedThisStep);
    /// 把 VkFormat 转成面板上能读的格式名（未知格式回退成数字）。
    static QString swapchainFormatName(int format);

    QQmlEngine *m_engine = nullptr;
    Engine *m_backend = nullptr;

    std::unique_ptr<QVulkanInstance> m_instance;
    std::unique_ptr<QWindow> m_window;
    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<FgHost> m_host;
    std::unique_ptr<FgBackend> m_fg;
    std::unique_ptr<VulkanPresenter> m_presenter;

    // 每个交换链图像一份"QRhi 视角的纹理 + 渲染目标 + 通道描述符"。
    std::vector<std::unique_ptr<QRhiTexture>> m_swapTextures;
    std::vector<std::unique_ptr<QRhiTextureRenderTarget>> m_swapTargets;
    std::vector<std::unique_ptr<QRhiRenderPassDescriptor>> m_swapPasses;

    QRhiBuffer *m_compositeUbo = nullptr;
    QRhiSampler *m_sampler = nullptr;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    // 管线要求 SRB 布局已确定，所以用一张 1×1 占位纹理建一份"模板" SRB；
    // 逐帧真正用的 SRB 由 bindingsFor() 按实际输入纹理缓存。
    QRhiTexture *m_pipelineBase = nullptr;
    QRhiShaderResourceBindings *m_pipelineBindings = nullptr;
    QHash<quint64, QRhiShaderResourceBindings *> m_bindingCache;
    // 建在本类自己的 QRhi 上、需要显式释放的资源（UBO、采样器、占位纹理）。
    QList<QRhiResource *> m_ownedResources;

    QTimer *m_timer = nullptr;
    QElapsedTimer m_frameClock;
    // QRhi 建的设备句柄：发自己的屏障时要用设备级函数表。
    VkDevice m_device = VK_NULL_HANDLE;
    QSize m_size;
    // 交换链包装纹理对应的真实尺寸；presenter 的 extent 变化就要重建。
    QSize m_swapSize;
    bool m_fgAvailable = false;
    bool m_fgArmed = false;
    bool m_needRecreate = false;
    bool m_started = false;
    quint64 m_lastTapSerial = 0;
    quint64 m_presentedFrames = 0;
    quint64 m_interpolatedFrames = 0;
    // 场景真实重渲的帧数（不是呈现帧数）：面板里的"渲染 FPS"用它。
    quint64 m_renderedFrames = 0;
    QString m_error;

    // —— 性能面板用的运行时统计 ——
    // 复用同一个计时器串行测量，避免每帧构造 QElapsedTimer；
    // 面板要的是分项趋势，不是单帧精度，所以一律走滑动平均。
    QElapsedTimer m_probe;
    RhineFg::Stats m_stats;
    // 呈现间隔环形缓冲：FG 路径不经过 QQuickWindow，拿不到 frameSwapped 事件，
    // 所以分位与卡顿计数自己算。600 个样本在 60Hz 下约 10 秒。
    static constexpr int kGapCapacity = 600;
    std::array<double, kGapCapacity> m_gapRing{};
    int m_gapCursor = 0;
    int m_gapFilled = 0;
    QElapsedTimer m_presentClock;
    // 两步之间的空档（定时器等待 + 事件循环排队）单独计时：它和 endOffscreenFrame
    // 是仅有的两个"没被分项计时覆盖"的时间块，排障时要能把它们和渲染成本分开看。
    QElapsedTimer m_stepClock;
    qint64 m_stepEndNs = 0;
    /// 插值帧先上屏、真实帧留到下一步：一步只做一次 present。
    bool m_pendingRealFrame = false;
    /// 交换链重建/窗口尺寸变化后必须无条件重画一次（纹理内容已失效）。
    bool m_forcePresent = true;
    /// 连续跳过的步数：引擎在推进却一直没有脏标记时用它兜底，避免画面冻住。
    int m_skipStreak = 0;
    /// 累计跳过的步数，写进统计日志，便于确认"静止时确实没在空转"。
    quint64 m_skippedSteps = 0;
    /// 上次渲染时引擎的步进计数：它变了才说明场景状态真的推进了。
    quint64 m_lastEngineSteps = 0;
    /// 排障计数：acquire 超时 / OutOfDate 的次数（空转时靠它们判断卡在哪条提前返回）。
    quint64 m_acquireTimeoutCount = 0;
    quint64 m_outOfDateCount = 0;
    /// 连续强制重建的次数（有一帧成功上屏就清零）。用来区分"重建能救活"和
    /// "这台设备就是不能呈现"：后者重建多少次都没用，要尽快判定失败让上层回退。
    int m_recreateStreak = 0;
    /// 距上次交换链重建成功上屏了多少帧。重建后前两帧不做插值：
    /// 那时离屏层可能还在按旧尺寸排版，跨尺寸插出来的帧会整屏错色。
    int m_framesSinceRecreate = 99;
    /// 判定呈现层无法工作。自检循环据此提前结束，不再白等超时。
    bool m_failed = false;
};
