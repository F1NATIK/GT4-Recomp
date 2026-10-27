#pragma once

// Holding the start (HUD hand-off 7 E, analysis/design/hud-pages/handoff7/STAGE-E-REPORT.md): the first HUD feature that
// changes how a race runs. Full edition only (never Classic), off unless GT4_START_HOLD=on. Only offline single-player races
// with a standing start (GT4's grid countdown, RaceOrganization+0xbc): Arcade single races, GT Mode races and
// championships. Never LAN / online / split-screen races, the attract demo, licences, missions, or replays.
//
// How GT4 starts a race (analysis/phase4/specs/race-start.md 3-4, checked again for this stage):
//   - O+0xbc counts the grid countdown down by 1 every simulation step (0x3c0208); at 0 it posts the start event (4, -1).
//   - While O+0xbc > 0 every car is held (the dynamics 0x34db30 set car+0x5c0 = 1) and the race clock does not run
//     (0x357e80 adds 1/60 s to D+0xf898 only when no hold is on).
//   - Once it is 0 each car's own delay D+0xcbfc[i] (frames) counts down; the car stays held until it is 0, then GT4 posts
//     a start event for that car.
// What this does, at the race frame update 0x389638 (once a frame, before the frame's step):
//   E1/E2 GT4_START_DELAY=<min>,<max> seconds (0..5; default 0.2,3.0): when GT4's countdown would end, it is kept at 1 for a
//         random time in that range (O+0xbc written back to 2 before each step), then let go: GT4's own start happens then,
//         and the start lights (GT4_START_LIGHTS, which follow O+0xbc) go out on that frame.
//   E3   GT4_START_AI=instant|quick|normal|slow: on the release frame each AI car gets its own delay D+0xcbfc[i]
//         (instant 0, quick 0.1-0.3 s, normal 0.2-0.6 s, slow 0.4-1.0 s), so GT4 itself holds it that much longer.
//   E4   GT4_START_CAR=held|free: free lets your car go from the first light (GT4_START_COUNT lights, one a second, the last
//         one 1 s before GT4's own start) while the AI cars stay held: car+0x5c0 cleared for your car after GT4's hold loop
//         (on the call to 0x357700 from 0x34de1c). Moving more than 0.5 m forward from your grid place before the lights go
//         out is a jump start: GT4's hold takes your car again at once (it stops dead where it is).
//   E5   GT4_START_JUMP=h5|h10: after a jump start your car is held 5 or 10 s after the lights go out (its own delay
//         D+0xcbfc, as E3). The race-time penalties (t5 / t10) are not built: STAGE-E-REPORT.md says why.
//   GT4_START_HOLD=on|off (default off): the switch for all of the above.
// Tests: GT4_START_HOLD_LOG=1 logs every race frame from the countdown to 20 s after the release (clock, the countdown, each
// car's place, hold flag, own delay and speed); GT4_START_SEED=<n> makes the random times repeatable.

#include <cstdint>

namespace gt4start
{
    // Whether any of it is on (settings read once). False in the Classic build.
    bool wanted();
    // The race frame update 0x389638 (a0 = the race object R), before the race reader.
    void onRaceFrame(uint8_t *rdram, uint32_t r, uint64_t vsync);
    // The dynamics' first per-car pass after its hold loop (the call to 0x357700 from 0x34de1c): your car let go in free mode.
    void afterHoldLoop(uint8_t *rdram);
} // namespace gt4start
