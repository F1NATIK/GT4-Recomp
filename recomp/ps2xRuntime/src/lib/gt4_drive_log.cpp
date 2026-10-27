// The lap and race recorder for GT Race Lab: see gt4_drive_log.h (Race Lab hand-off 1).
#include "gt4_drive_log.h"
#include "gt4_race_reader.h"
#include "gt4_ai_drivers.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gt4drivelog
{
    namespace
    {
        std::mutex g_lock;
        bool g_init = false, g_active = false, g_recording = false;
        std::string g_folder;
        // The race: its start time (the file names), course, the whole-race file.
        uint32_t g_course = ~0u;
        std::string g_stamp;
        std::FILE *g_race = nullptr;
        bool g_inRace = false;
        // The open lap: its number, rows, header facts.
        int g_lap = 0;
        std::vector<std::string> g_rows;
        std::string g_car, g_start;
        uint32_t g_t0 = 0, g_lastClock = ~0u, g_lapLastMsAtStart = 0;
        int g_closingLap = 0, g_closingWait = 0; // a finished lap waiting for GT4's lap time
        std::vector<std::string> g_closingRows;
        std::string g_closingCar, g_closingStart;
        uint32_t g_closingPrevLast = 0, g_lastLapMs = 0;

        void init()
        {
            if (g_init)
                return;
            g_init = true;
            const char *v = std::getenv("GT4_DRIVE_LOG");
            g_active = v && *v && std::string(v) != "off" && std::string(v) != "0";
            g_recording = g_active;
            if (g_active)
            {
                g_folder = v;
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::u8path(g_folder), ec);
                std::fprintf(stderr, "[gt4-drive-log] recording laps for the Race Lab into %s (F10 stops and starts)\n", g_folder.c_str());
            }
        }

        const char *track(uint32_t course)
        {
            // racelab/course-ids.json (the Race Lab thread's table: reader course id -> Race Lab track)
            switch (course)
            {
            case 81: return "gt4-mini";
            case 7: return "gt4-newyork";
            default: return nullptr;
            }
        }

        std::string stampNow()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char b[32];
            std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tm);
            return b;
        }

        std::string path(const char *suffix)
        {
            char b[96];
            std::snprintf(b, sizeof(b), "gt4-%u-%s-%s.csv", g_course, g_stamp.c_str(), suffix);
            const std::u8string u = (std::filesystem::u8path(g_folder) / b).u8string();
            return std::string(u.begin(), u.end());
        }

        void writeLap(int lap, const std::vector<std::string> &rows, const std::string &car, const std::string &start, uint32_t lapMs)
        {
            if (rows.empty())
                return;
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "lap%02d", lap);
            const std::string p = path(suffix);
            std::FILE *f = std::fopen(p.c_str(), "wb");
            if (!f)
            {
                std::fprintf(stderr, "[gt4-drive-log] cannot write %s\n", p.c_str());
                return;
            }
            std::fprintf(f, "# gt3-drive-log 1\r\n# game gt4\r\n");
            if (const char *t = track(g_course))
                std::fprintf(f, "# course replace-%s\r\n", t);
            else
                std::fprintf(f, "# course gt4-%u\r\n", g_course);
            std::fprintf(f, "# reader_course %u\r\n# driver player\r\n# car %s\r\n# lap %d\r\n", g_course, car.c_str(), lap);
            if (!start.empty())
                std::fprintf(f, "# start %s\r\n", start.c_str());
            if (lapMs)
                std::fprintf(f, "# lap_time_ms %u\r\n", lapMs);
            std::fprintf(f, "t_ms,lap_m,x,y,z,speed_ms,gear,rpm,throttle,brake,steer,surfaces,yaw,vsync\r\n");
            for (const std::string &r : rows)
                std::fprintf(f, "%s\r\n", r.c_str());
            std::fclose(f);
            if (lapMs)
                g_lastLapMs = lapMs;
            std::fprintf(stderr, "[gt4-drive-log] lap %d: %zu rows%s -> %s\n", lap, rows.size(), lapMs ? "" : " (no lap time: cut short)", p.c_str());
        }

        void flushClosing(uint32_t lapMs)
        {
            if (g_closingLap > 0)
                writeLap(g_closingLap, g_closingRows, g_closingCar, g_closingStart, lapMs);
            g_closingLap = 0;
            g_closingRows.clear();
        }

        // The open lap ends: kept until GT4 has its lap time (the line crossing updates it a frame or two later).
        void endLap(bool timed, uint32_t prevLast)
        {
            if (g_lap <= 0)
                return;
            flushClosing(0);
            if (timed)
            {
                g_closingLap = g_lap;
                g_closingRows.swap(g_rows);
                g_closingCar = g_car;
                g_closingStart = g_start;
                g_closingPrevLast = prevLast;
                g_closingWait = 0;
            }
            else
                writeLap(g_lap, g_rows, g_car, g_start, 0);
            g_rows.clear();
            g_lap = 0;
        }

        void endRace()
        {
            endLap(false, 0);
            flushClosing(0);
            if (g_race)
            {
                std::fclose(g_race);
                g_race = nullptr;
            }
            g_inRace = false;
            g_lastClock = ~0u;
        }
    } // namespace

    bool active()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        return g_active;
    }
    bool recording()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        return g_active && g_recording;
    }
    bool toggle()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        if (!g_active)
            return false;
        g_recording = !g_recording;
        if (!g_recording)
        {
            endLap(false, 0); // stopped part way: kept without a lap time
            flushClosing(0);
        }
        return g_recording;
    }
    int openLap()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        return g_lap;
    }
    uint32_t lastLapMs()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        return g_lastLapMs;
    }
    const std::string &where()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        return g_folder;
    }

    void onFrame(const gt4race::State &s)
    {
        std::lock_guard<std::mutex> lk(g_lock);
        init();
        if (!g_active)
            return;
        using gt4race::Phase;
        const bool live = s.phase == Phase::Starting || s.phase == Phase::Racing || s.phase == Phase::Finished;
        if (!live || (g_inRace && s.course != g_course))
        {
            if (g_inRace)
                endRace();
            if (!live)
                return;
        }
        if (!g_inRace)
        {
            g_inRace = true;
            g_course = s.course;
            g_stamp = stampNow();
            const std::string p = path("race");
            g_race = std::fopen(p.c_str(), "wb");
            if (g_race)
                gt4race::writeCsvHeader(g_race);
            // Beside it, the race's cars: who drove which (Race Lab's AI drivers, gt4_ai_drivers.h), for the launcher's Race Lab
            // tab. <...>-race-cars.txt: "gt4-race-cars 1", then per car "car <index>\t<you|ai<k>>\t<driver>\t<car name>".
            std::string q = p.substr(0, p.size() - 4) + "-cars.txt";
            if (std::FILE *f = std::fopen(q.c_str(), "wb"))
            {
                std::fprintf(f, "gt4-race-cars 1\r\n# course %u, %d laps\r\n", s.course, s.laps);
                for (int i = 0; i < s.carCount; ++i)
                {
                    const int k = gt4drivers::aiNumber(s, i);
                    std::fprintf(f, "car %d\t%s\t%s\t%s\r\n", i, s.cars[i].player ? "you" : ("ai" + std::to_string(k)).c_str(),
                                 s.cars[i].player ? "You" : gt4drivers::driverOf(s, i).c_str(), s.cars[i].name);
                }
                std::fclose(f);
            }
        }
        if (s.clockMs == g_lastClock)
            return; // (the race clock stands: the grid wait, or a frame shown twice)
        g_lastClock = s.clockMs;
        if (g_race)
            gt4race::writeCsvRows(g_race, s);
        if (s.player.index < 0)
            return;
        const gt4race::Car &c = s.cars[s.player.index];
        // A lap is c.lap while 1..laps; after the finish (laps + 1) nothing more. A grid start runs lap 0 from the green
        // (the clock moving from 0) to the line: that is lap 1's start, as GT4 times lap 1 from the green.
        const int lapNow = (c.lap == 0 && s.clockMs > 0) ? 1 : c.lap;
        if (g_recording && lapNow != g_lap)
        {
            const bool crossed = g_lap > 0 && lapNow == g_lap + 1;
            endLap(crossed, g_lapLastMsAtStart);
            if (lapNow >= 1 && (s.laps <= 0 || lapNow <= s.laps) && s.phase != Phase::Finished)
            {
                g_lap = lapNow;
                g_car = c.name;
                g_t0 = s.clockMs;
                g_lapLastMsAtStart = c.lastLapMs;
                g_start.clear();
                if (lapNow == 1)
                {
                    const bool grid = c.lap == 0;
                    g_start = grid ? "grid" : "rolling";
                    if (grid)
                        g_t0 = 0; // (GT4's lap 1 time runs from the green: clock 0)
                }
            }
        }
        // A finished lap waiting for its time: GT4's last lap time once it differs from the one at the lap's start (it
        // changes on the frame the lap number does, or a little later; give up after 2 s).
        if (g_closingLap > 0)
        {
            if (c.lastLapMs != g_closingPrevLast && c.lastLapMs != 0)
                flushClosing(c.lastLapMs);
            else if (++g_closingWait > 120)
                flushClosing(0);
        }
        if (!g_recording || g_lap <= 0)
            return;
        char row[256];
        std::snprintf(row, sizeof(row), "%u,%.2f,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.3f,%.3f,%.3f,,%.4f,%llu", s.clockMs - g_t0, c.lapDistance, c.x, c.z,
                      -c.y, c.speed, s.player.gear, s.player.rpm, s.player.throttle, s.player.brake, s.player.steer, c.yaw,
                      static_cast<unsigned long long>(s.vsync));
        g_rows.emplace_back(row);
    }
} // namespace gt4drivelog
