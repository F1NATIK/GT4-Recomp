#pragma once
#include "ps2x/iop/iop_host.h"
#include <mutex>
namespace ps2x::iop::gt3 {
// Owned native model storage only; this is not a resident mcman provider.
// Caller must verify supplied IRX before using original relative mappings.
class McmanStorage {
public:
    explicit McmanStorage(IopHost& host):host_(host) {}
    ~McmanStorage() { reset(); }
    McmanStorage(const McmanStorage&)=delete;
    McmanStorage& operator=(const McmanStorage&)=delete;
    bool prepare();
    void reset();
    uint32_t base() const;
    static constexpr uint32_t Size=0x26000;
private:
    IopHost& host_;
    mutable std::mutex mutex_;
    uint32_t base_=0;
};
}
