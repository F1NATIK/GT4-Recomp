// GT4 Recompilation launcher, Full edition: shared page pieces (gt4full_ui.h).

#include "gt4full_ui.h"

#include <algorithm>
#include <cmath>

namespace gt4stock::fullui
{
    using ui::Align;
    using ui::Weight;

    namespace
    {
        const char *pillText(Pill p)
        {
            switch (p)
            {
            case Pill::ClassicToo: return "Classic too";
            case Pill::Full: return "Full";
            case Pill::Experimental: return "Experimental";
            default: return "Not built yet";
            }
        }
        Color pillColor(Pill p)
        {
            const ui::Theme &t = ui::theme();
            switch (p)
            {
            case Pill::ClassicToo: return t.muted;
            case Pill::Full: return t.accent;
            case Pill::Experimental: return t.warn;
            default: return t.faint;
            }
        }
    }

    float pillWidth(ui::Ui &u, Pill p) { return u.textWidth(pillText(p), 11.0f, Weight::Semibold) + 16.0f; }

    float pill(ui::Ui &u, float x, float y, Pill p)
    {
        const Color c = pillColor(p);
        const float w = pillWidth(u, p), h = 11.0f * 1.33f + 6.0f;
        u.round(Rectangle{x, y, w, h}, h * 0.5f, ui::alpha(c, 0.14f));
        u.roundLine(Rectangle{x, y, w, h}, h * 0.5f, 1.0f, ui::alpha(c, 0.55f));
        u.text(pillText(p), x + 8.0f, y + 3.0f, 11.0f, c, Weight::Semibold);
        return w;
    }

    bool tabs(ui::Ui &u, const std::string &id, float x, float y, float w, const std::vector<std::string> &labels, int &sel, float size)
    {
        const ui::Theme &t = ui::theme();
        const float h = size * 1.33f + 16.0f;
        u.fill(Rectangle{x, y + h - 1.0f, w, 1.0f}, t.border);
        float cx = x;
        bool changed = false;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            const float tw = u.textWidth(labels[i], size, Weight::Semibold) + 28.0f;
            const Rectangle r{cx, y, tw, h};
            const bool on = static_cast<int>(i) == sel;
            const bool hot = u.hovered(r);
            u.text(labels[i], cx + 14.0f, y + 7.0f, size, on ? t.text : (hot ? ui::mixColor(t.muted, t.text, 0.5f) : t.muted), on ? Weight::Semibold : Weight::Regular);
            if (on)
                u.fill(Rectangle{cx + 6.0f, y + h - 2.5f, tw - 12.0f, 2.5f}, t.accent);
            if (!on && u.clicked(r))
            {
                sel = static_cast<int>(i);
                changed = true;
            }
            cx += tw + 4.0f;
        }
        (void)id;
        return changed;
    }

    void card(ui::Ui &u, Rectangle r, const std::string &title, const std::vector<Pill> &pills)
    {
        u.card(r, title);
        float px = r.x + r.width - 16.0f;
        for (Pill p : pills)
        {
            // One launcher, one GT4 (developer 2026-10-20: "stop labling thing FULL, and Classic"): the edition pills are not drawn.
            if (p == Pill::Full || p == Pill::ClassicToo)
                continue;
            px -= pillWidth(u, p);
            pill(u, px, r.y + 17.0f, p);
            px -= 6.0f;
        }
    }

    bool radio(ui::Ui &u, const std::string &id, Rectangle r, bool selected, const std::string &label, const std::string &hint, bool enabled, bool notBuilt)
    {
        const ui::Theme &t = ui::theme();
        const bool clicked = u.rowButton(id, r, selected && enabled, enabled && !selected);
        const float cy = r.y + r.height * 0.5f;
        const Color ring = !enabled ? t.panel3 : selected ? t.accent : t.faint;
        u.ring(r.x + 16.0f, cy, 6.0f, 7.6f, ring);
        if (selected)
            u.circle(r.x + 16.0f, cy, 3.6f, enabled ? t.accent : t.panel3);
        const float size = 13.5f;
        const float lw = u.text(label, r.x + 32.0f, cy - size * 0.68f, size, enabled ? t.text : t.faint, selected && enabled ? Weight::Semibold : Weight::Regular);
        float hx = r.x + 32.0f + lw + 10.0f;
        float right = r.x + r.width - 10.0f;
        if (notBuilt)
        {
            right -= pillWidth(u, Pill::NotBuilt);
            pill(u, right, cy - 9.5f, Pill::NotBuilt);
            right -= 8.0f;
        }
        if (!hint.empty() && right - hx > 40.0f)
            u.text(u.fit(hint, right - hx, 12.0f), hx, cy - 12.0f * 0.68f, 12.0f, enabled ? t.muted : ui::alpha(t.faint, 0.8f));
        return clicked;
    }

    void caps(ui::Ui &u, float x, float y, const std::string &text, Color c)
    {
        // Letter-spaced small capitals: each character (a whole UTF-8 sequence) on its own.
        float cx = x;
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            const size_t n = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
            std::string s = text.substr(i, n);
            if (n == 1 && s[0] >= 'a' && s[0] <= 'z')
                s[0] = static_cast<char>(s[0] - 32);
            u.text(s, cx, y, 11.0f, c, Weight::Semibold);
            cx += u.textWidth(s, 11.0f, Weight::Semibold) + 1.6f;
            i += n;
        }
    }

    float notBuilt(ui::Ui &u, float x, float y, float w, const std::string &what)
    {
        const ui::Theme &t = ui::theme();
        const std::string text = what.empty() ? std::string("Not built yet: GT4 does not have this yet.") : "Not built yet: " + what;
        return u.wrap(text, x, y, w, 12.0f, ui::alpha(t.faint, 0.95f));
    }

    bool chipButton(ui::Ui &u, float x, float y, const std::string &label, bool selected, bool enabled, float &width, float size)
    {
        const ui::Theme &t = ui::theme();
        width = u.textWidth(label, size, Weight::Semibold) + 28.0f;
        const float h = size * 1.33f + 16.0f;
        const Rectangle r{x, y, width, h};
        const bool hot = enabled && u.hovered(r);
        const Color bg = selected ? ui::mixColor(t.panel2, t.text, 0.85f) : hot ? t.panel3 : t.panel2;
        u.round(r, 7.0f, bg);
        if (!selected)
            u.roundLine(r, 7.0f, 1.0f, t.border);
        u.text(label, x + 14.0f, y + 8.0f, size, selected ? t.bg : (enabled ? t.text : t.faint), Weight::Semibold);
        return enabled && !selected && u.clicked(r);
    }

    void specRow(ui::Ui &u, float x, float y, float w, const std::string &label, const std::string &value, Color valueColor, float size)
    {
        const ui::Theme &t = ui::theme();
        u.text(label, x, y, size, t.muted);
        const float lw = u.textWidth(label, size) + 16.0f;
        u.text(u.fit(value, w - lw, size, Weight::Semibold), x + w, y, size, valueColor, Weight::Semibold, Align::Right);
        u.fill(Rectangle{x, y + size * 1.33f + 7.0f, w, 1.0f}, ui::alpha(t.border, 0.7f));
    }

    void conditionBar(ui::Ui &u, Rectangle r, float v, float warnAt, float badAt)
    {
        const ui::Theme &t = ui::theme();
        const Color c = v >= badAt ? t.bad : v >= warnAt ? t.warn : t.ok;
        u.meter(r, v, c);
    }

    void statTile(ui::Ui &u, Rectangle r, const std::string &value, const std::string &label, Color valueColor)
    {
        const ui::Theme &t = ui::theme();
        u.round(r, 10.0f, t.panel2);
        u.text(u.fit(value, r.width - 24.0f, 22.0f, Weight::Bold), r.x + 14.0f, r.y + 12.0f, 22.0f, valueColor, Weight::Bold);
        u.text(u.fit(label, r.width - 24.0f, 12.5f), r.x + 14.0f, r.y + 12.0f + 22.0f * 1.33f + 4.0f, 12.5f, t.muted);
    }
}
