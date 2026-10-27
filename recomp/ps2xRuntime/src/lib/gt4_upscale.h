#pragma once

// GT4's upscalers for the Full edition's Vulkan renderer (DLSS step 2; analysis/design/lighting/dlss-step1).
//   GT4_UPSCALER=dlss: NVIDIA DLSS through Streamline (sl.interposer.dll and its plugins beside GT4.exe, signature
//   checked; nothing is downloaded: Streamline's over-the-air updates are off). The picture is at GT3_GS_GL_SCALE;
//   the renderer draws at GT4_DLSS_DRAW (default: 2x for a 4x picture, one less below) and DLSS makes the picture
//   (tests: GT4_DLSS_OUT = the picture's scale, GT3_GS_GL_SCALE the drawing's). The mode follows the ratio: 1 DLAA,
//   1.5 quality, 1.72 balanced, 2 performance, 3 ultra performance. GT4_VK_PRESENT=gl: frames shown through OpenGL.
// Off (unset): nothing is loaded and nothing changes.

#include <cstdint>

namespace gt4up
{
    // Before the Vulkan instance is made: with GT4_UPSCALER=dlss and Streamline present, Streamline started and its
    // vkGetInstanceProcAddr (every Vulkan call through it: Streamline's own wrappers for the instance, the device and
    // the swapchain); null otherwise. Called once.
    void *instanceProcAddr();

    // DLSS asked for and Streamline started (the presenter must then be the Vulkan swapchain's).
    bool wanted();
    // Still usable (started, and no evaluation found DLSS missing): the renderer jitters only then.
    bool usable();
    // The picture's scale with DLSS (GT4_DLSS_OUT), or 0 (off / not usable).
    int outputScale(int renderScale);
    // With DLSS on (false: off / not usable): the renderer's drawing scale and the picture's scale DLSS makes, from
    // GT3_GS_GL_SCALE (`requested`). From the launcher GT3_GS_GL_SCALE is the picture's scale and GT4_DLSS_DRAW the
    // drawing's (default: half of 4x, one less below); with GT4_DLSS_OUT (tests) GT3_GS_GL_SCALE is the drawing's.
    bool scales(int requested, int &draw, int &out);

    struct Image
    {
        uint64_t image = 0, view = 0; // VkImage, VkImageView
        uint32_t format = 0, usage = 0; // VkFormat, VkImageUsageFlags
        int width = 0, height = 0;    // the whole image
        int x = 0, y = 0, w = 0, h = 0; // the part used
    };
    struct Frame
    {
        Image color, depth, motion, out;
        float jitterX = 0.0f, jitterY = 0.0f; // input pixels
        float motionScaleX = 0.0f, motionScaleY = 0.0f; // motion texel * scale = offset to last frame's position, in the input's [0, 1]
        bool reset = false;
        // (FSR 4 through Direct3D 12: vkgl::splitExternal, the renderer's hand-over mid-frame; see gt4_upscale_fsr4.cpp)
        void *(*split)(uint64_t semaphore, uint64_t signalValue, uint64_t waitValue) = nullptr;
    };
    // Records DLSS into cmd (a VkCommandBuffer; every image in the GENERAL layout). False: not done (logged).
    bool evaluate(void *cmd, const Frame &f);
} // namespace gt4up
