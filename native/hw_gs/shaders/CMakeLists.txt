# HWR shader toolchain (TASK-027, M4). Copyright (c) 2026 GT4Recomp contributors.
#
# GLSL sources in this directory are compiled at build time by a pinned glslangValidator into
# SPIR-V and embedded as C++ word arrays (hw_shader_blobs.hpp). A shader error fails the build.
# Permutations are compile-time #define sets listed in hw_add_shader() calls below (IN-211).
#
# Standalone use: cmake -S native/hw_gs/shaders -B <build dir>. Also usable from native/hw_gs.
# Pins and provenance: native/THIRD_PARTY.md. Interpretations: docs/hw-renderer-spec/IMPLEMENTATION-NOTES.md.

cmake_minimum_required(VERSION 3.24)

if(NOT PROJECT_NAME)
    project(HwGsShaders VERSION 0.1.0 LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

# ---- Pinned shader compiler ---------------------------------------------------------------------
# glslangValidator 16.2.0 from the Vulkan SDK 1.4.341.1 (SHA-256 below). Upgrading the SDK means
# updating this pin and THIRD_PARTY.md in the same commit.
set(GT4_HW_GLSLANG_SHA256 "db832826d73049193882d5572ad76e14fcb7b1cdfc1a6b892ac25217b850b6bc"
    CACHE STRING "Pinned SHA-256 of glslangValidator.exe (IN-201)")
set(GT4_HW_GLSLANG "$ENV{VULKAN_SDK}/Bin/glslangValidator.exe"
    CACHE FILEPATH "glslangValidator used to build HWR shaders (pinned by GT4_HW_GLSLANG_SHA256)")
if(NOT EXISTS "${GT4_HW_GLSLANG}")
    message(FATAL_ERROR "HWR shaders need glslangValidator: set GT4_HW_GLSLANG or VULKAN_SDK")
endif()
file(SHA256 "${GT4_HW_GLSLANG}" GT4_HW_GLSLANG_ACTUAL_SHA256)
if(NOT GT4_HW_GLSLANG_ACTUAL_SHA256 STREQUAL GT4_HW_GLSLANG_SHA256)
    message(FATAL_ERROR "glslangValidator hash ${GT4_HW_GLSLANG_ACTUAL_SHA256} does not match the pin "
                        "${GT4_HW_GLSLANG_SHA256} (IN-201). Update the pin deliberately or fix GT4_HW_GLSLANG.")
endif()
execute_process(COMMAND "${GT4_HW_GLSLANG}" --version OUTPUT_VARIABLE GT4_HW_GLSLANG_VERSION_TEXT
                ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "Glslang Version: [^\n]*" GT4_HW_GLSLANG_VERSION_LINE "${GT4_HW_GLSLANG_VERSION_TEXT}")
message(STATUS "HWR shaders: ${GT4_HW_GLSLANG_VERSION_LINE} (pin ${GT4_HW_GLSLANG_SHA256})")

set(HW_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(HW_SHADER_INCLUDES "${HW_SHADER_DIR}/hw_pixel_core.glsl" "${HW_SHADER_DIR}/hw_glm_common.glsl")
set(HW_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/embed_spirv.cmake")

# hw_add_shader(<symbol> <source> <vert|frag|comp> [DEFINES <macro=value>...])
# Produces <symbol>_words and <symbol>_word_count in namespace hwgs::shaders.
set(HW_SHADER_CPP_FILES "")
function(hw_add_shader SYMBOL SOURCE STAGE)
    cmake_parse_arguments(ARG "" "" "DEFINES" ${ARGN})
    set(defines "")
    foreach(define IN LISTS ARG_DEFINES)
        list(APPEND defines "-D${define}")
    endforeach()
    set(spv "${CMAKE_CURRENT_BINARY_DIR}/${SYMBOL}.spv")
    set(cpp "${CMAKE_CURRENT_BINARY_DIR}/${SYMBOL}_spirv.cpp")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND "${GT4_HW_GLSLANG}" -V --target-env vulkan1.2 "-I${HW_SHADER_DIR}" -S ${STAGE}
                ${defines} -o "${spv}" "${SOURCE}"
        DEPENDS "${SOURCE}" ${HW_SHADER_INCLUDES}
        COMMENT "glslang ${SYMBOL} (${STAGE})"
        VERBATIM)
    add_custom_command(
        OUTPUT "${cpp}"
        COMMAND ${CMAKE_COMMAND} "-DSPV=${spv}" "-DCPP=${cpp}" "-DSYMBOL=${SYMBOL}" -P "${HW_EMBED_SCRIPT}"
        DEPENDS "${spv}" "${HW_EMBED_SCRIPT}"
        COMMENT "embed ${SYMBOL}"
        VERBATIM)
    set(HW_SHADER_CPP_FILES ${HW_SHADER_CPP_FILES} "${cpp}" PARENT_SCOPE)
endfunction()

hw_add_shader(hw_vertex_smooth "${HW_SHADER_DIR}/hw_vertex.vert" vert DEFINES HWR_FLAT_COLOUR=0)
hw_add_shader(hw_vertex_flat "${HW_SHADER_DIR}/hw_vertex.vert" vert DEFINES HWR_FLAT_COLOUR=1)
hw_add_shader(hw_pixel_uber_smooth "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag DEFINES HWR_FLAT_COLOUR=0)
hw_add_shader(hw_pixel_uber_smooth_interlock "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag
              DEFINES HWR_FLAT_COLOUR=0 HWR_INTERLOCK=1)
hw_add_shader(hw_pixel_uber_flat "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag DEFINES HWR_FLAT_COLOUR=1)
# IN-451: core 2's variants reject occluded fragments before texturing (draw set binding 5: the depth image again).
hw_add_shader(hw_pixel_uber_core2 "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag DEFINES HWR_FLAT_COLOUR=0 HWR_EARLY_Z=1)
hw_add_shader(hw_pixel_uber_core2_interlock "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag
              DEFINES HWR_FLAT_COLOUR=0 HWR_INTERLOCK=1 HWR_EARLY_Z=1)
# IN-481: core 2's variant for draws that read nothing back: colour attachments instead of storage writes.
hw_add_shader(hw_pixel_uber_core2_rop "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag DEFINES HWR_FLAT_COLOUR=0 HWR_ROP=1)
hw_add_shader(hw_pixel_uber_core2_rop_blend "${HW_SHADER_DIR}/hw_pixel_uber.frag" frag DEFINES HWR_FLAT_COLOUR=0 HWR_ROP=2)
# M10 GLM kernels (IN-347): texture decode from the GLM and target swizzle into the GLM.
hw_add_shader(hw_glm_decode "${HW_SHADER_DIR}/hw_glm_decode.comp" comp)
hw_add_shader(hw_glm_swizzle "${HW_SHADER_DIR}/hw_glm_swizzle.comp" comp)
hw_add_shader(hw_glm_clut "${HW_SHADER_DIR}/hw_glm_clut.comp" comp)
# M12 upscaling (IN-371/IN-372): scaled-image refresh from 1x words and the display compose at scale.
hw_add_shader(hw_scale_upsample "${HW_SHADER_DIR}/hw_scale_upsample.comp" comp)
hw_add_shader(hw_display_compose_scaled "${HW_SHADER_DIR}/hw_display_compose_scaled.comp" comp)
# IN-379: the scaled image of a frame after GT4's RGB tone curve (09.4 variant A).
hw_add_shader(hw_scale_tone_carry "${HW_SHADER_DIR}/hw_scale_tone_carry.comp" comp)
# IN-383: the tone sequence's eight strip batches of one page in one dispatch (09.4 variant A, parity plan item 2).
hw_add_shader(hw_tone_strips "${HW_SHADER_DIR}/hw_tone_strips.comp" comp)
# IN-381: 13.4 deinterlace of the scaled presentation frame (480i menus at scale).
hw_add_shader(hw_deinterlace_scaled "${HW_SHADER_DIR}/hw_deinterlace_scaled.comp" comp)
# 13.5 edge-only FXAA of the presentation frame (IN-376).
hw_add_shader(hw_postfx_fxaa "${HW_SHADER_DIR}/hw_postfx_fxaa.comp" comp)
hw_add_shader(hw_glm_unswizzle "${HW_SHADER_DIR}/hw_glm_unswizzle.comp" comp)
hw_add_shader(hw_glm_merge "${HW_SHADER_DIR}/hw_glm_merge.comp" comp)
hw_add_shader(hw_glm_host_upload "${HW_SHADER_DIR}/hw_glm_host_upload.comp" comp)
hw_add_shader(hw_display_compose "${HW_SHADER_DIR}/hw_display_compose.comp" comp)
# CORE2 core rewrite (CORE2-SPEC): patch from memory, reduction to 1x, GT4 hook, display merge.
hw_add_shader(core2_patch "${HW_SHADER_DIR}/core2_patch.comp" comp)
hw_add_shader(core2_reduce "${HW_SHADER_DIR}/core2_reduce.comp" comp)
hw_add_shader(core2_tone "${HW_SHADER_DIR}/core2_tone.comp" comp)
hw_add_shader(core2_patch_glm "${HW_SHADER_DIR}/core2_patch_glm.comp" comp)
hw_add_shader(core2_compose "${HW_SHADER_DIR}/core2_compose.comp" comp)
hw_add_shader(core2_present_scale "${HW_SHADER_DIR}/core2_present_scale.comp" comp)
# TASK-135: the performance HUD's panel.
hw_add_shader(hw_hud_vertex "${HW_SHADER_DIR}/hw_hud.vert" vert)
hw_add_shader(hw_hud_fragment "${HW_SHADER_DIR}/hw_hud.frag" frag)
hw_add_shader(core2_fill "${HW_SHADER_DIR}/core2_fill.comp" comp)
hw_add_shader(core2_clut_target "${HW_SHADER_DIR}/core2_clut_target.comp" comp)
hw_add_shader(core2_materialize "${HW_SHADER_DIR}/core2_materialize.comp" comp)
hw_add_shader(hw_blend_exact "${HW_SHADER_DIR}/hw_blend_exact.comp" comp)
hw_add_shader(hw_pixel_exact "${HW_SHADER_DIR}/hw_pixel_exact.comp" comp)

add_library(gt4_hw_shaders STATIC ${HW_SHADER_CPP_FILES})
target_include_directories(gt4_hw_shaders PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(gt4_hw_shaders PUBLIC cxx_std_20)

# ---- Exactness fixture (headless Vulkan compute, no window) -------------------------------------
if(NOT TARGET hw_gs_pixel)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../pixel" "${CMAKE_CURRENT_BINARY_DIR}/pixel")
endif()

set(GT4_HW_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root with Include/ and Lib/vulkan-1.lib")
if(NOT EXISTS "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
    message(FATAL_ERROR "HWR GPU fixture needs a Vulkan SDK: set GT4_HW_VULKAN_SDK or VULKAN_SDK")
endif()

add_executable(hw_pixel_gpu_fixture hw_pixel_gpu_fixture.cpp)
target_include_directories(hw_pixel_gpu_fixture PRIVATE "${GT4_HW_VULKAN_SDK}/Include")
target_link_libraries(hw_pixel_gpu_fixture PRIVATE gt4_hw_shaders hw_gs_pixel "${GT4_HW_VULKAN_SDK}/Lib/vulkan-1.lib")
if(MSVC)
    # Vulkan create-info structs are brace-initialised with only sType set (pNext zero by design).
    target_compile_options(hw_pixel_gpu_fixture PRIVATE /W4 -Wno-missing-field-initializers)
    target_compile_definitions(hw_pixel_gpu_fixture PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

foreach(hw_gpu_test hw_gpu_blend_sweep hw_gpu_pixel_directed hw_gpu_pixel_random)
    add_test(NAME ${hw_gpu_test} COMMAND $<TARGET_FILE:hw_pixel_gpu_fixture> ${hw_gpu_test})
    # Exit 77 = no compute-capable Vulkan 1.2 device: reported as Skipped, never as a pass.
    set_tests_properties(${hw_gpu_test} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
