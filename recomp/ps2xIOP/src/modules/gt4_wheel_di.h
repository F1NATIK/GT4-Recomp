#pragma once

// GT4 LGDEV host adapter for real wheels: DirectInput 8 devices (Logitech
// G-series, Fanatec bases and pedals, any DirectInput wheel) presented to
// GT4's LGDEV service (gt4_lgdev.cpp) as the HostWheel of gt4_lgdev.h.
//
// Layers:
//   DirectInputWheel (this file)    protocol-independent: device choice, axis /
//     | (one worker thread)         pedal / button mapping, force-feedback state,
//     |                             safety (off by default, master gain, slew
//     |                             limit, pause when unfocused, stop on power-off,
//     |                             exit and from the crash handler)
//   DiBackend / DiDevice            the DirectInput calls themselves; the real one
//                                   is createDirectInput8Backend() (dinput8.dll
//                                   loaded at run time, gt4_wheel_dinput8.cpp), the
//                                   unit tests use a fake (analysis/extras/wheel-glue)
//
// The LGDEV side calls poll() / command() on the EE executor; both only take a
// short lock and never call DirectInput. Every DirectInput call happens on the
// worker thread (some drivers block for milliseconds in SetParameters).
//
// Units: WheelInput / WheelCommand are LGDEV's (gt4_lgdev.h). DirectInput
// forces are -10000..10000 (DI_FFNOMINALMAX), axes are set to -32768..32767.

#include "gt4_lgdev.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ps2x::iop::gt4
{
    // ------------------------------------------------------------------------
    // DirectInput layer (fake-able)
    // ------------------------------------------------------------------------
    enum class DiAxis : uint8_t
    {
        X,
        Y,
        Z,
        Rx,
        Ry,
        Rz,
        Slider0,
        Slider1,
        Count,
    };
    constexpr unsigned DiAxisCount = static_cast<unsigned>(DiAxis::Count);
    const char *diAxisName(DiAxis axis);
    bool parseDiAxis(std::string_view text, DiAxis &axis);

    // DI8DEVTYPE_* (dinput.h), LOBYTE of DIDEVICEINSTANCE.dwDevType.
    namespace DiType
    {
        constexpr uint8_t Joystick = 0x14, Gamepad = 0x15, Driving = 0x16, Flight = 0x17, Supplemental = 0x1c;
    }

    struct DiDeviceInfo
    {
        std::string instance;         // instance GUID text "{8-4-4-4-12}" (stable while plugged in)
        std::string name;             // product name
        uint16_t vendor = 0;          // USB VID (guidProduct.Data1 low word)
        uint16_t product = 0;         // USB PID (high word)
        uint8_t type = 0;             // DiType
        uint8_t subType = 0;
        bool forceFeedback = false;   // enumerated with DIEDFL_FORCEFEEDBACK
        uint8_t axes = 0;             // bit (1 << DiAxis) per axis the device has (filled by open())
        std::string describe() const; // "name vid:pid type, FFB"
    };

    // The adapter's data format (gt4_wheel_dinput8.cpp builds the matching
    // DIDATAFORMAT): DIJOYSTATE2's axes, POVs and 128 buttons.
    struct DiState
    {
        std::array<int32_t, DiAxisCount> axis{}; // -32768..32767 (DIPROP_RANGE)
        std::array<uint32_t, 4> pov{};            // hundredths of a degree clockwise from north; low word 0xffff = centred
        std::array<uint8_t, 128> buttons{};       // bit 7 = down
    };

    enum class DiStatus : uint8_t
    {
        Ok,
        NotAcquired, // DIERR_NOTACQUIRED: acquire again
        InputLost,   // DIERR_INPUTLOST: acquire again
        Gone,        // DIERR_UNPLUGGED / device removed
        Failed,
    };
    const char *diStatusName(DiStatus status);

    enum class DiEffectKind : uint8_t
    {
        Constant, // GUID_ConstantForce
        Spring,   // GUID_Spring
        Damper,   // GUID_Damper
        Friction, // GUID_Friction
    };
    const char *diEffectKindName(DiEffectKind kind);

    // One-axis DICONDITION.
    struct DiCondition
    {
        int32_t offset = 0;              // -10000..10000 of the axis
        int32_t positiveCoefficient = 0; // -10000..10000
        int32_t negativeCoefficient = 0;
        uint32_t positiveSaturation = 0; // 0..10000
        uint32_t negativeSaturation = 0;
        int32_t deadBand = 0;            // 0..10000
        bool operator==(const DiCondition &) const = default;
    };

    // One DirectInput effect on one axis (DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS,
    // direction 0: the sign of the magnitude / coefficients gives the direction).
    struct DiEffect
    {
        DiEffectKind kind = DiEffectKind::Constant;
        uint32_t axisOffset = 0;            // data-format offset of the force axis (X = 0)
        int32_t magnitude = 0;              // constant: -10000..10000
        DiCondition condition{};            // spring / damper / friction
        uint32_t durationUs = 0xffffffffu;  // INFINITE
        uint32_t startDelayUs = 0;
        bool operator==(const DiEffect &) const = default;
    };
    std::string describeDiEffect(const DiEffect &effect);

    class DiDevice
    {
    public:
        virtual ~DiDevice() = default;
        virtual const DiDeviceInfo &info() const = 0;
        // SetCooperativeLevel(window, (exclusive ? EXCLUSIVE : NONEXCLUSIVE) | BACKGROUND) when it
        // changes, then Acquire. DirectInput plays effects only with exclusive access.
        virtual DiStatus acquire(bool exclusive) = 0;
        virtual void unacquire() = 0;
        virtual DiStatus poll(DiState &state) = 0;
        // DIPROP_AUTOCENTER. Returns Ok, or NotAcquired when the driver needed the device
        // unacquired to change it (the backend then re-acquired: running effects stopped).
        virtual DiStatus setAutoCentre(bool on) = 0;
        virtual bool setDeviceGain(uint32_t gain) = 0; // DIPROP_FFGAIN 0..10000
        virtual bool stopAll() = 0;                    // DISFFC_STOPALL
        virtual int createEffect(const DiEffect &effect) = 0; // handle >= 0, or -1; created stopped
        virtual bool updateEffect(int handle, const DiEffect &effect) = 0; // SetParameters (type-specific)
        virtual bool startEffect(int handle) = 0;   // Start(1, 0)
        virtual bool stopEffect(int handle) = 0;
        virtual void releaseEffect(int handle) = 0; // Unload + Release
        // Any thread, takes no lock (crash handler): DISFFC_STOPALL.
        virtual void emergencyStop() noexcept = 0;
    };

    class DiBackend
    {
    public:
        virtual ~DiBackend() = default;
        virtual bool ready(std::string &error) = 0;              // DirectInput8Create succeeded
        virtual std::vector<DiDeviceInfo> enumerate() = 0;       // attached game controllers
        virtual std::unique_ptr<DiDevice> open(const DiDeviceInfo &device, void *window, std::string &error) = 0;
        virtual bool windowFocused(void *window) const = 0;      // GetForegroundWindow() == window
    };

    // DirectInput 8 through dinput8.dll loaded at run time (Windows); nullptr elsewhere.
    std::shared_ptr<DiBackend> createDirectInput8Backend();

    // ------------------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------------------
    // Defaults fit a Logitech G29 / G920 / G923 in DirectInput (wheel X, gas Y,
    // brake Rz [I]); everything is a config line. Text form (PS2X_WHEEL_CONFIG
    // file, one setting per line, '#' comment):
    //   wheel auto | <vid>:<pid> | {instance-guid} | <name text>
    //   pedals wheel | auto | <vid>:<pid> | {instance-guid} | <name text>
    //   steer <axis> [invert]           axis: x y z rx ry rz slider0 slider1
    //   rotation <deg>                  lock-to-lock set on the wheel base (its driver / menu)
    //   lock <deg>                      GT4 full lock wanted (default: the model's, DFP 900)
    //   gas|brake <axis> [released=<raw>] [full=<raw>] | none
    //   pedal-deadzone <0..0.5>
    //   button <host index 0..127> <cross|square|circle|triangle|r1|l1|r2|l2|select|start|b10|b11|b12|b13|none>
    //   hat <pov 0..3> | none
    //   model dfp | df | gtforce
    //   ffb on | off                    (default off)
    //   ffb-gain <0..1>                 master strength (default 0.25)
    //   ffb-invert 0 | 1                constant force direction
    //   ffb-slew <0.5..100>             constant force change limit, full scale per second (default 4)
    //   ffb-pause-unfocused 0 | 1       stop forces while the game window is not in front (default 1)
    //   ffb-effects constant,spring,damper,friction   (the kinds played; default all)
    //   log 0 | 1                       device list, axis monitor, every applied effect
    struct DiWheelConfig
    {
        struct Pedal
        {
            DiAxis axis = DiAxis::Y;
            bool enabled = true;
            bool calibrated = false; // released/full given; else: rest = first sample, full = the far end
            int32_t released = 0;
            int32_t full = 0;
        };

        std::string wheel = "auto";
        std::string pedals = "wheel";
        DiAxis steerAxis = DiAxis::X;
        bool steerInvert = false;
        double rotation = 900.0;
        double lock = 0.0; // 0: the presented model's (DFP 900, DF / GT Force 200)
        Pedal gas{DiAxis::Y};
        Pedal brake{DiAxis::Rz};
        double pedalDeadzone = 0.02;
        std::array<uint32_t, 128> buttons{}; // host button -> WheelButton bit (0 = none)
        int pov = 0;                         // -1: none
        WheelModel model = WheelModel::DrivingForcePro;
        bool ffb = false;
        double ffbGain = 0.25;
        bool ffbInvert = false;
        double ffbSlew = 4.0;
        bool ffbPauseUnfocused = true;
        bool playConstant = true, playSpring = true, playDamper = true, playFriction = true;
        bool verbose = false;
        std::string ffbLogPath; // every DirectInput force call with its parameters

        DiWheelConfig(); // DFP button layout: host buttons 0..13 -> DFP buttons 0..13
        double effectiveLock() const;
        std::string describe() const;
    };

    bool applyDiWheelConfigLine(DiWheelConfig &config, std::string_view line, std::string &error);
    bool parseDiWheelConfig(std::string_view text, DiWheelConfig &config, std::string &error);
    // PS2X_WHEEL_CONFIG=<file> first, then PS2X_WHEEL_LINES (the same lines
    // separated by ';'), then the single settings on top:
    // PS2X_WHEEL_DEVICE, PS2X_WHEEL_PEDALS, PS2X_WHEEL_ROTATION, PS2X_WHEEL_LOCK,
    // PS2X_WHEEL_STEER_INVERT, PS2X_WHEEL_MODEL, PS2X_WHEEL_FFB, PS2X_WHEEL_FFB_GAIN,
    // PS2X_WHEEL_FFB_INVERT, PS2X_WHEEL_FFB_LOG, PS2X_WHEEL_LOG.
    using EnvLookup = std::function<const char *(const char *name)>;
    bool diWheelConfigFrom(const EnvLookup &lookup, DiWheelConfig &config, std::string &error);

    // ------------------------------------------------------------------------
    // Pure mappings (unit-tested)
    // ------------------------------------------------------------------------
    // Raw steering axis -> LGDEV axis 0: the rim angle (raw / 32767 * rotation / 2)
    // over the model's half lock, clamped: a base at 1080 degrees reaches GT4's full
    // lock at 450 degrees of rim, like a real 900-degree DFP.
    int16_t diSteer(int32_t raw, const DiWheelConfig &config);
    // Pedal travel 0..1 from a raw axis value: rest -> 0, `full` -> 1, dead zone at rest.
    double diPedalTravel(int32_t raw, int32_t released, int32_t full, double deadzone);
    // Uncalibrated pedal: the far end of the axis from where it rests.
    int32_t diPedalFarEnd(int32_t rest);
    uint32_t diButtons(const DiState &state, const DiWheelConfig &config);
    uint8_t diHat(uint32_t pov); // 0..7 clockwise from up, 0xff centred
    // LGDEV effect (its stored, clamped parameters) -> the DirectInput effect played,
    // with LGDEV's gain (0..255), the master gain and the sign conventions applied.
    // The constant force is not slew-limited here.
    DiEffect diEffectFor(WheelEffectType type, const WheelEffectParams &params, uint32_t lgdevGain,
                         const DiWheelConfig &config);
    bool diEffectKindFor(WheelEffectType type, DiEffectKind &kind);

    // ------------------------------------------------------------------------
    // The adapter
    // ------------------------------------------------------------------------
    class DirectInputWheel final : public HostWheel
    {
    public:
        using Log = std::function<void(std::string_view)>;
        struct Options
        {
            void *window = nullptr;   // the game window (DirectInput cooperative level)
            bool startThread = true;  // false: the caller runs pump() (tests)
            unsigned periodMs = 4;    // worker period
        };

        DirectInputWheel(std::shared_ptr<DiBackend> backend, DiWheelConfig config, Options options, Log log);
        ~DirectInputWheel() override; // forces stopped, auto-centre off, devices released, worker joined

        // One host wheel ("USB port1"); connected while its device is open.
        unsigned deviceCount() const override { return 1; }
        WheelModel model(unsigned device) const override;
        bool poll(unsigned device, WheelInput &input) override;
        void command(unsigned device, const WheelCommand &command) override;
        void onVBlank(uint64_t vsync) override;

        // One worker step at `now` seconds (monotonic). The worker thread calls it.
        void pump(double now);
        // Crash handler / any thread: DISFFC_STOPALL on the open wheel; takes no blocking lock.
        void emergencyStop() noexcept;

        struct Stats
        {
            bool connected = false;
            bool ffbActive = false;
            bool paused = false;
            bool poweredOff = false;
            uint64_t enumerations = 0, opens = 0, losses = 0, reacquires = 0;
            uint64_t creates = 0, updates = 0, starts = 0, stops = 0, releases = 0, stopAlls = 0;
            int32_t constantApplied = 0; // last constant magnitude sent (DI units)
            std::string device;          // the wheel's describe()
            std::string pedals;          // the separate pedal device, if any
        };
        Stats stats() const;

    private:
        struct DesiredEffect
        {
            bool exists = false;
            WheelEffectType type = WheelEffectType::Constant;
            WheelEffectParams params{};
            bool playing = false;
            uint64_t startCount = 0;
        };
        struct Desired
        {
            bool open = false;
            int autoCentre = -1; // -1: never set by the game
            uint32_t gain = 0xff;
            std::array<DesiredEffect, 64> effects{};
            uint64_t stopAll = 0;
            bool powerOff = false;
            uint64_t version = 0;
        };
        struct AppliedEffect
        {
            int handle = -1;
            WheelEffectType type = WheelEffectType::Constant;
            DiEffect effect{};
            bool playing = false;
            bool failed = false; // creation failed: not retried until the device is re-opened
            uint64_t startCount = 0;
        };
        struct PedalState
        {
            bool haveRest = false;
            int32_t rest = 0;
        };

        void worker();
        void ensureDevices(double now);
        void openWheel(const DiDeviceInfo &info, double now);
        void openPedals(const std::vector<DiDeviceInfo> &list);
        void dropWheel(const char *why, double now);
        void dropPedals(const char *why);
        void readInput(double now);
        void applyForces(const Desired &d, double now, double dt);
        void releaseEffect(unsigned slot, const char *why);
        void stopEverything(const char *why);
        void forcesLost();
        uint16_t pedal(const DiWheelConfig::Pedal &pedal, PedalState &state, int32_t raw);
        void ffbLog(const std::string &line);
        void logLine(const std::string &line);
        double monotonicNow() const;

        std::shared_ptr<DiBackend> backend_;
        DiWheelConfig config_;
        Options options_;
        Log log_;

        // Shared with the EE side (short critical sections only).
        mutable std::mutex mutex_;
        WheelInput snapshot_{};
        Desired desired_{};
        uint64_t vsync_ = 0;
        Stats stats_{};
        int rangeLogged_ = 0;

        // Worker only.
        bool backendReady_ = false;
        std::string backendError_;
        std::unique_ptr<DiDevice> wheel_;
        std::unique_ptr<DiDevice> pedals_;
        bool exclusive_ = false;
        bool ffbActive_ = false;
        bool poweredOff_ = false;
        double nextEnumerate_ = 0.0;
        double lastPump_ = -1.0;
        double unfocusedSince_ = -1.0;
        bool paused_ = false;
        unsigned failures_ = 0;
        uint32_t plug_ = 0;
        bool everOpened_ = false;
        std::array<AppliedEffect, 64> applied_{};
        int appliedAutoCentre_ = -1;
        uint64_t appliedStopAll_ = 0;
        uint64_t appliedVersion_ = ~0ull;
        PedalState gasState_{}, brakeState_{};
        std::string lastDeviceList_;
        bool loggedNoWheel_ = false;
        bool loggedNoWindow_ = false;
        std::array<bool, 4> loggedUnsupported_{};
        DiState lastState_{};
        double nextMonitor_ = 0.0;
        DiState monitorState_{};
        DiState monitorPedals_{};
        void *ffbFile_ = nullptr; // FILE*

        // Crash-handler path: the open wheel, guarded by a lock the handler only try-locks.
        std::mutex deviceMutex_;
        std::atomic<DiDevice *> live_{nullptr};

        // Worker thread.
        std::mutex wakeMutex_;
        std::condition_variable wake_;
        bool stop_ = false;
        bool kick_ = false; // power-off: run the next step at once
        std::thread thread_;
    };

    // Every live DirectInputWheel (crash handler).
    void hostWheelEmergencyStopAll() noexcept;
}
