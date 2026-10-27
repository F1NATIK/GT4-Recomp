#pragma once
#include "ps2x/iop/iop_host.h"
#include <array>
#include <span>

namespace ps2x::iop::gt3 {
inline constexpr size_t PadmanPortBytes=0x1c8;
inline constexpr size_t PadmanFrameBytes=0x80;
struct PadmanPublication {
    std::array<uint8_t,PadmanPortBytes> port{};
    uint32_t eeTarget=0;
};
// Supplied PADMAN354/390c publication. Pure staging only: caller owns a
// verified module/port and frame scheduling. Valid frames are supported for
// the identities the native model reads: basic digital 4x and analog 7x
// (DualShock 2 analog 73, pressure 79); others remain unsupported.
bool stagePadmanPublication(std::span<const uint8_t> port,PadmanPublication& result);
// Strict allocated-IOP read, then one atomic port-state/EE-record commit.
// No padInit, port-open or frame-timing support is implied by this helper.
bool commitPadmanPublication(IopHost& host,uint32_t iopPort);
}
