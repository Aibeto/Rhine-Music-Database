#include "Fsr3Backend.h"
#include "FsrSharpness.h"

#include <QDebug>
#include <QLibrary>
#include <QSize>

#include <cstring>

#include <rhi/qrhi.h>

#include <vector>

// 两个后端宏可以同时成立：Windows 上 Vulkan 与 D3D12 都可能可用，而 supported()
// 对两者都返回 true。所以这里是两段独立的 #if 而不是 #elif——只在编译期留一个后端，
// 会让运行期选中另一个后端时去解引用错误的 nativeHandles 结构（未定义行为）。
#if defined(RHINE_FSR3_VULKAN)
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#include <vulkan/vulkan.h>
#endif

#if defined(RHINE_FSR3_DX12)
// SDK 1.1.4 的 ffx_vk.h 与 ffx_dx12.h 都无条件定义了同一个
// enum FfxFrameInterpolationSwapchainConfigureKey（连同它的两个枚举量），而 Windows 上
// RHINE_FSR3_VULKAN 与 RHINE_FSR3_DX12 可以同时成立，同一个 TU 里直接包含两个头会报
// 重定义。这里在包含 DX12 头之前把这三个标识符改个名字：改名在头文件内部是一致的，
// 我们从不使用它们，#undef 之后也不影响任何其它代码。将来 SDK 修掉这个重名后，这几个
// 宏退化成无害的空操作。
#define FfxFrameInterpolationSwapchainConfigureKey FfxFrameInterpolationSwapchainConfigureKeyDX12
#define FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK_DX12
#define FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING_DX12
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#undef FfxFrameInterpolationSwapchainConfigureKey
#undef FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK
#undef FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING

#include <d3d12.h>
#endif

#if defined(RHINE_FSR3_VULKAN) || defined(RHINE_FSR3_DX12)
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#endif

namespace {

#if defined(RHINE_FSR3_VULKAN)

// FFX 的 Vulkan 后端用 vkGetDeviceProcAddr 按"扩展名"去查
// vkGetBufferMemoryRequirements2KHR，然后在 CreateBackendContextVK 里直接调用它。
// 但 Qt 创建 VkDevice 时没有启用 VK_KHR_get_memory_requirements2，按扩展名查询会得到
// 空指针，实测就是这一步 SIGSEGV（ffx_vk.cpp:1663）。该功能在 Vulkan 1.1 已并入核心，
// KHR 与核心版本的函数签名、结构体定义完全相同，所以把别名指向核心名字即可。
PFN_vkGetDeviceProcAddr g_realGetDeviceProcAddr = nullptr;
PFN_vkVoidFunction fsr3GetDeviceProcAddr(VkDevice device, const char* name)
{
    if (!g_realGetDeviceProcAddr)
        return nullptr;
    if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0)
        name = "vkGetBufferMemoryRequirements2";
    return g_realGetDeviceProcAddr(device, name);
}

VkFormat toVkFormat(QRhiTexture::Format format)
{
    switch (format)
    {
    case QRhiTexture::RGBA8:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case QRhiTexture::BGRA8:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case QRhiTexture::RGBA16F:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case QRhiTexture::RGBA32F:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case QRhiTexture::R16F:
        return VK_FORMAT_R16_SFLOAT;
    case QRhiTexture::R32F:
        return VK_FORMAT_R32_SFLOAT;
    case QRhiTexture::D16:
        return VK_FORMAT_D16_UNORM;
    case QRhiTexture::D24:
        return VK_FORMAT_X8_D24_UNORM_PACK32;
    case QRhiTexture::D24S8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case QRhiTexture::D32F:
        return VK_FORMAT_D32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

/// Wrap an existing QRhi texture as an FfxResource. FSR3 does not take ownership: the
/// QRhi texture stays alive and keeps its own layout tracking.
FfxResource wrapTexture(QRhiTexture *texture, FfxResourceUsage usage, const wchar_t *name,
                        FfxResourceStates state)
{
    const QRhiTexture::NativeTexture native = texture->nativeTexture();
    const VkImage image = reinterpret_cast<VkImage>(quintptr(native.object));

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = toVkFormat(texture->format());
    info.extent = { quint32(texture->pixelSize().width()), quint32(texture->pixelSize().height()), 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // info.usage 决定 FFX 为这个资源创建哪些视图：ffx_vk.cpp 的
    // ffxGetImageResourceDescriptionVK 只要看到 STORAGE_BIT 就打上 UAV 标志。
    // 之前对所有资源都写 SAMPLED|STORAGE，等于把深度图和只读颜色图也当成 UAV，
    // FFX 会为它们创建非法的 storage 视图，随后在 ExecuteGpuJobsVK 里崩在驱动内部。
    // 这里按实际用途推导：只有输出图需要 STORAGE。
    VkImageUsageFlags vkUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & FFX_RESOURCE_USAGE_UAV)
        vkUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & FFX_RESOURCE_USAGE_DEPTHTARGET)
        vkUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & FFX_RESOURCE_USAGE_RENDERTARGET)
        vkUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.usage = vkUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const FfxResourceDescription description = ffxGetImageResourceDescriptionVK(image, info, usage);
    return ffxGetResourceVK(reinterpret_cast<void *>(image), description, name, state);
}

/// 把 QRhi 记录的 VkImageLayout 翻译成 FfxResourceStates。
///
/// FSR3 的 Vulkan 后端只在"当前状态与目标状态不同"时才排屏障（ffx_vk.cpp 的
/// ffxVulkanBarrier 调度逻辑），所以这里必须报真实布局：报错会让一次必要的布局转换
/// 被静默跳过，计算着色器就可能读到还没落盘的内容——表现为偶发花屏或整帧变黑，
/// 且只在特定帧序下复现。
///
/// layout 为 0（VK_IMAGE_LAYOUT_UNDEFINED，QRhi 还没跟踪过这张纹理）时退回 COMMON：
/// COMMON 不满足任何目标状态，屏障一定会发，代价只是多几次转换，功能仍然正确。
FfxResourceStates currentVkState(QRhiTexture *texture)
{
    switch (texture->nativeTexture().layout)
    {
    case VK_IMAGE_LAYOUT_GENERAL:
        return FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return FFX_RESOURCE_STATE_RENDER_TARGET;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        return FFX_RESOURCE_STATE_COMPUTE_READ;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return FFX_RESOURCE_STATE_COPY_SRC;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return FFX_RESOURCE_STATE_COPY_DEST;
    default:
        return FFX_RESOURCE_STATE_COMMON;
    }
}

#endif  // RHINE_FSR3_VULKAN

#if defined(RHINE_FSR3_DX12)

/// 把 QRhi 记录的 D3D12_RESOURCE_STATES 位组合翻译成 FFX 的状态枚举。
///
/// 这个值必须尽量准确：SDK 在 RegisterResourceDX12 时把声明的状态当成资源的真实状态，
/// 而它的屏障逻辑是"当前状态已经满足目标状态就不发屏障"。声明成固定值（比如统一
/// COMPUTE_READ）会让 RT/DEPTH_WRITE -> SRV 的转换被静默跳过，计算着色器读到的是
/// 还没落盘的内容。QRhi 文档里 setNativeLayout() 的 layout 在 D3D12 下就是这些位，
/// nativeTexture() 是它的反向操作。
///
/// 认不出来的组合退回 COMMON：它是资源创建时的状态，也是唯一允许"未知"的声明。
FfxResourceStates toFfxStateFromNative(int nativeState)
{
    const int state = nativeState;
    if (state == D3D12_RESOURCE_STATE_COMMON)
        return FFX_RESOURCE_STATE_COMMON;
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    if (state & D3D12_RESOURCE_STATE_RENDER_TARGET)
        return FFX_RESOURCE_STATE_RENDER_TARGET;
    // FFX 只有一个深度状态 DEPTH_ATTACHEMENT（映射到 D3D12_RESOURCE_STATE_DEPTH_WRITE）。
    // 只读深度也归到这里，因为它是唯一能让 SDK 从深度写状态起手的取值。
    if (state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ))
        return FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT;
    const bool nonPixelRead = (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    const bool pixelRead = (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
    if (nonPixelRead && pixelRead)
        return FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (nonPixelRead)
        return FFX_RESOURCE_STATE_COMPUTE_READ;
    if (pixelRead)
        return FFX_RESOURCE_STATE_PIXEL_READ;
    if (state & D3D12_RESOURCE_STATE_COPY_DEST)
        return FFX_RESOURCE_STATE_COPY_DEST;
    if (state & D3D12_RESOURCE_STATE_COPY_SOURCE)
        return FFX_RESOURCE_STATE_COPY_SRC;
    if (state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
        return FFX_RESOURCE_STATE_INDIRECT_ARGUMENT;
    return FFX_RESOURCE_STATE_COMMON;
}

/// Wrap an existing QRhi texture as an FfxResource. FSR3 does not take ownership: the
/// QRhi texture stays alive and keeps its own state tracking.
///
/// D3D12 这边不需要像 Vulkan 那样重建一份 VkImageCreateInfo——FFX 直接用
/// ffxGetResourceDescriptionDX12 从 ID3D12Resource::GetDesc() 推描述。
///
/// usage 的取值与 Vulkan 分支保持一致；SDK 会把它和格式推导出的标志按位或，
/// 而 FFX_RESOURCE_USAGE_READ_ONLY 的值是 0，所以重复声明是幂等的。
///
/// 状态不作为参数传入，而是从 QRhi 的跟踪里读——D3D12 的屏障必须知道资源真实处于
/// 什么状态，而只有 QRhi 知道（见 toFfxStateFromNative）。
FfxResource wrapTextureDX12(QRhiTexture *texture, FfxResourceUsage usage, const wchar_t *name)
{
    const QRhiTexture::NativeTexture native = texture->nativeTexture();
    auto *resource = reinterpret_cast<ID3D12Resource *>(quintptr(native.object));
    if (!resource)
        return {};

    return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource, usage), name,
                              toFfxStateFromNative(native.layout));
}

#endif  // RHINE_FSR3_DX12

#if defined(RHINE_FSR3_VULKAN) || defined(RHINE_FSR3_DX12)

void fsr3Message(FfxMsgType type, const wchar_t *message)
{
    const QString text = message ? QString::fromWCharArray(message) : QStringLiteral("(no message)");
    if (type == FFX_MESSAGE_TYPE_ERROR)
        qWarning().noquote() << "FSR3 error:" << text;
    else
        qInfo().noquote() << "FSR3:" << text;
}

#endif  // RHINE_FSR3_VULKAN || RHINE_FSR3_DX12

}  // namespace

struct Fsr3Backend::Private
{
    QRhi *rhi = nullptr;
    QSize renderSize;
    QSize displaySize;
    bool contextCreated = false;
    bool reset = true;

    float nearPlane = 0.1f;
    float farPlane = 5000.0f;
    float frameTimeMs = 16.6667f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // FfxInterface / FfxFsr3UpscalerContext / scratch 都是后端无关的公共结构，两个后端
    // 共用同一份：同一时刻只会有一个后端被 resize() 建立起来。
#if defined(RHINE_FSR3_VULKAN) || defined(RHINE_FSR3_DX12)
    FfxInterface backendInterface{};
    std::vector<quint8> scratch;
    FfxFsr3UpscalerContext context{};
#endif
};

Fsr3Backend::Fsr3Backend() : d(new Private) {}

Fsr3Backend::~Fsr3Backend()
{
    release();
}

bool Fsr3Backend::compiledIn()
{
#if defined(RHINE_FSR3_VULKAN) || defined(RHINE_FSR3_DX12)
    return true;
#else
    return false;
#endif
}

bool Fsr3Backend::supported(QRhi *rhi)
{
    if (!rhi)
        return false;
#if defined(RHINE_FSR3_VULKAN)
    if (rhi->backend() == QRhi::Vulkan)
        return true;
#endif
#if defined(RHINE_FSR3_DX12)
    if (rhi->backend() == QRhi::D3D12)
        return true;
#endif
    return false;
}

bool Fsr3Backend::available() const
{
    return d->contextCreated;
}

bool Fsr3Backend::resize(QRhi *rhi, QSize renderSize, QSize displaySize)
{
#if !defined(RHINE_FSR3_VULKAN) && !defined(RHINE_FSR3_DX12)
    Q_UNUSED(rhi)
    Q_UNUSED(renderSize)
    Q_UNUSED(displaySize)
    return false;
#else
    if (!supported(rhi) || !renderSize.isValid() || !displaySize.isValid())
        return false;

    // FFX 的 Vulkan 计算后端在本项目实测会在 Intel ANV 的 ExecuteGpuJobsVK 里崩溃
    // （SIGSEGV，栈顶在 libvulkan_intel.so），所以默认不创建它的上下文：FSR3 档位
    // 改走 RhiScene 里 FSR1 的 EASU/RCAS 片元路径，超分与锐化照常生效，只是没有
    // 时域累积。需要继续验证 FFX 计算路径时设 RHINE_FSR3_FFX=1。
    if (rhi->backend() == QRhi::Vulkan && qEnvironmentVariable("RHINE_FSR3_FFX") != "1")
        return false;

    if (d->contextCreated && d->rhi == rhi && d->renderSize == renderSize && d->displaySize == displaySize)
        return true;

    // Context recreation frees GPU resources, so it must not happen mid-frame. Callers
    // invoke this from the item's resize path, outside the render pass.
    release();

    // 只有"建立后端接口"这一步与后端相关；后面的 context 描述两个后端完全一致。
    FfxDevice device = nullptr;

#if defined(RHINE_FSR3_DX12)
    if (rhi->backend() == QRhi::D3D12)
    {
        const auto *handles = static_cast<const QRhiD3D12NativeHandles *>(rhi->nativeHandles());
        if (!handles || !handles->dev)
            return false;

        // QRhiD3D12NativeHandles 用 void* 是为了不把 d3d12.h 带进公共头，实际类型是
        // ID3D12Device*。ffxGetDeviceDX12 只做一次 reinterpret_cast，不接管引用计数。
        device = ffxGetDeviceDX12(static_cast<ID3D12Device *>(handles->dev));
        if (!device)
            return false;

        // DX12 的 scratch 大小与设备无关（Vulkan 那边要按物理设备能力算），单个 context
        // 就够：Rhine 同时只持有一个 FSR3 实例。
        const size_t scratchSize = ffxGetScratchMemorySizeDX12(1);
        d->scratch.assign(scratchSize, 0);
        if (ffxGetInterfaceDX12(&d->backendInterface, device, d->scratch.data(), d->scratch.size(), 1)
            != FFX_OK)
        {
            d->backendInterface = {};
            d->scratch.clear();
            return false;
        }
    }
#endif  // RHINE_FSR3_DX12

#if defined(RHINE_FSR3_VULKAN)
    if (rhi->backend() == QRhi::Vulkan)
    {
        const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
        if (!handles || handles->dev == VK_NULL_HANDLE || handles->physDev == VK_NULL_HANDLE)
            return false;

        VkDeviceContext deviceContext{};
        deviceContext.vkDevice = handles->dev;
        deviceContext.vkPhysicalDevice = handles->physDev;
        // FFX 的 VK 后端在 vkDeviceProcAddr 为空时会退回它自己的全局变量，而那个变量
        // 只有定义了 FFX_VK_FUNCTION_LIST 才会被填；本项目没定义，所以它恒为 0，
        // 进而在 CreateBackendContextVK 里对空指针发一次调用——实测 SIGSEGV。
        // 所以必须自己保证这里有一个可用的入口，不能指望 Qt 一定填了 inst
        // （qrhi_platform.h:93 注明 inst 仅用于查询，不保证非空）。
        PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
        if (handles->inst)
        {
            getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                handles->inst->getInstanceProcAddr("vkGetDeviceProcAddr"));
        }
        if (!getDeviceProcAddr)
        {
            // 兜底：从已加载的 Vulkan loader 取全局入口。它是一层跳板，
            // 对任何由同一个 loader 创建的 VkDevice 都有效。
            if (const auto symbol = QLibrary::resolve(QStringLiteral("vulkan"), 1, "vkGetDeviceProcAddr"))
                getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(symbol);
            else if (const auto fallback = QLibrary::resolve(QStringLiteral("vulkan-1"), "vkGetDeviceProcAddr"))
                getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fallback);
        }
        if (!getDeviceProcAddr)
        {
            qWarning() << "FSR3: 无法解析 vkGetDeviceProcAddr，Vulkan 下跳过 FSR3";
            return false;
        }
        // 交给 FFX 的是映射过的入口：扩展别名查不到时退回核心名字（见文件顶部说明）。
        g_realGetDeviceProcAddr = getDeviceProcAddr;
        deviceContext.vkDeviceProcAddr = &fsr3GetDeviceProcAddr;

        device = ffxGetDeviceVK(&deviceContext);
        if (!device)
            return false;

        const size_t scratchSize = ffxGetScratchMemorySizeVK(handles->physDev, 1);
        d->scratch.assign(scratchSize, 0);
        if (ffxGetInterfaceVK(&d->backendInterface, device, d->scratch.data(), d->scratch.size(), 1)
            != FFX_OK)
        {
            d->scratch.clear();
            return false;
        }
    }
#endif  // RHINE_FSR3_VULKAN

    // supported() 已经保证后端是这两者之一，走到这里说明 nativeHandles() 没给全。
    if (!device)
        return false;

    FfxFsr3UpscalerContextDescription description{};
    // 自动曝光关闭：Rhine 自己已经做过色调映射，再叠一层依赖历史的增益会漂。
    // HDR 标志也必须关：它的含义是"输入颜色是高动态范围"（ffx_fsr3upscaler.h:133），
    // 而 RhiScene 交给 FSR3 的是 finalFragment 做完 outputColor(aces(...)) 之后的
    // 显示参考 LDR 数据，开 HDR 会让 FSR3 按 HDR 语义处理 LDR 输入，色调和锐度都偏。
    // 深度非反向（Matrix::perspective 是标准 OpenGL 式透视，经 clipSpaceCorrMatrix
    // 之后是 0=near、1=far），速度未叠抖动，所以另外两个标志同样不置位。
    description.flags = 0;
    description.maxRenderSize = { quint32(renderSize.width()), quint32(renderSize.height()) };
    description.maxUpscaleSize = { quint32(displaySize.width()), quint32(displaySize.height()) };
    description.fpMessage = &fsr3Message;
    description.backendInterface = d->backendInterface;

    const FfxErrorCode result = ffxFsr3UpscalerContextCreate(&d->context, &description);
    if (result != FFX_OK)
    {
        qWarning() << "FSR3: ffxFsr3UpscalerContextCreate failed with" << int(result);
        d->backendInterface = {};
        d->scratch.clear();
        return false;
    }

    d->rhi = rhi;
    d->renderSize = renderSize;
    d->displaySize = displaySize;
    d->contextCreated = true;
    d->reset = true;
    return true;
#endif
}

void Fsr3Backend::release()
{
#if defined(RHINE_FSR3_VULKAN) || defined(RHINE_FSR3_DX12)
    if (d->contextCreated)
    {
        ffxFsr3UpscalerContextDestroy(&d->context);
        d->context = {};
    }
    d->backendInterface = {};
    d->scratch.clear();
    d->scratch.shrink_to_fit();
#endif
    d->contextCreated = false;
    d->rhi = nullptr;
    d->renderSize = QSize();
    d->displaySize = QSize();
    d->reset = true;
}

void Fsr3Backend::requestReset()
{
    d->reset = true;
}

void Fsr3Backend::setCamera(float nearPlane, float farPlane, float frameTimeMs)
{
    d->nearPlane = nearPlane > 0.0f ? nearPlane : 0.1f;
    d->farPlane = farPlane > d->nearPlane ? farPlane : d->nearPlane + 1.0f;
    d->frameTimeMs = frameTimeMs > 0.0f ? frameTimeMs : 16.6667f;
}

void Fsr3Backend::setJitter(float x, float y)
{
    d->jitterX = x;
    d->jitterY = y;
}

bool Fsr3Backend::dispatch(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *color,
                           QRhiTexture *depth, QRhiTexture *velocity, QRhiTexture *output)
{
#if !defined(RHINE_FSR3_VULKAN) && !defined(RHINE_FSR3_DX12)
    Q_UNUSED(rhi)
    Q_UNUSED(commandBuffer)
    Q_UNUSED(color)
    Q_UNUSED(depth)
    Q_UNUSED(velocity)
    Q_UNUSED(output)
    return false;
#else
    if (!d->contextCreated || !rhi || !commandBuffer || !color || !depth || !velocity || !output)
        return false;

    if (qEnvironmentVariableIsSet("RHINE_DEBUG_FSR")) {
        // 布局必须真实：FFX 只在"当前状态与目标状态不同"时排屏障，报错会让一次必要的
        // 布局转换被静默跳过，计算着色器读到错误布局时通常表现为驱动内部崩溃。
        qWarning() << "FSR3 dispatch: color" << color->pixelSize() << color->format()
                   << int(color->nativeTexture().layout) << "depth" << depth->pixelSize()
                   << depth->format() << int(depth->nativeTexture().layout) << "velocity"
                   << velocity->pixelSize() << velocity->format()
                   << int(velocity->nativeTexture().layout) << "output" << output->pixelSize()
                   << output->format() << int(output->nativeTexture().layout) << "render"
                   << d->renderSize << "display" << d->displaySize;
    }

    // 后端相关的只有命令列表和四个资源包装；下面那些字段两个后端逐字一致。
    FfxCommandList commandList = nullptr;
    FfxResource colorResource{};
    FfxResource depthResource{};
    FfxResource velocityResource{};
    FfxResource outputResource{};

#if defined(RHINE_FSR3_DX12)
    if (rhi->backend() == QRhi::D3D12)
    {
        const auto *bufferHandles =
            static_cast<const QRhiD3D12CommandBufferNativeHandles *>(commandBuffer->nativeHandles());
        if (!bufferHandles || !bufferHandles->commandList)
            return false;

        // 这条就是 QRhi 当前正在记录的命令列表（beginExternal/endExternal 之间不会另起
        // 一条），FSR3 的计算命令按顺序排进本帧，不需要额外的队列或围栏。
        commandList = ffxGetCommandListDX12(static_cast<ID3D12CommandList *>(bufferHandles->commandList));
        colorResource = wrapTextureDX12(color, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fsr3.Color");
        depthResource = wrapTextureDX12(depth, FFX_RESOURCE_USAGE_DEPTHTARGET, L"Rhine.Fsr3.Depth");
        velocityResource = wrapTextureDX12(velocity, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fsr3.Velocity");
        outputResource = wrapTextureDX12(output, FFX_RESOURCE_USAGE_UAV, L"Rhine.Fsr3.Output");
    }
#endif  // RHINE_FSR3_DX12

#if defined(RHINE_FSR3_VULKAN)
    if (rhi->backend() == QRhi::Vulkan)
    {
        const auto *bufferHandles =
            static_cast<const QRhiVulkanCommandBufferNativeHandles *>(commandBuffer->nativeHandles());
        if (!bufferHandles || bufferHandles->commandBuffer == VK_NULL_HANDLE)
            return false;

        commandList = ffxGetCommandListVK(bufferHandles->commandBuffer);
        colorResource = wrapTexture(color, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fsr3.Color",
                                    currentVkState(color));
        depthResource = wrapTexture(depth, FFX_RESOURCE_USAGE_DEPTHTARGET, L"Rhine.Fsr3.Depth",
                                    currentVkState(depth));
        velocityResource = wrapTexture(velocity, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fsr3.Velocity",
                                       currentVkState(velocity));
        outputResource = wrapTexture(output, FFX_RESOURCE_USAGE_UAV, L"Rhine.Fsr3.Output",
                                     currentVkState(output));
    }
#endif  // RHINE_FSR3_VULKAN

    // 纹理拿不到原生句柄时（nativeTexture() 返回空）FSR3 会收到空资源，放弃这一帧。
    if (!commandList || !colorResource.resource || !depthResource.resource || !velocityResource.resource
        || !outputResource.resource)
        return false;

    FfxFsr3UpscalerDispatchDescription description{};
    description.commandList = commandList;
    description.color = colorResource;
    description.depth = depthResource;
    description.motionVectors = velocityResource;
    description.output = outputResource;

    description.jitterOffset = { d->jitterX, d->jitterY };
    // Rhine's velocity pass writes normalised device coordinates, so scaling by the render
    // extent converts to the pixel deltas FSR3 expects.
    description.motionVectorScale = { float(d->renderSize.width()), float(d->renderSize.height()) };
    description.renderSize = { quint32(d->renderSize.width()), quint32(d->renderSize.height()) };
    description.upscaleSize = { quint32(d->displaySize.width()), quint32(d->displaySize.height()) };
    // sharpness 在 dispatch 描述里，逐帧生效，不需要重建 context。
    // 语义以 ffx_fsr3upscaler.h 为准：0 = 不额外锐化，1 = 最强锐化（与 FSR1 RCAS 的"档"相反）。
    const int sharpnessPercent = qEnvironmentVariableIsEmpty("RHINE_FSR_SHARPNESS")
                                     ? 50
                                     : qBound(0, qEnvironmentVariableIntValue("RHINE_FSR_SHARPNESS"), 100);
    // 与 FSR1 RCAS 同一条规则：放大倍数越高，锐化越容易在边缘留下亮环与散点，
    // 所以按实际放大倍率（renderSize -> displaySize）衰减有效强度。
    const int effectiveSharpness =
        RhineFsr::effectiveSharpnessPercent(sharpnessPercent, d->renderSize, d->displaySize);
    description.enableSharpening = effectiveSharpness > 0;
    description.sharpness = float(effectiveSharpness) / 100.f;
    description.frameTimeDelta = d->frameTimeMs;
    description.preExposure = 1.0f;
    description.reset = d->reset;
    description.cameraNear = d->nearPlane;
    description.cameraFar = d->farPlane;
    // 这个字段只有 frame interpolation 与 FSR2 会读（见 ffx_frameinterpolation.cpp:792、
    // ffx_fsr2.cpp:314），FSR3 upscaler 不消费它，所以不必为此把 FOV 从投影矩阵一路传进来。
    // 保留 1.0 只是为了满足 FSR2 风格的 "> 0 且 <= PI" 校验，免得将来接 FG 时被静默拒绝。
    description.cameraFovAngleVertical = 1.0f;
    description.viewSpaceToMetersFactor = 1.0f;

    const FfxErrorCode result = ffxFsr3UpscalerContextDispatch(&d->context, &description);
    d->reset = false;
    if (result != FFX_OK)
    {
        qWarning() << "FSR3: dispatch failed with" << int(result);
        return false;
    }
    return true;
#endif
}
