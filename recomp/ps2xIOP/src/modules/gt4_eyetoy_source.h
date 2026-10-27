#pragma once

// Picture sources for the GT4 EyeToy service (gt4_eyetoy.cpp): the built-in GT
// shirt target rendered from GT4's own template, a still image file, and a host
// webcam (Media Foundation, gt4_eyetoy_webcam.cpp). Sources produce RGB pictures
// at the size GT4 opened; the service applies the camera controls (mirror, the
// LED reflection) and encodes them as 'ipum' frames (gt4_eyetoy_ipum.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::iop::gt4::eyetoy
{
    struct RgbImage
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgb; // RGB24, top-down rows of width * 3 bytes
    };

    class FrameSource
    {
    public:
        virtual ~FrameSource() = default;
        virtual std::string describe() const = 0;
        // Live sources (a webcam) change over time and are opened only while GT4
        // streams; still sources return one picture for ever.
        virtual bool live() const = 0;
        // True when the picture is already in the orientation GT4's recogniser
        // stores its template in, so the mirror control is not applied.
        virtual bool fixedOrientation() const { return false; }
        // Whether the camera is there at all (asked at module load; a webcam
        // source lists the capture devices without opening one).
        virtual bool available() { return true; }
        // Called when GT4 starts streaming at the opened size (cmd 3), and stop()
        // when it stops (cmd 4) or the module goes away.
        virtual bool start(uint32_t width, uint32_t height, uint32_t fps) = 0;
        virtual void stop() = 0;
        // Newest picture at the started size; `sequence` changes with the picture.
        // False while there is none yet.
        virtual bool latest(RgbImage &out, uint64_t &sequence) = 0;
    };

    // level: 0 info, 1 warning, 2 error.
    using SourceLog = std::function<void(int level, const std::string &message)>;

    // GT4.VOL eyetoy/gtshirt.bin: {'GTSH', u32 width, u32 height, u32 0} + width*height
    // 8-bit samples (224x128), loaded by CORE 0x1c2b54 into the GT shirt recogniser.
    struct ShirtTemplate
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> samples;
    };
    bool parseShirtTemplate(const uint8_t *data, size_t bytes, ShirtTemplate &out);

    // GT4.VOL compressed entry: C5 EE F7 FF, int32 -size, raw DEFLATE (RFC 1951).
    bool unpackVolEntry(const uint8_t *data, size_t bytes, std::vector<uint8_t> &out);
    bool inflateRaw(const uint8_t *data, size_t bytes, size_t expectedBytes, std::vector<uint8_t> &out);
    uint32_t crc32(const uint8_t *data, size_t bytes);

    // The built-in target: the template 1:1 in a 320x240 frame (scaled with the
    // frame width), centred on a dark shirt in front of a wall. 1:1 at 320x240 is
    // the first scale GT4's search tries (template level 3 on image level 3,
    // CORE 0x1c3c28) and keeps the image pyramid aligned with the template's.
    void renderShirtTarget(const ShirtTemplate &shirt, uint32_t width, uint32_t height, RgbImage &out);

    // Still image files: binary PGM/PPM (P5/P6), BMP (24/32-bit, uncompressed),
    // PNG (8-bit grey/RGB/RGBA, not interlaced).
    bool loadImageFile(const std::string &path, RgbImage &out, std::string &error);
    // Scales to width x height, cropping to keep the aspect ratio (box filter).
    void scaleCover(const RgbImage &in, uint32_t width, uint32_t height, RgbImage &out);

    void mirrorHorizontally(RgbImage &image);
    // Virtual LED reflection. GT4 recognises the GT shirt only when part of its
    // print lights up red while the camera LED is on and goes dark when it is off
    // (a reflective print; MGTShirt, CORE 0x1c3288 / 0x1c3d30). A PC webcam has
    // no LED GT4 can switch and a printout does not reflect, so while GT4 has
    // the LED on, bright neutral pixels in every other band of rows get the red
    // cast a reflection gives (details in the .cpp).
    void applyLedReflection(RgbImage &image);

    std::unique_ptr<FrameSource> createShirtSource(ShirtTemplate shirt);
    std::unique_ptr<FrameSource> createImageSource(RgbImage image, std::string name);
    // Media Foundation capture device `device` (0 = first). Nothing is opened
    // until start(). Returns nullptr where Media Foundation is not available.
    std::unique_ptr<FrameSource> createWebcamSource(int device, SourceLog log);
}
