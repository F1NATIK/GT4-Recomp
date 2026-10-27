// Custom Sounds: the race cars' engine state (gt4_car_state.h). Not in the Classic build.
#if !defined(GT4_STOCK_BUILD)
#include "ps2x/iop/gt4_car_state.h"
#include "gt4_sound_pack.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace ps2x::iop::gt4::carstate
{
    namespace
    {
        constexpr uint32_t RamMask = 0x01ffffffu;
        constexpr uint32_t CarBase = 0x6698u, CarStride = 0x10a0u, CarCount = 0xf8a0u;

        std::mutex mutex_;
        Snapshot latest_;
        std::atomic<uint64_t> soundFrames_{0};

        uint32_t u32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (RamMask & ~3u)), 4);
            return v;
        }
        float f32(const uint8_t *rdram, uint32_t a)
        {
            const uint32_t v = u32(rdram, a);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        // GT4_CAR_DUMP=<file> (test only): per race frame a record
        //   "GT4C" u32 cars, u64 vsync, u64 sound frame, u32 R, u32 D, u32 R+0xcc8, s32 player, then cars x 0x10a0 bytes.
        // GT4_CAR_DUMP_EVERY=<n> keeps every n-th race frame (default 1).
        // GT4_CAR_DUMP_RAM=<file> (test only): the whole 32 MiB of EE RAM once, at race frame GT4_CAR_DUMP_RAM_AT (default
        // 3000, counted from the first race frame update), to find what the car records point to.
        struct Dump
        {
            std::FILE *file = nullptr;
            uint64_t every = 1, frames = 0, ramAt = 3000;
            std::string ramPath;
            Dump()
            {
                const char *path = std::getenv("GT4_CAR_DUMP");
                if (path && *path)
                    file = std::fopen(path, "wb");
                if (const char *e = std::getenv("GT4_CAR_DUMP_EVERY"); e && *e)
                    every = std::max<uint64_t>(1, std::strtoull(e, nullptr, 10));
                if (const char *r = std::getenv("GT4_CAR_DUMP_RAM"); r && *r)
                    ramPath = r;
                if (const char *e = std::getenv("GT4_CAR_DUMP_RAM_AT"); e && *e)
                    ramAt = std::strtoull(e, nullptr, 10);
            }
            ~Dump()
            {
                if (file)
                    std::fclose(file);
            }
        };
        Dump &dump()
        {
            static Dump d;
            return d;
        }
    }

    bool wanted()
    {
        static const bool on = [] {
            const char *pack = std::getenv("GT4_SOUND_PACK");
            const char *map = std::getenv("GT4_SOUND_PACK_MAP");
            const char *dump = std::getenv("GT4_CAR_DUMP"); // test only
            auto set = [](const char *v) { return v && *v && std::strcmp(v, "off") != 0; };
            return set(pack) || set(map) || set(dump);
        }();
        return on;
    }
}

namespace ps2x::iop::gt4::soundpack
{
    fs::path exeFolder()
    {
#if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n > 0 && n < buffer.size())
        {
            buffer.resize(n);
            return fs::path(buffer).parent_path();
        }
#endif
        std::error_code ec;
        return fs::current_path(ec);
    }
}

namespace ps2x::iop::gt4::carstate
{

    void onRaceFrame(const uint8_t *rdram, uint32_t race, uint64_t vsync)
    {
        race &= 0x1fffffffu;
        if (!rdram || race == 0u || race >= 0x02000000u)
            return;
        const uint32_t d = u32(rdram, race + 0x70u) & 0x1fffffffu;
        const uint32_t o = u32(rdram, race + 0x6cu) & 0x1fffffffu;
        if (d == 0u || d >= 0x02000000u - (CarBase + MaxCars * CarStride))
            return;
        Snapshot s;
        s.vsync = vsync;
        s.soundFrame = soundFrames();
        s.spec = u32(rdram, race + 0xcc8u);
        s.count = std::min<uint32_t>(u32(rdram, d + CarCount), MaxCars);
        for (unsigned i = 0; i < s.count; ++i)
        {
            const uint32_t c = d + CarBase + i * CarStride;
            Car &car = s.car[i];
            car.valid = true;
            const uint32_t ctrl = u32(rdram, c + 0xcu) & 0x1fffffffu;
            car.player = ctrl != 0u && ctrl < 0x02000000u && u32(rdram, ctrl + 0xd0u) == 0x686618u && u32(rdram, ctrl + 0x1ccu) != 0u;
            if (car.player && s.player < 0)
                s.player = int(i);
            car.throttle = f32(rdram, c + 0x598u);
            car.pedal = f32(rdram, c + 0x58cu);
            car.rpm = f32(rdram, c + 0x738u);
            car.gear = static_cast<int8_t>(rdram[(c + 0x624u) & RamMask]);
            // The car's spec code: the race entry's car record (SPEC_DATABASE::CarGarage: u64 code, u64 -1, ...) at
            // O + 0x9b0 + i * 0x35e0, O = *(R+0x6c) (the conductor every car's +0x0 points to). Checked by its -1 word.
            if (o != 0u && o < 0x02000000u - (0x9b0u + MaxCars * 0x35e0u + 16u))
            {
                const uint32_t rec = o + 0x9b0u + i * 0x35e0u;
                if (u32(rdram, rec + 4u) == 0u && u32(rdram, rec + 8u) == 0xffffffffu && u32(rdram, rec + 12u) == 0xffffffffu)
                    car.code = u32(rdram, rec);
            }
        }
        Dump &dmp = dump();
        if (!dmp.ramPath.empty() && dmp.frames == dmp.ramAt)
        {
            if (std::FILE *ram = std::fopen(dmp.ramPath.c_str(), "wb"))
            {
                std::fwrite(rdram, 1, 0x02000000u, ram);
                std::fclose(ram);
            }
        }
        if (dmp.file && (dmp.frames % dmp.every) == 0u)
        {
            uint8_t head[40] = {'G', 'T', '4', 'C'};
            const uint32_t n = s.count;
            const int32_t p = s.player;
            std::memcpy(head + 4, &n, 4);
            std::memcpy(head + 8, &s.vsync, 8);
            std::memcpy(head + 16, &s.soundFrame, 8);
            std::memcpy(head + 24, &race, 4);
            std::memcpy(head + 28, &d, 4);
            std::memcpy(head + 32, &s.spec, 4);
            std::memcpy(head + 36, &p, 4);
            std::fwrite(head, 1, sizeof(head), dmp.file);
            for (unsigned i = 0; i < n; ++i)
                std::fwrite(rdram + ((d + CarBase + i * CarStride) & RamMask), 1, CarStride, dmp.file);
        }
        ++dmp.frames;
        std::lock_guard lock(mutex_);
        s.seq = latest_.seq + 1u;
        latest_ = s;
    }

    Snapshot latest()
    {
        std::lock_guard lock(mutex_);
        return latest_;
    }

    void addSoundFrames(uint32_t frames) { soundFrames_.fetch_add(frames, std::memory_order_relaxed); }
    uint64_t soundFrames() { return soundFrames_.load(std::memory_order_relaxed); }
}
#endif
