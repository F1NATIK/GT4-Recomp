# Changelog

## v0.2-wip

Everything from the test builds after v0.1-wip.

### Screens and angled triple screens
- A new **Screens** page in the launcher replaces Multi-screen and the old screen choices. You pick One screen (16:9, 21:9 or 32:9, full screen, borderless or window) or Three screens.
- **Angled side screens:** GT4 draws a separate view for each side screen, turned in by your rig's angle (0 to 70 degrees, 60 by default). The middle screen keeps full resolution, the HUD and the mirror.
- **Per-camera lineup:** each race camera has its own field of view, side angle trim, bezel setting, side up/down, zoom and tilt. Changes apply live while GT4 runs, and there's a lineup picture on F12 plus Reset this camera / Reset all cameras.
- **Get screen specs** reads your monitors' sizes from their EDID (on your PC, nothing goes online).
- The side screens now join the middle one in every camera view: bumper, bonnet, roof, close chase, chase and far chase.
- GT4's old Multi Monitor Mode (one GT4 copy per screen at 30 fps) has been removed, along with the 48:9 and "Across all monitors" options. Old settings switch over automatically.

### Speed
- The Vulkan renderer is faster on angled triples because the drawing thread no longer feeds the graphics card in many small pieces.
- The VU1 3D maths unit does the same work about 7% faster, and its results are bit-identical.
- Busy city courses such as Tokyo R246 no longer drop to a few fps, thanks to a bigger upload space on graphics cards with room for it.
- Busy courses are smoother because the drawing thread no longer reads the clock for every triangle.

### Race cameras
- The view button cycles cameras in a new order: Bumper, Bonnet, Roof, Close chase, Chase, Far chase.
- **New bonnet camera:** it sits low on the bonnet in front of the windscreen. You can tune it live on Driving > Race cameras with Up/down, Forward/back and Tilt sliders.
- GT4's own "bonnet" view sits at roof height, so it's now named Roof and placed at the base of the windscreen.
- The race cameras and the 360 look have moved to Driving > Race cameras.

### Race HUD
- Drag any HUD box with the mouse during a race, and use the mouse wheel to resize it (40 to 250%). Positions are saved.
- The FPS counter can be dragged too, and its position is saved.
- **Timing tower:** your row shows your GT Mode name, and the other cars show their real names again (including with the LAN service on). Best and last laps and laps done are now read correctly.

### Wheels and controls
- Shift up and Shift down buttons, with two slots each (for example paddles plus a sequential shifter).
- A **Shifter** device choice for a shifter on its own USB cable, such as a Simagic.
- The controller keeps working for menus while the wheel is on.

### Launcher
- Display settings use tidy dropdowns, each with a one-line hint.
- F11 toggles full screen. With three screens, it switches between the three screens and a window on the middle monitor.
- Layout fixes at 1280x800 and up. Long pages scroll, and nothing is cut off or drawn on top of something else.
- **Cheats:** the switch is now "No 365-day wait for codes". It applies the next time GT4 starts.
- **Saves:** Back up and Restore are greyed out while GT4 is running, with a note explaining why.
- **Tracks page:** untick courses to leave them out of Arcade's lists, or turn on "Unlock every course" for Arcade. Your save isn't changed.

### Graphics cards
- AMD (for example RX 9000 series): Vulkan works again, because the picture now goes through a child window as it does on NVIDIA.
- AMD with OpenGL: no more black screen after the first menu. Vulkan is still the recommended renderer.

### Not in this release
- Extra courses and cars made from other discs (Valencia, Bathurst, ESSO Supra). The code is there, but the packs aren't shipped.
- The test builds' AVX2-only speed build. This release still runs on any 64-bit PC.

### Still required
- Your own Gran Turismo 4 USA disc (SCUS-97328) as a 1:1 ISO. No game code or assets are included.
