#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QImage>
#include <QPointer>
#include <array>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shobjidl.h>

class Engine;
class QQuickWindow;

// Windows 任务栏控制
class WindowsTaskbar : public QObject, public QAbstractNativeEventFilter {
  public:
    WindowsTaskbar(Engine *engine, QQuickWindow *window);
    ~WindowsTaskbar() override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

  private:
    friend bool runWindowsTaskbarChecks(Engine &engine, QQuickWindow *window);
    void updateButtons();
    void configurePreview();
    void updatePreview();
    QImage previewImage(QSize bounds) const;
    bool sendPreview(QSize bounds, bool live);
    Engine *m_engine;
    QPointer<QQuickWindow> m_window;
    HWND m_handle = nullptr;
    ITaskbarList3 *m_taskbar = nullptr;
    UINT m_createdMessage = 0;
    bool m_comInitialized = false;
    bool m_buttonsAdded = false;
    int m_lastState = -1;
    QString m_coverKey;
    QString m_trackKey;
    QImage m_cover;
    std::array<HICON, 6> m_icons{};
};
