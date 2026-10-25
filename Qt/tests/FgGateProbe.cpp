// FG 闸门探针：验证两件决定 FSR3 帧生成架构成败的事实。
//
//   检查 A（方案文档 §5.1）：QRhi::setQueueSubmitParams() 是否对
//   QQuickRenderControl::endFrame() 触发的那次 offscreen 提交生效。
//   不生效就只能退回 QRhi::finish() 硬同步，FG 的流水线收益会被吃光。
//
//   检查 B（方案文档 §4.2）：应用自建的 VkSurfaceKHR（来自可见 QWindow）能否在
//   QRhi 创建的 VkDevice 上建交换链并 acquire。这决定"共享一个 device"这条路线是否成立。
//
// 这个探针不依赖应用代码（检查 B 会链接 VulkanPresenter 的目标源），因此可以单独构建、
// 单独运行；它需要真实 GPU 与显示服务，**不进 CTest 默认集**（CI runner 没有 GPU）。
//
// 退出码：0 两项都通过；10/11 实例或 QRhi 创建失败；20 检查 A 失败；21 检查 A 超时；
//         30 检查 B 失败。标准输出以 "GATE-A:" / "GATE-B:" 前缀给出可 grep 的结论。

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSize>
#include <QTimer>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <QWindow>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <rhi/qrhi.h>

#if RHINE_FFX_VK_AVAILABLE
#include "fg/VulkanPresenter.h"
#endif

namespace {

constexpr int kProbeSize = 64;
constexpr quint64 kSemaphoreTimeoutNs = 2'000'000'000ull; // 2 秒

int fail(int code, const char *message)
{
    std::fprintf(stderr, "GATE: FAIL %s\n", message);
    std::fflush(stderr);
    return code;
}

/// 检查 A：把信号量注入离屏帧的提交，再用带超时的等待确认它真的被 signal。
int probeQueueSubmitParams(QRhi *rhi, QVulkanInstance *instance)
{
    const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    if (!handles || handles->dev == VK_NULL_HANDLE)
        return fail(10, "QRhi 没有给出 VkDevice 原生句柄");

    std::fprintf(stderr, "PROBE: construct control/window\n");
    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGraphicsDevice(QQuickGraphicsDevice::fromRhi(rhi));
    window.setColor(Qt::black);
    window.resize(kProbeSize, kProbeSize);
    std::fprintf(stderr, "PROBE: control initialize\n");
    if (!control.initialize())
        return fail(11, "QQuickRenderControl::initialize() 失败");
    if (control.rhi() != rhi)
        return fail(12, "QQuickRenderControl 没有复用传入的 QRhi");

    QRhiTexture *texture =
        rhi->newTexture(QRhiTexture::RGBA8, QSize(kProbeSize, kProbeSize), 1,
                        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    if (!texture->create())
        return fail(13, "离屏颜色纹理创建失败");
    std::unique_ptr<QRhiTexture> textureGuard(texture);
    std::unique_ptr<QRhiTextureRenderTarget> target(
        rhi->newTextureRenderTarget({QRhiColorAttachment(texture)}));
    // 没有 render pass descriptor 的纹理渲染目标 create() 会在 Qt 内部空指针崩溃。
    QRhiRenderPassDescriptor *renderPass = target->newCompatibleRenderPassDescriptor();
    if (!renderPass)
        return fail(14, "render pass 描述符创建失败");
    target->setRenderPassDescriptor(renderPass);
    if (!target->create())
        return fail(14, "离屏渲染目标创建失败");
    window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));

    std::fprintf(stderr, "PROBE: load qml\n");
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
Item {
    width: 64
    height: 64
    Rectangle { anchors.fill: parent; color: "#d02020" }
})",
                      QUrl(QStringLiteral("qrc:/fg-gate-probe.qml")));
    QObject *root = component.create();
    if (!root)
        return fail(15, "探针 QML 根对象创建失败");
    auto *item = qobject_cast<QQuickItem *>(root);
    if (!item)
        return fail(16, "探针 QML 根节点不是 Item");
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(kProbeSize, kProbeSize));

    QVulkanDeviceFunctions *deviceFunctions = instance->deviceFunctions(handles->dev);
    if (!deviceFunctions)
        return fail(17, "拿不到设备级 Vulkan 函数表");

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (deviceFunctions->vkCreateSemaphore(handles->dev, &semaphoreInfo, nullptr, &semaphore) !=
        VK_SUCCESS)
        return fail(19, "信号量创建失败");

    QRhiVulkanQueueSubmitParams submit{};
    submit.signalSemaphoreCount = 1;
    submit.signalSemaphores = &semaphore;

    std::fprintf(stderr, "PROBE: render frame\n");
    control.polishItems();
    control.beginFrame();
    if (!control.commandBuffer())
        return fail(18, "beginFrame() 没有拿到 command buffer");
    control.sync();
    control.render();
    // 关键：在 endFrame() 之前挂上参数，让 Qt 的下一次 vkQueueSubmit 带上这个信号量。
    rhi->setQueueSubmitParams(&submit);
    control.endFrame();

    std::fprintf(stderr, "PROBE: endFrame done, waiting semaphore\n");
    // vkWaitSemaphores 只能等 timeline 信号量，而 Qt 注入点用的是 binary 信号量；
    // 用 Vulkan 1.0 就能做的办法判定：自己发一次"等待该信号量"的空提交，配一个围栏，
    // 带超时等围栏。围栏在 2 秒内被 signal ⇒ 说明 Qt 的提交确实 signal 了这个信号量。
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (deviceFunctions->vkCreateFence(handles->dev, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        return fail(20, "围栏创建失败");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &semaphore;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    submitInfo.pWaitDstStageMask = &waitStage;
    const VkResult submitted =
        deviceFunctions->vkQueueSubmit(handles->gfxQueue, 1, &submitInfo, fence);
    VkResult waited = VK_ERROR_UNKNOWN;
    if (submitted == VK_SUCCESS)
        waited = deviceFunctions->vkWaitForFences(handles->dev, 1, &fence, VK_TRUE,
                                                  kSemaphoreTimeoutNs);
    deviceFunctions->vkDestroyFence(handles->dev, fence, nullptr);
    deviceFunctions->vkDestroySemaphore(handles->dev, semaphore, nullptr);
    if (waited == VK_SUCCESS) {
        std::printf("GATE-A: PASS setQueueSubmitParams 对 offscreen 提交生效（信号量在 2 秒内被 signal）\n");
        return 0;
    }
    if (waited == VK_TIMEOUT) {
        std::printf("GATE-A: FAIL-TIMEOUT 信号量未被 signal，注入对离屏帧不生效\n");
        return 21;
    }
    std::printf("GATE-A: FAIL vkWaitSemaphores 返回 %d\n", int(waited));
    return 20;
}

#if RHINE_FFX_VK_AVAILABLE
/// 检查 B：用可见 QWindow 的 surface 在共享 device 上建交换链并 acquire/present 一次。
int probeSwapchain(QRhi *rhi, QVulkanInstance *instance, QWindow *window)
{
    VulkanPresenter presenter;
    if (!VulkanPresenter::supported(rhi))
        return fail(29, "VulkanPresenter::supported() 认为当前 QRhi 不可用");

    const QSize extent(640, 360);
    window->resize(extent);
    window->show();
    // Wayland 下 surface 要等窗口真正 expose（提交了一次缓冲）之后才存在，
    // 只 create() 不转事件循环会拿到 VK_NULL_HANDLE。
    QElapsedTimer exposeClock;
    exposeClock.start();
    while (!window->isExposed() && exposeClock.elapsed() < 3000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::fprintf(stderr, "PROBE: exposed=%d\n", int(window->isExposed()));
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(window);
    if (surface == VK_NULL_HANDLE)
        return fail(30, "QWindow 没有产出 VkSurfaceKHR");

    if (!presenter.init(rhi, surface, extent, false)) {
        std::fprintf(stderr, "GATE-B: init 失败：%s\n", qPrintable(presenter.lastError()));
        return fail(31, "VulkanPresenter::init() 失败");
    }

    VulkanPresenter::Frame frame;
    const auto acquired = presenter.acquire(&frame);
    if (acquired != VulkanPresenter::AcquireResult::Ok &&
        acquired != VulkanPresenter::AcquireResult::Suboptimal) {
        std::fprintf(stderr, "GATE-B: acquire 失败：%s\n", qPrintable(presenter.lastError()));
        return fail(32, "交换链 acquire 失败");
    }
    bool recreate = false;
    if (!presenter.present(frame, &recreate)) {
        std::fprintf(stderr, "GATE-B: present 失败：%s\n", qPrintable(presenter.lastError()));
        return fail(33, "交换链 present 失败");
    }
    std::printf("GATE-B: PASS 共享 device 上建链成功（%ux%u，%u 张图，格式 %d）\n",
                unsigned(extent.width()), unsigned(extent.height()), presenter.imageCount(),
                int(presenter.colorFormat()));
    presenter.release();
    Q_UNUSED(instance);
    return 0;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stderr, "PROBE: app\n");
    QGuiApplication application(argc, argv);

    std::fprintf(stderr, "PROBE: instance\n");
    QVulkanInstance instance;
    // vkWaitSemaphores 是 Vulkan 1.2 核心函数；实例版本过低时该函数指针为空。
    // 这里请求 1.3，QVulkanInstance 会按驱动能力自动下调。
    instance.setApiVersion(QVersionNumber(1, 3));
    if (!instance.create()) {
        std::fprintf(stderr, "GATE: 无法创建 QVulkanInstance（%s）\n",
                     qPrintable(instance.errorCode() ? QString::number(instance.errorCode())
                                                    : QStringLiteral("无错误码")));
        return 10;
    }

    std::fprintf(stderr, "PROBE: instance created\n");
    QRhiVulkanInitParams params;
    params.inst = &instance;
    // 关键：让 QRhi 建 device 时就启用交换链扩展。应用的呈现层要在这同一个 device 上
    // 创建自己的 VkSwapchainKHR，反过来补扩展是做不到的。
    params.deviceExtensions = {QByteArrayLiteral("VK_KHR_swapchain")};
    std::fprintf(stderr, "PROBE: creating rhi\n");
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Vulkan, &params, {}));
    if (!rhi) {
        std::fprintf(stderr, "GATE: QRhi::create(Vulkan) 失败\n");
        return 11;
    }
    const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    std::fprintf(stderr, "PROBE: handles=%p dev=%p physDev=%p queue=%p\n",
                 static_cast<const void *>(handles),
                 reinterpret_cast<void *>(handles ? handles->dev : VK_NULL_HANDLE),
                 reinterpret_cast<void *>(handles ? handles->physDev : VK_NULL_HANDLE),
                 reinterpret_cast<void *>(handles ? handles->gfxQueue : VK_NULL_HANDLE));
    std::printf("GATE: device=%p physDev=%p queueFamily=%u queue=%p\n",
                reinterpret_cast<void *>(handles ? handles->dev : VK_NULL_HANDLE),
                reinterpret_cast<void *>(handles ? handles->physDev : VK_NULL_HANDLE),
                handles ? handles->gfxQueueFamilyIdx : 0u,
                reinterpret_cast<void *>(handles ? handles->gfxQueue : VK_NULL_HANDLE));

    int result = 0;
    const int gateA = probeQueueSubmitParams(rhi.get(), &instance);
    if (gateA != 0)
        result = gateA;

#if RHINE_FFX_VK_AVAILABLE
    QWindow window;
    window.setVulkanInstance(&instance);
    window.setTitle(QStringLiteral("Rhine FG gate probe"));
    window.setGeometry(0, 0, 640, 360);
    // 必须声明成 Vulkan surface：平台窗口默认按 raster/OpenGL 建，那样
    // surfaceForWindow() 永远返回 VK_NULL_HANDLE。要在 create() 之前设置。
    window.setSurfaceType(QSurface::VulkanSurface);
    window.create();
    const int gateB = probeSwapchain(rhi.get(), &instance, &window);
    if (gateB != 0 && result == 0)
        result = gateB;
    window.destroy();
#else
    std::printf("GATE-B: SKIP 该构建没有编入 VulkanPresenter\n");
#endif

    std::printf("GATE: done result=%d\n", result);
    return result;
}
