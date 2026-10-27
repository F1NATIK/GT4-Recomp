#pragma once

// The scanline pass's settings and placement maths (no graphics API: the
// host test analysis/extras/stock-scanline/tests builds this header alone,
// and analysis/extras/stock-scanline/scanline_ref.py is the same maths in
// Python). present_scanline.cpp draws with it.
//
// The picture is the PS2's display lines (448 for GT4's 480i field mode)
// drawn as CRT beams: each line a gaussian spot whose width grows with its
// brightness, summed in linear light and integrated over each window pixel's
// height (area sampling, so the average light of every window row is the
// same at any scale and nothing aliases into bands). Strength 0 gives flat
// lines (a clean picture), 50% the faithful beam, 100% a narrower beam with
// darker gaps. The flat lines and the picture across are scaled by the
// picture filter (Filter: nearest, bilinear, bicubic, Lanczos), and an
// interlaced picture can have its lines blended (Deinterlace::Blend) before
// either.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2x::scanline
{
    enum class Fit
    {
        Auto,    // whole window rows per line when a filled picture would band (or costs <= 10% height), else fill
        Fill,    // the largest 4:3 picture, any scale
        Integer, // a whole number (>= 2) of window rows per line
    };

    // The beam (gaussian sigma in display lines), dark -> bright by the
    // square root of the linear brightness, per colour channel.
    constexpr float kSigmaDark = 0.24f;
    constexpr float kSigmaBright = 0.42f;
    // Strength where the full faithful beam is reached, and the beam width
    // factor at strength 100%.
    constexpr float kFaithful = 0.5f;
    constexpr float kNarrowAtFull = 0.65f;

    // How the picture is scaled to the window (GT4_PICTURE_FILTER; the developer's
    // four choices, 2026-10-07: Nearest Neighbor, Bilinear Filtering, Bicubic
    // Filtering, Lanczos Resampling): across the display texture's pixels,
    // and down its display lines where no beam is drawn (lines off, or the
    // flat part below strength 50). The numbers are the shader's filterMode.
    //   Nearest:  every PS2 pixel a hard-edged block (point sampling).
    //   Bilinear: linear between pixel centres (the Classic default; Test
    //             Build 3's "Smooth", unchanged).
    //   Box:      the pass's original look (development builds without
    //             GT4_PICTURE_FILTER): linear across, each line a flat band
    //             area-sampled down.
    //   Bicubic:  Catmull-Rom (Mitchell-Netravali B = 0, C = 0.5), 4 taps:
    //             passes through every PS2 pixel's own value (no blur at the
    //             pixel centres), sharper than bilinear between them, a small
    //             halo on hard edges (at most 7.4 % of the step).
    //   Lanczos:  Lanczos-3 (a windowed sinc, 6 taps): the sharpest, a faint
    //             halo on hard edges (at most 11.8 % of the step).
    // Bicubic and Lanczos are separable (the same kernel across and down),
    // their weights normalised to sum 1, the picture's edge pixels and lines
    // repeated beyond it, across in the encoded signal and down in linear
    // light, as bilinear (kernelWeights; the shader's kernelWeights).
    // (1 was "Sharp", no longer offered: GT4_PICTURE_FILTER=sharp reads as
    // Bilinear, as the old name "smooth" does.)
    enum class Filter
    {
        Nearest = 0,
        Bilinear = 2,
        Box = 3,
        Bicubic = 4,
        Lanczos = 5,
    };

    // How the two interlaced fields of a 480i / 1080i picture make one
    // window picture (GT4_DEINTERLACE). The field pairing itself happens in
    // the GS presenter (gs_gl_present.h deinterlaceMode); Blend's vertical
    // filter is drawn by this pass.
    //   Weave:    both fields' lines together (a still picture is whole;
    //             movement combs). Also "off".
    //   Bob:      the newest field alone, the missing lines interpolated.
    //   Blend:    weave, then each line mixed 1/4 : 1/2 : 1/4 with the lines
    //             above and below (PCSX2's Blend; its Automatic choice for
    //             field-mode games such as GT4 up to 1.6): the two fields
    //             blended a little, as a TV's glow and the eye blend them.
    //   Adaptive: per pixel, weave where the picture is still, bob where it
    //             moves (motion from the fields two VSyncs back; PCSX2's
    //             FastMAD, its Automatic choice for GT4 today).
    //   Clamp:    the presenter's earlier adaptive mode (the other field's
    //             line clamped to the range of its neighbours).
    enum class Deinterlace
    {
        Weave,
        Bob,
        Blend,
        Adaptive,
        Clamp,
    };

    struct Settings
    {
        bool on = false;       // lines drawn at start (F1 turns them on and off)
        float strength = 0.5f; // 0..1; 0.5 = faithful (GT3_CRT_SCANLINES / 100)
        float field = 0.0f;    // 0..1 field emphasis (GT4_SCANLINE_FIELD / 100)
        Fit fit = Fit::Auto;   // GT4_PICTURE_FIT
        // GT4_PICTURE_FILTER / GT4_DEINTERLACE (parsePicture): the Classic
        // edition's defaults; a development build keeps Box and Clamp (its
        // look before these keys) unless the keys are set.
        Filter filter = Filter::Bilinear;
        Deinterlace deinterlace = Deinterlace::Blend;
        bool pictureKeys = false; // GT4_PICTURE_FILTER or GT4_DEINTERLACE was given
        // GT4_PICTURE_FILTER held an earlier name ("smooth" or "sharp"), read
        // as bilinear: the runtime logs it once. Null otherwise.
        const char *oldFilterName = nullptr;
    };

    inline bool equalsNoCase(const char *a, const char *b)
    {
        if (!a || !b)
            return false;
        for (; *a && *b; ++a, ++b)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
        return *a == *b;
    }

    // equalsNoCase with spaces and tabs around a ignored (the picture keys' values, as GT4.exe reads its other
    // settings' values).
    inline bool equalsNoCaseTrimmed(const char *a, const char *b)
    {
        if (!a || !b)
            return false;
        while (*a == ' ' || *a == '\t')
            ++a;
        for (; *b; ++a, ++b)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
        while (*a == ' ' || *a == '\t')
            ++a;
        return *a == '\0';
    }

    // A 0..100 percentage, or fallback when unset / not a number.
    inline float percent(const char *v, float fallback)
    {
        if (!v || !*v)
            return fallback;
        char *end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (end == v)
            return fallback;
        return static_cast<float>(std::clamp(n, 0L, 100L)) / 100.0f;
    }

    // Settings from the environment's values (any may be null):
    //   GT3_DISPLAY_FILTER  "crt": lines on at start. The stock build has
    //                       them on unless it names another filter.
    //   GT3_CRT_SCANLINES   strength 0-100 (default 50, faithful)
    //   GT4_SCANLINE_FIELD  field emphasis 0-100 (default 0)
    //   GT4_PICTURE_FIT     auto (default) / fill / integer
    inline Settings parse(const char *filter, const char *scanlines, const char *field, const char *fit, bool stock)
    {
        Settings s;
        const bool crt = equalsNoCase(filter, "crt");
        s.on = crt || (stock && (!filter || !*filter));
        s.strength = percent(scanlines, kFaithful);
        s.field = percent(field, 0.0f);
        if (equalsNoCase(fit, "fill"))
            s.fit = Fit::Fill;
        else if (equalsNoCase(fit, "integer"))
            s.fit = Fit::Integer;
        return s;
    }

    inline const char *fitName(Fit f)
    {
        return f == Fit::Fill ? "fill" : f == Fit::Integer ? "integer" : "auto";
    }

    // GT4_PICTURE_FILTER: nearest | bilinear | bicubic | lanczos; box (the
    // original look, for comparisons). The earlier names smooth and sharp
    // read as bilinear (oldName, when given, is set to the name; null for the
    // others). False when the value is none of them.
    inline bool parseFilter(const char *v, Filter &out, const char **oldName = nullptr)
    {
        if (oldName)
            *oldName = nullptr;
        if (equalsNoCaseTrimmed(v, "nearest"))
            out = Filter::Nearest;
        else if (equalsNoCaseTrimmed(v, "bilinear"))
            out = Filter::Bilinear;
        else if (equalsNoCaseTrimmed(v, "bicubic"))
            out = Filter::Bicubic;
        else if (equalsNoCaseTrimmed(v, "lanczos"))
            out = Filter::Lanczos;
        else if (equalsNoCaseTrimmed(v, "box"))
            out = Filter::Box;
        else if (equalsNoCaseTrimmed(v, "smooth") || equalsNoCaseTrimmed(v, "sharp"))
        {
            out = Filter::Bilinear;
            if (oldName)
                *oldName = equalsNoCaseTrimmed(v, "smooth") ? "smooth" : "sharp";
        }
        else
            return false;
        return true;
    }

    // GT4_DEINTERLACE: weave (also "off") | bob | blend | adaptive | clamp.
    inline bool parseDeinterlace(const char *v, Deinterlace &out)
    {
        if (equalsNoCaseTrimmed(v, "weave") || equalsNoCaseTrimmed(v, "off"))
            out = Deinterlace::Weave;
        else if (equalsNoCaseTrimmed(v, "bob"))
            out = Deinterlace::Bob;
        else if (equalsNoCaseTrimmed(v, "blend"))
            out = Deinterlace::Blend;
        else if (equalsNoCaseTrimmed(v, "adaptive"))
            out = Deinterlace::Adaptive;
        else if (equalsNoCaseTrimmed(v, "clamp"))
            out = Deinterlace::Clamp;
        else
            return false;
        return true;
    }

    inline const char *filterName(Filter f)
    {
        switch (f)
        {
        case Filter::Nearest: return "nearest";
        case Filter::Box: return "box";
        case Filter::Bicubic: return "bicubic";
        case Filter::Lanczos: return "lanczos";
        default: return "bilinear";
        }
    }

    inline const char *deinterlaceName(Deinterlace d)
    {
        switch (d)
        {
        case Deinterlace::Weave: return "weave";
        case Deinterlace::Bob: return "bob";
        case Deinterlace::Adaptive: return "adaptive";
        case Deinterlace::Clamp: return "clamp";
        default: return "blend";
        }
    }

    // The picture keys over s (any value may be null): GT4_PICTURE_FILTER and
    // GT4_DEINTERLACE. stock (the Classic edition): Bilinear and Blend unless
    // set (a PS2 on a TV, as PCSX2 shows it with its deinterlacing and
    // bilinear filtering on); a development build: Box and Clamp, its picture
    // before these keys, unless set. An unknown value counts as not set.
    inline void parsePicture(Settings &s, const char *filter, const char *deinterlace, bool stock)
    {
        s.filter = stock ? Filter::Bilinear : Filter::Box;
        s.deinterlace = stock ? Deinterlace::Blend : Deinterlace::Clamp;
        const bool f = parseFilter(filter, s.filter, &s.oldFilterName);
        const bool d = parseDeinterlace(deinterlace, s.deinterlace);
        s.pictureKeys = f || d;
    }

    // The weight of the next texel / line for a sample frac (0..1) of the way
    // from one centre to the next (the shader's filterWeight): Nearest steps
    // at half way, Bilinear (and Box across: its lines are area-sampled by
    // the shader) is linear. Bicubic and Lanczos use kernelWeights instead.
    inline float filterWeight(Filter f, float frac)
    {
        return f == Filter::Nearest ? (frac < 0.5f ? 0.0f : 1.0f) : frac;
    }

    // Bicubic and Lanczos (the shader's kernelWeights, the same float
    // formulas): the taps sit at offsets kernelFirst .. kernelFirst +
    // kernelTaps - 1 from the texel (line) centre at or before the sample,
    // t (0..1) the sample's distance past that centre. w[0..5]: the weights
    // in tap order, normalised to sum 1 (Bicubic: w[4] = w[5] = 0).
    //   Bicubic (Catmull-Rom, taps -1..2):
    //     w(-1) = ((-0.5 t + 1) t - 0.5) t      w(0) = (1.5 t - 2.5) t^2 + 1
    //     w(1)  = ((-1.5 t + 2) t + 0.5) t      w(2) = (0.5 t - 0.5) t^2
    //   Lanczos-3 (taps -2..3): L(d) = 3 sin(pi d) sin(pi d / 3) / (pi d)^2
    //     at d = t - k, L(0) = 1; sin(pi (t - k)) = (-1)^k sin(pi t), so at a
    //     centre (t = 0) the other weights are exactly 0.
    // At t = 0 both give 1 to the tap at offset 0 and 0 to the others: a PS2
    // pixel's centre keeps its value.
    constexpr float kPi = 3.14159265f;
    inline bool kernelFilter(Filter f) { return f == Filter::Bicubic || f == Filter::Lanczos; }
    inline int kernelTaps(Filter f) { return f == Filter::Lanczos ? 6 : 4; }
    inline int kernelFirst(Filter f) { return f == Filter::Lanczos ? -2 : -1; }
    inline void kernelWeights(Filter f, float t, float w[6])
    {
        if (f != Filter::Lanczos)
        {
            w[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
            w[1] = (1.5f * t - 2.5f) * t * t + 1.0f;
            w[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
            w[3] = (0.5f * t - 0.5f) * t * t;
            w[4] = 0.0f;
            w[5] = 0.0f;
        }
        else
        {
            const float s = std::sin(kPi * t);
            for (int j = 0; j < 6; ++j)
            {
                const float d = t - static_cast<float>(j - 2);
                w[j] = std::fabs(d) < 1e-5f ? 1.0f : 3.0f * ((j & 1) == 0 ? s : -s) * std::sin(kPi * d / 3.0f) / (kPi * kPi * d * d);
            }
        }
        const float sum = w[0] + w[1] + w[2] + w[3] + w[4] + w[5];
        for (int j = 0; j < 6; ++j)
            w[j] /= sum;
    }

    // What the pass draws besides the lines: the picture filter and whether
    // the lines are blended. The default is the pass's look before these
    // settings (Box, no blend).
    struct Look
    {
        Filter filter = Filter::Box;
        bool blend = false;
    };

    // Whether this pass blends the lines (Blend) for the shown picture: only
    // an interlaced field picture (woven by the presenter); a progressive
    // (480p) or whole-frame picture is shown as it is.
    inline bool blendLines(Deinterlace d, bool interlacedPicture) { return d == Deinterlace::Blend && interlacedPicture; }

    struct Placement
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; // window pixels (fractional edges allowed)
        int integer = 0;                              // window rows per line when whole (>= 2), else 0
        float rowsPerLine = 0.0f;                     // h / lines
    };

    // Where the picture goes in a winW x winH window (aspect: picture
    // width / height, 4:3). Whole rows per line: placed so each line's beam
    // centre falls on a window row's centre (y + (k + 1/2) n is x.5), else
    // the scanline pattern sits on the row boundaries and vanishes at 2x.
    inline Placement fitPicture(float winW, float winH, int lines, float aspect, Fit fit)
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

    // Strength factor for the placement. Whole rows per line: 1. A filled
    // picture draws the beam pattern at a phase that walks by frac(rows per
    // line) from line to line; below 3 rows per line that shows as bands of
    // strong and weak lines, below 2 there is no room for a gap: 0 below 2,
    // rising to 1 at 3 (analysis/extras/stock-scanline/moire.py).
    inline float moireGuard(float rowsPerLine, int integer)
    {
        if (integer >= 2)
            return 1.0f;
        return std::clamp(rowsPerLine - 2.0f, 0.0f, 1.0f);
    }

    // Strength 0..1 -> mix from flat lines to the beam, and the beam width
    // factor: 0 flat, 0.5 the faithful beam, 1 the beam at 0.65 x width.
    inline void beamParams(float strength, float &mix, float &widthK)
    {
        const float s = std::clamp(strength, 0.0f, 1.0f);
        mix = std::min(1.0f, s / kFaithful);
        widthK = 1.0f - (1.0f - kNarrowAtFull) * std::max(0.0f, (s - kFaithful) / (1.0f - kFaithful));
    }

    // Display lines in a picture of srcRows texture rows (an internal
    // resolution draws each line as several rows): rows per line =
    // round(srcRows / 448), at least 1 (224-line and 480-line pictures keep
    // their own count).
    inline int rowsPerDisplayLine(int srcRows)
    {
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(srcRows) / 448.0)));
    }

    // The display lines of a picture of srcRows texture rows when the
    // presenter knows them (lines > 0: 448 for 480i and 480p, 960 for GT4's
    // 1080i, whose 960 rows are 960 lines, not 480 doubled), else as
    // rowsPerDisplayLine counts them. Never more lines than rows.
    inline int displayLines(int srcRows, int lines)
    {
        if (srcRows <= 0)
            return 0;
        if (lines > 0 && lines <= srcRows)
            return lines;
        return srcRows / rowsPerDisplayLine(srcRows);
    }

    // Texture rows per display line for those lines (at least 1).
    inline int rowsPerLineOf(int srcRows, int lines)
    {
        const int n = displayLines(srcRows, lines);
        return n > 0 ? std::max(1, srcRows / n) : 1;
    }
}
