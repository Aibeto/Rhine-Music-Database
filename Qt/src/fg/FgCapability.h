#pragma once
#include <QtGlobal>

#include <QString>

namespace RhineFg {

/**
 * 帧生成能力的"对外快照"。
 *
 * 为什么需要这份快照：真正的实现（FgShell / FgBackend）依赖 <vulkan/vulkan.h> 与 FFX 的
 * 后端头文件，只在合适的平台上编进二进制；而设置界面的代码在 RhineCore 里，必须在没有
 * Vulkan 的平台上也能编译。所以启动路径把结论（几十个 bool 加一句原因）写进这里，
 * 设置界面只读这份快照，不反向依赖实现。
 *
 * 生命周期：由 main() 在启动路径上填一次（呈现层真正接管后再刷新一次），此后随运行期
 * 降级刷新。所有字段在 GUI 线程访问——呈现层也跑在 GUI 线程，不需要加锁。
 */
struct CapabilitySnapshot {
    /// 这个二进制里带了 FFX 的帧生成（FI/OF）组件。
    bool compiledIn = false;
    /**
     * 应用自持交换链的呈现层已经接管窗口。
     *
     * 注意它与"插值开着"是两件事：呈现层常驻（能力可用时启动即接管），偏好里的开关
     * 只决定每次呈现要不要插入补偿帧，所以开关能立刻生效、不需要重启。
     */
    bool presentLayerActive = false;
    /// 帧生成上下文真的建起来了（光流 + 插值都能跑）。运行期失败会退回 false。
    bool interpolationAvailable = false;
    /// 不可用时给用户看的原因；可用时为空。
    QString reason;
#ifdef Q_OS_DARWIN
    bool temporalAvailable = false;
#endif
};

inline CapabilitySnapshot &capabilityMutable()
{
    static CapabilitySnapshot snapshot;
    return snapshot;
}

inline const CapabilitySnapshot &capability()
{
    return capabilityMutable();
}

inline void publishCapability(bool compiledIn, bool presentLayerActive, bool interpolationAvailable,
                              const QString &reason = QString())
{
    auto &snapshot = capabilityMutable();
    snapshot.compiledIn = compiledIn;
    snapshot.presentLayerActive = presentLayerActive;
    snapshot.interpolationAvailable = interpolationAvailable;
    snapshot.reason = reason;
}

/// 呈现层接管成功；插值是否可用由上一步 arm 的结果决定。
inline void publishActive(bool interpolationAvailable)
{
    publishCapability(true, true, interpolationAvailable);
}

/**
 * 运行期刷新插值可用性，但保持"呈现层已接管"不变。
 *
 * 呈现层常驻后，插值上下文可能在建起来之后才失败（换分辨率、驱动拒绝、FFX 报错）。
 * 这时不该把整层判死——合成照旧走，只是没有补偿帧，所以单独降级。
 */
inline void publishInterpolationState(bool interpolationAvailable, const QString &reason = QString())
{
    auto &snapshot = capabilityMutable();
    publishCapability(snapshot.compiledIn, snapshot.presentLayerActive, interpolationAvailable, reason);
}

inline void publishInactive(const QString &reason)
{
    const auto compiledIn = capability().compiledIn;
    publishCapability(compiledIn, false, false, reason);
}

/**
 * 设置页"帧生成"这一行的结论。
 *
 * FG 与 FSR3 共用同一批后端能力，但额外依赖"由应用自己拥有交换链并负责呈现"。呈现层
 * 在能力可用时常驻接管窗口，于是只有四种情况：
 *   1. 呈现层已接管且插值可用：开关立刻生效，永不重启；
 *   2. 呈现层已接管但插值降级：置灰并写明原因（合成仍在跑，只是没有补偿帧）；
 *   3. 构建里没编进 FG、当前后端跑不了、或接管失败：置灰并写明原因；
 *   4. 能力齐备却没接管（被 RHINE_FG_LAYER=off 关掉）：置灰，不误导用户去点。
 * 判定放在这里而不是设置界面里，是为了能在没有 GPU 的机器上单测。
 */
struct FrameGenerationUi {
    /// 这一行是否可点。
    bool clickable = false;
    /// 右侧展示的文案（也是置灰时的原因）。
    QString value;
};

inline FrameGenerationUi frameGenerationUi(bool compiledIn, bool presentLayerActive,
                                           bool interpolationAvailable, const QString &reason,
                                           bool backendCapable, bool preferenceOn)
{
    if (!compiledIn) {
        return {false, reason.isEmpty() ? QStringLiteral("此构建未编入") : reason};
    }
    /// 与 FSR3 同一判据：opengl 后端没有 FFX 计算后端可用。
    if (!backendCapable) {
        return {false, QStringLiteral("当前图形后端不支持")};
    }
    if (presentLayerActive && interpolationAvailable) {
        return {true, preferenceOn ? QStringLiteral("开启") : QStringLiteral("关闭")};
    }
    /// 接管失败或运行期降级都会在快照里留下原因：那是真的不可用，别让用户反复点。
    if (!reason.isEmpty()) {
        return {false, reason};
    }
    if (!presentLayerActive) {
#ifdef Q_OS_LINUX
        // fg=off 启动时呈现层**本来就不接管**（内容帧实测高 35%），这不是故障，
        // 而是"当前设置就是关的"。此时必须允许用户点开，否则开关永远打不开。
        if (!preferenceOn) {
            return {true, QStringLiteral("关闭")};
        }
        // 用户已打开偏好，但本次进程启动时还是关的 → 呈现层没接管。这不是错误，
        // 而是"下次启动才生效"，提示重启而不是判死。
        return {true, QStringLiteral("已开启 · 重启生效")};
#else
        return {false, QStringLiteral("呈现层未接管")};
#endif
    }
    /// 接管了却没有上下文又没留原因，属于不该发生的状态，宁可置灰也不假装能开。
    return {false, QStringLiteral("插值暂不可用")};
}

} // namespace RhineFg
