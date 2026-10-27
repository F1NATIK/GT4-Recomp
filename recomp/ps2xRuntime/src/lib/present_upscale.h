#pragma once

#include "raylib.h"

#include <string>
#include <vector>

// Scales the game picture to the window with AMD FidelityFX Super Resolution 1
// (EASU upscale + RCAS sharpening) or NVIDIA Image Scaling (scaler, up to 2x
// per step, + sharpening). Both are spatial: they need only the finished
// picture, which is all a PS2 game gives. (FSR 2/3/4 and DLSS are temporal and
// need per-pixel motion vectors, which GT3 never produces.)
// Shader sources: ps2xRuntime/third_party (MIT), embedded at build time.
class DisplayUpscaler
{
public:
    enum class Kind
    {
        Fsr1,
        Nis
    };

    // Compiles the shaders (OpenGL 4.3). False: error says why; draw() then refuses.
    bool init(std::string &error);
    void shutdown();
    bool ready() const { return m_ready; }

    // A part of the picture (u0..u1: fractions of its width) shown at a window
    // rectangle: triple screens with bezel correction (the picture is wider
    // than the window by the bezels; each screen shows its part).
    struct Piece
    {
        float u0, u1;
        Rectangle dst;
    };

    // Draws the top-left srcWidth x srcHeight of `frame` into `dst` (window
    // pixels, top-left origin). sharpness: 0 (least) .. 1 (most). Call between
    // BeginDrawing and EndDrawing. False: nothing drawn (caller falls back).
    // With `pieces`, the picture is scaled to `dst` (which may be wider than
    // the window) and only those parts are drawn, where they say.
    bool draw(Kind kind, const Texture2D &frame, int srcWidth, int srcHeight, Rectangle dst, float sharpness,
              const std::vector<Piece> *pieces = nullptr);

private:
    bool ensureTarget(RenderTexture2D &target, int width, int height);
    bool ensureImage(unsigned &texture, int &width, int &height, int wantWidth, int wantHeight);
    void resample(const Texture2D &source, int srcWidth, int srcHeight, RenderTexture2D &target);

    bool m_ready = false;
    Shader m_easu{}, m_rcas{}, m_resample{};
    int m_easuCon[4]{-1, -1, -1, -1}, m_rcasCon = -1, m_resampleScale = -1, m_resampleSize = -1;
    unsigned m_nisScaler = 0, m_nisSharpen = 0, m_nisConfig = 0, m_nisCoefScale = 0, m_nisCoefUsm = 0;
    unsigned m_nisOut = 0;
    int m_nisOutWidth = 0, m_nisOutHeight = 0;
    int m_nisBlockHeightScale = 24, m_nisBlockHeightSharpen = 32;
    RenderTexture2D m_a{}, m_b{};
};
