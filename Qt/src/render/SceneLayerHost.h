#pragma once

#include <QSize>
#include <QString>
#include <QUrl>
#include <memory>
#include <functional>

QT_BEGIN_NAMESPACE
class QScreen;
class QQmlEngine;
class QRhi;
class QRhiRenderPassDescriptor;
class QRhiRenderBuffer;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class QVulkanInstance;
class QWindow;
QT_END_NAMESPACE

/**
 * 场景与 UI 的离屏宿主，共享一个 QRhi 并分别响应 Qt Quick 更新信号。
 * 所有操作在同一渲染线程执行，且调用前不得存在活动的 QRhi 帧。
 * QRhi 与可见窗口必须活到宿主销毁之后；两层离屏窗口不能 show。
 */
class SceneLayerHost {
  public:
    SceneLayerHost();
    ~SceneLayerHost();

    SceneLayerHost(const SceneLayerHost &) = delete;
    SceneLayerHost &operator=(const SceneLayerHost &) = delete;

    /**
     * 建立两个离屏场景并载入 QML。
     * scene3dUrl / overlayUrl 指向 Scene3D.qml / Overlay.qml（通常在 qrc:/qt/qml/RhineMusic/ 下）。
     * engine 必须已经设置好 rhine / profiler / rhineFontFamily 这些根上下文属性。
     * 初始尺寸是兜底值，调用方应在 start() 成功后立刻用真实窗口尺寸调用 resize()。
     */
    /// vulkanInstance 必须与 rhi 同源（同一个 QVulkanInstance 建的设备）。
    /// Qt 的 QQuickRenderControl::initialize() 即使拿到 fromRhi(rhi) 也仍然要求
    /// QQuickWindow 上有 QVulkanInstance，否则直接失败并报
    /// "No QVulkanInstance set for QQuickWindow, cannot initialize"。
    bool start(QQmlEngine &engine, const QUrl &scene3dUrl, const QUrl &overlayUrl, QRhi *rhi,
               QVulkanInstance *vulkanInstance = nullptr, QWindow *visibleWindow = nullptr);

    /** 两个场景各走一遍 polishItems → beginFrame → sync → render → endFrame。 */
    bool renderFrame();
    void releaseGraphics();
    bool resumeGraphics();

    /** 重建离屏目标。换 target 会触发 Qt 重新初始化渲染目标，所以只在这里做，不要每帧换。 */
    // Linux Vulkan 可传入 WSI 返回的实际像素尺寸；其余路径继续按逻辑尺寸和 DPR 分配。
    void resize(QSize size, QSize surfacePixels = {});

    /** 3D 层渲染结果（不透明，带应用底色），供 FG 做超分/插值。 */
    QRhiTexture *sceneTexture() const;
    /** UI 层渲染结果，内容是预乘 alpha（Qt Quick 的合成语义），FG 侧按 usePremulAlpha 消费。 */
    QRhiTexture *overlayTexture() const;

    /// 两个离屏窗口。FG 宿主需要把输入事件转发给 UI 层（QQuickRenderControl 下
    /// 事件必须由调用方手动投递），3D 层只用于取景不做交互。
    QQuickWindow *overlayWindow() const;
    QQuickWindow *sceneWindow() const;

    /// UI 层（Overlay.qml）的根条目。
    /// 离屏窗口永远不会被平台激活，Qt 也不会像默认路径那样把焦点自动落到输入层，
    /// 所以 FG 路径需要拿它显式做一次焦点初始化（Overlay.qml 导出 focusInputLayer()）。
    QQuickItem *overlayRootItem() const;

    bool isValid() const {
        return m_valid;
    }
    QSize size() const {
        return m_size;
    }
    /// 离屏目标的**物理像素**尺寸 = 逻辑尺寸 × 设备像素比。
    /// QML 仍按逻辑像素排版，只有光栅化面积变大；两者搞混就会出现"高 DPI 下发糊"。
    QSize pixelSize() const;
    /// 设置纹理的物理像素密度，不改变 QML 排版和输入坐标。
    void setPixelRatio(qreal ratio);
    qreal pixelRatio() const {
        return m_ratio;
    }
    QString lastError() const {
        return m_lastError;
    }

    /** 两层是否收到更新；各层在自己的渲染完成后独立清除标记。 */
    bool contentDirty() const {
        return m_scene.dirty || m_overlay.dirty;
    }
    /// 外部改了离屏窗口内容（比如转发输入）时显式标脏。
    void markContentDirty() {
        markOverlayDirty();
    }
    void markSceneDirty() {
        m_scene.dirty = true;
    }
    void markOverlayDirty() {
        m_overlay.dirty = true;
    }
    std::function<void()> requestRender;
    quint64 sceneRenderCount() const {
        return m_sceneRenders;
    }
    quint64 overlayRenderCount() const {
        return m_overlayRenders;
    }

  private:
    struct Layer {
        std::unique_ptr<QQuickRenderControl> control;
        std::unique_ptr<QQuickWindow> window;
        bool dirty = true;
        QQuickItem *rootItem = nullptr; // 由 window->contentItem() 持有，这里只观察
        QRhiTexture *texture = nullptr;
        QRhiRenderBuffer *depthStencil = nullptr;
        QRhiTextureRenderTarget *renderTarget = nullptr;
        QRhiRenderPassDescriptor *renderPass = nullptr;
    };

    bool fail(const QString &message);
    bool startInternal(QQmlEngine &engine, const QUrl &scene3dUrl, const QUrl &overlayUrl, QRhi *rhi,
                       QVulkanInstance *vulkanInstance);
    bool createLayerTargets(Layer &layer, const char *name);
    void releaseLayerTargets(Layer &layer);
    bool loadLayer(Layer &layer, QQmlEngine &engine, const QUrl &url, const char *name);
    bool renderLayer(Layer &layer, const char *name);
    void applyLayerGeometry(Layer &layer);
    void syncSceneBufferSize();
    void teardown();

    Layer m_scene;
    Layer m_overlay;
    QRhi *m_rhi = nullptr;
    QSize m_size;
    QSize m_surfacePixels;
    qreal m_ratio = 1.0;
    /// 首帧必须渲染，所以初值为真；renderFrame() 成功后清零。
    QWindow *m_visibleWindow = nullptr;
    quint64 m_sceneRenders = 0;
    quint64 m_overlayRenders = 0;
    bool m_valid = false;
    bool m_suspended = false;
    QString m_lastError;
};
