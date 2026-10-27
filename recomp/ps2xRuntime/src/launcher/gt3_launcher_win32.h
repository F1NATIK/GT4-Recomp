#pragma once

// Windows services for the GT3 launcher (kept apart from raylib, whose names
// clash with windows.h): file dialogs, starting the game, opening files.

#include <string>
#include <utility>
#include <vector>

namespace gt3launcher
{
    // Directory of the running executable (UTF-8, no trailing separator).
    std::string executableDirectory();
    bool fileExists(const std::string &path);
    bool directoryExists(const std::string &path);
    // Names (not paths) of the files in directory matching pattern ("*.iso").
    std::vector<std::string> filesIn(const std::string &directory, const std::string &pattern);

    // Open / save file dialogs; filter is a Win32 filter list with '|' as the
    // separator ("Disc images|*.iso|All files|*.*"). Returns false on cancel.
    bool browseOpenFile(void *owner, const std::string &title, const std::string &filter, std::string &path);
    bool browseSaveFile(void *owner, const std::string &title, const std::string &filter, std::string &path);

    // Opens a file or folder with its default program (Explorer, Notepad, ...).
    void shellOpen(const std::string &path);

    // The game process: started with the launcher's environment minus every
    // GT3_* / PS2X_* variable, plus `env` (empty values are left out), its
    // output written to logPath.
    struct GameProcess
    {
        void *handle = nullptr;
        bool running = false;
        unsigned long exitCode = 0;
        unsigned long long closeRequested = 0; // GetTickCount64 of stopGame's close request
    };
    bool startGame(const std::string &exe, const std::vector<std::string> &args, const std::string &workingDirectory,
                   const std::vector<std::pair<std::string, std::string>> &env, const std::string &logPath, GameProcess &process,
                   std::string &error);
    // Updates running/exitCode; returns true once when the game has exited.
    bool pollGame(GameProcess &process);
    void stopGame(GameProcess &process);

    // Moves log -> log-previous-1 ... keeps `keep` older logs (as Play-GT3-latest.cmd).
    void rotateLogs(const std::string &directory, const std::string &latestName, const std::string &previousPrefix, int keep);

    // "Adapter name: a.b.c.d" for each IPv4 address of the adapters that are up
    // (no loopback or link-local): what a host tells the players who join.
    std::vector<std::string> localAddresses();
    std::string computerName();
    // This process's ID (gt3RaceLab.exe is told it, to close when the launcher does).
    unsigned long currentProcessId();
}
