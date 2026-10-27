#pragma once

// End-of-race report (user 2026-10-03: "a more advanced end of race screen,
// with more in depth data, fastest laps for each driver, split times, and
// whatever else you think of"). Dev build only (GT3_ADDONS), off unless
// GT3_RACE_REPORT=1 (the launcher's HUD > Race report page).
//
// Collected every host frame from GT3's race state (gt3race::read: each
// car's lap, progress into the lap, position, speed, GT3's lap times):
// per car the race trace (clock, lap, progress), lap times (GT3's own, as its
// HUD shows them), sector times (the lap in thirds by distance, from the
// trace), the position at each lap's end, the top speed per lap. When the
// player's car finishes (its lap past the race's laps) the report shows over
// the picture, 4 s later, on five pages - Results, Lap times, Sectors,
// Positions, Your race - and (GT3_RACE_REPORT_DIR=<folder>) is saved there
// as an HTML page. F2: next page (the report of the last race at any time);
// Shift+F2: close; the tabs can be clicked. Replays are not collected.

#include <cstdint>
#include <string>

namespace gt3report
{
    // A car's driver (GT3's AI cars have none): car 0 the player (GT3_PLAYER_NAME, else "You"),
    // the others from GT3_DRIVER_NAMES (comma-separated; default: the GT Race Lab's made-up
    // roster). Also the timing tower's (gt3_hud.cpp).
    std::string driverName(int carIndex);
    // Every host frame (any time; cheap when off).
    void frame(const uint8_t *rdram);
    // Over the picture (x, y, w, h in window pixels), last of the overlays.
    void draw(float x, float y, float w, float h);
}
