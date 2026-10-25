#pragma once
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class QWindow;

/** 管理桌面驻留入口 */
class DesktopTray : public QObject {
  public:
    explicit DesktopTray(QWindow *window, QObject *parent = nullptr);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void restoreWindow();
    void quitApplication();
    QWindow *m_window;
    QMenu m_menu;
    QSystemTrayIcon m_icon;
    bool m_quitting = false;
};
