#pragma once

// The in-game cheat card's rules, without raylib (gt4_cheat_card.cpp draws them; the cheats runtime's host
// tests check them, analysis/cheats/runtime/tests): which controller's presses GT4 takes for a code, and so
// which the card may follow, and what the card may say about GT4_CHEATS, from what the IOP's script overlay
// actually serves (ps2x/iop/gt4_cheats_served.h), never from the setting alone.

#include "runtime/gt4_cheat_codes.h"

#include <array>
#include <cstdint>
#include <string>

namespace gt4cheatcard::rules
{
    // One GT4_CHEATS switch as the card sees it.
    enum class Switch
    {
        Off,     // not asked for
        Pending, // asked; the overlay is not built yet (it is built on the first disc read, before GT4 reads a script)
        Served,  // GT4 is given the patched script
        Failed,  // asked, but the overlay served the file without it (a failed check; the log says why)
    };
    struct Switches
    {
        Switch noday = Switch::Off, secret = Switch::Off, missionfix = Switch::Off;
    };
    // ServedBit flags (1 noday, 2 secret, 4 missionfix) as asked and as served.
    inline Switches fromOverlay(uint8_t asked, bool known, uint8_t served)
    {
        auto one = [&](uint8_t bit) {
            if (!(asked & bit))
                return Switch::Off;
            if (!known)
                return Switch::Pending;
            return (served & bit) ? Switch::Served : Switch::Failed;
        };
        return Switches{one(1), one(2), one(4)};
    }
    // Pending counts as on: GT4 cannot have read the script before the overlay is built (reads wait for it).
    inline bool active(Switch s) { return s == Switch::Served || s == Switch::Pending; }

    // GT4's controller rule. craft() (gtmode.adc) does not look at the port: any controller. OptionRoot.onKeyPress
    // (option.adc 0xbb0f) begins "if (event.port == 0) return 0": controller 1's key events are dropped before the
    // hidden code's count is read or changed, unless `secret` is served (GT4-CHEATS.md 1, 4).
    inline bool takesPort(const gt4cheats::Code &c, int port, const Switches &s) { return !c.controller2 || active(s.secret) || port != 0; }
    // The card is fed port 0 (controller 1) always, port 1 only once something feeds it (the player-2 path).
    inline bool followable(const gt4cheats::Code &c, const Switches &s, bool port1Fed) { return takesPort(c, 0, s) || port1Fed; }

    inline bool fixedMission(const gt4cheats::Code &c, const Switches &s) { return c.id == "mission" && active(s.missionfix); }
    inline std::string name(const gt4cheats::Code &c, const Switches &s)
    {
        return fixedMission(c, s) ? std::string("Pass a mission (our fix)") : std::string(c.name);
    }
    inline std::string screen(const gt4cheats::Code &c, const Switches &s)
    {
        return fixedMission(c, s) ? std::string("Mission Hall, mission highlighted") : std::string(c.screen);
    }
    inline std::string controllers(const gt4cheats::Code &c, const Switches &s)
    {
        if (!c.controller2)
            return "any controller";
        return active(s.secret) ? "controller 1 or 2" : "controller 2";
    }

    enum class Tone
    {
        Ok,
        Warn,
        Bad,
    };
    struct Chip
    {
        std::string text;
        Tone tone = Tone::Warn;
    };
    inline Chip chip(const gt4cheats::Code &c, const Switches &s)
    {
        if (c.id == "mission" && s.missionfix == Switch::Failed)
            return {"missionfix failed: see log", Tone::Bad};
        if (c.controller2)
        {
            if (active(s.secret))
                return {"Controller 1 can type it", Tone::Ok};
            if (s.secret == Switch::Failed)
                return {"secret failed: see log", Tone::Bad};
            return {"Needs controller 2", Tone::Warn};
        }
        if (!c.dayRule)
            return {"No day rule", Tone::Ok};
        if (active(s.noday))
            return {"Day rule skipped", Tone::Ok};
        if (s.noday == Switch::Failed)
            return {"noday failed: see log", Tone::Bad};
        return {"Needs Day 367", Tone::Warn};
    }
    // The line under the progress: GT4's rule, or why the buttons do not light.
    inline std::string hint(bool notFollowable)
    {
        return notFollowable ? "Typed on controller 2: not followed." : "Wrong button: starts over from 1.";
    }

    // GT4 keeps one count per code for every controller (craft_index, sSecretIndex); each controller's
    // presses are new presses against that controller's own previous sample. Ports 0 and 1.
    class PadFollower
    {
    public:
        using Event = gt4cheats::Follower::Event;
        // A new code (or none); the buttons held now on each port are not new presses.
        void reset(const gt4cheats::Code *code, const std::array<uint16_t, 2> &held)
        {
            follower_.reset(code);
            previous_ = held;
        }
        // One pad sample of `port` (buttons active high).
        Event sample(int port, uint16_t held, const Switches &s)
        {
            if (port < 0 || port > 1)
                return Event::None;
            const uint16_t before = previous_[size_t(port)];
            previous_[size_t(port)] = held;
            if (!follower_.code() || !takesPort(*follower_.code(), port, s))
                return Event::None;
            follower_.prime(before);
            return follower_.sample(held);
        }
        // One key event, as if pressed (test pictures).
        Event key(uint16_t button) { return follower_.key(button); }
        const gt4cheats::Code *code() const { return follower_.code(); }
        uint8_t progress() const { return follower_.progress(); }

    private:
        gt4cheats::Follower follower_;
        std::array<uint16_t, 2> previous_{};
    };
}
