#include "WindowsTaskbar.h"
#include "render/WindowsStaticImage.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QWindow>
#include <cstdio>
#include <functional>

// 读取真正的 COM 注册结果，不给产品接口增加测试状态或模拟任务栏实现。
struct WindowsTaskbarProbe {
    static bool ready(const WindowsTaskbar &taskbar, WId handle) {
        return taskbar.m_readyWindows.contains(quintptr(handle));
    }
    static bool registered(const WindowsTaskbar &taskbar) {
        return taskbar.m_taskbar && taskbar.m_buttonsAdded;
    }
};

namespace {
bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return ready();
}
}

/**
 * 仅验证 Windows 原生窗口、GDI 静态页及任务栏的消息边界。
 * 必须在交互桌面执行；入口在图形后端选择之前返回，不创建 QQuickWindow、QRhi 或 Vulkan。
 */
int runWindowsWindowChecks(int argc, char **argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        std::fflush(stderr);
    });
    QGuiApplication application(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    bool passed = true;
    const auto check = [&](bool value, const char *message) {
        std::fprintf(stderr, "%s: %s\n", value ? "PASS" : "FAIL", message);
        passed = passed && value;
    };
    // 先安装监听器，再创建窗口；故意延后绑定，覆盖系统就绪通知已到达的情况。
    WindowsTaskbar taskbar(nullptr);
    QWindow window;
    window.setSurfaceType(QSurface::RasterSurface);
    window.setTitle(QStringLiteral("Rhine native window regression - no Vulkan"));
    window.resize(480, 320);
    window.show();
    const auto handle = window.winId();
    check(waitFor([&] { return window.isExposed(); }), "initial native window is exposed");
    check(waitFor([&] { return WindowsTaskbarProbe::ready(taskbar, handle); }),
          "taskbar readiness is retained before binding");
    taskbar.bindWindow(&window);
    check(waitFor([&] { return WindowsTaskbarProbe::registered(taskbar); }),
          "real thumbnail buttons register after late binding");

    QImage pixels(64, 64, QImage::Format_RGB32);
    pixels.fill(QColor(40, 75, 90));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    check(pixels.save(&buffer, "PNG"), "static image fixture is valid");
    WindowsStaticImage image(&window);
    for (const auto mode : {0, 1, 2}) {
        check(image.setImage(png), "same native window accepts static content");
        if (mode == 1) {
            window.hide();
        } else if (mode == 2) {
            window.setWindowStates(window.windowStates() | Qt::WindowMinimized);
        }
        if (mode != 0) {
            check(waitFor([&] { return !window.isExposed(); }), "hidden or minimized window is unexposed");
        }
        window.setWindowStates(window.windowStates() & ~Qt::WindowMinimized);
        window.setVisible(true);
        image.repaint();
        check(waitFor([&] { return window.isVisible() && window.isExposed(); }),
              "static paint preserves Qt exposure during every restoration entry");
        check(window.winId() == handle, "window identity is unchanged");
        check(waitFor([&] { return WindowsTaskbarProbe::registered(taskbar); }),
              "thumbnail buttons remain registered after native restoration");
        const auto context = GetDC(reinterpret_cast<HWND>(handle));
        const auto color = GetPixel(context, 80, 80);
        ReleaseDC(reinterpret_cast<HWND>(handle), context);
        check(color == RGB(40, 75, 90), "static content is painted while Qt receives WM_PAINT");
        image.clear();
    }
    taskbar.bindWindow(nullptr);
    window.hide();
    std::fprintf(stderr, "Windows native window checks: %s (no Vulkan initialized)\n",
                 passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
