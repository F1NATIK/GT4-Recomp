#pragma once

// The race HUD boxes' look (header-only; 20 Oct 2026, analysis/design/hud-gt3-look): GT3's HUD look (src/lib/gt3_hud.cpp,
// gt3_radar.cpp, the GT3 start lights in ps2_runtime.cpp) for our own boxes. GT4.exe draws them over the race with it
// (src/lib/gt4_hud.cpp) and the launcher's Driving > Race HUD previews draw their sample boxes with the very same calls
// (gt4full_racehud.cpp), so the two look alike: Segoe UI Bold (GT3's: one 96 px atlas, mipmapped, trilinear), GT3's
// colours, black panels with GT3's rounding, and every shape's edge smoothed (a half-pixel fade drawn with rlgl: GT4's
// window has no multisampling; the launcher's has, which this does not mind).
//
// Everything here is in window pixels: r is a box's rectangle, its sizes come from the box (shared/gt4_hud_boxes.h).
// Only the look lives here; what a box shows (the race's numbers, or the launcher's samples) is the caller's.

#include "gt4_hud_boxes.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace gt4hudlook
{
    // ---- GT3's colours (gt3_hud.cpp)
    inline constexpr Color kText{235, 238, 244, 255};
    inline constexpr Color kMuted{150, 158, 172, 255};
    inline constexpr Color kAccent{255, 184, 48, 255};
    inline constexpr Color kGreen{70, 220, 110, 255};
    inline constexpr Color kYellow{255, 210, 60, 255};
    inline constexpr Color kRed{255, 70, 60, 255};
    inline constexpr Color kPurple{190, 110, 255, 255};
    inline constexpr Color kCyan{120, 220, 255, 255};   // GT3's player colour (the g-meter's dot here)
    inline constexpr Color kBadge{230, 232, 238, 255};  // the timing tower's place badges
    inline constexpr Color kInk{20, 22, 26, 255};       // numbers on a badge or a dot
    inline constexpr Color kReverse{235, 60, 50, 255};  // GT3's red R

    inline Color hex(uint32_t v, float a = 1.0f)
    {
        return Color{static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v),
                     static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)};
    }
    // c with its alpha times a.
    inline Color fade(Color c, float a)
    {
        c.a = static_cast<unsigned char>(std::clamp(static_cast<float>(c.a) * std::clamp(a, 0.0f, 1.0f), 0.0f, 255.0f) + 0.5f);
        return c;
    }
    inline Color me(float a = 1.0f) { return hex(gt4hudbox::kMe, a); }
    inline Color carColour(int car, bool player) { return player ? me() : hex(gt4hudbox::kCarColours[(car % 16 + 16) % 16]); }

    // ---- text: GT3's font (Segoe UI Bold at 96 px, mipmaps, trilinear), raylib's own font when Windows has none
    inline Font &font()
    {
        static Font f = [] {
            const char *path = "C:\\Windows\\Fonts\\segoeuib.ttf";
            if (FileExists(path))
            {
                Font loaded = LoadFontEx(path, 96, nullptr, 0);
                if (loaded.texture.id)
                {
                    GenTextureMipmaps(&loaded.texture);
                    SetTextureFilter(loaded.texture, TEXTURE_FILTER_TRILINEAR);
                    return loaded;
                }
            }
            return GetFontDefault();
        }();
        return f;
    }
    inline float textWidth(const std::string &s, float size) { return s.empty() ? 0.0f : MeasureTextEx(font(), s.c_str(), size, 1.0f).x; }
    // s with its middle at height cy (digits and capitals centred on it), at x by align (0 left, 0.5 centre, 1 right).
    inline void text(const std::string &s, float x, float cy, float size, Color c, float align = 0.5f)
    {
        if (s.empty() || size < 1.0f || c.a == 0)
            return;
        const Vector2 m = MeasureTextEx(font(), s.c_str(), size, 1.0f);
        DrawTextEx(font(), s.c_str(), Vector2{std::round(x - m.x * align), std::round(cy - m.y * 0.5f - size * 0.05f)}, size, 1.0f, c);
    }
    // The size (at most `size`) at which s fits `width`.
    inline float fitSize(const std::string &s, float size, float width)
    {
        const float w = textWidth(s, size);
        return w > width && w > 0.0f ? size * width / w : size;
    }
    // s cut to fit `width` at `size` (GT3's: the end dropped, no dots).
    inline std::string fitted(std::string s, float size, float width)
    {
        while (s.size() > 1u && textWidth(s, size) > width)
            s.pop_back();
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    }

    // ---- smoothed shapes: each a convex outline, filled, its edge faded over a pixel
    namespace detail
    {
        constexpr int kMaxPts = 160;
        struct Path
        {
            Vector2 p[kMaxPts];
            int n = 0;
            void add(float x, float y)
            {
                if (n < kMaxPts)
                    p[n++] = Vector2{x, y};
            }
        };
        inline void vtx(Vector2 v, Color c)
        {
            rlColor4ub(c.r, c.g, c.b, c.a);
            rlVertex2f(v.x, v.y);
        }
        // One triangle the way raylib draws them (counter-clockwise as seen; y down: a negative cross product).
        inline void tri(Vector2 a, Color ca, Vector2 b, Color cb, Vector2 c, Color cc)
        {
            if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0.0f)
            {
                std::swap(b, c);
                std::swap(cb, cc);
            }
            vtx(a, ca), vtx(b, cb), vtx(c, cc);
        }
        // Each point's outward miter (unit edge normals averaged, lengthened at the corners).
        inline void miters(const Path &P, Vector2 *out)
        {
            float area = 0.0f;
            for (int i = 0; i < P.n; ++i)
            {
                const Vector2 a = P.p[i], b = P.p[(i + 1) % P.n];
                area += a.x * b.y - b.x * a.y;
            }
            const float s = area >= 0.0f ? 1.0f : -1.0f; // (y down: area > 0 is clockwise as seen)
            for (int i = 0; i < P.n; ++i)
            {
                const Vector2 a = P.p[(i + P.n - 1) % P.n], b = P.p[i], c = P.p[(i + 1) % P.n];
                auto normal = [&](Vector2 u, Vector2 v) {
                    float dx = v.x - u.x, dy = v.y - u.y;
                    const float l = std::sqrt(dx * dx + dy * dy);
                    if (l < 1e-6f)
                        return Vector2{0.0f, 0.0f};
                    dx /= l, dy /= l;
                    return Vector2{dy * s, -dx * s};
                };
                Vector2 n1 = normal(a, b), n2 = normal(b, c);
                if (n1.x == 0.0f && n1.y == 0.0f)
                    n1 = n2;
                if (n2.x == 0.0f && n2.y == 0.0f)
                    n2 = n1;
                Vector2 m{n1.x + n2.x, n1.y + n2.y};
                const float ml = std::sqrt(m.x * m.x + m.y * m.y);
                if (ml < 1e-6f)
                    m = n1;
                else
                {
                    m.x /= ml, m.y /= ml;
                    const float d = std::max(0.35f, m.x * n1.x + m.y * n1.y);
                    m.x /= d, m.y /= d;
                }
                out[i] = m;
            }
        }
        // The path filled (convex), its edge faded from half a pixel inside to half a pixel outside.
        inline void fill(const Path &P, Color c)
        {
            if (P.n < 3 || c.a == 0)
                return;
            Vector2 m[kMaxPts];
            miters(P, m);
            Vector2 ctr{0.0f, 0.0f};
            for (int i = 0; i < P.n; ++i)
                ctr.x += P.p[i].x, ctr.y += P.p[i].y;
            ctr.x /= static_cast<float>(P.n), ctr.y /= static_cast<float>(P.n);
            const Color clear = fade(c, 0.0f);
            rlCheckRenderBatchLimit(P.n * 9);
            rlSetTexture(rlGetTextureIdDefault()); // (a draw of its own when the last one had a picture)
            rlBegin(RL_TRIANGLES);
            for (int i = 0; i < P.n; ++i)
            {
                const int j = (i + 1) % P.n;
                const Vector2 ii{P.p[i].x - m[i].x * 0.5f, P.p[i].y - m[i].y * 0.5f}, ij{P.p[j].x - m[j].x * 0.5f, P.p[j].y - m[j].y * 0.5f};
                const Vector2 oi{P.p[i].x + m[i].x * 0.5f, P.p[i].y + m[i].y * 0.5f}, oj{P.p[j].x + m[j].x * 0.5f, P.p[j].y + m[j].y * 0.5f};
                tri(ctr, c, ii, c, ij, c);
                tri(ii, c, oi, clear, oj, clear);
                tri(ii, c, oj, clear, ij, c);
            }
            rlEnd();
            rlSetTexture(0);
        }
        // A band `thick` wide along the path (closed), both edges faded.
        inline void stroke(const Path &P, float thick, Color c)
        {
            if (P.n < 3 || c.a == 0)
                return;
            if (thick < 1.0f)
                c = fade(c, std::max(thick, 0.0f)), thick = 1.0f;
            Vector2 m[kMaxPts];
            miters(P, m);
            const Color clear = fade(c, 0.0f);
            const float h = thick * 0.5f;
            const float d[4] = {-h - 0.5f, -h + 0.5f, h - 0.5f, h + 0.5f};
            const Color col[4] = {clear, c, c, clear};
            rlCheckRenderBatchLimit(P.n * 18);
            rlSetTexture(rlGetTextureIdDefault()); // (a draw of its own when the last one had a picture)
            rlBegin(RL_TRIANGLES);
            for (int i = 0; i < P.n; ++i)
            {
                const int j = (i + 1) % P.n;
                for (int b = 0; b < 3; ++b)
                {
                    const Vector2 a0{P.p[i].x + m[i].x * d[b], P.p[i].y + m[i].y * d[b]}, a1{P.p[i].x + m[i].x * d[b + 1], P.p[i].y + m[i].y * d[b + 1]};
                    const Vector2 b0{P.p[j].x + m[j].x * d[b], P.p[j].y + m[j].y * d[b]}, b1{P.p[j].x + m[j].x * d[b + 1], P.p[j].y + m[j].y * d[b + 1]};
                    tri(a0, col[b], a1, col[b + 1], b1, col[b + 1]);
                    tri(a0, col[b], b1, col[b + 1], b0, col[b]);
                }
            }
            rlEnd();
            rlSetTexture(0);
        }
        inline void roundPath(Path &P, Rectangle r, float radius)
        {
            radius = std::clamp(radius, 0.0f, std::min(r.width, r.height) * 0.5f);
            const int seg = radius < 0.5f ? 0 : std::clamp(static_cast<int>(radius * 0.6f) + 2, 3, 24);
            const float cx[4] = {r.x + r.width - radius, r.x + r.width - radius, r.x + radius, r.x + radius};
            const float cy[4] = {r.y + radius, r.y + r.height - radius, r.y + r.height - radius, r.y + radius};
            for (int k = 0; k < 4; ++k)
            {
                const float a0 = -1.5707963f + 1.5707963f * static_cast<float>(k);
                if (seg == 0)
                {
                    P.add(cx[k], cy[k]);
                    continue;
                }
                for (int s = 0; s <= seg; ++s)
                {
                    const float a = a0 + 1.5707963f * static_cast<float>(s) / static_cast<float>(seg);
                    P.add(cx[k] + std::cos(a) * radius, cy[k] + std::sin(a) * radius);
                }
            }
        }
        inline void circlePath(Path &P, Vector2 c, float r)
        {
            const int n = std::clamp(static_cast<int>(r * 0.9f) + 12, 16, kMaxPts);
            for (int i = 0; i < n; ++i)
            {
                const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
                P.add(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
            }
        }
    } // namespace detail

    // A rounded box: roundness as raylib's (0..1 of the shorter side's half).
    inline void roundRect(Rectangle r, float roundness, Color c)
    {
        if (r.width <= 0.0f || r.height <= 0.0f)
            return;
        detail::Path P;
        detail::roundPath(P, r, roundness * std::min(r.width, r.height) * 0.5f);
        detail::fill(P, c);
    }
    inline void roundRectLines(Rectangle r, float roundness, float thick, Color c)
    {
        if (r.width <= 0.0f || r.height <= 0.0f)
            return;
        detail::Path P;
        detail::roundPath(P, r, roundness * std::min(r.width, r.height) * 0.5f);
        detail::stroke(P, thick, c);
    }
    inline void rect(Rectangle r, Color c) { roundRect(r, 0.0f, c); }
    inline void circle(Vector2 c, float r, Color col)
    {
        if (r <= 0.0f)
            return;
        if (r < 1.0f)
            col = fade(col, r), r = 1.0f;
        detail::Path P;
        detail::circlePath(P, c, r);
        detail::fill(P, col);
    }
    // A circle's outline, `thick` wide, centred on radius r.
    inline void ring(Vector2 c, float r, float thick, Color col)
    {
        if (r <= 0.0f)
            return;
        detail::Path P;
        detail::circlePath(P, c, r);
        detail::stroke(P, thick, col);
    }
    inline void line(Vector2 a, Vector2 b, float thick, Color c)
    {
        float dx = b.x - a.x, dy = b.y - a.y;
        const float l = std::sqrt(dx * dx + dy * dy);
        if (l < 1e-4f)
            return;
        if (thick < 1.0f)
            c = fade(c, std::max(thick, 0.0f)), thick = 1.0f;
        dx /= l, dy /= l;
        const float h = thick * 0.5f;
        detail::Path P;
        P.add(a.x - dy * h, a.y + dx * h), P.add(b.x - dy * h, b.y + dx * h), P.add(b.x + dy * h, b.y - dx * h), P.add(a.x + dy * h, a.y - dx * h);
        detail::fill(P, c);
    }
    // A box w x h about its centre, turned deg degrees clockwise (the radar's cars).
    inline void turnedRect(Vector2 c, float w, float h, float deg, Color col)
    {
        const float a = deg * 0.017453292f, ca = std::cos(a), sa = std::sin(a);
        const float hx[4] = {-w * 0.5f, w * 0.5f, w * 0.5f, -w * 0.5f}, hy[4] = {-h * 0.5f, -h * 0.5f, h * 0.5f, h * 0.5f};
        detail::Path P;
        for (int i = 0; i < 4; ++i)
            P.add(c.x + hx[i] * ca - hy[i] * sa, c.y + hx[i] * sa + hy[i] * ca);
        detail::fill(P, col);
    }

    // ---- the boxes' parts
    // The box's panel: GT3's black at the box's opacity, its corners rounded as GT3's widgets (0.18).
    inline void panel(Rectangle r, float alpha) { roundRect(r, 0.18f, fade(BLACK, alpha)); }
    // A car on a map: a dot in its colour with its place in it (GT3's map dots).
    inline void mapDot(Vector2 c, float rad, Color col, int place)
    {
        circle(c, rad + 1.0f, fade(BLACK, 0.55f));
        circle(c, rad, col);
        if (rad >= 4.0f && place > 0)
            text(std::to_string(place), c.x, c.y, rad * 1.4f, kInk);
    }
    // You on a turning map: GT3's arrow, pointing the way you go (up).
    inline void mapArrow(Vector2 c, float a, Color col)
    {
        // (GT3's arrow: the tip a ahead, the back corners 0.6 a behind and 0.62 a to the sides; a dark edge under it)
        detail::Path O, A;
        O.add(c.x, c.y - a - 1.3f), O.add(c.x + a * 0.62f + 1.2f, c.y + a * 0.6f + 0.8f), O.add(c.x - a * 0.62f - 1.2f, c.y + a * 0.6f + 0.8f);
        A.add(c.x, c.y - a), A.add(c.x + a * 0.62f, c.y + a * 0.6f), A.add(c.x - a * 0.62f, c.y + a * 0.6f);
        detail::fill(O, fade(BLACK, 0.45f));
        detail::fill(A, col);
    }

    // ---- the boxes
    // Speed: the number, its unit under it in GT3's accent.
    inline void speed(Rectangle r, float alpha, const std::string &value, const std::string &unit)
    {
        panel(r, alpha);
        const float cx = r.x + r.width * 0.5f;
        text(value, cx, r.y + r.height * 0.42f, fitSize(value, r.height * 0.62f, r.width * 0.88f), kText);
        text(unit, cx, r.y + r.height * 0.82f, r.height * 0.2f, kAccent);
    }
    // The gear: GT3's accent, a red R in reverse.
    inline void gear(Rectangle r, float alpha, const std::string &g, bool reverse)
    {
        panel(r, alpha);
        text(g, r.x + r.width * 0.5f, r.y + r.height * 0.5f, fitSize(g, r.height * 0.85f, r.width * 0.9f), reverse ? kReverse : kAccent);
    }
    // The lap counter: LAP n /total (GT3's), and with the clock (style 1) the race time on its right.
    inline void lap(Rectangle r, float alpha, int lapNow, int laps, const std::string *clock)
    {
        panel(r, alpha);
        const float bh = r.height, w1 = clock ? r.width / 1.7f : r.width, cy = r.y + bh * 0.5f;
        const float ls = bh * 0.36f;
        text("LAP", r.x + w1 * 0.22f, cy, ls, kMuted);
        const float labelRight = r.x + w1 * 0.22f + textWidth("LAP", ls) * 0.5f;
        const std::string n = std::to_string(lapNow), of = "/" + std::to_string(laps);
        const float numRight = r.x + w1 * 0.62f;
        const float ns = fitSize(n, bh * 0.7f, numRight - labelRight - w1 * 0.04f);
        text(n, numRight, cy, ns, kText, 1.0f);
        text(of, numRight + w1 * 0.01f, cy + bh * 0.1f, fitSize(of, bh * 0.4f, w1 * 0.33f), kMuted, 0.0f);
        if (clock)
        {
            line(Vector2{r.x + w1, r.y + bh * 0.22f}, Vector2{r.x + w1, r.y + bh * 0.78f}, std::max(1.0f, bh * 0.02f), fade(WHITE, 0.15f));
            const float cw = r.width - w1;
            text(*clock, r.x + w1 + cw * 0.5f, cy, fitSize(*clock, bh * 0.42f, cw * 0.86f), kText);
        }
    }
    // A split cell: its time, the time's colour and the bar's under it.
    struct Split
    {
        std::string value;
        Color valueCol, barCol;
    };
    // GT3's split colours: 0 purple (the best of every car), 1 green (your best), 2 yellow; a cell shown again for the lap
    // just done (prev) has its bar fainter.
    inline Split splitDone(const std::string &v, int colour, bool prev = false)
    {
        const Color c = colour <= 0 ? kPurple : colour == 1 ? kGreen : kYellow;
        return Split{v, c, prev ? fade(c, 0.55f) : c};
    }
    inline Split splitRunning(const std::string &v) { return Split{v, kText, fade(kText, 0.5f)}; }
    inline Split splitNone() { return Split{"--.---", kMuted, fade(WHITE, 0.15f)}; }
    // The split times: a column per sector, S1.. over each, its time, a coloured bar under it (GT3's Splits).
    inline void splits(Rectangle r, float alpha, const Split *cells, int n)
    {
        panel(r, alpha);
        n = std::max(1, n);
        const float bw = r.width, bh = r.height, margin = bw * 0.035f, pitch = (bw - margin * 2.0f) / static_cast<float>(n), colW = pitch * 0.968f;
        for (int k = 0; k < n; ++k)
        {
            const float colX = r.x + margin + pitch * static_cast<float>(k), mx = colX + colW * 0.5f;
            text("S" + std::to_string(k + 1), mx, r.y + bh * 0.2f, bh * 0.2f, kMuted);
            text(cells[k].value, mx, r.y + bh * 0.53f, fitSize(cells[k].value, bh * 0.34f, colW * 0.9f), cells[k].valueCol);
            roundRect(Rectangle{colX + colW * 0.08f, r.y + bh * 0.8f, colW * 0.84f, std::max(2.0f, bh * 0.08f)}, 1.0f, cells[k].barCol);
        }
    }
    // The rev lights (GT3's shift lights): 10 lights, 1 at 85 % of the rev limit and 10 at 98 %, all flashing blue at the
    // limiter; style 0 with GT3's rev bar under them. f: rpm / the limit; flashOn: the flash's lit half.
    inline void revs(Rectangle r, float alpha, int style, float f, bool flashOn)
    {
        panel(r, alpha);
        const float bw = r.width, bh = r.height, lightsH = style == 1 ? bh : bh * 0.64f;
        const bool limiter = f >= 0.995f;
        const float d = std::min(lightsH * 0.7f, bw / 11.5f), cy = r.y + lightsH * (style == 1 ? 0.5f : 0.56f);
        for (int i = 0; i < 10; ++i)
        {
            const float on = 0.85f + 0.13f * static_cast<float>(i) / 9.0f;
            const Color col = limiter ? Color{80, 150, 255, 255} : (i < 4 ? Color{60, 220, 90, 255} : (i < 7 ? Color{255, 210, 50, 255} : Color{255, 60, 50, 255}));
            const bool lit = limiter ? flashOn : f >= on;
            circle(Vector2{r.x + bw * (0.5f + (static_cast<float>(i) - 4.5f) * (1.0f / 10.8f)), cy}, d * 0.42f, lit ? col : fade(col, 0.15f));
        }
        if (style != 1)
        {
            const float by = r.y + bh * 0.72f, barH = std::max(2.0f, bh * 0.13f);
            const Rectangle bar{r.x + bw * 0.06f, by, bw * 0.88f, barH};
            roundRect(bar, 1.0f, fade(WHITE, 0.12f));
            const float fc = std::clamp(f, 0.0f, 1.0f);
            if (fc > 0.0f)
                roundRect(Rectangle{bar.x, bar.y, std::max(barH, bar.width * fc), barH}, 1.0f, fc < 0.7f ? kGreen : fc < 0.9f ? kYellow : kRed);
            const float redX = bar.x + bar.width * 0.85f;
            rect(Rectangle{redX, by - barH * 0.35f, std::max(1.0f, bh * 0.025f), barH * 1.7f}, fade(kRed, 0.8f));
        }
    }
    // The delta to your best lap: sign -1 ahead (green), 1 behind (red), 0 none yet (muted); style 0 with a bar from the
    // middle (full at 0.5 s: frac -1..1), 1 the number only.
    inline void delta(Rectangle r, float alpha, int style, const std::string &value, int sign, float frac)
    {
        panel(r, alpha);
        const Color col = sign < 0 ? kGreen : sign > 0 ? kRed : kMuted;
        const float bw = r.width, bh = r.height;
        if (style == 1)
        {
            text(value, r.x + bw * 0.5f, r.y + bh * 0.5f, fitSize(value, bh * 0.72f, bw * 0.9f), col);
            return;
        }
        text(value, r.x + bw * 0.5f, r.y + bh * 0.38f, fitSize(value, bh * 0.52f, bw * 0.9f), col);
        const float barY = r.y + bh * 0.74f, barH = std::max(2.0f, bh * 0.12f), mid = r.x + bw * 0.5f, half = bw * 0.44f;
        roundRect(Rectangle{mid - half, barY, half * 2.0f, barH}, 1.0f, fade(WHITE, 0.12f));
        frac = std::clamp(frac, -1.0f, 1.0f);
        if (frac < 0.0f)
            roundRect(Rectangle{mid + frac * half, barY, -frac * half, barH}, 1.0f, kGreen);
        else if (frac > 0.0f)
            roundRect(Rectangle{mid, barY, frac * half, barH}, 1.0f, kRed);
        rect(Rectangle{mid - std::max(1.0f, bh * 0.02f), barY - barH * 0.45f, std::max(2.0f, bh * 0.04f), barH * 1.9f}, fade(WHITE, 0.6f));
    }
    // The pedals: brake and throttle, rounded bars filling up (GT3's bars), red and green.
    inline void pedals(Rectangle r, float alpha, float brake, float throttle)
    {
        panel(r, alpha);
        const float pad = r.width * 0.16f, bw = (r.width - pad * 3.0f) * 0.5f, top = r.y + pad * 1.1f, h = r.height - pad * 2.2f;
        const float vals[2] = {std::clamp(brake, 0.0f, 1.0f), std::clamp(throttle, 0.0f, 1.0f)};
        const Color cols[2] = {kRed, kGreen};
        for (int i = 0; i < 2; ++i)
        {
            const float x = r.x + pad + static_cast<float>(i) * (bw + pad);
            roundRect(Rectangle{x, top, bw, h}, 1.0f, fade(WHITE, 0.12f));
            if (vals[i] > 0.005f)
                roundRect(Rectangle{x, top + h * (1.0f - vals[i]), bw, std::max(bw, h * vals[i])}, 1.0f, cols[i]);
        }
    }
    // The g-meter: rings at 0.5, 1 and 1.5 g (GT3's guide lines), the dot (GT3's player cyan) with its trail (style 0), the g.
    // trail: the last points in g (x right, y forward), the newest last.
    inline void gmeter(Rectangle r, float alpha, int style, const Vector2 *trail, int n, const std::string &g)
    {
        panel(r, alpha);
        const float W = r.width, R = W * 0.5f - W * 0.06f, th = std::max(1.0f, W / 200.0f);
        const Vector2 c0{r.x + W * 0.5f, r.y + W * 0.5f};
        for (float f : {1.0f / 3.0f, 2.0f / 3.0f, 1.0f})
            ring(c0, R * f, th, fade(WHITE, f == 1.0f ? 0.3f : 0.18f));
        line(Vector2{c0.x, c0.y - R}, Vector2{c0.x, c0.y + R}, th, fade(WHITE, 0.18f));
        line(Vector2{c0.x - R, c0.y}, Vector2{c0.x + R, c0.y}, th, fade(WHITE, 0.18f));
        auto to = [&](Vector2 v) { return Vector2{c0.x + std::clamp(v.x / 1.5f, -1.0f, 1.0f) * R, c0.y - std::clamp(v.y / 1.5f, -1.0f, 1.0f) * R}; };
        for (int i = 0; i < n - 1 && style != 1; ++i)
            circle(to(trail[i]), W * 0.014f, fade(kCyan, static_cast<float>(i + 1) / static_cast<float>(n) * 0.5f));
        if (n > 0)
        {
            const Vector2 p = to(trail[n - 1]);
            circle(p, W * 0.036f, fade(BLACK, 0.55f));
            circle(p, W * 0.028f, kCyan);
            text(g, r.x + W * 0.95f, r.y + W * 0.9f, W * 0.1f, kMuted, 1.0f);
        }
    }
    // The start lights (GT3's housing and lamps): n columns of `rows` lamps, the first `lit` columns on; fade 0..1 (going out).
    inline void lights(Rectangle r, int n, int rows, int lit, float fadeOut)
    {
        rows = std::max(1, rows);
        const float cell = r.height / static_cast<float>(rows), gap = (r.width - cell * static_cast<float>(n)) / static_cast<float>(n + 1);
        roundRect(r, 0.22f, fade(Color{12, 12, 14, 235}, fadeOut));
        const float in = cell * 0.08f;
        roundRect(Rectangle{r.x + in, r.y + in, r.width - in * 2.0f, r.height - in * 2.0f}, 0.2f, fade(Color{26, 26, 30, 235}, fadeOut));
        const float rad = cell * 0.34f;
        for (int row = 0; row < rows; ++row)
            for (int k = 1; k <= n; ++k)
            {
                const bool on = k <= lit;
                const Vector2 c{r.x + gap * static_cast<float>(k) + cell * (static_cast<float>(k) - 0.5f), r.y + cell * (static_cast<float>(row) + 0.5f)};
                if (on)
                    circle(c, rad * 1.55f, fade(Color{255, 40, 20, 70}, fadeOut));
                circle(c, rad, fade(on ? Color{255, 36, 22, 255} : Color{58, 18, 18, 255}, fadeOut));
                if (on)
                    circle(Vector2{c.x - rad * 0.3f, c.y - rad * 0.3f}, rad * 0.28f, fade(Color{255, 170, 150, 200}, fadeOut));
            }
    }

    // The timing tower's rows: GT3's (a place badge, gold for the leader; the car's colour dot; the name; the times).
    struct TowerRow
    {
        int place = 0, car = 0;
        bool player = false, purpleBest = false;
        std::string name, vals[4]; // gap, int, best, last
    };
    // r: the box (its top, left and width; the panel is drawn behind the rows given only); k: the box's height unit (the HUD
    // area's height times the box's size); name: 0 car, 1 short, 2 driver; cols: gt4_hud_boxes.h's bits.
    inline void tower(Rectangle r, float alpha, float k, int name, int cols, const TowerRow *rows, int n)
    {
        const float rowH = gt4hudbox::kTowerRow * k, head = gt4hudbox::kTowerHead * k, pad = gt4hudbox::kTowerPad * k;
        panel(Rectangle{r.x, r.y, r.width, std::min(r.height, head + rowH * static_cast<float>(n))}, alpha);
        const float D = gt4hudbox::kTowerDot * k, fs = rowH * 0.5f, hs = std::min(head * 0.66f, rowH * 0.4f);
        const float x0 = r.x + pad, nameX = x0 + D * 1.1f, nameW = gt4hudbox::kTowerNameW[std::clamp(name, 0, 2)] * k - D * 0.1f;
        float colRight[4] = {};
        float x = x0 + D + gt4hudbox::kTowerNameW[std::clamp(name, 0, 2)] * k;
        for (int c = 0; c < 4; ++c)
            if ((cols >> c) & 1)
                x += gt4hudbox::kTowerColW[c] * k, colRight[c] = x - pad * 0.3f;
        const float badgeW = std::min(rowH * 0.78f, D * 0.58f), badgeX = x0 + D * 0.02f;
        const float headY = r.y + head * 0.55f;
        text("POS", badgeX + badgeW * 0.5f, headY, fitSize("POS", hs, D * 0.9f), kMuted);
        static const char *const kHead[4] = {"GAP", "INT", "BEST", "LAST"};
        for (int c = 0; c < 4; ++c)
            if ((cols >> c) & 1)
                text(kHead[c], colRight[c], headY, hs, kMuted, 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const TowerRow &t = rows[i];
            const float y = r.y + head + rowH * static_cast<float>(i), mid = y + rowH * 0.5f;
            if (t.player)
                roundRect(Rectangle{r.x + pad * 0.35f, y + rowH * 0.06f, r.width - pad * 0.7f, rowH * 0.88f}, 0.3f, me(0.22f));
            const Rectangle badge{badgeX, y + rowH * 0.14f, badgeW, rowH * 0.72f};
            roundRect(badge, 0.25f, t.place == 1 ? kAccent : kBadge);
            const std::string p = std::to_string(t.place);
            text(p, badge.x + badge.width * 0.5f, mid, fitSize(p, rowH * 0.56f, badgeW * 0.9f), kInk);
            const float dotR = std::min(rowH * 0.18f, D * 0.16f);
            const Vector2 dot{x0 + D * 0.82f, mid};
            circle(dot, dotR + 1.0f, fade(BLACK, 0.5f));
            circle(dot, dotR, carColour(t.car, t.player));
            text(fitted(t.name, fs, nameW - pad * 0.5f), nameX, mid, fs, t.player ? me() : kText, 0.0f);
            for (int c = 0; c < 4; ++c)
                if ((cols >> c) & 1)
                {
                    Color cc = kText;
                    if (c == 0 && t.vals[0] == "LEAD")
                        cc = kAccent;
                    if (c == 2 && t.purpleBest)
                        cc = kPurple;
                    text(t.vals[c], colRight[c], mid, fitSize(t.vals[c], fs, gt4hudbox::kTowerColW[c] * k - pad * 0.4f), cc, 1.0f);
                }
        }
    }

    // ---- the radar (GT3's: its box, outline and guide lines; GT4's rings and their metres, round or square)
    struct RadarCar
    {
        float left, fwd; // metres from you (left of you, ahead of you)
        float deg;       // turned against you, degrees clockwise
        Color col;
    };
    // r: the square box; bg: the background (its colour at its opacity); vis: how shown (near: fading); range: metres to the
    // edge; labels: the metres at the rings (labelRange their range); you: your car's colour; side: the bars' colour.
    inline void radar(Rectangle r, int style, Color bg, float vis, float range, bool labels, float labelRange, const RadarCar *cars, int n, Color you,
                      bool sideL, bool sideR, Color side)
    {
        const float W = r.width, R = W * 0.5f - W * 0.03f, sc = R / std::max(range, 1.0f);
        const Vector2 c0{r.x + W * 0.5f, r.y + W * 0.5f};
        roundRect(r, 0.12f, fade(bg, vis));
        const float lineA = std::max(bg.a / 255.0f, 0.45f) * vis;
        roundRectLines(r, 0.12f, std::max(1.0f, W / 220.0f), fade(WHITE, 0.35f * lineA));
        const float th = std::max(1.0f, W / 300.0f);
        const Color guide = fade(WHITE, 0.18f * vis);
        static const float kRings[3] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};
        if (style == 1)
        {
            rect(Rectangle{c0.x - R, c0.y - R, R * 2.0f, R * 2.0f}, fade(WHITE, 0.03f * vis));
            for (float f : kRings)
                roundRectLines(Rectangle{c0.x - f * R, c0.y - f * R, f * R * 2.0f, f * R * 2.0f}, 0.0f, th, guide);
        }
        else
        {
            circle(c0, R, fade(WHITE, 0.03f * vis));
            for (float f : kRings)
                ring(c0, f * R, th, guide);
        }
        line(Vector2{c0.x, c0.y - R}, Vector2{c0.x, c0.y + R}, th, guide);
        line(Vector2{c0.x - R, c0.y}, Vector2{c0.x + R, c0.y}, th, guide);
        if (labels)
        {
            const float fs = std::max(9.0f, W * 0.05f);
            for (float f : kRings)
                text(std::to_string(static_cast<int>(std::lround(labelRange * f))) + " m", c0.x + W * 0.02f, c0.y - f * R + fs * 0.62f, fs,
                     fade(kMuted, 0.85f * vis), 0.0f);
        }
        const float carW = std::max(3.0f, 1.8f * sc), carL = std::max(6.0f, 4.2f * sc);
        for (int i = 0; i < n; ++i)
            turnedRect(Vector2{c0.x - cars[i].left * sc, c0.y - cars[i].fwd * sc}, carW, carL, cars[i].deg, cars[i].col);
        turnedRect(c0, carW, carL, 0.0f, you);
        const float bwid = W * 0.035f;
        if (sideL)
            roundRect(Rectangle{r.x + W * 0.012f, c0.y - W * 0.18f, bwid, W * 0.36f}, 1.0f, side);
        if (sideR)
            roundRect(Rectangle{r.x + W - W * 0.012f - bwid, c0.y - W * 0.18f, bwid, W * 0.36f}, 1.0f, side);
    }
} // namespace gt4hudlook
