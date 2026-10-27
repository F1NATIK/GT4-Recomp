// GT4 movie stutter fix: see gt4_movie_feed.h and analysis/design/movie-stutter/REPORT.md.
//
// GT4's movie producer thread (0x54e3d8, priority 113, above the decoder thread's 114) moves the demuxed video records
// into the IPU video buffer with feed 0x54fbb8, which copies each record's payload with GT4's memcpy 0x5a4724 into the
// buffer's uncached mirror (0x2xxxxxxx). The payloads are not 16-byte aligned, so memcpy takes its byte loop
// (0x5a47b0: lbu / sb, one generated back-edge per byte = 32 charged EE cycles per byte). When the video buffer runs low
// the producer refills it in one go (~30 reads of 0xa000 bytes, ~1.2 MB): at 32 cycles a byte that is ~130 ms of EE
// time, 8 VSyncs in which the decoder (lower priority) gets no time, the decoded picture queue runs dry and the shown
// picture holds for 6-11 VSyncs instead of 2. That is the stutter, once a second in the opening movie and about every
// 0.6 s in the course-select clips.
//
// The fix copies those three calls natively and charges 1 EE cycle per byte, what the runtime charges for GT4's own
// aligned memcpy path (its qword loop: 32 cycles per 32 bytes). Everything else (the producer's pacing, the reader, the
// decoder, the sound) runs as before; only those copies take less EE time.
#include "gt4_movie_feed.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4movie
{
#if defined(PS2X_GAME_GT4)
namespace
{
constexpr uint32_t kMemcpy = 0x5a4724u;
// Return addresses of feed 0x54fbb8's three memcpy calls (jal at 0x54fc78 / 0x54fca0 / 0x54fcc4).
constexpr uint32_t kFeedReturns[3] = {0x54fc80u, 0x54fca8u, 0x54fccc};
constexpr uint32_t kRam = 0x02000000u;

PS2Runtime::RecompiledFunction g_memcpy = nullptr;

// Main RAM through KSEG0/1 or the uncached mirrors (0x2000_0000, 0x3000_0000); the whole range must be in RAM.
bool ramRange(uint32_t address, uint32_t size, uint32_t &offset)
{
    const uint32_t segment = address >> 28;
    if (segment != 0x0u && segment != 0x2u && segment != 0x3u && segment != 0x8u && segment != 0xau)
        return false;
    offset = address & 0x0fffffffu;
    return offset >= 0x10000u && uint64_t(offset) + size <= kRam;
}

void memcpyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = GPR_U32(ctx, 31);
    if (ra == kFeedReturns[0] || ra == kFeedReturns[1] || ra == kFeedReturns[2])
    {
        const uint32_t dst = GPR_U32(ctx, 4), src = GPR_U32(ctx, 5), size = GPR_U32(ctx, 6);
        uint32_t d = 0, s = 0;
        // A record is at most 0x13f0 bytes; anything else (or overlapping ranges) runs GT4's own copy.
        if (size <= 0x2000u && ramRange(dst, size, d) && ramRange(src, size, s) && (d + size <= s || s + size <= d))
        {
            std::memcpy(rdram + d, rdram + s, size);
            SET_GPR_U32(ctx, 2, dst); // memcpy returns the destination
            runtime->eeScheduler().accountCycles(size);
            ctx->pc = ra;
            return;
        }
    }
    g_memcpy(rdram, ctx, runtime);
}
} // namespace

void installFeedCopy(PS2Runtime &runtime)
{
    if (const char *v = std::getenv("GT4_MOVIE_FEED_COPY"); v && *v == '0')
    {
        std::fprintf(stderr, "[gt4-movie] GT4_MOVIE_FEED_COPY=0: GT4's own movie feed copy (byte loop)\n");
        return;
    }
    uint8_t *rdram = runtime.memory().getRDRAM();
    const auto u32 = [rdram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, rdram + a, 4);
        return v;
    };
    // jal 0x5a4724 at the three feed sites, the feed's uncached-mirror OR (lui 0x2000), memcpy's first words and its
    // byte loop (lbu / sb).
    const bool ok = rdram && u32(0x54fc78u) == 0x0c1691c9u && u32(0x54fca0u) == 0x0c1691c9u && u32(0x54fcc4u) == 0x0c1691c9u &&
                    u32(0x54fc6cu) == 0x3c032000u && u32(0x54fc90u) == 0x3c112000u && u32(kMemcpy) == 0x0080402du &&
                    u32(0x5a4728u) == 0x2cc20020u && u32(0x5a47b0u) == 0x90a20000u && u32(0x5a47bcu) == 0xa0620000u;
    g_memcpy = runtime.lookupFunction(kMemcpy);
    if (!ok || !g_memcpy)
    {
        std::fprintf(stderr, "[gt4-movie] CORE does not match (movie feed): GT4's own movie feed copy\n");
        return;
    }
    runtime.registerFunction(kMemcpy, &memcpyHook);
    std::fprintf(stderr, "[gt4-movie] movie feed copy: native (the video records into the IPU buffer)\n");
}
#else
void installFeedCopy(PS2Runtime &) {}
#endif
} // namespace gt4movie
