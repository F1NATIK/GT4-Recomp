cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" OFF)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    if(NOT PS2X_IS_ANDROID)
        # The GPU translation renderer (GSGlBackend) needs OpenGL 4.3.
        set(OPENGL_VERSION "4.3" CACHE STRING "" FORCE)
    endif()
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)
    if(WIN32)
        include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RaylibInitGuard.cmake")
        ps2x_guard_raylib_init()
    endif()

    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})
set(PS2X_FFMPEG_ROOT "" CACHE PATH "Existing Windows FFmpeg SDK (include and bin directories)")

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; native MPEG video decode is unavailable")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    if(PS2X_FFMPEG_ROOT)
        set(FFMPEG_SOURCE_DIR "${PS2X_FFMPEG_ROOT}")
        if(NOT EXISTS "${FFMPEG_SOURCE_DIR}/include/libavcodec/avcodec.h" OR
           NOT EXISTS "${FFMPEG_SOURCE_DIR}/bin/avcodec.lib")
            message(FATAL_ERROR "PS2X_FFMPEG_ROOT must contain FFmpeg headers and Windows import libraries")
        endif()
    endif()
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    if(NOT PS2X_FFMPEG_ROOT)
    file(MAKE_DIRECTORY "${FFMPEG_INCLUDE_DIR}")
    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        URL_HASH SHA256=b985887dbca8c0a9a4c47f9d2bac7ec3ab994335b1b7fe7000bdb1dcd772ac68
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )
    endif()

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    if(TARGET ffmpeg_external)
        foreach(component avcodec avformat avutil swresample swscale)
            add_dependencies(ffmpeg_${component} ffmpeg_external)
        endforeach()
    endif()

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32 OR NOT PS2X_ENABLE_FFMPEG)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

if(NOT DEFINED PS2X_GAME)
    set(PS2X_GAME GT4)
endif()
if(PS2X_GAME STREQUAL "GT3")
    add_library(ps2_mpeg_video STATIC src/lib/mpeg_video_decoder.cpp src/lib/mpeg_video_packet.cpp
        src/lib/gt3_movie_layout.cpp src/lib/gt3_movie_queue.cpp)
else()
    add_library(ps2_mpeg_video STATIC src/lib/mpeg_video_decoder.cpp src/lib/mpeg_video_packet.cpp)
endif()
target_include_directories(ps2_mpeg_video PUBLIC include)
target_compile_features(ps2_mpeg_video PUBLIC cxx_std_20)
target_compile_definitions(ps2_mpeg_video PRIVATE PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>)
target_link_libraries(ps2_mpeg_video PRIVATE ffmpeg)

set(PS2X_RUNTIME_CORE_SOURCES
    src/lib/crash_report.cpp
    src/lib/ipu_idec.cpp
    src/lib/ps2_ipu.cpp
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_worker.cpp
    src/lib/gs/gs_async_backend.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_iop_sha256.cpp
    src/lib/ps2_sound_output.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_pad_rumble.cpp
    src/lib/ps2_input_profile.cpp
    src/lib/ps2_runtime.cpp
    src/lib/present_scanline.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vif1_worker.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/games_database.cpp
)

# GT3 game hooks and mod features: GT3 builds only (GT4 reuse policy, COMPATIBILITY.md).
set(PS2X_RUNTIME_GT3_SOURCES
    src/lib/gt3_movie_guest_output.cpp
    src/lib/gt3_movie_session.cpp
    src/lib/gt3_movie_worker.cpp
    src/lib/gt3_ipu_image.cpp
    src/lib/gt3_motion.cpp
    src/lib/gt3_aspect.cpp
    src/lib/gt3_phone_view.cpp
    src/lib/gt3_phone_capture.cpp
    src/lib/gt3_recorder.cpp
    src/lib/gt3_radar.cpp
    src/lib/gt3_hud.cpp
    src/lib/gt3_camera.cpp
    src/lib/gt3_race_info.cpp
    src/lib/gt3_autopilot.cpp
    src/lib/gt3_texture_dump.cpp
    src/lib/gt3_texture_replace.cpp
    src/lib/gt3_course_patch.cpp
    src/lib/gt3_disc_patch.cpp
    src/lib/gt3_drive_log.cpp
    src/lib/gt3_ai_pilot.cpp
    src/lib/gt3_start_lights.cpp
    src/lib/gt3_race_report.cpp
)

if(PS2X_GAME STREQUAL "GT3")
    add_library(ps2_runtime STATIC ${PS2X_RUNTIME_CORE_SOURCES} ${PS2X_RUNTIME_GT3_SOURCES})
else()
    add_library(ps2_runtime STATIC ${PS2X_RUNTIME_CORE_SOURCES})
endif()
if(NOT PS2X_GAME STREQUAL "GT3")
    # GT4: the in-game cheat card (GT4_CHEAT_CARD, gt4_cheat_card.cpp; analysis/cheats).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_cheat_card.cpp)
    # GT4: the FPS counter (GT4_FPS_COUNTER, F3, gt4_fps_counter.cpp; the Classic edition never shows it).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_fps_counter.cpp)
    # GT4: our own race HUD widgets and start lights (GT4_HUD_*, GT4_START_*, gt4_hud.cpp; never in the Classic edition).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_hud.cpp)
    # GT4: the race reader (read-only race state for the HUD / telemetry, gt4_race_reader.cpp; off in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_race_reader.cpp src/lib/gt4_drive_log.cpp) # (+ the Race Lab recorder, GT4_DRIVE_LOG)
target_sources(ps2_runtime PRIVATE src/lib/gt4_ai_drivers.cpp) # (the Race Lab's AI cars' drivers' names, GT4_AI_DRIVERS)
    # GT4: holding the start (GT4_START_HOLD, gt4_start_hold.cpp; HUD hand-off 7 E; empty in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_start_hold.cpp)
    # GT4: the pages service (HTTP + live feed of the race reader for the telemetry window / phone pages, gt4_pages.cpp).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_pages.cpp)
    # GT4: the rear-view mirror on a page (GET /mirror.mjpg from the pages service, gt4_mirror.cpp; HUD hand-off 6 B).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_mirror.cpp)
    # GT4: the launcher's race pictures (hud-pictures folder, gt4_hud_pictures.cpp; HUD hand-off 7 A1; stubs in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_hud_pictures.cpp)
    # GT4: GT4's own race HUD parts moved and resized (GT4_HUD_MOVE, gt4_hud_move.cpp; HUD hand-off 7 B; stubs in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_hud_move.cpp)
    # GT4: the VU1 tap (per-vertex normals / positions for real lighting and motion vectors, gt4_vu1_tap.cpp; off by default).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_vu1_tap.cpp)
    # GT4: the movie stutter fix (the movie feed copy done natively, gt4_movie_feed.cpp; both editions; analysis/design/movie-stutter).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_movie_feed.cpp)
    if(WIN32)
        target_link_libraries(ps2_runtime PRIVATE ws2_32)
    endif()
    # GT4: the texture dumper (GT4_TEXTURE_DUMP=<folder>, gt4_texture_dump.cpp; empty in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_texture_dump.cpp)
    # GT4: the dump's car and course folders (gt4_texture_owner.cpp; stubs in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_texture_owner.cpp)
    # GT4: texture replacement (GT4_TEXTURE_REPLACE=<folder>, gt4_texture_replace.cpp; empty in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_texture_replace.cpp)
    # GT4: the detailed FPS counter's PC numbers (CPU / GPU load, RAM, VRAM: gt4_fps_stats.cpp; Windows PDH and DXGI).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_fps_stats.cpp)
    # GT4: Multi Monitor Mode's shared controller 1 (GT4_MM_PAD, gt4_multimon_pad.cpp; empty in the Classic build).
    target_sources(ps2_runtime PRIVATE src/lib/gt4_multimon_pad.cpp)
endif()

# GT3_STOCK_BUILD=ON (a separate build folder, e.g. out/gt3-msvc-stock): the
# game as on a PS2. The additions' sources are left out of the build
# entirely - HUD boxes, radar, extra cameras and the widescreen/triple
# picture, race info, self-driving, phone view, video recorder, motion data;
# the upscalers, frame generation and Vulkan are not built; the runtime
# skips their calls and every hotkey but F1 (the TV filter) - and the
# picture is always the PS2's own resolution and interlaced fields. GT3's
# own Screen Size 16:9 option still shows at 16:9 (gt3_aspect.cpp, trimmed).
option(GT3_STOCK_BUILD "Stock GT3: the game as on a PS2, no additions compiled in, no hotkeys but F1" OFF)
if(GT3_STOCK_BUILD AND NOT PS2X_GAME STREQUAL "GT3")
    message(FATAL_ERROR "GT3_STOCK_BUILD is a GT3 option; GT4 gets its own stock build (PLAN phase 6)")
endif()
if(GT3_STOCK_BUILD)
    set_source_files_properties(
        src/lib/gt3_motion.cpp
        src/lib/gt3_phone_view.cpp
        src/lib/gt3_phone_capture.cpp
        src/lib/gt3_recorder.cpp
        src/lib/gt3_radar.cpp
        src/lib/gt3_hud.cpp
        src/lib/gt3_camera.cpp
        src/lib/gt3_race_info.cpp
        src/lib/gt3_autopilot.cpp
        src/lib/gt3_texture_dump.cpp
        src/lib/gt3_texture_replace.cpp
        src/lib/gt3_course_patch.cpp
        src/lib/gt3_disc_patch.cpp
        src/lib/gt3_drive_log.cpp
        src/lib/gt3_ai_pilot.cpp
        src/lib/gt3_start_lights.cpp
        src/lib/gt3_race_report.cpp
        PROPERTIES HEADER_FILE_ONLY ON)
    target_compile_definitions(ps2_runtime PUBLIC GT3_STOCK_BUILD=1)
endif()

# GT4_STOCK_BUILD=ON (a separate build folder, e.g. out/gt4-msvc-stock; PLAN
# phase 6): the GT4-Stock package. GT4 builds already leave out every GT3
# addition; this one also fixes the picture whatever the environment or
# gt4-start.env says - the OpenGL renderer at the PS2's own resolution,
# adaptive interlaced fields, 4:3, the scanline pass as the only filter
# (F1; present_scanline.cpp) - and its gt4-start.env takes stock settings only.
option(GT4_STOCK_BUILD "Stock GT4: native resolution, 4:3, interlaced fields, only the scanline filter" OFF)
if(GT4_STOCK_BUILD AND PS2X_GAME STREQUAL "GT3")
    message(FATAL_ERROR "GT4_STOCK_BUILD is a GT4 option; GT3 has GT3_STOCK_BUILD")
endif()
if(GT4_STOCK_BUILD)
    target_compile_definitions(ps2_runtime PUBLIC GT4_STOCK_BUILD=1)
endif()
if(WIN32 AND NOT PS2X_GAME STREQUAL "GT3" AND NOT GT4_STOCK_BUILD)
    # The texture dump's car and course names (gt4_texture_owner.cpp): the launcher's read-only disc and name readers.
    target_sources(ps2_runtime PRIVATE
        src/launcher/gt4stock/gt4stock_names.cpp
        src/launcher/gt4stock/gt4stock_util.cpp
        src/launcher/gt4stock/savelib/gt4save.cpp
        src/launcher/gt4stock/savelib/gt4save_disc.cpp)
    target_include_directories(ps2_runtime PRIVATE src/launcher/gt4stock/savelib)
endif()

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)
# Inlining pays off in the VU1 interpreter (small per-lane helpers).
if(MSVC)
    target_compile_options(ps2_runtime PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
endif()
# GT4_FAST_CPU (default ON, GT4 builds): the emulation's code (this library, with the translated VU1
# microprograms, and GT4's generated game code) without buffer security checks (/GS-: a cookie check in
# every function with a local array, e.g. each translated VU1 block), with full inlining in the game code
# too (/Ob2) and global data in COMDATs (/Gw). Code generation only: no floating-point or result change
# (no /fp:fast). For the i7-9700K main PC, where the emulation threads, not the GPU, limit GT4's races.
option(GT4_FAST_CPU "Emulation code without /GS checks, fully inlined (RelWithDebInfo)" ON)
if(MSVC AND GT4_FAST_CPU AND NOT PS2X_GAME STREQUAL "GT3")
    target_compile_options(ps2_runtime PRIVATE $<$<CONFIG:RelWithDebInfo>:/GS- /Gw>)
endif()


file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

# VU1 microprograms translated ahead of time (gt3-tools/vu1_translate.py); the
# directory always holds vu1_images.cpp (the registry, possibly empty).
# GT4 keeps its own (initially empty) registry; GT3's translated programs are not GT4's.
if(PS2X_GAME STREQUAL "GT3")
    set(PS2X_VU_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/generated")
else()
    # Made from your own disc (tools/vu1_translate.py), never committed: see docs/BUILDING.md.
    set(PS2X_VU_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/generated_gt4" CACHE PATH "Translated GT4 VU microprograms")
    if(NOT EXISTS "${PS2X_VU_GENERATED_DIR}/vu1_images.cpp")
        message(FATAL_ERROR "No translated VU microprograms at ${PS2X_VU_GENERATED_DIR}: make them from your own disc (docs/BUILDING.md) or set -DPS2X_VU_GENERATED_DIR")
    endif()
endif()
file(GLOB VU1_TRANSLATED_SRC_FILES CONFIGURE_DEPENDS
    "${PS2X_VU_GENERATED_DIR}/*.cpp"
)
target_sources(ps2_runtime PRIVATE
    src/lib/vu/vu1_translated_runtime.cpp
    ${VU1_TRANSLATED_SRC_FILES}
)
# Experiment (GT3_VU1_AVX2): the translated microprograms with AVX2 encoding
# (VEX, no FMA contraction under /fp:precise, so results stay bit-exact).
option(GT3_VU1_AVX2 "Compile the translated VU1 microprograms with /arch:AVX2" OFF)
if(MSVC AND GT3_VU1_AVX2)
    set_source_files_properties(${VU1_TRANSLATED_SRC_FILES} src/lib/vu/vu1_translated_runtime.cpp
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:precise")
endif()

# Generated game code. GT3: src/runner + src/runner_core (merge_generated.py). GT4: compiled in
# place from ps2_recomp's output folder (analysis/core-generated), no copy.
set(PS2X_GENERATED_DIR "" CACHE PATH "Folder with ps2_recomp output for the selected game (GT4)")
if(PS2X_GAME STREQUAL "GT3")
    file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runner_core/*.cpp"
    )
    set(PS2X_GENERATED_HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
else()
    if(NOT PS2X_GENERATED_DIR OR NOT EXISTS "${PS2X_GENERATED_DIR}/register_functions.cpp")
        message(FATAL_ERROR "PS2X_GAME=GT4 needs -DPS2X_GENERATED_DIR=<ps2_recomp output with register_functions.cpp>")
    endif()
    file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS "${PS2X_GENERATED_DIR}/*.cpp")
    set(PS2X_GENERATED_HEADER_DIR "${PS2X_GENERATED_DIR}")
endif()

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

if(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
endif()

# GT4's largest generated functions (e.g. gt4f_00141b90, 34k lines) need native
# stack frames far beyond the 1 MiB default; the EE main thread runs on the host
# main thread. Reserve 256 MiB (address space only; committed on use).
if(MSVC AND PS2X_GAME STREQUAL "GT4")
    target_link_options(ps2EntryRunner PRIVATE "/STACK:268435456")
endif()

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime_macros.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${PS2X_GENERATED_HEADER_DIR}/ps2_recompiled_functions.h"
        "${PS2X_GENERATED_HEADER_DIR}/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_syscalls.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_stubs.h"
    )

    target_precompile_headers(ps2EntryRunner PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()
if(NOT PS2X_GAME STREQUAL "GT3")
    target_include_directories(ps2EntryRunner BEFORE PRIVATE "${PS2X_GENERATED_DIR}")
    # GT4's pointer-only CORE functions that the generated set lacks (analysis/design/multi-monitor,
    # make_late_functions.py): entered in the dispatch table only when GT4_NET is on (Gt4HostGlue.cpp).
    # Made from your own disc (tools/make_late_functions.py), never committed: see docs/BUILDING.md.
    set(PS2X_LATE_FUNCTIONS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/gt4_late/gt4_late_functions.cpp" CACHE FILEPATH "GT4 pointer-only functions")
    if(NOT EXISTS "${PS2X_LATE_FUNCTIONS_CPP}")
        message(FATAL_ERROR "No ${PS2X_LATE_FUNCTIONS_CPP}: make it from your own disc (docs/BUILDING.md) or set -DPS2X_LATE_FUNCTIONS_CPP")
    endif()
    target_sources(ps2EntryRunner PRIVATE "${PS2X_LATE_FUNCTIONS_CPP}")
    set_source_files_properties("${PS2X_LATE_FUNCTIONS_CPP}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME "GT4")
    if(MSVC)
        # 27k generated functions: large objects; no per-function debug info needed.
        target_compile_options(ps2EntryRunner PRIVATE /bigobj)
        if(GT4_FAST_CPU)
            target_compile_options(ps2EntryRunner PRIVATE $<$<CONFIG:RelWithDebInfo>:/GS- /Ob2 /Gw>)
        endif()
    endif()
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop ps2_mpeg_video)
if(WIN32)
    target_link_libraries(ps2_runtime PRIVATE bcrypt)
    if(MINGW)
        # (MSVC takes these from the sources' #pragma comment(lib, ...); llvm-mingw's linker does not)
        target_link_libraries(ps2_runtime PUBLIC dbghelp pdh dxgi)
    endif()
    target_sources(ps2EntryRunner PRIVATE src/windows_gpu_preference.cpp)
endif()
if(WIN32 AND TARGET glfw)
    target_link_libraries(ps2_runtime PRIVATE glfw)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_INPUT_GLFW=1)
    # GPU translation renderer (GT3_RENDERER=gl); needs raylib's OpenGL 4.3 context.
    target_sources(ps2_runtime PRIVATE src/lib/gs/gs_gl_context.cpp src/lib/gs/gs_gl_backend.cpp src/lib/gs/gs_gl_present.cpp)
    target_compile_definitions(ps2_runtime PRIVATE PS2X_GS_GL=1)
    # Vulkan renderer, work in progress (gt3-analysis/VULKAN-RENDERER-PLAN.md):
    # Khronos headers, volk (loads vulkan-1.dll at run time: no SDK needed) and
    # glslang (the GS shaders are generated at run time, compiled to SPIR-V).
    option(GT3_VULKAN "Build the Vulkan renderer (work in progress)" OFF)
    if(GT3_VULKAN AND (GT3_STOCK_BUILD OR GT4_STOCK_BUILD))
        message(FATAL_ERROR "The stock builds are the OpenGL renderer only: configure them with -DGT3_VULKAN=OFF")
    endif()
    if(GT3_VULKAN)
        include(FetchContent)
        FetchContent_Declare(vulkan_headers
            GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
            GIT_TAG vulkan-sdk-1.4.363.0
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(vulkan_headers)
        FetchContent_Declare(volk
            GIT_REPOSITORY https://github.com/zeux/volk.git
            GIT_TAG vulkan-sdk-1.4.363.0
            GIT_SHALLOW TRUE)
        FetchContent_GetProperties(volk)
        if(NOT volk_POPULATED)
            FetchContent_Populate(volk)
        endif()
        add_library(gt3_volk STATIC "${volk_SOURCE_DIR}/volk.c")
        target_include_directories(gt3_volk PUBLIC "${volk_SOURCE_DIR}")
        target_link_libraries(gt3_volk PUBLIC Vulkan::Headers)
        target_compile_definitions(gt3_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
        set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
        set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glslang
            GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
            GIT_TAG vulkan-sdk-1.4.363.0
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glslang)
        # gs_vk_backend.cpp compiles gs_gl_backend.cpp a second time against
        # vkgl (the OpenGL subset on Vulkan, gs_vkgl.cpp).
        target_sources(ps2_runtime PRIVATE src/lib/gs/gs_vk_context.cpp src/lib/gs/gs_vkgl.cpp src/lib/gs/gs_vk_backend.cpp src/lib/gs/gs_vk_present.cpp)
        target_link_libraries(ps2_runtime PRIVATE gt3_volk glslang glslang-default-resource-limits)
        target_compile_definitions(ps2_runtime PRIVATE PS2X_GS_VK=1)
        # GT4 Full: DLSS through NVIDIA Streamline (GT4_UPSCALER=dlss, gt4_upscale.cpp): sl.interposer.dll is loaded at
        # run time from beside the game; only the SDK's headers are needed here (sdk\streamline, not in git).
        if(NOT PS2X_GAME STREQUAL "GT3" AND NOT GT4_STOCK_BUILD)
            set(GT4_STREAMLINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/streamline/x" CACHE PATH "NVIDIA Streamline SDK (DLSS)")
            target_sources(ps2_runtime PRIVATE src/lib/gt4_upscale.cpp src/lib/gt4_upscale_fsr.cpp src/lib/gt4_upscale_fsr4.cpp)
            # GT4_UPSCALER=fsr4: AMD's newest upscaler through Direct3D 12 (gt4_upscale_fsr4.cpp; FSR 4 on Radeon RX 9000):
            # the FidelityFX SDK 2's headers for that file only (sdk\ffx-2.3.0, not in git); its DLLs load at run time.
            set(GT4_FFX2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/ffx-2.3.0/x/FidelityFX-SDK-2.3.0/Kits/FidelityFX" CACHE PATH "AMD FidelityFX SDK 2 (FSR 4)")
            if(EXISTS "${GT4_FFX2_DIR}/upscalers/include/ffx_upscale.h")
                set_source_files_properties(src/lib/gt4_upscale_fsr4.cpp PROPERTIES INCLUDE_DIRECTORIES "${GT4_FFX2_DIR}" COMPILE_DEFINITIONS GT4_HAVE_FFX2=1)
            else()
                message(STATUS "FidelityFX SDK 2 not found at ${GT4_FFX2_DIR}: the build has no FSR 4")
            endif()
            # GT4_UPSCALER=fsr: AMD FidelityFX FSR 3.1 (gt4_upscale_fsr.cpp): amd_fidelityfx_vk.dll loaded at run time from
            # beside the game; only the FidelityFX API's headers are needed here (sdk\ffx-1.1.4, not in git).
            set(GT4_FFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/ffx-1.1.4/x/ffx-api/include" CACHE PATH "AMD FidelityFX API headers (FSR 3.1)")
            if(EXISTS "${GT4_FFX_DIR}/ffx_api/ffx_upscale.h")
                target_include_directories(ps2_runtime PRIVATE "${GT4_FFX_DIR}")
                target_compile_definitions(ps2_runtime PRIVATE GT4_HAVE_FFX=1)
            else()
                message(STATUS "FidelityFX API headers not found at ${GT4_FFX_DIR}: the build has no FSR")
            endif()
            if(EXISTS "${GT4_STREAMLINE_DIR}/include/sl.h")
                target_include_directories(ps2_runtime PRIVATE "${GT4_STREAMLINE_DIR}/include")
                target_compile_definitions(ps2_runtime PRIVATE GT4_HAVE_STREAMLINE=1)
            else()
                message(STATUS "Streamline SDK not found at ${GT4_STREAMLINE_DIR}: the build has no DLSS")
            endif()
        endif()
    endif()
    # Display upscalers (GT3_DISPLAY_FILTER=fsr / nis): AMD FidelityFX FSR 1 and
    # NVIDIA Image Scaling shader sources (third_party, MIT), embedded as text.
    # (Not in the stock build, nor frame generation / motion that come with them.)
    # GT3 only for now: frame generation / motion data use gt3_motion.
    if(NOT GT3_STOCK_BUILD AND PS2X_GAME STREQUAL "GT3")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedText.cmake")
    ps2x_embed_text("${CMAKE_CURRENT_BINARY_DIR}/generated/upscale_shader_text.h"
        kFsrLicenseText "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1/license.txt"
        kFfxAText "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1/ffx_a.h"
        kFfxFsr1Text "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1/ffx_fsr1.h"
        kNisScalerText "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nis/NIS_Scaler.h")
    target_sources(ps2_runtime PRIVATE src/lib/present_upscale.cpp src/lib/present_motion.cpp)
    target_include_directories(ps2_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    target_compile_definitions(ps2_runtime PRIVATE PS2X_UPSCALERS=1)
    endif()
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

# GT3 launcher (Windows): a settings window that starts ps2EntryRunner. Its UI
# uses raygui, the single-header raylib UI that raylib's source ships with its
# examples (no extra download). GT3 builds only.
if(WIN32 AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_VITA AND PS2X_GAME STREQUAL "GT3")
    FetchContent_GetProperties(raylib SOURCE_DIR PS2X_RAYLIB_ROOT)
    if(PS2X_RAYLIB_ROOT AND EXISTS "${PS2X_RAYLIB_ROOT}/examples/shapes/raygui.h")
        add_executable(gt3Launcher WIN32
            src/launcher/gt3_launcher.cpp
            src/launcher/gt3_launcher_win32.cpp
        )
        target_include_directories(gt3Launcher PRIVATE "${PS2X_RAYLIB_ROOT}/examples/shapes")
        target_link_libraries(gt3Launcher PRIVATE raylib comdlg32 shell32 ws2_32 iphlpapi)
        # Wheels and pedal boxes on the Controls page: raw joysticks through GLFW.
        if(TARGET glfw)
            target_link_libraries(gt3Launcher PRIVATE glfw)
            target_compile_definitions(gt3Launcher PRIVATE PS2X_INPUT_GLFW=1)
        endif()
        if(MSVC)
            target_link_options(gt3Launcher PRIVATE "/ENTRY:mainCRTStartup")
            target_compile_options(gt3Launcher PRIVATE /wd4244 /wd4267 /wd4305)
        endif()
    else()
        message(STATUS "gt3Launcher: raygui.h not found in the raylib sources; launcher not built")
    endif()
    # GT Race Lab on this PC (development builds only; the launcher's Race Lab page starts it): a small web
    # server on 127.0.0.1 for the Race Lab page (GT-RACE-LAB\page\index.html, kept in the project), keeping
    # what the page saves as plain files in <game folder>\racelab-data.
    if(NOT GT3_STOCK_BUILD)
        add_executable(gt3RaceLab src/racelab/gt3_racelab.cpp)
        target_link_libraries(gt3RaceLab PRIVATE ws2_32 shell32)
    endif()
endif()

# GT4 Recompilation launcher, Classic edition (Windows, GT4 builds only): the stock build's
# display, controller (players 1 and 2), wheel, EyeToy camera and save settings, a read-only Save details view
# (names read from the player's disc at run time), the Cheats page (GT4's own button codes) and its Play button. It writes the stock
# start file gt4-start.env that GT4.exe reads (analysis/gt4-stock-launcher/README.md). Its own small program, GT4Recompilation.exe:
# the same guarded raylib as GT4.exe (a hidden window opens no gamepad), GLFW's joystick
# ids, DirectInput 8 and Media Foundation's camera list (both in the system; MF is loaded
# at run time); nothing of GT3's launcher.
if(WIN32 AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_VITA AND PS2X_GAME STREQUAL "GT4")
    set(GT4STOCK_DIR src/launcher/gt4stock)
    add_executable(gt4Recompilation WIN32
        ${GT4STOCK_DIR}/gt4stock_main.cpp
        ${GT4STOCK_DIR}/gt4stock_app.cpp
        ${GT4STOCK_DIR}/gt4stock_pages.cpp
        ${GT4STOCK_DIR}/gt4stock_ui.cpp
        ${GT4STOCK_DIR}/gt4stock_settings.cpp
        ${GT4STOCK_DIR}/gt4stock_devices.cpp
        ${GT4STOCK_DIR}/gt4stock_camera.cpp
        ${GT4STOCK_DIR}/gt4stock_saves.cpp
        ${GT4STOCK_DIR}/gt4stock_scanline.cpp
        ${GT4STOCK_DIR}/gt4stock_system.cpp
        ${GT4STOCK_DIR}/gt4stock_util.cpp
        ${GT4STOCK_DIR}/gt4stock_names.cpp
        ${GT4STOCK_DIR}/gt4stock_savedetails.cpp
        ${GT4STOCK_DIR}/gt4stock_details.cpp
        ${GT4STOCK_DIR}/gt4stock_doc.cpp
        ${GT4STOCK_DIR}/gt4stock_cheats.cpp
        ${GT4STOCK_DIR}/gt4stock_cheatspage.cpp
        ${GT4STOCK_DIR}/gt4stock_cheatsfiles.cpp
        ${GT4STOCK_DIR}/savelib/gt4save.cpp
        ${GT4STOCK_DIR}/savelib/gt4save_disc.cpp
        ${GT4STOCK_DIR}/gt4stock.rc
    )
    # The GT4 save reader (analysis/launcher-v3/savelib, read only: no round-trip code) for Save details.
    target_include_directories(gt4Recompilation PRIVATE ${GT4STOCK_DIR}/savelib)
    set_target_properties(gt4Recompilation PROPERTIES OUTPUT_NAME "GT4Recompilation")
    target_compile_features(gt4Recompilation PRIVATE cxx_std_20)
    target_link_libraries(gt4Recompilation PRIVATE raylib dinput8 dxguid ole32 comdlg32 shell32 advapi32)
    if(TARGET glfw)
        target_link_libraries(gt4Recompilation PRIVATE glfw)
        target_compile_definitions(gt4Recompilation PRIVATE PS2X_INPUT_GLFW=1)
    endif()
    set(GT4STOCK_COMMIT "" CACHE STRING "Source id shown on the GT4 Recompilation launcher's About page (empty: git's short HEAD)")
    set(gt4stock_commit "${GT4STOCK_COMMIT}")
    if(gt4stock_commit STREQUAL "")
        find_package(Git QUIET)
        if(GIT_FOUND)
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE gt4stock_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        if(gt4stock_commit STREQUAL "")
            set(gt4stock_commit "unknown")
        endif()
    endif()
    target_compile_definitions(gt4Recompilation PRIVATE GT4STOCK_COMMIT="${gt4stock_commit}")
    if(MSVC)
        target_link_options(gt4Recompilation PRIVATE "/ENTRY:mainCRTStartup")
        target_compile_options(gt4Recompilation PRIVATE /W4 /wd4244 /wd4267 /wd4305 /wd4100)
    endif()

    # GT4 Recompilation launcher, Full edition (analysis/design/full-launcher/FULL-LAUNCHER-PLAN.md 1.1): the same launcher
    # sources compiled with GT4L_EDITION_FULL=1, plus the Full edition's own pages (gt4full_*.cpp), as its own program,
    # GT4RecompilationFull.exe. The Classic target above is unchanged (the Classic package scripts look for
    # GT4Recompilation.exe only). GT4L_FULL_SHOW_UNBUILT (development builds): the controls GT4.exe has no side for yet are
    # shown greyed with a "Not built yet" line; off (release builds): they are hidden.
    option(GT4L_FULL_SHOW_UNBUILT "Full launcher: show what GT4.exe does not have yet, greyed (development builds)" ON)
    add_executable(gt4RecompilationFull WIN32
        ${GT4STOCK_DIR}/gt4stock_main.cpp
        ${GT4STOCK_DIR}/gt4stock_app.cpp
        ${GT4STOCK_DIR}/gt4stock_pages.cpp
        ${GT4STOCK_DIR}/gt4stock_ui.cpp
        ${GT4STOCK_DIR}/gt4stock_settings.cpp
        ${GT4STOCK_DIR}/gt4stock_devices.cpp
        ${GT4STOCK_DIR}/gt4stock_camera.cpp
        ${GT4STOCK_DIR}/gt4stock_saves.cpp
        ${GT4STOCK_DIR}/gt4stock_scanline.cpp
        ${GT4STOCK_DIR}/gt4stock_system.cpp
        ${GT4STOCK_DIR}/gt4stock_util.cpp
        ${GT4STOCK_DIR}/gt4stock_names.cpp
        ${GT4STOCK_DIR}/gt4stock_savedetails.cpp
        ${GT4STOCK_DIR}/gt4stock_details.cpp
        ${GT4STOCK_DIR}/gt4stock_doc.cpp
        ${GT4STOCK_DIR}/gt4stock_cheats.cpp
        ${GT4STOCK_DIR}/gt4stock_cheatspage.cpp
        ${GT4STOCK_DIR}/gt4stock_cheatsfiles.cpp
        ${GT4STOCK_DIR}/gt4full_app.cpp
        ${GT4STOCK_DIR}/gt4full_build.cpp
        ${GT4STOCK_DIR}/gt4full_display.cpp
        ${GT4STOCK_DIR}/gt4full_garage.cpp
        ${GT4STOCK_DIR}/gt4full_garageedit.cpp
        ${GT4STOCK_DIR}/gt4full_model.cpp
        ${GT4STOCK_DIR}/gt4full_pages.cpp
        ${GT4STOCK_DIR}/gt4full_racelab.cpp
        ${GT4STOCK_DIR}/gt4full_racehud.cpp
        ${GT4STOCK_DIR}/gt4full_racetiming.cpp
        ${GT4STOCK_DIR}/gt4full_saveedit.cpp
        ${GT4STOCK_DIR}/gt4full_settings.cpp
        ${GT4STOCK_DIR}/gt4full_shots.cpp
        ${GT4STOCK_DIR}/gt4full_sounds.cpp
        ${GT4STOCK_DIR}/gt4full_system.cpp
        ${GT4STOCK_DIR}/gt4full_telemetry.cpp
        ${GT4STOCK_DIR}/gt4full_telesys.cpp
        ${GT4STOCK_DIR}/gt4full_ui.cpp
        ${GT4STOCK_DIR}/savelib/gt4save.cpp
        ${GT4STOCK_DIR}/savelib/gt4save_disc.cpp
        # The save writer (the Full edition's Garage changes only; the Classic target never links it).
        ${GT4STOCK_DIR}/savelib/gt4save_edit.cpp
        ${GT4STOCK_DIR}/savelib/gt4save_roundtrip.cpp
        ${GT4STOCK_DIR}/gt4full.rc
    )
    # GT4_EXTRA_CARS: the pack reader of GT4.exe's disc layer (header-only ps2x/iop/gt4_extra_cars_pack.h), so the Garage and
    # Car Dealers read the same files GT4.exe serves (savelib/gt4save_disc.cpp). The Classic launcher never gets it.
    target_include_directories(gt4RecompilationFull PRIVATE ${GT4STOCK_DIR}/savelib ${CMAKE_CURRENT_SOURCE_DIR}/../ps2xIOP/include)
    set_target_properties(gt4RecompilationFull PROPERTIES OUTPUT_NAME "GT4RecompilationFull")
    target_compile_features(gt4RecompilationFull PRIVATE cxx_std_20)
    target_link_libraries(gt4RecompilationFull PRIVATE raylib dinput8 dxguid ole32 comdlg32 shell32 advapi32 iphlpapi ws2_32 winmm)
    if(TARGET glfw)
        target_link_libraries(gt4RecompilationFull PRIVATE glfw)
        target_compile_definitions(gt4RecompilationFull PRIVATE PS2X_INPUT_GLFW=1)
    endif()
    target_compile_definitions(gt4RecompilationFull PRIVATE GT4L_EDITION_FULL=1 GT4STOCK_COMMIT="${gt4stock_commit}")
    if(GT4L_FULL_SHOW_UNBUILT)
        target_compile_definitions(gt4RecompilationFull PRIVATE GT4L_SHOW_UNBUILT=1)
    endif()
    if(MSVC)
        target_link_options(gt4RecompilationFull PRIVATE "/ENTRY:mainCRTStartup")
        target_compile_options(gt4RecompilationFull PRIVATE /W4 /wd4244 /wd4267 /wd4305 /wd4100)
    endif()
    # GT Race Lab on this PC for GT4 (Race Lab hand-off 1; the Full launcher's Race Lab button starts it): the GT3 tree's
    # small web server, copied (src/racelab/gt4_racelab.cpp), for the Race Lab page and the laps GT4_DRIVE_LOG records.
    if(NOT GT4_STOCK_BUILD)
        add_executable(gt4RaceLab src/racelab/gt4_racelab.cpp)
        target_compile_features(gt4RaceLab PRIVATE cxx_std_20)
        target_link_libraries(gt4RaceLab PRIVATE ws2_32 shell32)
        set_target_properties(gt4RaceLab PROPERTIES OUTPUT_NAME "gt4RaceLab")
    endif()
endif()

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)

# Optional benchmarks: GS CPU backend (replays a recorded graphics trace) and
# VU1 interpreter (replays recorded microprogram kicks, checking the results).
option(PS2X_BUILD_GS_BENCH "Build the GS and VU1 replay benchmarks" OFF)
if(PS2X_BUILD_GS_BENCH)
    add_executable(gt3GsReplayBench "${CMAKE_SOURCE_DIR}/gt3-tools/gs_replay_bench.cpp")
    target_link_libraries(gt3GsReplayBench PRIVATE ps2_runtime)
    add_executable(gt3Vu1ReplayBench "${CMAKE_SOURCE_DIR}/gt3-tools/vu1_replay_bench.cpp")
    target_link_libraries(gt3Vu1ReplayBench PRIVATE ps2_runtime)
    # GS mipmapping LOD rules (runtime/gs/gs_mip.h, header only): no game, window or GPU.
    add_executable(gt3GsMipSelfTest "${CMAKE_SOURCE_DIR}/gt3-tools/gs_mip_selftest.cpp")
    target_include_directories(gt3GsMipSelfTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

# Optional CPU-only diagnostic; the interactive runner is unchanged.
option(PS2X_BUILD_BOOT_PROBE "Build the CPU-only GT3 boot diagnostic" OFF)
if(PS2X_BUILD_BOOT_PROBE)
    set(PS2X_PROBE_SOURCES ${RUNNER_SRC_FILES})
    list(REMOVE_ITEM PS2X_PROBE_SOURCES "${RUNNER_MAIN_CPP}" "${ROOT_MAIN_CPP}")
    add_executable(gt3BootProbe ${PS2X_PROBE_SOURCES}
        "${CMAKE_SOURCE_DIR}/gt3-tools/boot_probe.cpp")
    target_link_libraries(gt3BootProbe PRIVATE ps2_runtime)
    target_precompile_headers(gt3BootProbe PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
    set_target_properties(gt3BootProbe PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
endif()

option(PS2X_BUILD_IOP_BRIDGE_PROBE "Build isolated runtime IOP-memory/transfer tests" OFF)
if(PS2X_BUILD_IOP_BRIDGE_PROBE AND NOT PS2X_GAME STREQUAL "GT3")
    message(FATAL_ERROR "PS2X_BUILD_IOP_BRIDGE_PROBE tests GT3's IOP services and GT3 generated files")
endif()
if(PS2X_BUILD_IOP_BRIDGE_PROBE)
    if(PS2X_ENABLE_FFMPEG)
        add_executable(gt3MovieWorkerProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_worker_probe.cpp"
            src/runner_core/FUN_0025f5f8_0x25f5f8.cpp
            src/runner_core/sub_00260410_0x260410.cpp
            src/runner_core/sub_0025CEB8_0x25ceb8.cpp
            src/runner_core/FUN_0025cf50_0x25cf50.cpp
            src/runner_core/sub_0025D028_0x25d028.cpp
            src/runner_core/FUN_0025b300_0x25b300.cpp
            src/runner_core/sub_0025B3B0_0x25b3b0.cpp
            src/runner_core/FUN_0025b2a0_0x25b2a0.cpp
            src/runner_core/sub_0025B0C8_0x25b0c8.cpp
            src/runner_core/sub_0027EB84_0x27eb84.cpp
            src/runner_core/FUN_00269c10_0x269c10.cpp
            src/runner_core/sub_0025EF00_0x25ef00.cpp
            src/runner_core/sub_0025F820_0x25f820.cpp
            src/runner_core/sub_002604C8_0x2604c8.cpp
            src/runner_core/sub_001CD1E0_0x1cd1e0.cpp
            src/runner_core/sub_0025EC10_0x25ec10.cpp
            src/runner_core/sub_0025EC50_0x25ec50.cpp
            src/runner_core/sub_0025F840_0x25f840.cpp
            src/runner_core/sub_0025F870_0x25f870.cpp
            src/runner_core/sub_00258218_0x258218.cpp
            src/runner_core/sub_00276FD0_0x276fd0.cpp
            src/runner_core/sub_00260518_0x260518.cpp
            src/runner_core/FUN_0025f6e0_0x25f6e0.cpp
            src/runner_core/sub_00260998_0x260998.cpp
            src/runner_core/sub_00260660_0x260660.cpp
            src/runner_core/FUN_002606f0_0x2606f0.cpp
            src/runner_core/sub_0025ED78_0x25ed78.cpp
            src/runner_core/FUN_0025ed30_0x25ed30.cpp)
        target_link_libraries(gt3MovieWorkerProbe PRIVATE ps2_runtime ffmpeg)
        ps2x_stage_ffmpeg_runtime_dlls(gt3MovieWorkerProbe)
        add_executable(gt3MovieSessionProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_session_probe.cpp"
            src/runner_core/sub_00260748_0x260748.cpp
            src/runner_core/FUN_00260880_0x260880.cpp
            src/runner_core/sub_00260518_0x260518.cpp
            src/runner_core/sub_00260660_0x260660.cpp
            src/runner_core/FUN_002606f0_0x2606f0.cpp)
        if(WIN32 AND TARGET glfw)
            target_sources(gt3MovieSessionProbe PRIVATE src/windows_gpu_preference.cpp
                src/runner_core/sub_001CCEC0_0x1ccec0.cpp
                src/runner_core/sub_001CD048_0x1cd048.cpp
                src/runner_core/sub_001CD168_0x1cd168.cpp
                src/runner_core/sub_00258218_0x258218.cpp)
            target_compile_definitions(gt3MovieSessionProbe PRIVATE GT3_MOVIE_GPU_PROBE=1)
            target_link_libraries(gt3MovieSessionProbe PRIVATE ps2_host_backend)
        endif()
        target_link_libraries(gt3MovieSessionProbe PRIVATE ps2_runtime ffmpeg)
        ps2x_stage_ffmpeg_runtime_dlls(gt3MovieSessionProbe)
    endif()
    add_executable(gt3MovieGuestOutputProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_guest_output_probe.cpp"
        src/runner_core/FUN_002603b8_0x2603b8.cpp
        src/runner_core/sub_00260410_0x260410.cpp
        src/runner_core/FUN_00260458_0x260458.cpp
        src/runner_core/FUN_0025f890_0x25f890.cpp
        src/runner_core/sub_00260518_0x260518.cpp
        src/runner_core/sub_00260660_0x260660.cpp
        src/runner_core/FUN_002606f0_0x2606f0.cpp
        src/runner_core/sub_001CCEC0_0x1ccec0.cpp
        src/runner_core/sub_001CD048_0x1cd048.cpp)
    target_link_libraries(gt3MovieGuestOutputProbe PRIVATE ps2_runtime)
    ps2x_stage_ffmpeg_runtime_dlls(gt3MovieGuestOutputProbe)
    add_executable(gt3MovieQueueProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_queue_probe.cpp"
        src/runner_core/sub_002604C8_0x2604c8.cpp
        src/runner_core/sub_00260518_0x260518.cpp
        src/runner_core/sub_00260560_0x260560.cpp
        src/runner_core/FUN_002605e8_0x2605e8.cpp
        src/runner_core/sub_00260660_0x260660.cpp
        src/runner_core/FUN_002606f0_0x2606f0.cpp)
    target_link_libraries(gt3MovieQueueProbe PRIVATE ps2_runtime)
    ps2x_stage_ffmpeg_runtime_dlls(gt3MovieQueueProbe)
    add_executable(gt3MovieLayoutProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_layout_probe.cpp"
        src/runner_core/sub_001CCEC0_0x1ccec0.cpp
        src/runner_core/sub_001CD048_0x1cd048.cpp
        src/runner_core/sub_001CD168_0x1cd168.cpp
        src/runner_core/sub_00258218_0x258218.cpp)
    target_link_libraries(gt3MovieLayoutProbe PRIVATE ps2_runtime)
    ps2x_stage_ffmpeg_runtime_dlls(gt3MovieLayoutProbe)
    add_executable(gt3MpegVideoProbe "${CMAKE_SOURCE_DIR}/gt3-tools/mpeg_video_probe.cpp")
    target_link_libraries(gt3MpegVideoProbe PRIVATE ps2_mpeg_video ps2_iop ffmpeg)
    target_compile_definitions(gt3MpegVideoProbe PRIVATE PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>)
    ps2x_stage_ffmpeg_runtime_dlls(gt3MpegVideoProbe)
    add_executable(gt3MovieAudioProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_audio_probe.cpp")
    target_link_libraries(gt3MovieAudioProbe PRIVATE ps2_runtime)
    add_executable(gt3Spu2VoicesProbe "${CMAKE_SOURCE_DIR}/gt3-tools/spu2_voices_probe.cpp")
    target_link_libraries(gt3Spu2VoicesProbe PRIVATE ps2_iop)
    add_executable(gt3PdistrMpegPacketProbe "${CMAKE_SOURCE_DIR}/gt3-tools/pdistr_mpeg_packet_probe.cpp")
    target_link_libraries(gt3PdistrMpegPacketProbe PRIVATE ps2_iop)
    add_executable(gt3PadReplayProbe "${CMAKE_SOURCE_DIR}/gt3-tools/pad_replay_probe.cpp")
    target_include_directories(gt3PadReplayProbe PRIVATE include)
    add_executable(gt3IopBridgeProbe "${CMAKE_SOURCE_DIR}/gt3-tools/iop_bridge_probe.cpp")
    target_sources(gt3IopBridgeProbe PRIVATE
        src/runner_core/sub_00272090_0x272090.cpp
        src/runner_core/sub_00272140_0x272140.cpp)
    target_link_libraries(gt3IopBridgeProbe PRIVATE ps2_runtime)
    add_executable(gt3PadmanPublicationProbe "${CMAKE_SOURCE_DIR}/gt3-tools/padman_publication_probe.cpp")
    target_link_libraries(gt3PadmanPublicationProbe PRIVATE ps2_iop)
    add_executable(gt3PadmanDigitalStateProbe "${CMAKE_SOURCE_DIR}/gt3-tools/padman_digital_state_probe.cpp")
    target_link_libraries(gt3PadmanDigitalStateProbe PRIVATE ps2_iop)
    add_executable(gt3PadmanDs2Probe "${CMAKE_SOURCE_DIR}/gt3-tools/padman_ds2_probe.cpp")
    target_link_libraries(gt3PadmanDs2Probe PRIVATE ps2_iop)
    add_executable(gt3McCardProbe "${CMAKE_SOURCE_DIR}/gt3-tools/mc_card_probe.cpp")
    target_link_libraries(gt3McCardProbe PRIVATE ps2_iop)
    add_executable(gt3LibpdiStreamAllocatorProbe "${CMAKE_SOURCE_DIR}/gt3-tools/libpdi_stream_allocator_probe.cpp")
    target_link_libraries(gt3LibpdiStreamAllocatorProbe PRIVATE ps2_runtime)
    add_executable(gt3PdistrDiscStreamProbe "${CMAKE_SOURCE_DIR}/gt3-tools/pdistr_disc_stream_probe.cpp")
    target_link_libraries(gt3PdistrDiscStreamProbe PRIVATE ps2_runtime)
    add_executable(gt3PdistrMpegSourceProbe "${CMAKE_SOURCE_DIR}/gt3-tools/pdistr_mpeg_source_probe.cpp")
    target_link_libraries(gt3PdistrMpegSourceProbe PRIVATE ps2_runtime)
    add_executable(gt3EeCallbackStackProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/ee_callback_stack_probe.cpp"
        src/runner_core/FUN_00265808_0x265808.cpp)
    target_link_libraries(gt3EeCallbackStackProbe PRIVATE ps2_runtime)
    add_executable(gt3EeRpcResumeProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/ee_rpc_resume_probe.cpp"
        src/runner_core/sub_001019D0_0x1019d0.cpp
        src/runner_core/sub_001A9830_0x1a9830.cpp
        src/runner_core/sub_001B4550_0x1b4550.cpp
        src/runner_core/sub_001B6F78_0x1b6f78.cpp
        src/runner_core/sub_001C9700_0x1c9700.cpp
        src/runner_core/sub_001BFEC0_0x1bfec0.cpp
        src/runner_core/sub_002185A0_0x2185a0.cpp
        src/runner_core/sub_00219D80_0x219d80.cpp
        src/runner_core/sub_0021E750_0x21e750.cpp
        src/runner_core/sub_0022DCE8_0x22dce8.cpp
        src/runner_core/sub_0025B3B0_0x25b3b0.cpp
        src/runner_core/sub_00288B68_0x288b68.cpp)
    target_link_libraries(gt3EeRpcResumeProbe PRIVATE ps2_runtime)
    add_executable(gt3EeDecodeProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/ee_decode_probe.cpp"
        src/runner_core/sub_0026CBC8_0x26cbc8.cpp
        src/runner_core/FUN_0026d1d0_0x26d1d0.cpp
        src/runner_core/sub_0026D510_0x26d510.cpp)
    target_link_libraries(gt3EeDecodeProbe PRIVATE ps2_runtime)
    add_executable(gt3EmitLwuProbe "${CMAKE_SOURCE_DIR}/gt3-tools/emit_lwu_probe.cpp")
    target_link_libraries(gt3EmitLwuProbe PRIVATE ps2_recomp_lib)
    set(GT3_LWU_PROBE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/lwu_probe_generated.h")
    add_custom_command(OUTPUT "${GT3_LWU_PROBE_HEADER}"
        COMMAND $<TARGET_FILE:gt3EmitLwuProbe> "${GT3_LWU_PROBE_HEADER}"
        DEPENDS gt3EmitLwuProbe VERBATIM)
    add_executable(gt3LwuInstructionProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/lwu_instruction_probe.cpp" "${GT3_LWU_PROBE_HEADER}")
    target_include_directories(gt3LwuInstructionProbe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(gt3LwuInstructionProbe PRIVATE ps2_runtime)
    add_executable(gt3EmitVuMemoryProbe "${CMAKE_SOURCE_DIR}/gt3-tools/emit_vu_memory_probe.cpp")
    target_link_libraries(gt3EmitVuMemoryProbe PRIVATE ps2_recomp_lib)
    set(GT3_VU_MEMORY_PROBE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vu_memory_probe_generated.h")
    add_custom_command(OUTPUT "${GT3_VU_MEMORY_PROBE_HEADER}"
        COMMAND $<TARGET_FILE:gt3EmitVuMemoryProbe> "${GT3_VU_MEMORY_PROBE_HEADER}"
        DEPENDS gt3EmitVuMemoryProbe VERBATIM)
    add_executable(gt3VuMemoryInstructionProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/vu_memory_instruction_probe.cpp" "${GT3_VU_MEMORY_PROBE_HEADER}"
        src/runner_core/FUN_0024e670_0x24e670.cpp
        src/runner_core/FUN_0024e7f0_0x24e7f0.cpp)
    target_include_directories(gt3VuMemoryInstructionProbe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(gt3VuMemoryInstructionProbe PRIVATE ps2_runtime)
    add_executable(gt3EeThreadStartProbe
        "${CMAKE_SOURCE_DIR}/gt3-tools/ee_thread_start_probe.cpp"
        src/runner_core/FUN_00269c30_0x269c30.cpp
        src/runner_core/ps2__StartThread_0x285a80.cpp)
    target_link_libraries(gt3EeThreadStartProbe PRIVATE ps2_runtime)
    if(WIN32 AND TARGET glfw)
        add_executable(gt3MovieGraphicsProbe "${CMAKE_SOURCE_DIR}/gt3-tools/movie_graphics_probe.cpp"
            src/windows_gpu_preference.cpp)
        # Resolve raylib's CloseWindow before the identically named Win32 import.
        target_link_libraries(gt3MovieGraphicsProbe PRIVATE ps2_host_backend ps2_mpeg_video ps2_iop)
        ps2x_stage_ffmpeg_runtime_dlls(gt3MovieGraphicsProbe)
        add_executable(gt3GraphicsProbe "${CMAKE_SOURCE_DIR}/gt3-tools/graphics_probe.cpp"
            src/windows_gpu_preference.cpp)
        target_link_libraries(gt3GraphicsProbe PRIVATE ps2_host_backend glfw)
    endif()
endif()

option(PS2X_BUILD_INPUT_PROBE "Build interactive Windows input/calibration diagnostic" OFF)
if(PS2X_BUILD_INPUT_PROBE)
    if(NOT WIN32 OR NOT TARGET glfw)
        message(FATAL_ERROR "Input probe requires Windows GLFW backend")
    endif()
    add_executable(gt3InputProbe "${CMAKE_SOURCE_DIR}/gt3-tools/input_probe.cpp"
        src/lib/ps2_pad.cpp src/lib/ps2_pad_rumble.cpp src/lib/ps2_input_profile.cpp)
    target_include_directories(gt3InputProbe PRIVATE include)
    target_compile_definitions(gt3InputProbe PRIVATE PS2X_INPUT_GLFW=1)
    target_link_libraries(gt3InputProbe PRIVATE ps2_host_backend glfw)
endif()
