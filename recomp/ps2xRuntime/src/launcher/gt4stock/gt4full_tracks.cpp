// GT4 Recompilation launcher, Full edition: Your game > Tracks (developer, 27 Oct 2026: "could we have a section in the launcher where
// we choose which tracks appear in game?"; analysis/design/tracks). Arcade's four Single Race lists in GT4's order
// (gt4full_tracks_data.h), a tick per course: an unticked course is left out of Arcade's course lists (GT4_TRACKS_HIDE, which
// GT4.exe's gt4_tracks.cpp reads; the save's unlocked courses are not touched, GT Mode is not affected). Each list keeps at
// least one course (an empty list is a screen GT4 never shows). Valencia (from Tourist Trophy) is there when the extras pack
// beside GT4.exe (extra-cars\esso.gt4x, analysis/design/tracks/scripts/build_tracks_pack.py) carries it.

#include "gt4stock_app.h"

#include "gt4full_tracks_data.h"
#include "gt4full_ui.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace gt4stock
{
    using ui::Align;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kCategories[] = {"World Circuits", "Original Circuits", "City Courses", "Dirt & Snow"};

        // The pack's file list (version 2: its records after the region list; build_tracks_pack.py) names the added course's file.
        bool packHas(const fs::path &pack, const std::string &path)
        {
            std::ifstream in(pack, std::ios::binary);
            uint8_t h[64] = {};
            if (!in || !in.read(reinterpret_cast<char *>(h), sizeof(h)) || std::memcmp(h, "GT4XCAR1", 8) != 0)
                return false;
            auto le32 = [](const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); };
            if (le32(h + 8) != 2)
                return false;
            const uint32_t regions = le32(h + 40), recBytes = le32(h + 56);
            if (regions > 16 || recBytes > (8u << 20))
                return false;
            std::string rec(recBytes, '\0');
            in.seekg(64 + 40 * std::streamoff(regions));
            return in.read(rec.data(), std::streamsize(rec.size())) && rec.find(path) != std::string::npos;
        }
    }

    bool App::tracksAdded()
    {
        static fs::path checked;
        static bool has = false;
        const fs::path pack = inst_.exe.parent_path() / L"extra-cars" / L"esso.gt4x";
        if (checked != pack)
        {
            checked = pack;
            has = packHas(pack, "crs/tt_valencia");
        }
        return has;
    }

    // ------------------------------------------------------------------ Tracks
    void App::pageTracks()
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        drawHeader("Tracks", "Choose which courses Arcade mode lists. A course you untick is left out of Arcade's course lists; it stays "
                             "unlocked in your save, and GT Mode is not changed.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        const bool added = tracksAdded();
        auto hidden = [&](uint32_t code) { return std::binary_search(f.tracksHidden.begin(), f.tracksHidden.end(), code); };
        auto setHidden = [&](uint32_t code, bool hide) {
            auto it = std::lower_bound(f.tracksHidden.begin(), f.tracksHidden.end(), code);
            if (hide && (it == f.tracksHidden.end() || *it != code))
                f.tracksHidden.insert(it, code);
            else if (!hide && it != f.tracksHidden.end() && *it == code)
                f.tracksHidden.erase(it);
            markDirty();
        };
        auto offered = [&](const tracks::Course &c) { return !c.added || added; };

        // the summary line
        int shown = 0, total = 0;
        for (const tracks::Course &c : tracks::kCourses)
            if (offered(c))
                total += 1, shown += hidden(c.code) ? 0 : 1;
        ui_.text(strf("%d of %d courses in Arcade", shown, total), x0, kTop - 6, 15, t.text, Weight::Semibold);
        // (developer 2026-10-29: "can you unlock all the tracks for me in arcade mode?"; GT4_TRACKS_UNLOCK, the save not changed)
        if (ui_.checkbox("tracks-unlock", x0 + ui_.textWidth(strf("%d of %d courses in Arcade", shown, total), 15) + 40, kTop - 8,
                         f.tracksUnlockAll, "Unlock every course", !busy()))
            markDirty();
        if (ui_.button("tracks-all", R(x1 - 150, kTop - 12, 150, 30), "Show every course", Style::Secondary, !f.tracksHidden.empty(), 13))
        {
            f.tracksHidden.clear();
            markDirty();
        }

        const Rectangle view = R(x0, kTop + 30, cw, bottom - kTop - 30);
        const float off = ui_.beginScroll("tracks", view);
        const float colW = std::floor((cw - kGap) / 2.0f);
        const float rowH = 30.0f;
        float y0 = view.y - off, contentEnd = view.y - off;
        float rowTop = y0, rowBottom = y0;
        for (int k = 0; k < 4; ++k)
        {
            std::vector<const tracks::Course *> list;
            for (const tracks::Course &c : tracks::kCourses)
                if (std::strcmp(c.category, kCategories[k]) == 0)
                    list.push_back(&c);
            const int rows = static_cast<int>((list.size() + 1) / 2);
            const float h = 56 + rows * rowH + 14;
            if (k % 2 == 0)
                rowTop = rowBottom + (k ? kGap : 0.0f), rowBottom = rowTop;
            const Rectangle c = R(x0 + (k % 2) * (colW + kGap), rowTop, colW, h);
            rowBottom = std::max(rowBottom, c.y + c.height);
            fullui::card(ui_, c, kCategories[k]);
            int inList = 0, showing = 0;
            for (const tracks::Course *p : list)
                if (offered(*p))
                    inList += 1, showing += hidden(p->code) ? 0 : 1;
            ui_.text(strf("%d of %d", showing, inList), c.x + c.width - 18, c.y + 16, 13, t.muted, Weight::Regular, Align::Right);
            const float innerW = (c.width - 36) / 2.0f;
            for (size_t i = 0; i < list.size(); ++i)
            {
                const tracks::Course &course = *list[i];
                const float cx = c.x + 18 + (i % 2) * innerW, cy = c.y + 50 + static_cast<float>(i / 2) * rowH;
                bool on = offered(course) && !hidden(course.code);
                // (each list keeps one course: the last ticked one can't be unticked)
                const bool lastOne = on && showing <= 1;
                const bool enabled = offered(course) && !lastOne && !busy();
                const std::string label = ui_.fit(course.shortName, innerW - (course.added ? 92 : 40), 14);
                if (ui_.checkbox(strf("trk-%x", course.code), cx, cy, on, label, enabled))
                    setHidden(course.code, !on);
                if (course.added)
                {
                    const float lw = ui_.textWidth(label, 14) + 34;
                    ui_.chip(cx + lw, cy + 1, added ? "NEW" : "NEEDS PACK", added ? ui::alpha(t.accent, 0.22f) : ui::alpha(t.warn, 0.2f),
                             added ? t.accent : t.warn, 11);
                }
            }
            contentEnd = std::max(contentEnd, rowBottom);
        }
        // notes under the cards
        float y = contentEnd + 14;
        y += ui_.wrap("Valencia (Circuit Ricardo Tormo) comes from your Tourist Trophy disc and is in World Circuits only. " +
                          std::string(added ? "It is served from extra-cars\\esso.gt4x beside GT4.exe."
                                            : "It needs the extras pack (extra-cars\\esso.gt4x beside GT4.exe), built from your own GT4 and "
                                              "Tourist Trophy discs."),
                      x0 + 4, y, cw - 8, 12.5f, t.muted) +
             6;
        y += ui_.wrap(std::string(f.tracksUnlockAll ? "Unlock every course is on: Arcade offers every ticked course, whether or not your save has "
                                                      "it unlocked (your save isn't changed)."
                                                    : "A course shows only when your save has it unlocked in Arcade, as before.") +
                          " Each list keeps at least one course.",
                      x0 + 4, y, cw - 8, 12.5f, t.faint) +
             8;
        ui_.endScroll("tracks", y + off - view.y);
    }
}
