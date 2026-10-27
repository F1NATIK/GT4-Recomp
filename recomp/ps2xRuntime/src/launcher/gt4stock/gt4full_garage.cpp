// GT4 Recompilation launcher, Full edition: the Garage page (FULL-LAUNCHER-PLAN.md 2.6, the mock-ups): My garage and
// Compare cars read the save; Car Dealers lists GT4's hidden black cars and its prize cars from the disc. The actions that
// change the save (oil change, wash, removing Increase Rigidity, buying or adding a car) each open a confirmation that
// lists exactly what changes; they run only with GT4 closed, after an automatic backup (gt4full_saveedit.h,
// gt4full_garageedit.cpp). Each was tested on copies of a test save, loaded in GT4 (FULL-LAUNCHER-PLAN.md 7.6).

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;
    using fullui::Pill;

    namespace
    {
        constexpr float kSidebarW = 232.0f;
        constexpr float kTop = 104.0f;
        constexpr float kGap = 16.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kDot = " \xc2\xb7 ";

        std::string km(double kmValue) { return kmValue < 100 ? strf("%.1f km", kmValue) : groupedNumber(static_cast<uint64_t>(std::llround(kmValue))) + " km"; }

        // GT4's index power is PS: GT4's US car screen shows it x 0.98632 as HP (265.88 -> 262 HP, 722.42 -> 712 HP in the
        // SAVE-FORMAT.md runs) [I].
        std::string power(double ps, int unit)
        {
            if (ps <= 0)
                return "-";
            if (unit == 0)
                return strf("%.0f PS", ps);
            if (unit == 2)
                return strf("%.0f kW", ps * 0.73550);
            return strf("%.0f hp", ps * 0.98632);
        }
        double powerValue(double ps, int unit) { return unit == 0 ? ps : unit == 2 ? ps * 0.73550 : ps * 0.98632; }
        std::string torque(int x100, int unit)
        {
            if (x100 <= 0)
                return "-";
            const double kgfm = x100 / 100.0;
            if (unit == 1)
                return strf("%.1f kgfm", kgfm);
            if (unit == 2)
                return strf("%.0f lb-ft", kgfm * 7.23301);
            return strf("%.0f Nm", kgfm * 9.80665);
        }
        std::string seconds(double s) { return s > 0 ? strf("about %.1f s", s) : std::string("-"); }
        // Power to weight ratio (developer): power per tonne in the page's power unit ("255 hp per tonne"); 0 when unknown.
        double perTonne(double ps, int weightKg, int unit) { return ps > 0 && weightKg > 0 ? powerValue(ps, unit) * 1000.0 / weightKg : 0.0; }
        std::string perTonneText(double v, int unit) { return v > 0 ? strf("%.0f %s per tonne", v, unit == 0 ? "PS" : unit == 2 ? "kW" : "hp") : std::string("-"); }
    }

    // ------------------------------------------------------------------ the page
    void App::pageGarage()
    {
        loadGarage();
        drawHeader("Garage", "Your cars, read from your save: details, condition and a side-by-side compare. Car Dealers lists GT4's hidden and prize cars.");
        const float x0 = kSidebarW + 32, x1 = ui_.width - 32, bottom = ui_.height - 48, cw = x1 - x0;
        int tab = garageTab_;
        if (fullui::tabs(ui_, "garage-tabs", x0, kTop - 10, cw, {"My garage", "Compare cars", "Car Dealers", "GT Mode prizes"}, tab))
        {
            garageTab_ = tab;
            ui_.closePopups();
            compareStatsOpen_ = false;
        }
        const float y0 = kTop + 40;
        garageSourceLine(x0, y0, cw);
        const float top = y0 + 30;
        if (garageTab_ == 2)
        {
            garageDealers(x0, top, x1, bottom);
            return;
        }
        if (garageTab_ == 3)
        {
            garagePrizes(x0, top, x1, bottom);
            return;
        }
        const ui::Theme &t = ui::theme();
        if (!garage_.loaded)
        {
            ui_.wrap(garage_.problem.empty() ? std::string("No GT4 save on this card yet: GT4 makes one the first time you save in the game.")
                                             : garage_.problem,
                     x0, top + 10, cw, 14, t.muted);
            return;
        }
        if (garageTab_ == 1)
            garageCompare(x0, top, x1, bottom);
        else
            garageMine(x0, top, x1, bottom);
    }

    // ------------------------------------------------------------------ My garage
    void App::garageMine(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        const FullSettings &f = st_.full;
        const float cw = x1 - x0;
        const float lw = std::floor(std::min(440.0f, cw * 0.40f));
        // Garage condition (the mock-up's Overview counts)
        {
            const float tw = std::floor((lw - 2 * 10) / 3), th = 70;
            fullui::statTile(ui_, R(x0, y0, tw, th), std::to_string(garage_.oilDue), "due an oil change", garage_.oilDue ? t.warn : t.text);
            fullui::statTile(ui_, R(x0 + tw + 10, y0, tw, th), std::to_string(garage_.needsWash), "need a wash", garage_.needsWash ? t.warn : t.text);
            fullui::statTile(ui_, R(x0 + 2 * (tw + 10), y0, tw, th), std::to_string(garage_.withRigidity), "with Body Rigidity", t.text);
        }
        // The list
        const float ly = y0 + 70 + 12;
        {
            const Rectangle c = R(x0, ly, lw, y1 - ly);
            ui_.card(c, strf("%zu cars", garage_.cars.size()));
            int sort = garageSort_;
            if (ui_.segmented("garage-sort", R(c.x + c.width - 230, c.y + 12, 214, 28), {"GT4's order", "Name", "Power"}, sort, true, 12))
                garageSort_ = sort;
            std::vector<const GarageCar *> cars;
            for (const GarageCar &g : garage_.cars)
                cars.push_back(&g);
            if (garageSort_ == 1)
                std::stable_sort(cars.begin(), cars.end(), [](const GarageCar *a, const GarageCar *b) { return lower(a->name) < lower(b->name); });
            else if (garageSort_ == 2)
                std::stable_sort(cars.begin(), cars.end(), [](const GarageCar *a, const GarageCar *b) { return a->powerPs > b->powerPs; });
            const Rectangle view = R(c.x + 6, c.y + 50, c.width - 12, c.height - 56);
            const float off = ui_.beginScroll("garage-list", view);
            float y = view.y - off;
            const float rowH = 52;
            for (const GarageCar *g : cars)
            {
                const Rectangle r = R(view.x + 4, y, view.width - 18, rowH - 4);
                if (y + rowH > view.y - 60 && y < view.y + view.height + 60)
                {
                    if (ui_.rowButton("car-" + std::to_string(g->slot), r, g->slot == garageSel_))
                        garageSel_ = g->slot;
                    if (g->hasRgb)
                    {
                        ui_.circle(r.x + 16, r.y + 15, 6.5f, Color{g->r, g->g, g->b, 255});
                        ui_.ring(r.x + 16, r.y + 15, 6.5f, 7.5f, ui::alpha(t.border, 0.9f));
                    }
                    ui_.text(ui_.fit(g->name, r.width - 46, 13.5f, Weight::Semibold), r.x + 32, r.y + 6, 13.5f, t.text, Weight::Semibold);
                    ui_.text(ui_.fit(km(g->odometerM / 1000.0) + kDot + power(g->powerPs, f.powerUnit) + kDot + groupedNumber(static_cast<uint64_t>(std::max(0, g->weightKg))) +
                                         " kg" + (g->kind == 2 ? std::string(kDot) + "driving" : std::string()),
                                     r.width - 46, 12),
                             r.x + 32, r.y + 26, 12, t.muted);
                    if (g->oilDue || g->dirt01 >= kWashFrom)
                        ui_.circle(r.x + r.width - 12, r.y + 15, 4, t.warn);
                }
                y += rowH;
            }
            ui_.endScroll("garage-list", y + off - view.y);
        }
        // The car
        const GarageCar *g = garage_.bySlot(garageSel_);
        if (!g)
            return;
        const float rx = x0 + lw + kGap;
        const Rectangle c = R(rx, y0, x1 - rx, y1 - y0);
        ui_.round(c, 12, t.panel);
        ui_.roundLine(c, 12, 1, ui::alpha(t.border, 0.8f));
        const float inner = c.width - 40;
        const Rectangle view = R(c.x + 2, c.y + 2, c.width - 4, c.height - 4);
        const float off = ui_.beginScroll("garage-car", view);
        float y = c.y + 18 - off;
        // Hero: GT4's own picture, then the names.
        const float picW = std::min(360.0f, inner * 0.48f), picH = std::floor(picW * 100.0f / 160.0f);
        drawCarPicture(R(c.x + 20, y, picW, picH), g->label, true, blackCarGrey(g->label, g->colourIndex));
        {
            const float tx = c.x + 20 + picW + 20, tw = c.x + c.width - 20 - tx;
            float ty = y + 4;
            fullui::caps(ui_, tx, ty, g->maker.empty() ? std::string("GT4") : g->maker, t.accent);
            ty += 20;
            ty += ui_.wrap(g->fullName, tx, ty, tw, 20, t.text, Weight::Bold) + 6;
            std::string line = g->colourName;
            if (g->disc)
                line += kDot + std::string(drivetrainName(g->disc->drivetrain)) + (g->disc->year ? kDot + std::to_string(g->disc->year) : std::string());
            ui_.text(ui_.fit(line, tw, 13), tx, ty, 13, t.muted);
            ty += 22;
            ui_.text(ui_.fit(g->acquired + kDot + km(g->odometerM / 1000.0), tw, 13), tx, ty, 13, t.muted);
            ty += 24;
            const char *kind = g->kind == 2 ? "The car you drive" : g->kind == 3 ? "Kept with its parts and settings" : "As it came: GT4 makes its full record the first time you take it out";
            ui_.wrap(kind, tx, ty, tw, 12.5f, t.faint);
        }
        y += picH + 20;
        const float colW = std::floor((inner - 20) * 0.5f);
        // Performance (GT4's numbers, the disc's car data, the launcher's estimates)
        float py = y;
        {
            ui_.text("Performance", c.x + 20, py, 15, t.text, Weight::Semibold);
            py += 28;
            auto row = [&](const std::string &k, const std::string &v, Color vc) {
                fullui::specRow(ui_, c.x + 20, py, colW, k, v, vc, 13);
                py += 30;
            };
            row("Power (GT4's garage list)", power(g->powerPs, f.powerUnit), t.text);
            if (g->disc && g->disc->torqueX100)
                row("Torque (catalogue)", torque(g->disc->torqueX100, f.torqueUnit), t.text);
            row("Weight (GT4's garage list)", g->weightKg > 0 ? groupedNumber(static_cast<uint64_t>(g->weightKg)) + " kg" : std::string("-"), t.text);
            if (g->weightKg > 0 && g->powerPs > 0)
                row("Power to weight ratio", perTonneText(perTonne(g->powerPs, g->weightKg, f.powerUnit), f.powerUnit), t.text);
            if (g->disc && g->disc->frontWeightPct > 0)
                row("Weight balance (front : rear)", weightBalanceText(g->disc->frontWeightPct), t.text);
            if (g->disc && g->disc->front.ok())
                row("Tyres front / rear", tyreText(g->disc->front) + " / " + tyreText(g->disc->rear), t.text);
            if (!g->tyreType.empty())
                row("Tyre type", g->tyreType, t.text);
            row("0-100 km/h", seconds(g->est.zeroTo100), t.muted);
            row("0-200 km/h", seconds(g->est.zeroTo200), t.muted);
            row("100-0 km/h braking", g->est.ok ? strf("about %.0f m", g->est.brake100) : std::string("-"), t.muted);
            row("Top speed", g->est.ok ? strf("about %.0f km/h", g->est.topSpeedKmh) : std::string("-"), t.muted);
            const std::string listNote =
                std::string(g->kind == 2 ? "Power and weight: GT4's garage list. For the car you drive GT4 works its own figure out again each time it loads, so its car "
                                           "screen can show more. "
                                         : "Power and weight: GT4's garage list, from when the car was last put away; GT4's car screen works its own figure out "
                                           "again (fresh oil, parts), so it can show more. ") +
                "\"About\": the launcher's estimates from power, weight, drivetrain and tyres. GT4 does not store them.";
            py += ui_.wrap(listNote, c.x + 20, py, colW, 11.5f, t.faint) + 10;
        }
        // Condition and services (right column)
        float qy = y;
        {
            const float qx = c.x + 20 + colW + 20;
            ui_.text("Condition", qx, qy, 15, t.text, Weight::Semibold);
            if (!g->condition)
                ui_.text("when first taken out", qx + colW, qy + 3, 12, t.faint, Weight::Regular, Align::Right);
            qy += 28;
            auto bar = [&](const std::string &k, const std::string &v, float v01, float warnAt, float badAt) {
                ui_.text(k, qx, qy, 13, t.muted);
                ui_.text(v, qx + colW, qy, 13, t.text, Weight::Semibold, Align::Right);
                fullui::conditionBar(ui_, R(qx, qy + 22, colW, 7), v01, warnAt, badAt);
                qy += 42;
            };
            bar("Oil age", km(g->oilAgeKm) + (g->oilDue ? "  (change due)" : ""), static_cast<float>(g->oilAgeKm / 6000.0), 5000.0f / 6000.0f, 0.999f);
            bar("Dirt", strf("%.0f%%", g->dirt01 * 100.0), static_cast<float>(g->dirt01), static_cast<float>(kWashFrom), 0.75f);
            // Body wear: GT4's own count (metres of driving, up to 3,000 km; its Rigidity Refresher Plan sets it back to 0). How
            // GT4 weighs it (getBodyLifeRate's curve) is not decoded, so the bar is neutral: no warning colours.
            bar("Body wear (GT4's count)", km(g->bodyWearKm) + (g->bodyWearKm >= 2999.5 ? " (GT4's maximum)" : " of 3,000"), static_cast<float>(g->bodyWearKm / 3000.0), 2.0f,
                2.0f);
            if (g->frozen)
            {
                ui_.text("GT4 keeps this car's condition as it is.", qx, qy, 12.5f, t.faint);
                qy += 22;
            }
            qy += ui_.wrap("Body wear grows as the car is driven. Rigidity Refresh (GT4's Rigidity Refresher Plan) sets it back to 0. GT4 offers it for every car it "
                           "has a tune shop for, not only cars with Increase Rigidity, and counts it as already done while the wear is 500 km or less.",
                           qx, qy - 6, colW, 11.5f, t.faint) +
                  4;
            // GT4's services (they change the save: GT4 closed, a backup and the player's OK first; gt4full_saveedit.h).
            const std::string block = garageEditBlock();
            {
                const std::string oilWhy = block.empty() ? serviceBlock(garage_, *g, GarageAction::OilChange) : block;
                const std::string washWhy = block.empty() ? serviceBlock(garage_, *g, GarageAction::Wash) : block;
                const std::string refreshWhy = block.empty() ? serviceBlock(garage_, *g, GarageAction::RefreshBody) : block;
                // Rigidity Refresh next to Oil change and Wash car (developer): one row of three when each button gets 150 px or
                // more, else on its own row under the other two.
                const bool oneRow = colW >= 3 * 150.0f + 20;
                const float bw = oneRow ? std::floor((colW - 20) / 3.0f) : std::floor((colW - 10) * 0.5f);
                if (ui_.iconButton("svc-oil", R(qx, qy, bw, 34), Icon::Refresh, "Oil change", Style::Secondary, oilWhy.empty(), 13))
                    openGarageEdit(serviceEdit(*g, GarageAction::OilChange));
                if (ui_.iconButton("svc-wash", R(qx + bw + 10, qy, bw, 34), Icon::Sparkle, "Wash car", Style::Secondary, washWhy.empty(), 13))
                    openGarageEdit(serviceEdit(*g, GarageAction::Wash));
                const Rectangle rr = oneRow ? R(qx + 2 * (bw + 10), qy, colW - 2 * (bw + 10), 34) : R(qx, qy + 42, colW, 34);
                if (ui_.iconButton("svc-refresh", rr, Icon::Garage, "Rigidity Refresh", Style::Secondary, refreshWhy.empty(), 13))
                    openGarageEdit(serviceEdit(*g, GarageAction::RefreshBody));
                qy += oneRow ? 42 : 84;
                std::string why;
                if (!block.empty() || (oilWhy == washWhy && washWhy == refreshWhy))
                    why = oilWhy;
                else
                {
                    if (!oilWhy.empty())
                        why = "Oil change: " + oilWhy;
                    if (!washWhy.empty())
                        why += (why.empty() ? "" : " ") + std::string("Wash: ") + washWhy;
                    if (!refreshWhy.empty())
                        why += (why.empty() ? "" : " ") + std::string("Rigidity Refresh: ") + refreshWhy;
                }
                const std::string prices = strf("GT4's prices: oil change and wash Cr. %llu each, Rigidity Refresh Cr. %s.", static_cast<unsigned long long>(kServicePrice),
                                                groupedNumber(kRefreshPrice).c_str());
                qy += ui_.wrap(why.empty() ? prices + " You see what changes before your save is touched; it is backed up first." : why + " " + prices, qx, qy, colW,
                               12, t.faint) +
                      8;
            }
            qy += 10;
            ui_.text("Fitted parts", qx, qy, 15, t.text, Weight::Semibold);
            qy += 26;
            if (g->rigidity)
            {
                const std::string why = block.empty() ? serviceBlock(garage_, *g, GarageAction::RemoveRigidity) : block;
                const std::string partName = "Increase Rigidity (Body Rigidity Improvement)";
                const std::string partNote = "GT4 cannot take it off; on some cars it makes the handling worse.";
                const float textW = colW - 118;
                const float nameH = ui_.wrapHeight(partName, textW, 13, Weight::Semibold), noteH = ui_.wrapHeight(partNote, textW, 11.5f);
                const Rectangle pr = R(qx, qy, colW, std::max(50.0f, 7 + nameH + 3 + noteH + 8));
                ui_.round(pr, 8, ui::alpha(t.warn, 0.10f));
                const float ny = qy + 7 + ui_.wrap(partName, qx + 12, qy + 7, textW, 13, t.text, Weight::Semibold) + 3;
                ui_.wrap(partNote, qx + 12, ny, textW, 11.5f, t.muted);
                if (ui_.button("svc-unrigid", R(pr.x + pr.width - 96, pr.y + (pr.height - 30) * 0.5f, 86, 30), "Remove", Style::Secondary, why.empty(), 12.5f))
                    openGarageEdit(serviceEdit(*g, GarageAction::RemoveRigidity));
                qy += pr.height + 6;
                if (!why.empty() && why != block)
                    qy += ui_.wrap(why, qx, qy, colW, 12, t.faint) + 6;
            }
            for (const std::string &p : g->parts)
            {
                qy += ui_.wrap(p, qx, qy, colW, 12.5f, t.muted) + 3;
            }
            if (!g->partsNote.empty())
                qy += ui_.wrap(g->partsNote, qx, qy + 4, colW, 12, t.faint) + 4;
        }
        const float contentEnd = std::max(py, qy) + 20;
        ui_.endScroll("garage-car", contentEnd + off - c.y);
    }

    // ------------------------------------------------------------------ Compare cars (up to 4)
    void App::garageCompare(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        const bool dev = showUnbuilt_;
        FullSettings &f = st_.full;
        const float cw = x1 - x0;
        std::vector<const GarageCar *> cars;
        for (int slot : f.compareSlots)
            if (const GarageCar *g = garage_.bySlot(slot))
                cars.push_back(g);
        std::vector<std::string> names;
        for (const GarageCar &g : garage_.cars)
            names.push_back(g.name);
        // Four car cards
        const float labelW = 190;
        const float colW = std::floor((cw - labelW - 3 * 10) / 4);
        const float cardH = std::floor((colW - 24) * 100.0f / 160.0f) + 30 + 6 + 20 + 34 + 30 + 12;
        for (int i = 0; i < 4; ++i)
        {
            const float cx = x0 + labelW + i * (colW + 10);
            const Rectangle c = R(cx, y0, colW, cardH);
            ui_.round(c, 10, t.panel);
            ui_.roundLine(c, 10, 1, ui::alpha(t.border, 0.8f));
            fullui::caps(ui_, c.x + 12, c.y + 10, strf("Car %c", 'A' + i), t.faint);
            const GarageCar *g = i < static_cast<int>(cars.size()) ? cars[static_cast<size_t>(i)] : nullptr;
            const float picH = std::floor((colW - 24) * 100.0f / 160.0f);
            drawCarPicture(R(c.x + 12, c.y + 30, colW - 24, picH), g ? g->label : std::string(), false, g ? blackCarGrey(g->label, g->colourIndex) : 0);
            float yy = c.y + 30 + picH + 6;
            if (g)
                fullui::caps(ui_, c.x + 12, yy, g->maker.empty() ? std::string("GT4") : g->maker, t.accent);
            yy += 20;
            int sel = -1;
            for (size_t k = 0; k < garage_.cars.size(); ++k)
                if (g && garage_.cars[k].slot == g->slot)
                    sel = static_cast<int>(k);
            if (ui_.dropdown(strf("compare-%d", i), R(c.x + 12, yy, colW - 24, 30), names, sel, !garage_.cars.empty(), 12))
            {
                const int slot = garage_.cars[static_cast<size_t>(sel)].slot;
                if (static_cast<int>(f.compareSlots.size()) <= i)
                    f.compareSlots.push_back(slot);
                else
                    f.compareSlots[static_cast<size_t>(i)] = slot;
                markDirty();
            }
            if (ui_.button(strf("compare-view-%d", i), R(c.x + 12, yy + 36, colW - 24, 26), "View in garage", Style::Ghost, g != nullptr, 12))
            {
                garageSel_ = g->slot;
                garageTab_ = 0;
            }
        }
        // Units
        ui_.text("Units", x0, y0 + 6, 13, t.muted);
        int pu = std::clamp(f.powerUnit, 0, 2), tu = std::clamp(f.torqueUnit, 0, 2);
        if (ui_.segmented("unit-power", R(x0, y0 + 28, labelW - 20, 28), {"PS", "hp", "kW"}, pu, true, 12))
        {
            f.powerUnit = pu;
            markDirty();
        }
        if (ui_.segmented("unit-torque", R(x0, y0 + 62, labelW - 20, 28), {"Nm", "kgfm", "lb-ft"}, tu, true, 12))
        {
            f.torqueUnit = tu;
            markDirty();
        }
        ui_.wrap("Launcher units for this page only; GT4's own are in its Options.", x0, y0 + 98, labelW - 20, 11, t.faint);
        // Stats to show (remembered: GT4L_COMPARE_STATS, the ids shown; empty = all).
        struct Stat
        {
            const char *id, *label;
        };
        static const Stat kStats[] = {{"power", "Power"},        {"torque", "Torque"},      {"weight", "Weight"},       {"ratio", "Power to weight ratio"},
                                      {"balance", "Weight balance"}, {"drive", "Drivetrain"},   {"tyres", "Tyres"},        {"tyretype", "Tyre type"},  {"year", "Year"},
                                      {"0-100", "0-100"},        {"0-200", "0-200"},        {"0-300", "0-300"},         {"100-0", "100-0"},
                                      {"200-0", "200-0"},        {"top", "Top speed"},      {"odo", "Odometer"},        {"oil", "Oil age"},
                                      {"body", "Body wear"},     {"dirt", "Dirt"},          {"rigidity", "Body Rigidity"}};
        std::vector<std::string> shownIds = split(f.compareStats, ',');
        shownIds.erase(std::remove_if(shownIds.begin(), shownIds.end(), [](const std::string &v) { return v.empty(); }), shownIds.end());
        auto statOn = [&](const char *id) { return shownIds.empty() || std::find(shownIds.begin(), shownIds.end(), id) != shownIds.end(); };
        // A drop-down button (developer: tidier than the row of chips); its list of tick boxes opens over the table (drawn below).
        float sy = y0 + cardH + 12;
        size_t shownCount = 0;
        for (const Stat &st : kStats)
            shownCount += statOn(st.id) ? 1 : 0;
        const Rectangle statsButton = R(x0, sy, std::min(300.0f, cw), 34);
        if (ui_.button("compare-stats", statsButton, strf("Stats to show (%zu of %zu)", shownCount, std::size(kStats)), Style::Secondary, true, 13))
            compareStatsOpen_ = !compareStatsOpen_;
        {
            const float ax = statsButton.x + statsButton.width - 22, ay = statsButton.y + statsButton.height * 0.5f;
            if (compareStatsOpen_)
                ui_.triangle(Vector2{ax - 5, ay + 3}, Vector2{ax + 5, ay + 3}, Vector2{ax, ay - 3}, t.muted);
            else
                ui_.triangle(Vector2{ax - 5, ay - 3}, Vector2{ax + 5, ay - 3}, Vector2{ax, ay + 3}, t.muted);
        }
        sy += 46;
        // The table: specifications, performance (estimates), condition. The best value of each row is highlighted.
        const float ty = sy;
        const Rectangle view = R(x0, ty, cw, y1 - ty);
        const float off = ui_.beginScroll("compare-table", view);
        float y = ty - off;
        struct Row
        {
            std::string label;
            std::vector<std::string> values;
            std::vector<double> numbers; // for the best value (NaN: none)
            int better = 0;              // +1 higher is better, -1 lower is better, 0 no highlight
            const char *id = "";         // the "Stats to show" chip it belongs to
        };
        auto num = [](bool ok, double v) { return ok ? v : std::nan(""); };
        auto section = [&](const std::string &title, const std::vector<Row> &rows, const std::string &note) {
            bool any = false;
            for (const Row &r : rows)
                any = any || statOn(r.id);
            if (!any)
                return;
            ui_.text(title, x0, y, 15, t.text, Weight::Semibold);
            y += 28;
            for (const Row &r : rows)
            {
                if (!statOn(r.id))
                    continue;
                ui_.text(r.label, x0, y + 5, 13, t.muted);
                int best = -1;
                if (r.better != 0)
                    for (size_t i = 0; i < r.numbers.size(); ++i)
                        if (!std::isnan(r.numbers[i]) && (best < 0 || (r.better > 0 ? r.numbers[i] > r.numbers[static_cast<size_t>(best)] : r.numbers[i] < r.numbers[static_cast<size_t>(best)])))
                            best = static_cast<int>(i);
                for (size_t i = 0; i < r.values.size(); ++i)
                {
                    const Rectangle cell = R(x0 + labelW + i * (colW + 10), y, colW, 28);
                    const bool isBest = static_cast<int>(i) == best && cars.size() > 1;
                    if (isBest)
                        ui_.round(cell, 6, ui::alpha(t.accent, 0.16f));
                    ui_.text(ui_.fit(r.values[i], colW - 16, 13, Weight::Semibold), cell.x + 8, cell.y + 5, 13, isBest ? t.accent : t.text, Weight::Semibold);
                }
                ui_.fill(R(x0, y + 31, cw - 14, 1), ui::alpha(t.border, 0.6f));
                y += 34;
            }
            if (!note.empty())
            {
                y += 4;
                y += ui_.wrap(note, x0, y, cw - 14, 11.5f, t.faint) + 4;
            }
            y += 14;
        };
        std::vector<Row> spec(10), perf(6), cond(5);
        spec[0] = {"Power (garage list)", {}, {}, 1, "power"};
        spec[1] = {"Torque (catalogue)", {}, {}, 1, "torque"};
        spec[2] = {"Weight (garage list)", {}, {}, -1, "weight"};
        spec[3] = {"Power to weight ratio", {}, {}, 1, "ratio"};
        spec[4] = {"Weight balance (F : R)", {}, {}, 0, "balance"};
        spec[5] = {"Drivetrain", {}, {}, 0, "drive"};
        spec[6] = {"Tyres front", {}, {}, 0, "tyres"};
        spec[7] = {"Tyres rear", {}, {}, 0, "tyres"};
        spec[8] = {"Tyre type", {}, {}, 0, "tyretype"};
        spec[9] = {"Year", {}, {}, 0, "year"};
        perf[0] = {"0-100 km/h", {}, {}, -1, "0-100"};
        perf[1] = {"0-200 km/h", {}, {}, -1, "0-200"};
        perf[2] = {"0-300 km/h", {}, {}, -1, "0-300"};
        perf[3] = {"100-0 km/h braking", {}, {}, -1, "100-0"};
        perf[4] = {"200-0 km/h braking", {}, {}, -1, "200-0"};
        perf[5] = {"Top speed", {}, {}, 1, "top"};
        cond[0] = {"Odometer", {}, {}, 0, "odo"};
        cond[1] = {"Oil age", {}, {}, -1, "oil"};
        cond[2] = {"Body wear (GT4's count)", {}, {}, 0, "body"};
        cond[3] = {"Dirt", {}, {}, -1, "dirt"};
        cond[4] = {"Body Rigidity", {}, {}, 0, "rigidity"};
        for (const GarageCar *g : cars)
        {
            const DiscCar *d = g->disc;
            const double pv = powerValue(g->powerPs, f.powerUnit);
            spec[0].values.push_back(power(g->powerPs, f.powerUnit));
            spec[0].numbers.push_back(num(g->powerPs > 0, pv));
            spec[1].values.push_back(d ? torque(d->torqueX100, f.torqueUnit) : std::string("-"));
            spec[1].numbers.push_back(num(d && d->torqueX100 > 0, d ? d->torqueX100 : 0));
            spec[2].values.push_back(g->weightKg > 0 ? groupedNumber(static_cast<uint64_t>(g->weightKg)) + " kg" : std::string("-"));
            spec[2].numbers.push_back(num(g->weightKg > 0, g->weightKg));
            const double ptw = perTonne(g->powerPs, g->weightKg, f.powerUnit);
            spec[3].values.push_back(perTonneText(ptw, f.powerUnit));
            spec[3].numbers.push_back(num(ptw > 0, ptw));
            spec[4].values.push_back(weightBalanceText(d ? d->frontWeightPct : 0));
            spec[4].numbers.push_back(std::nan(""));
            spec[5].values.push_back(d ? drivetrainName(d->drivetrain) : "-");
            spec[5].numbers.push_back(std::nan(""));
            spec[6].values.push_back(d && d->front.ok() ? tyreText(d->front) : std::string("-"));
            spec[6].numbers.push_back(std::nan(""));
            spec[7].values.push_back(d && d->rear.ok() ? tyreText(d->rear) : std::string("-"));
            spec[7].numbers.push_back(std::nan(""));
            spec[8].values.push_back(g->tyreType.empty() ? std::string("-") : g->tyreType);
            spec[8].numbers.push_back(std::nan(""));
            spec[9].values.push_back(d && d->year ? std::to_string(d->year) : std::string("-"));
            spec[9].numbers.push_back(std::nan(""));
            perf[0].values.push_back(seconds(g->est.zeroTo100));
            perf[0].numbers.push_back(num(g->est.zeroTo100 > 0, g->est.zeroTo100));
            perf[1].values.push_back(seconds(g->est.zeroTo200));
            perf[1].numbers.push_back(num(g->est.zeroTo200 > 0, g->est.zeroTo200));
            perf[2].values.push_back(g->est.ok ? (g->est.zeroTo300 > 0 ? seconds(g->est.zeroTo300) : std::string("not reached")) : std::string("-"));
            perf[2].numbers.push_back(num(g->est.zeroTo300 > 0, g->est.zeroTo300));
            perf[3].values.push_back(g->est.ok ? strf("about %.0f m", g->est.brake100) : std::string("-"));
            perf[3].numbers.push_back(num(g->est.ok, g->est.brake100));
            perf[4].values.push_back(g->est.ok ? strf("about %.0f m", g->est.brake200) : std::string("-"));
            perf[4].numbers.push_back(num(g->est.ok, g->est.brake200));
            perf[5].values.push_back(g->est.ok ? strf("about %.0f km/h", g->est.topSpeedKmh) : std::string("-"));
            perf[5].numbers.push_back(num(g->est.ok, g->est.topSpeedKmh));
            cond[0].values.push_back(km(g->odometerM / 1000.0));
            cond[0].numbers.push_back(std::nan(""));
            cond[1].values.push_back(km(g->oilAgeKm));
            cond[1].numbers.push_back(g->oilAgeKm);
            cond[2].values.push_back(km(g->bodyWearKm));
            cond[2].numbers.push_back(g->bodyWearKm);
            cond[3].values.push_back(strf("%.0f%%", g->dirt01 * 100.0));
            cond[3].numbers.push_back(g->dirt01);
            cond[4].values.push_back(g->rigidity ? "fitted" : "-");
            cond[4].numbers.push_back(std::nan(""));
        }
        section("Specifications", spec,
                "Power and weight: GT4's garage list, from when each car was last put away (GT4's car screen works its own figure out again, so it can "
                "show more); torque, weight balance, drivetrain and tyre sizes: GT4's car data; tyre type: the fitted tyres in the save.");
        section("Performance", perf,
                "Estimates (\"about\"): the launcher's sums from power, weight, drivetrain and tyres (braking from the tyres' grip alone). GT4 does not store them.");
        if (dev && statOn("0-100") + statOn("0-200") + statOn("0-300") + statOn("100-0") + statOn("200-0") + statOn("top") > 0)
        {
            ui_.iconButton("perf-test", R(x0, y - 8, 230, 32), Icon::Play, "Run performance test", Style::Secondary, false, 12.5f);
            fullui::notBuilt(ui_, x0 + 244, y - 1, cw - 260, "each car driven hidden with GT4's own physics for exact times.");
            y += 40;
            ui_.text("Downforce at speed", x0, y, 15, t.faint, Weight::Semibold);
            y += 24;
            fullui::notBuilt(ui_, x0, y, cw - 14, "GT4's downforce values are not read from its car data yet.");
            y += 30;
        }
        section("Condition", cond, std::string());
        ui_.endScroll("compare-table", y + off - ty);
        // The "Stats to show" list: a tick box for each stat, in columns, over the table; a click outside or Esc closes it.
        if (compareStatsOpen_)
        {
            // Up to 4 columns of 200, so the list stays short and inside the window.
            const size_t cols = static_cast<size_t>(std::clamp(std::floor((cw - 40) / 200.0f), 2.0f, 4.0f));
            const float colStep = 200, rowH = 30, pw = 40 + cols * colStep;
            const size_t rows = (std::size(kStats) + cols - 1) / cols;
            const Rectangle pop = R(x0, statsButton.y + statsButton.height + 6, pw, 16 + 34 + rows * rowH + 10);
            ui_.round(R(pop.x + 2, pop.y + 4, pop.width, pop.height), 10, ui::alpha(BLACK, 0.35f));
            ui_.round(pop, 10, t.panel2);
            ui_.roundLine(pop, 10, 1, t.border);
            fullui::caps(ui_, pop.x + 20, pop.y + 18, "Rows in the table", t.faint);
            if (ui_.button("compare-stats-all", R(pop.x + pop.width - 20 - 96, pop.y + 10, 96, 28), "Show all", Style::Ghost, shownCount < std::size(kStats), 12.5f))
            {
                f.compareStats.clear();
                markDirty();
            }
            for (size_t i = 0; i < std::size(kStats); ++i)
            {
                const Stat &st = kStats[i];
                bool on = statOn(st.id);
                if (ui_.checkbox(std::string("compare-stat-") + st.id, pop.x + 20 + (i / rows) * colStep, pop.y + 50 + (i % rows) * rowH, on, st.label))
                {
                    std::vector<std::string> next;
                    for (const Stat &o : kStats)
                        if (o.id == st.id ? on : statOn(o.id))
                            next.push_back(o.id);
                    f.compareStats = next.size() == std::size(kStats) ? std::string() : next.empty() ? std::string("none") : join(next, ",");
                    markDirty();
                }
            }
            const bool pressedOutside = ui_.inputOn() && ui_.input().pressed && !CheckCollisionPointRec(ui_.mouseDp(), pop) &&
                                        !CheckCollisionPointRec(ui_.mouseDp(), statsButton);
            if (pressedOutside || IsKeyPressed(KEY_ESCAPE))
                compareStatsOpen_ = false;
        }
    }

    // ------------------------------------------------------------------ Car Dealers (Hidden Cars, Prize Cars)
    void App::garageDealers(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        const float cw = x1 - x0;
        if (!cat_)
        {
            ui_.wrap(catLoading_ ? std::string("Reading GT4's car data from the disc...")
                     : !disc_.exists ? std::string("Car Dealers reads GT4's cars from your disc image: choose it on the Play page.")
                                     : "GT4's car data could not be read: " + catError_,
                     x0, y0 + 10, cw, 14, t.muted);
            return;
        }
        // The cars of the section.
        struct Offer
        {
            const DiscCar *car;
            int colour = 0;
            bool black = false;
            int blackGrey = 0; // GT4's black: the picture darkened to it
            uint64_t price = 0;
            std::string priceNote, where, given;
            std::string guide; // the GT Mode prizes entry that gives it (guideKey), "" none
            const HiddenCar *hidden = nullptr; // Hidden Cars: GT4's black colour and lot, or a rare car
            bool extra = false;                // Extra Cars: a car the pack adds from the developer's own Japanese disc
        };
        // What GT4's prize table gives a car for, in a player's words ("Sunday Cup: win it; ...").
        auto givenFor = [&](const PrizeCar &p) {
            std::vector<std::string> given;
            for (const PrizeRow &r : p.given)
            {
                const std::string why = prizeReason(r, names_.get());
                if (std::find(given.begin(), given.end(), why) == given.end())
                    given.push_back(why);
            }
            return join(given, "; ");
        };
        // Extra Cars (analysis/jp-cars; no switch): the section is there while the car data, read through extra-cars\esso.gt4x
        // next to GT4.exe, has an extra car.
        bool hasExtra = false;
        for (const char *label : kExtraCars)
            hasExtra = hasExtra || cat_->carByLabel(label) != nullptr;
        if (dealerSection_ == 2 && !hasExtra)
        {
            dealerSection_ = 0;
            dealerMaker_ = 0;
            dealerSel_.clear();
        }
        std::vector<Offer> offers;
        if (dealerSection_ == 0)
        {
            for (const HiddenCar &h : kHiddenCars)
                if (const DiscCar *d = cat_->carByLabel(h.label); d && h.given())
                {
                    // The black Formula GT (developer, 2026-10-11): GT4 gives it; here new, at GT4's own price, in its black.
                    Offer o;
                    o.car = d;
                    o.colour = h.colour;
                    o.black = true;
                    o.blackGrey = h.blackGrey;
                    o.hidden = &h;
                    o.price = d->price;
                    o.priceNote = d->price ? "GT4's own price for this car (its car data), new" : "GT4's car data gives this car no price";
                    o.where = h.note ? h.note : "";
                    offers.push_back(o);
                }
                else if (d && h.rare())
                {
                    // A rare car (developer): GT4's own price, new, in GT4's first colour for it.
                    Offer o;
                    o.car = d;
                    o.colour = h.colour;
                    o.hidden = &h;
                    o.price = d->price;
                    o.priceNote = d->price ? "GT4's own price for this car (its car data)" : "GT4's car data gives this car no price";
                    for (const PrizeCar &p : cat_->prizes())
                        if (p.code == d->code)
                        {
                            o.given = givenFor(p);
                            o.guide = p.given.empty() ? std::string() : guideKey(p.given.front());
                        }
                    o.where = o.given.empty() ? std::string("GT4 sells it at none of its dealers, and it is not one of GT4's prizes.")
                                              : "GT4 sells it at none of its dealers; it is GT4's prize for: " + o.given + ".";
                    offers.push_back(o);
                }
                else if (d)
                {
                    Offer o;
                    o.car = d;
                    o.colour = h.colour;
                    o.black = true;
                    o.blackGrey = h.blackGrey;
                    o.hidden = &h;
                    const auto [lo, hi] = usedMileageRangeKm(d->year);
                    o.price = usedPrice(d->price, static_cast<uint64_t>(lo));
                    o.priceNote = strf("GT4's used price at %s km (its lot gives a '%02d car %s-%s km; new: Cr. %s)", groupedNumber(static_cast<uint64_t>(lo)).c_str(),
                                       d->year % 100, groupedNumber(static_cast<uint64_t>(lo)).c_str(), groupedNumber(static_cast<uint64_t>(hi)).c_str(),
                                       groupedNumber(d->price).c_str());
                    const bool lot = cat_->usedLotHas(h.week, h.label);
                    o.where = lot ? strf("GT4's used car dealer, in its week %d list only: Days %d-%d (GT4 has 100 weekly lists, so it should come back "
                                         "every 700 days)",
                                         h.week, h.week * 7 + 1, h.week * 7 + 7)
                                  : std::string("GT4's used car dealer, on rare days");
                    offers.push_back(o);
                }
        }
        else if (dealerSection_ == 2)
        {
            // Extra Cars (analysis/jp-cars): the cars the pack adds from the developer's own Japanese disc, read through the same pack
            // GT4.exe serves (the car data has them only while the pack is next to GT4.exe and checks out against the disc).
            for (const char *label : kExtraCars)
                if (const DiscCar *d = cat_->carByLabel(label))
                {
                    Offer o;
                    o.car = d;
                    o.extra = true;
                    o.price = d->price;
                    o.priceNote = d->price ? "The Japanese disc's price for this car (its car data), new" : "The car data gives this car no price";
                    o.where = "GT4's Toyota dealer sells it new, and Arcade lists it under Toyota once it is in your garage. From your own "
                              "Japanese GT4 disc (extra-cars\\esso.gt4x next to GT4.exe): car 229, its Japanese specifications, the US disc's own "
                              "model of it (toyt0035).";
                    offers.push_back(o);
                }
        }
        else
        {
            // Every car of GT4's prize table (PresentData: event wins, licences, missions, completion), in the table's order
            // (developer, 2026-10-08: the list complete, his rare race cars among them). The makers' new-car lists hold many cars
            // GT4's dealers never show (its race cars), so they do not decide what is listed here.
            for (const PrizeCar &p : cat_->prizes())
                if (const DiscCar *d = cat_->car(p.code))
                {
                    Offer o;
                    o.car = d;
                    o.price = d->price;
                    o.priceNote = d->price ? "GT4's own price for this car (its car data)" : "GT4's car data gives this car no price";
                    o.given = givenFor(p);
                    o.guide = p.given.empty() ? std::string() : guideKey(p.given.front());
                    o.where = "GT4's prize for: " + o.given + "." +
                              (p.soldUsed ? std::string(" GT4's used car dealer also sells it in some weeks.") : std::string());
                    offers.push_back(o);
                }
        }
        // Section chips, maker chips, credits.
        float cx = x0;
        fullui::caps(ui_, x0, y0, "Section", t.faint);
        float wch = 0;
        if (fullui::chipButton(ui_, cx, y0 + 18, "Hidden Cars", dealerSection_ == 0, true, wch))
        {
            dealerSection_ = 0;
            dealerMaker_ = 0;
            dealerSel_.clear();
        }
        cx += wch + 8;
        if (fullui::chipButton(ui_, cx, y0 + 18, "Prize Cars", dealerSection_ == 1, true, wch))
        {
            dealerSection_ = 1;
            dealerMaker_ = 0;
            dealerSel_.clear();
        }
        cx += wch + 8;
        // Extra Cars (analysis/jp-cars): only while the car data has one (the pack next to GT4.exe; no switch).
        if (hasExtra && fullui::chipButton(ui_, cx, y0 + 18, "Extra Cars", dealerSection_ == 2, true, wch))
        {
            dealerSection_ = 2;
            dealerMaker_ = 0;
            dealerSel_.clear();
        }
        std::vector<std::string> makers{"All"};
        for (const Offer &o : offers)
            if (std::find(makers.begin(), makers.end(), o.car->maker) == makers.end())
                makers.push_back(o.car->maker);
        dealerMaker_ = std::clamp(dealerMaker_, 0, static_cast<int>(makers.size()) - 1);
        float my = y0 + 64;
        fullui::caps(ui_, x0, my, "Maker", t.faint);
        cx = x0;
        // The maker chips (a few makers), or a drop-down list when there are many (Prize Cars: dozens of makers).
        float chipY = my + 18;
        if (makers.size() > 12)
        {
            std::vector<std::string> labels = makers;
            labels[0] = strf("All makers (%zu)", makers.size() - 1);
            int sel = dealerMaker_;
            if (ui_.dropdown("dealer-maker", R(x0, my + 18, 260, 32), labels, sel, true))
            {
                dealerMaker_ = sel;
                dealerSel_.clear();
            }
        }
        else
            for (size_t i = 0; i < makers.size(); ++i)
            {
                if (cx > x0 + cw * 0.62f)
                {
                    cx = x0;
                    chipY += 36;
                }
                if (fullui::chipButton(ui_, cx, chipY, makers[i], dealerMaker_ == static_cast<int>(i), true, wch, 12.5f))
                {
                    dealerMaker_ = static_cast<int>(i);
                    dealerSel_.clear();
                }
                cx += wch + 8;
            }
        const float chipRows = (chipY - (my + 18)) / 36.0f;
        if (garage_.loaded && garage_.hasCredits)
            ui_.text("Your credits  Cr. " + groupedNumber(garage_.credits), x1, my + 24, 13.5f, t.text, Weight::Semibold, Align::Right);
        std::vector<const Offer *> shown;
        for (const Offer &o : offers)
            if (dealerMaker_ == 0 || o.car->maker == makers[static_cast<size_t>(dealerMaker_)])
                shown.push_back(&o);
        if (!shown.empty() && (dealerSel_.empty() || std::none_of(shown.begin(), shown.end(), [&](const Offer *o) { return o->car->label == dealerSel_; })))
            dealerSel_ = shown.front()->car->label;
        // The cards (2 columns, scrolling) and the selected car.
        const float listTop = my + 64 + chipRows * 36.0f;
        const float panelW = std::floor(std::min(380.0f, cw * 0.33f));
        const float lx1 = x1 - panelW - kGap;
        fullui::caps(ui_, x0, listTop, strf("%s \xc2\xb7 %zu cars", dealerSection_ == 0 ? "Hidden cars" : dealerSection_ == 2 ? "Extra cars" : "Prize cars", shown.size()), t.faint);
        const Rectangle view = R(x0, listTop + 22, lx1 - x0 + 10, y1 - listTop - 22);
        const float off = ui_.beginScroll("dealer-list", view);
        const float cardW = std::floor((lx1 - x0 - 12) * 0.5f), picH = std::floor(cardW * 100.0f / 160.0f * 0.72f), cardH = picH + 118;
        float y = view.y - off;
        // Hidden Cars in two groups when there are several rare cars: GT4's black cars, then the rare cars (each group's cards
        // in 2 columns). With one rare car (the Abt Audi TT-R) it is listed with the black cars.
        size_t rareShown = 0;
        for (const Offer *o : shown)
            rareShown += o->hidden && o->hidden->rare() ? 1 : 0;
        const bool grouped = dealerSection_ == 0 && rareShown > 1;
        size_t inGroup = 0;
        for (size_t i = 0; i < shown.size(); ++i)
        {
            const Offer &o = *shown[i];
            const bool rare = o.hidden && o.hidden->rare();
            if (grouped && (i == 0 || rare != (shown[i - 1]->hidden && shown[i - 1]->hidden->rare())))
            {
                if (i > 0)
                    y += ((inGroup + 1) / 2) * (cardH + 12) + 6;
                size_t n = 0;
                for (size_t k = i; k < shown.size() && (shown[k]->hidden && shown[k]->hidden->rare()) == rare; ++k)
                    ++n;
                fullui::caps(ui_, x0, y, strf("%s \xc2\xb7 %zu", rare ? "Rare cars: GT4 sells them at none of its dealers" : "Black cars: GT4's used car dealer, on rare days", n),
                             t.faint);
                y += 24;
                inGroup = 0;
            }
            const size_t gi = grouped ? inGroup++ : i;
            const float cxx = x0 + (gi % 2) * (cardW + 12), cyy = y + (gi / 2) * (cardH + 12);
            const Rectangle c = R(cxx, cyy, cardW, cardH);
            const bool sel = o.car->label == dealerSel_;
            ui_.round(c, 10, t.panel);
            ui_.roundLine(c, 10, sel ? 1.6f : 1.0f, sel ? t.accent : ui::alpha(t.border, 0.8f));
            if (ui_.clicked(c))
                dealerSel_ = o.car->label;
            drawCarPicture(R(c.x + 1, c.y + 1, c.width - 2, picH), o.car->label, false, o.blackGrey);
            float yy = c.y + picH + 12;
            fullui::caps(ui_, c.x + 14, yy, o.car->maker, t.accent);
            yy += 20;
            ui_.text(ui_.fit(o.car->name, c.width - 28, 14, Weight::Semibold), c.x + 14, yy, 14, t.text, Weight::Semibold);
            yy += 26;
            ui_.text(o.black ? "Black" : rare ? "Rare car" : o.extra ? "Extra car" : "Prize car", c.x + 14, yy, 12.5f, t.muted);
            ui_.text(o.extra ? std::string("Toyota dealer, Japanese disc") : o.black ? (o.hidden && o.hidden->given() ? "Given by GT4" : "Rare used car") : rare && o.given.empty() ? std::string("Not a prize in GT4") : ui_.fit(o.given, c.width - 120, 12), c.x + c.width - 14, yy, 12,
                     t.muted, Weight::Regular, Align::Right);
            yy += 24;
            ui_.text(o.price ? "Cr. " + groupedNumber(o.price) : std::string("No price in GT4's data"), c.x + 14, yy, 15, t.text, Weight::Semibold);
        }
        y += (((grouped ? inGroup : shown.size()) + 1) / 2) * (cardH + 12);
        ui_.endScroll("dealer-list", y + off - view.y);
        // Selected car
        const Offer *s = nullptr;
        for (const Offer *o : shown)
            if (o->car->label == dealerSel_)
                s = o;
        const Rectangle p = R(x1 - panelW, listTop, panelW, y1 - listTop);
        ui_.round(p, 12, t.panel);
        ui_.roundLine(p, 12, 1, ui::alpha(t.border, 0.8f));
        if (!s)
            return;
        const float pw = p.width - 32;
        // Buy it (credits) or add it free: the actions stay in view at the panel's foot (the details above scroll); the
        // confirmation lists exactly what changes (gt4full_saveedit.h).
        const bool pay = f.dealerPay && s->price > 0;
        // The colour (developer, 2026-10-11): any of the car's own GT4 colours; a black car's black first.
        if (dealerColourFor_ != s->car->label)
        {
            dealerColourFor_ = s->car->label;
            dealerColour_ = s->colour;
        }
        const int colourCount = names_ ? names_->colourCount(s->car->code) : 0;
        if (colourCount > 0 && (dealerColour_ < 0 || dealerColour_ >= colourCount))
            dealerColour_ = s->colour;
        const GarageEdit e = withColour(s->hidden ? hiddenCarEdit(*cat_, *s->hidden, pay) : prizeCarEdit(*s->car, pay), dealerColour_, s->hidden);
        const int previewGrey = blackCarGrey(s->car->label, dealerColour_);
        std::string why = garageEditBlock();
        if (why.empty() && !garage_.loaded)
            why = "No GT4 save on this memory card to add the car to.";
        if (why.empty() && pay && garage_.credits < e.price)
            why = "Not enough credits: it costs Cr. " + groupedNumber(e.price) + " and you have Cr. " + groupedNumber(garage_.credits) +
                  ". Untick \"Pay with my credits\" to add it free.";
        const std::string footNote =
            why.empty() ? std::string("You see exactly what changes before your save is touched. GT4 must be closed; the card is backed up first.") : why;
        const float footH = std::floor(14 + 30 + 42 + ui_.wrapHeight(footNote, pw, 12) + 14);
        const Rectangle pv = R(p.x + 2, p.y + 2, p.width - 4, p.height - 4 - footH);
        const float poff = ui_.beginScroll("dealer-car", pv);
        float yy = p.y + 14 - poff;
        fullui::caps(ui_, p.x + 16, yy, "Selected car", t.faint);
        yy += 22;
        const float ph = std::floor(pw * 100.0f / 160.0f * 0.62f);
        drawCarPicture(R(p.x + 16, yy, pw, ph), s->car->label, true, previewGrey);
        yy += ph + 10;
        fullui::caps(ui_, p.x + 16, yy, s->car->maker, t.accent);
        yy += 18;
        yy += ui_.wrap(s->car->name, p.x + 16, yy, pw, 18, t.text, Weight::Bold) + 2;
        yy += ui_.wrap(s->black && blackCarPictureResource(s->car->label) ? "Original GT4 car, in its special black colour (the picture is GT4's own 3D "
                                                                            "render of it in black, from GT4's garage)"
                       : s->black && s->hidden && s->hidden->given()
                           ? "Original GT4 car, in its black colour (GT4's disc has no picture under this car's name: a plain outline is shown)"
                       : s->black ? "Original GT4 car, in its special black colour (GT4 has one picture per car, in its usual colours: the launcher shows it "
                                    "darkened to the black)"
                       : prizeCarPictureResource(s->car->label)
                           ? "Original GT4 car. Added here it comes in the colour chosen below, and GT4 does not count it as a prize won. GT4 has "
                             "no dealer picture of this car: the picture is GT4's own 3D render of it from GT4's garage, in its first colour."
                       : s->car->label == "acura_dnx_02" // pictureName: GT4's picture of the Honda Dualnote (analysis/design/prize-cars)
                           ? "Original GT4 car. Added here it comes in the colour chosen below, and GT4 does not count it as a prize won. GT4 has "
                             "no picture under the DN-X's own name: the picture is GT4's picture of the Honda Dualnote, the same concept car, in "
                             "silver (not the DN-X's first colour)."
                       : s->extra ? "A car from your own Japanese GT4 disc (extra-cars\\esso.gt4x next to GT4.exe). Keep the pack there while it is in "
                                    "your garage: GT4 without the pack does not know the car."
                                  : "Original GT4 car. Added here it comes in the colour chosen below, and GT4 does not count it as a prize won. The "
                                    "picture is GT4's one picture of the car, in its first colour.",
                       p.x + 16, yy, pw, 11.5f, t.muted) +
              6;
        {
            // The car's own GT4 colours (CAR_VARIATION_american -> VARIATIONamerican), two to a row; the chosen one ringed.
            fullui::caps(ui_, p.x + 16, yy, "Colour", t.faint);
            ui_.text(dealerColourName(names_.get(), s->car->code, dealerColour_, s->black && dealerColour_ == s->colour), p.x + 16 + pw, yy - 1, 12, t.text,
                     Weight::Semibold, Align::Right);
            yy += 22;
            const float colW = std::floor((pw - 8) * 0.5f);
            int swatches = 0;
            for (int i = 0; i < colourCount; ++i)
            {
                const auto c = names_->colour(s->car->code, i);
                if (!c)
                    continue;
                const bool isBlack = s->black && i == s->colour;
                const Rectangle cr = R(p.x + 16 + (swatches % 2) * (colW + 8), yy + (swatches / 2) * 26, colW, 24);
                const bool sel = i == dealerColour_;
                if (sel)
                    ui_.round(cr, 6, ui::alpha(t.accent, 0.14f));
                ui_.circle(cr.x + 12, cr.y + 12, 7, Color{c->r, c->g, c->b, 255});
                ui_.ring(cr.x + 12, cr.y + 12, 7, sel ? 8.5f : 7.6f, sel ? t.accent : t.border);
                ui_.text(ui_.fit(dealerColourName(names_.get(), s->car->code, i, isBlack), colW - 30, 12, sel ? Weight::Semibold : Weight::Regular), cr.x + 26, cr.y + 5, 12,
                         sel ? t.text : t.muted, sel ? Weight::Semibold : Weight::Regular);
                if (ui_.clicked(cr))
                    dealerColour_ = i;
                ++swatches;
            }
            if (swatches == 0)
                ui_.text(s->black ? "Black (GT4's colour list could not be read)" : "GT4's first colour (its colour list could not be read)", p.x + 16, yy, 12, t.muted);
            yy += swatches ? ((swatches + 1) / 2) * 26 + 8 : 24;
        }
        auto row = [&](const std::string &k, const std::string &v) {
            fullui::specRow(ui_, p.x + 16, yy, pw, k, v, t.text, 12.5f);
            yy += 27;
        };
        row("Power / torque", (s->car->powerPs ? strf("%d PS", s->car->powerPs) : std::string("-")) + " / " + torque(s->car->torqueX100, f.torqueUnit));
        row("Weight / drivetrain", (s->car->weightKg ? groupedNumber(static_cast<uint64_t>(s->car->weightKg)) + " kg" : std::string("-")) + " / " + drivetrainName(s->car->drivetrain));
        if (s->car->frontWeightPct > 0)
            row("Weight balance (front : rear)", weightBalanceText(s->car->frontWeightPct));
        if (s->car->front.ok())
            row("Tyres", tyreText(s->car->front) + " / " + tyreText(s->car->rear));
        yy += 2;
        yy += ui_.wrap(s->where, p.x + 16, yy, pw, 12, t.muted) + 8;
        if (!s->guide.empty())
        {
            // The link to the event (licence, missions) in GT Mode prizes, with everything else it gives.
            if (ui_.button("dealer-guide", R(p.x + 16, yy, pw, 30), "Show it in GT Mode prizes", Style::Secondary, true, 12.5f))
            {
                garageTab_ = 3;
                prizeFocus_ = s->guide;
                prizeFocusShown_ = false;
            }
            yy += 40;
        }
        ui_.text(s->black ? "Dealer price" : "Price", p.x + 16, yy, 12.5f, t.muted);
        yy += 18;
        ui_.text(s->price ? "Cr. " + groupedNumber(s->price) : std::string("-"), p.x + 16, yy, 24, t.text, Weight::Bold);
        yy += 34;
        yy += ui_.wrap(s->priceNote, p.x + 16, yy, pw, 11.5f, t.faint) + 8;
        ui_.endScroll("dealer-car", yy + poff - p.y + 14);
        {
            float fy = p.y + p.height - footH;
            ui_.fill(R(p.x + 16, fy, pw, 1), ui::alpha(t.border, 0.8f));
            fy += 14;
            if (ui_.checkbox("dealer-pay", p.x + 16, fy, f.dealerPay, "Pay with my credits", s->price > 0))
                markDirty();
            fy += 30;
            const float bw = std::floor((pw - 8) * 0.5f);
            if (ui_.iconButton("dealer-review", R(p.x + 16, fy, bw, 34), Icon::Garage, pay ? "Review purchase" : "Add to garage", Style::Primary, why.empty(),
                               12.5f))
                openGarageEdit(e);
            if (ui_.button("dealer-specs", R(p.x + 16 + bw + 8, fy, bw, 34), "Specifications", Style::Secondary, true, 12.5f))
            {
                specsLabel_ = s->car->label;
                modal_ = Modal::CarSpecs;
            }
            fy += 42;
            ui_.wrap(footNote, p.x + 16, fy, pw, 12, t.faint);
        }
    }

    // ------------------------------------------------------------------ GT Mode prizes
    void App::garagePrizes(float x0, float y0, float x1, float y1)
    {
        const ui::Theme &t = ui::theme();
        const float cw = x1 - x0;
        if (!cat_)
        {
            ui_.wrap(catLoading_ ? std::string("Reading GT4's prize table from the disc...")
                     : !disc_.exists ? std::string("GT Mode prizes reads GT4's prize table from your disc image: choose it on the Play page.")
                                     : "GT4's prize table could not be read: " + catError_,
                     x0, y0 + 10, cw, 14, t.muted);
            return;
        }
        // The guide (GT4's prize table, its event list and names, and the save's results when a save is read), made again
        // when the car data, the names or the save change.
        const std::string key = strf("%p|%p|", static_cast<const void *>(cat_.get()), static_cast<const void *>(names_.get())) + garageKey_;
        if (key != guideKey_)
        {
            guideKey_ = key;
            guide_ = buildPrizeGuide(*cat_, names_.get(), &progress_);
        }
        if (guide_.empty())
            return;
        // A link from Prize Cars: the group that holds the entry.
        if (!prizeFocus_.empty())
            for (size_t g = 0; g < guide_.size(); ++g)
                for (const GuideEntry &e : guide_[g].entries)
                    if (e.key == prizeFocus_)
                        prizeGroup_ = static_cast<int>(g);
        prizeGroup_ = std::clamp(prizeGroup_, 0, static_cast<int>(guide_.size()) - 1);
        // The group chips (GT4's map names; short forms for the launcher's own long ones).
        fullui::caps(ui_, x0, y0, "GT Mode", t.faint);
        float cx = x0, cy = y0 + 18, wch = 0;
        for (size_t g = 0; g < guide_.size(); ++g)
        {
            const GuideGroup &gr = guide_[g];
            const std::string label = gr.key == "maker" ? std::string("Car-maker events") : gr.key == "weeks" ? std::string("New courses") : gr.title;
            const float w = ui_.textWidth(label, 13.0f) + 34;
            if (cx > x0 && cx + w > x1)
            {
                cx = x0;
                cy += 36;
            }
            if (fullui::chipButton(ui_, cx, cy, label, prizeGroup_ == static_cast<int>(g), true, wch))
            {
                prizeGroup_ = static_cast<int>(g);
                prizeFocus_.clear();
                ui_.resetScroll("prize-list");
            }
            cx += wch + 8;
        }
        const GuideGroup &gr = guide_[static_cast<size_t>(prizeGroup_)];
        float y = cy + 50;
        // The group's title, GT4's note for it and a count.
        size_t cars = 0, done = 0, known = 0;
        for (const GuideEntry &e : gr.entries)
        {
            for (const GuidePrize &p : e.prizes)
                cars += p.kind == "car";
            known += e.state >= 0;
            done += e.state == 2;
        }
        ui_.text(gr.title, x0, y, 19, t.text, Weight::Bold);
        std::string count = strf("%zu %s \xc2\xb7 %zu prize car%s", gr.entries.size(), gr.key == "weeks" ? "courses" : "entries", cars, cars == 1 ? "" : "s");
        if (known)
            count += strf(" \xc2\xb7 %zu done in your save", done);
        ui_.text(count, x1, y + 4, 13, t.muted, Weight::Regular, Align::Right);
        y += 30;
        if (!gr.note.empty())
            y += ui_.wrap(gr.note, x0, y, cw * 0.7f, 12.5f, t.muted) + 4;
        if (!known && gr.key != "achievement")
            y += ui_.wrap(garage_.loaded ? std::string("Your save has no results for these yet.")
                                         : std::string("Read a GT4 save (Saves) to see which of these you have already won."),
                          x0, y, cw * 0.7f, 12, t.faint) +
                 2;
        y += 10;
        // The list: one row per entry; the prizes on the right (a car's picture, name and a link to it in Prize Cars).
        const float statusW = 170, nameW = std::floor(cw * 0.40f), px = x0 + nameW + 24, prizeW = x1 - statusW - 16 - px;
        const float picW = 96, picH = 60, prizeRowH = 68;
        const Rectangle view = R(x0, y, cw + 10, y1 - y);
        const float off = ui_.beginScroll("prize-list", view);
        float ry = view.y - off;
        float focusY = -1;
        for (const GuideEntry &e : gr.entries)
        {
            const float nameH = 24 + (e.sub.empty() ? 0 : 20) + ui_.wrapHeight(e.how, nameW - 28, 12);
            const float rowH = std::max({nameH + 24, static_cast<float>(e.prizes.size()) * prizeRowH + 16, 84.0f});
            const Rectangle c = R(x0, ry, cw, rowH);
            const bool focus = !prizeFocus_.empty() && e.key == prizeFocus_;
            if (focus)
                focusY = ry + off - view.y;
            if (ry + rowH >= view.y && ry <= view.y + view.height)
            {
                ui_.round(c, 10, t.panel);
                ui_.roundLine(c, 10, focus ? 1.6f : 1.0f, focus ? t.accent : ui::alpha(t.border, 0.8f));
                float ty = c.y + 14;
                float nw = ui_.text(ui_.fit(e.name, nameW - 28, 15, Weight::Semibold), c.x + 14, ty, 15, t.text, Weight::Semibold);
                if (!e.gt4Name && e.key.rfind("win:", 0) == 0)
                    ui_.text("(no GT4 name: made from its key)", c.x + 14 + nw + 10, ty + 2, 11.5f, t.warn);
                ty += 24;
                if (!e.sub.empty())
                {
                    ui_.text(ui_.fit(e.sub, nameW - 28, 12.5f), c.x + 14, ty, 12.5f, t.accent);
                    ty += 20;
                }
                ui_.wrap(e.how, c.x + 14, ty, nameW - 28, 12, t.muted);
                // The prizes.
                float py = c.y + 8;
                if (e.prizes.empty())
                    ui_.text("No prize in GT4's prize table", px, c.y + rowH * 0.5f - 8, 13, t.faint);
                for (const GuidePrize &p : e.prizes)
                {
                    const Rectangle pic = R(px, py, picW, picH);
                    if (p.kind == "car")
                    {
                        drawCarPicture(pic, p.label, false, p.colour >= 0 ? blackCarGrey(p.label, p.colour) : 0); // GT4's black: the black car's picture
                        if (ui_.hovered(R(px, py, prizeW, picH)))
                            ui_.roundLine(pic, 8, 1.2f, t.accent);
                        if (ui_.clicked(R(px, py, prizeW, picH)))
                        {
                            garageTab_ = 2;
                            dealerSection_ = 1;
                            dealerMaker_ = 0;
                            dealerSel_ = p.label;
                            ui_.resetScroll("dealer-list");
                        }
                    }
                    else
                    {
                        ui_.round(pic, 8, t.panel2);
                        ui_.icon(p.kind == "course" ? Icon::Play : Icon::Card, R(px + picW * 0.5f - 14, py + picH * 0.5f - 14, 28, 28), t.muted);
                    }
                    const float tx = px + picW + 14, tw = prizeW - picW - 14;
                    fullui::caps(ui_, tx, py + 4, p.kind == "car" ? "Prize car" : p.kind == "course" ? "Course" : "Paper", t.accent);
                    ui_.text(ui_.fit(p.name, tw, 14, Weight::Semibold), tx, py + 22, 14, t.text, Weight::Semibold);
                    const std::string note = p.kind == "car" ? (p.note.empty() ? std::string("Show it in Car Dealers > Prize Cars") : p.note + " \xc2\xb7 Show it in Prize Cars")
                                                             : p.note;
                    if (!note.empty())
                        ui_.text(ui_.fit(note, tw, 12), tx, py + 42, 12, p.kind == "car" ? t.faint : t.muted);
                    py += prizeRowH;
                }
                // The save's result.
                if (e.state >= 0)
                {
                    const Color col = e.state == 2 ? t.ok : e.state == 1 ? t.warn : t.faint;
                    const float sw = std::min(statusW - 10, ui_.textWidth(e.status, 12.5f) + 26);
                    const Rectangle pill = R(x1 - 14 - sw, c.y + 14, sw, 26);
                    ui_.round(pill, 13, ui::alpha(col, 0.16f));
                    ui_.text(e.status, pill.x + sw * 0.5f, pill.y + 5, 12.5f, col, Weight::Semibold, Align::Center);
                }
            }
            ry += rowH + 10;
        }
        ui_.endScroll("prize-list", ry + off - view.y);
        if (focusY >= 0 && !prizeFocusShown_)
        {
            ui_.scrollTo("prize-list", std::max(0.0f, focusY - 12));
            prizeFocusShown_ = true;
        }
    }
}
