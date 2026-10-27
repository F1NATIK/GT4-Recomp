#include "Gt4Pad2.h"

// GT4 player 2 (PS2 controller port 2): see Gt4Pad2.h and analysis/phase5b/player2.
// GT3 builds compile nothing here (they have no second DBCMAN port).

#if defined(PS2X_GAME_GT4)

#include "runtime/gt4_pad2_rules.h"
#include "runtime/ps2_pad_rumble.h"
#if !defined(GT4_STOCK_BUILD)
#include "runtime/ps2_pad_replay.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#if defined(PS2X_INPUT_GLFW)
#include "ps2_host_backend.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

namespace gt4pad2
{
    namespace
    {
        namespace rules = gt4pad2rules;

        // Shared with DBCMAN's thread (readState) under `mutex`: the settings (written once, by
        // configure), the test replay and the 32 bytes poll() publishes. The mutex is held only
        // to copy these, never across a device read or an XInput call.
        struct State
        {
            std::mutex mutex;
            bool configured = false;
            std::string text; // the start-up lines
            rules::Choice choice;
            ps2_pad::InputProfile profile = ps2_pad::InputProfile::defaults();
            uint32_t adapterVidPid = 0; // the wheel adapter's device (PS2X_WHEEL_DEVICE vvvv:pppp)
#if !defined(GT4_STOCK_BUILD)
            // GT4_PAD2_REPLAY (DBCMAN's thread only)
            bool replay = false;
            PSPadReplay pads;
            std::vector<std::pair<uint64_t, uint64_t>> unplug; // [begin, end)
            uint32_t lastPressed = 0x10000;
            int lastPlugged = -1;
#endif
            // published by poll()
            bool present = false;
            std::array<uint8_t, 32> snapshot{};
        };

        // The window thread's own state (poll, stopRumble): never locked. configure resets it
        // before any poll gets past its locked first step.
        struct Window
        {
            ps2_pad::MapState map{{}, false}; // player 2's own hysteresis; the right stick stays player 1's
            // player 2's controller
            int joystick = -1;
            std::string guid, why;
            bool reported = false;
            uint32_t skippedSlots = 0; // PS2X_INPUT2 device slots 1..3 left out: they point at player 1's joysticks
            // Automatic: a controller that has not joined yet
            uint64_t frame = 0; // polls
            int candidate = -1;
            std::string candidateGuid;
            int64_t candidateSince = 0;
            uint32_t candidateButtons = 0;
            int64_t pressAt = -1;
            bool refusedLogged = false;
            int64_t player1ActiveAt = INT64_MIN / 2;
            uint32_t tracked = 0; // player 1's joysticks with a previous state in player1Prev
            std::array<ps2_pad::DeviceState, rules::Joysticks> player1Prev{};
            // vibration: player 2's XInput pad
            std::string inventoryKey, indexKey;
            unsigned xinputMask = 0;
            int confirmedIndex = -1;
            bool waitLogged = false;
            int rumbleIndex = -1;
            uint32_t applied = 0;
            double retryAt = 0;
        };

        constexpr uint32_t Unsent = 0xffffffffu; // a newly claimed pad always gets one send
        constexpr int64_t JoinWindow = 6;        // polls (~0.1 s) around a joining press with no change on player 1's

        State &state()
        {
            static State s;
            return s;
        }

        Window &window()
        {
            static Window w;
            return w;
        }

        std::atomic<int> g_claimed{-1};
        std::atomic<uint32_t> g_actuators{0};
        std::function<void(const std::string &)> g_sink;
#if !defined(GT4_STOCK_BUILD)
        std::function<void()> g_readHook;
#endif

        void say(const std::string &line)
        {
            std::fprintf(stderr, "%s\n", line.c_str());
            if (g_sink)
                g_sink(line);
        }

#if defined(GT4_STOCK_BUILD)
        constexpr bool StockBuild = true;
#else
        constexpr bool StockBuild = false;
#endif

        std::string firstToken(const std::string &row)
        {
            std::istringstream in(row.substr(0, row.find('#')));
            std::string t;
            in >> t;
            return t;
        }

#if !defined(GT4_STOCK_BUILD)
        // GT3_PAD_REPLAY's lines plus "begin end unplug". The unplug lines become blank
        // lines for PSPadReplay, so its error line numbers stay the file's.
        bool loadReplay(State &s, const std::string &path, std::string &error)
        {
            std::ifstream file(path);
            if (!file)
            {
                error = "cannot open " + path;
                return false;
            }
            std::string line, rest;
            unsigned number = 0;
            bool anyPads = false;
            std::vector<std::pair<uint64_t, uint64_t>> unplug;
            while (std::getline(file, line))
            {
                ++number;
                std::istringstream row(line.substr(0, line.find('#')));
                std::vector<std::string> t;
                for (std::string w; row >> w;)
                    t.push_back(w);
                if (t.size() == 3 && t[2] == "unplug")
                {
                    char *e1 = nullptr, *e2 = nullptr;
                    const unsigned long long b = std::strtoull(t[0].c_str(), &e1, 10), e = std::strtoull(t[1].c_str(), &e2, 10);
                    if (*e1 || *e2 || b >= e || e > 1000000 || (!unplug.empty() && b < unplug.back().second))
                    {
                        error = "Invalid unplug line " + std::to_string(number);
                        return false;
                    }
                    unplug.emplace_back(b, e);
                    rest += "\n";
                    continue;
                }
                anyPads = anyPads || !t.empty();
                rest += line + "\n";
            }
            PSPadReplay pads;
            if (anyPads)
            {
                std::istringstream in(rest);
                if (!pads.load(in, error))
                    return false;
            }
            else if (unplug.empty())
            {
                error = "the replay has no lines";
                return false;
            }
            s.pads = pads;
            s.unplug = std::move(unplug);
            s.replay = true;
            return true;
        }
#endif

        void configureLocked(State &s, const EnvLookup &env, bool stock)
        {
            s.configured = true;
            s.text.clear();
            s.choice = {};
            s.profile = ps2_pad::InputProfile::defaults();
            s.adapterVidPid = 0;
#if !defined(GT4_STOCK_BUILD)
            s.replay = false;
            s.pads = PSPadReplay{};
            s.unplug.clear();
            s.lastPressed = 0x10000;
            s.lastPlugged = -1;
#endif
            s.present = false;
            s.snapshot = {};
            window() = Window{};
            g_claimed.store(-1);
            g_actuators.store(0);

            std::vector<std::string> lines;
            auto get = [&](const char *key) -> std::string {
                const char *v = env ? env(key) : nullptr;
                return v ? v : "";
            };
            std::string error;
            const std::string choice = get("GT4_PAD2");
            if (!rules::parseChoice(choice, s.choice, error))
                lines.push_back(error + "; port 2 stays empty (off)");
            std::string mapping = "default mapping";
            if (const std::string rows = get("PS2X_INPUT2"); !rows.empty())
            {
                std::string text = rows;
                std::replace(text.begin(), text.end(), ';', '\n');
                std::istringstream in(text);
                ps2_pad::InputProfile p;
                if (ps2_pad::readProfile(in, p, error))
                {
                    s.profile = p;
                    mapping = "own mapping from PS2X_INPUT2";
                    std::istringstream again(text);
                    for (std::string row; std::getline(again, row);)
                        if (firstToken(row) == "keyboard")
                            mapping += " (its keyboard row is ignored: the keyboard stays player 1's)";
                }
                else
                    mapping = "PS2X_INPUT2: " + error + " (rows count from 1, split at ';'); default mapping";
            }
            s.profile.keyboardFallback = false;
            if (const std::string wheel = get("PS2X_WHEEL_DEVICE"); wheel.size() == 9 && wheel[4] == ':')
            {
                const std::string vendor = wheel.substr(0, 4), product = wheel.substr(5);
                char *e1 = nullptr, *e2 = nullptr;
                const unsigned long v = std::strtoul(vendor.c_str(), &e1, 16), p = std::strtoul(product.c_str(), &e2, 16);
                if (!*e1 && !*e2)
                    s.adapterVidPid = static_cast<uint32_t>((v << 16) | p);
            }
            std::string head = "[GT4/PAD2] player 2 (controller port 2): " + rules::describe(s.choice) +
                               (rules::trim(choice).empty() ? " (GT4_PAD2 not set)" : "") + ", " + mapping;
            if (const std::string port = get("PS2X_WHEEL_PORT"); port == "2")
                head += "; the wheel is player 2's (PS2X_WHEEL_PORT=2)";
            if (const std::string path = get("GT4_PAD2_REPLAY"); !path.empty())
            {
#if defined(GT4_STOCK_BUILD)
                (void)stock;
                lines.push_back("GT4_PAD2_REPLAY ignored (a test switch, compiled out of the stock build)");
#else
                if (stock)
                    lines.push_back("GT4_PAD2_REPLAY ignored (a test switch, compiled out of the stock build)");
                else if (loadReplay(s, path, error))
                    lines.push_back("replay " + path + " (" + std::to_string(s.unplug.size()) +
                                    " unplug line(s)): port 2 is the replay's; host controllers are not used for port 2");
                else
                    lines.push_back("GT4_PAD2_REPLAY " + path + " NOT loaded: " + error);
#endif
            }
            s.text = head;
            for (const auto &l : lines)
                s.text += "\n[GT4/PAD2] " + l;
            std::istringstream out(s.text);
            for (std::string l; std::getline(out, l);)
                say(l);
        }

        void ensure(State &s)
        {
            if (!s.configured)
                configureLocked(s, [](const char *k) { return std::getenv(k); }, StockBuild);
        }

        // Device slots a profile's bindings use, as bits.
        uint32_t usedSlots(const ps2_pad::InputProfile &p)
        {
            uint32_t m = 0;
            auto device = [&](int d) {
                if (d >= 0 && d < 4)
                    m |= 1u << d;
            };
            for (const auto &b : p.buttons)
                if (b.button >= 0)
                    device(b.device);
            for (const auto &b : p.wheelButtons)
                if (b.button >= 0)
                    device(b.device);
            for (const auto &a : p.sticks)
                if (a.axis >= 0)
                    device(a.device);
            for (const auto &a : p.pressures)
                if (a.axis >= 0)
                    device(a.device);
            for (const auto &halves : p.stickHalves)
                for (const auto &a : halves)
                    if (a.axis >= 0)
                        device(a.device);
            if (p.wheelSteer.axis >= 0)
                device(p.wheelSteer.device);
            for (const auto &pedal : p.pedals)
                if (pedal.axis.axis >= 0)
                    device(pedal.axis.device);
            if (ps2_pad::pedalsOnTriggers())
                device(0);
            return m;
        }

        // Joysticks player 1's profile reads now, as bits.
        uint32_t player1Joysticks(const ps2_pad::InputProfile &p, const ps2_pad::DeviceInventory &ids)
        {
            uint32_t mask = 0;
            const uint32_t used = usedSlots(p);
            for (int slot = 0; slot < 4; ++slot)
                if ((used >> slot) & 1u)
                    if (const int j = ps2_pad::resolveSource(p.sources[slot], ids); j >= 0 && j < 32)
                        mask |= 1u << j;
            return mask;
        }

#if !defined(GT4_STOCK_BUILD)
        void neutral(std::array<uint8_t, 32> &packet)
        {
            packet.fill(0);
            packet[0] = 1;
            packet[1] = 0x79;
            packet[2] = packet[3] = 0xff;
            std::fill(packet.begin() + 4, packet.begin() + 8, uint8_t(0x80));
        }
#endif

        using ReadFn = std::function<ps2_pad::DeviceState(const ps2_pad::DeviceSource &, int)>;

        // raylib gamepad buttons that let a controller join (Automatic): the d-pad (1..4), the
        // face buttons (5..8), L1 (9), R1 (11), Select (13), Start (15) and the stick clicks
        // (16, 17); not the analog L2 / R2 (10, 12) and not the guide button (14).
        uint32_t joinButtons(const ps2_pad::DeviceState &d)
        {
            uint32_t m = 0;
            if (d.connected)
                for (int b = 1; b <= 17; ++b)
                    if (b != 10 && b != 12 && b != 14 && d.buttons[static_cast<size_t>(b)])
                        m |= 1u << b;
            return m;
        }

        // Did any of player 1's joysticks change this poll (a button, or an axis by more than a
        // quarter)? Read raw, whatever player 1's profile reads, so a mirrored press shows.
        bool player1Moved(Window &w, uint32_t player1, const ReadFn &read)
        {
            bool moved = false;
            uint32_t tracked = 0;
            for (int j = 0; j < rules::Joysticks; ++j)
            {
                if (!((player1 >> j) & 1u))
                    continue;
                const ps2_pad::DeviceState d = read({true, j, ""}, j);
                if (!d.connected)
                    continue;
                auto &prev = w.player1Prev[static_cast<size_t>(j)];
                if ((w.tracked >> j) & 1u)
                {
                    moved = moved || d.buttons != prev.buttons;
                    for (size_t a = 0; a < d.axes.size(); ++a)
                        moved = moved || std::fabs(d.axes[a] - prev.axes[a]) > 0.25f;
                }
                prev = d;
                tracked |= 1u << j;
            }
            w.tracked = tracked;
            return moved;
        }

        // Automatic: does `pick` (not player 2's yet) join now? It joins JoinWindow polls after a
        // button press of its own, when player 1's joysticks showed no change from JoinWindow polls
        // before that press to now. A controller that mirrors player 1's (the same pad seen twice)
        // always changes together with player 1's, so it never joins. A button held when the
        // candidate appears, or pressed in its first JoinWindow polls, does not count.
        bool joins(Window &w, int pick, const std::string &guid, const std::string &name, const ReadFn &read, bool moved)
        {
            const auto now = static_cast<int64_t>(w.frame);
            if (moved)
                w.player1ActiveAt = now;
            const uint32_t buttons = joinButtons(read({false, pick, ""}, pick));
            if (pick != w.candidate || guid != w.candidateGuid)
            {
                w.candidate = pick;
                w.candidateGuid = guid;
                w.candidateSince = now;
                w.candidateButtons = buttons;
                w.pressAt = -1;
                w.refusedLogged = false;
                return false;
            }
            if ((buttons & ~w.candidateButtons) && w.pressAt < 0 && now >= w.candidateSince + JoinWindow)
                w.pressAt = now;
            w.candidateButtons = buttons;
            if (w.pressAt < 0)
                return false;
            if (w.player1ActiveAt >= w.pressAt - JoinWindow)
            {
                if (!w.refusedLogged)
                    say("[GT4/PAD2] " + name + " (joystick " + std::to_string(pick) +
                        "): its press came with input on player 1's controller, so it did not join (a controller "
                        "that mirrors player 1's never joins); press a button on it alone");
                w.refusedLogged = true;
                w.pressAt = -1;
                return false;
            }
            return now >= w.pressAt + JoinWindow;
        }

        // Does GLFW's raw view of an XInput joystick show exactly this XInput pad's input? GLFW
        // (win32_joystick.c) lists A B X Y LB RB Back Start LS RS as buttons 0..9, the d-pad hat
        // as buttons 10..13 (up right down left) and LX -LY RX -RY LT RT as axes 0..5, scaled as
        // below. Two physical pads practically never read the same to the last stick count.
        bool sameInput(const ps2_pad::DeviceState &raw, const ps2_pad::XInputPadState &p)
        {
            static constexpr uint16_t Bits[14] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020,
                                                  0x0010, 0x0040, 0x0080, 0x0001, 0x0008, 0x0002, 0x0004};
            for (size_t i = 0; i < 14; ++i)
                if (raw.buttons[i] != ((p.buttons & Bits[i]) != 0))
                    return false;
            if (raw.axisCount < 6)
                return false;
            const float want[6] = {(p.lx + 0.5f) / 32767.5f, -(p.ly + 0.5f) / 32767.5f, (p.rx + 0.5f) / 32767.5f,
                                   -(p.ry + 0.5f) / 32767.5f, p.lt / 127.5f - 1.f,      p.rt / 127.5f - 1.f};
            for (size_t a = 0; a < 6; ++a)
                if (std::fabs(raw.axes[a] - want[a]) > 1e-4f)
                    return false;
            return true;
        }

        // Player 2's XInput user index for its Xbox-type controller, or -1 (none, or not known yet).
        // Empty XInput slots are slow to ask, so all four are asked only when the joysticks change;
        // the matching below asks only the connected ones.
        int xinputTarget(Window &w, const rules::Inventory &inv, int pick, const std::string &guid, const std::string &inventoryKey,
                         const ReadFn &read, const XInputLayer &x)
        {
            if (!x.pads || !rules::isXInputGuid(guid))
                return -1;
            const std::string indexKey = std::to_string(pick) + ":" + guid;
            if (indexKey != w.indexKey)
            {
                w.indexKey = indexKey;
                w.confirmedIndex = -1;
                w.waitLogged = false;
                w.inventoryKey.clear();
            }
            if (inventoryKey != w.inventoryKey)
            {
                w.inventoryKey = inventoryKey;
                w.xinputMask = x.pads(0xfu, nullptr);
                if (w.confirmedIndex >= 0 && !((w.xinputMask >> w.confirmedIndex) & 1u))
                    w.confirmedIndex = -1;
            }
            int xbox = 0;
            for (const auto &j : inv)
                if (j.connected && rules::isXInputGuid(j.guid))
                    ++xbox;
            if (xbox <= 1)
                return rules::xinputIndex(inv, pick, w.xinputMask); // the only one: the rank cannot be wrong
            if (w.confirmedIndex >= 0)
                return w.confirmedIndex;
            // Two or more: the one connected XInput pad that reads exactly as player 2's joystick,
            // while XInput sees as many pads as GLFW and that reading is not the bare rest state
            // (two idle virtual pads both read all zero).
            const ps2_pad::DeviceState raw = read({true, pick, ""}, pick);
            ps2_pad::XInputPadState pads[4] = {};
            const unsigned connected = raw.connected ? x.pads(w.xinputMask, pads) : 0u;
            int found = -1, count = 0, seen = 0;
            for (int i = 0; i < 4; ++i)
            {
                if (!((connected >> i) & 1u))
                    continue;
                ++seen;
                if (sameInput(raw, pads[i]))
                {
                    found = i;
                    ++count;
                }
            }
            auto atRest = [](const ps2_pad::XInputPadState &p) {
                auto still = [](int v) { return v >= -1 && v <= 0; };
                return !p.buttons && still(p.lx) && still(p.ly) && still(p.rx) && still(p.ry) && !p.lt && !p.rt;
            };
            if (count == 1 && seen == xbox && !atRest(pads[found]))
            {
                w.confirmedIndex = found;
                say("[GT4/PAD2] player 2's vibration: XInput pad " + std::to_string(found) + " (it reads as player 2's controller)");
                return found;
            }
            if (!w.waitLogged)
                say("[GT4/PAD2] player 2's vibration waits until one Xbox-type pad reads as player 2's (" + std::to_string(xbox) +
                    " Xbox-type controllers; PS2X_INPUT2 \"rumble xinput <n>\" names one)");
            w.waitLogged = true;
            return -1;
        }

        void applyRumble(Window &w, const ps2_pad::InputProfile &profile, int target, const XInputLayer &x)
        {
            g_claimed.store(target);
            if (target != w.rumbleIndex)
            {
                if (w.rumbleIndex >= 0 && w.applied && x.rumble)
                    x.rumble(w.rumbleIndex, 0, 0);
                w.rumbleIndex = target;
                w.applied = Unsent; // one send even for (0, 0): it stops what player 1 may have left on this pad
                w.retryAt = 0;
            }
            if (w.rumbleIndex < 0 || !x.rumble)
                return;
            const uint32_t wanted = profile.rumble && ps2_pad::vibrationEnabled() ? g_actuators.load(std::memory_order_relaxed) : 0u;
            if (wanted == w.applied)
                return;
            const double now = x.now ? x.now() : 0.0;
            if (now < w.retryAt)
                return;
            // DualShock 2: small motor on/off, large 0..255; XInput low frequency = large.
            const auto low = static_cast<uint16_t>(std::lround((wanted & 0xff) / 255.f * profile.rumbleLarge * 65535.f));
            const auto high = static_cast<uint16_t>((wanted & 0x100) ? std::lround(profile.rumbleSmall * 65535.f) : 0);
            if (x.rumble(w.rumbleIndex, low, high))
                w.applied = wanted;
            else if (!wanted)
                w.applied = 0; // a stop for a pad that is not there: nothing to stop
            else
                w.retryAt = now + 1.0; // the pad went away: once a second
        }

        void pollCore(const rules::Inventory &inv, const ps2_pad::DeviceInventory &ids, const ReadFn &read,
                      const ps2_pad::InputProfile &player1, const XInputLayer &x)
        {
            State &s = state();
            {
                std::lock_guard lock(s.mutex);
                ensure(s);
#if !defined(GT4_STOCK_BUILD)
                if (s.replay)
                    return; // port 2 is the replay's; nothing is read or driven
#endif
            }
            // The settings are fixed from here on (configure has run) and the rest is this thread's,
            // so no lock is held across the device reads and XInput calls below.
            Window &w = window();
            ++w.frame;
            const uint32_t p1 = player1Joysticks(player1, ids);
            std::string why;
            int pick = rules::choose(s.choice, inv, p1, w.joystick, w.guid, s.adapterVidPid, &why);
            std::string guid = pick >= 0 ? inv[pick].guid : std::string();
            if (s.choice.mode == rules::Mode::Automatic && pick >= 0 && (pick != w.joystick || guid != w.guid))
            {
                // A controller new to player 2: it joins with a press of its own.
                const bool moved = player1Moved(w, p1, read);
                if (!joins(w, pick, guid, inv[pick].name, read, moved))
                {
                    why = "waiting for a button press on " + inv[pick].name + " (joystick " + std::to_string(pick) + ")";
                    pick = -1;
                    guid.clear();
                }
            }
            else
            {
                w.candidate = -1;
                w.candidateGuid.clear();
                w.pressAt = -1;
                w.tracked = 0;
            }
            if (pick != w.joystick || guid != w.guid || (pick < 0 && why != w.why) || !w.reported)
            {
                say(pick >= 0 ? "[GT4/PAD2] player 2: " + inv[pick].name + " (joystick " + std::to_string(pick) + ", guid " + guid + ")"
                              : "[GT4/PAD2] player 2: none (" + why + ")");
                w.joystick = pick;
                w.guid = guid;
                w.why = why;
                w.reported = true;
            }
            ps2_pad::Devices devices{};
            uint32_t skipped = 0;
            if (pick >= 0)
            {
                devices[0] = read({false, pick, ""}, pick);
                const uint32_t used = usedSlots(s.profile);
                for (int slot = 1; slot < 4; ++slot)
                {
                    if (!((used >> slot) & 1u))
                        continue;
                    const int j = ps2_pad::resolveSource(s.profile.sources[slot], ids);
                    if (j < 0)
                        continue;
                    if (j < 32 && ((p1 >> j) & 1u))
                    {
                        skipped |= 1u << slot; // player 1's controller or wheel: never read for player 2
                        continue;
                    }
                    devices[slot] = read(s.profile.sources[slot], j);
                }
            }
            if (skipped != w.skippedSlots)
            {
                std::string list;
                for (int slot = 1; slot < 4; ++slot)
                    if ((skipped >> slot) & 1u)
                        list += " " + std::to_string(slot);
                if (skipped)
                    say("[GT4/PAD2] PS2X_INPUT2 device slot(s)" + list + " point at player 1's controller: not read for player 2");
                w.skippedSlots = skipped;
            }
            const bool present = pick >= 0 && devices[0].connected;
            std::array<uint8_t, 32> snapshot{};
            if (present)
                snapshot = ps2_pad::mapDevices(s.profile, devices, w.map);
            else
                w.map.analogHeld = {};
            {
                std::lock_guard lock(s.mutex); // publish: 32 bytes and a flag
                s.present = present;
                if (present)
                    s.snapshot = snapshot;
            }
            // Vibration (XInput), with no lock held.
            int target = -1;
            if (present)
            {
                std::string key;
                for (const auto &j : inv)
                    key += (j.connected ? j.guid : std::string("-")) + ",";
                target = s.profile.rumbleXInput >= 0 ? s.profile.rumbleXInput : xinputTarget(w, inv, pick, guid, key, read, x);
            }
            else
                w.indexKey.clear();
            applyRumble(w, s.profile, target, x);
        }

        void stopCore(const XInputLayer &x)
        {
            Window &w = window();
            if (w.rumbleIndex >= 0 && w.applied && x.rumble)
                x.rumble(w.rumbleIndex, 0, 0);
            w.applied = 0;
        }
    }

    std::string configure()
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ensure(s);
        return s.text;
    }

    void poll(const ps2_pad::DeviceInventory &inventory, const ps2_pad::InputProfile &player1)
    {
#if defined(PS2X_INPUT_GLFW)
        rules::Inventory inv;
        for (int j = 0; j < rules::Joysticks; ++j)
        {
            inv[j].connected = inventory[j].connected;
            inv[j].mapped = inventory[j].mapped;
            inv[j].guid = inventory[j].guid;
            if (inventory[j].connected)
                if (const char *name = glfwGetJoystickName(j))
                    inv[j].name = name;
        }
        // The same reads as PSPadBackend::poll makes for player 1's devices (ps2_pad.cpp): a fix
        // there belongs here too.
        const ReadFn read = [](const ps2_pad::DeviceSource &source, int joystick) {
            ps2_pad::DeviceState d;
            if (source.raw)
            {
                d.connected = glfwJoystickPresent(joystick) == GLFW_TRUE;
                if (!d.connected)
                    return d;
                int axisCount = 0, buttonCount = 0;
                const float *axes = glfwGetJoystickAxes(joystick, &axisCount);
                const unsigned char *buttons = glfwGetJoystickButtons(joystick, &buttonCount);
                d.axisCount = axes ? std::clamp(axisCount, 0, 8) : 0;
                for (int axis = 0; axis < d.axisCount; ++axis)
                    d.axes[axis] = axes[axis];
                if (buttons)
                    for (int button = 0; button < std::clamp(buttonCount, 0, 128); ++button)
                        d.buttons[button] = buttons[button] == GLFW_PRESS;
                return d;
            }
            d.connected = joystick >= 0 && joystick < 4 && IsGamepadAvailable(joystick) && glfwJoystickIsGamepad(joystick);
            if (!d.connected)
                return d;
            d.axisCount = std::clamp(GetGamepadAxisCount(joystick), 0, 8);
            for (int axis = 0; axis < d.axisCount; ++axis)
                d.axes[axis] = GetGamepadAxisMovement(joystick, axis);
            for (int button = 0; button < 32; ++button)
                d.buttons[button] = IsGamepadButtonDown(joystick, button);
            return d;
        };
        static const XInputLayer xinput{ps2_pad::readXInputPads, ps2_pad::setXInputRumble, [] { return GetTime(); }};
        pollCore(inv, inventory, read, player1, xinput);
#else
        (void)inventory;
        (void)player1;
#endif
    }

    bool readState(uint64_t vsync, std::array<uint8_t, 32> &packet)
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        ensure(s);
#if !defined(GT4_STOCK_BUILD)
        if (s.replay)
        {
            const bool unplugged = std::any_of(s.unplug.begin(), s.unplug.end(),
                                               [vsync](const auto &u) { return vsync >= u.first && vsync < u.second; });
            if (s.lastPlugged != (unplugged ? 0 : 1))
            {
                say("[GT4/PAD2] replay: port 2 " + std::string(unplugged ? "unplugged" : "plugged in") + " at vsync " + std::to_string(vsync));
                s.lastPlugged = unplugged ? 0 : 1;
            }
            if (unplugged)
                return false;
            if (!s.pads.sample(vsync, packet))
                neutral(packet);
            const uint32_t pressed = s.pads.enabled() ? s.pads.pressed(vsync) : 0u;
            if (pressed != s.lastPressed)
            {
                char text[96];
                std::snprintf(text, sizeof(text), "[GT4/PAD2] replay VSync=%llu pressed=%04x", static_cast<unsigned long long>(vsync), pressed);
                say(text);
                s.lastPressed = pressed;
            }
            return true;
        }
#else
        (void)vsync;
#endif
        if (s.choice.mode == rules::Mode::Off || !s.present)
            return false;
        packet = s.snapshot;
        return true;
    }

    void setActuators(bool smallMotor, uint8_t large, uint64_t vsync)
    {
        g_actuators.store((smallMotor ? 0x100u : 0u) | large, std::memory_order_relaxed);
        static const bool traceAll = std::getenv("GT3_RUMBLE_LOG") != nullptr;
        static std::atomic<bool> first{true};
        if (traceAll || ((smallMotor || large) && first.exchange(false)))
            std::fprintf(stderr, "[input] DualShock 2 port 2 motors: small=%d large=%u (VSync %llu)\n", smallMotor ? 1 : 0,
                         static_cast<unsigned>(large), static_cast<unsigned long long>(vsync));
    }

    int claimedXInput() { return g_claimed.load(); }

    int player1XInputPad()
    {
        const int claimed = g_claimed.load();
        if (claimed < 0)
            return ps2_pad::firstXInputPad();
        const unsigned free = ps2_pad::connectedXInputPads() & ~(1u << claimed);
        for (int index = 0; index < 4; ++index)
            if ((free >> index) & 1u)
                return index;
        return -1;
    }

    void stopRumble() { stopCore({nullptr, ps2_pad::setXInputRumble, nullptr}); }

#if !defined(GT4_STOCK_BUILD)
    std::string configureForTest(const EnvLookup &env, bool stockBuild)
    {
        State &s = state();
        std::lock_guard lock(s.mutex);
        configureLocked(s, env, stockBuild);
        return s.text;
    }

    void pollForTest(const TestInventory &joysticks, const ps2_pad::InputProfile &player1, const XInputLayer &xinput)
    {
        rules::Inventory inv;
        ps2_pad::DeviceInventory ids{};
        for (int j = 0; j < rules::Joysticks; ++j)
        {
            inv[j] = {joysticks[j].connected, joysticks[j].mapped, joysticks[j].guid, joysticks[j].name};
            ids[j] = {joysticks[j].connected, joysticks[j].mapped, joysticks[j].guid};
        }
        const ReadFn read = [&joysticks](const ps2_pad::DeviceSource &source, int joystick) {
            if (g_readHook)
                g_readHook();
            const auto &j = joysticks[static_cast<size_t>(joystick)];
            ps2_pad::DeviceState d = source.raw && j.hasRaw ? j.raw : j.state;
            d.connected = j.connected && (source.raw || (joystick < 4 && j.mapped));
            return d;
        };
        pollCore(inv, ids, read, player1, xinput);
    }

    void setLogSinkForTest(std::function<void(const std::string &)> sink) { g_sink = std::move(sink); }

    void setDeviceReadHookForTest(std::function<void()> hook) { g_readHook = std::move(hook); }

    void stopRumbleForTest(const XInputLayer &xinput) { stopCore(xinput); }

    int joystickForTest() { return window().joystick; }
#endif
}

#endif // PS2X_GAME_GT4
