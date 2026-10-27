#pragma once

// The GT4 Recompilation launcher's Full edition: the GT4.exe keys it writes and passes on (analysis/design/full-launcher/
// FULL-LAUNCHER-PLAN.md 1.6). Header-only, like gt4_start_keys.h (this folder), whose Classic list it extends:
//   kFullKeys = the Classic allowlist (gt4startkeys::kAllowed) + kFullExtra.
// A key joins kFullExtra in the same change that makes its feature live on the launcher's pages. Everything the Full
// launcher stores for a feature GT4.exe does not have yet is a "GT4L_NB_" line: GT4.exe skips every GT4L_ line of
// gt4-start.env (main.cpp portableStart), and the launcher never passes one on. The launcher's host test
// (analysis/design/full-launcher/tests) checks that each key here is read by GT4.exe's own source (or, for the 480p
// update's three keys, by that update's patch) and that none is a test-only switch.

#include "gt4_start_keys.h"

#include <string_view>

namespace gt4fullkeys
{
    // The keys the Full edition adds to the Classic list. GT4_RACE_MODE, GT4_RACE_COLOUR and GT4_ASPECT are the 480p
    // update's (analysis/design/480p-colour DESIGN.md D4, which also puts them on the Classic list): written from the
    // Display page as that update fixed them. GT3_RENDERER: Vulkan (the Full edition draws with Vulkan). GT3_DISPLAY_ASPECT:
    // square pixels (4:3 only; a development GT4.exe reads it). GT4_ENGINE_BASS: the Sound page's "Engine bass" (off / low /
    // medium / high; a development GT4.exe reads it, gt4_sound.cpp; never on the Classic list, so the Classic edition's sound
    // is unchanged). GT3_GS_GL_SCALE and GT3_GS_GL_FAST: Display > Resolution, the internal resolution (1 native, 2, 3, 4)
    // and the drawing (0 exact, 1 fast), live with a GT4.exe whose internal resolution was checked with GT4
    // (analysis/design/full-m2, Resolution; the launcher reads its marker string). GT4_FPS_COUNTER: Display > Smoothness,
    // the FPS counter (off / simple / detailed; gt4_fps_counter.h, never on the Classic list); GT4_FPS_STATS: what detailed
    // shows (a list of fps, frametime, worst, low1, graph, cpu, gpu, ram, vram, resolution, renderer, or all).
    // GT4_CAM_FAR_CHASE (on / off) and GT4_CAM_LOOK (360 / off): Display > Cameras (full-m2, Cameras). GT4_CAM_VIEWS (the
    // view button's views, bumper,bonnet,roof,close,chase,far) and GT4_CAM_LOCK (the views locked to the car's tilt, or
    // none): Display > Cameras (full-m2, MoreCams).
    // GT4_CAR_DETAIL and GT4_WHEEL_DETAIL (game / 2x / 4x / full) and GT4_WHEEL_BLUR (on / off): Display > Detail (full-m2,
    // Detail). GT4_SCREENS (1 / 3) and GT4_SCREENS_GEOMETRY (<width>,<height>,<gap> mm): Multi-screen, three screens side by
    // side with one wide picture (full-m2, Multi). GT4_LUT (off / gt3 / a .cube file) and GT4_LUT_STRENGTH (0..100): Display >
    // Picture, Colour (analysis/design/colour-lut; the Vulkan picture only).
    // GT4_HUD_RADAR, GT4_HUD_DELTA, GT4_HUD_PEDALS and GT4_HUD_GMETER (off, or on,<place 0-7>,<size 50-200>,<see-through 0-90>)
    // and GT4_START_LIGHTS (1 / 0) with GT4_START_COUNT (3..5): Display > HUD (analysis/design/hud-pages). GT4_HUD_HIDE (the
    // parts of GT4's own race HUD hidden: a list of position, lap, times, panel, map; empty: none): Display > HUD. GT4_PAGES
    // (off / local / lan), GT4_TELEMETRY_MONITOR (0 or x,y,w,h) and GT4_TELEMETRY_LAYOUT (timing-map / timing / map / dash /
    // mirror): Multi-screen, Telemetry screen (gt4_pages.cpp). GT4_MIRROR (screen / both / page): Display > HUD, where GT4's
    // rear-view mirror shows (gt4_mirror.h). HUD hand-off 7 A3: the widgets, GT4_HUD_HIDE and GT4_MIRROR are set on Driving >
    // Race HUD > HUD layout (the widgets' new form on,<x>,<y>,<size>,<see-through>[,<style>]); GT4_START_LIGHTS_BOX
    // (<x>,<y>,<size>) and GT4_START_LOOK (row / two) place and draw the start lights there. HUD hand-off 7 B1: GT4_HUD_MOVE
    // (<part>:<x>,<y>,<size>;...) moves and resizes GT4's own HUD parts, set on the same page (gt4_hud_move.h). HUD hand-off 7
    // D / 9: GT4_RADAR_SHOW, _RANGE, _ZOOM, _LABELS, _CARS, _COLOURS (and _BG: the GT3 radar settings) from Driving > Race HUD > Radar (gt4_hud.h; the pages'
    // /config passes them on too), GT4_HUD_ZOOM_KEY and GT4_HUD_ZOOM_PAD (the Race HUD zoom button) from Controllers. HUD
    // hand-off 7 E: GT4_START_HOLD (on / off), GT4_START_DELAY (<min>,<max> s), GT4_START_AI (instant / quick / normal / slow),
    // GT4_START_CAR (held / free) and GT4_START_JUMP (h5 / h10) from Driving > Race HUD > Start lights (gt4_start_hold.h).
    inline constexpr const char *kFullExtra[] = {
        "GT4_RACE_MODE", "GT4_RACE_COLOUR", "GT4_ASPECT", // the 480p update: video mode, colour, shape
        "GT3_RENDERER",                                    // the renderer (vk)
        "GT3_DISPLAY_ASPECT",                              // square pixels
        "GT4_ENGINE_BASS",                                 // the engines' bass (Sound page)
        "GT3_GS_GL_SCALE", "GT3_GS_GL_FAST",               // internal resolution and drawing (full-m2, Resolution)
        "GT4_EFFECTS_SCALE",                               // effects resolution (effects-scale)
        "GT4_VK_PRESENT",                                  // how frames are shown (Vulkan, or the older OpenGL way)
        "GT4_DRIVE_LOG",                                   // Race Lab: the lap recorder's folder (or off)
        "GT4_AI_DRIVERS",                                  // Race Lab: the AI cars' drivers' names (its folder, or off)
        "GT4_FPS_COUNTER", "GT4_FPS_STATS",                // the FPS counter, simple / detailed and its stats (Display > Smoothness)
        "GT4_CAM_FAR_CHASE", "GT4_CAM_LOOK",               // race cameras: far chase, 360 look (full-m2, Cameras)
        "GT4_CAM_LOOK_STICK",                              // the 360 look takes the right stick in races (take / share)
        "GT4_TRIGGER_RANGE",                               // the triggers' L2 / R2 pressure at full travel (Controllers)
        "GT4_CAM_VIEWS", "GT4_CAM_LOCK",                   // the view button's views, the tilt (full-m2, MoreCams)
        "GT4_CAR_DETAIL", "GT4_WHEEL_DETAIL", "GT4_WHEEL_BLUR", // car and wheel detail, wheel-spin blur (full-m2, Detail)
        "GT4_SCREENS", "GT4_SCREENS_GEOMETRY",                   // three screens, one wide picture (full-m2, Multi)
        "GT4_LUT", "GT4_LUT_STRENGTH",                           // colour LUTs: off, the GT3 look or a .cube file (colour-lut)
        "GT4_TEX_SMOOTH",                                        // texture smoothing at 2x and above (tex-smooth)
        "GT4_SOUND_PACK", "GT4_SOUND_PACK_MAP",                  // engine sound packs: every car, per car (Sound > Sound packs)
        "GT4_HUD_RADAR", "GT4_HUD_DELTA", "GT4_HUD_PEDALS", "GT4_HUD_GMETER", // HUD widgets: radar, delta, pedals, g-meter (Display > HUD)
        "GT4_START_LIGHTS", "GT4_START_COUNT",                                // start lights over GT4's countdown (Display > HUD)
        "GT4_START_LIGHTS_BOX", "GT4_START_LOOK",                             // the start lights' box and look (Driving > Race HUD)
        "GT4_HUD_HIDE",                                                       // parts of GT4's own race HUD hidden (Display > HUD)
        "GT4_HUD_MOVE",                                                       // GT4's own race HUD parts moved and resized (Driving > Race HUD)
        "GT4_HUD_TOWER", "GT4_HUD_MAP", "GT4_HUD_SPLITS", "GT4_HUD_REVS",     // HUD hand-off 7 C: the timing tower, the race map, split times,
        "GT4_HUD_SPEED", "GT4_HUD_GEAR", "GT4_HUD_LAP",                       // rev lights, speed, gear, our lap counter (Driving > Race HUD)
        "GT4_PAGES", "GT4_TELEMETRY_MONITOR", "GT4_TELEMETRY_LAYOUT",         // the telemetry screen and phone pages (Multi-screen)
        "GT4_MIRROR",                                                         // the rear-view mirror: screen, both, page (Display > HUD)
        "GT4_TELE_BG",                                                        // the telemetry window's background: dark, black (Telemetry screen)
        "GT4_RADAR_SHOW", "GT4_RADAR_RANGE", "GT4_RADAR_ZOOM", "GT4_RADAR_LABELS", // HUD hand-off 7 D / 9: the radar's show, range, zoom,
        "GT4_RADAR_CARS", "GT4_RADAR_COLOURS",                                // labels, car opacity, colours (Driving > Race HUD > Radar)
        "GT4_RADAR_BG",                                                       // the GT3 radar settings: the background's opacity
        "GT4_HUD_ZOOM_KEY", "GT4_HUD_ZOOM_PAD",                               // the Race HUD zoom key and button (no longer written: zoom off)
        "GT4_START_HOLD", "GT4_START_DELAY", "GT4_START_AI",                  // HUD hand-off 7 E: holding the start, the lights-out wait, the
        "GT4_START_CAR", "GT4_START_JUMP",                                    // AI's reaction, your car, the jump start (Race HUD > Start lights)
    };

    // Sound packs' GT4.exe keys (Sound > Sound packs; analysis/design/sound-packs): a pack folder for every car, a map file per
    // car, or off. Play passes them only to a GT4.exe that has them (the key's name in the program: the Classic GT4.exe never
    // reads them).
    inline constexpr const char *kSoundPacks[] = {"GT4_SOUND_PACK", "GT4_SOUND_PACK_MAP"};

    // Colour LUTs' GT4.exe keys (Display > Picture, Colour; analysis/design/colour-lut): a GT4.exe without them ignores them;
    // Play passes them only to one that has them (its marker string).
    inline constexpr const char *kLut[] = {"GT4_LUT", "GT4_LUT_STRENGTH"};

    // Texture smoothing's GT4.exe key (Display > Detail and textures; analysis/design/tex-smooth): on / off, the Vulkan / GL
    // renderer at an internal resolution of 2x and above only. Play passes it only to a GT4.exe that has it (its marker).
    inline constexpr const char *kTexSmooth[] = {"GT4_TEX_SMOOTH"};

    // Extra cars (analysis/jp-cars/README-BUILD.md): no key since 2026-10-14. A Full GT4.exe serves extra-cars\esso.gt4x
    // whenever it sits next to it; GT4_EXTRA_CARS=off in the environment is a hidden way out that the launcher never writes.

    // The race cameras' GT4.exe keys: a GT4.exe without them ignores them; Play passes them only to one that has them
    // (its marker string).
    inline constexpr const char *kCameras[] = {"GT4_CAM_FAR_CHASE", "GT4_CAM_LOOK", "GT4_CAM_LOOK_STICK"};

    // More race cameras' GT4.exe keys (the view button's views, the tilt): Play passes them only to a GT4.exe that has them
    // (its marker string).
    inline constexpr const char *kMoreCameras[] = {"GT4_CAM_VIEWS", "GT4_CAM_LOCK"};

    // Car and wheel detail's GT4.exe keys: a GT4.exe without them ignores them; Play passes them only to one that has them
    // (its marker string).
    inline constexpr const char *kDetail[] = {"GT4_CAR_DETAIL", "GT4_WHEEL_DETAIL", "GT4_WHEEL_BLUR"};

    // Three screens' GT4.exe keys: a GT4.exe without them ignores them; Play passes them only to one that has them (its
    // marker string).
    inline constexpr const char *kScreens[] = {"GT4_SCREENS", "GT4_SCREENS_GEOMETRY"};

    // The internal resolution's GT4.exe keys: an older GT4.exe reads them too, but was never checked with GT4 at a scale,
    // so Play gives such a program native (GT3_GS_GL_SCALE=1) and no drawing key.
    inline constexpr const char *kInternalResolution[] = {"GT3_GS_GL_SCALE", "GT3_GS_GL_FAST"};

    // The 480p update's keys: a GT4.exe without that update ignores them (it has no reader for them).
    inline constexpr const char *kFrom480p[] = {"GT4_RACE_MODE", "GT4_RACE_COLOUR", "GT4_ASPECT"};

    // The prefix of the launcher's stored "Not built yet" choices (never passed to GT4.exe).
    inline constexpr std::string_view kNotBuiltPrefix = "GT4L_NB_";

    inline bool extraKey(std::string_view key)
    {
        for (const char *k : kFullExtra)
            if (key == k)
                return true;
        return false;
    }

    // A Full setting GT4.exe gets: on the Classic list or the Full extras, spelt exactly as listed.
    // GT4's own Multi Monitor Mode's GT4.exe keys (Multi-screen; analysis/design/multi-monitor): Play adds them to each GT4
    // copy (never stored in the settings file); GT3_SOUND_OUTPUT (no sound device) and GT4_MC0_FOLDER (a copy of the card)
    // are given to the side screens' copies. Play passes them only to a GT4.exe that has the mode (its marker string).
    inline constexpr const char *kMultiMonitor[] = {"GT4_MM_POSITION", "GT4_MM_STYLE",    "GT4_MM_MONITORS", "GT4_MM_PLAYERS",
                                                    "GT4_MM_NAME",     "GT4_MM_TARGET",   "GT4_MM_HOST",     "GT4_MM_AUTO_CONNECT",
                                                    "GT4_MM_GEOMETRY", "GT4_MM_PAD",      "GT4_MM_GROUP",    "GT4_MC0_FOLDER",
                                                    "GT3_SOUND_OUTPUT"};

    inline bool multiMonitorKey(std::string_view key)
    {
        for (const char *k : kMultiMonitor)
            if (key == k)
                return true;
        return false;
    }

    inline bool fullKey(std::string_view key) { return gt4startkeys::classicKey(key) || extraKey(key) || multiMonitorKey(key); }

    inline bool notBuiltKey(std::string_view key) { return key.substr(0, kNotBuiltPrefix.size()) == kNotBuiltPrefix; }
}
