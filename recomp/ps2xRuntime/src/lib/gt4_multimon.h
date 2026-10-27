#pragma once
// GT4's own Multi Monitor Mode settings as runtime switches (analysis/design/multi-monitor; development and Full
// GT4.exe only: the Classic build compiles none of it). GT4 keeps them in its Options (Options > LAN pop-up, and the
// hidden Detail Monitor Settings page); these keys give one GT4 copy its screen without the menus and without
// changing the player's save. Keys (absent: GT4's own options):
//   GT4_MM_POSITION=center|left|right|top-center|top-left|top-right   Monitor Position (LAN_monitor_position_x/y)
//   GT4_MM_STYLE=play|live|monitor     Style (LAN_style)
//   GT4_MM_MONITORS=<0-5>              No. of Monitors (LAN_monitor_num)
//   GT4_MM_PLAYERS=<1-6>               No. of Players (LAN_player_num)
//   GT4_MM_NAME=<text>                 Console Name (LAN_machine_name, up to 15 characters)
//   GT4_MM_TARGET=<text>               Replay Target (LAN_target_name / LAN_live_target)
//   GT4_MM_HOST=0|1                    Host (LAN_host)
//   GT4_MM_AUTO_CONNECT=0|1            LAN_auto_connect (GT4's show-rig start: the network page connects by itself)
//   GT4_MM_AUTO_INDEX=<n>              LAN_auto_machine_idx (the console number of GT4's automatic network setting)
//   GT4_MM_GEOMETRY=<flag>,<zoom>,<distance>,<width>,<height>,<aspect>,<scale>,<border left>,<border right>,
//                   <border top>,<border bottom>   the Detail Monitor Settings (physical_monitor_size, mm), e.g. from
//                   gt4_screens.py; flag bit 0 = side screens angled, bit 1 = rows angled
//   GT4_MM_PAD=share|follow|off        the copies' controller 1: share = this copy (the one you drive) offers what it
//                                      reads to the other copies of the group; follow = a side copy takes it until its
//                                      network is up (title, Arcade, Multi-LAN Race: all screens go along), then
//                                      nothing; off = never any host input (gt4_multimon_pad.cpp)
//   GT4_MM_GROUP=<name>                the copies that belong together (default gt4)
//   GT4_MM_TRACE=1                     (diagnostics) log the LAN path's script calls and what was applied, and the race
//                                      frame number every 60 race frames (to pick the same moment on every screen)
// Where GT4 keeps them [C, CORE]: the Option object (context word 0x622f4c + 0x38cb0); its LAN block (accessors
// 0x187750 name +0x1348 (64 bytes), 0x187940 style +0x1388 (0x438cb8 / 0x438cf8), 0x187b38 target +0x138c (64 bytes),
// 0x186dd8 monitors +0x13cc, 0x186ec8 / 0x186fb8 position +0x13d0 / +0x13d4, 0x187198 host +0x13dc, 0x187388 players
// +0x13e4, 0x187478 auto connect +0x13e8, 0x187570 auto number +0x13ec, 0x187660 auto index +0x13f0) and the
// geometry at +0x24 (11 words). How they are applied:
//  - The LAN fields are written into the Option object just before GT4 reads them: each of their script accessors,
//    the race view set-up (0x439288 from 0x388a94), the centre-screen test 0x4395e8, the LAN member table 0x4f83e8,
//    the LAN race start 0x3368d0 and every race frame (0x389638). Whatever was there before (GT4's defaults, the
//    player's saved values, a menu change) is kept aside and put back before every Option save (0x4374a8, which
//    copies the Option object into the save), so the save keeps the player's own values.
//  - The geometry is GT4's only while the race view set-up runs (0x439288 from 0x388a94 to the render object's call
//    at 0x388b30, or an Option save / applyMenuDisplayMode first), inside GT4_ASPECT's own bracket (D6): the player's
//    geometry and GT4's wide flag are never changed.
// GT4 then draws this screen's view itself (0x439288 -> 0x4392e0: flat screens shifted off-axis by a whole screen
// and its borders, angled screens turned), in every race, LAN or not (TRIPLE-SCREEN.md 1.2).

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <atomic>
#include <string>

namespace gt4mm
{
    // Set when this copy's network is up (GT4's script calls initializeRTIME, 0x1dd118): a following side copy stops
    // taking controller 1 from then on (gt4_multimon_pad.cpp).
    std::atomic<bool> &lanUp(); // gt4_multimon_pad.cpp
    // GT4_MM_PAD for controller port 1 (ps2_iop_host.cpp readPadSnapshot): returns whether packet holds the state.
    bool padFilter(uint64_t tick, std::array<uint8_t, 32> &packet, bool ok);

    constexpr uint32_t kGeometry = 0x24u, kName = 0x1348u, kStyle = 0x1388u, kTarget = 0x138cu, kMonitors = 0x13ccu,
                       kPosX = 0x13d0u, kPosY = 0x13d4u, kHost = 0x13dcu, kPlayers = 0x13e4u, kAutoConnect = 0x13e8u,
                       kAutoIdx = 0x13f0u;
    constexpr uint32_t kStringBytes = 64u;

    struct Config
    {
        bool on = false, trace = false;
        // One entry per LAN word set (offset, value); the strings separately.
        uint32_t wordOffset[10] = {};
        uint32_t wordValue[10] = {};
        int words = 0;
        std::string name, target;
        bool hasName = false, hasTarget = false;
        bool geometry = false;
        uint32_t geo[11] = {};
        std::string summary;
    };

    inline std::string lowered(const char *v)
    {
        std::string m = v ? v : "";
        for (char &c : m)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        m.erase(0, m.find_first_not_of(" \t"));
        m.erase(m.find_last_not_of(" \t\r\n") + 1);
        return m;
    }

    inline const Config &config()
    {
        static const Config c = [] {
            Config k;
            auto word = [&k](uint32_t offset, uint32_t value, const std::string &what) {
                k.wordOffset[k.words] = offset;
                k.wordValue[k.words] = value;
                ++k.words;
                if (!what.empty())
                    k.summary += (k.summary.empty() ? "" : ", ") + what;
            };
            auto number = [](const char *key, int lo, int hi, int &out) {
                const char *v = std::getenv(key);
                if (!v || !*v)
                    return false;
                char *end = nullptr;
                const long n = std::strtol(v, &end, 10);
                if (end == v || *end || n < lo || n > hi)
                {
                    std::fprintf(stderr, "[gt4-mm] %s=%s not understood (%d-%d): GT4's own\n", key, v, lo, hi);
                    return false;
                }
                out = static_cast<int>(n);
                return true;
            };
            if (const char *v = std::getenv("GT4_MM_POSITION"); v && *v)
            {
                // GT4's menu (option.adc monpos_tbl2): Center (1,1), Left (0,1), Right (2,1), Top Center (1,0),
                // Top Left (0,0), Top Right (2,0).
                const std::string m = lowered(v);
                static const struct
                {
                    const char *name;
                    uint32_t x, y;
                } kPositions[] = {{"center", 1, 1}, {"centre", 1, 1}, {"left", 0, 1}, {"right", 2, 1}, {"top-center", 1, 0},
                                  {"top-centre", 1, 0}, {"top-left", 0, 0}, {"top-right", 2, 0}};
                bool found = false;
                for (const auto &p : kPositions)
                    if (m == p.name)
                    {
                        word(kPosX, p.x, std::string("position ") + p.name + " (" + std::to_string(p.x) + "," + std::to_string(p.y) + ")");
                        word(kPosY, p.y, "");
                        found = true;
                    }
                if (!found)
                    std::fprintf(stderr, "[gt4-mm] GT4_MM_POSITION=%s not understood (center, left, right, top-center, top-left, top-right): GT4's own\n", v);
            }
            if (const char *v = std::getenv("GT4_MM_STYLE"); v && *v)
            {
                const std::string m = lowered(v);
                const int s = m == "play" ? 0 : m == "live" ? 1 : m == "monitor" ? 2 : -1;
                if (s >= 0)
                    word(kStyle, static_cast<uint32_t>(s), "style " + m);
                else
                    std::fprintf(stderr, "[gt4-mm] GT4_MM_STYLE=%s not understood (play, live, monitor): GT4's own\n", v);
            }
            int n = 0;
            if (number("GT4_MM_MONITORS", 0, 5, n))
                word(kMonitors, static_cast<uint32_t>(n), "monitors " + std::to_string(n));
            if (number("GT4_MM_PLAYERS", 1, 6, n))
                word(kPlayers, static_cast<uint32_t>(n), "players " + std::to_string(n));
            if (number("GT4_MM_HOST", 0, 1, n))
                word(kHost, static_cast<uint32_t>(n), std::string("host ") + (n ? "yes" : "no"));
            if (number("GT4_MM_AUTO_CONNECT", 0, 1, n))
                word(kAutoConnect, static_cast<uint32_t>(n), std::string("auto connect ") + (n ? "on" : "off"));
            if (number("GT4_MM_AUTO_INDEX", 0, 254, n))
                word(kAutoIdx, static_cast<uint32_t>(n), "auto index " + std::to_string(n));
            auto text = [](const char *key, std::string &out) {
                const char *v = std::getenv(key);
                if (!v || !*v)
                    return false;
                std::string s(v);
                bool ok = s.size() <= 15;
                for (char c : s)
                    ok = ok && c >= 0x20 && c < 0x7f;
                if (!ok)
                {
                    std::fprintf(stderr, "[gt4-mm] %s=%s not understood (1-15 plain characters): GT4's own\n", key, v);
                    return false;
                }
                out = s;
                return true;
            };
            if ((k.hasName = text("GT4_MM_NAME", k.name)))
                k.summary += (k.summary.empty() ? "" : ", ") + std::string("name '") + k.name + "'";
            if ((k.hasTarget = text("GT4_MM_TARGET", k.target)))
                k.summary += (k.summary.empty() ? "" : ", ") + std::string("target '") + k.target + "'";
            if (const char *v = std::getenv("GT4_MM_GEOMETRY"); v && *v)
            {
                float f[11] = {};
                int flag = 0;
                const int got = std::sscanf(v, "%d,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &flag, &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10]);
                bool ok = got == 11 && flag >= 0 && flag <= 3;
                for (int i = 1; i <= 6 && ok; ++i)
                    ok = f[i] > 0.0f && f[i] < 100000.0f;
                for (int i = 7; i <= 10 && ok; ++i)
                    ok = f[i] >= 0.0f && f[i] < 10000.0f;
                if (ok)
                {
                    k.geometry = true;
                    k.geo[0] = static_cast<uint32_t>(flag);
                    for (int i = 1; i < 11; ++i)
                        std::memcpy(&k.geo[i], &f[i], 4);
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "geometry %s, zoom %g, eye %g mm, picture %g x %g mm, borders %g/%g/%g/%g mm",
                                  (flag & 1) ? "angled" : "flat", f[1], f[2], f[3], f[4], f[7], f[8], f[9], f[10]);
                    k.summary += (k.summary.empty() ? "" : ", ") + std::string(buf);
                }
                else
                    std::fprintf(stderr, "[gt4-mm] GT4_MM_GEOMETRY=%s not understood (11 numbers: flag 0-3, zoom, distance, width, height, aspect, scale, "
                                         "borders left, right, top, bottom): GT4's own\n", v);
            }
            k.trace = std::getenv("GT4_MM_TRACE") && *std::getenv("GT4_MM_TRACE") == '1';
            k.on = k.words > 0 || k.hasName || k.hasTarget || k.geometry || (std::getenv("GT4_MM_PAD") && *std::getenv("GT4_MM_PAD"));
            if (k.on)
                std::fprintf(stderr, "[gt4-mm] GT4 Multi Monitor settings (multi-monitor): %s; the player's own options and save unchanged\n", k.summary.c_str());
            return k;
        }();
        return c;
    }

    // The EE thread's state.
    struct State
    {
        bool guards = false, guardsChecked = false;
        uint32_t option = 0;          // the Option object last applied to
        uint32_t playerWord[10] = {}; // what was in GT4's Option before (the player's values)
        uint8_t playerName[kStringBytes] = {}, playerTarget[kStringBytes] = {};
        bool havePlayer = false;
        bool geoOpen = false;
        uint32_t geoSaved[11] = {};
        uint64_t applies = 0, geoOpens = 0, restores = 0;
        uint64_t raceFrames = 0; // race frame updates (0x389638) since the LAN race start (GT4_MM_TRACE)
    };
    inline State &state()
    {
        static State s;
        return s;
    }

    inline uint32_t rd(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (a & 0x01fffffcu), 4);
        return v;
    }
    inline void wr(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01fffffcu), &v, 4); }

    // The CORE words this relies on (GT4 USA, CORE.elf as the recompiler saw it).
    inline bool guards(const uint8_t *rdram)
    {
        State &s = state();
        if (!s.guardsChecked)
        {
            s.guardsChecked = true;
            s.guards = rd(rdram, 0x186e34u) == 0xac6213ccu && rd(rdram, 0x186f24u) == 0xac6213d0u && rd(rdram, 0x187014u) == 0xac6213d4u &&
                       rd(rdram, 0x1871f0u) == 0xac6213dcu && rd(rdram, 0x1873e4u) == 0xac6213e4u && rd(rdram, 0x1874d0u) == 0xac6213e8u &&
                       rd(rdram, 0x1876bcu) == 0xac6213f0u && rd(rdram, 0x1877d0u) == 0x24841348u && rd(rdram, 0x4374a8u) == 0x27bdffe0u &&
                       rd(rdram, 0x4375a0u) == 0x26041344u && rd(rdram, 0x388a94u) == 0x0c10e4a2u && rd(rdram, 0x388b30u) == 0x0040f809u &&
                       rd(rdram, 0x439290u) == 0x8c622f4cu && rd(rdram, 0x4392a0u) == 0x34a58cd4u;
            if (!s.guards)
                std::fprintf(stderr, "[gt4-mm] CORE does not match: GT4 Multi Monitor settings not applied, GT4's own options\n");
        }
        return s.guards;
    }

    inline uint32_t gameOption(const uint8_t *rdram) { return (rd(rdram, 0x622f4cu) & 0x1fffffffu) + 0x38cb0u; }

    inline void putString(uint8_t *rdram, uint32_t a, const std::string &s)
    {
        uint8_t buf[kStringBytes] = {};
        std::memcpy(buf, s.data(), std::min<size_t>(s.size(), kStringBytes - 1));
        std::memcpy(rdram + a, buf, kStringBytes);
    }
    inline bool sameString(const uint8_t *rdram, uint32_t a, const std::string &s)
    {
        uint8_t buf[kStringBytes] = {};
        std::memcpy(buf, s.data(), std::min<size_t>(s.size(), kStringBytes - 1));
        return std::memcmp(rdram + a, buf, kStringBytes) == 0;
    }

    // Writes the configured LAN values into GT4's Option object, keeping aside what was there.
    inline void apply(uint8_t *rdram, uint64_t tick, const char *why)
    {
        const Config &c = config();
        State &s = state();
        if (!c.on || !guards(rdram))
            return;
        const uint32_t o = gameOption(rdram);
        if (o < 0x100000u || o + 0x1400u >= 0x02000000u)
            return;
        if (o != s.option)
        {
            s.option = o;
            s.havePlayer = false;
        }
        bool changed = false;
        // A value that differs from ours is GT4's own (its default, the player's saved one or a menu change): kept aside.
        for (int i = 0; i < c.words; ++i)
        {
            const uint32_t a = o + c.wordOffset[i], now = rd(rdram, a);
            if (!s.havePlayer || now != c.wordValue[i])
                s.playerWord[i] = now;
            if (now != c.wordValue[i])
            {
                wr(rdram, a, c.wordValue[i]);
                changed = true;
            }
        }
        if (c.hasName && (!s.havePlayer || !sameString(rdram, o + kName, c.name)))
        {
            changed = changed || !sameString(rdram, o + kName, c.name);
            std::memcpy(s.playerName, rdram + o + kName, kStringBytes);
            putString(rdram, o + kName, c.name);
        }
        if (c.hasTarget && (!s.havePlayer || !sameString(rdram, o + kTarget, c.target)))
        {
            changed = changed || !sameString(rdram, o + kTarget, c.target);
            std::memcpy(s.playerTarget, rdram + o + kTarget, kStringBytes);
            putString(rdram, o + kTarget, c.target);
        }
        s.havePlayer = true;
        if (changed && (s.applies++ < 40 || c.trace))
            std::fprintf(stderr, "[gt4-mm] vsync %llu: the Multi Monitor settings written into GT4's Options (Option %x) at %s\n",
                         static_cast<unsigned long long>(tick), o, why);
    }

    // Before an Option save: the player's own values back (apply writes ours again at the next read).
    inline void restore(uint8_t *rdram, uint64_t tick)
    {
        const Config &c = config();
        State &s = state();
        if (!c.on || !s.havePlayer || !s.option || s.option != gameOption(rdram))
            return;
        const uint32_t o = s.option;
        for (int i = 0; i < c.words; ++i)
            wr(rdram, o + c.wordOffset[i], s.playerWord[i]);
        if (c.hasName)
            std::memcpy(rdram + o + kName, s.playerName, kStringBytes);
        if (c.hasTarget)
            std::memcpy(rdram + o + kTarget, s.playerTarget, kStringBytes);
        s.havePlayer = false; // the next apply keeps the player's values aside again
        if (s.restores++ < 40 || c.trace)
            std::fprintf(stderr, "[gt4-mm] vsync %llu: Option save: the player's own LAN options put back for the save\n", static_cast<unsigned long long>(tick));
    }

    // dispatchGuestBranch, before GT4_ASPECT's bracket code (so this geometry is taken off first).
    inline void beforeAspect(uint8_t *rdram, uint32_t targetPc, uint32_t sourcePc, uint64_t tick)
    {
        const Config &c = config();
        State &s = state();
        if (!c.on)
            return;
        if (s.geoOpen && (sourcePc == 0x388b30u || targetPc == 0x4374a8u || targetPc == 0x436740u))
        {
            std::memcpy(rdram + s.option + kGeometry, s.geoSaved, sizeof(s.geoSaved));
            s.geoOpen = false;
        }
        if (targetPc == 0x4374a8u)
            restore(rdram, tick);
    }

    // dispatchGuestBranch, after GT4_ASPECT's bracket code (so this geometry wins over its preset while the view is set up).
    inline void afterAspect(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc, uint64_t tick)
    {
        const Config &c = config();
        State &s = state();
        if (!c.on)
            return;
        switch (targetPc)
        {
        case 0x187750u: case 0x187940u: case 0x187b38u: case 0x186dd8u: case 0x186ec8u: case 0x186fb8u: case 0x187198u:
        case 0x187388u: case 0x187478u: case 0x187570u: case 0x187660u:
            apply(rdram, tick, "a LAN option read by GT4's scripts");
            break;
        case 0x439288u:
            apply(rdram, tick, "the race view set-up");
            if (sourcePc == 0x388a94u && c.geometry && guards(rdram) && !s.geoOpen && s.option)
            {
                std::memcpy(s.geoSaved, rdram + s.option + kGeometry, sizeof(s.geoSaved));
                std::memcpy(rdram + s.option + kGeometry, c.geo, sizeof(c.geo));
                s.geoOpen = true;
                if (s.geoOpens++ < 3 || c.trace)
                    std::fprintf(stderr, "[gt4-mm] vsync %llu: the race view set up with the Multi Monitor geometry (position %u,%u)\n",
                                 static_cast<unsigned long long>(tick), rd(rdram, s.option + kPosX), rd(rdram, s.option + kPosY));
            }
            else if (sourcePc == 0x388a94u && (s.geoOpens++ < 3 || c.trace))
                std::fprintf(stderr, "[gt4-mm] vsync %llu: the race view set up at position %u,%u (GT4's geometry)\n", static_cast<unsigned long long>(tick),
                             rd(rdram, s.option + kPosX), rd(rdram, s.option + kPosY));
            break;
        case 0x4395e8u:
            apply(rdram, tick, "GT4's centre-screen test");
            if (c.trace)
                std::fprintf(stderr, "[gt4-mm] vsync %llu: centre-screen test 0x4395e8 from %x\n", static_cast<unsigned long long>(tick), sourcePc);
            break;
        case 0x4f83e8u:
            apply(rdram, tick, "the LAN member table");
            break;
        case 0x3368d0u:
            apply(rdram, tick, "the LAN race start");
            s.raceFrames = 0; // the race frames are counted from here (the same moment on every copy of a LAN race)
            std::fprintf(stderr, "[gt4-mm] vsync %llu: LAN race start (RaceLanBattle 0x3368d0, mode %u)\n", static_cast<unsigned long long>(tick),
                         GPR_U32(ctx, 5));
            break;
        case 0x389638u:
            apply(rdram, tick, "a race frame");
            if (c.trace)
            {
                if (s.raceFrames++ % 60u == 0u)
                    std::fprintf(stderr, "[gt4-mm] race frame %llu at vsync %llu\n", static_cast<unsigned long long>(s.raceFrames - 1),
                                 static_cast<unsigned long long>(tick));
            }
            break;
        case 0x1dd118u:
            if (!lanUp().exchange(true))
                std::fprintf(stderr, "[gt4-mm] vsync %llu: the network is up (GT4's initializeRTIME)\n", static_cast<unsigned long long>(tick));
            break;
        default:
            break;
        }
        if (c.trace)
        {
            // GT4's LAN script calls (bindings: mNetwork methods), each logged its first 3 times and every 300th.
            static const struct
            {
                uint32_t pc;
                const char *name;
            } kNet[] = {{0x1e4b20u, "gcreateLAN"},          {0x1e4d48u, "gjoinLAN"},    {0x1e6780u, "raceMenuNotifySequence"},
                        {0x1e6b40u, "raceMenuAllConnected"}, {0x5d3458u, "enterNetSync"}, {0x1e70e0u, "netSync"},
                        {0x5d3480u, "leaveNetSync"}};
            static uint64_t counts[sizeof(kNet) / sizeof(kNet[0])] = {};
            for (size_t i = 0; i < sizeof(kNet) / sizeof(kNet[0]); ++i)
                if (targetPc == kNet[i].pc)
                {
                    const uint64_t n = counts[i]++;
                    if (n < 3 || n % 300 == 0)
                        std::fprintf(stderr, "[gt4-mm] vsync %llu: script %s (call %llu)\n", static_cast<unsigned long long>(tick), kNet[i].name,
                                     static_cast<unsigned long long>(n + 1));
                }
            static int said = 0;
            if (targetPc == 0x1dcca0u && said++ < 200)
                std::fprintf(stderr, "[gt4-mm] vsync %llu: script isNetworkAdaptorAvailable: adaptor flag 0x645430 = %u\n",
                             static_cast<unsigned long long>(tick), rd(rdram, 0x645430u));
            if (targetPc == 0x4ed008u && said++ < 200)
                std::fprintf(stderr, "[gt4-mm] vsync %llu: the adaptor check 0x4ed008 (devctl dev9x: 0x4401) from %x; flag before %u\n",
                             static_cast<unsigned long long>(tick), sourcePc, rd(rdram, 0x645430u));
        }
    }
}
#endif
