// vkgl: the OpenGL subset of the GS renderer on Vulkan (see gs_vkgl.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // wglGetProcAddress, CloseHandle (OpenGL interop)
#include <shellapi.h> // CommandLineToArgvW (the pipeline cache's folder)

#include "gs_vk_device.h"

#include "gs_vkgl.h"
#include "gs_gl_present.h"
#include "gs_vk_context.h"
#include "gs_vk_backend.h"
#include "gs_vk_present.h"
#include "gs_vk_present_internal.h"
#if !defined(PS2X_GAME_GT3)
#include "gs_vk_pipeline_keys.h"
#endif

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#endif

#ifndef GL_CONSERVATIVE_RASTERIZATION_NV
#define GL_CONSERVATIVE_RASTERIZATION_NV 0x9346
#endif

namespace vkgl
{
    namespace
    {
        constexpr uint32_t kMaxFrames = 64;            // command buffers in flight (at most; GT3_VKGL_FRAMES)
        constexpr VkDeviceSize kChunkSize = 32ull << 20; // streaming memory (uploads, uniforms, re-specified buffers)
        constexpr VkDeviceSize kImageBlock = 64ull << 20;
        constexpr uint32_t kBindlessSlots = 32768;
        constexpr uint32_t kUniformBinding = 40;       // the loose uniforms' block (set 0)
        constexpr uint32_t kInputBinding = 30;         // the render target as an input attachment (set 0)
        constexpr int kImageShift = 8, kSamplerShift = 16;
        constexpr int kUnits = 32, kImageUnits = 8, kSsboBindings = 8;

        const gsvk::Device *dev = nullptr;
        VkDevice vk = VK_NULL_HANDLE;

        void logOnce(const char *what, unsigned value)
        {
            // (Also from the pipeline store's threads.)
            static std::mutex mutex;
            std::lock_guard<std::mutex> lock(mutex);
            static std::map<std::string, int> seen;
            char key[128];
            std::snprintf(key, sizeof(key), "%s %x", what, value);
            if (seen[key]++ == 0)
                std::fprintf(stderr, "[vkgl] unsupported %s 0x%x\n", what, value);
        }

        VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }

        // ------------------------------------------------------------------ memory

        // Device-local memory for images: 64 MB blocks, first fit (many small
        // decoded textures; one allocation each would hit the driver's cap).
        struct MemBlock
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            uint32_t type = 0;
            std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size
        };
        struct MemAlloc
        {
            MemBlock *block = nullptr;
            VkDeviceMemory memory = VK_NULL_HANDLE; // dedicated (block == nullptr)
            VkDeviceSize offset = 0, size = 0;
        };

        struct Chunk
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint8_t *ptr = nullptr;
            VkDeviceSize size = 0, used = 0;
            uint64_t lastSerial = 0; // the last command buffer using it
            int refs = 0;            // buffer objects whose storage lies in it
        };
        struct Slice
        {
            Chunk *chunk = nullptr;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            uint8_t *ptr = nullptr;
        };

        // ------------------------------------------------------------------ objects

        struct Texture
        {
            bool exists = false; // named (glGenTextures)
            VkImage image = VK_NULL_HANDLE;
            MemAlloc memory;
            VkImageView view = VK_NULL_HANDLE;     // in the image's format (depth aspect for D32F)
            VkImageView r32View = VK_NULL_HANDLE;  // R32_UINT view of an RGBA8 image (image units)
            VkFormat format = VK_FORMAT_UNDEFINED;
            GLenum internal = 0;
            int width = 0, height = 0;
            bool depth = false, integer = false, storage = false;
            VkImageUsageFlags usage = 0;
            GLint minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
            // Hazards (see syncAccesses): the last write, the stages it has been
            // made visible to, the stages that read it since, and the render
            // pass it was last written in as an attachment.
            VkPipelineStageFlags2 writeStages = 0, visibleStages = 0, readStages = 0;
            VkAccessFlags2 writeAccess = 0;
            uint64_t attachmentPass = 0;
            uint32_t bindless = UINT32_MAX;
            // Transfer rectangles (copies, uploads, blits; see transferDisjoint):
            // written since the last write by another stage, and read by
            // transfers since the last write. *All: too many to list.
            struct Rect
            {
                int x0, y0, x1, y1;
            };
            std::vector<Rect> xferWrites, xferReads;
            bool xferWritesAll = false, xferReadsAll = false;
            // The transfer barrier serials (g_xferSerial) when it was last
            // written / read by a transfer: a barrier since orders those.
            uint64_t xferWriteSerial[2] = {}, xferReadSerial = 0;
        };

        struct Buffer
        {
            bool exists = false;
            Slice slice;                     // streaming storage (glBufferData)
            VkBuffer dedicated = VK_NULL_HANDLE; // glBufferStorage
            VkDeviceMemory dedicatedMemory = VK_NULL_HANDLE;
            uint8_t *mapped = nullptr;
            VkDeviceSize size = 0;
            VkBuffer handle() const { return dedicated ? dedicated : slice.buffer; }
            VkDeviceSize base() const { return dedicated ? 0 : slice.offset; }
            uint8_t *data() const { return dedicated ? mapped : slice.ptr; }
        };

        struct Framebuffer
        {
            bool exists = false;
            GLuint color = 0, depth = 0;
            bool drawNone = false;
            int defaultWidth = 0, defaultHeight = 0;
        };

        struct Shader
        {
            bool exists = false;
            GLenum type = 0;
            std::string source;
            bool compiled = false;
            bool inputAttachment = false; // reads the target as an input attachment (see prepareSource)
            bool bindless = false;        // samples through bindless handles
            std::string prepared;         // the source after prepareSource
            std::string log;
        };

        // A loose uniform in the push constant block (see extractLooseUniforms).
        struct PushMember
        {
            std::string type, name;
            uint32_t size = 0, align = 0, offset = 0;
        };

        struct Resource
        {
            uint32_t binding = 0;
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            int unit = 0;              // texture/image unit or SSBO binding point
            bool uintSampler = false;  // usampler2D / uimage2D
        };

        struct Uniform
        {
            std::string name;
            int offset = -1;   // in the uniform block (loose uniforms)
            int resource = -1; // samplers/images: index in resources
        };

        struct PipelineKey
        {
            uint32_t colorFormat = 0, depthFormat = 0;
            uint8_t colorMask = 15, blend = 0, conservative = 0, depthClamp = 0;
            uint16_t eqRGB = 0, eqA = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
            bool operator==(const PipelineKey &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
        };
        struct PipelineKeyHash
        {
            size_t operator()(const PipelineKey &k) const
            {
                const unsigned char *p = reinterpret_cast<const unsigned char *>(&k);
                size_t h = 1469598103934665603ull;
                for (size_t i = 0; i < sizeof(k); ++i)
                    h = (h ^ p[i]) * 1099511628211ull;
                return h;
            }
        };

        struct Program
        {
            bool exists = false;
            std::vector<GLuint> shaders;
            bool linked = false, compute = false;
            std::string log;
            VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; // vertex + fragment, or compute
            std::vector<Uniform> uniforms; // location = index
            std::unordered_map<std::string, int> byName;
            std::vector<Resource> resources;
            std::vector<uint8_t> uniformData;
            bool hasUniformBlock = false;         // uniformData goes in a uniform buffer per draw
            uint32_t pushSize = 0;                // or as push constants (bytes)
            uint32_t uboBase = 0;                 // the uniform buffer's part of uniformData
            VkShaderStageFlags pushStages = 0;
            bool pushDirty = true;                // uniformData changed since it was pushed
            bool usesBindless = false, inputAttachment = false;
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkPipeline computePipeline = VK_NULL_HANDLE;
            std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
            // The pipeline store (see "pipeline store"): this link's number, and
            // the program by its OpenGL source (exact) and by its variant (the
            // #version / #extension / #define lines and the uniform names: the
            // same after an edit of the shader's body).
            uint64_t serial = 0, exactHash = 0, variantHash = 0;
        };

        struct SamplerObject
        {
            bool exists = false;
            GLint minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
        };

        struct Sync
        {
            uint64_t serial = 0;
        };

        struct Frame
        {
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            uint64_t serial = 0;
            bool pending = false;
        };

        template <typename T>
        struct Names
        {
            std::vector<T> objects = std::vector<T>(1); // name 0: none
            std::vector<GLuint> freed;
            GLuint make()
            {
                GLuint n;
                if (!freed.empty())
                {
                    n = freed.back();
                    freed.pop_back();
                    objects[n] = T{};
                }
                else
                {
                    n = static_cast<GLuint>(objects.size());
                    objects.emplace_back();
                }
                objects[n].exists = true;
                return n;
            }
            T *get(GLuint n) { return n && n < objects.size() && objects[n].exists ? &objects[n] : nullptr; }
            void release(GLuint n)
            {
                if (get(n))
                {
                    objects[n] = T{};
                    freed.push_back(n);
                }
            }
        };

        // ------------------------------------------------------------------ context

        struct Context
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            Frame frames[kMaxFrames];
            uint32_t frameIndex = 0, frameCount = 32;
            bool recording = false;
            uint64_t serial = 1;    // the command buffer being recorded (or the next one)
            uint64_t completed = 0; // every command buffer up to this serial has finished
            std::vector<std::pair<uint64_t, std::function<void()>>> deferred;

            std::vector<std::unique_ptr<Chunk>> chunks;
            Chunk *current = nullptr;
            uint32_t streamType = UINT32_MAX;
            std::vector<std::unique_ptr<MemBlock>> blocks;
            VkBuffer readback = VK_NULL_HANDLE;
            VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
            uint8_t *readbackPtr = nullptr;
            VkDeviceSize readbackSize = 0;
            bool readbackCoherent = true;
            // readStart / readTake's host buffers (GT4's mirror page)
            struct AsyncRead
            {
                VkBuffer buffer = VK_NULL_HANDLE;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                uint8_t *ptr = nullptr;
                VkDeviceSize size = 0;
                bool coherent = true, busy = false;
                uint64_t serial = 0;
                int w = 0, h = 0;
                uint32_t tag = 0;
            } asyncReads[3];

            Names<Texture> textures;
            Names<Buffer> buffers;
            Names<Framebuffer> framebuffers;
            Names<Shader> shaders;
            Names<Program> programs;
            Names<SamplerObject> samplerObjects;
            GLuint queryCounter = 0, vaoCounter = 0;
            // GL_TIME_ELAPSED queries (the renderer's GPU profiler, GT3_GS_GL_GPU_PROFILE=1): two timestamps each, in
            // a ring of timestampSlots; made on first use (needs hostQueryReset and timestamps on the queue).
            struct TimeQuery
            {
                uint32_t slot = 0;
                uint64_t serial = 0;
                bool ended = false;
            };
            std::unordered_map<GLuint, TimeQuery> timeQueries;
            VkQueryPool timestampPool = VK_NULL_HANDLE;
            uint32_t timestampNext = 0;
            GLuint activeTimeQuery = 0;
            bool timestampsTried = false, timestampsOk = false;
            double timestampNs = 1.0;
            uint64_t timestampMask = ~0ull;

            // Samplers by (min linear, mag linear, wrap S, wrap T).
            std::map<uint32_t, VkSampler> samplers;
            VkSampler bindlessSamplers[4] = {};
            VkDescriptorSetLayout bindlessLayout = VK_NULL_HANDLE;
            VkDescriptorPool bindlessPool = VK_NULL_HANDLE;
            VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
            std::vector<uint32_t> bindlessFree;
            uint32_t bindlessNext = 0;

            GLuint dummyFloat = 0, dummyUint = 0; // 1x1 textures for unbound units
            VkBuffer dummyBuffer = VK_NULL_HANDLE;
            VkDeviceMemory dummyBufferMemory = VK_NULL_HANDLE;

            // OpenGL state
            GLenum activeUnit = 0;
            GLuint units[kUnits] = {};
            struct ImageUnit
            {
                GLuint texture = 0;
                GLenum format = 0;
            } imageUnits[kImageUnits];
            struct SsboBinding
            {
                GLuint buffer = 0;
                GLintptr offset = 0;
                GLsizeiptr size = 0; // 0: whole
            } ssbo[kSsboBindings];
            GLuint genericBuffer = 0;
            GLuint readFbo = 0, drawFbo = 0;
            GLuint program = 0;
            bool depthTest = false, blend = false, conservative = false, depthClamp = false;
            GLenum depthFunc = GL_LESS;
            bool depthMask = true;
            uint8_t colorMask = 15;
            GLenum eqRGB = GL_FUNC_ADD, eqA = GL_FUNC_ADD, srcRGB = GL_ONE, dstRGB = GL_ZERO, srcA = GL_ONE, dstA = GL_ZERO;
            int vpX = 0, vpY = 0, vpW = 0, vpH = 0;
            GLint unpackAlignment = 4, packAlignment = 4;

            // Recording state
            bool inPass = false;
            GLuint passColor = 0, passDepth = 0;
            int passWidth = 0, passHeight = 0;
            VkPipeline boundPipeline = VK_NULL_HANDLE;
            VkPipelineLayout boundLayout = VK_NULL_HANDLE;
            VkPipelineBindPoint boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
            // Repeats skipped within a command buffer: the last descriptor
            // push per bind point (graphics, compute) as its layout and
            // content, whose push constants were last written, the dynamic state.
            struct PushedSet
            {
                VkPipelineLayout layout = VK_NULL_HANDLE;
                std::vector<uint64_t> key;
            } pushedSet[2];
            VkPipelineLayout pushedConstants = VK_NULL_HANDLE;
            bool dynamicValid = false;
            uint32_t cbDraws = 0; // draws in the command buffer being recorded
            VkViewport lastViewport{};
            VkRect2D lastScissor{};
            VkBool32 lastDepthTest = VK_FALSE, lastDepthWrite = VK_FALSE;
            VkCompareOp lastCompare = VK_COMPARE_OP_NEVER;
            // The open render pass's number (attachment writes of one pass need no barrier among them).
            uint64_t passId = 0;
            // Textures the renderer's current batch samples through bindless
            // handles (noteSampledTextures), for the hazards of its draws.
            std::vector<GLuint> bindlessReads;

            std::vector<std::string> extensions;
            std::string rendererName, versionName;

            // Presentation: two images whose memory OpenGL shares, in turn.
            struct PresentImage
            {
                GLuint target = 0; // vkgl texture the frame is drawn into
                GLuint name = 0;   // vkgl texture OpenGL shares (copied from target)
                int width = 0, height = 0;
                VkSemaphore vkDone = VK_NULL_HANDLE, glDone = VK_NULL_HANDLE;
                GLuint glTexture = 0, glMemory = 0, glVkDone = 0, glGlDone = 0;
                bool glSignalled = false; // OpenGL signalled glDone after its copy
            } present[3]; // (the window presenter, gs_vk_present.h: three slots; OpenGL's: the first two in turn)
            int presentNext = 0, presentCurrent = -1;
            // The next submission waits for / signals these (presentation;
            // a value: a timeline semaphore's, for the window presenter).
            VkSemaphore submitWait = VK_NULL_HANDLE, submitSignal = VK_NULL_HANDLE;
            uint64_t submitWaitValue = 0, submitSignalValue = 0;
            // (and these, for outside work on another API: splitExternal; timeline semaphores)
            VkSemaphore bridgeWait = VK_NULL_HANDLE, bridgeSignal = VK_NULL_HANDLE;
            uint64_t bridgeWaitValue = 0, bridgeSignalValue = 0;

            // GT3_VKGL_STATS=1: counts per 5 s (logged at presentation).
            struct Stats
            {
                uint64_t draws = 0, passes = 0, barriers = 0, submits = 0, copies = 0, blits = 0, uploads = 0, uploadBytes = 0, readbacks = 0,
                         dispatches = 0, pipelines = 0, waitNs = 0, localBarriers = 0, smallCopies = 0, pageCopies = 0,
                         descriptorPushes = 0, idleSubmits = 0, clears = 0, links = 0, linkNs = 0, pipelineNs = 0, pipelinesAhead = 0;
            } stats;
            uint64_t statsStart = 0;
        };
        Context *g = nullptr;
        bool g_stats = false;

        // ------------------------------------------------------------------ command buffers

        void runDeferred()
        {
            auto &d = g->deferred;
            size_t kept = 0;
            for (size_t i = 0; i < d.size(); ++i)
            {
                if (d[i].first <= g->completed)
                    d[i].second();
                else if (kept++ != i) // (never moved onto itself: libc++ empties a std::function moved to itself)
                    d[kept - 1] = std::move(d[i]);
            }
            d.resize(kept);
        }

        void poll()
        {
            // One queue: command buffers finish in submission order, so
            // everything before the oldest one still pending has finished.
            uint64_t lowestPending = UINT64_MAX;
            for (Frame &f : g->frames)
            {
                if (f.pending && vkGetFenceStatus(vk, f.fence) == VK_SUCCESS)
                    f.pending = false;
                if (f.pending)
                    lowestPending = std::min(lowestPending, f.serial);
            }
            g->completed = lowestPending == UINT64_MAX ? g->serial - 1 : lowestPending - 1;
            runDeferred();
        }

        // Destroys after the command buffers that may use it have finished.
        void later(std::function<void()> fn) { g->deferred.emplace_back(g->serial, std::move(fn)); }

        VkCommandBuffer cmd()
        {
            if (!g->recording)
            {
                Frame &f = g->frames[g->frameIndex];
                if (f.pending)
                {
                    vkWaitForFences(vk, 1, &f.fence, VK_TRUE, UINT64_MAX);
                    f.pending = false;
                }
                poll();
                vkResetFences(vk, 1, &f.fence);
                vkResetCommandBuffer(f.cb, 0);
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(f.cb, &begin);
                f.serial = g->serial;
                g->recording = true;
                g->boundPipeline = VK_NULL_HANDLE;
                g->boundLayout = VK_NULL_HANDLE;
                g->boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
                g->pushedSet[0].layout = g->pushedSet[1].layout = VK_NULL_HANDLE;
                g->pushedConstants = VK_NULL_HANDLE;
                g->dynamicValid = false;
                g->cbDraws = 0;
            }
            return g->frames[g->frameIndex].cb;
        }

        void endPass()
        {
            if (!g->inPass)
                return;
            vkCmdEndRendering(cmd());
            g->inPass = false;
        }

        // ------------------------------------------------------------------ hazards
        //
        // Every image stays in the GENERAL layout, so ordering is memory
        // barriers only. Each texture remembers its last write (stages and
        // access), the stages that write has been made visible to, and the
        // stages that read it since. An operation lists its accesses; one
        // memory barrier covers what they need (read after write, write after
        // write, write after read) with the stages involved, not a full drain.
        // Draws of one render pass writing its attachments need none among
        // them (rasterization order); reads of the pass's colour target in it
        // go through local read (textureBarrier).

        struct Access
        {
            Texture *t;
            VkPipelineStageFlags2 stage;
            VkAccessFlags2 access;
            bool write;
            bool attachment; // a render pass attachment (the pass's own writes are ordered)
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // transfers: the rectangle (x1 <= x0: unknown / whole)
        };

        constexpr VkAccessFlags2 kWriteBits = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

        // The barrier these accesses need (0 stages: none). continuing: they
        // belong to a draw that continues the open pass.
        uint64_t g_barrierWhy[5][4] = {}; // GT3_VKGL_STATS: accesses needing a barrier, by kind and by the last writer

        // Transfers of disjoint rectangles of one image need no barrier between
        // them (a copy, upload or blit writes only its rectangle): a transfer
        // access needs none while every write it would wait for is a transfer
        // write of other rectangles (and, if it writes, every read since is a
        // transfer read of other rectangles). GT4's night course moves ~250
        // pages a frame between two targets, each copy its own barrier before
        // (analysis/frame-times/night-4x). GT3_VKGL_TRANSFER_REGIONS=1: on.
        // Off by default: night 4x barriers 1,406 -> 982 a frame, but GPU time
        // only 18.27 -> 18.09 ms (run a2-on vs a2-noreg), not worth the risk.
        constexpr size_t kMaxXferRects = 48;
        bool transferRegionsOn()
        {
            static const bool on = std::getenv("GT3_VKGL_TRANSFER_REGIONS") && std::strcmp(std::getenv("GT3_VKGL_TRANSFER_REGIONS"), "1") == 0;
            return on;
        }
        bool rectsMiss(const std::vector<Texture::Rect> &rects, bool all, const Access &x)
        {
            if (all)
                return false;
            for (const Texture::Rect &r : rects)
                if (x.x0 < r.x1 && r.x0 < x.x1 && x.y0 < r.y1 && r.y0 < x.y1)
                    return false;
            return true;
        }
        // Barriers issued since the start (syncAccesses) that order earlier
        // transfers before later ones: [0] transfer writes for later transfer
        // writes, [1] transfer writes for later transfer reads, [2] transfer
        // reads (execution order) for later transfer writes. An image's
        // transfers from before such a barrier need no barrier of their own
        // (GT4's ring of 16 upload textures, each reused 16 uploads later).
        uint64_t g_xferSerial[3] = {1, 1, 1};
        bool transferDisjoint(const Texture &t, const Access &x)
        {
            if (x.stage != VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT || x.x1 <= x.x0 || x.y1 <= x.y0 || !transferRegionsOn())
                return false;
            const bool waitsWrite = t.writeStages && (x.write || (t.visibleStages & x.stage) != x.stage);
            if (waitsWrite &&
                !(t.writeStages == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT &&
                  (t.xferWriteSerial[x.write ? 0 : 1] < g_xferSerial[x.write ? 0 : 1] || rectsMiss(t.xferWrites, t.xferWritesAll, x))))
                return false;
            if (x.write && t.readStages &&
                !(t.readStages == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT &&
                  (t.xferReadSerial < g_xferSerial[2] || rectsMiss(t.xferReads, t.xferReadsAll, x))))
                return false;
            return true;
        }
        void addRect(std::vector<Texture::Rect> &rects, bool &all, const Access &x)
        {
            if (all)
                return;
            if (x.x1 <= x.x0 || x.y1 <= x.y0 || rects.size() >= kMaxXferRects)
            {
                all = true;
                rects.clear();
                return;
            }
            rects.push_back({x.x0, x.y0, x.x1, x.y1});
        }
        std::vector<uint8_t> g_disjoint; // per access of the last neededBarrier: needs no barrier (transferDisjoint)

        void neededBarrier(const std::vector<Access> &a, bool continuing, VkPipelineStageFlags2 &src, VkAccessFlags2 &srcAccess,
                           VkPipelineStageFlags2 &dst, VkAccessFlags2 &dstAccess)
        {
            src = dst = 0;
            srcAccess = dstAccess = 0;
            g_disjoint.assign(a.size(), 0u);
            for (size_t i = 0; i < a.size(); ++i)
            {
                const Access &x = a[i];
                const Texture &t = *x.t;
                if (continuing && x.attachment && t.attachmentPass == g->passId)
                    continue;
                if (transferDisjoint(t, x))
                {
                    g_disjoint[i] = 1u;
                    continue;
                }
                if (t.writeStages && (x.write || (t.visibleStages & x.stage) != x.stage))
                {
                    if (g_stats)
                    {
                        // [kind of this access][what wrote it]: 0 sampled read, 1 storage, 2 colour attachment, 3 depth,
                        // 4 input attachment / by 0 attachment, 1 transfer, 2 shader, 3 other.
                        const int kind = x.access & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT ? 0
                                         : x.access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT ? 1
                                         : x.access & VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT ? 4
                                         : x.access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT ? 2 : 3;
                        const int by = (t.writeAccess & (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)) ? 0
                                       : (t.writeAccess & VK_ACCESS_2_TRANSFER_WRITE_BIT) ? 1
                                       : (t.writeAccess & (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)) ? 2 : 3;
                        ++g_barrierWhy[kind][by];
                    }
                    src |= t.writeStages;
                    srcAccess |= t.writeAccess;
                    dst |= x.stage;
                    dstAccess |= x.access;
                }
                if (x.write && t.readStages)
                {
                    src |= t.readStages; // write after read: execution order only
                    dst |= x.stage;
                }
            }
        }

        // Orders the accesses after what came before (a barrier outside any
        // pass: the caller has ended it when one is needed) and records them.
        void syncAccesses(const std::vector<Access> &a, bool continuing)
        {
            VkPipelineStageFlags2 src, dst;
            VkAccessFlags2 srcAccess, dstAccess;
            neededBarrier(a, continuing, src, srcAccess, dst, dstAccess);
            // GT3_VKGL_NO_BARRIERS=1: diagnostics only (what the barriers cost; pictures may break).
            static const bool skip = std::getenv("GT3_VKGL_NO_BARRIERS") && *std::getenv("GT3_VKGL_NO_BARRIERS") == '1';
            if (src && !skip)
            {
                endPass();
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = src;
                mb.srcAccessMask = srcAccess & kWriteBits;
                mb.dstStageMask = dst;
                mb.dstAccessMask = dstAccess;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.memoryBarrierCount = 1;
                di.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd(), &di);
                ++g->stats.barriers;
                if ((src & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT) && (dst & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT))
                {
                    const bool fromWrites = (mb.srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0;
                    if (fromWrites && (dstAccess & VK_ACCESS_2_TRANSFER_WRITE_BIT))
                        ++g_xferSerial[0];
                    if (fromWrites && (dstAccess & VK_ACCESS_2_TRANSFER_READ_BIT))
                        ++g_xferSerial[1];
                    ++g_xferSerial[2];
                }
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                const Access &x = a[i];
                Texture &t = *x.t;
                const bool transfer = x.stage == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                if (transfer && x.write)
                {
                    t.xferWriteSerial[0] = g_xferSerial[0];
                    t.xferWriteSerial[1] = g_xferSerial[1];
                }
                else if (transfer)
                    t.xferReadSerial = g_xferSerial[2];
                if (g_disjoint[i])
                {
                    // No barrier: the earlier transfers stay listed; this one joins them.
                    if (x.write)
                    {
                        t.writeStages = x.stage;
                        t.writeAccess = x.access & kWriteBits;
                        t.visibleStages = 0;
                        addRect(t.xferWrites, t.xferWritesAll, x);
                    }
                    else
                    {
                        t.readStages |= x.stage;
                        addRect(t.xferReads, t.xferReadsAll, x);
                    }
                    continue;
                }
                if (x.write)
                {
                    if (continuing && x.attachment && t.attachmentPass == g->passId)
                        continue;
                    t.writeStages = x.stage;
                    t.writeAccess = x.access & kWriteBits;
                    t.visibleStages = 0;
                    t.readStages = 0;
                    if (x.attachment)
                        t.attachmentPass = g->passId;
                    t.xferWrites.clear();
                    t.xferWritesAll = false;
                    t.xferReads.clear();
                    t.xferReadsAll = false;
                    if (transfer)
                        addRect(t.xferWrites, t.xferWritesAll, x);
                }
                else
                {
                    if (t.writeStages)
                        t.visibleStages |= x.stage;
                    t.readStages |= x.stage;
                    if (transfer)
                        addRect(t.xferReads, t.xferReadsAll, x);
                }
            }
        }

        constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        constexpr VkPipelineStageFlags2 kFragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        constexpr VkPipelineStageFlags2 kColorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        constexpr VkPipelineStageFlags2 kDepthTests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

        std::vector<Access> g_accesses;

        void submit()
        {
            if (!g->recording && (g->submitWait || g->submitSignal || g->bridgeWait || g->bridgeSignal))
                cmd(); // an (empty) command buffer carries the semaphores
            if (!g->recording)
                return;
            endPass();
            Frame &f = g->frames[g->frameIndex];
            vkEndCommandBuffer(f.cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &f.cb;
            const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
            VkSemaphore waits[2], signals[2];
            uint64_t waitValues[2], signalValues[2];
            uint32_t waitCount = 0, signalCount = 0;
            if (g->submitWait)
            {
                waits[waitCount] = g->submitWait;
                waitValues[waitCount++] = g->submitWaitValue;
            }
            if (g->bridgeWait)
            {
                waits[waitCount] = g->bridgeWait;
                waitValues[waitCount++] = g->bridgeWaitValue;
            }
            if (g->submitSignal)
            {
                signals[signalCount] = g->submitSignal;
                signalValues[signalCount++] = g->submitSignalValue;
            }
            if (g->bridgeSignal)
            {
                signals[signalCount] = g->bridgeSignal;
                signalValues[signalCount++] = g->bridgeSignalValue;
            }
            const bool timelineValues = g->submitWaitValue || g->submitSignalValue || g->bridgeWait || g->bridgeSignal;
            g->submitWait = g->submitSignal = g->bridgeWait = g->bridgeSignal = VK_NULL_HANDLE;
            g->submitWaitValue = g->submitSignalValue = g->bridgeWaitValue = g->bridgeSignalValue = 0;
            if (waitCount)
            {
                si.waitSemaphoreCount = waitCount;
                si.pWaitSemaphores = waits;
                si.pWaitDstStageMask = waitStages;
            }
            if (signalCount)
            {
                si.signalSemaphoreCount = signalCount;
                si.pSignalSemaphores = signals;
            }
            // (a binary semaphore's value is ignored)
            VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            if (timelineValues)
            {
                timeline.waitSemaphoreValueCount = si.waitSemaphoreCount;
                timeline.pWaitSemaphoreValues = waitValues;
                timeline.signalSemaphoreValueCount = si.signalSemaphoreCount;
                timeline.pSignalSemaphoreValues = signalValues;
                si.pNext = &timeline;
            }
            VkResult submitted;
            {
                // (The window presenter submits on the host thread to the same queue.)
                std::lock_guard<std::mutex> lock(gsvk::queueMutex());
                submitted = vkQueueSubmit(dev->queue, 1, &si, f.fence);
            }
            if (submitted != VK_SUCCESS)
                std::fprintf(stderr, "[vkgl] vkQueueSubmit failed\n");
            f.pending = true;
            ++g->stats.submits;
            g->recording = false;
            ++g->serial;
            g->frameIndex = (g->frameIndex + 1) % g->frameCount;
            poll();
        }

        // Waits until command buffer `serial` has finished (submitting it first
        // if it is being recorded). False on timeout.
        bool waitSerial(uint64_t serial, uint64_t timeoutNs = UINT64_MAX)
        {
            if (serial <= g->completed)
                return true;
            if (g->recording && serial >= g->frames[g->frameIndex].serial)
                submit();
            for (Frame &f : g->frames)
                if (f.pending && f.serial <= serial)
                {
                    if (vkWaitForFences(vk, 1, &f.fence, VK_TRUE, timeoutNs) != VK_SUCCESS)
                        return false;
                    f.pending = false;
                }
            poll();
            return true;
        }

        // ------------------------------------------------------------------ memory

        uint32_t findType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0)
        {
            if (prefer)
            {
                const uint32_t t = gsvk::memoryType(bits, want | prefer);
                if (t != UINT32_MAX)
                    return t;
            }
            return gsvk::memoryType(bits, want);
        }

        bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer, VkBuffer &buffer,
                        VkDeviceMemory &memory, uint8_t **ptr, uint32_t *typeOut = nullptr)
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = size;
            bci.usage = usage;
            if (vkCreateBuffer(vk, &bci, nullptr, &buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(vk, buffer, &req);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = findType(req.memoryTypeBits, want, prefer);
            VkResult r = mai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkAllocateMemory(vk, &mai, nullptr, &memory);
            if (r != VK_SUCCESS && prefer)
            {
                mai.memoryTypeIndex = findType(req.memoryTypeBits, want);
                r = mai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkAllocateMemory(vk, &mai, nullptr, &memory);
            }
            if (r != VK_SUCCESS)
            {
                vkDestroyBuffer(vk, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
                return false;
            }
            vkBindBufferMemory(vk, buffer, memory, 0);
            if (ptr)
                vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void **>(ptr));
            if (typeOut)
                *typeOut = mai.memoryTypeIndex;
            return true;
        }

        constexpr VkBufferUsageFlags kStreamUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        Chunk *newChunk(VkDeviceSize size)
        {
            auto c = std::make_unique<Chunk>();
            c->size = size;
            // Host-visible, device-local where the GPU offers it (resizable BAR):
            // shaders read primitives and states from here.
            if (!makeBuffer(size, kStreamUsage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, c->buffer, c->memory, &c->ptr))
            {
                std::fprintf(stderr, "[vkgl] out of memory for a %llu byte stream chunk\n", static_cast<unsigned long long>(size));
                return nullptr;
            }
            g->chunks.push_back(std::move(c));
            return g->chunks.back().get();
        }

        Slice streamAlloc(VkDeviceSize size, VkDeviceSize align)
        {
            align = std::max<VkDeviceSize>(align, 16);
            Chunk *c = g->current;
            if (!c || alignUp(c->used, align) + size > c->size)
            {
                c = nullptr;
                poll();
                for (auto &candidate : g->chunks)
                    if (candidate.get() != g->current && candidate->refs == 0 && candidate->lastSerial <= g->completed && candidate->size >= size)
                    {
                        c = candidate.get();
                        c->used = 0;
                        break;
                    }
                if (!c)
                    c = newChunk(std::max(kChunkSize, alignUp(size, 1u << 20)));
                if (!c)
                    return {};
                g->current = c;
            }
            const VkDeviceSize offset = alignUp(c->used, align);
            c->used = offset + size;
            c->lastSerial = g->serial;
            return Slice{c, c->buffer, offset, c->ptr + offset};
        }

        MemAlloc imageMemory(const VkMemoryRequirements &req)
        {
            MemAlloc a;
            const uint32_t type = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type == UINT32_MAX)
                return a;
            if (req.size >= kImageBlock / 4)
            {
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = type;
                if (vkAllocateMemory(vk, &mai, nullptr, &a.memory) == VK_SUCCESS)
                    a.size = req.size;
                return a;
            }
            auto tryBlock = [&](MemBlock &b) {
                for (auto it = b.free.begin(); it != b.free.end(); ++it)
                {
                    const VkDeviceSize start = alignUp(it->first, req.alignment);
                    const VkDeviceSize end = it->first + it->second;
                    if (start + req.size > end)
                        continue;
                    const VkDeviceSize before = start - it->first, after = end - (start + req.size);
                    const VkDeviceSize origin = it->first;
                    b.free.erase(it);
                    if (before)
                        b.free[origin] = before;
                    if (after)
                        b.free[start + req.size] = after;
                    a.block = &b;
                    a.offset = start;
                    a.size = req.size;
                    return true;
                }
                return false;
            };
            for (auto &b : g->blocks)
                if (b->type == type && tryBlock(*b))
                    return a;
            auto b = std::make_unique<MemBlock>();
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = kImageBlock;
            mai.memoryTypeIndex = type;
            if (vkAllocateMemory(vk, &mai, nullptr, &b->memory) != VK_SUCCESS)
                return a;
            b->size = kImageBlock;
            b->type = type;
            b->free[0] = kImageBlock;
            g->blocks.push_back(std::move(b));
            tryBlock(*g->blocks.back());
            return a;
        }

        void freeImageMemory(const MemAlloc &a)
        {
            if (a.memory)
            {
                vkFreeMemory(vk, a.memory, nullptr);
                return;
            }
            if (!a.block)
                return;
            auto &free = a.block->free;
            VkDeviceSize offset = a.offset, size = a.size;
            auto next = free.lower_bound(offset);
            if (next != free.end() && offset + size == next->first)
            {
                size += next->second;
                next = free.erase(next);
            }
            if (next != free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    prev->second += size;
                    return;
                }
            }
            free[offset] = size;
        }

        // ------------------------------------------------------------------ textures

        struct FormatInfo
        {
            VkFormat format;
            uint32_t bytes;
            bool depth, integer;
        };

        bool formatInfo(GLenum internal, FormatInfo &out)
        {
            switch (internal)
            {
            case GL_RGBA8: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, false, false}; return true;
            case GL_DEPTH_COMPONENT32F: out = {VK_FORMAT_D32_SFLOAT, 4, true, false}; return true;
            case GL_R8: out = {VK_FORMAT_R8_UNORM, 1, false, false}; return true;
            case GL_R8UI: out = {VK_FORMAT_R8_UINT, 1, false, true}; return true;
            case GL_R16UI: out = {VK_FORMAT_R16_UINT, 2, false, true}; return true;
            case GL_R32UI: out = {VK_FORMAT_R32_UINT, 4, false, true}; return true;
            case GL_RG16UI: out = {VK_FORMAT_R16G16_UINT, 4, false, true}; return true;
            case GL_RG16F: out = {VK_FORMAT_R16G16_SFLOAT, 4, false, false}; return true; // (GT4's motion texture, DLSS)
            default: return false;
            }
        }

        // Bytes per pixel of client data (upload / read back) in format/type.
        uint32_t clientBytes(GLenum format, GLenum type)
        {
            uint32_t components = 1;
            switch (format)
            {
            case GL_RGBA:
            case GL_RGBA_INTEGER: components = 4; break;
            case GL_RG:
            case GL_RG_INTEGER: components = 2; break;
            default: components = 1; break;
            }
            uint32_t size = 1;
            switch (type)
            {
            case GL_UNSIGNED_SHORT:
            case GL_SHORT: size = 2; break;
            case GL_UNSIGNED_INT:
            case GL_INT:
            case GL_FLOAT: size = 4; break;
            default: size = 1; break;
            }
            return components * size;
        }

        Texture *boundTexture() { return g->textures.get(g->units[g->activeUnit]); }

        VkImageAspectFlags aspect(const Texture &t) { return t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }

        void destroyTextureObjects(Texture &t)
        {
            if (!t.image)
                return;
            VkImage image = t.image;
            VkImageView view = t.view, r32 = t.r32View;
            MemAlloc memory = t.memory;
            const uint32_t slot = t.bindless;
            later([=] {
                if (view)
                    vkDestroyImageView(vk, view, nullptr);
                if (r32)
                    vkDestroyImageView(vk, r32, nullptr);
                vkDestroyImage(vk, image, nullptr);
                freeImageMemory(memory);
                if (slot != UINT32_MAX)
                    g->bindlessFree.push_back(slot);
            });
            t.image = VK_NULL_HANDLE;
            t.view = t.r32View = VK_NULL_HANDLE;
            t.memory = {};
            t.bindless = UINT32_MAX;
        }

        // exportable: dedicated memory OpenGL can import (presentation).
        bool createImage(Texture &t, GLenum internal, int width, int height, bool exportable = false)
        {
            FormatInfo fi{};
            if (!formatInfo(internal, fi))
            {
                logOnce("texture format", internal);
                return false;
            }
            destroyTextureObjects(t);
            t.format = fi.format;
            t.internal = internal;
            t.width = width;
            t.height = height;
            t.depth = fi.depth;
            t.integer = fi.integer;
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(dev->physical, fi.format, &fp);
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (fi.depth)
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            else if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && !exportable)
            {
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // (render targets may be compressed: not for OpenGL)
                if (dev->localRead)
                    usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT; // blends reading the target (glTextureBarrier)
            }
            // Images OpenGL imports have no format views: its import has to
            // agree with how the driver laid them out.
            t.storage = !fi.depth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
            if (t.storage)
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, exportable ? &external : nullptr};
            ici.flags = (internal == GL_RGBA8 && !exportable) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = fi.format;
            ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = usage;
            t.usage = usage;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(vk, &ici, nullptr, &t.image) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[vkgl] vkCreateImage %dx%d format %u failed\n", width, height, static_cast<unsigned>(fi.format));
                t.image = VK_NULL_HANDLE;
                return false;
            }
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(vk, t.image, &req);
            if (exportable)
            {
                VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
                dedicated.image = t.image;
                VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
                exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                t.memory = {};
                if (mai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(vk, &mai, nullptr, &t.memory.memory) == VK_SUCCESS)
                    t.memory.size = req.size;
            }
            else
                t.memory = imageMemory(req);
            if (!t.memory.block && !t.memory.memory)
            {
                std::fprintf(stderr, "[vkgl] out of video memory for a %dx%d texture\n", width, height);
                vkDestroyImage(vk, t.image, nullptr);
                t.image = VK_NULL_HANDLE;
                return false;
            }
            vkBindImageMemory(vk, t.image, t.memory.block ? t.memory.block->memory : t.memory.memory, t.memory.offset);
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = t.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = fi.format;
            vci.subresourceRange = {aspect(t), 0, 1, 0, 1};
            vkCreateImageView(vk, &vci, nullptr, &t.view);
            // Into the GENERAL layout, where every image stays.
            endPass();
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = t.image;
            ib.subresourceRange = {aspect(t), 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd(), &di);
            // (The transition's destination is every later command.)
            t.writeStages = t.visibleStages = t.readStages = 0;
            t.writeAccess = 0;
            t.attachmentPass = 0;
            t.xferWrites.clear();
            t.xferReads.clear();
            t.xferWritesAll = t.xferReadsAll = false;
            return true;
        }

        VkImageView r32View(Texture &t)
        {
            if (!t.r32View)
            {
                VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vci.image = t.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = VK_FORMAT_R32_UINT;
                vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCreateImageView(vk, &vci, nullptr, &t.r32View);
            }
            return t.r32View;
        }

        VkFilter filterOf(GLint f) { return (f == GL_LINEAR || f == GL_LINEAR_MIPMAP_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
        VkSamplerAddressMode wrapOf(GLint w)
        {
            switch (w)
            {
            case GL_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        }

        VkSampler sampler(GLint minF, GLint magF, GLint wrapS, GLint wrapT)
        {
            const VkFilter mn = filterOf(minF), mg = filterOf(magF);
            const VkSamplerAddressMode s = wrapOf(wrapS), t = wrapOf(wrapT);
            const uint32_t key = static_cast<uint32_t>(mn) | (static_cast<uint32_t>(mg) << 2) | (static_cast<uint32_t>(s) << 4) |
                                 (static_cast<uint32_t>(t) << 8);
            auto it = g->samplers.find(key);
            if (it != g->samplers.end())
                return it->second;
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = mg;
            sci.minFilter = mn;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = s;
            sci.addressModeV = t;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.maxLod = 0.25f; // one level: GL's non-mipmap filters
            VkSampler vs = VK_NULL_HANDLE;
            vkCreateSampler(vk, &sci, nullptr, &vs);
            g->samplers[key] = vs;
            return vs;
        }

        // Before a copy, upload or blit (transfer stage; outside any pass).
        void transferHazards(std::initializer_list<Texture *> reads, std::initializer_list<Texture *> writes)
        {
            endPass();
            g_accesses.clear();
            for (Texture *t : reads)
                if (t)
                    g_accesses.push_back({t, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT, false, false});
            for (Texture *t : writes)
                if (t)
                    g_accesses.push_back({t, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true, false});
            syncAccesses(g_accesses, false);
        }

        // transferHazards with each side's rectangle (x, y, w, h), so transfers of
        // disjoint rectangles of an image need no barrier between them.
        void transferHazardsRect(Texture *src, int sx, int sy, int sw, int sh, Texture *dst, int dx, int dy, int dw, int dh)
        {
            endPass();
            g_accesses.clear();
            if (src)
                g_accesses.push_back({src, kTransfer, VK_ACCESS_2_TRANSFER_READ_BIT, false, false, sx, sy, sx + sw, sy + sh});
            if (dst)
                g_accesses.push_back({dst, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, true, false, dx, dy, dx + dw, dy + dh});
            syncAccesses(g_accesses, false);
        }

        // ------------------------------------------------------------------ programs

        // The pipeline store (see "pipeline store" below): its VkPipelineCache
        // (VK_NULL_HANDLE without the store), and a linked program's pipelines
        // to build ahead.
        VkPipelineCache pipelineCache();
        void storeProgramLinked(GLuint name, const Program &p);

        // GT3_VKGL_SPIRV_SALT=<text> (diagnostics): an OpModuleProcessed
        // "vkgl salt <text>" in every SPIR-V module, so the modules' bytes
        // differ from any earlier run's and the driver's own shader cache
        // misses: a cold driver cache for this run, with nothing on the
        // machine changed or deleted. What the shaders do is unchanged.
        void saltSpirv(std::vector<uint32_t> &spirv)
        {
            static const std::string salt = [] {
                const char *v = std::getenv("GT3_VKGL_SPIRV_SALT");
                if (!v || !*v)
                    return std::string();
                std::fprintf(stderr, "[vkgl] SPIR-V salt \"%s\" (GT3_VKGL_SPIRV_SALT, diagnostics): driver caches miss\n", v);
                return std::string("vkgl salt ") + v;
            }();
            if (salt.empty() || spirv.size() < 5)
                return;
            // OpModuleProcessed's place in the logical layout: after the
            // capabilities, entry points, modes, sources and names, before the
            // first annotation or type.
            size_t pos = 5;
            while (pos < spirv.size())
            {
                const uint32_t words = spirv[pos] >> 16, op = spirv[pos] & 0xFFFFu;
                const bool preamble = op == 17 /* Capability */ || op == 10 /* Extension */ || op == 11 /* ExtInstImport */ || op == 14 /* MemoryModel */ ||
                                      op == 15 /* EntryPoint */ || op == 16 /* ExecutionMode */ || op == 331 /* ExecutionModeId */ || op == 7 /* String */ ||
                                      op == 4 /* SourceExtension */ || op == 3 /* Source */ || op == 2 /* SourceContinued */ || op == 5 /* Name */ ||
                                      op == 6 /* MemberName */ || op == 330 /* ModuleProcessed */;
                if (!preamble || words == 0)
                    break;
                pos += words;
            }
            std::vector<uint32_t> instruction((salt.size() + 1 + 3) / 4 + 1, 0u);
            instruction[0] = (static_cast<uint32_t>(instruction.size()) << 16) | 330u;
            std::memcpy(&instruction[1], salt.data(), salt.size());
            spirv.insert(spirv.begin() + static_cast<std::ptrdiff_t>(std::min(pos, spirv.size())), instruction.begin(), instruction.end());
        }

        // Local read (VK_KHR_dynamic_rendering_local_read) for target reads;
        // GT3_VKGL_LOCAL_READ=0: copies of the target instead, as without it.
        bool localReadOn()
        {
            static const bool off = std::getenv("GT3_VKGL_LOCAL_READ") && std::strcmp(std::getenv("GT3_VKGL_LOCAL_READ"), "0") == 0;
            return dev->localRead && !off;
        }

        const char kBindlessPrelude[] = R"GLSL(#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform texture2D vkgl_tex2D[];
layout(set = 1, binding = 0) uniform utexture2D vkgl_utex2D[];
layout(set = 1, binding = 1) uniform sampler vkgl_samplers[4];
#define sampler2D(h) sampler2D(vkgl_tex2D[nonuniformEXT((h).x)], vkgl_samplers[nonuniformEXT((h).y)])
#define usampler2D(h) usampler2D(vkgl_utex2D[nonuniformEXT((h).x)], vkgl_samplers[nonuniformEXT((h).y)])
)GLSL";

        // OpenGL GLSL as the renderer writes it -> what glslang's relaxed
        // Vulkan rules take: #version 450, bindless handles as indices into
        // the set 1 arrays. With local read, the draw shader's reads of its
        // own pixel of the target (u_dst, a texture-barrier read in OpenGL)
        // become input attachment reads: the pass goes on, draws are
        // separated by an in-pass barrier (see textureBarrier).
        std::string prepareSource(const std::string &source, bool &inputAttachment, bool &usesBindless)
        {
            std::string s = source;
            inputAttachment = false;
            usesBindless = false;
            const size_t v = s.find("#version 430");
            if (v != std::string::npos)
                s.replace(v, std::strlen("#version 430"), "#version 450");
            const char *bindless = "#extension GL_ARB_bindless_texture : require";
            const size_t b = s.find(bindless);
            if (b != std::string::npos)
            {
                s.replace(b, std::strlen(bindless), dev->bindless ? kBindlessPrelude : "");
                usesBindless = dev->bindless;
            }
            const char *dstDecl = "uniform sampler2D u_dst;";
            const char *dstRead = "texelFetch(u_dst, pix, 0)";
            const size_t d = s.find(dstDecl), r = s.find(dstRead);
            // (Any other use of u_dst would fail to compile after this.)
            if (localReadOn() && d != std::string::npos && r != std::string::npos)
            {
                s.replace(r, std::strlen(dstRead), "subpassLoad(vkgl_dstInput)");
                s.replace(d, std::strlen(dstDecl), "layout(input_attachment_index = 0, set = 0, binding = 30) uniform subpassInput vkgl_dstInput;");
                inputAttachment = true;
            }
            return s;
        }

        EShLanguage stageOf(GLenum type)
        {
            switch (type)
            {
            case GL_VERTEX_SHADER: return EShLangVertex;
            case GL_FRAGMENT_SHADER: return EShLangFragment;
            default: return EShLangCompute;
            }
        }

        bool isSamplerType(int t)
        {
            return (t >= 0x8B5D && t <= 0x8B64) || (t >= 0x8DC0 && t <= 0x8DD8) || (t >= 0x9108 && t <= 0x910D) || t == 0x8DCA || t == 0x8DD2;
        }
        bool isImageType(int t) { return t >= 0x904C && t <= 0x906C; }
        bool isUintType(int t) { return t == 0x8DD2 || t == 0x8DD1 || t == 0x8DD5 || (t >= 0x9062 && t <= 0x906C); }

        // Push constants for the loose uniforms: every stage's
        // "uniform <scalar or vector> name;" lines become one block, the same
        // in each stage with members at fixed offsets, written into the
        // command buffer by a draw (when changed) instead of a uniform buffer
        // allocated and bound by each draw. GT3_VKGL_PUSH_CONSTANTS=0: the
        // uniform buffer (glslang's block of the loose uniforms) as before.
        bool pushConstantsOn()
        {
            static const bool off = std::getenv("GT3_VKGL_PUSH_CONSTANTS") && std::strcmp(std::getenv("GT3_VKGL_PUSH_CONSTANTS"), "0") == 0;
            return !off;
        }

        bool pushMemberType(const std::string &type, uint32_t &size, uint32_t &align)
        {
            static const struct
            {
                const char *name;
                uint32_t size, align;
            } kTypes[] = {{"int", 4, 4},    {"uint", 4, 4},    {"float", 4, 4},   {"ivec2", 8, 8},  {"uvec2", 8, 8},  {"vec2", 8, 8},
                          {"ivec3", 12, 16}, {"uvec3", 12, 16}, {"vec3", 12, 16}, {"ivec4", 16, 16}, {"uvec4", 16, 16}, {"vec4", 16, 16}};
            for (const auto &t : kTypes)
                if (type == t.name)
                {
                    size = t.size;
                    align = t.align;
                    return true;
                }
            return false;
        }

        // src without its loose uniform declarations (their lines left empty)
        // -> out; the declarations join members. False when a name has two types.
        bool extractLooseUniforms(const std::string &src, std::string &out, std::vector<PushMember> &members)
        {
            out.clear();
            out.reserve(src.size());
            auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
            size_t pos = 0;
            while (pos < src.size())
            {
                size_t end = src.find('\n', pos);
                end = end == std::string::npos ? src.size() : end + 1;
                const std::string_view line(src.data() + pos, end - pos);
                pos = end;
                size_t k = line.find_first_not_of(" \t");
                if (k == std::string_view::npos || line.compare(k, 8, "uniform ") != 0)
                {
                    out.append(line);
                    continue;
                }
                auto skipSpaces = [&] {
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
                        ++k;
                };
                auto ident = [&] {
                    const size_t start = k;
                    while (k < line.size() && isIdent(line[k]))
                        ++k;
                    return std::string(line.substr(start, k - start));
                };
                k += 8;
                skipSpaces();
                const std::string type = ident();
                skipSpaces();
                const std::string name = ident();
                skipSpaces();
                uint32_t size = 0, align = 0;
                bool taken = !name.empty() && k < line.size() && line[k] == ';' && pushMemberType(type, size, align);
                if (taken)
                {
                    ++k;
                    skipSpaces();
                    taken = k >= line.size() || line[k] == '\r' || line[k] == '\n' || line.compare(k, 2, "//") == 0;
                }
                if (!taken)
                {
                    out.append(line);
                    continue;
                }
                auto same = std::find_if(members.begin(), members.end(), [&](const PushMember &m) { return m.name == name; });
                if (same == members.end())
                    members.push_back({type, name, size, align, 0});
                else if (same->type != type)
                    return false;
                out.push_back('\n');
            }
            return true;
        }

        // The block after the source's last #version / #extension line.
        void insertPushBlock(std::string &src, const std::string &block)
        {
            size_t insertAt = 0, pos = 0;
            while (pos < src.size())
            {
                size_t end = src.find('\n', pos);
                end = end == std::string::npos ? src.size() : end + 1;
                const size_t k = src.find_first_not_of(" \t", pos);
                if (k < end && (src.compare(k, 8, "#version") == 0 || src.compare(k, 10, "#extension") == 0))
                    insertAt = end;
                pos = end;
            }
            src.insert(insertAt, block);
        }

        // The program's shaders parsed and linked (shaders outlive program).
        struct ParsedProgram
        {
            std::vector<std::unique_ptr<glslang::TShader>> shaders;
            std::unique_ptr<glslang::TProgram> program;
        };

        bool parseProgram(const std::vector<std::pair<EShLanguage, std::string>> &sources, ParsedProgram &out, std::string &log)
        {
            out.program = std::make_unique<glslang::TProgram>();
            for (const auto &[stage, text] : sources)
            {
                out.shaders.push_back(std::make_unique<glslang::TShader>(stage));
                glslang::TShader &sh = *out.shaders.back();
                const char *strings[] = {text.c_str()};
                sh.setStrings(strings, 1);
                sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
                sh.setEnvInputVulkanRulesRelaxed();
                sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
                sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
                sh.setAutoMapBindings(true);
                sh.setAutoMapLocations(true);
                sh.setGlobalUniformBlockName("vkgl_Uniforms");
                sh.setGlobalUniformSet(0);
                sh.setGlobalUniformBinding(kUniformBinding);
                sh.setShiftBinding(glslang::EResImage, kImageShift);
                sh.setShiftBinding(glslang::EResCombinedSampler, kSamplerShift);
                if (!sh.parse(GetDefaultResources(), 450, false, EShMsgDefault))
                {
                    log = sh.getInfoLog();
                    return false;
                }
                out.program->addShader(&sh);
            }
            if (!out.program->link(EShMsgDefault) || !out.program->mapIO())
            {
                log = out.program->getInfoLog();
                return false;
            }
            return true;
        }

        bool linkProgram(Program &p)
        {
            p.linked = false;
            p.log.clear();
            bool haveVertex = false, haveFragment = false, haveCompute = false, inputAttachment = false;
            std::vector<std::pair<EShLanguage, std::string>> sources;
            // The pipeline store's names for the program: by its OpenGL source,
            // and by its variant (the #version / #extension / #define lines,
            // with the uniform names below).
            static uint64_t serials = 0;
            p.serial = ++serials;
            p.exactHash = 1469598103934665603ull;
            p.variantHash = 1469598103934665603ull;
            auto hashBytes = [](uint64_t &h, const void *data, size_t size) {
                const unsigned char *b = static_cast<const unsigned char *>(data);
                for (size_t i = 0; i < size; ++i)
                    h = (h ^ b[i]) * 1099511628211ull;
            };
            for (GLuint name : p.shaders)
            {
                Shader *s = g->shaders.get(name);
                if (!s || !s->compiled)
                {
                    p.log = "a shader did not compile";
                    return false;
                }
                const uint32_t type = s->type;
                hashBytes(p.exactHash, &type, sizeof(type));
                hashBytes(p.exactHash, s->source.data(), s->source.size());
                hashBytes(p.variantHash, &type, sizeof(type));
                for (size_t pos = 0; pos < s->source.size();)
                {
                    size_t end = s->source.find('\n', pos);
                    end = end == std::string::npos ? s->source.size() : end;
                    std::string_view line(s->source.data() + pos, end - pos);
                    pos = end + 1;
                    const size_t first = line.find_first_not_of(" \t");
                    if (first == std::string_view::npos)
                        continue;
                    line.remove_prefix(first);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                        line.remove_suffix(1);
                    // (GT4's GS_EFFECTS_SCALE, the target reads at each target's own scale: on with the effects scale
                    // and with the 480p menus' full-height car view; not part of the variant, so the built-in list's
                    // entries for the draw programs serve them either way: the same pipeline states.)
                    if (line == "#define GS_EFFECTS_SCALE 1")
                        continue;
                    if (line.rfind("#define", 0) == 0 || line.rfind("#extension", 0) == 0 || line.rfind("#version", 0) == 0)
                    {
                        hashBytes(p.variantHash, line.data(), line.size());
                        hashBytes(p.variantHash, "\n", 1);
                    }
                }
                inputAttachment |= s->inputAttachment;
                p.usesBindless |= s->bindless;
                const EShLanguage stage = stageOf(s->type);
                haveVertex |= stage == EShLangVertex;
                haveFragment |= stage == EShLangFragment;
                haveCompute |= stage == EShLangCompute;
                sources.emplace_back(stage, s->prepared);
            }
            const VkShaderStageFlags stageFlags = haveCompute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;

            // The loose uniforms as push constants when they fit (else, or
            // when that does not compile, the uniform buffer).
            ParsedProgram parsed;
            std::vector<PushMember> members;
            uint32_t pushSize = 0;
            if (pushConstantsOn())
            {
                std::vector<std::pair<EShLanguage, std::string>> pushSources = sources;
                bool ok = true;
                for (size_t i = 0; i < sources.size() && ok; ++i)
                    ok = extractLooseUniforms(sources[i].second, pushSources[i].second, members);
                std::stable_sort(members.begin(), members.end(), [](const PushMember &a, const PushMember &b) { return a.align > b.align; });
                for (PushMember &m : members)
                {
                    m.offset = static_cast<uint32_t>(alignUp(pushSize, m.align));
                    pushSize = m.offset + m.size;
                }
                pushSize = static_cast<uint32_t>(alignUp(pushSize, 4u));
                if (ok && !members.empty() && pushSize <= dev->properties.limits.maxPushConstantsSize)
                {
                    std::string block = "layout(push_constant) uniform VkglPush {";
                    for (const PushMember &m : members)
                        block += " layout(offset = " + std::to_string(m.offset) + ") " + m.type + " " + m.name + ";";
                    block += " };\n";
                    for (auto &src : pushSources)
                        insertPushBlock(src.second, block);
                    std::string log;
                    if (!parseProgram(pushSources, parsed, log))
                    {
                        std::fprintf(stderr, "[vkgl] push constants did not compile, a uniform buffer instead:\n%s\n", log.c_str());
                        parsed.program.reset();
                        parsed.shaders.clear();
                    }
                }
            }
            if (!parsed.program)
            {
                members.clear();
                pushSize = 0;
                if (!parseProgram(sources, parsed, p.log))
                {
                    std::fprintf(stderr, "[vkgl] program:\n%s\n", p.log.c_str());
                    return false;
                }
            }
            glslang::TProgram &tp = *parsed.program;
            p.compute = haveCompute;
            const EShLanguage stages[2] = {haveCompute ? EShLangCompute : EShLangVertex, EShLangFragment};
            const int stageCount = haveCompute ? 1 : 2;
            if (!haveCompute && (!haveVertex || !haveFragment))
            {
                p.log = "a graphics program needs a vertex and a fragment shader";
                return false;
            }
            for (int i = 0; i < stageCount; ++i)
            {
                std::vector<uint32_t> spirv;
                glslang::SpvOptions options;
                options.disableOptimizer = false;
                glslang::GlslangToSpv(*tp.getIntermediate(stages[i]), spirv, &options);
                saltSpirv(spirv);
                VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smci.codeSize = spirv.size() * 4u;
                smci.pCode = spirv.data();
                if (vkCreateShaderModule(vk, &smci, nullptr, &p.modules[i]) != VK_SUCCESS)
                {
                    p.log = "vkCreateShaderModule failed";
                    return false;
                }
            }

            // Reflection: the loose uniforms' blocks (push constants; the
            // uniform buffer for any the push block does not take), storage
            // buffers, samplers, images. uniformData: the push constants,
            // then the uniform buffer's data from uboBase.
            tp.buildReflection(EShReflectionSeparateBuffers);
            int globalBlock = -1, pushBlock = -1;
            uint32_t blockSize = 0;
            for (int i = 0; i < tp.getNumUniformBlocks(); ++i)
            {
                const glslang::TObjectReflection &b = tp.getUniformBlock(i);
                if (b.name == "vkgl_Uniforms")
                {
                    globalBlock = i;
                    blockSize = static_cast<uint32_t>(std::max(b.size, 0));
                }
                else if (b.name == "VkglPush")
                    pushBlock = i;
                else
                    std::fprintf(stderr, "[vkgl] unexpected uniform block %s (not bound)\n", b.name.c_str());
            }
            p.pushSize = members.empty() ? 0 : pushSize; // (declared in every stage, used or not)
            p.pushStages = stageFlags;
            p.pushDirty = true;
            p.uboBase = static_cast<uint32_t>(alignUp(p.pushSize, 16u));
            p.hasUniformBlock = globalBlock >= 0;
            p.uniformData.assign(p.uboBase + alignUp(std::max<uint32_t>(blockSize, 16u), 16u), 0);
            p.inputAttachment = inputAttachment;
            if (inputAttachment)
            {
                Resource r;
                r.binding = kInputBinding;
                r.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                p.resources.push_back(r);
            }
            for (int i = 0; i < tp.getNumBufferBlocks(); ++i)
            {
                const glslang::TObjectReflection &b = tp.getBufferBlock(i);
                Resource r;
                r.binding = static_cast<uint32_t>(b.getBinding());
                r.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                r.unit = static_cast<int>(r.binding);
                p.resources.push_back(r);
            }
            for (int i = 0; i < tp.getNumUniformVariables(); ++i)
            {
                const glslang::TObjectReflection &u = tp.getUniform(i);
                if (u.name.rfind("vkgl_", 0) == 0)
                    continue;
                Uniform un;
                un.name = u.name;
                if (isSamplerType(u.glDefineType) || isImageType(u.glDefineType))
                {
                    // Unused (dead code in this variant, e.g. the lean
                    // program's target reads): glslang gives it no binding.
                    if (u.getBinding() < 0)
                        continue;
                    Resource r;
                    r.binding = static_cast<uint32_t>(u.getBinding());
                    const bool image = isImageType(u.glDefineType);
                    r.type = image ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    r.unit = image ? static_cast<int>(r.binding) - kImageShift : 0;
                    r.uintSampler = isUintType(u.glDefineType);
                    un.resource = static_cast<int>(p.resources.size());
                    p.resources.push_back(r);
                }
                else if (u.index == pushBlock && pushBlock >= 0 && u.offset >= 0)
                    un.offset = u.offset;
                else if (u.index == globalBlock && globalBlock >= 0 && u.offset >= 0)
                    un.offset = static_cast<int>(p.uboBase) + u.offset;
                else
                    continue;
                p.byName[un.name] = static_cast<int>(p.uniforms.size());
                p.uniforms.push_back(un);
            }
            {
                std::vector<std::string> names;
                for (const Uniform &u : p.uniforms)
                    names.push_back(u.name);
                std::sort(names.begin(), names.end());
                for (const std::string &n : names)
                {
                    hashBytes(p.variantHash, n.data(), n.size());
                    hashBytes(p.variantHash, ";", 1);
                }
            }

            // GT3_VKGL_DEBUG=1: each program's uniforms and resources.
            static const bool debug = std::getenv("GT3_VKGL_DEBUG") && *std::getenv("GT3_VKGL_DEBUG") == '1';
            if (debug)
            {
                std::string line = haveCompute ? "compute" : "graphics";
                for (int i = 0; i < tp.getNumUniformVariables(); ++i)
                {
                    const glslang::TObjectReflection &u = tp.getUniform(i);
                    line += " " + u.name + "(type " + std::to_string(u.glDefineType) + " binding " + std::to_string(u.getBinding()) + " block " +
                            std::to_string(u.index) + " offset " + std::to_string(u.offset) + " stages " + std::to_string(u.stages) + ")";
                }
                std::fprintf(stderr, "[vkgl] program %s\n", line.c_str());
            }
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (const Resource &r : p.resources)
                bindings.push_back({r.binding, r.type, 1, r.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT ? VK_SHADER_STAGE_FRAGMENT_BIT : stageFlags, nullptr});
            if (p.hasUniformBlock)
                bindings.push_back({kUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stageFlags, nullptr});
            VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = static_cast<uint32_t>(bindings.size());
            dslci.pBindings = bindings.data();
            const VkResult setResult = vkCreateDescriptorSetLayout(vk, &dslci, nullptr, &p.setLayout);
            if (setResult != VK_SUCCESS)
            {
                p.log = "vkCreateDescriptorSetLayout failed (" + std::to_string(static_cast<int>(setResult)) + "); bindings:";
                for (const VkDescriptorSetLayoutBinding &b : bindings)
                    p.log += " " + std::to_string(b.binding) + "/" + std::to_string(static_cast<int>(b.descriptorType));
                for (const Uniform &u : p.uniforms)
                    p.log += " " + u.name;
                return false;
            }
            VkDescriptorSetLayout sets[2] = {p.setLayout, g->bindlessLayout};
            VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            plci.setLayoutCount = g->bindlessLayout ? 2u : 1u;
            plci.pSetLayouts = sets;
            const VkPushConstantRange pushRange{p.pushStages, 0, p.pushSize};
            plci.pushConstantRangeCount = p.pushSize ? 1u : 0u;
            plci.pPushConstantRanges = &pushRange;
            if (vkCreatePipelineLayout(vk, &plci, nullptr, &p.layout) != VK_SUCCESS)
            {
                p.log = "vkCreatePipelineLayout failed";
                return false;
            }
            if (haveCompute)
            {
                VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, p.modules[0], "main",
                              nullptr};
                cpci.layout = p.layout;
                if (vkCreateComputePipelines(vk, pipelineCache(), 1, &cpci, nullptr, &p.computePipeline) != VK_SUCCESS)
                {
                    p.log = "vkCreateComputePipelines failed";
                    return false;
                }
            }
            p.linked = true;
            return true;
        }

        VkBlendOp blendOp(GLenum e)
        {
            switch (e)
            {
            case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
            case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case GL_MIN: return VK_BLEND_OP_MIN;
            case GL_MAX: return VK_BLEND_OP_MAX;
            default: return VK_BLEND_OP_ADD;
            }
        }
        VkBlendFactor blendFactor(GLenum f)
        {
            switch (f)
            {
            case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
            case GL_ONE: return VK_BLEND_FACTOR_ONE;
            case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
            case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
            case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
            case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
            case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case GL_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
            case GL_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case GL_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
            case GL_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
            default: logOnce("blend factor", f); return VK_BLEND_FACTOR_ONE;
            }
        }
        VkCompareOp compareOp(GLenum f)
        {
            switch (f)
            {
            case GL_NEVER: return VK_COMPARE_OP_NEVER;
            case GL_LESS: return VK_COMPARE_OP_LESS;
            case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
            case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case GL_GREATER: return VK_COMPARE_OP_GREATER;
            case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
            case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            default: return VK_COMPARE_OP_ALWAYS;
            }
        }

        // The graphics pipeline of these modules, layout and state. Any thread:
        // the renderer's, or a pipeline store thread building one ahead (it
        // reads only the device and the pipeline cache, both shared).
        VkPipeline createGraphicsPipeline(VkShaderModule vertexModule, VkShaderModule fragmentModule, VkPipelineLayout layout, const PipelineKey &k)
        {
            VkPipelineShaderStageCreateInfo stages[2] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexModule, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr}};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationConservativeStateCreateInfoEXT cons{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
            cons.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.pNext = (k.conservative && dev->conservative) ? &cons : nullptr;
            rs.depthClampEnable = (k.depthClamp && dev->depthClamp) ? VK_TRUE : VK_FALSE;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            VkPipelineColorBlendAttachmentState att{};
            att.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
            att.colorBlendOp = blendOp(k.eqRGB);
            att.alphaBlendOp = blendOp(k.eqA);
            att.srcColorBlendFactor = blendFactor(k.srcRGB);
            att.dstColorBlendFactor = blendFactor(k.dstRGB);
            att.srcAlphaBlendFactor = blendFactor(k.srcA);
            att.dstAlphaBlendFactor = blendFactor(k.dstA);
            att.colorWriteMask = k.colorMask;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = k.colorFormat ? 1u : 0u;
            cb.pAttachments = &att;
            const VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                               VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 5;
            dy.pDynamicStates = dynamics;
            const VkFormat colorFormat = static_cast<VkFormat>(k.colorFormat);
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = k.colorFormat ? 1u : 0u;
            rendering.pColorAttachmentFormats = &colorFormat;
            rendering.depthAttachmentFormat = static_cast<VkFormat>(k.depthFormat);
            VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            gpci.stageCount = 2;
            gpci.pStages = stages;
            gpci.pVertexInputState = &vi;
            gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp;
            gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms;
            gpci.pDepthStencilState = &ds;
            gpci.pColorBlendState = &cb;
            gpci.pDynamicState = &dy;
            gpci.layout = layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(vk, pipelineCache(), 1, &gpci, nullptr, &pipeline) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[vkgl] vkCreateGraphicsPipelines failed\n");
                pipeline = VK_NULL_HANDLE;
            }
            return pipeline;
        }

        uint64_t nsSince(std::chrono::steady_clock::time_point t0)
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }

        // ------------------------------------------------------------------ pipeline store
        //
        // vkgl makes a graphics pipeline the first time a program draws with a
        // blend / format state, and the driver compiles it then. On a cold
        // driver cache (the first run after a new driver, new shaders or a new
        // install) that took about 2 s for each of the GS draw programs'
        // pipelines: about 10-19 s of stalls at boot, the movie and the first
        // car select (analysis/vulkan results.md, section 4). The store:
        //  - keeps a VkPipelineCache in a file, vk-pipelines.cache in the game
        //    folder's cache\ (the CORE.elf folder, where the memory card is:
        //    <root>\game\cache, see defaultCacheFolder; not .bin, which the
        //    package's tools take for a disc image; GT3_VKGL_PIPELINE_CACHE=<file>
        //    elsewhere, =0 none). It is used only when this device and driver made it (vendor,
        //    device, driver version, pipeline cache and driver UUIDs, in its
        //    header and in Vulkan's) and it is whole (size, checksum); every
        //    pipeline creation gets it; it is written whole into a new file
        //    renamed over the old one, a few seconds after new pipelines were
        //    made and when the game exits (gsvk::pipelineCacheSave, the last
        //    write); a failed save is tried again with the next new pipelines
        //    and at the exit;
        //  - keeps the list of the pipelines made (vk-pipelines-keys.txt next to
        //    it: the program by its source and by its variant, and the state),
        //    in the order a run first needed them, plus a built-in list of
        //    GT4's (gs_vk_pipeline_keys.h). When the renderer links a program,
        //    the pipelines listed for it - for its source, or for its variant
        //    (an older source: before an edit of a shader's body) - are built
        //    on store threads, below normal priority, earliest needed first;
        //  - hands them to the renderer: a draw whose pipeline a store thread
        //    is still building waits for it (the thread is raised to normal
        //    priority meanwhile); one not started yet is built on the spot, as
        //    before. The modules, layout and state are the draw's either way,
        //    so it is the same pipeline (the cache only saves the compile).
        // GT3_VKGL_PREBUILD=0: nothing built ahead; GT3_VKGL_PREBUILD_THREADS=<n>;
        // GT3_VKGL_PREBUILD_BUILTIN=0: without the built-in list (tests);
        // GT3_VKGL_PREPARE=1: build the listed pipelines, save and exit (for a
        // launcher's "Preparing shaders" step; gsvk::pipelinePrepareRun);
        // GT3_VKGL_PIPELINE_LOG=1: a line per pipeline. Defaults: on for GT4;
        // for GT3 (PS2X_GAME_GT3) off, as before (no store, no cache object),
        // unless GT3_VKGL_PIPELINE_CACHE / GT3_VKGL_PREBUILD turn it on.

        uint64_t fnv1a(const void *data, size_t size, uint64_t h = 1469598103934665603ull)
        {
            const unsigned char *p = static_cast<const unsigned char *>(data);
            for (size_t i = 0; i < size; ++i)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        // A program (by link serial, or by exact source hash in the key list) and a state.
        struct StoreKey
        {
            uint64_t program = 0;
            PipelineKey key;
            bool operator==(const StoreKey &o) const { return program == o.program && key == o.key; }
        };
        struct StoreKeyHash
        {
            size_t operator()(const StoreKey &k) const { return PipelineKeyHash()(k.key) ^ static_cast<size_t>(k.program * 0x9E3779B97F4A7C15ull); }
        };

        struct ListedKey
        {
            uint64_t exact = 0, variant = 0;
            PipelineKey key;
            bool builtIn = false;
        };

        struct Job
        {
            uint64_t program = 0; // the Program's link serial
            GLuint name = 0;
            PipelineKey key;
            VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            size_t order = 0; // in the key list: earlier needed first
            bool byVariant = false, builtIn = false;
            enum class State
            {
                Queued,
                Running,
                Done
            } state = State::Queued;
            int thread = -1;
            VkPipeline pipeline = VK_NULL_HANDLE;
            uint64_t ns = 0;
            std::multimap<size_t, std::shared_ptr<Job>>::iterator queued;
        };

        struct Store
        {
            std::mutex mutex;
            std::condition_variable wake, done;
            bool prebuild = false, persist = false, log = false, prepare = false;
            std::multimap<size_t, std::shared_ptr<Job>> queue;
            std::unordered_map<StoreKey, std::shared_ptr<Job>, StoreKeyHash> jobs; // built ahead or queued, not yet taken
            unsigned running = 0;
            std::vector<std::thread> threads;
            std::vector<HANDLE> handles;
            // Keys: listed (the file's, then the built-in ones it lacks), made in this run.
            std::vector<ListedKey> listed, made;
            std::unordered_set<StoreKey, StoreKeyHash> listedSet, madeSet; // (exact source hash, state)
            std::unordered_set<StoreKey, StoreKeyHash> fileSet;            // the keys the key file holds (loaded or last written)
            std::vector<std::pair<uint64_t, uint64_t>> linked;            // (exact, variant) of the programs linked
            size_t fileKeys = 0, builtInKeys = 0, badKeys = 0;
            // The cache and its files.
            VkPipelineCache cache = VK_NULL_HANDLE;
            std::filesystem::path cachePath, keysPath;
            uint8_t driverUUID[VK_UUID_SIZE] = {};
            size_t loadedBytes = 0;
            uint64_t dirty = 0; // pipelines or keys new since the last save
            bool saveFailed = false; // the last save failed: again after new pipelines, or at the exit
            bool closed = false;     // the exit (or prepare) save is done: nothing more is written (GT4 ends with _Exit)
            std::chrono::steady_clock::time_point start, lastNew, lastSave;
            std::mutex saveMutex;
            // Counts: built ahead (store threads), made on the spot (renderer),
            // taken ready / after a wait, queued ones the renderer took over.
            uint64_t aheadBuilt = 0, aheadFailed = 0, aheadNs = 0, onSpot = 0, onSpotNs = 0, takenReady = 0, takenWaited = 0, waitNs = 0, takenOver = 0,
                     saves = 0;
            uint64_t aheadSince = 0; // (for the per-5 s stats)
        };
        Store *g_store = nullptr;

        VkPipelineCache pipelineCache() { return g_store ? g_store->cache : VK_NULL_HANDLE; }

        double storeSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_store->start).count(); }

        std::string keyText(const PipelineKey &k)
        {
            char b[200];
            std::snprintf(b, sizeof(b), "colour format %u, depth format %u, mask %u, blend %u (%x %x / %x %x %x %x), conservative %u, depth clamp %u", k.colorFormat,
                          k.depthFormat, k.colorMask, k.blend, k.eqRGB, k.eqA, k.srcRGB, k.dstRGB, k.srcA, k.dstA, k.conservative, k.depthClamp);
            return b;
        }

        // A path for the log, as UTF-8 (path::string() throws for characters the
        // ANSI code page lacks, e.g. in a user folder's name).
        std::string pathText(const std::filesystem::path &p)
        {
            const std::wstring &w = p.native();
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string out(n > 0 ? static_cast<size_t>(n) : 0u, '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        bool knownBlendEquation(unsigned e) { return e == GL_FUNC_ADD || e == GL_FUNC_SUBTRACT || e == GL_FUNC_REVERSE_SUBTRACT || e == GL_MIN || e == GL_MAX; }
        bool knownBlendFactor(unsigned f, bool &dualSource)
        {
            switch (f)
            {
            case GL_ZERO: case GL_ONE: case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
            case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA: case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
                return true;
            case GL_SRC1_COLOR: case GL_ONE_MINUS_SRC1_COLOR: case GL_SRC1_ALPHA: case GL_ONE_MINUS_SRC1_ALPHA:
                dualSource = true;
                return true;
            default:
                return false;
            }
        }

        // A key read from a file: only states a draw of this device can have
        // (draw() leaves the blend fields 0 without blending).
        bool validKey(const PipelineKey &k)
        {
            if (k.colorMask > 15 || k.blend > 1 || k.conservative > 1 || k.depthClamp > 1)
                return false;
            if (k.depthFormat != 0 && k.depthFormat != static_cast<uint32_t>(VK_FORMAT_D32_SFLOAT))
                return false;
            bool integer = false;
            if (k.colorFormat)
            {
                bool known = false;
                for (GLenum internal : {GL_RGBA8, GL_R8, GL_R8UI, GL_R16UI, GL_R32UI, GL_RG16UI})
                {
                    FormatInfo fi{};
                    if (formatInfo(internal, fi) && static_cast<uint32_t>(fi.format) == k.colorFormat && !fi.depth)
                    {
                        known = true;
                        integer = fi.integer;
                    }
                }
                if (!known)
                    return false;
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(dev->physical, static_cast<VkFormat>(k.colorFormat), &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                    return false;
                if (k.blend && (integer || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)))
                    return false;
            }
            if (!k.blend)
                return !k.eqRGB && !k.eqA && !k.srcRGB && !k.dstRGB && !k.srcA && !k.dstA;
            bool dualSource = false;
            const bool known = knownBlendEquation(k.eqRGB) && knownBlendEquation(k.eqA) && knownBlendFactor(k.srcRGB, dualSource) &&
                               knownBlendFactor(k.dstRGB, dualSource) && knownBlendFactor(k.srcA, dualSource) && knownBlendFactor(k.dstA, dualSource);
            return known && (!dualSource || dev->dualSrcBlend) && k.colorFormat;
        }

        constexpr const char *kKeysHeader = "# vkgl pipeline keys 1";
        constexpr size_t kMaxKeys = 1024;

        void addListed(Store &s, const ListedKey &e)
        {
            if (s.listedSet.insert({e.exact, e.key}).second)
                s.listed.push_back(e);
        }

        void loadKeys(Store &s)
        {
            std::ifstream in(s.keysPath);
            std::string line;
            if (!in || !std::getline(in, line))
                return;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line != kKeysHeader)
            {
                std::fprintf(stderr, "[vkgl] pipeline keys: %s is not a version 1 key list; not used\n", pathText(s.keysPath).c_str());
                return;
            }
            while (std::getline(in, line) && s.listed.size() < kMaxKeys)
            {
                if (line.empty() || line[0] == '#' || line[0] == '\r')
                    continue;
                unsigned long long exact = 0, variant = 0;
                unsigned cf = 0, df = 0, mask = 0, blend = 0, cons = 0, clamp = 0, eqRGB = 0, eqA = 0, srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0;
                if (std::sscanf(line.c_str(), "%llx %llx %u %u %u %u %u %u %x %x %x %x %x %x", &exact, &variant, &cf, &df, &mask, &blend, &cons, &clamp,
                                &eqRGB, &eqA, &srcRGB, &dstRGB, &srcA, &dstA) != 14 ||
                    mask > 255 || blend > 255 || cons > 255 || clamp > 255 || eqRGB > 0xFFFF || eqA > 0xFFFF || srcRGB > 0xFFFF || dstRGB > 0xFFFF ||
                    srcA > 0xFFFF || dstA > 0xFFFF)
                {
                    ++s.badKeys;
                    continue;
                }
                ListedKey e;
                e.exact = exact;
                e.variant = variant;
                e.key.colorFormat = cf;
                e.key.depthFormat = df;
                e.key.colorMask = static_cast<uint8_t>(mask);
                e.key.blend = static_cast<uint8_t>(blend);
                e.key.conservative = static_cast<uint8_t>(cons);
                e.key.depthClamp = static_cast<uint8_t>(clamp);
                e.key.eqRGB = static_cast<uint16_t>(eqRGB);
                e.key.eqA = static_cast<uint16_t>(eqA);
                e.key.srcRGB = static_cast<uint16_t>(srcRGB);
                e.key.dstRGB = static_cast<uint16_t>(dstRGB);
                e.key.srcA = static_cast<uint16_t>(srcA);
                e.key.dstA = static_cast<uint16_t>(dstA);
                if (!validKey(e.key))
                {
                    ++s.badKeys;
                    continue;
                }
                const size_t before = s.listed.size();
                addListed(s, e);
                s.fileKeys += s.listed.size() - before;
                s.fileSet.insert({e.exact, e.key});
            }
        }

        // The key list to write: this run's keys (first needed first), then the
        // file's others. One of an older source (its source not linked in this
        // run, a program of its variant linked) stays while nothing written has
        // its variant and state: it still serves that variant; it goes once a
        // run has made that state with the new source.
        std::string keysText(const Store &s, std::unordered_set<StoreKey, StoreKeyHash> &written)
        {
            std::unordered_set<StoreKey, StoreKeyHash> writtenVariants; // (variant hash, state)
            std::string out = std::string(kKeysHeader) +
                              "\n# <source hash> <variant hash> <colour VkFormat> <depth VkFormat> <colour mask> <blend> <conservative> <depth clamp> "
                              "<eqRGB> <eqA> <srcRGB> <dstRGB> <srcA> <dstA> (OpenGL enums, hex); the first needed first. Written by GT4.exe "
                              "(gs_vkgl.cpp, pipeline store); safe to delete.\n";
            written.clear();
            auto add = [&](const ListedKey &e) {
                if (written.size() >= kMaxKeys || !written.insert({e.exact, e.key}).second)
                    return;
                writtenVariants.insert({e.variant, e.key});
                const PipelineKey &k = e.key;
                char line[256];
                std::snprintf(line, sizeof(line), "%016llx %016llx %u %u %u %u %u %u %x %x %x %x %x %x\n", static_cast<unsigned long long>(e.exact),
                              static_cast<unsigned long long>(e.variant), k.colorFormat, k.depthFormat, k.colorMask, k.blend, k.conservative, k.depthClamp,
                              k.eqRGB, k.eqA, k.srcRGB, k.dstRGB, k.srcA, k.dstA);
                out += line;
            };
            for (const ListedKey &e : s.made)
                add(e);
            for (const ListedKey &e : s.listed)
            {
                if (e.builtIn)
                    continue; // (in the exe)
                bool sourceLinked = false, variantLinked = false;
                for (const auto &[exact, variant] : s.linked)
                {
                    sourceLinked |= exact == e.exact;
                    variantLinked |= variant == e.variant;
                }
                if (!sourceLinked && variantLinked && writtenVariants.count({e.variant, e.key}))
                    continue; // (replaced: the new source has this state)
                add(e);
            }
            return out;
        }

        // The cache file: this header, then vkGetPipelineCacheData's bytes.
        struct CacheFileHeader
        {
            char magic[8];
            uint32_t headerSize;
            uint32_t vendorID, deviceID, driverVersion;
            uint8_t pipelineCacheUUID[VK_UUID_SIZE];
            uint8_t driverUUID[VK_UUID_SIZE];
            uint64_t dataSize;
            uint64_t dataHash; // fnv1a of the data
        };
        static_assert(sizeof(CacheFileHeader) == 72, "the cache file header is 72 bytes");
        constexpr char kCacheMagic[8] = {'V', 'K', 'G', 'L', 'P', 'C', '0', '1'};

        std::vector<uint8_t> loadCacheData(const Store &s, std::string &why)
        {
            std::ifstream in(s.cachePath, std::ios::binary);
            if (!in)
            {
                why = "absent";
                return {};
            }
            const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CacheFileHeader h{};
            if (file.size() < sizeof(h))
            {
                why = "discarded: too short";
                return {};
            }
            std::memcpy(&h, file.data(), sizeof(h));
            const VkPhysicalDeviceProperties &pr = dev->properties;
            if (std::memcmp(h.magic, kCacheMagic, sizeof(h.magic)) != 0 || h.headerSize != sizeof(h))
            {
                why = "discarded: not a vkgl pipeline cache file of this version";
                return {};
            }
            if (h.vendorID != pr.vendorID || h.deviceID != pr.deviceID)
            {
                why = "discarded: made on another GPU";
                return {};
            }
            if (h.driverVersion != pr.driverVersion || std::memcmp(h.pipelineCacheUUID, pr.pipelineCacheUUID, VK_UUID_SIZE) != 0 ||
                std::memcmp(h.driverUUID, s.driverUUID, VK_UUID_SIZE) != 0)
            {
                why = "discarded: made by another driver";
                return {};
            }
            if (h.dataSize != file.size() - sizeof(h))
            {
                why = "discarded: truncated";
                return {};
            }
            const uint8_t *data = file.data() + sizeof(h);
            if (fnv1a(data, static_cast<size_t>(h.dataSize)) != h.dataHash)
            {
                why = "discarded: damaged (checksum)";
                return {};
            }
            VkPipelineCacheHeaderVersionOne vh{};
            if (h.dataSize < sizeof(vh))
            {
                why = "discarded: no Vulkan cache header";
                return {};
            }
            std::memcpy(&vh, data, sizeof(vh));
            if (vh.headerSize < sizeof(vh) || vh.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vh.vendorID != pr.vendorID ||
                vh.deviceID != pr.deviceID || std::memcmp(vh.pipelineCacheUUID, pr.pipelineCacheUUID, VK_UUID_SIZE) != 0)
            {
                why = "discarded: its Vulkan header is another device's or driver's";
                return {};
            }
            why = "loaded";
            return std::vector<uint8_t>(data, data + h.dataSize);
        }

        // Writes the parts into a new file next to path, then renames it over
        // path: a reader (or another GT4) sees the old file or the new one.
        bool writeFileAtomic(const std::filesystem::path &path, std::initializer_list<std::pair<const void *, size_t>> parts, std::string &error)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::filesystem::path temp = path;
            temp += L".new" + std::to_wstring(GetCurrentProcessId());
            const HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                const DWORD code = GetLastError();
                error = "cannot create " + pathText(temp) + " (error " + std::to_string(code) + ")";
                return false;
            }
            bool ok = true;
            DWORD code = 0;
            for (const auto &[data, size] : parts)
            {
                size_t done = 0;
                while (ok && done < size)
                {
                    DWORD wrote = 0;
                    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 24));
                    ok = WriteFile(h, static_cast<const uint8_t *>(data) + done, chunk, &wrote, nullptr) && wrote == chunk;
                    done += wrote;
                }
            }
            ok = ok && FlushFileBuffers(h);
            if (!ok)
                code = GetLastError();
            CloseHandle(h);
            if (!ok)
            {
                error = "cannot write " + pathText(temp) + " (error " + std::to_string(code) + ")";
                DeleteFileW(temp.c_str());
                return false;
            }
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                code = GetLastError();
                error = "cannot replace " + pathText(path) + " (error " + std::to_string(code) + ")";
                DeleteFileW(temp.c_str());
                return false;
            }
            return true;
        }

        // The cache and the key list into their files (any thread; one save at a time).
        // last (the exit, prepare): the last save; a store thread's save that
        // waited for it, or starts after it, writes nothing (GT4 ends with
        // _Exit, which would cut a save short and leave its .new file). A save
        // that finds nothing new since the last one (it waited while another
        // saved; the exit after a quiet end) writes nothing.
        bool storeSave(const char *why, bool last = false)
        {
            Store *s = g_store;
            if (!s || !s->persist || !s->cache)
                return false;
            std::lock_guard<std::mutex> saveLock(s->saveMutex);
            std::string keys;
            std::unordered_set<StoreKey, StoreKeyHash> written;
            uint64_t dirtyBefore = 0;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (s->closed)
                    return false;
                if (last)
                    s->closed = true;
                if (!s->dirty && !(last && s->prepare))
                    return true; // (nothing new since the last save; prepare always writes)
                keys = keysText(*s, written);
                dirtyBefore = s->dirty;
                s->dirty = 0;
                s->lastSave = std::chrono::steady_clock::now();
            }
            const size_t keyCount = written.size();
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint8_t> data;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                size_t size = 0;
                if (vkGetPipelineCacheData(vk, s->cache, &size, nullptr) != VK_SUCCESS)
                    break;
                data.resize(size);
                const VkResult r = vkGetPipelineCacheData(vk, s->cache, &size, data.data());
                if (r == VK_SUCCESS)
                {
                    data.resize(size);
                    break;
                }
                data.clear();
                if (r != VK_INCOMPLETE)
                    break; // (incomplete: it grew meanwhile; again)
            }
            std::string error;
            bool ok = !data.empty();
            if (ok)
            {
                CacheFileHeader h{};
                std::memcpy(h.magic, kCacheMagic, sizeof(h.magic));
                h.headerSize = sizeof(h);
                h.vendorID = dev->properties.vendorID;
                h.deviceID = dev->properties.deviceID;
                h.driverVersion = dev->properties.driverVersion;
                std::memcpy(h.pipelineCacheUUID, dev->properties.pipelineCacheUUID, VK_UUID_SIZE);
                std::memcpy(h.driverUUID, s->driverUUID, VK_UUID_SIZE);
                h.dataSize = data.size();
                h.dataHash = fnv1a(data.data(), data.size());
                ok = writeFileAtomic(s->cachePath, {{&h, sizeof(h)}, {data.data(), data.size()}}, error);
            }
            else
                error = "vkGetPipelineCacheData gave nothing";
            ok = writeFileAtomic(s->keysPath, {{keys.data(), keys.size()}}, error) && ok;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                ++s->saves;
                s->saveFailed = !ok;
                if (ok)
                    s->fileSet = std::move(written); // (what the key file now holds)
                else
                    s->dirty += std::max<uint64_t>(dirtyBefore, 1u); // (still to save: with the next new pipelines, or at the exit)
            }
            if (ok)
                std::fprintf(stderr, "[vkgl] pipeline cache saved (%s, +%.1f s): %zu bytes, %zu keys, in %.0f ms (%s)\n", why, storeSeconds(), data.size(),
                             keyCount, nsSince(t0) / 1e6, pathText(s->cachePath).c_str());
            else
                std::fprintf(stderr, "[vkgl] pipeline cache not saved (%s): %s\n", why, error.c_str());
            return ok;
        }

        void nameThisThread(const wchar_t *name)
        {
            using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
            if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
                if (auto fn = reinterpret_cast<SetThreadDescriptionFn>(reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))))
                    fn(GetCurrentThread(), name);
        }

        // A store thread: builds queued pipelines (earliest needed first);
        // thread 0 also saves, 3 s after the last new pipeline (or 30 s after
        // the last save while new ones keep coming). After a failed save (a
        // read-only folder, the file held open elsewhere): again only once
        // new pipelines came after it, so a folder that stays unwritable costs
        // no more attempts than before; gsvk::pipelineCacheSave tries at the exit.
        void storeThread(Store *s, unsigned index)
        {
            nameThisThread((L"vkgl pipelines " + std::to_wstring(index)).c_str());
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            std::unique_lock<std::mutex> lock(s->mutex);
            for (;;)
            {
                if (!s->queue.empty())
                {
                    const auto first = s->queue.begin();
                    const std::shared_ptr<Job> job = first->second;
                    s->queue.erase(first);
                    job->state = Job::State::Running;
                    job->thread = static_cast<int>(index);
                    ++s->running;
                    lock.unlock();
                    const auto t0 = std::chrono::steady_clock::now();
                    const VkPipeline pipeline = createGraphicsPipeline(job->vertex, job->fragment, job->layout, job->key);
                    const uint64_t ns = nsSince(t0);
                    lock.lock();
                    job->pipeline = pipeline;
                    job->ns = ns;
                    job->state = Job::State::Done;
                    --s->running;
                    s->aheadNs += ns;
                    if (pipeline)
                    {
                        ++s->aheadBuilt;
                        ++s->aheadSince;
                        ++s->dirty;
                        s->lastNew = std::chrono::steady_clock::now();
                    }
                    else
                        ++s->aheadFailed; // (a draw that needs it makes it on the spot; prepare: not done)
                    if (s->log || !pipeline)
                        std::fprintf(stderr, "[vkgl] pipeline +%.3f s: program %u, %s: %s on store thread %u in %.1f ms (listed%s for its %s)\n",
                                     storeSeconds(), job->name, keyText(job->key).c_str(), pipeline ? "built ahead" : "build failed", index, ns / 1e6,
                                     job->builtIn ? " built in" : "", job->byVariant ? "variant" : "source");
                    s->done.notify_all();
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (index == 0 && s->persist && s->dirty && !s->prepare && !s->closed && (!s->saveFailed || s->lastNew > s->lastSave) &&
                    (now - s->lastNew >= std::chrono::seconds(3) || now - s->lastSave >= std::chrono::seconds(30)))
                {
                    lock.unlock();
                    storeSave("new pipelines");
                    lock.lock();
                    continue;
                }
                s->wake.wait_for(lock, std::chrono::milliseconds(500));
            }
        }

        // The game folder's cache\ (with the memory card, mc0\: GT4's files on
        // this computer): the folder of the ELF named on the command line
        // (<root>\game\CORE.elf), else, started without arguments, <exe
        // folder>\game as main.cpp's portableStart.
        std::filesystem::path defaultCacheFolder()
        {
            std::filesystem::path folder;
            std::error_code ec;
            int argc = 0;
            if (LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc))
            {
                if (argc >= 2 && argv[1][0] != L'-' && std::filesystem::is_regular_file(argv[1], ec))
                    folder = std::filesystem::absolute(std::filesystem::path(argv[1]), ec).parent_path();
                LocalFree(argv);
            }
            if (folder.empty())
            {
                wchar_t buffer[4096];
                const DWORD n = GetModuleFileNameW(nullptr, buffer, 4096);
                const std::filesystem::path exeFolder = n && n < 4096 ? std::filesystem::path(std::wstring(buffer, n)).parent_path() : std::filesystem::current_path(ec);
                folder = exeFolder;
                for (const std::filesystem::path &game : {exeFolder / "game", exeFolder / "windows-diagnostic" / "game"})
                    if (std::filesystem::exists(game / "CORE.elf", ec))
                    {
                        folder = game;
                        break;
                    }
            }
            return folder / "cache";
        }

        bool envIs(const char *name, const char *value)
        {
            const char *v = std::getenv(name);
            return v && std::strcmp(v, value) == 0;
        }

        void storeInit()
        {
#if defined(PS2X_GAME_GT3)
            const bool gameDefault = false; // GT3: as before unless asked
#else
            const bool gameDefault = true;
#endif
            const char *cacheEnv = std::getenv("GT3_VKGL_PIPELINE_CACHE");
            const char *prebuildEnv = std::getenv("GT3_VKGL_PREBUILD");
            const bool prepare = envIs("GT3_VKGL_PREPARE", "1");
            const bool persist = (cacheEnv && *cacheEnv) ? std::strcmp(cacheEnv, "0") != 0 : (gameDefault || prepare);
            const bool prebuild = prepare || ((prebuildEnv && *prebuildEnv) ? std::strcmp(prebuildEnv, "0") != 0 : gameDefault);
            if (!persist && !prebuild)
                return;
            auto *s = new Store();
            s->start = s->lastNew = s->lastSave = std::chrono::steady_clock::now();
            s->persist = persist;
            s->prebuild = prebuild;
            s->prepare = prepare;
            s->log = envIs("GT3_VKGL_PIPELINE_LOG", "1");
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
            vkGetPhysicalDeviceProperties2(dev->physical, &props);
            std::memcpy(s->driverUUID, id.driverUUID, VK_UUID_SIZE);
            std::vector<uint8_t> initial;
            std::string state = "not kept (GT3_VKGL_PIPELINE_CACHE=0)";
            if (persist)
            {
                std::error_code ec;
                s->cachePath = (cacheEnv && *cacheEnv && std::strcmp(cacheEnv, "1") != 0) ? std::filesystem::path(cacheEnv)
                                                                                           : defaultCacheFolder() / "vk-pipelines.cache";
                s->cachePath = std::filesystem::absolute(s->cachePath, ec);
                s->keysPath = s->cachePath;
                s->keysPath.replace_filename(s->cachePath.stem().wstring() + L"-keys.txt");
                initial = loadCacheData(*s, state);
                s->loadedBytes = initial.size();
                loadKeys(*s);
            }
#if !defined(PS2X_GAME_GT3)
            if (prebuild && !envIs("GT3_VKGL_PREBUILD_BUILTIN", "0"))
                for (const gsvk::BuiltInPipelineKey &b : gsvk::kGt4PipelineKeys)
                {
                    ListedKey e;
                    e.exact = b.exact;
                    e.variant = b.variant;
                    e.key.colorFormat = b.colorFormat;
                    e.key.depthFormat = b.depthFormat;
                    e.key.colorMask = b.colorMask;
                    e.key.blend = b.blend;
                    e.key.conservative = b.conservative;
                    e.key.depthClamp = b.depthClamp;
                    e.key.eqRGB = b.eqRGB;
                    e.key.eqA = b.eqA;
                    e.key.srcRGB = b.srcRGB;
                    e.key.dstRGB = b.dstRGB;
                    e.key.srcA = b.srcA;
                    e.key.dstA = b.dstA;
                    e.builtIn = true;
                    if (!validKey(e.key))
                        continue;
                    const size_t before = s->listed.size();
                    addListed(*s, e);
                    s->builtInKeys += s->listed.size() - before;
                }
#endif
            VkPipelineCacheCreateInfo pcci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            pcci.initialDataSize = initial.size();
            pcci.pInitialData = initial.empty() ? nullptr : initial.data();
            if (vkCreatePipelineCache(vk, &pcci, nullptr, &s->cache) != VK_SUCCESS)
            {
                state = "discarded: the driver refused it";
                s->loadedBytes = 0;
                pcci.initialDataSize = 0;
                pcci.pInitialData = nullptr;
                if (vkCreatePipelineCache(vk, &pcci, nullptr, &s->cache) != VK_SUCCESS)
                {
                    s->cache = VK_NULL_HANDLE; // (pipelines made without one, as before; nothing saved)
                    s->persist = false;        // (else store thread 0 would retry a save it cannot make, without pause)
                    state = "not kept: the driver made no pipeline cache";
                }
            }
            unsigned threads = 0;
            if (prebuild)
            {
                // Two while the game runs: GT4's first two GS pipelines are
                // needed together at boot, and more compiles at once slow each
                // one (cold driver cache, RTX 5060 laptop, one run each: the
                // slow ones 2.0-2.5 s each with two threads, 2.5-3.0 s with
                // four; the boot wait 2.1 s against 2.5 s; analysis/vulkan
                // results.md section 6). Preparing only: up to 8 (nothing else
                // runs; 17 in 5.3 s).
                const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
                threads = prepare ? std::clamp(hw / 2u, 1u, 8u) : (hw >= 8u ? 2u : 1u);
                if (const char *v = std::getenv("GT3_VKGL_PREBUILD_THREADS"); v && *v)
                    threads = std::clamp<unsigned>(static_cast<unsigned>(std::strtoul(v, nullptr, 10)), 1u, 16u);
            }
            else if (s->persist)
                threads = 1; // (saves only)
            std::fprintf(stderr,
                         "[vkgl] pipeline cache: %s %s (%zu bytes); pipeline keys: %zu from the file%s, %zu built in; %s on %u store thread%s%s\n",
                         persist ? pathText(s->cachePath).c_str() : "(none)", state.c_str(), s->loadedBytes, s->fileKeys,
                         s->badKeys ? (" (" + std::to_string(s->badKeys) + " unusable lines skipped)").c_str() : "", s->builtInKeys,
                         prebuild ? "built ahead" : "nothing built ahead", threads, threads == 1 ? "" : "s", prepare ? " (prepare only)" : "");
            g_store = s;
            for (unsigned i = 0; i < threads; ++i)
            {
                s->threads.emplace_back(storeThread, s, i);
                s->handles.push_back(static_cast<HANDLE>(s->threads.back().native_handle()));
            }
        }

        // The renderer linked graphics program `name`: its listed pipelines to
        // the store threads.
        void storeProgramLinked(GLuint name, const Program &p)
        {
            Store *s = g_store;
            if (!s)
                return;
            std::lock_guard<std::mutex> lock(s->mutex);
            s->linked.emplace_back(p.exactHash, p.variantHash);
            if (!s->prebuild)
                return;
            // Listed for its source or for its variant (an entry of an older
            // source of the same variant: before an edit of the shader's body).
            // A variant match the program never draws with costs one
            // background compile; one it draws with is the draw's own pipeline.
            size_t queued = 0, byVariant = 0;
            for (size_t i = 0; i < s->listed.size(); ++i)
            {
                const ListedKey &e = s->listed[i];
                if (e.exact != p.exactHash && e.variant != p.variantHash)
                    continue;
                const StoreKey id{p.serial, e.key};
                if (const auto found = s->jobs.find(id); found != s->jobs.end())
                {
                    // (One job per state. Listed for its variant first, then also for
                    // its source: counted as listed for its source.)
                    if (found->second->byVariant && e.exact == p.exactHash)
                    {
                        found->second->byVariant = false;
                        found->second->builtIn = e.builtIn;
                        --byVariant;
                    }
                    continue;
                }
                auto job = std::make_shared<Job>();
                job->program = p.serial;
                job->name = name;
                job->key = e.key;
                job->vertex = p.modules[0];
                job->fragment = p.modules[1];
                job->layout = p.layout;
                job->order = i;
                job->byVariant = e.exact != p.exactHash;
                job->builtIn = e.builtIn;
                job->queued = s->queue.emplace(i, job);
                s->jobs.emplace(id, job);
                ++queued;
                byVariant += job->byVariant;
            }
            if (s->log || byVariant)
                std::fprintf(stderr, "[vkgl] pipeline store +%.3f s: program %u (source %016llx, variant %016llx): %zu pipelines to build ahead (%zu listed for "
                             "its source, %zu only for its variant%s)\n",
                             storeSeconds(), name, static_cast<unsigned long long>(p.exactHash), static_cast<unsigned long long>(p.variantHash), queued,
                             queued - byVariant, byVariant, byVariant ? ": an older source of it" : "");
            if (queued)
                s->wake.notify_all();
        }

        enum class Origin
        {
            OnSpot,     // nothing listed: made by the renderer
            TakenOver,  // listed but not started: made by the renderer
            Ready,      // built ahead
            Waited      // a store thread was building it: the renderer waited
        };

        // The pipeline a store thread built (or is building: then waited for),
        // VK_NULL_HANDLE when the renderer makes it (none queued, or queued and
        // not started: taken out of the queue).
        VkPipeline storeTake(const Program &p, const PipelineKey &k, Origin &origin, uint64_t &builtNs)
        {
            origin = Origin::OnSpot;
            builtNs = 0;
            Store *s = g_store;
            if (!s)
                return VK_NULL_HANDLE;
            std::unique_lock<std::mutex> lock(s->mutex);
            const StoreKey id{p.serial, k};
            const auto it = s->jobs.find(id);
            if (it == s->jobs.end())
                return VK_NULL_HANDLE;
            const std::shared_ptr<Job> job = it->second;
            if (job->state == Job::State::Queued)
            {
                s->queue.erase(job->queued);
                s->jobs.erase(it);
                origin = Origin::TakenOver;
                s->done.notify_all(); // (the queue changed: a prepare waits for it to empty)
                return VK_NULL_HANDLE;
            }
            if (job->state == Job::State::Running)
            {
                const HANDLE thread = job->thread >= 0 && static_cast<size_t>(job->thread) < s->handles.size() ? s->handles[job->thread] : nullptr;
                if (thread)
                    SetThreadPriority(thread, THREAD_PRIORITY_NORMAL); // (the renderer waits on it)
                s->done.wait(lock, [&] { return job->state == Job::State::Done; });
                if (thread)
                    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
                origin = Origin::Waited;
            }
            else
                origin = Origin::Ready;
            builtNs = job->ns;
            s->jobs.erase(id);
            return job->pipeline;
        }

        // The renderer has the pipeline (rendererNs: its time getting it).
        void storeNote(GLuint name, const Program &p, const PipelineKey &k, Origin origin, uint64_t rendererNs, uint64_t builtNs)
        {
            Store *s = g_store;
            if (!s)
                return;
            std::lock_guard<std::mutex> lock(s->mutex);
            switch (origin)
            {
            case Origin::OnSpot:
            case Origin::TakenOver:
                ++s->onSpot;
                s->onSpotNs += rendererNs;
                s->takenOver += origin == Origin::TakenOver;
                ++s->dirty;
                s->lastNew = std::chrono::steady_clock::now();
                break;
            case Origin::Ready: ++s->takenReady; break;
            case Origin::Waited:
                ++s->takenWaited;
                s->waitNs += rendererNs;
                break;
            }
            const StoreKey made{p.exactHash, k};
            if (s->madeSet.insert(made).second)
            {
                s->made.push_back({p.exactHash, p.variantHash, k, false});
                if (!s->fileSet.count(made))
                    ++s->dirty; // (a key the key file lacks: built-in or new)
            }
            if (s->log)
            {
                static const char *const kHow[] = {"made on the spot (not listed)", "made on the spot (listed, not started yet)", "taken ready",
                                                   "taken after waiting"};
                std::fprintf(stderr, "[vkgl] pipeline +%.3f s: program %u (source %016llx), %s: %s, renderer %.1f ms%s\n", storeSeconds(), name,
                             static_cast<unsigned long long>(p.exactHash), keyText(k).c_str(), kHow[static_cast<int>(origin)], rendererNs / 1e6,
                             origin == Origin::Ready || origin == Origin::Waited ? (" (built in " + std::to_string(builtNs / 1000000) + " ms)").c_str() : "");
            }
        }

        // Program `serial` is going: its queued jobs are dropped, its running
        // ones waited for; the pipelines built for it, to destroy with it.
        std::vector<VkPipeline> storeForget(uint64_t serial)
        {
            std::vector<VkPipeline> built;
            Store *s = g_store;
            if (!s || !serial)
                return built;
            std::unique_lock<std::mutex> lock(s->mutex);
            bool dropped = false;
            for (auto it = s->jobs.begin(); it != s->jobs.end();)
            {
                const std::shared_ptr<Job> job = it->second;
                if (job->program != serial)
                {
                    ++it;
                    continue;
                }
                if (job->state == Job::State::Queued)
                {
                    s->queue.erase(job->queued);
                    dropped = true;
                }
                else
                {
                    s->done.wait(lock, [&] { return job->state == Job::State::Done; });
                    if (job->pipeline)
                        built.push_back(job->pipeline);
                }
                it = s->jobs.erase(it); // (only the renderer thread changes jobs)
            }
            if (dropped)
                s->done.notify_all(); // (the queue changed: a prepare waits for it to empty)
            return built;
        }

        std::string storeSummary()
        {
            const Store &s = *g_store;
            char b[640];
            // (The failures only when there are some: log scans look for "failed".)
            const std::string failures = s.aheadFailed ? "; " + std::to_string(s.aheadFailed) + " more failed" : std::string();
            std::snprintf(b, sizeof(b),
                          "renderer waited %.0f ms for pipelines in all: %llu made on the spot (%.0f ms; %llu of them listed but not started), %llu "
                          "taken ready, %llu taken after waiting (%.0f ms); store threads built %llu ahead (%.0f ms of compiling%s), %zu built (or "
                          "building) and not used yet, %zu queued; %zu keys listed (%zu from the file, %zu built in), %zu made in this run",
                          (s.onSpotNs + s.waitNs) / 1e6, static_cast<unsigned long long>(s.onSpot), s.onSpotNs / 1e6, static_cast<unsigned long long>(s.takenOver),
                          static_cast<unsigned long long>(s.takenReady), static_cast<unsigned long long>(s.takenWaited), s.waitNs / 1e6,
                          static_cast<unsigned long long>(s.aheadBuilt), s.aheadNs / 1e6, failures.c_str(), s.jobs.size() - s.queue.size(),
                          s.queue.size(), s.listed.size(), s.fileKeys, s.builtInKeys, s.made.size());
            return b;
        }

        // The program's pipeline for this state: made before, built ahead by
        // the store, or made now.
        VkPipeline graphicsPipeline(GLuint name, Program &p, const PipelineKey &k)
        {
            auto it = p.pipelines.find(k);
            if (it != p.pipelines.end())
                return it->second;
            ++g->stats.pipelines;
            const auto t0 = std::chrono::steady_clock::now();
            Origin origin = Origin::OnSpot;
            uint64_t builtNs = 0;
            VkPipeline pipeline = storeTake(p, k, origin, builtNs);
            if ((origin == Origin::Ready || origin == Origin::Waited) && !pipeline)
                origin = Origin::OnSpot; // (the store thread's attempt failed: again here, as before)
            if (origin == Origin::OnSpot || origin == Origin::TakenOver)
                pipeline = createGraphicsPipeline(p.modules[0], p.modules[1], p.layout, k);
            else
                ++g->stats.pipelinesAhead;
            const uint64_t ns = nsSince(t0);
            g->stats.pipelineNs += ns;
            if (g_stats && ns >= 50000000ull)
                std::fprintf(stderr, "[vkgl] slow pipeline: %.0f ms (blend %u, colour format %u, depth format %u; %s)\n", ns / 1e6, k.blend, k.colorFormat,
                             k.depthFormat, origin == Origin::Waited ? "waited for a store thread" : "made on the spot");
            storeNote(name, p, k, origin, ns, builtNs);
            p.pipelines[k] = pipeline;
            return pipeline;
        }

        // ------------------------------------------------------------------ draws and dispatches

        struct DescriptorWrites
        {
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> images;
            std::vector<VkDescriptorBufferInfo> buffers;
        };

        // The descriptors of p from the bound units, image units and storage
        // buffers; textures read and written go to reads / writes.
        // color: the draw's colour attachment (its input attachment view).
        void gatherDescriptors(Program &p, DescriptorWrites &dw, std::vector<Texture *> &reads, std::vector<Texture *> &writes,
                               Texture *color = nullptr)
        {
            dw.writes.clear();
            dw.images.clear();
            dw.buffers.clear();
            dw.images.reserve(p.resources.size());
            dw.buffers.reserve(p.resources.size() + 1);
            for (const Resource &r : p.resources)
            {
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstBinding = r.binding;
                w.descriptorCount = 1;
                w.descriptorType = r.type;
                if (r.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                {
                    const auto &b = g->ssbo[std::clamp(r.unit, 0, kSsboBindings - 1)];
                    Buffer *buf = g->buffers.get(b.buffer);
                    VkDescriptorBufferInfo info{g->dummyBuffer, 0, VK_WHOLE_SIZE};
                    if (buf && buf->handle())
                    {
                        if (!buf->dedicated && buf->slice.chunk)
                            buf->slice.chunk->lastSerial = g->serial;
                        info.buffer = buf->handle();
                        info.offset = buf->base() + static_cast<VkDeviceSize>(b.offset);
                        info.range = b.size > 0 ? static_cast<VkDeviceSize>(b.size) : buf->size - static_cast<VkDeviceSize>(b.offset);
                    }
                    dw.buffers.push_back(info);
                    w.pBufferInfo = &dw.buffers.back();
                }
                else if (r.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                {
                    Texture *t = g->textures.get(g->units[std::clamp(r.unit, 0, kUnits - 1)]);
                    if (!t || !t->image || t->integer != r.uintSampler)
                        t = g->textures.get(r.uintSampler ? g->dummyUint : g->dummyFloat);
                    reads.push_back(t);
                    dw.images.push_back({sampler(t->minFilter, t->magFilter, t->wrapS, t->wrapT), t->view, VK_IMAGE_LAYOUT_GENERAL});
                    w.pImageInfo = &dw.images.back();
                }
                else if (r.type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
                {
                    const Texture *t = color ? color : g->textures.get(g->dummyFloat);
                    dw.images.push_back({VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL});
                    w.pImageInfo = &dw.images.back();
                }
                else
                {
                    const auto &iu = g->imageUnits[std::clamp(r.unit, 0, kImageUnits - 1)];
                    Texture *t = g->textures.get(iu.texture);
                    VkImageView view = VK_NULL_HANDLE;
                    if (t && t->image && t->storage)
                    {
                        view = (t->internal == GL_RGBA8 && iu.format == GL_R32UI) ? r32View(*t) : t->view;
                        writes.push_back(t);
                    }
                    else
                        view = g->textures.get(r.uintSampler ? g->dummyUint : g->dummyFloat)->view;
                    dw.images.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
                    w.pImageInfo = &dw.images.back();
                }
                dw.writes.push_back(w);
            }
            if (p.hasUniformBlock)
            {
                const size_t size = p.uniformData.size() - p.uboBase;
                const Slice s = streamAlloc(size, dev->properties.limits.minUniformBufferOffsetAlignment);
                if (s.ptr)
                    std::memcpy(s.ptr, p.uniformData.data() + p.uboBase, size);
                dw.buffers.push_back({s.buffer ? s.buffer : g->dummyBuffer, s.offset, size});
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstBinding = kUniformBinding;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.pBufferInfo = &dw.buffers.back();
                dw.writes.push_back(w);
            }
        }

        std::vector<uint64_t> g_pushKey; // (bindProgram)

        void bindProgram(Program &p, VkPipelineBindPoint point, VkPipeline pipeline, const DescriptorWrites &dw)
        {
            VkCommandBuffer cb = cmd();
            if (g->boundPipeline != pipeline || g->boundPoint != point)
            {
                vkCmdBindPipeline(cb, point, pipeline);
                g->boundPipeline = pipeline;
                // Another layout's pipeline: the push constants written with
                // the last one are not this program's.
                if (g->pushedConstants != p.layout)
                    g->pushedConstants = VK_NULL_HANDLE;
            }
            // Set 0 first: pushing it with a layout that differs at set 0
            // disturbs set 1, so set 1 is bound after it when the layout changes.
            // Not pushed again when the same as this bind point's last push.
            if (!dw.writes.empty())
            {
                static const bool noSkip = std::getenv("GT3_VKGL_SKIP_REPEATS") && std::strcmp(std::getenv("GT3_VKGL_SKIP_REPEATS"), "0") == 0;
                Context::PushedSet &last = g->pushedSet[point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
                std::vector<uint64_t> &key = g_pushKey;
                key.clear();
                for (const VkWriteDescriptorSet &w : dw.writes)
                {
                    key.push_back((static_cast<uint64_t>(w.dstBinding) << 32) | static_cast<uint64_t>(w.descriptorType));
                    if (w.pImageInfo)
                    {
                        key.push_back(reinterpret_cast<uint64_t>(w.pImageInfo->sampler));
                        key.push_back(reinterpret_cast<uint64_t>(w.pImageInfo->imageView));
                    }
                    else if (w.pBufferInfo)
                    {
                        key.push_back(reinterpret_cast<uint64_t>(w.pBufferInfo->buffer));
                        key.push_back(w.pBufferInfo->offset);
                        key.push_back(w.pBufferInfo->range);
                    }
                }
                if (noSkip || last.layout != p.layout || last.key != key)
                {
                    vkCmdPushDescriptorSetKHR(cb, point, p.layout, 0, static_cast<uint32_t>(dw.writes.size()), dw.writes.data());
                    last.layout = p.layout;
                    last.key.swap(key);
                    ++g->stats.descriptorPushes;
                }
            }
            if (g->bindlessSet && (g->boundLayout != p.layout || g->boundPoint != point))
            {
                vkCmdBindDescriptorSets(cb, point, p.layout, 1, 1, &g->bindlessSet, 0, nullptr);
                // (Set 0 as pushed with another layout may be disturbed.)
                Context::PushedSet &last = g->pushedSet[point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
                if (last.layout != p.layout)
                    last.layout = VK_NULL_HANDLE;
            }
            g->boundLayout = p.layout;
            g->boundPoint = point;
            if (p.pushSize && (p.pushDirty || g->pushedConstants != p.layout))
            {
                vkCmdPushConstants(cb, p.layout, p.pushStages, 0, p.pushSize, p.uniformData.data());
                g->pushedConstants = p.layout;
                p.pushDirty = false;
            }
        }

        DescriptorWrites g_dw;
        std::vector<Texture *> g_reads, g_writes;

        void draw(GLint first, GLsizei count, GLsizei instances)
        {
            Program *p = g->programs.get(g->program);
            if (!p || !p->linked || p->compute || count <= 0)
                return;
            Framebuffer *fb = g->framebuffers.get(g->drawFbo);
            if (!fb)
            {
                logOnce("draw into the default framebuffer", 0);
                return;
            }
            Texture *color = fb->drawNone ? nullptr : g->textures.get(fb->color);
            Texture *depth = g->textures.get(fb->depth);
            if (color && !color->image)
                color = nullptr;
            if (depth && !depth->image)
                depth = nullptr;
            int width = fb->defaultWidth, height = fb->defaultHeight;
            if (color || depth)
            {
                width = std::min(color ? color->width : INT32_MAX, depth ? depth->width : INT32_MAX);
                height = std::min(color ? color->height : INT32_MAX, depth ? depth->height : INT32_MAX);
            }
            if (width <= 0 || height <= 0)
                return;

            g_reads.clear();
            g_writes.clear();
            gatherDescriptors(*p, g_dw, g_reads, g_writes, color);
            const GLuint colorName = color ? fb->color : 0, depthName = depth ? fb->depth : 0;
            const bool samePass = g->inPass && g->passColor == colorName && g->passDepth == depthName && g->passWidth == width &&
                                  g->passHeight == height;
            // The draw's accesses: textures sampled through units and (the
            // renderer's batch list) bindless handles, storage images, the
            // attachments; the colour target as an input attachment when a new
            // pass begins (its reads inside the pass go through textureBarrier).
            auto collect = [&](bool newPass) {
                g_accesses.clear();
                for (Texture *t : g_reads)
                    if (t != color && t != depth)
                        g_accesses.push_back({t, kFragment, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false});
                if (p->usesBindless)
                    for (GLuint name : g->bindlessReads)
                        if (Texture *t = g->textures.get(name); t && t->image && t != color && t != depth)
                            g_accesses.push_back({t, kFragment, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false});
                for (Texture *t : g_writes)
                    g_accesses.push_back({t, kFragment, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, true, false});
                if (color && newPass && p->inputAttachment)
                    g_accesses.push_back({color, kFragment, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT, false, true});
                if (color)
                    g_accesses.push_back({color, kColorOut, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true, true});
                if (depth)
                    g_accesses.push_back({depth, kDepthTests,
                                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true, true});
            };
            bool continuing = samePass;
            if (continuing)
            {
                collect(false);
                VkPipelineStageFlags2 src, dst;
                VkAccessFlags2 srcAccess, dstAccess;
                neededBarrier(g_accesses, true, src, srcAccess, dst, dstAccess);
                continuing = src == 0;
            }
            if (continuing)
                syncAccesses(g_accesses, true);
            else
            {
                endPass();
                ++g->passId;
                collect(true);
                syncAccesses(g_accesses, false);
                VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
                ri.layerCount = 1;
                if (color)
                {
                    ca.imageView = color->view;
                    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &ca;
                }
                if (depth)
                {
                    da.imageView = depth->view;
                    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    ri.pDepthAttachment = &da;
                }
                vkCmdBeginRendering(cmd(), &ri);
                ++g->stats.passes;
                g->inPass = true;
                g->passColor = colorName;
                g->passDepth = depthName;
                g->passWidth = width;
                g->passHeight = height;
            }

            PipelineKey key;
            key.colorFormat = color ? static_cast<uint32_t>(color->format) : 0u;
            key.depthFormat = depth ? static_cast<uint32_t>(depth->format) : 0u;
            key.colorMask = g->colorMask;
            key.blend = g->blend ? 1 : 0;
            if (g->blend)
            {
                key.eqRGB = static_cast<uint16_t>(g->eqRGB);
                key.eqA = static_cast<uint16_t>(g->eqA);
                key.srcRGB = static_cast<uint16_t>(g->srcRGB);
                key.dstRGB = static_cast<uint16_t>(g->dstRGB);
                key.srcA = static_cast<uint16_t>(g->srcA);
                key.dstA = static_cast<uint16_t>(g->dstA);
            }
            key.conservative = g->conservative ? 1 : 0;
            key.depthClamp = g->depthClamp ? 1 : 0;
            const VkPipeline pipeline = graphicsPipeline(g->program, *p, key);
            if (!pipeline)
                return;
            bindProgram(*p, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline, g_dw);
            VkCommandBuffer cb = cmd();
            // Dynamic state, set when it changes (every graphics pipeline has
            // the same dynamic states, so it holds across pipelines and passes).
            const VkViewport viewport{static_cast<float>(g->vpX), static_cast<float>(g->vpY), static_cast<float>(g->vpW), static_cast<float>(g->vpH),
                                      0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
            const bool depthTest = g->depthTest && depth;
            const VkBool32 testOn = depthTest ? VK_TRUE : VK_FALSE, writeOn = (depthTest && g->depthMask) ? VK_TRUE : VK_FALSE;
            const VkCompareOp compare = compareOp(g->depthFunc);
            const bool valid = g->dynamicValid;
            if (!valid || std::memcmp(&viewport, &g->lastViewport, sizeof(viewport)) != 0)
                vkCmdSetViewport(cb, 0, 1, &viewport);
            if (!valid || std::memcmp(&scissor, &g->lastScissor, sizeof(scissor)) != 0)
                vkCmdSetScissor(cb, 0, 1, &scissor);
            if (!valid || testOn != g->lastDepthTest)
                vkCmdSetDepthTestEnable(cb, testOn);
            if (!valid || writeOn != g->lastDepthWrite)
                vkCmdSetDepthWriteEnable(cb, writeOn);
            if (!valid || compare != g->lastCompare)
                vkCmdSetDepthCompareOp(cb, compare);
            g->dynamicValid = true;
            g->lastViewport = viewport;
            g->lastScissor = scissor;
            g->lastDepthTest = testOn;
            g->lastDepthWrite = writeOn;
            g->lastCompare = compare;
            ++g->stats.draws;
            vkCmdDraw(cb, static_cast<uint32_t>(count), static_cast<uint32_t>(std::max<GLsizei>(instances, 1)), static_cast<uint32_t>(first), 0);
            // The GPU out of work (a read-back drained the queue, or the
            // renderer fell behind): what is recorded goes now, not at the
            // renderer's next flush. GT3_VKGL_IDLE_SUBMIT=1 (off by default:
            // 59.40 -> 59.45 fps at 4x progressive, within noise).
            // GT4: on by default since 2026-10-14 (GT3_VKGL_IDLE_SUBMIT=0: off;
            // analysis/frame-times/RESULTS.md): the night course reads its lamp
            // probes back every frame, and the GPU then started the whole frame
            // only at the read; at the developer's settings dropped VSyncs 516-533 -> 455
            // there, New York 18 -> 8. The same commands in the same order on one
            // queue, only submitted earlier: the picture is the same.
#if defined(PS2X_GAME_GT3)
            static const bool idleSubmit = std::getenv("GT3_VKGL_IDLE_SUBMIT") && std::strcmp(std::getenv("GT3_VKGL_IDLE_SUBMIT"), "1") == 0;
#else
            static const bool idleSubmit = !(std::getenv("GT3_VKGL_IDLE_SUBMIT") && std::strcmp(std::getenv("GT3_VKGL_IDLE_SUBMIT"), "0") == 0);
#endif
            if (idleSubmit && ++g->cbDraws >= 4 && (g->cbDraws & 3) == 0)
            {
                const Frame &last = g->frames[(g->frameIndex + g->frameCount - 1) % g->frameCount];
                if (!last.pending || vkGetFenceStatus(vk, last.fence) == VK_SUCCESS)
                {
                    ++g->stats.idleSubmits;
                    submit();
                }
            }
        }

        // ------------------------------------------------------------------ read back

        uint8_t *readbackSpace(VkDeviceSize size)
        {
            if (size <= g->readbackSize)
                return g->readbackPtr;
            if (g->readback)
            {
                waitSerial(g->serial); // idle: the old buffer may be in use
                vkDestroyBuffer(vk, g->readback, nullptr);
                vkFreeMemory(vk, g->readbackMemory, nullptr);
                g->readback = VK_NULL_HANDLE;
            }
            const VkDeviceSize want = std::max<VkDeviceSize>(alignUp(size, 1u << 20), 4u << 20);
            uint32_t type = 0;
            if (!makeBuffer(want, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                            g->readback, g->readbackMemory, &g->readbackPtr, &type))
                return nullptr;
            g->readbackCoherent = (dev->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            g->readbackSize = want;
            return g->readbackPtr;
        }

        // Copies (x, y, w, h) of t into out (rows of w pixels, rowStride bytes).
        void readTexture(Texture &t, int x, int y, int w, int h, void *out, size_t rowStride)
        {
            FormatInfo fi{};
            formatInfo(t.internal, fi);
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * fi.bytes;
            if (!readbackSpace(bytes) || w <= 0 || h <= 0)
                return;
            transferHazards({&t}, {});
            VkBufferImageCopy region{};
            region.imageSubresource = {aspect(t), 0, 0, 1};
            region.imageOffset = {x, y, 0};
            region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
            vkCmdCopyImageToBuffer(cmd(), t.image, VK_IMAGE_LAYOUT_GENERAL, g->readback, 1, &region);
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd(), &di);
            const uint64_t serial = g->serial;
            const auto waitStart = std::chrono::steady_clock::now();
            waitSerial(serial);
            ++g->stats.readbacks;
            g->stats.waitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
            if (!g->readbackCoherent)
            {
                VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
                range.memory = g->readbackMemory;
                range.size = VK_WHOLE_SIZE;
                vkInvalidateMappedMemoryRanges(vk, 1, &range);
            }
            const size_t row = static_cast<size_t>(w) * fi.bytes;
            for (int j = 0; j < h; ++j)
                std::memcpy(static_cast<uint8_t *>(out) + static_cast<size_t>(j) * rowStride, g->readbackPtr + static_cast<size_t>(j) * row, row);
        }

        // ------------------------------------------------------------------ bindless

        uint64_t handleFor(GLuint name, int kind)
        {
            Texture *t = g->textures.get(name);
            if (!t || !t->image || !g->bindlessSet)
                return 0;
            if (t->bindless == UINT32_MAX)
            {
                poll();
                if (!g->bindlessFree.empty())
                {
                    t->bindless = g->bindlessFree.back();
                    g->bindlessFree.pop_back();
                }
                else if (g->bindlessNext < kBindlessSlots)
                    t->bindless = g->bindlessNext++;
                else
                {
                    logOnce("bindless slots exhausted", kBindlessSlots);
                    return 0;
                }
                VkDescriptorImageInfo info{VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = g->bindlessSet;
                w.dstBinding = 0;
                w.dstArrayElement = t->bindless;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                w.pImageInfo = &info;
                vkUpdateDescriptorSets(vk, 1, &w, 0, nullptr);
            }
            return static_cast<uint64_t>(t->bindless) | (static_cast<uint64_t>(kind) << 32);
        }

        int samplerKind(GLint minF, GLint magF, GLint wrapS)
        {
            const bool linear = filterOf(magF) == VK_FILTER_LINEAR || filterOf(minF) == VK_FILTER_LINEAR;
            const bool clamp = wrapS == GL_CLAMP_TO_EDGE;
            return (linear ? 1 : 0) | (clamp ? 2 : 0);
        }

        uint64_t GLAD_API_PTR getTextureHandle(GLuint texture)
        {
            Texture *t = g->textures.get(texture);
            return t ? handleFor(texture, samplerKind(t->minFilter, t->magFilter, t->wrapS)) : 0;
        }
        uint64_t GLAD_API_PTR getTextureSamplerHandle(GLuint texture, GLuint samplerName)
        {
            SamplerObject *s = g->samplerObjects.get(samplerName);
            if (!s)
                return getTextureHandle(texture);
            return handleFor(texture, samplerKind(s->minFilter, s->magFilter, s->wrapS));
        }
        // glTextureBarrier with local read: the pass's colour writes so far,
        // for the next draws' input attachment reads of the same pixels.
        void GLAD_API_PTR textureBarrier()
        {
            if (!g->inPass || !g->passColor)
                return; // a new pass begins after a global barrier anyway
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd(), &di);
            ++g->stats.localBarriers;
        }
        void GLAD_API_PTR makeHandleResident(uint64_t) {}
        void GLAD_API_PTR makeHandleNonResident(uint64_t) {}
        void GLAD_API_PTR clipControl(GLenum, GLenum) {} // Vulkan's own conventions: rows from 0, depth 0..1
        void GLAD_API_PTR bufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);

        // ------------------------------------------------------------------ setup

        bool init()
        {
            dev = gsvk::sharedDevice();
            if (!dev)
                return false;
            if (!dev->pushDescriptor)
            {
                std::fprintf(stderr, "[vkgl] the device lacks VK_KHR_push_descriptor\n");
                return false;
            }
            vk = dev->device;
            static bool glslangReady = false;
            if (!glslangReady)
                glslangReady = glslang::InitializeProcess();
            g = new Context();
            g_stats = std::getenv("GT3_VKGL_STATS") && *std::getenv("GT3_VKGL_STATS") == '1';
            // GT3_VKGL_FRAMES=<n>: command buffers in flight, how far the
            // renderer can get ahead of the GPU: 32, with a submission per 24
            // batches (GS_FLUSH_EVERY_DEFAULT; about 20 per race frame). 4x
            // progressive at Trial Mountain (laptop, a submission per 8
            // batches): 54.9 fps at 8, 58.2 at 32, 56.6 at 64.
            if (const char *v = std::getenv("GT3_VKGL_FRAMES"); v && *v)
                g->frameCount = std::clamp<uint32_t>(static_cast<uint32_t>(std::strtoul(v, nullptr, 10)), 2u, kMaxFrames);
            VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            cpi.queueFamilyIndex = dev->queueFamily;
            vkCreateCommandPool(vk, &cpi, nullptr, &g->pool);
            for (Frame &f : g->frames)
            {
                VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                cbai.commandPool = g->pool;
                cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbai.commandBufferCount = 1;
                vkAllocateCommandBuffers(vk, &cbai, &f.cb);
                VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                vkCreateFence(vk, &fci, nullptr, &f.fence);
            }
            // Bindless: sampled images at set 1 binding 0, four samplers at binding 1.
            if (dev->bindless)
            {
                g->bindlessSamplers[0] = sampler(GL_NEAREST, GL_NEAREST, GL_REPEAT, GL_REPEAT);
                g->bindlessSamplers[1] = sampler(GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT);
                g->bindlessSamplers[2] = sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
                g->bindlessSamplers[3] = sampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
                const VkDescriptorSetLayoutBinding bindings[2] = {
                    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kBindlessSlots, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                    {1, VK_DESCRIPTOR_TYPE_SAMPLER, 4, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, g->bindlessSamplers}};
                const VkDescriptorBindingFlags flags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                               VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                                           0};
                VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
                bf.bindingCount = 2;
                bf.pBindingFlags = flags;
                VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
                dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
                dslci.bindingCount = 2;
                dslci.pBindings = bindings;
                vkCreateDescriptorSetLayout(vk, &dslci, nullptr, &g->bindlessLayout);
                const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kBindlessSlots}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4}};
                VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                dpci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
                dpci.maxSets = 1;
                dpci.poolSizeCount = 2;
                dpci.pPoolSizes = sizes;
                vkCreateDescriptorPool(vk, &dpci, nullptr, &g->bindlessPool);
                VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                dsai.descriptorPool = g->bindlessPool;
                dsai.descriptorSetCount = 1;
                dsai.pSetLayouts = &g->bindlessLayout;
                if (vkAllocateDescriptorSets(vk, &dsai, &g->bindlessSet) != VK_SUCCESS)
                    g->bindlessSet = VK_NULL_HANDLE;
            }
            // Stand-ins for unbound units and buffers.
            uint8_t *unused = nullptr;
            makeBuffer(256, kStreamUsage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, g->dummyBuffer, g->dummyBufferMemory, &unused);
            g->dummyFloat = g->textures.make();
            createImage(*g->textures.get(g->dummyFloat), GL_RGBA8, 1, 1);
            g->dummyUint = g->textures.make();
            createImage(*g->textures.get(g->dummyUint), GL_R32UI, 1, 1);

            if (dev->bindless && g->bindlessSet)
                g->extensions.push_back("GL_ARB_bindless_texture");
            if (dev->conservative)
                g->extensions.push_back("GL_NV_conservative_raster");
            if (dev->interlock)
                g->extensions.push_back("GL_ARB_fragment_shader_interlock");
            g->rendererName = dev->name;
            g->versionName = "4.6 vkgl (Vulkan " + std::to_string(VK_API_VERSION_MAJOR(dev->properties.apiVersion)) + "." +
                             std::to_string(VK_API_VERSION_MINOR(dev->properties.apiVersion)) + ")";
            std::fprintf(stderr, "[vkgl] OpenGL subset on Vulkan: %s\n", dev->name.c_str());
            // The pipeline cache, its key list and the threads building ahead
            // (before the renderer links its programs).
            storeInit();
            return true;
        }
    }

    bool available()
    {
        static const bool ok = init();
        return ok;
    }

    bool nativeImage(GLuint texture, NativeImage &out)
    {
        Texture *t = g ? g->textures.get(texture) : nullptr;
        if (!t || !t->image)
            return false;
        out.image = reinterpret_cast<uint64_t>(t->image);
        out.view = reinterpret_cast<uint64_t>(t->view);
        out.format = static_cast<uint32_t>(t->format);
        out.usage = static_cast<uint32_t>(t->usage);
        out.width = t->width;
        out.height = t->height;
        return true;
    }

    void recordExternal(const GLuint *reads, int readCount, const GLuint *writes, int writeCount, void (*fn)(void *cmd, void *user), void *user)
    {
        endPass();
        // (what the outside work does with them is unknown: all stages, any memory access)
        std::vector<Access> a;
        for (int i = 0; i < readCount; ++i)
            if (Texture *t = g->textures.get(reads[i]))
                a.push_back({t, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, false, false});
        for (int i = 0; i < writeCount; ++i)
            if (Texture *t = g->textures.get(writes[i]))
                a.push_back({t, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, true, false});
        syncAccesses(a, false);
        const VkCommandBuffer cb = cmd();
        fn(cb, user);
        g->boundPipeline = VK_NULL_HANDLE;
        g->boundLayout = VK_NULL_HANDLE;
        g->boundPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
        g->pushedSet[0].layout = g->pushedSet[1].layout = VK_NULL_HANDLE;
        g->pushedConstants = VK_NULL_HANDLE;
        g->dynamicValid = false;
    }

    void *splitExternal(uint64_t semaphore, uint64_t signalValue, uint64_t waitValue)
    {
        g->bridgeSignal = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(semaphore));
        g->bridgeSignalValue = signalValue;
        submit();
        g->bridgeWait = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(semaphore));
        g->bridgeWaitValue = waitValue;
        return cmd();
    }

    bool readStart(GLuint texture, int x, int y, int w, int h, uint32_t tag)
    {
        Texture *t = g ? g->textures.get(texture) : nullptr;
        if (!t || !t->image || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > t->width || y + h > t->height)
            return false;
        FormatInfo fi{};
        formatInfo(t->internal, fi);
        if (fi.bytes != 4u)
            return false;
        Context::AsyncRead *slot = nullptr;
        for (Context::AsyncRead &r : g->asyncReads)
            if (!r.busy)
            {
                slot = &r;
                break;
            }
        if (!slot)
            return false;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4u;
        if (bytes > slot->size)
        {
            // (not busy: nothing the GPU still does uses it)
            if (slot->buffer)
            {
                vkDestroyBuffer(vk, slot->buffer, nullptr);
                vkFreeMemory(vk, slot->memory, nullptr);
                *slot = Context::AsyncRead{};
            }
            uint32_t type = 0;
            const VkDeviceSize want = alignUp(bytes, 1u << 16);
            if (!makeBuffer(want, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, slot->buffer,
                            slot->memory, &slot->ptr, &type))
            {
                *slot = Context::AsyncRead{};
                return false;
            }
            slot->coherent = (dev->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            slot->size = want;
        }
        transferHazards({t}, {});
        VkBufferImageCopy region{};
        region.imageSubresource = {aspect(*t), 0, 0, 1};
        region.imageOffset = {x, y, 0};
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyImageToBuffer(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, slot->buffer, 1, &region);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd(), &di);
        slot->busy = true;
        slot->serial = g->serial;
        slot->w = w, slot->h = h, slot->tag = tag;
        return true;
    }

    bool readTake(std::vector<uint8_t> &out, int &w, int &h, uint32_t &tag)
    {
        if (!g)
            return false;
        poll();
        Context::AsyncRead *done = nullptr;
        for (Context::AsyncRead &r : g->asyncReads)
            if (r.busy && r.serial <= g->completed && (!done || r.serial < done->serial))
                done = &r;
        if (!done)
            return false;
        if (!done->coherent)
        {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = done->memory;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(vk, 1, &range);
        }
        const size_t bytes = static_cast<size_t>(done->w) * static_cast<size_t>(done->h) * 4u;
        out.assign(done->ptr, done->ptr + bytes);
        w = done->w, h = done->h, tag = done->tag;
        done->busy = false;
        return true;
    }

    // ---------------------------------------------------------------------- textures

    void GLAD_API_PTR GenTextures(GLsizei n, GLuint *textures)
    {
        for (GLsizei i = 0; i < n; ++i)
            textures[i] = g->textures.make();
    }

    void GLAD_API_PTR DeleteTextures(GLsizei n, const GLuint *textures)
    {
        for (GLsizei i = 0; i < n; ++i)
        {
            const GLuint name = textures[i];
            Texture *t = g->textures.get(name);
            if (!t || name == g->dummyFloat || name == g->dummyUint)
                continue;
            if (g->inPass && (g->passColor == name || g->passDepth == name))
                endPass();
            for (GLuint &u : g->units)
                if (u == name)
                    u = 0;
            for (auto &iu : g->imageUnits)
                if (iu.texture == name)
                    iu.texture = 0;
            for (Framebuffer &fb : g->framebuffers.objects)
            {
                if (fb.color == name)
                    fb.color = 0;
                if (fb.depth == name)
                    fb.depth = 0;
            }
            destroyTextureObjects(*t);
            g->textures.release(name);
        }
    }

    void GLAD_API_PTR ActiveTexture(GLenum texture) { g->activeUnit = std::min<GLenum>(texture - GL_TEXTURE0, kUnits - 1); }

    void GLAD_API_PTR BindTexture(GLenum target, GLuint texture)
    {
        if (target != GL_TEXTURE_2D)
            logOnce("texture target", target);
        g->units[g->activeUnit] = texture;
    }

    void GLAD_API_PTR TexStorage2D(GLenum, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
    {
        Texture *t = boundTexture();
        if (!t)
            return;
        if (levels != 1)
            logOnce("mipmap levels", static_cast<unsigned>(levels));
        createImage(*t, internalformat, width, height);
    }

    void GLAD_API_PTR TexParameteri(GLenum, GLenum pname, GLint param)
    {
        Texture *t = boundTexture();
        if (!t)
            return;
        switch (pname)
        {
        case GL_TEXTURE_MIN_FILTER: t->minFilter = param; break;
        case GL_TEXTURE_MAG_FILTER: t->magFilter = param; break;
        case GL_TEXTURE_WRAP_S: t->wrapS = param; break;
        case GL_TEXTURE_WRAP_T: t->wrapT = param; break;
        default: logOnce("texture parameter", pname); break;
        }
    }

    void GLAD_API_PTR PixelStorei(GLenum pname, GLint param)
    {
        if (pname == GL_UNPACK_ALIGNMENT)
            g->unpackAlignment = std::max(param, 1);
        else if (pname == GL_PACK_ALIGNMENT)
            g->packAlignment = std::max(param, 1);
        else
            logOnce("pixel store", pname);
    }

    void GLAD_API_PTR TexSubImage2D(GLenum, GLint, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                    const void *pixels)
    {
        Texture *t = boundTexture();
        if (!t || !t->image || width <= 0 || height <= 0 || !pixels)
            return;
        FormatInfo fi{};
        formatInfo(t->internal, fi);
        const uint32_t bytes = clientBytes(format, type);
        if (bytes != fi.bytes)
        {
            logOnce("upload format", format);
            return;
        }
        const size_t row = static_cast<size_t>(width) * bytes;
        const size_t stride = alignUp(row, static_cast<VkDeviceSize>(g->unpackAlignment));
        const VkDeviceSize size = static_cast<VkDeviceSize>(row) * static_cast<VkDeviceSize>(height);
        const Slice s = streamAlloc(size, std::max<VkDeviceSize>(fi.bytes, 4));
        if (!s.ptr)
            return;
        for (GLsizei j = 0; j < height; ++j)
            std::memcpy(s.ptr + static_cast<size_t>(j) * row, static_cast<const uint8_t *>(pixels) + static_cast<size_t>(j) * stride, row);
        endPass();
        transferHazardsRect(nullptr, 0, 0, 0, 0, t, xoffset, yoffset, width, height);
        VkBufferImageCopy region{};
        region.bufferOffset = s.offset;
        region.imageSubresource = {aspect(*t), 0, 0, 1};
        region.imageOffset = {xoffset, yoffset, 0};
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(cmd(), s.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        ++g->stats.uploads;
        g->stats.uploadBytes += size;
    }

    void GLAD_API_PTR GetTexImage(GLenum, GLint, GLenum format, GLenum type, void *pixels)
    {
        Texture *t = boundTexture();
        if (!t || !t->image)
            return;
        FormatInfo fi{};
        formatInfo(t->internal, fi);
        if (clientBytes(format, type) != fi.bytes && !(t->depth && format == GL_DEPTH_COMPONENT && type == GL_FLOAT))
        {
            logOnce("read back format", format);
            return;
        }
        const size_t row = static_cast<size_t>(t->width) * fi.bytes;
        readTexture(*t, 0, 0, t->width, t->height, pixels, alignUp(row, static_cast<VkDeviceSize>(g->packAlignment)));
    }

    void GLAD_API_PTR CopyImageSubData(GLuint srcName, GLenum, GLint, GLint srcX, GLint srcY, GLint, GLuint dstName, GLenum, GLint, GLint dstX,
                                       GLint dstY, GLint, GLsizei srcWidth, GLsizei srcHeight, GLsizei)
    {
        Texture *src = g->textures.get(srcName), *dst = g->textures.get(dstName);
        if (!src || !dst || !src->image || !dst->image || srcWidth <= 0 || srcHeight <= 0)
            return;
        endPass();
        transferHazardsRect(src, srcX, srcY, srcWidth, srcHeight, dst, dstX, dstY, srcWidth, srcHeight);
        VkImageCopy region{};
        region.srcSubresource = {aspect(*src), 0, 0, 1};
        region.srcOffset = {srcX, srcY, 0};
        region.dstSubresource = {aspect(*dst), 0, 0, 1};
        region.dstOffset = {dstX, dstY, 0};
        region.extent = {static_cast<uint32_t>(srcWidth), static_cast<uint32_t>(srcHeight), 1};
        vkCmdCopyImage(cmd(), src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        ++g->stats.copies;
        const int64_t copied = static_cast<int64_t>(srcWidth) * srcHeight;
        if (copied <= 64 * 128)
            ++g->stats.smallCopies;
        else if (copied <= 256 * 256)
            ++g->stats.pageCopies;
    }

    namespace
    {
        // glClearTexImage (GL 4.4 / ARB_clear_texture), by name through
        // getProcAddress: GT4's stable targets clear a pooled target texture
        // when it is reused, as a new one (PS2X_GS_GL_STABLE_TARGETS,
        // gs_gl_backend.cpp acquireTargetTexture). Level 0 only (vkgl textures
        // have one); data null clears to zero, as in OpenGL.
        void GLAD_API_PTR clearTexImage(GLuint texture, GLint level, GLenum format, GLenum type, const void *data)
        {
            Texture *t = g->textures.get(texture);
            if (!t || !t->image || level != 0)
                return;
            endPass();
            transferHazards({}, {t});
            const VkImageSubresourceRange range{aspect(*t), 0, 1, 0, 1};
            if (t->depth)
            {
                VkClearDepthStencilValue v{0.0f, 0};
                if (data && format == GL_DEPTH_COMPONENT && type == GL_FLOAT)
                    std::memcpy(&v.depth, data, sizeof(float));
                else if (data)
                    logOnce("clear texture format", format);
                vkCmdClearDepthStencilImage(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            else
            {
                VkClearColorValue v{};
                if (data && !t->integer && format == GL_RGBA && type == GL_UNSIGNED_BYTE)
                    for (int i = 0; i < 4; ++i)
                        v.float32[i] = static_cast<float>(static_cast<const uint8_t *>(data)[i]) / 255.0f;
                else if (data && t->integer && (format == GL_RED_INTEGER || format == GL_RGBA_INTEGER) && type == GL_UNSIGNED_INT)
                    std::memcpy(v.uint32, data, format == GL_RED_INTEGER ? 4 : 16);
                else if (data)
                    logOnce("clear texture format", format);
                vkCmdClearColorImage(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            ++g->stats.clears;
        }
    }

    // ---------------------------------------------------------------------- samplers

    void GLAD_API_PTR GenSamplers(GLsizei n, GLuint *samplers)
    {
        for (GLsizei i = 0; i < n; ++i)
            samplers[i] = g->samplerObjects.make();
    }

    void GLAD_API_PTR SamplerParameteri(GLuint samplerName, GLenum pname, GLint param)
    {
        SamplerObject *s = g->samplerObjects.get(samplerName);
        if (!s)
            return;
        switch (pname)
        {
        case GL_TEXTURE_MIN_FILTER: s->minFilter = param; break;
        case GL_TEXTURE_MAG_FILTER: s->magFilter = param; break;
        case GL_TEXTURE_WRAP_S: s->wrapS = param; break;
        case GL_TEXTURE_WRAP_T: s->wrapT = param; break;
        default: break;
        }
    }

    // ---------------------------------------------------------------------- buffers

    void GLAD_API_PTR GenBuffers(GLsizei n, GLuint *buffers)
    {
        for (GLsizei i = 0; i < n; ++i)
            buffers[i] = g->buffers.make();
    }

    void GLAD_API_PTR BindBuffer(GLenum target, GLuint buffer)
    {
        if (target != GL_SHADER_STORAGE_BUFFER)
            logOnce("buffer target", target);
        g->genericBuffer = buffer;
    }

    void GLAD_API_PTR BindBufferBase(GLenum target, GLuint index, GLuint buffer)
    {
        if (target != GL_SHADER_STORAGE_BUFFER || index >= static_cast<GLuint>(kSsboBindings))
        {
            logOnce("buffer binding", index);
            return;
        }
        g->ssbo[index] = {buffer, 0, 0};
        g->genericBuffer = buffer;
    }

    void GLAD_API_PTR BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
    {
        if (target != GL_SHADER_STORAGE_BUFFER || index >= static_cast<GLuint>(kSsboBindings))
        {
            logOnce("buffer binding", index);
            return;
        }
        g->ssbo[index] = {buffer, offset, size};
        g->genericBuffer = buffer;
    }

    namespace
    {
        // Re-specified storage: a new stretch of streaming memory (as a driver
        // "orphans" a buffer), so draws already recorded keep the old data.
        void respecify(Buffer &b, VkDeviceSize size, const void *data, VkDeviceSize keepBytes)
        {
            const Slice old = b.slice;
            const Slice s = streamAlloc(std::max<VkDeviceSize>(size, 16), dev->properties.limits.minStorageBufferOffsetAlignment);
            if (!s.ptr)
                return;
            if (keepBytes && old.ptr)
                std::memcpy(s.ptr, old.ptr, static_cast<size_t>(std::min(keepBytes, size)));
            if (data)
                std::memcpy(s.ptr, data, static_cast<size_t>(size));
            if (old.chunk)
                --old.chunk->refs;
            ++s.chunk->refs;
            b.slice = s;
            b.size = size;
        }
    }

    void GLAD_API_PTR BufferData(GLenum, GLsizeiptr size, const void *data, GLenum)
    {
        Buffer *b = g->buffers.get(g->genericBuffer);
        if (!b || b->dedicated)
            return;
        respecify(*b, static_cast<VkDeviceSize>(size), data, 0);
    }

    void GLAD_API_PTR BufferSubData(GLenum, GLintptr offset, GLsizeiptr size, const void *data)
    {
        Buffer *b = g->buffers.get(g->genericBuffer);
        if (!b || !data || size <= 0)
            return;
        if (b->dedicated)
        {
            std::memcpy(b->mapped + offset, data, static_cast<size_t>(size));
            return;
        }
        // Draws recorded before read the old contents: copy into new storage.
        const Slice old = b->slice;
        respecify(*b, b->size, nullptr, b->size);
        if (b->slice.ptr && b->slice.ptr != old.ptr)
            std::memcpy(b->slice.ptr + offset, data, static_cast<size_t>(size));
    }

    namespace
    {
        void GLAD_API_PTR bufferStorage(GLenum, GLsizeiptr size, const void *data, GLbitfield)
        {
            Buffer *b = g->buffers.get(g->genericBuffer);
            if (!b)
                return;
            if (!makeBuffer(static_cast<VkDeviceSize>(size), kStreamUsage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b->dedicated, b->dedicatedMemory, &b->mapped))
            {
                std::fprintf(stderr, "[vkgl] glBufferStorage of %lld bytes failed\n", static_cast<long long>(size));
                return;
            }
            b->size = static_cast<VkDeviceSize>(size);
            if (data)
                std::memcpy(b->mapped, data, static_cast<size_t>(size));
        }
    }

    void *GLAD_API_PTR MapBufferRange(GLenum, GLintptr offset, GLsizeiptr, GLbitfield)
    {
        Buffer *b = g->buffers.get(g->genericBuffer);
        if (!b || !b->data())
            return nullptr;
        return b->data() + offset;
    }

    // ---------------------------------------------------------------------- framebuffers

    void GLAD_API_PTR GenFramebuffers(GLsizei n, GLuint *framebuffers)
    {
        for (GLsizei i = 0; i < n; ++i)
            framebuffers[i] = g->framebuffers.make();
    }

    // GT4's stable targets (PS2X_GS_GL_STABLE_TARGETS, gs_gl_backend.cpp
    // dropFbosOf) delete a texture's framebuffers with it. A framebuffer holds
    // only names (no Vulkan object): its name goes back to the free list, and a
    // binding of it is dropped, as in OpenGL.
    void GLAD_API_PTR DeleteFramebuffers(GLsizei n, const GLuint *framebuffers)
    {
        for (GLsizei i = 0; i < n; ++i)
        {
            const GLuint name = framebuffers[i];
            if (!g->framebuffers.get(name))
                continue;
            if (g->drawFbo == name)
                g->drawFbo = 0;
            if (g->readFbo == name)
                g->readFbo = 0;
            g->framebuffers.release(name);
        }
    }

    void GLAD_API_PTR BindFramebuffer(GLenum target, GLuint framebuffer)
    {
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
            g->drawFbo = framebuffer;
        if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
            g->readFbo = framebuffer;
    }

    void GLAD_API_PTR FramebufferTexture2D(GLenum target, GLenum attachment, GLenum, GLuint texture, GLint)
    {
        Framebuffer *fb = g->framebuffers.get(target == GL_READ_FRAMEBUFFER ? g->readFbo : g->drawFbo);
        if (!fb)
            return;
        if (attachment == GL_COLOR_ATTACHMENT0)
            fb->color = texture;
        else if (attachment == GL_DEPTH_ATTACHMENT)
            fb->depth = texture;
        else
            logOnce("attachment", attachment);
    }

    void GLAD_API_PTR FramebufferParameteri(GLenum target, GLenum pname, GLint param)
    {
        Framebuffer *fb = g->framebuffers.get(target == GL_READ_FRAMEBUFFER ? g->readFbo : g->drawFbo);
        if (!fb)
            return;
        if (pname == GL_FRAMEBUFFER_DEFAULT_WIDTH)
            fb->defaultWidth = param;
        else if (pname == GL_FRAMEBUFFER_DEFAULT_HEIGHT)
            fb->defaultHeight = param;
    }

    void GLAD_API_PTR DrawBuffer(GLenum buf)
    {
        if (Framebuffer *fb = g->framebuffers.get(g->drawFbo))
            fb->drawNone = buf == GL_NONE;
    }

    void GLAD_API_PTR ReadBuffer(GLenum) {}

    void GLAD_API_PTR BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                      GLbitfield mask, GLenum filter)
    {
        Framebuffer *rf = g->framebuffers.get(g->readFbo), *df = g->framebuffers.get(g->drawFbo);
        if (!rf || !df)
            return;
        const bool depth = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        Texture *src = g->textures.get(depth ? rf->depth : rf->color), *dst = g->textures.get(depth ? df->depth : df->color);
        if (!src || !dst || !src->image || !dst->image)
            return;
        endPass();
        transferHazardsRect(src, std::min(srcX0, srcX1), std::min(srcY0, srcY1), std::abs(srcX1 - srcX0), std::abs(srcY1 - srcY0), dst,
                            std::min(dstX0, dstX1), std::min(dstY0, dstY1), std::abs(dstX1 - dstX0), std::abs(dstY1 - dstY0));
        VkImageBlit region{};
        region.srcSubresource = {aspect(*src), 0, 0, 1};
        region.srcOffsets[0] = {srcX0, srcY0, 0};
        region.srcOffsets[1] = {srcX1, srcY1, 1};
        region.dstSubresource = {aspect(*dst), 0, 0, 1};
        region.dstOffsets[0] = {dstX0, dstY0, 0};
        region.dstOffsets[1] = {dstX1, dstY1, 1};
        vkCmdBlitImage(cmd(), src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                       filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        ++g->stats.blits;
    }

    void GLAD_API_PTR ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
    {
        Framebuffer *fb = g->framebuffers.get(g->readFbo);
        if (!fb)
            return;
        const bool depth = format == GL_DEPTH_COMPONENT;
        Texture *t = g->textures.get(depth ? fb->depth : fb->color);
        if (!t || !t->image)
            return;
        FormatInfo fi{};
        formatInfo(t->internal, fi);
        if (clientBytes(format, type) != fi.bytes && !(depth && type == GL_FLOAT))
        {
            logOnce("read pixels format", format);
            return;
        }
        const size_t row = static_cast<size_t>(width) * fi.bytes;
        readTexture(*t, x, y, width, height, pixels, alignUp(row, static_cast<VkDeviceSize>(g->packAlignment)));
    }

    // ---------------------------------------------------------------------- state

    void GLAD_API_PTR Enable(GLenum cap)
    {
        switch (cap)
        {
        case GL_DEPTH_TEST: g->depthTest = true; break;
        case GL_BLEND: g->blend = true; break;
        case GL_CONSERVATIVE_RASTERIZATION_NV: g->conservative = true; break;
        case GL_DEPTH_CLAMP: g->depthClamp = true; break;
        default: break; // debug output, clip distances (always on in Vulkan when written)
        }
    }

    void GLAD_API_PTR Disable(GLenum cap)
    {
        switch (cap)
        {
        case GL_DEPTH_TEST: g->depthTest = false; break;
        case GL_BLEND: g->blend = false; break;
        case GL_CONSERVATIVE_RASTERIZATION_NV: g->conservative = false; break;
        case GL_DEPTH_CLAMP: g->depthClamp = false; break;
        default: break;
        }
    }

    void GLAD_API_PTR DepthFunc(GLenum func) { g->depthFunc = func; }
    void GLAD_API_PTR DepthMask(GLboolean flag) { g->depthMask = flag != GL_FALSE; }
    void GLAD_API_PTR ColorMask(GLboolean r, GLboolean gr, GLboolean b, GLboolean a)
    {
        g->colorMask = static_cast<uint8_t>((r ? 1 : 0) | (gr ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
    }
    void GLAD_API_PTR BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha)
    {
        g->eqRGB = modeRGB;
        g->eqA = modeAlpha;
    }
    void GLAD_API_PTR BlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
    {
        g->srcRGB = srcRGB;
        g->dstRGB = dstRGB;
        g->srcA = srcAlpha;
        g->dstA = dstAlpha;
    }
    void GLAD_API_PTR Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
    {
        g->vpX = x;
        g->vpY = y;
        g->vpW = width;
        g->vpH = height;
    }
    void GLAD_API_PTR BindVertexArray(GLuint) {}
    void GLAD_API_PTR GenVertexArrays(GLsizei n, GLuint *arrays)
    {
        for (GLsizei i = 0; i < n; ++i)
            arrays[i] = ++g->vaoCounter;
    }

    void GLAD_API_PTR BindImageTexture(GLuint unit, GLuint texture, GLint, GLboolean, GLint, GLenum, GLenum format)
    {
        if (unit >= static_cast<GLuint>(kImageUnits))
            return;
        g->imageUnits[unit] = {texture, format};
    }

    // ---------------------------------------------------------------------- shaders and programs

    GLuint GLAD_API_PTR CreateShader(GLenum type)
    {
        const GLuint n = g->shaders.make();
        g->shaders.get(n)->type = type;
        return n;
    }

    void GLAD_API_PTR ShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
    {
        Shader *s = g->shaders.get(shader);
        if (!s)
            return;
        s->source.clear();
        for (GLsizei i = 0; i < count; ++i)
            s->source.append(string[i], length && length[i] >= 0 ? static_cast<size_t>(length[i]) : std::strlen(string[i]));
    }

    void GLAD_API_PTR CompileShader(GLuint shader)
    {
        Shader *s = g->shaders.get(shader);
        if (!s)
            return;
        // Parsed at link time, with the program's other stages (their loose
        // uniforms become one push constant block); errors show in the link log.
        s->prepared = prepareSource(s->source, s->inputAttachment, s->bindless);
        s->compiled = true;
        s->log.clear();
    }

    void GLAD_API_PTR GetShaderiv(GLuint shader, GLenum pname, GLint *params)
    {
        Shader *s = g->shaders.get(shader);
        if (!s)
            return;
        if (pname == GL_COMPILE_STATUS)
            *params = s->compiled ? GL_TRUE : GL_FALSE;
        else if (pname == GL_INFO_LOG_LENGTH)
            *params = static_cast<GLint>(s->log.size() + 1);
        else
            *params = 0;
    }

    void GLAD_API_PTR GetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
    {
        Shader *s = g->shaders.get(shader);
        const std::string &log = s ? s->log : std::string();
        const size_t n = bufSize > 0 ? std::min(log.size(), static_cast<size_t>(bufSize - 1)) : 0;
        if (bufSize > 0)
        {
            std::memcpy(infoLog, log.data(), n);
            infoLog[n] = 0;
        }
        if (length)
            *length = static_cast<GLsizei>(n);
    }

    void GLAD_API_PTR DeleteShader(GLuint shader) { g->shaders.release(shader); }

    GLuint GLAD_API_PTR CreateProgram() { return g->programs.make(); }

    void GLAD_API_PTR AttachShader(GLuint program, GLuint shader)
    {
        if (Program *p = g->programs.get(program))
            p->shaders.push_back(shader);
    }

    void GLAD_API_PTR LinkProgram(GLuint program)
    {
        Program *p = g->programs.get(program);
        if (!p)
            return;
        // (GT3_VKGL_STATS=1: time spent here, glslang parse + SPIR-V + reflection on the renderer thread.)
        const auto t0 = std::chrono::steady_clock::now();
        if (!linkProgram(*p))
            std::fprintf(stderr, "[vkgl] program link failed: %s\n", p->log.c_str());
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        ++g->stats.links;
        g->stats.linkNs += ns;
        if (g_stats && ns >= 50000000ull)
            std::fprintf(stderr, "[vkgl] slow program link: %.0f ms (program %u, %s)\n", ns / 1e6, program, p->compute ? "compute" : "graphics");
        // Its pipelines listed in the store: built ahead from now.
        if (p->linked && !p->compute)
            storeProgramLinked(program, *p);
    }

    void GLAD_API_PTR GetProgramiv(GLuint program, GLenum pname, GLint *params)
    {
        Program *p = g->programs.get(program);
        if (!p)
            return;
        if (pname == GL_LINK_STATUS)
            *params = p->linked ? GL_TRUE : GL_FALSE;
        else if (pname == GL_INFO_LOG_LENGTH)
            *params = static_cast<GLint>(p->log.size() + 1);
        else
            *params = 0; // GL_PROGRAM_BINARY_LENGTH: none
    }

    void GLAD_API_PTR GetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
    {
        Program *p = g->programs.get(program);
        const std::string &log = p ? p->log : std::string();
        const size_t n = bufSize > 0 ? std::min(log.size(), static_cast<size_t>(bufSize - 1)) : 0;
        if (bufSize > 0)
        {
            std::memcpy(infoLog, log.data(), n);
            infoLog[n] = 0;
        }
        if (length)
            *length = static_cast<GLsizei>(n);
    }

    void GLAD_API_PTR GetProgramBinary(GLuint, GLsizei, GLsizei *length, GLenum *, void *)
    {
        if (length)
            *length = 0;
    }

    void GLAD_API_PTR DeleteProgram(GLuint program)
    {
        Program *p = g->programs.get(program);
        if (!p)
            return;
        // (Store threads building for it finish first: they use its modules and layout.)
        std::vector<VkPipeline> pipelines = storeForget(p->serial);
        for (auto &kv : p->pipelines)
            pipelines.push_back(kv.second);
        if (p->computePipeline)
            pipelines.push_back(p->computePipeline);
        VkShaderModule m0 = p->modules[0], m1 = p->modules[1];
        VkPipelineLayout layout = p->layout;
        VkDescriptorSetLayout setLayout = p->setLayout;
        later([=] {
            for (VkPipeline pl : pipelines)
                if (pl)
                    vkDestroyPipeline(vk, pl, nullptr);
            if (m0)
                vkDestroyShaderModule(vk, m0, nullptr);
            if (m1)
                vkDestroyShaderModule(vk, m1, nullptr);
            if (layout)
                vkDestroyPipelineLayout(vk, layout, nullptr);
            if (setLayout)
                vkDestroyDescriptorSetLayout(vk, setLayout, nullptr);
        });
        if (g->program == program)
            g->program = 0;
        g->programs.release(program);
    }

    void GLAD_API_PTR UseProgram(GLuint program) { g->program = program; }

    GLint GLAD_API_PTR GetUniformLocation(GLuint program, const GLchar *name)
    {
        Program *p = g->programs.get(program);
        if (!p || !name)
            return -1;
        auto it = p->byName.find(name);
        return it == p->byName.end() ? -1 : it->second;
    }

    namespace
    {
        void setUniform(GLint location, const void *data, size_t bytes, int unitValue)
        {
            Program *p = g->programs.get(g->program);
            if (!p || location < 0 || static_cast<size_t>(location) >= p->uniforms.size())
                return;
            const Uniform &u = p->uniforms[static_cast<size_t>(location)];
            if (u.resource >= 0)
            {
                Resource &r = p->resources[static_cast<size_t>(u.resource)];
                if (r.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                    r.unit = unitValue;
                return;
            }
            if (u.offset >= 0 && static_cast<size_t>(u.offset) + bytes <= p->uniformData.size() &&
                std::memcmp(p->uniformData.data() + u.offset, data, bytes) != 0)
            {
                std::memcpy(p->uniformData.data() + u.offset, data, bytes);
                if (static_cast<uint32_t>(u.offset) < p->pushSize)
                    p->pushDirty = true;
            }
        }
    }

    void GLAD_API_PTR Uniform1i(GLint location, GLint v0) { setUniform(location, &v0, 4, v0); }
    void GLAD_API_PTR Uniform1ui(GLint location, GLuint v0) { setUniform(location, &v0, 4, static_cast<int>(v0)); }
    void GLAD_API_PTR Uniform1f(GLint location, GLfloat v0) { setUniform(location, &v0, 4, 0); }
    void GLAD_API_PTR Uniform2f(GLint location, GLfloat v0, GLfloat v1)
    {
        const GLfloat v[2] = {v0, v1};
        setUniform(location, v, 8, 0);
    }
    void GLAD_API_PTR Uniform2i(GLint location, GLint v0, GLint v1)
    {
        const GLint v[2] = {v0, v1};
        setUniform(location, v, 8, 0);
    }
    void GLAD_API_PTR Uniform3i(GLint location, GLint v0, GLint v1, GLint v2)
    {
        const GLint v[3] = {v0, v1, v2};
        setUniform(location, v, 12, 0);
    }
    void GLAD_API_PTR Uniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
    {
        const GLint v[4] = {v0, v1, v2, v3};
        setUniform(location, v, 16, 0);
    }
    void GLAD_API_PTR Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
    {
        const GLfloat v[3] = {v0, v1, v2};
        setUniform(location, v, 12, 0);
    }
    void GLAD_API_PTR Uniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
    {
        const GLfloat v[4] = {v0, v1, v2, v3};
        setUniform(location, v, 16, 0);
    }

    // ---------------------------------------------------------------------- draws and dispatches

    void GLAD_API_PTR DrawArrays(GLenum mode, GLint first, GLsizei count)
    {
        if (mode != GL_TRIANGLES)
            logOnce("primitive mode", mode);
        draw(first, count, 1);
    }

    void GLAD_API_PTR DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
    {
        if (mode != GL_TRIANGLES)
            logOnce("primitive mode", mode);
        draw(first, count, instances);
    }

    void GLAD_API_PTR DispatchCompute(GLuint x, GLuint y, GLuint z)
    {
        Program *p = g->programs.get(g->program);
        if (!p || !p->linked || !p->compute || !p->computePipeline)
            return;
        endPass();
        g_reads.clear();
        g_writes.clear();
        gatherDescriptors(*p, g_dw, g_reads, g_writes);
        g_accesses.clear();
        for (Texture *t : g_reads)
            g_accesses.push_back({t, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, false, false});
        for (Texture *t : g_writes)
            g_accesses.push_back({t, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  true, false});
        syncAccesses(g_accesses, false);
        bindProgram(*p, VK_PIPELINE_BIND_POINT_COMPUTE, p->computePipeline, g_dw);
        vkCmdDispatch(cmd(), x, y, z);
        ++g->stats.dispatches;
    }

    // Every access is ordered by the hazard tracking (syncAccesses), which
    // covers what the renderer's glMemoryBarrier calls ask for.
    void GLAD_API_PTR MemoryBarrier(GLbitfield) {}

    bool depthBlitSupported()
    {
        static const bool ok = [] {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(dev->physical, VK_FORMAT_D32_SFLOAT, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
            return (fp.optimalTilingFeatures & need) == need;
        }();
        return ok;
    }

    bool gpuUpsampleOn()
    {
        static const bool on = !(std::getenv("GT3_VKGL_GPU_UPSAMPLE") && std::strcmp(std::getenv("GT3_VKGL_GPU_UPSAMPLE"), "0") == 0);
        return on;
    }

    void noteSampledTextures(const GLuint *textures, size_t count)
    {
        if (g)
            g->bindlessReads.assign(textures, textures + count);
    }

    // ---------------------------------------------------------------------- synchronisation

    GLsync GLAD_API_PTR FenceSync(GLenum, GLbitfield)
    {
        auto *s = new Sync();
        s->serial = g->recording ? g->serial : g->serial - 1;
        return reinterpret_cast<GLsync>(s);
    }

    GLenum GLAD_API_PTR ClientWaitSync(GLsync sync, GLbitfield, GLuint64 timeout)
    {
        auto *s = reinterpret_cast<Sync *>(sync);
        if (!s)
            return GL_WAIT_FAILED;
        poll();
        if (s->serial <= g->completed)
            return GL_ALREADY_SIGNALED;
        return waitSerial(s->serial, timeout) ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
    }

    void GLAD_API_PTR DeleteSync(GLsync sync) { delete reinterpret_cast<Sync *>(sync); }

    void GLAD_API_PTR Flush() { submit(); }

    void GLAD_API_PTR Finish()
    {
        submit();
        waitSerial(g->serial - 1);
    }

    // ---------------------------------------------------------------------- queries (profiling)
    // GL_TIME_ELAPSED: GPU timestamps around the work between Begin and End (the renderer's per-batch GPU profiler);
    // other queries are not measured (0, always available).

    namespace
    {
        constexpr uint32_t timestampSlots = 32768;
        bool timestamps()
        {
            if (g->timestampsTried)
                return g->timestampsOk;
            g->timestampsTried = true;
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, nullptr);
            std::vector<VkQueueFamilyProperties> fams(n);
            vkGetPhysicalDeviceQueueFamilyProperties(dev->physical, &n, fams.data());
            const uint32_t bits = dev->queueFamily < n ? fams[dev->queueFamily].timestampValidBits : 0;
            if (!dev->hostQueryReset || !bits)
            {
                std::fprintf(stderr, "[vkgl] GPU timers unavailable (host query reset %d, timestamp bits %u): not measured\n", dev->hostQueryReset ? 1 : 0, bits);
                return false;
            }
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = timestampSlots;
            if (vkCreateQueryPool(vk, &qi, nullptr, &g->timestampPool) != VK_SUCCESS)
                return false;
            vkResetQueryPool(vk, g->timestampPool, 0, timestampSlots);
            g->timestampNs = static_cast<double>(dev->properties.limits.timestampPeriod);
            g->timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1ull);
            g->timestampsOk = true;
            std::fprintf(stderr, "[vkgl] GPU timers on (%u-bit timestamps, %.3f ns per tick)\n", bits, g->timestampNs);
            return true;
        }
    } // namespace

    void GLAD_API_PTR GenQueries(GLsizei n, GLuint *ids)
    {
        for (GLsizei i = 0; i < n; ++i)
            ids[i] = ++g->queryCounter;
    }
    void GLAD_API_PTR BeginQuery(GLenum target, GLuint id)
    {
        if (target != GL_TIME_ELAPSED || !timestamps())
            return;
        Context::TimeQuery &q = g->timeQueries[id];
        q.slot = g->timestampNext;
        g->timestampNext = (g->timestampNext + 2) % timestampSlots;
        vkResetQueryPool(vk, g->timestampPool, q.slot, 2); // (the ring is long: its slots' last use has finished)
        const VkCommandBuffer cb = cmd();
        q.serial = g->serial;
        q.ended = false;
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->timestampPool, q.slot);
        g->activeTimeQuery = id;
    }
    void GLAD_API_PTR EndQuery(GLenum target)
    {
        if (target != GL_TIME_ELAPSED || !g->activeTimeQuery)
            return;
        const auto it = g->timeQueries.find(g->activeTimeQuery);
        g->activeTimeQuery = 0;
        if (it == g->timeQueries.end())
            return;
        const VkCommandBuffer cb = cmd();
        it->second.serial = g->serial;
        it->second.ended = true;
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->timestampPool, it->second.slot + 1);
    }
    void GLAD_API_PTR GetQueryObjectiv(GLuint id, GLenum, GLint *params)
    {
        const auto it = g->timeQueries.find(id);
        if (it == g->timeQueries.end() || !it->second.ended)
        {
            *params = 1;
            return;
        }
        poll();
        *params = it->second.serial <= g->completed ? 1 : 0;
    }
    void GLAD_API_PTR GetQueryObjectui64v(GLuint id, GLenum, GLuint64 *params)
    {
        *params = 0;
        const auto it = g->timeQueries.find(id);
        if (it == g->timeQueries.end() || !it->second.ended)
            return;
        if (it->second.serial > g->completed)
        {
            if (it->second.serial >= g->serial)
                submit();
            waitSerial(it->second.serial);
        }
        uint64_t t[2] = {};
        if (vkGetQueryPoolResults(vk, g->timestampPool, it->second.slot, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            *params = static_cast<GLuint64>(static_cast<double>((t[1] - t[0]) & g->timestampMask) * g->timestampNs);
    }

    // ---------------------------------------------------------------------- queries of the implementation

    GLenum GLAD_API_PTR GetError() { return GL_NO_ERROR; }

    void GLAD_API_PTR GetIntegerv(GLenum pname, GLint *data)
    {
        const VkPhysicalDeviceLimits &l = dev->properties.limits;
        switch (pname)
        {
        case GL_MAX_TEXTURE_SIZE: *data = static_cast<GLint>(l.maxImageDimension2D); break;
        case GL_NUM_EXTENSIONS: *data = static_cast<GLint>(g->extensions.size()); break;
        case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: *data = static_cast<GLint>(l.minStorageBufferOffsetAlignment); break;
        case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: *data = static_cast<GLint>(std::min<uint64_t>(l.maxStorageBufferRange, 0x7FFFFFFFu)); break;
        case GL_MAX_FRAMEBUFFER_WIDTH: *data = static_cast<GLint>(l.maxFramebufferWidth); break;
        case GL_MAX_FRAMEBUFFER_HEIGHT: *data = static_cast<GLint>(l.maxFramebufferHeight); break;
        case GL_MAJOR_VERSION: *data = 4; break;
        case GL_MINOR_VERSION: *data = 6; break;
        // GT4's stable targets (gs_gl_backend.cpp soloFboFor) keep the draw and
        // read framebuffer bindings across making a framebuffer; an answer of 0
        // left the next batch drawing into "the default framebuffer" (dropped).
        case GL_DRAW_FRAMEBUFFER_BINDING: *data = static_cast<GLint>(g->drawFbo); break; // (= GL_FRAMEBUFFER_BINDING)
        case GL_READ_FRAMEBUFFER_BINDING: *data = static_cast<GLint>(g->readFbo); break;
        default: logOnce("integer query", pname); *data = 0; break;
        }
    }

    const GLubyte *GLAD_API_PTR GetString(GLenum name)
    {
        if (!available())
            return nullptr;
        switch (name)
        {
        case GL_VENDOR: return reinterpret_cast<const GLubyte *>("vkgl");
        case GL_RENDERER: return reinterpret_cast<const GLubyte *>(g->rendererName.c_str());
        case GL_VERSION: return reinterpret_cast<const GLubyte *>(g->versionName.c_str());
        default: return reinterpret_cast<const GLubyte *>("");
        }
    }

    const GLubyte *GLAD_API_PTR GetStringi(GLenum, GLuint index)
    {
        return index < g->extensions.size() ? reinterpret_cast<const GLubyte *>(g->extensions[index].c_str()) : nullptr;
    }

    void GLAD_API_PTR DebugMessageCallback(GLDEBUGPROC, const void *) {}

    void *getProcAddress(const char *name)
    {
        const std::string n = name ? name : "";
        if (n == "glGetTextureHandleARB")
            return reinterpret_cast<void *>(&getTextureHandle);
        if (n == "glGetTextureSamplerHandleARB")
            return reinterpret_cast<void *>(&getTextureSamplerHandle);
        if (n == "glMakeTextureHandleResidentARB")
            return reinterpret_cast<void *>(&makeHandleResident);
        if (n == "glMakeTextureHandleNonResidentARB")
            return reinterpret_cast<void *>(&makeHandleNonResident);
        if (n == "glClipControl")
            return reinterpret_cast<void *>(&clipControl);
        if (n == "glBufferStorage")
            return reinterpret_cast<void *>(&bufferStorage);
        if (n == "glClearTexImage")
            return reinterpret_cast<void *>(&clearTexImage);
        if ((n == "glTextureBarrier" || n == "glTextureBarrierNV") && localReadOn())
            return reinterpret_cast<void *>(&textureBarrier);
        return nullptr; // (no local read: blends that read the target get a copy of it)
    }

    // ---------------------------------------------------------------------- presentation

    namespace
    {
        // OpenGL's EXT_memory_object_win32 / EXT_semaphore_win32 (the real
        // OpenGL of the calling thread's context; glad has no entry points).
        struct GlInterop
        {
            bool loaded = false, ok = false;
            void(GLAD_API_PTR *createMemoryObjects)(GLsizei, GLuint *) = nullptr;
            void(GLAD_API_PTR *deleteMemoryObjects)(GLsizei, const GLuint *) = nullptr;
            void(GLAD_API_PTR *memoryObjectParameteriv)(GLuint, GLenum, const GLint *) = nullptr;
            void(GLAD_API_PTR *importMemoryWin32Handle)(GLuint, GLuint64, GLenum, void *) = nullptr;
            void(GLAD_API_PTR *texStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) = nullptr;
            void(GLAD_API_PTR *genSemaphores)(GLsizei, GLuint *) = nullptr;
            void(GLAD_API_PTR *importSemaphoreWin32Handle)(GLuint, GLenum, void *) = nullptr;
            void(GLAD_API_PTR *waitSemaphore)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *) = nullptr;
            void(GLAD_API_PTR *signalSemaphore)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *) = nullptr;
        } glx;
        constexpr GLenum kHandleOpaqueWin32 = 0x9587, kDedicatedMemoryObject = 0x9581, kLayoutGeneral = 0x958D;

        template <typename F>
        void loadGl(F &f, const char *name)
        {
            PROC p = wglGetProcAddress(name);
            f = reinterpret_cast<F>(p);
        }

        bool glInterop()
        {
            if (!glx.loaded)
            {
                glx.loaded = true;
                loadGl(glx.createMemoryObjects, "glCreateMemoryObjectsEXT");
                loadGl(glx.deleteMemoryObjects, "glDeleteMemoryObjectsEXT");
                loadGl(glx.memoryObjectParameteriv, "glMemoryObjectParameterivEXT");
                loadGl(glx.importMemoryWin32Handle, "glImportMemoryWin32HandleEXT");
                loadGl(glx.texStorageMem2D, "glTexStorageMem2DEXT");
                loadGl(glx.genSemaphores, "glGenSemaphoresEXT");
                loadGl(glx.importSemaphoreWin32Handle, "glImportSemaphoreWin32HandleEXT");
                loadGl(glx.waitSemaphore, "glWaitSemaphoreEXT");
                loadGl(glx.signalSemaphore, "glSignalSemaphoreEXT");
                // GT3_VK_INTEROP=0 (test): the CPU path, as on a driver without these.
                const char *interopEnv = std::getenv("GT3_VK_INTEROP");
                glx.ok = glx.createMemoryObjects && glx.deleteMemoryObjects && glx.memoryObjectParameteriv && glx.importMemoryWin32Handle &&
                         glx.texStorageMem2D && glx.genSemaphores && glx.importSemaphoreWin32Handle && glx.waitSemaphore && glx.signalSemaphore &&
                         dev->externalMemory && !(interopEnv && *interopEnv == '0');
                if (!glx.ok)
                    std::fprintf(stderr, "[vkgl] OpenGL interop unavailable: frames go through the CPU\n");
                // Which GPU this thread's OpenGL context is on (its LUID), next to Vulkan's.
                void(GLAD_API_PTR * getUnsignedBytev)(GLenum, GLubyte *) = nullptr;
                loadGl(getUnsignedBytev, "glGetUnsignedBytevEXT");
                GLubyte luid[8]{};
                if (getUnsignedBytev)
                    getUnsignedBytev(0x9599 /* GL_DEVICE_LUID_EXT */, luid);
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                vkGetPhysicalDeviceProperties2(dev->physical, &props);
                std::fprintf(stderr, "[vkgl] presentation context: %s; OpenGL LUID %02x%02x%02x%02x%02x%02x%02x%02x, Vulkan LUID %02x%02x%02x%02x%02x%02x%02x%02x\n",
                             reinterpret_cast<const char *>(glGetString(GL_RENDERER)), luid[0], luid[1], luid[2], luid[3], luid[4], luid[5], luid[6],
                             luid[7], id.deviceLUID[0], id.deviceLUID[1], id.deviceLUID[2], id.deviceLUID[3], id.deviceLUID[4], id.deviceLUID[5],
                             id.deviceLUID[6], id.deviceLUID[7]);
            }
            return glx.ok;
        }

        bool exportSemaphore(VkSemaphore &vkSem, GLuint &glSem)
        {
            VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
            es.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &es};
            if (vkCreateSemaphore(vk, &sci, nullptr, &vkSem) != VK_SUCCESS)
                return false;
            VkSemaphoreGetWin32HandleInfoKHR sh{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
            sh.semaphore = vkSem;
            sh.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            HANDLE handle = nullptr;
            if (vkGetSemaphoreWin32HandleKHR(vk, &sh, &handle) != VK_SUCCESS)
                return false;
            glx.genSemaphores(1, &glSem);
            glx.importSemaphoreWin32Handle(glSem, kHandleOpaqueWin32, handle);
            CloseHandle(handle);
            return true;
        }

        bool makePresentImage(Context::PresentImage &pi, int width, int height)
        {
            if (pi.name)
            {
                glFinish(); // OpenGL's last copy out of it
                glDeleteTextures(1, &pi.glTexture);
                glx.deleteMemoryObjects(1, &pi.glMemory);
                pi.glTexture = pi.glMemory = 0;
                const GLuint old[2] = {pi.name, pi.target};
                DeleteTextures(2, old);
                pi.name = pi.target = 0;
            }
            pi.glSignalled = false;
            pi.target = g->textures.make();
            if (!createImage(*g->textures.get(pi.target), GL_RGBA8, width, height))
                return false;
            g->textures.get(pi.target)->minFilter = g->textures.get(pi.target)->magFilter = GL_NEAREST;
            pi.name = g->textures.make();
            Texture &t = *g->textures.get(pi.name);
            if (!createImage(t, GL_RGBA8, width, height, true) || !t.memory.memory)
                return false;
            t.minFilter = t.magFilter = GL_NEAREST;
            VkMemoryGetWin32HandleInfoKHR hi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
            hi.memory = t.memory.memory;
            hi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            HANDLE handle = nullptr;
            if (vkGetMemoryWin32HandleKHR(vk, &hi, &handle) != VK_SUCCESS)
                return false;
            while (glGetError() != GL_NO_ERROR)
            {
            }
            glx.createMemoryObjects(1, &pi.glMemory);
            const GLint dedicated = GL_TRUE;
            glx.memoryObjectParameteriv(pi.glMemory, kDedicatedMemoryObject, &dedicated);
            glx.importMemoryWin32Handle(pi.glMemory, t.memory.size, kHandleOpaqueWin32, handle);
            const GLenum importError = glGetError();
            glGenTextures(1, &pi.glTexture);
            glBindTexture(GL_TEXTURE_2D, pi.glTexture);
            glx.texStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, pi.glMemory, 0);
            const GLenum storageError = glGetError();
            // Only now: the driver may resolve the handle when the texture's
            // storage is made (closed right after the import, OpenGL got
            // memory of its own: zeros; `local/vk*`).
            CloseHandle(handle);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindTexture(GL_TEXTURE_2D, 0);
            std::fprintf(stderr, "[vkgl] presentation image %dx%d shared with OpenGL (%llu bytes; import error 0x%x, storage error 0x%x)\n", width,
                         height, static_cast<unsigned long long>(t.memory.size), importError, storageError);
            // GT3_VKGL_INTEROP_PROBE=1: OpenGL fills the shared texture, Vulkan
            // reads it (diagnostics: is the memory shared at all?).
            if (const char *probe = std::getenv("GT3_VKGL_INTEROP_PROBE"); probe && *probe == '1')
            {
                Finish(); // the image's first layout transition goes before OpenGL's fill
                std::vector<uint8_t> fill(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0x7F);
                glBindTexture(GL_TEXTURE_2D, pi.glTexture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, fill.data());
                std::vector<uint8_t> back(fill.size(), 0);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glFinish();
                std::vector<uint8_t> seen(fill.size(), 0);
                readTexture(t, 0, 0, width, height, seen.data(), static_cast<size_t>(width) * 4u);
                size_t glSame = 0, vkSame = 0;
                for (size_t i = 0; i < fill.size(); i += 101)
                {
                    glSame += back[i] == 0x7F;
                    vkSame += seen[i] == 0x7F;
                }
                std::fprintf(stderr, "[vkgl] interop probe: OpenGL reads its fill at %zu, Vulkan at %zu of %zu samples\n", glSame, vkSame,
                             (fill.size() + 100) / 101);
            }
            if (!pi.vkDone && (!exportSemaphore(pi.vkDone, pi.glVkDone) || !exportSemaphore(pi.glDone, pi.glGlDone)))
                return false;
            pi.width = width;
            pi.height = height;
            return true;
        }
    }

    bool presentAvailable()
    {
        // GT3_VKGL_THREAD_TEST=1 (diagnostics): the phase 1 interop test on this thread.
        static const bool threadTest = [] {
            const char *v = std::getenv("GT3_VKGL_THREAD_TEST");
            if (v && *v == '1' && gsvk::testInit(64, 64))
            {
                gsvk::testFrame(1.0f);
                std::fprintf(stderr, "[vkgl] interop test on the renderer thread: OpenGL read back %d non-zero samples\n", gsvk::testReadbackNonZero());
                gsvk::testFrameDone();
                gsvk::testShutdown();
            }
            return true;
        }();
        (void)threadTest;
        // The window presenter (gs_vk_present.h): no OpenGL involved.
        if (gsvk::present::active())
            return available();
        return available() && glInterop();
    }

    GLuint presentBegin()
    {
        if (gsvk::present::active())
        {
            // A slot neither shown nor the newest; its drawing waits for the
            // host's last reads of it.
            const int width = 640 * gsgl::presentScale(), rows = static_cast<int>(gsgl::presentRows()) * gsgl::presentScaleY();
            uint64_t hostWait = 0;
            const int s = gsvk::present::rendererSlot(hostWait);
            Context::PresentImage &pi = g->present[s];
            const int height = pi.width == width && pi.height > rows ? pi.height : rows;
            submit(); // the work so far does not wait
            if (hostWait)
            {
                g->submitWait = gsvk::present::hostTimeline();
                g->submitWaitValue = hostWait;
            }
            if (pi.width != width || pi.height != height || !pi.target)
            {
                // (Its old image goes once this command buffer is done, after the host's reads.)
                if (pi.target)
                    DeleteTextures(1, &pi.target);
                pi.target = g->textures.make();
                Texture &t = *g->textures.get(pi.target);
                if (!createImage(t, GL_RGBA8, width, height))
                {
                    std::fprintf(stderr, "[vkgl] presentation image %dx%d failed\n", width, height);
                    pi.width = pi.height = 0;
                    g->presentCurrent = -1;
                    return 0;
                }
                t.minFilter = t.magFilter = GL_NEAREST;
                pi.width = width;
                pi.height = height;
                std::fprintf(stderr, "[vkgl] presentation slot %d: %dx%d (the window presenter)\n", s, width, height);
            }
            g->presentCurrent = s;
            return pi.target;
        }
        if (!glInterop())
            return 0;
        // 640x512 per scale, or 640x1024 for 1080i's 960-row pictures (gsgl::presentRows).
        // Like the GL slots, the image only grows in height (a 480i frame after a
        // 1080i race keeps the taller image), so the mode switches do not make it again.
        const int width = 640 * gsgl::presentScale(), rows = static_cast<int>(gsgl::presentRows()) * gsgl::presentScaleY();
        Context::PresentImage &pi = g->present[g->presentNext];
        const int height = pi.width == width && pi.height > rows ? pi.height : rows;
        if ((pi.width != width || pi.height != height) && !makePresentImage(pi, width, height))
        {
            std::fprintf(stderr, "[vkgl] presentation image %dx%d failed\n", width, height);
            pi.width = pi.height = 0;
            return 0;
        }
        if (pi.glSignalled)
        {
            // OpenGL's copy of the frame before last out of this image: the
            // work queued so far goes first, the drawing into it waits.
            submit();
            g->submitWait = pi.glDone;
            pi.glSignalled = false;
        }
        g->presentCurrent = g->presentNext;
        g->presentNext ^= 1;
        return pi.target;
    }

    void presentEnd(uint32_t width, uint32_t height)
    {
        if (g->presentCurrent < 0)
            return;
        const int presentSlot = g->presentCurrent;
        Context::PresentImage &pi = g->present[g->presentCurrent];
        g->presentCurrent = -1;
        if (g_stats)
        {
            const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                           std::chrono::steady_clock::now().time_since_epoch())
                                                           .count());
            if (!g->statsStart)
                g->statsStart = now;
            else if (now - g->statsStart >= 5000000000ull)
            {
                const Context::Stats &s = g->stats;
                const double secs = static_cast<double>(now - g->statsStart) / 1e9;
                std::fprintf(stderr,
                             "[vkgl] per second: %.0f draws, %.0f passes, %.0f barriers (+%.0f in-pass), %.0f submits (%.0f GPU idle), %.0f copies (%.0f small, %.0f page-size), %.0f blits, %.0f uploads "
                             "(%.1f MB), %.0f read-backs (%.1f ms waiting), %.0f dispatches, %.0f descriptor pushes, %.0f texture clears; %llu new pipelines (%.0f ms, "
                             "%llu built ahead), %llu programs linked (%.0f ms)\n",
                             s.draws / secs, s.passes / secs, s.barriers / secs, s.localBarriers / secs, s.submits / secs, s.idleSubmits / secs, s.copies / secs, s.smallCopies / secs, s.pageCopies / secs, s.blits / secs, s.uploads / secs,
                             s.uploadBytes / secs / 1e6, s.readbacks / secs, s.waitNs / secs / 1e6, s.dispatches / secs, s.descriptorPushes / secs,
                             s.clears / secs, static_cast<unsigned long long>(s.pipelines), s.pipelineNs / 1e6, static_cast<unsigned long long>(s.pipelinesAhead),
                             static_cast<unsigned long long>(s.links), s.linkNs / 1e6);
                std::fprintf(stderr, "[vkgl] barrier causes (sampled/storage/colour/depth/input x by attachment/transfer/shader/other):");
                for (auto &row : g_barrierWhy)
                {
                    std::fprintf(stderr, " |");
                    for (uint64_t &c : row)
                    {
                        std::fprintf(stderr, " %llu", (unsigned long long)c);
                        c = 0;
                    }
                }
                std::fprintf(stderr, "\n");
                g->stats = {};
                g->statsStart = now;
            }
        }
        // GT3_VKGL_PRESENT_DUMP=<n>: the n-th frame as Vulkan and as OpenGL
        // see it (vkgl-present-{vk,gl}.ppm; diagnostics).
        static const long dumpAt = std::getenv("GT3_VKGL_PRESENT_DUMP") ? std::atol(std::getenv("GT3_VKGL_PRESENT_DUMP")) : -1;
        static long presented = 0;
        const bool dump = presented++ == dumpAt;
        auto writePpm = [&](const char *path, const std::vector<uint8_t> &rgba) {
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", width, height);
                for (size_t i = 0; i + 3 < rgba.size(); i += 4)
                    std::fwrite(&rgba[i], 1, 3, f);
                std::fclose(f);
            }
        };
        if (gsvk::present::active())
        {
            // The window presenter: the frame stays in its slot image (GENERAL
            // layout, as every vkgl image); the submission's timeline signal
            // orders the host's reads after its drawing.
            endPass();
            Texture *t = g->textures.get(pi.target);
            if (!t)
                return;
            if (dump)
            {
                std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
                readTexture(*t, 0, 0, static_cast<int>(width), static_cast<int>(height), pixels.data(), static_cast<size_t>(width) * 4u);
                writePpm("vkgl-present-vk.ppm", pixels);
                std::fprintf(stderr, "[vkgl] presentation %ld dumped (%ux%u of %dx%d)\n", dumpAt, width, height, pi.width, pi.height);
            }
            const uint64_t value = gsvk::present::rendererNextValue();
            g->submitSignal = gsvk::present::rendererTimeline();
            g->submitSignalValue = value;
            submit();
            const gsgl::FieldInfo field = gsgl::composingField();
            gsvk::present::rendererPublish(presentSlot, t->image, t->view, pi.width, pi.height, width, height, value, gsgl::composingTick(),
                                           field.parity, field.mode, field.crt, field.lines);
            return;
        }
        // Into the image OpenGL shares (not a render target, so laid out as
        // OpenGL expects): taken back from OpenGL's queue family, copied,
        // handed to it again.
        Texture *shared = g->textures.get(pi.name);
        auto ownership = [&](bool toExternal) {
            endPass();
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = toExternal ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
            ib.srcAccessMask = toExternal ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0;
            ib.dstStageMask = toExternal ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ib.dstAccessMask = toExternal ? 0 : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = toExternal ? dev->queueFamily : VK_QUEUE_FAMILY_EXTERNAL;
            ib.dstQueueFamilyIndex = toExternal ? VK_QUEUE_FAMILY_EXTERNAL : dev->queueFamily;
            ib.image = shared->image;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = 1;
            di.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd(), &di);
        };
        ownership(false);
        CopyImageSubData(pi.target, GL_TEXTURE_2D, 0, 0, 0, 0, pi.name, GL_TEXTURE_2D, 0, 0, 0, 0, static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height), 1);
        ownership(true);
        if (dump)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(pi.width) * static_cast<size_t>(pi.height) * 4u);
            if (Texture *t = g->textures.get(pi.target))
                readTexture(*t, 0, 0, static_cast<int>(width), static_cast<int>(height), pixels.data(), static_cast<size_t>(width) * 4u);
            pixels.resize(static_cast<size_t>(width) * height * 4u);
            writePpm("vkgl-present-vk.ppm", pixels);
        }
        g->submitSignal = pi.vkDone;
        submit();
        glx.waitSemaphore(pi.glVkDone, 0, nullptr, 1, &pi.glTexture, &kLayoutGeneral);
        if (dump)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(pi.width) * static_cast<size_t>(pi.height) * 4u);
            glBindTexture(GL_TEXTURE_2D, pi.glTexture);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            std::vector<uint8_t> rows(static_cast<size_t>(width) * height * 4u);
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(&rows[static_cast<size_t>(y) * width * 4u], &pixels[static_cast<size_t>(y) * static_cast<size_t>(pi.width) * 4u], width * 4u);
            writePpm("vkgl-present-gl.ppm", rows);
            std::fprintf(stderr, "[vkgl] presentation %ld dumped (%ux%u of %dx%d)\n", dumpAt, width, height, pi.width, pi.height);
        }
        const GLuint slot = gsgl::beginPresentSlot();
        glCopyImageSubData(pi.glTexture, GL_TEXTURE_2D, 0, 0, 0, 0, slot, GL_TEXTURE_2D, 0, 0, 0, 0, static_cast<GLsizei>(width),
                           static_cast<GLsizei>(height), 1);
        glx.signalSemaphore(pi.glGlDone, 0, nullptr, 1, &pi.glTexture, &kLayoutGeneral);
        pi.glSignalled = true;
        gsgl::endPresentSlot(width, height);
    }
}

// ---------------------------------------------------------------------- the pipeline store, for the runtime (gs_vk_backend.h)

void gsvk::pipelineCacheSave(const char *why)
{
    if (!vkgl::g_store)
        return;
    {
        std::lock_guard<std::mutex> lock(vkgl::g_store->mutex);
        std::fprintf(stderr, "[vkgl] pipeline store (%s, +%.1f s): %s\n", why, vkgl::storeSeconds(), vkgl::storeSummary().c_str());
    }
    // The last save: written when anything is new since the last one; it
    // waits for a store thread's save in progress, and none starts after it.
    vkgl::storeSave(why, true);
}

bool gsvk::pipelinePrepareRequested() { return vkgl::envIs("GT3_VKGL_PREPARE", "1"); }

int gsvk::pipelinePrepareRun()
{
    vkgl::Store *s = vkgl::g_store;
    if (!s)
    {
        std::fprintf(stderr, "[vkgl] prepare: the Vulkan renderer is not running (GT3_RENDERER=vk and a usable Vulkan device are needed): nothing built\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    size_t built = 0, failed = 0, linked = 0;
    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->done.wait(lock, [&] { return s->queue.empty() && s->running == 0; });
        built = static_cast<size_t>(s->aheadBuilt);
        failed = static_cast<size_t>(s->aheadFailed);
        linked = s->linked.size();
        std::fprintf(stderr, "[vkgl] prepare: %zu pipelines built in %.0f ms after the renderer started (+%.1f s); %s\n", built,
                     vkgl::nsSince(t0) / 1e6, vkgl::storeSeconds(), vkgl::storeSummary().c_str());
    }
    // (The renderer links its programs while it starts, before this runs; one
    // it did not link has nothing built: not done.)
    if (!linked)
    {
        std::fprintf(stderr, "[vkgl] prepare: the renderer linked no graphics program: nothing built, nothing saved\n");
        return 1;
    }
    if (!s->persist)
        std::fprintf(stderr, "[vkgl] prepare: no cache file to write (GT3_VKGL_PIPELINE_CACHE=0, or the driver made no pipeline cache)\n");
    const bool saved = vkgl::storeSave("prepare", true);
    // (A failed build is not in the file: the game makes it on the spot. Not done.)
    if (saved && failed)
        std::fprintf(stderr, "[vkgl] prepare: %zu of the listed pipelines failed to build: saved without them, not done\n", failed);
    return saved && !failed ? 0 : 1;
}
