#pragma once
class PS2Runtime;
struct R5900Context;
namespace ps2x::video {
// GT3 USA 0x264e58 (called through 0x1cc6b8): decode one "ipum" still image
// (a0 = RGB32 destination, a1 = image) with the IPU's IDEC. Replaced natively on
// the SHA-256 verified CORE: the macroblock stream is decoded here and written
// exactly as the fromIPU DMA would, then the call returns. The IPU itself is
// not modelled, so the original would sleep forever waiting for its interrupts.
bool tryGt3IpuImageDecode(PS2Runtime& runtime, R5900Context& context);
}
