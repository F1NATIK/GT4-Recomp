#include "MiniTest.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "Stubs/Pad.h"
#include "runtime/ps2_pad.h"
#include "ps2_runtime.h"

#include <vector>
#include <cstdint>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <limits>


namespace
{
    constexpr uint32_t kPadDataAddr = 0x1000;

    constexpr uint16_t kPadBtnSelect = 1u << 0;
    constexpr uint16_t kPadBtnL3 = 1u << 1;
    constexpr uint16_t kPadBtnR3 = 1u << 2;
    constexpr uint16_t kPadBtnStart = 1u << 3;
    constexpr uint16_t kPadBtnUp = 1u << 4;
    constexpr uint16_t kPadBtnRight = 1u << 5;
    constexpr uint16_t kPadBtnDown = 1u << 6;
    constexpr uint16_t kPadBtnLeft = 1u << 7;
    constexpr uint16_t kPadBtnL2 = 1u << 8;
    constexpr uint16_t kPadBtnR2 = 1u << 9;
    constexpr uint16_t kPadBtnL1 = 1u << 10;
    constexpr uint16_t kPadBtnR1 = 1u << 11;
    constexpr uint16_t kPadBtnTriangle = 1u << 12;
    constexpr uint16_t kPadBtnCircle = 1u << 13;
    constexpr uint16_t kPadBtnCross = 1u << 14;
    constexpr uint16_t kPadBtnSquare = 1u << 15;

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void openPadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        setRegU32(ctx, 6, kPadDataAddr + 0x200u);
        ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
    }

    void closePadPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port = 0, uint32_t slot = 0)
    {
        setRegU32(ctx, 4, port);
        setRegU32(ctx, 5, slot);
        ps2_stubs::scePadPortClose(rdram.data(), &ctx, nullptr);
    }

    void runPadRead(R5900Context &ctx, std::vector<uint8_t> &rdram)
    {
        setRegU32(ctx, 4, 0u);
        setRegU32(ctx, 5, 0u);
        setRegU32(ctx, 6, kPadDataAddr); // a2
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
    }

    uint16_t readButtons(const std::vector<uint8_t> &rdram)
    {
        const uint8_t *data = rdram.data() + kPadDataAddr;
        return static_cast<uint16_t>(data[2] | (data[3] << 8));
    }
}

void register_pad_input_tests()
{
    MiniTest::Case("PadInput", [](TestCase &tc)
                   {
        tc.Run("pad buffers reject boundary overflow and failed open retains state", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE + 32, 0xa5);
            R5900Context ctx;
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);
            setRegU32(ctx, 4, 0); setRegU32(ctx, 5, 0);
            setRegU32(ctx, 6, PS2_RAM_SIZE - 16);
            ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), uint32_t(0), "short DMA destination rejected");
            ps2_stubs::scePadGetDmaStr(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), kPadDataAddr + 0x200u, "failed reopen retains previous DMA address");
            setRegU32(ctx, 6, 0x80000000u + PS2_RAM_SIZE - 16);
            ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), uint32_t(0), "aliased short read destination rejected");
            bool untouched = true;
            for (size_t i = PS2_RAM_SIZE - 16; i < rdram.size(); ++i) untouched &= rdram[i] == 0xa5;
            t.IsTrue(untouched, "rejected writes preserve destination and guard bytes");
            setRegU32(ctx, 6, PS2_RAM_SIZE - 32);
            ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), uint32_t(1), "exact-fit destination accepted");
            closePadPort(ctx, rdram);
            ps2_stubs::scePadGetDmaStr(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), uint32_t(0), "closed port drops DMA address");
            setRegU32(ctx, 6, kPadDataAddr);
            std::fill_n(rdram.data() + kPadDataAddr, 32, uint8_t(0xa5));
            ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
            t.Equals(getRegU32(&ctx, 2), uint32_t(0), "closed read fails");
            t.Equals(rdram[kPadDataAddr], uint8_t(0xa5), "closed read preserves destination");
        });

        tc.Run("GUID sources follow reconnects and reject duplicate models", [](TestCase &t)
               {
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            const std::string guid = "0123456789abcdef0123456789abcdef";
            std::istringstream config("source-guid 0 raw 0123456789ABCDEF0123456789ABCDEF\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "GUID profile loads");
            t.IsTrue(profile.sources[0].guid == guid, "GUID normalized");
            ps2_pad::DeviceInventory inventory{};
            inventory[2] = {true, false, guid};
            t.Equals(ps2_pad::resolveSource(profile.sources[0], inventory), 2, "unique raw model selected");
            inventory[2].connected = false; inventory[12] = {true, false, guid};
            t.Equals(ps2_pad::resolveSource(profile.sources[0], inventory), 12, "reconnect follows new index");
            inventory[2].connected = true;
            t.Equals(ps2_pad::resolveSource(profile.sources[0], inventory), -1, "duplicate models stay disconnected");
            inventory[2].connected = false;
            auto mapped = profile.sources[0]; mapped.raw = false;
            inventory[12].mapped = true;
            t.Equals(ps2_pad::resolveSource(mapped, inventory), -1, "mapped adapter cannot address high slots");
            inventory[12].connected = false; inventory[1] = {true, true, guid};
            t.Equals(ps2_pad::resolveSource(mapped, inventory), 1, "mapped GUID selects usable index");
            inventory[1].mapped = false;
            t.Equals(ps2_pad::resolveSource(mapped, inventory), -1, "unmapped source rejected");
            inventory[1].connected = false;
            t.Equals(ps2_pad::resolveSource(profile.sources[0], inventory), -1, "missing model stays disconnected");
            for (const char *bad : {"source-guid 0 raw short", "source-guid 4 raw 0123456789abcdef0123456789abcdef",
                "source-guid 0 raw z123456789abcdef0123456789abcdef", "source-guid 0 unknown 0123456789abcdef0123456789abcdef"})
            {
                std::istringstream invalid(bad);
                t.IsTrue(!ps2_pad::readProfile(invalid, profile, error), "invalid GUID selector rejected");
                t.IsTrue(profile.sources[0].guid == guid, "failure retains selector");
            }
            std::istringstream indexed("source-guid 0 raw " + guid + "\nsource 0 raw 3\n");
            t.IsTrue(ps2_pad::readProfile(indexed, profile, error), "index can replace GUID");
            t.IsTrue(profile.sources[0].guid.empty(), "index replacement clears GUID");
        });

        tc.Run("device sources select raw slots transactionally", [](TestCase &t)
               {
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream valid("source 0 raw 15\nsource 1 mapped 3\npressure cross 0 7 1 -1 0\n");
            t.IsTrue(ps2_pad::readProfile(valid, profile, error), "raw and mapped sources load");
            t.IsTrue(profile.sources[0].raw && !profile.sources[1].raw, "source modes retained");
            t.Equals(profile.sources[0].joystick, 15, "raw supports GLFW last slot");
            t.Equals(profile.sources[1].joystick, 3, "mapped source can differ from binding slot");
            for (const char *bad : {"source 4 raw 0", "source 0 raw 16", "source 0 raw -1",
                "source 0 mapped 4", "source 0 unknown 0", "source 0 raw 1 extra"})
            {
                std::istringstream invalid(bad);
                t.IsTrue(!ps2_pad::readProfile(invalid, profile, error), "invalid source rejected");
                t.Equals(profile.sources[0].joystick, 15, "invalid source retains profile");
            }
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 8;
            devices[0].axes[7] = 0;
            auto packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[14], uint8_t(128), "high raw axis preserves partial reversed pressure");
            devices[0].axisCount = 7;
            packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[14], uint8_t(0), "missing raw axis releases pressure");
        });

        tc.Run("profile calibrates separate devices and rejects invalid edits", [](TestCase &t)
               {
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream config("stick lx 0 0 -0.8 0.1 0.9 0.1\npressure cross 1 2 1 -1 0.05\nbutton start 1 9\nkeyboard 0\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "valid profile loads");
            ps2_pad::Devices devices{};
            devices[0].connected = devices[1].connected = true;
            devices[0].axisCount = devices[1].axisCount = 6;
            devices[0].axes[4] = devices[0].axes[5] = -1;
            devices[0].axes[0] = 0.1f;
            devices[1].axes[2] = 1.f;
            auto packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[6], uint8_t(128), "calibrated steering center");
            t.Equals(packet[14], uint8_t(0), "reversed pedal at rest");
            devices[0].axes[0] = -0.8f; devices[1].axes[2] = -1.f;
            devices[1].buttons[9] = true;
            packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[6], uint8_t(0), "steering minimum reaches byte zero");
            t.Equals(packet[14], uint8_t(255), "separate reversed pedal reaches full pressure");
            t.IsTrue((packet[2] & 8) == 0, "start uses remapped device/button");
            devices[0].axes[0] = 0.9f; devices[1].axes[2] = 0.f;
            packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[6], uint8_t(255), "steering maximum");
            t.Equals(packet[14], uint8_t(121), "pedal deadzone rescales partial pressure");
            for (const char *bad : {"stick lx 0 0 0 0 0 0", "pressure cross 1 2 1 -1 1", "button cross 4 7", "keyboard 2", "unknown x", "button cross 0 7 trailing"})
            {
                std::istringstream invalid(bad);
                t.IsTrue(!ps2_pad::readProfile(invalid, profile, error), "invalid configuration rejected");
                t.Equals(profile.pressures[6].device, 1, "failure retains prior profile");
            }
            devices[1].connected = false;
            packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[14], uint8_t(0), "disconnect releases mapped pedal");
            devices[0].axes[0] = std::numeric_limits<float>::quiet_NaN();
            packet = ps2_pad::mapDevices(profile, devices);
            t.Equals(packet[6], uint8_t(128), "nonfinite steering is neutral");
        });

        tc.Run("an analog trigger presses its button past 30% with hysteresis", [](TestCase &t)
               {
            // A finger resting on a trigger must not flicker the digital
            // button (each flicker is a new press in menus).
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream config("pressure cross 0 5 -1 1 0.02\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "trigger profile loads");
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes.fill(-1.f);
            const auto crossDown = [&](float trigger) {
                devices[0].axes[5] = trigger;
                const auto packet = ps2_pad::mapDevices(profile, devices);
                return (packet[3] & 0x40) == 0;
            };
            t.IsTrue(!crossDown(-1.f), "trigger at rest: released");
            t.IsTrue(!crossDown(-0.9f), "5% of the travel: still released");
            t.IsTrue(crossDown(0.f), "50%: pressed");
            t.IsTrue(crossDown(-0.5f), "25% after a press: still held (hysteresis)");
            t.IsTrue(!crossDown(-0.7f), "15%: released");
            t.IsTrue(!crossDown(-0.5f), "25% from released: not pressed");
            devices[0].axes[5] = -0.9f;
            const auto packet = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(packet[14] > 0, "the pressure byte stays analog below the press point");
        });

        tc.Run("pedals on the triggers: GT3's analog pedal range, no buttons", [](TestCase &t)
               {
            // GT3_PEDALS=triggers: the right trigger pushes the right stick up
            // (GT3's analog Acceleration), the left one down (Brakes), spread
            // over GT3's live range (28..113 of 128), and a trigger presses no
            // button even when the profile says so.
            ps2_pad::setPedalsOnTriggersForTest(1);
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream config("pressure cross 0 5 -1 1 0.02\npressure square 0 4 -1 1 0.02\n"
                                      "stick-half ry up 0 5 -1 1 0.02\nstick-half ry down 0 4 -1 1 0.02\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "pedal profile loads");
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes.fill(0.f);
            devices[0].axes[4] = devices[0].axes[5] = -1.f;
            auto map = [&](float lt, float rt) {
                devices[0].axes[4] = lt;
                devices[0].axes[5] = rt;
                return ps2_pad::mapDevices(profile, devices);
            };
            auto p = map(-1.f, -1.f);
            t.IsTrue(p[5] == 128, "both triggers at rest: the right stick centred");
            p = map(-1.f, 1.f);
            t.IsTrue(p[5] == 15, "right trigger full: stick up to GT3's full throttle (113 of 128)");
            t.IsTrue((p[3] & 0x40) != 0, "right trigger full: X (Cross) not pressed");
            p = map(-1.f, 0.f);
            t.IsTrue(p[5] == 58, "right trigger half: about the middle of GT3's range");
            p = map(-1.f, -0.9f);
            t.IsTrue(p[5] == 97, "a light touch: just past GT3's dead zone (28 of 128)");
            p = map(1.f, -1.f);
            t.IsTrue(p[5] == 240, "left trigger full: stick down (Brakes)");
            t.IsTrue((p[3] & 0x80) != 0, "left trigger full: Square not pressed");

            // No stick-half lines: the triggers still drive the pedals, and the
            // default trigger pressures (L2 / R2) press nothing.
            auto bare = ps2_pad::InputProfile::defaults();
            devices[0].axes[4] = -1.f;
            devices[0].axes[5] = 1.f;
            const auto q = ps2_pad::mapDevices(bare, devices);
            t.IsTrue(q[5] == 15, "default profile: right trigger = stick up");
            t.IsTrue((q[3] & 0x02) != 0, "default profile: R2 not pressed by the right trigger");
            ps2_pad::setPedalsOnTriggersForTest(-1);
        });

        tc.Run("analog triggers as L2 / R2: GT3's pressure range, pressed from a light touch", [](TestCase &t)
               {
            // A controller's triggers mapped to L2 / R2 (the launcher's
            // default): pressure spread over GT3's range (R2 1..45, the
            // throttle; L2 1..83, the brake) and the button down
            // from 4% of the travel (its pressure counts only while it is).
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream config("pressure l2 0 4 -1 1 0.02\npressure r2 0 5 -1 1 0.02\nbutton l2 0 -1\nbutton r2 0 -1\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "trigger profile loads");
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes.fill(0.f);
            auto map = [&](float lt, float rt) {
                devices[0].axes[4] = lt;
                devices[0].axes[5] = rt;
                return ps2_pad::mapDevices(profile, devices);
            };
            auto p = map(-1.f, -1.f);
            t.IsTrue(p[1] == 0x79 && p[18] == 0 && p[19] == 0, "at rest: no pressure");
            t.IsTrue((p[3] & 0x01) != 0 && (p[3] & 0x02) != 0, "at rest: L2 / R2 up");
            p = map(-1.f, 1.f);
            t.IsTrue(p[19] == 45, "right trigger full: R2 pressure 45 (GT3's full)");
            t.IsTrue((p[3] & 0x02) == 0, "right trigger full: R2 down");
            p = map(-1.f, 0.f);
            t.IsTrue(p[19] >= 21 && p[19] <= 24, "right trigger half: about half of GT3's range");
            p = map(-0.84f, -1.f);
            t.IsTrue((p[3] & 0x01) == 0 && p[18] >= 4 && p[18] <= 8, "left trigger at 8%: L2 down, a light pressure");
            p = map(1.f, -1.f);
            t.IsTrue(p[18] == 83 && (p[3] & 0x01) == 0, "left trigger full: L2 pressure 83 (GT3's full brake), down");
        });

        tc.Run("steering wheel and pedals: lock, GT3's steering dead zone, pedal curves, wheel buttons", [](TestCase &t)
               {
            // A wheel base (device 1: steering axis 0, pedals on axes 1 / 2
            // released at -1, +1 fully pressed; 128 buttons) next to a
            // controller (device 0), as the launcher's Wheel & pedals tab writes.
            auto profile = ps2_pad::InputProfile::defaults();
            std::string error;
            std::istringstream config("wheel-steer 1 0 -1 0 1 0.4 0.2913\n"
                                      "wheel-pedal throttle r2 1 1 -1 1 0.01\n"
                                      "wheel-pedal brake l2 1 2 -1 1 0.01\n"
                                      "wheel-pedal clutch none 1 3 -1 1 0\n"
                                      "wheel-button cross 1 100\n"
                                      "wheel-button start 1 3\n");
            t.IsTrue(ps2_pad::readProfile(config, profile, error), "wheel profile loads");
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes.fill(0.f); devices[0].axes[4] = -1.f; devices[0].axes[5] = -1.f;
            devices[1].connected = true; devices[1].axisCount = 4;
            devices[1].axes = {0.f, -1.f, -1.f, -1.f, 0.f, 0.f, 0.f, 0.f};
            auto p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] == 128, "wheel centred: left stick X centred");
            t.IsTrue(p[19] == 0 && p[18] == 0, "pedals released: no pressure");
            t.IsTrue((p[3] & 0x03) == 0x03, "pedals released: L2 / R2 up");
            devices[1].axes[0] = 0.4f; // the lock: full steering at 40% of the travel
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] == 255, "wheel at the lock: full right");
            devices[1].axes[0] = -0.8f;
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] == 0, "wheel past the lock the other way: full left");
            devices[1].axes[0] = 0.004f; // one degree or so: past GT3's dead zone at once
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] >= 128 + 37 && p[6] <= 128 + 39, "a touch of steering starts past GT3's 37 / 127 dead zone");
            devices[1].axes[0] = 0.2f; // half the lock
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] >= 128 + 81 && p[6] <= 128 + 83, "half the lock: 37 + 90 / 2 past the middle");
            devices[0].axes[0] = -1.f; // the controller's stick further out than the wheel: it wins
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] == 0, "the stick pushed further than the wheel is turned drives it");
            devices[0].axes[0] = 0.f;
            devices[1].axes[1] = 1.f; // throttle full
            devices[1].axes[2] = 0.f; // brake half
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[19] == 45 && (p[3] & 0x02) == 0, "throttle pedal full: R2 45 (GT3's full), down");
            t.IsTrue(p[18] >= 41 && p[18] <= 43 && (p[3] & 0x01) == 0, "brake pedal half: L2 about half of 1..83, down");
            devices[0].axes[5] = 0.f; // the controller's right trigger half: less than the pedal
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[19] == 45, "the pedal pressed further than the trigger keeps R2");
            devices[1].buttons[100] = true;
            devices[1].buttons[3] = true;
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue((p[3] & 0x40) == 0, "wheel button 100 presses Cross");
            t.IsTrue((p[2] & 0x08) == 0, "wheel button 3 presses Start");
            devices[1].connected = false;
            p = ps2_pad::mapDevices(profile, devices);
            t.IsTrue(p[6] == 128 && p[19] == 23, "wheel unplugged: the controller alone (right trigger half)");
            std::istringstream bad("wheel-steer 1 0 -1 0 1 0 0.29\n");
            t.IsTrue(!ps2_pad::readProfile(bad, profile, error), "a lock of 0 is refused");
        });

        tc.Run("analog backend pressures reach guest memory with masks and modes", [](TestCase &t)
               {
            auto runtime = std::make_unique<PS2Runtime>();
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::clearPadOverrideState();
            ps2_stubs::scePadInit(rdram.data(), &ctx, runtime.get());
            openPadPort(ctx, rdram);
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes[4] = -0.5f; devices[0].axes[5] = 0.f;
            runtime->padBackend().publishDevices(devices);
            auto read = [&]() {
                setRegU32(ctx, 4, 0); setRegU32(ctx, 5, 0); setRegU32(ctx, 6, kPadDataAddr);
                ps2_stubs::scePadRead(rdram.data(), &ctx, runtime.get());
            };
            read();
            t.Equals(rdram[kPadDataAddr + 18], uint8_t(0), "pressure disabled initially");
            setRegU32(ctx, 4, 0); setRegU32(ctx, 5, 0);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, runtime.get());
            read();
            // (L2 / R2 from triggers follow GT3's pressure ranges - L2 1..83,
            // R2 1..45: a quarter L2 64 -> 22, a half R2 128 -> 23;
            // ps2_input_profile.cpp.)
            t.Equals(rdram[kPadDataAddr + 18], uint8_t(22), "quarter L2 pressure retained");
            t.Equals(rdram[kPadDataAddr + 19], uint8_t(23), "half R2 pressure retained");
            setRegU32(ctx, 6, kPadBtnR2);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, runtime.get());
            read();
            t.Equals(rdram[kPadDataAddr + 18], uint8_t(0), "masked L2 is zero");
            t.Equals(rdram[kPadDataAddr + 19], uint8_t(23), "unmasked R2 stays analog");
            devices[0].axes[5] = -1;
            runtime->padBackend().publishDevices(devices); read();
            t.Equals(rdram[kPadDataAddr + 19], uint8_t(0), "release reaches guest as zero");
            devices[0].connected = false;
            runtime->padBackend().publishDevices(devices); read();
            t.Equals(readButtons(rdram), uint16_t(0xffff), "disconnect releases buttons");
            closePadPort(ctx, rdram);
        });

        tc.Run("snapshot publication keeps related axes coherent across threads", [](TestCase &t)
               {
            PSPadBackend backend;
            ps2_pad::Devices devices{};
            devices[0].connected = true; devices[0].axisCount = 6;
            devices[0].axes.fill(-1.f);
            backend.publishDevices(devices);
            std::atomic<bool> done{false}, coherent{true};
            std::thread reader([&]() {
                do {
                    uint8_t packet[32]{};
                    if (!backend.readState(0, 0, packet, 32) || packet[4] != packet[5] ||
                        packet[5] != packet[6] || packet[6] != packet[7]) coherent = false;
                } while (!done.load());
            });
            for (int i = 0; i < 10000; ++i)
            {
                devices[0].axes.fill(i % 2 ? -1.f : 1.f);
                backend.publishDevices(devices);
            }
            done = true; reader.join();
            t.IsTrue(coherent.load(), "guest receives whole packets rather than mixed frames");
        });
        tc.Run("backend snapshot starts neutral and rejects unsupported ports", [](TestCase &t)
               {
            PSPadBackend backend;
            uint8_t data[32]{};
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "initial snapshot is readable without a window");
            t.Equals(data[2], uint8_t(255), "initial buttons low byte is released");
            t.Equals(data[3], uint8_t(255), "initial buttons high byte is released");
            for (int i = 4; i < 8; ++i)
                t.Equals(data[i], uint8_t(128), "initial sticks are centered");
            t.IsTrue(!backend.readState(1, 0, data, sizeof(data)), "port 1 must not mirror port 0");
            t.IsTrue(!backend.readState(0, 1, data, sizeof(data)), "unsupported slot fails");
            t.IsTrue(!backend.readState(0, 0, data, 31), "short output fails");
            t.IsTrue(!backend.readState(0, 0, nullptr, 32), "null output fails");
        });
        tc.Run("scePadRead uses override state", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~kPadBtnCross & ~kPadBtnStart);
            ps2_stubs::setPadOverrideState(buttons, 0x00, 0xFF, 0x10, 0xEE);

            runPadRead(ctx, rdram);

            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should return 1");
            t.Equals(readButtons(rdram), buttons, "button bitmask should match override state");
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[4], static_cast<uint8_t>(0x10), "rx should match override");
            t.Equals(data[5], static_cast<uint8_t>(0xEE), "ry should match override");
            t.Equals(data[6], static_cast<uint8_t>(0x00), "lx should match override");
            t.Equals(data[7], static_cast<uint8_t>(0xFF), "ly should match override");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead button bits are active-low", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            struct ButtonCase
            {
                uint16_t mask;
                const char *name;
            };

            const ButtonCase cases[] = {
                {kPadBtnSelect, "select"},
                {kPadBtnL3, "l3"},
                {kPadBtnR3, "r3"},
                {kPadBtnStart, "start"},
                {kPadBtnUp, "up"},
                {kPadBtnRight, "right"},
                {kPadBtnDown, "down"},
                {kPadBtnLeft, "left"},
                {kPadBtnL2, "l2"},
                {kPadBtnR2, "r2"},
                {kPadBtnL1, "l1"},
                {kPadBtnR1, "r1"},
                {kPadBtnTriangle, "triangle"},
                {kPadBtnCircle, "circle"},
                {kPadBtnCross, "cross"},
                {kPadBtnSquare, "square"}};

            for (const auto &entry : cases)
            {
                const uint16_t buttons = static_cast<uint16_t>(0xFFFFu & ~entry.mask);
                ps2_stubs::setPadOverrideState(buttons, 0x80, 0x80, 0x80, 0x80);
                runPadRead(ctx, rdram);

                t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadRead should succeed for opened ports");
                const uint16_t mask = readButtons(rdram);
                t.IsTrue((mask & entry.mask) == 0, std::string("button should be active-low: ").append(entry.name));
            }

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("scePadGetButtonMask returns all buttons", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetButtonMask(nullptr, &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0xFFFF), "button mask should be 0xFFFF");
        });

        tc.Run("basic pad init/port/state functions return expected values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit should succeed");

            ps2_stubs::scePadInit2(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInit2 should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should report DISCONNECTED");

            openPadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortOpen should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "scePadGetState should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadGetReqState should return completed");

            ps2_stubs::scePadGetPortMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(2), "scePadGetPortMax should be 2");

            setRegU32(ctx, 4, 0u);
            ps2_stubs::scePadGetSlotMax(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadGetSlotMax should be 1");

            ps2_stubs::scePadGetModVersion(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0x0200), "scePadGetModVersion should be 0x0200");

            closePadPort(ctx, rdram);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadPortClose should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "closed port should return DISCONNECTED after close");
        });

        tc.Run("pad command state reports EXECCMD once before returning STABLE", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after mode command should return STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(5), "first state after press-mode command should be EXECCMD");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "second state after press-mode command should return STABLE");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad info and mode helpers return consistent values", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 6, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoAct(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoAct should report at least one actuator descriptor");

            ps2_stubs::scePadInfoComb(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadInfoComb should return 0");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, static_cast<uint32_t>(-1));
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.IsTrue(static_cast<uint32_t>(getRegU32(&ctx, 2)) >= 1u, "scePadInfoMode table count should be non-zero");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadInfoPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadInfoPressMode should report pressure support");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "CURID should switch to digital mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should accept analog mode");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 4);
            setRegU32(ctx, 7, 0u);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "mode table entry should return DualShock in analog mode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pads open in digital mode and switch to analog on scePadSetMainMode", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(6), "freshly opened port should report STABLE");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(4), "scePadInfoMode CURID should return digital at open");

            runPadRead(ctx, rdram);
            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[1], static_cast<uint8_t>(0x41), "mode byte should be 0x41 (digital) at open");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 3);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed switching to analog");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1);
            setRegU32(ctx, 7, 0);
            ps2_stubs::scePadInfoMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(7), "scePadInfoMode CURID should return analog after SetMainMode");

            // scePadSetMainMode queues a one-shot EXECCMD transient state; pump scePadGetState
            // once so the port settles back to STABLE before reading, mirroring the existing
            // "pad command state reports EXECCMD once before returning STABLE" test.
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);

            runPadRead(ctx, rdram);
            t.Equals(data[1], static_cast<uint8_t>(0x73), "mode byte should be 0x73 (analog) after SetMainMode");

            closePadPort(ctx, rdram);
        });

        tc.Run("pad setters return success", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActAlign(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActAlign should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetActDirect(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetActDirect should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 1u);
            setRegU32(ctx, 7, 3u);
            ps2_stubs::scePadSetMainMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetMainMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetReqState(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetReqState should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetVrefParam(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetVrefParam should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadSetWarningLevel(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(0), "scePadSetWarningLevel should return 0");

            ps2_stubs::scePadEnd(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnd should succeed");

            openPadPort(ctx, rdram);
            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should succeed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadExitPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadExitPressMode should succeed");

            closePadPort(ctx, rdram);
        });

        tc.Run("scePadRead fills pressure bytes and honors button info mask", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPadPort(ctx, rdram);

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, 0xFFFFu);
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should accept all buttons");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            ps2_stubs::scePadEnterPressMode(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadEnterPressMode should enable pressure data");

            const uint16_t pressedButtons = static_cast<uint16_t>(0xFFFFu &
                                                                   ~kPadBtnLeft &
                                                                   ~kPadBtnUp &
                                                                   ~kPadBtnTriangle &
                                                                   ~kPadBtnCross &
                                                                   ~kPadBtnL1 &
                                                                   ~kPadBtnR2);
            ps2_stubs::setPadOverrideState(pressedButtons, 0x80, 0x80, 0x80, 0x80);
            runPadRead(ctx, rdram);

            const uint8_t *data = rdram.data() + kPadDataAddr;
            t.Equals(data[8], static_cast<uint8_t>(0x00), "right pressure should be clear when not pressed");
            t.Equals(data[9], static_cast<uint8_t>(0xFF), "left pressure should be populated when pressed");
            t.Equals(data[10], static_cast<uint8_t>(0xFF), "up pressure should be populated when pressed");
            t.Equals(data[11], static_cast<uint8_t>(0x00), "down pressure should be clear when not pressed");
            t.Equals(data[12], static_cast<uint8_t>(0xFF), "triangle pressure should be populated when pressed");
            t.Equals(data[13], static_cast<uint8_t>(0x00), "circle pressure should be clear when not pressed");
            t.Equals(data[14], static_cast<uint8_t>(0xFF), "cross pressure should be populated when pressed");
            t.Equals(data[15], static_cast<uint8_t>(0x00), "square pressure should be clear when not pressed");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "L1 pressure should be populated when pressed");
            t.Equals(data[17], static_cast<uint8_t>(0x00), "R1 pressure should be clear when not pressed");
            t.Equals(data[18], static_cast<uint8_t>(0x00), "L2 pressure should be clear when not pressed");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "R2 pressure should be populated when pressed");

            setRegU32(ctx, 4, 0u);
            setRegU32(ctx, 5, 0u);
            setRegU32(ctx, 6, static_cast<uint32_t>(kPadBtnL1 | kPadBtnR2));
            ps2_stubs::scePadSetButtonInfo(rdram.data(), &ctx, nullptr);
            t.Equals(static_cast<uint32_t>(getRegU32(&ctx, 2)), static_cast<uint32_t>(1), "scePadSetButtonInfo should narrow the enabled pressure mask");

            runPadRead(ctx, rdram);

            t.Equals(data[9], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[10], static_cast<uint8_t>(0x00), "masked-out direction pressure should clear");
            t.Equals(data[12], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[14], static_cast<uint8_t>(0x00), "masked-out face-button pressure should clear");
            t.Equals(data[16], static_cast<uint8_t>(0xFF), "enabled L1 pressure should remain populated");
            t.Equals(data[19], static_cast<uint8_t>(0xFF), "enabled R2 pressure should remain populated");

            ps2_stubs::clearPadOverrideState();
            closePadPort(ctx, rdram);
        });

        tc.Run("pad string helpers map state codes", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr)).find("FINDPAD") != std::string::npos,
                     "state 1 should map to FINDPAD");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64)).find("DISCONNECTED") != std::string::npos,
                     "state 0 should map to DISCONNECTED");

            setRegU32(ctx, 4, 1);
            setRegU32(ctx, 5, kPadDataAddr + 128);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            t.IsTrue(std::string(reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 128)).find("BUSY") != std::string::npos,
                     "req state 1 should map to BUSY");
        });
        tc.Run("scePadGetFrameCount increments", [](TestCase &t)
               {
            R5900Context ctx;
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t first = getRegU32(&ctx, 2);
            ps2_stubs::scePadGetFrameCount(nullptr, &ctx, nullptr);
            const uint32_t second = getRegU32(&ctx, 2);
            t.Equals(second, first + 1, "frame count should increment");
        });

        tc.Run("scePadStateIntToStr and scePadReqIntToStr write strings", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;

            setRegU32(ctx, 4, 6);
            setRegU32(ctx, 5, kPadDataAddr);
            ps2_stubs::scePadStateIntToStr(rdram.data(), &ctx, nullptr);
            const char *stateStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr);
            t.IsTrue(std::string(stateStr).find("STABLE") != std::string::npos, "state string should include STABLE");

            setRegU32(ctx, 4, 0);
            setRegU32(ctx, 5, kPadDataAddr + 64);
            ps2_stubs::scePadReqIntToStr(rdram.data(), &ctx, nullptr);
            const char *reqStr = reinterpret_cast<const char *>(rdram.data() + kPadDataAddr + 64);
            t.IsTrue(std::string(reqStr).find("COMPLETE") != std::string::npos, "req string should include COMPLETE");
        });
    });
}
