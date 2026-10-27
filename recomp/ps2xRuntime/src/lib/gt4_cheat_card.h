#pragma once

// GT4's in-game cheat card (GT4 builds; analysis/cheats/CHEATS-TAB-DESIGN.md 6). Off unless
// GT4_CHEAT_CARD=key. Then F2 (GT4_CHEAT_CARD_KEY) shows the first card of GT4_CHEAT_CARD_LIST,
// F2 again the next, and after the last one hides it. The card lights each button as the pad
// presses it (GT4's craft() rule, gt4_cheat_codes.h): port 0 (controller 1, the DualShock path,
// not a wheel's buttons), and port 1 when something feeds it (onPad(1, ...), the player-2 path).
// The hidden Options code is not followed on port 0 unless `secret` is served (GT4 drops port 0's
// presses for it). What the card says about GT4_CHEATS is what the IOP overlay serves
// (gt4_cheat_card_rules.h). It is drawn by the presenter after the scanline pass, into the window
// only: never into GT4's frame, its photos or its replays.
//
//   GT4_CHEAT_CARD         off (default) | key | auto (full launcher, not built: taken as key)
//   GT4_CHEAT_CARD_LIST    ids in order: credits,licence,test,event,mission,options; absent (or empty,
//                          which the start file makes absent) = all; none = no card (off)
//   GT4_CHEAT_CARD_KEY     F2..F12 (default F2; F1 is the scanlines)
//   GT4_CHEAT_CARD_PLACE   auto (beside the picture when the bar is wide enough, else top right),
//                          left, right, top-right
//   GT4_CHEAT_CARD_FOLLOW  1 (light the buttons as pressed, default) | 0 (a static card)
//   GT4_CHEAT_CARD_TEST    (development builds, tests only; never in gt4-start.env)
//                          show:<n>,progress:<k>[,reset][,done][,live][,all][,showat:<vsync>][,shot:<prefix>]
//                          [,sizes:<W>x<H>+...][,at:<vsync>]
//                          (live: the pad drives the card as in play; else the given state holds;
//                          showat: hidden until that vsync, then shown as by F2)

#include <cstddef>
#include <cstdint>

struct Texture; // raylib's Texture / Texture2D

namespace gt4cheatcard
{
    struct Frame
    {
        float screenW = 0, screenH = 0;              // the window
        float picX = 0, picY = 0, picW = 0, picH = 0; // GT4's picture in it
        bool readKeys = false;                       // false in --headless runs
        uint64_t vsync = 0;
        const ::Texture *frame = nullptr;            // GT4's frame (test pictures only)
        int srcW = 0, srcH = 0;                      // its picture texels
        float aspect = 4.0f / 3.0f;
        bool interlaced = false;                     // a field picture (the picture pass's Blend applies; test pictures only)
    };

    // Presenter thread, every frame, after everything else is drawn (BeginDrawing..EndDrawing).
    void frame(const Frame &f);
    // EE side, every pad poll: port 0's packet as GT4 gets it (PS2IopHostAdapter::readPadSnapshot);
    // port 1's from the player-2 path when that is in the tree (bytes 2-3 the buttons, active low).
    void onPad(int port, const uint8_t *packet, size_t bytes);
    // Presenter thread, before the window closes.
    void shutdown();
}
