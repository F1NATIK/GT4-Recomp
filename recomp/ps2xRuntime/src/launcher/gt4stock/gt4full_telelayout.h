#pragma once

// GT4 Recompilation launcher, Full edition: the Telemetry screen's layouts (HUD hand-off 8 T3/T4), telemetry-layout.json
// beside gt4-start.env, which GT4.exe serves at GET /layout and telemetry.html#custom draws:
//   { "version": 1, "bg": "dark" | "black",
//     "monitor": [ {"k":"tower","x":1.2,"y":2.5,"w":31.8,"h":33,"name":"car","cols":["gap","int","best"]}, ... ],
//     "phone_upright": [ ... ], "phone_sideways": [ ... ] }
// x y w h are % of the screen, the list is back to front, an item missing from a list is off. Options per item: style (an
// index into its choices), name and cols (the tower), detail and names (the map), units (speed), range (radar).
// Header-only, no Windows or raylib (the launcher's host test checks the file round trip).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gt4stock::tele
{
    enum Target
    {
        Monitor,
        Upright,
        Sideways,
        kTargets
    };
    inline constexpr const char *kTargetKey[kTargets] = {"monitor", "phone_upright", "phone_sideways"};

    // The items (the page's keys), in the editor's list order, with their choices (style index 0 first).
    struct Kind
    {
        const char *k, *label;
        const char *styles[3]; // nullptr-ended; empty: no choice
        float w, h;            // the size a newly ticked item gets (% of the screen, the monitor's)
    };
    inline constexpr Kind kKinds[] = {
        {"tower", "Timing tower", {nullptr}, 32, 33},
        {"lapbox", "Lap counter", {"Lap", "Lap and race time", nullptr}, 20, 13},
        {"laptimes", "Lap times", {"Your laps", "Best of each car", nullptr}, 32, 29},
        {"map", "Track map", {"Locked", "Rotating", nullptr}, 40, 60},
        {"splits", "Sector splits", {nullptr}, 32, 9},
        {"mirror", "Rear-view mirror", {nullptr}, 42.5f, 29},
        {"speed", "Speed", {nullptr}, 20, 18.5f},
        {"gear", "Gear", {nullptr}, 11, 18.5f},
        {"tacho", "Tacho", {"Dial", "Rev lights", nullptr}, 20.5f, 50},
        {"position", "Position", {nullptr}, 20.5f, 13},
        {"delta", "Delta", {"Bar and number", "Number only", nullptr}, 20.5f, 13},
        {"radar", "Radar", {"Round", "Square", nullptr}, 20, 35},
        {"pedals", "Pedals", {nullptr}, 8, 25},
        {"gmeter", "G-meter", {"Trail", "Dot", nullptr}, 15, 27},
    };
    inline constexpr int kKindCount = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));
    inline int kindIndex(const std::string &k)
    {
        for (int i = 0; i < kKindCount; ++i)
            if (k == kKinds[i].k)
                return i;
        return -1;
    }
    inline int styleCount(int kind)
    {
        int n = 0;
        if (kind >= 0)
            while (n < 3 && kKinds[kind].styles[n])
                ++n;
        return n;
    }

    inline constexpr const char *kCols[4] = {"gap", "int", "best", "last"};
    inline constexpr const char *kNames[3] = {"car", "short", "driver"};

    struct Item
    {
        std::string k;
        float x = 0, y = 0, w = 10, h = 10;
        int style = 0;
        std::string name = "car";   // tower: car | short | driver
        int cols = 1 | 2 | 4;       // tower: gap, int, best, last (bits in kCols' order)
        std::string detail = "detailed"; // map: detailed | simple
        bool names = true;          // map: corner names
        std::string units;          // speed: "" GT4's own, kmh, mph
        int range = 0;              // radar: metres (0: the Radar page's)
    };

    struct Layout
    {
        bool black = false; // bg "black" (OLED) / "dark"
        std::vector<Item> t[kTargets];
    };

    inline Item make(const char *k, float x, float y, float w, float h)
    {
        Item i;
        i.k = k, i.x = x, i.y = y, i.w = w, i.h = h;
        return i;
    }

    // The mock-up's layouts (telemetry.html's DEF_LAYOUT, HUD thread 19 Oct): "Timing and map" on the monitor; the phone
    // upright and sideways.
    inline std::vector<Item> defaults(int target)
    {
        std::vector<Item> v;
        if (target == Monitor)
        {
            Item tw = make("tower", 1.2f, 2.5f, 31.8f, 33);
            tw.cols = 1 | 2 | 4;
            v.push_back(tw);
            v.push_back(make("laptimes", 1.2f, 37, 31.8f, 29));
            v.push_back(make("map", 34.2f, 2.5f, 42.8f, 63.5f));
            v.push_back(make("splits", 1.2f, 68, 31.8f, 9));
            v.push_back(make("speed", 1.2f, 78.5f, 20, 18.5f));
            v.push_back(make("gear", 22.2f, 78.5f, 10.8f, 18.5f));
            v.push_back(make("mirror", 34.2f, 68, 42.8f, 29));
            v.push_back(make("position", 78.2f, 2.5f, 20.6f, 13));
            Item lap = make("lapbox", 78.2f, 17, 20.6f, 13);
            lap.style = 1;
            v.push_back(lap);
            v.push_back(make("delta", 78.2f, 31.5f, 20.6f, 13));
            v.push_back(make("tacho", 78.2f, 46, 20.6f, 51));
        }
        else if (target == Upright)
        {
            v.push_back(make("mirror", 3, 1.5f, 94, 10));
            v.push_back(make("position", 3, 12.5f, 45.5f, 6.5f));
            v.push_back(make("lapbox", 51.5f, 12.5f, 45.5f, 6.5f));
            Item tw = make("tower", 3, 20, 94, 30);
            tw.name = "short";
            tw.cols = 1 | 4;
            v.push_back(tw);
            v.push_back(make("map", 3, 51, 94, 33));
            v.push_back(make("splits", 3, 85, 94, 6.5f));
            v.push_back(make("delta", 3, 92.5f, 94, 6));
        }
        else
        {
            v.push_back(make("mirror", 1.5f, 3, 47, 25));
            Item tw = make("tower", 1.5f, 31, 47, 66);
            tw.cols = 1 | 2;
            v.push_back(tw);
            v.push_back(make("map", 50, 3, 48.5f, 62));
            v.push_back(make("lapbox", 50, 68, 23.5f, 14));
            Item d = make("delta", 75, 68, 23.5f, 14);
            d.style = 1;
            v.push_back(d);
            v.push_back(make("speed", 50, 84.5f, 23.5f, 12.5f));
            v.push_back(make("gear", 75, 84.5f, 23.5f, 12.5f));
        }
        return v;
    }
    inline Layout defaultLayout()
    {
        Layout l;
        for (int t = 0; t < kTargets; ++t)
            l.t[t] = defaults(t);
        return l;
    }

    inline bool has(const std::vector<Item> &v, const char *k)
    {
        for (const Item &i : v)
            if (i.k == k)
                return true;
        return false;
    }
    inline int find(const std::vector<Item> &v, const std::string &k)
    {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].k == k)
                return static_cast<int>(i);
        return -1;
    }

    // ---- telemetry-layout.json
    inline std::string num(float v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f", std::round(v * 100.0f) / 100.0f);
        std::string s = b;
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        return s == "-0" ? "0" : s;
    }
    inline std::string toJson(const Layout &l)
    {
        std::string o = "{ \"version\": 1, \"bg\": \"";
        o += l.black ? "black" : "dark";
        o += "\"";
        for (int t = 0; t < kTargets; ++t)
        {
            o += ",\n  \"";
            o += kTargetKey[t];
            o += "\": [";
            bool first = true;
            for (const Item &i : l.t[t])
            {
                if (kindIndex(i.k) < 0)
                    continue;
                o += first ? "\n    " : ",\n    ";
                first = false;
                o += "{\"k\":\"" + i.k + "\",\"x\":" + num(i.x) + ",\"y\":" + num(i.y) + ",\"w\":" + num(i.w) + ",\"h\":" + num(i.h);
                if (styleCount(kindIndex(i.k)) > 0)
                    o += ",\"style\":" + std::to_string(i.style);
                if (i.k == "tower")
                {
                    o += ",\"name\":\"" + i.name + "\",\"cols\":[";
                    bool f = true;
                    for (int c = 0; c < 4; ++c)
                        if ((i.cols >> c) & 1)
                        {
                            o += f ? "\"" : ",\"";
                            o += kCols[c];
                            o += "\"";
                            f = false;
                        }
                    o += "]";
                }
                if (i.k == "map")
                    o += ",\"detail\":\"" + i.detail + "\",\"names\":" + (i.names ? "true" : "false");
                if (i.k == "speed" && !i.units.empty())
                    o += ",\"units\":\"" + i.units + "\"";
                if (i.k == "radar" && i.range > 0)
                    o += ",\"range\":" + std::to_string(i.range);
                o += "}";
            }
            o += first ? "]" : "\n  ]";
        }
        o += " }\n";
        return o;
    }

    // A small JSON reader for this file (objects, arrays, strings, numbers, true / false / null).
    namespace json
    {
        struct Value
        {
            enum Type
            {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object
            } type = Null;
            bool b = false;
            double n = 0;
            std::string s;
            std::vector<Value> a;
            std::vector<std::pair<std::string, Value>> o;
            const Value *get(const char *key) const
            {
                for (const auto &kv : o)
                    if (kv.first == key)
                        return &kv.second;
                return nullptr;
            }
        };
        struct Reader
        {
            const char *p, *e;
            bool ok = true;
            void ws()
            {
                while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                    ++p;
            }
            bool lit(const char *w)
            {
                const size_t n = std::strlen(w);
                if (static_cast<size_t>(e - p) >= n && std::strncmp(p, w, n) == 0)
                {
                    p += n;
                    return true;
                }
                return false;
            }
            std::string str()
            {
                std::string s;
                ++p; // the opening quote
                while (p < e && *p != '"')
                {
                    if (*p == '\\' && p + 1 < e)
                    {
                        ++p;
                        const char c = *p;
                        s += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'u' ? '?' : c;
                        if (c == 'u')
                            p += std::min<ptrdiff_t>(4, e - p - 1);
                    }
                    else
                        s += *p;
                    ++p;
                }
                if (p < e)
                    ++p;
                else
                    ok = false;
                return s;
            }
            Value value(int depth = 0)
            {
                Value v;
                ws();
                if (p >= e || depth > 16)
                {
                    ok = false;
                    return v;
                }
                if (*p == '{')
                {
                    v.type = Value::Object;
                    ++p;
                    ws();
                    if (p < e && *p == '}')
                    {
                        ++p;
                        return v;
                    }
                    while (ok)
                    {
                        ws();
                        if (p >= e || *p != '"')
                        {
                            ok = false;
                            break;
                        }
                        std::string k = str();
                        ws();
                        if (p >= e || *p != ':')
                        {
                            ok = false;
                            break;
                        }
                        ++p;
                        v.o.emplace_back(k, value(depth + 1));
                        ws();
                        if (p < e && *p == ',')
                        {
                            ++p;
                            continue;
                        }
                        if (p < e && *p == '}')
                        {
                            ++p;
                            break;
                        }
                        ok = false;
                    }
                }
                else if (*p == '[')
                {
                    v.type = Value::Array;
                    ++p;
                    ws();
                    if (p < e && *p == ']')
                    {
                        ++p;
                        return v;
                    }
                    while (ok)
                    {
                        v.a.push_back(value(depth + 1));
                        ws();
                        if (p < e && *p == ',')
                        {
                            ++p;
                            continue;
                        }
                        if (p < e && *p == ']')
                        {
                            ++p;
                            break;
                        }
                        ok = false;
                    }
                }
                else if (*p == '"')
                {
                    v.type = Value::String;
                    v.s = str();
                }
                else if (lit("true"))
                    v.type = Value::Bool, v.b = true;
                else if (lit("false"))
                    v.type = Value::Bool, v.b = false;
                else if (lit("null"))
                    v.type = Value::Null;
                else
                {
                    char *end = nullptr;
                    const std::string t(p, static_cast<size_t>(std::min<ptrdiff_t>(e - p, 40)));
                    v.n = std::strtod(t.c_str(), &end);
                    if (end == t.c_str())
                        ok = false;
                    else
                        v.type = Value::Number, p += end - t.c_str();
                }
                return v;
            }
        };
    } // namespace json

    // The layout from the file's text; false (l untouched) when it is not a version 1 layout.
    inline bool fromJson(const std::string &text, Layout &l)
    {
        json::Reader r{text.data(), text.data() + text.size()};
        const json::Value root = r.value();
        if (!r.ok || root.type != json::Value::Object)
            return false;
        const json::Value *ver = root.get("version");
        if (!ver || ver->type != json::Value::Number || ver->n != 1)
            return false;
        Layout out;
        if (const json::Value *bg = root.get("bg"))
            out.black = bg->s == "black";
        for (int t = 0; t < kTargets; ++t)
        {
            const json::Value *list = root.get(kTargetKey[t]);
            if (!list || list->type != json::Value::Array)
            {
                out.t[t] = defaults(t); // a target the file does not have: the defaults
                continue;
            }
            for (const json::Value &v : list->a)
            {
                const json::Value *k = v.get("k");
                if (!k || kindIndex(k->s) < 0 || find(out.t[t], k->s) >= 0)
                    continue;
                Item i;
                i.k = k->s;
                auto f = [&](const char *key, float &dst, float lo, float hi) {
                    if (const json::Value *x = v.get(key); x && x->type == json::Value::Number)
                        dst = std::clamp(static_cast<float>(x->n), lo, hi);
                };
                f("x", i.x, 0, 99);
                f("y", i.y, 0, 99);
                f("w", i.w, 1, 100);
                f("h", i.h, 1, 100);
                i.w = std::min(i.w, 100 - i.x);
                i.h = std::min(i.h, 100 - i.y);
                if (const json::Value *s = v.get("style"); s && s->type == json::Value::Number)
                    i.style = std::clamp(static_cast<int>(s->n), 0, std::max(0, styleCount(kindIndex(i.k)) - 1));
                if (const json::Value *s = v.get("name"); s && (s->s == "car" || s->s == "short" || s->s == "driver"))
                    i.name = s->s;
                if (const json::Value *c = v.get("cols"); c && c->type == json::Value::Array)
                {
                    i.cols = 0;
                    for (const json::Value &x : c->a)
                        for (int n = 0; n < 4; ++n)
                            if (x.s == kCols[n])
                                i.cols |= 1 << n;
                }
                if (const json::Value *s = v.get("detail"); s && (s->s == "detailed" || s->s == "simple"))
                    i.detail = s->s;
                if (const json::Value *s = v.get("names"); s && s->type == json::Value::Bool)
                    i.names = s->b;
                if (const json::Value *s = v.get("units"); s && (s->s == "kmh" || s->s == "mph"))
                    i.units = s->s;
                if (const json::Value *s = v.get("range"); s && s->type == json::Value::Number)
                    i.range = std::clamp(static_cast<int>(s->n), 0, 100);
                out.t[t].push_back(i);
            }
        }
        l = out;
        return true;
    }
} // namespace gt4stock::tele
