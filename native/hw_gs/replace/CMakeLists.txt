# HWR texture replacement core (TASK-027, milestones M16 and M18, pure C++ part).
# Written from the TASK-026 specification (docs/hw-renderer-spec), clean room (decision 0069).
cmake_minimum_required(VERSION 3.24)
project(hw_gs_replace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hw_gs_replace STATIC
  src/gs_types.cpp
  src/xxh3.cpp
  src/xxh3_impl.cpp
  src/clut.cpp
  src/keys.cpp
  src/names.cpp
  src/packs.cpp
  src/images.cpp
  src/stb_image_impl.cpp
  tests/png_encode.cpp  # the stored-deflate PNG writer also writes texture dumps (chapter 11.6)
)
target_include_directories(hw_gs_replace
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
if(MSVC)
  target_compile_options(hw_gs_replace PRIVATE /W4 /permissive-)
else()
  target_compile_options(hw_gs_replace PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(hw_gs_replace_tests tests/hw_gs_replace_tests.cpp tests/reference_gs_memory.cpp)
target_link_libraries(hw_gs_replace_tests PRIVATE hw_gs_replace)
target_include_directories(hw_gs_replace_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME hw_replace_all COMMAND hw_gs_replace_tests)
add_test(NAME hw_replace_q07_pcsx2_dumps COMMAND hw_gs_replace_tests --pending-q07)
set_tests_properties(hw_replace_q07_pcsx2_dumps PROPERTIES SKIP_RETURN_CODE 77)
