#include "DesktopVisibility.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace {
bool isDesktopWindow(HWND window) {
    wchar_t name[256]{};
    GetClassNameW(window, name, 256);
    const auto windowClass = QString::fromWCharArray(name);
    return window == GetShellWindow() || windowClass == "WorkerW" || windowClass == "Progman" ||
           windowClass == "Shell_TrayWnd" || windowClass == "Shell_SecondaryTrayWnd";
}
bool isCloaked(HWND window) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}
QRectF windowBounds(HWND window) {
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        GetWindowRect(window, &bounds);
    }
    return QRectF(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
}

/** 只选择面板的实际顶层窗口；Qt 的隐藏辅助 HWND 不能截断叠放顺序查询。 */
HWND interfaceWindow(qint64 processId) {
    HWND result = nullptr;
    double largestArea = 0;
    for (auto candidate = GetTopWindow(nullptr); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(candidate, &ownerPid);
        if (ownerPid != DWORD(processId) || !IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER) ||
            isDesktopWindow(candidate)) {
            continue;
        }
        const auto bounds = windowBounds(candidate);
        const auto area = bounds.width() * bounds.height();
        if (bounds.width() >= 480 && bounds.height() >= 270 && area > largestArea) {
            result = candidate;
            largestArea = area;
        }
    }
    return result;
}
}

/** 以面板 HWND 的原生显示器为准，避免 Qt 名称和逻辑像素与 Win32 物理坐标混用。 */
bool platformDesktopCovered(const QString &screenName, qint64 interfacePid, const QRect &) {
    const auto panel = interfaceWindow(interfacePid);
    if (!panel || IsIconic(panel) || isCloaked(panel)) {
        return false;
    }
    const auto panelMonitor = MonitorFromWindow(panel, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(panelMonitor, &information)) {
        return false;
    }
    Q_UNUSED(screenName);
    const auto rectangle = [](const RECT &rect) {
        return QRectF(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    };
    const auto panelArea = windowBounds(panel).intersected(rectangle(information.rcWork));
    for (auto candidate = GetTopWindow(nullptr); candidate; candidate = GetWindow(candidate, GW_HWNDNEXT)) {
        if (candidate == panel) {
            break;
        }
        if (!IsWindowVisible(candidate) || IsIconic(candidate) || isDesktopWindow(candidate) ||
            isCloaked(candidate) || MonitorFromWindow(candidate, MONITOR_DEFAULTTONEAREST) != panelMonitor) {
            continue;
        }
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(candidate, &ownerPid);
        if (ownerPid == DWORD(interfacePid) || ownerPid == GetCurrentProcessId()) {
            continue;
        }
        const auto bounds = windowBounds(candidate);
        const bool maximized =
            IsZoomed(candidate) || desktopWindowCoversScreen(bounds, rectangle(information.rcMonitor),
                                                             rectangle(information.rcWork));
        if (maximized && panelArea.isValid() && bounds.contains(panelArea.adjusted(2, 2, -2, -2))) {
            return true;
        }
    }
    return false;
}

qint64 platformDesktopForegroundPid() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid;
}
