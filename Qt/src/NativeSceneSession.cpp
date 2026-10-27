#include "NativeScene.h"
#include "NativeViewer.h"

// 仅保存逻辑连续性：不复制几何、图片、绘制缓冲或任何图形设备对象。
struct NativeScene::ResumeState {
    Actor actor;
    QVector<Actor> returning;
    std::array<int, 5> memories;
    SceneMotion::ThemeWave theme;
    SceneMotion::Decryption decryption;
    SceneMotion::Spring rail;
    SceneMotion::Spring shoulder;
    SceneMotion::Spring laneFocus;
    SceneMotion::Spring columnCamera;
    Cell origin;
    Vector eye;
    Vector aim;
    double fov;
    double clock;
    double last;
    double bootStart;
    double reveal;
    double targetReveal;
    double detail;
    double targetDetail;
    double rotation;
    double targetRotation;
    double scanTime;
    double scanBlend;
    double lastInteraction;
    double idleGain;
    double pulseGain;
    bool looping;
    int selected;
    int nextActor;
    QString mode;
    QVector<Pulse> pulses;
    std::shared_ptr<NativeViewer::ResumeState> viewer;
};

/** 捕获挂起前的相机、动画与浏览位置；恢复时取消尚未结束的指针手势。 */
std::shared_ptr<NativeScene::ResumeState> NativeScene::captureResumeState() const {
    auto state = std::make_shared<ResumeState>();
    state->actor = m_actor;
    state->returning = m_returning;
    state->memories = m_memories;
    state->theme = m_theme;
    state->decryption = m_decryption;
    state->rail = m_rail;
    state->shoulder = m_shoulder;
    state->laneFocus = m_laneFocus;
    state->columnCamera = m_columnCamera;
    state->origin = m_origin;
    state->eye = m_eye;
    state->aim = m_aim;
    state->fov = m_fov;
    state->clock = m_clock;
    state->last = m_last;
    state->bootStart = m_bootStart;
    state->reveal = m_reveal;
    state->targetReveal = m_targetReveal;
    state->detail = m_detail;
    state->targetDetail = m_targetDetail;
    state->rotation = m_rotation;
    state->targetRotation = m_targetRotation;
    state->scanTime = m_scanTime;
    state->scanBlend = m_scanBlend;
    state->lastInteraction = m_lastInteraction;
    state->idleGain = m_idleGain;
    state->pulseGain = m_pulseGain;
    state->looping = m_looping;
    state->selected = m_selected;
    state->nextActor = m_nextActor;
    state->mode = m_mode;
    state->pulses = m_pulses;
    state->viewer = m_viewer->captureResumeState();
    return state;
}

/** 把纯值状态应用到新实例；模型与渲染资源仍由正常初始化路径重新建立。 */
void NativeScene::restoreResumeState(const ResumeState &state) {
    m_actor = state.actor;
    m_returning = state.returning;
    m_memories = state.memories;
    m_theme = state.theme;
    m_decryption = state.decryption;
    m_rail = state.rail;
    m_shoulder = state.shoulder;
    m_laneFocus = state.laneFocus;
    m_columnCamera = state.columnCamera;
    m_origin = state.origin;
    m_eye = state.eye;
    m_aim = state.aim;
    m_fov = state.fov;
    m_clock = state.clock;
    m_last = state.last;
    m_bootStart = state.bootStart;
    m_reveal = state.reveal;
    m_targetReveal = state.targetReveal;
    m_detail = state.detail;
    m_targetDetail = state.targetDetail;
    m_rotation = state.rotation;
    m_targetRotation = state.targetRotation;
    m_scanTime = state.scanTime;
    m_scanBlend = state.scanBlend;
    m_lastInteraction = state.lastInteraction;
    m_idleGain = state.idleGain;
    m_pulseGain = state.pulseGain;
    m_looping = state.looping;
    m_selected = state.selected;
    m_nextActor = state.nextActor;
    m_mode = state.mode;
    m_pulses = state.pulses;
    m_viewer->restoreResumeState(*state.viewer);
    resetPointer();
    m_pointer = {};
    m_changed = true;
    m_view = Matrix::camera(m_eye, m_aim).inverse();
    resize(m_width, m_height);
}
