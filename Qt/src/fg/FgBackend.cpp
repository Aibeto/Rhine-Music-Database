#include "FgBackend.h"

#include "FgConfig.h"

#include <QDebug>
#include <QSize>
#include <QVector3D>

#include <vector>

#include <rhi/qrhi.h>

#if defined(RHINE_FSR3_VULKAN)
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

#include <vulkan/vulkan.h>
#endif

namespace {

#if defined(RHINE_FSR3_VULKAN)

// OF 建上下文时用的最小块大小（ffx_opticalflow.cpp:299 的 minBlockSize = 8）。
// FI 的 opticalFlowBlockSize 必须与它一致，否则光流向量场按错误的块尺寸采样，
// 表现为插值帧出现整块整块的错位。
constexpr int kOpticalFlowBlockSize = 8;

/**
 * 本模块用到的 Vulkan 入口。
 *
 * 为什么全部走 vkGetInstanceProcAddr/vkGetDeviceProcAddr 而不是直接调全局符号：
 * 这样 Qt/src/fg/ 不引入任何新的链接依赖，CMake 侧只需要把源文件加进目标，
 * 不需要额外链 Vulkan。同时 vkGetDeviceProcAddr 对未启用的扩展返回空指针，
 * 正好可以当"设备有没有开 VK_KHR_swapchain"的探测手段。
 */
struct VkFns
{
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
};

/// 自己持有的裸 VkImage。FG 的内部资源（共享面、光流输出）不经过 QRhi：
/// 它们的格式里 R16G16_SINT 在 QRhiTexture 里没有对应项，混用两套生命周期
/// 反而更容易在 resize 时漏释放。
struct RawImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};

    bool valid() const { return image != VK_NULL_HANDLE; }
};

void fgMessage(uint32_t type, const wchar_t *message)
{
    const QString text = message ? QString::fromWCharArray(message) : QStringLiteral("(no message)");
    if (type == FFX_MESSAGE_TYPE_ERROR)
        qWarning().noquote() << "FG error:" << text;
    else
        qInfo().noquote() << "FG:" << text;
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

FfxSurfaceFormat toFfxFormat(QRhiTexture::Format format)
{
    switch (format)
    {
    case QRhiTexture::RGBA8:
        return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case QRhiTexture::BGRA8:
        return FFX_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case QRhiTexture::RGBA16F:
        return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case QRhiTexture::RGBA32F:
        return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case QRhiTexture::R16F:
        return FFX_SURFACE_FORMAT_R16_FLOAT;
    case QRhiTexture::R32F:
        return FFX_SURFACE_FORMAT_R32_FLOAT;
    default:
        return FFX_SURFACE_FORMAT_UNKNOWN;
    }
}

/// FfxCreateResourceDescription 里的格式是 SDK 的枚举，这里翻回 Vulkan 格式，
/// 用于按 SDK 给的尺寸/格式分配共享资源——比自己抄一遍常量更不容易写错。
VkFormat toVkFormat(FfxSurfaceFormat format)
{
    switch (format)
    {
    case FFX_SURFACE_FORMAT_R32_FLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT:
        return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT:
        return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R16G16_SINT:
        return VK_FORMAT_R16G16_SINT;
    case FFX_SURFACE_FORMAT_R16G16_UINT:
        return VK_FORMAT_R16G16_UINT;
    case FFX_SURFACE_FORMAT_R8_UINT:
        return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R8G8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
    case FFX_SURFACE_FORMAT_R16_FLOAT:
        return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16_UINT:
        return VK_FORMAT_R16_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

/// 把 QRhi 记录的 VkImageLayout 翻译成 FfxResourceStates。
///
/// 这个值必须报真实布局：SDK 的 Vulkan 后端只在"当前状态与目标状态不同"时排屏障，
/// 报错会让一次必要的布局转换被静默跳过，计算着色器可能读到还没落盘的内容——
/// 表现为偶发花屏或整帧变黑，且只在特定帧序下复现。
///
/// layout 为 0（QRhi 还没跟踪过这张纹理）时退回 COMMON：COMMON 不满足任何目标状态，
/// 屏障一定会发，代价只是多几次转换，功能仍然正确。
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

/// FfxResourceStates -> VkImageLayout。与 ffx_vk.cpp 的
/// getVKImageLayoutFromResourceState 一一对应：SDK 在 UnregisterResourcesVK 里把
/// 动态资源屏障回它声明的初始状态，所以 effect 结束后资源的真实布局就是这个映射的
/// 结果。QRhi 的布局跟踪必须按同一张表还原，否则下一次 QRhi 采样会从错误的旧布局
/// 起手，少发一次必要的转换。
VkImageLayout layoutForFfxState(FfxResourceStates state)
{
    switch (state)
    {
    case FFX_RESOURCE_STATE_COMPUTE_READ:
    case FFX_RESOURCE_STATE_PIXEL_READ:
    case FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case FFX_RESOURCE_STATE_COPY_SRC:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case FFX_RESOURCE_STATE_COPY_DEST:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case FFX_RESOURCE_STATE_RENDER_TARGET:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case FFX_RESOURCE_STATE_DEPTH_ATTACHEMENT:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case FFX_RESOURCE_STATE_PRESENT:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    default:
        return VK_IMAGE_LAYOUT_GENERAL;
    }
}

/// 把 QRhi 纹理包成 FfxResource。FG 不接管所有权：QRhi 纹理继续由 QRhi 管理。
/// storage 表示这张图会不会被当成 UAV 写：深度图只读，传 false 可以避免给
/// 深度资源挂上 UAV 用法（Vulkan 下 D32F 的 STORAGE 用法并不普遍支持）。
FfxResource wrapTexture(QRhiTexture *texture, FfxResourceUsage usage, const wchar_t *name,
                        FfxResourceStates state, bool storage = true)
{
    if (!texture)
        return {};

    const QRhiTexture::NativeTexture native = texture->nativeTexture();
    const VkImage image = reinterpret_cast<VkImage>(quintptr(native.object));
    if (image == VK_NULL_HANDLE)
        return {};

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = toVkFormat(texture->format());
    info.extent = {quint32(texture->pixelSize().width()), quint32(texture->pixelSize().height()), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storage)
        info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const FfxResourceDescription description = ffxGetImageResourceDescriptionVK(image, info, usage);
    return ffxGetResourceVK(reinterpret_cast<void *>(image), description, name, state);
}

FfxResource wrapRawImage(const RawImage &raw, FfxResourceUsage usage, const wchar_t *name,
                         FfxResourceStates state)
{
    if (!raw.valid())
        return {};

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = raw.format;
    info.extent = {raw.extent.width, raw.extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const FfxResourceDescription description = ffxGetImageResourceDescriptionVK(raw.image, info, usage);
    return ffxGetResourceVK(reinterpret_cast<void *>(raw.image), description, name, state);
}

quint32 findDeviceLocalMemoryType(const VkPhysicalDeviceMemoryProperties &props, quint32 typeBits)
{
    for (quint32 i = 0; i < props.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) == 0)
            continue;
        if ((props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
            return i;
    }
    return UINT32_MAX;
}

void destroyRawImage(VkDevice device, const VkFns &fns, RawImage *image)
{
    if (!image)
        return;
    if (image->image != VK_NULL_HANDLE && fns.destroyImage)
        fns.destroyImage(device, image->image, nullptr);
    if (image->memory != VK_NULL_HANDLE && fns.freeMemory)
        fns.freeMemory(device, image->memory, nullptr);
    *image = RawImage{};
}

bool createRawImage(VkDevice device, const VkFns &fns,
                    const VkPhysicalDeviceMemoryProperties &memoryProperties, VkFormat format,
                    VkExtent2D extent, const char *what, RawImage *out, QString *error)
{
    if (format == VK_FORMAT_UNDEFINED || extent.width == 0 || extent.height == 0)
    {
        *error = QStringLiteral("FG: %1 的格式或尺寸无效").arg(QString::fromLatin1(what));
        return false;
    }

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE 是 FG 写 UAV 的前提；SAMPLED 供后续 pass 读；TRANSFER 让调用方
    // 可以直接拷进/拷出，不必再走一遍中间纹理。
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    RawImage result;
    result.format = format;
    result.extent = extent;

    VkResult vk = fns.createImage(device, &info, nullptr, &result.image);
    if (vk != VK_SUCCESS)
    {
        *error = QStringLiteral("FG: 创建 %1 的 VkImage 失败（%2）")
                     .arg(QString::fromLatin1(what))
                     .arg(int(vk));
        return false;
    }

    VkMemoryRequirements requirements{};
    fns.getImageMemoryRequirements(device, result.image, &requirements);
    const quint32 typeIndex = findDeviceLocalMemoryType(memoryProperties, requirements.memoryTypeBits);
    if (typeIndex == UINT32_MAX)
    {
        *error = QStringLiteral("FG: %1 找不到 DEVICE_LOCAL 显存类型").arg(QString::fromLatin1(what));
        destroyRawImage(device, fns, &result);
        return false;
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = typeIndex;
    vk = fns.allocateMemory(device, &allocation, nullptr, &result.memory);
    if (vk != VK_SUCCESS)
    {
        *error = QStringLiteral("FG: 为 %1 分配显存失败（%2）")
                     .arg(QString::fromLatin1(what))
                     .arg(int(vk));
        destroyRawImage(device, fns, &result);
        return false;
    }

    vk = fns.bindImageMemory(device, result.image, result.memory, 0);
    if (vk != VK_SUCCESS)
    {
        *error = QStringLiteral("FG: 绑定 %1 的显存失败（%2）")
                     .arg(QString::fromLatin1(what))
                     .arg(int(vk));
        destroyRawImage(device, fns, &result);
        return false;
    }

    *out = result;
    return true;
}

bool loadVkFns(const QRhiVulkanNativeHandles *handles, VkFns *fns, QString *error)
{
    if (!handles || handles->dev == VK_NULL_HANDLE || !handles->inst)
    {
        *error = QStringLiteral("FG: QRhi 没有提供 Vulkan 设备/实例句柄");
        return false;
    }

    const auto gipa = handles->inst->getInstanceProcAddr("vkGetDeviceProcAddr");
    if (!gipa)
    {
        *error = QStringLiteral("FG: 取不到 vkGetDeviceProcAddr");
        return false;
    }
    fns->getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa);

    auto loadDevice = [&](const char *name) {
        return fns->getDeviceProcAddr(handles->dev, name);
    };

    fns->deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(loadDevice("vkDeviceWaitIdle"));
    fns->createImage = reinterpret_cast<PFN_vkCreateImage>(loadDevice("vkCreateImage"));
    fns->destroyImage = reinterpret_cast<PFN_vkDestroyImage>(loadDevice("vkDestroyImage"));
    fns->getImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        loadDevice("vkGetImageMemoryRequirements"));
    fns->allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(loadDevice("vkAllocateMemory"));
    fns->freeMemory = reinterpret_cast<PFN_vkFreeMemory>(loadDevice("vkFreeMemory"));
    fns->bindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(loadDevice("vkBindImageMemory"));
    fns->cmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        loadDevice("vkCmdPipelineBarrier"));
    fns->getPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        handles->inst->getInstanceProcAddr("vkGetPhysicalDeviceMemoryProperties"));

    if (!fns->deviceWaitIdle || !fns->createImage || !fns->destroyImage
        || !fns->getImageMemoryRequirements || !fns->allocateMemory || !fns->freeMemory
        || !fns->bindImageMemory || !fns->cmdPipelineBarrier
        || !fns->getPhysicalDeviceMemoryProperties)
    {
        *error = QStringLiteral("FG: Vulkan 核心函数表不完整，设备可能不是由标准 loader 创建的");
        return false;
    }
    return true;
}

#endif  // RHINE_FSR3_VULKAN

}  // namespace

struct FgBackend::Private
{
    QRhi *rhi = nullptr;
    QSize renderSize;
    QSize displaySize;
    QRhiTexture::Format colorFormat = QRhiTexture::RGBA16F;
    bool ready = false;
    bool reset = true;
    bool lastFrameReset = false;
    bool needsInitialTransition = false;
    bool warnedHudLess = false;
    quint64 frameID = 0;
    quint32 dispatchDebugFlags = 0;

    float nearPlane = 5.0f;
    float farPlane = 300.0f;
    float fovY = 1.0f;
    float frameTimeMs = 16.6667f;
    float viewSpaceToMeters = 1.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    QVector3D cameraPosition;
    QVector3D cameraUp{0.0f, 1.0f, 0.0f};
    QVector3D cameraRight{1.0f, 0.0f, 0.0f};
    QVector3D cameraForward{0.0f, 0.0f, -1.0f};

    QString error;

#if defined(RHINE_FSR3_VULKAN)
    VkFns fns;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    FfxDevice ffxDevice = nullptr;

    // 两个 effect 各用一份 FfxInterface + scratch。SDK 的 scratch 里放的是
    // 后端自己的 context（资源表、屏障队列），maxContexts 只决定同一个接口里
    // 能挂几个 effect，不能让两个接口共用一个 scratch。
    FfxInterface ofInterface{};
    FfxInterface fiInterface{};
    std::vector<quint8> ofScratch;
    std::vector<quint8> fiScratch;
    FfxOpticalflowContext ofContext{};
    FfxFrameInterpolationContext fiContext{};
    // 分开记两个 effect 的创建状态：resize 中途失败时不能拿"整体就绪"当守卫，
    // 否则会漏销毁已建的 context，或者让 context 引用随后被清空的 scratch。
    bool ofCreated = false;
    bool fiCreated = false;

    // FI 的共享资源。SDK 只给描述、不给实例：ffxFrameInterpolationPrepare 会
    // 无条件把这三个当 UAV 注册并写入，所以必须由应用自己分配。
    RawImage dilatedDepth;
    RawImage dilatedMotionVectors;
    RawImage reconstructedPrevDepth;

    // OF 的输出。光流向量是 R16G16_SINT，QRhiTexture 没有这个格式，只能裸分配。
    RawImage opticalFlowVector;
    RawImage opticalFlowSceneChangeDetection;
    quint32 opticalFlowWidth = 0;
    quint32 opticalFlowHeight = 0;

    // 交给上层的插值帧。用 QRhiTexture 而不是裸图，调用方可以直接采样/拷贝。
    std::unique_ptr<QRhiTexture> outputTexture;
#endif
};

FgBackend::FgBackend() : d(new Private) {}

FgBackend::~FgBackend()
{
    release();
}

bool FgBackend::compiledIn()
{
#if defined(RHINE_FSR3_VULKAN)
    return true;
#else
    return false;
#endif
}

bool FgBackend::supported(QRhi *rhi)
{
    if (!rhi)
        return false;
#if defined(RHINE_FSR3_VULKAN)
    return rhi->backend() == QRhi::Vulkan;
#else
    return false;
#endif
}

bool FgBackend::available() const
{
    return d->ready;
}

bool FgBackend::resize(QRhi *rhi, QSize renderSize, QSize displaySize,
                       QRhiTexture::Format colorFormat)
{
#if !defined(RHINE_FSR3_VULKAN)
    Q_UNUSED(rhi)
    Q_UNUSED(renderSize)
    Q_UNUSED(displaySize)
    Q_UNUSED(colorFormat)
    d->error = QStringLiteral("FG: 这个构建没有编进 Vulkan 帧生成");
    return false;
#else
    d->error.clear();

    if (!supported(rhi))
    {
        d->error = QStringLiteral("FG: 只支持 Vulkan 后端");
        return false;
    }
    if (!renderSize.isValid() || !displaySize.isValid())
    {
        d->error = QStringLiteral("FG: 渲染/显示尺寸无效");
        return false;
    }
    if (renderSize.width() > 16384 || renderSize.height() > 16384
        || displaySize.width() > 16384 || displaySize.height() > 16384)
    {
        d->error = QStringLiteral("FG: 尺寸超出 FG 支持范围");
        return false;
    }
    // OF 内部会按 1/2 到 1/64 建金字塔（ffx_opticalflow.cpp:302-308），
    // 显示尺寸小于 64 时最后一层会算出 0 宽高，vkCreateImage 直接失败。
    if (displaySize.width() < 64 || displaySize.height() < 64)
    {
        d->error = QStringLiteral("FG: 显示尺寸太小（至少 64x64）");
        return false;
    }
    if (toFfxFormat(colorFormat) == FFX_SURFACE_FORMAT_UNKNOWN)
    {
        d->error = QStringLiteral("FG: 不支持的 backbuffer 格式");
        return false;
    }

    if (d->ready && d->rhi == rhi && d->renderSize == renderSize && d->displaySize == displaySize
        && d->colorFormat == colorFormat)
        return true;

    // 重建会释放 GPU 资源，必须先等 GPU 空闲；resize 本来就不在帧内，这里多等
    // 一次比留下悬空资源安全。
    if (d->device != VK_NULL_HANDLE && d->fns.deviceWaitIdle)
        d->fns.deviceWaitIdle(d->device);
    release();

    const auto *handles = static_cast<const QRhiVulkanNativeHandles *>(rhi->nativeHandles());
    if (!handles || handles->dev == VK_NULL_HANDLE || handles->physDev == VK_NULL_HANDLE)
    {
        d->error = QStringLiteral("FG: QRhi 没有提供 Vulkan 原生句柄");
        return false;
    }

    d->device = handles->dev;
    d->physicalDevice = handles->physDev;
    if (!loadVkFns(handles, &d->fns, &d->error))
        return false;

    d->fns.getPhysicalDeviceMemoryProperties(d->physicalDevice, &d->memoryProperties);

    VkDeviceContext deviceContext{};
    deviceContext.vkDevice = handles->dev;
    deviceContext.vkPhysicalDevice = handles->physDev;
    deviceContext.vkDeviceProcAddr = d->fns.getDeviceProcAddr;
    // ffxGetDeviceVK 把这份上下文拷进后端的一个静态变量，所以两个 effect 共用
    // 同一个 FfxDevice 就够了；它不接管设备生命周期。
    d->ffxDevice = ffxGetDeviceVK(&deviceContext);
    if (!d->ffxDevice)
    {
        d->error = QStringLiteral("FG: ffxGetDeviceVK 失败");
        return false;
    }

    const size_t scratchSize = ffxGetScratchMemorySizeVK(handles->physDev, 1);
    if (scratchSize == 0)
    {
        d->error = QStringLiteral("FG: ffxGetScratchMemorySizeVK 返回 0");
        return false;
    }
    d->ofScratch.assign(scratchSize, 0);
    d->fiScratch.assign(scratchSize, 0);

    if (ffxGetInterfaceVK(&d->ofInterface, d->ffxDevice, d->ofScratch.data(), d->ofScratch.size(), 1)
        != FFX_OK)
    {
        d->error = QStringLiteral("FG: 创建 OpticalFlow 后端接口失败");
        release();
        return false;
    }
    if (ffxGetInterfaceVK(&d->fiInterface, d->ffxDevice, d->fiScratch.data(), d->fiScratch.size(), 1)
        != FFX_OK)
    {
        d->error = QStringLiteral("FG: 创建 FrameInterpolation 后端接口失败");
        release();
        return false;
    }

    const FgConfig config = FgConfig::fromEnvironment();
    // 全局消息回调是 SDK 里唯一能在没有 per-context 回调字段时拿到诊断的通道：
    // FfxFrameInterpolationContextDescription 没有 fpMessage 字段。
    ffxFrameInterpolationSetGlobalDebugMessage(&fgMessage, 1);

    FfxOpticalflowContextDescription ofDescription{};
    ofDescription.backendInterface = d->ofInterface;
    ofDescription.flags = 0;
    ofDescription.resolution = {quint32(displaySize.width()), quint32(displaySize.height())};
    if (ffxOpticalflowContextCreate(&d->ofContext, &ofDescription) != FFX_OK)
    {
        d->error = QStringLiteral("FG: ffxOpticalflowContextCreate 失败");
        release();
        return false;
    }
    d->ofCreated = true;

    FfxFrameInterpolationContextDescription fiDescription{};
    // 与 Fsr3Backend 保持同一套语义：Rhine 的深度是标准透视（0=near、1=far），
    // 非无限远平面，速度是渲染分辨率的 NDC 位移且未叠抖动，颜色是 LDR。
    // 这些标志一旦和实际输入不符，SDK 不会报错，只会算错——所以宁可全关。
    fiDescription.flags = config.debugChecking ? FFX_FRAMEINTERPOLATION_ENABLE_DEBUG_CHECKING : 0;
    fiDescription.maxRenderSize = {quint32(renderSize.width()), quint32(renderSize.height())};
    fiDescription.displaySize = {quint32(displaySize.width()), quint32(displaySize.height())};
    fiDescription.backBufferFormat = toFfxFormat(colorFormat);
    // 我们的 HUD 是独立通道，交给 FG 的永远是同格式的无 HUD 颜色，
    // 所以"上一帧插值源"用和 backbuffer 一样的格式即可。
    fiDescription.previousInterpolationSourceFormat = fiDescription.backBufferFormat;
    fiDescription.backendInterface = d->fiInterface;
    if (ffxFrameInterpolationContextCreate(&d->fiContext, &fiDescription) != FFX_OK)
    {
        d->error = QStringLiteral("FG: ffxFrameInterpolationContextCreate 失败");
        release();
        return false;
    }
    d->fiCreated = true;

    d->dispatchDebugFlags = 0;
    if (config.drawDebugTearLines)
        d->dispatchDebugFlags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES;
    if (config.drawDebugResetIndicators)
        d->dispatchDebugFlags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS;
    if (config.drawDebugView)
        d->dispatchDebugFlags |= FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW;

    // 共享资源的格式与尺寸以 SDK 给的描述为准，不自己抄常量：
    // SDK 升级时这里会自动跟上。
    FfxFrameInterpolationSharedResourceDescriptions fiShared{};
    if (ffxFrameInterpolationGetSharedResourceDescriptions(&d->fiContext, &fiShared) != FFX_OK)
    {
        d->error = QStringLiteral("FG: 取 FI 共享资源描述失败");
        release();
        return false;
    }

    auto createFromDescription = [&](const FfxCreateResourceDescription &description,
                                     const char *what, RawImage *target) {
        const VkFormat format = toVkFormat(description.resourceDescription.format);
        const VkExtent2D extent{description.resourceDescription.width,
                                description.resourceDescription.height};
        return createRawImage(d->device, d->fns, d->memoryProperties, format, extent, what, target,
                              &d->error);
    };

    if (!createFromDescription(fiShared.dilatedDepth, "FI dilatedDepth", &d->dilatedDepth)
        || !createFromDescription(fiShared.dilatedMotionVectors, "FI dilatedMotionVectors",
                                  &d->dilatedMotionVectors)
        || !createFromDescription(fiShared.reconstructedPrevNearestDepth,
                                  "FI reconstructedPrevNearestDepth", &d->reconstructedPrevDepth))
    {
        release();
        return false;
    }

    FfxOpticalflowSharedResourceDescriptions ofShared{};
    if (ffxOpticalflowGetSharedResourceDescriptions(&d->ofContext, &ofShared) != FFX_OK)
    {
        d->error = QStringLiteral("FG: 取 OpticalFlow 共享资源描述失败");
        release();
        return false;
    }
    if (!createFromDescription(ofShared.opticalFlowVector, "OF opticalFlowVector",
                               &d->opticalFlowVector)
        || !createFromDescription(ofShared.opticalFlowSCD, "OF opticalFlowSCD",
                                  &d->opticalFlowSceneChangeDetection))
    {
        release();
        return false;
    }
    d->opticalFlowWidth = ofShared.opticalFlowVector.resourceDescription.width;
    d->opticalFlowHeight = ofShared.opticalFlowVector.resourceDescription.height;

    // 输出用 QRhiTexture：调用方可以直接把它当普通纹理采样或拷贝，不必再包一层。
    d->outputTexture.reset(rhi->newTexture(
        colorFormat, displaySize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedWithLoadStore
            | QRhiTexture::UsedAsTransferSource));
    if (!d->outputTexture || !d->outputTexture->create())
    {
        d->error = QStringLiteral("FG: 创建插值帧输出纹理失败");
        release();
        return false;
    }

    d->rhi = rhi;
    d->renderSize = renderSize;
    d->displaySize = displaySize;
    d->colorFormat = colorFormat;
    d->frameID = 0;
    d->reset = true;
    d->lastFrameReset = false;
    // 内部裸图从 UNDEFINED 起步，第一帧必须先转成 GENERAL 再交给 SDK；
    // 否则 SDK 按声明的 COMMON（=GENERAL）发屏障，而真实布局还是 UNDEFINED，
    // 属于"oldLayout 与实际不符"的未定义行为。
    d->needsInitialTransition = true;
    d->warnedHudLess = false;
    d->ready = true;
    qInfo().noquote() << "FG: 已建立" << renderSize << "->" << displaySize << "的帧生成上下文";
    return true;
#endif
}

void FgBackend::release()
{
#if defined(RHINE_FSR3_VULKAN)
    // 先销毁 effect：它们内部还会通过后端释放自己的资源，而后端的状态全在
    // scratch 里，scratch 必须活到这一步之后。
    if (d->fiCreated)
        ffxFrameInterpolationContextDestroy(&d->fiContext);
    if (d->ofCreated)
        ffxOpticalflowContextDestroy(&d->ofContext);
    d->fiContext = {};
    d->ofContext = {};
    d->fiCreated = false;
    d->ofCreated = false;

    destroyRawImage(d->device, d->fns, &d->dilatedDepth);
    destroyRawImage(d->device, d->fns, &d->dilatedMotionVectors);
    destroyRawImage(d->device, d->fns, &d->reconstructedPrevDepth);
    destroyRawImage(d->device, d->fns, &d->opticalFlowVector);
    destroyRawImage(d->device, d->fns, &d->opticalFlowSceneChangeDetection);

    d->outputTexture.reset();
    d->ofInterface = {};
    d->fiInterface = {};
    d->ofScratch.clear();
    d->ofScratch.shrink_to_fit();
    d->fiScratch.clear();
    d->fiScratch.shrink_to_fit();
    d->ffxDevice = nullptr;
    d->device = VK_NULL_HANDLE;
    d->physicalDevice = VK_NULL_HANDLE;
    d->opticalFlowWidth = 0;
    d->opticalFlowHeight = 0;
#endif
    d->ready = false;
    d->rhi = nullptr;
    d->renderSize = QSize();
    d->displaySize = QSize();
    d->needsInitialTransition = false;
    d->reset = true;
    d->frameID = 0;
}

void FgBackend::setCamera(float nearPlane, float farPlane, float fovYRadians, float frameTimeMs)
{
    d->nearPlane = nearPlane > 0.0f ? nearPlane : 0.1f;
    d->farPlane = farPlane > d->nearPlane ? farPlane : d->nearPlane + 1.0f;
    d->fovY = (fovYRadians > 0.0f && fovYRadians < 3.14159265f) ? fovYRadians : 1.0f;
    d->frameTimeMs = frameTimeMs > 0.0f ? frameTimeMs : 16.6667f;
}

void FgBackend::setJitter(float x, float y)
{
    d->jitterX = x;
    d->jitterY = y;
}

void FgBackend::setCameraPose(const QVector3D &position, const QVector3D &up,
                              const QVector3D &right, const QVector3D &forward)
{
    d->cameraPosition = position;
    d->cameraUp = up;
    d->cameraRight = right;
    d->cameraForward = forward;
}

void FgBackend::requestReset()
{
    d->reset = true;
}

QRhiTexture *FgBackend::output() const
{
#if defined(RHINE_FSR3_VULKAN)
    return d->outputTexture.get();
#else
    return nullptr;
#endif
}

QSize FgBackend::renderSize() const
{
    return d->renderSize;
}

QSize FgBackend::displaySize() const
{
    return d->displaySize;
}

bool FgBackend::lastFrameReset() const
{
    return d->lastFrameReset;
}

QString FgBackend::lastError() const
{
    return d->error;
}

bool FgBackend::prepare(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *colorHudLess,
                        QRhiTexture *depth, QRhiTexture *motionVectors)
{
#if !defined(RHINE_FSR3_VULKAN)
    Q_UNUSED(rhi)
    Q_UNUSED(commandBuffer)
    Q_UNUSED(colorHudLess)
    Q_UNUSED(depth)
    Q_UNUSED(motionVectors)
    return false;
#else
    d->error.clear();
    if (!d->ready || rhi != d->rhi)
    {
        d->error = QStringLiteral("FG: prepare 前没有可用的上下文");
        return false;
    }
    if (!commandBuffer || !colorHudLess || !depth || !motionVectors)
    {
        d->error = QStringLiteral("FG: prepare 缺少必要输入");
        return false;
    }
    if (colorHudLess->pixelSize() != d->displaySize)
    {
        d->error = QStringLiteral("FG: 无 HUD 颜色必须是显示分辨率");
        return false;
    }
    if (depth->pixelSize() != d->renderSize || motionVectors->pixelSize() != d->renderSize)
    {
        d->error = QStringLiteral("FG: 深度与运动矢量必须是渲染分辨率");
        return false;
    }

    const auto *bufferHandles =
        static_cast<const QRhiVulkanCommandBufferNativeHandles *>(commandBuffer->nativeHandles());
    if (!bufferHandles || bufferHandles->commandBuffer == VK_NULL_HANDLE)
    {
        d->error = QStringLiteral("FG: 拿不到当前 VkCommandBuffer");
        return false;
    }
    VkCommandBuffer commandList = bufferHandles->commandBuffer;
    const FfxCommandList ffxCommandList = ffxGetCommandListVK(commandList);

    if (d->needsInitialTransition)
        recordInitialTransitions(commandList);

    const FfxResource colorResource =
        wrapTexture(colorHudLess, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.HudLessColor",
                    currentVkState(colorHudLess));
    const FfxResource depthResource =
        wrapTexture(depth, FFX_RESOURCE_USAGE_DEPTHTARGET, L"Rhine.Fg.Depth",
                    currentVkState(depth), false);
    const FfxResource motionResource =
        wrapTexture(motionVectors, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.MotionVectors",
                    currentVkState(motionVectors));
    const FfxResource dilatedDepth =
        wrapRawImage(d->dilatedDepth, FFX_RESOURCE_USAGE_UAV, L"Rhine.Fg.DilatedDepth",
                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource dilatedMotion =
        wrapRawImage(d->dilatedMotionVectors, FFX_RESOURCE_USAGE_UAV,
                     L"Rhine.Fg.DilatedMotionVectors", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource reconstructed =
        wrapRawImage(d->reconstructedPrevDepth, FFX_RESOURCE_USAGE_UAV,
                     L"Rhine.Fg.ReconstructedPrevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource flowVector =
        wrapRawImage(d->opticalFlowVector, FFX_RESOURCE_USAGE_UAV, L"Rhine.Fg.OpticalFlowVector",
                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource flowScd =
        wrapRawImage(d->opticalFlowSceneChangeDetection, FFX_RESOURCE_USAGE_UAV,
                     L"Rhine.Fg.OpticalFlowSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!colorResource.resource || !depthResource.resource || !motionResource.resource
        || !dilatedDepth.resource || !dilatedMotion.resource || !reconstructed.resource
        || !flowVector.resource || !flowScd.resource)
    {
        d->error = QStringLiteral("FG: 有纹理拿不到原生句柄，跳过这一帧");
        return false;
    }

    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = ffxCommandList;
    flow.color = colorResource;
    flow.opticalFlowVector = flowVector;
    flow.opticalFlowSCD = flowScd;
    flow.reset = d->reset;
    // LDR 链路：颜色是显示参考的 sRGB 编码值，minMaxLuminance 只在 PQ/SCRGB 下生效。
    flow.backbufferTransferFunction = int(FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB);
    flow.minMaxLuminance = {0.0f, 0.0f};

    FfxErrorCode result = ffxOpticalflowContextDispatch(&d->ofContext, &flow);
    if (result != FFX_OK)
    {
        d->error = QStringLiteral("FG: ffxOpticalflowContextDispatch 失败（%1）").arg(int(result));
        qWarning().noquote() << d->error;
        return false;
    }

    FfxFrameInterpolationPrepareDescription prepare{};
    prepare.flags = 0;
    prepare.commandList = ffxCommandList;
    prepare.renderSize = {quint32(d->renderSize.width()), quint32(d->renderSize.height())};
    prepare.jitterOffset = {d->jitterX, d->jitterY};
    // 与 Fsr3Backend 同一口径：速度写的是 NDC 位移，乘渲染分辨率换成像素增量。
    prepare.motionVectorScale = {float(d->renderSize.width()), float(d->renderSize.height())};
    prepare.frameTimeDelta = d->frameTimeMs;
    prepare.cameraNear = d->nearPlane;
    prepare.cameraFar = d->farPlane;
    prepare.viewSpaceToMetersFactor = d->viewSpaceToMeters;
    prepare.cameraFovAngleVertical = d->fovY;
    prepare.depth = depthResource;
    prepare.motionVectors = motionResource;
    // Prepare 不消费 frameID，但保持和随后 Dispatch 一致，避免将来 SDK 用上它时错位。
    prepare.frameID = d->frameID + 1;
    prepare.dilatedDepth = dilatedDepth;
    prepare.dilatedMotionVectors = dilatedMotion;
    prepare.reconstructedPrevDepth = reconstructed;
    prepare.cameraPosition[0] = d->cameraPosition.x();
    prepare.cameraPosition[1] = d->cameraPosition.y();
    prepare.cameraPosition[2] = d->cameraPosition.z();
    prepare.cameraUp[0] = d->cameraUp.x();
    prepare.cameraUp[1] = d->cameraUp.y();
    prepare.cameraUp[2] = d->cameraUp.z();
    prepare.cameraRight[0] = d->cameraRight.x();
    prepare.cameraRight[1] = d->cameraRight.y();
    prepare.cameraRight[2] = d->cameraRight.z();
    prepare.cameraForward[0] = d->cameraForward.x();
    prepare.cameraForward[1] = d->cameraForward.y();
    prepare.cameraForward[2] = d->cameraForward.z();

    result = ffxFrameInterpolationPrepare(&d->fiContext, &prepare);
    if (result != FFX_OK)
    {
        d->error = QStringLiteral("FG: ffxFrameInterpolationPrepare 失败（%1）").arg(int(result));
        qWarning().noquote() << d->error;
        return false;
    }
    return true;
#endif
}

bool FgBackend::dispatch(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *color,
                         QRhiTexture *colorHudLess, QRhiTexture *output)
{
#if !defined(RHINE_FSR3_VULKAN)
    Q_UNUSED(rhi)
    Q_UNUSED(commandBuffer)
    Q_UNUSED(color)
    Q_UNUSED(colorHudLess)
    Q_UNUSED(output)
    return false;
#else
    d->error.clear();
    if (!d->ready || rhi != d->rhi)
    {
        d->error = QStringLiteral("FG: dispatch 前没有可用的上下文");
        return false;
    }
    if (!commandBuffer || !color)
    {
        d->error = QStringLiteral("FG: dispatch 缺少必要输入");
        return false;
    }
    if (color->pixelSize() != d->displaySize)
    {
        d->error = QStringLiteral("FG: 当前帧颜色必须是显示分辨率");
        return false;
    }

    QRhiTexture *target = output ? output : d->outputTexture.get();
    if (!target || target->pixelSize() != d->displaySize || target->format() != d->colorFormat)
    {
        d->error = QStringLiteral("FG: 输出纹理的尺寸或格式与上下文不一致");
        return false;
    }

    const auto *bufferHandles =
        static_cast<const QRhiVulkanCommandBufferNativeHandles *>(commandBuffer->nativeHandles());
    if (!bufferHandles || bufferHandles->commandBuffer == VK_NULL_HANDLE)
    {
        d->error = QStringLiteral("FG: 拿不到当前 VkCommandBuffer");
        return false;
    }
    VkCommandBuffer commandList = bufferHandles->commandBuffer;
    const FfxCommandList ffxCommandList = ffxGetCommandListVK(commandList);

    if (d->needsInitialTransition)
        recordInitialTransitions(commandList);

    // HUD-less 只有在格式与上下文一致时才能用：SDK 会把它拷进
    // previousInterpolationSource，格式不同时拷贝会失败，而且只在带断言的构建里报。
    bool useHudLess = colorHudLess && colorHudLess != color;
    if (useHudLess && (colorHudLess->pixelSize() != d->displaySize
                       || toFfxFormat(colorHudLess->format()) != toFfxFormat(d->colorFormat)))
    {
        if (!d->warnedHudLess)
        {
            qWarning().noquote() << "FG: 无 HUD 颜色的尺寸/格式与上下文不一致，退回用带 HUD 颜色插值";
            d->warnedHudLess = true;
        }
        useHudLess = false;
    }

    const FfxResource colorResource =
        wrapTexture(color, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.Color", currentVkState(color));
    const FfxResource hudLessResource =
        useHudLess ? wrapTexture(colorHudLess, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.HudLess",
                                 currentVkState(colorHudLess))
                   : FfxResource{};
    // 先记下"声明状态"：SDK 结束后会把资源还原到这个状态对应的布局，
    // 所以 QRhi 的跟踪要按它回写（见 layoutForFfxState 的注释）。
    const FfxResourceStates outputState = currentVkState(target);
    const FfxResource outputResource =
        wrapTexture(target, FFX_RESOURCE_USAGE_UAV, L"Rhine.Fg.Output", outputState);
    const FfxResource flowVector =
        wrapRawImage(d->opticalFlowVector, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.OpticalFlowVector",
                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource flowScd =
        wrapRawImage(d->opticalFlowSceneChangeDetection, FFX_RESOURCE_USAGE_READ_ONLY,
                     L"Rhine.Fg.OpticalFlowSCD", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource dilatedDepth =
        wrapRawImage(d->dilatedDepth, FFX_RESOURCE_USAGE_READ_ONLY, L"Rhine.Fg.DilatedDepth",
                     FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource dilatedMotion =
        wrapRawImage(d->dilatedMotionVectors, FFX_RESOURCE_USAGE_READ_ONLY,
                     L"Rhine.Fg.DilatedMotionVectors", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    const FfxResource reconstructed =
        wrapRawImage(d->reconstructedPrevDepth, FFX_RESOURCE_USAGE_READ_ONLY,
                     L"Rhine.Fg.ReconstructedPrevDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!colorResource.resource || !outputResource.resource || !flowVector.resource
        || !flowScd.resource || !dilatedDepth.resource || !dilatedMotion.resource
        || !reconstructed.resource)
    {
        d->error = QStringLiteral("FG: 有纹理拿不到原生句柄，跳过这一帧");
        return false;
    }

    FfxFrameInterpolationDispatchDescription description{};
    description.flags = d->dispatchDebugFlags;
    description.commandList = ffxCommandList;
    description.displaySize = {quint32(d->displaySize.width()), quint32(d->displaySize.height())};
    description.renderSize = {quint32(d->renderSize.width()), quint32(d->renderSize.height())};
    description.currentBackBuffer = colorResource;
    description.currentBackBuffer_HUDLess = hudLessResource;
    description.output = outputResource;
    description.interpolationRect = {0, 0, qint32(d->displaySize.width()),
                                     qint32(d->displaySize.height())};
    description.opticalFlowVector = flowVector;
    description.opticalFlowSceneChangeDetection = flowScd;
    description.opticalFlowBufferSize = {d->opticalFlowWidth, d->opticalFlowHeight};
    // 光流向量存的是像素位移，SDK 的着色器用 (1/scale)/blockSize 反推纹理尺寸，
    // 所以 scale 必须正好是显示尺寸的倒数；写成别的值会让采样坐标整体缩放。
    description.opticalFlowScale = {1.0f / float(d->displaySize.width()),
                                    1.0f / float(d->displaySize.height())};
    description.opticalFlowBlockSize = kOpticalFlowBlockSize;
    description.cameraNear = d->nearPlane;
    description.cameraFar = d->farPlane;
    description.cameraFovAngleVertical = d->fovY;
    description.viewSpaceToMetersFactor = d->viewSpaceToMeters;
    description.frameTimeDelta = d->frameTimeMs;
    description.reset = d->reset;
    description.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    description.minMaxLuminance[0] = 0.0f;
    description.minMaxLuminance[1] = 0.0f;
    // SDK 明确要求每帧严格 +1：任何非 1 的差值都会被当成 frameID 断档，
    // 从而重置 FG 的历史状态（ffx_frameinterpolation.h:275）。
    description.frameID = d->frameID + 1;
    description.dilatedDepth = dilatedDepth;
    description.dilatedMotionVectors = dilatedMotion;
    description.reconstructedPrevDepth = reconstructed;
    description.distortionField = {};

    const bool resetThisFrame = d->reset;
    const FfxErrorCode result = ffxFrameInterpolationDispatch(&d->fiContext, &description);
    d->lastFrameReset = resetThisFrame;

    // 无论成功与否都递增：SDK 在函数开头就把 previousFrameID 更新成了本次的值，
    // 失败后复用同一个 frameID 会让差值变成 0，SDK 不会把它当成断档。宁可让它
    // 跳号（SDK 会因此重置历史），也不要让两边的帧号悄悄错位。
    ++d->frameID;

    if (result != FFX_OK)
    {
        d->error = QStringLiteral("FG: ffxFrameInterpolationDispatch 失败（%1）").arg(int(result));
        qWarning().noquote() << d->error;
        return false;
    }

    d->reset = false;

    // SDK 在 UnregisterResourcesVK 里把动态资源屏障回它声明的初始状态，所以结束后
    // 的真实布局是 layoutForFfxState(outputState)。必须同步给 QRhi，否则下一次
    // QRhi 采样会从"它以为的旧布局"起手，少发一次必要转换。
    target->setNativeLayout(layoutForFfxState(outputState));
    return true;
#endif
}

#if defined(RHINE_FSR3_VULKAN)

void FgBackend::recordInitialTransitions(void *commandList)
{
    const VkCommandBuffer commandBuffer = reinterpret_cast<VkCommandBuffer>(commandList);
    VkImageMemoryBarrier barriers[6]{};
    VkImage images[6] = {
        d->dilatedDepth.image,
        d->dilatedMotionVectors.image,
        d->reconstructedPrevDepth.image,
        d->opticalFlowVector.image,
        d->opticalFlowSceneChangeDetection.image,
        d->outputTexture ? reinterpret_cast<VkImage>(quintptr(d->outputTexture->nativeTexture().object))
                         : VK_NULL_HANDLE,
    };

    quint32 count = 0;
    for (VkImage image : images)
    {
        if (image == VK_NULL_HANDLE)
            continue;
        VkImageMemoryBarrier &barrier = barriers[count++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }

    if (count > 0)
    {
        d->fns.cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                      | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  0, 0, nullptr, 0, nullptr, count, barriers);
    }

    if (d->outputTexture)
        d->outputTexture->setNativeLayout(VK_IMAGE_LAYOUT_GENERAL);
    d->needsInitialTransition = false;
}

#endif  // RHINE_FSR3_VULKAN
