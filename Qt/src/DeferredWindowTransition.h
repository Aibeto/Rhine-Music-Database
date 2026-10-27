#pragma once

#include <QObject>
#include <functional>

/**
 * 在当前输入/QML 回调退出后执行窗口内容迁移。
 * 使用 DeferredDelete 的事件循环层级规则，避免嵌套事件循环提前销毁仍在执行的 QML 对象。
 */
class DeferredWindowTransition final : public QObject {
    Q_OBJECT
  public:
    explicit DeferredWindowTransition(std::function<void()> transition, QObject *parent = nullptr);
    Q_INVOKABLE void request();
    void cancel();

  private:
    std::function<void()> m_transition;
    QObject *m_boundary = nullptr;
    bool m_requested = false;
};
