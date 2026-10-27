#pragma once

// GT4 Recompilation launcher: the scanline preview.
//
// The same maths as the Stock build's scanline pass (analysis/extras/stock-scanline:
// present_scanline_math.h, the fragment shader of present_scanline.cpp and its
// Python model scanline_ref.py): picture placement (fitPicture), the moire
// guard, strength -> beam parameters, and the shader text itself. The preview
// draws a GT4 frame (640x448, as the presenter receives it) exactly as GT4.exe
// would draw it in a window of the chosen size, so a 1:1 crop of it shows the
// real lines. When the stock-scanline patch is in the tree, this file and
// present_scanline.cpp must stay identical in the shader and the placement
// (tests/scanline-preview-check: launcher renders against scanline_ref.py).

#include "raylib.h"

#include "gt4stock_settings.h"

namespace gt4stock::scan
{
    constexpr float kSigmaDark = 0.24f, kSigmaBright = 0.42f, kFaithful = 0.5f, kNarrowAtFull = 0.65f;
    constexpr int kLines = 448; // GT4's 480i frame
    // The shader's filterMode (present_scanline_math.h Filter): nearest, sharp, smooth, and the pass's
    // original look (box; the compare_shot.py renders).
    constexpr int kFilterNearest = 0, kFilterSharp = 1, kFilterSmooth = 2, kFilterBox = 3;
    inline int filterMode(PictureFilter f) { return f == PictureFilter::Nearest ? kFilterNearest : f == PictureFilter::Sharp ? kFilterSharp : kFilterSmooth; }

    struct Placement
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        int integer = 0;          // whole window rows per line (>= 2), else 0
        float rowsPerLine = 0.0f; // h / lines
    };
    Placement fitPicture(float winW, float winH, int lines, float aspect, Fit fit);
    float moireGuard(float rowsPerLine, int integer);
    void beamParams(float strength, float &mix, float &widthK);
    int rowsPerDisplayLine(int srcRows);

    class Preview
    {
    public:
        bool init();     // compiles the shader (false: the preview shows a plain picture)
        void shutdown();
        bool ready() const { return shader_.id != 0; }

        // Draws the top-left srcW x srcH texels of tex into the current target
        // at the placement, as GT4.exe does: the scanline pass, with the lines
        // (linesOn) or flat (strength 0), the picture filter (filterMode) and
        // the line blend (blend: the Blend deinterlacing of a woven picture).
        // filterMode kFilterBox with the lines off: raylib's bilinear picture.
        void draw(const Texture2D &tex, int srcW, int srcH, const Placement &place, float strength, bool linesOn, int filterMode = kFilterBox,
                  bool blend = false);

        // The whole winW x winH window GT4 would show (black around the
        // picture) into target (re-made when the size changes).
        bool renderWindow(const Texture2D &tex, int srcW, int srcH, int winW, int winH, Fit fit, float strength, bool linesOn,
                          RenderTexture2D &target, int filterMode = kFilterBox, bool blend = false);
#if defined(GT4L_EDITION_FULL)
        // The same at a given placement (the Full edition's screen shapes).
        bool renderWindowAt(const Texture2D &tex, int srcW, int srcH, int winW, int winH, const Placement &place, float strength, bool linesOn,
                            RenderTexture2D &target, int filterMode, bool blend);
#endif
        // The same, saved as a PNG (for scanline_ref.py's compare_shot.py, and
        // the picture track's own reference, picture_ref.py).
        bool renderWindowToFile(const Texture2D &tex, int srcW, int srcH, int winW, int winH, Fit fit, float strength, const char *path,
                                int filterMode = kFilterBox, bool blend = false);

    private:
        Shader shader_{};
        int locTexSize_ = -1, locRows_ = -1, locLines_ = -1, locCols_ = -1, locHalf_ = -1, locMix_ = -1, locWidth_ = -1, locField_ = -1,
            locParity_ = -1, locFilter_ = -1, locScaleX_ = -1, locBlend_ = -1;
    };
}
