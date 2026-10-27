#pragma once

// GT4 (SCUS-97328) native IOP services. Written from GT4's own IOPRP300 and
// \IRX\ module builds (analysis\iop-sys, iop-pdi); no GT3
// service code is used (analysis\COMPATIBILITY.md). Each component answers the
// EE-visible SIF RPC protocol of one GT4 module. Anything not characterised is
// left unhandled (RejectUnhandled) so it shows up in the logs instead of being
// answered with invented success.

#include "gt4_powoff.h"
#include "iop_service.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::gt4
{
    namespace cheats { class Overlay; } // GT4_CHEATS (gt4_cheat_overlay.h)
    namespace extracars { class Pack; } // GT4_EXTRA_CARS (ps2x/iop/gt4_extra_cars_pack.h)

    // Disc image access shared by CDVDFSV and PDICDVD. Opened lazily so the
    // image path may be configured after the IOP profile (main.cpp order).
    class Disc
    {
    public:
        explicit Disc(IopHost &host) : host_(host) {}
        ~Disc() { close(); }
        void close();
        // Reads whole 2048-byte sectors at an absolute LSN of the DVD-9 image.
        bool readSectors(uint32_t lsn, uint32_t count, void *destination);
        // Reads any byte range of the image; returns the bytes read (short at the end).
        size_t readBytes(uint64_t offset, void *destination, size_t size);
        uint64_t sectorCount();
        static constexpr uint32_t Layer1Start = 1317040u; // analysis/disc, PVD at +16
    private:
        bool open();
        // GT4_CHEATS (gt4_cheat_overlay.cpp): the patched script bytes over a read; mutex_ held.
        void overlayCheats(uint64_t offset, void *data, size_t bytes, const char *path);
        // GT4_EXTRA_CARS (gt4_extra_cars.cpp): checks and keeps the pack once the image is open (nothing in the Classic
        // build); extraCars() lays its bytes over a read and logs the first read of each file it serves. mutex_ held.
        void loadExtraCars();
        void extraCars(uint64_t offset, uint8_t *data, size_t bytes);
        uint64_t servedSize() const; // the image's bytes, and the pack's files past its end
        IopHost &host_;
        std::mutex mutex_;
        uint64_t file_ = 0;
        uint64_t size_ = 0;
        bool failed_ = false;
        std::shared_ptr<cheats::Overlay> cheats_;
        std::shared_ptr<const extracars::Pack> extra_;
        std::vector<bool> extraServed_; // the TOC, then each pack file: its first read logged
    };

    // Helper for replies: write bytes to the EE receive buffer and mark handled.
    RpcResult reply(IopHost &host, const RpcRequest &request, const void *data, size_t bytes);
    RpcResult rejected();
    bool readSend(IopHost &host, const RpcRequest &request, void *data, size_t bytes);
    std::string hex(uint32_t value);

    class Component
    {
    public:
        virtual ~Component() = default;
        virtual void reset() {}
        virtual RpcResult handleRpc(const RpcRequest &request) = 0;
        virtual void onVBlank(bool, uint64_t) {}
        // POWOFF sequence (gt4_powoff.h), called once per stage in order before the host quits, on the
        // EE executor inside the VBlank event: release host resources, never wait for guest progress.
        virtual void onPowerOff(PowerOffStage) {}
    };

    std::unique_ptr<Component> createCdvdfsv(IopHost &host, Disc &disc);
    std::unique_ptr<Component> createDbcman(IopHost &host);
    std::unique_ptr<Component> createMcserv(IopHost &host);
    // networkAdaptor: GT4_NET != off (dev9x: devctl answers an expansion-bay adaptor).
    std::unique_ptr<Component> createFileio(IopHost &host, bool networkAdaptor = false);
    std::unique_ptr<Component> createAbsent(IopHost &host);
    std::unique_ptr<Component> createSound(IopHost &host, Disc &disc);
    std::unique_ptr<Component> createPdicdvd(IopHost &host, Disc &disc);
    std::unique_ptr<Component> createLgdev(IopHost &host);
    // `broadcast` runs onPowerOff(stage) on every component of the profile.
    std::unique_ptr<Component> createPowoff(IopHost &host, std::function<void(PowerOffStage)> broadcast);
    std::unique_ptr<Component> createUstorage(IopHost &host);
}

namespace ps2x::iop::detail
{
    std::unique_ptr<IopService> createGt4IopService(IopHost &host);
}
