#pragma once

// GT4's FPS counter (GT4 builds, not the Classic edition): a small box in the window's top left
// corner. A frame is a flip of GT4's displayed buffer (GS::frameStats / GS::recentFrameGapsMs).
// Drawn by the presenter after the scanline pass, into the window only: never into GT4's frame, its
// photos or its replays (as the cheat card, gt4_cheat_card.h).
//
//   GT4_FPS_COUNTER   off (default) | simple | detailed   (on / 1: detailed, the counter before simple
//                     and detailed). F3 while playing: off -> simple -> detailed -> off.
//   GT4_FPS_STATS     what detailed shows: a comma list of fps, frametime, worst, low1, graph, cpu,
//                     gpu, ram, vram, resolution, renderer (absent, empty or "all": every one).
//                     Simple always shows fps and frame time only.
//
// fps: frames per second over the last second. frametime: the latest frame's time (ms). worst: the
// longest frame of the last second. low1: the 1% low fps (the slowest 1% of the last ~10 s of frames).
// graph: the recent frame times. cpu / gpu: the PC's load and GT4's share (CPU: GetSystemTimes and
// GT4's process times; GPU: Windows' "GPU Engine" performance counters, any vendor). ram: GT4's
// memory and the PC's. vram: GT4's video memory (DXGI) and the graphics card's in use (Windows' "GPU
// Adapter Memory" counters). resolution: the internal resolution. renderer: Vulkan / OpenGL and the GPU.
// The PC numbers are sampled four times a second on their own thread (gt4_fps_stats.h), never on
// the presenter's.

#include <cstddef>
#include <string>

namespace gt4fps
{
    enum class Mode
    {
        Off = 0,
        Simple = 1,
        Detailed = 2,
    };

    // The stats detailed can show (GT4_FPS_STATS names, in this order).
    enum Stat : unsigned
    {
        StatFps = 1u << 0,
        StatFrameTime = 1u << 1,
        StatWorst = 1u << 2,
        StatLow1 = 1u << 3,
        StatGraph = 1u << 4,
        StatCpu = 1u << 5,
        StatGpu = 1u << 6,
        StatRam = 1u << 7,
        StatVram = 1u << 8,
        StatResolution = 1u << 9,
        StatRenderer = 1u << 10,
        StatAll = (1u << 11) - 1u,
    };
    inline constexpr const char *kStatNames[11] = {"fps", "frametime", "worst", "low1", "graph", "cpu", "gpu", "ram", "vram", "resolution", "renderer"};

    struct Frame
    {
        float screenW = 0, screenH = 0; // the window
        double fps = 0.0;               // GS::frameStats over the last second (0: no frame for a second)
        double worstMs = 0.0;           // the longest frame time of the last second
        const float *gapsMs = nullptr;  // the latest frame times, newest first (GS::recentFrameGapsMs)
        size_t gaps = 0;
        const char *resolution = "";    // the internal resolution, e.g. "2x, 1280 x 896, exact"
    };

    Mode mode();
    bool shown();
    // F3: the next mode (off -> simple -> detailed -> off); returns it.
    Mode cycle();
    const char *modeName(Mode m);
    // The renderer GT4 draws with ("Vulkan", "OpenGL", "CPU"): set once at start-up.
    void setRenderer(const char *name);
    // Presenter thread, every frame, after the picture is drawn (BeginDrawing..EndDrawing).
    void draw(const Frame &f);
}
