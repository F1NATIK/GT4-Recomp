#pragma once

// GT4 Recompilation launcher, Full edition: Driving > Race HUD > Timing (gt4full_racetiming.cpp; developer, 20 Oct 2026: "can we
// have a page for timing show like there is for radar an lights"). The timing tower's and the split times' settings
// (GT4_HUD_TOWER, GT4_HUD_SPLITS) on a page of their own, and the drivers the Race Lab sent to GT4 (GT4_AI_DRIVERS'
// gt4-drivers.txt) for the tower's Driver names in the launcher's previews.

#include <filesystem>
#include <string>

namespace gt4stock::racetiming
{
    // The drivers GT4 would show: <root>\racelab-data\ai-lines\gt4-drivers.txt (read again every 3 s; once in the hidden test
    // mode), none while GT4_AI_DRIVERS is off (driversOn false).
    void refreshDrivers(const std::filesystem::path &root, bool driversOn, bool hidden);
    // The tower's name for a sample car as GT4.exe writes it (gt4_hud.cpp drawTower): name 0 the car's, 1 its short code, 2 the
    // driver ("You" for yours, an AI car's driver from the Race Lab; one without a driver keeps the car's name), the driver with
    // the car's short code after it, shortened to fit `width` at font size `fs` (pixels).
    std::string towerName(int name, int car, bool player, const char *carName, const char *code, float fs, float width);
    // The width a driver's name and code is fitted to in a tower with height unit k (pixels).
    float driverWidth(float k);
    // The hidden test pictures: 0 the file's drivers (none in a test GT4 folder), 1 the Race Lab's five default drivers sent.
    void driversForTest(int mode);
}
