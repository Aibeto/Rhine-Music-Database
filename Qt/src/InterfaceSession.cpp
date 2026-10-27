#include "InterfaceSession.h"
#include <QGuiApplication>
#include <QScreen>
#include <utility>

/**
 * 建立界面生命周期与覆盖查询；
 * 查询可替换，离线回归无需操作用户窗口或修改系统设置。
 */
InterfaceSession::InterfaceSession(InterfaceLaunch launch, PresenceQuery query, QObject *parent)
    : QObject(parent), m_launch(std::move(launch)), m_desktop(createDesktopPresenceMonitor()),
      m_query(std::move(query)) {
    if (auto screen = QGuiApplication::primaryScreen()) {
        setScreen(screen->name(), screen->geometry());
    }
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    const auto cancelFailedLaunch = [this] {
        if (m_awaitingAttach) {
            m_awaitingAttach = false;
            m_state.wanted = false;
            emit stateChanged();
        }
    };
    connect(&m_process, &QProcess::errorOccurred, this, cancelFailedLaunch);
    connect(&m_process, &QProcess::finished, this, cancelFailedLaunch);
    m_monitor.setInterval(500);
    connect(&m_monitor, &QTimer::timeout, this, &InterfaceSession::updateVisibility);
    m_monitor.start();
}

/** 只清理本模块启动的 GUI；用户从外部启动的窗口通过连接协议正常退出。 */
InterfaceSession::~InterfaceSession() {
    stop();
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(2000)) {
            m_process.kill();
            m_process.waitForFinished(2000);
        }
    }
}

/** 用户主动打开的界面恢复自动恢复资格，并把桌面锚点重新落到当前 Space。 */
void InterfaceSession::attach(qint64 processId) {
    m_state.connected = true;
    m_state.ready = false;
    m_state.processId = processId;
    m_state.fullscreen = false;
    m_state.suspended = false;
    m_state.wanted = true;
    m_state.restoreRequiresUser = false;
    m_awaitingAttach = false;
    m_desktop->resetWorkspace();
    emit stateChanged();
}

void InterfaceSession::ready() {
    m_state.ready = true;
    if (m_activateNext) {
        m_state.suspended = false;
        emit activationRequested();
        m_activateNext = false;
    }
    emit stateChanged();
}

void InterfaceSession::detach() {
    m_state.connected = false;
    m_state.ready = false;
    m_state.processId = 0;
    m_state.wanted = false;
    m_state.suspended = false;
    emit stateChanged();
}

void InterfaceSession::release(bool automatic) {
    m_state.suspended = true;
    if (!automatic) {
        m_state.wanted = false;
        m_activateNext = false;
    }
    emit stateChanged();
}

void InterfaceSession::setScreen(QString name, QRect geometry) {
    m_screenName = std::move(name);
    m_screenGeometry = geometry;
    m_desktop->setScreen(m_screenName, m_screenGeometry);
}

void InterfaceSession::show() {
    if (m_stopping) {
        return;
    }
    m_state.wanted = true;
    m_state.restoreRequiresUser = false;
    m_activateNext = true;
    if (m_state.connected && m_state.ready) {
        m_state.suspended = false;
        emit activationRequested();
        m_activateNext = false;
    } else if (!m_state.connected) {
        launch();
    }
    emit stateChanged();
}

void InterfaceSession::resumed() {
    m_state.suspended = false;
    m_state.wanted = true;
    m_state.restoreRequiresUser = false;
    emit stateChanged();
}

void InterfaceSession::setFullscreen(bool fullscreen) {
    m_state.fullscreen = fullscreen;
}

void InterfaceSession::stop() {
    m_stopping = true;
    ++m_launchGeneration;
    m_monitor.stop();
}

void InterfaceSession::setPresenceQuery(PresenceQuery query) {
    m_query = std::move(query);
    updateVisibility();
}

DesktopPresence InterfaceSession::nativePresence(qint64 processId, bool fullscreen) const {
    if (processId >= 0) {
        return m_desktop->sample(processId, fullscreen);
    }
    return m_desktop->sample(m_state.processId, m_state.fullscreen);
}

const InterfaceState &InterfaceSession::state() const {
    return m_state;
}

/**
 * 只切换窗口内容；后台窗口、桌面归属和原生句柄持续保留，
 * 不主动改变焦点。
 */
void InterfaceSession::updateVisibility() {
    if (m_stopping || !m_state.wanted) {
        return;
    }
    const auto presence =
        m_query ? m_query(m_screenName, m_state.processId, m_screenGeometry) : nativePresence();
    m_state.covered = presence.covered;
    m_state.workspaceActive = presence.workspaceActive;
    const bool shouldSuspend = presence.covered || !presence.workspaceActive || presence.stagedAway;
    if (m_state.connected && m_state.ready) {
        if (shouldSuspend && !m_state.suspended && !m_activateNext) {
            m_state.restoreRequiresUser = !presence.automaticRestoreAllowed || presence.stagedAway;
            m_state.suspended = true;
            emit suspensionRequested();
        } else if (!shouldSuspend && m_state.suspended && !m_state.restoreRequiresUser) {
            m_state.suspended = false;
            emit restorationRequested();
        }
    } else if (!m_state.connected && m_activateNext) {
        launch();
    }
    emit stateChanged();
}

/** GUI 以独立进程重建；启动失败后停止重试并等待用户重新打开。 */
void InterfaceSession::launch() {
    if (m_launch.executable.isEmpty() || m_awaitingAttach || m_process.state() != QProcess::NotRunning) {
        return;
    }
    auto environment = m_launch.environment;
#ifdef Q_OS_MACOS
    // 服务自身以 agent 身份抑制前台转换；
    // 用户主动打开的界面必须恢复默认激活行为。
    environment.remove("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM");
#endif
    m_process.setProcessEnvironment(environment);
    const auto generation = ++m_launchGeneration;
    m_awaitingAttach = true;
    m_process.start(m_launch.executable, m_launch.arguments);
    m_activateNext = false;
    QTimer::singleShot(10000, this, [this, generation] {
        if (generation == m_launchGeneration && m_awaitingAttach && !m_state.connected) {
            m_awaitingAttach = false;
            m_state.wanted = false;
            m_process.kill();
            emit stateChanged();
        }
    });
}
