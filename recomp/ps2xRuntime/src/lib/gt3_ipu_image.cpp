#include "runtime/gt3_ipu_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ipu_idec.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace ps2x::video {
namespace {
// Verified CORE constants (see gt3-tools/apply_ipu_image_hooks.py).
constexpr uint32_t kGp = 0x3579f0;
constexpr uint32_t kIntraMatrix = 0x2ff0d0;   // 264530 loads it with SETIQ (intra)
constexpr uint32_t kThreshold0 = kGp - 0x472c; // 2641f0 issues SETTH from these
constexpr uint32_t kThreshold1 = kGp - 0x4728;
constexpr uint32_t kState = kGp - 0x4718;      // pending DMA/IPU bits, 0 when done
constexpr size_t kHeaderBytes = 17;            // 17 FDEC(8) skip the header and flag byte

// EE address as the DMA sees it (MADR & 0x0fffffff), inside main RAM.
bool ramRange(uint32_t address, uint64_t bytes, uint32_t& offset)
{
    offset = address & 0x0fffffffu;
    return offset + bytes <= PS2_RAM_SIZE;
}
} // namespace

bool tryGt3IpuImageDecode(PS2Runtime& runtime, R5900Context& context)
{
    if (!runtime.gt3CoreVerified())
        return false; // unverified image: original code
    auto* ctx = &context;
    if (GPR_U32(ctx, 28) != kGp)
        return false;
    uint8_t* ram = runtime.memory().getRDRAM();
    const uint32_t destination = GPR_U32(ctx, 4), image = GPR_U32(ctx, 5);

    uint32_t imageOffset = 0, outOffset = 0;
    bool ok = ramRange(image, 17, imageOffset);
    uint32_t size = 0, outBytes = 0, inputBytes = 0;
    int width = 0, height = 0;
    uint8_t flags = 0;
    if (ok) {
        // As the original: lwl/lwr size at +4, signed halves at +8/+10, flags at +0x10.
        std::memcpy(&size, ram + imageOffset + 4, 4);
        int16_t w, h;
        std::memcpy(&w, ram + imageOffset + 8, 2);
        std::memcpy(&h, ram + imageOffset + 10, 2);
        width = w;
        height = h;
        flags = ram[imageOffset + 16];
        outBytes = static_cast<uint32_t>(width * height) << 2;
        inputBytes = (((size + 15u) >> 4) + 1u) << 4; // toIPU QWC
        ok = width > 0 && height > 0 && inputBytes > kHeaderBytes && ramRange(image, inputBytes, imageOffset) &&
             ramRange(destination, outBytes, outOffset);
    }
    if (!ok) {
        std::cerr << "[GT3 IPU image] rejected image 0x" << std::hex << image << " -> 0x" << destination << std::dec
                  << " (" << width << "x" << height << ", size " << size << "); output left unwritten\n";
    } else {
        IpuIdecParams params;
        // IPU_CTRL = (flags & 0xfb) << 16; IDEC = 0x10010000 | (flags >> 2 & 1) << 24.
        params.intraDcPrecision = flags & 3;
        params.alternateScan = (flags >> 4) & 1;
        params.intraVlcFormat = (flags >> 5) & 1;
        params.nonLinearQuant = (flags >> 6) & 1;
        params.mpeg1 = (flags >> 7) & 1;
        params.dctTypeDecode = (flags >> 2) & 1;
        params.quantizerScaleCode = 1;
        params.th0 = runtime.memory().read32(kThreshold0) & 0x1ff;
        params.th1 = runtime.memory().read32(kThreshold1) & 0x1ff;
        std::memcpy(params.intraMatrix, ram + kIntraMatrix, 64);

        const auto start = std::chrono::steady_clock::now();
        IpuBitReader bits(ram + imageOffset, inputBytes, kHeaderBytes * 8);
        IpuIdecResult result;
        std::string error;
        bool decoded;
        if (outBytes % 1024u == 0) {
            decoded = ipuDecodeIdecRgb32(bits, params, ram + outOffset, outBytes, result, error);
        } else {
            // The DMA stops part way through the last macroblock.
            std::vector<uint8_t> staging((outBytes + 1023u) & ~1023u);
            decoded = ipuDecodeIdecRgb32(bits, params, staging.data(), staging.size(), result, error);
            std::memcpy(ram + outOffset, staging.data(), std::min<size_t>(outBytes, result.macroblocks * 1024u));
        }
        const size_t macroblocks = result.macroblocks;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const size_t expected = (outBytes + 1023u) / 1024u;
        if (!decoded || macroblocks != expected) {
            std::cerr << "[GT3 IPU image] decode stopped at macroblock " << macroblocks << "/" << expected
                      << " of 0x" << std::hex << image << std::dec << " (" << width << "x" << height
                      << "): " << (error.empty() ? "stream ended early" : error) << '\n';
        } else {
            std::cerr << "[GT3 IPU image] 0x" << std::hex << image << " -> 0x" << destination << std::dec << " "
                      << width << "x" << height << " flags 0x" << std::hex << unsigned(flags) << std::dec << ", "
                      << macroblocks << " macroblocks in " << ms << " ms\n";
        }
    }
    // The original finishes with its pending bits clear; its return value is unused.
    runtime.memory().write32(kState, 0);
    SET_GPR_U32(ctx, 2, 0);
    ctx->pc = GPR_U32(ctx, 31);
    return true;
}
} // namespace ps2x::video
