# Native SPU2 voice engine, always compiled with optimization.
#
# The engine runs on the host sound thread and must render 48 kHz audio in real
# time, including in Debug builds (the GT3 test/runner tree is Debug). MSVC
# Debug (/Od /RTC1 plus checked iterators) measured ~0.5x real time, and MSVC
# silently ignores #pragma optimize under /RTC. So this directory-scoped object
# library drops /RTC and uses /O2 for its single source, while keeping the same
# runtime library and _DEBUG/_ITERATOR_DEBUG_LEVEL as the rest of the build.
# Debug information is kept.
if(MSVC)
    foreach(flags CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC[1csu]*" "" ${flags} "${${flags}}")
        string(REGEX REPLACE "/Od" "/O2" ${flags} "${${flags}}")
        string(REGEX REPLACE "/Ob0" "/Ob2" ${flags} "${${flags}}")
        string(REGEX REPLACE "/ZI" "/Zi" ${flags} "${${flags}}")
    endforeach()
endif()

add_library(ps2_iop_spu2_voices OBJECT ../modules/gt3_spu2_voices.cpp)
target_compile_features(ps2_iop_spu2_voices PUBLIC cxx_std_20)
target_include_directories(ps2_iop_spu2_voices PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(ps2_iop_spu2_voices PRIVATE
    PS2X_IOP_ENABLE_PLUGINS=$<BOOL:${PS2X_IOP_ENABLE_PLUGINS}>)
if(MSVC)
    # CMake >= 4.0 (policy CMP0184) models /RTC as a target property instead
    # of a flag; clear it too. Older CMake ignores this property.
    set_property(TARGET ps2_iop_spu2_voices PROPERTY MSVC_RUNTIME_CHECKS "")
else()
    target_compile_options(ps2_iop_spu2_voices PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
