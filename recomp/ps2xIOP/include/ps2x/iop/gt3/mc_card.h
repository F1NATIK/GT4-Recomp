#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::gt3 {
// sceMcStDateTime: resv, sec, min, hour, day, month, year (JST).
struct McDateTime {
    uint8_t resv=0,sec=0,min=0,hour=0,day=1,month=1;
    uint16_t year=2001;
};
// sceMcTblGetDir (64 bytes on the EE).
struct McDirInfo {
    McDateTime created,modified;
    uint32_t size=0;
    uint16_t attr=0;
    uint32_t reserve2=0,pdaAplNo=0;
    std::string name;
    std::array<uint8_t,64> table() const;
    static McDirInfo fromTable(std::span<const uint8_t,64>);
};

// Native PS2 memory card file system (mcfs) on a host image in the PCSX2
// .ps2 layout: 512-byte pages each followed by 16 spare bytes holding the
// Hamming ECC of its four 128-byte chunks. MCMAN-level operations and
// result codes (0 success, -1 changed card, -2 unformatted, -3 full, -4 no
// entry, -5 denied, -6 not empty, -7 handle limit). Every mutating operation
// writes its changed pages to the image before returning.
class McCard {
public:
    McCard();
    ~McCard();
    // Opens the image (created and formatted when missing). false: no card.
    bool attach(const std::filesystem::path& image,std::string& error);
    void detach();
    bool present() const;
    // McDetectCard2: -1 on the first check after insertion (formatted),
    // -2 unformatted, then 0.
    int32_t detect();
    int32_t formatted() const;
    int32_t freeClusters() const;
    int32_t open(std::string_view path,uint32_t flags);
    int32_t close(int32_t fd);
    int32_t read(int32_t fd,std::span<uint8_t> data);
    int32_t write(int32_t fd,std::span<const uint8_t> data);
    int32_t seek(int32_t fd,int32_t offset,int32_t origin);
    int32_t flush(int32_t fd);
    // One entry per call: 1 written, 0 no more, <0 error. start restarts.
    int32_t getDir(std::string_view path,bool start,McDirInfo& entry);
    int32_t countDir(std::string_view path);
    int32_t chDir(std::string_view path,std::string& current);
    int32_t setFileInfo(std::string_view path,const McDirInfo& info,uint32_t flags);
    int32_t remove(std::string_view path);
    int32_t entSpace(std::string_view path);
    int32_t format();
    int32_t unformat();
    static constexpr int32_t MaxHandles=16;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::recursive_mutex mutex_;
};
// Hamming code of one 128-byte chunk as the PS2 card stores it (3 bytes).
std::array<uint8_t,3> mcEcc(std::span<const uint8_t,128> chunk);
}
