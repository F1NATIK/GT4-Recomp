#pragma once

// GT4's pages service (HUD and telemetry: hud/pages/FEED.md): a small HTTP server in GT4.exe that serves the pages
// folder next to GT4.exe and a live feed of the race reader's state (gt4_race_reader.h) as Server-Sent Events, 30 a
// second, for the telemetry window on another monitor and phone pages. Read-only on the game. Full edition only.
//   GT4_PAGES=off (default) | local (127.0.0.1: this PC only) | lan (the home network too; nothing goes online)
//   GT4_PAGES_PORT=<port> (default 8770), GT4_PAGES_DIR=<folder> (default: the pages folder next to GT4.exe)
//   GT4_TELEMETRY_MONITOR=<DISPLAYn>,<name>,<W>x<H>,<x>,<y> | x,y,w,h | <n> (unset/0: no window): the telemetry page full
//   screen on that monitor (Microsoft Edge in app mode, never in front of the game), telemetry.html?screen=monitor (the
//   launcher's layouts, GET /layout = telemetry-layout.json); GT4_TELEMETRY_LAYOUT (retired) only without that file.
//   GT4_TELE_BG=dark|black: the window's background. GET /config: the pages' settings from the environment.

namespace gt4pages
{
    // Starts the server (and the telemetry window) when the keys ask for it. Once, at the start of the game.
    void start();
    // Closes the telemetry window (the game is ending).
    void shutdown();
} // namespace gt4pages
