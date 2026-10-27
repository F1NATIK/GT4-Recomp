#pragma once

// Grid-start lights (user 2026-10-02: start lights instead of GT3's "3 2 1
// START", the lights going out at a random moment; options in the launcher's
// HUD tab). Dev build only (GT3_ADDONS), off unless switched on.
//
// GT3's grid start (found 2026-10-03 from a Trial Mountain Arcade race, see
// PS2_PROJECT_STATE.md "GRID-START LIGHTS"): the start controller
// 0x17a5e8(obj) runs once a race frame (called from 0x17c7e4). obj is the
// object at [0x351f50]. When a start is asked for (obj + 0x50 != 0) it picks
// the start kind (obj + 0x54, jump table at 0x327640), sets obj + 0x58 to the
// countdown's seconds (10 on Trial Mountain) and obj + 0x5c = 60 * seconds
// (frames to the start; 0x1a96b8 gets the seconds too). Every later frame
// it takes one off obj + 0x5c; on reaching 0 it sends message 4 to the
// queue at obj + 0x84 (0x1796c0(obj + 0x84, 4, 0, 0)): START, the cars
// released. GT3's digits show at 180 / 120 / 60 frames ("3" / "2" / "1").
// Holding obj + 0x5c at 1 (the controller not run) holds every car, the AI
// and the race clock (checked 2026-10-03, `start-lights/hold180`: 180 frames
// held, race clock 0 ms throughout, the grid still, START then as usual).
//
// GT3_START_LIGHTS=1: a light panel (drawn by the runtime over the picture)
// instead of GT3's countdown: one light a second, all lit when the countdown
// reaches its last frame, then held a random time (GT3_START_WAIT=min,max
// seconds, default 0.2,3.0) and all out = START. GT3_START_LIGHTS_COUNT=3..5
// (default 5). GT3_START_AI_REACTION=min,max seconds (default off): each AI
// car waits its own random time after the lights go out (throttle 0, brake on).
// No random wait in replays or i.LINK races (GT3_LINK_MODE set: every console
// must start together), the lights then go out at GT3's own moment.
// Diagnostics: GT3_START_HOLD=<frames> (a fixed hold), GT3_START_HIDE_DIGITS=1.

class PS2Runtime;

namespace gt3startlights
{
    void install(PS2Runtime &runtime, const unsigned char *rdram);

    // For the overlay (any thread): whether the panel is up, its lights and how many are lit.
    struct Panel
    {
        bool show = false;
        int count = 5;
        int lit = 0;
    };
    Panel panel();
}
