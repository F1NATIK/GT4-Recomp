#pragma once
// Host output clock for native IOP sound (2026-09-25, stage 4).
//
// One source at a time (IopHost::attachSoundSource). A dedicated sound thread
// calls the source in 32-frame chunks (48 kHz stereo int16) and keeps a small
// lock-free ring filled ahead of the consumer. The consumer decides the pace:
//   Device    - raylib AudioStream callback (miniaudio thread, under raylib's
//               mixer lock) pops frames; never calls the source or blocks.
//   WallClock - no audio device (headless, --cpu-only, device init failed):
//               frames are consumed at 48 kHz steady_clock time and dropped.
// The source therefore never runs on the audio device thread, and IRQ
// callbacks it triggers run on the sound thread.
#include "ps2x/iop/iop_host.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PS2SoundOutput
{
public:
    enum class Mode { Device, WallClock };
    struct Stats
    {
        Mode mode = Mode::WallClock;
        uint64_t framesRendered = 0;  // produced by the source (incl. silence on failure)
        uint64_t framesConsumed = 0;  // pulled by the device or the wall clock
        uint64_t underrunFrames = 0;  // device asked, ring empty (silence played)
        uint64_t silentRenders = 0;   // source returned false
        uint64_t maxRenderMicros = 0; // longest single source call
        uint64_t slowRenders = 0;     // source calls longer than 10 ms
    };
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChunkFrames = 32;

    // useDevice: try the raylib audio device (must already be initialized);
    // falls back to WallClock when it is not ready or the stream fails.
    PS2SoundOutput(ps2x::iop::IopHost::SoundSource source, bool useDevice);
    ~PS2SoundOutput(); // stops and joins; after return the source is never called
    PS2SoundOutput(const PS2SoundOutput &) = delete;
    PS2SoundOutput &operator=(const PS2SoundOutput &) = delete;

    Mode mode() const { return m_mode; }
    Stats stats() const;
    std::string describe() const;

    // Device callback body (public for the static trampoline only).
    void pull(int16_t *out, uint32_t frames);

private:
    void run();

    ps2x::iop::IopHost::SoundSource m_source;
    Mode m_mode = Mode::WallClock;
    // SPSC ring of stereo frames: producer = sound thread, consumer = device.
    static constexpr uint32_t kRingFrames = 8192;
    // Render-ahead lead. The IOP sees SPU2 IRQs this far ahead of real output,
    // and game timing is judged against it: the MPEG audio time must stay
    // inside the EE's picture range (steady-state margin ~50 ms). Too small a
    // lead turns host scheduling hiccups into audible gaps. 1024 frames = 21 ms.
    static constexpr uint32_t kTargetFrames = 1024;
    std::vector<int16_t> m_ring;
    std::atomic<uint64_t> m_write{0}, m_read{0};
    std::atomic<uint64_t> m_underrun{0}, m_silent{0}, m_maxRender{0}, m_slowRenders{0};
    std::atomic<bool> m_stop{false};
    std::mutex m_wakeMutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    struct DeviceStream;
    std::unique_ptr<DeviceStream> m_device;
};
