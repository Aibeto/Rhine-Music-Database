#include "WindowsTaskbar.h"
#include "Engine.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QTimer>
#include <cstring>
#include <iterator>
#include <algorithm>
#include <dwmapi.h>

namespace {
constexpr UINT FIRST_BUTTON_ID = 0x5200;
enum Icon { Previous, Play, Pause, Next, Like, Liked };

// 创建 Windows 图标与位图
HBITMAP createBitmap(const QImage &source) {
    const auto image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width();
    info.bmiHeader.biHeight = -image.height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap && pixels) {
        std::memcpy(pixels, image.constBits(), image.sizeInBytes());
    }
    return bitmap;
}

// Windows 菜单栏控制配置
HICON createButtonIcon(int kind) {
    constexpr int SIZE = 32;
    QImage image(SIZE, SIZE, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    if (kind == Like || kind == Liked) {
        path.moveTo(16, 27);
        path.cubicTo(12, 23, 3, 17, 3, 10);
        path.cubicTo(3, 2, 12, 2, 16, 8);
        path.cubicTo(20, 2, 29, 2, 29, 10);
        path.cubicTo(29, 17, 20, 23, 16, 27);
    } else if (kind == Pause) {
        path.addRect(8, 6, 5, 20);
        path.addRect(19, 6, 5, 20);
    } else {
        if (kind == Previous) {
            painter.translate(SIZE, 0);
            painter.scale(-1, 1);
        }
        path.moveTo(8, 6);
        path.lineTo(23, 16);
        path.lineTo(8, 26);
        path.closeSubpath();
        if (kind != Play) {
            path.addRect(24, 6, 4, 20);
        }
    }
    painter.setPen(QPen(QColor(30, 30, 30), 2));
    painter.setBrush(kind == Liked ? QColor(238, 75, 100) : QColor(245, 245, 245));
    painter.drawPath(path);
    if (kind == Like) {
        painter.setPen(QPen(QColor(30, 30, 30), 2));
        painter.setBrush(QColor(100, 100, 100));
        painter.drawPath(path);
    }
    painter.end();

    const auto bitmap = createBitmap(image);
    const std::array<BYTE, SIZE * SIZE / 8> maskPixels{};
    const auto mask = CreateBitmap(SIZE, SIZE, 1, 1, maskPixels.data());
    HICON icon = nullptr;
    if (bitmap && mask) {
        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmColor = bitmap;
        info.hbmMask = mask;
        icon = CreateIconIndirect(&info);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    if (mask) {
        DeleteObject(mask);
    }
    return icon;
}
}

WindowsTaskbar::WindowsTaskbar(Engine *engine, QQuickWindow *window)
    : QObject(window), m_engine(engine), m_window(window) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        qWarning() << "Taskbar COM initialization failed:" << initialized;
        return;
    }
    m_createdMessage = RegisterWindowMessageW(L"TaskbarButtonCreated");
    if (!m_createdMessage) {
        return;
    }
    for (int i = 0; i < int(m_icons.size()); ++i) {
        m_icons[i] = createButtonIcon(i);
    }
    QCoreApplication::instance()->installNativeEventFilter(this);
    m_handle = reinterpret_cast<HWND>(window->winId());
    configurePreview();
    connect(&engine->app->music.player, &MusicPlayer::changed, this, &WindowsTaskbar::updateButtons);
    connect(&engine->app->music.player, &MusicPlayer::changed, this, &WindowsTaskbar::updatePreview);
    connect(&engine->app->music, &MusicLibrary::invalidated, this, &WindowsTaskbar::updateButtons);
    updatePreview();
}

// 任务栏预览配置
void WindowsTaskbar::configurePreview() {
    const BOOL enabled = TRUE;
    DwmSetWindowAttribute(m_handle, DWMWA_FORCE_ICONIC_REPRESENTATION, &enabled, sizeof(enabled));
    DwmSetWindowAttribute(m_handle, DWMWA_HAS_ICONIC_BITMAP, &enabled, sizeof(enabled));
    DwmInvalidateIconicBitmaps(m_handle);
}

void WindowsTaskbar::updatePreview() {
    const auto &player = m_engine->app->music.player;
    const auto trackKey = MusicPlayer::identity(player.current);
    const auto coverKey = player.coverImage;
    if (trackKey == m_trackKey && coverKey == m_coverKey) {
        return;
    }
    m_trackKey = trackKey;
    m_coverKey = coverKey;
    m_cover = coverKey.isEmpty() ? QImage() : m_engine->host.image(coverKey);
    DwmInvalidateIconicBitmaps(m_handle);
}

QImage WindowsTaskbar::previewImage(QSize bounds) const {
    const int side = std::clamp(std::min(bounds.width(), bounds.height()), 1, 1024);
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(30, 30, 30));
    QPainter painter(&image);
    const auto cover = m_cover.isNull() ? QGuiApplication::windowIcon().pixmap(256, 256).toImage() : m_cover;
    if (!cover.isNull()) {
        const auto size = cover.size().scaled(image.size(), Qt::KeepAspectRatio);
        const QRect target((side - size.width()) / 2, (side - size.height()) / 2, size.width(),
                           size.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, cover);
    }
    return image;
}

bool WindowsTaskbar::sendPreview(QSize bounds, bool live) {
    if (bounds.isEmpty()) {
        return false;
    }
    const auto bitmap = createBitmap(previewImage(bounds));
    if (!bitmap) {
        return false;
    }
    const auto result = live ? DwmSetIconicLivePreviewBitmap(m_handle, bitmap, nullptr, 0)
                             : DwmSetIconicThumbnail(m_handle, bitmap, 0);
    DeleteObject(bitmap);
    return SUCCEEDED(result);
}

// 移除消息入口，释放图标与 COM 引用
WindowsTaskbar::~WindowsTaskbar() {
    QCoreApplication::instance()->removeNativeEventFilter(this);
    if (m_taskbar) {
        m_taskbar->Release();
    }
    for (const auto icon : m_icons) {
        if (icon) {
            DestroyIcon(icon);
        }
    }
    if (m_comInitialized) {
        CoUninitialize();
    }
}

void WindowsTaskbar::updateButtons() {
    if (!m_taskbar || !m_window) {
        return;
    }
    const auto &music = m_engine->app->music;
    const auto &player = music.player;
    const auto track = player.current;
    const bool hasTrack = !track.isEmpty();
    const bool playing = player.state["playing"].toBool();
    const bool canPlay = hasTrack && !player.loading;
    const bool canSkip = hasTrack && !player.queue.isEmpty();
    const bool canLike = hasTrack && track["kind"] == "song" && music.editable() && !music.uid.isEmpty() &&
                         !music.writeLoading && music.canMutate("/like");
    const bool liked = (music.likedIDs && MusicData::indexOf(*music.likedIDs, track["id"]) >= 0) ||
                       MusicData::indexOfId(music.lists[1], track["id"].toString()) >= 0 ||
                       music.subscriptionStates.value(track["key"].toString()) == "subscribed";
    const int state =
        int(playing) | (int(canPlay) << 1) | (int(canSkip) << 2) | (int(canLike) << 3) | (int(liked) << 4);
    if (m_buttonsAdded && state == m_lastState) {
        return;
    }
    const std::array<int, 4> icons{Previous, playing ? Pause : Play, Next, liked ? Liked : Like};
    const std::array<const wchar_t *, 4> tips{L"上一首", playing ? L"暂停" : L"播放", L"下一首",
                                              liked ? L"取消喜欢" : L"喜欢"};
    const std::array<bool, 4> enabled{canSkip, canPlay, canSkip, canLike};
    std::array<THUMBBUTTON, 4> buttons{};
    for (int i = 0; i < int(buttons.size()); ++i) {
        auto &button = buttons[i];
        button.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        button.iId = FIRST_BUTTON_ID + i;
        button.hIcon = m_icons[icons[i]];
        lstrcpynW(button.szTip, tips[i], int(std::size(button.szTip)));
        button.dwFlags = enabled[i] ? THBF_ENABLED : THBF_DISABLED;
    }
    const auto result = m_buttonsAdded
                            ? m_taskbar->ThumbBarUpdateButtons(m_handle, UINT(buttons.size()), buttons.data())
                            : m_taskbar->ThumbBarAddButtons(m_handle, UINT(buttons.size()), buttons.data());
    if (SUCCEEDED(result)) {
        m_buttonsAdded = true;
        m_lastState = state;
    } else {
        qWarning() << "Taskbar thumbnail toolbar update failed:" << result;
    }
}

bool WindowsTaskbar::nativeEventFilter(const QByteArray &, void *message, qintptr *result) {
    const auto event = static_cast<MSG *>(message);
    if (!m_window || event->hwnd != m_handle) {
        return false;
    }
    if (event->message == WM_DWMSENDICONICTHUMBNAIL || event->message == WM_DWMSENDICONICLIVEPREVIEWBITMAP) {
        const bool live = event->message == WM_DWMSENDICONICLIVEPREVIEWBITMAP;
        const QSize bounds = live ? m_window->size() : QSize(HIWORD(event->lParam), LOWORD(event->lParam));
        sendPreview(bounds, live);
        if (result) {
            *result = 0;
        }
        return true;
    }
    if (event->message == WM_DWMCOMPOSITIONCHANGED) {
        configurePreview();
    }
    if (event->message == m_createdMessage) {
        configurePreview();
        if (m_taskbar) {
            m_taskbar->Release();
            m_taskbar = nullptr;
        }
        m_buttonsAdded = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&m_taskbar))) &&
            SUCCEEDED(m_taskbar->HrInit())) {
            updateButtons();
        }
    } else if (event->message == WM_COMMAND && HIWORD(event->wParam) == THBN_CLICKED) {
        const auto id = LOWORD(event->wParam);
        if (id < FIRST_BUTTON_ID || id >= FIRST_BUTTON_ID + 4 || !m_buttonsAdded) {
            return false;
        }
        // 离开原生消息栈后再改播放状态，按钮不激活窗口，也不依赖 HUD 当前选中的歌曲。
        QTimer::singleShot(0, this, [this, id] {
            updateButtons();
            int enabledBit = 2;
            if (id == FIRST_BUTTON_ID + 1) {
                enabledBit = 1;
            } else if (id == FIRST_BUTTON_ID + 3) {
                enabledBit = 3;
            }
            if (!(m_lastState & (1 << enabledBit))) {
                return;
            }
            auto &music = m_engine->app->music;
            if (id == FIRST_BUTTON_ID + 3) {
                if (!music.player.current.isEmpty() && music.editable() && !music.uid.isEmpty() &&
                    !music.writeLoading && music.canMutate("/like")) {
                    music.like(music.player.current);
                    m_engine->wake();
                }
            } else {
                static const std::array<QString, 3> actions{"player:prev", "player:toggle", "player:next"};
                m_engine->action(actions[id - FIRST_BUTTON_ID]);
            }
            updateButtons();
        });
        if (result) {
            *result = 0;
        }
        return true;
    }
    return false;
}
