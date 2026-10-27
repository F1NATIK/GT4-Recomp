#pragma once

// GT4_CHEATS as the IOP's script overlay (src/modules/gt4_cheat_overlay.cpp) actually serves it, for
// GT4.exe's in-game cheat card (ps2xRuntime gt4_cheat_card.cpp). The card must not claim a switch that
// GT4 is not running: after a failed check (another disc or version: the file is served unchanged) or
// the noday + missionfix -> noday fallback, the card shows GT4's stock behaviour. GT4 builds only.

#include <cstdint>

namespace ps2x::iop::gt4::cheats
{
    enum ServedBit : uint8_t
    {
        ServedNoday = 1,
        ServedSecret = 2,
        ServedMissionfix = 4,
    };

    struct ServedState
    {
        bool known = false; // the overlay has been built: on the first disc read, ready about 0.2 s later
        uint8_t asked = 0;  // GT4_CHEATS as the overlay parsed it (ServedBit)
        uint8_t served = 0; // what GT4 is given (ServedBit); only meaningful when known
    };

    // GT4_CHEATS parsed by the overlay's own parser (ServedBit); unknown words are left out.
    uint8_t askedSwitches();
    // The overlay's result; known = false until it is built (or when the disc was never read).
    ServedState servedState();

    // GT4.exe (GT4_RACE_MODE=480p, analysis/design/480p-colour/DESIGN.md): races always run in 480p, so the
    // overlay serves option.adc without GT4's Video Output line (src/modules/gt4_cheat_overlay.h, video-output).
    // Said once, after CORE is loaded and its words are checked, before GT4 runs: the overlay is built at the
    // first disc read after it (a read before it that touches no script file passes unchanged). Never said
    // (GT3, tests): shown, as GT4 has it.
    void setVideoOutputHidden(bool hidden);
}
