#include "gt4_iop.h"
#include "gt4_powoff.h"

#include <cstdlib>

// POWOFF 1.1 (POWOFF.IRX SHA-256 086979b9...28682d, iopmod PDI_PowerOff): no
// SIF RPC, no EE notification; the power button wakes its thread, which shuts
// the hardware down in a fixed order (gt4_powoff.h). Native equivalent: the
// press is latched (any thread), the sequence runs at the next VBlank start on
// the EE side, every GT4 component gets onPowerOff(stage) in POWOFF's order,
// then the host quit handler runs. Test trigger: PS2X_POWER_BUTTON_AT=<vsync>.

namespace ps2x::iop::gt4
{
    namespace
    {
        struct PowerState
        {
            std::mutex mutex;
            PowerOffHandler handler;
            bool pressed = false;
            PowerButtonSource source = PowerButtonSource::Host;
        };

        PowerState &powerState()
        {
            static PowerState state;
            return state;
        }

        class Powoff final : public Component
        {
        public:
            Powoff(IopHost &host, std::function<void(PowerOffStage)> broadcast) : host_(host), broadcast_(std::move(broadcast))
            {
                if (const char *at = std::getenv("PS2X_POWER_BUTTON_AT"); at && *at)
                {
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(at, &end, 10);
                    if (end && *end == '\0' && v > 0)
                    {
                        testAt_ = v;
                        host_.log(LogLevel::Info, "[GT4/POWOFF] test power button armed for vsync " + std::to_string(v));
                    }
                    else
                        host_.log(LogLevel::Warning, std::string("[GT4/POWOFF] PS2X_POWER_BUTTON_AT=") + at + " ignored (vsync number > 0)");
                }
            }

            RpcResult handleRpc(const RpcRequest &) override
            {
                return rejected(); // POWOFF registers no SIF RPC server
            }

            void onVBlank(bool start, uint64_t tick) override
            {
                if (!start || done_)
                    return;
                if (testAt_ && tick >= testAt_)
                {
                    testAt_ = 0;
                    pressPowerButton(PowerButtonSource::Test);
                }
                PowerButtonSource source;
                PowerOffHandler handler;
                {
                    auto &state = powerState();
                    std::lock_guard lock(state.mutex);
                    if (!state.pressed)
                        return;
                    state.pressed = false;
                    source = state.source;
                    handler = state.handler;
                }
                const std::string at = std::string(powerButtonSourceName(source)) + " at vsync " + std::to_string(tick);
                if (!handler)
                {
                    host_.log(LogLevel::Warning, "[GT4/POWOFF] power button (" + at +
                                                     ") ignored: no host quit handler registered (runtime integration missing)");
                    return;
                }
                done_ = true;
                host_.log(LogLevel::Info, "[GT4/POWOFF] power button (" + at + "): power-off sequence");
                for (PowerOffStage stage : {PowerOffStage::CloseFiles, PowerOffStage::Dev9Off, PowerOffStage::CdPowerOff, PowerOffStage::MemoryCards})
                {
                    host_.log(LogLevel::Info, std::string("[GT4/POWOFF] stage ") + std::to_string(static_cast<int>(stage)) + " " + powerOffStageName(stage));
                    if (broadcast_)
                        broadcast_(stage);
                }
                host_.log(LogLevel::Info, "[power] power-off at vsync " + std::to_string(tick));
                handler(PowerOffReport{tick, source});
            }

        private:
            IopHost &host_;
            std::function<void(PowerOffStage)> broadcast_;
            uint64_t testAt_ = 0;
            bool done_ = false;
        };
    }

    const char *powerButtonSourceName(PowerButtonSource source)
    {
        switch (source)
        {
        case PowerButtonSource::Test: return "test trigger";
        case PowerButtonSource::WindowClose: return "window close";
        case PowerButtonSource::Hotkey: return "hotkey";
        case PowerButtonSource::Host: return "host";
        }
        return "?";
    }

    const char *powerOffStageName(PowerOffStage stage)
    {
        switch (stage)
        {
        case PowerOffStage::CloseFiles: return "close files (pfs: 0x5003; no HDD)";
        case PowerOffStage::Dev9Off: return "DEV9 off (dev9x: 0x4402; network, USB storage, wheel)";
        case PowerOffStage::CdPowerOff: return "CD power off (cdvdman #74; disc, streams, sound)";
        case PowerOffStage::MemoryCards: return "memory cards closed (host)";
        }
        return "?";
    }

    void setPowerOffHandler(PowerOffHandler handler)
    {
        auto &state = powerState();
        std::lock_guard lock(state.mutex);
        state.handler = std::move(handler);
    }

    bool hasPowerOffHandler()
    {
        auto &state = powerState();
        std::lock_guard lock(state.mutex);
        return static_cast<bool>(state.handler);
    }

    void pressPowerButton(PowerButtonSource source)
    {
        auto &state = powerState();
        std::lock_guard lock(state.mutex);
        if (state.pressed)
            return;
        state.pressed = true;
        state.source = source;
    }

    std::unique_ptr<Component> createPowoff(IopHost &host, std::function<void(PowerOffStage)> broadcast)
    {
        return std::make_unique<Powoff>(host, std::move(broadcast));
    }
}
