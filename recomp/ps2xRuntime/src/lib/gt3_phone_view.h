#pragma once

// A phone (or any browser on the local network) as GT3's rear-view mirror.
// GT3_PHONE_VIEW=<port> (1: 8093) serves http://<this PC>:<port>/, a page
// showing the mirror as the race draws it: an MJPEG stream (/mirror.mjpg;
// one picture: /mirror.jpg). The present loop crops the mirror out of each
// game frame (GS::insetView gives its rectangle) and reads it back without
// waiting (Capture, pixel buffer objects); JPEG encoding and sending run on
// their own threads. Nothing is captured while no viewer is connected.

#include <cstdint>

namespace phoneview
{
    // Starts the server if GT3_PHONE_VIEW asks for it (false otherwise). With
    // EE RAM given, /timing also shows live timing and a track map
    // (gt3_race_info.h).
    bool start(const uint8_t *rdram = nullptr);
    void stop();
    // A viewer is connected: the present loop captures only then.
    bool wanted();
    // The server runs and its layout has the mirror panel: GT3's rear-view
    // mirror is then drawn in races even if hidden on the screen (the phone's
    // picture is cut out of the game's; ps2_runtime.cpp).
    bool mirrorPanel();
    // Frames per second to send (GT3_PHONE_VIEW_FPS, default 30).
    int fps();
    // A cropped picture (RGBA, rows top down) to send.
    void submit(const uint8_t *rgba, int width, int height);

    // The phone dashboard's layout (the launcher's Phone page; the page at /
    // reads /layout.json): panels 0 mirror, 1 position, 2 timing, 3 map,
    // 4 radar. GT3_PHONE_ORDER=<5 digits, panel + 1, in order> (default
    // 12345), GT3_PHONE_SIZES=<5 digits, size + 1 per panel 0-4; size 0 off,
    // 1 small, 2 medium, 3 large> (default 42333), GT3_PHONE_ARRANGE=0 auto
    // (one column upright, two across), 1 one column, 2 two columns,
    // GT3_PHONE_RADAR_RANGE=<metres> (default 30), GT3_PHONE_MAP_FALLBACK=0:
    // no map drawn from the cars' paths when GT3's course data is not found
    // (default 1). Live: the same values as settings keys phone_order,
    // phone_sizes, phone_arrange, phone_radar_range, phone_map_fallback
    // (-1: unchanged).
    void applyLive(int order, int sizes, int arrange, int radarRange, int mapFallback);

    // Present thread, with the GL context current: reads a rectangle of a
    // texture back without stalling; pictures that arrived go to submit().
    class Capture
    {
    public:
        ~Capture();
        // Starts reading x, y, w, h (texels, row 0 = the picture's top) of
        // texture `texture`.
        void read(unsigned texture, int x, int y, int w, int h);
        // Hands over the readbacks that have completed.
        void poll();

    private:
        bool enlarge(unsigned texture, int x, int y, int w, int h, int k);
        struct Slot
        {
            unsigned buffer = 0;
            void *fence = nullptr;
            int w = 0, h = 0;
            bool busy = false;
        };
        Slot m_slots[3];
        unsigned m_fbo = 0;
        int m_next = 0;
        // The enlarged mirror (gt3_phone_capture.cpp): program (-1 failed),
        // its uniforms, target texture and framebuffer, an empty VAO.
        int m_program = 0, m_rectLoc = -1, m_scaleLoc = -1;
        unsigned m_big = 0, m_drawFbo = 0, m_vao = 0;
        int m_bigW = 0, m_bigH = 0;
    };
}
