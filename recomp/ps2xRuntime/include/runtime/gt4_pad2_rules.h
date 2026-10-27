#pragma once

// GT4 player 2 (PS2 controller port 2; analysis/phase5b/player2): how GT4.exe picks
// player 2's controller (GT4_PAD2) and finds its XInput index for vibration.
// Header-only and free of raylib / GLFW, so the GT4 Stock Launcher can show what
// "Automatic" picks with the same rule. The runtime side is Kernel/Stubs/Gt4Pad2.h.
//
// GLFW (3.4, raylib's) numbers joysticks 0..15: on Windows XInput pads first in
// user-index order, then DirectInput devices; a new device takes the lowest free
// number (win32_joystick.c, input.c). raylib reads mapped gamepads 0..3 only.
// GUIDs: XInput "78696e707574" + subtype + zeros (every Xbox-type pad of a subtype
// has the same one); DirectInput with vendor / product "03000000vvvv0000pppp...".

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace gt4pad2rules
{
    enum class Mode
    {
        Automatic, // the lowest-numbered mapped controller that is not player 1's (and not a wheel)
        Off,       // port 2 empty (the default: player 2 is turned on in the launcher)
        Joystick,  // a GLFW joystick number 0..3
        Guid,      // a GLFW GUID (32 hex)
    };

    struct Choice
    {
        Mode mode = Mode::Off;
        int joystick = -1;
        std::string guid; // lower case
    };

    constexpr int Joysticks = 16; // GLFW_JOYSTICK_LAST + 1
    constexpr int Gamepads = 4;   // raylib MAX_GAMEPADS: mapped controllers it reads

    struct Joystick
    {
        bool connected = false;
        bool mapped = false; // GLFW has a gamepad mapping for it (glfwJoystickIsGamepad)
        std::string guid;
        std::string name;
    };
    using Inventory = std::array<Joystick, Joysticks>;

    inline std::string lower(std::string_view s)
    {
        std::string out(s);
        for (char &c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    inline std::string trim(std::string_view s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string_view::npos)
            return {};
        return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
    }

    // GT4_PAD2 = auto | off | joystick:<0..3> | guid:<32 hex>. Empty (or absent) = off: port 2
    // stays empty until player 2 is turned on, so no second controller becomes player 2 by itself.
    inline bool parseChoice(std::string_view text, Choice &out, std::string &error)
    {
        const std::string t = lower(trim(text));
        Choice c;
        if (t == "auto" || t == "automatic")
            c.mode = Mode::Automatic;
        else if (t.empty() || t == "off" || t == "0" || t == "none")
            c.mode = Mode::Off;
        else if (t.rfind("joystick:", 0) == 0 && t.size() == 10 && t[9] >= '0' && t[9] <= '3')
        {
            c.mode = Mode::Joystick;
            c.joystick = t[9] - '0';
        }
        else if (t.rfind("guid:", 0) == 0 && t.size() == 37 &&
                 t.find_first_not_of("0123456789abcdef", 5) == std::string::npos)
        {
            c.mode = Mode::Guid;
            c.guid = t.substr(5);
        }
        else
        {
            error = "GT4_PAD2=" + std::string(text) + " not understood (auto, off, joystick:<0..3>, guid:<32 hex>)";
            return false;
        }
        out = c;
        error.clear();
        return true;
    }

    inline std::string describe(const Choice &c)
    {
        switch (c.mode)
        {
        case Mode::Automatic: return "auto";
        case Mode::Off: return "off";
        case Mode::Joystick: return "joystick:" + std::to_string(c.joystick);
        case Mode::Guid: return "guid:" + c.guid;
        }
        return "?";
    }

    inline bool isXInputGuid(std::string_view guid) { return lower(guid.substr(0, 12)) == "78696e707574"; }

    inline int hexByte(std::string_view s, size_t at)
    {
        if (s.size() < at + 2)
            return -1;
        int v = 0;
        for (size_t i = at; i < at + 2; ++i)
        {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -256);
        }
        return v >= 0 ? v : -1;
    }

    // (vendor << 16) | product from a DirectInput GUID with vendor / product, else 0.
    inline uint32_t vidPid(std::string_view guid)
    {
        if (guid.size() != 32 || lower(guid.substr(0, 8)) != "03000000")
            return 0;
        const int v0 = hexByte(guid, 8), v1 = hexByte(guid, 10), p0 = hexByte(guid, 16), p1 = hexByte(guid, 18);
        if (v0 < 0 || v1 < 0 || p0 < 0 || p1 < 0)
            return 0;
        return (static_cast<uint32_t>(v0 | (v1 << 8)) << 16) | static_cast<uint32_t>(p0 | (p1 << 8));
    }

    // A wheel, pedal set or wheel base is never player 2's controller: it is the
    // wheel adapter's (LGDEV, PS2X_WHEEL) or player 1's own profile rows. The vendor /
    // product lists are the common ones [I]; names catch the rest.
    inline bool isWheelOrPedals(const Joystick &j, uint32_t adapterVidPid = 0)
    {
        if (isXInputGuid(j.guid))
            return hexByte(j.guid, 12) == 0x02; // XINPUT_DEVSUBTYPE_WHEEL
        const uint32_t id = vidPid(j.guid);
        if (id && id == adapterVidPid)
            return true;
        const uint32_t vendor = id >> 16, product = id & 0xffffu;
        if (vendor == 0x0eb7 || vendor == 0x044f || vendor == 0x346e) // Fanatec, Thrustmaster, MOZA
            return true;
        if (vendor == 0x046d) // Logitech wheels (its gamepads are not listed)
            for (uint32_t p : {0xc293u, 0xc294u, 0xc295u, 0xc298u, 0xc299u, 0xc29au, 0xc29bu, 0xc24fu, 0xc260u, 0xc261u,
                               0xc262u, 0xc266u, 0xc267u, 0xc268u, 0xc26du, 0xc26eu, 0xca03u, 0xca04u})
                if (product == p)
                    return true;
        const std::string n = lower(j.name);
        for (const char *word : {"wheel", "pedal", "racing", "driving force", "dfgt", "g25", "g27", "g29", "g920", "g923",
                                 "fanatec", "podium", "clubsport", "thrustmaster", "moza"})
            if (n.find(word) != std::string::npos)
                return true;
        return false;
    }

    // Player 2's joystick, or -1 (port 2 empty). `player1` = bits of the joysticks
    // player 1's profile uses now (0: player 1 has no connected controller, and then
    // automatic picks no new controller, so a lone controller is never made player 2).
    // `sticky` / `stickyGuid`: player 2's current device, kept while it stays connected,
    // also while player 1's is unplugged (as pad 2 on a PS2 when pad 1 is pulled out).
    // `why`, when given, gets a short reason for -1.
    inline int choose(const Choice &c, const Inventory &inv, uint32_t player1, int sticky, std::string_view stickyGuid,
                      uint32_t adapterVidPid = 0, std::string *why = nullptr)
    {
        auto usable = [&](int j) { return j >= 0 && j < Gamepads && inv[j].connected && inv[j].mapped; };
        auto mine = [&](int j) { return (player1 >> j) & 1u; };
        auto say = [&](const std::string &text) {
            if (why)
                *why = text;
            return -1;
        };
        switch (c.mode)
        {
        case Mode::Off:
            return say("off");
        case Mode::Joystick:
            if (!usable(c.joystick))
                return say("joystick " + std::to_string(c.joystick) + " is not a connected controller");
            if (mine(c.joystick))
                return say("joystick " + std::to_string(c.joystick) + " is player 1's");
            return c.joystick;
        case Mode::Guid:
        {
            int found = -1, count = 0;
            for (int j = 0; j < Gamepads; ++j)
                if (usable(j) && !mine(j) && lower(inv[j].guid) == c.guid)
                {
                    found = j;
                    ++count;
                }
            if (count == 1)
                return found;
            return say(count ? "more than one connected controller has that id" : "the chosen controller is not connected");
        }
        case Mode::Automatic:
        {
            if (usable(sticky) && !mine(sticky) && inv[sticky].guid == stickyGuid && !isWheelOrPedals(inv[sticky], adapterVidPid))
                return sticky;
            if (!player1)
                return say("player 1 has no controller");
            for (int j = 0; j < Gamepads; ++j)
                if (usable(j) && !mine(j) && !isWheelOrPedals(inv[j], adapterVidPid))
                    return j;
            return say("no second controller");
        }
        }
        return say("?");
    }

    // XInput user index of an Xbox-type joystick: GLFW hides it, so its rank among
    // the connected Xbox-type joysticks is taken as its rank among the connected
    // XInput indices (`connected`: bit n = index n). True for GLFW's detection order;
    // can be wrong after unusual replug orders [I], so GT4.exe uses it only while one
    // Xbox-type controller is connected; with more it matches a button press instead
    // (Gt4Pad2.cpp). PS2X_INPUT2 "rumble xinput <n>" overrides both. -1: not an
    // Xbox-type pad, or no matching index.
    inline int xinputIndex(const Inventory &inv, int joystick, unsigned connected)
    {
        if (joystick < 0 || joystick >= Joysticks || !inv[joystick].connected || !isXInputGuid(inv[joystick].guid))
            return -1;
        int rank = 0;
        for (int k = 0; k < joystick; ++k)
            if (inv[k].connected && isXInputGuid(inv[k].guid))
                ++rank;
        for (int index = 0; index < 4; ++index)
            if ((connected >> index) & 1u)
                if (rank-- == 0)
                    return index;
        return -1;
    }
}
