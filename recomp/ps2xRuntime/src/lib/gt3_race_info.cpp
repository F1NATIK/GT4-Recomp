#include "gt3_race_info.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>

namespace gt3race
{
    namespace
    {
        constexpr uint32_t kHudLapTime = 0x193040u;   // (hud, entry): the race HUD's lap time
        constexpr uint32_t kVblankCount = 0x4a69c0u;  // GT3's VBlank counter
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr uint32_t kFirstCar = 0xc8u, kCarStride = 0x16dcu, kEntryStride = 0x2160u;
        constexpr int kMaxCars = 8;
        PS2Runtime::RecompiledFunction g_hudLapTime = nullptr;
        std::atomic<uint32_t> g_entry{0u};
        std::atomic<uint32_t> g_hud{0u}; // the race HUD object (a0 of 0x193040)
        std::atomic<uint32_t> g_seenAt{0u};

        uint32_t u32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & (kRamSize - 4u)), 4);
            return v;
        }
        float f32(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & (kRamSize - 4u)), 4);
            return v;
        }
        bool inRam(uint32_t address, uint32_t size) { return address >= 0x100000u && address < kRamSize - size; }

        // Names by car ID (the phone view's sampler and the HUD's name
        // thread; g_namesMutex). An ID not found is retried after a few
        // seconds (its data may still be loading).
        struct NameEntry
        {
            std::string name, model;
            uint32_t triedAt = 0;
        };
        std::map<uint64_t, NameEntry> g_names;
        std::mutex g_namesMutex;

        // String `index` of the STDB table at `table`, if it is plain text.
        bool stdbString(const uint8_t *rdram, uint32_t table, uint32_t index, std::string &out)
        {
            if (index >= u32(rdram, table + 4u))
                return false;
            const uint32_t at = table + u32(rdram, table + 0x10u + index * 4u);
            if (!inRam(at, 0x200u))
                return false;
            uint16_t length;
            std::memcpy(&length, rdram + at, 2);
            if (length < 2u || length > 0x100u)
                return false;
            out.clear();
            for (uint32_t i = 0; i < length && rdram[at + 2u + i] != 0; ++i)
            {
                const uint8_t c = rdram[at + 2u + i];
                if (c < 0x20u || c >= 0x7fu)
                    return false; // Japanese (EUC) or not text
                out += static_cast<char>(c);
            }
            return !out.empty();
        }

        // Maker and model; false if not found.
        bool lookUpName(const uint8_t *rdram, uint64_t id, std::string &makerOut, std::string &modelOut)
        {
            // The car-name string table: an "STDB" (count 1..10000) that holds
            // maker names (other tables, e.g. part names, match by chance).
            std::vector<uint32_t> tables;
            for (uint32_t a = 0x100000u; a + 16u < kRamSize && tables.size() < 4u; a += 4u)
            {
                if (std::memcmp(rdram + a, "STDB", 4) != 0 || u32(rdram, a + 4u) - 1u >= 10000u)
                    continue;
                int makers = 0;
                std::string s;
                for (uint32_t i = 0; i < u32(rdram, a + 4u) && makers < 2; ++i)
                    if (stdbString(rdram, a, i, s) && (s == "TOYOTA" || s == "HONDA" || s == "NISSAN" || s == "MAZDA"))
                        ++makers;
                if (makers >= 2)
                    tables.push_back(a);
            }
            // The loaded car table: 0x108-byte records that start with the car's
            // ID (then 0x27f731 at +0x18) and end with the model and maker
            // string numbers at +0x100 / +0x104. (+0x10 is not a constant: 0x376e
            // for the Arcade cars, 0x30e1 for the IMPREZA Rally Car prototype -
            // requiring 0x376e left such cars "Car N", the user's memory card
            // race, `ramdump/mcn-4500.bin`, 2026-10-01.) (Reading the numbers
            // just before the ID - the previous record's - named the Arcade
            // Altezza RS200 "ALTEZZA AS200", 2026-09-29.)
            uint8_t pattern[8];
            std::memcpy(pattern, &id, 8);
            for (uint32_t a = 0x100000u; a + 0x108u < kRamSize; a += 4u)
            {
                if (std::memcmp(rdram + a, pattern, 8) != 0 || u32(rdram, a + 0x18u) != 0x27f731u)
                    continue;
                uint16_t model, maker;
                std::memcpy(&model, rdram + a + 0x100u, 2);
                std::memcpy(&maker, rdram + a + 0x104u, 2);
                for (const uint32_t table : tables)
                {
                    std::string modelName, makerName;
                    if (stdbString(rdram, table, model, modelName) && stdbString(rdram, table, maker, makerName) && modelName.size() >= 3u)
                    {
                        makerOut = makerName;
                        modelOut = modelName;
                        return true;
                    }
                }
            }
            return false;
        }

        void hudLapTimeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t entry = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
            // Only the HUD of the race's first car (the player's): Free Run
            // runs a second HUD every frame for the car in slot 1 (entries
            // 0x92beb0 / 0x92e010 alternating), which hid the player's.
            const uint32_t e = entry & (kRamSize - 1u);
            bool first = false;
            if (inRam(e, 0x20u))
            {
                const uint32_t car = u32(rdram, e + 0x18u) & (kRamSize - 1u);
                first = inRam(car, 8u) && (u32(rdram, car + 4u) & (kRamSize - 1u)) + kFirstCar == car;
            }
            if (first)
            {
                // A new race entry: where its player car is (for offline RAM searches).
                if (entry != g_entry.load(std::memory_order_relaxed))
                    std::cerr << "[race-info] race entry 0x" << std::hex << entry << " player car 0x" << u32(rdram, e + 0x18u) << " hud 0x"
                              << (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & (kRamSize - 1u)) << std::dec << " at vsync "
                              << u32(rdram, kVblankCount) << std::endl;
                g_entry.store(entry, std::memory_order_relaxed);
                g_hud.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & (kRamSize - 1u), std::memory_order_relaxed);
                g_seenAt.store(u32(rdram, kVblankCount), std::memory_order_relaxed);
            }
            g_hudLapTime(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        if (g_hudLapTime)
            return; // already wrapped (the phone view and the autopilot both ask)
        // jal 0x1eb2f8 / lw $a0, 0x18($s1) at 0x193058 (GT3 USA CORE).
        if (u32(rdram, 0x193058u) != 0x0c07acbeu || u32(rdram, 0x19305cu) != 0x8e240018u)
        {
            std::cerr << "[race-info] not installed: this CORE differs at 0x193058" << std::endl;
            return;
        }
        g_hudLapTime = runtime.lookupFunction(kHudLapTime);
        if (!g_hudLapTime)
            return;
        runtime.registerFunction(kHudLapTime, &hudLapTimeHook);
    }

    bool read(const uint8_t *rdram, Snapshot &out, bool names)
    {
        out.cars.clear();
        const uint32_t entry = g_entry.load(std::memory_order_relaxed) & (kRamSize - 1u);
        if (!rdram || !g_hudLapTime || entry == 0u || u32(rdram, kVblankCount) - g_seenAt.load(std::memory_order_relaxed) > 30u)
            return false;
        if (!inRam(entry, 0x20u))
            return false;
        const uint32_t player = u32(rdram, entry + 0x18u) & (kRamSize - 1u);
        if (!inRam(player, kCarStride))
            return false;
        const uint32_t race = u32(rdram, player + 4u) & (kRamSize - 1u);
        if (!inRam(race, 0xe400u) || race + kFirstCar != player)
            return false;
        out.race = race;
        out.clockMs = u32(rdram, race + 0xe380u) / 3u;
        out.replay = u32(rdram, race + 0xb7acu) == 1u; // see Snapshot::replay
        // GT3's own gauge lamps (see Snapshot::lamps).
        if (const uint32_t hud = g_hud.load(std::memory_order_relaxed); inRam(hud, 0xA180u))
        {
            for (int k = 0; k < 4; ++k)
                out.lamps[k] = rdram[hud + 0xA15Cu + 0x18u + static_cast<uint32_t>(k)] != 0;
            const uint32_t total = u32(rdram, hud + 0x7Cu);
            out.totalLaps = total >= 1u && total <= 99u ? static_cast<int>(total) : 0;
        }
        for (int k = 0; k < kMaxCars; ++k)
        {
            const uint32_t car = race + kFirstCar + static_cast<uint32_t>(k) * kCarStride;
            if (!inRam(car, kCarStride) || (u32(rdram, car + 4u) & (kRamSize - 1u)) != race)
                break;
            Car c;
            c.index = k;
            c.player = k == 0;
            c.position = rdram[car + 0x1465u];
            c.lap = rdram[car + 0x13b8u];
            c.lapStartMs = u32(rdram, car + 0x14b4u);
            c.lastLapMs = u32(rdram, car + 0x16c4u);
            c.x = f32(rdram, car + 0x14bcu);
            c.y = f32(rdram, car + 0x14c0u);
            c.z = f32(rdram, car + 0x14c4u);
            c.distance = f32(rdram, car + 0x1374u);
            c.progress = f32(rdram, car + 0x1368u);
            c.yaw = f32(rdram, car + 0x13f4u);
            c.speed = f32(rdram, car + 0x140cu);
            {
                uint16_t v;
                std::memcpy(&v, rdram + car + 0x1414u, 2);
                c.rpm = v;
                std::memcpy(&v, rdram + car + 0x236u, 2);
                c.redline = v;
            }
            c.gear = rdram[car + 0x13bau];
            for (uint32_t w = 0; w < 4u; ++w)
                c.surface[w] = rdram[car + 0x1008u + 0xa4u * w];
            if (c.position < 1 || c.position > kMaxCars || c.lap > 99)
                break;
            // The car's race entry and name.
            const uint32_t carEntry = entry + static_cast<uint32_t>(k) * kEntryStride;
            if (names && inRam(carEntry, 0x28u) && (u32(rdram, carEntry + 0x18u) & (kRamSize - 1u)) == car)
            {
                uint64_t id;
                std::memcpy(&id, rdram + carEntry + 0x20u, 8);
                const uint32_t now = u32(rdram, kVblankCount);
                std::lock_guard<std::mutex> lock(g_namesMutex);
                NameEntry &n = g_names[id];
                if (n.name.empty() && (n.triedAt == 0u || now - n.triedAt > 300u))
                {
                    std::string maker, model;
                    if (lookUpName(rdram, id, maker, model))
                    {
                        n.name = maker + " " + model;
                        n.model = model;
                    }
                    n.triedAt = now | 1u;
                }
                c.name = n.name;
                c.model = n.model;
            }
            out.cars.push_back(c);
        }
        return !out.cars.empty();
    }

    bool findCourseEdges(const uint8_t *rdram, const Snapshot &s, std::vector<std::vector<float>> &edges)
    {
        edges.clear();
        if (!rdram || s.cars.empty())
            return false;
        struct Loop
        {
            uint32_t at, count;
            double area, length;
            float x0, x1, z0, z1;
        };
        std::vector<Loop> loops;
        for (uint32_t a = 0x100000u; a + 32u <= kRamSize; a += 16u)
        {
            // (The count may carry the 0x10000 flag: Laguna Seca's edges,
            // 0x1029a = 666 points.)
            const uint32_t n = u32(rdram, a + 12u) & 0xFFFFu;
            if (n < 16u || n > 20000u || a + n * 16u > kRamSize || u32(rdram, a + 28u) >= 0x40000u)
                continue;
            double length = 0.0, area = 0.0;
            float px = 0.0f, pz = 0.0f, x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
            bool ok = true;
            for (uint32_t i = 0; i < n && ok; ++i)
            {
                const uint32_t r = a + i * 16u;
                const float x = f32(rdram, r), y = f32(rdram, r + 4u), z = f32(rdram, r + 8u);
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::fabs(x) > 20000.0f || std::fabs(z) > 20000.0f ||
                    std::fabs(y) > 3000.0f)
                    ok = false;
                else if (i > 0)
                {
                    const double step = std::hypot(static_cast<double>(x) - px, static_cast<double>(z) - pz);
                    if (u32(rdram, r + 12u) >= 0x40000u || step < 0.05 || step > 60.0)
                        ok = false;
                    length += step;
                    area += static_cast<double>(px) * z - static_cast<double>(x) * pz;
                }
                px = x;
                pz = z;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                z0 = std::min(z0, z);
                z1 = std::max(z1, z);
            }
            if (!ok || length < 200.0)
                continue;
            const float fx = f32(rdram, a), fz = f32(rdram, a + 8u);
            if (std::hypot(fx - px, fz - pz) > 30.0f)
                continue; // not closed
            area = 0.5 * std::fabs(area + static_cast<double>(px) * fz - static_cast<double>(fx) * pz);
            if (area < 5000.0)
                continue; // out and back (pit lanes and the like)
            loops.push_back(Loop{a, n, area, length, x0, x1, z0, z1});
            a += (n - 1u) * 16u;
        }
        std::sort(loops.begin(), loops.end(), [](const Loop &p, const Loop &q) { return p.area > q.area; });
        // The two largest, skipping copies of one already taken.
        std::vector<const Loop *> chosen;
        for (const Loop &l : loops)
        {
            bool copy = false;
            for (const Loop *c : chosen)
                copy |= std::fabs(c->area - l.area) < c->area * 0.002 && std::fabs(c->length - l.length) < c->length * 0.002;
            if (!copy)
                chosen.push_back(&l);
            if (chosen.size() == 2u)
                break;
        }
        if (chosen.size() < 2u || chosen[1]->length < chosen[0]->length * 0.6 || chosen[1]->length > chosen[0]->length * 1.6)
            return false;
        const Loop &outer = *chosen[0];
        for (const Car &c : s.cars)
            if (c.x < outer.x0 - 30.0f || c.x > outer.x1 + 30.0f || c.z < outer.z0 - 30.0f || c.z > outer.z1 + 30.0f)
                return false; // another course's data
        for (const Loop *l : chosen)
        {
            std::vector<float> e;
            e.reserve(l->count * 2u);
            for (uint32_t i = 0; i < l->count; ++i)
            {
                e.push_back(f32(rdram, l->at + i * 16u));
                e.push_back(f32(rdram, l->at + i * 16u + 8u));
            }
            edges.push_back(std::move(e));
        }
        return true;
    }
}
