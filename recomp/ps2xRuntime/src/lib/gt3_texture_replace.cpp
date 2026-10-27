#include "gt3_texture_replace.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
    std::deque<uint64_t> g_queue;
    std::thread g_loader;
    bool g_stop = false;

    const std::string &folder()
    {
        static const std::string f = [] {
            const char *v = std::getenv("GT3_TEXTURE_REPLACE");
            return std::string(v && *v && std::strcmp(v, "0") != 0 ? v : "");
        }();
        return f;
    }

    // "gt3-" and 16 hex digits anywhere in the file name.
    bool hashOf(const std::string &name, uint64_t &hash)
    {
        for (size_t at = name.find("gt3-"); at != std::string::npos; at = name.find("gt3-", at + 1))
        {
            if (at + 4 + 16 > name.size())
                return false;
            uint64_t h = 0;
            bool ok = true;
            for (size_t i = 0; i < 16 && ok; ++i)
            {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[at + 4 + i])));
                if (c >= '0' && c <= '9')
                    h = (h << 4) | static_cast<uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    h = (h << 4) | static_cast<uint64_t>(c - 'a' + 10);
                else
                    ok = false;
            }
            if (ok)
            {
                hash = h;
                return true;
            }
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
                // 0..255 alpha to the PS2's 0..0x80.
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

    // First use: the folder's replacements (by hash; the first file wins),
    // the loader thread.
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
                if (!it->is_regular_file(ec))
                    continue;
                uint64_t hash = 0;
                if (!hashOf(it->path().filename().string(), hash) || g_entries.count(hash))
                    continue;
                g_entries[hash].path = it->path().string();
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

bool gt3texreplace::enabled() { return start(); }

bool gt3texreplace::has(uint64_t hash)
{
    if (!start())
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_entries.count(hash) != 0;
}

bool gt3texreplace::take(uint64_t hash, int &width, int &height, std::vector<uint32_t> &rgba)
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

size_t gt3texreplace::count()
{
    if (!start())
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_entries.size();
}

void gt3texreplace::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_loader.joinable())
        g_loader.join();
}
