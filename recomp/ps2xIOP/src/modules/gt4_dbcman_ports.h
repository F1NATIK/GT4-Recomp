#pragma once

// GT4 DBCMAN, two controller ports (player 2: analysis/phase5b/player2). The parts of
// the DS2U_D model that do not depend on the RPC layer, shared by gt4_dbcman.cpp and
// its host unit test. GT4 creates a DBCMAN socket for port 0 and port 1 at boot (EE
// 0x551810) and polls both every frame (EE 0x551580); DS2U_D keeps exactly two device
// contexts and CORE never loads MTAPMAN, so two ports are all GT4 can use.

#include <array>
#include <cstdint>

namespace ps2x::iop::gt4::dbcports
{
    constexpr unsigned Ports = 2;
    // Port 2's device follows the host: it is attached after ConnectAfter VBlanks with a
    // host device and detached after DisconnectAfter VBlanks without one (DS2U_D retries
    // a failed poll 10 times before dbcman#7 Disconnect, pads.md 3.4), so a host dropout
    // of a frame or two never unplugs player 2. Not byte-faithful inside that window, on
    // purpose: DS2U_D marks each failed retry with state 3, which GT4 treats as "no pad"
    // at once; here, as port 0 has always done on a failed host read, the packets keep
    // state 1 with the neutral pad (buildPacket(nullptr)), so GT4 sees an idle pad for
    // up to 9 VBlanks after an unplug and never sees a shorter dropout.
    constexpr unsigned ConnectAfter = 2;
    constexpr unsigned DisconnectAfter = 10;

    // One VBlank's 0x80-byte packet as DS2U_D sends it through dbcman#8 (pads.md 3.2):
    // state 1, mode 0x79 (18 data bytes), the host snapshot's bytes 2..19 at +0x1c with
    // DS2U_D's pressure rule, profile ff ff ff ff, the counter at +0x7c. `snapshot`
    // nullptr: the neutral pad (no buttons, sticks centred), as when the host read fails.
    std::array<uint8_t, 0x80> buildPacket(const std::array<uint8_t, 32> *snapshot, uint32_t counter);

    // Debounced "a device is plugged into this port".
    class Presence
    {
    public:
        enum class Change
        {
            None,
            Connected,
            Disconnected,
        };
        Presence() = default;
        explicit Presence(bool present) : present_(present) {}
        // Once per VBlank with what the host says now.
        Change update(bool hostPresent);
        bool present() const { return present_; }
        void reset(bool present)
        {
            present_ = present;
            run_ = 0;
        }

    private:
        bool present_ = false;
        unsigned run_ = 0; // consecutive VBlanks on which the host disagreed with present_
    };
}
