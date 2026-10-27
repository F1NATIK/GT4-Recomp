#pragma once

// GT4.exe's start keys (GT4 builds): the settings the Classic edition (GT4_STOCK_BUILD) takes from
// gt4-start.env and from its environment, the values it refuses, and GT4.exe's test and diagnostic
// switches, which the GT4 Recompilation launcher never keeps, writes or passes on.
// Header-only and free of the runtime, so the launcher reads the same lists: its copy
// src/launcher/gt4stock/shared/gt4_start_keys.h is this file byte for byte (the launcher's host test,
// analysis/gt4-stock-launcher/tests/build-test.ps1, compares them). Used by main.cpp (portableStart and
// the Classic environment check) and by the launcher's settings (gt4stock_settings.cpp isTestOnlyKey).

#include <cctype>
#include <string>
#include <string_view>

namespace gt4startkeys
{
    // The Classic edition's settings: the picture (scanlines, fit, filter, deinterlacing), the window, the disc, controls, GT4's
    // devices (phase 5b) and GT4's button codes. Exact spelling, as the launcher writes them; anything
    // else (renderer, resolution, test and diagnostic switches) is ignored and logged. The launcher's host
    // test reads this list from the source text (the string literals of the array below).
    inline constexpr const char *kAllowed[] = {
        "GT3_DISPLAY_FILTER", "GT3_CRT_SCANLINES", "GT4_SCANLINE_FIELD", "GT4_PICTURE_FIT", // picture
        "GT4_PICTURE_FILTER", "GT4_DEINTERLACE",                                         // picture: scaling, the two fields
        "GT4_RACE_MODE", "GT4_RACE_COLOUR", "GT4_ASPECT",                                 // picture: video mode, colour, shape
        "GT3_WINDOW_SIZE", "GT3_FULLSCREEN", "GT4_MONITOR",                                // window
        "GT4_DISC_IMAGE",                                                                  // disc
        "GT4_MC0", "GT4_MC1",                     // memory cards (ps2-card: folder / none / a .ps2 file)
        "PS2X_INPUT_PROFILE", "PS2X_INPUT", "GT3_VIBRATION",                               // controls
        "GT4_PAD2", "PS2X_INPUT2", "PS2X_WHEEL_PORT",                                      // player 2 (port 2)
        // the wheel and pedals (wheel-glue's PS2X_WHEEL_* keys; diagnostics such as
        // PS2X_WHEEL_LOG / PS2X_WHEEL_FFB_LOG / PS2X_WHEEL_REPLAY stay out)
        "PS2X_WHEEL", "PS2X_WHEEL_CONFIG", "PS2X_WHEEL_LINES", "PS2X_WHEEL_DEVICE", "PS2X_WHEEL_PEDALS",
        "PS2X_WHEEL_MODEL", "PS2X_WHEEL_ROTATION", "PS2X_WHEEL_LOCK", "PS2X_WHEEL_STEER_INVERT", "PS2X_WHEEL_FFB",
        "PS2X_WHEEL_FFB_GAIN", "PS2X_WHEEL_FFB_INVERT",
        "GT4_USB", "GT4_USB_ROOT", "GT4_EYETOY",                                           // devices
        // GT4's LAN battle (gt4_net.cpp): GT4_NET (refusedValue below), GT4_NET_IP (lan: this PC's address when
        // it has no private adapter with a gateway, or several; loop: a 127.x address), GT4_NET_INDEX and
        // GT4_NET_PEERS (loop: several GT4 windows on this PC, GT4's own LAN battle between them).
        "GT4_NET", "GT4_NET_IP", "GT4_NET_INDEX", "GT4_NET_PEERS",
        // GT4's button codes (analysis/cheats; off unless set): the in-memory script switches and the
        // in-game card (F2). GT4_CHEAT_CARD_TEST and GT4_CHEATS_TEST_NODAY are test switches and stay out.
        "GT4_CHEATS", "GT4_CHEAT_CARD", "GT4_CHEAT_CARD_LIST", "GT4_CHEAT_CARD_KEY", "GT4_CHEAT_CARD_PLACE", "GT4_CHEAT_CARD_FOLLOW",
    };

    inline std::string upperAscii(std::string_view s)
    {
        std::string out(s);
        for (char &c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    inline std::string lowerTrimmed(std::string_view s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t'))
            ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
            --b;
        std::string out(s.substr(a, b - a));
        for (char &c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // A Classic setting, spelt exactly as on the list.
    inline bool classicKey(std::string_view key)
    {
        for (const char *k : kAllowed)
            if (key == k)
                return true;
        return false;
    }

    // Why the Classic edition refuses this value of one of its settings; empty when it takes it. The device
    // keys whose other values are test devices, and the picture keys with a fixed list of values, are checked:
    //   PS2X_WHEEL: dinput / host (a wheel through DirectInput, the launcher's "dinput"), none / 0 / empty (no
    //     wheel). virtual / virtual1 / virtual2 (the test wheel with neutral input) and anything else: refused.
    //   GT4_NET: off / 0 / empty, lan (GT4's LAN battle between PCs), loop (GT4's LAN battle between GT4 windows
    //     on this PC, 127.x addresses). Anything else: refused (gt4_net.cpp would refuse it too).
    //   GT4_RACE_MODE: 480p / game / empty (480p). GT4_RACE_COLOUR: 16 / 32 / empty (32; 24 is no longer
    //     offered). GT4_ASPECT: 16:9 / 4:3 / game / empty (16:9). Anything else: refused (the default is used).
    inline std::string refusedValue(std::string_view key, std::string_view value)
    {
        const std::string v = lowerTrimmed(value);
        if (key == "PS2X_WHEEL")
        {
            if (v.empty() || v == "none" || v == "0" || v == "dinput" || v == "host")
                return {};
            return "a test or unknown wheel (dinput, host or none)";
        }
        if (key == "GT4_NET")
        {
            if (v.empty() || v == "off" || v == "0" || v == "lan" || v == "loop")
                return {};
            return "not a network mode (off, lan or loop)";
        }
        if (key == "GT4_RACE_MODE")
        {
            if (v.empty() || v == "480p" || v == "game")
                return {};
            return "not a race video mode (480p or game)";
        }
        if (key == "GT4_RACE_COLOUR")
        {
            if (v.empty() || v == "16" || v == "32")
                return {};
            return "not a race colour (16 or 32)";
        }
        if (key == "GT4_ASPECT")
        {
            if (v.empty() || v == "16:9" || v == "4:3" || v == "game")
                return {};
            return "not an aspect ratio (16:9, 4:3 or game)";
        }
        return {};
    }

    // GT4.exe's test and diagnostic switches that change what the game does or write files (replays, the
    // test pilot, picture and data dumps, the GL-against-CPU compare, video-mode overrides that GT4 would
    // save). The launcher drops them from its settings file and never passes them on, in any spelling
    // (Windows' environment ignores case). The Classic edition takes none of them outside a timed test run.
    inline constexpr const char *kTestOnly[] = {
        "GT4_CHEAT_CARD_TEST", "GT4_CHEATS_TEST_NODAY", "GT4_PAD2_REPLAY",         // the cheats and player-2 runtimes
        "GT4_RACE_VIDEO_MODE", "GT4_RACE_VIDEO_BLEND", "GT4_SCREEN_ASPECT",       // video-mode test switches (GT4's Options)
        "GT4_MENU_MODE", "GT4_MENU_480P_FROM", "GT4_MENU_480P_CAR",               // 480p menus: off, switch time, car views
        "GT4_AI_PILOT", "GT3_PAD_REPLAY", "PS2X_WHEEL_REPLAY",                    // driving without the player
        "PS2X_WHEEL_FFB_LOG", "GT4_USB_TRACE", "GT3_GS_THREAD",                   // diagnostics
        "GT3_RAM_DUMP", "GT3_TEXTURE_DUMP", "GT3_SOUND_CAPTURE", "GT4_IPU_DUMP", "GT3_BOOT_DUMP_PREFIX",
        "GT3_VKGL_PRESENT_DUMP", "GT3_GS_GL_DUMP_PROGRAMS", "GT3_VU1_VERIFY_DUMP", "GT3_VU0_RECORD",
    };
    // ... and every key that starts with one of these (shots, the compare's options, records, traces).
    inline constexpr const char *kTestOnlyPrefixes[] = {
        "GT3_PRESENT_SHOT", "PS2X_SCANLINE_SHOT", "GT3_WINDOW_SHOT", "GT3_FRAME_GEN_SHOT", "GT3_GS_COMPARE",
        "GT3_VU1_RECORD", "GT3_GFX_TRACE", "GT3_GS_SNAPSHOT", "GT3_VK_TEST",
    };

    inline bool testOnlyKey(std::string_view key)
    {
        const std::string k = upperAscii(lowerTrimmed(key));
        for (const char *t : kTestOnly)
            if (k == t)
                return true;
        for (const char *p : kTestOnlyPrefixes)
            if (k.compare(0, std::char_traits<char>::length(p), p) == 0)
                return true;
        return false;
    }

    // GT4.exe's own variables (and the other games' of this runtime): GT3_*, GT4_*, PS2X_*, TT_*, any case.
    // The launcher removes the inherited ones before it starts GT4.exe; the Classic edition checks them.
    inline bool ownedKey(std::string_view key)
    {
        const std::string k = upperAscii(key);
        for (const char *p : {"GT3_", "GT4_", "PS2X_", "TT_"})
            if (k.compare(0, std::char_traits<char>::length(p), p) == 0)
                return true;
        return false;
    }
}
