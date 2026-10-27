#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QImage>
#include <QPointer>
#include <QSet>
#include <array>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shobjidl.h>

class Engine;
class PlaybackConnection;
class DesktopPlaybackControls;
class QQuickWindow;
class QWindow;

// Windows 任务栏控制
class WindowsTaskbar : public QObject, public QAbstractNativeEventFilter {
  public:
    WindowsTaskbar(Engine *engine, QWindow *window = nullptr, PlaybackConnection *connection = nullptr);
    ~WindowsTaskbar() override;
    void bindWindow(QWindow *window);
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    friend struct WindowsTaskbarProbe;
    void bindEngine(Engine *engine);
    void resetToolbar();
    void initializeToolbar();
    void updateButtons();
    void configurePreview();
    void updatePreview();
    QImage previewImage(QSize bounds) const;
    bool sendPreview(QSize bounds, bool live);
    std::unique_ptr<DesktopPlaybackControls> m_controls;
    QPointer<Engine> m_engine;
    std::array<QMetaObject::Connection, 3> m_engineConnections;
    QPointer<QWindow> m_window;
    HWND m_handle = nullptr;
    ITaskbarList3 *m_taskbar = nullptr;
    UINT m_createdMessage = 0;
    QSet<quintptr> m_readyWindows;
    bool m_comInitialized = false;
    bool m_buttonsAdded = false;
    int m_lastState = -1;
    QString m_coverKey;
    QString m_trackKey;
    QImage m_cover;
    std::array<HICON, 6> m_icons{};
};
