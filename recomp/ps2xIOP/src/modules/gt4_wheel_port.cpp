#include "gt4_wheel_port.h"

#include <cstdlib>

namespace ps2x::iop::gt4
{
    namespace
    {
        // Everything is the wrapped wheel's, except the USB location of a lone wheel.
        class PortTwoWheel final : public HostWheel
        {
        public:
            explicit PortTwoWheel(std::shared_ptr<HostWheel> inner) : inner_(std::move(inner)) {}
            unsigned deviceCount() const override { return inner_->deviceCount(); }
            WheelModel model(unsigned device) const override { return inner_->model(device); }
            uint8_t usbPort(unsigned device) const override
            {
                return device == 0 && inner_->deviceCount() == 1 ? 1 : inner_->usbPort(device);
            }
            bool poll(unsigned device, WheelInput &input) override { return inner_->poll(device, input); }
            void command(unsigned device, const WheelCommand &command) override { inner_->command(device, command); }
            void onVBlank(uint64_t vsync) override { inner_->onVBlank(vsync); }

        private:
            std::shared_ptr<HostWheel> inner_;
        };
    }

    unsigned wheelPortSetting(const char *value, std::string &note)
    {
        const std::string v = value ? value : "";
        if (v.empty() || v == "1")
            return 1;
        if (v == "2")
            return 2;
        note = "PS2X_WHEEL_PORT=" + v + " not understood (1 or 2); the wheel is player 1's";
        return 1;
    }

    std::shared_ptr<HostWheel> applyWheelPort(std::shared_ptr<HostWheel> wheel, const std::function<void(std::string_view)> &log)
    {
        std::string note;
        const unsigned port = wheelPortSetting(std::getenv("PS2X_WHEEL_PORT"), note);
        if (!note.empty() && log)
            log(note);
        if (!wheel || port != 2)
            return wheel;
        if (log)
            log("PS2X_WHEEL_PORT=2: a lone wheel is player 2's (USB port byte 1, GT4 \"USB port2\", merged with controller port "
                "2 = player 2)" +
                std::string(wheel->deviceCount() > 1 ? "; with two wheels, wheel 0 stays player 1's" : ""));
        return std::make_shared<PortTwoWheel>(std::move(wheel));
    }
}
