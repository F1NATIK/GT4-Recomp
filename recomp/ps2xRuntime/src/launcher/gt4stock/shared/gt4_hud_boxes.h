#pragma once

// Our own race HUD boxes' shared geometry and colours (HUD hand-off 7; header-only): GT4.exe draws them (src/lib/gt4_hud.cpp)
// and the launcher's Driving > Race HUD > HUD layout page lays them out (gt4full_racehud.cpp, gt4full_settings.h), so both
// read the same sizes from here. Sizes are fractions of the HUD area's height at 100 %.

#include <algorithm>
#include <cstdint>

namespace gt4hudbox
{
    // GT4_HUD_<KEY>, in this order (the launcher's kFullHudLiveKeys).
    enum Box
    {
        Radar,
        Delta,
        Pedals,
        GMeter,
        Tower,  // HUD hand-off 7 C: the timing tower
        Map,    // the race map (GT4's course art)
        Splits, // the split times
        Revs,   // the rev lights
        Speed,
        Gear,
        Lap,    // our own lap counter
        kCount
    };
    inline constexpr const char *kKeys[kCount] = {"RADAR", "DELTA", "PEDALS", "GMETER", "TOWER", "MAP", "SPLITS", "REVS", "SPEED", "GEAR", "LAP"};
    // Width x height at 100 % (the tower's comes from its name, columns and rows: towerSize).
    inline constexpr float kBase[kCount][2] = {{0.24f, 0.24f},  {0.30f, 0.07f}, {0.05f, 0.16f},  {0.16f, 0.16f},   {0.0f, 0.0f}, {0.30f, 0.215f},
                                               {0.40f, 0.075f}, {0.40f, 0.05f}, {0.17f, 0.085f}, {0.085f, 0.085f}, {0.17f, 0.085f}};
    // Styles (the first is the default; none: a box without styles). The tower has its own fields instead (name, columns, rows).
    inline constexpr const char *kStyles[kCount][2] = {{"round", "square"}, {"bar", "number"}, {nullptr, nullptr}, {"trail", "dot"},
                                                       {nullptr, nullptr},  {"locked", "rotating"}, {nullptr, nullptr}, {"bar", "lights"},
                                                       {nullptr, nullptr},  {nullptr, nullptr}, {"lap", "clock"}};
    // The map's detail after its style (HUD hand-off 10: GT4_HUD_MAP=...,locked|rotating,detailed|simple; missing: detailed).
    // Simple draws the course's centreline instead of GT4's art.
    inline constexpr const char *kMapDetail[2] = {"detailed", "simple"};

    // ---- the timing tower: GT4_HUD_TOWER=on,<x>,<y>,<size>,<see-through>,<name>,<columns>,<rows>
    //   name car | short | driver; columns a + list of gap (to the leader), int (to the car ahead), best, last; rows 4..16.
    inline constexpr const char *kTowerNames[3] = {"car", "short", "driver"};
    inline constexpr const char *kTowerCols[4] = {"gap", "int", "best", "last"};
    inline constexpr int kTowerColsDefault = 1 | 2 | 4; // gap+int+best
    inline constexpr int kTowerRowsDefault = 8;
    inline constexpr float kTowerRow = 0.05f;   // a row
    inline constexpr float kTowerHead = 0.03f;  // the column names over the rows
    inline constexpr float kTowerPad = 0.012f;  // left and right
    inline constexpr float kTowerDot = 0.055f;  // the position dot's column
    inline constexpr float kTowerNameW[3] = {0.25f, 0.075f, 0.20f};     // car, short, driver
    inline constexpr float kTowerColW[4] = {0.115f, 0.115f, 0.135f, 0.135f}; // gap, int, best, last
    inline void towerSize(int name, int cols, int rows, float &w, float &h)
    {
        w = kTowerPad * 2.0f + kTowerDot + kTowerNameW[std::clamp(name, 0, 2)];
        for (int k = 0; k < 4; ++k)
            if ((cols >> k) & 1)
                w += kTowerColW[k];
        h = kTowerHead + kTowerRow * static_cast<float>(std::clamp(rows, 4, 16));
    }
    // A box's size at 100 % (fractions of the height): style 1 is the map's round rotating box and the lap counter with the
    // race clock (1.7 times as wide).
    inline void baseSize(int box, int style, int towerName, int towerCols, int towerRows, float &w, float &h)
    {
        if (box == Tower)
        {
            towerSize(towerName, towerCols, towerRows, w, h);
            return;
        }
        w = kBase[std::clamp(box, 0, kCount - 1)][0], h = kBase[std::clamp(box, 0, kCount - 1)][1];
        if (box == Map && style == 1)
            w = h = 0.24f;
        if (box == Lap && style == 1)
            w *= 1.7f;
    }

    // ---- colours: each car's (the phone and top-screen pages' COLORS[car % 16]), the player's gold
    inline constexpr uint32_t kCarColours[16] = {0x4f8bd6, 0xd65a4f, 0x58c4dc, 0x9bd65a, 0xc98bd6, 0xe08bb0, 0x7fd1b9, 0xd6a35a,
                                                 0x8f9cf0, 0xc7c7c7, 0x5ab0d6, 0xd6d65a, 0xb07fd6, 0x6fd68f, 0xd67f5a, 0x9bb0c7};
    inline constexpr uint32_t kMe = 0xf2b33d;
    // Split times: purple (the best of every car this race), green (your own best), yellow (slower than your best).
    inline constexpr uint32_t kPurple = 0xb67cf0, kGreen = 0x3ecf8e, kYellow = 0xf2d24b;
} // namespace gt4hudbox
