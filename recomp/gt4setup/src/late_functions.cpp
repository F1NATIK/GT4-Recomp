// late-functions: port of analysis/design/multi-monitor/scripts/make_late_functions.py.
// GT4's pointer-only functions that the main recompiled set lacks, as one source
// file: the functions a second ps2_recomp run (with every gap between Ghidra's
// functions as an extra entry point) made in addition to the first run, plus a
// table that enters them in the dispatch table (empty slots only) when GT4_NET is on.
//
// usage: gt4setup late-functions <old core-generated> <new ps2_recomp output> <out .cpp>
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace gt4setup {

namespace {

// Python text-mode read (universal newlines): \r\n and lone \r become \n.
std::string readUniversal(const fs::path &p)
{
    const std::string raw = readText(p);
    std::string s;
    s.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            s += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n')
                ++i;
        } else {
            s += raw[i];
        }
    }
    return s;
}

bool isWord(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool isSpace(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F); }

// str.splitlines() on the (already \n-normalised) text
std::vector<std::string> splitLines(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    auto flush = [&]() {
        out.push_back(cur);
        cur.clear();
    };
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n' || c == '\v' || c == '\f' || c == 0x1C || c == 0x1D || c == 0x1E) {
            flush();
            ++i;
        } else if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x85) {
            flush();
            i += 2;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) == 0xA8 || static_cast<unsigned char>(s[i + 2]) == 0xA9)) {
            flush();
            i += 3;
        } else {
            cur += s[i++];
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string rstrip(const std::string &s)
{
    size_t e = s.size();
    while (e > 0 && isSpace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(0, e);
}

std::set<std::string> listNames(const fs::path &dir)
{
    std::set<std::string> names;
    for (const auto &e : fs::directory_iterator(dir))
        names.insert(pathstr(e.path().filename()));
    return names;
}

// re.search(r'^void (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{', text, re.M)
bool findFunction(const std::string &text, size_t &start, std::string &name)
{
    static const std::string tail = "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {";
    size_t pos = 0;
    while (pos <= text.size()) {
        if (text.compare(pos, 5, "void ") == 0) {
            size_t e = pos + 5;
            while (e < text.size() && isWord(static_cast<unsigned char>(text[e])))
                ++e;
            if (e > pos + 5 && text.compare(e, tail.size(), tail) == 0) {
                start = pos;
                name = text.substr(pos + 5, e - pos - 5);
                return true;
            }
        }
        const size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }
    return false;
}

// re.match(r'\s*g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // (0x[0-9a-f]+)', line)
bool matchTableLine(const std::string &line, std::string &func, uint32_t &addr)
{
    size_t i = 0;
    while (i < line.size() && isSpace(static_cast<unsigned char>(line[i])))
        ++i;
    static const std::string a = "g_ps2RecompiledFunctionTable[";
    if (line.compare(i, a.size(), a) != 0)
        return false;
    i += a.size();
    const size_t d0 = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
        ++i;
    if (i == d0 || line.compare(i, 4, "] = ") != 0)
        return false;
    i += 4;
    const size_t w0 = i;
    while (i < line.size() && isWord(static_cast<unsigned char>(line[i])))
        ++i;
    if (i == w0)
        return false;
    // \w+ is greedy but may backtrack; the next characters must be "; // 0x"
    func = line.substr(w0, i - w0);
    if (line.compare(i, 7, "; // 0x") != 0)
        return false;
    i += 7;
    const size_t h0 = i;
    uint64_t v = 0;
    while (i < line.size() && ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f'))) {
        v = v * 16 + static_cast<uint64_t>(line[i] <= '9' ? line[i] - '0' : line[i] - 'a' + 10);
        ++i;
    }
    if (i == h0)
        return false;
    addr = static_cast<uint32_t>(v);
    return true;
}

} // namespace

int cmdLateFunctions(const Args &args)
{
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: gt4setup late-functions <old core-generated> <new ps2_recomp output> <out .cpp>\n");
        return 2;
    }
    const fs::path oldDir = u8path(args[0]), newDir = u8path(args[1]), out = u8path(args[2]);
    const std::set<std::string> oldNames = listNames(oldDir), newNames = listNames(newDir);
    std::vector<std::string> names;
    for (const std::string &n : newNames)
        if (!oldNames.count(n))
            names.push_back(n); // std::set: sorted by UTF-8 bytes = Python's code point order
    std::vector<std::string> funcs, bodies;
    for (const std::string &f : names) {
        if (f.size() < 4 || f.compare(f.size() - 4, 4, ".cpp") != 0)
            continue;
        const std::string text = readUniversal(newDir / u8path(f));
        size_t mstart = 0;
        std::string fname;
        if (!findFunction(text, mstart, fname))
            throw Error("no function in " + f);
        funcs.push_back(fname);
        static const std::string marker = "// Function:";
        size_t start = std::string::npos;
        if (mstart >= marker.size())
            start = text.rfind(marker, mstart - marker.size());
        bodies.push_back(rstrip(text.substr(start != std::string::npos ? start : mstart)) + "\n");
    }
    std::vector<std::pair<uint32_t, std::string>> table;
    const std::vector<std::string> oldreg = splitLines(readUniversal(oldDir / "register_functions.cpp"));
    const std::vector<std::string> newreg = splitLines(readUniversal(newDir / "register_functions.cpp"));
    const std::set<std::string> oldset(oldreg.begin(), oldreg.end());
    const std::set<std::string> known(funcs.begin(), funcs.end());
    for (const std::string &line : newreg) {
        if (oldset.count(line))
            continue;
        std::string func;
        uint32_t addr = 0;
        if (!matchTableLine(line, func, addr))
            throw Error("unexpected new line in register_functions.cpp: " + line);
        if (!known.count(func))
            throw Error("a new table line names an old function: " + line);
        table.push_back({addr, func});
    }
    std::vector<std::string> o;
    o.push_back("// GENERATED by analysis/design/multi-monitor/scripts/make_late_functions.py from ps2_recomp output (do not edit).");
    o.push_back(strf("// GT4 CORE functions reached only through pointers that the main generated set lacks (%d functions, %d table",
                     static_cast<int>(funcs.size()), static_cast<int>(table.size())));
    o.push_back("// entries). Entered in the dispatch table by gt4InstallLateFunctions (empty slots only) when GT4_NET is on.");
    o.push_back("#include <cstdint>");
    o.push_back("#include <cstdio>");
    o.push_back("#include <stdexcept>");
    o.push_back("#include \"ps2_runtime_macros.h\"");
    o.push_back("#include \"ps2_runtime.h\"");
    o.push_back("#include \"ps2_recompiled_functions.h\"");
    o.push_back("#include \"ps2_recompiled_stubs.h\"");
    o.push_back("#include \"ps2_syscalls.h\"");
    o.push_back("#include \"ps2_stubs.h\"");
    o.push_back("#ifdef PS2_FUNCTION_LOG_TRACKER");
    o.push_back("#include \"ps2_log.h\"");
    o.push_back("#endif");
    o.push_back("");
    for (const std::string &f : funcs)
        o.push_back("void " + f + "(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime);");
    o.push_back("");
    o.insert(o.end(), bodies.begin(), bodies.end());
    o.push_back("namespace");
    o.push_back("{");
    o.push_back("    struct LateEntry");
    o.push_back("    {");
    o.push_back("        uint32_t address;");
    o.push_back("        PS2Runtime::RecompiledFunction function;");
    o.push_back("    };");
    o.push_back("    const LateEntry kLateEntries[] = {");
    for (const auto &e : table)
        o.push_back(strf("        {0x%xu, %s},", e.first, e.second.c_str()));
    o.push_back("    };");
    o.push_back("}");
    o.push_back("");
    o.push_back("// Enters the functions in empty dispatch-table slots; returns how many were entered.");
    o.push_back("int gt4InstallLateFunctions(PS2Runtime &runtime)");
    o.push_back("{");
    o.push_back("    int entered = 0;");
    o.push_back("    for (const LateEntry &e : kLateEntries)");
    o.push_back("        if (!runtime.hasFunction(e.address) && runtime.replaceFunction(e.address, e.function))");
    o.push_back("            ++entered;");
    o.push_back("    return entered;");
    o.push_back("}");
    o.push_back("");
    o.push_back("// The runtime (Gt4HostGlue.cpp) calls it through this pointer: the generated code lives in GT4.exe, not in the");
    o.push_back("// ps2_runtime library.");
    o.push_back("extern int (*g_gt4LateFunctionsInstaller)(PS2Runtime &);");
    o.push_back("namespace");
    o.push_back("{");
    o.push_back("    const bool kLateInstallerSet = (g_gt4LateFunctionsInstaller = &gt4InstallLateFunctions, true);");
    o.push_back("}");
    std::string text;
    for (size_t i = 0; i < o.size(); ++i) {
        if (i)
            text += '\n';
        text += o[i];
    }
    text += '\n';
    writeFile(out, text);
    std::printf("%s: %d functions, %d table entries, %d bytes\n", pathstr(out).c_str(), static_cast<int>(funcs.size()),
                static_cast<int>(table.size()), static_cast<int>(text.size()));
    return 0;
}

} // namespace gt4setup
