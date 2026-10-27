#include "PresentationWindow.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QQuickItem>
#include <QQuickWindow>

namespace RhineRender {

PresentationWindow::PresentationWindow(QWindow *parent) : QWindow(parent) {
    // FocusIn 发生时 Qt 尚未完成平台焦点切换；等正式通知后再更新输入法状态。
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *window) {
        if (window == this) {
            refreshInputMethod();
        }
    });
}

QObject *PresentationWindow::focusObject() const {
    if (m_inputWindow) {
        if (QQuickItem *item = m_inputWindow->activeFocusItem()) {
            return item;
        }
    }
    return QWindow::focusObject();
}

/** 发布离屏输入项的焦点；平台输入上下文只监听当前可见焦点窗口的通知。 */
void PresentationWindow::setInputWindow(QQuickWindow *window) {
    if (m_inputWindow == window) {
        syncInputFocus();
        return;
    }
    if (m_inputWindow) {
        disconnect(m_inputWindow, nullptr, this, nullptr);
    }
    m_inputWindow = window;
    if (m_inputWindow) {
        connect(m_inputWindow, &QQuickWindow::focusObjectChanged, this, &PresentationWindow::syncInputFocus);
        connect(m_inputWindow, &QObject::destroyed, this, &PresentationWindow::syncInputFocus);
    }
    syncInputFocus();
}

void PresentationWindow::syncInputFocus() {
    QObject *object = focusObject();
    if (object != m_focusObject) {
        m_focusObject = object;
        emit focusObjectChanged(object);
    }
    refreshInputMethod();
}

/** 初始化绑定及重新激活时的坐标；后续位置、祖先变换和光标变化由 Qt Quick 正常更新。 */
void PresentationWindow::refreshInputMethod() {
    // 原生文件对话框等窗口激活时不能覆盖它们的输入项变换。
    if (QGuiApplication::focusWindow() != this) {
        return;
    }
    QInputMethod *input = QGuiApplication::inputMethod();
    auto *item = qobject_cast<QQuickItem *>(focusObject());
    if (item && m_inputWindow) {
        input->setInputItemTransform(item->itemTransform(m_inputWindow->contentItem(), nullptr));
        input->setInputItemRectangle(QRectF(0, 0, item->width(), item->height()));
    } else {
        input->setInputItemTransform(QTransform());
        input->setInputItemRectangle(QRectF());
    }
    input->update(Qt::ImQueryAll);
}

bool PresentationWindow::event(QEvent *event) {
    if (m_inputWindow && (event->type() == QEvent::InputMethod || event->type() == QEvent::InputMethodQuery ||
                          event->type() == QEvent::FocusAboutToChange)) {
        // 平台通常直接发给 focusObject；兼容发给宿主窗口的路径也只交付一次。
        // 必须传回 Qt Quick 的处理结果，否则 Windows 可能继续走默认字符处理而重复上屏。
        // 焦点即将切换时也沿用 Qt Quick 的提交预编辑流程。
        return QCoreApplication::sendEvent(m_inputWindow, event);
    }
    return QWindow::event(event);
}

} // namespace RhineRender
