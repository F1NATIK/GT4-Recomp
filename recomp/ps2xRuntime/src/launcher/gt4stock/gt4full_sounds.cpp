// GT4 Recompilation launcher, Full edition: Sound > Sound packs (Custom Sounds, analysis/design/sound-packs). The player's own
// engine sounds: a pack is a folder with pack.json and its WAV files, made from the player's recordings by sound_pack.py
// (standard-library Python; the launcher carries the script, gt4full_sound_pack.py, and writes it next to the packs). GT4.exe
// (a development / Full one: gt4_sound.cpp) plays a pack for the car the player drives in place of GT4's own engine:
// GT4_SOUND_PACK=<folder> for every car, or GT4_SOUND_PACK_MAP=<file> per car (the launcher writes the file). Python runs
// only on the player's click (Add sounds, From a recording, Check, Preview), never in the hidden test mode.

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4stock_resources.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;
    using fullui::Pill;

    namespace
    {
        constexpr float kGap = 16.0f;
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        bool audioFile(const fs::path &p)
        {
            const std::string e = lower(pathText(p.extension()));
            return e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac" || e == ".m4a" || e == ".aif" || e == ".aiff";
        }

        // A folder name for a new pack: letters, digits, space, - and _ (others become _), at most 48.
        std::string packFolderName(const std::string &from)
        {
            std::string out;
            for (char c : from)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                out.push_back(std::isalnum(u) || c == '-' || c == '_' || c == ' ' ? c : '_');
                if (out.size() >= 48)
                    break;
            }
            out = trim(out);
            return out.empty() ? std::string("my-pack") : out;
        }

        std::vector<std::string> lastLines(const fs::path &file, size_t n)
        {
            std::ifstream in(file, std::ios::binary);
            std::vector<std::string> all;
            std::string line;
            while (std::getline(in, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                if (!trim(line).empty())
                    all.push_back(line);
            }
            if (all.size() > n)
                all.erase(all.begin(), all.end() - static_cast<ptrdiff_t>(n));
            return all;
        }
    }

    fs::path App::soundPacksFolder() const { return (inst_.exeFound ? inst_.exe.parent_path() : inst_.root) / "sound-packs"; }

    void App::writeSoundMap()
    {
        // Only while per car is chosen (GT4_SOUND_PACK_MAP names the file then); the file is the launcher's own.
        const FullSettings &f = st_.full;
        if (f.soundMode != FullSoundMode::PerCar || f.soundMapFile.empty() || fullSoundMapValue(f) == "off")
            return;
        std::error_code ec;
        const fs::path file = pathFrom(f.soundMapFile);
        fs::create_directories(file.parent_path(), ec);
        std::string error;
        if (!writeFileAtomic(file, fullSoundMapText(f), error))
            setStatus("Could not write the sound pack list: " + error, ui::theme().bad);
    }

    void App::refreshSoundPacks(bool force)
    {
        if (!force && GetTime() - soundPacksAt_ < 3.0)
            return;
        soundPacksAt_ = GetTime();
        if (opt_.demo || hidden_)
        {
            // The test pictures: the starter pack and one with pops, as Add sounds would have made them.
            if (soundPacks_.empty())
            {
                SoundPackInfo a;
                a.folder = pathText(soundPacksFolder() / "starter-v6");
                a.name = "starter-v6";
                a.on = a.off = 5;
                a.bassDb = 4.0;
                a.ok = true;
                SoundPackInfo b = a;
                b.folder = pathText(soundPacksFolder() / "My Supra recording");
                b.name = "My Supra recording";
                b.on = 6;
                b.off = 4;
                b.pops = 3;
                b.bassDb = 0.0;
                soundPacks_ = {b, a};
                if (soundSel_.empty())
                    soundSel_ = a.folder;
            }
            return;
        }
        soundPacks_ = listSoundPacks(pathText(soundPacksFolder()));
        if (soundSel_.empty() && !soundPacks_.empty())
            soundSel_ = soundPacks_.front().folder;
    }

    bool App::soundImporter(fs::path &script, std::string &why)
    {
        // The launcher's copy of sound_pack.py (a resource), written next to the packs when missing or different.
        const std::vector<uint8_t> res = sys::resource(GT4FULL_IDR_SOUND_PACK_PY);
        if (res.empty())
        {
            why = "this launcher has no sound_pack.py inside it";
            return false;
        }
        std::error_code ec;
        const fs::path dir = soundPacksFolder();
        fs::create_directories(dir, ec);
        script = dir / "sound_pack.py";
        std::ifstream in(script, std::ios::binary);
        const std::vector<uint8_t> have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (have == res)
            return true;
        std::string error;
        if (!writeFileAtomic(script, std::string(res.begin(), res.end()), error))
        {
            why = "could not write " + pathText(script) + ": " + error;
            return false;
        }
        return true;
    }

    void App::startSoundJob(int kind, const std::vector<std::wstring> &args, const std::string &title, const std::string &pack)
    {
        if (soundJob_.running)
            return;
        if (!pythonLooked_)
        {
            pythonPath_ = sys::findPython();
            pythonLooked_ = true;
        }
        soundOutTitle_ = title;
        soundOut_.clear();
        soundOutBad_ = false;
        if (pythonPath_.empty())
        {
            soundOutBad_ = true;
            soundOut_ = {"Python is not installed on this PC, so the launcher cannot make, check or preview a pack.",
                         "Install Python 3 from python.org (tick \"Add python.exe to PATH\"), then come back here.",
                         "Packs you already have still work: GT4 reads them itself."};
            return;
        }
        fs::path script;
        std::string why;
        if (!soundImporter(script, why))
        {
            soundOutBad_ = true;
            soundOut_ = {why};
            return;
        }
        std::error_code ec;
        soundJob_ = SoundJob{};
        soundJob_.kind = kind;
        soundJob_.title = title;
        soundJob_.pack = pack;
        soundJob_.log = fs::temp_directory_path(ec) / "gt4-sound-pack-job.txt";
        sys::Launch l;
        l.exe = pathFrom(pythonPath_);
        l.args = {L"-I", script.wstring()};
        l.args.insert(l.args.end(), args.begin(), args.end());
        l.cwd = soundPacksFolder();
        l.log = soundJob_.log;
        if (kind == 3 && !args.empty())
            soundJob_.wav = fs::path(args.back());
        std::string error;
        if (!sys::start(l, soundJob_.proc, error))
        {
            soundOutBad_ = true;
            soundOut_ = {"Could not start Python: " + error};
            return;
        }
        soundJob_.running = true;
        soundJob_.startMs = sys::tickMs();
        soundOut_ = {"Working..."};
    }

    void App::pollSoundJob()
    {
        if (!soundJob_.running)
            return;
        int code = 0;
        if (!sys::poll(soundJob_.proc, code))
        {
            if (sys::tickMs() - soundJob_.startMs > 120000)
            {
                sys::terminate(soundJob_.proc);
                soundJob_.running = false;
                soundOutBad_ = true;
                soundOut_ = {"Python took more than two minutes and was stopped."};
            }
            return;
        }
        soundJob_.running = false;
        soundOut_ = lastLines(soundJob_.log, 9);
        soundOutBad_ = code != 0;
        if (soundOut_.empty())
            soundOut_ = {code ? strf("Python stopped with code %d and said nothing.", code) : std::string("Done.")};
        refreshSoundPacks(true);
        if ((soundJob_.kind == 1 || soundJob_.kind == 4) && code == 0)
            soundSel_ = soundJob_.pack;
        if (soundJob_.kind == 3 && code == 0)
        {
            soundPlaying_ = sys::playWav(pathText(soundJob_.wav));
            soundOut_.push_back(soundPlaying_ ? "Playing the 12-second test drive." : "Windows could not play " + pathText(soundJob_.wav) + ".");
        }
    }

    void App::soundAddFolder(const fs::path &folder)
    {
        std::vector<std::wstring> files;
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec) && audioFile(e.path()))
                files.push_back(e.path().wstring());
        std::sort(files.begin(), files.end());
        if (files.empty())
        {
            soundOutTitle_ = "Add sounds";
            soundOutBad_ = true;
            soundOut_ = {"No sound files in " + pathText(folder) + " (WAV; OGG, MP3, FLAC and M4A need ffmpeg)."};
            return;
        }
        const std::string base = packFolderName(pathText(folder.filename()));
        fs::path out = soundPacksFolder() / pathFrom(base);
        for (int n = 2; fs::exists(out, ec) && n < 100; ++n)
            out = soundPacksFolder() / pathFrom(strf("%s-%d", base.c_str(), n));
        std::vector<std::wstring> args = {L"import"};
        args.insert(args.end(), files.begin(), files.end());
        args.insert(args.end(), {L"--out", out.wstring(), L"--name", pathFrom(pathText(out.filename())).wstring()});
        startSoundJob(1, args, "Made " + pathText(out.filename()) + strf(" from %zu files", files.size()), pathText(out));
    }

    void App::soundSweepFile(const fs::path &file, const std::string &packFolder)
    {
        // sound_pack.py sweep: one long recording (a slow rev-up on the throttle, or a coast-down off it) followed by pitch,
        // a loop cut every 1500 rpm, added to the chosen pack (or a new one named after the file).
        static const char *const kCyl[] = {"2", "3", "4", "5", "6", "8", "10", "12", "16"};
        std::error_code ec;
        fs::path out = packFolder.empty() ? fs::path() : pathFrom(packFolder);
        if (out.empty())
        {
            const std::string base = packFolderName(pathText(file.stem()));
            out = soundPacksFolder() / pathFrom(base);
            for (int n = 2; fs::exists(out, ec) && n < 100; ++n)
                out = soundPacksFolder() / pathFrom(strf("%s-%d", base.c_str(), n));
        }
        const std::string cyl = kCyl[std::clamp(soundSweepCyl_, 0, 8)];
        std::vector<std::wstring> args = {L"sweep", file.wstring(), L"--load", soundSweepOff_ ? L"off" : L"on", L"--cylinders",
                                          pathFrom(cyl).wstring(), L"--out", out.wstring()};
        if (packFolder.empty())
            args.insert(args.end(), {L"--name", pathFrom(pathText(out.filename())).wstring()});
        startSoundJob(4, args,
                      "Loops from " + pathText(file.filename()) + " (" + (soundSweepOff_ ? "off" : "on") + " throttle, " + cyl + " cylinders) into " +
                          pathText(out.filename()),
                      pathText(out));
    }

    void App::pageSoundPacks(float x0, float top, float x1, float bottom)
    {
        const ui::Theme &t = ui::theme();
        FullSettings &f = st_.full;
        f.soundMapFile = pathText(soundPacksFolder() / "cars.txt"); // (set at the start too; kept after a settings reset)
        pollSoundJob();
        refreshSoundPacks();
        if (!pythonLooked_ && !hidden_ && !opt_.demo)
        {
            pythonPath_ = sys::findPython();
            pythonLooked_ = true;
        }
        const bool demoLook = hidden_ || opt_.demo;
        const bool live = !full_.read || full_.soundPacks;
        const float cw = x1 - x0;
        const float lw = std::floor(std::min(620.0f, cw * 0.52f));
        auto packName = [&](const std::string &folder) -> std::string {
            if (folder.empty())
                return std::string();
            for (const SoundPackInfo &p : soundPacks_)
                if (p.folder == folder)
                    return p.name;
            return pathText(pathFrom(folder).filename());
        };
        const SoundPackInfo *sel = nullptr;
        for (const SoundPackInfo &p : soundPacks_)
            if (p.folder == soundSel_)
                sel = &p;

        // ---- left: the packs
        {
            const Rectangle c = R(x0, top, lw, bottom - top);
            fullui::card(ui_, c, "Your sound packs", {Pill::Full, Pill::Experimental});
            float y = c.y + 50;
            y += ui_.wrap("Your own engine sounds: recordings of a car at several revs, on and off the throttle, from a free sound pack, "
                          "a real car or audio you made. GT4 plays them for the car you drive, crossfading with the revs, with pops "
                          "when you lift off. Put the recordings of one car in a folder and choose Add sounds.",
                          c.x + 20, y, c.width - 40, 13.0f, t.muted) +
                 8;
            // Python: found, or what to do.
            std::string py = demoLook ? std::string("Python 3 found: C:\\Users\\you\\AppData\\Local\\Programs\\Python\\Python314\\python.exe") : pythonPath_;
            if (py.empty())
                y += ui_.wrap("Python is not installed: Add sounds, Check and Preview need Python 3 (python.org, free). Packs you already "
                              "have still work without it.",
                              c.x + 20, y, c.width - 40, 12.5f, t.warn) +
                     8;
            else
            {
                ui_.icon(Icon::Check, R(c.x + 20, y + 1, 14, 14), t.ok);
                ui_.text(ui_.fit(demoLook ? py : "Python: " + py, c.width - 64, 12.0f), c.x + 40, y, 12.0f, t.faint);
                y += 22;
            }
            ui_.text(ui_.fit("Packs in " + pathText(soundPacksFolder()), c.width - 40, 12.0f), c.x + 20, y, 12.0f, t.faint);
            y += 22;
            // the list
            const float listBottom = std::max(y + 100.0f, c.y + c.height - 330.0f);
            if (soundPacks_.empty())
            {
                ui_.wrap("No packs yet. Choose Add sounds and pick the folder that holds one car's recordings.", c.x + 20, y, c.width - 40, 13.0f, t.muted);
                y += 40;
            }
            else
            {
                for (const SoundPackInfo &p : soundPacks_)
                {
                    if (y + 46 > listBottom)
                    {
                        ui_.text(strf("and %zu more", soundPacks_.size()), c.x + 20, y, 12.0f, t.faint);
                        y += 20;
                        break;
                    }
                    const Rectangle r = R(c.x + 10, y, c.width - 20, 44);
                    const bool chosen = p.folder == soundSel_;
                    if (ui_.rowButton("pack-" + p.folder, r, chosen, !soundJob_.running))
                        soundSel_ = p.folder;
                    ui_.text(ui_.fit(p.name, r.width - 30, 14.0f, Weight::Semibold), r.x + 14, r.y + 5, 14.0f, chosen ? t.text : t.muted, Weight::Semibold);
                    std::string what = p.ok ? strf("%d on-throttle, %d off-throttle loops", p.on, p.off) + (p.pops ? strf(", %d pops", p.pops) : std::string(", built-in pops"))
                                           : "cannot be used: " + p.problem;
                    if (p.ok && p.bassDb != 0.0)
                        what += strf(", bass %+.0f dB", p.bassDb);
                    ui_.text(ui_.fit(what, r.width - 30, 12.0f), r.x + 14, r.y + 25, 12.0f, p.ok ? t.faint : t.warn);
                    y += 48;
                }
            }
            y = std::max(y + 6, listBottom);
            // the buttons
            const bool canRun = !soundJob_.running && !demoLook;
            float bx = c.x + 20;
            const float bh = 34;
            // row 1: make a pack from a folder of recordings, or add loops cut from one rev-sweep recording (sound_pack.py sweep)
            const float half = std::floor((c.width - 40 - 8) / 2);
            if (ui_.button("pack-add", R(bx, y, half, bh), "Add sounds...", Style::Primary, canRun, 13.5f))
            {
                const fs::path folder = sys::browseForFolder(GetWindowHandle(), L"The folder with one car's recordings (on_3000.wav, 4500_off.wav, ...)", soundPacksFolder());
                if (!folder.empty())
                    soundAddFolder(folder);
            }
            if (ui_.button("pack-sweep", R(bx + half + 8, y, half, bh), "From a recording...", Style::Secondary, canRun, 13.5f))
            {
                const fs::path file = sys::browseForFile(GetWindowHandle(), L"One recording: a slow rev-up on the throttle, or a coast-down off it",
                                                         std::wstring(L"Sound files (*.wav;*.ogg;*.mp3;*.flac;*.m4a)\0*.wav;*.ogg;*.mp3;*.flac;*.m4a\0All files\0*.*\0\0", 91),
                                                         soundPacksFolder());
                if (!file.empty())
                    soundSweepFile(file, sel ? sel->folder : std::string());
            }
            y += bh + 8;
            // row 2: what the recording is (for From a recording)
            ui_.text("The recording:", c.x + 20, y + 8, 12.5f, t.faint);
            int load = soundSweepOff_ ? 1 : 0;
            if (ui_.segmented("pack-sweep-load", R(c.x + 120, y, 220, 30), {"On throttle", "Off throttle"}, load, canRun, 12.5f))
                soundSweepOff_ = load == 1;
            ui_.text("Cylinders", c.x + 356, y + 8, 12.5f, t.faint);
            static const std::vector<std::string> kCyl = {"2", "3", "4", "5", "6", "8", "10", "12", "16"};
            soundSweepCyl_ = std::clamp(soundSweepCyl_, 0, int(kCyl.size()) - 1);
            ui_.dropdown("pack-sweep-cyl", R(c.x + 430, y, std::min(90.0f, c.width - 450), 30), kCyl, soundSweepCyl_, canRun, 12.5f);
            y += 30 + 6;
            ui_.text(ui_.fit(sel ? "From a recording adds its loops to " + sel->name + "." : "From a recording makes a new pack named after the file.",
                             c.width - 40, 11.5f),
                     c.x + 20, y, 11.5f, t.faint);
            y += 22;
            // row 3: the chosen pack
            const float avail = c.width - 40 - 3 * 8;
            const float wCheck = std::floor(avail * 0.22f), wPrev = std::floor(avail * 0.26f), wStop = std::floor(avail * 0.2f),
                        wFolder = avail - wCheck - wPrev - wStop;
            if (ui_.button("pack-check", R(bx, y, wCheck, bh), "Check", Style::Secondary, canRun && sel, 13.5f))
                startSoundJob(2, {L"check", pathFrom(sel->folder).wstring()}, "Checked " + sel->name, sel->folder);
            bx += wCheck + 8;
            if (ui_.button("pack-preview", R(bx, y, wPrev, bh), "Preview", Style::Secondary, canRun && sel && sel->ok, 13.5f))
            {
                std::error_code ec;
                const fs::path wav = fs::temp_directory_path(ec) / "gt4-sound-pack-preview.wav";
                sys::stopWav();
                soundPlaying_ = false;
                startSoundJob(3, {L"preview", pathFrom(sel->folder).wstring(), wav.wstring(), L"--seed", L"1"}, "Preview of " + sel->name, sel->folder);
            }
            bx += wPrev + 8;
            if (ui_.button("pack-stop", R(bx, y, wStop, bh), "Stop", Style::Ghost, soundPlaying_, 13.5f))
            {
                sys::stopWav();
                soundPlaying_ = false;
            }
            bx += wStop + 8;
            if (ui_.button("pack-folder", R(bx, y, wFolder, bh), "Open folder", Style::Ghost, !demoLook, 13.5f))
            {
                std::error_code ec;
                fs::create_directories(soundPacksFolder(), ec);
                sys::openInExplorer(soundPacksFolder());
            }
            y += bh + 12;
            // the last job's output
            std::vector<std::string> out = soundOut_;
            std::string title = soundOutTitle_;
            if (demoLook && out.empty())
            {
                title = "Checked starter-v6";
                out = {"  note: starter_idle.wav: rpm 900 was guessed from the pitch, check it", "  note: no pops: backfires will use the built-in ones", "  OK"};
            }
            if (!out.empty())
            {
                ui_.text(title + (soundJob_.running ? "  (working)" : ""), c.x + 20, y, 12.5f, soundOutBad_ ? t.warn : t.faint, Weight::Semibold);
                y += 20;
                for (const std::string &s : out)
                {
                    if (y + 16 > c.y + c.height - 8)
                        break;
                    ui_.text(ui_.fit(s, c.width - 40, 12.0f), c.x + 20, y, 12.0f, soundOutBad_ ? t.warn : t.muted, Weight::Mono);
                    y += 17;
                }
            }
        }

        // ---- right: which cars
        {
            const float rx = x0 + lw + kGap;
            const Rectangle c = R(rx, top, x1 - rx, bottom - top);
            fullui::card(ui_, c, "Which cars", {Pill::Full});
            float y = c.y + 50;
            static const char *const hints[3] = {"GT4's own engine sounds", "the pack chosen here for every car you drive",
                                                 "a pack for each car you choose; the others as set below"};
            const FullSoundMode modes[3] = {FullSoundMode::Off, FullSoundMode::EveryCar, FullSoundMode::PerCar};
            for (int i = 0; i < 3; ++i)
            {
                if (fullui::radio(ui_, strf("sound-mode-%d", i), R(c.x + 10, y, c.width - 20, 34), f.soundMode == modes[i], fullSoundModeLabel(modes[i]), hints[i],
                                  live, !live))
                {
                    f.soundMode = modes[i];
                    if (modes[i] != FullSoundMode::Off && f.soundPackAll.empty() && sel && sel->ok && modes[i] == FullSoundMode::EveryCar)
                        f.soundPackAll = sel->folder;
                    markDirty();
                }
                y += 36;
            }
            y += 6;
            if (!live)
                y += fullui::notBuilt(ui_, c.x + 20, y, c.width - 40, "this GT4 program has no sound packs (it plays GT4's own engine sound).") + 8;
            ui_.fill(R(c.x + 20, y, c.width - 40, 1), t.border);
            y += 12;
            const float bw = 150;
            if (f.soundMode == FullSoundMode::EveryCar || f.soundMode == FullSoundMode::PerCar)
            {
                const bool every = f.soundMode == FullSoundMode::EveryCar;
                ui_.text(every ? "The pack for every car" : "Every other car", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
                y += 20;
                const std::string name = f.soundPackAll.empty() ? std::string("GT4's own engine") : packName(f.soundPackAll);
                ui_.text(ui_.fit(name, c.width - 40 - 2 * bw - 16, 14.0f), c.x + 20, y + 7, 14.0f, f.soundPackAll.empty() ? t.muted : t.text, Weight::Semibold);
                if (ui_.button("pack-all-use", R(c.x + c.width - 20 - 2 * bw - 8, y, bw, 32), "Use the chosen pack", Style::Secondary,
                               live && sel && sel->ok && sel->folder != f.soundPackAll, 12.5f))
                {
                    f.soundPackAll = sel->folder;
                    markDirty();
                }
                if (ui_.button("pack-all-none", R(c.x + c.width - 20 - bw, y, bw, 32), "GT4's own", Style::Ghost, live && !f.soundPackAll.empty(), 12.5f))
                {
                    f.soundPackAll.clear();
                    markDirty();
                }
                y += 44;
            }
            if (f.soundMode == FullSoundMode::PerCar)
            {
                ui_.text("Cars with their own pack", c.x + 20, y, 12.5f, t.faint, Weight::Semibold);
                y += 22;
                // The cars in the player's garage (GT4's car codes), then any code chosen earlier that is not there.
                loadGarage();
                std::vector<std::pair<uint32_t, std::string>> cars;
                for (const GarageCar &g : garage_.cars)
                    if (g.code && std::find_if(cars.begin(), cars.end(), [&](const auto &x) { return x.first == g.code; }) == cars.end())
                        cars.push_back({g.code, g.name.empty() ? g.label : g.name});
                if (demoLook && cars.empty())
                    cars = {{82, "Supra RZ '97"}, {59, "Ka '01"}, {757, "Escort Rally Car '98"}};
                for (const FullSoundCar &sc : f.soundCars)
                    if (std::find_if(cars.begin(), cars.end(), [&](const auto &x) { return x.first == sc.code; }) == cars.end())
                        cars.push_back({sc.code, names_ ? names_->carName(sc.code) : std::string()});
                const float listBottom = c.y + c.height - 120;
                const Rectangle view = R(c.x + 10, y, c.width - 20, std::max(60.0f, listBottom - y));
                const float off = ui_.beginScroll("sound-cars", view);
                float ry = view.y - off;
                for (const auto &[code, carName] : cars)
                {
                    const std::string mine = fullSoundPackFor(f, code);
                    if (ry + 40 > view.y - 40 && ry < view.y + view.height)
                    {
                        ui_.text(ui_.fit(carName.empty() ? strf("Car code %u", code) : carName, c.width - 2 * 120 - 80, 13.5f), view.x + 10, ry + 2, 13.5f,
                                 t.text);
                        ui_.text(ui_.fit(mine.empty() ? std::string("as every other car") : packName(mine), c.width - 2 * 120 - 80, 12.0f), view.x + 10,
                                 ry + 20, 12.0f, mine.empty() ? t.faint : t.accent);
                        if (ui_.button(strf("car-use-%u", code), R(view.x + view.width - 2 * 112 - 6, ry + 2, 112, 30), "Chosen pack", Style::Secondary,
                                       live && sel && sel->ok && sel->folder != mine, 12.0f))
                        {
                            bool found = false;
                            for (FullSoundCar &x : f.soundCars)
                                if (x.code == code)
                                {
                                    x.folder = sel->folder;
                                    found = true;
                                }
                            if (!found)
                                f.soundCars.push_back({code, sel->folder});
                            markDirty();
                        }
                        if (ui_.button(strf("car-clear-%u", code), R(view.x + view.width - 112, ry + 2, 112, 30), "Clear", Style::Ghost, live && !mine.empty(),
                                       12.0f))
                        {
                            f.soundCars.erase(std::remove_if(f.soundCars.begin(), f.soundCars.end(), [&](const FullSoundCar &x) { return x.code == code; }),
                                              f.soundCars.end());
                            markDirty();
                        }
                    }
                    ry += 44;
                }
                if (cars.empty())
                    ui_.wrap(garage_.loaded ? "Your garage has no cars yet." : "Your garage is read from memory card slot 1's save: none found.", view.x + 10,
                             view.y + 4, view.width - 20, 12.5f, t.muted);
                ui_.endScroll("sound-cars", ry + off - view.y);
                y = listBottom + 8;
            }
            // what GT4 gets
            const float wy = std::max(y, c.y + c.height - 110);
            ui_.fill(R(c.x + 20, wy, c.width - 40, 1), t.border);
            float gy = wy + 12;
            ui_.text("What GT4 gets", c.x + 20, gy, 12.5f, t.faint, Weight::Semibold);
            gy += 20;
            if (live)
            {
                ui_.text(ui_.fit("GT4_SOUND_PACK=" + fullSoundPackValue(f), c.width - 40, 12.5f, Weight::Mono, true), c.x + 20, gy, 12.5f, t.muted, Weight::Mono);
                gy += 18;
                ui_.text(ui_.fit("GT4_SOUND_PACK_MAP=" + fullSoundMapValue(f), c.width - 40, 12.5f, Weight::Mono, true), c.x + 20, gy, 12.5f, t.muted, Weight::Mono);
                gy += 22;
            }
            else
            {
                ui_.text("nothing (this GT4 program does not read GT4_SOUND_PACK)", c.x + 20, gy, 13, t.muted);
                gy += 22;
            }
            ui_.wrap("For the car you drive (the other cars keep GT4's own engines). Takes effect the next time GT4 starts.",
                     c.x + 20, gy, c.width - 40, 12.0f, t.faint);
        }
    }
}
