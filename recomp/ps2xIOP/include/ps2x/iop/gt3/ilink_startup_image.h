#pragma once
#include "ps2x/iop/iop_host.h"

namespace ps2x::iop::gt3 {
inline constexpr uint32_t IlinkOriginalModelBytes=0x1a500;
bool verifyIlinkModule(IopHost&);
// Hash-gated original rodata/data and402 data pointer relocations only. Caller
// owns an allocated, unpublished image and discards it on failure. This does
// not execute original instructions or initialize the physical link.
bool initializeIlinkImage(IopHost&,uint32_t image);
}
