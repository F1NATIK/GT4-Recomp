#pragma once

// GT4's button codes (USA disc, SCUS-97328) and the rule GT4 follows while one is typed.
// One table for GT4.exe's in-game cheat card (gt4_cheat_card.cpp) and the GT4 Recompilation
// launcher's Cheats page (Practise, Save card, the printed sheet): header only, C++20, no
// dependencies. From the launcher (src/launcher/gt4stock): #include "../../../include/runtime/gt4_cheat_codes.h".
//
// Facts [checked on the disc, analysis/cheats/GT4-CHEATS.md 1, 4, 5]: the five codes are the
// craft_command lists of projects/gtmode/gtmode.adc, the sixth is option.adc's sSecretCommand.
// Grouping and the "remember it as" lines are ours (CHEATS-TAB-DESIGN.md 3.2).
//
// Button bits are the PS2 pad's (the 16-bit button word, active high here):
//   Select 0x0001, L3 0x0002, R3 0x0004, Start 0x0008, Up 0x0010, Right 0x0020, Down 0x0040,
//   Left 0x0080, L2 0x0100, R2 0x0200, L1 0x0400, R1 0x0800, Triangle 0x1000, Circle 0x2000,
//   Cross 0x4000, Square 0x8000.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gt4cheats
{
    enum Button : uint16_t
    {
        Select = 0x0001,
        L3 = 0x0002,
        R3 = 0x0004,
        Start = 0x0008,
        Up = 0x0010,
        Right = 0x0020,
        Down = 0x0040,
        Left = 0x0080,
        L2 = 0x0100,
        R2 = 0x0200,
        L1 = 0x0400,
        R1 = 0x0800,
        Triangle = 0x1000,
        Circle = 0x2000,
        Cross = 0x4000,
        Square = 0x8000,
    };

    // The buttons that make a menu key event in GT4 (CORE key table 0x6192e0 plus the d-pad
    // arrows). L3 and R3 make none: a press of either neither advances nor resets a code
    // [checked in a pad run: analysis/cheats/runtime, run c-l3r3-credits].
    constexpr uint16_t kMenuKeys = 0xffffu & ~uint16_t(L3 | R3);

    // The order key events are taken in when several buttons go down in one pad sample: the CORE key
    // table's order (0x6192e0: Circle, Cross, Start, Select, L1, R1, L2, R2, Square, Triangle; checked,
    // analysis/cheats/cheat_evidence.py prints it), then the arrows. That GT4 walks the table in this
    // order within one sample, and makes the arrows after it, is inferred (GT4 makes the arrows elsewhere).
    constexpr std::array<uint16_t, 14> kKeyOrder{Circle, Cross, Start, Select, L1, R1, L2, R2, Square, Triangle, Up, Down, Left, Right};

    constexpr std::string_view buttonName(uint16_t b)
    {
        switch (b)
        {
        case Select: return "Select";
        case L3: return "L3";
        case R3: return "R3";
        case Start: return "Start";
        case Up: return "Up";
        case Right: return "Right";
        case Down: return "Down";
        case Left: return "Left";
        case L2: return "L2";
        case R2: return "R2";
        case L1: return "L1";
        case R1: return "R1";
        case Triangle: return "Triangle";
        case Circle: return "Circle";
        case Cross: return "Cross";
        case Square: return "Square";
        default: return "?";
        }
    }

    struct Code
    {
        std::string_view id;        // the settings id (GT4_CHEAT_CARD_LIST, GT4L_CHEATS_TICKED)
        std::string_view name;      // "10,000,000 credits"
        std::string_view shortName; // "10M credits"
        std::string_view screen;    // where it is typed, short
        std::string_view where;     // where it is typed, a sentence
        std::string_view effect;    // what it does
        std::string_view aid;       // "Remember it as"
        std::string_view days;      // calendar days it adds
        std::string_view history;
        std::string_view note;
        bool dayRule;               // needs Day 367 (GT4's getElapsedDate() > 365), unless GT4_CHEATS=noday
        bool controller2;           // GT4 takes it from controller 2 only, unless GT4_CHEATS=secret
        uint8_t count;              // presses
        std::array<uint16_t, 16> presses;
        uint8_t groupCount;
        std::array<uint8_t, 8> groups; // sizes, in order (sum == count)
    };

    inline constexpr std::array<Code, 6> kCodes{{
        {"credits", "10,000,000 credits", "10M credits", "GT Mode map", "On the GT Mode map (Home, the dealers, the tuners), nothing open.",
         "Adds 10,000,000 credits at once. Works again and again.",
         "Select to open and close. In between, four d-pad threes: Left Right Right, Down Up Up, Left Down Up, Right Left Down. Then bank it with L1, R1.",
         "Adds 0 days",
         "In the game since 2005, hidden behind the day check. Found by modder Nenkai in March 2023 while reading GT4's menu scripts.",
         "GT4 adds the credits with no cap; the display probably stops at 99,999,999, so nine times is plenty.", true, false, 16,
         {Select, Left, Right, Right, Down, Up, Up, Left, Down, Up, Right, Left, Down, L1, R1, Select}, 7, {1, 3, 3, 3, 3, 2, 1, 0}},
        {"licence", "Pass a licence", "Pass a licence", "Licence Center", "In the Licence Center, with the licence highlighted (not opened).",
         "All 16 tests of the highlighted licence become passed (no medal).",
         "Select R1 twice, then Select. Both twos twice: L2 L2, R2 R2. Then L1 Select twice.", "Adds 16 days",
         "The code in Nenkai's first video (25 March 2023). Some news sites printed it one press short (12 presses); that version never works.",
         "Each test counts as a day in GT4's calendar, so the day counter jumps by 16.", true, false, 13,
         {Select, R1, Select, R1, Select, L2, L2, R2, R2, L1, Select, L1, Select, 0, 0, 0}, 7, {2, 2, 1, 2, 2, 2, 2, 0}},
        {"test", "Gold on a licence test", "Gold one test", "Licence test list", "Inside a licence, with the test highlighted.",
         "The highlighted test gets a gold medal.", "Select twice. Then R1 R2 and L2 L2, Select. L1 R1, Select. R2 L1, Select.", "Adds 1 day",
         "Part of the same find (March 2023). One test at a time: handy for the one you cannot gold.", "Only the highlighted test changes.", true,
         false, 13, {Select, Select, R1, R2, L2, L2, Select, L1, R1, Select, R2, L1, Select, 0, 0, 0}, 8, {2, 2, 2, 1, 2, 1, 2, 1}},
        {"event", "Win an event race", "Win a race", "Event race list", "In an event (series), with the race highlighted.",
         "Records first place for the highlighted race. It pays no prize money itself.",
         "Select before each shoulder, in the order L1, R1, L2, R2. Up Up after L1, Down Down after R1. End on Select.", "Adds 1 day",
         "Same find. A GTPlanet player reported (29 March 2023): use it, choose Enter Series, abandon, and the championship's prize car is yours.",
         "The Up Up and Down Down cancel out, so the cursor ends on the same race.", true, false, 13,
         {Select, L1, Up, Up, Select, R1, Down, Down, Select, L2, Select, R2, Select, 0, 0, 0}, 5, {4, 4, 2, 2, 1, 0, 0, 0}},
        {"mission", "Mission Hall code", "Mission Hall (10M)", "Mission Hall", "In the Mission Hall.",
         "Gives 10,000,000 credits. Probably meant to pass a mission: a bug in the shipped game.",
         "Select R2 twice. L1 L2 L2. Select L1 R1. Select R1. Select.", "Adds 0 days",
         "Probably a mission skip wired to the money call by mistake. A GTPlanet poster noted in 2024 that a working version would make Mission 34 trivial.",
         "Same effect as the map code.", true, false, 13, {Select, R2, Select, R2, L1, L2, L2, Select, L1, R1, Select, R1, Select, 0, 0, 0}, 6,
         {2, 2, 3, 3, 2, 1, 0, 0}},
        {"options", "Hidden Options pages", "Hidden Options pages", "Options screen", "On the Options screen, typed on controller 2.",
         "Adds two pages to Options: Special Settings and Detail Monitor Settings.",
         "Shoulders in order L1, L2, R1, R2, each with a direction Up, Down, Left, Right. Then Start twice.", "No day rule",
         "Known before the 2023 find: posted to ChapterCheats in March 2021 and documented by The Cutting Room Floor.",
         "The pages are gone when you leave Options (type it again next time); what you set on them is saved.", false, true, 10,
         {L1, Up, L2, Down, R1, Left, R2, Right, Start, Start, 0, 0, 0, 0, 0, 0}, 5, {2, 2, 2, 2, 2, 0, 0, 0}},
    }};

    inline const Code *find(std::string_view id)
    {
        for (const Code &c : kCodes)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    // "credits,licence,options" (commas or spaces; case ignored) -> the codes in that order,
    // each once. Unknown ids go to `unknown`. "all" = every code in table order.
    inline std::vector<const Code *> parseList(std::string_view text, std::vector<std::string> *unknown = nullptr)
    {
        std::vector<const Code *> list;
        std::string word;
        auto take = [&] {
            if (word.empty())
                return;
            for (char &c : word)
                if (c >= 'A' && c <= 'Z')
                    c = char(c - 'A' + 'a');
            if (word == "all")
            {
                for (const Code &c : kCodes)
                    if (std::find(list.begin(), list.end(), &c) == list.end())
                        list.push_back(&c);
            }
            else if (const Code *c = find(word))
            {
                if (std::find(list.begin(), list.end(), c) == list.end())
                    list.push_back(c);
            }
            else if (unknown)
                unknown->push_back(word);
            word.clear();
        };
        for (char ch : text)
        {
            if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
                take();
            else
                word.push_back(ch);
        }
        take();
        return list;
    }

    // GT4's craft() (gtmode.adc 0x3b44d, re-created by Nenkai's OpenAdhoc; the same bytes on our disc):
    //   if (dayOK && index < size && key == list[index]) { if (++index >= size) { run the cheat; index = 0; } }
    //   else index = 0;
    // A wrong key starts over, and that key does not count as press 1. Only menu keys are
    // events; L3 / R3 change nothing. The day rule is not known here: the follower counts as
    // if the day test passes (the card says when it may not).
    class Follower
    {
    public:
        enum class Event
        {
            None,      // not a menu key, or no code
            Advanced,  // the next button: one more done
            Completed, // the last button: GT4 runs the cheat; the count starts over
            Reset,     // a wrong key after some progress: back to 0
            Wrong,     // a wrong key with nothing done yet
        };

        explicit Follower(const Code *code = nullptr) : code_(code) {}
        // A new code (or none); the pad's held buttons stay known, so a button already down is
        // not taken as a new press.
        void reset(const Code *code)
        {
            code_ = code;
            index_ = 0;
        }
        // The buttons held now, taken as already down (no key events).
        void prime(uint16_t held) { previous_ = held; }
        void restart() { index_ = 0; }
        const Code *code() const { return code_; }
        uint8_t progress() const { return index_; }

        // One key event (a single button bit).
        Event key(uint16_t button)
        {
            if (!code_ || !(button & kMenuKeys))
                return Event::None;
            if (index_ < code_->count && button == code_->presses[index_])
            {
                if (++index_ >= code_->count)
                {
                    index_ = 0;
                    return Event::Completed;
                }
                return Event::Advanced;
            }
            const bool lost = index_ != 0;
            index_ = 0;
            return lost ? Event::Reset : Event::Wrong;
        }

        // One pad sample (the buttons held now, active high): the new presses as key events
        // in kKeyOrder. Returns the most notable event (Completed > Reset > Wrong > Advanced).
        Event sample(uint16_t held)
        {
            const uint16_t pressed = uint16_t(held & ~previous_);
            previous_ = held;
            Event worst = Event::None;
            auto rank = [](Event e) {
                switch (e)
                {
                case Event::Completed: return 4;
                case Event::Reset: return 3;
                case Event::Wrong: return 2;
                case Event::Advanced: return 1;
                default: return 0;
                }
            };
            for (uint16_t b : kKeyOrder)
                if (pressed & b)
                {
                    const Event e = key(b);
                    if (rank(e) > rank(worst))
                        worst = e;
                }
            return worst;
        }

    private:
        const Code *code_ = nullptr;
        uint8_t index_ = 0;
        uint16_t previous_ = 0;
    };
}
