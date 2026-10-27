// GT4 Recompilation launcher, Full edition: the pages' data (gt4full_model.h). No raylib, no window.

#include "gt4full_model.h"
#include "gt4full_completion.h"
#include "gt4stock_resources.h"

#include "gt4save.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

namespace gt4stock
{
    namespace
    {
        constexpr const char *kSpec = "specdb/GT4_US2560/";
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        float lef(const uint8_t *p)
        {
            const uint32_t u = le32(p);
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
    }

    // ------------------------------------------------------------------ what the GT4 program reads
    std::string FullFeatures::text() const
    {
        if (!read)
            return "not read (" + error + ")";
        auto yn = [](bool b) { return b ? "yes" : "no"; };
        return strf("480p update keys %s (GT4_RACE_MODE %s, GT4_RACE_COLOUR %s, GT4_ASPECT %s), bicubic %s, lanczos %s, Vulkan %s, GT3_RENDERER %s, "
                    "square pixels %s, span %s, LAN %s, engine bass %s, FPS counter %s, FPS counter detailed %s, internal resolution checked with GT4 %s, wider shapes %s, race cameras %s, "
                    "more race cameras %s, car and wheel detail %s, three screens %s, GT4 Multi Monitor Mode %s, colour LUTs %s, extra cars %s, Classic build %s, "
                    "sound packs %s",
                    yn(has480p()), yn(raceMode), yn(raceColour), yn(aspect), yn(bicubic), yn(lanczos), yn(vulkan), yn(renderer), yn(squarePixels), yn(span),
                    yn(net), yn(engineBass), yn(fpsCounter), yn(fpsStats), yn(internalRes), yn(wideShapes), yn(cameras), yn(moreCameras), yn(detail), yn(screens), yn(multiMonitor), yn(lut), yn(extraCars), yn(classic), yn(soundPacks));
    }

    FullFeatures fullFeatures(const fs::path &exe)
    {
        FullFeatures f;
        std::ifstream in(exe, std::ios::binary);
        if (!in)
        {
            f.error = "cannot open the file";
            return f;
        }
        auto readAt = [&](uint64_t off, void *dst, size_t n) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(off));
            in.read(static_cast<char *>(dst), static_cast<std::streamsize>(n));
            return static_cast<size_t>(in.gcount()) == n;
        };
        uint8_t dos[64];
        if (!readAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z')
        {
            f.error = "not a Windows program (no MZ header)";
            return f;
        }
        const uint32_t pe = le32(dos + 0x3c);
        uint8_t hdr[24];
        if (pe > 0x10000000u || !readAt(pe, hdr, sizeof(hdr)) || hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0)
        {
            f.error = "not a Windows program (no PE header)";
            return f;
        }
        const uint32_t sections = std::min<uint32_t>(le16(hdr + 6), 96), optSize = le16(hdr + 20);
        std::vector<uint8_t> table(static_cast<size_t>(sections) * 40);
        if (!readAt(pe + 24 + optSize, table.data(), table.size()))
        {
            f.error = "no section table";
            return f;
        }
        bool any = false;
        for (size_t i = 0; i + 40 <= table.size(); i += 40)
        {
            const uint8_t *sec = table.data() + i;
            const std::string name(reinterpret_cast<const char *>(sec), strnlen(reinterpret_cast<const char *>(sec), 8));
            const uint32_t rawSize = le32(sec + 16), raw = le32(sec + 20);
            if ((name != ".rdata" && name != ".data") || rawSize == 0 || rawSize > (256u << 20))
                continue;
            std::string data(rawSize, '\0');
            if (!readAt(raw, data.data(), data.size()))
                continue;
            any = true;
            auto has = [&](std::string_view word) {
                std::string pattern(1, '\0');
                pattern.append(word);
                pattern.push_back('\0');
                return data.find(pattern) != std::string::npos;
            };
            // A marker that GT4.exe logs as a part of a longer line ("[window] full screen (exclusive) on monitor ...",
            // "[gt4-hud] GT4 own race HUD (hud-4): ...", "[gt4-mm] GT4 Multi Monitor settings (multi-monitor): ..."): looked for
            // anywhere in the text (as a whole string it is never there, and the launcher greyed or left out what GT4.exe has).
            auto inText = [&](std::string_view word) { return data.find(word) != std::string::npos; };
            f.raceMode = f.raceMode || has("GT4_RACE_MODE");
            f.raceColour = f.raceColour || has("GT4_RACE_COLOUR");
            f.aspect = f.aspect || has("GT4_ASPECT");
            f.bicubic = f.bicubic || has("bicubic");
            f.lanczos = f.lanczos || has("lanczos");
            f.vulkan = f.vulkan || has("GT3_VKGL_PREPARE");
            f.renderer = f.renderer || has("GT3_RENDERER");
            f.squarePixels = f.squarePixels || has("GT3_DISPLAY_ASPECT");
            f.span = f.span || has("span");
            f.exclusive = f.exclusive || inText("full screen (exclusive)");
            f.net = f.net || has("GT4_NET");
            f.engineBass = f.engineBass || has("GT4_ENGINE_BASS");
            f.fpsCounter = f.fpsCounter || has("GT4_FPS_COUNTER");
            f.fpsStats = f.fpsStats || has("GT4_FPS_STATS");
            f.internalRes = f.internalRes || has(kInternalResMarker);
            f.wideShapes = f.wideShapes || has(kWideShapesMarker);
            f.cameras = f.cameras || has(kCamerasMarker);
            f.moreCameras = f.moreCameras || has(kMoreCamerasMarker);
            f.hud = f.hud || inText(kHudMarker);
            f.detail = f.detail || has(kDetailMarker);
            f.screens = f.screens || has(kScreensMarker);
            f.multiMonitor = f.multiMonitor || inText(kMultiMonitorMarker);
            f.lut = f.lut || has(kLutMarker);
            f.texSmooth = f.texSmooth || has(kTexSmoothMarker);
            f.texDump = f.texDump || has("GT4_TEXTURE_REPLACE");
            f.extraCars = f.extraCars || has("GT4_EXTRA_CARS");
            f.rendererAuto = f.rendererAuto || has("GT4_RENDERER_PRETEND_VENDOR");
            // The Classic build's start line (main.cpp, GT4_STOCK_BUILD only). Not "GT4_CLASSIC_BUILD=1" alone: every development
            // GT4.exe has that too (gt4stock_util.cpp is linked into it), which made the launcher call it "the Classic GT4".
            f.classic = f.classic || inText("GT4_CLASSIC_BUILD=1 (GT4 Recompilation");
            f.soundPacks = f.soundPacks || has("GT4_SOUND_PACK");
        }
        if (!any)
        {
            f.error = "no data section";
            return f;
        }
        f.read = true;
        return f;
    }

    // ------------------------------------------------------------------ the disc
    const char *drivetrainName(int d)
    {
        switch (d)
        {
        case 0: return "FR";
        case 1: return "FF";
        case 2: return "4WD";
        case 3: return "MR";
        default: return "?";
        }
    }

    std::string tyreText(const DiscCar::Tyre &t) { return t.ok() ? strf("%d/%dR%d", t.width, t.aspect, t.rim) : std::string(); }
    std::string weightBalanceText(int frontPct) { return frontPct > 0 && frontPct < 100 ? strf("%d : %d", frontPct, 100 - frontPct) : std::string("-"); }

    // SAVE-FORMAT.md 8.1 [C]: colour index 1 is black (RGB 31 / 10 / 16 / 20, R = G = B); GT4's US used lots sell them
    // only in week 99. Then the one of the developer's rare cars (2026-10-08) that GT4 gives as no prize: the Abt Audi TT-R (in no
    // PresentData row, no US used-car week). the developer's other twelve are event prizes (PresentData) and are listed under Prize
    // Cars, which now holds every car of GT4's prize table (developer, 2026-10-08: "aren't they just prize cars?"): the BMW McLaren
    // F1 GTR '97, Sauber Mercedes C 9 '89, Honda MUGEN MOTUL CIVIC Si '87 (the developer's "Civic Si Group A"), AMG Mercedes 190 E
    // Evolution II '92, AMG Mercedes CLK-GTR Race Car '98 (the developer's CLK-LM: not in GT4 US), Mercedes-Benz CLK Touring Car '00,
    // Alfa Romeo 155 2.5 V6 TI '93, Nissan BLUEBIRD Rally Car '69, CALSONIC SKYLINE GT-R '93, MOTUL PITWORK Z '04, SKYLINE
    // 2000GT-R (KPGC10) '70 and SKYLINE 2000GT-B (S54A; the developer's S54B) '67.
    const HiddenCar kHiddenCars[kHiddenCarCount] = {
        {"r390_race_98", 1, 99, 2, 31},
        {"gt_one_race_exxon_99", 1, 99, 2, 10},
        {"_787b_91", 1, 99, 1, 16},
        {"r92cp_92", 1, 99, 1, 20},
        {"formula_gran_turismo_04", 10, -1, 0, 6,
         "GT4's Formula GT in its black colour (GT4 gives it for 100% completion, per GTPlanet; unverified here)."},
        {"tt_dtm_02", 0, -1, 0}, // Audi Abt Audi TT-R Touring Car '02 (the developer's; no prize in GT4)
    };

    int blackCarGrey(const std::string &label, int colourIndex)
    {
        for (const HiddenCar &h : kHiddenCars)
            if (h.blackGrey > 0 && label == h.label && colourIndex == h.colour)
                return h.blackGrey;
        return 0;
    }

    int blackCarPictureResource(const std::string &label)
    {
        static const std::pair<const char *, int> kPictures[] = {
            {"r390_race_98", GT4FULL_IDR_BLACK_R390},
            {"gt_one_race_exxon_99", GT4FULL_IDR_BLACK_GT_ONE},
            {"_787b_91", GT4FULL_IDR_BLACK_787B},
            {"r92cp_92", GT4FULL_IDR_BLACK_R92CP},
            {"formula_gran_turismo_04", GT4FULL_IDR_BLACK_FGT}, // GT4's own car_pict for it is empty
        };
        for (const auto &[l, id] : kPictures)
            if (label == l)
                return id;
        return 0;
    }

    int prizeCarPictureResource(const std::string &label)
    {
        // analysis/design/prize-cars: GT4's prize cars with no dealer picture on the disc (none under any name, or an empty
        // one), each GT4's own 3D render from its Garage car screen, in the car's first colour.
        static const std::pair<const char *, int> kPictures[] = {
            {"mazdaspeed_6_05", GT4FULL_IDR_PRIZE_MAZDASPEED6},
            {"motor_carriage_886", GT4FULL_IDR_PRIZE_MOTOR_CARRIAGE},
            {"autounion_v16_typec_sl_37", GT4FULL_IDR_PRIZE_AUTO_UNION},
            {"chaparral_2d_race_67", GT4FULL_IDR_PRIZE_CHAPARRAL_2D},
            {"patent_motor_wagen_886", GT4FULL_IDR_PRIZE_MOTOR_WAGEN},
            {"pikespeak_quattro_03", GT4FULL_IDR_PRIZE_PIKES_PEAK},
            {"mazda_suv_concept_05", GT4FULL_IDR_PRIZE_MX_CROSSPORT},
            {"model_t_tourer_15", GT4FULL_IDR_PRIZE_MODEL_T},
            {"nuvolari_quattro_03", GT4FULL_IDR_PRIZE_NUVOLARI},
            {"xjr9_race_88", GT4FULL_IDR_PRIZE_XJR9},
            {"formula_gran_turismo_04", GT4FULL_IDR_PRIZE_FGT}, // its first colour (black: blackCarPictureResource)
        };
        for (const auto &[l, id] : kPictures)
            if (label == l)
                return id;
        return 0;
    }

    void blackenPicture(std::vector<uint8_t> &rgba, int grey)
    {
        // The car's own brightness (opaque pixels), from its darkest 5% to its brightest 3%, mapped to GT4's black and a dark
        // sheen: the shape, panel lines and reflections stay; the livery's colours go (its tones stay faintly).
        auto lum = [&](size_t i) { return 0.299f * rgba[i] + 0.587f * rgba[i + 1] + 0.114f * rgba[i + 2]; };
        std::vector<float> ls;
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            if (rgba[i + 3] >= 100)
                ls.push_back(lum(i));
        if (ls.empty())
            return;
        std::sort(ls.begin(), ls.end());
        const float lo = ls[ls.size() * 5 / 100], hi = ls[ls.size() * 97 / 100];
        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            if (rgba[i + 3] == 0)
                continue;
            const float n = std::clamp((lum(i) - lo) / std::max(1.0f, hi - lo), 0.0f, 1.0f);
            // Body tones stay near GT4's black; the brightest parts give a grey sheen so the car reads on a dark panel.
            const float v = grey * 0.5f + 12.0f + 95.0f * std::pow(n, 2.0f);
            const uint8_t c = static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f));
            rgba[i] = rgba[i + 1] = rgba[i + 2] = c;
        }
    }

    const char *pictureName(const std::string &label)
    {
        // [C] each pair checked by eye (GT4's pictures side by side with the car's name, analysis/design/full-launcher/
        // screens/tweaks-2); the US label's own picture is GT4's AE86 stand-in or missing.
        static const std::pair<const char *, const char *> kNames[] = {
            {"viper_gtsr_00", "chrysler_viper_gtsr_00"},               // Dodge VIPER GTSR Concept '00 (its own: the stand-in)
            {"viper_gts_99", "chrysler_viper_gts_99"},                 // Dodge VIPER GTS '99
            {"viper_srt10_03", "chrysler_viper_srt10_03"},             // Dodge VIPER SRT10 '03
            {"srt4_03", "chrysler_srt4_03"},                           // Dodge SRT4 '03 (its own: the stand-in)
            {"dodge_viper_gtsr_oreca_00", "viper_gtsr_oreca_00"},      // Dodge VIPER GTS-R Team Oreca Race Car '00
            {"_350z_lm_us_02", "z_tmv_lm_01"},                         // Nissan 350Z Concept LM Race Car '02
            {"_110s_67", "cosmo_sport_67"},                            // Mazda 110S (L10A) '67
            {"_110s_68", "cosmo_sport_68"},                            // Mazda 110S (L10B) '68 (its own: the stand-in)
            {"_240zg_71", "fairlady_240zg_71"},                        // Nissan 240ZG (HS30) '71 (its own: the stand-in)
            {"_350z_roadster_03", "fairlady_z_roadster_03"},           // Nissan 350Z Roadster (Z33) '03 (its own: the stand-in)
            {"_240sx_180_us_96", "_180sx_x_96"},                       // Nissan 240SX '96
            {"clio_sport_race_00", "lutecia_sport_race_00"},           // Clio Renault Sport Trophy V6 24V Race Car '00 (the stand-in)
            {"clio_sport_v6_00", "lutecia_sport_v6_01"},               // Renault Clio Renault Sport V6 24V '00 (the stand-in)
            {"clio_sport_v6_2_03", "lutecia_sport_v6_2_03"},           // Renault Clio Renault Sport V6 Phase 2 '03
            {"clio_renaultsport_20_02", "lutecia_renaultsport_20_02"}, // Renault Clio Renault Sport 2.0 16V '02
            {"mazda6_sport_23z_03", "atenza_sport_23z_03"},            // Mazda Mazda6 5-door '03 (its own: the stand-in)
            {"micra_12c_5d_03", "march_12c_5d_03"},                    // Nissan MICRA '03
            {"acura_hsc_04", "hsc_concept_03"},                        // Acura HSC '04
            {"integra_r_03", "integra_r_01"},                          // Honda INTEGRA TYPE R (DC5) '03 (its own: the stand-in)
            {"ford_gt_no_stripe_05", "ford_gt_05"},                    // Ford GT '05 (GT4 has no picture without the stripes)
            // Prize cars (analysis/design/prize-cars): no picture under the US label, GT4's picture under the Japanese name.
            {"mazda6_tmv_01", "atenza_5hb_01"},                        // Mazda Mazda6 Concept '01 (yellow 5-door, "Atenza" badge)
            {"mazda6_tmv_n1_01", "atenza_5hb_n1_01"},                  // Mazda Mazda6 Touring Car '02 (the N1 Atenza)
            {"acura_dnx_02", "dualnote_tmv_01"},                       // Acura DN-X '02 (Honda's Dualnote concept)
        };
        for (const auto &[l, p] : kNames)
            if (label == l)
                return p;
        return nullptr;
    }

    double usedPriceFactor(uint64_t km)
    {
        // CORE.elf 0x6a6988: {thousands of km below which, factor}: <1 0.85, <3 0.80, <5 0.78, <10 0.77, <15 0.76, <25 0.75,
        // <40 0.65, <60 0.50, <100 0.35, else 0.10 [C, read from CORE.elf].
        static const struct
        {
            uint64_t below;
            double factor;
        } table[] = {{1, 0.85}, {3, 0.80}, {5, 0.78}, {10, 0.77}, {15, 0.76}, {25, 0.75}, {40, 0.65}, {60, 0.50}, {100, 0.35}};
        const uint64_t thousands = km / 1000;
        for (const auto &e : table)
            if (thousands < e.below)
                return e.factor;
        return 0.10;
    }

    uint64_t usedPrice(uint32_t newPrice, uint64_t km)
    {
        // (int)(price x factor) in single precision with the factor as GT4 stores it (0.65 is 0.649999976 there), the
        // product rounded toward zero as the PS2's FPU does [I: the rounding mode is the EE's; the code path is C], then cut
        // to a whole number.
        const double exact = static_cast<double>(static_cast<float>(newPrice)) * static_cast<double>(static_cast<float>(usedPriceFactor(km)));
        float f = static_cast<float>(exact);
        if (static_cast<double>(f) > exact)
            f = std::nextafter(f, 0.0f);
        return static_cast<uint64_t>(f);
    }

    std::pair<int, int> usedMileageRangeKm(int year)
    {
        // CORE.elf 0x6a6910 [C]: thousands of km per model year 1990..2004.
        static const int range[15][2] = {{60, 100}, {55, 95}, {50, 90}, {50, 85}, {45, 80}, {45, 75}, {40, 70}, {35, 65},
                                         {30, 60},  {25, 55}, {20, 50}, {15, 40}, {15, 30}, {10, 20}, {3, 10}};
        const int i = std::clamp(year - 1990, 0, 14);
        return {range[i][0] * 1000, range[i][1] * 1000};
    }

    std::vector<std::string> scriptTokens(const std::vector<uint8_t> &d)
    {
        std::vector<std::string> out;
        size_t i = 0;
        const size_t n = d.size();
        while (i + 2 < n)
        {
            const size_t len = d[i] | (d[i + 1] << 8);
            if (len >= 1 && len <= 200 && i + 2 + len <= n)
            {
                bool printable = true;
                for (size_t k = 0; k < len && printable; ++k)
                    printable = d[i + 2 + k] >= 32 && d[i + 2 + k] < 127;
                if (printable)
                {
                    out.emplace_back(reinterpret_cast<const char *>(d.data() + i + 2), len);
                    i += 2 + len;
                    continue;
                }
            }
            ++i;
        }
        return out;
    }

    bool decodeTex1(const std::vector<uint8_t> &b, int &w, int &h, std::vector<uint8_t> &rgba, std::string &error)
    {
        // carpics.py (SAVE-FORMAT.md 8.2): "Tex1", +0x14 u16 textures, +0x16 u16 uploads, +0x18 texture records (TEX0 first),
        // +0x1C upload records (u32 data, u16 GS block, u8 buffer width / 64, u8 PSM, u16 width, u16 height). The indices are
        // linear; a 256-colour palette in CSM1 order swaps entries 8-15 and 16-23 of every 32. PS2 alpha 0x80 = opaque.
        if (b.size() < 0x20 || std::memcmp(b.data(), "Tex1", 4) != 0)
        {
            error = "not a GT4 texture set";
            return false;
        }
        const uint32_t nu = le16(b.data() + 0x16), ta = le32(b.data() + 0x18), ua = le32(b.data() + 0x1C);
        if (ta + 8 > b.size() || ua + 12ull * nu > b.size() || nu == 0 || nu > 64)
        {
            error = "a damaged texture set";
            return false;
        }
        const uint64_t tex0 = uint64_t(le32(b.data() + ta)) | (uint64_t(le32(b.data() + ta + 4)) << 32);
        const uint32_t psm = static_cast<uint32_t>((tex0 >> 20) & 0x3F), cbp = static_cast<uint32_t>((tex0 >> 37) & 0x3FFF);
        struct Up
        {
            uint32_t off;
            uint16_t block;
            uint8_t psm;
            uint16_t w, h;
        };
        std::vector<Up> ups;
        for (uint32_t k = 0; k < nu; ++k)
        {
            const uint8_t *p = b.data() + ua + 12 * k;
            ups.push_back({le32(p), le16(p + 4), p[7], le16(p + 8), le16(p + 10)});
        }
        const Up *img = nullptr, *clut = nullptr;
        for (const Up &u : ups)
        {
            if (!img && u.psm == psm)
                img = &u;
            if (!clut && u.block == cbp)
                clut = &u;
        }
        if (!img || !clut || clut->psm != 0 || (psm != 0x13 && psm != 0x14))
        {
            error = "a texture kind the launcher does not read";
            return false;
        }
        const size_t n = size_t(clut->w) * clut->h;
        if (n == 0 || n > 256 || clut->off + 4 * n > b.size())
        {
            error = "a damaged palette";
            return false;
        }
        std::vector<std::array<uint8_t, 4>> pal(n);
        for (size_t i = 0; i < n; ++i)
            std::memcpy(pal[i].data(), b.data() + clut->off + 4 * i, 4);
        if (n == 256)
        {
            std::vector<std::array<uint8_t, 4>> sw(pal);
            for (size_t i = 0; i < 256; ++i)
            {
                const size_t j = (i & ~size_t(0x18)) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
                sw[i] = pal[j];
            }
            pal = sw;
        }
        w = img->w;
        h = img->h;
        const size_t bytes = psm == 0x13 ? size_t(w) * h : (size_t(w) * h + 1) / 2;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || img->off + bytes > b.size())
        {
            error = "a damaged picture";
            return false;
        }
        rgba.assign(size_t(w) * h * 4, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t p = size_t(y) * w + x;
                size_t idx = 0;
                if (psm == 0x13)
                    idx = b[img->off + p];
                else
                {
                    const uint8_t v = b[img->off + p / 2];
                    idx = (p & 1) ? (v >> 4) : (v & 15);
                }
                const std::array<uint8_t, 4> &c = pal[std::min(idx, n - 1)];
                uint8_t *o = rgba.data() + p * 4;
                o[0] = c[0];
                o[1] = c[1];
                o[2] = c[2];
                o[3] = static_cast<uint8_t>(std::min(255, c[3] * 2));
            }
        return true;
    }

    CarCatalogue::~CarCatalogue() = default;

    std::shared_ptr<CarCatalogue> CarCatalogue::load(const fs::path &iso, const GameNames *names, std::string &error)
    {
        auto cat = std::make_shared<CarCatalogue>();
        cat->disc_ = gt4save::Gt4Disc::open(iso, error);
        if (!cat->disc_)
            return nullptr;
        std::string err;
        if (!cat->disc_->read("piece/car_pict/disable.img", cat->placeholder_, err))
            cat->placeholder_.clear(); // GT4's stand-in picture (CarCatalogue::picture skips it)
        err.clear();
        auto table = [&](const char *name, GtdbTable &t) {
            std::vector<uint8_t> file;
            return cat->disc_->read(std::string(kSpec) + name + ".dbt", file, err) && decodeGtdb(file, t, err);
        };
        GtdbTable generic, defaults, engine, chassis, drivetrain, front, rear, tiresize;
        if (!table("GENERIC_CAR", generic) || !table("DEFAULT_PARTS", defaults))
        {
            error = "the disc's car table could not be read" + (err.empty() ? std::string() : ": " + err);
            return nullptr;
        }
        table("ENGINE", engine);
        table("CHASSIS", chassis);
        table("DRIVETRAIN", drivetrain);
        table("FRONTTIRE", front);
        table("REARTIRE", rear);
        table("TIRESIZE", tiresize);
        auto row = [](const GtdbTable &t, uint32_t code) -> const std::vector<uint8_t> * {
            const int i = t.firstRow(code);
            return i >= 0 && !t.rows[static_cast<size_t>(i)].empty() ? &t.rows[static_cast<size_t>(i)] : nullptr;
        };
        // The makers' names (GT4's text, "TunerName"), matched against the start of each car's full name.
        std::vector<std::string> makers;
        {
            std::vector<uint8_t> file;
            RText rt;
            if (cat->disc_->read("projects/gtmode/US/rtext.rt2", file, err) && decodeRText(file, rt, err))
            {
                const auto it = rt.find("TunerName");
                if (it != rt.end())
                    for (const auto &[k, v] : it->second)
                        makers.push_back(displayText(v));
            }
        }
        std::sort(makers.begin(), makers.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
        const std::map<uint32_t, std::string> *labels = nullptr;
        if (names && names->spec)
        {
            const auto it = names->spec->labels.find(0);
            if (it != names->spec->labels.end())
                labels = &it->second;
        }
        for (size_t i = 0; i < generic.codes.size(); ++i)
        {
            const std::vector<uint8_t> &g = generic.rows[i];
            if (g.size() < 24)
                continue;
            DiscCar c;
            c.code = generic.codes[i];
            if (labels)
            {
                const auto l = labels->find(c.code);
                if (l != labels->end())
                    c.label = l->second;
            }
            if (names)
            {
                c.name = names->carName(c.code);
                c.fullName = names->carFullName(c.code);
            }
            c.price = le32(g.data() + 8);
            c.year = le16(g.data() + 12);
            c.raceCar = g[18] != 0;
            c.tuner = g[16];
            for (const std::string &m : makers)
                if (!m.empty() && c.fullName.size() > m.size() && iequals(std::string_view(c.fullName).substr(0, m.size()), m) && c.fullName[m.size()] == ' ')
                {
                    c.maker = m;
                    break;
                }
            if (c.maker.empty() && !c.fullName.empty())
                c.maker = c.fullName.substr(0, c.fullName.find(' '));
            // DEFAULT_PARTS: 26 entries (code, table) for tables 1..26 [C: SAVE-FORMAT.md 8, rows read on the disc].
            if (le32(g.data() + 4) == 36)
                if (const std::vector<uint8_t> *dp = row(defaults, le32(g.data())))
                {
                    auto part = [&](uint32_t tableId) -> uint32_t {
                        const size_t at = 8 * size_t(tableId - 1);
                        if (at + 8 > dp->size() || le32(dp->data() + at + 4) != tableId)
                            return 0xFFFFFFFFu;
                        return le32(dp->data() + at);
                    };
                    if (const std::vector<uint8_t> *e = row(engine, part(12)); e && e->size() >= 0x1E)
                    {
                        c.powerPs = le16(e->data() + 0x1A);
                        c.torqueX100 = le16(e->data() + 0x1C);
                        c.specs = true;
                    }
                    if (const std::vector<uint8_t> *ch = row(chassis, part(6)); ch && ch->size() >= 8)
                    {
                        c.lengthMm = le16(ch->data());
                        c.heightMm = le16(ch->data() + 2);
                        c.wheelbaseMm = le16(ch->data() + 4);
                        c.weightKg = le16(ch->data() + 6);
                        if (ch->size() > 28 && (*ch)[28] > 0 && (*ch)[28] < 100)
                            c.frontWeightPct = (*ch)[28];
                    }
                    if (const std::vector<uint8_t> *dt = row(drivetrain, part(10)); dt && dt->size() > 16)
                        c.drivetrain = (*dt)[16] <= 3 ? (*dt)[16] : -1;
                    auto tyre = [&](const GtdbTable &t, uint32_t code, DiscCar::Tyre &out) {
                        if (const std::vector<uint8_t> *r = row(t, code); r && r->size() >= 8 && le32(r->data() + 4) == 31)
                            if (const std::vector<uint8_t> *s = row(tiresize, le32(r->data())); s && s->size() >= 4)
                            {
                                out.aspect = (*s)[0] * 5;
                                out.rim = (*s)[2] / 10;
                                out.width = (*s)[3] * 5;
                            }
                    };
                    tyre(front, part(25), c.front);
                    tyre(rear, part(26), c.rear);
                }
            if (!c.label.empty())
                cat->byLabel_[c.label] = c.code;
            cat->cars_[c.code] = std::move(c);
        }
        // GT4's prize table and its dealers (projects/gtmode/US_carlist.adc): "<name>_carlist" lists. The makers' new-car
        // lists are the ones with a dealer page (projects/gtmode/US/<maker>.gpb); PresentData is five parallel lists.
        {
            std::vector<uint8_t> file;
            if (cat->disc_->read("projects/gtmode/US_carlist.adc", file, err))
            {
                const std::vector<std::string> t = scriptTokens(file);
                std::map<std::string, std::vector<std::string>> lists;
                std::string key;
                for (const std::string &x : t)
                {
                    if (endsWith(x, "_carlist"))
                    {
                        key = x.substr(0, x.size() - 8);
                        lists[key];
                        continue;
                    }
                    // PresentData (the prizes) and PresentTimeData (the courses the weeks add) carry on the same five
                    // lists; their own names are not list entries (PresentTimeData kept content2 one place out).
                    if (endsWith(x, "_carnum") || x == "push" || x == "NewCarData" || x == "PresentData" || x == "PresentTimeData" || endsWith(x, ".ad"))
                        continue;
                    if (!key.empty())
                        lists[key].push_back(x);
                }
                std::set<std::string> soldNew;
                for (const auto &[k, labs] : lists)
                    if (cat->disc_->has("projects/gtmode/US/" + k + ".gpb"))
                        for (const std::string &l : labs)
                            soldNew.insert(l);
                std::set<std::string> soldUsed;
                for (int week = 0; week < 100; ++week)
                {
                    std::vector<uint8_t> wf;
                    if (cat->disc_->read(strf("usedcar/US_used%04d.adc", week), wf, err))
                        for (const std::string &x : scriptTokens(wf))
                            if (cat->byLabel_.count(x))
                                soldUsed.insert(x);
                }
                const std::vector<std::string> &cond = lists["condition"], &detail = lists["detail"], &type = lists["type"], &content = lists["content"],
                                               &content2 = lists["content2"];
                const size_t n = std::min({cond.size(), detail.size(), type.size(), content.size()});
                cat->prizeRows_ = n;
                std::map<std::string, size_t> at;
                for (size_t i = 0; i < n; ++i)
                    cat->presentRows_.push_back({cond[i], detail[i], type[i], content[i], i < content2.size() ? content2[i] : std::string("-")});
                for (size_t i = 0; i < n; ++i)
                {
                    if (type[i] != "car" || !cat->byLabel_.count(content[i]))
                        continue;
                    const std::string &l = content[i];
                    if (!at.count(l))
                    {
                        PrizeCar p;
                        p.label = l;
                        p.code = cat->byLabel_[l];
                        p.soldNew = soldNew.count(l) != 0;
                        p.soldUsed = soldUsed.count(l) != 0;
                        at[l] = cat->prizes_.size();
                        cat->prizes_.push_back(std::move(p));
                    }
                    cat->prizes_[at[l]].given.push_back({cond[i], detail[i], i < content2.size() ? content2[i] : std::string("-")});
                }
            }
        }
        if (cat->cars_.empty())
        {
            error = "the disc has no cars";
            return nullptr;
        }
        return cat;
    }

    const DiscCar *CarCatalogue::car(uint64_t code64) const
    {
        if ((code64 >> 32) != 0)
            return nullptr;
        const auto it = cars_.find(static_cast<uint32_t>(code64));
        return it == cars_.end() ? nullptr : &it->second;
    }

    const DiscCar *CarCatalogue::carByLabel(const std::string &label) const
    {
        const auto it = byLabel_.find(label);
        return it == byLabel_.end() ? nullptr : car(it->second);
    }

    bool CarCatalogue::picture(const std::string &label, int &w, int &h, std::vector<uint8_t> &rgba, std::string *usedLabel) const
    {
        if (!disc_ || label.empty())
            return false;
        // The car's picture name, its own label, then the label with one of its model words left out (the year stays).
        std::vector<std::string> tries;
        if (const char *p = pictureName(label))
            tries.push_back(p);
        tries.push_back(label);
        const std::vector<std::string> parts = split(label, '_');
        for (size_t drop = parts.size() >= 2 ? parts.size() - 2 : 0; drop >= 1 && drop < parts.size(); --drop)
        {
            std::vector<std::string> p = parts;
            p.erase(p.begin() + static_cast<std::ptrdiff_t>(drop));
            tries.push_back(join(p, "_"));
        }
        for (const std::string &t : tries)
        {
            const std::string path = "piece/car_pict/" + t + ".img";
            if (!disc_->has(path))
                continue;
            std::vector<uint8_t> file;
            std::string err;
            if (!disc_->read(path, file, err) || (!placeholder_.empty() && file == placeholder_) || !decodeTex1(file, w, h, rgba, err))
                continue;
            // An empty picture: every pixel (near) transparent (GT4's pictures keep alpha 2 of 128 around the car).
            bool empty = true;
            for (size_t i = 3; i < rgba.size() && empty; i += 4)
                empty = rgba[i] < 16;
            if (empty)
                continue;
            if (usedLabel)
                *usedLabel = t;
            return true;
        }
        return false;
    }

    bool CarCatalogue::usedLotHas(int week, const std::string &label) const
    {
        auto it = usedWeeks_.find(week);
        if (it == usedWeeks_.end())
        {
            std::set<std::string> s;
            std::vector<uint8_t> file;
            std::string err;
            if (disc_ && disc_->read(strf("usedcar/US_used%04d.adc", week), file, err))
                for (const std::string &t : scriptTokens(file))
                    s.insert(t);
            it = usedWeeks_.emplace(week, std::move(s)).first;
        }
        return it->second.count(label) != 0;
    }

    std::string prizeReason(const PrizeRow &r, const GameNames *names)
    {
        static const std::pair<const char *, const char *> lic[] = {{"l0b", "B"}, {"l0a", "A"}, {"lib", "IB"}, {"lia", "IA"}, {"l0s", "S"}};
        auto licence = [&](const std::string &k) {
            for (const auto &[pre, name] : lic)
                if (k == pre)
                    return std::string(name) + " licence";
            return k;
        };
        if (r.condition == "win")
        {
            // "am_sunday_0000" -> the series "am_sunday" -> GT4's event name.
            std::string series = r.detail;
            const size_t u = series.rfind('_');
            if (u != std::string::npos)
                series = series.substr(0, u);
            std::string name = names ? names->eventName(series) : std::string();
            if (name.empty())
                name = series;
            return name + ": win it";
        }
        if (r.condition == "license")
            return licence(r.detail) + ": pass it";
        if (r.condition == "all_silver")
            return licence(r.detail) + ": every test silver or better";
        if (r.condition == "all_gold")
            return licence(r.detail) + ": every test gold";
        if (r.condition == "mission_1st")
        {
            const std::vector<std::string> p = split(r.detail, ',');
            return p.size() == 2 ? "Missions " + p[0] + "-" + p[1] + ": win them all" : "Missions " + r.detail;
        }
        if (r.condition == "achievement")
            return "Game completion " + r.detail + "%";
        return r.condition + " " + r.detail;
    }

    // ------------------------------------------------------------------ estimates
    PerformanceEstimate estimatePerformance(double powerPs, double weightKg, int drivetrain, bool racingTyres)
    {
        PerformanceEstimate e;
        if (powerPs < 10 || weightKg < 200)
            return e;
        // A plain model, not GT4's physics: the engine's power less drivetrain losses (85% at the wheels), limited by the
        // grip of the driven tyres (their share of the car's weight while accelerating), against air drag and rolling
        // resistance; a driver of 75 kg; 0.25 s per gear change, five changes to 200 km/h and one more on the way to 300.
        const double g = 9.81, rho = 1.2, m = weightKg + 75.0;
        const double P = powerPs * 735.5 * 0.85;
        const double mu = racingTyres ? 1.45 : 1.05;
        // The driven wheels' share of the weight while accelerating (weight moves back): 4WD, FF, MR, FR.
        const double share = drivetrain == 2 ? 1.0 : drivetrain == 1 ? 0.56 : drivetrain == 3 ? 0.68 : 0.62;
        const double cda = racingTyres ? 0.95 : 0.70;  // drag area (m2): racing cars carry downforce, and its drag
        const double crr = 0.013;
        double v = 0.0, t = 0.0;
        const double dt = 0.002;
        int shifts = 0;
        const double shiftAt[6] = {50, 90, 130, 165, 195, 240}; // km/h
        while (t < 120.0)
        {
            const double kmh = v * 3.6;
            if (e.zeroTo100 == 0 && kmh >= 100)
                e.zeroTo100 = t;
            if (e.zeroTo200 == 0 && kmh >= 200)
                e.zeroTo200 = t;
            if (kmh >= 300)
            {
                e.zeroTo300 = t;
                break;
            }
            while (shifts < 6 && kmh >= shiftAt[shifts])
            {
                t += 0.25;
                ++shifts;
            }
            const double drive = std::min(P / std::max(v, 1.0), mu * g * m * share);
            const double resist = 0.5 * rho * cda * v * v + crr * m * g;
            const double a = (drive - resist) / m;
            if (a <= 0.0005)
                break;
            v += a * dt;
            t += dt;
        }
        // Top speed: where the power at the wheels meets the drag (no gearing limit: the cars' gears are not read).
        double lo = 1.0, hi = 150.0;
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (P > mid * (0.5 * rho * cda * mid * mid + crr * m * g))
                lo = mid;
            else
                hi = mid;
        }
        e.topSpeedKmh = lo * 3.6;
        const double v100 = 100.0 / 3.6, brakeMu = racingTyres ? 1.35 : 0.95;
        e.brake100 = v100 * v100 / (2.0 * brakeMu * g);
        e.brake200 = 4.0 * e.brake100; // the same grip from twice the speed (air drag and downforce left out)
        e.ok = e.zeroTo100 > 0;
        return e;
    }

    // ------------------------------------------------------------------ the garage
    const GarageCar *Garage::bySlot(int slot) const
    {
        for (const GarageCar &c : cars)
            if (c.slot == slot)
                return &c;
        return nullptr;
    }

    Garage buildGarage(const gt4save::Save &save, const GameNames *names, const CarCatalogue *catalogue)
    {
        Garage out;
        const SaveDetails d = buildDetails(save, names);
        if (!save.loaded)
        {
            out.problem = d.problems.empty() ? std::string("This save cannot be read.") : d.problems.front().title;
            return out;
        }
        out.loaded = true;
        out.player = d.player;
        out.credits = d.credits;
        out.hasCredits = d.hasCredits;
        out.gameDay = d.gameDay;
        const gt4save::Fields &f = save.fields;
        const std::vector<uint8_t> &plain = save.dec.gamedataPlain;
        for (const DetailCar &dc : d.cars)
        {
            GarageCar c;
            c.slot = dc.slot;
            c.code = static_cast<uint32_t>(dc.code);
            c.name = dc.name;
            c.fullName = dc.fullName.empty() ? dc.name : dc.fullName;
            c.colourName = dc.colourName;
            c.hasRgb = dc.hasRgb;
            c.r = dc.r;
            c.g = dc.g;
            c.b = dc.b;
            c.colourIndex = dc.colourIndex;
            c.odometerM = dc.odometerM;
            c.powerPs = dc.power;
            c.weightKg = dc.weightKg;
            c.acquired = dc.acquired;
            c.dayAdded = dc.dayAdded;
            c.prize = dc.prize;
            c.parts = dc.parts;
            c.partsNote = dc.partsNote;
            c.disc = catalogue ? catalogue->car(dc.code) : nullptr;
            if (c.disc)
            {
                c.maker = c.disc->maker;
                c.label = c.disc->label;
            }
            for (const std::string &p : dc.parts)
                if (c.tyreType.empty() && (startsWith(p, "Front: ") || endsWith(p, ", front and rear")))
                {
                    std::string t = startsWith(p, "Front: ") ? p.substr(7) : p.substr(0, p.size() - 16);
                    c.tyreType = t;
                }
            // The car record's bytes: the riding car in GAMEDATA, a stored car in its garage slot (SAVE-FORMAT.md 4).
            const uint8_t *rec = nullptr;
            if (dc.riding && plain.size() >= 0x135FC + 1208)
            {
                rec = plain.data() + 0x135FC;
                c.kind = 2;
            }
            else if (dc.fullRecord && save.dec.slotPlain.count(dc.slot) && save.dec.slotPlain.at(dc.slot).second.size() >= 1208)
            {
                rec = save.dec.slotPlain.at(dc.slot).second.data();
                c.kind = 3;
            }
            else
                c.kind = 1;
            if (rec)
            {
                const float oil = lef(rec + 0x4A0), dirt = lef(rec + 0x4A4), body = lef(rec + 0x4A8);
                c.condition = true;
                c.oilAgeKm = (oil < 0 ? double(oil) + 300000.0 : double(oil)) / 1000.0;
                c.oilDue = oil >= 5000000.0f;
                c.dirt01 = std::clamp(double(dirt) / 0.8, 0.0, 1.0);
                c.bodyWearKm = std::max(0.0, double(body)) / 1000.0;
                c.frozen = rec[0x4AC] != 0;
                c.rigidity = rec[0x168] != 0 && rec[376 + 0x168] != 0 && rec[752 + 0x168] != 0;
                out.fullRecords++;
            }
            else
            {
                // A car GT4 builds from the index when it is first taken out: oil and body wear = its odometer, capped
                // (0x440fb8), dirt 0 [C, SAVE-FORMAT.md 5].
                c.oilAgeKm = std::min<double>(static_cast<double>(dc.odometerM), 6000000.0) / 1000.0;
                c.oilDue = dc.odometerM >= 5000000u;
                c.dirt01 = 0;
                c.bodyWearKm = std::min<double>(static_cast<double>(dc.odometerM), 3000000.0) / 1000.0;
            }
            out.oilDue += c.oilDue;
            out.needsWash += c.dirt01 >= kWashFrom;
            out.withRigidity += c.rigidity;
            const bool racing = (c.disc && c.disc->raceCar) || startsWith(lower(c.tyreType), "racing");
            c.est = estimatePerformance(c.powerPs > 0 ? c.powerPs : (c.disc ? c.disc->powerPs : 0), c.weightKg > 0 ? c.weightKg : (c.disc ? c.disc->weightKg : 0),
                                        c.disc ? c.disc->drivetrain : 0, racing);
            out.cars.push_back(std::move(c));
        }
        (void)f;
        return out;
    }

    // ------------------------------------------------------------------ Game progress
    std::string lapTime(uint32_t ms)
    {
        if (ms == 0 || ms >= gt4save::kNoTime)
            return "-";
        const uint32_t m = ms / 60000, s = (ms / 1000) % 60, r = ms % 1000;
        return m ? strf("%u:%02u.%03u", m, s, r) : strf("%u.%03u", s, r);
    }

    namespace
    {
        std::string seriesOf(const std::string &raceLabel)
        {
            const size_t u = raceLabel.rfind('_');
            if (u == std::string::npos || u + 1 >= raceLabel.size())
                return raceLabel;
            for (size_t i = u + 1; i < raceLabel.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(raceLabel[i])))
                    return raceLabel;
            return raceLabel.substr(0, u);
        }
        // GT4's event halls by the race code's prefix (as Save details groups them, gt4stock_savedetails.cpp).
        std::string hallKey(const std::string &series)
        {
            static const std::pair<const char *, const char *> halls[] = {
                {"am_", "BeginnerHall"}, {"pr_", "ProfessionalHall"}, {"ex_", "ExtremeHall"}, {"en_", "EnduranceHall"},
                {"rh_", "RallyHall"},    {"jpr", "RaceJapanese"},     {"usr", "RaceAmerican"}, {"eur", "RaceEuropean"}};
            for (const auto &[pre, key] : halls)
                if (startsWith(series, pre))
                    return key;
            return std::string();
        }
        int hallOrder(const std::string &key)
        {
            static const char *const order[] = {"BeginnerHall", "ProfessionalHall", "ExtremeHall", "EnduranceHall", "RaceJapanese",
                                                "RaceAmerican", "RaceEuropean", "RallyHall"};
            for (int i = 0; i < 8; ++i)
                if (key == order[i])
                    return i;
            return 8;
        }
        std::string makerHall(const GameNames &n, const std::string &series)
        {
            const std::vector<std::string> parts = split(series, '_');
            if (parts.size() < 2 || parts[0].size() != 2)
                return std::string();
            static const std::pair<const char *, const char *> alias[] = {
                {"vw", "volkswagen"}, {"chev", "chevrolet"}, {"chry", "chrysler"}, {"alfa", "alfaromeo"}, {"aston", "astonmartin"},
                {"bmw1", "bmw"},      {"mg", "mg_mini"},      {"mercedes", "mercedes"}};
            std::string maker = parts[1];
            for (const auto &[a, b] : alias)
                if (maker == a)
                    maker = b;
            return n.text("GTmodeProject::EventHallRoot", "INFO|Race_" + maker);
        }
        char medalOf(const std::string &r) { return r == "gold" ? 'G' : r == "silver" ? 'S' : r == "bronze" ? 'B' : r == "red" ? 'P' : '-'; }
    }

    Progress buildProgress(const gt4save::Save &save, const GameNames *names)
    {
        Progress p;
        p.details = buildDetails(save, names);
        if (!save.loaded)
            return p;
        p.loaded = true;
        const gt4save::Fields &f = save.fields;
        p.distanceM = f.profile.aspecDistance + f.profile.bspecDistance;
        p.races = f.profile.racesTotal;
        p.wins = f.profile.wins;
        // GT4's completion rate, from the disc's own event lists (hidden when they cannot be read).
        if (names && names->spec && !names->source.empty())
            if (const std::shared_ptr<const CompletionList> cl = completionListForDisc(names->source, *names->spec))
                p.completion = completionOf(save, *cl);
        std::map<std::string, const gt4save::LicenceUnit *> times;
        for (const gt4save::LicenceUnit &u : f.licenseRecord)
            times[u.test] = &u;
        auto best = [&](const std::string &test) -> uint32_t {
            const auto it = times.find(test);
            if (it == times.end() || it->second->entries.empty())
                return 0;
            uint32_t b = gt4save::kNoTime;
            for (const gt4save::LicenceEntry &e : it->second->entries)
                b = std::min(b, e.timeMs);
            return b == gt4save::kNoTime ? 0 : b;
        };
        if (save.licences)
        {
            static const std::pair<const char *, const char *> keys[] = {{"B", "l0b"}, {"A", "l0a"}, {"IB", "lib"}, {"IA", "lia"}, {"S", "l0s"}};
            for (const gt4save::LicenceStatus::Licence &l : save.licences->licences)
            {
                ProgressLicence pl;
                pl.key = l.name;
                std::string prefix;
                for (const auto &[k, pre] : keys)
                    if (l.name == k)
                        prefix = pre;
                pl.name = names && !prefix.empty() ? names->licenceText(prefix + "0000") : std::string();
                if (pl.name.empty())
                    pl.name = l.name + " licence";
                for (size_t i = 0; i < 16; ++i)
                {
                    ProgressTest t;
                    t.key = l.name + "-" + std::to_string(i + 1);
                    t.medal = medalOf(l.tests[i]);
                    t.name = names && !prefix.empty() ? names->licenceText(prefix + strf("%04zu", i + 1)) : std::string();
                    t.bestMs = best(t.key);
                    pl.done += t.medal != '-';
                    pl.tests.push_back(t);
                }
                pl.passed = l.passed;
                pl.allGold = l.allGold;
                pl.gold = l.gold;
                pl.silver = l.silver;
                pl.bronze = l.bronze;
                p.licences.push_back(pl);
            }
            for (size_t i = 0; i < save.licences->missions.size(); ++i)
            {
                ProgressTest t;
                t.key = "mission-" + std::to_string(i + 1);
                t.medal = save.licences->missions[i] == "none" ? '-' : medalOf(save.licences->missions[i]) == '-' ? 'P' : medalOf(save.licences->missions[i]);
                t.name = names ? names->licenceText(strf("l0m%04zu", i + 1)) : std::string();
                t.bestMs = best(t.key);
                p.missions.push_back(t);
            }
            for (size_t i = 0; i < save.licences->coffeeBreaks.size(); ++i)
            {
                ProgressTest t;
                t.key = "coffee-" + std::to_string(i + 1);
                t.medal = save.licences->coffeeBreaks[i] == "none" ? '-' : medalOf(save.licences->coffeeBreaks[i]) == '-' ? 'P' : medalOf(save.licences->coffeeBreaks[i]);
                t.name = names ? names->licenceText(strf("l0c%04zu", i + 1)) : std::string();
                t.bestMs = best(t.key);
                p.coffee.push_back(t);
            }
        }
        // Every event series on the disc (RACE.dbt), with the save's result for each race GT4 recorded.
        const gt4save::SpecDb *spec = names ? names->spec.get() : nullptr;
        if (spec && !spec->raceRows.empty())
        {
            std::map<int, const gt4save::RaceUnit *> units;
            for (const gt4save::RaceUnit &u : f.raceRecord)
                units[u.row] = &u;
            std::map<std::string, ProgressEvent> bySeries;
            for (size_t row = 0; row < spec->raceRows.size(); ++row)
            {
                const std::optional<std::string> l = spec->raceLabel(static_cast<int>(row));
                if (!l)
                    continue;
                const std::string se = seriesOf(*l);
                const std::string hk = hallKey(se);
                const std::string mh = hk.empty() && names ? makerHall(*names, se) : std::string();
                if (hk.empty() && mh.empty())
                    continue; // licences, missions, coffee breaks, samples and previews are not events
                ProgressEvent &e = bySeries[se];
                if (e.series.empty())
                {
                    e.series = se;
                    e.firstRow = static_cast<int>(row);
                    e.name = names ? names->eventName(se) : std::string();
                    if (e.name.empty())
                        e.name = se;
                    e.hall = hk.empty() ? mh : (names ? names->text("GTmodeProject::EventHallRoot", hk) : hk);
                    e.hallOrder = hallOrder(hk);
                }
                ProgressRace r;
                r.row = static_cast<int>(row);
                r.label = *l;
                const auto u = units.find(static_cast<int>(row));
                if (u != units.end() && u->second->typeRaw() == 2)
                {
                    r.result = static_cast<int>(u->second->best());
                    r.score = static_cast<int>(u->second->bestScore());
                    r.run = r.result <= 7;
                }
                e.races.push_back(r);
                e.done += r.run;
                e.won += r.run && r.result == 0;
                e.points += r.score;
                p.eventRaces++;
                p.eventRacesWon += r.run && r.result == 0;
            }
            for (auto &[k, e] : bySeries)
                p.events.push_back(std::move(e));
            std::stable_sort(p.events.begin(), p.events.end(), [](const ProgressEvent &a, const ProgressEvent &b) {
                return a.hallOrder != b.hallOrder ? a.hallOrder < b.hallOrder : a.firstRow < b.firstRow;
            });
        }
        // Arcade: GT4's course records (best score and time rankings), its machine records, the cars and courses it lists
        // as available, the favourites.
        auto course = [&](const gt4save::CourseUnit &u) {
            ProgressCourse c;
            const std::optional<std::string> lab = u.course && spec ? spec->label(*u.course) : std::nullopt;
            c.label = lab ? *lab : (u.course ? strf("#%llu", static_cast<unsigned long long>(*u.course & 0xFFFFFFFFu)) : std::string());
            c.name = names && lab ? names->text("CourseName", *lab) : std::string();
            if (c.name.empty())
                c.name = c.label;
            c.bestScore = u.bestScore;
            c.entries = static_cast<int>(u.entries.size());
            const gt4save::CourseEntry *b = nullptr;
            for (const gt4save::CourseEntry &e : u.entries)
                if (e.value != 0 && e.value != gt4save::kNoTime && (!b || e.value < b->value))
                    b = &e;
            if (b)
            {
                c.bestMs = b->value;
                c.car = b->car && names ? names->carName(*b->car) : std::string();
                c.date = b->year ? strf("%d-%02d-%02d", b->year, b->month, b->day) : std::string();
                c.driver = b->name;
            }
            return c;
        };
        for (const gt4save::CourseUnit &u : f.courseRecord)
            if (u.course && (u.bestScore > 0 || !u.entries.empty()))
            {
                p.courses.push_back(course(u));
                p.arcadeScoreTotal += u.bestScore;
            }
        for (const gt4save::CourseUnit &u : f.machineRecord)
            if (u.course && !u.entries.empty())
                p.machineRecords.push_back(course(u));
        p.availableCars = f.available.cars.size();
        p.availableCourses = f.available.courses.size();
        for (uint64_t c : f.favoriteCar.codes)
            p.favouriteCars.push_back(names ? names->carName(c) : strf("#%llu", static_cast<unsigned long long>(c & 0xFFFFFFFFu)));
        for (uint64_t c : f.favoriteCourse.codes)
        {
            const std::optional<std::string> lab = spec ? spec->label(c) : std::nullopt;
            const std::string n = names && lab ? names->text("CourseName", *lab) : std::string();
            p.favouriteCourses.push_back(!n.empty() ? n : lab ? *lab : strf("#%llu", static_cast<unsigned long long>(c & 0xFFFFFFFFu)));
        }
        return p;
    }

    // ------------------------------------------------------------------ GT Mode prizes
    std::string guideKey(const PrizeRow &r)
    {
        if (r.condition == "win")
            return "win:" + seriesOf(r.detail);
        return r.condition + ":" + r.detail;
    }

    std::vector<GuideGroup> buildPrizeGuide(const CarCatalogue &cat, const GameNames *names, const Progress *progress)
    {
        auto text = [&](const char *c, const std::string &k) { return names ? names->text(c, k) : std::string(); };
        // GT4's prize texts (GTmodeProject::PresentRoot) without their "[NOTE] this prize is available to win just once."
        auto present = [&](const std::string &k) {
            std::string s = text("GTmodeProject::PresentRoot", k);
            const size_t n = s.find("[NOTE]");
            if (n != std::string::npos)
                s = s.substr(0, n);
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
            // GT4 words them as its message on winning ("You have won %s"); the guide says when it is given.
            if (s.rfind("You have ", 0) == 0)
                s = "Given when you have " + s.substr(9);
            return s;
        };
        auto fill = [](std::string s, const std::string &v) {
            const size_t p = s.find("%s");
            return p == std::string::npos ? s : s.replace(p, 2, v);
        };
        // A readable name from GT4's key when GT4 has no text for it ("jp_toyota_vitz" -> "Toyota Vitz").
        auto fromKey = [](const std::string &key) {
            std::vector<std::string> parts = split(key, '_');
            if (parts.size() > 1 && parts[0].size() <= 3)
                parts.erase(parts.begin());
            for (std::string &p : parts)
                if (!p.empty())
                    p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
            return join(parts, " ");
        };
        // The groups in GT4's map order (GTmodeProject::TopRoot names), then the licences, missions, completion and weeks.
        struct Def
        {
            const char *key, *top, *info, *need;
        };
        static const Def defs[] = {
            {"am_", "Beginner Hall", "INFO|BeginnerHall", nullptr},
            {"pr_", "Professional Hall", "INFO|ProfessionalHall", nullptr},
            {"ex_", "Extreme Hall", "INFO|ExtremeHall", "you cannot play Extreme yet"},
            {"usr", "American Race", "INFO|RaceAmerican", nullptr},
            {"eur", "European Race", "INFO|RaceEuropean", nullptr},
            {"jpr", "Japanese Race", "INFO|RaceJapanese", nullptr},
            {"rh_", "Rally", "INFO|RallyHall", nullptr},
            {"en_", "Endurance", "INFO|EnduranceHall", "you cannot play Endurance yet"},
            {"maker", nullptr, nullptr, nullptr},
            {"license", "License", nullptr, nullptr},
            {"mission", "Mission", "INFO|MissionImpossibleHall", nullptr},
            {"achievement", nullptr, nullptr, nullptr},
            {"weeks", nullptr, nullptr, nullptr},
        };
        std::vector<GuideGroup> groups;
        for (const Def &d : defs)
        {
            GuideGroup g;
            g.key = d.key;
            g.title = d.top ? text("GTmodeProject::TopRoot", d.top) : std::string();
            if (g.title.empty())
            {
                g.gt4Title = false;
                g.title = g.key == "maker"         ? std::string("Car-maker events (at the dealers)")
                          : g.key == "achievement" ? std::string("Game completion")
                          : g.key == "weeks"       ? std::string("New courses as the weeks pass")
                          : d.top                  ? std::string(d.top)
                                                   : g.key;
            }
            std::string note = d.info ? text("GTmodeProject::EventHallRoot", d.info) : std::string();
            if (d.need)
            {
                const std::string n = text("GTmodeProject::TopRoot", d.need);
                if (!n.empty())
                    note += (note.empty() ? "" : " ") + n;
            }
            if (g.key == "maker")
                note = "Each car maker's own events, entered at its dealer (GT4's showrooms), by country.";
            else if (g.key == "weeks")
                note = "GT4 adds these courses to the game as GT Mode's weeks go by (its PresentTimeData list): the week each one comes.";
            else if (g.key == "achievement")
                note = "GT4's prizes for the game completion rate (Game Status).";
            g.note = note;
            groups.push_back(std::move(g));
        }
        auto group = [&](const std::string &key) -> GuideGroup & {
            for (GuideGroup &g : groups)
                if (g.key == key)
                    return g;
            return groups.back();
        };
        auto groupOfSeries = [&](const std::string &se) -> std::string {
            for (const Def &d : defs)
                if (std::strlen(d.key) == 3 && startsWith(se, d.key))
                    return d.key;
            return "maker";
        };
        // The country of a car maker's event, by GT4's race-code prefix, as GT4's map names it.
        auto country = [&](const std::string &se) {
            static const std::pair<const char *, const char *> c[] = {{"jp_", "Japanese Car"}, {"kr_", "Korean Car"}, {"us_", "American Car"},
                                                                      {"uk_", "British Car"},  {"de_", "Germany Car"}, {"fr_", "French Car"},
                                                                      {"it_", "Italian Car"}};
            for (const auto &[pre, k] : c)
                if (startsWith(se, pre))
                    return text("GTmodeProject::TopRoot", k);
            return std::string();
        };
        std::map<std::string, std::pair<std::string, size_t>> where; // entry key -> group key, index
        auto entry = [&](const std::string &groupKey, const std::string &key, bool &isNew) -> GuideEntry & {
            auto it = where.find(key);
            isNew = it == where.end();
            if (!isNew)
                return group(it->second.first).entries[it->second.second];
            GuideGroup &g = group(groupKey);
            where[key] = {groupKey, g.entries.size()};
            g.entries.emplace_back();
            g.entries.back().key = key;
            return g.entries.back();
        };
        auto eventEntry = [&](const std::string &se) -> GuideEntry & {
            const std::string gk = groupOfSeries(se);
            bool isNew = false;
            GuideEntry &e = entry(gk, "win:" + se, isNew);
            if (isNew)
            {
                e.name = names ? names->eventName(se) : std::string();
                if (e.name.empty())
                {
                    e.name = fromKey(se);
                    e.gt4Name = false;
                }
                if (gk == "maker" && names)
                {
                    const std::string hall = makerHall(*names, se);
                    const std::string c = country(se);
                    e.sub = hall + (!hall.empty() && !c.empty() ? " \xc2\xb7 " : "") + c;
                }
                e.how = fill(present("win"), e.name);
                if (e.how.empty())
                    e.how = "Win it";
            }
            return e;
        };
        // Every event series on the disc (RACE.dbt), in GT4's row order, so events without a prize are listed too.
        const gt4save::SpecDb *spec = names ? names->spec.get() : nullptr;
        if (spec)
            for (size_t row = 0; row < spec->raceRows.size(); ++row)
                if (const std::optional<std::string> l = spec->raceLabel(static_cast<int>(row)))
                {
                    const std::string se = seriesOf(*l);
                    if (hallKey(se).empty() && (!names || makerHall(*names, se).empty()))
                        continue; // licences, missions, coffee breaks, samples and previews
                    eventEntry(se);
                }
        // The licences: GT4's name for each ("B License", from "B License required").
        auto licenceName = [&](const std::string &k) {
            static const std::pair<const char *, const char *> m[] = {
                {"l0b", "NeedLicense_0b"}, {"l0a", "NeedLicense_0a"}, {"lib", "NeedLicense_ib"}, {"lia", "NeedLicense_ia"}, {"l0s", "NeedLicense_0s"}};
            for (const auto &[a, b] : m)
                if (k == a)
                {
                    std::string s = text("GTmodeProject::TopRoot", b);
                    const size_t r = s.rfind(" required");
                    if (r != std::string::npos)
                        s = s.substr(0, r);
                    if (!s.empty())
                        return s;
                }
            return k;
        };
        for (const PresentRow &r : cat.presentRows())
        {
            GuideEntry *e = nullptr;
            bool isNew = false;
            if (r.condition == "win")
                e = &eventEntry(seriesOf(r.detail));
            else if (r.condition == "license" || r.condition == "all_silver" || r.condition == "all_gold")
            {
                e = &entry("license", r.condition + ":" + r.detail, isNew);
                if (isNew)
                {
                    const std::string ln = licenceName(r.detail);
                    e->name = ln + (r.condition == "license" ? std::string() : r.condition == "all_gold" ? " \xc2\xb7 all gold" : " \xc2\xb7 all silver");
                    e->how = fill(present(r.condition), ln);
                }
            }
            else if (r.condition == "mission_1st")
            {
                e = &entry("mission", r.condition + ":" + r.detail, isNew);
                if (isNew)
                {
                    const std::vector<std::string> p = split(r.detail, ',');
                    const int a = p.size() == 2 ? std::atoi(p[0].c_str()) : 0, b = p.size() == 2 ? std::atoi(p[1].c_str()) : 0;
                    e->name = a && b ? strf("Missions %d-%d", a, b) : "Missions " + r.detail;
                    std::string h = present("mission_1st");
                    if (a && b && !h.empty())
                    {
                        const size_t p1 = h.find("%d");
                        if (p1 != std::string::npos)
                            h.replace(p1, 2, std::to_string(a));
                        const size_t p2 = h.find("%d");
                        if (p2 != std::string::npos)
                            h.replace(p2, 2, std::to_string(b));
                    }
                    e->how = h;
                }
            }
            else if (r.condition == "achievement")
            {
                e = &entry("achievement", r.condition + ":" + r.detail, isNew);
                if (isNew)
                {
                    e->name = "Game completion " + r.detail + "%";
                    e->gt4Name = false;
                    e->how = present("achievement_" + r.detail);
                }
            }
            else if (r.condition == "weeks")
            {
                e = &entry("weeks", "weeks:" + r.detail, isNew);
                e->name = "Week " + r.detail;
                e->gt4Name = false;
                e->how = "After " + r.detail + " weeks of GT Mode (day " + std::to_string(std::atoi(r.detail.c_str()) * 7) + ")";
            }
            else
                continue;
            GuidePrize p;
            p.kind = r.type;
            if (r.type == "car")
            {
                p.label = r.content;
                if (const DiscCar *d = cat.carByLabel(r.content))
                {
                    p.carCode = d->code;
                    p.name = !d->fullName.empty() ? d->fullName : !d->name.empty() ? d->name : d->label;
                }
                else
                    p.name = r.content;
                if (r.extra == "ending")
                    p.note = "Also unlocks the ending movie"; // GT4: "you got ending movie"
                // The car itself is fixed: one car per row, every row its own prize (analysis/design/prize-cars). Its colour:
                // content2's number (GT4's colour number, index + 1), else GT4 picks one of the car's colours at random
                // (gtmode.adc PresentRoot::CarPane get_car: col "-" -> MRandom::GetValue(the car's colour count)).
                const bool numbered = !r.extra.empty() && std::isdigit(static_cast<unsigned char>(r.extra[0]));
                std::string colour;
                if (numbered && names && p.carCode)
                {
                    p.colour = std::atoi(r.extra.c_str()) - 1;
                    const auto c = names->colour(p.carCode, p.colour);
                    // GT4's colour list names every colour but the Formula GT's black (colour 11, RGB 6, 6, 6).
                    colour = c && !c->name.empty()                               ? "Colour: " + c->name
                             : c && c->r == c->g && c->g == c->b && c->r < 48 ? "Colour: black (GT4's colour " + r.extra + ")"
                                                                              : "Colour: GT4's colour " + r.extra;
                }
                else if (!numbered && names && p.carCode && names->colourCount(p.carCode) > 1)
                    colour = "Colour picked at random";
                if (!colour.empty())
                    p.note += (p.note.empty() ? "" : " \xc2\xb7 ") + colour;
            }
            else if (r.type == "course")
            {
                p.label = r.content;
                p.name = text("CourseName", r.content);
                if (p.name.empty())
                    p.name = r.content;
                p.note = text("GTmodeProject::TopRoot", "place_" + r.extra);
            }
            else if (r.type == "paper")
            {
                p.name = "Licence certificate";
                p.note = "The licence itself (GT4 prints a certificate)";
            }
            else
                p.name = r.type + " " + r.content;
            e->prizes.push_back(std::move(p));
        }
        // The car makers' events: by country (as GT4's map lists its showrooms), then GT4's row order.
        {
            std::vector<GuideEntry> &m = group("maker").entries;
            auto rank = [](const std::string &key) {
                static const char *const pre[] = {"win:jp_", "win:kr_", "win:us_", "win:uk_", "win:de_", "win:fr_", "win:it_"};
                for (int i = 0; i < 7; ++i)
                    if (startsWith(key, pre[i]))
                        return i;
                return 7;
            };
            std::stable_sort(m.begin(), m.end(), [&](const GuideEntry &a, const GuideEntry &b) { return rank(a.key) < rank(b.key); });
        }
        // The save's results, when a save is read.
        if (progress && progress->loaded)
        {
            std::map<std::string, const ProgressEvent *> ev;
            for (const ProgressEvent &e : progress->events)
                ev[e.series] = &e;
            std::map<std::string, const ProgressLicence *> lic;
            static const std::pair<const char *, const char *> lk[] = {{"l0b", "B"}, {"l0a", "A"}, {"lib", "IB"}, {"lia", "IA"}, {"l0s", "S"}};
            for (const ProgressLicence &l : progress->licences)
                for (const auto &[k, n] : lk)
                    if (l.key == n)
                        lic[k] = &l;
            for (GuideGroup &g : groups)
                for (GuideEntry &e : g.entries)
                {
                    const size_t c = e.key.find(':');
                    const std::string cond = e.key.substr(0, c), det = e.key.substr(c + 1);
                    if (cond == "win")
                    {
                        const auto it = ev.find(det);
                        if (it == ev.end())
                            continue;
                        const int n = static_cast<int>(it->second->races.size()), w = it->second->won;
                        e.state = n > 0 && w >= n ? 2 : w > 0 ? 1 : 0;
                        e.status = n == 1 ? std::string(w ? "Won" : "Not won yet") : w >= n ? strf("All %d races won", n) : strf("%d of %d races won", w, n);
                    }
                    else if (cond == "license" || cond == "all_silver" || cond == "all_gold")
                    {
                        const auto it = lic.find(det);
                        if (it == lic.end())
                            continue;
                        const ProgressLicence &l = *it->second;
                        const int n = static_cast<int>(l.tests.size());
                        const bool done = cond == "license" ? l.passed : cond == "all_gold" ? (l.allGold || (n > 0 && l.gold >= n)) : (n > 0 && l.gold + l.silver >= n);
                        e.state = done ? 2 : l.done > 0 ? 1 : 0;
                        // (No "of N": the save's test list can be longer than the licence's tests.)
                        e.status = done ? std::string(cond == "license" ? "Passed" : cond == "all_gold" ? "All gold" : "All silver or better")
                                   : cond == "license" ? (l.done ? strf("%d tests done", l.done) : std::string("Not passed yet"))
                                   : cond == "all_gold" ? strf("%d gold so far", l.gold)
                                                        : strf("%d silver or gold so far", l.gold + l.silver);
                    }
                    else if (cond == "mission_1st")
                    {
                        const std::vector<std::string> p = split(det, ',');
                        if (p.size() != 2)
                            continue;
                        const int a = std::atoi(p[0].c_str()), b = std::atoi(p[1].c_str());
                        int n = 0, d = 0;
                        for (int i = a; i <= b; ++i)
                            if (i >= 1 && static_cast<size_t>(i) <= progress->missions.size())
                            {
                                ++n;
                                d += progress->missions[static_cast<size_t>(i - 1)].medal != '-';
                            }
                        if (n == 0)
                            continue;
                        e.state = d >= n ? 2 : d > 0 ? 1 : 0;
                        e.status = strf("%d of %d cleared", d, n);
                    }
                    else if (cond == "weeks")
                    {
                        const int64_t days = progress->details.days;
                        const int64_t need = static_cast<int64_t>(std::atoi(det.c_str())) * 7;
                        e.state = days >= need ? 2 : 0;
                        e.status = days >= need ? std::string("Added") : strf("In %lld days", static_cast<long long>(need - days));
                    }
                    else if (cond == "achievement" && progress->completion.known)
                    {
                        // PresentRoot::achievement (gtmode.adc 0x0e2aef): perMille >= detail * 10.
                        const Completion &cp = progress->completion;
                        const int need = std::atoi(det.c_str()) * 10;
                        e.state = cp.perMille >= need ? 2 : cp.won > 0 ? 1 : 0;
                        e.status = cp.perMille >= need ? "Reached (" + cp.shown() + ")" : cp.shown() + " so far";
                    }
                }
        }
        return groups;
    }

    // ------------------------------------------------------------------ multi-screen
    RigView rigView(double W, double gap, double d, bool angled, bool autoAngle, double angleDeg)
    {
        RigView r;
        if (W <= 0 || d <= 0)
            return r;
        const double pi = 3.14159265358979323846;
        double th = 0.0;
        if (angled)
        {
            if (autoAngle)
            {
                // Each side screen faces the eye. The hinge sits half a gap past the centre picture's edge and the side picture
                // starts half a gap past the hinge, so the side picture's centre is seen square on when
                // (W/2 + gap/2) cos(t) + (W/2 + gap/2) = d sin(t), i.e. tan(t/2) = (W/2 + gap/2) / d (MULTI-MONITOR-NOTES: 54.2 deg).
                th = 2.0 * std::atan2(W / 2 + gap / 2, d);
            }
            else
                th = angleDeg * pi / 180.0;
        }
        r.sideAngleDeg = th * 180.0 / pi;
        r.centreFovDeg = 2.0 * std::atan2(W / 2, d) * 180.0 / pi;
        // The side screen's far edge: from the hinge (half a gap past the centre picture), half a gap and a picture along the side screen.
        const double hinge = W / 2 + gap / 2;
        const double ex = hinge + (gap / 2 + W) * std::cos(th), ez = d - (gap / 2 + W) * std::sin(th);
        r.totalFovDeg = 2.0 * std::atan2(ex, ez) * 180.0 / pi;
        return r;
    }
}
