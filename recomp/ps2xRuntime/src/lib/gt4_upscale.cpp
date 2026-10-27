#include "gt4_upscale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32) && defined(GT4_HAVE_STREAMLINE) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include <windows.h>

#include <volk.h>

#include <sl.h>
#include <sl_dlss.h>
#include <sl_security.h>

#include <string>

namespace gt4dlss
{
    using gt4up::Frame;
    using gt4up::Image;
    namespace
    {
        HMODULE g_sl = nullptr;
        bool g_tried = false, g_started = false, g_failedEval = false;
        PFN_vkGetInstanceProcAddr g_gipa = nullptr;
        decltype(&slInit) pInit = nullptr;
        decltype(&slSetTagForFrame) pSetTag = nullptr;
        decltype(&slSetConstants) pSetConstants = nullptr;
        decltype(&slEvaluateFeature) pEvaluate = nullptr;
        decltype(&slGetFeatureFunction) pFeatureFunction = nullptr;
        decltype(&slGetNewFrameToken) pNewFrame = nullptr;
        decltype(&slIsFeatureLoaded) pLoaded = nullptr;
        PFun_slDLSSSetOptions *pSetOptions = nullptr;
        uint32_t g_frameIndex = 0;

        std::wstring exeDir()
        {
            wchar_t path[MAX_PATH * 4];
            const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
            std::wstring s(path, n);
            const size_t slash = s.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring(L".") : s.substr(0, slash);
        }

        void logMessage(sl::LogType type, const char *msg)
        {
            // (Streamline's own lines; warnings and errors only, they end in a new line)
            if (type != sl::LogType::eInfo)
                std::fprintf(stderr, "[dlss] %s", msg);
        }

        template <class T>
        bool resolve(T &fn, const char *name)
        {
            fn = reinterpret_cast<T>(GetProcAddress(g_sl, name));
            if (!fn)
                std::fprintf(stderr, "[dlss] sl.interposer.dll has no %s: DLSS off\n", name);
            return fn != nullptr;
        }

        bool askedFor()
        {
            const char *v = std::getenv("GT4_UPSCALER");
            return v && std::strcmp(v, "dlss") == 0;
        }

        sl::DLSSMode modeFor(float ratio)
        {
            if (ratio < 1.25f)
                return sl::DLSSMode::eDLAA;
            if (ratio < 1.6f)
                return sl::DLSSMode::eMaxQuality;
            if (ratio < 1.86f)
                return sl::DLSSMode::eBalanced;
            if (ratio < 2.5f)
                return sl::DLSSMode::eMaxPerformance;
            return sl::DLSSMode::eUltraPerformance;
        }

        const char *modeName(sl::DLSSMode m)
        {
            switch (m)
            {
            case sl::DLSSMode::eDLAA: return "DLAA";
            case sl::DLSSMode::eMaxQuality: return "quality";
            case sl::DLSSMode::eBalanced: return "balanced";
            case sl::DLSSMode::eMaxPerformance: return "performance";
            case sl::DLSSMode::eUltraPerformance: return "ultra performance";
            default: return "?";
            }
        }

        sl::Resource resource(const Image &i)
        {
            sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void *>(i.image), nullptr, reinterpret_cast<void *>(i.view),
                           static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
            r.width = static_cast<uint32_t>(i.width);
            r.height = static_cast<uint32_t>(i.height);
            r.nativeFormat = i.format;
            r.mipLevels = 1;
            r.arrayLayers = 1;
            r.usage = i.usage;
            return r;
        }

        sl::Extent extent(const Image &i)
        {
            sl::Extent e{};
            e.top = static_cast<uint32_t>(i.y);
            e.left = static_cast<uint32_t>(i.x);
            e.width = static_cast<uint32_t>(i.w);
            e.height = static_cast<uint32_t>(i.h);
            return e;
        }

        sl::float4x4 identity()
        {
            sl::float4x4 m;
            m[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
            m[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
            m[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
            m[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
            return m;
        }
    } // namespace

    void *instanceProcAddr()
    {
        if (g_tried)
            return reinterpret_cast<void *>(g_gipa);
        g_tried = true;
        if (!askedFor())
            return nullptr;
        const std::wstring dir = exeDir();
        const std::wstring path = dir + L"\\sl.interposer.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::fprintf(stderr, "[dlss] GT4_UPSCALER=dlss but sl.interposer.dll is not beside GT4.exe: DLSS off\n");
            return nullptr;
        }
        if (!sl::security::verifyEmbeddedSignature(path.c_str()))
        {
            std::fprintf(stderr, "[dlss] sl.interposer.dll is not signed by NVIDIA: not loaded, DLSS off\n");
            return nullptr;
        }
        g_sl = LoadLibraryW(path.c_str());
        if (!g_sl)
        {
            std::fprintf(stderr, "[dlss] sl.interposer.dll did not load (error %lu): DLSS off\n", GetLastError());
            return nullptr;
        }
        if (!resolve(pInit, "slInit") || !resolve(pSetTag, "slSetTagForFrame") || !resolve(pSetConstants, "slSetConstants") ||
            !resolve(pEvaluate, "slEvaluateFeature") || !resolve(pFeatureFunction, "slGetFeatureFunction") ||
            !resolve(pNewFrame, "slGetNewFrameToken") || !resolve(pLoaded, "slIsFeatureLoaded") || !resolve(g_gipa, "vkGetInstanceProcAddr"))
        {
            g_gipa = nullptr;
            return nullptr;
        }
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eDefault;
        const wchar_t *paths[] = {dir.c_str()};
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        pref.pathToLogsAndData = nullptr;
        pref.logMessageCallback = logMessage;
        // (no over-the-air updates or downloaded plugins: only the DLLs shipped beside the game)
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        static const sl::Feature features[] = {sl::kFeatureDLSS};
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 1;
        pref.engineVersion = "GT4 recomp 1";
        pref.projectId = "5c1e0d3a-8f4b-4b27-9e61-2d7a4f9c0b18";
        pref.renderAPI = sl::RenderAPI::eVulkan;
        const sl::Result r = pInit(pref, sl::kSDKVersion);
        if (r != sl::Result::eOk)
        {
            std::fprintf(stderr, "[dlss] Streamline did not start (result %d): DLSS off\n", static_cast<int>(r));
            g_gipa = nullptr;
            return nullptr;
        }
        g_started = true;
        std::fprintf(stderr, "[dlss] Streamline started (manual hooking, no downloads); Vulkan through its wrappers\n");
        return reinterpret_cast<void *>(g_gipa);
    }

    bool wanted() { return g_started; }
    bool usable() { return g_started && !g_failedEval; }

    int outputScale(int renderScale)
    {
        if (!g_started || g_failedEval)
            return 0;
        const char *v = std::getenv("GT4_DLSS_OUT");
        const int out = v && *v ? std::atoi(v) : renderScale * 2;
        return out >= renderScale && out <= 8 ? out : 0;
    }

    bool scales(int requested, int &draw, int &out)
    {
        draw = requested;
        out = 0;
        if (!g_started || g_failedEval)
            return false;
        // GT4_DLSS_OUT (tests): GT3_GS_GL_SCALE is the drawing's scale and this the picture's.
        if (const char *v = std::getenv("GT4_DLSS_OUT"); v && *v)
        {
            const int o = std::atoi(v);
            if (o < requested || o > 8)
                return false;
            out = o;
            return true;
        }
        // The launcher: GT3_GS_GL_SCALE is the picture's scale; DLSS draws at GT4_DLSS_DRAW, or by default at half of 4x
        // (performance) and one less below it (2x from 3x: quality; native from 2x: performance). Native: nothing to do.
        if (requested < 2)
            return false;
        int d = 0;
        if (const char *v = std::getenv("GT4_DLSS_DRAW"); v && *v)
            d = std::atoi(v);
        if (d < 1 || d > requested)
            d = requested >= 4 ? requested / 2 : requested - 1;
        draw = d;
        out = requested;
        return true;
    }

    bool evaluate(void *cmd, const Frame &f)
    {
        if (!g_started || g_failedEval)
            return false;
        if (!pSetOptions)
        {
            bool loaded = false;
            void *fn = nullptr;
            if (pLoaded(sl::kFeatureDLSS, loaded) != sl::Result::eOk || !loaded ||
                pFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", fn) != sl::Result::eOk || !fn)
            {
                std::fprintf(stderr, "[dlss] DLSS is not loaded (an RTX card and sl.dlss.dll / nvngx_dlss.dll beside GT4.exe are needed): off\n");
                g_failedEval = true;
                return false;
            }
            pSetOptions = reinterpret_cast<PFun_slDLSSSetOptions *>(fn);
        }
        sl::FrameToken *token = nullptr;
        const uint32_t index = g_frameIndex++;
        if (pNewFrame(token, &index) != sl::Result::eOk || !token)
            return false;
        const sl::ViewportHandle viewport(0);

        sl::Constants c{};
        c.cameraViewToClip = identity();
        c.clipToCameraView = identity();
        c.clipToLensClip = identity();
        c.clipToPrevClip = identity();
        c.prevClipToClip = identity();
        c.jitterOffset = sl::float2(f.jitterX, f.jitterY);
        c.mvecScale = sl::float2(f.motionScaleX, f.motionScaleY);
        c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        c.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
        c.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
        c.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
        c.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
        c.cameraNear = 1.0f;
        c.cameraFar = 100000.0f;
        c.cameraFOV = 1.0f;
        c.cameraAspectRatio = f.color.h > 0 ? static_cast<float>(f.color.w) / static_cast<float>(f.color.h) : 1.0f;
        c.depthInverted = sl::Boolean::eTrue;      // (GS Z: nearer is higher)
        c.cameraMotionIncluded = sl::Boolean::eTrue; // (the motion is each vertex's whole move)
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        if (pSetConstants(c, *token, viewport) != sl::Result::eOk)
            return false;

        sl::DLSSOptions o{};
        const float ratio = f.color.w > 0 ? static_cast<float>(f.out.w) / static_cast<float>(f.color.w) : 1.0f;
        o.mode = modeFor(ratio);
        o.outputWidth = static_cast<uint32_t>(f.out.w);
        o.outputHeight = static_cast<uint32_t>(f.out.h);
        o.colorBuffersHDR = sl::Boolean::eFalse;
        static sl::DLSSMode logged = sl::DLSSMode::eCount;
        if (o.mode != logged)
        {
            logged = o.mode;
            std::fprintf(stderr, "[dlss] %dx%d -> %dx%d (%s)\n", f.color.w, f.color.h, f.out.w, f.out.h, modeName(o.mode));
        }
        if (pSetOptions(viewport, o) != sl::Result::eOk)
        {
            std::fprintf(stderr, "[dlss] options refused: off\n");
            g_failedEval = true;
            return false;
        }

        sl::Resource color = resource(f.color), depth = resource(f.depth), motion = resource(f.motion), out = resource(f.out);
        const sl::Extent ec = extent(f.color), ed = extent(f.depth), em = extent(f.motion), eo = extent(f.out);
        sl::ResourceTag tags[] = {
            sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &ec),
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &ed),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &em),
            sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &eo),
        };
        sl::CommandBuffer *cb = reinterpret_cast<sl::CommandBuffer *>(cmd);
        if (pSetTag(*token, viewport, tags, 4, cb) != sl::Result::eOk)
        {
            static int failures = 0;
            if (failures++ < 8)
                std::fprintf(stderr, "[dlss] tagging failed\n");
            return false;
        }
        const sl::BaseStructure *inputs[] = {&viewport};
        const sl::Result r = pEvaluate(sl::kFeatureDLSS, *token, inputs, 1, cb);
        if (r != sl::Result::eOk)
        {
            static int failures = 0;
            if (failures++ < 8)
                std::fprintf(stderr, "[dlss] evaluate failed (result %d)\n", static_cast<int>(r));
            return false;
        }
        return true;
    }
} // namespace gt4dlss

#else

namespace gt4dlss
{
    using gt4up::Frame;
    using gt4up::Image;
    void *instanceProcAddr()
    {
        const char *v = std::getenv("GT4_UPSCALER");
        if (v && std::strcmp(v, "dlss") == 0)
            std::fprintf(stderr, "[dlss] GT4_UPSCALER=dlss: this build has no DLSS (Streamline SDK not found when built)\n");
        return nullptr;
    }
    bool wanted() { return false; }
    bool usable() { return false; }
    int outputScale(int) { return 0; }
    bool scales(int requested, int &draw, int &out)
    {
        draw = requested;
        out = 0;
        return false;
    }
    bool evaluate(void *, const Frame &) { return false; }
} // namespace gt4dlss

#endif

// GT4_UPSCALER picks the upscaler: dlss (above), fsr (FSR 3.1 on Vulkan, gt4_upscale_fsr.cpp) or fsr4 (AMD's newest
// through Direct3D 12, gt4_upscale_fsr4.cpp); the renderer sees one.
namespace gt4fsr
{
    bool asked();
    bool start();
    bool usable();
    bool evaluate(void *cmd, const gt4up::Frame &f);
}
namespace gt4fsr4
{
    bool asked();
    bool start();
    bool usable();
    bool evaluate(void *cmd, const gt4up::Frame &f);
}

namespace gt4up
{
    namespace
    {
        bool amd() { return gt4fsr::asked() || gt4fsr4::asked(); }
        bool amdUsable() { return gt4fsr4::asked() ? gt4fsr4::usable() : gt4fsr::usable(); }
    }
    void *instanceProcAddr() { return amd() ? nullptr : gt4dlss::instanceProcAddr(); }
    bool wanted() { return gt4fsr4::asked() ? gt4fsr4::start() : gt4fsr::asked() ? gt4fsr::start() : gt4dlss::wanted(); }
    bool usable() { return amd() ? amdUsable() : gt4dlss::usable(); }

    int outputScale(int renderScale)
    {
        if (!amd())
            return gt4dlss::outputScale(renderScale);
        if (!amdUsable())
            return 0;
        const char *v = std::getenv("GT4_DLSS_OUT");
        const int out = v && *v ? std::atoi(v) : renderScale * 2;
        return out >= renderScale && out <= 8 ? out : 0;
    }

    bool scales(int requested, int &draw, int &out)
    {
        if (!amd())
            return gt4dlss::scales(requested, draw, out);
        // (as DLSS: GT4_DLSS_OUT for tests; else the picture at GT3_GS_GL_SCALE, drawn at GT4_DLSS_DRAW or half of 4x / one less)
        draw = requested;
        out = 0;
        if (!amdUsable())
            return false;
        if (const char *v = std::getenv("GT4_DLSS_OUT"); v && *v)
        {
            const int o = std::atoi(v);
            if (o < requested || o > 8)
                return false;
            out = o;
            return true;
        }
        if (requested < 2)
            return false;
        int d = 0;
        if (const char *v = std::getenv("GT4_DLSS_DRAW"); v && *v)
            d = std::atoi(v);
        if (d < 1 || d > requested)
            d = requested >= 4 ? requested / 2 : requested - 1;
        draw = d;
        out = requested;
        return true;
    }

    bool evaluate(void *cmd, const Frame &f)
    {
        return gt4fsr4::asked() ? gt4fsr4::evaluate(cmd, f) : gt4fsr::asked() ? gt4fsr::evaluate(cmd, f) : gt4dlss::evaluate(cmd, f);
    }
} // namespace gt4up
