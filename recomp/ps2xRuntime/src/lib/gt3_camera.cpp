#include "gt3_camera.h"

#include "ps2_runtime.h"
#include "runtime/ps2_input_profile.h"
#include "runtime/gs/gs_frontend.h"
#include "gt3_aspect.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>

namespace gt3camera
{
    namespace
    {
        // GT3 USA CORE (RAM dumps of Trial Mountain races, 2026-09-29):
        // - the race view object (vtable 0x328d30) has its camera at +0x40
        //   (camera vtable 0x33eeb8; the view's getter 0x17bbd0 returns this + 0x40);
        // - each race frame calls camera->vtable[0x14] = 0x209e58(camera):
        //   the camera mode at camera + 0x5d4 + index * 0x24c0 (index at
        //   camera + 0x96d8, 0 in races; slots 1-3 are not set up there)
        //   computes the camera;
        // - that mode's word +0x100 is GT3's view: 0 bumper, 1 chase. R1 (the
        //   view button) flips it; GT3 draws the frame from it (the player's
        //   car in chase), so forcing it before 0x209e58 switches the view;
        // - after it: the world -> view matrix at camera + 0x640 (row vectors,
        //   columns right, up, back; translation row), the eye at +0x690 and
        //   +0x6a0, the car followed at +0x4e0 (x, y, z, yaw). The frame is
        //   drawn from that matrix (rewriting it after 0x209e58 turns the
        //   picture; verified with a fixed 90 degree turn).
        // The race frame (0x17e740) is also run inline by its caller, so it is
        // not hooked; the camera call is.
        constexpr uint32_t kCameraApply = 0x209e58u;
        constexpr uint32_t kRamMask = 0x1FFFFFFu;
        constexpr uint32_t kCameraVtable = 0x33eeb8u, kRaceViewVtable = 0x328d30u;
        constexpr uint32_t kMatrix = 0x640u, kEye = 0x690u, kEye2 = 0x6a0u, kTarget = 0x4e0u;
        constexpr uint32_t kModeIndex = 0x96d8u, kModes = 0x5d4u, kModeStride = 0x24c0u, kViewFlag = 0x100u;
        constexpr uint32_t kVblankCount = 0x4a69c0u;
        PS2Runtime::RecompiledFunction g_cameraApply = nullptr;
        uint32_t u32(const uint8_t *rdram, uint32_t address);

        // The race view: 0x328d30 is the Arcade race's; each game mode has its
        // own class of the same family (Free Run's 0x32b8f8 - the extra views,
        // look-around and HUD boxes never saw Free Run, user 2026-09-30), 18 in
        // the CORE's data at 0x327c68..0x32d518, all with the cars' draw
        // 0x17f730 at vtable + 0x19c. Any of them is a race view.
        bool isRaceView(const uint8_t *rdram, uint32_t vtable)
        {
            return vtable == kRaceViewVtable ||
                   (vtable >= 0x327000u && vtable < 0x32e000u && (vtable & 3u) == 0u && u32(rdram, vtable + 0x19cu) == 0x17f730u);
        }

        enum View
        {
            Bumper,
            Bonnet,
            Roof,
            CloseChase,
            Chase,
            FarChase,
            ViewCount
        };
        const char *const kViewKeys[ViewCount] = {"bumper", "bonnet", "roof", "close", "chase", "far"};
        const char *const kViewNames[ViewCount] = {"Bumper", "Bonnet", "Roof", "Close chase", "Chase", "Far chase"};
        bool chaseBased(int v) { return v != Bumper; } // GT3 draws the player's car in its chase view

        enum class Look
        {
            Off,
            Sideways,
            Full
        };

        struct Settings
        {
            bool enabled[ViewCount] = {true, true, true, true, true, true};
            int start = -1; // GT3's own view at a race's start
            Look look = Look::Off;
            float bonnet[3] = {0.8f, 1.0f, -8.0f}; // ahead, up (m, from the car GT3 follows), pitch (degrees)
            // Roof: at the top of the windscreen (both need clipSetupHook: with
            // the camera inside the car GT3 would cull its body).
            float roof[3] = {-0.3f, 1.55f, -10.0f};
            // Close chase: 0.7 of GT3's distance, 0.35 m higher, 4 degrees
            // down (user: "come up a bit higher, you can't see the track");
            // far chase: 1.5 times ("maybe half way" from double).
            float close = 0.7f, closeRaise = 0.35f, closePitch = -4.0f, far = 1.5f;
        };
        int viewFromName(const std::string &name)
        {
            for (int v = 0; v < ViewCount; ++v)
                if (name == kViewKeys[v])
                    return v;
            return -1;
        }
        void parseTriple(const char *name, float out[3])
        {
            const char *v = std::getenv(name);
            if (!v || !*v)
                return;
            float t[3];
            if (std::sscanf(v, "%f,%f,%f", &t[0], &t[1], &t[2]) == 3)
                std::memcpy(out, t, sizeof(t));
        }
        Settings loadSettings()
        {
            Settings s;
            if (const char *v = std::getenv("GT3_CAMERA_VIEWS"); v && *v)
            {
                std::fill(std::begin(s.enabled), std::end(s.enabled), false);
                std::stringstream list(v);
                std::string item;
                while (std::getline(list, item, ','))
                    if (const int view = viewFromName(item); view >= 0)
                        s.enabled[view] = true;
                if (std::none_of(std::begin(s.enabled), std::end(s.enabled), [](bool b) { return b; }))
                    s.enabled[Bumper] = s.enabled[Chase] = true;
            }
            if (const char *v = std::getenv("GT3_CAMERA_START"); v && *v)
                s.start = viewFromName(v);
            if (const char *v = std::getenv("GT3_CAMERA_LOOK"); v && *v)
                s.look = std::strcmp(v, "full") == 0 ? Look::Full : (std::strcmp(v, "sideways") == 0 ? Look::Sideways : Look::Off);
            parseTriple("GT3_CAMERA_BONNET", s.bonnet);
            parseTriple("GT3_CAMERA_ROOF", s.roof);
            return s;
        }
        // The settings: from the environment at start, then changed live by
        // applyLive (the launcher's Camera page while the game runs). Plain
        // bools / ints, read by the game and host threads.
        Settings &mutableSettings()
        {
            static Settings s = loadSettings();
            return s;
        }
        const Settings &settings() { return mutableSettings(); }

        bool logOn()
        {
            static const bool log = [] {
                const char *v = std::getenv("GT3_CAMERA_LOG");
                return v && *v == '1';
            }();
            return log;
        }

        // Shared between the host thread (keys, stick) and the game's.
        std::atomic<int> g_cycle{0};          // F9 steps not yet taken
        std::atomic<float> g_lookTarget{0.f}; // radians, right positive
        std::atomic<int> g_announce{-1};      // a view to name on screen
        std::atomic<int64_t> g_raceSeenMs{0}; // the race camera's last call (steady clock)

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        uint32_t u32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & kRamMask), 4);
            return v;
        }
        void setU32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & kRamMask), &v, 4); }
        float f32(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & kRamMask), 4);
            return v;
        }
        void setF32(uint8_t *rdram, uint32_t address, float v) { std::memcpy(rdram + (address & kRamMask), &v, 4); }

        struct Vec
        {
            float x = 0, y = 0, z = 0;
        };
        Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        Vec operator*(Vec a, float k) { return {a.x * k, a.y * k, a.z * k}; }
        float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        // About the vertical axis; positive turns to the right (seen from above,
        // as the camera's right vector lies from its forward one).
        Vec turn(Vec v, float right)
        {
            const float c = std::cos(-right), s = std::sin(-right);
            return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
        }
        Vec readVec(const uint8_t *rdram, uint32_t address) { return {f32(rdram, address), f32(rdram, address + 4u), f32(rdram, address + 8u)}; }

        // The camera: axes (right, up, back) and the eye.
        struct Pose
        {
            Vec right, up, back, eye;
        };
        Pose readPose(const uint8_t *rdram, uint32_t camera)
        {
            Pose p;
            auto column = [&](int k) {
                return Vec{f32(rdram, camera + kMatrix + static_cast<uint32_t>(k * 4)), f32(rdram, camera + kMatrix + 16u + static_cast<uint32_t>(k * 4)),
                           f32(rdram, camera + kMatrix + 32u + static_cast<uint32_t>(k * 4))};
            };
            p.right = column(0);
            p.up = column(1);
            p.back = column(2);
            p.eye = readVec(rdram, camera + kEye);
            return p;
        }
        // `listener` (if set): the point written as the eye instead of p.eye
        // (the matrix still uses p.eye; see the bonnet view).
        void writePose(uint8_t *rdram, uint32_t camera, const Pose &p, bool eyeToo = true, const Vec *listener = nullptr, bool oldEyeToo = true)
        {
            const Vec axes[3] = {p.right, p.up, p.back};
            for (int k = 0; k < 3; ++k)
            {
                setF32(rdram, camera + kMatrix + static_cast<uint32_t>(k * 4), axes[k].x);
                setF32(rdram, camera + kMatrix + 16u + static_cast<uint32_t>(k * 4), axes[k].y);
                setF32(rdram, camera + kMatrix + 32u + static_cast<uint32_t>(k * 4), axes[k].z);
                setF32(rdram, camera + kMatrix + 48u + static_cast<uint32_t>(k * 4), -dot(p.eye, axes[k]));
            }
            // The eye (+0x690) and the eye a frame ago (+0x6a0): GT3's sound takes
            // the listener's motion from the two (2026-09-30: writing the same
            // point to both stopped the engine's pitch - "no sound" in bonnet,
            // roof and close chase). The old eye is written as ours less GT3's
            // own camera motion this frame.
            if (!eyeToo)
                return;
            const Vec gt3Motion = readVec(rdram, camera + kEye) - readVec(rdram, camera + kEye2);
            const Vec eye = listener ? *listener : p.eye;
            const Vec previous = eye - gt3Motion;
            // GT3_CAMERA_EYES=1|2|3 (test): which of the two are written (bits; default both).
            static const int eyes = [] {
                const char *v = std::getenv("GT3_CAMERA_EYES");
                return (v && *v) ? std::atoi(v) : 3;
            }();
            if (eyes & 1)
            {
                setF32(rdram, camera + kEye, eye.x);
                setF32(rdram, camera + kEye + 4u, eye.y);
                setF32(rdram, camera + kEye + 8u, eye.z);
            }
            if ((eyes & 2) && oldEyeToo)
            {
                setF32(rdram, camera + kEye2, previous.x);
                setF32(rdram, camera + kEye2 + 4u, previous.y);
                setF32(rdram, camera + kEye2 + 8u, previous.z);
            }
        }

        // GT3's own pose (the matrix entries and eyes writePose changes) as its
        // camera left it, for the camera we re-placed last: put back before
        // GT3's next camera update. GT3 derives the camera's motion from its
        // previous pose; seeing ours (metres from its chase camera) it moved
        // metres a frame, and the Doppler shift took the engine pitch to 0 or
        // off the scale (user: "the sound from bonnet cam and roof cam and I
        // think close chase cam isn't working"; GT3_SPU_VOICE_LOG, 2026-09-30).
        // GT3_CAMERA_RESTORE=0: off.
        struct SavedPose
        {
            uint32_t camera = 0;
            bool valid = false;
            uint8_t matrix[64];
            uint8_t eyes[2][12];
        };
        SavedPose g_gt3Pose;
        void saveGt3Pose(const uint8_t *rdram, uint32_t camera)
        {
            g_gt3Pose.camera = camera;
            g_gt3Pose.valid = true;
            std::memcpy(g_gt3Pose.matrix, rdram + ((camera + kMatrix) & kRamMask), 64);
            std::memcpy(g_gt3Pose.eyes[0], rdram + ((camera + kEye) & kRamMask), 12);
            std::memcpy(g_gt3Pose.eyes[1], rdram + ((camera + kEye2) & kRamMask), 12);
        }
        void restoreGt3Pose(uint8_t *rdram, uint32_t camera)
        {
            // Off by default: with it GT3's chase camera, followed by far / close
            // chase, stayed behind when switched to mid-race ("the camera stays
            // still and the car drives off", 2026-09-30). GT3_CAMERA_RESTORE=1: on.
            static const bool restore = [] {
                const char *v = std::getenv("GT3_CAMERA_RESTORE");
                return v && *v == '1';
            }();
            if (!restore || !g_gt3Pose.valid || g_gt3Pose.camera != camera)
                return;
            std::memcpy(rdram + ((camera + kMatrix) & kRamMask), g_gt3Pose.matrix, 64);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), g_gt3Pose.eyes[0], 12);
            std::memcpy(rdram + ((camera + kEye2) & kRamMask), g_gt3Pose.eyes[1], 12);
        }

        // The eye for the track only. GT3's sound uses the camera's eye (+0x690,
        // and +0x6a0 = the eye a frame ago) as its listener: any eye of ours
        // there broke the engine's pitch in the bonnet view. But the track draw
        // 0x1a92e8(track, grid, camera, flags) picks the visible sections from
        // the eye (the camera's getters at vtable +0x24/+0x2c/+0x34 ->
        // 0x226950): with GT3's chase eye, sections around a bonnet camera were
        // missing (white sky where a tunnel is). So the camera keeps GT3's eye
        // and ours is put in for that call only (trackDrawHook).
        // GT3_CAMERA_TRACK_EYE=0: the eye written for the whole frame (as before).
        PS2Runtime::RecompiledFunction g_trackDraw = nullptr;
        uint32_t g_trackEyeCamera = 0u; // the camera whose track draw gets g_trackEye (0: none)
        float g_trackEye[3] = {};
        void sideTrackGrid(const uint8_t *rdram, R5900Context *ctx); // angled side screens (below)
        void trackDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            sideTrackGrid(rdram, ctx);
            const uint32_t camera = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0)) & kRamMask;
            if (g_trackEyeCamera == 0u || camera != g_trackEyeCamera)
            {
                g_trackDraw(rdram, ctx, runtime);
                return;
            }
            uint8_t saved[12];
            std::memcpy(saved, rdram + ((camera + kEye) & kRamMask), 12);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), g_trackEye, 12);
            g_trackDraw(rdram, ctx, runtime);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), saved, 12);
        }

        // GT3_CAMERA_GETTER_LOG=1 (diagnostic): who asks the race camera for its
        // position (the camera's getters 0x209d58 / 0x209d10 / 0x209cc8 =
        // vtable +0x24 / +0x2c / +0x34, which forward to the mode's), and the
        // mode's vtable once: every 600 vblanks the callers (return
        // addresses) with their counts.
        //
        // The sound: GT3's engine Doppler (0x177a50, once per car) takes the
        // listener from these getters (calls returning to 0x177cb8 / 0x177cd4
        // / 0x177cf0) and its motion as that minus the old eye at camera +
        // 0x6a0 (the mode object is camera + 0x600; its +0xa0). With our eye
        // in the camera the listener jumped metres a frame and the engine's
        // pitch fell to 0 (bonnet, roof, close chase; 2026-09-30). So those
        // three calls get GT3's own eye of this frame (g_soundEye), the old eye
        // stays GT3's, and everything else (the track draw's sections, the
        // sky, ...) sees ours: the sound of GT3's own chase view.
        // GT3_CAMERA_SOUND_EYE=0: off.
        PS2Runtime::RecompiledFunction g_getter[3] = {};
        std::map<uint32_t, int> g_getterCallers[3];
        bool g_getterLog = false;
        uint32_t g_soundEyeCamera = 0u; // the camera whose Doppler gets g_soundEye (0: none)
        float g_soundEye[3] = {};
        // GT3's eye of the frame before (for the old eye the Doppler subtracts).
        uint32_t g_prevGt3EyeCamera = 0u;
        Vec g_prevGt3Eye{};
        template <int K>
        void getterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t camera = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            if (g_getterLog)
                ++g_getterCallers[K][ra];
            g_getter[K](rdram, ctx, runtime);
            static const uint32_t doppler[3] = {0x177cb8u, 0x177cd4u, 0x177cf0u};
            // GT3_CAMERA_SOUND_CALLERS (test): hex return addresses of the +0x24
            // getter call (the +0x2c / +0x34 ones follow at +0x18 / +0x30) that also
            // get GT3's eye; "all": every caller but the track draw.
            static const std::vector<uint32_t> extra = [] {
                std::vector<uint32_t> out;
                if (const char *v = std::getenv("GT3_CAMERA_SOUND_CALLERS"))
                {
                    if (std::strcmp(v, "all") == 0)
                        out.push_back(0xFFFFFFFFu);
                    std::stringstream list(v);
                    std::string item;
                    while (std::getline(list, item, ','))
                        if (item != "all" && !item.empty())
                            out.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
                }
                return out;
            }();
            bool sound = ra == doppler[K];
            for (const uint32_t e : extra)
                sound |= e == 0xFFFFFFFFu ? (ra != 0x1a932cu + K * 0x18u) : ra == e + K * 0x18u;
            if (g_soundEyeCamera != 0u && camera == g_soundEyeCamera && sound)
                ctx->f[0] = g_soundEye[K];
        }

        // The cars' sound update 0x177a50(?, camera) (the engine Doppler above
        // is part of it) also reads the camera's eye and matrix directly: with
        // only the getters answered, bonnet / roof still had most engine
        // voices at pitch 0. So while it runs the camera holds GT3's own eye
        // and matrix of this frame (saved in g_gt3Pose before ours were
        // written), and ours are put back after it: the sound of GT3's own
        // chase view, the picture from ours. GT3_CAMERA_SOUND_EYE=0: off.
        PS2Runtime::RecompiledFunction g_soundUpdate = nullptr;
        uint32_t g_soundViewMode = 0u; // bonnet / roof: the camera mode whose view flag reads bumper during the sound update
        void soundUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) & kRamMask;
            // GT3_CAMERA_INCAR_SOUND=1 (test): bonnet and roof with the view
            // flag reading 0 (GT3's bumper view) while the sound update runs.
            // Negative: more engine voices at pitch 0 (vsw8), so off.
            static const bool inCar = [] {
                const char *v = std::getenv("GT3_CAMERA_INCAR_SOUND");
                return v && *v == '1';
            }();
            if (inCar && g_soundViewMode != 0u)
            {
                const uint32_t flag = u32(rdram, g_soundViewMode + kViewFlag);
                setU32(rdram, g_soundViewMode + kViewFlag, 0u);
                g_soundUpdate(rdram, ctx, runtime);
                setU32(rdram, g_soundViewMode + kViewFlag, flag);
                return;
            }
            if (g_soundEyeCamera == 0u || camera != g_soundEyeCamera || !g_gt3Pose.valid || g_gt3Pose.camera != camera)
            {
                g_soundUpdate(rdram, ctx, runtime);
                return;
            }
            uint8_t matrix[64], eye[12];
            std::memcpy(matrix, rdram + ((camera + kMatrix) & kRamMask), 64);
            std::memcpy(eye, rdram + ((camera + kEye) & kRamMask), 12);
            std::memcpy(rdram + ((camera + kMatrix) & kRamMask), g_gt3Pose.matrix, 64);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), g_gt3Pose.eyes[0], 12);
            g_soundUpdate(rdram, ctx, runtime);
            std::memcpy(rdram + ((camera + kMatrix) & kRamMask), matrix, 64);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), eye, 12);
        }

        // The cars' sound frame 0x17da58 (it sets up the view from the camera
        // (0x257B30 and a camera call), then runs the sound update 0x177a50,
        // 0x1779D8, 0x1AB0F8 and 0x1A79C0): wrapped whole like 0x177a50, so
        // everything it computes from the camera is from GT3's own chase view.
        PS2Runtime::RecompiledFunction g_soundFrame = nullptr;
        void soundFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = g_soundEyeCamera;
            if (camera == 0u || !g_gt3Pose.valid || g_gt3Pose.camera != camera)
            {
                g_soundFrame(rdram, ctx, runtime);
                return;
            }
            uint8_t matrix[64], eye[12];
            std::memcpy(matrix, rdram + ((camera + kMatrix) & kRamMask), 64);
            std::memcpy(eye, rdram + ((camera + kEye) & kRamMask), 12);
            std::memcpy(rdram + ((camera + kMatrix) & kRamMask), g_gt3Pose.matrix, 64);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), g_gt3Pose.eyes[0], 12);
            g_soundFrame(rdram, ctx, runtime);
            std::memcpy(rdram + ((camera + kMatrix) & kRamMask), matrix, 64);
            std::memcpy(rdram + ((camera + kEye) & kRamMask), eye, 12);
        }

        // ---- Angled side screens (triple monitors) ----
        // GT3_TRIPLE_SIDES=angled, GT3_TRIPLE_ANGLE=<degrees> (the side screens'
        // angle toward the driver). After the race frame's scene (its call of
        // 0x19AD00, reached every frame after the scene and the mirror), GT3's
        // mirror render 0x180330(view, sp, 0) is run once per side screen with
        // the rectangle at view + 0xC6F4 set to that screen's part of the
        // picture. It draws GT3's scene from the race camera (view + 0x40)
        // into the rectangle: the camera's update (0x209e58, cameraApplyHook)
        // then gets the main view's pose turned by the angle, its field of
        // view (camera vtable +0x54 = 0x209c00) and pixel shape (0x257EF0)
        // the main view's, the rectangle's scale call (0x257F04 at 0x180530)
        // the main view's scale (gt3aspect), the mirror flip (0x257F04(1, 1,
        // -1) at 0x18055c) none, and its border lines (0x2566F0) are skipped.
        int g_side = 0;                // a side screen being drawn: -1 left, +1 right
        bool g_sidesOn = false;
        float g_sideAngle = 0.f;       // radians
        float g_sideGap = 0.f;         // GT3_TRIPLE_BEZEL: the gap between two screens' pictures, screen widths
        struct MainPose
        {
            uint32_t camera = 0u, vblank = 0u;
            bool valid = false;
            float axes[3][3], eye[3];
        };
        MainPose g_mainPose;
        float g_mainFov = 0.f, g_mainPixelAspect = 0.f;
        bool g_mainFovValid = false, g_mainPixelAspectValid = false;
        PS2Runtime::RecompiledFunction g_mirrorOriginal = nullptr, g_afterScene = nullptr, g_fovSet = nullptr, g_pixelAspect = nullptr,
                                       g_mirrorBorder = nullptr;
        constexpr uint32_t kMirrorRectOffset = 0xC6F4u;

        void fovSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            if (g_side == 0 && ra == 0x17e868u)
            {
                g_mainFov = ctx->f[12];
                g_mainFovValid = true;
            }
            else if (g_side != 0 && g_mainFovValid)
                ctx->f[12] = g_mainFov;
            g_fovSet(rdram, ctx, runtime);
        }
        void pixelAspectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            if (g_side == 0 && ra == 0x17e888u)
            {
                g_mainPixelAspect = ctx->f[12];
                g_mainPixelAspectValid = true;
            }
            else if (g_side != 0 && g_mainPixelAspectValid)
                ctx->f[12] = g_mainPixelAspect;
            g_pixelAspect(rdram, ctx, runtime);
        }
        // 0x251930(a0): a render state at scratchpad 0x700031C8 the mirror
        // render sets to 1 (its picture is flipped, so the triangles' facing
        // is reversed) and back to 0 at its end: a side screen, not flipped,
        // keeps 0 (else the road and sky faces are culled).
        PS2Runtime::RecompiledFunction g_mirrorFacing = nullptr;
        void mirrorFacingHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool keep = !(std::getenv("GT3_SIDE_FACING") && *std::getenv("GT3_SIDE_FACING") == '1');
            if (g_side != 0 && keep)
                ctx->r[4] = _mm_set_epi64x(0, 0);
            g_mirrorFacing(rdram, ctx, runtime);
        }
        // 0x225C50(a0) stores gp[-0x4F0C]: with it set, the model runner
        // 0x2250d8 picks trackside models' detail level (forced to gp[-0x4F04],
        // which the mirror sets to 2 with 0x225C60, or by distance); cleared,
        // they are drawn whole (level -1), as in the main view, whose frame
        // never sets it. The mirror render sets it around its track draw.
        // Side screens keep it clear: the main view's scenery detail (the
        // mirror's is low-detail models with blurred low-resolution textures;
        // full costs ~1 fps at Trial Mountain's start). GT3_SIDE_DETAIL=mirror:
        // the mirror's detail.
        //
        // Correction (same night): the main view's own track draws (view
        // methods at 0x17f644 / 0x17f320) set 0x225C50(1) and 0x225C60(-1):
        // levels by distance through the spheres, not whole models; level -1
        // (switching off) picks each chunk's base table (model +0x2c), full
        // detail at every distance. So side screens now get the main view's
        // setting: the mirror's 0x225C60(2) becomes -1 (forcedLevelHook).
        // GT3_SIDE_DETAIL=whole: switching off instead (0x225C50(0)).
        PS2Runtime::RecompiledFunction g_modelLevels = nullptr, g_forcedLevel = nullptr;
        bool g_sideFullDetail = false;
        void modelLevelsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_side != 0 && g_sideFullDetail)
                ctx->r[4] = _mm_set_epi64x(0, 0);
            g_modelLevels(rdram, ctx, runtime);
        }
        // GT3_MIRROR_DETAIL=full: GT3's own rear-view mirror (the race frame's
        // call of the mirror render at 0x17ea38, return 0x17ea40) at the main
        // view's detail too: its 0x225C60(2) (return 0x1805cc) becomes -1 and
        // its cars are drawn as the race frame draws them (carsDrawHook), with
        // the race frame's env map (its sp, the mirror render's a1, + 0x40).
        bool g_sideLevelsByDistance = false, g_mirrorFull = false;
        // GT3_MIRROR_DETAIL_PARTS (diagnostic): 1 the scenery's detail only, 2 the cars only.
        bool g_mirrorFullScenery = true, g_mirrorFullCars = true;
        uint32_t g_mirrorRaceSp = 0u;
        void forcedLevelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool side = g_side != 0;
            if ((side && g_sideLevelsByDistance) ||
                (!side && g_mirrorFull && g_mirrorFullScenery && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x1805ccu))
                ctx->r[4] = _mm_set_epi64x(0, -1);
            g_forcedLevel(rdram, ctx, runtime);
        }
        PS2Runtime::RecompiledFunction g_mirrorEntry = nullptr;
        void mirrorEntryHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x17ea40u)
                g_mirrorRaceSp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
            g_mirrorEntry(rdram, ctx, runtime);
        }
        // The track's visibility grid: the main view draws with the course's
        // grid +0x8 and flags [+0x80] +0xF30, the mirror (0x1805e8..0x1805f8)
        // with +0xC (or +0x1C) and +0xF38. GT3_SIDE_GRID=main: side renders use
        // the main view's (see trackDrawHook).
        bool g_sideMainGrid = false;
        void sideTrackGrid(const uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_sideMainGrid || g_side == 0 || static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) != 0x1805fcu)
                return;
            const uint32_t course = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            const uint32_t info = u32(rdram, course + 0x80u) & kRamMask;
            if (course < 0x100000u || info < 0x100000u)
                return;
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(u32(rdram, course + 0x8u))));
            ctx->r[7] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(u32(rdram, info + 0xf30u))));
        }
        // A guest stack word (the EE stack may be in RAM or the scratchpad).
        uint32_t stackWord(const uint8_t *rdram, PS2Runtime *runtime, uint32_t address)
        {
            uint32_t v = 0u;
            if ((address & 0xF0000000u) == 0x70000000u)
            {
                if (const uint8_t *scratch = runtime->memory().getScratchpad())
                    std::memcpy(&v, scratch + (address & 0x3FFCu), 4);
                return v;
            }
            return u32(rdram, address);
        }
        // Called from the model interpreter 0x2250d8 (its frame: addiu sp, sp,
        // -0xA0, ra at +0x90; the hooked callee has not moved sp yet): true
        // when that run came through 0x225090 (return 0x2250b8): an object
        // model - cars, their wheels, models nested in another model.
        bool objectModelRun(const uint8_t *rdram, const R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            return stackWord(rdram, runtime, sp + 0x90u) == 0x2250b8u;
        }

        // GT3_HOOK_YIELD_LOG=1 (diagnostic): every 600 VBlanks, how often the
        // runs these hooks wrap were stopped at a checkpoint, and how often the
        // old wrapper flags disagreed with the interpreter's own frame.
        struct YieldStats
        {
            uint64_t objectRuns = 0, objectRunsYielded = 0, carsDraws = 0, carsDrawsYielded = 0;
            uint64_t clipTests = 0, clipFlagWrong = 0, clipFixed = 0, clipFixedFlagWrong = 0;
            uint64_t levelPicks = 0, levelFlagWrong = 0;
            uint64_t cameraUpdates = 0, cameraUpdatesYielded = 0;
        } g_yield;
        bool yieldLogOn()
        {
            static const bool on = std::getenv("GT3_HOOK_YIELD_LOG") != nullptr;
            return on;
        }
        // Whether the guest call just made from a hook stopped at a checkpoint
        // (the flag left as it was).
        bool callYielded()
        {
            const bool y = PS2Runtime::exchangeEeYieldTaken(false);
            PS2Runtime::exchangeEeYieldTaken(y);
            return y;
        }
        void yieldReport(const uint8_t *rdram)
        {
            static uint32_t last = 0u;
            const uint32_t now = u32(rdram, kVblankCount);
            if (!yieldLogOn() || now - last < 600u)
                return;
            last = now;
            const YieldStats &s = g_yield;
            std::fprintf(stderr,
                         "[hook-yield] vblank %u: object model runs %llu (stopped at a checkpoint %llu), cars draws %llu (stopped %llu); "
                         "car chunk tests %llu (old flag wrong %llu), chunks given the clipped draw %llu (old flag would have culled %llu); "
                         "object level picks %llu (old flag wrong %llu); race camera updates %llu (stopped %llu)\n",
                         now, static_cast<unsigned long long>(s.objectRuns), static_cast<unsigned long long>(s.objectRunsYielded),
                         static_cast<unsigned long long>(s.carsDraws), static_cast<unsigned long long>(s.carsDrawsYielded),
                         static_cast<unsigned long long>(s.clipTests), static_cast<unsigned long long>(s.clipFlagWrong),
                         static_cast<unsigned long long>(s.clipFixed), static_cast<unsigned long long>(s.clipFixedFlagWrong),
                         static_cast<unsigned long long>(s.levelPicks), static_cast<unsigned long long>(s.levelFlagWrong),
                         static_cast<unsigned long long>(s.cameraUpdates), static_cast<unsigned long long>(s.cameraUpdatesYielded));
        }

        // The cars: the view method +0x198 (0x17f730(view, envViewport, level,
        // flag)) draws each car with 0x1AAA90 (the mirror's simple car) when
        // level > 0, else with the full draw 0x1AA7A0 (level -1: by distance;
        // flag: the other cars' mode 2, the player's 1). The race frame calls
        // it with (its 256x256 env map viewport at sp + 0x40, -1, 1), the
        // mirror with (its 64x64 one, 2, 0) from 0x180630. Side screens call it
        // as the race frame does (full cars, reflections from the main view's
        // env map; `sccar*`: the mirror's are blurred low-detail models), unless
        // GT3_SIDE_DETAIL=mirror or GT3_SIDE_CARS=mirror.
        PS2Runtime::RecompiledFunction g_carsDraw = nullptr;
        bool g_sideFullCars = false;
        uint32_t g_raceSp = 0u; // the race frame's stack pointer while its side screens are drawn
        int g_carsPass = -1;    // the cars draw under way: 0 the race frame's, 1 the mirror's, 2 a side screen's

        // The cars the race frame's cars draw has drawn the full way (their
        // wheels through 0x177938, see carWheelsHook), for GT3_CARS_DIFF.
        std::vector<uint32_t> g_carsSeen;

        // GT3_CARS_DIFF=<vblank> (diagnostic): from that VBlank for 600, the
        // words of each car object (0..0x1900) that a cars draw changes, per
        // pass, printed with how many draws changed them.
        void carsDiff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int pass)
        {
            static const uint32_t from = [] {
                const char *v = std::getenv("GT3_CARS_DIFF");
                return (v && *v) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
            }();
            const uint32_t now = u32(rdram, kVblankCount);
            static bool printed = false;
            if (from == 0u || now < from || printed)
            {
                g_carsDraw(rdram, ctx, runtime);
                return;
            }
            constexpr uint32_t kSize = 0x1900u;
            static std::map<uint32_t, uint32_t> changed[3];
            static uint32_t draws[3] = {};
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> before;
            for (const uint32_t car : g_carsSeen)
                before.emplace_back(car, std::vector<uint8_t>(rdram + car, rdram + car + kSize));
            g_carsDraw(rdram, ctx, runtime);
            ++draws[pass];
            for (const auto &[car, bytes] : before)
                for (uint32_t o = 0; o < kSize; o += 4u)
                    if (std::memcmp(bytes.data() + o, rdram + car + o, 4) != 0)
                        ++changed[pass][o];
            if (now >= from + 600u)
            {
                printed = true;
                static const char *names[3] = {"race frame", "mirror", "side"};
                for (int p = 0; p < 3; ++p)
                {
                    std::fprintf(stderr, "[cars-diff] %s: %u draws, words changed (car offset x draws):", names[p], draws[p]);
                    for (const auto &[o, n] : changed[p])
                        std::fprintf(stderr, " %x x%u", o, n);
                    std::fprintf(stderr, "\n");
                }
            }
        }

        // The wheel draw 0x2222B8 (a0 the wheel, a1 its level or -1 by
        // distance, f16 the spin blur: the wheel's turn over the blur's time
        // span, from its screen motion since its last draw). A turning wheel
        // (f16 != 0) gets GT3's spin blur: its rim is rebuilt each frame in a
        // small target (FBP 0x16f, 64 x 192) and drawn from it - visibly
        // smaller than the still rim, also in PCSX2 (user, 2026-10-02: "the
        // tyre actually gets bigger ... and like clips in to the rims").
        // Wheel spin blur off (GT3_WHEEL_BLUR=0, live wheel_blur; launcher
        // Detail page): f16 = 0, the still rim, turning, every frame.
        // GT3_WHEEL_LOG=<vblank>,<count> (diagnostic): each call with the cars pass.
        PS2Runtime::RecompiledFunction g_wheelDraw = nullptr;
        std::atomic<bool> g_wheelBlur{[] {
            const char *v = std::getenv("GT3_WHEEL_BLUR");
            return !(v && *v == '0');
        }()};
        void wheelDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const std::pair<uint32_t, uint32_t> range = [] {
                std::pair<uint32_t, uint32_t> r{0u, 0u};
                if (const char *v = std::getenv("GT3_WHEEL_LOG"); v && *v)
                    std::sscanf(v, "%u,%u", &r.first, &r.second);
                return r;
            }();
            const uint32_t now = u32(rdram, kVblankCount);
            if (now >= range.first && now < range.first + range.second)
                std::fprintf(stderr, "[wheel] vb %u pass %d wheel 0x%x level %d ra 0x%x f12 %.3f f13 %.3f f14 %.3f f15 %.3f blur %.4f\n", now, g_carsPass,
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)), static_cast<int32_t>(_mm_extract_epi32(ctx->r[5], 0)),
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), ctx->f[12], ctx->f[13], ctx->f[14], ctx->f[15], ctx->f[16]);
            if (!g_wheelBlur.load(std::memory_order_relaxed))
                ctx->f[16] = 0.0f;
            g_wheelDraw(rdram, ctx, runtime);
        }

        // The wheels' last screen positions (car + 0x11F8 + 0x66C, 4 x 16
        // bytes): the full wheel draw 0x1AA7A0 (the race frame's path, a1 != 0)
        // projects each wheel's centre, blurs the rim by its screen motion since
        // the position kept there (the spin blur's angle span, f16 of 0x2222B8)
        // and keeps the new one. GT3's own mirror draws the cars' simple path,
        // which leaves them alone; drawn the full way from the mirror or a side
        // screen, a car seen in both views had its position swapped between
        // the views twice a frame, so its rims blurred into flat discs while it
        // ran alongside (user, 2026-10-02: "the rims would change to like a low
        // poly version"). Each extra view keeps its own positions.
        // GT3_VIEW_WHEELS=0: shared (the old behaviour).
        // Swapped per car around the cars draw's call of 0x177938 (car = a0;
        // jal at 0x17faa0, return 0x17faa8). (A draw can stop at a checkpoint
        // and finish later from the dispatcher, see clipSetupHook: then the
        // swap back waits for the next car's call or the next cars draw, which
        // can only come after it.)
        constexpr uint32_t kWheelScreen = 0x11F8u + 0x66Cu, kWheelScreenSize = 64u;
        using WheelScreen = std::array<uint8_t, kWheelScreenSize>;
        std::map<std::pair<int, uint32_t>, WheelScreen> g_viewWheels; // (view, car) -> positions
        uint32_t g_viewWheelsRace = 0u;                               // the race view they belong to
        int g_viewKey = -1;                                           // the cars draw under way: its view (-1: the race frame's own)
        int g_pendingViewKey = -1;                                    // a car's view positions not yet swapped back
        uint32_t g_pendingCar = 0u;
        WheelScreen g_pendingRaceWheels{};
        bool viewWheelsOn()
        {
            static const bool on = !(std::getenv("GT3_VIEW_WHEELS") && *std::getenv("GT3_VIEW_WHEELS") == '0');
            return on;
        }
        void finishViewWheels(uint8_t *rdram)
        {
            if (g_pendingViewKey < 0)
                return;
            std::memcpy(g_viewWheels[{g_pendingViewKey, g_pendingCar}].data(), rdram + g_pendingCar + kWheelScreen, kWheelScreenSize);
            std::memcpy(rdram + g_pendingCar + kWheelScreen, g_pendingRaceWheels.data(), kWheelScreenSize);
            g_pendingViewKey = -1;
            g_pendingCar = 0u;
        }
        PS2Runtime::RecompiledFunction g_carWheels = nullptr;
        void closeClipLog();
        std::pair<uint32_t, uint32_t> clipLogRange();
        void carWheelsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            finishViewWheels(rdram);
            const uint32_t car = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            const bool fromCarsDraw = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x17faa8u && car >= 0x100000u &&
                                      car < 0x2000000u - 0x2000u;
            // GT3_CAR_CLIP_LOG also marks each car's wheels in the stream: the
            // [car-clip] tests before a marker (after the previous one) are that
            // car's other parts (the cars draw runs a car's parts, then 0x177938).
            if (fromCarsDraw)
                if (const auto range = clipLogRange(); range.second != 0u)
                    if (const uint32_t vb = u32(rdram, kVblankCount); vb >= range.first && vb < range.first + range.second)
                    {
                        closeClipLog();
                        std::fprintf(stderr, "[car-seq] vb %u pass %d car 0x%x\n", vb, g_carsPass, car);
                    }
            if (fromCarsDraw && g_carsPass == 0 && g_carsSeen.size() < 32u &&
                std::find(g_carsSeen.begin(), g_carsSeen.end(), car) == g_carsSeen.end())
                g_carsSeen.push_back(car);
            if (fromCarsDraw && g_viewKey >= 0)
            {
                g_pendingViewKey = g_viewKey;
                g_pendingCar = car;
                std::memcpy(g_pendingRaceWheels.data(), rdram + car + kWheelScreen, kWheelScreenSize);
                // A car this view has not drawn yet starts from the race frame's.
                if (const auto it = g_viewWheels.find({g_viewKey, car}); it != g_viewWheels.end())
                    std::memcpy(rdram + car + kWheelScreen, it->second.data(), kWheelScreenSize);
            }
            g_carWheels(rdram, ctx, runtime);
            if (!callYielded())
                finishViewWheels(rdram);
        }

        void carsDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            finishViewWheels(rdram);
            int pass = 0;
            bool full = false;
            if (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x180638u)
            {
                pass = g_side != 0 ? 2 : 1;
                // A side screen, or GT3's own mirror with GT3_MIRROR_DETAIL=full.
                const uint32_t raceSp = g_side != 0 ? (g_sideFullCars ? g_raceSp : 0u) : (g_mirrorFull && g_mirrorFullCars ? g_mirrorRaceSp : 0u);
                if (raceSp != 0u)
                {
                    ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(raceSp + 0x40u));
                    ctx->r[6] = _mm_set_epi64x(0, -1);
                    ctx->r[7] = _mm_set_epi64x(0, 1);
                    full = true;
                }
            }
            const uint32_t view = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            if (pass == 0 && view != g_viewWheelsRace)
            {
                g_viewWheels.clear(); // a new race: other cars
                g_carsSeen.clear();
                g_viewWheelsRace = view;
            }
            // (g_carsPass / g_viewKey label the draw for the hooks inside it;
            // after a checkpoint the rest of the draw keeps them: they change
            // only here.)
            g_viewKey = full && viewWheelsOn() ? pass * 4 + g_side + 1 : -1;
            g_carsPass = pass;
            carsDiff(rdram, ctx, runtime, pass);
            ++g_yield.carsDraws;
            g_yield.carsDrawsYielded += callYielded() ? 1u : 0u;
        }
        void mirrorBorderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_side != 0)
            {
                ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                return;
            }
            g_mirrorBorder(rdram, ctx, runtime);
        }
        uint32_t vblankNow(const uint8_t *rdram);
        // The side screens are drawn as guest calls, not from here: the mirror
        // render can wait (DMA, semaphores), and the runtime resumes waiting
        // code by unwinding to its run loop, which a C++ caller would break
        // (first try: the race frame then called through a clobbered register).
        // So 0x19AD00's entry points execution at the mirror render with the
        // return address 0x19AD00: when it returns, this hook runs again for
        // the next side, and at last runs 0x19AD00 itself with the race frame's
        // own arguments and return address (callee-saved registers are the
        // race frame's: the mirror render saves and restores those it uses).
        int g_sideStep = 0;            // 0 none; 1 left under way; 2 right under way
        uint32_t g_sideView = 0u;
        __m128i g_sideArgs[4], g_sideRa;
        float g_sideRect[5];
        uint32_t g_raceFrames = 0u, g_sidesFrame = 0u; // race frames counted; the last one that drew the sides

        // The main view drawn into the middle screen's rectangle only (the side
        // views cover the rest), so GT3 culls and draws it for one screen, not
        // three. The race frame's viewport (on its stack, set up by 0x244a30 at
        // 0x17e818) gets a rectangle as the mirror's does (0x244988: +0x18 x,
        // +0x1c y, +0x20 w, +0x24 h, +0x30 on; 0x252bb8 makes it the scissor
        // and the screen transform), and the race camera's x scale (gt3aspect)
        // is times 640 / its width. Only while the sides are drawn (the last
        // race frame drew them), so the whole picture is never left undrawn.
        // GT3_TRIPLE_MIDDLE=full: off (the main view across all three screens).
        bool g_middleRectOn = false;
        float g_mainRectScale = 1.0f;
        PS2Runtime::RecompiledFunction g_viewportSet = nullptr;
        void viewportSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // GT3_SIDE_LOG: the other activations of the race frame's viewport
            // (they get the middle rectangle too), by return address.
            static int viewportLog = std::getenv("GT3_SIDE_LOG") ? 60 : 0;
            static uint32_t raceViewport = 0u;
            const uint32_t vpNow = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            const int32_t middleX0 = static_cast<int32_t>(std::floor(640.0f / (3.0f + 2.0f * g_sideGap)));
            const bool middleRect = u32(rdram, vpNow + 0x30u) == 1u && static_cast<int32_t>(u32(rdram, vpNow + 0x18u)) == middleX0 &&
                                    static_cast<int32_t>(u32(rdram, vpNow + 0x20u)) == 640 - 2 * middleX0;
            if (viewportLog > 0 && (vpNow == raceViewport || middleRect) && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) != 0x17e820u)
            {
                --viewportLog;
                std::fprintf(stderr, "[side-viewport] %s from 0x%x (side %d, rect %u)\n", vpNow == raceViewport ? "race viewport" : "a copy",
                             static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), g_side, u32(rdram, vpNow + 0x30u));
            }
            if (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x17e820u)
            {
                raceViewport = vpNow;
                ++g_raceFrames;
                g_mainRectScale = 1.0f;
                if (g_middleRectOn && g_side == 0 && g_sidesFrame != 0u && g_raceFrames - g_sidesFrame == 1u)
                {
                    const uint32_t vp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
                    const int32_t x0 = static_cast<int32_t>(std::floor(640.0f / (3.0f + 2.0f * g_sideGap)));
                    const int32_t r[4] = {x0, 0, 640 - 2 * x0, 448};
                    const uint32_t on = 1u;
                    std::memcpy(rdram + vp + 0x18u, r, sizeof(r));
                    std::memcpy(rdram + vp + 0x30u, &on, 4);
                    g_mainRectScale = 640.0f / static_cast<float>(r[2]);
                }
            }
            g_viewportSet(rdram, ctx, runtime);
        }

        void startSide(uint8_t *rdram, R5900Context *ctx, int side)
        {
            // Whole pixels, the right one to the picture's edge (the middle
            // rectangle above covers what lies between).
            const float screenW = std::floor(640.0f / (3.0f + 2.0f * g_sideGap));
            const float r[5] = {side < 0 ? 0.0f : 640.0f - screenW, 0.0f, screenW, 448.0f, g_sideRect[4]};
            std::memcpy(rdram + g_sideView + kMirrorRectOffset, r, sizeof(r));
            g_side = side;
            ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(g_sideView));
            ctx->r[5] = ctx->r[29];
            ctx->r[6] = _mm_set_epi64x(0, 0);
            ctx->r[31] = _mm_set_epi64x(0, 0x19ad00);
            ctx->pc = 0x180330u; // the mirror render (kMirrorRender below)
        }
        void afterSceneHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            if (g_sideStep != 0 && ra == 0x19ad00u)
            {
                // A side screen's render returned here.
                if (g_sideStep == 1)
                {
                    g_sideStep = 2;
                    startSide(rdram, ctx, 1);
                    return;
                }
                // Both done: 0x19AD00 as the race frame called it.
                g_side = 0;
                g_sideStep = 0;
                std::memcpy(rdram + g_sideView + kMirrorRectOffset, g_sideRect, sizeof(g_sideRect));
                for (int k = 0; k < 4; ++k)
                    ctx->r[4 + k] = g_sideArgs[k];
                ctx->r[31] = g_sideRa;
                ctx->pc = 0x19ad00u;
                g_afterScene(rdram, ctx, runtime);
                return;
            }
            g_side = 0;
            g_sideStep = 0;
            const uint32_t view = (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) - 0x9830u) & kRamMask;
            if (g_sidesOn && g_mirrorOriginal && g_mainPose.valid && g_mainPose.vblank == vblankNow(rdram) && ra == 0x17ea54u &&
                view > 0x100000u && isRaceView(rdram, u32(rdram, view)) && g_mainPose.camera == view + 0x40u)
            {
                g_sideView = view;
                for (int k = 0; k < 4; ++k)
                    g_sideArgs[k] = ctx->r[4 + k];
                g_sideRa = ctx->r[31];
                std::memcpy(g_sideRect, rdram + view + kMirrorRectOffset, sizeof(g_sideRect));
                g_sidesFrame = g_raceFrames;
                g_raceSp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
                g_sideStep = 1;
                startSide(rdram, ctx, -1);
                return;
            }
            g_afterScene(rdram, ctx, runtime);
        }

        // The game thread's state.
        int g_view = -1;
        uint32_t g_written = 0;   // the view flag as last written
        uint32_t g_lastSeen = 0;  // GT3's VBlank counter at the last race camera call
        uint32_t g_raceCamera = 0u; // the race camera last applied
        float g_look = 0.f;       // the look-around as shown (eased to the target)

        uint32_t vblankNow(const uint8_t *rdram) { return u32(rdram, kVblankCount); }

        int nextView(int from, int step)
        {
            const Settings &s = settings();
            int v = from;
            for (int i = 0; i < ViewCount; ++i)
            {
                v = (v + step + ViewCount) % ViewCount;
                if (s.enabled[v])
                    return v;
            }
            return from;
        }

        void cameraApplyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
            const bool race = (camera & 0xFu) == 0u && camera > 0x100040u && camera < 0x1FF0000u && u32(rdram, camera) == kCameraVtable &&
                              isRaceView(rdram, u32(rdram, camera - 0x40u)) && u32(rdram, camera + kModeIndex) == 0u;
            if (g_side != 0)
            {
                // A side screen: GT3's update, then the main view's pose turned.
                g_cameraApply(rdram, ctx, runtime);
                static int sideLog = std::getenv("GT3_SIDE_LOG") ? 12 : 0;
                if (sideLog > 0)
                {
                    --sideLog;
                    const Pose g = readPose(rdram, camera);
                    std::fprintf(stderr,
                                 "[side-camera] side %d camera 0x%x main 0x%x mode index %u race %d; GT3 back %.3f %.3f %.3f eye %.2f %.2f %.2f; main back %.3f %.3f "
                                 "%.3f eye %.2f %.2f %.2f\n",
                                 g_side, camera, g_mainPose.camera, u32(rdram, camera + kModeIndex), race ? 1 : 0, g.back.x, g.back.y, g.back.z, g.eye.x,
                                 g.eye.y, g.eye.z, g_mainPose.axes[2][0], g_mainPose.axes[2][1], g_mainPose.axes[2][2], g_mainPose.eye[0],
                                 g_mainPose.eye[1], g_mainPose.eye[2]);
                }
                if (g_mainPose.valid && (camera & kRamMask) == g_mainPose.camera) // (the mirror switches the camera's mode: not "race")
                {
                    Pose p;
                    p.right = {g_mainPose.axes[0][0], g_mainPose.axes[0][1], g_mainPose.axes[0][2]};
                    p.up = {g_mainPose.axes[1][0], g_mainPose.axes[1][1], g_mainPose.axes[1][2]};
                    p.back = {g_mainPose.axes[2][0], g_mainPose.axes[2][1], g_mainPose.axes[2][2]};
                    p.eye = {g_mainPose.eye[0], g_mainPose.eye[1], g_mainPose.eye[2]};
                    const float a = static_cast<float>(g_side) * g_sideAngle;
                    p.right = turn(p.right, a);
                    p.up = turn(p.up, a);
                    p.back = turn(p.back, a);
                    writePose(rdram, camera, p, true, nullptr, false);
                }
                return;
            }
            const uint32_t mode = race ? u32(rdram, camera + kModes) & kRamMask : 0u;
            const uint32_t flag = mode > 0x100000u ? u32(rdram, mode + kViewFlag) : 2u;
            // GT3_CAMERA_ALL_LOG=1 (diagnostic): the first 20 cameras seen here
            // that are not the race camera (the mirror's?), with their owners;
            // and each owner class taken as a race view.
            if (race)
            {
                static const bool allLog = std::getenv("GT3_CAMERA_ALL_LOG") != nullptr;
                static std::vector<uint32_t> seenViews;
                const uint32_t vt = u32(rdram, (camera & kRamMask) - 0x40u);
                if (allLog && seenViews.size() < 20u && std::find(seenViews.begin(), seenViews.end(), vt) == seenViews.end())
                {
                    seenViews.push_back(vt);
                    std::fprintf(stderr, "[camera-all] race view: camera 0x%x owner-vtable 0x%x vblank %u\n", camera, vt, u32(rdram, kVblankCount));
                }
            }
            if (!race)
            {
                static const bool allLog = std::getenv("GT3_CAMERA_ALL_LOG") != nullptr;
                static std::vector<uint32_t> seen;
                if (allLog && seen.size() < 20u && std::find(seen.begin(), seen.end(), camera) == seen.end())
                {
                    seen.push_back(camera);
                    const uint32_t c = camera & kRamMask;
                    std::fprintf(stderr, "[camera-all] camera 0x%x vtable 0x%x owner-vtable(-0x40) 0x%x mode index %u ra 0x%x vblank %u\n", camera,
                                 u32(rdram, c), c >= 0x40u ? u32(rdram, c - 0x40u) : 0u, u32(rdram, c + kModeIndex),
                                 static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), u32(rdram, kVblankCount));
                }
            }
            if (!race || flag > 1u)
            {
                g_cameraApply(rdram, ctx, runtime);
                return;
            }
            const Settings &s = settings();
            const uint32_t vblank = u32(rdram, kVblankCount);
            if (g_getterLog)
            {
                static bool modeLogged = false;
                if (!modeLogged)
                {
                    modeLogged = true;
                    const uint32_t vt = u32(rdram, mode) & kRamMask;
                    std::fprintf(stderr, "[camera-getters] camera 0x%x mode 0x%x vtable 0x%x:", camera, mode, vt);
                    for (uint32_t k = 0; k < 0x60u; k += 8u)
                        std::fprintf(stderr, " +%02x %06x", k + 4u, u32(rdram, vt + k + 4u));
                    std::fprintf(stderr, "\n");
                }
                if (vblank % 600u == 0u)
                {
                    static const char *names[3] = {"+24", "+2c", "+34"};
                    for (int k = 0; k < 3; ++k)
                    {
                        std::fprintf(stderr, "[camera-getters] %s:", names[k]);
                        for (const auto &[ra, n] : g_getterCallers[k])
                            std::fprintf(stderr, " %06x x%d", ra, n);
                        std::fprintf(stderr, "\n");
                        g_getterCallers[k].clear();
                    }
                }
            }
            if (g_view < 0 || vblank - g_lastSeen > 30u)
            {
                // A race (re)starts: its start view, or GT3's own.
                g_view = (s.start >= 0 && s.enabled[s.start]) ? s.start : (flag ? Chase : Bumper);
                if (!s.enabled[g_view])
                    g_view = nextView(g_view, 1);
                g_written = flag;
                g_look = 0.f;
            }
            else if (flag != g_written)
            {
                // R1 (GT3's view button) flipped the flag: the next view.
                g_view = nextView(g_view, 1);
                g_announce.store(g_view, std::memory_order_relaxed);
            }
            if (const int steps = g_cycle.exchange(0, std::memory_order_relaxed); steps != 0)
            {
                g_view = nextView(g_view, steps > 0 ? 1 : -1);
                g_announce.store(g_view, std::memory_order_relaxed);
            }
            g_lastSeen = vblank;
            g_raceCamera = camera;
            g_raceSeenMs.store(nowMs(), std::memory_order_relaxed);
            {
                // The model interpreter (0x2250d8) skips a model with a
                // bounding point past the +z guard plane while the word at
                // gp - 0x4f18 is set (the camera inside a car: no body).
                const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
                static bool logged = false;
                if (!logged && logOn())
                {
                    logged = true;
                    std::cerr << std::hex << "[camera] gp 0x" << gp << ", near-skip switch at 0x" << gp - 0x4f18u << " = " << u32(rdram, gp - 0x4f18u)
                              << std::dec << std::endl;
                }
                // GT3_CAMERA_NEAR_SKIP=0 (test): the switch cleared in races.
                static const int nearSkip = [] {
                    const char *v = std::getenv("GT3_CAMERA_NEAR_SKIP");
                    return (v && *v) ? std::atoi(v) : -1;
                }();
                if (nearSkip >= 0)
                    setU32(rdram, gp - 0x4f18u, static_cast<uint32_t>(nearSkip));
            }
            g_written = chaseBased(g_view) ? 1u : 0u;
            setU32(rdram, mode + kViewFlag, g_written);
            g_soundViewMode = (g_view == Bonnet || g_view == Roof) ? mode : 0u;
            GS::setHudView(static_cast<int>(g_written)); // the wide HUD's layout
            gt3aspect::raceCameraCalled(rdram);          // GT3's 16:9 flag for the widening that follows

            restoreGt3Pose(rdram, camera); // GT3 sees its own previous pose (see SavedPose; off by default)
            g_gt3Pose.valid = false;
            g_trackEyeCamera = 0u;
            const bool wroteLast = g_soundEyeCamera != 0u;
            g_soundEyeCamera = 0u;
            if (!wroteLast)
                g_prevGt3EyeCamera = 0u;
            // GT3_CAMERA_TEST_STEPS=<vblank>,<vblank>... (tests): the next view at those vblanks.
            {
                static std::vector<uint32_t> steps = [] {
                    std::vector<uint32_t> out;
                    if (const char *v = std::getenv("GT3_CAMERA_TEST_STEPS"))
                    {
                        std::stringstream list(v);
                        std::string item;
                        while (std::getline(list, item, ','))
                            if (!item.empty())
                                out.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10)));
                    }
                    return out;
                }();
                static uint32_t lastStep = 0u;
                for (const uint32_t at : steps)
                    if (vblank >= at && lastStep < at)
                    {
                        lastStep = at;
                        g_cycle.fetch_add(1, std::memory_order_relaxed);
                        std::fprintf(stderr, "[camera] test step at vblank %u\n", vblank);
                    }
            }
            g_cameraApply(rdram, ctx, runtime);
            ++g_yield.cameraUpdates;
            g_yield.cameraUpdatesYielded += callYielded() ? 1u : 0u;

            if (logOn() && vblank % 300u == 0u)
            {
                const Vec e1 = readVec(rdram, camera + kEye), e2 = readVec(rdram, camera + kEye2), t = readVec(rdram, camera + kTarget);
                const Pose g = readPose(rdram, camera);
                std::fprintf(stderr, "[camera] eye %.2f %.2f %.2f  eye2 %.2f %.2f %.2f  car %.2f %.2f %.2f  back %.3f %.3f %.3f\n", e1.x, e1.y, e1.z, e2.x, e2.y,
                             e2.z, t.x, t.y, t.z, g.back.x, g.back.y, g.back.z);
            }
            // The view.
            Pose p = readPose(rdram, camera);
            // The eye GT3 gets back (its sound's listener): the view's own, but
            // never ahead of the roof view's. With the bonnet view's eye 0.8 m
            // ahead of the car's point the player's engine voice stayed at pitch 0
            // (GT3_SPU_VOICE_LOG, 2026-09-30); roof, close and far were fine.
            Vec listener{};
            bool ownListener = false;
            const Vec target = readVec(rdram, camera + kTarget);
            Vec pivot = p.eye; // the look-around turns the driver's head...
            switch (g_view)
            {
            case CloseChase:
            {
                const Vec away = p.eye - target;
                p.eye = target + Vec{away.x * s.close, away.y + s.closeRaise, away.z * s.close};
                // Tipped down: forward' = forward cos a + up sin a (a < 0).
                const float a = s.closePitch * 3.14159265f / 180.0f;
                const Vec forward = p.back * -1.f;
                const Vec f2 = forward * std::cos(a) + p.up * std::sin(a);
                p.up = p.up * std::cos(a) - forward * std::sin(a);
                p.back = f2 * -1.f;
                pivot = target;
                break;
            }
            case FarChase:
                p.eye = target + (p.eye - target) * s.far;
                [[fallthrough]];
            case Chase:
                pivot = target; // ...or the camera about the car
                break;
            case Bonnet:
            case Roof:
            {
                const float *t = g_view == Bonnet ? s.bonnet : s.roof;
                const float yaw = f32(rdram, camera + kTarget + 12u), pitch = t[2] * 3.14159265f / 180.0f;
                const Vec forward{-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
                const Vec flat{-std::sin(yaw), 0.f, -std::cos(yaw)};
                p.right = {std::cos(yaw), 0.f, -std::sin(yaw)};
                p.up = cross(p.right, forward);
                p.back = forward * -1.f;
                p.eye = target + flat * t[0] + Vec{0.f, t[1], 0.f};
                pivot = p.eye;
                static const bool listenerTest = std::getenv("GT3_CAMERA_LISTENER") != nullptr; // (test; made the pitch tiny)
                if (listenerTest && t[0] > s.roof[0])
                {
                    listener = target + flat * s.roof[0] + Vec{0.f, t[1], 0.f};
                    ownListener = true;
                }
                break;
            }
            default:
                break;
            }
            // The look-around, eased toward the stick's angle.
            float want = g_lookTarget.load(std::memory_order_relaxed);
            float delta = std::remainder(want - g_look, 6.2831853f);
            g_look = std::fabs(delta) < 0.002f ? want : g_look + delta * 0.3f;
            if (std::fabs(g_look) > 0.0005f)
            {
                p.right = turn(p.right, g_look);
                p.up = turn(p.up, g_look);
                p.back = turn(p.back, g_look);
                p.eye = pivot + turn(p.eye - pivot, g_look);
            }
            struct PoseCapture
            {
                uint8_t *rdram;
                uint32_t camera, vblank;
                ~PoseCapture()
                {
                    // The main view's pose as drawn (after any of ours): the side screens turn it.
                    // Only the frame's first camera call (GT3's own mirror calls it again).
                    if (g_mainPose.valid && g_mainPose.vblank == vblank)
                        return;
                    const Pose d = readPose(rdram, camera);
                    const Vec axes[3] = {d.right, d.up, d.back};
                    for (int k = 0; k < 3; ++k)
                    {
                        g_mainPose.axes[k][0] = axes[k].x;
                        g_mainPose.axes[k][1] = axes[k].y;
                        g_mainPose.axes[k][2] = axes[k].z;
                    }
                    g_mainPose.eye[0] = d.eye.x;
                    g_mainPose.eye[1] = d.eye.y;
                    g_mainPose.eye[2] = d.eye.z;
                    g_mainPose.camera = camera & kRamMask;
                    g_mainPose.vblank = vblank;
                    g_mainPose.valid = true;
                }
            } capture{rdram, camera, vblank};
            if ((g_view != Bumper && g_view != Chase) || std::fabs(g_look) > 0.0005f)
            {
                // GT3_CAMERA_KEEP_EYE=1 (test): the eye vector left as GT3 made it.
                static const bool keepEye = [] {
                    const char *v = std::getenv("GT3_CAMERA_KEEP_EYE");
                    return v && *v == '1';
                }();
                saveGt3Pose(rdram, camera);
                static const bool trackEyeOnly = [] {
                    const char *v = std::getenv("GT3_CAMERA_TRACK_EYE");
                    return !(v && *v == '0');
                }();
                static const bool soundEye = [] {
                    const char *v = std::getenv("GT3_CAMERA_SOUND_EYE");
                    return !(v && *v == '0');
                }();
                if (soundEye && g_getter[0] && g_getter[1] && g_getter[2] && !keepEye)
                {
                    // Our eye for the picture, GT3's for its sound (getterHook);
                    // the old eye: GT3's own of the frame before (GT3 copies the
                    // eye there at its update, which would be ours).
                    const Vec gt3Eye = readVec(rdram, camera + kEye);
                    g_soundEye[0] = gt3Eye.x;
                    g_soundEye[1] = gt3Eye.y;
                    g_soundEye[2] = gt3Eye.z;
                    g_soundEyeCamera = camera & kRamMask;
                    if (g_prevGt3EyeCamera == (camera & kRamMask))
                    {
                        setF32(rdram, camera + kEye2, g_prevGt3Eye.x);
                        setF32(rdram, camera + kEye2 + 4u, g_prevGt3Eye.y);
                        setF32(rdram, camera + kEye2 + 8u, g_prevGt3Eye.z);
                    }
                    g_prevGt3Eye = gt3Eye;
                    g_prevGt3EyeCamera = camera & kRamMask;
                    writePose(rdram, camera, p, true, nullptr, false);
                }
                else if (trackEyeOnly && g_trackDraw && !keepEye && std::getenv("GT3_CAMERA_TRACK_EYE"))
                {
                    // The matrix only; the eye for the track draw (trackDrawHook).
                    writePose(rdram, camera, p, false);
                    g_trackEye[0] = p.eye.x;
                    g_trackEye[1] = p.eye.y;
                    g_trackEye[2] = p.eye.z;
                    g_trackEyeCamera = camera & kRamMask;
                }
                else
                    writePose(rdram, camera, p, !keepEye, ownListener ? &listener : nullptr);
            }
        }
    }

    namespace
    {
        void skipHook(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)); }

        // GT3_MODEL_LOG=<vblank> (diagnostics): in that frame, each model the
        // display-list interpreter 0x2250d8(model, index) runs, with how many
        // draws (0x2513e0 clipped / 0x251670 unclipped) it made.
        PS2Runtime::RecompiledFunction g_modelRun = nullptr, g_drawClipped = nullptr, g_drawPlain = nullptr;
        int g_drawsClipped = 0, g_drawsPlain = 0;
        uint32_t modelLogVblank()
        {
            static const uint32_t at = [] {
                const char *v = std::getenv("GT3_MODEL_LOG");
                return (v && *v) ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
            }();
            return at;
        }
        // GT3_CAR_CLIP_LOG=<vblank>,<count> (diagnostic): every object-model
        // chunk test in those VBlanks: the GS tick (GT3_RECORD's frames.csv
        // vsync), cars pass, model (s3), its points, how many lie in front of /
        // behind the camera plane, the frustum planes all points are outside
        // (GT3's AND, for points in front), our choice (0 none needed, 1 the
        // straddle fix, 2 kept: around the camera, 3 no replacement points),
        // what GT3 then did ('c' clipped draw, 'p' plain draw, '-' culled or
        // skipped), the guard-band flags (OR, points in front), GT3's "skip a
        // chunk past +z" switch at gp - 0x4f18, and gt3ChunkOutcome's outcome
        // for GT3's own points with this build's VCLIPW ("predicted") and with
        // the other semantics ("other", see kVclipHardware).
        struct ClipTestLog
        {
            bool open = false;
            uint32_t vblank = 0, tick = 0, model = 0, points = 0, count = 0, front = 0, behind = 0, andFlags = 0, orGuard = 0, skipSwitch = 0;
            int pass = -1, choice = 0, level = 0;
            char outcome = '-', predicted = '?', predictedOther = '?';
        } g_clipLog;
        std::pair<uint32_t, uint32_t> clipLogRange()
        {
            static const std::pair<uint32_t, uint32_t> r = [] {
                std::pair<uint32_t, uint32_t> v{0u, 0u};
                if (const char *e = std::getenv("GT3_CAR_CLIP_LOG"); e && *e)
                    std::sscanf(e, "%u,%u", &v.first, &v.second);
                return v;
            }();
            return r;
        }
        void closeClipLog()
        {
            if (!g_clipLog.open)
                return;
            const ClipTestLog &t = g_clipLog;
            std::fprintf(stderr,
                         "[car-clip] vb %u tick %u pass %d model 0x%x pts 0x%x n %u front %u behind %u and 0x%02x choice %d gt3 %c guard 0x%02x skip %u predicted %c other %c level %d\n",
                         t.vblank, t.tick, t.pass, t.model, t.points, t.count, t.front, t.behind, t.andFlags, t.choice, t.outcome, t.orGuard, t.skipSwitch,
                         t.predicted, t.predictedOther, t.level);
            g_clipLog.open = false;
        }
        void drawClippedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_drawsClipped;
            if (g_clipLog.open && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x2253f8u)
                g_clipLog.outcome = 'c';
            g_drawClipped(rdram, ctx, runtime);
        }
        void drawPlainHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_drawsPlain;
            if (g_clipLog.open && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x2253e8u)
                g_clipLog.outcome = 'p';
            g_drawPlain(rdram, ctx, runtime);
        }
        // The camera inside a car (our bonnet / roof views): the chunk bounding
        // tests in 0x2250d8 (after 0x2486b8) drop the chunks of the player's
        // car that lie across the camera plane - GT3 never draws a car from
        // inside. For car models (run from 0x2250b8) in those views such a
        // chunk gets, for its test only, two points far to the camera's left
        // and right: the test then takes GT3's clipped draw (0x2513e0). The
        // points are put back before the next chunk's test (the interpreter
        // reads them once, right after 0x2486b8). GT3_CAMERA_CAR_CLIP=0: off.
        //
        // History: cars alongside losing their bodies ("when I get close to
        // cars they go invisible", 2026-09-30) and the one-frame flicker of
        // car parts (2026-10-02) were not GT3's: the recompiled VCLIPW had its
        // flag pairs swapped and used the signed w (vu_translator.cpp, fixed
        // there and in the generated code), so GT3's "skip a chunk with a
        // point past +z" rule (the switch at gp - 0x4f18, on in races) fired
        // for every chunk with a point behind the camera. This hook covered
        // that in every view, but its 4 x 4 inverse failed on ~40% of those
        // chunks (see localForClip): the flicker. GT3_CAR_CLIP_LOG shows each
        // test with gt3ChunkOutcome's prediction (100% of 79,826 tests matched
        // GT3's own outcome under the old VCLIPW).
        //
        // Whether a car model is being run is read from the interpreter's own
        // stack frame, not from a flag set around the run: the runtime can stop
        // guest code at any backward branch (a scheduler checkpoint: VSync,
        // DMA and timer events) and finish it later from its dispatcher, after
        // a C++ wrapper has already returned; a flag cleared by the wrapper
        // was then wrong for the rest of that car (rare: 12 of 162,306 tests).
        PS2Runtime::RecompiledFunction g_clipSetup = nullptr;
        bool g_carModelInside = false;  // the old wrapper flag, kept for GT3_HOOK_YIELD_LOG's comparison
        bool g_trackModelRun = false;   // a track model is being drawn (GT3_CAMERA_FOV_LOG)
        struct SavedBytes
        {
            uint32_t address;
            uint8_t bytes[12];   // GT3's point
            uint8_t written[12]; // ours
        };
        std::vector<SavedBytes> g_savedPoints;
        void restoreSavedPoints(uint8_t *rdram)
        {
            // Only where our point is still there (not model data loaded since).
            for (auto it = g_savedPoints.rbegin(); it != g_savedPoints.rend(); ++it)
                if (std::memcmp(rdram + it->address, it->written, 12) == 0)
                    std::memcpy(rdram + it->address, it->bytes, 12);
            g_savedPoints.clear();
        }

        bool invert4(const float m[4][4], float out[4][4])
        {
            float a[4][8];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 8; ++c)
                    a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.f : 0.f);
            for (int c = 0; c < 4; ++c)
            {
                int pivot = c;
                for (int r = c + 1; r < 4; ++r)
                    if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                        pivot = r;
                if (std::fabs(a[pivot][c]) < 1e-12f)
                    return false;
                for (int k = 0; k < 8; ++k)
                    std::swap(a[c][k], a[pivot][k]);
                const float inv = 1.f / a[c][c];
                for (int k = 0; k < 8; ++k)
                    a[c][k] *= inv;
                for (int r = 0; r < 4; ++r)
                    if (r != c)
                    {
                        const float f = a[r][c];
                        for (int k = 0; k < 8; ++k)
                            a[r][k] -= f * a[c][k];
                    }
            }
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    out[r][c] = a[r][c + 4];
            return true;
        }

        // GT3_CAMERA_FOV_LOG=1 (diagnostic): for a track model (placed in the
        // world, so its clip matrix is view x projection), the projection =
        // inverse(view at camera + 0x640) x the clip matrix (vf24..vf27 rows),
        // logged every 5 s with the frustum's tangents (|x| = w at its edges).
        void logProjection(const uint8_t *rdram, const R5900Context *ctx)
        {
            static uint32_t last = 0u;
            const uint32_t now = u32(rdram, kVblankCount);
            if (g_raceCamera == 0u || now - last < 300u)
                return;
            last = now;
            float m[4][4], v[4][4], vi[4][4], p[4][4];
            for (int r = 0; r < 4; ++r)
            {
                std::memcpy(m[r], &ctx->vu0_vf[24 + r], 16);
                std::memcpy(v[r], rdram + g_raceCamera + kMatrix + static_cast<uint32_t>(r * 16), 16);
            }
            for (int r = 0; r < 3; ++r)
                v[r][3] = 0.f;
            v[3][3] = 1.f;
            if (!invert4(v, vi))
                return;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    p[r][c] = vi[r][0] * m[0][c] + vi[r][1] * m[1][c] + vi[r][2] * m[2][c] + vi[r][3] * m[3][c];
            std::fprintf(stderr, "[camera-fov] vblank %u view %d projection", now, g_view);
            for (int r = 0; r < 4; ++r)
                std::fprintf(stderr, " [%.4f %.4f %.4f %.4f]", p[r][0], p[r][1], p[r][2], p[r][3]);
            if (std::fabs(p[0][0]) > 1e-6f && std::fabs(p[1][1]) > 1e-6f)
                std::fprintf(stderr, " tan x %.4f tan y %.4f (%.1f x %.1f degrees)\n", std::fabs(p[2][3] / p[0][0]), std::fabs(p[2][3] / p[1][1]),
                             2.0 * std::atan(std::fabs(p[2][3] / p[0][0])) * 57.29578, 2.0 * std::atan(std::fabs(p[2][3] / p[1][1])) * 57.29578);
            else
                std::fprintf(stderr, "\n");
        }

        // GT3's rear-view mirror render (race view vtable + 0x1b4 = 0x180330,
        // a0 = the race view): its rectangle is 4 floats x, y, w, h at race
        // view + 0xC6F4 (the race frame writes 200, 20, 240, 64 at 0x17ea18
        // just before the call). GT3_MIRROR_RECT=x,y,w,h (diagnostic, the
        // first step toward per-screen views): drawn there instead.
        constexpr uint32_t kMirrorRender = 0x180330u, kMirrorRect = 0xC6F4u;
        PS2Runtime::RecompiledFunction g_mirrorRender = nullptr;
        void mirrorRenderHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool override = std::getenv("GT3_MIRROR_RECT") != nullptr;
            static float rect[4] = {0.f, 0.f, 640.f, 448.f};
            static const bool parsed = [] {
                if (const char *v = std::getenv("GT3_MIRROR_RECT"))
                    std::sscanf(v, "%f,%f,%f,%f", &rect[0], &rect[1], &rect[2], &rect[3]);
                return true;
            }();
            (void)parsed;
            const uint32_t view = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) & kRamMask;
            float saved[4];
            if (override && view > 0x100000u)
            {
                std::memcpy(saved, rdram + view + kMirrorRect, 16);
                static int logged = 0;
                if (logged++ < 3)
                    std::fprintf(stderr, "[mirror] rect %.1f %.1f %.1f %.1f -> %.1f %.1f %.1f %.1f\n", saved[0], saved[1], saved[2], saved[3], rect[0],
                                 rect[1], rect[2], rect[3]);
                std::memcpy(rdram + view + kMirrorRect, rect, 16);
            }
            g_mirrorRender(rdram, ctx, runtime);
            if (override && view > 0x100000u)
                std::memcpy(rdram + view + kMirrorRect, saved, 16);
        }

        // GT3's chunk test (the interpreter at 0x2252f0..0x2253f8), as it runs:
        // per point vf1 = point x vf24..vf27 and vf2 = vf1 * vf20 (the guard
        // band), flags as VCLIPW (+x if x > |w|, -x if x < -|w|, y, z); a point
        // whose w has its sign bit set has each axis' pair turned into
        // (not minus, not plus); the AND of vf1's flags != 0: culled ('x');
        // else with the switch at gp - 0x4f18 set (it is in races) and the OR
        // of vf2's flags holding +z: skipped ('s', GT3's rule for the camera
        // inside a car); else drawn: clipped ('c') when that OR is not 0, else
        // plain ('p').
        // How this build's recompiled VCLIPW sets the flags: false = as the
        // recompiler first emitted it (bit 0 for x < -w, bit 1 for x > w, ...
        // against the signed w), true = as the VU does (bit 0 +x: x > |w|,
        // bit 1 -x: x < -|w|, ...; ps2_vu1_upper.cpp's CLIP, PCSX2).
        constexpr bool kVclipHardware = true;
        uint32_t vuClipFlagsRecompiledOld(const float v[4])
        {
            return (v[0] < -v[3] ? 1u : 0u) | (v[0] > v[3] ? 2u : 0u) | (v[1] < -v[3] ? 4u : 0u) | (v[1] > v[3] ? 8u : 0u) | (v[2] < -v[3] ? 16u : 0u) |
                   (v[2] > v[3] ? 32u : 0u);
        }
        uint32_t vuClipFlags(const float v[4])
        {
            const float aw = std::fabs(v[3]);
            return (v[0] > aw ? 1u : 0u) | (v[0] < -aw ? 2u : 0u) | (v[1] > aw ? 4u : 0u) | (v[1] < -aw ? 8u : 0u) | (v[2] > aw ? 16u : 0u) |
                   (v[2] < -aw ? 32u : 0u);
        }
        uint32_t gt3FixFlags(uint32_t f, float w)
        {
            if (!std::signbit(w))
                return f;
            uint32_t r = 0u;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t plus = (f >> (2 * k)) & 1u, minus = (f >> (2 * k + 1)) & 1u;
                r |= ((minus ^ 1u) << (2 * k)) | ((plus ^ 1u) << (2 * k + 1));
            }
            return r;
        }
        void clipOf(const float p[3], const float m[4][4], float c[4])
        {
            for (int k = 0; k < 4; ++k)
                c[k] = p[0] * m[0][k] + p[1] * m[1][k] + p[2] * m[2][k] + m[3][k];
        }
        char gt3ChunkOutcome(const float (*pts)[3], uint32_t count, const float m[4][4], const float guard[4], bool skipSwitch,
                             bool hardware = kVclipHardware)
        {
            uint32_t andFlags = 0x3Fu, orFlags = 0u;
            for (uint32_t i = 0; i < count; ++i)
            {
                float c[4], g[4];
                clipOf(pts[i], m, c);
                for (int k = 0; k < 4; ++k)
                    g[k] = c[k] * guard[k];
                andFlags &= gt3FixFlags(hardware ? vuClipFlags(c) : vuClipFlagsRecompiledOld(c), c[3]);
                orFlags |= gt3FixFlags(hardware ? vuClipFlags(g) : vuClipFlagsRecompiledOld(g), g[3]);
            }
            if (andFlags != 0u)
                return 'x';
            if (skipSwitch && (orFlags & 0x10u) != 0u)
                return 's';
            return orFlags != 0u ? 'c' : 'p';
        }
        // A local point whose clip x, y, w are the given ones (z follows), from
        // the 3 x 3 system of vf24..vf26's x, y, w columns: the full 4 x 4
        // inverse is useless here, its z and w columns are nearly equal (near
        // 0.1, far ~2000: float inverses ~1e6 with random signs - the first
        // version of this fix failed on ~40% of the chunks it was for).
        bool localForClip(const float m[4][4], float cx, float cy, float cw, float out[3])
        {
            const int sel[3] = {0, 1, 3};
            const float target[3] = {cx, cy, cw};
            float a[3][3], b[3];
            for (int j = 0; j < 3; ++j)
            {
                for (int r = 0; r < 3; ++r)
                    a[j][r] = m[r][sel[j]]; // a x L = b (rows: the clip components)
                b[j] = target[j] - m[3][sel[j]];
            }
            const auto det3 = [](const float q[3][3]) {
                return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
                       q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
            };
            const float d = det3(a);
            if (!std::isfinite(d) || std::fabs(d) < 1e-20f)
                return false;
            for (int k = 0; k < 3; ++k)
            {
                float q[3][3];
                for (int j = 0; j < 3; ++j)
                    for (int r = 0; r < 3; ++r)
                        q[j][r] = r == k ? b[j] : a[j][r];
                out[k] = det3(q) / d;
                if (!std::isfinite(out[k]))
                    return false;
            }
            return true;
        }

        void clipSetupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // The previous chunk's test is done (the interpreter tests one
            // chunk after each call): its points go back.
            restoreSavedPoints(rdram);
            closeClipLog();
            // GT3_CAMERA_CAR_CLIP=0: off; =all: GT3's own views too (not
            // needed since the VCLIPW fix: GT3's test there is the hardware's).
            static const int carClipMode = [] {
                const char *v = std::getenv("GT3_CAMERA_CAR_CLIP");
                if (v && *v == '0')
                    return 0;
                return (v && std::strcmp(v, "all") == 0) ? 2 : 1;
            }();
            const bool carClip = carClipMode != 0;
            // From the interpreter's chunk test (jal 0x2486b8 at 0x2252d8),
            // in an object model run, in a race.
            const bool chunkTest = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x2252e0u;
            const bool carRun = carClip && chunkTest && g_raceCamera != 0u && modelLogVblank() == 0u && objectModelRun(rdram, ctx, runtime);
            // (0x2486b8 and its matrix rebuild 0x24E670 have no backward
            // branches, so no checkpoint stops them: vf24..vf27 are final.)
            g_clipSetup(rdram, ctx, runtime);
            if (g_trackModelRun)
                logProjection(rdram, ctx);
            if (!carRun)
                return;
            ++g_yield.clipTests;
            g_yield.clipFlagWrong += g_carModelInside ? 0u : 1u;
            // At this call: s0 = the point count, s2 = its byte; the points
            // (x, y, z floats) follow; the loop maps them by vf24..vf27 rows.
            const uint32_t count = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0)) & 0xFFu;
            const uint32_t points = (static_cast<uint32_t>(_mm_extract_epi32(ctx->r[18], 0)) + 1u) & kRamMask;
            if (count < 2u || count > 32u)
                return;
            // Not bonnet / roof: chunks around the camera keep GT3's cull.
            const bool keepAroundCamera = g_view != Bonnet && g_view != Roof;
            float m[4][4], guard[4], pts[32][3];
            for (int r = 0; r < 4; ++r)
                std::memcpy(m[r], &ctx->vu0_vf[24 + r], 16);
            std::memcpy(guard, &ctx->vu0_vf[20], 16);
            std::memcpy(pts, rdram + points, count * 12u);
            const bool skipSwitch = u32(rdram, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) - 0x4f18u) != 0u;
            uint32_t nFront = 0u, nBehind = 0u, andFlags = 0x3Fu, orGuard = 0u;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f}; // clip x, y
            for (uint32_t i = 0; i < count; ++i)
            {
                float c[4], g[4];
                clipOf(pts[i], m, c);
                for (int k = 0; k < 4; ++k)
                    g[k] = c[k] * guard[k];
                if (c[3] > 0.f)
                {
                    ++nFront;
                    andFlags &= vuClipFlags(c);
                    orGuard |= vuClipFlags(g);
                }
                else
                    ++nBehind;
                for (int k = 0; k < 2; ++k)
                {
                    lo[k] = std::min(lo[k], c[k]);
                    hi[k] = std::max(hi[k], c[k]);
                }
            }
            const bool front = nFront != 0u, behind = nBehind != 0u;
            const char gt3 = gt3ChunkOutcome(pts, count, m, guard, skipSwitch);
            const uint32_t vblank = u32(rdram, kVblankCount);
            const auto range = clipLogRange();
            if (range.second != 0u && vblank >= range.first && vblank < range.first + range.second)
            {
                ClipTestLog &t = g_clipLog;
                t.open = true;
                t.vblank = vblank;
                t.tick = static_cast<uint32_t>(runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed));
                t.pass = g_carsPass;
                t.model = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[19], 0));
                t.points = points;
                t.count = count;
                t.front = nFront;
                t.behind = nBehind;
                t.andFlags = front ? andFlags : 0u;
                t.orGuard = orGuard;
                t.skipSwitch = skipSwitch ? 1u : 0u;
                t.predicted = gt3;
                t.level = static_cast<int32_t>(_mm_extract_epi32(ctx->r[21], 0)); // s5: the model's level (-1 whole)
                t.predictedOther = gt3ChunkOutcome(pts, count, m, guard, skipSwitch, !kVclipHardware);
                t.choice = 0;
                t.outcome = '-';
            }
            if (gt3 == 'c' || gt3 == 'p')
                return; // GT3 draws it
            // Around the camera (x and y straddle 0 as w does): the player's
            // own car seen from inside.
            const bool aroundCamera = front && behind && lo[0] < 0.f && hi[0] > 0.f && lo[1] < 0.f && hi[1] > 0.f;
            // Worth drawing: in bonnet / roof (the camera inside the player's
            // car, which GT3 never draws from inside) a chunk across the camera
            // plane; in GT3's own views only with GT3_CAMERA_CAR_CLIP=all.
            const bool straddle = front && behind && (!keepAroundCamera || (carClipMode == 2 && !aroundCamera));
            if (!straddle)
            {
                g_clipLog.choice = aroundCamera ? 2 : 0;
                return;
            }
            // Two points far left and right of the picture, ahead of the camera
            // (clip x = -+50 w, y 0): GT3's test then sees a chunk across the
            // picture and takes its clipped draw (0x2513e0). Checked with GT3's
            // own test before use; a few depths are tried.
            float local[2][3];
            bool ok = false;
            for (const float w : {4.f, 8.f, 2.f, 16.f, 1.f})
            {
                if (!localForClip(m, -50.f * w, 0.f, w, local[0]) || !localForClip(m, 50.f * w, 0.f, w, local[1]))
                    continue;
                if (gt3ChunkOutcome(local, 2u, m, guard, skipSwitch) == 'c')
                {
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                g_clipLog.choice = 3;
                return;
            }
            g_clipLog.choice = 1;
            ++g_yield.clipFixed;
            g_yield.clipFixedFlagWrong += g_carModelInside ? 0u : 1u;
            for (uint32_t i = 0; i < count; ++i)
            {
                SavedBytes s;
                s.address = points + i * 12u;
                std::memcpy(s.bytes, rdram + s.address, 12);
                std::memcpy(s.written, local[i & 1u], 12);
                g_savedPoints.push_back(s);
                std::memcpy(rdram + s.address, local[i & 1u], 12);
            }
        }

        // Car detail (user, 2026-09-30: cars "change to a low res or high res
        // model" at 10-20 m). The interpreter 0x2250d8 picks a model's level
        // (0 = most detailed) with 0x224E88(bounding sphere): size = gp[-0x4798]
        // * radius / distance (0x224D88), threshold t = gp[-0x4F08]: level 0
        // if size > t, 1 if > t/2, 2 if > t/4, else 3; then capped to the
        // model's level count (lhu +0x18). Each level is twice the distance, so
        // for object models (cars: run through 0x225090, return 0x2250b8) the
        // level is lowered by g_carDetail (1: full detail twice as far, 2: four
        // times; 3: always full). GT3_CAR_DETAIL=0..3, live car_detail.
        PS2Runtime::RecompiledFunction g_lodSelect = nullptr;
        std::atomic<int> g_carDetail{[] {
            const char *v = std::getenv("GT3_CAR_DETAIL");
            return (v && *v) ? std::clamp(std::atoi(v), 0, 3) : 0;
        }()};
        // The wheels' detail (rims and tyres): the wheel draw 0x2222B8, asked
        // for level -1 (the race frame's cars), picks it with 0x222258 from the
        // wheel's size on screen, 0x2221C8 (only called there): 0 above
        // gp[-0x7060] (0.09), 1 above gp[-0x705C] (0.03), 2 above gp[-0x7058]
        // (0.015), else 3 (the plain wheel 0x222550). The race frame draws car
        // bodies whole (level -1: no 0x224E88 picks for cars in races), so the
        // wheels are what switch as cars come and go (user, 2026-10-02: "the
        // pop in is always cars"). Rim detail (GT3_RIM_DETAIL / live
        // rim_detail; launcher Detail page) scales that size: 1 twice, 2 four
        // times (each rim kept to that many times the distance), 3 always the
        // detailed rim. Unset: as car detail. (0x2221C8 has no backward
        // branches: f0 is final when it returns.)
        PS2Runtime::RecompiledFunction g_wheelSize = nullptr;
        std::atomic<int> g_rimDetail{[] {
            const char *v = std::getenv("GT3_RIM_DETAIL");
            return (v && *v) ? std::clamp(std::atoi(v), 0, 3) : -1;
        }()};
        uint64_t g_wheelLevels[2][4] = {}; // GT3's level for the size, then the drawn one (GT3_HOOK_YIELD_LOG)
        void wheelSizeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_wheelSize(rdram, ctx, runtime);
            const int rim = g_rimDetail.load(std::memory_order_relaxed);
            const int detail = rim >= 0 ? rim : g_carDetail.load(std::memory_order_relaxed);
            const float size = ctx->f[0];
            const float scaled = detail >= 3 ? 1e30f : (detail == 2 ? size * 4.0f : (detail == 1 ? size * 2.0f : size));
            ctx->f[0] = scaled;
            // The level 0x222258 picks from each (its thresholds, gp - 0x7060 / - 0x705C / - 0x7058).
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            const auto levelOf = [&](float s) {
                return s > f32(rdram, gp - 0x7060u) ? 0 : (s > f32(rdram, gp - 0x705Cu) ? 1 : (s > f32(rdram, gp - 0x7058u) ? 2 : 3));
            };
            ++g_wheelLevels[0][levelOf(size)];
            ++g_wheelLevels[1][levelOf(scaled)];
            static uint32_t last = 0u;
            if (const uint32_t now = u32(rdram, kVblankCount); yieldLogOn() && now - last >= 600u)
            {
                last = now;
                const auto &w = g_wheelLevels;
                std::fprintf(stderr, "[car-lod] vblank %u wheel levels 0/1/2/3: GT3 %llu/%llu/%llu/%llu, drawn %llu/%llu/%llu/%llu (rim detail %d)\n", now,
                             static_cast<unsigned long long>(w[0][0]), static_cast<unsigned long long>(w[0][1]), static_cast<unsigned long long>(w[0][2]),
                             static_cast<unsigned long long>(w[0][3]), static_cast<unsigned long long>(w[1][0]), static_cast<unsigned long long>(w[1][1]),
                             static_cast<unsigned long long>(w[1][2]), static_cast<unsigned long long>(w[1][3]), detail);
            }
        }

        bool g_objectModel = false; // the old wrapper flag, kept for GT3_HOOK_YIELD_LOG's comparison
        void lodSelectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // An object model's level (jal 0x224E88 at 0x22514c in the
            // interpreter; its frame read before the call, see clipSetupHook).
            const bool object = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x225154u && objectModelRun(rdram, ctx, runtime);
            g_lodSelect(rdram, ctx, runtime);
            if (object)
            {
                ++g_yield.levelPicks;
                g_yield.levelFlagWrong += g_objectModel ? 0u : 1u;
            }
            const int detail = g_carDetail.load(std::memory_order_relaxed);
            // (0x224E88 is a leaf without loops: v0 is final when it returns.)
            if (!object || detail == 0)
                return;
            const int level = static_cast<int>(_mm_extract_epi32(ctx->r[2], 0));
            const int lowered = detail >= 3 ? 0 : std::max(0, level - detail);
            setReturnS32(ctx, lowered);
        }

        // GT3_TRACK_CULL=<n> (experiment): track sections (the track draw's
        // model runs, return 0x1a9418) whose bounding sphere (model set +0x30
        // + index * 16: x, y, z, r; present when +0x34 is set) lies entirely
        // behind the camera are not run. The position comes from GT3's current
        // clip matrix, vf24..vf27 (valid unless scratchpad 0x70003000's byte 0
        // says it must be rebuilt); n picks the depth lane (2 z, 3 w).
        // GT3_TRACK_CULL_LOG=1: the first sections' spheres and positions.
        uint64_t g_sectionsRun = 0u, g_sectionsCulled = 0u;
        bool trackSectionBehind(uint8_t *rdram, const R5900Context *ctx, PS2Runtime *runtime, uint32_t set, uint32_t index, int lane)
        {
            set &= kRamMask;
            if (set < 0x100000u || u32(rdram, set + 0x34u) == 0u || index >= (u32(rdram, set + 0x10u) & 0xFFFFu))
                return false;
            const uint8_t *scratch = runtime->memory().getScratchpad();
            if (!scratch || scratch[0] != 0u)
                return false;
            const uint32_t sphere = ((u32(rdram, set + 0x30u) & kRamMask) + index * 16u) & kRamMask;
            float c[4];
            std::memcpy(c, rdram + sphere, 16);
            alignas(16) float m[4][4];
            for (int r = 0; r < 4; ++r)
                _mm_store_ps(m[r], ctx->vu0_vf[24 + r]);
            float p[4];
            for (int k = 0; k < 4; ++k)
                p[k] = c[0] * m[0][k] + c[1] * m[1][k] + c[2] * m[2][k] + m[3][k];
            static int logLeft = std::getenv("GT3_TRACK_CULL_LOG") ? 60 : 0;
            if (logLeft > 0)
            {
                --logLeft;
                std::fprintf(stderr, "[track-cull] set 0x%x index %u sphere %.1f %.1f %.1f r %.1f -> %.2f %.2f %.2f %.2f\n", set, index, c[0], c[1], c[2],
                             c[3], p[0], p[1], p[2], p[3]);
            }
            const float depth = p[lane == 3 ? 3 : 2];
            return std::isfinite(depth) && c[3] >= 0.f && depth < -(1.5f * c[3] + 10.f);
        }

        void modelRunHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const int trackCull = [] {
                const char *v = std::getenv("GT3_TRACK_CULL");
                return v && *v ? std::atoi(v) : 0;
            }();
            if (trackCull != 0 && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x1a9418u)
            {
                ++g_sectionsRun;
                if (trackSectionBehind(rdram, ctx, runtime, static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)),
                                       static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)), trackCull))
                {
                    ++g_sectionsCulled;
                    static uint32_t lastReport = 0u;
                    const uint32_t now = u32(rdram, kVblankCount);
                    if (now - lastReport >= 600u)
                    {
                        lastReport = now;
                        std::fprintf(stderr, "[track-cull] %llu of %llu section runs skipped\n", static_cast<unsigned long long>(g_sectionsCulled),
                                     static_cast<unsigned long long>(g_sectionsRun));
                    }
                    ctx->pc = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                    return;
                }
            }
            static const bool carClip = [] {
                const char *v = std::getenv("GT3_CAMERA_CAR_CLIP");
                return !(v && *v == '0');
            }();
            const bool objectModel = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) == 0x2250b8u;
            yieldReport(rdram);
            // The old wrapper flags (decisions now read the interpreter's frame,
            // see clipSetupHook); kept only to count, with GT3_HOOK_YIELD_LOG,
            // how often a checkpoint left them wrong.
            const bool wasObject = g_objectModel;
            g_objectModel = objectModel;
            const bool inside = carClip && g_raceCamera != 0u && objectModel;
            if (inside && modelLogVblank() == 0u)
            {
                const bool wasInside = g_carModelInside;
                g_carModelInside = true;
                g_modelRun(rdram, ctx, runtime);
                const bool stopped = callYielded();
                ++g_yield.objectRuns;
                g_yield.objectRunsYielded += stopped ? 1u : 0u;
                g_carModelInside = wasInside;
                g_objectModel = wasObject;
                // Finished (not stopped at a checkpoint): the last chunk's
                // points go back now, else before the next chunk's test.
                if (!wasInside && !stopped)
                    restoreSavedPoints(rdram);
                return;
            }
            const uint32_t now = u32(rdram, kVblankCount);
            const bool log = now >= modelLogVblank() && now <= modelLogVblank() + 1u;
            const uint32_t model = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
            const uint32_t index = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const int clippedBefore = g_drawsClipped, plainBefore = g_drawsPlain;
            static const bool fovLog = std::getenv("GT3_CAMERA_FOV_LOG") != nullptr;
            g_trackModelRun = fovLog && ra == 0x1a9418u;
            g_modelRun(rdram, ctx, runtime);
            g_trackModelRun = false;
            g_objectModel = wasObject;
            if (log)
                std::cerr << std::hex << "[model] vblank " << std::dec << now << std::hex << " model 0x" << model << " index " << index << " from 0x" << ra
                          << std::dec << ": draws clipped " << g_drawsClipped - clippedBefore << " plain " << g_drawsPlain - plainBefore << std::endl;
        }
    }

    bool raceActive() { return nowMs() - g_raceSeenMs.load(std::memory_order_relaxed) < 300; }

    int sideScreen() { return g_side; }
    float sideScreenWidths() { return 3.0f + 2.0f * g_sideGap; }
    float mainRectScale() { return g_mainRectScale; }

    std::string applyWheelBlur(int on)
    {
        if (on < 0 || on > 1 || (on != 0) == g_wheelBlur.load(std::memory_order_relaxed))
            return {};
        g_wheelBlur.store(on != 0, std::memory_order_relaxed);
        return on ? "Wheel spin blur: GT3's own" : "Wheel spin blur: off";
    }

    std::string applyRimDetail(int detail)
    {
        if (detail < 0 || detail > 3 || detail == g_rimDetail.load(std::memory_order_relaxed))
            return {};
        g_rimDetail.store(detail, std::memory_order_relaxed);
        static const char *names[] = {"GT3's own", "detailed rims twice as far", "detailed rims four times as far", "always detailed rims"};
        return std::string("Rim detail: ") + names[detail];
    }

    std::string applyCarDetail(int detail)
    {
        if (detail < 0 || detail > 3 || detail == g_carDetail.load(std::memory_order_relaxed))
            return {};
        g_carDetail.store(detail, std::memory_order_relaxed);
        static const char *names[] = {"GT3's own", "full detail twice as far", "full detail four times as far", "always full detail"};
        return std::string("Car detail: ") + names[detail];
    }

    std::string applyLive(int views, int start, int look)
    {
        Settings &s = mutableSettings();
        std::string note;
        if (views >= 1 && views <= 0x3F)
        {
            bool changed = false;
            for (int v = 0; v < ViewCount; ++v)
            {
                const bool on = ((views >> v) & 1) != 0;
                changed |= s.enabled[v] != on;
                s.enabled[v] = on;
            }
            if (changed)
            {
                note = "Camera views changed";
                // The view in use was turned off: on to the next one (the game
                // thread takes the step, as for F9).
                const int now = g_view;
                if (now >= 0 && now < ViewCount && !s.enabled[now])
                    g_cycle.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (start >= -1 && start < ViewCount && start != s.start)
        {
            s.start = start;
            note += std::string(note.empty() ? "" : ", ") + "race start view: " + (start < 0 ? "GT3's own" : kViewNames[start]);
        }
        if (look >= 0 && look <= 2 && static_cast<Look>(look) != s.look)
        {
            s.look = static_cast<Look>(look);
            static const char *names[] = {"off", "left-right", "full 360"};
            note += std::string(note.empty() ? "" : ", ") + "look-around: " + names[look];
        }
        return note;
    }

    void install(PS2Runtime &runtime, const uint8_t *rdram)
    {
        if (g_cameraApply)
            return;
        // The model interpreter and its clip setup (bonnet / roof: see
        // clipSetupHook); jal 0x2486b8 at 0x2252d8 and the car caller's
        // return address 0x2250b8 checked.
        if (u32(rdram, 0x2252d8u) == 0x0c0921aeu && u32(rdram, 0x2250b0u) != 0u)
        {
            g_modelRun = runtime.lookupFunction(0x2250d8u);
            g_clipSetup = runtime.lookupFunction(0x2486b8u);
            if (g_modelRun && g_clipSetup)
            {
                runtime.registerFunction(0x2250d8u, &modelRunHook);
                runtime.registerFunction(0x2486b8u, &clipSetupHook);
            }
            // The level choice (jal 0x224E88 at 0x22514c; addiu sp, sp, -0x20 there).
            if (g_modelRun && u32(rdram, 0x22514cu) == 0x0c0893a2u && u32(rdram, 0x224e88u) == 0x27bdffe0u)
            {
                g_lodSelect = runtime.lookupFunction(0x224e88u);
                if (g_lodSelect)
                    runtime.registerFunction(0x224e88u, &lodSelectHook);
            }
            // The wheels' size on screen (jal 0x2221C8 at 0x222260 in 0x222258,
            // its only caller; addiu sp, sp, -0x50 at 0x2221C8).
            if (u32(rdram, 0x222260u) == 0x0c088872u && u32(rdram, 0x2221c8u) == 0x27bdffb0u && (g_wheelSize = runtime.lookupFunction(0x2221c8u)))
                runtime.registerFunction(0x2221c8u, &wheelSizeHook);
        }
        if (modelLogVblank() || clipLogRange().second != 0u)
        {
            g_drawClipped = runtime.lookupFunction(0x2513e0u);
            g_drawPlain = runtime.lookupFunction(0x251670u);
            if (g_drawClipped && g_drawPlain)
            {
                runtime.registerFunction(0x2513e0u, &drawClippedHook);
                runtime.registerFunction(0x251670u, &drawPlainHook);
            }
        }
        // The cars' sound frame (addiu sp, sp, -0x90 at 0x17da58, its call to
        // 0x177a50 at 0x17db1c; see soundFrameHook). GT3_CAMERA_SOUND_FRAME=0: off.
        if (u32(rdram, 0x17da58u) == 0x27bdff70u && u32(rdram, 0x17db1cu) == 0x0c05de94u && !(std::getenv("GT3_CAMERA_SOUND_FRAME") && *std::getenv("GT3_CAMERA_SOUND_FRAME") == '0'))
        {
            if ((g_soundFrame = runtime.lookupFunction(0x17da58u)))
                runtime.registerFunction(0x17da58u, &soundFrameHook);
        }
        // The cars' sound update (addiu sp, sp, -0x510 at 0x177a50; see soundUpdateHook).
        if (u32(rdram, 0x177a50u) == 0x27bdfaf0u && u32(rdram, 0x177a60u) == 0x00a0882du)
        {
            static const bool soundEyeOn = [] {
                const char *v = std::getenv("GT3_CAMERA_SOUND_EYE");
                return !(v && *v == '0');
            }();
            if (soundEyeOn && (g_soundUpdate = runtime.lookupFunction(0x177a50u)))
                runtime.registerFunction(0x177a50u, &soundUpdateHook);
        }
        // The camera's position getters (GT3_CAMERA_GETTER_LOG and the sound, see getterHook).
        g_getterLog = std::getenv("GT3_CAMERA_GETTER_LOG") != nullptr;
        if (u32(rdram, 0x209d58u) == 0x27bdfff0u && u32(rdram, 0x209d10u) == 0x27bdfff0u && u32(rdram, 0x209cc8u) == 0x27bdfff0u &&
            u32(rdram, 0x177cb0u) == 0x0060f809u)
        {
            const uint32_t getters[3] = {0x209d58u, 0x209d10u, 0x209cc8u};
            const PS2Runtime::RecompiledFunction hooks[3] = {&getterHook<0>, &getterHook<1>, &getterHook<2>};
            for (int k = 0; k < 3; ++k)
                if ((g_getter[k] = runtime.lookupFunction(getters[k])))
                    runtime.registerFunction(getters[k], hooks[k]);
        }
        // Angled side screens (see afterSceneHook).
        if (const char *v = std::getenv("GT3_TRIPLE_SIDES"); v && std::strcmp(v, "angled") == 0)
        {
            const char *angle = std::getenv("GT3_TRIPLE_ANGLE");
            g_sideAngle = std::clamp(angle && *angle ? static_cast<float>(std::atof(angle)) : 45.0f, 0.0f, 80.0f) * 3.14159265f / 180.0f;
            const char *gap = std::getenv("GT3_TRIPLE_BEZEL");
            g_sideGap = gap && *gap ? std::clamp(static_cast<float>(std::atof(gap)), 0.0f, 0.5f) : 0.0f;
            // mirror render (addiu sp, sp, -0x120), 0x19AD00's call at 0x17ea4c,
            // 0x209c00 (jr ra; swc1 f12, 0x18(a0)), 0x257EF0, 0x2566F0.
            if (u32(rdram, kMirrorRender) == 0x27bdfee0u && u32(rdram, 0x17ea4cu) == 0x0c066b40u && u32(rdram, 0x209c04u) == 0xe48c0018u)
            {
                g_mirrorOriginal = runtime.lookupFunction(kMirrorRender);
                g_afterScene = runtime.lookupFunction(0x19ad00u);
                g_fovSet = runtime.lookupFunction(0x209c00u);
                g_pixelAspect = runtime.lookupFunction(0x257ef0u);
                g_mirrorBorder = runtime.lookupFunction(0x2566f0u);
                g_mirrorFacing = u32(rdram, 0x251938u) == 0xac4401c8u ? runtime.lookupFunction(0x251930u) : nullptr;
                if (g_mirrorOriginal && g_afterScene && g_fovSet && g_pixelAspect && g_mirrorBorder && g_mirrorFacing)
                {
                    runtime.registerFunction(0x251930u, &mirrorFacingHook);
                    runtime.registerFunction(0x19ad00u, &afterSceneHook);
                    runtime.registerFunction(0x209c00u, &fovSetHook);
                    runtime.registerFunction(0x257ef0u, &pixelAspectHook);
                    runtime.registerFunction(0x2566f0u, &mirrorBorderHook);
                    g_sidesOn = true;
                    // The race frame's viewport call (jal 0x244a30 at 0x17e818;
                    // 0x244a30: addiu sp, sp, -0x20), see viewportSetHook.
                    const char *middle = std::getenv("GT3_TRIPLE_MIDDLE");
                    if (!(middle && std::strcmp(middle, "full") == 0) && u32(rdram, 0x17e818u) == 0x0c09128cu && u32(rdram, 0x244a30u) == 0x27bdffe0u &&
                        (g_viewportSet = runtime.lookupFunction(0x244a30u)))
                    {
                        runtime.registerFunction(0x244a30u, &viewportSetHook);
                        g_middleRectOn = true;
                    }
                    // 0x225C50: jr ra; sw a0, -0x4f0c(gp).
                    const char *detail = std::getenv("GT3_SIDE_DETAIL");
                    // 0x225C50 / 0x225C60: jr ra; sw a0, -0x4f0c(gp) / -0x4f04(gp).
                    const bool mirrorDetail = detail && std::strcmp(detail, "mirror") == 0;
                    if (!mirrorDetail && detail && std::strcmp(detail, "whole") == 0 && u32(rdram, 0x225c50u) == 0x03e00008u &&
                        u32(rdram, 0x225c54u) == 0xaf84b0f4u && (g_modelLevels = runtime.lookupFunction(0x225c50u)))
                    {
                        runtime.registerFunction(0x225c50u, &modelLevelsHook);
                        g_sideFullDetail = true;
                    }
                    else if (!mirrorDetail && u32(rdram, 0x225c60u) == 0x03e00008u && u32(rdram, 0x225c64u) == 0xaf84b0fcu &&
                             (g_forcedLevel = runtime.lookupFunction(0x225c60u)))
                    {
                        runtime.registerFunction(0x225c60u, &forcedLevelHook);
                        g_sideFullDetail = true;
                        g_sideLevelsByDistance = true;
                    }
                    const char *grid = std::getenv("GT3_SIDE_GRID");
                    g_sideMainGrid = grid && std::strcmp(grid, "main") == 0 && u32(rdram, 0x1805ecu) == 0x8c85000cu;
                    // The cars' draw (addiu sp, sp, -0x430 at 0x17f730; the race
                    // view's vtable +0x19c holds it; the mirror's jalr at 0x180630).
                    const char *cars = std::getenv("GT3_SIDE_CARS");
                    const bool mirrorCars = (detail && std::strcmp(detail, "mirror") == 0) || (cars && std::strcmp(cars, "mirror") == 0);
                    if (!mirrorCars && u32(rdram, 0x17f730u) == 0x27bdfbd0u && u32(rdram, kRaceViewVtable + 0x19cu) == 0x17f730u &&
                        u32(rdram, 0x180630u) == 0x0060f809u && (g_carsDraw = runtime.lookupFunction(0x17f730u)))
                    {
                        runtime.registerFunction(0x17f730u, &carsDrawHook);
                        g_sideFullCars = true;
                    }
                    std::cerr << "[camera] angled side screens: " << g_sideAngle * 180.0f / 3.14159265f << " degrees, gap " << g_sideGap
                              << (g_middleRectOn ? ", main view in the middle screen" : "")
                              << (g_sideFullDetail ? (g_modelLevels ? ", whole scenery models" : ", scenery detail as the main view") : "")
                              << (g_sideFullCars ? ", full car detail" : "") << (g_sideMainGrid ? ", main track grid" : "") << std::endl;
                }
            }
            if (!g_sidesOn)
                std::cerr << "[camera] angled side screens not set up (this CORE differs)" << std::endl;
        }
        // GT3's own rear-view mirror at the main view's detail (see
        // forcedLevelHook): the race frame's jalr v0 to the mirror at 0x17ea38;
        // hooks shared with the side screens are looked up once.
        if (const char *v = std::getenv("GT3_MIRROR_DETAIL"); v && std::strcmp(v, "full") == 0)
        {
            if (u32(rdram, kMirrorRender) == 0x27bdfee0u && u32(rdram, 0x17ea38u) == 0x0040f809u && u32(rdram, 0x225c60u) == 0x03e00008u &&
                u32(rdram, 0x225c64u) == 0xaf84b0fcu && u32(rdram, 0x17f730u) == 0x27bdfbd0u && u32(rdram, 0x180630u) == 0x0060f809u)
            {
                if (!g_forcedLevel && (g_forcedLevel = runtime.lookupFunction(0x225c60u)))
                    runtime.registerFunction(0x225c60u, &forcedLevelHook);
                if (!g_carsDraw && (g_carsDraw = runtime.lookupFunction(0x17f730u)))
                    runtime.registerFunction(0x17f730u, &carsDrawHook);
                if ((g_mirrorEntry = runtime.lookupFunction(kMirrorRender)))
                    runtime.registerFunction(kMirrorRender, &mirrorEntryHook);
                g_mirrorFull = g_forcedLevel && g_carsDraw && g_mirrorEntry;
                if (const char *parts = std::getenv("GT3_MIRROR_DETAIL_PARTS"); parts && *parts)
                {
                    g_mirrorFullScenery = (std::atoi(parts) & 1) != 0;
                    g_mirrorFullCars = (std::atoi(parts) & 2) != 0;
                }
            }
            std::cerr << "[camera] rear-view mirror detail: " << (g_mirrorFull ? "as the main view" : "GT3's (this CORE differs)") << std::endl;
        }
        // Diagnostics: GT3_CARS_DIFF needs the cars draw hooked even without
        // the mirror / side options; GT3_WHEEL_LOG the wheel draw
        // (0x2222B8: addiu sp, sp, -0xA0).
        if (std::getenv("GT3_CARS_DIFF") && !g_carsDraw && u32(rdram, 0x17f730u) == 0x27bdfbd0u && (g_carsDraw = runtime.lookupFunction(0x17f730u)))
            runtime.registerFunction(0x17f730u, &carsDrawHook);
        // (Always: wheel spin blur can be switched off live.)
        if (u32(rdram, 0x2222b8u) == 0x27bdff60u && (g_wheelDraw = runtime.lookupFunction(0x2222b8u)))
            runtime.registerFunction(0x2222b8u, &wheelDrawHook);
        // Each extra view's own wheel positions (see carWheelsHook): 0x177938
        // (addiu sp, sp, -0x10) and the cars draw's jal 0x177938 at 0x17faa0.
        if (g_carsDraw && u32(rdram, 0x177938u) == 0x27bdfff0u && u32(rdram, 0x17faa0u) == 0x0c05de4eu && (g_carWheels = runtime.lookupFunction(0x177938u)))
            runtime.registerFunction(0x177938u, &carWheelsHook);
        // The track draw (addiu sp, sp, -0x60 at 0x1a92e8; see trackDrawHook).
        if (u32(rdram, 0x1a92e8u) == 0x27bdffa0u && u32(rdram, 0x1a9384u) == 0x0c06a4ecu)
        {
            g_trackDraw = runtime.lookupFunction(0x1a92e8u);
            if (g_trackDraw)
                runtime.registerFunction(0x1a92e8u, &trackDrawHook);
        }
        // GT3_MIRROR_RECT (diagnostic, see mirrorRenderHook): the mirror render
        // (addiu sp, sp, -0x120 at 0x180330) wrapped.
        if (std::getenv("GT3_MIRROR_RECT") && u32(rdram, kMirrorRender) == 0x27bdfee0u)
        {
            g_mirrorRender = runtime.lookupFunction(kMirrorRender);
            if (g_mirrorRender)
                runtime.registerFunction(kMirrorRender, &mirrorRenderHook);
        }
        // GT3_SKIP_FUNCS=<hex>,<hex>... (diagnostics): those functions return
        // at once (finding what draws what).
        if (const char *v = std::getenv("GT3_SKIP_FUNCS"); v && *v)
        {
            std::stringstream list(v);
            std::string item;
            while (std::getline(list, item, ','))
                if (const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)); address)
                {
                    runtime.registerFunction(address, &skipHook);
                    std::cerr << "[camera] skipping 0x" << std::hex << address << std::dec << std::endl;
                }
        }
        // lw $v1, 0x14($v0) / jalr $v1 at 0x17e870 (the camera call in the race
        // frame), the camera's start, and the view flag's load in GT3's code
        // is not checked (data): the vtable addresses are, per call.
        if (u32(rdram, 0x17e870u) != 0x8c430014u || u32(rdram, kCameraApply) != 0x27bdfff0u || u32(rdram, 0x209e70u) != 0x8c4316d8u)
        {
            std::cerr << "[camera] not installed: this CORE differs at 0x17e870 / 0x209e58" << std::endl;
            return;
        }
        g_cameraApply = runtime.lookupFunction(kCameraApply);
        if (g_cameraApply)
            runtime.registerFunction(kCameraApply, &cameraApplyHook);
    }

    void hostUpdate(std::string &note)
    {
        const Settings &s = settings();
        if (IsKeyPressed(KEY_F9))
            g_cycle.fetch_add((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? -1 : 1, std::memory_order_relaxed);
        if (const int v = g_announce.exchange(-1, std::memory_order_relaxed); v >= 0 && v < ViewCount)
            note = std::string("Camera: ") + kViewNames[v] + "   (R1 / F9)";
        // The right stick: claimed (kept from the game) in races in full mode,
        // and in any mode with the pedals on the triggers (GT3_PEDALS=triggers:
        // GT3's pedals are its right stick then, so the physical stick must
        // not reach it; the triggers still do).
        const bool racing = nowMs() - g_raceSeenMs.load(std::memory_order_relaxed) < 500;
        ps2_pad::setRightStickClaimed(racing && (s.look == Look::Full || ps2_pad::pedalsOnTriggers()));
        float x = 0.f, y = 0.f;
        ps2_pad::rightStick(x, y);
        float target = 0.f;
        if (s.look == Look::Sideways && std::fabs(x) > 0.2f)
            target = std::copysign((std::fabs(x) - 0.2f) / 0.8f * 2.6f, x); // up to ~150 degrees
        else if (s.look == Look::Full && std::sqrt(x * x + y * y) > 0.35f)
            target = std::atan2(x, -y); // up: ahead, right: right, down: behind
        // GT3_CAMERA_LOOK_TEST=<degrees> (tests): a fixed look-around.
        static const float testLook = [] {
            const char *v = std::getenv("GT3_CAMERA_LOOK_TEST");
            return (v && *v) ? static_cast<float>(std::atof(v)) * 3.14159265f / 180.0f : 0.f;
        }();
        if (testLook != 0.f)
            target = testLook;
        g_lookTarget.store(racing ? target : 0.f, std::memory_order_relaxed);
    }
}
