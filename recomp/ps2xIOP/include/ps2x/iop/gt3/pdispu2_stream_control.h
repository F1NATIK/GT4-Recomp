#pragma once
#include "ps2x/iop/iop_host.h"
#include <memory>
#include <optional>
#include <stop_token>

namespace ps2x::iop::gt3 {
// Owned binary wake resource and original2320/23c8 timing descriptor. The
// caller owns the descriptor's IOP allocation until stop and all waits finish.
// No playback or automatic sample clock/voice progress is supplied here.
class Pdispu2StreamControl {
public:
    struct Snapshot { uint32_t semaphore=0; unsigned pending=0; std::array<uint32_t,4> range{}; };
    explicit Pdispu2StreamControl(IopHost&);
    ~Pdispu2StreamControl();
    bool initialize(uint32_t descriptor);
    bool setRange(const std::array<uint32_t,4>& words);
    // Original42/43/45 arithmetic; positive rate and nonnegative period subset.
    // advance is called only for a real downstream progress event, never a
    // fabricated startup completion. Return indicates strict time > range end.
    bool configure(uint32_t samplesPerTick,uint32_t rate);
    bool setPeriod(uint32_t samples);
    std::optional<bool> advance();
    bool wait(std::stop_token);
    // Original46: consume a range signal, wait again when time>=range end,
    // otherwise delay by the positive signed32 difference of start/time lows.
    bool waitForRange(std::stop_token);
    void stop();
    std::optional<Snapshot> snapshot() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
