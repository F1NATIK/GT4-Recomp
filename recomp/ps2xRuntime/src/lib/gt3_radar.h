#pragma once

#include <cstdint>
#include <map>
#include <string>

// Proximity radar: the cars around the player, drawn over the race picture
// from GT3's own race state (gt3race::read: each car's world x / z and yaw).
// The player's car is in the middle pointing up; other cars are rectangles at
// their place and heading, in the "near" colour when alongside.
//
// Settings (the launcher's Radar page passes them; live through
// GT3_LIVE_SETTINGS keys radar, radar_x, ...):
//   GT3_RADAR          0 / off, 1 / near (shown while a car is within range), always
//   GT3_RADAR_X, _Y    the radar's middle, percent of the HUD's width (x: the
//                      whole picture up to 21:9, the middle screen on triple
//                      screens, the middle 4:3 with GT3_HUD_ANCHOR=0) and of
//                      the height (y); default 86, 52 (right, half way down)
//   GT3_RADAR_SIZE     percent of the picture height (8-60, default 24)
//   GT3_RADAR_RANGE    metres from the middle to the edge (10-80, default 25)
//   GT3_RADAR_OPACITY  cars, 10-100 (default 90); GT3_RADAR_BG_OPACITY 0-100 (35)
//   GT3_RADAR_COLOR_PLAYER / _CARS / _NEAR / _BG  RRGGBB (hex)
// In game: F8 cycles off / near / always; Shift+F8 edits (arrows move, +/-
// size, [ ] opacity, Shift+F8 or Enter saves).
namespace gt3radar
{
    enum class Mode
    {
        Off,
        Near,
        Always
    };
    struct Settings
    {
        Mode mode = Mode::Off;
        int x = 86, y = 52;       // percent
        int size = 24;            // percent of the picture height
        int range = 25;           // metres
        int opacity = 90;         // percent
        int bgOpacity = 35;       // percent
        uint32_t player = 0x78DCFF, cars = 0xFFE65A, near = 0xFF8C00, background = 0x000000;
    };
    Settings &settings();
    Mode cycleMode(); // F8
    // Live settings (the launcher's file): the radar keys present; returns a
    // note describing what changed (empty: nothing).
    std::string applyLive(const std::map<std::string, int> &values);
    // The radar keys as the launcher's settings file holds them.
    std::map<std::string, int> liveValues();
    // In-game edit mode (Shift+F8): true while editing; the keys it handles
    // this frame are consumed. `saved` becomes true when the edit ends.
    bool editing();
    void handleKeys(bool &saved, std::string &note);
    // Inside BeginDrawing: the radar over the game picture (x, y, w, h in
    // window pixels). Does nothing outside races (except while editing).
    void draw(const uint8_t *rdram, float x, float y, float w, float h);
}
