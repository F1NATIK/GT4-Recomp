#pragma once
#include "ps2x/iop/native_library.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/gt3/mcman_startup_services.h"
#include "ps2x/iop/gt3/mc_card.h"
namespace ps2x::iop::gt3 {
// Native MCMAN resident. Ports without a card image answer as absent over
// the original SIO2 exchanges; a card image (GT3_MEMCARD1/GT3_MEMCARD2, PCSX2
// .ps2 layout, created when missing) is served by the native McCard file
// system. Does not execute original IOP instructions.
// Verifies the original MCMAN file before applying version-specific mappings.
// Host outlives provider. Publication/reset run on the controlling thread.
class McmanNativeExports {
public:
    explicit McmanNativeExports(IopHost&);
    ~McmanNativeExports();
    McmanNativeExports(const McmanNativeExports&)=delete;
    McmanNativeExports& operator=(const McmanNativeExports&)=delete;
    bool publish();
    bool active() const;
    McmanStartupServices& services();
    // The card inserted in port 0/1, or null (absent).
    McCard* card(uint32_t port);
    void reset();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
