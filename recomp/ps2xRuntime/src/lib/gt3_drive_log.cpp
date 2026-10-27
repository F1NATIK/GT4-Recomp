#include "gt3_drive_log.h"

#include "gt3_course_patch.h"
#include "gt3_race_info.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace gt3drivelog
{
    namespace
    {
        // All disk work on a thread of its own: the pad thread (which the game waits on every
        // frame) only adds text to memory. Writing there cost frames: Trial Mountain, Vulkan 4x
        // progressive, 719 -> 835 frames over 17.5 ms and a 74 ms frame (run rec-ab, 2026-10-02).
        class Writer
        {
        public:
            ~Writer()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_cv.notify_all();
                if (m_thread.joinable())
                    m_thread.join(); // what is queued still reaches the disk on a normal exit
            }
            void open(std::string path, std::string text) { push(Job{0, std::move(path), std::move(text)}); }
            void append(std::string text) { push(Job{1, {}, std::move(text)}); }
            void close(std::string text, std::string note) { push(Job{2, std::move(note), std::move(text)}); }
            void discard(std::string note) { push(Job{3, std::move(note), {}}); } // close and delete the file

        private:
            struct Job
            {
                int kind; // 0 open (path) and write, 1 write, 2 write and close, 3 close and delete (2, 3: path = a note for stderr)
                std::string path, text;
            };
            void push(Job job)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_jobs.push_back(std::move(job));
                    if (!m_thread.joinable())
                        m_thread = std::thread([this] { run(); });
                }
                m_cv.notify_one();
            }
            void run()
            {
                std::FILE *f = nullptr;
                std::string current; // the open file's path
                for (;;)
                {
                    std::deque<Job> jobs;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                        if (m_jobs.empty())
                            break; // stopping, nothing left
                        jobs.swap(m_jobs);
                    }
                    for (Job &job : jobs)
                    {
                        if (job.kind == 0)
                        {
                            if (f)
                                std::fclose(f);
                            std::error_code ec;
                            std::filesystem::create_directories(std::filesystem::path(job.path).parent_path(), ec);
                            f = std::fopen(job.path.c_str(), "w");
                            current = job.path;
                        }
                        if (f && !job.text.empty())
                            std::fwrite(job.text.data(), 1, job.text.size(), f);
                        if (job.kind == 2 || job.kind == 3)
                        {
                            if (f)
                                std::fclose(f);
                            f = nullptr;
                            if (job.kind == 3 && !current.empty())
                            {
                                std::error_code ec;
                                std::filesystem::remove(current, ec);
                            }
                            current.clear();
                            std::fputs(job.path.c_str(), stderr);
                        }
                    }
                }
                if (f)
                    std::fclose(f);
            }
            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<Job> m_jobs;
            std::thread m_thread;
            bool m_stop = false;
        };
        Writer &writer()
        {
            static Writer w;
            return w;
        }

        struct Lap
        {
            bool open = false;
            bool fromStart = false; // the first lap's file, begun at the start (lap 0, before the line)
            std::string buf; // rows not yet handed to the writer
            std::string path;
            int lap = -1;    // GT3's lap number now (0 while a start file is still short of the line)
            uint32_t startMs = 0;
            uint32_t race = 0;
            uint32_t lastMs = 0;
            float lastX = 0.0f, lastZ = 0.0f; // the last row's place (GT3_DRIVE_LOG_STEP)
            uint64_t lastTick = ~0ull; // one row a frame (GT3 reads the pad more than once a frame)
            int rows = 0;
        };

        Lap g_lap;
        std::string g_stamp; // per race: yyyymmdd-hhmmss

        // GT3_DRIVE_LOG_ALL=1: everything while the race clock runs, lap 0 in a file of its own
        // (the recorder before 2026-10-02), GT3's own driving of the car included.
        bool allRows()
        {
            static const bool all = [] {
                const char *v = std::getenv("GT3_DRIVE_LOG_ALL");
                return v && *v && *v != '0';
            }();
            return all;
        }

        // GT3's AI driver (0x1f7ae0, a0 = a car's controller, car + 0xec8; as in gt3_autopilot.cpp). When
        // it runs for the player's car, GT3 is driving it (Free Run's AUTO DRIVE up to the start line, the
        // car after the finish): not the player's driving. Every controller it runs for gets a slot (up to
        // 8 cars, so the player's is never pushed out by the AI cars') counting its calls; GT3's VBlank count
        // only picks the slot to reuse (it moves several times a frame with GT3_TURBO).
        constexpr uint32_t kAiDriver = 0x1f7ae0u, kVblankCounter = 0x4a69c0u;
        constexpr int kSlots = 8;
        PS2Runtime::RecompiledFunction g_aiNext = nullptr;
        std::atomic<uint32_t> g_aiController[kSlots]{};
        std::atomic<uint32_t> g_aiCalls[kSlots]{};
        std::atomic<uint32_t> g_aiVblank[kSlots]{};
        void aiDriverHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t controller = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & 0x1FFFFFFu;
            uint32_t vblank;
            std::memcpy(&vblank, rdram + kVblankCounter, 4);
            int slot = -1;
            for (int i = 0; i < kSlots && slot < 0; ++i)
                if (g_aiController[i].load(std::memory_order_relaxed) == controller)
                    slot = i;
            if (slot < 0)
            {
                // a free slot, else the one unused the longest
                uint32_t oldest = 0;
                slot = 0;
                for (int i = 0; i < kSlots; ++i)
                {
                    if (g_aiController[i].load(std::memory_order_relaxed) == 0u)
                    {
                        slot = i;
                        break;
                    }
                    const uint32_t age = vblank - g_aiVblank[i].load(std::memory_order_relaxed);
                    if (age >= oldest)
                    {
                        oldest = age;
                        slot = i;
                    }
                }
                g_aiController[slot].store(controller, std::memory_order_relaxed);
            }
            g_aiVblank[slot].store(vblank, std::memory_order_relaxed);
            g_aiCalls[slot].fetch_add(1u, std::memory_order_relaxed);
            g_aiNext(rdram, ctx, runtime);
        }
        // GT3's AI driver ran for the player's car in this game frame or the 3 before (it runs every frame
        // it drives; frames counted by the race clock's steps, so GT3_TURBO doesn't matter). Pad thread only.
        bool gt3Drives(const gt3race::Snapshot &s)
        {
            static uint32_t lastClock = ~0u, lastCalls = 0u, quietFrames = 99u;
            if (!g_aiNext)
                return false;
            const uint32_t controller = (s.race + 0xc8u + 0xec8u) & 0x1FFFFFFu;
            uint32_t calls = 0u;
            for (int i = 0; i < kSlots; ++i)
                if (g_aiController[i].load(std::memory_order_relaxed) == controller)
                    calls = g_aiCalls[i].load(std::memory_order_relaxed);
            if (s.clockMs == 0u)
            {
                // the countdown (not recorded): keep up with the calls, so the green light's frame
                // counts only what GT3 does from then on
                lastClock = 0u;
                lastCalls = calls;
                return false;
            }
            if (s.clockMs != lastClock)
            {
                lastClock = s.clockMs;
                quietFrames = calls != lastCalls ? 0u : std::min(quietFrames + 1u, 99u);
                lastCalls = calls;
            }
            return quietFrames <= 3u;
        }

        // GT3_DRIVE_LOG=<folder>: recording from the start, into that folder; without it the
        // record key (F10) starts it, into "recordings" in the working folder
        const char *envFolder()
        {
            const char *v = std::getenv("GT3_DRIVE_LOG");
            return v && *v ? v : nullptr;
        }
        const std::string &folder()
        {
            static const std::string dir = envFolder() ? envFolder() : "recordings";
            return dir;
        }
        std::atomic<bool> g_on{envFolder() != nullptr}; // recording wanted (the pad thread follows it)
        std::atomic<bool> g_open{false};                 // a lap file is open
        std::atomic<int> g_openLap{0};
        std::atomic<uint32_t> g_savedMs{0};              // the last finished lap's time (for the screen)

        // GT3_DRIVE_LOG_STEP=<metres>: a row only once the car has moved that far
        // (0.01 = 1 cm ... 5); unset or 0: every frame GT3 runs (the most there is).
        float stepMetres()
        {
            static const float step = [] {
                const char *v = std::getenv("GT3_DRIVE_LOG_STEP");
                return v && *v ? std::max(0.0f, static_cast<float>(std::atof(v))) : 0.0f;
            }();
            return step;
        }

        std::string courseKey()
        {
            uint32_t crc = 0;
            std::string replacement;
            if (!gt3coursepatch::lastCourse(crc, replacement))
                return "unknown";
            if (!replacement.empty())
                return replacement;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "runway-%08x", crc);
            return buf;
        }

        void closeLap(bool complete, uint32_t endMs)
        {
            if (!g_lap.open)
                return;
            char tail[64];
            if (complete)
            {
                std::snprintf(tail, sizeof(tail), "# lap_time_ms %u\n", endMs - g_lap.startMs);
                if (g_lap.lap >= 1) // (lap 0 is the grid to the line)
                    g_savedMs = endMs - g_lap.startMs;
            }
            else
                std::snprintf(tail, sizeof(tail), "# incomplete\n");
            g_lap.buf += tail;
            char note[400];
            // a lap given up within its first second isn't kept (GT3 taking the car back at once, a restart)
            const bool discard = !complete && g_lap.rows < 60;
            std::snprintf(note, sizeof(note), "[drive-log] %s: %d rows%s\n", g_lap.path.c_str(), g_lap.rows,
                          complete ? "" : discard ? " (lap not finished, under a second: removed)" : " (lap not finished)");
            if (discard)
                writer().discard(note);
            else
                writer().close(std::move(g_lap.buf), note);
            g_lap.buf.clear();
            g_lap.open = false;
            g_lap.fromStart = false;
            g_open = false;
        }

        void openLap(const gt3race::Snapshot &s, const gt3race::Car &me, bool autopilot)
        {
            // the start (lap 0, short of the line) opens the first lap's file: from the green light
            // off the grid (its time counted from the clock's zero, as GT3 counts lap 1), or from
            // where the player took the car over on a rolling start
            const bool fromStart = !allRows() && me.lap == 0;
            const bool grid = fromStart && me.speed < 1.0f;
            const int fileLap = fromStart ? 1 : me.lap;
            char name[96];
            std::snprintf(name, sizeof(name), "%s-%s-lap%02d.csv", courseKey().c_str(), g_stamp.c_str(), fileLap);
            g_lap.path = (std::filesystem::path(folder()) / name).string();
            g_lap.lap = me.lap;
            g_lap.fromStart = fromStart;
            g_lap.startMs = grid ? 0u : s.clockMs;
            g_lap.race = s.race;
            g_lap.rows = 0;
            g_lap.lastMs = 0;
            char head[400];
            std::snprintf(head, sizeof(head), "# gt3-drive-log 1\n# course %s\n# driver %s\n# car %s\n# lap %d\n%s"
                                              "t_ms,lap_m,x,y,z,speed_ms,gear,rpm,throttle,brake,steer,surfaces,yaw,vsync\n",
                          courseKey().c_str(), autopilot ? "autopilot" : "player", me.name.c_str(), fileLap,
                          !fromStart ? "" : grid ? "# start grid\n" : "# start rolling\n");
            writer().open(g_lap.path, head); // the writer makes the folder and the file
            g_lap.buf.clear();
            g_lap.buf.reserve(40000);
            g_lap.open = true;
            g_open = true;
            g_openLap = fileLap;
        }
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        if (g_aiNext || allRows())
            return;
        // addiu sp, sp, -0x110 at 0x1f7ae0 (GT3 USA CORE), as the autopilot checks
        uint32_t first;
        std::memcpy(&first, rdram + kAiDriver, 4);
        if (first == 0x27bdfef0u && (g_aiNext = runtime.lookupFunction(kAiDriver)) != nullptr)
            runtime.registerFunction(kAiDriver, &aiDriverHook);
        else
            std::fprintf(stderr, "[drive-log] GT3's AI driver not found: GT3's own driving of the car is recorded too\n");
    }

    bool enabled()
    {
        return g_on.load() || g_open.load(); // (an open lap still has to be closed after a stop)
    }
    bool recording() { return g_on.load(); }
    bool toggle()
    {
        g_on = !g_on.load();
        return g_on.load();
    }
    int openLap() { return g_open.load() ? g_openLap.load() : 0; }
    uint32_t lastLapMs() { return g_savedMs.load(); }
    const std::string &where() { return folder(); }

    void onPad(const uint8_t *rdram, uint64_t vsyncTick, const std::array<uint8_t, 32> &packet, bool autopilot)
    {
        if (!g_on.load())
        {
            closeLap(false, 0); // stopped (F10) in the middle of a lap
            return;
        }
        if (!rdram || vsyncTick == g_lap.lastTick)
            return;
        gt3race::Snapshot s;
        if (!gt3race::read(rdram, s, false) || s.replay)
        {
            closeLap(false, 0);
            return;
        }
        const gt3race::Car *me = nullptr;
        for (const auto &c : s.cars)
            if (c.player)
            {
                me = &c;
                break;
            }
        if (!me)
            return;
        const bool all = allRows();
        const bool gt3 = !all && gt3Drives(s); // (followed in the countdown too)
        if (s.clockMs == 0)
            return;
        if (g_lap.open && (s.race != g_lap.race || s.clockMs < g_lap.lastMs))
            closeLap(false, 0); // a new race (or a restart)
        // GT3 drives the player's car (AUTO DRIVE, after the finish): not the player's driving
        if (gt3)
        {
            closeLap(false, 0);
            return;
        }
        // the first lap's file runs on over the line (it began at the start)
        if (g_lap.open && g_lap.fromStart && g_lap.lap == 0 && me->lap == 1)
        {
            char mark[48];
            std::snprintf(mark, sizeof(mark), "# line %u\n", s.clockMs);
            g_lap.buf += mark;
            g_lap.lap = 1;
        }
        if (!g_lap.open || me->lap != g_lap.lap)
        {
            const bool finished = g_lap.open && me->lap == g_lap.lap + 1;
            closeLap(finished, s.clockMs);
            // a lap is begun only at the line (switched on part way round: from the next line) or at
            // the start, before the line; none after the race's last lap
            if (!all && ((me->lap >= 1 && me->progress >= 30.0f) || (s.totalLaps > 0 && me->lap > s.totalLaps)))
                return;
            if (g_stamp.empty() || !finished)
            {
                const std::time_t now = std::time(nullptr);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &now);
#else
                localtime_r(&now, &tm);
#endif
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
                g_stamp = buf; // a new race (laps of one race share the stamp)
            }
            openLap(s, *me, autopilot);
        }
        // every frame GT3 runs (60 a second): its own update rate, the finest there is.
        // One row per race-clock step: the VSync tick moves twice a game frame, and
        // half the rows of the first test repeated the one before (Suzuka, 2026-10-01).
        if (g_lap.rows > 0 && s.clockMs == g_lap.lastMs)
            return;
        // with GT3_DRIVE_LOG_STEP only once the car has moved that far
        if (stepMetres() > 0.0f && g_lap.rows > 0 && std::hypot(me->x - g_lap.lastX, me->z - g_lap.lastZ) < stepMetres())
            return;
        // GT3's default controls: Cross (pressure index 6, byte 14) accelerates,
        // Square (7, byte 15) brakes; R2 (11, byte 19) / L2 (10, byte 18) for
        // pedals mapped there; the left stick's x (byte 6) steers. Without
        // pressures (0x73), the buttons (active low: Cross bit 14, Square 15).
        const uint16_t buttons = static_cast<uint16_t>(packet[2] | (packet[3] << 8));
        float throttle, brake;
        if (packet[1] == 0x79)
        {
            throttle = std::max(packet[14], packet[19]) / 255.0f;
            brake = std::max(packet[15], packet[18]) / 255.0f;
        }
        else
        {
            throttle = (buttons & 0x4000u) ? 0.0f : 1.0f;
            brake = (buttons & 0x8000u) ? 0.0f : 1.0f;
        }
        float steer = (static_cast<int>(packet[6]) - 128) / 127.0f;
        if (!(buttons & 0x0080u))
            steer = -1.0f; // d-pad left
        else if (!(buttons & 0x0020u))
            steer = 1.0f;  // d-pad right
        char row[256];
        const int n = std::snprintf(row, sizeof(row), "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%d%d%d%d,%.4f,%llu\n", s.clockMs, me->progress,
                                    me->x, me->y, me->z, me->speed, me->gear, me->rpm, throttle, brake, std::clamp(steer, -1.0f, 1.0f),
                                    me->surface[0] % 10, me->surface[1] % 10, me->surface[2] % 10, me->surface[3] % 10, me->yaw,
                                    static_cast<unsigned long long>(vsyncTick));
        if (n > 0)
            g_lap.buf.append(row, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(row)) - 1)));
        if (g_lap.buf.size() >= 32768) // ~6 s of rows to the writer
        {
            writer().append(std::move(g_lap.buf));
            g_lap.buf.clear();
            g_lap.buf.reserve(40000);
        }
        g_lap.lastTick = vsyncTick;
        g_lap.lastMs = s.clockMs;
        g_lap.lastX = me->x;
        g_lap.lastZ = me->z;
        ++g_lap.rows;
    }
}
