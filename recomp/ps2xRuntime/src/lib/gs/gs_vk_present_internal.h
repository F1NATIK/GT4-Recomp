#pragma once

// The renderer's side of the window presenter (gs_vk_present.h), for vkgl
// (gs_vkgl.cpp): three slots, as gs_gl_present's. The renderer draws a frame
// into a slot that is neither the one the host holds nor the newest
// published, then publishes it. Vulkan orders the two threads' GPU work with
// two timeline semaphores: the renderer's (a value per published frame) and
// the host's (a value per presented frame).

#include "gs_vk_device.h"

namespace gsvk::present
{
    // Renderer thread, before drawing a frame: the slot to draw it into, and the
    // host timeline value its drawing has to wait for (the host's last reads
    // of that slot; 0: none).
    int rendererSlot(uint64_t &hostWaitValue);
    VkSemaphore hostTimeline();
    VkSemaphore rendererTimeline();
    // The renderer timeline value to signal with the frame's submission.
    uint64_t rendererNextValue();
    // After that submission: the frame in slot `slot` (image, its view, its
    // size; width x height of it valid) is published.
    void rendererPublish(int slot, VkImage image, VkImageView view, int texWidth, int texHeight, uint32_t width, uint32_t height,
                         uint64_t timelineValue, uint64_t tick, int parity, int mode, uint32_t crt, int lines);
}
