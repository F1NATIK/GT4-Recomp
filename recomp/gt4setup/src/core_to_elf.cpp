// core-to-elf: port of tools/core_to_elf_gt4.py.
// Converts GT4 USA CORE.GT4 (SCUS-97328) to a byte-preserving MIPS ELF plus the
// <elf stem>.json report. No game instruction bytes are patched.
//
// Layout read from this exact image (2026-10-03):
//   segment 0x6179fc..0x617a14  .reginfo (gp 0x6dddf0), the tail of segment 1
//   segment 0x100000..0x617a14  code; the last function returns at 0x616f14,
//                               then the constructor / destructor tables at 0x616f20
//   segment 0x617a80..0x6d5dfc  data
//   startup clears BSS 0x6d5e00..0x8a215c (0x10011c..0x100194) and loads gp 0x6dddf0
// The RSA signature is not verified; the input is pinned by SHA-256 instead.
#include "inflate.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace gt4setup {

namespace {

const char *const CORE_SHA256 = "85d26aa8430154967b2633eede929286694ac39e99762527edcec365fd642ff9";
constexpr uint32_t TEXT_END = 0x616f20;
constexpr uint32_t BSS_START = 0x6d5e00, BSS_END = 0x8a215c, GP = 0x6dddf0;
// EE code stored inside .data (rt_crypt SHA-1/MD5, called by jal from .text), given its own
// executable section so ps2_recomp translates it at its real address.
constexpr uint32_t DATA_CODE_LO = 0x6c5400, DATA_CODE_HI = 0x6c833c;

struct Segment {
    uint32_t base;
    Bytes payload;
};

Json hexPair(uint64_t a, uint64_t b)
{
    Json j = Json::array();
    j.push(Json::string(pyhex(a)));
    j.push(Json::string(pyhex(b)));
    return j;
}

} // namespace

int cmdCoreToElf(const Args &args)
{
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: gt4setup core-to-elf <CORE.GT4> <CORE.elf>\n");
        return 2;
    }
    const fs::path source = u8path(args[0]), target = u8path(args[1]);
    const Bytes packed = readFile(source);
    if (packed.size() < 6)
        throw Error("Unsupported CORE header");
    const uint16_t flags = rd16(&packed[0]);
    const uint32_t expected = rd32(&packed[2]);
    if (flags != 0x0101 || !(0 < expected && expected <= 32u * 1024u * 1024u))
        throw Error("Unsupported CORE header");
    InflateResult inf = inflateRaw(packed.data() + 6, packed.size() - 6, static_cast<size_t>(expected) + 1u);
    if (!inf.error.empty())
        throw Error("CORE decompression failed: " + inf.error);
    const Bytes &raw = inf.data;
    if (raw.size() != expected || !inf.ended)
        throw Error("CORE decompression length or end-of-stream mismatch");
    size_t pos = 0;
    auto need = [&](size_t n) {
        if (pos + n > raw.size())
            throw Error("Unexpected GT4 CORE segment layout");
    };
    for (int k = 0; k < 2; ++k) {
        need(2);
        const uint16_t size = rd16(&raw[pos]);
        if (size != 128)
            throw Error("Unexpected RSA field");
        pos += 2u + size;
    }
    need(8);
    const uint32_t count = rd32(&raw[pos]), entry = rd32(&raw[pos + 4]);
    pos += 8;
    std::vector<Segment> segments;
    for (uint32_t k = 0; k < count; ++k) {
        need(8);
        const uint32_t base = rd32(&raw[pos]), size = rd32(&raw[pos + 4]);
        pos += 8;
        Bytes payload;
        if (pos < raw.size()) {
            const size_t avail = std::min<size_t>(size, raw.size() - pos);
            payload.assign(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                           raw.begin() + static_cast<std::ptrdiff_t>(pos + avail));
        }
        segments.push_back({base, std::move(payload)});
        pos += size;
    }
    if (pos != raw.size() || entry != 0x100008 || count != 3)
        throw Error("Unexpected GT4 CORE segment layout");

    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (const Segment &s : segments) {
        lo = std::min(lo, s.base);
        hi = std::max<uint32_t>(hi, s.base + static_cast<uint32_t>(s.payload.size()));
    }
    if (lo != 0x100000 || hi > BSS_START)
        throw Error("Initialized image bounds unexpected");
    Bytes image(hi - lo, 0);
    for (const Segment &s : segments)
        std::copy(s.payload.begin(), s.payload.end(), image.begin() + (s.base - lo));
    // dict((b, p) for b, p in segments)[0x6179fc]: the last segment with that base
    const Segment *reginfo = nullptr;
    for (const Segment &s : segments)
        if (s.base == 0x6179fc)
            reginfo = &s;
    if (!reginfo)
        throw Error("Unexpected GT4 CORE segment layout (no .reginfo)");
    if (reginfo->payload.size() < 24 || rd32(&reginfo->payload[20]) != GP)
        throw Error("reginfo gp mismatch");
    auto word = [&](uint32_t addr) { return rd32(&image[addr - lo]); };
    const std::pair<uint32_t, uint32_t> startup[] = {{0x10011c, 0x3c02006d}, {0x100120, 0x3c03008a},
                                                      {0x100124, 0x24425e00}, {0x100128, 0x2463215c},
                                                      {0x100198, 0x3c04006e}, {0x1001ac, 0x2484ddf0}};
    for (const auto &a : startup)
        if (word(a.first) != a.second)
            throw Error("Unrecognized startup layout: inspect BSS and GP manually");
    if (word(TEXT_END - 0xc) != 0x03e00008 || word(TEXT_END + 4) != 0xffffffff)
        throw Error("Code/data boundary marker not found: review TEXT_END");
    const std::string sha = sha256Hex(packed);
    if (sha != CORE_SHA256)
        throw Error("Unknown CORE build: code/data boundary needs review (sha256 " + sha + ")");

    const uint32_t size = static_cast<uint32_t>(image.size());
    const uint32_t memory_end = (BSS_END + 31) & ~15u; // startup SQ loop also writes the last block
    const uint32_t text_size = TEXT_END - lo;
    static const char names_c[] = "\0.text\0.data\0.bss\0.reginfo\0.shstrtab\0.text.rt_crypt\0.data.1\0";
    const std::string names(names_c, sizeof(names_c) - 1);
    auto name_at = [&](const char *n) {
        const std::string key = std::string(1, '\0') + n + std::string(1, '\0');
        return static_cast<uint32_t>(names.find(key) + 1);
    };
    const uint32_t section_count = 8;
    const uint32_t data_offset = 0x1000;
    const uint32_t reg_offset = (data_offset + size + 3) & ~3u;
    const uint32_t names_offset = reg_offset + 24;
    const uint32_t sh_offset = (names_offset + static_cast<uint32_t>(names.size()) + 3) & ~3u;
    Bytes elf(sh_offset + section_count * 40, 0);
    // ELF header
    const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::copy(ident, ident + 16, elf.begin());
    wr16(&elf[16], 2);
    wr16(&elf[18], 8);
    wr32(&elf[20], 1);
    wr32(&elf[24], entry);
    wr32(&elf[28], 52);
    wr32(&elf[32], sh_offset);
    wr32(&elf[36], 0x20924001);
    wr16(&elf[40], 52);
    wr16(&elf[42], 32);
    wr16(&elf[44], 2);
    wr16(&elf[46], 40);
    wr16(&elf[48], static_cast<uint16_t>(section_count));
    wr16(&elf[50], static_cast<uint16_t>(section_count - 1));
    auto pack = [&](uint32_t off, std::initializer_list<uint32_t> vals) {
        for (uint32_t v : vals) {
            wr32(&elf[off], v);
            off += 4;
        }
    };
    pack(52, {1, data_offset, lo, lo, text_size, text_size, 5, 0x1000});
    pack(84, {1, data_offset + text_size, lo + text_size, lo + text_size, size - text_size,
              memory_end - lo - text_size, 6, 0x1000});
    std::copy(image.begin(), image.end(), elf.begin() + data_offset);
    pack(reg_offset, {0, 0, 0, 0, 0, GP});
    std::copy(names.begin(), names.end(), elf.begin() + names_offset);
    auto off = [&](uint32_t addr) { return data_offset + addr - lo; };
    const uint32_t sections[7][10] = {
        {name_at(".text"), 1, 6, lo, data_offset, text_size, 0, 0, 64, 0},
        {name_at(".data"), 1, 3, TEXT_END, off(TEXT_END), DATA_CODE_LO - TEXT_END, 0, 0, 64, 0},
        {name_at(".text.rt_crypt"), 1, 6, DATA_CODE_LO, off(DATA_CODE_LO), DATA_CODE_HI - DATA_CODE_LO, 0, 0, 4, 0},
        {name_at(".data.1"), 1, 3, DATA_CODE_HI, off(DATA_CODE_HI), lo + size - DATA_CODE_HI, 0, 0, 4, 0},
        {name_at(".bss"), 8, 3, BSS_START, data_offset + size, memory_end - BSS_START, 0, 0, 16, 0},
        {name_at(".reginfo"), 0x70000006, 0, 0, reg_offset, 24, 0, 0, 4, 24},
        {name_at(".shstrtab"), 3, 0, 0, names_offset, static_cast<uint32_t>(names.size()), 0, 0, 1, 0},
    };
    for (uint32_t i = 1; i <= 7; ++i)
        for (int k = 0; k < 10; ++k)
            wr32(&elf[sh_offset + i * 40 + 4 * k], sections[i - 1][k]);
    writeFile(target, elf);

    Json report = Json::object();
    report.set("source_sha256", Json::string(sha));
    report.set("elf_sha256", Json::string(sha256Hex(elf)));
    report.set("core_rsa_verified", Json::boolean(false));
    report.set("decompressed_bytes", Json::integer(static_cast<int64_t>(raw.size())));
    Json segs = Json::array();
    for (const Segment &s : segments)
        segs.push(hexPair(s.base, s.payload.size()));
    report.set("segments", segs);
    report.set("entry", Json::string(pyhex(entry)));
    report.set("gp", Json::string(pyhex(GP)));
    report.set("text", hexPair(lo, TEXT_END));
    report.set("data", hexPair(TEXT_END, hi));
    report.set("bss_start", Json::string(pyhex(BSS_START)));
    report.set("bss_logical_end", Json::string(pyhex(BSS_END)));
    report.set("memory_end_including_startup_padding", Json::string(pyhex(memory_end)));
    report.set("instruction_bytes_changed", Json::integer(0));
    report.set("data_code_section", hexPair(DATA_CODE_LO, DATA_CODE_HI));
    report.set("section_note", Json::string("Text ends after the last function (jr ra at 0x616f14); 0x616f20 starts the "
                                            "ctor/dtor tables (0, -1, pointers). Gap 0x617a14..0x617a80 is zero-filled."));
    fs::path jsonPath = target;
    jsonPath.replace_extension(".json");
    writeFile(jsonPath, toCrlf(report.dump(2) + "\n"));
    std::printf("%s\n", report.dump(2).c_str());
    return 0;
}

} // namespace gt4setup
