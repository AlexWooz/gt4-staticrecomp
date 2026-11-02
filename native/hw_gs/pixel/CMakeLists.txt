# HWR pixel reference (TASK-027, M5). Self-contained: builds standalone or as a
# subdirectory of the GT4Recomp tree. Written from docs/hw-renderer-spec.
cmake_minimum_required(VERSION 3.24)

if(NOT PROJECT_NAME)
    project(HwGsPixel VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

add_library(hw_gs_pixel STATIC hw_gs_pixel.cpp)
target_include_directories(hw_gs_pixel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(hw_gs_pixel PUBLIC cxx_std_20)

add_executable(hw_gs_pixel_fixture hw_gs_pixel_fixture.cpp)
target_link_libraries(hw_gs_pixel_fixture PRIVATE hw_gs_pixel)

# One CTest case per fixture group, named as in the specification (chapter 14.7 and
# the M5 checks of chapter 15).
foreach(hw_test
        hw_tfx_vectors hw_tfx_sweep hw_texa_vectors hw_texa_sweep hw_fog_vectors hw_fog_sweep
        hw_atest_fold hw_atest_vectors hw_blend_vectors hw_blend_modes hw_blend_sweep
        hw_colclamp_vectors hw_dither_vectors hw_frame_convert hw_fbmask_merge hw_date_vectors
        hw_depth_vectors hw_wrap_vectors hw_pipeline_vectors)
    add_test(NAME ${hw_test} COMMAND $<TARGET_FILE:hw_gs_pixel_fixture> ${hw_test})
endforeach()
