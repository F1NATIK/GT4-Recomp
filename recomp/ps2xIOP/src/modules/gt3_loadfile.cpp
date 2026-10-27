#include "module_factories.h"
#include "ps2x/iop/gt3/sio2_native_backend.h"
#include "ps2x/iop/gt3/sio2_digital_pad.h"
#include "ps2x/iop/gt3/mtap_resident.h"
#include "ps2x/iop/gt3/mcman_native_exports.h"
#include "ps2x/iop/gt3/mcserv_resident.h"
#include "ps2x/iop/gt3/padman_resident.h"
#include "ps2x/iop/gt3/libsd_resident.h"
#include <atomic>
#include "ps2x/iop/gt3/usbd_resident.h"
#include "ps2x/iop/gt3/ilink_resident.h"
#include "ps2x/iop/gt3/libpdi_resident.h"
#include "ps2x/iop/gt3/pdicdvd_resident.h"
#include "ps2x/iop/gt3/pdispu2_resident.h"
#include "ps2x/iop/gt3/pdistr_resident.h"
#include "ps2x/iop/gt3/pdi1394_resident.h"
#include "ps2x/iop/gt3/pdiusb_resident.h"
#include <array>
#include <cstdlib>
#include <mutex>
#include <string>
namespace ps2x::iop::detail {
namespace {
bool verifiedModule(IopHost& host,std::string_view path,std::string_view expected) {
    const auto file=host.openHostFile(host.translateGuestPath(path));
    if (!file) return false;
    struct Close { IopHost& host; uint64_t file; ~Close() { host.closeHostFile(file); } } close{host,file};
    uint64_t size=0; size_t read=0;
    if (!host.hostFileSize(file,size) || size<52 || size>1024*1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size)); std::array<uint8_t,32> digest{};
    if (!host.readHostFile(file,0,bytes.data(),bytes.size(),read) || read!=bytes.size() ||
        !host.sha256(bytes,digest)) return false;
    constexpr char hex[]="0123456789abcdef"; std::string actual;
    for (auto byte:digest) { actual+=hex[byte>>4]; actual+=hex[byte&15]; }
    return actual==expected;
}
// Port0 controller: a DualShock 2 (GT3_PAD_TYPE=digital: the basic digital pad).
std::shared_ptr<gt3::Sio2Device> padDevice(IopHost& host) {
    const char* type=std::getenv("GT3_PAD_TYPE");
    return type && std::string_view(type)=="digital"?gt3::createSio2DigitalPad(host):gt3::createSio2DualShock2(host);
}
class Gt3Loadfile final : public IopService {
public:
    explicit Gt3Loadfile(IopHost& host): host_(host),
        backend_(std::make_shared<gt3::Sio2NativeBackend>(host,padDevice(host))),
        resident_(host,*backend_,backend_),client_(host),mtap_(host,backend_),mcman_(host),mcserv_(host,&mcman_),padman_(host,true),libsd_(host),usbd_(host),ilink_(host),libpdi_(host),pdicdvd_(host),pdispu2_(host,libsd_),pdistr_(host,pdispu2_,pdicdvd_),pdi1394_(host),pdiusb_(host) {}
    ~Gt3Loadfile() override { reset(); }
    std::string_view name() const override { return "gt3-loadfile"; }
    std::span<const uint32_t> sids() const override { return ids_; }
    void onVBlank(bool start,uint64_t tick) override { padman_.onVBlank(start,tick); pdi1394_.onVBlank(start,tick); }
    void onSifTransfer(const SifTransfer& transfer) override { pdi1394_.onSifTransfer(transfer); }
    void reset() override {
        std::lock_guard lock(mutex_); detachSoundOutput(); const bool usbReleased=pdiusb_.reset(); pdi1394_.reset(); pdistr_.reset(); const bool soundReleased=pdispu2_.reset();
        pdicdvd_.reset(); libpdi_.reset(); ilink_.reset(); usbd_.reset(); libsd_.reset();
        if (!soundReleased) pdispu2_.reset(); // Broader IOP reset has now invalidated a failed restore token.
        if (!usbReleased) pdiusb_.reset(); // A poisoned USB catalog no longer owns the retained descriptor.
        padman_.reset(); mcserv_.reset(); mcman_.reset(); mtap_.reset(); client_.reset(); resident_.reset(); lastPath_.clear(); attempts_=0;
    }
    RpcResult handleRpc(const RpcRequest& request) override {
        if (request.sid==gt3::PdiusbResident::Sid) return pdiusb_.handleRpc(request);
        if (request.sid==0x4d494650 || request.sid==0x53494650) return pdi1394_.handleRpc(request);
        if (request.sid==0x50434456 || request.sid==0x50636476) return pdicdvd_.handleRpc(request);
        if (request.sid==0x53505550 || request.sid==0x53505556) return pdispu2_.handleRpc(request);
        if (request.sid==0x5042474d || request.sid==0x4d504731 || request.sid==0x4d504732 || request.sid==0x50444950) return pdistr_.handleRpc(request);
        if (request.sid==0x80000701) {
            // Supplied LIBSD is an IOP export library, not this generic RPC
            // shim. Do not turn missing GT3 audio behavior into a success reply.
            RpcResult result; result.serverDispatchPolicy=ServerDispatchPolicy::RejectUnhandled; return result;
        }
        if (request.sid==0x80000100 || request.sid==0x80000101) return padman_.handleRpc(request);
        if (request.sid==0x80000400 || request.sid==0x80000480) {
            auto result=mcserv_.handleRpc(request);
            result.serverDispatchPolicy=result.handled?ServerDispatchPolicy::Suppress:ServerDispatchPolicy::RejectUnhandled;
            return result;
        }
        if (request.sid != ids_[0]) return mtap_.handleRpc(request);
        if (request.function == 0xff) {
            // LOADFILE.IRX handler 0x5ac returns the word at module address 0x1cfc.
            constexpr uint32_t version = 0x30343232u;
            if (!request.receive.address || request.receive.size < 4 ||
                !host_.writeGuest(request.receive.address, &version, 4)) return {};
            host_.log(LogLevel::Info, "[GT3/LOADFILE] supplied module version replied");
            RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
            return result;
        }
        if (request.function == 0 && request.send.address && request.send.size >= 9 &&
            request.send.address <= 0xffffffffu - 260u) {
            // Load/start packet: arg length at 0, path at 8, arguments at 0x104.
            std::string path;
            bool terminated=false;
            for (uint32_t i=8; i<260 && i<request.send.size; ++i) {
                char c=0;
                if (!host_.readGuest(request.send.address+i,&c,1)) return {};
                if (!c) { terminated=true; break; }
                path.push_back(c);
            }
            if (terminated) {
                // Parse all input first: LOADFILE permits aliased send/receive buffers.
                uint32_t argumentBytes=0;
                if (!host_.readGuest(request.send.address,&argumentBytes,4)) return {};
                // Validate the reply before performing module side effects.
                const std::array<int32_t,2> failure{-1,-1};
                if (!request.receive.address || request.receive.size < sizeof(failure) ||
                    !host_.writeGuest(request.receive.address,failure.data(),sizeof(failure))) return {};
                std::lock_guard lock(mutex_);
                if (path != lastPath_) attempts_=0;
                if (attempts_ != UINT64_MAX) ++attempts_;
                if (path == "cdrom0:\\MEMCLEAR.IRX;1" && argumentBytes == 0 && host_.clearFreeIopMemory()) {
                    // Native equivalent of this nonresident module's entrypoint.
                    // 0x7000 is an HLE transaction ID, not a resident IRX address.
                    // Supplied MODLOAD 0x6a4 saves the ID before calling entry;
                    // 0xb34 writes entry's return (1) and 0xb48 unloads it.
                    const std::array<int32_t,2> success{0x7000,1};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) return {};
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] MEMCLEAR native HLE completed; free IOP heap cleared, active allocations preserved");
                } else if (path=="cdrom0:\\SIO2MAN.IRX;1" && !argumentBytes && startSio2()) {
                    // Native resident ID, not an original IOP/EE code address.
                    const std::array<int32_t,2> success{0x7001,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) {
                        client_.reset(); resident_.reset(); return {};
                    }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified SIO2MAN native resident started; queued digital endpoint/client transfer committed; id=0x7001 entry-return=0; other devices/commands remain unsupported");
                } else if (path=="cdrom0:\\MTAPMAN.IRX;1" && !argumentBytes && resident_.active() &&
                    (mtap_.active() || (verifiedModule(host_,path,"38cf336a18766d6fe2300939c720a2351871a42d756832ca1c8662b864ea47c4") && mtap_.start()))) {
                    const std::array<int32_t,2> success{0x7002,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { mtap_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified MTAPMAN native absent-device resident started; IOP callbacks/import probe/rescan worker active; id=0x7002 entry-return=0; priority overrides/attached multitaps unsupported");
                } else if (path=="cdrom0:\\MCMAN.IRX;1" && !argumentBytes && resident_.active() &&
                    (mcman_.active() || mcman_.publish())) {
                    const std::array<int32_t,2> success{0x7003,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { mcman_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified MCMAN native absent-card resident started; IOP cache/two semaphores/mc-device/security transport and boot-path callbacks active; id=0x7003 entry-return=0; attached media/block writes/authentication/contended waits unsupported");
                } else if (path=="cdrom0:\\MCSERV.IRX;1" && !argumentBytes && mcman_.active() &&
                    (mcserv_.active() || mcserv_.start())) {
                    const std::array<int32_t,2> success{0x7004,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { mcserv_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified MCSERV native RPC worker started; SID=0x80000400 version=0x20d mcman=0x217 library=0x201; IOP image/stack and MCMAN semaphore acquired; id=0x7004 entry-return=0; absent-card queries/basic errors supported, other RPC operations remain unsupported");
                } else if (path=="cdrom0:\\PADMAN.IRX;1" && !argumentBytes && resident_.active() &&
                    (padman_.active() || padman_.start())) {
                    const std::array<int32_t,2> success{0x7005,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { padman_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PADMAN native resident started; two queued RPC workers SID=0x80000100/101, library=0x203; id=0x7005 entry-return=0; frame-driven basic-digital port0 available after separate initialization; analog/pressure/rumble and other ports unsupported");
                } else if (path=="cdrom0:\\LIBSD.IRX;1" && !argumentBytes && (libsd_.active() || libsd_.start())) {
                    const std::array<int32_t,2> success{0x7006,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { libsd_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified LIBSD native entry completed; library=0x104, configuration exports5..10 and first cold SdInit(0) with native RAM/muted startup voices/events/IRQ paths; id=0x7006 entry-return=0; native SPU2 voices render through the host output clock; warm/repeated init and guest DMA remain unsupported");
                    attachSoundOutput();
                } else if (path=="cdrom0:\\USBD.IRX;1" && !argumentBytes && (usbd_.active() || usbd_.start())) {
                    const std::array<int32_t,2> success{0x7007,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { usbd_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified USBD native empty-bus entry completed; owned OHCI/pools,timed frames,IRQ/callback workers/events/semaphore and driver catalog; library=0x101 id=0x7007 entry-return=0; external registration requires explicit caller GP/native callbacks; attached USB devices and transfers unsupported");
                } else if (path=="cdrom0:\\ILINK.IRX;1" && !argumentBytes && (ilink_.active() || ilink_.start())) {
                    const std::array<int32_t,2> success{0x7008,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { ilink_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified ILINK native entry registered library=0x202; original BSS initialization count remains zero, queried uninitialized paths return -1000; id=0x7008 entry-return=0; separate hardware/network initialization and transfers remain unsupported");
                } else if (path=="cdrom0:\\LIBPDI.IRX;1" && !argumentBytes && (libpdi_.active() || libpdi_.start())) {
                    const std::array<int32_t,2> success{0x7009,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { libpdi_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified LIBPDI native entry registered library=0x10a and committed original initialization flag; strict IOP memcpy,owned allocation/free,monotonic seconds/date and debug-tag exports4/11/14/96/98/99; id=0x7009 entry-return=0; allocation exhaustion/retry,file/thread/semaphore/SIF utilities remain unsupported");
                } else if (path=="cdrom0:\\PDICDVD.IRX;1" && !argumentBytes && libpdi_.active() && (pdicdvd_.active() || pdicdvd_.start())) {
                    const std::array<int32_t,2> success{0x700a,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { pdicdvd_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PDICDVD native startup completed; four workers/stacks, owned synchronization and two queued RPC services 0x50434456/0x50636476; calendar uses native UTC profile, version/config/time/idle-cancel and job-init supported; id=0x700a entry-return=0; bounded flags0/1 reads require a configured ISO; general streaming/cancellation remain unsupported");
                } else if (path=="cdrom0:\\PDISPU2.IRX;1" && !argumentBytes && libpdi_.active() && libsd_.active() &&
                    (pdispu2_.active() || pdispu2_.start())) {
                    const std::array<int32_t,2> success{0x700b,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { pdispu2_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PDISPU2 native startup completed;140 native sound imports,owned sound RAM/effect clears/initial IRQ callback,4 workers/8 synchronization resources; SIDs0x53505550/0x53505556 version RPC0 and volume descriptor4; id=0x700b entry-return=0; audio streaming/playback and other RPCs remain unsupported");
                } else if (path=="cdrom0:\\PDISTR.IRX;1" && !argumentBytes && libpdi_.active() && pdicdvd_.active() && pdispu2_.active() &&
                    (pdistr_.active() || pdistr_.start())) {
                    const std::array<int32_t,2> success{0x700c,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { pdistr_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PDISTR native startup completed; relocated module data,real sound volume descriptor,5 workers/4 synchronization resources/4 RPC queues; version106 on SID0x50444950,other initial null replies,idle cancel export8/MPG RPC4; id=0x700c entry-return=0; bounded disc stream RPC1..4 with native sound-RAM delivery supported; file/reverse/playback/video jobs remain unsupported");
                } else if (path=="cdrom0:\\PDI1394.IRX;1" && !argumentBytes && ilink_.active() &&
                    (pdi1394_.active() || pdi1394_.start())) {
                    const std::array<int32_t,2> success{0x700d,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { pdi1394_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PDI1394 image; native i.LINK protocol over the network (GT3_LINK_MODE); id=0x700d entry-return=0");
                } else if (path=="cdrom0:\\PDIUSB.IRX;1" && !argumentBytes && libpdi_.active() && usbd_.active() &&
                    (pdiusb_.active() || pdiusb_.start())) {
                    const std::array<int32_t,2> success{0x700e,0};
                    if (!host_.writeGuest(request.receive.address,success.data(),sizeof(success))) { pdiusb_.reset(); return {}; }
                    host_.log(LogLevel::Info,"[GT3/LOADFILE] hash-verified PDIUSB native entry completed;LIBPDI context/request/reply allocations,2 workers/count0 resources,real USBD empty-bus driver registration and queued SID=0x594f4a50; id=0x700e entry-return=0; absent-device queries only,attached USB/transfers/reverse EE services unsupported");
                } else if (attempts_ == 1 || (attempts_ & (attempts_-1)) == 0) {
                    // CORE 0x25ddc8 retries negative loads forever via 0x269ba0.
                    // Report powers of two to expose stalls without flooding logs.
                    if (path=="cdrom0:\\PDI1394.IRX;1") {
                        // Original worker8dc waits for this real EE registration
                        // before invoking ILINK4. Observe that boot prerequisite
                        // without manufacturing a binding or accepting the load.
                        const auto ee=host_.findEeRpcServer(0x45574650);
                        host_.log(LogLevel::Info,"[GT3/PDI1394 dependency] EE SID=0x45574650 lookup="+
                            std::string(ee.supported?"supported":"unsupported")+
                            "; registration="+std::to_string(ee.registration)+
                            "; server="+std::to_string(ee.server)+"; function="+std::to_string(ee.function)+
                            "; queue="+std::to_string(ee.queue)+"; no binding or ILINK initialization performed");
                    }
                    host_.log(LogLevel::Warning, "[GT3/LOADFILE] unsupported module load: " + path +
                        "; attempt=" + std::to_string(attempts_) +
                        "; argument-bytes=" + std::to_string(argumentBytes) +
                        "; reply=(-1,-1); CORE startup cannot advance this load");
                }
                lastPath_=path;
                RpcResult result; result.handled=true; result.resultAddress=request.receive.address;
                return result;
            }
        }
        // Unimplemented RPC operations remain unhandled.
        return {};
    }
private:
    bool startSio2() {
        if (resident_.active()) return true;
        // Addresses/packet constants below belong only to these supplied IRXs.
        if (!verifiedModule(host_,"cdrom0:\\SIO2MAN.IRX;1","4e12becc4fe5c6e9cf082417ed70eb7b9b521ed9ee122f73ef2b1478f06494d6") ||
            !verifiedModule(host_,"cdrom0:\\PADMAN.IRX;1","10120230a66017c200c5ce8e4b9f11c42ffb240bc2df9d194e5d2624625df5d7") || !resident_.start()) return false;
        std::array<uint8_t,4> packet{};
        try { if (client_.poll(packet)) return true; }
        catch (...) { client_.reset(); resident_.reset(); throw; }
        client_.reset(); resident_.reset(); return false;
    }
    inline static constexpr std::array<uint32_t,22> ids_{0x80000006u,0x80000901u,0x80000902u,0x80000903u,0x80000904u,0x80000905u,0x80000400u,0x80000480u,0x80000100u,0x80000101u,0x80000701u,0x50434456u,0x50636476u,0x53505550u,0x53505556u,0x5042474du,0x4d504731u,0x4d504732u,0x50444950u,0x4d494650u,0x53494650u,0x594f4a50u};
    // Output clock (stage 4): the host's sound thread advances the native SPU2
    // voices in real time, which raises the SPU2 IRQs native sound drivers
    // (PDISPU2 movie audio) wait on. Attached once LIBSD is running; detached
    // first on reset, before any resident is torn down.
    void attachSoundOutput() {
        if (soundToken_) return;
        soundFailureLogged_.store(false);
        soundToken_=host_.attachSoundSource([this](uint32_t frames,std::span<int16_t> stereo) {
            if (!libsd_.soundInitialized()) return false; // silence until SdInit
            if (libsd_.renderSound(frames,stereo)) return true;
            if (!soundFailureLogged_.exchange(true))
                host_.log(LogLevel::Error,"[GT3/SOUND] native SPU2 render or IRQ delivery failed; the chunk was silenced (logged once per attach)");
            return false;
        });
        if (soundToken_) host_.log(LogLevel::Info,"[GT3/SOUND] native SPU2 output clock attached (48 kHz, host sound thread)");
        else host_.log(LogLevel::Warning,"[GT3/SOUND] host has no output clock; native SPU2 voices and their IRQs will not advance, so audio-driven services stay blocked");
    }
    void detachSoundOutput() {
        if (!soundToken_) return;
        host_.detachSoundSource(soundToken_); soundToken_=0;
    }
    IopHost& host_; std::mutex mutex_; std::string lastPath_;
    uint64_t soundToken_=0; std::atomic<bool> soundFailureLogged_{false};
    uint64_t attempts_=0;
    std::shared_ptr<gt3::Sio2NativeBackend> backend_;
    gt3::Sio2Resident resident_;
    gt3::Sio2DigitalPadClient client_;
    gt3::MtapResident mtap_;
    gt3::McmanNativeExports mcman_;
    gt3::McservResident mcserv_;
    gt3::PadmanResident padman_;
    gt3::LibsdResident libsd_;
    gt3::UsbdResident usbd_;
    gt3::IlinkResident ilink_;
    gt3::LibpdiResident libpdi_;
    gt3::PdicdvdResident pdicdvd_;
    gt3::Pdispu2Resident pdispu2_;
    gt3::PdistrResident pdistr_;
    gt3::Pdi1394Resident pdi1394_;
    gt3::PdiusbResident pdiusb_;
};
}
std::unique_ptr<IopService> createGt3LoadfileService(IopHost& host) {
    return std::make_unique<Gt3Loadfile>(host);
}
}
