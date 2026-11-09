#pragma once

// Arcade's courses (analysis/design/tracks; developer 2026-10-27: Valencia added "only in arcade mode ... correctly as a new
// track", and "a section in the launcher where we choose which tracks appear in game"). Not the Classic edition.
//   - a course the extra-cars pack adds (analysis/design/tracks/scripts/build_tracks_pack.py: Tourist Trophy's Valencia,
//     COURSE code 0xee) is offered in Arcade without writing the save: GT4's course-available check (0x42eae8: a0 the
//     save's Available lists, a1 the course code; Arcade's course lists skip courses it says no to) answers yes for it;
//   - GT4_TRACKS_HIDE=<COURSE codes, comma separated (decimal or 0x..)>: the launcher's track picker; the check answers
//     no for them, so Arcade's lists leave them out (GT4's own unlock state in the save is not touched).
//   - GT4_TRACKS_UNLOCK=on (developer 2026-10-29: "can you unlock all the tracks for me in arcade mode?"): the check answers yes
//     for every course not hidden, so Arcade lists them all whatever the save has unlocked (the save is not written).
// GT Mode never lists the added course (its events name their own courses).

//   - bigger courses (developer 2026-10-28: "is it possible to increse the 12mb limit since we are running GT4 natively on
//     PC"): GT4 reads a course into a 12 MB block of its course pool (0x394ed8: block from 0x4638d8(pool 0x6214b0), then
//     0x3978f8(name, block, 0xbc8000) reads the file, and 0x396298 relocates it where it landed). The read is sent to
//     guest RAM above the PS2's 32 MB instead (PS2_RAM_SIZE), with that much room, so a course can be up to ~32 MB.
//     The pool block is still taken and given back as before. GT4_COURSE_RAM=0 keeps GT4's own 12 MB.

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace gt4tracks
{
    // Called at the first call of GT4's isCourseAvailable script method (0x166670): wraps 0x42eae8 once.
    void install(uint8_t *rdram, PS2Runtime *runtime);
    // Called at 0x394fe4's call of 0x3978f8 (the course file read): moves the read above 32 MB.
    void courseRead(uint8_t *rdram, R5900Context *ctx);
}
