#pragma once

#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <span>

namespace ps2x::iop
{
    class NativeLibraryRegistry;
    enum class HostPathKind : uint32_t
    {
        ElfDirectory = 0,
        CdRoot = 1,
        CdImage = 2,
        HostRoot = 3,
        MemoryCardRoot = 4,
    };

    enum class LogLevel : uint32_t
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
    };

    enum class MemoryCardOperation : uint32_t
    {
        Init,
        GetInfo,
        Open,
        Close,
        Seek,
        Read,
        Write,
        Flush,
        Chdir,
        GetDir,
        SetFileInfo,
        Delete,
        Format,
        Unformat,
        Mkdir,
    };

    struct MemoryCardRequest
    {
        MemoryCardOperation operation = MemoryCardOperation::Init;
        // The fifth and sixth arguments are carried in $t0/$t1 by the EE n32 ABI
        std::array<uint32_t, 6> arguments{};
    };

    struct IopMemoryWrite { uint32_t address; std::span<const uint8_t> bytes; };

    class IopHost
    {
    public:
        virtual NativeLibraryRegistry* nativeLibraries() { return nullptr; }
        virtual EeRpcServerSnapshot findEeRpcServer(uint32_t) const { return {}; }
        virtual uint32_t allocateIopMemory(uint32_t) { return 0; }
        // Last suitable free block, allocated from its upper end (sysmem mode1).
        virtual uint32_t allocateIopMemoryLast(uint32_t) { return 0; }
        virtual uint32_t maxFreeIopMemory() const { return 0; }
        virtual void freeIopMemory(uint32_t) {}
        // Strict allocated IOP reads; never accept an EE address or alias.
        virtual bool readIopMemory(uint32_t,void*,size_t) const { return false; }
        virtual bool commitPadMemory(std::span<const IopMemoryWrite>) { return false; }
        // Atomic RPC replies and data DMA to allocated IOP memory/strict EE RAM.
        virtual bool commitIopAndEeMemory(std::span<const IopMemoryWrite>) { return false; }
        virtual bool readPadSnapshot(std::array<uint8_t,32> &) const { return false; }
        // Port0 DualShock 2 motors as the controller drives them from a poll:
        // small motor on/off, large motor 0..255.
        virtual void setPadActuators(bool, uint8_t) {}
        // GT4 DBCMAN's two controller ports (0 = PS2 port 1, 1 = port 2; false = nothing
        // plugged in). The defaults keep one-port hosts exactly as they were.
        virtual bool readPortPadSnapshot(unsigned port, std::array<uint8_t,32> &packet) const { return port == 0 && readPadSnapshot(packet); }
        virtual void setPortPadActuators(unsigned port, bool smallMotor, uint8_t large) { if (port == 0) setPadActuators(smallMotor, large); }
        virtual bool sha256(std::span<const uint8_t>,std::array<uint8_t,32>&) const { return false; }
        virtual ~IopHost() = default;

        virtual bool readGuest(uint32_t address, void *destination, size_t size) const = 0;
        virtual bool writeGuest(uint32_t address, const void *source, size_t size) = 0;
        virtual bool zeroGuest(uint32_t address, size_t size) = 0;
        virtual bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const = 0;
        virtual uint32_t allocateIopHandle(IopHandleKind kind) = 0;
        virtual uint32_t allocateGuest(uint32_t size, uint32_t alignment) = 0;
        virtual void freeGuest(uint32_t address) = 0;

        virtual void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) = 0;

        virtual std::string hostPath(HostPathKind kind) const = 0;
        virtual std::string translateGuestPath(std::string_view path) const = 0;
        virtual uint64_t openHostFile(std::string_view path) = 0;
        virtual bool hostFileSize(uint64_t handle, uint64_t &size) const = 0;
        virtual bool readHostFile(uint64_t handle,
                                  uint64_t offset,
                                  void *destination,
                                  size_t size,
                                  size_t &bytesRead) = 0;
        virtual void closeHostFile(uint64_t handle) = 0;

        virtual int32_t memoryCard(const MemoryCardRequest &request) = 0;

        virtual bool hasGuestFunction(uint32_t address) const = 0;
        virtual bool invokeGuestFunction(uint64_t callToken,
                                         uint32_t address,
                                         uint32_t a0,
                                         uint32_t a1,
                                         uint32_t a2,
                                         uint32_t a3,
                                         uint32_t *resultAddress) = 0;

        virtual void log(LogLevel level, std::string_view message) = 0;

        // Optional HLE capability. Preserve every active IOP allocation.
        virtual bool clearFreeIopMemory() { return false; }
        // All-or-none writes to allocated IOP memory; default host has no capability.
        virtual bool writeIopMemoryBatch(std::span<const IopMemoryWrite>) { return false; }
        // Optional output clock. The host calls `source(frames, stereo)` from its
        // own sound thread at 48 kHz (stereo holds 2*frames interleaved int16),
        // paced by the audio device or, without one, by wall-clock time. A false
        // return plays silence for that chunk. One source at a time; returns 0
        // when the host has no output clock (or one is already attached).
        using SoundSource = std::function<bool(uint32_t frames, std::span<int16_t> stereo)>;
        virtual uint64_t attachSoundSource(SoundSource) { return 0; }
        // Stops the clock. After return the source is never called again.
        virtual bool detachSoundSource(uint64_t) { return false; }
        // iWakeupThread for an EE thread, as the EE's SIF command handler does
        // when an IOP request reaches an RPC queue. EE executor only (onVBlank).
        virtual bool wakeupEeThread(int) { return false; }
        // iSignalSema for an EE semaphore, as an EE SIF command handler does when
        // an IOP result packet arrives (SDK 3.0 FILEIO completion).
        virtual bool signalEeSemaphore(int) { return false; }
    };
}
