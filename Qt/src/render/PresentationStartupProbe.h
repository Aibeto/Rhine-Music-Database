#pragma once

#include <QtGlobal>

namespace RhineRender {

/** 只给可呈现的前台窗口计时；启动与后台等待不能消耗首帧自检预算。 */
class PresentationStartupProbe {
  public:
    enum class Result { Waiting, Running, Passed, Failed };

    void reset() {
        m_readySinceMs = -1;
    }

    Result update(qint64 nowMs, bool ready, quint64 presentedFrames, bool failed) {
        if (failed) {
            return Result::Failed;
        }
        if (presentedFrames > 0) {
            return Result::Passed;
        }
        if (!ready) {
            reset();
            return Result::Waiting;
        }
        if (m_readySinceMs < 0) {
            m_readySinceMs = nowMs;
        }
        return nowMs - m_readySinceMs >= kTimeoutMs ? Result::Failed : Result::Running;
    }

  private:
    static constexpr qint64 kTimeoutMs = 3000;
    qint64 m_readySinceMs = -1;
};

}
