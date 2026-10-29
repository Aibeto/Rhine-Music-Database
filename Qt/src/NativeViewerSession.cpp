#include "NativeViewer.h"

// 仅保存逻辑连续性：不复制几何、图片、绘制缓冲或任何图形设备对象。
struct NativeViewer::ResumeState {
    Spherical pose;
    Spherical goal;
    Spherical resetFrom;
    Vector eye;
    Vector focus;
    Vector target;
    Vector resetFocus;
    SceneMotion::Spring clarity;
    SceneMotion::Spring spread;
    double theme;
    double time;
    double last;
    double pendingUntil;
    double closeAt;
    double resetElapsed;
    double targetClarity;
    double targetSpread;
    bool open;
    bool closing;
    bool resetting;
    QString transition;
    QString surface;
};

/** 捕获挂起前的相机、动画与浏览位置；恢复时取消尚未结束的指针手势。 */
std::shared_ptr<NativeViewer::ResumeState> NativeViewer::captureResumeState() const {
    auto state = std::make_shared<ResumeState>();
    state->pose = m_pose;
    state->goal = m_goal;
    state->resetFrom = m_resetFrom;
    state->eye = m_eye;
    state->focus = m_focus;
    state->target = m_target;
    state->resetFocus = m_resetFocus;
    state->clarity = m_clarity;
    state->spread = m_spread;
    state->theme = m_theme;
    state->time = m_time;
    state->last = m_last;
    state->pendingUntil = m_pendingUntil;
    state->closeAt = m_closeAt;
    state->resetElapsed = m_resetElapsed;
    state->targetClarity = m_targetClarity;
    state->targetSpread = m_targetSpread;
    state->open = m_open;
    state->closing = m_closing;
    state->resetting = m_resetting;
    state->transition = m_transition;
    state->surface = m_surface;
    return state;
}

/** 把纯值状态应用到新实例；模型与渲染资源仍由正常初始化路径重新建立。 */
void NativeViewer::restoreResumeState(const ResumeState &state) {
    if (state.open) {
        open(state.time);
    }
    m_pose = state.pose;
    m_goal = state.goal;
    m_resetFrom = state.resetFrom;
    m_eye = state.eye;
    m_focus = state.focus;
    m_target = state.target;
    m_resetFocus = state.resetFocus;
    m_clarity = state.clarity;
    m_spread = state.spread;
    m_theme = state.theme;
    m_time = state.time;
    m_last = state.last;
    m_pendingUntil = state.pendingUntil;
    m_closeAt = state.closeAt;
    m_resetElapsed = state.resetElapsed;
    m_targetClarity = state.targetClarity;
    m_targetSpread = state.targetSpread;
    m_open = state.open;
    m_closing = state.closing;
    m_resetting = state.resetting;
    m_transition = state.transition;
    m_surface = state.surface;
    m_dragging = false;
}
