// GT4 Multi Monitor Mode: controller 1 for the copies of one group (GT4_MM_PAD, gt4_multimon.h; analysis/design/
// multi-monitor). Development and Full GT4.exe only (the Classic build compiles an empty filter).
//   share   the copy you drive: what it reads for controller 1 (pad, keyboard, its controller mapping) is offered to
//           the other copies of its group (GT4_MM_GROUP, default gt4) through a small named shared memory block
//           ("Local\GT4MultiMonitorPad-<group>": a sequence number, the time, the 32-byte pad packet).
//   follow  a side copy: until its network is up (GT4's script calls initializeRTIME, gt4mm::lanUp) controller 1 is
//           the shared one (released when nothing was offered for half a second), so the side screens follow you
//           through the opening, the title, Arcade Mode and Multi-LAN Race; from then on nothing is pressed (the side
//           screens connect, wait and follow the race by themselves). Its own pads and keyboard are never read.
//   off     never any host input on controller 1.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
// (Not gt4_multimon.h: that header needs ps2_runtime.h, which does not mix with windows.h.)
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gt4mm
{
    std::atomic<bool> &lanUp()
    {
        static std::atomic<bool> up{false};
        return up;
    }

    namespace
    {
        std::string lowered(const char *v)
        {
            std::string m = v ? v : "";
            for (char &c : m)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            m.erase(0, m.find_first_not_of(" \t"));
            m.erase(m.find_last_not_of(" \t\r\n") + 1);
            return m;
        }

        enum class PadMode
        {
            Own,
            Share,
            Follow,
            Off
        };

#if defined(_WIN32)
        struct Shared
        {
            uint32_t magic;            // 'GT4P'
            uint32_t owner;            // the sharing copy's process id
            volatile LONG64 sequence;  // odd while being written
            volatile LONG64 writtenMs; // steady clock, ms
            uint8_t packet[32];
        };
#else
        struct Shared;
#endif
        constexpr uint32_t kMagic = 0x50345447u;

        PadMode mode()
        {
            static const PadMode m = [] {
                const std::string v = lowered(std::getenv("GT4_MM_PAD"));
                const PadMode k = v == "share" ? PadMode::Share : v == "follow" ? PadMode::Follow : v == "off" ? PadMode::Off : PadMode::Own;
                if (!v.empty() && k == PadMode::Own)
                    std::fprintf(stderr, "[gt4-mm] GT4_MM_PAD=%s not understood (share, follow, off): this copy's own controller\n", v.c_str());
                else if (k != PadMode::Own)
                    std::fprintf(stderr, "[gt4-mm] controller 1: %s\n",
                                 k == PadMode::Share    ? "shared with the side copies of this group"
                                 : k == PadMode::Follow ? "follows the copy you drive until the network is up, then nothing"
                                                        : "no host input");
                return k;
            }();
            return m;
        }

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void neutral(std::array<uint8_t, 32> &p)
        {
            // As PSPadReplay with nothing pressed: DualShock 2 with pressure bytes, buttons released, sticks centred.
            p.fill(0);
            p[0] = 1;
            p[1] = 0x79;
            p[2] = p[3] = 0xff;
            std::memset(p.data() + 4, 0x80, 4);
        }

#if defined(_WIN32)
        Shared *block(bool create)
        {
            static Shared *mapped = nullptr;
            static int64_t lastTry = -100000;
            if (mapped)
                return mapped;
            const int64_t now = nowMs();
            if (!create && now - lastTry < 1000)
                return nullptr; // a follower looks again once a second
            lastTry = now;
            const char *g = std::getenv("GT4_MM_GROUP");
            std::string group = g && *g ? g : "gt4";
            for (char &c : group)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                    c = '_';
            const std::string name = "Local\\GT4MultiMonitorPad-" + group.substr(0, 40);
            HANDLE h = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str())
                              : OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
            if (!h)
                return nullptr;
            void *v = MapViewOfFile(h, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, sizeof(Shared));
            if (!v)
            {
                CloseHandle(h);
                return nullptr;
            }
            mapped = static_cast<Shared *>(v); // kept for the whole run (the handle with it)
            std::fprintf(stderr, "[gt4-mm] controller 1 %s '%s'\n", create ? "offered through" : "taken from", name.c_str());
            return mapped;
        }
#else
        Shared *block(bool) { return nullptr; }
#endif
    }

    bool padFilter(uint64_t tick, std::array<uint8_t, 32> &packet, bool ok)
    {
        (void)tick;
        switch (mode())
        {
        case PadMode::Own:
            return ok;
        case PadMode::Share:
#if defined(_WIN32)
            if (ok)
                if (Shared *s = block(true))
                {
                    InterlockedIncrement64(&s->sequence); // odd: being written
                    s->magic = kMagic;
                    s->owner = GetCurrentProcessId();
                    std::memcpy(s->packet, packet.data(), sizeof(s->packet));
                    s->writtenMs = nowMs();
                    InterlockedIncrement64(&s->sequence); // even: complete
                }
#endif
            return ok;
        case PadMode::Follow:
            neutral(packet);
            if (!lanUp().load(std::memory_order_relaxed))
            {
#if defined(_WIN32)
                if (const Shared *s = block(false); s && s->magic == kMagic)
                {
                    for (int attempt = 0; attempt < 4; ++attempt)
                    {
                        const LONG64 before = s->sequence;
                        if (before & 1)
                            continue;
                        uint8_t copy[32];
                        std::memcpy(copy, s->packet, sizeof(copy));
                        const LONG64 written = s->writtenMs;
                        if (s->sequence != before)
                            continue;
                        if (nowMs() - written <= 500)
                            std::memcpy(packet.data(), copy, sizeof(copy));
                        break;
                    }
                }
#endif
            }
            return true;
        case PadMode::Off:
            neutral(packet);
            return true;
        }
        return ok;
    }
}
#endif
