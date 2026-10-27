#pragma once

#include <cstdint>

class PS2Runtime;

// GT3's AI cars (development build; gt3-analysis/AI-drivers.md). Both options
// wrap GT3's AI driver 0x1f7ae0 (after the autopilot's wrapper of it) and act
// once it has run for AI car 1-5.
//
// GT3_AI_LINES=<folder>: the AI cars drive the lines given in
// <folder>/<course>.txt (<course> as the drive log names it: the
// GT3_COURSE_REPLACE folder's name, else "runway-<crc32>"): per car a line in
// the autopilot's format (a recorded lap: GT4-WORK tools/ai_line_from_lap.py;
// the Racing Line Editor's "AI cars 1-5"), ± speed and braking points varied
// each lap, mistakes (late braking, running wide, early throttle) at a chance
// per braking zone, and passing (gap kept to the car ahead, a side with room
// taken, away from a car alongside). The car's controller steering / throttle /
// brake (+0x9c / +0xa0 / +0xa4) are overwritten after GT3's AI has run; GT3's
// own AI takes over for a few seconds when a car is far off its line or stuck.
//
// GT3_AI_TRACE=1: the controller (car + 0xec8) and the car's own throttle /
// brake / steering (car + 0x13ac / 0x13b0 / 0x13a0), speed, heading and lap
// distance to stderr every 6th frame ("[ai-trace]").
namespace gt3aipilot
{
    void install(PS2Runtime &runtime, const uint8_t *rdram);
}
