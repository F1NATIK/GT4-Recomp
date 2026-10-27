#pragma once

// Overlays drawn over GT4's picture by the host loop (the FPS counter, the cheat card, notes) say
// which window rectangle they cover. With the Vulkan renderer's window presenter
// (gs/gs_vk_present.h) only those rectangles are read back from OpenGL and drawn over the
// picture; otherwise nothing listens. x, y, w, h: window pixels; mark(0, 0, kAll, kAll): all of it.
namespace ps2x::overlay
{
    using Hook = void (*)(float x, float y, float w, float h);
    inline constexpr float kAll = 1e9f;
    inline Hook &hook()
    {
        static Hook h = nullptr;
        return h;
    }
    inline void mark(float x, float y, float w, float h)
    {
        if (Hook f = hook())
            f(x, y, w, h);
    }
}
