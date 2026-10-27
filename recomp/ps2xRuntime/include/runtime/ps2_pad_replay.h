#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

// Opt-in diagnostic input source. It supplies buttons (and optionally sticks)
// to SIO2; it never writes game state or bypasses PADMAN/the original EE input
// and menu code. Line: begin end buttons-hex [rx ry lx ly as 8 hex digits].
class PSPadReplay
{
    struct Interval { uint64_t begin, end; uint16_t pressed; std::array<uint8_t,4> sticks; };
    std::vector<Interval> m_intervals;
    bool m_enabled = false;
public:
    bool load(std::istream& input, std::string& error)
    {
        std::vector<Interval> candidate;
        std::string line;
        unsigned lineNumber = 0;
        auto number=[](const std::string& text,int base,uint64_t& value) {
            const auto result=std::from_chars(text.data(),text.data()+text.size(),value,base);
            return result.ec==std::errc{} && result.ptr==text.data()+text.size();
        };
        while (std::getline(input,line)) {
            ++lineNumber;
            if (const auto comment=line.find('#'); comment!=std::string::npos) line.resize(comment);
            std::istringstream row(line);
            std::string begin,end,mask,sticks,extra;
            if (!(row>>begin)) continue;
            uint64_t first=0,last=0,pressed=0,axes=0x80808080u;
            if (!(row>>end>>mask) || ((row>>sticks) && (sticks.size()!=8 || !number(sticks,16,axes))) ||
                (row>>extra) || !number(begin,10,first) ||
                !number(end,10,last) || !number(mask,16,pressed) || first>=last ||
                last>1000000 || pressed>0xffff || candidate.size()>=4096 ||
                (!candidate.empty() && first<candidate.back().end)) {
                error="Invalid pad replay line "+std::to_string(lineNumber);
                return false;
            }
            candidate.push_back({first,last,static_cast<uint16_t>(pressed),
                {uint8_t(axes>>24),uint8_t(axes>>16),uint8_t(axes>>8),uint8_t(axes)}});
        }
        if (input.bad() || candidate.empty()) {
            error="Pad replay must contain readable frame intervals";
            return false;
        }
        m_intervals=std::move(candidate); m_enabled=true; error.clear(); return true;
    }
    bool enabled() const { return m_enabled; }
    const Interval* at(uint64_t tick) const
    {
        const auto it=std::upper_bound(m_intervals.begin(),m_intervals.end(),tick,
            [](uint64_t value,const Interval& interval) { return value<interval.begin; });
        if (it==m_intervals.begin()) return nullptr;
        return tick<(it-1)->end?&*(it-1):nullptr;
    }
    uint16_t pressed(uint64_t tick) const
    {
        const auto it=std::upper_bound(m_intervals.begin(),m_intervals.end(),tick,
            [](uint64_t value,const Interval& interval) { return value<interval.begin; });
        if (it==m_intervals.begin()) return 0;
        const auto& interval=*(it-1);
        return tick<interval.end?interval.pressed:0;
    }
    bool sample(uint64_t tick,std::array<uint8_t,32>& packet) const
    {
        if (!m_enabled) return false;
        packet.fill(0); packet[0]=1; packet[1]=0x79; // with pressure bytes (below)
        const auto down=pressed(tick);
        const auto released=static_cast<uint16_t>(~down);
        packet[2]=static_cast<uint8_t>(released); packet[3]=static_cast<uint8_t>(released>>8);
        std::fill(packet.begin()+4,packet.begin()+8,0x80);
        if (const auto* interval=at(tick)) std::copy(interval->sticks.begin(),interval->sticks.end(),packet.begin()+4);
        // The pressure bytes (DualShock 2 order: right, left, up, down, triangle,
        // circle, cross, square, L1, R1, L2, R2): a pressed button fully
        // pressed, as a digital replay means (GT3 reads them in pressure
        // mode). GT3_PAD_PRESSURE=<0..255> (tests): the level pressed buttons get.
        static const int level=[] {
            const char* v=std::getenv("GT3_PAD_PRESSURE");
            return v&&*v?std::clamp(std::atoi(v),0,255):255;
        }();
        constexpr uint16_t masks[12]={0x20,0x80,0x10,0x40,0x1000,0x2000,0x4000,0x8000,0x400,0x800,0x100,0x200};
        for (int i=0;i<12;++i) packet[8+i]=(down&masks[i])?static_cast<uint8_t>(level):0;
        return true;
    }
};
