#include "WindowsTaskbar.h"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QWindow>
#include <functional>

namespace {
bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return ready();
}
}

/** 在交互桌面验证真实 Explorer 通知与 COM 注册，不创建 Vulkan 设备或访问音乐账号。 */
bool runWindowsTaskbarChecks(Engine &engine) {
    QGuiApplication::setQuitOnLastWindowClosed(false);
    bool passed = true;
    const auto check = [&](bool value, const char *message) {
        qInfo() << (value ? "PASS:" : "FAIL:") << message;
        passed = passed && value;
    };
    WindowsTaskbar taskbar(&engine);
    QWindow window;
    window.setTitle(QStringLiteral("Rhine taskbar regression"));
    window.resize(480, 320);
    window.show();
    check(waitFor([&] { return taskbar.m_readyWindows.contains(quintptr(window.winId())); }),
          "readiness arrives before window binding");
    taskbar.bindWindow(&window);
    check(waitFor([&] { return taskbar.m_taskbar && taskbar.m_buttonsAdded; }),
          "late binding registers real thumbnail buttons");
    const auto handle = window.winId();
    window.hide();
    window.show();
    check(waitFor([&] { return window.isExposed(); }), "window restores after hiding");
    check(window.winId() == handle && taskbar.m_buttonsAdded,
          "restoration preserves the handle and registered buttons");
    window.destroy();
    check(!window.handle() && !taskbar.m_handle && !taskbar.m_buttonsAdded,
          "native destruction clears registration without recreating a window");
    window.show();
    check(waitFor([&] {
              return taskbar.m_handle == reinterpret_cast<HWND>(window.winId()) && taskbar.m_taskbar &&
                     taskbar.m_buttonsAdded;
          }),
          "new native handle registers thumbnail buttons");
    taskbar.bindWindow(nullptr);
    window.hide();
    return passed;
}
