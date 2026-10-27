// gt4save_edit.cpp - the Full edition's save writer (see gt4save_edit.h and SAVE-FORMAT.md).

#include "gt4save_edit.h"
#include "gt4save_crypto.h"
#include "gt4save_roundtrip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace gt4save::edit
{
    namespace
    {
        uint32_t rd32(const uint8_t *p) { return crypto::rd32(p); }
        void wr32(uint8_t *p, uint32_t v) { crypto::wr32(p, v); }
        uint64_t rd64(const uint8_t *p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }
        void wr64(uint8_t *p, uint64_t v)
        {
            wr32(p, uint32_t(v));
            wr32(p + 4, uint32_t(v >> 32));
        }
        float rdf(const uint8_t *p)
        {
            const uint32_t u = rd32(p);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        void wrf(uint8_t *p, float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            wr32(p, u);
        }
        std::string hex(size_t v)
        {
            char b[32];
            std::snprintf(b, sizeof b, "0x%zx", v);
            return b;
        }

        bool plainOk(const Save &s, std::string &note)
        {
            if (!s.loaded)
            {
                note = "the save is not loaded: " + s.error;
                return false;
            }
            if (s.dec.gamedataPlain.size() < kRidingCar + 1208)
            {
                note = "the GAMEDATA plaintext is short";
                return false;
            }
            return true;
        }

        uint64_t indexFlags(const Save &s, int slot) { return rd64(s.dec.gamedataPlain.data() + kGarageIndex + 32 * size_t(slot) + 8); }
        uint64_t indexCode(const Save &s, int slot) { return rd64(s.dec.gamedataPlain.data() + kGarageIndex + 32 * size_t(slot)); }

        // The car-record field name of an offset inside the 1,208 bytes.
        std::string carField(size_t o)
        {
            if (o < 3 * kBlockBytes)
            {
                const size_t b = o / kBlockBytes, r = o % kBlockBytes;
                std::string f;
                if (r < 0x08) f = "car code";
                else if (r < 0x10) f = "+0x08 (unknown u64)";
                else if (r < 0x18) f = "colour variation";
                else if (r < 0x20) f = "colour";
                else if (r < 0x110) f = "fitted part #" + std::to_string((r - 0x20) / 8);
                else if (r < 0x128) f = "gear ratio";
                else if (r == kBlockRigidity) f = "rigidity flag (Increase Rigidity)";
                else f = "setting " + hex(r);
                return "block " + std::to_string(b) + " " + f;
            }
            if (o >= 0x468 && o < 0x470) return "custom wheel";
            if (o >= 0x470 && o < 0x488) return "+0x470 (wheel data?)";
            if (o >= 0x488 && o < kOwnedParts) return "active block";
            if (o >= kOwnedParts && o < kOdometer) return "owned parts bit " + std::to_string((o - kOwnedParts) * 8) + "-" + std::to_string((o - kOwnedParts) * 8 + 7);
            if (o >= kOdometer && o < kBreakIn) return "odometer";
            if (o >= kBreakIn && o < kOil) return "break-in distance";
            if (o >= kOil && o < kDirt) return "oil";
            if (o >= kDirt && o < kBodyLife) return "dirtiness";
            if (o >= kBodyLife && o < kFrozen) return "body life";
            if (o == kFrozen) return "condition frozen";
            return "car +" + hex(o);
        }

        // writeFolder() writes a new folder (the launcher's staging folder, a test's copy), never a memory card's own
        // folder: the card's files are replaced only by the caller, after verify() (gt4full_saveedit.cpp).
        bool safeOut(const fs::path &dir, std::string &error)
        {
            for (const fs::path &part : fs::absolute(dir))
            {
                std::string p = part.string();
                std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (p == "mc0" || p == "mc1")
                {
                    error = "refusing to write into a memory card folder: " + dir.string();
                    return false;
                }
            }
            return true;
        }
    }

    const char *recordName(Record r)
    {
        switch (r)
        {
        case Record::None: return "none";
        case Record::Stock: return "stock (rebuilt from the index)";
        case Record::Riding: return "riding (GAMEDATA)";
        case Record::Stored: return "stored (garage slot)";
        }
        return "?";
    }

    Record record(const Save &s, int slot)
    {
        if (!s.loaded || slot < 0 || slot >= kGarageSlots || s.dec.gamedataPlain.size() < kRidingCar)
            return Record::None;
        const uint64_t fl = indexFlags(s, slot);
        if (indexCode(s, slot) == kNone64 || !(fl & 1))
            return Record::None;
        if (int32_t(rd32(s.dec.gamedataPlain.data() + kRidingSlot)) == slot)
            return Record::Riding;
        if ((fl & 2) && s.dec.slotPlain.count(slot))
            return Record::Stored;
        return Record::Stock;
    }

    const uint8_t *carBytes(const Save &s, int slot)
    {
        switch (record(s, slot))
        {
        case Record::Riding: return s.dec.gamedataPlain.data() + kRidingCar;
        case Record::Stored: return s.dec.slotPlain.at(slot).second.data();
        default: return nullptr;
        }
    }
    uint8_t *carBytes(Save &s, int slot) { return const_cast<uint8_t *>(carBytes(static_cast<const Save &>(s), slot)); }

    std::optional<Condition> condition(const Save &s, int slot)
    {
        const uint8_t *c = carBytes(s, slot);
        if (!c)
            return std::nullopt;
        Condition k;
        k.odometerM = rd32(c + kOdometer);
        k.breakIn = rdf(c + kBreakIn);
        k.oil = rdf(c + kOil);
        k.dirt = rdf(c + kDirt);
        k.bodyLife = rdf(c + kBodyLife);
        k.frozen = c[kFrozen] != 0;
        k.rigidity = c[kBlockRigidity] != 0 && c[kBlockBytes + kBlockRigidity] != 0 && c[2 * kBlockBytes + kBlockRigidity] != 0;
        k.rigidityOwned = (c[kOwnedParts + kRigidityOwnedBit / 8] >> (kRigidityOwnedBit % 8)) & 1;
        return k;
    }

    void fixInnerCrc(Save &s)
    {
        uint8_t *p = s.dec.gamedataPlain.data();
        wr32(p + kInnerCrcOffset, crypto::crc32(p + kBodyOffset, kBodyBytes));
    }

    // ------------------------------------------------------------------ the player's name
    std::string playerNameField(const Save &s)
    {
        if (s.dec.gamedataPlain.size() < kPlayerName + kPlayerNameBytes)
            return std::string();
        const char *p = reinterpret_cast<const char *>(s.dec.gamedataPlain.data() + kPlayerName);
        return std::string(p, strnlen(p, kPlayerNameBytes));
    }

    bool setPlayerName(Save &s, const std::string &name, std::string &note)
    {
        if (!plainOk(s, note))
            return false;
        if (name.empty() || name.size() >= kPlayerNameBytes || name.find('\0') != std::string::npos)
        {
            note = "the name cannot be stored (1 to 31 bytes, no NUL)";
            return false;
        }
        const std::string before = playerNameField(s);
        uint8_t *p = s.dec.gamedataPlain.data() + kPlayerName;
        std::memset(p, 0, kPlayerNameBytes);
        std::memcpy(p, name.data(), name.size());
        s.fields.profile.username = name;
        s.gamedata.profileName = name;
        fixInnerCrc(s);
        note = "name \"" + before + "\" -> \"" + name + "\"";
        return true;
    }

    // ------------------------------------------------------------------ the append bug (gt4save.py repair)
    namespace
    {
        // Which appended header goes to offset 0 (-1: none; the one at offset 0 stays). false: refused (why).
        bool repairHeaderChoice(const Save &s, int &useHeader, bool &stale, std::string &why)
        {
            useHeader = -1;
            stale = false;
            if (!s.garage)
                return true;
            int ih = -1, lastValid = -1, heads = 0;
            bool cars = false;
            for (const TrailingWrite &t : s.garage->trailing)
            {
                if (t.kind == TrailingKind::Header)
                {
                    ++ih;
                    ++heads;
                    if (garageHeaderValid(t.header, s.gamedata.garageKey, s.gamedata.garageCounter))
                        lastValid = ih;
                }
                else if (t.kind == TrailingKind::Car)
                    cars = true;
            }
            if (heads && lastValid < 0)
            {
                if (!s.garage->validForGamedata)
                {
                    why = "no garage header matches the save data (key, counter)";
                    return false;
                }
                if (cars)
                {
                    why = "the garage header written at the end is older than the one at the start of the file, and car records were written "
                          "at the end too: which are the newest cannot be told";
                    return false;
                }
                stale = true;
                return true;
            }
            useHeader = lastValid;
            return true;
        }
    }

    bool repairFiles(const Save &s, const std::map<std::string, std::vector<uint8_t>> &files, std::map<std::string, std::vector<uint8_t>> &out,
                     std::string &why, RepairInfo *info)
    {
        out.clear();
        if (!s.loaded)
        {
            why = "the save cannot be read: " + s.error;
            return false;
        }
        const GamedataInfo &g = s.gamedata;
        if ((!s.garage || s.garage->trailing.empty()) && g.copies == 1)
        {
            why = "nothing to repair: the garage has no bytes past its end and the save data holds one copy";
            return false;
        }
        if (!g.outerCrcOk)
        {
            why = "the newest copy of the save data does not decrypt cleanly";
            return false;
        }
        if (s.garage)
            for (const TrailingWrite &t : s.garage->trailing)
                if (t.kind == TrailingKind::Raw)
                {
                    why = "the garage has bytes at its end that are not a header or a car record";
                    return false;
                }
        int useHeader = -1;
        bool stale = false;
        if (!repairHeaderChoice(s, useHeader, stale, why))
            return false;
        const auto gd = files.find(kGamedataFile);
        if (gd == files.end())
        {
            why = "no save data file";
            return false;
        }
        // GAMEDATA: the newest copy (the last 237,760 bytes).
        const std::vector<uint8_t> &gdb = gd->second;
        out[kGamedataFile].assign(gdb.end() - std::ptrdiff_t(std::min(gdb.size(), kGamedataBytes)), gdb.end());
        // icon.sys, icon0.ico: their last copy when they were appended too (a whole number of copies), else as they are.
        for (const auto &[name, n] : {std::pair<const char *, size_t>{"icon.sys", 964}, {"icon0.ico", 9796}})
        {
            const auto it = files.find(name);
            if (it == files.end())
                continue;
            const std::vector<uint8_t> &b = it->second;
            if (b.size() > n && b.size() % n == 0)
                out[name].assign(b.end() - std::ptrdiff_t(n), b.end());
            else
                out[name] = b;
        }
        RepairInfo inf;
        inf.gamedataCopies = g.copies;
        if (s.garage)
        {
            const auto gr = files.find(kGarageFile);
            if (gr == files.end())
            {
                why = "the garage file cannot be read";
                return false;
            }
            const std::vector<uint8_t> &full = gr->second;
            inf.garageBytes = full.size();
            std::vector<uint8_t> raw(full.begin(), full.begin() + std::ptrdiff_t(std::min(full.size(), kGarageBytes)));
            inf.creditsBefore = inf.creditsAfter = s.garage->header.money;
            int ih = -1;
            for (const TrailingWrite &t : s.garage->trailing)
            {
                const size_t o = size_t(t.offset);
                if (t.kind == TrailingKind::Header)
                {
                    ++ih;
                    ++inf.headerWrites;
                    if (ih == useHeader && o + 64 <= full.size() && raw.size() >= 64)
                    {
                        std::copy(full.begin() + std::ptrdiff_t(o), full.begin() + std::ptrdiff_t(o + 64), raw.begin());
                        inf.tailHeader = true;
                        inf.creditsAfter = t.header.money;
                    }
                }
                else if (t.kind == TrailingKind::Car)
                {
                    ++inf.carWrites;
                    const size_t s0 = 64 + size_t(t.slot) * kSlotBytes;
                    if (o + kSlotBytes > full.size() || s0 + kSlotBytes > raw.size())
                    {
                        why = "a car record written at the end does not fit its slot";
                        return false;
                    }
                    std::copy(full.begin() + std::ptrdiff_t(o), full.begin() + std::ptrdiff_t(o + kSlotBytes), raw.begin() + std::ptrdiff_t(s0));
                    if (std::find(inf.carSlots.begin(), inf.carSlots.end(), t.slot) == inf.carSlots.end())
                        inf.carSlots.push_back(t.slot);
                }
            }
            inf.staleTail = stale;
            out[kGarageFile] = std::move(raw);
        }
        if (info)
            *info = inf;
        return true;
    }

    Save repairExpected(const Save &s)
    {
        Save e = s;
        int useHeader = -1;
        bool stale = false;
        std::string why;
        if (e.garage && repairHeaderChoice(s, useHeader, stale, why))
        {
            int ih = -1;
            size_t ic = 0;
            for (const TrailingWrite &t : s.garage->trailing)
                if (t.kind == TrailingKind::Header)
                {
                    if (++ih == useHeader && size_t(ih) < s.dec.trailingHeaders.size())
                        e.dec.garageHeader = s.dec.trailingHeaders[size_t(ih)];
                }
                else if (t.kind == TrailingKind::Car && ic < s.dec.trailingCars.size())
                {
                    e.dec.slotPlain[t.slot] = {t.seed, s.dec.trailingCars[ic++]};
                    e.dec.slotRaw.erase(t.slot);
                }
        }
        e.dec.gamedataPrefix.clear();
        e.dec.trailingHeaders.clear();
        e.dec.trailingCars.clear();
        e.dec.trailingRaw.clear();
        e.gamedata.copies = 1;
        if (e.garage)
            e.garage->trailing.clear();
        return e;
    }

    namespace
    {
        uint8_t *serviceCar(Save &s, int slot, const char *what, std::string &note)
        {
            if (!plainOk(s, note))
                return nullptr;
            const Record r = record(s, slot);
            if (r != Record::Riding && r != Record::Stored)
            {
                note = std::string(what) + ": garage slot " + std::to_string(slot) + " is " + recordName(r) +
                       "; only a car GT4 keeps a full record of (driven at least once) can be changed";
                return nullptr;
            }
            uint8_t *c = carBytes(s, slot);
            if (c[kFrozen])
            {
                note = std::string(what) + ": this car's condition is frozen by GT4 (it never changes)";
                return nullptr;
            }
            return c;
        }
        void done(Save &s, int slot, std::string &note, const std::string &text)
        {
            if (record(s, slot) == Record::Riding)
                fixInnerCrc(s);
            note = text + " (slot " + std::to_string(slot) + ", " + recordName(record(s, slot)) + ")";
        }
    }

    bool changeOil(Save &s, int slot, std::string &note)
    {
        uint8_t *c = serviceCar(s, slot, "oil change", note);
        if (!c)
            return false;
        const float before = rdf(c + kOil);
        wrf(c + kOil, kOilAfterChange);
        done(s, slot, note, "oil " + std::to_string(before) + " -> " + std::to_string(kOilAfterChange));
        return true;
    }

    bool wash(Save &s, int slot, std::string &note)
    {
        uint8_t *c = serviceCar(s, slot, "wash", note);
        if (!c)
            return false;
        const float before = rdf(c + kDirt);
        wrf(c + kDirt, 0.0f);
        done(s, slot, note, "dirtiness " + std::to_string(before) + " -> 0");
        return true;
    }

    bool refreshBody(Save &s, int slot, std::string &note)
    {
        uint8_t *c = serviceCar(s, slot, "rigidity refresh", note);
        if (!c)
            return false;
        const float before = rdf(c + kBodyLife);
        wrf(c + kBodyLife, 0.0f);
        done(s, slot, note, "body life " + std::to_string(before) + " -> 0");
        return true;
    }

    bool removeRigidity(Save &s, int slot, std::string &note)
    {
        if (!plainOk(s, note))
            return false;
        const Record r = record(s, slot);
        if (r != Record::Riding && r != Record::Stored)
        {
            note = "remove rigidity: slot " + std::to_string(slot) + " is " + recordName(r) + " (a stock car never has it)";
            return false;
        }
        uint8_t *c = carBytes(s, slot);
        const auto k = condition(s, slot);
        if (!k->rigidity && !k->rigidityOwned && !c[kBlockRigidity] && !c[kBlockBytes + kBlockRigidity] && !c[2 * kBlockBytes + kBlockRigidity])
        {
            note = "remove rigidity: this car does not have Increase Rigidity";
            return false;
        }
        for (int b = 0; b < 3; ++b)
            c[b * kBlockBytes + kBlockRigidity] = 0;
        c[kOwnedParts + kRigidityOwnedBit / 8] &= static_cast<uint8_t>(0xFFu ^ (1u << (kRigidityOwnedBit % 8)));
        done(s, slot, note, "Increase Rigidity removed (flag 0 in blocks 0-2, owned bit 95 cleared)");
        return true;
    }

    bool fitRigidity(Save &s, int slot, std::string &note)
    {
        if (!plainOk(s, note))
            return false;
        const Record r = record(s, slot);
        if (r != Record::Riding && r != Record::Stored)
        {
            note = "fit rigidity: slot " + std::to_string(slot) + " is " + recordName(r);
            return false;
        }
        uint8_t *c = carBytes(s, slot);
        for (int b = 0; b < 3; ++b)
            c[b * kBlockBytes + kBlockRigidity] = 1;
        c[kOwnedParts + kRigidityOwnedBit / 8] |= uint8_t(1u << (kRigidityOwnedBit % 8));
        done(s, slot, note, "Increase Rigidity fitted (test helper)");
        return true;
    }

    bool setCredits(Save &s, uint64_t credits, std::string &note)
    {
        if (!plainOk(s, note))
            return false;
        if (!s.garage || !s.dec.hasGarage)
        {
            note = "the save has no garage file (credits live in its header)";
            return false;
        }
        const uint64_t before = (uint64_t(rd32(s.dec.garageHeader.data())) << 32) | rd32(s.dec.garageHeader.data() + 4);
        wr32(s.dec.garageHeader.data(), uint32_t(credits >> 32));
        wr32(s.dec.garageHeader.data() + 4, uint32_t(credits));
        s.garage->header.money = credits;
        note = "credits " + std::to_string(before) + " -> " + std::to_string(credits);
        return true;
    }

    int addCar(Save &s, const NewCar &car, std::string &note)
    {
        if (!plainOk(s, note))
            return -1;
        if ((car.code >> 32) != 0 || car.code == kNone64)
        {
            note = "add car: not a car code (table 0)";
            return -1;
        }
        if (car.colour < 0 || car.colour > 127)
        {
            note = "add car: colour index out of range";
            return -1;
        }
        uint8_t *p = s.dec.gamedataPlain.data();
        // 0x435348: the first index entry whose in-garage bit is clear.
        int slot = -1;
        for (int i = 0; i < kGarageSlots; ++i)
            if (!(indexFlags(s, i) & 1))
            {
                slot = i;
                break;
            }
        if (slot < 0)
        {
            note = "add car: the garage is full (1,000 cars)";
            return -1;
        }
        uint64_t credits = s.garage ? s.garage->header.money : 0;
        if (car.price)
        {
            if (!s.garage)
            {
                note = "add car: no garage file, so no credits";
                return -1;
            }
            if (credits < *car.price)
            {
                note = "add car: not enough credits";
                return -1;
            }
        }
        // A reused entry may still hold a written slot of the car it held before (delCar keeps it); GT4's
        // addCar clears bit 1 (0x434e38), so the old record is never read again.
        if (s.dec.slotPlain.count(slot))
            note = "(slot " + std::to_string(slot) + " held a deleted car's record; it stays in the file, unused) ";
        const uint32_t today = rd32(p + kToday);
        const int64_t elapsed = int64_t(today) - int64_t(kDay0);
        uint8_t *e = p + kGarageIndex + 32 * size_t(slot);
        // 0x434e38: code; flags = 1 | colour << 2 (bit 1 clear: no record), power x100 bits 9-26, weight bits 32-44.
        wr64(e, car.code);
        uint64_t fl = rd64(e + 8);
        fl &= ~uint64_t(0x07FFFFFF);                // bits 0-26 (in garage, record, colour, power); 27-31 kept as GT4 does
        fl &= ~(uint64_t(0x1FFF) << 32);            // bits 32-44: weight (0x434e38 masks 0xF80001FF.., 0xFFFFE000FFFFFFFF)
        fl |= 1u | (uint64_t(car.colour & 0x7F) << 2) | (uint64_t(car.powerX100 & 0x3FFFF) << 9) | (uint64_t(car.weightKg & 0x1FFF) << 32);
        wr64(e + 8, fl);
        // 0x435148: low 24 bits = the elapsed day when added, top byte kept.
        const uint32_t w10 = rd32(e + 0x10);
        wr32(e + 0x10, (w10 & 0xFF000000u) | (uint32_t(elapsed) & 0x00FFFFFFu));
        wr32(e + 0x14, car.odometerM);              // odometer at purchase
        // +0x18: drive order 0x3FF = never driven (0x434d80); the rest of the word kept.
        const uint32_t w18 = rd32(e + 0x18);
        wr32(e + 0x18, (w18 & ~0x3FFu) | 0x3FFu);
        std::string what = "added to garage slot " + std::to_string(slot);

        if (car.setAvailable)
        {
            // Available car list: sorted, unique, -1 padded (insert 0x42e968).
            std::vector<uint64_t> list;
            for (size_t i = 0; i < kAvailableCarSlots; ++i)
            {
                const uint64_t c = rd64(p + kAvailableCars + 8 * i);
                if (c != kNone64)
                    list.push_back(c);
            }
            if (std::find(list.begin(), list.end(), car.code) == list.end() && list.size() < kAvailableCarSlots)
            {
                list.push_back(car.code);
                std::sort(list.begin(), list.end());
                for (size_t i = 0; i < kAvailableCarSlots; ++i)
                    wr64(p + kAvailableCars + 8 * i, i < list.size() ? list[i] : kNone64);
                what += ", Arcade list";
            }
        }
        if (car.logCalendar)
        {
            // 0x42f0e0 put + 0x42f1b8 GET_CAR: {type 1, colour, how (1 new, 2 used), 0, u32 0, u64 code}; today + 1.
            uint32_t count = rd32(p + kCalendarCount);
            if (count >= kCalendarMax)
            {
                std::memmove(p + kCalendarEntries, p + kCalendarEntries + 16, 16 * size_t(kCalendarMax - 1));
                count = kCalendarMax - 1;
            }
            uint8_t *ce = p + kCalendarEntries + 16 * size_t(count);
            std::memset(ce, 0, 16);
            ce[0] = 1;
            ce[1] = uint8_t(car.colour);
            ce[2] = uint8_t(car.how == 1 ? 1 : 2);
            wr64(ce + 8, car.code);
            wr32(p + kCalendarCount, count + 1);
            wr32(p + kToday, today + 1);
            what += std::string(", calendar GET_CAR (") + (car.how == 1 ? "new" : "used") + "), day " + std::to_string(elapsed) + " -> " + std::to_string(elapsed + 1);
        }
        if (car.price)
        {
            std::string n2;
            setCredits(s, credits - *car.price, n2);
            what += ", " + n2;
        }
        fixInnerCrc(s);
        note += what;
        return slot;
    }

    bool writeFolder(const Save &s, const fs::path &outDir, std::string &error)
    {
        if (!safeOut(outDir, error))
            return false;
        if (!s.gt4LoadChecksPass() || s.appendBugHit())
        {
            error = "the save does not pass GT4's load checks, or holds appended copies: not written";
            return false;
        }
        std::map<std::string, std::vector<uint8_t>> files;
        if (!roundtrip::rebuild(s, files, error))
            return false;
        std::error_code ec;
        if (fs::exists(outDir, ec) && !fs::is_empty(outDir, ec))
        {
            error = "the output folder is not empty: " + outDir.string();
            return false;
        }
        fs::create_directories(outDir, ec);
        for (const auto &[name, data] : files)
        {
            std::ofstream f(outDir / name, std::ios::binary);
            f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
            if (!f)
            {
                error = "cannot write " + (outDir / name).string();
                return false;
            }
        }
        return true;
    }

    std::string describeOffset(const std::string &file, size_t o)
    {
        if (file == "garage header")
        {
            if (o < 4) return "credits (high word)";
            if (o < 8) return "credits (low word)";
            return "garage header +" + hex(o);
        }
        if (file.rfind("garage slot", 0) == 0)
            return file + " car " + carField(o);
        // GAMEDATA plaintext
        if (o < 0x10) return o >= kInnerCrcOffset && o < kInnerCrcOffset + 4 ? "inner CRC" : "inner header";
        if (o >= kPlayerName && o < kPlayerName + kPlayerNameBytes) return "player name";
        if (o < 0x230) return "profile +" + hex(o);
        if (o < kCalendarEntries) return "calendar count";
        if (o < kToday) return "calendar entry " + std::to_string((o - kCalendarEntries) / 16);
        if (o < kGarageIndex) return "calendar today";
        if (o < kRidingSlot)
        {
            const size_t i = (o - kGarageIndex) / 32, r = (o - kGarageIndex) % 32;
            const char *f = r < 8 ? "car code" : r < 16 ? "flags (in garage, record, colour, power, weight)" : r < 0x14 ? "day added" : r < 0x18 ? "odometer at purchase" : "drive order";
            return "garage index " + std::to_string(i) + " " + f;
        }
        if (o < kRidingSlot + 4) return "riding slot";
        if (o < kRidingCar) return "garage key / counter";
        if (o < kRidingCar + 1208) return "riding car " + carField(o - kRidingCar);
        if (o >= 0x13AC0 && o < 0x146C0) return "race record row " + std::to_string((o - 0x13AC0) / 4);
        if (o >= kAvailableCars && o < kAvailableCars + 8 * kAvailableCarSlots) return "Arcade car list";
        return "GAMEDATA +" + hex(o);
    }

    std::vector<Change> diff(const Save &a, const Save &b)
    {
        std::vector<Change> out;
        auto cmp = [&](const std::string &file, const uint8_t *x, const uint8_t *y, size_t n) {
            size_t i = 0;
            while (i < n)
            {
                if (x[i] == y[i])
                {
                    ++i;
                    continue;
                }
                size_t j = i;
                std::string f = describeOffset(file, i);
                while (j < n && x[j] != y[j] && describeOffset(file, j) == f)
                    ++j;
                out.push_back({file, i, j - i, f});
                i = j;
            }
        };
        const size_t n = std::min(a.dec.gamedataPlain.size(), b.dec.gamedataPlain.size());
        cmp("GAMEDATA", a.dec.gamedataPlain.data(), b.dec.gamedataPlain.data(), n);
        if (a.dec.gamedataPlain.size() != b.dec.gamedataPlain.size())
            out.push_back({"GAMEDATA", n, 0, "size differs"});
        cmp("garage header", a.dec.garageHeader.data(), b.dec.garageHeader.data(), 64);
        std::map<int, int> slots;
        for (const auto &[k, v] : a.dec.slotPlain) slots[k] |= 1;
        for (const auto &[k, v] : b.dec.slotPlain) slots[k] |= 2;
        for (const auto &[k, v] : slots)
        {
            const std::string f = "garage slot " + std::to_string(k);
            if (v != 3)
                out.push_back({f, 0, 0, v == 1 ? "slot gone" : "slot new"});
            else
            {
                const auto &x = a.dec.slotPlain.at(k), &y = b.dec.slotPlain.at(k);
                if (x.first != y.first)
                    out.push_back({f, 0, 4, "slot seed"});
                cmp(f, x.second.data(), y.second.data(), std::min(x.second.size(), y.second.size()));
            }
        }
        // One line per field: join ranges of the same field (e.g. the Arcade car list after an insert).
        std::vector<Change> joined;
        for (const Change &c : out)
            if (!joined.empty() && joined.back().file == c.file && joined.back().field == c.field)
                joined.back().length = c.offset + c.length - joined.back().offset;
            else
                joined.push_back(c);
        return joined;
    }

    VerifyResult verify(const Save &original, const fs::path &dir, const std::vector<std::string> &allowed)
    {
        VerifyResult r;
        r.ok = true;
        Save w = loadSaveFolder(dir, nullptr, original.rounding);
        auto fail = [&](const std::string &t) {
            r.ok = false;
            r.lines.push_back("FAIL " + t);
        };
        if (!w.loaded)
        {
            fail("the written copy does not load: " + w.error);
            return r;
        }
        r.lines.push_back(std::string("GT4 load checks: ") + (w.gt4LoadChecksPass() ? "pass" : "FAIL"));
        if (!w.gt4LoadChecksPass())
            r.ok = false;
        for (const Check &c : w.checks)
            if (c.level == Level::Error)
                fail("check: " + c.message);
        const struct
        {
            const char *name;
            uint64_t size;
        } sizes[] = {{kGamedataFile, kGamedataBytes}, {kGarageFile, kGarageBytes}, {"icon.sys", 964}, {"icon0.ico", 9796}};
        for (const auto &sz : sizes)
        {
            std::error_code ec;
            const uint64_t got = fs::file_size(dir / sz.name, ec);
            if (ec || got != sz.size)
                fail(std::string(sz.name) + " is " + std::to_string(got) + " bytes, " + std::to_string(sz.size) + " expected");
        }
        // The copy re-encrypts to itself (GT4's seeds kept): rebuild(load(copy)) == copy.
        std::map<std::string, std::vector<uint8_t>> files;
        std::string err;
        if (!roundtrip::rebuild(w, files, err))
            fail("rebuild of the written copy: " + err);
        else
            for (const auto &[name, data] : files)
            {
                std::ifstream f(dir / name, std::ios::binary);
                std::vector<uint8_t> disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (disk != data)
                    fail(name + " does not re-encrypt to itself");
            }
        for (const Change &c : diff(original, w))
        {
            bool okField = c.field == "inner CRC";
            for (const std::string &a : allowed)
                if (c.field.find(a) != std::string::npos)
                    okField = true;
            char line[400];
            std::snprintf(line, sizeof line, "%s %s +0x%zx (%zu bytes): %s", okField ? "changed" : "UNEXPECTED", c.file.c_str(), c.offset, c.length, c.field.c_str());
            r.lines.push_back(line);
            if (!okField)
                r.ok = false;
        }
        return r;
    }
}
