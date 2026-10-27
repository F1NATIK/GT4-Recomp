#pragma once

// GT4 Recompilation launcher, Full edition: Driving > Telemetry screen's Windows side (HUD hand-off 8 T1, T2; kept out of the
// raylib translation units): the monitors with their EDID names, "Identify monitors", this PC's address on the home network
// (the adapter with the default route) and Microsoft Edge for "Show it now".

#include "shared/gt4_tele_monitors.h"

#include <string>
#include <vector>

namespace gt4stock::sys
{
    // The monitors as Windows arranges them now (gt4tele::list).
    std::vector<gt4tele::Monitor> teleMonitors();
    // A borderless topmost window on every monitor with its big number (1-based, the list's order) for about 3 s; never
    // takes the focus. Returns at once (a thread of its own draws and closes them).
    void identifyMonitors(const std::vector<gt4tele::Monitor> &list);
    // This PC's IPv4 address on the adapter that has the default route ("" none); its adapter name in adapter.
    std::string lanAddress(std::string *adapter = nullptr);
    // Microsoft Edge's msedge.exe ("" not installed).
    std::string edgePath();
}
