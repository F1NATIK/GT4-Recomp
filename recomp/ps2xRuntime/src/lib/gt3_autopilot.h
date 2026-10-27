#pragma once

// An autopilot for GT3 races (tests: whole races, results, replays).
// GT3_AUTOPILOT=1: in a race the pad is driven from GT3's race state
// (gt3_race_info.h) instead of the controller or the pad replay (which still
// drive the menus).
// - The racing line is learned from the AI cars: their positions and speeds,
//   in 2 m steps of GT3's own track progress (metres into the lap).
// - Steering aims at a point on that line ahead of the car (pure pursuit,
//   look-ahead growing with speed); throttle and brake (Cross / Square,
//   pulsed for partial pedal) follow the AI's speed there, braking early
//   enough for the slowest point within braking distance.
// - Learning: where the car goes off the line or loses speed suddenly (a
//   crash), the target speeds before that point go down; where a lap section
//   was clean they creep back up. Lap times are logged ([autopilot] lap).
// GT3_AUTOPILOT_SPEED=<0.5..1.2>: the starting fraction of the AI's speed
// (default 0.92). GT3_AUTOPILOT_LOG=1: a line per second (progress, speeds,
// steering, pedals, distance from the line).

#include <array>
#include <cstdint>

class PS2Runtime;

namespace gt3autopilot
{
    bool enabled();
    // Installs GT3's race-state hook (gt3_race_info) when enabled.
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    // The pad packet for this VSync when the autopilot drives (in a race).
    bool drive(const uint8_t *rdram, uint64_t tick, std::array<uint8_t, 32> &packet);
}
