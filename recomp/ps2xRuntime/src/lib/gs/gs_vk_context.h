#pragma once

// Vulkan renderer (gt3-analysis/VULKAN-RENDERER-PLAN.md), phase 1 self-test:
// the Vulkan device (gs_vk_device.h) on the same GPU as the OpenGL window, an
// image shared with OpenGL (external memory and semaphores), shaders compiled
// from GLSL to SPIR-V at run time (glslang). Built with GT3_VULKAN=ON
// (PS2X_GS_VK). No Vulkan types here: included next to raylib.
//
// GT3_VK_TEST=1: at startup the runtime shows a few seconds of an image that
// Vulkan renders (a compute shader) and OpenGL presents - the whole chain the
// Vulkan GS renderer will use.

namespace gsvk
{
    // With the OpenGL context current: whether the Vulkan renderer can run
    // here (a Vulkan 1.3 device on that GPU with push descriptors and OpenGL
    // interop, unless it shows frames with its own presenter: ownPresenter,
    // gs_vk_present.h). Logs why not.
    bool rendererUsable(bool ownPresenter = false);

    // With the OpenGL context current: a width x height RGBA8 image shared
    // with OpenGL. False (logged) when something is missing.
    bool testInit(int width, int height);
    // Renders frame `time` (seconds) with Vulkan and returns the OpenGL
    // texture holding it, ready to sample (0: failed).
    unsigned testFrame(float time);
    // After OpenGL has drawn from the texture: Vulkan may write it again.
    void testFrameDone();
    // Diagnostics: samples of the shared texture read back through OpenGL
    // (glGetTexImage) that are not zero.
    int testReadbackNonZero();
    void testShutdown();
    // One line about the device (after testInit).
    const char *deviceName();
    // Phase 2 check: compiles the OpenGL renderer's GS shaders (gs_gl_shaders.h)
    // to SPIR-V under glslang's relaxed Vulkan rules and logs each variant's
    // result and its loose uniforms. True when all compile.
    bool testCompileGsShaders();
}
