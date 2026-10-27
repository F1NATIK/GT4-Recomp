#include "gt4_dbcman_ports.h"

#include <algorithm>
#include <cstring>

// See gt4_dbcman_ports.h. buildPacket is gt4_dbcman.cpp's VBlank packet code moved
// here unchanged, so port 1 and port 2 send exactly what port 1 sent before.

namespace ps2x::iop::gt4::dbcports
{
    std::array<uint8_t, 0x80> buildPacket(const std::array<uint8_t, 32> *snapshot, uint32_t counter)
    {
        std::array<uint8_t, 0x80> packet{};
        packet[0] = 1;    // state: last poll OK
        packet[1] = 0x16; // 2 * (0x79 & 0xf) + 4
        packet[2] = 0x12; // DualShock 2 data bytes
        packet[3] = 4;    // button profile bytes
        packet[4] = 1;    // SIO2 transfer OK
        if (snapshot)
        {
            std::copy_n(snapshot->begin() + 2, 18, packet.begin() + 0x1c);
        }
        else
        {
            std::fill_n(packet.begin() + 0x1c, 2, uint8_t(0xff));
            std::fill_n(packet.begin() + 0x1e, 4, uint8_t(0x80));
        }
        // Pressure rule (DS2U_D 0x191c): pressure != 0 exactly when pressed.
        // Map table 0x2298 order: Right Left Up Down Tri Circle Cross Square L1 R1 L2 R2.
        const uint16_t buttons = static_cast<uint16_t>(~(packet[0x1c] | (packet[0x1d] << 8)));
        constexpr std::array<unsigned, 12> bit{5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
        for (unsigned i = 0; i < 12; ++i)
        {
            uint8_t &pressure = packet[0x22 + i];
            const bool pressed = (buttons >> bit[i]) & 1u;
            if (pressed && !pressure)
                pressure = 1;
            if (!pressed)
                pressure = 0;
        }
        std::fill_n(packet.begin() + 0x2e, 4, uint8_t(0xff));
        std::memcpy(packet.data() + 0x7c, &counter, 4);
        return packet;
    }

    Presence::Change Presence::update(bool hostPresent)
    {
        if (hostPresent == present_)
        {
            run_ = 0;
            return Change::None;
        }
        if (++run_ < (present_ ? DisconnectAfter : ConnectAfter))
            return Change::None;
        present_ = hostPresent;
        run_ = 0;
        return present_ ? Change::Connected : Change::Disconnected;
    }
}
