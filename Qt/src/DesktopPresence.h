#pragma once
#include <QRect>
#include <QString>
#include <memory>

struct DesktopPresence {
    bool covered = false;
    bool workspaceActive = true;
    bool stagedAway = false;
    bool automaticRestoreAllowed = true;
};

/** 平台只报告桌面事实；释放、恢复及用户意图由共享生命周期模块决定。 */
class DesktopPresenceMonitor {
  public:
    virtual ~DesktopPresenceMonitor() = default;
    virtual void setScreen(const QString &name, const QRect &geometry) = 0;
    virtual void resetWorkspace() = 0;
    virtual DesktopPresence sample(qint64 interfacePid, bool fullscreen) const = 0;
};

std::unique_ptr<DesktopPresenceMonitor> createDesktopPresenceMonitor();
