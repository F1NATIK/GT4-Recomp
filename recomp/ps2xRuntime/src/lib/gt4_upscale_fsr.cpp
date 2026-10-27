// GT4_UPSCALER=fsr: AMD FidelityFX Super Resolution 3.1 on the Vulkan renderer (the upscaler queue's FSR step; developer
// 2026-10-14: "FSR after Tourist Trophy"). The same inputs as DLSS (gt4_upscale.h: the drawing, GS Z, the VU1 tap's
// motion, the jitter) through AMD's FidelityFX API, amd_fidelityfx_vk.dll beside GT4.exe (AMD's signed prebuilt, from
// the FidelityFX SDK 1.1.4; sdk\ffx-1.1.4, not in git). It runs on any Vulkan card (AMD, NVIDIA, Intel). Nothing is
// downloaded. Off (unset): nothing is loaded.
#include "gt4_upscale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4fsr
{
    bool asked()
    {
        static const bool a = [] {
            const char *v = std::getenv("GT4_UPSCALER");
            return v && std::strcmp(v, "fsr") == 0;
        }();
        return a;
    }
} // namespace gt4fsr

#if defined(_WIN32) && defined(GT4_HAVE_FFX) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include <windows.h>

#include "gs/gs_vk_device.h"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include <algorithm>
#include <string>

namespace gt4fsr
{
    namespace
    {
        HMODULE g_dll = nullptr;
        bool g_tried = false, g_failed = false;
        PfnFfxCreateContext pCreate = nullptr;
        PfnFfxDestroyContext pDestroy = nullptr;
        PfnFfxDispatch pDispatch = nullptr;
        PfnFfxQuery pQuery = nullptr;
        ffxContext g_ctx = nullptr;
        int g_maxW = 0, g_maxH = 0, g_outW = 0, g_outH = 0;
        uint64_t g_frames = 0;

        std::wstring exeDir()
        {
            wchar_t path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring p(path, n);
            const size_t slash = p.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
        }

        void message(uint32_t type, const wchar_t *text)
        {
            static int shown = 0;
            if (shown++ < 32)
                std::fprintf(stderr, "[fsr] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
        }

        // FidelityFX asks for some calls by their extension names (vkGetBufferMemoryRequirements2KHR) and calls them
        // unchecked; the renderer's Vulkan 1.3 device has them as core calls, without the extension: those given.
        PFN_vkVoidFunction VKAPI_PTR deviceProc(VkDevice device, const char *name)
        {
            if (PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name))
                return f;
            const size_t n = std::strlen(name);
            if (n > 3 && std::strcmp(name + n - 3, "KHR") == 0)
                return vkGetDeviceProcAddr(device, std::string(name, n - 3).c_str());
            return nullptr;
        }

        FfxApiResource resource(const gt4up::Image &img, uint32_t state)
        {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = static_cast<VkFormat>(img.format);
            info.extent = {static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1u};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = static_cast<VkImageUsageFlags>(img.usage);
            return ffxApiGetResourceVK(reinterpret_cast<void *>(static_cast<uintptr_t>(img.image)),
                                       ffxApiGetImageResourceDescriptionVK(reinterpret_cast<VkImage>(static_cast<uintptr_t>(img.image)), info, 0u), state);
        }

        bool makeContext(int maxW, int maxH, int outW, int outH)
        {
            if (g_ctx)
            {
                pDestroy(&g_ctx, nullptr);
                g_ctx = nullptr;
            }
            const gsvk::Device *dev = gsvk::sharedDevice();
            if (!dev || !dev->device)
                return false;
            ffxCreateBackendVKDesc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
            backend.vkDevice = dev->device;
            backend.vkPhysicalDevice = dev->physical;
            backend.vkDeviceProcAddr = &deviceProc;
            ffxCreateContextDescUpscale desc{};
            desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            desc.header.pNext = &backend.header;
            // (GS Z: nearer is higher; GT4's colours are as shown, not linear light)
            desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
            desc.maxRenderSize = {static_cast<uint32_t>(maxW), static_cast<uint32_t>(maxH)};
            desc.maxUpscaleSize = {static_cast<uint32_t>(outW), static_cast<uint32_t>(outH)};
            desc.fpMessage = &message;
            const ffxReturnCode_t r = pCreate(&g_ctx, &desc.header, nullptr);
            if (r != FFX_API_RETURN_OK || !g_ctx)
            {
                std::fprintf(stderr, "[fsr] context not made (result %u): FSR off\n", r);
                g_ctx = nullptr;
                return false;
            }
            g_maxW = maxW;
            g_maxH = maxH;
            g_outW = outW;
            g_outH = outH;
            std::fprintf(stderr, "[fsr] FSR 3.1 context: up to %dx%d -> %dx%d\n", maxW, maxH, outW, outH);
            return true;
        }
    } // namespace

    bool start()
    {
        if (g_tried)
            return g_dll != nullptr && !g_failed;
        g_tried = true;
        if (!asked())
            return false;
        const std::wstring path = exeDir() + L"\\amd_fidelityfx_vk.dll";
        g_dll = LoadLibraryW(path.c_str());
        if (!g_dll)
        {
            std::fprintf(stderr, "[fsr] GT4_UPSCALER=fsr but amd_fidelityfx_vk.dll did not load from beside GT4.exe (error %lu): FSR off\n",
                         GetLastError());
            return false;
        }
        pCreate = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_dll, "ffxCreateContext"));
        pDestroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_dll, "ffxDestroyContext"));
        pDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_dll, "ffxDispatch"));
        pQuery = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_dll, "ffxQuery"));
        if (!pCreate || !pDestroy || !pDispatch)
        {
            std::fprintf(stderr, "[fsr] amd_fidelityfx_vk.dll lacks the FidelityFX API: FSR off\n");
            FreeLibrary(g_dll);
            g_dll = nullptr;
            return false;
        }
        std::fprintf(stderr, "[fsr] AMD FidelityFX API loaded (amd_fidelityfx_vk.dll): FSR 3.1 upscaling\n");
        return true;
    }

    bool usable() { return start() && !g_failed; }

    bool evaluate(void *cmd, const gt4up::Frame &f)
    {
        if (!usable())
            return false;
        // (FidelityFX reads its inputs from the image's corner: a frame whose parts are elsewhere is left to the plain path)
        if (f.color.x || f.color.y || f.depth.x || f.depth.y || f.motion.x || f.motion.y || f.out.x || f.out.y)
        {
            static int logged = 0;
            if (logged++ < 4)
                std::fprintf(stderr, "[fsr] a frame with offset parts (%d,%d / %d,%d / %d,%d -> %d,%d): not upscaled\n", f.color.x, f.color.y, f.depth.x,
                             f.depth.y, f.motion.x, f.motion.y, f.out.x, f.out.y);
            return false;
        }
        if (!g_ctx || f.color.w > g_maxW || f.color.h > g_maxH || f.out.w != g_outW || f.out.h != g_outH)
            if (!makeContext((std::max)(f.color.w, g_maxW), (std::max)(f.color.h, g_maxH), f.out.w, f.out.h))
            {
                g_failed = true;
                return false;
            }
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = cmd;
        d.color = resource(f.color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.depth = resource(f.depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.motionVectors = resource(f.motion, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.output = resource(f.out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitterX, f.jitterY};
        // (gt4up's motion scale gives the way back in [0, 1] of the picture; FidelityFX wants render pixels)
        d.motionVectorScale = {f.motionScaleX * static_cast<float>(f.color.w), f.motionScaleY * static_cast<float>(f.color.h)};
        d.renderSize = {static_cast<uint32_t>(f.color.w), static_cast<uint32_t>(f.color.h)};
        d.upscaleSize = {static_cast<uint32_t>(f.out.w), static_cast<uint32_t>(f.out.h)};
        static const float sharpness = [] {
            const char *v = std::getenv("GT4_FSR_SHARPNESS");
            return v && *v ? static_cast<float>(std::atof(v)) : 0.3f;
        }();
        d.enableSharpening = sharpness > 0.0f;
        d.sharpness = sharpness > 1.0f ? 1.0f : sharpness;
        d.frameTimeDelta = 1000.0f / 60.0f;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = 1.0f;
        d.cameraFar = 100000.0f;
        d.cameraFovAngleVertical = 1.0f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = 0;
        const ffxReturnCode_t r = pDispatch(&g_ctx, &d.header);
        if (r != FFX_API_RETURN_OK)
        {
            static int failures = 0;
            if (failures++ < 8)
                std::fprintf(stderr, "[fsr] dispatch failed (result %u)\n", r);
            return false;
        }
        if ((++g_frames % 3600u) == 1u)
            std::fprintf(stderr, "[fsr] %llu frames: %dx%d -> %dx%d\n", static_cast<unsigned long long>(g_frames), f.color.w, f.color.h, f.out.w, f.out.h);
        return true;
    }
} // namespace gt4fsr

#else

namespace gt4fsr
{
    bool start()
    {
        static bool logged = false;
        if (asked() && !logged)
        {
            logged = true;
            std::fprintf(stderr, "[fsr] GT4_UPSCALER=fsr: this build has no FSR (FidelityFX SDK not found when built)\n");
        }
        return false;
    }
    bool usable() { return false; }
    bool evaluate(void *, const gt4up::Frame &) { return false; }
} // namespace gt4fsr

#endif
