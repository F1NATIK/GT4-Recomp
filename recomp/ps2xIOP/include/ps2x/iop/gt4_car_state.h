#pragma once

// Custom Sounds (the Full edition's "Sounds" page; analysis/design/sound-packs/README.md): each race car's engine state as
// GT4's EE keeps it, handed from the EE thread (GT4.exe's race frame hook, ps2_runtime.cpp at 0x389638) to the sound
// module (gt4_sound.cpp / gt4_sound_pack.cpp), which mixes a sound pack for the cars that have one. Development and Full
// builds only: every caller is inside #if !defined(GT4_STOCK_BUILD), and the Classic build compiles none of it.
//
// The car fields (EE addresses; analysis/design/sound-packs/README.md "Car state" has the evidence):
//   R  = a0 of the race frame update 0x389638; D = *(R+0x70); car i = D + 0x6698 + i * 0x10a0, count *(D+0xf8a0)
//   (analysis/phase5/specs/autopilot-design.md 2.1). The player car: controller *(car+0xc) is a RaceInput (vptr
//   0x686618 at +0xd0) with +0x1cc != 0 (the AI pilot's rule).
//   car+0x738 float rpm (car+0x600 is the same as rad/s); car+0x598 float the engine's throttle 0..1; car+0x58c float the
//   driver's accelerator 0..1; car+0x624 u8 gear (1 = first); the car's GENERIC_CAR code: u32 at O + 0x9b0 + i * 0x35e0,
//   O = *(R+0x6c), followed by the u64 -1 of its CarGarage record.

#include <array>
#include <cstdint>

namespace ps2x::iop::gt4::carstate
{
    inline constexpr unsigned MaxCars = 8;

    struct Car
    {
        bool valid = false;
        bool player = false;  // the car the pad drives (an active RaceInput controller)
        uint32_t code = 0;    // GENERIC_CAR spec code (table 0), e.g. 59 = ka_01
        float rpm = 0.0f;     // engine revs per minute
        float throttle = 0.0f; // the engine's throttle 0..1 (+0x598; drops to 0 during an upshift, cut by traction control)
        float pedal = 0.0f;    // the driver's accelerator 0..1 (+0x58c; the AI's or the pad's)
        int32_t gear = 0;     // +0x624: 1 = first, 2 = second, ... (as GT4 counts them)
    };

    struct Snapshot
    {
        uint64_t seq = 0;          // race frames published so far (0 = never)
        uint64_t vsync = 0;        // GT4's vsync tick at the race frame
        uint64_t soundFrame = 0;   // the sound clock (48 kHz frames rendered) at the race frame
        uint32_t spec = 0;         // R+0xcc8: 0 A-spec, 1 B-Spec, 2/3 replay
        unsigned count = 0;
        int player = -1;
        std::array<Car, MaxCars> car{};
    };

    // Sound packs are wanted (GT4_SOUND_PACK or GT4_SOUND_PACK_MAP set, or GT4_CAR_DUMP): read once.
    bool wanted();
    // EE thread, each race frame update 0x389638 (a0 = R): reads the cars from EE memory and publishes them; with
    // GT4_CAR_DUMP=<file> (test only) also appends the raw car records (see gt4_sound_pack.cpp).
    void onRaceFrame(const uint8_t *rdram, uint32_t race, uint64_t vsync);
    // The newest snapshot (seq 0 when none).
    Snapshot latest();
    // The sound module's 48 kHz clock: frames rendered so far.
    void addSoundFrames(uint32_t frames);
    uint64_t soundFrames();
}
