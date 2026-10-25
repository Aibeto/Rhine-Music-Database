#pragma once
#include <QString>

/**
 * 本次启动实际应用的图形后端名（"vulkan" / "opengl" / "d3d12" / "metal"）。
 * 启动选后端的代码在 main.cpp，设置页在 RhineCore 里，而 RhineCore 不链接 QtQuick，
 * 拿不到 QQuickWindow::rendererInterface()，因此由启动逻辑写入、设置页读取。
 * 未写入时返回空，调用方自行回退到显示持久化偏好。
 */
namespace RhineGraphics {
inline QString &activeBackendStorage() {
    static QString name;
    return name;
}
inline QString activeBackend() {
    return activeBackendStorage();
}
inline void setActiveBackend(QString name) {
    activeBackendStorage() = std::move(name);
}
/**
 * 本次启动枚举到的显卡数量（Linux 由 /sys/class/drm 得出，其余平台保持 0）。
 * 设置页在 RhineCore 里，既不链接 QtQuick 也读不到 sysfs 解析器，只能读这份启动快照：
 * 它决定「显卡选择」这一行出不出来（只有一块卡时没有可选项，整行不显示）。
 */
inline int &gpuDeviceCountStorage() {
    static int count = 0;
    return count;
}
inline int gpuDeviceCount() {
    return gpuDeviceCountStorage();
}
inline void setGpuDeviceCount(int count) {
    gpuDeviceCountStorage() = count;
}
/**
 * 本次启动真正生效的显卡序号（0 基）；-1 表示没有注入任何选择变量、走驱动默认。
 * 保存的序号失效时它保持 -1，设置页据此显示「默认（序号 N 不存在）」，
 * 让回退这件事在界面上是可见的，而不是悄悄换了一张卡。
 */
inline int &gpuAppliedIndexStorage() {
    static int index = -1;
    return index;
}
inline int gpuAppliedIndex() {
    return gpuAppliedIndexStorage();
}
inline void setGpuAppliedIndex(int index) {
    gpuAppliedIndexStorage() = index;
}
} // namespace RhineGraphics
