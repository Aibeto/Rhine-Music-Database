// POSIX stand-in for the Windows SDK's <synchapi.h>, which FidelityFX's Vulkan
// frame-interpolation swapchain includes alongside <Windows.h>. Every synchronisation
// entry point it needs already lives in the Windows.h shim installed next to this file.
#pragma once

#include <Windows.h>
