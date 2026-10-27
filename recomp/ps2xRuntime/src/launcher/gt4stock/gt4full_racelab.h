#pragma once

// GT4 Recompilation launcher, Full edition: the Race Lab tab (gt4full_racelab.cpp; analysis/design/racelab-tab/REPORT.md).

namespace gt4stock::racelab
{
    // The test pictures: the tab shown (0 AI drivers, 1 Laps, 2 Races, 3 Settings) and, for 0, a driver's name being changed.
    void showTabForTest(int tab, bool renaming = false);
}
