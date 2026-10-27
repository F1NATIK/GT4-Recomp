#pragma once

#include <cstdint>

// Gameplay video recording (GT3_RECORD=<file.mp4|.mkv>): each new game frame
// the host shows is scaled on the GPU, read back asynchronously and encoded
// on a thread of its own by the GPU's video encoder where there is one
// (NVENC; MediaFoundation for AMD / Intel; MPEG-4 on the CPU otherwise).
// Frames carry the host clock, so a slow frame is a longer frame in the
// video; <file>.frames.csv lists each frame's time and VSync tick, to match
// GT3_FRAME_TIMES. GT3_RECORD_HEIGHT=<lines> (default 720).
namespace gt3rec
{
    bool enabled();
    // The host's display texture holds a new frame (width x height at its
    // origin). On the host's OpenGL thread.
    void captureFrame(unsigned texture, uint32_t width, uint32_t height, uint64_t vsyncTick);
    // Writes the rest of the video (before the OpenGL context goes).
    void shutdown();
}
