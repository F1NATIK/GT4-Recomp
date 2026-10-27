#pragma once

// GT4 Recompilation launcher (Classic edition; internal names keep "stock"): the Cheats page's model
// (new in 1.4; analysis/cheats/CHEATS-TAB-DESIGN.md 3, 7). No raylib types here, so the host tests build it.
//
// The six cards are GT4's own button codes, one table shared with GT4.exe's in-game cheat card:
// shared/gt4_cheat_codes.h, a byte-for-byte copy of the cheats runtime's
// ps2xRuntime/include/runtime/gt4_cheat_codes.h (analysis/cheats/runtime; tests/build-test.ps1 compares
// the two). The launcher adds what only its page needs: the status chip, the day as GT4 counts it, the
// Mission Hall card with the fix switched on, the file names "Save card" and "Save PDF" use, and a
// small PDF writer (one RGB picture per page, Flate-compressed with raylib's deflate).

#include "shared/gt4_cheat_codes.h"
#include "gt4stock_settings.h"
#include "gt4stock_util.h" // ExeFeatures

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gt4stock
{
    namespace fs = std::filesystem;

    // GT4's GT Mode "Days" label is dayToGameDay(today) + 1 (gtmode.adc 0x6165c); the save library's
    // count (today - 2,453,462, Calendar+0xB6C0) is GT4's getElapsedDate(). A new game is Day 1.
    constexpr long long kCheatDayShown = 367;       // the codes need Day 367 on GT4's counter...
    constexpr long long kCheatElapsedNeeded = 366;  // ...which is getElapsedDate() > 365
    inline long long gameDay(long long elapsed) { return elapsed + 1; }
    inline bool dayRuleMet(long long elapsed) { return elapsed >= kCheatElapsedNeeded; }

    enum class CheatStatus
    {
        NeedsDay,         // "Needs Day 367"
        ReadySkipped,     // "Ready: wait skipped" (GT4_CHEATS=noday)
        Ready,            // "Ready" (the save is at Day 367 or later)
        NeedsController2, // "Needs controller 2" (the hidden Options code, no player 2 and no unlock)
        ReadyController2, // "Ready: on controller 2" (a player 2 controller is there now)
        ReadyController1, // "Ready: controller 1 can type it" (GT4_CHEATS=secret)
    };
    // elapsed: the slot-1 save's getElapsedDate(), or -1 (no save: a new game starts at Day 1). s: the switches GT4.exe
    // acts on (cheatsInEffect); player2Ready: a player 2 now, with a GT4.exe that has player 2.
    CheatStatus cheatStatus(const gt4cheats::Code &code, const CheatSettings &s, long long elapsed, bool player2Ready);
    const char *cheatStatusText(CheatStatus st);
    bool cheatStatusReady(CheatStatus st);

    // (1.4 review) The switches the GT4 program to be started acts on: the settings with every switch that program
    // does not read turned off (exeFeatures: no cheats runtime, no in-game card, no Mission Hall fix). The page's chips
    // and words, the Play page's chip and the F2 hints use these, so the launcher never says something works that the
    // program cannot do. The settings file keeps the player's choice.
    CheatSettings cheatsInEffect(const CheatSettings &s, const ExeFeatures &gt4);

    // What the card says, with the Mission Hall fix applied when it is on (MISSION-FIX.md 0 and 4).
    std::string cheatEffect(const gt4cheats::Code &code, const CheatSettings &s);
    std::string cheatDays(const gt4cheats::Code &code, const CheatSettings &s); // "Adds 0 days", "Adds 1 day", "No day rule"
    std::string cheatWhere(const gt4cheats::Code &code, const CheatSettings &s);  // the sentence
    std::string cheatScreen(const gt4cheats::Code &code);                         // short: "GT Mode map screen"
    std::string cheatController(const gt4cheats::Code &code, const CheatSettings &s); // "Any controller", "Controller 2", ...
    std::string cheatHistory(const gt4cheats::Code &code, const CheatSettings &s);
    // The printed sheet's "Does:" line (the name alone does not say it for the Mission Hall code), with the Mission Hall
    // fix's garage warning when it is on.
    std::string cheatSheetDoes(const gt4cheats::Code &code, const CheatSettings &s);
    std::string cheatNote(const gt4cheats::Code &code, const CheatSettings &s);
    std::string cheatShortName(const gt4cheats::Code &code, const CheatSettings &s); // "10M credits", "Mission Hall (our fix)"
    // Changes what is saved when GT4 saves (every code but the hidden pages' itself changes save data;
    // the hidden pages' settings are saved too): the "Back up your save first" line.
    bool cheatChangesSave(const gt4cheats::Code &code);
    // A button's name as the page says it: "Select", "Up", "L1".
    std::string cheatButtonName(uint16_t button);

    // The ticked cards in their order (GT4L_CHEATS_TICKED, GT4_CHEAT_CARD_LIST: the order F2 shows them; a card ticked
    // on the page goes last).
    std::vector<const gt4cheats::Code *> tickedCodes(const CheatSettings &s);
    bool isTicked(const CheatSettings &s, std::string_view id);
    void setTicked(CheatSettings &s, std::string_view id, bool on);
    // The switches that change GT4 when on, in words ("365-day wait skipped", ...); empty = GT4 as shipped.
    std::vector<std::string> cheatSwitchesOn(const CheatSettings &s);

    // "Save card" / "Save PDF": long names, which GT4's USB stick skips (no 8.3 form, gt4_usbdisk.cpp).
    // With the Mission Hall fix on, its card is "GT4 cheat card - Mission Hall (our fix).png" (the picture shows the fix).
    std::string cheatCardFileName(const gt4cheats::Code &code, const CheatSettings &s); // "GT4 cheat card - 10M credits.png"
    const char *cheatSheetFileName();                           // "GT4 cheat sheet.pdf"
    // name, or "name (2)", "name (3)"... when it exists in dir.
    fs::path uniqueFile(const fs::path &dir, const std::string &name);

    // Paper for the sheet, at 300 dpi.
    struct Paper
    {
        int widthPx, heightPx;
        float widthPt, heightPt;
        const char *name;
    };
    Paper paperFor(bool letter); // A4 2480 x 3508 (595.28 x 841.89 pt); Letter 2550 x 3300 (612 x 792 pt)

    // ---- PDF: one RGB picture per page (DeviceRGB, 8 bits, FlateDecode), no other dependency.
    struct PdfPage
    {
        int width = 0, height = 0;   // pixels
        std::vector<uint8_t> rgb;    // width * height * 3
        float widthPt = 0, heightPt = 0;
    };
    // zlib stream (RFC 1950) around raw DEFLATE: 78 9c, the data, Adler-32 (big endian).
    std::vector<uint8_t> zlibStream(const uint8_t *data, size_t size);
    uint32_t adler32(const uint8_t *data, size_t size);
    std::vector<uint8_t> makePdf(const std::vector<PdfPage> &pages, const std::string &title);

    // The PS2 button word (gt4cheats bits, active high) a player's pad gives through the mapping
    // GT4.exe uses (PadSettings: raylib gamepad buttons, triggers as pressure sources); `down` tells
    // whether a raylib gamepad button is held, `axis` an axis' value -1..1. `held` is the pressure buttons' state
    // from the last call (GT4's hysteresis with gt4-pad-pressure.patch: an analog source presses its button from
    // 10 / 255 and lets go below 5, so a trigger resting near 5-10 does not flicker into new presses).
    using PressureHeld = std::array<bool, kPadPressures>;
    template <typename Down, typename Axis> uint16_t padWord(const PadSettings &pad, Down down, Axis axis, PressureHeld &held)
    {
        static const uint16_t bits[kPadButtons] = {gt4cheats::Select, gt4cheats::L3, gt4cheats::R3, gt4cheats::Start, gt4cheats::Up,
                                                   gt4cheats::Right, gt4cheats::Down, gt4cheats::Left, gt4cheats::L2, gt4cheats::R2,
                                                   gt4cheats::L1, gt4cheats::R1, gt4cheats::Triangle, gt4cheats::Circle, gt4cheats::Cross,
                                                   gt4cheats::Square};
        uint16_t w = 0;
        for (int i = 0; i < kPadButtons; ++i)
        {
            const int b = pad.buttons[static_cast<size_t>(i)];
            if (b > 0 && down(b))
                w |= bits[i];
            const int p = pressureOfButton(i);
            if (p >= 0)
            {
                const int a = pad.pressure[static_cast<size_t>(p)];
                bool &h = held[static_cast<size_t>(p)];
                if (a >= 0)
                {
                    // ps2_input_profile.cpp pressure(): the 0..255 value GT4 gets, then gt4-pad-pressure.patch's
                    // "analogHeld = analogHeld ? analog >= 5 : analog >= 10".
                    const float raw = std::clamp((axis(a) + 1.0f) * 0.5f, 0.0f, 1.0f), dz = pad.triggerDeadzone;
                    const long analog = std::lround(std::max(0.0f, raw - dz) / (1.0f - dz) * 255.0f);
                    h = h ? analog >= 5 : analog >= 10;
                    if (h)
                        w |= bits[i];
                }
                else
                    h = false;
            }
        }
        return w;
    }
    // GT4.exe's keyboard keys when no pad is used (ps2_pad.cpp): arrows / WASD, Tab = Select,
    // Enter = Start, Q / E = L1 / R1, Left / Right Shift = L2 / R2 (and X, C, Z, V for the face
    // buttons; Escape is Circle there, but closes the launcher's pop-up).
    struct KeyboardKeys
    {
        bool up, down, left, right, tab, enter, q, e, lshift, rshift, x, c, z, v;
    };
    uint16_t keyboardWord(const KeyboardKeys &k);
    // "View = Select, LB / RB = L1 / R1, LT / RT = L2 / R2" for the player's mapping.
    std::string padNamesLine(const PadSettings &pad, bool playstation);
}
