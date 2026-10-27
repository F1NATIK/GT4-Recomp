#pragma once

// Colour LUTs (GT4_LUT, GT4_LUT_STRENGTH; analysis/design/colour-lut): a 3D colour table the picture pass
// (present_scanline.cpp) applies to the picture's colours after GT4 drew them and before the TV lines. Header-only and
// free of raylib / OpenGL, so the host tests (analysis/design/colour-lut/tests) check the parsing and the tables.
//   GT4_LUT = off (the default) | gt3 (the built-in "GT3 look") | <path to an Adobe .cube file> (a relative path is
//             taken from GT4.exe's folder)
//   GT4_LUT_STRENGTH = 0..100 (default 100): the mix between the picture as GT4 drew it and the table's colour.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::lut
{
    // A 3D table of size^3 RGB entries, red changing fastest, then green, then blue (the .cube order). Input and output
    // are display colours 0..1 (DOMAIN_MIN / DOMAIN_MAX are resolved when a file is read: see parseCube).
    struct Table
    {
        int size = 0;
        std::vector<float> rgb;
        bool ok() const { return size >= 2 && rgb.size() == static_cast<size_t>(size) * size * size * 3; }
        const float *at(int r, int g, int b) const { return &rgb[(static_cast<size_t>(b) * size * size + static_cast<size_t>(g) * size + r) * 3]; }
    };

    // The largest table the picture pass keeps: its texture is size*size texels across (64: 4096); a bigger file is
    // resampled to 64 points per channel.
    inline constexpr int kMaxGpuSize = 64;

    // Trilinear lookup (input clamped to 0..1).
    inline void sample(const Table &t, float r, float g, float b, float out[3])
    {
        const float in[3] = {r, g, b};
        int i0[3], i1[3];
        float f[3];
        for (int c = 0; c < 3; ++c)
        {
            const float p = std::clamp(in[c], 0.0f, 1.0f) * static_cast<float>(t.size - 1);
            i0[c] = std::min(static_cast<int>(std::floor(p)), t.size - 1);
            i1[c] = std::min(i0[c] + 1, t.size - 1);
            f[c] = p - static_cast<float>(i0[c]);
        }
        for (int c = 0; c < 3; ++c)
        {
            auto v = [&](int r_, int g_, int b_) { return t.at(r_, g_, b_)[c]; };
            const float x00 = v(i0[0], i0[1], i0[2]) + (v(i1[0], i0[1], i0[2]) - v(i0[0], i0[1], i0[2])) * f[0];
            const float x10 = v(i0[0], i1[1], i0[2]) + (v(i1[0], i1[1], i0[2]) - v(i0[0], i1[1], i0[2])) * f[0];
            const float x01 = v(i0[0], i0[1], i1[2]) + (v(i1[0], i0[1], i1[2]) - v(i0[0], i0[1], i1[2])) * f[0];
            const float x11 = v(i0[0], i1[1], i1[2]) + (v(i1[0], i1[1], i1[2]) - v(i0[0], i1[1], i1[2])) * f[0];
            const float y0 = x00 + (x10 - x00) * f[1], y1 = x01 + (x11 - x01) * f[1];
            out[c] = y0 + (y1 - y0) * f[2];
        }
    }

    // A table of n points per channel over display colours 0..1 from src, whose inputs span dmin..dmax per channel.
    inline Table resample(const Table &src, const float dmin[3], const float dmax[3], int n)
    {
        Table t;
        t.size = n;
        t.rgb.resize(static_cast<size_t>(n) * n * n * 3);
        float *o = t.rgb.data();
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r, o += 3)
                {
                    const float x[3] = {static_cast<float>(r) / (n - 1), static_cast<float>(g) / (n - 1), static_cast<float>(b) / (n - 1)};
                    float u[3];
                    for (int c = 0; c < 3; ++c)
                        u[c] = (x[c] - dmin[c]) / (dmax[c] - dmin[c]);
                    sample(src, u[0], u[1], u[2], o);
                }
        return t;
    }

    // An Adobe .cube file (LUT_3D_SIZE, DOMAIN_MIN / DOMAIN_MAX, TITLE, '#' comments; Resolve's LUT_3D_INPUT_RANGE as a
    // domain for all three channels). A 1D LUT (LUT_1D_SIZE) is not taken. The result covers display colours 0..1 with at
    // most kMaxGpuSize points per channel (a file with another domain or more points is resampled). false: error says why.
    inline bool parseCube(std::string_view text, Table &out, std::string &error)
    {
        Table raw;
        float dmin[3] = {0.0f, 0.0f, 0.0f}, dmax[3] = {1.0f, 1.0f, 1.0f};
        size_t expected = 0;
        int lineNo = 0;
        size_t pos = 0;
        auto fail = [&](const std::string &why) {
            error = "line " + std::to_string(lineNo) + ": " + why;
            return false;
        };
        while (pos <= text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos)
                end = text.size();
            std::string line(text.substr(pos, end - pos));
            pos = end + 1;
            ++lineNo;
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            size_t s = 0;
            while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s])))
                ++s;
            line.erase(0, s);
            if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3); // a UTF-8 byte order mark
            if (line.empty())
                continue;
            const char *p = line.c_str();
            char *e = nullptr;
            if (std::isalpha(static_cast<unsigned char>(line[0])))
            {
                const size_t sp = line.find_first_of(" \t");
                const std::string key = line.substr(0, sp);
                const char *args = sp == std::string::npos ? "" : p + sp;
                if (key == "TITLE")
                    continue;
                if (key == "LUT_1D_SIZE")
                    return fail("a 1D LUT (LUT_1D_SIZE); only 3D LUTs are taken");
                if (key == "LUT_3D_SIZE")
                {
                    const long n = std::strtol(args, &e, 10);
                    if (e == args || n < 2 || n > 256)
                        return fail("LUT_3D_SIZE must be 2..256");
                    raw.size = static_cast<int>(n);
                    expected = static_cast<size_t>(n) * n * n;
                    raw.rgb.reserve(expected * 3);
                    continue;
                }
                if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX" || key == "LUT_3D_INPUT_RANGE")
                {
                    float v[3];
                    const char *q = args;
                    int got = 0;
                    for (; got < 3; ++got)
                    {
                        v[got] = std::strtof(q, &e);
                        if (e == q)
                            break;
                        q = e;
                    }
                    if (key == "LUT_3D_INPUT_RANGE")
                    {
                        if (got < 2)
                            return fail("LUT_3D_INPUT_RANGE needs two numbers");
                        for (int c = 0; c < 3; ++c)
                            dmin[c] = v[0], dmax[c] = v[1];
                        continue;
                    }
                    if (got != 3)
                        return fail(key + " needs three numbers");
                    for (int c = 0; c < 3; ++c)
                        (key == "DOMAIN_MIN" ? dmin : dmax)[c] = v[c];
                    continue;
                }
                continue; // another tool's keyword (Resolve's LUT_IN_VIDEO_RANGE, ...): ignored
            }
            if (raw.size == 0)
                return fail("colour data before LUT_3D_SIZE");
            float v[3];
            const char *q = p;
            for (int c = 0; c < 3; ++c)
            {
                v[c] = std::strtof(q, &e);
                if (e == q || !std::isfinite(v[c]))
                    return fail("expected three numbers");
                q = e;
            }
            while (*q && std::isspace(static_cast<unsigned char>(*q)))
                ++q;
            if (*q)
                return fail("more than three numbers");
            if (raw.rgb.size() / 3 >= expected)
                return fail("more than LUT_3D_SIZE^3 entries");
            raw.rgb.insert(raw.rgb.end(), v, v + 3);
        }
        if (raw.size == 0)
        {
            lineNo = 0;
            error = "no LUT_3D_SIZE";
            return false;
        }
        if (raw.rgb.size() / 3 != expected)
        {
            error = std::to_string(raw.rgb.size() / 3) + " entries, LUT_3D_SIZE " + std::to_string(raw.size) + " needs " + std::to_string(expected);
            return false;
        }
        for (int c = 0; c < 3; ++c)
            if (!(dmax[c] > dmin[c]))
            {
                error = "DOMAIN_MAX must be above DOMAIN_MIN";
                return false;
            }
        const bool unitDomain = dmin[0] == 0.0f && dmin[1] == 0.0f && dmin[2] == 0.0f && dmax[0] == 1.0f && dmax[1] == 1.0f && dmax[2] == 1.0f;
        out = (unitDomain && raw.size <= kMaxGpuSize) ? std::move(raw) : resample(raw, dmin, dmax, std::min(raw.size, kMaxGpuSize));
        error.clear();
        return true;
    }

    // ---- The built-in "GT3 look" (analysis/design/colour-lut/README.md and scripts/gt3_look_fit.py): per channel
    // out = lift + (1 - lift) * in^gamma (white stays white), then the saturation scaled about Rec. 709 luma. Fitted from GT3
    // race pictures against GT4 race pictures, half of the fit applied and capped (gamma <= 1.15, lift <= 0.04, saturation
    // within 15%).
    struct Gt3Curve
    {
        float lift, gamma;
    };
    inline constexpr Gt3Curve kGt3Curves[3] = {{0.0245f, 1.15f}, {0.0350f, 1.15f}, {0.0400f, 1.15f}};
    inline constexpr float kGt3Saturation = 0.9352f;

    inline void gt3LookColour(const float in[3], float out[3])
    {
        float m[3];
        for (int c = 0; c < 3; ++c)
            m[c] = kGt3Curves[c].lift + (1.0f - kGt3Curves[c].lift) * std::pow(std::clamp(in[c], 0.0f, 1.0f), kGt3Curves[c].gamma);
        const float y = 0.2126f * m[0] + 0.7152f * m[1] + 0.0722f * m[2];
        for (int c = 0; c < 3; ++c)
            out[c] = std::clamp(y + kGt3Saturation * (m[c] - y), 0.0f, 1.0f);
    }

    inline Table gt3Look(int n = 33)
    {
        Table t;
        t.size = n;
        t.rgb.resize(static_cast<size_t>(n) * n * n * 3);
        float *o = t.rgb.data();
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r, o += 3)
                {
                    const float in[3] = {static_cast<float>(r) / (n - 1), static_cast<float>(g) / (n - 1), static_cast<float>(b) / (n - 1)};
                    gt3LookColour(in, o);
                }
        return t;
    }

    // ---- The keys
    enum class Kind
    {
        Off,
        Gt3,
        File
    };
    struct Choice
    {
        Kind kind = Kind::Off;
        std::string path;      // Kind::File: as given
        int strength = 100;    // 0..100
        bool badStrength = false;
    };

    inline Choice parseChoice(const char *lut, const char *strength)
    {
        Choice c;
        std::string v = lut ? lut : "";
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
            v.pop_back();
        size_t s = 0;
        while (s < v.size() && std::isspace(static_cast<unsigned char>(v[s])))
            ++s;
        v.erase(0, s);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        std::string low = v;
        for (char &ch : low)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.empty() || low == "off" || low == "0" || low == "none")
            c.kind = Kind::Off;
        else if (low == "gt3")
            c.kind = Kind::Gt3;
        else
        {
            c.kind = Kind::File;
            c.path = v;
        }
        if (strength && *strength)
        {
            char *e = nullptr;
            const long n = std::strtol(strength, &e, 10);
            while (e && *e && std::isspace(static_cast<unsigned char>(*e)))
                ++e;
            if (e == strength || (e && *e) || n < 0 || n > 100)
                c.badStrength = true;
            else
                c.strength = static_cast<int>(n);
        }
        return c;
    }
}
