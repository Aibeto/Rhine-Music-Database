#pragma once

#include <QPointer>
#include <QWindow>

QT_BEGIN_NAMESPACE
class QQuickWindow;
QT_END_NAMESPACE

namespace RhineRender {

/** 将可见宿主的输入法焦点接到离屏 UI，保持平台查询与 Qt Quick 输入项一致。 */
class PresentationWindow : public QWindow {
    Q_OBJECT

  public:
    explicit PresentationWindow(QWindow *parent = nullptr);

    QObject *focusObject() const override;

    // 离屏窗口不拥有原生焦点；绑定后由可见窗口发布其焦点变化，销毁前先解除绑定。
    void setInputWindow(QQuickWindow *window);

  protected:
    bool event(QEvent *event) override;

  private:
    void syncInputFocus();
    void refreshInputMethod();

    QPointer<QQuickWindow> m_inputWindow;
    QPointer<QObject> m_focusObject;
};

} // namespace RhineRender
