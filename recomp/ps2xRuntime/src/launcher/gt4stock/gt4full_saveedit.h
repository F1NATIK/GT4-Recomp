#pragma once

// GT4 Recompilation launcher, Full edition: the Garage's save-changing actions (FULL-LAUNCHER-PLAN.md 1.9, F4), with no
// raylib and no window (the host tests build it):
//   My garage: Oil change and Wash car (GT4's own price, Cr. 50 each), Remove Increase Rigidity (the developer's "Body Rigidity
//   Improvement"; free: GT4 has no such service), Rigidity Refresh (GT4's Rigidity Refresher Plan: body wear back to 0,
//   GT4's own price Cr. 50,000 and its own rule for which cars, kRefreshPrice below);
//   Car Dealers: Hidden Cars (the four black race cars, as GT4's used-car lot sells them) and Prize Cars (the cars GT4
//   only gives as prizes, as a new car), each bought with credits or added free;
//   Save details (developer 2026-10-09): Repair (the old memory-card append bug's extra data, gt4save.py repair's rules and
//   bytes; checked against the save as the repair leaves it, nothing else may differ) and Change name (GT4's name).
//
// The rules, in this order, every time (applyEdit):
//   1. GT4 is closed (no GT4.exe running; when that cannot be checked, nothing is changed);
//   2. the save is the one the player confirmed (its files unchanged since the confirmation was shown) and passes GT4's
//      own load checks; a .ps2 card file is not changed (folder cards only);
//   3. an automatic backup of the memory card (game\backups\GT4-mc<n>-<date>_<time>-before-edit.zip), read back and
//      compared with the save's files byte for byte;
//   4. the changed save written to a new folder (game\backups\garage-staging) with GT4's own seeds, key, counters and
//      nonces, and checked: it loads, GT4's load checks pass, exact file sizes, it re-encrypts to itself, and only the
//      planned fields differ (gt4save::edit::verify);
//   5. then the card's files replaced (each written next to it, flushed to the disk, and renamed over it), read back and
//      checked again; if anything fails, the save's own bytes are put back and the player is told.
// While step 5 runs, a note of the change (game\backups\garage-change-pending.txt: the backup, the save's files before and
// after) is on the disk. It goes once the card checks out (or is put back). A note still there at the next start means the
// change was cut off (a crash, the power): the launcher compares the save with the note and, when it is half changed,
// offers to restore the backup; the Garage makes no other change until then.
// The Classic edition never compiles this file.

#include "gt4full_model.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gt4save
{
    struct Save;
    class SpecDb;
}

namespace gt4stock
{
    namespace fs = std::filesystem;

    // GT4's price for an oil change and for a car wash: gtmode.adc (US) sureEnoughMoney(context, 50) and incMoney(-50) in
    // CarWashRoot::Wash (0xc679a, 0xc690b) and OilChangeRoot::Change (0xc9ef1, 0xca118), the same 50 on both price labels.
    constexpr uint64_t kServicePrice = 50;

    // GT4's Rigidity Refresher Plan (the developer's "Rigidity Refresh"), from gtmode.adc (US) [C]:
    //   - TuningRoot::Pane::onLoad lists it (type 32, STIFFNESS_REFRESH) in every tune shop's "Others" page, after Increase
    //     Rigidity (type 31): parts_types [10, 7, 8, 31, 32]; enable_parts enables type >= 32 for any car the shop can tune
    //     (car.canTune; a car whose tuner is "polyphony", DiscCar::tuner, gets "this car has no tuningshop"). It is NOT limited
    //     to cars with Increase Rigidity.
    //   - TuningApplyRoot::getPartsCondition, type 32: exist 1, allowed 1, price 50000, have = (car.getBodyLifeRate() == 0.0);
    //     SelectBox::onActivate: "have" -> "You have already done this parts." (refused), else TuningDialog, incMoney(-price),
    //     setNeedSave, refreshBody() (0x14edd8 -> 0x343a58: body life = 0).
    //   - getBodyLifeRate (0x343a80): 0 when the condition is frozen (+0x4AC), else 0x366300: 0.5 x the curve at 0x620b24 of
    //     the body life, a 2-point table x {500,000, 3,000,000} -> y {0, 2.0} (GT4's RAM, run fl-oil ee-1300.bin; 0x359568
    //     returns y[0] for x <= x[0]). So GT4 treats the plan as done while body life is 500,000 m (500 km) or less.
    constexpr uint64_t kRefreshPrice = 50000;
    constexpr float kRefreshFreshM = 500000.0f;

    enum class GarageAction
    {
        OilChange,
        Wash,
        RemoveRigidity,
        AddCar,
        RefreshBody, // "Rigidity Refresh": GT4's Rigidity Refresher Plan (body life = 0, Cr. 50,000)
        RepairSave,  // Save details > Repair: the recomp's memory-card append bug undone (gt4save.py repair; developer 2026-10-09)
        RenameSave,  // Save details > Change name: the player's name in GT4 (developer 2026-10-09)
    };

    // GT4's own name entry (boot.adc, EntryRoot: its prompt "Please input your name within 10 letters", "No letters" for
    // an empty name, then "Are you sure your name is this?"): at most 10 letters. Which characters GT4's on-screen keyboard
    // has is in boot.mproject (not extracted), so the launcher takes only what that keyboard surely has: A-Z, a-z, 0-9 and
    // spaces between them.
    constexpr size_t kNameMaxLetters = 10;
    // Why GT4 would not take this name ("" when it would).
    std::string nameProblem(const std::string &name);
    // The two save actions of Save details.
    struct GarageEdit;
    GarageEdit repairEdit();
    GarageEdit renameEdit(const std::string &name);

    // What the player asked for.
    struct GarageEdit
    {
        GarageAction action = GarageAction::OilChange;
        int slot = -1;           // the services: the garage slot
        std::string carName;     // GT4's name of the car (the texts)
        // Adding a car (Car Dealers)
        uint32_t code = 0;       // spec code (GENERIC_CAR row)
        std::string label;       // "r390_race_98"
        int colour = 0;          // colour index
        std::string colourName;  // "black"
        uint32_t odometerM = 0;  // the car's odometer when it comes (a used car's lot mileage; 0 for a new car)
        int how = 2;             // GT4's diary: 1 bought new, 2 bought used
        uint32_t powerX100 = 0;  // the garage list's power until GT4 works its own out (the disc's catalogue value)
        uint32_t weightKg = 0;
        bool hiddenCar = false;  // Hidden Cars (else Prize Cars)
        // Rigidity Refresh: false when GT4 has no tune shop for the car (it never offers the plan for it)
        bool tuneShop = true;
        // Credits: taken when pay is set (the car's price, GT4's Cr. 50 for an oil change or a wash, Cr. 50,000 for
        // Rigidity Refresh).
        bool pay = false;
        uint64_t price = 0;
        // Change name: the new name (nameProblem() "")
        std::string newName;
    };

    // The services for a car of the garage (pay: GT4's price, always for an oil change, a wash and Rigidity Refresh).
    GarageEdit serviceEdit(const GarageCar &car, GarageAction action);
    // A hidden black car: GT4's used price at the lowest mileage its used-car lot gives a car of that year, that mileage on
    // the odometer, GT4's black colour; bought used. A rare car (HiddenCar::rare): as a prize car (GT4's new price, new,
    // GT4's first colour). Empty label when the car is not on the disc.
    GarageEdit hiddenCarEdit(const CarCatalogue &cat, const HiddenCar &h, bool pay);
    // A prize car: GT4's new price, new (0 km), GT4's first colour; bought new.
    GarageEdit prizeCarEdit(const DiscCar &car, bool pay);
    // The colour chosen in Car Dealers (an index of the car's own GT4 colour list, CAR_VARIATION_american ->
    // VARIATIONamerican): it goes into the added car's garage index entry (flags bits 2-8) and GT4's diary entry.
    // h: the Hidden Cars entry (its black named "black"), or nullptr.
    GarageEdit withColour(GarageEdit e, int colour, const HiddenCar *h);
    // A colour's name: GT4's own, else "Black" (black set: a hidden car's black) or "Colour N" (N = index + 1).
    std::string dealerColourName(const GameNames *names, uint32_t code, int index, bool black);

    // Why an action cannot be offered for a car right now ("" when it can). The services need a car GT4 keeps a full
    // record of, not frozen, and the credits; the removal needs the part; Rigidity Refresh follows GT4's own rule (a car
    // GT4 has a tune shop for, body wear over 500 km) and costs Cr. 50,000.
    std::string serviceBlock(const Garage &g, const GarageCar &car, GarageAction action);

    // Where the change goes: a folder card (game\mc0 or game\mc1) and its backups folder.
    struct EditTarget
    {
        bool ok = false;
        std::string why;    // why not, in the player's words
        int slot = 0;       // memory card slot 1 / 2
        fs::path cardDir;   // game\mc0
        fs::path saveDir;   // game\mc0\BASCUS-97328GAMEDATA
        fs::path backupDir; // game\backups
    };
    EditTarget folderCardTarget(const fs::path &cardDir, int slot, const fs::path &backupDir);

    // What the confirmation shows, worked out on a copy of the save in memory (nothing is written).
    struct EditPlan
    {
        bool ok = false;
        std::string why;                  // refused: the reason in the player's words
        std::string title;                // "Change the oil of your Mazda RX-7 GT-X (FC, J) '90?"
        std::string confirm;              // the button: "Change the oil (Cr. 50)"
        std::vector<std::string> changes; // exactly what changes in the save, one line each
        std::vector<std::string> allowed; // the save fields verify() lets differ (the rest must stay byte-identical)
        std::string fingerprint;          // the save's files as they were read (sizes and CRC-32s)
        int newSlot = -1;                 // adding a car: the garage slot it gets
    };
    EditPlan planEdit(const gt4save::Save &save, const fs::path &saveDir, const GarageEdit &e, const GameNames *names);

    // The save's files (name, size, CRC-32), sorted; "" when the folder cannot be read.
    std::string saveFingerprint(const fs::path &saveDir);

    struct EditResult
    {
        bool ok = false;
        std::string message;           // for the player (the status line and the dialog)
        fs::path backup;               // the automatic backup made before the change
        bool rolledBack = false;       // the card's files were put back after a failed check
        bool logged = true;            // the line in garage-changes.txt was written (a done change whose line failed: false)
        std::vector<std::string> log;  // every step (tests, the dialog's details)
    };
    // gt4State: 0 GT4 closed, 1 running, -1 could not tell (refused). fingerprint: EditPlan::fingerprint (refused when the
    // save changed since). version: the launcher's, for the backup's note.
    EditResult applyEdit(const EditTarget &target, const GarageEdit &e, const std::string &fingerprint, int gt4State,
                         const gt4save::SpecDb *spec, const GameNames *names, const std::string &version);

    // What the player is told after a change went through (backup: the automatic backup made before it).
    std::string editDoneText(const GarageEdit &e, const fs::path &backup);

    // 0 no GT4 running, 1 a GT4.exe (or "Gran Turismo 4.exe") is running, -1 the process list could not be read.
    int gt4ProcessState();

    // The changes made so far (game\backups\garage-changes.txt, one line each), newest last.
    std::vector<std::string> garageChangeLog(const fs::path &backupDir);

    // A change that was cut off (the note of step 5 still on the disk).
    struct PendingChange
    {
        bool present = false;
        std::string title;      // the change, as the confirmation named it
        std::string backup;     // the backup's file name in game\backups
        int slot = 0;           // memory card slot 1 / 2
        fs::path saveDir;       // the save's folder on the card
        std::string before;     // saveFingerprint() of the save before the change
        std::string after;      // and of the changed save
        std::string state;      // "half" (some files replaced), "done" (all replaced), "none" (none replaced)
    };
    PendingChange pendingChange(const fs::path &backupDir); // present = false when there is no note
    // Removes the note (the launcher's own file) when the save is as it was or as planned; false when it is half changed.
    bool settlePendingChange(const fs::path &backupDir, std::string *what = nullptr);
    // The note's file name (tests).
    fs::path pendingChangeFile(const fs::path &backupDir);
}
