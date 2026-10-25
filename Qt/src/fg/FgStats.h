#pragma once

#include <QSize>
#include <QString>

namespace RhineFg {

/**
 * 呈现层每帧写一次、性能面板读的运行时统计。
 *
 * 与 FgCapability 同样必须 header-only 且不 include 任何 Vulkan 头：读它的 FrameProfiler
 * 属于 RhineCore，在没有 Vulkan 的平台上也要能编译。写入方（FgShell）跑在 GUI 线程，
 * 读取方（FrameProfiler::sample）也在 GUI 线程，所以不加锁，只保证结构体拷贝是整体的。
 *
 * 时间单位统一为毫秒。计数器是累计值，面板自己取差值算 FPS。
 */
struct Stats {
    /// 呈现层是否在跑（false 时其余字段无意义）。
    bool layerActive = false;
    /// 本次启动的插值上下文是否可用。
    bool interpolationAvailable = false;
    /// 设置里的开关是否要求插值（每帧重读偏好，热切换靠它）。
    bool interpolationWanted = false;
    /// 这一帧是否真的插入了补偿帧（开关、空闲节流、reset 帧都会影响它）。
    bool interpolationActive = false;
    /// 场景深度 / 运动矢量这一帧是否拿到了（FG 的输入前提）。
    bool depthReady = false;
    bool motionVectorsReady = false;

    /// 累计计数：呈现的帧数（含插值帧）、其中插值帧数、场景真实渲染帧数。
    quint64 presented = 0;
    quint64 interpolated = 0;
    quint64 rendered = 0;

    /// 分项耗时（近若干帧的滑动平均）。
    double acquireMs = 0.0;
    double sceneMs = 0.0;
    double prepareMs = 0.0;
    double dispatchMs = 0.0;
    double compositeMs = 0.0;
    double presentMs = 0.0;
    /// QRhi 的离屏帧边界：这两处会隐式等 GPU 完成，之前的计时刚好把它们漏掉了，
    /// 于是出现"各项耗时都 <1ms、帧间隔却 30ms+"的怪现象。
    double beginMs = 0.0;
    double endMs = 0.0;
    /// 两步之间（定时器等待 + 事件循环排队）花掉的时间。
    double idleGapMs = 0.0;

    /// 呈现间隔统计（由呈现层自己的环形缓冲算出，FG 路径下没有 frameSwapped 可用）。
    double gapP50Ms = 0.0;
    double gapP95Ms = 0.0;
    double gapP99Ms = 0.0;
    double gapMaxMs = 0.0;
    int over33 = 0;
    int over50 = 0;
    int over100 = 0;
    int sampleCount = 0;

    /// 交换链与设备信息（启动时填一次）。
    int swapchainImages = 0;
    QString swapchainFormat;
    /// 真正生效的呈现模式（FIFO / MAILBOX / IMMEDIATE）。帧率上限几乎全由它决定，
    /// 性能面板要显示出来，否则用户没法判断"这次跑得快是因为换对了呈现模式"。
    QString presentMode;
    QString adapterName;
    QString backendName;

    QSize renderSize;
    QSize displaySize;

    /// 每次 publish 自增；读方可据此判断数据是否还在更新。
    quint64 serial = 0;
};

inline Stats &statsMutable()
{
    static Stats value;
    return value;
}

inline const Stats &stats()
{
    return statsMutable();
}

inline void publish(const Stats &value)
{
    auto &slot = statsMutable();
    const quint64 serial = slot.serial + 1;
    slot = value;
    slot.serial = serial;
}

/// 呈现层退出或从未启动时把状态清掉，避免面板继续显示上一轮的旧数字。
inline void clear()
{
    const quint64 serial = statsMutable().serial + 1;
    statsMutable() = Stats{};
    statsMutable().serial = serial;
}

} // namespace RhineFg
