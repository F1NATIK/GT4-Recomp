#pragma once

// GT4 Recompilation launcher (Stock edition): the Windows side (kept out of the raylib translation
// units, whose names clash with windows.h): resources, folders, the file
// dialog, starting and stopping GT4.exe, the console for --dry-run.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gt4stock::sys
{
    namespace fs = std::filesystem;

    // The hidden test mode: no Explorer window and no file dialog, ever.
    void setNoShell(bool on);
    fs::path exePath();
    std::vector<std::wstring> arguments(); // the command line, UTF-16 (any path)
    std::vector<uint8_t> resource(int id); // RCDATA from the exe, empty when missing

    // Explorer on a folder / the default program on a file.
    bool openInExplorer(const fs::path &path);
    // The Windows file dialog (visible sessions only); empty when cancelled.
    // mustExist = false: a new file name may be typed (a card GT4 will make).
    fs::path browseForFile(void *owner, const std::wstring &title, const std::wstring &filter, const fs::path &start, bool mustExist = true);
    // The Windows "Save as" dialog (visible sessions only); empty when cancelled.
    fs::path browseForSaveFile(void *owner, const std::wstring &title, const std::wstring &filter, const fs::path &suggested,
                               const std::wstring &extension);
    // The Windows folder picker; empty when cancelled.
    fs::path browseForFolder(void *owner, const std::wstring &title, const fs::path &start);

    // 1.4: printing the cheat sheet. Windows' print window (PrintDlgEx), then one page per picture (RGB, top row
    // first), each scaled to the printable area with its shape kept. Only on the player's click: the hidden test
    // mode (setNoShell) never opens it. message: what happened, in a player's words.
    struct PrintPage
    {
        int width = 0, height = 0;
        const uint8_t *rgb = nullptr;
    };
    bool printPages(void *owner, const std::wstring &documentName, const std::vector<PrintPage> &pages, std::string &message);

    // GT4.exe (or the stock exe) as a child process.
    struct Launch
    {
        fs::path exe;
        std::vector<std::wstring> args;
        fs::path cwd;
        std::vector<std::pair<std::string, std::string>> env; // set (UTF-8)
        std::vector<std::string> stripPrefixes{"GT3_", "PS2X_", "GT4_", "TT_"}; // inherited variables removed first
        fs::path log; // stdout + stderr to this file (empty: none)
    };
    std::wstring commandLine(const Launch &l);
    // What the child's environment block would hold (sorted, for --dry-run).
    std::vector<std::string> environmentFor(const Launch &l);

    struct Process
    {
        void *handle = nullptr;
        unsigned long pid = 0;
        uint64_t startMs = 0;
        bool closeSent = false;
        uint64_t closeSentMs = 0;
    };
    bool start(const Launch &l, Process &p, std::string &error);
    // True once it has ended (exitCode set); the handle is closed then.
    bool poll(Process &p, int &exitCode);
    // The window close (GT4's power button: a clean quit), then, after a wait, the hard stop.
    void requestClose(Process &p);
    void terminate(Process &p);
    uint64_t tickMs();

    // Another GT4 (started outside the launcher) running: its exe names.
    bool gameRunning(const std::vector<std::wstring> &exeNames);

    // --dry-run / tests: the parent's console if there is one (a GUI program has none of its own).
    void attachParentConsole();
    // Whether this process has a visible top-level window (the hidden mode's self-check).
    bool anyVisibleWindow();
    std::string computerName();

    // The graphics adapter GT4.exe runs on, as far as it can be told without starting it. Read
    // only: the exe's export table (GT4.exe exports NvOptimusEnablement and
    // AmdPowerXpressRequestHighPerformance, ps2xRuntime/src/windows_gpu_preference.cpp), Windows'
    // graphics setting for that exe (Settings > Display > Graphics) and DXGI's adapter list
    // (dxgi.dll loaded at run time; no device is created).
    struct GpuChoice
    {
        std::vector<std::string> adapters;         // the hardware adapters, DXGI's order
        std::string highPerformance, minimumPower; // DXGI 1.6's pick for each preference
        unsigned highPerformanceVendor = 0;        // its PCI vendor id
        bool exeRead = false, nvExport = false, amdExport = false;
        int windowsSetting = -1; // for the exe: 0 let Windows decide, 1 power saving, 2 high performance (-1: none)
        std::string game;        // the adapter GT4.exe runs on (empty: it cannot be told, Windows chooses)
        std::string why;         // how that was decided (the test report)
        std::string label() const; // About's Graphics row: "<adapter> (game)" or "chosen by Windows"
    };
    GpuChoice gameGpu(const fs::path &exe);
}
