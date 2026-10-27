#include "gs_vk_context.h"
#include "gs_vk_device.h"
#include "gs_gl_shaders.h"
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "../gt4_upscale.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gsvk
{
    namespace
    {
        // ---- OpenGL side (EXT_memory_object(_win32), EXT_semaphore(_win32))
        using GLenum = unsigned int;
        using GLuint = unsigned int;
        using GLint = int;
        using GLsizei = int;
        using GLuint64 = unsigned long long;
        using GLubyte = unsigned char;
        constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_RGBA8 = 0x8058, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                         GL_LINEAR = 0x2601, GL_DEVICE_LUID_EXT = 0x9599, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT = 0x9587,
                         GL_DEDICATED_MEMORY_OBJECT_EXT = 0x9581, GL_LAYOUT_GENERAL_EXT = 0x958D;

        struct GlFunctions
        {
            void(APIENTRY *getUnsignedBytev)(GLenum, GLubyte *) = nullptr;
            void(APIENTRY *createMemoryObjects)(GLsizei, GLuint *) = nullptr;
            void(APIENTRY *memoryObjectParameteriv)(GLuint, GLenum, const GLint *) = nullptr;
            void(APIENTRY *importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void *) = nullptr;
            void(APIENTRY *texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
            void(APIENTRY *genSemaphores)(GLsizei, GLuint *) = nullptr;
            void(APIENTRY *importSemaphoreWin32Handle)(GLuint, GLenum, void *) = nullptr;
            void(APIENTRY *waitSemaphore)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *) = nullptr;
            void(APIENTRY *signalSemaphore)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *) = nullptr;
            void(APIENTRY *genTextures)(GLsizei, GLuint *) = nullptr;
            void(APIENTRY *bindTexture)(GLenum, GLuint) = nullptr;
            void(APIENTRY *texParameteri)(GLenum, GLenum, GLint) = nullptr;
            void(APIENTRY *flush)() = nullptr;
            void(APIENTRY *deleteTextures)(GLsizei, const GLuint *) = nullptr;
        };
        GlFunctions gl;

        template <typename F>
        bool loadGl(F &f, const char *name)
        {
            PROC p = wglGetProcAddress(name);
            if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) ||
                p == reinterpret_cast<PROC>(-1))
                p = GetProcAddress(GetModuleHandleA("opengl32.dll"), name);
            f = reinterpret_cast<F>(p);
            return f != nullptr;
        }

        bool check(VkResult r, const char *what)
        {
            if (r == VK_SUCCESS)
                return true;
            std::fprintf(stderr, "[vk] %s failed: %d\n", what, static_cast<int>(r));
            return false;
        }

        bool hasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
        {
            for (const VkExtensionProperties &e : list)
                if (std::strcmp(e.extensionName, name) == 0)
                    return true;
            return false;
        }

        bool envOff(const char *name)
        {
            const char *v = std::getenv(name);
            return v && std::strcmp(v, "0") == 0;
        }

        Device g_shared;
        bool g_sharedTried = false;
        std::mutex g_sharedMutex;

        bool createSharedDevice(Device &d)
        {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            // GT4's DLSS (gt4_upscale.h, GT4_UPSCALER=dlss): Vulkan through Streamline's wrappers, which add what DLSS
            // needs to the instance and the device.
            if (void *proc = gt4up::instanceProcAddr())
                volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc));
            else
#endif
            if (!check(volkInitialize(), "volkInitialize (vulkan-1.dll)"))
                return false;
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "GT3 native";
            app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            ici.pApplicationInfo = &app;
            // The window surface (the presenter that shows frames on the window
            // itself, gs_vk_present.h), where the loader has it.
            uint32_t instanceExtCount = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtCount, nullptr);
            std::vector<VkExtensionProperties> instanceExts(instanceExtCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtCount, instanceExts.data());
            const char *surfaceExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
            d.surface = hasExtension(instanceExts, surfaceExts[0]) && hasExtension(instanceExts, surfaceExts[1]);
            if (d.surface)
            {
                ici.enabledExtensionCount = 2;
                ici.ppEnabledExtensionNames = surfaceExts;
            }
            // GT3_VK_VALIDATION=1: the Khronos validation layer, if installed.
            const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
            if (const char *v = std::getenv("GT3_VK_VALIDATION"); v && *v == '1')
            {
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = layers;
            }
            if (vkCreateInstance(&ici, nullptr, &d.instance) != VK_SUCCESS)
            {
                ici.enabledLayerCount = 0;
                if (!check(vkCreateInstance(&ici, nullptr, &d.instance), "vkCreateInstance"))
                    return false;
            }
            volkLoadInstance(d.instance);

            // The OpenGL context's GPU (its LUID), to render on the same one.
            loadGl(gl.getUnsignedBytev, "glGetUnsignedBytevEXT");
            GLubyte glLuid[8]{};
            bool haveGlLuid = false;
            if (gl.getUnsignedBytev)
            {
                gl.getUnsignedBytev(GL_DEVICE_LUID_EXT, glLuid);
                for (GLubyte b : glLuid)
                    haveGlLuid |= b != 0;
            }
            uint32_t count = 0;
            vkEnumeratePhysicalDevices(d.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(d.instance, &count, devices.data());
            int bestScore = -1;
            for (VkPhysicalDevice pd : devices)
            {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                vkGetPhysicalDeviceProperties2(pd, &props);
                int score = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                const bool sameGpu = haveGlLuid && id.deviceLUIDValid && std::memcmp(id.deviceLUID, glLuid, 8) == 0;
                if (sameGpu)
                    score = 10;
                if (props.properties.apiVersion < VK_API_VERSION_1_3)
                    score = 0;
                if (score > bestScore)
                {
                    bestScore = score;
                    d.physical = pd;
                    d.properties = props.properties;
                    d.sameGpuAsGl = sameGpu;
                    d.name = props.properties.deviceName;
                }
            }
            if (!d.physical || bestScore <= 0)
            {
                std::fprintf(stderr, "[vk] no Vulkan 1.3 device\n");
                return false;
            }
            vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory);
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &familyCount, families.data());
            d.queueFamily = UINT32_MAX;
            for (uint32_t i = 0; i < familyCount; ++i)
                if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                {
                    d.queueFamily = i;
                    break;
                }
            if (d.queueFamily == UINT32_MAX)
                return false;

            // Extensions and features: what the GS renderer uses, where present.
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &extCount, available.data());
            std::vector<const char *> extensions;
            auto want = [&](const char *name) {
                if (!hasExtension(available, name))
                    return false;
                extensions.push_back(name);
                return true;
            };
            d.externalMemory = want(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) && want(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
            d.pushDescriptor = want(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
            d.swapchain = d.surface && want(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            const bool hasInterlockExt = !envOff("GT3_VK_INTERLOCK") && want(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
            d.conservative = !envOff("GT3_VK_CONSERVATIVE") && want(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
            const bool hasLocalReadExt = !envOff("GT3_VK_LOCAL_READ") && want(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);

            // Extension feature structs only for extensions the device has.
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR localReadQ{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
            VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockQ{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
            void *queryExt = nullptr;
            if (hasLocalReadExt)
            {
                localReadQ.pNext = queryExt;
                queryExt = &localReadQ;
            }
            if (hasInterlockExt)
            {
                interlockQ.pNext = queryExt;
                queryExt = &interlockQ;
            }
            VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, queryExt};
            VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &q13};
            VkPhysicalDeviceVulkan11Features q11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &q12};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q11};
            vkGetPhysicalDeviceFeatures2(d.physical, &q);
            if (!q13.dynamicRendering || !q13.synchronization2)
            {
                std::fprintf(stderr, "[vk] the device lacks dynamic rendering / synchronization2\n");
                return false;
            }

            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR localRead{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
            localRead.dynamicRenderingLocalRead = localReadQ.dynamicRenderingLocalRead;
            d.localRead = hasLocalReadExt && localReadQ.dynamicRenderingLocalRead;
            VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
            interlock.fragmentShaderPixelInterlock = interlockQ.fragmentShaderPixelInterlock;
            d.interlock = hasInterlockExt && interlockQ.fragmentShaderPixelInterlock;
            void *enableExt = nullptr;
            if (hasLocalReadExt)
            {
                localRead.pNext = enableExt;
                enableExt = &localRead;
            }
            if (hasInterlockExt)
            {
                interlock.pNext = enableExt;
                enableExt = &interlock;
            }
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, enableExt};
            f13.dynamicRendering = VK_TRUE;
            f13.synchronization2 = VK_TRUE;
            f13.maintenance4 = q13.maintenance4;
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
            f12.timelineSemaphore = q12.timelineSemaphore;
            d.timeline = q12.timelineSemaphore;
            f12.hostQueryReset = q12.hostQueryReset;
            d.hostQueryReset = q12.hostQueryReset;
            d.bindless = q12.descriptorIndexing && q12.runtimeDescriptorArray && q12.descriptorBindingPartiallyBound &&
                         q12.descriptorBindingSampledImageUpdateAfterBind && q12.descriptorBindingUpdateUnusedWhilePending &&
                         q12.shaderSampledImageArrayNonUniformIndexing && !envOff("GT3_VK_BINDLESS");
            if (d.bindless)
            {
                f12.descriptorIndexing = VK_TRUE;
                f12.runtimeDescriptorArray = VK_TRUE;
                f12.descriptorBindingPartiallyBound = VK_TRUE;
                f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
                f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
                f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            }
            VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
            f11.shaderDrawParameters = q11.shaderDrawParameters;
            d.drawParameters = q11.shaderDrawParameters;
            VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
            f.features.shaderFloat64 = q.features.shaderFloat64;
            f.features.fragmentStoresAndAtomics = q.features.fragmentStoresAndAtomics;
            f.features.shaderClipDistance = q.features.shaderClipDistance;
            f.features.dualSrcBlend = q.features.dualSrcBlend;
            f.features.depthClamp = q.features.depthClamp;
            f.features.shaderStorageImageWriteWithoutFormat = q.features.shaderStorageImageWriteWithoutFormat;
            f.features.shaderStorageImageReadWithoutFormat = q.features.shaderStorageImageReadWithoutFormat;
            d.float64 = q.features.shaderFloat64;
            d.dualSrcBlend = q.features.dualSrcBlend;
            d.depthClamp = q.features.depthClamp;

            const float priority = 1.0f;
            VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qci.queueFamilyIndex = d.queueFamily;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f};
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &qci;
            dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            dci.ppEnabledExtensionNames = extensions.data();
            if (!check(vkCreateDevice(d.physical, &dci, nullptr, &d.device), "vkCreateDevice"))
                return false;
            volkLoadDevice(d.device);
            vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);
            std::fprintf(stderr,
                         "[vk] device %s%s: interlock %d, conservative raster %d, push descriptors %d, float64 %d, dual-source blend %d, "
                         "depth clamp %d, bindless %d, external memory %d, local read %d, swapchain %d, timeline %d\n",
                         d.name.c_str(), d.sameGpuAsGl ? " (the OpenGL window's GPU)" : "", d.interlock, d.conservative, d.pushDescriptor,
                         d.float64, d.dualSrcBlend, d.depthClamp, d.bindless, d.externalMemory, d.localRead, d.swapchain, d.timeline);
            return true;
        }

        // ---- the interop self-test
        VkImage g_image = VK_NULL_HANDLE;
        VkDeviceMemory g_memory = VK_NULL_HANDLE;
        VkImageView g_view = VK_NULL_HANDLE;
        VkSemaphore g_vkDone = VK_NULL_HANDLE, g_glDone = VK_NULL_HANDLE;
        VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
        VkDescriptorPool g_pool = VK_NULL_HANDLE;
        VkDescriptorSet g_set = VK_NULL_HANDLE;
        VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
        VkPipeline g_pipeline = VK_NULL_HANDLE;
        VkCommandPool g_commandPool = VK_NULL_HANDLE;
        VkCommandBuffer g_commands = VK_NULL_HANDLE;
        VkFence g_fence = VK_NULL_HANDLE;
        VkDevice g_device = VK_NULL_HANDLE;
        VkQueue g_queue = VK_NULL_HANDLE;
        int g_width = 0, g_height = 0;
        uint64_t g_frame = 0;
        bool g_glSignalled = false;
        std::string g_deviceName = "none";

        GLuint g_glMemory = 0, g_glTexture = 0, g_glVkDone = 0, g_glGlDone = 0;

        // GLSL (Vulkan) -> SPIR-V with glslang.
        bool compileGlsl(const char *source, EShLanguage stage, std::vector<uint32_t> &spirv)
        {
            static bool initialised = false;
            if (!initialised)
                initialised = glslang::InitializeProcess();
            glslang::TShader shader(stage);
            shader.setStrings(&source, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
            if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault))
            {
                std::fprintf(stderr, "[vk] GLSL: %s\n", shader.getInfoLog());
                return false;
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(EShMsgDefault))
            {
                std::fprintf(stderr, "[vk] GLSL link: %s\n", program.getInfoLog());
                return false;
            }
            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
            return !spirv.empty();
        }

        const char kTestShader[] = R"GLSL(#version 450
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0, rgba8) uniform writeonly image2D u_image;
layout(push_constant) uniform Push { float time; } pc;
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(u_image);
    if (p.x >= size.x || p.y >= size.y)
        return;
    vec2 uv = vec2(p) / vec2(size);
    float wave = 0.5 + 0.5 * sin(uv.x * 14.0 + pc.time * 3.0);
    vec3 c = mix(vec3(0.85, 0.12, 0.10), vec3(0.10, 0.30, 0.90), uv.y) * (0.55 + 0.45 * wave);
    // A moving bright band: frames change, so the chain runs every frame.
    float band = smoothstep(0.02, 0.0, abs(fract(uv.x - pc.time * 0.25) - 0.5));
    imageStore(u_image, p, vec4(mix(c, vec3(1.0), band), 1.0));
}
)GLSL";

        bool loadGlInterop()
        {
            bool ok = true;
            ok &= loadGl(gl.createMemoryObjects, "glCreateMemoryObjectsEXT");
            ok &= loadGl(gl.memoryObjectParameteriv, "glMemoryObjectParameterivEXT");
            ok &= loadGl(gl.importMemoryWin32Handle, "glImportMemoryWin32HandleEXT");
            ok &= loadGl(gl.texStorageMem2D, "glTexStorageMem2DEXT");
            ok &= loadGl(gl.genSemaphores, "glGenSemaphoresEXT");
            ok &= loadGl(gl.importSemaphoreWin32Handle, "glImportSemaphoreWin32HandleEXT");
            ok &= loadGl(gl.waitSemaphore, "glWaitSemaphoreEXT");
            ok &= loadGl(gl.signalSemaphore, "glSignalSemaphoreEXT");
            ok &= loadGl(gl.genTextures, "glGenTextures");
            ok &= loadGl(gl.bindTexture, "glBindTexture");
            ok &= loadGl(gl.texParameteri, "glTexParameteri");
            ok &= loadGl(gl.flush, "glFlush");
            ok &= loadGl(gl.deleteTextures, "glDeleteTextures");
            if (!ok)
                std::fprintf(stderr, "[vk] OpenGL interop (EXT_memory_object_win32 / EXT_semaphore_win32) not available\n");
            return ok;
        }

        bool createSharedImage()
        {
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8G8B8A8_UNORM;
            ici.extent = {static_cast<uint32_t>(g_width), static_cast<uint32_t>(g_height), 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!check(vkCreateImage(g_device, &ici, nullptr, &g_image), "vkCreateImage"))
                return false;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(g_device, g_image, &req);
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.image = g_image;
            VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
            exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(g_device, &mai, nullptr, &g_memory), "vkAllocateMemory"))
                return false;
            if (!check(vkBindImageMemory(g_device, g_image, g_memory, 0), "vkBindImageMemory"))
                return false;
            VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
            hi.memory = g_memory;
            hi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            HANDLE memoryHandle = nullptr;
            if (!check(vkGetMemoryWin32HandleKHR(g_device, &hi, &memoryHandle), "vkGetMemoryWin32HandleKHR"))
                return false;

            // OpenGL: the same memory as a texture.
            gl.createMemoryObjects(1, &g_glMemory);
            const GLint dedicatedFlag = 1;
            gl.memoryObjectParameteriv(g_glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicatedFlag);
            gl.importMemoryWin32Handle(g_glMemory, req.size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, memoryHandle);
            gl.genTextures(1, &g_glTexture);
            gl.bindTexture(GL_TEXTURE_2D, g_glTexture);
            gl.texStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, g_width, g_height, g_glMemory, 0);
            gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.bindTexture(GL_TEXTURE_2D, 0);
            CloseHandle(memoryHandle); // OpenGL holds its own reference

            // Semaphores both ways.
            for (VkSemaphore *s : {&g_vkDone, &g_glDone})
            {
                VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
                es.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &es};
                if (!check(vkCreateSemaphore(g_device, &sci, nullptr, s), "vkCreateSemaphore"))
                    return false;
                VkSemaphoreGetWin32HandleInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
                sh.semaphore = *s;
                sh.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                HANDLE semaphoreHandle = nullptr;
                if (!check(vkGetSemaphoreWin32HandleKHR(g_device, &sh, &semaphoreHandle), "vkGetSemaphoreWin32HandleKHR"))
                    return false;
                GLuint &glSemaphore = s == &g_vkDone ? g_glVkDone : g_glGlDone;
                gl.genSemaphores(1, &glSemaphore);
                gl.importSemaphoreWin32Handle(glSemaphore, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, semaphoreHandle);
                CloseHandle(semaphoreHandle);
            }

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = g_image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return check(vkCreateImageView(g_device, &vci, nullptr, &g_view), "vkCreateImageView");
        }

        bool createPipeline()
        {
            std::vector<uint32_t> spirv;
            if (!compileGlsl(kTestShader, EShLangCompute, spirv))
                return false;
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = spirv.size() * 4u;
            smci.pCode = spirv.data();
            VkShaderModule module;
            if (!check(vkCreateShaderModule(g_device, &smci, nullptr, &module), "vkCreateShaderModule"))
                return false;
            VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dslci.bindingCount = 1;
            dslci.pBindings = &binding;
            vkCreateDescriptorSetLayout(g_device, &dslci, nullptr, &g_setLayout);
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &g_setLayout;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &push;
            vkCreatePipelineLayout(g_device, &plci, nullptr, &g_pipelineLayout);
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
            cpci.layout = g_pipelineLayout;
            const bool ok = check(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &g_pipeline), "vkCreateComputePipelines");
            vkDestroyShaderModule(g_device, module, nullptr);
            if (!ok)
                return false;
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
            VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpci.maxSets = 1;
            dpci.poolSizeCount = 1;
            dpci.pPoolSizes = &size;
            vkCreateDescriptorPool(g_device, &dpci, nullptr, &g_pool);
            VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsai.descriptorPool = g_pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &g_setLayout;
            vkAllocateDescriptorSets(g_device, &dsai, &g_set);
            VkDescriptorImageInfo info{VK_NULL_HANDLE, g_view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = g_set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);

            VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpi.queueFamilyIndex = sharedDevice()->queueFamily;
            vkCreateCommandPool(g_device, &cpi, nullptr, &g_commandPool);
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.commandPool = g_commandPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            vkAllocateCommandBuffers(g_device, &cbai, &g_commands);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            return check(vkCreateFence(g_device, &fci, nullptr, &g_fence), "vkCreateFence");
        }
    }

    const Device *sharedDevice()
    {
        std::lock_guard<std::mutex> lock(g_sharedMutex);
        if (!g_sharedTried)
        {
            g_sharedTried = true;
            if (!createSharedDevice(g_shared))
                g_shared.device = VK_NULL_HANDLE;
        }
        return g_shared.device ? &g_shared : nullptr;
    }

    std::mutex &queueMutex()
    {
        static std::mutex m;
        return m;
    }

    bool rendererUsable(bool ownPresenter)
    {
        const Device *d = sharedDevice();
        if (!d)
            return false;
        if (!d->pushDescriptor || !d->float64 || (!ownPresenter && !d->externalMemory))
        {
            std::fprintf(stderr, "[vk] the Vulkan renderer needs push descriptors, float64 shaders and (shown through OpenGL) external memory "
                                 "(have %d %d %d)\n",
                         d->pushDescriptor, d->float64, d->externalMemory);
            return false;
        }
        // Its own presenter (gs_vk_present.h): frames go to a Vulkan swapchain
        // on the window; OpenGL never touches them.
        if (ownPresenter)
            return true;
        // OpenGL's side of the frame hand-over (EXT_memory_object_win32,
        // EXT_semaphore_win32); without it frames would go through the CPU.
        for (const char *name : {"glImportMemoryWin32HandleEXT", "glTexStorageMem2DEXT", "glImportSemaphoreWin32HandleEXT", "glWaitSemaphoreEXT",
                                 "glSignalSemaphoreEXT"})
        {
            PROC p = wglGetProcAddress(name);
            if (!p || p == reinterpret_cast<PROC>(1) || p == reinterpret_cast<PROC>(2) || p == reinterpret_cast<PROC>(3) ||
                p == reinterpret_cast<PROC>(-1))
            {
                std::fprintf(stderr, "[vk] the OpenGL driver lacks %s (Vulkan frames could not be shown)\n", name);
                return false;
            }
        }
        return true;
    }

    uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags want)
    {
        const VkPhysicalDeviceMemoryProperties &props = g_shared.memory;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }

    bool testInit(int width, int height)
    {
        g_width = width;
        g_height = height;
        const Device *d = sharedDevice();
        if (!d || !d->externalMemory)
            return false;
        g_device = d->device;
        g_queue = d->queue;
        g_deviceName = d->name + (d->sameGpuAsGl ? " (the OpenGL window's GPU)" : "");
        if (!loadGlInterop() || !createSharedImage() || !createPipeline())
        {
            testShutdown();
            return false;
        }
        std::fprintf(stderr, "[vk] Vulkan device: %s; %dx%d image shared with OpenGL\n", g_deviceName.c_str(), width, height);
        return true;
    }

    unsigned testFrame(float time)
    {
        if (!g_device || !g_pipeline)
            return 0;
        vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(g_device, 1, &g_fence);
        vkResetCommandBuffer(g_commands, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_commands, &begin);
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = g_frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = g_image;
        toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(g_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toGeneral);
        vkCmdBindPipeline(g_commands, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipeline);
        vkCmdBindDescriptorSets(g_commands, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipelineLayout, 0, 1, &g_set, 0, nullptr);
        vkCmdPushConstants(g_commands, g_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &time);
        vkCmdDispatch(g_commands, static_cast<uint32_t>((g_width + 15) / 16), static_cast<uint32_t>((g_height + 15) / 16), 1);
        VkImageMemoryBarrier written = toGeneral;
        written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        written.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(g_commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &written);
        vkEndCommandBuffer(g_commands);
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (g_glSignalled)
        {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &g_glDone;
            submit.pWaitDstStageMask = &waitStage;
        }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &g_vkDone;
        if (!check(vkQueueSubmit(g_queue, 1, &submit, g_fence), "vkQueueSubmit"))
            return 0;
        g_glSignalled = false;
        ++g_frame;
        // OpenGL waits for Vulkan's writes before sampling.
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        gl.waitSemaphore(g_glVkDone, 0, nullptr, 1, &g_glTexture, &layout);
        return g_glTexture;
    }

    int testReadbackNonZero()
    {
        if (!g_glTexture)
            return -1;
        void(APIENTRY * getTexImage)(GLenum, GLint, GLenum, GLenum, void *) = nullptr;
        loadGl(getTexImage, "glGetTexImage");
        if (!getTexImage)
            return -2;
        std::vector<uint8_t> pixels(static_cast<size_t>(g_width) * static_cast<size_t>(g_height) * 4u);
        gl.bindTexture(GL_TEXTURE_2D, g_glTexture);
        getTexImage(GL_TEXTURE_2D, 0, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
        gl.bindTexture(GL_TEXTURE_2D, 0);
        int nonZero = 0;
        for (size_t i = 0; i < pixels.size(); i += 97)
            nonZero += pixels[i] != 0;
        return nonZero;
    }

    void testFrameDone()
    {
        if (!g_device || !g_pipeline)
            return;
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        gl.signalSemaphore(g_glGlDone, 0, nullptr, 1, &g_glTexture, &layout);
        gl.flush();
        g_glSignalled = true;
    }

    void testShutdown()
    {
        if (g_device)
        {
            vkDeviceWaitIdle(g_device);
            if (g_fence)
                vkDestroyFence(g_device, g_fence, nullptr);
            if (g_commandPool)
                vkDestroyCommandPool(g_device, g_commandPool, nullptr);
            if (g_pool)
                vkDestroyDescriptorPool(g_device, g_pool, nullptr);
            if (g_pipeline)
                vkDestroyPipeline(g_device, g_pipeline, nullptr);
            if (g_pipelineLayout)
                vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
            if (g_setLayout)
                vkDestroyDescriptorSetLayout(g_device, g_setLayout, nullptr);
            if (g_view)
                vkDestroyImageView(g_device, g_view, nullptr);
            for (VkSemaphore s : {g_vkDone, g_glDone})
                if (s)
                    vkDestroySemaphore(g_device, s, nullptr);
            if (g_image)
                vkDestroyImage(g_device, g_image, nullptr);
            if (g_memory)
                vkFreeMemory(g_device, g_memory, nullptr);
        }
        if (g_glTexture && gl.deleteTextures)
            gl.deleteTextures(1, &g_glTexture);
        g_fence = VK_NULL_HANDLE;
        g_commandPool = VK_NULL_HANDLE;
        g_pool = VK_NULL_HANDLE;
        g_pipeline = VK_NULL_HANDLE;
        g_pipelineLayout = VK_NULL_HANDLE;
        g_setLayout = VK_NULL_HANDLE;
        g_view = VK_NULL_HANDLE;
        g_vkDone = g_glDone = VK_NULL_HANDLE;
        g_image = VK_NULL_HANDLE;
        g_memory = VK_NULL_HANDLE;
        g_device = VK_NULL_HANDLE; // the shared device stays (the renderer's)
        g_glTexture = 0;
    }

    const char *deviceName() { return g_deviceName.c_str(); }

    bool testCompileGsShaders()
    {
        static bool initialised = false;
        if (!initialised)
            initialised = glslang::InitializeProcess();
        // The OpenGL renderer's program variants (see GSGlBackend init), without
        // bindless textures (texture units, auto-mapped to bindings here).
        struct Variant
        {
            const char *name;
            std::string defines;
            std::string extensions;
        };
        const Variant variants[] = {
            {"exact", "", ""},
            {"interlock", "#define GS_INTERLOCK 1\n", "#extension GL_ARB_fragment_shader_interlock : require\n"},
            {"fast", "#define GS_FAST_RASTER 1\n", ""},
            {"lean", "#define GS_FAST_RASTER 1\n#define GS_SPEC_TEX15 1\n", ""},
        };
        bool all = true;
        for (const Variant &v : variants)
        {
            const std::string header = "#version 450 core\n" + v.extensions + "#define GS_TRI_GEOMETRY 1\n" + v.defines + gsgl::kPrimStruct;
            const std::string vs = header + gsgl::kVertexShader;
            const std::string fs = header + gsgl::kFragmentShader;
            glslang::TShader vert(EShLangVertex), frag(EShLangFragment);
            glslang::TShader *shaders[] = {&vert, &frag};
            const std::string *sources[] = {&vs, &fs};
            bool ok = true;
            for (int s = 0; s < 2 && ok; ++s)
            {
                glslang::TShader &sh = *shaders[s];
                const char *text = sources[s]->c_str();
                const EShLanguage stage = s == 0 ? EShLangVertex : EShLangFragment;
                sh.setStrings(&text, 1);
                sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
                sh.setEnvInputVulkanRulesRelaxed();
                sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
                sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
                sh.setAutoMapBindings(true);
                sh.setAutoMapLocations(true);
                sh.setGlobalUniformBlockName("gs_Uniforms");
                sh.setGlobalUniformSet(0);
                sh.setGlobalUniformBinding(15);
                if (!sh.parse(GetDefaultResources(), 450, false, EShMsgDefault))
                {
                    std::fprintf(stderr, "[vk] %s %s shader: %s\n", v.name, s == 0 ? "vertex" : "fragment", sh.getInfoLog());
                    ok = false;
                }
            }
            glslang::TProgram program;
            if (ok)
            {
                program.addShader(&vert);
                program.addShader(&frag);
                ok = program.link(EShMsgDefault) && program.mapIO();
                if (!ok)
                    std::fprintf(stderr, "[vk] %s link: %s\n", v.name, program.getInfoLog());
            }
            if (ok)
            {
                std::vector<uint32_t> spvV, spvF;
                glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), spvV);
                glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spvF);
                program.buildReflection();
                std::string uniforms;
                for (int i = 0; i < program.getNumUniformVariables(); ++i)
                {
                    const glslang::TObjectReflection &u = program.getUniform(i);
                    uniforms += " " + u.name + "@" + std::to_string(u.offset);
                }
                std::fprintf(stderr, "[vk] GS shaders '%s': SPIR-V %zu + %zu words; %d uniforms:%s\n", v.name, spvV.size(), spvF.size(),
                             program.getNumUniformVariables(), uniforms.c_str());
            }
            all &= ok;
        }
        return all;
    }
}
