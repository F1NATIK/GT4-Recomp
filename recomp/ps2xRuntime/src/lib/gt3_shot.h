#pragma once

#include <atomic>

// Screenshots at places on the course (development build): GT3_SHOT_POINTS=<file
// of "x z" lines> makes the autopilot ask for one as the player's car passes each
// point (within 4 m), saved by GT3_PRESENT_SHOT's code as <prefix>-at<NNN>.png.
// The same points on two runs (e.g. GT3's course and a converted GT4 one) give
// pictures from the same spots.
namespace gt3shot
{
    extern std::atomic<int> request; // point index to save at the next presented frame, -1 none
}
