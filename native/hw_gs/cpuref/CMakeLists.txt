# HWR CPU reference renderer (TASK-027). Standalone project that composes the sibling pure
# libraries by relative path (memory, clut, frontend, pixel, display). It does not touch
# native/CMakeLists.txt. Build:
#   cmake -S native/hw_gs/cpuref -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release
# Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
# Written from the TASK-026 specification (docs/hw-renderer-spec), chapters 02, 10.2, 12.9, 14.3.
cmake_minimum_required(VERSION 3.24)
project(HwGsCpuRef VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HW_GS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(hw_gs_cpuref STATIC
    cpuref_memory.cpp
    cpuref_draw.cpp
    cpuref_renderer.cpp
    cpuref_vram_dump.cpp
    cpuref_replay.cpp
    cpuref_replay_v2.cpp
    "${HW_GS_DIR}/memory/hw_memory.cpp"
    "${HW_GS_DIR}/clut/hw_clut.cpp"
    "${HW_GS_DIR}/frontend/hw_gs_registers.cpp"
    "${HW_GS_DIR}/frontend/hw_gif_parser.cpp"
    "${HW_GS_DIR}/frontend/hw_transfers.cpp"
    "${HW_GS_DIR}/frontend/hw_batch.cpp"
    "${HW_GS_DIR}/frontend/hw_front_end.cpp"
    "${HW_GS_DIR}/pixel/hw_gs_pixel.cpp"
    "${HW_GS_DIR}/display/hw_gs_display.cpp")
target_include_directories(hw_gs_cpuref PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${HW_GS_DIR}/memory"
    "${HW_GS_DIR}/clut"
    "${HW_GS_DIR}/frontend"
    "${HW_GS_DIR}/pixel"
    "${HW_GS_DIR}/display")
if(MSVC)
    target_compile_options(hw_gs_cpuref PRIVATE /W4 /permissive-)
else()
    target_compile_options(hw_gs_cpuref PRIVATE -Wall -Wextra)
endif()

add_executable(hw_cpuref_replay hw_cpuref_replay.cpp)
target_link_libraries(hw_cpuref_replay PRIVATE hw_gs_cpuref)

enable_testing()

add_executable(cpuref_tests tests/cpuref_tests.cpp)
target_link_libraries(cpuref_tests PRIVATE hw_gs_cpuref)

add_executable(cpuref_replay_v2_tests tests/cpuref_replay_v2_tests.cpp)
target_link_libraries(cpuref_replay_v2_tests PRIVATE hw_gs_cpuref)
foreach(t IN ITEMS parity resets fifo malformed)
    add_test(NAME cpuref_capture_v2_${t} COMMAND cpuref_replay_v2_tests ${t})
endforeach()

add_executable(cpuref_vram_dump_tests tests/cpuref_vram_dump_tests.cpp)
target_link_libraries(cpuref_vram_dump_tests PRIVATE hw_gs_cpuref)
foreach(t IN ITEMS selection formats offsets line_double independence metadata)
    add_test(NAME cpuref_vram_dump_${t} COMMAND cpuref_vram_dump_tests ${t})
endforeach()

# Existing milestone fixtures, built against the same sources so the composed libraries are
# covered by CTest too.
add_executable(hw_pixel_fixture "${HW_GS_DIR}/pixel/hw_gs_pixel_fixture.cpp")
target_link_libraries(hw_pixel_fixture PRIVATE hw_gs_cpuref)
add_executable(hw_display_fixture "${HW_GS_DIR}/display/hw_gs_display_fixture.cpp")
target_link_libraries(hw_display_fixture PRIVATE hw_gs_cpuref)
add_executable(hw_memory_tests "${HW_GS_DIR}/memory/hw_memory_tests.cpp")
target_link_libraries(hw_memory_tests PRIVATE hw_gs_cpuref)
add_executable(hw_m3_tests "${HW_GS_DIR}/frontend/tests/hw_m3_tests.cpp")
target_link_libraries(hw_m3_tests PRIVATE hw_gs_cpuref)

foreach(t IN ITEMS
        cpuref_coverage_vectors cpuref_line_pixels cpuref_triangle_shared_edge cpuref_flat_provoking
        cpuref_gouraud cpuref_constant_attributes cpuref_sprite_rectangle cpuref_scissor cpuref_depth cpuref_texture_nearest_decal
        cpuref_clut_indexed cpuref_blend_sweep cpuref_in07_aem cpuref_capture_replay cpuref_ppm cpuref_display_frame)
    add_test(NAME ${t} COMMAND cpuref_tests ${t})
endforeach()

foreach(t IN ITEMS hw_tfx_vectors hw_tfx_sweep hw_texa_vectors hw_texa_sweep hw_fog_vectors hw_fog_sweep
        hw_atest_fold hw_atest_vectors hw_blend_vectors hw_blend_modes hw_blend_sweep
        hw_colclamp_vectors hw_dither_vectors hw_frame_convert hw_fbmask_merge hw_date_vectors
        hw_depth_vectors hw_wrap_vectors hw_pipeline_vectors)
    add_test(NAME pixel_${t} COMMAND hw_pixel_fixture ${t})
endforeach()

foreach(t IN ITEMS hw_video_mode hw_pcrtc_rects hw_pcrtc_offsets hw_merge_plan hw_merge_compose
        hw_interlace_phase hw_deint_weave_blend hw_deint_bob hw_deint_adaptive
        hw_auto_deinterlace hw_shadeboost hw_scanlines hw_present_rect hw_display_options)
    add_test(NAME display_${t} COMMAND hw_display_fixture ${t})
endforeach()

foreach(t IN ITEMS hw_swizzle_matches_donor hw_roundtrip_psms hw_psm_layout hw_v01_3_t8_lanes
        hw_v01_4_z_unaligned hw_reinterp_maps hw_format_predicates hw_transfers hw_rect_matches_pixels)
    add_test(NAME memory_${t} COMMAND hw_memory_tests ${t})
endforeach()

foreach(t IN ITEMS hw_gif_split hw_gif_ad_xyz hw_flush_rules hw_autoflush hw_vertex_trace hw_st_rounding
        hw_clut_load hw_clut_snapshot hw_clut_copy_free hw_block_ranges_overlap hw_front_end_fast_paths
        hw_transfers
        hw_counters)
    add_test(NAME m3_${t} COMMAND hw_m3_tests ${t})
endforeach()
