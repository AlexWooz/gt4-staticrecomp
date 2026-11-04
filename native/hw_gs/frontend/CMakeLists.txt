# HWR milestone M3: GS front end and CLUT unit (pure C++, no Vulkan).
# Self-contained: configure this directory as its own project for the M3 fixtures,
# or add_subdirectory() it from native/CMakeLists.txt (the integrator decides).
# Copyright 2026 GT4Recomp contributors. Licensed under the MIT License (see LICENSE).
# Written from the TASK-026 specification (docs/hw-renderer-spec), chapters 01.1 and 02.

cmake_minimum_required(VERSION 3.24)
project(hw_gs_m3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HW_GS_M3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(HW_GS_CLUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../clut")

add_library(hw_gs_m3 STATIC
    hw_gs_registers.cpp
    hw_gif_parser.cpp
    hw_transfers.cpp
    hw_batch.cpp
    hw_front_end.cpp
    "${HW_GS_CLUT_DIR}/hw_clut.cpp")
target_include_directories(hw_gs_m3 PUBLIC
    "${HW_GS_M3_ROOT}"
    "${HW_GS_CLUT_DIR}")
if(MSVC)
    target_compile_options(hw_gs_m3 PRIVATE /W4 /permissive-)
else()
    target_compile_options(hw_gs_m3 PRIVATE -Wall -Wextra)
endif()

enable_testing()

add_executable(hw_gs_m3_tests tests/hw_m3_tests.cpp)
target_link_libraries(hw_gs_m3_tests PRIVATE hw_gs_m3)
set_target_properties(hw_gs_m3_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

foreach(hw_m3_test IN ITEMS
        hw_gif_split hw_gif_ad_xyz hw_flush_rules hw_autoflush hw_vertex_trace hw_st_rounding
        hw_clut_load hw_clut_snapshot hw_clut_copy_free hw_transfers hw_counters)
    add_test(NAME ${hw_m3_test} COMMAND hw_gs_m3_tests ${hw_m3_test})
    set_tests_properties(${hw_m3_test} PROPERTIES TIMEOUT 120)
endforeach()
