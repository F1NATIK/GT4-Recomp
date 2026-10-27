#include "gt4_texture_replace.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if !defined(GT4_STOCK_BUILD)
namespace
{
    constexpr int kMaxSide = 4096; // larger pictures are scaled down to this

    enum class State
    {
        Idle,    // not asked for yet
        Loading, // queued or being read
        Ready,   // read, waiting for take()
        Taken,   // handed over
        Failed,  // could not be read
    };
    struct Entry
    {
        std::string path;
        State state = State::Idle;
        int width = 0, height = 0;
        std::vector<uint32_t> rgba;
    };

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::unordered_map<uint64_t, Entry> g_entries;
    std::map<std::pair<int, int>, std::vector<gt4texreplace::Rect>> g_rects; // (width, height) -> sampled areas named
    std::deque<uint64_t> g_queue;
    std::thread g_loader;
    bool g_stop = false;

    const std::string &folder()
    {
        static const std::string f = [] {
            // A folder, or on / 1: texture-replace next to GT4.exe (the launcher's switch); off / 0 / unset: none.
            const std::string v = std::getenv("GT4_TEXTURE_REPLACE") ? std::getenv("GT4_TEXTURE_REPLACE") : "";
            if (v.empty() || v == "0" || v == "off")
                return std::string();
            return v == "on" || v == "1" ? std::string(GetApplicationDirectory()) + "texture-replace" : v;
        }();
        return f;
    }

    // "gt4-<16 hex>-<w>x<h>" and an optional "-r<u0>_<v0>_<u1>_<v1>" anywhere in the file name (upper or lower case).
    bool parse(std::string name, uint64_t &hash, int &w, int &h, gt4texreplace::Rect &r)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (size_t at = name.find("gt4-"); at != std::string::npos; at = name.find("gt4-", at + 1))
        {
            unsigned long long hv = 0;
            int n = 0;
            if (std::sscanf(name.c_str() + at, "gt4-%16llx-%dx%d%n", &hv, &w, &h, &n) != 3 || n < 4 + 16 + 4 || w <= 0 || h <= 0)
                continue;
            // GT4's sizes are powers of two up to 1024: digits an upscaler added straight after the name ("64x64" +
            // "4x" = "64x644x.png") are dropped from the height (and from the area's last number below).
            auto pow2 = [](int v) { return v > 0 && v <= 1024 && (v & (v - 1)) == 0; };
            while (h > 0 && !pow2(h))
                h /= 10;
            if (!pow2(w) || !pow2(h))
                continue;
            hash = hv;
            r = {0, 0, w - 1, h - 1};
            gt4texreplace::Rect q{};
            if (std::sscanf(name.c_str() + at + n, "-r%d_%d_%d_%d", &q.u0, &q.v0, &q.u1, &q.v1) == 4)
            {
                while (q.v1 >= h)
                    q.v1 /= 10;
                if (q.u0 >= 0 && q.v0 >= 0 && q.u1 >= q.u0 && q.v1 >= q.v0 && q.u1 < w)
                    r = q;
            }
            return true;
        }
        return false;
    }

    void loaderLoop()
    {
        for (;;)
        {
            uint64_t hash = 0;
            std::string path;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
                if (g_stop)
                    return;
                hash = g_queue.front();
                g_queue.pop_front();
                path = g_entries[hash].path;
            }
            Image image = LoadImage(path.c_str());
            int w = 0, h = 0;
            std::vector<uint32_t> rgba;
            if (image.data && image.width > 0 && image.height > 0)
            {
                ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                if (image.width > kMaxSide || image.height > kMaxSide)
                {
                    const float k = static_cast<float>(kMaxSide) / static_cast<float>(std::max(image.width, image.height));
                    ImageResize(&image, std::max(1, static_cast<int>(image.width * k)), std::max(1, static_cast<int>(image.height * k)));
                }
                w = image.width;
                h = image.height;
                rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
                std::memcpy(rgba.data(), image.data, rgba.size() * sizeof(uint32_t));
                // 0..255 alpha back to the PS2's 0..0x80 (the dump's rule reversed).
                for (uint32_t &t : rgba)
                    t = (t & 0x00FFFFFFu) | ((((t >> 24) * 128u + 127u) / 255u) << 24);
            }
            UnloadImage(image);
            std::lock_guard<std::mutex> lock(g_mutex);
            Entry &e = g_entries[hash];
            if (rgba.empty())
            {
                e.state = State::Failed;
                std::cerr << "[texture-replace] could not read " << path << std::endl;
            }
            else
            {
                e.state = State::Ready;
                e.width = w;
                e.height = h;
                e.rgba = std::move(rgba);
            }
        }
    }

    // First use: the folder's replacements (by hash; the first file wins), their sampled areas, the loader thread.
    bool start()
    {
        static const bool started = [] {
            if (folder().empty())
                return false;
            std::error_code ec;
            std::filesystem::create_directories(folder(), ec);
            size_t found = 0;
            for (auto it = std::filesystem::recursive_directory_iterator(folder(), ec); !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (it->is_directory(ec) && it->path().filename() == "_opaque")
                {
                    it.disable_recursion_pending(); // the dump's copies without alpha (GT4_TEXTURE_DUMP_OPAQUE): not replacements
                    continue;
                }
                if (!it->is_regular_file(ec))
                    continue;
                uint64_t hash = 0;
                int w = 0, h = 0;
                gt4texreplace::Rect r{};
                if (!parse(it->path().filename().string(), hash, w, h, r) || g_entries.count(hash))
                    continue;
                g_entries[hash].path = it->path().string();
                auto &list = g_rects[{w, h}];
                // The whole texture is always tried too (first), then the named area.
                for (const gt4texreplace::Rect &a : {gt4texreplace::Rect{0, 0, w - 1, h - 1}, r})
                    if (std::none_of(list.begin(), list.end(),
                                     [&](const gt4texreplace::Rect &q) { return q.u0 == a.u0 && q.v0 == a.v0 && q.u1 == a.u1 && q.v1 == a.v1; }))
                        list.push_back(a);
                ++found;
            }
            std::cerr << "[texture-replace] " << found << " replacements in " << folder() << std::endl;
            if (found == 0)
                return false;
            g_loader = std::thread(loaderLoop);
            return true;
        }();
        return started;
    }
}

bool gt4texreplace::enabled() { return start(); }

const std::vector<gt4texreplace::Rect> &gt4texreplace::rects(int width, int height)
{
    static const std::vector<Rect> none;
    if (!start())
        return none;
    auto it = g_rects.find({width, height}); // filled once by start(), read only afterwards
    return it == g_rects.end() ? none : it->second;
}

bool gt4texreplace::has(uint64_t hash)
{
    if (!start())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_entries.count(hash) != 0;
}

bool gt4texreplace::take(uint64_t hash, int &width, int &height, std::vector<uint32_t> &rgba)
{
    if (!start())
        return false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_entries.find(hash);
        if (it == g_entries.end())
            return false;
        Entry &e = it->second;
        switch (e.state)
        {
        case State::Idle:
            e.state = State::Loading;
            g_queue.push_back(hash);
            queued = true;
            break;
        case State::Ready:
            width = e.width;
            height = e.height;
            rgba = std::move(e.rgba);
            e.rgba.clear();
            e.state = State::Taken;
            return true;
        default:
            break;
        }
    }
    if (queued)
        g_wake.notify_one();
    return false;
}

void gt4texreplace::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_loader.joinable())
    {
        g_loader.join();
        size_t used = 0;
        for (const auto &e : g_entries)
            used += e.second.state == State::Taken;
        std::cerr << "[texture-replace] used this run: " << used << " of " << g_entries.size() << " replacements" << std::endl;
    }
}
#else
bool gt4texreplace::enabled() { return false; }
const std::vector<gt4texreplace::Rect> &gt4texreplace::rects(int, int)
{
    static const std::vector<Rect> none;
    return none;
}
bool gt4texreplace::has(uint64_t) { return false; }
bool gt4texreplace::take(uint64_t, int &, int &, std::vector<uint32_t> &) { return false; }
void gt4texreplace::shutdown() {}
#endif
