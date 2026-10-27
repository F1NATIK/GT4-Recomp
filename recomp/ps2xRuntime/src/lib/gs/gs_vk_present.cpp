// The Vulkan renderer's window presenter (gs_vk_present.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // the window, wglGetProcAddress (the overlays' OpenGL read back)

#include "gs_vk_present.h"
#include "gs_vk_present_internal.h"
#include "gs_vk_device.h"
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
#include "../gt4_upscale.h"
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ps2x::scanline
{
    // present_scanline.cpp: the scanline pass's fragment shader (raylib's GLSL 330).
    const char *fragmentSource();
}

namespace gsvk::present
{
    namespace
    {
        // ------------------------------------------------------------------ slots (renderer -> host)

        constexpr int kSlots = 3;
        struct Slot
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            int texWidth = 0, texHeight = 0;
            uint32_t width = 0, height = 0;
            uint64_t value = 0; // renderer timeline value of its frame (0: none yet)
            uint64_t tick = 0;
            int parity = -1, mode = 0;
            uint32_t crt = 0;
            int lines = 0;
            uint64_t hostRead = 0; // host timeline value of the host's last submission reading it
        };
        std::mutex g_slotMutex;
        Slot g_slots[kSlots];
        int g_latest = -1, g_held = -1, g_composing = -1;
        uint64_t g_heldValue = 0;          // host: the value of the frame it holds
        uint64_t g_rendererValue = 0;      // renderer thread
        std::atomic<bool> g_active{false};
        // shutdown() ran: nothing more is shown, but the renderer keeps its slots (it may present
        // until it stops; going back to the OpenGL hand-off then crashed at exit).
        std::atomic<bool> g_shutDown{false};

        // ------------------------------------------------------------------ the presenter (host thread)

        const Device *dev = nullptr;
        VkDevice vk = VK_NULL_HANDLE;
        VkSemaphore g_hostTimeline = VK_NULL_HANDLE, g_rendererTimeline = VK_NULL_HANDLE;
        uint64_t g_hostValue = 0;

        HWND g_hwnd = nullptr;
        bool g_headless = false;
        VkSurfaceKHR g_surface = VK_NULL_HANDLE;
        VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
        VkFormat g_format = VK_FORMAT_UNDEFINED;
        VkPresentModeKHR g_presentMode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D g_extent{0, 0};
        bool g_canShoot = false; // swapchain images allow transfer reads
        std::vector<VkImage> g_images;
        std::vector<VkImageView> g_views;
        std::vector<VkSemaphore> g_renderDone; // per swapchain image
        bool g_recreate = true;
        std::string g_describe = "Vulkan swapchain (not started)";

        constexpr int kInFlight = 2;
        struct HostFrame
        {
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            // Overlay pixels: a staging buffer (host visible) and an image of the
            // overlay framebuffer's size (rows bottom up, as OpenGL's).
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            uint8_t *stagingPtr = nullptr;
            VkDeviceSize stagingSize = 0;
            VkImage overlay = VK_NULL_HANDLE;
            VkDeviceMemory overlayMemory = VK_NULL_HANDLE;
            VkImageView overlayView = VK_NULL_HANDLE;
            int overlayWidth = 0, overlayHeight = 0;
            bool overlayFresh = false; // still UNDEFINED
        } g_frames[kInFlight];
        uint32_t g_frameIndex = 0;
        VkCommandPool g_pool = VK_NULL_HANDLE;

        VkSampler g_nearest = VK_NULL_HANDLE, g_linear = VK_NULL_HANDLE;
        VkDescriptorSetLayout g_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout g_layout = VK_NULL_HANDLE;
        VkShaderModule g_vert = VK_NULL_HANDLE, g_scanFrag = VK_NULL_HANDLE, g_plainFrag = VK_NULL_HANDLE;
        VkPipeline g_scanPipe = VK_NULL_HANDLE, g_plainPipe = VK_NULL_HANDLE, g_overlayPipe = VK_NULL_HANDLE;
        VkFormat g_pipeFormat = VK_FORMAT_UNDEFINED;

        // The LUT (or a 1x1 stand-in).
        VkImage g_lut = VK_NULL_HANDLE;
        VkDeviceMemory g_lutMemory = VK_NULL_HANDLE;
        VkImageView g_lutView = VK_NULL_HANDLE;

        // Shots.
        VkBuffer g_shotBuffer = VK_NULL_HANDLE;
        VkDeviceMemory g_shotMemory = VK_NULL_HANDLE;
        uint8_t *g_shotPtr = nullptr;
        VkDeviceSize g_shotSize = 0;
        std::vector<uint8_t> g_shot;
        int g_shotWidth = 0, g_shotHeight = 0;
        bool g_shotReady = false;

        // This frame's drawing.
        struct Push
        {
            float rect[4];
            float uv[4];
            float colDiffuse[4];
            float texSize[2];
            int32_t rowsPerLine, lines, cols;
            float halfPixel, mixBeam, widthK, field;
            int32_t parity, filterMode, blendLines;
            float lutSize, lutMix;
        };
        static_assert(sizeof(Push) == 104, "the shaders' push constant block");
        struct Draw
        {
            bool scan = false, linear = true;
            float x, y, w, h;
            Push push{};
        };
        std::vector<Draw> g_draws;
        struct Rect
        {
            int x, y, w, h;
        };
        std::vector<Rect> g_overlay;
        bool g_overlayAll = false;

        // Timing (GT4_VK_PRESENT_LOG=1): per 5 s.
        bool g_log = false;
        double g_presentMs = 0.0, g_presentMaxMs = 0.0, g_readMs = 0.0;
        uint64_t g_presents = 0, g_overlayBytes = 0;
        std::chrono::steady_clock::time_point g_logStart;

        bool check(VkResult r, const char *what)
        {
            if (r == VK_SUCCESS)
                return true;
            std::fprintf(stderr, "[vk-present] %s failed: %d\n", what, static_cast<int>(r));
            return false;
        }

        // ------------------------------------------------------------------ OpenGL (the overlay read back)

        using GLenum = unsigned int;
        using GLint = int;
        using GLuint = unsigned int;
        using GLsizei = int;
        struct Gl
        {
            void(APIENTRY *readPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;
            void(APIENTRY *bindFramebuffer)(GLenum, GLuint) = nullptr;
            void(APIENTRY *getIntegerv)(GLenum, GLint *) = nullptr;
            void(APIENTRY *pixelStorei)(GLenum, GLint) = nullptr;
        } gl;
        constexpr GLenum GL_READ_FRAMEBUFFER = 0x8CA8, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401,
                         GL_PACK_ALIGNMENT = 0x0D05, GL_PACK_ROW_LENGTH = 0x0D02;

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

        // ------------------------------------------------------------------ shaders

        const char kPushBlock[] = R"GLSL(layout(push_constant) uniform Push
{
    vec4 rect;       // window rectangle, normalized device coordinates (x0, y0, x1, y1)
    vec4 uv;         // texture rectangle (u0, v0, u1, v1)
    vec4 colDiffuse;
    vec2 texSize;
    int rowsPerLine;
    int lines;
    int cols;
    float halfPixel;
    float mixBeam;
    float widthK;
    float field;
    int parity;
    int filterMode;
    int blendLines;
    float lutSize;
    float lutMix;
};
)GLSL";

        const char kVertex[] = R"GLSL(
layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
void main()
{
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, c), 0.0, 1.0);
    fragTexCoord = mix(uv.xy, uv.zw, c);
    fragColor = vec4(1.0);
}
)GLSL";

        const char kFragmentHeader[] = R"GLSL(
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 finalColor;
layout(set = 0, binding = 0) uniform sampler2D texture0;
layout(set = 0, binding = 1) uniform sampler2D lutTex;
)GLSL";

        const char kPlainBody[] = R"GLSL(
void main()
{
    finalColor = texture(texture0, fragTexCoord) * colDiffuse;
}
)GLSL";

        bool compile(const std::string &source, EShLanguage stage, VkShaderModule &module, const char *what)
        {
            static bool initialised = false;
            if (!initialised)
                initialised = glslang::InitializeProcess();
            glslang::TShader shader(stage);
            const char *text = source.c_str();
            shader.setStrings(&text, 1);
            shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
            if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault))
            {
                std::fprintf(stderr, "[vk-present] %s shader: %s\n", what, shader.getInfoLog());
                return false;
            }
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(EShMsgDefault))
            {
                std::fprintf(stderr, "[vk-present] %s link: %s\n", what, program.getInfoLog());
                return false;
            }
            std::vector<uint32_t> spirv;
            glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = spirv.size() * 4u;
            smci.pCode = spirv.data();
            return check(vkCreateShaderModule(vk, &smci, nullptr, &module), what);
        }

        // The scanline pass's raylib shader for Vulkan: its #version, in / out
        // and uniform lines replaced by the push constant block and bindings
        // above (its body unchanged).
        std::string scanFragment()
        {
            std::string body;
            std::istringstream in(ps2x::scanline::fragmentSource());
            for (std::string line; std::getline(in, line);)
            {
                size_t i = line.find_first_not_of(" \t");
                const std::string t = i == std::string::npos ? std::string() : line.substr(i);
                if (t.rfind("#version", 0) == 0 || t.rfind("in ", 0) == 0 || t.rfind("out ", 0) == 0 || t.rfind("uniform ", 0) == 0)
                    continue;
                body += line;
                body += '\n';
            }
            return std::string("#version 450\n") + kPushBlock + kFragmentHeader + body;
        }

        bool makeShaders()
        {
            return compile(std::string("#version 450\n") + kPushBlock + kVertex, EShLangVertex, g_vert, "vertex") &&
                   compile(scanFragment(), EShLangFragment, g_scanFrag, "scanline pass") &&
                   compile(std::string("#version 450\n") + kPushBlock + kFragmentHeader + kPlainBody, EShLangFragment, g_plainFrag, "picture");
        }

        VkPipeline makePipeline(VkShaderModule frag, bool premultiplied)
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_vert, "main", nullptr};
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};
            // As raylib draws (BLEND_ALPHA over the black window), or the
            // overlay's premultiplied pixels (src + dst * (1 - src alpha)).
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &blend;
            const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            ds.dynamicStateCount = 2;
            ds.pDynamicStates = dynamic;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &g_format;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            gp.stageCount = 2;
            gp.pStages = stages;
            gp.pVertexInputState = &vi;
            gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vp;
            gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms;
            gp.pColorBlendState = &cb;
            gp.pDynamicState = &ds;
            gp.layout = g_layout;
            VkPipeline pipe = VK_NULL_HANDLE;
            check(vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe), "vkCreateGraphicsPipelines");
            return pipe;
        }

        bool makePipelines()
        {
            if (g_pipeFormat == g_format && g_scanPipe)
                return true;
            for (VkPipeline *p : {&g_scanPipe, &g_plainPipe, &g_overlayPipe})
                if (*p)
                {
                    vkDestroyPipeline(vk, *p, nullptr);
                    *p = VK_NULL_HANDLE;
                }
            g_scanPipe = makePipeline(g_scanFrag, false);
            g_plainPipe = makePipeline(g_plainFrag, false);
            g_overlayPipe = makePipeline(g_plainFrag, true);
            g_pipeFormat = g_format;
            return g_scanPipe && g_plainPipe && g_overlayPipe;
        }

        bool makeLayout()
        {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            if (!check(vkCreateSampler(vk, &sci, nullptr, &g_nearest), "vkCreateSampler"))
                return false;
            sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
            if (!check(vkCreateSampler(vk, &sci, nullptr, &g_linear), "vkCreateSampler"))
                return false;
            VkDescriptorSetLayoutBinding bindings[2] = {
                {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            };
            VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dsl.bindingCount = 2;
            dsl.pBindings = bindings;
            if (!check(vkCreateDescriptorSetLayout(vk, &dsl, nullptr, &g_setLayout), "vkCreateDescriptorSetLayout"))
                return false;
            VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount = 1;
            pl.pSetLayouts = &g_setLayout;
            pl.pushConstantRangeCount = 1;
            pl.pPushConstantRanges = &range;
            return check(vkCreatePipelineLayout(vk, &pl, nullptr, &g_layout), "vkCreatePipelineLayout");
        }

        // ------------------------------------------------------------------ memory

        bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buffer, VkDeviceMemory &memory, uint8_t *&ptr)
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = size;
            bci.usage = usage;
            if (!check(vkCreateBuffer(vk, &bci, nullptr, &buffer), "vkCreateBuffer"))
                return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(vk, buffer, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            // Cached where the GPU writes and the CPU reads (shots).
            mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                     ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0));
            if (mai.memoryTypeIndex == UINT32_MAX)
                mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(vk, &mai, nullptr, &memory), "vkAllocateMemory"))
                return false;
            vkBindBufferMemory(vk, buffer, memory, 0);
            return check(vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(&ptr)), "vkMapMemory");
        }

        void freeBuffer(VkBuffer &buffer, VkDeviceMemory &memory)
        {
            if (buffer)
                vkDestroyBuffer(vk, buffer, nullptr);
            if (memory)
                vkFreeMemory(vk, memory, nullptr);
            buffer = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        }

        bool makeImage(VkFormat format, int width, int height, VkImageUsageFlags usage, VkImage &image, VkDeviceMemory &memory, VkImageView &view)
        {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = format;
            ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = usage;
            if (!check(vkCreateImage(vk, &ici, nullptr, &image), "vkCreateImage"))
                return false;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(vk, image, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX || !check(vkAllocateMemory(vk, &mai, nullptr, &memory), "vkAllocateMemory"))
                return false;
            vkBindImageMemory(vk, image, memory, 0);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            return check(vkCreateImageView(vk, &vci, nullptr, &view), "vkCreateImageView");
        }

        void freeImage(VkImage &image, VkDeviceMemory &memory, VkImageView &view)
        {
            if (view)
                vkDestroyImageView(vk, view, nullptr);
            if (image)
                vkDestroyImage(vk, image, nullptr);
            if (memory)
                vkFreeMemory(vk, memory, nullptr);
            view = VK_NULL_HANDLE;
            image = VK_NULL_HANDLE;
            memory = VK_NULL_HANDLE;
        }

        void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
        {
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = srcStage;
            ib.srcAccessMask = srcAccess;
            ib.dstStageMask = dstStage;
            ib.dstAccessMask = dstAccess;
            ib.oldLayout = from;
            ib.newLayout = to;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = image;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cb, &di);
        }

        // One command buffer run to the end (set-up work: the LUT).
        bool runOnce(const std::function<void(VkCommandBuffer)> &record)
        {
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.commandPool = g_pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            if (!check(vkAllocateCommandBuffers(vk, &cbai, &cb), "vkAllocateCommandBuffers"))
                return false;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &begin);
            record(cb);
            vkEndCommandBuffer(cb);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE;
            vkCreateFence(vk, &fci, nullptr, &fence);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            VkResult r;
            {
                std::lock_guard<std::mutex> lock(queueMutex());
                r = vkQueueSubmit(dev->queue, 1, &si, fence);
            }
            if (r == VK_SUCCESS)
                vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
            vkDestroyFence(vk, fence, nullptr);
            vkFreeCommandBuffers(vk, g_pool, 1, &cb);
            return check(r, "vkQueueSubmit (set-up)");
        }

        uint16_t toHalf(float f)
        {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int exp = static_cast<int>((x >> 23) & 0xFFu) - 127 + 15;
            uint32_t mant = x & 0x7FFFFFu;
            if (exp <= 0)
                return static_cast<uint16_t>(sign);
            if (exp >= 31)
                return static_cast<uint16_t>(sign | 0x7C00u);
            mant += 0x1000u; // round to nearest
            if (mant & 0x800000u)
            {
                mant = 0;
                if (exp + 1 >= 31)
                    return static_cast<uint16_t>(sign | 0x7C00u);
                return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp + 1) << 10));
            }
            return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13));
        }

        // The LUT image from a strip (n*n x n RGB floats), or a 1x1 stand-in.
        bool makeLut(const float *strip, int n)
        {
            if (g_lut)
            {
                {
                    std::lock_guard<std::mutex> lock(queueMutex());
                    vkQueueWaitIdle(dev->queue);
                }
                freeImage(g_lut, g_lutMemory, g_lutView);
            }
            const int w = strip ? n * n : 1, h = strip ? n : 1;
            if (!makeImage(VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, g_lut, g_lutMemory, g_lutView))
                return false;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint8_t *ptr = nullptr;
            const size_t texels = static_cast<size_t>(w) * static_cast<size_t>(h);
            if (!makeBuffer(texels * 8u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, buffer, memory, ptr))
                return false;
            uint16_t *out = reinterpret_cast<uint16_t *>(ptr);
            for (size_t i = 0; i < texels; ++i)
            {
                out[i * 4 + 0] = toHalf(strip ? strip[i * 3 + 0] : 0.0f);
                out[i * 4 + 1] = toHalf(strip ? strip[i * 3 + 1] : 0.0f);
                out[i * 4 + 2] = toHalf(strip ? strip[i * 3 + 2] : 0.0f);
                out[i * 4 + 3] = toHalf(1.0f);
            }
            const bool ok = runOnce([&](VkCommandBuffer cb) {
                imageBarrier(cb, g_lut, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
                vkCmdCopyBufferToImage(cb, buffer, g_lut, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                imageBarrier(cb, g_lut, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            });
            freeBuffer(buffer, memory);
            return ok;
        }

        // ------------------------------------------------------------------ swapchain

        void destroySwapchainViews()
        {
            for (VkImageView v : g_views)
                vkDestroyImageView(vk, v, nullptr);
            for (VkSemaphore s : g_renderDone)
                vkDestroySemaphore(vk, s, nullptr);
            g_views.clear();
            g_renderDone.clear();
            g_images.clear();
        }

        // (Re)makes the swapchain for the window's size now. False: none now
        // (minimized: tried again next frame).
        bool makeSwapchain()
        {
            VkSurfaceCapabilitiesKHR caps{};
            if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physical, g_surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
                return false;
            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)
            {
                RECT r{};
                GetClientRect(g_hwnd, &r);
                extent = {static_cast<uint32_t>(std::max<LONG>(0, r.right - r.left)), static_cast<uint32_t>(std::max<LONG>(0, r.bottom - r.top))};
            }
            if (extent.width == 0 || extent.height == 0)
                return false;
            {
                // Every use of the old swapchain's images done (the renderer's work goes on meanwhile).
                std::lock_guard<std::mutex> lock(queueMutex());
                vkQueueWaitIdle(dev->queue);
            }
            destroySwapchainViews();
            uint32_t images = std::max<uint32_t>(caps.minImageCount, 3u);
            if (caps.maxImageCount)
                images = std::min(images, caps.maxImageCount);
            VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            sci.surface = g_surface;
            sci.minImageCount = images;
            sci.imageFormat = g_format;
            sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            sci.imageExtent = extent;
            sci.imageArrayLayers = 1;
            g_canShoot = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
            sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (g_canShoot ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                  : caps.currentTransform;
            sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                    : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            sci.presentMode = g_presentMode;
            sci.clipped = VK_TRUE;
            VkSwapchainKHR old = g_swapchain;
            sci.oldSwapchain = old;
            VkSwapchainKHR made = VK_NULL_HANDLE;
            const VkResult r = vkCreateSwapchainKHR(vk, &sci, nullptr, &made);
            if (old)
                vkDestroySwapchainKHR(vk, old, nullptr);
            g_swapchain = VK_NULL_HANDLE;
            if (!check(r, "vkCreateSwapchainKHR"))
                return false;
            g_swapchain = made;
            g_extent = extent;
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(vk, g_swapchain, &count, nullptr);
            g_images.resize(count);
            vkGetSwapchainImagesKHR(vk, g_swapchain, &count, g_images.data());
            for (VkImage image : g_images)
            {
                VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vci.image = image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = g_format;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageView view = VK_NULL_HANDLE;
                vkCreateImageView(vk, &vci, nullptr, &view);
                g_views.push_back(view);
                VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                VkSemaphore s = VK_NULL_HANDLE;
                vkCreateSemaphore(vk, &semi, nullptr, &s);
                g_renderDone.push_back(s);
            }
            const char *mode = g_presentMode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
                               : g_presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                               : g_presentMode == VK_PRESENT_MODE_FIFO_KHR      ? "fifo"
                                                                                : "other";
            char text[160];
            std::snprintf(text, sizeof(text), "Vulkan swapchain %ux%u, %s, %u images", extent.width, extent.height, mode, count);
            g_describe = text;
            std::fprintf(stderr, "[vk-present] %s (%s)\n", text, g_format == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8");
            g_recreate = false;
            return makePipelines();
        }

        bool makeOverlayImage(HostFrame &f, int width, int height)
        {
            if (f.overlayWidth == width && f.overlayHeight == height && f.overlay)
                return true;
            freeImage(f.overlay, f.overlayMemory, f.overlayView);
            f.overlayWidth = f.overlayHeight = 0;
            if (!makeImage(VK_FORMAT_R8G8B8A8_UNORM, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, f.overlay,
                           f.overlayMemory, f.overlayView))
                return false;
            f.overlayWidth = width;
            f.overlayHeight = height;
            f.overlayFresh = true;
            return true;
        }

        void logTiming()
        {
            if (!g_log)
                return;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - g_logStart).count();
            if (secs < 5.0)
                return;
            std::fprintf(stderr, "[vk-present] %.1f presents/s: submit+present %.2f ms (max %.2f), overlay read %.2f ms, %.1f KB overlay a frame\n",
                         static_cast<double>(g_presents) / secs, g_presents ? g_presentMs / static_cast<double>(g_presents) : 0.0, g_presentMaxMs,
                         g_presents ? g_readMs / static_cast<double>(g_presents) : 0.0,
                         g_presents ? static_cast<double>(g_overlayBytes) / static_cast<double>(g_presents) / 1024.0 : 0.0);
            g_logStart = now;
            g_presents = 0;
            g_presentMs = g_presentMaxMs = g_readMs = 0.0;
            g_overlayBytes = 0;
        }
    }

    // ---------------------------------------------------------------------- renderer side

    int rendererSlot(uint64_t &hostWaitValue)
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        int chosen = -1;
        for (int i = 1; i <= kSlots; ++i)
        {
            const int s = ((g_latest < 0 ? 0 : g_latest) + i) % kSlots;
            if (s != g_held && s != g_latest)
            {
                chosen = s;
                break;
            }
        }
        g_composing = chosen;
        hostWaitValue = g_slots[chosen].hostRead;
        return chosen;
    }

    VkSemaphore hostTimeline() { return g_hostTimeline; }
    VkSemaphore rendererTimeline() { return g_rendererTimeline; }
    uint64_t rendererNextValue() { return ++g_rendererValue; }

    void rendererPublish(int slot, VkImage image, VkImageView view, int texWidth, int texHeight, uint32_t width, uint32_t height,
                         uint64_t timelineValue, uint64_t tick, int parity, int mode, uint32_t crt, int lines)
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        Slot &s = g_slots[slot];
        s.image = image;
        s.view = view;
        s.texWidth = texWidth;
        s.texHeight = texHeight;
        s.width = width;
        s.height = height;
        s.value = timelineValue;
        s.tick = tick;
        s.parity = parity == 0 || parity == 1 ? parity : -1;
        s.mode = mode;
        s.crt = crt;
        s.lines = lines;
        g_latest = slot;
        g_composing = -1;
    }

    // ---------------------------------------------------------------------- host side

    bool wanted()
    {
        const Device *d = sharedDevice();
        const char *v = std::getenv("GT4_VK_PRESENT");
        const std::string choice = (v && *v) ? v : "auto";
        if (choice == "gl")
        {
            std::fprintf(stderr, "[vk-present] GT4_VK_PRESENT=gl: frames shown through OpenGL\n");
            return false;
        }
        if (!d)
            return false;
        if (!d->swapchain || !d->timeline || !d->pushDescriptor)
        {
            std::fprintf(stderr, "[vk-present] not available (swapchain %d, timeline semaphores %d, push descriptors %d): frames shown through OpenGL\n",
                         d->swapchain, d->timeline, d->pushDescriptor);
            return false;
        }
        if (choice == "window")
            return true;
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // GT4's DLSS: Streamline sees each frame at the swapchain's present.
        if (gt4up::wanted())
        {
            std::fprintf(stderr, "[vk-present] DLSS: frames shown through the Vulkan swapchain\n");
            return true;
        }
#endif
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
        // auto (GT4 Full): Vulkan on every card. NVIDIA's swapchain goes on a child window over the game's, which
        // OpenGL never touches (start); if it cannot start, the frames go through OpenGL as before.
        return true;
#else
        // auto: NVIDIA keeps the OpenGL presentation (its interop works there and
        // is what was tested); any other card presents with Vulkan.
        const bool nvidia = d->properties.vendorID == 0x10DE;
        if (nvidia)
            std::fprintf(stderr, "[vk-present] NVIDIA card: frames shown through OpenGL (GT4_VK_PRESENT=window: the Vulkan swapchain)\n");
        return !nvidia;
#endif
    }

    // NVIDIA refuses a Vulkan swapchain on a window that has an OpenGL pixel format (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR):
    // there the swapchain goes on a child window covering the game window's client area. It takes no input (hit tests
    // pass through to the game window) and follows the game window's size (presentFrame).
    namespace
    {
        HWND g_parent = nullptr, g_child = nullptr;
        LRESULT CALLBACK childProc(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            switch (m)
            {
            case WM_NCHITTEST: return HTTRANSPARENT;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: ValidateRect(h, nullptr); return 0;
            default: return DefWindowProcW(h, m, w, l);
            }
        }
        HWND makeChildWindow(HWND parent)
        {
            static const ATOM cls = [] {
                WNDCLASSW wc{};
                wc.lpfnWndProc = childProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"GT4VulkanPresent";
                return RegisterClassW(&wc);
            }();
            if (!cls)
                return nullptr;
            RECT r{};
            GetClientRect(parent, &r);
            return CreateWindowExW(0, L"GT4VulkanPresent", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, std::max<LONG>(1, r.right - r.left),
                                   std::max<LONG>(1, r.bottom - r.top), parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
    } // namespace

    bool start(void *hwnd, bool headless)
    {
        if (g_active.load())
            return true;
        dev = sharedDevice();
        if (!dev || !hwnd)
            return false;
        vk = dev->device;
        g_hwnd = static_cast<HWND>(hwnd);
        g_headless = headless;
        // (NVIDIA, or GT4_VK_PRESENT_CHILD=1: the swapchain on a child window, see makeChildWindow)
        {
            const char *c = std::getenv("GT4_VK_PRESENT_CHILD");
            const bool child = c && *c ? std::strcmp(c, "1") == 0 : dev->properties.vendorID == 0x10DE;
            if (child)
            {
                g_parent = g_hwnd;
                g_child = makeChildWindow(g_parent);
                if (!g_child)
                {
                    std::fprintf(stderr, "[vk-present] the child window for the swapchain failed (error %lu)\n", GetLastError());
                    return false;
                }
                g_hwnd = g_child;
                std::fprintf(stderr, "[vk-present] swapchain on a child window over the game window (OpenGL's)\n");
            }
        }
        g_log = [] {
            const char *v = std::getenv("GT4_VK_PRESENT_LOG");
            return v && *v == '1';
        }();
        g_logStart = std::chrono::steady_clock::now();
        if (!loadGl(gl.readPixels, "glReadPixels") || !loadGl(gl.bindFramebuffer, "glBindFramebuffer") || !loadGl(gl.getIntegerv, "glGetIntegerv") ||
            !loadGl(gl.pixelStorei, "glPixelStorei"))
        {
            std::fprintf(stderr, "[vk-present] OpenGL entry points for the overlays missing\n");
            return false;
        }
        VkWin32SurfaceCreateInfoKHR wci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        wci.hinstance = GetModuleHandleA(nullptr);
        wci.hwnd = g_hwnd;
        if (!check(vkCreateWin32SurfaceKHR(dev->instance, &wci, nullptr, &g_surface), "vkCreateWin32SurfaceKHR"))
            return false;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev->physical, dev->queueFamily, g_surface, &supported);
        if (!supported)
        {
            std::fprintf(stderr, "[vk-present] the renderer's queue cannot present to the window\n");
            shutdown();
            return false;
        }
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, g_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical, g_surface, &count, formats.data());
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
            for (const VkSurfaceFormatKHR &f : formats)
                if (g_format == VK_FORMAT_UNDEFINED && f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    g_format = want;
        if (g_format == VK_FORMAT_UNDEFINED)
        {
            std::fprintf(stderr, "[vk-present] the window offers no 8-bit UNORM format\n");
            shutdown();
            return false;
        }
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, g_surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physical, g_surface, &count, modes.data());
        auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
        const char *modeEnv = std::getenv("GT4_VK_PRESENT_MODE");
        const std::string modeWant = (modeEnv && *modeEnv) ? modeEnv : "";
        if (modeWant == "fifo")
            g_presentMode = VK_PRESENT_MODE_FIFO_KHR;
        else if (modeWant == "immediate" && has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            g_presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (has(VK_PRESENT_MODE_MAILBOX_KHR) && modeWant != "immediate")
            g_presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR))
            g_presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else
            g_presentMode = VK_PRESENT_MODE_FIFO_KHR;

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = dev->queueFamily;
        if (!check(vkCreateCommandPool(vk, &cpi, nullptr, &g_pool), "vkCreateCommandPool"))
        {
            shutdown();
            return false;
        }
        for (HostFrame &f : g_frames)
        {
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.commandPool = g_pool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            vkAllocateCommandBuffers(vk, &cbai, &f.cb);
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(vk, &fci, nullptr, &f.fence);
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(vk, &semi, nullptr, &f.acquired);
        }
        for (VkSemaphore *s : {&g_hostTimeline, &g_rendererTimeline})
        {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            if (!check(vkCreateSemaphore(vk, &semi, nullptr, s), "vkCreateSemaphore (timeline)"))
            {
                shutdown();
                return false;
            }
        }
        if (!makeLayout() || !makeShaders() || !makeLut(nullptr, 0) || !makeSwapchain())
        {
            std::fprintf(stderr, "[vk-present] could not start: frames shown through OpenGL\n");
            shutdown();
            return false;
        }
        g_active.store(true);
        std::fprintf(stderr, "[vk-present] frames go from the Vulkan renderer to the window's own Vulkan swapchain (no OpenGL sharing)%s\n",
                     headless ? "; hidden window" : "");
        return true;
    }

    bool active() { return g_active.load(std::memory_order_acquire); }

    const char *describe() { return g_describe.c_str(); }

    void shutdown()
    {
        if (!vk)
            return;
        {
            std::lock_guard<std::mutex> lock(queueMutex());
            vkQueueWaitIdle(dev->queue);
        }
        g_shutDown.store(true);
        destroySwapchainViews();
        if (g_swapchain)
            vkDestroySwapchainKHR(vk, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        if (g_surface)
            vkDestroySurfaceKHR(dev->instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
        // (The rest lives until the process ends: the renderer may still wait on the timelines.)
    }

    int takeLatest(uint64_t maxTick, Frame &frame)
    {
        std::lock_guard<std::mutex> lock(g_slotMutex);
        auto describeSlot = [&](int s) {
            const Slot &slot = g_slots[s];
            frame.width = slot.width;
            frame.height = slot.height;
            frame.texWidth = slot.texWidth;
            frame.texHeight = slot.texHeight;
            frame.parity = slot.parity;
            frame.mode = slot.mode;
            frame.crt = slot.crt;
            frame.lines = slot.lines;
        };
        if (g_latest < 0 || g_slots[g_latest].value == g_heldValue)
        {
            if (g_held < 0)
                return -1;
            describeSlot(g_held);
            return 0;
        }
        int s = g_latest;
        if (maxTick != ~0ull && g_slots[s].tick > maxTick)
        {
            // Published early (grid pacing): an older frame not shown yet whose
            // latch tick is due, if one is still there.
            int best = -1;
            for (int i = 0; i < kSlots; ++i)
            {
                const Slot &c = g_slots[i];
                if (i == g_composing || c.value == 0 || c.value <= g_heldValue || c.tick > maxTick)
                    continue;
                if (best < 0 || c.value > g_slots[best].value)
                    best = i;
            }
            if (best >= 0)
                s = best;
        }
        g_held = s;
        g_heldValue = g_slots[s].value;
        describeSlot(s);
        return 1;
    }

    void drawScan(const ScanUniforms &u, float srcW, float srcH, float x, float y, float w, float h)
    {
        Draw d;
        d.scan = true;
        d.x = x, d.y = y, d.w = w, d.h = h;
        Push &p = d.push;
        // fragTexCoord as raylib gives it: texture coordinates of the whole texture.
        p.uv[0] = 0.0f;
        p.uv[1] = 0.0f;
        p.uv[2] = u.texSize[0] > 0.0f ? srcW / u.texSize[0] : 1.0f;
        p.uv[3] = u.texSize[1] > 0.0f ? srcH / u.texSize[1] : 1.0f;
        for (float &c : p.colDiffuse)
            c = 1.0f;
        p.texSize[0] = u.texSize[0];
        p.texSize[1] = u.texSize[1];
        p.rowsPerLine = u.rowsPerLine;
        p.lines = u.lines;
        p.cols = u.cols;
        p.halfPixel = u.halfPixel;
        p.mixBeam = u.mixBeam;
        p.widthK = u.widthK;
        p.field = u.field;
        p.parity = u.parity;
        p.filterMode = u.filterMode;
        p.blendLines = u.blendLines;
        p.lutSize = u.lutSize;
        p.lutMix = g_lutView && u.lutMix > 0.0f ? u.lutMix : 0.0f;
        g_draws.push_back(d);
    }

    void drawPlain(float u0, float v0, float u1, float v1, float x, float y, float w, float h, bool linear)
    {
        Draw d;
        d.scan = false;
        d.linear = linear;
        d.x = x, d.y = y, d.w = w, d.h = h;
        // Texels: made relative to the held image at presentation.
        d.push.uv[0] = u0;
        d.push.uv[1] = v0;
        d.push.uv[2] = u1;
        d.push.uv[3] = v1;
        for (float &c : d.push.colDiffuse)
            c = 1.0f;
        g_draws.push_back(d);
    }

    void setLut(const float *strip, int points)
    {
        if (!active())
            return;
        makeLut(strip && points >= 2 ? strip : nullptr, points);
    }

    void overlayRect(float x, float y, float w, float h)
    {
        if (w >= 1e8f || h >= 1e8f)
        {
            g_overlayAll = true;
            return;
        }
        const int x0 = static_cast<int>(std::floor(x)) - 1, y0 = static_cast<int>(std::floor(y)) - 1;
        const int x1 = static_cast<int>(std::ceil(x + w)) + 1, y1 = static_cast<int>(std::ceil(y + h)) + 1;
        if (x1 > x0 && y1 > y0)
            g_overlay.push_back(Rect{x0, y0, x1 - x0, y1 - y0});
    }

    void presentFrame(unsigned glFramebuffer, int fbWidth, int fbHeight, int windowWidth, int windowHeight, bool shot)
    {
        std::vector<Draw> draws;
        draws.swap(g_draws);
        std::vector<Rect> rects;
        rects.swap(g_overlay);
        const bool overlayAll = g_overlayAll;
        g_overlayAll = false;
        if (!active() || g_shutDown.load())
            return;
        const auto t0 = std::chrono::steady_clock::now();
        // The window changed size (resize, full screen): a swapchain of the new size.
        static int lastWidth = 0, lastHeight = 0;
        if (windowWidth != lastWidth || windowHeight != lastHeight)
        {
            if (lastWidth != 0)
                g_recreate = true;
            lastWidth = windowWidth;
            lastHeight = windowHeight;
            // (the child window, if any, follows the game window's client area; asynchronously, never waiting on its thread)
            if (g_child && g_parent)
            {
                RECT r{};
                GetClientRect(g_parent, &r);
                SetWindowPos(g_child, nullptr, 0, 0, std::max<LONG>(1, r.right - r.left), std::max<LONG>(1, r.bottom - r.top),
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
        }
        if (g_recreate || !g_swapchain)
        {
            if (!makeSwapchain())
                return; // minimized: nothing to show
        }
        HostFrame &f = g_frames[g_frameIndex];
        vkWaitForFences(vk, 1, &f.fence, VK_TRUE, UINT64_MAX);

        // The overlays: their rectangles out of the OpenGL framebuffer (rows bottom up).
        if (overlayAll && fbWidth > 0 && fbHeight > 0)
        {
            rects.clear();
            rects.push_back(Rect{0, 0, fbWidth, fbHeight});
        }
        std::vector<Rect> clipped;
        VkDeviceSize bytes = 0;
        for (const Rect &r : rects)
        {
            const int x0 = std::clamp(r.x, 0, fbWidth), y0 = std::clamp(r.y, 0, fbHeight);
            const int x1 = std::clamp(r.x + r.w, 0, fbWidth), y1 = std::clamp(r.y + r.h, 0, fbHeight);
            if (x1 > x0 && y1 > y0)
            {
                clipped.push_back(Rect{x0, y0, x1 - x0, y1 - y0});
                bytes += static_cast<VkDeviceSize>(x1 - x0) * static_cast<VkDeviceSize>(y1 - y0) * 4u;
            }
        }
        if (!clipped.empty() && !makeOverlayImage(f, fbWidth, fbHeight))
            clipped.clear();
        if (bytes > f.stagingSize)
        {
            freeBuffer(f.staging, f.stagingMemory);
            f.stagingSize = 0;
            const VkDeviceSize size = std::max<VkDeviceSize>(bytes + (bytes >> 1), 256u << 10);
            if (makeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, f.staging, f.stagingMemory, f.stagingPtr))
                f.stagingSize = size;
            else
                clipped.clear();
        }
        std::vector<VkBufferImageCopy> copies;
        if (!clipped.empty())
        {
            const auto r0 = std::chrono::steady_clock::now();
            GLint previous = 0;
            gl.getIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER, glFramebuffer);
            gl.pixelStorei(GL_PACK_ALIGNMENT, 4);
            gl.pixelStorei(GL_PACK_ROW_LENGTH, 0);
            VkDeviceSize offset = 0;
            for (const Rect &r : clipped)
            {
                const int glY = fbHeight - r.y - r.h; // OpenGL's rows from the bottom
                gl.readPixels(r.x, glY, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, f.stagingPtr + offset);
                VkBufferImageCopy c{};
                c.bufferOffset = offset;
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageOffset = {r.x, glY, 0};
                c.imageExtent = {static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h), 1};
                copies.push_back(c);
                offset += static_cast<VkDeviceSize>(r.w) * static_cast<VkDeviceSize>(r.h) * 4u;
            }
            gl.bindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
            g_readMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
            g_overlayBytes += bytes;
        }

        uint32_t index = 0;
        VkResult acquired = vkAcquireNextImageKHR(vk, g_swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            g_recreate = true;
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
        {
            check(acquired, "vkAcquireNextImageKHR");
            g_recreate = true;
            return;
        }
        if (acquired == VK_SUBOPTIMAL_KHR)
            g_recreate = true; // after this frame

        // The held picture (if any): its image, size and the renderer's value.
        Slot held{};
        bool haveHeld = false;
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            if (g_held >= 0 && g_slots[g_held].view)
            {
                held = g_slots[g_held];
                haveHeld = true;
            }
        }

        vkResetFences(vk, 1, &f.fence);
        vkResetCommandBuffer(f.cb, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.cb, &begin);
        if (!copies.empty())
        {
            // This frame's overlay image: the previous use of it (two frames ago) is done (its fence).
            imageBarrier(f.cb, f.overlay, f.overlayFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            f.overlayFresh = false;
            vkCmdCopyBufferToImage(f.cb, f.staging, f.overlay, VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(copies.size()), copies.data());
            imageBarrier(f.cb, f.overlay, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        const VkImage target = g_images[index];
        imageBarrier(f.cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = g_views[index];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, g_extent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(f.cb, &ri);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(g_extent.width), static_cast<float>(g_extent.height), 0.0f, 1.0f};
        vkCmdSetViewport(f.cb, 0, 1, &viewport);
        const VkRect2D full{{0, 0}, g_extent};
        vkCmdSetScissor(f.cb, 0, 1, &full);
        const float ww = static_cast<float>(std::max(1, windowWidth)), wh = static_cast<float>(std::max(1, windowHeight));
        auto toNdc = [&](float x, float y, float w, float h, float out[4]) {
            out[0] = 2.0f * x / ww - 1.0f;
            out[1] = 2.0f * y / wh - 1.0f;
            out[2] = 2.0f * (x + w) / ww - 1.0f;
            out[3] = 2.0f * (y + h) / wh - 1.0f;
        };
        auto pushTextures = [&](VkImageView picture, VkSampler sampler) {
            VkDescriptorImageInfo images[2] = {{sampler, picture, VK_IMAGE_LAYOUT_GENERAL}, {g_linear, g_lutView, VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &images[i];
            }
            vkCmdPushDescriptorSetKHR(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 2, writes);
        };
        bool pictureDrawn = false;
        if (haveHeld)
            for (Draw &d : draws)
            {
                Push p = d.push;
                toNdc(d.x, d.y, d.w, d.h, p.rect);
                if (!d.scan)
                {
                    p.uv[0] /= static_cast<float>(held.texWidth);
                    p.uv[2] /= static_cast<float>(held.texWidth);
                    p.uv[1] /= static_cast<float>(held.texHeight);
                    p.uv[3] /= static_cast<float>(held.texHeight);
                }
                vkCmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.scan ? g_scanPipe : g_plainPipe);
                pushTextures(held.view, d.scan || !d.linear ? g_nearest : g_linear);
                vkCmdPushConstants(f.cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &p);
                vkCmdDraw(f.cb, 4, 1, 0, 0);
                pictureDrawn = true;
            }
        if (!copies.empty())
        {
            vkCmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_overlayPipe);
            pushTextures(f.overlayView, g_nearest);
            const float fw = static_cast<float>(fbWidth), fh = static_cast<float>(fbHeight);
            for (const Rect &r : clipped)
            {
                Push p{};
                // The framebuffer's pixels onto the window's (the same size unless the window scales).
                const float sx = ww / fw, sy = wh / fh;
                toNdc(static_cast<float>(r.x) * sx, static_cast<float>(r.y) * sy, static_cast<float>(r.w) * sx, static_cast<float>(r.h) * sy, p.rect);
                p.uv[0] = static_cast<float>(r.x) / fw;
                p.uv[2] = static_cast<float>(r.x + r.w) / fw;
                p.uv[1] = (fh - static_cast<float>(r.y)) / fh;           // the rectangle's top: OpenGL row fh - y
                p.uv[3] = (fh - static_cast<float>(r.y + r.h)) / fh;
                for (float &c : p.colDiffuse)
                    c = 1.0f;
                vkCmdPushConstants(f.cb, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &p);
                vkCmdDraw(f.cb, 4, 1, 0, 0);
            }
        }
        vkCmdEndRendering(f.cb);
        const bool shoot = shot && g_canShoot;
        imageBarrier(f.cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     shoot ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, shoot ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_NONE,
                     shoot ? VK_ACCESS_2_TRANSFER_READ_BIT : 0);
        if (shoot)
        {
            const VkDeviceSize size = static_cast<VkDeviceSize>(g_extent.width) * g_extent.height * 4u;
            if (size > g_shotSize)
            {
                freeBuffer(g_shotBuffer, g_shotMemory);
                g_shotSize = 0;
                if (makeBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, g_shotBuffer, g_shotMemory, g_shotPtr))
                    g_shotSize = size;
            }
            if (g_shotSize >= size)
            {
                VkBufferImageCopy c{};
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageExtent = {g_extent.width, g_extent.height, 1};
                vkCmdCopyImageToBuffer(f.cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shotBuffer, 1, &c);
            }
            imageBarrier(f.cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                         VK_PIPELINE_STAGE_2_NONE, 0);
        }
        vkEndCommandBuffer(f.cb);

        const uint64_t hostValue = g_hostValue + 1;
        VkSemaphore waits[2] = {f.acquired, g_rendererTimeline};
        const uint64_t waitValues[2] = {0, held.value};
        const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
        VkSemaphore signals[2] = {g_renderDone[index], g_hostTimeline};
        const uint64_t signalValues[2] = {0, hostValue};
        VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tl.waitSemaphoreValueCount = pictureDrawn ? 2u : 1u;
        tl.pWaitSemaphoreValues = waitValues;
        tl.signalSemaphoreValueCount = 2;
        tl.pSignalSemaphoreValues = signalValues;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tl};
        si.waitSemaphoreCount = pictureDrawn ? 2u : 1u;
        si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cb;
        si.signalSemaphoreCount = 2;
        si.pSignalSemaphores = signals;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_renderDone[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swapchain;
        pi.pImageIndices = &index;
        VkResult submitted, presented;
        {
            std::lock_guard<std::mutex> lock(queueMutex());
            submitted = vkQueueSubmit(dev->queue, 1, &si, f.fence);
            presented = submitted == VK_SUCCESS ? vkQueuePresentKHR(dev->queue, &pi) : VK_ERROR_UNKNOWN;
        }
        if (!check(submitted, "vkQueueSubmit (present)"))
            return;
        g_hostValue = hostValue;
        if (pictureDrawn)
        {
            // The renderer may draw into this slot again once the host's value passes this.
            std::lock_guard<std::mutex> lock(g_slotMutex);
            for (Slot &s : g_slots)
                if (s.view == held.view && s.value == held.value)
                    s.hostRead = hostValue;
        }
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            g_recreate = true;
        else if (presented != VK_SUCCESS)
            check(presented, "vkQueuePresentKHR");
        g_frameIndex = (g_frameIndex + 1) % kInFlight;

        if (shoot && g_shotSize > 0)
        {
            vkWaitForFences(vk, 1, &f.fence, VK_TRUE, UINT64_MAX);
            g_shotWidth = static_cast<int>(g_extent.width);
            g_shotHeight = static_cast<int>(g_extent.height);
            g_shot.assign(g_shotPtr, g_shotPtr + static_cast<size_t>(g_shotWidth) * g_shotHeight * 4u);
            if (g_format == VK_FORMAT_B8G8R8A8_UNORM)
                for (size_t i = 0; i < g_shot.size(); i += 4)
                    std::swap(g_shot[i], g_shot[i + 2]);
            g_shotReady = true;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_presentMs += ms;
        g_presentMaxMs = std::max(g_presentMaxMs, ms);
        ++g_presents;
        logTiming();
    }

    bool takeShot(std::vector<uint8_t> &rgba, int &width, int &height)
    {
        if (!g_shotReady)
            return false;
        g_shotReady = false;
        rgba.swap(g_shot);
        width = g_shotWidth;
        height = g_shotHeight;
        return true;
    }
}
