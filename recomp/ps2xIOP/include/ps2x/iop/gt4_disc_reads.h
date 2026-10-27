#pragma once

// GT4's disc reads as the IOP serves them (src/modules/gt4_common.cpp, Disc::readSectors / readBytes), for GT4.exe's
// texture dump (ps2xRuntime gt4_texture_owner.cpp: which car or course GT4 loaded). An observer is told the byte
// range of the disc image every successful read covered, on the IOP's thread with the disc's mutex held: it must be
// quick and must not read the disc through the IOP. None set (the default): nothing is called. GT4 builds only.

#include <cstdint>

namespace ps2x::iop::gt4
{
    using DiscReadObserver = void (*)(uint64_t offset, uint64_t bytes);
    void setDiscReadObserver(DiscReadObserver observer);
}
