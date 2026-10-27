// GT4 Recompilation (Classic edition): the launcher. The settings of GT4's stock build
// (display, controllers and player 2, wheel and pedals, EyeToy camera, saves, GT4's button codes) and its Play button.
// analysis/gt4-stock-launcher/README.md.
//
//   GT4Recompilation.exe [--root <GT4 folder>] [--settings <file>] [--exe <GT4 program>] [--page <name>]
//     pages: play, display, controllers, eyetoy, saves, cheats, about, how (About -> How it works)
//   Tests (hidden window, nothing on the desktop):
//     --test-shots <folder> [--sizes 2560x1440,1280x960] [--demo] [--save-copy <.ps2 or folder>]... [--report <file>]
//                  [--cheat-files <folder>]   (1.4) the Cheats page's Save card / Save PDF go here, never to the GT4 folder
//     --scanline-shot <prefix> [--report <file>]   preview renders for compare_shot.py
//     --dry-run [--report <file>]                    what Play would start (no window)
//     --save-settings                                write the settings file once

#include "gt4stock_app.h"
#include "gt4stock_system.h"
#include "gt4stock_util.h"

#include <iostream>

int main()
{
    using namespace gt4stock;
    Options o;
    const std::vector<std::wstring> args = sys::arguments();
    bool wantConsole = false;
    for (size_t i = 1; i < args.size(); ++i)
    {
        const std::string a = utf8(args[i]);
        auto next = [&]() -> std::wstring { return i + 1 < args.size() ? args[++i] : std::wstring(); };
        if (a == "--root")
            o.root = next();
        else if (a == "--settings")
            o.settings = next();
        else if (a == "--exe")
            o.exe = next();
        else if (a == "--page")
            o.page = utf8(next());
        else if (a == "--test-shots")
        {
            o.shots = next();
            wantConsole = true;
        }
        else if (a == "--sizes")
        {
            o.sizes.clear();
            for (const std::string &wh : split(utf8(next()), ','))
            {
                const std::vector<std::string> p = split(lower(wh), 'x');
                long long w = 0, h = 0;
                if (p.size() == 2 && parseInt(p[0], w) && parseInt(p[1], h) && w >= 640 && h >= 400 && w <= 8192 && h <= 8192)
                    o.sizes.push_back({static_cast<int>(w), static_cast<int>(h)});
            }
        }
        else if (a == "--demo")
            o.demo = true;
        else if (a == "--save-copy")
            o.saveCopies.push_back(next());
        else if (a == "--cheat-files")
            o.cheatFiles = next();
        else if (a == "--dry-run")
        {
            o.dryRun = true;
            wantConsole = true;
        }
        else if (a == "--save-settings")
            o.saveSettings = true;
        else if (a == "--report")
            o.report = next();
        else if (a == "--scanline-shot")
        {
            o.scanlineShot = utf8(next());
            wantConsole = true;
        }
#if defined(GT4L_EDITION_FULL)
        // The Full edition (GT4RecompilationFull.exe): pages multiscreen, garage, network, progress too;
        //   --release-look   test shots and dry runs as a release build shows them ("Not built yet" hidden)
        //   --only <prefix>  test shots: only the scenes whose name starts with it
        else if (a == "--release-look")
            o.releaseLook = true;
        else if (a == "--only")
            o.only = utf8(next());
        //   --build-test  tests only: Build GT4 as Play starts it, from --root's build-kit and disc, no window
        else if (a == "--build-test")
        {
            o.buildTest = true;
            wantConsole = true;
        }
        //   --edit-test <action>  tests only: one Garage change on the test copy at --root (gt4full_garageedit.cpp), no window
        else if (a == "--edit-test")
        {
            o.editTest = utf8(next());
            wantConsole = true;
        }
#endif
        else
        {
            sys::attachParentConsole();
            std::cerr << "GT4Recompilation: unknown option " << a << "\n";
            return 2;
        }
    }
    if (wantConsole)
        sys::attachParentConsole();
    App app;
    return app.run(o);
}
