#pragma once

// PS2X_WHEEL_PORT=1|2 (default 1): which player a lone host wheel belongs to
// (analysis/phase5b/player2). GT4's wheel manager puts a wheel whose USB location
// byte (LGDEV fn1 info+0x0c) is 1 into its slot 1 ("USB port2", merged with pad
// port 2 into "PS2GT4 slot2" = player 2) and any other wheel into slot 0 ("USB
// port1", player 1) (EE 0x5557f0). HostWheel::usbPort gives host wheel 0 byte 2
// and wheel 1 byte 1; with port 2 a lone wheel (deviceCount() == 1) reports 1.
// Two host wheels (virtual2) keep wheel 0 = player 1, wheel 1 = player 2.

#include "gt4_lgdev.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ps2x::iop::gt4
{
    // 1 or 2 from the setting's text (nullptr / empty: 1). Anything else: 1, with a line in `note`.
    unsigned wheelPortSetting(const char *value, std::string &note);

    // createHostWheel's last step: with PS2X_WHEEL_PORT=2 and a wheel, the wheel wrapped so
    // that a lone wheel reports USB location byte 1. Otherwise `wheel` itself, unchanged.
    std::shared_ptr<HostWheel> applyWheelPort(std::shared_ptr<HostWheel> wheel, const std::function<void(std::string_view)> &log);
}
