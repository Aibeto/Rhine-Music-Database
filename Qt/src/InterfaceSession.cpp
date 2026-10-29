#include <QtGlobal>
#include "InterfaceSession.h"
#include <QTimer>
#include <utility>

/** 建立界面进程的生命周期管理；不探测桌面状态。 */
InterfaceSession::InterfaceSession(InterfaceLaunch launch, QObject *parent)
    : QObject(parent), m_launch(std::move(launch)) {
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

/** 用户主动打开的界面恢复自动恢复资格。 */
void InterfaceSession::attach(qint64 processId) {
    m_state.connected = true;
    m_state.ready = false;
    m_state.processId = processId;
    m_state.wanted = true;
    m_awaitingAttach = false;
    emit stateChanged();
}

void InterfaceSession::ready() {
    m_state.ready = true;
    if (m_activateNext) {
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
    emit stateChanged();
}

/** 已连接的界面只做激活；未连接时按需拉起新进程。 */
void InterfaceSession::show() {
    if (m_stopping) {
        return;
    }
    m_state.wanted = true;
    m_activateNext = true;
    if (m_state.connected && m_state.ready) {
        emit activationRequested();
        m_activateNext = false;
    } else if (!m_state.connected) {
        launch();
    }
    emit stateChanged();
}

void InterfaceSession::stop() {
    m_stopping = true;
    ++m_launchGeneration;
}

#ifdef Q_OS_LINUX
/** 把调用方解析出的后端钉进重建环境：新进程按它选后端，和请求方看到的一致。 */
void InterfaceSession::overrideBackend(const QString &backend) {
    // QSG_RHI_BACKEND 优先级高于 RHINE_GRAPHICS，留着会盖掉刚写进去的值。
    m_launch.environment.remove(QStringLiteral("QSG_RHI_BACKEND"));
    if (backend.isEmpty()) {
        m_launch.environment.remove(QStringLiteral("RHINE_GRAPHICS"));
        return;
    }
    m_launch.environment.insert(QStringLiteral("RHINE_GRAPHICS"), backend);
}

#endif
const InterfaceState &InterfaceSession::state() const {
    return m_state;
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
