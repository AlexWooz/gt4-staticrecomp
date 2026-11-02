cmake_minimum_required(VERSION 3.24)
project(GT4Recomp VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The qualified target is Windows x86-64")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT MSVC)
    message(FATAL_ERROR "Use clang-cl with the Visual Studio x64 environment")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Build tuning candidates (TASK-010). Defaults keep the qualified code generation unchanged.
# Each option applies to every target of this tree (runtime, generated VU1 code, tests) so the
# executables and the tables they link agree. Pinned EE game library (ps2game.lib) is not affected.
option(GT4_FP_CONTRACT_OFF "Build with -ffp-contract=off (control: no fused multiply-add)" OFF)
option(GT4_X86_64_V3 "Build for x86-64-v3 (AVX2, FMA); implies GT4_FP_CONTRACT_OFF" OFF)
if(GT4_FP_CONTRACT_OFF OR GT4_X86_64_V3)
    add_compile_options(/clang:-ffp-contract=off)
endif()
if(GT4_X86_64_V3)
    add_compile_options(/clang:-march=x86-64-v3)
endif()
# Owner rule (2026-10-30, decision isa-sse41-mandatory): builds target SSE4.1, the launcher's CPU floor; AVX2
# (GT4_X86_64_V3) is an A/B experiment only. SSE4.1 adds no fused multiply-add, so floating-point results are unchanged.
# Measured on the 3x night race (TASK-027 CPU survey, medians of 3): SSE2 -> SSE4.1 -3.8% wall time per VBlank, VU1
# worker -16%; SSE4.1 -> AVX2 no gain. The pinned EE game library is not rebuilt by this tree (TASK-134 P4).
option(GT4_SSE41 "Build for SSE4.1 (the launcher's CPU floor)" ON)
if(GT4_SSE41 AND NOT GT4_X86_64_V3)
    add_compile_options(/clang:-msse4.1)
endif()
# HWR hardware GS renderer (TASK-027, chapter 12.10): default OFF until qualified.
option(GT4_HW_GS "Build the HWR hardware GS renderer (native/hw_gs)" OFF)
# enable_testing() comes before the subdirectories so their add_test() calls register (native_memcard_store,
# native_license_guard).
enable_testing()
add_subdirectory(native)
add_subdirectory(tools/vu_recomp)
add_subdirectory(tools/gs_stream_record)
add_subdirectory(tools/gs_live_check)

set(GT4_PYTHON "${CMAKE_SOURCE_DIR}/private/tooling-venv/Scripts/python.exe"
    CACHE FILEPATH "Python environment with scripts/requirements.txt installed")
if(NOT EXISTS "${GT4_PYTHON}")
    message(FATAL_ERROR "Missing GT4_PYTHON environment; see docs/BUILD.md")
endif()
add_test(NAME native_vulkan_pixels COMMAND $<TARGET_FILE:vulkan_fixture>)
add_test(NAME native_vulkan_pixels_sync COMMAND $<TARGET_FILE:vulkan_fixture>)
set_tests_properties(native_vulkan_pixels_sync PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" ENVIRONMENT "GT4_GS_SYNC=1")
# TASK-081: the same pixel and Win32 presentation fixture on the baseline path (no swapchain maintenance 1, per-image
# present semaphores), as on a pre-2023 driver; and the pure extension selection.
add_test(NAME native_vulkan_pixels_baseline COMMAND $<TARGET_FILE:vulkan_fixture>)
set_tests_properties(native_vulkan_pixels_baseline PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" ENVIRONMENT "GT4_VULKAN_FORCE_BASELINE=1"
    PASS_REGULAR_EXPRESSION "presentation path: baseline [^\n]*forced by GT4_VULKAN_FORCE_BASELINE=1.*PASS: real Vulkan")
add_test(NAME native_vulkan_extensions COMMAND $<TARGET_FILE:vulkan_extensions_fixture>)
add_test(NAME native_gs_submit_ring COMMAND $<TARGET_FILE:gs_submit_ring_fixture>)
add_test(NAME native_fpu_chop COMMAND $<TARGET_FILE:fpu_chop_fixture>)
add_test(NAME native_vulkan_async_stress_async
    COMMAND $<TARGET_FILE:vulkan_async_stress> "${CMAKE_BINARY_DIR}/async-stress-async.bin")
add_test(NAME native_vulkan_async_stress_sync
    COMMAND $<TARGET_FILE:vulkan_async_stress> "${CMAKE_BINARY_DIR}/async-stress-sync.bin")
add_test(NAME native_vulkan_async_stress_compare
    COMMAND ${CMAKE_COMMAND} -E compare_files "${CMAKE_BINARY_DIR}/async-stress-async.bin" "${CMAKE_BINARY_DIR}/async-stress-sync.bin")
set_tests_properties(native_gs_submit_ring native_vulkan_async_stress_async native_vulkan_async_stress_sync
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 600 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(native_vulkan_async_stress_async PROPERTIES ENVIRONMENT "GT4_GS_SYNC=0")
set_tests_properties(native_vulkan_async_stress_sync PROPERTIES ENVIRONMENT "GT4_GS_SYNC=1")
set_tests_properties(native_vulkan_async_stress_compare PROPERTIES DEPENDS
    "native_vulkan_async_stress_async;native_vulkan_async_stress_sync")
add_test(NAME native_audio_input COMMAND $<TARGET_FILE:host_io_fixture>)
add_test(NAME native_controls COMMAND $<TARGET_FILE:controls_fixture>)
add_test(NAME native_pad_mapping COMMAND $<TARGET_FILE:pad_mapping_fixture>)
add_test(NAME native_pacing COMMAND $<TARGET_FILE:pacing_fixture>)
add_test(NAME native_achievements COMMAND $<TARGET_FILE:achievements_fixture>)
add_test(NAME native_achievements_spec2 COMMAND $<TARGET_FILE:achievements_spec2_fixture>)
add_test(NAME native_achievements_http COMMAND $<TARGET_FILE:achievements_http_fixture>)
set_tests_properties(native_achievements native_achievements_spec2 native_achievements_http PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
add_test(NAME native_quickfix COMMAND $<TARGET_FILE:quickfix_fixture>)
add_test(NAME native_race_display COMMAND $<TARGET_FILE:race_display_fixture>)
add_test(NAME native_race_widescreen COMMAND $<TARGET_FILE:race_widescreen_fixture>)
# TASK-080: GT4_DISPLAY_ASPECT parse, Hor+/Vert+ factors, matrix scale, scene policy.
add_test(NAME native_display_aspect COMMAND $<TARGET_FILE:display_aspect_fixture>)
add_test(NAME native_cursor_autohide COMMAND $<TARGET_FILE:cursor_autohide_fixture>)
add_test(NAME native_shape_observer COMMAND $<TARGET_FILE:shape_observer_fixture>)
add_test(NAME native_performance_accounting COMMAND $<TARGET_FILE:perf_accounting_fixture>)
add_test(NAME native_performance_probe COMMAND $<TARGET_FILE:perf_probe_fixture>)
# TASK-011 step 0: GIF scanner, worker overlap model, interval counters, report lines, GT4_MTVU_MEASURE switch.
add_test(NAME native_mtvu_measure COMMAND $<TARGET_FILE:mtvu_measure_fixture>)
# TASK-011 step 1: SPSC ring and ordered journal (docs/MTVU-PLAN.md section 5).
add_test(NAME native_mtvu_ring COMMAND $<TARGET_FILE:mtvu_ring_fixture>)
add_test(NAME native_mtvu_journal COMMAND $<TARGET_FILE:mtvu_journal_fixture>)
add_test(NAME native_mtvu_perf COMMAND $<TARGET_FILE:mtvu_perf_fixture>)
# TASK-084 step 1: the VIF1 chain batch payload (pure host code, round-trips and rejects malformed records).
add_test(NAME native_mtvu_vif_batch COMMAND $<TARGET_FILE:mtvu_vif_batch_fixture>)
# TASK-092: GT4_MTVU=0 / GT4_MTVU_VERIFY=1 decision (one condition for the GS client deferral and vblank_sync).
add_test(NAME native_mtvu_switches COMMAND $<TARGET_FILE:mtvu_switch_fixture>)
# TASK-035: GS stream transport (threads and a child process) and the game-side client end to end with the recorder.
add_test(NAME native_gs_stream_ipc COMMAND $<TARGET_FILE:gs_stream_ipc_fixture>)
add_test(NAME native_gs_stream_client
    COMMAND $<TARGET_FILE:gs_stream_client_fixture> $<TARGET_FILE:gs_stream_record> "${CMAKE_BINARY_DIR}/gs-stream-client-scratch")
set_tests_properties(native_gs_stream_ipc native_gs_stream_client PROPERTIES RUN_SERIAL TRUE TIMEOUT 300
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
# TASK-116 step 0: the READ_FIFO probe of the flare readback (GT4_GS_READBACK_LOG): ini parse, hash, aggregation.
add_test(NAME native_gs_readback_probe
    COMMAND $<TARGET_FILE:gs_readback_probe_fixture> "${CMAKE_BINARY_DIR}/gs-readback-probe-work")
set_tests_properties(native_gs_readback_probe PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
add_test(NAME native_shape_capture_export COMMAND $<TARGET_FILE:shape_capture_export_fixture>)
add_test(NAME native_shape_capture_control COMMAND $<TARGET_FILE:shape_capture_control_fixture>)
add_test(NAME python_native_contracts
    COMMAND "${GT4_PYTHON}" -m unittest discover -s "${CMAKE_SOURCE_DIR}/tests/python")
set_tests_properties(native_mtvu_measure native_mtvu_ring native_mtvu_journal native_mtvu_perf native_mtvu_vif_batch native_quickfix native_race_display native_race_widescreen native_vulkan_pixels native_audio_input native_shape_observer native_performance_accounting native_performance_probe native_shape_capture_export native_shape_capture_control python_native_contracts
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" ENVIRONMENT "GT4_NATIVE_BUILD=${CMAKE_BINARY_DIR}")
if(TARGET shape_runtime_fixture)
    add_test(NAME native_shape_runtime COMMAND $<TARGET_FILE:shape_runtime_fixture>)
    set_tests_properties(native_shape_runtime PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_property(TEST python_native_contracts APPEND PROPERTY ENVIRONMENT "GT4_SHAPE_RUNTIME_TESTS=1")
endif()
add_test(NAME vu_recomp_core COMMAND $<TARGET_FILE:gt4_vu_recomp_core_tests>)
add_test(NAME vu_recomp_differential COMMAND $<TARGET_FILE:gt4_vu_differential_tests>)
add_test(NAME vu_recomp_corpus COMMAND $<TARGET_FILE:gt4_vu_corpus_differential>)
set_tests_properties(vu_recomp_core vu_recomp_differential vu_recomp_corpus PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(vu_recomp_corpus PROPERTIES ENVIRONMENT "GT4_VU_CORPUS_DIR=${GT4_VU_CORPUS_DIR}")
if(TARGET gt4_vu1_aot_registry_test)
    add_test(NAME vu1_aot_registry COMMAND $<TARGET_FILE:gt4_vu1_aot_registry_test>)
    set_tests_properties(vu1_aot_registry PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" ENVIRONMENT "GT4_VU_CORPUS_DIR=${GT4_VU_CORPUS_DIR}")
endif()
add_test(NAME vu0_emitter COMMAND $<TARGET_FILE:gt4_vu0_emitter_tests>)
set_tests_properties(vu0_emitter PROPERTIES RUN_SERIAL TRUE TIMEOUT 300 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
if(TARGET gt4_vu0_aot_registry_test)
    add_test(NAME vu0_aot_registry COMMAND $<TARGET_FILE:gt4_vu0_aot_registry_test> "${GT4_VU0_IMAGE_FILE}")
    set_tests_properties(vu0_aot_registry PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME vu0_aot_differential COMMAND $<TARGET_FILE:gt4_vu0_differential_tests> "${GT4_VU0_IMAGE_FILE}")
    set_tests_properties(vu0_aot_differential PROPERTIES RUN_SERIAL TRUE TIMEOUT 3600 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
if(GT4_HW_GS)
    add_test(NAME hw_options COMMAND $<TARGET_FILE:hw_options_fixture>)
    add_test(NAME hw_present_rect COMMAND $<TARGET_FILE:hw_present_rect_fixture>)
    # Empty GT4_RENDERER means unset (donor): nothing runs.
    add_test(NAME hw_smoke_donor COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 10)
    set_tests_properties(hw_smoke_donor PROPERTIES ENVIRONMENT "GT4_RENDERER=" PASS_REGULAR_EXPRESSION "HWR not selected")
    # Headless 4,200 frames with black presents (no window: presents are counted, nothing shown).
    add_test(NAME hw_smoke_headless COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 4200)
    set_tests_properties(hw_smoke_headless PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "frames=4200 presents=4200 black_presents=0")
    # Hidden window with a real swapchain: 120 presents, then the fail-closed cases. With no GS data the
    # all-zero register file decodes as a known mode with both circuits off, so every present carries the
    # composed background image (BGCOLOR 0, black) and none is an image-less black present (IN-333).
    add_test(NAME hw_smoke_window COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 120 --window)
    set_tests_properties(hw_smoke_window PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "frames=120 presents=120 black_presents=0 gif_bytes=0 swapchain_recreations=0 faults=0\ngpu_composed=0\npresent_rect=[0-9]+,[0-9]+ [0-9]+x[0-9]+ present_16x9=1")
    # IN-364: with a display on circuit 1 every present is composed on the GPU and copied into the swapchain.
    add_test(NAME hw_smoke_window_gpu_compose COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 120 --window --display)
    set_tests_properties(hw_smoke_window_gpu_compose PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "frames=120 presents=120 black_presents=0 gif_bytes=0 swapchain_recreations=0 faults=0\ngpu_composed=120\npresent_rect=[0-9]+,[0-9]+ [0-9]+x[0-9]+ present_16x9=1")
    # IN-372/IN-375: at GT4_HW_SCALE=4 the GPU compose is 4x and the present reduces it by 2x2 box steps first.
    add_test(NAME hw_smoke_window_scaled COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 120 --window --display-640)
    set_tests_properties(hw_smoke_window_scaled PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_SCALE=4"
        PASS_REGULAR_EXPRESSION "faults=0\ngpu_composed=120\npresent_rect=[0-9]+,[0-9]+ [0-9]+x[0-9]+ present_16x9=1\nclient=[0-9]+x[0-9]+ fill=0 present_expected=1\npresent_downscale_levels=[1-9] scale=4")
    # TASK-080 on CoreGS 2.0 (owner test 6, ultrawide): the race-view display-aspect fill. A 1720 x 720 client (the
    # owner's 21.5:9) shows 16:9 as a pillarbox without the fill, the whole client at 21.5:9, 21:9 with thin side bars,
    # and 32:9 letterboxed (never stretched); a 1280 x 720 client letterboxes 21:9. Each rectangle must be exactly the
    # one gt4::hw::present_rect_for gives (hw_present_rect checks that it equals the default renderer's).
    add_test(NAME hw_smoke_window_aspect_169 COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 30 --display-640 --client 1720 720)
    set_tests_properties(hw_smoke_window_aspect_169 PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_SCALE=2"
        PASS_REGULAR_EXPRESSION "present_rect=220,0 1280x720 present_16x9=1\nclient=1720x720 fill=0 present_expected=1")
    add_test(NAME hw_smoke_window_aspect_ultrawide COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 30 --display-640 --client 1720 720 --fill 2.388889)
    set_tests_properties(hw_smoke_window_aspect_ultrawide PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_SCALE=2"
        PASS_REGULAR_EXPRESSION "present_rect=0,0 1720x720 present_16x9=0\nclient=1720x720 fill=1 present_expected=1")
    add_test(NAME hw_smoke_window_aspect_219 COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 30 --display-640 --client 1720 720 --fill 2.333333)
    set_tests_properties(hw_smoke_window_aspect_219 PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_SCALE=2"
        PASS_REGULAR_EXPRESSION "present_rect=20,0 1680x720 present_16x9=0\nclient=1720x720 fill=1 present_expected=1")
    add_test(NAME hw_smoke_window_aspect_329 COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 30 --display-640 --client 1720 720 --fill 3.555556)
    set_tests_properties(hw_smoke_window_aspect_329 PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_SCALE=2"
        PASS_REGULAR_EXPRESSION "present_rect=0,118 1720x484 present_16x9=0\nclient=1720x720 fill=1 present_expected=1")
    add_test(NAME hw_smoke_window_aspect_219_in_169 COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 30 --display-640 --client 1280 720 --fill 2.333333)
    set_tests_properties(hw_smoke_window_aspect_219_in_169 PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "present_rect=0,85 1280x549 present_16x9=0\nclient=1280x720 fill=1 present_expected=1")
    # The fill requests reach the in-process presenter through main's GS stream seam (no GS process in hw mode).
    add_test(NAME hw_stream_sink_display_aspect COMMAND $<TARGET_FILE:hw_stream_sink_fixture> --display-aspect)
    set_tests_properties(hw_stream_sink_display_aspect PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "presentation: fill at ratio 2.3333.*presentation: fill at ratio 3.5556.*presentation: 16:9 pillarbox.*aspect_changes=3 .*PASS: hw stream sink")
    # Fullscreen-style toggles on the hidden window: resize flag, swapchain rebuild, every present under 100 ms.
    add_test(NAME hw_smoke_resize COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 1 --resize-toggles 5)
    set_tests_properties(hw_smoke_resize PROPERTIES ENVIRONMENT "GT4_RENDERER=hw" PASS_REGULAR_EXPRESSION "resize_toggles=5")
    add_test(NAME hw_smoke_parallel_refused COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 1)
    set_tests_properties(hw_smoke_parallel_refused PROPERTIES ENVIRONMENT "GT4_RENDERER=parallel" WILL_FAIL TRUE)
    # task/027-coregs2: GT4_RENDERER=hw through main's GS stream seam, in process (native/gs_stream/gs_stream_sink.hpp).
    add_test(NAME hw_stream_sink COMMAND $<TARGET_FILE:hw_stream_sink_fixture>)
    set_tests_properties(hw_stream_sink PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "vsyncs=3 frames=3 presents=0 black_presents=0 gif_bytes=32 .*PASS: hw stream sink")
    # TASK-084 merge: the VBlank journal holds the VSYNC and posts it after the frame's output; the hw sink gets one per post.
    add_test(NAME hw_stream_sink_deferred_vsync COMMAND $<TARGET_FILE:hw_stream_sink_fixture> --deferred-vsync)
    set_tests_properties(hw_stream_sink_deferred_vsync PROPERTIES ENVIRONMENT "GT4_RENDERER=hw"
        PASS_REGULAR_EXPRESSION "vsyncs=3 frames=3 presents=0 black_presents=0 gif_bytes=128 .*PASS: hw stream sink")
    # TASK-027 CPU survey: the transport line (EE-side hook cost, back-pressure, worker busy/idle) with the profile.
    add_test(NAME hw_stream_sink_transport COMMAND $<TARGET_FILE:hw_stream_sink_fixture>)
    set_tests_properties(hw_stream_sink_transport PROPERTIES ENVIRONMENT "GT4_RENDERER=hw;GT4_HW_PROFILE_EVERY=1"
        PASS_REGULAR_EXPRESSION "hw. transport vsync=2 ee: gif_cmds=[0-9.]+ gif_kib=[0-9]+ .* worker: gif_ms=[0-9.]+ .*PASS: hw stream sink")
    add_test(NAME hw_cpu_sampler COMMAND $<TARGET_FILE:hw_cpu_sampler_fixture>)
    set_tests_properties(hw_cpu_sampler PROPERTIES RUN_SERIAL TRUE TIMEOUT 60
        PASS_REGULAR_EXPRESSION "hw_cpu_sampler: all checks passed")
    add_test(NAME hw_stream_sink_capture_refused COMMAND $<TARGET_FILE:hw_stream_sink_fixture>)
    set_tests_properties(hw_stream_sink_capture_refused PROPERTIES
        ENVIRONMENT "GT4_RENDERER=hw;GT4_GS_CAPTURE=${CMAKE_BINARY_DIR}/hw-capture-refused.gscap" WILL_FAIL TRUE)
    add_test(NAME hw_smoke_unknown_renderer_refused COMMAND $<TARGET_FILE:hw_gs_smoke> --frames 1)
    set_tests_properties(hw_smoke_unknown_renderer_refused PROPERTIES ENVIRONMENT "GT4_RENDERER=vulkan" WILL_FAIL TRUE)
    set_tests_properties(hw_options hw_present_rect hw_smoke_donor hw_smoke_headless hw_smoke_window
        hw_smoke_parallel_refused hw_smoke_unknown_renderer_refused hw_stream_sink hw_stream_sink_capture_refused hw_stream_sink_deferred_vsync hw_smoke_window_scaled
        hw_smoke_window_aspect_169 hw_smoke_window_aspect_ultrawide hw_smoke_window_aspect_219 hw_smoke_window_aspect_329
        hw_smoke_window_aspect_219_in_169 hw_stream_sink_display_aspect
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 300 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
