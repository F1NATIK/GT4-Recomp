#pragma once

#include <array>
#include <cstdint>
#include <string>

// GT3_DRIVE_LOG=<folder> (development build): the driving in races, lap by
// lap, for the Racing Line Editor (GT4-WORK tools/drive_log_upload.py puts
// the laps on the page, where they play back over the track).
// <folder>/<course>-<yyyymmdd-hhmmss>-lapNN.csv, one row every frame GT3
// runs (60 a second, its own update rate; GT3_DRIVE_LOG_STEP=<metres>: only
// once the car has moved that far, 0.01 .. 5):
//   t_ms (race clock), lap_m (GT3's lap distance), x, y, z, speed_ms, gear,
//   rpm, throttle, brake (0..1), steer (-1..1), surfaces (one digit a wheel),
//   yaw (GT3 heading), vsync (the frame)
// Inputs are what GT3 reads from the pad (autopilot, replay, controller or
// wheel alike), with GT3's default controls: throttle = Cross pressure (or
// R2's when larger), brake = Square (or L2), steer = the left stick's x.
// <course> is `replace-<name>` for a GT3_COURSE_REPLACE course, else
// `runway-<crc32>` (the runway section's CRC32, as gt3_course_patch keys it).
// The header lines say who drove (autopilot / player) and the lap time.
// Recording on and off: GT3_DRIVE_LOG starts with it on; otherwise F10 in the
// game turns it on (and off), into "recordings" in the working folder; a lap
// stopped half way is kept, marked "# incomplete". GT Race Lab's "Add recorded
// laps" reads the files.
// Only the player's own driving is recorded (2026-10-02, the user: "only record
// from when we have actual control of the car"): nothing while the race clock
// stands (the grid countdown) or while GT3's AI driver drives the player's car
// (AUTO DRIVE, the car after the finish); a grid start goes into the first
// lap's file from the green light ("# start grid", its time from the green as
// GT3 counts lap 1; "# line <t_ms>" where it crosses the line), as does a
// rolling handover before the line ("# start rolling"); a lap is only begun at
// the line (switched on part way round: from the next line); none after the
// race's last lap; a lap given up within its first second is removed.
// GT3_DRIVE_LOG_ALL=1: everything while the clock runs, lap 0 in a file of its
// own (the recorder before this).
class PS2Runtime;
namespace gt3drivelog
{
    // Wraps GT3's AI driver (0x1f7ae0) to see when it drives the player's car.
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    bool enabled();          // recording, or a lap still to close
    bool recording();        // recording wanted
    bool toggle();           // the record key: returns the new state
    int openLap();           // the lap being written (0: none)
    uint32_t lastLapMs();    // the last finished lap's time (0: none yet)
    const std::string &where();
    // Each pad read GT3 makes, with the packet it gets (32 bytes, 0x79 mode
    // with pressures).
    void onPad(const uint8_t *rdram, uint64_t vsyncTick, const std::array<uint8_t, 32> &packet, bool autopilot);
}
