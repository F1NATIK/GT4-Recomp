#include "ps2x/iop/gt3/padman_publication.h"
#include <algorithm>
#include <limits>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(std::span<const uint8_t> bytes,size_t offset) {
    return uint32_t(bytes[offset]) | (uint32_t(bytes[offset+1])<<8) |
        (uint32_t(bytes[offset+2])<<16) | (uint32_t(bytes[offset+3])<<24);
}
void put(std::span<uint8_t> bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
}
bool stagePadmanPublication(std::span<const uint8_t> source,PadmanPublication& result) {
    if (source.size()!=PadmanPortBytes) return false;
    const bool valid=(uint32_t(source[0x168])|(uint32_t(source[0x169])<<8))==1;
    if (valid && (source[0xc1]>>4)!=4 && (source[0xc1]>>4)!=7) return false;
    const auto frame=word(source,0x180),eeBase=word(source,0x184);
    // Native adapter requires a complete, aligned 256-byte double buffer.
    // Original caller validates alignment; do not allow arithmetic wrap here.
    if (!eeBase || (eeBase&63u) || eeBase>std::numeric_limits<uint32_t>::max()-255u) return false;
    PadmanPublication staged;
    std::copy(source.begin(),source.end(),staged.port.begin());
    auto& out=staged.port;
    put(out,0x58,frame); put(out,0x180,frame+1u);
    put(out,0x5c,word(source,0x188));
    constexpr std::array<size_t,17> fields{
        0xc0,0xc1,0xc2,0x168,0xc3,0xc4,0xc5,0xc6,0x106,
        0x16a,0x16b,0x174,0x17c,0x17e,0x1b0,0x1b4,0x1bc};
    for (size_t i=0;i<fields.size();++i) out[0x64+i]=source[fields[i]];
    std::copy_n(source.begin()+0x16c,8,out.begin()+0x20);
    std::copy_n(source.begin()+0x176,8,out.begin()+0x28);
    std::copy_n(source.begin()+0xc8,40,out.begin()+0x30);
    std::copy_n(source.begin()+0xf0,4,out.begin()+0x75);
    std::copy_n(source.begin()+0xf4,4,out.begin()+0x79);
    if (valid) {
        out[0]=0; out[1]=source[0xc1];
        std::copy_n(source.begin()+0x14b,29,out.begin()+2);
        out[31]=source[0x14a];
        if (source[0xc1]==0x79) {
            // 390c keeps each pressure consistent with its digital button: a
            // pressed button reads at least 1, a released one 0. Protocol
            // order: right left up down triangle circle cross square L1 R1 L2 R2.
            constexpr std::array<uint16_t,12> buttons{0x20,0x80,0x10,0x40,0x1000,0x2000,0x4000,0x8000,
                                                      0x400,0x800,0x100,0x200};
            const auto pressed=uint16_t(~(out[2]|(out[3]<<8)));
            for (size_t i=0;i<buttons.size();++i) {
                auto& pressure=out[8+i];
                if (pressed&buttons[i]) { if (!pressure) pressure=1; }
                else pressure=0;
            }
        }
    }
    put(out,0x60,valid?32:0);
    staged.eeTarget=eeBase+(frame&1u)*PadmanFrameBytes;
    result=staged;
    return true;
}
bool commitPadmanPublication(IopHost& host,uint32_t iopPort) {
    std::array<uint8_t,PadmanPortBytes> source{};
    if (!host.readIopMemory(iopPort,source.data(),source.size())) return false;
    PadmanPublication staged;
    if (!stagePadmanPublication(source,staged)) return false;
    uint32_t eeBase=0,eeEnd=0;
    const auto address=word(source,0x184);
    // normalizeGuestAddress also recognizes allocated IOP addresses. Require
    // the full double buffer to resolve into this game's 32MiB EE RAM.
    if (!host.normalizeGuestAddress(address,eeBase) ||
        !host.normalizeGuestAddress(address+255u,eeEnd) ||
        eeBase>0x02000000u-256u || eeEnd!=eeBase+255u) return false;
    const std::array<IopMemoryWrite,2> writes{{
        {iopPort,staged.port},
        {staged.eeTarget,std::span<const uint8_t>(staged.port.data(),PadmanFrameBytes)}}};
    return host.commitIopAndEeMemory(writes);
}
}
