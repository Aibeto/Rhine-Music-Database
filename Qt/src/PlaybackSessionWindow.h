#pragma once

#include "DeferredWindowTransition.h"
#include <QObject>
#include <QRect>
#include <QString>

class EngineHolder;
class PlaybackConnection;
class PresentationShell;
class QWindow;

/**
 * 连接同一用户的唯一服务；没有运行时按需拉起后台 helper，
 * 并校验协议版本，避免新版界面连到旧版服务。
 */
bool connectPlaybackService(PlaybackConnection &connection, const QString &root, QString &error);

/**
 * 性能模式下界面窗口与服务之间的生命周期控制。
 * 关闭到菜单栏、最小化或自动遮挡时把呈现层切到静态页并销毁业务引擎；
 * 恢复时在同一窗口重建。窗口、原生句柄和 Space 归属始终不变。
 */
class PlaybackSessionWindow : public QObject {
    Q_OBJECT
  public:
    PlaybackSessionWindow(PlaybackConnection *connection, QWindow *window, PresentationShell *shell,
                          EngineHolder *holder, QObject *parent = nullptr);
    ~PlaybackSessionWindow() override;

    /// 把当前窗口、引擎与呈现层状态发给服务，供离线验收与诊断读取。
    void publishRenderState();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    enum class Intent { None, SuspendAutomatically, SuspendManually, Restore, Activate };
    void requestSuspension(bool automatic = false);
    void requestRestoration(bool activate = false);
    void applyTransition();
    void suspendRendering(bool automatic);
    void restoreRendering(bool activate);
    void notifyScreen();
    void saveInterfaceState();

    DeferredWindowTransition m_transition;
    Intent m_intent = Intent::None;
    PlaybackConnection *m_connection;
    QWindow *m_window;
    PresentationShell *m_shell;
    EngineHolder *m_holder;
    QRect m_normalGeometry;
    Qt::WindowStates m_savedStates;
    bool m_restoreWhenExposed = false;
    bool m_suspended = false;
    bool m_suspensionReported = false;
    bool m_automaticSuspension = false;
    bool m_explicitExit = false;
    bool m_changingWindowState = false;
};
