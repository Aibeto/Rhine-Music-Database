#pragma once
#include "DesktopPresence.h"
#include <QObject>
#include <QProcess>
#include <QRect>
#include <QTimer>
#include <functional>

struct InterfaceLaunch {
    QString executable;
    QStringList arguments;
    QProcessEnvironment environment;
};

struct InterfaceState {
    bool connected = false;
    bool ready = false;
    bool wanted = false;
    bool covered = false;
    bool workspaceActive = true;
    bool restoreRequiresUser = false;
    bool fullscreen = false;
    bool suspended = false;
    qint64 processId = 0;
};

/** 独占 GUI 进程与恢复意图；不依赖音频、账号、IPC 套接字或渲染器。 */
class InterfaceSession : public QObject {
    Q_OBJECT
  public:
    using PresenceQuery = std::function<DesktopPresence(const QString &, qint64, const QRect &)>;
    explicit InterfaceSession(InterfaceLaunch launch, PresenceQuery query = {}, QObject *parent = nullptr);
    ~InterfaceSession() override;
    void attach(qint64 processId);
    void ready();
    void detach();
    void release(bool automatic);
    void setScreen(QString name, QRect geometry);
    void setFullscreen(bool fullscreen);
    void resumed();
    void show();
    void stop();
    /// 重建界面时把调用方解析出的图形后端显式传给新进程。
    /// 配置与一次性覆盖（RHINE_GRAPHICS / QSG_RHI_BACKEND）两条路径都在这里收敛，
    /// 否则新界面会重新解析回旧值，反复触发重建。
    void overrideBackend(const QString &backend);
    void setPresenceQuery(PresenceQuery query);
    DesktopPresence nativePresence(qint64 processId = -1, bool fullscreen = false) const;
    const InterfaceState &state() const;

  signals:
    void activationRequested();
    void suspensionRequested();
    void restorationRequested();
    void stateChanged();

  private:
    InterfaceLaunch m_launch;
    std::unique_ptr<DesktopPresenceMonitor> m_desktop;
    PresenceQuery m_query;
    InterfaceState m_state;
    QProcess m_process;
    QTimer m_monitor;
    QString m_screenName;
    QRect m_screenGeometry;
    bool m_activateNext = false;
    bool m_awaitingAttach = false;
    bool m_stopping = false;
    quint64 m_launchGeneration = 0;
    void updateVisibility();
    void launch();
};
