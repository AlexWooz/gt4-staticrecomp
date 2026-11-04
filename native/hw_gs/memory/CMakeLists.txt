# Standalone build of the HWR local memory model (TASK-027 M2).
# Build alone:  cmake -S native/hw_gs/memory -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release
# Included from the native tree it is a plain subdirectory with no extra options.
cmake_minimum_required(VERSION 3.24)
project(HwGsMemory LANGUAGES CXX)

add_library(hw_gs_memory STATIC hw_memory.cpp)
target_include_directories(hw_gs_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(hw_gs_memory PUBLIC cxx_std_20)
set_target_properties(hw_gs_memory PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()

add_executable(hw_gs_memory_tests hw_memory_tests.cpp)
target_link_libraries(hw_gs_memory_tests PRIVATE hw_gs_memory)
set_target_properties(hw_gs_memory_tests PROPERTIES CXX_EXTENSIONS OFF)

foreach(hw_test IN ITEMS
    hw_swizzle_matches_donor
    hw_roundtrip_psms
    hw_psm_layout
    hw_v01_3_t8_lanes
    hw_v01_4_z_unaligned
    hw_reinterp_maps
    hw_format_predicates
    hw_transfers
    hw_rect_matches_pixels)
  add_test(NAME ${hw_test} COMMAND hw_gs_memory_tests ${hw_test})
endforeach()
