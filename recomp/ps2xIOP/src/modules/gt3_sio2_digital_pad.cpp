#include "ps2x/iop/gt3/sio2_digital_pad.h"
#include <bit>
#include <memory>
namespace ps2x::iop::gt3 {
namespace {
class DigitalPad final : public Sio2Device {
    IopHost& host_;
public:
    DigitalPad(IopHost& host,bool dualShock2):host_(host),dualShock2_(dualShock2) {}
    bool exchange(const Sio2Transfer& request,Sio2Completion& reply) override {
        // Supplied MCMAN 0x25c4 selects index0 through 0x22d0.
        // Table 0x10b50 gives four bytes: SEND3=0x00100470|card-port(2/3).
        // An absent virtual card completes the bus exchange, never a card reset.
        const bool cardReset=request.input==std::vector<uint8_t>({0x81,0x11,0,0}) && request.outputSize==4 &&
            (request.sendControl[0]&~1u)==0x00100472u;
        // Original legacy detection 0xa358: 81/52/00, SEND3=0x000c0342/43.
        const bool legacyProbe=request.input==std::vector<uint8_t>({0x81,0x52,0}) && request.outputSize==3 &&
            (request.sendControl[0]&~1u)==0x000c0342u;
        if ((cardReset || legacyProbe) && !request.inputDma.address &&
            !request.outputDma.address && request.dmaInput.empty() && !request.dmaOutputSize) {
            for (size_t i=1;i<16;++i) if (request.sendControl[i]) return false;
            Sio2Completion absent; absent.receiveStatus={0x1d100,0xf,0};
            absent.output.assign(request.outputSize,0xff); reply=std::move(absent); return true;
        }
        // Original PS2 MCMAN uses a padded 0x24-word DMA block, not FIFO.
        // Index0=11/4, index9=28/5, index17=f3/5 in table0x10b50.
        if (request.input.empty() && !request.outputSize && request.inputDma.address &&
            request.outputDma.address && !(request.inputDma.address&3u) && !(request.outputDma.address&3u) &&
            (request.inputDma.blockSize&0xffffu)==0x24 && (request.inputDma.blockCount&0xffffu)==1 &&
            (request.outputDma.blockSize&0xffffu)==0x24 && (request.outputDma.blockCount&0xffffu)==1 &&
            request.dmaInput.size()==144 && request.dmaOutputSize==144 && request.dmaInput[0]==0x81) {
            const auto opcode=request.dmaInput[1];
            const uint32_t length=opcode==0x11?4:(opcode==0x28 || opcode==0xf3)?5:0;
            if (!length || (request.sendControl[0]&~1u)!=((length<<18)|(length<<8)|0x72)) return false;
            for (size_t i=1;i<16;++i) if (request.sendControl[i]) return false;
            Sio2Completion absent; absent.receiveStatus={0x1d100,0xf,0};
            absent.dmaOutput.assign(144,0xff); reply=std::move(absent); return true;
        }
        // Supplied MTAPMAN 0x170 probes pad slots (0x12) or card slots (0x13).
        // Neither physical port has a virtual multitap attached.
        if (request.input.size()==6 && request.input[0]==0x21 &&
            (request.input[1]==0x12 || request.input[1]==0x13) && request.outputSize==6 &&
            (request.sendControl[0]&~1u)==0x00180640u && !request.inputDma.address &&
            !request.outputDma.address && request.dmaInput.empty() && !request.dmaOutputSize) {
            for (size_t i=2;i<6;++i) if (request.input[i]) return false;
            for (size_t i=1;i<16;++i) if (request.sendControl[i]) return false;
            Sio2Completion absent; absent.receiveStatus={0x1d100,0xf,0}; absent.output.assign(6,0xff);
            reply=std::move(absent); return true;
        }
        // PADMAN exchanges (supplied 0x5718.. builders): 01+command, 5/9/21
        // FIFO bytes, SEND3=(n<<18)|(n<<8)|0x40|port, no DMA.
        const auto size=request.input.size();
        if ((size!=5 && size!=9 && size!=21) || request.outputSize!=size || request.input[0]!=1 ||
            request.inputDma.address || request.outputDma.address || !request.dmaInput.empty() || request.dmaOutputSize ||
            (request.sendControl[0]&~1u)!=((uint32_t(size)<<18)|(uint32_t(size)<<8)|0x40u)) return false;
        for (size_t i=1;i<16;++i) if (request.sendControl[i]) return false;
        // RECV1 error bits identify the packet index, not physical port. The
        // virtual endpoint exists only at port0 (original PADMAN0x44f0/0x4824).
        std::vector<uint8_t> output;
        if (!(request.sendControl[0]&1u) && !respond(request.input,output)) return false;
        Sio2Completion staged;
        if (output.empty()) { staged.receiveStatus={0x1d100,0xf,0}; staged.output.assign(size,0xff); }
        else {
            // A controller answers only its own packet length; the bus then reads ff.
            output.resize(size,0xff);
            staged.receiveStatus={0x1100,0xf,0}; staged.output=std::move(output);
        }
        reply=std::move(staged); return true;
    }
private:
    const bool dualShock2_;
    uint8_t mode_=0x41;
    bool config_=false;
    std::array<uint8_t,6> motorMap_{0xff,0xff,0xff,0xff,0xff,0xff};
    bool small_=false; uint8_t large_=0;
    // Digital bytes, then sticks rx ry lx ly (analog 73/79), then twelve
    // pressures in protocol order (79): right left up down triangle circle
    // cross square L1 R1 L2 R2.
    bool payload(uint8_t mode,std::vector<uint8_t>& out) const {
        std::array<uint8_t,32> snapshot{};
        if (!host_.readPadSnapshot(snapshot)) return false;
        out.insert(out.end(),snapshot.begin()+2,snapshot.begin()+4);
        if ((mode>>4)==7) out.insert(out.end(),snapshot.begin()+4,snapshot.begin()+8);
        if (mode==0x79) {
            // Snapshots without pressures (id 73) press digitally: 0 or ff.
            constexpr std::array<uint16_t,12> masks{0x20,0x80,0x10,0x40,0x1000,0x2000,0x4000,0x8000,0x400,0x800,0x100,0x200};
            const uint16_t buttons=uint16_t(snapshot[2]|(snapshot[3]<<8));
            for (size_t i=0;i<12;++i)
                out.push_back(snapshot[1]==0x79?snapshot[8+i]:(buttons&masks[i])?0:0xff);
        }
        return true;
    }
    void drive(const std::vector<uint8_t>& command) {
        // Motor map (4d): poll byte 3+i drives motor map[i]: 0 small, 1 large.
        bool small=false; uint8_t large=0;
        for (size_t i=0;i<motorMap_.size() && 3+i<command.size();++i) {
            if (motorMap_[i]==0) small=command[3+i]&1;
            else if (motorMap_[i]==1) large=command[3+i];
        }
        if (small!=small_ || large!=large_) { small_=small; large_=large; host_.setPadActuators(small,large); }
    }
    bool respond(const std::vector<uint8_t>& command,std::vector<uint8_t>& out) {
        const uint8_t op=command[1];
        if (!dualShock2_) {
            // Basic digital pad: polls only; no configuration mode.
            if (command.size()!=5) return false;
            if (op==0x43 && command[3]==1) return true; // Unanswered: no-device status.
            if (op!=0x42) return false;
            out={0xff,0x41,0x5a};
            return payload(0x41,out);
        }
        // Protocol-level DualShock 2, as gt3-tools/audit_padman_ds2.py.
        if (!config_) {
            if (op!=0x42 && op!=0x43) return true; // Needs configuration mode: unanswered.
            out={0xff,mode_,0x5a};
            if (!payload(mode_,out)) return false;
            if (op==0x42) drive(command);
            else if (command[3]==1) config_=true;
            return true;
        }
        const uint8_t arg=command[3];
        std::array<uint8_t,6> data{};
        switch (op) {
        case 0x42: {
            std::vector<uint8_t> values;
            if (!payload(0x73,values)) return false;
            std::copy_n(values.begin(),6,data.begin()); break;
        }
        case 0x43: if (!arg) config_=false; break;
        case 0x44: mode_=arg==1?0x73:0x41; break;
        case 0x45: data={0x03,0x02,uint8_t(mode_!=0x41),0x02,0x01,0x00}; break;
        case 0x46: data=arg?std::array<uint8_t,6>{0,0,1,1,1,0x14}:std::array<uint8_t,6>{0,0,1,2,0,0x0a}; break;
        case 0x47: data={0,0,2,0,1,0}; break;
        case 0x4c: data={0,0,0,uint8_t(arg?7:4),0,0}; break;
        case 0x4d: data=motorMap_; std::copy_n(command.begin()+3,6,motorMap_.begin()); break;
        case 0x4f: {
            // Response mask: one bit per data byte after the digital bytes; the
            // identity's low nibble counts the halfwords (3f: 73, 3ffff: 79).
            const uint32_t mask=(command[3]|(command[4]<<8)|(command[5]<<16))&0x3ffffu;
            const auto count=std::popcount(mask);
            mode_=count>2?uint8_t(0x70|((count+1)/2)):0x41;
            data={0,0,0,0,0,0x5a}; break;
        }
        // Which buttons have pressure: a DualShock 2 answers FF FF 03 (all 18)
        // in analog mode, before any pressure mode (as PCSX2's pad does); the
        // pad library's scePadInfoPressMode is true only for that mask. Asked
        // only in pressure mode before, so GT3 never enabled its analog
        // buttons (2026-09-30).
        case 0x41: if (mode_!=0x41) data={0xff,0xff,0x03,0,0,0x5a}; break;
        case 0x40: data={0,0,2,0,0,0x5a}; break;
        default: return true;
        }
        out={0xff,0xf3,0x5a};
        out.insert(out.end(),data.begin(),data.end());
        return true;
    }
};
void put(std::vector<uint8_t>& bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(8*i));
}
}
std::shared_ptr<Sio2Device> createSio2DigitalPad(IopHost& host) { return std::make_shared<DigitalPad>(host,false); }
std::shared_ptr<Sio2Device> createSio2DualShock2(IopHost& host) { return std::make_shared<DigitalPad>(host,true); }
void Sio2DigitalPadClient::reset() { if (storage_) host_.freeIopMemory(storage_); storage_=0; }
bool Sio2DigitalPadClient::poll(std::array<uint8_t,4>& packet) {
    auto* registry=host_.nativeLibraries(); if (!registry) return false;
    const auto begin=registry->bind("sio2man",0x203,23),transfer=registry->bind("sio2man",0x203,25),end=registry->bind("sio2man",0x203,52);
    if (!begin.registration || !transfer.registration || !end.registration) return false;
    if (!storage_) storage_=host_.allocateIopMemory(0xa8);
    if (!storage_) return false;
    std::vector<uint8_t> bytes(0xa8);
    put(bytes,4,0xffc00505); put(bytes,0x14,0x20014); put(bytes,0x28,0x140540);
    put(bytes,0x6c,5); put(bytes,0x70,5); put(bytes,0x74,storage_+0x94); put(bytes,0x78,storage_+0xa0);
    bytes[0x94]=1; bytes[0x95]=0x42;
    const std::array<IopMemoryWrite,1> writes{{{storage_,bytes}}};
    if (!host_.writeIopMemoryBatch(writes) || !registry->invoke(begin,{}).handled) return false;
    struct EndSession { NativeLibraryRegistry& registry; NativeImportBinding binding;
        ~EndSession() { registry.invoke(binding,{}); } } finish{*registry,end};
    const auto result=registry->invoke(transfer,{storage_,0,0,0});
    if (!result.handled || result.value!=1) return false;
    std::array<uint8_t,5> response{}; uint32_t status=0;
    if (!host_.readGuest(storage_,&status,4) || (status&0xf000)!=0x1000 ||
        !host_.readGuest(storage_+0xa0,response.data(),5) || response[0]!=0xff || response[1]!=0x41 || response[2]!=0x5a) return false;
    packet={0,response[1],response[3],response[4]}; return true;
}
}
