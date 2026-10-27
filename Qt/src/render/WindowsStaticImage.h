#pragma once

#include <QAbstractNativeEventFilter>
#include <QImage>
#include <QObject>
#include <QPointer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class QByteArray;
class QWindow;

/** 在原 HWND 上按需绘制静态图片；不持有 Qt Quick 场景、交换链或图形设备。 */
class WindowsStaticImage final : public QObject, public QAbstractNativeEventFilter {
  public:
    explicit WindowsStaticImage(QWindow *window);
    ~WindowsStaticImage() override;
    bool setImage(const QByteArray &source);
    void clear();
    void repaint();
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

  private:
    void paint(HDC context);
    QPointer<QWindow> m_window;
    HWND m_handle = nullptr;
    QImage m_image;
};
