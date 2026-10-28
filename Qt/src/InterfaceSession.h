#pragma once
#include <QObject>
#include <QProcess>

struct InterfaceLaunch {
    QString executable;
    QStringList arguments;
    QProcessEnvironment environment;
};

struct InterfaceState {
    bool connected = false;
    bool ready = false;
    bool wanted = false;
    qint64 processId = 0;
};

/** 独占 GUI 进程；不依赖音频、账号或 IPC 套接字。 */
class InterfaceSession : public QObject {
    Q_OBJECT
  public:
    explicit InterfaceSession(InterfaceLaunch launch, QObject *parent = nullptr);
    ~InterfaceSession() override;
    void attach(qint64 processId);
    void ready();
    void detach();
    void show();
    void stop();
    const InterfaceState &state() const;

  signals:
    void activationRequested();
    void stateChanged();

  private:
    InterfaceLaunch m_launch;
    InterfaceState m_state;
    QProcess m_process;
    bool m_activateNext = false;
    bool m_awaitingAttach = false;
    bool m_stopping = false;
    quint64 m_launchGeneration = 0;
    void launch();
};
