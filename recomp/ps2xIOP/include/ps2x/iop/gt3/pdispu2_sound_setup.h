#pragma once
#include "ps2x/iop/gt3/libsd_resident.h"

namespace ps2x::iop::gt3 {
bool verifyPdispu2Module(IopHost&);
std::vector<uint8_t> readPdispu2Module(IopHost&);
// Requires an allocated PDISPU2 image and a registered native callback at+9d8.
// On first sound mutation, token receives ownership of the entry checkpoint.
// The module owner must finish it after all remaining startup succeeds, or
// restore it on failure/teardown (retry failed restoration before a new start).
// This helper does not register/load PDISPU2 or create its workers/RPC services.
bool initializePdispu2Sound(IopHost&,LibsdResident&,uint32_t image,uint64_t& token);
// Original9d8/ac4 initial callback subset: empty per-core callback table, followed
// by real LIBSD11 interrupt disable/enable. Nonempty secondary callbacks reject.
NativeExportResult pdispu2InitialSoundInterrupt(IopHost&,uint32_t image,const std::array<uint32_t,4>&);
}
