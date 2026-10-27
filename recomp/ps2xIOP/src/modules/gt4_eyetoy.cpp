#include "gt4_eyetoy.h"

#include "gt4_eyetoy_ipum.h"
#include "gt4_eyetoy_source.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

// EYETOY.IRX 2.3 as GT4 drives it (addresses: EYETOY = module-relative in
// game/IRX/EYETOY.IRX, listing analysis/iop-sys/disasm/gt4/EYETOY.txt; CORE =
// game/CORE.elf). Everything the EE can see is reproduced:
// - LOADFILE: module arguments parsed as func_310c does (defaults at 0x47d8),
//   camera presence in EE SIF register 7 (func_27b0: SetSreg(7, cam0 | cam1<<16)),
//   cleared at module start/stop, set on connect.
// - RPC 0x80001400 (handler 0x1570): in-place 16-byte buffer {cmd, arg1, arg2,
//   camera}, result in word 0. cmd 8 info word and cmd 2/9 work without a
//   camera; everything else answers 0xffff6fc4 when the camera is absent.
// - Frames: 'ipum' frames into the 3-slot EE ring given by cmd 2, slot k at
//   ring + k*framesize, the running counter in the slot's last word (0x1d80-0x1e94).

namespace ps2x::iop::gt4
{
    namespace
    {
        using namespace eyetoy;

        constexpr uint32_t EeSregTable = 0x008869c0u;  // sceSifGetReg (CORE 0x5b0880) reads 0x8869c0 + 4n
        constexpr uint32_t SregCamera = 7;             // read by libeyetoy status (CORE 0x58c740)
        constexpr uint32_t NotConnected = 0xffff6fc4u; // EYETOY 0x16e0: camera object +0x44 == 0
        constexpr uint32_t ControlCount = 0x12;        // set/get control tables 0x45a0 / 0x45e8
        constexpr uint32_t ControlJpeg = 0xe, ControlLed = 9, ControlMirror = 0x10;

        // GT4.VOL entry eyetoy/gtshirt.bin, read with the GT3 recomp
        // (volume at LBA 105879, real TOC page 0x2159): entry page 35810 of the data area
        // (ISO byte 0x125c1800), compressed 1074 bytes, 28688 bytes unpacked, CRC32 0x412af037.
        constexpr uint64_t ShirtEntryOffset = 0x125c1800ull;
        constexpr size_t ShirtPackedBytes = 1074;
        constexpr size_t ShirtBytes = 28688;
        constexpr uint32_t ShirtCrc = 0x412af037u;

        enum class Mode
        {
            Absent,
            Shirt,
            Image,
            Webcam,
        };

        // strtol(s, 0, 10) as IOP sysclib#36 parses the module arguments.
        int64_t parseDecimal(std::string_view text)
        {
            size_t i = 0;
            while (i < text.size() && (text[i] == ' ' || (text[i] >= '\t' && text[i] <= '\r')))
                ++i;
            bool negative = false;
            if (i < text.size() && (text[i] == '+' || text[i] == '-'))
                negative = text[i++] == '-';
            int64_t value = 0;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
                value = std::min<int64_t>(value * 10 + (text[i] - '0'), 0x7fffffff);
            return negative ? -value : value;
        }

        struct ModuleArgs
        {
            uint8_t maxcam = 1, pack = 1, iopstrip = 0, isoc = 8; // 0x47d8..0x47db
            uint16_t framesize = 0x5000, micsize = 0x400;         // 0x47dc / 0x47de (16-bit stores)
            uint8_t samprate = 16;                                // 0x47e0

            void parse(std::string_view block)
            {
                // func_310c: strncmp per argument, strtol base 10; out-of-range values are ignored.
                while (!block.empty())
                {
                    const size_t end = block.find('\0');
                    const std::string_view arg = block.substr(0, end);
                    block = end == std::string_view::npos ? std::string_view{} : block.substr(end + 1);
                    auto value = [&](std::string_view prefix, int64_t &out) {
                        if (arg.substr(0, prefix.size()) != prefix)
                            return false;
                        out = parseDecimal(arg.substr(prefix.size()));
                        return true;
                    };
                    int64_t v = 0;
                    if (value("pack=", v))
                    {
                        if (uint32_t(v) < 2) pack = uint8_t(v);
                    }
                    else if (value("iopstrip=", v))
                    {
                        if (uint32_t(v) < 2) iopstrip = uint8_t(v);
                    }
                    else if (value("isoc=", v))
                    {
                        if (v == 4 || v == 8) isoc = uint8_t(v);
                    }
                    else if (value("maxcam=", v))
                    {
                        if (uint32_t(v - 1) < 2) maxcam = uint8_t(v);
                    }
                    else if (value("samprate=", v))
                    {
                        if (v == 0 || v == 8 || v == 16) samprate = uint8_t(v);
                    }
                    else if (value("framesize=", v))
                    {
                        if (uint32_t(v) < 0x100) framesize = uint16_t(uint32_t(v) << 10); // sh: 16 bits kept
                    }
                    else if (value("micsize=", v))
                    {
                        if (uint32_t(v) < 0x100) micsize = uint16_t(uint32_t(v) << 8);
                    }
                    // thpri=a,b only sets IOP thread priorities.
                }
            }

            // cmd 8 (EYETOY 0x1628-0x169c); the EE decodes it at CORE 0x58bff4.
            uint32_t info() const
            {
                return uint32_t(framesize >> 10) | micsize | uint32_t(pack) << 16 | uint32_t(maxcam) << 17 |
                       uint32_t(samprate / 8) << 19 | uint32_t(iopstrip) << 24;
            }
        };

        struct Control
        {
            uint32_t v1 = 0, v2 = 0;
        };

        struct Encoded
        {
            std::shared_ptr<const IpumFrame> frame;
            uint32_t width = 0, height = 0;
        };

        class EyeToy final : public EyeToyService
        {
        public:
            EyeToy(IopHost &host, Disc &disc, EyeToyOptions options) : host_(host), disc_(disc), options_(std::move(options))
            {
                const std::string &m = options_.mode;
                if (m.empty() || m == "absent" || m == "0" || m == "off")
                    mode_ = Mode::Absent;
                else if (m == "shirt" || m == "1" || m == "on")
                    mode_ = Mode::Shirt;
                else if (m.rfind("image:", 0) == 0 && m.size() > 6)
                    mode_ = Mode::Image;
                else if (m == "webcam" || m.rfind("webcam:", 0) == 0)
                    mode_ = Mode::Webcam;
                else
                    host_.log(LogLevel::Warning, "[GT4/EYETOY] unknown GT4_EYETOY='" + m +
                                                     "' (absent, shirt, image:<file>, webcam[:n]); no camera");
                options_.quantiser = std::clamp(options_.quantiser, 1, 31);
                options_.webcamDelayMs = std::clamp(options_.webcamDelayMs, 0, 60000);
            }

            ~EyeToy() override
            {
                std::lock_guard lock(mutex_);
                stopStreaming();
            }

            void reset() override
            {
                std::lock_guard lock(mutex_);
                stopStreaming();
                loaded_ = false;
                present_ = false;
                args_ = {};
                clearCameraState();
            }

            void moduleLoaded(std::string_view args) override
            {
                std::lock_guard lock(mutex_);
                stopStreaming();
                args_ = {};
                args_.parse(args);
                loaded_ = true;
                delivered_ = 0;
                clearCameraState();
                present_ = mode_ != Mode::Absent && ensureSource() && source_->available();
                writeSreg(present_ ? 1u : 0u); // module start clears it (0x359c); connect sets it (0x3e34)
                host_.log(LogLevel::Info, "[GT4/EYETOY] EYETOY loaded: info " + hex(args_.info()) + ", framesize " +
                                              std::to_string(args_.framesize) + "; " +
                                              (present_ ? "camera present: " + source_->describe()
                                                        : std::string("no camera (GT4_EYETOY=") + options_.mode + ")"));
            }

            void moduleStopped() override
            {
                std::lock_guard lock(mutex_);
                if (!loaded_)
                    return;
                stopStreaming();
                writeSreg(0); // module stop (0x3af8)
                loaded_ = false;
                present_ = false;
                clearCameraState();
                host_.log(LogLevel::Info, "[GT4/EYETOY] EYETOY stopped (" + std::to_string(delivered_) + " frames delivered)");
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                std::lock_guard lock(mutex_);
                std::array<uint32_t, 4> d{};
                const size_t bytes = std::min<size_t>(request.send.size, sizeof(d));
                if (bytes && !host_.readGuest(request.send.address, d.data(), bytes))
                    return rejected();
                if (bytes < 4)
                {
                    d[0] = 0; // handler 0x15b0: short buffer, result 0
                }
                else
                {
                    const uint32_t cmd = d[0];
                    const uint32_t arg1 = bytes >= 8 ? d[1] : 0, arg2 = bytes >= 12 ? d[2] : 0, camera = bytes >= 16 ? d[3] : 0;
                    d[0] = command(cmd, arg1, arg2, camera, d);
                    if (options_.log)
                        host_.log(LogLevel::Info, "[GT4/EYETOY] cmd " + hex(cmd) + " (" + hex(arg1) + ", " + hex(arg2) +
                                                      ", cam " + std::to_string(camera) + ") -> " + hex(d[0]));
                }
                return reply(host_, request, d.data(), std::min<size_t>(sizeof(d), request.receive.size));
            }

            void onVBlank(bool start, uint64_t tick) override
            {
                if (!start)
                    return;
                std::lock_guard lock(mutex_);
                tick_ = tick;
                if (!loaded_ || !present_ || !streaming_ || !ring_ || !sizeOk_)
                    return;
                if (streamTickPending_)
                {
                    streamTick_ = tick; // the first VBlank after cmd 3
                    streamTickPending_ = false;
                }
                if (source_->live() && !sourceStarted_ &&
                    (tick - streamTick_) * 1000u >= uint64_t(options_.webcamDelayMs) * 60u)
                    startLiveSource();
                // One frame per 60/fps VBlanks (cmd 7; GT4 asks 60 or 30).
                fpsCredit_ += fps_ ? fps_ : 60u;
                if (fpsCredit_ < 60u)
                    return;
                fpsCredit_ -= 60u;
                const std::shared_ptr<const IpumFrame> frame = currentFrame();
                if (!frame)
                    return;
                deliver(*frame);
                if (options_.log && ++logFrames_ >= 60u)
                {
                    host_.log(LogLevel::Info, "[GT4/EYETOY] vsync " + std::to_string(tick) + ": " + std::to_string(delivered_) +
                                                  " frames, last " + std::to_string(frame->bytes.size()) + " bytes, code " +
                                                  std::to_string(frame->quantiserScaleCode) + ", LED " + (led_ ? "on" : "off"));
                    logFrames_ = 0;
                }
            }

        private:
            uint32_t command(uint32_t cmd, uint32_t arg1, uint32_t arg2, uint32_t camera, std::array<uint32_t, 4> &d)
            {
                switch (cmd)
                {
                case 8: return args_.info();
                case 9: return 0; // sampling interval (func_29a8); GT4 never sends it
                case 2:
                    if (camera == 0)
                    {
                        ring_ = arg1; // frame ring (0x47a4[cam])
                        mic_ = arg2;  // microphone buffer = ring + 3*framesize (0x4850[cam])
                    }
                    return 0;
                default: break;
                }
                if (!present_ || camera != 0) // one camera object (maxcam 1)
                    return NotConnected;
                switch (cmd)
                {
                case 1: // open (0x1710): size, streaming off, counter 0
                    stopStreaming();
                    streaming_ = false;
                    width_ = arg1 & 0xffffu;
                    height_ = arg2 & 0xffffu;
                    counter_ = 0;
                    slot_ = 0;
                    sizeOk_ = width_ && height_ && width_ % 16 == 0 && height_ % 16 == 0 && width_ <= 640 && height_ <= 480 &&
                              slotBytes() >= 0x400u;
                    if (!sizeOk_)
                        host_.log(LogLevel::Warning, "[GT4/EYETOY] open " + std::to_string(width_) + "x" + std::to_string(height_) +
                                                         ": no frames for this size");
                    stillCache_.clear();
                    return 0;
                case 3: // start (0x1758): no-op while streaming
                    if (!streaming_)
                    {
                        streaming_ = true;
                        startStreaming();
                    }
                    return 0;
                case 4: // stop (0x17b0)
                    streaming_ = false;
                    stopStreaming();
                    return 0;
                case 5: // microphone start / stop (0x17bc / 0x17f0); GT4 never sends them
                case 6: return 0;
                case 7: // frame rate (0x17fc)
                    fps_ = arg1 & 0xffu;
                    return 0;
                default: break;
                }
                if ((cmd & ~0x3fffu) == 0x4000u && (cmd & 0x3fffu) < ControlCount)
                {
                    const uint32_t id = cmd & 0x3fffu;
                    controls_[id] = Control{arg1, arg2};
                    if (id == ControlLed)
                    {
                        // n=9: OV519 GPIO 0x71 bit 0 = (arg1 > 0) (EYETOY 0x1954); GT4 sends the
                        // recogniser's LED state as arg1 (CORE 0x1bf5c0 -> 0x1c4a90).
                        const bool on = static_cast<int32_t>(arg1) > 0;
                        if (on != led_.load() && options_.log)
                            host_.log(LogLevel::Info, "[GT4/EYETOY] LED " + std::string(on ? "on" : "off") + " at vsync " +
                                                          std::to_string(tick_));
                        led_ = on;
                    }
                    else if (id == ControlMirror)
                    {
                        mirror_ = (arg1 & 1u) != 0; // sensor reg 0x12 bit 6 (EYETOY 0x1a34)
                    }
                    else if (id == ControlJpeg && arg1 && !jpegWarned_)
                    {
                        jpegWarned_ = true;
                        host_.log(LogLevel::Warning, "[GT4/EYETOY] JPEG mode requested; only 'ipum' frames are produced");
                    }
                    return 0;
                }
                if ((cmd & ~0x7fffu) == 0x8000u && (cmd & 0x7fffu) < ControlCount)
                {
                    // get-control (table 0x45e8): the IRX reads the sensor; the stored values here.
                    const Control &c = controls_[cmd & 0x7fffu];
                    d[1] = c.v1;
                    d[2] = c.v2;
                    return 0;
                }
                return 0; // other commands fall through the handler with result 0
            }

            void clearCameraState()
            {
                ring_ = mic_ = 0;
                width_ = height_ = 0;
                sizeOk_ = false;
                fps_ = 60;
                streaming_ = false;
                counter_ = 0;
                slot_ = 0;
                controls_ = {};
                led_ = false;
                mirror_ = false;
                fpsCredit_ = 0;
                stillCache_.clear();
            }

            void writeSreg(uint32_t value)
            {
                if (value != sreg_ || !sregWritten_)
                    host_.log(LogLevel::Info, "[GT4/EYETOY] SIF register 7 = " + std::to_string(value));
                sreg_ = value;
                sregWritten_ = true;
                host_.writeGuest(EeSregTable + 4u * SregCamera, &value, 4);
            }

            uint32_t slotBytes() const
            {
                // framesize, halved for two cameras at widths up to 512 (EYETOY 0x1de8-0x1e38) [I: never for GT4].
                const uint32_t frame = args_.framesize;
                return (args_.maxcam >= 2 && width_ <= 0x200) ? frame / 2 : frame;
            }

            bool ensureSource()
            {
                if (source_)
                    return true;
                if (sourceFailed_)
                    return false;
                std::string error = "no picture source";
                switch (options_.sourceOverride ? Mode::Absent : mode_)
                {
                case Mode::Absent:
                    if (options_.sourceOverride)
                        source_ = options_.sourceOverride();
                    break;
                case Mode::Shirt:
                {
                    ShirtTemplate shirt;
                    if (loadShirt(shirt, error))
                        source_ = createShirtSource(std::move(shirt));
                    break;
                }
                case Mode::Image:
                {
                    RgbImage image;
                    const std::string path = options_.mode.substr(6);
                    if (loadImageFile(path, image, error))
                        source_ = createImageSource(std::move(image), path);
                    break;
                }
                case Mode::Webcam:
                {
                    const int device = options_.mode.size() > 7 ? std::atoi(options_.mode.c_str() + 7) : 0;
                    source_ = createWebcamSource(device, [this](int level, const std::string &text) {
                        host_.log(level == 2 ? LogLevel::Error : level == 1 ? LogLevel::Warning : LogLevel::Info, text);
                    });
                    if (!source_)
                        error = "no webcam support on this platform";
                    break;
                }
                }
                if (!source_)
                {
                    sourceFailed_ = true;
                    host_.log(LogLevel::Error, "[GT4/EYETOY] GT4_EYETOY=" + options_.mode + ": " + error + "; no camera");
                }
                return source_ != nullptr;
            }

            bool loadShirt(ShirtTemplate &shirt, std::string &error)
            {
                std::vector<uint8_t> raw;
                if (!options_.templatePath.empty())
                {
                    std::ifstream in(options_.templatePath, std::ios::binary);
                    raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                    if (raw.empty())
                        return error = "cannot read " + options_.templatePath, false;
                }
                else
                {
                    std::vector<uint8_t> packed(ShirtPackedBytes);
                    if (disc_.readBytes(ShirtEntryOffset, packed.data(), packed.size()) != packed.size())
                        return error = "cannot read eyetoy/gtshirt.bin from the disc image", false;
                    if (!unpackVolEntry(packed.data(), packed.size(), raw) || raw.size() != ShirtBytes ||
                        crc32(raw.data(), raw.size()) != ShirtCrc)
                        return error = "eyetoy/gtshirt.bin on the disc image does not match GT4 USA (SCUS-97328)", false;
                }
                if (!parseShirtTemplate(raw.data(), raw.size(), shirt))
                    return error = "not a GT shirt template ('GTSH')", false;
                host_.log(LogLevel::Info, "[GT4/EYETOY] GT shirt template " + std::to_string(shirt.width) + "x" +
                                              std::to_string(shirt.height) + " (CRC32 " + hex(crc32(raw.data(), raw.size())) + ")");
                return true;
            }

            void startStreaming()
            {
                if (!source_ || !sizeOk_)
                    return;
                fpsCredit_ = 0;
                streamTickPending_ = true;
                if (!source_->live())
                    source_->start(width_, height_, fps_); // renders the still picture once per size
                // Live sources open in onVBlank after options_.webcamDelayMs of streaming: GT4's
                // USA page probe starts and stops the camera within a few frames on every visit.
            }

            void startLiveSource()
            {
                sourceStarted_ = true;
                budget_ = slotBytes() - 4u;
                source_->start(width_, height_, fps_);
                {
                    std::lock_guard lock(encodeMutex_);
                    encodeStop_ = false;
                    encoded_ = {};
                }
                encoder_ = std::thread([this, w = width_, h = height_, budget = budget_] { encodeLoop(w, h, budget); });
            }

            void stopStreaming()
            {
                if (!sourceStarted_)
                    return;
                {
                    std::lock_guard lock(encodeMutex_);
                    encodeStop_ = true;
                }
                encodeWake_.notify_all();
                if (encoder_.joinable())
                    encoder_.join();
                source_->stop();
                sourceStarted_ = false;
            }

            // Mirror (control 0x10) and the virtual LED reflection, then 'ipum'.
            bool encodePicture(RgbImage &picture, bool led, bool mirror, bool fixedOrientation, size_t budget, int &code,
                               IpumFrame &out) const
            {
                if (mirror != options_.flip && !fixedOrientation)
                    mirrorHorizontally(picture);
                if (led && options_.reflect)
                    applyLedReflection(picture);
                YuvPicture yuv;
                rgbToYuv420(picture.rgb.data(), picture.width, picture.height, yuv);
                if (!encodeIpum(yuv, budget, code, out))
                    return false;
                code = out.quantiserScaleCode;
                return true;
            }

            void encodeLoop(uint32_t width, uint32_t height, size_t budget)
            {
                uint64_t lastSequence = ~0ull;
                int lastKey = -1, code = options_.quantiser;
                for (;;)
                {
                    {
                        std::unique_lock lock(encodeMutex_);
                        encodeWake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return encodeStop_; });
                        if (encodeStop_)
                            return;
                    }
                    RgbImage picture;
                    uint64_t sequence = 0;
                    if (!source_->latest(picture, sequence) || picture.width != width || picture.height != height)
                        continue;
                    const bool led = led_, mirror = mirror_;
                    const int key = (led ? 1 : 0) | (mirror ? 2 : 0);
                    if (sequence == lastSequence && key == lastKey)
                        continue;
                    auto frame = std::make_shared<IpumFrame>();
                    int next = code;
                    if (!encodePicture(picture, led, mirror, false, budget, next, *frame))
                        continue;
                    // Keep the quality up: step the start code back down while frames are small.
                    code = (frame->bytes.size() * 10 < budget * 6 && next > options_.quantiser) ? next - 1 : next;
                    lastSequence = sequence;
                    lastKey = key;
                    std::lock_guard lock(encodeMutex_);
                    encoded_ = Encoded{std::move(frame), width, height};
                }
            }

            std::shared_ptr<const IpumFrame> currentFrame()
            {
                const size_t budget = slotBytes() - 4u;
                if (source_->live())
                {
                    if (sourceStarted_)
                    {
                        std::lock_guard lock(encodeMutex_);
                        if (encoded_.frame && encoded_.width == width_ && encoded_.height == height_)
                            return encoded_.frame;
                    }
                    // Until the webcam delivers: a dark grey picture, as a camera warming up.
                    const int key = 8;
                    if (const auto it = stillCache_.find(key); it != stillCache_.end())
                        return it->second;
                    RgbImage grey;
                    grey.width = width_;
                    grey.height = height_;
                    grey.rgb.assign(size_t(width_) * height_ * 3, 64);
                    auto frame = std::make_shared<IpumFrame>();
                    int code = options_.quantiser;
                    if (!encodePicture(grey, false, false, true, budget, code, *frame))
                        return nullptr;
                    return stillCache_[key] = std::move(frame);
                }
                const bool led = led_ && options_.reflect, mirror = mirror_ != options_.flip && !source_->fixedOrientation();
                const int key = (led ? 1 : 0) | (mirror ? 2 : 0);
                if (const auto it = stillCache_.find(key); it != stillCache_.end())
                    return it->second;
                RgbImage picture;
                uint64_t sequence = 0;
                if (!source_->latest(picture, sequence) || picture.width != width_ || picture.height != height_)
                    return nullptr;
                auto frame = std::make_shared<IpumFrame>();
                int code = options_.quantiser;
                if (!encodePicture(picture, led_, mirror_, source_->fixedOrientation(), budget, code, *frame))
                {
                    host_.log(LogLevel::Error, "[GT4/EYETOY] cannot encode a " + std::to_string(width_) + "x" +
                                                   std::to_string(height_) + " frame in " + std::to_string(budget) + " bytes");
                    return nullptr;
                }
                if (options_.log)
                    host_.log(LogLevel::Info, "[GT4/EYETOY] still frame " + std::to_string(width_) + "x" + std::to_string(height_) +
                                                  (led ? " (LED reflection)" : "") + ": " + std::to_string(frame->bytes.size()) +
                                                  " bytes, code " + std::to_string(frame->quantiserScaleCode) + ", " +
                                                  std::to_string(frame->passes) + " passes");
                return stillCache_[key] = std::move(frame);
            }

            void deliver(const IpumFrame &frame)
            {
                const uint32_t slot = slotBytes();
                if (frame.bytes.size() + 4u > slot)
                    return;
                const uint32_t base = ring_ + slot_ * slot;
                // The slot first, then its trailer: the EE picks the slot with the
                // newest trailer (CORE 0x58caa4), so it never pairs it with old data.
                host_.writeGuest(base, frame.bytes.data(), frame.bytes.size());
                const uint32_t counter = counter_;
                host_.writeGuest(base + slot - 4u, &counter, 4);
                counter_ = static_cast<int32_t>(counter_ + 1) < 0 ? 0u : counter_ + 1u; // 0x1e74-0x1e94
                slot_ = (slot_ + 1u) % 3u;
                ++delivered_;
            }

            IopHost &host_;
            Disc &disc_;
            EyeToyOptions options_;
            Mode mode_ = Mode::Absent;
            std::mutex mutex_;

            std::unique_ptr<FrameSource> source_;
            bool sourceFailed_ = false;
            bool sourceStarted_ = false;

            ModuleArgs args_;
            bool loaded_ = false, present_ = false;
            uint32_t sreg_ = 0;
            bool sregWritten_ = false;
            uint32_t ring_ = 0, mic_ = 0;
            uint32_t width_ = 0, height_ = 0, fps_ = 60;
            bool sizeOk_ = false, streaming_ = false, jpegWarned_ = false;
            uint32_t counter_ = 0, slot_ = 0;
            std::array<Control, ControlCount> controls_{};
            std::atomic<bool> led_{false}, mirror_{false};

            uint64_t tick_ = 0, streamTick_ = 0;
            bool streamTickPending_ = false;
            uint32_t fpsCredit_ = 0, logFrames_ = 0;
            uint64_t delivered_ = 0;
            size_t budget_ = 0;
            std::map<int, std::shared_ptr<const IpumFrame>> stillCache_;

            std::thread encoder_;
            std::mutex encodeMutex_;
            std::condition_variable encodeWake_;
            bool encodeStop_ = false;
            Encoded encoded_;
        };

        bool envFlag(const char *name, bool fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F') : fallback;
        }

        int envInt(const char *name, int fallback)
        {
            const char *v = std::getenv(name);
            return v && *v ? std::atoi(v) : fallback;
        }
    }

    EyeToyOptions eyeToyOptionsFromEnvironment()
    {
        EyeToyOptions o;
        if (const char *v = std::getenv("GT4_EYETOY"); v && *v)
            o.mode = v;
        o.log = envFlag("GT4_EYETOY_LOG", false);
        o.reflect = envFlag("GT4_EYETOY_REFLECT", true);
        o.flip = envFlag("GT4_EYETOY_FLIP", false);
        o.webcamDelayMs = envInt("GT4_EYETOY_WEBCAM_DELAY_MS", o.webcamDelayMs);
        o.quantiser = envInt("GT4_EYETOY_QS", o.quantiser);
        if (const char *v = std::getenv("GT4_EYETOY_TEMPLATE"); v && *v)
            o.templatePath = v;
        return o;
    }

    std::unique_ptr<EyeToyService> createEyeToy(IopHost &host, Disc &disc, const EyeToyOptions &options)
    {
        return std::make_unique<EyeToy>(host, disc, options);
    }

    std::unique_ptr<EyeToyService> createEyeToy(IopHost &host, Disc &disc)
    {
        return createEyeToy(host, disc, eyeToyOptionsFromEnvironment());
    }
}
