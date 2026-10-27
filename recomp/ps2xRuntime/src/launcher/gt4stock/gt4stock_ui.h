#pragma once

// GT4 Recompilation launcher: a small immediate-mode UI on raylib.
//
// Layout is in dp (design units): the pages are laid out for 1280 x 800 dp and
// drawn at s = min(width / 1280, height / 800) pixels per dp, so a 2560x1440
// window draws at 1.8x and a 1280x960 one at 1x. Text is rasterised at the
// final pixel size (one font atlas per size), never scaled, so it stays sharp.
// Every widget is drawn and hit-tested in the same call; the mouse is ignored
// when input is off (the hidden test mode, or behind a dialog).

#include "raylib.h"

#include <map>
#include <string>
#include <vector>

namespace gt4stock::ui
{
    struct Theme
    {
        Color bg{13, 16, 21, 255};
        Color sidebar{17, 21, 28, 255};
        Color panel{22, 27, 35, 255};
        Color panel2{31, 38, 49, 255};
        Color panel3{40, 48, 62, 255};
        Color border{44, 52, 66, 255};
        Color text{232, 236, 242, 255};
        Color muted{154, 164, 178, 255};
        Color faint{104, 114, 130, 255};
        Color accent{76, 141, 255, 255};
        Color accentDim{36, 64, 120, 255};
        Color red{215, 38, 46, 255};
        Color redHi{236, 64, 70, 255};
        Color ok{63, 185, 80, 255};
        Color warn{227, 179, 65, 255};
        Color bad{248, 81, 73, 255};
    };
    const Theme &theme();
    Color alpha(Color c, float a);
    Color mixColor(Color a, Color b, float t);

    enum class Weight
    {
        Regular,
        Semibold,
        Bold,
        Mono, // Consolas (the How it works page's code and diagram labels)
    };
    enum class Align
    {
        Left,
        Center,
        Right,
    };
    enum class Style
    {
        Primary,   // accent
        Play,      // the big red Play
        Secondary, // raised panel
        Ghost,     // text only
        Danger,
    };
    enum class Icon
    {
        Play,
        Display,
        Gamepad,
        Wheel,
        Card,
        Info,
        Folder,
        Check,
        Cross,
        Warn,
        Usb,
        Disc,
        Stop,
        Refresh,
        Camera,
        Sparkle, // 1.4: the Cheats page (a code that unlocks something)
        Eye,     // Show big
        Save,    // an arrow down onto a line: Save card / Save PDF
        Print,
#if defined(GT4L_EDITION_FULL)
        Screens, // the Full edition's Multi-screen page: three monitors
        Garage,  // its Garage page: a car seen from the side
        Network, // its Network page: three linked computers
        Trophy,  // Game progress
        Speaker, // its Sound page: a loudspeaker and two sound waves
#endif
    };

    struct Input
    {
        bool enabled = false;
        Vector2 mouse{-1e6f, -1e6f}; // pixels
        bool pressed = false, down = false, released = false;
        bool rightPressed = false;
        float wheel = 0.0f;
    };

    class Ui
    {
    public:
        Ui();
        ~Ui();
        void loadFonts(); // finds Segoe UI (C:\Windows\Fonts); raylib's font otherwise
        void unloadFonts();

        void begin(int widthPx, int heightPx, const Input &input);
        void end(); // overlays (an open drop-down list)

        float s = 1.0f;          // pixels per dp
        float width = 1280.0f;   // the window in dp
        float height = 800.0f;
        int widthPx = 1280, heightPx = 800;

        // ---- drawing (dp) ----
        Rectangle px(Rectangle dp) const;
        void fill(Rectangle r, Color c);
        void round(Rectangle r, float radius, Color c);
        void roundLine(Rectangle r, float radius, float thick, Color c);
        void line(float x1, float y1, float x2, float y2, float thick, Color c);
        void circle(float cx, float cy, float r, Color c);
        void ring(float cx, float cy, float r0, float r1, Color c);
        void gradientH(Rectangle r, Color left, Color right);
        void gradientV(Rectangle r, Color top, Color bottom);
        // A line from (x1, y1) to (x2, y2): dashed when dash > 0, with an arrowhead at the end
        // (heads 1) or at both ends (heads 2).
        void arrowLine(float x1, float y1, float x2, float y2, float thick, Color c, int heads = 1, float dash = 0.0f);
        void icon(Icon kind, Rectangle r, Color c);

        Font font(float sizeDp, Weight w);
        float textWidth(const std::string &t, float size, Weight w = Weight::Regular);
        // Draws one line; y is the top of the line box. Returns the width used.
        float text(const std::string &t, float x, float y, float size, Color c, Weight w = Weight::Regular, Align a = Align::Left);
        // Word-wrapped; returns the height used.
        float wrap(const std::string &t, float x, float y, float maxWidth, float size, Color c, Weight w = Weight::Regular,
                   float lineGap = 1.35f);
        float wrapHeight(const std::string &t, float maxWidth, float size, Weight w = Weight::Regular, float lineGap = 1.35f);
        // Shortened with "..." in the middle (paths) or the end to fit.
        std::string fit(const std::string &t, float maxWidth, float size, Weight w = Weight::Regular, bool middle = false);

        // ---- widgets ----
        bool hovered(Rectangle r) const;
        bool clicked(Rectangle r) const; // pressed this frame on r (input on)
        bool rightClicked(Rectangle r) const;
        bool button(const std::string &id, Rectangle r, const std::string &label, Style style = Style::Secondary, bool enabled = true,
                    float textSize = 14.0f);
        bool iconButton(const std::string &id, Rectangle r, Icon icon, const std::string &label, Style style, bool enabled = true,
                        float textSize = 14.0f);
        bool toggle(const std::string &id, float x, float y, bool &value, bool enabled = true); // 40 x 22 dp switch
        bool checkbox(const std::string &id, float x, float y, bool &value, const std::string &label, bool enabled = true);
        // A slider: the value snaps to step; returns true while it changes.
        bool slider(const std::string &id, Rectangle r, float &value, float lo, float hi, float step, bool enabled = true,
                    Color fillColor = Color{0, 0, 0, 0});
        bool segmented(const std::string &id, Rectangle r, const std::vector<std::string> &options, int &selected, bool enabled = true,
                       float textSize = 13.5f);
        bool dropdown(const std::string &id, Rectangle r, const std::vector<std::string> &options, int &selected, bool enabled = true,
                      float textSize = 13.5f);
        bool rowButton(const std::string &id, Rectangle r, bool selected, bool enabled = true); // a clickable row; draws its background
        void card(Rectangle r, const std::string &title, const std::string &note = std::string());
        float chip(float x, float y, const std::string &t, Color bg, Color fg, float size = 12.5f); // returns its width
        float keycap(float x, float y, const std::string &t, float size = 12.0f);                  // a key ("F2", "Esc"); returns its width
        void triangle(Vector2 a, Vector2 b, Vector2 c, Color col);                                // dp corners, any winding
        void meter(Rectangle r, float v01, Color c);
        void centreMeter(Rectangle r, float vMinus1to1, Color c);
        void statusDot(float cx, float cy, Color c);

        // ---- scrolling ----
        // A scrolling area: what is drawn between beginScroll and endScroll is clipped to the
        // viewport, and drawn `offset` dp higher (beginScroll returns it). endScroll gets the
        // content's height (it applies from the next frame), then draws the scroll bar and
        // takes the mouse wheel and the bar's drag. Hit tests inside see only the viewport.
        float beginScroll(const std::string &id, Rectangle viewport);
        void endScroll(const std::string &id, float contentHeight);
        float scrollOffset(const std::string &id) const;
        void scrollTo(const std::string &id, float offset); // clamped on the next beginScroll
        void resetScroll(const std::string &id) { scrollTo(id, 0.0f); }

        bool inputOn() const { return in_.enabled && !inputBlocked_; }
        void blockInput(bool blocked) { inputBlocked_ = blocked; }
        Vector2 mouseDp() const { return Vector2{in_.mouse.x / s, in_.mouse.y / s}; }
        const Input &input() const { return in_; }
        bool popupOpen() const { return !openDropdown_.empty(); }
        void closePopups() { openDropdown_.clear(); }
        void openPopup(const std::string &id) { openDropdown_ = id; } // the test pictures: a drop-down shown open

    private:
        struct FontKey
        {
            int px;
            int weight;
            bool operator<(const FontKey &o) const { return px != o.px ? px < o.px : weight < o.weight; }
        };
        std::map<FontKey, Font> fonts_;
        std::string fontPath_[4];
        bool haveFonts_ = false;
        Input in_;
        bool inputBlocked_ = false;
        std::string active_;       // the widget being dragged
        std::string openDropdown_; // its id
        Rectangle dropRect_{};
        std::vector<std::string> dropOptions_;
        int dropSelected_ = -1;
        int dropPicked_ = -1;      // chosen this frame (read back by dropdown())
        std::string dropPickedId_;
        float dropTextSize_ = 13.5f;
        bool dropJustOpened_ = false; // the click that opened the list never picks a row of it (developer, Prize Cars makers)
        int dropFirst_ = 0;           // the first row shown of a list longer than the window (scrolls with the wheel)
        Rectangle dropListRect(int &rows) const; // where the open list is drawn, and how many rows fit

    public:
        // Test pictures only: a drop-down drawn open (its box fills in the list when it is drawn).
        void openDropdownForTest(const std::string &id) { openDropdown_ = id; dropJustOpened_ = true; dropFirst_ = 0; }

    private:
        bool consumedClick_ = false;
        struct Scroll
        {
            float offset = 0.0f;
            float content = 0.0f;
            Rectangle view{};
        };
        std::map<std::string, Scroll> scrolls_;
        Rectangle clip_{};   // the open scrolling area (width 0: none)
        float dragGrab_ = 0.0f; // where the scroll bar's thumb was grabbed
    };
}
