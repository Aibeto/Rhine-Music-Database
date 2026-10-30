#pragma once

#include <QtCore/QString>

namespace RhineRender {

/**
 * D01：Vulkan 资源账本的窗口增量。
 *
 * 只做计数与格式化，不碰 QRhi，方便离线验证格式化逻辑。创建次数与稳态存量必须分开：
 * 前者说明"这个窗口新建了多少原生对象"（冷加载、扩容、resize 的信号），后者说明
 * "此刻一共有多少对象在活"（内存与绑定成本）。混在一起看时，一次 resize 造成的
 * 批量创建会被误读成每帧开销。
 */
struct VulkanResourceWindow {
    /// 本窗口新建的 Dynamic buffer 数（UBO 与实例 buffer 合计）。
    quint64 dynamicBufferCreates = 0;
    /// 本窗口真正提交的 updateDynamicBuffer 次数与复制字节。
    quint64 dynamicBufferUpdates = 0;
    quint64 dynamicBufferBytes = 0;
    /// 本窗口新建的 SRB 数；bindAborts 是"指针不同但绑定被去重"的次数。
    quint64 srbCreates = 0;
    quint64 srbBinds = 0;
    quint64 srbReuseSkips = 0;
    quint64 pipelineBinds = 0;
    /// 本窗口 beginPass 次数，以及其中深度内容被保留／显式丢弃的 pass 数。
    quint64 passes = 0;
    quint64 depthStorePasses = 0;
    quint64 depthDiscardPasses = 0;

    void clear() { *this = VulkanResourceWindow{}; }
};

/**
 * 某一时刻的稳态存量。usedBytes 只统计"按当前内容实际需要"的字节，capacityBytes 是
 * 分配到的容量上限：两者差得越多，说明 padding／翻倍扩容有多浪费。
 */
struct VulkanResourceLive {
    quint64 dynamicBuffers = 0;
    quint64 capacityBytes = 0;
    quint64 usedBytes = 0;
    quint64 shaderResourceBindings = 0;
    /// "store"／"discard"／"mixed"：主场景深度附件在本窗口的策略。
    QString depthPolicy = QStringLiteral("store");
};

inline QString formatVulkanResourceLine(const VulkanResourceWindow &window,
                                        const VulkanResourceLive &live, qint64 frames)
{
    const double perFrame = frames > 0 ? 1.0 / double(frames) : 0.0;
    return QStringLiteral(
               "资源账本: 窗口 %1 帧 | 新建 buffer %2 次 | 更新 %3 次 / %4 B（每帧 %5 次） | "
               "SRB 新建 %6 次 / 绑定 %7 次（省 %8） | pipeline 绑定 %9 次 | pass %10 次"
               "（深度 store %11 / discard %12） | 存量 buffer %13 个 / 容量 %14 B / 有效 %15 B | "
               "SRB 存量 %16 个 | 深度策略 %17")
        .arg(frames)
        .arg(window.dynamicBufferCreates)
        .arg(window.dynamicBufferUpdates)
        .arg(window.dynamicBufferBytes)
        .arg(double(window.dynamicBufferUpdates) * perFrame, 0, 'f', 2)
        .arg(window.srbCreates)
        .arg(window.srbBinds)
        .arg(window.srbReuseSkips)
        .arg(window.pipelineBinds)
        .arg(window.passes)
        .arg(window.depthStorePasses)
        .arg(window.depthDiscardPasses)
        .arg(live.dynamicBuffers)
        .arg(live.capacityBytes)
        .arg(live.usedBytes)
        .arg(live.shaderResourceBindings)
        .arg(live.depthPolicy);
}

} // namespace RhineRender
