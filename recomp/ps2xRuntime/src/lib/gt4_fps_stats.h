#pragma once

// The PC numbers for GT4's detailed FPS counter (gt4_fps_counter.h): CPU load, GPU load, RAM and video
// memory, sampled four times a second on a thread of their own (below-normal priority) while the
// detailed counter shows any of them. Windows only (elsewhere every number is "not known").
//   CPU   GetSystemTimes (the whole PC) and GetProcessTimes (GT4), over the last quarter second
//   GPU   the "GPU Engine" performance counters (PDH; NVIDIA, AMD and Intel alike): the busiest engine
//         type of GT4's graphics card, all programs and GT4's own share (as Task Manager shows it)
//   RAM   GetProcessMemoryInfo (GT4's working set) and GlobalMemoryStatusEx (the PC)
//   VRAM  DXGI QueryVideoMemoryInfo (GT4's video memory) and the "GPU Adapter Memory" counters (all
//         programs' dedicated use), with the card's dedicated memory
// GT4's graphics card: the one its OpenGL window is on (GL_EXT_memory_object's device LUID, read on
// the presenter thread, noteRenderGpu); the Vulkan renderer picks the same one. Without it: the
// card with the most dedicated memory.

#include <string>

namespace gt4fps::stats
{
    struct Sample
    {
        bool cpuValid = false;
        float cpuAll = 0, cpuGt4 = 0; // % of all logical processors
        bool gpuValid = false;
        float gpuAll = 0, gpuGt4 = 0; // % (the busiest engine type)
        bool ramValid = false;
        double ramGt4Mb = 0, ramUsedMb = 0, ramTotalMb = 0;
        bool vramValid = false;
        double vramGt4Mb = 0, vramUsedMb = -1, vramTotalMb = 0; // vramUsedMb < 0: all programs' use not known
        std::string gpuName;                                     // empty: not known
    };

    // The detailed counter shows a PC number (starts the sampling thread the first time).
    void want(bool on);
    Sample latest();
    // Presenter thread, with its OpenGL context current: the GPU it is on (once).
    void noteRenderGpu();
}
