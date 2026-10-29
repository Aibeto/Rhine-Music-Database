#pragma once
#include <QRectF>
#include <QString>

// 原生窗口边框和系统取整允许两个逻辑像素误差，不把普通失焦当作覆盖。
inline bool desktopWindowCoversScreen(const QRectF &window, const QRectF &screen, const QRectF &work) {
    const auto covers = [&window](const QRectF &area) {
        return area.isValid() && window.contains(area.adjusted(2, 2, -2, -2));
    };
    return window.isValid() && (covers(screen) || covers(work));
}

// macOS Stage Manager 保留窗口条，Dock 动画也会短暂改变可用高度。
inline bool desktopWindowFillsManagedArea(const QRectF &window, const QRectF &work, bool hasWindowStrip) {
    constexpr double EDGE_TOLERANCE = 12;
    constexpr double MINIMUM_MANAGED_WIDTH = .75;
    const bool fillsHeight =
        window.top() <= work.top() + EDGE_TOLERANCE && window.bottom() >= work.bottom() - EDGE_TOLERANCE;
    const bool touchesSide =
        window.left() <= work.left() + EDGE_TOLERANCE || window.right() >= work.right() - EDGE_TOLERANCE;
    const auto minimumWidth =
        hasWindowStrip ? work.width() * MINIMUM_MANAGED_WIDTH : work.width() - 2 * EDGE_TOLERANCE;
    const auto overlap = window.intersected(work);
    return window.isValid() && work.isValid() && fillsHeight && touchesSide &&
           overlap.width() >= minimumWidth;
}

bool platformDesktopCovered(const QString &screenName, qint64 interfacePid, const QRect &screenGeometry = {});
qint64 platformDesktopForegroundPid();
