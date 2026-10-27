#include "gt4stock_scanline.h"

#include "rlgl.h"

#include <algorithm>
#include <cmath>

namespace gt4stock::scan
{
    Placement fitPicture(float winW, float winH, int lines, float aspect, Fit fit)
    {
        Placement p;
        if (lines <= 0 || winW <= 0.0f || winH <= 0.0f || aspect <= 0.0f)
            return p;
        const float fillH = std::min(winH, winW / aspect);
        const float perLine = fillH / static_cast<float>(lines);
        const int n = static_cast<int>(std::floor(perLine + 1e-4f));
        bool whole = false;
        if (fit == Fit::Integer)
            whole = n >= 2;
        else if (fit == Fit::Auto)
            whole = n >= 2 && (perLine < 3.0f || static_cast<float>(n * lines) >= 0.9f * fillH);
        if (!whole)
        {
            p.h = fillH;
            p.w = fillH * aspect;
            p.x = (winW - p.w) * 0.5f;
            p.y = (winH - p.h) * 0.5f;
            p.rowsPerLine = perLine;
            return p;
        }
        p.integer = n;
        p.h = static_cast<float>(n * lines);
        p.w = p.h * aspect;
        p.y = std::floor((winH - p.h) * 0.5f) + ((n & 1) == 0 ? 0.5f : 0.0f);
        p.x = std::floor((winW - p.w) * 0.5f);
        p.rowsPerLine = static_cast<float>(n);
        return p;
    }

    float moireGuard(float rowsPerLine, int integer)
    {
        if (integer >= 2)
            return 1.0f;
        return std::clamp(rowsPerLine - 2.0f, 0.0f, 1.0f);
    }

    void beamParams(float strength, float &mix, float &widthK)
    {
        const float s = std::clamp(strength, 0.0f, 1.0f);
        mix = std::min(1.0f, s / kFaithful);
        widthK = 1.0f - (1.0f - kNarrowAtFull) * std::max(0.0f, (s - kFaithful) / (1.0f - kFaithful));
    }

    int rowsPerDisplayLine(int srcRows) { return std::max(1, static_cast<int>(std::lround(static_cast<double>(srcRows) / 448.0))); }

    namespace
    {
        // present_scanline.cpp kFragment, unchanged (character for character
        // without the CRs; the launcher's host test compares them).
        const char *const kFragment = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 texSize;      // the display texture in texels
uniform int rowsPerLine;   // texture rows per display line (1 at native)
uniform int lines;         // display lines in the picture (448)
uniform int cols;          // texels across the picture
uniform float halfPixel;   // half a window pixel's height, in display lines
uniform float mixBeam;     // 0 flat lines .. 1 the beam
uniform float widthK;      // beam width factor
uniform float field;       // field emphasis 0..1
uniform int parity;        // the newest field's line parity, -1 unknown
uniform int filterMode;    // 0 nearest, 1 sharp, 2 smooth, 3 box (Filter)
uniform float scaleX;      // window pixels per texel across
uniform int blendLines;    // 1: each line mixed 1/4 : 1/2 : 1/4 with its neighbours (Blend)
out vec4 finalColor;

const float kSigmaDark = 0.24;
const float kSigmaBright = 0.42;

vec3 toLin(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(2.2)); }

// Abramowitz & Stegun 7.1.26 (error <= 1.5e-7).
vec3 erf3(vec3 x)
{
    vec3 a = abs(x);
    vec3 t = 1.0 / (1.0 + 0.3275911 * a);
    vec3 poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t;
    return sign(x) * (1.0 - poly * exp(-a * a));
}
vec3 phi3(vec3 z) { return 0.5 * (1.0 + erf3(z * 0.70710678)); }

// The next centre's weight at frac of the way to it, scale window pixels
// per texel / line (present_scanline_math.h filterWeight).
float filterWeight(float frac, float scale)
{
    if (filterMode == 0)
        return frac < 0.5 ? 0.0 : 1.0;
    if (filterMode == 1)
        return clamp((frac - 0.5) * max(scale, 1.0) + 0.5, 0.0, 1.0);
    return frac;
}

// Display line k at texel column tx: between texel centres across by the
// filter (encoded, like the analog signal), averaged over the line's
// texture rows.
vec3 lineColour(int k, float tx)
{
    float x = tx - 0.5;
    float xi = floor(x);
    float f = filterWeight(x - xi, scaleX);
    int x0 = clamp(int(xi), 0, cols - 1);
    int x1 = clamp(int(xi) + 1, 0, cols - 1);
    vec3 c = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        if (i >= rowsPerLine)
            break;
        int y = k * rowsPerLine + i;
        c += mix(texelFetch(texture0, ivec2(x0, y), 0).rgb, texelFetch(texture0, ivec2(x1, y), 0).rgb, f);
    }
    return c / float(rowsPerLine);
}

void main()
{
    vec2 t = fragTexCoord * texSize;           // texel position
    float Y = t.y / float(rowsPerLine);        // display lines from the top
    float lo = Y - halfPixel, hi = Y + halfPixel;
    int r = int(floor(Y - 0.5));
    // Lines r - 2 .. r + 3 in linear light (the edge lines repeated beyond
    // the picture), then lines r - 1 .. r + 2 as drawn: blended (Blend) and
    // with the field emphasis; sigma from the line before the emphasis.
    vec3 raw[6];
    for (int i = 0; i < 6; ++i)
        raw[i] = (blendLines == 0 && (i == 0 || i == 5)) ? vec3(0.0) : toLin(lineColour(clamp(r - 2 + i, 0, lines - 1), t.x));
    vec3 L[4], S[4];
    for (int i = 0; i < 4; ++i)
    {
        vec3 c = blendLines != 0 ? 0.25 * (raw[i] + 2.0 * raw[i + 1] + raw[i + 2]) : raw[i + 1];
        S[i] = mix(vec3(kSigmaDark), vec3(kSigmaBright), sqrt(c)) * widthK;
        int k = clamp(r - 1 + i, 0, lines - 1);
        if (parity >= 0)
            c *= ((k & 1) == parity) ? 1.0 + field : 1.0 - field;
        L[i] = c;
    }
    vec3 box = vec3(0.0), beam = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        int k = r - 1 + i;
        if (k < 0 || k >= lines)
            continue;
        float top = float(k);
        // box (Box): the line fills [k, k + 1)
        box += L[i] * max(min(hi, top + 1.0) - max(lo, top), 0.0);
        // beam: centred on the line, integrated over this pixel's height
        vec3 centre = vec3(top + 0.5);
        beam += L[i] * (phi3((vec3(hi) - centre) / S[i]) - phi3((vec3(lo) - centre) / S[i]));
    }
    // Flat lines: Box area-sampled (the pass's original sum); the filters
    // at the pixel's centre between the line centres r and r + 1 (L[1], L[2]).
    vec3 lin;
    if (filterMode == 3)
        lin = mix(box, beam, mixBeam) / (2.0 * halfPixel);
    else
        lin = mix(mix(L[1], L[2], filterWeight(Y - 0.5 - float(r), 0.5 / halfPixel)), beam / (2.0 * halfPixel), mixBeam);
    finalColor = vec4(pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0) * colDiffuse * fragColor;
}
)GLSL";
#if defined(GT4L_EDITION_FULL)
        // The Full edition's preview: the 480p update's picture pass (analysis/design/480p-colour/480p-colour.patch,
        // present_scanline.cpp kFragment) byte for byte: nearest 0, bilinear 2, box 3 as GT4.exe draws them today, bicubic 4
        // (Catmull-Rom) and Lanczos-3 5 through its kernelPixel (the lines as drawn, Blend and the field emphasis, before the
        // kernel down). The launcher's host test checks the copy against the patch (FULL-LAUNCHER README). The Classic
        // preview keeps kFragment, today's GT4.exe pass.
        const char *const kFragmentFull = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 texSize;      // the display texture in texels
uniform int rowsPerLine;   // texture rows per display line (1 at native)
uniform int lines;         // display lines in the picture (448)
uniform int cols;          // texels across the picture
uniform float halfPixel;   // half a window pixel's height, in display lines
uniform float mixBeam;     // 0 flat lines .. 1 the beam
uniform float widthK;      // beam width factor
uniform float field;       // field emphasis 0..1
uniform int parity;        // the newest field's line parity, -1 unknown
uniform int filterMode;    // 0 nearest, 2 bilinear, 3 box, 4 bicubic, 5 lanczos (Filter)
uniform int blendLines;    // 1: each line mixed 1/4 : 1/2 : 1/4 with its neighbours (Blend)
out vec4 finalColor;

const float kSigmaDark = 0.24;
const float kSigmaBright = 0.42;
const float kPi = 3.14159265;

vec3 toLin(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(2.2)); }

// Abramowitz & Stegun 7.1.26 (error <= 1.5e-7).
vec3 erf3(vec3 x)
{
    vec3 a = abs(x);
    vec3 t = 1.0 / (1.0 + 0.3275911 * a);
    vec3 poly = ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t;
    return sign(x) * (1.0 - poly * exp(-a * a));
}
vec3 phi3(vec3 z) { return 0.5 * (1.0 + erf3(z * 0.70710678)); }

// The next centre's weight at frac of the way to it (present_scanline_math.h
// filterWeight): nearest steps at half way, bilinear (and box across) linear.
float filterWeight(float frac)
{
    if (filterMode == 0)
        return frac < 0.5 ? 0.0 : 1.0;
    return frac;
}

// Display line k at texel column tx: between texel centres across by the
// filter (encoded, like the analog signal), averaged over the line's
// texture rows.
vec3 lineColour(int k, float tx)
{
    float x = tx - 0.5;
    float xi = floor(x);
    float f = filterWeight(x - xi);
    int x0 = clamp(int(xi), 0, cols - 1);
    int x1 = clamp(int(xi) + 1, 0, cols - 1);
    vec3 c = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        if (i >= rowsPerLine)
            break;
        int y = k * rowsPerLine + i;
        c += mix(texelFetch(texture0, ivec2(x0, y), 0).rgb, texelFetch(texture0, ivec2(x1, y), 0).rgb, f);
    }
    return c / float(rowsPerLine);
}

// Internal resolution (rowsPerLine above 1: GT4 drew each display line as
// that many texture rows): in a whole picture (no Blend, no field emphasis:
// 480p) the flat lines (mixBeam below 1) are filtered down between the rows
// themselves, so the picture keeps the detail of its rows; the beam (the TV
// lines) still lights each display line with its rows' average. At native
// (one row per line) nothing changes.
bool rowDetail() { return rowsPerLine > 1 && blendLines == 0 && parity < 0; }

// Texture row y at texel column tx: between texel centres across by the
// filter (encoded), as lineColour does for one row.
vec3 rowColour(int y, float tx)
{
    float x = tx - 0.5;
    float xi = floor(x);
    float f = filterWeight(x - xi);
    int x0 = clamp(int(xi), 0, cols - 1);
    int x1 = clamp(int(xi) + 1, 0, cols - 1);
    return mix(texelFetch(texture0, ivec2(x0, y), 0).rgb, texelFetch(texture0, ivec2(x1, y), 0).rgb, f);
}

// Bicubic (4: Catmull-Rom, taps at offsets -1..2) and Lanczos-3 (5: taps
// -2..3): the taps' weights for a sample t (0..1) past the centre at offset
// 0, normalised to sum 1 (present_scanline_math.h kernelWeights).
void kernelWeights(float t, out float w[6])
{
    if (filterMode == 4)
    {
        w[0] = ((-0.5 * t + 1.0) * t - 0.5) * t;
        w[1] = (1.5 * t - 2.5) * t * t + 1.0;
        w[2] = ((-1.5 * t + 2.0) * t + 0.5) * t;
        w[3] = (0.5 * t - 0.5) * t * t;
        w[4] = 0.0;
        w[5] = 0.0;
    }
    else
    {
        // L(d) = 3 sin(pi d) sin(pi d / 3) / (pi d)^2 at d = t - k, with
        // sin(pi (t - k)) = (-1)^k sin(pi t): exactly 0 at the other centres.
        float s = sin(kPi * t);
        for (int j = 0; j < 6; ++j)
        {
            float d = t - float(j - 2);
            w[j] = abs(d) < 1e-5 ? 1.0 : 3.0 * ((j & 1) == 0 ? s : -s) * sin(kPi * d / 3.0) / (kPi * kPi * d * d);
        }
    }
    float sum = w[0] + w[1] + w[2] + w[3] + w[4] + w[5];
    for (int j = 0; j < 6; ++j)
        w[j] /= sum;
}

// Display line k across by the kernel (weights w for the texels x0, x0 + 1,
// ..., the picture's edge texels repeated beyond it; encoded, like the
// analog signal), averaged over the line's texture rows.
vec3 lineColourK(int k, int x0, float w[6])
{
    int taps = filterMode == 4 ? 4 : 6;
    vec3 c = vec3(0.0);
    for (int i = 0; i < 8; ++i)
    {
        if (i >= rowsPerLine)
            break;
        int y = k * rowsPerLine + i;
        for (int j = 0; j < 6; ++j)
        {
            if (j >= taps)
                break;
            c += w[j] * texelFetch(texture0, ivec2(clamp(x0 + j, 0, cols - 1), y), 0).rgb;
        }
    }
    return c / float(rowsPerLine);
}

// Texture row y across by the kernel (one row of lineColourK).
vec3 rowColourK(int y, int x0, float w[6])
{
    int taps = filterMode == 4 ? 4 : 6;
    vec3 c = vec3(0.0);
    for (int j = 0; j < 6; ++j)
    {
        if (j >= taps)
            break;
        c += w[j] * texelFetch(texture0, ivec2(clamp(x0 + j, 0, cols - 1), y), 0).rgb;
    }
    return c;
}

// Bicubic and Lanczos, in linear light: the kernel across every line, the
// lines as drawn (Blend, field emphasis) as below, the beam from lines
// r - 1 .. r + 2 as for the other filters, and, where the lines are flat
// (mixBeam < 1), the kernel down between the line centres (lines r - 1 ..
// r + 2, Lanczos r - 2 .. r + 3, the edge lines repeated). Only the lines
// that are used are read.
vec3 kernelPixel(vec2 t, float Y, float lo, float hi, int r)
{
    float x = t.x - 0.5;
    float xi = floor(x);
    float wx[6];
    kernelWeights(x - xi, wx);
    int x0 = int(xi) + (filterMode == 4 ? -1 : -2);
    bool flatPart = mixBeam < 1.0;
    int reach = (flatPart && filterMode == 5 ? 2 : 1) + (blendLines != 0 ? 1 : 0);
    vec3 raw[8]; // lines r - 3 .. r + 4
    for (int i = 0; i < 8; ++i)
    {
        int k = r - 3 + i;
        raw[i] = (k < r - reach || k > r + 1 + reach) ? vec3(0.0) : toLin(lineColourK(clamp(k, 0, lines - 1), x0, wx));
    }
    vec3 L[6], S[6]; // lines r - 2 .. r + 3 as drawn
    for (int i = 0; i < 6; ++i)
    {
        vec3 c = blendLines != 0 ? 0.25 * (raw[i] + 2.0 * raw[i + 1] + raw[i + 2]) : raw[i + 1];
        S[i] = mix(vec3(kSigmaDark), vec3(kSigmaBright), sqrt(c)) * widthK;
        int k = clamp(r - 2 + i, 0, lines - 1);
        if (parity >= 0)
            c *= ((k & 1) == parity) ? 1.0 + field : 1.0 - field;
        L[i] = c;
    }
    vec3 beam = vec3(0.0);
    for (int i = 1; i < 5; ++i)
    {
        int k = r - 2 + i;
        if (k < 0 || k >= lines)
            continue;
        vec3 centre = vec3(float(k) + 0.5);
        beam += L[i] * (phi3((vec3(hi) - centre) / S[i]) - phi3((vec3(lo) - centre) / S[i]));
    }
    vec3 flatLines = vec3(0.0);
    if (flatPart && rowDetail())
    {
        // Down between the rows (internal resolution, a whole picture): rows yi - 1 .. yi + 2 (bicubic),
        // yi - 2 .. yi + 3 (Lanczos), the edge rows repeated.
        float yr = t.y - 0.5;
        float yi = floor(yr);
        float wy[6];
        kernelWeights(yr - yi, wy);
        int rows = lines * rowsPerLine;
        int y0 = int(yi) + (filterMode == 4 ? -1 : -2);
        int taps = filterMode == 4 ? 4 : 6;
        for (int j = 0; j < 6; ++j)
        {
            if (j >= taps)
                break;
            flatLines += wy[j] * toLin(rowColourK(clamp(y0 + j, 0, rows - 1), x0, wx));
        }
    }
    else if (flatPart)
    {
        float wy[6];
        kernelWeights(Y - 0.5 - float(r), wy);
        int first = filterMode == 4 ? 1 : 0; // L index of the first tap: line r - 1 (bicubic), r - 2 (Lanczos)
        for (int j = 0; j < 6; ++j)
            flatLines += wy[j] * L[min(first + j, 5)];
    }
    return mix(flatLines, beam / (2.0 * halfPixel), mixBeam);
}

void main()
{
    vec2 t = fragTexCoord * texSize;           // texel position
    float Y = t.y / float(rowsPerLine);        // display lines from the top
    float lo = Y - halfPixel, hi = Y + halfPixel;
    int r = int(floor(Y - 0.5));
    if (filterMode >= 4)
    {
        finalColor = vec4(pow(clamp(kernelPixel(t, Y, lo, hi, r), 0.0, 1.0), vec3(1.0 / 2.2)), 1.0) * colDiffuse * fragColor;
        return;
    }
    // Lines r - 2 .. r + 3 in linear light (the edge lines repeated beyond
    // the picture), then lines r - 1 .. r + 2 as drawn: blended (Blend) and
    // with the field emphasis; sigma from the line before the emphasis.
    vec3 raw[6];
    for (int i = 0; i < 6; ++i)
        raw[i] = (blendLines == 0 && (i == 0 || i == 5)) ? vec3(0.0) : toLin(lineColour(clamp(r - 2 + i, 0, lines - 1), t.x));
    vec3 L[4], S[4];
    for (int i = 0; i < 4; ++i)
    {
        vec3 c = blendLines != 0 ? 0.25 * (raw[i] + 2.0 * raw[i + 1] + raw[i + 2]) : raw[i + 1];
        S[i] = mix(vec3(kSigmaDark), vec3(kSigmaBright), sqrt(c)) * widthK;
        int k = clamp(r - 1 + i, 0, lines - 1);
        if (parity >= 0)
            c *= ((k & 1) == parity) ? 1.0 + field : 1.0 - field;
        L[i] = c;
    }
    vec3 box = vec3(0.0), beam = vec3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        int k = r - 1 + i;
        if (k < 0 || k >= lines)
            continue;
        float top = float(k);
        // box (Box): the line fills [k, k + 1)
        box += L[i] * max(min(hi, top + 1.0) - max(lo, top), 0.0);
        // beam: centred on the line, integrated over this pixel's height
        vec3 centre = vec3(top + 0.5);
        beam += L[i] * (phi3((vec3(hi) - centre) / S[i]) - phi3((vec3(lo) - centre) / S[i]));
    }
    // Flat lines: Box area-sampled (the pass's original sum); the filters
    // at the pixel's centre between the line centres r and r + 1 (L[1], L[2]).
    vec3 lin;
    if (filterMode == 3)
        lin = mix(box, beam, mixBeam) / (2.0 * halfPixel);
    else if (rowDetail() && mixBeam < 1.0)
    {
        // Internal resolution, a whole picture: the flat lines filtered down between the rows (rowDetail).
        float yr = t.y - 0.5;
        float yi = floor(yr);
        int rows = lines * rowsPerLine;
        vec3 flatRows = mix(toLin(rowColour(clamp(int(yi), 0, rows - 1), t.x)), toLin(rowColour(clamp(int(yi) + 1, 0, rows - 1), t.x)),
                            filterWeight(yr - yi));
        lin = mix(flatRows, beam / (2.0 * halfPixel), mixBeam);
    }
    else
        lin = mix(mix(L[1], L[2], filterWeight(Y - 0.5 - float(r))), beam / (2.0 * halfPixel), mixBeam);
    finalColor = vec4(pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0) * colDiffuse * fragColor;
}
)GLSL";
#endif
    }

    bool Preview::init()
    {
        if (ready())
            return true;
#if defined(GT4L_EDITION_FULL)
        shader_ = LoadShaderFromMemory(nullptr, kFragmentFull);
#else
        shader_ = LoadShaderFromMemory(nullptr, kFragment);
#endif
        if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault())
        {
            shader_ = Shader{};
            return false;
        }
        locTexSize_ = GetShaderLocation(shader_, "texSize");
        locRows_ = GetShaderLocation(shader_, "rowsPerLine");
        locLines_ = GetShaderLocation(shader_, "lines");
        locCols_ = GetShaderLocation(shader_, "cols");
        locHalf_ = GetShaderLocation(shader_, "halfPixel");
        locMix_ = GetShaderLocation(shader_, "mixBeam");
        locWidth_ = GetShaderLocation(shader_, "widthK");
        locField_ = GetShaderLocation(shader_, "field");
        locParity_ = GetShaderLocation(shader_, "parity");
        locFilter_ = GetShaderLocation(shader_, "filterMode");
        locScaleX_ = GetShaderLocation(shader_, "scaleX");
        locBlend_ = GetShaderLocation(shader_, "blendLines");
        return true;
    }

    void Preview::shutdown()
    {
        if (ready())
            UnloadShader(shader_);
        shader_ = Shader{};
    }

    void Preview::draw(const Texture2D &tex, int srcW, int srcH, const Placement &place, float strength, bool linesOn, int filterMode, bool blend)
    {
        if (srcW <= 0 || srcH <= 0 || place.h <= 0.0f)
            return;
        const Rectangle src{0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH)};
        const Rectangle dst{place.x, place.y, place.w, place.h};
        if (!ready() || (!linesOn && filterMode == kFilterBox))
        {
            // The pass's original look with the lines off (the compare_shot.py renders): raylib's bilinear
            // picture in the same place.
            DrawTexturePro(tex, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            return;
        }
        // GT4.exe's Classic edition draws through the pass with the lines off too (strength 0: flat lines,
        // scaled by the picture filter).
        if (!linesOn)
            strength = 0.0f;
        const int rowsPerLine = rowsPerDisplayLine(srcH);
        const int lines = std::max(1, srcH / rowsPerLine);
        float mix = 0.0f, widthK = 1.0f;
        beamParams(strength * moireGuard(place.rowsPerLine, place.integer), mix, widthK);
        const float texSize[2] = {static_cast<float>(tex.width), static_cast<float>(tex.height)};
        const float halfPixel = 0.5f * static_cast<float>(lines) / place.h;
        const float field = 0.0f;
        const int parity = -1;
        SetShaderValue(shader_, locTexSize_, texSize, SHADER_UNIFORM_VEC2);
        SetShaderValue(shader_, locRows_, &rowsPerLine, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, locLines_, &lines, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, locCols_, &srcW, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, locHalf_, &halfPixel, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locMix_, &mix, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locWidth_, &widthK, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locField_, &field, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locParity_, &parity, SHADER_UNIFORM_INT);
        const float scaleX = place.w / static_cast<float>(srcW);
        const int blendLines = blend ? 1 : 0;
        SetShaderValue(shader_, locFilter_, &filterMode, SHADER_UNIFORM_INT);
        SetShaderValue(shader_, locScaleX_, &scaleX, SHADER_UNIFORM_FLOAT);
        SetShaderValue(shader_, locBlend_, &blendLines, SHADER_UNIFORM_INT);
        BeginShaderMode(shader_);
        DrawTexturePro(tex, src, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
    }

    bool Preview::renderWindow(const Texture2D &tex, int srcW, int srcH, int winW, int winH, Fit fit, float strength, bool linesOn,
                               RenderTexture2D &target, int filterMode, bool blend)
    {
        if (winW <= 0 || winH <= 0)
            return false;
        if (target.id == 0 || target.texture.width != winW || target.texture.height != winH)
        {
            if (target.id != 0)
                UnloadRenderTexture(target);
            target = LoadRenderTexture(winW, winH);
            if (target.id == 0)
                return false;
        }
        const Placement place = fitPicture(static_cast<float>(winW), static_cast<float>(winH), srcH / rowsPerDisplayLine(srcH), 4.0f / 3.0f, fit);
        BeginTextureMode(target);
        ClearBackground(BLACK);
        draw(tex, srcW, srcH, place, strength, linesOn, filterMode, blend);
        EndTextureMode();
        return true;
    }

#if defined(GT4L_EDITION_FULL)
    bool Preview::renderWindowAt(const Texture2D &tex, int srcW, int srcH, int winW, int winH, const Placement &place, float strength, bool linesOn,
                                 RenderTexture2D &target, int filterMode, bool blend)
    {
        // As renderWindow, at a placement the caller works out (the Full edition's screen shape: 16:9, 4:3 or square pixels).
        if (winW <= 0 || winH <= 0)
            return false;
        if (target.id == 0 || target.texture.width != winW || target.texture.height != winH)
        {
            if (target.id != 0)
                UnloadRenderTexture(target);
            target = LoadRenderTexture(winW, winH);
            if (target.id == 0)
                return false;
        }
        BeginTextureMode(target);
        ClearBackground(BLACK);
        draw(tex, srcW, srcH, place, strength, linesOn, filterMode, blend);
        EndTextureMode();
        return true;
    }

#endif
    bool Preview::renderWindowToFile(const Texture2D &tex, int srcW, int srcH, int winW, int winH, Fit fit, float strength, const char *path,
                                     int filterMode, bool blend)
    {
        RenderTexture2D target{};
        if (!renderWindow(tex, srcW, srcH, winW, winH, fit, strength, true, target, filterMode, blend))
            return false;
        Image image = LoadImageFromTexture(target.texture);
        ImageFlipVertical(&image);
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        const bool ok = ExportImage(image, path);
        UnloadImage(image);
        UnloadRenderTexture(target);
        return ok;
    }
}
