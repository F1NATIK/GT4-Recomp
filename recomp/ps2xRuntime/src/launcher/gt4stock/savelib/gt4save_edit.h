#pragma once

// gt4save_edit - the save writer of the Full edition's Garage (oil change, car wash, Increase Rigidity removal, adding a
// car at Car Dealers). From the save-format prototype (analysis/design/full-launcher/savefmt, SAVE-FORMAT.md), proven
// there and again by the Full edition's own tests: every change below was loaded by GT4 from a changed copy, shown, and
// kept when GT4 wrote the card itself.
//
// Builds on the launcher's read-only save library (gt4save.h) and the round-trip re-encryption (gt4save_roundtrip.h),
// which rebuild GT4's files byte for byte. Every edit changes the decrypted bytes the library keeps (Save::dec), then
// writeFolder() re-encrypts with GT4's own seeds, key, counters and nonces kept, so only the edited fields and their
// checksums change.
//
// The rules the caller keeps (gt4full_saveedit.cpp does, SAVE-FORMAT.md section 9.2): GT4 is closed; the card's save
// folder is backed up first; the player confirmed the change; the edited copy is written to a new folder and checked with
// verify() before it replaces the card's files; a save that fails GT4's load checks, or holds the recomp's appended
// copies, is refused. writeFolder() never writes into a memory card folder (mc0 / mc1) itself.
//
// Linked by the Full edition only: the Classic edition never changes a save.

#include "gt4save.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gt4save::edit
{
    namespace fs = std::filesystem;

    // ---- where things are (decrypted GAMEDATA payload offsets; car record offsets) [C, SAVE-FORMAT.md]
    constexpr size_t kInnerCrcOffset = 0x08;     // inner header CRC-32 of the body
    constexpr size_t kBodyOffset = 0x10;         // the body the inner CRC covers
    constexpr size_t kBodyBytes = 237680;        // fixed (Status::size() - 16)
    constexpr size_t kCalendarCount = 0x230;     // u32, then 2,922 x 16-byte entries
    constexpr size_t kCalendarEntries = 0x234;
    constexpr uint32_t kCalendarMax = 2922;
    constexpr size_t kToday = 0xB8D4;            // u32 day number (day 0 = 2,453,462)
    constexpr size_t kGarageIndex = 0xB8E0;      // 1,000 x 32 bytes
    constexpr size_t kRidingSlot = 0x135E0;      // s32
    constexpr size_t kRidingCar = 0x135FC;       // 1,208-byte car record (the car being driven)
    constexpr size_t kAvailableCars = 0x361C0;   // 1,024 x u64 car codes, sorted, -1 padded
    constexpr size_t kAvailableCarSlots = 1024;
    constexpr size_t kProfilePrizeCars = 0x58;   // u32 total prize cars

    // car record (SPEC_DATABASE::CarGarage, 1,208 bytes)
    constexpr size_t kBlockBytes = 376;          // 3 parameter blocks at +0, +376, +752
    constexpr size_t kBlockRigidity = 0x168;     // u8: 1 = Increase Rigidity fitted (0x4468f8 type 31)
    constexpr size_t kOwnedParts = 0x48C;        // 96 owned-part bits (0x43f470)
    constexpr int kRigidityOwnedBit = 95;        // base(type 31) 94 + category 1 (0x43f270)
    constexpr size_t kOdometer = 0x498;          // u32 metres (max 100,000,000)
    constexpr size_t kBreakIn = 0x49C;           // f32 engine distance (break-in), max 11,000,000
    constexpr size_t kOil = 0x4A0;               // f32 metres on this oil; < 0 = fresh-oil window
    constexpr size_t kDirt = 0x4A4;              // f32 0 .. 0.8
    constexpr size_t kBodyLife = 0x4A8;          // f32 body wear in metres, max 3,000,000
    constexpr size_t kFrozen = 0x4AC;            // u8: condition never changes (0x343420)

    // GT4's own values (CORE.elf bytes at 0x620cd8 + 0xD0..0xD8 = 3,30,10,100,50,2,50,60,50, scaled by 0x350678 into
    // 0x620320; read back from GT4's RAM in run sf-x1-garage; SAVE-FORMAT.md section 5)
    constexpr float kOilAfterChange = -300000.0f; // replaceOil 0x343928: 3 x -(D5 x 100,000) / 2
    constexpr float kOilMax = 6000000.0f;         // D7 x 100,000
    constexpr float kOilChangeDue = 5000000.0f;   // D8 x 100,000 (0x3437b0)
    constexpr float kBreakInStart = -300000.0f;   // -(D0 x 100,000), a new car (0x343658)
    constexpr float kBreakInMax = 11000000.0f;    // (D3 + D2) x 100,000
    constexpr float kBodyLifeMax = 3000000.0f;    // 0x620b00 + 0x18 (run time)

    struct Condition
    {
        uint32_t odometerM = 0;
        float breakIn = 0, oil = 0, dirt = 0, bodyLife = 0;
        bool frozen = false;
        bool rigidity = false;      // fitted (all three blocks' flag)
        bool rigidityOwned = false; // the owned-part bit
        // metres driven on this oil (GT4's getOilLifeRate numerator, 0x3437f0)
        double oilAgeM() const { return oil < 0 ? double(oil) - double(kOilAfterChange) : double(oil); }
    };

    enum class Record
    {
        None,     // no car in that garage slot
        Stock,    // in the garage, no full record: GT4 rebuilds it from the index (0x435568)
        Riding,   // the car being driven: GAMEDATA 0x135FC
        Stored    // full record in its garage file slot
    };
    Record record(const Save &save, int slot);
    const char *recordName(Record r);
    // The car's 1,208 bytes inside save.dec (nullptr for None / Stock).
    uint8_t *carBytes(Save &save, int slot);
    const uint8_t *carBytes(const Save &save, int slot);
    std::optional<Condition> condition(const Save &save, int slot);

    // ---- the services (full-record cars only: Riding or Stored). Each says what it did in `note`.
    bool changeOil(Save &save, int slot, std::string &note);      // oil = kOilAfterChange (GT4's replaceOil)
    bool wash(Save &save, int slot, std::string &note);           // dirtiness = 0 (GT4's car wash ends at 0)
    bool removeRigidity(Save &save, int slot, std::string &note); // flag 0 in all 3 blocks + owned bit cleared
    bool refreshBody(Save &save, int slot, std::string &note);    // bodyLife = 0 (GT4's Rigidity Refresher Plan)
    // Test helper only (no launcher button): fit Increase Rigidity the way GT4's tune shop does.
    bool fitRigidity(Save &save, int slot, std::string &note);

    // ---- credits (garage header words 0-1)
    bool setCredits(Save &save, uint64_t credits, std::string &note);

    // ---- adding a car (Car Dealers). GT4's purchase (gtmode.adc BuyCarDialog): garage.addUsedCar / addNewCar (both
    // 0x435800: the index entry and the day), calendar.putBuyUsedCarEvent / putBuyNewCarEvent (one day passes),
    // setCarAvailable, incMoney(-price).
    struct NewCar
    {
        uint64_t code = 0;          // spec code, table 0 (GENERIC_CAR)
        int colour = 0;             // colour index (< the car's colour count)
        uint32_t odometerM = 0;     // index +0x14 (GT4 rebuilds the car's condition from it)
        uint32_t powerX100 = 0;     // index bits 9-26 (GT4's garage list power); 0 = leave for GT4
        uint32_t weightKg = 0;      // index bits 32-44
        bool logCalendar = true;    // GET_CAR entry and today + 1, as GT4 does
        int how = 2;                // the GET_CAR entry's "how": 1 bought new, 2 bought used (0x134c60 / 0x134d40)
        bool setAvailable = true;   // the Arcade list of owned cars, as GT4 does
        std::optional<uint64_t> price; // credits taken (nullopt = free)
    };
    int addCar(Save &save, const NewCar &car, std::string &note); // the garage slot, or -1 (note says why)

    // ---- the player's name (GAMEDATA profile +0x00 = payload 0x10, char[32], "username"; setter 0x43af08). Written as
    // gt4save.py edit --name writes it: the bytes, then zeros to 32. The caller keeps GT4's own name rules (nameProblem in
    // gt4full_saveedit.h); this refuses only what cannot be stored (empty, 32 bytes or more, a NUL).
    constexpr size_t kPlayerName = 0x10;
    constexpr size_t kPlayerNameBytes = 32;
    std::string playerNameField(const Save &save); // the stored name (up to the first NUL)
    bool setPlayerName(Save &save, const std::string &name, std::string &note);

    // ---- the recomp's memory-card append bug (gt4save.py repair, the same rules and the same bytes). An earlier recomp
    // build appended each write past the end of the file: GAMEDATA copies one after the other (the newest last), garage
    // header and car-slot writes past the 1,000 slots. The repair keeps the newest GAMEDATA copy, the icons' last copy when
    // they were appended too, and writes the garage at GT4's size with the appended writes put where GT4 meant them:
    //   - the newest appended header that matches the GAMEDATA (GT4's own check 0x1cba80: key, counter in A or B) goes to
    //     offset 0; when none matches, the header at offset 0 stays if it matches (a later, fixed build wrote the start of
    //     the file again, so the appended headers are older: the developer's GT4 Test Build 11 save, 2026-10-09), else refused;
    //   - appended car writes go to their slots in file order (the last wins); refused when the appended headers are the
    //     older ones (then which car records are newest cannot be told);
    //   - refused: nothing to repair, the newest GAMEDATA copy fails its envelope CRC, unrecognised appended bytes.
    // files: the save folder's files as read (name -> bytes); save: loaded from the same files. out: the files the repair
    // writes (GAMEDATA, garage, icon.sys, icon0.ico; the ones the folder has). why: the refusal.
    struct RepairInfo
    {
        uint32_t gamedataCopies = 1;
        uint64_t garageBytes = 0;
        int headerWrites = 0, carWrites = 0;
        bool tailHeader = false;      // an appended header goes to offset 0
        bool staleTail = false;       // the header at offset 0 stays: the appended headers are older
        std::vector<int> carSlots;    // the slots the appended car writes go to
        uint64_t creditsBefore = 0, creditsAfter = 0;
    };
    bool repairFiles(const Save &save, const std::map<std::string, std::vector<uint8_t>> &files,
                     std::map<std::string, std::vector<uint8_t>> &out, std::string &why, RepairInfo *info = nullptr);
    // The save as the repair leaves it, in decrypted form (verify() compares the repaired copy with it: nothing may differ).
    Save repairExpected(const Save &save);

    // ---- finishing
    void fixInnerCrc(Save &save); // after any GAMEDATA change (the services call it)
    // Re-encrypt (GT4's seeds, key, counters, nonces kept) and write the four files into a NEW folder.
    bool writeFolder(const Save &save, const fs::path &outDir, std::string &error);

    // ---- checking a written copy
    struct Change
    {
        std::string file;   // "GAMEDATA", "garage header", "garage slot 12"
        size_t offset = 0;  // in the decrypted bytes
        size_t length = 0;
        std::string field;  // e.g. "car[slot 0] +0x4a0 oil"
    };
    std::vector<Change> diff(const Save &before, const Save &after);
    std::string describeOffset(const std::string &file, size_t offset);
    // Load the written folder, check GT4's load checks and that only the allowed fields changed.
    struct VerifyResult
    {
        bool ok = false;
        std::vector<std::string> lines;
    };
    VerifyResult verify(const Save &original, const fs::path &writtenDir, const std::vector<std::string> &allowedFields);
}
