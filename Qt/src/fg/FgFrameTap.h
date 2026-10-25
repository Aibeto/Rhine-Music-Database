#pragma once
#include <QPointF>
#include <QSize>
#include <QtGlobal>

#include <atomic>

class QRhiTexture;

/// 场景渲染器 → FG 宿主 的每帧输入通道。
///
/// 为什么用进程级单例而不是信号槽：RhiScene 的渲染发生在 ItemRenderer 里，而
/// ItemRenderer 是 QQuickRhiItem 私有的实现类，宿主拿不到它的实例；同时两条渲染
/// 路径（离屏 ItemRenderer 与 FG 宿主）都在 GUI 线程上同步执行，中间没有跨线程
/// 交接，所以一个进程级指针就足以表达"本帧的输入已经就绪"。
///
/// 数据只在宿主调用 renderFrame() 之后读取，读到的一定是刚渲染完那一帧的值。
///
/// 实现放在头文件里（inline 变量）：RhiScene.cpp 属于 RhineCore，而 CMake 把 /fg/
/// 目录排除在 RhineCore 之外，单独出一个 .cpp 会让回归测试链不上符号。
namespace RhineFg {

struct FrameInputs {
    /// 渲染分辨率下的深度（D32F）与运动矢量，归渲染器所有，尺寸变化后失效，
    /// 宿主必须每帧重新取。
    QRhiTexture *depth = nullptr;
    QRhiTexture *motionVectors = nullptr;
    QSize renderSize;
    QSize displaySize;
    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
    float fovYRadians = 1.0f;
    float frameTimeMs = 16.6667f;
    QPointF jitter;
    /// 渲染器要求 FG 丢弃历史（场景切换、尺寸变化、TemporalReset）。
    bool reset = false;
    /// 每发布一次 +1，宿主据此判断"这一帧的输入是不是新的"。
    quint64 serial = 0;
};

namespace detail {
// 只有 GUI 线程会写；用原子量是为了让"是否启用"的读取在渲染器里不需要加锁。
inline std::atomic<bool> enabled{false};
inline FrameInputs inputs;
inline std::atomic<quint64> serial{0};
}  // namespace detail

/// 渲染器在每帧末尾调用；tap 未启用时是空操作，开销为一次原子读。
inline void publish(const FrameInputs &value)
{
    if (!detail::enabled.load(std::memory_order_relaxed))
        return;
    detail::inputs = value;
    detail::inputs.serial = detail::serial.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// 宿主读取最近一次发布的输入。返回引用，调用方不应保存超过一帧。
inline const FrameInputs &latest()
{
    return detail::inputs;
}

inline quint64 latestSerial()
{
    return detail::serial.load(std::memory_order_relaxed);
}

/// FG 宿主启动时打开、退出时关闭（RAII 封装见 TapScope）。
inline void setEnabled(bool value)
{
    if (value == detail::enabled.load(std::memory_order_relaxed))
        return;
    if (!value) {
        // 关闭时清掉纹理指针：它们可能随着渲染目标重建而失效，
        // 留着会让下一次启动读到悬空句柄。
        detail::inputs = FrameInputs{};
    }
    detail::enabled.store(value, std::memory_order_relaxed);
}

inline bool isEnabled()
{
    return detail::enabled.load(std::memory_order_relaxed);
}

/// 只为在栈上开关 tap 的 RAII 便利类，避免早退漏关。
class TapScope {
  public:
    explicit TapScope(bool on = true) : m_previous(isEnabled()) {
        if (on)
            setEnabled(true);
    }
    ~TapScope() {
        setEnabled(m_previous);
    }
    TapScope(const TapScope &) = delete;
    TapScope &operator=(const TapScope &) = delete;

  private:
    bool m_previous;
};

}  // namespace RhineFg
