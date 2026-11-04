# SPDX-License-Identifier: MIT
#
# HWR per-draw decisions (TASK-027, M5 to M11): pure C++20, no GPU. Standalone or as a
# subdirectory of the GT4Recomp tree. Written from docs/hw-renderer-spec.
cmake_minimum_required(VERSION 3.24)

if(NOT PROJECT_NAME)
    project(HwGsDecide VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

add_library(hw_gs_decide STATIC hw_gs_decide.cpp hw_gs_decide_extra.cpp)
target_include_directories(hw_gs_decide PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hw_gs_decide PUBLIC cxx_std_20)

# The pixel reference (native/hw_gs/pixel) is used only by the fixtures, as the exactness
# oracle for blend, fold and dither arithmetic. The decider itself does not depend on it.
add_library(hw_gs_decide_pixel_ref STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../pixel/hw_gs_pixel.cpp")
target_include_directories(hw_gs_decide_pixel_ref PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../pixel")
target_compile_features(hw_gs_decide_pixel_ref PUBLIC cxx_std_20)

add_executable(hw_gs_decide_fixture hw_gs_decide_fixture.cpp)
target_link_libraries(hw_gs_decide_fixture PRIVATE hw_gs_decide hw_gs_decide_pixel_ref)

foreach(hw_test
        hw_draw_rect_table hw_coverage_1x hw_blend_classes hw_blend_vectors hw_blend_levels
        hw_blend_simplify hw_colour_wrap hw_channel_writes hw_atest_fold hw_atest_methods
        hw_date_methods hw_date_vectors hw_barrier_rules hw_overlap_groups hw_vertex_trace
        hw_primitive_expansion hw_scanmsk hw_texture_shuffle hw_channel_shuffle hw_gt4_patterns
        hw_clear_vectors hw_pipeline_keys hw_draw_plan_smoke)
    add_test(NAME ${hw_test} COMMAND $<TARGET_FILE:hw_gs_decide_fixture> ${hw_test})
endforeach()
