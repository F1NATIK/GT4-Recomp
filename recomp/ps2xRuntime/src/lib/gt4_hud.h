#pragma once

// Our own race HUD widgets and start lights over GT4's picture (Full edition only, never Classic; HUD hand-off 4,
// hud/HANDOFF-4-IN-GAME-WIDGETS.md). Read-only on the game: everything comes from the race reader
// (gt4race::snapshot, gt4_race_reader.h). Drawn by the presenter after the scanline pass and all scaling, into the
// window only (the overlay route, present_overlay.h), never into GT4's frame, its photos or its replays.
//
//   GT4_HUD_RADAR / GT4_HUD_DELTA / GT4_HUD_PEDALS / GT4_HUD_GMETER = off | on,<place>,<size %>,<see-through %>
//       place: 0 bottom right, 1 bottom left, 2 top right, 3 top left, 4 bottom centre, 5 top centre, 6 above GT4's dash
//       (bottom centre, its bottom edge at 31 % of the picture's height), 7 automatic (HUD hand-off 6 A2: clear of GT4's HUD
//       in the view shown; the delta above GT4's dash in the bumper and bonnet views and at the top centre in the chase
//       views, the other widgets at the bottom right, lifted over the chase views' compact gear and speed box unless GT4's
//       panel is hidden; the view known from which of GT4's two panels GT4 shows; with GT4_MIRROR=both the chase views'
//       delta goes under GT4's mirror and race clock);
//       size 50..200; see-through 0..90. Absent: off (the launcher writes them).
//     The new form (HUD hand-off 7 A3, the launcher's Race HUD > HUD layout page): on,<x>,<y>,<size %>,<see-through %>[,<style>]
//       with x, y the box's centre in % of the HUD area (the game picture; on triple screens the middle screen), or auto,auto
//       (Automatic, place 7's rules); size 40..250; the box is kept inside the HUD area. Styles: RADAR round | square, DELTA
//       bar | number, GMETER trail | dot. The old form above is still read and placed as before.
//   HUD hand-off 7 C, boxes GT4 doesn't have (the new form only; off by default; sizes and colours in
//   launcher/gt4stock/shared/gt4_hud_boxes.h, shared with the launcher's page):
//     GT4_HUD_TOWER = on,<x>,<y>,<size>,<see>,<name car|short|driver>,<columns gap+int+best+last>,<rows 4-16>: the timing tower,
//       every car by place with its colour dot (the pages' COLORS[car % 16], you gold), gaps as hud/pages/telemetry.html works
//       them out (progress history, the time at the same progress; finished cars by total time), the rows kept around you.
//       driver: GT4's AI cars have no driver names in their race entry (looked for, read only); LAN names are not read yet, so
//       the car names show.
//     GT4_HUD_MAP = on,...,locked|rotating[,detailed|simple]: the race map with GT4's course art (simple, HUD hand-off 10:
//       only the course's centreline, the art's line, a light grey stroke with a dark edge) (pages\tracks\<course>.json next to GT4.exe, or
//       GT4_PAGES_DIR), cars placed by lap distance along its line; rotating: a round box, about 30 % of the course across, you
//       62 % down it, turned so the way you go is up. Without art (every raced course has some): nothing until the cars have
//       driven the whole lap, then the line they drove.
//     GT4_HUD_SPLITS: the split times at GT4's timing lines (car+0x5b2, D+0xd624 / D+0xd7a4: gt4_race_reader.h), purple /
//       green / yellow; the sector you are in counts up; the lap just done stays 4 s.
//     GT4_HUD_REVS = ...,bar|lights: 10 shift lights from 85 % to 98 % of the rev limit (the highest rpm held for 0.5 s this
//       race: GT4's own redline was not found), all flashing at the limiter.
//     GT4_HUD_SPEED (GT4's own unit), GT4_HUD_GEAR (N, R), GT4_HUD_LAP = ...,lap|clock (our lap counter; clock adds the race time).
//   GT4_START_LIGHTS_BOX = <x %>,<y %>,<size %>: the start lights' centre and size (absent: the top centre as before);
//   GT4_START_LOOK = row | two (two rows lit together, as GT3 has them). GT4_START_LIGHTS also reads on / off.
//   GT4_START_LIGHTS = 1 | 0, GT4_START_COUNT = 3..5 (default 5): a row of red lights at top centre that follows
//       GT4's own grid countdown (standing starts only); all go out on GT4's own start.
//
//   The widgets are shown only while a race is live (starting, racing, finished; not paused, not on the pre-race screen,
//   not in replays) and the last race frame is under 0.5 s old, inside the game picture (on triple screens the middle
//   screen's part). The maths is the Dash page's (hud/pages/telemetry.html drawDash): radar 30 m, g-meter 1.5 g at the rim.
//
//   HUD hand-off 7 D / 9, the radar (Driving > Race HUD > Radar; every key is read only with GT4_HUD_RADAR on):
//     GT4_RADAR_SHOW = always | near: near fades the radar in over 0.25 s while a car is inside the current range and out
//       2 s after the last one leaves (then over 0.25 s).
//     GT4_RADAR_RANGE = 10..80 m in 1 m steps (default 25; GT3's radar settings, 20 Oct 2026; other numbers clamped): the
//       range each race starts at.
//     GT4_RADAR_ZOOM = <list of ranges in m> | off (default off since GT3's radar settings: the launcher writes off and no
//       longer offers it; the code stays for a list set by hand): the Race HUD zoom button steps to the next larger one
//       (wrapping to the smallest), eased over 0.25 s, the new range in a small pill under the radar's centre for 1.5 s.
//     GT4_RADAR_LABELS = on | off (default on): the metres right of the top of each ring (rings at 1/3, 2/3 and the edge;
//       none when the radar is under 90 px).
//     GT4_RADAR_CARS = 10..100 (default 90): the cars' opacity (your car 10 % more, as GT3 draws it).
//     GT4_RADAR_BG = 0..100 (GT3's background opacity): the background's opacity; not set: 100 - GT4_HUD_RADAR's see-through.
//     GT4_RADAR_COLOURS = <you>,<other>,<close>,<alongside>,<background> RRGGBB (default GT3's 78dcff,ffe65a,ffe65a,ff8c00,
//       000000): alongside under 6 m and the side bars; close (under 12 m) is read but drawn in the other cars' colour.
//     GT4_HUD_ZOOM_KEY = a..z | 0..9 | none (default z) and GT4_HUD_ZOOM_PAD = none | <raylib gamepad button 1..17> (default
//       none): the Race HUD zoom button (Controllers > Controller). While the radar and its zoom are on, that key and that
//       controller button are taken from GT4's own controls (ps2_pad.cpp): Z is then not Square on the keyboard. Since GT3's
//       radar settings the launcher writes neither and the zoom is off: nothing is taken, Z is Square.
//     Tests: GT4_HUD_ZOOM_TEST=<vsync>,<vsync>,... presses the zoom at those vsyncs (the race reader's); GT4_HUD_ZOOM_SELFTEST=1
//       logs what GT4 would get with the key and the button down (ps2_pad.cpp zoomSelfTest).
//
//   GT4_HUD_HIDE = <comma list of position, lap, times, panel, map>: parts of GT4's own race HUD left out (HUD hand-off 5).
//       Each part is a member of GT4's RaceDisplay (vtable 0x67e278) whose byte +0 is a visible flag its draw (0x39f498)
//       checks (e.g. 0x39fcc4 for the position); the flag is cleared just before that draw while a race is live and put back
//       otherwise (pre-race screen, replays, menus). The one write to the game in the HUD work: what is drawn, not the race.
//       Messages (wrong way, countdown, start) are never hidden. "panel" is both of GT4's car meters: the bumper view's
//       RaceOnboardPanel (+0x108) and the chase view's compact panel (RaceSimplePanel +0xd58: gear and speed box, rev bar,
//       pedal bars, fuel, warning icons, the g bar). A part's static label goes with it: "Position" with position, "Lap"
//       with lap, "Best Lap" and "Last Lap" with times (HUD hand-off 6 A1). HUD hand-off 7 A3: clock (the race clock, member
//       +0x15f8) and besttimes (best and last lap: +0x14a8, +0x1588, their labels, and the members that draw nothing in a race)
//       hide the two halves of times; times keeps meaning both.
//

#include <cstdint>

namespace gt4hud
{
    // Whether any widget or the start lights are on (the presenter skips the call otherwise).
    bool enabled();
    // One presented frame: the window's size and the game picture's rectangle in it (window pixels).
    void draw(float screenW, float screenH, float x, float y, float w, float h);
    // GT4's RaceDisplay draw is about to run (ps2_runtime.cpp, the call to 0x39f498; rd = a0): GT4_HUD_HIDE's parts.
    void beforeRaceDisplayDraw(uint8_t *rdram, uint32_t rd);
    // The Race HUD zoom button (HUD hand-off 9): the key (raylib KEY_*) and the controller button (raylib GamepadButton) kept
    // from GT4's own controls; 0 / -1 when none (the radar off, GT4_RADAR_ZOOM=off, or nothing bound).
    int zoomKey();
    int zoomPadButton();
    // The zoom button was pressed (any thread); the presenter steps the radar's range on its next race frame.
    void zoomPress();
} // namespace gt4hud
