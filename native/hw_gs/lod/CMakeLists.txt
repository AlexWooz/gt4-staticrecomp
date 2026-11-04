# SPDX-License-Identifier: MIT
#
# HWR mipmap and LOD reference (TASK-027, M13). Standalone or as a subdirectory of the
# GT4Recomp tree. Written from docs/hw-renderer-spec.
cmake_minimum_required(VERSION 3.24)

if(NOT PROJECT_NAME)
    project(HwGsLod VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

add_library(hw_gs_lod STATIC hw_gs_lod.cpp)
target_include_directories(hw_gs_lod PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hw_gs_lod PUBLIC cxx_std_20)

add_executable(hw_gs_lod_fixture hw_gs_lod_fixture.cpp)
target_link_libraries(hw_gs_lod_fixture PRIVATE hw_gs_lod)

foreach(hw_test
        hw_tex1_decode hw_lod_formula hw_mip_base hw_mip_nearest hw_mip_filter
        hw_miptbp_layout hw_mip_sizes hw_mtba hw_mtba_rules hw_mip_scaling)
    add_test(NAME ${hw_test} COMMAND $<TARGET_FILE:hw_gs_lod_fixture> ${hw_test})
endforeach()
