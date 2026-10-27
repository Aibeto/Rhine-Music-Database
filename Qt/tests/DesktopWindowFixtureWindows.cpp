#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM parameter, LPARAM detail) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, parameter, detail);
}
}

/** 独立 Win32 窗口夹具；覆盖判断必须经过真实 HWND、DWM 边界和显示器。 */
int main(int count, char **arguments) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"RhineDesktopFixture";
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);
    const char *mode = count > 1 ? arguments[1] : "normal";
    const bool fullscreen = std::strcmp(mode, "fullscreen") == 0;
    const auto style = fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    const auto window = CreateWindowW(windowClass.lpszClassName, L"Rhine coverage fixture", style, 160, 100,
                                      800, 600, nullptr, nullptr, instance, nullptr);
    if (!window) {
        return 1;
    }
    if (fullscreen) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto bounds = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_SHOWWINDOW);
    } else {
        const bool maximized = std::strncmp(mode, "maximized", 9) == 0;
        ShowWindow(window, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    }
    SetForegroundWindow(window);
    if (std::strcmp(mode, "maximized-with-normal") == 0) {
        const auto normal =
            CreateWindowW(windowClass.lpszClassName, L"Normal foreground fixture", WS_OVERLAPPEDWINDOW, 250,
                          180, 640, 400, nullptr, nullptr, instance, nullptr);
        ShowWindow(normal, SW_SHOWNORMAL);
        SetForegroundWindow(normal);
    }
    std::puts("COVERAGE_FIXTURE_READY");
    std::fflush(stdout);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
