#include "ps2x/iop/gt3/pdi1394_resident.h"
#include "ps2x/iop/gt3/pdi1394_startup.h"
#include "ps2x/iop/native_library.h"
#include <mutex>

namespace ps2x::iop::gt3 {
namespace {
RpcResult rejected() { RpcResult r; r.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return r; }
}

struct Pdi1394Resident::State {
    IopHost& host; NativeLibraryRegistry* registry;
    explicit State(IopHost& h):host(h),registry(h.nativeLibraries()) {}
    mutable std::mutex mutex;
    uint32_t image=0,replyPacket=0; uint64_t registration=0;
    // Shared so a frame exchange blocked on the EE executor keeps the link
    // alive while another thread resets the IOP.
    std::shared_ptr<Pdi1394Link> link;
    void release() {
        std::shared_ptr<Pdi1394Link> old;
        { std::lock_guard lock(mutex); old=std::move(link); }
        if (old) old->stop();
        std::lock_guard lock(mutex);
        if (registration && registry) registry->unregisterLibrary(registration);
        if (replyPacket) host.freeIopMemory(replyPacket);
        if (image) host.freeIopMemory(image);
        registration=0; replyPacket=0; image=0;
    }
};

Pdi1394Resident::Pdi1394Resident(IopHost& host):state_(std::make_shared<State>(host)) {}
Pdi1394Resident::~Pdi1394Resident() { reset(); }

bool Pdi1394Resident::start() {
    auto& s=*state_;
    { std::lock_guard lock(s.mutex); if (s.link || s.image || !s.registry) return false; }
    struct Undo { State& s; bool done=false; ~Undo() { if (!done) s.release(); } } undo{s};
    const uint32_t image=s.host.allocateIopMemory(ModelBytes);
    { std::lock_guard lock(s.mutex); s.image=image; }
    if (!image || !initializePdi1394Image(s.host,image)) return false;
    // Original exports 1..3 are empty void functions without a return value.
    NativeLibrary library{"pdi1394",0x101,{}};
    for (uint16_t ordinal=1;ordinal<=3;++ordinal) library.exports.push_back({ordinal,[](const auto&) { return NativeExportResult{}; }});
    // Nothing imports these; a second console in one process (the link
    // probe) finds the name taken and runs without it.
    const auto registration=s.registry->registerLibrary(std::move(library));
    const uint32_t replyPacket=s.host.allocateIopMemory(0x40);
    { std::lock_guard lock(s.mutex); s.registration=registration; s.replyPacket=replyPacket; }
    if (!replyPacket) return false;
    auto link=std::make_shared<Pdi1394Link>(s.host);
    if (!link->start(replyPacket)) return false;
    { std::lock_guard lock(s.mutex); s.link=std::move(link); }
    undo.done=true;
    s.host.log(LogLevel::Info,"[GT3/PDI1394] native i.LINK module ready");
    return true;
}

bool Pdi1394Resident::reset() { state_->release(); return true; }

bool Pdi1394Resident::active() const { std::lock_guard lock(state_->mutex); return state_->link!=nullptr; }

std::optional<Pdi1394Resident::Snapshot> Pdi1394Resident::snapshot() const {
    auto& s=*state_; std::shared_ptr<Pdi1394Link> link; Snapshot snap;
    {
        std::lock_guard lock(s.mutex); if (!s.link) return {};
        link=s.link; snap.image=s.image; snap.replyPacket=s.replyPacket; snap.phase=Phase::Ready;
    }
    snap.link=link->status();
    return snap;
}

RpcResult Pdi1394Resident::handleRpc(const RpcRequest& request) {
    std::shared_ptr<Pdi1394Link> link;
    { std::lock_guard lock(state_->mutex); link=state_->link; }
    return link?link->handleRpc(request):rejected();
}

void Pdi1394Resident::onVBlank(bool start,uint64_t) {
    if (!start) return;
    std::shared_ptr<Pdi1394Link> link;
    { std::lock_guard lock(state_->mutex); link=state_->link; }
    if (link) link->onVBlank();
}

void Pdi1394Resident::onSifTransfer(const SifTransfer& transfer) {
    std::shared_ptr<Pdi1394Link> link;
    { std::lock_guard lock(state_->mutex); link=state_->link; }
    if (link) link->onSifTransfer(transfer);
}
}
