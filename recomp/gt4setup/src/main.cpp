// gt4setup: makes the GT4 recomp's game-specific inputs from the player's own disc.
// One command-line tool with a subcommand per step (each replaces a Python script).
#include "util.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace {

const char *const kUsage =
    "gt4setup - builds the GT4 recomp's game code inputs from your own disc (GT4 USA, SCUS-97328)\n"
    "\n"
    "usage: gt4setup <command> [arguments]\n"
    "\n"
    "  extract-boot   <iso> <outdir>\n"
    "                 copy the boot files (not GT4.VOL) and write boot-files-manifest.json\n"
    "  core-to-elf    <CORE.GT4> <CORE.elf>\n"
    "                 unpack CORE.GT4 into a byte-preserving MIPS ELF (+ CORE.json)\n"
    "  vu-images      <CORE.elf> <outdir>\n"
    "                 extract the VU0/VU1 microcode images (VU1 de-obfuscated)\n"
    "  vu-translate   [--vu0] [--no-sticky] [--entries pc,pc,...] <code.bin | kicks.bin> <out_dir>\n"
    "                 translate a VU microcode image to C++ (and rewrite the registry)\n"
    "  vu-disasm      <code.bin | kicks.bin> [start_hex end_hex]\n"
    "                 list a VU1 image in vu1_decode.py's format\n"
    "  late-functions <old core-generated> <new ps2_recomp output> <out .cpp>\n"
    "                 collect the functions only the second ps2_recomp run made\n"
    "  build          --iso <iso> --source <recomp> --config <config> --tools <tools> --work <work>\n"
    "                 [--deps <folder>] [--jobs N] [--from <step>]\n"
    "                 every step from the disc to GT4.exe (the launcher's Build GT4); see build.cpp\n";

int run(const gt4setup::Args &all)
{
    using namespace gt4setup;
    if (all.empty() || all[0] == "-h" || all[0] == "--help" || all[0] == "help") {
        std::fputs(kUsage, all.empty() ? stderr : stdout);
        return all.empty() ? 2 : 0;
    }
    const std::string cmd = all[0];
    const Args args(all.begin() + 1, all.end());
    try {
        if (cmd == "extract-boot")
            return cmdExtractBoot(args);
        if (cmd == "core-to-elf")
            return cmdCoreToElf(args);
        if (cmd == "vu-images")
            return cmdVuImages(args);
        if (cmd == "vu-translate")
            return cmdVuTranslate(args);
        if (cmd == "vu-disasm")
            return cmdVuDisasm(args);
        if (cmd == "late-functions")
            return cmdLateFunctions(args);
        if (cmd == "build")
            return cmdBuild(args);
    } catch (const std::exception &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "gt4setup %s: error: %s\n", cmd.c_str(), e.what());
        return 1;
    }
    std::fprintf(stderr, "gt4setup: unknown command '%s'\n\n%s", cmd.c_str(), kUsage);
    return 2;
}

} // namespace

#if defined(_WIN32)
// Unicode command line on Windows (paths with any characters), converted to UTF-8.
static std::string toUtf8(const wchar_t *w)
{
    std::string s;
    for (size_t i = 0; w[i]; ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (cp >= 0xD800 && cp < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        }
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return s;
}

int wmain(int argc, wchar_t **argv)
{
    gt4setup::Args all;
    for (int i = 1; i < argc; ++i)
        all.push_back(toUtf8(argv[i]));
    return run(all);
}
#else
int main(int argc, char **argv)
{
    gt4setup::Args all;
    for (int i = 1; i < argc; ++i)
        all.push_back(argv[i]);
    return run(all);
}
#endif
