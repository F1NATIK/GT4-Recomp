#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace ps2x::iop::gt3 {
enum class MpegPacketStatus { NeedInput, Complete, Unsupported };
enum class MpegPacketKind { Pack, Skip, Video, Audio, End };
struct MpegPacket {
    MpegPacketStatus status=MpegPacketStatus::NeedInput;
    MpegPacketKind kind=MpegPacketKind::Skip;
    uint32_t consumed=0;
    uint32_t advance=0;
    // Write these bytes, then advance the destination cursor by advance.
    // Video/end records retain prior ring bytes after this prefix, NOT zeros.
    std::vector<uint8_t> output;
};
// Original PDISTR1750/1d90 packet contract, native parsing only. Caller retains
// incomplete input. No source consumption or partial output on NeedInput/error.
// Handles MPEG2 pack/system/skip packets, E0 video and baseline BD private audio
// with optional PTS/DTS. Other BD header extensions stay explicitly unsupported.
// This utility does not activate RPC1 or implement worker/ring/playback services.
MpegPacket parsePdistrMpegPacket(std::span<const uint8_t> input,bool audioEnabled);
}
