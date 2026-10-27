#pragma once

// The AI cars' drivers (Race Lab, "AI cars 1-5" > Send to AI cars; analysis/design/racelab-tab/REPORT.md). Full edition only.
// GT4's AI cars have no driver names of their own: the Race Lab (its page in the browser, or the launcher's Race Lab tab)
// writes the names it gives them to <GT4_AI_DRIVERS>\gt4-drivers.txt, and our HUD (the timing tower's "driver" names), the
// pages' feed (each car's "driver") and the Race Lab recorder's race files show them. GT4's own AI still drives every car;
// nothing in GT4's memory is written.
//   GT4_AI_DRIVERS=<folder> (default racelab-data\ai-lines: GT4.exe's working folder, else beside GT4.exe) or off.
//   gt4-drivers.txt:  gt4-ai-drivers 1
//                     # comments
//                     car <k> <name>        (k 1..7: AI car k = the k-th car that is not yours, in GT4's car order)
//                     style <k> <words>     (optional: how they drive, as the Race Lab describes them)
// The file is read again when it changes (looked at every 2 s at most), so drivers sent while GT4 runs count from then on.

#include <string>

namespace gt4race
{
    struct State;
}

namespace gt4drivers
{
    constexpr int kMaxAi = 7;
    // AI car number of car `index` in s: 1.. (the k-th car that is not the player's), 0 for the player's car or none.
    int aiNumber(const gt4race::State &s, int index);
    // The driver's name of car `index` in s ("" when the Race Lab gave that car none, or for the player's car).
    std::string driverOf(const gt4race::State &s, int index);
    // The name in plain ASCII letters for our HUD's font (which has no accents): "Amélie Duret" -> "Amelie Duret"; other
    // letters it can't spell become '?'.
    std::string asciiName(const std::string &utf8);
    // Whether any names are there now (the file read, at least one car named).
    bool any();
    // The folder read (for logs).
    std::string folder();
}
