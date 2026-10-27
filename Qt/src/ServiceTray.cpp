#include "ServiceTray.h"
#include "TrayIcon.h"
#include <QAction>
#include <QApplication>
#include <QCursor>

ServiceTray::ServiceTray(std::function<void()> showInterface, std::function<void()> quit, QObject *parent)
    : QObject(parent) {
    m_icon.setObjectName("desktopTrayIcon");
    m_icon.setParent(this);
    m_icon.setIcon(rhineTrayIcon());
    m_icon.setToolTip(QGuiApplication::applicationDisplayName());
    auto show = m_menu.addAction(QStringLiteral("打开界面"));
    connect(show, &QAction::triggered, this, showInterface);
    auto exit = m_menu.addAction(QStringLiteral("退出"));
    connect(exit, &QAction::triggered, this, quit);
#ifndef Q_OS_MACOS
    m_icon.setContextMenu(&m_menu);
#endif
    connect(&m_icon, &QSystemTrayIcon::activated, this,
            [this, showInterface](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    showInterface();
                }
#ifdef Q_OS_MACOS
                if (reason == QSystemTrayIcon::Context) {
                    m_menu.popup(QCursor::pos());
                }
#endif
            });
    m_icon.show();
    QGuiApplication::setQuitOnLastWindowClosed(false);
}
