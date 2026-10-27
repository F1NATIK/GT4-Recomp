#pragma once
#include "ps2x/iop/iop_host.h"

namespace ps2x::iop::gt3 {
inline constexpr uint32_t PdistrOriginalModelBytes=0x3580;
bool verifyPdistrModule(IopHost& host);
// Caller exclusively owns an unpublished allocation of at least ModelBytes.
// Prepares original data/relocations and invokes the real PDISPU2 volume export.
// Does not start workers, register PDISTR or set its initialized flag. Discard
// the unpublished image on failure; successful earlier writes are not undone.
bool initializePdistrImage(IopHost& host,uint32_t image);
}
