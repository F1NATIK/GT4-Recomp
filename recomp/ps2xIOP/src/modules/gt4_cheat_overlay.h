#pragma once

// GT4_CHEATS (analysis/cheats/CHEATS-TAB-DESIGN.md 4 and 5, analysis/cheats/mission-fix/MISSION-FIX.md 4):
// two of GT4's compiled menu scripts served patched, in memory, as GT4 reads them from the disc.
// Nothing is written to the disc image or to a save.
//   noday       projects/gtmode/gtmode.adc: craft()'s day test getElapsedDate() > 365 becomes > -1,
//               so GT4's five button codes work from Day 1 (GT4-CHEATS.md 3, 7.1).
//   missionfix  projects/gtmode/gtmode.adc: MissionRoot::doCraft (the Mission Hall code, which gives
//               10,000,000 credits on the disc) passes the highlighted mission instead, step for step
//               like GT4's own event-race code ("parity", MISSION-FIX.md 2.1: our design, not Polyphony's).
//               The file keeps its size: 45 later frames' unused source-path strings get shorter.
//   secret      projects/option/option.adc: OptionRoot.onKeyPress's "if (event.port == 0) return 0"
//               becomes port == -1, so controller 1 can type the hidden Options code (GT4-CHEATS.md 4).
// And one change that is not a GT4_CHEATS word (analysis/design/480p-colour/DESIGN.md, "GT4's Video Output
// line"): with GT4_RACE_MODE=480p GT4.exe runs every race in 480p and says so (setVideoOutputHidden, once
// CORE is loaded and its words are checked, before GT4 runs):
//   video-output projects/option/option.adc: ModeType::onInitialize's "Pane::HiVision.visible = 1" (shown
//               on an NTSC console) becomes 0, so Options > Screen Settings (also from inside a race) has no
//               Video Output line, as GT4's own layout has it and a PAL GT4 shows the page.
// One overlay entry per file: the splices of every switch that is on and touches the file are applied
// together. Each file is checked first (the stored stream inflates to the known size and SHA-256, every
// splice's old bytes match, the result has the published SHA-256); then the patched file is deflated
// again and padded with empty deflate blocks to exactly the stream's stored size in GT4.VOL, so the
// sectors GT4 reads keep their layout. The bytes after the stored size in the last sector are the
// disc's own. A failed check is logged once; then the requested switches without missionfix are tried
// (never one that was not asked for), and when nothing fits the file is served unchanged. What was
// served is published for GT4.exe's in-game card (ps2x/iop/gt4_cheats_served.h).

#include "ps2x/iop/gt4_cheats_served.h"

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::gt4::cheats
{
    using Digest = std::array<uint8_t, 32>;

    struct Switches
    {
        bool noday = false;
        bool secret = false;
        bool missionfix = false;
        int32_t nodayValue = -1; // GT4_CHEATS_TEST_NODAY (tests only): the compare value
        std::vector<std::string> unknown;
        // Not a GT4_CHEATS word: GT4.exe's GT4_RACE_MODE=480p (setVideoOutputHidden).
        bool hideVideoOutput = false;
        bool cheats() const { return noday || secret || missionfix; }
        bool any() const { return cheats() || hideVideoOutput; }
    };
    // GT4_CHEATS: a comma (or space) list; absent or empty = off. Case is ignored.
    Switches parseSwitches(std::string_view text);
    // GT4_CHEATS, and hideVideoOutput as GT4.exe set it (setVideoOutputHidden).
    Switches switchesFromEnvironment();
    // What GT4.exe said about the Video Output line: -1 nothing yet, 0 shown, 1 hidden.
    int videoOutputDecision();
    // A read of [offset, offset + bytes) touches a script file the overlay can patch (gtmode.adc, option.adc).
    bool touchesScriptFiles(uint64_t offset, size_t bytes);
    // The switches as ServedBit flags (ps2x/iop/gt4_cheats_served.h).
    uint8_t switchBits(const Switches &switches);
    // Sets what servedState() returns (known = true): Disc::overlayCheats, once the overlay is built.
    void publishServed(uint8_t asked, uint8_t served);

    // One compiled script inside GT4.VOL: where its stored stream is on the disc (absolute sector
    // of the 8-byte C5 EE F7 FF header), its stored and inflated sizes, its SHA-256 on the USA disc.
    struct ScriptFile
    {
        const char *name = "";    // for the log
        uint32_t lba = 0;
        uint32_t storedBytes = 0; // header + raw deflate, as in GT4.VOL's table
        uint32_t size = 0;        // inflated
        Digest sha256{};
        uint64_t discOffset() const { return uint64_t(lba) * 2048u; }
        uint64_t discEnd() const { return discOffset() + storedBytes; }
        uint32_t sectors() const { return (storedBytes + 2047u) / 2048u; }
    };
    // The USA disc (SCUS-97328) facts, GT4-CHEATS.md 5.3 [checked on the disc].
    ScriptFile gtmodeFile();
    ScriptFile optionFile();

    // One change in the inflated file: oldLength bytes at offset (in the STOCK file) become newBytes.
    // The old bytes are checked as given (oldBytes) or by their SHA-256 (oldSha256).
    struct Splice
    {
        uint32_t offset = 0;
        uint32_t oldLength = 0;
        std::vector<uint8_t> oldBytes;
        std::optional<Digest> oldSha256;
        std::vector<uint8_t> newBytes;
    };
    // The splices one switch makes in one file.
    struct SwitchPatch
    {
        std::string switchName; // "noday"
        std::string summary;    // for the log: "0x3b501 365 -> -1"
        std::vector<Splice> splices;
    };
    SwitchPatch nodayPatch(int32_t value = -1);
    SwitchPatch missionfixPatch();
    SwitchPatch secretPatch();
    SwitchPatch videoOutputPatch();

    // One way to patch a file: the switches' splices together, and the SHA-256 the result must have
    // (MISSION-FIX.md 2.4; none for a test value, then only the stock and old-byte checks run).
    struct Attempt
    {
        std::vector<SwitchPatch> parts;
        std::optional<Digest> resultSha256;
        std::string switchNames() const; // "noday, missionfix"
        std::string summary() const;     // the parts' summaries, "; " between
    };
    // One overlay entry: a file and what to try, in order (everything asked for first, then the
    // fallbacks within it).
    struct FilePatch
    {
        ScriptFile file;
        std::vector<Attempt> attempts;
    };

    using Sha256 = std::function<bool(std::span<const uint8_t>, Digest &)>;

    // Applies one attempt's splices to the stock file (checked: in range, not overlapping, old bytes,
    // the same size as before, the result's SHA-256 when one is given).
    bool applySplices(std::span<const uint8_t> file, const Attempt &attempt, const Sha256 &sha, std::vector<uint8_t> &out, std::string &why);

    struct Built
    {
        std::vector<uint8_t> stream; // header + patched stream, exactly storedBytes long
        size_t attempt = 0;          // which attempt was used
        Digest result{};             // SHA-256 of the patched file
        size_t deflated = 0, padding = 0;
        std::vector<std::string> failures; // why earlier attempts failed, in order
    };
    // The replacement for `stored` (the disc's storedBytes bytes of the file). False with `why` set
    // when the file itself fails a check, or when no attempt fits (then `built.failures` has each).
    bool buildReplacement(const FilePatch &patch, std::span<const uint8_t> stored, const Sha256 &sha, Built &built, std::string &why);

    // Raw deflate (RFC 1951) helpers, used by buildReplacement and the tests.
    // deflateRaw: dynamic / fixed / stored blocks as smallest, lazy matching (zlib level 9 rules);
    // every block non-final, ending byte aligned (an empty stored block, like a sync flush).
    std::vector<uint8_t> deflateRaw(std::span<const uint8_t> data);
    // Empty deflate blocks, starting byte aligned, ending with a final block: exactly `bytes`
    // long, or empty when no such padding exists (bytes 0, 1 and 6).
    std::vector<uint8_t> emptyBlocks(size_t bytes);
    // Strict inflate: false on any error, a stream end past `stream`, or output over `limit`.
    // `consumed` is the bytes up to the end of the final block.
    bool inflateRaw(std::span<const uint8_t> stream, size_t limit, std::vector<uint8_t> &out, size_t &consumed);

    // The overlay one disc image gets. start() reads the stored streams of the files through `read`
    // (called once, by the caller's thread) and builds the replacements on a worker thread; apply()
    // waits for them only when a read touches one of the files. Two entries whose sectors overlap
    // are refused (logged once, both served unchanged), so one can never silently overwrite another.
    class Overlay
    {
    public:
        using Reader = std::function<bool(uint64_t offset, void *destination, size_t bytes)>;
        using Log = std::function<void(bool error, const std::string &message)>;

        Overlay(Switches switches, std::vector<FilePatch> patches, Sha256 sha, Log log);
        ~Overlay();
        Overlay(const Overlay &) = delete;
        Overlay &operator=(const Overlay &) = delete;

        bool enabled() const { return !patches_.empty(); }
        // Called once on the build thread when every entry is built (or failed), with the ServedBit
        // flags of the switches actually served. Set before start().
        void onBuilt(std::function<void(uint8_t served)> done) { onBuilt_ = std::move(done); }
        void start(const Reader &read);
        // Replaces the bytes of [offset, offset + bytes) that lie in a patched file's stored stream.
        // `path` names the disc path for the one-time "served" line ("sector read", "byte read").
        void apply(uint64_t offset, uint8_t *data, size_t bytes, const char *path);
        // Tests: wait for the build; how many files are served patched; the switches each file got;
        // the ServedBit flags of everything served.
        size_t ready();
        std::vector<std::string> servedSwitches();
        uint8_t servedBits();

    private:
        struct Entry
        {
            FilePatch patch;
            std::vector<uint8_t> stored;   // the disc's bytes (start)
            std::vector<uint8_t> replaced; // the patched stream (empty: serve the disc's)
            std::string switches;          // the attempt used
            uint8_t bits = 0;              // its switches as ServedBit flags
            bool served = false;
        };
        void build();

        std::vector<Entry> entries_;
        std::vector<FilePatch> patches_;
        Sha256 sha_;
        Log log_;
        std::function<void(uint8_t)> onBuilt_;
        std::once_flag started_;
        std::shared_future<void> built_;
        std::mutex servedMutex_;
    };

    // The overlay entries the switches select, one per file, in order (gtmode, option).
    std::vector<FilePatch> patchesFor(const Switches &switches);
    // One line naming the GT4_CHEATS switches and what they do, for the start-up log.
    std::string describe(const Switches &switches);
    // "8d07e652...3bc1"
    std::string shortHex(const Digest &digest);
}
