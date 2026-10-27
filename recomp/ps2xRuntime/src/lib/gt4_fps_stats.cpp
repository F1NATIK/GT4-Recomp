#include "gt4_fps_stats.h"

// The PC numbers of GT4's detailed FPS counter (gt4_fps_stats.h). No raylib here: <windows.h> and
// raylib.h do not mix.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace gt4fps::stats
{
    namespace
    {
        std::mutex g_mutex;
        Sample g_latest;
        std::atomic<bool> g_want{false};
        // GT4's graphics card (LUID high / low), from the presenter's OpenGL context; 0 / 0 until known.
        std::atomic<bool> g_luidNoted{false};
        std::atomic<bool> g_luidValid{false};
        std::atomic<int32_t> g_luidHigh{0};
        std::atomic<uint32_t> g_luidLow{0};

#if defined(_WIN32)
        uint64_t fileTime(const FILETIME &f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

        // "luid_0x00000000_0x0000D1F2" as the GPU counters' instance names spell a card.
        std::wstring luidText(int32_t high, uint32_t low)
        {
            wchar_t b[64];
            std::swprintf(b, 64, L"luid_0x%08X_0x%08X", static_cast<uint32_t>(high), low);
            return b;
        }

        struct Sampler
        {
            // CPU
            uint64_t sysIdle = 0, sysKernel = 0, sysUser = 0, procKernel = 0, procUser = 0, wall = 0;
            bool cpuPrimed = false;
            unsigned processors = 1;
            // GPU / VRAM counters
            PDH_HQUERY query = nullptr;
            PDH_HCOUNTER engine = nullptr, adapterMemory = nullptr;
            bool pdhPrimed = false;
            // DXGI
            IDXGIAdapter3 *adapter = nullptr;
            bool adapterTried = false;
            bool adapterFromLuid = false;
            bool luidChecked = false; // a pick made with the OpenGL context's LUID known (matched or not)
            int32_t luidHigh = 0;
            uint32_t luidLow = 0;
            double dedicatedMb = 0;
            std::string name;
            std::wstring pidText;

            Sampler()
            {
                processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
                wchar_t b[32];
                std::swprintf(b, 32, L"pid_%lu_", static_cast<unsigned long>(GetCurrentProcessId()));
                pidText = b;
                if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS)
                {
                    if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine) != ERROR_SUCCESS)
                        engine = nullptr;
                    if (PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &adapterMemory) != ERROR_SUCCESS)
                        adapterMemory = nullptr;
                    if (!engine && !adapterMemory)
                    {
                        PdhCloseQuery(query);
                        query = nullptr;
                    }
                }
            }

            ~Sampler()
            {
                if (adapter)
                    adapter->Release();
                if (query)
                    PdhCloseQuery(query);
            }

            // GT4's card: by the OpenGL context's LUID once known, else the one with the most dedicated memory.
            void pickAdapter()
            {
                const bool luidNow = g_luidValid.load();
                if (adapterTried && (adapterFromLuid || !luidNow || luidChecked))
                    return;
                adapterTried = true;
                luidChecked = luidNow;
                IDXGIFactory1 *factory = nullptr;
                if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
                    return;
                IDXGIAdapter1 *best = nullptr;
                SIZE_T bestMemory = 0;
                bool bestByLuid = false;
                for (UINT i = 0;; ++i)
                {
                    IDXGIAdapter1 *a = nullptr;
                    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a)
                        break;
                    DXGI_ADAPTER_DESC1 d{};
                    a->GetDesc1(&d);
                    const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                    const bool byLuid = luidNow && d.AdapterLuid.HighPart == g_luidHigh.load() && d.AdapterLuid.LowPart == g_luidLow.load();
                    if (!software && (byLuid || (!bestByLuid && d.DedicatedVideoMemory > bestMemory)))
                    {
                        if (best)
                            best->Release();
                        best = a;
                        bestMemory = d.DedicatedVideoMemory;
                        bestByLuid = byLuid;
                        continue;
                    }
                    a->Release();
                }
                factory->Release();
                if (!best)
                    return;
                if (adapter)
                {
                    adapter->Release();
                    adapter = nullptr;
                }
                IDXGIAdapter3 *a3 = nullptr;
                if (SUCCEEDED(best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&a3))))
                    adapter = a3;
                DXGI_ADAPTER_DESC1 d{};
                best->GetDesc1(&d);
                best->Release();
                adapterFromLuid = bestByLuid;
                luidHigh = d.AdapterLuid.HighPart;
                luidLow = d.AdapterLuid.LowPart;
                dedicatedMb = static_cast<double>(d.DedicatedVideoMemory) / (1024.0 * 1024.0);
                char n[256];
                const int len = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof(n), nullptr, nullptr);
                name = len > 0 ? std::string(n) : std::string();
            }

            // The formatted values of a wildcard counter, by instance name.
            static bool values(PDH_HCOUNTER c, std::vector<std::pair<std::wstring, double>> &out)
            {
                out.clear();
                DWORD bytes = 0, count = 0;
                PDH_STATUS s = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
                if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bytes == 0)
                    return false;
                std::vector<unsigned char> buf(bytes);
                auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
                s = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
                if (s != ERROR_SUCCESS)
                    return false;
                for (DWORD i = 0; i < count; ++i)
                    if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
                        out.emplace_back(items[i].szName ? items[i].szName : L"", items[i].FmtValue.doubleValue);
                return true;
            }

            void sample(Sample &s)
            {
                // CPU: the whole PC (kernel time includes idle) and GT4's process, over the time since the last sample.
                FILETIME idle{}, kernel{}, user{}, created{}, exited{}, pk{}, pu{}, now{};
                GetSystemTimeAsFileTime(&now);
                if (GetSystemTimes(&idle, &kernel, &user) && GetProcessTimes(GetCurrentProcess(), &created, &exited, &pk, &pu))
                {
                    const uint64_t i = fileTime(idle), k = fileTime(kernel), u = fileTime(user), ppk = fileTime(pk), ppu = fileTime(pu), w = fileTime(now);
                    if (cpuPrimed)
                    {
                        const double total = static_cast<double>((k - sysKernel) + (u - sysUser));
                        const double busy = total - static_cast<double>(i - sysIdle);
                        const double span = static_cast<double>(w - wall) * processors;
                        if (total > 0.0 && span > 0.0)
                        {
                            s.cpuAll = static_cast<float>(std::clamp(100.0 * busy / total, 0.0, 100.0));
                            s.cpuGt4 = static_cast<float>(std::clamp(100.0 * static_cast<double>((ppk - procKernel) + (ppu - procUser)) / span, 0.0, 100.0));
                            s.cpuValid = true;
                        }
                    }
                    sysIdle = i;
                    sysKernel = k;
                    sysUser = u;
                    procKernel = ppk;
                    procUser = ppu;
                    wall = w;
                    cpuPrimed = true;
                }
                // RAM: GT4's working set, the PC's in use.
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof(pm);
                MEMORYSTATUSEX ms{};
                ms.dwLength = sizeof(ms);
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pm), sizeof(pm)) && GlobalMemoryStatusEx(&ms))
                {
                    constexpr double kMb = 1024.0 * 1024.0;
                    s.ramGt4Mb = static_cast<double>(pm.WorkingSetSize) / kMb;
                    s.ramTotalMb = static_cast<double>(ms.ullTotalPhys) / kMb;
                    s.ramUsedMb = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / kMb;
                    s.ramValid = true;
                }
                // The card, GT4's video memory on it.
                pickAdapter();
                s.gpuName = name;
                if (adapter)
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
                    if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                    {
                        s.vramGt4Mb = static_cast<double>(local.CurrentUsage) / (1024.0 * 1024.0);
                        s.vramTotalMb = dedicatedMb;
                        s.vramValid = true;
                    }
                }
                // The GPU counters: the card's busiest engine type (3D, copy, video ...), all programs and GT4's share.
                if (query && PdhCollectQueryData(query) == ERROR_SUCCESS)
                {
                    if (!pdhPrimed)
                    {
                        pdhPrimed = true; // a rate needs two samples
                        return;
                    }
                    const std::wstring card = adapter ? luidText(luidHigh, luidLow) : std::wstring();
                    std::vector<std::pair<std::wstring, double>> v;
                    if (engine && values(engine, v))
                    {
                        std::map<std::wstring, double> all, ours;
                        for (const auto &[inst, val] : v)
                        {
                            if (!card.empty() && inst.find(card) == std::wstring::npos)
                                continue;
                            const size_t at = inst.find(L"engtype_");
                            const std::wstring type = at == std::wstring::npos ? L"" : inst.substr(at + 8);
                            all[type] += val;
                            if (inst.compare(0, pidText.size(), pidText) == 0)
                                ours[type] += val;
                        }
                        double a = 0.0, o = 0.0;
                        for (const auto &[type, val] : all)
                            a = std::max(a, val);
                        for (const auto &[type, val] : ours)
                            o = std::max(o, val);
                        s.gpuAll = static_cast<float>(std::clamp(a, 0.0, 100.0));
                        s.gpuGt4 = static_cast<float>(std::clamp(o, 0.0, 100.0));
                        s.gpuValid = true;
                    }
                    if (adapterMemory && !card.empty() && values(adapterMemory, v))
                    {
                        double used = 0.0;
                        bool any = false;
                        for (const auto &[inst, val] : v)
                            if (inst.find(card) != std::wstring::npos)
                            {
                                used += val;
                                any = true;
                            }
                        if (any)
                            s.vramUsedMb = used / (1024.0 * 1024.0);
                    }
                }
            }
        };
#endif

        struct Worker
        {
            std::thread thread;
            std::mutex m;
            std::condition_variable cv;
            bool quit = false;

            void start()
            {
                thread = std::thread([this] {
#if defined(_WIN32)
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    Sampler sampler;
#endif
                    std::unique_lock<std::mutex> lock(m);
                    while (!quit)
                    {
                        if (g_want.load())
                        {
                            lock.unlock();
#if defined(_WIN32)
                            Sample s;
                            sampler.sample(s);
                            {
                                std::lock_guard<std::mutex> g(g_mutex);
                                g_latest = s;
                            }
#endif
                            lock.lock();
                        }
                        cv.wait_for(lock, std::chrono::milliseconds(250), [this] { return quit; });
                    }
                });
            }

            ~Worker()
            {
                if (!thread.joinable())
                    return;
                {
                    std::lock_guard<std::mutex> lock(m);
                    quit = true;
                }
                cv.notify_all();
                thread.join();
            }
        };

        Worker &worker()
        {
            static Worker w;
            return w;
        }
    }

    void want(bool on)
    {
        g_want.store(on);
        if (!on)
            return;
        static std::once_flag started;
        std::call_once(started, [] { worker().start(); });
    }

    Sample latest()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_latest;
    }

    void noteRenderGpu()
    {
        if (g_luidNoted.exchange(true))
            return;
#if defined(_WIN32)
        // glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT) (GL_EXT_memory_object_win32), as the Vulkan renderer uses to pick the same card.
        using GetUnsignedBytev = void(APIENTRY *)(unsigned int, unsigned char *);
        const auto fn = reinterpret_cast<GetUnsignedBytev>(reinterpret_cast<void *>(wglGetProcAddress("glGetUnsignedBytevEXT")));
        if (!fn || !wglGetCurrentContext())
            return;
        unsigned char luid[8]{};
        fn(0x9599u /* GL_DEVICE_LUID_EXT */, luid);
        uint32_t low = 0;
        int32_t high = 0;
        std::memcpy(&low, luid, 4);
        std::memcpy(&high, luid + 4, 4);
        if (low == 0 && high == 0)
            return;
        g_luidLow.store(low);
        g_luidHigh.store(high);
        g_luidValid.store(true);
#endif
    }
}
