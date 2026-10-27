#pragma once
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gt3_diagnostic {
// Read-only trace of selected supplied-CORE calls. Bounded and
// opt-in; a return record can describe a scheduler yield, not just completion.
class InputCallTrace {
    std::ofstream output;
    unsigned records=0;
    uint32_t firstFrame=190, lastFrame=450;
    std::unordered_set<uint32_t> targets{0x22fc30,0x22ffe8,0x22dc10,
        0x1c9e38,0x1baa70,0x1b7c08,0x1b4560,0x1ca0f8};
    static uint32_t number(std::string_view value,int base) {
        uint32_t result=0;
        const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result,base);
        if (error!=std::errc{} || end!=value.data()+value.size())
            throw std::runtime_error("Invalid GT3_INPUT_CALL_TRACE filter");
        return result;
    }
public:
    InputCallTrace() {
        if (const char* path=std::getenv("GT3_INPUT_CALL_TRACE"); path && *path) {
            if (const char* list=std::getenv("GT3_INPUT_CALL_TRACE_PC")) {
                targets.clear();
                std::string_view remaining(list);
                do {
                    const auto comma=remaining.find(',');
                    targets.insert(number(remaining.substr(0,comma),16));
                    if (comma==remaining.npos) break;
                    remaining.remove_prefix(comma+1);
                } while (targets.size()<=64);
                if (targets.size()>64) throw std::runtime_error("Too many GT3_INPUT_CALL_TRACE targets");
            }
            if (const char* frames=std::getenv("GT3_INPUT_CALL_TRACE_FRAMES")) {
                const std::string_view range(frames);
                const auto colon=range.find(':');
                if (colon==range.npos) throw std::runtime_error("Invalid GT3_INPUT_CALL_TRACE frame range");
                firstFrame=number(range.substr(0,colon),10);
                lastFrame=number(range.substr(colon+1),10);
                if (firstFrame>lastFrame || lastFrame>1000000)
                    throw std::runtime_error("Invalid GT3_INPUT_CALL_TRACE frame range");
            }
            output.open(path,std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot open GT3_INPUT_CALL_TRACE");
            output<<"# selected-call trace v2: frame phase source target resume_pc a0 a1 v0; source384 destination384; frames="
                <<firstFrame<<':'<<lastFrame<<"; max16384 records\n";
        }
    }
    bool selected(uint64_t tick,uint32_t target) const {
        return output.is_open() && tick>=firstFrame && tick<=lastFrame && records<16384 && targets.contains(target);
    }
    void record(const char* phase,uint64_t tick,uint32_t source,uint32_t target,uint32_t pc,
        uint32_t a0,uint32_t a1,uint32_t result,const uint8_t* ram) {
        if (!selected(tick,target)) return;
        ++records;
        output<<std::dec<<tick<<' '<<phase<<' '<<std::hex<<source<<' '<<target<<' '<<pc<<' '<<a0<<' '<<a1<<' '<<result;
        for (auto [address,count]:{std::pair{a0,384u},std::pair{a1,384u}}) {
            address&=0x1fffffffu;
            output<<' ';
            if (!ram || address>PS2_RAM_SIZE-count) { output<<"invalid"; continue; }
            constexpr char hex[]="0123456789abcdef";
            for (unsigned i=0;i<count;++i) { const auto b=ram[address+i]; output<<hex[b>>4]<<hex[b&15]; }
        }
        output<<'\n';
    }
};
inline InputCallTrace& inputCallTrace() { static thread_local InputCallTrace trace; return trace; }
}
