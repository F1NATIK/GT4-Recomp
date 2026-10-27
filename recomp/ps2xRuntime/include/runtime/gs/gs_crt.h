#pragma once

#include <atomic>
#include <cstdint>

// The video mode the game chose with the SetGsCrt syscall (interlace, mode,
// field/frame), which the GS registers alone do not tell (SMODE1 is the
// BIOS's): the presenters need it to know a 1080i or 480p picture from a
// 480i one (analysis/design/video-modes/VIDEO-MODES.md 3.1).
//
// Packed as one word: bits 0-7 the mode (0x02 NTSC, 0x03 PAL, 0x50 480p,
// 0x51 1080i, 0x52 ...), bit 8 interlaced, bit 9 FFMD (frame mode), bit 31
// set once SetGsCrt was called (0: never called, mode unknown).
namespace ps2x::crt
{
    constexpr uint32_t kKnown = 0x80000000u;
    constexpr uint32_t kNtsc = 0x02u;
    constexpr uint32_t kPal = 0x03u;
    constexpr uint32_t k480p = 0x50u;
    constexpr uint32_t k1080i = 0x51u;

    constexpr uint32_t pack(uint32_t interlace, uint32_t mode, uint32_t ffmd)
    {
        return kKnown | (mode & 0xFFu) | ((interlace & 1u) << 8) | ((ffmd & 1u) << 9);
    }
    constexpr bool known(uint32_t crt) { return (crt & kKnown) != 0u; }
    constexpr uint32_t mode(uint32_t crt) { return crt & 0xFFu; }
    constexpr bool interlaced(uint32_t crt) { return (crt & 0x100u) != 0u; }
    constexpr bool frameMode(uint32_t crt) { return (crt & 0x200u) != 0u; }
    constexpr bool is1080i(uint32_t crt) { return known(crt) && mode(crt) == k1080i; }
    constexpr bool is480p(uint32_t crt) { return known(crt) && mode(crt) == k480p; }

    // A name for logs: "480i", "576i", "480p", "1080i", "mode 0xNN", "unknown".
    inline const char *name(uint32_t crt)
    {
        if (!known(crt))
            return "unknown";
        switch (mode(crt))
        {
        case kNtsc: return "480i";
        case kPal: return "576i";
        case k480p: return "480p";
        case k1080i: return "1080i";
        default: return "other";
        }
    }

    // The mode as SetGsCrt last set it (EE thread writes, any thread reads).
    inline std::atomic<uint32_t> g_current{0u};
    inline uint32_t current() { return g_current.load(std::memory_order_acquire); }
    // Returns the previous value.
    inline uint32_t set(uint32_t crt) { return g_current.exchange(crt, std::memory_order_acq_rel); }

    // The picture's shape (width / height) as a TV showed it: 4:3 for 480i,
    // 480p and PAL; 1080i's picture alone (1728 of 1920 HD pixels across, 960
    // of 1080 lines; GT4's DISPLAY setup) inside a 16:9 frame: 1.8:1.
    constexpr float pictureAspect(uint32_t crt) { return is1080i(crt) ? 1.8f : 4.0f / 3.0f; }
}
