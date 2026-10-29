#include "DesktopPresence.h"
#include "DesktopVisibility.h"

#ifndef Q_OS_MACOS
namespace {
class DefaultDesktopPresence final : public DesktopPresenceMonitor {
  public:
    void setScreen(const QString &name, const QRect &geometry) override {
        m_screen = name;
        m_geometry = geometry;
    }
    void resetWorkspace() override {
        // 当前平台的覆盖适配器不保留原生桌面锚点。
    }
    DesktopPresence sample(qint64 interfacePid, bool) const override {
        DesktopPresence result;
        result.covered = platformDesktopCovered(m_screen, interfacePid, m_geometry);
        return result;
    }

  private:
    QString m_screen;
    QRect m_geometry;
};
}

std::unique_ptr<DesktopPresenceMonitor> createDesktopPresenceMonitor() {
    return std::make_unique<DefaultDesktopPresence>();
}
#endif
