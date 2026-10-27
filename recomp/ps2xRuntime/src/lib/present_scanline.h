#pragma once

#include "present_lut_math.h"
#include "present_scanline_math.h"
#include "raylib.h"

#include <vector>

// The scanline pass (GT4's TV look; the stock build's only filter): the
// display texture drawn into the window with the PS2's display lines as CRT
// beams (present_scanline_math.h), scaled by the picture filter (nearest,
// bilinear, bicubic or Lanczos) and, for an interlaced picture, its lines
// blended when the deinterlacing is Blend. One raylib shader on the window's
// context; compiled once, drawn with DrawTexturePro.
namespace ps2x::scanline
{
    // The pass's fragment shader (raylib's GLSL 330), for the Vulkan
    // renderer's window presenter (gs_vk_present.cpp), which draws the same
    // pass with Vulkan.
    const char *fragmentSource();

    class Pass
    {
    public:
        // The shader's uniforms for a draw (as draw() sets them; texWidth x
        // texHeight: the texture holding the picture).
        struct Uniforms
        {
            float texSize[2] = {0, 0};
            int rowsPerLine = 1, lines = 448, cols = 640;
            float halfPixel = 0.5f, mixBeam = 0.0f, widthK = 1.0f, field = 0.0f;
            int parity = -1, filterMode = 3, blendLines = 0;
            float lutSize = 2.0f, lutMix = 0.0f;
        };
        Uniforms uniforms(int texWidth, int texHeight, int srcW, int srcH, const Placement &placement, float strength, float field, int parity,
                          int lines = 0, Look look = {}) const;

        // Compiles the shader (logs and returns false when the driver refuses it).
        bool init();
        void shutdown();
        bool ready() const { return m_shader.id != 0; }

        // Draws the top-left srcW x srcH texels of tex (the picture) into dst.
        // placement: the fitPicture result dst came from (rows per line,
        // whole-row fit). strength 0..1 (already the user's choice; the moire
        // guard is applied here). field 0..1 with parity 0/1 (the newest
        // field's line parity) or parity -1 (unknown: no emphasis).
        // lines: the picture's display lines when known (0: counted from
        // srcH as rowsPerDisplayLine does; displayLines). look: the picture
        // filter and the line blend (present_scanline_math.h Look; the
        // default is the pass's original look).
        void draw(const Texture2D &tex, int srcW, int srcH, const Placement &placement, float strength, float field, int parity, int lines = 0,
                  Look look = {});

        // Diagnostics (PS2X_SCANLINE_SHOT): the pass drawn offscreen for a
        // width x height window (placed by fitPicture) and saved as a PNG,
        // as the window would show it. Works with a hidden window.
        bool renderToFile(const Texture2D &tex, int srcW, int srcH, int width, int height, Fit fit, float aspect, float strength, float field,
                          int parity, const char *path, int lines = 0, Look look = {});

        // Colour LUT (GT4_LUT; present_lut_math.h): the table applied to the picture's colours before the TV lines, mixed
        // in by strength 0..1. A null or empty table, or strength 0, turns it off. false: its texture was not made (off).
        bool setLut(const ps2x::lut::Table *table, float strength);
        bool lutOn() const { return m_lut.id != 0 && m_lutStrength > 0.0f; }
        // The LUT's mix for the next draws (0 = off), keeping the table (diagnostic shots: with and without it).
        void setLutStrength(float strength) { m_lutStrength = strength; }
        float lutStrength() const { return m_lutStrength; }
        // The LUT's strip (lutPoints()^2 x lutPoints() RGB floats; empty: none), for the Vulkan presenter.
        const std::vector<float> &lutStrip() const { return m_lutStrip; }
        int lutPoints() const { return m_lutPoints; }

    private:
        Shader m_shader{};
        Texture2D m_lut{};
        std::vector<float> m_lutStrip;
        float m_lutStrength = 0.0f;
        int m_lutPoints = 0;
        int m_lutTex = -1, m_lutSize = -1, m_lutMix = -1;
        int m_texSize = -1, m_rowsPerLine = -1, m_lines = -1, m_cols = -1, m_halfPixel = -1, m_mix = -1, m_widthK = -1, m_field = -1,
            m_parity = -1, m_filterMode = -1, m_blendLines = -1;
    };
}
