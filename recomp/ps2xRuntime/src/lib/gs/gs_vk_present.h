#pragma once

// The Vulkan renderer's own window presenter: frames go from the renderer's
// images to a Vulkan swapchain on the game window, never through OpenGL.
// (The other way, vkgl::presentEnd copies each frame into an image OpenGL
// imports through external memory; AMD's OpenGL driver crashed on that import
// at GT4's first picture, RX 9070 XT, developer 2026-10-11.)
//
// The host loop still draws its overlays (FPS counter, notes, the cheat card)
// with raylib, into an OpenGL render texture: the rectangles they cover are
// read back (a few KB a frame) and drawn over the picture by Vulkan. The
// picture itself (the scanline pass, present_scanline.cpp, or a plain scaled
// copy) is drawn by Vulkan from the renderer's image.
//
//   GT4_VK_PRESENT       auto (default: the window presenter unless the card is
//                        NVIDIA's, which keeps the OpenGL presentation) |
//                        window (always this presenter) | gl (always OpenGL's)
//   GT4_VK_PRESENT_MODE  mailbox | immediate | fifo (default: mailbox, else
//                        immediate, else fifo: the host loop paces itself, as
//                        the OpenGL window swaps without vsync)
//
// No Vulkan types here: included next to raylib.

#include <cstdint>
#include <vector>

namespace gsvk::present
{
    // Whether GT4_VK_PRESENT asks for this presenter on the Vulkan device's
    // card (the device must exist: gsvk::rendererUsable first). Logs why.
    bool wanted();

    // Host thread, the window made, before the renderer starts: the surface,
    // swapchain and pipelines. False (logged): the OpenGL presentation stays.
    bool start(void *hwnd, bool headless);
    // The presenter runs (start succeeded): the renderer publishes its frames
    // here, the host loop draws through it.
    bool active();
    void shutdown();

    // ---- the host loop, each frame

    struct Frame
    {
        uint32_t width = 0, height = 0; // the picture's texels in the image
        int texWidth = 0, texHeight = 0; // the image
        int parity = -1, mode = 0;      // gsgl::FieldInfo
        uint32_t crt = 0;
        int lines = 0;
    };
    // The newest published frame (with grid pacing, maxTick: the newest whose
    // latch tick is <= maxTick, as gsgl::hostCopyLatestFrame): 1 a new one is
    // held now, 0 none new (the one held stays; frame describes it when there
    // is one), -1 no frame yet.
    int takeLatest(uint64_t maxTick, Frame &frame);

    // The scanline pass's uniforms (present_scanline.cpp's shader).
    struct ScanUniforms
    {
        float texSize[2] = {0, 0};
        int rowsPerLine = 1, lines = 448, cols = 640;
        float halfPixel = 0.5f, mixBeam = 0.0f, widthK = 1.0f, field = 0.0f;
        int parity = -1, filterMode = 3, blendLines = 0;
        float lutSize = 2.0f, lutMix = 0.0f;
    };
    // The held picture's top-left srcW x srcH texels with the scanline pass into
    // the window rectangle (x, y, w, h: window pixels).
    void drawScan(const ScanUniforms &u, float srcW, float srcH, float x, float y, float w, float h);
    // Texels (u0, v0)-(u1, v1) of the held picture scaled into the rectangle.
    void drawPlain(float u0, float v0, float u1, float v1, float x, float y, float w, float h, bool linear);
    // The scanline pass's colour LUT (Pass::lutStrip: n*n x n RGB floats; null: none).
    void setLut(const float *strip, int points);

    // An overlay drawn this frame covers this window rectangle (huge: the whole window).
    void overlayRect(float x, float y, float w, float h);
    // Shows the frame: the pictures drawn above, then the overlays read back
    // from the OpenGL framebuffer `glFramebuffer` (fbW x fbH; raylib's render
    // texture, rows bottom up) over them. The OpenGL context is current.
    // shot: the window as shown is also read back (takeShot).
    void presentFrame(unsigned glFramebuffer, int fbWidth, int fbHeight, int windowWidth, int windowHeight, bool shot = false);
    // The last shot (RGBA, top row first). False: none.
    bool takeShot(std::vector<uint8_t> &rgba, int &width, int &height);
    // One line for logs and the FPS counter ("Vulkan swapchain, mailbox, 3 images").
    const char *describe();
}
