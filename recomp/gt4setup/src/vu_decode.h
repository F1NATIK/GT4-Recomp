// Port of recomp/gt3-tools/vu1_decode.py: VU microcode decoder mirroring
// ps2xRuntime/src/lib/vu (decodeUpperUsage/decodeLowerUsage/decodeInstructionPair)
// and its disassembler (fmt_pair).
#pragma once
#include "util.h"

namespace gt4setup {
namespace vu {

constexpr int FMAC_LATENCY = 4;
enum Pipe { PIPE_NONE, PIPE_FMAC, PIPE_LSU, PIPE_FDIV, PIPE_EFU, PIPE_IALU, PIPE_BRANCH, PIPE_XGKICK };

inline int lane(int component) { return 1 << (3 - component); }
inline int DEST(uint32_t i) { return (i >> 21) & 0xF; }
inline int FT(uint32_t i) { return (i >> 16) & 0x1F; }
inline int FS(uint32_t i) { return (i >> 11) & 0x1F; }
inline int FD(uint32_t i) { return (i >> 6) & 0x1F; }
inline int VIT(uint32_t i) { return (i >> 16) & 0xF; }
inline int VIS(uint32_t i) { return (i >> 11) & 0xF; }
inline int VID(uint32_t i) { return (i >> 6) & 0xF; }
inline int IMM11(uint32_t i)
{
    const int v = static_cast<int>(i & 0x7FF);
    return (v & 0x400) ? v - 0x800 : v;
}

struct Usage {
    int vfReadReg[2] = {0, 0};
    int vfReadLanes[2] = {0, 0};
    int vfReadCount = 0;
    int vfWriteReg = 0, vfWriteLanes = 0;
    uint32_t viRead = 0, viWrite = 0;
    int accRead = 0, accWrite = 0;
    int latency = 0, vfLatency = 0, viLatency = 0;
    int pipeline = PIPE_NONE;
    bool waitQ = false, waitP = false, readsClip = false, writesClip = false;
    bool delaysNextBranchRead = false, reserved = false;

    void addVfRead(int reg, int lanes);
    void addVfWrite(int reg, int lanes);
    int vfReadLanesOf(int reg) const;
};

Usage upperUsage(uint32_t upper);
Usage lowerUsage(uint32_t lower);

struct Pair {
    uint32_t pc = 0, lower = 0, upper = 0;
    bool iBit = false, eBit = false, mBit = false, dBit = false, tBit = false;
    Usage uu, lu;
    int upperShadow = 0;
    int suppressedLowerVf = 0;
};

Pair decodePair(const uint8_t *code, uint32_t pc);

std::string upperName(uint32_t upper);
std::string lowerName(uint32_t lower);
std::string fmtPair(const Pair &p);

// load_code: the first code image of a GT3VU1K1 kick recording, else the first 16 KiB.
Bytes loadCode(const Bytes &file);

} // namespace vu
} // namespace gt4setup
