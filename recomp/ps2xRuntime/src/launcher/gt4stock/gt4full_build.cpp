// GT4 Recompilation launcher, Full edition: Build GT4 from the player's own disc. The release has no game code in it;
// its build-kit folder holds gt4setup.exe, the recomp source, the release config (addresses only), the tools (ps2_recomp,
// llvm-mingw, CMake, Ninja) and the libraries' sources. "gt4setup build" makes the game code from the disc chosen on Play,
// compiles GT4.exe and copies it and game\ (the disc's boot files and CORE.elf; the cards stay) next to the launcher.
// It runs hidden, below normal priority; its output goes to build-kit\build.log, read here for its "@step" lines.

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4stock_system.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }

        // The last part of a text file (the build log can grow to megabytes: CMake's and the compiler's lines).
        std::string tail(const fs::path &p, size_t bytes)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return {};
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            const std::streamoff from = std::max<std::streamoff>(0, size - static_cast<std::streamoff>(bytes));
            in.seekg(from);
            std::string s(static_cast<size_t>(size - from), '\0');
            in.read(s.data(), static_cast<std::streamsize>(s.size()));
            return s;
        }

        std::string minutesText(uint64_t ms)
        {
            const int s = static_cast<int>(ms / 1000u);
            return s < 60 ? strf("%d s", s) : strf("%d min %02d s", s / 60, s % 60);
        }
    }

    fs::path App::buildKit() const
    {
        std::error_code ec;
        const fs::path kit = inst_.root / "build-kit";
        return fs::is_regular_file(kit / "gt4setup.exe", ec) || (hidden_ && buildShot_) ? kit : fs::path{};
    }

    void App::startBuild()
    {
        const ui::Theme &t = ui::theme();
        const fs::path kit = buildKit();
        if ((hidden_ && !opt_.buildTest) || build_.running || kit.empty())
            return;
        if (busy())
        {
            setStatus("Close GT4 first: it is built with GT4 closed.", t.warn);
            return;
        }
        if (!disc_.exists)
        {
            setStatus("Choose the Gran Turismo 4 disc image first.", t.warn);
            return;
        }
        sys::Launch l;
        l.exe = kit / "gt4setup.exe";
        l.cwd = kit;
        const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        l.args = {L"build", L"--iso", pathFrom(st_.disc).wstring(), L"--source", (kit / "recomp").wstring(), L"--config", (kit / "config").wstring(),
                  L"--tools", (kit / "tools").wstring(), L"--work", (kit / "work").wstring(), L"--install", inst_.root.wstring(),
                  L"--jobs", std::to_wstring(cores)};
        std::error_code ec;
        if (fs::is_directory(kit / "deps", ec))
            l.args.insert(l.args.end(), {L"--deps", (kit / "deps").wstring()});
        l.log = kit / "build.log";
        build_ = BuildJob{};
        build_.log = l.log;
        std::string error;
        if (!sys::start(l, build_.proc, error))
        {
            build_.failed = true;
            build_.error = "Could not start the build: " + error;
            setStatus(build_.error, t.bad);
            return;
        }
        build_.running = true;
        build_.startMs = build_.stepMs = sys::tickMs();
        setStatus("Building GT4 from your disc...", t.muted);
    }

    void App::pollBuild()
    {
        if (!build_.running)
            return;
        const uint64_t now = sys::tickMs();
        int code = 0;
        const bool ended = sys::poll(build_.proc, code);
        if (!ended && now - build_.readMs < 500u)
            return;
        build_.readMs = now;
        // The newest "@step" line in the log's last 64 KB (each step prints one before its own output).
        const std::string log = tail(build_.log, 65536);
        for (size_t at = log.rfind("\n@step "); at != std::string::npos;)
        {
            char *num = nullptr;
            const int n = static_cast<int>(std::strtol(log.c_str() + at + 7, &num, 10));
            const int of = static_cast<int>(std::strtol(num, &num, 10));
            if (n > 0 && of > 0)
            {
                const size_t colon = log.find(": ", at), end = log.find('\n', at + 1);
                if (n != build_.step)
                    build_.stepMs = now;
                build_.step = n, build_.of = std::max(of, 1);
                if (colon != std::string::npos && colon < end)
                    build_.what = trim(log.substr(colon + 2, end == std::string::npos ? std::string::npos : end - colon - 2));
            }
            break;
        }
        if (!ended)
            return;
        const ui::Theme &t = ui::theme();
        build_.running = false;
        build_.endMs = now;
        if (code == 0 && log.find("\n@done ") != std::string::npos)
        {
            build_.done = true;
            // The new GT4.exe and game\ next to the launcher: found again, and its features read.
            locate();
            inst_.exe = inst_.root / "GT4.exe";
            std::error_code ec;
            inst_.exeFound = fs::is_regular_file(inst_.exe, ec);
            inst_.package = inst_.exeFound;
            fullSetup();
            setStatus("GT4 is built. Press Play.", t.ok);
            return;
        }
        build_.failed = true;
        // The failure's own words: gt4setup's "error:" line, or the log's last lines.
        const size_t e = log.rfind("error: ");
        if (e != std::string::npos)
        {
            const size_t eol = log.find('\n', e);
            std::string why = trim(log.substr(e + 7, eol == std::string::npos ? std::string::npos : eol - e - 7));
            if (!why.empty())
                why[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(why[0])));
            build_.error = why + ".";
        }
        else
            build_.error = trim(log.substr(log.size() > 400 ? log.size() - 400 : 0));
        setStatus(strf("The build stopped (exit %d): see build-kit\\build.log.", code), t.bad);
    }

    // --build-test: what the Play page's button does, then the panel's steps as report lines, until the build ends.
    int App::runBuildTest()
    {
        report(strf("%s launcher %s: --build-test, root %s", kProductName, kVersion, pathText(inst_.root).c_str()));
        report("build-kit: " + (buildKit().empty() ? std::string("not found") : pathText(buildKit())));
        report("disc: " + (disc_.exists ? pathText(pathFrom(st_.disc)) : std::string("not found")));
        report(std::string("before: GT4 program ") + (inst_.exeFound ? "found " + pathText(inst_.exe) : "missing"));
        startBuild();
        int shown = -1;
        while (build_.running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            pollBuild();
            if (build_.step != shown)
            {
                shown = build_.step;
                report(strf("step %d of %d: %s (%s in)", build_.step, build_.of, build_.what.c_str(), minutesText(sys::tickMs() - build_.startMs).c_str()));
            }
        }
        report(build_.done ? "result: built in " + minutesText(build_.endMs - build_.startMs) : "result: stopped: " + build_.error);
        report(std::string("after: GT4 program ") + (inst_.exeFound ? "found " + pathText(inst_.exe) : "missing") + "; game files " +
               (inst_.coreFound ? "found" : "missing") + "; Play page " + (!buildKit().empty() && (!inst_.exeFound || !inst_.coreFound || build_.failed) ? "Build GT4" : "PLAY"));
        return build_.done ? 0 : 1;
    }

    // The Play page's build panel under the hero: the steps as a bar, this step's words and the time so far; or the result.
    void App::drawBuild(float x, float y, float w)
    {
        const ui::Theme &t = ui::theme();
        const Rectangle card = R(x, y, w, 132);
        ui_.round(card, 12, t.panel);
        ui_.roundLine(card, 12, 1, t.border);
        const float ix = x + 24, iw = w - 48;
        if (build_.running)
        {
            const uint64_t now = sys::tickMs();
            ui_.text("Building GT4 from your disc", ix, y + 18, 17, t.text, Weight::Semibold);
            ui_.text(strf("%s so far", minutesText(now - build_.startMs).c_str()), ix + iw, y + 20, 13, t.muted, Weight::Regular, Align::Right);
            // One segment per step: done ones full, this one partly filled with its time (most steps take under a minute;
            // compiling a few minutes), the rest empty.
            const int of = std::max(build_.of, 1);
            const float gap = 6.0f, segW = (iw - gap * (of - 1)) / of, by = y + 54;
            for (int k = 1; k <= of; ++k)
            {
                const Rectangle s = R(ix + (k - 1) * (segW + gap), by, segW, 8);
                ui_.round(s, 4, t.panel3);
                float f = k < build_.step ? 1.0f : 0.0f;
                if (k == build_.step)
                {
                    const float expect = k == of ? 300000.0f : 60000.0f;
                    f = std::min(0.9f, static_cast<float>(now - build_.stepMs) / expect);
                }
                if (f > 0.0f)
                    ui_.round(R(s.x, s.y, s.width * f, s.height), 4, t.accent);
            }
            const std::string step = build_.step > 0 ? strf("Step %d of %d: %s", build_.step, of, build_.what.c_str()) : std::string("Starting...");
            ui_.text(step, ix, by + 22, 14, t.text);
            ui_.text("About 5 to 10 minutes the first time. It runs in the background; you can keep using the launcher.", ix, by + 48, 12.5f, t.muted);
            return;
        }
        // (idle or stopped: the button; it needs the disc chosen and GT4 closed)
        const Rectangle br = R(ix + iw - 160, y + 12, 160, 36);
        if (!build_.done && ui_.button("build-gt4", br, build_.failed ? "Try again" : "Build GT4", Style::Primary, disc_.exists && !busy(), 14))
            startBuild();
        if (build_.failed)
        {
            ui_.text("The build stopped", ix, y + 18, 17, t.bad, Weight::Semibold);
            const float ey = y + 48 + ui_.wrap(build_.error.empty() ? std::string("Something went wrong.") : build_.error, ix, y + 48, iw, 13, t.text);
            ui_.text("Everything it printed is in build-kit\\build.log next to the launcher.", ix, ey + 8, 12.5f, t.muted);
            return;
        }
        if (build_.done)
        {
            ui_.text("GT4 is built", ix, y + 18, 17, t.ok, Weight::Semibold);
            ui_.wrap(strf("Made from your disc in %s. GT4.exe and the game files are next to the launcher; your memory cards were left as "
                          "they were.",
                          minutesText(build_.endMs - build_.startMs).c_str()),
                     ix, y + 48, iw, 13, t.muted);
            return;
        }
        ui_.text("Build GT4 from your disc", ix, y + 18, 17, t.text, Weight::Semibold);
        ui_.wrap(disc_.exists ? "This download has no game code in it. Build GT4 turns the game code on your own disc into a Windows program on "
                                "this PC. Nothing is sent anywhere, and the first build takes about 5 to 10 minutes."
                              : "This download has no game code in it: GT4 is built on this PC from your own disc. Choose the disc image below first.",
                 ix, y + 48, iw, 13, t.muted);
    }
}
