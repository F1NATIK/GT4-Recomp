// GT4_UPSCALER=fsr4: AMD's newest upscaler through Direct3D 12 (FSR 4 on Radeon RX 9000 cards; AMD's FSR 3.1 on other
// cards), from the FidelityFX SDK 2.3 (amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll beside
// GT4.exe; AMD ships FSR 4 for Direct3D 12 only). A small bridge: a Direct3D 12 device on the Vulkan renderer's own
// graphics card, four images both APIs share (the drawing, its depth, the motion, the picture) and a fence both share.
// Each frame the renderer copies its inputs into the shared images, hands over (vkgl::splitExternal: its work so far
// submitted, signalling the fence), Direct3D 12 runs the upscaler (waiting for that, signalling when done), and the
// renderer's next commands wait for it and copy the picture back. Nothing waits on the CPU. Off (unset): nothing loads.
#include "gt4_upscale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4fsr4
{
    bool asked()
    {
        static const bool a = [] {
            const char *v = std::getenv("GT4_UPSCALER");
            return v && std::strcmp(v, "fsr4") == 0;
        }();
        return a;
    }
} // namespace gt4fsr4

#if defined(_WIN32) && defined(GT4_HAVE_FFX2) && defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include <windows.h>

#include "gs/gs_vk_device.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"

#include <algorithm>
#include <string>

namespace gt4fsr4
{
    namespace
    {
        // ---- the FidelityFX loader
        HMODULE g_ffx = nullptr;
        PfnFfxCreateContext pCreate = nullptr;
        PfnFfxDestroyContext pDestroy = nullptr;
        PfnFfxDispatch pDispatch = nullptr;
        PfnFfxQuery pQuery = nullptr;
        ffxContext g_ctx = nullptr;
        int g_maxW = 0, g_maxH = 0, g_outW = 0, g_outH = 0;
        bool g_tried = false, g_ok = false, g_failed = false;
        uint64_t g_frames = 0;

        // ---- Direct3D 12
        ID3D12Device *g_dev = nullptr;
        ID3D12CommandQueue *g_queue = nullptr;
        ID3D12CommandAllocator *g_alloc[3] = {};
        uint64_t g_allocDone[3] = {};
        ID3D12GraphicsCommandList *g_list = nullptr;
        ID3D12Fence *g_fence = nullptr;
        HANDLE g_fenceEvent = nullptr;
        uint64_t g_value = 0;
        int g_allocNext = 0;

        // ---- the fence and images both APIs share
        VkSemaphore g_sem = VK_NULL_HANDLE;
        struct Shared
        {
            ID3D12Resource *res = nullptr;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            DXGI_FORMAT dx = DXGI_FORMAT_UNKNOWN;
            VkFormat vk = VK_FORMAT_UNDEFINED;
            int w = 0, h = 0;
            bool fresh = true; // (its Vulkan layout still undefined)
        };
        Shared g_color, g_depth, g_motion, g_out;
        VkBuffer g_depthBuf = VK_NULL_HANDLE; // (Vulkan's depth goes through a buffer: depth and colour images do not copy)
        VkDeviceMemory g_depthBufMem = VK_NULL_HANDLE;
        VkDeviceSize g_depthBufSize = 0;

        void message(uint32_t type, const wchar_t *text)
        {
            static int shown = 0;
            if (shown++ < 32)
                std::fprintf(stderr, "[fsr4] %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
        }

        std::wstring exeDir()
        {
            wchar_t path[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring p(path, n);
            const size_t slash = p.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
        }

        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        const gsvk::Device *vkdev() { return gsvk::sharedDevice(); }

        // The Direct3D 12 device on the Vulkan device's graphics card (the same LUID), its queue, lists and the shared fence.
        bool makeD3D()
        {
            const gsvk::Device *dev = vkdev();
            if (!dev || !dev->device || !dev->externalMemory || !dev->timeline)
            {
                std::fprintf(stderr, "[fsr4] the Vulkan device cannot share images or fences (external memory %d, timeline %d): FSR 4 off\n",
                             dev ? dev->externalMemory : 0, dev ? dev->timeline : 0);
                return false;
            }
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vkGetPhysicalDeviceProperties2(dev->physical, &props);
            if (!id.deviceLUIDValid)
            {
                std::fprintf(stderr, "[fsr4] the Vulkan device has no LUID: FSR 4 off\n");
                return false;
            }
            LUID luid;
            std::memcpy(&luid, id.deviceLUID, sizeof(luid));
            HMODULE dxgi = LoadLibraryW(L"dxgi.dll"), d3d12 = LoadLibraryW(L"d3d12.dll");
            using FnFactory = HRESULT(WINAPI *)(REFIID, void **);
            const auto makeFactory = dxgi ? reinterpret_cast<FnFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
            const auto makeDevice = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
            if (!makeFactory || !makeDevice)
            {
                std::fprintf(stderr, "[fsr4] Direct3D 12 is not on this PC: FSR 4 off\n");
                return false;
            }
            IDXGIFactory4 *factory = nullptr;
            IDXGIAdapter1 *adapter = nullptr;
            HRESULT hr = makeFactory(__uuidof(IDXGIFactory4), reinterpret_cast<void **>(&factory));
            if (SUCCEEDED(hr))
                hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter));
            if (SUCCEEDED(hr))
                hr = makeDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g_dev));
            release(adapter);
            release(factory);
            if (FAILED(hr) || !g_dev)
            {
                std::fprintf(stderr, "[fsr4] no Direct3D 12 device on %s (0x%08lx): FSR 4 off\n", dev->name.c_str(), static_cast<unsigned long>(hr));
                return false;
            }
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g_queue));
            for (auto &a : g_alloc)
                if (SUCCEEDED(hr))
                    hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&a));
            if (SUCCEEDED(hr))
                hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                              reinterpret_cast<void **>(&g_list));
            if (SUCCEEDED(hr))
                hr = g_list->Close();
            if (SUCCEEDED(hr))
                hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&g_fence));
            HANDLE fh = nullptr;
            if (SUCCEEDED(hr))
                hr = g_dev->CreateSharedHandle(g_fence, nullptr, GENERIC_ALL, nullptr, &fh);
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[fsr4] Direct3D 12 queue or fence not made (0x%08lx): FSR 4 off\n", static_cast<unsigned long>(hr));
                return false;
            }
            g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            // The fence as a Vulkan timeline semaphore.
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            VkResult r = vkCreateSemaphore(dev->device, &sci, nullptr, &g_sem);
            VkImportSemaphoreWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
            imp.semaphore = g_sem;
            imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            imp.handle = fh;
            if (r == VK_SUCCESS)
                r = vkImportSemaphoreWin32HandleKHR ? vkImportSemaphoreWin32HandleKHR(dev->device, &imp) : VK_ERROR_EXTENSION_NOT_PRESENT;
            CloseHandle(fh);
            if (r != VK_SUCCESS)
            {
                std::fprintf(stderr, "[fsr4] the shared fence did not reach Vulkan (%d): FSR 4 off\n", static_cast<int>(r));
                return false;
            }
            std::fprintf(stderr, "[fsr4] Direct3D 12 bridge on %s\n", dev->name.c_str());
            return true;
        }

        void dropShared(Shared &s)
        {
            const gsvk::Device *dev = vkdev();
            if (s.image)
                vkDestroyImage(dev->device, s.image, nullptr);
            if (s.memory)
                vkFreeMemory(dev->device, s.memory, nullptr);
            release(s.res);
            s = Shared{};
        }

        // A Direct3D 12 texture (shared) and the same memory as a Vulkan image.
        bool makeShared(Shared &s, int w, int h, DXGI_FORMAT dx, VkFormat vk, bool uav)
        {
            if (s.res && s.w == w && s.h == h)
                return true;
            dropShared(s);
            const gsvk::Device *dev = vkdev();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = static_cast<UINT64>(w);
            rd.Height = static_cast<UINT>(h);
            rd.DepthOrArraySize = 1;
            rd.MipLevels = 1;
            rd.Format = dx;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            HRESULT hr = g_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&s.res));
            HANDLE handle = nullptr;
            if (SUCCEEDED(hr))
                hr = g_dev->CreateSharedHandle(s.res, nullptr, GENERIC_ALL, nullptr, &handle);
            if (FAILED(hr))
            {
                std::fprintf(stderr, "[fsr4] shared %dx%d image not made (0x%08lx)\n", w, h, static_cast<unsigned long>(hr));
                return false;
            }
            VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = vk;
            ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        (uav ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkResult r = vkCreateImage(dev->device, &ici, nullptr, &s.image);
            VkMemoryRequirements req{};
            VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
            if (r == VK_SUCCESS)
            {
                vkGetImageMemoryRequirements(dev->device, s.image, &req);
                r = vkGetMemoryWin32HandlePropertiesKHR
                        ? vkGetMemoryWin32HandlePropertiesKHR(dev->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &hp)
                        : VK_ERROR_EXTENSION_NOT_PRESENT;
            }
            uint32_t typeBits = req.memoryTypeBits & (hp.memoryTypeBits ? hp.memoryTypeBits : ~0u);
            const uint32_t type = gsvk::memoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != UINT32_MAX
                                      ? gsvk::memoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                      : gsvk::memoryType(typeBits, 0);
            if (r == VK_SUCCESS && type != UINT32_MAX)
            {
                VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                dedicated.image = s.image;
                VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
                imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
                imp.handle = handle;
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = type;
                r = vkAllocateMemory(dev->device, &mai, nullptr, &s.memory);
                if (r == VK_SUCCESS)
                    r = vkBindImageMemory(dev->device, s.image, s.memory, 0);
            }
            else if (r == VK_SUCCESS)
                r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            CloseHandle(handle);
            if (r != VK_SUCCESS)
            {
                std::fprintf(stderr, "[fsr4] shared %dx%d image did not reach Vulkan (%d)\n", w, h, static_cast<int>(r));
                dropShared(s);
                return false;
            }
            s.dx = dx;
            s.vk = vk;
            s.w = w;
            s.h = h;
            s.fresh = true;
            return true;
        }

        bool makeDepthBuffer(VkDeviceSize size)
        {
            if (g_depthBuf && g_depthBufSize >= size)
                return true;
            const gsvk::Device *dev = vkdev();
            if (g_depthBuf)
                vkDestroyBuffer(dev->device, g_depthBuf, nullptr);
            if (g_depthBufMem)
                vkFreeMemory(dev->device, g_depthBufMem, nullptr);
            g_depthBuf = VK_NULL_HANDLE;
            g_depthBufMem = VK_NULL_HANDLE;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(dev->device, &bci, nullptr, &g_depthBuf) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(dev->device, g_depthBuf, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = gsvk::memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev->device, &mai, nullptr, &g_depthBufMem) != VK_SUCCESS)
                return false;
            vkBindBufferMemory(dev->device, g_depthBuf, g_depthBufMem, 0);
            g_depthBufSize = size;
            return true;
        }

        bool makeContext(int maxW, int maxH, int outW, int outH)
        {
            if (g_ctx)
            {
                pDestroy(&g_ctx, nullptr);
                g_ctx = nullptr;
            }
            ffxCreateBackendDX12Desc backend{};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = g_dev;
            ffxCreateContextDescUpscale desc{};
            desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            desc.header.pNext = &backend.header;
            desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
            desc.maxRenderSize = {static_cast<uint32_t>(maxW), static_cast<uint32_t>(maxH)};
            desc.maxUpscaleSize = {static_cast<uint32_t>(outW), static_cast<uint32_t>(outH)};
            desc.fpMessage = &message;
            const ffxReturnCode_t r = pCreate(&g_ctx, &desc.header, nullptr);
            if (r != FFX_API_RETURN_OK || !g_ctx)
            {
                std::fprintf(stderr, "[fsr4] upscaler not made (result %u): FSR 4 off\n", r);
                g_ctx = nullptr;
                return false;
            }
            g_maxW = maxW;
            g_maxH = maxH;
            g_outW = outW;
            g_outH = outH;
            ffxQueryGetProviderVersion v{};
            v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            const bool named = pQuery && pQuery(&g_ctx, &v.header) == FFX_API_RETURN_OK && v.versionName;
            std::fprintf(stderr, "[fsr4] AMD upscaler %s: up to %dx%d -> %dx%d\n", named ? v.versionName : "(version unknown)", maxW, maxH, outW, outH);
            return true;
        }

        void barrier(VkCommandBuffer cb, const Shared &s, VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess, uint32_t srcFamily, uint32_t dstFamily,
                     bool fresh)
        {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = dstAccess;
            b.oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = srcFamily;
            b.dstQueueFamilyIndex = dstFamily;
            b.image = s.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.imageMemoryBarrierCount = 1;
            d.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cb, &d);
        }

        void d3dBarrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            g_list->ResourceBarrier(1, &b);
        }
    } // namespace

    bool start()
    {
        if (g_tried)
            return g_ok;
        g_tried = true;
        if (!asked())
            return false;
        const std::wstring dir = exeDir();
        // (the loader finds amd_fidelityfx_upscaler_dx12.dll beside itself)
        g_ffx = LoadLibraryExW((dir + L"\\amd_fidelityfx_loader_dx12.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_ffx)
        {
            std::fprintf(stderr, "[fsr4] GT4_UPSCALER=fsr4 but amd_fidelityfx_loader_dx12.dll did not load from beside GT4.exe (error %lu): FSR 4 off\n",
                         GetLastError());
            return false;
        }
        pCreate = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_ffx, "ffxCreateContext"));
        pDestroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_ffx, "ffxDestroyContext"));
        pDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_ffx, "ffxDispatch"));
        pQuery = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_ffx, "ffxQuery"));
        if (!pCreate || !pDestroy || !pDispatch)
        {
            std::fprintf(stderr, "[fsr4] amd_fidelityfx_loader_dx12.dll lacks the FidelityFX API: FSR 4 off\n");
            return false;
        }
        std::fprintf(stderr, "[fsr4] AMD FidelityFX loader (Direct3D 12) loaded: FSR 4 on Radeon RX 9000, FSR 3.1 elsewhere\n");
        g_ok = true;
        return true;
    }

    bool usable() { return start() && !g_failed; }

    bool evaluate(void *cmd, const gt4up::Frame &f)
    {
        if (!usable())
            return false;
        if (!f.split)
        {
            std::fprintf(stderr, "[fsr4] the renderer cannot hand over mid-frame: FSR 4 off\n");
            g_failed = true;
            return false;
        }
        if (!g_dev && !makeD3D())
        {
            g_failed = true;
            return false;
        }
        const gsvk::Device *dev = vkdev();
        if (f.color.format != VK_FORMAT_R8G8B8A8_UNORM || f.depth.format != VK_FORMAT_D32_SFLOAT || f.motion.format != VK_FORMAT_R16G16_SFLOAT ||
            f.out.format != VK_FORMAT_R8G8B8A8_UNORM)
        {
            static int logged = 0;
            if (logged++ < 4)
                std::fprintf(stderr, "[fsr4] unexpected image formats (%u %u %u -> %u): not upscaled\n", f.color.format, f.depth.format, f.motion.format,
                             f.out.format);
            return false;
        }
        const int w = f.color.w, h = f.color.h, ow = f.out.w, oh = f.out.h;
        // (the shared images at the largest size asked for: a smaller frame uses their corner)
        const int sw = (std::max)(w, g_color.w), sh = (std::max)(h, g_color.h);
        const bool sizes = (g_color.res && sw == g_color.w && sh == g_color.h && ow <= g_out.w && oh <= g_out.h);
        if (!sizes)
        {
            {
                std::lock_guard<std::mutex> lock(gsvk::queueMutex());
                vkQueueWaitIdle(dev->queue); // (only when the sizes grow: the old shared images may still be in use)
            }
            if (g_value)
            {
                g_fence->SetEventOnCompletion(g_value, g_fenceEvent);
                if (g_fence->GetCompletedValue() < g_value)
                    WaitForSingleObject(g_fenceEvent, 5000);
            }
            if (!makeShared(g_color, sw, sh, DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, false) ||
                !makeShared(g_depth, sw, sh, DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, false) ||
                !makeShared(g_motion, sw, sh, DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, false) ||
                !makeShared(g_out, (std::max)(ow, g_out.w), (std::max)(oh, g_out.h), DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, true) ||
                !makeDepthBuffer(static_cast<VkDeviceSize>(sw) * static_cast<VkDeviceSize>(sh) * 4u))
            {
                g_failed = true;
                return false;
            }
        }
        if (!g_ctx || w > g_maxW || h > g_maxH || ow != g_outW || oh != g_outH)
            if (!makeContext((std::max)(w, g_maxW), (std::max)(h, g_maxH), ow, oh))
            {
                g_failed = true;
                return false;
            }
        // ---- Vulkan: the inputs into the shared images, then handed over
        const VkCommandBuffer cb = static_cast<VkCommandBuffer>(cmd);
        const uint32_t fam = dev->queueFamily;
        Shared *const ins[] = {&g_color, &g_depth, &g_motion};
        for (Shared *s : ins)
            barrier(cb, *s, VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT, s->fresh ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL,
                    s->fresh ? VK_QUEUE_FAMILY_IGNORED : fam, s->fresh);
        VkImageCopy ic{};
        ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ic.srcOffset = {f.color.x, f.color.y, 0};
        ic.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        vkCmdCopyImage(cb, reinterpret_cast<VkImage>(static_cast<uintptr_t>(f.color.image)), VK_IMAGE_LAYOUT_GENERAL, g_color.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        ic.srcOffset = {f.motion.x, f.motion.y, 0};
        vkCmdCopyImage(cb, reinterpret_cast<VkImage>(static_cast<uintptr_t>(f.motion.image)), VK_IMAGE_LAYOUT_GENERAL, g_motion.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
        VkBufferImageCopy bc{};
        bc.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        bc.imageOffset = {f.depth.x, f.depth.y, 0};
        bc.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        vkCmdCopyImageToBuffer(cb, reinterpret_cast<VkImage>(static_cast<uintptr_t>(f.depth.image)), VK_IMAGE_LAYOUT_GENERAL, g_depthBuf, 1, &bc);
        {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.memoryBarrierCount = 1;
            d.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cb, &d);
        }
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageOffset = {0, 0, 0};
        vkCmdCopyBufferToImage(cb, g_depthBuf, g_depth.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        for (Shared *s : ins)
        {
            barrier(cb, *s, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_NONE, fam, VK_QUEUE_FAMILY_EXTERNAL, false);
            s->fresh = false;
        }
        barrier(cb, g_out, VK_ACCESS_2_NONE, VK_ACCESS_2_NONE, g_out.fresh ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL,
                g_out.fresh ? VK_QUEUE_FAMILY_IGNORED : fam, g_out.fresh);
        if (g_out.fresh)
            barrier(cb, g_out, VK_ACCESS_2_NONE, VK_ACCESS_2_NONE, fam, VK_QUEUE_FAMILY_EXTERNAL, false);
        g_out.fresh = false;
        const uint64_t ready = ++g_value, done = ++g_value;
        // ---- Direct3D 12: the upscaler (after Vulkan's copies; Vulkan's next commands wait for it)
        const int ai = g_allocNext;
        g_allocNext = (g_allocNext + 1) % 3;
        if (g_fence->GetCompletedValue() < g_allocDone[ai])
        {
            g_fence->SetEventOnCompletion(g_allocDone[ai], g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 1000);
        }
        g_alloc[ai]->Reset();
        g_list->Reset(g_alloc[ai], nullptr);
        d3dBarrier(g_color.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3dBarrier(g_depth.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3dBarrier(g_motion.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d3dBarrier(g_out.res, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = g_list;
        d.color = ffxApiGetResourceDX12(g_color.res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(g_depth.res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(g_motion.res, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(g_out.res, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitterX, f.jitterY};
        d.motionVectorScale = {f.motionScaleX * static_cast<float>(w), f.motionScaleY * static_cast<float>(h)};
        d.renderSize = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
        d.upscaleSize = {static_cast<uint32_t>(ow), static_cast<uint32_t>(oh)};
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
        const ffxReturnCode_t fr = pDispatch(&g_ctx, &d.header);
        d3dBarrier(g_color.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        d3dBarrier(g_depth.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        d3dBarrier(g_motion.res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        d3dBarrier(g_out.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        g_list->Close();
        if (fr != FFX_API_RETURN_OK)
        {
            static int failures = 0;
            if (failures++ < 8)
                std::fprintf(stderr, "[fsr4] dispatch failed (result %u): this frame's picture is the plain one\n", fr);
        }
        // (Vulkan's work so far submitted, signalling `ready`; Direct3D 12 waits for it, runs, signals `done`)
        const VkCommandBuffer cb2 = static_cast<VkCommandBuffer>(f.split(reinterpret_cast<uint64_t>(g_sem), ready, done));
        g_queue->Wait(g_fence, ready);
        if (fr == FFX_API_RETURN_OK)
        {
            ID3D12CommandList *lists[] = {g_list};
            g_queue->ExecuteCommandLists(1, lists);
        }
        g_queue->Signal(g_fence, done);
        g_allocDone[ai] = done;
        // ---- Vulkan again: the picture back (after Direct3D 12's signal)
        barrier(cb2, g_out, VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, fam, false);
        if (fr == FFX_API_RETURN_OK)
        {
            VkImageCopy oc{};
            oc.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            oc.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            oc.dstOffset = {f.out.x, f.out.y, 0};
            oc.extent = {static_cast<uint32_t>(ow), static_cast<uint32_t>(oh), 1u};
            vkCmdCopyImage(cb2, g_out.image, VK_IMAGE_LAYOUT_GENERAL, reinterpret_cast<VkImage>(static_cast<uintptr_t>(f.out.image)),
                           VK_IMAGE_LAYOUT_GENERAL, 1, &oc);
        }
        barrier(cb2, g_out, VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_NONE, fam, VK_QUEUE_FAMILY_EXTERNAL, false);
        if (fr != FFX_API_RETURN_OK)
            return false;
        if ((++g_frames % 3600u) == 1u)
            std::fprintf(stderr, "[fsr4] %llu frames: %dx%d -> %dx%d\n", static_cast<unsigned long long>(g_frames), w, h, ow, oh);
        return true;
    }
} // namespace gt4fsr4

#else

namespace gt4fsr4
{
    bool start()
    {
        static bool logged = false;
        if (asked() && !logged)
        {
            logged = true;
            std::fprintf(stderr, "[fsr4] GT4_UPSCALER=fsr4: this build has no FSR 4 (FidelityFX SDK 2 not found when built)\n");
        }
        return false;
    }
    bool usable() { return false; }
    bool evaluate(void *, const gt4up::Frame &) { return false; }
} // namespace gt4fsr4

#endif
