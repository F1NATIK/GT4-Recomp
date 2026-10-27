#pragma once

// GT4 Recompilation launcher: what the Saves page's read-only "Save details" view shows, built
// from a GT4 save (the gt4save library, savelib/) and the names read from the disc
// (gt4stock_names.h). No raylib here: the host tests build it on its own.
//
// The checks are worded for a player. The library's own messages are the Python tools' (for
// its golden tests); none of them reaches the page, and nothing here offers to change the save.

#include "gt4stock_names.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4save
{
    struct Save;
}

namespace gt4stock
{
    struct DetailSetting
    {
        std::string label, value;
    };

    struct DetailCar
    {
        int slot = 0;            // the garage slot (GT4's order of purchase)
        bool riding = false;     // the car being driven
        bool fullRecord = false; // GT4 keeps its parts and settings (driven at least once)
        uint64_t code = 0;       // spec code (table 0)
        std::string name;        // GT4's list name from the disc, else the spec label / "Car #n"
        std::string fullName;    // GT4's full name when it differs
        bool nameFromDisc = false;
        int colourIndex = 0;
        std::string colourName; // from the disc ("Silver Metallic Graphite"), else "Colour n"
        bool hasRgb = false;
        uint8_t r = 0, g = 0, b = 0;
        uint64_t odometerM = 0; // metres
        double power = 0.0;     // as GT4's garage list shows it (the index value / 100)
        int weightKg = 0;
        int dayAdded = 0;     // the save's elapsed count (GT4 shows it as day dayAdded + 1)
        std::string acquired; // "Bought new, day 3", "Prize car, day 12", ... (GT4's day numbers)
        bool prize = false;
        // Expanded: the fitted parts GT4's tune shop names, and the car's settings.
        std::vector<std::string> parts;
        std::string partsNote;
        std::vector<DetailSetting> settings;
    };

    struct DetailLicence
    {
        std::string key;   // "B", "A", "IB", "IA", "S"
        std::string name;  // "B License" (GT4's text)
        std::array<char, 16> marks{}; // per test: 'G' gold, 'S' silver, 'B' bronze, 'P' passed, '-' not yet
        bool passed = false, allGold = false;
        int gold = 0, silver = 0, bronze = 0, done = 0;
    };

    struct DetailSeries
    {
        std::string series; // "am_sunday"
        std::string name;   // "Sunday Cup" (GT4's text), else the code
        std::string hall;   // "Beginner Events"
        int races = 0, done = 0, won = 0;
        int points = 0; // A-spec points: the best score of each race
        int firstRow = 0;
    };

    struct DetailProblem
    {
        int level = 1; // 2: GT4 will not load the save; 1: a note
        std::string title, text;
    };

    struct SaveDetails
    {
        bool loaded = false;
        std::string source;
        std::string player;
        bool appendBug = false; // the recomp's old memory-card append bug left extra data (Full: Repair)
        int64_t days = 0;    // the save library's count, GT4's getElapsedDate() (0 on a new game)
        int64_t gameDay = 1; // 1.4: the day as GT4's own Days counter shows it, days + 1 (gtmode.adc 0x6165c)
        std::string date; // "12 May 2005"
        bool hasCredits = false;
        uint64_t credits = 0;
        uint64_t aspecPoints = 0;
        uint64_t races = 0;
        uint32_t wins = 0;
        uint64_t prizeMoney = 0;
        uint32_t prizeCars = 0;
        uint64_t distanceM = 0;
        int loadState = 0; // 0: GT4 loads it; 1: GT4 loads it, see the note; 2: GT4 will not load it
        std::vector<DetailProblem> problems;
        std::vector<std::string> technical; // the library's other findings, plainly
        std::vector<DetailCar> cars;        // the riding car first, then GT4's garage order
        std::vector<DetailLicence> licences;
        int coffeeDone = 0, coffeeTotal = 5, missionsDone = 0, missionsTotal = 34;
        std::vector<DetailSeries> series; // events with at least one race run, in hall order
        int eventsWon = 0, eventsDone = 0;
        std::string championship;          // a championship saved half-way ("" none)
        std::vector<std::string> prizeList; // prize cars by name
        bool names = false;                // names from the disc
    };

    SaveDetails buildDetails(const gt4save::Save &save, const GameNames *names);

    // Plain text helpers (also used by the page).
    std::string dateText(const std::string &isoDate); // "2005-05-12" -> "12 May 2005"
    std::string loadStateText(int loadState);         // "GT4 loads this save", ...
    // A save library check as the technical notes show it: without its tool advice, and (1.4 review) its day numbers on
    // GT4's own count (elapsed + 1), as the rest of the page.
    std::string plainCheck(const std::string &libraryMessage);
}
