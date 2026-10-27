#pragma once

// GT4 POWOFF 1.1 (PDI_PowerOff): the console power button mapped to a clean
// host quit. Self-contained (standard headers only) so the runtime can call it
// once the header is reachable (README in analysis/phase5b/lgdev).
//
// What GT4's module does [C, analysis/iop-sys/disasm/gt4/POWOFF.txt]: entry
// IOP+0xdc registers cdvdman #78 (power-off callback IOP+0x0, which only wakes
// the thread IOP+0x20). The thread then runs, in order:
//   1. devctl("pfs:", 0x5003)             close every PFS file (no HDD in GT4)
//   2. devctl("dev9x:", 0x4402) until 0 or -19, 1 ms apart   DEV9 (network, USB) off
//   3. cdvdman #74 until it succeeds with status 0, 1 ms apart CD power off
// There is no SIF RPC and the EE is never told.
//
// Host mapping (spec 3.2): pressPowerButton() is the callback; at the next
// VBlank start the POWOFF component runs Component::onPowerOff(stage) on every
// GT4 IOP component for each stage below, in this order, then calls the
// handler the runtime registered (requestStop(), exit code 0). Without a
// registered handler the press is logged and ignored, so a game that cannot
// quit is never left with its devices torn down.

#include <cstdint>
#include <functional>

namespace ps2x::iop::gt4
{
    enum class PowerOffStage : uint8_t
    {
        CloseFiles = 1, // POWOFF step 1 (pfs close-all): nothing in GT4 (no HDD)
        Dev9Off = 2,    // step 2: network sockets, USB storage files, wheel forces off and released
        CdPowerOff = 3, // step 3: disc streams, disc image, sound output
        MemoryCards = 4, // host extra (spec 3.2 item 4): close memory-card files before the quit
    };

    enum class PowerButtonSource : uint8_t
    {
        Test,        // PS2X_POWER_BUTTON_AT=<vsync>
        WindowClose, // window close button / Alt+F4 (runtime)
        Hotkey,      // optional key (runtime)
        Host,        // anything else
    };

    struct PowerOffReport
    {
        uint64_t vsync = 0;
        PowerButtonSource source = PowerButtonSource::Host;
    };

    const char *powerButtonSourceName(PowerButtonSource source);
    const char *powerOffStageName(PowerOffStage stage);

    // Runs on the EE scheduler thread inside the VBlank callback, after every
    // stage: request the stop (set a flag), never join threads here.
    using PowerOffHandler = std::function<void(const PowerOffReport &)>;
    void setPowerOffHandler(PowerOffHandler handler);
    bool hasPowerOffHandler();

    // Thread-safe. Like the CDVD power-off interrupt: only marks the press; the
    // sequence runs at the next VBlank start. Later presses are ignored.
    void pressPowerButton(PowerButtonSource source);
}
