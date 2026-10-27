#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ps2x::iop::gt3 {
// Native owned resources for LIBSD's two transfer events and IRQ36/40/9.
// Transfer completion is raised only after the supplied memory operation
// succeeds. Operations/handlers execute without the event-state lock; reset
// cancels their publication. No generic IOP kernel or physical IRQ timing.
class Spu2Events {
public:
    using Operation = std::function<bool()>;
    using SoundHandler = std::function<bool(uint32_t)>;
    struct Transfer {
        uint32_t eventId = 0, eventBits = 0;
        uint64_t ticket = 0, irqCount = 0;
        bool busy = false, complete = false;
    };
    struct Result { bool accepted = false, complete = false; uint64_t ticket = 0; };
    Spu2Events();
    ~Spu2Events();
    bool initialize();
    void reset();
    bool ready() const;
    std::optional<Transfer> transfer(unsigned core) const;
    Result runTransfer(unsigned core, const Operation&);
    bool waitTransfer(unsigned core, uint64_t ticket, std::chrono::milliseconds timeout) const;
    bool clearEvent(unsigned core, uint32_t bits);
    bool setSoundHandler(SoundHandler);
    bool raiseSound(uint32_t coreMask);
    uint64_t soundIrqCount() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
