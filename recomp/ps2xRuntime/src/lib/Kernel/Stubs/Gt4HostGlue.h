#pragma once

// GT4 runtime glue (PS2X_GAME_GT4 builds): the POWOFF quit handler and window
// close -> power button, the LGDEV host wheel (DirectInput, visible play
// only), and CORE's libmrpc functions answered natively by RTINETB with
// deferred completion through the EE scheduler. The IOP side is
// ps2xIOP/include/ps2x/iop/gt4_host_glue.h; notes in
// analysis/extras/wheel-glue/README.md.

#include <cstdint>

class PS2Runtime;

namespace ps2_gt4_glue
{
    // loadELF, before the IOP profile is created (LGDEV asks for its host wheel then).
    void beforeIopProfile(PS2Runtime &runtime);
    // loadELF, after the profile and the generated function table: the libmrpc
    // replacements when GT4_NET / GT4_NET_MRPC want them (GT4's CORE.elf only).
    void afterLoad(PS2Runtime &runtime, uint32_t entry, uint32_t crc32);
    // ~PS2Runtime: drop the handlers that point at this runtime.
    void shutdown();
    // The window was asked to close. True: it became GT4's power button (keep presenting;
    // the power-off sequence requests the stop at the next VBlank). False: stop now.
    bool windowCloseToPowerButton();
}
