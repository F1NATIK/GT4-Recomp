#pragma once

// GT4_EXTRA_CARS (analysis/jp-cars/README-BUILD.md, ESSO-SUPRA-FINDINGS.md 6): a car from the developer's own Japanese GT4 disc
// added to the US disc as a NEW car, served from a pack the builder makes on this PC (analysis/jp-cars/scripts/
// build_esso_pack.py; the pack holds disc-derived bytes, so it is never committed). Header-only and std-only: GT4.exe's
// IOP disc layer (gt4_common.cpp / gt4_extra_cars.cpp) and the launcher's GT4.VOL reader (savelib/gt4save_disc.cpp)
// read the same pack, so both see the same files.
//
// What a pack serves over the US image (SCUS-97328, 5,314,478,080 bytes):
//   - GT4.VOL's real table of contents (tocSectors from tocLba): the disc's, with only the changed files' entries
//     rewritten (page, stored size, real size), the TOC pages holding them deflated again;
//   - the changed files themselves, stored as the disc stores their entries (C5 EE F7 FF, -real size, raw deflate; or
//     plain), at LBAs from the image's end on (extLba = 2,594,960): nothing on the image moves.
// matches() is the checksum fallback: the image's size, the SHA-256 of its TOC sectors and of every replaced file's
// stored bytes must be the ones the pack was built from; otherwise nothing is served (the caller logs why).
//
// Layout (little endian; build_esso_pack.py writes it):
//   0   "GT4XCAR1"   8   u32 version (1)   12 u32 file count   16 char[16] name ("esso")
//   32  u64 image bytes   40 u32 tocLba   44 u32 tocSectors   48 u8[32] SHA-256 of the disc's TOC sectors
//   80  u32 extLba   84 u32 extSectors   88 u32 record bytes   92 u32 0
//   96  records: u32 usLba, u32 usStored, u8[32] usSha, u32 newLba, u32 newStored, u32 newReal, u8 type (1 plain,
//       2 compressed), u8 0, u16 path length, path
//   then the served TOC (tocSectors * 2048), then the extension (extSectors * 2048), then the SHA-256 of all before.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::iop::gt4::extracars
{
    using Digest = std::array<uint8_t, 32>;

    // SHA-256 (FIPS 180-4).
    inline Digest sha256(const uint8_t *data, size_t size)
    {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
        auto block = [&](const uint8_t *p) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
            for (int i = 16; i < 64; ++i)
            {
                const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (int i = 0; i < 64; ++i)
            {
                const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
                const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                hh = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        };
        size_t i = 0;
        for (; i + 64 <= size; i += 64)
            block(data + i);
        uint8_t tail[128] = {};
        const size_t rest = size - i;
        if (rest)
            std::memcpy(tail, data + i, rest);
        tail[rest] = 0x80;
        const size_t tailLen = rest + 9 <= 64 ? 64 : 128;
        const uint64_t bits = uint64_t(size) * 8u;
        for (int b = 0; b < 8; ++b)
            tail[tailLen - 1 - b] = uint8_t(bits >> (8 * b));
        block(tail);
        if (tailLen == 128)
            block(tail + 64);
        Digest out{};
        for (int j = 0; j < 8; ++j)
            for (int b = 0; b < 4; ++b)
                out[size_t(4 * j + b)] = uint8_t(h[j] >> (24 - 8 * b));
        return out;
    }

    inline std::string hexOf(const Digest &d, size_t bytes = 32)
    {
        static const char *x = "0123456789abcdef";
        std::string s;
        for (size_t i = 0; i < bytes && i < d.size(); ++i)
        {
            s += x[d[i] >> 4];
            s += x[d[i] & 15];
        }
        return s;
    }

    struct PackFile
    {
        std::string path;   // "specdb/GT4_US2560/GENERIC_CAR.dbt"
        uint8_t type = 0;   // 1 plain, 2 compressed (C5 EE F7 FF)
        uint32_t usLba = 0, usStored = 0;
        Digest usSha{};     // the US disc's stored bytes
        uint32_t newLba = 0, newStored = 0, newReal = 0;
    };

    class Pack
    {
    public:
        static constexpr uint32_t kSector = 2048;
        using Reader = std::function<bool(uint64_t offset, void *destination, size_t bytes)>;

        // A pack's bytes; nullptr and `why` when they are not a whole, consistent pack.
        static std::shared_ptr<const Pack> parse(std::vector<uint8_t> bytes, std::string &why)
        {
            auto p = std::shared_ptr<Pack>(new Pack());
            p->bytes_ = std::move(bytes);
            const std::vector<uint8_t> &b = p->bytes_;
            auto le32 = [&b](size_t at) { return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24); };
            if (b.size() < 96 + 32 || std::memcmp(b.data(), "GT4XCAR1", 8) != 0)
            {
                why = "not a GT4 extra-cars pack";
                return nullptr;
            }
            const size_t body = b.size() - 32;
            if (sha256(b.data(), body) != *reinterpret_cast<const Digest *>(b.data() + body))
            {
                why = "the pack is damaged (its SHA-256 does not match)";
                return nullptr;
            }
            if (le32(8) != 1)
            {
                why = "pack version " + std::to_string(le32(8)) + " (this GT4 reads version 1)";
                return nullptr;
            }
            const uint32_t count = le32(12);
            p->name_.assign(reinterpret_cast<const char *>(b.data() + 16), strnlen(reinterpret_cast<const char *>(b.data() + 16), 16));
            p->imageBytes_ = uint64_t(le32(32)) | (uint64_t(le32(36)) << 32);
            p->tocLba_ = le32(40);
            p->tocSectors_ = le32(44);
            std::memcpy(p->tocSha_.data(), b.data() + 48, 32);
            p->extLba_ = le32(80);
            p->extSectors_ = le32(84);
            const uint32_t recBytes = le32(88);
            size_t at = 96;
            if (uint64_t(at) + recBytes + uint64_t(p->tocSectors_) * kSector + uint64_t(p->extSectors_) * kSector != body)
            {
                why = "the pack's sizes do not add up";
                return nullptr;
            }
            const size_t recEnd = at + recBytes;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (at + 64 > recEnd)
                {
                    why = "the pack's file list is cut short";
                    return nullptr;
                }
                PackFile f;
                f.usLba = le32(at);
                f.usStored = le32(at + 4);
                std::memcpy(f.usSha.data(), b.data() + at + 8, 32);
                f.newLba = le32(at + 40);
                f.newStored = le32(at + 44);
                f.newReal = le32(at + 48);
                f.type = b[at + 52];
                const uint16_t n = uint16_t(b[at + 54] | (b[at + 55] << 8));
                at += 56;
                if (at + n > recEnd || (f.type != 1 && f.type != 2) || f.newLba < p->extLba_ ||
                    uint64_t(f.newLba - p->extLba_) * kSector + f.newStored > uint64_t(p->extSectors_) * kSector)
                {
                    why = "the pack's file list is damaged";
                    return nullptr;
                }
                f.path.assign(reinterpret_cast<const char *>(b.data() + at), n);
                at += n;
                p->files_.push_back(std::move(f));
            }
            if (at != recEnd || p->tocSectors_ == 0 || p->extLba_ == 0 || uint64_t(p->extLba_) * kSector != p->imageBytes_)
            {
                why = "the pack's layout is damaged";
                return nullptr;
            }
            p->toc_ = recEnd;
            p->ext_ = recEnd + size_t(p->tocSectors_) * kSector;
            return p;
        }

        static std::shared_ptr<const Pack> load(const std::filesystem::path &path, std::string &why)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                why = "no pack at " + path.string();
                return nullptr;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return parse(std::move(bytes), why);
        }

        // The checksum fallback: is this image the US disc the pack was built from? (its size, TOC sectors and every
        // replaced file's stored bytes, read through `read`).
        bool matches(uint64_t imageBytes, const Reader &read, std::string &why) const
        {
            if (imageBytes != imageBytes_)
            {
                why = "the disc image is " + std::to_string(imageBytes) + " bytes, not the US disc's " + std::to_string(imageBytes_);
                return false;
            }
            std::vector<uint8_t> buf(size_t(tocSectors_) * kSector);
            if (!read(uint64_t(tocLba_) * kSector, buf.data(), buf.size()) || sha256(buf.data(), buf.size()) != tocSha_)
            {
                why = "GT4.VOL's table of contents is not the US disc's (SHA-256 " + hexOf(sha256(buf.data(), buf.size()), 6) + "...)";
                return false;
            }
            for (const PackFile &f : files_)
            {
                buf.resize(f.usStored);
                if (!read(uint64_t(f.usLba) * kSector, buf.data(), buf.size()) || sha256(buf.data(), buf.size()) != f.usSha)
                {
                    why = f.path + " is not the US disc's";
                    return false;
                }
            }
            return true;
        }

        // The pack's bytes over [offset, offset + bytes): the TOC sectors, and the extension past the image's end.
        void apply(uint64_t offset, uint8_t *data, size_t bytes) const
        {
            const uint64_t end = offset + bytes;
            auto copy = [&](uint64_t base, uint64_t length, size_t from) {
                const uint64_t lo = std::max(offset, base), hi = std::min(end, base + length);
                if (lo < hi)
                    std::memcpy(data + (lo - offset), bytes_.data() + from + (lo - base), size_t(hi - lo));
            };
            copy(uint64_t(tocLba_) * kSector, uint64_t(tocSectors_) * kSector, toc_);
            copy(uint64_t(extLba_) * kSector, uint64_t(extSectors_) * kSector, ext_);
        }

        const std::string &name() const { return name_; }
        const std::vector<PackFile> &files() const { return files_; }
        uint64_t imageBytes() const { return imageBytes_; }
        uint64_t extEnd() const { return uint64_t(extLba_ + extSectors_) * kSector; } // the served disc's end
        uint32_t extLba() const { return extLba_; }
        uint32_t extSectors() const { return extSectors_; }
        uint32_t tocLba() const { return tocLba_; }
        uint32_t tocSectors() const { return tocSectors_; }
        Digest digest() const { return sha256(bytes_.data(), bytes_.size()); }

    private:
        Pack() = default;
        std::vector<uint8_t> bytes_;
        std::string name_;
        uint64_t imageBytes_ = 0;
        uint32_t tocLba_ = 0, tocSectors_ = 0, extLba_ = 0, extSectors_ = 0;
        Digest tocSha_{};
        size_t toc_ = 0, ext_ = 0;
        std::vector<PackFile> files_;
    };

    // A comma (or space) list of car names; "esso" is the one pack there is; off / 0 / none name nothing. Returns the known
    // names listed (lower case); `unknown` gets the rest. (GT4_EXTRA_CARS itself is read by wantedNames below.)
    inline std::vector<std::string> parseNames(const char *text, std::vector<std::string> *unknown = nullptr)
    {
        std::vector<std::string> out;
        std::string word;
        auto take = [&] {
            if (word.empty() || word == "off" || word == "0" || word == "none")
            {
                word.clear();
                return;
            }
            if (word == "esso")
            {
                if (std::find(out.begin(), out.end(), word) == out.end())
                    out.push_back(word);
            }
            else if (unknown)
                unknown->push_back(word);
            word.clear();
        };
        for (const char *c = text ? text : ""; *c; ++c)
        {
            if (*c == ',' || *c == ' ' || *c == ';' || *c == '\t')
                take();
            else
                word += char(std::tolower(static_cast<unsigned char>(*c)));
        }
        take();
        return out;
    }

    // The extra cars there are (one pack each). With no GT4_EXTRA_CARS (developer 2026-10-14: no switch) every one of them whose
    // pack sits in extra-cars\ is served; GT4_EXTRA_CARS=off (or 0 / none) is the hidden way to turn them all off.
    inline const std::vector<std::string> &knownNames()
    {
        static const std::vector<std::string> names{"esso"};
        return names;
    }

    // GT4_EXTRA_CARS read the no-switch way: absent or empty = every known car (served when its pack is there); only
    // off / 0 / none = none; otherwise the known names it lists (unknown words go to `unknown`; a value naming no known car
    // and not off counts as absent).
    inline std::vector<std::string> wantedNames(const char *text, std::vector<std::string> *unknown = nullptr)
    {
        std::vector<std::string> bad;
        std::vector<std::string> out = parseNames(text, &bad);
        if (unknown)
            unknown->insert(unknown->end(), bad.begin(), bad.end());
        if (!out.empty())
            return out;
        bool anyWord = false;
        for (const char *c = text ? text : ""; *c; ++c)
            if (!std::isspace(static_cast<unsigned char>(*c)) && *c != ',' && *c != ';')
                anyWord = true;
        if (anyWord && bad.empty())
            return {}; // off / 0 / none
        return knownNames();
    }

    // The pack file for a name, in a GT4 folder: <folder>/extra-cars/<name>.gt4x.
    inline std::filesystem::path packPath(const std::filesystem::path &gt4Folder, const std::string &name)
    {
        return gt4Folder / "extra-cars" / (name + ".gt4x");
    }
}
