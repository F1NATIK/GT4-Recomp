#pragma once

// gt4save - read-only GT4 (US, SCUS-97328) save reader for the GT4 Recompilation launcher.
//
// A dependency-free C++20 port of the Python tools that decrypt GT4's memory-card save and map its
// fields:
//   analysis/saves/save-crypto/gt4save.py, gt4save_crypto.py   (layers, CRCs; GT4-SAVE-FORMAT.md)
//   analysis/design/save-editor/gt4save_fields.py, gt4save_report.py, gt4specdb.py (SAVE-EDITOR.md)
// The Python tools stay the reference: the tests in ../tests check that this library decrypts to the
// same bytes, re-encrypts to the original files, and reports the same value for every field.
//
// What the launcher gets: load a save (from a folder card, a .ps2 card image, or any card reader that
// implements CardFiles), and read plain structs: credits, player name, days, the garage with every
// car's parts and setup, event results, licences, records, unlocks, options, plus GT4's own load
// checks. Nothing here writes to a card or a save. (Re-encryption exists only for the tests, in
// gt4save_roundtrip.h.)
//
// Names of cars, parts, races and courses are the spec database labels ("s13_ks_88"), read at run
// time from the GT4 disc image the launcher already knows (SpecDb::loadFromIso). No game data is
// compiled in.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gt4save
{
    namespace fs = std::filesystem;

    constexpr const char *kSaveFolder = "BASCUS-97328GAMEDATA"; // GT4's card folder, and its main file
    constexpr const char *kGamedataFile = "BASCUS-97328GAMEDATA";
    constexpr const char *kGarageFile = "garage";
    constexpr size_t kGamedataBytes = 237760;  // one GAMEDATA file (8-byte envelope header + 237,752)
    constexpr size_t kGarageBytes = 1280064;   // 64-byte header + 1,000 x 1,280-byte car slots
    constexpr int kGarageSlots = 1000;
    constexpr size_t kSlotBytes = 1280;
    constexpr size_t kCarBytes = 1208;         // SPEC_DATABASE::CarGarage
    constexpr uint32_t kDay0 = 2453462;        // day number of 2005-04-02 (day 0 of GT Mode)
    constexpr uint64_t kNone64 = ~uint64_t(0); // "no code" in the save
    constexpr uint32_t kNoTime = 0x157529FF;   // 359,999,999 ms: "no time"

    // The EE rounds float results toward zero; GT4's shuffle positions depend on it. Nearest exists
    // only to show the difference (a PC float picks other positions in ~90% of garage car slots).
    enum class Rounding
    {
        TowardZero,
        Nearest
    };

    // =========================================================================== where the files come from
    // A memory card as the save reader sees it: files inside save folders. The launcher's own card
    // reader (folder card or .ps2 image) can implement this; FolderCard and Ps2CardImage below are
    // ready-made, read-only implementations.
    class CardFiles
    {
    public:
        virtual ~CardFiles() = default;
        virtual bool exists(const std::string &folder, const std::string &name) const = 0;
        // false when the file is missing or unreadable (error says which).
        virtual bool read(const std::string &folder, const std::string &name, std::vector<uint8_t> &out, std::string &error) const = 0;
        virtual std::string describe() const = 0; // for messages ("card folder C:\...\mc0")
    };

    // The runtime's folder card: <cardDir>\<folder>\<name> (e.g. game\mc0\BASCUS-97328GAMEDATA\garage).
    class FolderCard final : public CardFiles
    {
    public:
        explicit FolderCard(fs::path cardDir) : dir_(std::move(cardDir)) {}
        bool exists(const std::string &folder, const std::string &name) const override;
        bool read(const std::string &folder, const std::string &name, std::vector<uint8_t> &out, std::string &error) const override;
        std::string describe() const override;

    private:
        fs::path dir_;
    };

    // A PCSX2-style .ps2 card image (8 MB of 512-byte pages, each followed by 16 ECC bytes, or the
    // ECC-less 512-byte-page form), read only: the same superblock / FAT / directory rules as the
    // stock launcher's card reader (gt4stock_saves.cpp) and ps2card_extract.py. Deleted entries
    // (the 0x8000 "exists" bit clear) are skipped.
    class Ps2CardImage final : public CardFiles
    {
    public:
        static std::unique_ptr<Ps2CardImage> open(const fs::path &file, std::string &error);
        static std::unique_ptr<Ps2CardImage> fromImage(std::vector<uint8_t> image, std::string &error);

        struct Entry
        {
            std::string name;
            uint16_t mode = 0;
            uint32_t length = 0, cluster = 0;
        };
        std::vector<Entry> folders() const;                             // save folders in the root
        std::vector<Entry> files(const std::string &folder) const;      // files of one save folder

        bool exists(const std::string &folder, const std::string &name) const override;
        bool read(const std::string &folder, const std::string &name, std::vector<uint8_t> &out, std::string &error) const override;
        std::string describe() const override { return describe_; }

    private:
        Ps2CardImage() = default;
        bool parse(std::string &error);
        void cluster(uint32_t abs, uint8_t *out) const;
        std::vector<uint8_t> data(uint32_t first, size_t length) const;
        std::vector<Entry> list(uint32_t first, uint32_t count) const;
        std::vector<uint8_t> image_;
        std::string describe_;
        size_t pageLen_ = 512, rawPage_ = 528, pagesPerCluster_ = 2;
        uint32_t allocOffset_ = 0, allocEnd_ = 0, root_ = 0;
        std::vector<uint32_t> fat_;
    };

    // =========================================================================== spec database (names)
    // Read-only label lookup for GT4's 64-bit "spec codes" (high 32 bits = table, low 32 = row code),
    // from the disc's specdb/GT4_US2560/*.idi and *.dbt (port of gt4specdb.py). Loaded at run time.
    class SpecDb
    {
    public:
        // From the GT4 US ISO: ISO 9660 -> GT4.VOL -> its RoFS table of contents -> the compressed files.
        static std::unique_ptr<SpecDb> loadFromIso(const fs::path &iso, std::string &error);
        // From a folder holding the extracted .idi / .dbt files.
        static std::unique_ptr<SpecDb> loadFromFolder(const fs::path &dir, std::string &error);

        std::string source;                                              // the ISO or folder path
        std::map<uint32_t, std::map<uint32_t, std::string>> labels;      // table -> code -> label
        std::vector<uint32_t> raceRows;                                  // RACE.dbt code order: row i -> code
        std::map<uint32_t, int> colourCount;                             // VARIATION code -> colours

        // nullopt for kNone64; "#<code>" when the table or code is unknown.
        std::optional<std::string> label(uint64_t code64) const;
        std::optional<int> colours(std::optional<uint64_t> variation64) const;
        std::optional<std::string> raceLabel(int row) const;
        // Row of a race label with GT4's own fallbacks (0x430d20: name, "us"+name, "jp"+name).
        std::optional<int> raceRow(const std::string &label) const;
        bool hasTable(uint32_t table) const;
        bool hasCode(uint32_t table, uint32_t code) const;

    private:
        // Fills the tables from a file getter (used by both loaders).
        template <class Get>
        static std::unique_ptr<SpecDb> loadWith(Get &&get, std::string source);
        std::map<std::string, uint32_t> raceInv_; // RACE label -> code (gt4specdb.race_row's inverse map)
    };
    const char *tableName(uint32_t table);      // "GENERIC_CAR", ... (nullptr when unknown)
    std::string tableNameOr(uint64_t code64);   // tableName, or "T<id>"

    // A read-only view of the GT4 disc image: files inside GT4.VOL by path (e.g.
    // "specdb/GT4_US2560/RACE.dbt"), compressed files unpacked. Used by SpecDb::loadFromIso.
    class Gt4Disc
    {
    public:
        static std::unique_ptr<Gt4Disc> open(const fs::path &iso, std::string &error);
        // GT4_EXTRA_CARS (ps2x/iop/gt4_extra_cars_pack.h, analysis/jp-cars/README-BUILD.md): every disc opened after this
        // call serves the pack's files over the image, as GT4.exe's disc layer does (the Full launcher: its Extra cars
        // switch). An empty path: none. False with `error` when the pack cannot be read (then none), or when this build has
        // no pack reader (the Classic launcher).
        static bool useExtraCars(const fs::path &pack, std::string &error);
        // After open(): the pack this disc serves ("" none), or why a pack set with useExtraCars is not served.
        std::string extraCars, extraCarsNote;
        bool read(const std::string &path, std::vector<uint8_t> &out, std::string &error) const;
        bool has(const std::string &path) const { return files_.count(path) != 0; }
        size_t fileCount() const { return files_.size(); }
        struct VolFile
        {
            bool compressed = false;
            uint64_t lba = 0;
            uint32_t stored = 0, real = 0;
        };
        const std::map<std::string, VolFile> &files() const { return files_; }

    private:
        bool readImage(std::ifstream &f, uint64_t offset, size_t n, std::vector<uint8_t> &out) const; // the image + the pack
        fs::path iso_;
        uint64_t imageBytes_ = 0;
        std::shared_ptr<const void> pack_; // the extra-cars pack served (ps2x::iop::gt4::extracars::Pack)
        std::map<std::string, VolFile> files_;
    };
    // Raw DEFLATE (RFC 1951) decoder; false on an invalid stream. Like zlib's decompressobj().decompress()
    // (what the Python tools use), a stream that simply ends returns what was decoded so far: GT4.VOL's
    // table-of-contents pages are blocks with no final block. complete (optional) says whether the
    // final block was reached.
    bool inflateRaw(const uint8_t *in, size_t inLen, std::vector<uint8_t> &out, size_t maxOut = 64u << 20, bool *complete = nullptr);

    // =========================================================================== file layer (gt4save.py analyse)
    struct InnerHeader // GranTurismo4::GameSerializeBase header (0x43aaf0 / 0x43aa68)
    {
        uint32_t magic = 0;
        bool magicOk = false;
        uint32_t bodyLen = 0;
        bool lenOk = false;
        uint32_t crcStored = 0;
        std::optional<uint32_t> crc; // nullopt when the length runs past the file
        bool crcOk = false;
        uint32_t word3 = 0;
    };

    struct GamedataInfo
    {
        uint64_t size = 0;
        uint32_t copies = 1;            // > 1: the recomp's append bug wrote several copies; the last is used
        std::vector<bool> olderCopiesOk;
        uint32_t seed = 0, outerCrc = 0, outerCrcStored = 0;
        bool outerCrcOk = false;
        uint64_t swapA = 0, swapB = 0;
        InnerHeader inner;
        // summary
        std::string profileName;
        uint32_t calendarToday = 0;
        std::optional<std::string> calendarDate; // "2005-04-02"
        int64_t days = 0;
        uint32_t calendarEntries = 0;
        int32_t garageCurrentSlot = -1;
        uint32_t garageKey = 0;
        std::array<uint32_t, 4> garageUnknown{};
        uint32_t garageCounter = 0;
        struct IndexUsed // gt4save.py's garage_index_used (entries whose car code is not -1)
        {
            int index = 0;
            uint64_t carId = 0, flags = 0, w16 = 0, w24 = 0;
        };
        std::vector<IndexUsed> indexUsed;
    };

    struct GarageHeader
    {
        uint64_t money = 0; // credits
        std::array<uint32_t, 9> w2_10{};
        uint32_t counterA = 0, counterB = 0, noncePrev = 0, key = 0, nonce = 0;
    };
    GarageHeader garageHeaderFields(const uint8_t *plain64);

    enum class SlotKind
    {
        Car,             // a car GT4 wrote; decrypts with a valid CRC
        Raw,             // written, but does not decrypt (kept as is)
        Missing,         // the file is too short
        NoiseButIndexed  // never written, and the index uses it (normal for a stock car)
    };
    const char *slotKindName(SlotKind k); // "car", "raw", "missing", "noise_but_indexed"

    struct SlotState
    {
        int slot = 0;
        SlotKind kind = SlotKind::Raw;
        uint32_t seed = 0, crc = 0;
        bool indexed = false;
    };

    enum class TrailingKind
    {
        Header,
        Car,
        Raw
    };
    struct TrailingWrite // bytes past the 1,000 slots (the recomp's append bug)
    {
        uint64_t offset = 0;
        TrailingKind kind = TrailingKind::Raw;
        GarageHeader header; // Header
        int slot = -1;       // Car
        uint32_t seed = 0, crc = 0;
        uint64_t length = 0; // Raw
    };

    struct GarageInfo
    {
        uint64_t size = 0, expectedSize = kGarageBytes;
        GarageHeader header;
        bool keyOk = false, validForGamedata = false;
        std::vector<SlotState> slots; // every slot that is not plain never-written noise
        int noiseSlots = 0;
        std::vector<TrailingWrite> trailing;
    };
    // 0x1cba80: key match; counter in A or B; the nonce is checked only when non-zero.
    bool garageHeaderValid(const GarageHeader &h, uint32_t key, uint32_t counter, uint32_t nonce = 0);

    // =========================================================================== fields (gt4save_fields.py)
    struct Profile
    {
        std::string username, password, lastEntryName;
        uint64_t score = 0, totalPrizeMoney = 0;
        uint32_t totalPrizeCars = 0;
        std::array<uint32_t, 7> racesByResult{}; // [0] retired, [1] won, [2..6] 2nd..6th
        uint64_t aspecDistance = 0, bspecDistance = 0;
        uint32_t withdrawn = 0, withdrawnGt4p = 0;
        uint8_t metType = 0;
        uint64_t racesTotal = 0;
        uint32_t wins = 0;
        std::vector<int> unknownNonzero;
    };

    struct DayEvent
    {
        int64_t day = 0;
        uint8_t type = 0; // 0 NO_EVENT, 1 GET_CAR, 2 SELL_CAR, 3 RUN_RACE, 4 RUN_LICENSE, 5 RUN_MISSION, 6 BUY_WHEEL, 7 BUY_WING, 8 RUN_COURSE
        uint8_t a = 0, b = 0, c = 0;
        uint32_t u32 = 0;
        std::optional<uint64_t> code;
        // derived as gt4save_fields.parse_calendar does
        uint8_t colour() const { return a; }           // GET_CAR / SELL_CAR
        uint8_t how() const { return b; }              // GET_CAR: 1 new, 2 used, 3 trade, 4 present
        uint8_t result() const { return a; }           // RUN_*: result enum
        std::optional<uint32_t> timeMs() const { return u32 == kNoTime ? std::nullopt : std::optional<uint32_t>(u32); }
        uint16_t value() const { return uint16_t(b | (c << 8)); } // SELL_CAR
    };
    const char *dayEventName(uint8_t type); // nullptr when >= 9
    const char *getCarHowName(uint8_t how); // nullptr when not 1..4

    struct Calendar
    {
        uint32_t count = 0, today = 0;
        int64_t days = 0;
        std::vector<DayEvent> events;
    };

    struct GarageFields
    {
        int32_t ridingSlot = -1;
        uint32_t garageKey = 0;
        std::array<uint32_t, 4> unknown81d0{};
        uint32_t saveCounter = 0;
    };

    struct GarageEntry // one of the 1,000 32-byte garage index entries
    {
        int slot = 0;
        std::optional<uint64_t> code;
        uint32_t inGarage = 0, storedParams = 0, colour = 0, powerX100 = 0, weight = 0, dayAdded = 0, flagsTop = 0;
        uint64_t odometer = 0;
        uint32_t rideOrder = 0;
        std::array<uint64_t, 4> raw{};
    };

    struct BlockSettings // CarParameter setup fields (SETTING_FIELDS)
    {
        uint16_t finalGearX1000 = 0;
        uint8_t gearAutoLevel = 0;
        uint16_t finalGearDefault = 0;
        uint8_t drivetrain = 0, brakeCtrlF = 0, brakeCtrlR = 0, racingModifyA = 0, racingModifyB = 0, camberF = 0, camberR = 0;
        uint16_t rideHeightF = 0, rideHeightR = 0;
        uint8_t toeF = 0, toeR = 0, springF = 0, springR = 0;
        std::array<uint8_t, 8> damper{};
        uint8_t stabilizerF = 0, stabilizerR = 0;
        std::array<uint8_t, 3> lsdF{}, lsdR{}; // initial / accel / decel
        uint8_t tcs = 0;
        std::array<uint8_t, 4> asmSetting{};
        uint8_t ballastWeight = 0;
        int8_t ballastPosition = 0;
        uint8_t nos = 0;
    };

    struct Block // one of a car's three 376-byte parameter blocks
    {
        std::optional<uint64_t> car, unknown08, variation;
        int8_t colour = 0;
        std::vector<uint64_t> parts; // fitted parts as spec codes (table in the high word)
        std::array<uint16_t, 12> gearsX1000{};
        BlockSettings settings;
    };

    struct Car // SPEC_DATABASE::CarGarage, 1,208 bytes
    {
        std::array<Block, 3> blocks;
        std::optional<uint64_t> wheel;
        uint32_t activeBlock = 0;
        std::array<uint8_t, 12> ownedPartsBits{};
        int ownedPartsCount = 0;
        uint32_t odometer = 0;
        float unknown49c = 0, oil = 0, dirtiness = 0, bodyLife = 0;
        uint8_t wearFrozen = 0;
        std::array<uint8_t, 24> unknown470{};
        const Block &active() const { return activeBlock < 3 ? blocks[activeBlock] : blocks[0]; }
    };
    Car parseCar(const uint8_t *car1208);

    struct RaceUnit // RaceRecord row (RACE.dbt row order)
    {
        int row = 0;
        uint32_t raw = 0;
        uint32_t typeRaw() const { return raw & 15; }        // 0 none, 1 licence, 2 event, 3 mission
        uint32_t best() const { return (raw >> 4) & 15; }    // result enum
        uint32_t present() const { return (raw >> 8) & 15; }
        uint32_t spot() const { return (raw >> 12) & 15; }
        uint32_t bestScore() const { return (raw >> 16) & 0xFF; }
        uint32_t byte3() const { return raw >> 24; }
    };
    const char *resultName(uint32_t r); // "1st" .. "8th", "abandon", "gold", "silver", "bronze", "red", "failed", "view", "none"
    std::string unitTypeName(uint32_t t); // "none", "license", "event", "mission", "?<n>"

    struct CourseEntry
    {
        int rank = 0;
        uint32_t value = 0; // time in ms (max-speed records: the float in `speed`)
        float speed = 0;
        uint16_t year = 0;
        uint8_t month = 0, day = 0;
        std::optional<uint64_t> car;
        std::string name;
        std::string date() const;
    };
    struct CourseUnit
    {
        int index = 0;
        std::optional<uint64_t> course;
        std::vector<CourseEntry> entries;
        std::vector<uint32_t> extraTimes;
        uint8_t bestScore = 0;
    };

    struct LicenceEntry
    {
        int rank = 0;
        uint32_t timeMs = 0;
        uint16_t year = 0;
        uint8_t month = 0, day = 0;
        uint8_t result = 15;
        std::string name;
        std::string date() const;
    };
    struct LicenceUnit
    {
        int index = 0;
        std::string test; // "B-1" .. "S-16", "coffee-1", "mission-34", "unused-n"
        std::vector<LicenceEntry> entries;
        std::vector<uint32_t> extraTimes;
    };

    struct Available
    {
        std::vector<uint64_t> cars, courses;
        uint32_t tailU32 = 0;
    };
    struct Favorite
    {
        uint32_t limit = 0, count = 0;
        std::vector<uint64_t> codes;
    };
    struct Present
    {
        std::vector<int> byRace, byTime;
    };
    struct Championship
    {
        std::string raceCode;
        uint8_t suspended = 0, session = 0;
        std::vector<int> restNonzero;
    };
    struct UsedCar
    {
        std::vector<int> sold;
        uint32_t week = 0;
    };
    struct Context
    {
        std::string project, page;
    };

    struct Options
    {
        enum class Kind
        {
            U32,
            U8,
            F32Raw // a float volume, kept as its raw 32-bit pattern (as the report does)
        };
        struct Field
        {
            const char *name;
            Kind kind;
            uint32_t value;
        };
        uint32_t plainSize = 0;
        std::vector<Field> fields; // OPTION_U32, OPTION_U8, OPTION_F32 in that order
        uint32_t gameZone = 0;
        std::string lanMachineName;
        std::optional<uint32_t> get(std::string_view name) const;
    };

    struct Aspec // A-spec points as gtmode.adc sums them (best score per race)
    {
        uint64_t events = 0, missions = 0, untyped = 0;
        int eventsWon = 0, eventsRecorded = 0;
    };

    struct Fields
    {
        Profile profile;
        Calendar calendar;
        GarageFields garageFields;
        std::vector<GarageEntry> garageIndex; // entries that hold a car code
        Car currentCar;                       // the car being driven (GAMEDATA)
        std::map<int, Car> slotCars;          // cars stored with their parameters in the garage file
        std::vector<RaceUnit> raceRecord;     // rows that are not untouched
        std::vector<CourseUnit> courseRecord; // 128
        std::vector<CourseUnit> machineRecord; // 4
        std::vector<LicenceUnit> licenseRecord;
        Available available;
        Favorite favoriteCar, favoriteCourse;
        Present present;
        Championship championship;
        UsedCar usedCar;
        Options option;
        Context context;
        Aspec aspec;
    };

    struct LicenceStatus // needs the spec database (race labels)
    {
        struct Licence
        {
            std::string name; // B, A, IB, IA, S
            std::array<std::string, 16> tests;
            bool passed = false, allGold = false;
            int gold = 0, silver = 0, bronze = 0, score = 0;
        };
        std::vector<Licence> licences;
        std::array<std::string, 5> coffeeBreaks;
        std::array<std::string, 34> missions;
    };

    enum class Level
    {
        Error, // GT4 would reject the save or misbehave
        Warn,  // unusual; leave alone
        Info
    };
    const char *levelName(Level l); // "ERROR", "WARN", "INFO"
    struct Check
    {
        Level level;
        std::string message;
    };

    // The decrypted data (the tests rebuild the files from it; the launcher can ignore it).
    struct Decrypted
    {
        std::vector<uint8_t> gamedataPlain;  // the envelope payload (237,752 bytes)
        std::vector<uint8_t> gamedataPrefix; // older appended GAMEDATA copies, verbatim
        bool hasGarage = false;
        std::array<uint8_t, 64> garageHeader{};
        std::map<int, std::pair<uint32_t, std::vector<uint8_t>>> slotPlain; // slot -> (seed, 1,272 bytes)
        std::map<int, std::vector<uint8_t>> slotRaw;
        std::vector<std::array<uint8_t, 64>> trailingHeaders;
        std::vector<std::vector<uint8_t>> trailingCars;
        std::vector<std::vector<uint8_t>> trailingRaw;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> otherFiles; // icon.sys, icon0.ico
    };

    // =========================================================================== the save
    struct Save
    {
        bool loaded = false;  // false: error says why (no GAMEDATA, not a GT4 file, ...)
        std::string error;
        std::string source;
        Rounding rounding = Rounding::TowardZero;

        GamedataInfo gamedata;
        std::optional<GarageInfo> garage;
        Fields fields;
        std::optional<LicenceStatus> licences; // with a spec database only
        std::vector<Check> checks;             // GT4's load checks and the editor invariants
        Decrypted dec;

        // ---- the headline numbers
        bool gt4LoadChecksPass() const;  // what GT4 itself checks when it loads (CRCs, magic, length, garage key/counter)
        bool hasErrors() const;          // any ERROR check
        // The recomp's memory-card append bug left extra copies / writes in the files (the check messages
        // say "run gt4save.py repair", as the Python report does; the launcher can word it its own way).
        bool appendBugHit() const;
        std::optional<uint64_t> credits() const { return garage ? std::optional<uint64_t>(garage->header.money) : std::nullopt; }
        const std::string &playerName() const { return fields.profile.username; }
        int64_t days() const { return fields.calendar.days; }
        int carCount() const; // garage index entries with the in-garage bit set
        uint64_t aspecPoints() const { return fields.aspec.events + fields.aspec.missions; }
    };

    // Loads <folder> from a card. db (optional) adds names, licence status and the name checks.
    Save load(const CardFiles &card, const std::string &folder = kSaveFolder, const SpecDb *db = nullptr,
              Rounding rounding = Rounding::TowardZero);
    // A folder card (e.g. game\mc0): its BASCUS-97328GAMEDATA folder.
    Save loadCardFolder(const fs::path &cardDir, const SpecDb *db = nullptr, Rounding rounding = Rounding::TowardZero);
    // A save folder itself (e.g. game\mc0\BASCUS-97328GAMEDATA, or a copy of it under any name).
    Save loadSaveFolder(const fs::path &saveDir, const SpecDb *db = nullptr, Rounding rounding = Rounding::TowardZero);
    // A .ps2 card file (opened read only).
    Save loadCardFile(const fs::path &ps2, const SpecDb *db = nullptr, Rounding rounding = Rounding::TowardZero);

    // Day number <-> date (0x5797a8: proleptic Gregorian ordinal + 1,721,424). nullopt outside 0001..9999.
    std::optional<std::string> dayNumberToDate(int64_t dayNumber);
    std::string hex32(uint32_t v); // "0x%08x"

    // The pieces of load(), exposed for tests and tools.
    Fields parseFields(const std::vector<uint8_t> &plain, const std::map<int, std::vector<uint8_t>> &slots);
    std::vector<Check> fieldChecks(const Fields &f, const std::map<int, SlotKind> &slotStatus, const SpecDb *db);
    std::optional<LicenceStatus> licenceStatus(const Fields &f, const SpecDb *db);
}
