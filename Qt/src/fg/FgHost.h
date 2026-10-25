#pragma once

#include <QSize>
#include <QString>
#include <QUrl>
#include <memory>

QT_BEGIN_NAMESPACE
class QQmlEngine;
class QRhi;
class QRhiRenderPassDescriptor;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class QVulkanInstance;
QT_END_NAMESPACE

/**
 * FG（帧生成）的离屏双场景宿主。
 *
 * 职责：把 Scene3D.qml 与 Overlay.qml 分别渲染进两张 QRhiTexture，供 FG 后端
 * 做 optical flow / 帧插值，并在插值帧之后重新叠加 UI。两个场景各自一个
 * QQuickWindow + QQuickRenderControl，但共用调用方传入的同一个 QRhi——FG 要直接
 * 采样 3D 纹理，跨 QRhi 的纹理不可共享。
 *
 * 只在 FG 启用时实例化；默认路径（不开 FG）完全不碰这个类。
 *
 * 失败策略：任何一步失败都返回 false 并打印可诊断日志，绝不 abort。
 * 调用方据此回退到现有在屏 QQuickWindow 路径——FG 是可选增强，不能阻断启动。
 *
 * 线程与生命周期约束（调用方必须保证）：
 *   - 所有成员函数都在 Qt Quick 的渲染线程调用（本项目是 GUI 线程），且调用时
 *     不能有另一个 QRhi 帧正在录制：beginFrame()/endFrame() 内部会开离屏帧。
 *   - start() 传入的 QRhi 必须比本对象活得久；析构会释放建在该 QRhi 上的纹理与
 *     渲染目标，所以必须在 QRhi 所有者（主窗口的场景图）销毁之前析构本对象。
 *   - 两个离屏 QQuickWindow 永不 show()：RenderControl 下没有原生窗口。
 *
 * Qt 6.12 说明：QQuickRenderControl 没有 initialize(QRhi*) 重载。共享 QRhi 的公开
 * 入口是 QQuickWindow::setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi))，
 * 之后 initialize() 不会再自建 QRhi（见 QQuickRenderControl::initialize 文档）。
 */
class FgHost
{
public:
    FgHost();
    ~FgHost();

    FgHost(const FgHost &) = delete;
    FgHost &operator=(const FgHost &) = delete;

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
               QVulkanInstance *vulkanInstance = nullptr);

    /** 两个场景各走一遍 polishItems → beginFrame → sync → render → endFrame。 */
    bool renderFrame();

    /** 重建离屏目标。换 target 会触发 Qt 重新初始化渲染目标，所以只在这里做，不要每帧换。 */
    void resize(QSize size);

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

    bool isValid() const { return m_valid; }
    QSize size() const { return m_size; }
    QString lastError() const { return m_lastError; }

    /**
     * 内容有没有变化。
     * 两个离屏窗口收到 Qt 的 UpdateRequest（QML 里任何东西改了都会发）或者被
     * resize()/输入转发显式标脏时为真。呈现层只在为真时渲染：静止画面下继续
     * "渲染→合成→present"是纯浪费，实测这条路径每步仍在消耗 GPU，还会把
     * 呈现节奏挤到 40ms 一帧。
     */
    bool contentDirty() const { return m_contentDirty; }
    /// 外部改了离屏窗口内容（比如转发输入）时显式标脏。
    void markContentDirty() { m_contentDirty = true; }

private:
    struct Layer {
        std::unique_ptr<QQuickRenderControl> control;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *rootItem = nullptr;   // 由 window->contentItem() 持有，这里只观察
        QRhiTexture *texture = nullptr;
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
    void teardown();

    Layer m_scene;
    Layer m_overlay;
    QRhi *m_rhi = nullptr;
    QSize m_size;
    /// 首帧必须渲染，所以初值为真；renderFrame() 成功后清零。
    bool m_contentDirty = true;
    bool m_valid = false;
    QString m_lastError;
};
