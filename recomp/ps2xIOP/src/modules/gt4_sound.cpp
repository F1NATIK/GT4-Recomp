#include "gt4_iop.h"
#include "gt4_ads.h"
#include "gt4_spu2.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#if !defined(GT4_STOCK_BUILD)
#include <chrono>
#include "ps2x/iop/gt4_car_state.h"
#include "gt4_sound_pack.h"
#endif

// GT4 PDISPU2 (SPUP 0x53505550 / SPUT 0x53505554) and PDISTR (STRP, PBGM, VOIC,
// MPG1, MPG2) as the EE uses them (analysis/phase4/specs/sound-stream.md).
// Sound effects run on the software SPU2 (gt4_spu2.*) through the per-frame
// voice command (SPU fn4); sample banks reach SPU2 RAM through STRP fn5.
// BGM / narration (.ads) and movie audio are decoded natively and mixed as
// core 1's streamed voices 22/23; VOIC PCM is core 1's memory input. Movie
// video is demultiplexed here and handed to the EE as PDISTR's 0x1400-byte
// records. Everything is answered synchronously; the runtime fires the EE's
// end callbacks for the nowait calls (SPU fn4, MPG2 fn3).

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t SidSpup = 0x53505550u, SidSput = 0x53505554u;
        constexpr uint32_t SidPbgm = 0x5042474du, SidStrp = 0x53545250u, SidVoic = 0x564f4943u;
        constexpr uint32_t SidMpg1 = 0x4d504731u, SidMpg2 = 0x4d504732u;
        constexpr uint32_t CoreBlock = 0x1d0u;
        constexpr uint32_t RecordBytes = 0x1400u;

        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

        // WAV writer for GT4_SOUND_WAV and the test-only trace: 48 kHz 16-bit PCM,
        // `channels` interleaved; the header sizes are refreshed about once a second.
        class WavFile
        {
        public:
            ~WavFile() { close(); }
            bool open(const std::string &path, uint16_t channels)
            {
                close();
                channels_ = channels ? channels : 1;
                file_ = path.empty() ? nullptr : std::fopen(path.c_str(), "wb");
                if (file_)
                    writeHeader();
                return file_ != nullptr;
            }
            bool isOpen() const { return file_ != nullptr; }
            void write(std::span<const int16_t> samples)
            {
                if (!file_)
                    return;
                std::fwrite(samples.data(), sizeof(int16_t), samples.size(), file_);
                bytes_ += uint32_t(samples.size() * sizeof(int16_t));
                if (bytes_ - lastHeader_ >= 48000u * 2u * channels_)
                {
                    lastHeader_ = bytes_;
                    writeHeader();
                }
            }
            void writeFloat(std::span<const float> samples)
            {
                buffer_.resize(samples.size());
                for (size_t i = 0; i < samples.size(); ++i)
                    buffer_[i] = static_cast<int16_t>(std::clamp(std::lround(samples[i]), -32768L, 32767L));
                write(buffer_);
            }
            void close()
            {
                if (!file_)
                    return;
                writeHeader();
                std::fclose(file_);
                file_ = nullptr;
                bytes_ = lastHeader_ = 0;
            }

        private:
            void writeHeader()
            {
                auto put16 = [](uint8_t *p, uint32_t v) {
                    p[0] = uint8_t(v);
                    p[1] = uint8_t(v >> 8);
                };
                auto put32 = [](uint8_t *p, uint32_t v) {
                    for (int i = 0; i < 4; ++i)
                        p[i] = uint8_t(v >> (8 * i));
                };
                uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
                put32(h + 4, 36u + bytes_);
                put16(h + 22, channels_);
                put32(h + 24, 48000u);
                put32(h + 28, 48000u * 2u * channels_);
                put16(h + 32, 2u * channels_);
                put32(h + 40, bytes_);
                const long at = std::ftell(file_);
                std::fseek(file_, 0, SEEK_SET);
                std::fwrite(h, 1, sizeof(h), file_);
                std::fseek(file_, at > 44 ? at : 44, SEEK_SET);
                std::fflush(file_);
            }
            std::FILE *file_ = nullptr;
            uint16_t channels_ = 2;
            uint32_t bytes_ = 0, lastHeader_ = 0;
            std::vector<int16_t> buffer_;
        };

        // GT4_SOUND_WAV=<path>: records the final 48 kHz stereo output to a WAV
        // file (debugging aid, in every build as before).
        class WavCapture
        {
        public:
            WavCapture()
            {
                if (const char *path = std::getenv("GT4_SOUND_WAV"); path && *path)
                    wav_.open(path, 2);
            }
            void write(std::span<const int16_t> stereo) { wav_.write(stereo); }

        private:
            WavFile wav_;
        };

#if !defined(GT4_STOCK_BUILD)
        // GT4_SPU_TRACE=<base> (test-only, not in the Classic build): what the software SPU2
        // does with the EE's sound commands, to measure voices in hidden runs (no audio device):
        //  <base>.cmd.csv    every SPU fn4 voice command as received (core flags, voice records,
        //                    key-on / key-off masks), the ENDX / ENVX status it answers, the
        //                    STRP opens and transfers into SPU2 RAM (what sample lands where) and
        //                    the SPU fn2 uploads from the EE into SPU2 RAM (P lines);
        //  <base>.voice.csv  every 10 ms of output: each sounding voice's registers and phase,
        //                    its own level (after VOLL/VOLR), its level as heard at the output
        //                    (both before the engine bass shelf) and whether it is an engine voice;
        //  <base>.mix.csv    every 10 ms: output level, all voices' heard level, per-core gains,
        //                    the engine voices as heard (after the shelf); against the mix at full
        //                    precision ("pre": no clamp in the cores, no limiter; the same measure
        //                    with the bass on or off): the output samples that differ from it
        //                    (GT4's clamps or the limiter changed them), its samples past 16 bits
        //                    and its largest one; the time spent in the SPU2's render in that
        //                    interval; the smallest scale of the engine bass boost and the frames
        //                    it was scaled down (room); the frames through the limiter; the
        //                    boost's mean scale;
        //  <base>.voices.wav all SPU2 voices as heard, without the BGM / movie / VOIC streams
        //                    (each voice before the engine bass shelf);
        //  <base>.engine.f32 the engine voices as heard, after the engine bass shelf (raw 32-bit float,
        //                    stereo interleaved, 48 kHz: no clamp, so the start's loud engines are whole);
        //  <base>.engine-dry.f32 the same before the shelf (the same as engine.f32 with the bass off);
        //  <base>.pre.f32    the mix at full precision, before GT4's clamps or the limiter (the same form);
        //  <base>.render-ns.u32  the time of every SPU2 render call (32 samples), in ns, 32-bit;
        //  <base>.pack.csv   (Custom Sounds) every 10 ms with a sound pack playing: the player car's slot, code, revs,
        //                    throttle, gear, the pack's level, GT4's muted engine and the pack as heard (RMS), pops;
        //  <base>.muted.f32  the player car's GT4 engine voices that a pack muted, as they would have been heard;
        //  <base>.pack.f32   the pack's output as added (both raw 32-bit float stereo, 48 kHz);
        //  <base>.stems.wav  with GT4_SPU_STEMS=<from>-<to> (output seconds): each of the 48
        //                    voices as heard, in stereo (96 channels: voice k left 2k, right 2k+1;
        //                    core 0 voices 0-23 then core 1), in that window.
        struct SpuTrace
        {
            bool on = false;
            std::FILE *cmd = nullptr, *voice = nullptr, *mix = nullptr, *cost = nullptr, *engineF32 = nullptr, *engineDryF32 = nullptr,
                       *preF32 = nullptr, *pack = nullptr, *mutedF32 = nullptr, *packF32 = nullptr;
            WavFile voicesWav, stemsWav;
            double stemsFrom = 0.0, stemsTo = -1.0;
            uint64_t commands = 0;
            uint32_t intervalFrames = 0;
            double outSq = 0.0;
            uint64_t renderNs = 0;
            uint32_t renderCalls = 0;
            Spu2::Probe probe;
            std::array<std::array<Spu2::VoiceState, Spu2::Voices>, 2> state{};

            void open()
            {
                const char *base = std::getenv("GT4_SPU_TRACE");
                if (!base || !*base)
                    return;
                const std::string b(base);
                cmd = std::fopen((b + ".cmd.csv").c_str(), "w");
                voice = std::fopen((b + ".voice.csv").c_str(), "w");
                mix = std::fopen((b + ".mix.csv").c_str(), "w");
                cost = std::fopen((b + ".render-ns.u32").c_str(), "wb");
                voicesWav.open(b + ".voices.wav", 2);
                engineF32 = std::fopen((b + ".engine.f32").c_str(), "wb");
                engineDryF32 = std::fopen((b + ".engine-dry.f32").c_str(), "wb");
                preF32 = std::fopen((b + ".pre.f32").c_str(), "wb");
                pack = std::fopen((b + ".pack.csv").c_str(), "w");
                mutedF32 = std::fopen((b + ".muted.f32").c_str(), "wb");
                packF32 = std::fopen((b + ".pack.f32").c_str(), "wb");
                if (pack)
                    std::fprintf(pack, "t_ms,seq,slot,code,rpm,throttle,gear,level,gt4_muted_rms,pack_rms,pop_triggers,pops_fired\n");
                if (const char *s = std::getenv("GT4_SPU_STEMS"); s && *s)
                {
                    double from = 0.0, to = 0.0;
                    if (std::sscanf(s, "%lf-%lf", &from, &to) == 2 && to > from)
                    {
                        stemsFrom = from;
                        stemsTo = to;
                        stemsWav.open(b + ".stems.wav", 4u * Spu2::Voices);
                    }
                }
                on = cmd && voice && mix;
                if (cmd)
                    std::fprintf(cmd, "# K,t_ms,seq,core,flags,mask,kon,koff | V,t_ms,seq,core,voice,vflags,pitch,volL,volR,ssa,adsr1,adsr2 | "
                                      "E,t_ms,seq,endx0,endx1,envx c0v0..c1v23 | O,t_ms,handle,lsn,count | W,t_ms,handle,spu,bytes,stream_byte | "
                                      "P,t_ms,spu,bytes\n");
                if (voice)
                    std::fprintf(voice, "t_ms,core,voice,phase,env,volL,volR,pitch,adsr1,adsr2,ssa,lsa,nax,active_frames,own_rms,heard_rms,heard_peak,engine\n");
                if (mix)
                    std::fprintf(mix, "t_ms,frames,out_rms,voices_rms,gain0L,gain0R,gain1L,gain1R,engine_rms,limited,over_full,pre_peak,render_us,render_calls,"
                                      "bass_scale_min,bass_backed_off,limiter_frames,bass_scale_mean\n");
            }
            void close()
            {
                for (std::FILE **f : {&cmd, &voice, &mix, &cost, &engineF32, &engineDryF32, &preF32, &pack, &mutedF32, &packF32})
                    if (*f)
                    {
                        std::fclose(*f);
                        *f = nullptr;
                    }
                voicesWav.close();
                stemsWav.close();
            }
        };
#endif

        struct Stream // STRP handle
        {
            uint64_t position = 0; // absolute image byte offset
#if !defined(GT4_STOCK_BUILD)
            bool bankNoted = false; // engine bass: this stream's bank counted for the log line (GT4 reads its 128-byte
                                    // header first, so the first SPU2 RAM transfer starts after it)
#endif
        };

        // PSS (MPEG-2 program stream, 4096-byte packs) reader over the disc image.
        // loop (MPG fn1 flag bit4, the title and course-preview movies): PDISTR passes the bit to PDICDVD's stream job
        // (byte +0x20), which reads the file's sectors again from the first one each time it reaches the last
        // (PDICDVD 0x804 -> 0x704), so the stream never ends. The demuxer (PDISTR 0x1f74, loop byte = its 5th argument)
        // takes a program end code 0x1b9 in loop mode as "skip the rest of this pass" (ring_read_mem of sectors * 2048
        // less the bytes parsed since the pass began, 0x2318) and parses on from the file's start (0x2328 -> 0x1fec);
        // without the loop bit, 0x1b9 ends the video (0x2330).
        class PssReader
        {
        public:
            void start(Disc *disc, uint64_t begin, uint64_t end, bool loop = false)
            {
                disc_ = disc;
                begin_ = begin;
                position_ = begin;
                end_ = end;
                loop_ = loop && end > begin;
                buffer_.clear();
                at_ = 0;
                consumed_ = 0;
                passes_ = 0;
                ended_ = false;
            }
            bool ended() const { return ended_; }
            uint32_t passes() const { return passes_; } // loop mode: times the reader went back to the file's start
            // Next PES packet: stream id and body (bytes after the length field).
            bool next(uint8_t &id, std::vector<uint8_t> &body)
            {
                while (!ended_)
                {
                    if (!fill(6))
                        break;
                    const uint8_t *p = buffer_.data() + at_;
                    if (p[0] != 0 || p[1] != 0 || p[2] != 1)
                    {
                        ++at_; // resynchronise
                        continue;
                    }
                    const uint8_t sid = p[3];
                    if (sid == 0xba)
                    {
                        if (!fill(14))
                            break;
                        const size_t stuffing = buffer_[at_ + 13] & 7u;
                        at_ += 14 + stuffing;
                        continue;
                    }
                    if (sid == 0xb9)
                    {
                        if (!loop_)
                        {
                            ended_ = true;
                            break;
                        }
                        at_ += 4;
                        const uint64_t pass = end_ - begin_, into = (consumed_ + at_) % pass;
                        if (into)
                            skip(pass - into);
                        continue;
                    }
                    const size_t length = (size_t(p[4]) << 8) | p[5];
                    if (!fill(6 + length))
                        break;
                    id = sid;
                    body.assign(buffer_.begin() + static_cast<ptrdiff_t>(at_ + 6),
                                buffer_.begin() + static_cast<ptrdiff_t>(at_ + 6 + length));
                    at_ += 6 + length;
                    return true;
                }
                ended_ = true;
                return false;
            }

        private:
            // Drop n stream bytes (reading on through the file's end in loop mode).
            void skip(uint64_t n)
            {
                while (n)
                {
                    if (at_ >= buffer_.size() && !fill(1))
                        return;
                    const size_t take = static_cast<size_t>(std::min<uint64_t>(n, buffer_.size() - at_));
                    at_ += take;
                    n -= take;
                }
            }

            bool fill(size_t need)
            {
                if (buffer_.size() - at_ >= need)
                    return true;
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(at_));
                consumed_ += at_;
                at_ = 0;
                while (buffer_.size() < need)
                {
                    if (position_ >= end_)
                    {
                        if (!loop_)
                            break;
                        position_ = begin_; // PDICDVD's looping stream job: the first sector again
                        ++passes_;
                    }
                    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(0x40000u, end_ - position_));
                    const size_t old = buffer_.size();
                    buffer_.resize(old + chunk);
                    const size_t got = disc_ ? disc_->readBytes(position_, buffer_.data() + old, chunk) : 0;
                    buffer_.resize(old + got);
                    position_ += chunk;
                    if (!got)
                        break;
                }
                return buffer_.size() >= need;
            }

            Disc *disc_ = nullptr;
            uint64_t begin_ = 0, position_ = 0, end_ = 0;
            uint64_t consumed_ = 0; // stream bytes before buffer_[0] since start()
            uint32_t passes_ = 0;
            std::vector<uint8_t> buffer_;
            size_t at_ = 0;
            bool loop_ = false, ended_ = true;
        };

        struct Voic
        {
            StreamGain gain;
            std::deque<int16_t> pcm; // interleaved stereo, 48 kHz
        };

        class Sound final : public Component
        {
        public:
            Sound(IopHost &host, Disc &disc) : host_(host), disc_(disc)
            {
#if !defined(GT4_STOCK_BUILD)
                // Engine bass (the Full edition's "Engine bass"; not in the Classic build): GT4_ENGINE_BASS=off, low,
                // medium or high; absent = off (the mix is then exactly the one without this code).
                if (const char *value = std::getenv("GT4_ENGINE_BASS"))
                {
                    bool known = true;
                    const Spu2::EngineBass level = parseEngineBass(value, known);
                    spu_.setEngineBass(level);
                    if (!known)
                        host_.log(LogLevel::Warning, std::string("[GT4/SOUND] GT4_ENGINE_BASS=") + value + ": not off, low, medium or high; engine bass off");
                    else if (level == Spu2::EngineBass::Off)
                        host_.log(LogLevel::Info, "[GT4/SOUND] engine bass: off");
                    else
                        host_.log(LogLevel::Info, "[GT4/SOUND] engine bass: " + std::string(engineBassName(level)) + " (low shelf at " +
                                                      std::to_string(int(Spu2::EngineBassCornerHz)) + " Hz, up to +" +
                                                      std::to_string(int(Spu2::engineBassGainDb(level))) +
                                                      " dB, on GT4's engine and exhaust voices; soft limiter on the output while they sound)");
                    engineBassOn_ = known && level != Spu2::EngineBass::Off;
                }
                trace_.open();
                if (trace_.on)
                {
                    spu_.setProbe(true, trace_.stemsWav.isOpen());
                    host_.log(LogLevel::Info, "[GT4/SOUND] SPU trace on (GT4_SPU_TRACE, test only)");
                }
                loadSoundPacks();
#endif
            }
            ~Sound() override
            {
                stopBgmReader();
                detach();
#if !defined(GT4_STOCK_BUILD)
                trace_.close();
#endif
            }

            void reset() override
            {
                detach();
                std::lock_guard lock(mutex_);
                stopBgmReader(); // (before bgm_.mutex: the reader takes it)
                spu_.reset();
                applyBootState();
                streams_.clear();
                nextHandle_ = 0x1000;
                {
                    std::lock_guard bgm(bgm_.mutex);
                    bgm_.active = false;
                    bgm_.stream.reset();
                }
                {
                    std::lock_guard movie(movie_.mutex);
                    movie_.active = false;
                    movie_.stream.reset();
                }
                {
                    std::lock_guard voic(voicMutex_);
                    voic_.clear();
                }
                movieActive_ = false;
                logged_.clear();
#if !defined(GT4_STOCK_BUILD)
                bankNote_ = {};
#endif
                outputMode_ = {1, 0, 0, 0};
                attach();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
#if !defined(GT4_STOCK_BUILD)
                noteEngineBanks();
#endif
                switch (request.sid)
                {
                case SidSpup: case SidSput: return spuCall(request);
                case SidStrp: return strpCall(request);
                case SidPbgm: return pbgmCall(request);
                case SidVoic: return voicCall(request);
                case SidMpg1: case SidMpg2: return mpgCall(request);
                default: return rejected();
                }
            }

        private:
            void note(const RpcRequest &request, const std::string &what, LogLevel level = LogLevel::Info)
            {
                // Per sid/function and message kind; warnings get a larger budget.
                const uint64_t key = ((uint64_t(request.sid) << 32) | request.function) ^
                                     (std::hash<std::string>{}(what.substr(0, 14)) << 8);
                if (logged_[key]++ < (level == LogLevel::Warning ? 64u : 4u))
                    host_.log(level, "[GT4/SOUND] " + what);
            }

            // PDISPU2 entry (startup trace): MVOL 0x3fff, VMIXL/R all voices, MMIX
            // 0xfc0 / 0xfcc, EVOL 0x7fff, AVOL core1 0x7fff, BVOL 0.
            void applyBootState()
            {
                for (unsigned c = 0; c < 2; ++c)
                {
                    spu_.setCoreMask(c, Spu2::CoreMask::DryLeft, 0xffffff);
                    spu_.setCoreMask(c, Spu2::CoreMask::DryRight, 0xffffff);
                    spu_.setMix(c, c ? 0xfcc : 0xfc0);
                    spu_.setCoreVolume(c, Spu2::CoreVolume::EffectLeft, 0x7fff);
                    spu_.setCoreVolume(c, Spu2::CoreVolume::EffectRight, 0x7fff);
                    spu_.setCoreVolume(c, Spu2::CoreVolume::ExtInputLeft, c ? 0x7fff : 0); // AVOL: core 0 into core 1
                    spu_.setCoreVolume(c, Spu2::CoreVolume::ExtInputRight, c ? 0x7fff : 0);
                    spu_.setCoreVolume(c, Spu2::CoreVolume::MasterLeft, 0x3fff);
                    spu_.setCoreVolume(c, Spu2::CoreVolume::MasterRight, 0x3fff);
                }
            }

            void attach()
            {
                if (token_)
                    return;
                token_ = host_.attachSoundSource([this](uint32_t frames, std::span<int16_t> stereo) {
                    return render(frames, stereo);
                });
                if (token_)
                    host_.log(LogLevel::Info, "[GT4/SOUND] software SPU2 attached to the 48 kHz output clock");
            }

            void detach()
            {
                if (token_)
                    host_.detachSoundSource(token_);
                token_ = 0;
            }

            bool render(uint32_t frames, std::span<int16_t> stereo)
            {
                streamMix_.assign(size_t(frames) * 2u, 0);
                bool bgmStopped = false;
                {
                    std::lock_guard bgm(bgm_.mutex);
                    bgm_.mix(frames, streamMix_);
                    bgmStopped = bgm_.fadeStop;
                }
                {
                    std::lock_guard movie(movie_.mutex);
                    movie_.mix(frames, streamMix_);
                }
                (void)bgmStopped;
                extra_.resize(size_t(frames) * 2u);
                for (size_t i = 0; i < extra_.size(); ++i)
                    extra_[i] = static_cast<int16_t>(std::clamp(streamMix_[i], -32768, 32767));
                input_.assign(size_t(frames) * 2u, 0);
                {
                    std::lock_guard voic(voicMutex_);
                    for (auto &[handle, v] : voic_)
                    {
                        const int32_t gl = v.gain.left(), gr = v.gain.right();
                        for (uint32_t f = 0; f < frames && v.pcm.size() >= 2; ++f)
                        {
                            const int32_t l = v.pcm.front();
                            v.pcm.pop_front();
                            const int32_t r = v.pcm.front();
                            v.pcm.pop_front();
                            input_[f * 2u] = static_cast<int16_t>(std::clamp(input_[f * 2u] + ((l * gl) >> 15), -32768, 32767));
                            input_[f * 2u + 1u] = static_cast<int16_t>(std::clamp(input_[f * 2u + 1u] + ((r * gr) >> 15), -32768, 32767));
                        }
                    }
                }
#if !defined(GT4_STOCK_BUILD)
                // Test-only: the time of the SPU2's render call (with the probe on, as the trace has it).
                const auto renderStart = trace_.on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
#if !defined(GT4_STOCK_BUILD)
                // Custom Sounds: the player car's pack (nothing at all without GT4_SOUND_PACK / GT4_SOUND_PACK_MAP).
                const bool packOn = packs_.any && mixPack(frames);
                packTraceOn_ = packOn;
                spu_.render(frames, stereo, extra_, {}, input_, packOn ? std::span<const int32_t>(packOut_) : std::span<const int32_t>());
#else
                spu_.render(frames, stereo, extra_, {}, input_);
#endif
#if !defined(GT4_STOCK_BUILD)
                if (trace_.on)
                {
                    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - renderStart).count());
                    trace_.renderNs += ns;
                    ++trace_.renderCalls;
                    if (trace_.cost)
                    {
                        const uint32_t n32 = uint32_t(std::min<uint64_t>(ns, 0xffffffffu));
                        std::fwrite(&n32, sizeof(n32), 1, trace_.cost);
                    }
                }
#endif
                wav_.write(stereo.first(size_t(frames) * 2u));
#if !defined(GT4_STOCK_BUILD)
                if (trace_.on)
                    traceRender(frames, stereo.first(size_t(frames) * 2u));
                framesRendered_.fetch_add(frames, std::memory_order_release);
                carstate::addSoundFrames(frames);
#endif
                return true;
            }

#if !defined(GT4_STOCK_BUILD)
            double traceMs() const { return double(framesRendered_.load(std::memory_order_acquire)) / 48.0; }

            // RPC thread (under mutex_): with the engine bass on, one log line per race load (half a second after the
            // last engine or exhaust bank came in): how many banks the boost will act on, so a player's log shows it.
            void noteEngineBanks()
            {
                auto &b = bankNote_;
                const uint32_t total = b.count[1] + b.count[2] + b.count[3];
                if (!total || std::chrono::steady_clock::now() - b.last < std::chrono::milliseconds(500))
                    return;
                if (engineBassOn_)
                    host_.log(LogLevel::Info, "[GT4/SOUND] engine bass: " + std::to_string(total) + " engine and exhaust banks loaded and marked (" +
                                                  std::to_string(b.count[1]) + " sound/engine, " + std::to_string(b.count[2]) + " sound/normal0-3, " +
                                                  std::to_string(b.count[3]) + " sound/turbo0-3)");
                b = {};
            }

            // ---- Custom Sounds (analysis/design/sound-packs/README.md) ----------------------------------------------------
            // GT4_SOUND_PACK=<pack folder>: the pack for every car; GT4_SOUND_PACK_MAP=<file>: a pack per car code (and
            // "all"), soundpack::parsePackMap. A relative folder is under GT4.exe's folder\sound-packs. This version plays
            // the pack for the player's car only (the car the pad drives); the other cars keep GT4's own engines.
            void loadSoundPacks()
            {
                const char *packKey = std::getenv("GT4_SOUND_PACK");
                const char *mapKey = std::getenv("GT4_SOUND_PACK_MAP");
                const std::string packValue = packKey ? packKey : "", mapValue = mapKey ? mapKey : "";
                if (packValue.empty() && mapValue.empty())
                    return;
                const soundpack::fs::path base = soundpack::exeFolder() / "sound-packs";
                std::map<std::string, std::shared_ptr<const soundpack::Pack>> loaded;
                auto load = [&](const std::string &value, const std::string &what) -> std::shared_ptr<const soundpack::Pack> {
                    if (value.empty() || value == "off")
                        return nullptr;
                    const soundpack::fs::path folder = soundpack::resolvePackFolder(value, base);
                    const std::string key = soundpack::pathUtf8(folder);
                    if (const auto it = loaded.find(key); it != loaded.end())
                        return it->second;
                    auto pack = std::make_shared<soundpack::Pack>();
                    std::string error;
                    if (!soundpack::loadPack(folder, *pack, error))
                    {
                        host_.log(LogLevel::Warning, "[GT4/SOUND] sound pack for " + what + ": " + error + " (GT4's own engine sound)");
                        loaded[key] = nullptr;
                        return nullptr;
                    }
                    host_.log(LogLevel::Info, "[GT4/SOUND] sound pack for " + what + ": '" + pack->name + "' from " + key + " (" +
                                                  std::to_string(pack->on.size()) + " on-throttle and " + std::to_string(pack->off.size()) +
                                                  " off-throttle loops, " + std::to_string(pack->pops.size()) + " pops; volume " +
                                                  std::to_string(pack->volume).substr(0, 4) + ", bass " + std::to_string(pack->bassDb).substr(0, 4) +
                                                  " dB, pop chance " + std::to_string(pack->popChance).substr(0, 4) + ")");
                    loaded[key] = pack;
                    return pack;
                };
                packs_.all = load(packValue, "every car (GT4_SOUND_PACK)");
                if (!mapValue.empty())
                {
                    std::ifstream in(soundpack::fs::path(std::u8string(mapValue.begin(), mapValue.end())), std::ios::binary);
                    if (!in)
                        host_.log(LogLevel::Warning, "[GT4/SOUND] sound pack map " + mapValue + ": cannot be read");
                    else
                    {
                        std::stringstream text;
                        text << in.rdbuf();
                        soundpack::PackMap map;
                        std::vector<std::string> warnings;
                        soundpack::parsePackMap(text.str(), map, warnings);
                        for (const std::string &w : warnings)
                            host_.log(LogLevel::Warning, "[GT4/SOUND] sound pack map " + mapValue + ": " + w + " (skipped)");
                        if (!map.all.empty() && !packs_.all)
                            packs_.all = load(map.all, "every car (the map's 'all' line)");
                        for (const auto &[code, folder] : map.car)
                            if (auto p = load(folder, "car code " + std::to_string(code)))
                                packs_.byCar[code] = p;
                    }
                }
                packs_.any = packs_.all || !packs_.byCar.empty();
                if (const char *s = std::getenv("GT4_SOUND_PACK_SEED"); s && *s)
                    packs_.seed = static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
                if (packs_.any)
                    host_.log(LogLevel::Info, "[GT4/SOUND] sound pack: on (" + std::string(packs_.all ? "every car" : "no pack for every car") + ", " +
                                                  std::to_string(packs_.byCar.size()) +
                                                  " car codes; the player's car plays its pack, GT4's own engine and exhaust voices of that car muted)");
            }

            // Sound thread, before the SPU2's render: the player car's pack into packOut_ (stereo, added after the cores).
            // The car state comes from the EE's race frames (gt4_car_state.h). The pack plays while that car's race frames
            // keep coming (stopped 0.5 s after the last) and follows GT4's own engine of that car in level: the muted
            // voices' heard level gates it (paused, finished or a menu fades the pack as GT4 fades its engine).
            bool mixPack(uint32_t frames)
            {
                auto &p = packs_;
                const carstate::Snapshot s = carstate::latest();
                const uint64_t now = carstate::soundFrames();
                if (s.seq != p.seq)
                {
                    p.seq = s.seq;
                    p.lastFrameAt = now;
                    const int player = s.player;
                    std::shared_ptr<const soundpack::Pack> want;
                    uint32_t code = 0;
                    if (player >= 0 && player < int(Spu2::EngineSlots) && s.car[player].valid)
                    {
                        code = s.car[player].code;
                        const auto it = p.byCar.find(code);
                        want = it != p.byCar.end() ? it->second : p.all;
                    }
                    if (!want)
                        p.mixer.reset();
                    else if (!p.mixer || &p.mixer->pack() != want.get() || p.slot != player || p.code != code)
                    {
                        p.mixer = std::make_unique<soundpack::Mixer>(want, p.seed);
                        p.mixer->reset(s.car[player].rpm, s.car[player].throttle);
                        host_.log(LogLevel::Info, "[GT4/SOUND] sound pack: the player's car (car " + std::to_string(player) + ", code " +
                                                      std::to_string(code) + ") plays '" + want->name + "'; its GT4 engine voices (SPU2 slot " +
                                                      std::to_string(player) + ") muted");
                    }
                    p.slot = want ? player : -1;
                    p.code = code;
                    if (p.mixer)
                        p.mixer->control(s.car[player].rpm, s.car[player].throttle, s.car[player].gear, s.car[player].pedal);
                }
                const bool fresh = p.seq != 0 && now - p.lastFrameAt < 24000u;
                const bool on = p.mixer && p.slot >= 0 && fresh;
                const uint32_t mask = on ? (1u << unsigned(p.slot)) : 0u;
                if (mask != p.mask)
                {
                    spu_.setMutedEngineSlots(mask);
                    p.mask = mask;
                    p.level = 0.0;
                }
                if (!on)
                    return false;
                // GT4's own engine of this car, as it would be heard (the last render call): the pack follows it in and out.
                const double heard = spu_.mutedHeardRms();
                const double target = std::min(1.0, heard / kPackPresenceRef);
                p.level += (target - p.level) * (target > p.level ? 0.25 : 0.04); // per 32-sample call: ~3 ms up, ~17 ms down
                p.mono.resize(frames);
                p.mixer->render(p.mono.data(), frames);
                packOut_.resize(size_t(frames) * 2u);
                double sq = 0.0;
                for (uint32_t f = 0; f < frames; ++f)
                {
                    const int32_t v = static_cast<int32_t>(std::lround(p.mono[f] * 32767.0 * kPackOutputGain * p.level));
                    packOut_[f * 2u] = packOut_[f * 2u + 1u] = v;
                    sq += double(v) * v;
                }
                if (trace_.on && trace_.pack)
                {
                    p.traceSq += sq;
                    p.traceHeard += heard * heard * frames;
                    p.traceFrames += frames;
                    if (p.traceFrames >= 480u)
                    {
                        const auto &c = s.car[std::max(0, p.slot)];
                        std::fprintf(trace_.pack, "%.1f,%llu,%d,%u,%.1f,%.3f,%d,%.3f,%.1f,%.1f,%llu,%llu\n", double(now) / 48.0,
                                     static_cast<unsigned long long>(s.seq), p.slot, c.code, c.rpm, c.throttle, c.gear, p.level,
                                     std::sqrt(p.traceHeard / p.traceFrames), std::sqrt(p.traceSq / p.traceFrames),
                                     static_cast<unsigned long long>(p.mixer->popTriggers()), static_cast<unsigned long long>(p.mixer->popsFired()));
                        p.traceSq = p.traceHeard = 0.0;
                        p.traceFrames = 0;
                    }
                }
                return true;
            }

            // Sound thread: the voices-only output, the stems window and the 10 ms voice lines.
            void traceRender(uint32_t frames, std::span<const int16_t> stereo)
            {
                auto &t = trace_;
                t.voicesWav.writeFloat(spu_.probeVoicesOnly());
                const auto raw = [](std::FILE *f, const std::vector<float> &v) {
                    if (f)
                        std::fwrite(v.data(), sizeof(float), v.size(), f);
                };
                raw(t.engineF32, spu_.probeEngine());
                raw(t.engineDryF32, spu_.probeEngineDry());
                raw(t.preF32, spu_.probePre());
                // Custom Sounds: the player's muted GT4 engine (what it would have played) and the pack as added (stereo float).
                raw(t.mutedF32, spu_.probeMuted());
                packTrace_.assign(size_t(frames) * 2u, 0.0f);
                if (packTraceOn_)
                    for (size_t i = 0; i < packTrace_.size() && i < packOut_.size(); ++i)
                        packTrace_[i] = float(packOut_[i]);
                raw(t.packF32, packTrace_);
                const double startS = double(framesRendered_.load(std::memory_order_acquire)) / 48000.0;
                if (t.stemsWav.isOpen() && startS >= t.stemsFrom && startS < t.stemsTo)
                    t.stemsWav.writeFloat(spu_.probeStems());
                for (const int16_t s : stereo)
                    t.outSq += double(s) * s;
                t.intervalFrames += frames;
                if (t.intervalFrames < 480u)
                    return;
                spu_.takeProbe(t.probe, t.state);
                const double ms = (double(framesRendered_.load(std::memory_order_acquire)) + frames) / 48.0;
                const double n = std::max<double>(1.0, t.probe.frames);
                std::fprintf(t.mix, "%.1f,%u,%.1f,%.1f,%.4f,%.4f,%.4f,%.4f,%.1f,%u,%u,%d,%.1f,%u,%.3f,%u,%u,%.3f\n", ms, t.intervalFrames,
                             std::sqrt(t.outSq / (2.0 * t.intervalFrames)), std::sqrt(t.probe.voicesSq / n), t.probe.path[0][0],
                             t.probe.path[0][1], t.probe.path[1][0], t.probe.path[1][1], std::sqrt(t.probe.engineSq / n), t.probe.limited,
                             t.probe.overFull, t.probe.prePeak, double(t.renderNs) / 1000.0, t.renderCalls, t.probe.bassScaleMin, t.probe.bassBackedOff,
                             t.probe.limiterFrames, engineBassOn_ && t.probe.frames ? t.probe.bassScaleSum / t.probe.frames : 1.0);
                for (unsigned c = 0; c < 2; ++c)
                    for (unsigned i = 0; i < Spu2::Voices; ++i)
                    {
                        const auto &p = t.probe.voice[c][i];
                        const auto &s = t.state[c][i];
                        if (!p.activeFrames && p.ownSq <= 0.0)
                            continue;
                        std::fprintf(t.voice, "%.1f,%u,%u,%u,%d,%04x,%04x,%04x,%04x,%04x,%x,%x,%x,%u,%.1f,%.1f,%.0f,%u\n", ms, c, i, s.phase, s.env,
                                     s.volL, s.volR, s.pitch, s.adsr1, s.adsr2, s.ssa, s.lsa, s.nax, p.activeFrames, std::sqrt(p.ownSq / n),
                                     std::sqrt(p.heardSq / n), p.heardPeak, s.engine ? 1u : 0u);
                    }
                t.intervalFrames = 0;
                t.outSq = 0.0;
                t.renderNs = 0;
                t.renderCalls = 0;
            }

            // RPC thread (under mutex_): one fn4 command as received, then the status it answers.
            void traceCommand(const uint8_t *cmd, const std::array<uint8_t, 0x80> &status)
            {
                auto &t = trace_;
                const double ms = traceMs();
                const uint64_t seq = t.commands++;
                for (unsigned c = 0; c < 2; ++c)
                {
                    const uint8_t *b = cmd + c * CoreBlock;
                    const uint32_t flags = le32(b), mask = le32(b + 4);
                    if (!flags && !mask)
                        continue;
                    std::fprintf(t.cmd, "K,%.1f,%llu,%u,%x,%x,%x,%x\n", ms, static_cast<unsigned long long>(seq), c, flags, mask,
                                 (flags & 0x800u) ? le32(b + 0x1b4) : 0u, (flags & 0x1000u) ? le32(b + 0x1b8) : 0u);
                    for (unsigned v = 0; v < 24; ++v)
                    {
                        if (!((mask >> v) & 1u))
                            continue;
                        const uint8_t *rec = b + 8 + v * 0x10;
                        std::fprintf(t.cmd, "V,%.1f,%llu,%u,%u,%x,%x,%x,%x,%x,%x,%x\n", ms, static_cast<unsigned long long>(seq), c, v,
                                     le16(rec), le16(rec + 2), le16(rec + 4), le16(rec + 6), le32(rec + 8), le16(rec + 0xc), le16(rec + 0xe));
                    }
                }
                std::fprintf(t.cmd, "E,%.1f,%llu,%x,%x", ms, static_cast<unsigned long long>(seq), le32(status.data()),
                             le32(status.data() + 0x34));
                for (unsigned c = 0; c < 2; ++c)
                    for (unsigned v = 0; v < 24; ++v)
                        std::fprintf(t.cmd, ",%x", le16(status.data() + c * 0x34u + 4u + v * 2u));
                std::fputc('\n', t.cmd);
                traceEeVoices(ms, seq);
            }

            // The EE's voice manager (sound state 0x6518c8, analysis/phase4/specs/sound-stream.md 3.2): 48
            // voice records of 0xe0 bytes at +0xa8 (+0x10 owner, +0x14 frames since ENVX first read non-zero,
            // +0x1a flags (bit 0 in use), +0x30 tracking mode, +0x34 priority) and the free list at +0x2aa8
            // (linked through +0xc). F line every command: free voices, voices in use. X line when a voice's
            // owner, flags, mode or priority changes, or its age restarts.
            void traceEeVoices(double ms, uint64_t seq)
            {
                constexpr uint32_t State = 0x6518c8u, Records = State + 0xa8u, Stride = 0xe0u;
                std::vector<uint8_t> v(48u * Stride);
                host_.readGuest(Records, v.data(), v.size());
                uint32_t head = 0, free = 0, inUse = 0;
                host_.readGuest(State + 0x2aa8u, &head, 4);
                for (uint32_t p = head; p && free < 64u; ++free)
                {
                    if (p < Records || p >= Records + 48u * Stride)
                        break;
                    p = le32(v.data() + (p - Records) + 0xcu);
                }
                for (unsigned i = 0; i < 48u; ++i)
                {
                    const uint8_t *r = v.data() + i * Stride;
                    const std::array<uint32_t, 5> now{le32(r + 0x10), le32(r + 0x14), le16(r + 0x1a), le32(r + 0x30), le32(r + 0x34)};
                    inUse += now[2] & 1u;
                    const auto &was = eeVoiceLast_[i];
                    if (now[0] != was[0] || now[2] != was[2] || now[3] != was[3] || now[4] != was[4] || now[1] < was[1])
                        std::fprintf(trace_.cmd, "X,%.1f,%llu,%u,%x,%u,%x,%u,%d\n", ms, static_cast<unsigned long long>(seq), i, now[0],
                                     now[1], now[2], now[3], static_cast<int32_t>(now[4]));
                    eeVoiceLast_[i] = now;
                }
                std::fprintf(trace_.cmd, "F,%.1f,%llu,%u,%u\n", ms, static_cast<unsigned long long>(seq), free, inUse);
            }
#endif

            // ---- PDISPU2 -------------------------------------------------------
            RpcResult spuCall(const RpcRequest &request)
            {
                switch (request.function)
                {
                case 0:
                {
                    const std::array<uint32_t, 3> v{0x112u, SidSpup, SidSput};
                    return reply(host_, request, v.data(), std::min<size_t>(12u, request.receive.size));
                }
                case 1: case 2:
                {
                    // {spu, ee, size}: fn1 SPU2 RAM -> EE, fn2 EE -> SPU2 RAM.
                    std::array<uint32_t, 3> r{};
                    if (!readSend(host_, request, r.data(), sizeof(r)) || r[2] > Spu2::RamBytes)
                        return rejected();
                    std::vector<uint8_t> data(r[2]);
                    if (request.function == 2)
                    {
                        host_.readGuest(r[1], data.data(), data.size());
                        spu_.writeRam(r[0], data);
#if !defined(GT4_STOCK_BUILD)
                        spu_.markEngineSamples(r[0], r[2], false); // engine bass: not from an engine bank
                        if (trace_.on)
                            std::fprintf(trace_.cmd, "P,%.1f,%x,%u\n", traceMs(), r[0], r[2]);
#endif
                    }
                    else
                    {
                        spu_.readRam(r[0], data);
                        host_.writeGuest(r[1], data.data(), data.size());
                    }
                    return reply(host_, request, nullptr, 0);
                }
                case 3:
                {
                    const auto status = statusBlock(pdiSpu2StatusAhead(3, 0, 0));
                    return reply(host_, request, status.data(), std::min<size_t>(status.size(), request.receive.size));
                }
                case 4:
                {
                    std::vector<uint8_t> cmd(0x3b8);
                    if (!readSend(host_, request, cmd.data(), std::min<size_t>(cmd.size(), request.send.size)))
                        return rejected();
                    applyVoiceCommand(cmd.data());
                    // PDISPU2 0x1188 reads the status only after its DelayThread(100) following a
                    // key-on or key-off (pdiSpu2StatusAhead, gt4_spu2.h), so on the PS2 a voice
                    // keyed on in this command already shows its attack in ENVX. The EE (0x55d890)
                    // takes a tracked voice as started once ENVX != 0 and frees it when ENVX is
                    // back to 0: a status read at the key-on instant said 0, and GT4 freed and
                    // restarted the voice every second frame.
#if !defined(GT4_STOCK_BUILD)
                    // Test only (not in the Classic build): GT4_SPU_TEST_STATUS_NOW=1 answers without
                    // the IOP's delay, as before this fix, for A/B runs.
                    static const bool statusNow = [] {
                        const char *v = std::getenv("GT4_SPU_TEST_STATUS_NOW");
                        return v && *v == '1';
                    }();
#else
                    constexpr bool statusNow = false;
#endif
                    const auto status =
                        statusBlock(statusNow ? 0u : pdiSpu2StatusAhead(4, le32(cmd.data()), le32(cmd.data() + CoreBlock)));
#if !defined(GT4_STOCK_BUILD)
                    if (trace_.on)
                        traceCommand(cmd.data(), status);
#endif
                    // GT4_SPU_LOG=1: key-on events and a periodic voice summary.
                    static const bool spuLog = [] {
                        const char *v = std::getenv("GT4_SPU_LOG");
                        return v && *v == '1';
                    }();
                    if (spuLog)
                    {
                        for (unsigned c = 0; c < 2; ++c)
                        {
                            const uint8_t *b = cmd.data() + c * CoreBlock;
                            const uint32_t flags = le32(b);
                            if (flags & 0x800u)
                            {
                                const uint32_t kon = le32(b + 0x1b4);
                                for (unsigned v = 0; v < 24; ++v)
                                    if ((kon >> v) & 1u)
                                    {
                                        const uint8_t *rec = b + 8 + v * 0x10;
                                        host_.log(LogLevel::Info, "[GT4/SPU] key-on c" + std::to_string(c) + " v" + std::to_string(v) +
                                                                      " ssa " + hex(le32(rec + 8)) + " pitch " + hex(le16(rec + 2)) +
                                                                      " vol " + hex(le16(rec + 4)) + "/" + hex(le16(rec + 6)) +
                                                                      " adsr " + hex(le16(rec + 0xc)) + "/" + hex(le16(rec + 0xe)));
                                    }
                            }
                        }
                        if ((++spuFrames_ % 300u) == 0u)
                            host_.log(LogLevel::Info, "[GT4/SPU] endx " + hex(spu_.coreMask(0, Spu2::CoreMask::EndFlags)) + "/" +
                                                          hex(spu_.coreMask(1, Spu2::CoreMask::EndFlags)));
                    }
                    return reply(host_, request, status.data(), std::min<size_t>(status.size(), request.receive.size));
                }
                case 5: case 6:
                {
                    std::array<uint32_t, 3> r{};
                    readSend(host_, request, r.data(), request.function == 5 ? 12 : 4);
                    if (request.function == 5)
                        std::copy_n(r.begin(), 3, outputMode_.begin());
                    else
                        outputMode_[3] = r[0];
                    note(request, "output mode " + std::to_string(outputMode_[0]) + "/" + std::to_string(outputMode_[1]) + "/" +
                                      std::to_string(outputMode_[2]) + "/" + std::to_string(outputMode_[3]) + " (PCM out)");
                    return reply(host_, request, nullptr, 0);
                }
                default:
                    note(request, "unsupported SPU fn " + hex(request.function), LogLevel::Warning);
                    return rejected();
                }
            }

            // 2 x {u32 ENDX, u16 ENVX[24]} (0x34 each), then zeros up to 0x80; ENDX and ENVX
            // as they read `aheadSamples` SPU2 samples from now (both at the same instant).
            std::array<uint8_t, 0x80> statusBlock(unsigned aheadSamples)
            {
                const Spu2::Status s = spu_.statusAfter(aheadSamples);
                std::array<uint8_t, 0x80> status{};
                for (unsigned c = 0; c < 2; ++c)
                {
                    std::memcpy(status.data() + c * 0x34u, &s.endx[c], 4);
                    for (unsigned v = 0; v < 24; ++v)
                        std::memcpy(status.data() + c * 0x34u + 4u + v * 2u, &s.envx[c][v], 2);
                }
                return status;
            }

            void applyVoiceCommand(const uint8_t *cmd)
            {
                const uint8_t *block[2] = {cmd, cmd + CoreBlock};
                uint32_t flags[2] = {le32(block[0]), le32(block[1])};
                // 1. key-off
                if ((flags[0] | flags[1]) & 0x1000u)
                    for (unsigned c = 0; c < 2; ++c)
                        if (flags[c] & 0x1000u)
                            spu_.setCoreMask(c, Spu2::CoreMask::KeyOff, le32(block[c] + 0x1b8));
                // 2-3. effect unit: enable bit only (no reverb model yet)
                for (unsigned c = 0; c < 2; ++c)
                    if (block[c][0x1c0] && block[c][0x1c1])
                        spu_.setAttr(c, static_cast<uint16_t>(spu_.attr(c) | 0x80u));
                // 4. per-voice parameters
                for (unsigned c = 0; c < 2; ++c)
                {
                    const uint32_t mask = le32(block[c] + 4);
                    for (unsigned v = 0; v < 24; ++v)
                    {
                        if (!((mask >> v) & 1u))
                            continue;
                        const uint8_t *rec = block[c] + 8 + v * 0x10;
                        const uint16_t f = le16(rec);
                        if (f & 0x40u)
                        {
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeLeft, 0);
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeRight, 0);
                            if (f & 0x08u)
                                spu_.setVoiceAddr(c, v, Spu2::VoiceAddr::Start, le32(rec + 8));
                        }
                        else
                        {
                            if ((f & 1u) && (f & 6u))
                                spu_.setVoiceParam(c, v, Spu2::VoiceParam::Pitch, 0);
                            if (f & 2u)
                                spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeLeft, le16(rec + 4) & 0x7fffu);
                            if (f & 4u)
                                spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeRight, le16(rec + 6) & 0x7fffu);
                        }
                        if (f & 1u)
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::Pitch, le16(rec + 2));
                        if (f & 0x10u)
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::Adsr1, le16(rec + 0xc));
                        if (f & 0x20u)
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::Adsr2, le16(rec + 0xe));
                    }
                }
                // 5. core switches and volumes (MVOL, bit 0x1, is not applied)
                for (unsigned c = 0; c < 2; ++c)
                {
                    const uint32_t f = flags[c];
                    const uint8_t *b = block[c];
                    if (!f)
                        continue;
                    if (f & 0x2000u)
                        spu_.setAttr(c, static_cast<uint16_t>((spu_.attr(c) & ~0x3f00u) | ((le16(b + 0x1bc) & 0x3fu) << 8)));
                    if (f & 0x10u) spu_.setCoreMask(c, Spu2::CoreMask::PitchMod, le32(b + 0x198));
                    if (f & 0x20u) spu_.setCoreMask(c, Spu2::CoreMask::Noise, le32(b + 0x19c));
                    if (f & 0x40u) spu_.setCoreMask(c, Spu2::CoreMask::DryLeft, le32(b + 0x1a0));
                    if (f & 0x80u) spu_.setCoreMask(c, Spu2::CoreMask::WetLeft, le32(b + 0x1a4));
                    if (f & 0x100u) spu_.setCoreMask(c, Spu2::CoreMask::DryRight, le32(b + 0x1a8));
                    if (f & 0x200u) spu_.setCoreMask(c, Spu2::CoreMask::WetRight, le32(b + 0x1ac));
                    if (f & 0x400u) spu_.setMix(c, le16(b + 0x1b0));
                    if (f & 0x4u) // AVOL (libsd 0xd80/0xe80 | core)
                    {
                        spu_.setCoreVolume(c, Spu2::CoreVolume::ExtInputLeft, le16(b + 0x190));
                        spu_.setCoreVolume(c, Spu2::CoreVolume::ExtInputRight, le16(b + 0x192));
                    }
                    if (f & 0x8u) // BVOL (libsd 0xf80/0x1080 | core)
                    {
                        spu_.setCoreVolume(c, Spu2::CoreVolume::DataInputLeft, le16(b + 0x194));
                        spu_.setCoreVolume(c, Spu2::CoreVolume::DataInputRight, le16(b + 0x196));
                    }
                    if (f & 0x2u)
                    {
                        spu_.setCoreVolume(c, Spu2::CoreVolume::EffectLeft, le16(b + 0x18c));
                        spu_.setCoreVolume(c, Spu2::CoreVolume::EffectRight, le16(b + 0x18e));
                    }
                }
                // 6. key-on
                for (unsigned c = 0; c < 2; ++c)
                    if (flags[c] & 0x800u)
                        spu_.setCoreMask(c, Spu2::CoreMask::KeyOn, le32(block[c] + 0x1b4));
                // 7. volumes of keyed-on voices (regardless of the mask)
                for (unsigned c = 0; c < 2; ++c)
                    for (unsigned v = 0; v < 24; ++v)
                    {
                        const uint8_t *rec = block[c] + 8 + v * 0x10;
                        if (le16(rec) & 0x40u)
                        {
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeLeft, le16(rec + 4));
                            spu_.setVoiceParam(c, v, Spu2::VoiceParam::VolumeRight, le16(rec + 6));
                        }
                    }
            }

            // ---- STRP ----------------------------------------------------------
            RpcResult strpCall(const RpcRequest &request)
            {
                std::array<uint32_t, 0x80 / 4> r{};
                readSend(host_, request, r.data(), std::min<size_t>(sizeof(r), request.send.size));
                const auto name = [&](size_t offset) {
                    const char *text = reinterpret_cast<const char *>(r.data()) + offset;
                    return std::string(text, strnlen(text, sizeof(r) - offset));
                };
                switch (request.function)
                {
                case 0:
                {
                    const uint32_t v = 0x107u;
                    return reply(host_, request, &v, std::min<size_t>(4u, request.receive.size));
                }
                case 1:
                {
                    // getstat(name): host files are not available; err = 1.
                    note(request, "STRP file info '" + name(0) + "': no host file system");
                    std::array<uint32_t, 16> info{1u};
                    return reply(host_, request, info.data(), std::min<size_t>(sizeof(info), request.receive.size));
                }
                case 3: case 6:
                {
                    // open {a = lsn, b = count, cap, ..., name @0x14}
                    const std::string file = name(0x14);
                    if (!file.empty() || request.function == 6)
                    {
                        note(request, "STRP open of host file '" + file + "' not supported", LogLevel::Warning);
                        const uint32_t none = 0;
                        return reply(host_, request, &none, std::min<size_t>(4u, request.receive.size));
                    }
                    const uint32_t handle = nextHandle_++;
                    streams_[handle].position = uint64_t(r[0]) * 2048u;
                    note(request, "STRP open disc lsn " + std::to_string(r[0]) + " count " + std::to_string(r[1]));
#if !defined(GT4_STOCK_BUILD)
                    if (trace_.on)
                        std::fprintf(trace_.cmd, "O,%.1f,%x,%u,%u\n", traceMs(), handle, r[0], r[1]);
#endif
                    return reply(host_, request, &handle, std::min<size_t>(4u, request.receive.size));
                }
                case 4: case 5:
                {
                    // {h, ee/spu, n}: next n bytes of the stream to EE memory / SPU2 RAM.
                    const auto it = streams_.find(r[0]);
                    if (it == streams_.end() || r[2] > 0x1000000u)
                        return rejected();
                    std::vector<uint8_t> data(r[2]);
                    const size_t got = disc_.readBytes(it->second.position, data.data(), data.size());
                    it->second.position += r[2];
                    if (got < data.size())
                        std::fill(data.begin() + static_cast<ptrdiff_t>(got), data.end(), 0);
                    if (request.function == 4)
                        host_.writeGuest(r[1], data.data(), (data.size() + 15u) & ~size_t(15));
                    else
                    {
                        spu_.writeRam(r[1], data);
#if !defined(GT4_STOCK_BUILD)
                        // Engine bass: these samples are an engine voice's when they come from GT4's engine and exhaust
                        // banks; anything else written here unmarks the blocks it covers.
                        const uint64_t from = it->second.position - r[2];
                        const EngineBank bank = engineBankOf(from, r[2]);
                        spu_.markEngineSamples(r[1], r[2], bank != EngineBank::None);
                        if (bank != EngineBank::None && !it->second.bankNoted)
                        {
                            it->second.bankNoted = true;
                            ++bankNote_.count[size_t(bank)];
                            bankNote_.last = std::chrono::steady_clock::now();
                        }
                        if (trace_.on)
                            std::fprintf(trace_.cmd, "W,%.1f,%x,%x,%u,%llu\n", traceMs(), r[0], r[1], r[2],
                                         static_cast<unsigned long long>(it->second.position - r[2]));
#endif
                    }
                    return reply(host_, request, nullptr, 0);
                }
                case 2: // close
                    streams_.erase(r[0]);
                    return reply(host_, request, nullptr, 0);
                case 8: case 9:
                    return reply(host_, request, nullptr, 0);
                default:
                    note(request, "unsupported STRP fn " + hex(request.function), LogLevel::Warning);
                    return rejected();
                }
            }

            // ---- PBGM ----------------------------------------------------------
            // PBGM fn1 reads the start of the stream in the RPC and the rest on a
            // reader thread (GT4_BGM_STREAM=0: all of it in the RPC, as before).
            // The whole 8.5-10.8 MB read and its copies took the EE 6-8 ms per
            // open, twice per race start, and held bgm_.mutex (the mixer) all
            // that time (analysis/phase5/specs/step4-hitches.md P4). PDISTR
            // streams in the background on a PS2. The reader is far ahead of
            // playback (~47 KB of the stream per second), so the mixer sees the
            // same bytes; the stream is complete when the reader has fed the last
            // piece (or a short read at the end of the image).
            // (Pieces of 256 KB: the disc image's lock is held for one piece at a
            // time, so the EE's own disc reads wait at most that long.)
            static constexpr size_t kBgmFirstBytes = 256u << 10u, kBgmPieceBytes = 256u << 10u;
            std::thread bgmReader_;
            std::atomic<bool> bgmReaderStop_{false};

            // Before bgm_.stream is replaced (fn1, reset) or the component goes;
            // never with bgm_.mutex held (the reader takes it to feed).
            void stopBgmReader()
            {
                if (bgmReader_.joinable())
                {
                    bgmReaderStop_.store(true, std::memory_order_release);
                    bgmReader_.join();
                }
                bgmReaderStop_.store(false, std::memory_order_release);
            }

            void startBgmReader(uint64_t offset, size_t remaining)
            {
                bgmReader_ = std::thread([this, offset, remaining]() mutable {
                    std::vector<uint8_t> piece;
                    while (remaining != 0u && !bgmReaderStop_.load(std::memory_order_acquire))
                    {
                        const size_t want = std::min(remaining, kBgmPieceBytes);
                        piece.resize(want);
                        const size_t got = disc_.readBytes(offset, piece.data(), want);
                        piece.resize(got);
                        std::lock_guard bgm(bgm_.mutex);
                        if (bgmReaderStop_.load(std::memory_order_acquire))
                            return;
                        bgm_.stream.feed(piece);
                        offset += got;
                        remaining -= got;
                        if (got < want || remaining == 0u)
                        {
                            bgm_.stream.markComplete();
                            return;
                        }
                    }
                });
            }

            RpcResult pbgmCall(const RpcRequest &request)
            {
                std::array<uint32_t, 0x80 / 4> r{};
                readSend(host_, request, r.data(), std::min<size_t>(sizeof(r), request.send.size));
                if (request.function == 1 && reinterpret_cast<const char *>(r.data())[0x10] == '\0')
                    stopBgmReader(); // the previous stream's reader, before that stream is replaced
                std::lock_guard bgm(bgm_.mutex);
                switch (request.function)
                {
                case 1:
                {
                    const char *text = reinterpret_cast<const char *>(r.data()) + 0x10;
                    if (*text)
                    {
                        note(request, std::string("PBGM host file '") + text + "' not supported", LogLevel::Warning);
                        break;
                    }
                    static const bool streamBgm = [] {
                        const char *v = std::getenv("GT4_BGM_STREAM");
                        return !(v && *v == '0');
                    }();
                    // {lsn, bytes, loop, ring size, name}.
                    const uint64_t offset = uint64_t(r[0]) * 2048u;
                    const size_t total = r[1];
                    const size_t first = streamBgm ? std::min(total, kBgmFirstBytes) : total;
                    std::vector<uint8_t> data(first);
                    const size_t got = disc_.readBytes(offset, data.data(), data.size());
                    data.resize(got);
                    const bool more = streamBgm && got == first && first < total;
                    bgm_.stream.reset();
                    bgm_.stream.setLoop(r[2] & 0xffu);
                    if (more)
                        bgm_.stream.reserve(total);
                    bgm_.stream.feed(data);
                    if (!more)
                        bgm_.stream.markComplete();
                    bgm_.gain.w[4] = 0x8000;
                    bgm_.gain.fading = false;
                    bgm_.fadeStop = false;
                    bgm_.active = true;
                    bgm_.paused = false;
                    bgm_.phase = 0.0;
                    host_.log(LogLevel::Info, "[GT4/SOUND] BGM lsn " + std::to_string(r[0]) + " (" + std::to_string(more ? total : got) +
                                                  " bytes, " + std::to_string(bgm_.stream.rate()) + " Hz" +
                                                  ((r[2] & 0xffu) ? ", loop" : "") + (more ? ", streamed" : "") + ")");
                    if (more)
                        startBgmReader(offset + got, total - got);
                    break;
                }
                case 2:
                    bgm_.active = false;
                    bgm_.gain.w[4] = 0;
                    break;
                case 3:
                    bgm_.gain.fadeRate = static_cast<int32_t>(r[0]);
                    bgm_.gain.fading = r[0] != 0;
                    break;
                case 4:
                    bgm_.gain.w[0] = static_cast<int32_t>(r[0]);
                    break;
                case 5:
                    bgm_.gain.fading = false;
                    bgm_.gain.w[3] = static_cast<int32_t>(r[0]);
                    break;
                case 6:
                    bgm_.paused = true;
                    break;
                case 7:
                    bgm_.paused = false;
                    break;
                case 8:
                {
                    // {u64 elapsed us, s64 samples played, u32 total samples, u32 finished}
                    std::array<uint8_t, 0x40> status{};
                    if (bgm_.stream.headerParsed())
                    {
                        const uint64_t played = bgm_.stream.played();
                        const uint64_t micros = played * 1000000u / std::max<uint32_t>(1u, bgm_.stream.rate());
                        const uint64_t total = bgm_.stream.totalSamples();
                        const uint32_t finished = (!bgm_.active && !bgm_.paused) || played >= total ? 1u : 0u;
                        const uint32_t totalLow = static_cast<uint32_t>(total);
                        std::memcpy(status.data(), &micros, 8);
                        std::memcpy(status.data() + 8, &played, 8);
                        std::memcpy(status.data() + 0x10, &totalLow, 4);
                        std::memcpy(status.data() + 0x14, &finished, 4);
                        static const bool bgmLog = [] {
                            const char *v = std::getenv("GT4_SPU_LOG");
                            return v && *v == '1';
                        }();
                        static uint32_t polls = 0;
                        if (bgmLog && (polls++ % 300u) == 0u)
                            host_.log(LogLevel::Info, "[GT4/SOUND] BGM status: " + std::to_string(micros / 1000u) + " ms, " +
                                                          std::to_string(played) + "/" + std::to_string(total) + " samples, active " +
                                                          std::to_string(bgm_.active) + " finished " + std::to_string(finished));
                    }
                    return reply(host_, request, status.data(), std::min<size_t>(status.size(), request.receive.size));
                }
                case 9:
                    bgm_.gain.mono = r[0] != 0;
                    break;
                default:
                    note(request, "unsupported PBGM fn " + hex(request.function), LogLevel::Warning);
                    return rejected();
                }
                return reply(host_, request, nullptr, 0);
            }

            // ---- VOIC ----------------------------------------------------------
            RpcResult voicCall(const RpcRequest &request)
            {
                std::array<uint32_t, 0x40 / 4> r{};
                readSend(host_, request, r.data(), std::min<size_t>(sizeof(r), request.send.size));
                std::lock_guard voic(voicMutex_);
                switch (request.function)
                {
                case 1:
                {
                    const uint32_t handle = nextHandle_++;
                    voic_[handle].gain.w[4] = 0x8000;
                    note(request, "VOIC stream created (half " + hex(r[1]) + ")");
                    return reply(host_, request, &handle, std::min<size_t>(4u, request.receive.size));
                }
                case 2:
                    voic_.erase(r[0]);
                    break;
                case 3:
                {
                    // 48 kHz stereo PCM in alternating 0x200-byte L / R blocks.
                    const auto it = voic_.find(r[0]);
                    if (it == voic_.end() || r[2] > 0x100000u)
                        return rejected();
                    std::vector<int16_t> data(r[2] / 2u);
                    host_.readGuest(r[1], data.data(), data.size() * 2u);
                    auto &pcm = it->second.pcm;
                    for (size_t block = 0; block + 512 <= data.size(); block += 512)
                        for (size_t i = 0; i < 256; ++i)
                        {
                            pcm.push_back(data[block + i]);
                            pcm.push_back(data[block + 256 + i]);
                        }
                    while (pcm.size() > 48000u * 2u) // keep at most one second queued
                        pcm.pop_front();
                    break;
                }
                case 4: case 5: case 6:
                    if (const auto it = voic_.find(r[0]); it != voic_.end())
                    {
                        if (request.function == 5)
                            it->second.gain.w[0] = static_cast<int32_t>(r[1]);
                        else if (request.function == 6)
                            it->second.gain.w[3] = static_cast<int32_t>(r[1]);
                    }
                    break;
                default:
                    note(request, "unsupported VOIC fn " + hex(request.function), LogLevel::Warning);
                    return rejected();
                }
                return reply(host_, request, nullptr, 0);
            }

            // ---- MPG1 / MPG2 ---------------------------------------------------
            RpcResult mpgCall(const RpcRequest &request)
            {
                std::array<uint32_t, 0x80 / 4> r{};
                readSend(host_, request, r.data(), std::min<size_t>(sizeof(r), request.send.size));
                switch (request.function)
                {
                case 1:
                {
                    // {flags, lsn, sectors, name}: low nibble audio track, 0x40 audio sink A, 0x10 loop (PssReader).
                    const uint64_t begin = uint64_t(r[1]) * 2048u;
                    reader_.start(&disc_, begin, begin + uint64_t(r[2]) * 2048u, (r[0] & 0x10u) != 0);
                    moviePasses_ = 0;
                    audioTrack_ = r[0] & 0xfu;
                    audioOn_ = (r[0] & 0x40u) != 0;
                    movieActive_ = true;
                    {
                        std::lock_guard movie(movie_.mutex);
                        movie_.stream.reset();
                        movie_.gain = {};
                        movie_.gain.w[4] = 0x8000;
                        movie_.active = audioOn_;
                        movie_.paused = false;
                        movie_.phase = 0.0;
                    }
                    host_.log(LogLevel::Info, "[GT4/SOUND] movie lsn " + std::to_string(r[1]) + " sectors " + std::to_string(r[2]) +
                                                  " flags " + hex(r[0]));
                    return reply(host_, request, nullptr, 0);
                }
                case 2:
                    return movieRead(request, r[0], r[1]);
                case 3:
                    return reply(host_, request, nullptr, 0); // A/V clock sync: audio runs on the output clock
                case 4:
                {
                    host_.log(LogLevel::Info, "[GT4/SOUND] movie stop after " + std::to_string(movieReads_) + " reads");
                    movieActive_ = false;
                    std::lock_guard movie(movie_.mutex);
                    movie_.active = false;
                    return reply(host_, request, nullptr, 0);
                }
                default:
                    note(request, "unsupported MPG fn " + hex(request.function), LogLevel::Warning);
                    return rejected();
                }
            }

            RpcResult movieRead(const RpcRequest &request, uint32_t destination, uint32_t bytes)
            {
                // One video PES per 0x1400-byte record: {fragLen, misalign, PES length, 0}
                // + data (16-byte padded) + zero header. Unfilled records stay zero.
                const uint32_t slots = bytes / RecordBytes;
                const auto traceStart = std::chrono::steady_clock::now(); // GT4_MOVIE_TRACE (analysis/design/movie-stutter)
                std::vector<uint8_t> out(size_t(slots) * RecordBytes, 0);
                uint32_t filled = 0;
                std::vector<uint8_t> body;
                uint8_t id = 0;
                while (movieActive_ && filled < slots && reader_.next(id, body))
                {
                    if (id == 0xbd && body.size() > 3)
                    {
                        const size_t payload = 3u + body[2];
                        if (audioOn_ && body.size() >= payload + 4)
                        {
                            const uint32_t sub = le32(body.data() + payload) ;
                            // sub-stream id 0xffa1000N, stored big-endian on disc
                            const uint32_t be = (sub >> 24) | ((sub >> 8) & 0xff00u) | ((sub << 8) & 0xff0000u) | (sub << 24);
                            // PDISTR 0x2208: the selection is a track number until the
                            // first match latches the full id; track 0 takes the first
                            // audio stream in the file.
                            bool route = false;
                            if ((be & 0xfffe0000u) == 0xffa00000u)
                            {
                                if ((audioTrack_ >> 16) == 0)
                                {
                                    route = audioTrack_ == 0 || (be & 0xffffu) == audioTrack_;
                                    if (route)
                                        audioTrack_ = be;
                                }
                                else
                                    route = be == audioTrack_;
                            }
                            if (route)
                            {
                                std::lock_guard movie(movie_.mutex);
                                movie_.stream.feed(std::span(body).subspan(payload + 4));
                            }
                        }
                        continue;
                    }
                    if ((id & 0xf0u) != 0xe0u)
                        continue;
                    const uint32_t length = static_cast<uint32_t>(body.size());
                    if (length == 0 || length >= 0x13f0u)
                        continue;
                    uint8_t *record = out.data() + size_t(filled) * RecordBytes;
                    const std::array<uint32_t, 4> header{length, 0u, length, 0u};
                    std::memcpy(record, header.data(), sizeof(header));
                    std::memcpy(record + 16, body.data(), length);
                    ++filled;
                }
                if (reader_.ended())
                {
                    std::lock_guard movie(movie_.mutex);
                    movie_.stream.markComplete();
                }
                if (!out.empty())
                    host_.writeGuest(destination, out.data(), out.size());
                note(request, "movie read " + std::to_string(filled) + "/" + std::to_string(slots) + " video records");
                {
                    static const bool movieTrace = std::getenv("GT4_MOVIE_TRACE") != nullptr;
                    if (movieTrace)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[mvt] rd t=%lld us=%.0f rec=%u\n",
                                     static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()),
                                     std::chrono::duration<double, std::micro>(now - traceStart).count(), filled);
                    }
                }
                if (reader_.passes() != moviePasses_)
                {
                    moviePasses_ = reader_.passes();
                    note(request, "movie looped: pass " + std::to_string(moviePasses_ + 1));
                }
                if (std::getenv("GT4_SPU_LOG") && (++movieReads_ % 32u) == 0)
                {
                    std::lock_guard movie(movie_.mutex);
                    host_.log(LogLevel::Info, "[GT4/SOUND] movie read #" + std::to_string(movieReads_) + " records " +
                                                  std::to_string(filled) + " audio id " + hex(audioTrack_) + " played " +
                                                  std::to_string(movie_.stream.played()) + " active " +
                                                  std::to_string(movie_.active) + " ended " + std::to_string(reader_.ended()));
                }
                return reply(host_, request, nullptr, 0);
            }

            IopHost &host_;
            Disc &disc_;
            std::mutex mutex_;
            Spu2 spu_;
            uint64_t token_ = 0;
            std::map<uint32_t, Stream> streams_;
            uint32_t nextHandle_ = 0x1000;
            StreamPlayer bgm_, movie_;
            std::mutex voicMutex_;
            std::map<uint32_t, Voic> voic_;
            PssReader reader_;
            uint32_t audioTrack_ = 0;
            bool audioOn_ = false, movieActive_ = false;
            uint32_t movieReads_ = 0, moviePasses_ = 0;
            std::array<uint32_t, 4> outputMode_{1, 0, 0, 0};
            std::map<uint64_t, uint32_t> logged_;
            std::vector<int32_t> streamMix_;
            std::vector<int16_t> extra_, input_;
            uint64_t spuFrames_ = 0;
            WavCapture wav_;
#if !defined(GT4_STOCK_BUILD)
            std::atomic<uint64_t> framesRendered_{0};
            SpuTrace trace_;
            std::array<std::array<uint32_t, 5>, 48> eeVoiceLast_{};
            bool engineBassOn_ = false; // GT4_ENGINE_BASS low, medium or high
            struct BankNote
            {
                std::array<uint32_t, 4> count{}; // per EngineBank: banks loaded since the last line
                std::chrono::steady_clock::time_point last{};
            } bankNote_;
            // Custom Sounds (sound thread after the constructor): the packs, the player car's mixer and its state.
            // kPackOutputGain: the pack's tanh-limited output (about -18 dBFS RMS for a levelled pack) at 0.6 of full
            // scale comes out near GT4's own player engine in the measured New York race (median heard RMS 2,570 of
            // 32,767; results\car-fields-sp-dump1.txt). kPackPresenceRef: GT4's muted engine heard at this RMS or more
            // keeps the pack at its full level (the race's 10th percentile is 1,206).
            static constexpr double kPackOutputGain = 0.6, kPackPresenceRef = 400.0;
            struct Packs
            {
                bool any = false;
                std::shared_ptr<const soundpack::Pack> all;
                std::map<uint32_t, std::shared_ptr<const soundpack::Pack>> byCar;
                uint32_t seed = 1;
                std::unique_ptr<soundpack::Mixer> mixer;
                uint64_t seq = 0, lastFrameAt = 0;
                int slot = -1;
                uint32_t code = 0, mask = 0;
                double level = 0.0;
                std::vector<float> mono;
                double traceSq = 0.0, traceHeard = 0.0;
                uint32_t traceFrames = 0;
            } packs_;
            std::vector<int32_t> packOut_;
            std::vector<float> packTrace_; // test-only trace: the pack's output of the last render call
            bool packTraceOn_ = false;
#endif
        };
    }

    std::unique_ptr<Component> createSound(IopHost &host, Disc &disc)
    {
        return std::make_unique<Sound>(host, disc);
    }
}
