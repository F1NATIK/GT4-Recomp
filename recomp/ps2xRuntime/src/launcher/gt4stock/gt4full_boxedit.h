#pragma once

// GT4 Recompilation launcher, Full edition: boxes on a stage dragged by the mouse and resized by a corner handle, each in
// width and height (HUD hand-off 8 T3, the Telemetry screen's layout editor; small and on its own so Driving > Race HUD can
// take it up too). A box is x y w h in % of the stage, the list back to front (the last drawn is picked first).
//   step(): one frame of the mouse and the keys (arrows move the picked box, Shift + arrows resize it); plain C++, no raylib
//   calls, so the host test can drive it. keysNow(): the arrow keys this frame from raylib (in the page's translation unit).

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gt4stock::boxedit
{
    struct Box
    {
        float x = 0, y = 0, w = 10, h = 10; // % of the stage
    };

    struct Options
    {
        float snap = 2.5f;    // % steps for drags, resizes and keys (0: free, keys 0.5 %)
        float minW = 3.0f;    // the smallest box, %
        float minH = 3.0f;
        float handleDp = 14;  // the corner handle's size on the screen
    };

    struct Keys
    {
        int dx = 0, dy = 0;   // -1 / 0 / 1 this frame (an arrow pressed or repeating)
        bool shift = false;   // resize instead of move
    };

    struct Mouse
    {
        Vector2 at{-1e6f, -1e6f}; // dp
        bool pressed = false, down = false;
        bool enabled = true;      // input on (no popup open, no modal)
    };

    struct State
    {
        int sel = -1;   // the picked box (index into the list), -1 none
        int drag = -1;  // the box being dragged or resized
        bool resizing = false;
        Vector2 grab{0, 0}; // the mouse when the drag began (dp)
        Box start;          // the box then
    };

    inline Rectangle onStage(const Box &b, Rectangle stage)
    {
        return Rectangle{stage.x + b.x * stage.width / 100.0f, stage.y + b.y * stage.height / 100.0f, b.w * stage.width / 100.0f,
                         b.h * stage.height / 100.0f};
    }
    inline Rectangle handle(const Box &b, Rectangle stage, const Options &o)
    {
        const Rectangle r = onStage(b, stage);
        return Rectangle{r.x + r.width - o.handleDp, r.y + r.height - o.handleDp, o.handleDp, o.handleDp};
    }
    inline float snapTo(float v, float step) { return step > 0 ? std::round(v / step) * step : std::round(v * 10.0f) / 10.0f; }
    inline bool inside(Vector2 p, Rectangle r) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height; }

    // Keeps the box on the stage and at least the smallest size.
    inline void clampBox(Box &b, const Options &o)
    {
        b.w = std::clamp(b.w, o.minW, 100.0f);
        b.h = std::clamp(b.h, o.minH, 100.0f);
        b.x = std::clamp(b.x, 0.0f, 100.0f - b.w);
        b.y = std::clamp(b.y, 0.0f, 100.0f - b.h);
    }

    // One frame. Returns true when a box moved or changed size (the caller saves). A press on a box picks it (on its corner
    // handle: resize); a press on the stage's empty part drops the pick.
    inline bool step(State &st, std::vector<Box> &boxes, Rectangle stage, const Mouse &m, const Keys &k, const Options &o)
    {
        const int n = static_cast<int>(boxes.size());
        if (st.sel >= n)
            st.sel = -1;
        if (st.drag >= n)
            st.drag = -1;
        bool changed = false;
        if (m.enabled && m.pressed && inside(m.at, stage))
        {
            st.drag = -1;
            for (int i = n - 1; i >= 0 && st.drag < 0; --i)
            {
                const bool onHandle = inside(m.at, handle(boxes[static_cast<size_t>(i)], stage, o));
                if (onHandle || inside(m.at, onStage(boxes[static_cast<size_t>(i)], stage)))
                {
                    st.sel = st.drag = i;
                    st.resizing = onHandle;
                    st.grab = m.at;
                    st.start = boxes[static_cast<size_t>(i)];
                }
            }
            if (st.drag < 0)
                st.sel = -1;
        }
        if (!m.down)
            st.drag = -1;
        if (st.drag >= 0 && m.enabled && stage.width > 0 && stage.height > 0)
        {
            Box b = st.start;
            const float dx = (m.at.x - st.grab.x) * 100.0f / stage.width, dy = (m.at.y - st.grab.y) * 100.0f / stage.height;
            if (st.resizing)
            {
                b.w = snapTo(st.start.x + st.start.w + dx, o.snap) - b.x;
                b.h = snapTo(st.start.y + st.start.h + dy, o.snap) - b.y;
                b.w = std::min(b.w, 100.0f - b.x);
                b.h = std::min(b.h, 100.0f - b.y);
            }
            else
            {
                b.x = snapTo(st.start.x + dx, o.snap);
                b.y = snapTo(st.start.y + dy, o.snap);
            }
            clampBox(b, o);
            Box &cur = boxes[static_cast<size_t>(st.drag)];
            if (std::fabs(cur.x - b.x) > 1e-3f || std::fabs(cur.y - b.y) > 1e-3f || std::fabs(cur.w - b.w) > 1e-3f || std::fabs(cur.h - b.h) > 1e-3f)
            {
                cur = b;
                changed = true;
            }
        }
        if (st.sel >= 0 && st.drag < 0 && (k.dx || k.dy))
        {
            Box &b = boxes[static_cast<size_t>(st.sel)];
            const float s = o.snap > 0 ? o.snap : 0.5f;
            if (k.shift)
            {
                b.w = std::min(snapTo(b.w + k.dx * s, o.snap), 100.0f - b.x);
                b.h = std::min(snapTo(b.h + k.dy * s, o.snap), 100.0f - b.y);
            }
            else
            {
                b.x = snapTo(b.x + k.dx * s, o.snap);
                b.y = snapTo(b.y + k.dy * s, o.snap);
            }
            clampBox(b, o);
            changed = true;
        }
        return changed;
    }

    // The arrow keys this frame (raylib; the page's translation unit).
    inline Keys keysNow()
    {
        Keys k;
        auto hit = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
        k.dx = hit(KEY_RIGHT) ? 1 : hit(KEY_LEFT) ? -1 : 0;
        k.dy = hit(KEY_DOWN) ? 1 : hit(KEY_UP) ? -1 : 0;
        k.shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        return k;
    }
} // namespace gt4stock::boxedit
