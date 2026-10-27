#include "ps2_runtime.h"
#include "games_database.h"
#include "runtime/crash_report.h"
#include "ThreadNaming.h"
#if defined(PS2X_GAME_GT3)
#include "ps2x/iop/gt3/pdi1394_link.h"
#endif
#if defined(PS2X_GAME_GT4)
#include "runtime/gt4_start_keys.h" // the Classic edition's settings (gt4-start.env, its environment)
#endif
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <type_traits>
#include <vector>
#include <cwchar>
#include <cctype>
// --boot-seconds / --cpu-only diagnostics: Windows and desktop Linux (the
// Linux bench runs them headless or under Xvfb).
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
#define PS2X_BOOT_DIAGNOSTICS 1
#else
#define PS2X_BOOT_DIAGNOSTICS 0
#endif
#if PS2X_BOOT_DIAGNOSTICS
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

#if defined(_WIN32)
    extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *module, wchar_t *name, unsigned long size);
    extern "C" __declspec(dllimport) int __stdcall FreeConsole(void);
    extern "C" __declspec(dllimport) int __stdcall SetCurrentDirectoryW(const wchar_t *path);
    // --cpu-only: the 1 ms timer a window run gets from raylib (InitWindow).
    extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);
    extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int period);
#if defined(_MSC_VER)
#pragma comment(lib, "winmm.lib")
#endif

    // Started with no arguments (double-clicked, a folder of its own): the
    // game from the files next to the executable -
    //   game\CORE.elf (else windows-diagnostic\game\CORE.elf), GT3's program;
    //   the first .iso / .bin in disc\ (GT3_DISC_IMAGE);
    //   memcards\Mcd001.ps2 (GT3_MEMCARD1; a new card is made when missing; GT3 only);
    //   gt3-start.env: KEY=VALUE lines ('#' comments) set before the game
    //   starts (e.g. GT3_RENDERER=gl), unless already set.
    // The console window goes away and the log goes to logs\gt3.log.
    // Returns the CORE.elf path, or empty (not such a folder).
#if defined(PS2X_GAME_GT4)
#define PS2X_PORTABLE_ENV "gt4-start.env"
#define PS2X_PORTABLE_DISC_VAR "GT4_DISC_IMAGE"
#define PS2X_PORTABLE_LOG "gt4.log"
#else
#define PS2X_PORTABLE_ENV "gt3-start.env"
#define PS2X_PORTABLE_DISC_VAR "GT3_DISC_IMAGE"
#define PS2X_PORTABLE_LOG "gt3.log"
#endif
    std::filesystem::path portableStart()
    {
        wchar_t buffer[4096];
        const unsigned long n = GetModuleFileNameW(nullptr, buffer, 4096);
        if (n == 0 || n >= 4096)
            return {};
        const std::filesystem::path dir = std::filesystem::path(std::wstring(buffer, n)).parent_path();
        std::filesystem::path elf;
        for (const auto &candidate : {dir / "game" / "CORE.elf", dir / "windows-diagnostic" / "game" / "CORE.elf"})
            if (std::error_code ec; std::filesystem::exists(candidate, ec))
            {
                elf = candidate;
                break;
            }
        if (elf.empty())
            return {};
        SetCurrentDirectoryW(dir.wstring().c_str());
        const auto setDefault = [](const std::string &key, const std::string &value) {
            const char *existing = std::getenv(key.c_str());
            if (!existing || !*existing)
                _putenv_s(key.c_str(), value.c_str());
        };
#if defined(GT4_STOCK_BUILD)
        // The Classic package's start file holds Classic settings only (runtime/gt4_start_keys.h: the
        // picture, the window, the disc, controls, GT4's devices and its button codes), and a device key
        // may not name a test device (refusedValue). Anything else (renderer, resolution, test and
        // diagnostic switches) is ignored and logged.
        std::vector<std::string> ignored;
#endif
        if (std::ifstream env(dir / PS2X_PORTABLE_ENV); env)
        {
            std::string line;
            while (std::getline(env, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0)
                    continue;
#if defined(PS2X_GAME_GT4)
                // GT4L_*: the GT4 Stock Launcher's own state in the same file
                // (analysis/gt4-stock-launcher/README.md); not GT4.exe's, never set.
                if (line.compare(0, 5, "GT4L_") == 0)
                    continue;
#endif
#if defined(GT4_STOCK_BUILD)
                if (!gt4startkeys::classicKey(line.substr(0, eq)))
                {
                    ignored.push_back(line.substr(0, eq) + " ignored (not a Classic setting)");
                    continue;
                }
                if (const std::string why = gt4startkeys::refusedValue(line.substr(0, eq), line.substr(eq + 1)); !why.empty())
                {
                    ignored.push_back(line + " ignored (" + why + ")");
                    continue;
                }
#endif
                setDefault(line.substr(0, eq), line.substr(eq + 1));
            }
        }
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(dir / "disc", ec))
        {
            std::string ext = entry.path().extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (entry.is_regular_file(ec) && (ext == ".iso" || ext == ".bin"))
            {
                setDefault(PS2X_PORTABLE_DISC_VAR, entry.path().string());
                break;
            }
        }
#if !defined(PS2X_GAME_GT4)
        // GT3's card file (GT4 never reads GT3_MEMCARD1: its cards are game\mc0 and GT4_MC0 / GT4_MC1).
        std::filesystem::create_directories(dir / "memcards", ec);
        setDefault("GT3_MEMCARD1", (dir / "memcards" / "Mcd001.ps2").string());
#endif
        std::filesystem::create_directories(dir / "logs", ec);
        std::string logName = PS2X_PORTABLE_LOG;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // GT4's own Multi Monitor Mode: a side screen's GT4 copy (GT4_MM_NAME LEFT, RIGHT, TOP, LIVE) keeps its own log,
        // gt4-<name>.log, next to the middle copy's gt4.log.
        if (const char *mm = std::getenv("GT4_MM_NAME"); mm && *mm && std::string(mm) != "CENTER")
        {
            std::string n = mm;
            for (char &c : n)
                c = std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '-';
            logName = "gt4-" + n.substr(0, 16) + ".log";
        }
#endif
        const std::string log = (dir / "logs" / logName).string();
        {
            std::ofstream truncate(log, std::ios::trunc); // a fresh log each start
        }
        // The console first (released after the redirect, later writes were
        // lost), then both streams appending, so neither overwrites the
        // other's lines.
        FreeConsole();
        FILE *out = std::freopen(log.c_str(), "a", stdout);
        FILE *err = out ? std::freopen(log.c_str(), "a", stderr) : nullptr;
        if (!out || !err)
        {
            std::ofstream(log, std::ios::app) << "[start] could not redirect the log (" << (out ? "stderr" : "stdout") << ")\n";
        }
        else
        {
            // Redirected to a file the CRT buffers both; the log should
            // survive a crash or a forced stop (set before any output).
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            std::cerr << "[start] started without arguments: the log is this file" << std::endl;
        }
#if defined(GT4_STOCK_BUILD)
        for (const std::string &note : ignored)
            std::cerr << "[start] " PS2X_PORTABLE_ENV ": " << note << std::endl;
#endif
        return elf;
    }
#endif

#if defined(_WIN32) && defined(GT4_STOCK_BUILD)
    extern "C" __declspec(dllimport) wchar_t *__stdcall GetEnvironmentStringsW(void);
    extern "C" __declspec(dllimport) int __stdcall FreeEnvironmentStringsW(wchar_t *block);

    // The Classic edition takes only its own settings from its environment too (runtime/gt4_start_keys.h),
    // so a test or diagnostic switch set by hand (a pad or wheel replay, the test pilot, picture and data
    // dumps, the GL-against-CPU compare, the GS worker turned off) does nothing in a player's game. Outside
    // a timed test run (--boot-seconds, the hidden checks), every GT3_* / GT4_* / PS2X_* / TT_* variable
    // that is not a Classic setting, or holds a value Classic refuses, is removed before anything reads
    // it. Returns the log lines, written once the log is open.
    std::vector<std::string> classicEnvironment()
    {
        std::vector<std::pair<std::string, std::string>> drop; // the name as set, why
        if (wchar_t *block = GetEnvironmentStringsW())
        {
            for (const wchar_t *e = block; *e; e += std::wcslen(e) + 1)
            {
                const std::wstring entry(e);
                const size_t eq = entry.find(L'=', 1); // the "=C:=C:\..." entries start with '='
                if (eq == std::wstring::npos)
                    continue;
                std::string key, value;
                bool ascii = true;
                for (const wchar_t c : entry.substr(0, eq))
                {
                    ascii = ascii && c < 0x80;
                    key.push_back(static_cast<char>(c));
                }
                if (!ascii || !gt4startkeys::ownedKey(key))
                    continue;
                for (const wchar_t c : entry.substr(eq + 1))
                    value.push_back(c < 0x80 ? static_cast<char>(c) : '?');
                const std::string name = gt4startkeys::upperAscii(key);
                const std::string why = gt4startkeys::classicKey(name) ? gt4startkeys::refusedValue(name, value) : std::string("not a Classic setting");
                if (!why.empty())
                    drop.emplace_back(key, why);
            }
            FreeEnvironmentStringsW(block);
        }
        std::vector<std::string> notes;
        for (const auto &[key, why] : drop)
        {
            _putenv_s(key.c_str(), ""); // removes it (the C runtime's copy and the process's)
            notes.push_back("[start] environment: " + key + " ignored (" + why + ")");
        }
        return notes;
    }
#endif

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(_WIN32)
        if (std::filesystem::path elf = portableStart(); !elf.empty())
        {
            std::cout << "Started without arguments: " << elf.string() << std::endl;
            return elf;
        }
#endif
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
    bool testRun = false; // a timed run (--boot-seconds): the hidden checks
    for (int i=1;i<argc;++i)
        if (argv[i] && std::string(argv[i]) == "--boot-seconds")
            testRun = true;
    (void)testRun;
#if defined(_WIN32) && defined(GT4_STOCK_BUILD)
    // First, before anything reads the environment (classicEnvironment).
    const std::vector<std::string> classicNotes = testRun ? std::vector<std::string>{} : classicEnvironment();
#endif
#if PS2X_BOOT_DIAGNOSTICS
    // Every timed run is a test run from its first statement: no error
    // dialogs, a crash ends it (setTestRunErrorMode).
    if (testRun)
        ps2x::setTestRunErrorMode();
#endif
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();
    ps2x::installCrashReporter();
    ps2x::startSamplingProfiler();
    ThreadNaming::PreferPerformanceProcess();

    try
    {
#if PS2X_BOOT_DIAGNOSTICS
        const char* usage="Usage: ps2EntryRunner guest.elf [--boot-seconds 1..3600 [--cpu-only|--headless]]";
        // A flag in the ELF's place ("--headless" alone, a mistyped "-headless")
        // is refused before any window exists; so is an ELF that cannot be read.
        if (argc >= 2 && argv[1] && argv[1][0] == '-')
            throw std::runtime_error(usage);
#endif
        std::filesystem::path pathObj = getExecutablePath(argc, argv);
#if defined(GT4_STOCK_BUILD)
        // A stable marker of the Classic build (the GT4 Recompilation launcher's exeFeatures and
        // analysis/classic-package/make-package.ps1 look for it), printed so the linker keeps the string.
        std::cerr << "[start] GT4_CLASSIC_BUILD=1 (GT4 Recompilation, Classic edition)" << std::endl;
#endif
#if defined(_WIN32) && defined(GT4_STOCK_BUILD)
        for (const std::string &note : classicNotes) // the log is open now (portableStart)
            std::cerr << note << std::endl;
#endif
#if PS2X_BOOT_DIAGNOSTICS
        if (std::error_code ec; !std::filesystem::is_regular_file(pathObj,ec) || !std::ifstream(pathObj,std::ios::binary))
            throw std::runtime_error("Cannot read the guest ELF: "+pathObj.string());
        unsigned bootSeconds=0;
        bool cpuOnly=false, headless=false;
        if (argc > 2) {
            const std::string mode=argc == 5 ? argv[4] : "";
            if ((argc != 4 && argc != 5) || std::string(argv[2]) != "--boot-seconds" ||
                (argc == 5 && mode != "--cpu-only" && mode != "--headless"))
                throw std::runtime_error(usage);
            const std::string duration(argv[3]); size_t parsed=0;
            const int seconds=std::stoi(duration,&parsed);
            if (parsed != duration.size() || seconds < 1 || seconds > 3600)
                throw std::runtime_error("--boot-seconds must be an integer from 1 to 3600");
            bootSeconds=static_cast<unsigned>(seconds);
            cpuOnly=mode == "--cpu-only";
            // --headless: the normal window path with a hidden window, no audio
            // device and no host input (PS2Runtime::setHeadless); it needs the
            // time limit, so with the run deadline below it always ends on its own.
            headless=mode == "--headless";
        }
        // A timed run always ends on its own, whatever becomes of whoever
        // started it: <seconds> + 45 s from here (initialization, the run, the
        // normal stop and the end-of-run report take far less), it exits with
        // code 5, even if initialization or the stop hangs.
        std::jthread runDeadline;
        if (bootSeconds) {
            runDeadline=std::jthread([bootSeconds](std::stop_token stop) {
                std::mutex mutex; std::condition_variable_any condition;
                std::unique_lock lock(mutex);
                condition.wait_for(lock,stop,std::chrono::seconds(bootSeconds+45u),[] { return false; });
                if (stop.stop_requested())
                    return;
                std::cerr<<"[boot-diagnostic] The run did not end "<<bootSeconds+45u
                         <<" s after it started (time limit + 45 s); exiting with code 5."<<std::endl;
                std::cout.flush();
                std::_Exit(5);
            });
        }
#endif

        std::string filePathStr = pathObj.string();
#if defined(PS2X_GAME_GT4)
        // The game's folders from the ELF's before the runtime starts (loadELF sets the same again): the
        // IOP services' start (MCSERV's reset makes both card folders) otherwise made mc0 and mc1 in the
        // working folder, e.g. two empty folders next to GT4.exe in a package (the card is game\mc0).
        PS2Runtime::configureIoPathsFromElf(filePathStr);
#endif
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
#if PS2X_BOOT_DIAGNOSTICS
        runtime.setHeadless(headless);
        const bool initialized=cpuOnly
            ? (runtime.memory().initialize() && runtime.syncCoreSubsystems())
            : runtime.initialize(windowTitle.c_str());
#else
        const bool initialized=runtime.initialize(windowTitle.c_str());
#endif
        if (!initialized)
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

#if defined(PS2X_GAME_GT4)
        const char* gt4Disc=std::getenv("GT4_DISC_IMAGE");
        if (const char* disc=gt4Disc && *gt4Disc ? gt4Disc : std::getenv("GT3_DISC_IMAGE"); disc && *disc) {
#else
        if (const char* disc=std::getenv("GT3_DISC_IMAGE"); disc && *disc) {
#endif
            auto paths=PS2Runtime::getIoPaths(); paths.cdImage=disc;
            PS2Runtime::setIoPaths(paths);
        }

#if PS2X_BOOT_DIAGNOSTICS
        if(bootSeconds) {
            if(const char* prefix=std::getenv("GT3_BOOT_DUMP_PREFIX"); prefix && *prefix)
                runtime.gs().setDebugHistoryPaused(false);
        }
        {
            std::jthread bootTimer;
            if (bootSeconds) {
                bootTimer=std::jthread([&runtime,bootSeconds](std::stop_token stop) {
                    std::mutex mutex; std::condition_variable_any condition;
                    std::unique_lock lock(mutex);
                    condition.wait_for(lock,stop,std::chrono::seconds(bootSeconds),[] { return false; });
                    if (!stop.stop_requested()) {
                        std::cout<<"[boot-diagnostic] Time limit reached; requesting normal runtime stop. This does not establish boot success."<<std::endl;
                        runtime.requestStop();
                    }
                });
            }
            if (cpuOnly) {
                std::cout<<"[boot-diagnostic] CPU-only execution; no window, rendering, audio or device initialization is tested."<<std::endl;
                runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
                auto* rdram=runtime.memory().getRDRAM();
                runtime.initializeEeKernelState(rdram);
                runtime.cpu().r[4]=_mm_setzero_si128();
                runtime.cpu().r[5]=_mm_setzero_si128();
                runtime.cpu().r[29]=_mm_set_epi64x(0,PS2_RAM_SIZE-0x10u);
                runtime.eeScheduler().reset(rdram,runtime.cpu());
                // The EE here as on run()'s game thread (the guest float mode,
                // no power throttling), with the 1 ms timer raylib gives a
                // window run, so VBlanks are paced alike.
                ThreadNaming::SetCurrentThreadName("GameThread");
                ThreadNaming::PreferPerformance();
                const unsigned hostCsr=_mm_getcsr();
                applyGuestFloatMode();
#if defined(_WIN32)
                timeBeginPeriod(1);
#endif
                std::cout<<"[boot-diagnostic] EE thread: guest float mode, performance cores, 1 ms timer."<<std::endl;
                {
                    // PS2X_FPS_LOG: the same [fps] lines as a window run (no presents).
                    std::jthread fpsLog;
                    if (const char* v=std::getenv("PS2X_FPS_LOG"); v && *v) {
                        fpsLog=std::jthread([&runtime](std::stop_token stop) {
                            std::mutex mutex; std::condition_variable_any condition;
                            std::unique_lock lock(mutex);
                            while (!condition.wait_for(lock,stop,std::chrono::milliseconds(100),[] { return false; }) &&
                                   !stop.stop_requested())
                                runtime.logFrameRate(0);
                        });
                    }
                    runtime.eeScheduler().run();
                }
#if defined(_WIN32)
                timeEndPeriod(1);
#endif
                _mm_setcsr(hostCsr);
            } else {
                runtime.run();
            }
            if (bootSeconds) {
                if (const char* prefix=std::getenv("GT3_BOOT_DUMP_PREFIX"); prefix && *prefix) {
                    // Opt-in local replay fixture, captured after guest execution stops.
                    // The raw context is an ABI-specific diagnostic, not a save-state format.
                    static_assert(std::is_trivially_copyable_v<R5900Context>);
                    auto write=[&](const char* suffix,const void* bytes,size_t size) {
                        std::ofstream file(std::string(prefix)+suffix,std::ios::binary|std::ios::trunc);
                        file.exceptions(std::ios::failbit|std::ios::badbit);
                        file.write(static_cast<const char*>(bytes),static_cast<std::streamsize>(size));file.close();
                    };
                    write(".rdram.bin",runtime.memory().getRDRAM(),PS2_RAM_SIZE);
                    runtime.syncGs();
                    runtime.gs().flushRaster();
                    write(".vram.bin",runtime.memory().getGSVRAM(),PS2_GS_VRAM_SIZE);
                    write(".vu0-code.bin",runtime.memory().getVU0Code(),PS2_VU0_CODE_SIZE);
                    write(".vu1-code.bin",runtime.memory().getVU1Code(),PS2_VU1_CODE_SIZE);
                    write(".vu0-data.bin",runtime.memory().getVU0Data(),PS2_VU0_DATA_SIZE);
                    write(".vu1-data.bin",runtime.memory().getVU1Data(),PS2_VU1_DATA_SIZE);
                    write(".ee.bin",&runtime.cpu(),sizeof(R5900Context));
                    std::ofstream regs(std::string(prefix)+".ee.txt",std::ios::trunc);
                    regs.exceptions(std::ios::failbit|std::ios::badbit);
                    regs<<"context-bytes="<<sizeof(R5900Context)<<" pc="<<std::hex<<runtime.cpu().pc<<'\n';
                    for(unsigned reg=0;reg<32;++reg) {
                        uint64_t lanes[2];std::memcpy(lanes,&runtime.cpu().r[reg],16);
                        regs<<std::dec<<reg<<' '<<std::hex<<lanes[0]<<' '<<lanes[1]<<'\n';
                    }
                    regs.close();
                    runtime.gs().latchHostPresentationFrame();
                    std::vector<uint8_t> pixels;
                    uint32_t width=0,height=0,display=0,source=0;bool preferred=false;
                    const bool frame=runtime.gs().copyLatchedHostPresentationFrame(
                        pixels,width,height,&display,&source,&preferred);
                    // Raw RGBA readback from the existing renderer; no synthetic image.
                    if(frame) write(".rgba.bin",pixels.data(),pixels.size());
                    std::ofstream graphics(std::string(prefix)+".gs.txt",std::ios::trunc);
                    graphics.exceptions(std::ios::failbit|std::ios::badbit);
                    graphics<<"frame="<<frame<<" width="<<width<<" height="<<height
                            <<" display="<<display<<" source="<<source<<" preferred="<<preferred
                            <<" vsync="<<runtime.eeScheduler().currentVSyncTick()<<'\n';
                    const auto& gsRegs=runtime.memory().gs();
                    graphics<<std::hex<<"pmode="<<gsRegs.pmode<<" smode2="<<gsRegs.smode2
                            <<" dispfb1="<<gsRegs.dispfb1<<" display1="<<gsRegs.display1
                            <<" dispfb2="<<gsRegs.dispfb2<<" display2="<<gsRegs.display2<<'\n';
                    graphics<<std::dec<<"native-image-uploads="<<runtime.gs().nativeImageUploadCount()
                            <<" native-packed-packets="<<runtime.gs().nativePackedGIFPacketCount()<<'\n';
                    graphics<<"history-seq,tick,kind,gif-bytes,vertices,x-min,x-max,y-min,y-max,gif-nloop,gif-flg,gif-nreg,register,register-value,transfer-pixels\n";
                    for(const auto& event:runtime.gs().getDebugHistory())
                        graphics<<event.seq<<','<<event.vsyncTick<<','<<static_cast<unsigned>(event.kind)
                                <<','<<event.gifSizeBytes<<','<<event.vertexCount<<','<<event.xMin
                                <<','<<event.xMax<<','<<event.yMin<<','<<event.yMax
                                <<','<<event.gifNloop<<','<<static_cast<unsigned>(event.gifFlg)
                                <<','<<static_cast<unsigned>(event.gifNreg)<<','<<static_cast<unsigned>(event.reg)
                                <<','<<event.regValue<<','<<event.transferPixels<<'\n';
                    graphics.close();
                    std::cout<<"[boot-diagnostic] Local main-context/RDRAM snapshot saved to "<<prefix<<std::endl;
                }
                const auto snapshot=runtime.eeScheduler().snapshot();
                for (const auto &thread : snapshot.threads) {
                    std::cout<<"[boot-diagnostic] Thread "<<thread.id<<" pc=0x"<<std::hex
                        <<thread.pc<<" entry=0x"<<thread.entry<<std::dec
                        <<" status="<<static_cast<int>(thread.status)
                        <<" wait="<<static_cast<int>(thread.waitReason)<<" wait-id="<<thread.waitId
                        <<" priority="<<thread.currentPriority<<std::endl;
                    // Threads not waiting may be spinning: show their registers
                    // (the executor has stopped, so the context is at rest).
                    const auto* live = runtime.eeScheduler().thread(thread.id);
                    if (live && static_cast<int>(thread.waitReason) == 0) {
                        const R5900Context& c = live->activeContext();
                        std::cout<<"[boot-diagnostic]   gpr"<<std::hex;
                        for (int r : {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16, 17, 18, 19, 29, 31})
                            std::cout<<" r"<<std::dec<<r<<std::hex<<"="<<getRegU32(&c, r);
                        std::cout<<std::dec<<"\n[boot-diagnostic]   fpr";
                        for (int f = 0; f < 32; ++f)
                            std::cout<<" f"<<f<<"="<<c.f[f];
                        std::cout<<std::endl;
                    }
                }
                for (const auto &sema:snapshot.semaphores) {
                    if (sema.waiters) std::cout<<"[boot-diagnostic] Semaphore "<<sema.id
                        <<" count="<<sema.count<<" max="<<sema.maxCount<<" waiters="<<sema.waiters<<std::endl;
                }
            }
            // Scope joins the timer before runtime destruction or the _Exit below.
        }
#else
        runtime.run();
#endif

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        // _Exit skips destructors: tell linked consoles we left (i.LINK Battle).
#if defined(PS2X_GAME_GT3)
        ps2x::iop::gt3::LinkTransport::goodbyeAll();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

#if defined(PS2X_GAME_GT3)
    ps2x::iop::gt3::LinkTransport::goodbyeAll();
#endif
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
