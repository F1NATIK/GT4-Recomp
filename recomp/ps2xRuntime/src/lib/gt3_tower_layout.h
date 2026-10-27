#pragma once

// The timing tower's columns (our race HUD, gt3_hud.cpp) and their widths:
// shared by the HUD and the launcher's layout editor (gt3_launcher.cpp), so
// the box it shows is the box the game draws. Header only, no dependencies.
namespace gt3hud
{
    // The custom tower's columns (Box::columns bits), in this order across.
    enum TowerColumn
    {
        ColPlace = 1,       // place, in a badge (the leader's gold)
        ColColour = 2,      // the car's colour dot (as its map dot)
        ColCar = 4,         // the car: its model name ("Car N" with ColCarNumber)
        ColLap = 8,         // lap
        ColTime = 16,       // a time: Box::timeMode
        ColGap = 32,        // gap to the leader (LEAD for the leader)
        ColInterval = 64,   // interval to the car ahead
        ColCarNumber = 128, // (with ColCar) "Car 1".."Car 6" instead of names
    };
    constexpr int kTowerColumnCount = 7; // ColPlace .. ColInterval
    constexpr int kTowerDefaultColumns = ColPlace | ColColour | ColCar | ColLap | ColTime | ColGap;
    // Box::timeMode: the tables' time column (F5 in a race cycles it).
    enum TowerTime
    {
        TimeBest = 0,
        TimeLast = 1,
        TimeCurrent = 2,
    };
    constexpr int kTowerTimeCount = 3;

    // The rows: 6 cars and a heading, in row heights.
    constexpr float kTowerRows = 7.25f;
    // Column k's width in row heights (the car's with names or numbers), and
    // the box's margins.
    constexpr float towerColumnWidth(int k, bool carNumbers)
    {
        return k == 0 ? 1.5f : k == 1 ? 0.9f : k == 2 ? (carNumbers ? 3.4f : 7.2f) : k == 3 ? 1.5f : k == 4 ? 3.3f : 2.8f;
    }
    constexpr float kTowerMargin = 0.7f;
    // The custom tower's box shape (width / height).
    inline float towerCustomAspect(int columns)
    {
        float w = kTowerMargin;
        for (int k = 0; k < kTowerColumnCount; ++k)
            if (columns & (1 << k))
                w += towerColumnWidth(k, (columns & ColCarNumber) != 0);
        return (w < 3.0f ? 3.0f : w) / kTowerRows;
    }
}
