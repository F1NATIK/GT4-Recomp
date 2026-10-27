#include "gt4_mirror.h"
#include "gt4_hud_pictures.h"
#include "gt4_hud_move.h"

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include "external/stb_image_write.h" // (raylib builds its implementation, the JPEG writer included)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace gt4mirror
{
    namespace
    {
        std::atomic<int> g_viewers{0};
        // The rectangle GT4 drew its mirror at (EE thread -> GS thread), and how many frames noted it.
        std::mutex g_rectMutex;
        int g_rx = 0, g_ry = 0, g_rw = 0, g_rh = 0;
        uint64_t g_noted = 0, g_taken = 0;
        // The newest picture read back (GS thread -> encoder).
        std::mutex g_rawMutex;
        std::condition_variable g_rawReady;
        std::vector<uint8_t> g_raw;
        int g_rawW = 0, g_rawH = 0;
        bool g_rawNew = false;
        std::once_flag g_encoderStarted;
        // The newest JPEG (encoder -> viewers).
        std::mutex g_jpegMutex;
        std::condition_variable g_jpegReady;
        std::shared_ptr<const std::vector<uint8_t>> g_jpeg;
        uint64_t g_jpegSerial = 0;
        std::chrono::steady_clock::time_point g_jpegTime;

        void appendBytes(void *context, void *data, int size)
        {
            auto *out = static_cast<std::vector<uint8_t> *>(context);
            const auto *p = static_cast<const uint8_t *>(data);
            out->insert(out->end(), p, p + size);
        }

        // Up to 30 JPEGs a second (GT4_MIRROR_QUALITY 30..95, default 80), from the newest picture each time.
        void encodeLoop()
        {
            static const int quality = [] {
                const char *v = std::getenv("GT4_MIRROR_QUALITY");
                const int q = v && *v ? std::atoi(v) : 80;
                return q < 30 ? 30 : q > 95 ? 95 : q;
            }();
            std::vector<uint8_t> raw;
            auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            for (;;)
            {
                int w = 0, h = 0;
                {
                    std::unique_lock<std::mutex> lock(g_rawMutex);
                    g_rawReady.wait(lock, [] { return g_rawNew; });
                    raw.swap(g_raw);
                    w = g_rawW, h = g_rawH;
                    g_rawNew = false;
                }
                const auto next = last + std::chrono::milliseconds(33);
                if (std::chrono::steady_clock::now() < next)
                    std::this_thread::sleep_until(next);
                last = std::chrono::steady_clock::now();
                if (w <= 0 || h <= 0 || raw.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u)
                    continue;
                auto jpeg = std::make_shared<std::vector<uint8_t>>();
                jpeg->reserve(static_cast<size_t>(w) * static_cast<size_t>(h) / 4u);
                if (!stbi_write_jpg_to_func(&appendBytes, jpeg.get(), w, h, 4, raw.data(), quality))
                    continue;
                {
                    std::lock_guard<std::mutex> lock(g_jpegMutex);
                    g_jpeg = std::move(jpeg);
                    ++g_jpegSerial;
                    g_jpegTime = std::chrono::steady_clock::now();
                }
                g_jpegReady.notify_all();
                static bool said = false;
                if (!said)
                    std::fprintf(stderr, "[gt4-mirror] first picture of the rear-view mirror: %d x %d, JPEG quality %d\n", w, h, quality), said = true;
            }
        }
    } // namespace

    void viewerJoin()
    {
        if (g_viewers.fetch_add(1) == 0)
            std::fprintf(stderr, "[gt4-mirror] a viewer opened the rear-view mirror page: capturing\n");
    }
    void viewerLeave()
    {
        if (g_viewers.fetch_sub(1) == 1)
            std::fprintf(stderr, "[gt4-mirror] no viewer of the rear-view mirror page: not capturing\n");
    }
    namespace
    {
        std::atomic<int64_t> g_stillAt{INT64_MIN / 2}; // (steady clock ms of the last /mirror.jpg)
        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    } // namespace
    void stillAsked() { g_stillAt.store(nowMs(), std::memory_order_relaxed); }
    bool wanted() { return g_viewers.load(std::memory_order_relaxed) > 0 || nowMs() - g_stillAt.load(std::memory_order_relaxed) < 2000; }

    namespace
    {
        std::atomic<uint32_t> g_cam{0}; // (EE: the player's on-board camera, its view read as the mirror is drawn)
    } // namespace
    void setCamera(uint32_t cam) { g_cam.store(cam, std::memory_order_relaxed); }
    bool forcing() { return mode() != 0 || wanted(); }
    int scissorKind(int x0, int y0, int x1, int y1)
    {
        std::lock_guard<std::mutex> lock(g_rectMutex);
        if (!g_noted || x0 != g_rx || x1 != g_rx + g_rw - 1 || y0 != g_ry)
            return 0;
        return y1 == g_ry + g_rh - 1 ? 1 : y1 == g_ry + g_rh - 2 ? 2 : 0;
    }

    void noteDrawn(uint8_t *rdram, uint32_t rect, uint64_t vsync)
    {
        // (GT4_MIRROR_LOG=1: the vsyncs GT4 drew no mirror in, as they end)
        static const bool log = [] {
            const char *v = std::getenv("GT4_MIRROR_LOG");
            return v && *v == '1';
        }();
        static uint64_t lastVsync = 0;
        static int gapsSaid = 0;
        if (log && lastVsync && vsync > lastVsync + 3u && gapsSaid < 300)
            ++gapsSaid, std::fprintf(stderr, "[gt4-mirror] no mirror drawn from vsync %llu to %llu\n", static_cast<unsigned long long>(lastVsync + 1u),
                                     static_cast<unsigned long long>(vsync - 1u));
        lastVsync = vsync;
        // (a mirror drawn while the camera is in another view than the bumper (+0x180 view 0) is one GT4 draws only because it
        // was made to: read at the draw, as the camera's update and the view button can come either side of it in a frame)
        const uint32_t cam = g_cam.load(std::memory_order_relaxed);
        uint32_t view = 0;
        if (cam && cam + 0x184u < 0x02000000u)
            std::memcpy(&view, rdram + cam + 0x180u, 4);
        const bool forced = cam && view != 0u;
        if ((!wanted() && mode() != 2 && !gt4hudpic::wantMirror() && !gt4hudmove::mirrorMoved()) || rect + 16u >= 0x02000000u)
            return;
        float f[4];
        std::memcpy(f, rdram + rect, sizeof(f));
        // (as GT4 sets it: inside the 640 x 448 buffer)
        if (!(f[2] > 8.0f && f[3] > 4.0f && f[0] >= 0.0f && f[1] >= 0.0f && f[0] + f[2] <= 640.5f && f[1] + f[3] <= 448.5f))
            return;
        // (developer, 20 Oct: GT4's mirror on the game screen in the bumper view only, the page's rear view in every view. A mirror
        // drawn only for the page (a view GT4 draws none in) is drawn one row shorter, so the GS thread knows it from its
        // scissor, in order with the frame it belongs to, and takes it off the screen: GT4_MIRROR=screen while a page watches.
        // both keeps it on the screen in every view; page takes every mirror off.)
        if (forced && mode() == 0)
        {
            const float h1 = f[3] - 1.0f;
            std::memcpy(rdram + rect + 12u, &h1, 4);
        }
        std::lock_guard<std::mutex> lock(g_rectMutex);
        // (GT4's own rectangle; a page-only mirror's scissor is one row shorter: scissorKind)
        g_rx = static_cast<int>(f[0] + 0.5f), g_ry = static_cast<int>(f[1] + 0.5f);
        g_rw = static_cast<int>(f[2] + 0.5f), g_rh = static_cast<int>(f[3] + 0.5f);
        ++g_noted;
        gt4hudpic::noteMirror(g_rx, g_ry, g_rw, g_rh); // (the launcher's race pictures: the mirror's rectangle)
    }

    int mode()
    {
        static const int m = [] {
            const char *v = std::getenv("GT4_MIRROR");
            const std::string s = v ? v : "";
            const int r = s == "both" ? 1 : s == "page" ? 2 : 0;
            if (!s.empty() && s != "screen" && s != "both" && s != "page")
                std::fprintf(stderr, "[gt4-mirror] GT4_MIRROR=%s not understood (screen, both, page): screen\n", s.c_str());
            else if (r)
                std::fprintf(stderr, "[gt4-mirror] GT4_MIRROR=%s: the rear-view mirror in every view, %s\n", s.c_str(),
                             r == 1 ? "on the screen and on the page" : "on the page only (the main screen shows the road)");
            return r;
        }();
        return m;
    }

    bool lastRect(int &x, int &y, int &w, int &h)
    {
        std::lock_guard<std::mutex> lock(g_rectMutex);
        if (!g_noted)
            return false;
        x = g_rx, y = g_ry, w = g_rw, h = g_rh;
        return true;
    }

    bool takeRect(int &x, int &y, int &w, int &h)
    {
        std::lock_guard<std::mutex> lock(g_rectMutex);
        if (g_noted == g_taken)
            return false;
        g_taken = g_noted;
        x = g_rx, y = g_ry, w = g_rw, h = g_rh;
        return true;
    }

    void submit(std::vector<uint8_t> &&pixels, int w, int h)
    {
        std::call_once(g_encoderStarted, [] { std::thread(encodeLoop).detach(); });
        {
            std::lock_guard<std::mutex> lock(g_rawMutex);
            g_raw = std::move(pixels);
            g_rawW = w, g_rawH = h;
            g_rawNew = true;
        }
        g_rawReady.notify_one();
    }

    std::shared_ptr<const std::vector<uint8_t>> nextJpeg(uint64_t &serial, int ms)
    {
        std::unique_lock<std::mutex> lock(g_jpegMutex);
        g_jpegReady.wait_for(lock, std::chrono::milliseconds(ms), [&] { return g_jpegSerial != serial; });
        if (g_jpegSerial == serial || !g_jpeg)
            return nullptr;
        serial = g_jpegSerial;
        return g_jpeg;
    }

    std::shared_ptr<const std::vector<uint8_t>> lastJpeg()
    {
        std::lock_guard<std::mutex> lock(g_jpegMutex);
        return g_jpeg;
    }

    int64_t lastAgeMs()
    {
        std::lock_guard<std::mutex> lock(g_jpegMutex);
        if (!g_jpeg)
            return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_jpegTime).count();
    }
} // namespace gt4mirror

#endif
