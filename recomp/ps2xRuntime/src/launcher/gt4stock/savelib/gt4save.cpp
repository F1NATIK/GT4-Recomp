// gt4save.cpp - read path of the GT4 save library: card sources, decryption and checks
// (gt4save.py analyse), field parsers and invariants (gt4save_fields.py), the report's extra checks
// and licence status (gt4save_report.py). See gt4save.h.

#include "gt4save.h"
#include "gt4save_crypto.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

namespace gt4save
{
    using namespace crypto;

    namespace
    {
        // ------------------------------------------------------------------ helpers
        std::string fmt(const char *f, ...)
        {
            char buf[512];
            va_list ap;
            va_start(ap, f);
            const int n = std::vsnprintf(buf, sizeof buf, f, ap);
            va_end(ap);
            if (n < 0)
                return {};
            if (size_t(n) < sizeof buf)
                return std::string(buf, size_t(n));
            std::string big(size_t(n) + 1, '\0');
            va_start(ap, f);
            std::vsnprintf(big.data(), big.size(), f, ap);
            va_end(ap);
            big.resize(size_t(n));
            return big;
        }

        uint8_t u8(const uint8_t *b, size_t o) { return b[o]; }
        int8_t s8(const uint8_t *b, size_t o) { return int8_t(b[o]); }
        uint16_t u16(const uint8_t *b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
        uint32_t u32(const uint8_t *b, size_t o) { return rd32(b + o); }
        int32_t s32(const uint8_t *b, size_t o) { return int32_t(rd32(b + o)); }
        uint64_t u64(const uint8_t *b, size_t o) { return uint64_t(rd32(b + o)) | (uint64_t(rd32(b + o + 4)) << 32); }
        float f32(const uint8_t *b, size_t o)
        {
            float f;
            const uint32_t v = rd32(b + o);
            std::memcpy(&f, &v, 4);
            return f;
        }
        // b[o:o+n].split(b"\0")[0], kept as raw (latin-1) bytes.
        std::string cstr(const uint8_t *b, size_t o, size_t n)
        {
            size_t len = 0;
            while (len < n && b[o + len])
                ++len;
            return std::string(reinterpret_cast<const char *>(b + o), len);
        }
        std::optional<uint64_t> code(uint64_t v) { return v == kNone64 ? std::nullopt : std::optional<uint64_t>(v); }

        bool readFile(const fs::path &p, std::vector<uint8_t> &out)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f)
                return false;
            f.seekg(0, std::ios::end);
            const std::streamoff n = f.tellg();
            if (n < 0)
                return false;
            f.seekg(0, std::ios::beg);
            out.resize(size_t(n));
            if (n > 0)
                f.read(reinterpret_cast<char *>(out.data()), n);
            return bool(f) || f.eof();
        }

        // GAMEDATA payload map (UserProfile::save 0x43b168 call order; sizes from each size()).
        constexpr size_t kOffProfile = 0x10, kOffCalendar = 0x230, kOffToday = 0xB8D4, kOffGarageIndex = 0xB8E0,
                         kOffGarageFields = 0x135E0, kOffGarageKey = 0x135E4, kOffGarageCounter = 0x135F8, kOffCurrentCar = 0x135FC,
                         kOffRaceRecord = 0x13AC0, kOffCourseRecord = 0x146C0, kOffMachineRecord = 0x262C0, kOffLicense = 0x26BA0,
                         kOffAvailable = 0x361C0, kOffFavoriteCar = 0x385D0, kOffFavoriteCourse = 0x386E0, kOffPresent = 0x387F0,
                         kOffChampionship = 0x38870, kOffUsedCar = 0x38900, kOffOption = 0x38930, kOffContext = 0x39F30, kOffEnd = 0x3A080;
        constexpr uint32_t kSerialMagic = 0xFF60BAD3, kGamedataBody = 0x3A070; // Status::size() - 16 (US)
        constexpr size_t kCourseUnit = 568, kLicenseUnit = 504, kBlock = 376;

        const char *const kDayEventTypes[9] = {"NO_EVENT", "GET_CAR", "SELL_CAR", "RUN_RACE", "RUN_LICENSE", "RUN_MISSION", "BUY_WHEEL", "BUY_WING", "RUN_COURSE"};
        const char *const kResult[16] = {"1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "abandon", "gold", "silver", "bronze", "red", "failed", "view", "none"};
        const char *const kUnitType[4] = {"none", "license", "event", "mission"};

        struct OptionDef
        {
            const char *name;
            uint32_t ram;
        };
        // MOption script accessors (0x17fd20 .. 0x188cd8): payload offset = option + (RAM offset - 8).
        const OptionDef kOptionU32[] = {
            {"language", 12}, {"wide_mode", 20}, {"screen_adjust_h", 24}, {"screen_adjust_v", 28}, {"sharpness_mode", 32}, {"menu_bgm", 92},
            {"menu_se", 100}, {"race_bgm", 108}, {"race_se", 116}, {"replay_bgm", 124}, {"replay_se", 132}, {"sound_type", 4272},
            {"narration", 4276}, {"enableAC3", 4280}, {"auto_save", 4284}, {"race_laps", 4288}, {"tire_damage", 4292},
            {"automatic_gear", 4296}, {"automatic_ghost", 4300}, {"assist_asm", 4304}, {"assist_tcs", 4308}, {"difficulty", 4312},
            {"run_with_enemies", 4316}, {"license_transmission_MT", 4320}, {"penalty_type", 4324}, {"limit_favorite_car_only", 4328},
            {"limit_favorite_course_only", 4332}, {"limit_enemies_to_favorite_car_only", 4336}, {"view_type_1p", 4340},
            {"view_type_2p", 4344}, {"split_race_laps", 4348}, {"split_tire_damage", 4352}, {"split_handicap", 4356},
            {"split_boost_level", 4360}, {"photo_quality", 4364}, {"photo_manual_focus", 4368}, {"photo_aspect", 4372},
            {"photo_shutter", 4376}, {"photo_professional", 4380}, {"photo_saturation", 4384}, {"photo_save_slot", 4388},
            {"photo_save_method", 4392}, {"steering_assist_1p", 4396}, {"steering_assist_2p", 4400}, {"active_steering_1p", 4404},
            {"active_steering_2p", 4408}, {"replay_mode", 4412}, {"race_display_flags", 4416}, {"replay_2p_split", 4420},
            {"replay_memory_card_slot", 4424}, {"replay_play_mode", 4428}, {"timeout_count_to_autodemo", 4432},
            {"timeout_count_to_topmenu", 4436}, {"machine_role", 4440}, {"special_arcade_tuner", 4444},
            {"arcade_skip_to_favorite", 4448}, {"opening_after_autoload", 4452}, {"can_watch_ending", 4456},
            {"demo_movie_interval", 4460}, {"album_slide_effect", 4464}, {"album_slide_play_time", 4468},
            {"album_memory_card_slot", 4472}, {"ustorage_photo_quality", 4476}, {"album_slide_transition_time", 4480}};
        const OptionDef kOptionU8[] = {{"unit_velocity", 16}, {"unit_power", 17}, {"unit_torque", 18}};
        const OptionDef kOptionF32[] = {{"menu_bgm_volume", 96},  {"menu_se_volume", 104},    {"race_bgm_volume", 112}, {"race_se_volume", 120},
                                        {"replay_bgm_volume", 128}, {"replay_se_volume", 136}, {"slide_bgm_volume", 140}};

        struct LicenceRange
        {
            const char *cat;
            int start, n;
        };
        const LicenceRange kLicenceIndex[] = {{"B", 0, 16}, {"A", 16, 16}, {"IB", 32, 16}, {"IA", 48, 16}, {"S", 64, 16}, {"coffee", 80, 5}, {"mission", 85, 34}}; // 0x4323d0

        std::string licenceIndexName(int i)
        {
            for (const auto &r : kLicenceIndex)
                if (r.start <= i && i < r.start + r.n)
                    return fmt("%s-%d", r.cat, i - r.start + 1);
            return fmt("unused-%d", i);
        }

        std::vector<uint64_t> codeList(const uint8_t *p, size_t o, size_t n)
        {
            std::vector<uint64_t> out;
            for (size_t k = 0; k < n; ++k)
            {
                const uint64_t v = u64(p, o + 8 * k);
                if (v != kNone64)
                    out.push_back(v);
            }
            return out;
        }

        // ------------------------------------------------------------------ parsers (gt4save_fields.py)
        Profile parseProfile(const uint8_t *p)
        {
            Profile r;
            r.username = cstr(p, 0x10, 32);
            r.password = cstr(p, 0x30, 8);
            r.lastEntryName = cstr(p, 0x38, 16);
            r.score = u64(p, 0x48);
            r.totalPrizeMoney = u64(p, 0x50);
            r.totalPrizeCars = u32(p, 0x58);
            for (int k = 0; k < 7; ++k)
                r.racesByResult[k] = u32(p, 0x5C + 4 * k);
            r.aspecDistance = u64(p, 0x78);
            r.bspecDistance = u64(p, 0x80);
            r.withdrawn = u32(p, 0x88);
            r.withdrawnGt4p = u32(p, 0x8C);
            r.metType = u8(p, 0x90);
            for (uint32_t v : r.racesByResult)
                r.racesTotal += v; // script getTotalRaceCount (0x43b018)
            r.wins = r.racesByResult[1];
            for (int o = 0x94; o < 0x228; ++o)
                if (p[o])
                    r.unknownNonzero.push_back(o);
            return r;
        }

        Calendar parseCalendar(const uint8_t *p)
        {
            Calendar c;
            c.count = u32(p, kOffCalendar);
            c.today = u32(p, kOffToday);
            c.days = int64_t(c.today) - int64_t(kDay0);
            const uint32_t n = std::min<uint32_t>(c.count, 2922);
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint8_t *e = p + kOffCalendar + 4 + 16 * size_t(k);
                DayEvent ev;
                ev.day = c.days - int64_t(n) + int64_t(k);
                ev.type = e[0];
                ev.a = e[1];
                ev.b = e[2];
                ev.c = e[3];
                ev.u32 = u32(e, 4);
                ev.code = code(u64(e, 8));
                c.events.push_back(ev);
            }
            return c;
        }

        GarageEntry parseGarageEntry(const uint8_t *e, int slot)
        {
            GarageEntry g;
            g.slot = slot;
            const uint64_t c = u64(e, 0), flags = u64(e, 8), w16 = u64(e, 16), w24 = u64(e, 24);
            g.code = code(c);
            g.inGarage = uint32_t(flags & 1);              // delCar 0x435860 clears / undelCar 0x4358c8 sets
            g.storedParams = uint32_t((flags >> 1) & 1);   // 0x435388 / 0x4354f8; clear -> rebuilt from the index
            g.colour = uint32_t((flags >> 2) & 0x7F);
            g.powerX100 = uint32_t((flags >> 9) & 0x3FFFF);
            g.weight = uint32_t((flags >> 32) & 0x1FFF);
            g.dayAdded = uint32_t(w16 & 0xFFFFFF);
            g.flagsTop = uint32_t((w16 >> 24) & 0xFF);
            g.odometer = (w16 >> 32) & 0xFFFFFFFFu;
            g.rideOrder = uint32_t(w24 & 0x3FF);
            g.raw = {c, flags, w16, w24};
            return g;
        }

        Block parseBlock(const uint8_t *b)
        {
            Block k;
            k.car = code(u64(b, 0x00));
            k.unknown08 = code(u64(b, 0x08));
            k.variation = code(u64(b, 0x10));
            k.colour = s8(b, 0x18);
            for (int i = 0; i < 30; ++i)
            {
                const uint64_t v = u64(b, 0x20 + 8 * i);
                if (v != kNone64)
                    k.parts.push_back(v);
            }
            for (int i = 0; i < 12; ++i)
                k.gearsX1000[i] = u16(b, 0x110 + 2 * i);
            BlockSettings &s = k.settings;
            s.finalGearX1000 = u16(b, 0x128);
            s.gearAutoLevel = u8(b, 0x12A);
            s.finalGearDefault = u16(b, 0x12C);
            s.drivetrain = u8(b, 0x12E);
            s.brakeCtrlF = u8(b, 0x12F);
            s.brakeCtrlR = u8(b, 0x130);
            s.racingModifyA = u8(b, 0x131);
            s.racingModifyB = u8(b, 0x132);
            s.camberF = u8(b, 0x139);
            s.camberR = u8(b, 0x13A);
            s.rideHeightF = u16(b, 0x13C);
            s.rideHeightR = u16(b, 0x13E);
            s.toeF = u8(b, 0x140);
            s.toeR = u8(b, 0x141);
            s.springF = u8(b, 0x142);
            s.springR = u8(b, 0x143);
            for (int i = 0; i < 8; ++i)
                s.damper[i] = u8(b, 0x146 + i);
            s.stabilizerF = u8(b, 0x14E);
            s.stabilizerR = u8(b, 0x14F);
            s.lsdF = {u8(b, 0x150), u8(b, 0x152), u8(b, 0x154)};
            s.lsdR = {u8(b, 0x151), u8(b, 0x153), u8(b, 0x155)};
            s.tcs = u8(b, 0x156);
            for (int i = 0; i < 4; ++i)
                s.asmSetting[i] = u8(b, 0x157 + i);
            s.ballastWeight = u8(b, 0x15B);
            s.ballastPosition = s8(b, 0x15C);
            s.nos = u8(b, 0x167);
            return k;
        }

        CourseUnit parseCourseUnit(const uint8_t *b, int index)
        {
            CourseUnit u;
            u.index = index;
            for (int k = 0; k < 10; ++k)
            {
                const size_t e = 8 + 48 * size_t(k);
                const uint32_t t = u32(b, e);
                if ((t == kNoTime || t == 0) && u64(b, e + 8) == kNone64)
                    continue;
                CourseEntry ce;
                ce.rank = k;
                ce.value = t;
                ce.speed = f32(b, e); // max-speed records use a float
                ce.year = u16(b, e + 4);
                ce.month = b[e + 6];
                ce.day = b[e + 7];
                ce.car = code(u64(b, e + 8));
                ce.name = cstr(b, e + 16, 9);
                u.entries.push_back(ce);
            }
            for (int k = 0; k < 16; ++k)
            {
                const uint32_t x = u32(b, 0x1E8 + 4 * size_t(k));
                if (x != kNoTime)
                    u.extraTimes.push_back(x);
            }
            u.course = code(u64(b, 0));
            u.bestScore = b[0x230];
            return u;
        }

        std::vector<LicenceUnit> parseLicenceRecords(const uint8_t *p)
        {
            std::vector<LicenceUnit> out;
            for (int i = 0; i < 125; ++i)
            {
                const uint8_t *b = p + kOffLicense + kLicenseUnit * size_t(i);
                LicenceUnit u;
                u.index = i;
                u.test = licenceIndexName(i);
                for (int k = 0; k < 10; ++k)
                {
                    const size_t e = 44 * size_t(k);
                    const uint32_t t = u32(b, e);
                    if (t == kNoTime)
                        continue;
                    LicenceEntry le;
                    le.rank = k;
                    le.timeMs = t;
                    le.year = u16(b, e + 4);
                    le.month = b[e + 6];
                    le.day = b[e + 7];
                    le.result = b[e + 8];
                    le.name = cstr(b, e + 9, 9);
                    u.entries.push_back(le);
                }
                for (int k = 0; k < 16; ++k)
                {
                    const uint32_t x = u32(b, 440 + 4 * size_t(k));
                    if (x != kNoTime)
                        u.extraTimes.push_back(x);
                }
                if (!u.entries.empty() || !u.extraTimes.empty())
                    out.push_back(std::move(u));
            }
            return out;
        }

        Options parseOptions(const uint8_t *p)
        {
            Options o;
            const size_t base = kOffOption;
            o.plainSize = u32(p, base);
            for (const auto &d : kOptionU32)
                o.fields.push_back({d.name, Options::Kind::U32, u32(p, base + d.ram - 8)});
            for (const auto &d : kOptionU8)
                o.fields.push_back({d.name, Options::Kind::U8, p[base + d.ram - 8]});
            for (const auto &d : kOptionF32)
                o.fields.push_back({d.name, Options::Kind::F32Raw, u32(p, base + d.ram - 8)}); // raw: the *_volume_raw script props read it as int
            o.gameZone = u32(p, base + 0x11C0);
            o.lanMachineName = cstr(p, base + 0x1290, 32);
            return o;
        }

        std::vector<int> bitsSet(const uint8_t *b, int nbits)
        {
            std::vector<int> out;
            for (int i = 0; i < nbits; ++i)
                if ((b[i >> 3] >> (i & 7)) & 1)
                    out.push_back(i);
            return out;
        }
    }

    // ====================================================================== small public helpers
    std::string hex32(uint32_t v) { return fmt("0x%08x", v); }

    const char *dayEventName(uint8_t type) { return type < 9 ? kDayEventTypes[type] : nullptr; }
    const char *getCarHowName(uint8_t how)
    {
        switch (how)
        {
        case 1: return "new";
        case 2: return "used";
        case 3: return "trade";
        case 4: return "present";
        default: return nullptr;
        }
    }
    const char *resultName(uint32_t r) { return r < 16 ? kResult[r] : nullptr; }
    std::string unitTypeName(uint32_t t) { return t < 4 ? std::string(kUnitType[t]) : fmt("?%u", t); }
    const char *slotKindName(SlotKind k)
    {
        switch (k)
        {
        case SlotKind::Car: return "car";
        case SlotKind::Raw: return "raw";
        case SlotKind::Missing: return "missing";
        default: return "noise_but_indexed";
        }
    }
    const char *levelName(Level l) { return l == Level::Error ? "ERROR" : l == Level::Warn ? "WARN" : "INFO"; }
    std::string CourseEntry::date() const { return fmt("%04d-%02d-%02d", int(year), int(month), int(day)); }
    std::string LicenceEntry::date() const { return fmt("%04d-%02d-%02d", int(year), int(month), int(day)); }

    std::optional<uint32_t> Options::get(std::string_view name) const
    {
        for (const Field &f : fields)
            if (name == f.name)
                return f.value;
        if (name == "game_zone")
            return gameZone;
        if (name == "plain_size")
            return plainSize;
        return std::nullopt;
    }

    std::optional<std::string> dayNumberToDate(int64_t dayNumber)
    {
        // Python date.fromordinal(n - 1721424): ordinal 1 = 0001-01-01, valid up to 9999-12-31.
        const int64_t ordinal = dayNumber - 1721424;
        if (ordinal < 1 || ordinal > 3652059)
            return std::nullopt;
        int64_t z = ordinal - 1 - 719162 + 719468; // days since 1970-01-01, then shifted to 0000-03-01
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const int64_t doe = z - era * 146097;
        const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t y = yoe + era * 400;
        const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const int64_t mp = (5 * doy + 2) / 153;
        const int64_t d = doy - (153 * mp + 2) / 5 + 1;
        const int64_t m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2)
            ++y;
        return fmt("%04lld-%02lld-%02lld", (long long)y, (long long)m, (long long)d);
    }

    GarageHeader garageHeaderFields(const uint8_t *h)
    {
        GarageHeader g;
        g.money = (uint64_t(u32(h, 0)) << 32) | u32(h, 4);
        for (int i = 0; i < 9; ++i)
            g.w2_10[i] = u32(h, 8 + 4 * i);
        g.counterA = u32(h, 0x2C);
        g.counterB = u32(h, 0x30);
        g.noncePrev = u32(h, 0x34);
        g.key = u32(h, 0x38);
        g.nonce = u32(h, 0x3C);
        return g;
    }

    bool garageHeaderValid(const GarageHeader &h, uint32_t key, uint32_t counter, uint32_t nonce)
    {
        return h.key == key && (counter == h.counterA || counter == h.counterB) && (nonce == 0 || nonce == h.noncePrev || nonce == h.nonce);
    }

    Car parseCar(const uint8_t *c)
    {
        Car car;
        for (int k = 0; k < 3; ++k)
            car.blocks[k] = parseBlock(c + kBlock * size_t(k));
        car.wheel = code(u64(c, 0x468));          // 0x4409c8 getCurrentWheel
        car.activeBlock = u32(c, 0x488);          // 0x441248; changed only by the script property 'slot'
        std::memcpy(car.ownedPartsBits.data(), c + 0x48C, 12); // 0x43f470 / 0x43f4a8
        car.ownedPartsCount = 0;
        for (uint8_t x : car.ownedPartsBits)
            for (int b = 0; b < 8; ++b)
                car.ownedPartsCount += (x >> b) & 1;
        car.odometer = u32(c, 0x498);             // setter 0x3434a8 clamps to 100,000,000
        car.unknown49c = f32(c, 0x49C);
        car.oil = f32(c, 0x4A0);                  // script 'oil' (0x343698)
        car.dirtiness = f32(c, 0x4A4);            // script 'dirtiness' (0x3438a0: max 0.8)
        car.bodyLife = f32(c, 0x4A8);             // script 'bodyLife' (0x343978)
        car.wearFrozen = c[0x4AC];
        std::memcpy(car.unknown470.data(), c + 0x470, 24);
        return car;
    }

    Fields parseFields(const std::vector<uint8_t> &plain, const std::map<int, std::vector<uint8_t>> &slots)
    {
        const uint8_t *p = plain.data();
        Fields f;
        f.profile = parseProfile(p);
        f.calendar = parseCalendar(p);
        f.garageFields.ridingSlot = s32(p, kOffGarageFields);
        f.garageFields.garageKey = u32(p, kOffGarageFields + 4);
        for (int k = 0; k < 4; ++k)
            f.garageFields.unknown81d0[k] = u32(p, kOffGarageFields + 8 + 4 * size_t(k));
        f.garageFields.saveCounter = u32(p, kOffGarageFields + 24);
        for (int i = 0; i < kGarageSlots; ++i)
        {
            GarageEntry e = parseGarageEntry(p + kOffGarageIndex + 32 * size_t(i), i);
            if (e.code)
                f.garageIndex.push_back(e);
        }
        f.currentCar = parseCar(p + kOffCurrentCar);
        for (const auto &[s, b] : slots)
            if (b.size() >= kCarBytes)
                f.slotCars[s] = parseCar(b.data());
        for (int i = 0; i < 768; ++i)
        {
            const uint32_t w = u32(p, kOffRaceRecord + 4 * size_t(i));
            if ((w & 0xFFFF) == 0xFFF0 && (w >> 16) == 0)
                continue; // untouched unit (0x430958 init value)
            RaceUnit u;
            u.row = i;
            u.raw = w;
            f.raceRecord.push_back(u);
        }
        for (int i = 0; i < 128; ++i)
            f.courseRecord.push_back(parseCourseUnit(p + kOffCourseRecord + kCourseUnit * size_t(i), i));
        for (int i = 0; i < 4; ++i)
            f.machineRecord.push_back(parseCourseUnit(p + kOffMachineRecord + kCourseUnit * size_t(i), i));
        f.licenseRecord = parseLicenceRecords(p);
        f.available.cars = codeList(p, kOffAvailable, 1024);
        f.available.courses = codeList(p, kOffAvailable + 8192, 128);
        f.available.tailU32 = u32(p, kOffAvailable + 9216);
        for (auto [fav, off] : {std::pair<Favorite *, size_t>{&f.favoriteCar, kOffFavoriteCar}, {&f.favoriteCourse, kOffFavoriteCourse}})
        {
            fav->limit = u32(p, off);
            fav->count = u32(p, off + 4);
            fav->codes = codeList(p, off + 8, std::min<uint32_t>(fav->count, 32));
        }
        for (int i : bitsSet(p + kOffPresent, 928))
            (i < 868 ? f.present.byRace : f.present.byTime).push_back(i < 868 ? i : i - 868);
        {
            const uint8_t *b = p + kOffChampionship;
            f.championship.raceCode = cstr(b, 0, 32);
            f.championship.suspended = b[0x20];
            f.championship.session = b[0x21];
            for (int i = 0x22; i < 136; ++i)
                if (b[i])
                    f.championship.restNonzero.push_back(i);
        }
        f.usedCar.sold = bitsSet(p + kOffUsedCar, 256);
        f.usedCar.week = u32(p, kOffUsedCar + 32);
        f.option = parseOptions(p);
        f.context.project = cstr(p, kOffContext, 32);
        f.context.page = cstr(p, kOffContext + 32, 32);
        // gtmode.adc: cached_aspec_score = race_score (every event sub-race) + mission_score
        // (l0m0001..0034) + family_score; it sums the byte whatever the unit's type.
        for (const RaceUnit &u : f.raceRecord)
        {
            const uint32_t t = u.typeRaw();
            if (t == 2)
            {
                f.aspec.events += u.bestScore();
                ++f.aspec.eventsRecorded;
                if (u.best() == 0)
                    ++f.aspec.eventsWon;
            }
            else if (t == 3)
                f.aspec.missions += u.bestScore();
            else if (t != 1)
                f.aspec.untyped += u.bestScore();
        }
        return f;
    }

    // ====================================================================== invariants (gt4save_fields.check)
    std::vector<Check> fieldChecks(const Fields &f, const std::map<int, SlotKind> &slotStatus, const SpecDb *db)
    {
        std::vector<Check> m;
        auto add = [&](Level l, std::string s) { m.push_back({l, std::move(s)}); };
        const Calendar &cal = f.calendar;
        const int64_t daysPos = std::max<int64_t>(cal.days, 0);
        if (cal.count > 2922)
            add(Level::Error, fmt("calendar count %u > 2922", cal.count));
        if (cal.days < 0)
            add(Level::Error, fmt("today is before 2005-04-02 (day %lld)", (long long)cal.days));
        if (int64_t(cal.count) > daysPos)
            add(Level::Warn, fmt("calendar holds %u days of events but only %lld days have passed", cal.count, (long long)cal.days));
        std::map<int, const GarageEntry *> idx;
        for (const GarageEntry &e : f.garageIndex)
            idx[e.slot] = &e;
        const int32_t r = f.garageFields.ridingSlot;
        if (!(-1 <= r && r < 1000))
            add(Level::Error, fmt("riding slot %d out of range", r));
        else if (r >= 0 && (!idx.count(r) || !idx[r]->inGarage))
            add(Level::Error, fmt("riding slot %d has no car in the garage index", r));
        else if (r >= 0 && f.currentCar.active().car != idx[r]->code)
            add(Level::Warn, fmt("current car code differs from garage index entry %d", r));
        const bool haveCarTable = db && db->hasTable(0);
        for (const auto &[s, e] : idx)
        {
            const auto st = slotStatus.find(s);
            if (e->inGarage && e->storedParams && s != r && (st == slotStatus.end() || st->second != SlotKind::Car))
                add(Level::Error, fmt("garage entry %d says its parameters are stored but slot %d does not decrypt "
                                      "(GT4 reports INVALID_FILE when the car is taken out)", s, s));
            if (e->code && (*e->code >> 32) != 0)
                add(Level::Error, fmt("garage entry %d holds a non-car spec code 0x%llx", s, (unsigned long long)*e->code));
            if (haveCarTable && e->code && !db->hasCode(0, uint32_t(*e->code & 0xFFFFFFFFu)))
                add(Level::Warn, fmt("garage entry %d: car code %llu not in GENERIC_CAR", s, (unsigned long long)*e->code));
            if (int64_t(e->dayAdded) > daysPos)
                add(Level::Warn, fmt("garage entry %d added on day %u, after today (%lld)", s, e->dayAdded, (long long)cal.days));
        }
        for (const auto &[s, st] : slotStatus)
            if (st == SlotKind::Car && (!idx.count(s) || !idx[s]->storedParams))
                add(Level::Info, fmt("garage slot %d holds car data that the index does not use (left by a sold/moved car)", s));
        std::vector<std::pair<std::string, const Car *>> cars;
        for (const auto &[s, c] : f.slotCars)
            cars.push_back({fmt("slot %d", s), &c});
        if (r >= 0) // a new game has no riding car: the current car is empty
            cars.insert(cars.begin(), {"current car", &f.currentCar});
        else if (f.currentCar.active().car)
            add(Level::Warn, "no riding car, but the current car record holds a car");
        for (const auto &[name, car] : cars)
        {
            const char *nm = name.c_str();
            if (car->activeBlock >= 3)
                add(Level::Error, fmt("%s: active parameter block %u not in 0..2", nm, car->activeBlock));
            if (car->odometer > 100000000u)
                add(Level::Warn, fmt("%s: odometer above GT4's clamp (100,000,000)", nm));
            if (double(car->dirtiness) > 0.8 + 1e-6)
                add(Level::Warn, fmt("%s: dirtiness above GT4's clamp (0.8)", nm));
            for (int k = 0; k < 3; ++k)
            {
                const Block &b = car->blocks[k];
                if (b.car != car->blocks[0].car)
                    add(Level::Warn, fmt("%s: block %d is a different car", nm, k));
                if (!b.car || (*b.car >> 32) != 0)
                    add(Level::Error, fmt("%s: block %d holds no car code", nm, k));
                if (b.variation && (*b.variation >> 32) != 42)
                    add(Level::Warn, fmt("%s: block %d variation code is not a VARIATION entry", nm, k));
                const std::optional<int> n = db ? db->colours(b.variation) : std::nullopt;
                if (n && !(0 <= b.colour && b.colour < *n))
                    add(Level::Warn, fmt("%s: block %d colour %d outside 0..%d", nm, k, int(b.colour), *n - 1));
                for (uint64_t c : b.parts)
                {
                    const uint64_t t = c >> 32;
                    if (!(1 <= t && t <= 33))
                        add(Level::Warn, fmt("%s: block %d part code 0x%llx is not a parts table", nm, k, (unsigned long long)c));
                    else if (db && db->hasTable(uint32_t(t)) && !db->hasCode(uint32_t(t), uint32_t(c & 0xFFFFFFFFu)))
                        add(Level::Warn, fmt("%s: block %d part %u not in table %u", nm, k, uint32_t(c & 0xFFFFFFFFu), uint32_t(t)));
                }
            }
        }
        for (const RaceUnit &u : f.raceRecord)
        {
            if (u.typeRaw() > 3)
                add(Level::Warn, fmt("race record row %d has type %u (not 0..3): leave it alone", u.row, u.typeRaw()));
            else if (u.typeRaw() == 0 && (u.raw & 0xFFFF) != 0xFFF0)
                add(Level::Warn, fmt("race record row %d has results but no type", u.row));
            else if (u.typeRaw() == 0 && u.bestScore())
                add(Level::Info, fmt("race record row %d has a best score (%u) without a result", u.row, u.bestScore()));
        }
        for (auto [nm, lst] : {std::pair<const char *, const std::vector<uint64_t> *>{"cars", &f.available.cars}, {"courses", &f.available.courses}})
        {
            std::vector<uint64_t> s(lst->begin(), lst->end());
            std::sort(s.begin(), s.end());
            s.erase(std::unique(s.begin(), s.end()), s.end());
            if (*lst != s)
                add(Level::Error, fmt("available %s list is not sorted/unique (GT4 inserts sorted, 0x42e968)", nm));
        }
        for (auto [nm, fav] : {std::pair<const char *, const Favorite *>{"favorite_car", &f.favoriteCar}, {"favorite_course", &f.favoriteCourse}})
            if (fav->limit > 32 || fav->count > fav->limit)
                add(Level::Error, fmt("%s: count %u / limit %u invalid", nm, fav->count, fav->limit));
        if (f.option.plainSize != 4544)
            add(Level::Warn, fmt("option block size field is %u (4544 expected)", f.option.plainSize));
        return m;
    }

    std::optional<LicenceStatus> licenceStatus(const Fields &f, const SpecDb *db)
    {
        if (!db || db->raceRows.empty())
            return std::nullopt;
        std::map<int, const RaceUnit *> units;
        for (const RaceUnit &u : f.raceRecord)
            units[u.row] = &u;
        auto unitFor = [&](const std::string &label) -> const RaceUnit * {
            const std::optional<int> row = db->raceRow(label);
            if (!row)
                return nullptr;
            const auto it = units.find(*row);
            return it == units.end() ? nullptr : it->second;
        };
        LicenceStatus st;
        static const std::pair<const char *, const char *> cats[] = {{"B", "l0b"}, {"A", "l0a"}, {"IB", "lib"}, {"IA", "lia"}, {"S", "l0s"}};
        for (const auto &[cat, pre] : cats)
        {
            LicenceStatus::Licence lic;
            lic.name = cat;
            for (int i = 1; i <= 16; ++i)
            {
                const RaceUnit *u = unitFor(fmt("%s%04d", pre, i));
                lic.tests[i - 1] = u && u->typeRaw() == 1 ? kResult[u->best()] : "none";
            }
            lic.passed = lic.allGold = true;
            for (const std::string &t : lic.tests)
            {
                if (!(t == "gold" || t == "silver" || t == "bronze" || t == "red"))
                    lic.passed = false; // 0x4310f0: best < 13
                if (t != "gold")
                    lic.allGold = false; // 0x430f90
                lic.gold += t == "gold";
                lic.silver += t == "silver";
                lic.bronze += t == "bronze";
            }
            lic.score = 3 * lic.gold + 2 * lic.silver + lic.bronze;
            st.licences.push_back(lic);
        }
        for (int i = 1; i <= 5; ++i)
        {
            const RaceUnit *u = unitFor(fmt("l0c%04d", i));
            st.coffeeBreaks[i - 1] = u ? kResult[u->best()] : "none";
        }
        for (int i = 1; i <= 34; ++i)
        {
            const RaceUnit *u = unitFor(fmt("l0m%04d", i));
            st.missions[i - 1] = u ? kResult[u->best()] : "none";
        }
        return st;
    }

    // ====================================================================== the save
    bool Save::gt4LoadChecksPass() const
    {
        const InnerHeader &in = gamedata.inner;
        bool ok = loaded && gamedata.outerCrcOk && in.magicOk && in.lenOk && in.crcOk;
        if (garage)
            ok = ok && garage->validForGamedata;
        return ok;
    }
    bool Save::hasErrors() const
    {
        return !loaded || std::any_of(checks.begin(), checks.end(), [](const Check &c) { return c.level == Level::Error; });
    }
    bool Save::appendBugHit() const
    {
        return loaded && (gamedata.copies > 1 || (garage && (garage->size > garage->expectedSize || !garage->trailing.empty())));
    }
    int Save::carCount() const
    {
        int n = 0;
        for (const GarageEntry &e : fields.garageIndex)
            n += e.inGarage ? 1 : 0;
        return n;
    }

    Save load(const CardFiles &card, const std::string &folder, const SpecDb *db, Rounding rounding)
    {
        Save s;
        s.rounding = rounding;
        s.source = card.describe() + (folder.empty() ? std::string() : " / " + folder);
        std::string err;
        std::vector<uint8_t> gd;
        if (!card.read(folder, kGamedataFile, gd, err))
        {
            s.error = "no GT4 save data: " + err;
            return s;
        }
        // ---- GAMEDATA (the recomp's O_APPEND bug appends each re-save: the newest copy is the last one)
        GamedataInfo &g = s.gamedata;
        g.size = gd.size();
        g.copies = (!gd.empty() && gd.size() % kGamedataBytes == 0) ? uint32_t(gd.size() / kGamedataBytes) : 1;
        const uint8_t *raw = gd.data();
        size_t rawLen = gd.size();
        if (g.copies > 1)
        {
            raw = gd.data() + gd.size() - kGamedataBytes;
            rawLen = kGamedataBytes;
            s.dec.gamedataPrefix.assign(gd.begin(), gd.end() - std::ptrdiff_t(kGamedataBytes));
        }
        EnvelopeResult env = envDecrypt(raw, rawLen, kGamedataEnvelope, rounding);
        if (!env.ok || env.payload.size() < kOffEnd)
        {
            s.error = fmt("%s is not a GT4 save file (%llu bytes)", kGamedataFile, (unsigned long long)gd.size());
            return s;
        }
        g.seed = env.seed;
        g.outerCrc = env.crc;
        g.outerCrcStored = env.crcStored;
        g.outerCrcOk = env.crcOk;
        g.swapA = env.swapA;
        g.swapB = env.swapB;
        for (uint32_t k = 0; k + 1 < g.copies; ++k)
            g.olderCopiesOk.push_back(envDecrypt(gd.data() + k * kGamedataBytes, kGamedataBytes, kGamedataEnvelope, rounding).crcOk);
        const std::vector<uint8_t> &plain = env.payload;
        const uint8_t *p = plain.data();
        // inner header (0x43aa68 checks magic, CRC and length)
        InnerHeader &in = g.inner;
        in.magic = u32(p, 0);
        in.bodyLen = uint32_t(0u - u32(p, 4));
        in.crcStored = u32(p, 8);
        in.word3 = u32(p, 12);
        in.magicOk = in.magic == kSerialMagic;
        in.lenOk = in.bodyLen == kGamedataBody;
        if (16 + uint64_t(in.bodyLen) <= plain.size())
            in.crc = crc32(p + 16, in.bodyLen);
        in.crcOk = in.crc && *in.crc == in.crcStored;
        // summary (gt4save.gamedata_summary)
        g.profileName = cstr(p, kOffProfile, 32);
        g.calendarToday = u32(p, kOffToday);
        g.calendarDate = dayNumberToDate(g.calendarToday);
        g.days = int64_t(g.calendarToday) - int64_t(kDay0);
        g.calendarEntries = u32(p, kOffCalendar);
        g.garageCurrentSlot = s32(p, kOffGarageFields);
        g.garageKey = u32(p, kOffGarageFields + 4);
        for (int k = 0; k < 4; ++k)
            g.garageUnknown[k] = u32(p, kOffGarageFields + 8 + 4 * size_t(k));
        g.garageCounter = u32(p, kOffGarageFields + 24);
        std::set<int> usedIdx;
        for (int i = 0; i < kGarageSlots; ++i)
        {
            const uint8_t *e = p + kOffGarageIndex + 32 * size_t(i);
            const uint64_t carId = u64(e, 0);
            if (carId != kNone64)
            {
                g.indexUsed.push_back({i, carId, u64(e, 8), u64(e, 16), u64(e, 24)});
                usedIdx.insert(i);
            }
        }
        const uint32_t key = u32(p, kOffGarageKey), counter = u32(p, kOffGarageCounter);
        s.dec.gamedataPlain = plain;

        // ---- garage
        std::map<int, std::vector<uint8_t>> slotBytes;
        std::map<int, SlotKind> slotStatus;
        if (card.exists(folder, kGarageFile))
        {
            std::vector<uint8_t> gr;
            if (!card.read(folder, kGarageFile, gr, err))
            {
                s.error = "cannot read the garage file: " + err;
                return s;
            }
            GarageInfo ga;
            ga.size = gr.size();
            s.dec.hasGarage = true;
            if (gr.size() >= 64)
            {
                const std::array<uint8_t, 64> hdr = garageHeaderDecrypt(gr.data(), key, rounding);
                s.dec.garageHeader = hdr;
                ga.header = garageHeaderFields(hdr.data());
                ga.keyOk = ga.header.key == key;
                ga.validForGamedata = garageHeaderValid(ga.header, key, counter);
            }
            const std::vector<uint8_t> noise = garageNoise(key, kGarageBytes);
            for (int sl = 0; sl < kGarageSlots; ++sl)
            {
                const size_t a = 64 + size_t(sl) * kSlotBytes;
                if (a + kSlotBytes > gr.size())
                {
                    ga.slots.push_back({sl, SlotKind::Missing});
                    continue;
                }
                if (std::memcmp(gr.data() + a, noise.data() + a, kSlotBytes) == 0)
                {
                    ++ga.noiseSlots;
                    if (usedIdx.count(sl))
                        ga.slots.push_back({sl, SlotKind::NoiseButIndexed});
                    continue; // never written since the file was created
                }
                SlotDecrypt d = garageSlotDecrypt(gr.data() + a, kSlotBytes, key, uint32_t(sl), rounding);
                if (d.crcOk)
                {
                    ga.slots.push_back({sl, SlotKind::Car, d.seed, d.crc, usedIdx.count(sl) != 0});
                    slotBytes[sl] = d.plain;
                    s.dec.slotPlain[sl] = {d.seed, std::move(d.plain)};
                }
                else
                {
                    ga.slots.push_back({sl, SlotKind::Raw});
                    s.dec.slotRaw[sl].assign(gr.data() + a, gr.data() + a + kSlotBytes);
                }
            }
            // bytes past the 1,000 slots: GT4's own maths never puts them there (append bug)
            if (gr.size() > kGarageBytes)
            {
                const uint8_t *tail = gr.data() + kGarageBytes;
                const size_t tailLen = gr.size() - kGarageBytes;
                size_t pos = 0;
                while (pos < tailLen)
                {
                    if (tailLen - pos >= 64)
                    {
                        const std::array<uint8_t, 64> th = garageHeaderDecrypt(tail + pos, key, rounding);
                        const GarageHeader tf = garageHeaderFields(th.data());
                        if (tf.key == key && std::memcmp(garageHeaderEncrypt(th.data(), rounding).data(), tail + pos, 64) == 0)
                        {
                            TrailingWrite t;
                            t.offset = kGarageBytes + pos;
                            t.kind = TrailingKind::Header;
                            t.header = tf;
                            ga.trailing.push_back(t);
                            s.dec.trailingHeaders.push_back(th);
                            pos += 64;
                            continue;
                        }
                    }
                    if (tailLen - pos >= kSlotBytes)
                    {
                        // a car slot written past the end: find which slot it was meant for
                        std::vector<int> cand;
                        for (const SlotState &x : ga.slots)
                            if (x.kind == SlotKind::NoiseButIndexed)
                                cand.push_back(x.slot);
                        const std::set<int> first(cand.begin(), cand.end());
                        for (int i = 0; i < kGarageSlots; ++i)
                            if (!first.count(i))
                                cand.push_back(i);
                        bool hit = false;
                        for (int i : cand)
                        {
                            SlotDecrypt d = garageSlotDecrypt(tail + pos, kSlotBytes, key, uint32_t(i), rounding);
                            if (d.crcOk)
                            {
                                TrailingWrite t;
                                t.offset = kGarageBytes + pos;
                                t.kind = TrailingKind::Car;
                                t.slot = i;
                                t.seed = d.seed;
                                t.crc = d.crc;
                                ga.trailing.push_back(t);
                                s.dec.trailingCars.push_back(std::move(d.plain));
                                hit = true;
                                break;
                            }
                        }
                        if (hit)
                        {
                            pos += kSlotBytes;
                            continue;
                        }
                    }
                    TrailingWrite t;
                    t.offset = kGarageBytes + pos;
                    t.kind = TrailingKind::Raw;
                    t.length = tailLen - pos;
                    ga.trailing.push_back(t);
                    s.dec.trailingRaw.emplace_back(tail + pos, tail + tailLen);
                    break;
                }
            }
            for (const SlotState &x : ga.slots)
                slotStatus[x.slot] = x.kind;
            s.garage = std::move(ga);
        }
        for (const char *name : {"icon.sys", "icon0.ico"})
        {
            std::vector<uint8_t> b;
            if (card.exists(folder, name) && card.read(folder, name, b, err))
                s.dec.otherFiles.push_back({name, std::move(b)});
        }

        // ---- fields and checks (gt4save_report.all_checks)
        s.fields = parseFields(plain, slotBytes);
        std::vector<Check> &c = s.checks;
        if (!g.outerCrcOk)
            c.push_back({Level::Error, "GAMEDATA envelope CRC fails (GT4: INVALID_FILE)"});
        if (!(in.magicOk && in.lenOk && in.crcOk))
            c.push_back({Level::Error, "GAMEDATA inner header/CRC fails (GT4: INVALID_FILE)"});
        if (g.copies > 1)
            c.push_back({Level::Error, fmt("GAMEDATA holds %u appended copies (recomp append bug); run gt4save.py repair", g.copies)});
        if (!s.garage)
            c.push_back({Level::Error, "no garage file"});
        else
        {
            const GarageInfo &ga = *s.garage;
            if (!ga.validForGamedata)
                c.push_back({Level::Error, "garage header does not match GAMEDATA key/counter (GT4: DIFFERENT_MEMORYCARD)"});
            if (ga.size != ga.expectedSize || !ga.trailing.empty())
                c.push_back({Level::Error, fmt("garage file is %llu bytes, %llu expected (append bug); run gt4save.py repair",
                                               (unsigned long long)ga.size, (unsigned long long)ga.expectedSize)});
            if (ga.header.money > 99999999u)
                c.push_back({Level::Warn, fmt("credits %llu above 99,999,999", (unsigned long long)ga.header.money)});
        }
        std::vector<Check> fc = fieldChecks(s.fields, slotStatus, db);
        c.insert(c.end(), fc.begin(), fc.end());
        s.licences = licenceStatus(s.fields, db);
        s.loaded = true;
        return s;
    }

    Save loadCardFolder(const fs::path &cardDir, const SpecDb *db, Rounding rounding)
    {
        return load(FolderCard(cardDir), kSaveFolder, db, rounding);
    }

    Save loadSaveFolder(const fs::path &saveDir, const SpecDb *db, Rounding rounding)
    {
        return load(FolderCard(saveDir), "", db, rounding);
    }

    Save loadCardFile(const fs::path &ps2, const SpecDb *db, Rounding rounding)
    {
        std::string err;
        std::unique_ptr<Ps2CardImage> card = Ps2CardImage::open(ps2, err);
        if (!card)
        {
            Save s;
            s.rounding = rounding;
            s.error = err;
            return s;
        }
        return load(*card, kSaveFolder, db, rounding);
    }

    // ====================================================================== folder card
    namespace
    {
        fs::path folderFile(const fs::path &dir, const std::string &folder, const std::string &name)
        {
            // Card folder and file names are plain ASCII (the PS2 card's 32-byte names).
            return folder.empty() ? dir / fs::path(name) : dir / fs::path(folder) / fs::path(name);
        }
    }
    bool FolderCard::exists(const std::string &folder, const std::string &name) const
    {
        std::error_code ec;
        return fs::is_regular_file(folderFile(dir_, folder, name), ec);
    }
    bool FolderCard::read(const std::string &folder, const std::string &name, std::vector<uint8_t> &out, std::string &error) const
    {
        const fs::path p = folderFile(dir_, folder, name);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec))
        {
            error = name + " not found in " + describe() + (folder.empty() ? "" : " / " + folder);
            return false;
        }
        if (!readFile(p, out))
        {
            error = "cannot read " + name;
            return false;
        }
        return true;
    }
    std::string FolderCard::describe() const
    {
        const auto u = dir_.u8string();
        return "folder " + std::string(u.begin(), u.end());
    }

    // ====================================================================== .ps2 card image
    std::unique_ptr<Ps2CardImage> Ps2CardImage::open(const fs::path &file, std::string &error)
    {
        std::vector<uint8_t> image;
        if (!readFile(file, image))
        {
            error = "cannot read the card file";
            return nullptr;
        }
        std::unique_ptr<Ps2CardImage> c = fromImage(std::move(image), error);
        if (c)
        {
            const auto u = file.u8string();
            c->describe_ = "card file " + std::string(u.begin(), u.end());
        }
        return c;
    }

    std::unique_ptr<Ps2CardImage> Ps2CardImage::fromImage(std::vector<uint8_t> image, std::string &error)
    {
        std::unique_ptr<Ps2CardImage> c(new Ps2CardImage());
        c->image_ = std::move(image);
        c->describe_ = "card image";
        if (!c->parse(error))
            return nullptr;
        return c;
    }

    bool Ps2CardImage::parse(std::string &error)
    {
        if (image_.size() < 0x154 || std::memcmp(image_.data(), "Sony PS2 Memory Card Format ", 28) != 0)
        {
            error = "not a formatted PS2 memory card image";
            return false;
        }
        const uint8_t *sb = image_.data();
        pageLen_ = u16(sb, 0x28);
        pagesPerCluster_ = u16(sb, 0x2A);
        allocOffset_ = u32(sb, 0x34);
        allocEnd_ = u32(sb, 0x38);
        root_ = u32(sb, 0x3C);
        if (pageLen_ == 0 || pageLen_ > 4096 || pagesPerCluster_ == 0 || pagesPerCluster_ > 16)
        {
            error = "damaged card (superblock)";
            return false;
        }
        // 512-byte pages, each followed by 16 ECC bytes (PCSX2's 8,650,752-byte cards), or without ECC.
        rawPage_ = image_.size() % (pageLen_ + 16) == 0 ? pageLen_ + 16 : pageLen_;
        // The FAT, as MCMAN reads it: indirect FAT clusters listed in the superblock (ifc_list at 0x50).
        const size_t clusterBytes = pageLen_ * pagesPerCluster_;
        const size_t perCluster = clusterBytes / 4;
        std::vector<uint8_t> c(clusterBytes);
        std::vector<uint32_t> fatClusters;
        for (int i = 0; i < 32; ++i)
        {
            const uint32_t ifc = u32(sb, 0x50 + 4 * size_t(i));
            if (ifc == 0 && i > 0)
                break;
            if ((size_t(ifc) + 1) * pagesPerCluster_ * rawPage_ > image_.size())
                break;
            cluster(ifc, c.data());
            for (size_t k = 0; k < perCluster && fatClusters.size() * perCluster < allocEnd_; ++k)
            {
                const uint32_t e = u32(c.data(), 4 * k);
                if (e == 0xFFFFFFFFu)
                    break;
                fatClusters.push_back(e);
            }
            if (fatClusters.size() * perCluster >= allocEnd_)
                break;
        }
        for (uint32_t fc : fatClusters)
        {
            if ((size_t(fc) + 1) * pagesPerCluster_ * rawPage_ > image_.size())
            {
                error = "damaged card (FAT)";
                return false;
            }
            cluster(fc, c.data());
            for (size_t k = 0; k < perCluster; ++k)
                fat_.push_back(u32(c.data(), 4 * k));
        }
        if (fat_.size() < allocEnd_)
        {
            error = "damaged card (FAT size)";
            return false;
        }
        return true;
    }

    void Ps2CardImage::cluster(uint32_t abs, uint8_t *out) const
    {
        for (size_t i = 0; i < pagesPerCluster_; ++i)
        {
            const size_t o = (size_t(abs) * pagesPerCluster_ + i) * rawPage_;
            if (o + pageLen_ <= image_.size())
                std::memcpy(out + i * pageLen_, image_.data() + o, pageLen_);
            else
                std::memset(out + i * pageLen_, 0, pageLen_);
        }
    }

    std::vector<uint8_t> Ps2CardImage::data(uint32_t first, size_t length) const
    {
        std::vector<uint8_t> out;
        const size_t clusterBytes = pageLen_ * pagesPerCluster_;
        std::vector<uint8_t> c(clusterBytes);
        std::set<uint32_t> seen;
        uint32_t cl = first;
        while (out.size() < length)
        {
            if (cl >= allocEnd_ || !seen.insert(cl).second)
                break; // outside the allocatable area, or a FAT loop
            cluster(allocOffset_ + cl, c.data());
            out.insert(out.end(), c.begin(), c.end());
            const uint32_t e = fat_[cl];
            if (e == 0xFFFFFFFFu || !(e & 0x80000000u))
                break;
            cl = e & 0x7FFFFFFFu;
        }
        if (out.size() > length)
            out.resize(length);
        return out;
    }

    std::vector<Ps2CardImage::Entry> Ps2CardImage::list(uint32_t first, uint32_t count) const
    {
        std::vector<Entry> out;
        const std::vector<uint8_t> raw = data(first, size_t(std::min<uint32_t>(count, 4096)) * 512);
        for (size_t i = 0; i + 512 <= raw.size(); i += 512)
        {
            const uint8_t *e = raw.data() + i;
            Entry x;
            x.mode = u16(e, 0);
            x.length = u32(e, 4);
            x.cluster = u32(e, 0x10);
            x.name = cstr(e, 0x40, 32);
            out.push_back(x);
        }
        return out;
    }

    std::vector<Ps2CardImage::Entry> Ps2CardImage::folders() const
    {
        std::vector<Entry> out;
        const std::vector<Entry> dot = list(root_, 1);
        if (dot.empty())
            return out;
        for (const Entry &e : list(root_, dot[0].length))
            if ((e.mode & 0x8000) && (e.mode & 0x20) && e.name != "." && e.name != "..")
                out.push_back(e);
        return out;
    }

    std::vector<Ps2CardImage::Entry> Ps2CardImage::files(const std::string &folder) const
    {
        std::vector<Entry> out;
        for (const Entry &d : folders())
            if (d.name == folder)
            {
                for (const Entry &f : list(d.cluster, d.length))
                    if ((f.mode & 0x8000) && (f.mode & 0x10) && f.name != "." && f.name != "..")
                        out.push_back(f);
                break;
            }
        return out;
    }

    bool Ps2CardImage::exists(const std::string &folder, const std::string &name) const
    {
        for (const Entry &f : files(folder))
            if (f.name == name)
                return true;
        return false;
    }

    bool Ps2CardImage::read(const std::string &folder, const std::string &name, std::vector<uint8_t> &out, std::string &error) const
    {
        for (const Entry &f : files(folder))
            if (f.name == name)
            {
                out = data(f.cluster, f.length);
                if (out.size() != f.length)
                {
                    error = name + " is damaged on the card (" + std::to_string(out.size()) + " of " + std::to_string(f.length) + " bytes)";
                    return false;
                }
                return true;
            }
        error = name + " not found in " + folder + " on the " + describe_;
        return false;
    }
}
