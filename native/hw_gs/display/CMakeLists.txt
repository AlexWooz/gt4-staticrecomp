# SPDX-License-Identifier: MIT
#
# HWR display output reference (TASK-027, M6 and M15). Standalone or as a subdirectory of
# the GT4Recomp tree. Written from docs/hw-renderer-spec.
cmake_minimum_required(VERSION 3.24)

if(NOT PROJECT_NAME)
    project(HwGsDisplay VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

add_library(hw_gs_display STATIC hw_gs_display.cpp)
target_include_directories(hw_gs_display PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hw_gs_display PUBLIC cxx_std_20)

add_executable(hw_gs_display_fixture hw_gs_display_fixture.cpp)
target_link_libraries(hw_gs_display_fixture PRIVATE hw_gs_display)

foreach(hw_test
        hw_video_mode hw_pcrtc_rects hw_pcrtc_offsets hw_merge_plan hw_merge_compose
        hw_interlace_phase hw_deint_weave_blend hw_deint_bob hw_deint_adaptive
        hw_auto_deinterlace hw_shadeboost hw_scanlines hw_present_rect hw_display_options)
    add_test(NAME ${hw_test} COMMAND $<TARGET_FILE:hw_gs_display_fixture> ${hw_test})
endforeach()
