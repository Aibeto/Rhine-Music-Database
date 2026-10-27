#include "EngineHolder.h"
#include "Engine.h"
#include "FrameProfiler.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QPixmapCache>
#include <QTimer>
#include <QVariant>
#ifdef Q_OS_MACOS
#include <malloc/malloc.h>
#include "InterfaceWindowMac.h"
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <malloc.h>
#endif

namespace {
/**
 * 把已释放的空闲页归还系统。Qt 与平台字体缓存会把大量空闲页留在进程里，
 * 只析构对象并不降低物理占用；延迟执行是因为部分分配要等 QML 对象真正销毁后才可回收。
 */
void relieveMemoryPressure() {
#ifdef Q_OS_MACOS
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(Q_OS_WIN)
    // 已释放的物理页交还系统；不遍历或压缩进程堆，图形资源由呈现层负责销毁。
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
#else
    malloc_trim(0);
#endif
}
} // namespace

EngineHolder::EngineHolder(QString root, bool offline, QQmlContext *context, PlaybackConnection *connection,
                           QString profilePath, QObject *parent)
    : QObject(parent), m_root(std::move(root)), m_offline(offline), m_context(context),
      m_connection(connection), m_profilePath(std::move(profilePath)) {
}

EngineHolder::~EngineHolder() {
    stop();
}

bool EngineHolder::start() {
#ifdef Q_OS_MACOS
    bool started = false;
    drainInterfaceResources([this, &started] { started = startInternal(); });
    return started;
#else
    return startInternal();
#endif
}

bool EngineHolder::startInternal() {
    if (m_engine) {
        return true;
    }
    m_engine = std::make_unique<Engine>(m_root, m_offline, nullptr, m_connection, m_resumeState);
    if (m_sceneState) {
        m_engine->restorePresentation(*m_sceneState, m_animationTime);
    }
    // 恢复后由新引擎持有状态，挂起检查点不继续占用已失去用途的元数据。
    m_resumeState = {};
    m_sceneState.reset();
    m_profiler = std::make_unique<FrameProfiler>(m_engine.get(), m_profilePath);
    connect(m_engine.get(), &Engine::performanceRequested, m_profiler.get(), &FrameProfiler::toggleVisible);
    if (m_context) {
        m_context->setContextProperty("rhine", m_engine.get());
        m_context->setContextProperty("profiler", m_profiler.get());
    }
    emit started(m_engine.get());
    return true;
}

void EngineHolder::stop() {
    if (!m_engine) {
        return;
    }
    // 只保留元数据和场景纯值，不保留引擎、图片、QML 或 GPU 资源。
    m_resumeState = m_engine->app->sessionState();
    m_resumeState.remove("player");
    m_sceneState = m_engine->scene.captureResumeState();
    m_animationTime = m_engine->animationTime();
    if (m_context) {
        m_context->setContextProperty("rhine", QVariant::fromValue<QObject *>(nullptr));
        m_context->setContextProperty("profiler", QVariant::fromValue<QObject *>(nullptr));
    }
    m_profiler.reset();
    m_engine.reset();
    QPixmapCache::clear();
    if (m_context && m_context->engine()) {
        m_context->engine()->collectGarbage();
        m_context->engine()->clearComponentCache();
    }
    // 等 QML 对象与字体缓存的延迟销毁完成后，再把空闲页交还系统。
    QTimer::singleShot(1000, this, [this] {
        if (!m_engine) {
            relieveMemoryPressure();
        }
    });
}
