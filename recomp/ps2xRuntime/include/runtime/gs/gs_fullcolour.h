#pragma once

// GT4's race colour (analysis/design/480p-colour/DESIGN.md D2; investigation inv-colour.md), GT4 builds:
// GT4_RACE_COLOUR=16|32 (default 32). In its 480p and 1080i modes GT4 draws the 3D picture in 32-bit colour
// (CT32) and copies it into a 16-bit display buffer (CT16, with GS dithering), then draws the HUD on it.
//   16: GT4's own 16-bit picture, exactly as a PS2 makes it (dithered; nothing changes).
//   32: every draw into a 16-bit display buffer in those modes is computed as the GS computes a 32-bit (CT32)
//       frame: no dithering, colour and alpha in 8 bits, DATE on alpha bit 7, RGB_ONLY keeping alpha.
// (24, a CT24 frame's rules, was dropped (developer, 2026-10-07): GT4 never uses the display buffers' alpha, so it
// gave exactly the 32-bit picture. An old "24" reads as 32, logged once.)
// GS memory keeps the 16-bit reduction of every such pixel (r >> 3 ..., alpha bit 15), so whatever reads the
// memory as 16-bit (textures, CLUT loads, transfers, other formats over the same pages) sees a normal CT16
// picture, only undithered. The full colour lives beside it, one RGBA8 word per halfword of GS memory (the
// plane, 8 MiB): a pixel's full colour F is the plane word S while reduce(S) equals the halfword M in memory,
// else expand(M) (v << 3). Anything else that changes M switches the pixel back to 16-bit colour by itself.
// The presenters show F for a 16-bit display buffer in 480p and 1080i (32).
// The decision is made once per draw in the GS frontend (GSDrawState::fc), so the CPU reference renderer and
// the GL renderer (and Vulkan, which runs the GL renderer's code) always agree which draws are full colour.
// Only GT4's display buffers are kept in full colour: 16-bit frames at least 576 pixels wide (FBW >= 9: GT4's
// 480p buffers are FBW 10, its 1080i fields FBW 9). GT4's small 16-bit effect targets (FBW 1-4: colour grading
// tables, the colour select's effects, textures it reads back) keep GT4's own 16-bit rules, as on a PS2.
// 480i (GT4's own 480i menus and races: 24-bit display buffers) is never full colour.
// GT3 builds: always 16 (fc is never set).

#include "runtime/gs/gs_crt.h"
#include "runtime/gs/gs_types.h"

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gsfc
{
    // One plane word per 16-bit halfword of the 4 MiB of GS memory.
    constexpr size_t kPlaneWords = 2u * 1024u * 1024u;

    // GT4_RACE_COLOUR's value: 16 or 32; empty (or not set) is 32; 24 for an old "24" (no longer offered: the
    // caller logs it and takes 32); 0 for anything else.
    inline int parse(const char *v)
    {
        if (!v)
            return 32;
        std::string s;
        for (const char *p = v; *p; ++p)
            if (*p != ' ' && *p != '\t')
                s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
        if (s.size() > 3 && s.compare(s.size() - 3, 3, "bit") == 0)
            s.resize(s.size() - 3);
        if (!s.empty() && s.back() == '-')
            s.pop_back();
        if (s.empty())
            return 32;
        if (s == "16")
            return 16;
        if (s == "24")
            return 24;
        if (s == "32")
            return 32;
        return 0;
    }

    // The race colour a GT4_RACE_COLOUR value gives (16 or 32): unknown values and an old 24 are 32.
    inline int resolve(int parsed) { return parsed == 16 ? 16 : 32; }

    // Unit tests set the mode (16 / 32) without the environment; 0: the setting again.
    inline std::atomic<int> &testMode()
    {
        static std::atomic<int> m{0};
        return m;
    }
    inline void setModeForTests(int m) { testMode().store(m, std::memory_order_relaxed); }

    // The race colour, read once (16 or 32). One [gs-colour] line at the first call.
    inline int mode()
    {
#if defined(PS2X_GAME_GT4)
        if (const int forced = testMode().load(std::memory_order_relaxed))
            return forced;
        static const int m = [] {
            const char *v = std::getenv("GT4_RACE_COLOUR");
            const int parsed = parse(v);
            if (parsed == 0)
                std::fprintf(stderr, "[gs-colour] GT4_RACE_COLOUR=%s not understood (16 or 32): 32-bit\n", v);
            else if (parsed == 24)
                std::fprintf(stderr, "[gs-colour] GT4_RACE_COLOUR=%s: 24-bit is no longer offered (it gave the 32-bit picture in GT4): 32-bit\n", v);
            const int p = resolve(parsed);
            std::fprintf(stderr, "[gs-colour] race colour %d-bit (GT4_RACE_COLOUR%s%s)%s\n", p, v && *v ? "=" : " not set", v && *v ? v : "",
                         p == 16 ? ": GT4's own 16-bit picture with GS dithering" : ": 16-bit display buffers kept in full colour in 480p/1080i");
            return p;
        }();
        return m;
#else
        return 16;
#endif
    }

    // The video modes whose 16-bit display buffers are kept in full colour.
    constexpr bool fullModes(uint32_t crt) { return ps2x::crt::is480p(crt) || ps2x::crt::is1080i(crt); }
    constexpr bool frame16(uint32_t psm) { return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }

    // The width of a display buffer: GT4's 480p (FBW 10) and 1080i (FBW 9) buffers, not its small effect targets.
    constexpr uint32_t kDisplayFbw = 9u;

    // The flag a draw carries (GSDrawState::fc): 32 for a CT16/CT16S display-buffer frame (FBW >= 9) while the
    // video mode SetGsCrt chose is 480p or 1080i and the race colour is 32; 0 otherwise (GT4's own 16-bit rules).
    inline uint8_t drawFlag(uint32_t framePsm, uint32_t fbw, uint32_t crt)
    {
        if (!frame16(framePsm) || fbw < kDisplayFbw || !fullModes(crt))
            return 0u;
        return mode() == 16 ? 0u : 32u;
    }

    // A texture read of a display buffer (GSDrawState::ftex): GT4's 480p / 1080i draws copy their CT16 display
    // buffer back as a texture (the car views' background copy into the car target, the blur planes; analysis/
    // design/menus-480p). Read as 16-bit values, the full colour drawn there would be lost again (banding on the
    // colour select's studio background). 1: a CT16 texture laid out as a display buffer (TBW >= 9, TBP on a page
    // boundary, so the GL renderer reads it from its 16-bit target) while the video mode is 480p or 1080i and the
    // race colour is 32; each texel is then its full colour F (alpha from the halfword's bit 15, TEXA as always).
    // 0 otherwise (race colour 16, 480i, GT4's small effect targets: the 16-bit value, as on a PS2).
    // GT4_RACE_COLOUR_TEXTURES=0: never (every texture read sees the 16-bit value, as before).
    inline bool texturesOn()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_RACE_COLOUR_TEXTURES");
            return !(v && *v == '0');
        }();
        return on;
    }
    inline uint8_t texFlag(bool tme, uint32_t texPsm, uint32_t tbp0, uint32_t tbw, uint32_t crt)
    {
        if (!tme || texPsm != GS_PSM_CT16 || tbw < kDisplayFbw || (tbp0 & 31u) != 0u || !fullModes(crt))
            return 0u;
        return (mode() != 16 && texturesOn()) ? 1u : 0u;
    }

    // The presenters show a display buffer of this format in full colour (F), not as 16-bit (v << 3 | v >> 2).
    inline bool shows(uint32_t displayPsm, uint32_t crt) { return frame16(displayPsm) && fullModes(crt) && mode() != 16; }

    // RGBA8 -> RGBA5551 (the GS's own reduction) and back (v << 3, alpha bit 15 as 0x80).
    constexpr uint16_t reduce(uint32_t c)
    {
        return static_cast<uint16_t>(((c >> 3) & 0x1Fu) | (((c >> 11) & 0x1Fu) << 5) | (((c >> 19) & 0x1Fu) << 10) | (((c >> 31) & 1u) << 15));
    }
    constexpr uint32_t expand(uint32_t v)
    {
        return ((v & 0x1Fu) << 3) | (((v >> 5) & 0x1Fu) << 11) | (((v >> 10) & 0x1Fu) << 19) | (((v >> 15) & 1u) << 31);
    }
    // A pixel's full colour from its plane word and its 16-bit value in memory.
    constexpr bool valid(uint32_t s, uint32_t m16) { return reduce(s) == (m16 & 0xFFFFu); }
    constexpr uint32_t full(uint32_t s, uint32_t m16) { return valid(s, m16) ? s : expand(m16); }
}
