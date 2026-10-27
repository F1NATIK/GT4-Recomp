#include "present_scanline.h"

#include "rlgl.h" // rlGetShaderIdDefault

#include <algorithm>
#include <iostream>
#include <vector>

namespace ps2x::scanline
{
    namespace
    {
        // raylib's default vertex shader feeds fragTexCoord (texture
        // coordinates of the whole texture: the picture is its top-left
        // srcW x srcH texels, so 0..srcW/texW across and 0..srcH/texH down)
        // and fragColor. Mirrored in analysis/extras/stock-scanline/scanline_ref.py.
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
uniform int filterMode;    // 0 nearest, 2 bilinear, 3 box, 4 bicubic, 5 lanczos (Filter)
uniform int blendLines;    // 1: each line mixed 1/4 : 1/2 : 1/4 with its neighbours (Blend)
uniform sampler2D lutTex;  // colour LUT (GT4_LUT): lutSize^3 entries as a strip, x = b * lutSize + r, y = g
uniform float lutSize;     // its points per channel
uniform float lutMix;      // 0 off .. 1 the table's colour (GT4_LUT_STRENGTH)
out vec4 finalColor;

const float kSigmaDark = 0.24;
const float kSigmaBright = 0.42;
const float kPi = 3.14159265;

// The colour LUT on a picture colour (encoded, as GT4 drew it): trilinear, the two blue slices filtered by the texture
// unit across red and green.
vec3 lutColour(vec3 c)
{
    if (lutMix <= 0.0)
        return c;
    c = clamp(c, 0.0, 1.0);
    vec3 p = c * (lutSize - 1.0);
    float b0 = floor(p.b);
    float b1 = min(b0 + 1.0, lutSize - 1.0);
    float across = lutSize * lutSize;
    float y = (p.g + 0.5) / lutSize;
    vec3 v0 = texture(lutTex, vec2((b0 * lutSize + p.r + 0.5) / across, y)).rgb;
    vec3 v1 = texture(lutTex, vec2((b1 * lutSize + p.r + 0.5) / across, y)).rgb;
    return mix(c, mix(v0, v1, p.b - b0), lutMix);
}

// Every picture colour goes through here (lines, rows, the kernels' taps): the LUT first, then linear light for the
// filters and the TV lines.
vec3 toLin(vec3 c) { return pow(clamp(lutColour(c), 0.0, 1.0), vec3(2.2)); }

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
    }

    const char *fragmentSource() { return kFragment; }

    bool Pass::init()
    {
        if (ready())
            return true;
        m_shader = LoadShaderFromMemory(nullptr, kFragment);
        // raylib falls back to its default shader when compiling fails.
        if (m_shader.id == 0 || m_shader.id == rlGetShaderIdDefault())
        {
            m_shader = Shader{};
            std::cerr << "[scanline] the scanline shader did not compile; the picture is drawn without lines" << std::endl;
            return false;
        }
        m_texSize = GetShaderLocation(m_shader, "texSize");
        m_rowsPerLine = GetShaderLocation(m_shader, "rowsPerLine");
        m_lines = GetShaderLocation(m_shader, "lines");
        m_cols = GetShaderLocation(m_shader, "cols");
        m_halfPixel = GetShaderLocation(m_shader, "halfPixel");
        m_mix = GetShaderLocation(m_shader, "mixBeam");
        m_widthK = GetShaderLocation(m_shader, "widthK");
        m_field = GetShaderLocation(m_shader, "field");
        m_parity = GetShaderLocation(m_shader, "parity");
        m_filterMode = GetShaderLocation(m_shader, "filterMode");
        m_blendLines = GetShaderLocation(m_shader, "blendLines");
        m_lutTex = GetShaderLocation(m_shader, "lutTex");
        m_lutSize = GetShaderLocation(m_shader, "lutSize");
        m_lutMix = GetShaderLocation(m_shader, "lutMix");
        return true;
    }

    bool Pass::setLut(const ps2x::lut::Table *table, float strength)
    {
        if (m_lut.id != 0)
            UnloadTexture(m_lut);
        m_lut = Texture2D{};
        m_lutStrip.clear();
        m_lutPoints = 0;
        m_lutStrength = 0.0f;
        if (!table || !table->ok() || strength <= 0.0f || table->size > ps2x::lut::kMaxGpuSize)
            return table == nullptr || strength <= 0.0f;
        const int n = table->size;
        // The strip: n*n texels across (blue slices of n red texels), n rows (green); 32-bit float RGB.
        std::vector<float> strip(static_cast<size_t>(n) * n * n * 3);
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r)
                {
                    const float *v = table->at(r, g, b);
                    float *o = &strip[(static_cast<size_t>(g) * n * n + static_cast<size_t>(b) * n + r) * 3];
                    o[0] = v[0], o[1] = v[1], o[2] = v[2];
                }
        Image image{strip.data(), n * n, n, 1, PIXELFORMAT_UNCOMPRESSED_R32G32B32};
        m_lut = LoadTextureFromImage(image);
        if (m_lut.id == 0)
            return false;
        SetTextureFilter(m_lut, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(m_lut, TEXTURE_WRAP_CLAMP);
        m_lutPoints = n;
        m_lutStrength = std::clamp(strength, 0.0f, 1.0f);
        m_lutStrip = std::move(strip);
        return true;
    }

    void Pass::shutdown()
    {
        if (m_lut.id != 0)
            UnloadTexture(m_lut);
        m_lut = Texture2D{};
        if (ready())
            UnloadShader(m_shader);
        m_shader = Shader{};
    }

    Pass::Uniforms Pass::uniforms(int texWidth, int texHeight, int srcW, int srcH, const Placement &placement, float strength, float field, int parity,
                                  int knownLines, Look look) const
    {
        Uniforms u;
        u.rowsPerLine = rowsPerLineOf(srcH, knownLines);
        u.lines = std::max(1, displayLines(srcH, knownLines));
        beamParams(strength * moireGuard(placement.rowsPerLine, placement.integer), u.mixBeam, u.widthK);
        u.texSize[0] = static_cast<float>(texWidth);
        u.texSize[1] = static_cast<float>(texHeight);
        u.cols = srcW;
        u.halfPixel = 0.5f * static_cast<float>(u.lines) / placement.h;
        u.field = parity == 0 || parity == 1 ? std::clamp(field, 0.0f, 1.0f) : 0.0f;
        u.parity = u.field > 0.0f ? parity : -1;
        // Nearest 0, Bilinear 2, Box 3, Bicubic 4, Lanczos 5 (the shader's filterMode)
        u.filterMode = static_cast<int>(look.filter);
        u.blendLines = look.blend ? 1 : 0;
        u.lutMix = m_lutPoints >= 2 ? m_lutStrength : 0.0f;
        u.lutSize = static_cast<float>(std::max(2, m_lutPoints));
        return u;
    }

    void Pass::draw(const Texture2D &tex, int srcW, int srcH, const Placement &placement, float strength, float field, int parity, int knownLines,
                    Look look)
    {
        if (!ready() || srcW <= 0 || srcH <= 0 || placement.h <= 0.0f)
            return;
        const Uniforms u = uniforms(tex.width, tex.height, srcW, srcH, placement, strength, field, parity, knownLines, look);
        SetShaderValue(m_shader, m_texSize, u.texSize, SHADER_UNIFORM_VEC2);
        SetShaderValue(m_shader, m_rowsPerLine, &u.rowsPerLine, SHADER_UNIFORM_INT);
        SetShaderValue(m_shader, m_lines, &u.lines, SHADER_UNIFORM_INT);
        SetShaderValue(m_shader, m_cols, &u.cols, SHADER_UNIFORM_INT);
        SetShaderValue(m_shader, m_halfPixel, &u.halfPixel, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_mix, &u.mixBeam, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_widthK, &u.widthK, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_field, &u.field, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_parity, &u.parity, SHADER_UNIFORM_INT);
        SetShaderValue(m_shader, m_filterMode, &u.filterMode, SHADER_UNIFORM_INT);
        SetShaderValue(m_shader, m_blendLines, &u.blendLines, SHADER_UNIFORM_INT);
        const float lutMix = m_lut.id != 0 ? u.lutMix : 0.0f;
        SetShaderValue(m_shader, m_lutMix, &lutMix, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_lutSize, &u.lutSize, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(m_shader);
        // (after BeginShaderMode: a shader change draws the batch, which clears raylib's extra texture units)
        if (lutMix > 0.0f)
            SetShaderValueTexture(m_shader, m_lutTex, m_lut);
        DrawTexturePro(tex, Rectangle{0.0f, 0.0f, static_cast<float>(srcW), static_cast<float>(srcH)},
                       Rectangle{placement.x, placement.y, placement.w, placement.h}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
    }

    bool Pass::renderToFile(const Texture2D &tex, int srcW, int srcH, int width, int height, Fit fit, float aspect, float strength, float field,
                            int parity, const char *path, int knownLines, Look look)
    {
        if (!ready() || width <= 0 || height <= 0 || srcH <= 0)
            return false;
        RenderTexture2D target = LoadRenderTexture(width, height);
        if (target.id == 0)
            return false;
        const Placement place = fitPicture(static_cast<float>(width), static_cast<float>(height), displayLines(srcH, knownLines), aspect, fit);
        BeginTextureMode(target);
        ClearBackground(BLACK);
        draw(tex, srcW, srcH, place, strength, field, parity, knownLines, look);
        EndTextureMode();
        Image image = LoadImageFromTexture(target.texture);
        ImageFlipVertical(&image); // a render texture is stored bottom row first
        const bool ok = ExportImage(image, path);
        UnloadImage(image);
        UnloadRenderTexture(target);
        return ok;
    }
}
