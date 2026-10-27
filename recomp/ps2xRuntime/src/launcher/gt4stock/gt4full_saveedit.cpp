// GT4 Recompilation launcher, Full edition: the Garage's save-changing actions (gt4full_saveedit.h). No raylib, no window.

#include "gt4full_saveedit.h"

#include "gt4save.h"
#include "gt4save_edit.h"
#include "gt4save_roundtrip.h"
#include "gt4stock_saves.h"
#include "gt4stock_util.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace gt4stock
{
    namespace edit = gt4save::edit;

    namespace
    {
        constexpr const char *kChangeLog = "garage-changes.txt";
        constexpr const char *kStaging = "garage-staging";
        constexpr const char *kPending = "garage-change-pending.txt";
        const char *const kSaveFiles[] = {gt4save::kGamedataFile, gt4save::kGarageFile, "icon.sys", "icon0.ico"};

        uint32_t rd32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        // GT4's Days counter: elapsed days + 1 (SAVE-FORMAT.md 6).
        int64_t gameDay(const gt4save::Save &s)
        {
            if (s.dec.gamedataPlain.size() < edit::kToday + 4)
                return 0;
            return int64_t(rd32(s.dec.gamedataPlain.data() + edit::kToday)) - int64_t(gt4save::kDay0) + 1;
        }

        std::string credits(uint64_t c) { return "Cr. " + groupedNumber(c); }

        std::string kmText(double km) { return km < 100 ? strf("%.1f km", km) : groupedNumber(static_cast<uint64_t>(std::llround(km))) + " km"; }

        std::string colourOf(const GarageEdit &e, const GameNames *names)
        {
            if (!e.colourName.empty())
                return e.colourName;
            return dealerColourName(names, e.code, e.colour, false);
        }

        // The edit itself, on a save in memory (planEdit's copy, applyEdit's copy). newSlot: the garage slot of an added car.
        bool editInMemory(gt4save::Save &s, const GarageEdit &e, std::string &note, int *newSlot)
        {
            const uint64_t have = s.credits().value_or(0);
            switch (e.action)
            {
            case GarageAction::OilChange:
            case GarageAction::Wash:
            case GarageAction::RemoveRigidity:
            case GarageAction::RefreshBody:
            {
                const bool ok = e.action == GarageAction::OilChange   ? edit::changeOil(s, e.slot, note)
                                : e.action == GarageAction::Wash      ? edit::wash(s, e.slot, note)
                                : e.action == GarageAction::RefreshBody ? edit::refreshBody(s, e.slot, note)
                                                                        : edit::removeRigidity(s, e.slot, note);
                if (!ok)
                    return false;
                if (e.pay)
                {
                    std::string n2;
                    if (have < e.price || !edit::setCredits(s, have - e.price, n2))
                    {
                        note = "not enough credits";
                        return false;
                    }
                    note += "; " + n2;
                }
                return true;
            }
            case GarageAction::AddCar:
            {
                edit::NewCar car;
                car.code = e.code;
                car.colour = e.colour;
                car.odometerM = e.odometerM;
                car.powerX100 = e.powerX100;
                car.weightKg = e.weightKg;
                car.how = e.how;
                car.logCalendar = true; // one day passes, as when GT4 sells a car
                car.setAvailable = true;
                if (e.pay)
                    car.price = e.price;
                const int slot = edit::addCar(s, car, note);
                if (newSlot)
                    *newSlot = slot;
                return slot >= 0;
            }
            case GarageAction::RenameSave:
                if (!nameProblem(e.newName).empty())
                {
                    note = nameProblem(e.newName);
                    return false;
                }
                return edit::setPlayerName(s, e.newName, note);
            case GarageAction::RepairSave:
                note = "the repair works on the files, not on the save in memory";
                return false;
            }
            return false;
        }

        std::map<std::string, std::vector<uint8_t>> folderFiles(const fs::path &dir)
        {
            std::map<std::string, std::vector<uint8_t>> m;
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
            {
                std::error_code e2;
                if (entry.is_regular_file(e2) && !endsWith(lower(pathText(entry.path().filename())), ".tmp"))
                {
                    std::vector<uint8_t> d;
                    readFile(entry.path(), d);
                    m[pathText(entry.path().filename())] = std::move(d);
                }
            }
            return m;
        }

        // The repair's confirmation (gt4save.py repair's rules, gt4save::edit::repairFiles).
        EditPlan planRepair(const gt4save::Save &save, const fs::path &saveDir, EditPlan p)
        {
            std::map<std::string, std::vector<uint8_t>> out;
            std::string why;
            edit::RepairInfo info;
            if (!edit::repairFiles(save, folderFiles(saveDir), out, why, &info))
            {
                p.why = startsWith(why, "nothing to repair") ? std::string("Nothing to repair: this save has no extra data from the old memory-card bug.")
                                                             : "The launcher cannot repair this save safely (" + why + "). Nothing was changed.";
                return p;
            }
            p.title = "Repair this save?";
            p.confirm = "Repair it";
            if (info.gamedataCopies > 1)
                p.changes.push_back(strf("Save data: the newest of the %u saves in the file is kept, at GT4's size (237,760 bytes); it is the one shown here",
                                         info.gamedataCopies));
            if (info.headerWrites || info.carWrites)
            {
                p.changes.push_back("Garage file: " + groupedNumber(info.garageBytes) + " -> 1,280,064 bytes, GT4's size (the data written past its end is taken out)");
                if (info.tailHeader)
                    p.changes.push_back("Garage header: the newer one from the end of the file goes to its start; credits " +
                                        (info.creditsAfter == info.creditsBefore ? credits(info.creditsBefore) + ", unchanged"
                                                                                 : credits(info.creditsBefore) + " -> " + credits(info.creditsAfter)));
                else if (info.staleTail)
                    p.changes.push_back(strf("Garage header: the one at the start of the file stays (it matches the save data; the %d at the end %s older)",
                                             info.headerWrites, info.headerWrites == 1 ? "is" : "are") +
                                        std::string("; credits ") + credits(info.creditsBefore) + ", unchanged");
                if (info.carWrites)
                {
                    std::vector<std::string> sl;
                    for (int s : info.carSlots)
                        sl.push_back(std::to_string(s));
                    p.changes.push_back(strf("Car records: the %d written at the end go to their places (garage slot%s ", info.carWrites, sl.size() == 1 ? "" : "s") +
                                        join(sl, ", ") + ")");
                }
            }
            p.changes.push_back("Nothing else changes: the same result as the save tool's repair (gt4save.py repair)");
            p.allowed.clear(); // verify() compares with the save as the repair leaves it (repairExpected): nothing may differ
            p.ok = !p.fingerprint.empty();
            if (!p.ok)
                p.why = "The save's files cannot be read.";
            return p;
        }

        std::vector<uint8_t> fileBytes(const fs::path &p)
        {
            std::vector<uint8_t> d;
            readFile(p, d);
            return d;
        }

        // A file replaced so that a crash or a power cut leaves either the old file or the new one: written next to it
        // (<name>.tmp), its data flushed to the disk (FlushFileBuffers: the rename alone does not carry the data), then
        // renamed over it. The temporary file is the launcher's own; it is removed when the write fails.
        bool writeBytes(const fs::path &p, const std::vector<uint8_t> &d, std::string &error)
        {
            fs::path tmp = p;
            tmp += L".tmp";
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                error = strf("cannot write %s (error %lu)", pathText(tmp).c_str(), GetLastError());
                return false;
            }
            bool ok = true;
            size_t off = 0;
            while (ok && off < d.size())
            {
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(d.size() - off, size_t(1) << 24));
                DWORD done = 0;
                ok = WriteFile(h, d.data() + off, chunk, &done, nullptr) && done == chunk;
                off += done;
            }
            ok = ok && FlushFileBuffers(h);
            const DWORD err = ok ? 0 : GetLastError();
            CloseHandle(h);
            if (!ok)
            {
                error = strf("write failed: %s (error %lu)", pathText(tmp).c_str(), err);
                DeleteFileW(tmp.c_str());
                return false;
            }
            if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = strf("cannot replace %s (error %lu)", pathText(p).c_str(), GetLastError());
                DeleteFileW(tmp.c_str());
                return false;
            }
            return true;
        }

        std::string fingerprintOf(const std::map<std::string, std::vector<uint8_t>> &files)
        {
            std::vector<std::string> parts;
            for (const auto &[name, d] : files)
                parts.push_back(strf("%s:%zu:%08x", name.c_str(), d.size(), crc32(d.data(), d.size())));
            std::sort(parts.begin(), parts.end());
            return join(parts, ";");
        }

        bool isTemp(const fs::path &p) { return endsWith(lower(pathText(p.filename())), ".tmp"); }
    }

    // ------------------------------------------------------------------ the requests
    std::string nameProblem(const std::string &name)
    {
        if (name.find_first_not_of(' ') == std::string::npos)
            return "Type a name: GT4 takes no empty name (\"No letters\").";
        if (name.size() > kNameMaxLetters)
            return strf("GT4's name entry takes at most %zu letters; this name has %zu.", kNameMaxLetters, name.size());
        for (const char c : name)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' '))
                return "Use letters A-Z and a-z, digits 0-9 and spaces only (what GT4's own name keyboard surely has).";
        if (name.front() == ' ' || name.back() == ' ')
            return "No space at the start or the end of the name.";
        return std::string();
    }

    GarageEdit repairEdit()
    {
        GarageEdit e;
        e.action = GarageAction::RepairSave;
        return e;
    }

    GarageEdit renameEdit(const std::string &name)
    {
        GarageEdit e;
        e.action = GarageAction::RenameSave;
        e.newName = name;
        return e;
    }

    GarageEdit serviceEdit(const GarageCar &car, GarageAction action)
    {
        GarageEdit e;
        e.action = action;
        e.slot = car.slot;
        e.carName = car.fullName.empty() ? car.name : car.fullName;
        e.code = car.code;
        e.label = car.label;
        // GT4's own price (the developer's answer 4): Cr. 50 for an oil change or a wash, Cr. 50,000 for the Rigidity Refresher Plan.
        e.pay = action == GarageAction::OilChange || action == GarageAction::Wash || action == GarageAction::RefreshBody;
        e.price = action == GarageAction::RefreshBody ? kRefreshPrice : e.pay ? kServicePrice : 0;
        e.tuneShop = !car.disc || car.disc->hasTuneShop(); // a car not in the disc's data: GT4 has a tune shop for all cars but one
        return e;
    }

    GarageEdit hiddenCarEdit(const CarCatalogue &cat, const HiddenCar &h, bool pay)
    {
        GarageEdit e;
        e.action = GarageAction::AddCar;
        const DiscCar *d = cat.carByLabel(h.label);
        if (!d)
            return e;
        if (h.rare() || h.given())
        {
            e = prizeCarEdit(*d, pay); // a rare car: GT4's new price, new, its first colour; the black Formula GT: in its black
            e.colour = h.colour;
            if (h.given())
            {
                e.colourName = "black";
                e.hiddenCar = true;
            }
            return e;
        }
        const auto [lo, hi] = usedMileageRangeKm(d->year);
        (void)hi;
        e.code = d->code;
        e.label = d->label;
        e.carName = d->fullName.empty() ? d->name : d->fullName;
        e.colour = h.colour;
        e.colourName = "black";
        e.odometerM = static_cast<uint32_t>(lo) * 1000u; // the lot's lowest mileage for the year: the price shown
        e.how = 2;                                         // GT4 sells them only on its used-car lot
        e.powerX100 = static_cast<uint32_t>(std::max(0, d->powerPs)) * 100u;
        e.weightKg = static_cast<uint32_t>(std::max(0, d->weightKg));
        e.hiddenCar = true;
        e.pay = pay;
        e.price = usedPrice(d->price, static_cast<uint64_t>(lo));
        return e;
    }

    GarageEdit withColour(GarageEdit e, int colour, const HiddenCar *h)
    {
        e.colour = colour;
        e.colourName = h && h->blackGrey > 0 && colour == h->colour ? std::string("black") : std::string();
        return e;
    }

    std::string dealerColourName(const GameNames *names, uint32_t code, int index, bool black)
    {
        if (names)
            if (const auto c = names->colour(code, index); c && !c->name.empty())
                return c->name;
        return black ? std::string("Black") : strf("Colour %d", index + 1);
    }

    GarageEdit prizeCarEdit(const DiscCar &d, bool pay)
    {
        GarageEdit e;
        e.action = GarageAction::AddCar;
        e.code = d.code;
        e.label = d.label;
        e.carName = d.fullName.empty() ? d.name : d.fullName;
        e.colour = 0;     // GT4's first colour of the car
        e.odometerM = 0;  // a prize comes new
        e.how = 1;        // bought new, at GT4's own new price
        e.powerX100 = static_cast<uint32_t>(std::max(0, d.powerPs)) * 100u;
        e.weightKg = static_cast<uint32_t>(std::max(0, d.weightKg));
        e.pay = pay;
        e.price = d.price;
        return e;
    }

    std::string serviceBlock(const Garage &g, const GarageCar &car, GarageAction action)
    {
        if (!g.loaded)
            return "No GT4 save to change.";
        if (action == GarageAction::RemoveRigidity)
        {
            if (!car.rigidity)
                return "This car does not have Increase Rigidity.";
            return std::string();
        }
        if (car.kind == 1)
            return "GT4 keeps this car's condition once you have taken it out: get in it once in GT4 first.";
        if (car.frozen)
            return "GT4 keeps this car's condition as it is.";
        if (action == GarageAction::OilChange && car.oilAgeKm < 0.05)
            return "The oil is fresh.";
        if (action == GarageAction::Wash && car.dirt01 <= 0.0)
            return "The car is clean.";
        if (action == GarageAction::RefreshBody)
        {
            // GT4's own rule (kRefreshPrice): every car it has a tune shop for; "already done" while its wear rate is 0.
            if (car.disc && !car.disc->hasTuneShop())
                return "GT4 has no tune shop for this car, so it never offers the Rigidity Refresher Plan for it.";
            if (car.bodyWearKm <= double(kRefreshFreshM) / 1000.0)
                return "Body wear is 500 km or less: GT4 counts the body as fresh and says the Rigidity Refresher Plan is already done.";
        }
        const uint64_t price = action == GarageAction::RefreshBody ? kRefreshPrice : kServicePrice;
        if (!g.hasCredits)
            return "This save has no credits to pay with.";
        if (g.credits < price)
            return "GT4 charges " + credits(price) + "; this save has " + credits(g.credits) + ".";
        return std::string();
    }

    EditTarget folderCardTarget(const fs::path &cardDir, int slot, const fs::path &backupDir)
    {
        EditTarget t;
        t.slot = slot;
        t.cardDir = cardDir;
        t.saveDir = cardDir / gt4save::kSaveFolder;
        t.backupDir = backupDir;
        std::error_code ec;
        if (!fs::is_regular_file(t.saveDir / gt4save::kGamedataFile, ec))
        {
            t.why = "No GT4 save on memory card slot " + std::to_string(slot) + ".";
            return t;
        }
        t.ok = true;
        return t;
    }

    std::string saveFingerprint(const fs::path &saveDir)
    {
        std::vector<std::string> parts;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(saveDir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            if (!entry.is_regular_file(e2) || isTemp(entry.path()))
                continue; // a launcher temporary file (an interrupted write) is not one of the save's files
            std::vector<uint8_t> d;
            if (!readFile(entry.path(), d))
                return std::string();
            parts.push_back(strf("%s:%zu:%08x", pathText(entry.path().filename()).c_str(), d.size(), crc32(d.data(), d.size())));
        }
        if (ec)
            return std::string();
        std::sort(parts.begin(), parts.end());
        return join(parts, ";");
    }

    // ------------------------------------------------------------------ the plan (the confirmation)
    EditPlan planEdit(const gt4save::Save &save, const fs::path &saveDir, const GarageEdit &e, const GameNames *names)
    {
        EditPlan p;
        p.fingerprint = saveFingerprint(saveDir);
        if (!save.loaded)
        {
            p.why = "This save cannot be read: " + save.error;
            return p;
        }
        if (e.action == GarageAction::RepairSave)
            return planRepair(save, saveDir, p); // its own rules (the save it repairs may fail GT4's checks until then)
        if (save.appendBugHit())
        {
            p.why = "This save still holds the extra data an older GT4 Recompilation memory card wrote: repair it first (Saves > Save details > "
                    "Repair), then try again.";
            return p;
        }
        if (!save.gt4LoadChecksPass())
        {
            p.why = "GT4's own checks fail on this save, so the launcher does not change it.";
            return p;
        }
        if (e.action == GarageAction::RenameSave)
        {
            const std::string bad = nameProblem(e.newName);
            const std::string old = edit::playerNameField(save);
            if (!bad.empty())
                p.why = bad;
            else if (e.newName == old)
                p.why = "That is the name the save has already.";
            else
            {
                gt4save::Save after = save;
                std::string note;
                if (!editInMemory(after, e, note, nullptr))
                    p.why = "This change cannot be made to this save (" + note + ").";
                else
                {
                    p.title = "Change your name in GT4 to \"" + e.newName + "\"?";
                    p.confirm = "Change the name";
                    p.changes.push_back("Name: \"" + old + "\" -> \"" + e.newName + "\" (GT4's Home screen, licences and records show it)");
                    p.changes.push_back("Nothing else changes: credits, cars, days and progress stay as they are");
                    p.allowed = {"player name"};
                    p.ok = !p.fingerprint.empty();
                    if (!p.ok)
                        p.why = "The save's files cannot be read.";
                }
            }
            return p;
        }
        if (e.action == GarageAction::RefreshBody)
        {
            // GT4's own rule for its Rigidity Refresher Plan (kRefreshPrice): a car it has a tune shop for, and not "already
            // done" (body life over 500,000 m; at or under it GT4's wear rate is 0). GT4 says so before it looks at the price.
            // (A frozen car, whose wear rate is 0 too, and a car with no full record are refused below with their own reason.)
            const auto k = edit::condition(save, e.slot);
            if (!e.tuneShop)
            {
                p.why = "GT4 has no tune shop for this car, so it never offers the Rigidity Refresher Plan for it.";
                return p;
            }
            if (k && !k->frozen && k->bodyLife <= kRefreshFreshM)
            {
                p.why = "Body wear is " + kmText(std::max(0.0, double(k->bodyLife)) / 1000.0) +
                        ": GT4 counts the body as fresh at 500 km or less and says the Rigidity Refresher Plan is already done.";
                return p;
            }
        }
        const uint64_t have = save.credits().value_or(0);
        const bool hasCredits = save.credits().has_value();
        if (e.pay && !hasCredits)
        {
            p.why = "This save has no credits to pay with.";
            return p;
        }
        if (e.pay && have < e.price)
        {
            p.why = "Not enough credits: this costs " + credits(e.price) + " and the save has " + credits(have) + ".";
            return p;
        }
        gt4save::Save after = save;
        std::string note;
        if (e.action == GarageAction::AddCar && e.label.empty())
        {
            p.why = "This car is not in GT4's car data on this disc.";
            return p;
        }
        std::optional<edit::Condition> before;
        if (e.action != GarageAction::AddCar)
            before = edit::condition(save, e.slot);
        if (e.action != GarageAction::AddCar && !before)
        {
            p.why = e.action == GarageAction::RemoveRigidity ? "This car has no full record in the save (GT4 makes one the first time you take it out)."
                                                             : "GT4 keeps this car's condition once you have taken it out: get in it once in GT4 first.";
            return p;
        }
        if (before && before->frozen && e.action != GarageAction::RemoveRigidity)
        {
            p.why = "GT4 keeps this car's condition as it is.";
            return p;
        }
        if (!editInMemory(after, e, note, &p.newSlot))
        {
            if (note.find("garage is full") != std::string::npos)
                p.why = "The garage is full (1,000 cars): GT4 cannot take another one.";
            else if (note.find("does not have Increase Rigidity") != std::string::npos)
                p.why = "This car does not have Increase Rigidity.";
            else
                p.why = "This change cannot be made to this save (" + note + ").";
            return p;
        }
        const std::string car = e.carName.empty() ? std::string("car") : e.carName;
        const std::string creditLine = e.pay ? "Credits: " + credits(have) + " -> " + credits(have - e.price) : std::string();
        switch (e.action)
        {
        case GarageAction::OilChange:
            p.title = "Change the oil of your " + car + "?";
            p.confirm = "Change the oil (" + credits(e.price) + ")";
            p.changes.push_back("Oil: " + kmText(before->oilAgeM() / 1000.0) + " on this oil -> fresh oil, as GT4's own oil change leaves it (GT4 counts the first 300 km as fresh)");
            p.changes.push_back(creditLine + " (GT4's price for an oil change)");
            p.allowed = {"oil"};
            break;
        case GarageAction::Wash:
            p.title = "Wash your " + car + "?";
            p.confirm = "Wash it (" + credits(e.price) + ")";
            p.changes.push_back(strf("Dirt: %.0f%% -> 0%%, as GT4's own car wash leaves it", std::clamp(double(before->dirt) / 0.8, 0.0, 1.0) * 100.0));
            p.changes.push_back(creditLine + " (GT4's price for a car wash)");
            p.allowed = {"dirtiness"};
            break;
        case GarageAction::RemoveRigidity:
            p.title = "Remove Increase Rigidity from your " + car + "?";
            p.confirm = "Remove it";
            p.changes.push_back("Increase Rigidity: taken off the car (in all three of its setting copies)");
            p.changes.push_back("The part is no longer marked as bought for this car, so GT4's tune shop offers it again");
            p.changes.push_back("Credits: unchanged (GT4 has no such service)");
            p.allowed = {"rigidity flag", "owned parts bit 88-95"};
            break;
        case GarageAction::RefreshBody:
            p.title = "Rigidity Refresh for your " + car + "?";
            p.confirm = "Rigidity Refresh (" + credits(e.price) + ")";
            p.changes.push_back("Body wear: " + kmText(double(before->bodyLife) / 1000.0) + (before->bodyLife >= edit::kBodyLifeMax ? " (GT4's maximum)" : "") +
                                " -> 0 km, as GT4's own Rigidity Refresher Plan leaves it");
            p.changes.push_back(before->rigidity ? std::string("Increase Rigidity stays fitted: the body is as it was just after the part was fitted")
                                                 : std::string("This car has no Increase Rigidity; GT4 offers the plan to it all the same (to every car it has a "
                                                               "tune shop for)"));
            p.changes.push_back(creditLine + " (GT4's price for the Rigidity Refresher Plan)");
            p.allowed = {"body life"};
            break;
        case GarageAction::AddCar:
        {
            const int64_t day = gameDay(save);
            const int cars = save.carCount() + 1;
            p.title = e.pay ? "Buy the " + car + "?" : "Add the " + car + " to your garage, free?";
            p.confirm = e.pay ? "Buy it (" + credits(e.price) + ")" : std::string("Add it");
            p.changes.push_back("Adds the " + car + " in " + colourOf(e, names) + " to your garage (" + std::to_string(cars) + " cars then)");
            p.changes.push_back(e.how == 1 ? std::string("Odometer: 0 km (a new car)")
                                           : "Odometer: " + groupedNumber(e.odometerM / 1000u) + " km (the lowest GT4's used-car lot gives a car of its year)");
            p.changes.push_back(e.pay ? creditLine : std::string("Credits: unchanged (added free)"));
            p.changes.push_back(strf("One day passes, as when you buy a car in GT4: Day %lld -> Day %lld; GT4's diary lists it as bought %s", static_cast<long long>(day),
                                     static_cast<long long>(day + 1), e.how == 1 ? "new" : "used"));
            p.changes.push_back("Arcade mode: the car joins your list of cars (if it is not there yet)");
            if (!e.hiddenCar)
                p.changes.push_back("GT4 does not count a car added this way as a prize won (your prize-car count stays as it is)");
            p.allowed = {"calendar count", "calendar entry", "calendar today", strf("garage index %d ", p.newSlot), "Arcade car list"};
            break;
        }
        }
        if (e.pay)
            p.allowed.push_back("credits");
        p.ok = !p.fingerprint.empty();
        if (!p.ok)
            p.why = "The save's files cannot be read.";
        return p;
    }

    // ------------------------------------------------------------------ the change
    EditResult applyEdit(const EditTarget &t, const GarageEdit &e, const std::string &fingerprint, int gt4State, const gt4save::SpecDb *spec,
                         const GameNames *names, const std::string &version)
    {
        EditResult r;
        auto log = [&](const std::string &s) { r.log.push_back(s); };
        auto refuse = [&](const std::string &why) {
            r.message = why;
            log("refused: " + why);
            return r;
        };
        if (!t.ok)
            return refuse(t.why.empty() ? std::string("No GT4 save to change.") : t.why);
        // 1. GT4 closed.
        if (gt4State == 1)
            return refuse("Close GT4 first: the launcher changes a save only while GT4 is closed (GT4 writes the memory card by itself while it runs).");
        if (gt4State != 0)
            return refuse("The launcher could not check whether GT4 is running, so nothing was changed. Try again.");
        log("GT4 is closed");
        // A change that was cut off earlier (its note still on the disk): the Garage changes nothing until the save is as it
        // was or as planned (then the note goes); a half-changed save needs its backup restored first.
        {
            std::string what;
            if (!settlePendingChange(t.backupDir, &what))
            {
                const PendingChange pc = pendingChange(t.backupDir);
                return refuse("A Garage change was cut off earlier and your save is half changed: restore " + pc.backup +
                              " first (the launcher offers it when it starts, or Saves > Restore). Nothing was changed.");
            }
            if (!what.empty())
                log(what);
        }
        // An interrupted change can leave a launcher temporary file next to the save's files: removed (they are only ours).
        std::error_code ec;
        for (const char *n : kSaveFiles)
        {
            fs::path tmp = t.saveDir / n;
            tmp += L".tmp";
            if (fs::exists(tmp, ec) && fs::remove(tmp, ec))
                log("removed a leftover temporary file " + pathText(tmp.filename()));
        }
        // 2. The save the player confirmed, as GT4 checks it.
        if (saveFingerprint(t.saveDir) != fingerprint || fingerprint.empty())
            return refuse("Your save changed after this was shown (GT4 may have saved), so nothing was changed. Look at it again and retry.");
        std::map<std::string, std::vector<uint8_t>> original;
        for (const auto &entry : fs::directory_iterator(t.saveDir, fs::directory_options::skip_permission_denied, ec))
        {
            std::error_code e2;
            if (entry.is_regular_file(e2) && !isTemp(entry.path()))
                original[pathText(entry.path().filename())] = fileBytes(entry.path());
        }
        const gt4save::Save before = gt4save::loadSaveFolder(t.saveDir, spec);
        const EditPlan plan = planEdit(before, t.saveDir, e, names);
        if (!plan.ok)
            return refuse(plan.why);
        if (plan.fingerprint != fingerprint)
            return refuse("Your save changed while it was read, so nothing was changed. Try again.");
        log("the save loads and passes GT4's load checks: " + std::string(before.gt4LoadChecksPass() ? "yes" : "no"));
        const bool repair = e.action == GarageAction::RepairSave;
        gt4save::Save after = before;
        std::map<std::string, std::vector<uint8_t>> files;
        std::string error;
        if (repair)
        {
            // The repair works on the files as they are (gt4save.py repair's bytes), from the save's files read above.
            edit::RepairInfo info;
            if (!edit::repairFiles(before, original, files, error, &info))
                return refuse("Nothing was changed: " + error + ".");
            log(strf("repair: GAMEDATA copies %u -> 1; garage %llu -> %zu bytes; appended headers %d (%s), car records %d", info.gamedataCopies,
                     static_cast<unsigned long long>(info.garageBytes), files.count(gt4save::kGarageFile) ? files[gt4save::kGarageFile].size() : size_t(0),
                     info.headerWrites, info.tailHeader ? "the newest matching one to offset 0" : info.staleTail ? "older: the one at offset 0 stays" : "none",
                     info.carWrites));
        }
        else
        {
            std::string note;
            int slot = -1;
            if (!editInMemory(after, e, note, &slot))
                return refuse("This change cannot be made to this save (" + note + ").");
            log("edit: " + note);
            if (!gt4save::roundtrip::rebuild(after, files, error))
                return refuse("Nothing was changed: the changed save could not be built (" + error + ").");
        }
        // What the changed save is checked against: the save as it was (only the planned fields may differ), or for the
        // repair the save as the repair leaves it (nothing may differ).
        const gt4save::Save reference = repair ? edit::repairExpected(before) : before;
        // 3. The backup, read back and compared with the save's files.
        fs::path zip;
        if (!backupCard(t.cardDir, t.slot, t.backupDir, version, "-before-edit", zip, error))
            return refuse("Nothing was changed: the backup could not be made (" + error + ").");
        r.backup = zip;
        log("backup " + pathText(zip));
        {
            std::vector<ZipItem> items;
            if (!readZip(zip, items, error))
                return refuse("Nothing was changed: the backup does not read back (" + error + ").");
            const std::string top = strf("mc%d/", t.slot - 1) + gt4save::kSaveFolder + "/";
            size_t matched = 0;
            for (const auto &[name, data] : original)
            {
                const auto it = std::find_if(items.begin(), items.end(), [&](const ZipItem &z) { return z.name == top + name; });
                if (it == items.end() || it->data != data)
                    return refuse("Nothing was changed: the backup does not hold the save's " + name + " as it is.");
                ++matched;
            }
            log(strf("backup read back: the save's %zu files byte-identical in it", matched));
        }
        // 4. The changed save in a new folder, checked.
        fs::path stage = t.backupDir / kStaging / pathFrom(nowFileStamp());
        for (int n = 2; fs::exists(stage, ec) && n < 100; ++n)
            stage = t.backupDir / kStaging / pathFrom(nowFileStamp() + "-" + std::to_string(n));
        const fs::path stagedSave = stage / gt4save::kSaveFolder;
        auto dropStage = [&]() {
            std::error_code e3;
            fs::remove_all(stage, e3); // the launcher's own staging copy
            fs::remove(t.backupDir / kStaging, e3); // only when empty
        };
        if (repair)
        {
            // The repaired files as they go to the card (writeFolder re-encrypts a save in memory; the repair is bytes).
            fs::create_directories(stagedSave, ec);
            for (const auto &[name, data] : files)
                if (!writeBytes(stagedSave / pathFrom(name), data, error))
                {
                    dropStage();
                    return refuse("Nothing was changed: the repaired save could not be written (" + error + ").");
                }
        }
        else
        {
            if (!edit::writeFolder(after, stagedSave, error))
            {
                dropStage();
                return refuse("Nothing was changed: the changed save could not be written (" + error + ").");
            }
        }
        const edit::VerifyResult v = edit::verify(reference, stagedSave, plan.allowed);
        for (const std::string &l : v.lines)
            log("staged: " + l);
        if (!v.ok)
        {
            dropStage();
            return refuse("Nothing was changed: the changed save did not pass the checks.");
        }
        // 5. The note of the change (so a cut-off change is found at the next start), then the card's files replaced (only
        // the ones that differ), then checked on the card.
        std::map<std::string, std::vector<uint8_t>> planned = original;
        for (const auto &[name, data] : files)
            planned[name] = data;
        {
            const std::string noteText = "# A Garage change of the GT4 Recompilation launcher in progress. If this file is still here, the\r\n"
                                     "# change was cut off: the launcher compares the save with the lines below when it starts and offers the backup.\r\n"
                                     "title=" + plan.title + "\r\nbackup=" + pathText(zip.filename()) + "\r\nslot=" + std::to_string(t.slot) +
                                     "\r\nsave=" + pathText(t.saveDir) + "\r\nbefore=" + fingerprint + "\r\nafter=" + fingerprintOf(planned) + "\r\n";
            std::string e5;
            if (!writeBytes(pendingChangeFile(t.backupDir), std::vector<uint8_t>(noteText.begin(), noteText.end()), e5))
            {
                dropStage();
                return refuse("Nothing was changed: the launcher could not write its note of the change (" + e5 + ").");
            }
            log("note of the change written: " + pathText(pendingChangeFile(t.backupDir).filename()));
        }
        auto dropNote = [&]() {
            std::error_code e6;
            fs::remove(pendingChangeFile(t.backupDir), e6); // the launcher's own note
        };
        std::vector<std::string> replaced;
        auto rollBack = [&]() {
            bool ok = true;
            for (const std::string &n : replaced)
            {
                std::string e4;
                if (!writeBytes(t.saveDir / pathFrom(n), original[n], e4))
                    ok = false;
            }
            r.rolledBack = true;
            const bool back = ok && saveFingerprint(t.saveDir) == fingerprint;
            log(std::string("rolled back: ") + (back ? "the save is as it was" : "FAILED (the note stays: the launcher offers the backup at its next start)"));
            if (back)
                dropNote();
            return back;
        };
        for (const auto &[name, data] : files)
        {
            const auto o = original.find(name);
            if (o != original.end() && o->second == data)
                continue;
            if (!writeBytes(t.saveDir / pathFrom(name), data, error))
            {
                const bool back = rollBack();
                dropStage();
                return refuse(back ? "Nothing was changed: the card could not be written (" + error + ")."
                                   : "The card could not be written (" + error + ") and was not put back as it was: restore " + pathText(zip.filename()) +
                                         " on the Saves page.");
            }
            replaced.push_back(name);
            log("replaced " + name + strf(" (CRC-32 %08x -> %08x)", o != original.end() ? crc32(o->second.data(), o->second.size()) : 0u, crc32(data.data(), data.size())));
        }
        const edit::VerifyResult onCard = edit::verify(reference, t.saveDir, plan.allowed);
        bool same = true;
        for (const auto &[name, data] : files)
            same = same && fileBytes(t.saveDir / pathFrom(name)) == data;
        for (const std::string &l : onCard.lines)
            log("card: " + l);
        if (!onCard.ok || !same)
        {
            const bool back = rollBack();
            dropStage();
            return refuse(back ? "The change did not check out on the card, so your save was put back as it was."
                               : "The change did not check out on the card and could not be undone: restore " + pathText(zip.filename()) + " on the Saves page.");
        }
        dropStage();
        dropNote();
        log("the note of the change removed (the card checked out)");
        // The record of the change.
        {
            std::string line = nowText() + strf("  slot %d  ", t.slot) + plan.title + "  backup " + pathText(zip.filename()) + "  files:";
            for (const std::string &n : replaced)
                line += strf(" %s %08x->%08x", n.c_str(), crc32(original[n].data(), original[n].size()), crc32(files[n].data(), files[n].size()));
            std::ofstream out(t.backupDir / kChangeLog, std::ios::binary | std::ios::app);
            out << line << "\r\n";
            out.flush();
            r.logged = static_cast<bool>(out);
            log(r.logged ? "recorded in " + std::string(kChangeLog) : "the change is done, but its line could not be added to " + std::string(kChangeLog));
        }
        r.message = editDoneText(e, zip);
        r.ok = true;
        log("done");
        return r;
    }

    std::string editDoneText(const GarageEdit &e, const fs::path &backup)
    {
        const std::string car = e.carName.empty() ? std::string("car") : e.carName;
        std::string m;
        switch (e.action)
        {
        case GarageAction::OilChange: m = "Done: the oil of your " + car + " is changed (" + credits(e.price) + ")."; break;
        case GarageAction::Wash: m = "Done: your " + car + " is washed (" + credits(e.price) + ")."; break;
        case GarageAction::RemoveRigidity: m = "Done: Increase Rigidity is off your " + car + "."; break;
        case GarageAction::RefreshBody: m = "Done: the body of your " + car + " is refreshed, body wear 0 km (" + credits(e.price) + ")."; break;
        case GarageAction::AddCar:
            m = "Done: the " + car + " is in your garage" + (e.pay ? " (" + credits(e.price) + ")." : std::string(".")) + " GT4 shows it the next time it starts.";
            break;
        case GarageAction::RepairSave:
            m = "Done: your save is repaired. The extra data the old memory-card bug left in it is gone, and its files are GT4's own sizes.";
            break;
        case GarageAction::RenameSave: m = "Done: your name in GT4 is now \"" + e.newName + "\". GT4 shows it the next time it starts."; break;
        }
        return m + " The card as it was is saved in backups\\" + pathText(backup.filename()) + " (Saves > Restore puts it back).";
    }

    // ------------------------------------------------------------------ Windows
    int gt4ProcessState()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return -1;
        PROCESSENTRY32W e{};
        e.dwSize = sizeof(e);
        int state = 0;
        BOOL ok = Process32FirstW(snap, &e);
        if (!ok)
            state = -1;
        for (; ok && state == 0; ok = Process32NextW(snap, &e))
            if (_wcsicmp(e.szExeFile, L"GT4.exe") == 0 || _wcsicmp(e.szExeFile, L"Gran Turismo 4.exe") == 0)
                state = 1;
        CloseHandle(snap);
        return state;
    }

    fs::path pendingChangeFile(const fs::path &backupDir) { return backupDir / kPending; }

    PendingChange pendingChange(const fs::path &backupDir)
    {
        PendingChange p;
        std::string text;
        if (!readText(pendingChangeFile(backupDir), text))
            return p;
        for (std::string &l : split(text, '\n'))
        {
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            const size_t eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos)
                continue;
            const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "title")
                p.title = v;
            else if (k == "backup")
                p.backup = v;
            else if (k == "slot")
            {
                long long n = 0;
                if (parseInt(v, n))
                    p.slot = static_cast<int>(n);
            }
            else if (k == "save")
                p.saveDir = pathFrom(v);
            else if (k == "before")
                p.before = v;
            else if (k == "after")
                p.after = v;
        }
        p.present = true;
        const std::string now = p.saveDir.empty() ? std::string() : saveFingerprint(p.saveDir);
        p.state = !now.empty() && now == p.before ? "none" : !now.empty() && now == p.after ? "done" : "half";
        return p;
    }

    bool settlePendingChange(const fs::path &backupDir, std::string *what)
    {
        const PendingChange p = pendingChange(backupDir);
        if (!p.present)
            return true;
        if (p.state == "half")
            return false;
        std::error_code ec;
        fs::remove(pendingChangeFile(backupDir), ec); // the launcher's own note
        if (what)
            *what = p.state == "done" ? "an earlier Garage change (" + p.title + ") had finished; its note removed"
                                      : "an earlier Garage change (" + p.title + ") had not started; the save is as it was; its note removed";
        return true;
    }

    std::vector<std::string> garageChangeLog(const fs::path &backupDir)
    {
        std::vector<std::string> out;
        std::string text;
        if (!readText(backupDir / kChangeLog, text))
            return out;
        for (std::string &l : split(text, '\n'))
        {
            if (!l.empty() && l.back() == '\r')
                l.pop_back();
            if (!l.empty())
                out.push_back(l);
        }
        return out;
    }
}
