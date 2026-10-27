#pragma once

// GS dithering (DTHE / DIMX), GT4 builds (analysis/tb2-fixes/dither): with DTHE.DTHE = 1 and a 16-bit frame
// buffer (PSMCT16 / PSMCT16S, and a Z16 format used as a frame), the GS adds the dither matrix entry of the
// pixel's frame position, DIMX DM[y & 3][x & 3] (3 bits, signed: -4..3), to each of R, G and B after alpha
// blending, then applies COLCLAMP (1: clamp to 0..255; 0: keep the low 8 bits) and only then drops each
// channel to 5 bits. 32-bit frames are never dithered. GT4 dithers its 480p and 1080i races: the copy of the
// 32-bit picture into the 16-bit display buffer has DTHE = 1, with two DIMX matrices taken in turn frame by
// frame; 480i never sets DTHE = 1 (PS2X_GS_DITHER_LOG census, analysis/tb2-fixes/dither/README.md).
// Shared by the CPU reference renderer (gs_cpu_backend.cpp) and the GL renderer (gs_gl_backend.cpp host side;
// the shader's ditherOffset in gs_gl_shaders.h repeats offset() with the same integer operations), so both
// give the same pixels.
//
// Switch (read once): PS2X_GS_DITHER=0 turns dithering off (A/B; GT4 default on). GT3 builds: always off.

#include "runtime/gs/gs_types.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace gsdither
{
    inline bool enabled()
    {
#if defined(PS2X_GAME_GT4)
        static const bool on = [] {
            const char *v = std::getenv("PS2X_GS_DITHER");
            const bool value = !(v && *v == '0');
            std::fprintf(stderr, "[gs-dither] GS dithering %s (DTHE on 16-bit frames; PS2X_GS_DITHER=0 off)\n", value ? "on" : "off");
            return value;
        }();
        return on;
#else
        return false;
#endif
    }

    inline bool frame16(uint32_t fpsm)
    {
        return fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S || fpsm == GS_PSM_Z16 || fpsm == GS_PSM_Z16S;
    }

    // The draw is dithered: DTHE on, a 16-bit frame, and the switch on. A full-colour draw (GT4_RACE_COLOUR
    // 32, gs_fullcolour.h: computed as a CT32 frame) never is.
    inline bool applies(const GSDrawState &state)
    {
        return (state.dthe & 1u) != 0u && state.fc == 0u && frame16(state.context.frame.psm) && enabled();
    }

    // DIMX DM[y & 3][x & 3]: row y in bits 16y..16y+15, entry x in bits 4x..4x+2, signed.
    inline int offset(uint64_t dimx, int x, int y)
    {
        const int v = static_cast<int>((dimx >> ((y & 3) * 16 + (x & 3) * 4)) & 7u);
        return v >= 4 ? v - 8 : v;
    }

    // One dithered channel: the (unclamped) colour plus the offset, then COLCLAMP.
    inline uint8_t channel(int value, int d, bool clamp)
    {
        const int v = value + d;
        if (!clamp)
            return static_cast<uint8_t>(v & 0xFF);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}
