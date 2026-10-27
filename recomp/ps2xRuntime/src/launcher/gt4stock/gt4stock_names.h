#pragma once

// GT4 Recompilation launcher: the names the Save details view shows, read from the player's
// own GT4 disc image at run time (nothing of the game is compiled in):
//   - the spec database labels (gt4save::SpecDb: cars, parts, races);
//   - specdb/GT4_US2560/CAR_NAME_american.dbt: each car's name ("Toyota SUPRA RZ `97"), as
//     string ids into american_StrDB.sdb;
//   - CAR_VARIATION_american.dbt: each car's colour group, and VARIATIONamerican.dbt: the
//     colours of a group (name in american_StrDB.sdb, RGB); a car's colour index i is the row
//     whose colour number is i + 1 (CORE.elf 0x4458d0);
//   - the parts tables' level byte, and projects/gtmode/US/rtext.rt2 (GT4's own UI text):
//     event, licence, mission, part and wheel names.
// The .dbt rows are GT4's compressed table format (CORE.elf 0x4492d8 -> 0x449440 Huffman
// symbols -> 0x449600 dictionary row + changed bytes): decodeGtdb below.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gt4save
{
    class SpecDb;
}

namespace gt4stock
{
    namespace fs = std::filesystem;

    // ---- the disc's formats (exposed for the tests)
    struct GtdbTable
    {
        uint32_t rowSize = 0;
        bool compressed = false;
        std::vector<uint32_t> codes;            // row order (sorted by code; a code can repeat)
        std::vector<std::vector<uint8_t>> rows; // decoded, rowSize bytes each (empty: a row GT4 leaves unwritten)
        // The first row with this code (CORE.elf 0x449210), -1 when none.
        int firstRow(uint32_t code) const;
    };
    bool decodeGtdb(const std::vector<uint8_t> &file, GtdbTable &out, std::string &error);

    // "GTST" string tables (american_StrDB.sdb): u16 length + UTF-8 text.
    bool decodeStrDb(const std::vector<uint8_t> &file, std::vector<std::string> &out, std::string &error);

    // "RT03" UI text (rtext.rt2): categories of key -> text.
    using RText = std::map<std::string, std::map<std::string, std::string>>;
    bool decodeRText(const std::vector<uint8_t> &file, RText &out, std::string &error);

    // GT4's text as the launcher's font draws it: "`97" -> "'97" (with a space before the
    // year), runs of spaces closed up, full-width letters and a few symbols the font lacks
    // replaced, line breaks to spaces.
    std::string displayText(const std::string &utf8);

    struct CarColour
    {
        std::string name; // "Silver Metallic"
        uint8_t r = 0, g = 0, b = 0;
        int number = 0;   // GT4's colour number (index + 1)
    };

    class GameNames
    {
    public:
        // Everything from the GT4 (USA) disc image, read only. About half a second: the
        // launcher calls it once, on a worker thread.
        static std::shared_ptr<GameNames> load(const fs::path &iso, std::string &error);
        ~GameNames();

        std::string source;                    // the disc image
        std::shared_ptr<gt4save::SpecDb> spec; // labels (cars, parts, races, wheels)

        // A car (spec code, table 0): the list name (CAR_NAME's second name, as GT4's garage
        // lists it) and the full name (the first). Empty when the disc has none.
        std::string carName(uint64_t code64) const;
        std::string carFullName(uint64_t code64) const;
        // The car's colour: the group from the car's own record when known (variation64),
        // else the car's standard group (CAR_VARIATION).
        std::optional<CarColour> colour(uint64_t carCode64, int colourIndex, std::optional<uint64_t> variation64 = std::nullopt) const;
        int colourCount(uint64_t carCode64) const; // colours of the car's standard group
        // An event series ("am_sunday" -> "Sunday Cup"); empty when the disc has no name.
        std::string eventName(const std::string &series) const;
        // A licence test / coffee break / mission name from GT4's text ("l0b0001", "l0m0003").
        std::string licenceText(const std::string &key) const;
        // A fitted part (spec code with its table in the high word) as GT4's tune shop
        // names it ("Suspension: Full Customize", "Sports Tires: Soft (S3)"); empty for a
        // standard part, or a part GT4 does not name.
        std::string partName(uint64_t code64) const;
        int partLevel(uint64_t code64) const; // -1 unknown
        std::string wheelName(uint64_t code64) const; // a custom wheel ("BBS 001")
        std::string text(const std::string &category, const std::string &key) const;

        // For the report and the tests.
        size_t carNameCount() const { return carNames_.size(); }
        size_t colourGroupCount() const { return colours_.size(); }
        size_t stringCount() const { return strings_.size(); }
        size_t textCategoryCount() const { return rtext_.size(); }
        size_t partCount() const { return partLevels_.size(); }

    private:
        std::vector<std::string> strings_;                    // american_StrDB.sdb
        std::map<uint32_t, std::array<uint32_t, 3>> carNames_; // car code -> string ids
        std::map<uint32_t, uint32_t> carVariation_;            // car code -> VARIATION code
        std::map<uint32_t, std::vector<CarColour>> colours_;   // VARIATION code -> colours (dbt order)
        std::map<uint64_t, uint8_t> partLevels_;               // part spec code -> level
        RText rtext_;
        std::string str(uint32_t id) const;
    };
}
