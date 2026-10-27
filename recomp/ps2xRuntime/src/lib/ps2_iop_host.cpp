#include "ps2_iop_host.h"
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
#include "gt3_autopilot.h"
#include "gt3_disc_patch.h"
#include "gt3_drive_log.h"
#endif
#if defined(PS2X_GAME_GT4)
#include "gt4_cheat_card.h" // GT4_CHEAT_CARD (the card follows port 0)
#endif

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Syscalls/Common.h"
#if defined(PS2X_GAME_GT4)
#include "Kernel/Stubs/Gt4Pad2.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <utility>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

ps2x::iop::NativeLibraryRegistry* PS2IopHostAdapter::nativeLibraries() { return m_runtime.nativeIopLibraries(); }
ps2x::iop::EeRpcServerSnapshot PS2IopHostAdapter::findEeRpcServer(uint32_t sid) const { return m_runtime.findEeRpcServer(sid); }

PS2IopHostAdapter::CallScope::CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram)
    : m_lock(owner.m_callMutex),
      m_owner(&owner),
      m_previousContext(owner.m_activeContext),
      m_previousRdram(owner.m_activeRdram),
      m_previousToken(owner.m_activeToken)
{
    m_token = owner.m_nextToken++;
    if (m_token == 0)
    {
        m_token = owner.m_nextToken++;
    }
    owner.m_activeContext = context;
    owner.m_activeRdram = rdram;
    owner.m_activeToken = m_token;
}

PS2IopHostAdapter::CallScope::~CallScope()
{
    release();
}

PS2IopHostAdapter::CallScope::CallScope(CallScope &&other) noexcept
    : m_lock(std::move(other.m_lock)),
      m_owner(std::exchange(other.m_owner, nullptr)),
      m_previousContext(other.m_previousContext),
      m_previousRdram(other.m_previousRdram),
      m_previousToken(other.m_previousToken),
      m_token(other.m_token)
{
}

PS2IopHostAdapter::CallScope &PS2IopHostAdapter::CallScope::operator=(CallScope &&other) noexcept
{
    if (this != &other)
    {
        release();
        m_lock = std::move(other.m_lock);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_previousContext = other.m_previousContext;
        m_previousRdram = other.m_previousRdram;
        m_previousToken = other.m_previousToken;
        m_token = other.m_token;
    }
    return *this;
}

void PS2IopHostAdapter::CallScope::release()
{
    if (m_owner)
    {
        m_owner->m_activeContext = m_previousContext;
        m_owner->m_activeRdram = m_previousRdram;
        m_owner->m_activeToken = m_previousToken;
        m_owner = nullptr;
    }
}

PS2IopHostAdapter::PS2IopHostAdapter(PS2Runtime &runtime)
    : m_runtime(runtime)
{
    if (const char* path=std::getenv("GT3_PAD_REPLAY"); path && *path) {
        std::ifstream input(path);
        std::string error;
        if (!input || !m_padReplay.load(input,error))
            throw std::runtime_error("Cannot load GT3_PAD_REPLAY: "+std::string(path)+" "+error);
        std::fprintf(stderr,"[input-replay] Synthetic frame input enabled: %s; physical input is overridden.\n",path);
    }
}

PS2IopHostAdapter::~PS2IopHostAdapter()
{
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    for (auto &[handle, file] : m_hostFiles)
    {
        (void)handle;
        if (file.stream)
        {
            std::fclose(file.stream);
        }
    }
    m_hostFiles.clear();
}

PS2IopHostAdapter::CallScope PS2IopHostAdapter::enterCall(R5900Context *context, uint8_t *rdram)
{
    return CallScope(*this, context, rdram);
}

bool PS2IopHostAdapter::guestRange(uint32_t address, size_t size, uint8_t *&begin) const
{
    begin = nullptr;
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return false;
    }
    if (size == 0)
    {
        begin = getMemPtr(rdram, address);
        return begin != nullptr;
    }
    if (size - 1 > std::numeric_limits<uint32_t>::max() - address)
    {
        return false;
    }
    uint8_t *const first = getMemPtr(rdram, address);
    uint8_t *const last = getMemPtr(rdram, address + static_cast<uint32_t>(size - 1));
    if (!first || !last || last < first || static_cast<size_t>(last - first) != size - 1)
    {
        return false;
    }
    begin = first;
    return true;
}

bool PS2IopHostAdapter::readGuest(uint32_t address, void *destination, size_t size) const
{
    if (!destination && size != 0)
    {
        return false;
    }
    if (ps2_stubs::isSifIopHeapAddress(address))
    {
        return ps2_stubs::readSifIopHeap(address, destination, size);
    }

    uint8_t *source = nullptr;
    if (!guestRange(address, size, source))
    {
        return false;
    }
    if (size != 0)
    {
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::writeGuest(uint32_t address, const void *source, size_t size)
{
    if (!source && size != 0)
    {
        return false;
    }
    if (ps2_stubs::isSifIopHeapAddress(address))
    {
        return ps2_stubs::writeSifIopHeap(address, source, size);
    }

    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::writeGuest", nullptr);
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::zeroGuest(uint32_t address, size_t size)
{
    if (ps2_stubs::isSifIopHeapAddress(address))
    {
        return ps2_stubs::zeroSifIopHeap(address, size);
    }

    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::zeroGuest", nullptr);
        std::memset(destination, 0, size);
    }
    return true;
}

bool PS2IopHostAdapter::normalizeGuestAddress(uint32_t address, uint32_t &normalized) const
{
    if (ps2_stubs::isSifIopHeapAddress(address))
    {
        normalized = address;
        return ps2_stubs::isSifIopHeapRange(address, 0u);
    }

    bool scratchpad = false;
    if (!ps2ResolveGuestPointer(address, normalized, scratchpad) || scratchpad)
    {
        normalized = 0;
        return false;
    }
    return true;
}

uint32_t PS2IopHostAdapter::allocateIopHandle(ps2x::iop::IopHandleKind kind)
{
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return 0;
    }
    return kind == ps2x::iop::IopHandleKind::RpcPacket
               ? rpcAllocPacketAddr(rdram)
               : rpcAllocServerAddr(rdram);
}

uint32_t PS2IopHostAdapter::allocateIopMemory(uint32_t size) { return ps2_stubs::allocateNativeIopMemory(size); }
uint32_t PS2IopHostAdapter::allocateIopMemoryLast(uint32_t size) { return ps2_stubs::allocateNativeIopMemoryLast(size); }
uint32_t PS2IopHostAdapter::maxFreeIopMemory() const { return ps2_stubs::maxFreeNativeIopMemory(); }
bool PS2IopHostAdapter::readIopMemory(uint32_t address,void* destination,size_t size) const {
    return ps2_stubs::readSifIopHeap(address,destination,size);
}
void PS2IopHostAdapter::freeIopMemory(uint32_t address) { ps2_stubs::freeNativeIopMemory(address); }
bool PS2IopHostAdapter::commitPadMemory(std::span<const ps2x::iop::IopMemoryWrite> writes)
{
    return commitIopAndEeMemory(writes);
}
bool PS2IopHostAdapter::commitIopAndEeMemory(std::span<const ps2x::iop::IopMemoryWrite> writes)
{
    std::lock_guard lock(m_callMutex);
    return ps2_stubs::commitNativePadMemory(m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM(),writes);
}
void PS2IopHostAdapter::setPadActuators(bool smallMotor, uint8_t large)
{
    // First activation always logged; every change with GT3_RUMBLE_LOG=1.
    static const bool traceAll = std::getenv("GT3_RUMBLE_LOG") != nullptr;
    static std::atomic<bool> first{true};
    if (traceAll || ((smallMotor || large) && first.exchange(false)))
        std::fprintf(stderr, "[input] DualShock 2 motors: small=%d large=%u (VSync %llu)\n", smallMotor ? 1 : 0,
                     static_cast<unsigned>(large),
                     static_cast<unsigned long long>(m_runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed)));
    m_runtime.padBackend().setActuators(smallMotor, large);
}
bool PS2IopHostAdapter::readPortPadSnapshot(unsigned port, std::array<uint8_t,32> &packet) const
{
    if (port == 0)
        return readPadSnapshot(packet);
#if defined(PS2X_GAME_GT4)
    if (port == 1)
        return gt4pad2::readState(m_runtime.memory().gs().vsyncTick.load(std::memory_order_acquire), packet);
#endif
    return false;
}
void PS2IopHostAdapter::setPortPadActuators(unsigned port, bool smallMotor, uint8_t large)
{
    if (port == 0)
    {
        setPadActuators(smallMotor, large);
        return;
    }
#if defined(PS2X_GAME_GT4)
    if (port == 1) // player 2 (its GT3_RUMBLE_LOG line is gt4pad2's)
        gt4pad2::setActuators(smallMotor, large, m_runtime.memory().gs().vsyncTick.load(std::memory_order_relaxed));
#endif
}
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
// GT4 Multi Monitor Mode: controller 1 shared with / followed by the other copies (GT4_MM_PAD, gt4_multimon_pad.cpp).
namespace gt4mm { bool padFilter(uint64_t tick, std::array<uint8_t, 32> &packet, bool ok); }
#endif
bool PS2IopHostAdapter::readPadSnapshot(std::array<uint8_t,32> &packet) const
{
    const auto tick=m_runtime.memory().gs().vsyncTick.load(std::memory_order_acquire);
    bool ok=false, autopilot=false;
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
    // GT3_AUTOPILOT: in races the autopilot drives (gt3_autopilot.h).
    if (gt3autopilot::drive(m_runtime.memory().getRDRAM(),tick,packet))
        ok=autopilot=true;
#endif
    if (!ok && m_padReplay.sample(tick,packet)) {
        const auto buttons=m_padReplay.pressed(tick);
        if (buttons!=m_lastReplayButtons) {
            std::fprintf(stderr,"[input-replay] VSync=%llu pressed=%04x\n",
                static_cast<unsigned long long>(tick),static_cast<unsigned>(buttons));
            m_lastReplayButtons=buttons;
        }
        ok=true;
    }
    if (!ok)
        ok=m_runtime.padBackend().readState(0,0,packet.data(),packet.size());
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    ok=gt4mm::padFilter(tick,packet,ok);
#endif
#if defined(PS2X_GAME_GT4)
    if (ok)
        gt4cheatcard::onPad(0,packet.data(),packet.size()); // the cheat card follows port 0 (gt4_cheat_card.h)
#endif
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
    // GT3_DRIVE_LOG: the laps driven, with the inputs GT3 got (gt3_drive_log.h).
    if (ok && gt3drivelog::enabled())
        gt3drivelog::onPad(m_runtime.memory().getRDRAM(),tick,packet,autopilot);
#endif
    return ok;
}

uint32_t PS2IopHostAdapter::allocateGuest(uint32_t size, uint32_t alignment)
{
    return m_runtime.guestMalloc(size, alignment);
}

void PS2IopHostAdapter::freeGuest(uint32_t address)
{
    m_runtime.guestFree(address);
}

void PS2IopHostAdapter::audioCommand(uint32_t sid,
                                     uint32_t function,
                                     ps2x::iop::GuestBuffer send,
                                     ps2x::iop::GuestBuffer receive)
{
    uint8_t *sendPointer = nullptr;
    uint8_t *receivePointer = nullptr;
    if (send.address && !guestRange(send.address, send.size, sendPointer))
    {
        sendPointer = nullptr;
    }
    if (receive.address && !guestRange(receive.address, receive.size, receivePointer))
    {
        receivePointer = nullptr;
    }
    m_runtime.audioBackend().onSoundCommand(sid,
                                            function,
                                            sendPointer,
                                            send.size,
                                            receivePointer,
                                            receive.size);
}

std::string PS2IopHostAdapter::hostPath(ps2x::iop::HostPathKind kind) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    switch (kind)
    {
    case ps2x::iop::HostPathKind::CdRoot:
        return paths.cdRoot.string();
    case ps2x::iop::HostPathKind::CdImage:
        return paths.cdImage.string();
    case ps2x::iop::HostPathKind::HostRoot:
        return paths.hostRoot.string();
    case ps2x::iop::HostPathKind::MemoryCardRoot:
        return paths.mcRoot.string();
    case ps2x::iop::HostPathKind::ElfDirectory:
    default:
        return paths.elfDirectory.string();
    }
}

std::string PS2IopHostAdapter::translateGuestPath(std::string_view path) const
{
    return translatePs2Path(std::string(path).c_str());
}

uint64_t PS2IopHostAdapter::openHostFile(std::string_view path)
{
    if (path.empty())
    {
        return 0u;
    }

    const std::filesystem::path hostPath{std::string(path)};
#if defined(_WIN32)
    std::FILE *stream = ::_wfopen(hostPath.c_str(), L"rb");
#else
    std::FILE *stream = std::fopen(hostPath.string().c_str(), "rb");
#endif
    if (!stream)
    {
        return 0u;
    }

    std::error_code error;
    const uint64_t size = std::filesystem::file_size(hostPath, error);
    if (error)
    {
        std::fclose(stream);
        return 0u;
    }

    bool discImage = false;
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
    if (gt3discpatch::enabled())
    {
        std::error_code same;
        discImage = std::filesystem::equivalent(hostPath, PS2Runtime::getIoPaths().cdImage, same) && !same;
    }
#endif

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    uint64_t handle = m_nextHostFileHandle++;
    if (handle == 0u)
    {
        handle = m_nextHostFileHandle++;
    }
    m_hostFiles.emplace(handle, HostFile{stream, size, discImage});
    return handle;
}

bool PS2IopHostAdapter::hostFileSize(uint64_t handle, uint64_t &size) const
{
    size = 0u;
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }
    size = it->second.size;
    return true;
}

bool PS2IopHostAdapter::readHostFile(uint64_t handle,
                                     uint64_t offset,
                                     void *destination,
                                     size_t size,
                                     size_t &bytesRead)
{
    bytesRead = 0u;
    if (!destination && size != 0u)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }

#if defined(_WIN32)
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        _fseeki64(it->second.stream, static_cast<int64_t>(offset), SEEK_SET) != 0)
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(it->second.stream, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
    {
        return false;
    }

    bytesRead = size == 0u
                    ? 0u
                    : std::fread(destination, 1u, size, it->second.stream);
    if (bytesRead < size && std::ferror(it->second.stream))
    {
        std::clearerr(it->second.stream);
        return false;
    }
#if !defined(GT3_STOCK_BUILD) && !defined(PS2X_GAME_GT4)
    if (it->second.discImage)
        gt3discpatch::overlay(offset, static_cast<uint8_t *>(destination), bytesRead);
#endif
    return true;
}

void PS2IopHostAdapter::closeHostFile(uint64_t handle)
{
    std::FILE *stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_hostFileMutex);
        const auto it = m_hostFiles.find(handle);
        if (it == m_hostFiles.end())
        {
            return;
        }
        stream = it->second.stream;
        m_hostFiles.erase(it);
    }
    if (stream)
    {
        std::fclose(stream);
    }
}

int32_t PS2IopHostAdapter::memoryCard(const ps2x::iop::MemoryCardRequest &request)
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    Handler handler = nullptr;
    switch (request.operation)
    {
    case ps2x::iop::MemoryCardOperation::Init:
        handler = ps2_stubs::sceMcInit;
        break;
    case ps2x::iop::MemoryCardOperation::GetInfo:
        handler = ps2_stubs::sceMcGetInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Open:
        handler = ps2_stubs::sceMcOpen;
        break;
    case ps2x::iop::MemoryCardOperation::Close:
        handler = ps2_stubs::sceMcClose;
        break;
    case ps2x::iop::MemoryCardOperation::Seek:
        handler = ps2_stubs::sceMcSeek;
        break;
    case ps2x::iop::MemoryCardOperation::Read:
        handler = ps2_stubs::sceMcRead;
        break;
    case ps2x::iop::MemoryCardOperation::Write:
        handler = ps2_stubs::sceMcWrite;
        break;
    case ps2x::iop::MemoryCardOperation::Flush:
        handler = ps2_stubs::sceMcFlush;
        break;
    case ps2x::iop::MemoryCardOperation::Chdir:
        handler = ps2_stubs::sceMcChdir;
        break;
    case ps2x::iop::MemoryCardOperation::GetDir:
        handler = ps2_stubs::sceMcGetDir;
        break;
    case ps2x::iop::MemoryCardOperation::SetFileInfo:
        handler = ps2_stubs::sceMcSetFileInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Delete:
        handler = ps2_stubs::sceMcDelete;
        break;
    case ps2x::iop::MemoryCardOperation::Format:
        handler = ps2_stubs::sceMcFormat;
        break;
    case ps2x::iop::MemoryCardOperation::Unformat:
        handler = ps2_stubs::sceMcUnformat;
        break;
    case ps2x::iop::MemoryCardOperation::Mkdir:
        handler = ps2_stubs::sceMcMkdir;
        break;
    }
    if (!handler)
    {
        return -1;
    }

    R5900Context context{};
    for (size_t i = 0; i < 4u; ++i)
    {
        setRegU32(&context, static_cast<int>(4 + i), request.arguments[i]);
    }

    // EE n32 ABI: the fifth argument travels in $t0, matching the sceMc* stubs.
    setRegU32(&context, 8, request.arguments[4]);
    setRegU32(&context, 9, request.arguments[5]);

    handler(m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM(),
            &context,
            &m_runtime);
    return ps2_stubs::getMemoryCardDebugSnapshot().lastResult;
}

bool PS2IopHostAdapter::hasGuestFunction(uint32_t address) const
{
    return m_runtime.hasFunction(address);
}

bool PS2IopHostAdapter::invokeGuestFunction(uint64_t callToken,
                                            uint32_t address,
                                            uint32_t a0,
                                            uint32_t a1,
                                            uint32_t a2,
                                            uint32_t a3,
                                            uint32_t *resultAddress)
{
    (void)callToken;
    (void)address;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    if (resultAddress)
    {
        *resultAddress = 0u;
    }
    return false;
}

void PS2IopHostAdapter::log(ps2x::iop::LogLevel level, std::string_view message)
{
    const char *prefix = "[ps2xIOP]";
    if (level == ps2x::iop::LogLevel::Warning)
    {
        prefix = "[ps2xIOP:warning]";
    }
    else if (level == ps2x::iop::LogLevel::Error)
    {
        prefix = "[ps2xIOP:error]";
    }
    std::cerr << prefix << ' ' << message << std::endl;
}

bool PS2IopHostAdapter::clearFreeIopMemory()
{
    ps2_stubs::clearFreeSifIopHeap();
    return true;
}

bool PS2IopHostAdapter::writeIopMemoryBatch(std::span<const ps2x::iop::IopMemoryWrite> writes)
{
    return ps2_stubs::writeSifIopHeapBatch(writes);
}

#if defined(PS2X_GAME_GT3)
ps2x::iop::gt3::Sio2Result PS2IopHostAdapter::runSio2Transfer(uint32_t packet, uint32_t sessionRequest,
    ps2x::iop::gt3::Sio2Device* device, uint32_t& events)
{
    std::lock_guard lock(m_callMutex);
    return ps2x::iop::gt3::executeSio2Transfer(*this,packet,sessionRequest,device,events);
}
#endif

uint64_t PS2IopHostAdapter::attachSoundSource(SoundSource source)
{
    if (!source)
        return 0;
    const char *forced = std::getenv("GT3_SOUND_OUTPUT");
    const bool useDevice = !(forced && std::string(forced) == "wallclock");
    {
        std::lock_guard<std::mutex> lock(m_soundMutex);
        if (m_sound)
            return 0;
        m_soundToken = m_nextSoundToken++;
        // Constructed under the lock: the sound thread starts here but the
        // source never takes m_soundMutex.
        m_sound = std::make_unique<PS2SoundOutput>(std::move(source), useDevice);
        const auto token = m_soundToken;
        const auto text = m_sound->describe();
        std::fprintf(stderr, "[sound-output] attached token %llu: %s\n", (unsigned long long)token, text.c_str());
        return token;
    }
}

bool PS2IopHostAdapter::detachSoundSource(uint64_t token)
{
    std::unique_ptr<PS2SoundOutput> output;
    {
        std::lock_guard<std::mutex> lock(m_soundMutex);
        if (!m_sound || token == 0 || token != m_soundToken)
            return false;
        output = std::move(m_sound);
        m_soundToken = 0;
    }
    const auto text = output->describe();
    output.reset(); // joins the sound thread outside m_soundMutex
    std::fprintf(stderr, "[sound-output] detached token %llu: %s\n", (unsigned long long)token, text.c_str());
    return true;
}

bool PS2IopHostAdapter::wakeupEeThread(int id)
{
    // interruptSafe: the IOP calls this from vblank event processing, where
    // the EE SIF command handler's iWakeupThread would run.
    return m_runtime.eeScheduler().wakeupThread(id, true) == 0;
}

bool PS2IopHostAdapter::signalEeSemaphore(int id)
{
    return m_runtime.eeScheduler().signalSemaphore(id, true) >= 0;
}

std::optional<PS2SoundOutput::Stats> PS2IopHostAdapter::soundOutputStats() const
{
    std::lock_guard<std::mutex> lock(m_soundMutex);
    if (!m_sound)
        return std::nullopt;
    return m_sound->stats();
}
