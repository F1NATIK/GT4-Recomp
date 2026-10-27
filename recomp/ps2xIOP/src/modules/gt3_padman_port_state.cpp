#include "ps2x/iop/gt3/padman_port_state.h"
#include <algorithm>

namespace ps2x::iop::gt3 {
namespace {
uint32_t word(const auto& bytes,size_t offset) {
    return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8)|
        (uint32_t(bytes[offset+2])<<16)|(uint32_t(bytes[offset+3])<<24);
}
void put(auto& bytes,size_t offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8));
}
uint32_t half(const auto& bytes,size_t offset) { return uint32_t(bytes[offset])|(uint32_t(bytes[offset+1])<<8); }
void put16(auto& bytes,size_t offset,uint32_t value) { bytes[offset]=uint8_t(value); bytes[offset+1]=uint8_t(value>>8); }
// Port structure fields (module 7d60 + port*0x720 + slot*0x1c8).
constexpr size_t Count=0xc0,Id=0xc1,Model=0xc2,Modes=0xc3,CurrentMode=0xc4,Actuators=0xc5,Combos=0xc6,Fresh=0xc7,
    ActTable=0xc8,CombTable=0xd8,ModeTable=0xe8,PressMask=0xf0,ButtonMask=0xf4,Vref=0xf8,PressMode=0x106,
    Data=0x148,Valid=0x168,MainMode=0x16a,MainLock=0x16b,ActDirect=0x16c,ActSize=0x174,Align=0x176,
    State=0x17c,RequestState=0x17e,Errors=0x188,WorkerMode=0x1b0,Requested=0x1b4;
// Transfer parameters by device type id>>4 (57a8/5840/58d4 and the type
// table's functions): id 0 is the discovery probe, which uses the 41 length.
uint32_t exchangeSize(uint8_t id) { return 3u+2u*((id?id:0x41u)&0xfu); }
uint32_t control1(uint8_t id,uint8_t model) { return (id>>4)==7 && (model&2)?0xff600a0au:0xffc00505u; }
uint32_t control2(uint8_t id) { return id?0x20014u:0x2000au; }
// Discovery 0dd0 accepts registered device types; this model defines basic
// digital 4x, analog 7x and configuration fx.
bool knownType(uint8_t id) { const auto type=id>>4; return type==4 || type==7 || type==0xf; }
}
void PadmanPortState::reset() { *this=PadmanPortState{}; }
bool PadmanPortState::open(uint32_t eeDoubleBuffer) {
    if (worker_!=Worker::Closed || !eeDoubleBuffer || (eeDoubleBuffer&63u) || eeDoubleBuffer>0xffffff00u) return false;
    reset(); worker_=Worker::Read; op_=Op::Poll;
    put(port_,0x184,eeDoubleBuffer);
    port_[Fresh]=1; port_[State]=5; port_[WorkerMode]=1;
    build({},nullptr);
    return true;
}
bool PadmanPortState::request(PadmanRequest& result) const {
    if (worker_==Worker::Closed) return false;
    result=pending_; return true;
}
void PadmanPortState::beginDetection() {
    // Original discovery worker2544's pre-wait initialization. Publication
    // of the frame that starts this worker occurs before this prefix runs.
    for (size_t at:{Id,Count,Model,Modes,Actuators,Combos}) port_[at]=0;
    put16(port_,Valid,0); put16(port_,ActSize,0); put(port_,Errors,0);
    std::fill_n(port_.begin()+PressMask,8,0);
    port_[Align]=0; port_[Align+1]=1;
    std::fill_n(port_.begin()+Align+2,4,0xff);
    std::fill_n(port_.begin()+ActDirect,6,0);
    worker_=Worker::Detect; op_=Op::Probe; attempts_=index_=0;
}
void PadmanPortState::beginWorker(Worker worker) {
    put16(port_,Valid,0); put16(port_,State,5);
    worker_=worker; op_=Op::EnterConfig; attempts_=index_=0;
}
// Every worker leaves through ExitThread with worker mode 1; the read worker,
// which never exits, then builds the next poll.
void PadmanPortState::exitWorker(size_t failField) {
    if (failField==RequestState) put16(port_,RequestState,1);
    else if (failField==Id) port_[Id]=0;
    put(port_,WorkerMode,1);
    worker_=Worker::Read; op_=Op::Poll; attempts_=index_=0;
}
// Retry bookkeeping of every worker step: ten failed exchanges end the step.
bool PadmanPortState::retry(bool succeeded) {
    if (succeeded) { attempts_=0; return false; }
    return ++attempts_==10;
}
void PadmanPortState::nextDetectTable() {
    // Actuator, combination and mode tables (27a0..2908), then the motor map.
    constexpr std::array<std::pair<Op,size_t>,3> tables{{{Op::ActInfo,Actuators},{Op::CombInfo,Combos},{Op::ModeInfo,Modes}}};
    size_t table=0;
    if (op_==Op::ActInfo || op_==Op::CombInfo || op_==Op::ModeInfo) {
        while (tables[table].first!=op_) ++table;
        if (++index_<port_[tables[table].second]) return;
        ++table;
    }
    for (;table<tables.size();++table)
        if (port_[tables[table].second]) { op_=tables[table].first; index_=0; return; }
    op_=Op::Align;
}
void PadmanPortState::afterDetectModel() {
    // A newly connected pad left in analog mode is first returned to digital.
    if (port_[Fresh]==1 && port_[CurrentMode]) { port_[MainMode]=port_[MainLock]=0; op_=Op::SetMainMode; return; }
    nextDetectTable();
}
void PadmanPortState::handleDetect(bool ok,std::span<const uint8_t> r) {
    const bool config=ok && r[1]==0xf3;
    switch (op_) {
    case Op::Probe: {
        // 0dd0 stores a registered identity; 2544 then also rejects ff.
        const bool probed=ok && r[1] && knownType(r[1]);
        if (probed) port_[Id]=r[1];
        if (probed && r[1]!=0xff) {
            id_=r[1]; port_[Id]=0; put16(port_,State,5);
            if (port_[Fresh]) std::fill_n(port_.begin()+Align,6,0xff);
            op_=Op::EnterConfig; attempts_=0;
        } else { put16(port_,State,0); port_[Fresh]=1; }
        break;
    }
    case Op::EnterConfig:
        // f80 needs only a completed exchange. Ten failures: basic pad.
        if (ok) { port_[Count]=2; op_=Op::Model; attempts_=0; }
        else if (retry(false)) { port_[Count]=1; port_[Id]=id_; exitWorker(); }
        break;
    case Op::Model:
        if (config) {
            port_[Model]=r[3]; port_[Modes]=std::min<uint8_t>(r[4],4); port_[CurrentMode]=r[5];
            port_[Actuators]=std::min<uint8_t>(r[6],4); port_[Combos]=std::min<uint8_t>(r[7],4);
            attempts_=0; afterDetectModel();
        } else if (retry(false)) exitWorker(Id);
        break;
    case Op::SetMainMode:
        if (config) { exitWorker(Id); port_[Fresh]=0; }
        else if (retry(false)) exitWorker(RequestState);
        break;
    case Op::ActInfo: case Op::CombInfo: case Op::ModeInfo:
        if (config) {
            if (op_==Op::ModeInfo) { port_[ModeTable+2*index_]=r[6]; port_[ModeTable+2*index_+1]=r[5]; }
            else std::copy_n(r.begin()+5,4,port_.begin()+(op_==Op::ActInfo?ActTable:CombTable)+4*index_);
            attempts_=0; nextDetectTable();
        } else if (retry(false)) exitWorker(Id);
        break;
    case Op::Align:
        if (config) { attempts_=0; op_=port_[Count]==2 && (port_[Model]&2)?Op::MaskQuery:Op::ExitConfig; }
        else if (retry(false)) exitWorker(RequestState);
        break;
    case Op::MaskQuery:
        if (ok) {
            for (size_t i=0;i<4;++i) port_[ButtonMask+i]=r[8]==0x5a?r[3+i]:0;
            attempts_=0; op_=Op::ExitConfig;
        } else if (retry(false)) exitWorker(Id);
        break;
    case Op::ExitConfig:
        if (ok) { attempts_=0; op_=Op::Verify; }
        else if (retry(false)) exitWorker(Id);
        break;
    case Op::Verify:
        if (ok && r[1] && knownType(r[1])) { put16(port_,State,5); exitWorker(); port_[Id]=r[1]; port_[Fresh]=0; }
        else if (retry(false)) exitWorker(Id);
        break;
    default: break;
    }
}
void PadmanPortState::handleWorker(bool ok,std::span<const uint8_t> r) {
    // Workers 2a94/2bcc/2d04/2ea8: f80 and 10ec need a completed exchange,
    // the configuration commands also the f3 identity.
    const bool done=op_==Op::EnterConfig || op_==Op::ExitConfig?ok:ok && r[1]==0xf3;
    if (!done) { if (retry(false)) exitWorker(RequestState); return; }
    attempts_=0;
    switch (op_) {
    case Op::EnterConfig:
        op_=worker_==Worker::MainMode?Op::SetMainMode:worker_==Worker::ActAlign?Op::Align:
            worker_==Worker::PressMode?Op::SetMask:Op::SetVref;
        index_=0; break;
    case Op::SetMask: op_=Op::SetVref; index_=0; break;
    case Op::SetVref: if (++index_==12) op_=Op::ExitConfig; break;
    case Op::ExitConfig: if (worker_==Worker::PressMode) put16(port_,PressMode,1); exitWorker(); break;
    default: op_=Op::ExitConfig; break;
    }
}
bool PadmanPortState::handle(bool ok,std::span<const uint8_t> r) {
    if (worker_==Worker::Detect) { handleDetect(ok,r); return true; }
    if (worker_!=Worker::Read) { handleWorker(ok,r); return true; }
    // Read worker 23b4 after bf4.
    auto detect=[&] { put(port_,WorkerMode,2); worker_=Worker::Detect; startDetection_=true; };
    if (!ok) {
        put16(port_,Valid,0); put16(port_,State,7); put(port_,Errors,word(port_,Errors)+1u);
        if (++readErrors_>=10) { readErrors_=0; detect(); }
        return true;
    }
    readErrors_=0;
    if (r[2]!=0x5a || r[1]!=port_[Id] || r[1]==0xf3) { put16(port_,Valid,0); put16(port_,State,5); detect(); return true; }
    std::fill_n(port_.begin()+Data,32,0);
    std::copy(r.begin(),r.end(),port_.begin()+Data);
    put16(port_,Valid,1); put16(port_,State,port_[Count]==1?2:6);
    if (half(port_,RequestState)==2 && !word(port_,Requested)) put16(port_,RequestState,0);
    return true;
}
void PadmanPortState::build(std::span<const uint8_t> slots,int* consumedSlot) {
    PadmanRequest next; next.bytes[0]=1;
    auto shape=[&](uint8_t id) {
        next.size=exchangeSize(id); next.portControl1=control1(id,port_[Model]); next.portControl2=control2(id);
    };
    auto command=[&](uint8_t op,std::initializer_list<uint8_t> tail) {
        next.bytes[1]=op; std::copy(tail.begin(),tail.end(),next.bytes.begin()+3);
    };
    const uint8_t i=index_;
    constexpr uint8_t z=0x5a;
    switch (op_) {
    case Op::Poll:
        // 0x14 applies the newest pending EE actuator slot through 4040; bf4
        // then places each aligned actuator value at poll byte 3+i.
        if (slots.size()==PadmanActSlotsBytes) {
            const size_t slot=word(slots,0)<word(slots,0x20)?1:0;
            if (word(slots,slot*0x20+4)) {
                setActDirect(slots.subspan(slot*0x20+0xc).first<6>());
                if (consumedSlot) *consumedSlot=int(slot);
            }
        }
        shape(port_[Id]); next.bytes[1]=0x42;
        for (size_t at=0;at<half(port_,ActSize) && at<6;++at)
            next.bytes[3+at]=port_[Align+at]!=0xff?port_[ActDirect+at]:0;
        break;
    case Op::Probe: case Op::Verify: shape(0); next.bytes[1]=0x42; break;
    case Op::EnterConfig: shape(worker_==Worker::Detect?id_:port_[Id]); command(0x43,{1}); break;
    default:
        shape(0xf3);
        switch (op_) {
        case Op::Model: command(0x45,{z,z,z,z,z,z}); break;
        case Op::ActInfo: command(0x46,{i,z,z,z,z,z}); break;
        case Op::CombInfo: command(0x47,{i,z,z,z,z,z}); break;
        case Op::ModeInfo: command(0x4c,{i,z,z,z,z,z}); break;
        case Op::Align: command(0x4d,{}); std::copy_n(port_.begin()+Align,6,next.bytes.begin()+3); break;
        case Op::MaskQuery: command(0x41,{z,z,z,z,z,z}); break;
        case Op::ExitConfig: command(0x43,{0,z,z,z,z,z}); break;
        case Op::SetMainMode: command(0x44,{port_[MainMode],port_[MainLock]}); break;
        case Op::SetMask: command(0x4f,{port_[PressMask],port_[PressMask+1],port_[PressMask+2]}); break;
        case Op::SetVref: command(0x40,{i,port_[Vref+i]}); break;
        default: break;
        }
    }
    pending_=next;
}
bool PadmanPortState::complete(uint32_t status,std::span<const uint8_t> response,PadmanPublication& publication,
                               std::span<const uint8_t> actSlots,int* consumedSlot) {
    if (worker_==Worker::Closed || response.size()!=pending_.size ||
        (!actSlots.empty() && actSlots.size()!=PadmanActSlotsBytes)) return false;
    // The completion profiles the native devices produce: a reply, or no
    // device (all ff). Other bus status semantics remain unsupported.
    const bool ok=status==0x1100;
    const bool absent=status==0x1d100 && std::all_of(response.begin(),response.end(),[](uint8_t b) { return b==0xff; });
    if (!ok && !absent) return false;
    auto next=*this;
    if (!next.handle(ok,response)) return false;
    PadmanPublication staged;
    if (!stagePadmanPublication(next.port_,staged)) return false;
    next.port_=staged.port;
    // Main thread 98c after publication: a started discovery worker's prefix,
    // then a worker requested by RPC starts only from worker mode 1.
    if (next.startDetection_) { next.startDetection_=false; next.beginDetection(); }
    if (const auto requested=word(next.port_,Requested)) {
        put(next.port_,Requested,0);
        if (word(next.port_,WorkerMode)==1 && requested>=4 && requested<=7) {
            put16(next.port_,RequestState,2); put(next.port_,WorkerMode,requested);
            next.beginWorker(Worker(uint8_t(Worker::MainMode)+(requested-4)));
        } else put16(next.port_,RequestState,1);
    }
    if (word(next.port_,WorkerMode)!=1) put16(next.port_,Valid,0);
    int consumed=-1;
    next.build(actSlots,&consumed);
    *this=next; publication=staged;
    if (consumedSlot) *consumedSlot=consumed;
    return true;
}
int32_t PadmanPortState::infoAct(int32_t actuator,int32_t term) const {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2) return 0;
    if (actuator==-1) return port_[Actuators];
    if (actuator<0 || actuator>=port_[Actuators] || term<1 || term>4) return -1;
    return port_[ActTable+4*actuator+term-1];
}
int32_t PadmanPortState::infoComb(int32_t list,int32_t offset) const {
    if (word(port_,WorkerMode)!=1) return 0;
    if (port_[Count]<2) return -1;
    if (list==-1) return port_[Combos];
    if (list<0 || list>=port_[Combos] || offset<-1 || offset>2) return -1;
    return port_[CombTable+4*list+offset+1];
}
int32_t PadmanPortState::infoMode(int32_t term,int32_t offset) const {
    if (word(port_,WorkerMode)!=1 || half(port_,RequestState)==2) return 0;
    switch (term) {
    case 1: return port_[Id]==0xf3?0:port_[Id]>>4;
    case 2: return port_[Count]==1?0:int32_t(half(port_,ModeTable+2*port_[CurrentMode]));
    case 3: return port_[Count]==1?0:port_[CurrentMode];
    case 4:
        if (port_[Count]==1) return 0;
        if (offset==-1) return port_[Modes];
        return offset>=0 && offset<port_[Modes]?int32_t(half(port_,ModeTable+2*offset)):0;
    default: return -1;
    }
}
int32_t PadmanPortState::setMainMode(uint32_t mode,uint32_t lock) {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2 || int32_t(mode)>=port_[Modes] ||
        half(port_,RequestState)==2) return 0;
    port_[MainMode]=uint8_t(mode); port_[MainLock]=uint8_t(lock);
    put(port_,Requested,4); put16(port_,RequestState,2);
    return 1;
}
int32_t PadmanPortState::setActDirect(std::span<const uint8_t,6> values) {
    // 2288's actuator power budget (61 units over all open ports) cannot
    // trigger for one DualShock 2 (10+20 units); it is not modeled.
    if (word(port_,WorkerMode)!=1) return 0;
    std::copy(values.begin(),values.end(),port_.begin()+ActDirect);
    put16(port_,ActSize,6);
    return 1;
}
int32_t PadmanPortState::setActAlign(std::span<const uint8_t,6> align) {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2 || half(port_,RequestState)==2) return 0;
    std::copy(align.begin(),align.end(),port_.begin()+Align);
    put16(port_,RequestState,2); put(port_,Requested,5);
    return 1;
}
uint32_t PadmanPortState::buttonMask() const {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2 || port_[Model]<2) return 0;
    return word(port_,ButtonMask);
}
int32_t PadmanPortState::setButtonInfo(uint32_t mask) {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2 || !(port_[Model]&2) || half(port_,RequestState)==2) return 0;
    put(port_,PressMask,(mask<<6)|0x3fu);
    std::fill_n(port_.begin()+Vref,12,2);
    put(port_,Requested,6); put16(port_,RequestState,2);
    return 1;
}
int32_t PadmanPortState::setVrefParam(std::span<const uint8_t,12> values) {
    if (word(port_,WorkerMode)!=1 || port_[Count]<2 || port_[Model]<2 || half(port_,RequestState)==2) return 0;
    std::copy(values.begin(),values.end(),port_.begin()+Vref);
    put(port_,Requested,7); put16(port_,RequestState,2);
    return 1;
}
}
