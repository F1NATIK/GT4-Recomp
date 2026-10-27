#pragma once

// Per-pixel motion of the displayed picture (groundwork for frame generation
// above 60 fps, and for temporal upscalers): the main-view triangles of the
// last frame (gt3_motion.h: where each vertex is and was a frame ago) drawn
// into a motion texture, the nearest surface winning (depth 1/(1 + 1/w)).
// Pixels no main-view triangle covers (HUD, 2D, GT3's full-screen passes over
// empty areas) keep motion 0.

#include "gt3_motion.h"
#include "raylib.h"

#include <string>
#include <vector>

class MotionBuffer
{
public:
    bool init(std::string &error);
    void shutdown();
    // Motion texture (RG16F, frame pixels: previous position minus current)
    // of width x height covering the 640 x 448 frame.
    void build(const std::vector<gt3motion::Triangle> &triangles, int width, int height);
    unsigned texture() const { return m_tex; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    // Debug view: the motion over dst as colours (hue = direction, brightness
    // = length; 0 motion transparent).
    void drawDebug(const Rectangle &dst);

private:
    bool ensure(int width, int height);
    unsigned m_program = 0, m_vao = 0, m_vbo = 0, m_fbo = 0, m_tex = 0, m_depth = 0;
    int m_width = 0, m_height = 0;
    Shader m_debug{};
    std::vector<float> m_vertices;
};

// Frame generation (above 60 fps with GT3's 60 Hz game): the picture between
// two game frames, warped along the motion buffer of the newer one. Each
// output pixel looks up where its surface is in the newer frame (two steps
// along the motion) and in the older one; where "not moving" explains the
// change between the frames better than the motion does (the HUD, which is
// not in the motion buffer, and uniform areas), the pixel stays in place.
class FrameGenerator
{
public:
    bool init(std::string &error);
    void shutdown();
    // prev (phase 0) and cur (phase 1): frame textures of the same size with
    // the picture (picWidth x picHeight) at their top-left; motion: the
    // MotionBuffer texture of cur's picture. Returns the generated frame
    // (a texture of the same size and layout).
    const Texture2D &generate(const Texture2D &prev, const Texture2D &cur, unsigned motion, int picWidth, int picHeight, float phase);
    // Once per new game frame: the overlay mask, pixels that keep their colour
    // while the motion buffer says the scene there moves (the HUD, the mirror
    // frame, the logo), held over frames; generate() never warps them.
    void updateOverlay(const Texture2D &prev, const Texture2D &cur, unsigned motion, int picWidth, int picHeight);

private:
    Shader m_shader{}, m_overlayShader{};
    RenderTexture2D m_target{}, m_overlay[2]{};
    int m_overlayCur = 0;
    int m_locPrev = -1, m_locMotion = -1, m_locPhase = -1, m_locTexSize = -1, m_locPicSize = -1, m_locOverlay = -1;
    int m_oLocPrev = -1, m_oLocMotion = -1, m_oLocOld = -1, m_oLocTexSize = -1, m_oLocPicSize = -1;
};
