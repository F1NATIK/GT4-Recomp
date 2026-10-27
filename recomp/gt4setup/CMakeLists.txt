# gt4setup: the "make the game code from your own disc" steps of the GT4 recomp
# (formerly Python scripts) as one standalone C++17 command-line tool.
# Standard library only; builds with MSVC and with clang / llvm-mingw.
cmake_minimum_required(VERSION 3.20)
project(gt4setup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(gt4setup
    src/main.cpp
    src/util.cpp
    src/sha256.cpp
    src/inflate.cpp
    src/extract_boot.cpp
    src/core_to_elf.cpp
    src/vu_decode.cpp
    src/vu_images.cpp
    src/vu_translate.cpp
    src/late_functions.cpp
    src/build.cpp
)

if(WIN32)
    # std::getenv / std::fopen family: standard C++, not the _s variants
    target_compile_definitions(gt4setup PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    target_compile_options(gt4setup PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # static runtime: the exe runs on a PC without the VC++ redistributable
    set_property(TARGET gt4setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(gt4setup PRIVATE -Wall -Wextra)
    if(WIN32)
        # wmain (Unicode command line) and a self-contained exe
        target_link_options(gt4setup PRIVATE -municode -static)
    endif()
endif()
