# raylib 5.5 calls rlglInit even when InitPlatform fails. Compile a guarded
# build-directory copy so missing displays/drivers return instead of crashing.
# The copy also includes a guarded copy of the GLFW platform file: with a
# hidden window (FLAG_WINDOW_HIDDEN, set only by --headless test runs) raylib
# never opens or reads host gamepads, wheels or pedals (GLFW sets joysticks up
# on first use, so XInput / DirectInput are then never called), and, when the
# runtime asks for it (GT4 --headless while the console display is off), it
# does not swap its buffers.
# Do not modify the fetched dependency or silently accept a changed upstream body.
function(ps2x_guard_raylib_replace text_var needle replacement what)
    string(FIND "${${text_var}}" "${needle}" first)
    string(FIND "${${text_var}}" "${needle}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "raylib ${what} changed; review the guard in RaylibInitGuard.cmake")
    endif()
    string(REPLACE "${needle}" "${replacement}" replaced "${${text_var}}")
    set(${text_var} "${replaced}" PARENT_SCOPE)
endfunction()

function(ps2x_guard_raylib_init)
    get_target_property(raylib_dir raylib SOURCE_DIR)
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/guarded-raylib")

    set(platform_original "${raylib_dir}/platforms/rcore_desktop_glfw.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${platform_original}")
    file(READ "${platform_original}" platform)
    set(no_pads "((CORE.Window.flags & FLAG_WINDOW_HIDDEN) == 0)")
    set(hidden "((CORE.Window.flags & FLAG_WINDOW_HIDDEN) != 0)")
    # The copy lives elsewhere: its relative includes (win32_clipboard.h) made absolute.
    string(FIND "${platform}" "\"../" relative_include)
    if(relative_include EQUAL -1)
        message(FATAL_ERROR "raylib GLFW platform includes changed; review the guard in RaylibInitGuard.cmake")
    endif()
    string(REPLACE "\"../" "\"${raylib_dir}/" platform "${platform}")
    ps2x_guard_raylib_replace(platform [=[    glfwSetJoystickCallback(NULL);]=]
        "    if ${no_pads} glfwSetJoystickCallback(NULL); // ps2x: hidden window, no gamepads" "joystick initialization")
    ps2x_guard_raylib_replace(platform [=[    glfwSetJoystickCallback(JoystickCallback);]=]
        "    if ${no_pads} glfwSetJoystickCallback(JoystickCallback); // ps2x: hidden window, no gamepads" "joystick callback")
    ps2x_guard_raylib_replace(platform [=[        if (glfwJoystickPresent(i)) strcpy(]=]
        "        if (${no_pads} && glfwJoystickPresent(i)) strcpy(" "gamepad name loop")
    ps2x_guard_raylib_replace(platform [=[        if (glfwJoystickPresent(i)) CORE.Input.Gamepad.ready[i] = true;]=]
        "        if (${no_pads} && glfwJoystickPresent(i)) CORE.Input.Gamepad.ready[i] = true; // not ready: never polled" "gamepad poll")
    # A hidden window's back buffer is never shown. When the runtime sets ps2x_skip_hidden_swap
    # (GT4 --headless runs, by default only while the console display is off), its buffer swap is
    # left out and the frame's commands are only flushed to the GPU. With the display off (locked PC) the NVIDIA driver's present of the hidden
    # window (through DXGI on the Intel display adapter) blocked ~0.5 s about once a second while it
    # held a driver lock the GS GL thread needs (analysis/phase5/scratch/step3/lockstall).
    # ps2x_vulkan_presents: the Vulkan renderer's window presenter (gs_vk_present.h) shows every frame
    # through its own swapchain on the window; OpenGL never swaps it (its draws are only flushed).
    ps2x_guard_raylib_replace(platform [=[void SwapScreenBuffer(void)]=] [=[// ps2x: set by the runtime (GT4 --headless, display off): a hidden window does not swap its buffers.
int ps2x_skip_hidden_swap = 0;
// ps2x: set by the runtime: a Vulkan swapchain presents to the window, OpenGL never swaps it.
int ps2x_vulkan_presents = 0;
static void ps2xFlushHiddenWindow(void)
{
    static void (__stdcall *flush)(void) = NULL;
    if (flush == NULL) flush = (void (__stdcall *)(void))glfwGetProcAddress("glFlush");
    if (flush != NULL) flush();
}
void SwapScreenBuffer(void)]=] "buffer swap function")
    ps2x_guard_raylib_replace(platform [=[    glfwSwapBuffers(platform.handle);]=]
        "    if ((ps2x_skip_hidden_swap && ${hidden}) || ps2x_vulkan_presents) { ps2xFlushHiddenWindow(); return; } // ps2x: hidden, never shown; or Vulkan presents\n    glfwSwapBuffers(platform.handle);" "buffer swap")
    file(CONFIGURE OUTPUT "${generated_dir}/platforms/rcore_desktop_glfw.c" CONTENT "${platform}" @ONLY)

    set(original "${raylib_dir}/rcore.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    file(READ "${original}" source)
    ps2x_guard_raylib_replace(source "    InitPlatform();" [=[    if (InitPlatform() != 0)
    {
        TRACELOG(LOG_WARNING, "PLATFORM: Initialization failed; GPU setup skipped");
        CORE.Window.ready = false;
        return;
    }]=] "InitPlatform call")
    ps2x_guard_raylib_replace(source [=[    #include "platforms/rcore_desktop_glfw.c"]=]
        "    #include \"${generated_dir}/platforms/rcore_desktop_glfw.c\" // ps2x: the guarded copy" "GLFW platform include")
    set(generated "${generated_dir}/rcore.c")
    file(CONFIGURE OUTPUT "${generated}" CONTENT "${source}" @ONLY)
    get_target_property(sources raylib SOURCES)
    set(updated_sources)
    foreach(source_file IN LISTS sources)
        get_filename_component(name "${source_file}" NAME)
        if(name STREQUAL "rcore.c")
            list(APPEND updated_sources "${generated}")
        elseif(IS_ABSOLUTE "${source_file}" OR source_file MATCHES "^\\$<")
            list(APPEND updated_sources "${source_file}")
        else()
            list(APPEND updated_sources "${raylib_dir}/${source_file}")
        endif()
    endforeach()
    set_property(TARGET raylib PROPERTY SOURCES "${updated_sources}")
endfunction()
