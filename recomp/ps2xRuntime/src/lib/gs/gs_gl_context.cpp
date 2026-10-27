#include "gs_gl_context.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>

namespace
{
    GLFWwindow *g_shared = nullptr;
    GLFWwindow *g_main = nullptr;
}

bool gsgl::makeMainContextCurrent()
{
    if (!g_main)
        return false;
    glfwMakeContextCurrent(g_main);
    return true;
}

bool gsgl::createSharedContext()
{
    if (g_shared)
        return true;
    // raylib's window context is current on the main thread; GetWindowHandle()
    // returns the native HWND on Windows, not the GLFW window.
    GLFWwindow *main = glfwGetCurrentContext();
    if (!main)
    {
        std::cerr << "[gs-gl] no current GLFW context on the main thread" << std::endl;
        return false;
    }
    g_main = main;
    // raylib left its context hints (OpenGL 4.3 core) in place; only hide it.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    g_shared = glfwCreateWindow(16, 16, "GT3 GS", nullptr, main);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwMakeContextCurrent(main); // creation does not change it, but be explicit
    if (!g_shared)
        std::cerr << "[gs-gl] could not create the shared OpenGL context" << std::endl;
    return g_shared != nullptr;
}

bool gsgl::hasSharedContext()
{
    return g_shared != nullptr;
}

bool gsgl::makeSharedContextCurrent()
{
    if (!g_shared)
        return false;
    glfwMakeContextCurrent(g_shared);
    return glfwGetCurrentContext() == g_shared;
}

void gsgl::releaseCurrentContext()
{
    glfwMakeContextCurrent(nullptr);
}

void *gsgl::getProcAddress(const char *name)
{
    return reinterpret_cast<void *>(glfwGetProcAddress(name));
}

std::string gsgl::currentVendor()
{
    // GL_VENDOR (0x1F00) of the context current on this thread (the raylib window's on the main thread).
    if (!glfwGetCurrentContext())
        return {};
    using GetString = const unsigned char *(__stdcall *)(unsigned int);
    const auto getString = reinterpret_cast<GetString>(glfwGetProcAddress("glGetString"));
    const unsigned char *v = getString ? getString(0x1F00) : nullptr;
    return v ? std::string(reinterpret_cast<const char *>(v)) : std::string();
}
