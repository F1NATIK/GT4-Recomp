#pragma once

// The GS renderer on Vulkan (GT3_RENDERER=vk; built with GT3_VULKAN=ON): the
// OpenGL renderer's source compiled against vkgl (gs_vkgl.h), so it draws the
// same way through Vulkan. No Vulkan types here: included next to raylib.

#include "runtime/gs/gs_backend.h"

#include <functional>
#include <memory>

namespace gsvk
{
    using RunOnThread = std::function<void(const std::function<void()> &)>;

    // As the OpenGL renderer: `run` executes a task on the renderer thread,
    // `runNext` before GS packets still queued. compare: also a CPU renderer,
    // compared at every presentation (GT3_GS_COMPARE).
    std::unique_ptr<GSRasterBackend> createBackend(RunOnThread run, RunOnThread runNext, bool compare);

    // The renderer's pipeline store (gs_vkgl.cpp, "pipeline store"): its
    // pipeline cache file and key list. All do nothing when the Vulkan
    // renderer is not running or the store is off (GT3's default).
    // As the game exits (any thread): its summary, and the files written
    // when anything is new since they were last; nothing is written after it.
    void pipelineCacheSave(const char *why);
    // GT3_VKGL_PREPARE=1: only build the pipelines into the cache file, for a
    // launcher's "Preparing shaders" step. After the renderer started: waits
    // for the store's threads, saves; the exit code (0 done, 1 not done: not
    // saved, no program linked, or a listed pipeline failed to build, 2 the
    // Vulkan renderer is not running).
    bool pipelinePrepareRequested();
    int pipelinePrepareRun();
}
