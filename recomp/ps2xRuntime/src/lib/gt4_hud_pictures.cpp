#include "gt4_hud_pictures.h"

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include "gt4_hud_move.h"
#include "gt4_race_reader.h"

#include "external/stb_image_write.h" // (raylib builds its implementation, the JPEG writer included)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// HUD hand-off 7 A1 (gt4_hud_pictures.h). Markers in GT4's GS stream: an A+D write to GS register 0x7f whose value's high
// word is 'HUDP' (0x48554450) and whose low byte is the code: 0xfe the frame's RaceDisplay draw begins, 0xfd it ends, 0xff
// no member, 0..kMembers-1 that member's draw begins.

namespace gt4hudpic
{
    std::atomic<bool> g_markingNow{false};
    namespace
    {
        // RaceDisplay's members (vtable 0x67e278; HUD-HANDOFF-3/5/6 reports) and the part each belongs to.
        struct Member
        {
            uint32_t off;
            const char *part; // nullptr: not one of the page's parts (GT4's messages)
        };
        constexpr Member kMembers[] = {
            {0x1398u, "position"},  // RaceRankDisplay
            {0x1668u, "position"},  // its "Position" label
            {0x1300u, "lap"},       // RaceLapDisplay
            {0x1684u, "lap"},       // its "Lap" label
            {0x13f0u, "besttimes"}, // RaceLapTimesDisplay
            {0x1438u, "besttimes"}, // RaceTimeDisplay (nothing drawn in a race)
            {0x14a8u, "besttimes"}, // RaceTimeDisplay
            {0x1518u, "besttimes"}, // RaceTimeDisplay
            {0x1588u, "besttimes"}, // RaceTimeDisplay
            {0x15f8u, "clock"},     // RaceTimeDisplay: the race clock, top centre (hud7-a1)
            {0x16a0u, "besttimes"}, // "Best Lap" label
            {0x16bcu, "besttimes"}, // "Last Lap" label
            {0x0108u, "panel"},     // RaceOnboardPanel (bumper, bonnet)
            {0x0d58u, "panel"},     // the compact panel (chase views)
            {0x1108u, "map"},       // RaceMiniMap
            {0x17bcu, nullptr},     // RaceEventDisplay
            {0x11b0u, nullptr},     // RaceMessageDisplay
            {0x228cu, nullptr},     // RaceMessageDisplay
        };
        constexpr int kMemberCount = static_cast<int>(sizeof(kMembers) / sizeof(kMembers[0]));
        constexpr const char *kParts[] = {"position", "lap", "clock", "besttimes", "map", "panel"};

        constexpr uint32_t kSig = 0x48554450u; // 'HUDP'
        constexpr uint32_t kBegin = 0xfeu, kEnd = 0xfdu, kNone = 0xffu;
        constexpr uint32_t kMoveFlag = 0x100u; // (a move-only frame's markers, gt4_hud_move.h: not a race picture's)

        struct Box
        {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            bool empty() const { return x1 < x0; }
            void add(float a0, float b0, float a1, float b1)
            {
                x0 = std::min(x0, a0), y0 = std::min(y0, b0), x1 = std::max(x1, a1), y1 = std::max(y1, b1);
            }
            void add(const Box &b)
            {
                if (!b.empty())
                    add(b.x0, b.y0, b.x1, b.y1);
            }
        };

        // ---- settings
        int afterSeconds()
        {
            static const int s = [] {
                const char *v = std::getenv("GT4_HUD_PICTURES_AFTER"); // (tests: seconds after racing begins; default 20)
                return v && *v ? std::clamp(std::atoi(v), 1, 600) : 20;
            }();
            return s;
        }
        int everySeconds()
        {
            static const int s = [] {
                const char *v = std::getenv("GT4_HUD_PICTURES_EVERY"); // (tests: another picture every n seconds in a race)
                return v && *v ? std::clamp(std::atoi(v), 0, 600) : 0;
            }();
            return s;
        }

        // ---- state shared between the threads
        std::atomic<bool> &g_marking = g_markingNow;
        std::atomic<bool> g_gsArmed{false}, g_vulkan{false}, g_hudShotWanted{false}, g_wantMirror{false}, g_retry{false};
        std::atomic<int> g_view{-1};
        std::atomic<float> g_gameW{0.0f}, g_gameH{0.0f};
        uint8_t *g_spr = nullptr;

        struct Capture
        {
            uint64_t id = 0;
            uint32_t course = 0;
            std::string view, stamp;
            bool mirror = false;
            int mx = 0, my = 0, mw = 0, mh = 0;
            // GS
            bool gsDone = false;
            uint32_t wantedTag = 0, wantedShownTag = 0;
            Box frame;
            Box member[kMemberCount];
            std::vector<Box> prims[kMemberCount];
            Box other;
            int otherCount = 0, markers = 0;
            // pictures
            bool cleanDone = false, hudDone = false, shownDone = false;
            std::vector<uint8_t> clean, hud, shown; // clean: the 3D buffer (the road under the mirror); shown: the shown buffer
            int cw = 0, ch = 0, hw = 0, hh = 0, sw = 0, sh = 0;  // after the copy, before GT4's HUD (GT4's colours)
            std::chrono::steady_clock::time_point started;
        };
        std::mutex g_mutex;
        std::condition_variable g_cv;
        Capture g_cap;       // the capture on its way (g_cap.id != 0)
        bool g_capOpen = false;
        std::once_flag g_writerStarted;

        // ---- EE thread's own
        enum class Ee
        {
            Idle,
            Armed,   // the next RaceDisplay draw is marked
            Marking, // inside it
            Waiting, // the GS, the presenter and the writer finish it
        };
        Ee g_ee = Ee::Idle;
        uint32_t g_rd = 0;
        int g_curMember = -1;
        uint64_t g_raceStart = 0, g_lastTaken = 0, g_armVsync = 0, g_lastRaceFrames = 0;
        uint32_t g_lastCourse = ~0u;
        bool g_taken = false;
        uint64_t g_nextId = 1;
        int g_mirrorX = 0, g_mirrorY = 0, g_mirrorW = 0, g_mirrorH = 0;
        uint64_t g_mirrorVsync = 0, g_vsync = 0;
        int g_markerFails = 0;
        bool g_moveMarking = false; // (GT4_HUD_MOVE: this frame's RaceDisplay draw bracketed for the moved parts)
        uint32_t g_moveCursorMin = ~0u, g_moveCursorMax = 0; // (GT4_HUD_MOVE_LOG: where the markers went)
        uint64_t g_moveFrames = 0;

        bool putMarker(uint8_t *rdram, uint32_t code)
        {
            if (!g_spr)
                return false;
            uint32_t cur = 0;
            std::memcpy(&cur, g_spr + 0x21f8u, 4);
            uint8_t *p = nullptr;
            if ((cur & 0xf0000000u) == 0x70000000u)
            {
                if ((cur & 0x3fffu) + 48u <= 0x4000u)
                    p = g_spr + (cur & 0x3fffu);
            }
            else if ((cur & 0x1fffffffu) + 48u <= 0x02000000u)
                p = rdram + (cur & 0x1fffffffu);
            if (!p || (cur & 15u) != 0u || cur == 0u)
            {
                if (g_markerFails++ < 4)
                    std::fprintf(stderr, "[gt4-hud-pic] GT4's packet cursor %08x is not usable: no marker\n", cur);
                return false;
            }
            const uint32_t w[4] = {0x10000002u, 0u, 0x11000000u, 0x50000002u}; // DMA CNT 2 qw, FLUSH, DIRECT 2
            const uint64_t tag[2] = {1ull | (1ull << 15) | (1ull << 60), 0xeull}; // NLOOP 1, EOP, PACKED, NREG 1, A+D
            const uint64_t ad[2] = {(static_cast<uint64_t>(kSig) << 32) | code, 0x7full};
            std::memcpy(p, w, 16);
            std::memcpy(p + 16, tag, 16);
            std::memcpy(p + 32, ad, 16);
            if (code & kMoveFlag)
                g_moveCursorMin = std::min(g_moveCursorMin, cur), g_moveCursorMax = std::max(g_moveCursorMax, cur);
            cur += 48u;
            std::memcpy(g_spr + 0x21f8u, &cur, 4);
            return true;
        }

        std::string exeDir()
        {
#if defined(_WIN32)
            char path[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string s = path;
            const size_t at = s.find_last_of("\\/");
            return at == std::string::npos ? std::string(".") : s.substr(0, at);
#else
            return ".";
#endif
        }
        std::string folder()
        {
            static const std::string d = [] {
                const char *v = std::getenv("GT4_HUD_PICTURES_DIR"); // (tests: another folder)
                return v && *v ? std::string(v) : exeDir() + "\\hud-pictures";
            }();
            return d;
        }

        void appendBytes(void *context, void *data, int size)
        {
            auto *out = static_cast<std::vector<uint8_t> *>(context);
            const auto *p = static_cast<const uint8_t *>(data);
            out->insert(out->end(), p, p + size);
        }
        // Bilinear scaling of RGBA rows (alpha set to 255).
        std::vector<uint8_t> scaled(const std::vector<uint8_t> &src, int sw, int sh, int dw, int dh)
        {
            std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4u);
            for (int y = 0; y < dh; ++y)
            {
                const float fy = std::clamp((y + 0.5f) * sh / dh - 0.5f, 0.0f, static_cast<float>(sh - 1));
                const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, sh - 1);
                const float ty = fy - y0;
                for (int x = 0; x < dw; ++x)
                {
                    const float fx = std::clamp((x + 0.5f) * sw / dw - 0.5f, 0.0f, static_cast<float>(sw - 1));
                    const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, sw - 1);
                    const float tx = fx - x0;
                    const uint8_t *a = &src[(static_cast<size_t>(y0) * sw + x0) * 4u], *b = &src[(static_cast<size_t>(y0) * sw + x1) * 4u];
                    const uint8_t *c = &src[(static_cast<size_t>(y1) * sw + x0) * 4u], *d = &src[(static_cast<size_t>(y1) * sw + x1) * 4u];
                    uint8_t *o = &out[(static_cast<size_t>(y) * dw + x) * 4u];
                    for (int k = 0; k < 3; ++k)
                    {
                        const float top = a[k] + (b[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
                        o[k] = static_cast<uint8_t>(std::clamp(top + (bot - top) * ty + 0.5f, 0.0f, 255.0f));
                    }
                    o[3] = 255;
                }
            }
            return out;
        }
        bool writeJpeg(const std::string &path, const std::vector<uint8_t> &rgba, int w, int h)
        {
            std::vector<uint8_t> jpg;
            if (!stbi_write_jpg_to_func(&appendBytes, &jpg, w, h, 4, rgba.data(), 85))
                return false;
            std::FILE *f = std::fopen(path.c_str(), "wb");
            if (!f)
                return false;
            const bool ok = std::fwrite(jpg.data(), 1, jpg.size(), f) == jpg.size();
            std::fclose(f);
            return ok;
        }

        // A part's separate pieces: its primitives' rectangles merged while they touch (within 6 frame pixels).
        std::vector<Box> pieces(const std::vector<Box> &prims)
        {
            std::vector<Box> c = prims;
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t i = 0; i < c.size() && !merged; ++i)
                    for (size_t j = i + 1; j < c.size(); ++j)
                        if (c[i].x0 - 6.0f <= c[j].x1 && c[j].x0 - 6.0f <= c[i].x1 && c[i].y0 - 6.0f <= c[j].y1 && c[j].y0 - 6.0f <= c[i].y1)
                        {
                            c[i].add(c[j]);
                            c.erase(c.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
            }
            // (tiny specks left out: under 2 x 2 frame pixels)
            c.erase(std::remove_if(c.begin(), c.end(), [](const Box &b) { return b.x1 - b.x0 < 2.0f && b.y1 - b.y0 < 2.0f; }), c.end());
            std::sort(c.begin(), c.end(), [](const Box &a, const Box &b) { return a.x0 < b.x0; });
            return c;
        }

        void writeSet(Capture &c)
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string dir = folder();
            fs::create_directories(dir, ec);
            const std::string base = dir + "\\" + c.stamp + "-" + std::to_string(c.course);
            // The picture's size: the game rectangle's shape, at most 1920 wide.
            float gw = g_gameW.load(), gh = g_gameH.load();
            if (gw < 16.0f || gh < 16.0f)
                gw = static_cast<float>(c.hw), gh = static_cast<float>(c.hh);
            if (gw < 16.0f || gh < 16.0f)
                gw = 16.0f * 60.0f, gh = 9.0f * 60.0f;
            const int W = std::min(1920, static_cast<int>(std::lround(gw))), H = std::max(1, static_cast<int>(std::lround(W * gh / gw)));
            // The clean picture: the shown buffer after the copy (GT4's own colours), the mirror's rectangle from the 3D buffer's
            // road under it, brought to the shown buffer's colours (a gain per channel measured on a band around the mirror).
            std::vector<uint8_t> clean = c.shownDone ? c.shown : c.clean;
            int cw = c.shownDone ? c.sw : c.cw, ch = c.shownDone ? c.sh : c.ch;
            if (c.shownDone && c.cleanDone && c.mirror && c.cw > 0 && c.ch > 0)
            {
                const float kx = cw / 640.0f, ky = ch / 448.0f, ux = c.cw / 640.0f, uy = c.ch / 448.0f;
                auto at = [](const std::vector<uint8_t> &px, int w, int h, int x, int y) {
                    x = std::clamp(x, 0, w - 1), y = std::clamp(y, 0, h - 1);
                    return &px[(static_cast<size_t>(y) * w + x) * 4u];
                };
                double sumShown[3] = {}, sumScene[3] = {};
                const int band = 6;
                for (int y = c.my - band; y < c.my + c.mh + band; ++y)
                    for (int x = c.mx - band; x < c.mx + c.mw + band; ++x)
                    {
                        if (x >= c.mx && x < c.mx + c.mw && y >= c.my && y < c.my + c.mh)
                            continue;
                        const uint8_t *a = at(clean, cw, ch, static_cast<int>((x + 0.5f) * kx), static_cast<int>((y + 0.5f) * ky));
                        const uint8_t *b = at(c.clean, c.cw, c.ch, static_cast<int>((x + 0.5f) * ux), static_cast<int>((y + 0.5f) * uy));
                        for (int k = 0; k < 3; ++k)
                            sumShown[k] += a[k], sumScene[k] += b[k];
                    }
                double gain[3];
                for (int k = 0; k < 3; ++k)
                    gain[k] = sumScene[k] > 1.0 ? std::clamp(sumShown[k] / sumScene[k], 0.25, 4.0) : 1.0;
                const int x0 = static_cast<int>(c.mx * kx), y0 = static_cast<int>(c.my * ky), x1 = static_cast<int>((c.mx + c.mw) * kx), y1 = static_cast<int>((c.my + c.mh) * ky);
                for (int y = std::max(0, y0); y < std::min(ch, y1); ++y)
                    for (int x = std::max(0, x0); x < std::min(cw, x1); ++x)
                    {
                        const uint8_t *b = at(c.clean, c.cw, c.ch, static_cast<int>((x + 0.5f) / kx * ux), static_cast<int>((y + 0.5f) / ky * uy));
                        uint8_t *o = &clean[(static_cast<size_t>(y) * cw + x) * 4u];
                        for (int k = 0; k < 3; ++k)
                            o[k] = static_cast<uint8_t>(std::clamp(b[k] * gain[k] + 0.5, 0.0, 255.0));
                    }
            }
            const bool okClean = !clean.empty() && writeJpeg(base + ".jpg", scaled(clean, cw, ch, W, H), W, H);
            const bool okHud = c.hudDone && writeJpeg(base + "-hud.jpg", scaled(c.hud, c.hw, c.hh, W, H), W, H);
            // The rectangles, in fractions of the picture (the frame: the copy's rectangle in the shown buffer).
            const Box fr = c.frame.empty() ? Box{0, 0, 640, 448} : c.frame;
            const float fw = std::max(1.0f, fr.x1 - fr.x0), fh = std::max(1.0f, fr.y1 - fr.y0);
            auto rect = [&](const Box &b) {
                char t[96];
                std::snprintf(t, sizeof(t), "[%.5f,%.5f,%.5f,%.5f]", (b.x0 - fr.x0) / fw, (b.y0 - fr.y0) / fh, (b.x1 - b.x0) / fw, (b.y1 - b.y0) / fh);
                return std::string(t);
            };
            std::string parts, members, piecesJson, said, shown;
            // GT4_HUD_MOVE (gt4_hud_move.h): a moved part's rectangle is GT4's place as the frontend learned it, and where the
            // -hud picture has it now goes to "shown" (the launcher cuts it from there). The chase views' compact panel's four
            // pieces are also parts of their own: fuel, warnings, gbar, gearspeed (left to right).
            std::vector<gt4hudmove::PartPlace> places;
            bool movePieces = false;
            gt4hudmove::places(places, movePieces);
            auto placeOf = [&](const char *part) -> const gt4hudmove::PartPlace * {
                for (const auto &pl : places)
                    if (pl.part == part)
                        return &pl;
                return nullptr;
            };
            auto boxOf = [](float x0, float y0, float x1, float y1) {
                Box b;
                b.add(x0, y0, x1, y1);
                return b;
            };
            auto addShown = [&](const char *part, const Box &b) { shown += (shown.empty() ? "" : ", ") + std::string("\"") + part + "\": " + rect(b); };
            static constexpr const char *kPieceNames[4] = {"fuel", "warnings", "gbar", "gearspeed"};
            std::string pieceParts;
            for (const char *part : kParts)
            {
                Box b;
                std::vector<Box> prims;
                for (int m = 0; m < kMemberCount; ++m)
                    if (kMembers[m].part && std::strcmp(kMembers[m].part, part) == 0)
                    {
                        b.add(c.member[m]);
                        prims.insert(prims.end(), c.prims[m].begin(), c.prims[m].end());
                    }
                if (b.empty())
                    continue;
                std::vector<Box> pc = pieces(prims);
                const bool panel = std::strcmp(part, "panel") == 0;
                if (panel && movePieces)
                {
                    // (the pieces at GT4's places: some may be drawn elsewhere now)
                    pc.clear();
                    b = Box{};
                    for (int k = 0; k < 4; ++k)
                        if (const auto *pl = placeOf(kPieceNames[k]))
                        {
                            pc.push_back(boxOf(pl->x0, pl->y0, pl->x1, pl->y1));
                            b.add(pc.back());
                            if (pl->moved)
                                addShown(kPieceNames[k], boxOf(pl->mx0, pl->my0, pl->mx1, pl->my1));
                        }
                }
                else if (const auto *pl = placeOf(part); pl && pl->moved)
                {
                    b = boxOf(pl->x0, pl->y0, pl->x1, pl->y1);
                    addShown(part, boxOf(pl->mx0, pl->my0, pl->mx1, pl->my1));
                    pc.clear();
                }
                parts += (parts.empty() ? "" : ", ") + std::string("\"") + part + "\": " + rect(b);
                if (pc.size() > 1u)
                {
                    std::string list;
                    for (const Box &p : pc)
                        list += (list.empty() ? "" : ", ") + rect(p);
                    piecesJson += (piecesJson.empty() ? "" : ", ") + std::string("\"") + part + "\": [" + list + "]";
                }
                if (panel && pc.size() == 4u && (c.view == "chase" || c.view == "far"))
                    for (int k = 0; k < 4; ++k)
                        pieceParts += std::string(", \"") + kPieceNames[k] + "\": " + rect(pc[static_cast<size_t>(k)]);
                said += std::string(said.empty() ? "" : ", ") + part + (pc.size() > 1u ? "(" + std::to_string(pc.size()) + " pieces)" : "");
            }
            parts += pieceParts;
            if (c.mirror)
            {
                char t[128];
                std::snprintf(t, sizeof(t), "\"mirror\": [%.5f,%.5f,%.5f,%.5f]", c.mx / 640.0f, c.my / 448.0f, c.mw / 640.0f, c.mh / 448.0f);
                parts += (parts.empty() ? "" : ", ") + std::string(t);
                said += std::string(said.empty() ? "" : ", ") + "mirror";
                float x0 = static_cast<float>(c.mx), y0 = static_cast<float>(c.my), x1 = static_cast<float>(c.mx + c.mw), y1 = static_cast<float>(c.my + c.mh);
                if (gt4hudmove::mirrorMoved() && gt4hudmove::movedRect("mirror", x0, y0, x1, y1))
                    addShown("mirror", boxOf(x0, y0, x1, y1));
            }
            if (!shown.empty())
                said += "; " + std::to_string(std::count(shown.begin(), shown.end(), '[')) + " moved";
            for (int m = 0; m < kMemberCount; ++m)
                if (!c.member[m].empty())
                {
                    char k[16];
                    std::snprintf(k, sizeof(k), "\"%x\": ", kMembers[m].off);
                    members += (members.empty() ? "" : ", ") + std::string(k) + rect(c.member[m]);
                }
            std::string json = "{\n  \"course\": " + std::to_string(c.course) + ",\n  \"view\": \"" + c.view + "\",\n  \"w\": " +
                               std::to_string(static_cast<int>(std::lround(gw))) + ",\n  \"h\": " + std::to_string(static_cast<int>(std::lround(gh))) +
                               ",\n  \"parts\": {" + parts + "},\n  \"shown\": {" + shown + "},\n  \"pieces\": {" + piecesJson + "},\n  \"members\": {" + members + "}";
            if (!c.other.empty())
                json += ",\n  \"other\": " + rect(c.other) + ",\n  \"other_count\": " + std::to_string(c.otherCount);
            json += "\n}\n";
            if (std::FILE *f = std::fopen((base + ".json").c_str(), "wb"))
            {
                std::fwrite(json.data(), 1, json.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[gt4-hud-pic] saved %s (.jpg %s from the %s, -hud.jpg %s; %dx%d; view %s; %d markers; parts: %s)\n", base.c_str(),
                         okClean ? "ok" : "FAILED", c.shownDone ? "shown buffer" : "3D buffer", okHud ? "ok" : "FAILED", W, H, c.view.c_str(), c.markers,
                         said.empty() ? "none" : said.c_str());
            // The newest 16 sets kept (by name: the time stamp first).
            std::vector<std::string> sets;
            for (const auto &e : fs::directory_iterator(dir, ec))
                if (e.path().extension() == ".json")
                    sets.push_back(e.path().stem().string());
            std::sort(sets.begin(), sets.end());
            for (size_t i = 0; i + 16u < sets.size(); ++i)
                for (const char *ext : {".json", ".jpg", "-hud.jpg"})
                    fs::remove(dir + "\\" + sets[i] + ext, ec);
        }

        void writerLoop()
        {
            for (;;)
            {
                Capture c;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    auto done = [] {
                        return g_capOpen && g_cap.gsDone && g_cap.cleanDone && g_cap.hudDone && (g_cap.wantedShownTag == 0u || g_cap.shownDone);
                    };
                    g_cv.wait_for(lock, std::chrono::milliseconds(250), done);
                    if (!g_capOpen)
                        continue;
                    const bool ready = done();
                    const bool late = std::chrono::steady_clock::now() - g_cap.started > std::chrono::seconds(6);
                    if (!ready && !late)
                        continue;
                    c = std::move(g_cap);
                    g_cap = Capture{};
                    g_capOpen = false;
                    g_gsArmed.store(false);
                    // (a frame where GT4 drew under three of its parts, a view change: taken again on the next frame)
                    int parts = 0;
                    for (const char *part : kParts)
                    {
                        bool any = false;
                        for (int m = 0; m < kMemberCount; ++m)
                            any = any || (kMembers[m].part && std::strcmp(kMembers[m].part, part) == 0 && !c.member[m].empty());
                        parts += any;
                    }
                    // The parts GT4_HUD_HIDE leaves on screen: with them hidden GT4 draws fewer, and that frame is fine (the developer's
                    // TB39 hid all six: every frame was taken again, and each retake skips our widgets for a frame, so the HUD
                    // flickered). At most 10 retakes a race.
                    static const int expected = [] {
                        std::string hide = std::getenv("GT4_HUD_HIDE") ? std::getenv("GT4_HUD_HIDE") : "";
                        auto hidden = [&](const char *k) { return ("," + hide + ",").find(std::string(",") + k + ",") != std::string::npos; };
                        int n = 0;
                        for (const char *part : kParts)
                        {
                            const bool times = std::strcmp(part, "clock") == 0 || std::strcmp(part, "besttimes") == 0;
                            n += !(hidden(part) || (times && hidden("times")));
                        }
                        return n;
                    }();
                    static int retakes = 0;
                    if (ready && parts < std::min(3, expected) && retakes < 10)
                    {
                        ++retakes;
                        std::fprintf(stderr, "[gt4-hud-pic] GT4 drew %d of its parts in that frame (a view change?): taken again\n", parts);
                        g_retry.store(true);
                        continue;
                    }
                    retakes = 0;
                    if (!ready)
                    {
                        std::fprintf(stderr, "[gt4-hud-pic] race picture not finished in 6 s (rectangles %s, picture %s, HUD picture %s): none saved\n",
                                     c.gsDone ? "yes" : "no", c.cleanDone ? "yes" : "no", c.hudDone ? "yes" : "no");
                        continue;
                    }
                }
                writeSet(c);
            }
        }

        // GS thread's own
        bool g_inFrame = false;
        int g_gsCur = -1;
        uint32_t g_latestTag = 0, g_tagSerial = 0, g_copyFbp = ~0u, g_shownFbp = ~0u, g_shownTag = 0;
        Box g_copyBox, g_frameBox;
        Box g_gsMember[kMemberCount];
        std::vector<Box> g_gsPrims[kMemberCount];
        Box g_gsOther;
        int g_gsOtherCount = 0, g_gsMarkers = 0;
        struct Read
        {
            uint32_t tag = 0;
            std::vector<uint8_t> px;
            int w = 0, h = 0;
        };
        std::vector<Read> g_reads; // (recent reads while the wanted one is not known yet)
    } // namespace

    bool enabled()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_HUD_PICTURES");
            const bool off = v && (std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0);
            if (off)
                std::fprintf(stderr, "[gt4-hud-pic] GT4_HUD_PICTURES=off: no race pictures for the launcher\n");
            return !off;
        }();
        return on;
    }

    void setScratchpad(uint8_t *spr) { g_spr = spr; }
    bool wantMirror() { return g_wantMirror.load(std::memory_order_relaxed); }
    void noteView(int view) { g_view.store(view, std::memory_order_relaxed); }
    void noteMirror(int x, int y, int w, int h)
    {
        g_mirrorX = x, g_mirrorY = y, g_mirrorW = w, g_mirrorH = h;
        g_mirrorVsync = g_vsync ? g_vsync : 1u;
    }

    namespace
    {
        void captureStep(uint8_t *rdram, uint32_t rd, uint64_t vsync);
    }

    uint32_t memberOffset(int index) { return index >= 0 && index < kMemberCount ? kMembers[index].off : 0u; }

    std::atomic<int> g_pendingNow{0};
    namespace
    {
        struct Pending
        {
            int token;
            uint32_t ret, sp;
        };
        std::vector<Pending> g_pending; // (EE thread: innermost last)
        uint64_t g_suspends = 0, g_closedLate = 0, g_forgotten = 0;
        bool deferOn()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT4_HUD_MOVE_DEFER");
                return !(v && *v == '0');
            }();
            return on;
        }
        bool logOn()
        {
            static const bool on = [] {
                const char *v = std::getenv("GT4_HUD_MOVE_LOG");
                return v && *v == '1';
            }();
            return on;
        }
    } // namespace

    void raceDisplayDraw(uint8_t *rdram, uint32_t rd, uint64_t vsync)
    {
        g_vsync = vsync;
        // (a bracketed call still waiting for its return from the last frame: never came back that way; forgotten)
        if (!g_pending.empty())
        {
            static int said = 0;
            if (logOn() && said++ < 8)
                std::fprintf(stderr, "[gt4-hud-pic] vsync %llu: %zu bracketed call(s) of the last frame never returned past the scheduler: forgotten\n",
                             static_cast<unsigned long long>(vsync), g_pending.size());
            g_forgotten += g_pending.size();
            g_pending.clear();
            g_pendingNow.store(0, std::memory_order_relaxed);
        }
        g_moveMarking = false;
        captureStep(rdram, rd, vsync);
        // GT4_HUD_MOVE (gt4_hud_move.h): every other race frame's RaceDisplay draw bracketed for the moved parts (a race
        // picture's frame has all its members bracketed already).
        if (g_ee != Ee::Marking && gt4hudmove::on())
        {
            g_rd = rd & 0x1fffffffu;
            g_curMember = -1;
            if (putMarker(rdram, kBegin | kMoveFlag))
            {
                g_moveMarking = true;
                g_marking.store(true);
                static const bool log = [] {
                    const char *v = std::getenv("GT4_HUD_MOVE_LOG");
                    return v && *v == '1';
                }();
                if (log && (++g_moveFrames == 1u || g_moveFrames % 1800u == 0u))
                    std::fprintf(stderr, "[gt4-hud-move] frame %llu: markers in GT4's packet chain between %08x and %08x\n",
                                 static_cast<unsigned long long>(g_moveFrames), g_moveCursorMin, g_moveCursorMax);
            }
        }
    }

    namespace
    {
    void captureStep(uint8_t *rdram, uint32_t rd, uint64_t vsync)
    {
        if (!enabled() || !g_vulkan.load(std::memory_order_relaxed))
            return;
        gt4race::State s;
        if (!gt4race::snapshot(s))
            return;
        // A new race: the race frames went back, another course, or the pre-race screen.
        if (s.raceFrames < g_lastRaceFrames || s.course != g_lastCourse || s.phase == gt4race::Phase::PreRace || s.phase == gt4race::Phase::None)
            g_raceStart = 0, g_taken = false;
        g_lastRaceFrames = s.raceFrames, g_lastCourse = s.course;
        const bool racing = s.phase == gt4race::Phase::Racing && s.player.index >= 0 && gt4race::msSinceLastFrame() < 500u;
        if (racing && g_raceStart == 0)
            g_raceStart = vsync;
        switch (g_ee)
        {
        case Ee::Idle:
        {
            if (!racing || g_raceStart == 0)
                return;
            const bool first = (!g_taken && vsync >= g_raceStart + static_cast<uint64_t>(afterSeconds()) * 60u) || g_retry.exchange(false);
            const bool again = g_taken && everySeconds() > 0 && vsync >= g_lastTaken + static_cast<uint64_t>(everySeconds()) * 60u;
            if (!first && !again)
                return;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_capOpen)
                    return; // (the last one is still being written)
            }
            g_ee = Ee::Armed;
            g_armVsync = vsync;
            g_wantMirror.store(true);
            g_gsArmed.store(true);
            return;
        }
        case Ee::Armed:
        {
            if (!racing)
            {
                g_ee = Ee::Idle;
                g_gsArmed.store(false);
                g_wantMirror.store(false);
                return;
            }
            Capture c;
            c.id = g_nextId++;
            c.course = s.course;
            const int v = g_view.load();
            static const char *const kView[] = {"bumper", "bonnet", "bonnet", "chase", "chase", "far"};
            if (v >= 0 && v < 6)
                c.view = kView[v];
            else
            {
                // Without the race cameras: the view from GT4's panel (gt4hud), chase or bumper.
                const uint32_t r = rd & 0x1fffffffu;
                c.view = r + 0xd60u < 0x02000000u && rdram[r + 0xd58u] != 0u ? "chase" : "bumper";
            }
            if (g_mirrorVsync && vsync - g_mirrorVsync <= 2u)
                c.mirror = true, c.mx = g_mirrorX, c.my = g_mirrorY, c.mw = g_mirrorW, c.mh = g_mirrorH;
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            c.stamp = stamp;
            c.started = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_cap = std::move(c);
                g_capOpen = true;
            }
            std::call_once(g_writerStarted, [] { std::thread(writerLoop).detach(); });
            g_rd = rd & 0x1fffffffu;
            g_curMember = -1;
            if (!putMarker(rdram, kBegin))
            {
                g_ee = Ee::Waiting; // (the writer gives up on it)
                return;
            }
            g_ee = Ee::Marking;
            g_marking.store(true);
            g_taken = true;
            g_lastTaken = vsync;
            return;
        }
        case Ee::Marking:
            return;
        case Ee::Waiting:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_capOpen)
                g_ee = Ee::Idle;
            return;
        }
        }
    }
    } // namespace

    int callEnter(uint8_t *rdram, uint32_t target, uint32_t a0)
    {
        const bool moveOnly = g_moveMarking && g_ee != Ee::Marking;
        if (g_ee != Ee::Marking && !moveOnly)
            return -1;
        if (target == 0x39f498u)
            return 1000; // RaceDisplay's draw itself: its end ends the marking
        if (g_curMember >= 0)
            return -1;
        const uint32_t a = a0 & 0x1fffffffu;
        if (a < g_rd)
            return -1;
        for (int m = 0; m < kMemberCount; ++m)
            if (a == g_rd + kMembers[m].off)
            {
                // (a move-only frame: only the moved parts' members)
                if (moveOnly && !gt4hudmove::wantsMember(kMembers[m].off))
                    return -1;
                if (!putMarker(rdram, static_cast<uint32_t>(m) | (moveOnly ? kMoveFlag : 0u)))
                    return -1;
                g_curMember = m;
                return m;
            }
        return -1;
    }

    void callSuspended(uint8_t *rdram, int token, uint32_t ret, uint32_t sp, bool threw)
    {
        ++g_suspends;
        if (logOn() && (g_suspends <= 12u || g_suspends % 500u == 0u))
            std::fprintf(stderr, "[gt4-hud-pic] vsync %llu: the bracketed %s %s (%llu so far; %llu closed at their return, %llu never)%s\n",
                         static_cast<unsigned long long>(g_vsync), token == 1000 ? "RaceDisplay draw" : "member draw",
                         threw ? "was left by a blocking wait" : "yielded at a scheduler checkpoint", static_cast<unsigned long long>(g_suspends),
                         static_cast<unsigned long long>(g_closedLate), static_cast<unsigned long long>(g_forgotten), deferOn() ? ": closed at its return" : ": closed now (GT4_HUD_MOVE_DEFER=0)");
        if (!deferOn())
        {
            callLeave(rdram, token);
            return;
        }
        g_pending.push_back(Pending{token, ret & 0x1fffffffu, sp});
        g_pendingNow.store(static_cast<int>(g_pending.size()), std::memory_order_relaxed);
    }

    void atDispatch(uint8_t *rdram, uint32_t pc, uint32_t sp)
    {
        // (the calls are pushed innermost first, as the yield unwinds them: any of them may be the one returning)
        for (size_t i = 0; i < g_pending.size(); ++i)
            if (g_pending[i].ret == (pc & 0x1fffffffu) && g_pending[i].sp == sp)
            {
                const int token = g_pending[i].token;
                g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
                g_pendingNow.store(static_cast<int>(g_pending.size()), std::memory_order_relaxed);
                ++g_closedLate;
                callLeave(rdram, token);
                return;
            }
    }

    void callLeave(uint8_t *rdram, int token)
    {
        if (token == 1000)
        {
            if (g_moveMarking && g_ee != Ee::Marking)
            {
                if (g_curMember >= 0)
                    putMarker(rdram, kNone | kMoveFlag);
                putMarker(rdram, kEnd | kMoveFlag);
                g_curMember = -1;
                g_moveMarking = false;
                g_marking.store(false);
                return;
            }
            if (g_curMember >= 0)
                putMarker(rdram, kNone);
            putMarker(rdram, kEnd);
            g_curMember = -1;
            g_marking.store(false);
            g_wantMirror.store(false);
            g_ee = Ee::Waiting;
            return;
        }
        if (token >= 0 && token == g_curMember)
        {
            putMarker(rdram, kNone | (g_moveMarking && g_ee != Ee::Marking ? kMoveFlag : 0u));
            g_curMember = -1;
        }
    }

    // ---- GS backend thread
    void vulkanSeen()
    {
        if (!g_vulkan.exchange(true) && enabled())
            std::fprintf(stderr, "[gt4-hud-pic] race pictures for the launcher's HUD layout page: on (%s, %d s into each race)\n", folder().c_str(),
                         afterSeconds());
    }
    bool gsArmed() { return g_gsArmed.load(std::memory_order_relaxed); }
    bool gsBusy()
    {
        if (g_gsArmed.load(std::memory_order_relaxed))
            return true;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_capOpen && (!g_cap.cleanDone || (g_cap.wantedShownTag != 0u && !g_cap.shownDone));
    }
    uint32_t sceneReadTag()
    {
        g_latestTag = 0x48500000u | (++g_tagSerial & 0xffffu);
        return g_latestTag;
    }

    uint32_t shownReadTag(uint32_t &fbp, float &x0, float &y0, float &x1, float &y1)
    {
        if (!g_inFrame || g_frameBox.empty() || g_shownFbp == ~0u)
            return 0u;
        fbp = g_shownFbp;
        x0 = g_frameBox.x0, y0 = g_frameBox.y0, x1 = g_frameBox.x1, y1 = g_frameBox.y1;
        g_shownTag = 0x48510000u | (++g_tagSerial & 0xffffu);
        return g_shownTag;
    }

    void shownReadDropped() { g_shownTag = 0; }

    void sceneRead(uint32_t tag, std::vector<uint8_t> &&pixels, int w, int h)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((tag >> 16) == 0x4851u)
        {
            // (the shown buffer: its tag is known when it is read)
            if (g_capOpen && !g_cap.shownDone && tag == g_shownTag)
            {
                g_cap.shown = std::move(pixels);
                g_cap.sw = w, g_cap.sh = h;
                g_cap.shownDone = true;
                g_cv.notify_all();
            }
            return;
        }
        if (!g_capOpen || g_cap.cleanDone)
            return;
        if (g_cap.wantedTag == 0u)
        {
            // (not known yet: kept, the newest two)
            g_reads.push_back(Read{tag, std::move(pixels), w, h});
            if (g_reads.size() > 2u)
                g_reads.erase(g_reads.begin());
            return;
        }
        if (tag != g_cap.wantedTag)
            return;
        g_cap.clean = std::move(pixels);
        g_cap.cw = w, g_cap.ch = h;
        g_cap.cleanDone = true;
        g_cv.notify_all();
    }

    void gsMarker(uint32_t value)
    {
        if (value & kMoveFlag)
            return; // (GT4_HUD_MOVE's own frames: gt4_hud_move.cpp)
        const uint32_t code = value & 0xffu;
        ++g_gsMarkers;
        if (code == kBegin)
        {
            g_inFrame = true;
            g_gsCur = -1;
            g_frameBox = g_copyBox; // (this frame's copy of the 3D into the shown buffer, before the HUD)
            g_shownFbp = g_copyFbp;
            g_copyBox = Box{};
            g_shownTag = 0;
            for (int m = 0; m < kMemberCount; ++m)
                g_gsMember[m] = Box{}, g_gsPrims[m].clear();
            g_gsOther = Box{};
            g_gsOtherCount = 0;
            g_gsMarkers = 1;
            return;
        }
        if (code == kEnd)
        {
            g_inFrame = false;
            g_gsCur = -1;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_capOpen && !g_cap.gsDone)
                {
                    g_cap.gsDone = true;
                    g_cap.wantedTag = g_latestTag;
                    g_cap.wantedShownTag = g_shownTag;
                    g_cap.frame = g_frameBox;
                    for (int m = 0; m < kMemberCount; ++m)
                        g_cap.member[m] = g_gsMember[m], g_cap.prims[m] = g_gsPrims[m];
                    g_cap.other = g_gsOther;
                    g_cap.otherCount = g_gsOtherCount;
                    g_cap.markers = g_gsMarkers;
                    for (Read &r : g_reads)
                        if (r.tag == g_cap.wantedTag && !g_cap.cleanDone)
                        {
                            g_cap.clean = std::move(r.px);
                            g_cap.cw = r.w, g_cap.ch = r.h;
                            g_cap.cleanDone = true;
                        }
                    g_reads.clear();
                    g_cv.notify_all();
                }
            }
            g_gsArmed.store(false);
            g_hudShotWanted.store(true);
            return;
        }
        if (code == kNone)
        {
            g_gsCur = -1;
            return;
        }
        if (code < static_cast<uint32_t>(kMemberCount))
            g_gsCur = static_cast<int>(code);
    }

    void gsPrimitive(uint32_t fbp, float x0, float y0, float x1, float y1)
    {
        if (!g_inFrame || fbp != g_shownFbp || x1 < x0 || y1 < y0)
            return;
        if (g_gsCur >= 0)
        {
            g_gsMember[g_gsCur].add(x0, y0, x1, y1);
            if (g_gsPrims[g_gsCur].size() < 512u)
            {
                Box b;
                b.add(x0, y0, x1, y1);
                g_gsPrims[g_gsCur].push_back(b);
            }
        }
        else
            g_gsOther.add(x0, y0, x1, y1), ++g_gsOtherCount;
    }

    void gsCopy(uint32_t fbp, float x0, float y0, float x1, float y1)
    {
        if (fbp != g_copyFbp)
            g_copyBox = Box{};
        g_copyFbp = fbp;
        g_copyBox.add(x0, y0, x1, y1);
    }

    // ---- presenter
    void noteGameRect(float w, float h)
    {
        g_gameW.store(w, std::memory_order_relaxed);
        g_gameH.store(h, std::memory_order_relaxed);
    }
    bool hudShotTurn() { return g_hudShotWanted.load(std::memory_order_relaxed) && g_hudShotWanted.exchange(false); }
    void hudShot(const uint8_t *rgba, int sw, int sh, float x, float y, float w, float h)
    {
        const int x0 = std::clamp(static_cast<int>(std::lround(x)), 0, sw), y0 = std::clamp(static_cast<int>(std::lround(y)), 0, sh);
        const int x1 = std::clamp(static_cast<int>(std::lround(x + w)), 0, sw), y1 = std::clamp(static_cast<int>(std::lround(y + h)), 0, sh);
        if (!rgba || x1 - x0 < 16 || y1 - y0 < 16)
            return;
        std::vector<uint8_t> crop(static_cast<size_t>(x1 - x0) * (y1 - y0) * 4u);
        for (int yy = y0; yy < y1; ++yy)
            std::memcpy(&crop[static_cast<size_t>(yy - y0) * (x1 - x0) * 4u], rgba + (static_cast<size_t>(yy) * sw + x0) * 4u, static_cast<size_t>(x1 - x0) * 4u);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_capOpen || g_cap.hudDone)
            return;
        g_cap.hud = std::move(crop);
        g_cap.hw = x1 - x0, g_cap.hh = y1 - y0;
        g_cap.hudDone = true;
        g_cv.notify_all();
    }
} // namespace gt4hudpic

#else

namespace gt4hudpic
{
    bool enabled() { return false; }
    void setScratchpad(uint8_t *) {}
    std::atomic<bool> g_markingNow{false};
    int callEnter(uint8_t *, uint32_t, uint32_t) { return -1; }
    void callLeave(uint8_t *, int) {}
    std::atomic<int> g_pendingNow{0};
    void callSuspended(uint8_t *, int, uint32_t, uint32_t, bool) {}
    void atDispatch(uint8_t *, uint32_t, uint32_t) {}
    void raceDisplayDraw(uint8_t *, uint32_t, uint64_t) {}
    void noteView(int) {}
    void noteMirror(int, int, int, int) {}
    bool wantMirror() { return false; }
    void vulkanSeen() {}
    bool gsArmed() { return false; }
    bool gsBusy() { return false; }
    uint32_t sceneReadTag() { return 0; }
    uint32_t shownReadTag(uint32_t &, float &, float &, float &, float &) { return 0; }
    void shownReadDropped() {}
    void sceneRead(uint32_t, std::vector<uint8_t> &&, int, int) {}
    void gsMarker(uint32_t) {}
    void gsPrimitive(uint32_t, float, float, float, float) {}
    void gsCopy(uint32_t, float, float, float, float) {}
    void noteGameRect(float, float) {}
    bool hudShotTurn() { return false; }
    void hudShot(const uint8_t *, int, int, float, float, float, float) {}
} // namespace gt4hudpic

#endif
