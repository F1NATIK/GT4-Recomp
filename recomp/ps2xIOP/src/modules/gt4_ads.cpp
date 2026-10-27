#include "gt4_ads.h"

#include <algorithm>
#include <cstring>

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr int32_t FilterPos[5] = {0, 60, 115, 98, 122};
        constexpr int32_t FilterNeg[5] = {0, 0, -52, -55, -60};

        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        // One 16-byte PS-ADPCM block -> 28 samples.
        void decodeBlock(const uint8_t *block, int32_t hist[2], std::deque<int16_t> &out)
        {
            const int shift = std::min(block[0] & 0xf, 12);
            const unsigned filter = std::min<unsigned>((block[0] >> 4) & 0xf, 4u);
            for (unsigned i = 0; i < 28; ++i)
            {
                const int32_t nibble = (block[2 + i / 2] >> ((i & 1) * 4)) & 0xf;
                int32_t sample = static_cast<int16_t>(nibble << 12) >> shift;
                sample += (hist[0] * FilterPos[filter] + hist[1] * FilterNeg[filter] + 32) >> 6;
                sample = std::clamp(sample, -32768, 32767);
                hist[1] = hist[0];
                hist[0] = sample;
                out.push_back(static_cast<int16_t>(sample));
            }
        }
    }

    void AdsStream::reset()
    {
        *this = AdsStream{};
    }

    void AdsStream::feed(std::span<const uint8_t> bytes)
    {
        size_t at = 0;
        if (!parsed_)
        {
            const size_t need = std::min(bytes.size(), 40u - header_.size());
            header_.insert(header_.end(), bytes.begin(), bytes.begin() + need);
            at = need;
            if (header_.size() < 40)
                return;
            parsed_ = true;
            if (std::memcmp(header_.data(), "SShd", 4) != 0)
            {
                ended_ = true; // not an .ads stream
                return;
            }
            type_ = le32(&header_[8]);
            rate_ = le32(&header_[12]);
            channels_ = le32(&header_[16]);
            interleave_ = le32(&header_[20]);
            dataSize_ = le32(&header_[36]);
            if (!rate_ || rate_ > 96000)
                rate_ = 48000;
            if (!interleave_ || interleave_ > 0x10000)
                interleave_ = 0x400;
        }
        // Drop consumed bytes now and then when streaming (movies).
        if (!loop_ && read_ > (1u << 20))
        {
            data_.erase(data_.begin(), data_.begin() + static_cast<ptrdiff_t>(read_));
            read_ = 0;
        }
        data_.insert(data_.end(), bytes.begin() + static_cast<ptrdiff_t>(at), bytes.end());
    }

    uint64_t AdsStream::totalSamples() const
    {
        if (type_ == 1)
            return dataSize_ / 4u;
        if (type_ == 2)
            return uint64_t(dataSize_) / 16u * 28u;
        return uint64_t(dataSize_) / 32u * 28u;
    }

    bool AdsStream::decodeChunk()
    {
        if (!parsed_ || ended_)
            return false;
        if (dataSize_ && consumed_ >= dataSize_)
        {
            if (loop_ && dataSize_ <= data_.size())
            {
                read_ = 0;
                consumed_ = 0;
                std::memset(hist_, 0, sizeof(hist_));
            }
            else
            {
                if (complete_ || !loop_)
                    ended_ = complete_ || consumed_ >= dataSize_;
                return false;
            }
        }
        const size_t available = data_.size() - read_;
        if (type_ == 0x10 || (type_ != 1 && type_ != 2 && channels_ == 2))
        {
            const size_t chunk = size_t(interleave_) * 2u;
            if (available < chunk)
            {
                if (complete_)
                    ended_ = true;
                return false;
            }
            const uint8_t *l = data_.data() + read_;
            const uint8_t *r = l + interleave_;
            for (size_t b = 0; b + 16 <= interleave_; b += 16)
            {
                decodeBlock(l + b, hist_[0], left_);
                decodeBlock(r + b, hist_[1], right_);
            }
            read_ += chunk;
            consumed_ += chunk;
            return true;
        }
        if (type_ == 2)
        {
            if (available < 16)
            {
                if (complete_)
                    ended_ = true;
                return false;
            }
            decodeBlock(data_.data() + read_, hist_[0], left_);
            right_.insert(right_.end(), left_.end() - 28, left_.end());
            read_ += 16;
            consumed_ += 16;
            return true;
        }
        // 16-bit stereo PCM, interleaved frames.
        if (available < 4)
        {
            if (complete_)
                ended_ = true;
            return false;
        }
        const size_t frames = std::min<size_t>(available / 4u, 1024u);
        for (size_t i = 0; i < frames; ++i)
        {
            const uint8_t *p = data_.data() + read_ + i * 4u;
            left_.push_back(static_cast<int16_t>(p[0] | p[1] << 8));
            right_.push_back(static_cast<int16_t>(p[2] | p[3] << 8));
        }
        read_ += frames * 4u;
        consumed_ += frames * 4u;
        return true;
    }

    bool AdsStream::next(int16_t &left, int16_t &right)
    {
        while (left_.empty() || right_.empty())
        {
            if (!decodeChunk())
                return false;
        }
        left = left_.front();
        right = right_.front();
        left_.pop_front();
        right_.pop_front();
        ++played_;
        return true;
    }

    int32_t StreamGain::left() const
    {
        const int64_t g = (int64_t(w[0]) * w[1] * w[3] * w[4]) >> 45;
        return static_cast<int32_t>(std::clamp<int64_t>(g, -32768, 32767));
    }

    int32_t StreamGain::right() const
    {
        const int64_t g = (int64_t(w[0]) * w[2] * w[3] * w[4]) >> 45;
        return static_cast<int32_t>(std::clamp<int64_t>(g, -32768, 32767));
    }

    bool StreamPlayer::mix(uint32_t frames, std::span<int32_t> out)
    {
        if (!active || paused)
            return false;
        const double step = double(stream.rate()) / 48000.0;
        bool finished = false;
        for (uint32_t f = 0; f < frames; ++f)
        {
            // 10 ms fade steps (PDISTR alarm 0xb4).
            if (gain.fading && ++fadeTick >= 480u)
            {
                fadeTick = 0;
                gain.w[3] += gain.fadeRate;
                if (gain.w[3] > 0x7fff)
                {
                    gain.w[3] = 0x8000;
                    gain.fading = false;
                }
                else if (gain.w[3] <= 0)
                {
                    gain.w[3] = 0;
                    gain.fading = false;
                    fadeStop = true; // PDISTR queues job 7: stop the BGM
                    active = false;
                    finished = true;
                    break;
                }
            }
            phase += step;
            while (phase >= 1.0)
            {
                phase -= 1.0;
                prev[0] = cur[0];
                prev[1] = cur[1];
                int16_t l = 0, r = 0;
                if (!stream.next(l, r))
                {
                    if (stream.ended())
                    {
                        finished = true;
                        active = false;
                    }
                    l = r = 0;
                }
                cur[0] = l;
                cur[1] = r;
            }
            if (finished)
                break;
            const double t = phase;
            int32_t l = static_cast<int32_t>(prev[0] + (cur[0] - prev[0]) * t);
            int32_t r = static_cast<int32_t>(prev[1] + (cur[1] - prev[1]) * t);
            if (gain.mono)
            {
                const int32_t m = (l + r) / 2;
                l = r = m;
            }
            out[f * 2u] += (l * gain.left()) >> 15;
            out[f * 2u + 1u] += (r * gain.right()) >> 15;
        }
        return finished;
    }
}
