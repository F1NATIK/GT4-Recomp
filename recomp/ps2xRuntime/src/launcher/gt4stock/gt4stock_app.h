#pragma once

// GT4 Recompilation launcher (Stock edition): the application (pages in gt4stock_pages.cpp).

#include "gt4stock_camera.h"
#include "gt4stock_cheats.h"
#include "gt4stock_devices.h"
#include "gt4stock_doc.h"
#include "gt4stock_names.h"
#include "gt4stock_savedetails.h"
#include "gt4stock_saves.h"
#include "gt4stock_scanline.h"
#include "gt4stock_settings.h"
#include "gt4stock_system.h"
#include "gt4stock_ui.h"

#include "shared/gt4_pad2_rules.h" // player 2's controller, by the player-2 runtime's own rule

#if defined(GT4L_EDITION_FULL)
#include "gt4full_model.h"  // the Full edition's pages' data (features, garage, progress, car data)
#include "gt4full_saveedit.h" // the Garage's save-changing actions (oil change, wash, rigidity removal, Car Dealers)
#include "gt4full_system.h" // its Windows side (network addresses)
#include <map>
#endif

#include <array>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace gt4stock
{
    extern const char *const kVersion;
    extern const char *const kProductName; // "GT4 Recompilation"
    extern const char *const kEdition;     // "Classic" (was "Stock")
    struct PadView; // the DualShock 2 drawing's state (gt4stock_pages.cpp)
#if defined(GT4L_EDITION_FULL)
    // The "How it works" text with the edition's words switched for the Full edition (gt4full_app.cpp).
    std::string fullHowText(const std::string &markdown);
#endif

    struct Options
    {
        fs::path root;     // --root: the GT4 folder (game\CORE.elf below it)
        fs::path settings; // --settings: the settings file (default <root>\gt4-start.env)
        fs::path exe;      // --exe: the GT4 program
        fs::path shots;    // --test-shots <dir>: hidden, every screen to PNG, then exit
        std::vector<std::pair<int, int>> sizes{{2560, 1440}, {1280, 960}};
        bool demo = false;        // --demo: demo devices, monitors and a last session (screenshots)
        bool dryRun = false;      // --dry-run: print what Play would start, then exit
        bool saveSettings = false; // --save-settings: write the settings file once, then exit (tests)
        fs::path report;          // --report <file>: test / dry-run output
        std::string scanlineShot; // --scanline-shot <prefix>: preview renders for compare_shot.py
        std::string page;         // --page <name>: start page
        // --save-copy <.ps2 file or folder> (repeatable, test shots): copies of GT4 saves for the
        // Save details pictures (the first: a card file in slot 2; the others opened as copies).
        std::vector<fs::path> saveCopies;
        // --cheat-files <folder> (test shots, 1.4): where the Cheats page's Save card / Save PDF write in
        // the test mode, in place of the Photos folder (the test mode writes nothing to the GT4 folder).
        fs::path cheatFiles;
#if defined(GT4L_EDITION_FULL)
        // --release-look (Full edition, test shots and dry runs): as a release build shows it, "Not built yet" hidden.
        bool releaseLook = false;
        // --only <prefix> (Full edition, test shots): only the scenes whose name starts with it.
        std::string only;
        // --edit-test <action> (Full edition, tests only): one Garage change made through the launcher's own path on the GT4
        // folder of --root, which must be a test copy (an "edit-test-root.txt" file in it); no window. Actions: oil:<slot>,
        // wash:<slot>, unrigid:<slot>, refresh:<slot> (Rigidity Refresh), hidden:<label>:pay|free, prize:<label>:pay|free.
        std::string editTest;
        bool buildTest = false; // --build-test: Build GT4 as the Play page starts it, no window (gt4full_build.cpp)
#endif
    };

    enum class Page
    {
        Play,
        Display,
        Controllers,
        Saves,
        Cheats, // 1.4
        About,
#if defined(GT4L_EDITION_FULL)
        Multiscreen, // the Full edition's pages
        Garage,
        Network,
        Sound,
        Roadmap, // Roadmap (developer, 2026-10-08: first named "Future features")
        RaceLab, // Race Lab (racelab/HANDOFF-1): the lap recorder and the Race Lab page on this PC
        RaceHud, // Driving > Race HUD (HUD hand-off 7): the HUD layout page
        Telemetry, // Driving > Telemetry screen (HUD hand-off 8): the monitor, the phone and their layouts
#endif
    };

#if defined(GT4L_EDITION_FULL)
    // One race picture set for the HUD layout page (GT4.exe's hud-pictures folder, HUD hand-off 7 A1; or the stock set the
    // launcher carries): the clean picture, the one with GT4's HUD, and GT4's parts' rectangles (fractions of the picture).
    // Driving > Telemetry screen (gt4full_telemetry.cpp): its state, and a layout's box (gt4full_telelayout.h).
    struct TeleState;
    namespace tele
    {
        struct Item;
    }

    struct RaceHudSet
    {
        std::string name;   // <yyyymmdd-hhmmss>-<course id> ("stock" for the launcher's own)
        fs::path clean, hud; // empty for the stock set (resources)
        int course = 0;
        std::string view;   // bumper, bonnet, chase, far
        int w = 0, h = 0;   // the game picture's size when it was taken
        std::vector<std::pair<std::string, Rectangle>> parts; // GT4's places (fractions of the picture)
        std::vector<std::pair<std::string, Rectangle>> shown; // where a part moved with GT4_HUD_MOVE is in the -hud picture
        bool stock = false;
        const Rectangle *part(const std::string &n) const
        {
            for (const auto &p : parts)
                if (p.first == n)
                    return &p.second;
            return nullptr;
        }
        const Rectangle *shownPart(const std::string &n) const
        {
            for (const auto &p : shown)
                if (p.first == n)
                    return &p.second;
            return part(n);
        }
    };
    // A set's .json read (false: not one GT4.exe wrote).
    bool parseRaceHudJson(const std::string &text, RaceHudSet &out);
#endif

    struct MonitorInfo
    {
        int index = 0;
        std::string name;
        int x = 0, y = 0, w = 0, h = 0, hz = 0;
        int workW = 0, workH = 0;
        bool primary = false;
        bool demo = false;
    };

    struct Install
    {
        fs::path launcherDir, root, gameDir, coreElf, exe, settingsFile;
        bool package = false;  // the exe is started without arguments and reads gt4-start.env itself
        std::string layout;    // "Classic package" / "development tree"
        bool coreFound = false, exeFound = false;
        uint64_t coreBytes = 0;
        uint32_t coreCrc = 0;
    };

    struct DiscCheck
    {
        std::string path;
        bool exists = false, sizeMatches = false, iso9660 = false, volumeMatches = false;
        uint64_t bytes = 0;
        std::string volume;
    };

    class App
    {
    public:
        int run(const Options &options);

    private:
        // ---- setup ----
        void locate();
        void loadAll();
        void applyDemo();
        void refreshMonitors();
        void refreshCards();
        void checkDisc();
        void loadPictures();
        void loadShirtDesign(); // the EyeToy page's picture of the GT shirt design, from the disc
        void saveShirtPrint();  // "Save the design to print..."
        void markDirty();
        bool saveNow(); // false: the file could not be written (the status line says why)
        sys::Launch playLaunch() const;

        // ---- frame ----
        void frame(int widthPx, int heightPx, const ui::Input &input);
        void drawSidebar();
        void drawHeader(const std::string &title, const std::string &subtitle);
        void drawStatusLine();
        void drawModal();
        void pagePlay();
        void pageDisplay();
        void pageControllers();
        void pagePad(float x, float y, float w, float h);
        void pageWheel(float x, float y, float w, float h);
        void pageCamera(float x, float y, float w, float h);
        void pageSaves();
        void pageSaveDetails(); // the Saves page's read-only "Save details" view
        void pageAbout();
        // 1.4: the Cheats page (gt4stock_cheatspage.cpp) and its files (gt4stock_cheatsfiles.cpp)
        void pageCheats();
        void drawCheatDetail(Rectangle d, bool player2, const std::string &player2Name);
        void drawCheatBig();  // Show big, with Practise
        void drawCheatSave(); // Save or print
        void readCheatSave(bool force); // the slot-1 save's day (read only)
        void startPractise(bool on);
        uint16_t practiseWord();                              // player 1's buttons now (gt4cheats bits)
        std::string practiseSource(bool *keyboardOnly) const; // what Practise reads
        void cheatTile(const gt4cheats::Code &code, float x, float y, float s, Color panel);
        float cheatSequence(const gt4cheats::Code &code, float x, float y, float maxW, float s, int progress, bool paper, bool center, Color bg);
        float cheatSequenceHeight(const gt4cheats::Code &code, float maxW, float s, bool numbers);
        Image renderOffscreen(int w, int h, Color clear, const std::function<void()> &draw);
        // Writes a PNG through the wide path (any folder name): encoded in memory, then std::ofstream.
        static bool exportPng(const Image &img, const fs::path &file);
        Image renderCheatCard(const gt4cheats::Code &code);
        std::vector<Image> renderCheatSheet(const std::vector<const gt4cheats::Code *> &codes, bool letter, bool ink);
        void prepareCheats(int widthPx, int heightPx); // outside drawing: the sheet's preview, and Save / Print
        fs::path cheatOutDir() const;
        // Player 2 (Controllers page, 1.4): the GLFW joysticks as the player-2 rule sees them, its pick now.
        gt4pad2rules::Inventory pad2Inventory() const;
        int pad2Pick(std::string *why) const;
        bool player2Ready(std::string *name) const;
        void pageHowItWorks();  // About -> "How it works", the whole text
        const DocLayout &howLayout(float width); // the text laid out for a column width (kept until it changes)
        void drawDocItem(const DocItem &it, float ox, float oy);
        void drawDualShock(Rectangle r, const PadView &view);
        PadView padView() const;
        void preparePreview(); // re-renders the preview targets when a setting changed (outside drawing)
        void drawPreview(Rectangle r);

        // ---- actions ----
        void play();
        void stopGame();
        void pollGame();
        void backup(int slot);
        void restore(const fs::path &zip, int slot);
        void setStatus(const std::string &text, Color c);
        // Save details: a slot's GT4 save (its folder card or card file), or a copy (tests).
        void openDetails(int slot);
        void openDetailsPath(const fs::path &source, const std::string &title);
        void reloadDetails();
        // The names from the disc, read once on a worker thread (again only if the disc changes).
        void startNames();
        void pollNames(bool wait = false);
        // What the names were read for: the disc image (and, Full edition, the extra-cars pack served over it).
        std::string namesKey() const;

        // ---- helpers ----
        const MonitorInfo *selectedMonitor() const;
        int defaultMonitor() const;
        void pickMonitor(int index);
        scan::Placement gamePlacement(const MonitorInfo *m, int *winW = nullptr, int *winH = nullptr) const;
        std::vector<std::pair<int, int>> windowSizes(const MonitorInfo *m) const;
        const DeviceInfo *wheelDevice() const;
        const DeviceInfo *pedalDevice() const;
        // The chosen webcam's place in the current camera list (the n of webcam:<n>), set
        // again whenever the list changes; a webcam that is not connected gives -1.
        void resolveCamera();
        const CameraInfo *chosenCamera() const;
        std::string cameraSummary() const; // the Play page's line
        int padJoystick() const; // raylib gamepad index of player 1 (-1 none; visible sessions)
        bool busy() const;       // GT4 running (ours or another)
        fs::path cardDir(int slot) const;
        fs::path cardFile(int slot) const; // the slot's .ps2 file (relative paths from game\)
        bool cardIsFile(int slot) const;
        fs::path backupDir() const;
        fs::path usbRoot() const;
        fs::path logFile() const;

        // ---- test modes ----
        int runTestShots();
        int runScanlineShot();
        int runDryRun();
        void report(const std::string &line);

#if defined(GT4L_EDITION_FULL)
        // ---- the Full edition (gt4full_app.cpp, gt4full_display.cpp, gt4full_pages.cpp, gt4full_garage.cpp,
        // gt4full_shots.cpp). A control is live when the GT4 program here has its side, greyed with a "Not built yet"
        // line in development builds otherwise (showUnbuilt_), hidden in release builds.
        void fullSetup(); // after the settings and the GT4 program are known
        bool showUnbuilt() const { return showUnbuilt_; }
        void drawFullSidebar();
        bool pageVisible(Page p) const; // Multi-screen is hidden in release builds (nothing on it is built yet)
        // Display
        void pageDisplayFull();
        std::vector<int> displayTabsShown() const;
        void displayPicture(float x0, float y0, float x1, float y1);
        void displayResolution(float x0, float y0, float x1, float y1);
        void displayShape(float x0, float y0, float x1, float y1);
        float displayCars(float x, float y, float w); // Display > Graphics: the Cars card (car and wheel detail); its bottom
        void displaySmoothness(float x0, float y0, float x1, float y1);
        void displayDetail(float x0, float y0, float x1, float y1);
        void displayHud(float x0, float y0, float x1, float y1);
        void displayCameras(float x0, float y0, float x1, float y1);
        // Why the 360 look cannot have the right stick with this controller mapping (Controllers > Controller: GT4's right
        // stick fed by something other than the controller's right stick); empty when it can.
        std::string cameraLookBlock() const;
        void prepareFullPreview();         // outside drawing: the picture with and without the settings
        void drawFullPreview(Rectangle r); // the split view (drag); the zoom follows the mouse over it
        void drawZoomPanes(Rectangle r);   // the same spot without and with the picture settings, side by side
        void drawPreviewFullscreen();      // over the whole launcher window; Esc or a click goes back
        int previewFilterMode() const;     // the preview shader's mode for the chosen scaling
        scan::Placement fullPlacement(const MonitorInfo *m, int *winW = nullptr, int *winH = nullptr) const;
        float fullAspectRatio() const; // the picture's shape (16:9, 4:3 or square pixels)
        // The preview's window (kept to a render target's size) and the picture's place in it, at the Full screen shape.
        scan::Placement fullPreviewPlacement(const MonitorInfo *m, int &ww, int &wh) const;
        void startPrepare(); // Prepare shaders: GT4.exe hidden, GT3_VKGL_PREPARE=1
        void pollPrepare();
        // Build GT4 from the player's own disc (the release's build-kit\gt4setup.exe build, gt4full_build.cpp)
        fs::path buildKit() const; // <root>\build-kit when it holds gt4setup.exe, else empty
        void startBuild();
        void pollBuild();
        void drawBuild(float x, float y, float w); // the Play page's progress while it runs, or its result
        std::string exeStamp() const; // "<bytes> <time>" of the GT4 program (Prepare shaders' record)
        bool preparedForThisExe() const;
        // Pages
        void pageMultiscreen();
        void pageNetwork();
        void pageSound();
        // Sound > Sound packs (gt4full_sounds.cpp; analysis/design/sound-packs): the player's own engine sound packs, made by
        // sound_pack.py (Python, standard library; the launcher carries the script and writes it next to the packs).
        void pageSoundPacks(float x0, float top, float x1, float bottom);
        fs::path soundPacksFolder() const; // <GT4.exe folder>\sound-packs (GT4.exe reads a relative pack name there too)
        void writeSoundMap();              // the per-car map file (GT4_SOUND_PACK_MAP), when per car is chosen
        void refreshSoundPacks(bool force = false);
        bool soundImporter(fs::path &script, std::string &why); // sound_pack.py written from the launcher's copy
        void startSoundJob(int kind, const std::vector<std::wstring> &args, const std::string &title, const std::string &pack);
        void pollSoundJob();
        void soundAddFolder(const fs::path &folder); // Add sounds: import every audio file in a folder as a new pack
        // From a recording: sound_pack.py sweep of one file into the chosen pack (or a new one named after the file)
        void soundSweepFile(const fs::path &file, const std::string &packFolder);
        bool soundSweepOff_ = false; // the recording is off throttle (a coast-down)
        int soundSweepCyl_ = 4;      // its cylinders: 2, 3, 4, 5, 6 (the default), 8, 10, 12, 16
        std::vector<SoundPackInfo> soundPacks_;
        double soundPacksAt_ = -100.0;
        std::string soundSel_;   // the pack chosen in the list (its folder)
        std::string pythonPath_; // the Python the jobs run ("" none found)
        bool pythonLooked_ = false;
        struct SoundJob
        {
            sys::Process proc;
            int kind = 0; // 1 import, 2 check, 3 preview, 4 sweep
            bool running = false;
            fs::path log, wav;
            std::string title, pack;
            uint64_t startMs = 0;
        } soundJob_;
        std::string soundOutTitle_;            // the last job's title ("Checked starter-v6")
        std::vector<std::string> soundOut_;    // and its output lines
        bool soundOutBad_ = false;
        bool soundPlaying_ = false;
        int soundCarPick_ = 0;                 // per car: the garage car the list adds
        void pageRoadmap();  // Roadmap: what is coming (a plain list)
        void pageRaceLab();  // Race Lab: record laps, open the Race Lab page (gt4RaceLab.exe)
        // Driving > Race HUD > HUD layout (gt4full_racehud.cpp; HUD hand-off 7 A2): the race picture with GT4's parts and our
        // boxes, dragged where the player wants them.
        // Driving > Telemetry screen (gt4full_telemetry.cpp; HUD hand-off 8): the telemetry monitor, the phone's QR code and
        // the three layouts (telemetry-layout.json beside gt4-start.env).
        void pageTelemetry();
        TeleState &teleState();
        fs::path teleLayoutFile() const;
        void teleWrite();       // the layouts' file, when changed (saveNow)
        void teleMirrorCheck(); // GT4L_TELE_MIRROR from the layouts in use
        void teleEditor(float x0, float top, float x1, float bottom);
        void teleDrawItem(const tele::Item &it, Rectangle r, bool sel);
        void teleShot(int scene); // the hidden test pictures' starting points
        const std::vector<Rectangle> &teleBoxes() const;
        std::shared_ptr<TeleState> tele_;
        std::vector<Rectangle> teleBoxDp_;
        void pageRaceHud();
        void raceHudLoadSets(bool force = false);
        bool raceHudTextures(); // the shown set's pictures as textures (false: none)
        fs::path raceHudFolder() const;
        std::vector<RaceHudSet> hudSets_;
        double hudSetsAt_ = -100.0;
        int hudSetIdx_ = 0;
        Texture2D hudClean_{}, hudHud_{}, hudCut_{}; // (hudCut_: GT4's parts cut out of the -hud picture)
        std::string hudTexKey_;
        std::string hudSel_ = "radar"; // the picked box
        std::string hudDrag_;          // the box being dragged ("" none)
        Vector2 hudDragOff_{0, 0};
        // (HUD hand-off 7 C) the race map's course art for the picture's course (pages\tracks\<course>.json next to GT4.exe)
        bool raceHudArt(int course);
        int hudArtCourse_ = -1;
        Texture2D hudArt_{};
        float hudArtW_ = 0, hudArtH_ = 0;
        std::vector<Vector2> hudArtLine_;
        // Driving > Race HUD's pages (HUD hand-off 7 D, E): 0 HUD layout, 1 Radar, 2 Start lights (gt4full_racehud.cpp), 3 Timing
        // (the timing tower and split times, gt4full_racetiming.cpp)
        int raceHudTab_ = 0;
        void pageRaceHudLayout(float top);
        void pageRaceHudRadar(float top);
        void pageRaceHudLights(float top);
        void pageRaceHudTiming(float top);
        // The Start lights page's "Try a start" (launcher only): when it was started (GetTime(), -1e9: not), when the lights go
        // out, and the last result.
        double lightsTryAt_ = -1e9, lightsTryOut_ = 0.0;
        std::string lightsTryResult_;
        Texture2D radarStockHud_{};                  // the stock chase picture with GT4's HUD (the Radar page's preview)
        sys::Process raceLab_;
        std::string raceLabNote_;
        void pageProgress(); // Saves > Game progress
        void pageGarage();
        void garageMine(float x0, float y0, float x1, float y1);
        void garageCompare(float x0, float y0, float x1, float y1);
        void garageDealers(float x0, float y0, float x1, float y1);
        void garagePrizes(float x0, float y0, float x1, float y1); // GT Mode prizes: what each event, licence and mission gives
        // blackGrey > 0: a black car (blackCarGrey): the launcher's render of it in black (blackCarPictureResource), else GT4's
        // picture darkened to that black (blackenPicture).
        void drawCarPicture(Rectangle r, const std::string &label, bool frame, int blackGrey = 0);
        void garageSourceLine(float x, float y, float w);
        // The car data (a worker thread after the names), the garage and progress of the chosen card (read only).
        void startCatalogue();
        void pollCatalogue(bool wait = false);
        // Extra cars (analysis/jp-cars; no switch since 2026-10-14): while extra-cars\esso.gt4x sits next to a GT4 program that
        // serves it (a Full GT4.exe, not the Classic one; not with GT4_EXTRA_CARS=off in the environment) the disc readers
        // serve it too (gt4save::Gt4Disc::useExtraCars), so the Garage and Car Dealers see the cars GT4.exe adds; a change
        // reads the names and car data again. Called every frame; it looks for the pack every couple of seconds.
        void applyExtraCars();
        fs::path extraCarsPack() const; // extra-cars\esso.gt4x next to GT4.exe
        bool extraCarsServed() const { return extraCarsKey_ != "?" && !extraCarsKey_.empty() && extraCarsNote_.empty(); }
        // Saves: which garage slots of each card's save hold an extra car (code 229), so the page can warn that GT4 without
        // the pack (the Classic GT4 among them) does not know it (analysis/jp-cars/README-BUILD.md). Read in refreshCards.
        void scanExtraCarsInSaves();
        void loadGarage(bool force = false);
        fs::path garageSource(std::string *title) const;
        const Texture2D *carTexture(const std::string &label, int blackGrey = 0);
        void unloadFull();
        // Tests
        int runFullTestShots();
        void reportFullDryRun(const sys::Launch &l);
        void adjustFullLaunch(sys::Launch &l) const; // the values this GT4 program understands
        // GT4's own Multi Monitor Mode (Multi-screen; gt4full_app.cpp): one GT4 copy per screen, the middle one started as
        // Play does, the side ones (mmSideLaunches) right after it and closed with it.
        bool mmOwnChosen() const { return st_.full.mmMode == kMmGt4Own; }
        bool mmOwnLive() const; // chosen, this GT4 program has it, and at least one side screen was found
        // The monitor of a copy: the middle one is Display > Screen shape's; left / right right next to it (half its height
        // shared), top right above it. Null when there is none.
        const MonitorInfo *mmMonitor(const std::string &role) const;
        std::vector<FullMmCopy> mmPlan() const; // the copies whose monitors were found (the middle one first)
        std::string mmGroup() const;            // the copies' group (GT4_MM_GROUP): this launcher's process
        std::vector<sys::Launch> mmSideLaunches(const sys::Launch &centre) const;
        bool mmCardCopy(const std::string &role, sys::Launch &l, std::string &why) const; // a fresh copy of slot 1's card
        void startMmSides(const sys::Launch &centre);
        void closeMmSides(bool hard);
        std::vector<sys::Process> mmSides_;
        static void setLaunchKey(sys::Launch &l, const std::string &key, const std::string &value);
        void reportGarage(const std::string &scene);
        // The Garage's changes to the save (gt4full_saveedit.h): what the player asked for, the plan the confirmation
        // lists, where it goes, and the result.
        EditTarget garageEditTarget() const;
        std::string garageEditBlock() const; // why the Garage cannot change the save now ("" when it can)
        void openGarageEdit(const GarageEdit &e, const EditTarget *target = nullptr); // target: else garageEditTarget()
        void confirmGarageEdit();
        // Save details' Repair and Change name (developer 2026-10-09): the save the page shows, when it is a folder card's.
        EditTarget detailsEditTarget() const;
        void drawRenameModal();
        std::string nameEdit_; // Change name: the name being typed
        void drawGarageEditModal();
        void drawGarageDoneModal();
        void drawCarSpecsModal();
        // A Garage change that was cut off (its note still in gameackups): found at the start, the backup offered.
        void checkPendingChange();
        void drawGarageCutOffModal();
        PendingChange pending_;
        int runEditTest();
        int runBuildTest();
        GarageEdit editReq_;
        EditPlan editPlan_;
        EditTarget editTarget_;
        EditResult editResult_;
        std::string specsLabel_; // Car Dealers' Specifications

        FullFeatures full_;
        bool showUnbuilt_ = true;
        bool previewFullscreen_ = false;
        RenderTexture2D previewRaw_{}; // the picture without the settings (nearest, no lines)
        Texture2D race16_{};           // the race picture as GT4's 16-bit race colour draws it (GS dithering)
        bool previewJustOpened_ = false;
        Vector2 zoomFocus_{0.235f, 0.135f}; // where the zoom looks, 0..1 of the picture (follows the mouse)
        sys::Process prepare_;
        bool preparing_ = false;
        std::string prepareStatus_;
        struct BuildJob
        {
            sys::Process proc;
            bool running = false, done = false, failed = false;
            int step = 0, of = 7;       // from gt4setup's "@step <n> <of> <name>: <what>" lines
            std::string what, error;    // the step's words; the failure's last lines
            uint64_t startMs = 0, stepMs = 0, endMs = 0, readMs = 0;
            fs::path log;               // <kit>\build.log
        } build_;
        bool buildShot_ = false; // test shots: Play as a release without GT4.exe shows it (no build-kit needed)
        std::vector<sys::NetAddress> netAddresses_;
        bool netListed_ = false;
        int garageTab_ = 0;     // 0 My garage, 1 Compare cars, 2 Car Dealers, 3 GT Mode prizes
        int prizeGroup_ = 0;    // GT Mode prizes: the group shown (buildPrizeGuide's order)
        std::string prizeFocus_; // GT Mode prizes: the entry to bring into view and mark (from Prize Cars' link)
        bool prizeFocusShown_ = false; // the list was scrolled to prizeFocus_ once
        std::vector<GuideGroup> guide_;
        std::string guideKey_;
        int garageSel_ = -1;    // the garage slot shown
        int garageSort_ = 0;    // 0 GT4's order, 1 name, 2 power
        int dealerSection_ = 0; // 0 Hidden Cars, 1 Prize Cars, 2 Extra Cars
        std::string extraCarsKey_ = "?"; // the pack the disc readers serve ("" none; "?" not set yet)
        std::string extraCarsNote_;      // why the pack is not served (damaged, another disc)
        int extraCarsTick_ = 0;          // frames since the pack was last looked for
        struct SaveExtraCars             // Saves: a card's save and the extra cars in its garage
        {
            std::string key;             // the save file and its time (read again when it changes)
            std::vector<int> slots;      // garage slots holding the ESSO (code 229)
            bool riding = false;         // one of them is the car being driven
        };
        std::array<SaveExtraCars, 2> savesExtra_{};
        std::string catKey_;             // namesKey() the car data was read for
        int dealerMaker_ = 0;   // 0 all, then the makers in the list
        std::string dealerSel_; // the car label shown
        std::string dealerColourFor_; // the car the colour choice is for (another car selected: its default colour again)
        int dealerColour_ = 0;        // the colour chosen (an index of the car's GT4 colour list)
        bool compareStatsOpen_ = false; // Compare cars: the "Stats to show" list is open
        bool savesProgress_ = false; // Saves shows Game progress
        int progressTab_ = 0;        // 0 Overview, 1 GT Mode, 2 Arcade
        struct CatalogueLoad
        {
            std::shared_ptr<CarCatalogue> cat;
            std::string error;
            double ms = 0.0;
        };
        std::future<CatalogueLoad> catFuture_;
        bool catLoading_ = false;
        std::shared_ptr<const CarCatalogue> cat_;
        std::string catError_;
        double catMs_ = 0.0;
        Garage garage_;
        Progress progress_;
        std::string garageKey_; // the source and the names / car data it was built with
        std::string garageTitle_;
        fs::path garageCopy_; // test shots: a save copy in place of the card
        std::map<std::string, Texture2D> carTex_;
#endif

        Options opt_;
        Install inst_;
        Settings st_;
        std::vector<std::string> warnings_;
        bool settingsFileExisted_ = false;
        bool dirty_ = false;
        double dirtyAt_ = 0.0;
        bool hidden_ = false; // the hidden test mode
        std::vector<std::string> reportLines_;

        ui::Ui ui_;
        Devices devices_;
        Cameras cameras_; // the EyeToy page's webcam list (listed, never opened)
        double lastCameraList_ = -100.0;
        ShirtDesign shirt_;
        Texture2D shirtTex_{};
        std::string shirtError_;
        scan::Preview preview_;
        Texture2D pictures_[3]{}; // race, menu, title (640x448)
        int pictureW_[3]{}, pictureH_[3]{};
        // The same pictures as Bob shows a still frame: the even lines (one field), the odd ones the average of
        // the lines above and below (made with the pictures).
        Texture2D picturesBob_[3]{};
        RenderTexture2D previewOn_{}, previewOff_{};
        std::string previewKey_;
        float previewSplit_ = 0.5f;
        bool draggingSplit_ = false;
        std::string glRenderer_;

        std::vector<MonitorInfo> monitors_;
        DiscCheck disc_;
        CardInfo cards_[2];
        CardFileInfo cardFiles_[2];
        PhotoInfo photos_;
        std::vector<BackupInfo> backups_;
        double lastRefresh_ = -100.0;

        Page page_ = Page::Play;
        int controllersTab_ = 0; // 0 controller, 1 wheel & pedals, 2 EyeToy camera
        int savesView_ = 0;      // 0 the cards, 1 Save details
        int aboutView_ = 0;      // 0 About, 1 How this works

        // Save details (read only: nothing here writes to a card)
        SaveDetails details_;
        bool detailsOpen_ = false;
        int detailsSlot_ = 0;        // 1, 2, or 0 for a copy
        fs::path detailsSource_;     // the folder card, the card file or the copy
        std::string detailsTitle_;   // where the save is ("Memory card slot 1, folder card game\mc0")
        bool detailsNames_ = false;  // built with the disc's names
        int expandedCar_ = -1;       // the garage slot shown open
        bool showTechnical_ = false;
        double detailsMs_ = 0.0;

        // The disc's names (cars, colours, events, parts), loaded in the background
        struct NamesLoad
        {
            std::shared_ptr<GameNames> names;
            std::string error;
            double ms = 0.0;
        };
        std::future<NamesLoad> namesFuture_;
        bool namesLoading_ = false;
        std::shared_ptr<const GameNames> names_;
        std::string namesError_, namesIso_;
        double namesMs_ = 0.0;

        Doc howDoc_; // "How it works" (a resource: gt4stock_how_it_works.md, the full explainer)
        RecompAnswer recomp_; // About's "Is it a full recompilation?" card, from the text
        DocLayout howLayout_;
        float howLayoutW_ = -1.0f, howLayoutS_ = -1.0f;
        std::string howAnchor_;      // test pictures: scroll the text to this anchor (DocLayout::anchors)
        sys::GpuChoice gpu_;         // the graphics adapter GT4.exe runs on (About's Graphics row)
        // (1.4 review) What the GT4 program to be started reads (its strings, read only): the cheat switches, the in-game
        // card, the Mission Hall fix, player 2. A switch it does not read is shown off and greyed, with a plain line.
        ExeFeatures gt4_;
        ExeFeatures gt4Real_; // the real program's (the demo pictures assume the runtimes are in)
        int howJump_ = -1;           // the section "On this page" last jumped to (-1: none)
        float howJumpOff_ = 0.0f;    // where that jump left the text
        bool howJumpPending_ = false; // the jump applies on the next frame
        std::string status_;
        Color statusColor_{};
        double statusAt_ = 0.0;

        // the running game
        sys::Process game_;
        bool gameRunning_ = false, otherGameRunning_ = false;
        double lastProcessCheck_ = -100.0;

        // The Cheats page (1.4)
        int cheatSel_ = 0;                // the card shown (gt4cheats::kCodes)
        long long cheatElapsed_ = -1;     // the slot-1 save's getElapsedDate()
        int cheatSaveState_ = 0;          // 0 no GT4 save, 1 read, 2 not readable
        std::string cheatSaveKey_;        // read again when it changes
        bool cheatPractise_ = false;      // Practise with my pad (in Show big)
        gt4cheats::Follower cheatFollower_;
        int cheatFlash_ = 0;              // 1 done, 2 a wrong button
        double cheatFlashAt_ = -100.0;
        int cheatTestProgress_ = 6;       // the test pictures' Practise state
        bool cheatInk_ = true;            // the sheet on white paper
        bool cheatBodyScrolls_ = false;   // the card did not fit (the smallest windows): its body scrolls
        std::string cheatLevelText_;      // what the card left out to fit (the test report)
        PressureHeld practiseHeld_{};     // Practise: the pressure buttons held (GT4's 10 / 5 hysteresis)
        Texture2D cheatThumb_{};          // the sheet's preview (page 1)
        std::string cheatThumbKey_;
        int cheatThumbPages_ = 0;
        enum class CheatJob
        {
            None,
            Card,     // Save card: the card shown
            Pictures, // Save pictures: every ticked card
            Pdf,      // Save PDF (Shift: Save as...)
            PdfAs,
            Print,
        } cheatJob_ = CheatJob::None;
        std::vector<fs::path> lastCheatFiles_; // what Save wrote (the test report)

        // modals and captures
        enum class Modal
        {
            None,
            Restore,
            Calibrate,
            WheelButtons,
            Message,
            CheatBig,  // 1.4: Show big (and Practise)
            CheatSave, // 1.4: Save or print cheat cards
#if defined(GT4L_EDITION_FULL)
            GarageConfirm, // the Garage: what will change in the save, Confirm / Cancel
            GarageResult,  // the Garage: what happened
            CarSpecs,      // Car Dealers: a car's specifications
            GarageCutOff,  // a Garage change was cut off: restore its backup?
            RenameSave,    // Save details > Change name: the new name (then the Garage's confirmation)
#endif
        } modal_ = Modal::None;
        fs::path restoreZip_;
        int restoreSlot_ = 1;
        int calibratePedal_ = 0; // 0 gas, 1 brake
        bool calibrateHaveRest_ = false;
        int calibrateRest_ = 0, calibrateFar_ = 0;
        std::string messageTitle_, messageText_;
        int capture_ = -1;  // pad row being captured: 0..15 button, 100+p pressure; 200 the Race HUD zoom key, 201 its button
        double captureAt_ = 0.0;
        std::array<float, 6> captureRest_{};
        std::string ffbMessage_;
    };
}
