#pragma once
#include <cstdint>

namespace ps2_pad
{
    // First connected XInput user index, or -1 (none, or XInput unavailable).
    int firstXInputPad();
    // One XInput pad's input (XINPUT_GAMEPAD's fields).
    struct XInputPadState
    {
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        uint8_t lt = 0, rt = 0;
    };
    // The connected XInput user indices among `mask` (bit n = index n; only those are
    // asked), 0 if XInput is unavailable; their input in pads[0..3] when given.
    unsigned readXInputPads(unsigned mask, XInputPadState *pads);
    // Connected XInput user indices as bits: readXInputPads(0xf, nullptr).
    unsigned connectedXInputPads();
    // Low-frequency (large, left) and high-frequency (small, right) motors.
    bool setXInputRumble(int index, uint16_t lowFrequency, uint16_t highFrequency);
    // Vibration on / off for every controller (the launcher's Controls page:
    // GT3_VIBRATION=0 at start, live key vibration); off stops the motors.
    void setVibrationEnabled(bool on);
    bool vibrationEnabled();
}
