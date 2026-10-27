cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_ENABLE_PLUGINS "Enable dynamic ps2xIOP plugins" OFF)

# Game selection (set at the top level): GT3 builds GT3's native IOP services; GT4 builds
# only the generic core plus src/modules/gt4_*.cpp (analysis/COMPATIBILITY.md).
if(NOT DEFINED PS2X_GAME)
    set(PS2X_GAME GT4)
endif()

set(PS2X_IOP_CORE_SOURCES
    src/iop_subsystem.cpp
    src/native_library.cpp
    src/builtin_profiles.cpp
    src/plugin_loader.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
    src/modules/tsnddrv.cpp
    src/modules/cri_dtx.cpp
    src/modules/clfile.cpp
    src/modules/sound_update_stub.cpp
    src/modules/sdrdrv.cpp
)

set(PS2X_IOP_GT3_SOURCES
    src/modules/gt3_sio2_transfer.cpp
    src/modules/gt3_sio2_native_exports.cpp
    src/modules/gt3_sio2_resident.cpp
    src/modules/gt3_sio2_queued_device.cpp
    src/modules/gt3_sio2_native_backend.cpp
    src/modules/gt3_sio2_digital_pad.cpp
    src/modules/gt3_mtap_resident.cpp
    src/modules/gt3_mcman_storage.cpp
    src/modules/gt3_mc_card.cpp
    src/modules/gt3_mcman_native_exports.cpp
    src/modules/gt3_mcman_startup_services.cpp
    src/modules/gt3_mcserv_resident.cpp
    src/modules/gt3_padman_resident.cpp
    src/modules/gt3_padman_publication.cpp
    src/modules/gt3_padman_port_state.cpp
    src/modules/gt3_padman_frame_input.cpp
    src/modules/gt3_libsd_resident.cpp
    src/modules/gt3_spu2_memory.cpp
    src/modules/gt3_spu2_startup_voices.cpp
    src/modules/gt3_pdispu2_movie_audio.cpp
    src/modules/gt3_spu2_events.cpp
    src/modules/gt3_libsd_cold_startup.cpp
    src/modules/gt3_usbd_storage.cpp
    src/modules/gt3_usbd_ohci.cpp
    src/modules/gt3_usbd_workers.cpp
    src/modules/gt3_usbd_resident.cpp
    src/modules/gt3_ilink_resident.cpp
    src/modules/gt3_ilink_packet_pool.cpp
    src/modules/gt3_ilink_timer_pool.cpp
    src/modules/gt3_ilink_startup_image.cpp
    src/modules/gt3_libpdi_resident.cpp
    src/modules/gt3_pdicdvd_resident.cpp
    src/modules/gt3_pdispu2_sound_setup.cpp
    src/modules/gt3_pdispu2_workers.cpp
    src/modules/gt3_pdispu2_resident.cpp
    src/modules/gt3_pdispu2_stream_control.cpp
    src/modules/gt3_pdistr_startup_image.cpp
    src/modules/gt3_pdistr_workers.cpp
    src/modules/gt3_pdistr_resident.cpp
    src/modules/gt3_pdistr_disc_stream.cpp
    src/modules/gt3_pdistr_mpeg_packet.cpp
    src/modules/gt3_pdistr_mpeg_ring.cpp
    src/modules/gt3_pdistr_mpeg_demux.cpp
    src/modules/gt3_pdistr_mpeg_source.cpp
    src/modules/gt3_pdistr_mpeg_video_job.cpp
    src/modules/gt3_pdistr_bgm_job.cpp
    src/modules/gt3_pdistr_mpeg_audio.cpp
    src/modules/gt3_pdi1394_startup.cpp
    src/modules/gt3_pdi1394_resident.cpp
    src/modules/gt3_pdi1394_link.cpp
    src/modules/gt3_link_transport.cpp
    src/modules/gt3_pdiusb_resident.cpp
    src/modules/gt3_loadfile.cpp
    src/modules/gt3_cdvd.cpp
    src/modules/gt3_padman.cpp
)

file(GLOB PS2X_IOP_GT4_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/gt4_*.cpp")

if(PS2X_GAME STREQUAL "GT3")
    add_library(ps2_iop STATIC ${PS2X_IOP_CORE_SOURCES} ${PS2X_IOP_GT3_SOURCES})
    target_compile_definitions(ps2_iop PUBLIC PS2X_GAME_GT3=1)
else()
    add_library(ps2_iop STATIC ${PS2X_IOP_CORE_SOURCES} ${PS2X_IOP_GT4_SOURCES})
    target_compile_definitions(ps2_iop PUBLIC PS2X_GAME_GT4=1)
endif()
# GT4_STOCK_BUILD (the stock package; a -D cache option, declared in ps2xRuntime): the test-only
# GT4_CHEATS_TEST_NODAY is left out of the script overlay (gt4_cheat_overlay.cpp; analysis/cheats).
if(GT4_STOCK_BUILD AND NOT PS2X_GAME STREQUAL "GT3")
    target_compile_definitions(ps2_iop PRIVATE GT4_STOCK_BUILD=1)
endif()

target_compile_features(ps2_iop PUBLIC cxx_std_20)

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(ps2x::iop ALIAS ps2_iop)

# SPU2 voice engine: optimized even in Debug (real-time host sound thread).
# GT3 only until a GT4 service uses it and COMPATIBILITY.md records a per-file verdict.
if(PS2X_GAME STREQUAL "GT3")
    add_subdirectory(src/spu2_optimized)
    target_sources(ps2_iop PRIVATE $<TARGET_OBJECTS:ps2_iop_spu2_voices>)
endif()

target_compile_definitions(ps2_iop PUBLIC
    PS2X_IOP_ENABLE_PLUGINS=$<BOOL:${PS2X_IOP_ENABLE_PLUGINS}>
)

# i.LINK battle over UDP (gt3_link_transport.cpp).
if(WIN32 AND PS2X_GAME STREQUAL "GT3")
    target_link_libraries(ps2_iop PUBLIC ws2_32)
endif()

# GT4 LAN network over Winsock (gt4_net.cpp; adapter lookup for GT4_NET=lan).
if(WIN32 AND NOT PS2X_GAME STREQUAL "GT3")
    target_link_libraries(ps2_iop PUBLIC ws2_32 iphlpapi)
endif()

if(PS2X_IOP_ENABLE_PLUGINS AND UNIX AND NOT APPLE)
    target_link_libraries(ps2_iop PRIVATE ${CMAKE_DL_LIBS})
endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
