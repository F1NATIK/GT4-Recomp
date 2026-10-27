// GT4 Recompilation launcher (Stock edition): the Saves page's read-only "Save details" view and
// the About page's "How it works" page (new in 1.2; 1.3: the full text, with its tables, code,
// quote and the PS2 diagram drawn as boxes).

#include "gt4stock_app.h"

#include "gt4save.h"
#include "gt4stock_resources.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;

        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        const char *const kDot = " \xc2\xb7 "; // a middle dot between spaces

        Color medal(char m)
        {
            switch (m)
            {
            case 'G': return Color{222, 182, 62, 255};
            case 'S': return Color{178, 188, 200, 255};
            case 'B': return Color{190, 124, 70, 255};
            case 'P': return Color{63, 185, 80, 255};
            default: return Color{40, 48, 62, 255};
            }
        }

        std::string km(uint64_t metres)
        {
            if (metres < 100000)
                return strf("%.1f km", metres / 1000.0);
            return groupedNumber((metres + 500) / 1000) + " km";
        }
    }

    // ------------------------------------------------------------------ loading
    std::string App::namesKey() const
    {
#if defined(GT4L_EDITION_FULL)
        return pathText(pathFrom(st_.disc)) + "|" + extraCarsKey_;
#else
        return pathText(pathFrom(st_.disc));
#endif
    }

    void App::startNames()
    {
        if (!disc_.exists)
        {
            namesError_ = "no disc image";
            return;
        }
        const std::string iso = namesKey();
        if (namesLoading_ || (names_ && iso == namesIso_))
            return;
        namesIso_ = iso;
        namesLoading_ = true;
        namesError_.clear();
        const fs::path p = pathFrom(st_.disc);
        namesFuture_ = std::async(std::launch::async, [p]() {
            NamesLoad r;
            const auto t0 = std::chrono::steady_clock::now();
            r.names = GameNames::load(p, r.error);
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return r;
        });
    }

    void App::pollNames(bool wait)
    {
        if (!namesLoading_)
            return;
        if (!wait && namesFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        NamesLoad r = namesFuture_.get();
        namesLoading_ = false;
        namesMs_ = r.ms;
        namesError_ = r.error;
        if (r.names)
        {
            names_ = r.names;
#if defined(GT4L_EDITION_FULL)
            cat_.reset(); // the car data again, with these names (another disc, or the extra cars switched)
            catError_.clear();
#endif
        }
        if (detailsOpen_ && !detailsNames_ && names_)
            reloadDetails();
        if (disc_.exists && namesKey() != namesIso_)
            startNames(); // the disc (or the extra cars) changed while reading
    }

    void App::openDetails(int slot)
    {
        const bool file = cardIsFile(slot);
        const fs::path source = file ? cardFile(slot) : cardDir(slot);
        std::string where = shortPath(source, file ? inst_.gameDir : inst_.root);
        if (where == pathText(source))
            where = shortPath(source, inst_.root); // a card file outside game\ (as GT4.exe takes it: any path)
        openDetailsPath(source, strf("Memory card slot %d", slot) + kDot + (file ? "card file " : "folder card ") + where);
        detailsSlot_ = slot;
    }

    void App::openDetailsPath(const fs::path &source, const std::string &title)
    {
        detailsSlot_ = 0;
        detailsSource_ = source;
        detailsTitle_ = title;
        expandedCar_ = -1;
        showTechnical_ = false;
        ui_.resetScroll("details-left");
        ui_.resetScroll("details-garage");
        savesView_ = 1;
        detailsOpen_ = true;
        if (!names_)
            startNames();
        reloadDetails();
    }

    void App::reloadDetails()
    {
        const auto t0 = std::chrono::steady_clock::now();
        const gt4save::SpecDb *spec = names_ ? names_->spec.get() : nullptr;
        std::error_code ec;
        gt4save::Save s;
        if (fs::is_regular_file(detailsSource_, ec))
            s = gt4save::loadCardFile(detailsSource_, spec);
        else if (fs::is_regular_file(detailsSource_ / gt4save::kGamedataFile, ec))
            s = gt4save::loadSaveFolder(detailsSource_, spec); // a save folder itself (a card folder holds it as a folder)
        else
            s = gt4save::loadCardFolder(detailsSource_, spec);
        details_ = buildDetails(s, names_.get());
        detailsNames_ = names_ != nullptr; // read again with names when they arrive (pollNames), and when GT4 closes (pollGame)
        detailsMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // ------------------------------------------------------------------ Save details
    void App::pageSaveDetails()
    {
        const ui::Theme &t = ui::theme();
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        const SaveDetails &d = details_;

        // Header with its own buttons: back to the cards, read again.
        ui_.text("Save details", x0, 28, 28, t.text, Weight::Bold);
        const float bw = 150, rw = 116;
        if (ui_.button("details-back", R(x1 - bw, 30, bw, 36), "\xe2\x86\x90  Back to Saves", Style::Secondary, true, 13.5f))
        {
            savesView_ = 0;
            detailsOpen_ = false;
            refreshCards();
            return;
        }
        if (ui_.iconButton("details-refresh", R(x1 - bw - 10 - rw, 30, rw, 36), Icon::Refresh, "Refresh", Style::Secondary, true, 13.5f))
        {
            reloadDetails();
            setStatus(strf("Read the save again (%.0f ms).", detailsMs_), t.muted);
        }
        ui_.text(ui_.fit(detailsTitle_ + kDot + "read only", x1 - x0 - bw - rw - 30, 14.5f, Weight::Regular, true), x0, 66, 14.5f, t.muted);

        // The read-only note, and where the names come from.
        const Rectangle note = R(x0, kTop, cw, 40);
        ui_.round(note, 10, ui::alpha(t.accent, 0.10f));
        ui_.roundLine(note, 10, 1, ui::alpha(t.accent, 0.35f));
        ui_.icon(Icon::Info, R(note.x + 14, note.y + 10, 20, 20), t.accent);
        float chipX = note.x + note.width - 12;
        auto rightChip = [&](const std::string &s, Color bg, Color fg) {
            const float w = ui_.textWidth(s, 12, Weight::Semibold) + 18;
            chipX -= w;
            ui_.chip(chipX, note.y + 9, s, bg, fg, 12);
            chipX -= 8;
        };
        if (namesLoading_)
            rightChip("Reading names from the disc...", ui::alpha(t.accent, 0.18f), t.accent);
        else if (names_)
            rightChip("Names from your GT4 disc", ui::alpha(t.ok, 0.16f), t.ok);
        else
            rightChip(disc_.exists ? "Names could not be read from the disc" : "No disc image: names shown as codes", ui::alpha(t.warn, 0.16f), t.warn);
        if (busy())
            rightChip("GT4 is running: read again when it closes", ui::alpha(t.warn, 0.16f), t.warn);
#if defined(GT4L_EDITION_FULL)
        const char *readOnly = "Read only, except Change name and Repair (each asks first and backs up the card).";
#else
        const char *readOnly = "Read only: this view never changes your save.";
#endif
        ui_.text(ui_.fit(readOnly, chipX - note.x - 50, 13.5f, Weight::Semibold),
                 note.x + 44, note.y + 11, 13.5f, t.text, Weight::Semibold);

        const float top = kTop + 40 + 12;
        const float lw = std::floor(cw * 0.41f);
        const float rx = x0 + lw + kGap, rwid = x1 - rx;

        if (!d.loaded)
        {
            const Rectangle c = R(x0, top, cw, std::min(260.0f, bottom - top));
            ui_.card(c, "This save could not be shown");
            float y = c.y + 54;
            for (const DetailProblem &p : d.problems)
            {
                ui_.icon(Icon::Warn, R(c.x + 20, y, 22, 22), t.bad);
                ui_.text(p.title, c.x + 52, y + 2, 15, t.text, Weight::Semibold);
                y += 28;
                y += ui_.wrap(p.text, c.x + 52, y, c.width - 80, 13.5f, t.muted) + 10;
            }
            return;
        }

        // ---------------- left: player, events, licences (scrolls when it does not fit)
        {
            const Rectangle view = R(x0, top, lw, bottom - top);
            const float off = ui_.beginScroll("details-left", view);
            float y = top - off;
            const float x = x0, w = lw - 10; // room for the scroll bar

            // Player
            {
                const float y0 = y;
                std::vector<std::pair<std::string, std::string>> stats = {
                    {"Credits", d.hasCredits ? groupedNumber(d.credits) + " Cr." : std::string("-")},
                    {"A-spec points", groupedNumber(d.aspecPoints)},
                    {"Races won", strf("%u of %s", d.wins, groupedNumber(d.races).c_str())},
                    {"Prize money", groupedNumber(d.prizeMoney) + " Cr."},
                    {"Prize cars", groupedNumber(d.prizeCars)},
                    {"Distance driven", km(d.distanceM)}};
                float h = 50 + 30 + 2 * 50 + 6;
                for (const DetailProblem &p : d.problems)
                    h += 36 + ui_.wrapHeight(p.text, w - 72, 12.5f) + 8;
                if (!d.technical.empty())
                    h += 30 + (showTechnical_ ? [&] {
                        float th = 0;
                        for (const std::string &s : d.technical)
                            th += ui_.wrapHeight(s, w - 40, 12) + 2;
                        return th + 4;
                    }()
                                              : 0.0f);
                const Rectangle c = R(x, y0, w, h);
                ui_.round(c, 12, t.panel);
                ui_.roundLine(c, 12, 1, ui::alpha(t.border, 0.8f));
                ui_.text(ui_.fit(d.player.empty() ? std::string("(no name)") : d.player, w - 40, 24, Weight::Bold), c.x + 20, c.y + 14, 24, t.text,
                         Weight::Bold);
                ui_.text(strf("Day %lld", static_cast<long long>(d.gameDay)) + (d.date.empty() ? std::string() : kDot + d.date), c.x + 20, c.y + 48,
                         13.5f, t.muted);
                // GT4's verdict on loading it
                {
                    const Color sc = d.loadState == 0 ? t.ok : d.loadState == 1 ? t.warn : t.bad;
                    const std::string s = loadStateText(d.loadState);
                    const float cwid = ui_.textWidth(s, 12, Weight::Semibold) + 18;
                    ui_.chip(c.x + c.width - 20 - cwid, c.y + 18, s, ui::alpha(sc, 0.16f), sc, 12);
                }
                float sy = c.y + 80;
                const float colW = (w - 40) / 3.0f;
                for (size_t i = 0; i < stats.size(); ++i)
                {
                    const float sx = c.x + 20 + (i % 3) * colW, yy = sy + (i / 3) * 50;
                    ui_.text(stats[i].first, sx, yy, 12, t.faint);
                    ui_.text(ui_.fit(stats[i].second, colW - 10, 17, Weight::Semibold), sx, yy + 17, 17, t.text, Weight::Semibold);
                }
#if defined(GT4L_EDITION_FULL)
                // Change name (developer 2026-10-09): the player's name, through the Garage's confirmation and checks.
                if (ui_.button("details-rename", R(c.x + c.width - 20 - 130, c.y + 44, 130, 26), "Change name", Style::Secondary, !busy(), 12.5f))
                {
                    nameEdit_ = d.player;
                    modal_ = Modal::RenameSave;
                }
                bool repairShown = false;
#endif
                float py = sy + 100 + 6;
                for (const DetailProblem &p : d.problems)
                {
                    const Color pc = p.level >= 2 ? t.bad : t.warn;
                    const float ph = 30 + ui_.wrapHeight(p.text, w - 72, 12.5f) + 6;
                    const Rectangle pr = R(c.x + 12, py, w - 24, ph);
                    ui_.round(pr, 8, ui::alpha(pc, 0.10f));
                    ui_.roundLine(pr, 8, 1, ui::alpha(pc, 0.45f));
                    ui_.icon(Icon::Warn, R(pr.x + 10, pr.y + 8, 18, 18), pc);
                    float titleRoom = pr.width - 50;
#if defined(GT4L_EDITION_FULL)
                    // Repair (developer 2026-10-09): where the old memory-card bug's extra data is shown (gt4save.py repair's rules).
                    if (d.appendBug && !repairShown && (p.title == "Extra data at the end of the garage file" || endsWith(p.title, "saves in one file")))
                    {
                        repairShown = true;
                        titleRoom -= 96;
                        if (ui_.button("details-repair", R(pr.x + pr.width - 8 - 88, pr.y + 5, 88, 24), "Repair", Style::Primary, !busy(), 12.5f))
                        {
                            const EditTarget target = detailsEditTarget();
                            openGarageEdit(repairEdit(), &target);
                        }
                    }
#endif
                    ui_.text(ui_.fit(p.title, titleRoom, 13.5f, Weight::Semibold), pr.x + 36, pr.y + 8, 13.5f, t.text, Weight::Semibold);
                    ui_.wrap(p.text, pr.x + 36, pr.y + 30, pr.width - 48, 12.5f, t.muted);
                    py += ph + 8;
                }
                if (!d.technical.empty())
                {
                    const std::string label = (showTechnical_ ? "Hide" : "Show") + strf(" technical notes (%zu)", d.technical.size());
                    const float lwid = ui_.textWidth(label, 12.5f, Weight::Semibold) + 24;
                    if (ui_.button("details-tech", R(c.x + 12, py, lwid, 26), label, Style::Ghost, true, 12.5f))
                        showTechnical_ = !showTechnical_;
                    py += 30;
                    if (showTechnical_)
                        for (const std::string &s : d.technical)
                            py += ui_.wrap(s, c.x + 20, py, w - 40, 12, t.faint) + 2;
                }
                y = y0 + h + kGap;
            }

            // Events (championship progress), prize cars
            {
                float h = 50 + (d.series.empty() ? 30 : d.series.size() * 46.0f) + 8 + 24 + 10;
                if (!d.championship.empty())
                    h += 24;
                const Rectangle c = R(x, y, w, h);
                ui_.card(c, "Events", d.eventsDone ? strf("%d won of %d run", d.eventsWon, d.eventsDone) : std::string());
                float ey = c.y + 50;
                if (d.series.empty())
                {
                    ui_.text(details_.names ? "No events run yet." : "Event names need the disc.", c.x + 20, ey + 4, 13, t.muted);
                    ey += 30;
                }
                for (const DetailSeries &s : d.series)
                {
                    const float barW = 70;
                    const std::string right = strf("%d / %d", s.done, s.races);
                    ui_.text(ui_.fit(s.name, w - 40 - barW - 70, 13.5f, Weight::Semibold), c.x + 20, ey, 13.5f, t.text, Weight::Semibold);
                    ui_.text(ui_.fit((s.hall.empty() ? std::string() : s.hall + kDot) + strf("%d won%s%d pts", s.won, kDot, s.points), w - 40 - barW - 70,
                                     12),
                             c.x + 20, ey + 20, 12, t.faint);
                    const Rectangle bar = R(c.x + c.width - 20 - barW, ey + 8, barW, 6);
                    ui_.meter(bar, s.races ? static_cast<float>(s.done) / s.races : 0.0f, s.done >= s.races ? t.ok : t.accent);
                    ui_.text(right, bar.x - 10, ey + 2, 12.5f, t.muted, Weight::Regular, Align::Right);
                    ey += 46;
                }
                ey += 8;
                if (!d.championship.empty())
                {
                    ui_.text(ui_.fit("Championship in progress: " + d.championship, w - 40, 12.5f), c.x + 20, ey, 12.5f, t.warn);
                    ey += 24;
                }
                ui_.text(ui_.fit(d.prizeList.empty() ? std::string("Prize cars: none yet") : "Prize cars: " + join(d.prizeList, ", "), w - 40, 12.5f),
                         c.x + 20, ey, 12.5f, t.muted);
                ey += 24;
                y += h + kGap;
            }
            // Licences
            {
                int passed = 0;
                for (const DetailLicence &l : d.licences)
                    passed += l.passed;
                const float h = d.licences.empty() ? 50 + 36 : 50 + 5 * 30 + 44;
                const Rectangle c = R(x, y, w, h);
                ui_.card(c, "Licences", d.licences.empty() ? std::string() : strf("%d of 5 passed", passed));
                if (d.licences.empty())
                    ui_.text("Licence results need the disc's race list.", c.x + 20, c.y + 52, 13, t.muted);
                float ly = c.y + 50;
                const float chipW = 34, sq = std::floor(std::min(11.0f, (w - 40 - chipW - 12 - 84 - 15 * 3) / 16.0f));
                for (const DetailLicence &l : d.licences)
                {
                    ui_.round(R(c.x + 20, ly + 2, chipW, 22), 6, l.passed ? ui::alpha(t.ok, 0.18f) : t.panel3);
                    ui_.text(l.key, c.x + 20 + chipW * 0.5f, ly + 5, 12, l.passed ? t.ok : t.text, Weight::Semibold, Align::Center);
                    float qx = c.x + 20 + chipW + 12;
                    for (size_t i = 0; i < 16; ++i)
                    {
                        ui_.round(R(qx, ly + 13 - sq * 0.5f, sq, sq), 2.5f, medal(l.marks[i]));
                        qx += sq + 3;
                    }
                    const std::string st = l.allGold ? "All gold" : l.passed ? "Passed" : l.done ? strf("%d of 16", l.done) : "Not yet";
                    ui_.text(st, c.x + c.width - 20, ly + 5, 12.5f, l.allGold ? medal('G') : l.passed ? t.ok : l.done ? t.muted : t.faint,
                             Weight::Regular, Align::Right);
                    ly += 30;
                }
                // legend and the other tests
                if (!d.licences.empty())
                {
                    float lx = c.x + 20;
                    const float ly2 = c.y + c.height - 30;
                    for (const auto &[m, label] : std::vector<std::pair<char, const char *>>{{'G', "gold"}, {'S', "silver"}, {'B', "bronze"}})
                    {
                        ui_.round(R(lx, ly2 + 4, 9, 9), 2, medal(m));
                        lx += 14 + ui_.text(label, lx + 14, ly2, 12, t.faint) + 10;
                    }
                    ui_.text(ui_.fit(strf("Coffee breaks %d of %d%sMissions %d of %d", d.coffeeDone, d.coffeeTotal, kDot, d.missionsDone, d.missionsTotal),
                                     c.x + c.width - 20 - lx, 12),
                             c.x + c.width - 20, ly2, 12, t.muted, Weight::Regular, Align::Right);
                }
                y += h;
            }

            ui_.endScroll("details-left", y + off - top);
        }

        // ---------------- right: the garage
        {
            const Rectangle c = R(rx, top, rwid, bottom - top);
            ui_.card(c, "Garage", d.cars.size() == 1 ? std::string("1 car") : strf("%zu cars", d.cars.size()));
            const Rectangle view = R(c.x + 6, c.y + 50, c.width - 12, c.height - 56);
            const float off = ui_.beginScroll("details-garage", view);
            float y = view.y - off;
            const float x = view.x + 6, w = view.width - 22;
            if (d.cars.empty())
            {
                ui_.text("No cars in the garage yet.", x + 14, y + 6, 13.5f, t.muted);
                y += 30;
            }
            for (const DetailCar &car : d.cars)
            {
                const bool open = expandedCar_ == car.slot;
                const Rectangle row = R(x, y, w, 54);
                if (ui_.rowButton("car-" + std::to_string(car.slot), row, open))
                    expandedCar_ = open ? -1 : car.slot;
                // colour swatch
                const float cx = row.x + 22, cy = row.y + 27;
                ui_.circle(cx, cy, 10, car.hasRgb ? Color{car.r, car.g, car.b, 255} : t.panel3);
                ui_.ring(cx, cy, 10, 11.2f, ui::alpha(WHITE, 0.25f));
                float nx = row.x + 44;
                const float nameRoom = w - 44 - 40 - (car.riding ? 70 : 0) - (car.prize ? 56 : 0);
                nx += ui_.text(ui_.fit(car.name, nameRoom, 14.5f, Weight::Semibold), nx, row.y + 8, 14.5f, car.nameFromDisc ? t.text : t.muted,
                               Weight::Semibold) +
                      8;
                if (car.riding)
                    nx += ui_.chip(nx, row.y + 8, "Driving", ui::alpha(t.accent, 0.2f), t.accent, 11) + 6;
                if (car.prize)
                    ui_.chip(nx, row.y + 8, "Prize", ui::alpha(t.warn, 0.18f), t.warn, 11);
                const std::string line = car.colourName + kDot + km(car.odometerM) + kDot + strf("%.0f hp", car.power) + kDot +
                                         groupedNumber(static_cast<uint64_t>(std::max(0, car.weightKg))) + " kg" + kDot + car.acquired;
                ui_.text(ui_.fit(line, w - 44 - 40, 12.5f), row.x + 44, row.y + 31, 12.5f, t.muted);
                // chevron
                {
                    const float ax = row.x + row.width - 20, ay = row.y + 27;
                    if (open)
                    {
                        ui_.line(ax - 5, ay - 2.5f, ax, ay + 2.5f, 1.8f, t.muted);
                        ui_.line(ax, ay + 2.5f, ax + 5, ay - 2.5f, 1.8f, t.muted);
                    }
                    else
                    {
                        ui_.line(ax - 2.5f, ay - 5, ax + 2.5f, ay, 1.8f, t.faint);
                        ui_.line(ax + 2.5f, ay, ax - 2.5f, ay + 5, 1.8f, t.faint);
                    }
                }
                y += 58;
                if (open)
                {
                    // Parts on the left, settings on the right.
                    const float px = x + 16, pw = std::floor((w - 32) * 0.52f), sx = px + pw + 16, swid = w - 32 - pw - 16;
                    float py = y + 4, sy = y + 4;
                    if (!car.fullName.empty())
                    {
                        py += ui_.wrap("Full name: " + car.fullName, px, py, w - 32, 12.5f, t.muted) + 6;
                        sy = py;
                    }
                    ui_.text("FITTED PARTS", px, py, 11, t.faint, Weight::Semibold);
                    py += 20;
                    for (const std::string &p : car.parts)
                    {
                        ui_.circle(px + 4, py + 8, 2.2f, t.accent);
                        py += ui_.wrap(p, px + 14, py, pw - 14, 12.5f, t.text, Weight::Regular, 1.3f) + 3;
                    }
                    py += ui_.wrap(car.partsNote, px, py + 2, pw, 12, t.faint) + 4;
                    if (!car.settings.empty())
                    {
                        ui_.text("SETTINGS (AS GT4 STORES THEM)", sx, sy, 11, t.faint, Weight::Semibold);
                        sy += 20;
                        const float labW = std::min(140.0f, swid * 0.48f);
                        for (const DetailSetting &st : car.settings)
                        {
                            ui_.text(ui_.fit(st.label, labW - 6, 12), sx, sy, 12, t.faint);
                            sy += std::max(17.0f, ui_.wrap(st.value, sx + labW, sy, swid - labW, 12.5f, t.text, Weight::Regular, 1.3f)) + 2;
                        }
                    }
                    const float endY = std::max(py, sy) + 10;
                    ui_.fill(R(x + 12, endY - 4, w - 24, 1), ui::alpha(t.border, 0.7f));
                    y = endY + 4;
                }
            }
            ui_.endScroll("details-garage", y + off - view.y + 6);
        }
    }

    // ------------------------------------------------------------------ How it works
    namespace
    {
        Color docColor(DocColor c)
        {
            const ui::Theme &t = ui::theme();
            switch (c)
            {
            case DocColor::Text: return t.text;
            case DocColor::Body: return ui::mixColor(t.text, t.muted, 0.25f);
            case DocColor::Muted: return t.muted;
            case DocColor::Faint: return t.faint;
            case DocColor::Accent: return t.accent;
            case DocColor::AccentText: return Color{150, 188, 255, 255};
            case DocColor::Ok: return t.ok;
            case DocColor::Warn: return t.warn;
            case DocColor::Panel: return t.panel;
            case DocColor::Panel2: return t.panel2;
            case DocColor::Panel3: return t.panel3;
            case DocColor::RowAlt: return ui::mixColor(t.panel, t.panel2, 0.7f);
            case DocColor::Border: return t.border;
            case DocColor::CodeBg: return Color{15, 19, 26, 255};
            case DocColor::AccentFill: return ui::mixColor(t.panel, t.accent, 0.11f);
            case DocColor::AccentLine: return ui::mixColor(t.panel, t.accent, 0.5f);
            case DocColor::WarnFill: return ui::mixColor(t.panel, t.warn, 0.09f);
            case DocColor::WarnLine: return ui::mixColor(t.panel, t.warn, 0.5f);
            case DocColor::OkFill: return ui::mixColor(t.panel, t.ok, 0.16f);
            }
            return t.text;
        }

        Weight docWeight(DocFont f)
        {
            switch (f)
            {
            case DocFont::Semibold: return Weight::Semibold;
            case DocFont::Bold: return Weight::Bold;
            case DocFont::Mono: return Weight::Mono;
            default: return Weight::Regular;
            }
        }
    }

    void App::drawDocItem(const DocItem &it, float ox, float oy)
    {
        const Color c = ui::alpha(docColor(it.color), it.alpha);
        const float x = ox + it.x, y = oy + it.y;
        switch (it.kind)
        {
        case DocItem::Kind::Text:
            // Consolas sits higher in its line box than Segoe UI: lowered to share the baseline.
            ui_.text(it.text, x, it.font == DocFont::Mono ? y + it.size * 0.1f : y, it.size, c, docWeight(it.font));
            break;
        case DocItem::Kind::Fill: ui_.fill(R(x, y, it.w, it.h), c); break;
        case DocItem::Kind::Round: ui_.round(R(x, y, it.w, it.h), it.r, c); break;
        case DocItem::Kind::RoundLine: ui_.roundLine(R(x, y, it.w, it.h), it.r, it.thick, c); break;
        case DocItem::Kind::Line:
            if (it.heads == 0 && it.dash <= 0.0f)
                ui_.line(x, y, x + it.w, y + it.h, it.thick, c);
            else
                ui_.arrowLine(x, y, x + it.w, y + it.h, it.thick, c, it.heads, it.dash);
            break;
        case DocItem::Kind::Circle: ui_.circle(x, y, it.r, c); break;
        case DocItem::Kind::Check:
            ui_.circle(x, y, it.r, c);
            ui_.icon(Icon::Check, R(x - it.r * 0.8f, y - it.r * 0.8f, it.r * 1.6f, it.r * 1.6f), ui::theme().panel);
            break;
        }
    }

    const DocLayout &App::howLayout(float width)
    {
        // Laid out again only when the column or the scale changes (every text width depends on both).
        if (width != howLayoutW_ || ui_.s != howLayoutS_)
        {
            howLayout_ = layoutDoc(howDoc_, width, [this](const std::string &t, float size, DocFont f) { return ui_.textWidth(t, size, docWeight(f)); });
            howLayoutW_ = width;
            howLayoutS_ = ui_.s;
        }
        return howLayout_;
    }

    void App::pageHowItWorks()
    {
        const ui::Theme &t = ui::theme();
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, cw = x1 - x0, bottom = ui_.height - 48;
        ui_.text(howDoc_.title.empty() ? std::string("How it works") : howDoc_.title, x0, 28, 28, t.text, Weight::Bold);
        const float bw = 150;
        ui_.text(ui_.fit("GT4 Recompilation in plain words: what was translated, what was rebuilt, and what still comes from your disc.", cw - bw - 20,
                         14.5f),
                 x0, 66, 14.5f, t.muted);
        if (ui_.button("how-back", R(x1 - bw, 30, bw, 36), "\xe2\x86\x90  Back to About", Style::Secondary, true, 13.5f))
        {
            aboutView_ = 0;
            return;
        }
        const float sideW = std::min(270.0f, std::floor(cw * 0.27f));
        const float rx = x1 - sideW;
        const Rectangle card = R(x0, kTop, cw - sideW - kGap, bottom - kTop);
        ui_.round(card, 12, t.panel);
        ui_.roundLine(card, 12, 1, ui::alpha(t.border, 0.8f));
        const Rectangle view = R(card.x + 2, card.y + 2, card.width - 4, card.height - 4);
        const float tx = card.x + 30, tw = std::floor(std::min(card.width - 60, 720.0f));
        const float padTop = 26.0f;
        const DocLayout &L = howLayout(tw);
        // The test pictures: a place in the text (a section, a heading, the diagram...).
        if (!howAnchor_.empty())
        {
            ui_.scrollTo("how", std::max(0.0f, L.anchor(howAnchor_, 0.0f) + padTop - 18.0f));
            howAnchor_.clear();
        }
        const float off = ui_.beginScroll("how", view);
        // A jump from "On this page" stays marked while the text stays where the jump put it
        // (the last sections cannot scroll to the top, so their headings sit lower down).
        if (howJumpPending_)
        {
            howJumpOff_ = off;
            howJumpPending_ = false;
        }
        else if (howJump_ >= 0 && std::fabs(off - howJumpOff_) > 0.5f)
            howJump_ = -1;
        const float oy = card.y + padTop - off;
        if (howDoc_.sections.empty())
            ui_.wrap("The text is built into GT4Recompilation.exe; this build has no copy of it.", tx, oy, tw, 14.5f, t.muted);
        // Only what is in view is drawn (the layout is in content coordinates).
        const float viewTop = off - padTop - 20.0f, viewBottom = off - padTop + view.height + 20.0f;
        for (const DocItem &it : L.items)
            if (it.bottom() >= viewTop && it.top() <= viewBottom)
                drawDocItem(it, tx, oy);
        const float contentH = padTop + L.height + 10.0f;
        ui_.endScroll("how", contentH);

        // On this page: the sections, each a jump.
        {
            const float rowH = 33.0f;
            const float h = std::min(bottom - kTop, 54.0f + rowH * static_cast<float>(howDoc_.sections.size()) + 10.0f);
            const Rectangle c = R(rx, kTop, sideW, h);
            ui_.card(c, "On this page");
            float y2 = c.y + 50;
            const float cur = ui_.scrollOffset("how");
            const float maxOff = std::max(0.0f, contentH - view.height);
            int active = 0;
            for (size_t i = 0; i < L.sectionY.size(); ++i)
                if (L.sectionY[i] <= cur + 40)
                    active = static_cast<int>(i);
            if (howJump_ >= 0 && static_cast<size_t>(howJump_) < L.sectionY.size())
                active = howJump_; // the section just jumped to
            else if (maxOff > 0.0f && cur >= maxOff - 1.0f && !L.sectionY.empty())
                active = static_cast<int>(L.sectionY.size()) - 1; // at the end: the last section
            for (size_t i = 0; i < howDoc_.sections.size() && y2 + rowH - 4 <= c.y + c.height; ++i)
            {
                const DocSection &s = howDoc_.sections[i];
                const bool on = static_cast<int>(i) == active;
                const Rectangle r = R(c.x + 10, y2, c.width - 20, rowH - 4);
                if (ui_.rowButton("how-sec-" + std::to_string(i), r, on) && i < L.sectionY.size())
                {
                    ui_.scrollTo("how", L.sectionY[i]);
                    howJump_ = static_cast<int>(i);
                    howJumpPending_ = true;
                }
                float lx = r.x + 12;
                if (!s.number.empty())
                {
                    ui_.text(s.number, r.x + 12, r.y + (r.height - 11.5f * 1.33f) * 0.5f + 1.2f, 11.5f, on ? t.accent : t.faint, Weight::Mono);
                    lx = r.x + 34;
                }
                ui_.text(ui_.fit(s.title, r.x + r.width - 10 - lx, 13), lx, r.y + (r.height - 13 * 1.33f) * 0.5f, 13, on ? t.text : t.muted,
                         on ? Weight::Semibold : Weight::Regular);
                y2 += rowH;
            }
        }
    }
}
