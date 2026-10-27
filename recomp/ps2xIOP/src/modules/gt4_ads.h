#pragma once

// 'SShd'/'SSbd' (.ads) audio streams as GT4 uses them for BGM, narration and
// movie audio (analysis/phase4/specs/sound-stream.md section 5.3): a 40-byte
// header {SShd, 0x18, type, rate, channels, interleave, loopStart, loopEnd,
// SSbd, dataSize} then PS-ADPCM, stereo with 0x400-byte L/R interleave (type
// 0x10), mono ADPCM (type 2) or 16-bit PCM (type 1). This player decodes the
// stream to PCM, resamples it to 48 kHz and applies PDISTR's gain model.

#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace ps2x::iop::gt4
{
    class AdsStream
    {
    public:
        // Bytes of the stream in order (header first). May be fed incrementally.
        void feed(std::span<const uint8_t> bytes);
        // Room for a stream fed in pieces (no reallocation while it is played).
        void reserve(size_t bytes) { data_.reserve(bytes); }
        void reset();
        void setLoop(bool loop) { loop_ = loop; }
        bool headerParsed() const { return parsed_; }
        uint32_t rate() const { return rate_; }
        uint64_t totalSamples() const;
        bool ended() const { return ended_; }
        // Pull one source-rate stereo frame; false when no data is available.
        bool next(int16_t &left, int16_t &right);
        uint64_t played() const { return played_; }
        bool complete() const { return complete_; } // no more data will be fed

        void markComplete() { complete_ = true; }

    private:
        bool decodeChunk();
        std::vector<uint8_t> data_; // stream bytes after the header (whole stream or queue)
        size_t read_ = 0;
        std::vector<uint8_t> header_;
        bool parsed_ = false, loop_ = false, ended_ = false, complete_ = false;
        uint32_t type_ = 0x10, rate_ = 48000, channels_ = 2, interleave_ = 0x400, dataSize_ = 0;
        uint64_t consumed_ = 0, played_ = 0;
        int32_t hist_[2][2] = {};
        std::deque<int16_t> left_, right_;
    };

    // Gain model of PDISPU2 volume objects (exports 4/7/10..14): words w0..w4,
    // gain L = w0*w1*w3*w4 >> 45, R = w0*w2*w3*w4 >> 45, mono flag halves and
    // cross-feeds. Fade adds `rate` to w3 every 10 ms.
    struct StreamGain
    {
        int32_t w[5] = {0x8000, 0x8000, 0x8000, 0x8000, 0};
        bool mono = false;
        int32_t fadeRate = 0;
        bool fading = false;
        int32_t left() const;
        int32_t right() const;
    };

    // Resampling player over an AdsStream (48 kHz output).
    class StreamPlayer
    {
    public:
        std::mutex mutex;
        AdsStream stream;
        StreamGain gain;
        bool active = false, paused = false;
        double phase = 0.0;
        int16_t prev[2] = {}, cur[2] = {};
        // Adds `frames` 48 kHz frames into mix (interleaved, 32-bit accumulators).
        // Returns true when the stream reached its end (not looping).
        bool mix(uint32_t frames, std::span<int32_t> out);
        uint64_t fadeTick = 0; // output frames since the last 10 ms fade step
        bool fadeStop = false;
    };
}
