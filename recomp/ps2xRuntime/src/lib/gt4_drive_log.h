#pragma once

// GT4_DRIVE_LOG=<folder> (Full edition, the launcher's "Record laps for the Race Lab", off by default; F10 stops and
// starts it while set): the player's laps for GT Race Lab, from the race reader's per-frame state (read-only; Race Lab
// hand-off 1). One file per lap, <folder>\gt4-<course id>-<yyyymmdd-hhmmss>-lapNN.csv, in the gt3-drive-log format the
// Race Lab page reads ("Add laps from the game"):
//   # gt3-drive-log 1 / # game gt4 / # course replace-<Race Lab track> (gt4-<course id> when it has none) /
//   # reader_course <id> / # driver player / # car <name> / # lap <n> / # start grid|rolling (lap 1) /
//   # lap_time_ms <GT4's lap time> (a lap cut short has none)
//   t_ms,lap_m,x,y,z,speed_ms,gear,rpm,throttle,brake,steer,surfaces,yaw,vsync
// one row per race frame (the race clock moved); x = reader x, y = reader z (height), z = -(reader y); surfaces empty.
// And the whole race, every car: <folder>\gt4-<course id>-<time>-race.csv, the race reader's CSV (reader_to_racelab.py).

#include <cstdint>
#include <string>

namespace gt4race
{
    struct State;
}

namespace gt4drivelog
{
    bool active();           // GT4_DRIVE_LOG is set
    bool recording();        // recording now (F10 toggles)
    bool toggle();           // the new state
    int openLap();           // the lap being recorded (0: none)
    uint32_t lastLapMs();    // the last recorded lap's time (0: none yet)
    const std::string &where();
    // Each race frame (the race reader, after it has read the state).
    void onFrame(const gt4race::State &s);
}
