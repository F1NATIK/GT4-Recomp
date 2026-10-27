#pragma once
#include <cstdint>
#include <span>

namespace ps2x::video {
struct MpegVideoPes {
    std::span<const uint8_t> payload;
    int64_t pts90k=-1, dts90k=-1;
};
// One exact MPEG-2 E0 PES prefix, excluding any padding in a 4096-byte PDISTR
// record. Optional fields are skipped using header length; timestamps validated.
bool parseMpegVideoPes(std::span<const uint8_t>, MpegVideoPes&);

enum class Gt3MovieRecordKind { Invalid, Video, Boundary };
struct Gt3MovieRecord {
    Gt3MovieRecordKind kind = Gt3MovieRecordKind::Invalid;
    std::span<const uint8_t> packet;
};
// One complete 4096-byte EE record produced by PDISTR. Bytes beyond the PES
// length are stale ring padding, not compressed input. The six-byte B9 record
// marks a pass boundary; the caller decides whether to finish or repeat.
Gt3MovieRecord parseGt3MovieRecord(std::span<const uint8_t>);
}
