#pragma once

// GT4 EyeToy service: EYETOY.IRX 2.3 (Sony libeyetoy server, SID 0x80001400,
// SHA-256 f84f01f2...), loaded by CORE on demand (EyeToyManager 0x1c56b0) with
// "framesize=37 samprate=0". GT4 uses the camera for one thing: the GT shirt
// recogniser that unlocks the Nike One (nike_one_04). Spec:
// analysis/phase5/specs/p5b-usb-eyetoy.md section 4; protocol notes in
// analysis/phase5b/eyetoy/README.md.

#include "gt4_iop.h"

#include <functional>

namespace ps2x::iop::gt4
{
    namespace eyetoy
    {
        class FrameSource;
    }

    struct EyeToyOptions
    {
        // "absent" (no camera, as a console without an EyeToy), "shirt" (built-in
        // GT shirt target from the disc's template), "image:<file>" (PGM/PPM/BMP/
        // PNG), "webcam" or "webcam:<n>" (Media Foundation device n).
        std::string mode = "absent";
        bool log = false;         // GT4_EYETOY_LOG=1: RPCs, LED, per-second delivery
        bool reflect = true;      // GT4_EYETOY_REFLECT=0: no virtual LED reflection
        bool flip = false;        // GT4_EYETOY_FLIP=1: invert the mirror control for image/webcam
        int webcamDelayMs = 500;  // GT4_EYETOY_WEBCAM_DELAY_MS: streaming time before the webcam opens
        int quantiser = 2;        // GT4_EYETOY_QS: first quantiser_scale_code tried (1..31)
        std::string templatePath; // GT4_EYETOY_TEMPLATE: gtshirt.bin file instead of the disc
        // Unit tests only: a picture source used instead of the one `mode` names
        // (mode must not be "absent").
        std::function<std::unique_ptr<eyetoy::FrameSource>()> sourceOverride;
    };
    EyeToyOptions eyeToyOptionsFromEnvironment();

    class EyeToyService : public Component
    {
    public:
        // LOADFILE fn 0 for cdrom0:\IRX\EYETOY.IRX;1. `args` is the module's
        // NUL-separated argument block (GT4: "framesize=37\0samprate=0\0").
        virtual void moduleLoaded(std::string_view args) = 0;
        // LOADFILE fn 7 (stop) or fn 8 (unload) of the loaded EYETOY module.
        virtual void moduleStopped() = 0;
    };

    std::unique_ptr<EyeToyService> createEyeToy(IopHost &host, Disc &disc);
    std::unique_ptr<EyeToyService> createEyeToy(IopHost &host, Disc &disc, const EyeToyOptions &options);
}
