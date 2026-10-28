#include "render/SceneLayerHost.h"

#include <QFile>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRhiItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <rhi/qrhi.h>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>) && !defined(Q_OS_DARWIN)
#include <QVulkanInstance>
#define RHINE_LAYER_VULKAN 1
#else
#define RHINE_LAYER_VULKAN 0
#endif

namespace {
int liveRenderers = 0;

class ProbeRenderer final : public QQuickRhiItemRenderer {
  public:
    ProbeRenderer() {
        ++liveRenderers;
    }
    ~ProbeRenderer() override {
        --liveRenderers;
    }
    void initialize(QRhiCommandBuffer *) override {
    }
    void synchronize(QQuickRhiItem *) override {
    }
    void render(QRhiCommandBuffer *commands) override {
        commands->beginPass(renderTarget(), Qt::red, {1, 0});
        commands->endPass();
    }
};

class ProbeItem : public QQuickRhiItem {
    Q_OBJECT
  public:
    using QQuickRhiItem::QQuickRhiItem;
    QQuickRhiItemRenderer *createRenderer() override {
        return new ProbeRenderer;
    }
};

bool check(bool passed, const char *message) {
    if (!passed) {
        qCritical("%s", message);
    }
    return passed;
}

/** 使用真实 RhiItem 验证内部纹理密度、逻辑输入及宿主重复建立，不访问音乐服务。 */
bool verifyLayers(QRhi *rhi, QVulkanInstance *instance, const QUrl &fixture) {
    QQmlEngine engine;
    QWindow visible;
    bool passed = true;
    for (int cycle = 0; cycle < 3; ++cycle) {
        SceneLayerHost host;
        if (!check(host.start(engine, fixture, fixture, rhi, instance, &visible), "启动双层离屏宿主")) {
            return false;
        }
        const QSize logical(800, 600);
        for (const QSize pixels : {QSize(800, 600), QSize(1000, 750), QSize(1600, 1200), QSize(1001, 751)}) {
            host.resize(logical, pixels);
            if (!check(host.renderFrame(), "目标尺寸改变后正常渲染")) {
                return false;
            }
            passed &= check(host.sceneTexture()->pixelSize() == pixels &&
                                host.overlayTexture()->pixelSize() == pixels,
                            "两层纹理跟随实际 surface 尺寸");
            passed &= check(host.sceneWindow()->size() == logical &&
                                host.overlayRootItem()->size() == QSizeF(logical) &&
                                host.overlayRootItem()->scale() == 1.0,
                            "排版和输入保持逻辑坐标");
#ifdef Q_OS_LINUX
            if (rhi->backend() == QRhi::Vulkan) {
                auto *scene = host.sceneWindow()->findChild<ProbeItem *>();
                passed &= check(scene && scene->effectiveColorBufferSize() == pixels,
                                "Linux Vulkan 的 RhiItem 内部纹理不能被宿主 DPR 降采样");
            }
#endif
        }
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            const QPointF position(logical.width() * 0.8, logical.height() * 0.8);
            QMouseEvent click(type, position, position, Qt::LeftButton,
                              type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(host.overlayWindow(), &click);
        }
        passed &= check(host.overlayRootItem()->property("clicked").toBool(), "逻辑坐标点击命中右下区域");
    }
    passed &= check(liveRenderers == 0, "宿主析构后不保留场景渲染器");
    return passed;
}
} // namespace

int main(int argc, char **argv) {
    // offscreen 平台可能自动选择 software；本测试明确验证 QRhi 的 Null/Vulkan 适配。
    QQuickWindow::setSceneGraphBackend(QStringLiteral("rhi"));
    QGuiApplication application(argc, argv);
    const bool vulkan = application.arguments().contains("--vulkan");
    QVulkanInstance *instance = nullptr;
#if RHINE_LAYER_VULKAN
    QVulkanInstance vulkanInstance;
#endif
    std::unique_ptr<QRhi> rhi;
#if RHINE_LAYER_VULKAN
    if (vulkan) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        vulkanInstance.setApiVersion(QVersionNumber(1, 1));
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
    if (!check(bool(rhi), "请求的图形后端可用")) {
        return 1;
    }
    qmlRegisterType<ProbeItem>("Rhine.LayerTest", 1, 0, "ProbeItem");
    QTemporaryDir directory;
    QFile fixture(directory.filePath("Layer.qml"));
    if (!fixture.open(QIODevice::WriteOnly)) {
        return 1;
    }
    fixture.write(R"(
        import QtQuick
        import Rhine.LayerTest
        Item {
            property bool clicked: false
            ProbeItem { anchors.fill: parent }
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
    return verifyLayers(rhi.get(), instance, QUrl::fromLocalFile(fixture.fileName())) ? 0 : 1;
}

#include "SceneLayerChecks.moc"
