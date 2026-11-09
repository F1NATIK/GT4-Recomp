#include "gt4_fps_counter.h"
#include "gt4_fps_stats.h"
#include "gt4_hud.h"
#include "present_overlay.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The FPS counter (gt4_fps_counter.h): raylib shapes and its default font only.

namespace gt4fps
{
    namespace
    {
        std::string lowerTrim(const char *v)
        {
            std::string s = v ? v : "";
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        Mode startMode()
        {
            const char *v = std::getenv("GT4_FPS_COUNTER");
            const std::string s = lowerTrim(v);
            if (s.empty() || s == "off" || s == "0")
                return Mode::Off;
            if (s == "simple")
                return Mode::Simple;
            if (s == "detailed" || s == "on" || s == "1")
                return Mode::Detailed;
            std::fprintf(stderr, "[gt4-fps] GT4_FPS_COUNTER=%s not understood (off, simple or detailed): off\n", v);
            return Mode::Off;
        }

        unsigned startStats()
        {
            const char *v = std::getenv("GT4_FPS_STATS");
            const std::string s = lowerTrim(v);
            if (s.empty() || s == "all")
                return StatAll;
            unsigned bits = 0;
            size_t at = 0;
            while (at <= s.size())
            {
                const size_t comma = std::min(s.find(',', at), s.size());
                std::string item = s.substr(at, comma - at);
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                bool found = false;
                for (unsigned i = 0; i < 11; ++i)
                    if (item == kStatNames[i])
                    {
                        bits |= 1u << i;
                        found = true;
                    }
                if (!found && !item.empty() && item != "none")
                    std::fprintf(stderr, "[gt4-fps] GT4_FPS_STATS: '%s' not understood (fps, frametime, worst, low1, graph, cpu, gpu, ram, vram, resolution, renderer)\n",
                                 item.c_str());
                at = comma + 1;
            }
            return bits ? bits : static_cast<unsigned>(StatFps);
        }

        Mode g_mode = startMode();
        const unsigned g_stats = startStats();
        std::string g_renderer = "CPU";

        // 60 fps is 16.7 ms a frame; 30 fps 33.3 ms.
        Color frameColour(double ms)
        {
            if (ms <= 17.5)
                return Color{80, 220, 110, 255};
            if (ms <= 34.0)
                return Color{240, 200, 60, 255};
            return Color{240, 80, 70, 255};
        }

        Color fpsColour(double fps)
        {
            if (fps >= 59.0)
                return Color{80, 220, 110, 255};
            if (fps >= 45.0)
                return Color{240, 200, 60, 255};
            return Color{240, 80, 70, 255};
        }

        Color loadColour(float pct)
        {
            if (pct < 70.0f)
                return Color{235, 235, 235, 255};
            if (pct < 90.0f)
                return Color{240, 200, 60, 255};
            return Color{240, 80, 70, 255};
        }

        std::string sizeText(double mb)
        {
            char b[32];
            if (mb >= 1024.0)
                std::snprintf(b, sizeof(b), "%.2f GB", mb / 1024.0);
            else
                std::snprintf(b, sizeof(b), "%.0f MB", mb);
            return b;
        }

        // The 1% low: the frame rate of the slowest 1% of the latest frames (at least one frame).
        double onePercentLow(const float *gaps, size_t n)
        {
            if (!gaps || n < 10)
                return 0.0;
            std::vector<float> v(gaps, gaps + n);
            const size_t k = std::max<size_t>(1, n / 100);
            std::partial_sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end(), [](float a, float b) { return a > b; });
            double sum = 0.0;
            for (size_t i = 0; i < k; ++i)
                sum += v[i];
            const double ms = sum / static_cast<double>(k);
            return ms > 0.0 ? 1000.0 / ms : 0.0;
        }

        struct Line
        {
            std::string text;
            int size;
            Color colour;
        };

        // ---- Its place (30 Oct 2026, developer: "make it so the FPS counter can be placed in different places or make it
        // dragable"). GT4_FPS_POS=<x %>,<y %>: the box's top left in % of the area it is drawn in (absent: 10 px from the top
        // left, as before), kept inside it. The left mouse button drags it anywhere in GT4 (menus too) unless our race HUD
        // has the mouse (gt4hud::mouseTaken); the line is written to gt4-start.env when the button is let go
        // (gt4hud::saveStartLine), and the launcher keeps it when GT4 closes. An outline shows on it for 2 s after the
        // mouse last moved over it.
        bool g_posSet = false;
        float g_posX = 0.0f, g_posY = 0.0f;
        const bool g_posRead = [] {
            const char *v = std::getenv("GT4_FPS_POS");
            float x = 0.0f, y = 0.0f;
            if (v && *v)
            {
                if (std::sscanf(v, "%f,%f", &x, &y) == 2 && x >= 0.0f && x <= 100.0f && y >= 0.0f && y <= 100.0f)
                    g_posSet = true, g_posX = x / 100.0f, g_posY = y / 100.0f;
                else
                    std::fprintf(stderr, "[gt4-fps] GT4_FPS_POS=%s not understood (<x %%>,<y %%>): the top left\n", v);
            }
            return true;
        }();
        Rectangle g_box{};     // last frame's box
        bool g_held = false;
        Vector2 g_grabOff{};
        double g_mouseAt = -10.0;

        // The box's top left for a w x h box this frame (the mouse moves it first).
        void place(const Frame &f, int w, int h, int &x, int &y)
        {
            const float W = f.screenW, H = f.screenH;
            const Vector2 mouse = GetMousePosition(), d = GetMouseDelta();
            const bool over = g_box.width > 0.0f && CheckCollisionPointRec(mouse, g_box);
            if (over && (d.x != 0.0f || d.y != 0.0f))
                g_mouseAt = GetTime();
            if (!g_held && over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !gt4hud::mouseTaken())
                g_held = true, g_grabOff = Vector2{mouse.x - g_box.x, mouse.y - g_box.y};
            if (g_held)
            {
                g_mouseAt = GetTime();
                const float maxX = std::max(0.0f, W - static_cast<float>(w)), maxY = std::max(0.0f, H - static_cast<float>(h));
                g_posSet = true;
                g_posX = W > 0.0f ? std::clamp(mouse.x - g_grabOff.x - f.originX, 0.0f, maxX) / W : 0.0f;
                g_posY = H > 0.0f ? std::clamp(mouse.y - g_grabOff.y - f.originY, 0.0f, maxY) / H : 0.0f;
                if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                {
                    g_held = false;
                    char line[64];
                    std::snprintf(line, sizeof(line), "GT4_FPS_POS=%.1f,%.1f", g_posX * 100.0f, g_posY * 100.0f);
                    gt4hud::saveStartLine(line);
                }
            }
            if (g_posSet)
            {
                x = static_cast<int>(f.originX + std::clamp(g_posX * W, 0.0f, std::max(0.0f, W - static_cast<float>(w))));
                y = static_cast<int>(f.originY + std::clamp(g_posY * H, 0.0f, std::max(0.0f, H - static_cast<float>(h))));
            }
            else
                x = 10 + static_cast<int>(f.originX), y = 10 + static_cast<int>(f.originY);
            g_box = Rectangle{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
        }
        // After the box: its outline while held, or for 2 s after the mouse moved over it.
        void outline()
        {
            if (!g_held && (GetTime() - g_mouseAt > 2.0 || !CheckCollisionPointRec(GetMousePosition(), g_box)))
                return;
            const Rectangle r{g_box.x - 2.0f, g_box.y - 2.0f, g_box.width + 4.0f, g_box.height + 4.0f};
            DrawRectangleLinesEx(r, 2.0f, g_held ? Color{255, 214, 64, 230} : Color{255, 255, 255, 170});
            ps2x::overlay::mark(r.x, r.y, r.width, r.height);
        }
    }

    Mode mode() { return g_mode; }
    bool shown() { return g_mode != Mode::Off; }

    Mode cycle()
    {
        g_mode = g_mode == Mode::Off ? Mode::Simple : g_mode == Mode::Simple ? Mode::Detailed : Mode::Off;
        return g_mode;
    }

    const char *modeName(Mode m) { return m == Mode::Simple ? "simple" : m == Mode::Detailed ? "detailed" : "off"; }

    void setRenderer(const char *name) { g_renderer = name ? name : "CPU"; }

    void draw(const Frame &f)
    {
        const bool detailed = g_mode == Mode::Detailed;
        const unsigned pcStats = static_cast<unsigned>(StatCpu | StatGpu | StatRam | StatVram | StatRenderer);
        stats::want(detailed && (g_stats & pcStats) != 0u);
        if (g_mode == Mode::Off || f.screenW <= 0.0f || f.screenH <= 0.0f)
            return;
        if (detailed && (g_stats & (StatGpu | StatVram | StatRenderer)))
            stats::noteRenderGpu(); // once: this thread's OpenGL context is on GT4's graphics card

        // The numbers change four times a second (readable); the graph every frame.
        static double lastUpdate = -1.0, fps = 0.0, nowMs = 0.0, worstMs = 0.0, low1 = 0.0;
        static stats::Sample pc;
        const double t = GetTime();
        if (lastUpdate < 0.0 || t - lastUpdate >= 0.25)
        {
            lastUpdate = t;
            fps = f.fps;
            worstMs = f.worstMs;
            nowMs = (f.fps > 0.0 && f.gaps > 0) ? f.gapsMs[0] : 0.0;
            low1 = onePercentLow(f.gapsMs, std::min<size_t>(f.gaps, 600));
            if (detailed)
                pc = stats::latest();
        }

        const int big = std::max(16, static_cast<int>(f.screenH / 40.0f));
        const int sub = std::max(12, static_cast<int>(big * 0.72f));
        const int pad = std::max(6, sub / 2);
        const Color white{235, 235, 235, 255}, grey{170, 170, 170, 255};
        char b[160];

        if (!detailed)
        {
            // Simple: fps and the frame time on one line.
            char ms[48];
            if (fps > 0.0)
            {
                std::snprintf(b, sizeof(b), "%.1f fps", fps);
                std::snprintf(ms, sizeof(ms), "%.1f ms", nowMs);
            }
            else
            {
                std::snprintf(b, sizeof(b), "-- fps");
                std::snprintf(ms, sizeof(ms), "no new frame");
            }
            const int w1 = MeasureText(b, big), w2 = MeasureText(ms, sub);
            const int w = pad + w1 + pad + w2 + pad, h = pad + big + pad;
            int x = 0, y = 0;
            place(f, w, h, x, y);
            ps2x::overlay::mark(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
            DrawRectangle(x, y, w, h, Color{0, 0, 0, 165});
            DrawText(b, x + pad, y + pad, big, fps > 0.0 ? fpsColour(fps) : Color{200, 200, 200, 255});
            DrawText(ms, x + pad + w1 + pad, y + pad + (big - sub), sub, white);
            outline();
            return;
        }

        // Detailed: the chosen lines, then the graph.
        std::vector<Line> lines;
        if (g_stats & StatFps)
        {
            if (fps > 0.0)
                std::snprintf(b, sizeof(b), "%.1f fps", fps);
            else
                std::snprintf(b, sizeof(b), "-- fps");
            lines.push_back({b, big, fps > 0.0 ? fpsColour(fps) : Color{200, 200, 200, 255}});
        }
        if (g_stats & (StatFrameTime | StatWorst))
        {
            std::string s;
            if (fps <= 0.0)
                s = "no new frame";
            else
            {
                if (g_stats & StatFrameTime)
                {
                    std::snprintf(b, sizeof(b), "%.1f ms", nowMs);
                    s = b;
                }
                if (g_stats & StatWorst)
                {
                    std::snprintf(b, sizeof(b), "%sworst %.1f ms", s.empty() ? "" : "   ", worstMs);
                    s += b;
                }
            }
            lines.push_back({s, sub, white});
        }
        if (g_stats & StatLow1)
        {
            if (low1 > 0.0)
                std::snprintf(b, sizeof(b), "1%% low %.1f fps", low1);
            else
                std::snprintf(b, sizeof(b), "1%% low --");
            lines.push_back({b, sub, low1 > 0.0 ? fpsColour(low1) : white});
        }
        const size_t graphAt = lines.size();
        if (g_stats & StatCpu)
        {
            if (pc.cpuValid)
            {
                std::snprintf(b, sizeof(b), "CPU %.0f%%   GT4 %.0f%%", pc.cpuAll, pc.cpuGt4);
                lines.push_back({b, sub, loadColour(pc.cpuAll)});
            }
            else
                lines.push_back({"CPU --", sub, grey});
        }
        if (g_stats & StatGpu)
        {
            if (pc.gpuValid)
            {
                std::snprintf(b, sizeof(b), "GPU %.0f%%   GT4 %.0f%%", pc.gpuAll, pc.gpuGt4);
                lines.push_back({b, sub, loadColour(pc.gpuAll)});
            }
            else
                lines.push_back({"GPU --", sub, grey});
        }
        if (g_stats & StatRam)
        {
            if (pc.ramValid)
                lines.push_back({"RAM GT4 " + sizeText(pc.ramGt4Mb) + "   PC " + sizeText(pc.ramUsedMb) + " / " + sizeText(pc.ramTotalMb), sub, white});
            else
                lines.push_back({"RAM --", sub, grey});
        }
        if (g_stats & StatVram)
        {
            if (pc.vramValid)
            {
                std::string s = "VRAM GT4 " + sizeText(pc.vramGt4Mb);
                if (pc.vramUsedMb >= 0.0)
                    s += "   all " + sizeText(pc.vramUsedMb);
                if (pc.vramTotalMb > 0.0)
                    s += " / " + sizeText(pc.vramTotalMb);
                lines.push_back({s, sub, white});
            }
            else
                lines.push_back({"VRAM --", sub, grey});
        }
        if ((g_stats & StatResolution) && f.resolution && *f.resolution)
            lines.push_back({f.resolution, sub, white});
        if (g_stats & StatRenderer)
            lines.push_back({pc.gpuName.empty() ? g_renderer : g_renderer + "   " + pc.gpuName, sub, white});

        const bool graph = (g_stats & StatGraph) != 0u;
        const int graphH = graph ? big * 2 : 0;
        int contentW = graph ? big * 8 : 0;
        for (const Line &l : lines)
            contentW = std::max(contentW, MeasureText(l.text.c_str(), l.size));
        int h = pad;
        for (const Line &l : lines)
            h += l.size + 4;
        if (graph)
            h += pad / 2 + graphH + pad / 2;
        h += pad - (lines.empty() ? 0 : 4);
        const int w = contentW + pad * 2;
        int x = 0, y = 0;
        place(f, w, h, x, y);
        ps2x::overlay::mark(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        DrawRectangle(x, y, w, h, Color{0, 0, 0, 165});
        int yy = y + pad;
        for (size_t i = 0; i <= lines.size(); ++i)
        {
            if (i == graphAt && graph)
            {
                // The graph: one bar a frame, newest on the right, 0 to 50 ms high; lines at 16.7 and 33.3 ms.
                yy += pad / 2;
                const int gx = x + pad, gy = yy, gw = contentW;
                DrawRectangle(gx, gy, gw, graphH, Color{255, 255, 255, 18});
                constexpr float kTopMs = 50.0f;
                auto yFor = [&](float ms) { return gy + graphH - static_cast<int>(std::min(ms, kTopMs) / kTopMs * static_cast<float>(graphH)); };
                const int barW = std::max(2, big / 8);
                const size_t bars = std::min(f.gaps, static_cast<size_t>(gw / barW));
                for (size_t k = 0; k < bars; ++k)
                {
                    const float ms = f.gapsMs[k];
                    const int top = yFor(ms);
                    const int bx = gx + gw - static_cast<int>(k + 1) * barW;
                    DrawRectangle(bx, top, barW - (barW > 2 ? 1 : 0), std::max(1, gy + graphH - top), frameColour(ms));
                }
                DrawRectangle(gx, yFor(1000.0f / 60.0f), gw, 1, Color{255, 255, 255, 110});
                DrawRectangle(gx, yFor(1000.0f / 30.0f), gw, 1, Color{255, 255, 255, 60});
                yy += graphH + pad / 2;
            }
            if (i == lines.size())
                break;
            DrawText(lines[i].text.c_str(), x + pad, yy, lines[i].size, lines[i].colour);
            yy += lines[i].size + 4;
        }
        outline();
    }
}
