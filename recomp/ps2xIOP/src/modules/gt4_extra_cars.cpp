// GT4_EXTRA_CARS (analysis/jp-cars/README-BUILD.md): cars from the developer's own Japanese GT4 disc added to the US disc as new
// cars. The pack (ps2x/iop/gt4_extra_cars_pack.h, built on this PC by analysis/jp-cars/scripts/build_esso_pack.py) is
// served over the image by Disc (gt4_common.cpp): GT4.VOL's table of contents with the changed entries, and the changed
// files from host memory past the image's end. Full edition only: the Classic build (GT4_STOCK_BUILD) never reads a pack.
// No switch (developer 2026-10-14): when extra-cars\esso.gt4x sits next to GT4.exe and checks out against the disc, GT4 has
// the Toyota ESSO Ultraflo Supra (JGTC) '01 (car code 229: the Toyota dealer, Arcade's Toyota cars, the AI opponents).
// No pack: the image is served exactly as before.
//   GT4_EXTRA_CARS=off          (hidden) no extra cars even with the pack there; also 0 / none
//   GT4_EXTRA_CARS_PACK=<file>  (tests) another pack file
// A pack that is damaged, or a disc that is not the US disc it was built from (its size, its TOC sectors' and each
// replaced file's SHA-256), is logged once and GT4 runs with the stock disc.

#include "gt4_iop.h"
#include "ps2x/iop/gt4_extra_cars_pack.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ps2x::iop::gt4
{
    namespace
    {
        std::filesystem::path programFolder()
        {
#if defined(_WIN32)
            std::wstring buf(32768, L'\0');
            const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
            if (n && n < buf.size())
                return std::filesystem::path(buf.substr(0, n)).parent_path();
#endif
            std::error_code ec;
            return std::filesystem::current_path(ec);
        }
    }

    uint64_t Disc::servedSize() const { return extra_ ? std::max<uint64_t>(size_, extra_->extEnd()) : size_; }

#if defined(GT4_STOCK_BUILD)
    void Disc::loadExtraCars() {}
    void Disc::extraCars(uint64_t, uint8_t *, size_t) {}
#else
    void Disc::loadExtraCars()
    {
        extra_.reset();
        extraServed_.clear();
        const char *value = std::getenv("GT4_EXTRA_CARS");
        std::vector<std::string> unknown;
        const std::vector<std::string> names = extracars::wantedNames(value, &unknown);
        for (const std::string &u : unknown)
            host_.log(LogLevel::Warning, "[extra-cars] GT4_EXTRA_CARS: '" + u + "' is not an extra car (esso): ignored");
        if (names.empty())
            return; // GT4_EXTRA_CARS=off: the hidden way out
        // One pack today (esso); a second car would be a second pack, served the same way.
        const std::string name = names.front();
        std::filesystem::path path = extracars::packPath(programFolder(), name);
        if (const char *p = std::getenv("GT4_EXTRA_CARS_PACK"); p && *p)
            path = std::filesystem::u8path(p);
        if (std::error_code ec; !std::filesystem::exists(path, ec))
        {
            // No pack: GT4's own cars, as the stock disc has them (one line, so a log says why there is no ESSO).
            host_.log(LogLevel::Info, "[extra-cars] no " + path.string() + ": GT4 runs with the stock disc");
            return;
        }
        std::string why;
        std::shared_ptr<const extracars::Pack> pack = extracars::Pack::load(path, why);
        if (pack && pack->name() != name)
        {
            why = path.string() + " is the '" + pack->name() + "' pack";
            pack.reset();
        }
        if (pack && !pack->matches(
                        size_,
                        [this](uint64_t at, void *destination, size_t n) {
                            size_t got = 0;
                            return host_.readHostFile(file_, at, destination, n, got) && got == n;
                        },
                        why))
            pack.reset();
        if (!pack)
        {
            host_.log(LogLevel::Error, "[extra-cars] " + name + ": " + why + "; GT4 runs with the stock disc");
            return;
        }
        extra_ = pack;
        extraServed_.assign(pack->files().size() + 1, false);
        host_.log(LogLevel::Info, "[extra-cars] " + name + ": Toyota ESSO Ultraflo Supra (JGTC) '01 added as car 229 (pack " +
                                      path.string() + ", SHA-256 " + extracars::hexOf(pack->digest(), 8) + "...; " +
                                      std::to_string(pack->files().size()) + " files served from LBA " + std::to_string(pack->extLba()) +
                                      ", past the image's end, and GT4.VOL's table of contents with their entries; US disc checked)");
    }

    void Disc::extraCars(uint64_t offset, uint8_t *data, size_t bytes)
    {
        extra_->apply(offset, data, bytes);
        const uint64_t end = offset + bytes;
        auto touches = [&](uint64_t lba, uint64_t length) { return offset < lba * 2048u + length && lba * 2048u < end; };
        if (!extraServed_[0] && touches(extra_->tocLba(), uint64_t(extra_->tocSectors()) * 2048u))
        {
            extraServed_[0] = true;
            host_.log(LogLevel::Info, "[extra-cars] served GT4.VOL's table of contents (read at LSN " + std::to_string(offset / 2048u) + ", " +
                                          std::to_string(bytes) + " bytes)");
        }
        const std::vector<extracars::PackFile> &files = extra_->files();
        if (offset + bytes <= uint64_t(extra_->extLba()) * 2048u)
            return;
        for (size_t i = 0; i < files.size(); ++i)
            if (!extraServed_[i + 1] && touches(files[i].newLba, files[i].newStored))
            {
                extraServed_[i + 1] = true;
                host_.log(LogLevel::Info, "[extra-cars] served " + files[i].path + " (LSN " + std::to_string(files[i].newLba) + ", " +
                                              std::to_string(files[i].newStored) + " bytes stored, " + std::to_string(files[i].newReal) + " real)");
            }
    }
#endif
}
