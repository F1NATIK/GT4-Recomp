#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

// GT4's 3D car in its 480p menus drawn at full height (analysis/design/menus-480p/CAR-PICTURE.md section 8).
// GT4 draws its car views (Arcade's car pages and colour select, GT Mode's dealer car pages) into render targets
// of 480i's field shape, 640 x 224 (the "D5 car picture" hooks in ps2_runtime.cpp), and stretches them over its
// 448-line menus: every second line of the car is a repeat. The GPU renderer gives those targets (and the depth
// buffers they are drawn with) twice the samples down each GS pixel, so the stretched car has a line of its own
// for every display line at the internal resolution.
namespace gt4carview
{
    // Set by the runtime (EE thread) while GT4's menus are 480p, the D5 car picture is on and no race session runs;
    // read by the renderer for each draw (it changes only between screens, so a frame of lag does not matter).
    inline std::atomic<bool> g_armed{false};
    inline bool armed() { return g_armed.load(std::memory_order_relaxed); }

    // Deep Z (developer 2026-10-17, "cars still flickering in the showroom"): GT4's 480p car views use its Z16 buffer, and
    // GT4 scales the car's Z for 16 bits (the largest Z, scratchpad 0x7000223c, 65535): the whole car takes about 150
    // Z steps and decals fight the body (flicker as the car turns). While the renderer draws the car views tall
    // (g_tallActive, set by the renderer), the runtime gives the car passes Z24's scale (16776191, GT4's own value for
    // PSMZ24) and the renderer's tall depth keeps those Z values unclamped. GT4_MENU_480P_CAR_DEEPZ=0: off.
    inline std::atomic<bool> g_tallActive{false};
    inline bool deepZAllowed()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_MENU_480P_CAR_DEEPZ");
            return !(v && std::strcmp(v, "0") == 0);
        }();
        return on;
    }

    // GT4_MENU_480P_CAR_TALL=0 (development): the car views as GT4 draws them (224 rows, stretched).
    // (Off for a day after the developer's Test Build 30 report, 2026-10-17: half the car missing, a black block and a striped
    // band after GT Mode's car pages. The cause was the renderer's target index: a dropped tall target removed the
    // normal target's entry and left its own naming a reused slot (gs_gl_backend.cpp, targetIndexKey). On again.)
    inline bool allowed()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_MENU_480P_CAR_TALL");
            return !(v && std::strcmp(v, "0") == 0);
        }();
        return on;
    }
}
