#pragma once

// Pictures wider than 4:3 in races through GT3's own 16:9 mode (GT3_ASPECT=
// 16:9, 21:9, 32:9, 48:9 (three 16:9 screens), 4:1 (three 4:3 screens), or a
// number). GT3 USA's Screen Size option sets a flag at 0x2f424c (display
// block 0x2f4230 +0x1c); with it a camera calls 0x257f04(0.75, 1) to squeeze
// x (anamorphic: the picture is stretched back on a 16:9 TV).
// Here the race camera (0x17e740) runs with that flag set, only during the
// call (the saved option is untouched), and its 0.75 becomes (4/3) / aspect:
// it sees that much wider, and the window shows the frame at that shape. In
// races the GS squeezes the 2D (HUD) and the rear-view mirror toward the
// middle (GS::setHudSqueeze), keeping their 4:3 shape; GT3's other 16:9
// squeezes (mirror camera, text helper 0x18a938) become 1. Menus and other
// screens stay 4:3, pillarboxed. GT3_ASPECT_HUD=stretch: stretched race HUD.

#include <cstdint>

class PS2Runtime;

namespace gt3aspect
{
    // After the ELF is loaded: reads GT3_ASPECT and wraps the functions
    // (only when this CORE has the expected code there).
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    // Each presented frame: the HUD squeeze follows the race camera.
    void perFrame(uint8_t *rdram);
    // The window's picture aspect: GT3_ASPECT's while the race camera runs,
    // or 16:9 when GT3's own Screen Size option is 16:9 (0: the usual 4:3).
    float displayAspect(const uint8_t *rdram);
    // The race camera's call (gt3_camera.cpp's hook on 0x209e58, made at
    // 0x17e874 just before the race frame tests the 16:9 flag at 0x17e888):
    // the flag set until the race frame's scale (0x17e8a4) has run. The
    // race frame is often run inline by its caller (0x17e670 falls into
    // 0x17e740), which the wrapper on 0x17e740 does not see.
    void raceCameraCalled(uint8_t *rdram);
}
