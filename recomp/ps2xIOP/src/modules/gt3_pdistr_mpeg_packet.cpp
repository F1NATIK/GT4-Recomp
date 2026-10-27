#include "ps2x/iop/gt3/pdistr_mpeg_packet.h"

namespace ps2x::iop::gt3 {
MpegPacket parsePdistrMpegPacket(std::span<const uint8_t> input,bool audioEnabled) {
    MpegPacket result;
    auto unsupported=[] { MpegPacket r; r.status=MpegPacketStatus::Unsupported; return r; };
    if (input.size()<4) return result;
    if (input[0] || input[1] || input[2]!=1) return unsupported();
    const auto id=input[3];
    if (id==0xb9) {
        result.status=MpegPacketStatus::Complete; result.kind=MpegPacketKind::End;
        result.consumed=4; result.advance=4096; result.output={0,0,1,0xb9,0,0}; return result;
    }
    if (id==0xba) {
        if (input.size()<5) return result;
        if ((input[4]>>6)!=1) return unsupported(); // Original rejects non-MPEG2 pack headers.
        if (input.size()<14) return result;
        const uint32_t bytes=14+(input[13]&7);
        if (input.size()<bytes) return result;
        result.status=MpegPacketStatus::Complete; result.kind=MpegPacketKind::Pack; result.consumed=bytes; return result;
    }
    if (id<0xbb) return unsupported();
    if (input.size()<6) return result;
    const uint32_t length=(uint32_t(input[4])<<8)|input[5],bytes=length+6;
    if (id==0xe0 && length>4090) return unsupported();
    if (input.size()<bytes) return result;
    result.kind=MpegPacketKind::Skip;
    if (id==0xe0) {
        result.kind=MpegPacketKind::Video; result.advance=4096;
        result.output.assign(input.begin(),input.begin()+bytes);
    } else if (id==0xbd) {
        if (length<7 || (input[6]>>6)!=2 || (input[7]&0x3f)) return unsupported();
        const auto timestamps=input[7]>>6;
        if (timestamps==1) return unsupported();
        const uint32_t header=input[8],required=timestamps==3?10:timestamps==2?5:0;
        if (header<required || header+7>length) return unsupported();
        const uint32_t payload=9+header;
        const uint32_t tag=(uint32_t(input[payload])<<24)|(uint32_t(input[payload+1])<<16)|
            (uint32_t(input[payload+2])<<8)|input[payload+3];
        if (audioEnabled && (tag==0xffa00000 || tag==0xffa10000)) {
            result.kind=MpegPacketKind::Audio; result.advance=bytes-payload-4;
            result.output.assign(input.begin()+payload+4,input.begin()+bytes);
        }
    }
    result.status=MpegPacketStatus::Complete; result.consumed=bytes; return result;
}
}
