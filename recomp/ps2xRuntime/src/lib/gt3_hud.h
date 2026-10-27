#pragma once

#include "gt3_tower_layout.h"

#include <cstdint>
#include <map>
#include <string>

// Our own race HUD: small boxes drawn over the race picture by the host (as
// the radar is), from GT3's own race state (gt3race::read: position, laps,
// lap times, positions; the player's speed, rpm, gear). Meant to replace
// GT3's HUD parts the user hides (GS::setHudHide, the launcher's HUD page).
//
// Each widget: on / off, its middle as percent of the HUD's width (x, as the
// radar: the whole picture up to 21:9, the middle screen on triple screens)
// and of the height (y), its height as percent of the picture height, the
// box's opacity, a style and (zoomed map) a range, (tower) columns and a
// time. GT3_HUD_<NAME>=on,x,y,size,opacity[,style[,range[,columns[,time]]]]
// at start (NAME: SPEED, RPM, GEAR, SHIFT, POSITION, LAP, TIMES, MAP, TOWER,
// SPLITS, ZOOMMAP, LIGHTS, ODO); live keys hud_<name>_on / _x / _y / _size /
// _alpha / _style / _range / _columns / _time (the launcher's settings file).
namespace gt3hud
{
    enum Widget
    {
        Speed,    // speed, in the HUD units (mph or km/h)
        Rpm,      // rev counter (styles below)
        Gear,     // the gear (R, a red R, for reverse: GT3's gear 0)
        Shift,    // shift lights: green, yellow, red toward the red line, flashing past it
        Position, // place / cars
        Lap,      // lap number
        Times,    // lap times (styles below)
        Map,      // the whole course (GT3's own edges) with the cars
        Tower,    // timing tower: every car's place, name and gap (the phone's timing page)
        Splits,   // the lap in three sectors: this lap's sector times against the best
        ZoomMap,  // the course around the player (range metres across)
        Lights,   // GT3's gauge lamps: ASM, TCS, parking brake, oil
        Odo,      // odometer: the distance driven this race (as GT3's gauge counts it), in the HUD units
        WidgetCount
    };
    extern const char *const kKeys[WidgetCount]; // "speed", "rpm", ...

    // Styles, per widget (0 the default):
    // Rpm: 0 bar, 1 dial, 2 arc, 3 LED strip, 4 digital.
    // Times: 0 lap / last / best, 1 lap and the live difference to the best
    //   lap (at the same point of the lap), 2 lap and best on one line.
    // Map: 0 north up, 1 turns with the car.
    // Tower: 0 gap to the leader, 1 interval to the car ahead, 2 last lap,
    //   3 a table as the phone's (place, the car's colour, car name, lap, a
    //   time - Box::timeMode -, gap - LEAD for the leader), 4 the same with
    //   "Car 1".."Car 6", 5 custom: the columns in Box::columns.
    // Splits: 0 sector times, 1 differences to the best sectors.
    // ZoomMap: 0 turns with the car, 1 north up.
    constexpr int kStyleCount[WidgetCount] = {1, 5, 1, 1, 1, 1, 3, 2, 6, 2, 2, 1, 1};
    constexpr int kTowerCustom = 5; // columns and time: gt3_tower_layout.h
    // A widget's box shape (width / height) in a style (the launcher's editor
    // has the same numbers); columns: the custom tower's.
    float aspect(int widget, int style, int columns = kTowerDefaultColumns);

    struct Box
    {
        bool on = false;
        int x = 50, y = 50; // percent (middle)
        int size = 10;      // percent of the picture height
        int opacity = 60;   // the box background, percent
        int style = 0;
        int range = 300;    // ZoomMap: metres across
        int columns = kTowerDefaultColumns; // Tower, custom style: TowerColumn bits
        int timeMode = TimeBest;            // Tower tables: TowerTime
    };
    Box &box(Widget w);
    // Speed units: 0 km/h, 1 mph (GT3's units setting).
    void setSpeedUnits(int units);
    // Live settings: the hud_* keys present; returns a note (empty: nothing changed).
    std::string applyLive(const std::map<std::string, int> &values);
    // Inside BeginDrawing: the widgets over the game picture (window pixels).
    // Nothing outside races.
    void draw(const uint8_t *rdram, float x, float y, float w, float h);
    // Stops the timing tower's name thread (before EE RAM goes away).
    void shutdown();
}
