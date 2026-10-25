#include "FgHost.h"

#include <QColor>
#include <QDebug>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QVulkanInstance>
#include <QQuickRenderTarget>
#include <QQuickWindow>

namespace {
/**
 * 离屏窗口的"内容变脏"监听器。
 * FgHost 本身不是 QObject，装不了事件过滤器；而 QML 里任何改动最终都会给
 * 离屏窗口投一个 QEvent::UpdateRequest，所以用一个极小的 QObject 转一道。
 * 没有 Q_OBJECT 也不需要：只覆盖虚函数 eventFilter。
 */
class DirtyWatcher : public QObject
{
public:
    explicit DirtyWatcher(bool *flag, QObject *parent = nullptr)
        : QObject(parent), m_flag(flag) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::UpdateRequest && m_flag)
            *m_flag = true;
        return QObject::eventFilter(watched, event);
    }

private:
    bool *m_flag = nullptr;
};
}  // namespace
#include <rhi/qrhi.h>

namespace {

// 初始尺寸只作兜底（与 Main.qml 的 Window 默认尺寸一致）。离屏目标建好后再由调用方
// 用真实窗口尺寸 resize() 重建；这样 start() 的签名不必依赖窗口类型。
constexpr QSize kFallbackSize(1280, 720);

// 与 Main.qml 的 Window.color 一致：3D 层要带上应用底色。否则 FG 合成后
// 3D 纹理的透明区会变成黑色，而默认路径下窗口底色是 #e7e7e1。
const QColor kSceneClearColor(0xe7, 0xe7, 0xe1);

} // namespace

FgHost::FgHost() = default;

FgHost::~FgHost()
{
    teardown();
}

bool FgHost::fail(const QString &message)
{
    m_lastError = message;
    m_valid = false;
    qWarning().noquote() << "FgHost:" << message;
    return false;
}

bool FgHost::start(QQmlEngine &engine, const QUrl &scene3dUrl, const QUrl &overlayUrl, QRhi *rhi,
                   QVulkanInstance *vulkanInstance)
{
    // 失败时立刻回收半成品：调用方会回退到默认路径，本对象可能被直接丢弃，
    // 不能把还挂着 QRhi 引用的窗口/纹理留给析构时机不确定的路径。
    if (!startInternal(engine, scene3dUrl, overlayUrl, rhi, vulkanInstance)) {
        teardown();
        return false;
    }
    return true;
}

bool FgHost::startInternal(QQmlEngine &engine, const QUrl &scene3dUrl, const QUrl &overlayUrl, QRhi *rhi,
                           QVulkanInstance *vulkanInstance)
{
    if (m_valid)
        return fail(QStringLiteral("start() 被重复调用"));
    if (!rhi)
        return fail(QStringLiteral("传入的 QRhi 为空，无法建立离屏目标"));
    if (scene3dUrl.isEmpty() || overlayUrl.isEmpty())
        return fail(QStringLiteral("场景 URL 无效：scene3d=%1 overlay=%2")
                        .arg(scene3dUrl.toString(), overlayUrl.toString()));

    m_rhi = rhi;
    m_size = kFallbackSize;
    m_lastError.clear();

    // 两个场景各自一个 QQuickRenderControl：一个 RenderControl 每帧换 render target
    // 会触发 Qt 重新初始化渲染目标（QQuickWindow::setRenderTarget 文档明确警告），
    // 拆成两个后各自固定一个 target，代价稳定，UI 层还能按需重渲。
    m_scene.control = std::make_unique<QQuickRenderControl>();
    m_overlay.control = std::make_unique<QQuickRenderControl>();
    m_scene.window = std::make_unique<QQuickWindow>(m_scene.control.get());
    m_overlay.window = std::make_unique<QQuickWindow>(m_overlay.control.get());

    // 关键：Qt 6.12 没有 initialize(QRhi*) 重载。共享同一个 QRhi 的公开入口是
    // setGraphicsDevice(fromRhi(rhi))；之后 initialize() 不会再自建 QRhi。
    // 两个场景必须共用同一个 QRhi，FG 才能直接采样 3D 纹理做插值。
    const QQuickGraphicsDevice device = QQuickGraphicsDevice::fromRhi(rhi);
    m_scene.window->setGraphicsDevice(device);
    m_overlay.window->setGraphicsDevice(device);

    // QQuickRenderControl::initialize() 在 Vulkan 下强制要求窗口上有 QVulkanInstance
    // （Qt 内部要查实例级扩展/能力），只给 fromRhi(rhi) 是不够的。
    if (vulkanInstance) {
        m_scene.window->setVulkanInstance(vulkanInstance);
        m_overlay.window->setVulkanInstance(vulkanInstance);
    }

    m_scene.window->setColor(kSceneClearColor);
    // UI 层必须透明清屏，否则 FG 合成时会把整帧盖住。
    m_overlay.window->setColor(Qt::transparent);

    // 脏标记：QML 里任何东西变化时 Qt 都会给离屏窗口投一个 UpdateRequest，
    // 呈现层据此决定"这一步要不要真的渲染"。没有它就只能每帧盲渲。
    m_scene.window->installEventFilter(new DirtyWatcher(&m_contentDirty, m_scene.window.get()));
    m_overlay.window->installEventFilter(new DirtyWatcher(&m_contentDirty, m_overlay.window.get()));

    // 两个窗口都不 show()：RenderControl 下没有原生窗口，show() 会破坏离屏语义。
    if (!m_scene.control->initialize())
        return fail(QStringLiteral("3D 场景 QQuickRenderControl::initialize() 失败"));
    if (!m_overlay.control->initialize())
        return fail(QStringLiteral("UI 场景 QQuickRenderControl::initialize() 失败"));

    // 如果 Qt 没有复用传入的 QRhi（例如图形后端不匹配），两张纹理就不在同一设备上，
    // FG 无法采样。宁可失败回退，也不要跑到合成阶段才炸。
    if (m_scene.control->rhi() != rhi || m_overlay.control->rhi() != rhi)
        return fail(QStringLiteral("QQuickRenderControl 没有复用传入的 QRhi（图形后端可能不匹配）"));

    applyLayerGeometry(m_scene);
    applyLayerGeometry(m_overlay);

    if (!createLayerTargets(m_scene, "3D"))
        return false;
    if (!createLayerTargets(m_overlay, "UI"))
        return false;

    if (!loadLayer(m_scene, engine, scene3dUrl, "3D"))
        return false;
    if (!loadLayer(m_overlay, engine, overlayUrl, "UI"))
        return false;

    m_valid = true;
    qInfo().noquote() << QStringLiteral("FgHost: 双场景宿主就绪，离屏尺寸 %1x%2")
                             .arg(m_size.width())
                             .arg(m_size.height());
    return true;
}

bool FgHost::createLayerTargets(Layer &layer, const char *name)
{
    if (!m_rhi || !layer.window)
        return fail(QStringLiteral("%1 层离屏目标创建时状态无效").arg(QLatin1String(name)));
    if (m_size.isEmpty())
        return fail(QStringLiteral("%1 层离屏目标尺寸为空").arg(QLatin1String(name)));

    // RenderTarget 让纹理能作为颜色附件，UsedAsTransferSource 让 FG 能采样/拷贝它。
    layer.texture = m_rhi->newTexture(QRhiTexture::RGBA8, m_size, 1,
                                      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    layer.texture->setName(QByteArray("fg-") + name + "-color");
    if (!layer.texture->create()) {
        releaseLayerTargets(layer);
        return fail(QStringLiteral("%1 层颜色纹理创建失败（%2x%3）")
                        .arg(QLatin1String(name))
                        .arg(m_size.width())
                        .arg(m_size.height()));
    }

    const QRhiTextureRenderTargetDescription description{QRhiColorAttachment(layer.texture)};
    layer.renderTarget = m_rhi->newTextureRenderTarget(description);
    layer.renderTarget->setName(QByteArray("fg-") + name + "-target");
    layer.renderPass = layer.renderTarget->newCompatibleRenderPassDescriptor();
    if (!layer.renderPass) {
        releaseLayerTargets(layer);
        return fail(QStringLiteral("%1 层 render pass 描述符创建失败").arg(QLatin1String(name)));
    }
    layer.renderTarget->setRenderPassDescriptor(layer.renderPass);
    if (!layer.renderTarget->create()) {
        releaseLayerTargets(layer);
        return fail(QStringLiteral("%1 层 QRhiTextureRenderTarget::create() 失败").arg(QLatin1String(name)));
    }

    // setRenderTarget 只能在渲染线程调用，且不要每帧换；这里只在 start()/resize() 里做。
    layer.window->setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(layer.renderTarget));
    return true;
}

void FgHost::releaseLayerTargets(Layer &layer)
{
    // 先摘掉窗口对 target 的引用，再销毁底层 QRhi 资源，避免窗口持有悬空指针。
    if (layer.window)
        layer.window->setRenderTarget(QQuickRenderTarget());
    delete layer.renderTarget;
    layer.renderTarget = nullptr;
    delete layer.renderPass;
    layer.renderPass = nullptr;
    delete layer.texture;
    layer.texture = nullptr;
}

bool FgHost::loadLayer(Layer &layer, QQmlEngine &engine, const QUrl &url, const char *name)
{
    QQmlComponent component(&engine, url);
    if (component.isError()) {
        for (const QQmlError &error : component.errors())
            qWarning().noquote() << "FgHost:" << name << "QML 错误:" << error.toString();
        return fail(QStringLiteral("%1 层 QML 载入失败：%2").arg(QLatin1String(name), url.toString()));
    }

    QObject *object = component.create();
    if (!object)
        return fail(QStringLiteral("%1 层 QML 根对象创建失败：%2").arg(QLatin1String(name), url.toString()));

    QQuickItem *item = qobject_cast<QQuickItem *>(object);
    if (!item) {
        // 传进来的是 Window 之类的根节点：RenderControl 下没有原生窗口，不能挂载。
        qWarning().noquote() << "FgHost:" << name << "QML 根节点不是 Item，拒绝挂载";
        delete object;
        return fail(QStringLiteral("%1 层 QML 根节点不是 Item：%2").arg(QLatin1String(name), url.toString()));
    }

    item->setParentItem(layer.window->contentItem());
    item->setSize(QSizeF(m_size));
    layer.rootItem = item;
    return true;
}

void FgHost::applyLayerGeometry(Layer &layer)
{
    if (!layer.window)
        return;
    layer.window->resize(m_size);
    if (QQuickItem *content = layer.window->contentItem())
        content->setSize(QSizeF(m_size));
    if (layer.rootItem)
        layer.rootItem->setSize(QSizeF(m_size));
}

bool FgHost::renderLayer(Layer &layer, const char *name)
{
    QQuickRenderControl *control = layer.control.get();
    if (!control || !layer.renderTarget)
        return fail(QStringLiteral("%1 层尚未就绪就调用了 renderFrame()").arg(QLatin1String(name)));

    // 官方推荐的顺序：polishItems() 在 beginFrame() 之前，sync()/render() 必须在
    // beginFrame()/endFrame() 之间（QQuickRenderControl 文档的更新步骤示例）。
    control->polishItems();
    control->beginFrame();
    // beginFrame() 不返回状态；command buffer 为空是最直接的失败信号（例如设备丢失）。
    if (!control->commandBuffer()) {
        control->endFrame();
        return fail(QStringLiteral("%1 层 beginFrame() 没有拿到 command buffer").arg(QLatin1String(name)));
    }
    control->sync();
    control->render();
    control->endFrame();
    return true;
}

bool FgHost::renderFrame()
{
    // 失败原因已经在 fail() 里打印过一次，这里直接返回，避免每帧刷屏。
    if (!m_valid)
        return false;
    if (!renderLayer(m_scene, "3D"))
        return false;
    if (!renderLayer(m_overlay, "UI"))
        return false;
    // 这一帧已经把两层的当前内容都画进纹理了，脏标记到此为止；
    // 之后再有 QML 变化会重新把它置起来。
    m_contentDirty = false;
    return true;
}

QQuickWindow *FgHost::overlayWindow() const
{
    return m_overlay.window.get();
}

QQuickWindow *FgHost::sceneWindow() const
{
    return m_scene.window.get();
}

QQuickItem *FgHost::overlayRootItem() const
{
    return m_overlay.rootItem;
}

void FgHost::resize(QSize size)
{
    if (!m_valid) {
        qWarning().noquote() << "FgHost: resize() 在宿主未就绪时被调用，已忽略";
        return;
    }
    if (size.isEmpty()) {
        qWarning().noquote() << "FgHost: resize() 收到空尺寸，已忽略";
        return;
    }
    if (size == m_size)
        return;

    // 先释放旧目标再建新的：窗口不能指向已销毁的 QRhiRenderTarget。
    releaseLayerTargets(m_scene);
    releaseLayerTargets(m_overlay);
    m_size = size;
    applyLayerGeometry(m_scene);
    applyLayerGeometry(m_overlay);
    if (!createLayerTargets(m_scene, "3D"))
        return; // fail() 已把 m_valid 置 false，调用方应回退并销毁本对象
    if (!createLayerTargets(m_overlay, "UI"))
        return;
    qInfo().noquote() << QStringLiteral("FgHost: 离屏目标已重建为 %1x%2")
                             .arg(m_size.width())
                             .arg(m_size.height());
    // 换了渲染目标，两张纹理的内容都是新的（未定义），必须重画一次。
    m_contentDirty = true;
}

QRhiTexture *FgHost::sceneTexture() const
{
    return m_scene.texture;
}

QRhiTexture *FgHost::overlayTexture() const
{
    return m_overlay.texture;
}

void FgHost::teardown()
{
    // 先解绑 target，再让 Qt 释放场景图，最后销毁自建 QRhi 资源。
    // 逆序销毁（overlay 在前）没有语义依赖，只是让日志顺序与创建顺序对称。
    for (Layer *layer : { &m_overlay, &m_scene }) {
        if (layer->window)
            layer->window->setRenderTarget(QQuickRenderTarget());
        // rhi() 在 initialize() 成功前恒为 null；用它判断"确实初始化过"，
        // 避免在 start() 中途失败时对未初始化的 control 调 invalidate()。
        if (layer->control && layer->control->rhi())
            layer->control->invalidate();
        // 根 Item 的父项是 contentItem，随窗口析构一起销毁，这里只清观察指针。
        layer->rootItem = nullptr;
        layer->window.reset();
        layer->control.reset();
        delete layer->renderTarget;
        layer->renderTarget = nullptr;
        delete layer->renderPass;
        layer->renderPass = nullptr;
        delete layer->texture;
        layer->texture = nullptr;
    }
    m_rhi = nullptr;
    m_valid = false;
}
