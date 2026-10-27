#pragma once
// Bounded, opt-in local packet capture. Observes bytes without altering transfers.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_graphics_trace {
struct Capture {
    std::ofstream file;
    std::mutex mutex;
    uint64_t bytes=0,records=0,dropped=0;
    std::vector<uint32_t> only; // GT3_GFX_TRACE_ONLY: comma-separated hex kinds to keep
    uint64_t fromTick=0; // GT3_GFX_TRACE_FROM_TICK: skip records before this VSync tick
    bool enabled=false,failed=false;
    Capture() {
        if(const char* prefix=std::getenv("GT3_GFX_TRACE_PREFIX");prefix && *prefix) {
            file.open(std::string(prefix)+".packets.bin",std::ios::binary|std::ios::trunc);
            enabled=bool(file);
            if(enabled) file.write("GT3GFX01",8);
            if(const char* from=std::getenv("GT3_GFX_TRACE_FROM_TICK");from && *from) fromTick=std::strtoull(from,nullptr,10);
            if(const char* list=std::getenv("GT3_GFX_TRACE_ONLY");list && *list) {
                for(const char* p=list;*p;) { char* end=nullptr; only.push_back(uint32_t(std::strtoul(p,&end,16))); p=(*end==',')?end+1:end; if(end==p && *p) break; }
            }
            else std::cerr<<"[graphics-trace] Cannot open local capture\n";
        }
    }
    void record(uint32_t kind,uint32_t address,const uint8_t* data,uint32_t size,uint64_t tick) {
        if(!enabled || tick<fromTick) return;
        if(!only.empty() && std::find(only.begin(),only.end(),kind)==only.end()) return;
        std::lock_guard lock(mutex);
        if(failed || records>=8192 || bytes+size+24>64u*1024u*1024u || (size && !data)) {
            if(dropped++ == 0)
                std::cerr<<"[graphics-trace] Capture stopped or bounded; later records omitted after "
                         <<records<<" records / "<<bytes<<" bytes\n";
            return;
        }
        const uint32_t header[]={kind,address,size,uint32_t(tick),uint32_t(tick>>32),0};
        file.write(reinterpret_cast<const char*>(header),sizeof(header));
        if(size) file.write(reinterpret_cast<const char*>(data),size);
        file.flush(); // Diagnostic runtime uses _Exit, so retain every complete record.
        failed=!file;
        if(failed) std::cerr<<"[graphics-trace] Capture write failed; stopping capture\n";
        ++records;bytes+=sizeof(header)+size;
    }
};
inline Capture& capture() {static Capture state;return state;}
// Opt-in counters (GT3_VU1_STATS=1): VIF1/GIF DMA kicks and completions, VIF1
// i-bit stalls and FBRST.STC restarts; printed at every 16th VIF1 kick.
struct DmaStats {
    bool enabled=std::getenv("GT3_VU1_STATS")!=nullptr;
    std::atomic<uint64_t> vif1Kicks{0},vif1Done{0},vif1Istall{0},vif1Stc{0},gifKicks{0},gifDone{0},gifKicksMasked{0};
};
inline DmaStats& dmaStats() {static DmaStats stats;return stats;}
inline void noteVif1Kick(uint64_t tick) {
    auto& s=dmaStats(); if(!s.enabled) return;
    if((++s.vif1Kicks & 15u)==0u)
        std::cerr<<"[dma-stats] tick="<<tick<<" vif1Kicks="<<s.vif1Kicks<<" vif1Done="<<s.vif1Done<<" vif1Istall="<<s.vif1Istall
                 <<" vif1Stc="<<s.vif1Stc<<" gifKicks="<<s.gifKicks<<" gifDone="<<s.gifDone<<" gifKicksWhilePath3Masked="<<s.gifKicksMasked<<std::endl;
}
// Opt-in text trace of DMA/VIF events (GT3_DMA_TRACE=<first VSync tick>).
inline uint64_t dmaTraceFrom() {
    static const uint64_t from=[] { const char* v=std::getenv("GT3_DMA_TRACE"); return (v && *v) ? std::strtoull(v,nullptr,10) : ~0ull; }();
    return from;
}
inline void dmaTrace(uint64_t tick,const char* format,...) {
    if(tick<dmaTraceFrom()) return;
    char line[320];
    va_list args; va_start(args,format); std::vsnprintf(line,sizeof(line),format,args); va_end(args);
    std::fprintf(stderr,"[dma-trace] t=%llu %s\n",static_cast<unsigned long long>(tick),line);
}
inline void record(uint32_t kind,uint32_t address,const uint8_t* data,uint32_t size,uint64_t tick) {
    capture().record(kind,address,data,size,tick);
}
}
