// GT4 Recompilation launcher: names for the Save details view, read from the GT4 disc image
// (gt4stock_names.h). Read only; nothing of the game is stored in the launcher.

#include "gt4stock_names.h"

#include "gt4save.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace gt4stock
{
    namespace
    {
        const char *const kSpecDir = "specdb/GT4_US2560/";
        const char *const kRText = "projects/gtmode/US/rtext.rt2";
        const char *const kTuning = "GTmodeProject::TuningApplyRoot";

        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        // A NUL-terminated string at p (empty when p is outside the file or has no end).
        bool cstrAt(const std::vector<uint8_t> &d, size_t p, std::string &out)
        {
            if (p >= d.size())
                return false;
            const void *end = std::memchr(d.data() + p, 0, d.size() - p);
            if (!end)
                return false;
            out.assign(reinterpret_cast<const char *>(d.data() + p), static_cast<const uint8_t *>(end) - (d.data() + p));
            return true;
        }

        // The parts GT4's tune shop names, by level (GT4's own lists in projects/gtmode/gtmode.adc,
        // e.g. SUSPENSION_list = NORMAL_SUSPENSION, SPORTS_SUSPENSION, ...; level 0 is the
        // standard part, which the shop does not name). The level is a byte of the part's row
        // in its spec table (the offset below; -1: the row's last byte, the tyres).
        struct PartTable
        {
            uint32_t table;
            int levelOffset;
            std::vector<const char *> keys; // rtext keys in GTmodeProject::TuningApplyRoot, by level
        };
        const std::vector<PartTable> &partTables()
        {
            static const std::vector<PartTable> t = {
                {1, 2, {nullptr, "RACING_BRAKE"}},
                {2, 2, {nullptr, "BRAKECONTROLLER"}},
                {3, 14, {nullptr, "SPORTS_SUSPENSION", "SEMIRACING_SUSPENSION", "RACING_SUSPENSION", "ORIGINAL_SUSPENSION"}},
                {8, 4, {nullptr, "LIGHT_WEIGHT_STAGE1", "LIGHT_WEIGHT_STAGE2", "LIGHT_WEIGHT_STAGE3"}},
                {10, 14, {nullptr, "VALIABLE_CENTER_DEF_DRIVETRAIN"}},
                {11, 34, {nullptr, "CLOSE_GEAR", "SUPER_CLOSE_GEAR", "VALIABLE_GEAR"}},
                {13, 8, {nullptr, "NA_TUNE_STAGE1", "NA_TUNE_STAGE2", "NA_TUNE_STAGE3"}},
                {14, 8, {nullptr, "TURBINE_KIT_STAGE1", "TURBINE_KIT_STAGE2", "TURBINE_KIT_STAGE3", "TURBINE_KIT_STAGE4", "TURBINE_KIT_STAGE5"}},
                {15, 6, {nullptr, "PORTPOLISH"}},
                {16, 8, {nullptr, "ENGINEBALANCE"}},
                {17, 6, {nullptr, "DISPLACEMENT"}},
                {18, 6, {nullptr, "COMPUTER"}},
                {19, 6, {nullptr, "INTERCOOLER_SPORTS", "INTERCOOLER_RACING_BIG"}},
                {20, 6, {nullptr, "SPORTS_MUFFLER", "SEMI_RACING_MUFFLER", "RACING_MUFFLER"}},
                {21, 2, {nullptr, "HIGH_CAPACITY_CLUTCH", "TWIN_CLUTCH", "TRIPLE_CLUTCH"}},
                {22, 2, {nullptr, "FLYWHEEL_LIGHT", "FLYWHEEL_Cr_Mo", "FLYWHEEL_LIGHT_Cr_Mo"}},
                {23, 2, {nullptr, "PROPELLERSHAFT"}},
                {24, 2, {nullptr, "1WAY_LSD", "15WAY_LSD", "2WAY_LSD", "VARIABLE_LSD", "AYCC_LSD"}},
                {25, -1, {"FRONTTIRE_NORMAL_ECONOMY", "FRONTTIRE_NORMAL_COMFORT", "FRONTTIRE_NORMAL_STREET", "FRONTTIRE_SPORTS_HARD",
                          "FRONTTIRE_SPORTS_MEDIUM", "FRONTTIRE_SPORTS_SOFT", "FRONTTIRE_RACING_SUPER_HARD", "FRONTTIRE_RACING_HARD",
                          "FRONTTIRE_RACING_MEDIUM", "FRONTTIRE_RACING_SOFT", "FRONTTIRE_RACING_QUALIFY", "FRONTTIRE_DIRT", "FRONTTIRE_SNOW"}},
                {26, -1, {"FRONTTIRE_NORMAL_ECONOMY", "FRONTTIRE_NORMAL_COMFORT", "FRONTTIRE_NORMAL_STREET", "FRONTTIRE_SPORTS_HARD",
                          "FRONTTIRE_SPORTS_MEDIUM", "FRONTTIRE_SPORTS_SOFT", "FRONTTIRE_RACING_SUPER_HARD", "FRONTTIRE_RACING_HARD",
                          "FRONTTIRE_RACING_MEDIUM", "FRONTTIRE_RACING_SOFT", "FRONTTIRE_RACING_QUALIFY", "FRONTTIRE_DIRT", "FRONTTIRE_SNOW"}},
                {27, 4, {nullptr, "NOS"}},
                {28, 6, {nullptr, "SUPERCHARGER"}},
            };
            return t;
        }

        // UTF-8 -> code points (a bad byte becomes U+FFFD).
        std::vector<uint32_t> codePoints(const std::string &s)
        {
            std::vector<uint32_t> out;
            for (size_t i = 0; i < s.size();)
            {
                const uint8_t c = static_cast<uint8_t>(s[i]);
                uint32_t cp = 0xFFFD;
                size_t n = 1;
                if (c < 0x80)
                    cp = c;
                else if ((c & 0xE0) == 0xC0)
                    n = 2, cp = c & 0x1F;
                else if ((c & 0xF0) == 0xE0)
                    n = 3, cp = c & 0x0F;
                else if ((c & 0xF8) == 0xF0)
                    n = 4, cp = c & 0x07;
                if (n > 1)
                {
                    if (i + n > s.size())
                    {
                        cp = 0xFFFD;
                        n = 1;
                    }
                    else
                        for (size_t k = 1; k < n; ++k)
                        {
                            const uint8_t cc = static_cast<uint8_t>(s[i + k]);
                            if ((cc & 0xC0) != 0x80)
                            {
                                cp = 0xFFFD;
                                n = k;
                                break;
                            }
                            cp = (cp << 6) | (cc & 0x3F);
                        }
                }
                out.push_back(cp);
                i += n;
            }
            return out;
        }

        void appendUtf8(std::string &out, uint32_t cp)
        {
            if (cp < 0x80)
                out += static_cast<char>(cp);
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }

    // ------------------------------------------------------------------ formats
    int GtdbTable::firstRow(uint32_t code) const
    {
        const auto it = std::lower_bound(codes.begin(), codes.end(), code);
        return it != codes.end() && *it == code ? static_cast<int>(it - codes.begin()) : -1;
    }

    bool decodeGtdb(const std::vector<uint8_t> &d, GtdbTable &t, std::string &error)
    {
        t = GtdbTable{};
        if (d.size() < 16 || std::memcmp(d.data(), "GTDB", 4) != 0)
        {
            error = "not a GT4 spec table";
            return false;
        }
        const uint16_t flags = le16(d.data() + 4);
        const uint32_t n = le32(d.data() + 8);
        t.rowSize = le32(d.data() + 12);
        if (t.rowSize == 0 || t.rowSize > 4096 || n > 1000000 || 0x10 + uint64_t(n) * 8 > d.size())
        {
            error = "the spec table's header is damaged";
            return false;
        }
        std::vector<uint32_t> offs(n);
        t.codes.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            t.codes[i] = le32(d.data() + 0x10 + 8 * size_t(i));
            offs[i] = le32(d.data() + 0x14 + 8 * size_t(i));
        }
        const size_t p = 0x10 + size_t(n) * 8;
        t.compressed = (flags & 1) != 0;
        t.rows.assign(n, {});
        if (!t.compressed)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                if (p + uint64_t(offs[i]) + t.rowSize > d.size())
                {
                    error = "a spec table row is past the end of the file";
                    return false;
                }
                t.rows[i].assign(d.begin() + std::ptrdiff_t(p + offs[i]), d.begin() + std::ptrdiff_t(p + offs[i] + t.rowSize));
            }
            return true;
        }
        // The Huffman code (0x449440 / 0x449510 / 0x449558): an 8-byte header (u32 size of the
        // block, u32 code count), a 256-entry lookup by the next 8 bits (symbol | length << 8;
        // 0 = a longer code), then every code {u8 length, u8 symbol, u16, u32 code bits}.
        if (p + 8 + 512 > d.size())
        {
            error = "the spec table's code is damaged";
            return false;
        }
        const uint32_t nrec = le32(d.data() + p + 4);
        const size_t look = p + 8, recs = look + 512;
        if (nrec > 256 || le32(d.data() + p) != 8 + 512 + 8 * nrec || recs + uint64_t(nrec) * 8 + 8 > d.size())
        {
            error = "the spec table's code is damaged";
            return false;
        }
        std::unordered_map<uint64_t, uint8_t> slow;
        for (uint32_t k = 0; k < nrec; ++k)
        {
            const uint8_t *r = d.data() + recs + 8 * size_t(k);
            if (r[0] >= 1 && r[0] <= 32)
                slow[(uint64_t(r[0]) << 32) | le32(r + 4)] = r[1];
        }
        // The dictionary of whole rows (u32 size of the block, u32 row count), then the coded rows.
        const size_t dh = recs + size_t(nrec) * 8;
        const uint32_t ndict = le32(d.data() + dh + 4);
        const size_t dict = dh + 8;
        if (ndict > 64 || le32(d.data() + dh) != 8 + uint64_t(ndict) * t.rowSize || dict + uint64_t(ndict) * t.rowSize > d.size())
        {
            error = "the spec table's dictionary is damaged";
            return false;
        }
        const size_t data = dict + size_t(ndict) * t.rowSize;
        std::vector<uint8_t> syms;
        for (uint32_t i = 0; i < n; ++i)
        {
            const size_t src = data + offs[i];
            if (src >= d.size())
            {
                error = "a spec table row is past the end of the file";
                return false;
            }
            const uint32_t count = d[src];
            syms.clear();
            uint64_t bit = 0;
            for (uint32_t s = 0; s < count; ++s)
            {
                // 32 bits from the byte holding the next bit, first bit lowest (bytes past
                // the end read as 0), as 0x449440 reads them.
                const size_t at = src + 1 + size_t(bit / 8);
                uint32_t w = 0;
                for (size_t b = 0; b < 4; ++b)
                    w |= uint32_t(at + b < d.size() ? d[at + b] : 0) << (8 * b);
                w >>= bit % 8;
                const uint16_t e = le16(d.data() + look + 2 * size_t(w & 0xFF));
                uint32_t len = e >> 8;
                uint8_t sym = static_cast<uint8_t>(e & 0xFF);
                if (!len)
                {
                    for (uint32_t L = 9; L <= 32 && !len; ++L)
                    {
                        const uint32_t mask = L == 32 ? 0xFFFFFFFFu : ((1u << L) - 1u);
                        const auto it = slow.find((uint64_t(L) << 32) | (w & mask));
                        if (it != slow.end())
                        {
                            sym = it->second;
                            len = L;
                        }
                    }
                    if (!len)
                    {
                        error = "a spec table row does not decode";
                        return false;
                    }
                }
                syms.push_back(sym);
                bit += len;
            }
            if (syms.empty())
            {
                error = "an empty spec table row";
                return false;
            }
            // 0x449600: the first symbol is mode << 6 | dictionary row.
            const uint32_t mode = syms[0] >> 6, idx = syms[0] & 63u;
            std::vector<uint8_t> &row = t.rows[i];
            if (mode == 1)
            {
                if (syms.size() < 1 + size_t(t.rowSize))
                {
                    error = "a spec table row is short";
                    return false;
                }
                row.assign(syms.begin() + 1, syms.begin() + 1 + t.rowSize);
            }
            else if (mode == 0 || mode == 2)
            {
                if (idx >= ndict)
                {
                    error = "a spec table row names a missing dictionary row";
                    return false;
                }
                row.assign(d.begin() + std::ptrdiff_t(dict + idx * t.rowSize), d.begin() + std::ptrdiff_t(dict + (idx + 1) * t.rowSize));
                if (mode == 2)
                {
                    // A bit per byte (first byte lowest), then the changed bytes in order.
                    const size_t mb = (t.rowSize + 7) / 8;
                    size_t lit = 1 + mb;
                    if (syms.size() < lit)
                    {
                        error = "a spec table row is short";
                        return false;
                    }
                    for (uint32_t k = 0; k < t.rowSize; ++k)
                        if ((syms[1 + k / 8] >> (k % 8)) & 1)
                        {
                            if (lit >= syms.size())
                            {
                                error = "a spec table row is short";
                                return false;
                            }
                            row[k] = syms[lit++];
                        }
                }
            }
            // mode 3: GT4 leaves the row unwritten; it stays empty here.
        }
        return true;
    }

    bool decodeStrDb(const std::vector<uint8_t> &d, std::vector<std::string> &out, std::string &error)
    {
        out.clear();
        if (d.size() < 16 || std::memcmp(d.data(), "GTST", 4) != 0)
        {
            error = "not a GT4 string table";
            return false;
        }
        const uint32_t n = le32(d.data() + 4);
        const uint64_t base = 16 + uint64_t(n) * 4;
        if (n > 1000000 || base > d.size())
        {
            error = "the string table's header is damaged";
            return false;
        }
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint64_t p = base + le32(d.data() + 16 + 4 * size_t(i));
            if (p + 2 > d.size())
                continue;
            const uint16_t ln = le16(d.data() + p);
            const uint64_t end = std::min<uint64_t>(p + 2 + ln, d.size());
            std::string s(reinterpret_cast<const char *>(d.data() + p + 2), static_cast<size_t>(end - (p + 2)));
            const size_t z = s.find('\0');
            out[i] = z == std::string::npos ? s : s.substr(0, z);
        }
        return true;
    }

    bool decodeRText(const std::vector<uint8_t> &d, RText &out, std::string &error)
    {
        out.clear();
        if (d.size() < 16 || std::memcmp(d.data(), "RT03", 4) != 0)
        {
            error = "not GT4's text file";
            return false;
        }
        const uint32_t ncat = le32(d.data() + 12);
        if (ncat > 4096 || 16 + uint64_t(ncat) * 16 > d.size())
        {
            error = "GT4's text file is damaged";
            return false;
        }
        for (uint32_t c = 0; c < ncat; ++c)
        {
            const uint8_t *h = d.data() + 16 + 16 * size_t(c);
            std::string name;
            if (!cstrAt(d, le32(h), name))
                continue;
            const uint32_t count = le32(h + 4), ptrs = le32(h + 8);
            if (count > 100000 || ptrs + uint64_t(count) * 8 > d.size())
                continue;
            std::map<std::string, std::string> &cat = out[name];
            for (uint32_t k = 0; k < count; ++k)
            {
                std::string key, value;
                if (cstrAt(d, le32(d.data() + ptrs + 8 * size_t(k)), key) && cstrAt(d, le32(d.data() + ptrs + 8 * size_t(k) + 4), value))
                    cat[key] = value;
            }
        }
        return true;
    }

    std::string displayText(const std::string &utf8)
    {
        std::string s;
        for (uint32_t cp : codePoints(utf8))
        {
            if (cp == '`')
                cp = '\'';
            else if (cp >= 0xFF01 && cp <= 0xFF5E)
                cp -= 0xFEE0; // full-width letters
            else if (cp == 0xFF65)
                cp = 0xB7; // half-width middle dot
            else if (cp == '\n' || cp == '\r' || cp == '\t')
                cp = ' ';
            if (cp >= 0x2160 && cp <= 0x2162) // Roman numerals I, II, III
            {
                s.append(cp - 0x2160 + 1, 'I');
                continue;
            }
            const bool latin = (cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF);
            const bool marks = cp == 0x2013 || cp == 0x2014 || cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D || cp == 0x2022 ||
                               cp == 0x2026;
            if (!latin && !marks)
                cp = ' '; // a symbol the launcher's font does not have (e.g. GT4's star)
            appendUtf8(s, cp);
        }
        // Spaces closed up; the year apart ("Ltd.'97" -> "Ltd. '97").
        std::string t;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == ' ' && (t.empty() || t.back() == ' '))
                continue;
            if (c == '\'' && !t.empty() && t.back() != ' ' && i + 2 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isdigit(static_cast<unsigned char>(s[i + 2])) && (i + 3 == s.size() || s[i + 3] == ' '))
                t += ' ';
            t += c;
        }
        while (!t.empty() && t.back() == ' ')
            t.pop_back();
        return t;
    }

    // ------------------------------------------------------------------ the names
    GameNames::~GameNames() = default;

    std::shared_ptr<GameNames> GameNames::load(const fs::path &iso, std::string &error)
    {
        std::unique_ptr<gt4save::Gt4Disc> disc = gt4save::Gt4Disc::open(iso, error);
        if (!disc)
            return nullptr;
        std::unique_ptr<gt4save::SpecDb> spec = gt4save::SpecDb::loadFromIso(iso, error);
        if (!spec || spec->labels.empty())
        {
            if (error.empty())
                error = "the disc's spec database could not be read";
            return nullptr;
        }
        auto names = std::make_shared<GameNames>();
        names->source = pathText(iso);
        names->spec = std::move(spec);
        std::string err;
        std::vector<uint8_t> file;
        auto get = [&](const std::string &path) { return disc->read(path, file, err); };
        const std::string dir = kSpecDir;

        if (get(dir + "american_StrDB.sdb"))
            decodeStrDb(file, names->strings_, err);
        GtdbTable t;
        if (get(dir + "CAR_NAME_american.dbt") && decodeGtdb(file, t, err))
            for (size_t i = 0; i < t.codes.size(); ++i)
                if (t.rows[i].size() >= 12)
                    names->carNames_[t.codes[i]] = {le32(t.rows[i].data()), le32(t.rows[i].data() + 4), le32(t.rows[i].data() + 8)};
        if (get(dir + "CAR_VARIATION_american.dbt") && decodeGtdb(file, t, err))
            for (size_t i = 0; i < t.codes.size(); ++i)
                if (t.rows[i].size() >= 8 && le32(t.rows[i].data() + 4) == 42) // a VARIATION spec code
                    names->carVariation_[t.codes[i]] = le32(t.rows[i].data());
        if (get(dir + "VARIATIONamerican.dbt") && decodeGtdb(file, t, err))
            for (size_t i = 0; i < t.codes.size(); ++i)
            {
                const std::vector<uint8_t> &r = t.rows[i];
                if (r.size() < 40)
                    continue;
                CarColour c;
                c.number = static_cast<int>(le32(r.data() + 4));
                c.name = names->str(le32(r.data() + 12));
                c.r = r[36];
                c.g = r[37];
                c.b = r[38];
                names->colours_[t.codes[i]].push_back(std::move(c));
            }
        for (const PartTable &pt : partTables())
        {
            const char *name = gt4save::tableName(pt.table);
            if (!name || !get(dir + name + ".dbt") || !decodeGtdb(file, t, err))
                continue;
            for (size_t i = 0; i < t.codes.size(); ++i)
            {
                const std::vector<uint8_t> &r = t.rows[i];
                const int off = pt.levelOffset < 0 ? static_cast<int>(r.size()) - 1 : pt.levelOffset;
                if (off >= 0 && static_cast<size_t>(off) < r.size())
                    names->partLevels_[(uint64_t(pt.table) << 32) | t.codes[i]] = r[static_cast<size_t>(off)];
            }
        }
        if (get(kRText))
            decodeRText(file, names->rtext_, err);
        if (names->carNames_.empty())
            error = "the disc's car names could not be read" + (err.empty() ? std::string() : ": " + err);
        return names;
    }

    std::string GameNames::str(uint32_t id) const { return id < strings_.size() ? displayText(strings_[id]) : std::string(); }

    std::string GameNames::carName(uint64_t code64) const
    {
        if ((code64 >> 32) != 0)
            return std::string();
        const auto it = carNames_.find(static_cast<uint32_t>(code64));
        if (it == carNames_.end())
            return std::string();
        const std::string s = str(it->second[1]);
        return s.empty() ? str(it->second[0]) : s;
    }

    std::string GameNames::carFullName(uint64_t code64) const
    {
        if ((code64 >> 32) != 0)
            return std::string();
        const auto it = carNames_.find(static_cast<uint32_t>(code64));
        return it == carNames_.end() ? std::string() : str(it->second[0]);
    }

    std::optional<CarColour> GameNames::colour(uint64_t carCode64, int colourIndex, std::optional<uint64_t> variation64) const
    {
        uint32_t var = 0;
        if (variation64 && (*variation64 >> 32) == 42)
            var = static_cast<uint32_t>(*variation64);
        else
        {
            const auto it = carVariation_.find(static_cast<uint32_t>(carCode64));
            if ((carCode64 >> 32) != 0 || it == carVariation_.end())
                return std::nullopt;
            var = it->second;
        }
        const auto g = colours_.find(var);
        if (g == colours_.end())
            return std::nullopt;
        for (const CarColour &c : g->second)
            if (c.number == colourIndex + 1) // CORE.elf 0x4458d0: the row whose number is index + 1
                return c;
        return std::nullopt;
    }

    int GameNames::colourCount(uint64_t carCode64) const
    {
        const auto it = carVariation_.find(static_cast<uint32_t>(carCode64));
        if ((carCode64 >> 32) != 0 || it == carVariation_.end())
            return 0;
        const auto g = colours_.find(it->second);
        return g == colours_.end() ? 0 : static_cast<int>(g->second.size());
    }

    std::string GameNames::text(const std::string &category, const std::string &key) const
    {
        const auto c = rtext_.find(category);
        if (c == rtext_.end())
            return std::string();
        const auto it = c->second.find(key);
        return it == c->second.end() ? std::string() : displayText(trim(it->second));
    }

    std::string GameNames::eventName(const std::string &series) const { return text("EventName", series); }

    std::string GameNames::licenceText(const std::string &key) const { return text("LicenseName", key); }

    int GameNames::partLevel(uint64_t code64) const
    {
        const auto it = partLevels_.find(code64);
        return it == partLevels_.end() ? -1 : it->second;
    }

    std::string GameNames::partName(uint64_t code64) const
    {
        const uint32_t table = static_cast<uint32_t>(code64 >> 32);
        if (table == 29)
            return wheelName(code64);
        for (const PartTable &pt : partTables())
        {
            if (pt.table != table)
                continue;
            const int level = partLevel(code64);
            if (level < 0 || static_cast<size_t>(level) >= pt.keys.size() || !pt.keys[static_cast<size_t>(level)])
                return std::string();
            return text(kTuning, pt.keys[static_cast<size_t>(level)]);
        }
        return std::string();
    }

    std::string GameNames::wheelName(uint64_t code64) const
    {
        if (!spec || (code64 >> 32) != 29)
            return std::string();
        const std::optional<std::string> label = spec->label(code64);
        return label ? text("WheelName", *label) : std::string();
    }
}
