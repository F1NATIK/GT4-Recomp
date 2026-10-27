#include "gt4_texture_dump.h"
#include "gt4_texture_owner.h"

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if !defined(GT4_STOCK_BUILD)
namespace
{
    struct Pending
    {
        int width = 0, height = 0;
        std::vector<uint32_t> texels;
        std::string info;
        gt4texowner::Sample sample; // the upload into the page at its TBP (its car or course: gt4_texture_owner.h)
        bool race = false;          // first drawn in a race session
        int u0 = 1 << 30, v0 = 1 << 30, u1 = -1, v1 = -1; // the sampled rectangle (u1 < u0: none yet)
        uint64_t lastFrame = 0, grewFrame = 0; // last drawn with; the rectangle last grew
    };

    constexpr uint64_t kQuietFrames = 180;               // written once not drawn with for this many frames (~3 s)
    constexpr uint64_t kSettledFrames = 300;             // or drawn with but its rectangle the same for this many (~5 s)
    constexpr size_t kMaxPendingBytes = 512u << 20;      // kept textures (memory bound): the least recently drawn go first
    constexpr int kNoiseBlock = 8;

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::unordered_map<uint64_t, Pending> g_pending; // all-texel hash -> texture still being drawn with
    size_t g_pendingBytes = 0;
    std::unordered_set<uint64_t> g_known;  // all-texel hashes seen this run
    std::unordered_set<uint64_t> g_saved;  // sampled-rectangle hashes on disk (file names)
    uint64_t g_frame = 0;
    std::thread g_writer;
    bool g_stop = false, g_flushAll = false;
    uint64_t g_written = 0, g_noisy = 0, g_same = 0, g_unsampled = 0;
    bool g_race = false; // a race session is on (new textures: a car's race\, not its showroom\)
    int g_races = 0;
    std::set<std::string> g_carDirs; // car folders written to this run (their contact sheets, at exit)

    const std::string &folder()
    {
        static const std::string f = [] {
            // A folder, or on / 1: texture-dump next to GT4.exe (the launcher's switch); off / 0 / unset: none.
            const std::string v = std::getenv("GT4_TEXTURE_DUMP") ? std::getenv("GT4_TEXTURE_DUMP") : "";
            if (v.empty() || v == "0" || v == "off")
                return std::string();
            return v == "on" || v == "1" ? std::string(GetApplicationDirectory()) + "texture-dump" : v;
        }();
        return f;
    }

    // gt4-<hash>-<w>x<h>.png; a texture sampled in part also names that area (-r<u0>_<v0>_<u1>_<v1>), so the picture
    // alone tells texture replacement (gt4_texture_replace.h) where to look.
    std::string fileName(uint64_t hash, int w, int h, int u0, int v0, int u1, int v1)
    {
        char name[96];
        int n = std::snprintf(name, sizeof(name), "gt4-%016llx-%dx%d", static_cast<unsigned long long>(hash), w, h);
        if (u0 != 0 || v0 != 0 || u1 != w - 1 || v1 != h - 1)
            n += std::snprintf(name + n, sizeof(name) - n, "-r%d_%d_%d_%d", u0, v0, u1, v1);
        std::snprintf(name + n, sizeof(name) - n, ".png");
        return name;
    }

    bool opaqueCopies()
    {
        static const bool on = [] {
            const char *v = std::getenv("GT4_TEXTURE_DUMP_OPAQUE");
            return v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0;
        }();
        return on;
    }

    // Much of the rectangle like random data: over a quarter of its 8 x 8 blocks with a mean neighbour difference
    // (brightness, 0..255) above 60. Real textures are smooth or patterned; leftover GS memory read with the wrong
    // format or palette is not.
    bool looksNoisy(const Pending &p)
    {
        int blocks = 0, noisy = 0;
        auto lum = [&](int x, int y) {
            const uint32_t c = p.texels[static_cast<size_t>(y) * p.width + x];
            return static_cast<int>(((c & 0xFFu) * 2u + ((c >> 8) & 0xFFu) * 5u + ((c >> 16) & 0xFFu)) / 8u);
        };
        for (int by = p.v0; by + kNoiseBlock <= p.v1; by += kNoiseBlock)
            for (int bx = p.u0; bx + kNoiseBlock <= p.u1; bx += kNoiseBlock)
            {
                int sum = 0;
                for (int y = by; y < by + kNoiseBlock; ++y)
                    for (int x = bx; x < bx + kNoiseBlock; ++x)
                        sum += std::abs(lum(x, y) - lum(x + 1, y)) + std::abs(lum(x, y) - lum(x, y + 1));
                ++blocks;
                noisy += sum > 60 * 2 * kNoiseBlock * kNoiseBlock;
            }
        return blocks >= 4 && noisy * 4 > blocks;
    }

    void write(Pending &p)
    {
        if (p.u1 < p.u0 || p.v1 < p.v0)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++g_unsampled;
            return;
        }
        const uint64_t hash = gt4texdump::maskedHash(p.texels.data(), p.width, p.height, p.u0, p.v0, p.u1, p.v1);
        // Outside the sampled rectangle: transparent.
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x)
                if (x < p.u0 || x > p.u1 || y < p.v0 || y > p.v1)
                    p.texels[static_cast<size_t>(y) * p.width + x] = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_saved.insert(hash).second)
            {
                ++g_same; // the same picture with other data outside what GT4 samples
                return;
            }
        }
        const bool noisy = looksNoisy(p);
        // The folder: the car (race\ or showroom\) or course the upload's bytes came from, else other\.
        const gt4texowner::Owner owner = gt4texowner::resolve(p.sample);
        std::filesystem::path sub;
        if (owner.kind == gt4texowner::Owner::Car)
            sub = std::filesystem::path("cars") / owner.folder / (p.race ? "race" : "showroom");
        else if (owner.kind == gt4texowner::Owner::Track)
            sub = std::filesystem::path("tracks") / owner.folder;
        else if (owner.kind == gt4texowner::Owner::Wheels)
            sub = "wheels";
        else
            sub = "other";
        for (uint32_t &t : p.texels)
        {
            const uint32_t a = t >> 24;
            t = (t & 0x00FFFFFFu) | ((a >= 0x80u ? 255u : (a * 255u + 64u) / 128u) << 24);
        }
        const std::filesystem::path dir(folder());
        std::error_code ec;
        std::filesystem::create_directories(dir / sub, ec);
        const std::string name = fileName(hash, p.width, p.height, p.u0, p.v0, p.u1, p.v1);
        const std::filesystem::path rel = sub / name;
        Image image{p.texels.data(), p.width, p.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        if (!ExportImage(image, (dir / rel).string().c_str()))
            return;
        if (opaqueCopies())
        {
            // GT4_TEXTURE_DUMP_OPAQUE: a copy without alpha (inside the sampled area) in an _opaque\ folder next to it,
            // for viewing (texture replacement skips _opaque folders).
            for (int y = p.v0; y <= p.v1; ++y)
                for (int x = p.u0; x <= p.u1; ++x)
                    p.texels[static_cast<size_t>(y) * p.width + x] |= 0xFF000000u;
            std::filesystem::create_directories(dir / sub / "_opaque", ec);
            ExportImage(image, (dir / sub / "_opaque" / name).string().c_str());
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ofstream index(dir / "index.csv", std::ios::app);
        index << rel.generic_string() << "," << p.width << "," << p.height << "," << p.u0 << "," << p.v0 << "," << p.u1 << "," << p.v1 << ","
              << p.info << "," << (noisy ? "noisy" : "") << "," << owner.key << "\n";
        if (owner.kind == gt4texowner::Owner::Car)
            g_carDirs.insert((std::filesystem::path("cars") / owner.folder).string());
        ++g_written;
        g_noisy += noisy;
    }

    void writerLoop()
    {
        for (;;)
        {
            std::vector<Pending> ready;
            bool last = false;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait_for(lock, std::chrono::milliseconds(500), [] { return g_stop || g_flushAll; });
                last = g_stop;
                const bool all = g_stop || g_flushAll;
                g_flushAll = false;
                for (auto it = g_pending.begin(); it != g_pending.end();)
                {
                    if (all || it->second.lastFrame + kQuietFrames < g_frame || (it->second.u1 >= 0 && it->second.grewFrame + kSettledFrames < g_frame))
                    {
                        g_pendingBytes -= it->second.texels.size() * 4u;
                        ready.push_back(std::move(it->second));
                        it = g_pending.erase(it);
                    }
                    else
                        ++it;
                }
                while (g_pendingBytes > kMaxPendingBytes && !g_pending.empty())
                {
                    auto oldest = std::min_element(g_pending.begin(), g_pending.end(),
                                                   [](const auto &a, const auto &b) { return a.second.lastFrame < b.second.lastFrame; });
                    g_pendingBytes -= oldest->second.texels.size() * 4u;
                    ready.push_back(std::move(oldest->second));
                    g_pending.erase(oldest);
                }
            }
            for (Pending &p : ready)
                write(p);
            if (last)
                return;
        }
    }

    // contact-sheet.png in a car's folder: every picture of its race\ and showroom\ folders (this run's and earlier
    // runs'), each fitted into a 128 x 128 cell on a grey checkerboard (its alpha shown), 8 to a row; a red bar
    // before the race pictures, a blue bar before the showroom's.
    void contactSheet(const std::filesystem::path &carDir)
    {
        constexpr int kCell = 128, kGap = 4, kColumns = 8, kBar = 6;
        std::vector<std::vector<std::filesystem::path>> parts(2);
        std::error_code ec;
        for (int i = 0; i < 2; ++i)
        {
            for (const auto &e : std::filesystem::directory_iterator(carDir / (i == 0 ? "race" : "showroom"), ec))
            {
                const std::string n = e.path().filename().string();
                if (e.is_regular_file(ec) && n.rfind("gt4-", 0) == 0 && e.path().extension() == ".png")
                    parts[i].push_back(e.path());
            }
            std::sort(parts[i].begin(), parts[i].end());
            ec.clear();
        }
        if (parts[0].empty() && parts[1].empty())
            return;
        int height = kGap;
        for (const auto &list : parts)
            if (!list.empty())
                height += kBar + kGap + static_cast<int>((list.size() + kColumns - 1) / kColumns) * (kCell + kGap);
        const int width = kGap + kColumns * (kCell + kGap);
        Image sheet = GenImageChecked(width, height, 8, 8, Color{150, 150, 150, 255}, Color{110, 110, 110, 255});
        int y = kGap;
        for (int i = 0; i < 2; ++i)
        {
            if (parts[i].empty())
                continue;
            ImageDrawRectangle(&sheet, kGap, y, width - 2 * kGap, kBar, i == 0 ? Color{200, 40, 40, 255} : Color{40, 90, 210, 255});
            y += kBar + kGap;
            for (size_t k = 0; k < parts[i].size(); ++k)
            {
                Image pic = LoadImage(parts[i][k].string().c_str());
                if (!pic.data)
                    continue;
                ImageFormat(&pic, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                const float scale = std::min(static_cast<float>(kCell) / pic.width, static_cast<float>(kCell) / pic.height);
                const float w = pic.width * scale, h = pic.height * scale;
                const float x0 = static_cast<float>(kGap + static_cast<int>(k % kColumns) * (kCell + kGap)) + (kCell - w) / 2;
                const float y0 = static_cast<float>(y + static_cast<int>(k / kColumns) * (kCell + kGap)) + (kCell - h) / 2;
                ImageDraw(&sheet, pic, Rectangle{0, 0, static_cast<float>(pic.width), static_cast<float>(pic.height)}, Rectangle{x0, y0, w, h}, WHITE);
                UnloadImage(pic);
            }
            y += static_cast<int>((parts[i].size() + kColumns - 1) / kColumns) * (kCell + kGap);
        }
        ExportImage(sheet, (carDir / "contact-sheet.png").string().c_str());
        UnloadImage(sheet);
    }

    // First use: the folder, the pictures already there (any subfolder; not written again), the writer thread.
    bool start()
    {
        static const bool started = [] {
            if (folder().empty())
                return false;
            std::error_code ec;
            std::filesystem::create_directories(folder(), ec);
            for (auto it = std::filesystem::recursive_directory_iterator(folder(), ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                unsigned long long hash = 0;
                if (name.size() > 20 && std::sscanf(name.c_str(), "gt4-%16llx-", &hash) == 1)
                    g_saved.insert(hash);
            }
            if (!std::filesystem::exists(std::filesystem::path(folder()) / "index.csv"))
            {
                std::ofstream index(std::filesystem::path(folder()) / "index.csv");
                index << "file,width,height,u0,v0,u1,v1,format,tbp,tbw,cbp,csa,frame,check,owner\n";
            }
            std::cerr << "[texture-dump] into " << folder() << " (" << g_saved.size() << " already there)" << std::endl;
            gt4texowner::start();
            g_writer = std::thread(writerLoop);
            return true;
        }();
        return started;
    }
}

bool gt4texdump::enabled() { return !folder().empty(); }

uint64_t gt4texdump::maskedHash(const uint32_t *texels, int width, int height, int u0, int v0, int u1, int v1)
{
    uint64_t h = 0xCBF29CE484222325ull ^ (static_cast<uint64_t>(width) << 32) ^ static_cast<uint64_t>(height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const uint32_t t = x < u0 || x > u1 || y < v0 || y > v1 ? 0u : texels[static_cast<size_t>(y) * width + x];
            h = (h ^ t) * 0x100000001B3ull;
        }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

bool gt4texdump::known(uint64_t hash)
{
    if (!start())
        return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_known.count(hash) != 0;
}

void gt4texdump::submit(uint64_t hash, int width, int height, std::vector<uint32_t> &&texels, const std::string &info)
{
    if (!start() || width <= 0 || height <= 0 || texels.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_known.insert(hash).second)
        return;
    Pending &p = g_pending[hash];
    p.width = width;
    p.height = height;
    p.texels = std::move(texels);
    p.info = info;
    unsigned tbp = 0; // info: "psm<psm>,<tbp>,..." (gs_gl_backend.cpp dumpTexture)
    if (std::sscanf(info.c_str(), "psm%*x,%x", &tbp) == 1)
        p.sample = gt4texowner::sampleAt(tbp);
    p.race = g_race;
    p.lastFrame = p.grewFrame = g_frame;
    g_pendingBytes += p.texels.size() * 4u;
}

void gt4texdump::touch(uint64_t hash, int u0, int v0, int u1, int v1, uint64_t frame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frame = std::max(g_frame, frame);
    auto it = g_pending.find(hash);
    if (it == g_pending.end())
        return;
    Pending &p = it->second;
    const int a0 = std::min(p.u0, std::max(u0, 0)), b0 = std::min(p.v0, std::max(v0, 0));
    const int a1 = std::max(p.u1, std::min(u1, p.width - 1)), b1 = std::max(p.v1, std::min(v1, p.height - 1));
    if (a0 != p.u0 || b0 != p.v0 || a1 != p.u1 || b1 != p.v1)
        p.grewFrame = frame;
    p.u0 = a0;
    p.v0 = b0;
    p.u1 = a1;
    p.v1 = b1;
    p.lastFrame = frame;
}

void gt4texdump::setRace(bool race)
{
    if (!enabled())
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_race = race;
        if (race)
            std::fprintf(stderr, "[texture-dump] scene: race %d\n", ++g_races);
        else
            std::fprintf(stderr, "[texture-dump] scene: menus\n");
        g_flushAll = true; // the last scene's textures written now
    }
    g_wake.notify_one();
}

uint64_t gt4texdump::written()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_written;
}

void gt4texdump::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_writer.joinable())
    {
        g_writer.join();
        gt4texowner::shutdown();
        for (const std::string &car : g_carDirs)
            contactSheet(std::filesystem::path(folder()) / car);
        std::fprintf(stderr, "[texture-dump] written %llu (flagged noisy %llu), same picture as one saved %llu, never sampled %llu\n",
                     static_cast<unsigned long long>(g_written), static_cast<unsigned long long>(g_noisy),
                     static_cast<unsigned long long>(g_same), static_cast<unsigned long long>(g_unsampled));
    }
}
#else
bool gt4texdump::enabled() { return false; }
uint64_t gt4texdump::maskedHash(const uint32_t *, int, int, int, int, int, int) { return 0; }
bool gt4texdump::known(uint64_t) { return true; }
void gt4texdump::submit(uint64_t, int, int, std::vector<uint32_t> &&, const std::string &) {}
void gt4texdump::touch(uint64_t, int, int, int, int, uint64_t) {}
void gt4texdump::setRace(bool) {}
uint64_t gt4texdump::written() { return 0; }
void gt4texdump::shutdown() {}
#endif
