#pragma once

// GT3's live race state, read from EE RAM for the phone's timing page
// (gt3_phone_view.cpp). Found from RAM dumps of an Arcade race at Trial
// Mountain (see PS2_PROJECT_STATE.md, "LIVE TIMING"):
// - the race HUD's lap-time updater 0x193040(hud, entry) runs every race
//   frame; entry + 0x18 is the player's car object and car + 4 the race;
// - the race clock is u32(race + 0xe380) / 3 ms (GT3's lap time getter
//   0x1ef8c8: clock / 3 - car[0x14b4]);
// - the cars are race + 0xc8 + k * 0x16dc (k = 0 the player), each with
//   +0x4 the race, +0x1368 progress into the lap (float m; GT3's running
//   order), +0x1374 distance driven (float m), +0x13b8 lap (byte, 1 = first
//   lap, 0 = on the grid behind the line), +0x1465 race position (byte, from
//   1), +0x13f4 yaw (radians; forward = (-sin, -cos) in x / z), +0x14b4 lap
//   start (ms), +0x14bc / +0x14c4 world x / z (float m),
//   +0x16c4 last lap time (ms; 359999999 = none yet).
// - car names: the race entries are the player's entry + k * 0x2160 (entry +
//   0x18 = car k's object), entry + 0x20 the car's 64-bit ID. The race's
//   loaded car table has 0x108-byte records starting with the ID (+0x10 =
//   0x376e, +0x18 = 0x27f731) with u16 string numbers of the model and the
//   maker at +0x100 / +0x104, in a string table "STDB" (u32 count, u32 ?,
//   u32 size, u32 offsets[count] from the header; strings are u16 length +
//   text, NUL-padded to even).
// - the course's edges: GT3 keeps lines of the course as arrays of 16-byte
//   records (x, y, z float, word), the first record's word the number of
//   records (low 16 bits; Laguna Seca's also have the 0x10000 flag), the
//   others' 0 / small flags (0x10000). The track's two edges are
//   the two closed ones that enclose the largest areas (Super Speedway: 424
//   and 411 points, 2.2 / 2.5 km; Trial Mountain 882 / 859, 4.1 / 3.8 km);
//   others are pit lanes and lines that go out and back (no area).

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace gt3race
{
    struct Car
    {
        int index = 0;
        bool player = false;
        int position = 0;
        int lap = 0;
        uint32_t lapStartMs = 0;
        uint32_t lastLapMs = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float distance = 0.0f;
        float progress = 0.0f; // metres into the lap (just short of a lap on the grid, lap 0)
        float yaw = 0.0f;      // heading: the car points along (-sin yaw, -cos yaw) in (x, z)
        float speed = 0.0f;    // m/s (car + 0x140c)
        int rpm = 0;           // engine (u16 at car + 0x1414)
        int gear = 0;          // byte at car + 0x13ba (1 = first)
        int redline = 0;       // rpm where the rev counter turns red (u16 at car + 0x236; to be confirmed on more cars)
        // Each wheel's surface (byte at car + 0x1008 + 0xa4 * wheel): 0 on
        // tarmac, 4 in Laguna Seca's gravel (RAM dumps `surf-*`, 2026-10-01;
        // other values still being mapped).
        uint8_t surface[4] = {};
        std::string name;      // maker and model ("TOYOTA ALTEZZA RS200"); empty if not found
        std::string model;     // the model alone ("ALTEZZA RS200")
    };

    struct Snapshot
    {
        uint32_t race = 0; // the race object's address (a new race: a new value or clock)
        uint32_t clockMs = 0;
        // GT3's gauge lamps as its HUD shows them: ASM, TCS, parking brake,
        // oil. The race HUD update 0x192a58 writes them each frame to bytes
        // hud + 0xA15C + 0x18..0x1B from the player's car: 0x1EB330 (car byte
        // +0x14A4), 0x1EB338 (+0x14A5), 0x1EB360 (float +0x13B4 > 0.25) and
        // 0x1EB340 (0x1D9E20(car + 0x137C): the float at car + 0x1384 at or
        // past a limit from 0x1DACF0(0x2D2B00)).
        bool lamps[4] = {};
        // The race's laps as GT3's HUD shows "Lap n/total": hud + 0x78 (the
        // lap shown) and + 0x7C (the total; RAM dumps of a 2-lap race in laps 1
        // and 2, 2026-09-30). 0 if not known.
        int totalLaps = 0;
        // The race is a replay (GT3 replays the race object after an Arcade
        // race): u32 race + 0xb7ac == 1. Provisional, from RAM dumps: 0 in
        // every race dump (Super Speedway, Trial Mountain, 18 dumps), 1 in all
        // 5 of a Super Speedway replay; race + 0xbda0 was 8 / 2 the same way.
        bool replay = false;
        std::vector<Car> cars;
    };

    // Wraps 0x193040 (only if this CORE has the expected code there).
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    // The race state if a race HUD ran within the last half second. Names are
    // looked up (and cached) only with `names`: a scan of EE RAM the first
    // time (tens of ms), so from a worker thread (the phone view's sampler,
    // the HUD's name thread), not the host's drawing.
    bool read(const uint8_t *rdram, Snapshot &out, bool names = true);
    // The course's two edges (see above), each as x, z pairs, by a scan of
    // EE RAM (~10-30 ms); only if every car in `s` is inside the outer one's
    // box. False if not found (e.g. still loading).
    bool findCourseEdges(const uint8_t *rdram, const Snapshot &s, std::vector<std::vector<float>> &edges);
}
