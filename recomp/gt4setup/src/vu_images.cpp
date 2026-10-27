// vu-images: the VU0/VU1 microcode images out of CORE.elf.
//
// Replaces the VU part of tools/gt4_embedded_regions.py (step 6), which found the
// two MPG uploads by analysis and wrote analysis/recomp-prep/vu/:
//   gt4_vu0_0x6d3280_raw.bin, gt4_vu0_code_image.bin (4 KiB), gt4_vu0_listing.txt
//   gt4_vu1_0x624dd0_decoded.bin, gt4_vu1_code_image.bin (16 KiB), gt4_vu1_listing.txt
// Here the uploads are pinned to GT4 USA (SCUS-97328); every instruction the
// constants come from is checked word for word, the LCG seed is read from the ELF,
// and the decoded images must have no reserved pairs and their known SHA-256.
//   VU0: 0x4a0584 jal 0x4a12b8 (unit 0 = VIF0, micro address 0, 0xa80 bytes from 0x6d3280), plain.
//   VU1: 0x4a05a4 jal 0x4a12b8 (unit 1 = VIF1, micro address 0, 0x3ff0 bytes from 0x624dd0), decoded
//        in place first by 0x4aa350 (called at 0x4a0500): byte ^= low byte of state, state =
//        state * 0x5d588b65 + 1 (0x4aa308), state word 0x62a03c, done-flag byte 0x62a038.
#include "util.h"
#include "vu_decode.h"

#include <algorithm>
#include <cstdio>

namespace gt4setup {

namespace {

struct Elf {
    Bytes file;
    struct Seg {
        uint32_t vaddr, offset, filesz;
    };
    std::vector<Seg> segs;

    explicit Elf(Bytes b) : file(std::move(b))
    {
        if (file.size() < 52 || file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F' || file[4] != 1 ||
            file[5] != 1)
            throw Error("not a little-endian ELF32 file");
        const uint32_t phoff = rd32(&file[28]);
        const uint16_t phentsize = rd16(&file[42]), phnum = rd16(&file[44]);
        for (uint32_t i = 0; i < phnum; ++i) {
            const size_t o = phoff + static_cast<size_t>(i) * phentsize;
            if (o + 32 > file.size())
                throw Error("ELF program header out of range");
            if (rd32(&file[o]) != 1)
                continue;
            segs.push_back({rd32(&file[o + 8]), rd32(&file[o + 4]), rd32(&file[o + 16])});
        }
    }
    const uint8_t *at(uint32_t addr, uint32_t size) const
    {
        for (const Seg &s : segs)
            if (addr >= s.vaddr && addr + size <= s.vaddr + s.filesz && s.offset + (addr - s.vaddr) + size <= file.size())
                return &file[s.offset + (addr - s.vaddr)];
        throw Error(strf("address 0x%x (0x%x bytes) is not in CORE.elf", addr, size));
    }
    uint32_t word(uint32_t addr) const { return rd32(at(addr, 4)); }
};

void expectWords(const Elf &elf, uint32_t addr, std::initializer_list<uint32_t> words)
{
    for (uint32_t w : words) {
        if (elf.word(addr) != w)
            throw Error(strf("CORE.elf 0x%x is %08x, expected %08x: not GT4 USA (SCUS-97328)", addr, elf.word(addr), w));
        addr += 4;
    }
}

uint32_t luiAddiu(uint32_t lui, uint32_t addiu)
{
    return ((lui & 0xFFFFu) << 16) + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(addiu & 0xFFFF)));
}

// (reserved pair count, E-bit count) as gt4_embedded_regions.vu_check
std::pair<int, int> vuCheck(const Bytes &blob)
{
    int bad = 0, ebits = 0;
    for (size_t pc = 0; pc + 7 < blob.size(); pc += 8) {
        const uint32_t lo = rd32(&blob[pc]), up = rd32(&blob[pc + 4]);
        if (vu::upperUsage(up).reserved || (!(up >> 31) && vu::lowerUsage(lo).reserved))
            ++bad;
        if (up & 0x40000000u)
            ++ebits;
    }
    return {bad, ebits};
}

void writeImage(const fs::path &dir, const char *unit, uint32_t src, uint32_t size, uint32_t vuaddrB, const Bytes &blob,
                bool decoded, size_t pad, const char *expectSha)
{
    const std::string sha = sha256Hex(blob);
    const auto chk = vuCheck(blob);
    if (chk.first != 0)
        throw Error(strf("%s image has %d reserved pairs", unit, chk.first));
    if (sha != expectSha)
        throw Error(std::string(unit) + " image sha256 " + sha + " is not the known " + expectSha);
    std::string lowerUnit = unit;
    for (char &c : lowerUnit)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    const std::string fname = strf("gt4_%s_0x%06x_%s.bin", lowerUnit.c_str(), src, decoded ? "decoded" : "raw");
    writeFile(dir / fname, blob);
    if (vuaddrB + blob.size() > pad)
        throw Error("image does not fit the micro memory");
    Bytes img(pad, 0);
    std::copy(blob.begin(), blob.end(), img.begin() + vuaddrB);
    writeFile(dir / ("gt4_" + lowerUnit + "_code_image.bin"), img);
    std::string listing = strf("# %s micro memory image from CORE 0x%x (0x%x bytes%s), vu1_decode.py format\n", unit, src,
                               size, decoded ? ", LCG-decoded" : "");
    for (uint32_t pc = vuaddrB; pc < vuaddrB + blob.size(); pc += 8)
        listing += vu::fmtPair(vu::decodePair(img.data(), pc)) + "\n";
    writeFile(dir / ("gt4_" + lowerUnit + "_listing.txt"), toCrlf(listing)); // Python text mode on Windows
    std::printf("%s %s src 0x%06x size 0x%x E %d sha256 %s -> %s\n", unit, decoded ? "LCG-decoded" : "plain", src, size,
                chk.second, sha.c_str(), pathstr(dir / ("gt4_" + lowerUnit + "_code_image.bin")).c_str());
}

} // namespace

int cmdVuImages(const Args &args)
{
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: gt4setup vu-images <CORE.elf> <outdir>\n");
        return 2;
    }
    const Elf elf(readFile(u8path(args[0])));
    const fs::path out = u8path(args[1]);

    // VU0 upload: a0 = 0 (unit), a1 = 0 (micro address), a2 = 0xa80, a3 = 0x6d3280; jal 0x4a12b8
    expectWords(elf, 0x4a0574, {0x3c07006d, 0x24e73280, 0x0000202d, 0x0000282d, 0x0c1284ae, 0x24060a80});
    const uint32_t vu0src = luiAddiu(elf.word(0x4a0574), elf.word(0x4a0578));
    const uint32_t vu0size = elf.word(0x4a0588) & 0xFFFF;
    // VU1 upload: a3 = 0x624dd0, a0 = 1, a1 = 0, a2 = 0x3ff0; jal 0x4a12b8
    expectWords(elf, 0x4a0594, {0x3c070062, 0x24e74dd0, 0x24040001, 0x0000282d, 0x0c1284ae, 0x24063ff0});
    const uint32_t vu1src = luiAddiu(elf.word(0x4a0594), elf.word(0x4a0598));
    const uint32_t vu1size = elf.word(0x4a05a8) & 0xFFFF;
    // in-place decoder call on the same buffer: a0 = 0x624dd0, jal 0x4aa350, a1 = 0x3ff0
    expectWords(elf, 0x4a04f8, {0x3c040062, 0x24844dd0, 0x0c12a8d4, 0x24053ff0});
    // decoder 0x4aa350: done flag byte (lui 0x63; lb -0x5fc8), xor loop calling 0x4aa308
    expectWords(elf, 0x4aa358, {0x3c120063, 0x8242a038});
    expectWords(elf, 0x4aa380, {0x0c12a8c2, 0x2631ffff, 0x92030000, 0x00621826, 0xa2030000});
    // LCG step 0x4aa308: state word 0x62a03c, multiplier 0x5d588b65, increment 1
    expectWords(elf, 0x4aa308, {0x3c040063, 0x3c035d58, 0x2484a03c, 0x34638b65, 0x8c820000, 0x00430019});
    expectWords(elf, 0x4aa338, {0x64420001});
    const uint32_t stateAddr = luiAddiu(elf.word(0x4aa308), elf.word(0x4aa310));
    const uint32_t mult = ((elf.word(0x4aa30c) & 0xFFFF) << 16) | (elf.word(0x4aa314) & 0xFFFF);
    const uint32_t inc = elf.word(0x4aa338) & 0xFFFF;
    const uint32_t flagAddr = luiAddiu(elf.word(0x4aa358), elf.word(0x4aa35c));
    uint32_t state = elf.word(stateAddr);
    if (*elf.at(flagAddr, 1) != 0)
        throw Error("VU1 decoder done flag is set in CORE.elf");

    const uint8_t *p0 = elf.at(vu0src, vu0size);
    const Bytes vu0(p0, p0 + vu0size);
    const uint8_t *p1 = elf.at(vu1src, vu1size);
    Bytes vu1(p1, p1 + vu1size);
    for (uint8_t &b : vu1) {
        state = state * mult + inc;
        b ^= static_cast<uint8_t>(state & 0xFF);
    }
    writeImage(out, "VU0", vu0src, vu0size, 0, vu0, false, 0x1000,
               "6ee4bdde81d98d0cf0cfdf65f5bb64697b4fd4bfee408d4d6125e39d8d602690");
    writeImage(out, "VU1", vu1src, vu1size, 0, vu1, true, 0x4000,
               "ae6b6cb3b4406ff3350a9d32b8abc044122c62e3e86f0e295bac54b38d2ac4c1");
    return 0;
}

} // namespace gt4setup
