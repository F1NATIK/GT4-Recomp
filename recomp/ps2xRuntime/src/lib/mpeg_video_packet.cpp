#include "runtime/mpeg_video_packet.h"

namespace ps2x::video {
Gt3MovieRecord parseGt3MovieRecord(std::span<const uint8_t> bytes) {
    if (bytes.size() != 4096 || bytes[0] || bytes[1] || bytes[2] != 1) return {};
    if (bytes[3] == 0xb9)
        return !bytes[4] && !bytes[5] ? Gt3MovieRecord{Gt3MovieRecordKind::Boundary, {}} : Gt3MovieRecord{};
    const size_t size = 6u + (unsigned(bytes[4]) << 8) + bytes[5];
    if (size > bytes.size()) return {};
    const auto packet = bytes.first(size);
    MpegVideoPes pes;
    if (!parseMpegVideoPes(packet, pes) || pes.payload.empty()) return {};
    return {Gt3MovieRecordKind::Video, packet};
}
bool parseMpegVideoPes(std::span<const uint8_t> bytes,MpegVideoPes& result) {
    result={};
    if (bytes.size()<9 || bytes[0] || bytes[1] || bytes[2]!=1 || bytes[3]!=0xe0 ||
        bytes.size()!=6u+(unsigned(bytes[4])<<8)+bytes[5] || (bytes[6]>>6)!=2 ||
        (bytes[6]&0x30)) return false; // Scrambled data is not a video bitstream.
    const auto flags=bytes[7]>>6;
    const size_t header=bytes[8],required=flags==3?10:flags==2?5:0;
    if (flags==1 || header<required || 9+header>bytes.size()) return false;
    auto timestamp=[&](size_t at,unsigned prefix,int64_t& value) {
        if ((bytes[at]>>4)!=prefix || !(bytes[at]&1) || !(bytes[at+2]&1) || !(bytes[at+4]&1)) return false;
        value=(int64_t((bytes[at]>>1)&7)<<30)|(int64_t(bytes[at+1])<<22)|
              (int64_t(bytes[at+2]>>1)<<15)|(int64_t(bytes[at+3])<<7)|(bytes[at+4]>>1);
        return true;
    };
    MpegVideoPes parsed;
    if (flags>=2 && !timestamp(9,flags,parsed.pts90k)) return false;
    if (flags==3 && !timestamp(14,1,parsed.dts90k)) return false;
    parsed.payload=bytes.subspan(9+header); result=parsed; return true;
}
}
