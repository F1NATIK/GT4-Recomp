// Arcade's courses: the pack's added course offered, the launcher's hidden courses left out (gt4_tracks.h).

#include "gt4_tracks.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gt4tracks
{
    namespace
    {
        constexpr uint32_t kAddedCourses[] = {0xeeu, 0xc9u}; // tt_valencia, gt6_bathurst (build_tracks_pack.py COURSES)
        PS2Runtime::RecompiledFunction g_available = nullptr;
        std::vector<uint32_t> g_hidden;
        bool g_unlockAll = false; // GT4_TRACKS_UNLOCK=on: every course offered (gt4_tracks.h)

        uint32_t u32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4);
            return v;
        }

        void isCourseAvailable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint64_t code = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[5], 0));
            const uint32_t row = static_cast<uint32_t>(code);
            {
                static int left = [] {
                    const char *v = std::getenv("GT4_TRACKS_LOG");
                    return v && *v ? std::atoi(v) : 0;
                }();
                if (left > 0)
                    --left, std::fprintf(stderr, "[gt4-tracks] isCourseAvailable(0x%llx)\n", static_cast<unsigned long long>(code));
            }
            if (std::find(g_hidden.begin(), g_hidden.end(), row) != g_hidden.end())
            {
                SET_GPR_U32(ctx, 2, 0u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            if (g_unlockAll || std::find(std::begin(kAddedCourses), std::end(kAddedCourses), row) != std::end(kAddedCourses))
            {
                SET_GPR_U32(ctx, 2, 1u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_available(rdram, ctx, runtime);
        }
    }

    void courseRead(uint8_t *rdram, R5900Context *ctx)
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_COURSE_RAM");
            return !(v && *v == '0');
        }();
        const uint32_t block = GPR_U32(ctx, 5), size = GPR_U32(ctx, 6);
        if (!on || (size != 0xbc8000u && size != 0xac8000u))
            return;
        constexpr uint32_t kBase = PS2_GUEST_RAM_SIZE, kSize = PS2_RAM_SIZE - PS2_GUEST_RAM_SIZE;
        SET_GPR_U32(ctx, 5, kBase);
        SET_GPR_U32(ctx, 6, kSize);
        std::fprintf(stderr, "[gt4-tracks] course read at 0x%x (%u MB) instead of pool block 0x%x (0x%x); pool in use %u, free %u\n",
                     kBase, kSize >> 20, block, size, u32(rdram, 0x6214b0u + 0x10u), u32(rdram, 0x6214b0u + 0x14u));
    }

    void install(uint8_t *rdram, PS2Runtime *runtime)
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        if (const char *e = std::getenv("GT4_TRACKS_UNLOCK"); e && *e)
            g_unlockAll = std::strcmp(e, "off") != 0 && std::strcmp(e, "0") != 0;
        if (const char *e = std::getenv("GT4_TRACKS_HIDE"); e && *e)
            for (const char *c = e; *c;)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(c, &end, 0);
                if (end != c && v)
                    g_hidden.push_back(static_cast<uint32_t>(v));
                c = end != c ? end : c + 1;
                while (*c == ',' || *c == ' ' || *c == ';')
                    ++c;
            }
        // (GT4 USA 0x42eae8: addiu sp,-0x10 / addiu a2,a0,0x2008 / j 0x42ea80)
        if (u32(rdram, 0x42eae8u) != 0x27bdfff0u || u32(rdram, 0x42eaecu) != 0x24862008u ||
            !(g_available = runtime->lookupFunction(0x42eae8u)))
        {
            std::fprintf(stderr, "[gt4-tracks] CORE does not match (isCourseAvailable): no added or hidden courses\n");
            return;
        }
        runtime->registerFunction(0x42eae8u, &isCourseAvailable);
        std::fprintf(stderr, "[gt4-tracks] Arcade courses: %s; %zu hidden%s\n",
                     g_unlockAll ? "every course offered (GT4_TRACKS_UNLOCK)"
                                 : "added course codes offered (0xee tt_valencia, 0xc9 gt6_bathurst, when the pack has them)",
                     g_hidden.size(), g_hidden.empty() ? "" : " (GT4_TRACKS_HIDE)");
    }
}
