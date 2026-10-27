#pragma once

// Shaders of the OpenGL translation renderer (GSGlBackend).
//
// Each GS primitive is drawn as its clipped pixel bounding box; the fragment
// shader decides coverage and computes every attribute with the same
// operations, in the same order and precision, as GSCpuBackend (DrawTriangle,
// DrawSprite, sampleTexturePipe, combineTexture, writePixelPipe), so the two
// renderers can be compared pixel for pixel. `precise` stops the compiler from
// fusing the multiply-adds the CPU code does as separate float operations.

namespace gsgl
{
    inline constexpr const char *kPrimStruct = R"GLSL(
struct Prim
{
    dvec4 edgeC;   // C0 C1 C2 1/area (triangles)
    dvec4 zv;      // z0 z1 z2 (triangles), w: sprite Z
    vec4 edgeA;    // A0 A1 A2, w: float C0 (triangles)
    vec4 edgeB;    // B0 B1 B2, w: float C1
    vec4 rgba0;
    vec4 rgba1;
    vec4 rgba2;
    vec4 stqf0;
    vec4 stqf1;
    vec4 stqf2;
    vec4 uvA;      // triangles: u0 v0 u1 v1 (FST); sprites: u0f v0f du/dx dv/dy
    vec4 uvB;      // triangles: u2 v2, float C2, float 1/area; sprites: X0 Y0
    ivec4 bbox;    // x0 y0 x1 y1, inclusive pixels
    ivec4 extra;   // x: 0 triangle / 1 sprite; y: owned-edge bits; z: sprite fog; w: draw state index
};
layout(std430, binding = 0) readonly buffer Prims { Prim prims[]; };
)GLSL";

    inline constexpr const char *kVertexShader = R"GLSL(
uniform int u_firstPrim;
uniform vec2 u_targetSize;
flat out int v_prim;
flat out int v_half; // which of a box's two triangles (0: corners 0-2)
#ifdef GS_HW_TRI
// Fast mode's program for batches of triangles that sample GPU-decoded
// colour textures: each corner's attributes (with GS_TRI_GEOMETRY corner k is
// the vertex of weight 1 for attribute k), interpolated linearly in the screen
// by the GPU as the GS does, so fragments read no primitive record.
noperspective out vec4 v_stqf;
noperspective out vec4 v_rgba;
noperspective out vec2 v_uv;
flat out vec4 v_rgbaFlat;
flat out int v_state;
#ifdef GS_LIT
noperspective out vec3 v_bary; // the corner weights (GT4's per-pixel car lighting)
flat out int v_lit;
#endif
#endif
#ifdef GS_FAST_RASTER
// Fast mode: the primitive's box (its scissor) as four clip planes, so the
// GPU clips to it (no discard, which would stop early depth tests).
out float gl_ClipDistance[4];
void clipToBox(vec2 p, ivec4 bb)
{
    gl_ClipDistance[0] = p.x - float(bb.x);
    gl_ClipDistance[1] = float(bb.z + 1) - p.x;
    gl_ClipDistance[2] = p.y - float(bb.y);
    gl_ClipDistance[3] = float(bb.w + 1) - p.y;
}
#endif
void main()
{
    int id = u_firstPrim + gl_VertexID / 6;
    int corner = gl_VertexID % 6;
    v_prim = id;
    v_half = corner >= 3 ? 1 : 0;
#ifdef GS_TRI_GEOMETRY
    if (prims[id].extra.x == 0)
    {
#ifdef GS_HW_TRI
        v_stqf = corner == 0 ? prims[id].stqf0 : (corner == 1 ? prims[id].stqf1 : prims[id].stqf2);
        v_rgba = corner == 0 ? prims[id].rgba0 : (corner == 1 ? prims[id].rgba1 : prims[id].rgba2);
        v_uv = corner == 0 ? prims[id].uvA.xy : (corner == 1 ? prims[id].uvA.zw : prims[id].uvB.xy);
        v_rgbaFlat = prims[id].rgba2;
        v_state = prims[id].extra.w;
#ifdef GS_LIT
        v_bary = corner == 0 ? vec3(1.0, 0.0, 0.0) : (corner == 1 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        v_lit = prims[id].extra.z;
#endif
#endif
        // A triangle drawn as itself with conservative rasterization (every
        // pixel it touches; the fragment shader decides coverage): vertex k
        // where the other two edges meet. Corners 3-5 are left out of view.
        if (corner >= 3)
        {
            gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
#ifdef GS_FAST_RASTER
            clipToBox(vec2(-1.0e6), prims[id].bbox);
#endif
            return;
        }
        int i = (corner + 1) % 3, j = (corner + 2) % 3;
        double ai = double(prims[id].edgeA[i]), bi = double(prims[id].edgeB[i]), ci = prims[id].edgeC[i];
        double aj = double(prims[id].edgeA[j]), bj = double(prims[id].edgeB[j]), cj = prims[id].edgeC[j];
        double det = ai * bj - aj * bi;
        vec2 v = vec2(float((bi * cj - bj * ci) / det), float((aj * ci - ai * cj) / det));
#ifdef GS_FAST_RASTER
        // Fast mode: the GPU rasterizes the triangle and tests depth itself
        // (early, before shading) with vertex k's Z, interpolated as the GS does
        // (linear in the screen; depth range 0..1 = Z / 2^24). Not clamped here:
        // the GS clamps each pixel's Z after interpolating (depth clamp does).
        float vz = float(prims[id].zv[corner] / 16777216.0lf);
        gl_Position = vec4(v / u_targetSize * 2.0 - 1.0, vz, 1.0);
        clipToBox(v, prims[id].bbox);
#else
        gl_Position = vec4(v / u_targetSize * 2.0 - 1.0, 0.0, 1.0);
#endif
        return;
    }
#endif
    ivec4 bb = prims[id].bbox;
    int cx = (corner == 1 || corner == 3 || corner == 4) ? 1 : 0;
    int cy = (corner == 2 || corner == 4 || corner == 5) ? 1 : 0;
    vec2 p = vec2(cx == 1 ? float(bb.z + 1) : float(bb.x), cy == 1 ? float(bb.w + 1) : float(bb.y));
#ifdef GS_FAST_RASTER
    float sz = float(min(prims[id].zv.w, 16777215.0lf)) / 16777216.0; // a sprite's one Z
    gl_Position = vec4(p / u_targetSize * 2.0 - 1.0, sz, 1.0);
    clipToBox(p, bb);
#else
    gl_Position = vec4(p / u_targetSize * 2.0 - 1.0, 0.0, 1.0);
#endif
}
)GLSL";

    inline constexpr const char *kFragmentShader = R"GLSL(
flat in int v_prim;
flat in int v_half;
#if defined(GS_NO_ALPHA_TEST) && defined(GS_HW_TRI)
// Batches none of whose states alpha-test: nothing is discarded, so the depth
// test and write happen before shading (hidden samples cost nothing).
layout(early_fragment_tests) in;
#endif
#ifdef GS_HW_TRI
noperspective in vec4 v_stqf;
noperspective in vec4 v_rgba;
noperspective in vec2 v_uv;
flat in vec4 v_rgbaFlat;
flat in int v_state;
#ifdef GS_LIT
noperspective in vec3 v_bary;
flat in int v_lit;
#endif
#define PRIM_IS_TRIANGLE true
#else
#define PRIM_IS_TRIANGLE (P.extra.x == 0)
#endif

// Per-draw state (texture, palette, pixel pipeline), one entry per distinct
// state of a batch, so primitives with different textures share a draw call.
struct DrawState
{
    uvec2 tex;     // bindless handle: texture, or the depth target (texMode 4)
    uvec2 palette; // bindless handle: resolved CLUT 256x1, or the GPU CLUT 16x16
    ivec4 v[15];   // 13 used (256 bytes, the upload ring's element stride)
};
layout(std430, binding = 1) readonly buffer States { DrawState states[]; };
uniform int u_stateBase; // the batch's first draw state in the buffer
// The fragment's draw state: read from the buffer where each field is used
// (macros below), not copied into registers up front - the copy of all 16
// vectors kept them live through the whole program (register pressure).
int g_si;
#define g_state states[g_si]
// Internal resolution: samples per GS pixel of the targets (1 1 natively).
// gl_FragCoord is then a sample; kInvScale(n) is 1 / n.
uniform ivec2 u_scale;
// Sprites at a scale: along an axis where g_native is set, texture
// coordinates are the GS pixel's own and g_sub is this sample's place in it.
bvec2 g_native = bvec2(false);
ivec2 g_sub = ivec2(0);
// As functions of constants, not arrays: an indexed constant array went to
// local memory, written again by every fragment (9 doubles, 9 floats).
double kInvScale(int n) { return n == 2 ? 0.5lf : n == 3 ? 1.0lf / 3.0lf : n == 4 ? 0.25lf : n == 5 ? 0.2lf : n == 6 ? 1.0lf / 6.0lf : n == 7 ? 1.0lf / 7.0lf : n == 8 ? 0.125lf : 1.0lf; }
float kInvScaleF(int n) { return n == 2 ? 0.5 : n == 3 ? 1.0 / 3.0 : n == 4 ? 0.25 : n == 5 ? 0.2 : n == 6 ? 1.0 / 6.0 : n == 7 ? 1.0 / 7.0 : n == 8 ? 0.125 : 1.0; }

#ifdef GS_BINDLESS
#define TEX_S sampler2D(g_state.tex)
#define TEXRAW_S usampler2D(g_state.tex)
#define TEXDEPTH_S sampler2D(g_state.tex)
#define PALETTE_S sampler2D(g_state.palette)
#define CLUT16_S sampler2D(g_state.palette)
#else
uniform sampler2D u_tex;
uniform sampler2D u_texDepth;   // depth target (u_texMode 4)
uniform sampler2D u_palette;    // resolved CLUT, 256x1 (u_paletteMode 0)
uniform sampler2D u_clut16;     // the CLUT buffer as loaded from a GPU target, CSM1 16x16 (u_paletteMode 1)
uniform usampler2D u_texRaw;    // (u_texMode 13 is used with bindless textures only)
#define TEX_S u_tex
#define TEXRAW_S u_texRaw
#define TEXDEPTH_S u_texDepth
#define PALETTE_S u_palette
#define CLUT16_S u_clut16
#endif

// Draw state fields (read-only; each use reads the buffer, which the GPU caches).
#define u_tme (g_state.v[0].x)
#define u_fst (g_state.v[0].y)
#define u_iip (g_state.v[0].z)
#define u_fge (g_state.v[0].w)
#define u_tfx (g_state.v[1].x)
#define u_tcc (g_state.v[1].y)
#define u_texSize (g_state.v[1].zw)      // TW, TH in texels
#define u_linear (g_state.v[2].x)
#define u_wrap (g_state.v[2].yz)         // CLAMP.WMS, WMT
// Texture modes: 0 decoded RGBA texels; render targets read as: 1 CT32, 2 CT24
// (TEXA alpha), 3 CT16 (TEXA alpha), 4 Z24/Z32 from a depth target, 5 Z24
// from a Z-format colour target, 6 Z32 from a Z-format colour target; 7-11
// T8H/T4HL/T4HH/T8/T4 views of a 32-bit target; 12 decoded indices; 13 raw
// local memory (fetchRaw); 14 the raw values decoded once on the GPU
// (gpuDecodedTexture); 15 fast mode: the texels' colours decoded once on the
// GPU (gpuColourTexture), filtered by the GPU's own bilinear sampler
// (texture wrap REPEAT; clamps by coordinate)
#define u_texMode (g_state.v[2].w)
#define u_region (g_state.v[3])          // MINU MAXU MINV MAXV
#define u_texa (g_state.v[4].xyz)        // TA0, AEM, TA1
#define u_paletteMode (g_state.v[4].w)
#define u_clut (g_state.v[5].xyz)        // CSA, 4-bit texture, CLUT is CT24 (TEXA alpha)
#define u_fbmsk (uint(g_state.v[5].w))
#define u_fogColor (g_state.v[6].xyz)
#define u_frameFormat (g_state.v[6].w)   // 0 CT32, 1 CT24, 2 CT16, 3 Z24, 4 Z32
#define u_ate (g_state.v[7].x)
#define u_atst (g_state.v[7].y)
#define u_aref (g_state.v[7].z)
#define u_zWrite (g_state.v[7].w)        // the draw tests or writes Z (gl_FragDepth)
#define u_blend (g_state.v[8])           // ALPHA A B C D selectors
#define u_abe (g_state.v[9].x)
#define u_pabe (g_state.v[9].y)
#define u_fix (g_state.v[9].z)
#define u_fba (g_state.v[9].w)
#define u_dstRead (g_state.v[10].x)      // blending / DATE / bit FBMSK read the target
#define u_date (g_state.v[10].y)
#define u_datm (g_state.v[10].z)
#define u_hwBlend (g_state.v[10].w)      // fast mode: the GPU's blend unit blends (output Cs*ks, second source kd)
#define u_texPages (g_state.v[11])
#ifdef GS_EFFECTS_SCALE
#define u_texTarget ivec3(g_state.v[12].x, g_state.v[12].y, g_state.v[12].z & 0xFFFF)
#define u_clutOrigin ivec2(g_state.v[14].x & 0xFFFF, g_state.v[14].y) // paletteMode 1: the CLUT's 16x16 square in its texture (GS pixels)
// Effects at their own scale (GT4_EFFECTS_SCALE, analysis/design/effects-scale): the samples per GS pixel of a
// target read as the texture (v[12].z bits 16-23: x | y << 4) and of a CLUT read from a target (v[14].x bits
// 16-23); 0: the destination's (u_scale), as always before.
ivec2 scaleField(int word) { int s = (word >> 16) & 0xFF; return s == 0 ? u_scale : ivec2(s & 15, s >> 4); }
#define texScale scaleField(g_state.v[12].z)
#define paletteScale scaleField(g_state.v[14].x)
// A sample's place in its GS pixel at the destination's scale, at another scale.
ivec2 subAt(ivec2 sub, ivec2 scale) { return scale == u_scale ? sub : (sub * scale) / u_scale; }
#else
#define u_texTarget (g_state.v[12].xyz)
#define u_clutOrigin (g_state.v[14].xy)  // paletteMode 1: the CLUT's 16x16 square in its texture (GS pixels)
#define texScale u_scale
#define paletteScale u_scale
#define subAt(sub, scale) (sub)
#endif
#ifdef GS_SPEC_TEX15
// Fast mode's lean program: every state of the batch samples a GPU-decoded
// colour texture (texMode 15) and reads no target, so the compiler drops the
// other texture paths and the destination reads (fewer registers).
#undef u_tme
#define u_tme 1
#undef u_texMode
#define u_texMode 15
#undef u_dstRead
#define u_dstRead 0
#undef u_date
#define u_date 0
#endif
// Per-batch specialisation (fast mode): when every state of a batch shares a
// field, the program built for that batch has it as a constant (SPEC_*), so
// the compiler drops the other paths and the per-fragment state reads.
#ifdef SPEC_FST
#undef u_fst
#define u_fst SPEC_FST
#endif
#ifdef SPEC_IIP
#undef u_iip
#define u_iip SPEC_IIP
#endif
#ifdef SPEC_FGE
#undef u_fge
#define u_fge SPEC_FGE
#endif
#ifdef SPEC_TFX
#undef u_tfx
#define u_tfx SPEC_TFX
#endif
#ifdef SPEC_TCC
#undef u_tcc
#define u_tcc SPEC_TCC
#endif
#ifdef SPEC_WRAPU
#undef u_wrap
#define u_wrap ivec2(SPEC_WRAPU, SPEC_WRAPV)
#endif
#ifdef SPEC_ATE
#undef u_ate
#define u_ate SPEC_ATE
#endif
#ifdef SPEC_ATST
#undef u_atst
#define u_atst SPEC_ATST
#endif
#ifdef SPEC_ABE
#undef u_abe
#define u_abe SPEC_ABE
#endif
#ifdef SPEC_PABE
#undef u_pabe
#define u_pabe SPEC_PABE
#endif
#ifdef SPEC_BLEND_A
#undef u_blend
#define u_blend ivec4(SPEC_BLEND_A, SPEC_BLEND_B, SPEC_BLEND_C, SPEC_BLEND_D)
#endif
#ifdef SPEC_FBA
#undef u_fba
#define u_fba SPEC_FBA
#endif
#ifdef SPEC_HWBLEND
#undef u_hwBlend
#define u_hwBlend SPEC_HWBLEND
#endif
#ifdef SPEC_FRAME_FORMAT
#undef u_frameFormat
#define u_frameFormat SPEC_FRAME_FORMAT
#endif
#ifdef SPEC_FBMSK
#undef u_fbmsk
#define u_fbmsk SPEC_FBMSK
#endif

void loadState(int index)
{
    g_si = index;
}

// A paletted texel's colour (modes 7-12: indices from a target or a decoded index texture).
uvec4 paletteLookup(uint index)
{
    if (u_paletteMode == 0)
        return uvec4(round(texelFetch(PALETTE_S, ivec2(int(index), 0), 0) * 255.0));
    uint entry = (u_clut.y != 0) ? (uint(u_clut.x) * 16u + (index & 15u)) : ((uint(u_clut.x) & 15u) * 16u + index);
    entry &= 255u;
    uint pos = (entry & ~0x18u) | ((entry & 0x08u) << 1) | ((entry & 0x10u) >> 1); // CSM1 swaps index bits 3 and 4
    // (u_clutOrigin: the CLUT's square in the texture, when it is read straight from its target.)
    // At a scale the CLUT holds the samples of the pixels it was loaded from.
    // A lookup reads the entry's middle sample; but a whole-step nearest
    // sprite (g_native) reads the sample at this sample's own place, so a
    // picture passed through a palette keeps its detail (GT3's colour pass
    // reads the frame as 8-bit indices and writes it back through CLUTs
    // loaded from the result: every race frame, before the HUD). Uploaded
    // CLUTs have the same value in every sample, so nothing else changes.
    // (GT3_GS_GL_SHADER_DEFINES=GS_NO_CLUT_SAMPLES: the middle sample always.)
#ifndef GS_NO_CLUT_SAMPLES
    ivec2 pScale = paletteScale;
    ivec2 pSub = subAt(g_sub, pScale);
    ivec2 clutSub = ivec2(g_native.x ? pSub.x : pScale.x / 2, g_native.y ? pSub.y : pScale.y / 2);
#else
    ivec2 pScale = paletteScale;
    ivec2 clutSub = pScale / 2;
#endif
    uvec4 c = uvec4(round(texelFetch(CLUT16_S, (ivec2(int(pos & 15u), int(pos >> 4)) + u_clutOrigin) * pScale + clutSub, 0) * 255.0));
    if (u_clut.z == 2)
    {
        // A CT16 CLUT copied on the GPU (gpuClut16): the halfword in R, G.
        uint h = c.r | (c.g << 8);
        c = uvec4((h & 31u) << 3, ((h >> 5) & 31u) << 3, ((h >> 10) & 31u) << 3, 0u);
        bool rgbZero16 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (h & 0x8000u) != 0u ? uint(u_texa.z) : ((u_texa.y != 0 && rgbZero16) ? 0u : uint(u_texa.x));
    }
    else if (u_clut.z != 0)
    {
        bool rgbZero = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (u_texa.y != 0 && rgbZero) ? 0u : uint(u_texa.x);
    }
    return c;
}
uniform int u_alphaPass;        // 0: fragments passing the alpha test, 1: failing ones
uniform sampler2D u_dst;        // the target (texture barrier) or a copy of it

layout(location = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_color1; // fast mode: the blend unit's destination factor
#ifdef GS_INTERLOCK
layout(pixel_interlock_ordered) in;
layout(r32ui, binding = 1) uniform coherent uimage2D u_dstImage; // the target's RGBA8 words
uniform sampler2D u_zbuf;       // the depth target (not written by these draws)
uniform int u_zTest;            // 0 none, 1 ALWAYS, 2 GEQUAL, 3 GREATER
uniform int u_colorMaskBits;    // the batch's RGBA write mask
#endif

int clampU8(int v) { return clamp(v, 0, 255); }
// GS_SHADE_EXACT (every GT4 build and resolution, developer 2026-10-10; analysis/design/tex-smooth, "The dot grid"): the
// Gouraud colour gets 1/128 of a level of slack before it is truncated. A colour that is the
// same at all three vertices (GT4's vertex alpha 0x80 on Trial Mountain's rock faces) then stays that value where the
// float interpolation lands a hair below it (the GS's fixed-point DDA keeps it exact). Without it those pixels drop to
// 0x7f, fail GT4's alpha test (A > 0x7f, AFAIL RGB only: no Z), and the forest drawn behind later shows through them as a
// grid of dark dots (at 1x a scattered speckle).
vec4 shadeSlack(vec4 c)
{
#ifdef GS_SHADE_EXACT
    return c + vec4(1.0 / 128.0);
#endif
    return c;
}

float fabsQ(float q) { return (abs(q) > 1.0e-8) ? q : 1.0; }

// IEEE single-precision 1/q, as the CPU's 1.0f / q.
float recipExact(float q) { return float(1.0lf / double(q)); }

int wrapCoord(int c, int size, int mode, int rmin, int rmax)
{
    if (mode == 0) return int(uint(c) & uint(size - 1));
    if (mode == 1) return clamp(c, 0, size - 1);
    if (mode == 2) return min(max(c, rmin), rmax);
    return int((uint(c) & uint(rmin)) | uint(rmax));
}

// A render target sampled as the texture, page by page: the texture's base
// page, pages per row and page size (its own format), and the target's base
// page, width in pages and page height. Views: 10 PSMT8, 11 PSMT4 of a 32-bit
// target (u_lut8 / u_lut4 give each texel's pixel and byte in a CT32 page).
uniform usampler2D u_lut8;
uniform usampler2D u_lut4;

// A texture read from raw local memory (u_texMode 13), addressed as GSMem
// does: TEXRAW_S holds u_texTarget.y pages from page u_texTarget.x (64 words
// a row, 32 rows a page); u_texPages is TBP0, TBW, PSM. u_addrLut: each
// format's pixel offset inside a page at block 0 (PageTable*[0]): PSMT4 rows
// 0-127, PSMT8 128-191, 32-bit 192-223, PSMCT16 224-287, PSMCT16S 288-351.
uniform usampler2D u_addrLut;

// A raw texel value (shifted to bit 0) as a colour: palette lookup, 16-bit
// expansion or TEXA alpha by the texture's format (u_texPages.z).
uvec4 rawColor(uint value)
{
    int psm = u_texPages.z;
    if (psm == 0x13 || psm == 0x1B)
        return paletteLookup(value & 0xFFu);
    if (psm == 0x14 || psm == 0x24 || psm == 0x2C)
        return paletteLookup(value & 0x0Fu);
    uvec4 c;
    if (psm == 0x02 || psm == 0x0A)
    {
        c = uvec4((value & 31u) << 3, ((value >> 5) & 31u) << 3, ((value >> 10) & 31u) << 3, 0u);
        bool rgbZero16 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (value & 0x8000u) != 0u ? uint(u_texa.z) : ((u_texa.y != 0 && rgbZero16) ? 0u : uint(u_texa.x));
        return c;
    }
    c = uvec4(value & 255u, (value >> 8) & 255u, (value >> 16) & 255u, value >> 24);
    if (psm == 0x01)
)GLSL"
R"GLSL(    {
        bool rgbZero24 = c.r == 0u && c.g == 0u && c.b == 0u;
        c.a = (u_texa.y != 0 && rgbZero24) ? 0u : uint(u_texa.x);
    }
    return c;
}

// The texture's layout for fetchRaw (once per bilinear footprint): base
// block and width, page width/height, texels per page and block, bits per
// texel (all powers of two, as shifts: a GPU has no fast integer division).
struct RawLayout
{
    uint bp, bw, pwS, phS, row, pppS, ppbS, widthS, bitOffset;
};
RawLayout rawLayout()
{
    int psm = u_texPages.z;
    RawLayout L = RawLayout(uint(u_texPages.x), uint(u_texPages.y), 6u, 5u, 192u, 11u, 6u, 5u, 0u);
    if (psm == 0x14) { L.pwS = 7u; L.phS = 7u; L.row = 0u; L.pppS = 14u; L.ppbS = 9u; L.widthS = 2u; }
    else if (psm == 0x13) { L.pwS = 7u; L.phS = 6u; L.row = 128u; L.pppS = 13u; L.ppbS = 8u; L.widthS = 3u; }
    else if (psm == 0x02) { L.phS = 6u; L.row = 224u; L.pppS = 12u; L.ppbS = 7u; L.widthS = 4u; }
    else if (psm == 0x0A) { L.phS = 6u; L.row = 288u; L.pppS = 12u; L.ppbS = 7u; L.widthS = 4u; }
    else if (psm == 0x1B || psm == 0x24) L.bitOffset = 24u;
    else if (psm == 0x2C) L.bitOffset = 28u;
    return L;
}

uvec4 rawTexel(RawLayout L, int u, int v)
{
    uint uu = uint(u), vv = uint(v);
    uint page = (L.bp >> 5) + (vv >> L.phS) * ((L.bw << 6) >> L.pwS) + (uu >> L.pwS);
    uint offset = texelFetch(u_addrLut, ivec2(int(uu & ((1u << L.pwS) - 1u)), int(L.row + (vv & ((1u << L.phS) - 1u)))), 0).r;
    uint bits = (((page << L.pppS) + ((L.bp & 31u) << L.ppbS) + offset) << L.widthS) + L.bitOffset;
    uint rel = (((bits >> 5) & 0xFFFFFu) - uint(u_texTarget.x) * 2048u) & 0xFFFFFu;
    if (rel >= uint(u_texTarget.y) * 2048u)
        return uvec4(0u);
    uint value = texelFetch(TEXRAW_S, ivec2(int(rel & 63u), int(rel >> 6)), 0).r >> (bits & 31u);
    return rawColor(value);
}

uvec4 fetchRaw(int u, int v)
{
    return rawTexel(rawLayout(), u, v);
}

// Texel (u, v); sub: which of its samples, for a target read at the internal
// resolution (0 0 natively; a clamped coordinate takes the edge sample).
uvec4 fetchTexel(int u, int v, ivec2 sub)
{
    int wu = wrapCoord(u, u_texSize.x, u_wrap.x, u_region.x, u_region.y);
    int wv = wrapCoord(v, u_texSize.y, u_wrap.y, u_region.z, u_region.w);
    if (wu != u && (u_wrap.x == 1 || u_wrap.x == 2))
        sub.x = wu > u ? 0 : texScale.x - 1;
    if (wv != v && (u_wrap.y == 1 || u_wrap.y == 2))
        sub.y = wv > v ? 0 : texScale.y - 1;
    u = wu;
    v = wv;
    if (u_texMode == 13)
        return fetchRaw(u, v);
    if (u_texMode == 14)
        return rawColor(texelFetch(TEXRAW_S, ivec2(u, v), 0).r); // decoded once on the GPU
    if (u_texMode == 0 || u_texMode == 15)
        return uvec4(round(texelFetch(TEX_S, ivec2(u, v), 0) * 255.0));
    if (u_texMode == 12)
        return paletteLookup(uint(round(texelFetch(TEX_S, ivec2(u, v), 0).r * 255.0)));

    // Page size (powers of two) as shifts; u and v are not negative here.
    int pwS = findLSB(u_texPages.z), phS = findLSB(u_texPages.w);
    int page = (u_texPages.x + (v >> phS) * u_texPages.y + (u >> pwS)) & 511;
    int r = (page - u_texTarget.x) & 511;
    // r / FBW without an integer division (slow on a GPU): (r + 0.5) / FBW is
    // at least 0.5 / 63 away from a whole number, far more than the float
    // error for r < 512, so the truncation is exact.
    int rowIndex = int((float(r) + 0.5) / float(u_texTarget.y));
    int tx = (r - rowIndex * u_texTarget.y) * 64, ty = rowIndex * u_texTarget.z;
    int lu = u & (u_texPages.z - 1), lv = v & (u_texPages.w - 1);

    if (u_texMode == 10 || u_texMode == 11)
    {
        uint e = (u_texMode == 10) ? texelFetch(u_lut8, ivec2(lu, lv), 0).r : texelFetch(u_lut4, ivec2(lu, lv), 0).r;
        uvec4 word = uvec4(round(texelFetch(TEX_S, ivec2(tx + int(e & 63u), ty + int((e >> 6) & 31u)) * texScale + sub, 0) * 255.0));
        uint b = word[int((e >> 11) & 3u)];
        uint index = (u_texMode == 10) ? b : ((((e >> 13) & 1u) != 0u) ? (b >> 4) : (b & 0x0Fu));
        return paletteLookup(index);
    }
    ivec2 p = ivec2(tx + lu, ty + lv) * texScale + sub;
    if (u_texMode == 4)
    {
        uint zv = min(uint(round(texelFetch(TEXDEPTH_S, p, 0).r * 16777216.0)), 16777215u);
        return uvec4(zv & 0xFFu, (zv >> 8) & 0xFFu, (zv >> 16) & 0xFFu, 0u);
    }
    uvec4 c = uvec4(round(texelFetch(TEX_S, p, 0) * 255.0));
    // A CT16 target read as a texture: its 16-bit value (GT4's race colour keeps the full colour in these
    // texels, gs_fullcolour.h; GS memory and texture reads see only the 5-bit channels and alpha bit), or, with
    // v[13].z bit 1 (DrawKey::texFull: a display buffer read back in 480p / 1080i, race colour 32), the texel's
    // full colour; the alpha (bit 15, TEXA, AEM on the 16-bit RGB) is the 16-bit value's either way. (A state
    // word bit, not a new #define: the Vulkan pipeline list matches programs by their #define lines.)
    uvec4 c16 = c & uvec4(0xF8u, 0xF8u, 0xF8u, 0x80u);
    if (u_texMode == 3 && (g_state.v[13].z & 2) == 0)
        c = c16;
    bool rgbZero = c.r == 0u && c.g == 0u && c.b == 0u;
    if (u_texMode == 2)
        c.a = (u_texa.y != 0 && rgbZero) ? 0u : uint(u_texa.x);
    else if (u_texMode == 3)
        c.a = c16.a != 0u ? uint(u_texa.z) : ((u_texa.y != 0 && c16.r == 0u && c16.g == 0u && c16.b == 0u) ? 0u : uint(u_texa.x));
    else if (u_texMode == 5)
        c.a = 0u;
    else if (u_texMode >= 7)
    {
        uint index = u_texMode == 7 ? c.a : (u_texMode == 8 ? (c.a & 0x0Fu) : (c.a >> 4));
        c = paletteLookup(index);
    }
    return c;
}

// Sample (su, sv) of a target read at the internal resolution: its texel
// (rounded down) and the sample within it.
uvec4 fetchSample(int su, int sv)
{
    // Floor division by the scale (no integer division): (s + 0.5) / scale
    // is at least 1/16 away from a whole number, far above the float error.
    ivec2 ts = texScale;
    int u = int(floor((float(su) + 0.5) * kInvScaleF(ts.x)));
    int v = int(floor((float(sv) + 0.5) * kInvScaleF(ts.y)));
    return fetchTexel(u, v, ivec2(su - u * ts.x, sv - v * ts.y));
}

uvec4 lerpTexels(uvec4 c00, uvec4 c10, uvec4 c01, uvec4 c11, float fx, float fy)
{
    precise vec4 a = vec4(c00), b = vec4(c10), c = vec4(c01), d = vec4(c11);
    precise vec4 top = a + (b - a) * fx;
    precise vec4 bottom = c + (d - c) * fx;
    precise vec4 value = top + (bottom - top) * fy;
    ivec4 whole = ivec4(value);
    precise vec4 rest = value - vec4(whole);
    ivec4 rounded = whole + ivec4(greaterThanEqual(rest, vec4(0.5)));
    return uvec4(clamp(rounded, ivec4(0), ivec4(255)));
}

#if defined(GS_MIPMAP) && !defined(GS_SPEC_TEX15)
// GS mipmapping (gs_mip.h; analysis/phase5/specs/mipmap-design.md 4.4): a
// per-pixel mip primitive's state has u_mipCtl != 0 (gsmip::packGlCtl: bits 0-2
// MXL, 3-5 MMIN, 6 MMAG, 7 LCM, 8-9 L, 10 set, 16-27 K; gsmip::packGlRange: bit
// 15 its level range in use, 12-14 lowest level, 28-30 highest) and samples raw memory
// (texMode 13) through one texture over every level's pages; levels 1-5 as
// TBP | TBW << 14 in mipWord. The LOD and the level choice are integer copies
// of gsmip::log2Fixed16 / lod16 / pick, the texel coordinates the CPU's float
// operations, so at native scale GL and the CPU reference agree exactly.
#define u_mipCtl (g_state.v[12].w)
uniform usampler2D u_log2Lut; // gsmip::kLog2Lut, 1025x1
// PS2X_GS_MIP_TINT (u_mipCtl bit 11; a primitive resolved to level n >= 1 has
// only bit 11 and n in bits 12-14): texel RGB times gsmip::tint's colour.
uvec4 mipTint(uvec4 c, int lvl)
{
    uvec3 m = lvl == 1 ? uvec3(256u, 96u, 96u) : lvl == 2 ? uvec3(96u, 256u, 96u) : lvl == 3 ? uvec3(96u, 96u, 256u)
            : lvl == 4 ? uvec3(256u, 256u, 96u) : lvl == 5 ? uvec3(256u, 96u, 256u) : lvl == 6 ? uvec3(96u, 256u, 256u) : uvec3(256u);
    return uvec4((c.rgb * m) >> 8u, c.a);
}
int mipWord(int n)
{
    return n == 1 ? g_state.v[13].x : n == 2 ? g_state.v[13].y : n == 3 ? g_state.v[13].w : n == 4 ? g_state.v[14].z : g_state.v[14].w;
}
int mipLog2Fixed16(float aq)
{
    uint b = floatBitsToUint(aq) & 0x7FFFFFFFu;
    int e = int(b >> 23) - 127;
    uint m = b & 0x7FFFFFu;
    int i = int(m >> 13);
    uint f = m & 0x1FFFu;
    int lo = int(texelFetch(u_log2Lut, ivec2(i, 0), 0).r);
    int hi = int(texelFetch(u_log2Lut, ivec2(i + 1, 0), 0).r);
    return e * 65536 + lo + int((uint(hi - lo) * f) >> 13);
}
// (level, weight of level + 1 in 1/256, linear filter within the level).
ivec3 mipPick(int lod, int ctl)
{
    int mxl = ctl & 7, mmin = (ctl >> 3) & 7;
    if (lod <= 0)
        return ivec3(0, 0, (ctl >> 6) & 1);
    int lin = (mmin == 1 || (mmin & 4) != 0) ? 1 : 0;
    if (mxl == 0 || mmin < 2 || mmin > 5)
        return ivec3(0, 0, lin);
    int c = min(lod, mxl << 16);
    if (mmin == 2 || mmin == 4)
        return ivec3(min((c + 0x8000) >> 16, mxl), 0, lin);
    int n = c >> 16;
    return ivec3(n, n >= mxl ? 0 : (c & 0xFFFF) >> 8, lin);
}
// Level n at (su, sv) = (S/Q, T/Q): its base / width from mipWord, size halved
// (never below 2), CLAMP region shifted; nearest and bilinear as sampleTexture's
// raw path (texMode 13).
uvec4 mipSampleLevel(int n, float su, float sv, bool lin)
{
    RawLayout L = rawLayout();
    ivec2 size = u_texSize;
    ivec4 rg = u_region;
    if (n > 0)
    {
        int w = mipWord(n);
        L.bp = uint(w & 0x3FFF);
        L.bw = uint((w >> 14) & 0x3F);
        int tw = findMSB(u_texSize.x), th = findMSB(u_texSize.y);
        size = ivec2(tw > n ? 1 << (tw - n) : 2, th > n ? 1 << (th - n) : 2);
        rg = rg >> n;
    }
    precise float texUf = su * float(size.x);
    precise float texVf = sv * float(size.y);
    // Nearest: the texel under (texUf, texVf); bilinear: the 2x2 around it less
    // half a texel, as sampleTexture's raw path.
    int u0, v0;
    precise float fx = 0.0, fy = 0.0;
    if (lin)
    {
        precise float sampleU = texUf - 0.5;
        precise float sampleV = texVf - 0.5;
        u0 = int(floor(sampleU));
        v0 = int(floor(sampleV));
        fx = sampleU - float(u0);
        fy = sampleV - float(v0);
    }
    else
    {
        u0 = int(texUf);
        v0 = int(texVf);
    }
    int ua = wrapCoord(u0, size.x, u_wrap.x, rg.x, rg.y);
    int ub = wrapCoord(u0 + 1, size.x, u_wrap.x, rg.x, rg.y);
    int va = wrapCoord(v0, size.y, u_wrap.y, rg.z, rg.w);
    int vb = wrapCoord(v0 + 1, size.y, u_wrap.y, rg.z, rg.w);
    // The taps in a loop, so rawTexel (with its palette lookup) is in the
    // program once: with a copy per call (and two sampleTexture calls) the
    // renderer's start-up with the driver's shader cache cold took ~97 s
    // instead of ~24 s (~13 s without GS_MIPMAP; RTX 5060 Laptop, 2026-10-04).
    uvec4 c00 = uvec4(0u), c10 = uvec4(0u), c01 = uvec4(0u), c11 = uvec4(0u);
    int taps = lin ? 4 : 1;
    for (int k = 0; k < taps; ++k)
    {
        uvec4 c = rawTexel(L, (k & 1) != 0 ? ub : ua, (k & 2) != 0 ? vb : va);
        if (k == 0)
            c00 = c;
        else if (k == 1)
            c10 = c;
        else if (k == 2)
            c01 = c;
        else
            c11 = c;
    }
    return lin ? lerpTexels(c00, c10, c01, c11, fx, fy) : c00;
}
uvec4 sampleMip(float s, float t, float q)
{
#ifdef GS_FAST_RASTER
    float invQ = 1.0 / fabsQ(q);
#else
    float invQ = u_scale == ivec2(1) ? recipExact(fabsQ(q)) : 1.0 / fabsQ(q);
#endif
    precise float su = s * invQ;
    precise float sv = t * invQ;
    int ctl = u_mipCtl;
    int kRaw = (ctl >> 16) & 0xFFF;
    int k16 = (kRaw >= 0x800 ? kRaw - 0x1000 : kRaw) * 4096;
    int lod = ((ctl >> 7) & 1) != 0 ? k16 : k16 - mipLog2Fixed16(abs(fabsQ(q))) * (1 << ((ctl >> 8) & 3));
    ivec3 p = mipPick(lod, ctl);
    // gsmip::confine: kept inside the levels the primitive was given, the
    // only ones whose pages the raw texture holds (at an internal resolution
    // the float edge functions can carry Q past the vertices' range).
    if ((ctl & 0x8000) != 0)
    {
        int lo = (ctl >> 12) & 7, hi = (ctl >> 28) & 7, mmin = (ctl >> 3) & 7;
        if (p.x < lo)
            p = ivec3(lo, 0, (mmin == 1 || (mmin & 4) != 0) ? 1 : 0);
        else if (p.x >= hi)
            p = ivec3(hi, 0, p.z);
    }
    bool tint = (ctl & 0x800) != 0;
    // Level p.x, and p.x + 1 when blended: (c0 * (256 - w) + c1 * w + 128) >> 8
    // (one level: c0 * 256 + 128 >> 8 = c0). A loop, so the level sampler is
    // in the program once.
    int levels = p.y == 0 ? 1 : 2;
    uvec4 sum = uvec4(0u);
    for (int i = 0; i < levels; ++i)
    {
        uvec4 c = mipSampleLevel(p.x + i, su, sv, p.z != 0);
        if (tint)
            c = mipTint(c, p.x + i);
        sum += c * uint(i == 0 ? 256 - p.y : p.y);
    }
    return (sum + 128u) >> 8u;
}
#endif

uvec4 sampleTexture(float s, float t, float q, uint iu, uint iv)
{
    precise float texUf, texVf;
    if (u_fst != 0)
    {
        texUf = float(iu) / 16.0;
        texVf = float(iv) / 16.0;
)GLSL"
R"GLSL(    }
    else
    {
        // 1/q as the CPU's float division (via double) natively; at an
        // internal resolution a float reciprocal (a double division per
        // fragment was a large part of the GPU time at 3x/4x).
#ifdef GS_FAST_RASTER
        float invQ = 1.0 / fabsQ(q);
#else
        float invQ = u_scale == ivec2(1) ? recipExact(fabsQ(q)) : 1.0 / fabsQ(q);
#endif
        texUf = s * invQ * float(u_texSize.x);
        texVf = t * invQ * float(u_texSize.y);
    }
    if (u_texMode >= 1 && u_texMode <= 11 && texScale != ivec2(1))
    {
        // A target at the internal resolution: its samples are the texels.
        ivec2 ts = texScale;
        ivec2 tsub = subAt(g_sub, ts);
        precise float su = texUf * float(ts.x);
        precise float sv = texVf * float(ts.y);
#ifdef GS_BINDLESS
        if (u_linear != 0 && (g_state.v[13].z & 1) != 0)
        {
            // Fast mode (4x): the GPU's bilinear filter on the target's
            // samples (a linear-sampler handle in the palette slot), at the
            // offset where the texture's pages lie; CLAMP / REGION_CLAMP keep
            // the footprint inside by clamping, REPEAT only when inside.
            vec2 sp = vec2(su, sv);
            vec2 lo = vec2(0.5), hi = vec2(u_texSize * ts) - 0.5;
            if (u_wrap.x == 2) { lo.x = float(u_region.x * ts.x) + 0.5; hi.x = float((u_region.y + 1) * ts.x) - 0.5; }
            if (u_wrap.y == 2) { lo.y = float(u_region.z * ts.y) + 0.5; hi.y = float((u_region.w + 1) * ts.y) - 0.5; }
            bool inside = (u_wrap.x != 0 || (sp.x >= lo.x && sp.x <= hi.x)) && (u_wrap.y != 0 || (sp.y >= lo.y && sp.y <= hi.y));
            if (inside)
            {
                sp = clamp(sp, lo, hi) + vec2(g_state.v[13].xy * ts);
                vec4 f = texture(sampler2D(g_state.palette), sp / vec2(textureSize(sampler2D(g_state.palette), 0)));
                uvec4 c = uvec4(round(f * 255.0));
                bool rgbZero = c.r == 0u && c.g == 0u && c.b == 0u;
                if (u_texMode == 2)
                    c.a = (u_texa.y != 0 && rgbZero) ? 0u : uint(u_texa.x);
                else if (u_texMode == 3) // the RGBA5551 alpha bit, filtered: between TA0 and TA1
                    c.a = uint(round(mix(float((u_texa.y != 0 && rgbZero) ? 0 : u_texa.x), float(u_texa.z), clamp(f.a * 255.0 / 128.0, 0.0, 1.0))));
                return c;
            }
        }
#endif
        if (u_linear == 0)
            return fetchSample(g_native.x ? int(texUf) * ts.x + tsub.x : int(floor(su)),
                               g_native.y ? int(texVf) * ts.y + tsub.y : int(floor(sv)));
        precise float sampleSU = su - 0.5;
        precise float sampleSV = sv - 0.5;
        int s0 = int(floor(sampleSU));
        int t0 = int(floor(sampleSV));
        precise float gx = sampleSU - float(s0);
        precise float gy = sampleSV - float(t0);
        return lerpTexels(fetchSample(s0, t0), fetchSample(s0 + 1, t0), fetchSample(s0, t0 + 1), fetchSample(s0 + 1, t0 + 1), gx, gy);
    }
    if (u_linear == 0)
        return fetchTexel(int(texUf), int(texVf), ivec2(0));
    if (u_texMode == 15)
    {
        // Fast mode: the GPU's bilinear filter on decoded colours (not exact:
        // 8-bit weights). CLAMP / REGION_CLAMP keep the 2x2 footprint inside
        // by clamping the coordinate half a texel in; REPEAT is the sampler's.
        vec2 uv = vec2(texUf, texVf);
        if (u_wrap.x == 1) uv.x = clamp(uv.x, 0.5, float(u_texSize.x) - 0.5);
        else if (u_wrap.x == 2) uv.x = clamp(uv.x, float(u_region.x) + 0.5, float(u_region.y) + 0.5);
        if (u_wrap.y == 1) uv.y = clamp(uv.y, 0.5, float(u_texSize.y) - 0.5);
        else if (u_wrap.y == 2) uv.y = clamp(uv.y, float(u_region.z) + 0.5, float(u_region.w) + 0.5);
        // A replaced texture (textures\replace, any size): u_texPages.xy is
        // the size it stands for (unused by this mode otherwise: 0).
        vec2 size = u_texPages.x > 0 ? vec2(u_texPages.xy) : vec2(textureSize(TEX_S, 0));
        return uvec4(round(texture(TEX_S, uv / size) * 255.0));
    }
    precise float sampleU = texUf - 0.5;
    precise float sampleV = texVf - 0.5;
    int u0 = int(floor(sampleU));
    int v0 = int(floor(sampleV));
    precise float fx = sampleU - float(u0);
    precise float fy = sampleV - float(v0);
    if (u_texMode == 13)
    {
        // Raw memory: each axis wrapped once for both texels, the layout once
        // (as four fetchTexel calls, which repeated them; ~35% of the GPU time at 4x).
        RawLayout L = rawLayout();
        int ua = wrapCoord(u0, u_texSize.x, u_wrap.x, u_region.x, u_region.y);
        int ub = wrapCoord(u0 + 1, u_texSize.x, u_wrap.x, u_region.x, u_region.y);
        int va = wrapCoord(v0, u_texSize.y, u_wrap.y, u_region.z, u_region.w);
        int vb = wrapCoord(v0 + 1, u_texSize.y, u_wrap.y, u_region.z, u_region.w);
        return lerpTexels(rawTexel(L, ua, va), rawTexel(L, ub, va), rawTexel(L, ua, vb), rawTexel(L, ub, vb), fx, fy);
    }
    return lerpTexels(fetchTexel(u0, v0, ivec2(0)), fetchTexel(u0 + 1, v0, ivec2(0)), fetchTexel(u0, v0 + 1, ivec2(0)),
                      fetchTexel(u0 + 1, v0 + 1, ivec2(0)), fx, fy);
}

#if defined(GS_MIPMAP) && !defined(GS_SPEC_TEX15)
// The texel with mipmapping: a per-pixel primitive (bit 10) samples its levels;
// another with u_mipCtl set is one resolved to a level, tinted (PS2X_GS_MIP_TINT).
uvec4 sampleTextureMipmapped(float s, float t, float q, uint iu, uint iv)
{
    int ctl = u_mipCtl;
    if ((ctl & 0x400) != 0)
        return sampleMip(s, t, q);
    // (one call of sampleTexture: the compiler inlines it, so a second call
    // would double the largest part of the program)
    uvec4 c = sampleTexture(s, t, q, iu, iv);
    return ctl == 0 ? c : mipTint(c, (ctl >> 12) & 7);
}
#endif

ivec4 combineTexture(ivec4 v, uvec4 tu)
{
    ivec4 t = ivec4(tu);
    bool hasAlpha = u_tcc != 0;
    ivec4 o = ivec4(t.rgb, hasAlpha ? t.a : v.a);
    if (u_tfx == 0)
    {
        o.rgb = clamp((t.rgb * v.rgb) >> 7, 0, 255);
        o.a = hasAlpha ? clampU8((t.a * v.a) >> 7) : v.a;
    }
    else if (u_tfx == 1)
    {
        o = ivec4(t.rgb, hasAlpha ? t.a : v.a);
    }
    else if (u_tfx == 2)
    {
        o.rgb = clamp(((t.rgb * v.rgb) >> 7) + v.a, 0, 255);
        o.a = hasAlpha ? clampU8(t.a + v.a) : v.a;
    }
    else
    {
        o.rgb = clamp(((t.rgb * v.rgb) >> 7) + v.a, 0, 255);
        o.a = hasAlpha ? t.a : v.a;
    }
    return o;
}

bool passesAlphaTest(int a)
{
    if (u_ate == 0) return true;
    if (u_atst == 0) return false;
    if (u_atst == 1) return true;
    if (u_atst == 2) return a < u_aref;
    if (u_atst == 3) return a <= u_aref;
    if (u_atst == 4) return a == u_aref;
    if (u_atst == 5) return a >= u_aref;
    if (u_atst == 6) return a > u_aref;
    return a != u_aref;
}

int pickBlend(int sel, int cs, int cd) { return sel == 0 ? cs : (sel == 1 ? cd : 0); }

// GS dithering (gs_dither.h): a CT16 frame's state with DTHE has DIMX rows 0-1 in v[13].w and rows 2-3 in
// v[14].z, and v[14].w = -1 (a per-pixel mip state's level words there are never negative). DM[y & 3][x & 3]
// of the GS pixel (3 bits, signed) is added to the unclamped colour before the clamp; the CT16 mask then
// drops the low 3 bits (gsdither::offset with the same integer operations).
bool ditherOn() { return u_frameFormat == 2 && g_state.v[14].w < 0; }
int ditherOffset(ivec2 gsPix)
{
    int row = (gsPix.y & 2) != 0 ? g_state.v[14].z : g_state.v[13].w;
    int v = (row >> ((gsPix.y & 1) * 16 + (gsPix.x & 3) * 4)) & 7;
    return v >= 4 ? v - 8 : v;
}

#ifdef GS_LIT
// GT4's car lighting per pixel (GT4_CAR_LIGHTING=pixel; lighting step 2): a lit triangle (the triangle's extra.z, a
// record of litTris + 1) carries its corners' normals (local space) and its light set; GT4's own M07 sum (ambient +
// each light's colour times max(L . n, 0) to its power (M01-M03: lights 3 and 4 to the 4th / 8th / 16th), kept within
// 0..clamp) with the normal interpolated and normalised here.
struct LitTri { vec4 n0; vec4 n1; vec4 n2; };      // n0.w: the light set (int bits)
struct LightSet { vec4 l0; vec4 l1; vec4 l2; vec4 amb; vec4 c0; vec4 c1; vec4 c2; vec4 c3; vec4 clampv; vec4 pw; };
layout(std430, binding = 4) readonly buffer LitTris { LitTri litTris[]; };
layout(std430, binding = 5) readonly buffer LightSets { LightSet lightSets[]; };
// The interpolated normal at the corners' interpolated length (GT4's normals are 1/127 steps: within 5% of unit
// length), so a corner gets its own normal back exactly.
vec3 gt4Normal(vec3 n0, vec3 n1, vec3 n2, float w0, float w1, float w2)
{
    vec3 n = n0 * w0 + n1 * w1 + n2 * w2;
    float len = length(n);
    return len > 1.0e-6 ? n * ((length(n0) * w0 + length(n1) * w1 + length(n2) * w2) / len) : n0;
}
ivec4 gt4Light(int i, float w0, float w1, float w2)
{
    vec3 n = gt4Normal(litTris[i].n0.xyz, litTris[i].n1.xyz, litTris[i].n2.xyz, w0, w1, w2);
    int s = floatBitsToInt(litTris[i].n0.w);
    vec4 d = pow(max(vec4(0.0), lightSets[s].l0 * n.x + lightSets[s].l1 * n.y + lightSets[s].l2 * n.z), lightSets[s].pw);
    vec4 c = lightSets[s].amb + lightSets[s].c0 * d.x + lightSets[s].c1 * d.y + lightSets[s].c2 * d.z + lightSets[s].c3 * d.w;
    return ivec4(min(max(c, vec4(0.0)), lightSets[s].clampv));
}
// GT4's car reflections per pixel (lighting step 2c): a reflection-layer triangle (extra.z = -(record of envTris + 1))
// carries its corners' local positions (w: the corner's Q, for perspective-correct weights), normals (n0.w: its set)
// and input colours; M10's sphere-map UV and colour factor are made here from the interpolated position and normal.
struct EnvTri { vec4 p0; vec4 p1; vec4 p2; vec4 n0; vec4 n1; vec4 n2; vec4 c0; vec4 c1; vec4 c2; };
struct EnvSet { vec4 eye; vec4 r0; vec4 r1; vec4 r2; vec4 f0; vec4 f1; };
layout(std430, binding = 6) readonly buffer EnvTris { EnvTri envTris[]; };
layout(std430, binding = 7) readonly buffer EnvSets { EnvSet envSets[]; };
void gt4Env(int i, float w0, float w1, float w2, out vec2 uv, out ivec4 col)
{
    vec3 pw = vec3(w0 * envTris[i].p0.w, w1 * envTris[i].p1.w, w2 * envTris[i].p2.w);
    pw /= max(pw.x + pw.y + pw.z, 1.0e-30);
    vec3 pos = envTris[i].p0.xyz * pw.x + envTris[i].p1.xyz * pw.y + envTris[i].p2.xyz * pw.z;
    vec3 n = gt4Normal(envTris[i].n0.xyz, envTris[i].n1.xyz, envTris[i].n2.xyz, pw.x, pw.y, pw.z);
    int s = floatBitsToInt(envTris[i].n0.w);
    vec3 V = pos - envSets[s].eye.xyz;
    float d = dot(V, n);
    float inv = inversesqrt(max(dot(V, V), 1.0e-30));
    vec3 R = V - 2.0 * d * n;
    vec3 r = (envSets[s].r0.xyz * R.x + envSets[s].r1.xyz * R.y + envSets[s].r2.xyz * R.z) * inv;
    r.z += 1.0;
    float q = 0.5 * inversesqrt(max(dot(r, r), 1.0e-30));
    uv = vec2(0.5 + r.x * q, 0.5 - r.y * q);
    vec4 cin = envTris[i].c0 * w0 + envTris[i].c1 * w1 + envTris[i].c2 * w2; // (the GS's own: screen-linear)
    col = clamp(ivec4(cin * (envSets[s].f0 + envSets[s].f1 * (min(d, 0.0) * inv))), ivec4(0), ivec4(255));
}
bool litHere()
{
#ifdef GS_LIT_SPLIT
    return gl_FragCoord.x >= GS_LIT_SPLIT * float(u_scale.x);
#else
    return true;
#endif
}
#endif

void main()
{
    ivec2 pix = ivec2(gl_FragCoord.xy); // the GS pixel natively, else a sample of one
    // The primitive's fields read where used (not a copy of the whole record).
#define P prims[v_prim]
#if defined(GS_TRI_GEOMETRY) && !defined(GS_FAST_RASTER)
    // Conservative rasterization reaches past the box (clipped to the scissor).
    if (pix.x < P.bbox.x * u_scale.x || pix.y < P.bbox.y * u_scale.y || pix.x >= (P.bbox.z + 1) * u_scale.x ||
        pix.y >= (P.bbox.w + 1) * u_scale.y)
        discard;
    if (P.extra.x != 0)
    {
        // A sprite's box is two triangles, and conservative rasterization
        // gives both the pixels along their shared diagonal: each pixel goes
        // to the one its centre lies in (on the diagonal: the second).
        int W = (P.bbox.z + 1 - P.bbox.x) * u_scale.x, H = (P.bbox.w + 1 - P.bbox.y) * u_scale.y;
        int side = (2 * (pix.x - P.bbox.x * u_scale.x) + 1) * H + (2 * (pix.y - P.bbox.y * u_scale.y) + 1) * W;
        if ((side < 2 * W * H) != (v_half == 0))
            discard;
    }
#endif
    // DrawTriangle coverage first (before the draw state is read): exact edge
    // functions at the pixel (sample) centre, tie-break by edge ownership.
    // At an internal resolution in float (double maths was ~25% of the GPU
    // time at 4x): triangles sharing an edge have exactly negated
    // coefficients, and with no contraction (precise) their values are exact
    // negatives, so no sample is lost or drawn twice. Depth stays double.
#ifdef GS_FAST_RASTER
    const bool floatEdges = true; // fast mode is never native: no double paths compiled in
#else
    const bool floatEdges = u_scale != ivec2(1);
#endif
    precise double e0 = 0.0lf, e1 = 0.0lf, e2 = 0.0lf;
    precise float f0 = 0.0, f1 = 0.0, f2 = 0.0;
#ifdef GS_HW_TRI
    loadState(u_stateBase + v_state);
#else
    if (P.extra.x == 0 && floatEdges)
    {
        precise float px = (float(pix.x) + 0.5) * kInvScaleF(u_scale.x);
        precise float py = (float(pix.y) + 0.5) * kInvScaleF(u_scale.y);
        vec3 c = vec3(P.edgeA.w, P.edgeB.w, P.uvB.z); // C0 C1 C2 as floats (set with the primitive)
        f0 = P.edgeA.x * px + P.edgeB.x * py + c.x;
        f1 = P.edgeA.y * px + P.edgeB.y * py + c.y;
        f2 = P.edgeA.z * px + P.edgeB.z * py + c.z;
#ifndef GS_FAST_RASTER
        // (Fast mode: the rasterizer decides coverage; no discard, so the
        // GPU's early depth test can skip hidden samples before shading.)
        if (f0 < 0.0 || f1 < 0.0 || f2 < 0.0)
            discard;
        int own = P.extra.y;
        if ((f0 == 0.0 && (own & 1) == 0) || (f1 == 0.0 && (own & 2) == 0) || (f2 == 0.0 && (own & 4) == 0))
            discard;
#endif
    }
    else if (P.extra.x == 0)
    {
        precise double px = (double(pix.x) + 0.5lf) * kInvScale(u_scale.x);
        precise double py = (double(pix.y) + 0.5lf) * kInvScale(u_scale.y);
        e0 = double(P.edgeA.x) * px + double(P.edgeB.x) * py + P.edgeC.x;
        e1 = double(P.edgeA.y) * px + double(P.edgeB.y) * py + P.edgeC.y;
        e2 = double(P.edgeA.z) * px + double(P.edgeB.z) * py + P.edgeC.z;
        if (e0 < 0.0lf || e1 < 0.0lf || e2 < 0.0lf)
            discard;
        int own = P.extra.y;
        if ((e0 == 0.0lf && (own & 1) == 0) || (e1 == 0.0lf && (own & 2) == 0) || (e2 == 0.0lf && (own & 4) == 0))
            discard;
    }
    loadState(u_stateBase + P.extra.w);
#endif
    ivec4 color;
    uint z;
    int fog;
    precise float s = 0.0, t = 0.0, q = 1.0;
    uint iu = 0u, iv = 0u;

    if (PRIM_IS_TRIANGLE)
    {
#ifdef GS_HW_TRI
        z = 0u;
        vec4 stqf = v_stqf;
        if (u_iip != 0)
            color = clamp(ivec4(clamp(shadeSlack(v_rgba), vec4(-1.0), vec4(256.0))), ivec4(0), ivec4(255));
        else
            color = ivec4(v_rgbaFlat);
#ifdef GS_LIT
        if (v_lit > 0 && litHere())
            color = gt4Light(v_lit - 1, v_bary.x, v_bary.y, v_bary.z);
        bool envHere = v_lit < 0 && litHere();
        vec2 envUv;
        if (envHere)
            gt4Env(-v_lit - 1, v_bary.x, v_bary.y, v_bary.z, envUv, color);
#ifdef GS_LIT_SHOW
        if (v_lit != 0)
            color = v_lit > 0 ? ivec4(color.r / 2 + 128, color.g / 2, color.b / 2, color.a) : ivec4(color.r / 2, color.g / 2 + 128, color.b / 2, color.a);
#endif
#endif
        if (u_fst != 0)
        {
            iu = uint(int(v_uv.x)) & 0xFFFFu;
            iv = uint(int(v_uv.y)) & 0xFFFFu;
        }
        else
        {
            s = stqf.x;
            t = stqf.y;
            q = stqf.z;
        }
#ifdef GS_LIT
        if (envHere)
        {
            s = envUv.x;
            t = envUv.y;
            q = 1.0;
        }
#endif
        fog = clampU8(int(clamp(stqf.w, -1.0, 256.0)));
#else
        // DrawTriangle: attributes from the barycentric weights.
        float w0, w1, w2;
        if (floatEdges)
        {
            float inverseArea = P.uvB.w;
            w0 = f0 * inverseArea;
            w1 = f1 * inverseArea;
            w2 = f2 * inverseArea;
        }
        else
        {
            w0 = float(e0 * P.edgeC.w);
            w1 = float(e1 * P.edgeC.w);
            w2 = float(e2 * P.edgeC.w);
        }
        // Z only for draws that test or write it (double maths is slow on
        // consumer GPUs; z is not read otherwise).
        z = 0u;
#ifndef GS_FAST_RASTER // (fast mode: the rasterizer interpolates Z)
        if (u_zWrite != 0)
        {
            precise double zz = P.zv.x * double(w0) + P.zv.y * double(w1) + P.zv.z * double(w2);
            z = uint(zz + 0.5lf);
        }
)GLSL"
R"GLSL(#endif
        precise vec4 stqf = P.stqf0 * w0 + P.stqf1 * w1 + P.stqf2 * w2;
        if (u_iip != 0)
        {
            precise vec4 shade = P.rgba0 * w0 + P.rgba1 * w1 + P.rgba2 * w2;
            color = clamp(ivec4(clamp(shadeSlack(shade), vec4(-1.0), vec4(256.0))), ivec4(0), ivec4(255));
        }
        else
        {
            color = ivec4(P.rgba2);
        }
#ifdef GS_LIT
        if (P.extra.z > 0 && litHere())
            color = gt4Light(P.extra.z - 1, w0, w1, w2);
        bool envHere = P.extra.z < 0 && litHere();
        vec2 envUv;
        if (envHere)
            gt4Env(-P.extra.z - 1, w0, w1, w2, envUv, color);
#ifdef GS_LIT_SHOW
        if (P.extra.z != 0)
            color = P.extra.z > 0 ? ivec4(color.r / 2 + 128, color.g / 2, color.b / 2, color.a) : ivec4(color.r / 2, color.g / 2 + 128, color.b / 2, color.a);
#endif
#endif
        if (u_fst != 0)
        {
            precise float uf = P.uvA.x * w0 + P.uvA.z * w1 + P.uvB.x * w2;
            precise float vf = P.uvA.y * w0 + P.uvA.w * w1 + P.uvB.y * w2;
            iu = uint(int(uf)) & 0xFFFFu;
            iv = uint(int(vf)) & 0xFFFFu;
        }
        else
        {
            s = stqf.x;
            t = stqf.y;
            q = stqf.z;
        }
#ifdef GS_LIT
        if (envHere)
        {
            s = envUv.x;
            t = envUv.y;
            q = 1.0;
        }
#endif
        fog = clampU8(int(clamp(stqf.w, -1.0, 256.0)));
#endif
    }
    else
    {
        // DrawSprite: flat colour and Z of the second vertex; UV stepped from the first.
        color = ivec4(P.rgba0);
        z = uint(P.zv.w);
        fog = P.extra.z;
        if (u_tme != 0)
        {
            // A pixel's texture coordinates at its top-left corner. At a scale
            // a sample's at its centre less half a GS pixel; but along an axis
            // where a nearest-sampled sprite steps a whole number of texels
            // per pixel (copies, reductions, format views such as a frame read
            // as 8-bit indices) the GS pixel's own, so it reads the texel it
            // reads natively (from a target: the sample at this sample's place
            // in it). A fractional step (a slightly zoomed copy: GT3's speed
            // blur) keeps the sample positions, or it steps in whole pixels.
            precise float spx = float(pix.x);
            precise float spy = float(pix.y);
            if (u_scale.x != 1 || u_scale.y != 1)
            {
                g_sub = pix - (pix / u_scale) * u_scale;
                vec2 steps = abs(P.uvA.zw);
                bvec2 whole = lessThan(abs(steps - round(steps)), vec2(0.001));
                g_native = bvec2(u_linear == 0 && steps.x >= 0.999 && whole.x, u_linear == 0 && steps.y >= 0.999 && whole.y);
                spx = g_native.x ? float(pix.x / u_scale.x) : (float(pix.x) + 0.5) * float(kInvScale(u_scale.x)) - 0.5;
                spy = g_native.y ? float(pix.y / u_scale.y) : (float(pix.y) + 0.5) * float(kInvScale(u_scale.y)) - 0.5;
            }
            precise float texUf = P.uvA.x + (spx - P.uvB.x) * P.uvA.z;
            precise float texVf = P.uvA.y + (spy - P.uvB.y) * P.uvA.w;
            if (u_fst != 0)
            {
                iu = uint(clamp(int(floor(texUf * 16.0)), 0, 0xFFFF));
                iv = uint(clamp(int(floor(texVf * 16.0)), 0, 0xFFFF));
            }
            else
            {
                s = texUf / float(u_texSize.x);
                t = texVf / float(u_texSize.y);
                q = 1.0;
            }
        }
    }

#if defined(EXP_HW_NOTEX) && defined(GS_HW_TRI)
    color = combineTexture(color, uvec4(128u, 128u, 128u, 128u)); // (experiment: no texture fetch)
#else
    if (u_tme != 0)
#if defined(GS_MIPMAP) && !defined(GS_SPEC_TEX15)
        color = combineTexture(color, sampleTextureMipmapped(s, t, q, iu, iv));
#else
        color = combineTexture(color, sampleTexture(s, t, q, iu, iv));
#endif
#endif

    if (u_fge != 0)
    {
        int inv = 255 - fog;
        color.rgb = ((fog * color.rgb) >> 8) + ((inv * u_fogColor) >> 8);
    }

    bool pass = passesAlphaTest(color.a);
#if defined(GS_NO_ALPHA_TEST) && defined(GS_HW_TRI)
    // (no state of the batch alpha-tests: every fragment passes)
#else
    if ((u_alphaPass == 0) != pass)
        discard;
#endif

#ifdef GS_INTERLOCK
    // A blending batch that writes no depth, drawn in one draw: the
    // read-modify-write of the target is ordered by primitive (pixel interlock),
    // so overlapping primitives need no levels. Its depth test is here (the
    // depth target is not written meanwhile), as the fixed-function one
    // compares: gl_FragDepth against the D32F value.
    if (u_zTest >= 2)
    {
        float fz = float(min(z, u_zWrite == 2 ? 0xFFFFu : 0xFFFFFFu)) / 16777216.0;
        float sz = texelFetch(u_zbuf, pix, 0).r;
        if (u_zTest == 2 ? !(fz >= sz) : !(fz > sz))
            discard;
    }
    beginInvocationInterlockARB();
    uint rawOrig = imageLoad(u_dstImage, pix).r;
    uint rawDstI = rawOrig;
    if (u_frameFormat == 3)
        rawDstI &= 0x00FFFFFFu; // a Z24 frame reads 24 bits
    // A CT16 frame reads its 16-bit value (a texel may hold a full colour: GT4's race colour, gs_fullcolour.h).
    if (u_frameFormat == 2)
        rawDstI &= 0x80F8F8F8u;
    ivec4 dstI = ivec4(rawDstI & 0xFFu, (rawDstI >> 8) & 0xFFu, (rawDstI >> 16) & 0xFFu, rawDstI >> 24);
    if (u_frameFormat == 1)
        dstI.a = 0x80;
    bool keep = !(u_date != 0 && (((rawDstI >> 31) & 1u) != 0u) != (u_datm != 0));
    int ditherI = ditherOn() ? ditherOffset(pix / u_scale) : 0;
    if (u_abe != 0 && !(u_pabe != 0 && (color.a & 0x80) == 0))
    {
        int cAlpha = (u_blend.z == 0) ? color.a : (u_blend.z == 1 ? dstI.a : u_fix);
        color.r = clampU8(((pickBlend(u_blend.x, color.r, dstI.r) - pickBlend(u_blend.y, color.r, dstI.r)) * cAlpha >> 7) + pickBlend(u_blend.w, color.r, dstI.r) + ditherI);
        color.g = clampU8(((pickBlend(u_blend.x, color.g, dstI.g) - pickBlend(u_blend.y, color.g, dstI.g)) * cAlpha >> 7) + pickBlend(u_blend.w, color.g, dstI.g) + ditherI);
        color.b = clampU8(((pickBlend(u_blend.x, color.b, dstI.b) - pickBlend(u_blend.y, color.b, dstI.b)) * cAlpha >> 7) + pickBlend(u_blend.w, color.b, dstI.b) + ditherI);
    }
    else if (ditherI != 0)
        color.rgb = clamp(color.rgb + ditherI, 0, 255);
    if (u_fba != 0)
        color.a |= 0x80;
    uint pixelI = uint(color.r) | (uint(color.g) << 8) | (uint(color.b) << 16) | (uint(color.a & 0xFF) << 24);
    if (u_fbmsk != 0u)
        pixelI = (pixelI & ~u_fbmsk) | (rawDstI & u_fbmsk);
    if (u_frameFormat == 2)
        pixelI &= 0x80F8F8F8u; // RGBA5551, kept expanded in the target
    // The batch's colour write mask, as glColorMask does for the other draws.
    uint writeBits = ((u_colorMaskBits & 1) != 0 ? 0x000000FFu : 0u) | ((u_colorMaskBits & 2) != 0 ? 0x0000FF00u : 0u) |
                     ((u_colorMaskBits & 4) != 0 ? 0x00FF0000u : 0u) | ((u_colorMaskBits & 8) != 0 ? 0xFF000000u : 0u);
    // (A CT16 pixel the batch writes is 16-bit as a whole: the bytes it keeps too.)
    pixelI = (pixelI & writeBits) | ((u_frameFormat == 2 && writeBits != 0u ? (rawOrig & 0x80F8F8F8u) : rawOrig) & ~writeBits);
    if (keep)
        imageStore(u_dstImage, pix, uvec4(pixelI, 0u, 0u, 0u));
    endInvocationInterlockARB();
}
#else
    ivec4 dst = ivec4(0);
    uint rawDst = 0u;
    if (u_dstRead != 0)
    {
        uvec4 d = uvec4(round(texelFetch(u_dst, pix, 0) * 255.0));
        rawDst = d.r | (d.g << 8) | (d.b << 16) | (d.a << 24);
        if (u_frameFormat == 3)
            rawDst &= 0x00FFFFFFu; // a Z24 frame reads 24 bits
        // A CT16 frame reads its 16-bit value (a texel may hold a full colour: GT4's race colour, gs_fullcolour.h).
        if (u_frameFormat == 2)
            rawDst &= 0x80F8F8F8u;
        dst = ivec4(rawDst & 0xFFu, (rawDst >> 8) & 0xFFu, (rawDst >> 16) & 0xFFu, rawDst >> 24);
        if (u_frameFormat == 1)
            dst.a = 0x80;
        if (u_date != 0 && (((rawDst >> 31) & 1u) != 0u) != (u_datm != 0))
            discard;
    }

    int dither = ditherOn() ? ditherOffset(pix / u_scale) : 0;
    if (u_dstRead != 0 && u_abe != 0 && !(u_pabe != 0 && (color.a & 0x80) == 0))
    {
        int cAlpha = (u_blend.z == 0) ? color.a : (u_blend.z == 1 ? dst.a : u_fix);
        color.r = clampU8(((pickBlend(u_blend.x, color.r, dst.r) - pickBlend(u_blend.y, color.r, dst.r)) * cAlpha >> 7) + pickBlend(u_blend.w, color.r, dst.r) + dither);
        color.g = clampU8(((pickBlend(u_blend.x, color.g, dst.g) - pickBlend(u_blend.y, color.g, dst.g)) * cAlpha >> 7) + pickBlend(u_blend.w, color.g, dst.g) + dither);
        color.b = clampU8(((pickBlend(u_blend.x, color.b, dst.b) - pickBlend(u_blend.y, color.b, dst.b)) * cAlpha >> 7) + pickBlend(u_blend.w, color.b, dst.b) + dither);
    }
    else if (dither != 0)
        color.rgb = clamp(color.rgb + dither, 0, 255);
    // Fast mode: (A-B)*C+D as Cs*ks + Cd*kd on the GPU's blend unit (the
    // equation's signs set per batch); C is As or FIX, 128 = 1.
    float hwKs = 1.0, hwKd = 0.0;
    if (u_hwBlend == 4)
    {
        // Cs * Ad + Cd: the blend unit takes Ad from the target (twice); Cs plain.
        hwKd = 1.0;
    }
    else if (u_hwBlend != 0 && u_abe != 0 && !(u_pabe != 0 && (color.a & 0x80) == 0))
    {
        float c = float(u_blend.z == 0 ? color.a : u_fix) / 128.0;
        float cs = float((u_blend.x == 0 ? 1 : 0) - (u_blend.y == 0 ? 1 : 0));
        float cd = float((u_blend.x == 1 ? 1 : 0) - (u_blend.y == 1 ? 1 : 0));
        hwKs = abs(cs * c + (u_blend.w == 0 ? 1.0 : 0.0));
        hwKd = abs(cd * c + (u_blend.w == 1 ? 1.0 : 0.0));
#ifndef GS_HW_BLEND_ABS
        // Cd x (1 - C) goes below 0 for C above 1 (As above 0x80): the GS
        // subtracts it and clamps, the blend unit cannot. For a black source
        // the exact result is black: 0 (abs kept a third of the picture behind
        // GT3's black loading cover, As 169 - the menus' "double flash",
        // 2026-09-30). Other sources keep abs (a clamp there made some cars'
        // windows darker than before: exact needs the target read).
        if (u_blend.w == 1 && color.r == 0 && color.g == 0 && color.b == 0)
            hwKd = max(cd * c + 1.0, 0.0);
#endif
    }
    if (u_fba != 0)
        color.a |= 0x80;

    uint pixel = uint(color.r) | (uint(color.g) << 8) | (uint(color.b) << 16) | (uint(color.a & 0xFF) << 24);
    if (u_dstRead != 0 && u_fbmsk != 0u)
        pixel = (pixel & ~u_fbmsk) | (rawDst & u_fbmsk);
    if (u_frameFormat == 2)
        pixel &= 0x80F8F8F8u; // RGBA5551, kept expanded in the target
    o_color = vec4(float(pixel & 0xFFu), float((pixel >> 8) & 0xFFu), float((pixel >> 16) & 0xFFu), float(pixel >> 24)) / 255.0;
    o_color1 = vec4(hwKd);
    if (u_hwBlend != 0)
        o_color.rgb *= hwKs;

#ifndef GS_FAST_RASTER
    // u_zWrite: 1 a Z24 buffer, 2 a Z16 buffer (Z clamped to 0xFFFF; Layout::Z16, PS2X_GS_GL_Z16).
    if (u_zWrite != 0)
        gl_FragDepth = float(min(z, u_zWrite == 2 ? 0xFFFFu : 0xFFFFFFu)) / 16777216.0;
#endif
}
#endif
)GLSL";
}
