#pragma once

// GT4's race state, read-only (HUD and telemetry screen, hud/HANDOFF-1-RACE-READER.md; the fields and their evidence:
// analysis/design/race-reader/RACE-STATE.md). Once per race frame, on the race frame update 0x389638 (a0 = the race
// object R), the reader copies every car's place, laps, times, position and the player's inputs out of EE memory into
// a plain struct. It never writes guest memory. Full edition only (never Classic).
//   GT4_RACE_READER_CSV=<file>: test log, one line per car per race frame (hidden runs; analysis/design/race-reader).
//   GT4_RACE_READER_SCAN=1 (tests, HUD hand-off 7 C): once 40 s into a race and at the player's finish, every car's highest rpm
//   so far and the car record's / race entry's words that could hold its rev limit, and the race entry's text (driver names?).

#include <cstdint>
#include <cstdio>

namespace gt4race
{
    constexpr int kMaxCars = 8;
    constexpr int kMaxSectors = 4; // GT4's timing lines a lap (the finish line included): 3 on Autumn Ring Mini and New York

    struct Car
    {
        int place = 0;            // 1 = leader (car+0x5b3)
        int lap = 0;              // current lap number: 0 before the line on a rolling start, laps + 1 after the finish (car+0x5b0)
        int lapsDone = 0;         // laps completed (entry+0x1ac)
        uint32_t lastLapMs = 0;   // entry+0x11dc
        uint32_t bestLapMs = 0;   // entry+0x1198
        uint32_t totalMs = 0;     // sum of completed laps (entry+0x1b0)
        uint32_t lapTimeMs = 0;   // current lap: race clock - time at the last line crossing (car+0x718)
        bool finished = false;    // car+0x5b6
        bool player = false;      // the car a pad drives (controller vptr 0x686618, +0x1cc != 0)
        float lapDistance = 0.0f; // metres from the control line along the course's reference line (car+0x5a8)
        float x = 0.0f, y = 0.0f, z = 0.0f; // world position, metres, z up (car+0x104..0x10c)
        float yaw = 0.0f;         // radians from +x toward +y (rotation row 0, car+0x110 / +0x114)
        float speed = 0.0f;       // forward speed m/s (car+0x640)
        uint32_t carCode = 0;     // GENERIC_CAR code (entry+0x20)
        char name[64] = {};       // GT4's short name, as its results board prints it (entry+0x350c)
        float rpm = 0.0f;         // car+0x738 (every car; the player's is also Player::rpm)
        // HUD hand-off 7 C, the split times (RACE-STATE.md section 1; checked in stage C, STAGE-C-REPORT.md):
        int sector = 0;                          // the timing lines passed this lap (car+0x5b2): 0 in the first sector
        uint32_t sectorMs[kMaxSectors] = {};     // this lap's sector times so far (u32 D+0xd624 + (car*16 + k)*4; 0: none)
        uint32_t bestSectorMs[kMaxSectors] = {}; // the car's best time for each sector this race (u32 D+0xd7a4 + (car*16 + k)*4)
    };

    struct Player
    {
        int index = -1;           // car index, -1: no player car (replay, demo)
        float speed = 0.0f;       // m/s
        int gear = 0;             // 1 = first
        float rpm = 0.0f;
        float throttle = 0.0f;    // driver accelerator 0..1 (car+0x58c)
        float brake = 0.0f;       // 0..1 (car+0x590)
        float steer = 0.0f;       // -1..+1, + = left (car+0x560)
        bool shifting = false;    // a gear change in progress (car+0x610 != 0); no clutch value in GT4's arcade cars
        float accelLong = 0.0f;   // m/s2 (car+0x654)
        float accelLat = 0.0f;    // m/s2 (car+0x658)
        uint32_t lapTimeMs = 0;
    };

    enum class Phase
    {
        None,     // no race frame seen recently (menus)
        PreRace,  // the pre-race screen (R+0xcc8 == 4)
        Starting, // live race, before the start event (R+0xd4c == 0)
        Racing,
        Paused,   // R+0xcd8 == 1
        Finished, // the player car (or, without one, every car) took the flag
        Replay,   // R+0xcc8 == 2
    };

    struct State
    {
        uint64_t raceFrames = 0; // race frame updates seen (0: never)
        uint64_t vsync = 0;      // VSync tick of the last update
        Phase phase = Phase::None;
        int mode = 0;            // R+0xcc8 as read
        uint32_t course = 0;     // course id (O+0x78): 0x07 New York, 0x51 Autumn Ring Mini, ...
        int laps = 0;            // race laps (*O)
        uint32_t clockMs = 0;    // race clock (D+0xf898 / 3)
        float courseLength = 0;  // metres at which lapDistance wraps, once a car was seen wrapping (0: not known yet)
        int startHold = 0;       // the grid countdown, frames left (O+0xbc): 599 -> 0 on a standing start, 0 on a rolling start
        int sectorCount = 3;     // the course's sectors a lap (3; more once a car is seen past a third timing line)
        // GT4's own units (its Options, mirrored at 0x6244d8 by Option::apply 0x4365b8): speed 0 km/h, 1 mph; power 0 PS,
        // 1 HP, 2 BHP, 3 CH, 4 kW, 5 CV, 6 PF; torque 0 kgf.m, 1 ft.lb, 2 Nm (-1: not read)
        int unitSpeed = -1, unitPower = -1, unitTorque = -1;
        int carCount = 0;
        Car cars[kMaxCars];
        Player player;
    };

    // The hook (ps2_runtime.cpp, on the call to 0x389638): r = a0.
    void onRaceFrame(const uint8_t *rdram, uint32_t r, uint64_t vsync);
    // The reader CSV's header line and one race frame's rows (one per car): GT4_RACE_READER_CSV and the Race Lab
    // recorder's whole-race file (gt4_drive_log.h).
    void writeCsvHeader(std::FILE *f, bool sectors = false);
    // (sectors: the test CSV's extra columns sector,s0..s3,b0..b3 at the end of each row; never in the Race Lab's file)
    void writeCsvRows(std::FILE *f, const State &s, bool sectors = false);
    // A copy of the latest state (thread safe). False before the first race frame.
    bool snapshot(State &out);
    // Milliseconds since the last race frame update (a large value: none yet).
    uint64_t msSinceLastFrame();
    // Whether the reader is on (Full edition with a matching CORE).
    bool enabled();
} // namespace gt4race
