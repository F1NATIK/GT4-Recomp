#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2_pad
{
    // Pressure byte order, starting at byte 8 of the backend/status packet:
    // the DualShock 2 protocol order (right left up down triangle circle
    // cross square L1 R1 L2 R2), as PADMAN 390c pairs them with buttons.
    inline constexpr std::array<uint16_t, 12> pressureMasks = {
        0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
        0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};

    struct PressureState
    {
        std::array<uint8_t, 12> values{};
        bool valid = false;

        void readBackend(const uint8_t (&packet)[32])
        {
            // Legacy 0x73 packet padding is not valid pressure data.
            valid = packet[1] == 0x79;
            for (std::size_t i = 0; i < values.size(); ++i)
                values[i] = valid ? packet[8 + i] : 0;
        }

        uint8_t value(uint16_t buttons, uint16_t enabledMask,
                      bool enabled, uint16_t mask) const
        {
            if (!enabled || (enabledMask & mask) == 0)
                return 0;
            if (valid)
            {
                for (std::size_t i = 0; i < pressureMasks.size(); ++i)
                    if (pressureMasks[i] == mask)
                        return values[i];
                return 0;
            }
            return (buttons & mask) == 0 ? 255 : 0;
        }
    };
}
