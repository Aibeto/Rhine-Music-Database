#include "WindowsStaticImage.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QImageReader>
#include <QWindow>
#include <algorithm>

WindowsStaticImage::WindowsStaticImage(QWindow *window) : m_window(window) {
}

WindowsStaticImage::~WindowsStaticImage() {
    clear();
}

/** 先解码确认图片有效，再启用原生绘制；失败时不改变现有窗口内容或设备。 */
bool WindowsStaticImage::setImage(const QByteArray &source) {
    if (!m_window) {
        return false;
    }
    QBuffer buffer;
    buffer.setData(source);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }
    QImageReader reader(&buffer, "png");
    const auto size = reader.size();
    if (!size.isValid()) {
        return false;
    }
    reader.setScaledSize(size.scaled(QSize(1280, 1280), Qt::KeepAspectRatio));
    auto image = reader.read().convertToFormat(QImage::Format_RGB32);
    if (image.isNull()) {
        return false;
    }
    clear();
    m_image = std::move(image);
    m_handle = reinterpret_cast<HWND>(m_window->winId());
    QCoreApplication::instance()->installNativeEventFilter(this);
    return true;
}

void WindowsStaticImage::clear() {
    if (m_handle) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    m_handle = nullptr;
    m_image = {};
}

void WindowsStaticImage::repaint() {
    if (m_handle) {
        InvalidateRect(m_handle, nullptr, FALSE);
    }
}

/** 使用物理客户区坐标居中裁切，匹配 macOS 静态页的等比填充；不改变窗口句柄或焦点。 */
void WindowsStaticImage::paint(HDC context) {
    RECT client{};
    if (!context || !GetClientRect(m_handle, &client) || m_image.isNull()) {
        return;
    }
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const double scale = std::max(double(width) / m_image.width(), double(height) / m_image.height());
    const auto targetWidth = qRound(m_image.width() * scale);
    const auto targetHeight = qRound(m_image.height() * scale);
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = m_image.width();
    information.bmiHeader.biHeight = -m_image.height();
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    const auto savedState = SaveDC(context);
    SetStretchBltMode(context, HALFTONE);
    SetBrushOrgEx(context, 0, 0, nullptr);
    StretchDIBits(context, (width - targetWidth) / 2, (height - targetHeight) / 2, targetWidth, targetHeight,
                  0, 0, m_image.width(), m_image.height(), m_image.constBits(), &information, DIB_RGB_COLORS,
                  SRCCOPY);
    if (savedState) {
        RestoreDC(context, savedState);
    }
}

bool WindowsStaticImage::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    const auto event = static_cast<MSG *>(message);
    if (!m_handle || event->hwnd != m_handle) {
        return false;
    }
    if (event->message == WM_SIZE) {
        repaint();
        return false;
    }
    if (event->message == WM_PAINT) {
        // 不调用 BeginPaint/EndPaint，也不消费消息：更新区域必须留给 Qt，
        // Qt 的 WM_PAINT 处理负责发出 Expose，最小化和隐藏恢复都依赖这一环。
        const auto context = GetDC(m_handle);
        paint(context);
        if (context) {
            ReleaseDC(m_handle, context);
        }
        return false;
    } else if (event->message == WM_PRINTCLIENT) {
        paint(reinterpret_cast<HDC>(event->wParam));
    } else if (event->message != WM_ERASEBKGND) {
        return false;
    }
    if (result) {
        *result = event->message == WM_ERASEBKGND ? 1 : 0;
    }
    return true;
}
