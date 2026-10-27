#pragma once
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include <functional>

/** 后台服务的桌面驻留入口：打开界面和退出都由服务回调决定。 */
class ServiceTray : public QObject {
  public:
    ServiceTray(std::function<void()> showInterface, std::function<void()> quit,
                QObject *parent = nullptr);

  private:
    QMenu m_menu;
    QSystemTrayIcon m_icon;
};
