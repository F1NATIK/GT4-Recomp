#include "ps2_sound_output.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if !defined(PLATFORM_VITA)
#include "ps2_host_backend.h"
#endif

namespace
{
std::atomic<PS2SoundOutput *> g_deviceOwner{nullptr};
}

struct PS2SoundOutput::DeviceStream
{
#if !defined(PLATFORM_VITA)
    AudioStream stream{};
#endif
    bool open = false;
};

#if !defined(PLATFORM_VITA)
static void soundOutputDeviceCallback(void *buffer, unsigned int frames)
{
    auto *owner = g_deviceOwner.load(std::memory_order_acquire);
    auto *out = static_cast<int16_t *>(buffer);
    if (!owner)
    {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
        return;
    }
    owner->pull(out, frames);
}
#endif

PS2SoundOutput::PS2SoundOutput(ps2x::iop::IopHost::SoundSource source, bool useDevice)
    : m_source(std::move(source)), m_ring(size_t(kRingFrames) * 2, 0)
{
#if !defined(PLATFORM_VITA)
    PS2SoundOutput *expected = nullptr;
    if (useDevice && IsAudioDeviceReady() && g_deviceOwner.compare_exchange_strong(expected, this))
    {
        m_device = std::make_unique<DeviceStream>();
        m_device->stream = LoadAudioStream(kSampleRate, 16, 2);
        if (IsAudioStreamValid(m_device->stream))
        {
            SetAudioStreamCallback(m_device->stream, soundOutputDeviceCallback);
            m_device->open = true;
            m_mode = Mode::Device;
        }
        else
        {
            g_deviceOwner.store(nullptr, std::memory_order_release);
            m_device.reset();
        }
    }
#else
    (void)useDevice;
#endif
    m_thread = std::thread([this] { run(); });
#if !defined(PLATFORM_VITA)
    // Start the device only after the thread exists; the ring starts empty,
    // so the first callbacks may play a few frames of silence (counted).
    if (m_device && m_device->open)
        PlayAudioStream(m_device->stream);
#endif
}

PS2SoundOutput::~PS2SoundOutput()
{
#if !defined(PLATFORM_VITA)
    if (m_device && m_device->open)
    {
        StopAudioStream(m_device->stream);
        // UnloadAudioStream takes raylib's mixer lock, so no callback is
        // running into this object after it returns.
        UnloadAudioStream(m_device->stream);
        m_device->open = false;
        PS2SoundOutput *self = this;
        g_deviceOwner.compare_exchange_strong(self, nullptr);
    }
#endif
    m_stop.store(true, std::memory_order_release);
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void PS2SoundOutput::pull(int16_t *out, uint32_t frames)
{
    const uint64_t read = m_read.load(std::memory_order_relaxed);
    const uint64_t available = m_write.load(std::memory_order_acquire) - read;
    const uint32_t n = uint32_t(std::min<uint64_t>(available, frames));
    for (uint32_t i = 0; i < n; ++i)
    {
        const size_t slot = size_t((read + i) % kRingFrames) * 2;
        out[i * 2] = m_ring[slot];
        out[i * 2 + 1] = m_ring[slot + 1];
    }
    if (n < frames)
    {
        std::memset(out + size_t(n) * 2, 0, size_t(frames - n) * 2 * sizeof(int16_t));
        m_underrun.fetch_add(frames - n, std::memory_order_relaxed);
    }
    m_read.store(read + n, std::memory_order_release);
    m_wake.notify_one();
}

void PS2SoundOutput::run()
{
    using clock = std::chrono::steady_clock;
    std::vector<int16_t> chunk(size_t(kChunkFrames) * 2);
    std::FILE *capture = nullptr;
    if (const char *path = std::getenv("GT3_SOUND_CAPTURE"); path && *path)
        capture = std::fopen(path, "wb");
    struct CloseCapture { std::FILE *&f; ~CloseCapture() { if (f) std::fclose(f); } } closeCapture{capture};
    auto origin = clock::now();
    uint64_t originFrames = 0; // wall clock: frames consumed at `origin`
    while (!m_stop.load(std::memory_order_acquire))
    {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        uint64_t read = m_read.load(std::memory_order_acquire);
        if (m_mode == Mode::WallClock && write < kTargetFrames)
        {
            // Prime the first target before the wall clock starts, so thread
            // start-up and the first renders are not reported as underruns.
            origin = clock::now();
        }
        else if (m_mode == Mode::WallClock)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - origin).count();
            uint64_t due = originFrames + uint64_t(elapsed) * kSampleRate / 1000000u;
            if (due > write)
            {
                // Source slower than real time: consume only what exists and
                // restart the clock from here (counted as underrun), rather
                // than accumulating an unbounded catch-up backlog.
                m_underrun.fetch_add(due - write, std::memory_order_relaxed);
                origin = clock::now();
                originFrames = due = write;
            }
            if (due > read)
            {
                m_read.store(due, std::memory_order_release);
                read = due;
            }
        }
        const uint64_t buffered = write - read;
        if (buffered >= kTargetFrames || kRingFrames - buffered < kChunkFrames)
        {
            std::unique_lock lock(m_wakeMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(2));
            continue;
        }
        bool ok = false;
        const auto renderBegan = clock::now();
        try
        {
            ok = m_source && m_source(kChunkFrames, chunk);
        }
        catch (...)
        {
            ok = false;
        }
        const auto renderMicros = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - renderBegan).count());
        if (renderMicros > m_maxRender.load(std::memory_order_relaxed))
            m_maxRender.store(renderMicros, std::memory_order_relaxed);
        if (renderMicros > 10000)
            m_slowRenders.fetch_add(1, std::memory_order_relaxed);
        if (!ok)
        {
            std::fill(chunk.begin(), chunk.end(), int16_t(0));
            m_silent.fetch_add(1, std::memory_order_relaxed);
        }
        for (uint32_t i = 0; i < kChunkFrames; ++i)
        {
            const size_t slot = size_t((write + i) % kRingFrames) * 2;
            m_ring[slot] = chunk[i * 2];
            m_ring[slot + 1] = chunk[i * 2 + 1];
        }
        if (capture)
        {
            // Diagnostic (GT3_SOUND_CAPTURE=file): the exact rendered stream,
            // raw s16le stereo 48 kHz, flushed about once a second.
            std::fwrite(chunk.data(), sizeof(int16_t), chunk.size(), capture);
            if (((write / kChunkFrames) % 1500u) == 0u)
                std::fflush(capture);
        }
        m_write.store(write + kChunkFrames, std::memory_order_release);
    }
}

PS2SoundOutput::Stats PS2SoundOutput::stats() const
{
    Stats s;
    s.mode = m_mode;
    s.framesRendered = m_write.load(std::memory_order_acquire);
    s.framesConsumed = m_read.load(std::memory_order_acquire);
    s.underrunFrames = m_underrun.load(std::memory_order_relaxed);
    s.silentRenders = m_silent.load(std::memory_order_relaxed);
    s.maxRenderMicros = m_maxRender.load(std::memory_order_relaxed);
    s.slowRenders = m_slowRenders.load(std::memory_order_relaxed);
    return s;
}

std::string PS2SoundOutput::describe() const
{
    const auto s = stats();
    std::ostringstream o;
    o << (s.mode == Mode::Device ? "audio device" : "wall clock (no audio device; output discarded)")
      << " 48000 Hz stereo s16, chunk " << kChunkFrames << ", target " << kTargetFrames
      << " frames; rendered " << s.framesRendered << ", consumed " << s.framesConsumed
      << ", underrun frames " << s.underrunFrames << ", silent chunks " << s.silentRenders
      << ", longest render " << s.maxRenderMicros << " us, renders >10 ms " << s.slowRenders;
    return o.str();
}
