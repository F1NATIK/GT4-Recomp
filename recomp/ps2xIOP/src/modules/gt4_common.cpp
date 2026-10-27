#include "gt4_iop.h"
#include "ps2x/iop/gt4_disc_reads.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace ps2x::iop::gt4
{
    namespace
    {
        std::atomic<DiscReadObserver> g_readObserver{nullptr}; // GT4_TEXTURE_DUMP's car / course tags
        void observeRead(uint64_t offset, uint64_t bytes)
        {
            if (const DiscReadObserver o = g_readObserver.load(std::memory_order_acquire))
                o(offset, bytes);
        }
    }

    void setDiscReadObserver(DiscReadObserver observer) { g_readObserver.store(observer, std::memory_order_release); }

    void Disc::close()
    {
        std::lock_guard lock(mutex_);
        if (file_)
        {
            host_.closeHostFile(file_);
        }
        file_ = 0;
        size_ = 0;
        failed_ = false;
        extra_.reset(); // GT4_EXTRA_CARS: checked again at the next open
        extraServed_.clear();
    }

    bool Disc::open()
    {
        if (file_)
        {
            return true;
        }
        if (failed_)
        {
            return false;
        }
        const std::string path = host_.hostPath(HostPathKind::CdImage);
        if (path.empty())
        {
            failed_ = true;
            host_.log(LogLevel::Error, "[GT4/DISC] no disc image configured (set GT4_DISC_IMAGE to the GT4 ISO)");
            return false;
        }
        file_ = host_.openHostFile(path);
        if (!file_ || !host_.hostFileSize(file_, size_))
        {
            if (file_)
            {
                host_.closeHostFile(file_);
            }
            file_ = 0;
            failed_ = true;
            host_.log(LogLevel::Error, "[GT4/DISC] cannot open disc image " + path);
            return false;
        }
        host_.log(LogLevel::Info, "[GT4/DISC] disc image " + path + " (" + std::to_string(size_ / 2048u) + " sectors)");
        loadExtraCars(); // GT4_EXTRA_CARS
        return true;
    }

    bool Disc::readSectors(uint32_t lsn, uint32_t count, void *destination)
    {
        std::lock_guard lock(mutex_);
        if (!open())
        {
            return false;
        }
        const uint64_t offset = uint64_t(lsn) * 2048u;
        const uint64_t bytes = uint64_t(count) * 2048u;
        const uint64_t end = servedSize(); // GT4_EXTRA_CARS: its files past the image's end too
        if (offset > end || bytes > end - offset)
        {
            return false;
        }
        const uint64_t inImage = offset < size_ ? std::min<uint64_t>(bytes, size_ - offset) : 0;
        size_t read = 0;
        if (inImage && (!host_.readHostFile(file_, offset, destination, size_t(inImage), read) || read != inImage))
            return false;
        if (bytes > inImage)
            std::memset(static_cast<uint8_t *>(destination) + inImage, 0, size_t(bytes - inImage));
        if (extra_)
            extraCars(offset, static_cast<uint8_t *>(destination), size_t(bytes)); // GT4_EXTRA_CARS
        overlayCheats(offset, destination, size_t(bytes), "PDICDVD sector read"); // GT4_CHEATS
        observeRead(offset, bytes);
        return true;
    }

    size_t Disc::readBytes(uint64_t offset, void *destination, size_t size)
    {
        std::lock_guard lock(mutex_);
        if (!open() || offset >= servedSize())
        {
            return 0;
        }
        size = static_cast<size_t>(std::min<uint64_t>(size, servedSize() - offset));
        size_t read = 0;
        if (offset < size_)
        {
            if (!host_.readHostFile(file_, offset, destination, static_cast<size_t>(std::min<uint64_t>(size, size_ - offset)), read))
                return 0;
        }
        if (extra_ && read < size && offset + read >= size_)
        {
            std::memset(static_cast<uint8_t *>(destination) + read, 0, size - read); // past the image: the pack's files
            read = size;
        }
        if (extra_)
            extraCars(offset, static_cast<uint8_t *>(destination), read); // GT4_EXTRA_CARS
        overlayCheats(offset, destination, read, "byte read"); // GT4_CHEATS
        observeRead(offset, read);
        return read;
    }

    uint64_t Disc::sectorCount()
    {
        std::lock_guard lock(mutex_);
        return open() ? size_ / 2048u : 0u;
    }

    RpcResult reply(IopHost &host, const RpcRequest &request, const void *data, size_t bytes)
    {
        if (bytes)
        {
            if (!request.receive.address || request.receive.size < bytes ||
                !host.writeGuest(request.receive.address, data, bytes))
            {
                return rejected();
            }
        }
        RpcResult result;
        result.handled = true;
        result.resultAddress = request.receive.address;
        result.serverDispatchPolicy = ServerDispatchPolicy::Suppress;
        return result;
    }

    RpcResult rejected()
    {
        RpcResult result;
        result.serverDispatchPolicy = ServerDispatchPolicy::RejectUnhandled;
        return result;
    }

    bool readSend(IopHost &host, const RpcRequest &request, void *data, size_t bytes)
    {
        return request.send.address && request.send.size >= bytes &&
               host.readGuest(request.send.address, data, bytes);
    }

    std::string hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%x", value);
        return text;
    }
}
