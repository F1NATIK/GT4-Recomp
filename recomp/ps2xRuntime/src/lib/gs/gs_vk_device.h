#pragma once

// The Vulkan device of the Vulkan renderer (gs_vkgl.cpp) and the interop
// self-test (gs_vk_context.cpp). Internal: includes Vulkan (and through it
// windows.h), so not for files that include raylib.

#include <cstdint>
#include <mutex>
#include <string>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
// volk includes vulkan_win32.h and with it windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <volk.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // winnt.h's; vkgl::MemoryBarrier is glMemoryBarrier
#endif

namespace gsvk
{
    struct Device
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceMemoryProperties memory{};
        std::string name;
        bool sameGpuAsGl = false;
        // Optional features the device has (and were enabled).
        bool interlock = false;     // VK_EXT_fragment_shader_interlock (pixel interlock)
        bool conservative = false;  // VK_EXT_conservative_rasterization
        bool pushDescriptor = false;
        bool float64 = false;
        bool dualSrcBlend = false;
        bool depthClamp = false;
        bool bindless = false;      // descriptor indexing for sampled images
        bool drawParameters = false;
        bool externalMemory = false;
        bool localRead = false;     // VK_KHR_dynamic_rendering_local_read (render target reads in a pass)
        bool timeline = false;      // timeline semaphores (Vulkan 1.2 feature)
        bool hostQueryReset = false; // queries reset from the CPU (Vulkan 1.2 feature): the GPU timers (profiling)
        bool surface = false;       // VK_KHR_surface + VK_KHR_win32_surface on the instance
        bool swapchain = false;     // VK_KHR_swapchain (the window presenter, gs_vk_present.h)
    };

    // Created on first use, with the OpenGL context current on the calling
    // thread (to pick the same GPU); null when Vulkan 1.3 is not available.
    const Device *sharedDevice();
    // The index of a memory type with these properties (UINT32_MAX: none).
    uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags want);
    // Held around every use of the device's one queue (vkQueueSubmit,
    // vkQueuePresentKHR, vkQueueWaitIdle): the renderer thread and the
    // window presenter (the host thread) share it.
    std::mutex &queueMutex();
}
