// build: every step from the player's own disc to GT4.exe, for the launcher's "Build GT4" step.
//
// usage: gt4setup build --iso <GT4 USA iso> --source <recomp folder> --config <release config folder>
//                       --tools <tools folder> --work <work folder> [--jobs N] [--from <step>]
//
//   --source  the folder with the recomp's CMakeLists.txt (recomp\)
//   --config  core-gt4.toml, core-functions-gt4-authoritative.csv, core-extra-entry-points.txt and
//             late-function-starts.txt (addresses only: analysis\design\release-pipeline\config\README.md)
//   --tools   ps2_recomp.exe, llvm-mingw\ (bin\clang++.exe), cmake\ (bin\cmake.exe) and ninja\ninja.exe
//   --work    everything made from the disc: game\ (the boot files and CORE.elf), vu\, core-generated\,
//             gt4_late_functions.cpp and build\ (the CMake build; kept, so a rebuild takes a minute)
//   --deps    optional: raylib-src, glslang-src, volk-src, vulkan_headers-src (else CMake fetches them)
//   --from    start at a step (boot, elf, vu, recomp, late, configure, compile), the earlier ones' output kept
//   --install optional: when it is built, GT4.exe and game\ (the boot files and CORE.elf) are copied here (the
//             launcher's folder), each one through a temporary name so a failed copy leaves the old one
//
// Each step prints "@step <n> <of> <name>: <what>" first, then its own output; "@done <GT4.exe>" at the end.
// The tools run at below-normal priority with no window. Nothing outside --work is written.
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gt4setup {

namespace {

const char *const kSteps[] = {"boot", "elf", "vu", "recomp", "late", "configure", "compile"};
constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

// The shipped VU entry points (README.md: VU1 with sticky flags off, VU0 with the 14 VCALLMS entries).
const char *const kVu1Entries = "1b08,1b10,1b30,1b38,1b58,1b60,1b80,1b88,1ba8,1bb0,1bd0,1bd8,1bf8,1c00,1c20,1c28,1c48,1c50,"
                                "1c70,1c78,1c98,1ca0,1cc0,1cc8,1ce8,1cf0,1d10,1d18,1d38,1d40,1d60,1d68,1d88,1d90,1db0,1db8,"
                                "1dd8,1de0,1e00,1e08,1e28,1e30,1e50,1e58,1e78,1e80,1ea0,1ea8,1ec8,1ed0,3d50,3ea0";
const char *const kVu0Entries = "58,78,b8,f8,2e8,308,460,478,548,678,700,858,900,930";

std::string fwd(const fs::path &p)
{
    std::string s = pathstr(fs::absolute(p));
    for (char &c : s)
        if (c == '\\')
            c = '/';
    return s;
}

void need(const fs::path &p, const char *what)
{
    if (!fs::exists(p))
        throw Error(strf("%s not found: %s", what, pathstr(p).c_str()));
}

#if defined(_WIN32)
std::wstring wide(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument quoted the way CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring &a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
        return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}
#endif

// Runs a program (its output goes to ours) and throws when it fails. cwd: its working folder.
void runTool(const fs::path &exe, const Args &args, const fs::path &cwd, const std::string &pathFirst = {})
{
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    std::wstring cmd = quoteArg(exe.wstring());
    for (const std::string &a : args)
        cmd += L" " + quoteArg(wide(a));
    // A copy of our environment, with pathFirst in front of PATH (llvm-mingw's bin for CMake's compiler checks).
    std::wstring env;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (LPWCH v = block; *v; v += wcslen(v) + 1) {
            std::wstring e = v;
            if (!pathFirst.empty() && _wcsnicmp(e.c_str(), L"PATH=", 5) == 0)
                e = L"PATH=" + wide(pathFirst) + L";" + e.substr(5);
            env += e;
            env += L'\0';
        }
        FreeEnvironmentStringsW(block);
    }
    env += L'\0';
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring dir = cwd.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_UNICODE_ENVIRONMENT,
                        env.data(), dir.c_str(), &si, &pi))
        throw Error(strf("could not start %s (Windows error %lu)", pathstr(exe).c_str(), GetLastError()));
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0)
        throw Error(strf("%s failed (exit code %lu)", pathstr(exe.filename()).c_str(), code));
#else
    std::string cmd = "cd '" + pathstr(cwd) + "' && ";
    if (!pathFirst.empty())
        cmd += "PATH='" + pathFirst + "':\"$PATH\" ";
    cmd += "'" + pathstr(exe) + "'";
    for (const std::string &a : args)
        cmd += " '" + a + "'";
    if (std::system(cmd.c_str()) != 0)
        throw Error(strf("%s failed", pathstr(exe.filename()).c_str()));
#endif
}

// core-gt4.toml with its four paths set to absolute ones (the shipped file's are relative to the repository).
std::string tomlWithPaths(const std::string &toml, const fs::path &elf, const fs::path &out, const fs::path &functions,
                          const fs::path &extra)
{
    std::string res;
    size_t at = 0;
    while (at < toml.size()) {
        size_t end = toml.find('\n', at);
        if (end == std::string::npos)
            end = toml.size();
        std::string line = toml.substr(at, end - at);
        auto key = [&](const char *k) { return line.rfind(k, 0) == 0; };
        const bool cr = !line.empty() && line.back() == '\r';
        const std::string nl = cr ? "\r" : "";
        if (key("input ="))
            line = "input = \"" + fwd(elf) + "\"" + nl;
        else if (key("output ="))
            line = "output = \"" + fwd(out) + "\"" + nl;
        else if (key("ghidra_output ="))
            line = "ghidra_output = \"" + fwd(functions) + "\"" + nl;
        else if (key("extra_entry_points_file ="))
            line = "extra_entry_points_file = \"" + fwd(extra) + "\"" + nl;
        res += line;
        if (end < toml.size())
            res += '\n';
        at = end + 1;
    }
    return res;
}

} // namespace

int cmdBuild(const Args &args)
{
    fs::path iso, source, config, tools, work, deps, install;
    int jobs = static_cast<int>(std::thread::hardware_concurrency());
    int from = 0;
    for (size_t i = 0; i < args.size(); ++i) {
        auto next = [&]() -> const std::string & {
            if (i + 1 >= args.size())
                throw Error("missing value after " + args[i]);
            return args[++i];
        };
        const std::string &a = args[i];
        if (a == "--iso")
            iso = u8path(next());
        else if (a == "--source")
            source = u8path(next());
        else if (a == "--config")
            config = u8path(next());
        else if (a == "--tools")
            tools = u8path(next());
        else if (a == "--work")
            work = u8path(next());
        else if (a == "--deps")
            deps = u8path(next());
        else if (a == "--install")
            install = u8path(next());
        else if (a == "--jobs")
            jobs = std::atoi(next().c_str());
        else if (a == "--from") {
            const std::string s = next();
            from = -1;
            for (int k = 0; k < kStepCount; ++k)
                if (s == kSteps[k])
                    from = k;
            if (from < 0)
                throw Error("--from: unknown step '" + s + "' (boot, elf, vu, recomp, late, configure, compile)");
        } else
            throw Error("unknown option " + a);
    }
    if (iso.empty() || source.empty() || config.empty() || tools.empty() || work.empty())
        throw Error("usage: gt4setup build --iso <iso> --source <recomp> --config <config> --tools <tools> --work <work>");
    jobs = jobs > 0 ? jobs : 4;
    work = fs::absolute(work);

    const fs::path recomp = tools / "ps2_recomp.exe", llvm = tools / "llvm-mingw", cmake = tools / "cmake" / "bin" / "cmake.exe",
                   ninja = tools / "ninja" / "ninja.exe";
    need(iso, "the GT4 disc image");
    need(source / "CMakeLists.txt", "the recomp source");
    for (const char *f : {"core-gt4.toml", "core-functions-gt4-authoritative.csv", "core-extra-entry-points.txt", "late-function-starts.txt"})
        need(config / f, "the release config");
    need(recomp, "ps2_recomp");
    need(llvm / "bin", "llvm-mingw");
    need(cmake, "CMake");
    need(ninja, "Ninja");

    const fs::path game = work / "game", vuImages = work / "vu-images", vu = work / "vu", generated = work / "core-generated",
                   lateGenerated = work / "late-generated", late = work / "gt4_late_functions.cpp", build = work / "build";
    fs::create_directories(work);
    auto step = [&](int n, const char *what) {
        if (n < from)
            return false;
        std::printf("@step %d %d %s: %s\n", n + 1, kStepCount, kSteps[n], what);
        std::fflush(stdout);
        return true;
    };

    if (step(0, "copying the boot files off the disc"))
        cmdExtractBoot({pathstr(iso), pathstr(game)});
    if (step(1, "unpacking CORE.GT4 into CORE.elf"))
        cmdCoreToElf({pathstr(game / "CORE.GT4"), pathstr(game / "CORE.elf")});
    if (step(2, "translating the VU microprograms")) {
        fs::remove_all(vu);
        cmdVuImages({pathstr(game / "CORE.elf"), pathstr(vuImages)});
        cmdVuTranslate({"--no-sticky", "--entries", kVu1Entries, pathstr(vuImages / "gt4_vu1_code_image.bin"), pathstr(vu)});
        cmdVuTranslate({"--vu0", "--entries", kVu0Entries, pathstr(vuImages / "gt4_vu0_code_image.bin"), pathstr(vu)});
    }
    const std::string toml = readText(config / "core-gt4.toml");
    if (step(3, "recompiling GT4's game code")) {
        fs::remove_all(generated);
        writeFile(work / "core-gt4.toml",
                  tomlWithPaths(toml, game / "CORE.elf", generated, config / "core-functions-gt4-authoritative.csv", config / "core-extra-entry-points.txt"));
        runTool(recomp, {pathstr(work / "core-gt4.toml")}, work);
    }
    if (step(4, "recompiling the pointer-only functions")) {
        fs::remove_all(lateGenerated);
        writeFile(work / "late-extra-entry-points.txt",
                  readText(config / "core-extra-entry-points.txt") + "\n# the pointer-only functions' starts\n" + readText(config / "late-function-starts.txt"));
        writeFile(work / "late-gt4.toml", tomlWithPaths(toml, game / "CORE.elf", lateGenerated, config / "core-functions-gt4-authoritative.csv",
                                                        work / "late-extra-entry-points.txt"));
        runTool(recomp, {pathstr(work / "late-gt4.toml")}, work);
        cmdLateFunctions({pathstr(generated), pathstr(lateGenerated), pathstr(late)});
        fs::remove_all(lateGenerated);
    }
    const std::string pathFirst = pathstr(llvm / "bin") + ";" + pathstr(ninja.parent_path());
    if (step(5, "setting up the build")) {
        Args cfg = {"-S", pathstr(source), "-B", pathstr(build), "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + fwd(ninja),
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_TOOLCHAIN_FILE=" + fwd(source / "cmake" / "windows-llvm-mingw.cmake"),
                    "-DPS2X_GAME=GT4", "-DGT3_VULKAN=ON", "-DGT4_STOCK_BUILD=OFF", "-DPS2X_BUILD_RUNTIME=ON", "-DPS2X_BUILD_RECOMP=OFF",
                    "-DPS2X_BUILD_ANALYZER=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF", "-DPS2X_ENABLE_DEBUG_UI=OFF",
                    "-DPS2X_ENABLE_FFMPEG=OFF", "-DPS2X_ENABLE_SCCACHE=OFF", "-DPS2X_GENERATED_DIR=" + fwd(generated),
                    "-DPS2X_VU_GENERATED_DIR=" + fwd(vu), "-DPS2X_LATE_FUNCTIONS_CPP=" + fwd(late)};
        if (!deps.empty()) {
            cfg.push_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
            for (const auto &[var, dir] : {std::pair<const char *, const char *>{"RAYLIB", "raylib-src"}, {"GLSLANG", "glslang-src"},
                                           {"VOLK", "volk-src"}, {"VULKAN_HEADERS", "vulkan_headers-src"}})
                cfg.push_back(std::string("-DFETCHCONTENT_SOURCE_DIR_") + var + "=" + fwd(deps / dir));
        }
#if defined(_WIN32)
        _wputenv_s(L"LLVM_MINGW_ROOT", fs::absolute(llvm).wstring().c_str());
#else
        setenv("LLVM_MINGW_ROOT", pathstr(fs::absolute(llvm)).c_str(), 1);
#endif
        runTool(cmake, cfg, work, pathFirst);
    }
    if (step(6, "compiling GT4.exe (the longest step)")) {
#if defined(_WIN32)
        _wputenv_s(L"LLVM_MINGW_ROOT", fs::absolute(llvm).wstring().c_str());
#endif
        runTool(cmake, {"--build", pathstr(build), "--target", "ps2EntryRunner", "--parallel", std::to_string(jobs)}, work, pathFirst);
    }
    const fs::path exe = build / "ps2xRuntime" / "GT4.exe";
    need(exe, "the built GT4.exe");
    fs::path done = exe;
    if (!install.empty()) {
        std::printf("@install %s\n", pathstr(install).c_str());
        std::fflush(stdout);
        fs::create_directories(install);
        // game\ first (GT4.exe without its CORE.elf would not start), then GT4.exe. Only the disc's files are copied
        // over: game\ also holds the memory cards (mc0, mc1) and GT4's cache, which stay as they are.
        for (const fs::directory_entry &e : fs::recursive_directory_iterator(game)) {
            const fs::path to = install / "game" / fs::relative(e.path(), game);
            if (e.is_directory())
                fs::create_directories(to);
            else {
                fs::path tmp = to;
                tmp += ".new";
                fs::copy_file(e.path(), tmp, fs::copy_options::overwrite_existing);
                fs::rename(tmp, to);
            }
        }
        done = install / "GT4.exe";
        fs::copy_file(exe, install / "GT4.exe.new", fs::copy_options::overwrite_existing);
        fs::rename(install / "GT4.exe.new", done);
    }
    std::printf("@done %s\n", pathstr(done).c_str());
    return 0;
}

} // namespace gt4setup
