#include "gt3_course_patch.h"

#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace gt3coursepatch
{
    namespace
    {
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr uint32_t kCourseLoader = 0x1A8D18u; // course file setup (a0 = the file, unrelocated)
        constexpr uint32_t kRunwaySlot = 0x70u;       // course slot 28: GTRW
        constexpr uint32_t kCourseBuffer = 0xD00000u; // GT3 reads a course into a pool block, at most this much (sub_001A9AD8)
        constexpr uint32_t kCoursePool = 0x2BB5B8u;   // that block pool (sub_0022CAA8 pops its free list)

        std::map<uint32_t, std::vector<uint8_t>> g_replacements; // crc32 of the GT3 section -> new section
        std::map<uint32_t, std::vector<uint8_t>> g_courses;      // crc32 of the GT3 runway section -> whole new course file
        PS2Runtime::RecompiledFunction g_loader = nullptr;
        std::mutex g_lastMutex;  // the course loaded last (lastCourse)
        bool g_haveLast = false;
        uint32_t g_lastCrc = 0;
        std::string g_lastReplacement;
        std::string g_replaceFolder; // GT3_COURSE_REPLACE's folder name

        uint32_t crc32(const uint8_t *p, size_t n)
        {
            static uint32_t table[256];
            static bool made = false;
            if (!made)
            {
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    table[i] = c;
                }
                made = true;
            }
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i)
                c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }

        uint32_t u32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (kRamSize - 4u)), 4);
            return v;
        }

        void loaderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t base = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & (kRamSize - 1u);
            // Unrelocated (word 0 = 0) course files only, with a GTRW in slot 28.
            const uint32_t slot = base + 0x80u < kRamSize && u32(rdram, base) == 0u ? u32(rdram, base + kRunwaySlot) : 0u;
            const uint32_t runway = base + slot;
            if (slot != 0u && runway + 0x20u < kRamSize && std::memcmp(rdram + runway, "GTRW", 4) == 0)
            {
                const uint32_t length = u32(rdram, runway + 0x0Cu) + 16u;
                if (runway + length <= kRamSize)
                {
                    const uint32_t crc = crc32(rdram + runway, length);
                    const auto course = g_courses.find(crc);
                    const auto it = g_replacements.find(crc);
                    {
                        std::lock_guard<std::mutex> lock(g_lastMutex);
                        g_haveLast = true;
                        g_lastCrc = crc;
                        g_lastReplacement = course != g_courses.end() && course->second.size() <= kCourseBuffer ? g_replaceFolder : std::string();
                    }
                    if (course != g_courses.end())
                    {
                        // The whole course file (its own runway included).
                        std::fprintf(stderr, "[course-patch] course pool 0x%x: %08x %08x %08x %08x %08x %08x\n", kCoursePool, u32(rdram, kCoursePool),
                                     u32(rdram, kCoursePool + 4u), u32(rdram, kCoursePool + 8u), u32(rdram, kCoursePool + 12u), u32(rdram, kCoursePool + 16u),
                                     u32(rdram, kCoursePool + 20u));
                        if (course->second.size() > kCourseBuffer || base + course->second.size() > kRamSize)
                            std::fprintf(stderr, "[course-patch] course for runway %08x: %zu bytes, over GT3's buffer: kept\n", crc, course->second.size());
                        else
                        {
                            std::memcpy(rdram + base, course->second.data(), course->second.size());
                            std::fprintf(stderr, "[course-patch] course at 0x%x replaced (runway %08x; %zu bytes)\n", base, crc, course->second.size());
                        }
                    }
                    else if (it == g_replacements.end())
                        std::fprintf(stderr, "[course-patch] course at 0x%x: runway %u bytes, crc32 %08x (no replacement)\n", base, length, crc);
                    else if (it->second.size() > length)
                        std::fprintf(stderr, "[course-patch] runway %08x: the replacement (%zu bytes) does not fit (%u): kept\n", crc, it->second.size(),
                                     length);
                    else
                    {
                        std::memcpy(rdram + runway, it->second.data(), it->second.size());
                        std::memset(rdram + runway + it->second.size(), 0, length - it->second.size());
                        std::fprintf(stderr, "[course-patch] runway %08x replaced (%zu of %u bytes)\n", crc, it->second.size(), length);
                    }
                }
            }
            g_loader(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        std::error_code ec;
        if (const char *dir = std::getenv("GT3_RUNWAY_REPLACE"); dir && *dir)
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                const std::string name = entry.path().filename().string();
                if (entry.path().extension() != ".gtrw" || name.size() != 13)
                    continue;
                const uint32_t crc = static_cast<uint32_t>(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16));
                std::ifstream in(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.size() >= 0x60u && std::memcmp(data.data(), "GTRW", 4) == 0)
                {
                    g_replacements[crc] = std::move(data);
                    std::fprintf(stderr, "[course-patch] runway for GT3 section %08x: %s (%zu bytes)\n", crc, name.c_str(), g_replacements[crc].size());
                }
            }
        // GT3_COURSE_REPLACE=<folder> of `<crc32>.crs`: whole course files (GT3's
        // layout: 48 section offsets, slot 0 = 0), keyed by the CRC32 of the GT3
        // course's runway section (as the runway files are).
        if (const char *dir = std::getenv("GT3_COURSE_REPLACE"); dir && *dir)
            g_replaceFolder = std::filesystem::path(dir).filename().string();
        if (const char *dir = std::getenv("GT3_COURSE_REPLACE"); dir && *dir)
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                const std::string name = entry.path().filename().string();
                if (entry.path().extension() != ".crs" || name.size() != 12)
                    continue;
                const uint32_t crc = static_cast<uint32_t>(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16));
                std::ifstream in(entry.path(), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.size() >= 0x100u && data.size() <= kCourseBuffer && u32(data.data(), 0) == 0u)
                {
                    g_courses[crc] = std::move(data);
                    std::fprintf(stderr, "[course-patch] course for GT3 runway %08x: %s (%zu bytes)\n", crc, name.c_str(), g_courses[crc].size());
                }
            }
        // The loader's first words: addiu sp, sp, -0x80 / sd s2, 0x20(sp).
        if (u32(rdram, kCourseLoader) != 0x27bdff80u || u32(rdram, kCourseLoader + 4u) != 0xffb20020u)
        {
            std::fprintf(stderr, "[course-patch] the course loader is not at 0x%x in this CORE: off\n", kCourseLoader);
            return;
        }
        // Also for GT3_DRIVE_LOG (which course each recorded lap is on) and
        // GT3_AI_LINES (which course's lines the AI cars drive).
        const char *driveLog = std::getenv("GT3_DRIVE_LOG");
        const char *aiLines = std::getenv("GT3_AI_LINES");
        if ((!g_replacements.empty() || !g_courses.empty() || (driveLog && *driveLog) || (aiLines && *aiLines)) &&
            (g_loader = runtime.lookupFunction(kCourseLoader)))
            runtime.registerFunction(kCourseLoader, &loaderHook);
    }

    bool lastCourse(uint32_t &runwayCrc, std::string &replacement)
    {
        std::lock_guard<std::mutex> lock(g_lastMutex);
        runwayCrc = g_lastCrc;
        replacement = g_lastReplacement;
        return g_haveLast;
    }
}
