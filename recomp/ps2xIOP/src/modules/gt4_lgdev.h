#pragma once

// GT4 LGDEV 1.11.036 (Logitech USB wheel driver, SIF RPC 0x046d046d): the host
// wheel interface the native service (gt4_lgdev.cpp) talks to.
//
// Self-contained on purpose (standard headers only): the runtime's real wheel
// adapter can include it once it is moved to include/ps2x/iop/ or the runtime
// adds this directory to its include path (README in analysis/phase5b/lgdev).
//
// Layering (spec analysis/phase5/specs/p5b-wheel-poweroff-padman.md 2.9.2):
//   GT4 EE liblgdev --RPC--> gt4_lgdev.cpp (protocol only, exact LGDEV codes)
//                               | HostWheel::poll()    (never blocks)
//                               | HostWheel::command() (never blocks; a mailbox)
//                               v
//                            HostWheel implementation: VirtualWheel (below,
//                            tests and hidden runs) or the real host adapter.
//
// Units are LGDEV's own (what the IOP module hands the EE / receives from it);
// translation to DirectInput / SDL units belongs to the adapter.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::gt4
{
    // USB vendor/product id as LGDEV reports it (fn4 property 0x10005). LGDEV
    // 1.11 builds a device-info block and picks a driver class per id (connect
    // IOP+0x3e08, info IOP+0x48d8, factory IOP+0x8148). GT4 knows these three
    // (pad types 1/2/3, EE 0x566808). the developer's default: Driving Force Pro.
    enum class WheelModel : uint32_t
    {
        DrivingForcePro = 0x046dc298u, // 900 degrees (property 4), friction effect, no start-up force test
        DrivingForce = 0x046dc294u,    // 200 degrees [I: hardware]
        GtForce = 0x046dc293u,         // 200 degrees [I]
    };

    // LGDEV button word bits for a DFP (report parser IOP+0x5be4..0x5d10) and
    // what GT4 makes of them (EE 0x5664b0).
    namespace WheelButton
    {
        constexpr uint32_t Cross = 0x00010000u;    // DFP button 0
        constexpr uint32_t Square = 0x00020000u;   // 1
        constexpr uint32_t Circle = 0x00040000u;   // 2
        constexpr uint32_t Triangle = 0x00080000u; // 3
        constexpr uint32_t Start = 0x00100000u;    // 9
        constexpr uint32_t Select = 0x00200000u;   // 8
        constexpr uint32_t L1 = 0x00400000u;       // 5
        constexpr uint32_t R1 = 0x00800000u;       // 4
        constexpr uint32_t L2 = 0x01000000u;       // 7
        constexpr uint32_t R2 = 0x02000000u;       // 6
        constexpr uint32_t B11 = 0x04000000u;      // 11, kept by GT4 as an extra bit ([I] shifter back)
        constexpr uint32_t B10 = 0x08000000u;      // 10, kept by GT4 ([I] shifter forward)
        constexpr uint32_t B13 = 0x10000000u;      // 13, dropped by GT4
        constexpr uint32_t B12 = 0x20000000u;      // 12, dropped by GT4
        constexpr uint32_t Mask = 0x3fff0000u;     // every bit the DFP parser can set (other models differ)
    }

    // One host wheel as LGDEV's normaliser would have produced it (24-byte
    // sample, spec 2.5). The adapter does calibration and rotation scaling.
    struct WheelInput
    {
        bool connected = false;
        uint32_t plug = 0;      // change it while connected to simulate unplug + re-plug
        int16_t steer = 0;      // axis 0: -32767 full left .. +32767 full right (DFP: +-450 degrees)
        uint16_t pedalA = 0;    // axis 6 (DFP report byte 5, gas [I]): 0 released .. 65535 pressed
        uint16_t pedalB = 0;    // axis 7 (DFP report byte 6, brake [I])
        uint8_t hat = 0xff;     // 0..7 clockwise from up, 0xff (or 8) centred; other values pass through as LGDEV does
        uint32_t buttons = 0;   // LGDEV button word of the model (DFP: WheelButton bits), passed through
    };

    // Effect types (params byte 0; factory IOP+0x81c8..0x84c4).
    enum class WheelEffectType : uint8_t
    {
        Constant = 0x00,
        Spring = 0x07,
        Damper = 0x08,
        Friction = 0x10, // DFP only
    };

    // The 0x3c-byte effect block (RPC buffer +0x70) as LGDEV keeps it after
    // its per-class clamps (constant IOP+0x4b0, condition IOP+0xbb4, DF spring
    // IOP+0x894, DF damper IOP+0x668). Word 4 of a constant is the direction
    // already converted to LGDEV's 0..255 circle.
    struct WheelEffectParams
    {
        std::array<int32_t, 15> word{};

        uint8_t type() const { return static_cast<uint8_t>(word[0] & 0xff); }
        int32_t duration() const { return word[1]; } // GT4: -1 (infinite) [I name]
        int32_t delay() const { return word[2]; }    // GT4: 0 [I name]
        // Constant
        int32_t magnitude() const { return word[3]; } // -255..255
        int32_t direction() const { return word[4]; } // 0..255 of a full circle (90 degrees = 64)
        // Condition axis 0 (wheel) or 1 (GT4 leaves it zero)
        struct Condition
        {
            int32_t offset;         // centre, wheel-axis units (-32767..32767)
            int32_t deadBand;       // GT4: 0
            int32_t saturationPos;  // <= 255
            int32_t saturationNeg;  // <= 255 (Logitech "clip")
            int32_t coefficientPos; // -255..255
            int32_t coefficientNeg; // -255..255
        };
        Condition condition(unsigned axis) const
        {
            const unsigned b = axis ? 9u : 3u;
            return {word[b], word[b + 1], word[b + 2], word[b + 3], word[b + 4], word[b + 5]};
        }
    };

    // LGDEV's constant force for this block (IOP+0x204..0x374): x =
    // round(magnitude * sin(direction) * 127 / (255 * 1024)) with LGDEV's
    // 64-entry quarter-sine table (IOP+0x9040); -127..127, before gain.
    // Returned normalised to -1..1 (x / 127). Positive = LGDEV's positive x
    // [C]; which way that turns the rim is a test item with a real wheel [I].
    double wheelConstantLevel(const WheelEffectParams &params);
    // LGDEV's gain scaling (IOP+0x7b20..0x7b30, 0x73d4): round(value * gain / 255) unless gain == 255.
    int32_t wheelApplyGain(int32_t value, uint32_t gain);

    enum class WheelCommandKind : uint8_t
    {
        Open,         // fn2: the game opened the device
        Close,        // fn3: every effect of the device stopped and unloaded (LGDEV IOP+0x7eac + 0x78d4)
        StopAll,      // first open after a connect: LGDEV sends 0xf3 (IOP+0x7e24)
        AutoCentre,   // value 1 = built-in centring spring on (0xf4), 0 = off (0xf5); IOP+0x809c
        Range,        // value 900 or 200 degrees (DFP only: 0xf8 0x03 / 0xf8 0x02, IOP+0x85ac)
        Gain,         // value 0..255; LGDEV scales every force by gain/255
        EffectCreate, // effect slot 0..63, type, params
        EffectUpdate, // value 1 if the stored params changed
        EffectStart,  // value 1 if it was already playing (GT4 restarts every effect every frame)
        EffectStop,
        EffectUnload,
        PowerOff,     // power button (POWOFF): stop all forces, auto-centre off, release the device; the last command sent
    };

    struct WheelCommand
    {
        WheelCommandKind kind = WheelCommandKind::Open;
        uint8_t effect = 0;           // effect slot (id & 0x3f)
        uint32_t effectId = 0;        // LGDEV id: (handle << 16) | (generation << 8) | slot
        WheelEffectType type = WheelEffectType::Constant;
        WheelEffectParams params{};   // EffectCreate / EffectUpdate
        int32_t value = 0;
    };

    const char *wheelCommandName(WheelCommandKind kind);
    std::string describeWheelCommand(const WheelCommand &command);

    // ------------------------------------------------------------------------
    // HostWheel: what gt4_lgdev.cpp needs from the host. Called on the EE side
    // (RPC handler thread and the VBlank callback), so both calls must return
    // at once: poll() reads a snapshot, command() posts to a mailbox.
    // ------------------------------------------------------------------------
    class HostWheel
    {
    public:
        static constexpr unsigned MaxDevices = 2; // GT4 drives "USB port1" and "USB port2"

        virtual ~HostWheel() = default;
        // Host wheels 0..deviceCount()-1 (at most MaxDevices). May change at run time: a wheel
        // at or beyond the current count is treated as unplugged.
        virtual unsigned deviceCount() const = 0;
        virtual WheelModel model(unsigned device) const { (void)device; return WheelModel::DrivingForcePro; }
        // usbd #13 location byte 0. GT4 puts byte == 1 into its slot 1 ("USB
        // port2") and anything else into slot 0 ("USB port1") (EE 0x5557f0),
        // so player 1 gets 2 and player 2 gets 1 by default.
        virtual uint8_t usbPort(unsigned device) const { return device == 0 ? 2 : 1; }
        // Fills `input`; returns false if the device does not exist at all.
        virtual bool poll(unsigned device, WheelInput &input) = 0;
        virtual void command(unsigned device, const WheelCommand &command) = 0;
        // Once per VBlank start, before the service polls (replays, logs).
        virtual void onVBlank(uint64_t vsync) { (void)vsync; }
    };

    // ========================================================================
    // ADAPTER POINT for the real host input layer (not opened here).
    //
    // The runtime registers a factory before the IOP profile is created:
    //     ps2x::iop::gt4::setHostWheelFactory([] { return std::make_shared<DirectInputWheel>(...); });
    // A real adapter (spec 2.9.4 / 2.9.5; GT3 used GLFW raw joysticks for input
    // and never had force feedback) is expected to:
    //   - read the wheel on its own thread (DirectInput 8 DIJOYSTATE2, or SDL3
    //     joystick/haptic) and publish WheelInput snapshots under a lock;
    //     XInput has no wheel axes or effects and is only a rumble fallback;
    //   - queue WheelCommands and apply only the newest state per effect on
    //     that thread (DirectInput SetParameters / Start can block for ms);
    //   - keep force feedback OFF until the input profile enables it, apply a
    //     master gain and a slew limit, stop everything on PowerOff, exit and
    //     from the crash handler;
    //   - never open a device in --headless runs (joysticks are off by rule).
    // Without a registered factory the service uses the environment:
    //   PS2X_WHEEL=virtual | virtual2   one or two VirtualWheels (else none)
    //   PS2X_WHEEL_MODEL=dfp | df | gtforce   (default dfp)
    //   PS2X_WHEEL_REPLAY=<file>        VSync-indexed input script (VirtualWheel)
    //   PS2X_WHEEL_FFB_LOG=<file>       every command the device receives
    // No wheel: LGDEV's device table stays empty and fn1 answers 0x80000002,
    // exactly what GT4 saw before this service existed.
    // ========================================================================
    using HostWheelFactory = std::function<std::shared_ptr<HostWheel>()>;
    void setHostWheelFactory(HostWheelFactory factory);
    // Registered factory, else the environment; nullptr = no wheel. `log` gets one line per decision.
    std::shared_ptr<HostWheel> createHostWheel(const std::function<void(std::string_view)> &log);

    // Scripted wheel: the fake device for unit tests and hidden runs. Input
    // comes from set() or a replay; every command is kept (and written to the
    // force-feedback log if one is configured). Thread-safe.
    class VirtualWheel final : public HostWheel
    {
    public:
        struct Config
        {
            unsigned devices = 1;
            WheelModel model = WheelModel::DrivingForcePro;
            std::string ffbLogPath; // empty = no file
        };

        explicit VirtualWheel(Config config);
        ~VirtualWheel() override;

        unsigned deviceCount() const override { return config_.devices; }
        WheelModel model(unsigned) const override { return config_.model; }
        bool poll(unsigned device, WheelInput &input) override;
        void command(unsigned device, const WheelCommand &command) override;
        void onVBlank(uint64_t vsync) override;

        // Direct control (tests). A replay, when loaded, overrides it every VBlank.
        void set(unsigned device, const WheelInput &input);
        // Replay text, one entry per line, '#' starts a comment, end exclusive:
        //   [p1|p2] begin end steer-deg gas% brake% buttons-hex [hat]
        //   [p1|p2] begin end unplug
        // steer-deg is the rim angle (DFP: +-450 = full lock; DF/GT Force: +-100);
        // outside every entry the wheel is connected and neutral. First match wins.
        bool loadReplay(std::string_view text, std::string &error);
        bool loadReplayFile(const std::string &path, std::string &error);

        struct LoggedCommand
        {
            uint64_t vsync;
            unsigned device;
            WheelCommand command;
        };
        std::vector<LoggedCommand> commands() const;
        void clearCommands();
        uint64_t vsync() const;

    private:
        struct Entry
        {
            unsigned device;
            uint64_t begin, end;
            bool unplug;
            double steerDegrees;
            double gasPercent, brakePercent;
            uint32_t buttons;
            uint8_t hat;
        };
        WheelInput fromEntry(const Entry &entry, const WheelInput &previous) const;

        Config config_;
        mutable std::mutex mutex_;
        std::array<WheelInput, MaxDevices> input_{};
        std::vector<Entry> replay_;
        std::vector<LoggedCommand> log_;
        uint64_t vsync_ = 0;
        void *file_ = nullptr; // FILE*
    };
}
