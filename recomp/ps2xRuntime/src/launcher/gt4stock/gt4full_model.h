#pragma once

// GT4 Recompilation launcher, Full edition: the data behind its pages, with no raylib and no window (the host tests
// build it):
//   - what the GT4 program to be started reads (its strings), so each control is live, greyed or hidden;
//   - GT4's car data from the disc (GENERIC_CAR, DEFAULT_PARTS, ENGINE, CHASSIS, DRIVETRAIN, FRONTTIRE / REARTIRE, TIRESIZE),
//     GT4's own car pictures (piece/car_pict, "Tex1"), the makers' names, GT4's present lists (Prize Cars) and the
//     used-car lots (Hidden Cars) (analysis/design/full-launcher/SAVE-FORMAT.md 8);
//   - the Garage (My garage, Compare cars) and Game progress (Overview, GT Mode, Arcade) of a save, read only;
//   - the launcher's own estimates (0-100 km/h and so on: GT4 never stores them), always labelled as estimates;
//   - the multi-screen rig's side angle and field of view (MULTI-MONITOR-NOTES.md, the developer's rig).

#include "gt4stock_names.h"
#include "gt4stock_savedetails.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gt4save
{
    struct Save;
    class Gt4Disc;
}

namespace gt4stock
{
    namespace fs = std::filesystem;

    // ------------------------------------------------------------------ what the GT4 program reads
    // Whole strings in its .rdata / .data (as exeFeatures does for the cheats): each proves a reader in that GT4.exe.
    struct FullFeatures
    {
        bool read = false;
        std::string error;
        bool raceMode = false;      // GT4_RACE_MODE (the 480p update)
        bool raceColour = false;    // GT4_RACE_COLOUR
        bool aspect = false;        // GT4_ASPECT
        bool bicubic = false;       // GT4_PICTURE_FILTER=bicubic
        bool lanczos = false;       // GT4_PICTURE_FILTER=lanczos
        bool vulkan = false;        // GT3_VKGL_PREPARE: a GT4.exe built with the Vulkan renderer
        bool renderer = false;      // GT3_RENDERER
        bool squarePixels = false;  // GT3_DISPLAY_ASPECT
        bool span = false;          // GT3_FULLSCREEN=span
        bool exclusive = false;     // GT3_FULLSCREEN=exclusive (its log line "full screen (exclusive)")
        bool net = false;           // GT4_NET (the LAN service)
        bool engineBass = false;    // GT4_ENGINE_BASS (the Sound page's engine bass)
        bool fpsCounter = false;    // GT4_FPS_COUNTER (Display > Smoothness, FPS counter)
        bool fpsStats = false;      // GT4_FPS_STATS: the FPS counter's simple and detailed modes (an older one: on / off only)
        // Internal resolution checked with GT4 (GT3_GS_GL_SCALE / GT3_GS_GL_FAST, Display > Resolution): the marker string
        // kInternalResMarker, which only a GT4.exe from the full-m2 Resolution track on has (its renderer logs it at start).
        bool internalRes = false;
        // The wider shapes 21:9, 32:9 and 48:9 (GT4_ASPECT): the marker string kWideShapesMarker, which only a GT4.exe from
        // the full-m2 Wide track on has (it logs it when a wider shape is chosen).
        bool wideShapes = false;
        // The race cameras GT4_CAM_FAR_CHASE and GT4_CAM_LOOK (Display > Cameras): the marker string kCamerasMarker, which
        // only a GT4.exe from the full-m2 Cameras track on has (it logs it when either is on).
        bool cameras = false;
        // More race cameras GT4_CAM_VIEWS and GT4_CAM_LOCK (Display > Cameras: roof and close chase, the view button's views,
        // the views locked to the car's tilt): the marker string kMoreCamerasMarker, which only a GT4.exe from the full-m2
        // MoreCams track on has (it logs it when either is given).
        bool moreCameras = false;
        // Our own race HUD widgets and start lights GT4_HUD_* / GT4_START_* (Display > HUD): the marker string kHudMarker, which
        // only a GT4.exe from HUD hand-off 4 on has (gt4_hud.cpp logs it when a widget or the lights are on).
        bool hud = false;
        // Car and wheel detail GT4_CAR_DETAIL, GT4_WHEEL_DETAIL and GT4_WHEEL_BLUR (Display > Detail): the marker string
        // kDetailMarker, which only a GT4.exe from the full-m2 Detail track on has (it logs it when any of them is chosen).
        bool detail = false;
        // Three screens side by side, one wide picture GT4_SCREENS / GT4_SCREENS_GEOMETRY (Multi-screen): the marker string
        // kScreensMarker, which only a GT4.exe from the full-m2 Multi track on has (it logs it when GT4_SCREENS=3).
        bool screens = false;
        // GT4's own Multi Monitor Mode GT4_MM_* (Multi-screen; analysis/design/multi-monitor): the marker string
        // kMultiMonitorMarker, which only a GT4.exe from the multi-monitor work on has (it logs it when a GT4_MM_ key is given).
        bool multiMonitor = false;
        // Colour LUTs GT4_LUT and GT4_LUT_STRENGTH (Display > Picture, Colour): the marker string kLutMarker, which only a
        // GT4.exe from the colour-lut track on has (it logs it at start).
        bool lut = false;
        // Texture smoothing GT4_TEX_SMOOTH (Display > Detail and textures): the marker string kTexSmoothMarker, which only a
        // GT4.exe from the tex-smooth track on has (its renderer logs it at start).
        bool texSmooth = false;
        // Texture dump / replacement (GT4_TEXTURE_DUMP, GT4_TEXTURE_REPLACE): GT4.exe from the texture-dump track on.
        bool texDump = false;
        // Extra cars (analysis/jp-cars): the key's name GT4_EXTRA_CARS, which only a Full GT4.exe from the jp-cars work on has
        // (its disc layer serves extra-cars\esso.gt4x; the Classic build never does).
        bool extraCars = false;
        // GT3_RENDERER=auto and gl in a Vulkan build (GT4_RENDERER_PRETEND_VENDOR, its test switch): an older one reads vk only.
        bool rendererAuto = false;
        bool classic = false;       // GT4_CLASSIC_BUILD=1: the Classic GT4 (the Full launcher needs a development one)
        bool soundPacks = false;    // GT4_SOUND_PACK (Sound > Sound packs; analysis/design/sound-packs)
        bool has480p() const { return raceMode && raceColour && aspect; }
        std::string text() const;
    };
    FullFeatures fullFeatures(const fs::path &exe);
    // The string GT4.exe's renderer logs at start (gs_gl_backend.cpp, GT4 builds), whole in its .rdata.
    inline constexpr const char *kInternalResMarker = "GT4 internal resolution (full-m2)";
    // The string GT4.exe logs when GT4_ASPECT is 21:9, 32:9 or 48:9 (ps2_runtime.cpp, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kWideShapesMarker = "GT4 wider shapes (full-m2)";
    // The string GT4.exe logs when GT4_CAM_FAR_CHASE or GT4_CAM_LOOK is on (ps2_runtime.cpp, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kCamerasMarker = "GT4 race cameras (full-m2)";
    // The string GT4.exe logs when GT4_CAM_VIEWS or GT4_CAM_LOCK is given (ps2_runtime.cpp, development GT4 builds), whole in
    // its .rdata.
    inline constexpr const char *kMoreCamerasMarker = "GT4 more race cameras (full-m2)";
    // The string GT4.exe logs when a GT4_HUD_* widget or GT4_START_LIGHTS is on (gt4_hud.cpp), whole in its .rdata.
    inline constexpr const char *kHudMarker = "GT4 own race HUD (hud-4)";
    // The string GT4.exe logs when GT4_CAR_DETAIL, GT4_WHEEL_DETAIL or GT4_WHEEL_BLUR changes GT4's own detail
    // (ps2_runtime.cpp, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kDetailMarker = "GT4 car and wheel detail (full-m2)";
    // The string GT4.exe logs when GT4_SCREENS=3 (ps2_runtime.cpp, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kScreensMarker = "GT4 three screens (full-m2)";
    // The string GT4.exe logs when a GT4_MM_ key is given (gt4_multimon.h, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kMultiMonitorMarker = "GT4 Multi Monitor settings (multi-monitor)";
    // The string GT4.exe logs at start with its colour LUT ([lut], ps2_runtime.cpp, development GT4 builds), whole in its .rdata.
    inline constexpr const char *kLutMarker = "GT4 colour LUTs (colour-lut)";
    // The string GT4.exe's renderer logs at start with GT4_TEX_SMOOTH's state (gs_gl_backend.cpp, development GT4 builds),
    // whole in its .rdata.
    inline constexpr const char *kTexSmoothMarker = "GT4 texture smoothing (tex-smooth)";

    // ------------------------------------------------------------------ GT4's car data (the disc)
    struct DiscCar
    {
        uint32_t code = 0;
        std::string label;     // spec label ("r390_race_98")
        std::string name;      // GT4's list name
        std::string fullName;  // GT4's full name ("Nissan R390 GT1 Race Car '98")
        std::string maker;     // GT4's maker name ("Nissan"), from its tuner names
        uint32_t price = 0;    // new price (GENERIC_CAR +8)
        int year = 0;          // GENERIC_CAR +12
        bool raceCar = false;  // GENERIC_CAR +18 != 0 [I: race-car flags]
        // GT4's tuner (tune shop) for the car: GENERIC_CAR +16, an id in CORE.elf's tuner table 0x6a7658 (96 {id, name}
        // pairs: 34 renault, 76 hks, 33 polyphony, 0 nothing, ...; read by MCarData::GetTuner 0x12eba0 -> 0x448738 / 0x448df0).
        // gtmode.adc TuningRoot::onActivate: a car whose tuner is "polyphony" gets "this car has no tuningshop" [C]; on the
        // US disc that is only the Formula Gran Turismo '04 (and "nothing", only the unused sileighty_xx) [C, the disc's rows].
        int tuner = -1;
        bool hasTuneShop() const { return tuner != 33 && tuner != 0; }
        int powerPs = 0;       // ENGINE +0x1A (catalogue PS)
        int torqueX100 = 0;    // ENGINE +0x1C (kgfm x 100)
        int weightKg = 0;      // CHASSIS u16[3]
        int lengthMm = 0, heightMm = 0, wheelbaseMm = 0;
        // Front share of the weight, % (CHASSIS byte 28; 0 unknown). [I: GT4's data, read as front % by its values on all 722
        // rows (38-68, FF 61 on average, MR 45) against published splits: S2000, RX-7 FD, RX-8, M3 50; Elise 38; Ford GT 43;
        // 350Z 53; analysis/design/full-launcher/SAVE-FORMAT.md 8]
        int frontWeightPct = 0;
        int drivetrain = -1;   // DRIVETRAIN byte 16: 0 FR, 1 FF, 2 4WD, 3 MR
        struct Tyre
        {
            int width = 0, aspect = 0, rim = 0; // 235 / 45 R 17
            bool ok() const { return width > 0 && rim > 0; }
        } front, rear;
        bool specs = false;    // the engine / chassis rows were found
    };
    const char *drivetrainName(int d); // "FR", "FF", "4WD", "MR", "?"
    std::string tyreText(const DiscCar::Tyre &t); // "235/45R17"
    std::string weightBalanceText(int frontPct);  // "55 : 45" (front : rear), "-" unknown

    // A car GT4 gives as a prize: GT4's prize table (projects/gtmode/US_carlist.adc, "PresentData": parallel lists condition,
    // detail, type, content, content2), one row per prize, e.g. win / am_sunday_0000 / car / a112_abarth_79 / -.
    struct PrizeRow
    {
        std::string condition; // win, license, all_silver, all_gold, mission_1st, achievement
        std::string detail;    // the race (am_sunday_0000), the licence (l0b), the missions (1,10), the completion (25)
        std::string extra;     // content2: "-", a colour number, "ending"
    };
    // One row of GT4's prize table as stored: PresentData (car / paper prizes) and PresentTimeData (condition "weeks": a
    // course added after that many GT Mode weeks; content2 the course's place: road, city, real, dirt).
    struct PresentRow
    {
        std::string condition, detail, type, content, extra;
    };
    struct PrizeCar
    {
        uint32_t code = 0;
        std::string label;
        std::vector<PrizeRow> given; // every prize that gives it
        bool soldNew = false;        // in a maker's new-car dealer list (US_carlist.adc, a list with a dealer page)
        bool soldUsed = false;       // in any week of the US used-car lots
        bool prizeOnly() const { return !soldNew && !soldUsed; }
    };

    // Car Dealers > Hidden Cars. First the four hidden black cars (SAVE-FORMAT.md 8.1): label, GT4's black colour index, the
    // used-car week and lot that sell them, and GT4's black (its colour table's RGB, a grey). Then the rare car (developer,
    // 2026-10-08) GT4 gives as no prize, the Abt Audi TT-R: GT4's own price, new, in GT4's first colour (week -1: no used-car
    // week). the developer's other rare cars are GT4 prizes: Car Dealers > Prize Cars (every car of GT4's prize table).
    // The black Formula GT (developer, 2026-10-11): GT4 gives it, not a used-car lot (week -1 with a black grey: given()); it comes
    // new at GT4's own price, in its colour 11 (index 10, RGB 6,6,6, no name in GT4's data).
    struct HiddenCar
    {
        const char *label;
        int colour;   // the black cars: their black (1; the Formula GT 10); the rare cars: 0, GT4's first colour
        int week;     // 99: elapsed days 693-699 (GT4's Day 694-700), then every 700 days; -1 not on a used-car lot
        int lot;      // 1 '90s early, 2 '90s late; 0 a rare car
        int blackGrey = 0; // the black cars: GT4's black (R = G = B, VARIATIONamerican +36); 0 a rare car
        const char *note = nullptr; // where GT4 gives it, when it is no used-car lot
        bool rare() const { return week < 0 && blackGrey == 0; }
        bool given() const { return week < 0 && blackGrey > 0; } // a black car GT4 gives (no used-car lot)
    };
    // The extra cars a Full GT4.exe adds from the developer's own Japanese disc (analysis/jp-cars; no switch): Car Dealers > Extra Cars
    // lists them while the car data (read through extra-cars\esso.gt4x next to GT4.exe) has them. kExtraCarCode: the ESSO's
    // car code (229), which the stock US car data does not have (Saves warns about a save holding it).
    inline constexpr const char *kExtraCars[] = {"esso_supra_01"};
    inline constexpr uint64_t kExtraCarCode = 229;
    inline constexpr size_t kHiddenCarCount = 6;
    extern const HiddenCar kHiddenCars[kHiddenCarCount];
    // The black car a garage car is (its label and GT4's black colour index): GT4's black grey for its picture, else 0.
    int blackCarGrey(const std::string &label, int colourIndex);
    // GT4's car picture shown in a black car's colour: GT4 keeps ONE picture per car (piece/car_pict, in the car's usual
    // livery) and none per colour, so the launcher darkens that picture to GT4's black, keeping its shading (the car's own
    // tones, stretched between its darkest and brightest parts, mapped to GT4's black and a dark sheen).
    void blackenPicture(std::vector<uint8_t> &rgba, int grey);
    // The launcher's own picture of a hidden black car (gt4full.rc): GT4's 3D render of the car in black, cropped from GT4's
    // Garage car screen (analysis/design/black-cars). Its resource id, 0 for any other car (then blackenPicture is used).
    int blackCarPictureResource(const std::string &label);
    // The launcher's own picture of a prize car GT4 has no dealer picture of (gt4full.rc; analysis/design/prize-cars): GT4's
    // 3D render of it in its first colour, from GT4's Garage car screen. Its resource id, 0 for any other car.
    int prizeCarPictureResource(const std::string &label);

    // GT4's used-car price (CORE.elf 0x441128 with the factor table 0x6a6988): new price x factor(odometer).
    double usedPriceFactor(uint64_t odometerKm);
    uint64_t usedPrice(uint32_t newPrice, uint64_t odometerKm);
    // The odometer range GT4's used lots draw for a model year (CORE.elf 0x6a6910, thousands of km: 1990 60-100 ...
    // 2004 3-10; years before 1990 as 1990, after 2004 as 2004).
    std::pair<int, int> usedMileageRangeKm(int year);

    class CarCatalogue
    {
    public:
        // Everything from the GT4 (USA) disc image, read only (about half a second; the launcher loads it on a worker thread).
        static std::shared_ptr<CarCatalogue> load(const fs::path &iso, const GameNames *names, std::string &error);
        ~CarCatalogue();
        const DiscCar *car(uint64_t code64) const;
        const DiscCar *carByLabel(const std::string &label) const;
        size_t carCount() const { return cars_.size(); }
        // GT4's own dealer picture of a car (piece/car_pict/<label>.img: 160 x 100, RGBA); false when the disc has none under
        // the car's picture name (pictureName: a US car whose picture GT4 keeps under its Japanese name), its label or the
        // simple fallback names (a model's words dropped one at a time, e.g. gt_one_race_exxon_99 -> gt_one_race_99). GT4's
        // stand-in picture (piece/car_pict/disable.img, an AE86 that 81 names share, viper_gtsr_00 among them) and an empty
        // picture (formula_gran_turismo_04) count as none. Thread-safe (each call opens the image).
        bool picture(const std::string &label, int &w, int &h, std::vector<uint8_t> &rgba, std::string *usedLabel = nullptr) const;
        // GT4's prize cars (every car its prize table gives), in the table's order.
        const std::vector<PrizeCar> &prizes() const { return prizes_; }
        size_t prizeRowCount() const { return prizeRows_; }
        // Every row of GT4's prize table (cars, papers, courses), in the table's order.
        const std::vector<PresentRow> &presentRows() const { return presentRows_; }
        // Whether a used-car week file (usedcar/US_used<week>.adc) lists this label.
        bool usedLotHas(int week, const std::string &label) const;

    private:
        std::unique_ptr<gt4save::Gt4Disc> disc_;
        std::vector<uint8_t> placeholder_; // GT4's stand-in picture (piece/car_pict/disable.img), as stored
        std::map<uint32_t, DiscCar> cars_;
        std::map<std::string, uint32_t> byLabel_;
        std::vector<PrizeCar> prizes_;
        std::vector<PresentRow> presentRows_;
        size_t prizeRows_ = 0;
        mutable std::map<int, std::set<std::string>> usedWeeks_;
    };

    // The name GT4 keeps a US car's picture under when it is not the car's label (its Japanese name, e.g. viper_gtsr_00 ->
    // chrysler_viper_gtsr_00, _240sx_180_us_96 -> _180sx_x_96), checked by eye against GT4's names; nullptr when none.
    const char *pictureName(const std::string &label);
    // Tex1 (GT4's texture set) -> RGBA: the first texture with its palette (PSMT8 / PSMT4, a PSMCT32 palette in CSM1).
    bool decodeTex1(const std::vector<uint8_t> &file, int &w, int &h, std::vector<uint8_t> &rgba, std::string &error);
    // A compiled menu-script module's strings (u16 length + printable text), in order (usedcars.py tokens).
    std::vector<std::string> scriptTokens(const std::vector<uint8_t> &data);
    // What a prize row gives the car for, in a player's words ("Sunday Cup: win it", "B licence: every test gold").
    std::string prizeReason(const PrizeRow &row, const GameNames *names);

    // ------------------------------------------------------------------ the launcher's estimates
    struct PerformanceEstimate
    {
        bool ok = false;
        double zeroTo100 = 0, zeroTo200 = 0, zeroTo300 = 0; // s (0: not reached)
        double brake100 = 0, brake200 = 0;                   // m, 100-0 and 200-0 km/h
        double topSpeedKmh = 0;
    };
    // A simple model (power with a traction limit, air drag, rolling resistance; tyre grip from road or racing tyres):
    // the launcher's "about" values, never GT4's.
    PerformanceEstimate estimatePerformance(double powerPs, double weightKg, int drivetrain, bool racingTyres);

    // ------------------------------------------------------------------ the garage of a save (read only)
    struct GarageCar
    {
        int slot = 0;
        int kind = 0; // 1 stock (no full record yet), 2 riding, 3 stored
        uint32_t code = 0;
        std::string name, fullName, maker, label;
        std::string colourName;
        bool hasRgb = false;
        uint8_t r = 0, g = 0, b = 0;
        int colourIndex = 0;
        uint64_t odometerM = 0;
        double powerPs = 0; // GT4's own number (garage index / 100): PS (GT4's car screen shows it x 0.986 as HP) [I]
        int weightKg = 0;   // GT4's own number
        std::string acquired; // "Bought used, day 8"
        int dayAdded = 0;
        bool prize = false;
        // condition (full-record cars; a stock car gets GT4's values when it is first taken out: oil and wear = its odometer)
        bool condition = false;
        double oilAgeKm = 0;      // metres on this oil / 1000 (GT4: oil < 0 = the fresh-oil window)
        bool oilDue = false;      // GT4's rule: oil >= 5,000,000 m
        double dirt01 = 0;        // dirtiness / 0.8 (GT4's maximum)
        double bodyWearKm = 0;    // body life (metres of wear, max 3,000 km)
        bool frozen = false;      // GT4 never changes this car's condition
        bool rigidity = false;    // Increase Rigidity (the developer's "Body Rigidity Improvement") fitted
        std::vector<std::string> parts; // fitted parts as GT4's tune shop names them
        std::string partsNote;
        std::string tyreType;     // the fitted tyres' name (front), when the record names them
        const DiscCar *disc = nullptr;
        PerformanceEstimate est;
    };
    struct Garage
    {
        bool loaded = false;
        std::string problem; // why not (player text)
        std::string player;
        uint64_t credits = 0;
        bool hasCredits = false;
        int64_t gameDay = 1;
        std::vector<GarageCar> cars; // the riding car first, then GT4's garage order
        int oilDue = 0, needsWash = 0, withRigidity = 0, fullRecords = 0;
        const GarageCar *bySlot(int slot) const;
    };
    // needsWash counts cars at or above this share of GT4's maximum dirt (the launcher's line, not GT4's).
    constexpr double kWashFrom = 0.25;
    Garage buildGarage(const gt4save::Save &save, const GameNames *names, const CarCatalogue *catalogue);

    // ------------------------------------------------------------------ Game progress (read only, real save data only)
    struct ProgressRace
    {
        int row = 0;
        std::string label;
        int result = 15;   // GT4's best result: 0..7 = 1st..8th, 15 none
        int score = 0;     // the best score (A-spec points)
        bool run = false;
    };
    struct ProgressEvent
    {
        std::string series, name, hall;
        int hallOrder = 99, firstRow = 0;
        std::vector<ProgressRace> races;
        int won = 0, done = 0, points = 0;
    };
    struct ProgressTest
    {
        std::string key;   // "B-1"
        std::string name;  // GT4's text for the test, when known
        char medal = '-';  // G S B P (passed) -
        uint32_t bestMs = 0; // the best time GT4 kept (0: none)
    };
    struct ProgressLicence
    {
        std::string key, name;
        std::vector<ProgressTest> tests;
        bool passed = false, allGold = false;
        int gold = 0, silver = 0, bronze = 0, done = 0;
    };
    struct ProgressCourse
    {
        std::string label, name;
        int bestScore = 0;
        uint32_t bestMs = 0;
        std::string car, date, driver;
        int entries = 0;
    };
    // GT4's game completion rate (gt4full_completion.h): won of total entries, perMille = won * 1000 / total (remainder
    // dropped), shown as GT4 shows it ("0.8%"). known false: the disc's event lists could not be read (hide it).
    struct Completion
    {
        bool known = false;
        int won = 0, total = 0, perMille = 0;
        std::string shown() const { return std::to_string(perMille / 10) + "." + std::to_string(perMille % 10) + "%"; }
    };
    struct Progress
    {
        bool loaded = false;
        SaveDetails details; // the Classic Save details model (credits, days, licences, events with races run, ...)
        // Overview
        uint64_t distanceM = 0; // A-spec + B-spec
        uint64_t races = 0;
        uint32_t wins = 0;
        int eventRaces = 0, eventRacesWon = 0; // every event race on the disc, and the ones won
        Completion completion;                 // GT4's own completion rate (Home "Complete")
        // GT Mode
        std::vector<ProgressLicence> licences;
        std::vector<ProgressEvent> events; // every event series on the disc, GT4's hall order
        std::vector<ProgressTest> missions, coffee;
        // Arcade
        std::vector<ProgressCourse> courses;        // courses with an Arcade record
        std::vector<ProgressCourse> machineRecords; // GT4's 4 "machine records"
        size_t availableCars = 0, availableCourses = 0;
        std::vector<std::string> favouriteCars, favouriteCourses;
        int arcadeScoreTotal = 0;
    };
    Progress buildProgress(const gt4save::Save &save, const GameNames *names);
    std::string lapTime(uint32_t ms); // "1:23.456"

    // ------------------------------------------------------------------ GT Mode prizes (Garage > GT Mode prizes, read only)
    // GT Mode's events, licences, missions and completion rewards grouped as GT4's map has them, each with what GT4's prize
    // table gives for it. Names are GT4's own text (projects/gtmode/US/rtext.rt2) where it has them; gt4Name is false for
    // a name the launcher made from GT4's key.
    struct GuidePrize
    {
        std::string kind;  // car, course, paper
        std::string label; // the car or course label ("" for a paper)
        std::string name;  // what the player sees: the car's or course's name, "Licence certificate"
        uint32_t carCode = 0;
        std::string note;  // "ending movie", the course's place ("City Courses")
        int colour = -1;   // a car GT4 gives in a set colour (content2): its colour index; -1 a colour at random
    };
    struct GuideEntry
    {
        std::string key;  // win:<series>, license:l0b, all_silver:l0b, all_gold:l0b, mission_1st:1,10, achievement:25, weeks:2
        std::string name;
        bool gt4Name = true;
        std::string sub;  // a car maker's event hall and country ("Toyota Race Events · Japan")
        std::string how;  // what earns it, GT4's own prize text where it has one
        std::vector<GuidePrize> prizes;
        int state = -1;   // from the save: -1 not known, 0 not yet, 1 part done, 2 done
        std::string status;
    };
    struct GuideGroup
    {
        std::string key, title, note;
        bool gt4Title = true;
        std::vector<GuideEntry> entries;
    };
    // The key of the entry that lists a car prize row (PrizeRow from PrizeCar::given): win:<series>, license:l0b, ...
    std::string guideKey(const PrizeRow &row);
    // progress: the save's results (Saves > Game progress); nullptr or not loaded: no status.
    std::vector<GuideGroup> buildPrizeGuide(const CarCatalogue &cat, const GameNames *names, const Progress *progress);

    // ------------------------------------------------------------------ multi-screen rig
    struct RigView
    {
        double sideAngleDeg = 0; // the side screens' angle (auto: each faces the eye)
        double totalFovDeg = 0;  // the horizontal view across the three screens
        double centreFovDeg = 0;
    };
    // widthMm / bezelMm: one screen's picture and the gap between two pictures (half of it on each side of the hinge, as
    // the multi-monitor calculator gt4_screens.py has it); distanceMm: the eye to the centre screen.
    RigView rigView(double widthMm, double bezelMm, double distanceMm, bool angled, bool autoAngle, double angleDeg);
}
