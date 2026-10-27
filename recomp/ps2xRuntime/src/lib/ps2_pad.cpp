#include "runtime/ps2_pad.h"
#include "runtime/ps2_pad_rumble.h"
#include "ps2_host_backend.h"
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#if defined(PS2X_INPUT_GLFW)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif
#if defined(PS2X_GAME_GT4)
#include "Kernel/Stubs/Gt4Pad2.h" // player 2 (controller port 2)
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "gt4_hud.h" // the Race HUD zoom button, kept from GT4's own controls (HUD hand-off 9)
#endif

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    std::atomic<bool> g_vibration{[] {
        const char *v = std::getenv("GT3_VIBRATION");
        return !(v && *v == '0');
    }()};

    // Player 1's vibration pad when the profile names none ("rumble xinput auto").
    int automaticRumblePad()
    {
#if defined(PS2X_GAME_GT4)
        return gt4pad2::player1XInputPad(); // never player 2's pad (Gt4Pad2.h)
#else
        return ps2_pad::firstXInputPad();
#endif
    }

    // The keyboard as player 1 (no controller): PS2 buttons, active low, from the keys down (down(raylib key)); the key
    // `claimed` (GT4: the Race HUD zoom key, gt4_hud.h; 0 none) never counts.
    template <typename Down>
    uint16_t keyboardButtons(Down down, int claimed)
    {
        uint16_t btns = 0xFFFFu;
        auto key = [&](int k) { return k != claimed && down(k); };
        if (key(KEY_UP) || key(KEY_W))
            btns &= ~PAD_UP;
        if (key(KEY_DOWN) || key(KEY_S))
            btns &= ~PAD_DOWN;
        if (key(KEY_LEFT) || key(KEY_A))
            btns &= ~PAD_LEFT;
        if (key(KEY_RIGHT) || key(KEY_D))
            btns &= ~PAD_RIGHT;
        if (key(KEY_X) || key(KEY_SPACE))
            btns &= ~PAD_CROSS;
#if defined(PS2X_GAME_GT3)
        if (key(KEY_C) || key(KEY_ESCAPE))
#else
        // GT4: Esc leaves full screen (ps2_runtime.cpp), so it is not Circle too.
        if (key(KEY_C))
#endif
            btns &= ~PAD_CIRCLE;
        if (key(KEY_Z) || key(KEY_KP_0))
            btns &= ~PAD_SQUARE;
        if (key(KEY_V) || key(KEY_KP_1))
            btns &= ~PAD_TRIANGLE;
        if (key(KEY_Q))
            btns &= ~PAD_L1;
        if (key(KEY_E))
            btns &= ~PAD_R1;
        if (key(KEY_LEFT_SHIFT))
            btns &= ~PAD_L2;
        if (key(KEY_RIGHT_SHIFT))
            btns &= ~PAD_R2;
        if (key(KEY_ENTER))
            btns &= ~PAD_START;
        if (key(KEY_TAB))
            btns &= ~PAD_SELECT;
        return btns;
    }

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // HUD hand-off 9: the Race HUD zoom's controller button (gt4_hud.h zoomPadButton) taken out of player 1's mapped
    // controllers before GT4's mapping reads them, so it never reaches GT4's own controls; whether it is down.
    bool claimZoomButton(ps2_pad::Devices &devices, const ps2_pad::InputProfile &profile)
    {
        const int zb = gt4hud::zoomPadButton();
        if (zb < 0 || zb >= 32)
            return false;
        bool down = false;
        for (int i = 0; i < 4; ++i)
            if (!profile.sources[i].raw && devices[i].connected)
            {
                down = down || devices[i].buttons[static_cast<size_t>(zb)];
                devices[i].buttons[static_cast<size_t>(zb)] = false;
            }
        return down;
    }
#endif
}

void ps2_pad::setVibrationEnabled(bool on) { g_vibration.store(on, std::memory_order_relaxed); }
bool ps2_pad::vibrationEnabled() { return g_vibration.load(std::memory_order_relaxed); }

PSPadBackend::PSPadBackend()
{
    m_snapshot[0] = 1;
    m_snapshot[1] = kPadAnalogMarker;
    m_snapshot[2] = m_snapshot[3] = 0xff;
    m_snapshot[4] = m_snapshot[5] = m_snapshot[6] = m_snapshot[7] = kPadStickCenter;
}

PSPadBackend::~PSPadBackend()
{
    if (m_appliedActuators && m_rumblePad >= 0) ps2_pad::setXInputRumble(m_rumblePad, 0, 0);
}

void PSPadBackend::setActuators(bool smallMotor, uint8_t large)
{
    m_actuators.store((smallMotor ? 0x100u : 0u) | large, std::memory_order_relaxed);
}

void PSPadBackend::applyRumble()
{
    // DualShock 2: small motor on/off, large motor 0..255. XInput: left is the
    // low-frequency (large) motor, right the high-frequency (small) one.
    const uint32_t wanted = m_profile.rumble && ps2_pad::vibrationEnabled() ? m_actuators.load(std::memory_order_relaxed) : 0u;
#if defined(PS2X_GAME_GT4)
    // Player 2 (Gt4Pad2.h) holds this pad now: player 1's automatic choice leaves it without
    // writing to it (player 2's poll ran first this frame and set the pad's motors itself).
    if (m_profile.rumbleXInput < 0 && m_rumblePad >= 0 && m_rumblePad == gt4pad2::claimedXInput())
    {
        m_rumblePad = -1;
        m_appliedActuators = 0;
        m_rumbleRetry = 0;
    }
#endif
    if (wanted == m_appliedActuators) return;
    // Looking for a controller is slow when none is connected: once a second.
    const double now = GetTime();
    if (m_rumblePad < 0 && now < m_rumbleRetry) return;
    m_rumbleRetry = now + 1.0;
    if (m_rumblePad < 0 || m_profile.rumbleXInput >= 0)
        m_rumblePad = m_profile.rumbleXInput >= 0 ? m_profile.rumbleXInput : automaticRumblePad();
    const auto low = static_cast<uint16_t>(std::lround((wanted & 0xff) / 255.f * m_profile.rumbleLarge * 65535.f));
    const auto high = static_cast<uint16_t>((wanted & 0x100) ? std::lround(m_profile.rumbleSmall * 65535.f) : 0);
    if (m_rumblePad >= 0 && ps2_pad::setXInputRumble(m_rumblePad, low, high)) m_appliedActuators = wanted;
    else m_rumblePad = -1; // Retried on the next poll, e.g. after a controller connects.
}

void PSPadBackend::stopRumble()
{
    if (m_appliedActuators && m_rumblePad >= 0) ps2_pad::setXInputRumble(m_rumblePad, 0, 0);
    m_appliedActuators = 0;
#if defined(PS2X_GAME_GT4)
    gt4pad2::stopRumble(); // player 2's controller too
#endif
}

bool PSPadBackend::loadProfile(const char *path, std::string &error)
{
    if (!path) { error = "Missing input profile path"; return false; }
    std::ifstream file(path);
    if (!file) { error = "Cannot open input profile: " + std::string(path); return false; }
    return ps2_pad::readProfile(file, m_profile, error);
}

bool PSPadBackend::loadProfileText(const std::string &rows, std::string &error)
{
    std::string text = rows;
    for (char &c : text)
        if (c == ';')
            c = '\n';
    std::istringstream stream(text);
    return ps2_pad::readProfile(stream, m_profile, error);
}

void PSPadBackend::publishDevices(const ps2_pad::Devices &devices)
{
    auto packet = ps2_pad::mapDevices(m_profile, devices);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = packet;
}

bool PSPadBackend::readState(int port, int slot, uint8_t *data, size_t size)
{
    if (!data || size < 32 || port != 0 || slot != 0)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memcpy(data, m_snapshot.data(), m_snapshot.size());
    return true;
}

void PSPadBackend::poll()
{
    uint8_t data[32]{};

#if !defined(PS2X_GAME_GT4)
    applyRumble(); // GT4: below, after player 2's poll
#endif
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    const bool windowReady = IsWindowReady();
    ps2_pad::Devices devices{};
    if (windowReady)
    {
#if defined(PS2X_INPUT_GLFW)
        ps2_pad::DeviceInventory inventory{};
        for (int joystick = GLFW_JOYSTICK_1; joystick <= GLFW_JOYSTICK_LAST; ++joystick)
        {
            std::string description;
            if (glfwJoystickPresent(joystick))
            {
                const char *name = glfwGetJoystickName(joystick);
                const char *guid = glfwGetJoystickGUID(joystick);
                inventory[joystick] = {true, glfwJoystickIsGamepad(joystick) == GLFW_TRUE, guid ? guid : ""};
                int axes = 0, buttons = 0;
                glfwGetJoystickAxes(joystick, &axes);
                glfwGetJoystickButtons(joystick, &buttons);
                description = std::string(name ? name : "unknown") + " guid=" + (guid ? guid : "unknown") +
                    " axes=" + std::to_string(axes) + " buttons=" + std::to_string(buttons) +
                    " mapped=" + (glfwJoystickIsGamepad(joystick) ? "yes" : "no");
            }
            if (description != m_deviceInventory[joystick])
            {
                std::fprintf(stderr, "[input] joystick %d %s\n", joystick,
                    description.empty() ? "disconnected" : description.c_str());
                m_deviceInventory[joystick] = description;
            }
        }
#if defined(PS2X_GAME_GT4)
        gt4pad2::poll(inventory, m_profile); // player 2 (port 2): its controller, mapping and motors
#endif
#endif
        for (int i = 0; i < 4; ++i)
        {
            auto &device = devices[i];
            const auto source = m_profile.sources[i];
#if defined(PS2X_INPUT_GLFW)
            // GT4's player 2 reads its devices the same way (Kernel/Stubs/Gt4Pad2.cpp poll): a fix here belongs there too.
            const int joystick = ps2_pad::resolveSource(source, inventory);
            if (joystick < 0) continue;
            if (source.raw)
            {
                device.connected = glfwJoystickPresent(joystick) == GLFW_TRUE;
                if (!device.connected) continue;
                int axisCount = 0, buttonCount = 0;
                const float *axes = glfwGetJoystickAxes(joystick, &axisCount);
                const unsigned char *buttons = glfwGetJoystickButtons(joystick, &buttonCount);
                device.axisCount = axes ? std::clamp(axisCount, 0, 8) : 0;
                for (int axis = 0; axis < device.axisCount; ++axis) device.axes[axis] = axes[axis];
                if (buttons) for (int button = 0; button < std::clamp(buttonCount, 0, 128); ++button)
                    device.buttons[button] = buttons[button] == GLFW_PRESS;
                continue;
            }
#else
            if (source.raw || !source.guid.empty()) continue; // Unsupported selectors retain disconnected state.
            const int joystick = source.joystick;
#endif
            device.connected = IsGamepadAvailable(joystick);
#if defined(PS2X_INPUT_GLFW)
            // raylib's ready flag also includes unmapped joysticks. Reject them
            // here: its failed GLFW mapping can otherwise look half-pressed.
            device.connected = device.connected && glfwJoystickIsGamepad(joystick);
#endif
            if (!device.connected) continue;
            device.axisCount = std::clamp(GetGamepadAxisCount(joystick), 0, 8);
            for (int axis = 0; axis < device.axisCount; ++axis)
                device.axes[axis] = GetGamepadAxisMovement(joystick, axis);
            for (int button = 0; button < 32; ++button)
                device.buttons[button] = IsGamepadButtonDown(joystick, button);
        }
    }
#if defined(PS2X_GAME_GT4)
    applyRumble(); // after player 2's poll: a pad player 2 claimed this frame is never written by player 1
#endif
    int claimedKey = 0;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // The Race HUD zoom (HUD hand-off 9): its controller button out of GT4's mapping (a press counted on its way down),
    // its key out of the keyboard's (the presenter reads the key itself, ps2_runtime.cpp).
    {
        const bool down = claimZoomButton(devices, m_profile);
        if (down && !m_zoomDown)
            gt4hud::zoomPress();
        m_zoomDown = down;
        claimedKey = gt4hud::zoomKey();
    }
#endif
    const bool useGamepad = ps2_pad::hasBoundDevice(m_profile, devices);

    if (useGamepad)
    {
        publishDevices(devices);
        return;
    }
    else if (windowReady && m_profile.keyboardFallback)
        btns = keyboardButtons([](int k) { return IsKeyDown(k); }, claimedKey);

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    // The pressure bytes (read in pressure mode): pressed keys fully pressed
    // (DualShock 2 order: right, left, up, down, triangle, circle, cross,
    // square, L1, R1, L2, R2; btns is active low).
    {
        constexpr uint16_t pressureMasks[12] = {0x20, 0x80, 0x10, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800, 0x100, 0x200};
        for (int i = 0; i < 12; ++i)
            data[8 + i] = (btns & pressureMasks[i]) ? 0 : 255;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::memcpy(m_snapshot.data(), data, sizeof(data));
}

void PSPadBackend::zoomSelfTest()
{
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
    // What GT4 would get with the Race HUD zoom key, then its controller button, held down (HUD hand-off 9), through the same
    // keyboard and mapping code poll() uses; and the same without the claim, for comparison.
    const int key = gt4hud::zoomKey(), zb = gt4hud::zoomPadButton();
    if (key > 0)
    {
        auto only = [key](int k) { return k == key; };
        std::fprintf(stderr, "[zoom-selftest] keyboard: key %c down -> GT4's buttons %04x (none pressed: ffff); without the claim %04x\n", key,
                     keyboardButtons(only, key), keyboardButtons(only, 0));
    }
    else
        std::fprintf(stderr, "[zoom-selftest] keyboard: no zoom key (the radar or its zoom off, or GT4_HUD_ZOOM_KEY=none)\n");
    if (zb >= 0)
    {
        ps2_pad::Devices d{};
        d[0].connected = true;
        d[0].axisCount = 6;
        d[0].axes[4] = d[0].axes[5] = -1.0f; // (the triggers at rest)
        d[0].buttons[static_cast<size_t>(zb)] = true;
        const ps2_pad::Devices open = d;
        const bool down = claimZoomButton(d, m_profile);
        const auto a = ps2_pad::mapDevices(m_profile, d), b = ps2_pad::mapDevices(m_profile, open);
        int pa = 0, pb = 0;
        for (int i = 8; i < 20; ++i)
            pa += a[static_cast<size_t>(i)], pb += b[static_cast<size_t>(i)];
        std::fprintf(stderr, "[zoom-selftest] controller: button %d down -> zoom press %s; GT4's buttons %02x%02x, pressure sum %d (none pressed: ffff, 0); "
                             "without the claim %02x%02x, pressure sum %d\n",
                     zb, down ? "yes" : "no", a[3], a[2], pa, b[3], b[2], pb);
    }
    else
        std::fprintf(stderr, "[zoom-selftest] controller: no zoom button (GT4_HUD_ZOOM_PAD=none, the default)\n");
#endif
}
