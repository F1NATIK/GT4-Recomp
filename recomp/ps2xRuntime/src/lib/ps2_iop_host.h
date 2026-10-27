#pragma once

#include "ps2x/iop/iop_host.h"
#if defined(PS2X_GAME_GT3)
#include "ps2x/iop/gt3/sio2_transfer.h"
#endif
#include "runtime/ps2_pad_replay.h"
#include "ps2_sound_output.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <memory>
#include <mutex>
#include <unordered_map>

class PS2Runtime;
struct R5900Context;

class PS2IopHostAdapter final : public ps2x::iop::IopHost
{
public:
    class CallScope
    {
    public:
        CallScope() = default;
        CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram);
        ~CallScope();

        CallScope(const CallScope &) = delete;
        CallScope &operator=(const CallScope &) = delete;
        CallScope(CallScope &&other) noexcept;
        CallScope &operator=(CallScope &&other) noexcept;

        [[nodiscard]] uint64_t token() const { return m_token; }

    private:
        void release();

        std::unique_lock<std::recursive_mutex> m_lock;
        PS2IopHostAdapter *m_owner = nullptr;
        R5900Context *m_previousContext = nullptr;
        uint8_t *m_previousRdram = nullptr;
        uint64_t m_previousToken = 0;
        uint64_t m_token = 0;
    };

    explicit PS2IopHostAdapter(PS2Runtime &runtime);
    ps2x::iop::NativeLibraryRegistry* nativeLibraries() override;
    ps2x::iop::EeRpcServerSnapshot findEeRpcServer(uint32_t sid) const override;
    uint32_t allocateIopMemory(uint32_t size) override;
    uint32_t allocateIopMemoryLast(uint32_t size) override;
    uint32_t maxFreeIopMemory() const override;
    void freeIopMemory(uint32_t address) override;
    bool readIopMemory(uint32_t address,void* destination,size_t size) const override;
    bool commitPadMemory(std::span<const ps2x::iop::IopMemoryWrite> writes) override;
    bool commitIopAndEeMemory(std::span<const ps2x::iop::IopMemoryWrite> writes) override;
    bool readPadSnapshot(std::array<uint8_t,32> &packet) const override;
    void setPadActuators(bool smallMotor, uint8_t large) override;
    // GT4 DBCMAN port 2 = player 2 (Kernel/Stubs/Gt4Pad2.h); port 1 = the two above.
    bool readPortPadSnapshot(unsigned port, std::array<uint8_t,32> &packet) const override;
    void setPortPadActuators(unsigned port, bool smallMotor, uint8_t large) override;
    bool sha256(std::span<const uint8_t>,std::array<uint8_t,32>&) const override;
    // Output clock (stage 4): raylib audio device when ready, else wall clock.
    // GT3_SOUND_OUTPUT=wallclock forces the wall clock (diagnostics).
    uint64_t attachSoundSource(SoundSource source) override;
    bool detachSoundSource(uint64_t token) override;
    bool wakeupEeThread(int id) override;
    bool signalEeSemaphore(int id) override;
    std::optional<PS2SoundOutput::Stats> soundOutputStats() const;
    ~PS2IopHostAdapter() override;

    [[nodiscard]] CallScope enterCall(R5900Context *context,
                                      uint8_t *rdram = nullptr);

    bool readGuest(uint32_t address, void *destination, size_t size) const override;
    bool writeGuest(uint32_t address, const void *source, size_t size) override;
    bool zeroGuest(uint32_t address, size_t size) override;
#if defined(PS2X_GAME_GT3)
    ps2x::iop::gt3::Sio2Result runSio2Transfer(uint32_t packet, uint32_t sessionRequest,
        ps2x::iop::gt3::Sio2Device* device, uint32_t& events);
#endif
    bool clearFreeIopMemory() override;
    bool writeIopMemoryBatch(std::span<const ps2x::iop::IopMemoryWrite> writes) override;
    bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override;
    uint32_t allocateIopHandle(ps2x::iop::IopHandleKind kind) override;
    uint32_t allocateGuest(uint32_t size, uint32_t alignment) override;
    void freeGuest(uint32_t address) override;

    void audioCommand(uint32_t sid,
                      uint32_t function,
                      ps2x::iop::GuestBuffer send,
                      ps2x::iop::GuestBuffer receive) override;

    std::string hostPath(ps2x::iop::HostPathKind kind) const override;
    std::string translateGuestPath(std::string_view path) const override;
    uint64_t openHostFile(std::string_view path) override;
    bool hostFileSize(uint64_t handle, uint64_t &size) const override;
    bool readHostFile(uint64_t handle,
                      uint64_t offset,
                      void *destination,
                      size_t size,
                      size_t &bytesRead) override;
    void closeHostFile(uint64_t handle) override;

    int32_t memoryCard(const ps2x::iop::MemoryCardRequest &request) override;

    bool hasGuestFunction(uint32_t address) const override;
    bool invokeGuestFunction(uint64_t callToken,
                             uint32_t address,
                             uint32_t a0,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t *resultAddress) override;

    void log(ps2x::iop::LogLevel level, std::string_view message) override;

private:
    friend class CallScope;

    bool guestRange(uint32_t address, size_t size, uint8_t *&begin) const;

    PS2Runtime &m_runtime;
    PSPadReplay m_padReplay;
    mutable uint32_t m_lastReplayButtons = 0x10000;
    std::recursive_mutex m_callMutex;
    R5900Context *m_activeContext = nullptr;
    uint8_t *m_activeRdram = nullptr;
    uint64_t m_activeToken = 0;
    uint64_t m_nextToken = 1;
    struct HostFile
    {
        std::FILE *stream = nullptr;
        uint64_t size = 0u;
        bool discImage = false; // the CD image (GT3_DISC_PATCH overlays its reads)
    };
    mutable std::mutex m_hostFileMutex;
    std::unordered_map<uint64_t, HostFile> m_hostFiles;
    uint64_t m_nextHostFileHandle = 1u;
    mutable std::mutex m_soundMutex;
    std::unique_ptr<PS2SoundOutput> m_sound;
    uint64_t m_soundToken = 0;
    uint64_t m_nextSoundToken = 1;
};
