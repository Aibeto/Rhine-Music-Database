// Frame-generation stand-in for platforms where the SDK's Vulkan swapchain is not built.
//
// ffx_vk.cpp unconditionally installs ffxSetFrameGenerationConfigToSwapchainVK into the
// backend interface, so the symbol has to exist for the Vulkan backend to link. The real
// implementation lives in sdk/src/backends/vk/FrameInterpolationSwapchain, which is
// Windows-only in FidelityFX SDK 1.1.4: it uses UINT64/LARGE_INTEGER, Win32 critical
// sections, VkSurfaceFullScreenExclusiveWin32InfoEXT, and it owns the VkSwapchainKHR and
// the present thread. Rhine renders through Qt's QRhi, which owns the swapchain, so
// taking presentation over is a separate architectural project rather than a port.
//
// Reporting FFX_ERROR_BACKEND_API_ERROR keeps frame generation honestly unavailable
// instead of silently presenting nothing.

#include <FidelityFX/host/backends/vk/ffx_vk.h>

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* /*config*/)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}
