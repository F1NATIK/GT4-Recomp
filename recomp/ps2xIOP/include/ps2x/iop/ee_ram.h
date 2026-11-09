#pragma once
#include <cstdint>

namespace ps2x::iop {
// Guest EE RAM the IOP may DMA into: the PS2's 32 MB plus the native build's 32 MB above it (the runtime's
// PS2_RAM_SIZE, runtime/ps2_memory.h; GT4 reads bigger courses there). The modeled IOP heap starts at 0x04000000.
inline constexpr uint32_t kEeRamBytes = 0x4000000u;
}
