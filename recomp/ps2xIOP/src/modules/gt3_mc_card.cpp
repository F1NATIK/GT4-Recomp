#include "ps2x/iop/gt3/mc_card.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>

namespace ps2x::iop::gt3 {
namespace {
constexpr uint32_t PageSize=512,RawPage=528,PagesPerCluster=2,ClusterSize=1024,EntriesPerCluster=2,StandardPages=16384;
constexpr uint16_t ModeRwx=7,ModeFile=0x10,ModeDir=0x20,ModeClosed=0x80,ModePs2=0x400,ModeHidden=0x2000,ModeExists=0x8000;
constexpr uint32_t FatFree=0x7fffffff,FatEnd=0xffffffff,FatUsed=0x80000000;
constexpr int32_t Ok=0,Changed=-1,NoFormat=-2,Full=-3,NoEntry=-4,Denied=-5,NotEmpty=-6,Handles=-7;
constexpr uint32_t OpenRead=1,OpenWrite=2,OpenMkdir=0x40,OpenCreate=0x200,OpenTrunc=0x400;
// Directory entry (512 bytes): mode u16, length u32 @4, created @8, cluster @16,
// index in parent (".") @20, modified @24, attr @32, name[32] @64.
constexpr size_t EMode=0,ELength=4,ECreated=8,ECluster=16,EParent=20,EModified=24,EAttr=32,EName=64;

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
void wr32(uint8_t* p,uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]|(p[1]<<8)); }
void wr16(uint8_t* p,uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }

struct EccTables {
    std::array<uint8_t,256> parity{},columns{};
    EccTables() {
        constexpr std::array<uint8_t,7> masks{0x55,0x33,0x0f,0x00,0xaa,0xcc,0xf0};
        for (unsigned b=0;b<256;++b) parity[b]=uint8_t(std::popcount(b)&1);
        for (unsigned b=0;b<256;++b) {
            uint8_t mask=0;
            for (unsigned i=0;i<masks.size();++i) mask|=uint8_t(parity[b&masks[i]]<<i);
            columns[b]=mask;
        }
    }
};
const EccTables& ecc() { static const EccTables tables; return tables; }

McDateTime now() {
    // The PS2 clock and card timestamps are Japan time (UTC+9).
    const auto seconds=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())+9*3600;
    std::tm t{};
#if defined(_WIN32)
    gmtime_s(&t,&seconds);
#else
    gmtime_r(&seconds,&t);
#endif
    McDateTime d; d.sec=uint8_t(t.tm_sec); d.min=uint8_t(t.tm_min); d.hour=uint8_t(t.tm_hour);
    d.day=uint8_t(t.tm_mday); d.month=uint8_t(t.tm_mon+1); d.year=uint16_t(t.tm_year+1900);
    return d;
}
void putTime(uint8_t* p,const McDateTime& d) { p[0]=d.resv; p[1]=d.sec; p[2]=d.min; p[3]=d.hour; p[4]=d.day; p[5]=d.month; wr16(p+6,d.year); }
McDateTime getTime(const uint8_t* p) { McDateTime d; d.resv=p[0]; d.sec=p[1]; d.min=p[2]; d.hour=p[3]; d.day=p[4]; d.month=p[5]; d.year=rd16(p+6); return d; }
std::string entryName(const uint8_t* e) {
    const auto* name=reinterpret_cast<const char*>(e+EName);
    return std::string(name,strnlen(name,32));
}
// mcman wildcards: '*' any run, '?' one character.
bool matches(std::string_view pattern,std::string_view name) {
    if (pattern.empty()) return name.empty();
    if (pattern[0]=='*') {
        for (size_t i=0;i<=name.size();++i) if (matches(pattern.substr(1),name.substr(i))) return true;
        return false;
    }
    return !name.empty() && (pattern[0]=='?' || pattern[0]==name[0]) && matches(pattern.substr(1),name.substr(1));
}
std::vector<std::string> components(std::string_view cwd,std::string_view path) {
    std::vector<std::string> parts;
    auto add=[&](std::string_view text) {
        size_t start=0;
        while (start<=text.size()) {
            const auto end=std::min(text.find('/',start),text.size());
            const auto part=text.substr(start,end-start);
            if (part==".." ) { if (!parts.empty()) parts.pop_back(); }
            else if (!part.empty() && part!=".") parts.emplace_back(part);
            start=end+1;
        }
    };
    if (path.empty() || path[0]!='/') add(cwd);
    add(path);
    return parts;
}
}

std::array<uint8_t,3> mcEcc(std::span<const uint8_t,128> chunk) {
    const auto& t=ecc();
    uint8_t column=0x77,line0=0x7f,line1=0x7f;
    for (unsigned i=0;i<chunk.size();++i) {
        const auto b=chunk[i];
        column^=t.columns[b];
        if (t.parity[b]) { line0^=uint8_t(~i); line1^=uint8_t(i); }
    }
    return {column,uint8_t(line0&0x7f),line1};
}

std::array<uint8_t,64> McDirInfo::table() const {
    std::array<uint8_t,64> t{};
    putTime(t.data(),created); putTime(t.data()+8,modified);
    wr32(t.data()+16,size); wr16(t.data()+20,attr); wr32(t.data()+24,reserve2); wr32(t.data()+28,pdaAplNo);
    std::copy_n(name.begin(),std::min<size_t>(name.size(),32),t.begin()+32);
    return t;
}
McDirInfo McDirInfo::fromTable(std::span<const uint8_t,64> t) {
    McDirInfo info;
    info.created=getTime(t.data()); info.modified=getTime(t.data()+8);
    info.size=rd32(t.data()+16); info.attr=rd16(t.data()+20); info.reserve2=rd32(t.data()+24); info.pdaAplNo=rd32(t.data()+28);
    const auto* name=reinterpret_cast<const char*>(t.data()+32);
    info.name.assign(name,strnlen(name,32));
    return info;
}

struct McCard::Impl {
    std::filesystem::path path;
    std::fstream file;
    std::vector<uint8_t> image;
    std::set<uint32_t> dirty;
    bool present=false,valid=false,checked=false;
    uint32_t pages=0,clusters=0,allocOffset=0,allocEnd=0,root=0;
    std::array<uint32_t,32> ifc{};
    std::string cwd="/";
    // An entry: index within the directory whose cluster chain starts at dir.
    struct EntryRef { uint32_t dir=0,index=0; };
    // A directory: its first cluster and the entry holding its length (root: its ".").
    struct DirRef { uint32_t first=0; EntryRef owner; };
    struct Handle { bool used=false,read=false,write=false; EntryRef entry; uint32_t position=0; };
    std::array<Handle,MaxHandles> handles{};
    struct Listing { bool active=false; DirRef dir; std::string pattern; uint32_t next=0; } listing;

    uint8_t* page(uint32_t n) { return image.data()+size_t(n)*RawPage; }
    void touch(uint32_t n) {
        auto* p=page(n);
        for (unsigned c=0;c<4;++c) {
            const auto e=mcEcc(std::span<const uint8_t,128>(p+c*128,128));
            std::copy(e.begin(),e.end(),p+PageSize+c*3);
        }
        std::fill(p+PageSize+12,p+RawPage,0);
        dirty.insert(n);
    }
    bool commit() {
        if (dirty.empty()) return true;
        bool ok=file.is_open();
        for (auto n:dirty) {
            if (!ok) break;
            file.seekp(std::streamoff(n)*RawPage);
            file.write(reinterpret_cast<const char*>(page(n)),RawPage);
            ok=bool(file);
        }
        file.flush(); dirty.clear();
        return ok && bool(file);
    }
    // FAT entry of allocatable cluster n (relative to allocOffset).
    uint8_t* fatSlot(uint32_t n) {
        const uint32_t perCluster=ClusterSize/4;
        const auto indirect=ifc[n/perCluster/perCluster];
        const auto fatCluster=rd32(clusterByte(indirect,(n/perCluster%perCluster)*4));
        return clusterByte(fatCluster,(n%perCluster)*4);
    }
    uint8_t* clusterByte(uint32_t absolute,uint32_t offset) {
        return page(absolute*PagesPerCluster+offset/PageSize)+offset%PageSize;
    }
    uint32_t fat(uint32_t n) { return rd32(fatSlot(n)); }
    void setFat(uint32_t n,uint32_t value) {
        const uint32_t perCluster=ClusterSize/4;
        const auto fatCluster=rd32(clusterByte(ifc[n/perCluster/perCluster],(n/perCluster%perCluster)*4));
        wr32(fatSlot(n),value);
        touch(fatCluster*PagesPerCluster+(n%perCluster)*4/PageSize);
    }
    std::vector<uint32_t> chain(uint32_t first) {
        std::vector<uint32_t> out;
        for (uint32_t c=first;c<allocEnd && out.size()<allocEnd;) {
            out.push_back(c);
            const auto next=fat(c);
            if (next==FatEnd || !(next&FatUsed)) break;
            c=next&~FatUsed;
        }
        return out;
    }
    int64_t allocate() {
        for (uint32_t n=0;n<allocEnd;++n) if (!(fat(n)&FatUsed)) { setFat(n,FatEnd); return n; }
        return -1;
    }
    void release(uint32_t first) {
        // Clearing the used bit keeps the old link, as mcman does.
        for (auto c:chain(first)) setFat(c,fat(c)&~FatUsed);
    }
    // Entry i of a directory: cluster chain[i/2], page i%2 (one entry per page).
    uint8_t* entry(const EntryRef& ref,uint32_t* pageNumber=nullptr) {
        const auto list=chain(ref.dir);
        if (ref.index/EntriesPerCluster>=list.size()) return nullptr;
        const auto n=(list[ref.index/EntriesPerCluster]+allocOffset)*PagesPerCluster+ref.index%EntriesPerCluster;
        if (pageNumber) *pageNumber=n;
        return page(n);
    }
    void writeEntry(const EntryRef& ref) { uint32_t n=0; if (entry(ref,&n)) touch(n); }
    uint32_t length(const DirRef& dir) { const auto* e=entry(dir.owner); return e?rd32(e+ELength):0; }
    DirRef rootDir() { return {root,{root,0}}; }
    int64_t find(const DirRef& dir,std::string_view name) {
        const auto count=length(dir);
        for (uint32_t i=0;i<count;++i) {
            const auto* e=entry({dir.first,i});
            if (e && (rd16(e+EMode)&ModeExists) && entryName(e)==name) return i;
        }
        return -1;
    }
    // Walks all but the last component (parent) or all components.
    int32_t resolve(std::string_view path,DirRef& dir,std::string& leaf,bool parent) {
        auto parts=components(cwd,path);
        dir=rootDir(); leaf.clear();
        if (parent) { if (parts.empty()) return NoEntry; leaf=parts.back(); parts.pop_back(); }
        for (const auto& part:parts) {
            const auto index=find(dir,part);
            if (index<0) return NoEntry;
            const EntryRef ref{dir.first,uint32_t(index)};
            const auto* e=entry(ref);
            if (!(rd16(e+EMode)&ModeDir)) return NoEntry;
            dir={rd32(e+ECluster),ref};
        }
        return Ok;
    }
    int64_t addEntry(const DirRef& dir) {
        const auto count=length(dir);
        for (uint32_t i=2;i<count;++i) {
            const auto* e=entry({dir.first,i});
            if (e && !(rd16(e+EMode)&ModeExists)) return i;
        }
        if (count/EntriesPerCluster>=chain(dir.first).size()) {
            const auto added=allocate(); if (added<0) return -1;
            const auto list=chain(dir.first);
            setFat(list.back(),FatUsed|uint32_t(added));
            for (uint32_t p=0;p<PagesPerCluster;++p) {
                const auto n=(uint32_t(added)+allocOffset)*PagesPerCluster+p;
                std::fill_n(page(n),PageSize,0); touch(n);
            }
        }
        auto* owner=entry(dir.owner);
        wr32(owner+ELength,count+1); putTime(owner+EModified,now()); writeEntry(dir.owner);
        return count;
    }
    void initEntry(uint8_t* e,uint16_t mode,uint32_t len,uint32_t cluster,uint32_t parentIndex,std::string_view name,
                   const McDateTime& created) {
        std::fill_n(e,PageSize,0);
        wr16(e+EMode,mode); wr32(e+ELength,len); putTime(e+ECreated,created); wr32(e+ECluster,cluster);
        wr32(e+EParent,parentIndex); putTime(e+EModified,created);
        std::copy_n(name.begin(),std::min<size_t>(name.size(),32),e+EName);
    }
    bool parse() {
        valid=false;
        const auto* sb=page(0);
        if (!std::equal(sb,sb+28,"Sony PS2 Memory Card Format ")) return false;
        if (rd16(sb+40)!=PageSize || rd16(sb+42)!=PagesPerCluster) return false;
        clusters=rd32(sb+48); allocOffset=rd32(sb+52); allocEnd=rd32(sb+56); root=rd32(sb+60);
        for (unsigned i=0;i<32;++i) ifc[i]=rd32(sb+80+i*4);
        if (clusters*PagesPerCluster>pages || allocOffset+allocEnd>clusters || root>=allocEnd || ifc[0]>=clusters) return false;
        valid=true; return true;
    }
    void erase() {
        std::fill(image.begin(),image.end(),0xff);
        for (uint32_t n=0;n<pages;++n) dirty.insert(n);
    }
    void formatImage() {
        // Standard layout (as the reference card): superblock, indirect FAT
        // at cluster 8, FAT from 9, allocatable clusters from 9+FAT, and two
        // erase blocks at the end reserved as backup blocks.
        erase();
        clusters=pages/PagesPerCluster;
        const uint32_t perCluster=ClusterSize/4,fatClusters=(clusters+perCluster-1)/perCluster;
        allocOffset=9+fatClusters; const uint32_t blocks=pages/16;
        allocEnd=clusters-allocOffset-2*8; root=0;
        ifc.fill(0); ifc[0]=8;
        auto* sb=page(0); std::fill_n(sb,PageSize,0);
        constexpr char magic[]="Sony PS2 Memory Card Format 1.2.0.0";
        std::copy_n(magic,sizeof(magic)-1,sb);
        wr16(sb+40,PageSize); wr16(sb+42,PagesPerCluster); wr16(sb+44,16); wr16(sb+46,0xff00);
        wr32(sb+48,clusters); wr32(sb+52,allocOffset); wr32(sb+56,allocEnd); wr32(sb+60,root);
        wr32(sb+64,blocks-1); wr32(sb+68,blocks-2); wr32(sb+80,8);
        std::fill_n(sb+208,128,0xff); sb[336]=2; sb[337]=0x2b;
        touch(0);
        for (uint32_t c=8;c<allocOffset;++c) for (uint32_t p=0;p<PagesPerCluster;++p) std::fill_n(page(c*PagesPerCluster+p),PageSize,0);
        for (uint32_t i=0;i<fatClusters;++i) wr32(clusterByte(8,i*4),9+i);
        for (uint32_t n=0;n<fatClusters*perCluster;++n) wr32(clusterByte(9+n/perCluster,(n%perCluster)*4),n<allocEnd?FatFree:FatEnd);
        for (uint32_t c=8;c<allocOffset;++c) for (uint32_t p=0;p<PagesPerCluster;++p) touch(c*PagesPerCluster+p);
        valid=true;
        wr32(fatSlot(root),FatEnd); setFat(root,FatEnd);
        const auto created=now();
        for (uint32_t p=0;p<PagesPerCluster;++p) {
            const auto n=(root+allocOffset)*PagesPerCluster+p;
            initEntry(page(n),p?uint16_t(ModeExists|ModeHidden|ModePs2|ModeDir|6):uint16_t(ModeExists|ModePs2|ModeDir|ModeRwx),
                      p?0:2,0,0,p?"..":".",created);
            touch(n);
        }
        cwd="/"; handles={}; listing={};
    }
    int32_t closeAll() { handles={}; listing={}; return Ok; }
    McDirInfo info(const uint8_t* e) {
        McDirInfo i; i.created=getTime(e+ECreated); i.modified=getTime(e+EModified);
        i.size=rd32(e+ELength); i.attr=rd16(e+EMode); i.reserve2=rd32(e+EAttr); i.name=entryName(e);
        return i;
    }
    Handle* handle(int32_t fd) { return fd>=0 && fd<MaxHandles && handles[fd].used?&handles[fd]:nullptr; }
    // Byte offset position of an open file: data pointer for up to one page.
    uint8_t* fileByte(uint32_t first,uint32_t position,uint32_t* pageNumber) {
        const auto list=chain(first);
        const auto index=position/ClusterSize;
        if (index>=list.size()) return nullptr;
        const auto n=(list[index]+allocOffset)*PagesPerCluster+position%ClusterSize/PageSize;
        *pageNumber=n;
        return page(n)+position%PageSize;
    }
};

McCard::McCard():impl_(std::make_unique<Impl>()) {}
McCard::~McCard() { detach(); }
bool McCard::attach(const std::filesystem::path& path,std::string& error) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    detach();
    std::error_code ec;
    const bool exists=std::filesystem::exists(path,ec);
    if (exists) {
        const auto size=std::filesystem::file_size(path,ec);
        if (ec || !size || size%RawPage || size/RawPage<1024) {
            error="memory card image is not a PS2 card with ECC (size "+std::to_string(size)+"): "+path.string();
            return false;
        }
        s.image.resize(size);
        std::ifstream in(path,std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(s.image.data()),std::streamsize(size))) { error="cannot read "+path.string(); return false; }
        s.pages=uint32_t(size/RawPage);
        s.parse();
    } else {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(),ec);
        s.pages=StandardPages; s.image.assign(size_t(s.pages)*RawPage,0xff);
        s.formatImage();
        std::ofstream out(path,std::ios::binary);
        if (!out.write(reinterpret_cast<const char*>(s.image.data()),std::streamsize(s.image.size()))) {
            error="cannot create "+path.string(); return false;
        }
        s.dirty.clear();
    }
    s.file.open(path,std::ios::binary|std::ios::in|std::ios::out);
    if (!s.file) { error="cannot open "+path.string()+" for writing"; return false; }
    s.path=path; s.present=true; s.checked=false; s.cwd="/";
    return true;
}
void McCard::detach() {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    s.commit(); if (s.file.is_open()) s.file.close();
    s.image.clear(); s.present=s.valid=false; s.handles={}; s.listing={};
}
bool McCard::present() const { std::lock_guard lock(mutex_); return impl_->present; }
int32_t McCard::detect() {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.present) return -12;
    if (s.checked) return Ok;
    s.checked=true;
    return s.valid?Changed:NoFormat;
}
int32_t McCard::formatted() const { std::lock_guard lock(mutex_); return impl_->present && impl_->valid; }
int32_t McCard::freeClusters() const {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.present || !s.valid) return 0;
    int32_t count=0;
    for (uint32_t n=0;n<s.allocEnd;++n) if (!(s.fat(n)&FatUsed)) ++count;
    return count;
}
int32_t McCard::open(std::string_view path,uint32_t flags) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    Impl::DirRef dir; std::string leaf;
    if (const auto r=s.resolve(path,dir,leaf,true); r<0) return r;
    auto index=s.find(dir,leaf);
    if (flags&OpenMkdir) {
        // sceMcMkdir: a new directory with "." and ".."; an existing name: -4.
        if (index>=0) return NoEntry;
        const auto added=s.addEntry(dir); if (added<0) { s.commit(); return Full; }
        const auto cluster=s.allocate(); if (cluster<0) { s.commit(); return Full; }
        const auto created=now();
        for (uint32_t p=0;p<PagesPerCluster;++p) {
            const auto n=(uint32_t(cluster)+s.allocOffset)*PagesPerCluster+p;
            s.initEntry(s.page(n),ModeExists|ModePs2|ModeDir|ModeRwx,0,0,p?0:uint32_t(added),p?"..":".",created);
            s.touch(n);
        }
        const Impl::EntryRef ref{dir.first,uint32_t(added)};
        s.initEntry(s.entry(ref),ModeExists|ModePs2|ModeDir|ModeRwx,2,uint32_t(cluster),0,leaf,created);
        s.writeEntry(ref);
        return s.commit()?Ok:Denied;
    }
    if (index<0) {
        if (!(flags&OpenCreate)) return NoEntry;
        if (leaf.size()>31) return NoEntry;
        const auto added=s.addEntry(dir); if (added<0) { s.commit(); return Full; }
        const Impl::EntryRef ref{dir.first,uint32_t(added)};
        s.initEntry(s.entry(ref),ModeExists|ModePs2|ModeClosed|ModeFile|ModeRwx,0,FatEnd,0,leaf,now());
        s.writeEntry(ref);
        index=added;
    }
    const Impl::EntryRef ref{dir.first,uint32_t(index)};
    auto* e=s.entry(ref);
    if (rd16(e+EMode)&ModeDir) return Denied;
    auto slot=std::find_if(s.handles.begin(),s.handles.end(),[](const auto& h) { return !h.used; });
    if (slot==s.handles.end()) { s.commit(); return Handles; }
    if ((flags&OpenTrunc) && (flags&OpenWrite) && rd32(e+ELength)) {
        if (rd32(e+ECluster)!=FatEnd) s.release(rd32(e+ECluster));
        wr32(e+ECluster,FatEnd); wr32(e+ELength,0); s.writeEntry(ref);
    }
    *slot={true,(flags&OpenRead)!=0,(flags&OpenWrite)!=0,ref,0};
    if (!s.commit()) return Denied;
    return int32_t(slot-s.handles.begin());
}
int32_t McCard::close(int32_t fd) {
    std::lock_guard lock(mutex_);
    auto* h=impl_->handle(fd); if (!h) return Denied;
    *h={}; return impl_->commit()?Ok:Denied;
}
int32_t McCard::flush(int32_t fd) {
    std::lock_guard lock(mutex_);
    if (!impl_->handle(fd)) return Denied;
    return impl_->commit()?Ok:Denied;
}
int32_t McCard::seek(int32_t fd,int32_t offset,int32_t origin) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    auto* h=s.handle(fd); if (!h) return Denied;
    const int64_t size=rd32(s.entry(h->entry)+ELength);
    int64_t target=origin==0?offset:origin==1?int64_t(h->position)+offset:origin==2?size+offset:-1;
    if (target<0) return Denied;
    h->position=uint32_t(target); return int32_t(target);
}
int32_t McCard::read(int32_t fd,std::span<uint8_t> data) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    auto* h=s.handle(fd); if (!h || !h->read) return Denied;
    const auto* e=s.entry(h->entry);
    const auto size=rd32(e+ELength),first=rd32(e+ECluster);
    uint32_t done=0;
    while (done<data.size() && h->position<size) {
        uint32_t pageNumber=0;
        const auto* p=s.fileByte(first,h->position,&pageNumber); if (!p) break;
        const auto count=std::min<uint32_t>({uint32_t(data.size())-done,size-h->position,PageSize-h->position%PageSize});
        std::copy_n(p,count,data.begin()+done);
        done+=count; h->position+=count;
    }
    return int32_t(done);
}
int32_t McCard::write(int32_t fd,std::span<const uint8_t> data) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    auto* h=s.handle(fd); if (!h || !h->write) return Denied;
    auto* e=s.entry(h->entry);
    uint32_t first=rd32(e+ECluster),done=0;
    int32_t result=0;
    while (done<data.size()) {
        // Extend the chain to cover this position.
        if (first==FatEnd) {
            const auto added=s.allocate(); if (added<0) { result=Full; break; }
            first=uint32_t(added); e=s.entry(h->entry); wr32(e+ECluster,first); s.writeEntry(h->entry);
        }
        auto list=s.chain(first);
        while (h->position/ClusterSize>=list.size()) {
            const auto added=s.allocate(); if (added<0) break;
            s.setFat(list.back(),FatUsed|uint32_t(added)); list.push_back(uint32_t(added));
        }
        if (h->position/ClusterSize>=list.size()) { result=Full; break; }
        uint32_t pageNumber=0;
        auto* p=s.fileByte(first,h->position,&pageNumber);
        const auto count=std::min<uint32_t>(uint32_t(data.size())-done,PageSize-h->position%PageSize);
        std::copy_n(data.begin()+done,count,p);
        s.touch(pageNumber);
        done+=count; h->position+=count;
    }
    e=s.entry(h->entry);
    if (h->position>rd32(e+ELength)) wr32(e+ELength,h->position);
    putTime(e+EModified,now()); s.writeEntry(h->entry);
    if (!s.commit()) return Denied;
    return done || !result?int32_t(done):result;
}
int32_t McCard::countDir(std::string_view path) {
    std::lock_guard lock(mutex_);
    McDirInfo entry; int32_t count=0;
    for (auto r=getDir(path,true,entry);r>0;r=getDir(path,false,entry)) ++count;
    return count;
}
int32_t McCard::getDir(std::string_view path,bool start,McDirInfo& out) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    if (start) {
        s.listing={};
        Impl::DirRef dir; std::string leaf;
        const bool rootOnly=components(s.cwd,path).empty();
        if (rootOnly) { dir=s.rootDir(); leaf="."; }
        // A missing parent directory is -4, as original MCMAN 7f08 (0x61d8 > 0).
        else if (const auto r=s.resolve(path,dir,leaf,true); r<0) return r;
        s.listing={true,dir,leaf,0};
    }
    if (!s.listing.active) return 0;
    const auto count=s.length(s.listing.dir);
    while (s.listing.next<count) {
        const auto* e=s.entry({s.listing.dir.first,s.listing.next++});
        if (!e || !(rd16(e+EMode)&ModeExists) || !matches(s.listing.pattern,entryName(e))) continue;
        out=s.info(e);
        // A directory's entry count is kept by its parent entry (root: ".").
        if (s.listing.next==1 && s.listing.dir.first==s.root) out.size=count;
        return 1;
    }
    s.listing.active=false;
    return 0;
}
int32_t McCard::chDir(std::string_view path,std::string& current) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    if (!path.empty()) {
        Impl::DirRef dir; std::string leaf;
        if (const auto r=s.resolve(path,dir,leaf,false); r<0) return r;
        std::string normalized;
        for (const auto& part:components(s.cwd,path)) normalized+="/"+part;
        s.cwd=normalized.empty()?"/":normalized;
    }
    current=s.cwd;
    return Ok;
}
int32_t McCard::setFileInfo(std::string_view path,const McDirInfo& info,uint32_t flags) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    Impl::DirRef dir; std::string leaf;
    if (const auto r=s.resolve(path,dir,leaf,true); r<0) return r;
    const auto index=s.find(dir,leaf); if (index<0) return NoEntry;
    const Impl::EntryRef ref{dir.first,uint32_t(index)};
    if ((flags&0x10) && info.name!=leaf) {
        if (info.name.empty() || info.name.size()>31 || s.find(dir,info.name)>=0) return Denied;
    }
    auto* e=s.entry(ref);
    if (flags&0x01) putTime(e+ECreated,info.created);
    if (flags&0x02) putTime(e+EModified,info.modified);
    if (flags&0x04) {
        // Access, protection and hidden bits only; the entry kind is kept.
        constexpr uint16_t settable=ModeRwx|0x0008|ModeHidden;
        wr16(e+EMode,uint16_t((rd16(e+EMode)&~settable)|(info.attr&settable)));
    }
    if (flags&0x10) { std::fill_n(e+EName,32,0); std::copy_n(info.name.begin(),info.name.size(),e+EName); }
    s.writeEntry(ref);
    return s.commit()?Ok:Denied;
}
int32_t McCard::remove(std::string_view path) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    Impl::DirRef dir; std::string leaf;
    if (const auto r=s.resolve(path,dir,leaf,true); r<0) return r;
    const auto index=s.find(dir,leaf); if (index<0) return NoEntry;
    const Impl::EntryRef ref{dir.first,uint32_t(index)};
    auto* e=s.entry(ref);
    const auto mode=rd16(e+EMode);
    for (const auto& h:s.handles) if (h.used && h.entry.dir==ref.dir && h.entry.index==ref.index) return Denied;
    if (mode&ModeDir) {
        const Impl::DirRef inner{rd32(e+ECluster),ref};
        const auto count=rd32(e+ELength);
        for (uint32_t i=2;i<count;++i) {
            const auto* child=s.entry({inner.first,i});
            if (child && (rd16(child+EMode)&ModeExists)) return NotEmpty;
        }
    }
    if (rd32(e+ECluster)!=FatEnd && ((mode&ModeDir) || rd32(e+ELength))) s.release(rd32(e+ECluster));
    e=s.entry(ref);
    wr16(e+EMode,uint16_t(mode&~ModeExists)); s.writeEntry(ref);
    return s.commit()?Ok:Denied;
}
int32_t McCard::entSpace(std::string_view path) {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.valid) return NoFormat;
    Impl::DirRef dir; std::string leaf;
    if (const auto r=s.resolve(path,dir,leaf,false); r<0) return r;
    // Unused entries, plus the free half of the last cluster.
    const auto count=s.length(dir);
    int32_t free=count%EntriesPerCluster;
    for (uint32_t i=2;i<count;++i) {
        const auto* e=s.entry({dir.first,i});
        if (e && !(rd16(e+EMode)&ModeExists)) ++free;
    }
    return free;
}
int32_t McCard::format() {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.present) return -12;
    s.formatImage();
    return s.commit()?Ok:Denied;
}
int32_t McCard::unformat() {
    std::lock_guard lock(mutex_);
    auto& s=*impl_;
    if (!s.present) return -12;
    s.erase(); s.valid=false; s.closeAll();
    return s.commit()?Ok:Denied;
}
}
