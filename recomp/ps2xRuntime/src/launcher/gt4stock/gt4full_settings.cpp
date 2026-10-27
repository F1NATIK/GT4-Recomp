// GT4 Recompilation launcher, Full edition: its part of gt4-start.env (gt4full_settings.h). Compiled into the Full
// target only; the Classic edition's settings file is unchanged (gt4stock_settings.cpp keeps its own lines in #else).

#include "gt4stock_settings.h"

#include "gt4stock_util.h"
#include "shared/gt4_full_keys.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>

namespace gt4stock
{
    const char *const kFullHudWidgetKeys[kFullHudWidgets] = {"SPEED", "REVS", "GEAR", "MAP", "TOWER"};
    const char *const kFullHudWidgetLabels[kFullHudWidgets] = {"Speed", "Revs", "Gear", "Map", "Timing tower"};
    const char *const kFullHudLiveKeys[kFullHudLiveWidgets] = {"RADAR", "DELTA", "PEDALS", "GMETER", "TOWER", "MAP", "SPLITS", "REVS", "SPEED", "GEAR", "LAP"};
    const char *const kFullHudLiveLabels[kFullHudLiveWidgets] = {"Radar",        "Delta",    "Pedals",      "G-meter", "Timing tower", "Track map",
                                                                 "Split times", "Rev lights", "Speed", "Gear",    "Lap counter"};
    const char *const kFullHudPlaces[kFullHudPlaceCount] = {"Bottom right", "Bottom left", "Top right", "Top left", "Bottom centre", "Top centre",
                                                            "Above GT4's dash", "Automatic"};

    const char *fullScalingKey(FullScaling s)
    {
        switch (s)
        {
        case FullScaling::Nearest: return "nearest";
        case FullScaling::Bicubic: return "bicubic";
        case FullScaling::Lanczos: return "lanczos";
        case FullScaling::Nis: return "nis";
        case FullScaling::Tv: return "tv";
        default: return "bilinear";
        }
    }

    const char *fullScalingLabel(FullScaling s)
    {
        switch (s)
        {
        case FullScaling::Nearest: return "Nearest Neighbor";
        case FullScaling::Bicubic: return "Bicubic Filtering";
        case FullScaling::Lanczos: return "Lanczos Resampling";
        case FullScaling::Nis: return "NVIDIA NIS";
        case FullScaling::Tv: return "TV (CRT)";
        default: return "Bilinear Filtering";
        }
    }

    bool fullScalingBuilt(FullScaling s) { return s != FullScaling::Nis && s != FullScaling::Tv; }

    const char *fullScalingFilterValue(FullScaling s) { return fullScalingBuilt(s) ? fullScalingKey(s) : "bilinear"; }

    const char *fullAspectKey(FullAspect a)
    {
        switch (a)
        {
        case FullAspect::Normal: return "4:3";
        case FullAspect::SquarePixels: return "pixels";
        case FullAspect::Ultra21: return "21:9";
        case FullAspect::Ultra32: return "32:9";
        case FullAspect::Triple48: return "48:9";
        default: return "16:9";
        }
    }

    const char *fullAspectLabel(FullAspect a)
    {
        switch (a)
        {
        case FullAspect::Normal: return "4:3";
        case FullAspect::SquarePixels: return "Square pixels";
        case FullAspect::Ultra21: return "21:9";
        case FullAspect::Ultra32: return "32:9";
        case FullAspect::Triple48: return "48:9 (one screen)";
        default: return "16:9";
        }
    }

    bool fullAspectBuilt(FullAspect) { return true; }

    bool fullAspectWider(FullAspect a) { return a == FullAspect::Ultra21 || a == FullAspect::Ultra32 || a == FullAspect::Triple48; }

    float fullAspectRatioOf(FullAspect a)
    {
        switch (a)
        {
        case FullAspect::Normal: return 4.0f / 3.0f;
        case FullAspect::SquarePixels: return 640.0f / 448.0f;
        case FullAspect::Ultra21: return 21.0f / 9.0f;
        case FullAspect::Ultra32: return 32.0f / 9.0f;
        case FullAspect::Triple48: return 48.0f / 9.0f;
        default: return 16.0f / 9.0f;
        }
    }

    const char *fullScaleLabel(int scale)
    {
        switch (scale)
        {
        case 2: return "2\xc3\x97";
        case 3: return "3\xc3\x97";
        case 4: return "4\xc3\x97";
        default: return "Native";
        }
    }

    const char *fullScaleSize(int scale)
    {
        switch (scale)
        {
        case 2: return "1280 \xc3\x97 896";
        case 3: return "1920 \xc3\x97 1344";
        case 4: return "2560 \xc3\x97 1792";
        default: return "640 \xc3\x97 448";
        }
    }

    // Which scales passed the full-m2 Resolution track's checks with GT4 on Vulkan on the RTX 5060 laptop
    // (analysis/design/full-m2/README.md, Resolution): the pictures (races in 480p with 32 and 16-bit colour, GT4's 480i
    // menus, the picture pass and its four filters, the read-backs) and New York at 60 fps with the drawing "follow the
    // resolution" picks (mean 59.5 or more, no 5-second stretch below 55). 4x: the pictures pass, but New York stuttered
    // (lowest stretch 54.3 fps, a frame of 8 VSyncs) and the night course ran at 53 fps (GT4 reads its own drawing back
    // every frame there and waits for the graphics card). developer, 2026-10-08: 4x is selectable anyway, its row noting that
    // it is for faster PCs.
    bool fullScaleBuilt(int scale) { return scale >= 1 && scale <= 4; }

    int fullPresetScale(int preset, int monitorH)
    {
        int s = -1;
        switch (preset)
        {
        case 1: s = monitorH <= 0 ? 3 : monitorH <= 800 ? 2 : monitorH <= 1200 ? 3 : 4; break;
        case 2: s = 2; break;
        case 3: s = 3; break;
        case 4:
        case 5: s = 4; break;
        default: return -1;
        }
        while (s > 1 && !fullScaleBuilt(s))
            --s;
        return s;
    }
    const char *fullPresetKey(int preset)
    {
        static const char *const keys[kFullResPresets] = {"custom", "auto", "720p", "1080p", "1440p", "2160p"};
        return keys[std::clamp(preset, 0, kFullResPresets - 1)];
    }
    const char *fullPresetLabel(int preset)
    {
        static const char *const labels[kFullResPresets] = {"Custom", "Automatic", "720p", "1080p", "1440p", "4K"};
        return labels[std::clamp(preset, 0, kFullResPresets - 1)];
    }

    const char *fullScaleWhy(int scale)
    {
        if (fullScaleBuilt(scale))
            return "";
        return scale == 4 ? "2560 \xc3\x97 1792 \xc2\xb7 not yet: the night course runs at 53 fps on an RTX 5060 laptop, New York stutters" : "not offered";
    }

    const char *fullScaleSpeed(int scale)
    {
        switch (scale)
        {
        case 2: return "New York 60 fps \xc2\xb7 night course 58";
        case 3: return "New York 60 fps, a rare short stutter \xc2\xb7 night course 58";
        case 4: return "for faster PCs \xc2\xb7 night course about 53 fps on an RTX 5060 laptop";
        default: return "";
        }
    }

    const char *fullScaleExactSpeed(int scale)
    {
        switch (scale)
        {
        case 3: return "Exact at 3\xc3\x97: about 57 fps in New York on an RTX 5060 laptop, with stutters. Fast keeps 60.";
        case 4: return "Exact at 4\xc3\x97: about 40 fps in New York on an RTX 5060 laptop.";
        default: return "";
        }
    }

    bool fullDrawingFast(int scale, int drawing)
    {
        if (scale <= 1)
            return false; // native is always exact (GT4.exe: fast mode only at a scale)
        return drawing == 2 || (drawing == 0 && scale >= 3);
    }

    std::string fullNetModeKey(int mode) { return mode == 1 ? "lan" : mode == 2 ? "loop" : "off"; }

    const char *fullEngineBassKey(int level)
    {
        switch (level)
        {
        case 0: return "off";
        case 1: return "low";
        case 3: return "high";
        default: return "medium";
        }
    }

    const char *fullEngineBassLabel(int level)
    {
        switch (level)
        {
        case 0: return "Off";
        case 1: return "Low";
        case 3: return "High";
        default: return "Medium";
        }
    }

    int fullEngineBassGainDb(int level) { return level == 1 ? 3 : level == 2 ? 6 : level == 3 ? 9 : 0; }

    std::string fullFpsStatList(int bits)
    {
        if ((bits & 0x7FF) == 0x7FF)
            return "all";
        std::string out;
        for (int i = 0; i < kFullFpsStatCount; ++i)
            if ((bits >> i) & 1)
                out += (out.empty() ? "" : ",") + std::string(kFullFpsStatKeys[i]);
        return out.empty() ? "fps" : out;
    }

    bool fullFpsStatBits(const std::string &list, int &bits)
    {
        bits = 0;
        bool ok = true;
        size_t at = 0;
        bool any = false;
        while (at <= list.size())
        {
            const size_t comma = std::min(list.find(',', at), list.size());
            std::string item = list.substr(at, comma - at);
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            for (char &c : item)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (item == "all")
                bits |= 0x7FF;
            else if (!item.empty())
            {
                int found = -1;
                for (int i = 0; i < kFullFpsStatCount; ++i)
                    if (item == kFullFpsStatKeys[i])
                        found = i;
                if (found >= 0)
                    bits |= 1 << found;
                else
                    ok = false;
            }
            any = any || !item.empty();
            at = comma + 1;
        }
        if (!any)
            bits = 0x7FF; // empty: every stat
        return ok;
    }

    const char *fullFpsCounterKey(int mode) { return mode == 1 ? "simple" : mode == 2 ? "detailed" : "off"; }

    std::string fullCamViewList(int bits)
    {
        std::string out;
        for (int v = 0; v < 6; ++v)
            if ((bits >> v) & 1)
                out += (out.empty() ? "" : ",") + std::string(kFullCamViewKeys[v]);
        return out.empty() ? "none" : out;
    }

    bool fullCamViewBits(const std::string &list, int &bits)
    {
        bits = 0;
        bool ok = true;
        size_t at = 0;
        while (at <= list.size())
        {
            const size_t comma = std::min(list.find(',', at), list.size());
            std::string item = list.substr(at, comma - at);
            item.erase(0, item.find_first_not_of(" \t"));
            item.erase(item.find_last_not_of(" \t") + 1);
            int found = -1;
            for (int v = 0; v < 6; ++v)
                if (item == kFullCamViewKeys[v])
                    found = v;
            if (found >= 0)
                bits |= 1 << found;
            else if (item != "none")
                ok = false;
            at = comma + 1;
        }
        return ok;
    }

    int fullHudMovePart(const std::string &key)
    {
        for (int i = 0; i < kFullHudMoveParts; ++i)
            if (key == kFullHudMoveKeys[i])
                return i;
        return -1;
    }

    std::string fullHudMoveList(const FullHudMoves &moves)
    {
        std::string out;
        for (int i = 0; i < kFullHudMoveParts; ++i)
        {
            const FullHudMove &m = moves[static_cast<size_t>(i)];
            if (m.on)
                out += (out.empty() ? "" : ";") +
                       strf("%s:%.1f,%.1f,%d", kFullHudMoveKeys[i], std::clamp(m.x, 0.0f, 100.0f), std::clamp(m.y, 0.0f, 100.0f), std::clamp(m.size, 40, 250));
        }
        return out;
    }

    std::string fullRadarZoomList(int bits)
    {
        std::string out;
        for (int i = 0; i < kFullRadarZoomSteps; ++i)
            if ((bits >> i) & 1)
                out += (out.empty() ? "" : ",") + std::to_string(kFullRadarZoomM[i]);
        return out.empty() ? std::string("off") : out;
    }
    std::string fullRadarColourList(const std::array<uint32_t, 5> &c)
    {
        return strf("%06x,%06x,%06x,%06x,%06x", c[0] & 0xffffffu, c[1] & 0xffffffu, c[2] & 0xffffffu, c[3] & 0xffffffu, c[4] & 0xffffffu);
    }
    std::string fullZoomKeyName(int key)
    {
        if (key >= 'A' && key <= 'Z')
            return std::string(1, static_cast<char>(key + 32));
        if (key >= '0' && key <= '9')
            return std::string(1, static_cast<char>(key));
        return "none";
    }

    std::string fullHudHideList(int bits)
    {
        // (the race clock and best / last lap both hidden: "times", which an older GT4.exe reads too)
        const bool times = ((bits >> kFullHudHideClock) & 1) && ((bits >> kFullHudHideBest) & 1);
        std::string out;
        for (int i = 0; i < kFullHudHideCount; ++i)
            if ((bits >> i) & 1)
            {
                if (times && i == kFullHudHideBest)
                    continue;
                out += (out.empty() ? "" : ",") + std::string(times && i == kFullHudHideClock ? "times" : kFullHudHideKeys[i]);
            }
        return out;
    }

    bool fullHudHideBits(const std::string &list, int &bits)
    {
        bits = 0;
        bool ok = true;
        for (const std::string &part : split(list, ','))
        {
            const std::string item = lower(trim(part));
            int found = -1;
            for (int i = 0; i < kFullHudHideCount; ++i)
                if (item == kFullHudHideKeys[i])
                    found = i;
            if (item == "times")
                bits |= (1 << kFullHudHideClock) | (1 << kFullHudHideBest);
            else if (found >= 0)
                bits |= 1 << found;
            else if (!item.empty())
                ok = false;
        }
        return ok;
    }

    std::string fullScreensGeometry(const FullSettings &f)
    {
        return numberText(f.mmScreenW, 1) + "," + numberText(f.mmScreenH, 1) + "," + (f.mmPanorama ? std::string("0") : numberText(f.mmBezel, 1));
    }

    std::vector<FullMmCopy> fullMmCopies(const FullSettings &f)
    {
        std::vector<FullMmCopy> c = {{"center", "CENTER", "center", "play", 1}, {"left", "LEFT", "left", "monitor", 2}, {"right", "RIGHT", "right", "monitor", 3}};
        if (f.mmTop == 1)
            c.push_back({"top", "TOP", "top-center", "monitor", 4});
        else if (f.mmTop == 2)
            c.push_back({"top", "LIVE", "center", "live", 4});
        return c;
    }

    double fullMmGt4Angle(const FullSettings &f)
    {
        // GT4 0x4392e0, angled: each side screen turns by 2 atan2(pitch, 2 distance), the pitch one picture plus the gap.
        const double pitch = std::max(1.0, f.mmScreenW) + std::max(0.0, f.mmBezel), d = std::max(100, f.mmDistanceCm * 10);
        return 2.0 * std::atan2(pitch, 2.0 * d) * 180.0 / 3.14159265358979323846;
    }

    std::string fullMmGeometry(const FullSettings &f)
    {
        // GT4's own 16:9 preset's aspect (0.98) and scale (1); the frames' top / bottom are the MSI G321Q's (gt4_screens.py)
        // until a top screen's real measures are known; the side frames are half the gap between two pictures each.
        const bool panorama = f.mmPanorama;
        const double side = panorama ? 0.0 : std::max(0.0, f.mmBezel) / 2.0;
        const double top = panorama ? 0.0 : 9.0, bottom = panorama ? 0.0 : 26.8;
        return std::string(f.mmAngled && !panorama ? "1" : "0") + ",1," + std::to_string(std::clamp(f.mmDistanceCm, 20, 400) * 10) + "," +
               numberText(f.mmScreenW, 1) + "," + numberText(f.mmScreenH, 1) + ",0.98,1," + numberText(side, 2) + "," + numberText(side, 2) + "," +
               numberText(top, 1) + "," + numberText(bottom, 1);
    }

    std::vector<std::pair<std::string, std::string>> fullMmKeys(const FullSettings &f, const FullMmCopy &c, size_t copies, const std::string &group)
    {
        std::vector<std::pair<std::string, std::string>> k = {
            {"GT4_NET", "loop"},
            {"GT4_NET_INDEX", std::to_string(c.index)},
            {"GT4_NET_PEERS", std::to_string(std::max<size_t>(copies, 2))},
            {"GT4_MM_STYLE", c.style},
            {"GT4_MM_POSITION", c.position},
            {"GT4_MM_NAME", c.name},
            {"GT4_MM_HOST", c.role == "center" ? "1" : "0"},
            {"GT4_MM_PLAYERS", "1"},
            {"GT4_MM_MONITORS", std::to_string(copies > 0 ? copies - 1 : 0)},
            {"GT4_MM_AUTO_CONNECT", "1"},
            {"GT4_MM_GEOMETRY", fullMmGeometry(f)},
            {"GT4_MM_GROUP", group},
            {"GT4_MM_PAD", c.role == "center" ? "share" : "follow"},
            {"GT4_SCREENS", "1"},
            {"GT4_ASPECT", "16:9"},
            {"GT3_FULLSCREEN", "1"},
        };
        if (c.role != "center")
        {
            const std::vector<std::pair<std::string, std::string>> side = {
                {"GT4_MM_TARGET", "CENTER"}, {"GT3_SOUND_OUTPUT", "wallclock"}, {"GT3_VIBRATION", "0"},  {"PS2X_WHEEL", "none"},
                {"PS2X_WHEEL_FFB", "0"},     {"GT4_PAD2", "off"},                {"GT4_EYETOY", "absent"}, {"GT4_MC1", "none"},
                {"GT4_TEXTURE_DUMP", "off"}, {"GT4_FPS_COUNTER", "off"},         {"GT4_CHEAT_CARD", "off"},
            };
            k.insert(k.end(), side.begin(), side.end());
        }
        return k;
    }

    const std::vector<std::string> &fullManagedKeys()
    {
        static const std::vector<std::string> keys = [] {
            std::vector<std::string> k = {"GT4_RACE_MODE", "GT4_RACE_COLOUR", "GT4_ASPECT", "GT3_DISPLAY_ASPECT", "GT3_RENDERER",
                                          "GT4L_RES_PRESET", "GT3_GS_GL_SCALE", "GT3_GS_GL_FAST", "GT4L_DRAWING","GT4_EFFECTS_SCALE", "GT4_UPSCALER", "GT4_DLSS_DRAW", "GT4_VK_PRESENT", "GT4_CAR_LIGHTING", "GT4_AO", "GT4_AO_STRENGTH", "GT4_SSR", "GT4_RELIGHT", "GT4_SHADOWS", "GT4_SUN_GLOW", "GT4_BLOOM", "GT4_DRIVE_LOG", "GT4_AI_DRIVERS", "GT4_CAM_FAR_CHASE", "GT4_CAM_LOOK", "GT4_CAM_LOOK_STICK", "GT4_TRIGGER_RANGE", "GT4_CAM_VIEWS", "GT4_CAM_LOCK",
                                          "GT4_CAR_DETAIL", "GT4_WHEEL_DETAIL", "GT4_WHEEL_BLUR", "GT4_SCREENS", "GT4_SCREENS_GEOMETRY", "GT4_PAGES", "GT4_TELEMETRY_MONITOR", "GT4_TELEMETRY_LAYOUT", "GT4L_TELEMETRY_SCREEN", "GT4_MIRROR", "GT4L_TELE_MIRROR", "GT4_TELE_BG",
                                          "GT4_NET",       "GT4_NET_IP",      "GT4_NET_INDEX", "GT4_NET_PEERS", "GT4_ENGINE_BASS", "GT4_FPS_COUNTER", "GT4_FPS_STATS",
                                          "GT4_LUT", "GT4_LUT_STRENGTH", "GT4L_LUT_FILE", "GT4_TEX_SMOOTH",
                                          "GT4_TEXTURE_DUMP", "GT4_TEXTURE_DUMP_OPAQUE", "GT4_TEXTURE_REPLACE",
                                          "GT4_EXTRA_CARS", // owned so an earlier launcher's line is dropped; never written (no switch)
                                          "GT4L_EDITION",  "GT4L_DISPLAY_TAB", "GT4L_OTHER_MODES_OPEN", "GT4L_PREVIEW_ZOOM",
                                          "GT4L_COMPARE_CARS", "GT4L_COMPARE_STATS", "GT4L_COMPARE_UNITS", "GT4L_DEALER_PAY",
                                          "GT4L_GARAGE_CARD", "GT4L_RENDERER_PREPARED",
                                          "GT4_SOUND_PACK", "GT4_SOUND_PACK_MAP", "GT4L_SOUND_MODE", "GT4L_SOUND_ALL", "GT4L_SOUND_CARS", "GT4L_SOUND_TAB",
                                          "GT4_START_LIGHTS", "GT4_START_COUNT", "GT4_HUD_HIDE", "GT4L_HUD_DEFAULTS",
                                          "GT4_START_LIGHTS_BOX", "GT4_START_LOOK", "GT4L_HUD_PICTURE", "GT4_HUD_MOVE",
                                          "GT4_RADAR_SHOW", "GT4_RADAR_RANGE", "GT4_RADAR_ZOOM", "GT4_RADAR_LABELS", "GT4_RADAR_CARS",
                                          "GT4_RADAR_COLOURS", "GT4_RADAR_BG", "GT4_HUD_ZOOM_KEY", "GT4_HUD_ZOOM_PAD",
                                          "GT4_START_HOLD", "GT4_START_DELAY", "GT4_START_AI", "GT4_START_CAR", "GT4_START_JUMP",
                                          // the stored "Not built yet" choices (GT4L_NB_START_* and GT4L_NB_HUD_HIDE_*: an earlier
                                          // launcher's, read once, never written)
                                          "GT4L_NB_UPSCALER", "GT4L_NB_NIS_SHARPNESS", "GT4L_NB_CRT_CURVE", "GT4L_NB_CRT_GLOW", "GT4L_NB_CRT_MASK",
                                          "GT4L_NB_INTERNAL_RES", "GT4L_NB_DRAWING", "GT4L_NB_ASPECT", "GT4L_NB_VSYNC", "GT4L_NB_FPS_COUNTER",
                                          "GT4L_NB_CAR_DETAIL", "GT4L_NB_WHEEL_DETAIL", "GT4L_NB_WHEEL_BLUR", "GT4L_NB_MIRROR_DETAIL",
                                          "GT4L_NB_TEXTURE_LOD", "GT4L_NB_EFFECT_DETAIL", "GT4L_NB_HUD_WIDE", "GT4L_NB_HUD_HIDE_MAP",
                                          "GT4L_NB_HUD_HIDE_POSITION", "GT4L_NB_START_LIGHTS", "GT4L_NB_START_COUNT", "GT4L_NB_START_RANDOM_MS",
                                          "GT4L_NB_START_AI_MS", "GT4L_NB_PHONE_MIRROR", "GT4L_NB_MM_MODE", "GT4L_NB_MM_LAYOUT", "GT4L_NB_MM_ANGLE",
                                          "GT4L_NB_MM_GEOMETRY", "GT4L_NB_MM_PANORAMA", "GT4L_NB_MM_HUD", "GT4L_NB_MM_LIVE"};
            for (const char *w : kFullHudWidgetKeys)
                k.push_back(std::string("GT4L_NB_HUD_") + w);
            for (const char *w : kFullHudLiveKeys)
            {
                k.push_back(std::string("GT4_HUD_") + w);
                k.push_back(std::string("GT4L_HUD_") + w);
            }
            return k;
        }();
        return keys;
    }

    namespace
    {
        bool intIn(const std::string &v, int lo, int hi, int &out)
        {
            long long n = 0;
            if (!parseInt(trim(v), n) || n < lo || n > hi)
                return false;
            out = static_cast<int>(n);
            return true;
        }

        bool flag(const std::string &v, bool &out)
        {
            const std::string t = lower(trim(v));
            if (t == "1" || t == "on" || t == "yes")
                out = true;
            else if (t == "0" || t == "off" || t == "no")
                out = false;
            else
                return false;
            return true;
        }

        int detailIndex(const std::string &v)
        {
            const std::string t = lower(trim(v));
            return t == "x2" ? 1 : t == "x4" ? 2 : t == "full" ? 3 : t == "gt4" ? 0 : -1;
        }

        void line(std::ostream &o, const std::string &t) { o << t << "\r\n"; }
        void kv(std::ostream &o, const char *k, const std::string &v) { o << k << '=' << v << "\r\n"; }
        const char *onOff(bool b) { return b ? "1" : "0"; }

        // A HUD widget's GT4_HUD_ / GT4L_NB_HUD_ value: off, or on,<place>,<size>,<see-through>.
        std::string hudValue(const FullHudWidget &h)
        {
            return h.on ? strf("on,%d,%d,%d", std::clamp(h.place, 0, kFullHudPlaceCount - 1), std::clamp(h.size, 50, 200), std::clamp(h.seeThrough, 0, 90))
                        : std::string("off");
        }

        // HUD 4's and 4B's defaults for GT4.exe's widgets (before 4B and HUD 6 moved the radar and the delta), and whether two
        // widgets are alike.
        constexpr FullHudWidget kHud4Defaults[kFullHudLiveWidgets] = {{true, 4, 100, 20}, {true, 5, 100, 20}, {false, 0, 100, 20}, {false, 1, 100, 20}};
        constexpr FullHudWidget kHud4bDefaults[kFullHudLiveWidgets] = {{true, 0, 100, 20}, {true, 6, 100, 20}, {false, 0, 100, 20}, {false, 1, 100, 20}};
        bool hudSame(const FullHudWidget &a, const FullHudWidget &b)
        {
            return a.on == b.on && a.place == b.place && a.size == b.size && a.seeThrough == b.seeThrough;
        }
        // Whether widget w read as h gets HUD 6's default, by the file's GT4L_HUD_DEFAULTS (0: none, a file from before 4B; 1:
        // "4b"; 2: "6"): an older default held exactly (HUD 4's in a file without the line, 4B's in one with "4b" or none).
        bool hudMoves(int version, int w, const FullHudWidget &h)
        {
            return !hudSame(h, kFullHudLiveDefaults[w]) &&
                   ((version == 0 && hudSame(h, kHud4Defaults[w])) || (version <= 1 && hudSame(h, kHud4bDefaults[w])));
        }

        // <place>,<size>,<see-through> from p[at] on (exactly three, each in range) into out; false (out unchanged) otherwise.
        bool hudLayout(const std::vector<std::string> &p, size_t at, FullHudWidget &out)
        {
            FullHudWidget r = out;
            if (p.size() != at + 3 || !intIn(p[at], 0, kFullHudPlaceCount - 1, r.place) || !intIn(p[at + 1], 50, 200, r.size) ||
                !intIn(p[at + 2], 0, 90, r.seeThrough))
                return false;
            r.xy = false;
            out = r;
            return true;
        }

        bool percentIn(const std::string &v, float lo, float hi, float &out)
        {
            const std::string t = trim(v);
            char *end = nullptr;
            const double d = std::strtod(t.c_str(), &end);
            if (t.empty() || !end || *end || !(d >= lo && d <= hi))
                return false;
            out = static_cast<float>(d);
            return true;
        }

        // The new form (HUD hand-off 7 A3) from p[at] on: <x>,<y>,<size>,<see-through>[,<style>], x and y percent or auto,auto;
        // false (out unchanged) otherwise.
        bool hudLayoutXy(int w, const std::vector<std::string> &p, size_t at, FullHudWidget &out)
        {
            FullHudWidget r = out;
            // (the timing tower: <name>,<columns>,<rows> after the see-through, each optional from the end)
            // (the map: <detail> after its style, HUD hand-off 10)
            if (w == kFullHudTower ? (p.size() < at + 4 || p.size() > at + 7)
                                   : (p.size() != at + 4 && p.size() != at + 5 && !(w == kFullHudMap && p.size() == at + 6)))
                return false;
            const bool autoPlace = lower(trim(p[at])) == "auto" && lower(trim(p[at + 1])) == "auto";
            if (!autoPlace && (!percentIn(p[at], 0.0f, 100.0f, r.x) || !percentIn(p[at + 1], 0.0f, 100.0f, r.y)))
                return false;
            if (!intIn(p[at + 2], 40, 250, r.size) || !intIn(p[at + 3], 0, 90, r.seeThrough))
                return false;
            r.xy = !autoPlace;
            r.place = autoPlace ? 7 : 0;
            r.style = 0;
            r.simple = false;
            if (p.size() == at + 6)
            {
                const std::string d = lower(trim(p[at + 5]));
                if (d == gt4hudbox::kMapDetail[1])
                    r.simple = true;
                else if (d != gt4hudbox::kMapDetail[0])
                    return false;
            }
            if (w == kFullHudTower)
            {
                r.towerName = 0, r.towerCols = gt4hudbox::kTowerColsDefault, r.towerRows = gt4hudbox::kTowerRowsDefault;
                if (p.size() > at + 4)
                {
                    const std::string nm = lower(trim(p[at + 4]));
                    int n = -1;
                    for (int k = 0; k < 3; ++k)
                        if (nm == gt4hudbox::kTowerNames[k])
                            n = k;
                    if (n < 0)
                        return false;
                    r.towerName = n;
                }
                if (p.size() > at + 5)
                {
                    const std::string cols = lower(trim(p[at + 5]));
                    int bits = 0;
                    if (cols != "none")
                        for (const std::string &c : split(cols, '+'))
                        {
                            int k = -1;
                            for (int j = 0; j < 4; ++j)
                                if (trim(c) == gt4hudbox::kTowerCols[j])
                                    k = j;
                            if (k < 0)
                                return false;
                            bits |= 1 << k;
                        }
                    r.towerCols = bits;
                }
                if (p.size() > at + 6 && !intIn(p[at + 6], 4, 16, r.towerRows))
                    return false;
                out = r;
                return true;
            }
            if (p.size() >= at + 5)
            {
                const std::string st = lower(trim(p[at + 4]));
                if (!kFullHudStyleKeys[w][0])
                    return false;
                if (st == kFullHudStyleKeys[w][1])
                    r.style = 1;
                else if (st != kFullHudStyleKeys[w][0])
                    return false;
            }
            out = r;
            return true;
        }
    }

    bool fullHudMoveParse(const std::string &value, FullHudMoves &moves)
    {
        moves = FullHudMoves{};
        bool ok = true;
        for (const std::string &raw : split(value, ';'))
        {
            const std::string item = lower(trim(raw));
            if (item.empty())
                continue;
            const size_t colon = item.find(':');
            const int part = colon == std::string::npos ? -1 : fullHudMovePart(trim(item.substr(0, colon)));
            const std::vector<std::string> p = part < 0 ? std::vector<std::string>{} : split(item.substr(colon + 1), ',');
            float x = 0, y = 0;
            int size = 100;
            if (p.size() == 3 && percentIn(p[0], 0.0f, 100.0f, x) && percentIn(p[1], 0.0f, 100.0f, y) && intIn(p[2], 40, 250, size))
                moves[static_cast<size_t>(part)] = FullHudMove{true, std::round(x * 10.0f) / 10.0f, std::round(y * 10.0f) / 10.0f, size};
            else
                ok = false;
        }
        return ok;
    }

    void fullHudPlaceCentre(int w, int place, int size, float aspect, float &x, float &y)
    {
        // gt4_hud.cpp's old placement for one widget in its place (no stacking): fractions of the HUD area's height.
        x = 50.0f, y = 50.0f;
        if (w < 0 || w >= kFullHudLiveWidgets || place < 0 || place > 6 || aspect <= 0.0f)
            return;
        const float s = std::clamp(size, 40, 250) / 100.0f, m = 0.02f;
        const float ww = kFullHudBase[w][0] * s, wh = kFullHudBase[w][1] * s; // (of the height)
        const bool bottom = place == 0 || place == 1 || place == 4 || place == 6, right = place == 0 || place == 2, centre = place == 4 || place == 5 || place == 6;
        const float cx = centre ? 0.5f : right ? 1.0f - (m + ww * 0.5f) / aspect : (m + ww * 0.5f) / aspect;
        const float floor = place == 6 ? 1.0f - 0.31f : 1.0f - m;
        const float cy = bottom ? floor - wh * 0.5f : m + wh * 0.5f;
        x = std::round(cx * 1000.0f) / 10.0f;
        y = std::round(cy * 1000.0f) / 10.0f;
    }

    std::string fullHudTowerCols(int bits)
    {
        std::string v;
        for (int k = 0; k < 4; ++k)
            if ((bits >> k) & 1)
                v += (v.empty() ? "" : "+") + std::string(gt4hudbox::kTowerCols[k]);
        return v.empty() ? std::string("none") : v;
    }

    std::string fullHudValue(int w, const FullHudWidget &h, float aspect)
    {
        if (!h.on)
            return "off";
        float x = h.x, y = h.y;
        const bool autoPlace = !h.xy && h.place == 7;
        if (!h.xy && !autoPlace)
            fullHudPlaceCentre(w, h.place, h.size, aspect, x, y);
        std::string v = autoPlace ? std::string("on,auto,auto") : strf("on,%.1f,%.1f", std::clamp(x, 0.0f, 100.0f), std::clamp(y, 0.0f, 100.0f));
        v += strf(",%d,%d", std::clamp(h.size, 40, 250), std::clamp(h.seeThrough, 0, 90));
        if (w >= 0 && w < kFullHudLiveWidgets && kFullHudStyleKeys[w][0])
            v += std::string(",") + kFullHudStyleKeys[w][std::clamp(h.style, 0, 1)];
        if (w == kFullHudMap)
            v += std::string(",") + gt4hudbox::kMapDetail[h.simple ? 1 : 0];
        if (w == kFullHudTower)
            v += std::string(",") + gt4hudbox::kTowerNames[std::clamp(h.towerName, 0, 2)] + "," + fullHudTowerCols(h.towerCols) + "," +
                 std::to_string(std::clamp(h.towerRows, 4, 16));
        return v;
    }

    bool parseFullSetting(const std::string &key, const std::string &value, Settings &s, const std::function<void(const char *)> &bad)
    {
        FullSettings &f = s.full;
        const std::string v = lower(trim(value));
        // ---- GT4.exe keys
        if (key == "GT4_RACE_MODE")
        {
            if (v.empty() || v == "480p")
                f.gameVideoModes = false;
            else if (v == "game")
#if defined(GT4L_EDITION_FULL)
                f.gameVideoModes = false; // (no "Other video modes" in the Full edition: developer, 2026-10-19)
#else
                f.gameVideoModes = true;
#endif
            else
                bad("480p or game: 480p");
            return true;
        }
        if (key == "GT4_RACE_COLOUR")
        {
            if (v == "16")
                f.colour = 16;
            else if (v.empty() || v == "32" || v == "24")
                f.colour = 32; // 24 looks exactly like 32 in GT4 (developer dropped it): shown as 32
            else
                bad("16 or 32: 32");
            return true;
        }
        if (key == "GT4_PICTURE_FILTER")
        {
            // The 480p update's four filters; GT4.exe reads smooth and sharp as bilinear too.
            if (v == "nearest")
                f.scaling = FullScaling::Nearest;
            else if (v == "bilinear" || v == "smooth" || v.empty())
                f.scaling = FullScaling::Bilinear;
            else if (v == "sharp")
            {
                f.scaling = FullScaling::Bilinear;
                bad("sharp is not a choice here: shown as Bilinear Filtering");
            }
            else if (v == "bicubic")
                f.scaling = FullScaling::Bicubic;
            else if (v == "lanczos")
                f.scaling = FullScaling::Lanczos;
            else
                bad("nearest, bilinear, bicubic or lanczos: bilinear");
            s.filter = f.scaling == FullScaling::Nearest ? PictureFilter::Nearest : PictureFilter::Smooth;
            return true;
        }
        if (key == "GT3_GS_GL_SCALE")
        {
            // Display > Resolution: 1 (native) to 4. GT4.exe takes up to 8; 6x and 8x are not offered (developer: not for now).
            long long n = 0;
            if (!f.resPresetSet)
                f.resPreset = 0; // (a file from before the presets: its choice is kept as Custom)
            if (parseInt(trim(value), n) && n >= 1 && n <= kFullScales)
                f.internalRes = static_cast<int>(n);
            else if (parseInt(trim(value), n) && n > kFullScales && n <= 8)
            {
                f.internalRes = kFullScales;
                bad("1, 2, 3 or 4 (6x and 8x are not offered): 4");
            }
            else
            {
                f.internalRes = 1;
                bad("1, 2, 3 or 4: 1 (native)");
            }
            return true;
        }
        if (key == "GT4L_RES_PRESET")
        {
            int p = -1;
            for (int i = 0; i < kFullResPresets; ++i)
                if (v == fullPresetKey(i))
                    p = i;
            if (p < 0)
            {
                p = 0;
                bad("custom, auto, 720p, 1080p, 1440p or 2160p: custom");
            }
            f.resPreset = p;
            f.resPresetSet = true;
            return true;
        }
        if (key == "GT4L_TELEMETRY_SCREEN")
        {
            long long n = 0;
            f.telemetryScreen = parseInt(trim(value), n) && n >= 0 && n <= 16 ? static_cast<int>(n) : 0;
            return true;
        }
        if (key == "GT4_TELEMETRY_MONITOR")
        {
            const std::string tv = trim(value);
            f.telemetryRect = tv.find(',') != std::string::npos ? tv : std::string();
            return true;
        }
        if (key == "GT4_TELEMETRY_LAYOUT")
        {
            const std::string tv = trim(value);
            f.telemetryLayout = tv == "timing" ? 1 : tv == "map" ? 2 : tv == "dash" ? 3 : tv == "mirror" ? 4 : 0;
            return true;
        }
        if (key == "GT4_PAGES")
        {
            f.phonePages = trim(value) == "lan";
            return true;
        }
        if (key == "GT4L_TELE_MIRROR")
        {
            f.telemetryMirror = trim(value) == "1";
            return true;
        }
        if (key == "GT4_TELE_BG")
        {
            f.teleBlack = trim(value) == "black";
            return true;
        }
        if (key == "GT4_MIRROR")
        {
            // Display > HUD, Rear-view mirror: screen (GT4's own), both (screen + phone), page (phone only); gt4_mirror.h.
            const std::string m = trim(value);
            f.mirrorMode = m == "both" ? 1 : m == "page" ? 2 : 0;
            if (!m.empty() && m != "screen" && m != "both" && m != "page")
                bad("screen, both or page: screen");
            return true;
        }
        // Retired (developer 2026-10-17: the upscalers and the lighting options are gone from the launcher; their code is on
        // the local branch parked/launcher-extras): an older file's lines are read and dropped.
        for (const char *r : {"GT4_UPSCALER", "GT4_DLSS_DRAW", "GT4_CAR_LIGHTING", "GT4_AO", "GT4_AO_STRENGTH", "GT4_SSR", "GT4_RELIGHT",
                              "GT4_SHADOWS", "GT4_SUN_GLOW", "GT4_BLOOM", "GT3_DISPLAY_FILTER", "GT3_CRT_SCANLINES"})
            if (key == r)
            {
                if (key == "GT3_DISPLAY_FILTER" || key == "GT3_CRT_SCANLINES")
                    s.linesOn = false, s.strength = 0; // (no TV lines in the Full edition)
                return true;
            }
        if (key == "GT4_DRIVE_LOG")
        {
            // Race Lab: the lap recorder (a folder: on; off).
            f.raceLabRecord = !v.empty() && v != "off" && v != "0";
            return true;
        }
        if (key == "GT4_AI_DRIVERS")
        {
            // Race Lab: the AI cars' drivers' names (the folder they are sent to: on; off).
            f.raceLabDrivers = v != "off" && v != "0";
            return true;
        }
        if (key == "GT4_VK_PRESENT")
        {
            f.presenter = v == "gl" ? 1 : 0;
            if (v != "gl" && v != "auto" && v != "window" && !v.empty())
                bad("auto or gl: auto");
            return true;
        }
        if (key == "GT4_EFFECTS_SCALE")
        {
            // Display > Resolution, Effects: 0 Normal (as the picture), 2, 3 or 4 (analysis/design/effects-scale).
            long long n = 0;
            if (parseInt(trim(value), n) && (n == 0 || (n >= 2 && n <= kFullScales)))
                f.effectsScale = static_cast<int>(n);
            else
            {
                f.effectsScale = 0;
                bad("0 (normal), 2, 3 or 4: 0");
            }
            return true;
        }
        if (key == "GT3_GS_GL_FAST")
        {
            // What GT4.exe gets; the launcher's choice is GT4L_DRAWING (it wins when both are read).
            if (v == "0" || v == "1")
            {
                if (!f.drawingSet)
                    f.drawing = v == "1" ? 2 : 1;
            }
            else
                bad("0 (exact) or 1 (fast)");
            return true;
        }
        if (key == "GT4L_DRAWING")
        {
            if (v == "auto" || v == "exact" || v == "fast")
            {
                f.drawing = v == "exact" ? 1 : v == "fast" ? 2 : 0;
                f.drawingSet = true;
            }
            return true;
        }
        if (key == "GT4_ASPECT")
        {
            if (v.empty() || v == "16:9")
                f.aspect = f.aspect == FullAspect::Ultra21 || f.aspect == FullAspect::Ultra32 || f.aspect == FullAspect::Triple48 ? f.aspect : FullAspect::Wide;
            else if (v == "4:3")
            {
#if defined(GT4L_EDITION_FULL)
                f.aspect = FullAspect::Wide; // (the Full edition has no 4:3 or square pixels: developer, 2026-10-19)
#else
                if (f.aspect != FullAspect::SquarePixels)
                    f.aspect = FullAspect::Normal;
#endif
            }
            else if (v == "21:9")
                f.aspect = FullAspect::Ultra21;
            else if (v == "32:9")
                f.aspect = FullAspect::Ultra32;
            else if (v == "48:9")
                f.aspect = FullAspect::Triple48;
            else if (v == "game")
            {
                f.aspect = FullAspect::Wide;
                bad("game (GT4's own Aspect Ratio) is not offered by the launcher: 16:9");
            }
            else
                bad("16:9, 4:3, 21:9, 32:9 or 48:9: 16:9");
            return true;
        }
        if (key == "GT3_DISPLAY_ASPECT")
        {
            if (v == "pixels")
#if defined(GT4L_EDITION_FULL)
                f.aspect = FullAspect::Wide; // (no square pixels in the Full edition)
#else
                f.aspect = FullAspect::SquarePixels;
#endif
            else if (!v.empty() && v != "4:3")
                bad("pixels (square pixels): left out");
            return true;
        }
        if (key == "GT3_FULLSCREEN")
        {
            s.fullscreen = v == "1" || v == "span" || v == "exclusive";
            f.span = v == "span";
            f.exclusive = v == "exclusive";
            return true;
        }
        if (key == "GT3_RENDERER")
        {
            if (v == "auto" || v.empty())
                f.renderer = 0;
            else if (v == "vk" || v == "vulkan")
                f.renderer = 1;
            else if (v == "gl" || v == "opengl")
                f.renderer = 2;
            else
            {
                f.renderer = 0;
                bad("auto, vk or gl: auto");
            }
            return true;
        }
        if (key == "GT4_CAM_FAR_CHASE")
        {
            // The far chase's bit of the view button's views (GT4_CAM_VIEWS, written after it, wins).
            if (v == "on" || v == "1")
                f.camViews |= 1 << 5;
            else if (v == "off" || v == "0")
                f.camViews &= ~(1 << 5);
            else
            {
                f.camViews |= 1 << 5;
                bad("on or off: on");
            }
            if ((f.camViews & 0x3F) == 0)
                f.camViews = 1 << 4;
            return true;
        }
        if (key == "GT4_CAM_VIEWS")
        {
            int bits = 0;
            const bool ok = fullCamViewBits(v, bits);
            f.camViews = bits & 0x3F ? bits : 0x3F;
            if (!ok || !(bits & 0x3F))
                bad(("views from bumper,bonnet,roof,close,chase,far: " + fullCamViewList(f.camViews)).c_str());
            return true;
        }
        if (key == "GT4_CAM_LOCK")
        {
            int bits = 0;
            if (fullCamViewBits(v, bits))
                f.camLock = bits;
            else
            {
                f.camLock = 0x07;
                bad("views from bumper,bonnet,roof,close,chase,far, or none: bumper,bonnet,roof");
            }
            return true;
        }
        if (key == "GT4_CAM_LOOK")
        {
            if (v == "360")
                f.look360 = true;
            else if (v == "off" || v == "0")
                f.look360 = false;
            else
            {
                f.look360 = true;
                bad("360 or off: 360");
            }
            return true;
        }
        if (key == "GT4_TRIGGER_RANGE")
        {
            char *end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (end == v.c_str() || n < 30 || n > 100)
                bad("30 to 100: 100");
            f.triggerRange = end == v.c_str() ? 100 : static_cast<int>(std::clamp(n, 30L, 100L));
            return true;
        }
        if (key == "GT4_CAM_LOOK_STICK")
        {
            if (v == "take")
                f.lookTakeStick = true;
            else if (v == "share")
                f.lookTakeStick = false;
            else
            {
                f.lookTakeStick = true;
                bad("take or share: take");
            }
            return true;
        }
        if (key == "GT4_CAR_DETAIL" || key == "GT4_WHEEL_DETAIL")
        {
            // GT4.exe's spelling: game, 2x, 4x, full (the stored choices' gt4 / x2 / x4 are read too).
            const int d = v == "game" ? 0 : v == "2x" ? 1 : v == "4x" ? 2 : detailIndex(value);
            (key == "GT4_CAR_DETAIL" ? f.carDetail : f.wheelDetail) = std::max(0, d);
            f.detailSet = true;
            if (d < 0)
                bad("game, 2x, 4x or full: game");
            return true;
        }
        // Extra cars (analysis/jp-cars): no switch since 2026-10-14 (the pack next to GT4.exe is enough). The line an earlier
        // launcher wrote (esso / off, off by default) is read and dropped, so it cannot keep the ESSO away; GT4_EXTRA_CARS=off in
        // the environment is the hidden way out (GT4.exe and App::applyExtraCars read it there).
        if (key == "GT4_EXTRA_CARS")
            return true;
        if (key == "GT4_TEX_SMOOTH")
        {
            if (v == "on" || v == "1")
                f.texSmooth = true;
            else if (v == "off" || v == "0")
                f.texSmooth = false;
            else
            {
                f.texSmooth = false;
                bad("on or off: off");
            }
            return true;
        }
        // Texture dump / replacement (analysis/design/texture-dump): on, off, or a folder (read as on; written back as on).
        if (key == "GT4_TEXTURE_DUMP" || key == "GT4_TEXTURE_DUMP_OPAQUE" || key == "GT4_TEXTURE_REPLACE")
        {
            bool &b = key == "GT4_TEXTURE_DUMP" ? f.texDump : key == "GT4_TEXTURE_REPLACE" ? f.texReplace : f.texDumpOpaque;
            b = !v.empty() && v != "off" && v != "0";
            return true;
        }
        if (key == "GT4_WHEEL_BLUR")
        {
            if (v == "on" || v == "1")
                f.wheelBlur = true;
            else if (v == "off" || v == "0")
                f.wheelBlur = false;
            else
            {
                f.wheelBlur = true;
                bad("on or off: on");
            }
            f.detailSet = true;
            return true;
        }
        if (key == "GT4_SCREENS")
        {
            // Multi-screen: 3 = three screens, one wide picture (the mode kMmThreeWide); 1 = one screen (a stored mode that
            // is not built yet, GT4L_NB_MM_MODE, is kept).
            if (v == "3")
                f.mmMode = kMmThreeWide;
            else if (v.empty() || v == "1")
            {
                if (f.mmMode == kMmThreeWide)
                    f.mmMode = 0;
            }
            else
                bad("1 or 3: 1");
            return true;
        }
        if (key == "GT4_SCREENS_GEOMETRY")
        {
            // <width>,<height>,<gap> in mm: one screen's picture and the gap between two pictures (0: Panorama on, the gap
            // stored with the rig, GT4L_NB_MM_GEOMETRY, is kept).
            const std::vector<std::string> p = split(v, ',');
            double w = 0, h = 0, g = 0;
            if (p.size() == 3 && parseFloat(p[0], w) && parseFloat(p[1], h) && parseFloat(p[2], g) && w >= 50 && h >= 30 && w / h >= 1.0 && w / h <= 2.5 &&
                g >= 0 && g <= 0.5 * w)
            {
                f.mmScreenW = w;
                f.mmScreenH = h;
                f.mmPanorama = g == 0;
                if (g > 0)
                    f.mmBezel = g;
            }
            else
                bad("<width>,<height>,<gap> in mm: your rig's defaults");
            return true;
        }
        if (key == "GT4_NET")
        {
            if (v.empty() || v == "off" || v == "0")
                f.netMode = 0;
            else if (v == "lan")
                f.netMode = 1;
            else if (v == "loop")
                f.netMode = 2;
            else
            {
                f.netMode = 0;
                bad("off, lan or loop: off");
            }
            return true;
        }
        if (key == "GT4_NET_IP")
        {
            f.netIp = trim(value);
            return true;
        }
        if (key == "GT4_NET_INDEX")
        {
            if (!intIn(value, 1, 16, f.netIndex))
                bad("1..16: 1");
            return true;
        }
        if (key == "GT4_NET_PEERS")
        {
            if (!intIn(value, 1, 16, f.netPeers))
            {
                f.netPeers = 6;
                bad("a count 1..16: 6");
            }
            return true;
        }
        if (key == "GT4_LUT")
        {
            // Display > Picture, Colour (analysis/design/colour-lut): off, gt3 (the GT3 look) or a .cube file's path.
            if (v.empty() || v == "off")
                f.lut = 0;
            else if (v == "gt3")
                f.lut = 1;
            else
            {
                f.lut = 2;
                f.lutFile = trim(value);
            }
            return true;
        }
        if (key == "GT4L_LUT_FILE")
        {
            if (!trim(value).empty())
                f.lutFile = trim(value);
            return true;
        }
        if (key == "GT4_LUT_STRENGTH")
        {
            if (!intIn(value, 0, 100, f.lutStrength))
            {
                f.lutStrength = 100;
                bad("0..100: 100");
            }
            return true;
        }
        if (key == "GT4_FPS_COUNTER")
        {
            // Display > Smoothness: the FPS counter (GT4.exe: gt4_fps_counter.h). on / 1 (the counter before simple and
            // detailed, Test Build 5) reads as detailed.
            if (v == "simple")
                f.fpsCounter = 1;
            else if (v == "detailed" || v == "on" || v == "1")
                f.fpsCounter = 2;
            else if (v == "off" || v == "0" || v.empty())
                f.fpsCounter = 0;
            else
            {
                f.fpsCounter = 0;
                bad("off, simple or detailed: off");
            }
            return true;
        }
        if (key == "GT4_FPS_STATS")
        {
            // What the detailed FPS counter shows (Display > Smoothness, the tick list).
            int bits = 0;
            const bool ok = fullFpsStatBits(v, bits);
            f.fpsStats = bits & 0x7FF ? bits : 0x7FF;
            if (!ok || !(bits & 0x7FF))
                bad(("stats from fps,frametime,worst,low1,graph,cpu,gpu,ram,vram,resolution,renderer: " + fullFpsStatList(f.fpsStats)).c_str());
            return true;
        }
        if (key == "GT4_ENGINE_BASS")
        {
            if (v == "off")
                f.engineBass = 0;
            else if (v == "low")
                f.engineBass = 1;
            else if (v == "medium" || v.empty())
                f.engineBass = 2;
            else if (v == "high")
                f.engineBass = 3;
            else
            {
                f.engineBass = kFullEngineBassDefault;
                bad("off, low, medium or high: medium");
            }
            return true;
        }
        if (key == "GT4_HUD_MOVE")
        {
            // Race HUD > HUD layout (HUD hand-off 7 B1): GT4's own parts moved, <part>:<x>,<y>,<size>;... An item not understood
            // is dropped with a warning; the others are kept.
            if (!fullHudMoveParse(v, f.hudMoves))
                bad(("<part>:<x>,<y>,<size 40-250>;... (parts: position, lap, clock, besttimes, map, panel, fuel, warnings, gbar, gearspeed, "
                     "mirror): " + (fullHudMoveList(f.hudMoves).empty() ? std::string("none moved") : fullHudMoveList(f.hudMoves))).c_str());
            return true;
        }
        if (key == "GT4_HUD_HIDE")
        {
            // Display > HUD: the parts of GT4's own race HUD hidden, a list of position, lap, times, panel and map (empty: none).
            // A name not understood is dropped with a warning; the others are kept.
            int bits = 0;
            if (!fullHudHideBits(v, bits))
                bad(("parts from position,lap,times,panel,map: " + (bits ? fullHudHideList(bits) : std::string("none hidden"))).c_str());
            f.hudHide = bits;
            f.hudHideSet = true;
            return true;
        }
        if (key == "GT4L_HUD_DEFAULTS")
        {
            // Written from HUD 4B on ("4b") and HUD 6 on ("6"), before the widget lines: which older default a widget may still
            // hold exactly and so moves to HUD 6's (hudMoves; none for "6": the widgets are the player's). One moved already
            // (its line came first, a file put together by hand) gets its value back if this version does not move it.
            f.hudDefaultsVersion = trim(v) == "4b" ? 1 : trim(v) == "6" ? 2 : 3; // ("7": HUD hand-off 7's new form, written by this launcher)
            for (int w = 0; w < kFullHudLiveWidgets; ++w)
                if (((f.liveWidgetsMoved >> w) & 1) && !hudMoves(f.hudDefaultsVersion, w, f.liveWidgetsRead[static_cast<size_t>(w)]))
                {
                    f.liveWidgets[static_cast<size_t>(w)] = f.liveWidgetsRead[static_cast<size_t>(w)];
                    f.liveWidgetsMoved &= ~(1 << w);
                }
            return true;
        }
        // Display > HUD, the widgets GT4.exe draws: off, or on,<place 0-7>,<size 50-200>,<see-through 0-90>. One that is off
        // keeps its place, size and see-through in the launcher's GT4L_HUD_<KEY>; an on line's win, wherever it is in the file.
        if ((startsWith(key, "GT4_HUD_") || startsWith(key, "GT4L_HUD_")) && key != "GT4L_HUD_PICTURE" && key != "GT4_HUD_ZOOM_KEY" && key != "GT4_HUD_ZOOM_PAD")
        {
            const bool kept = startsWith(key, "GT4L_");
            int w = -1;
            for (int i = 0; i < kFullHudLiveWidgets; ++i)
                if (key.substr(kept ? 9 : 8) == kFullHudLiveKeys[i])
                    w = i;
            if (w < 0)
                return false; // not ours: kept as written
            FullHudWidget &h = f.liveWidgets[static_cast<size_t>(w)];
            const std::vector<std::string> p = split(v, ',');
            if (w == 0)
                f.radarLineRead = true; // (an older file without GT4_RADAR_BG: the radar's see-through gives it, parseSettingsText)
            if (kept)
            {
                if (!((f.liveWidgetsSet >> w) & 1) && !hudLayoutXy(w, p, 0, h))
                    hudLayout(p, 0, h); // (a value the launcher does not write is dropped)
            }
            else if (v == "off")
                h.on = false;
            else if (p.size() >= 5 && trim(p[0]) == "on" && hudLayoutXy(w, p, 1, h))
            {
                // The new form (HUD hand-off 7 A3): the player's, never moved.
                h.on = true;
                f.liveWidgetsSet |= 1 << w;
            }
            else if (p.size() == 4 && trim(p[0]) == "on" && hudLayout(p, 1, h))
            {
                h.on = true;
                f.liveWidgetsSet |= 1 << w;
                // A file that still holds an older default exactly (hudMoves: HUD 4's radar at the bottom centre and delta at the
                // top centre, or 4B's at the bottom right and above GT4's dash, each 100 % and 20 % see-through): HUD 6's default
                // (placed automatically). One the player changed stays.
                if (hudMoves(f.hudDefaultsVersion, w, h))
                {
                    f.liveWidgetsRead[static_cast<size_t>(w)] = h;
                    h = kFullHudLiveDefaults[w];
                    f.liveWidgetsMoved |= 1 << w;
                }
            }
            else
            {
                h = kFullHudLiveDefaults[w];
                if (!v.empty())
                    bad(("off or on,<x>,<y>,<size 40-250>,<see-through 0-90>[,<style>]: " + fullHudValue(w, h, fullAspectRatioOf(f.aspect))).c_str());
            }
            return true;
        }
        if (key == "GT4_START_LIGHTS")
        {
            // Display > HUD: start lights over GT4's own countdown (they follow it), 1 or 0.
            if (!flag(value, f.startLights))
            {
                f.startLights = false;
                bad("1 or 0: 0");
            }
            return true;
        }
        if (key == "GT4_START_LIGHTS_BOX")
        {
            // Race HUD > HUD layout: the start lights' centre (percent of the HUD area) and size.
            const std::vector<std::string> p = split(v, ',');
            float x = kFullLightsX, y = kFullLightsY;
            int size = 100;
            if (p.size() == 3 && percentIn(p[0], 0.0f, 100.0f, x) && percentIn(p[1], 0.0f, 100.0f, y) && intIn(p[2], 40, 250, size))
                f.lightsX = x, f.lightsY = y, f.lightsSize = size;
            else
            {
                f.lightsX = kFullLightsX, f.lightsY = kFullLightsY, f.lightsSize = 100;
                bad("<x>,<y>,<size 40-250>: 50,7.5,100");
            }
            return true;
        }
        if (key == "GT4_START_LOOK")
        {
            f.startLook = v == "two" ? 1 : 0;
            if (v != "two" && v != "row")
                bad("row or two: row");
            return true;
        }
        // HUD hand-off 7 E: Driving > Race HUD > Start lights, holding the start.
        if (key == "GT4_START_HOLD")
        {
            if (!flag(value, f.startHold))
            {
                f.startHold = false;
                bad("on or off: off");
            }
            return true;
        }
        if (key == "GT4_START_DELAY")
        {
            const std::vector<std::string> p = split(v, ',');
            float a = 0.2f, b = 3.0f;
            if (p.size() == 2 && percentIn(p[0], 0.0f, 5.0f, a) && percentIn(p[1], 0.0f, 5.0f, b) && a <= b)
                f.startDelayMin = std::round(a * 10.0f) / 10.0f, f.startDelayMax = std::round(b * 10.0f) / 10.0f;
            else
            {
                f.startDelayMin = 0.2f, f.startDelayMax = 3.0f;
                bad("<min>,<max> seconds, 0 to 5: 0.2,3.0");
            }
            return true;
        }
        if (key == "GT4_START_AI")
        {
            static const char *const kAi[] = {"instant", "quick", "normal", "slow"};
            int k = 0;
            while (k < 4 && v != kAi[k])
                ++k;
            f.startAi = k < 4 ? k : 2;
            if (k == 4)
                bad("instant, quick, normal or slow: normal");
            return true;
        }
        if (key == "GT4_START_CAR")
        {
            f.startFree = v == "free";
            if (v != "free" && v != "held")
                bad("held or free: held");
            return true;
        }
        if (key == "GT4_START_JUMP")
        {
            // (t5 / t10, time added to your first lap, are not built: read as h5 / h10)
            f.startJump = (v == "h10" || v == "t10") ? 1 : 0;
            if (v != "h5" && v != "h10" && v != "t5" && v != "t10")
                bad("h5 or h10: h5");
            return true;
        }
        // HUD hand-off 7 D / 9: Driving > Race HUD > Radar, and Controllers' Race HUD zoom button.
        if (key == "GT4_RADAR_SHOW")
        {
            f.radarShow = v == "near" ? 1 : 0;
            if (v != "near" && v != "always")
                bad("always or near: always");
            return true;
        }
        if (key == "GT4_RADAR_RANGE")
        {
            // 10 to 80 m in 1 m steps (the GT3 radar settings); an older file's 5 to 100 m (hand-off 9) clamped to 10 to 80.
            int r = kFullRadarRangeDefault;
            if (intIn(v, 1, 1000, r))
                f.radarRange = std::clamp(r, 10, 80);
            else
            {
                f.radarRange = kFullRadarRangeDefault;
                bad("10 to 80 m: 25");
            }
            return true;
        }
        if (key == "GT4_RADAR_ZOOM")
        {
            if (v == "off")
            {
                f.radarZoom = 0;
                return true;
            }
            int bits = 0;
            bool ok = !v.empty();
            for (const std::string &p : split(v, ','))
            {
                int m = 0, at = -1;
                if (intIn(trim(p), 5, 100, m))
                    for (int i = 0; i < kFullRadarZoomSteps; ++i)
                        if (kFullRadarZoomM[i] == m)
                            at = i;
                if (at >= 0)
                    bits |= 1 << at;
                else
                    ok = false;
            }
            f.radarZoom = bits ? bits : kFullRadarZoomDefault;
            if (!ok)
                bad("off, or ranges of 10, 15, 20, 30, 40, 50, 60, 80, 100 m: the others left out (written as off: no zoom since the GT3 radar settings)");
            return true;
        }
        if (key == "GT4_RADAR_LABELS")
        {
            f.radarLabels = v != "off";
            if (v != "on" && v != "off")
                bad("on or off: on");
            return true;
        }
        if (key == "GT4_RADAR_CARS")
        {
            int c = kFullRadarCarsDefault;
            if (intIn(v, 10, 100, c))
                f.radarCars = c;
            else
            {
                f.radarCars = kFullRadarCarsDefault;
                bad("10 to 100: 90");
            }
            return true;
        }
        if (key == "GT4_RADAR_BG")
        {
            int b = kFullRadarBgDefault;
            if (intIn(v, 0, 100, b))
                f.radarBg = b;
            else
            {
                f.radarBg = kFullRadarBgDefault;
                bad("0 to 100: 35");
            }
            f.radarBgRead = true;
            return true;
        }
        if (key == "GT4_RADAR_COLOURS")
        {
            const std::vector<std::string> p = split(v, ',');
            bool ok = p.size() == 5;
            for (size_t k = 0; k < 5; ++k)
            {
                std::string h = k < p.size() ? trim(p[k]) : std::string();
                if (!h.empty() && h[0] == '#')
                    h.erase(0, 1);
                char *end = nullptr;
                const unsigned long c = std::strtoul(h.c_str(), &end, 16);
                if (h.size() == 6 && end && !*end)
                    f.radarColours[k] = static_cast<uint32_t>(c);
                else
                    f.radarColours[k] = kFullRadarColours[k], ok = false;
            }
            if (!ok)
                bad("five RRGGBB colours (you, other, close, alongside, background): the defaults for the others");
            return true;
        }
        if (key == "GT4_HUD_ZOOM_KEY")
        {
            if (v == "none" || v.empty())
                f.hudZoomKey = 0;
            else if (v.size() == 1 && ((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= '0' && v[0] <= '9')))
                f.hudZoomKey = v[0] >= 'a' ? v[0] - 32 : v[0];
            else
            {
                f.hudZoomKey = 'Z';
                bad("a letter, a digit or none: z");
            }
            return true;
        }
        if (key == "GT4_HUD_ZOOM_PAD")
        {
            int b = -1;
            if (v == "none" || v.empty())
                f.hudZoomPad = -1;
            else if (intIn(v, 1, 17, b))
                f.hudZoomPad = b;
            else
            {
                f.hudZoomPad = -1;
                bad("none or a controller button 1 to 17: none");
            }
            return true;
        }
        if (key == "GT4L_HUD_PICTURE")
        {
            f.hudPicture = trim(value);
            return true;
        }
        if (key == "GT4_START_COUNT")
        {
            if (!intIn(value, 3, 5, f.startCount))
            {
                f.startCount = 5;
                bad("3, 4 or 5: 5");
            }
            return true;
        }
        // ---- the stored "Not built yet" choices
        if (gt4fullkeys::notBuiltKey(key))
        {
            const std::string k = key.substr(gt4fullkeys::kNotBuiltPrefix.size());
            bool ok = true;
            if (k == "UPSCALER")
            {
                if (v == "nis")
                    f.scaling = FullScaling::Nis;
                else if (v == "tv")
                    f.scaling = FullScaling::Tv;
                else if (v != "off" && !v.empty())
                    ok = false;
            }
            else if (k == "NIS_SHARPNESS" || k == "CRT_CURVE" || k == "CRT_GLOW" || k == "CRT_MASK")
                ok = true; // (retired: read and dropped)
            else if (k == "INTERNAL_RES")
            {
                // A file from before internal resolution went live (full-m2, Resolution): read once, then written as
                // GT3_GS_GL_SCALE. A GT3_GS_GL_SCALE line in the same file wins (it is the newer one).
                int stored = 1;
                ok = intIn(value, 1, 4, stored);
                if (ok && f.internalRes == 1)
                    f.internalRes = stored;
            }
            else if (k == "DRAWING")
            {
                ok = v == "exact" || v == "fast" || v == "auto";
                if (ok && !f.drawingSet)
                {
                    f.drawing = v == "exact" ? 1 : v == "fast" ? 2 : 0;
                    f.drawingSet = true;
                }
            }
            else if (k == "ASPECT")
            {
                if (v == "21:9")
                    f.aspect = FullAspect::Ultra21;
                else if (v == "32:9")
                    f.aspect = FullAspect::Ultra32;
                else if (v == "48:9")
                    f.aspect = FullAspect::Triple48;
                else
                    ok = v == "off" || v.empty();
            }
            else if (k == "VSYNC")
                ok = flag(value, f.vsync);
            else if (k == "FPS_COUNTER")
            {
                f.fpsCounter = v == "ms" || v == "graph" ? 2 : 0; // the stored choice before GT4_FPS_COUNTER: detailed
                ok = v == "ms" || v == "graph" || v == "off";
            }
            else if (k == "CAR_DETAIL" || k == "WHEEL_DETAIL")
            {
                // A file from before car and wheel detail went live (full-m2, Detail): read once, then written as
                // GT4_CAR_DETAIL / GT4_WHEEL_DETAIL. A GT4_ line in the same file wins (it is the newer one).
                const int d = detailIndex(value);
                ok = d >= 0;
                if (!f.detailSet)
                    (k == "CAR_DETAIL" ? f.carDetail : f.wheelDetail) = std::max(0, d);
            }
            else if (k == "WHEEL_BLUR")
            {
                bool b = f.wheelBlur;
                ok = flag(value, b);
                if (ok && !f.detailSet)
                    f.wheelBlur = b;
            }
            else if (k == "MIRROR_DETAIL")
                ok = flag(value, f.mirrorDetail);
            else if (k == "TEXTURE_LOD")
            {
                f.sharpTextures = v == "sharp";
                ok = v == "sharp" || v == "ps2";
            }
            else if (k == "EFFECT_DETAIL")
                ok = flag(value, f.effectDetail);
            else if (k == "HUD_WIDE")
            {
                f.hudKeepMiddle = v == "middle";
                ok = v == "middle" || v == "edges";
            }
            else if (k == "HUD_HIDE_MAP" || k == "HUD_HIDE_POSITION")
            {
                // A file from before hiding went live: read once, then written as GT4_HUD_HIDE (map; position and lap, which
                // this switch hid together). A GT4_HUD_HIDE line in the same file wins (it is the newer one).
                bool hide = false;
                ok = flag(value, hide);
                const int parts = k == "HUD_HIDE_MAP" ? 1 << kFullHudHideMap : (1 << 0) | (1 << 1); // kFullHudHideKeys' order
                if (ok && !f.hudHideSet)
                    f.hudHide = hide ? f.hudHide | parts : f.hudHide & ~parts;
            }
            else if (k == "START_LIGHTS" || k == "START_COUNT")
            {
                // A file from before the start lights went live: read once, then written as GT4_START_LIGHTS / GT4_START_COUNT.
                ok = k == "START_LIGHTS" ? flag(value, f.startLights) : intIn(value, 3, 5, f.startCount);
            }
            else if (k == "START_RANDOM_MS" || k == "START_AI_MS")
            {
                // The random wait and the AI's reaction are gone (the lights follow GT4's own countdown): read and dropped.
            }
            else if (k == "PHONE_MIRROR")
                ok = flag(value, f.phoneMirror);
            else if (k == "MM_MODE")
            {
                // The modes that are not built yet (1-3); GT4_SCREENS=3 (three screens, one wide picture) wins.
                int m = 0;
                ok = intIn(value, 0, 3, m);
                if (ok && f.mmMode != kMmThreeWide)
                    f.mmMode = m;
            }
            else if (k == "MM_LAYOUT")
                ok = (v == "3" || v == "6") && intIn(value, 3, 6, f.mmLayout);
            else if (k == "MM_ANGLE")
            {
                f.mmAngled = v != "flat";
                f.mmAutoAngle = v == "auto";
                if (v != "flat" && v != "auto")
                    ok = intIn(value, 0, 90, f.mmAngleDeg);
            }
            else if (k == "MM_GEOMETRY")
            {
                const std::vector<std::string> p = split(v, ',');
                double w = 0, h = 0, b = 0, d = 0;
                ok = p.size() == 4 && parseFloat(p[0], w) && parseFloat(p[1], h) && parseFloat(p[2], b) && parseFloat(p[3], d) && w > 100 && h > 50 &&
                     b >= 0 && d >= 20 && d <= 400;
                if (ok)
                {
                    f.mmScreenW = w;
                    f.mmScreenH = h;
                    f.mmBezel = b;
                    f.mmDistanceCm = static_cast<int>(std::lround(d));
                }
            }
            else if (k == "MM_PANORAMA")
                ok = flag(value, f.mmPanorama);
            else if (k == "MM_HUD")
            {
                f.mmHudEvery = v == "every";
                ok = v == "every" || v == "middle";
            }
            else if (k == "MM_LIVE")
                ok = flag(value, f.mmLive);
            else if (k == "MM_TOP")
            {
                ok = v == "off" || v == "top" || v == "live";
                f.mmTop = v == "top" ? 1 : v == "live" ? 2 : 0;
            }
            else if (startsWith(k, "HUD_"))
            {
                int w = -1;
                for (int i = 0; i < kFullHudWidgets; ++i)
                    if (k == std::string("HUD_") + kFullHudWidgetKeys[i])
                        w = i;
                if (w < 0)
                    return false; // not ours: kept as written
                FullHudWidget &h = f.widgets[static_cast<size_t>(w)];
                const std::vector<std::string> p = split(v, ',');
                if (v == "off" || v.empty())
                    h.on = false;
                else if (p.size() == 4 && trim(p[0]) == "on" && hudLayout(p, 1, h))
                    h.on = true;
                else
                    ok = false;
            }
            else
                return false; // an unknown GT4L_NB_ line: kept as written
            if (!ok)
                bad("not a value the launcher writes: the default");
            return true;
        }
        // ---- the launcher's own (Full)
        if (key == "GT4L_EDITION")
            return true;
        if (key == "GT4L_DISPLAY_TAB")
        {
            intIn(value, 0, 8, f.displayTab);
            return true;
        }
        if (key == "GT4L_OTHER_MODES_OPEN")
        {
            flag(value, f.otherModesOpen);
            return true;
        }
        if (key == "GT4L_PREVIEW_ZOOM")
        {
            intIn(value, 1, 8, f.previewZoom);
            return true;
        }
        if (key == "GT4L_COMPARE_CARS")
        {
            f.compareSlots.clear();
            for (const std::string &p : split(v, ','))
            {
                int slot = -1;
                if (intIn(p, 0, 999, slot) && f.compareSlots.size() < 4 &&
                    std::find(f.compareSlots.begin(), f.compareSlots.end(), slot) == f.compareSlots.end())
                    f.compareSlots.push_back(slot);
            }
            return true;
        }
        if (key == "GT4L_COMPARE_STATS")
        {
            f.compareStats = trim(value);
            return true;
        }
        if (key == "GT4L_COMPARE_UNITS")
        {
            const std::vector<std::string> p = split(v, ',');
            if (!p.empty())
                f.powerUnit = p[0] == "ps" ? 0 : p[0] == "kw" ? 2 : 1;
            if (p.size() > 1)
                f.torqueUnit = p[1] == "kgfm" ? 1 : p[1] == "lbft" ? 2 : 0;
            return true;
        }
        if (key == "GT4L_DEALER_PAY")
        {
            flag(value, f.dealerPay);
            return true;
        }
        if (key == "GT4L_GARAGE_CARD")
        {
            intIn(value, 1, 2, f.garageCard);
            return true;
        }
        if (key == "GT4L_RENDERER_PREPARED")
        {
            f.prepared = trim(value);
            return true;
        }
        // ---- Sound > Sound packs. GT4L_SOUND_MODE wins; without it (a hand-written file) a folder in GT4_SOUND_PACK means
        // every car, a map file per car (its lines are not read back: GT4L_SOUND_CARS holds the launcher's list).
        if (key == "GT4L_SOUND_MODE")
        {
            if (v == "off")
                f.soundMode = FullSoundMode::Off;
            else if (v == "all")
                f.soundMode = FullSoundMode::EveryCar;
            else if (v == "cars")
                f.soundMode = FullSoundMode::PerCar;
            else
                bad("off, all or cars: off");
            f.soundModeSet = true;
            return true;
        }
        if (key == "GT4_SOUND_PACK")
        {
            const std::string t = trim(value);
            if (!t.empty() && v != "off")
            {
                if (f.soundPackAll.empty())
                    f.soundPackAll = t;
                if (!f.soundModeSet)
                    f.soundMode = FullSoundMode::EveryCar;
            }
            return true;
        }
        if (key == "GT4_SOUND_PACK_MAP")
        {
            if (!trim(value).empty() && v != "off" && !f.soundModeSet)
                f.soundMode = FullSoundMode::PerCar;
            return true;
        }
        if (key == "GT4L_SOUND_ALL")
        {
            f.soundPackAll = trim(value);
            return true;
        }
        if (key == "GT4L_SOUND_CARS")
        {
            if (!fullSoundCarsParse(trim(value), f.soundCars))
                bad("<car code>=<pack folder>|...: the entries that read");
            return true;
        }
        if (key == "GT4L_SOUND_TAB")
        {
            intIn(value, 0, 1, f.soundTab);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ Sound packs
    const char *fullSoundModeLabel(FullSoundMode m)
    {
        switch (m)
        {
        case FullSoundMode::EveryCar: return "Every car";
        case FullSoundMode::PerCar: return "Per car";
        default: return "Off";
        }
    }

    std::string fullSoundPackValue(const FullSettings &f)
    {
        return f.soundMode == FullSoundMode::EveryCar && !f.soundPackAll.empty() ? f.soundPackAll : std::string("off");
    }

    std::string fullSoundMapValue(const FullSettings &f)
    {
        return f.soundMode == FullSoundMode::PerCar && !f.soundMapFile.empty() && (!f.soundCars.empty() || !f.soundPackAll.empty()) ? f.soundMapFile
                                                                                                                                     : std::string("off");
    }

    std::string fullSoundMapText(const FullSettings &f)
    {
        std::ostringstream o;
        o << "# GT4 sound packs per car, written by the GT4 Recompilation launcher (Sound > Sound packs).\r\n";
        o << "# One line per car: <GT4 car code> <pack folder>; 'all <pack folder>' is for every other car.\r\n";
        for (const FullSoundCar &c : f.soundCars)
            if (c.code && !c.folder.empty())
                o << c.code << ' ' << c.folder << "\r\n";
        if (!f.soundPackAll.empty())
            o << "all " << f.soundPackAll << "\r\n";
        return o.str();
    }

    std::string fullSoundCarsList(const std::vector<FullSoundCar> &cars)
    {
        std::string out;
        for (const FullSoundCar &c : cars)
        {
            if (!c.code || c.folder.empty() || c.folder.find('|') != std::string::npos)
                continue;
            if (!out.empty())
                out += '|';
            out += std::to_string(c.code) + "=" + c.folder;
        }
        return out;
    }

    bool fullSoundCarsParse(const std::string &v, std::vector<FullSoundCar> &out)
    {
        out.clear();
        bool ok = true;
        for (const std::string &entry : split(v, '|'))
        {
            const std::string e = trim(entry);
            if (e.empty())
                continue;
            const size_t eq = e.find('=');
            long long code = 0;
            if (eq == std::string::npos || !parseInt(trim(e.substr(0, eq)), code) || code <= 0 || code > 0xffffffffll || trim(e.substr(eq + 1)).empty())
            {
                ok = false;
                continue;
            }
            FullSoundCar c;
            c.code = static_cast<uint32_t>(code);
            c.folder = trim(e.substr(eq + 1));
            bool replaced = false;
            for (FullSoundCar &x : out)
                if (x.code == c.code)
                {
                    x = c;
                    replaced = true;
                }
            if (!replaced)
                out.push_back(c);
        }
        return ok;
    }

    std::string fullSoundPackFor(const FullSettings &f, uint32_t code)
    {
        for (const FullSoundCar &c : f.soundCars)
            if (c.code == code)
                return c.folder;
        return std::string();
    }

    SoundPackInfo readSoundPackInfo(const std::string &folderUtf8)
    {
        SoundPackInfo p;
        p.folder = folderUtf8;
        const fs::path folder = fs::path(std::u8string(folderUtf8.begin(), folderUtf8.end()));
        {
            const auto u = folder.filename().u8string();
            p.name = std::string(u.begin(), u.end());
        }
        std::ifstream in(folder / "pack.json", std::ios::binary);
        if (!in)
        {
            p.problem = "no pack.json";
            return p;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string t = ss.str();
        // A pack.json as sound_pack.py writes it: "key": value pairs; only what the list shows is picked out.
        auto valueAfter = [&](const std::string &key, size_t from, size_t to) -> std::string {
            const size_t k = t.find("\"" + key + "\"", from);
            if (k == std::string::npos || k >= to)
                return std::string();
            size_t c = t.find(':', k);
            if (c == std::string::npos)
                return std::string();
            ++c;
            while (c < t.size() && std::isspace(static_cast<unsigned char>(t[c])))
                ++c;
            if (c < t.size() && t[c] == '"')
            {
                const size_t e = t.find('"', c + 1);
                return e == std::string::npos ? std::string() : t.substr(c + 1, e - c - 1);
            }
            size_t e = c;
            while (e < t.size() && t[e] != ',' && t[e] != '}' && t[e] != ']' && t[e] != '\n')
                ++e;
            return trim(t.substr(c, e - c));
        };
        if (trim(valueAfter("format", 0, t.size())) != "1")
        {
            p.problem = "pack.json is not format 1";
            return p;
        }
        if (const std::string n = valueAfter("name", 0, t.size()); !n.empty())
            p.name = n;
        auto section = [&](const std::string &key, size_t &a, size_t &b) {
            a = t.find("\"" + key + "\"");
            if (a == std::string::npos)
                return false;
            a = t.find('[', a);
            b = a == std::string::npos ? std::string::npos : t.find(']', a);
            return a != std::string::npos && b != std::string::npos;
        };
        size_t a = 0, b = 0;
        if (section("layers", a, b))
            for (size_t at = t.find("\"load\"", a); at != std::string::npos && at < b; at = t.find("\"load\"", at + 6))
            {
                const std::string load = valueAfter("load", at, b);
                (load == "off" ? p.off : p.on) += 1;
            }
        if (section("pops", a, b))
            for (size_t at = t.find("\"file\"", a); at != std::string::npos && at < b; at = t.find("\"file\"", at + 6))
                ++p.pops;
        if (const std::string bass = valueAfter("bass_db", 0, t.size()); !bass.empty())
            p.bassDb = std::strtod(bass.c_str(), nullptr);
        p.ok = p.on > 0;
        if (!p.ok)
            p.problem = "no on-throttle loop";
        return p;
    }

    std::vector<SoundPackInfo> listSoundPacks(const std::string &packsFolderUtf8)
    {
        std::vector<SoundPackInfo> out;
        std::error_code ec;
        const fs::path root = fs::path(std::u8string(packsFolderUtf8.begin(), packsFolderUtf8.end()));
        if (!fs::is_directory(root, ec))
            return out;
        for (const auto &e : fs::directory_iterator(root, ec))
        {
            if (!e.is_directory(ec) || !fs::is_regular_file(e.path() / "pack.json", ec))
                continue;
            const auto u = e.path().u8string();
            out.push_back(readSoundPackInfo(std::string(u.begin(), u.end())));
        }
        std::sort(out.begin(), out.end(), [](const SoundPackInfo &x, const SoundPackInfo &y) { return lower(x.name) < lower(y.name); });
        return out;
    }

    void renderFullPicture(std::ostream &o, const Settings &s)
    {
        const FullSettings &f = s.full;
        line(o, "# ---- Picture: video mode and colour ----");
        line(o, "# 480p = races and GT4's menus in 480p (the default; the logos and 2-player races stay 480i, as on a PS2);");
        line(o, "# game = GT4's own Options (480i menus, 480i / 1080i races, the closed section)");
        kv(o, "GT4_RACE_MODE", f.gameVideoModes ? "game" : "480p");
        line(o, "# race colour: 32 = full colour (the default); 16 = 16-bit as the PS2 drew it, with its dithering");
        kv(o, "GT4_RACE_COLOUR", f.colour == 16 ? "16" : "32");
        line(o, "");
        line(o, "# ---- Picture: scaling ----");
        line(o, "# nearest, bilinear (the default), bicubic or lanczos. Scaling always acts across the picture; down");
        line(o, "# it acts only where the TV lines are flat (lines off; partly below strength 50)");
        kv(o, "GT4_PICTURE_FILTER", fullScalingFilterValue(f.scaling));
        line(o, "");
        line(o, "# ---- Picture: no TV scanlines ----");
        kv(o, "GT3_DISPLAY_FILTER", "off");
        kv(o, "GT3_CRT_SCANLINES", "0");
        kv(o, "GT4_PICTURE_FIT", "fill");
        line(o, "");
        line(o, "# ---- Picture: colour (a colour LUT on the picture, before the TV lines) ----");
        line(o, "# off (the default), gt3 = the GT3 look (GT3's colours, matched from GT3 and GT4 race pictures), or the path of");
        line(o, "# a .cube file (3D; a relative path is from GT4.exe's folder). Strength 0-100: 100 = the LUT's full colour");
        kv(o, "GT4_LUT", f.lut == 1 ? std::string("gt3") : f.lut == 2 && !f.lutFile.empty() ? f.lutFile : std::string("off"));
        kv(o, "GT4_LUT_STRENGTH", std::to_string(std::clamp(f.lutStrength, 0, 100)));
        line(o, "");
        line(o, "# ---- Other video modes (480i, 1080i): the two interlaced fields ----");
        line(o, "# GT4's 480i menus and car select, and 480i / 1080i races: blend (the default), adaptive, weave or bob;");
        line(o, "# interlace flicker 0-100 (0 = off)");
        kv(o, "GT4_DEINTERLACE", deinterlaceKey(s.deinterlace));
        kv(o, "GT4_SCANLINE_FIELD", std::to_string(std::clamp(s.field, 0, 100)));
        line(o, "");
        line(o, "# ---- Screen shape ----");
        line(o, "# 16:9 (the default) or 4:3; square pixels = 4:3 with GT3_DISPLAY_ASPECT=pixels; 21:9, 32:9 or 48:9 (one screen):");
        line(o, "# races and replays that wide (GT4's camera widened), GT4's menus 16:9");
        const bool narrow = f.aspect == FullAspect::Normal || f.aspect == FullAspect::SquarePixels;
        kv(o, "GT4_ASPECT", narrow ? "4:3" : fullAspectWider(f.aspect) ? fullAspectKey(f.aspect) : "16:9");
        if (f.aspect == FullAspect::SquarePixels)
            kv(o, "GT3_DISPLAY_ASPECT", "pixels");
        else
            line(o, "# GT3_DISPLAY_ASPECT=pixels");
        line(o, "");
        line(o, "# ---- Window ----");
        line(o, "# exclusive = full screen; 1 = borderless window over the monitor below; span = across all monitors;");
        line(o, "# 0 = a window of the size below");
        kv(o, "GT3_FULLSCREEN", !s.fullscreen ? "0" : f.span ? "span" : f.exclusive ? "exclusive" : "1");
        kv(o, "GT3_WINDOW_SIZE", strf("%dx%d", s.windowW, s.windowH));
        line(o, "# The monitor: its desktop position x,y, then its name (without it GT4 opens on the main display).");
        if (s.monitorSet)
            kv(o, "GT4_MONITOR", strf("%d,%d", s.monitorX, s.monitorY) + (s.monitorName.empty() ? std::string() : " " + s.monitorName));
        else
            line(o, "# GT4_MONITOR=0,0");
        line(o, "");
    }

    void renderFullMore(std::ostream &o, const Settings &s)
    {
        const FullSettings &f = s.full;
        line(o, "# ---- Renderer: auto (Vulkan on an NVIDIA or AMD graphics card, OpenGL on any other), vk (Vulkan) or gl (OpenGL) ----");
        kv(o, "GT3_RENDERER", f.renderer == 1 ? "vk" : f.renderer == 2 ? "gl" : "auto");
        line(o, "");
        line(o, "# ---- Resolution: GT4's pictures drawn at 1 (native, 640 x 448), 2, 3 or 4 times the PS2's size ----");
        line(o, "# preset: auto (from the monitor), 720p (2x), 1080p (3x), 1440p (4x), 2160p (4x) or custom (the scale below as chosen)");
        kv(o, "GT4L_RES_PRESET", fullPresetKey(std::clamp(f.resPreset, 0, kFullResPresets - 1)));
        const int scale = std::clamp(f.internalRes, 1, kFullScales);
        // A choice that is not offered (none today; 4x was until 2026-10-08) is kept for the launcher (GT4L_NB_INTERNAL_RES) and GT4 gets native, so a
        // GT4.exe started from this folder without the launcher draws natively too.
        const int given = fullScaleBuilt(scale) ? scale : 1;
        kv(o, "GT3_GS_GL_SCALE", std::to_string(given));
        if (given != scale)
        {
            line(o, strf("# chosen: %dx, not offered yet (Display > Resolution says why): GT4 draws at native", scale));
            kv(o, "GT4L_NB_INTERNAL_RES", std::to_string(scale));
        }
        line(o, "# drawing: auto (exact at native and 2x, fast at 3x and 4x), exact (the PS2's blending, exact for every pixel) or fast");
        line(o, "# (the graphics card's own blending); GT3_GS_GL_FAST is what GT4 gets: 0 exact, 1 fast (native is always exact)");
        kv(o, "GT4L_DRAWING", f.drawing == 1 ? "exact" : f.drawing == 2 ? "fast" : "auto");
        kv(o, "GT3_GS_GL_FAST", fullDrawingFast(given, f.drawing) ? "1" : "0");
        line(o, "# effects: GT4's glow, light and reflection buffers drawn at this size (0: as the picture; 2-4: used only below the picture's)");
        kv(o, "GT4_EFFECTS_SCALE", std::to_string(f.effectsScale)); // (GT4 uses it only below the picture's size)
        line(o, "# frames shown: auto (through Vulkan) or gl (the older way, through OpenGL)");
        kv(o, "GT4_VK_PRESENT", f.presenter == 1 ? "gl" : "auto");
        line(o, "");
        line(o, "# ---- Race Lab ----");
        line(o, "# lap recorder: racelab-laps (the folder beside this file: one file per lap and one per race, for the Race Lab page;");
        line(o, "# F10 in a race stops and starts it) or off");
        kv(o, "GT4_DRIVE_LOG", f.raceLabRecord ? "racelab-laps" : "off");
        line(o, "# the AI cars' drivers' names (the timing tower's driver names, the pages): racelab-data\\ai-lines (where the Race Lab's");
        line(o, "# Send to AI cars puts gt4-drivers.txt) or off (the AI cars keep their car names)");
        kv(o, "GT4_AI_DRIVERS", f.raceLabDrivers ? "racelab-data\\ai-lines" : "off");
        line(o, "");
        line(o, "# ---- Race cameras ----");
        line(o, "# far chase: on = a chase view 1.5 times as far back, after GT4's own views (the view button); off = GT4's views");
        line(o, "# (for a GT4 program without GT4_CAM_VIEWS; GT4_CAM_VIEWS wins)");
        kv(o, "GT4_CAM_FAR_CHASE", (f.camViews >> 5) & 1 ? "on" : "off");
        line(o, "# the views the view button goes through, in this order: bumper, bonnet, roof, close (chase), chase, far (chase)");
        kv(o, "GT4_CAM_VIEWS", fullCamViewList(f.camViews & 0x3F ? f.camViews : 0x3F));
        line(o, "# the views locked to the car's tilt (tilting with its body); the others keep the horizon level. GT4's own:");
        line(o, "# bumper,bonnet,roof. none = no view locked");
        kv(o, "GT4_CAM_LOCK", fullCamViewList(f.camLock & 0x3F));
        line(o, "# 360 = the right stick looks all the way round in races and springs back (only while it does not drive the car);");
        line(o, "# off = GT4's own look buttons only");
        kv(o, "GT4_CAM_LOOK", f.look360 ? "360" : "off");
        line(o, "# with the 360 look on: take = in races the right stick only turns the view (GT4 does not see it there: accelerate");
        line(o, "# and brake on the triggers or buttons); share = GT4 keeps it, and the look is off while GT4 drives with it");
        kv(o, "GT4_CAM_LOOK_STICK", f.lookTakeStick ? "take" : "share");
        line(o, "");
        line(o, "# ---- Controller triggers ----");
        line(o, "# the L2 / R2 pressure at a trigger's full travel, 30..100 percent (100 = GT4's full 255); lower spreads");
        line(o, "# the part GT4 uses over the whole trigger");
        kv(o, "GT4_TRIGGER_RANGE", std::to_string(std::clamp(f.triggerRange, 30, 100)));
        line(o, "");
        line(o, "# ---- Car and wheel detail ----");
        line(o, "# game = GT4's own switching to simpler cars and wheels in the distance; 2x / 4x = full detail twice / four times");
        line(o, "# as far; full = always full detail. Wheel-spin blur: on = GT4's blurred rims at speed, off = sharp rims");
        kv(o, "GT4_CAR_DETAIL", f.carDetail == 1 ? "2x" : f.carDetail == 2 ? "4x" : f.carDetail == 3 ? "full" : "game");
        kv(o, "GT4_WHEEL_DETAIL", f.wheelDetail == 1 ? "2x" : f.wheelDetail == 2 ? "4x" : f.wheelDetail == 3 ? "full" : "game");
        kv(o, "GT4_WHEEL_BLUR", f.wheelBlur ? "on" : "off");
        line(o, "");
        line(o, "# ---- Texture smoothing ----");
        line(o, "# on = at an internal resolution of 2x and above, fine textures GT4 draws sharp on 3D surfaces are filtered smooth");
        line(o, "# (no shimmer); off = as the PS2 drew them (the default). Never at native resolution");
        kv(o, "GT4_TEX_SMOOTH", f.texSmooth ? "on" : "off");
        line(o, "");
        line(o, "# ---- Texture dump and replacement ----");
        line(o, "# GT4_TEXTURE_DUMP on = every texture GT4 draws saved once as a PNG in texture-dump next to GT4.exe (menus and");
        line(o, "# track/race-NN); GT4_TEXTURE_DUMP_OPAQUE on = also copies without transparency in texture-dump/_opaque.");
        line(o, "# GT4_TEXTURE_REPLACE on = PNGs in texture-replace next to GT4.exe (named like the dump's, any size) drawn instead.");
        kv(o, "GT4_TEXTURE_DUMP", f.texDump ? "on" : "off");
        kv(o, "GT4_TEXTURE_DUMP_OPAQUE", f.texDumpOpaque ? "on" : "off");
        kv(o, "GT4_TEXTURE_REPLACE", f.texReplace ? "on" : "off");
        line(o, "");
        line(o, "# ---- Multi-screen: three screens side by side, one wide picture ----");
        line(o, "# 3 = races and replays across three screens: the middle one is GT4_MONITOR, the side ones the monitors right next");
        line(o, "# to it; the HUD and GT4's menus on the middle screen. 1 = one screen (the default). Geometry: one screen's picture");
        line(o, "# width and height and the gap between two pictures in mm (0 = a panorama, nothing left out at the frames)");
        kv(o, "GT4_SCREENS", f.mmMode == kMmThreeWide ? "3" : "1");
        kv(o, "GT4_SCREENS_GEOMETRY", fullScreensGeometry(f));
        line(o, "");
        line(o, "# ---- Telemetry screen: GT4's timing and course map page (live from the race) ----");
        line(o, "# pages: off, local (this PC: the telemetry screen) or lan (phones on this home network too; nothing goes online)");
        line(o, "# monitor: 0 = no telemetry window, or <DISPLAYn>,<monitor name>,<W>x<H>,<x>,<y> (Driving > Telemetry screen; an older");
        line(o, "# launcher's x,y,w,h still works); the layouts: telemetry-layout.json beside this file (GT4 serves it at /layout)");
        const bool tele = f.telemetryScreen > 0 && !f.telemetryRect.empty();
        // (HUD hand-off 8: the tick "Phones and tablets on this home network" alone opens the pages to the home network;
        // otherwise this PC only, for the telemetry window or the mirror's pages)
        kv(o, "GT4_PAGES", f.phonePages ? "lan" : tele || f.mirrorMode != 0 || f.telemetryMirror ? "local" : "off");
        kv(o, "GT4_TELEMETRY_MONITOR", tele ? f.telemetryRect : std::string("0"));
        kv(o, "GT4_TELE_BG", f.teleBlack ? "black" : "dark");
        line(o, "# rear-view mirror on the game screen: screen (GT4's own: the bumper view), both (every view) or page (none). The pages'");
        line(o, "# mirror (telemetry screen, phone) shows the rear view in every view whichever it is (developer, 21 Oct)");
        kv(o, "GT4_MIRROR", f.mirrorMode == 1 ? "both" : f.mirrorMode == 2 ? "page" : "screen");
        kv(o, "GT4L_TELE_MIRROR", f.telemetryMirror ? "1" : "0");
        kv(o, "GT4L_TELEMETRY_SCREEN", std::to_string(f.telemetryScreen));
        line(o, "");
        line(o, "# ---- Network: GT4's LAN Battle (Arcade) ----");
        line(o, "# off (the default), lan = other PCs on this network, loop = GT4 windows on this PC");
        kv(o, "GT4_NET", fullNetModeKey(f.netMode));
        if (f.netMode == 1 && !f.netIp.empty())
            kv(o, "GT4_NET_IP", f.netIp);
        if (f.netMode == 2)
        {
            kv(o, "GT4_NET_INDEX", std::to_string(std::clamp(f.netIndex, 1, 16)));
            kv(o, "GT4_NET_PEERS", std::to_string(std::clamp(f.netPeers, 1, 16)));
        }
        line(o, "");
        line(o, "# ---- Sound: engine bass ----");
        line(o, "# The cars' engines and exhausts deeper: off (GT4's own sound), low, medium (the default) or high");
        line(o, "# (up to +3 / +6 / +9 dB well below 150 Hz, half that at 150 Hz; in a race a soft limiter rounds off GT4's loudest peaks)");
        kv(o, "GT4_ENGINE_BASS", fullEngineBassKey(std::clamp(f.engineBass, 0, 3)));
        line(o, "");
        line(o, "# ---- Sound: sound packs (your own engine sounds) ----");
        line(o, "# A pack folder (pack.json and its WAV files, made by Sound > Sound packs > Add sounds) for every car, or off;");
        line(o, "# GT4 plays it for the car you drive in place of its own engine. Per car: the map file (one '<car code> <folder>'");
        line(o, "# line per car, 'all <folder>' for the others), or off");
        kv(o, "GT4_SOUND_PACK", fullSoundPackValue(f));
        kv(o, "GT4_SOUND_PACK_MAP", fullSoundMapValue(f));
        line(o, "");
        line(o, "# ---- FPS counter in GT4's top left corner ----");
        line(o, "# off (the default), simple (frames per second and frame time) or detailed (the stats below); F3 in GT4 goes");
        line(o, "# off -> simple -> detailed -> off while playing");
        kv(o, "GT4_FPS_COUNTER", fullFpsCounterKey(std::clamp(f.fpsCounter, 0, 2)));
        line(o, "# what detailed shows: all, or a list of fps, frametime, worst (the longest frame of the last second), low1 (1% low fps),");
        line(o, "# graph (frame times), cpu, gpu (load), ram, vram (use), resolution (internal), renderer (and the graphics card)");
        kv(o, "GT4_FPS_STATS", fullFpsStatList(f.fpsStats & 0x7FF ? f.fpsStats : 0x7FF));
        line(o, "");
        line(o, "# ---- HUD widgets and start lights (in races, over GT4's own HUD), and parts of GT4's own HUD hidden ----");
        line(o, "# (Driving > Race HUD > HUD layout) off, or on,<x>,<y>,<size>,<see-through>[,<style>]: x and y the box's centre in");
        line(o, "# percent of the game picture (one decimal), or auto,auto (clear of GT4's own HUD in every view); size 40-250 percent;");
        line(o, "# see-through 0-90 percent; styles: radar round or square, delta bar or number, gmeter trail or dot. Radar: the cars");
        line(o, "# close around you; delta: your time against your best lap; pedals: throttle and brake; gmeter: the g-forces on the car;");
        line(o, "# tower: the timing tower, then <name> car, short or driver, <columns> gap+int+best+last (any of them) and <rows> 4-16;");
        line(o, "# map: the race map, locked or rotating; splits: the split times; revs: the rev lights, bar or lights; speed; gear;");
        line(o, "# lap: our lap counter, lap or clock (with the race time)");
        // (Before the widget lines: the file is from HUD hand-off 7 on, so its widgets are read as written; parseFullSetting.)
        kv(o, "GT4L_HUD_DEFAULTS", "7");
        const float hudAspect = fullAspectRatioOf(f.aspect);
        for (int i = 0; i < kFullHudLiveWidgets; ++i)
            kv(o, (std::string("GT4_HUD_") + kFullHudLiveKeys[i]).c_str(), fullHudValue(i, f.liveWidgets[static_cast<size_t>(i)], hudAspect));
        line(o, "# start lights: 1 = lights over GT4's own countdown (they follow it and go out at the start), 0 = GT4's countdown");
        line(o, "# only (the default); 3, 4 or 5 lights (5 the default); their box: <x>,<y>,<size> (50,7.5,100 the top centre);");
        line(o, "# look: row (one row) or two (two rows)");
        kv(o, "GT4_START_LIGHTS", onOff(f.startLights));
        kv(o, "GT4_START_COUNT", std::to_string(std::clamp(f.startCount, 3, 5)));
        kv(o, "GT4_START_LIGHTS_BOX", strf("%.1f,%.1f,%d", std::clamp(f.lightsX, 0.0f, 100.0f), std::clamp(f.lightsY, 0.0f, 100.0f), std::clamp(f.lightsSize, 40, 250)));
        kv(o, "GT4_START_LOOK", f.startLook == 1 ? "two" : "row");
        line(o, "# holding the start (Driving > Race HUD > Start lights; offline races with a standing start only, never LAN races or");
        line(o, "# replays): on or off (the default: GT4's own start). On: the lights go out a random <min>,<max> seconds (0-5) after");
        line(o, "# the last one; each AI car reacts instant (GT4's own), quick (0.1-0.3 s), normal (0.2-0.6 s) or slow (0.4-1.0 s);");
        line(o, "# your car held, or free from the first light (moving off early is a jump start); a jump start holds your car h5 or");
        line(o, "# h10 seconds after the lights go out");
        kv(o, "GT4_START_HOLD", f.startHold ? "on" : "off");
        kv(o, "GT4_START_DELAY", strf("%.1f,%.1f", std::clamp(f.startDelayMin, 0.0f, 5.0f), std::clamp(std::max(f.startDelayMax, f.startDelayMin), 0.0f, 5.0f)));
        {
            static const char *const kAi[] = {"instant", "quick", "normal", "slow"};
            kv(o, "GT4_START_AI", kAi[std::clamp(f.startAi, 0, 3)]);
        }
        kv(o, "GT4_START_CAR", f.startFree ? "free" : "held");
        kv(o, "GT4_START_JUMP", f.startJump == 1 ? "h10" : "h5");
        line(o, "# hidden in races: a list of position (the race position, top left), lap (the lap counter), clock (the race clock),");
        line(o, "# besttimes (best and last lap; times = clock and besttimes), panel (speedo, revs, gear and digital speed) and map");
        line(o, "# (the course map); empty = nothing hidden (the default). GT4's messages always show");
        kv(o, "GT4_HUD_HIDE", fullHudHideList(f.hudHide));
        line(o, "# GT4's own parts moved and resized: <part>:<x>,<y>,<size>;... (x, y the centre in % of the HUD area, size 40-250 %),");
        line(o, "# parts position, lap, clock, besttimes, map, panel (the bumper and bonnet views' dash), fuel, warnings, gbar,");
        line(o, "# gearspeed (the chase views' four pieces) and mirror; empty = every part where GT4 puts it (the default)");
        kv(o, "GT4_HUD_MOVE", fullHudMoveList(f.hudMoves));
        line(o, "# radar (Driving > Race HUD > Radar, GT3's radar settings): show always, or near (only while a car is within the");
        line(o, "# range); range 10-80 m; zoom off (no zoom button: GT4's own controls keep every key); labels: the metres on the");
        line(o, "# rings, on or off; cars: the cars' opacity 10-100 %; bg: the background's opacity 0-100 %; colours RRGGBB: your car,");
        line(o, "# other cars, close (the other cars' colour), alongside (under 6 m, and the side bars), background. The phone and");
        line(o, "# top-screen radars use the same (the pages' /config)");
        kv(o, "GT4_RADAR_SHOW", f.radarShow == 1 ? "near" : "always");
        kv(o, "GT4_RADAR_RANGE", std::to_string(std::clamp(f.radarRange, 10, 80)));
        kv(o, "GT4_RADAR_ZOOM", "off");
        kv(o, "GT4_RADAR_LABELS", f.radarLabels ? "on" : "off");
        kv(o, "GT4_RADAR_CARS", std::to_string(std::clamp(f.radarCars, 10, 100)));
        kv(o, "GT4_RADAR_BG", std::to_string(std::clamp(f.radarBg, 0, 100)));
        {
            std::array<uint32_t, 5> c = f.radarColours;
            c[2] = c[1]; // (close cars: the other cars' colour)
            kv(o, "GT4_RADAR_COLOURS", fullRadarColourList(c));
        }
        line(o, "");
        line(o, "# ---- Stored choices this GT4.exe does not read (GT4.exe skips GT4L_ lines; the launcher never passes them) ----");
        kv(o, "GT4L_NB_UPSCALER", f.scaling == FullScaling::Nis ? "nis" : f.scaling == FullScaling::Tv ? "tv" : "off");
        kv(o, "GT4L_NB_ASPECT", fullAspectBuilt(f.aspect) ? std::string("off") : std::string(fullAspectKey(f.aspect)));
        kv(o, "GT4L_NB_VSYNC", onOff(f.vsync));
        kv(o, "GT4L_NB_MIRROR_DETAIL", onOff(f.mirrorDetail));
        kv(o, "GT4L_NB_TEXTURE_LOD", f.sharpTextures ? "sharp" : "ps2");
        kv(o, "GT4L_NB_EFFECT_DETAIL", onOff(f.effectDetail));
        kv(o, "GT4L_NB_HUD_WIDE", f.hudKeepMiddle ? "middle" : "edges");
        for (int i = 0; i < kFullHudWidgets; ++i)
            kv(o, (std::string("GT4L_NB_HUD_") + kFullHudWidgetKeys[i]).c_str(), hudValue(f.widgets[static_cast<size_t>(i)]));
        kv(o, "GT4L_NB_PHONE_MIRROR", onOff(f.phoneMirror));
        kv(o, "GT4L_NB_MM_MODE", std::to_string(f.mmMode == kMmThreeWide ? 0 : std::clamp(f.mmMode, 0, 3)));
        kv(o, "GT4L_NB_MM_LAYOUT", f.mmLayout == 6 ? "6" : "3");
        kv(o, "GT4L_NB_MM_ANGLE", !f.mmAngled ? std::string("flat") : f.mmAutoAngle ? std::string("auto") : std::to_string(std::clamp(f.mmAngleDeg, 0, 90)));
        kv(o, "GT4L_NB_MM_GEOMETRY", numberText(f.mmScreenW, 1) + "," + numberText(f.mmScreenH, 1) + "," + numberText(f.mmBezel, 1) + "," +
                                         std::to_string(std::clamp(f.mmDistanceCm, 20, 400)));
        kv(o, "GT4L_NB_MM_PANORAMA", onOff(f.mmPanorama));
        kv(o, "GT4L_NB_MM_HUD", f.mmHudEvery ? "every" : "middle");
        kv(o, "GT4L_NB_MM_LIVE", onOff(f.mmLive));
        kv(o, "GT4L_NB_MM_TOP", f.mmTop == 1 ? "top" : f.mmTop == 2 ? "live" : "off");
        line(o, "");
    }

    void renderFullLauncher(std::ostream &o, const Settings &s)
    {
        const FullSettings &f = s.full;
        kv(o, "GT4L_EDITION", "full");
        kv(o, "GT4L_DISPLAY_TAB", std::to_string(std::clamp(f.displayTab, 0, 8)));
        kv(o, "GT4L_OTHER_MODES_OPEN", onOff(f.otherModesOpen));
        kv(o, "GT4L_PREVIEW_ZOOM", std::to_string(std::clamp(f.previewZoom, 1, 8)));
        std::vector<std::string> slots;
        for (int c : f.compareSlots)
            slots.push_back(std::to_string(c));
        kv(o, "GT4L_COMPARE_CARS", join(slots, ","));
        if (!f.compareStats.empty())
            kv(o, "GT4L_COMPARE_STATS", f.compareStats);
        static const char *const power[3] = {"ps", "hp", "kw"}, *const torque[3] = {"nm", "kgfm", "lbft"};
        kv(o, "GT4L_COMPARE_UNITS", std::string(power[std::clamp(f.powerUnit, 0, 2)]) + "," + torque[std::clamp(f.torqueUnit, 0, 2)]);
        kv(o, "GT4L_DEALER_PAY", onOff(f.dealerPay));
        kv(o, "GT4L_GARAGE_CARD", f.garageCard == 2 ? "2" : "1");
        if (!f.prepared.empty())
            kv(o, "GT4L_RENDERER_PREPARED", f.prepared);
        if (!f.lutFile.empty())
            kv(o, "GT4L_LUT_FILE", f.lutFile);
        kv(o, "GT4L_SOUND_MODE", f.soundMode == FullSoundMode::EveryCar ? "all" : f.soundMode == FullSoundMode::PerCar ? "cars" : "off");
        kv(o, "GT4L_SOUND_ALL", f.soundPackAll);
        kv(o, "GT4L_SOUND_CARS", fullSoundCarsList(f.soundCars));
        kv(o, "GT4L_SOUND_TAB", std::to_string(std::clamp(f.soundTab, 0, 1)));
        // A HUD widget that is off keeps its place, size, see-through and style here (GT4_HUD_<KEY>=off carries none).
        for (int i = 0; i < kFullHudLiveWidgets; ++i)
            if (FullHudWidget h = f.liveWidgets[static_cast<size_t>(i)]; !h.on)
            {
                h.on = true;
                kv(o, (std::string("GT4L_HUD_") + kFullHudLiveKeys[i]).c_str(), fullHudValue(i, h, fullAspectRatioOf(f.aspect)).substr(3));
            }
        if (!f.hudPicture.empty())
            kv(o, "GT4L_HUD_PICTURE", f.hudPicture);
    }
}
