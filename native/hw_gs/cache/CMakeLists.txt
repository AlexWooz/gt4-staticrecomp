# Standalone build of the HWR surface cache bookkeeping (TASK-027 M7).
# Build alone:  cmake -S native/hw_gs/cache -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release
# Depends on the M2 memory library sources (native/hw_gs/memory), compiled in
# directly so the cache builds without the memory library's own test target.
cmake_minimum_required(VERSION 3.24)
project(HwGsCache LANGUAGES CXX)

set(HW_GS_MEMORY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../memory)

add_library(hw_gs_cache STATIC
    ${HW_GS_MEMORY_DIR}/hw_memory.cpp
    hw_cache_geometry.cpp
    hw_cache_dirty.cpp
    hw_cache.cpp
    hw_cache_fake_gpu.cpp)
target_include_directories(hw_gs_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${HW_GS_MEMORY_DIR})
target_compile_features(hw_gs_cache PUBLIC cxx_std_20)
set_target_properties(hw_gs_cache PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()

add_executable(hw_gs_cache_tests hw_cache_tests.cpp)
target_link_libraries(hw_gs_cache_tests PRIVATE hw_gs_cache)
set_target_properties(hw_gs_cache_tests PROPERTIES CXX_EXTENSIONS OFF)

foreach(hw_test IN ITEMS
    hw_cache_geometry
    hw_clamp_regions
    hw_dirty_merge
    hw_ageing
    hw_cache_operations
    hw_v10_alpha_writeback
    hw_cache_sources_display_moves
    hw_cache_budget
    hw_cache_authority_fuzz)
  add_test(NAME ${hw_test} COMMAND hw_gs_cache_tests ${hw_test})
endforeach()
