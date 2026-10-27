// GT4 Recompilation launcher: the Save details view's model (gt4stock_savedetails.h).

#include "gt4stock_savedetails.h"

#include "gt4save.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace gt4stock
{
    namespace
    {
        std::string seriesOf(const std::string &raceLabel)
        {
            // "am_sunday_0003" -> "am_sunday"; "en_nur24h" stays.
            const size_t u = raceLabel.rfind('_');
            if (u == std::string::npos || u + 1 >= raceLabel.size())
                return raceLabel;
            for (size_t i = u + 1; i < raceLabel.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(raceLabel[i])))
                    return raceLabel;
            return raceLabel.substr(0, u);
        }

        // GT4's event halls (GTmodeProject::EventHallRoot keys) by the race code's prefix.
        std::string hallKey(const std::string &series)
        {
            static const std::pair<const char *, const char *> halls[] = {
                {"am_", "BeginnerHall"}, {"pr_", "ProfessionalHall"}, {"ex_", "ExtremeHall"}, {"en_", "EnduranceHall"},
                {"rh_", "RallyHall"},    {"jpr", "RaceJapanese"},     {"usr", "RaceAmerican"}, {"eur", "RaceEuropean"}};
            for (const auto &[pre, key] : halls)
                if (startsWith(series, pre))
                    return key;
            return std::string();
        }
        int hallOrder(const std::string &key)
        {
            static const char *const order[] = {"BeginnerHall", "ProfessionalHall", "ExtremeHall", "EnduranceHall", "RaceJapanese",
                                                "RaceAmerican", "RaceEuropean", "RallyHall"};
            for (int i = 0; i < 8; ++i)
                if (key == order[i])
                    return i;
            return 8; // the makers' events
        }
        // A maker's events ("jp_nissan_gtr" -> "Nissan Race Events", GT4's INFO|Race_nissan text).
        std::string makerHall(const GameNames &n, const std::string &series)
        {
            const std::vector<std::string> parts = split(series, '_');
            if (parts.size() < 2 || parts[0].size() != 2)
                return std::string();
            static const std::pair<const char *, const char *> alias[] = {
                {"vw", "volkswagen"}, {"chev", "chevrolet"}, {"chry", "chrysler"}, {"alfa", "alfaromeo"}, {"aston", "astonmartin"},
                {"bmw1", "bmw"},      {"mg", "mg_mini"},      {"mercedes", "mercedes"}};
            std::string maker = parts[1];
            for (const auto &[a, b] : alias)
                if (maker == a)
                    maker = b;
            return n.text("GTmodeProject::EventHallRoot", "INFO|Race_" + maker);
        }

        std::string thousands(double v) { return groupedNumber(static_cast<uint64_t>(std::llround(std::max(0.0, v)))); }

        std::string ratio(uint32_t x1000) { return strf("%.3f", x1000 / 1000.0); }

    }

    std::string plainCheck(const std::string &m)
    {
        std::string s = m;
        for (const char *advice : {"; run gt4save.py", ": leave it alone"})
        {
            const size_t r = s.find(advice);
            if (r != std::string::npos)
                s = s.substr(0, r);
        }
        // (1.4 review) The library counts days elapsed (0 on a new game); the page counts as GT4's Days counter does
        // (elapsed + 1), so a car shown as "day 8" is never "added on day 7" in a note. Its three day messages
        // (savelib gt4save.cpp checks), reworded with GT4's count:
        size_t at = 0;
        auto word = [&](std::string_view w) {
            if (s.compare(at, w.size(), w) != 0)
                return false;
            at += w.size();
            return true;
        };
        auto number = [&](long long &v) {
            const size_t start = at;
            if (at < s.size() && s[at] == '-')
                ++at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9')
                ++at;
            return parseInt(std::string_view(s).substr(start, at - start), v);
        };
        long long slot = 0, added = 0, today = 0, events = 0;
        if (word("garage entry ") && number(slot) && word(" added on day ") && number(added) && word(", after today (") && number(today) && word(")") &&
            at == s.size())
            return strf("garage entry %lld added on Day %lld, after today (Day %lld on GT4's counter)", slot, added + 1, today + 1);
        at = 0;
        if (word("calendar holds ") && number(events) && word(" days of events but only ") && number(today) && word(" days have passed") && at == s.size())
            return s + strf(" (today is Day %lld on GT4's counter)", today + 1);
        at = 0;
        if (word("today is before 2005-04-02 (day ") && number(today) && word(")") && at == s.size())
            return strf("today is before 2 Apr 2005 (Day %lld on GT4's counter; a new game starts at Day 1)", today + 1);
        return s;
    }

    std::string dateText(const std::string &iso)
    {
        static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        long long y = 0, m = 0, d = 0;
        if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-' || !parseInt(iso.substr(0, 4), y) || !parseInt(iso.substr(5, 2), m) ||
            !parseInt(iso.substr(8, 2), d) || m < 1 || m > 12)
            return iso;
        return strf("%lld %s %lld", d, months[m - 1], y);
    }

    std::string loadStateText(int s)
    {
        return s == 0 ? "GT4 loads this save" : s == 1 ? "GT4 loads this save (see the note)" : "GT4 will not load this save";
    }

    SaveDetails buildDetails(const gt4save::Save &s, const GameNames *names)
    {
        SaveDetails out;
        out.source = s.source;
        out.names = names != nullptr;
        if (!s.loaded)
        {
            DetailProblem p;
            p.level = 2;
            const std::string &e = s.error;
            if (startsWith(e, "no GT4 save data"))
            {
                p.title = "No game data in this save";
                p.text = "The GT4 save folder is there, but its main file is missing, so GT4 sees no save on this card.";
            }
            else if (e.find("is not a GT4 save file") != std::string::npos)
            {
                p.title = "This save cannot be read";
                p.text = "The GT4 save file on this card is not one GT4 can read: it may be damaged or cut short, so GT4 will not load it. "
                         "Backups made before then are not affected.";
            }
            else if (startsWith(e, "cannot read the garage file"))
            {
                p.title = "The garage file cannot be read";
                p.text = "Windows could not read the save's garage file (" + e.substr(std::min(e.size(), size_t(29))) + ").";
            }
            else
            {
                p.title = "This save cannot be read";
                p.text = "The launcher could not read it: " + e + ".";
            }
            out.problems.push_back(p);
            out.loadState = 2;
            return out;
        }
        out.loaded = true;
        const gt4save::Fields &f = s.fields;
        out.player = s.playerName();
        out.days = s.days();
        out.gameDay = out.days + 1; // GT4's Days counter (CHEATS-TAB-DESIGN.md 3.3)
        out.date = s.gamedata.calendarDate ? dateText(*s.gamedata.calendarDate) : std::string();
        if (const std::optional<uint64_t> c = s.credits())
        {
            out.hasCredits = true;
            out.credits = *c;
        }
        out.aspecPoints = s.aspecPoints();
        out.races = f.profile.racesTotal;
        out.wins = f.profile.wins;
        out.prizeMoney = f.profile.totalPrizeMoney;
        out.prizeCars = f.profile.totalPrizeCars;
        out.distanceM = f.profile.aspecDistance;

        // ---- what GT4 makes of it, in a player's words
        const gt4save::GamedataInfo &g = s.gamedata;
        const bool crcOk = g.outerCrcOk && g.inner.magicOk && g.inner.lenOk && g.inner.crcOk;
        const bool refused = !s.gt4LoadChecksPass() || g.copies > 1 || !crcOk;
        out.appendBug = s.appendBugHit();
        if (g.copies > 1)
            out.problems.push_back({2, strf("%u saves in one file", g.copies),
                                    strf("The save file holds %u saves one after the other: an earlier GT4 Recompilation build added each new save to "
                                         "the end of the file instead of replacing it (a memory-card bug, since fixed). GT4 reads the whole file, so it "
                                         "reports this save as damaged and will not load it. The newest of the %u is shown here. Backups made before "
                                         "the second save are not affected.",
                                         g.copies, g.copies)});
        if (!crcOk)
            out.problems.push_back({2, "The save data is damaged",
                                    "Its built-in check does not match, so GT4 will not load this save (it reports the data as corrupted). "
                                    "Backups made before the damage are not affected."});
        if (!s.garage)
            out.problems.push_back({2, "The garage file is missing", "GT4 needs it next to the game data, so it will not load this save."});
        else
        {
            const gt4save::GarageInfo &ga = *s.garage;
            if (!ga.validForGamedata)
                out.problems.push_back({2, "The garage does not belong to this save",
                                        "The garage file was written with a different save (GT4 treats it as another memory card's), so GT4 will "
                                        "not load this save."});
            if (ga.size != ga.expectedSize || !ga.trailing.empty())
                out.problems.push_back(
                    {1, ga.size > ga.expectedSize ? "Extra data at the end of the garage file" : "The garage file is short",
                     ga.size < ga.expectedSize ? "The garage file is shorter than GT4 writes it: the cars stored at its end are missing."
                     : refused ? "The garage file also has part of a later save at its end, from the same memory-card bug."
                               : "An earlier GT4 Recompilation build wrote part of a later save to the end of the garage file instead of its start "
                                 "(a memory-card bug, since fixed). GT4 still loads this save, but it reads the start of the file, so the garage "
                                 "and credits it shows may be from before that save."});
            if (ga.header.money > 99999999u)
                out.problems.push_back({1, "Credits above GT4's limit", "GT4 itself never goes above 99,999,999 Cr."});
        }
        // The other findings: the field checks (library wording, tool advice removed).
        bool fieldErrors = false;
        for (const gt4save::Check &c : s.checks)
        {
            const std::string &m = c.message;
            const bool covered = startsWith(m, "GAMEDATA envelope CRC") || startsWith(m, "GAMEDATA inner header") || startsWith(m, "GAMEDATA holds") ||
                                 m == "no garage file" || startsWith(m, "garage header does not match") || startsWith(m, "garage file is") ||
                                 startsWith(m, "credits ");
            if (covered)
                continue;
            fieldErrors = fieldErrors || c.level == gt4save::Level::Error;
            out.technical.push_back(std::string(c.level == gt4save::Level::Error ? "Unusual: " : c.level == gt4save::Level::Warn ? "Note: " : "Info: ") +
                                    plainCheck(m));
        }
        if (fieldErrors)
            out.problems.push_back({1, "Unusual values", "Some of the save's values are outside what GT4 itself writes (see the technical notes). "
                                                         "GT4 may not behave normally with them."});
        out.loadState = refused ? 2 : out.problems.empty() ? 0 : 1;
        std::stable_sort(out.problems.begin(), out.problems.end(), [](const DetailProblem &a, const DetailProblem &b) { return a.level > b.level; });

        // ---- the calendar: how each car came, and the prize cars
        std::map<std::pair<uint64_t, int64_t>, uint8_t> gotHow; // (code, day) -> 1 new, 2 used, 3 trade, 4 present
        for (const gt4save::DayEvent &e : f.calendar.events)
            if (e.type == 1 && e.code)
            {
                gotHow[{*e.code, e.day}] = e.how();
                if (e.how() == 4)
                {
                    const std::string n = names ? names->carName(*e.code) : std::string();
                    out.prizeList.push_back((n.empty() ? strf("Car #%llu", static_cast<unsigned long long>(*e.code & 0xFFFFFFFFu)) : n) +
                                            strf(" (day %lld)", static_cast<long long>(e.day) + 1));
                }
            }

        // ---- the garage
        const gt4save::SpecDb *spec = names ? names->spec.get() : nullptr;
        for (const gt4save::GarageEntry &e : f.garageIndex)
        {
            if (!e.inGarage || !e.code)
                continue;
            DetailCar c;
            c.slot = e.slot;
            c.code = *e.code;
            c.riding = e.slot == f.garageFields.ridingSlot;
            const gt4save::Car *full = c.riding ? &f.currentCar : (f.slotCars.count(e.slot) ? &f.slotCars.at(e.slot) : nullptr);
            c.fullRecord = full != nullptr;
            if (names)
            {
                c.name = names->carName(c.code);
                const std::string fn = names->carFullName(c.code);
                if (fn != c.name)
                    c.fullName = fn;
            }
            c.nameFromDisc = !c.name.empty();
            if (c.name.empty())
            {
                const std::optional<std::string> label = spec ? spec->label(c.code) : std::nullopt;
                c.name = label && !label->empty() && (*label)[0] != '#' ? *label : strf("Car #%llu", static_cast<unsigned long long>(c.code & 0xFFFFFFFFu));
            }
            c.colourIndex = static_cast<int>(e.colour);
            std::optional<uint64_t> variation;
            if (full)
            {
                variation = full->active().variation;
                c.colourIndex = full->active().colour;
            }
            if (names)
                if (const std::optional<CarColour> col = names->colour(c.code, c.colourIndex, variation))
                {
                    c.colourName = col->name;
                    c.hasRgb = true;
                    c.r = col->r;
                    c.g = col->g;
                    c.b = col->b;
                }
            if (c.colourName.empty())
                c.colourName = strf("Colour %d", c.colourIndex + 1);
            c.odometerM = full ? full->odometer : e.odometer;
            c.power = e.powerX100 / 100.0;
            c.weightKg = static_cast<int>(e.weight);
            c.dayAdded = static_cast<int>(e.dayAdded);
            const auto how = gotHow.find({c.code, static_cast<int64_t>(e.dayAdded)});
            if (how != gotHow.end())
            {
                static const char *const kHow[5] = {"Got", "Bought new", "Bought used", "Traded", "Prize car"};
                c.prize = how->second == 4;
                c.acquired = strf("%s, day %d", kHow[how->second <= 4 ? how->second : 0], c.dayAdded + 1);
            }
            else
                c.acquired = strf("Day %d", c.dayAdded + 1);

            if (full)
            {
                const gt4save::Block &b = full->active();
                std::string front, rear;
                for (uint64_t p : b.parts)
                {
                    const uint32_t table = static_cast<uint32_t>(p >> 32);
                    const std::string n = names ? names->partName(p) : std::string();
                    if (n.empty())
                        continue;
                    if (table == 25)
                        front = n;
                    else if (table == 26)
                        rear = n;
                    else if (table == 29)
                        c.parts.push_back("Wheels: " + n);
                    else
                        c.parts.push_back(n);
                }
                if (!front.empty() && front == rear)
                    c.parts.insert(c.parts.begin(), front + ", front and rear");
                else
                {
                    if (!rear.empty())
                        c.parts.insert(c.parts.begin(), "Rear: " + rear);
                    if (!front.empty())
                        c.parts.insert(c.parts.begin(), "Front: " + front);
                }
                // The tyres are always named (every tyre has a shop name, the standard ones too), so
                // a car with nothing else fitted lists its tyres and then this note.
                c.partsNote = !names ? "Part names come from your GT4 disc image." : c.parts.empty() ? "Standard parts." : "Everything else is standard.";

                // Settings as GT4 stores them; gear ratios and final drive in GT4's x1000 form.
                const gt4save::BlockSettings &st = b.settings;
                std::vector<std::string> gears;
                for (size_t i = 1; i < b.gearsX1000.size() && b.gearsX1000[i]; ++i)
                    gears.push_back(ratio(b.gearsX1000[i]));
                if (!gears.empty())
                    c.settings.push_back({"Gear ratios", join(gears, " / ") + (b.gearsX1000[0] ? "  (reverse " + ratio(b.gearsX1000[0]) + ")" : "")});
                if (st.finalGearX1000)
                    c.settings.push_back({"Final drive", ratio(st.finalGearX1000)});
                c.settings.push_back({"Ride height F / R", strf("%u / %u", st.rideHeightF, st.rideHeightR)});
                c.settings.push_back({"Springs F / R", strf("%u / %u", st.springF, st.springR)});
                c.settings.push_back({"Dampers", strf("%u %u %u %u / %u %u %u %u", st.damper[0], st.damper[1], st.damper[2], st.damper[3], st.damper[4],
                                                      st.damper[5], st.damper[6], st.damper[7])});
                c.settings.push_back({"Camber F / R", strf("%u / %u", st.camberF, st.camberR)});
                c.settings.push_back({"Toe F / R", strf("%u / %u", st.toeF, st.toeR)});
                c.settings.push_back({"Anti-roll bars F / R", strf("%u / %u", st.stabilizerF, st.stabilizerR)});
                c.settings.push_back({"LSD front", strf("%u / %u / %u", st.lsdF[0], st.lsdF[1], st.lsdF[2])});
                c.settings.push_back({"LSD rear", strf("%u / %u / %u", st.lsdR[0], st.lsdR[1], st.lsdR[2])});
                c.settings.push_back({"Traction control", strf("%u", st.tcs)});
                c.settings.push_back({"Brake balance F / R", strf("%u / %u", st.brakeCtrlF, st.brakeCtrlR)});
                c.settings.push_back({"Ballast", st.ballastWeight ? strf("%u, position %d", st.ballastWeight, st.ballastPosition) : std::string("none")});
                if (st.nos)
                    c.settings.push_back({"Nitrous", strf("%u", st.nos)});
            }
            else
                c.partsNote = "As it came: GT4 keeps a car's parts and settings once it has been driven.";
            out.cars.push_back(std::move(c));
        }
        std::stable_sort(out.cars.begin(), out.cars.end(), [](const DetailCar &a, const DetailCar &b) { return a.riding && !b.riding; });

        // ---- licences, coffee breaks, missions
        if (s.licences)
        {
            static const std::pair<const char *, const char *> keys[] = {{"B", "l0b0000"}, {"A", "l0a0000"}, {"IB", "lib0000"}, {"IA", "lia0000"}, {"S", "l0s0000"}};
            for (const gt4save::LicenceStatus::Licence &l : s.licences->licences)
            {
                DetailLicence d;
                d.key = l.name;
                for (const auto &[k, t] : keys)
                    if (l.name == k && names)
                        d.name = names->licenceText(t);
                if (d.name.empty())
                    d.name = l.name + " licence";
                for (size_t i = 0; i < 16; ++i)
                {
                    const std::string &r = l.tests[i];
                    d.marks[i] = r == "gold" ? 'G' : r == "silver" ? 'S' : r == "bronze" ? 'B' : r == "red" ? 'P' : '-';
                    d.done += d.marks[i] != '-';
                }
                d.passed = l.passed;
                d.allGold = l.allGold;
                d.gold = l.gold;
                d.silver = l.silver;
                d.bronze = l.bronze;
                out.licences.push_back(d);
            }
            for (const std::string &r : s.licences->coffeeBreaks)
                out.coffeeDone += r != "none";
            for (const std::string &r : s.licences->missions)
                out.missionsDone += r != "none";
        }

        // ---- events: the race record's event rows (type 2), by series
        if (spec && !spec->raceRows.empty())
        {
            std::map<std::string, DetailSeries> bySeries;
            std::map<std::string, int> totals;
            for (size_t row = 0; row < spec->raceRows.size(); ++row)
                if (const std::optional<std::string> l = spec->raceLabel(static_cast<int>(row)))
                    totals[seriesOf(*l)]++;
            for (const gt4save::RaceUnit &u : f.raceRecord)
            {
                if (u.typeRaw() != 2)
                    continue;
                const std::optional<std::string> l = spec->raceLabel(u.row);
                if (!l)
                    continue;
                const std::string se = seriesOf(*l);
                DetailSeries &d = bySeries[se];
                if (d.series.empty())
                {
                    d.series = se;
                    d.firstRow = u.row;
                    d.races = totals[se];
                    d.name = names ? names->eventName(se) : std::string();
                    if (d.name.empty())
                        d.name = se;
                    const std::string hk = hallKey(se);
                    d.hall = names ? (hk.empty() ? makerHall(*names, se) : names->text("GTmodeProject::EventHallRoot", hk)) : std::string();
                }
                d.firstRow = std::min(d.firstRow, u.row);
                if (u.best() <= 7)
                {
                    d.done++;
                    out.eventsDone++;
                }
                if (u.best() == 0)
                {
                    d.won++;
                    out.eventsWon++;
                }
                d.points += static_cast<int>(u.bestScore());
            }
            for (auto &[k, d] : bySeries)
                if (d.done > 0)
                    out.series.push_back(d);
            std::stable_sort(out.series.begin(), out.series.end(), [](const DetailSeries &a, const DetailSeries &b) {
                const int ha = hallOrder(hallKey(a.series)), hb = hallOrder(hallKey(b.series));
                return ha != hb ? ha < hb : a.firstRow < b.firstRow;
            });
        }
        if (f.championship.suspended && !f.championship.raceCode.empty())
        {
            const std::string se = seriesOf(f.championship.raceCode);
            const std::string n = names ? names->eventName(se) : std::string();
            out.championship = (n.empty() ? f.championship.raceCode : n) + ", saved during the championship";
        }
        return out;
    }
}
