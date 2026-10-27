#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;

// Race cameras: six views cycled with GT3's own view button (R1; keyboard E)
// or F9 / Shift+F9 - Bumper (GT3's), Bonnet, Roof, Close chase, Chase (GT3's),
// Far chase - and a look-around with the controller's right stick.
//
// GT3 has two race views, one camera mode with a bumper / chase flag. After
// GT3 computes its camera (0x209e58) the view is re-placed: the chase-based
// views move the eye along its line (close x0.6, far x2), bonnet and roof
// put it on the car (GT3 draws the player's car in its chase view), and the
// look-around turns it about the car (chase) or the driver's head.
//
// Settings: GT3_CAMERA_VIEWS=bumper,bonnet,roof,close,chase,far (the R1 cycle,
// default all), GT3_CAMERA_START=<view> (at a race's start; default GT3's
// own), GT3_CAMERA_LOOK=off|sideways|full (right stick: sideways uses its
// left-right only, so right-stick pedals still work; full takes the whole
// stick - pedals on the triggers - and the stick no longer reaches the game).
// GT3_CAMERA_BONNET / _ROOF = "ahead,up,pitch" (metres, degrees) tune them.
namespace gt3camera
{
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    // Host thread, each shown frame: F9 / Shift+F9, the right stick. `note`:
    // the view's name when it changes (for the on-screen note).
    void hostUpdate(std::string &note);
    // Live settings (the launcher's Camera page while the game runs): views
    // as bits (bumper 1 ... far chase 32; 0 / -1 unchanged), the race's start
    // view (-1 GT3's own, 0-5; -2 unchanged), look-around 0 off, 1 sideways,
    // 2 full (-1 unchanged). Returns a note for the screen, empty if nothing
    // changed.
    std::string applyLive(int views, int start, int look);
    // A race is on: GT3's race camera ran within the last 0.3 s.
    bool raceActive();
    // The angled side screen being drawn (GT3_TRIPLE_SIDES=angled): -1 left,
    // +1 right, 0 none (the main view and everything else).
    int sideScreen();
    // The whole picture's width in screen widths (3 + 2 bezel gaps): a side
    // screen's rectangle is 1 / this of it.
    float sideScreenWidths();
    // The race camera's x scale factor for the main view's rectangle this
    // frame (angled sides: the middle screen only), 1 for the whole picture.
    float mainRectScale();
    // Car detail (GT3_CAR_DETAIL, live car_detail): 0 GT3's own levels, 1 full
    // detail twice as far, 2 four times as far, 3 always full detail. Returns
    // a note, empty if unchanged.
    std::string applyCarDetail(int detail);
    // Rim detail (GT3_RIM_DETAIL, live rim_detail): the wheels' levels, same
    // steps (as car detail while unset). Returns a note, empty if unchanged.
    std::string applyRimDetail(int detail);
    // Wheel spin blur (GT3_WHEEL_BLUR=0 off, live wheel_blur): 1 GT3's own,
    // 0 the still rim while turning. Returns a note, empty if unchanged.
    std::string applyWheelBlur(int on);
}
