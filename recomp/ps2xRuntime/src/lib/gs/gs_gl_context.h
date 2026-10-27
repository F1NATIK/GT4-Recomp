#pragma once

#include <string>

// OpenGL context for the GPU renderer (GSGlBackend). GLFW creates windows only
// on the main thread, so createSharedContext() runs there after InitWindow and
// makes a hidden window whose context shares textures and buffers with the
// raylib window's; the GS worker thread then makes it current on itself.
namespace gsgl
{
    bool createSharedContext();
    bool hasSharedContext();
    // On the calling thread (the GS worker).
    bool makeSharedContextCurrent();
    void releaseCurrentContext();
    // The raylib window's context again (main thread; after createSharedContext).
    bool makeMainContextCurrent();
    // An OpenGL entry point for the current context (nullptr if unavailable).
    void *getProcAddress(const char *name);
    // GL_VENDOR of the context current on the calling thread (empty without one).
    std::string currentVendor();
}
