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
} // namespace RhineGraphics
