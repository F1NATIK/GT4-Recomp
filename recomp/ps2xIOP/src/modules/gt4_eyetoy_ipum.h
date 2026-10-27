#pragma once

// GT4 EyeToy frame encoder: pictures to the 'ipum' frames EYETOY.IRX streams to
// the EE (camera bridge output, IPU-ready MPEG intra macroblocks). Pure C++, no
// OS calls. Written from GT4's own decoder (CORE 0x54c8f0, mode 1 as called by
// EyeToyPS2::update 0x1c4f08) and libeyetoy's frame read (CORE 0x58ca18); spec
// analysis/phase5/specs/p5b-usb-eyetoy.md section 4.4.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop::gt4::eyetoy
{
    // One picture in YCbCr 4:2:0, BT.601 limited range (the IPU CSC's input).
    struct YuvPicture
    {
        uint32_t width = 0, height = 0; // multiples of 16
        std::vector<uint8_t> y;         // width * height
        std::vector<uint8_t> cb, cr;    // (width / 2) * (height / 2)
    };

    // RGB24 (top-down rows, 3 bytes per pixel, width*3 bytes per row) to 4:2:0.
    void rgbToYuv420(const uint8_t *rgb, uint32_t width, uint32_t height, YuvPicture &out);

    constexpr uint32_t IpumMagic = 0x6d757069u; // 'ipum' (CORE 0x6c8490, checked by 0x58cbd4 and 0x54c968)
    constexpr size_t IpumHeaderBytes = 0x10;    // byte 0x10 follows: read and dropped by FDEC 8 (CORE 0x563298)

    struct IpumFrame
    {
        std::vector<uint8_t> bytes; // header + FDEC byte + IDEC bitstream + start code, padded to 16
        int quantiserScaleCode = 0; // quantiser_scale_code of every macroblock
        int passes = 0;             // encodes run (rate control and pack retries)
        int packRetries = 0;        // re-encodes because a qword began with 0xFFFFFFFF
        bool dcOnly = false;        // last resort when even code 31 did not fit
    };

    // Encodes `picture` as one frame of at most `maxBytes` (the slot budget,
    // framesize - 4). Rate control starts at `preferredCode` (1..31) and raises the
    // quantiser_scale_code until the frame fits. Returns false only if no
    // encoding fits or every retry broke the pack rule.
    bool encodeIpum(const YuvPicture &picture, size_t maxBytes, int preferredCode, IpumFrame &out);

    // libeyetoy's pack rule (pack=1, iopstrip=0; CORE 0x58cc30-0x58cca4): the EE drops
    // every 16-byte-aligned qword whose first word is 0xFFFFFFFF. True if `frame` has one.
    bool ipumHasPackedQword(const uint8_t *frame, size_t bytes);

    // GT4's SETIQ intra matrix (CORE 0x64c410: the MPEG default matrix, zigzag order).
    const uint8_t *ipumIntraMatrix();

    // The encoder's table B-14 code for (run, level > 0), without the sign bit;
    // false when the pair is sent with an escape. For the table cross-check test.
    bool ipumRunLevelCode(int run, int level, uint16_t &code, int &length);
}
