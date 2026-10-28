#include "render/SceneLayerHost.h"

#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && !defined(Q_OS_DARWIN)
#include <QVulkanInstance>
#define RHINE_LAYER_TEST_VULKAN 1
#else
#define RHINE_LAYER_TEST_VULKAN 0
#endif

namespace {
bool check(bool passed, const char *message) {
    if (!passed) {
        qCritical() << message;
    }
    return passed;
}

/** 使用真实离屏窗口验证尺寸、目标密度及点击坐标，不访问音乐服务。 */
bool verifyLayers(QRhi *rhi, QQmlEngine &engine, const QUrl &url, QVulkanInstance *instance) {
    QWindow visible;
    SceneLayerHost host;
    if (!check(host.start(engine, url, url, rhi, instance, &visible), "Layer host starts")) {
        return false;
    }
    bool passed = true;
    QWindow *expectedRenderWindow = &visible;
#ifdef Q_OS_LINUX
    if (rhi->backend() == QRhi::Vulkan) {
        expectedRenderWindow = nullptr;
    }
#endif
    passed &= check(QQuickRenderControl::renderWindowFor(host.overlayWindow()) == expectedRenderWindow,
                    "Only Linux Vulkan changes render-window DPR ownership");
    const QSize logicalSize(800, 600);
    for (const QSize pixels : {QSize(800, 600), QSize(1000, 750), QSize(1200, 900), QSize(1600, 1200),
                               QSize(1001, 751), QSize(1000, 750)}) {
        host.resize(logicalSize, pixels);
        passed &= check(host.pixelSize() == pixels && host.sceneTexture()->pixelSize() == pixels &&
                            host.overlayTexture()->pixelSize() == pixels,
                        "Both textures follow the actual surface even when logical size is unchanged");
        passed &=
            check(host.sceneWindow()->size() == logicalSize && host.overlayWindow()->size() == logicalSize &&
                      host.overlayRootItem()->size() == QSizeF(logicalSize) &&
                      host.overlayRootItem()->scale() == 1.0,
                  "Layout and input stay in logical coordinates");
#ifdef Q_OS_LINUX
        if (rhi->backend() == QRhi::Vulkan) {
            passed &=
                check(qFuzzyCompare(host.overlayWindow()->effectiveDevicePixelRatio(), host.pixelRatio()),
                      "Linux Vulkan uses target density instead of render-window density");
        }
#endif
        passed &= check(host.renderFrame(), "Layer renders after a density change");
        host.resize(logicalSize, pixels);
        passed &= check(!host.contentDirty(), "Unchanged surface does not rebuild targets");
    }
    host.releaseGraphics();
    host.resize(QSize(960, 640), QSize(1440, 960));
    passed &= check(host.resumeGraphics() && host.renderFrame() &&
                        host.overlayTexture()->pixelSize() == QSize(1440, 960),
                    "Resume uses the latest surface dimensions");

    QQuickWindow *overlay = host.overlayWindow();
    for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent click(type, QPointF(720, 480), QPointF(720, 480), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(overlay, &click);
    }
    passed &= check(host.overlayRootItem()->property("clicked").toBool(),
                    "Logical-coordinate click reaches the lower-right control at fractional density");
    host.resize(logicalSize);
    host.setPixelRatio(2.0);
    passed &= check(host.pixelSize() == QSize(1600, 1200),
                    "Default sizing continues to follow the explicit pixel ratio");
    SceneLayerHost staticHost;
    if (!check(staticHost.startStatic(engine, url, rhi, instance), "Static host starts")) {
        return false;
    }
    staticHost.resize(QSize(800, 600), QSize(1200, 900));
    staticHost.setPixelRatio(1.0);
    passed &= check(staticHost.sceneTexture() == nullptr &&
                        staticHost.overlayTexture()->pixelSize() == QSize(1200, 900) &&
                        qFuzzyCompare(staticHost.overlayWindow()->effectiveDevicePixelRatio(), 1.5) &&
                        staticHost.renderFrame(),
                    "Explicit surface controls static target density independently of window DPR");
    return passed;
}
} // namespace

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const bool vulkan = application.arguments().contains("--vulkan");
    std::unique_ptr<QRhi> rhi;
    QVulkanInstance *instance = nullptr;
#if RHINE_LAYER_TEST_VULKAN
    QVulkanInstance vulkanInstance;
    if (vulkan) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        if (!vulkanInstance.create()) {
            return 1;
        }
        instance = &vulkanInstance;
        QRhiVulkanInitParams params;
        params.inst = instance;
        rhi.reset(QRhi::create(QRhi::Vulkan, &params));
    }
#endif
    if (!vulkan) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Null);
        QRhiNullInitParams params;
        rhi.reset(QRhi::create(QRhi::Null, &params));
    }
    if (!check(bool(rhi), "Requested graphics backend is available")) {
        return 1;
    }
    QTemporaryDir directory;
    QFile fixture(directory.filePath("Layer.qml"));
    if (!fixture.open(QIODevice::WriteOnly)) {
        return 1;
    }
    fixture.write(R"(
        import QtQuick
        Item {
            property bool clicked: false
            MouseArea {
                x: parent.width * 0.7
                y: parent.height * 0.7
                width: parent.width * 0.2
                height: parent.height * 0.2
                onClicked: parent.clicked = true
            }
        }
    )");
    fixture.close();
    QQmlEngine engine;
    return verifyLayers(rhi.get(), engine, QUrl::fromLocalFile(fixture.fileName()), instance) ? 0 : 1;
}
