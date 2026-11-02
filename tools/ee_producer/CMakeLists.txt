cmake_minimum_required(VERSION 3.24)
project(GT4EEProducer LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The qualified EE producer requires Windows x86-64")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT MSVC)
    message(FATAL_ERROR "Use clang-cl with the Visual Studio x64 environment")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(GT4_DONOR "" CACHE PATH "Verified frozen ps2-web-recomp source directory")
set(GT4_GENERATED "" CACHE PATH "Verified fresh EE generated source directory")
set(GT4_CHANGED_UNITS "" CACHE FILEPATH "Diagnostic-only list of changed generated compilation units")
option(GT4_BUILD_TRANSLATOR "Build the frozen donor's offline translator" ON)
option(GT4_ANALYSIS_CONTROLS "Build bounded offline recovery controls" OFF)
if(NOT EXISTS "${GT4_DONOR}/runtime/include/ps2/generated.hpp")
    message(FATAL_ERROR "Missing verified donor; run ee_producer.py prepare first")
endif()

if(GT4_ANALYSIS_CONTROLS)
    add_executable(ee_analysis_controls analysis_controls.cpp
        "${GT4_DONOR}/recomp/analysis.cpp" "${GT4_DONOR}/recomp/elf.cpp"
        "${GT4_DONOR}/recomp/r5900.cpp")
    target_include_directories(ee_analysis_controls PRIVATE "${GT4_DONOR}/recomp")
    target_compile_options(ee_analysis_controls PRIVATE /W4)
    enable_testing()
    add_test(NAME ee_recovery_controls COMMAND ee_analysis_controls)
    set_tests_properties(ee_recovery_controls PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
endif()

if(GT4_BUILD_TRANSLATOR)
    add_executable(eerecomp
        "${GT4_DONOR}/recomp/main.cpp"
        "${GT4_DONOR}/recomp/r5900.cpp"
        "${GT4_DONOR}/recomp/elf.cpp"
        "${GT4_DONOR}/recomp/analysis.cpp"
        "${GT4_DONOR}/recomp/codegen.cpp"
        "${GT4_DONOR}/recomp/snr2.cpp")
    target_compile_options(eerecomp PRIVATE /W4)
endif()

if(GT4_GENERATED)
    if(NOT EXISTS "${GT4_GENERATED}/table.cpp" OR NOT EXISTS "${GT4_GENERATED}/units.hpp")
        message(FATAL_ERROR "Missing generated registry/header")
    endif()
    file(GLOB generated_sources CONFIGURE_DEPENDS "${GT4_GENERATED}/*.cpp")
    add_library(ps2game STATIC ${generated_sources})
    target_include_directories(ps2game PRIVATE "${GT4_GENERATED}" "${GT4_DONOR}/runtime/include")
    # TASK-134: the same units with the exact faster forms of fp::add and fp::mul (scripts/prepare_fast_headers.py).
    option(GT4_FAST_HEADERS "Build the units with the exact faster donor headers" OFF)
    if(GT4_FAST_HEADERS)
        execute_process(COMMAND "${GT4_PYTHON}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/prepare_fast_headers.py" "${GT4_DONOR}"
                        "${CMAKE_BINARY_DIR}/fast_headers" RESULT_VARIABLE fast_headers_result)
        if(NOT fast_headers_result EQUAL 0)
            message(FATAL_ERROR "scripts/prepare_fast_headers.py failed")
        endif()
        target_include_directories(ps2game BEFORE PRIVATE "${CMAKE_BINARY_DIR}/fast_headers")
    endif()
    target_compile_options(ps2game PRIVATE -Wno-unused-label -Wno-unused-variable)
    if(GT4_CHANGED_UNITS)
        file(STRINGS "${GT4_CHANGED_UNITS}" changed_unit_names)
        set(changed_sources)
        foreach(name IN LISTS changed_unit_names)
            if(NOT name MATCHES "^(unit_[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]|table)\\.cpp$"
               OR NOT EXISTS "${GT4_GENERATED}/${name}")
                message(FATAL_ERROR "Invalid diagnostic compilation-unit name")
            endif()
            list(APPEND changed_sources "${GT4_GENERATED}/${name}")
        endforeach()
        add_library(ee_changed_unit_controls OBJECT ${changed_sources})
        target_include_directories(ee_changed_unit_controls PRIVATE "${GT4_GENERATED}" "${GT4_DONOR}/runtime/include")
        target_compile_options(ee_changed_unit_controls PRIVATE -Wno-unused-label -Wno-unused-variable)
    endif()
endif()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/toolchain.txt" CONTENT
"compiler=${CMAKE_CXX_COMPILER}\ncompiler_id=${CMAKE_CXX_COMPILER_ID}\ncompiler_version=${CMAKE_CXX_COMPILER_VERSION}\nbuild_type=${CMAKE_BUILD_TYPE}\nflags=${CMAKE_CXX_FLAGS}\nrelease_flags=${CMAKE_CXX_FLAGS_RELEASE}\ncxx_standard=20\ndonor=${GT4_DONOR}\ngenerated=${GT4_GENERATED}\n")

set(GT4_PYTHON "" CACHE FILEPATH "Python environment with project requirements installed")
if(GT4_PYTHON)
    if(NOT EXISTS "${GT4_PYTHON}")
        message(FATAL_ERROR "GT4_PYTHON does not exist")
    endif()
    get_filename_component(GT4_PROJECT_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    enable_testing()
    add_test(NAME ee_producer_controls COMMAND "${GT4_PYTHON}" -m unittest discover
             -s "${GT4_PROJECT_ROOT}/tests/python")
    set_tests_properties(ee_producer_controls PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
                        WORKING_DIRECTORY "${GT4_PROJECT_ROOT}")
endif()
