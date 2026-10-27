// The GS renderer on Vulkan: gs_gl_backend.cpp compiled a second time with
// its OpenGL calls going to vkgl (gs_vkgl.h) and its classes renamed. Frames
// are drawn into images OpenGL shares and copied into the host's
// presentation slots (vkgl::presentBegin / presentEnd).

#include "external/glad.h"
#define VKGL_REDIRECT 1
#include "gs_vkgl.h"
#include "gs_gl_context.h"
#include "gs_gl_present.h"

namespace gsgl
{
    // The renderer source's gsgl:: calls that touch OpenGL objects -> vkgl.
    inline void *vkglGetProcAddress(const char *name) { return vkgl::getProcAddress(name); }
    inline unsigned vkglBeginPresentSlot() { return vkgl::presentBegin(); }
    inline void vkglEndPresentSlot(uint32_t width, uint32_t height) { vkgl::presentEnd(width, height); }
}
#define getProcAddress vkglGetProcAddress
#define beginPresentSlot vkglBeginPresentSlot
#define endPresentSlot vkglEndPresentSlot
#define GSGlBackend GSVkBackend
#define GSCompareBackend GSVkCompareBackend
#define GS_GL_GPU_PRESENT (vkgl::presentAvailable())
#define GS_NOTE_BATCH_TEXTURES(textures, count) vkgl::noteSampledTextures(textures, count)
// (In writeTargetPixels, where `depth` is the target's kind.)
#define GS_GPU_UPSAMPLE (vkgl::gpuUpsampleOn() && (!depth || vkgl::depthBlitSupported()))
// A submission per 24 batches (4x progressive at Trial Mountain, laptop, with
// 32 command buffers in flight: 58.1 fps at 8, 59.4 at 24, 59.2 at 48).
#define GS_FLUSH_EVERY_DEFAULT 24u

#include "gs_gl_backend.cpp"

#include "gs_vk_backend.h"

std::unique_ptr<GSRasterBackend> gsvk::createBackend(RunOnThread run, RunOnThread runNext, bool compare)
{
    auto backend = std::make_unique<GSVkBackend>(std::move(run), std::move(runNext));
    if (compare)
        return std::make_unique<GSVkCompareBackend>(std::move(backend));
    return backend;
}
