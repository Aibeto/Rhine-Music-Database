#include "DeferredWindowTransition.h"
#include <utility>

DeferredWindowTransition::DeferredWindowTransition(std::function<void()> transition, QObject *parent)
    : QObject(parent), m_transition(std::move(transition)) {
}

/** 合并本轮输入触发的重复请求；实际目标状态由窗口生命周期控制器保存。 */
void DeferredWindowTransition::request() {
    m_requested = true;
    if (m_boundary) {
        return;
    }
    m_boundary = new QObject(this);
    connect(m_boundary, &QObject::destroyed, this, [this] {
        m_boundary = nullptr;
        if (!m_requested) {
            return;
        }
        m_requested = false;
        m_transition();
    });
    // 普通零延迟定时器仍可能在嵌套事件循环中执行；DeferredDelete 会等待原回调栈退出。
    m_boundary->deleteLater();
}

void DeferredWindowTransition::cancel() {
    m_requested = false;
}
