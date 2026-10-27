#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_crt.h"
#include "runtime/gs/gs_fullcolour.h"
#include "runtime/gs/gs_car_view.h" // the renderer's full-height car view in GT4's 480p menus (D5 car picture)
#include "runtime/ee_scheduler.h"
#include "runtime/vblank_clock.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "Kernel/Stubs/Gt4HostGlue.h"
#if defined(PS2X_GAME_GT4)
#include "Kernel/Stubs/Gt4Pad2.h"
#endif
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "input_call_trace.h"
#if defined(PS2X_GAME_GT3)
#include "runtime/gt3_movie_worker.h"
#endif
#include "ps2x/iop/iop_subsystem.h"
#if defined(PS2X_GS_GL)
#include "runtime/gs/gs_gl_backend.h"
#include "runtime/gs/gs_async_backend.h"
#include "gs/gs_gl_context.h"
#include "gs/gs_gl_present.h"
#endif
#if defined(PS2X_GS_VK)
#include "gs/gs_vk_context.h"
#include "gs/gs_vk_backend.h"
#include "gs/gs_vk_present.h"
#endif
#include "present_overlay.h"
#if defined(PS2X_UPSCALERS)
#include "present_upscale.h"
#include "present_motion.h"
#endif
#if defined(PS2X_GAME_GT3)
#include "ps2x/iop/gt3/pdi1394_link.h"
#include "gt3_aspect.h"
#endif
#include "rlgl.h" // rlDrawRenderBatchActive (GT3_WINDOW_SHOT)
#include "runtime/ps2_pad_rumble.h"
#include "runtime/ps2_input_profile.h"
#if !defined(PS2X_GAME_GT3)
// GT4: the scanline pass (present_scanline.h) in place of GT3's TV filter.
#include "present_scanline.h"
#define PS2X_SCANLINE_PASS 1
#endif
#if defined(PS2X_GAME_GT4)
#include "gt4_cheat_card.h" // GT4_CHEAT_CARD: the in-game cheat card (F2)
#include "gt4_fps_counter.h" // GT4_FPS_COUNTER: the FPS counter (F3; not the Classic edition)
#include "gt4_hud.h" // GT4_HUD_* / GT4_START_*: our own race HUD widgets and start lights (not the Classic edition)
#include "gt4_pages.h" // GT4_PAGES: the pages service and telemetry window (not the Classic edition)
#include "gt4_mirror.h" // the rear-view mirror page (GET /mirror.mjpg; not the Classic edition)
#include "gt4_hud_pictures.h" // the launcher's race pictures (HUD hand-off 7 A1; not the Classic edition)
#include "gt4_vu1_tap.h" // GT4_VU1_TAP: the VU1 tap (lighting step 1; not the Classic edition)
#include "gt4_race_reader.h" // the race reader: read-only race state for the HUD / telemetry (not the Classic edition)
#include "gt4_start_hold.h" // GT4_START_HOLD: holding the start (HUD hand-off 7 E; not the Classic edition)
#include "gt4_drive_log.h" // GT4_DRIVE_LOG: the Race Lab lap recorder (F10)
#include "gt4_movie_feed.h" // the movie stutter fix: GT4's movie feed copy done natively (analysis/design/movie-stutter)
#include "gt4_multimon.h" // GT4_MM_*: GT4's own Multi Monitor Mode settings (not the Classic edition)
#include "gt4_texture_dump.h" // GT4_TEXTURE_DUMP: the texture dumper (not the Classic edition)
#include "gt4_texture_replace.h" // GT4_TEXTURE_REPLACE: texture replacement (not the Classic edition)
#include "ps2x/iop/gt4_cheats_served.h" // setVideoOutputHidden (GT4_RACE_MODE=480p: GT4's Video Output line)
#include "ps2x/iop/gt4_car_state.h"     // Custom Sounds: the race cars' engine state for the sound module
#endif
// GT3_STOCK_BUILD (CMake GT3_STOCK_BUILD=ON): the game as on a PS2 - none of
// the additions below are compiled in (their sources are left out of the
// build), and no hotkeys but F1 (the TV filter).
// GT4 builds: none of GT3's additions (GT4 reuse policy, PLAN.md).
// GT4_STOCK_BUILD (CMake GT4_STOCK_BUILD=ON, PLAN phase 6): the GT4-Stock
// package - native resolution, adaptive fields, GT4's own picture shape (4:3,
// 16:9 with GT4's widescreen setting, 1.8:1 in 1080i), the OpenGL renderer and
// the scanline pass as its only filter (F1), whatever the environment says.
#if defined(GT3_STOCK_BUILD) || defined(PS2X_GAME_GT4)
#define GT3_ADDONS 0
#else
#define GT3_ADDONS 1
#include "gt3_motion.h"
#include "gt3_phone_view.h"
#include "gt3_recorder.h"
#include "gt3_radar.h"
#include "gt3_camera.h"
#include "gt3_race_info.h"
#include "gt3_course_patch.h"
#include "gt3_ai_pilot.h"
#include "gt3_start_lights.h"
#include "gt3_race_report.h"
#include "gt3_drive_log.h"
#include "gt3_hud.h"
#include "gt3_shot.h"
#include "gt3_autopilot.h"
#include "gt3_texture_dump.h"
#include "gt3_texture_replace.h"
#endif

#include <iostream>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <exception>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <map>
#include <sstream>
#include <functional>

namespace
{
// Set when guest code yields at a scheduler checkpoint (the EE executor is the
// only thread running guest code). See dispatchGuestBranch.
thread_local bool g_eeYieldTaken = false;
// (PS2Runtime::eeNoYield) host code running guest code it cannot resume: no checkpoint yields
thread_local int g_eeNoYield = 0;
std::atomic<uint64_t> *g_ps2WatchTickSource = nullptr;
uint64_t g_ps2WatchFromTick = 0u;
std::atomic<uint32_t> g_ps2WatchReports{0u};

#if defined(_WIN32)
// --headless: Win32 window checks (declared here: windows.h clashes with raylib).
extern "C" __declspec(dllimport) int __stdcall EnumWindows(int(__stdcall *callback)(void *, intptr_t), intptr_t param);
extern "C" __declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void *window, unsigned long *processId);
extern "C" __declspec(dllimport) int __stdcall IsWindowVisible(void *window);
extern "C" __declspec(dllimport) void *__stdcall GetForegroundWindow(void);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError(void);

struct WindowScan
{
    unsigned long pid = 0;
    bool visible = false;
};
int __stdcall scanWindow(void *window, intptr_t param)
{
    auto *scan = reinterpret_cast<WindowScan *>(param);
    unsigned long owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == scan->pid && IsWindowVisible(window))
        scan->visible = true;
    return 1;
}
#endif

#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
// The guarded raylib copy (cmake/RaylibInitGuard.cmake): non-zero leaves a
// hidden window's buffer swap out of EndDrawing (a glFlush instead).
extern "C" int ps2x_skip_hidden_swap;
// The guarded raylib copy: non-zero leaves the buffer swap out of EndDrawing
// altogether (a glFlush instead): the Vulkan renderer's window presenter
// (gs/gs_vk_present.h) shows the frames through its own swapchain.
extern "C" int ps2x_vulkan_presents;

// GT4 --headless buffer swap (PS2X_HEADLESS_SWAP): by default the hidden
// window swaps like a visible one, so headless timing includes the swap, and
// skips it only while the console display is off. With the display off
// (locked PC, Modern Standby) the NVIDIA present of the hidden window
// (through DXGI on the Intel display adapter) blocked ~0.5 s about once a
// second and stalled the GS GL thread and then the EE
// (analysis/phase5/scratch/step3/lockstall). The display state comes from a
// power-setting notification (read only, no window). 1: always swap; 0: never.
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryExA(const char *name, void *file, unsigned long flags);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
enum class HeadlessSwap
{
    WhileDisplayOn,
    Always,
    Never
};
HeadlessSwap g_headlessSwap = HeadlessSwap::WhileDisplayOn;
std::atomic<int> g_consoleDisplay{-1}; // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed; -1 not known
struct HostGuid
{
    unsigned long d1;
    unsigned short d2, d3;
    unsigned char d4[8];
};
constexpr HostGuid kConsoleDisplayState{0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
struct PowerSettingChange // POWERBROADCAST_SETTING
{
    HostGuid setting;
    unsigned long dataLength;
    unsigned char data[1];
};
unsigned long __stdcall onConsoleDisplayState(void *, unsigned long type, void *setting)
{
    const auto *change = static_cast<const PowerSettingChange *>(setting);
    if (type != 0x8013u /* PBT_POWERSETTINGCHANGE */ || change == nullptr || change->dataLength < 4u ||
        std::memcmp(&change->setting, &kConsoleDisplayState, sizeof(HostGuid)) != 0)
        return 0;
    unsigned long state = 0;
    std::memcpy(&state, change->data, sizeof(state));
    g_consoleDisplay.store(static_cast<int>(state), std::memory_order_relaxed);
    return 0;
}

// Reads PS2X_HEADLESS_SWAP and, by default, starts watching the console
// display state (Windows reports the current state at once). Returns how the
// swap is handled, for the [headless] line.
std::string startHeadlessSwap()
{
    const char *v = std::getenv("PS2X_HEADLESS_SWAP");
    g_headlessSwap = (v && *v == '1') ? HeadlessSwap::Always : (v && *v == '0') ? HeadlessSwap::Never : HeadlessSwap::WhileDisplayOn;
    if (g_headlessSwap == HeadlessSwap::Always)
        return "buffer swap always (PS2X_HEADLESS_SWAP=1)";
    if (g_headlessSwap == HeadlessSwap::Never)
        return "no buffer swap (PS2X_HEADLESS_SWAP=0)";
    struct SubscribeParameters // DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS
    {
        unsigned long(__stdcall *callback)(void *, unsigned long, void *);
        void *context;
    };
    using RegisterFn = unsigned long(__stdcall *)(const HostGuid *, unsigned long, void *, void **);
    static SubscribeParameters parameters{onConsoleDisplayState, nullptr};
    static void *registration = nullptr;
    void *powrprof = LoadLibraryExA("powrprof.dll", nullptr, 0x800u /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    const auto registerFn = powrprof ? reinterpret_cast<RegisterFn>(GetProcAddress(powrprof, "PowerSettingRegisterNotification")) : nullptr;
    if (!registerFn || registerFn(&kConsoleDisplayState, 2u /* DEVICE_NOTIFY_CALLBACK */, &parameters, &registration) != 0u)
    {
        g_headlessSwap = HeadlessSwap::Always;
        return "buffer swap always (the display state could not be read)";
    }
    for (int i = 0; i < 250 && g_consoleDisplay.load(std::memory_order_relaxed) < 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int display = g_consoleDisplay.load(std::memory_order_relaxed);
    return std::string("buffer swap while the display is on (display now ") + (display == 0 ? "off" : display < 0 ? "not known" : "on") +
           "; PS2X_HEADLESS_SWAP=1 always, 0 never)";
}

// Host loop, each frame: the swap mode for this frame's EndDrawing. A change
// is logged with the VSync, so a run's numbers can be split by mode.
void updateHeadlessSwap(uint64_t vsync)
{
    const int display = g_consoleDisplay.load(std::memory_order_relaxed);
    const int skip = g_headlessSwap == HeadlessSwap::Never ? 1 : g_headlessSwap == HeadlessSwap::Always ? 0 : (display == 0 ? 1 : 0);
    if (skip == ps2x_skip_hidden_swap)
        return;
    ps2x_skip_hidden_swap = skip;
    std::fprintf(stderr, "[headless] %s from vsync %llu (console display %s)\n", skip ? "buffer swap skipped" : "buffer swap on",
                 static_cast<unsigned long long>(vsync), display == 0 ? "off" : display < 0 ? "not known" : "on");
}

// GT4 visible play only (never --headless, and --cpu-only has no window):
// keep the display on while the window is shown. Pad input does not reset
// Windows' idle timer, so a long race or B-Spec session reached the display-off
// timeout, and with the display off the present blocks on this laptop's
// Optimus path and stalls the game (analysis/phase5/specs/step4-hitches.md P6).
// A request of this thread (the host loop's): cleared while the window is
// minimized and at exit, and it ends with the process; no setting changes.
// PS2X_DISPLAY_REQUIRED=0: no request.
extern "C" __declspec(dllimport) unsigned long __stdcall SetThreadExecutionState(unsigned long flags);
int g_displayRequired = -1; // -1 not decided yet, 0 cleared, 1 set
void updateDisplayRequired(bool want)
{
    static const bool enabled = [] {
        const char *v = std::getenv("PS2X_DISPLAY_REQUIRED");
        const bool on = !(v && *v == '0');
        if (!on)
            std::fprintf(stderr, "[display] keep-on request off (PS2X_DISPLAY_REQUIRED=0)\n");
        return on;
    }();
    if (!enabled || (want ? 1 : 0) == g_displayRequired || (!want && g_displayRequired < 0))
        return;
    constexpr unsigned long kContinuous = 0x80000000ul, kSystemRequired = 0x1ul, kDisplayRequired = 0x2ul; // ES_*
    const unsigned long previous = SetThreadExecutionState(want ? (kContinuous | kSystemRequired | kDisplayRequired) : kContinuous);
    g_displayRequired = want ? 1 : 0;
    std::fprintf(stderr, "[display] keep-on request %s%s\n", want ? "set (window shown)" : "cleared",
                 previous == 0ul ? " - the request failed" : "");
}
#endif

// --headless: fail closed. Any top-level window of this process (raylib's,
// the GL renderer's context, GLFW's helper) visible, or one of them in front,
// or the windows could not be listed: the process ends at once (exit code 4),
// before the next frame.
void stopIfHeadlessWindowShown(const char *where)
{
#if defined(_WIN32)
    WindowScan scan;
    scan.pid = GetCurrentProcessId();
    if (!EnumWindows(scanWindow, reinterpret_cast<intptr_t>(&scan)))
    {
        // scanWindow never stops the enumeration: a zero return is a failure.
        std::cerr << "[headless] the window check failed (error " << GetLastError() << ", " << where << "); stopping"
                  << std::endl;
        std::cout.flush();
        std::_Exit(4);
    }
    unsigned long front = 0;
    if (void *window = GetForegroundWindow())
        GetWindowThreadProcessId(window, &front);
    if (!scan.visible && front != scan.pid)
        return;
    std::cerr << "[headless] a window of this process is " << (scan.visible ? "visible" : "in front") << " (" << where
              << "); stopping" << std::endl;
    std::cout.flush();
    std::_Exit(4);
#else
    (void)where;
#endif
}

// VIF1 and VU1 on their own thread unless GT3_VIF1_THREAD=0. A graphics
// trace (GT3_GFX_TRACE_PREFIX) keeps them on the EE thread: its records are
// not thread safe.
bool vif1ThreadEnabled()
{
    const char *v = std::getenv("GT3_VIF1_THREAD");
    if (v && *v == '0')
        return false;
    const char *capture = std::getenv("GT3_GFX_TRACE_PREFIX");
    return !(capture && *capture);
}

// PS2X_FPS_LOG=<seconds> (0 or unset: off).
double frameLogPeriod()
{
    static const double period = [] {
        const char *v = std::getenv("PS2X_FPS_LOG");
        return v && *v ? std::atof(v) : 0.0;
    }();
    return period;
}

// PS2X_FPS_LOG: game frames timed on the EE thread, at the VSync where the
// displayed buffer changes (the flip), so the GS worker's lag behind the EE
// and the GS in use (CPU, OpenGL, inline) do not change the numbers. The
// gaps are those since the last report: how many, their sum and the
// longest, in ns and in VSyncs.
struct FrameLog
{
    std::mutex mutex;
    uint32_t lastFbp = ~0u; // EE thread only
    uint64_t flips = 0, lastFlipTick = 0;
    int64_t lastFlipNs = 0;
    uint64_t gaps = 0, maxVSyncs = 0;
    int64_t sumNs = 0, maxNs = 0;
};
FrameLog g_frameLog;

int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

// The EE FPU and the VUs round toward zero and treat denormals as zero. Host
// round-to-nearest let GT3 compute two car-outline corners with the same x, so
// a side edge's line got +/-Fmax coefficients and the first car-to-car contact
// blew up the physics. Set on every thread that runs guest code or VU
// microprograms (the game thread or --cpu-only's EE thread, the VIF1/VU1
// worker). GT3_EE_ROUND=nearest keeps the host default.
void applyGuestFloatMode()
{
    const char *round = std::getenv("GT3_EE_ROUND");
    if (!(round && std::strcmp(round, "nearest") == 0))
        _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u /* RC toward zero */ | 0x8000u /* FTZ */ | 0x0040u /* DAZ */);
}

void ps2ReportWatchedWrite(uint32_t guestAddr, uint32_t size, uint64_t valueLo, uint64_t valueHi,
                           const char *op, const R5900Context *ctx)
{
    const uint64_t tick = g_ps2WatchTickSource ? g_ps2WatchTickSource->load() : 0u;
    if (tick < g_ps2WatchFromTick || g_ps2WatchReports.fetch_add(1u) >= 20000u)
        return;
    std::fprintf(stderr, "[write-watch] tick=%llu pc=%08x ra=%08x %s addr=%08x size=%u lo=%016llx hi=%016llx\n",
                 static_cast<unsigned long long>(tick), ctx ? ctx->pc : 0u,
                 ctx ? static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31])) : 0u, op, guestAddr, size,
                 static_cast<unsigned long long>(valueLo), static_cast<unsigned long long>(valueHi));
}

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    // Opt-in diagnostic: GT3_VU1_RECORD=<file> [GT3_VU1_RECORD_FROM_TICK=n]
    // [GT3_VU1_RECORD_KICKS=n (default 2000)] writes every VU1 microprogram kick
    // (MSCAL/MSCNT) with its inputs (state, cycle, code when it changed, data)
    // and results (state, cycle, data hash, XGKICK packet hash), for the VU1
    // replay benchmark / differential test (gt3-tools/vu1_replay_bench.cpp).
    struct Vu1KickRecorder
    {
        std::FILE *file = nullptr;
        uint64_t fromTick = 0;
        long remaining = 0;
        std::vector<uint8_t> lastCode;
        uint64_t packetHash = 0, packetBytes = 0, packetCount = 0;
        bool recording = false;

        static uint64_t fnv(uint64_t hash, const uint8_t *data, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ data[i]) * 1099511628211ull;
            return hash;
        }

        Vu1KickRecorder()
        {
            const char *path = std::getenv("GT3_VU1_RECORD");
            if (!path || !*path)
                return;
            file = std::fopen(path, "wb");
            if (!file)
                return;
            std::fwrite("GT3VU1K1", 1, 8, file);
            if (const char *v = std::getenv("GT3_VU1_RECORD_FROM_TICK"); v && *v)
                fromTick = std::strtoull(v, nullptr, 10);
            remaining = 2000;
            if (const char *v = std::getenv("GT3_VU1_RECORD_KICKS"); v && *v)
                remaining = std::strtol(v, nullptr, 10);
        }

        ~Vu1KickRecorder()
        {
            if (file)
                std::fclose(file);
        }

        void before(VU1Interpreter &vu, uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles,
                    const uint8_t *code, const uint8_t *data, uint64_t tick)
        {
            recording = file && remaining > 0 && tick >= fromTick;
            if (!recording)
                return;
            --remaining;
            const bool codeChanged = lastCode.size() != PS2_VU1_CODE_SIZE || std::memcmp(lastCode.data(), code, PS2_VU1_CODE_SIZE) != 0;
            if (codeChanged)
                lastCode.assign(code, code + PS2_VU1_CODE_SIZE);
            // header[7]: the vsync tick (low 32 bits), to split kicks into frames.
            const uint32_t header[8] = {0x4B43494Bu, kind, startPC, top, itop, maxCycles, codeChanged ? 1u : 0u, static_cast<uint32_t>(tick)};
            std::fwrite(header, sizeof(header), 1, file);
            const uint64_t cycle = vu.cycle();
            std::fwrite(&cycle, sizeof(cycle), 1, file);
            std::fwrite(&vu.state(), sizeof(VU1State), 1, file);
            if (codeChanged)
                std::fwrite(code, 1, PS2_VU1_CODE_SIZE, file);
            std::fwrite(data, 1, PS2_VU1_DATA_SIZE, file);
            packetHash = 1469598103934665603ull;
            packetBytes = packetCount = 0;
            vu.packetSink = [this](const uint8_t *packet, uint32_t size) {
                packetHash = fnv(packetHash, packet, size);
                packetBytes += size;
                ++packetCount;
                return false;
            };
        }

        void after(VU1Interpreter &vu, const uint8_t *data)
        {
            if (!recording)
                return;
            vu.packetSink = nullptr;
            const uint64_t cycle = vu.cycle();
            std::fwrite(&cycle, sizeof(cycle), 1, file);
            std::fwrite(&vu.state(), sizeof(VU1State), 1, file);
            const uint64_t tail[4] = {fnv(1469598103934665603ull, data, PS2_VU1_DATA_SIZE), packetHash, packetBytes, packetCount};
            std::fwrite(tail, sizeof(tail), 1, file);
            if (remaining == 0)
                std::fflush(file);
            recording = false;
        }
    };

    Vu1KickRecorder &vu1KickRecorder()
    {
        static Vu1KickRecorder recorder;
        return recorder;
    }
}

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;
    // Native kernel callback arena, below user ELF memory and above the kernel
    // syscall mirror. Never allocate these stacks from the game's main stack.
    constexpr uint32_t kCallbackStackFloor = 0x00080000u;
    constexpr uint32_t kCallbackStackTop = 0x00100000u;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// The display texture's filter (the run loop sets it; kept when the texture
// is remade for another internal resolution).
static int g_displayTextureFilter = TEXTURE_FILTER_BILINEAR;

// Frames UploadFrame put into its texture (a new picture; frame generation
// swaps its two textures on a change).
static uint64_t g_frameUploads = 0;

// GT4 grid pacing (GridPacer): the latch tick of the frame this loop presents;
// a GPU frame published early (a later latch) stays for the next present.
// ~0: the newest frame.
static uint64_t g_presentMaxTick = ~0ull;

#if defined(PS2X_GS_VK)
// The Vulkan renderer's window presenter (gs/gs_vk_present.h): the frame it holds (UploadFrame).
static gsvk::present::Frame g_vkFrame;
#endif

// The newest field's line parity of the frame in the display texture (a GPU
// frame's gsgl::FieldInfo; -1 for a CPU frame or a frame without fields):
// the scanline pass's field emphasis.
static int g_shownFieldParity = -1;
// The video mode (runtime/gs/gs_crt.h) and display lines (448 for 480i and
// 480p, 960 for 1080i; 0 unknown) of the frame in the display texture: its
// shape and the scanline pass's lines (analysis/design/video-modes).
static uint32_t g_shownCrt = 0;
static int g_shownLines = 0;
// The frame in the display texture is a field picture (a GPU frame woven or
// bobbed from fields, a CPU frame woven from an interlaced mode's half-height
// fields, FFMD 1): the picture pass blends its lines when the deinterlacing
// is Blend. A frame-mode picture (FFMD 0: one full-height buffer, read every
// other line per field; GT4's Race Monitor, which already softens it with its
// own one-line flicker filter) is whole, as the GPU presenter's parity -1
// frames: never blended.
static bool g_shownInterlaced = false;
#if defined(PS2X_SCANLINE_PASS)
// What the scanline pass draws for the shown picture's video mode; the live
// window and the PS2X_SCANLINE_SHOT files both go through it. 1080i: no
// lines and no field emphasis (developer, 2026-10-04: no scanlines in 1080i; its
// 960 lines get under 2 window rows each on his screens, where alternate
// brighter and darker lines would only make a moire). 480i and 480p: as set.
struct ScanForMode
{
    float strength = 0.0f, field = 0.0f;
    int parity = -1;
};
static ScanForMode scanForMode(uint32_t crt, float strength, float field, int parity)
{
    if (ps2x::crt::is1080i(crt))
        return ScanForMode{0.0f, 0.0f, -1};
    return ScanForMode{strength, field, parity};
}
#endif
#if !defined(PS2X_GAME_GT3)
// GT4's own Aspect Ratio setting (Options > Screen Settings: 4:3 / 16:9):
// its wide flag, DisplaySwitching+0x20 at 0x6184f0, which the options apply
// 0x4365b8 sets to Option+0x30 / Option+0x34 >= 1.5 (0x4390d0; VIDEO-MODES.md
// 3.1). With 16:9 GT4 draws a squeezed (anamorphic) picture that a 16:9 TV
// stretched: 480i and 480p are shown 16:9 (developer, 2026-10-04: "follow GT4's own
// setting"). Read unlocked once per host frame (one aligned word; it changes
// only when the options are applied). Off if CORE's store is not where expected.
// GT4_RACE_MODE=480p's menu part is active (dispatchGuestBranch, D5): for the
// [gt4-video] log line of applyMenuDisplayMode.
static std::atomic<bool> g_gt4Menus480p{false};


#if defined(PS2X_GAME_GT4)
// GT4_RACE_MODE=480p|game (analysis/design/480p-colour/DESIGN.md D1): true for 480p (unset, empty or 480p;
// anything else but "game" is logged once and read as 480p). Read once, after gt4-start.env is in the
// environment (loadELF and the hooks of dispatchGuestBranch ask).
static bool gt4RaceMode480p()
{
    static const bool on = [] {
        const char *v = std::getenv("GT4_RACE_MODE");
        std::string m = v ? v : "";
        for (char &c : m)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        m.erase(0, m.find_first_not_of(" \t"));
        m.erase(m.find_last_not_of(" \t") + 1);
        if (m == "game")
        {
            std::fprintf(stderr, "[gt4-video] GT4_RACE_MODE=game: GT4's own Video Output (Options > Screen Settings) and 480i menus\n");
            return false;
        }
        if (!m.empty() && m != "480p")
            std::fprintf(stderr, "[gt4-video] GT4_RACE_MODE=%s not understood (480p or game): 480p\n", v);
        return true;
    }();
    return on;
}

// The development test switch GT4_RACE_VIDEO_MODE is set: it wins over GT4_RACE_MODE (old test routes keep their
// baselines). Never in the Classic build.
static bool gt4RaceModeTestSwitch()
{
#if !defined(GT4_STOCK_BUILD)
    static const bool set = [] {
        const char *v = std::getenv("GT4_RACE_VIDEO_MODE");
        return v && *v;
    }();
    return set;
#else
    return false;
#endif
}

// The 17 CORE words GT4_RACE_MODE's hooks rely on (inv-mode.md 2): applyRaceDisplayMode 0x436640 and its five
// race-session call sites, applyMenuDisplayMode 0x436740, GT4's own 15-second test stub 0x189a04, the readers of
// race_display_mode (Option+0x16b0: the race camera 0x388ab4, the Race Monitor 0x3da478, the licence result
// 0x3e0f98, the Options getter 0x4373f0) and its writers (the Options setter 0x4373c0, the Option constructor
// 0x436598), and the Options' getter call 0x1806b0. CORE is read where loaded (rdram).
static bool gt4RaceModeGuards(const uint8_t *rdram)
{
    auto u32 = [rdram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    };
    return u32(0x436640u) == 0x27bdffe0u && u32(0x43665cu) == 0x8e2316b0u && u32(0x436740u) == 0x27bdfff0u &&
           u32(0x436774u) == 0x8e0216b0u && u32(0x1084c4u) == 0x0c10d990u && u32(0x10befcu) == 0x0c10d990u &&
           u32(0x10caacu) == 0x0c10d990u && u32(0x110478u) == 0x0c10d990u && u32(0x3c4c0cu) == 0x0c10d990u &&
           u32(0x189a04u) == 0x0c10d990u && u32(0x388ab4u) == 0x8c4316b0u && u32(0x3da478u) == 0x8c84a360u &&
           u32(0x3e0f98u) == 0x8c84a360u && u32(0x4373f0u) == 0x8c8316b0u && u32(0x4373c0u) == 0xae7016b0u &&
           u32(0x436598u) == 0xae0016b0u && u32(0x1806b0u) == 0x0c10dcfcu;
}
#endif

static bool Gt4WideFlag(const uint8_t *rdram)
{
    static bool coreOk = false; // checked until CORE is in memory
    if (!coreOk)
    {
        uint32_t w = 0;
        std::memcpy(&w, rdram + 0x4365f4u, 4); // sw v0, -0x7b10(v1)
        coreOk = w == 0xac6284f0u;
        if (!coreOk)
            return false;
    }
    uint32_t v = 0;
    std::memcpy(&v, rdram + 0x6184f0u, 4);
    return v == 1u;
}

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
// GT4_ASPECT's wider shapes, 21:9, 32:9 and 48:9 on one screen (analysis/design/full-m2, track "Wide"; development
// and Full GT4.exe only: the Classic build reads them as 16:9). GT4 keeps its own 16:9 state (the wide flag, its
// 640 x 448 frame, its 16:9 menus). Its race view (the race and its replays, while GT4's race HUD or replay captions
// are drawn) is widened: the race camera (W2: GT4's 16:9 monitor geometry made wider), the race display's 2D canvas
// and the HUD's side elements (dispatchGuestBranch, D6). The presenter shows those pictures at the wider shape and
// everything else (GT4's menus, the pre-race and post-race menus, the results table) at 16:9 with bars at the sides
// (Gt4PictureAspect); the race view is narrowed back to 16:9 for those.
namespace gt4wide
{
    std::atomic<float> shape{0.0f}; // 21/9, 32/9 or 48/9; 0: none (set once)
    // GT4's race HUD is laid out on a 2D canvas 640 x 480 units (its 4:3 layout), 853.3 wide in 16:9
    // (-106.7..746.7: the race display's ortho, 0x39b3e8 -> 0x39bb38); the elements GT4 places at the screen's sides
    // (layout flag 4, flag 8 = right) are moved 96 units out in 16:9 by three helpers (0x3afd18 float x, 0x3afd78
    // integer x, 0x3afdd8 the offset alone). In 21:9 and 32:9 the canvas is 480 x shape units wide and those elements
    // go out by the extra half-width too, so they keep their distance from the screen's edges; in 48:9 (one screen)
    // the canvas is as wide but the HUD keeps its 16:9 places, in the centre section (extra 0).
    float edgeExtra = 0.0f; // canvas units (set once)
    // The EE thread's state.
    bool session = false;  // between applyRaceDisplayMode and applyMenuDisplayMode
    bool wideNow = false;  // the race view is drawn wide now
    float edgeNow = 0.0f;  // the side elements' extra shift in effect
    uint64_t frame = 0;    // race frame updates (0x389638) so far
    uint64_t hudFrame = 0, resultsFrame = 0; // the last frame GT4's HUD helpers / its results table drew (0: none)
    uint64_t pauseFrame = 0; // the last frame GT4's in-race pause menu ran (0x3c3820, vtable 0x681b4c; 0: none)
    uint32_t view = 0;     // the race render object's view matrix (render object + 0x9c0, 0x371f38 copies it there)
    float viewWide = 0.0f, viewNarrow = 0.0f, viewY = 0.0f; // its x scale wide and at 16:9, its y scale
    uint32_t canvas = 0;   // the race display's ortho matrix (+0x70 of the object 0x39bb38 fills)
    // GT4's rear-view mirror: a rectangle in frame pixels (x, y, w, h: 190, 20, 260, 58 in 16:9, centred) at
    // +0xe2e0 of the race render object (0x33eab8's s1); its 3D view and frame are drawn into it. While the race view
    // is wide it is squeezed about the frame's middle by (16:9) / shape, so it keeps its 16:9 look (and in 48:9 its
    // place in the centre section).
    float mirrorX = 0.0f, mirrorW = 0.0f; // the squeezed rectangle last written (0: none)
    uint64_t mirrorWrites = 0;
    PS2Runtime::RecompiledFunction edgeFloat = nullptr, edgeInt = nullptr, edgeOffset = nullptr;
    // The presenter's side: the shape changes a little after the EE's (its pictures are shown a frame or two later).
    std::atomic<bool> presentWide{false}, presentBefore{false};
    std::atomic<uint64_t> presentTick{0};
    int presentDelay = 2; // VSyncs (GT4_WIDE_PRESENT_DELAY, diagnostics)

    float rd(const uint8_t *rdram, uint32_t a)
    {
        float f = 0.0f;
        std::memcpy(&f, rdram + (a & 0x01fffffcu), 4);
        return f;
    }
    void wr(uint8_t *rdram, uint32_t a, float f) { std::memcpy(rdram + (a & 0x01fffffcu), &f, 4); }
    bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); }
    // The canvas half-width (units) and GT4's ortho matrix words [0] = 2 / (r - l), [12] = (l + r) / (l - r).
    float canvasHalf(bool wide) { return wide ? 240.0f * shape.load(std::memory_order_relaxed) : 1280.0f / 3.0f; }
    void canvasWords(bool wide, float &m0, float &m12)
    {
        const float h = canvasHalf(wide), l = 320.0f - h, r = 320.0f + h;
        m0 = 2.0f / (r - l);
        m12 = (l + r) / (l - r);
    }
    // The race view drawn wide or at 16:9 from now on: the view matrix's x scale, the canvas and the side shift; each
    // matrix only while it still holds what was put there (the objects live as long as their race phase).
    void setWide(uint8_t *rdram, bool wide, uint64_t tick, const char *why)
    {
        if (wide && (!view || !canvas))
        {
            // The race view's matrices are not known (another geometry, a CORE that differs): GT4's 16:9 picture.
            wide = false;
            why = "the race view's matrices are not known";
        }
        if (wide == wideNow)
            return;
        wideNow = wide;
        edgeNow = wide ? edgeExtra : 0.0f;
        if (view && near(rd(rdram, view), wide ? viewNarrow : viewWide) && near(rd(rdram, view + 0x14u), viewY))
            wr(rdram, view, wide ? viewWide : viewNarrow);
        else
            view = 0; // (the next race view set-up finds it again)
        if (canvas)
        {
            float o0 = 0.0f, o12 = 0.0f, n0 = 0.0f, n12 = 0.0f;
            canvasWords(!wide, o0, o12);
            canvasWords(wide, n0, n12);
            if (near(rd(rdram, canvas), o0) && near(rd(rdram, canvas + 0x30u), o12))
            {
                wr(rdram, canvas, n0);
                wr(rdram, canvas + 0x30u, n12);
            }
            else
                canvas = 0;
        }
        presentBefore.store(presentWide.load(std::memory_order_relaxed), std::memory_order_relaxed);
        presentTick.store(tick, std::memory_order_relaxed);
        presentWide.store(wide, std::memory_order_relaxed);
        static int said = 0;
        if (said++ < 200)
            std::fprintf(stderr, "[gt4-video] vsync %llu: race view %s (%s)\n", static_cast<unsigned long long>(tick),
                         wide ? "wide" : "16:9", why);
    }
    // GT4's race render (0x33eab8) sets the mirror rectangle every frame (0x33ef78-0x33f044) and then draws the race
    // view (its virtual +0x3c0 from 0x33f0a4, or 0x341d88 from 0x33f080): at those calls, while the race view is
    // wide, the rectangle is squeezed about the frame's middle. Only a rectangle that looks like GT4's (centred on
    // 320, inside the frame, followed by its 1.3) is touched, once per frame (a repeated dispatch finds it squeezed).
    void mirrorAtDraw(uint8_t *rdram, uint32_t r)
    {
        if (!wideNow || r + 0x14u >= 0x02000000u)
            return;
        const float x = rd(rdram, r), y = rd(rdram, r + 4u), w = rd(rdram, r + 8u), h = rd(rdram, r + 12u), a = rd(rdram, r + 16u);
        if (!(w > 8.0f && w < 640.0f && h > 4.0f && h < 448.0f && y >= 0.0f && y < 448.0f && a > 0.5f && a < 3.0f) ||
            std::fabs(x + 0.5f * w - 320.0f) > 1.0f || (mirrorW > 0.0f && w == mirrorW))
            return;
        const float k = (16.0f / 9.0f) / shape.load(std::memory_order_relaxed);
        mirrorX = 320.0f - 0.5f * w * k;
        mirrorW = w * k;
        wr(rdram, r, mirrorX);
        wr(rdram, r + 8u, mirrorW);
        if (mirrorWrites++ == 0u)
            std::fprintf(stderr, "[gt4-video] the rear-view mirror kept in shape: x %.1f width %.1f -> x %.1f width %.1f\n", x, w, mirrorX, mirrorW);
    }
    bool edgeActive(const uint8_t *rdram, uint32_t flags)
    {
        uint32_t wideFlag = 0;
        std::memcpy(&wideFlag, rdram + 0x6184f0u, 4);
        if (session)
            hudFrame = frame;
        return (flags & 4u) != 0u && wideFlag != 0u && session && edgeNow != 0.0f;
    }
    void edgeFloatHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flags = GPR_U32(ctx, 4);
        edgeFloat(rdram, ctx, runtime);
        if (edgeActive(rdram, flags))
            ctx->f[0] += (flags & 8u) ? edgeNow : -edgeNow;
    }
    void edgeIntHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flags = GPR_U32(ctx, 6);
        edgeInt(rdram, ctx, runtime);
        if (edgeActive(rdram, flags))
        {
            const int32_t extra = static_cast<int32_t>(std::lround(edgeNow));
            SET_GPR_S32(ctx, 2, static_cast<int32_t>(GPR_U32(ctx, 2)) + ((flags & 8u) ? extra : -extra));
        }
    }
    void edgeOffsetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flags = GPR_U32(ctx, 4);
        edgeOffset(rdram, ctx, runtime);
        if (edgeActive(rdram, flags))
            ctx->f[0] += (flags & 8u) ? edgeNow : -edgeNow;
    }
}

// Three screens side by side, one wide picture (analysis/design/full-m2, track "Multi"; development and Full GT4.exe
// only: the Classic build has none of it). The first step of MULTI-MONITOR-NOTES.md / TRIPLE-SCREEN.md 3.2 item 7
// ("flat wide"): one GT4, one race camera widened to three screens and the two gaps between their pictures, GT4's own
// flat maths (the same as the wider shapes, gt4wide), the race HUD and the mirror in the middle screen, GT4's menus
// 16:9 on the middle screen. The presenter gives each screen its third of the picture and leaves out what lies
// behind the frames between them, so lines run straight across. Keys (absent or 1: one screen, as before):
//   GT4_SCREENS=3                       three screens across
//   GT4_SCREENS_GEOMETRY=<w>,<h>,<gap>  one screen's picture and the gap between two pictures, in mm (default
//                                       16:9 with no gap); gap 0 = a panorama with nothing left out at the frames
// The window (not --headless): borderless over the middle monitor (GT4_MONITOR, else the one GT4 opens on) and the
// monitors right next to it on the left and right; GT3_FULLSCREEN and GT3_WINDOW_SIZE are then not used.
// Not built: angled side screens (that needs GT4 to draw each screen's own view), 6 screens, the HUD on every screen.
namespace gt4screens
{
    struct Config
    {
        bool on = false;
        float widthMm = 16.0f, heightMm = 9.0f, gapMm = 0.0f;
        float gap = 0.0f;   // the gap in screen widths
        float shape = 0.0f; // the whole picture's shape: (3 w + 2 gap) / h
    };
    const Config &config()
    {
        static const Config c = [] {
            Config k;
            const char *v = std::getenv("GT4_SCREENS");
            std::string s = v ? v : "";
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
            if (s.empty() || s == "1")
                return k;
            if (s != "3")
            {
                std::fprintf(stderr, "[gt4-screens] GT4_SCREENS=%s not understood (1 or 3): one screen\n", v);
                return k;
            }
            if (const char *g = std::getenv("GT4_SCREENS_GEOMETRY"); g && *g)
            {
                float w = 0.0f, h = 0.0f, gap = 0.0f;
                if (std::sscanf(g, "%f,%f,%f", &w, &h, &gap) == 3 && w >= 50.0f && h >= 30.0f && w <= 5000.0f && h <= 5000.0f && w / h >= 1.0f &&
                    w / h <= 2.5f && gap >= 0.0f && gap <= 0.5f * w)
                {
                    k.widthMm = w;
                    k.heightMm = h;
                    k.gapMm = gap;
                }
                else
                    std::fprintf(stderr, "[gt4-screens] GT4_SCREENS_GEOMETRY=%s not understood (<width>,<height>,<gap> in mm): 16:9 screens, no gap\n", g);
            }
            k.on = true;
            k.gap = k.gapMm / k.widthMm;
            k.shape = (3.0f * k.widthMm + 2.0f * k.gapMm) / k.heightMm;
            std::fprintf(stderr, "[gt4-screens] %s: GT4_SCREENS=3: three screens %.1f x %.1f mm (%.3f:1), %.1f mm between pictures (%s): races and replays %.3f:1, "
                                 "the HUD and GT4's menus on the middle screen\n",
                         "GT4 three screens (full-m2)", k.widthMm, k.heightMm, k.widthMm / k.heightMm, k.gapMm,
                         k.gapMm > 0.0f ? "what lies behind the frames is left out" : "a panorama", k.shape);
            return k;
        }();
        return c;
    }
}
#endif

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
// GT4's race cameras (analysis/design/full-m2, track "Cameras"; development and Full GT4.exe only: the Classic build
// has none of it). Keys (absent: GT4's own cameras):
//   GT4_CAM_FAR_CHASE=on|off  a far chase view, 1.5 times GT4's chase distance, added to GT4's view cycle after its
//                             own views (bumper -> chase -> bonnet -> far chase -> bumper; the view button, Select on
//                             GT4's default DualShock 2 layout)
//   GT4_CAM_LOOK=off|360      the 360 look: the right stick turns the view all the way round the car in the chase
//                             views and looks around from the driver's views, springing back when released (on a PC
//                             also the numeric keypad 4 6 2 7 9 and the mouse with its right button held). The right
//                             stick is used only while it does not drive the car: when GT4's Controller Setup for the
//                             DualShock 2 (controller port 1) puts any analog control (Accelerate, Brake ...) on the
//                             right stick, GT4's default, the stick does not turn the view (logged once per race);
//                             with a wheel on port 1 the pad's right stick is free.
// GT4's player camera in a race is CameraSys::CameraOnBoard (vtable 0x67a5d8). Each frame its update (0x37a458, vtable
// +0x54) first runs its input method (0x37ad40, vtable +0x13c, a1 = the race input: +0x19 the view button's step), then
// builds the camera's world -> view matrix at +0x100 (row vectors, translation in the last row; the view looks along
// -z). The view (+0x180) cycles through the table 0x6211e8 {0 bumper, 1 chase, 6 bonnet}: index (i + step) % 12.
// The chase view (0x37b020) places the eye with the car's camera offset o (camera space, stored at +0x40: 0, -1.7,
// -6.0 m for the Ford Ka in the research dump), so the car is at o in camera space; the far chase adds 0.5 x o to the
// matrix's translation (the eye 1.5 times as far back and as high, the car at the same place on screen). GT4's own
// look-left / right / back (its buttons) still work and add to the 360 look. Replays keep GT4's own replay cameras
// (CameraRideOn and the others; only CameraOnBoard objects with a car are touched).
// More views (track "MoreCams", the six views of the GT3 launcher). Keys (absent: as above):
//   GT4_CAM_VIEWS=bumper,bonnet,roof,close,chase,far   the views the view button goes through, in this order (GT4's own
//                             bumper, bonnet and chase as GT4 draws them; roof, close chase and far chase added); wins over
//                             GT4_CAM_FAR_CHASE. Without it: GT4's own cycle (bumper, chase, bonnet), then the far chase
//                             when GT4_CAM_FAR_CHASE=on.
//   GT4_CAM_LOCK=bumper,bonnet,roof | none   the views locked to the car's tilt. GT4's own: bumper and bonnet tilt with
//                             the car's body (0x37b020 / 0x37b580 rotate by its roll, pitch and heading), the chase view
//                             does not (no roll; its heading and pitch lag behind the body, +0x994 / +0x998). A driver's
//                             view (bumper, bonnet, roof) not locked keeps the horizon level (no roll; the chase view's
//                             pitch); a chase view (close, chase, far) locked stays straight behind the car and tilts with
//                             it. Default and absent: GT4's own (bumper, bonnet, roof).
// The roof view is the bonnet view's eye 0.75 m higher and 1.50 m further back, 7 degrees further down (above the car's
// roof, its roof at the bottom of the picture; 0.40 / 0.80 / 3 until 2026-10-20 looked like the bonnet view); the close chase
// is the chase view 0.7 times as far, 0.35 m higher and 4 degrees down (the GT3 launcher's close chase). The view
// button moves GT4's own view (its step byte given as the step from one of GT4's three views to the next one's), so
// GT4 itself switches between bumper, chase and bonnet; a step between two views on the same GT4 view is given as 0.
// The car's body: the car GT4 follows (*(+0x18c) + 0x18), its roll / pitch / heading at +0x8c0 / +0x8bc / +0x8c4 (the
// frame before at +0x670 / +0x66c / +0x674, mixed by +0x554 as 0x344858 and its neighbours do).
namespace gt4cam
{
    constexpr uint32_t kOnBoardVtable = 0x67a5d8u;
    constexpr float kFarScale = 1.5f;
    // The six views, each drawn on one of GT4's views (0 bumper, 6 bonnet, 1 chase).
    enum View
    {
        Bumper,
        Bonnet,
        Roof,
        Close,
        Chase,
        Far,
        ViewCount
    };
    constexpr const char *kViewKeys[ViewCount] = {"bumper", "bonnet", "roof", "close", "chase", "far"};
    constexpr const char *kViewNames[ViewCount] = {"bumper", "bonnet", "roof", "close chase", "chase", "far chase"};
    constexpr uint32_t kBase[ViewCount] = {0u, 6u, 6u, 1u, 1u, 1u};
    constexpr unsigned kGt4Lock = (1u << Bumper) | (1u << Bonnet) | (1u << Roof); // GT4's own tilt
    constexpr float kCloseScale = 0.7f, kCloseRaise = 0.35f, kCloseDown = 4.0f;  // close chase
    // Roof (from the bonnet view). Was 0.40 m up, 0.80 m back, 3 degrees down: on GT4's bonnet view (its eye already
    // about the roof's height) that looked the same as the bonnet view (developer 2026-10-20: "double ups of cameras").
    // GT4_CAM_ROOF=<up>,<back>,<down> (tests only) tries others.
    const std::array<float, 3> kRoof = [] {
        std::array<float, 3> r{0.75f, 1.50f, 7.0f};
        if (const char *v = std::getenv("GT4_CAM_ROOF"); v && *v)
            if (std::sscanf(v, "%f,%f,%f", &r[0], &r[1], &r[2]) != 3)
                r = {0.75f, 1.50f, 7.0f};
        return r;
    }();
    bool farOn = false, lookOn = false;
    std::vector<int> cycle;       // the view button's views in order (empty: GT4's own cycle, untouched)
    unsigned lockBits = kGt4Lock; // the views locked to the car's tilt
    bool lockOn = false;          // lockBits differs from GT4's own (and GT4's body words matched)
    PS2Runtime::RecompiledFunction update = nullptr, input = nullptr;
    const std::atomic<uint64_t> *vsync = nullptr;
    // The player cameras seen (2-player split screen has two): their view.
    struct Cam
    {
        uint32_t addr = 0;
        int view = -1;     // View (-1: not yet seen)
        float wrote[16] = {}; // the matrix written here last (GT4 does not always build it again: then it is left as it is)
        bool wroteValid = false;
        uint64_t seen = 0; // vsync of its last update
        int pending = -1;      // the view the button chose, until GT4 is on its GT4 view (viewOn)
        uint64_t pendingAt = 0; // vsync of that press
    };
    std::array<Cam, 4> cams{};
    int lastView = -1;      // the last view chosen (a race's new camera starts with it when on the same GT4 view, as GT4 keeps its view)
    float angle = 0.0f;     // the look-around now (radians, + to the right; EE thread)
    uint64_t angleTick = 0; // the vsync it was last moved at
    std::atomic<float> hostTarget{0.0f}; // keys / mouse (host thread)
    std::atomic<bool> hostActive{false};
    std::vector<std::pair<uint64_t, float>> testLook; // GT4_CAM_LOOK_TEST=<vsync>:<degrees>,... (tests only)
    int blockedSaid = 0;
    uint64_t blockedTick = 0;
    bool blockedNow = false;
    std::string blockedWhy;
    // GT4_CAM_LOOK_STICK=take (default) | share. take: in a race the 360 look owns the controller's right stick and GT4 no
    // longer sees it (GT4's Controller Setup cannot clear the right stick's Accelerate / Brake, developer 2026-10-09; drive with
    // the triggers, GT4's L2 / R2); share: the old rule, the look is off while GT4's setup puts a control on the right stick.
    const bool takeStick = [] {
        const char *v = std::getenv("GT4_CAM_LOOK_STICK");
        return !(v && std::string(v) == "share");
    }();
    bool tookSaid = false;

    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    }
    float rdf(const uint8_t *rdram, uint32_t a)
    {
        float v = 0.0f;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    }
    void wrf(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & 0x01fffffcu), &v, 4); }
    uint64_t now() { return vsync ? vsync->load(std::memory_order_relaxed) : 0u; }

    Cam &camFor(uint32_t addr)
    {
        Cam *oldest = &cams[0];
        for (Cam &c : cams)
        {
            if (c.addr == addr)
                return c;
            if (c.seen < oldest->seen)
                oldest = &c;
        }
        *oldest = Cam{};
        oldest->addr = addr;
        oldest->seen = now();
        return *oldest;
    }
    // GT4's three views' places in its cycle table 0x6211e8 {0, 1, 6}.
    int gt4Index(uint32_t view) { return view == 0u ? 0 : view == 1u ? 1 : view == 6u ? 2 : -1; }
    // The view a camera shows on GT4's view `gt4`: the view the button chose once GT4 is on its GT4 view (taken then:
    // GT4's input method can finish after inputHook returns), else its own while that is on the same GT4 view, else the
    // last one chosen (a new race), else GT4's own view.
    int viewOn(uint32_t gt4, Cam &c)
    {
        if (c.pending >= 0)
        {
            if (kBase[c.pending] == gt4)
            {
                c.view = lastView = c.pending;
                c.pending = -1;
                return c.view;
            }
            if (now() > c.pendingAt + 30u) // GT4 did not take the view in half a second (not one of this car's views)
                c.pending = -1;
        }
        if (c.view >= 0 && kBase[c.view] == gt4)
            return c.view;
        if (lastView >= 0 && kBase[lastView] == gt4)
            return lastView;
        return gt4 == 0u ? Bumper : gt4 == 6u ? Bonnet : gt4 == 1u ? Chase : -1;
    }
    // Player cameras updated in the last second (more than one: 2-player split screen).
    int liveCams()
    {
        const uint64_t t = now();
        int n = 0;
        for (const Cam &c : cams)
            n += c.addr != 0u && t - c.seen <= 60u;
        return n;
    }

    // Does the pad's right stick drive the car? GT4's Controller Setup for the DualShock 2 on controller port 1 is the
    // OptionRaceControllerPS2 at Option + 0x11ec (Option = *0x622f4c + 0x38cb0; vtable 0x688210 at +0xc, its input
    // names 0x623358 LX LY RX RY LXF LXL LYF LYL RXF RXL RYF RYL and 0x623390, the 12 analog inputs' controls at +0x11:
    // 0 steering, 1 shift down, 2 shift up, 3 accelerate, 4 brake, 5 handbrake, 6 reverse, 0xff none). GT4's default
    // (0x43b808): LX steering, RYF accelerate, RYL brake. A wheel on port 1 (GT4's PS2GT4 slot 1 at 0x654df8, +0x10:
    // a wheel is there) drives the car instead [I: the pad's right stick is then not mapped].
    bool stickDrives(const uint8_t *rdram, std::string &why)
    {
        if (rd32(rdram, 0x654df8u) == 0x689c38u && rd32(rdram, 0x654dfcu) == 0x64c728u && rd32(rdram, 0x654e08u) != 0u)
            return false;
        const uint32_t base = rd32(rdram, 0x622f4cu);
        const uint32_t ds2 = (base + 0x38cb0u + 0x11ecu) & 0x01ffffffu;
        if (base == 0u || ds2 + 0x30u >= 0x02000000u || rd32(rdram, ds2) != 0x623358u || rd32(rdram, ds2 + 4u) != 0x623390u ||
            rd32(rdram, ds2 + 0xcu) != 0x688210u)
        {
            why = "GT4's Controller Setup could not be read";
            return true;
        }
        static const char *const inputs[12] = {"", "", "right stick across", "right stick up / down", "", "", "", "",
                                               "right stick left", "right stick right", "right stick up", "right stick down"};
        static const char *const controls[7] = {"Steering", "Shift Down", "Shift Up", "Accelerate", "Brake", "Handbrake", "Reverse"};
        std::string list;
        for (int i : {2, 3, 8, 9, 10, 11})
        {
            const uint8_t c = rdram[(ds2 + 0x11u + static_cast<uint32_t>(i)) & 0x01ffffffu];
            if (c == 0xffu)
                continue;
            list += (list.empty() ? "" : ", ") + std::string(c < 7u ? controls[c] : "a control") + " on the " + inputs[i];
        }
        if (list.empty())
            return false;
        why = "GT4's Controller Setup (DualShock 2, controller port 1) has " + list;
        return true;
    }

    float wrapPi(float a)
    {
        a = std::remainder(a, 6.2831853f);
        return a;
    }

    // The look-around's target this frame (radians), and whether the right stick may be read.
    float target(const uint8_t *rdram)
    {
        const uint64_t t = now();
        std::string why;
        const bool take = takeStick && liveCams() <= 1;
        ps2_pad::setRightStickClaimed(take);
        if (take && !tookSaid)
        {
            tookSaid = true;
            std::fprintf(stderr, "[gt4-cam] vsync %llu: the 360 look takes the right stick in races (GT4 does not see it there; "
                                 "accelerate and brake on the triggers; GT4_CAM_LOOK_STICK=share gives it back)\n",
                         static_cast<unsigned long long>(t));
        }
        const bool blocked = !take && stickDrives(rdram, why);
        if (blocked && (!blockedNow || t - blockedTick > 600u * 60u) && blockedSaid < 20)
        {
            ++blockedSaid;
            std::fprintf(stderr, "[gt4-cam] vsync %llu: the 360 look's right stick is off: %s (the keypad and mouse still turn the view)\n",
                         static_cast<unsigned long long>(t), why.c_str());
            blockedTick = t;
        }
        if (!blocked && blockedNow)
            std::fprintf(stderr, "[gt4-cam] vsync %llu: the right stick is free: the 360 look takes it\n", static_cast<unsigned long long>(t));
        blockedNow = blocked;
        if (!testLook.empty())
        {
            float deg = 0.0f;
            for (const auto &[from, d] : testLook)
                if (t >= from)
                    deg = d;
            return deg * 3.14159265f / 180.0f;
        }
        if (!blocked && liveCams() <= 1)
        {
            float x = 0.0f, y = 0.0f;
            ps2_pad::rightStick(x, y);
            if (std::sqrt(x * x + y * y) > 0.35f)
                return std::atan2(x, -y); // up: ahead, right: right, down: behind
        }
        if (hostActive.load(std::memory_order_relaxed))
            return hostTarget.load(std::memory_order_relaxed);
        return 0.0f;
    }

    // GT4's input method (0x37ad40): the view button's step is the race input's byte +0x19 (1 on a press); GT4 goes to
    // table[(index + step) % 12] of its cycle table {0 bumper, 1 chase, 6 bonnet}. On a press the next view of `cycle`
    // is chosen and GT4's step is given as the step from GT4's view now to that view's GT4 view (11 = one back in GT4's
    // modulus 12; 0 when both are on the same GT4 view: GT4 then keeps its view), so GT4 changes the view itself; the
    // byte is put back after the call. With the far chase only (GT4_CAM_FAR_CHASE=on) the cycle is bumper -> chase ->
    // bonnet -> far chase.
    void inputHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = GPR_U32(ctx, 4) & 0x01ffffffu, in = GPR_U32(ctx, 5) & 0x01ffffffu;
        if (cycle.empty() || cam + 0x280u >= 0x02000000u || in + 0x20u >= 0x02000000u || rd32(rdram, cam) != kOnBoardVtable ||
            rd32(rdram, cam + 0x18cu) == 0u)
        {
            input(rdram, ctx, runtime);
            return;
        }
        const uint8_t step = rdram[in + 0x19u];
        const uint32_t view = rd32(rdram, cam + 0x180u);
        Cam &c = camFor(cam);
        const bool press = step == 1u && rdram[cam + 0x26au] != 0u && gt4Index(view) >= 0;
        if (!press)
        {
            input(rdram, ctx, runtime);
            return;
        }
        const int cur = viewOn(view, c);
        int pos = -1;
        for (size_t i = 0; i < cycle.size(); ++i)
            if (cycle[i] == cur)
                pos = static_cast<int>(i);
        const int next = cycle[static_cast<size_t>(pos + 1) % cycle.size()];
        rdram[in + 0x19u] = static_cast<uint8_t>((gt4Index(kBase[next]) - gt4Index(view) + 12) % 12);
        input(rdram, ctx, runtime);
        rdram[in + 0x19u] = step;
        // GT4's input method may not have finished here: a call in it (0x3481c8, 0x37bc20) can go back to the dispatcher
        // (dispatchGuestBranch) and the rest of it run later in the same frame, GT4's view (+0x180) changed only then
        // (cam-cycle 2026-10-20: 1 to 2 presses in 16 read the old view here; roof -> close chase then came up as GT4's own
        // chase view). So the chosen view waits as pending until GT4 is on its GT4 view (viewOn).
        c.pending = next;
        c.pendingAt = now();
        const uint32_t after = rd32(rdram, cam + 0x180u);
        const int shown = viewOn(after, c);
        static int said = 0;
        if (said++ < 60)
            std::fprintf(stderr, "[gt4-cam] vsync %llu: view %u -> %u (%s)%s\n", static_cast<unsigned long long>(now()), view, after,
                         kViewNames[next], shown == next ? "" : " (GT4 switches later this frame)");
    }

    // ---- The car's tilt (GT4_CAM_LOCK). GT4 builds its views on a matrix stack, each operation applied to the world
    // first (M = op x M): the chase view (0x37b020) T(-car) x rotY(-(heading - lag)) x rotX(-(pitch - lag)) x rotZ(0) x
    // T(offset) x small swings; the bumper view the same with rotZ(roll) and its heading and pitch without the lag (and the
    // car's own bumper pitch). The rotations' signs and order are taken from GT4's own matrices while it draws (calibrate):
    // the chase view gives the heading's and pitch's, the bumper view the roll's.
    struct Conv
    {
        int order = 0;     // 0: rotY x rotX x rotZ (GT4's call order), 1: the other way round
        float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    } conv;
    double fitChase[8] = {}, fitRoll[2] = {};
    int fitChaseN = 0, fitRollN = 0;
    bool chaseFitted = false, rollFitted = false;

    // A row-vector 3x3 (p' = p R) for GT4's rotation arguments (radians) x, y, z.
    void rot3(float out[9], float x, float y, float z, const Conv &k)
    {
        auto mul = [](const float a[9], const float b[9], float r[9]) {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        };
        const float cx = std::cos(k.sx * x), sx = std::sin(k.sx * x), cy = std::cos(k.sy * y), sy = std::sin(k.sy * y), cz = std::cos(k.sz * z),
                    sz = std::sin(k.sz * z);
        const float rx[9] = {1, 0, 0, 0, cx, sx, 0, -sx, cx};
        const float ry[9] = {cy, 0, -sy, 0, 1, 0, sy, 0, cy};
        const float rz[9] = {cz, sz, 0, -sz, cz, 0, 0, 0, 1};
        float t[9];
        if (k.order == 0)
        {
            mul(ry, rx, t);
            mul(t, rz, out);
        }
        else
        {
            mul(rz, rx, t);
            mul(t, ry, out);
        }
    }
    // The car's body this frame, as GT4's view builders read it.
    struct Body
    {
        float pitch = 0, roll = 0, heading = 0, lagHeading = 0, lagPitch = 0;
        float car[3] = {}; // the car's point (world), -(+0x28..+0x30) as GT4 stores it
    };
    bool body(const uint8_t *rdram, uint32_t cam, Body &b)
    {
        const uint32_t race = rd32(rdram, cam + 0x18cu) & 0x01ffffffu;
        if (race == 0u || race + 0x20u >= 0x02000000u)
            return false;
        const uint32_t car = rd32(rdram, race + 0x18u) & 0x01ffffffu;
        if (car == 0u || car + 0xa00u >= 0x02000000u)
            return false;
        const float t = rdf(rdram, car + 0x554u);
        auto mix = [&](uint32_t cur, uint32_t prev) { return rdf(rdram, car + cur) * t + rdf(rdram, car + prev) * (1.0f - t); };
        b.pitch = mix(0x8bcu, 0x66cu);
        b.roll = mix(0x8c0u, 0x670u);
        b.heading = mix(0x8c4u, 0x674u);
        b.lagHeading = mix(0x994u, 0x700u);
        b.lagPitch = mix(0x998u, 0x704u);
        for (int i = 0; i < 3; ++i)
            b.car[i] = -rdf(rdram, cam + 0x28u + 4u * static_cast<uint32_t>(i));
        const float all[] = {t, b.pitch, b.roll, b.heading, b.lagHeading, b.lagPitch, b.car[0], b.car[1], b.car[2]};
        for (float v : all)
            if (!std::isfinite(v) || std::fabs(v) > 1.0e6f)
                return false;
        return t >= -0.01f && t <= 1.01f;
    }
    double fit(const float m[16], const float r[9])
    {
        double e = 0.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                const double d = static_cast<double>(m[i * 4 + j]) - r[i * 3 + j];
                e += d * d;
            }
        return e;
    }
    // GT4's own matrix (before any change here) against the candidate rotations.
    void calibrate(const float m[16], uint32_t gt4View, const Body &b)
    {
        if (gt4View == 1u && !chaseFitted)
        {
            for (int c = 0; c < 8; ++c)
            {
                const Conv k{c >> 2, (c & 1) ? -1.0f : 1.0f, (c & 2) ? -1.0f : 1.0f, 1.0f};
                float r[9];
                rot3(r, -(b.pitch - b.lagPitch), -(b.heading - b.lagHeading), 0.0f, k);
                fitChase[c] += fit(m, r);
            }
            if (++fitChaseN >= 120)
            {
                int best = 0;
                for (int c = 1; c < 8; ++c)
                    if (fitChase[c] < fitChase[best])
                        best = c;
                conv.order = best >> 2;
                conv.sx = (best & 1) ? -1.0f : 1.0f;
                conv.sy = (best & 2) ? -1.0f : 1.0f;
                chaseFitted = true;
                std::fprintf(stderr, "[gt4-cam] tilt: GT4's chase view fitted (%d frames): order %d, pitch sign %+.0f, heading sign %+.0f; mean error %.5f (next best %.5f)\n",
                             fitChaseN, conv.order, conv.sx, conv.sy, fitChase[best] / fitChaseN, [&] {
                                 double second = 1e30;
                                 for (int c = 0; c < 8; ++c)
                                     if (c != best)
                                         second = std::min(second, fitChase[c]);
                                 return second / fitChaseN;
                             }());
            }
        }
        if (gt4View == 0u && !rollFitted && std::fabs(b.roll) > 0.003f)
        {
            for (int s = 0; s < 2; ++s)
            {
                Conv k = conv;
                k.sz = s ? -1.0f : 1.0f;
                float r[9];
                rot3(r, -b.pitch, -b.heading, b.roll, k);
                fitRoll[s] += fit(m, r);
            }
            if (++fitRollN >= 120)
            {
                conv.sz = fitRoll[1] < fitRoll[0] ? -1.0f : 1.0f;
                rollFitted = true;
                std::fprintf(stderr, "[gt4-cam] tilt: GT4's bumper view fitted (%d frames): roll sign %+.0f; mean error %.5f against %.5f\n", fitRollN, conv.sz,
                             std::min(fitRoll[0], fitRoll[1]) / fitRollN, std::max(fitRoll[0], fitRoll[1]) / fitRollN);
            }
        }
    }
    // M = X M with X the world turned by q about the point p (row vectors: p' = (p - c) q + c).
    void turnWorld(float m[16], const float q[9], const float c[3])
    {
        float t[3];
        for (int j = 0; j < 3; ++j)
            t[j] = c[j] - (c[0] * q[j] + c[1] * q[3 + j] + c[2] * q[6 + j]);
        float out[16];
        for (int j = 0; j < 4; ++j)
        {
            for (int i = 0; i < 3; ++i)
                out[i * 4 + j] = q[i * 3] * m[j] + q[i * 3 + 1] * m[4 + j] + q[i * 3 + 2] * m[8 + j];
            out[12 + j] = t[0] * m[j] + t[1] * m[4 + j] + t[2] * m[8 + j] + m[12 + j];
        }
        std::memcpy(m, out, sizeof(out));
    }
    // a x b^T (3x3).
    void mulT(const float a[9], const float b[9], float r[9])
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i * 3 + j] = a[i * 3] * b[j * 3] + a[i * 3 + 1] * b[j * 3 + 1] + a[i * 3 + 2] * b[j * 3 + 2];
    }
    // The tilt the player chose where it differs from GT4's own: a chase view locked (the body's heading, pitch and roll
    // in place of the chase view's lagging ones, turned about the car), a driver's view level (no roll, the chase view's
    // pitch, the body's heading, turned about the eye).
    bool retilt(float m[16], uint32_t gt4View, const Body &b)
    {
        float from[9], to[9], q[9];
        if (gt4View == 1u)
        {
            rot3(from, -(b.pitch - b.lagPitch), -(b.heading - b.lagHeading), 0.0f, conv);
            rot3(to, -b.pitch, -b.heading, b.roll, conv);
            mulT(to, from, q);
            turnWorld(m, q, b.car);
            return true;
        }
        float eye[3];
        for (int j = 0; j < 3; ++j) // e = -T R^T
            eye[j] = -(m[12] * m[j * 4] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
        rot3(from, -b.pitch, -b.heading, b.roll, conv);
        rot3(to, -(b.pitch - b.lagPitch), -b.heading, 0.0f, conv);
        mulT(to, from, q);
        turnWorld(m, q, eye);
        return true;
    }
    // The eye moved in the view's own axes (up +y, back +z) and the view tipped down by `down` degrees about the eye.
    void moveEye(float m[16], float up, float back, float down)
    {
        m[13] -= up;
        m[14] -= back;
        const float a = down * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        for (int r = 0; r < 4; ++r)
        {
            const float y = m[r * 4 + 1], z = m[r * 4 + 2];
            m[r * 4 + 1] = y * c - z * s;
            m[r * 4 + 2] = y * s + z * c;
        }
    }

    // A row-vector 4x4 times the turn about the camera's own up axis by a (radians, + to the right): p' = p R.
    void turn(float m[16], float a, const float pivot[3])
    {
        const float c = std::cos(a), s = std::sin(a);
        for (int r = 0; r < 4; ++r)
        {
            float x = m[r * 4 + 0], z = m[r * 4 + 2];
            if (r == 3)
            {
                x -= pivot[0];
                z -= pivot[2];
            }
            float nx = x * c + z * s, nz = -x * s + z * c;
            if (r == 3)
            {
                nx += pivot[0];
                nz += pivot[2];
            }
            m[r * 4 + 0] = nx;
            m[r * 4 + 2] = nz;
        }
    }

    // GT4's camera update (0x37a458) stores its matrix at +0x100 (0x37ac70), then gives back the VU0 lock (0x4a06f0 from
    // 0x37ac80: SignalSema). That signal can switch at once to a GT4 thread that was waiting for the lock, which may draw
    // with +0x100 as it is: changed after the update returned, an occasional frame showed GT4's own view (cam-switch-fix:
    // the close chase, far chase and roof views popped to GT4's chase / bonnet view for one frame). So the far / close
    // chase, the roof, the tilt and the 360 look are put in at that call, before the lock is given back (dispatchGuestBranch).
    bool releaseHooked = false; // the call at 0x37ac80 is hooked (else: after the update, as before)
    void edit(uint8_t *rdram, uint32_t cam);
    void updateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cam = GPR_U32(ctx, 4) & 0x01ffffffu;
        update(rdram, ctx, runtime);
        // GT4's rear-view mirror in every view of the player's camera (GT4_MIRROR=both / page, gt4_mirror.h; the test switch
        // GT4_MIRROR_ALL=1, HUD hand-off 6 B1). GT4's input method sets the mirror's byte +0x26d only in the bumper view; set
        // here in the others too, unless the player looks back or aside (+0x26b, +0x269), as GT4 does in the bumper view.
        static const bool mirrorAll = [] {
            const char *v = std::getenv("GT4_MIRROR_ALL");
            return v && *v == '1';
        }();
        // (GT4_MIRROR=screen: only while a page watches the mirror; the views GT4 draws none in are then drawn for the page
        // only and kept off the game screen, gt4mirror::setCamera)
        // GT4's own: the bumper view (+0x180 view 0). A byte set here and left set is cleared once the page stops watching.
        static bool forcedLast = false;
        if (cam + 0x280u < 0x02000000u && rd32(rdram, cam) == kOnBoardVtable && rd32(rdram, cam + 0x18cu) != 0u)
        {
            const bool bumper = rd32(rdram, cam + 0x180u) == 0u;
            const bool force = (mirrorAll || gt4mirror::forcing()) && !bumper && rdram[cam + 0x26bu] == 0u && rdram[cam + 0x269u] == 0u;
            if (force)
                rdram[cam + 0x26du] = 1u;
            else if (forcedLast && !bumper)
                rdram[cam + 0x26du] = 0u;
            forcedLast = force;
            gt4mirror::setCamera(mirrorAll ? 0u : cam);
        }
        if (!releaseHooked)
            edit(rdram, cam);
    }
    void edit(uint8_t *rdram, uint32_t cam)
    {
        cam &= 0x01ffffffu;
        if (cam + 0x280u >= 0x02000000u || rd32(rdram, cam) != kOnBoardVtable || rd32(rdram, cam + 0x18cu) == 0u)
            return;
        Cam &c = camFor(cam);
        c.seen = now();
        const uint32_t view = rd32(rdram, cam + 0x180u);
        const int v = viewOn(view, c);
        if (v >= 0)
            c.view = v;
#if !defined(GT4_STOCK_BUILD)
        gt4hudpic::noteView(c.view); // (the launcher's race pictures: the view they were taken in)
#endif
        float m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = rdf(rdram, cam + 0x100u + 4u * static_cast<uint32_t>(i));
        if (!std::isfinite(m[0]) || !std::isfinite(m[12]) || m[15] != 1.0f)
            return;
        // GT4's update did not build the matrix again (it still holds what was written here): changing it again would add
        // the far chase's distance, the tilt and the look a second time.
        if (c.wroteValid && std::memcmp(m, c.wrote, sizeof(m)) == 0)
        {
            static uint64_t kept = 0;
            if (++kept <= 3 || kept % 1000u == 0u)
                std::fprintf(stderr, "[gt4-cam] vsync %llu: GT4 kept its view's matrix this update (%llu so far): left as it is\n",
                             static_cast<unsigned long long>(now()), static_cast<unsigned long long>(kept));
            return;
        }
        bool changed = false;
        // The car's tilt where the player's choice differs from GT4's own (not while GT4 looks back or to the side).
        if (lockOn && v >= 0 && rdram[cam + 0x26bu] == 0u && rdram[cam + 0x269u] == 0u)
        {
            Body b;
            if (body(rdram, cam, b))
            {
                calibrate(m, view, b);
                const bool gt4Locked = view != 1u, want = ((lockBits >> v) & 1u) != 0u;
                if (want != gt4Locked)
                    changed |= retilt(m, view, b);
            }
        }
        if ((v == Far || v == Close) && view == 1u)
        {
            const float o[3] = {rdf(rdram, cam + 0x40u), rdf(rdram, cam + 0x44u), rdf(rdram, cam + 0x48u)};
            const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len > 0.5f && len < 40.0f)
            {
                const float s = v == Far ? kFarScale : kCloseScale;
                for (int i = 0; i < 3; ++i)
                    m[12 + i] += (s - 1.0f) * o[i];
                if (v == Close)
                    moveEye(m, kCloseRaise, 0.0f, kCloseDown);
                changed = true;
            }
        }
        if (v == Roof && view == 6u)
        {
            moveEye(m, kRoof[0], kRoof[1], kRoof[2]);
            changed = true;
        }
        if (lookOn)
        {
            // Once a frame (a second player camera would turn with the same look: 2-player split screen keeps none).
            const uint64_t t = now();
            if (t != angleTick)
            {
                angleTick = t;
                const float want = liveCams() > 1 && testLook.empty() ? 0.0f : target(rdram);
                angle += wrapPi(want - angle) * 0.25f;
                angle = wrapPi(angle);
                if (std::fabs(want) < 1e-6f && std::fabs(angle) < 0.003f)
                    angle = 0.0f;
            }
            if (angle != 0.0f)
            {
                // The chase views turn round the car (its place in camera space, c x M with c = -(+0x28..+0x30), the car
                // GT4 stores there), the driver's views (bumper, bonnet) about the eye.
                float pivot[3] = {0.0f, 0.0f, 0.0f};
                if (view == 1u)
                {
                    const float p[3] = {-rdf(rdram, cam + 0x28u), -rdf(rdram, cam + 0x2cu), -rdf(rdram, cam + 0x30u)};
                    for (int j = 0; j < 3; ++j)
                        pivot[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[12 + j];
                }
                turn(m, angle, pivot);
                changed = true;
            }
        }
        if (changed)
            for (int i = 0; i < 16; ++i)
                wrf(rdram, cam + 0x100u + 4u * static_cast<uint32_t>(i), m[i]);
        c.wroteValid = changed;
        if (changed)
            std::memcpy(c.wrote, m, sizeof(m));
        static uint64_t lastLog = 0;
        // GT4_CAM_LOG=1: a line every 300 VSyncs; GT4_CAM_LOG=<n> (n > 1): every n VSyncs (tests only).
        static const uint64_t logEvery = [] {
            const char *v = std::getenv("GT4_CAM_LOG");
            const long n = v ? std::strtol(v, nullptr, 10) : 0;
            return n == 1 ? 300u : n > 1 ? static_cast<uint64_t>(n) : 0u;
        }();
        if (logEvery && now() >= lastLog + logEvery)
        {
            lastLog = now();
            // The car's point in the view's axes (the eye at 0, looking along -z) and the view's up axis (world).
            const float p[3] = {-rdf(rdram, cam + 0x28u), -rdf(rdram, cam + 0x2cu), -rdf(rdram, cam + 0x30u)};
            float pv[3];
            for (int j = 0; j < 3; ++j)
                pv[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
            Body b;
            const bool hasBody = body(rdram, cam, b);
            // The view's roll against the world's horizontal (its right axis' height, y up) in degrees.
            const float viewRoll = std::asin(std::clamp(m[4], -1.0f, 1.0f)) * 57.2957795f;
            std::fprintf(stderr, "[gt4-cam] vsync %llu: camera %x view %u (%s) look %.1f deg; car %.2f %.2f %.2f, in view %.2f %.2f %.2f; up %.3f %.3f %.3f; "
                                 "view roll %.2f deg; body pitch %.4f roll %.4f heading %.4f lag %.4f %.4f%s%s\n",
                         static_cast<unsigned long long>(now()), cam, view, v >= 0 ? kViewNames[v] : "?", angle * 57.2957795f, p[0], p[1], p[2], pv[0], pv[1],
                         pv[2], m[1], m[5], m[9], viewRoll, b.pitch, b.roll, b.heading, b.lagPitch, b.lagHeading, hasBody ? "" : " (no body)",
                         v >= 0 && ((lockBits >> v) & 1u) ? " [locked]" : " [level]");
        }
    }

    // The host thread, each shown frame: the numeric keypad (9 / 7 half right / left, 6 / 4 right / left, 2 behind)
    // and the mouse with its right button held (drag across to turn); both spring back when let go.
    void hostUpdate()
    {
        if (!lookOn)
            return;
        // Out of a race (no player camera this last second) GT4 has the right stick again.
        if (takeStick && liveCams() == 0)
            ps2_pad::setRightStickClaimed(false);
        static float mouseAngle = 0.0f;
        float t = 0.0f;
        bool active = true;
        if (IsKeyDown(KEY_KP_2))
            t = 3.14159265f;
        else if (IsKeyDown(KEY_KP_6))
            t = 1.5707963f;
        else if (IsKeyDown(KEY_KP_4))
            t = -1.5707963f;
        else if (IsKeyDown(KEY_KP_9))
            t = 0.7853982f;
        else if (IsKeyDown(KEY_KP_7))
            t = -0.7853982f;
        else if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
        {
            mouseAngle = std::clamp(mouseAngle + GetMouseDelta().x * 0.006f, -3.14159265f, 3.14159265f);
            t = mouseAngle;
        }
        else
            active = false;
        if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            mouseAngle = 0.0f;
        hostTarget.store(t, std::memory_order_relaxed);
        hostActive.store(active, std::memory_order_relaxed);
    }
}

// GT4's car and wheel detail (analysis/design/full-m2, track "Detail"; development and Full GT4.exe only: the Classic
// build has none of it). Keys (absent or game: GT4's own detail):
//   GT4_CAR_DETAIL=game|2x|4x|full    the cars' bodies: GT4's own switching, full detail twice / four times as far,
//                                     always full detail
//   GT4_WHEEL_DETAIL=game|2x|4x|full  the cars' wheels (rims and tyres): the same choices
//   GT4_WHEEL_BLUR=on|off             GT4's spinning-wheel blur (its blurred rim at speed); off: the sharp rim always
// GT4's model interpreter (0x455708) picks a model's level at its level op (0x455d78): the size on screen s =
// *0x8465e4 x radius / depth (0x454f40; 0 while GT4 has the switching off, *0x6238b4 = 0), then the model's callback
// (vtable +0x18, called through its vtable) turns s into a level (0 = most detailed):
//   a car's body, CarModel::Callback (vtable 0x688628), 0x4539a8: the car's forced level (CarModel +0x8c, -1 none), else
//     0 when s > t, 1 when s > 0.33 t, else 2 (t = CarModel +0x90, 220 by default);
//   a car's wheels, CarModel::WheelCallback (vtable 0x6885f0), 0x4540a8: the car's forced level, else 0x454fb0: 0 when
//     s > T, 1 when s > T / 2, 2 when s > T / 4, else 3 (T = *0x6238a4, 200).
// s falls with the distance, so 2x / 4x give the pick s x 2 / x 4 (each level kept to that many times the distance) and
// full a size above every threshold (level 0). A forced level and s = 0 stay GT4's. The wheel's texture: WheelCallback's
// +0x14 method 0x453e68 draws the next texture (the blurred rim) when its blur mode (+8) is 1 and the wheel turns faster
// than 0.0873 rad a frame; GT4_WHEEL_BLUR=off runs it with the mode 0 (the sharp rim, as at low speed) and puts 1 back.
// Nothing here changes GT4's data or the save.
namespace gt4detail
{
    constexpr uint32_t kBodyVtable = 0x688628u, kWheelVtable = 0x6885f0u;
    PS2Runtime::RecompiledFunction bodyPick = nullptr, wheelPick = nullptr, wheelTexture = nullptr;
    int detail[2] = {0, 0}; // [0] body, [1] wheels: 0 game, 1 2x, 2 4x, 3 full (EE thread)
    bool blurOff = false;
    bool log = false;
    const std::atomic<uint64_t> *vsync = nullptr;
    struct TestStep
    {
        uint64_t at = 0;
        int car = 0, wheel = 0;
        bool blurOff = false;
    };
    std::vector<TestStep> test; // GT4_DETAIL_TEST=<vsync>:<car>:<wheel>:<blur on|off>,... (tests only)
    size_t testNext = 0;
    uint64_t levels[2][2][4] = {}; // [body, wheels][GT4's level, drawn][level]
    uint64_t forced[2] = {}, sizeZero[2] = {}, blurMode[2] = {}; // blurMode: wheel texture calls with GT4's blur mode 1 [as is, run sharp]

    const char *name(int d) { return d == 1 ? "2x" : d == 2 ? "4x" : d == 3 ? "full" : "game"; }
    // game | 2x | 4x | full (also x2, x4, gt4, 0..3); -1: not understood.
    int parse(const std::string &v)
    {
        if (v.empty() || v == "game" || v == "gt4" || v == "0")
            return 0;
        if (v == "2x" || v == "x2" || v == "1")
            return 1;
        if (v == "4x" || v == "x4" || v == "2")
            return 2;
        if (v == "full" || v == "3")
            return 3;
        return -1;
    }
    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    }
    float rdf(const uint8_t *rdram, uint32_t a)
    {
        float v = 0.0f;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    }
    uint64_t now() { return vsync ? vsync->load(std::memory_order_relaxed) : 0u; }

    // Every 600 VSyncs, and before a test step changes the choices (force), with GT4_DETAIL_LOG=1.
    void report(bool force = false)
    {
        static uint64_t last = 0;
        if (!log || (!force && now() < last + 600u))
            return;
        last = now();
        const auto &l = levels;
        std::fprintf(stderr,
                     "[gt4-detail] vsync %llu: car %s wheel %s blur %s; body levels 0/1/2 GT4 %llu/%llu/%llu drawn %llu/%llu/%llu (forced %llu, "
                     "switching off %llu); wheel levels 0/1/2/3 GT4 %llu/%llu/%llu/%llu drawn %llu/%llu/%llu/%llu (forced %llu, switching off %llu); "
                     "wheel textures with GT4's blur %llu, run sharp %llu\n",
                     static_cast<unsigned long long>(now()), name(detail[0]), name(detail[1]), blurOff ? "off" : "on",
                     static_cast<unsigned long long>(l[0][0][0]), static_cast<unsigned long long>(l[0][0][1]), static_cast<unsigned long long>(l[0][0][2]),
                     static_cast<unsigned long long>(l[0][1][0]), static_cast<unsigned long long>(l[0][1][1]), static_cast<unsigned long long>(l[0][1][2]),
                     static_cast<unsigned long long>(forced[0]), static_cast<unsigned long long>(sizeZero[0]),
                     static_cast<unsigned long long>(l[1][0][0]), static_cast<unsigned long long>(l[1][0][1]), static_cast<unsigned long long>(l[1][0][2]),
                     static_cast<unsigned long long>(l[1][0][3]), static_cast<unsigned long long>(l[1][1][0]), static_cast<unsigned long long>(l[1][1][1]),
                     static_cast<unsigned long long>(l[1][1][2]), static_cast<unsigned long long>(l[1][1][3]), static_cast<unsigned long long>(forced[1]),
                     static_cast<unsigned long long>(sizeZero[1]), static_cast<unsigned long long>(blurMode[0]), static_cast<unsigned long long>(blurMode[1]));
        std::memset(levels, 0, sizeof(levels));
        forced[0] = forced[1] = sizeZero[0] = sizeZero[1] = blurMode[0] = blurMode[1] = 0;
    }
    void testStep()
    {
        while (testNext < test.size() && now() >= test[testNext].at)
        {
            const TestStep &s = test[testNext++];
            report(true);
            detail[0] = s.car;
            detail[1] = s.wheel;
            blurOff = s.blurOff;
            std::fprintf(stderr, "[gt4-detail] vsync %llu: test switch: car %s wheel %s blur %s\n", static_cast<unsigned long long>(now()), name(s.car),
                         name(s.wheel), s.blurOff ? "off" : "on");
        }
    }

    // which: 0 the body (0x4539a8), 1 the wheels (0x4540a8). a0 = the callback (its +4: the CarModel), f12 = the size.
    void pick(int which, PS2Runtime::RecompiledFunction gt4, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        testStep();
        const uint32_t cb = GPR_U32(ctx, 4) & 0x01ffffffu;
        const uint32_t car = cb + 8u < 0x02000000u ? rd32(rdram, cb + 4u) & 0x01ffffffu : 0u;
        const bool valid = car != 0u && car + 0x94u < 0x02000000u;
        const bool isForced = valid && static_cast<int32_t>(rd32(rdram, car + 0x8cu)) >= 0;
        const float size = ctx->f[12];
        const int d = detail[which];
        const bool change = d != 0 && valid && !isForced && size > 0.0f;
        if (change)
            ctx->f[12] = d >= 3 ? 1.0e30f : size * (d == 2 ? 4.0f : 2.0f);
        gt4(rdram, ctx, runtime);
        ctx->f[12] = size;
        if (!log)
            return;
        if (isForced)
            ++forced[which];
        else if (!(size > 0.0f))
            ++sizeZero[which];
        else if (valid)
        {
            // GT4's own level for this size (the same comparisons as 0x4539a8 / 0x454fb0), then the level drawn.
            int own = 0;
            if (which == 0)
            {
                const float t = rdf(rdram, car + 0x90u);
                own = t < size ? 0 : (t * 0.33f < size ? 1 : 2);
            }
            else
            {
                const float t = rdf(rdram, 0x6238a4u);
                own = t < size ? 0 : (t * 0.5f < size ? 1 : (t * 0.25f < size ? 2 : 3));
            }
            ++levels[which][0][std::clamp(own, 0, 3)];
            ++levels[which][1][std::clamp(static_cast<int>(GPR_U32(ctx, 2)), 0, 3)];
        }
        report();
    }
    void bodyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { pick(0, bodyPick, rdram, ctx, runtime); }
    void wheelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { pick(1, wheelPick, rdram, ctx, runtime); }
    void textureHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        testStep();
        const uint32_t cb = GPR_U32(ctx, 4) & 0x01ffffffu;
        const bool blurMode1 = cb + 0x24u < 0x02000000u && rd32(rdram, cb) == kWheelVtable && rd32(rdram, cb + 8u) == 1u;
        if (!blurOff || !blurMode1)
        {
            blurMode[0] += blurMode1 ? 1u : 0u;
            wheelTexture(rdram, ctx, runtime);
            return;
        }
        ++blurMode[1];
        const uint32_t sharp = 0u, mode = 1u;
        std::memcpy(rdram + cb + 8u, &sharp, 4);
        wheelTexture(rdram, ctx, runtime);
        std::memcpy(rdram + cb + 8u, &mode, 4);
    }
}
#endif

// The shape the shown GT4 picture is drawn for: 0 for 4:3 (or the frame's own shape with square pixels), 16:9 with
// GT4's wide flag set, or the wider shape while GT4 draws its widened race view (tick: the presenter's VSync).
static float Gt4PictureAspect(const uint8_t *rdram, uint64_t tick);
// GT4's 1080i races are shown at their own 1.8:1 (VIDEO-MODES.md 3.1); while the race view is drawn wider
// (GT4_ASPECT=21:9 / 32:9 / 48:9) that shape is widened by the same shape / (16:9) (full-m2 Wide review: without it a
// 1080i race was shown squeezed).
static float Gt4Picture1080Aspect(const uint8_t *rdram, uint64_t tick, float own)
{
    const float shape = Gt4PictureAspect(rdram, tick);
    return shape > 16.0f / 9.0f + 1e-3f ? own * shape * (9.0f / 16.0f) : own;
}
static float Gt4PictureAspect(const uint8_t *rdram, uint64_t tick)
{
    if (!Gt4WideFlag(rdram))
        return 0.0f;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    const float shape = gt4wide::shape.load(std::memory_order_relaxed);
    if (shape > 0.0f)
    {
        const uint64_t at = gt4wide::presentTick.load(std::memory_order_relaxed);
        const bool wide = tick >= at + static_cast<uint64_t>(gt4wide::presentDelay) ? gt4wide::presentWide.load(std::memory_order_relaxed)
                                                                                     : gt4wide::presentBefore.load(std::memory_order_relaxed);
        if (wide)
            return shape;
    }
#else
    (void)tick;
#endif
    return 16.0f / 9.0f;
}
#endif

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    // GT3_PACING_LOG=1 (diagnostic): every 5 s, how the host's presents met
    // GT3's frames - a new one (its VSync / published frame advanced by one),
    // the same one again (a repeat: judder), or more than one advanced (a
    // skipped frame) - and the host frame interval's range.
    static const bool pacingLog = [] {
        const char *v = std::getenv("GT3_PACING_LOG");
        return v && *v == '1';
    }();
    const uint64_t pacingSource = rt->streamPresentation() ? rt->gs().publishedFrameCount() : currentTick;
    if (pacingLog)
    {
        static uint64_t last = ~0ull, presents = 0, fresh = 0, repeats = 0, skips = 0;
        static double minMs = 1e9, maxMs = 0.0;
        static auto lastPresent = std::chrono::steady_clock::now(), windowStart = lastPresent;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - lastPresent).count();
        lastPresent = now;
        // A host frame this long: the controller was not read meanwhile, so
        // the game kept seeing its last state (a released button still held).
        if (last != ~0ull && ms > 40.0)
            std::fprintf(stderr, "[pacing] host frame %.0f ms at vsync %llu\n", ms, static_cast<unsigned long long>(currentTick));
        if (last != ~0ull)
        {
            ++presents;
            const uint64_t delta = pacingSource - last;
            fresh += delta == 1u;
            repeats += delta == 0u;
            skips += delta > 1u ? delta - 1u : 0u;
            minMs = std::min(minMs, ms);
            maxMs = std::max(maxMs, ms);
        }
        last = pacingSource;
        if (std::chrono::duration<double>(now - windowStart).count() >= 5.0)
        {
            std::fprintf(stderr, "[pacing] %llu presents: %llu new, %llu repeated, %llu skipped; host frame %.1f-%.1f ms (vsync %llu)\n",
                         static_cast<unsigned long long>(presents), static_cast<unsigned long long>(fresh), static_cast<unsigned long long>(repeats),
                         static_cast<unsigned long long>(skips), minMs, maxMs, static_cast<unsigned long long>(currentTick));
            presents = fresh = repeats = skips = 0;
            minMs = 1e9;
            maxMs = 0.0;
            windowStart = now;
        }
    }
#if defined(PS2X_GS_VK)
    // The Vulkan renderer's window presenter: the newest frame stays in the
    // renderer's image (no copy); the host loop draws it through the presenter.
    if (gsvk::present::active())
    {
        gsvk::present::Frame vf;
        const int got = gsvk::present::takeLatest(g_presentMaxTick, vf);
        if (got < 0)
        {
            outWidth = FB_WIDTH;
            outHeight = DEFAULT_DISPLAY_HEIGHT;
            return; // no frame yet: the window stays black
        }
        if (got == 1)
        {
            g_shownFieldParity = vf.parity;
            g_shownCrt = vf.crt;
            g_shownLines = vf.lines;
            g_shownInterlaced = vf.parity >= 0; // fields woven / bobbed (not 480p, not a whole frame)
            ++g_frameUploads;
        }
        g_vkFrame = vf;
        s_lastWidth = outWidth = vf.width;
        s_lastHeight = outHeight = vf.height;
        s_hasUploadedFrame = true;
        return;
    }
#endif
    bool needsLatch;
    if (rt->streamPresentation())
    {
        // The GS worker latched it at the VSync (in GS stream order): show the
        // newest published frame.
        static uint64_t s_lastPublished = ~0ull;
        const uint64_t published = pacingSource;
        needsLatch = published != s_lastPublished;
        s_lastPublished = published;
    }
    else
    {
        needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
        if (needsLatch)
            rt->gs().latchHostPresentationFrame();
    }
    if (needsLatch)
    {
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

#if defined(PS2X_GS_GL)
    // Frames the OpenGL renderer composed on the GPU: copied GPU to GPU into
    // the display texture (no pixels through the CPU).
    if (gsgl::gpuPresentationActive())
    {
        uint32_t gpuWidth = 0u, gpuHeight = 0u;
        int frameScale = 1, frameScaleY = 1;
        int copied = gsgl::hostCopyLatestFrame(tex.id, static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height), gpuWidth, gpuHeight,
                                               frameScale, frameScaleY, g_presentMaxTick);
        if (copied == 2)
        {
            // The renderer switched internal resolution (live settings), or
            // a taller picture came (GT4's 1080i: 960 rows): a display
            // texture for frames of that scale and height, then the copy.
            const int texRows = static_cast<int>(gpuHeight) > FB_HEIGHT * frameScaleY ? 2 * FB_HEIGHT * frameScaleY : FB_HEIGHT * frameScaleY;
            Image blank = GenImageColor(FB_WIDTH * frameScale, texRows, BLANK);
            UnloadTexture(tex);
            tex = LoadTextureFromImage(blank);
            UnloadImage(blank);
            SetTextureFilter(tex, g_displayTextureFilter);
            copied = gsgl::hostCopyLatestFrame(tex.id, static_cast<uint32_t>(tex.width), static_cast<uint32_t>(tex.height), gpuWidth, gpuHeight,
                                               frameScale, frameScaleY, g_presentMaxTick);
        }
        if (copied == 1)
        {
            const gsgl::FieldInfo shownField = gsgl::copiedFrameField();
            g_shownFieldParity = shownField.parity;
            g_shownCrt = shownField.crt;
            g_shownLines = shownField.lines;
            g_shownInterlaced = shownField.parity >= 0; // fields woven / bobbed (not 480p, not a whole frame)
#if GT3_ADDONS
            if (gt3rec::enabled())
                gt3rec::captureFrame(tex.id, gpuWidth, gpuHeight, currentTick);
#endif
            // GT3_PRESENT_SHOT for GPU frames: read the display texture back.
            static const char *gpuShotPrefix = [] {
                const char *v = std::getenv("GT3_PRESENT_SHOT");
                return (v && *v) ? v : nullptr;
            }();
            static const uint64_t gpuShotEvery = [] {
                const char *v = std::getenv("GT3_PRESENT_SHOT_EVERY");
                const long n = (v && *v) ? std::strtol(v, nullptr, 10) : 60L;
                return static_cast<uint64_t>(std::max(1L, n));
            }();
            static uint64_t s_lastGpuShot = 0u;
            // GT3_PRESENT_SHOT_FROM=<tick> / GT3_PRESENT_SHOT_COUNT=<n>: a burst.
            static const uint64_t gpuShotFrom = [] {
                const char *v = std::getenv("GT3_PRESENT_SHOT_FROM");
                return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            static uint64_t gpuShotsLeft = [] {
                const char *v = std::getenv("GT3_PRESENT_SHOT_COUNT");
                return (v && *v) ? std::strtoull(v, nullptr, 10) : ~0ull;
            }();
            // GT3_PRESENT_LUMA=<from tick>,<count>: every presented frame's
            // mean brightness (0..255) and its tick, for one-frame flashes the
            // PNG shots (1-20 VSyncs each) miss.
            static const std::pair<uint64_t, uint64_t> lumaRange = [] {
                std::pair<uint64_t, uint64_t> r{~0ull, 0ull};
                if (const char *v = std::getenv("GT3_PRESENT_LUMA"); v && *v)
                {
                    unsigned long long from = 0, count = 600;
                    std::sscanf(v, "%llu,%llu", &from, &count);
                    r = {from, count};
                }
                return r;
            }();
            if (currentTick >= lumaRange.first && currentTick < lumaRange.first + lumaRange.second)
            {
                Image img = LoadImageFromTexture(tex);
                ImageCrop(&img, Rectangle{0.0f, 0.0f, static_cast<float>(gpuWidth), static_cast<float>(gpuHeight)});
                ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                const uint8_t *px = static_cast<const uint8_t *>(img.data);
                const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
                uint64_t sum = 0, top = 0, bottom = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    const uint32_t l = (px[i * 4] * 3u + px[i * 4 + 1] * 6u + px[i * 4 + 2]) / 10u;
                    sum += l;
                    (i < n / 2 ? top : bottom) += l;
                }
                std::fprintf(stderr, "[present-luma] tick=%llu mean=%.1f top=%.1f bottom=%.1f\n", static_cast<unsigned long long>(currentTick),
                             n ? static_cast<double>(sum) / static_cast<double>(n) : 0.0, n ? 2.0 * static_cast<double>(top) / static_cast<double>(n) : 0.0,
                             n ? 2.0 * static_cast<double>(bottom) / static_cast<double>(n) : 0.0);
                UnloadImage(img);
            }
            if (gpuShotPrefix && currentTick >= gpuShotFrom && gpuShotsLeft != 0u && currentTick >= s_lastGpuShot + gpuShotEvery &&
                gpuShotsLeft-- != 0u)
            {
                s_lastGpuShot = currentTick;
                Image shot = LoadImageFromTexture(tex);
                ImageCrop(&shot, Rectangle{0.0f, 0.0f, static_cast<float>(gpuWidth), static_cast<float>(gpuHeight)});
                // GT3_PRESENT_SHOT_CROP=x,y,w,h (pixels of the picture): only
                // that part (small files, so every frame can be kept at 4x).
                static const Rectangle shotCrop = [] {
                    Rectangle r{0.0f, 0.0f, 0.0f, 0.0f};
                    if (const char *v = std::getenv("GT3_PRESENT_SHOT_CROP"))
                        std::sscanf(v, "%f,%f,%f,%f", &r.x, &r.y, &r.width, &r.height);
                    return r;
                }();
                if (shotCrop.width > 0.0f && shotCrop.height > 0.0f)
                    ImageCrop(&shot, shotCrop);
                char path[1024];
                std::snprintf(path, sizeof(path), "%s-%06llu.png", gpuShotPrefix, static_cast<unsigned long long>(currentTick));
                ExportImage(shot, path);
                UnloadImage(shot);
            }
#if GT3_ADDONS
            // GT3_SHOT_POINTS (gt3_shot.h): a picture where the autopilot asked.
            if (const int at = gt3shot::request.exchange(-1); gpuShotPrefix && at >= 0)
            {
                Image shot = LoadImageFromTexture(tex);
                ImageCrop(&shot, Rectangle{0.0f, 0.0f, static_cast<float>(gpuWidth), static_cast<float>(gpuHeight)});
                char path[1024];
                std::snprintf(path, sizeof(path), "%s-at%03d.png", gpuShotPrefix, at);
                ExportImage(shot, path);
                UnloadImage(shot);
            }
#endif
            s_lastWidth = gpuWidth;
            s_lastHeight = gpuHeight;
            s_hasUploadedFrame = true;
            ++g_frameUploads;
            outWidth = gpuWidth;
            outHeight = gpuHeight;
            return;
        }
    }
#endif
    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    uint32_t frameCrt = 0u;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource,
                                                   &frameCrt))
    {
#if defined(PS2X_GS_GL)
        if (gsgl::gpuPresentationActive() && s_hasUploadedFrame)
        {
            // The newest frame came from the GPU and is already shown.
            outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
            outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
            return;
        }
#endif
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTextureRec(tex, Rectangle{0.0f, 0.0f, static_cast<float>(FB_WIDTH), static_cast<float>(FB_HEIGHT)}, blank.data);
        UnloadImage(blank);
        g_shownFieldParity = -1;
        g_shownCrt = 0u;
        g_shownLines = 0;
        g_shownInterlaced = false;
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        ++g_frameUploads;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    g_shownFieldParity = -1; // a CPU frame: no field information
    g_shownCrt = frameCrt;
    g_shownLines = static_cast<int>(height);
#if defined(PS2X_GAME_GT3)
    g_shownInterlaced = ps2x::crt::interlaced(frameCrt); // the CPU presenter's fields (480i / 1080i)
#else
    // The CPU presenter's woven fields (480i / 1080i, FFMD 1); a frame-mode
    // picture (FFMD 0, the latched SMODE2's: gs_frontend.cpp) is shown whole
    // and not blended (picture track review).
    g_shownInterlaced = ps2x::crt::interlaced(frameCrt) && ps2x::crt::frameMode(frameCrt);
#endif
    s_lastWidth = width;
    s_lastHeight = height;

    // Opt-in diagnostic: GT3_PRESENT_SHOT=<path prefix> saves the presented frame as
    // <prefix>-<vsync tick>.png every GT3_PRESENT_SHOT_EVERY ticks (default 60).
    {
        static const char *shotPrefix = [] {
            const char *v = std::getenv("GT3_PRESENT_SHOT");
            return (v && *v) ? v : nullptr;
        }();
        static const uint64_t shotEvery = [] {
            const char *v = std::getenv("GT3_PRESENT_SHOT_EVERY");
            const long n = (v && *v) ? std::strtol(v, nullptr, 10) : 60L;
            return static_cast<uint64_t>(std::max(1L, n));
        }();
        static uint64_t s_lastShotTick = 0u;
        if (shotPrefix && !s_scratch.empty() && width != 0u && height != 0u && currentTick >= s_lastShotTick + shotEvery &&
            s_scratch.size() >= static_cast<size_t>(width) * height * 4u)
        {
            s_lastShotTick = currentTick;
            Image shot{};
            shot.data = s_scratch.data();
            shot.width = static_cast<int>(width);
            shot.height = static_cast<int>(height);
            shot.mipmaps = 1;
            shot.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            std::vector<uint8_t> opaque(s_scratch);
            for (size_t i = 3; i < opaque.size(); i += 4)
                opaque[i] = 0xFFu;
            shot.data = opaque.data();
            char path[1024];
            std::snprintf(path, sizeof(path), "%s-%06llu.png", shotPrefix, static_cast<unsigned long long>(currentTick));
            ExportImage(shot, path);
        }
    }

    // Diagnostic: GT3_GS_DRAW_LOG=teal:<n> asks the GS to log the draws of the
    // next frame after a presented frame comes out cyan-tinted (G ~ B > R), at
    // most <n> times, 60 VSyncs apart.
    {
        static const long tealLogs = [] {
            const char *v = std::getenv("GT3_GS_DRAW_LOG");
            return (v && std::strncmp(v, "teal", 4) == 0) ? std::max(1L, std::strtol(v[4] == ':' ? v + 5 : "3", nullptr, 10)) : 0L;
        }();
        static long s_tealLogged = 0;
        static uint64_t s_lastTealTick = 0u;
        if (s_tealLogged < tealLogs && currentTick >= s_lastTealTick + 60u && !s_scratch.empty() && width >= 64u && height >= 64u &&
            s_scratch.size() >= static_cast<size_t>(width) * height * 4u)
        {
            uint64_t sum[3] = {0u, 0u, 0u};
            uint32_t n = 0u;
            for (uint32_t y = height / 8u; y < height - height / 8u; y += 8u)
                for (uint32_t x = width / 8u; x < width - width / 8u; x += 8u)
                {
                    const uint8_t *p = s_scratch.data() + (static_cast<size_t>(y) * width + x) * 4u;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++n;
                }
            const double r = static_cast<double>(sum[0]) / n, g = static_cast<double>(sum[1]) / n, b = static_cast<double>(sum[2]) / n;
            if (g > 30.0 && r < 0.8 * g && std::abs(g - b) < 0.05 * g)
            {
                ++s_tealLogged;
                s_lastTealTick = currentTick;
                std::fprintf(stderr, "[gs-draw] teal frame presented tick=%llu mean=%.0f/%.0f/%.0f\n",
                             static_cast<unsigned long long>(currentTick), r, g, b);
                rt->gs().requestDrawLog(currentTick);
            }
        }
    }

    // CPU frames up to 640x512 fill the top-left 640x512; GT4's 1080i
    // picture (960 rows) needs a taller display texture (and stays in it).
    const uint32_t uploadRows = height > static_cast<uint32_t>(FB_HEIGHT) ? 2u * FB_HEIGHT : static_cast<uint32_t>(FB_HEIGHT);
    if (static_cast<uint32_t>(tex.height) < uploadRows)
    {
        Image blank = GenImageColor(std::max(tex.width, FB_WIDTH), static_cast<int>(uploadRows), BLANK);
        UnloadTexture(tex);
        tex = LoadTextureFromImage(blank);
        UnloadImage(blank);
        SetTextureFilter(tex, g_displayTextureFilter);
    }
    s_uploadBuffer.resize(static_cast<size_t>(FB_WIDTH) * uploadRows * 4u);
    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, uploadRows);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    // CPU frames fill the top-left 640x512 (640x1024 for 1080i; the texture is larger at a GPU internal resolution).
    UpdateTextureRec(tex, Rectangle{0.0f, 0.0f, static_cast<float>(FB_WIDTH), static_cast<float>(uploadRows)}, s_uploadBuffer.data());
#if GT3_ADDONS
    if (gt3rec::enabled())
        gt3rec::captureFrame(tex.id, width, height, currentTick);
#endif
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
    ++g_frameUploads;
}

ps2x::iop::EeRpcServerSnapshot PS2Runtime::findEeRpcServer(uint32_t sid) const {
    std::lock_guard lock(m_eeRpcDirectoryMutex);
    const auto found=m_eeRpcDirectory.find(sid);
    if (found!=m_eeRpcDirectory.end()) return found->second;
    ps2x::iop::EeRpcServerSnapshot missing; missing.supported=true; missing.sid=sid; return missing;
}
void PS2Runtime::recordEeRpcServer(uint32_t sid,uint32_t server,uint32_t function,uint32_t buffer,uint32_t queue) {
    std::lock_guard lock(m_eeRpcDirectoryMutex);
    // Called only after the real syscall registration and strict EE descriptor
    // validation. Tokens survive clearing; replacement never revives a snapshot.
    m_eeRpcDirectory.erase(sid);
    if (server) std::erase_if(m_eeRpcDirectory,[&](const auto& entry) { return entry.second.server==server; });
    if (!server || !function || m_nextEeRpcRegistration==UINT64_MAX) return;
    m_eeRpcDirectory[sid]={true,m_nextEeRpcRegistration++,sid,server,function,buffer,queue};
}
void PS2Runtime::removeEeRpcServer(uint32_t server,uint32_t queue) {
    std::lock_guard lock(m_eeRpcDirectoryMutex);
    std::erase_if(m_eeRpcDirectory,[&](const auto& entry) { return entry.second.server==server && entry.second.queue==queue; });
}
void PS2Runtime::removeEeRpcQueue(uint32_t queue) {
    std::lock_guard lock(m_eeRpcDirectoryMutex);
    std::erase_if(m_eeRpcDirectory,[&](const auto& entry) { return entry.second.queue==queue; });
}
void PS2Runtime::clearEeRpcServers() { std::lock_guard lock(m_eeRpcDirectoryMutex); m_eeRpcDirectory.clear(); }

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    m_eeScheduler = std::make_unique<EeScheduler>(*this);
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
    if (const char *applicationDirectory = GetApplicationDirectory();
        applicationDirectory && applicationDirectory[0] != '\0')
    {
        m_iopSubsystem->setPluginSearchPaths({std::filesystem::path(applicationDirectory) / "iop_plugins"});
    }
#endif

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = kCallbackStackFloor;
    m_asyncCallbackStackTop = kCallbackStackTop;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
#if defined(PS2X_GAME_GT4)
        // The power-off quit handler and the wheel factory point at this runtime. The wheel
        // itself (LGDEV's) stops its forces when the IOP subsystem goes below.
        ps2_gt4_glue::shutdown();
#endif
        requestStop();
        m_memory.stopVif1Worker(); // it feeds the GS worker and wakes the scheduler
        m_gsWorker.stop(); // before the scheduler its GS interrupts wake goes away
#if defined(PS2X_GAME_GT3)
        m_gt3MovieWorker.reset();
#endif
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
#if GT3_ADDONS
            gt3rec::shutdown();
#endif
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

void PS2Runtime::setIopPluginSearchPaths(std::vector<std::filesystem::path> paths)
{
    m_iopSubsystem->setPluginSearchPaths(std::move(paths));
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::runInIopCallScope(uint8_t *rdram, R5900Context *ctx, const std::function<void()> &work)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    work();
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::NativeLibraryRegistry* PS2Runtime::nativeIopLibraries()
{
    return m_iopSubsystem ? &m_iopSubsystem->nativeLibraries() : nullptr;
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

namespace
{
// Debug: GT3_EE_WATCH=base,bytes,watch (hex EE addresses). Every VSync copies
// [base, base+bytes) into a 128-entry ring. The first time the float at `watch`
// is non-finite or beyond 1e6 in magnitude, the ring (oldest first; per entry a
// u64 VSync tick then the bytes) goes to GT3_EE_WATCH_OUT (default
// ee-watch.bin). Finds where a guest value starts to diverge.
// GT4_WATCH=<addr>[,<addr>...] (hex EE addresses): every GT4_WATCH_EVERY
// VSyncs (default 120) prints the 32-bit words at those addresses.
void gt4WatchVSync(uint8_t *rdram, uint64_t tick)
{
    static const std::vector<uint32_t> addresses = [] {
        std::vector<uint32_t> out;
        const char *v = std::getenv("GT4_WATCH");
        for (const char *p = v ? v : ""; *p;)
        {
            char *end = nullptr;
            out.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 16)) & 0x01fffffcu);
            if (end == p)
                break;
            p = *end == ',' ? end + 1 : end;
        }
        return out;
    }();
    static const uint64_t every = [] {
        const char *v = std::getenv("GT4_WATCH_EVERY");
        return v && *v ? std::max<uint64_t>(1, std::strtoull(v, nullptr, 10)) : 120u;
    }();
    if (addresses.empty() || !rdram || tick % every)
        return;
    std::fprintf(stderr, "[watch] vsync %llu", static_cast<unsigned long long>(tick));
    for (uint32_t a : addresses)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + a, 4);
        std::fprintf(stderr, " %06x=%08x", a, value);
    }
    std::fprintf(stderr, "\n");
}

void eeWatchVSync(uint8_t *rdram, uint64_t tick)
{
    struct Watch
    {
        bool enabled = false, fired = false;
        uint32_t base = 0, bytes = 0, watch = 0;
        std::vector<std::vector<uint8_t>> ring;
        std::vector<uint64_t> ticks;
        size_t next = 0, filled = 0;
    };
    static Watch w = [] {
        Watch value;
        const char *spec = std::getenv("GT3_EE_WATCH");
        if (spec && std::sscanf(spec, "%x,%x,%x", &value.base, &value.bytes, &value.watch) == 3 &&
            value.bytes && value.base + value.bytes <= PS2_RAM_SIZE && value.watch + 4 <= PS2_RAM_SIZE)
        {
            value.enabled = true;
            value.ring.assign(128, std::vector<uint8_t>(value.bytes));
            value.ticks.assign(128, 0);
            std::cerr << "[ee-watch] base 0x" << std::hex << value.base << " bytes 0x" << value.bytes << " watch 0x"
                      << value.watch << std::dec << std::endl;
        }
        return value;
    }();
    if (!w.enabled || w.fired || !rdram)
        return;
    std::memcpy(w.ring[w.next].data(), rdram + w.base, w.bytes);
    w.ticks[w.next] = tick;
    w.next = (w.next + 1) % w.ring.size();
    w.filled = std::min(w.filled + 1, w.ring.size());
    float value;
    std::memcpy(&value, rdram + w.watch, 4);
    if (std::isfinite(value) && std::fabs(value) <= 1e6f)
        return;
    w.fired = true;
    const char *out = std::getenv("GT3_EE_WATCH_OUT");
    std::ofstream file(out ? out : "ee-watch.bin", std::ios::binary);
    for (size_t k = 0; k < w.filled; ++k)
    {
        const size_t index = (w.next + w.ring.size() - w.filled + k) % w.ring.size();
        file.write(reinterpret_cast<const char *>(&w.ticks[index]), 8);
        file.write(reinterpret_cast<const char *>(w.ring[index].data()), w.bytes);
    }
    std::cerr << "[ee-watch] 0x" << std::hex << w.watch << std::dec << " = " << value << " at VSync " << tick
              << "; wrote " << w.filled << " snapshots" << std::endl;
}
} // namespace

namespace vif1lat
{
    extern std::atomic<int> eeActivity;
}
namespace gsHandoff
{
    extern std::function<void()> wake;
}

void PS2Runtime::queueVSyncPresentation(uint64_t tick)
{
    struct Mark
    {
        int before = vif1lat::eeActivity.exchange(3, std::memory_order_relaxed);
        ~Mark() { vif1lat::eeActivity.store(before, std::memory_order_relaxed); }
    } activityMark;
    eeWatchVSync(m_memory.getRDRAM(), tick);
    gt4WatchVSync(m_memory.getRDRAM(), tick);
    if (frameLogPeriod() > 0.0)
    {
        // A flip: the displayed buffer (read circuit 2 if enabled, else 1)
        // changed, as GS::latchPresentationSnapshot counts it for frameStats.
        const GSRegisters &regs = m_memory.gs();
        const uint32_t fbp = static_cast<uint32_t>(((regs.pmode & 2u) ? regs.dispfb2 : regs.dispfb1) & 0x1FFu);
        FrameLog &frameLog = g_frameLog;
        if (fbp != frameLog.lastFbp)
        {
            frameLog.lastFbp = fbp;
            const int64_t now = steadyNowNs();
            std::lock_guard<std::mutex> lock(frameLog.mutex);
            if (frameLog.flips != 0u)
            {
                const int64_t gap = now - frameLog.lastFlipNs;
                ++frameLog.gaps;
                frameLog.sumNs += gap;
                frameLog.maxNs = std::max(frameLog.maxNs, gap);
                frameLog.maxVSyncs = std::max(frameLog.maxVSyncs, tick - frameLog.lastFlipTick);
            }
            frameLog.lastFlipTick = tick;
            frameLog.lastFlipNs = now;
            ++frameLog.flips;
        }
    }
    if (!m_gsWorker.running())
    {
        m_streamPresentation = false;
        return;
    }
    m_streamPresentation = true;
    const GSRegisters &regs = m_memory.gs();
    GS::DisplaySnapshot snapshot;
    snapshot.pmode = regs.pmode;
    snapshot.smode2 = regs.smode2;
    snapshot.dispfb1 = regs.dispfb1;
    snapshot.display1 = regs.display1;
    snapshot.dispfb2 = regs.dispfb2;
    snapshot.display2 = regs.display2;
    snapshot.bgcolor = regs.bgcolor;
    snapshot.vsyncTick = tick;
    snapshot.crt = ps2x::crt::current();
    m_gsWorker.postFrame([this, snapshot] { m_gs.latchPresentationSnapshot(snapshot); });
}

// The Vulkan renderer was selected (the colour LUT, GT4_LUT, is for its picture only; set below).
static bool g_vulkanRendererSelected = false;

void PS2Runtime::enableGpuRendererIfRequested()
{
    const char *renderer = std::getenv("GT3_RENDERER");
    if (renderer && !*renderer)
        renderer = nullptr;
#if defined(GT4_STOCK_BUILD)
    renderer = "gl"; // the stock package draws with the OpenGL renderer (the CPU one if that fails)
#elif defined(PS2X_GS_VK)
    // A Vulkan build (the Full edition): GT3_RENDERER = auto (the default when unset) | vk | gl.
    if (!renderer)
        renderer = "auto";
#endif
    // GT3_RENDERER=auto: Vulkan on an NVIDIA or AMD graphics card; any other card draws with the
    // OpenGL renderer. On AMD the Vulkan frames go to the window through Vulkan's own swapchain
    // (gs_vk_present.h): AMD's OpenGL driver crashed on the Vulkan-to-OpenGL shared presentation
    // image at GT4's first picture (RX 9070 XT), and its OpenGL renderer ran a race at 6-7 fps there.
    // GT4_RENDERER_PRETEND_VENDOR=amd|nvidia|<name>: a test switch standing in for the card's vendor.
    std::string why;
    bool autoAmd = false;
    if (renderer && std::strcmp(renderer, "auto") == 0)
    {
        std::string vendor;
        const char *pretend = std::getenv("GT4_RENDERER_PRETEND_VENDOR");
        if (pretend && *pretend)
            vendor = pretend;
#if defined(PS2X_GS_GL)
        else
            vendor = gsgl::currentVendor();
#endif
        std::string lower = vendor;
        for (char &ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool nvidia = lower.find("nvidia") != std::string::npos || lower.find("10de") != std::string::npos;
        // AMD (2026-10-11): Vulkan with its own window presenter (gs_vk_present.h; below), never
        // through OpenGL. Other cards (Intel: not tried) keep the OpenGL renderer.
        autoAmd = !nvidia && (lower.find("amd") != std::string::npos || lower.find("ati technologies") != std::string::npos ||
                              lower.find("advanced micro devices") != std::string::npos || lower.find("1002") != std::string::npos);
        renderer = nvidia || autoAmd ? "vk" : "gl";
        why = std::string("auto: ") + (nvidia ? "NVIDIA" : autoAmd ? "AMD" : "not NVIDIA or AMD") + " graphics card, vendor \"" + vendor + "\"" +
              (pretend && *pretend ? " (GT4_RENDERER_PRETEND_VENDOR)" : " (GL_VENDOR)");
    }
    else if (renderer)
        why = std::string("GT3_RENDERER=") + renderer;
    // GT3_RENDERER=vk: the same renderer on Vulkan (experimental; builds with
    // GT3_VULKAN=ON). Without a usable Vulkan device: the OpenGL one.
    const bool wantsVulkan = renderer && std::strcmp(renderer, "vk") == 0;
    if (!renderer || (std::strcmp(renderer, "gl") != 0 && !wantsVulkan))
        return;
    bool vulkan = wantsVulkan;
#if defined(PS2X_GS_VK)
    if (vulkan && !gsvk::rendererUsable(true))
    {
        std::cerr << "[gs-vk] Vulkan is not usable here; using the OpenGL renderer" << std::endl;
        vulkan = false;
    }
    // The renderer's own window presenter (gs_vk_present.h: a Vulkan swapchain on the window;
    // GT4_VK_PRESENT, by default on any card but NVIDIA's), else its frames go through OpenGL.
    bool ownPresenter = false;
    if (vulkan && gsvk::present::wanted())
        ownPresenter = gsvk::present::start(GetWindowHandle(), m_headless);
    if (vulkan && !ownPresenter && (autoAmd || !gsvk::rendererUsable(false)))
    {
        // (auto on AMD: never the OpenGL hand-over, which crashed AMD's OpenGL driver.)
        std::cerr << "[gs-vk] the Vulkan frames cannot be shown here; using the OpenGL renderer" << std::endl;
        why += ", no Vulkan presenter";
        vulkan = false;
    }
    if (ownPresenter)
    {
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
        ps2x_vulkan_presents = 1; // OpenGL never swaps the window again
#endif
        why += ", " + std::string(gsvk::present::describe());
    }
#else
    if (vulkan)
        std::cerr << "[gs-vk] this build has no Vulkan renderer (GT3_VULKAN=OFF); using the OpenGL renderer" << std::endl;
    vulkan = false;
#endif
    if (wantsVulkan && !vulkan)
        why += ", Vulkan not usable";
    std::cerr << "[renderer] " << (vulkan ? "vk" : "gl") << " (" << why << ")" << std::endl;
    _putenv_s("GT3_RENDERER", vulkan ? "vk" : "gl"); // what was chosen, for the live settings (GPU renderer)
#if defined(PS2X_GS_GL)
    if (!gsgl::createSharedContext())
    {
        std::cerr << "[gs-gl] no shared OpenGL context; using the CPU renderer" << std::endl;
        return;
    }
    // The renderer runs on its own thread ("GS GL", owning the OpenGL
    // context) behind GSAsyncBackend: the GS worker parses GIF packets and
    // queues the draws. GT3_GS_GL_SPLIT=0: the GS worker owns the context and
    // draws itself (one thread, as before).
    const bool compare = [] {
        const char *cmp = std::getenv("GT3_GS_COMPARE");
        return cmp && *cmp == '1';
    }();
    const bool split = vulkan || [] {
        const char *v = std::getenv("GT3_GS_GL_SPLIT");
        return !(v && *v == '0');
    }();
    m_memory.quiesceVif1();
    m_gsWorker.stop();
    const auto makeCurrent = [] {
        if (!gsgl::makeSharedContextCurrent())
            std::cerr << "[gs-gl] could not make the context current on the renderer thread" << std::endl;
    };
    const auto release = [] { gsgl::releaseCurrentContext(); };
    const bool threaded = split ? m_gsWorker.start([this](const uint8_t *data, uint32_t size) { m_gs.processGIFPacket(data, size); })
                                : m_gsWorker.start([this](const uint8_t *data, uint32_t size) { m_gs.processGIFPacket(data, size); },
                                                   makeCurrent, release);
    if (!threaded)
    {
        std::cerr << "[gs-gl] the OpenGL renderer needs the GS worker thread (GT3_GS_THREAD=0 is set); using the CPU renderer" << std::endl;
        return;
    }
    if (compare)
        std::cerr << "[gs-compare] comparing the " << (vulkan ? "Vulkan" : "OpenGL") << " renderer against the CPU renderer at every presentation"
                  << std::endl;
#if defined(PS2X_GS_VK)
    if (vulkan)
    {
        // Its own thread with the shared OpenGL context current (the Vulkan
        // device is picked to match that context's GPU).
        m_gs.setRasterBackend(std::make_unique<GSAsyncBackend>(
            [compare](GSAsyncBackend::RunSync run) { return gsvk::createBackend(run, run, compare); }, makeCurrent, release));
        std::cerr << "[gs-vk] Vulkan renderer selected (see the [gs-gl] lines of its OpenGL-derived source and [vkgl])" << std::endl;
        g_vulkanRendererSelected = true;
#if defined(PS2X_GAME_GT4)
        gt4fps::setRenderer("Vulkan");
#endif
        return;
    }
#endif
    GSGlBackend *gl = nullptr;
    auto wrap = [&gl, compare](std::unique_ptr<GSGlBackend> backend) -> std::unique_ptr<GSRasterBackend> {
        gl = backend.get();
        if (compare)
            return std::make_unique<GSCompareBackend>(std::move(backend));
        return backend;
    };
    if (split)
    {
        m_gs.setRasterBackend(std::make_unique<GSAsyncBackend>(
            [&wrap](GSAsyncBackend::RunSync run) { return wrap(std::make_unique<GSGlBackend>(run, run)); }, makeCurrent, release));
    }
    else
    {
        m_gs.setRasterBackend(wrap(std::make_unique<GSGlBackend>([this](const std::function<void()> &task) { m_gsWorker.runOnWorker(task); },
                                                                 [this](const std::function<void()> &task) { m_gsWorker.runOnWorker(task, true); })));
    }
    std::cerr << "[gs-gl] OpenGL renderer " << (gl->glReady() ? "active" : "failed to initialise (drawing stays on the CPU)")
              << (split ? " (renderer thread separate from the GS worker)" : "") << std::endl;
#if defined(PS2X_GAME_GT4)
    gt4fps::setRenderer(gl->glReady() ? "OpenGL" : "CPU");
#endif
#else
    std::cerr << "[gs-gl] this build has no OpenGL renderer; using the CPU renderer" << std::endl;
#endif
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_memory.stopVif1Worker(); // it feeds the GS worker
    m_gsWorker.stop(); // finishes queued packets before the GS is re-bound
    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    // GIF packets go to the GS worker thread in order (GT3_GS_THREAD=0: inline).
    if (m_gsWorker.start([this](const uint8_t *data, uint32_t size) { m_gs.processGIFPacket(data, size); }))
        std::cerr << "[gs] GS worker thread running" << std::endl;
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gsWorker.submit(data, size); });
    m_gifArbiter.setTakePacketFn([this](std::vector<uint8_t> &packet) { m_gsWorker.submitOwned(packet); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setLocalToHostReader([this](uint8_t *dst, uint32_t maxBytes)
                                  {
                                      m_gsWorker.sync();
                                      return m_gs.consumeLocalToHostBytes(dst, maxBytes);
                                  });
    m_gs.setEventSink([this](uint32_t csrBits)
                      {
                          // From the GS worker the EE may be idle: wake it to dispatch the interrupt.
                          const bool raised = m_memory.raiseGsEvent(csrBits);
#if !defined(PS2X_GAME_GT3)
                          m_gsWorker.noteGsEvent(csrBits, raised); // GT3_GS_WORKER_STATS
#endif
                          if (raised && m_eeScheduler)
                              m_eeScheduler->postEvent(EeEvent{EeEventType::Wake, 0, 0});
                      });
    // MSCAL/MSCNT run VU1 on the VIF1 worker thread when it is active: the
    // D/T-bit enables then come from the EE thread that posted the input, and
    // the stop bits go back through the memory for the EE to publish.
    auto runVu1 = [this](bool resume, uint32_t startPC, uint32_t top, uint32_t itop)
    {
        const bool onWorker = m_memory.onVif1WorkerThread();
        R5900Context *cpuContext = nullptr;
        uint32_t fbrst = 0u;
        if (onWorker)
            fbrst = m_memory.vif1WorkerVu0Fbrst();
        else
        {
            cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
            if (!cpuContext)
                cpuContext = &m_cpuContext;
            fbrst = cpuContext->vu0_fbrst;
        }
        m_vu1.state().dBitEnabled = (fbrst & (1u << 10)) != 0u;
        m_vu1.state().tBitEnabled = (fbrst & (1u << 11)) != 0u;
        vu1KickRecorder().before(m_vu1, resume ? 1u : 0u, resume ? 0u : startPC, top, itop, 65536u, m_memory.getVU1Code(),
                                 m_memory.getVU1Data(), m_memory.gs().vsyncTick.load());
#if GT3_ADDONS
        // Motion data (gt3_motion.h; GT3_MOTION_DEBUG or F6): off by default.
        gt3motion::Tracker &motion = gt3motion::Tracker::instance();
        const bool trackMotion = motion.enabled() && !m_vu1.packetSink;
        if (trackMotion)
        {
            motion.beginKick(0u, startPC, resume, top, m_memory.getVU1Data(), m_memory.gs().vsyncTick.load());
            m_vu1.packetSink = [this](const uint8_t *packet, uint32_t size) {
                gt3motion::Tracker::instance().packet(m_vu1.state(), m_vu1.translatedImageHash(), packet, size);
                return false;
            };
        }
#else
        constexpr bool trackMotion = false;
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // The VU1 tap (gt4_vu1_tap.h, GT4_VU1_TAP; lighting step 1): off by default, one branch per program.
        const bool tap = gt4tap::enabled() && !resume && !m_vu1.packetSink;
        if (tap)
        {
            gt4tap::beginProgram(m_vu1.state(), startPC, top, itop, m_memory.getVU1Data(), m_memory.gs().vsyncTick.load());
            m_vu1.packetSink = [](const uint8_t *packet, uint32_t size) {
                gt4tap::packet(packet, size);
                return false;
            };
        }
#endif
        if (resume)
            m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs, &m_memory,
                         top, itop, 65536);
        else
            m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE, m_memory.getVU1Data(), PS2_VU1_DATA_SIZE, m_gs, &m_memory,
                          startPC, top, itop, 65536);
        if (trackMotion)
            m_vu1.packetSink = nullptr;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        if (tap)
        {
            m_vu1.packetSink = nullptr;
            gt4tap::endProgram();
        }
#endif
        vu1KickRecorder().after(m_vu1, m_memory.getVU1Data());
        const uint32_t stop = (m_vu1.state().stoppedByD ? 0x0200u : 0u) | (m_vu1.state().stoppedByT ? 0x0400u : 0u);
        if (onWorker)
            m_memory.m_vif1VpuStop.store(0x80000000u | stop, std::memory_order_release);
        else
            cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stop;
    };
    m_memory.setVu1MscalCallback([runVu1](uint32_t startPC, uint32_t top, uint32_t itop)
                                 { runVu1(false, startPC, top, itop); });
    m_memory.setVu1MscntCallback([runVu1](uint32_t top, uint32_t itop)
                                 { runVu1(true, 0u, top, itop); });
    if (vif1ThreadEnabled())
    {
        // The VIF1 worker hands its XGKICK packets to the GS worker in
        // batches, flushed after every command (before it parks or raises an
        // interrupt): one lock per batch instead of per packet (the GS
        // worker's lock was ~5% of the VIF1/VU1 thread).
        m_memory.setVif1AfterCommand([this] { m_gsWorker.flushProducerBatch(); });
        // GT4_VU1_HANDOFF (gs_worker.cpp): the GS side drained below its frame cap: the EE scheduler may run
        // the guest's threads again.
        gsHandoff::wake = [this]
        {
            if (m_eeScheduler)
                m_eeScheduler->postEvent(EeEvent{EeEventType::Wake, 0, 0});
        };
        m_memory.startVif1Worker(
            [this]
            {
                // An I-bit interrupt or a finished DMA: the EE may be idle.
                if (m_eeScheduler)
                    m_eeScheduler->postEvent(EeEvent{EeEventType::Wake, 0, 0});
            },
            [this]
            {
                ThreadNaming::SetCurrentThreadName("VIF1/VU1");
                ThreadNaming::PreferPerformance();
                applyGuestFloatMode();
                m_gsWorker.beginProducerBatch();
            },
            [this]
            {
                const R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                return (context ? context : &m_cpuContext)->vu0_fbrst;
            });
        std::cerr << "[vif1] VIF1/VU1 worker thread running (GT3_VIF1_THREAD=0 runs them on the EE thread)" << std::endl;
    }
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

#if !defined(PS2X_GAME_GT3)
namespace
{
    // GT4_MONITOR=<x>,<y> [name] (the GT4 Stock Launcher's choice): the monitor
    // at that desktop position with that name, else one of that name (the
    // layout changed), else one at that position; -1 when none matches.
    int chooseGt4Monitor(const char *spec)
    {
        int x = 0, y = 0, used = 0;
        if (!spec || std::sscanf(spec, "%d,%d%n", &x, &y, &used) < 2)
            return -1;
        std::string name = spec + used;
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t\r\n") + 1);
        const int count = GetMonitorCount();
        auto at = [&](int m) {
            const Vector2 p = GetMonitorPosition(m);
            return static_cast<int>(p.x) == x && static_cast<int>(p.y) == y;
        };
        auto named = [&](int m) {
            const char *n = GetMonitorName(m);
            return !name.empty() && n && name == n;
        };
        for (int m = 0; m < count; ++m)
            if (at(m) && (name.empty() || named(m)))
                return m;
        for (int m = 0; m < count; ++m)
            if (named(m))
                return m;
        for (int m = 0; m < count; ++m)
            if (at(m))
                return m;
        return -1;
    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4_SCREENS=3 (gt4screens): the middle monitor and the monitors right next to it on the left and right (their
    // desktop rectangles meet it within 8 pixels and share at least half its height). Returns how many of the three
    // were found; x, y, w, h: the rectangle over them; what: a line for the log.
    int gt4ThreeScreens(int middle, int &x, int &y, int &w, int &h, std::string &what)
    {
        auto rect = [](int m, int &mx, int &my, int &mw, int &mh) {
            const Vector2 p = GetMonitorPosition(m);
            mx = static_cast<int>(p.x);
            my = static_cast<int>(p.y);
            mw = GetMonitorWidth(m);
            mh = GetMonitorHeight(m);
        };
        int cx = 0, cy = 0, cw = 0, ch = 0;
        rect(middle, cx, cy, cw, ch);
        int left = -1, right = -1;
        for (int m = 0; m < GetMonitorCount(); ++m)
        {
            int mx = 0, my = 0, mw = 0, mh = 0;
            rect(m, mx, my, mw, mh);
            if (m == middle || std::min(my + mh, cy + ch) - std::max(my, cy) < ch / 2)
                continue;
            if (left < 0 && std::abs(mx + mw - cx) <= 8)
                left = m;
            else if (right < 0 && std::abs(mx - (cx + cw)) <= 8)
                right = m;
        }
        int x0 = cx, y0 = cy, x1 = cx + cw, y1 = cy + ch;
        auto name = [&](int m) {
            if (m < 0)
                return std::string("none");
            int mx = 0, my = 0, mw = 0, mh = 0;
            rect(m, mx, my, mw, mh);
            x0 = std::min(x0, mx);
            y0 = std::min(y0, my);
            x1 = std::max(x1, mx + mw);
            y1 = std::max(y1, my + mh);
            const char *n = GetMonitorName(m);
            return "monitor " + std::to_string(m + 1) + " '" + (n ? n : "") + "' " + std::to_string(mw) + "x" + std::to_string(mh) + " at " + std::to_string(mx) + "," +
                   std::to_string(my);
        };
        what = "left " + name(left) + ", middle " + name(middle) + ", right " + name(right);
        x = x0;
        y = y0;
        w = x1 - x0;
        h = y1 - y0;
        return 1 + (left >= 0 ? 1 : 0) + (right >= 0 ? 1 : 0);
    }
#endif
}
#endif

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }
        if (const char *watch = std::getenv("GT3_WATCH_ADDR"); watch && *watch)
        {
            g_ps2WatchTickSource = &m_memory.gs().vsyncTick;
            if (const char *size = std::getenv("GT3_WATCH_SIZE"); size && *size)
                g_ps2WriteWatchSize = static_cast<uint32_t>(std::strtoul(size, nullptr, 0));
            if (const char *from = std::getenv("GT3_WATCH_FROM_TICK"); from && *from)
                g_ps2WatchFromTick = std::strtoull(from, nullptr, 10);
            g_ps2WriteWatchAddr = static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)) & 0x01FFFFFFu;
            std::cerr << "[write-watch] watching 0x" << std::hex << g_ps2WriteWatchAddr << std::dec
                      << " size " << g_ps2WriteWatchSize << " from tick " << g_ps2WatchFromTick << std::endl;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
        std::string pluginError;
        if (!m_iopSubsystem->loadPlugins(&pluginError))
        {
            std::cerr << "Failed to load IOP plugins: " << pluginError << std::endl;
            return false;
        }
#endif
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
        std::string headlessSwapMode; // GT4 --headless, for the [headless] line
#endif
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        // ALWAYS_RUN: raylib otherwise blocks EndDrawing in glfwWaitEvents
        // while the window is minimized, so the host loop (presentation, the
        // runtime stop request, shutdown) froze until the window came back.
        // --headless: hidden and unfocused (GLFW then never shows or
        // activates it), and never resized: SetWindowSize and full screen
        // reach glfwFocusWindow (SetForegroundWindow).
        SetConfigFlags(m_headless ? (FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNFOCUSED | FLAG_WINDOW_ALWAYS_RUN)
                                  : (FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN));
        int windowWidth = HOST_WINDOW_WIDTH, windowHeight = HOST_WINDOW_HEIGHT;
#if !defined(PS2X_GAME_GT3) && !defined(GT4_STOCK_BUILD)
        // PS2X_HEADLESS_WINDOW_SIZE=<w>x<h> (GT4 development builds, --headless only; diagnostics,
        // analysis/design/480p-colour/DESIGN.md D7): the hidden window is created at that size (it is never
        // resized, see above), so a hidden run draws the picture pass at a real window's size, as a player's
        // 2560x1440 or 2560x1600 window does (the scaling filters' cost in a race).
        if (const char *size = std::getenv("PS2X_HEADLESS_WINDOW_SIZE"); m_headless && size && *size)
        {
            int w = 0, h = 0;
            if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 224 && w <= 7680 && h <= 4320)
            {
                windowWidth = w;
                windowHeight = h;
            }
        }
#endif
        InitWindow(windowWidth, windowHeight, title);
        if (m_headless && IsWindowReady())
        {
            stopIfHeadlessWindowShown("window created");
            if (windowWidth != HOST_WINDOW_WIDTH || windowHeight != HOST_WINDOW_HEIGHT)
                std::cerr << "[headless] hidden window created at " << windowWidth << "x" << windowHeight << " (PS2X_HEADLESS_WINDOW_SIZE): screen "
                          << GetScreenWidth() << "x" << GetScreenHeight() << ", render " << GetRenderWidth() << "x" << GetRenderHeight() << std::endl;
            SetExitKey(KEY_NULL);
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
            // GT4 --headless: the hidden window's back buffer is never shown
            // (screenshots come from the frame textures). It swaps like a
            // visible window, except while the console display is off
            // (startHeadlessSwap); the host loop and its pacing run the same
            // either way.
            headlessSwapMode = startHeadlessSwap();
            updateHeadlessSwap(0);
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            if (gt4screens::config().on)
            {
                // Hidden runs are never moved or resized: only what a visible window would cover.
                int x = 0, y = 0, w = 0, h = 0;
                std::string what;
                const int found = gt4ThreeScreens(GetCurrentMonitor(), x, y, w, h, what);
                std::cerr << "[window] GT4_SCREENS=3 (hidden run: the window stays " << windowWidth << "x" << windowHeight << "): a visible window would cover "
                          << found << " of 3 monitors (" << what << "), " << w << "x" << h << " at " << x << "," << y << std::endl;
            }
#endif
        }
        // GT3_WINDOW_SIZE=<w>x<h>: the window's starting size (centred);
        // GT3_FULLSCREEN=1: borderless full screen on the current monitor.
        // GT4_MONITOR=<x>,<y> [name] (GT4 builds): that monitor is the current
        // one (the window is centred on it first).
        if (IsWindowReady() && !m_headless)
        {
#if !defined(PS2X_GAME_GT3)
            // GT4 (developer, 2026-10-07): Esc never quits. It leaves full screen
            // (the host loop handles it); closing the window stays the PS2
            // power button.
            SetExitKey(KEY_NULL);
            [[maybe_unused]] int chosenMonitor = -1;
            if (const char *want = std::getenv("GT4_MONITOR"); want && *want)
            {
                const int m = chooseGt4Monitor(want);
                chosenMonitor = m;
                if (m >= 0)
                {
                    const Vector2 o = GetMonitorPosition(m);
                    SetWindowPosition(static_cast<int>(o.x) + std::max(0, (GetMonitorWidth(m) - GetScreenWidth()) / 2),
                                      static_cast<int>(o.y) + std::max(0, (GetMonitorHeight(m) - GetScreenHeight()) / 2));
                    std::cerr << "[window] GT4_MONITOR: monitor " << (m + 1) << " '" << GetMonitorName(m) << "' " << GetMonitorWidth(m) << "x"
                              << GetMonitorHeight(m) << " at " << static_cast<int>(o.x) << "," << static_cast<int>(o.y) << std::endl;
                }
                else
                    std::cerr << "[window] GT4_MONITOR=" << want << ": no such monitor now; the current one" << std::endl;
            }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            // GT4_SCREENS=3 (gt4screens): one borderless window over the middle monitor and its neighbours on the left
            // and right; GT3_WINDOW_SIZE and GT3_FULLSCREEN are not used.
            bool screensWindow = false;
            if (gt4screens::config().on)
            {
                int x = 0, y = 0, w = 0, h = 0;
                std::string what;
                const int found = gt4ThreeScreens(chosenMonitor >= 0 ? chosenMonitor : GetCurrentMonitor(), x, y, w, h, what);
                SetWindowState(FLAG_WINDOW_UNDECORATED);
                SetWindowPosition(x, y);
                SetWindowSize(w, h);
                screensWindow = true;
                std::cerr << "[window] GT4_SCREENS=3: " << found << " of 3 monitors (" << what << "): one window " << w << "x" << h << " at " << x << "," << y
                          << (found < 3 ? " (the picture is still cut for three screens)" : "") << std::endl;
            }
            if (!screensWindow)
            {
#endif
            if (const char *size = std::getenv("GT3_WINDOW_SIZE"); size && *size)
            {
                int w = 0, h = 0;
                if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 224)
                {
                    SetWindowSize(w, h);
                    const int monitor = GetCurrentMonitor();
#if defined(PS2X_GAME_GT3)
                    SetWindowPosition(std::max(0, (GetMonitorWidth(monitor) - w) / 2), std::max(0, (GetMonitorHeight(monitor) - h) / 2));
#else
                    // On that monitor (its desktop position: not every monitor starts at 0,0).
                    const Vector2 origin = GetMonitorPosition(monitor);
                    SetWindowPosition(static_cast<int>(origin.x) + std::max(0, (GetMonitorWidth(monitor) - w) / 2),
                                      static_cast<int>(origin.y) + std::max(0, (GetMonitorHeight(monitor) - h) / 2));
#endif
                }
            }
            if (const char *full = std::getenv("GT3_FULLSCREEN"); full && *full == '1')
                ToggleBorderlessWindowed();
#if !defined(PS2X_GAME_GT3)
            else if (full && std::strcmp(full, "exclusive") == 0)
            {
                // GT3_FULLSCREEN=exclusive (Full launcher, developer 2026-10-09: "full screen, windowed, borderless windowed"):
                // the monitor taken over at its own desktop size and refresh (GLFW's full-screen window); Esc and Alt+Enter
                // leave it for a window as the borderless one does.
                const int monitor = GetCurrentMonitor();
                SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
                ToggleFullscreen();
                std::cerr << "[window] full screen (exclusive) on monitor " << (monitor + 1) << " '" << GetMonitorName(monitor) << "' "
                          << GetMonitorWidth(monitor) << "x" << GetMonitorHeight(monitor) << " " << GetMonitorRefreshRate(monitor) << " Hz" << std::endl;
            }
#endif
            else if (full && std::strcmp(full, "span") == 0)
            {
                // GT3_FULLSCREEN=span: one borderless window over every
                // monitor's area (triple screens side by side; with
                // GT3_ASPECT set to their combined shape, e.g. 48:9).
                int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
                for (int m = 0; m < GetMonitorCount(); ++m)
                {
                    const Vector2 p = GetMonitorPosition(m);
                    x0 = std::min(x0, static_cast<int>(p.x));
                    y0 = std::min(y0, static_cast<int>(p.y));
                    x1 = std::max(x1, static_cast<int>(p.x) + GetMonitorWidth(m));
                    y1 = std::max(y1, static_cast<int>(p.y) + GetMonitorHeight(m));
                }
                if (x1 > x0 && y1 > y0)
                {
                    SetWindowState(FLAG_WINDOW_UNDECORATED);
                    SetWindowPosition(x0, y0);
                    SetWindowSize(x1 - x0, y1 - y0);
                    std::cerr << "[window] spanning " << GetMonitorCount() << " monitor(s): " << (x1 - x0) << "x" << (y1 - y0) << " at " << x0
                              << "," << y0 << std::endl;
                }
            }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            }
#endif
        }
#endif
        if (!IsWindowReady())
        {
            std::cerr << "Graphics window initialization failed; check the active display and OpenGL driver."
                      << std::endl;
            return false;
        }
#if defined(PS2X_GS_VK)
        // GT3_VK_TEST=1: a few seconds of a Vulkan-rendered image shown by
        // OpenGL (gs_vk_context.h). GT3_VK_TEST_SHOT=<png> saves one frame.
        // GT3_VK_TEST=shared (diagnostics): the interop import and one frame on
        // the renderer's shared OpenGL context instead of the window's.
        if (const char *vkTest = std::getenv("GT3_VK_TEST"); vkTest && std::strcmp(vkTest, "shared") == 0 && gsgl::createSharedContext() &&
                                                            gsgl::makeSharedContextCurrent())
        {
            if (gsvk::testInit(640, 448))
            {
                gsvk::testFrame(1.0f);
                std::cerr << "[vk] test on the shared context: OpenGL read back " << gsvk::testReadbackNonZero() << " non-zero samples" << std::endl;
                gsvk::testFrameDone();
                gsvk::testShutdown();
            }
            gsgl::makeMainContextCurrent();
        }
        if (const char *vkTest = std::getenv("GT3_VK_TEST"); vkTest && *vkTest == '1')
        {
            const int w = 1280, h = 896;
            if (gsvk::testInit(w, h))
            {
                const auto start = std::chrono::steady_clock::now();
                int frames = 0;
                while (!WindowShouldClose())
                {
                    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
                    if (t > 4.0f)
                        break;
                    const unsigned id = gsvk::testFrame(t);
                    BeginDrawing();
                    ClearBackground(BLACK);
                    if (id)
                    {
                        const Texture2D tex{id, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                        DrawTexturePro(tex, {0, 0, static_cast<float>(w), static_cast<float>(h)},
                                       {0, 0, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())}, {0, 0}, 0.0f, WHITE);
                    }
                    DrawText(TextFormat("Vulkan -> OpenGL: %s", gsvk::deviceName()), 20, 20, 20, WHITE);
                    EndDrawing();
                    gsvk::testFrameDone();
                    if (++frames == 120)
                        std::cerr << "[vk] test: OpenGL read back " << gsvk::testReadbackNonZero() << " non-zero samples" << std::endl;
                    if (frames == 120)
                        if (const char *shot = std::getenv("GT3_VK_TEST_SHOT"); shot && *shot)
                        {
                            Image image = LoadImageFromScreen();
                            ExportImage(image, shot);
                            UnloadImage(image);
                        }
                }
                std::cerr << "[vk] test: " << frames << " frames in 4 s" << std::endl;
                std::cerr << "[vk] GS shaders for Vulkan: " << (gsvk::testCompileGsShaders() ? "all compile" : "FAILED (see above)") << std::endl;
                gsvk::testShutdown();
            }
            else
                std::cerr << "[vk] test: Vulkan could not start (see above)" << std::endl;
        }
#endif
        enableGpuRendererIfRequested();
#if defined(PS2X_GS_VK)
        // GT3_VKGL_PREPARE=1 (a launcher's "Preparing shaders" step): the Vulkan
        // renderer's pipelines into its cache file, then the end, before any game
        // code ran (gs_vk_backend.h).
        if (gsvk::pipelinePrepareRequested())
        {
            const int code = gsvk::pipelinePrepareRun();
            std::cout.flush();
            std::cerr.flush();
            std::_Exit(code);
        }
#endif
#if !defined(PLATFORM_VITA)
        // --headless: no audio device (sound runs on the wall clock and is
        // discarded, as in --cpu-only).
        if (!m_headless)
        {
            InitAudioDevice();
            m_audioBackend.setAudioReady(IsAudioDeviceReady());
        }
#endif
        if (m_headless)
        {
            stopIfHeadlessWindowShown("renderer started");
            const char *glRenderer = nullptr;
#if defined(PS2X_GS_GL)
            using GetStringFn = const unsigned char *(__stdcall *)(unsigned int);
            if (auto getString = reinterpret_cast<GetStringFn>(gsgl::getProcAddress("glGetString")))
                glRenderer = reinterpret_cast<const char *>(getString(0x1F01 /* GL_RENDERER */));
#endif
            std::cerr << "[headless] hidden window, OpenGL " << (glRenderer ? glRenderer : "?") << ", no audio device, no host input"
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
                      << ", " << headlessSwapMode
#endif
                      << std::endl;
        }
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
#if defined(PS2X_GAME_GT3)
    m_gt3MovieWorker.reset();
    m_gt3CoreVerified = false;
#endif
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;
    bool callbackArenaOverlapsElf = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
            callbackArenaOverlapsElf |= physAddr < kCallbackStackTop && segmentMemEnd > kCallbackStackFloor;
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = kCallbackStackFloor;
        // Fail callback allocation explicitly for an incompatible ELF layout.
        m_asyncCallbackStackTop = callbackArenaOverlapsElf ? kCallbackStackFloor : kCallbackStackTop;
        m_asyncCallbackStackLimit = m_asyncCallbackStackTop;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string iopError;
#if defined(PS2X_GAME_GT4)
    // Before the profile exists: LGDEV asks for its host wheel when it is created, and POWOFF's
    // power-off sequence ends with this runtime's quit.
    ps2_gt4_glue::beforeIopProfile(*this);
#endif
    if (!m_iopSubsystem->configure(identity, &iopError))
    {
        std::cerr << "[ps2xIOP] failed to configure profile: " << iopError << std::endl;
        return false;
    }

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);
#if defined(PS2X_GAME_GT4)
    // CORE's libmrpc answered natively when the LAN network is on (GT4_NET / GT4_NET_MRPC).
    ps2_gt4_glue::afterLoad(*this, m_cpuContext.pc, elfCrc32Valid ? elfCrc32 : 0u);
    // GT4's Video Output line (analysis/design/480p-colour/DESIGN.md; developer: hidden unless the launcher says
    // "GT4's own Options"): with GT4_RACE_MODE=480p and the CORE words its hooks rely on, every race runs in
    // 480p, so GT4's Options > Screen Settings (also from inside a race) is served without the line (the IOP's
    // script overlay, video-output: one value in option.adc, checked against the disc's SHA-256; the save is
    // never touched). GT4_RACE_MODE=game, the development test switch GT4_RACE_VIDEO_MODE or another CORE:
    // shown, as on a PS2. Said here, before GT4 runs and reads its disc.
    {
        const bool hide = gt4RaceMode480p() && !gt4RaceModeTestSwitch() && gt4RaceModeGuards(m_memory.getRDRAM());
        ps2x::iop::gt4::cheats::setVideoOutputHidden(hide);
        std::fprintf(stderr, "[gt4-video] GT4's Video Output line (Options > Screen Settings): %s\n",
                     hide ? "hidden (GT4_RACE_MODE=480p: every race in 480p)"
                          : !gt4RaceMode480p() ? "shown (GT4_RACE_MODE=game)"
                          : gt4RaceModeTestSwitch() ? "shown (test switch GT4_RACE_VIDEO_MODE)" : "shown (CORE does not match)");
    }
#endif

#if defined(PS2X_GAME_GT3)
    // Fixed USA guest addresses are enabled only for the original, exact image.
    // Read the already-open file, not a second pathname that could change.
    if (fileSize == 2442004 && m_cpuContext.pc == 0x100008) {
        const std::array<uint8_t, 32> expected{
            0xd0,0x18,0x88,0x95,0x87,0x12,0xbd,0xa1,0x21,0x06,0xa7,0x99,0xdf,0x79,0x00,0x2a,
            0xec,0x06,0x3e,0xea,0x10,0x17,0xce,0xe0,0x8c,0x95,0x1d,0x36,0x13,0xb9,0x18,0x8c};
        std::vector<uint8_t> image(static_cast<size_t>(fileSize)); std::array<uint8_t, 32> digest{};
        file.clear(); file.seekg(0, std::ios::beg);
        if (file.read(reinterpret_cast<char*>(image.data()), fileSize) &&
            m_iopHost->sha256(image, digest) && digest == expected) {
            m_gt3CoreVerified = true;
            std::cerr << "[GT3] verified CORE; native IPU still-image decode enabled\n";
            if (ps2x::video::MpegVideoDecoder::available()) {
                m_gt3MovieWorker = std::make_unique<ps2x::video::Gt3MovieWorker>(*this);
                std::cerr << "[GT3 native movie] verified CORE; decoder/input hooks enabled\n";
            }
        }
    }
#endif
#if defined(PS2X_GAME_GT4)
    gt4movie::installFeedCopy(*this); // movie stutter fix (analysis/design/movie-stutter/REPORT.md)
#endif
    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4_MC0_FOLDER=<folder>: the folder card of memory card slot 1 (default <CORE.elf folder>\mc0; slot 2's folder is
    // its neighbour mc1). GT4 Multi Monitor Mode gives each side copy its own copy of the player's card this way.
    if (const char *folder = std::getenv("GT4_MC0_FOLDER"); folder && *folder)
    {
        paths.mcRoot = normalizeAbsolutePath(std::filesystem::path(folder));
        std::cerr << "[mc] GT4_MC0_FOLDER: memory card slot 1 is the folder " << paths.mcRoot.string() << std::endl;
    }
#endif

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}


namespace
{
// Debug: GT3_EE_CALLWATCH=addr (hex). At every guest call entry and return the
// float at addr is checked; the first boundary where it is non-finite or beyond
// 1e6 in magnitude is logged with the 32 boundaries before it, so the write lies
// between the last good boundary and that one.
struct CallWatch
{
    bool enabled = false, fired = false;
    uint32_t address = 0;
    uint64_t from = 0; // GT3_EE_CALLWATCH_FROM: first VSync to check
    struct Boundary
    {
        const char *kind;
        uint32_t target, source, ra;
        float value;
        uint64_t tick;
    };
    std::array<Boundary, 32> ring{};
    size_t next = 0, filled = 0;
};
CallWatch &callWatch()
{
    static CallWatch w = [] {
        CallWatch value;
        const char *spec = std::getenv("GT3_EE_CALLWATCH");
        if (spec && std::sscanf(spec, "%x", &value.address) == 1 && value.address + 4 <= PS2_RAM_SIZE)
        {
            value.enabled = true;
            if (const char *from = std::getenv("GT3_EE_CALLWATCH_FROM"))
                value.from = std::strtoull(from, nullptr, 10);
            std::cerr << "[ee-callwatch] 0x" << std::hex << value.address << std::dec << std::endl;
        }
        return value;
    }();
    return w;
}
void callWatchCheck(uint8_t *rdram, const R5900Context *ctx, const char *kind, uint32_t target, uint32_t source, uint32_t ra, uint64_t tick)
{
    CallWatch &w = callWatch();
    if (!w.enabled || w.fired || !rdram || tick < w.from)
        return;
    float value;
    std::memcpy(&value, rdram + w.address, 4);
    w.ring[w.next] = {kind, target, source, ra, value, tick};
    w.next = (w.next + 1) % w.ring.size();
    w.filled = std::min(w.filled + 1, w.ring.size());
    if (std::isfinite(value) && std::fabs(value) <= 1e6f)
        return;
    w.fired = true;
    std::cerr << "[ee-callwatch] 0x" << std::hex << w.address << std::dec << " went bad (" << value << ") at VSync "
              << tick << "; boundaries, oldest first:\n";
    for (size_t k = 0; k < w.filled; ++k)
    {
        const auto &b = w.ring[(w.next + w.ring.size() - w.filled + k) % w.ring.size()];
        std::cerr << "[ee-callwatch]   " << b.kind << " target=0x" << std::hex << b.target << " source=0x" << b.source
                  << " ra=0x" << b.ra << std::dec << " value=" << b.value << '\n';
    }
    std::cerr << "[ee-callwatch]   gpr" << std::hex;
    for (int r : {2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 29, 31})
        std::cerr << " r" << std::dec << r << std::hex << "=" << getRegU32(ctx, r);
    std::cerr << std::dec << std::endl;
}
// Either dispatch debug hook enabled (read once at startup): the hot dispatch
// path then costs one predictable branch when both are off.
const bool g_dispatchDebug = std::getenv("GT3_EE_CALLWATCH") != nullptr || std::getenv("GT3_EE_TRACEFN") != nullptr;
// Debug: GT3_EE_TRACEFN=addr (hex) logs a0-a3, v0 and f0-f31 at each entry and
// return of that guest function, from VSync GT3_EE_TRACEFN_FROM (decimal), at
// most GT3_EE_TRACEFN_LIMIT (default 200) calls.
struct TraceFn
{
    uint32_t address = 0;
    uint64_t from = 0;
    int remaining = 0;
};
TraceFn &traceFn()
{
    static TraceFn t = [] {
        TraceFn value;
        const char *spec = std::getenv("GT3_EE_TRACEFN");
        if (spec && std::sscanf(spec, "%x", &value.address) == 1)
        {
            const char *from = std::getenv("GT3_EE_TRACEFN_FROM");
            const char *limit = std::getenv("GT3_EE_TRACEFN_LIMIT");
            value.from = from ? std::strtoull(from, nullptr, 10) : 0;
            value.remaining = limit ? std::atoi(limit) : 200;
        }
        return value;
    }();
    return t;
}
void traceFnLog(const R5900Context *ctx, const char *kind, uint64_t tick)
{
    std::cerr << "[ee-tracefn] " << kind << " 0x" << std::hex << traceFn().address << std::dec << " vsync " << tick
              << std::hex;
    for (int r : {2, 4, 5, 6, 7})
        std::cerr << " r" << std::dec << r << std::hex << "=" << getRegU32(ctx, r);
    std::cerr << std::dec;
    for (int f = 0; f < 32; ++f)
        std::cerr << " f" << f << "=" << ctx->f[f];
    std::cerr << std::endl;
}
} // namespace

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // (HUD hand-off 7 C: a suspended bracketed call returning through a dispatched jump: its bracket closed now)
    if (!isCall && gt4hudpic::pendingReturns())
        gt4hudpic::atDispatch(rdram, targetPc, GPR_U32(ctx, 29));
#endif
    // GT3_CALL_TRACE=<hex address>: counts the call sites of that function and
    // logs them (with the caller's a0 and the returned vsync tick) every 5 s.
    static const uint32_t callTraceTarget = [] {
        const char *v = std::getenv("GT3_CALL_TRACE");
        return v && *v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0u;
    }();
#if defined(PS2X_GAME_GT4)
    // GT4_CALL_PROFILE=<from vsync>: counts guest calls by target from that
    // VSync on and prints the 40 most called every 600 VSyncs.
    {
        static const uint64_t profileFrom = [] {
            const char *v = std::getenv("GT4_CALL_PROFILE");
            return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
        }();
        if (profileFrom != ~0ull && isCall)
        {
            const uint64_t tick = m_memory.gs().vsyncTick.load();
            if (tick >= profileFrom)
            {
                static std::mutex profileMutex;
                static std::unordered_map<uint32_t, uint64_t> counts;
                static uint64_t lastDump = 0;
                std::lock_guard<std::mutex> lock(profileMutex);
                ++counts[targetPc];
                if (tick >= lastDump + 600)
                {
                    lastDump = tick;
                    std::vector<std::pair<uint64_t, uint32_t>> top;
                    for (const auto &[pc, n] : counts)
                        top.emplace_back(n, pc);
                    std::sort(top.rbegin(), top.rend());
                    std::fprintf(stderr, "[call-profile] vsync %llu:", static_cast<unsigned long long>(tick));
                    for (size_t i = 0; i < top.size() && i < 40; ++i)
                        std::fprintf(stderr, " %x=%llu", top[i].second, static_cast<unsigned long long>(top[i].first));
                    std::fputc('\n', stderr);
                    counts.clear();
                }
            }
        }
    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // Holding the start (gt4_start_hold.h, GT4_START_HOLD; HUD hand-off 7 E): GT4's grid countdown kept at 1, then let go, before
    // the frame's step; and, in free mode, your car let go after the dynamics' hold loop (its first per-car pass, 0x357700 from
    // 0x34de1c).
    if (isCall && (targetPc == 0x389638u || (targetPc == 0x357700u && sourcePc == 0x34de1cu)) && gt4start::wanted())
    {
        if (targetPc == 0x389638u)
            gt4start::onRaceFrame(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), m_memory.gs().vsyncTick.load());
        else
            gt4start::afterHoldLoop(rdram);
    }
    // The race reader (gt4_race_reader.h; HUD and telemetry): every race frame's state copied out, read-only.
    if (isCall && targetPc == 0x389638u)
    {
        gt4race::onRaceFrame(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), m_memory.gs().vsyncTick.load());
    }
    // GT4_HUD_HIDE (gt4_hud.h): parts of GT4's own race HUD left out, just before RaceDisplay's draw (a0 the display).
    if (isCall && targetPc == 0x39f498u)
    {
        gt4hud::beforeRaceDisplayDraw(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)));
        // The launcher's race pictures (gt4_hud_pictures.h): a capture armed / marked at RaceDisplay's draw.
        gt4hudpic::setScratchpad(m_memory.getScratchpad());
        gt4hudpic::raceDisplayDraw(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), m_memory.gs().vsyncTick.load());
    }
#endif
    // GT4_RACE_WATCH=<every n VSyncs>: on the race frame update 0x389638
    // (a0 = race object R) logs the start gates and the first cars' state
    // (analysis/phase4/specs/race-start.md).
    {
        static const uint64_t raceEvery = [] {
            const char *v = std::getenv("GT4_RACE_WATCH");
            return v && *v ? std::max<uint64_t>(1, std::strtoull(v, nullptr, 10)) : 0ull;
        }();
        if (raceEvery && isCall && targetPc == 0x389638u)
        {
            static uint64_t lastTick = ~0ull;
            const uint64_t tick = m_memory.gs().vsyncTick.load();
            if (lastTick == ~0ull || tick / raceEvery != lastTick / raceEvery)
            {
                lastTick = tick;
                auto u32 = [rdram](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
                    return v;
                };
                auto f32 = [&](uint32_t a) {
                    const uint32_t v = u32(a);
                    float f;
                    std::memcpy(&f, &v, 4);
                    return f;
                };
                auto u8 = [rdram](uint32_t a) { return rdram[a & 0x01ffffffu]; };
                auto s16 = [rdram](uint32_t a) {
                    int16_t v = 0;
                    std::memcpy(&v, rdram + (a & 0x01fffffeu), 2);
                    return v;
                };
                const uint32_t r = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
                const uint32_t o = u32(r + 0x6c), d = u32(r + 0x70);
                // Both lines formatted first and written in one call: written
                // piece by piece, other threads' stderr lines landed inside the
                // car row (analysis/phase5/specs/step6-coverage.md B7).
                char record[2048];
                size_t used = 0;
                auto advance = [&](int n) {
                    if (n > 0)
                        used = std::min(sizeof(record) - 1u, used + static_cast<size_t>(n));
                };
                advance(std::snprintf(record, sizeof(record),
                                      "[race] vsync %llu R=%x O=%x D=%x | O.b0=%x b4=%x b8=%x bc=%d ev=%u | R.d4c=%x d38=%x d3c=%g d48=%x d50=%x cd8=%x d30=%x | hud=%02x%02x%02x\n",
                                      static_cast<unsigned long long>(tick), r, o, d, u32(o + 0xb0), u32(o + 0xb4), u32(o + 0xb8),
                                      static_cast<int32_t>(u32(o + 0xbc)), u8(o + 0x900), u32(r + 0xd4c), u32(r + 0xd38), f32(r + 0xd3c),
                                      u32(r + 0xd48), u32(r + 0xd50), u32(r + 0xcd8), u32(r + 0xd30), u8(0x621514), u8(0x621515), u8(0x621516)));
                if (d)
                {
                    const uint32_t cars = std::min<uint32_t>(u32(d + 0xf8a0), 6u);
                    advance(std::snprintf(record + used, sizeof(record) - used, "[race]   cars=%u", u32(d + 0xf8a0)));
                    for (uint32_t i = 0; i < cars; ++i)
                    {
                        const uint32_t c = d + 0x6698 + i * 0x10a0;
                        advance(std::snprintf(record + used, sizeof(record) - used,
                                              " | c%u hold=%x dly=%d cfg=%d pos=(%.2f,%.2f,%.2f) prog=%g vel=(%g,%g) 56b=%u", i,
                                              u32(c + 0x5c0), static_cast<int32_t>(u32(d + 0xcbfc + i * 4)),
                                              s16(d + 0xf878 + i * 2), f32(c + 0x104), f32(c + 0x108),
                                              f32(c + 0x10c), f32(c + 0x5a8), f32(c + 0x640), f32(c + 0x644), u8(c + 0x56b)));
                    }
                    advance(std::snprintf(record + used, sizeof(record) - used, "\n"));
                }
                std::fputs(record, stderr);
            }
        }
    }
    // GT4_AI_PILOT=1 (test laps only, default off; analysis/phase5/specs/autopilot-design.md
    // option A): on the race frame update 0x389638 (a0 = race R), at the first
    // race frame after the control handover (the player car's AutoDrive
    // car+0x56b went 1 -> 0; on a grid start, once the race-start event set
    // R+0xd4c), the player car's driver mode car+0x56a 0 -> 1 hands it to GT4's
    // own B-Spec AI. The race stays A-spec (camera, HUD and pad layout follow
    // R+0xcc8, left at 0). Acts only in a live A-spec race (R+0xcc8 == 0) on the
    // car whose controller is an active RaceInput (vptr 0x686618 at +0xd0,
    // +0x1cc != 0), once per race; off if CORE's words at 0x389638 / 0x349fd0
    // are not the expected ones. Re-arms (spec 3.1) when R changes, when the
    // player car changes, or when its mode returns to 0 after it was handed
    // over (a race restart: car init 0x363b10 stores the mode); a re-armed grid
    // start then waits for the restarted race's start event (R+0xd4c seen 0,
    // then set), and a rolling start for its AutoDrive again.
    {
        static const bool aiPilot = [] {
            const char *v = std::getenv("GT4_AI_PILOT");
            return v && *v && *v != '0';
        }();
        if (aiPilot && isCall && targetPc == 0x389638u)
        {
            constexpr uint32_t kRam = 0x02000000u;
            auto u32 = [rdram](uint32_t a) {
                uint32_t v = 0;
                std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
                return v;
            };
            auto f32 = [&](uint32_t a) {
                const uint32_t v = u32(a);
                float f;
                std::memcpy(&f, &v, 4);
                return f;
            };
            static int coreOk = -1;
            if (coreOk < 0)
            {
                coreOk = (u32(0x389638u) == 0x27bdffe0u && u32(0x349fd0u) == 0x27bdff80u) ? 1 : 0;
                std::fprintf(stderr, coreOk ? "[gt4-ai-pilot] armed (GT4_AI_PILOT): the player car goes to GT4's AI at the control handover\n"
                                            : "[gt4-ai-pilot] CORE does not match (0x389638 / 0x349fd0): off\n");
            }
            static uint32_t raceR = 0, pilotCar = 0;
            // needStartEdge: re-armed at the same R, whose race-start event
            // R+0xd4c may still hold the last race's 1 (it is set once, by
            // 0x389d48): a grid start waits to see it 0 again first.
            static bool sawAutoDrive = false, engaged = false, needStartEdge = false;
            const uint32_t r = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
            if (coreOk == 1 && r != 0u && r < kRam)
            {
                if (r != raceR)
                {
                    raceR = r;
                    pilotCar = 0;
                    sawAutoDrive = engaged = needStartEdge = false;
                }
                const uint32_t o = u32(r + 0x6c) & 0x1fffffffu, d = u32(r + 0x70) & 0x1fffffffu;
                uint32_t car = 0;
                int player = -1;
                if (u32(r + 0xcc8) == 0u && o != 0u && o < kRam && d != 0u && d < kRam)
                {
                    const uint32_t cars = std::min<uint32_t>(u32(d + 0xf8a0), 8u);
                    for (uint32_t i = 0; i < cars && player < 0; ++i)
                    {
                        const uint32_t c = d + 0x6698u + i * 0x10a0u;
                        const uint32_t ctrl = u32(c + 0xc) & 0x1fffffffu;
                        if (ctrl != 0u && ctrl < kRam && u32(ctrl + 0xd0) == 0x686618u && u32(ctrl + 0x1cc) != 0u)
                        {
                            player = static_cast<int>(i);
                            car = c;
                        }
                    }
                }
                if (player >= 0)
                {
                    const uint8_t mode = rdram[(car + 0x56a) & 0x01ffffffu];
                    const uint8_t autoDrive = rdram[(car + 0x56b) & 0x01ffffffu];
                    const uint32_t startEvent = u32(r + 0xd4c);
                    const char *rearm = nullptr;
                    if (pilotCar != 0u && car != pilotCar)
                        rearm = "another player car";
                    else if (engaged && mode == 0u)
                        rearm = "the car's mode is back to 0 (race restart)";
                    pilotCar = car;
                    if (rearm)
                    {
                        sawAutoDrive = engaged = false;
                        needStartEdge = true;
                        std::fprintf(stderr, "[gt4-ai-pilot] vsync %llu: re-armed: %s R=%x car=%x R+0xd4c=%u\n",
                                     static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), rearm, r, car, startEvent);
                    }
                    if (needStartEdge && startEvent == 0u)
                        needStartEdge = false; // the restarted race's start event is still to come
                    if (autoDrive == 1u)
                    {
                        sawAutoDrive = true;
                        engaged = false; // a (re)started rolling start: arm again
                    }
                    const bool gridStarted = startEvent != 0u && !needStartEdge;
                    if (!engaged && mode == 0u && autoDrive == 0u && u32(o + 0xbc) == 0u && (sawAutoDrive || gridStarted))
                    {
                        rdram[(car + 0x56a) & 0x01ffffffu] = 1u;
                        engaged = true;
                        std::fprintf(stderr,
                                     "[gt4-ai-pilot] vsync %llu: car %d handed to GT4's AI (+0x56a 0 -> 1) R=%x car=%x prog=%.2f speed=%.2f "
                                     "idle-quit O+0xd0=%u (%s)\n",
                                     static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), player, r, car, f32(car + 0x5a8),
                                     f32(car + 0x640), u32(o + 0xd0), sawAutoDrive ? "after the rolling-start handover" : "grid start");
                    }
                }
            }
        }
    }
#if !defined(GT4_STOCK_BUILD)
    // Custom Sounds (GT4_SOUND_PACK / GT4_SOUND_PACK_MAP; not in the Classic build): each race frame hands the cars'
    // revs, throttle, gear and car code to the sound module (ps2x/iop/gt4_car_state.h). Nothing happens without a key.
    if (isCall && targetPc == 0x389638u && ps2x::iop::gt4::carstate::wanted())
        ps2x::iop::gt4::carstate::onRaceFrame(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)),
                                              m_memory.gs().vsyncTick.load());
#endif
    // GT4's video modes (analysis/design/video-modes/VIDEO-MODES.md 1.1):
    // applyRaceDisplayMode 0x436640 (a0 = Option) reads race_display_mode
    // Option+0x16b0 (0 480i, 1 480p, 2 1080i) and switches the video mode as a
    // race starts; applyMenuDisplayMode 0x436740 goes back to 480i. Each call
    // is logged ([gt4-video]). Test switches (development builds only, never
    // in the stock build; headless runs without menu replays):
    //   GT4_RACE_VIDEO_MODE=480i|480p|1080i  sets Option+0x16b0 on entry, as
    //                                        if chosen in GT4's Options
    //   GT4_RACE_VIDEO_BLEND=0|1             sets Option+0x20 on entry (1:
    //                                        1080i's two-circuit softening,
    //                                        VIDEO-MODES.md 1.5)
    // The values go into the game's Option object (as the Options screen
    // would put them): a save made afterwards keeps Option+0x20 and the aspect
    // (test cards only); race_display_mode is never saved (inv-mode.md 1.3).
    // GT4_SCREEN_ASPECT=16:9|4:3 (test switch, development builds only): GT4's
    // Aspect Ratio as Options > Screen Settings sets it (Option+0x30 / +0x34,
    // 16 / 9 or 4 / 3), at the options apply 0x4365b8 (a0 = Option), which
    // derives the wide flag 0x6184f0 from it (Gt4WideFlag).
#if !defined(GT4_STOCK_BUILD)
    if (isCall && targetPc == 0x4365b8u)
    {
        static const int aspect = [] {
            const char *v = std::getenv("GT4_SCREEN_ASPECT");
            const std::string a = v ? v : "";
            return a == "16:9" ? 169 : a == "4:3" ? 43 : 0;
        }();
        uint32_t first = 0;
        std::memcpy(&first, rdram + 0x4365b8u, 4);
        const uint32_t option = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
        if (aspect && first == 0x27bdffe0u && option != 0u && option + 0x38u < 0x02000000u)
        {
            const float w = aspect == 169 ? 16.0f : 4.0f, h = aspect == 169 ? 9.0f : 3.0f;
            std::memcpy(rdram + ((option + 0x30u) & 0x01fffffcu), &w, 4);
            std::memcpy(rdram + ((option + 0x34u) & 0x01fffffcu), &h, 4);
            std::fprintf(stderr, "[gt4-video] vsync %llu: test switch GT4_SCREEN_ASPECT: Option+0x30/0x34 = %g/%g at the options apply\n",
                         static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), w, h);
        }
    }
#endif
    // GT4's video modes for players (analysis/design/480p-colour/DESIGN.md D1,
    // D5, D6; inv-mode.md, inv-more.md). Classic settings (gt4-start.env), in
    // development and Classic builds alike:
    //   GT4_RACE_MODE=480p|game (unset or empty: 480p): GT4's own Progressive
    //     (480p) mode for every race session (pre-race screen, race, results,
    //     GT4's replay, race again, Replay Theater, title demo), as if chosen
    //     in GT4's Options > Screen Settings > Video Output, without GT4's
    //     15-second test. In development and Full builds GT4's menus, car
    //     select and garage are its own 480p too, from the title on (D5,
    //     analysis/design/menus-480p; GT4_MENU_MODE=480i: GT4's own 480i
    //     menus); the Classic build keeps GT4's 480i menus. game: GT4's own
    //     Video Output, as on a PS2.
    //   GT4_ASPECT=16:9|4:3|game (unset or empty: 16:9): GT4's own 16:9 (or
    //     4:3) state while it draws (wide flag, the race view's geometry and
    //     wide_mode); game: GT4's own Aspect Ratio. Development and Full
    //     builds also take 21:9, 32:9 and 48:9 (one screen): GT4's 16:9 state
    //     with its race view widened (full-m2 "Wide"; the Classic build reads
    //     them as 16:9).
    // Neither changes the player's save. In development builds the test
    // switches GT4_RACE_VIDEO_MODE and GT4_SCREEN_ASPECT win (old test routes
    // keep their baselines). Each part checks the CORE words it relies on once;
    // any other CORE leaves GT4's own choices, with one log line.
#if !defined(GT4_STOCK_BUILD)
    // GT4_MM_GEOMETRY taken off (and the player's LAN options put back before an Option save) before GT4_ASPECT's
    // bracket below restores the player's geometry (gt4_multimon.h).
    if (isCall && gt4mm::config().on)
        gt4mm::beforeAspect(rdram, targetPc, sourcePc, m_memory.gs().vsyncTick.load());
#endif
    if (isCall && (targetPc == 0x436640u || targetPc == 0x4373f0u || targetPc == 0x436740u || targetPc == 0x1074d0u ||
                   targetPc == 0x108040u || targetPc == 0x107368u || targetPc == 0x4390d0u || targetPc == 0x439288u ||
                   targetPc == 0x4374a8u || sourcePc == 0x388b30u || targetPc == 0x39bb38u || targetPc == 0x389638u ||
                   targetPc == 0x393790u || targetPc == 0x44e390u || targetPc == 0x497af0u || sourcePc == 0x33f0a4u ||
                   sourcePc == 0x33f080u || targetPc == 0x3c3820u || targetPc == 0x105338u || targetPc == 0x1d6378u ||
                   targetPc == 0x1052e0u || targetPc == 0x105528u || sourcePc == 0x109f44u || targetPc == 0x4a7c60u ||
                   targetPc == 0x1059f8u))
    {
        auto u32 = [rdram](uint32_t a) {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
            return v;
        };
        auto w32 = [rdram](uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01fffffcu), &v, 4); };
        auto lower = [](const char *v) {
            std::string m = v ? v : "";
            for (char &c : m)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            m.erase(0, m.find_first_not_of(" \t"));
            m.erase(m.find_last_not_of(" \t") + 1);
            return m;
        };
        const bool mode480p = gt4RaceMode480p();
        // 169: 16:9, 43: 4:3, 0: game; development and Full builds: 219 (21:9), 329 (32:9), 489 (48:9 on one screen).
        static const int aspect = [&lower] {
            const char *v = std::getenv("GT4_ASPECT");
            const std::string m = lower(v);
#if !defined(GT4_STOCK_BUILD)
            // Three screens (gt4screens): GT4's 16:9 with the race view as wide as the three screens (the 48:9 path with the
            // screens' own shape), whatever GT4_ASPECT says.
            if (gt4screens::config().on)
            {
                if (!m.empty() && m != "16:9" && m != "48:9")
                    std::fprintf(stderr, "[gt4-video] GT4_SCREENS=3: GT4_ASPECT=%s set aside (three screens: GT4's 16:9 menus, races across the screens)\n", v);
                return 489;
            }
#endif
            if (m == "game")
            {
                std::fprintf(stderr, "[gt4-video] GT4_ASPECT=game: GT4's own Aspect Ratio (Options > Screen Settings)\n");
                return 0;
            }
            if (m == "4:3")
                return 43;
#if !defined(GT4_STOCK_BUILD)
            if (m == "21:9")
                return 219;
            if (m == "32:9")
                return 329;
            if (m == "48:9")
                return 489;
#endif
            if (!m.empty() && m != "16:9")
                std::fprintf(stderr, "[gt4-video] GT4_ASPECT=%s not understood (16:9, 4:3 or game): 16:9\n", v);
            return 169;
        }();
        const bool modeTestSwitch = gt4RaceModeTestSwitch();
#if !defined(GT4_STOCK_BUILD)
        static const bool aspectTestSwitch = [] {
            const char *v = std::getenv("GT4_SCREEN_ASPECT");
            return v && *v;
        }();
#else
        constexpr bool aspectTestSwitch = false;
#endif
        const uint32_t gameOption = (u32(0x622f4cu) & 0x1fffffffu) + 0x38cb0u;

        // ---- D1 (inv-mode.md 2). A: at every race-session entry (StartSequence 0x1084c4: network, LAN and
        // rally battles, the demo race; the title demo 0x10befc; Replay Theater 0x10caac; the race functions
        // 0x110478, also after the in-race Options; leaving the replay photo screen 0x3c4c0c)
        // race_display_mode (Option+0x16b0) is set to 1 (Progressive) before applyRaceDisplayMode 0x436640
        // reads it. B: the same when GT4's Options read it (getter 0x4373f0 from 0x1806b0), so they show
        // Progressive. It stays 1: the Race Monitor (0x3da150), the licence result screen (0x3e0e38) and
        // applyMenuDisplayMode's return value read it while racing. GT4 never saves it (the Option save 0x4374a8
        // stops at +0x16ac), and every start of GT4 begins at Normal (480i). GT4's own 15-second "Is this mode
        // satisfactory?" test (script stub, 0x189a04) is left alone.
        static const bool raceGuards = [rdram] {
            const bool ok = gt4RaceModeGuards(rdram);
            if (!ok)
                std::fprintf(stderr, "[gt4-video] CORE does not match: GT4_RACE_MODE not applied, GT4's own Video Output and 480i menus\n");
            return ok;
        }();
        const bool modeOn = mode480p && !modeTestSwitch && raceGuards;
        if (modeOn && ((targetPc == 0x436640u && (sourcePc == 0x1084c4u || sourcePc == 0x10befcu || sourcePc == 0x10caacu ||
                                                  sourcePc == 0x110478u || sourcePc == 0x3c4c0cu)) ||
                       (targetPc == 0x4373f0u && sourcePc == 0x1806b0u)))
        {
            const uint32_t option = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
            if (option == gameOption && option + 0x16b4u < 0x02000000u)
            {
                const uint32_t was = u32(option + 0x16b0u);
                if (was != 1u)
                    w32(option + 0x16b0u, 1u);
                static bool said = false;
                if (!said)
                    std::fprintf(stderr, "[gt4-video] vsync %llu: GT4_RACE_MODE=480p: races, replays and demos use GT4's 480p (GT4's Video Output was %u; GT4 does not save it)\n",
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), was);
                said = true;
            }
        }

        // ---- D5 (inv-more.md 5.3; DESIGN.md D5; analysis/design/menus-480p): GT4's menus in GT4's own 480p mode,
        // in development and Full builds while GT4_RACE_MODE is 480p (GT4_MENU_MODE=480i: GT4's own 480i menus, as
        // on a PS2; never in the Classic build). GT4 never ran its menus in 480p on a PS2, and three things needed
        // help: a switch during the boot's SCEA screen leaves GT4 on that screen for good (so H2 waits for the boot
        // project's end, below); its car views' render targets do not fit the GS memory at 448 lines (the "D5 car
        // picture" hooks keep them at 480i's 224 rows); and its menus' 3D uses a 16-bit Z buffer (drawn on the GPU:
        // gs_gl_backend.cpp Layout::Z16).
        // H1: applyMenuDisplayMode's setter call (0x1074d0 from 0x436764, after every race session, Photo Travel
        // and GT4's own test) asks for (CT16, id 6) instead of (CT24, id 0); with races in 480p too the setter finds
        // the mode unchanged and does nothing (no blank, no rebuild). H2: once GT4's sequence loop runs
        // (StartSequence 0x108040, called once from main at 0x107f40) and GT4's boot project has given way to the next
        // (below; or from VSync GT4_MENU_480P_FROM), whenever DisplaySwitching (0x6184d0) is idle in GT4's 480i field
        // mode, its update (0x107368 from the flip, 0x107938) is given what GT4's setter stores for (CT16, 6, 640, 448) (0x107570-0x10759c,
        // its 60-VSync countdown included): GT4 then switches to 480p by itself, as for its own 15-second Video
        // Output test while its menus run. DisplaySwitching+0x14 (the menus' 3D y scale) is left as the boot set it.
#if !defined(GT4_STOCK_BUILD)
        static const bool menuTestSwitch = [&lower] {
            const char *v = std::getenv("GT4_MENU_MODE");
            const std::string m = lower(v);
            if (m == "480i")
            {
                std::fprintf(stderr, "[gt4-video] GT4_MENU_MODE=480i: GT4's own 480i menus\n");
                return false;
            }
            if (!m.empty() && m != "480p")
                std::fprintf(stderr, "[gt4-video] GT4_MENU_MODE=%s not understood (480p or 480i): 480p\n", v);
            return true;
        }();
#else
        constexpr bool menuTestSwitch = false;
#endif
        static const bool menuGuards = [&u32] {
            const bool ok = u32(0x436750u) == 0x24050001u && u32(0x436754u) == 0x0000302du && u32(0x43675cu) == 0x24070280u &&
                            u32(0x436764u) == 0x0c041d34u && u32(0x436768u) == 0x240801c0u && u32(0x43668cu) == 0x24050002u &&
                            u32(0x436694u) == 0x24060006u && u32(0x43669cu) == 0x0c041d34u && u32(0x1074d0u) == 0x27bdffd0u &&
                            u32(0x107368u) == 0x27bdffe0u && u32(0x10737cu) == 0x8e020028u && u32(0x107390u) == 0x8e110024u &&
                            u32(0x107938u) == 0x0c041cdau && u32(0x107570u) == 0xae030028u && u32(0x107578u) == 0xae040000u &&
                            u32(0x107580u) == 0xae030004u && u32(0x107588u) == 0xae130038u && u32(0x10758cu) == 0xae040008u &&
                            u32(0x107590u) == 0xae070024u && u32(0x107594u) == 0xae12002cu && u32(0x107598u) == 0xae110030u &&
                            u32(0x10759cu) == 0xae140034u && u32(0x618648u) == 1u && u32(0x61864cu) == 1u && u32(0x618650u) == 2u &&
                            u32(0x618654u) == 1u && u32(0x618688u) == 6u && u32(0x61868cu) == 0u && u32(0x618690u) == 0x50u &&
                            u32(0x618694u) == 1u && u32(0x107f40u) == 0x0c042010u && u32(0x108040u) == 0x27bdff20u &&
                            u32(0x100be4u) == 0xae11009cu && u32(0x100bf4u) == 0xae0200a0u && u32(0x100b48u) == 0xae0000a0u &&
                            u32(0x109f40u) == 0x24a5ced0u && u32(0x109f44u) == 0x0c15f688u && u32(0x109f48u) == 0x00c0382du;
            if (!ok)
                std::fprintf(stderr, "[gt4-video] CORE does not match (DisplaySwitching): GT4's menus stay 480i\n");
            return ok;
        }();
        const bool menuOn = modeOn && menuTestSwitch && menuGuards;
        g_gt4Menus480p.store(menuOn, std::memory_order_relaxed);
        if (menuOn && targetPc == 0x1074d0u && sourcePc == 0x436764u)
        {
            SET_GPR_S32(ctx, 5, 2); // psm: PSMCT16
            SET_GPR_S32(ctx, 6, 6); // id 6: 480p (mode table 0x618688); 640 x 448 as given
        }
        static bool sequenceRunning = false;
        if (targetPc == 0x108040u && sourcePc == 0x107f40u)
            sequenceRunning = true;
#if !defined(GT4_STOCK_BUILD)
        // GT4_MENU_480P_FROM=<vsync> (diagnostics, development builds only): H2 not before that VSync, and from then on
        // without waiting for the boot's end below.
        static const uint64_t menuFrom = [] {
            const char *v = std::getenv("GT4_MENU_480P_FROM");
            return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
        }();
#else
        constexpr uint64_t menuFrom = ~0ull;
#endif
        // The boot's end: GT4 starts its projects by name (sprintf(path, "/projects/%s/%s", name, name), 0x57da20 from
        // 0x109f44: "boot", then "opening" or "title", ...). Switched while "boot" shows SCEA's screen, GT4 waits there
        // for good (m480-boot0: no memory-card check, no logo); switched after it, from the black at its end, the
        // Polyphony logo or later, GT4 goes on as in 480i (m480-from250, -from450, -from650). So H2 waits for the first
        // project after "boot".
        static bool pastBoot = false;
        if (targetPc == 0x57da20u && sourcePc == 0x109f44u)
        {
            char name[24] = {};
            const uint32_t a = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0)) & 0x01ffffffu;
            for (uint32_t i = 0; i + 1 < sizeof(name) && a + i < 0x02000000u && rdram[a + i]; ++i)
                name[i] = static_cast<char>(rdram[a + i]);
            if (menuOn)
                std::fprintf(stderr, "[gt4-video] vsync %llu: GT4 starts project \"%s\"\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), name);
            if (std::strcmp(name, "boot") != 0)
                pastBoot = true;
        }
        const bool menuArmed = menuFrom != ~0ull ? m_memory.gs().vsyncTick.load() >= menuFrom : pastBoot;
        // StartSequence's mode word (u32(*0x622f4c + 0x3a368 + 8), the sequence the loop 0x108040 runs: 12 is the 2 Player
        // Battle's split screen, inv-mode.md 1.2), read at the flip.
        uint32_t sequenceMode = ~0u;
        if (menuOn && sequenceRunning && targetPc == 0x107368u && sourcePc == 0x107938u)
        {
            const uint32_t g = u32(0x622f4cu) & 0x1fffffffu;
            sequenceMode = g ? u32(g + 0x3a368u + 8u) : ~0u;
            static uint32_t shown = ~0u;
            if (sequenceMode != shown)
            {
                std::fprintf(stderr, "[gt4-video] vsync %llu: GT4's sequence %u\n", static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()),
                             sequenceMode);
                shown = sequenceMode;
            }
        }
        // H3: GT4's 2 Player Battle never sets a video mode: on a PS2 its split screen runs in the menus' 480i fields,
        // and drawn in 480p whole frames come out as garbage (m480-v-2p, about one frame in six). While that sequence
        // runs, GT4's 480p goes back to its own 480i fields (what GT4's setter 0x1074d0 stores for (CT24, id 1, 640,
        // 448): the mode table entry 0x618648, its 60-VSync countdown included); H2 brings 480p back after it.
        if (sequenceMode == 12u)
        {
            const uint32_t o = 0x6184d0u, state = u32(o + 0x24u);
            if (u32(0x6d67a8u) == 0u && state != 1u && state != 2u && static_cast<int32_t>(u32(o + 0x28u)) <= 0 && u32(o + 0x30u) == 6u &&
                u32(o + 0x0u) == 0u && u32(o + 0x4u) == 0x50u && u32(o + 0x34u) == 640u && u32(o + 0x38u) == 448u)
            {
                w32(o + 0x28u, 60u);
                w32(o + 0x0u, u32(0x61864cu)); // interlace 1
                w32(o + 0x4u, u32(0x618650u)); // gsmode 2
                w32(o + 0x8u, u32(0x618654u)); // ffmd 1
                w32(o + 0x38u, 448u);
                w32(o + 0x24u, 1u);
                w32(o + 0x2cu, 1u); // psm CT24
                w32(o + 0x30u, 1u); // id 1
                w32(o + 0x34u, 640u);
                std::fprintf(stderr, "[gt4-video] vsync %llu: 2 Player Battle: GT4's 480p -> its own 480i fields, as on a PS2\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
            }
        }
        if (menuOn && sequenceRunning && menuArmed && sequenceMode != 12u && targetPc == 0x107368u && sourcePc == 0x107938u)
        {
            const uint32_t o = 0x6184d0u, state = u32(o + 0x24u);
            if (u32(0x6d67a8u) == 0u && state != 1u && state != 2u && static_cast<int32_t>(u32(o + 0x28u)) <= 0 && u32(o + 0x30u) == 1u &&
                u32(o + 0x0u) == 1u && u32(o + 0x4u) == 2u && u32(o + 0x8u) == 1u && u32(o + 0x34u) == 640u && u32(o + 0x38u) == 448u)
            {
                w32(o + 0x28u, 60u);           // the countdown when the GS mode changes (GT4's setter)
                w32(o + 0x0u, u32(0x61868cu)); // interlace 0
                w32(o + 0x4u, u32(0x618690u)); // gsmode 0x50
                w32(o + 0x8u, u32(0x618694u)); // ffmd 1
                w32(o + 0x38u, 448u);
                w32(o + 0x24u, 1u); // state 1: rebuild at this update
                w32(o + 0x2cu, 2u); // psm CT16
                w32(o + 0x30u, 6u); // id 6
                w32(o + 0x34u, 640u);
                std::fprintf(stderr, "[gt4-video] vsync %llu: GT4_RACE_MODE=480p: GT4's 480i fields -> GT4's own 480p, menus included\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
            }
        }

        // ---- D5 car picture (analysis/design/menus-480p/CAR-PICTURE.md): the car and colour select's 3D car in GT4's
        // 480p menus. GT4's car view (0x1d5c00) sizes its render targets from the frame descriptor (0x61850c /
        // 0x618548, chosen by 0x618584): at 640x448 the composite (0x105338 from 0x1d5fac) takes the GS memory the car
        // target (0x105338 from 0x1d5fc0) needs, GT4's allocator (0x4aa570) returns -1 and the car is drawn at FBP 0x1ff,
        // over the display and its depth (m480-plog2) -- GT4 never showed this scene in 480p on a PS2. While the menus are
        // 480p: C1 those two allocations see the frame descriptor in GT4's 480i field shape (640x224, field flag), so the
        // targets are exactly 480i's (0xf2, 0x138); it is put back at the passes (0x1d6378 from 0x1d5fdc), whose copies
        // stretch between the 448-line frame and the 224-row targets by the descriptors' own sizes (0x105ad8, 0x4a6788).
        // C2 the background blur's planes (0x1052e0 from 0x1d5714: frame width / 2 x height / 2) are 480i's 320x112.
        // The passes keep GT4's own 480p Z16 depth (drawn on the GPU: gs_gl_backend.cpp Layout::Z16).
#if !defined(GT4_STOCK_BUILD)
        static const int carPicture = [] {
            const char *v = std::getenv("GT4_MENU_480P_CAR"); // 0: off (development); default on
            return v && *v ? std::atoi(v) : 1;
        }();
        static const bool carGuards = [&u32] {
            return u32(0x1d5fa4u) == 0x0260282du && u32(0x1d5facu) == 0x0c0414ceu && u32(0x1d5fb4u) == 0x0260282du &&
                   u32(0x1d5fc0u) == 0x0c0414ceu && u32(0x1d5fd0u) == 0x0260282du && u32(0x1d5fdcu) == 0x0c0758deu &&
                   u32(0x1d5714u) == 0x0c0414b8u &&
                   u32(0x105338u) == 0x27bdffe0u && u32(0x1052e0u) == 0x27bdfff0u &&
                   u32(0x105818u) == 0x8c83000cu && u32(0x1d5cecu) == 0x2450850cu;
        }();
        static const bool menuViewGuards = [&u32] {
            return u32(0x2171d8u) == 0x0c0414deu && u32(0x2171dcu) == 0x02a0282du && u32(0x2171e0u) == 0x02a0282du &&
                   u32(0x2171e8u) == 0x0c0414deu && u32(0x2171f4u) == 0x0c04154au && u32(0x21720cu) == 0x0c04154au &&
                   u32(0x217220u) == 0x0c0415c2u && u32(0x217224u) == 0x260400f8u && u32(0x21722cu) == 0x0c041ebau &&
                   u32(0x2176d8u) == 0x0c0415c2u && u32(0x105378u) == 0x8ca60000u && u32(0x105528u) == 0x27bdfff0u;
        }();
        static uint32_t carFrame = 0u, carSaved[4] = {};
        // A race session (applyRaceDisplayMode 0x436640 .. applyMenuDisplayMode 0x436740): its screens are GT4's own 480p
        // on a PS2 too (the pre-race screen's car included), so C4 leaves them as GT4 draws them.
        static bool raceSession = false;
        if (targetPc == 0x436640u)
            raceSession = true;
        else if (targetPc == 0x436740u)
            raceSession = false;
        // The renderer's full-height car view (gs_car_view.h, CAR-PICTURE.md section 8): while the menus are 480p, the
        // car picture is on and no race session runs.
        {
            static int armedSaid = -1;
            const bool armed = menuOn && carPicture > 0 && carGuards && !raceSession && gt4carview::allowed();
            if (armedSaid != (armed ? 1 : 0))
            {
                gt4carview::g_armed.store(armed, std::memory_order_relaxed);
                armedSaid = armed ? 1 : 0;
            }
        }
        const auto frameIs480p = [&u32](uint32_t d) {
            return d >= 0x618500u && d < 0x618600u && u32(d + 4u) == 2u && u32(d + 0xcu) == 448u && u32(d + 0x38u) == 0u;
        };
        // F1 (the developer's Test Build 22 freeze, 2026-10-17: Arcade Time Trial stuck on its black loading screen; m480-tt1):
        // two of GT4's per-frame steps act only on the odd field of its 480i display (0x4a7c60: 4 odd field, 3 even; in
        // 480p menus it never says 4): the screen transition 0x104b68 (0x104b90) that starts a sequence's loading
        // screen (it reads the last menu picture back, field by field, for the loading screen to fade from, and
        // StartSequence's race 0x112620 -> 0x10af40 -> 0x104010 waits for it), and the fades' step 0x109080 (0x10908c).
        // GT4 used its 480p only inside race sessions on a PS2, so in 480p menus they waited for a field that never
        // comes. Outside race sessions, while the display is 480p, those two calls see 480i's fields (odd on odd
        // VSyncs, GT4's pace on a PS2). The transition's picture read-back (0x1059f8 from 0x104be4: a 24-bit read of one
        // 640x224 field into a 0x78000-byte half of its 0xf0000-byte buffer) would read the 448-line 16-bit frame as
        // 24-bit, past the buffer's end: it is left out and that half made black, so the loading screen fades from black.
        static const bool fieldGuards = [&u32] {
            return u32(0x104b90u) == 0x0c129f18u && u32(0x10908cu) == 0x0c129f18u && u32(0x4a7c6cu) == 0x90a300ebu &&
                   u32(0x104be4u) == 0x0c04167eu && u32(0x103fdcu) == 0x3c05000fu;
        }();
        if (targetPc == 0x4a7c60u && sourcePc == 0x104b90u && std::getenv("GT4_FIELD_LOG"))
        {
            static int n = 0;
            if (n++ < 6)
                std::fprintf(stderr, "[gt4-video] vsync %llu: transition field check: menus %d guards %d race session %d DS id %u interlace %u\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), menuOn, fieldGuards, raceSession, u32(0x6184d0u + 0x30u),
                             u32(0x6184d0u));
        }
        if (menuOn && fieldGuards && !raceSession && (targetPc == 0x4a7c60u || targetPc == 0x1059f8u))
        {
            // (GT4's 480p as DisplaySwitching has it: id 6, not interlaced. H2 switches through DisplaySwitching's update,
            // not GT4's setter, so the GS state 0x4a7c60 reads still says interlaced and its field never changes.)
            const bool progressive = u32(0x6184d0u + 0x30u) == 6u && u32(0x6184d0u) == 0u;
            if (progressive && targetPc == 0x4a7c60u && (sourcePc == 0x104b90u || sourcePc == 0x10908cu))
            {
                SET_GPR_U32(ctx, 2, (m_memory.gs().vsyncTick.load() & 1u) ? 4u : 3u);
                ctx->pc = fallthroughPc;
                return true;
            }
            if (progressive && targetPc == 0x1059f8u && sourcePc == 0x104be4u)
            {
                const uint32_t dest = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & 0x01ffffffu;
                if (dest + 0x78000u <= 0x02000000u)
                    std::memset(rdram + dest, 0, 0x78000u);
                static bool said = false;
                if (!said)
                    std::fprintf(stderr, "[gt4-video] vsync %llu: GT4's screen transition in 480p menus: fields given, the last menu picture not read back (fades from black)\n",
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
                said = true;
                ctx->pc = fallthroughPc;
                return true;
            }
        }
        if (menuOn && carPicture > 0 && carGuards)
        {
            const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & 0x1fffffffu;
            if (targetPc == 0x105338u && (sourcePc == 0x1d5facu || sourcePc == 0x1d5fc0u) && (carFrame == a1 || frameIs480p(a1)))
            {
                if (carFrame == 0u)
                {
                    carFrame = a1;
                    carSaved[0] = u32(a1 + 0xcu), carSaved[1] = u32(a1 + 0x1cu), carSaved[2] = u32(a1 + 0x28u), carSaved[3] = u32(a1 + 0x38u);
                    w32(a1 + 0xcu, 224u), w32(a1 + 0x1cu, 224u), w32(a1 + 0x28u, 1u), w32(a1 + 0x38u, 1u);
                }
            }
            else if (targetPc == 0x1d6378u && sourcePc == 0x1d5fdcu && carFrame != 0u)
            {
                // Deep Z (gs_car_view.h): the car passes with GT4's Z24 scale while the renderer draws them tall
                // (GT4's next depth set, 0x105708 at 0x1d5eb0, puts its Z16 scale back for the next frame).
                if (gt4carview::g_tallActive.load(std::memory_order_relaxed) && gt4carview::deepZAllowed() &&
                    gt4carview::armed())
                {
                    uint8_t *spr = m_memory.getScratchpad();
                    uint32_t largest = 0;
                    std::memcpy(&largest, spr + 0x223cu, 4);
                    if (largest == 0x477fff00u || (largest >> 16) == 0x477fu)
                    {
                        const uint32_t deep = 0x4b7ffbffu; // (GT4's own largest Z for PSMZ24, 0x4a2658)
                        std::memcpy(spr + 0x223cu, &deep, 4);
                    }
                }
                w32(carFrame + 0xcu, carSaved[0]), w32(carFrame + 0x1cu, carSaved[1]), w32(carFrame + 0x28u, carSaved[2]);
                w32(carFrame + 0x38u, carSaved[3]);
                carFrame = 0u;
            }
            else if (targetPc == 0x1052e0u && sourcePc == 0x1d5714u && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0)) == 224u)
            {
                SET_GPR_S32(ctx, 7, 112); // a3: the blur planes' height, as in 480i
            }
            // C4: GT Mode's car view (the menu render context 0x2170e0: dealers' car pages, ...) copies the frame descriptor
            // for its composite and car target (0x105378 from 0x2171d8 / 0x2171e8) and allocates them (0x105528 from
            // 0x2171f4 / 0x21720c); at 448 lines they leave the car's textures too little GS memory and GT4's texture
            // loads wrap into the display buffer (the car drawn green, m480-gt2p). Each copy, a 640x448 CT32 target, gets
            // 480i's field shape just before its allocation.
            else if (menuViewGuards && !raceSession && targetPc == 0x105528u && (sourcePc == 0x2171f4u || sourcePc == 0x21720cu))
            {
                const uint32_t d = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
                if (u32(d + 4u) == 0u && u32(d + 8u) == 640u && u32(d + 0xcu) == 448u && u32(d + 0x1cu) == 448u && u32(d + 0x38u) == 0u)
                    w32(d + 0xcu, 224u), w32(d + 0x1cu, 224u), w32(d + 0x28u, 1u), w32(d + 0x38u, 1u);
            }
            static bool said = false;
            if (!said && targetPc == 0x1d6378u && sourcePc == 0x1d5fdcu)
            {
                said = true;
                std::fprintf(stderr, "[gt4-video] vsync %llu: GT4's car view in 480p menus: its 3D car drawn at 480i's 224 rows (GT4_MENU_480P_CAR=%d)\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), carPicture);
            }
        }
#endif

        // ---- D6 (inv-more.md 6.3): GT4's 16:9 (or 4:3) without touching the player's save, while the player's
        // monitor geometry (Option+0x24, 11 words) is one of GT4's two flat presets (4:3 0x6a6188, 16:9 0x6a61b8).
        // W1: GT4's wide flag 0x6184f0 is 0x4390d0(geometry) (width / height >= 1.5) at the options apply
        // (0x4365e8) and the 1080i apply (0x4366d4): it is given the chosen preset instead. W2: the race view
        // set-up (0x388a80: matrix 0x439288 from 0x388a94, field of view 0x439240, then the render object's call at
        // 0x388b30) reads the geometry and wide_mode (Option+0x14): GT4's own values for the chosen shape while it
        // runs, the player's back at its end (and before any Option save 0x4374a8 or applyMenuDisplayMode).
        // The presenter already follows GT4's wide flag (Gt4WideFlag).
        static const bool aspectGuards = [&u32] {
            const bool ok = u32(0x4365e8u) == 0x0c10e434u && u32(0x4365ecu) == 0x26040024u && u32(0x4365f4u) == 0xac6284f0u &&
                            u32(0x4366d4u) == 0x0c10e434u && u32(0x4366d8u) == 0x26240024u && u32(0x4366dcu) == 0xae020020u &&
                            u32(0x4390d0u) == 0xc4810010u && u32(0x4390d4u) == 0xc480000cu && u32(0x6a6194u) == 0x43898000u &&
                            u32(0x6a6198u) == 0x434d0000u && u32(0x6a61c4u) == 0x43d48000u && u32(0x6a61c8u) == 0x43700000u &&
                            u32(0x388a94u) == 0x0c10e4a2u && u32(0x388ac0u) == 0x8c420014u && u32(0x388b1cu) == 0x0c10e490u &&
                            u32(0x388b30u) == 0x0040f809u && u32(0x439290u) == 0x8c622f4cu && u32(0x4392a0u) == 0x34a58cd4u &&
                            u32(0x439248u) == 0x8c622f4cu && u32(0x439254u) == 0x34218cd4u && u32(0x687b8cu) == 0x004374a8u;
            if (!ok)
                std::fprintf(stderr, "[gt4-video] CORE does not match: GT4_ASPECT not applied, GT4's own Aspect Ratio\n");
            return ok;
        }();
        const bool aspectOn = aspect != 0 && !aspectTestSwitch && aspectGuards;
        // GT4's own preset for the chosen shape; the wider shapes keep GT4's 16:9 (flag, menus) and widen the race view.
        const uint32_t preset = aspect == 43 ? 0x6a6188u : 0x6a61b8u;
#if !defined(GT4_STOCK_BUILD)
        const bool screensOn = gt4screens::config().on; // three screens (full-m2 "Multi")
#else
        constexpr bool screensOn = false;
#endif
        const char *shapeName = aspect == 43    ? "4:3"
                                : aspect == 219 ? "21:9"
                                : aspect == 329 ? "32:9"
                                : aspect == 489 ? (screensOn ? "three screens" : "48:9")
                                                : "16:9";
#if !defined(GT4_STOCK_BUILD)
        // ---- The wider shapes (full-m2 "Wide"; GT4_ASPECT=21:9|32:9|48:9, development and Full builds). GT4 keeps its
        // 16:9 state (W1, W2) and, while a race session is on (applyRaceDisplayMode 0x436640 from the race-session
        // sites to applyMenuDisplayMode 0x436740):
        //  - the race camera: W2 gives the race view set-up GT4's 16:9 geometry with the width times shape / (16:9)
        //    (425 mm -> 557.8 / 850 / 1275 at the same 240 mm height and distance): the same height of view, wider
        //    across (75.2 deg -> 90.6 / 114.0 / 133.2 deg), drawn squeezed into GT4's 640-wide frame;
        //  - the race HUD: the race display's 2D canvas (its ortho 0x39bb38 from 0x39ba50, -106.7..746.7 in 16:9) is
        //    480 x shape units wide, so every element keeps its shape; the side elements go out with the canvas edges
        //    in 21:9 and 32:9 (gt4wide::edgeFloatHook / IntHook / OffsetHook), in 48:9 they keep their 16:9 places in the
        //    centre section;
        //  - the presenter shows the race view's pictures at the shape (Gt4PictureAspect), GT4's menus at 16:9; while
        //    a menu is up over the race view (no race HUD, or the results table) the race view is narrowed to 16:9.
        const bool wideShape = aspect == 219 || aspect == 329 || aspect == 489;
        // Three screens (gt4screens, full-m2 "Multi"): the 48:9 path (the HUD in the centre section) at the screens' shape.
        const float shapeA = aspect == 219 ? 21.0f / 9.0f : aspect == 329 ? 32.0f / 9.0f : gt4screens::config().on ? gt4screens::config().shape : 48.0f / 9.0f;
        static const bool wideGuards = [&u32] {
            const bool ok = u32(0x39ba08u) == 0x8c6284f0u && u32(0x39ba18u) == 0x34215554u && u32(0x39ba24u) == 0x3421aaaau &&
                            u32(0x39ba50u) == 0x0c0e6eceu && u32(0x39bb38u) == 0x460d6141u && u32(0x3afd18u) == 0x30820004u &&
                            u32(0x3afd24u) == 0x8c4384f0u && u32(0x3afd30u) == 0x3c0142c0u && u32(0x3afd78u) == 0x30c80003u &&
                            u32(0x3afd90u) == 0x8c4384f0u && u32(0x3afd9cu) == 0x24e4ffa0u && u32(0x3afdd8u) == 0x30850008u &&
                            u32(0x3afdecu) == 0x3c0142c0u && u32(0x3afdf4u) == 0x8c6284f0u && u32(0x436640u) == 0x27bdffe0u &&
                            u32(0x436740u) == 0x27bdfff0u && u32(0x6a61c4u) == 0x43d48000u;
            return ok;
        }();
        const bool wideOn = aspectOn && wideShape && wideGuards;
        static bool wideInstalled = false;
        if (aspectOn && wideShape && !wideInstalled)
        {
            wideInstalled = true;
            if (!wideGuards)
                std::fprintf(stderr, "[gt4-video] CORE does not match (race display): GT4_ASPECT=%s not applied, GT4's 16:9\n", shapeName);
            else
            {
                gt4wide::edgeExtra = aspect == 489 ? 0.0f : 240.0f * shapeA - 1280.0f / 3.0f;
                gt4wide::edgeFloat = lookupFunction(0x3afd18u);
                gt4wide::edgeInt = lookupFunction(0x3afd78u);
                gt4wide::edgeOffset = lookupFunction(0x3afdd8u);
                if (gt4wide::edgeFloat && gt4wide::edgeInt && gt4wide::edgeOffset)
                {
                    registerFunction(0x3afd18u, &gt4wide::edgeFloatHook);
                    registerFunction(0x3afd78u, &gt4wide::edgeIntHook);
                    registerFunction(0x3afdd8u, &gt4wide::edgeOffsetHook);
                }
                if (const char *d = std::getenv("GT4_WIDE_PRESENT_DELAY"); d && *d)
                    gt4wide::presentDelay = std::clamp(std::atoi(d), 0, 10);
                gt4wide::shape.store(shapeA, std::memory_order_relaxed);
                std::fprintf(stderr, "[gt4-video] %s: GT4_ASPECT=%s: races and replays %.3f:1 (GT4's 16:9 race view widened: camera %.1f mm wide, HUD canvas %.1f units, side elements %+.1f), GT4's menus 16:9 with bars\n",
                             "GT4 wider shapes (full-m2)", shapeName, shapeA, 425.0f * shapeA * 9.0f / 16.0f, 480.0f * shapeA, gt4wide::edgeExtra);
            }
        }
        const uint64_t wideTick = m_memory.gs().vsyncTick.load();
        if (wideOn && targetPc == 0x436640u &&
            (sourcePc == 0x1084c4u || sourcePc == 0x10befcu || sourcePc == 0x10caacu || sourcePc == 0x110478u || sourcePc == 0x3c4c0cu))
            gt4wide::session = true;
        else if (targetPc == 0x436740u && gt4wide::session)
        {
            gt4wide::setWide(rdram, false, wideTick, "the race session ends");
            gt4wide::session = false;
            gt4wide::view = gt4wide::canvas = 0u;
        }
        // Each race frame (the race frame update 0x389638, once a frame in a race session): the race view is wide
        // while GT4's race HUD helpers ran in the last frames (the race, the finish, GT4's replays with their captions)
        // and its results table is not up (0x393790 draws the table's cars); otherwise GT4 is showing a menu over
        // the race view (pre-race and post-race menus, the results), shown at 16:9.
        if (wideOn && targetPc == 0x393790u && gt4wide::session)
            gt4wide::resultsFrame = gt4wide::frame;
        // GT4's in-race pause menu (Continue / Exit / Screen Settings): its per-frame method 0x3c3820 (the only entry of
        // the vtable 0x681b4c that holds it) runs once a frame while it is up; GT4 keeps drawing the race and its HUD
        // behind it, so without this the menu would be shown stretched (full-m2 Wide review).
        static const bool pauseGuards = u32(0x3c3820u) == 0x27bdffd0u && u32(0x3c3830u) == 0x00a0982du && u32(0x681b4cu) == 0x003c3820u;
        if (wideOn && pauseGuards && targetPc == 0x3c3820u && gt4wide::session)
            gt4wide::pauseFrame = gt4wide::frame;
        if (wideOn && targetPc == 0x389638u && gt4wide::session)
        {
            ++gt4wide::frame;
            const bool hud = gt4wide::hudFrame != 0u && gt4wide::frame - gt4wide::hudFrame <= 3u;
            const bool results = gt4wide::resultsFrame != 0u && gt4wide::frame - gt4wide::resultsFrame <= 10u;
            const bool paused = gt4wide::pauseFrame != 0u && gt4wide::frame - gt4wide::pauseFrame <= 3u;
            gt4wide::setWide(rdram, hud && !results && !paused, wideTick,
                             results ? "the results table" : paused ? "GT4's pause menu" : hud ? "GT4's race HUD" : "no race HUD");
        }
        // Two per-frame factors of GT4's 16:9 race display, while the race view is wide: the x scale of its 2D draw
        // state 0x623838 (0x39f498: 0.75 in 16:9, set with 0x44e390 from 0x39f588; the HUD's numbers are drawn with
        // it) times (16:9) / shape, and the view shape GT4 gives the effect 0x497af0 from 0x3b8204 (1.214 in 16:9,
        // 0.911 in 4:3: the shape times 0.683) times shape / (16:9).
        static const bool factorGuards = u32(0x39f554u) == 0x3c013f40u && u32(0x39f55cu) == 0x8c6284f0u && u32(0x39f588u) == 0x0c1138e4u &&
                                         u32(0x3b8134u) == 0x3c013f9bu && u32(0x3b81a8u) == 0x8c4384f0u && u32(0x3b8204u) == 0x0c125ebcu &&
                                         u32(0x3b81d8u) == 0x4600a306u;
        static const bool mirrorGuards = u32(0x33efc8u) == 0x3407e2e0u && u32(0x33f020u) == 0xe4e00008u && u32(0x33f030u) == 0xe4e10000u &&
                                         u32(0x33f0a4u) == 0x0060f809u && u32(0x33f080u) == 0x0c0d0762u && u32(0x33f0a8u) == 0x02242021u;
        if (wideOn && mirrorGuards && gt4wide::session && (sourcePc == 0x33f0a4u || sourcePc == 0x33f080u))
            gt4wide::mirrorAtDraw(rdram, (GPR_U32(ctx, 17) & 0x1fffffffu) + 0xe2e0u);
        // The rear-view mirror page (gt4_mirror.h): GT4 draws its mirror this frame, at this rectangle (as kept above).
        if (mirrorGuards && (sourcePc == 0x33f0a4u || sourcePc == 0x33f080u))
            gt4mirror::noteDrawn(rdram, (GPR_U32(ctx, 17) & 0x1fffffffu) + 0xe2e0u, m_memory.gs().vsyncTick.load());
        if (wideOn && factorGuards && gt4wide::wideNow && gt4wide::session)
        {
            if (targetPc == 0x44e390u && sourcePc == 0x39f588u && std::fabs(ctx->f[12] - 0.75f) < 1e-5f)
                ctx->f[12] = 0.75f * (16.0f / 9.0f) / shapeA;
            else if (targetPc == 0x497af0u && sourcePc == 0x3b8204u && std::fabs(ctx->f[12] - 1.2139999f) < 1e-4f)
                ctx->f[12] *= shapeA * 9.0f / 16.0f;
        }
        if (wideOn && targetPc == 0x39bb38u && sourcePc == 0x39ba50u && std::fabs(ctx->f[12] + 320.0f / 3.0f) < 0.01f &&
            std::fabs(ctx->f[13] - 2240.0f / 3.0f) < 0.01f)
        {
            // A new race display (each race phase): GT4 chose its 16:9 canvas (wide flag set). While the race view is
            // wide: 480 x shape units, centred on 320; setWide changes it with the race view.
            gt4wide::canvas = (GPR_U32(ctx, 4) & 0x1fffffffu) + 0x70u;
            if (gt4wide::wideNow)
            {
                const float half = gt4wide::canvasHalf(true);
                ctx->f[12] = 320.0f - half;
                ctx->f[13] = 320.0f + half;
            }
        }
#else
        constexpr bool wideOn = false;
        constexpr float shapeA = 16.0f / 9.0f;
#endif
        // The player's geometry is one of GT4's flat presets (GT4's own comparison, 0x439220: 0x2c bytes).
        auto flatGeometry = [rdram](uint32_t geometry) {
            if (geometry + 0x2cu >= 0x02000000u)
                return false;
            const uint8_t *g = rdram + geometry;
            return std::memcmp(g, rdram + 0x6a6188u, 0x2cu) == 0 || std::memcmp(g, rdram + 0x6a61b8u, 0x2cu) == 0;
        };
        if (aspectOn && targetPc == 0x4390d0u && (sourcePc == 0x4365e8u || sourcePc == 0x4366d4u))
        {
            const uint32_t geometry = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
            static bool said = false, saidOther = false;
            if (geometry == gameOption + 0x24u && flatGeometry(geometry))
            {
                SET_GPR_S32(ctx, 4, static_cast<int32_t>(preset));
                if (!said)
                    std::fprintf(stderr, "[gt4-video] vsync %llu: GT4_ASPECT=%s: GT4's own %s state while it draws (the player's Aspect Ratio and save unchanged)\n",
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), shapeName, aspect == 43 ? "4:3" : "16:9");
                said = true;
            }
            else if (!saidOther)
            {
                saidOther = true;
                std::fprintf(stderr, "[gt4-video] GT4_ASPECT: the monitor geometry is not one of GT4's flat presets (LAN / triple screen): GT4's own\n");
            }
        }
        static bool bracketOpen = false;
        static uint32_t bracketOption = 0u, savedWide = 0u;
        static uint8_t savedGeometry[0x2c] = {};
        if (bracketOpen && (sourcePc == 0x388b30u || targetPc == 0x4374a8u || targetPc == 0x436740u))
        {
#if !defined(GT4_STOCK_BUILD)
            // The wider shapes: the race render object's set-up (0x371f38 copies the matrix to +0x9c0) gets the x
            // scale of the race view as it is drawn now (narrowed to 16:9 while a menu is up); setWide switches it.
            if (wideOn && sourcePc == 0x388b30u && GPR_U32(ctx, 2) == 0x371f38u && u32(0x371f38u) == 0xdca20000u &&
                u32(0x371f3cu) == 0x248409c0u && u32(0x371f78u) == 0xe48c0040u)
            {
                const uint32_t m = GPR_U32(ctx, 5) & 0x1fffffffu;
                gt4wide::viewWide = gt4wide::rd(rdram, m);
                gt4wide::viewNarrow = gt4wide::viewWide * shapeA * 9.0f / 16.0f;
                gt4wide::viewY = gt4wide::rd(rdram, m + 0x14u);
                gt4wide::view = (GPR_U32(ctx, 4) & 0x1fffffffu) + 0x9c0u;
                if (!gt4wide::wideNow)
                    gt4wide::wr(rdram, m, gt4wide::viewNarrow);
            }
#endif
            w32(bracketOption + 0x14u, savedWide);
            std::memcpy(rdram + bracketOption + 0x24u, savedGeometry, sizeof(savedGeometry));
            bracketOpen = false;
        }
        else if (aspectOn && !bracketOpen && targetPc == 0x439288u && sourcePc == 0x388a94u && gameOption + 0x50u < 0x02000000u &&
                 flatGeometry(gameOption + 0x24u))
        {
            bracketOption = gameOption;
            savedWide = u32(gameOption + 0x14u);
            std::memcpy(savedGeometry, rdram + gameOption + 0x24u, sizeof(savedGeometry));
            w32(gameOption + 0x14u, aspect == 43 ? 0u : 1u);
            std::memcpy(rdram + gameOption + 0x24u, rdram + preset, sizeof(savedGeometry));
            if (wideOn)
            {
                // The wider shapes: GT4's 16:9 width (Option+0x30) times shape / (16:9), the height and distance kept.
                float width = 0.0f;
                std::memcpy(&width, rdram + gameOption + 0x30u, 4);
                width *= shapeA * 9.0f / 16.0f;
                std::memcpy(rdram + gameOption + 0x30u, &width, 4);
            }
            bracketOpen = true;
        }
    }
#if !defined(GT4_STOCK_BUILD)
    // GT4's own Multi Monitor Mode settings (gt4_multimon.h, GT4_MM_*): written just before GT4 reads them; the geometry
    // while the race view is set up, after GT4_ASPECT's bracket so it wins over its preset.
    if (isCall && gt4mm::config().on)
        gt4mm::afterAspect(rdram, ctx, targetPc, sourcePc, m_memory.gs().vsyncTick.load());
#endif
    if (isCall && (targetPc == 0x436640u || targetPc == 0x436740u))
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        gt4texdump::setRace(targetPc == 0x436640u); // GT4_TEXTURE_DUMP's folders: track\race-NN or menus
#endif
        auto u32 = [rdram](uint32_t a) {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
            return v;
        };
        static const bool coreOk = u32(0x436640u) == 0x27bdffe0u && u32(0x43665cu) == 0x8e2316b0u && u32(0x436740u) == 0x27bdfff0u;
        const uint64_t tick = m_memory.gs().vsyncTick.load();
        const uint32_t option = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
        if (!coreOk)
        {
            static bool said = false;
            if (!said)
                std::fprintf(stderr, "[gt4-video] CORE does not match (0x436640 / 0x436740): no log, no test switches\n");
            said = true;
        }
        else if (targetPc == 0x436740u)
            std::fprintf(stderr, "[gt4-video] vsync %llu: applyMenuDisplayMode (menus: %s); call from 0x%x\n", static_cast<unsigned long long>(tick),
                         g_gt4Menus480p.load(std::memory_order_relaxed) ? "480p, GT4_RACE_MODE" : "480i", sourcePc);
        else if (option != 0u && option + 0x16b4u < 0x02000000u)
        {
#if !defined(GT4_STOCK_BUILD)
            static const int forcedMode = [] {
                const char *v = std::getenv("GT4_RACE_VIDEO_MODE");
                const std::string m = v ? v : "";
                const int mode = m == "480i" || m == "0" ? 0 : m == "480p" || m == "1" ? 1 : m == "1080i" || m == "2" ? 2 : -1;
                if (v && *v && mode < 0)
                    std::fprintf(stderr, "[gt4-video] GT4_RACE_VIDEO_MODE=%s not understood (480i, 480p, 1080i): ignored\n", v);
                else if (mode >= 0)
                    std::fprintf(stderr, "[gt4-video] test switch GT4_RACE_VIDEO_MODE=%s: races use it whatever GT4's Options say\n", v);
                return mode;
            }();
            static const int forcedBlend = [] {
                const char *v = std::getenv("GT4_RACE_VIDEO_BLEND");
                return v && (*v == '0' || *v == '1') ? *v - '0' : -1;
            }();
            const uint32_t chosen = u32(option + 0x16b0u), blendWas = u32(option + 0x20u);
            if (forcedMode >= 0)
            {
                const uint32_t v = static_cast<uint32_t>(forcedMode);
                std::memcpy(rdram + ((option + 0x16b0u) & 0x01fffffcu), &v, 4);
            }
            if (forcedBlend >= 0)
            {
                const uint32_t v = static_cast<uint32_t>(forcedBlend);
                std::memcpy(rdram + ((option + 0x20u) & 0x01fffffcu), &v, 4);
            }
            if (forcedMode >= 0 || forcedBlend >= 0)
                std::fprintf(stderr, "[gt4-video] vsync %llu: test switch: race_display_mode %u -> %u, Option+0x20 %u -> %u\n",
                             static_cast<unsigned long long>(tick), chosen, u32(option + 0x16b0u), blendWas, u32(option + 0x20u));
#endif
            const uint32_t mode = u32(option + 0x16b0u);
            std::fprintf(stderr, "[gt4-video] vsync %llu: applyRaceDisplayMode Option=%x race_display_mode=%u (%s) Option+0x20=%u Option+0x24=%08x; call from 0x%x\n",
                         static_cast<unsigned long long>(tick), option, mode, mode == 0 ? "480i" : mode == 1 ? "480p" : mode == 2 ? "1080i" : "?",
                         u32(option + 0x20u), u32(option + 0x24u), sourcePc);
        }
    }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4's race cameras (gt4cam, full-m2 "Cameras": GT4_CAM_FAR_CHASE, GT4_CAM_LOOK): installed at the first race
    // session (applyRaceDisplayMode 0x436640: GT4's CORE is in memory), behind the CORE words they rely on. Absent keys
    // (or off): nothing is installed, GT4's own cameras.
    // The camera's matrix changes before GT4's update gives back the VU0 lock (gt4cam::edit; s1 is the camera there).
    if (gt4cam::releaseHooked && isCall && targetPc == 0x4a06f0u && sourcePc == 0x37ac80u)
        gt4cam::edit(rdram, GPR_U32(ctx, 17));
    if (isCall && targetPc == 0x436640u)
    {
        static bool camTried = false;
        if (!camTried)
        {
            camTried = true;
            auto onOff = [](const char *name, const char *v, const char *onWord) {
                const std::string s = v ? v : "";
                if (s == onWord || (std::string(onWord) == "on" && s == "1"))
                    return true;
                if (!s.empty() && s != "off" && s != "0")
                    std::fprintf(stderr, "[gt4-cam] %s=%s not understood (%s or off): off\n", name, s.c_str(), onWord);
                return false;
            };
            const bool wantFar = onOff("GT4_CAM_FAR_CHASE", std::getenv("GT4_CAM_FAR_CHASE"), "on");
            const bool wantLook = onOff("GT4_CAM_LOOK", std::getenv("GT4_CAM_LOOK"), "360");
            // MoreCams: GT4_CAM_VIEWS (the view button's views, in order) and GT4_CAM_LOCK (the views locked to the car's tilt).
            auto viewList = [](const char *name, const char *v, bool &given) {
                std::vector<int> out;
                given = v && *v;
                if (!given)
                    return out;
                std::stringstream list(v);
                std::string item;
                while (std::getline(list, item, ','))
                {
                    item.erase(0, item.find_first_not_of(" \t"));
                    item.erase(item.find_last_not_of(" \t") + 1);
                    int found = -1;
                    for (int i = 0; i < gt4cam::ViewCount; ++i)
                        if (item == gt4cam::kViewKeys[i])
                            found = i;
                    if (found >= 0 && std::find(out.begin(), out.end(), found) == out.end())
                        out.push_back(found);
                    else if (found < 0 && item != "none" && !item.empty())
                        std::fprintf(stderr, "[gt4-cam] %s: '%s' not understood (bumper, bonnet, roof, close, chase, far): left out\n", name, item.c_str());
                }
                return out;
            };
            bool viewsGiven = false, lockGiven = false;
            const std::vector<int> views = viewList("GT4_CAM_VIEWS", std::getenv("GT4_CAM_VIEWS"), viewsGiven);
            const std::vector<int> locked = viewList("GT4_CAM_LOCK", std::getenv("GT4_CAM_LOCK"), lockGiven);
            if (viewsGiven && views.empty())
            {
                std::fprintf(stderr, "[gt4-cam] GT4_CAM_VIEWS has no view: GT4's own views\n");
                viewsGiven = false;
            }
            unsigned lockBits = gt4cam::kGt4Lock;
            if (lockGiven)
            {
                lockBits = 0u;
                for (int i : locked)
                    lockBits |= 1u << i;
            }
            // (GT4_MIRROR=both / page: GT4's mirror in every view, set after the camera's update; gt4_mirror.h)
            // (GT4_PAGES local / lan: the mirror page may watch with GT4_MIRROR=screen)
            static const bool pagesOn = [] {
                const char *p = std::getenv("GT4_PAGES");
                return p && (std::strcmp(p, "local") == 0 || std::strcmp(p, "lan") == 0);
            }();
            if (wantFar || wantLook || viewsGiven || lockBits != gt4cam::kGt4Lock || gt4mirror::mode() != 0 || pagesOn)
            {
                auto u32 = [rdram](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
                    return v;
                };
                // CameraOnBoard's update and input methods (prologues, its vtable entries), the view button's byte, the
                // matrix store, the view cycle (its table {0, 1, 6} and modulus 12) and the chase offset's stores.
                const bool guards = u32(0x37a458u) == 0x27bdff70u && u32(0x37ad40u) == 0x27bdffd0u && u32(0x37ad6cu) == 0x82500019u &&
                                    u32(0x37ac70u) == 0x0c129d54u && u32(0x37acd8u) == 0x244711e8u && u32(0x37ad10u) == 0x2402000cu &&
                                    u32(0x37ad38u) == 0x8c4211e8u && u32(0x6211e8u) == 0u && u32(0x6211ecu) == 1u && u32(0x6211f0u) == 6u &&
                                    u32(0x67a62cu) == 0x37a458u && u32(0x67a714u) == 0x37ad40u && u32(0x37b318u) == 0xe6200040u &&
                                    u32(0x37b320u) == 0xe6210044u && u32(0x37b328u) == 0xe6200048u;
                gt4cam::update = lookupFunction(0x37a458u);
                gt4cam::input = lookupFunction(0x37ad40u);
                if (!guards || !gt4cam::update || !gt4cam::input)
                    std::fprintf(stderr, "[gt4-cam] CORE does not match (race camera): GT4_CAM_FAR_CHASE / GT4_CAM_LOOK not applied, GT4's own cameras\n");
                else
                {
                    gt4cam::vsync = &m_memory.gs().vsyncTick;
                    gt4cam::farOn = wantFar;
                    gt4cam::lookOn = wantLook;
                    if (viewsGiven)
                        gt4cam::cycle = views;
                    else if (wantFar)
                        gt4cam::cycle = {gt4cam::Bumper, gt4cam::Chase, gt4cam::Bonnet, gt4cam::Far};
                    // The car's body as GT4's view builders read it (0x344858 / 0x344888 / 0x3448b8: pitch, roll, heading,
                    // mixed by +0x554; 0x345138 / 0x345168: the chase view's lag) and the chase view's use of them.
                    const bool bodyWords = u32(0x344798u) == 0xc4800554u && u32(0x344868u) == 0xc48008bcu && u32(0x34486cu) == 0xc481066cu &&
                                           u32(0x344898u) == 0xc48008c0u && u32(0x34489cu) == 0xc4810670u && u32(0x3448c8u) == 0xc48008c4u &&
                                           u32(0x3448ccu) == 0xc4810674u && u32(0x345148u) == 0xc4800994u && u32(0x34514cu) == 0xc4810700u &&
                                           u32(0x345178u) == 0xc4800998u && u32(0x34517cu) == 0xc4810704u && u32(0x37b3b8u) == 0x0c0d144eu &&
                                           u32(0x37b3c8u) == 0x0c0d145au && u32(0x37b4dcu) == 0x0c129e11u && u32(0x37b064u) == 0x8e22018cu &&
                                           u32(0x37b06cu) == 0x8c520018u;
                    gt4cam::lockBits = lockBits;
                    gt4cam::lockOn = lockBits != gt4cam::kGt4Lock && bodyWords;
                    if (lockBits != gt4cam::kGt4Lock && !bodyWords)
                        std::fprintf(stderr, "[gt4-cam] CORE does not match (the car's body): GT4_CAM_LOCK not applied, GT4's own tilt\n");
                    if (viewsGiven || lockGiven)
                    {
                        std::string cyc, lk;
                        for (int i : gt4cam::cycle)
                            cyc += (cyc.empty() ? "" : " -> ") + std::string(gt4cam::kViewNames[i]);
                        for (int i = 0; i < gt4cam::ViewCount; ++i)
                            if ((lockBits >> i) & 1u)
                                lk += (lk.empty() ? "" : ", ") + std::string(gt4cam::kViewNames[i]);
                        std::fprintf(stderr, "[gt4-cam] %s: view button %s; locked to the car's tilt: %s%s\n", "GT4 more race cameras (full-m2)",
                                     cyc.empty() ? "GT4's own views" : cyc.c_str(), lk.empty() ? "none" : lk.c_str(),
                                     lockBits == gt4cam::kGt4Lock ? " (GT4's own)" : gt4cam::lockOn ? "" : " (not applied)");
                    }
                    // GT4_CAM_LOOK_TEST=<vsync>:<degrees>,... (tests only): the look-around from those VSyncs on.
                    if (const char *v = std::getenv("GT4_CAM_LOOK_TEST"); v && *v && wantLook)
                    {
                        std::stringstream list(v);
                        std::string item;
                        while (std::getline(list, item, ','))
                        {
                            const size_t colon = item.find(':');
                            if (colon != std::string::npos)
                                gt4cam::testLook.emplace_back(std::strtoull(item.c_str(), nullptr, 10), static_cast<float>(std::atof(item.c_str() + colon + 1)));
                        }
                        std::fprintf(stderr, "[gt4-cam] test switch GT4_CAM_LOOK_TEST=%s: %zu steps\n", v, gt4cam::testLook.size());
                    }
                    // The lock's release after the matrix store (jal 0x4a06f0 at 0x37ac80, s1 the camera since 0x37ac74).
                    gt4cam::releaseHooked = u32(0x37ac80u) == 0x0c1281bcu && u32(0x37ac74u) == 0x26240100u && u32(0x4a06f0u) == 0x27bdfff0u;
                    if (!gt4cam::releaseHooked)
                        std::fprintf(stderr, "[gt4-cam] CORE does not match (the VU0 lock's release): the camera changed after GT4's update\n");
                    registerFunction(0x37a458u, &gt4cam::updateHook);
                    registerFunction(0x37ad40u, &gt4cam::inputHook);
                    std::fprintf(stderr, "[gt4-cam] %s: far chase %s (%.1f times GT4's chase distance%s), 360 look %s\n",
                                 "GT4 race cameras (full-m2)",
                                 viewsGiven ? (std::find(views.begin(), views.end(), static_cast<int>(gt4cam::Far)) != views.end() ? "on" : "off") : wantFar ? "on" : "off",
                                 gt4cam::kFarScale, viewsGiven ? ", in GT4_CAM_VIEWS' order" : ", after GT4's own views",
                                 wantLook ? "on (the right stick while it does not drive the car; keypad, mouse)" : "off");
                }
            }
        }
    }
    // GT4's car and wheel detail (gt4detail, full-m2 "Detail": GT4_CAR_DETAIL, GT4_WHEEL_DETAIL, GT4_WHEEL_BLUR): installed
    // at the first race session, behind the CORE words it relies on. Absent keys (or game / on): nothing is installed.
    if (isCall && targetPc == 0x436640u)
    {
        static bool detailTried = false;
        if (!detailTried)
        {
            detailTried = true;
            auto level = [](const char *key, const char *v) {
                const std::string s = v ? v : "";
                const int d = gt4detail::parse(s);
                if (d < 0)
                    std::fprintf(stderr, "[gt4-detail] %s=%s not understood (game, 2x, 4x or full): game\n", key, s.c_str());
                return std::max(d, 0);
            };
            const int car = level("GT4_CAR_DETAIL", std::getenv("GT4_CAR_DETAIL"));
            const int wheel = level("GT4_WHEEL_DETAIL", std::getenv("GT4_WHEEL_DETAIL"));
            bool blurOff = false;
            if (const char *v = std::getenv("GT4_WHEEL_BLUR"); v && *v)
            {
                const std::string s = v;
                blurOff = s == "off" || s == "0";
                if (!blurOff && s != "on" && s != "1")
                    std::fprintf(stderr, "[gt4-detail] GT4_WHEEL_BLUR=%s not understood (on or off): on\n", v);
            }
            // GT4_DETAIL_TEST=<vsync>:<car>:<wheel>:<on|off>,... (tests only): the three choices from those VSyncs on.
            std::vector<gt4detail::TestStep> test;
            if (const char *v = std::getenv("GT4_DETAIL_TEST"); v && *v)
            {
                std::stringstream list(v);
                std::string item;
                while (std::getline(list, item, ','))
                {
                    std::stringstream parts(item);
                    std::string at, c, w, b;
                    if (std::getline(parts, at, ':') && std::getline(parts, c, ':') && std::getline(parts, w, ':') && std::getline(parts, b, ':') &&
                        gt4detail::parse(c) >= 0 && gt4detail::parse(w) >= 0)
                        test.push_back({std::strtoull(at.c_str(), nullptr, 10), gt4detail::parse(c), gt4detail::parse(w), b == "off"});
                }
                std::fprintf(stderr, "[gt4-detail] test switch GT4_DETAIL_TEST=%s: %zu steps\n", v, test.size());
            }
            if (car != 0 || wheel != 0 || blurOff || !test.empty())
            {
                auto u32 = [rdram](uint32_t a) {
                    uint32_t v = 0;
                    std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
                    return v;
                };
                // The two level picks (their first words, the forced level and threshold reads, the 0.33 and the jump to
                // 0x4574e0, 0x454fb0's threshold read), the wheel texture method (its blur mode read and the tag + 1), the
                // three vtable entries and the interpreter's two callback calls (vtable +0x10 / +0x18, jalr).
                const bool guards = u32(0x4539a8u) == 0x8c840004u && u32(0x4539acu) == 0x8c83008cu && u32(0x4539b8u) == 0xc4810090u &&
                                    u32(0x4539ccu) == 0x3421f5c2u && u32(0x4540a8u) == 0x27bdfff0u && u32(0x4540b4u) == 0x8c43008cu &&
                                    u32(0x4540c4u) == 0x08115d38u && u32(0x454fb4u) == 0xc44138a4u && u32(0x453e68u) == 0x27bdffc0u &&
                                    u32(0x453f04u) == 0x8e120008u && u32(0x453f28u) == 0x26310001u && u32(0x688644u) == 0x4539a8u &&
                                    u32(0x68860cu) == 0x4540a8u && u32(0x688604u) == 0x453e68u && u32(0x455dd0u) == 0x24630018u &&
                                    u32(0x455d5cu) == 0x24630010u && u32(0x455ddcu) == 0x0040f809u;
                gt4detail::bodyPick = lookupFunction(0x4539a8u);
                gt4detail::wheelPick = lookupFunction(0x4540a8u);
                gt4detail::wheelTexture = lookupFunction(0x453e68u);
                if (!guards || !gt4detail::bodyPick || !gt4detail::wheelPick || !gt4detail::wheelTexture)
                    std::fprintf(stderr, "[gt4-detail] CORE does not match (car detail): GT4_CAR_DETAIL / GT4_WHEEL_DETAIL / GT4_WHEEL_BLUR not applied, GT4's own detail\n");
                else
                {
                    gt4detail::vsync = &m_memory.gs().vsyncTick;
                    gt4detail::detail[0] = car;
                    gt4detail::detail[1] = wheel;
                    gt4detail::blurOff = blurOff;
                    gt4detail::test = std::move(test);
                    const char *lv = std::getenv("GT4_DETAIL_LOG");
                    gt4detail::log = lv && *lv == '1';
                    const bool testing = !gt4detail::test.empty();
                    if (car != 0 || testing)
                        registerFunction(0x4539a8u, &gt4detail::bodyHook);
                    if (wheel != 0 || testing)
                        registerFunction(0x4540a8u, &gt4detail::wheelHook);
                    if (blurOff || testing)
                        registerFunction(0x453e68u, &gt4detail::textureHook);
                    std::fprintf(stderr, "[gt4-detail] %s: car detail %s, wheel detail %s, wheel-spin blur %s\n", "GT4 car and wheel detail (full-m2)",
                                 gt4detail::name(car), gt4detail::name(wheel), blurOff ? "off" : "on");
                }
            }
        }
    }
#endif
    // GT3_CALL_STR=<hex target>,<arg 0..3> (diagnostics): the string an argument points to, logged for each call of that
    // function (the first 300), with the caller and the return address.
    {
        static const std::pair<uint32_t, int> callStr = [] {
            const char *v = std::getenv("GT3_CALL_STR");
            if (!v || !*v)
                return std::pair<uint32_t, int>{0u, 0};
            char *end = nullptr;
            const uint32_t t = static_cast<uint32_t>(std::strtoul(v, &end, 16));
            return std::pair<uint32_t, int>{t, end && *end == ',' ? std::clamp(std::atoi(end + 1), 0, 3) : 0};
        }();
        static std::atomic<int> callStrLeft{300};
        if (callStr.first && isCall && targetPc == callStr.first && callStrLeft.fetch_sub(1) > 0)
        {
            const uint32_t a = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4 + callStr.second], 0)) & 0x01ffffffu;
            char text[96] = {};
            for (uint32_t i = 0; i + 1 < sizeof(text) && a + i < 0x02000000u; ++i)
            {
                const uint8_t c = rdram[a + i];
                if (!c)
                    break;
                text[i] = c >= 32 && c < 127 ? static_cast<char>(c) : '.';
            }
            std::fprintf(stderr, "[call-str] vsync %llu: %08x(a%d=%08x \"%s\") from %08x ra %08x\n", static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()),
                         targetPc, callStr.second, a, text, sourcePc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)));
        }
    }
    // GT3_COPY_WATCH=<hex lo>,<hex hi> (diagnostics): every call of GT4's memcpy (0x5a4724) whose destination range
    // [a0, a0 + a2) overlaps [lo, hi), logged at once with the caller, its return address and the source.
    if (isCall && targetPc == 0x5a4724u)
    {
        static const std::pair<uint32_t, uint32_t> copyWatch = [] {
            const char *v = std::getenv("GT3_COPY_WATCH");
            if (!v || !*v)
                return std::pair<uint32_t, uint32_t>{0u, 0u};
            char *end = nullptr;
            const uint32_t lo = static_cast<uint32_t>(std::strtoul(v, &end, 16));
            return std::pair<uint32_t, uint32_t>{lo, end && *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16)) : lo + 4u};
        }();
        if (copyWatch.second)
        {
            const uint32_t dst = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1fffffffu;
            const uint32_t len = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
            if (dst < copyWatch.second && dst + len > copyWatch.first)
                std::fprintf(stderr, "[copy-watch] vsync %llu: memcpy to %08x..%08x (%u bytes) from %08x, called from %08x (ra %08x, sp %08x)\n",
                             static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()), dst, dst + len, len,
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)), sourcePc, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)),
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)));
        }
    }
    if (callTraceTarget != 0u && isCall && targetPc == callTraceTarget)
    {
        static std::mutex traceMutex;
        static std::map<std::pair<uint32_t, int32_t>, uint64_t> sites;
        static auto last = std::chrono::steady_clock::now();
        // GT3_CALL_TRACE_ARG=0..3: which argument (a0..a3) to list (default a0).
        static const int traceArg = [] {
            const char *v = std::getenv("GT3_CALL_TRACE_ARG");
            return v && *v ? std::clamp(std::atoi(v), 0, 3) : 0;
        }();
        std::lock_guard<std::mutex> lock(traceMutex);
        ++sites[{sourcePc, static_cast<int32_t>(_mm_extract_epi32(ctx->r[4 + traceArg], 0))}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(5))
        {
            last = now;
            std::cerr << "[call-trace] 0x" << std::hex << callTraceTarget << std::dec << " at vsync " << m_memory.gs().vsyncTick.load() << ":";
            for (const auto &[site, count] : sites)
                std::cerr << " 0x" << std::hex << site.first << std::dec << "(a0=" << site.second << ")x" << count;
            std::cerr << std::endl;
            sites.clear();
        }
    }
#if defined(PS2X_GAME_GT3)
    // GT3's scePadGetDmaStr (0x272090) drives its PADMAN profile service.
    if (isCall && targetPc == 0x272090u && m_iopSubsystem && m_iopHost)
    {
        auto scope = m_iopHost->enterCall(ctx, rdram);
        m_iopSubsystem->onGuestCall(targetPc);
    }
#endif

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (g_eeNoYield == 0 && m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // (HUD hand-off 7 C: RaceDisplay's draw, bracketed already, yields before it starts: closed at its real return)
        if (isCall && targetPc == 0x39f498u && gt4hudpic::marking())
            gt4hudpic::callSuspended(rdram, 1000, fallthroughPc, GPR_U32(ctx, 29), false);
#endif
        g_eeYieldTaken = true;
        if (isCall)
            m_eeScheduler->noteYieldAtCall(targetPc, fallthroughPc);
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            return true;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    auto& callTrace=gt3_diagnostic::inputCallTrace();
    const bool traceInput=callTrace.selected(m_memory.gs().vsyncTick.load(),targetPc);
    const auto traceA0=GPR_U32(ctx,4),traceA1=GPR_U32(ctx,5);
    if (traceInput) callTrace.record("enter",m_memory.gs().vsyncTick.load(),sourcePc,
        targetPc,ctx->pc,traceA0,traceA1,GPR_U32(ctx,2),rdram);
    const bool outerYield = g_eeYieldTaken;
    g_eeYieldTaken = false;
    bool traced = false;
    if (g_dispatchDebug)
    {
        if (callWatch().enabled)
            callWatchCheck(rdram, ctx, "enter", targetPc, sourcePc, GPR_U32(ctx, 31), m_memory.gs().vsyncTick.load());
        traced = targetPc == traceFn().address && traceFn().remaining > 0 &&
                 m_memory.gs().vsyncTick.load() >= traceFn().from;
        if (traced)
            traceFnLog(ctx, "enter", m_memory.gs().vsyncTick.load());
    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // The launcher's race pictures (gt4_hud_pictures.h): on the one marked frame, RaceDisplay's member draws bracketed.
    const int hudPicToken = gt4hudpic::marking() ? gt4hudpic::callEnter(rdram, targetPc, GPR_U32(ctx, 4)) : -1;
    // (HUD hand-off 7 C: a bracketed call that a blocking wait leaves (an exception to the scheduler) is closed when it really
    // returns: gt4_hud_pictures.h callSuspended)
    struct HudCallGuard
    {
        int token;
        uint8_t *rdram;
        uint32_t ret, sp;
        bool done = false;
        ~HudCallGuard()
        {
            if (token >= 0 && !done && std::uncaught_exceptions() > 0)
                gt4hudpic::callSuspended(rdram, token, ret, sp, true);
        }
    } hudCallGuard{hudPicToken, rdram, fallthroughPc, GPR_U32(ctx, 29)};
#endif
    targetFn(rdram, ctx, this);
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    if (hudPicToken >= 0)
    {
        // (a call that came back through a scheduler checkpoint has not finished: it is closed at its real return, HUD
        // hand-off 7 C; before, its bracket closed here and the rest of GT4's HUD drew outside it now and then)
        hudCallGuard.done = true;
        if (g_eeYieldTaken)
            gt4hudpic::callSuspended(rdram, hudPicToken, fallthroughPc, hudCallGuard.sp, false);
        else
            gt4hudpic::callLeave(rdram, hudPicToken);
    }
#endif
    if (g_dispatchDebug)
    {
        if (traced)
        {
            traceFnLog(ctx, "return", m_memory.gs().vsyncTick.load());
            --traceFn().remaining;
        }
        if (callWatch().enabled)
            callWatchCheck(rdram, ctx, "return", targetPc, sourcePc, GPR_U32(ctx, 31), m_memory.gs().vsyncTick.load());
    }
    const bool yielded = g_eeYieldTaken;
    g_eeYieldTaken = outerYield || yielded;
    if (traceInput) callTrace.record("return",m_memory.gs().vsyncTick.load(),sourcePc,
        targetPc,ctx->pc,traceA0,traceA1,GPR_U32(ctx,2),rdram);

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // The callee stopped at a scheduler checkpoint: ctx->pc is where it must
    // resume, and the dispatcher continues there. This must be checked before
    // the stub heuristic below, because a loop that branches back to the
    // callee's own entry yields with ctx->pc == entryPc. Treating that as a
    // return resumed the caller with the loop unfinished and a stale v0 (GT3
    // 25a258 returned 0, so the race's GS packet count/pointer became garbage
    // and VIF1 deadlocked on the corrupt display list).
    if (yielded)
    {
        return false;
    }

    // Native stubs may return without setting ctx->pc.
    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    {
        // GT3_VU0_RECORD=<dir>: count VCALLMS by (code image FNV-1a 64, start pc)
        // and save each new 4 KiB code image as <dir>/vu0_<hash>.bin (input for
        // translating VU0 microprograms); a summary every 4096 calls.
        static const char *recordDir = [] {
            const char *v = std::getenv("GT3_VU0_RECORD");
            return v && *v ? v : nullptr;
        }();
        if (recordDir)
        {
            static std::map<uint64_t, std::map<uint32_t, uint64_t>> calls;
            static uint64_t total = 0;
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t i = 0; i < PS2_VU0_CODE_SIZE; ++i)
                hash = (hash ^ vu0Code[i]) * 1099511628211ull;
            auto &entries = calls[hash];
            if (entries.empty())
            {
                char name[64];
                std::snprintf(name, sizeof(name), "/vu0_%016llx.bin", static_cast<unsigned long long>(hash));
                if (std::FILE *f = std::fopen((std::string(recordDir) + name).c_str(), "wb"))
                {
                    std::fwrite(vu0Code, 1, PS2_VU0_CODE_SIZE, f);
                    std::fclose(f);
                }
            }
            ++entries[startPC];
            if ((++total & 4095u) == 0u)
            {
                std::cerr << "[vu0-record] " << total << " calls:";
                for (const auto &[h, pcs] : calls)
                    for (const auto &[pc, n] : pcs)
                        std::cerr << ' ' << std::hex << h << ':' << pc << std::dec << '=' << n;
                std::cerr << std::endl;
            }
        }
    }
    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
#if defined(PS2X_GAME_GT4)
    // GT4's VU0 microprograms can run far longer than 4096 cycles; a cut
    // program leaves its results unwritten. Run to the E bit (bounded).
    constexpr uint32_t kVu0Budget = 1u << 20;
#else
    constexpr uint32_t kVu0Budget = 4096;
#endif
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, kVu0Budget);
#if defined(PS2X_GAME_GT4)
    if (m_vu0.midProgram())
    {
        static uint32_t cutLogs = 0;
        if (cutLogs++ < 16)
            std::cerr << "[vu0] microprogram at 0x" << std::hex << startPC << " cut at pc 0x" << m_vu0.state().pc << std::dec
                      << " after " << kVu0Budget << " cycles" << std::endl;
    }
#endif
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    m_memory.serviceVif1Worker();
    if (m_memory.m_vif1VpuStop.load(std::memory_order_relaxed) != 0u)
    {
        const uint32_t stop = m_memory.m_vif1VpuStop.exchange(0u, std::memory_order_acq_rel);
        R5900Context *context = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
        if (stop != 0u && context)
            context->vu0_vpu_stat = (context->vu0_vpu_stat & ~0x0600u) | (stop & 0x0600u);
    }
    const uint32_t vifInterrupts = m_memory.consumeVifInterrupts();
    if (vifInterrupts & 2u)
    {
        extern void vif1latNoteNotice();
        vif1latNoteNotice();
    }
    for (uint32_t channel = 0; channel < 2u; ++channel)
        if ((vifInterrupts & (1u << channel)) != 0u)
            eeScheduler().dispatchIrq(false, 4u + channel);
    if (m_memory.consumeGsInterrupts() != 0u)
        eeScheduler().dispatchIrq(false, 0u);
#if defined(PS2X_GAME_GT4)
    if (m_memory.consumeIpuInterrupts() != 0u)
        eeScheduler().dispatchIrq(false, 8u); // INTC_IPU
#endif
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

void PS2Runtime::resetAsyncCallbackStacks()
{
    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    m_asyncCallbackStackTop = m_asyncCallbackStackLimit;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

// GT3_VIF1_THREAD_STATS: EE-thread time in 32-bit stores outside RAM (DMA kicks gather their data
// there: time the EE does not reach a checkpoint), by address, every 10 s.
static void noteSlowStore32(uint32_t vaddr, uint64_t ns)
{
    static const bool on = [] {
        const char *s = std::getenv("GT3_VIF1_THREAD_STATS");
        return s && *s && *s != '0';
    }();
    if (!on)
        return;
    static std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> byAddress; // count, ns
    static auto windowStart = std::chrono::steady_clock::now();
    auto &e = byAddress[vaddr & 0x1FFFFFFFu];
    ++e.first;
    e.second += ns;
    const auto now = std::chrono::steady_clock::now();
    if (now - windowStart < std::chrono::seconds(10))
        return;
    windowStart = now;
    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> rows(byAddress.begin(), byAddress.end());
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
    std::string line = "[ee-store32] 10 s, slowest addresses (count, ms):";
    for (size_t i = 0; i < rows.size() && i < 8u; ++i)
    {
        char part[64];
        std::snprintf(part, sizeof(part), " %08x %llu %.1f", rows[i].first, (unsigned long long)rows[i].second.first, rows[i].second.second / 1e6);
        line += part;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    byAddress.clear();
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        static const bool timeStores = [] {
            const char *s = std::getenv("GT3_VIF1_THREAD_STATS");
            return s && *s && *s != '0';
        }();
        const auto t0 = timeStores ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool io = (vaddr & 0x1FFFFFFFu) >= 0x10000000u;
        if (io)
            vif1lat::eeActivity.store(1, std::memory_order_relaxed);
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
        if (io)
            vif1lat::eeActivity.store(0, std::memory_order_relaxed);
        if (timeStores && (vaddr & 0x1FFFFFFFu) >= 0x10000000u)
            noteSlowStore32(vaddr, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    m_gsWorker.sync(); // the native chain paths write the GS directly
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::exchangeEeYieldTaken(bool value) noexcept
{
    const bool previous = g_eeYieldTaken;
    g_eeYieldTaken = value;
    return previous;
}

void PS2Runtime::eeNoYield(int delta) noexcept
{
    g_eeNoYield += delta;
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    if (g_eeNoYield != 0)
        return false;
    const bool due = m_eeScheduler->checkpointDue(cycles);
    if (due)
        g_eeYieldTaken = true; // generated code now returns with ctx->pc = its resume point
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

#if !defined(PS2X_GAME_GT3)
namespace
{
// GT4: the host loop presents each game frame once, on the EE's VBlank grid
// (analysis/phase5/specs/step3-pacing.md 6). raylib's SetTargetFPS(60) is a
// relative limiter (59.88-59.93 Hz here) running free against the EE's
// 59.9988 Hz VBlank: where their phases met, one frame was shown twice and the
// next never (up to 57 of each per 5 s in the race, 72 in the movie).
// Here slot n, the frame latched at VBlank n, is swapped at T_n = t_n + D: t_n
// the VBlank's grid time, D one period by default (PS2X_PRESENT_DELAY_US),
// which leaves room for its lateness and the GS publish delay. Each loop:
// - waitForFrame (loop top): picks the slot and waits until its frame is ready
//   (ps2x::vblank::presentationReady: CPU- or GPU-composed, counted), at most
//   until T_n + half a period. The loop then copies it at once (UploadFrame):
//   frame n+1 is latched only at t_(n+1), so the copy is frame n even from the
//   CPU renderer's single buffer, unless the GS publishes more than a period
//   late (a GPU frame is also picked by latch tick, so then too);
// - beforeSwap: waits until T_n, then EndDrawing swaps (GT4 --headless with
//   the display off: a flush, no swap; T_n is still the present time);
// - presented: the next slot is n + 1, whatever was shown. A host that fell a
//   period or more behind skips to the newest slot whose time has come (no
//   fast replay); while the EE delivers no new VBlank (a stall) the loop runs
//   on its own clock, a period apart.
// Game timing is untouched: the EE paces itself and the GS never waits for the
// host loop. The GL driver still couples them: a host swap that blocks was
// seen to hold the GS GL thread inside the driver, and through
// GsWorker::postFrame the EE (display off; startHeadlessSwap).
// PS2X_PRESENT_GRID=0: raylib's limiter, as before.
struct GridPacer
{
    using Clock = std::chrono::steady_clock;
    bool enabled = false;
    bool log = false; // GT3_PACING_LOG=1: a "[pacing] grid" line every 5 s
    Clock::duration period = std::chrono::microseconds(16667);
    Clock::duration delay = std::chrono::microseconds(16667);
    uint64_t want = 0;     // the slot (VSync) the next present shows; 0 before the grid
    uint64_t lastSlot = 0; // the slot of the last present (0: none)
    Clock::time_point target{}, last{};
    uint64_t presents = 0, late = 0, missed = 0, skipped = 0, stalled = 0;
    std::vector<float> offsetsMs; // present time - target
    Clock::time_point windowStart{};

    GridPacer()
    {
        const char *v = std::getenv("PS2X_PRESENT_GRID");
        enabled = !(v && *v == '0');
        if (const char *d = std::getenv("PS2X_PRESENT_DELAY_US"); d && *d)
            delay = std::chrono::microseconds(std::clamp(std::atol(d), 0L, 50000L));
        const char *pl = std::getenv("GT3_PACING_LOG");
        log = pl && *pl == '1';
    }

    // Loop top: the slot of this loop's present and its time (target), then
    // its frame. streamed: the GS worker latches the frames (otherwise the
    // loop latches them itself in UploadFrame, so there is nothing to wait for).
    void waitForFrame(bool streamed)
    {
        const auto now = Clock::now();
        uint64_t k = 0;
        Clock::time_point due{}, nextDue{};
        bool stall = true;
        if (ps2x::vblank::grid(k, due, nextDue))
        {
            // t_n (the newest VBlank's due time; later ones on its next grid).
            const auto gridTime = [&](uint64_t n) {
                return n <= k ? due - period * static_cast<int64_t>(k - n) : nextDue + period * static_cast<int64_t>(n - k - 1);
            };
            if (want == 0 || lastSlot > k + 1)
            {
                // The first slot, or the VSync count went back (a reset).
                want = k;
                lastSlot = 0;
            }
            if (want < k && gridTime(want) + delay + period <= now)
            {
                // A period or more behind: the newest slot whose time has come.
                uint64_t n = k + 1;
                while (n > want && gridTime(n) + delay > now)
                    --n;
                skipped += n - want;
                want = n;
            }
            if (want > k + 1)
                want = k + 1; // the EE has not reached it
            stall = lastSlot != 0 && want <= lastSlot;
            stalled += stall ? 1u : 0u;
            target = stall ? last + period : gridTime(want) + delay;
        }
        else
            target = last == Clock::time_point{} ? now : last + period;
        // Never two presents within half a period, never more than two
        // periods away.
        if (last != Clock::time_point{})
            target = std::max(target, last + period / 2);
        target = std::min(target, now + 2 * period);
        if (streamed && !stall && ps2x::vblank::readyPresentation() < want)
        {
            // Not ready by T_n: shown as soon as it is (late), or the newest
            // after half a period (missed).
            if (!ps2x::vblank::waitForPresentation(want, target + period / 2))
                ++missed;
            else if (Clock::now() > target)
                ++late;
        }
    }

    // Right before the swap (EndDrawing): the frame's draws go to the GPU,
    // then the wait for T_n.
    void beforeSwap()
    {
        rlDrawRenderBatchActive();
        ps2x::vblank::sleepUntil(target);
        last = Clock::now();
        if (log && offsetsMs.size() < 4096u)
            offsetsMs.push_back(static_cast<float>(std::chrono::duration<double, std::milli>(last - target).count()));
    }

    // After the swap: the next slot.
    void presented(uint64_t vsync)
    {
        if (want != 0)
        {
            lastSlot = want;
            ++want;
        }
        ++presents;
        if (!log)
            return;
        if (windowStart == Clock::time_point{})
            windowStart = last;
        if (last - windowStart < std::chrono::seconds(5))
            return;
        std::sort(offsetsMs.begin(), offsetsMs.end());
        const double p99 = offsetsMs.empty() ? 0.0 : offsetsMs[std::min(offsetsMs.size() - 1u, (offsetsMs.size() * 99u) / 100u)];
        const double maxMs = offsetsMs.empty() ? 0.0 : offsetsMs.back();
        std::fprintf(stderr,
                     "[pacing] grid: %llu presents, %llu late (frame ready after its present time), %llu missed (not ready within half a "
                     "period), %llu slots skipped (host behind), %llu on the host clock (EE stalled); present - grid time p99 %.2f ms, max %.2f ms "
                     "(vsync %llu)\n",
                     static_cast<unsigned long long>(presents), static_cast<unsigned long long>(late), static_cast<unsigned long long>(missed),
                     static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(stalled), p99, maxMs,
                     static_cast<unsigned long long>(vsync));
        presents = late = missed = skipped = stalled = 0;
        offsetsMs.clear();
        windowStart = last;
    }
};
}
#endif

void PS2Runtime::logFrameRate(uint64_t hostPresents)
{
    // A game frame is the time between two DISPFB flips, timed on the EE
    // (FrameLog). The game's physics steps once per frame, so one frame over
    // 16.7 ms is slow motion: "max" (the longest gap in the period, or the
    // time since the last flip if longer) is the number to watch, not the
    // average.
    const double period = frameLogPeriod();
    if (period <= 0.0)
        return;
    static std::chrono::steady_clock::time_point last{};
    static uint64_t lastFlips = 0, lastTick = 0, lastPresents = 0;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t vsync = m_memory.gs().vsyncTick.load(std::memory_order_acquire); // the EE's, published each VSync
    FrameLog &frameLog = g_frameLog;
    const bool first = last == std::chrono::steady_clock::time_point{};
    if (!first && std::chrono::duration<double>(now - last).count() < period)
        return;
    uint64_t flips = 0, gaps = 0, maxVSyncs = 0;
    int64_t sumNs = 0, maxNs = 0;
    {
        std::lock_guard<std::mutex> lock(frameLog.mutex);
        flips = frameLog.flips;
        gaps = frameLog.gaps;
        sumNs = frameLog.sumNs;
        maxNs = frameLog.maxNs;
        maxVSyncs = frameLog.maxVSyncs;
        if (flips != 0u)
        {
            maxNs = std::max(maxNs, steadyNowNs() - frameLog.lastFlipNs);
            maxVSyncs = std::max(maxVSyncs, vsync > frameLog.lastFlipTick ? vsync - frameLog.lastFlipTick : 0u);
        }
        frameLog.gaps = frameLog.maxVSyncs = 0;
        frameLog.sumNs = frameLog.maxNs = 0;
        if (first)
        {
            last = now;
            lastFlips = flips;
            lastTick = vsync;
            lastPresents = hostPresents;
            return;
        }
    }
    const double seconds = std::chrono::duration<double>(now - last).count();
    std::fprintf(stderr, "[fps] game %.1f (avg %.1f ms, max %.1f ms) vsync %.1f presents %.1f at vsync %llu; longest frame %llu vsyncs\n",
                 static_cast<double>(flips - lastFlips) / seconds, gaps ? static_cast<double>(sumNs) / 1e6 / static_cast<double>(gaps) : 0.0,
                 static_cast<double>(maxNs) / 1e6, static_cast<double>(vsync - lastTick) / seconds,
                 static_cast<double>(hostPresents - lastPresents) / seconds, static_cast<unsigned long long>(vsync),
                 static_cast<unsigned long long>(maxVSyncs));
    // VBlankStart lateness on the EE (when processed minus its grid time) and
    // lag-cap rebases (a VBlank over 4 periods late moves the grid).
    const ps2x::vblank::LatenessWindow late = ps2x::vblank::takeLateness();
    if (late.count != 0u)
        std::fprintf(stderr, "[vblank] %llu late p50 %.2f p99 %.2f max %.2f ms, lag-cap rebases %llu (vsync %llu)\n",
                     static_cast<unsigned long long>(late.count), late.p50Ms, late.p99Ms, late.maxMs, static_cast<unsigned long long>(late.rebases),
                     static_cast<unsigned long long>(vsync));
#if !defined(PS2X_GAME_GT3)
    // VBlankStarts delivered on the host clock (PS2X_IPU_VBLANK / PS2X_VBLANK_HOST_CLOCK).
    if (ps2x::vblank::ipuVBlankEnabled() || ps2x::vblank::hostClockVBlankEnabled())
    {
        const ps2x::vblank::HostClockWindow host = ps2x::vblank::takeHostClock();
        std::fprintf(stderr, "[vblank] host clock: %llu delivered (IPU %llu, timer %llu) of %llu requests, EE cycles fast-forwarded avg %.0f max %.0f, "
                             "%llu passed earlier deadlines or EE timer interrupts on the way (max %u steps) (vsync %llu)\n",
                     static_cast<unsigned long long>(host.ipu + host.timer), static_cast<unsigned long long>(host.ipu),
                     static_cast<unsigned long long>(host.timer), static_cast<unsigned long long>(host.requests), host.avgCycles, host.maxCycles,
                     static_cast<unsigned long long>(host.multiStep), host.maxSteps, static_cast<unsigned long long>(vsync));
    }
    // EE timer interrupts in idle waits (PS2X_IDLE_TIMER_HOLD / PS2X_IDLE_CHARGE,
    // analysis/phase5/specs/step6-l1-steploss.md 7.2).
    {
        const ps2x::vblank::IdleTimerWindow idle = ps2x::vblank::takeIdleTimers();
        if (idle.held != 0u || idle.expired != 0u || idle.charges != 0u)
            std::fprintf(stderr, "[vblank] idle timers: %llu waits held for the VBlank, %llu expired (%llu while the cycle clock trailed); "
                                 "%llu event-ended waits charged %.1f ms (vsync %llu)\n",
                         static_cast<unsigned long long>(idle.held), static_cast<unsigned long long>(idle.expired),
                         static_cast<unsigned long long>(idle.expiredTrailing), static_cast<unsigned long long>(idle.charges),
                         static_cast<double>(idle.chargedCycles) * 1000.0 / static_cast<double>(EeScheduler::kEeClockHz), static_cast<unsigned long long>(vsync));
    }
#endif
    last = now;
    lastFlips = flips;
    lastTick = vsync;
    lastPresents = hostPresents;
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    resetIop();
    ps2_stubs::resetSifState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    gt4pages::start(); // GT4_PAGES: the pages service (off by default)
#endif

    // A blank image to use as a framebuffer (GPU frames at an internal
    // resolution are presentScale() x presentScaleY() times larger).
#if defined(PS2X_GS_GL)
    const int frameScale = gsgl::presentScale(), frameScaleY = gsgl::presentScaleY();
#else
    const int frameScale = 1, frameScaleY = 1;
#endif
    Image blank = GenImageColor(FB_WIDTH * frameScale, FB_HEIGHT * frameScaleY, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    // Frame generation's second frame texture (GT3_FRAME_GEN): new frames go
    // into the spare one, which then becomes the current one.
    Texture2D fgSpare = LoadTextureFromImage(blank);
#if GT3_ADDONS
    // GT3_PHONE_VIEW: the rear-view mirror on a phone (gt3_phone_view.h).
    const bool phoneView = phoneview::start(m_memory.getRDRAM());
    if (phoneView)
        GS::setInsetWatch(true);
    phoneview::Capture phoneCapture;
    // GT3_HUD_HIDE=<bits> (the launcher's HUD page; live key hud_hide): GT3's
    // race HUD groups hidden - 1 position and map, 2 lap times, 4 gauges and
    // lights, 8 the rear-view mirror (GS::HudHide).
    // The phone's mirror panel keeps the mirror drawn (applied each frame
    // below: the phone's picture is cut out of the game's).
    uint32_t userHudHide = 0u, appliedHudHide = ~0u;
    if (const char *v = std::getenv("GT3_HUD_HIDE"); v && *v)
        userHudHide = static_cast<uint32_t>(std::atoi(v)) & 15u;
    // Triple screens (the launcher's Triple screens page): the bezel gap in
    // screen widths (GT3_TRIPLE_BEZEL) and the screens' shape
    // (GT3_TRIPLE_MONITOR_ASPECT, default 16:9); the HUD's sides go to the
    // middle screen's edges.
    const float tripleBezel = [] {
        const char *v = std::getenv("GT3_TRIPLE_BEZEL");
        return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 0.0f, 0.5f) : 0.0f;
    }();
    {
        const char *m = std::getenv("GT3_TRIPLE_MONITOR_ASPECT");
        float monitorAspect = 16.0f / 9.0f;
        if (m && *m)
        {
            const char *colon = std::strchr(m, ':');
            const float a = colon ? static_cast<float>(std::atof(m)) / std::max(0.01f, static_cast<float>(std::atof(colon + 1)))
                                  : static_cast<float>(std::atof(m));
            if (a > 0.5f && a < 5.0f)
                monitorAspect = a;
        }
        if (tripleBezel > 0.0f || (m && *m))
        {
            GS::setHudMiddleScreen(1.0f / (3.0f + 2.0f * tripleBezel));
            std::cerr << "[triple] screens " << monitorAspect << ":1, bezel gap " << tripleBezel << " screen widths" << std::endl;
        }
    }
    uint64_t phoneUploads = 0;
    double phoneLast = 0.0;
    constexpr bool tripleScreens = false;
#elif defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // GT4_SCREENS=3 (gt4screens, full-m2 "Multi"): the race picture spans three screens and the gaps between their pictures;
    // each third of the window shows its screen's part (the pieces below, the same as GT3's triple screens), filling the
    // screens' height. GT4's menus (16:9) are shown in the middle, on the middle screen.
    const float tripleBezel = gt4screens::config().on ? gt4screens::config().gap : 0.0f;
    const bool tripleScreens = gt4screens::config().on;
#else
    constexpr float tripleBezel = 0.0f;
    constexpr bool tripleScreens = false;
#endif
    // How the frame is scaled to the window (GT3_DISPLAY_FILTER):
    // - bilinear (default): smooth;
    // - sharp: "sharp bilinear", each PS2 pixel a crisp block at the largest
    //   whole-number scale that fits, blended only at its edges (no uneven
    //   pixels at a scale like 3.6x);
    // - nearest: point sampling (uneven, blocky pixels at non-integer scales);
    // - fsr / nis: AMD FSR 1 or NVIDIA Image Scaling (present_upscale.cpp),
    //   edge-aware upscaling plus sharpening (GT3_UPSCALE_SHARPNESS 0-100);
    // - crt: a TV (CRT) look - the analog signal's horizontal softness,
    //   GT3's 448 lines as scanlines, a light aperture-grille mask
    //   (GT3_CRT_SCANLINES / GT3_CRT_MASK 0-100, defaults 60 / 30).
    enum class DisplayFilter
    {
        Bilinear,
        Sharp,
        Nearest,
        Fsr1,
        Nis,
        Crt
    };
    DisplayFilter displayFilter = [] {
#if defined(GT4_STOCK_BUILD)
        return DisplayFilter::Bilinear; // the stock picture goes through the scanline pass (below)
#else
        const char *v = std::getenv("GT3_DISPLAY_FILTER");
        if (v && std::strcmp(v, "nearest") == 0)
            return DisplayFilter::Nearest;
        if (v && std::strcmp(v, "sharp") == 0)
            return DisplayFilter::Sharp;
        if (v && std::strcmp(v, "crt") == 0)
            return DisplayFilter::Crt;
        if (v && std::strcmp(v, "fsr") == 0)
            return DisplayFilter::Fsr1;
        if (v && std::strcmp(v, "nis") == 0)
            return DisplayFilter::Nis;
        return DisplayFilter::Bilinear;
#endif
    }();
    float upscaleSharpness = [] {
        const char *v = std::getenv("GT3_UPSCALE_SHARPNESS");
        return v && *v ? std::clamp(std::atoi(v), 0, 100) / 100.0f : 0.5f;
    }();
#if GT3_ADDONS
    // HUD units (GT3_UNITS_SPEED kmh|mph, GT3_UNITS_POWER ps|hp|bhp|cv|kw,
    // GT3_UNITS_TORQUE kgfm|ftlb|nm; live keys units_speed/power/torque with
    // the game's own numbers). GT3 USA keeps its units in one global,
    // 0x2f59d8 {speed and distance: 0 km/h+km, 1 mph+miles; power: 0 PS,
    // 1 HP, 2 BHP, 3 CV, 4 kW; torque: 0 kgf.m, 1 ft.lb, 2 NM}, set at boot
    // from the language (sub_127908 -> sub_245820). Values are stored in
    // km/h, PS and kgf.m and converted when drawn (0x245a18/58/b0), so the
    // numbers and the labels both follow. Written each frame when it differs,
    // and only when this CORE has GT3's unit strings where expected.
    std::array<int, 3> hudUnits = [] {
        std::array<int, 3> u{-1, -1, -1};
        auto pick = [](const char *env, std::initializer_list<const char *> names) {
            const char *v = std::getenv(env);
            std::string value = v ? v : "";
            for (char &c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int i = 0;
            for (const char *n : names)
            {
                if (value == n)
                    return i;
                ++i;
            }
            return -1;
        };
        u[0] = pick("GT3_UNITS_SPEED", {"kmh", "mph"});
        u[1] = pick("GT3_UNITS_POWER", {"ps", "hp", "bhp", "cv", "kw"});
        u[2] = pick("GT3_UNITS_TORQUE", {"kgfm", "ftlb", "nm"});
        return u;
    }();
    constexpr uint32_t kGt3Units = 0x2f59d8;
    int hudUnitsCore = -1; // -1 not checked yet, 0 not GT3 USA's CORE, 1 ok
    auto applyHudUnits = [&]() {
        if (hudUnits[0] < 0 && hudUnits[1] < 0 && hudUnits[2] < 0)
            return;
        uint8_t *const rdram = m_memory.getRDRAM();
        if (hudUnitsCore < 0)
        {
            if (std::memcmp(rdram + 0x353028, "km/h", 5) != 0)
                return; // CORE not loaded yet
            hudUnitsCore = std::memcmp(rdram + 0x353030, "mph", 4) == 0 && std::memcmp(rdram + 0x353040, "PS", 3) == 0 &&
                                   std::memcmp(rdram + 0x353060, "kW", 3) == 0 && std::memcmp(rdram + 0x353070, "kgf.m", 6) == 0 &&
                                   std::memcmp(rdram + 0x353080, "NM", 3) == 0
                               ? 1
                               : 0;
            std::cerr << "[units] " << (hudUnitsCore ? "GT3 units global 0x2f59d8 in use" : "unit strings differ; units left to the game")
                      << std::endl;
        }
        if (hudUnitsCore != 1)
            return;
        for (int i = 0; i < 3; ++i)
        {
            if (hudUnits[i] < 0)
                continue;
            int32_t current;
            std::memcpy(&current, rdram + kGt3Units + 4 * i, 4);
            if (current != hudUnits[i])
            {
                const int32_t want = hudUnits[i];
                std::memcpy(rdram + kGt3Units + 4 * i, &want, 4);
            }
        }
    };
    // GT3_PEDALS=triggers (ps2_input_profile.h): GT3's Key Configuration
    // for player 1 is held at Acceleration = right stick up, Brakes = right
    // stick down, GT3's own analog pedals, which the triggers drive. GT3 USA
    // keeps the 1P table at 0x9894a0 as ten u32 (steering, acceleration,
    // brakes, rear gear, emergency brakes, shift up, shift down, rear view,
    // change views, ?; defaults 1 c e d b 10 12 11 f 13; codes: 7 right stick
    // up, 9 right stick down, b circle, c cross, d triangle, e square, f R1,
    // 10 R2, 11 L1, 12 L2), 2P at 0x9894c8; its Key Configuration screen edits
    // a copy and writes the table on leaving (RAM dumps `kc*`, 2026-09-30).
    // Written each frame when it differs (a card load or the Options screen
    // can change it), only once the table looks like one; the other rows stay
    // the player's.
    const bool pedalsOnTriggers = ps2_pad::pedalsOnTriggers();
    bool pedalsLogged = false;
    // GT3_KEYCONFIG_1P=<accel hex>,<brakes hex> (tests): GT3's Key
    // Configuration rows for Acceleration / Brakes written like the pedals
    // below (e.g. 10,12 = R2, L2), to test a layout without its menus.
    static const std::pair<uint32_t, uint32_t> testKeys = [] {
        uint32_t a = 0, b = 0;
        const char *v = std::getenv("GT3_KEYCONFIG_1P");
        if (v && std::sscanf(v, "%x,%x", &a, &b) == 2 && a && b && a < 0x30 && b < 0x30)
            return std::make_pair(a, b);
        return std::make_pair(0u, 0u);
    }();
    auto applyPedals = [&]() {
        if (testKeys.first && hudUnitsCore != 0 && std::memcmp(m_memory.getRDRAM() + 0x353028, "km/h", 5) == 0)
        {
            uint8_t *const rdram = m_memory.getRDRAM();
            uint32_t table[10];
            std::memcpy(table, rdram + 0x9894a0, sizeof(table));
            bool ready = table[0] != 0u || table[1] != 0u;
            for (const uint32_t v : table)
                ready = ready && v <= 0x30u;
            if (ready)
            {
                std::memcpy(rdram + 0x9894a4, &testKeys.first, 4);
                std::memcpy(rdram + 0x9894a8, &testKeys.second, 4);
            }
            return;
        }
        if (!pedalsOnTriggers || hudUnitsCore == 0)
            return;
        uint8_t *const rdram = m_memory.getRDRAM();
        if (std::memcmp(rdram + 0x353028, "km/h", 5) != 0)
            return; // CORE not loaded yet (the same check as the units)
        constexpr uint32_t kKeyConfig1P = 0x9894a0;
        uint32_t table[10];
        std::memcpy(table, rdram + kKeyConfig1P, sizeof(table));
        for (const uint32_t v : table)
            if (v > 0x30u)
                return; // not set up yet
        if (table[0] == 0u && table[1] == 0u)
            return;
        if (table[1] != 7u || table[2] != 9u)
        {
            const uint32_t accel = 7u, brakes = 9u;
            std::memcpy(rdram + kKeyConfig1P + 4, &accel, 4);
            std::memcpy(rdram + kKeyConfig1P + 8, &brakes, 4);
            if (!pedalsLogged)
                std::cerr << "[pedals] GT3 Key Configuration 1P: Acceleration = right stick up, Brakes = right stick down (was "
                          << std::hex << table[1] << " / " << table[2] << std::dec << "); the triggers drive them" << std::endl;
            pedalsLogged = true;
        }
    };
#else
    auto applyHudUnits = [] {};
    auto applyPedals = [] {};
#endif
#if defined(PS2X_UPSCALERS)
    // Compiled on first use (FSR 1 / NIS chosen at start or while playing).
    DisplayUpscaler upscaler;
    bool upscalerTried = false;
    // Per-pixel motion of the picture (present_motion.h; F7 shows it,
    // GT3_MOTION_VIEW=1 at start): groundwork for frame generation.
    MotionBuffer motionBuffer;
    bool motionView = [] {
        const char *v = std::getenv("GT3_MOTION_VIEW");
        return v && *v && *v != '0';
    }();
    bool motionBufferReady = false, motionBufferTried = false;
    uint64_t motionSerial = 0;
    std::vector<gt3motion::Triangle> motionTriangles;
    // Frame generation (GT3_FRAME_GEN=90 or 120, any rate 61-360): the window
    // shows that many pictures a second while GT3 runs at 60; the pictures
    // between two game frames are warped along the motion buffer
    // (present_motion.h), one game frame behind (latency ~17 ms).
    const int frameGenRate = [] {
        const char *v = std::getenv("GT3_FRAME_GEN");
        const int r = v && *v ? std::atoi(v) : 0;
        return r > 60 && r <= 360 ? r : 0;
    }();
    FrameGenerator frameGen;
    bool frameGenReady = false, frameGenTried = false;
    double fgPrevArrival = -1.0, fgCurArrival = -1.0;
    uint64_t fgMotionTick = 0, fgMotionLate = 0, fgGenerated = 0, fgCuts = 0;
    bool fgMotionOk = false, fgOverlayDue = false, fgUsable = false;
    if (frameGenRate)
    {
        SetTargetFPS(frameGenRate);
        std::cerr << "[frame-gen] " << frameGenRate << " pictures a second from GT3's 60" << std::endl;
    }
#endif
    auto applyDisplayFilter = [&]() {
        g_displayTextureFilter = displayFilter == DisplayFilter::Nearest ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR;
        SetTextureFilter(frameTex, g_displayTextureFilter);
        SetTextureFilter(fgSpare, g_displayTextureFilter);
    };
    applyDisplayFilter();
    // The picture's shape (GT3_DISPLAY_ASPECT): 4:3 by default, as a TV shows
    // GT3's 640x448 (its pixels are not square); "pixels" keeps square pixels
    // (640:448, about 7% wider than 4:3, as before).
    bool squarePixels = [] {
#if defined(GT4_STOCK_BUILD)
        return false; // the stock picture: shaped as a TV showed it (4:3, GT4's 16:9 or 1080i's 1.8:1)
#else
        const char *v = std::getenv("GT3_DISPLAY_ASPECT");
        return v && std::strcmp(v, "pixels") == 0;
#endif
    }();
    // FPS / frame time overlay (F3 toggles; GT3_FPS_OVERLAY=0 starts it off).
#if GT3_ADDONS
    bool overlay = [] {
        const char *v = std::getenv("GT3_FPS_OVERLAY");
        return !(v && *v == '0');
    }();
#else
    bool overlay = false; // the stock build: never (no F3)
#endif
    Shader sharpShader{};
    int sharpTexSizeLoc = -1, sharpScaleLoc = -1;
    // (Loaded in every case: the filter can change while the game runs.)
    {
        static const char *kSharpBilinear = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 texSize;  // source texture size in texels
uniform vec2 outScale; // window pixels per source texel
out vec4 finalColor;
void main()
{
    vec2 texel = fragTexCoord * texSize;
    vec2 scale = max(floor(outScale), vec2(1.0));
    vec2 base = floor(texel);
    vec2 centre = fract(texel) - 0.5;
    vec2 range = 0.5 - 0.5 / scale;
    vec2 f = (centre - clamp(centre, -range, range)) * scale + 0.5;
    finalColor = texture(texture0, (base + f) / texSize) * colDiffuse * fragColor;
}
)GLSL";
        sharpShader = LoadShaderFromMemory(nullptr, kSharpBilinear);
        sharpTexSizeLoc = GetShaderLocation(sharpShader, "texSize");
        sharpScaleLoc = GetShaderLocation(sharpShader, "outScale");
    }
    // The TV (CRT) look (GT3_DISPLAY_FILTER=crt). A PS2 drew GT3's fields on
    // an interlaced TV through an analog cable: soft across, lit in lines,
    // seen through the tube's phosphor stripes. Everything in linear light:
    // - across: a 5-tap blur over about one PS2 pixel (composite / S-video);
    // - down: each of GT3's lines is a beam whose height grows with its
    //   brightness (bright lines bloom, dark ones leave gaps), from the two
    //   nearest lines;
    // - an aperture grille (R, G, B stripes one window pixel each), then the
    //   brightness lost to lines and mask is given back.
    // Lines and mask fade out when the window has too few pixels per PS2
    // line / pixel for them (they would only make moire).
    Shader crtShader{};
    int crtLinesLoc = -1, crtOutLoc = -1, crtScanLoc = -1, crtMaskLoc = -1;
    // On from GT3_DISPLAY_FILTER=crt; F1 turns it on and off while playing
    // (then over the chosen filter's plain picture).
    bool crtOn = displayFilter == DisplayFilter::Crt;
    const float crtScanlines = [] {
        const char *v = std::getenv("GT3_CRT_SCANLINES");
        return v && *v ? std::clamp(std::atoi(v), 0, 100) / 100.0f : 0.6f;
    }();
    const float crtMask = [] {
        const char *v = std::getenv("GT3_CRT_MASK");
        return v && *v ? std::clamp(std::atoi(v), 0, 100) / 100.0f : 0.3f;
    }();
    {
        static const char *kCrt = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 lines;     // the picture in PS2 pixels (640 x 448 for GT3)
uniform vec2 outSize;   // its size in window pixels
uniform float scan;     // scanline strength 0..1
uniform float mask;     // aperture-grille strength 0..1
out vec4 finalColor;

vec3 lin(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

// One PS2 line (its centre), softened across like an analog signal.
vec3 line(float row, float u)
{
    float v = (clamp(row, 0.0, lines.y - 1.0) + 0.5) / lines.y;
    float dx = 0.5 / lines.x;
    vec3 c = lin(texture(texture0, vec2(u, v)).rgb) * 0.36;
    c += lin(texture(texture0, vec2(u - dx, v)).rgb) * 0.22;
    c += lin(texture(texture0, vec2(u + dx, v)).rgb) * 0.22;
    c += lin(texture(texture0, vec2(u - 2.0 * dx, v)).rgb) * 0.10;
    c += lin(texture(texture0, vec2(u + 2.0 * dx, v)).rgb) * 0.10;
    return c;
}

// The beam of a line of colour c at distance d (in lines): a gaussian, wider
// when brighter, scaled so its average over a line spacing is c (a flat
// field keeps its brightness; bright lines fill, dark ones leave gaps).
vec3 beam(vec3 c, float d)
{
    vec3 width = mix(vec3(0.24), vec3(0.42), sqrt(clamp(c, 0.0, 1.0)));
    return c * exp(-0.5 * (d * d) / (width * width)) / (width * 2.5066);
}

void main()
{
    vec2 uv = fragTexCoord;
    float perLine = outSize.y / lines.y;    // window pixels per PS2 line
    float perPixel = outSize.x / lines.x;   // window pixels per PS2 pixel
    float s = scan * clamp(perLine - 1.5, 0.0, 1.0);    // full from 2.5 window pixels a line
    float m = mask * clamp(perPixel - 1.5, 0.0, 1.0);   // and from 2.5 a pixel

    float y = uv.y * lines.y - 0.5;
    float row = floor(y);
    float f = y - row;
    vec3 a = line(row, uv.x), b = line(row + 1.0, uv.x);
    // Scanlines: the two nearest beams, blended with the plain picture by
    // the strength.
    vec3 lit = beam(a, f) + beam(b, 1.0 - f);
    vec3 c = mix(mix(a, b, f), lit, s);

    // Aperture grille: every third window column favours R, G or B; its
    // average (0.81) is given back.
    int col = int(mod(floor(gl_FragCoord.x), 3.0));
    vec3 stripe = col == 0 ? vec3(1.0, 0.72, 0.72) : (col == 1 ? vec3(0.72, 1.0, 0.72) : vec3(0.72, 0.72, 1.0));
    c *= mix(vec3(1.0), stripe / 0.8133, m);

    finalColor = vec4(pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0) * colDiffuse * fragColor;
}
)GLSL";
        crtShader = LoadShaderFromMemory(nullptr, kCrt);
        crtLinesLoc = GetShaderLocation(crtShader, "lines");
        crtOutLoc = GetShaderLocation(crtShader, "outSize");
        crtScanLoc = GetShaderLocation(crtShader, "scan");
        crtMaskLoc = GetShaderLocation(crtShader, "mask");
    }
#if defined(PS2X_SCANLINE_PASS)
    // GT4's TV look (present_scanline.h): the display lines as CRT beams.
    // GT3_DISPLAY_FILTER=crt: on at start; GT3_CRT_SCANLINES: strength 0-100
    // (default 50, faithful); GT4_SCANLINE_FIELD: field emphasis 0-100
    // (default 0); GT4_PICTURE_FIT: auto (default), fill, integer. The stock
    // build always draws through the pass (lines on unless GT3_DISPLAY_FILTER
    // names another filter). F1 turns the lines on and off.
    // GT4_PICTURE_FILTER: nearest, bilinear, bicubic, lanczos (the picture's
    // scaling; the earlier smooth and sharp read as bilinear; box: the
    // development look); GT4_DEINTERLACE: weave, bob, blend, adaptive (how
    // the two fields make one picture; the GS presenter pairs them,
    // gs_gl_present.cpp, and this pass blends the lines for blend). The
    // Classic edition: bilinear and blend unless set; a development build
    // (the Full edition's GT4.exe too) draws through the pass only when one
    // of the picture keys is set, else as before (box, clamp): its launcher
    // always writes GT4_PICTURE_FILTER and GT4_DEINTERLACE.
#if defined(GT4_STOCK_BUILD)
    constexpr bool scanStock = true;
#else
    constexpr bool scanStock = false;
#endif
    ps2x::scanline::Settings scanSettings =
        ps2x::scanline::parse(std::getenv("GT3_DISPLAY_FILTER"), std::getenv("GT3_CRT_SCANLINES"), std::getenv("GT4_SCANLINE_FIELD"),
                              std::getenv("GT4_PICTURE_FIT"), scanStock);
    ps2x::scanline::parsePicture(scanSettings, std::getenv("GT4_PICTURE_FILTER"), std::getenv("GT4_DEINTERLACE"), scanStock);
#if defined(PS2X_GS_GL)
    // The GS presenter pairs the fields (gs_gl_present.h deinterlaceMode): set
    // here, after gt4-start.env is in the environment. A development build
    // without GT4_DEINTERLACE keeps GT3_DEINTERLACE's choice.
    {
        ps2x::scanline::Deinterlace given = scanSettings.deinterlace;
        if (scanStock || ps2x::scanline::parseDeinterlace(std::getenv("GT4_DEINTERLACE"), given))
        {
            using DI = ps2x::scanline::Deinterlace;
            const DI d = scanSettings.deinterlace;
            gsgl::requestDeinterlace(d == DI::Weave ? 1 : d == DI::Bob ? 2 : d == DI::Blend ? 4 : d == DI::Adaptive ? 5 : 0);
        }
    }
#endif
    ps2x::scanline::Pass scanPass;
#if defined(PS2X_GS_VK)
    // The Vulkan renderer's window presenter (gs/gs_vk_present.h): the picture is drawn by
    // Vulkan; the overlays go into vkOverlay (premultiplied) and the rectangles they mark
    // (present_overlay.h) are drawn over it.
    const bool vkPresents = gsvk::present::active();
    RenderTexture2D vkOverlay{};
    bool vkOverlayOpen = false;
    std::string vkShotPath; // GT3_WINDOW_SHOT's file for this frame
    if (vkPresents)
        ps2x::overlay::hook() = &gsvk::present::overlayRect;
#endif
#if !defined(GT4_STOCK_BUILD)
    // Colour LUTs (GT4_LUT = off | gt3 | <.cube file>, GT4_LUT_STRENGTH 0..100; present_lut_math.h,
    // analysis/design/colour-lut): the Full edition's Vulkan picture only, applied in this pass before the TV lines (so a
    // LUT also brings the pass in). A file that cannot be read is logged once and the game runs without a LUT.
    // The launcher reads this marker from GT4.exe (gt4full_model.h kLutMarker) before it passes GT4_LUT.
    const char *const lutMarker = "GT4 colour LUTs (colour-lut)";
    ps2x::lut::Table lutTable;
    const ps2x::lut::Choice lutChoice = ps2x::lut::parseChoice(std::getenv("GT4_LUT"), std::getenv("GT4_LUT_STRENGTH"));
    if (lutChoice.badStrength)
        std::cerr << "[lut] GT4_LUT_STRENGTH=" << std::getenv("GT4_LUT_STRENGTH") << " is not 0..100: 100" << std::endl;
    if (lutChoice.kind != ps2x::lut::Kind::Off && !g_vulkanRendererSelected)
        std::cerr << "[lut] GT4_LUT is for the Vulkan renderer (GT3_RENDERER=vk): no LUT" << std::endl;
    else if (lutChoice.kind == ps2x::lut::Kind::Gt3)
        lutTable = ps2x::lut::gt3Look(33);
    else if (lutChoice.kind == ps2x::lut::Kind::File)
    {
        std::filesystem::path lutPath(lutChoice.path);
        if (lutPath.is_relative())
            lutPath = std::filesystem::path(GetApplicationDirectory()) / lutPath; // GT4.exe's folder
        std::error_code lutError;
        const auto lutBytes = std::filesystem::file_size(lutPath, lutError);
        std::string why;
        if (lutError || lutBytes > (512ull << 20))
            why = lutError ? "cannot be read" : "is larger than 512 MB";
        else
        {
            std::ifstream lutIn(lutPath, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(lutIn)), std::istreambuf_iterator<char>());
            if (!lutIn.is_open())
                why = "cannot be read";
            else if (!ps2x::lut::parseCube(text, lutTable, why))
                why = "is not a 3D .cube LUT (" + why + ")";
        }
        if (!why.empty())
        {
            lutTable = ps2x::lut::Table{};
            std::cerr << "[lut] " << lutPath.string() << " " << why << ": no LUT" << std::endl;
        }
    }
    const bool lutWanted = lutTable.ok() && lutChoice.strength > 0;
#else
    constexpr bool lutWanted = false;
#endif
    bool scanActive = (scanStock || scanSettings.on || scanSettings.pictureKeys || lutWanted) && scanPass.init(); // the picture goes through the pass
#if !defined(GT4_STOCK_BUILD)
    if (lutWanted)
    {
        const bool lutReady = scanActive && scanPass.setLut(&lutTable, static_cast<float>(lutChoice.strength) / 100.0f);
#if defined(PS2X_GS_VK)
        if (lutReady && vkPresents)
            gsvk::present::setLut(scanPass.lutStrip().data(), scanPass.lutPoints());
#endif
        std::cerr << "[lut] " << lutMarker << ": " << (lutChoice.kind == ps2x::lut::Kind::Gt3 ? std::string("GT3 look (built in)") : "file " + lutChoice.path) << ", "
                  << lutTable.size << " points per channel, strength " << lutChoice.strength << "%"
                  << (lutReady ? "" : ": the picture pass could not take it, no LUT") << std::endl;
    }
    else if (lutChoice.kind == ps2x::lut::Kind::Off || lutChoice.strength == 0)
        std::cerr << "[lut] " << lutMarker << ": off" << std::endl;
#endif
    bool scanOn = scanActive && scanSettings.on;                                                      // with lines (F1)
    crtOn = false;                                                                                    // GT3's TV filter: GT3 builds only
    std::cerr << "[scanline] " << (scanActive ? (scanOn ? "lines on" : "lines off (F1)") : "not used (GT3_DISPLAY_FILTER=crt or F1)")
              << ", strength " << std::lround(scanSettings.strength * 100.0f) << "%, field emphasis " << std::lround(scanSettings.field * 100.0f)
              << "%, picture fit " << ps2x::scanline::fitName(scanSettings.fit) << ", picture filter " << ps2x::scanline::filterName(scanSettings.filter)
              << ", deinterlace " << ps2x::scanline::deinterlaceName(scanSettings.deinterlace)
#if defined(PS2X_GS_GL)
              << " (presenter mode " << gsgl::deinterlaceMode() << ")"
#endif
              << (scanStock ? " (stock build)" : "") << std::endl;
    // GT4_PICTURE_FILTER's earlier names (Test Build 3's Scaling list): smooth is bilinear by its new name; sharp
    // is no longer offered (the developer's four: nearest, bilinear, bicubic, lanczos) and reads as bilinear.
    if (scanSettings.oldFilterName)
        std::cerr << "[scanline] GT4_PICTURE_FILTER=" << scanSettings.oldFilterName
                  << (std::string(scanSettings.oldFilterName) == "sharp" ? " is no longer offered" : " is bilinear's earlier name")
                  << ": bilinear (nearest, bilinear, bicubic or lanczos)" << std::endl;
#endif
#if defined(PS2X_GAME_GT4)
    // GT4's race colour (GT4_RACE_COLOUR, gs_fullcolour.h): read once, here (after gt4-start.env), and
    // logged ([gs-colour]); the GS frontend asks for it at every draw.
    (void)gsfc::mode();
#endif
    UnloadImage(blank);

    // Wider pictures through GT3's own 16:9 mode (GT3_ASPECT; gt3_aspect.h):
    // the hooks go in before the game runs.
#if defined(PS2X_GAME_GT3)
    gt3aspect::install(*this, m_memory.getRDRAM());
#endif
#if GT3_ADDONS
    gt3camera::install(*this, m_memory.getRDRAM()); // (after gt3aspect: wraps its wrapper)
    gt3autopilot::install(*this, m_memory.getRDRAM());
    // The lap recorder (F10 / GT3_DRIVE_LOG) sees when GT3 drives the player's car (gt3_drive_log.h).
    gt3drivelog::install(*this, m_memory.getRDRAM());
    // GT3's race state (gt3_race_info.h) for the phone's live timing page and
    // the radar (which F8 can turn on at any time): always wrapped.
    gt3race::install(*this, m_memory.getRDRAM());
    // GT3_RUNWAY_REPLACE=<folder> (GT4 track-import research): a course's runway
    // replaced as GT3's course loader starts on it (gt3_course_patch.h).
    gt3coursepatch::install(*this, m_memory.getRDRAM());
    // GT3_AI_TRACE=1: GT3's AI cars' controls logged (gt3_ai_pilot.h; after the autopilot's AI-driver wrapper).
    gt3aipilot::install(*this, m_memory.getRDRAM());
    // Grid-start lights (gt3_start_lights.h): GT3_START_HOLD=<frames> (step 1: the start held before START).
    gt3startlights::install(*this, m_memory.getRDRAM());
    // GT3_DT_EXPERIMENT=<k> (diagnostic, 2026-10-01: "more than 60 fps of real
    // game logic"): the 1/60 literals GT3's functions scale by (the .lit4
    // pool: 51 words at 0x34fb00-0x3509d0, one function each) divided by k,
    // for runs with GT3_TURBO=k (k times the VSyncs). Shows which parts
    // of the game follow them (and which count frames instead).
    if (const char *v = std::getenv("GT3_DT_EXPERIMENT"); v && *v)
    {
        const float k = std::max(1.0f, static_cast<float>(std::atof(v)));
        uint8_t *rdram = m_memory.getRDRAM();
        int patched = 0;
        for (uint32_t a = 0x34fb00u; a < 0x350a00u; a += 4u)
        {
            uint32_t w;
            std::memcpy(&w, rdram + a, 4);
            if (w != 0x3c888889u)
                continue;
            const float dt = 1.0f / (60.0f * k);
            std::memcpy(rdram + a, &dt, 4);
            ++patched;
        }
        std::fprintf(stderr, "[dt-experiment] %d literals 1/60 -> 1/%g\n", patched, 60.0 * k);
    }
#endif

    std::atomic<bool> gameThreadFinished{false};

    // Controller mapping: PS2X_INPUT (the profile's rows separated by ';', as the
    // GT4 Stock Launcher writes them into gt4-start.env), else PS2X_INPUT_PROFILE,
    // else gt3-input.profile in the working folder when present, else the built-in defaults.
    {
        std::string error;
        const char *rows = std::getenv("PS2X_INPUT");
        const char *profilePath = std::getenv("PS2X_INPUT_PROFILE");
        if (!profilePath || !*profilePath)
            profilePath = std::ifstream("gt3-input.profile").good() ? "gt3-input.profile" : nullptr;
        if (rows && *rows)
        {
            if (!m_padBackend.loadProfileText(rows, error))
                std::cerr << "[input] PS2X_INPUT: " << error << " (rows count from 1, split at ';'); retaining previous/default profile\n";
            else
                std::cerr << "[input] controller mapping from PS2X_INPUT\n";
        }
        else if (profilePath && !m_padBackend.loadProfile(profilePath, error))
            std::cerr << "[input] " << error << "; retaining previous/default profile\n";
        else if (profilePath)
            std::cerr << "[input] controller mapping " << profilePath << "\n";
    }
#if defined(PS2X_GAME_GT4)
    // Player 2 (controller port 2): GT4_PAD2 / PS2X_INPUT2 (Kernel/Stubs/Gt4Pad2.h); logs its settings.
    gt4pad2::configure();
#if !defined(GT4_STOCK_BUILD)
    // (tests, HUD hand-off 9: the Race HUD zoom key and button never reach GT4's own controls; ps2_pad.cpp)
    if (const char *t = std::getenv("GT4_HUD_ZOOM_SELFTEST"); t && *t == '1')
        m_padBackend.zoomSelfTest();
#endif
#endif
    // --headless: host pads and keyboard are never read and no rumble is
    // sent; the game sees the neutral pad (or GT3_PAD_REPLAY). raylib does
    // not open or poll gamepads for a hidden window either (RaylibInitGuard.cmake).
    if (!m_headless)
        m_padBackend.poll();
    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        ThreadNaming::PreferPerformance();
        applyGuestFloatMode();
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    // Live settings: GT3_LIVE_SETTINGS names the launcher's settings file,
    // which the launcher rewrites when a graphics setting changes while the
    // game runs. Checked twice a second; the display filter, shape, FPS
    // overlay, interlacing and internal resolution apply at once (the
    // renderer switches resolution at its next frame). F4 cycles the
    // internal resolution (OpenGL renderer). A short note shows what changed.
#if GT3_ADDONS
    const std::string livePath = [] {
        const char *v = std::getenv("GT3_LIVE_SETTINGS");
        return std::string(v ? v : "");
    }();
#else
    const std::string livePath; // no launcher settings in the stock build
#endif
    std::error_code liveError;
    std::filesystem::file_time_type liveStamp{};
    if (!livePath.empty())
        liveStamp = std::filesystem::last_write_time(livePath, liveError);
    double liveNextCheck = 0.0;
    std::string osdText;
    double osdUntil = 0.0;
#if defined(PS2X_GS_GL)
    // The GPU renderers (OpenGL, or its source on Vulkan) take live changes.
    const bool glRenderer = [] {
        const char *v = std::getenv("GT3_RENDERER");
        return v && (std::strcmp(v, "gl") == 0 || std::strcmp(v, "vk") == 0);
    }();
#else
    const bool glRenderer = false;
#endif
    auto showNote = [&](const std::string &text) {
        osdText = text;
        osdUntil = GetTime() + 2.5;
    };
    auto scaleName = [](int scale, bool progressive) {
        std::string s = scale <= 1 ? "Native" : std::to_string(scale) + "x";
        return progressive ? s + ", progressive" : s;
    };
#if GT3_ADDONS
    auto applyLiveFile = [&]() {
        std::ifstream in(livePath);
        std::map<std::string, int> values;
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;
            values[line.substr(0, eq)] = std::atoi(line.c_str() + eq + 1);
        }
        std::vector<std::string> changed;
        if (values.count("display_filter"))
        {
            static const DisplayFilter filters[] = {DisplayFilter::Sharp, DisplayFilter::Bilinear, DisplayFilter::Nearest, DisplayFilter::Fsr1,
                                                    DisplayFilter::Nis, DisplayFilter::Crt};
            static const char *names[] = {"sharp", "bilinear", "nearest", "FSR 1", "NIS", "TV (CRT)"};
            const int f = std::clamp(values["display_filter"], 0, 5);
            if (filters[f] != displayFilter)
            {
                displayFilter = filters[f];
                crtOn = displayFilter == DisplayFilter::Crt;
                applyDisplayFilter();
                changed.push_back(std::string("Display: ") + names[f]);
            }
        }
        if (values.count("upscale_sharpness"))
        {
            const float want = std::clamp(values["upscale_sharpness"], 0, 100) / 100.0f;
            if (std::fabs(want - upscaleSharpness) > 0.001f)
            {
                upscaleSharpness = want;
                changed.push_back("Sharpening: " + std::to_string(values["upscale_sharpness"]) + "%");
            }
        }
        if (values.count("display_aspect") && (values["display_aspect"] == 1) != squarePixels)
        {
            squarePixels = values["display_aspect"] == 1;
            changed.push_back(squarePixels ? "Shape: square pixels" : "Shape: 4:3");
        }
        if (values.count("fps_overlay") && (values["fps_overlay"] != 0) != overlay)
            overlay = values["fps_overlay"] != 0;
        {
            static const char *keys[] = {"units_speed", "units_power", "units_torque"};
            static const int limits[] = {1, 4, 2};
            static const char *names[3][5] = {{"km/h", "mph"}, {"PS", "HP", "BHP", "CV", "kW"}, {"kgf.m", "ft.lb", "Nm"}};
            for (int i = 0; i < 3; ++i)
            {
                if (!values.count(keys[i]))
                    continue;
                const int u = std::clamp(values[keys[i]], 0, limits[i]);
                if (u != hudUnits[i])
                {
                    hudUnits[i] = u;
                    changed.push_back(std::string("Units: ") + names[i][u]);
                }
            }
        }
#if defined(PS2X_GS_GL)
        if (glRenderer && values.count("deinterlace"))
        {
            const int d = std::clamp(values["deinterlace"], 0, 4);
            const bool progressive = d == 4;
            const int mode = progressive ? 0 : d;
            if (mode != gsgl::deinterlaceMode())
            {
                gsgl::requestDeinterlace(mode);
                static const char *names[] = {"adaptive", "weave", "bob", "double"};
                changed.push_back(std::string("Interlacing: ") + names[mode]);
            }
            int scale = gsgl::renderScale();
            if (values.count("internal_resolution"))
            {
                // The launcher's choices: native, 2x, 3x, 4x, 6x, 8x.
                static const int kScales[] = {1, 2, 3, 4, 6, 8};
                scale = kScales[std::clamp(values["internal_resolution"], 0, 5)];
            }
            if (scale != gsgl::renderScale() || progressive != gsgl::progressiveScan())
            {
                gsgl::requestRenderScale(scale, progressive);
                changed.push_back("Internal resolution: " + scaleName(scale, progressive));
            }
        }
#endif
        if (const std::string radar = gt3radar::applyLive(values); !radar.empty())
            changed.push_back(radar);
        if (const std::string hud = gt3hud::applyLive(values); !hud.empty())
            changed.push_back(hud);
        // Controller vibration on / off (the launcher's Controls page).
        if (values.count("vibration") && (values["vibration"] != 0) != ps2_pad::vibrationEnabled())
        {
            ps2_pad::setVibrationEnabled(values["vibration"] != 0);
            changed.push_back(values["vibration"] != 0 ? "Vibration on" : "Vibration off");
        }
        // GT3's HUD groups hidden (the launcher's HUD page).
        if (values.count("hud_hide"))
        {
            static int hudHide = [] {
                const char *v = std::getenv("GT3_HUD_HIDE");
                return v && *v ? std::atoi(v) & 15 : 0;
            }();
            const int want = values["hud_hide"] & 15;
            if (want != hudHide)
            {
                hudHide = want;
                userHudHide = static_cast<uint32_t>(want);
                changed.push_back(want ? "GT3's HUD: some parts hidden" : "GT3's HUD: all shown");
            }
        }
        // Race cameras (the launcher's Camera page).
        {
            const std::string camera = gt3camera::applyLive(values.count("camera_views") ? values["camera_views"] : -1,
                                                            values.count("camera_start") ? values["camera_start"] : -2,
                                                            values.count("camera_look") ? values["camera_look"] : -1);
            if (!camera.empty())
                changed.push_back(camera);
            if (values.count("car_detail"))
                if (const std::string detail = gt3camera::applyCarDetail(values["car_detail"]); !detail.empty())
                    changed.push_back(detail);
            if (values.count("rim_detail"))
                if (const std::string detail = gt3camera::applyRimDetail(values["rim_detail"]); !detail.empty())
                    changed.push_back(detail);
            if (values.count("wheel_blur"))
                if (const std::string blur = gt3camera::applyWheelBlur(values["wheel_blur"]); !blur.empty())
                    changed.push_back(blur);
        }
        // The phone dashboard's layout (its page picks the change up itself).
        {
            auto value = [&](const char *key) { return values.count(key) ? values[key] : -1; };
            phoneview::applyLive(value("phone_order"), value("phone_sizes"), value("phone_arrange"), value("phone_radar_range"),
                                 value("phone_map_fallback"));
        }
        if (!changed.empty())
        {
            std::string text;
            for (const std::string &c : changed)
                text += (text.empty() ? "" : "   |   ") + c;
            showNote(text);
            std::cerr << "[live] " << text << std::endl;
        }
    };
    // The radar's settings after an in-game change (F8, Shift+F8 edit) into
    // the launcher's settings file, so the launcher and the next start keep
    // them (its other lines unchanged).
    auto saveRadarSettings = [&]() {
        if (livePath.empty())
            return;
        std::vector<std::string> lines;
        {
            std::ifstream in(livePath);
            std::string line;
            while (std::getline(in, line))
            {
                const size_t eq = line.find('=');
                const std::string key = eq == std::string::npos ? std::string{} : line.substr(0, eq);
                if (key == "radar" || key.rfind("radar_", 0) == 0 || line.rfind("# proximity radar", 0) == 0)
                    continue;
                lines.push_back(line);
            }
        }
        std::ofstream out(livePath, std::ios::trunc);
        for (const std::string &line : lines)
            out << line << "\n";
        out << "# proximity radar: 0 off, 1 when cars are near, 2 always; position percent; colours 0xRRGGBB as decimal\n";
        for (const auto &[key, value] : gt3radar::liveValues())
            out << key << "=" << value << "\n";
        out.close();
        liveStamp = std::filesystem::last_write_time(livePath, liveError);
    };
#else
    auto applyLiveFile = [] {};
#endif

    uint64_t tick = 0;
    uint64_t presents = 0;
    double headlessNextCheck = 0.0;
#if !defined(PS2X_GAME_GT3)
    GridPacer gridPacer;
    // The host loop (presentation, input) at the emulation threads' priority (GT4_THREAD_PRIORITY): with only
    // those raised, a busy desktop delayed the presents instead (48-57 a second at Trial Mountain).
    ThreadNaming::PreferPerformance();
    if (gridPacer.enabled)
    {
        SetTargetFPS(0); // the grid paces the loop
        std::cerr << "[present] on the VBlank grid, " << std::chrono::duration<double, std::milli>(gridPacer.delay).count()
                  << " ms behind (PS2X_PRESENT_GRID=0: raylib's 60 fps limiter)" << std::endl;
    }
#endif
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
#if !defined(PS2X_GAME_GT3)
        if (gridPacer.enabled)
        {
            gridPacer.waitForFrame(streamPresentation());
            g_presentMaxTick = gridPacer.want != 0 ? gridPacer.want : ~0ull;
        }
#endif
        if (!m_headless)
            m_padBackend.poll();
        else if (GetTime() >= headlessNextCheck)
        {
            headlessNextCheck = GetTime() + 0.25;
            stopIfHeadlessWindowShown("running");
        }
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
        if (m_headless)
            updateHeadlessSwap(m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u);
        else
            updateDisplayRequired(!IsWindowMinimized());
#endif
        applyHudUnits();
        applyPedals();
#if defined(PS2X_GAME_GT3)
        gt3aspect::perFrame(m_memory.getRDRAM());
#endif
#if GT3_ADDONS
        // GT3's HUD groups the user hid (GT3_HUD_HIDE) apply in races only;
        // the mirror stays while the phone shows it (its picture is cut out
        // of this one: hidden, the phone got the road ahead).
        // (Also before the start: GT3's race HUD runs from the grid on - the
        // camera round the car, the countdown - while the race camera only
        // runs once the player drives; the user: "the original HUD stays on
        // until you get control of your car".)
        {
            gt3race::Snapshot hudRace;
            const bool raceHud = gt3race::read(m_memory.getRDRAM(), hudRace, false) && !hudRace.replay;
            GS::setHudRace(gt3camera::raceActive() || raceHud);
        }
        {
            const uint32_t hide = phoneView && phoneview::mirrorPanel() ? (userHudHide & ~static_cast<uint32_t>(GS::HudHideMirror)) : userHudHide;
            if (hide != appliedHudHide)
            {
                appliedHudHide = hide;
                GS::setHudHide(hide);
            }
        }
#endif
        if (!livePath.empty() && GetTime() >= liveNextCheck)
        {
            liveNextCheck = GetTime() + 0.5;
            const auto stamp = std::filesystem::last_write_time(livePath, liveError);
            if (!liveError && stamp != liveStamp)
            {
                liveStamp = stamp;
                applyLiveFile();
            }
        }
#if defined(PS2X_GS_GL) && GT3_ADDONS
        if (glRenderer && IsKeyPressed(KEY_F4))
        {
            // Native to 4x and round (from 6x / 8x back to native).
            const int scale = gsgl::renderScale() >= 4 ? 1 : gsgl::renderScale() + 1;
            gsgl::requestRenderScale(scale, gsgl::progressiveScan());
            showNote("Internal resolution: " + scaleName(scale, gsgl::progressiveScan()) + "   (F4)");
        }
#endif
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
#if defined(PS2X_UPSCALERS)
        if (frameGenRate)
        {
            const uint64_t uploadsBefore = g_frameUploads;
            UploadFrame(fgSpare, this, presentWidth, presentHeight);
            if (g_frameUploads != uploadsBefore)
            {
                std::swap(frameTex, fgSpare); // fgSpare: the previous game frame
                fgPrevArrival = fgCurArrival;
                fgCurArrival = GetTime();
                // Its motion: the frame drawn during the vsync before (double
                // buffering), published by the tracker a moment later.
                fgMotionTick = m_memory.gs().vsyncTick.load() - 1u;
                fgMotionOk = false;
                fgUsable = false;
                // GT3_FRAME_GEN_LOG=<vsync>: from then, which game frame's motion
                // is at hand when a picture arrives (120 lines).
                static const uint64_t fgLogFrom = [] {
                    const char *v = std::getenv("GT3_FRAME_GEN_LOG");
                    return v && *v ? std::strtoull(v, nullptr, 10) : ~0ull;
                }();
                static int fgLogLeft = 120;
                if (m_memory.gs().vsyncTick.load() >= fgLogFrom && (g_frameUploads % 600u) == 0u)
                    std::cerr << "[frame-gen] generated " << fgGenerated << ", motion late " << fgMotionLate << ", cuts / no 3D " << fgCuts << std::endl;
                if (m_memory.gs().vsyncTick.load() >= fgLogFrom && fgLogLeft-- > 0)
                {
                    uint64_t publishedTick = 0;
                    std::vector<gt3motion::Triangle> none;
                    const uint64_t serial = gt3motion::Tracker::instance().latestTriangles(none, ~0ull - 1, &publishedTick);
                    std::cerr << "[frame-gen] picture at vsync " << m_memory.gs().vsyncTick.load() << ", motion of vsync " << publishedTick
                              << " (serial " << serial << ")" << std::endl;
                }
            }
        }
        else
#endif
            UploadFrame(frameTex, this, presentWidth, presentHeight);
#if !defined(PS2X_GAME_GT3)
            {
                // [present] picture: the shown video mode, size and shape, once per change.
                static uint64_t lastPicture = ~0ull;
                const float shape = Gt4PictureAspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load());
                const bool wide = shape > 0.0f;
                const uint64_t picture = (static_cast<uint64_t>(g_shownCrt) << 32) ^ (static_cast<uint64_t>(presentWidth) << 16) ^
                                         static_cast<uint64_t>(presentHeight) ^ (static_cast<uint64_t>(static_cast<uint32_t>(g_shownLines)) << 48) ^
                                         (wide ? (1ull << 63) : 0ull) ^ (static_cast<uint64_t>(shape * 1000.0f) << 36);
                static int pictureLogs = 0;
                if (picture != lastPicture && pictureLogs < 400)
                {
                    lastPicture = picture;
                    ++pictureLogs;
                    std::fprintf(stderr, "[present] picture %s %ux%u, %d display lines, shape %.3f:1, scanlines %s at vsync %llu\n",
                                 ps2x::crt::name(g_shownCrt), presentWidth, presentHeight, g_shownLines,
                                 ps2x::crt::is1080i(g_shownCrt) ? static_cast<double>(Gt4Picture1080Aspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load(), 1.8f))
                                 : wide ? static_cast<double>(shape) : 4.0 / 3.0,
                                 ps2x::crt::is1080i(g_shownCrt) ? "off (1080i)" : "as set",
                                 static_cast<unsigned long long>(m_memory.gs().vsyncTick.load()));
                }
            }
#endif
        {
            // GT3_RAM_DUMP=<path prefix> + GT3_RAM_DUMP_TICKS=<tick>[,<tick>...]:
            // EE RAM (32 MB) written to <prefix>-<tick>.bin at those VSyncs,
            // for offline searches (race state for live timing). Taken while
            // the game runs, so a structure may be caught mid-update.
            static const std::string ramDumpPrefix = [] {
                const char *v = std::getenv("GT3_RAM_DUMP");
                return std::string(v ? v : "");
            }();
            static std::vector<uint64_t> ramDumpTicks = [] {
                std::vector<uint64_t> out;
                if (const char *v = std::getenv("GT3_RAM_DUMP_TICKS"))
                    for (const char *p = v; *p;)
                    {
                        char *end = nullptr;
                        const unsigned long long t = std::strtoull(p, &end, 10);
                        if (end == p)
                            break;
                        out.push_back(t);
                        p = *end == ',' ? end + 1 : end;
                    }
                std::sort(out.begin(), out.end(), std::greater<uint64_t>());
                return out;
            }();
            const uint64_t tickNow = m_memory.gs().vsyncTick.load();
            if (!ramDumpPrefix.empty() && !ramDumpTicks.empty() && tickNow >= ramDumpTicks.back())
            {
                ramDumpTicks.pop_back();
                const std::string file = ramDumpPrefix + "-" + std::to_string(tickNow) + ".bin";
                if (FILE *f = std::fopen(file.c_str(), "wb"))
                {
                    std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                    std::cerr << "[ram-dump] " << file << std::endl;
                }
            }
        }
#if GT3_ADDONS
        if (phoneView)
        {
            // The mirror's rectangle (as drawn this race frame) cropped out of
            // the newest game frame, at most GT3_PHONE_VIEW_FPS times a second.
            phoneCapture.poll();
            const double now = GetTime();
            if (phoneview::wanted() && g_frameUploads != phoneUploads && now - phoneLast >= 1.0 / phoneview::fps() - 0.004)
            {
                phoneUploads = g_frameUploads;
                uint16_t r[4];
                uint64_t drawnAt = 0;
                if (GS::insetView(r, drawnAt) && m_memory.gs().vsyncTick.load() - drawnAt < 10u)
                {
                    phoneLast = now;
                    const float fx = static_cast<float>(presentWidth) / FB_WIDTH, fy = static_cast<float>(presentHeight) / DEFAULT_DISPLAY_HEIGHT;
                    const int x0 = static_cast<int>(r[0] * fx), y0 = static_cast<int>(r[1] * fy);
                    const int x1 = std::min(static_cast<int>((r[2] + 1) * fx), frameTex.width);
                    const int y1 = std::min(static_cast<int>((r[3] + 1) * fy), frameTex.height);
                    phoneCapture.read(frameTex.id, x0, y0, x1 - x0, y1 - y0);
                }
            }
        }
#endif
#if defined(PS2X_UPSCALERS)
        if (IsKeyPressed(KEY_F7))
        {
            motionView = !motionView;
            showNote(motionView ? "Motion buffer shown (F7)" : "Motion buffer off (F7)");
        }
        if (motionView || frameGenRate)
        {
            gt3motion::Tracker::instance().setEnabled(true);
            if (!motionBufferTried)
            {
                motionBufferTried = true;
                std::string error;
                motionBufferReady = motionBuffer.init(error);
                if (!motionBufferReady)
                    std::cerr << "[motion] motion buffer unavailable: " << error << std::endl;
            }
            if (motionBufferReady && frameGenRate)
            {
                gt3motion::Tracker::instance().publishBefore(m_memory.gs().vsyncTick.load());
                // The shown picture's own motion, once published.
                gt3motion::FrameStats fs;
                if (!fgMotionOk && gt3motion::Tracker::instance().trianglesOfTick(fgMotionTick, motionTriangles, &fs))
                {
                    motionBuffer.build(motionTriangles, static_cast<int>(presentWidth), static_cast<int>(presentHeight));
                    fgMotionOk = true;
                    fgOverlayDue = true;
                    // No pictures between two game frames that do not continue
                    // each other: no 3D (menus, movies), or a camera cut (most
                    // batches new, or a mean move over 24 frame pixels).
                    const double meanMotion = fs.vertices ? fs.motionSum / fs.vertices : 0.0;
                    fgUsable = fs.mainBatches > 0u && fs.matched * 5u >= fs.mainBatches * 4u && meanMotion <= 24.0;
                    if (!fgUsable)
                        ++fgCuts;
                }
            }
            else if (motionBufferReady)
            {
                const uint64_t serial = gt3motion::Tracker::instance().latestTriangles(motionTriangles, motionSerial);
                if (serial != motionSerial)
                {
                    motionSerial = serial;
                    motionBuffer.build(motionTriangles, static_cast<int>(presentWidth), static_cast<int>(presentHeight));
                }
            }
        }
#endif
        // The picture to show: the game frame, or with frame generation the
        // picture at this moment between the last two game frames.
        const Texture2D *shownTex = &frameTex;
#if defined(PS2X_UPSCALERS)
        if (frameGenRate && motionBufferReady && fgPrevArrival >= 0.0 && fgSpare.width == frameTex.width && fgSpare.height == frameTex.height)
        {
            if (!frameGenTried)
            {
                frameGenTried = true;
                std::string error;
                frameGenReady = frameGen.init(error);
                if (!frameGenReady)
                    std::cerr << "[frame-gen] unavailable: " << error << std::endl;
            }
            if (frameGenReady && fgOverlayDue)
            {
                fgOverlayDue = false;
                frameGen.updateOverlay(fgSpare, frameTex, motionBuffer.texture(), static_cast<int>(presentWidth), static_cast<int>(presentHeight));
            }
            const double interval = std::max(1.0e-3, fgCurArrival - fgPrevArrival);
            const double phase = std::clamp((GetTime() - fgCurArrival) / interval, 0.0, 1.0);
            if (frameGenReady && phase < 0.02)
                shownTex = &fgSpare; // the older game frame itself
            else if (frameGenReady && phase < 0.995 && (!fgMotionOk || !fgUsable))
            {
                // The motion not there yet, or no continuity: the nearer game frame.
                if (phase < 0.5)
                    shownTex = &fgSpare;
                if (!fgMotionOk)
                    ++fgMotionLate;
            }
            else if (frameGenReady && phase < 0.995)
            {
                ++fgGenerated;
                shownTex = &frameGen.generate(fgSpare, frameTex, motionBuffer.texture(), static_cast<int>(presentWidth),
                                              static_cast<int>(presentHeight), static_cast<float>(phase));
                // GT3_FRAME_GEN_SHOT=<prefix> (+ GT3_FRAME_GEN_SHOT_FROM=<vsync>):
                // 6 sets of older / generated / newer frame, phase 0.3-0.7.
                static const char *shotPrefix = std::getenv("GT3_FRAME_GEN_SHOT");
                static const uint64_t shotFrom = [] {
                    const char *v = std::getenv("GT3_FRAME_GEN_SHOT_FROM");
                    return v && *v ? std::strtoull(v, nullptr, 10) : 0ull;
                }();
                static int shotsLeft = 6;
                const uint64_t tick = m_memory.gs().vsyncTick.load();
                if (shotPrefix && *shotPrefix && shotsLeft > 0 && tick >= shotFrom && phase > 0.3 && phase < 0.7)
                {
                    --shotsLeft;
                    const Texture2D *parts[3] = {&fgSpare, shownTex, &frameTex};
                    const char *names[3] = {"a-older", "b-generated", "c-newer"};
                    for (int i = 0; i < 3; ++i)
                    {
                        Image img = LoadImageFromTexture(*parts[i]);
                        ImageCrop(&img, Rectangle{0.0f, 0.0f, static_cast<float>(presentWidth), static_cast<float>(presentHeight)});
                        char path[1024];
                        std::snprintf(path, sizeof(path), "%s-%06llu-%s.png", shotPrefix, static_cast<unsigned long long>(tick), names[i]);
                        ExportImage(img, path);
                        UnloadImage(img);
                    }
                    std::cerr << "[frame-gen] saved tick " << tick << " phase " << phase << std::endl;
                }
            }
        }
#endif

#if GT3_ADDONS
        // The launcher's HUD layout editor shows a race picture from the
        // user's own game: 8 s into each race, the game frame (no overlays;
        // GT3's HUD as shown) into hud-background.png next to the launcher's
        // settings file, and a copy kept in hud-pictures (the last 16).
        {
            static bool saved = false;
            static double racingSince = -1.0;
            static double notRacingSince = -1.0;
            if (!livePath.empty())
            {
                // A race over (10 s without the race camera): the next one saves again.
                if (gt3camera::raceActive())
                    notRacingSince = -1.0;
                else if (notRacingSince < 0.0)
                    notRacingSince = GetTime();
                else if (saved && GetTime() - notRacingSince > 10.0)
                    saved = false;
            }
            if (!saved && !livePath.empty())
            {
                if (!gt3camera::raceActive())
                    racingSince = -1.0;
                else if (racingSince < 0.0)
                    racingSince = GetTime();
                else if (GetTime() - racingSince > 8.0 && presentWidth > 0u && presentHeight > 0u)
                {
                    saved = true;
                    Image img = LoadImageFromTexture(frameTex);
                    ImageCrop(&img, Rectangle{0.0f, 0.0f, static_cast<float>(presentWidth), static_cast<float>(presentHeight)});
                    // Stored at 4:3 (or the race's wider shape) and at most 1280 wide.
                    const float shape = gt3aspect::displayAspect(m_memory.getRDRAM()) > 0.0f
                                            ? gt3aspect::displayAspect(m_memory.getRDRAM())
                                            : 4.0f / 3.0f;
                    const int outW = std::min(1280, static_cast<int>(presentWidth));
                    ImageResize(&img, outW, std::max(1, static_cast<int>(outW / shape)));
                    const std::string path =
                        std::filesystem::path(livePath).parent_path().append("hud-background.png").string();
                    ExportImage(img, path.c_str());
                    {
                        std::error_code ec;
                        const std::filesystem::path folder = std::filesystem::path(livePath).parent_path() / "hud-pictures";
                        std::filesystem::create_directories(folder, ec);
                        const std::time_t now = std::time(nullptr);
                        std::tm local{};
                        localtime_s(&local, &now);
                        char name[64];
                        std::strftime(name, sizeof(name), "race-%Y%m%d-%H%M%S.png", &local);
                        ExportImage(img, (folder / name).string().c_str());
                        // The oldest beyond 16 go (names sort by time).
                        std::vector<std::filesystem::path> kept;
                        for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
                            if (entry.path().extension() == ".png" && entry.path().filename().string().rfind("race-", 0) == 0)
                                kept.push_back(entry.path());
                        std::sort(kept.begin(), kept.end());
                        for (size_t i = 0; i + 16 < kept.size(); ++i)
                            std::filesystem::remove(kept[i], ec);
                    }
                    UnloadImage(img);
                    std::cerr << "[hud] race picture for the launcher's HUD layout editor: " << path << std::endl;
                }
            }
        }
#endif
#if defined(PS2X_SCANLINE_PASS) && !defined(GT4_STOCK_BUILD)
        // PS2X_SCANLINE_SHOT=<prefix> (diagnostics, hidden window too): every
        // PS2X_SCANLINE_SHOT_EVERY VSyncs (default 600) the frame as
        // <prefix>-<vsync>-src.png and the scanline pass drawn offscreen as
        // <prefix>-<vsync>-<W>x<H>-s<strength>-f<field>-p<parity>.png for each
        // window size in PS2X_SCANLINE_SHOT_SIZES (default 1280x960,1920x1440)
        // and strength in PS2X_SCANLINE_SHOT_STRENGTHS (default 0,50,100):
        // analysis/extras/stock-scanline/compare_shot.py draws the same frame
        // with the Python model and reports the difference.
        // (Picture filter and deinterlacing, analysis/tb2-fixes/picture.) The
        // name ends -<filter>-b<0|1> (the line blend) for each filter in
        // PS2X_SCANLINE_SHOT_FILTERS (nearest, bilinear, bicubic, lanczos, box;
        // default the running one) and each blend in PS2X_SCANLINE_SHOT_BLEND (0 / 1, only
        // applied to an interlaced picture; default the running choice).
        // PS2X_SCANLINE_SHOT_TICKS=<v1,v2,...>: instead of every N, the first
        // frame at or after each of these VSyncs.
        {
            static const char *scanShot = [] {
                const char *v = std::getenv("PS2X_SCANLINE_SHOT");
                return (v && *v) ? v : nullptr;
            }();
            static const uint64_t scanShotEvery = [] {
                const char *v = std::getenv("PS2X_SCANLINE_SHOT_EVERY");
                return static_cast<uint64_t>(std::max(1L, (v && *v) ? std::strtol(v, nullptr, 10) : 600L));
            }();
            static uint64_t scanShotLast = 0;
            static std::vector<uint64_t> scanShotTicks = [] {
                std::vector<uint64_t> ticks;
                const char *v = std::getenv("PS2X_SCANLINE_SHOT_TICKS");
                std::stringstream in((v && *v) ? v : "");
                for (std::string item; std::getline(in, item, ',');)
                    if (!item.empty())
                        ticks.push_back(std::strtoull(item.c_str(), nullptr, 10));
                std::sort(ticks.begin(), ticks.end(), std::greater<uint64_t>()); // the next one last
                return ticks;
            }();
            static const bool scanShotByTicks = !scanShotTicks.empty();
            const uint64_t tick = m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u;
            bool scanShotDue = false;
            if (scanShotByTicks)
            {
                while (!scanShotTicks.empty() && tick >= scanShotTicks.back())
                {
                    scanShotTicks.pop_back();
                    scanShotDue = true;
                }
            }
            else
                scanShotDue = tick >= scanShotLast + scanShotEvery;
            if (scanShot && scanShotDue && scanPass.init())
            {
                scanShotLast = tick;
                const auto list = [](const char *env, const char *fallback) {
                    const char *v = std::getenv(env);
                    std::vector<std::string> items;
                    std::stringstream in((v && *v) ? v : fallback);
                    for (std::string item; std::getline(in, item, ',');)
                        if (!item.empty())
                            items.push_back(item);
                    return items;
                };
                const int w = static_cast<int>(presentWidth), h = static_cast<int>(presentHeight);
                char path[1024];
                Image src = LoadImageFromTexture(*shownTex);
                ImageCrop(&src, Rectangle{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
                std::snprintf(path, sizeof(path), "%s-%06llu-src.png", scanShot, static_cast<unsigned long long>(tick));
                ExportImage(src, path);
                UnloadImage(src);
                const bool shot1080 = ps2x::crt::is1080i(g_shownCrt);
                const float shotAspect = shot1080 ? Gt4Picture1080Aspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load(), ps2x::crt::pictureAspect(g_shownCrt))
                                         : Gt4PictureAspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load()) > 0.0f ? Gt4PictureAspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load())
                                         : (squarePixels ? static_cast<float>(w) / static_cast<float>(std::max(1, h)) : 4.0f / 3.0f);
                const int shotLines = ps2x::scanline::displayLines(h, g_shownLines);
                const std::string runningFilter = ps2x::scanline::filterName(scanSettings.filter);
                const std::string runningBlend = scanSettings.deinterlace == ps2x::scanline::Deinterlace::Blend ? "1" : "0";
                for (const std::string &size : list("PS2X_SCANLINE_SHOT_SIZES", "1280x960,1920x1440"))
                {
                    int sw = 0, sh = 0;
                    if (std::sscanf(size.c_str(), "%dx%d", &sw, &sh) != 2 || sw < 64 || sh < 64 || sw > 8192 || sh > 8192)
                        continue;
                    for (const std::string &strength : list("PS2X_SCANLINE_SHOT_STRENGTHS", "0,50,100"))
                        for (const std::string &filterText : list("PS2X_SCANLINE_SHOT_FILTERS", runningFilter.c_str()))
                            for (const std::string &blendText : list("PS2X_SCANLINE_SHOT_BLEND", runningBlend.c_str()))
                            {
                                ps2x::scanline::Filter shotFilter = scanSettings.filter;
                                if (!ps2x::scanline::parseFilter(filterText.c_str(), shotFilter))
                                    continue;
                                // As the window draws it (scanForMode: none in 1080i); the file name
                                // gives the strength, field emphasis, parity, filter and blend actually drawn.
                                const int pct = std::clamp(std::atoi(strength.c_str()), 0, 100);
                                const ScanForMode shotScan = scanForMode(g_shownCrt, static_cast<float>(pct) / 100.0f, scanSettings.field, g_shownFieldParity);
                                const ps2x::scanline::Look shotLook{shotFilter, blendText == "1" && g_shownInterlaced};
                                // With a colour LUT (GT4_LUT): each shot with it (-lutNNN, its strength) and without (-lut000).
                                const float lutKeep = scanPass.lutStrength();
                                for (int lutPass = 0; lutPass < (scanPass.lutOn() ? 2 : 1); ++lutPass)
                                {
                                    char lutTag[16] = "";
                                    if (lutKeep > 0.0f)
                                        std::snprintf(lutTag, sizeof(lutTag), "-lut%03d", lutPass == 0 ? static_cast<int>(std::lround(lutKeep * 100.0f)) : 0);
                                    scanPass.setLutStrength(lutPass == 0 ? lutKeep : 0.0f);
                                    std::snprintf(path, sizeof(path), "%s-%06llu-%dx%d-s%03d-f%03d-p%d-%s-b%d%s.png", scanShot, static_cast<unsigned long long>(tick),
                                                  sw, sh, static_cast<int>(std::lround(shotScan.strength * 100.0f)),
                                                  static_cast<int>(std::lround(shotScan.field * 100.0f)), shotScan.parity, ps2x::scanline::filterName(shotFilter),
                                                  shotLook.blend ? 1 : 0, lutTag);
                                    scanPass.renderToFile(*shownTex, w, h, sw, sh, scanSettings.fit, shotAspect, shotScan.strength, shotScan.field,
                                                          shotScan.parity, path, shotLines, shotLook);
                                }
                                scanPass.setLutStrength(lutKeep);
                            }
                }
                std::cerr << std::dec << "[scanline] shots at vsync " << tick << ": " << scanShot << "-" << tick << "-* (" << ps2x::crt::name(g_shownCrt) << ", "
                          << w << "x" << h << ", " << shotLines << " lines, shape " << shotAspect << ", interlaced " << (g_shownInterlaced ? 1 : 0)
#if defined(PS2X_GS_GL)
                          << ", presenter field mode " << gsgl::copiedFrameField().mode
#endif
                          << ")" << std::endl;
            }
        }
#endif
        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        // Display shape: 4:3 (or the frame's own pixel shape), or the wider
        // shape GT3 draws for (its 16:9 option, GT3_ASPECT).
#if defined(PS2X_GAME_GT3)
        const float wideAspect = gt3aspect::displayAspect(m_memory.getRDRAM());
#else
        // GT4: the shown picture's video mode. 480i and 480p: 4:3, or 16:9
        // with GT4's own Aspect Ratio set to 16:9 (Gt4WideFlag); 1080i: GT4's
        // picture alone, 1728 of 1920 HD pixels across and 960 of 1080 lines,
        // 1.8:1 (developer, 2026-10-04: "GT4's picture only"; VIDEO-MODES.md 3.1).
        const float wideAspect = ps2x::crt::is1080i(g_shownCrt) ? Gt4Picture1080Aspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load(), ps2x::crt::pictureAspect(g_shownCrt))
                                 : Gt4PictureAspect(m_memory.getRDRAM(), m_memory.gs().vsyncTick.load());
#endif
        const float aspect = wideAspect > 0.0f ? wideAspect : (squarePixels ? srcWidth / srcHeight : 4.0f / 3.0f);
        float dstHeight = std::min(screenHeight, screenWidth / aspect);
        float dstWidth = dstHeight * aspect;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
#if defined(PS2X_SCANLINE_PASS)
        // The scanline pass places the picture itself: a whole number of
        // window rows per display line where a filled picture would band.
        ps2x::scanline::Placement scanPlace;
        // The picture's display lines (448 for 480i and 480p, 960 for 1080i);
        // what its mode draws on them: scanForMode (none in 1080i; VIDEO-MODES.md
        // 3.4). 480p: the same lines and strength as 480i.
        const int scanLines = ps2x::scanline::displayLines(static_cast<int>(srcHeight), g_shownLines);
        if (scanActive)
        {
            // With the lines off (F1) there is no line pattern to keep whole: auto then fills the screen (developer, 2026-10-09:
            // a 21:9 race on the 2560 x 1600 laptop was drawn at 2 rows per line, 2090 x 896, with bars on all four sides).
            const ps2x::scanline::Fit fit =
                scanSettings.fit == ps2x::scanline::Fit::Auto && !scanOn ? ps2x::scanline::Fit::Fill : scanSettings.fit;
            scanPlace = ps2x::scanline::fitPicture(screenWidth, screenHeight, scanLines, aspect, fit);
            dstRect = Rectangle{scanPlace.x, scanPlace.y, scanPlace.w, scanPlace.h};
            dstWidth = scanPlace.w;
            dstHeight = scanPlace.h;
        }
#endif
        // Triple screens with bezel correction (GT3_TRIPLE_BEZEL = the gap
        // between two screens' pictures in screen widths): in races the
        // picture is wider than the window by two gaps (GT3_ASPECT includes
        // them); each third of the window shows its screen's part, so what
        // lies behind the bezels is hidden and lines run straight across.
        struct ScreenPiece
        {
            float u0, u1;
            Rectangle dst;
        };
        std::vector<ScreenPiece> pieces;
        if ((tripleBezel > 0.0f || tripleScreens) && wideAspect > 2.5f)
        {
            const float units = 3.0f + 2.0f * tripleBezel; // the picture's width in screen widths
            const float screenW = screenWidth / 3.0f;
            const float pictureH = std::min(screenHeight, screenW * units / wideAspect);
            const float partW = pictureH * wideAspect / units; // one screen's part, window pixels
            const float y = (screenHeight - pictureH) * 0.5f;
            for (int i = 0; i < 3; ++i)
            {
                const float u0 = static_cast<float>(i) * (1.0f + tripleBezel) / units;
                pieces.push_back(ScreenPiece{u0, u0 + 1.0f / units,
                                             Rectangle{static_cast<float>(i) * screenW + (screenW - partW) * 0.5f, y, partW, pictureH}});
            }
            // The whole picture, placed so its middle part is on the middle screen
            // (the radar and the upscalers work on it).
            dstRect = Rectangle{pieces[1].dst.x - (1.0f + tripleBezel) * partW, y, units * partW, pictureH};
            dstWidth = dstRect.width;
            dstHeight = dstRect.height;
        }
        auto drawPicture = [&](const Texture2D &tex) {
            if (pieces.empty())
            {
                DrawTexturePro(tex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
                return;
            }
            for (const ScreenPiece &piece : pieces)
                DrawTexturePro(tex, Rectangle{piece.u0 * srcWidth, 0.0f, (piece.u1 - piece.u0) * srcWidth, srcHeight}, piece.dst,
                               Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        };
        bool drawn = false;
#if defined(PS2X_UPSCALERS)
        if (displayFilter == DisplayFilter::Fsr1 || displayFilter == DisplayFilter::Nis)
        {
            if (!upscalerTried)
            {
                upscalerTried = true;
                std::string error;
                if (!upscaler.init(error))
                {
                    std::cerr << "[display] FSR 1 / NIS unavailable, using bilinear: " << error << std::endl;
                    showNote("FSR 1 / NIS unavailable on this GPU: bilinear instead");
                }
                else
                    std::cerr << "[display] FSR 1 and NIS shaders ready" << std::endl;
            }
            std::vector<DisplayUpscaler::Piece> upscalerPieces;
            for (const ScreenPiece &piece : pieces)
                upscalerPieces.push_back(DisplayUpscaler::Piece{piece.u0, piece.u1, piece.dst});
            drawn = upscaler.draw(displayFilter == DisplayFilter::Fsr1 ? DisplayUpscaler::Kind::Fsr1 : DisplayUpscaler::Kind::Nis, *shownTex,
                                  static_cast<int>(srcWidth), static_cast<int>(srcHeight), dstRect, upscaleSharpness,
                                  upscalerPieces.empty() ? nullptr : &upscalerPieces);
        }
#endif
#if !defined(PS2X_GAME_GT3)
        // Esc leaves borderless full screen; Alt+Enter toggles it. (GT3_FULLSCREEN=span
        // windows are left as they are.)
        if (!m_headless)
        {
            const bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
            if ((IsKeyPressed(KEY_ESCAPE) || (alt && IsKeyPressed(KEY_ENTER))) && IsWindowFullscreen())
            {
                // GT3_FULLSCREEN=exclusive: Esc or Alt+Enter gives a window (Alt+Enter then goes to borderless full screen).
                ToggleFullscreen();
                showNote("Full screen off (Alt+Enter for borderless full screen)");
            }
            else if (IsKeyPressed(KEY_ESCAPE) && IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            {
                ToggleBorderlessWindowed();
                showNote("Full screen off (Esc; Alt+Enter to go back)");
            }
            else if (alt && IsKeyPressed(KEY_ENTER))
            {
                const bool wasFull = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
                ToggleBorderlessWindowed();
                showNote(wasFull ? "Full screen off (Alt+Enter)" : "Full screen (Esc or Alt+Enter to leave)");
            }
        }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // The 360 look's keypad and mouse (gt4cam, full-m2 "Cameras"; nothing unless GT4_CAM_LOOK=360).
        if (!m_headless)
            gt4cam::hostUpdate();
#endif
        if (!m_headless && IsKeyPressed(KEY_F1))
        {
#if defined(PS2X_SCANLINE_PASS)
            // (A development build without GT3_DISPLAY_FILTER=crt starts the
            // pass on the first F1; it places the picture from the next frame.)
            if (!scanActive)
                scanActive = scanPass.init();
            scanOn = scanActive && !scanOn;
            showNote(scanOn ? (ps2x::crt::is1080i(g_shownCrt) ? "Scanlines on (F1; none in 1080i)" : "Scanlines on (F1)") : "Scanlines off (F1)");
#else
            crtOn = !crtOn;
            showNote(crtOn ? "TV filter on (F1)" : "TV filter off (F1)");
#endif
        }
#if defined(PS2X_GS_VK)
        if (!drawn && vkPresents)
        {
            // The window presenter draws the picture from the renderer's image.
#if defined(PS2X_SCANLINE_PASS)
            if (scanActive && scanPlace.h > 0.0f && pieces.empty())
            {
                const ScanForMode liveScan = scanForMode(g_shownCrt, scanOn ? scanSettings.strength : 0.0f, scanSettings.field, g_shownFieldParity);
                const ps2x::scanline::Look look{scanSettings.filter, ps2x::scanline::blendLines(scanSettings.deinterlace, g_shownInterlaced)};
                const ps2x::scanline::Pass::Uniforms u =
                    scanPass.uniforms(g_vkFrame.texWidth, g_vkFrame.texHeight, static_cast<int>(srcWidth), static_cast<int>(srcHeight), scanPlace,
                                      liveScan.strength, liveScan.field, liveScan.parity, scanLines, look);
                gsvk::present::ScanUniforms vu;
                vu.texSize[0] = u.texSize[0];
                vu.texSize[1] = u.texSize[1];
                vu.rowsPerLine = u.rowsPerLine;
                vu.lines = u.lines;
                vu.cols = u.cols;
                vu.halfPixel = u.halfPixel;
                vu.mixBeam = u.mixBeam;
                vu.widthK = u.widthK;
                vu.field = u.field;
                vu.parity = u.parity;
                vu.filterMode = u.filterMode;
                vu.blendLines = u.blendLines;
                vu.lutSize = u.lutSize;
                vu.lutMix = u.lutMix;
                gsvk::present::drawScan(vu, srcWidth, srcHeight, scanPlace.x, scanPlace.y, scanPlace.w, scanPlace.h);
            }
            else
#endif
            {
                const bool linear = g_displayTextureFilter != TEXTURE_FILTER_POINT;
                if (pieces.empty())
                    gsvk::present::drawPlain(0.0f, 0.0f, srcWidth, srcHeight, dstRect.x, dstRect.y, dstRect.width, dstRect.height, linear);
                for (const ScreenPiece &piece : pieces)
                    gsvk::present::drawPlain(piece.u0 * srcWidth, 0.0f, piece.u1 * srcWidth, srcHeight, piece.dst.x, piece.dst.y, piece.dst.width,
                                             piece.dst.height, linear);
            }
            drawn = true;
        }
#endif
#if defined(PS2X_SCANLINE_PASS)
        if (!drawn && scanActive && scanPlace.h > 0.0f && pieces.empty())
        {
            const ScanForMode liveScan = scanForMode(g_shownCrt, scanOn ? scanSettings.strength : 0.0f, scanSettings.field, g_shownFieldParity);
            const ps2x::scanline::Look look{scanSettings.filter, ps2x::scanline::blendLines(scanSettings.deinterlace, g_shownInterlaced)};
            scanPass.draw(*shownTex, static_cast<int>(srcWidth), static_cast<int>(srcHeight), scanPlace, liveScan.strength, liveScan.field,
                          liveScan.parity, scanLines, look);
            drawn = true;
        }
#endif
        if (!drawn && crtOn && crtShader.id != 0)
        {
            // The picture in PS2 pixels: GT3's 640 x 448 frame at the internal
            // resolution's (whole-number) scale, per axis.
            const float lines[2] = {srcWidth / std::max(1.0f, std::round(srcWidth / 640.0f)),
                                    srcHeight / std::max(1.0f, std::round(srcHeight / 448.0f))};
            const float outSize[2] = {dstWidth, dstHeight};
            SetShaderValue(crtShader, crtLinesLoc, lines, SHADER_UNIFORM_VEC2);
            SetShaderValue(crtShader, crtOutLoc, outSize, SHADER_UNIFORM_VEC2);
            SetShaderValue(crtShader, crtScanLoc, &crtScanlines, SHADER_UNIFORM_FLOAT);
            SetShaderValue(crtShader, crtMaskLoc, &crtMask, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(crtShader);
            drawPicture(*shownTex);
            EndShaderMode();
            drawn = true;
        }
        if (!drawn && displayFilter == DisplayFilter::Sharp && sharpShader.id != 0)
        {
            const float texSize[2] = {static_cast<float>(shownTex->width), static_cast<float>(shownTex->height)};
            const float outScale[2] = {dstWidth / srcWidth, dstHeight / srcHeight};
            SetShaderValue(sharpShader, sharpTexSizeLoc, texSize, SHADER_UNIFORM_VEC2);
            SetShaderValue(sharpShader, sharpScaleLoc, outScale, SHADER_UNIFORM_VEC2);
            BeginShaderMode(sharpShader);
            drawPicture(*shownTex);
            EndShaderMode();
        }
        else if (!drawn)
            drawPicture(*shownTex);
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // PS2X_WINDOW_SHOT=<path prefix> with PS2X_WINDOW_SHOT_TICKS=<vsync>,... (diagnostics, development builds;
        // full-m2 "Multi"): the picture as this frame places it in the window (drawPicture, with the three screens'
        // pieces), drawn again into a texture of the window's size with plain bilinear scaling and saved as
        // <prefix>-<vsync>-window.png at the first frame at or after each VSync asked (works in hidden runs too).
        {
            static const char *windowPictureShot = [] {
                const char *v = std::getenv("PS2X_WINDOW_SHOT");
                return (v && *v) ? v : nullptr;
            }();
            static std::vector<uint64_t> windowPictureTicks = [] {
                std::vector<uint64_t> t;
                const char *v = std::getenv("PS2X_WINDOW_SHOT_TICKS");
                std::stringstream in(v ? v : "");
                for (std::string item; std::getline(in, item, ',');)
                    if (!item.empty())
                        t.push_back(std::strtoull(item.c_str(), nullptr, 10));
                std::sort(t.rbegin(), t.rend()); // the next one last
                return t;
            }();
            const uint64_t shotTick = m_memory.gs().vsyncTick.load();
            if (windowPictureShot && !windowPictureTicks.empty() && shotTick >= windowPictureTicks.back())
            {
                while (!windowPictureTicks.empty() && shotTick >= windowPictureTicks.back())
                    windowPictureTicks.pop_back();
                const int ww = GetRenderWidth(), wh = GetRenderHeight();
                RenderTexture2D target = LoadRenderTexture(ww, wh);
                if (target.id != 0)
                {
                    rlDrawRenderBatchActive();
                    BeginTextureMode(target);
                    ClearBackground(BLACK);
                    drawPicture(*shownTex);
                    EndTextureMode();
                    Image image = LoadImageFromTexture(target.texture);
                    ImageFlipVertical(&image);
                    char path[1024];
                    std::snprintf(path, sizeof(path), "%s-%06llu-window.png", windowPictureShot, static_cast<unsigned long long>(shotTick));
                    ExportImage(image, path);
                    UnloadImage(image);
                    UnloadRenderTexture(target);
                    std::cerr << "[window] picture shot at vsync " << shotTick << ": " << ww << "x" << wh << ", " << pieces.size() << " screen piece(s), picture "
                              << dstRect.width << "x" << dstRect.height << " at " << dstRect.x << "," << dstRect.y << ", shape " << aspect << std::endl;
                }
            }
        }
#endif
#if defined(PS2X_UPSCALERS)
        if (motionView && motionBufferReady && pieces.empty())
            motionBuffer.drawDebug(dstRect);
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // The launcher's race pictures (gt4_hud_pictures.h): the game rectangle (on triple screens the middle screen's part,
        // where GT4's HUD is), and the one frame a capture wants the window as shown with GT4's HUD and none of ours.
        const Rectangle hudPicRect = pieces.size() == 3u ? pieces[1].dst : dstRect;
        gt4hudpic::noteGameRect(hudPicRect.width, hudPicRect.height);
        const bool hudPicShot = gt4hudpic::hudShotTurn();
#endif
#if defined(PS2X_GS_VK)
        // With the window presenter, the overlays below are drawn into a render texture of the
        // window's size, premultiplied (colour * alpha; alpha accumulated), for the presenter.
        if (vkPresents)
        {
            const int ow = GetScreenWidth(), oh = GetScreenHeight();
            if (ow > 0 && oh > 0 && (vkOverlay.id == 0 || vkOverlay.texture.width != ow || vkOverlay.texture.height != oh))
            {
                if (vkOverlay.id != 0)
                    UnloadRenderTexture(vkOverlay);
                vkOverlay = LoadRenderTexture(ow, oh);
            }
            if (vkOverlay.id != 0)
            {
                BeginTextureMode(vkOverlay);
                ClearBackground(BLANK);
                rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD, RL_FUNC_ADD);
                BeginBlendMode(BLEND_CUSTOM_SEPARATE);
                vkOverlayOpen = true;
            }
        }
#endif
        {
#if GT3_ADDONS
            // Race cameras (gt3_camera.h): F9 / Shift+F9, the right stick.
            {
                std::string note;
                gt3camera::hostUpdate(note);
                if (!note.empty())
                    showNote(note);
            }
            // Proximity radar (gt3_radar.h; F8: off / when cars are near /
            // always; Shift+F8: move, resize, opacity with the mouse and keys).
            {
                bool saved = false;
                std::string note;
                gt3radar::handleKeys(saved, note);
                if (!note.empty())
                    showNote(note);
                if (saved)
                    saveRadarSettings();
            }
            gt3radar::draw(m_memory.getRDRAM(), dstRect.x, dstRect.y, dstRect.width, dstRect.height);
            // Our own HUD widgets (gt3_hud.h), in GT3's speed units.
            gt3hud::setSpeedUnits(hudUnits[0] >= 0 ? hudUnits[0] : 1); // not set: the US game's own mph
            gt3hud::draw(m_memory.getRDRAM(), dstRect.x, dstRect.y, dstRect.width, dstRect.height);
            // The end-of-race report's data (GT3_RACE_REPORT; gt3_race_report.h), every frame.
            gt3report::frame(m_memory.getRDRAM());
#endif
#if GT3_ADDONS
            if (IsKeyPressed(KEY_F3))
                overlay = !overlay;
#endif
            if (overlay)
            {
                static GS::FrameStats shown{};
                static double lastUpdate = 0.0;
                const double now = GetTime();
                if (now - lastUpdate >= 0.25)
                {
                    shown = m_gs.frameStats();
                    lastUpdate = now;
                }
                char text[128];
                std::snprintf(text, sizeof(text), "FPS %5.1f   frame %5.1f ms   max %5.1f ms", shown.fps, shown.avgMs, shown.maxMs);
#if defined(PS2X_UPSCALERS)
                if (frameGenRate && frameGenReady)
                    std::snprintf(text + std::strlen(text), sizeof(text) - std::strlen(text), "   shown %d (frame generation)", GetFPS());
#endif
                const int fontSize = std::max(14, static_cast<int>(screenHeight / 40.0f));
                const int textWidth = MeasureText(text, fontSize);
                ps2x::overlay::mark(8.0f, 8.0f, static_cast<float>(textWidth + 16), static_cast<float>(fontSize + 10));
                DrawRectangle(8, 8, textWidth + 16, fontSize + 10, Fade(BLACK, 0.6f));
                const Color colour = shown.fps >= 59.0 ? GREEN : (shown.fps >= 45.0 ? YELLOW : RED);
                DrawText(text, 16, 13, fontSize, colour);
            }
#if GT3_ADDONS
            // Motion data debug view (F6, GT3_MOTION_DEBUG; gt3_motion.h): each
            // main-view vertex's move since the previous frame, drawn 4x long,
            // coloured by direction.
            {
                gt3motion::Tracker &motion = gt3motion::Tracker::instance();
                static bool vectorView = [] { // GT3_MOTION_DEBUG: this view from the start
                    const char *v = std::getenv("GT3_MOTION_DEBUG");
                    return v && *v && *v != '0';
                }();
                if (IsKeyPressed(KEY_F6))
                {
                    vectorView = !vectorView;
                    showNote(vectorView ? "Motion vectors shown (F6)" : "Motion vectors off (F6)");
                }
#if defined(PS2X_UPSCALERS)
                motion.setEnabled(vectorView || motionView);
#else
                motion.setEnabled(vectorView);
#endif
                if (vectorView)
                {
                    static std::vector<gt3motion::Sample> samples;
                    gt3motion::FrameStats ms;
                    motion.latest(samples, ms);
                    const float kx = dstRect.width / 640.0f, ky = dstRect.height / 448.0f;
                    for (const gt3motion::Sample &s : samples)
                    {
                        const float dx = s.x - s.px, dy = s.y - s.py;
                        const float length = std::sqrt(dx * dx + dy * dy);
                        const Color c = ColorFromHSV(std::atan2(dy, dx) * 57.29578f + 180.0f, 0.9f, std::min(1.0f, 0.4f + length / 8.0f));
                        const Vector2 a{dstRect.x + s.x * kx, dstRect.y + s.y * ky};
                        DrawLineV(a, Vector2{a.x - dx * 4.0f * kx, a.y - dy * 4.0f * ky}, c);
                    }
                    char text[200];
                    std::snprintf(text, sizeof(text), "motion: %u of %u main-view batches found in the previous frame, %u vertices, %u rejected",
                                  ms.matched, ms.mainBatches, ms.vertices, ms.rejected);
                    const int fontSize = std::max(14, static_cast<int>(screenHeight / 45.0f));
                    DrawRectangle(8, static_cast<int>(screenHeight) - fontSize - 18, MeasureText(text, fontSize) + 16, fontSize + 10, Fade(BLACK, 0.6f));
                    DrawText(text, 16, static_cast<int>(screenHeight) - fontSize - 13, fontSize, YELLOW);
                }
            }
            // The record button (F10): every lap driven goes to a file for GT Race Lab
            // (gt3_drive_log.h); a red "REC" top right while it records.
            if (IsKeyPressed(KEY_F10))
            {
                const bool on = gt3drivelog::toggle();
                showNote(on ? "Recording laps to " + gt3drivelog::where() + " (F10 stops)" : std::string("Recording stopped (F10)"));
            }
            if (gt3drivelog::recording())
            {
                char text[96];
                const int lap = gt3drivelog::openLap();
                const uint32_t last = gt3drivelog::lastLapMs();
                if (lap > 0 && last > 0)
                    std::snprintf(text, sizeof(text), "REC  lap %d  (last %u'%06.3f)", lap, last / 60000u, (last % 60000u) / 1000.0);
                else if (lap > 0)
                    std::snprintf(text, sizeof(text), "REC  lap %d", lap);
                else
                    std::snprintf(text, sizeof(text), "REC  waiting for a race");
                const int fontSize = std::max(14, static_cast<int>(screenHeight / 42.0f));
                const int w = MeasureText(text, fontSize) + fontSize + 22;
                const int x = static_cast<int>(screenWidth) - w - 10, y = 10;
                DrawRectangle(x, y, w, fontSize + 12, Fade(BLACK, 0.65f));
                const bool blink = std::fmod(GetTime(), 1.0) < 0.6;
                DrawCircle(x + 10 + fontSize / 2, y + 6 + fontSize / 2, fontSize * 0.38f, blink ? RED : Fade(RED, 0.35f));
                DrawText(text, x + 16 + fontSize, y + 6, fontSize, WHITE);
            }
            // Grid-start lights (GT3_START_LIGHTS; gt3_start_lights.h) in place of GT3's
            // "3 2 1 START": a housing of 3-5 columns, two red lamps each, over the
            // picture's upper middle (below the mirror) while the countdown runs.
            {
                const gt3startlights::Panel lights = gt3startlights::panel();
                if (lights.show && lights.count > 0)
                {
                    const float cell = dstRect.height * 0.072f;
                    const float r = cell * 0.34f;
                    const float w = cell * (static_cast<float>(lights.count) + 0.5f), h = cell * 2.4f;
                    const float cx = dstRect.x + dstRect.width * 0.5f, top = dstRect.y + dstRect.height * 0.26f;
                    const Rectangle box{cx - w * 0.5f, top, w, h};
                    DrawRectangleRounded(box, 0.22f, 8, Color{12, 12, 14, 235});
                    DrawRectangleRounded(Rectangle{box.x + cell * 0.08f, box.y + cell * 0.08f, box.width - cell * 0.16f, box.height - cell * 0.16f}, 0.2f, 8,
                                         Color{26, 26, 30, 235});
                    for (int i = 0; i < lights.count; ++i)
                    {
                        const bool on = i < lights.lit;
                        const float x = box.x + cell * 0.25f + cell * (static_cast<float>(i) + 0.5f);
                        for (int row = 0; row < 2; ++row)
                        {
                            const Vector2 c{x, box.y + cell * 0.2f + cell * (static_cast<float>(row) + 0.5f)};
                            if (on)
                                DrawCircleV(c, r * 1.55f, Color{255, 40, 20, 70});
                            DrawCircleV(c, r, on ? Color{255, 36, 22, 255} : Color{58, 18, 18, 255});
                            if (on)
                                DrawCircleV(Vector2{c.x - r * 0.3f, c.y - r * 0.3f}, r * 0.28f, Color{255, 170, 150, 200});
                        }
                    }
                }
            }
            // The end-of-race report (GT3_RACE_REPORT; F2), over everything else.
            gt3report::draw(dstRect.x, dstRect.y, dstRect.width, dstRect.height);
#endif
            // A setting changed while playing: what it is now, for a moment.
            if (!osdText.empty() && GetTime() < osdUntil)
            {
                const int fontSize = std::max(16, static_cast<int>(screenHeight / 32.0f));
                const int textWidth = MeasureText(osdText.c_str(), fontSize);
                const int x = (static_cast<int>(screenWidth) - textWidth) / 2, y = static_cast<int>(screenHeight) - fontSize * 3;
                const float fade = static_cast<float>(std::min(1.0, (osdUntil - GetTime()) / 0.5));
                ps2x::overlay::mark(static_cast<float>(x - 16), static_cast<float>(y - 8), static_cast<float>(textWidth + 32), static_cast<float>(fontSize + 16));
                DrawRectangle(x - 16, y - 8, textWidth + 32, fontSize + 16, Fade(BLACK, 0.7f * fade));
                DrawText(osdText.c_str(), x, y, fontSize, Fade(WHITE, fade));
            }
#if defined(PS2X_GAME_GT4)
            // The in-game cheat card (GT4_CHEAT_CARD, F2; gt4_cheat_card.h): window only, after the scanline pass.
            gt4cheatcard::frame({screenWidth, screenHeight, dstRect.x, dstRect.y, dstRect.width, dstRect.height, !m_headless,
                                 m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u, shownTex, static_cast<int>(srcWidth),
                                 static_cast<int>(srcHeight), aspect, g_shownInterlaced});
#if !defined(GT4_STOCK_BUILD)
            // The Race Lab lap recorder (GT4_DRIVE_LOG; gt4_drive_log.h): F10 stops and starts it, a red "REC" top right.
            if (gt4drivelog::active())
            {
                if (!m_headless && IsKeyPressed(KEY_F10))
                    showNote(gt4drivelog::toggle() ? "Recording laps for the Race Lab (F10 stops)" : std::string("Lap recording stopped (F10)"));
                if (!m_headless && gt4drivelog::recording())
                {
                    char text[96];
                    const int lap = gt4drivelog::openLap();
                    const uint32_t last = gt4drivelog::lastLapMs();
                    if (lap > 0 && last > 0)
                        std::snprintf(text, sizeof(text), "REC  lap %d  (last %u'%06.3f)", lap, last / 60000u, (last % 60000u) / 1000.0);
                    else if (lap > 0)
                        std::snprintf(text, sizeof(text), "REC  lap %d", lap);
                    else
                        std::snprintf(text, sizeof(text), "REC  waiting for a race");
                    const int fontSize = std::max(14, static_cast<int>(screenHeight / 42.0f));
                    const int w = MeasureText(text, fontSize) + fontSize + 22;
                    const int x = static_cast<int>(screenWidth) - w - 10, y = 10;
                    DrawRectangle(x, y, w, fontSize + 12, Fade(BLACK, 0.65f));
                    const bool blink = std::fmod(GetTime(), 1.0) < 0.6;
                    DrawCircle(x + 10 + fontSize / 2, y + 6 + fontSize / 2, fontSize * 0.38f, blink ? RED : Fade(RED, 0.35f));
                    DrawText(text, x + 16 + fontSize, y + 6, fontSize, WHITE);
                }
            }
            // The FPS counter (GT4_FPS_COUNTER, F3; gt4_fps_counter.h): window only, after the scanline pass.
            if (!m_headless && IsKeyPressed(KEY_F3))
            {
                const gt4fps::Mode fpsMode = gt4fps::cycle();
                showNote(fpsMode == gt4fps::Mode::Off ? "FPS counter off (F3)" : fpsMode == gt4fps::Mode::Simple ? "FPS counter: simple (F3)" : "FPS counter: detailed (F3)");
            }
            if (gt4fps::shown() && !hudPicShot)
            {
                static float fpsGaps[1024];
                const GS::FrameStats fpsStats = m_gs.frameStats();
                const size_t fpsGapCount = m_gs.recentFrameGapsMs(fpsGaps, 1024);
                // The internal resolution for the detailed counter: GT4's 640 x 448 picture at the renderer's scale (across x down).
                static std::string fpsResolution;
#if defined(PS2X_GS_GL)
                if (glRenderer)
                {
                    static const bool fpsFast = [] {
                        const char *v = std::getenv("GT3_GS_GL_FAST");
                        return v && *v && *v != '0';
                    }();
                    const int fs = gsgl::renderScale(), fsx = gsgl::presentScale(), fsy = gsgl::presentScaleY();
                    char fr[96];
                    std::snprintf(fr, sizeof(fr), "%s   %d x %d%s", fs <= 1 ? "Native" : (std::to_string(fs) + "x").c_str(), 640 * fsx, 448 * fsy,
                                  fs <= 1 ? "" : fpsFast ? "   fast" : "   exact");
                    fpsResolution = fr;
                }
                else
#endif
                    fpsResolution = "Native   640 x 448";
                gt4fps::draw({screenWidth, screenHeight, fpsStats.fps, fpsStats.maxMs, fpsGaps, fpsGapCount, fpsResolution.c_str()});
            }
            // Our own race HUD widgets and start lights (GT4_HUD_*, GT4_START_*; gt4_hud.h): on the game picture (on triple
            // screens the middle screen's part, with GT4's own HUD), after the scanline pass and all scaling.
            // The Race HUD zoom key (HUD hand-off 9; GT4_HUD_ZOOM_KEY, z by default): kept from GT4's keyboard (ps2_pad.cpp).
            if (!m_headless)
                if (const int zk = gt4hud::zoomKey(); zk > 0 && IsKeyPressed(zk))
                    gt4hud::zoomPress();
            if (gt4hud::enabled() && !hudPicShot)
            {
                Rectangle hudRect = pieces.size() == 3u ? pieces[1].dst : dstRect;
                // (21 Oct: GT4_PICTURE_FIT=fill crops the picture past the window's edges; the boxes are placed and kept in the
                // part that shows, so one at the right edge, the developer's tower at x 88.6 %, sits fully on the screen)
                {
                    const float x0 = std::max(hudRect.x, 0.0f), y0 = std::max(hudRect.y, 0.0f);
                    const float x1 = std::min(hudRect.x + hudRect.width, static_cast<float>(screenWidth));
                    const float y1 = std::min(hudRect.y + hudRect.height, static_cast<float>(screenHeight));
                    if (x1 - x0 >= 16.0f && y1 - y0 >= 16.0f)
                        hudRect = Rectangle{x0, y0, x1 - x0, y1 - y0};
                }
                gt4hud::draw(screenWidth, screenHeight, hudRect.x, hudRect.y, hudRect.width, hudRect.height);
            }
#endif
#endif
        }
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            ps2x::overlay::mark(0.0f, 0.0f, ps2x::overlay::kAll, ps2x::overlay::kAll);
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
#if defined(PS2X_GS_VK)
        if (vkOverlayOpen)
        {
            EndBlendMode();
            EndTextureMode();
            vkOverlayOpen = false;
        }
#endif
        // GT3_WINDOW_SHOT=<prefix> (diagnostics): the window as shown, overlays
        // included, every GT3_WINDOW_SHOT_EVERY VSyncs (default 300).
        {
            static const char *windowShot = [] {
                const char *v = std::getenv("GT3_WINDOW_SHOT");
                return (v && *v) ? v : nullptr;
            }();
            static const uint64_t windowShotEvery = [] {
                const char *v = std::getenv("GT3_WINDOW_SHOT_EVERY");
                return static_cast<uint64_t>(std::max(1L, (v && *v) ? std::strtol(v, nullptr, 10) : 300L));
            }();
            // (GT3_WINDOW_SHOT_RANGE=<first>,<count>: every presented frame from VSync <first>, <count> pictures)
            static const std::pair<uint64_t, uint64_t> windowShotRange = [] {
                unsigned long long a = 0, b = 0;
                const char *v = std::getenv("GT3_WINDOW_SHOT_RANGE");
                if (v && std::sscanf(v, "%llu,%llu", &a, &b) == 2)
                    return std::pair<uint64_t, uint64_t>{a, b};
                return std::pair<uint64_t, uint64_t>{0u, 0u};
            }();
            static uint64_t lastWindowShot = 0, rangeShots = 0;
            const uint64_t tick = m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u;
            const bool inRange = windowShotRange.second && tick >= windowShotRange.first && rangeShots < windowShotRange.second && tick != lastWindowShot;
            if (inRange)
                ++rangeShots;
            if (windowShot && (inRange || (!windowShotRange.second && tick >= lastWindowShot + windowShotEvery)))
            {
                lastWindowShot = tick;
                char path[1024];
                std::snprintf(path, sizeof(path), "%s-%06llu.png", windowShot, static_cast<unsigned long long>(tick));
#if defined(PS2X_GS_VK)
                if (vkPresents)
                    vkShotPath = path; // the presenter reads the window back as it shows it (below)
                else
#endif
                {
                    rlDrawRenderBatchActive(); // raylib draws its batch at EndDrawing
                    Image image = LoadImageFromScreen();
                    ExportImage(image, path);
                    UnloadImage(image);
                }
            }
        }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // (the launcher's race pictures without the Vulkan window presenter: the window read back here)
        if (hudPicShot
#if defined(PS2X_GS_VK)
            && !vkPresents
#endif
        )
        {
            rlDrawRenderBatchActive();
            Image image = LoadImageFromScreen();
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (image.data)
                gt4hudpic::hudShot(static_cast<const uint8_t *>(image.data), image.width, image.height, hudPicRect.x, hudPicRect.y, hudPicRect.width,
                                   hudPicRect.height);
            UnloadImage(image);
        }
#endif
#if !defined(PS2X_GAME_GT3)
        if (gridPacer.enabled)
            gridPacer.beforeSwap();
#endif
#if defined(PS2X_GS_VK)
        if (vkPresents)
        {
            // The frame through the Vulkan swapchain (EndDrawing then only flushes OpenGL).
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            const bool picShot = hudPicShot;
#else
            const bool picShot = false;
#endif
            gsvk::present::presentFrame(vkOverlay.id, vkOverlay.texture.width, vkOverlay.texture.height, GetScreenWidth(), GetScreenHeight(),
                                        !vkShotPath.empty() || picShot);
            if (!vkShotPath.empty() || picShot)
            {
                std::vector<uint8_t> rgba;
                int sw = 0, sh = 0;
                if (gsvk::present::takeShot(rgba, sw, sh))
                {
                    if (!vkShotPath.empty())
                    {
                        Image image{rgba.data(), sw, sh, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                        ExportImage(image, vkShotPath.c_str());
                    }
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
                    if (picShot)
                        gt4hudpic::hudShot(rgba.data(), sw, sh, hudPicRect.x, hudPicRect.y, hudPicRect.width, hudPicRect.height);
#endif
                }
                vkShotPath.clear();
            }
        }
#endif
        EndDrawing();
#if !defined(PS2X_GAME_GT3)
        if (gridPacer.enabled)
            gridPacer.presented(m_eeScheduler ? m_eeScheduler->currentVSyncTick() : 0u);
#endif
        logFrameRate(++presents);

        if (WindowShouldClose())
        {
#if defined(PS2X_GAME_GT4)
            // GT4: the close button / Alt+F4 is the console's power button (POWOFF): at the next
            // VBlank the wheel's forces stop and the IOP services shut down in POWOFF's order,
            // then the quit handler requests the stop. Keep presenting until then; stop anyway
            // after 1.5 s (EE stalled, no VBlank). PS2X_POWER_BUTTON=0: stop at once.
            static double powerPressedAt = -1.0;
            if (powerPressedAt < 0.0 && !m_headless && ps2_gt4_glue::windowCloseToPowerButton())
            {
                powerPressedAt = GetTime();
                std::cerr << "[power] window closed: GT4 power button pressed" << std::endl;
            }
            if (powerPressedAt >= 0.0 && GetTime() - powerPressedAt < 1.5)
                continue;
            if (powerPressedAt >= 0.0)
                std::cerr << "[power] no power-off sequence within 1.5 s: stopping" << std::endl;
#endif
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    // Before waiting for the game thread: linked consoles go on at once.
#if defined(PS2X_GAME_GT3)
    ps2x::iop::gt3::LinkTransport::goodbyeAll();
#endif
    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
#if defined(PS2X_GS_VK)
    // The Vulkan renderer's pipeline cache and key list (main() ends with _Exit).
    gsvk::pipelineCacheSave("exit");
#endif

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    m_padBackend.stopRumble(); // the game has stopped: the controllers' motors off (main() ends with _Exit, no destructors)
    if (sharpShader.id != 0)
        UnloadShader(sharpShader);
    if (crtShader.id != 0)
        UnloadShader(crtShader);
#if defined(PS2X_SCANLINE_PASS)
    scanPass.shutdown();
#endif
#if defined(PS2X_GAME_GT4)
    gt4cheatcard::shutdown();
    gt4texdump::shutdown(); // the textures still waiting are written
    gt4texreplace::shutdown();
#if !defined(GT4_STOCK_BUILD)
    gt4pages::shutdown();
#endif
#endif
#if defined(PS2X_UPSCALERS)
    upscaler.shutdown();
#endif
#if GT3_ADDONS
    phoneview::stop();
    gt3hud::shutdown(); // the timing tower's name thread reads EE RAM
    gt3texdump::shutdown(); // the textures still waiting are written
    gt3texreplace::shutdown();
    gt3rec::shutdown(); // the video's last frames need the OpenGL context
#endif
#if defined(PS2X_GS_VK)
    ps2x::overlay::hook() = nullptr;
    gsvk::present::shutdown();
    if (vkOverlay.id != 0)
        UnloadRenderTexture(vkOverlay);
#endif
    UnloadTexture(frameTex);
    UnloadTexture(fgSpare);
#if defined(_WIN32) && !defined(PS2X_GAME_GT3)
    if (!m_headless)
        updateDisplayRequired(false);
#endif
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
