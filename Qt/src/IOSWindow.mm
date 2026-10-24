#include "IOSWindow.h"
#include "Engine.h"
#include <QGuiApplication>
#include <QQuickWindow>

/** iOS 只允许横屏；允许后台释放图形资源 */
void configureIOSWindow(QQuickWindow *window, Engine *backend) {
    window->reportContentOrientationChange(Qt::LandscapeOrientation);
    window->showFullScreen();
    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, backend,
                     [backend, window](Qt::ApplicationState state) {
                         const bool active = state == Qt::ApplicationActive;
                         backend->setVisible(active);
                         if (!active) {
                             backend->trimMemory();
                             window->releaseResources();
                         }
                     });
}
