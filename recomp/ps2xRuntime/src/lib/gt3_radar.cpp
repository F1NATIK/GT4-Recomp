#include "gt3_radar.h"

#include "gt3_race_info.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    int envInt(const char *name, int fallback, int lo, int hi)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? std::clamp(std::atoi(v), lo, hi) : fallback;
    }
    uint32_t envColour(const char *name, uint32_t fallback)
    {
        const char *v = std::getenv(name);
        if (!v || !*v)
            return fallback;
        if (*v == '#')
            ++v;
        return static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) & 0xFFFFFFu;
    }
    gt3radar::Settings initialSettings()
    {
        gt3radar::Settings s;
        if (const char *v = std::getenv("GT3_RADAR"); v && *v && std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0)
            s.mode = (std::strcmp(v, "always") == 0 || std::strcmp(v, "2") == 0) ? gt3radar::Mode::Always : gt3radar::Mode::Near;
        // (GT3_RADAR_SIDE=left: the earlier setting, before X / Y.)
        if (const char *v = std::getenv("GT3_RADAR_SIDE"); v && std::strcmp(v, "left") == 0)
            s.x = 14;
        s.x = envInt("GT3_RADAR_X", s.x, 0, 100);
        s.y = envInt("GT3_RADAR_Y", s.y, 0, 100);
        s.size = envInt("GT3_RADAR_SIZE", s.size, 8, 60);
        s.range = envInt("GT3_RADAR_RANGE", s.range, 10, 80);
        s.opacity = envInt("GT3_RADAR_OPACITY", s.opacity, 10, 100);
        s.bgOpacity = envInt("GT3_RADAR_BG_OPACITY", s.bgOpacity, 0, 100);
        s.player = envColour("GT3_RADAR_COLOR_PLAYER", s.player);
        s.cars = envColour("GT3_RADAR_COLOR_CARS", s.cars);
        s.near = envColour("GT3_RADAR_COLOR_NEAR", s.near);
        s.background = envColour("GT3_RADAR_COLOR_BG", s.background);
        return s;
    }
    gt3radar::Settings g_settings = initialSettings();
    bool g_editing = false;
    gt3radar::Settings g_beforeEdit;

    constexpr float kCarLength = 4.4f; // metres (GT3's cars are 4-5 m)
    constexpr float kCarWidth = 1.8f;

    Color rgb(uint32_t c, float alpha)
    {
        return Color{static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c),
                     static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
    }

    // Where the radar is: its middle and size in window pixels.
    struct Place
    {
        float cx, cy, size;
    };
    // The area the radar's across position is a percentage of, as the race
    // HUD is placed (GS::squeezeHud): the whole picture up to 2.5:1 (16:9,
    // 21:9), the middle screen on triple screens (bezel gaps included), or
    // the middle 4:3 when the wide HUD is kept in the middle
    // (GT3_HUD_ANCHOR=0).
    void hudArea(float x, float w, float h, float &left, float &width)
    {
        static const bool anchored = [] {
            const char *v = std::getenv("GT3_HUD_ANCHOR");
            return !(v && *v == '0');
        }();
        static const float bezel = [] {
            const char *v = std::getenv("GT3_TRIPLE_BEZEL");
            return v && *v ? std::clamp(static_cast<float>(std::atof(v)), 0.0f, 0.5f) : 0.0f;
        }();
        const float aspect = w / std::max(1.0f, h);
        width = std::min(w, h * 4.0f / 3.0f);
        if (anchored && aspect > 4.0f / 3.0f + 0.01f)
            width = aspect <= 2.5f ? w : w * (bezel > 0.0f ? 1.0f / (3.0f + 2.0f * bezel) : (16.0f / 9.0f) / aspect);
        left = x + (w - width) * 0.5f;
    }

    Place placeIn(float x, float y, float w, float h)
    {
        // Within the HUD's area (see hudArea); the box kept inside it.
        const gt3radar::Settings &s = g_settings;
        float left = x, middleW = w;
        hudArea(x, w, h, left, middleW);
        const float size = h * s.size / 100.0f;
        const float half = size * 0.5f;
        const float cx = std::clamp(left + middleW * s.x / 100.0f, left + half, left + middleW - half);
        const float cy = std::clamp(y + h * s.y / 100.0f, y + half, y + h - half);
        return {cx, cy, size};
    }
    Place g_lastPlace{};
    float g_lastPicture[4] = {};
}

gt3radar::Settings &gt3radar::settings() { return g_settings; }

gt3radar::Mode gt3radar::cycleMode()
{
    Mode &m = g_settings.mode;
    m = m == Mode::Off ? Mode::Near : (m == Mode::Near ? Mode::Always : Mode::Off);
    return m;
}

std::map<std::string, int> gt3radar::liveValues()
{
    const Settings &s = g_settings;
    return {{"radar", static_cast<int>(s.mode)},
            {"radar_x", s.x},
            {"radar_y", s.y},
            {"radar_size", s.size},
            {"radar_range", s.range},
            {"radar_opacity", s.opacity},
            {"radar_bg_opacity", s.bgOpacity},
            {"radar_color_player", static_cast<int>(s.player)},
            {"radar_color_cars", static_cast<int>(s.cars)},
            {"radar_color_near", static_cast<int>(s.near)},
            {"radar_color_bg", static_cast<int>(s.background)}};
}

std::string gt3radar::applyLive(const std::map<std::string, int> &values)
{
    if (g_editing)
        return {}; // the in-game edit wins until it is saved
    Settings s = g_settings;
    auto get = [&](const char *key, int &out, int lo, int hi) {
        if (auto it = values.find(key); it != values.end())
            out = std::clamp(it->second, lo, hi);
    };
    auto colour = [&](const char *key, uint32_t &out) {
        if (auto it = values.find(key); it != values.end())
            out = static_cast<uint32_t>(it->second) & 0xFFFFFFu;
    };
    int mode = static_cast<int>(s.mode);
    get("radar", mode, 0, 2);
    s.mode = static_cast<Mode>(mode);
    get("radar_x", s.x, 0, 100);
    get("radar_y", s.y, 0, 100);
    get("radar_size", s.size, 8, 60);
    get("radar_range", s.range, 10, 80);
    get("radar_opacity", s.opacity, 10, 100);
    get("radar_bg_opacity", s.bgOpacity, 0, 100);
    colour("radar_color_player", s.player);
    colour("radar_color_cars", s.cars);
    colour("radar_color_near", s.near);
    colour("radar_color_bg", s.background);
    const bool modeChanged = s.mode != g_settings.mode;
    const bool otherChanged = std::memcmp(&s, &g_settings, sizeof(s)) != 0 && !modeChanged;
    g_settings = s;
    if (modeChanged)
        return s.mode == Mode::Off ? "Radar off" : (s.mode == Mode::Near ? "Radar: when cars are near" : "Radar: always");
    return otherChanged ? "Radar settings changed" : "";
}

bool gt3radar::editing() { return g_editing; }

void gt3radar::handleKeys(bool &saved, std::string &note)
{
    saved = false;
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (IsKeyPressed(KEY_F8))
    {
        if (shift)
        {
            g_editing = !g_editing;
            if (g_editing)
            {
                g_beforeEdit = g_settings;
                note = "Radar edit: drag to move, mouse wheel or +/- size, [ ] opacity, I J K L nudge; Shift+F8 saves";
            }
            else
            {
                saved = true;
                note = "Radar saved";
            }
        }
        else if (!g_editing)
        {
            const Mode m = cycleMode();
            saved = true;
            note = m == Mode::Off ? "Radar off (F8)" : (m == Mode::Near ? "Radar: when cars are near (F8)" : "Radar: always (F8)");
        }
        return;
    }
    if (!g_editing)
        return;
    Settings &s = g_settings;
    // Drag: the radar follows the mouse (percent of the middle 4:3 / height).
    static bool dragging = false;
    const Vector2 mouse = GetMousePosition();
    const float px = g_lastPicture[0], py = g_lastPicture[1], pw = g_lastPicture[2], ph = g_lastPicture[3];
    if (pw > 0.0f && ph > 0.0f)
    {
        const Place p = g_lastPlace;
        const bool over = std::fabs(mouse.x - p.cx) <= p.size * 0.5f && std::fabs(mouse.y - p.cy) <= p.size * 0.5f;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && over)
            dragging = true;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            dragging = false;
        if (dragging)
        {
            float left = px, middleW = pw;
            hudArea(px, pw, ph, left, middleW);
            s.x = std::clamp(static_cast<int>(std::lround((mouse.x - left) / middleW * 100.0f)), 0, 100);
            s.y = std::clamp(static_cast<int>(std::lround((mouse.y - py) / ph * 100.0f)), 0, 100);
        }
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
        s.size = std::clamp(s.size + (wheel > 0.0f ? 2 : -2), 8, 60);
    auto pressed = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
    if (pressed(KEY_EQUAL) || pressed(KEY_KP_ADD))
        s.size = std::min(60, s.size + 2);
    if (pressed(KEY_MINUS) || pressed(KEY_KP_SUBTRACT))
        s.size = std::max(8, s.size - 2);
    if (pressed(KEY_RIGHT_BRACKET))
        s.opacity = std::min(100, s.opacity + 10);
    if (pressed(KEY_LEFT_BRACKET))
        s.opacity = std::max(10, s.opacity - 10);
    if (pressed(KEY_J))
        s.x = std::max(0, s.x - 1);
    if (pressed(KEY_L))
        s.x = std::min(100, s.x + 1);
    if (pressed(KEY_I))
        s.y = std::max(0, s.y - 1);
    if (pressed(KEY_K))
        s.y = std::min(100, s.y + 1);
}

void gt3radar::draw(const uint8_t *rdram, float x, float y, float w, float h)
{
    g_lastPicture[0] = x;
    g_lastPicture[1] = y;
    g_lastPicture[2] = w;
    g_lastPicture[3] = h;
    const Settings &s = g_settings;
    if ((s.mode == Mode::Off && !g_editing) || w <= 0.0f || h <= 0.0f)
        return;
    static gt3race::Snapshot snap;
    static float alpha = 0.0f;
    const bool race = gt3race::read(rdram, snap, false);
    if (!race && !g_editing)
    {
        alpha = 0.0f;
        return;
    }
    const gt3race::Car *player = nullptr;
    for (const gt3race::Car &c : snap.cars)
        if (race && c.player)
            player = &c;

    struct Blip
    {
        float along, side, heading; // heading: radians clockwise from straight ahead
        bool alongside;
    };
    Blip blips[16];
    int count = 0;
    float nearest = 1e9f;
    const float range = static_cast<float>(s.range);
    if (player)
    {
        // The player's frame: forward (-sin yaw, -cos yaw) in world x / z,
        // right (cos yaw, -sin yaw). GT3_RADAR_MIRROR=1 flips left and right.
        static const bool mirror = [] {
            const char *v = std::getenv("GT3_RADAR_MIRROR");
            return v && *v == '1';
        }();
        const float fx = -std::sin(player->yaw), fz = -std::cos(player->yaw);
        const float rx = std::cos(player->yaw) * (mirror ? -1.0f : 1.0f), rz = -std::sin(player->yaw) * (mirror ? -1.0f : 1.0f);
        for (const gt3race::Car &c : snap.cars)
        {
            if (c.player || count >= 16)
                continue;
            const float dx = c.x - player->x, dz = c.z - player->z;
            const float along = dx * fx + dz * fz, side = dx * rx + dz * rz;
            nearest = std::min(nearest, std::sqrt(along * along + side * side));
            if (std::fabs(along) > range + kCarLength || std::fabs(side) > range + kCarLength)
                continue;
            const float ofx = -std::sin(c.yaw), ofz = -std::cos(c.yaw);
            blips[count++] = {along, side, std::atan2(ofx * rx + ofz * rz, ofx * fx + ofz * fz),
                              std::fabs(along) < kCarLength + 0.5f && std::fabs(side) < 4.0f};
        }
    }
    else
    {
        // Editing outside a race: example cars, to see the colours.
        blips[count++] = {range * 0.45f, -range * 0.12f, 0.1f, false};
        blips[count++] = {1.0f, range * 0.14f, 0.0f, true};
        blips[count++] = {-range * 0.6f, range * 0.05f, -0.2f, false};
        nearest = 0.0f;
    }

    // Near mode fades in while a car is within 80% of the range (and out after).
    const float target = (s.mode == Mode::Always || g_editing || nearest < range * 0.8f) ? 1.0f : 0.0f;
    const float step = std::min(0.1f, GetFrameTime()) * 4.0f;
    alpha = target > alpha ? std::min(target, alpha + step) : std::max(target, alpha - step);
    const Place p = placeIn(x, y, w, h);
    g_lastPlace = p;
    if (alpha <= 0.0f)
        return;
    const float carAlpha = alpha * s.opacity / 100.0f, bgAlpha = alpha * s.bgOpacity / 100.0f;
    const float k = p.size * 0.5f / range; // pixels per metre
    const Rectangle box{p.cx - p.size * 0.5f, p.cy - p.size * 0.5f, p.size, p.size};
    DrawRectangleRounded(box, 0.12f, 6, rgb(s.background, bgAlpha));
    DrawRectangleRoundedLines(box, 0.12f, 6, Fade(WHITE, (g_editing ? 0.9f : 0.35f) * std::max(bgAlpha, carAlpha * 0.5f)));
    const Color guide = Fade(WHITE, 0.18f * std::max(bgAlpha, carAlpha * 0.4f));
    if (10.0f < range)
        DrawCircleLines(static_cast<int>(p.cx), static_cast<int>(p.cy), 10.0f * k, guide); // 10 m
    DrawLineV(Vector2{p.cx, box.y + 4.0f}, Vector2{p.cx, box.y + p.size - 4.0f}, guide);
    DrawLineV(Vector2{box.x + 4.0f, p.cy}, Vector2{box.x + p.size - 4.0f, p.cy}, guide);

    BeginScissorMode(static_cast<int>(box.x), static_cast<int>(box.y), static_cast<int>(p.size), static_cast<int>(p.size));
    const float carW = std::max(3.0f, kCarWidth * k), carL = std::max(6.0f, kCarLength * k);
    for (int i = 0; i < count; ++i)
    {
        const Blip &b = blips[i];
        DrawRectanglePro(Rectangle{p.cx + b.side * k, p.cy - b.along * k, carW, carL}, Vector2{carW * 0.5f, carL * 0.5f}, b.heading * 57.29578f,
                         rgb(b.alongside ? s.near : s.cars, carAlpha));
    }
    // The player: in the middle, pointing up.
    DrawRectanglePro(Rectangle{p.cx, p.cy, carW, carL}, Vector2{carW * 0.5f, carL * 0.5f}, 0.0f, rgb(s.player, std::min(1.0f, carAlpha + 0.1f)));
    EndScissorMode();
    if (g_editing)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "RADAR  x %d%%  y %d%%  size %d%%  opacity %d%%", s.x, s.y, s.size, s.opacity);
        const int fontSize = std::max(12, static_cast<int>(h / 50.0f));
        const float tx = std::clamp(box.x, x, x + w - MeasureText(text, fontSize) - 4.0f);
        const float ty = box.y + p.size + 6.0f + fontSize < y + h ? box.y + p.size + 6.0f : box.y - fontSize - 6.0f;
        DrawRectangle(static_cast<int>(tx) - 4, static_cast<int>(ty) - 2, MeasureText(text, fontSize) + 8, fontSize + 4, Fade(BLACK, 0.7f));
        DrawText(text, static_cast<int>(tx), static_cast<int>(ty), fontSize, YELLOW);
    }
}
