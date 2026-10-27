#include "gt3_texture_dump.h"

#include "raylib.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    struct Job
    {
        uint64_t hash;
        int width, height;
        std::vector<uint32_t> texels;
        std::string info;
    };

    constexpr size_t kMaxWaiting = 256; // textures waiting to be written (memory bound)

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::deque<Job> g_jobs;
    std::unordered_set<uint64_t> g_known;
    std::thread g_writer;
    bool g_stop = false;
    std::atomic<uint64_t> g_written{0};

    const std::string &folder()
    {
        static const std::string f = [] {
            const char *v = std::getenv("GT3_TEXTURE_DUMP");
            return std::string(v && *v && std::strcmp(v, "0") != 0 ? v : "");
        }();
        return f;
    }

    std::string fileName(uint64_t hash, int w, int h)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "gt3-%016llx-%dx%d.png", static_cast<unsigned long long>(hash), w, h);
        return name;
    }

    void writerLoop()
    {
        const std::filesystem::path dir(folder());
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait(lock, [] { return g_stop || !g_jobs.empty(); });
                if (g_jobs.empty())
                    return; // stopping, nothing left
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
            }
            // PS2 alpha (0x80 opaque) to 0..255.
            for (uint32_t &t : job.texels)
            {
                const uint32_t a = t >> 24;
                t = (t & 0x00FFFFFFu) | ((a >= 0x80u ? 255u : (a * 255u + 64u) / 128u) << 24);
            }
            Image image{job.texels.data(), job.width, job.height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            const std::string path = (dir / fileName(job.hash, job.width, job.height)).string();
            if (ExportImage(image, path.c_str()))
            {
                g_written.fetch_add(1, std::memory_order_relaxed);
                std::ofstream index(dir / "index.csv", std::ios::app);
                index << fileName(job.hash, job.width, job.height) << "," << job.width << "," << job.height << "," << job.info << "\n";
            }
        }
    }

    // First use: the folder, the textures already there (not written again),
    // the writer thread.
    bool start()
    {
        static const bool started = [] {
            if (folder().empty())
                return false;
            std::error_code ec;
            std::filesystem::create_directories(folder(), ec);
            size_t existing = 0;
            for (const auto &entry : std::filesystem::directory_iterator(folder(), ec))
            {
                const std::string name = entry.path().filename().string();
                unsigned long long hash = 0;
                if (name.size() > 20 && std::sscanf(name.c_str(), "gt3-%16llx-", &hash) == 1)
                {
                    g_known.insert(hash);
                    ++existing;
                }
            }
            if (!std::filesystem::exists(std::filesystem::path(folder()) / "index.csv"))
            {
                std::ofstream index(std::filesystem::path(folder()) / "index.csv");
                index << "file,width,height,format,tbp,tbw,cbp,csa,frame\n";
            }
            std::cerr << "[texture-dump] into " << folder() << " (" << existing << " already there)" << std::endl;
            g_writer = std::thread(writerLoop);
            return true;
        }();
        return started;
    }
}

bool gt3texdump::enabled() { return !folder().empty(); }

bool gt3texdump::known(uint64_t hash)
{
    if (!start())
        return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_known.count(hash) != 0;
}

void gt3texdump::submit(uint64_t hash, int width, int height, std::vector<uint32_t> &&texels, const std::string &info)
{
    if (!start() || width <= 0 || height <= 0 || texels.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_known.count(hash) || g_jobs.size() >= kMaxWaiting)
            return;
        g_known.insert(hash);
        g_jobs.push_back(Job{hash, width, height, std::move(texels), info});
    }
    g_wake.notify_one();
}

uint64_t gt3texdump::written() { return g_written.load(std::memory_order_relaxed); }

void gt3texdump::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_writer.joinable())
        g_writer.join();
}
