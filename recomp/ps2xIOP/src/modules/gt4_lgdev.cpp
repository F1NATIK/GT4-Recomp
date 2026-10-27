#include "gt4_iop.h"
#include "gt4_lgdev.h"

#include <cstdlib>
#include <set>

// LGDEV 1.11.036 "PDI branch" (LGDEV.IRX SHA-256 72527de3...b73273, iopmod
// LgDev_tb_rb_Driver): the Logitech USB wheel driver GT4 talks to through
// liblgdev 1.11.036 (EE 0x560778..0x561dfc). Written from GT4's own module
// (analysis/iop-sys/disasm/gt4/LGDEV.txt, IOP+ addresses below) and the spec
// analysis/phase5/specs/p5b-wheel-poweroff-padman.md; GT3 has no such service.
//
// One SIF RPC server, SID 0x046d046d (IOP+0x2194), handler IOP+0x21dc, one
// static 0x240-byte buffer (IOP+0x9540) that is request and reply: function
// 1..18 = table IOP+0x8d08, result in word 0, anything else 0x80000000. Every
// function runs under one semaphore (IOP+0x90d4) - here one mutex. The EE
// learns "device list changed" from a 64-byte status area whose address fn12
// receives (+0x5c): LGDEV SIF-DMAs {1, 0 x 15} there at init, on connect and
// on disconnect (IOP+0x1950). Host wheels come from a HostWheel (gt4_lgdev.h);
// no host wheel = empty device table (fn1 -> 0x80000002, as before).

namespace ps2x::iop::gt4
{
    namespace
    {
        constexpr uint32_t Sid = 0x046d046du;
        constexpr uint32_t BufferBytes = 0x240u;  // EE 0x873f40, ssize = rsize = 0x240 (EE 0x560b14)
        constexpr uint32_t Version = 0x010b2400u; // fn12 +4; EE 0x5608b0 spins on anything but 0x010bxxxx
        constexpr unsigned Slots = 8;             // no load arguments -> dynamic mode, 8 devices (IOP+0x1efc)
        constexpr unsigned EffectSlots = 64;      // per device (IOP+0x7994)
        constexpr unsigned RingSamples = 6;       // per device (IOP+0x6194)
        constexpr uint32_t StatusBytes = 64;      // IOP+0x8c20 = {1, 0 x 15}

        constexpr uint32_t ErrUnsupported = 0x80000000u; // function number, fn1 product
        constexpr uint32_t ErrNoDevice = 0x80000002u;    // fn1: no index-th device
        constexpr uint32_t ErrArgument = 0x80000004u;    // handle -1, not open, no effect, effect type, property
        constexpr uint32_t ErrAlreadyOpen = 0x80000005u;
        constexpr uint32_t ErrStale = 0x80000006u;       // empty slot or handle / id of an older open
        constexpr uint32_t ErrNoEffect = 0x80000007u;    // 64 effects in use

        // Buffer offsets (EE wrappers 0x560ac0..0x5615b0, IOP dispatcher 0x221c..0x2394).
        constexpr uint32_t OffIndex = 0x04, OffHandle = 0x08, OffEffectId = 0x18, OffProperty = 0x58,
                           OffValue = 0x5c, OffMode = 0x60, OffData = 0x70;
        constexpr uint32_t CalibrationBytes = 0x160; // fn17/18: 8 records of 0x2c (IOP+0x3804 / 0x3ba0)

        // Per product: device info words (IOP+0x48d8) and initial axis
        // calibration (IOP+0x45a8), echoed only by fn17 (GT4 never calls it).
        // DFP (firmware taken as >= 0x1104): fixed ranges, the same as LGDEV
        // keeps. DF / GT Force: LGDEV widens these from raw reports; the native
        // samples are already normalised, so the initial values stay.
        struct ModelInfo
        {
            uint32_t product;
            uint32_t deviceClass; // info +0: 0x11/0x12 also enable the combined pedal axis
            uint32_t caps;        // info +4: 0x400 = property 5
            uint32_t feature;     // info +8 [I name]
            std::array<uint16_t, 8> calMin;
            std::array<uint16_t, 8> calMax;
            const char *name;
        };
        constexpr ModelInfo Dfp{0x046dc298u, 0x12, 0x7e8, 7, {0, 0, 0, 0, 0, 0, 0, 0}, {0x3fff, 0, 0, 0, 0, 0, 0xff, 0xff}, "Driving Force Pro"};
        constexpr ModelInfo Df{0x046dc294u, 0x12, 0x1e8, 3, {0x100, 0, 0, 0, 0, 0, 0xff, 0xff}, {0x300, 0, 0, 0, 0, 0, 0, 0}, "Driving Force"};
        constexpr ModelInfo GtForce{0x046dc293u, 0x11, 0, 3, {0x100, 0, 0, 0, 0, 0, 0xff, 0xff}, {0x300, 0, 0, 0, 0, 0, 0, 0}, "GT Force"};

        const ModelInfo &modelInfo(WheelModel model)
        {
            switch (model)
            {
            case WheelModel::DrivingForce: return Df;
            case WheelModel::GtForce: return GtForce;
            case WheelModel::DrivingForcePro: break;
            }
            return Dfp;
        }

        // Products fn1 hands out (IOP+0x25e8..0x264c).
        bool supportedProduct(uint32_t p)
        {
            return p == 0x046dc298u || p == 0x046dca03u || (p >= 0x046dc293u && p <= 0x046dc295u) ||
                   (p - 0x046dca00u) < 0x80u;
        }

        // Effect classes by their set-params clamp.
        enum class EffectClass : uint8_t
        {
            Constant,  // IOP+0x4b0
            Condition, // IOP+0xbb4 (both axes): DFP spring/damper/friction, GT Force spring/damper
            DfSpring,  // IOP+0x894 (DF 0xc294 spring)
            DfDamper,  // IOP+0x668 (DF 0xc294 / 0xc295 damper)
        };

        int32_t clampTo(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

        WheelEffectParams clampParams(EffectClass cls, WheelEffectParams p)
        {
            auto &w = p.word;
            switch (cls)
            {
            case EffectClass::Constant:
                w[3] = clampTo(w[3], -255, 255);         // magnitude
                w[4] = ((w[4] % 360) * 256) / 360;      // degrees -> 0..255 circle (C division, as the IOP)
                break;
            case EffectClass::Condition:
                for (unsigned b : {3u, 9u})
                {
                    w[b + 2] = std::min(w[b + 2], 255); // saturations: upper bound only
                    w[b + 3] = std::min(w[b + 3], 255);
                    w[b + 4] = clampTo(w[b + 4], -255, 255);
                    w[b + 5] = clampTo(w[b + 5], -255, 255);
                }
                break;
            case EffectClass::DfDamper:
                w[5] = std::min(w[5], 255);
                w[6] = std::min(w[6], 255);
                w[7] = clampTo(w[7], -255, 255);
                w[8] = clampTo(w[8], -255, 255);
                break;
            case EffectClass::DfSpring:
                w[5] = std::min(static_cast<int32_t>(static_cast<uint32_t>(w[5]) >> 1), 0x7f);
                w[6] = std::min(static_cast<int32_t>(static_cast<uint32_t>(w[6]) >> 1), 0x7f);
                w[7] = clampTo(w[7], -255, 255);
                w[8] = clampTo(w[8], -255, 255);
                w[4] = std::min(w[4], 0x10000);
                w[3] = clampTo(w[3], -0x7fff, 0x7fff);
                break;
            }
            return p;
        }

        struct Sample
        {
            std::array<uint8_t, 24> b{};
            bool operator==(const Sample &) const = default;
        };

        // Ring entry LGDEV writes at connect (IOP+0x45a8): axes 0, axis 4 0x8000, hat and +0x13 0xff.
        Sample defaultSample()
        {
            Sample s;
            s.b[0x0b] = 0x80;
            s.b[0x12] = 0xff;
            s.b[0x13] = 0xff;
            return s;
        }

        struct Effect
        {
            bool used = false;
            WheelEffectType type = WheelEffectType::Constant;
            EffectClass cls = EffectClass::Constant;
            WheelEffectParams params{};
            bool playing = false;
        };

        struct Device // LGDEV slot object (IOP+0x4348) + its driver object (+0x180)
        {
            unsigned host = 0;
            uint32_t plug = 0;
            const ModelInfo *model = &Dfp;
            std::array<uint8_t, 24> info{}; // slot+0x24
            int32_t enumIndex = -1;         // slot+0x0c, set by fn1
            bool open = false;              // slot+0x1e
            int32_t handle = -1;            // slot+0x08
            uint32_t flags = 1;             // driver+0x10c: bit 0 auto-centre, bit 1 DFP 900 degrees
            uint32_t gain = 0xff;           // driver+0x118
            bool pipe = false;              // driver+0x11c, created by the first open
            bool prop5 = false;             // slot+0x1f0 bit 11
            std::array<Sample, RingSamples> ring{};
            uint32_t head = 0, write = 1, count = 1; // slot+0x80/0x84/0x88
            std::array<Effect, EffectSlots> effects{};
            std::array<uint16_t, 8> calMin{}, calMax{};

            bool dfp() const { return model->product == 0x046dc298u; }
        };

        const char *functionName(uint32_t fn)
        {
            static constexpr const char *names[] = {"?", "GetDeviceInfo", "Open", "Close", "GetProperty", "SetProperty",
                                                    "Read", "DownloadEffect", "StartEffect", "StopEffect", "UnloadEffect",
                                                    "(none)", "Init", "UpdateEffect", "ReadMany", "StartEffects",
                                                    "StopEffects", "SaveCalibration", "RestoreCalibration"};
            return fn < std::size(names) ? names[fn] : "?";
        }

        class Lgdev final : public Component
        {
        public:
            explicit Lgdev(IopHost &host) : host_(host)
            {
                wheel_ = createHostWheel([this](std::string_view line) { host_.log(LogLevel::Info, "[GT4/LGDEV] " + std::string(line)); });
                const char *v = std::getenv("PS2X_WHEEL_LOG");
                verbose_ = v && *v && *v != '0';
                clearState();
            }

            void reset() override
            {
                std::lock_guard lock(mutex_);
                // An IOP reboot unloads LGDEV: forces stop, the module starts from its .data again.
                for (auto &slot : slots_)
                    if (slot && slot->open)
                        send(*slot, {WheelCommandKind::Close});
                clearState();
            }

            RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != Sid)
                    return rejected();
                std::lock_guard lock(mutex_);
                const size_t in = std::min<size_t>(BufferBytes, request.send.size);
                if (in && !host_.readGuest(request.send.address, buffer_.data(), in))
                    return rejected();
                const uint32_t fn = request.function;
                const bool trace = verbose_ && fn != 6 && fn != 14;
                const std::string args = trace ? describeArgs(fn) : std::string();
                const uint32_t result = dispatch(fn);
                put32(0, result);
                if (seen_.insert((uint64_t(fn) << 32) | result).second)
                    host_.log(fn >= 1 && fn <= 18 && fn != 11 ? LogLevel::Info : LogLevel::Warning,
                              "[GT4/LGDEV] fn " + std::to_string(fn) + " " + functionName(fn) + " -> " + hex(result) + " (first)");
                if (trace)
                    host_.log(LogLevel::Info, "[GT4/LGDEV] vsync " + std::to_string(vsync_) + " fn " + std::to_string(fn) + " " +
                                                  functionName(fn) + args + " -> " + hex(result) + describeOutputs(fn, result));
                return reply(host_, request, buffer_.data(), std::min<size_t>(BufferBytes, request.receive.size));
            }

            void onVBlank(bool start, uint64_t tick) override
            {
                if (!start)
                    return;
                std::lock_guard lock(mutex_);
                vsync_ = tick;
                if (!wheel_)
                    return;
                wheel_->onVBlank(tick);
                refreshAll();
            }

            void onPowerOff(PowerOffStage stage) override
            {
                if (stage != PowerOffStage::Dev9Off)
                    return;
                std::lock_guard lock(mutex_);
                // POWOFF step 2 switches DEV9 (and with it the USB wheels) off: stop every force.
                for (auto &slot : slots_)
                    if (slot)
                    {
                        send(*slot, {WheelCommandKind::PowerOff});
                        host_.log(LogLevel::Info, "[GT4/LGDEV] power-off: forces stopped on host wheel " + std::to_string(slot->host));
                    }
                // On the console the wheels have no power from here on. The game keeps running until
                // the host quit takes effect (GT4's wheel thread still sends fn13/fn15 after this
                // VBlank's IOP work), so every later device command is dropped; replies are unchanged.
                // Sticky: power-off ends the run, an IOP reset does not undo it.
                poweredOff_ = true;
                if (wheel_)
                    host_.log(LogLevel::Info, "[GT4/LGDEV] power-off: no further commands reach the host wheel");
            }

        private:
            // ---------------------------------------------------------------- buffer
            uint32_t word(uint32_t off) const
            {
                uint32_t v;
                std::memcpy(&v, buffer_.data() + off, 4);
                return v;
            }
            int32_t sword(uint32_t off) const { return static_cast<int32_t>(word(off)); }
            void put32(uint32_t off, uint32_t v) { std::memcpy(buffer_.data() + off, &v, 4); }
            void put16(uint32_t off, uint16_t v) { std::memcpy(buffer_.data() + off, &v, 2); }
            WheelEffectParams params(uint32_t off) const
            {
                WheelEffectParams p;
                std::memcpy(p.word.data(), buffer_.data() + off, sizeof(p.word));
                return p;
            }

            void clearState()
            {
                slots_ = {};
                hostSlot_.fill(-1);
                hostPlug_.fill(0);
                eeStatus_ = 0;
                openCounter_ = 1; // IOP+0x9158 (.data = 1)
                slotCounter_ = 0; // IOP+0x915c
                effectCounter_ = 0; // IOP+0x93b4
                buffer_.fill(0);
                calibration_.fill(0);
                seen_.clear();
                noSlotLogged_ = false;
            }

            void send(const Device &d, WheelCommand command)
            {
                if (!wheel_ || poweredOff_)
                    return;
                if (verbose_)
                    host_.log(LogLevel::Info, "[GT4/LGDEV] vsync " + std::to_string(vsync_) + " host wheel " + std::to_string(d.host) +
                                                  ": " + describeWheelCommand(command));
                wheel_->command(d.host, command);
            }

            void notifyEe()
            {
                // IOP+0x1950: SIF DMA of {1, 0 x 15} to the EE status area, only once fn12 gave its address.
                if (!eeStatus_)
                    return;
                std::array<uint8_t, StatusBytes> block{};
                block[0] = 1;
                if (!host_.writeGuest(eeStatus_, block.data(), block.size()))
                    host_.log(LogLevel::Warning, "[GT4/LGDEV] status area " + hex(eeStatus_) + " not writable");
            }

            // ---------------------------------------------------------------- devices
            void refreshAll()
            {
                if (!wheel_)
                    return;
                // Also the host wheels that are connected here but beyond a deviceCount() that shrank
                // (an adapter may report only the wheels attached now): they are unplugged.
                const unsigned n = std::min(wheel_->deviceCount(), HostWheel::MaxDevices);
                for (unsigned h = 0; h < HostWheel::MaxDevices; ++h)
                    if (h < n || hostSlot_[h] >= 0)
                        refreshHost(h);
            }

            // Connect / disconnect / new sample for one host wheel.
            void refreshHost(unsigned h)
            {
                WheelInput input{};
                const bool present = h < std::min(wheel_->deviceCount(), HostWheel::MaxDevices) && wheel_->poll(h, input) &&
                                     input.connected;
                if (hostSlot_[h] >= 0 && (!present || input.plug != hostPlug_[h]))
                    disconnect(h);
                if (hostSlot_[h] < 0 && present)
                    connect(h, input);
                if (hostSlot_[h] >= 0)
                    insert(*slots_[hostSlot_[h]], sampleFrom(*slots_[hostSlot_[h]], input));
            }

            void connect(unsigned h, const WheelInput &input)
            {
                // Slot allocation is round-robin from a counter (IOP+0x4348).
                int slot = -1;
                for (unsigned k = 0; k < Slots; ++k)
                {
                    const unsigned s = (slotCounter_ + k) % Slots;
                    if (!slots_[s])
                    {
                        slot = static_cast<int>(s);
                        break;
                    }
                }
                if (slot < 0)
                {
                    if (!noSlotLogged_)
                        host_.log(LogLevel::Warning, "[GT4/LGDEV] no free LGDEV slot for host wheel " + std::to_string(h));
                    noSlotLogged_ = true;
                    return;
                }
                slotCounter_ = (static_cast<unsigned>(slot) + 1) % Slots;
                auto d = std::make_unique<Device>();
                d->host = h;
                d->plug = input.plug;
                d->model = &modelInfo(wheel_->model(h));
                const uint32_t infoWords[3] = {d->model->deviceClass, d->model->caps, d->model->feature};
                std::memcpy(d->info.data(), infoWords, sizeof(infoWords));
                d->info[0x0c] = wheel_->usbPort(h); // usbd #13 location, root port first (IOP+0x4224)
                d->ring.fill(defaultSample());
                d->calMin = d->model->calMin;
                d->calMax = d->model->calMax;
                applyCalibration(*d); // IOP+0x4d24 at device ready
                hostSlot_[h] = slot;
                hostPlug_[h] = input.plug;
                host_.log(LogLevel::Info, "[GT4/LGDEV] host wheel " + std::to_string(h) + " connected as " + d->model->name + " (" +
                                              hex(d->model->product) + "), LGDEV slot " + std::to_string(slot) + ", USB port byte " +
                                              std::to_string(d->info[0x0c]) + " at vsync " + std::to_string(vsync_));
                slots_[slot] = std::move(d);
                notifyEe(); // ready flag + status DMA (IOP+0x4d34 / 0x4ec4)
            }

            void disconnect(unsigned h)
            {
                const int slot = hostSlot_[h];
                if (slots_[slot] && slots_[slot]->open)
                    send(*slots_[slot], {WheelCommandKind::Close}); // the device is gone; make sure nothing keeps pushing
                slots_[slot].reset();                               // slot table entry = 0 (IOP+0x42d8)
                hostSlot_[h] = -1;
                host_.log(LogLevel::Info, "[GT4/LGDEV] host wheel " + std::to_string(h) + " disconnected (LGDEV slot " +
                                              std::to_string(slot) + ") at vsync " + std::to_string(vsync_));
                notifyEe(); // IOP+0x4324..0x4330
            }

            Sample sampleFrom(const Device &d, const WheelInput &in) const
            {
                // Report parser (IOP+0x565c) memsets the sample, the normaliser
                // (IOP+0x5210) fills axes 0, 6, 7 for a DFP, hat, +0x13 = 0xff, buttons.
                Sample s;
                const int16_t steer = in.steer == -32768 ? int16_t(-32767) : in.steer;
                std::memcpy(&s.b[0x02], &steer, 2);
                if (d.model->deviceClass == 0x11 || d.model->deviceClass == 0x12)
                {
                    // Combined pedal axis 1 = (axis 7 - axis 6) / 2, -0x8000 -> -0x7fff (IOP+0x6088..0x60d0).
                    int32_t combined = (static_cast<int32_t>(in.pedalB) - static_cast<int32_t>(in.pedalA)) >> 1;
                    if (combined == -0x8000)
                        combined = -0x7fff;
                    const int16_t c = static_cast<int16_t>(combined);
                    std::memcpy(&s.b[0x04], &c, 2);
                }
                std::memcpy(&s.b[0x0e], &in.pedalA, 2);
                std::memcpy(&s.b[0x10], &in.pedalB, 2);
                s.b[0x12] = in.hat == 8 ? 0xff : in.hat; // the DFP parser maps only 8 (centre) to 0xff (IOP+0x5d34)
                s.b[0x13] = 0xff;
                std::memcpy(&s.b[0x14], &in.buttons, 4); // the model's LGDEV word as given (DF sets bit 31 too)
                return s;
            }

            // USB interrupt handler (IOP+0x6130..0x6220): keep a sample only when it differs from the previous one.
            static void insert(Device &d, const Sample &s)
            {
                // Previous = entry write-1, entry 5 when write is 0 (IOP+0x6128 delay slot).
                if (d.ring[(d.write + RingSamples - 1) % RingSamples] == s)
                    return;
                if (d.count >= RingSamples)
                {
                    --d.count;
                    d.head = (d.head + 1) % RingSamples;
                }
                d.ring[d.write] = s;
                d.write = (d.write + 1) % RingSamples;
                ++d.count;
            }

            void applyCalibration(Device &d)
            {
                // IOP+0x39b4 / 0x3a90: a record with the same product and 8-byte location is applied and cleared.
                for (unsigned k = 0; k < 8; ++k)
                {
                    uint8_t *r = calibration_.data() + 0x2c * k;
                    uint32_t product;
                    std::memcpy(&product, r + 8, 4);
                    if (product != d.model->product || std::memcmp(r, d.info.data() + 0x0c, 8) != 0)
                        continue;
                    for (unsigned a = 0; a < 8; ++a)
                    {
                        std::memcpy(&d.calMin[a], r + 0x0c + 2 * a, 2);
                        std::memcpy(&d.calMax[a], r + 0x1c + 2 * a, 2);
                    }
                    std::memset(r, 0, 0x2c);
                    return;
                }
            }

            Device *bySlotOf(int32_t handle) { return slots_[static_cast<uint32_t>(handle) & 7u].get(); }

            // fn8/9/10/13: id >> 16 must be the device's current handle (IOP+0x3394..0x3408).
            Effect *effectFor(uint32_t id, uint32_t &error, Device *&device)
            {
                const int32_t h = static_cast<int32_t>(id) >> 16;
                device = slots_[static_cast<uint32_t>(h) & 7u].get();
                if (!device || device->handle != h)
                {
                    error = ErrStale;
                    return nullptr;
                }
                if (!device->open)
                {
                    error = ErrArgument;
                    return nullptr;
                }
                Effect &e = device->effects[id & 0x3fu];
                if (!e.used)
                {
                    error = ErrArgument; // driver IOP+0x75c4 / 0x7688 / 0x772c / 0x8518
                    return nullptr;
                }
                return &e;
            }

            // Driver flag setter IOP+0x77e4 and apply IOP+0x7a44 (only once the pipe exists).
            void setFlags(Device &d, uint32_t flags)
            {
                flags &= 3u;
                if ((d.flags & 3u) == flags)
                    return;
                d.flags = (d.flags & ~3u) | flags;
                if (!d.pipe)
                    return;
                send(d, {WheelCommandKind::AutoCentre, 0, 0, {}, {}, static_cast<int32_t>(d.flags & 1u)}); // 0xf4 / 0xf5
                if (d.dfp())
                    send(d, {WheelCommandKind::Range, 0, 0, {}, {}, (d.flags & 2u) ? 900 : 200}); // 0xf8 0x03 / 0x02
                else
                    d.flags &= ~2u; // IOP+0x7ab0: no rotation range on other models
            }

            // ---------------------------------------------------------------- functions
            uint32_t dispatch(uint32_t fn)
            {
                switch (fn)
                {
                case 1: return getDeviceInfo(sword(OffIndex));
                case 2: return openDevice(sword(OffIndex));
                case 3: return closeDevice(sword(OffHandle));
                case 4: return getProperty(sword(OffHandle), sword(OffProperty));
                case 5: return setProperty(sword(OffHandle), sword(OffProperty), sword(OffValue));
                case 6: return read(sword(OffHandle), word(OffMode), OffData);
                case 7: return download(sword(OffHandle));
                case 8: return startEffect(word(OffEffectId));
                case 9: return stopEffect(word(OffEffectId));
                case 10: return unloadEffect(word(OffEffectId));
                case 12: return init();
                case 13: return update(word(OffEffectId));
                case 14: return readMany();
                case 15: return effectsMany(true);
                case 16: return effectsMany(false);
                case 17: return saveCalibration();
                case 18: return restoreCalibration();
                default: return ErrUnsupported; // fn11 (table entry IOP+0x2398) and outside 1..18
                }
            }

            uint32_t init() // fn12, IOP+0x2350
            {
                put32(OffIndex, Version);
                eeStatus_ = word(OffValue);
                refreshAll();
                notifyEe();
                return 0;
            }

            uint32_t getDeviceInfo(int32_t index) // fn1, IOP+0x2518
            {
                if (index >= static_cast<int32_t>(Slots))
                    return ErrNoDevice;
                if (index == 0)
                    for (auto &slot : slots_)
                        if (slot)
                            slot->enumIndex = -1;
                Device *d = nullptr;
                int32_t n = -1;
                for (auto &slot : slots_) // index-th ready device in slot order (IOP+0x242c)
                    if (slot && ++n == index)
                    {
                        d = slot.get();
                        break;
                    }
                if (!d)
                    return ErrNoDevice;
                if (!supportedProduct(d->model->product))
                    return ErrUnsupported;
                std::memcpy(buffer_.data() + OffData, d->info.data(), d->info.size());
                put32(OffData + 0x14, static_cast<uint32_t>(d->handle));
                d->enumIndex = index;
                return 0;
            }

            uint32_t openDevice(int32_t index) // fn2, IOP+0x270c
            {
                put32(OffHandle, 0xffffffffu);
                if (index >= static_cast<int32_t>(Slots))
                    return ErrArgument;
                int slot = -1;
                for (unsigned s = 0; s < Slots; ++s) // IOP+0x23b8: by the index fn1 recorded
                    if (slots_[s] && slots_[s]->enumIndex == index)
                    {
                        slot = static_cast<int>(s);
                        break;
                    }
                if (slot < 0)
                    return ErrArgument;
                Device &d = *slots_[slot];
                if (d.open)
                {
                    put32(OffHandle, static_cast<uint32_t>(d.handle));
                    return ErrAlreadyOpen;
                }
                d.open = true;
                d.handle = static_cast<int32_t>((openCounter_ << 4) | (static_cast<uint32_t>(slot) & 0xffu));
                ++openCounter_;
                send(d, {WheelCommandKind::Open});
                if (!d.pipe) // IOP+0x7d5c: first open after connect sends 0xf3 (stop all) and 0xf4 (centring on)
                {
                    d.pipe = true;
                    send(d, {WheelCommandKind::StopAll});
                    send(d, {WheelCommandKind::AutoCentre, 0, 0, {}, {}, 1});
                }
                d.gain = 0xff; // fn2 sets gain 0xff on every open (IOP+0x2850)
                send(d, {WheelCommandKind::Gain, 0, 0, {}, {}, 0xff});
                put32(OffHandle, static_cast<uint32_t>(d.handle));
                return 0;
            }

            uint32_t closeDevice(int32_t handle) // fn3, IOP+0x28d4
            {
                if (handle == -1)
                    return ErrArgument;
                Device *d = bySlotOf(handle);
                if (!d)
                    return 0; // empty slot: success
                if (!d->open)
                    return ErrArgument;
                d->open = false;
                d->handle = -1;
                d->effects = {}; // IOP+0x7eac + 0x78d4: playing effects detached, all 64 destroyed
                d->flags = 1;    // 0x78d4 (no device command)
                send(*d, {WheelCommandKind::Close});
                return 0;
            }

            uint32_t getProperty(int32_t handle, int32_t property) // fn4, IOP+0x29bc
            {
                if (handle == -1)
                    return ErrArgument;
                Device *d = bySlotOf(handle);
                if (!d)
                    return ErrStale;
                if (!d->open)
                    return ErrArgument;
                uint32_t caps;
                std::memcpy(&caps, d->info.data() + 4, 4);
                switch (property)
                {
                case 2: put32(OffValue, d->flags & 1u); return 0;
                case 3: put32(OffValue, d->gain); return 0;
                case 4: put32(OffValue, (d->flags >> 1) & 1u); return 0;
                case 5:
                    if (caps & 0x400u)
                    {
                        put32(OffValue, d->prop5 ? 1u : 0u);
                        return 0;
                    }
                    put32(OffValue, 0);
                    return ErrArgument;
                case 0x10005: put32(OffValue, d->model->product); return 0;
                default: return ErrArgument; // IOP+0x2a68 j 0x2b84 (lui in the delay slot) -> 0x80000004
                }
            }

            uint32_t setProperty(int32_t handle, int32_t property, int32_t value) // fn5, IOP+0x2bcc
            {
                if (handle == -1)
                    return ErrArgument;
                Device *d = bySlotOf(handle);
                if (!d)
                    return ErrStale;
                if (!d->open)
                    return ErrArgument;
                uint32_t caps;
                std::memcpy(&caps, d->info.data() + 4, 4);
                switch (property)
                {
                case 3:
                    d->gain = static_cast<uint32_t>(clampTo(value, 0, 255)); // IOP+0x7844
                    send(*d, {WheelCommandKind::Gain, 0, 0, {}, {}, static_cast<int32_t>(d->gain)});
                    return 0;
                case 2:
                case 4:
                {
                    const uint32_t mask = property == 2 ? 1u : 2u;
                    setFlags(*d, value == 1 ? (d->flags | mask) : (d->flags & ~mask)); // only exactly 1 sets
                    return 0; // the driver's result is not looked at (IOP+0x2d10)
                }
                case 5:
                    if (!(caps & 0x400u))
                        return ErrArgument;
                    d->prop5 = value != 0;
                    return 0;
                default:
                    return ErrArgument; // IOP+0x2c70 / 0x2c8c j 0x2d98 -> 0x80000004 (0x10005 included)
                }
            }

            uint32_t read(int32_t handle, uint32_t mode, uint32_t out) // fn6, IOP+0x2de0
            {
                put16(out, 0xff);
                if (handle == -1)
                    return ErrArgument;
                const unsigned slot = static_cast<uint32_t>(handle) & 7u;
                if (slots_[slot] && wheel_)
                    refreshHost(slots_[slot]->host); // newest host state (USB reports arrive many times per frame)
                Device *d = slots_[slot].get();
                if (!d)
                    return ErrStale;
                if (!d->open)
                    return ErrArgument;
                uint16_t status = 0;
                const Sample *s;
                if (d->count == 0)
                {
                    // Nothing new: the entry read last, status 1.
                    s = &d->ring[d->head == 0 ? RingSamples - 1 : d->head - 1];
                    status = 1;
                }
                else if (mode & 1u)
                {
                    // Newest; everything older is dropped.
                    const uint32_t newest = (d->head + d->count - 1) % RingSamples;
                    s = &d->ring[newest];
                    d->head = (newest + 1) % RingSamples;
                    d->count = 0;
                }
                else
                {
                    // Oldest first.
                    s = &d->ring[d->head];
                    d->head = (d->head + 1) % RingSamples;
                    --d->count;
                }
                std::memcpy(buffer_.data() + out, s->b.data(), s->b.size());
                put16(out, status);
                return 0;
            }

            uint32_t readMany() // fn14, IOP+0x3090 (GT4 never calls it)
            {
                const int32_t n = sword(OffValue);
                if (static_cast<uint32_t>(n - 1) >= 4u)
                    return ErrArgument;
                uint32_t last = 0;
                for (int32_t i = 0; i < n; ++i)
                {
                    const uint32_t r = read(sword(OffHandle + 4u * i), word(OffMode + 4u * i), OffData + 0x18u * i);
                    if (r & 0x80000000u)
                        last = r;
                }
                return last;
            }

            uint32_t download(int32_t handle) // fn7, IOP+0x3170 -> driver IOP+0x8148 (create)
            {
                if (handle == -1)
                    return ErrArgument;
                Device *d = bySlotOf(handle);
                if (!d)
                    return ErrStale;
                if (!d->open)
                    return ErrArgument;
                // Effect id (IOP+0x7994): first free slot from a counter shared by every
                // device; generation byte IOP+0x93b8 is never written, so always 0.
                int slot = -1;
                for (unsigned k = 0; k < EffectSlots; ++k)
                {
                    const unsigned e = (effectCounter_ + k) % EffectSlots;
                    if (!d->effects[e].used)
                    {
                        slot = static_cast<int>(e);
                        effectCounter_ = (e + 1) % EffectSlots;
                        break;
                    }
                }
                if (slot < 0)
                    return ErrNoEffect; // the id word is left as sent
                const uint32_t id = (static_cast<uint32_t>(d->handle) << 16) | (0u << 8) | static_cast<uint32_t>(slot);
                put32(OffEffectId, id); // written before the type is looked at
                const WheelEffectParams raw = params(OffData);
                EffectClass cls;
                switch (raw.type())
                {
                case 0x00: cls = EffectClass::Constant; break;
                case 0x07: cls = d->model->product == 0x046dc294u ? EffectClass::DfSpring : EffectClass::Condition; break;
                case 0x08:
                    cls = (d->model->product - 0x046dc294u) < 2u ? EffectClass::DfDamper : EffectClass::Condition;
                    break;
                case 0x10:
                    if (!d->dfp())
                        return ErrArgument; // friction is DFP only (IOP+0x8488)
                    cls = EffectClass::Condition;
                    break;
                default:
                    return ErrArgument; // unknown type (IOP+0x81e8 / 0x8200 -> 0x8568)
                }
                Effect &e = d->effects[slot];
                e.used = true;
                e.type = static_cast<WheelEffectType>(raw.type());
                e.cls = cls;
                e.params = clampParams(cls, raw);
                e.playing = false;
                send(*d, {WheelCommandKind::EffectCreate, static_cast<uint8_t>(slot), id, e.type, e.params, 0});
                return 0;
            }

            uint32_t update(uint32_t id) // fn13, IOP+0x3274 -> driver IOP+0x8148 (update)
            {
                if (static_cast<int32_t>(id) >> 16 == -1 || id == 0xffffffffu)
                    return ErrArgument;
                uint32_t error = 0;
                Device *d = nullptr;
                Effect *e = effectFor(id, error, d);
                if (!e)
                    return error;
                const WheelEffectParams p = clampParams(e->cls, params(OffData)); // class kept, no type check
                const bool changed = p.word != e->params.word;
                e->params = p;
                send(*d, {WheelCommandKind::EffectUpdate, static_cast<uint8_t>(id & 0x3fu), id, e->type, e->params, changed ? 1 : 0});
                return 0;
            }

            uint32_t startEffect(uint32_t id) // fn8, IOP+0x3394 -> effect IOP+0x6c
            {
                if (static_cast<int32_t>(id) >> 16 == -1)
                    return ErrArgument;
                uint32_t error = 0;
                Device *d = nullptr;
                Effect *e = effectFor(id, error, d);
                if (!e)
                    return error;
                const bool again = e->playing;
                e->playing = true;
                send(*d, {WheelCommandKind::EffectStart, static_cast<uint8_t>(id & 0x3fu), id, e->type, {}, again ? 1 : 0});
                return 0;
            }

            uint32_t stopEffect(uint32_t id) // fn9, IOP+0x3548 -> effect IOP+0x94
            {
                if (static_cast<int32_t>(id) >> 16 == -1)
                    return ErrArgument;
                uint32_t error = 0;
                Device *d = nullptr;
                Effect *e = effectFor(id, error, d);
                if (!e)
                    return error;
                e->playing = false;
                send(*d, {WheelCommandKind::EffectStop, static_cast<uint8_t>(id & 0x3fu), id, e->type, {}, 0});
                return 0;
            }

            uint32_t unloadEffect(uint32_t id) // fn10, IOP+0x36fc -> driver IOP+0x75c4
            {
                if (static_cast<int32_t>(id) >> 16 == -1)
                    return ErrArgument;
                uint32_t error = 0;
                Device *d = nullptr;
                Effect *e = effectFor(id, error, d);
                if (!e)
                    return error;
                const WheelEffectType type = e->type;
                *e = {};
                send(*d, {WheelCommandKind::EffectUnload, static_cast<uint8_t>(id & 0x3fu), id, type, {}, 0});
                return 0;
            }

            uint32_t effectsMany(bool start) // fn15 IOP+0x349c / fn16 IOP+0x3650: last error wins
            {
                const int32_t n = sword(OffValue);
                if (static_cast<uint32_t>(n - 1) >= 16u)
                    return ErrArgument;
                uint32_t last = 0;
                for (int32_t i = 0; i < n; ++i)
                {
                    const uint32_t id = word(OffEffectId + 4u * i);
                    const uint32_t r = start ? startEffect(id) : stopEffect(id);
                    if (r & 0x80000000u)
                        last = r;
                }
                return last;
            }

            uint32_t saveCalibration() // fn17, IOP+0x3804 (GT4 never calls it)
            {
                std::memset(buffer_.data() + OffData, 0, CalibrationBytes);
                uint8_t *r = buffer_.data() + OffData;
                for (auto &slot : slots_)
                {
                    if (!slot)
                        continue;
                    std::memcpy(r, slot->info.data() + 0x0c, 8);
                    std::memcpy(r + 8, &slot->model->product, 4);
                    for (unsigned a = 0; a < 8; ++a)
                    {
                        std::memcpy(r + 0x0c + 2 * a, &slot->calMin[a], 2);
                        std::memcpy(r + 0x1c + 2 * a, &slot->calMax[a], 2);
                    }
                    r += 0x2c;
                }
                return 0;
            }

            uint32_t restoreCalibration() // fn18, IOP+0x3ba0 (only after a fn17; never in GT4)
            {
                std::memcpy(calibration_.data(), buffer_.data() + OffData, CalibrationBytes);
                for (auto &slot : slots_)
                    if (slot)
                        applyCalibration(*slot);
                return 0;
            }

            // ---------------------------------------------------------------- logging
            std::string describeArgs(uint32_t fn) const
            {
                switch (fn)
                {
                case 1: case 2: return " index " + std::to_string(sword(OffIndex));
                case 3: return " handle " + hex(word(OffHandle));
                case 4: return " handle " + hex(word(OffHandle)) + " prop " + hex(word(OffProperty));
                case 5: return " handle " + hex(word(OffHandle)) + " prop " + hex(word(OffProperty)) + " value " + std::to_string(sword(OffValue));
                case 7: case 13:
                {
                    const WheelEffectParams p = params(OffData);
                    std::string s = (fn == 7 ? " handle " + hex(word(OffHandle)) : " id " + hex(word(OffEffectId))) + " type " + hex(p.type()) + " params";
                    for (unsigned i = 1; i < 9; ++i)
                        s += " " + std::to_string(p.word[i]);
                    return s;
                }
                case 8: case 9: case 10: return " id " + hex(word(OffEffectId));
                case 12: return " status area " + hex(word(OffValue));
                case 15: case 16:
                {
                    std::string s = " n " + std::to_string(sword(OffValue));
                    const int32_t n = std::min<int32_t>(std::max<int32_t>(sword(OffValue), 0), 16);
                    for (int32_t i = 0; i < n; ++i)
                        s += " " + hex(word(OffEffectId + 4u * i));
                    return s;
                }
                default: return {};
                }
            }

            std::string describeOutputs(uint32_t fn, uint32_t result) const
            {
                if (result & 0x80000000u)
                    return fn == 2 && result == ErrAlreadyOpen ? " handle " + hex(word(OffHandle)) : std::string();
                switch (fn)
                {
                case 1: return " info class " + hex(word(OffData)) + " port " + std::to_string(buffer_[OffData + 0x0c]) + " handle " + hex(word(OffData + 0x14));
                case 2: return " handle " + hex(word(OffHandle));
                case 4: return " value " + hex(word(OffValue));
                case 7: return " id " + hex(word(OffEffectId));
                case 12: return " version " + hex(word(OffIndex));
                default: return {};
                }
            }

            IopHost &host_;
            std::shared_ptr<HostWheel> wheel_;
            bool verbose_ = false;
            std::mutex mutex_;
            std::array<uint8_t, BufferBytes> buffer_{};
            std::array<std::unique_ptr<Device>, Slots> slots_;
            std::array<int, HostWheel::MaxDevices> hostSlot_{};
            std::array<uint32_t, HostWheel::MaxDevices> hostPlug_{};
            std::array<uint8_t, CalibrationBytes> calibration_{}; // IOP+0x93e0
            uint32_t eeStatus_ = 0;      // IOP+0x9100
            uint32_t openCounter_ = 1;
            uint32_t slotCounter_ = 0;
            uint32_t effectCounter_ = 0;
            uint64_t vsync_ = 0;
            std::set<uint64_t> seen_;
            bool noSlotLogged_ = false;
            bool poweredOff_ = false; // set by onPowerOff(Dev9Off); never cleared
        };
    }

    std::unique_ptr<Component> createLgdev(IopHost &host)
    {
        return std::make_unique<Lgdev>(host);
    }
}
