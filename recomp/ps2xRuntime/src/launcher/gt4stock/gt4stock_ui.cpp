#include "gt4stock_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace gt4stock::ui
{
    const Theme &theme()
    {
#if defined(GT4L_EDITION_FULL)
        // The Full edition's look (the mock-ups: a deeper slate and a teal accent; Play stays GT4's red).
        static const Theme t = [] {
            Theme f;
            f.bg = Color{11, 16, 22, 255};
            f.sidebar = Color{14, 20, 27, 255};
            f.panel = Color{18, 25, 33, 255};
            f.panel2 = Color{26, 34, 44, 255};
            f.panel3 = Color{36, 46, 58, 255};
            f.border = Color{38, 48, 60, 255};
            f.text = Color{232, 237, 242, 255};
            f.muted = Color{148, 160, 174, 255};
            f.faint = Color{100, 112, 128, 255};
            f.accent = Color{56, 200, 190, 255};
            f.accentDim = Color{22, 82, 78, 255};
            return f;
        }();
#else
        static const Theme t;
#endif
        return t;
    }

    Color alpha(Color c, float a)
    {
        c.a = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * static_cast<float>(c.a));
        return c;
    }

    Color mixColor(Color a, Color b, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        auto m = [&](unsigned char x, unsigned char y) { return static_cast<unsigned char>(std::lround(x + (y - x) * t)); };
        return Color{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
    }

    namespace
    {
        // The glyphs the pages use: ASCII, Latin-1 (GT4's names have a few accented letters)
        // and a few typographic marks (gt4stock_names.cpp displayText keeps to this set).
        std::vector<int> codepoints()
        {
            std::vector<int> cps;
            for (int c = 32; c < 127; ++c)
                cps.push_back(c);
            for (int c = 0xa0; c <= 0xff; ++c)
                cps.push_back(c);
            for (int c : {0x2013, 0x2014, 0x2026, 0x2018, 0x2019, 0x201c, 0x201d, 0x2192, 0x2190, 0x2264, 0x2265, 0x2022})
                cps.push_back(c);
            return cps;
        }

        // A no-break space (U+00A0) keeps words together when a text wraps ("L2 / R2"); raylib gives that glyph no
        // width, so it is drawn and measured as a plain space.
        const std::string &spaced(const std::string &t, std::string &buf)
        {
            if (t.find("\xc2\xa0") == std::string::npos)
                return t;
            buf.clear();
            for (size_t i = 0; i < t.size(); ++i)
            {
                if (t[i] == '\xc2' && i + 1 < t.size() && t[i + 1] == '\xa0')
                {
                    buf.push_back(' ');
                    ++i;
                }
                else
                    buf.push_back(t[i]);
            }
            return buf;
        }
    }

    Ui::Ui() = default;
    Ui::~Ui() = default;

    void Ui::loadFonts()
    {
        const char *const dirs[] = {"C:\\Windows\\Fonts\\"};
        const char *const files[4] = {"segoeui.ttf", "seguisb.ttf", "segoeuib.ttf", "consola.ttf"};
        haveFonts_ = true;
        for (int w = 0; w < 4; ++w)
        {
            fontPath_[w].clear();
            for (const char *d : dirs)
            {
                const std::string p = std::string(d) + files[w];
                std::error_code ec;
                if (std::filesystem::exists(p, ec))
                {
                    fontPath_[w] = p;
                    break;
                }
            }
            if (fontPath_[w].empty())
                fontPath_[w] = w == 0 ? std::string() : fontPath_[0];
        }
        if (fontPath_[0].empty())
            haveFonts_ = false;
    }

    void Ui::unloadFonts()
    {
        for (auto &[k, f] : fonts_)
            UnloadFont(f);
        fonts_.clear();
    }

    Font Ui::font(float sizeDp, Weight w)
    {
        const int px = std::max(6, static_cast<int>(std::lround(sizeDp * s)));
        if (!haveFonts_)
            return GetFontDefault();
        const FontKey key{px, static_cast<int>(w)};
        if (auto it = fonts_.find(key); it != fonts_.end())
            return it->second;
        std::vector<int> cps = codepoints();
        Font f = LoadFontEx(fontPath_[static_cast<int>(w)].c_str(), px, cps.data(), static_cast<int>(cps.size()));
        if (f.texture.id == 0)
            f = GetFontDefault();
        else
            SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
        fonts_[key] = f;
        return f;
    }

    void Ui::begin(int wPx, int hPx, const Input &input)
    {
        widthPx = wPx;
        heightPx = hPx;
        const float ns = std::clamp(std::min(wPx / 1280.0f, hPx / 800.0f), 0.5f, 4.0f);
        if (std::fabs(ns - s) > 0.001f)
        {
            // The atlases are kept by pixel size, so a picture drawn off screen at another scale (the Cheats page's card
            // and sheet) and the window's next frame both find theirs: nothing is reloaded from disk (1.4 review). Only a
            // long run of window sizes lets the cache grow; past 160 atlases it starts again.
            if (fonts_.size() > 160)
                unloadFonts();
            s = ns;
        }
        width = wPx / s;
        height = hPx / s;
        in_ = input;
        inputBlocked_ = false;
        consumedClick_ = false;
        // A press on an open drop-down list belongs to the list.
        if (!openDropdown_.empty() && in_.enabled && in_.pressed)
        {
            int rows = 0;
            const Rectangle list = dropListRect(rows);
            if (CheckCollisionPointRec(mouseDp(), list) || CheckCollisionPointRec(mouseDp(), dropRect_))
                consumedClick_ = true;
        }
    }

    // The open list: below the box, else above it, else as tall as the window allows (a long list, e.g. Prize Cars' dozens of
    // makers, shows the rows that fit and scrolls with the wheel; before, it covered the box and the click that opened it
    // picked a row and closed it at once).
    Rectangle Ui::dropListRect(int &rows) const
    {
        const float rowH = std::max(26.0f, dropTextSize_ * 2.0f);
        const int n = static_cast<int>(dropOptions_.size());
        const int fit = std::max(1, std::min(14, static_cast<int>((height - 16.0f) / rowH))); // at most 14 rows, then it scrolls
        rows = std::min(n, fit);
        Rectangle list{dropRect_.x, dropRect_.y + dropRect_.height + 2.0f, dropRect_.width, rowH * static_cast<float>(rows) + 8.0f};
        if (list.y + list.height > height - 4.0f)
        {
            const float above = dropRect_.y - list.height - 2.0f;
            list.y = above >= 4.0f ? above : std::max(4.0f, height - list.height - 4.0f);
        }
        return list;
    }

    void Ui::end()
    {
        if (openDropdown_.empty())
            return;
        const Theme &t = theme();
        const float rowH = std::max(26.0f, dropTextSize_ * 2.0f);
        int rows = 0;
        const Rectangle list = dropListRect(rows);
        const int n = static_cast<int>(dropOptions_.size());
        if (in_.enabled && in_.wheel != 0.0f && CheckCollisionPointRec(mouseDp(), list))
            dropFirst_ -= static_cast<int>(in_.wheel > 0 ? std::ceil(in_.wheel) : std::floor(in_.wheel)) * 3;
        dropFirst_ = std::clamp(dropFirst_, 0, std::max(0, n - rows));
        round(Rectangle{list.x + 2, list.y + 4, list.width, list.height}, 8.0f, alpha(BLACK, 0.35f));
        round(list, 8.0f, t.panel2);
        roundLine(list, 8.0f, 1.0f, t.border);
        if (rows < n) // a scroll thumb on the right edge
        {
            const float trackH = list.height - 8.0f, thumbH = std::max(18.0f, trackH * rows / n);
            const float thumbY = list.y + 4.0f + (trackH - thumbH) * dropFirst_ / std::max(1, n - rows);
            round(Rectangle{list.x + list.width - 6.0f, thumbY, 3.0f, thumbH}, 1.5f, t.faint);
        }
        bool pickedNow = false;
        const bool justOpened = dropJustOpened_;
        dropJustOpened_ = false;
        for (int k = 0; k < rows; ++k)
        {
            const size_t i = static_cast<size_t>(dropFirst_ + k);
            const Rectangle row{list.x + 4.0f, list.y + 4.0f + rowH * static_cast<float>(k), list.width - 12.0f, rowH};
            const bool hot = in_.enabled && !justOpened && CheckCollisionPointRec(mouseDp(), row);
            if (static_cast<int>(i) == dropSelected_)
                round(row, 6.0f, t.accentDim);
            else if (hot)
                round(row, 6.0f, t.panel3);
            text(fit(dropOptions_[i], row.width - 20.0f, dropTextSize_), row.x + 10.0f, row.y + (rowH - dropTextSize_ * 1.33f) * 0.5f,
                 dropTextSize_, t.text);
            if (hot && in_.pressed)
            {
                dropPicked_ = static_cast<int>(i);
                dropPickedId_ = openDropdown_;
                pickedNow = true;
            }
        }
        if (!justOpened && in_.enabled && in_.pressed && (pickedNow || (!CheckCollisionPointRec(mouseDp(), dropRect_) &&
                                                                         !CheckCollisionPointRec(mouseDp(), list))))
            openDropdown_.clear();
    }

    Rectangle Ui::px(Rectangle r) const
    {
        const float x0 = std::round(r.x * s), y0 = std::round(r.y * s);
        const float x1 = std::round((r.x + r.width) * s), y1 = std::round((r.y + r.height) * s);
        return Rectangle{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }

    void Ui::fill(Rectangle r, Color c) { DrawRectangleRec(px(r), c); }

    void Ui::round(Rectangle r, float radius, Color c)
    {
        const Rectangle p = px(r);
        const float m = std::min(p.width, p.height);
        if (m <= 0.0f)
            return;
        const float roundness = std::clamp(radius * s * 2.0f / m, 0.0f, 1.0f);
        DrawRectangleRounded(p, roundness, 10, c);
    }

    void Ui::roundLine(Rectangle r, float radius, float thick, Color c)
    {
        const Rectangle p = px(r);
        const float m = std::min(p.width, p.height);
        if (m <= 0.0f)
            return;
        const float roundness = std::clamp(radius * s * 2.0f / m, 0.0f, 1.0f);
        DrawRectangleRoundedLinesEx(p, roundness, 10, std::max(1.0f, thick * s), c);
    }

    void Ui::line(float x1, float y1, float x2, float y2, float thick, Color c)
    {
        DrawLineEx(Vector2{x1 * s, y1 * s}, Vector2{x2 * s, y2 * s}, std::max(1.0f, thick * s), c);
    }

    void Ui::circle(float cx, float cy, float r, Color c) { DrawCircleV(Vector2{cx * s, cy * s}, r * s, c); }

    void Ui::ring(float cx, float cy, float r0, float r1, Color c) { DrawRing(Vector2{cx * s, cy * s}, r0 * s, r1 * s, 0.0f, 360.0f, 48, c); }

    void Ui::gradientH(Rectangle r, Color left, Color right) { DrawRectangleGradientH(static_cast<int>(px(r).x), static_cast<int>(px(r).y), static_cast<int>(px(r).width), static_cast<int>(px(r).height), left, right); }
    void Ui::gradientV(Rectangle r, Color top, Color bottom) { DrawRectangleGradientV(static_cast<int>(px(r).x), static_cast<int>(px(r).y), static_cast<int>(px(r).width), static_cast<int>(px(r).height), top, bottom); }

    void Ui::arrowLine(float x1, float y1, float x2, float y2, float thick, Color c, int heads, float dash)
    {
        const float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.5f)
            return;
        const float ux = dx / len, uy = dy / len;
        const float head = std::max(7.0f, thick * 4.0f), headW = head * 0.42f;
        // The line stops where the heads start.
        const float a = heads >= 2 ? head * 0.8f : 0.0f, b = len - (heads >= 1 ? head * 0.8f : 0.0f);
        if (dash > 0.0f)
        {
            for (float t = a; t < b; t += dash * 2.0f)
                line(x1 + ux * t, y1 + uy * t, x1 + ux * std::min(b, t + dash), y1 + uy * std::min(b, t + dash), thick, c);
        }
        else if (b > a)
            line(x1 + ux * a, y1 + uy * a, x1 + ux * b, y1 + uy * b, thick, c);
        auto tip = [&](float tx, float ty, float dirx, float diry) {
            // A filled triangle pointing along (dirx, diry), its tip at (tx, ty).
            const float bx = tx - dirx * head, by = ty - diry * head;
            Vector2 p1{tx * s, ty * s}, p2{(bx - diry * headW) * s, (by + dirx * headW) * s}, p3{(bx + diry * headW) * s, (by - dirx * headW) * s};
            // raylib wants the corners counter-clockwise on screen.
            if ((p2.x - p1.x) * (p3.y - p1.y) - (p2.y - p1.y) * (p3.x - p1.x) > 0.0f)
                std::swap(p2, p3);
            DrawTriangle(p1, p2, p3, c);
        };
        if (heads >= 1)
            tip(x2, y2, ux, uy);
        if (heads >= 2)
            tip(x1, y1, -ux, -uy);
    }

    void Ui::icon(Icon kind, Rectangle r, Color c)
    {
        const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f, u = std::min(r.width, r.height) / 24.0f;
        auto L = [&](float x1, float y1, float x2, float y2, float t = 1.8f) { line(cx + x1 * u, cy + y1 * u, cx + x2 * u, cy + y2 * u, t * u, c); };
        auto P = [&](float x, float y) { return Vector2{(cx + x * u) * s, (cy + y * u) * s}; };
        switch (kind)
        {
        case Icon::Play:
            DrawTriangle(P(-6, -8), P(-6, 8), P(8, 0), c);
            break;
        case Icon::Stop:
            fill(Rectangle{cx - 6 * u, cy - 6 * u, 12 * u, 12 * u}, c);
            break;
        case Icon::Display:
            roundLine(Rectangle{cx - 10 * u, cy - 8 * u, 20 * u, 13 * u}, 2 * u, 1.8f * u, c);
            L(-4, 9, 4, 9);
            L(0, 5, 0, 9);
            break;
        case Icon::Gamepad:
            roundLine(Rectangle{cx - 11 * u, cy - 6 * u, 22 * u, 12 * u}, 6 * u, 1.8f * u, c);
            L(-7, 0, -3, 0);
            L(-5, -2, -5, 2);
            circle(cx + 4 * u, cy + 1 * u, 1.3f * u, c);
            circle(cx + 7 * u, cy - 1.5f * u, 1.3f * u, c);
            break;
        case Icon::Wheel:
            ring(cx, cy, 8.0f * u, 10.0f * u, c);
            circle(cx, cy, 2.2f * u, c);
            L(-8.5f, 1, -2, 1);
            L(8.5f, 1, 2, 1);
            L(0, 3, 0, 9);
            break;
        case Icon::Card:
            roundLine(Rectangle{cx - 7 * u, cy - 10 * u, 14 * u, 20 * u}, 2 * u, 1.8f * u, c);
            fill(Rectangle{cx - 4 * u, cy + 3 * u, 8 * u, 4 * u}, c);
            break;
        case Icon::Info:
            ring(cx, cy, 8.5f * u, 10.3f * u, c);
            fill(Rectangle{cx - 1 * u, cy - 2 * u, 2 * u, 7 * u}, c);
            circle(cx, cy - 5 * u, 1.3f * u, c);
            break;
        case Icon::Folder:
            roundLine(Rectangle{cx - 10 * u, cy - 6 * u, 20 * u, 14 * u}, 2 * u, 1.8f * u, c);
            L(-10, -6, -9, -9);
            L(-9, -9, -3, -9);
            L(-3, -9, -2, -6);
            break;
        case Icon::Check:
            L(-7, 0, -2, 6, 2.6f);
            L(-2, 6, 8, -6, 2.6f);
            break;
        case Icon::Cross:
            L(-6, -6, 6, 6, 2.6f);
            L(-6, 6, 6, -6, 2.6f);
            break;
        case Icon::Warn:
            DrawTriangleLines(P(0, -9), P(-10, 8), P(10, 8), c);
            L(0, -3, 0, 3, 2.2f);
            circle(cx, cy + 5.5f * u, 1.2f * u, c);
            break;
        case Icon::Usb:
            L(0, -9, 0, 8);
            L(0, 3, -5, -1);
            L(0, 0, 5, -4);
            circle(cx, cy + 8.5f * u, 2.0f * u, c);
            fill(Rectangle{cx - 6.5f * u, cy - 2.5f * u, 3 * u, 3 * u}, c);
            circle(cx + 5 * u, cy - 4.5f * u, 1.6f * u, c);
            break;
        case Icon::Disc:
            ring(cx, cy, 8.5f * u, 10.3f * u, c);
            ring(cx, cy, 2.0f * u, 3.6f * u, c);
            break;
        case Icon::Refresh:
            DrawRing(Vector2{cx * s, cy * s}, 6.5f * u * s, 8.3f * u * s, 30.0f, 320.0f, 32, c);
            DrawTriangle(P(9, -6), P(4, -6), P(8, -1), c);
            break;
        case Icon::Camera: // the EyeToy: a round lens in a rounded body, on a small stand
            roundLine(Rectangle{cx - 10 * u, cy - 7 * u, 20 * u, 13 * u}, 4 * u, 1.8f * u, c);
            ring(cx, cy - 0.5f * u, 3.2f * u, 5.0f * u, c);
            circle(cx + 6.5f * u, cy - 4 * u, 1.0f * u, c);
            L(-4, 9, 4, 9);
            L(0, 6, 0, 9);
            break;
        case Icon::Sparkle: // a four-point star outline, and a small filled one top right
        {
            auto star = [&](float ox, float oy, float big, float waist, bool filled) {
                Vector2 pts[8];
                for (int i = 0; i < 8; ++i)
                {
                    const float a = -1.5707963f + static_cast<float>(i) * 0.78539816f;
                    const float r = (i % 2 == 0) ? big : waist;
                    pts[i] = Vector2{cx + (ox + r * std::cos(a)) * u, cy + (oy + r * std::sin(a)) * u};
                }
                for (int i = 0; i < 8; ++i)
                {
                    const Vector2 &p = pts[i], &q = pts[(i + 1) % 8];
                    if (filled)
                        triangle(Vector2{cx + ox * u, cy + oy * u}, p, q, c);
                    else
                        line(p.x, p.y, q.x, q.y, 1.7f * u, c);
                }
            };
            star(-1.5f, 1.8f, 10.0f, 2.9f, false);
            star(7.6f, -7.4f, 4.0f, 1.2f, true);
            break;
        }
        case Icon::Eye:
            L(-10, 0, -5, -5);
            L(-5, -5, 5, -5);
            L(5, -5, 10, 0);
            L(10, 0, 5, 5);
            L(5, 5, -5, 5);
            L(-5, 5, -10, 0);
            ring(cx, cy, 2.4f * u, 4.2f * u, c);
            break;
        case Icon::Save:
            L(0, -9, 0, 4);
            L(-5, -1, 0, 4.5f);
            L(5, -1, 0, 4.5f);
            L(-9, 9, 9, 9);
            break;
        case Icon::Print:
            roundLine(Rectangle{cx - 10 * u, cy - 3 * u, 20 * u, 9 * u}, 2 * u, 1.8f * u, c);
            L(-5, -3, -5, -9);
            L(-5, -9, 5, -9);
            L(5, -9, 5, -3);
            fill(Rectangle{cx - 5 * u, cy + 3 * u, 10 * u, 7 * u}, c);
            break;
#if defined(GT4L_EDITION_FULL)
        case Icon::Screens: // three monitors, the side ones angled in
            roundLine(Rectangle{cx - 4.5f * u, cy - 7 * u, 9 * u, 9 * u}, 1.2f * u, 1.6f * u, c);
            L(-11, -5, -6.5f, -6.5f, 1.6f);
            L(-11, -5, -11, 3, 1.6f);
            L(-11, 3, -6.5f, 1.5f, 1.6f);
            L(11, -5, 6.5f, -6.5f, 1.6f);
            L(11, -5, 11, 3, 1.6f);
            L(11, 3, 6.5f, 1.5f, 1.6f);
            L(-3, 7, 3, 7);
            L(0, 2, 0, 7);
            break;
        case Icon::Garage: // a coupe from the side
            L(-11, 3, 11, 3);
            L(-11, 3, -10, -1);
            L(-10, -1, -4, -2);
            L(-4, -2, -1, -6);
            L(-1, -6, 5, -6);
            L(5, -6, 9, -1);
            L(9, -1, 11, 3);
            ring(cx - 6 * u, cy + 4 * u, 1.6f * u, 3.2f * u, c);
            ring(cx + 6 * u, cy + 4 * u, 1.6f * u, 3.2f * u, c);
            break;
        case Icon::Network: // three linked boxes
            roundLine(Rectangle{cx - 3.5f * u, cy - 10 * u, 7 * u, 6 * u}, 1 * u, 1.6f * u, c);
            roundLine(Rectangle{cx - 11 * u, cy + 4 * u, 7 * u, 6 * u}, 1 * u, 1.6f * u, c);
            roundLine(Rectangle{cx + 4 * u, cy + 4 * u, 7 * u, 6 * u}, 1 * u, 1.6f * u, c);
            L(0, -4, 0, 0, 1.6f);
            L(-7.5f, 0, 7.5f, 0, 1.6f);
            L(-7.5f, 0, -7.5f, 4, 1.6f);
            L(7.5f, 0, 7.5f, 4, 1.6f);
            break;
        case Icon::Speaker: // a loudspeaker and two sound waves
            fill(Rectangle{cx - 10 * u, cy - 3.5f * u, 4.5f * u, 7 * u}, c);
            DrawTriangle(P(-5.5f, -3.5f), P(-5.5f, 3.5f), P(1, 9), c);
            DrawTriangle(P(-5.5f, -3.5f), P(1, 9), P(1, -9), c);
            L(4.5f, -3.5f, 6, 0);
            L(6, 0, 4.5f, 3.5f);
            L(7.5f, -7.5f, 10.5f, 0);
            L(10.5f, 0, 7.5f, 7.5f);
            break;
        case Icon::Trophy: // a cup on a stand
            L(-6, -9, 6, -9);
            L(-6, -9, -5, -2);
            L(6, -9, 5, -2);
            L(-5, -2, 0, 2);
            L(5, -2, 0, 2);
            L(0, 2, 0, 6);
            L(-5, 9, 5, 9);
            L(-3, 6, 3, 6);
            L(-6, -7, -9, -6);
            L(-9, -6, -7, -2);
            L(6, -7, 9, -6);
            L(9, -6, 7, -2);
            break;
#endif
        }
    }

    void Ui::triangle(Vector2 a, Vector2 b, Vector2 c, Color col)
    {
        Vector2 p1{a.x * s, a.y * s}, p2{b.x * s, b.y * s}, p3{c.x * s, c.y * s};
        // raylib wants the corners counter-clockwise on screen.
        if ((p2.x - p1.x) * (p3.y - p1.y) - (p2.y - p1.y) * (p3.x - p1.x) > 0.0f)
            std::swap(p2, p3);
        DrawTriangle(p1, p2, p3, col);
    }

    float Ui::keycap(float x, float y, const std::string &t, float size)
    {
        const Theme &th = theme();
        const float w = textWidth(t, size, Weight::Semibold) + 16.0f;
        const float h = size * 1.33f + 8.0f;
        round(Rectangle{x, y, w, h}, 5.0f, th.panel2);
        roundLine(Rectangle{x, y, w, h}, 5.0f, 1.0f, mixColor(th.border, WHITE, 0.15f));
        fill(Rectangle{x + 3.0f, y + h - 3.0f, w - 6.0f, 1.5f}, mixColor(th.border, BLACK, 0.3f));
        text(t, x + 8.0f, y + 4.0f, size, th.text, Weight::Semibold);
        return w;
    }

    float Ui::textWidth(const std::string &str, float size, Weight w)
    {
        if (str.empty())
            return 0.0f;
        std::string buf;
        const std::string &t = spaced(str, buf);
        const Font f = font(size, w);
        const float pxSize = std::max(6.0f, std::round(size * s));
        return MeasureTextEx(f, t.c_str(), pxSize, 0.0f).x / s;
    }

    float Ui::text(const std::string &str, float x, float y, float size, Color c, Weight w, Align a)
    {
        if (str.empty())
            return 0.0f;
        std::string buf;
        const std::string &t = spaced(str, buf);
        const Font f = font(size, w);
        const float pxSize = std::max(6.0f, std::round(size * s));
        const float widthDp = MeasureTextEx(f, t.c_str(), pxSize, 0.0f).x / s;
        float left = x;
        if (a == Align::Center)
            left = x - widthDp * 0.5f;
        else if (a == Align::Right)
            left = x - widthDp;
        DrawTextEx(f, t.c_str(), Vector2{std::round(left * s), std::round(y * s)}, pxSize, 0.0f, c);
        return widthDp;
    }

    namespace
    {
        std::vector<std::string> wrapLines(Ui &ui, const std::string &t, float maxWidth, float size, Weight w)
        {
            std::vector<std::string> lines;
            for (const std::string &para : [&] {
                     std::vector<std::string> ps;
                     size_t a = 0;
                     for (;;)
                     {
                         const size_t b = t.find('\n', a);
                         ps.push_back(t.substr(a, b == std::string::npos ? std::string::npos : b - a));
                         if (b == std::string::npos)
                             break;
                         a = b + 1;
                     }
                     return ps;
                 }())
            {
                std::string cur;
                size_t i = 0;
                if (para.empty())
                {
                    lines.push_back(std::string());
                    continue;
                }
                while (i < para.size())
                {
                    size_t j = para.find(' ', i);
                    if (j == std::string::npos)
                        j = para.size();
                    const std::string word = para.substr(i, j - i);
                    const std::string trial = cur.empty() ? word : cur + " " + word;
                    if (!cur.empty() && ui.textWidth(trial, size, w) > maxWidth)
                    {
                        lines.push_back(cur);
                        cur = word;
                    }
                    else
                        cur = trial;
                    i = j + 1;
                }
                lines.push_back(cur);
            }
            return lines;
        }
    }

    float Ui::wrap(const std::string &t, float x, float y, float maxWidth, float size, Color c, Weight w, float lineGap)
    {
        const std::vector<std::string> lines = wrapLines(*this, t, maxWidth, size, w);
        float yy = y;
        for (const std::string &l : lines)
        {
            text(l, x, yy, size, c, w);
            yy += size * lineGap;
        }
        return yy - y;
    }

    float Ui::wrapHeight(const std::string &t, float maxWidth, float size, Weight w, float lineGap)
    {
        return static_cast<float>(wrapLines(*this, t, maxWidth, size, w).size()) * size * lineGap;
    }

    std::string Ui::fit(const std::string &t, float maxWidth, float size, Weight w, bool middle)
    {
        if (textWidth(t, size, w) <= maxWidth)
            return t;
        const std::string dots = "...";
        if (middle)
        {
            size_t keep = t.size();
            while (keep > 4)
            {
                keep -= 1;
                const size_t head = keep * 2 / 5, tail = keep - head;
                const std::string c = t.substr(0, head) + dots + t.substr(t.size() - tail);
                if (textWidth(c, size, w) <= maxWidth)
                    return c;
            }
            return dots;
        }
        std::string c = t;
        while (!c.empty())
        {
            c.pop_back();
            if (textWidth(c + dots, size, w) <= maxWidth)
                return c + dots;
        }
        return dots;
    }

    bool Ui::hovered(Rectangle r) const
    {
        return inputOn() && openDropdown_.empty() && CheckCollisionPointRec(mouseDp(), r) && (clip_.width <= 0.0f || CheckCollisionPointRec(mouseDp(), clip_));
    }

    float Ui::beginScroll(const std::string &id, Rectangle view)
    {
        Scroll &sc = scrolls_[id];
        sc.view = view;
        const float maxOff = std::max(0.0f, sc.content - view.height);
        if (inputOn() && openDropdown_.empty() && in_.wheel != 0.0f && CheckCollisionPointRec(mouseDp(), view))
            sc.offset -= in_.wheel * 56.0f;
        if (sc.content > 0.0f) // known from the last frame (a scrollTo before the first frame is kept)
            sc.offset = std::clamp(sc.offset, 0.0f, maxOff);
        const Rectangle p = px(view);
        BeginScissorMode(static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(p.width), static_cast<int>(p.height));
        clip_ = view;
        return sc.offset;
    }

    void Ui::endScroll(const std::string &id, float contentHeight)
    {
        EndScissorMode();
        clip_ = Rectangle{};
        Scroll &sc = scrolls_[id];
        sc.content = std::max(0.0f, contentHeight);
        const Rectangle v = sc.view;
        if (sc.content <= v.height + 0.5f)
            return;
        // The bar: a thin track at the right edge, the thumb sized to what is shown.
        const Theme &t = theme();
        const float maxOff = sc.content - v.height;
        const Rectangle track{v.x + v.width - 7.0f, v.y + 4.0f, 5.0f, v.height - 8.0f};
        const float th = std::max(28.0f, track.height * v.height / sc.content);
        const float ty = track.y + (track.height - th) * std::clamp(sc.offset / maxOff, 0.0f, 1.0f);
        const Rectangle thumb{track.x, ty, track.width, th};
        const std::string barId = id + "#bar";
        const Rectangle grab{track.x - 6.0f, track.y, track.width + 10.0f, track.height};
        if (inputOn() && openDropdown_.empty() && in_.pressed && !consumedClick_ && CheckCollisionPointRec(mouseDp(), grab))
        {
            active_ = barId;
            const float my = mouseDp().y;
            dragGrab_ = (my >= thumb.y && my <= thumb.y + thumb.height) ? my - thumb.y : th * 0.5f;
            consumedClick_ = true;
        }
        if (active_ == barId)
        {
            if (!in_.down)
                active_.clear();
            else
            {
                const float f = (mouseDp().y - dragGrab_ - track.y) / std::max(1.0f, track.height - th);
                sc.offset = std::clamp(f, 0.0f, 1.0f) * maxOff;
            }
        }
        round(track, 2.5f, alpha(t.panel3, 0.55f));
        round(thumb, 2.5f, active_ == barId ? t.muted : alpha(t.muted, 0.7f));
    }

    float Ui::scrollOffset(const std::string &id) const
    {
        const auto it = scrolls_.find(id);
        return it == scrolls_.end() ? 0.0f : it->second.offset;
    }

    void Ui::scrollTo(const std::string &id, float offset) { scrolls_[id].offset = std::max(0.0f, offset); }

    bool Ui::clicked(Rectangle r) const { return hovered(r) && in_.pressed && !consumedClick_; }
    bool Ui::rightClicked(Rectangle r) const { return hovered(r) && in_.rightPressed; }

    bool Ui::button(const std::string &id, Rectangle r, const std::string &label, Style style, bool enabled, float textSize)
    {
        return iconButton(id, r, static_cast<Icon>(-1), label, style, enabled, textSize);
    }

    bool Ui::iconButton(const std::string &, Rectangle r, Icon ic, const std::string &label, Style style, bool enabled, float textSize)
    {
        const Theme &t = theme();
        const bool hot = enabled && hovered(r);
        const bool down = hot && in_.down;
        Color bg = t.panel2, fg = t.text, edge = t.border;
        switch (style)
        {
        case Style::Primary:
            bg = t.accent;
#if defined(GT4L_EDITION_FULL)
            fg = Color{6, 24, 26, 255}; // dark text on the Full edition's bright teal
#else
            fg = WHITE;
#endif
            edge = bg;
            break;
        case Style::Play:
            bg = t.red;
            fg = WHITE;
            edge = bg;
            break;
        case Style::Danger:
            bg = Color{90, 30, 34, 255};
            fg = Color{255, 205, 205, 255};
            edge = Color{150, 50, 56, 255};
            break;
        case Style::Ghost:
            bg = Color{0, 0, 0, 0};
            fg = t.accent;
            edge = bg;
            break;
        default:
            break;
        }
        if (!enabled)
        {
            bg = style == Style::Ghost ? bg : mixColor(t.panel, t.panel2, 0.5f);
            fg = t.faint;
            edge = t.border;
        }
        else if (hot)
        {
            bg = style == Style::Ghost ? alpha(t.accent, 0.12f) : mixColor(bg, WHITE, down ? 0.04f : 0.10f);
            if (style == Style::Play)
                bg = down ? t.red : t.redHi;
        }
        const float radius = std::min(8.0f, r.height * 0.5f);
        if (bg.a)
            round(r, radius, bg);
        if (style == Style::Secondary || (!enabled && style != Style::Ghost))
            roundLine(r, radius, 1.0f, edge);
        const bool hasIcon = static_cast<int>(ic) >= 0;
        const float iconSize = textSize * 1.25f;
        const float labelW = textWidth(label, textSize, Weight::Semibold);
        const float total = labelW + (hasIcon ? iconSize + (label.empty() ? 0.0f : 8.0f) : 0.0f);
        float x = r.x + (r.width - total) * 0.5f;
        if (hasIcon)
        {
            icon(ic, Rectangle{x, r.y + (r.height - iconSize) * 0.5f, iconSize, iconSize}, fg);
            x += iconSize + 8.0f;
        }
        text(label, x, r.y + (r.height - textSize * 1.33f) * 0.5f, textSize, fg, Weight::Semibold);
        return enabled && clicked(r);
    }

    bool Ui::toggle(const std::string &, float x, float y, bool &value, bool enabled)
    {
        const Theme &t = theme();
        const Rectangle r{x, y, 40.0f, 22.0f};
        const bool hot = enabled && hovered(r);
        Color track = value ? t.accent : t.panel3;
        if (!enabled)
            track = value ? t.accentDim : t.panel2;
        else if (hot)
            track = mixColor(track, WHITE, 0.08f);
        round(r, 11.0f, track);
        const float kx = value ? x + 29.0f : x + 11.0f;
        circle(kx, y + 11.0f, 8.0f, enabled ? WHITE : t.muted);
        if (enabled && clicked(r))
        {
            value = !value;
            return true;
        }
        return false;
    }

    bool Ui::checkbox(const std::string &, float x, float y, bool &value, const std::string &label, bool enabled)
    {
        const Theme &t = theme();
        const float labelW = textWidth(label, 14.0f);
        const Rectangle box{x, y, 20.0f, 20.0f};
        const Rectangle hit{x, y - 2.0f, 28.0f + labelW, 24.0f};
        const bool hot = enabled && hovered(hit);
        round(box, 5.0f, value ? (enabled ? t.accent : t.accentDim) : (hot ? t.panel3 : t.panel2));
        if (!value)
            roundLine(box, 5.0f, 1.0f, t.border);
        else
            icon(Icon::Check, Rectangle{x + 2.0f, y + 2.0f, 16.0f, 16.0f}, WHITE);
        text(label, x + 28.0f, y + 1.0f, 14.0f, enabled ? t.text : t.faint);
        if (enabled && clicked(hit))
        {
            value = !value;
            return true;
        }
        return false;
    }

    bool Ui::slider(const std::string &id, Rectangle r, float &value, float lo, float hi, float step, bool enabled, Color fillColor)
    {
        const Theme &t = theme();
        const float cy = r.y + r.height * 0.5f;
        const Rectangle track{r.x, cy - 3.0f, r.width, 6.0f};
        const float frac = hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        const Color fc = fillColor.a ? fillColor : (enabled ? t.accent : t.accentDim);
        round(track, 3.0f, t.panel3);
        round(Rectangle{track.x, track.y, track.width * frac, track.height}, 3.0f, fc);
        const float kx = r.x + r.width * frac;
        bool changed = false;
        const bool hot = enabled && hovered(Rectangle{r.x - 8.0f, r.y, r.width + 16.0f, r.height});
        if (enabled && inputOn() && hot && in_.pressed && !consumedClick_ && openDropdown_.empty())
            active_ = id;
        if (active_ == id)
        {
            if (!in_.down || !enabled)
                active_.clear();
            else
            {
                float v = lo + std::clamp((mouseDp().x - r.x) / r.width, 0.0f, 1.0f) * (hi - lo);
                if (step > 0.0f)
                    v = lo + std::round((v - lo) / step) * step;
                v = std::clamp(v, lo, hi);
                if (v != value)
                {
                    value = v;
                    changed = true;
                }
            }
        }
        if (enabled && hot && in_.wheel != 0.0f && step > 0.0f && active_.empty())
        {
            const float v = std::clamp(value + (in_.wheel > 0 ? step : -step), lo, hi);
            if (v != value)
            {
                value = v;
                changed = true;
            }
        }
        circle(kx, cy, active_ == id ? 10.0f : 9.0f, enabled ? WHITE : t.muted);
        ring(kx, cy, active_ == id ? 10.0f : 9.0f, active_ == id ? 11.0f : 10.0f, alpha(BLACK, 0.25f));
        return changed;
    }

    bool Ui::segmented(const std::string &, Rectangle r, const std::vector<std::string> &options, int &selected, bool enabled, float textSize)
    {
        const Theme &t = theme();
        round(r, 8.0f, t.panel2);
        roundLine(r, 8.0f, 1.0f, t.border);
        const float w = r.width / static_cast<float>(std::max<size_t>(1, options.size()));
        bool changed = false;
        for (size_t i = 0; i < options.size(); ++i)
        {
            const Rectangle cell{r.x + w * static_cast<float>(i) + 3.0f, r.y + 3.0f, w - 6.0f, r.height - 6.0f};
            const bool sel = static_cast<int>(i) == selected;
            const bool hot = enabled && hovered(cell);
            if (sel)
                round(cell, 6.0f, enabled ? t.accent : t.accentDim);
            else if (hot)
                round(cell, 6.0f, t.panel3);
#if defined(GT4L_EDITION_FULL)
            const Color selText = enabled ? Color{6, 24, 26, 255} : t.muted; // dark text on the Full edition's bright teal
#else
            const Color selText = WHITE;
#endif
            text(options[i], cell.x + cell.width * 0.5f, cell.y + (cell.height - textSize * 1.33f) * 0.5f, textSize,
                 sel ? selText : (enabled ? t.text : t.faint), sel ? Weight::Semibold : Weight::Regular, Align::Center);
            if (enabled && !sel && clicked(cell))
            {
                selected = static_cast<int>(i);
                changed = true;
            }
        }
        return changed;
    }

    bool Ui::dropdown(const std::string &id, Rectangle r, const std::vector<std::string> &options, int &selected, bool enabled, float textSize)
    {
        const Theme &t = theme();
        bool changed = false;
        if (dropPickedId_ == id)
        {
            if (dropPicked_ >= 0 && dropPicked_ < static_cast<int>(options.size()) && dropPicked_ != selected)
            {
                selected = dropPicked_;
                changed = true;
            }
            dropPickedId_.clear();
            dropPicked_ = -1;
        }
        const bool open = openDropdown_ == id;
        const bool hot = enabled && (hovered(r) || (open && inputOn() && CheckCollisionPointRec(mouseDp(), r)));
        round(r, 8.0f, enabled ? (hot || open ? t.panel3 : t.panel2) : t.panel);
        roundLine(r, 8.0f, 1.0f, open ? t.accent : t.border);
        const std::string label = selected >= 0 && selected < static_cast<int>(options.size()) ? options[static_cast<size_t>(selected)] : "-";
        text(fit(label, r.width - 40.0f, textSize), r.x + 12.0f, r.y + (r.height - textSize * 1.33f) * 0.5f, textSize, enabled ? t.text : t.faint);
        // chevron
        const float ax = r.x + r.width - 18.0f, ay = r.y + r.height * 0.5f;
        line(ax - 5.0f, ay - 2.5f, ax, ay + 2.5f, 1.8f, enabled ? t.muted : t.faint);
        line(ax, ay + 2.5f, ax + 5.0f, ay - 2.5f, 1.8f, enabled ? t.muted : t.faint);
        if (enabled && inputOn() && in_.pressed && CheckCollisionPointRec(mouseDp(), r))
        {
            if (open)
                openDropdown_.clear();
            else if (openDropdown_.empty() && !consumedClick_)
            {
                openDropdown_ = id;
                dropRect_ = r;
                dropOptions_ = options;
                dropSelected_ = selected;
                dropTextSize_ = textSize;
                consumedClick_ = true;
                dropJustOpened_ = true;
                int rows = 0;
                (void)dropListRect(rows);
                dropFirst_ = std::max(0, selected - rows / 2); // the chosen row in view
            }
        }
        if (open)
        {
            dropRect_ = r;
            dropOptions_ = options;
            dropSelected_ = selected;
            dropTextSize_ = textSize;
        }
        return changed;
    }

    bool Ui::rowButton(const std::string &, Rectangle r, bool selected, bool enabled)
    {
        const Theme &t = theme();
        const bool hot = enabled && hovered(r);
        if (selected)
        {
            round(r, 8.0f, alpha(t.accent, 0.16f));
            roundLine(r, 8.0f, 1.0f, alpha(t.accent, 0.7f));
        }
        else if (hot)
            round(r, 8.0f, t.panel2);
        return enabled && clicked(r);
    }

    void Ui::card(Rectangle r, const std::string &title, const std::string &note)
    {
        const Theme &t = theme();
        round(r, 12.0f, t.panel);
        roundLine(r, 12.0f, 1.0f, alpha(t.border, 0.8f));
        if (!title.empty())
        {
            text(title, r.x + 20.0f, r.y + 16.0f, 16.5f, t.text, Weight::Semibold);
            if (!note.empty())
                text(fit(note, r.width - 60.0f - textWidth(title, 16.5f, Weight::Semibold), 13.0f), r.x + r.width - 20.0f, r.y + 19.0f, 13.0f,
                     t.faint, Weight::Regular, Align::Right);
        }
    }

    float Ui::chip(float x, float y, const std::string &t, Color bg, Color fg, float size)
    {
        const float w = textWidth(t, size, Weight::Semibold) + 18.0f;
        const float h = size * 1.33f + 8.0f;
        round(Rectangle{x, y, w, h}, h * 0.5f, bg);
        text(t, x + 9.0f, y + 4.0f, size, fg, Weight::Semibold);
        return w;
    }

    void Ui::meter(Rectangle r, float v, Color c)
    {
        const Theme &t = theme();
        round(r, r.height * 0.5f, t.panel3);
        const float f = std::clamp(v, 0.0f, 1.0f);
        if (f > 0.0f)
            round(Rectangle{r.x, r.y, std::max(r.height, r.width * f), r.height}, r.height * 0.5f, c);
    }

    void Ui::centreMeter(Rectangle r, float v, Color c)
    {
        const Theme &t = theme();
        round(r, r.height * 0.5f, t.panel3);
        const float mid = r.x + r.width * 0.5f;
        const float f = std::clamp(v, -1.0f, 1.0f);
        const float w = std::fabs(f) * r.width * 0.5f;
        if (w > 0.5f)
            round(Rectangle{f < 0 ? mid - w : mid, r.y, w, r.height}, r.height * 0.5f, c);
        fill(Rectangle{mid - 1.0f, r.y - 3.0f, 2.0f, r.height + 6.0f}, t.muted);
    }

    void Ui::statusDot(float cx, float cy, Color c)
    {
        circle(cx, cy, 5.0f, alpha(c, 0.25f));
        circle(cx, cy, 3.2f, c);
    }
}
