@echo off
rem TASK-180: configure and build libgt4.so with the NDK.
rem usage: configure-android.cmd <x86_64 | arm64-v8a> <build folder> [cmake --build target]
rem Inputs (environment, with defaults for this machine):
rem   GT4_ANDROID_NDK      NDK root (default: %LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125)
rem   GT4_ASSETS_ROOT      main checkout with private/ (default: C:/Antigravity/gt4-staticrecomp)
rem   GT4_EE_UNITS_DIR     translated v2.00 units (required; e.g. a copy of the rc14b units under build/android-inputs)
rem   GT4_EE_UNITS_MANIFEST hook manifest of those units (default: private/slice408-ee-v200/v200-units-rc14b.camera360-units.json)
rem   GT4_HW_SHADER_CPP_DIR CoreGS 2.0 shaders as C++ from a Windows build (required; <build>/native/hw_gs/shaders)
rem   GT4_VULKAN_INCLUDE   Khronos headers (default: C:/VulkanSDK/1.4.341.1/Include)
rem API level 29 (Android 10) is the floor: Vulkan 1.1 and AAudio everywhere; the 16 KB page alignment is set in
rem CMakeLists.txt for every ABI.
setlocal
set "ABI=%~1"
set "OUT=%~2"
if "%ABI%"=="" goto usage
if "%OUT%"=="" goto usage
if "%GT4_ANDROID_NDK%"=="" set "GT4_ANDROID_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125"
if "%GT4_ASSETS_ROOT%"=="" set "GT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp"
if "%GT4_EE_UNITS_MANIFEST%"=="" set "GT4_EE_UNITS_MANIFEST=%GT4_ASSETS_ROOT%/private/slice408-ee-v200/v200-units-rc14b.camera360-units.json"
if "%GT4_VULKAN_INCLUDE%"=="" set "GT4_VULKAN_INCLUDE=C:/VulkanSDK/1.4.341.1/Include"
if "%GT4_EE_UNITS_DIR%"=="" (echo GT4_EE_UNITS_DIR is required & exit /b 2)
if "%GT4_HW_SHADER_CPP_DIR%"=="" (echo GT4_HW_SHADER_CPP_DIR is required & exit /b 2)
set "A=%GT4_ASSETS_ROOT%"
cmake -S "%~dp0." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_TOOLCHAIN_FILE="%GT4_ANDROID_NDK%\build\cmake\android.toolchain.cmake" ^
 -DANDROID_ABI=%ABI% -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared ^
 "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" ^
 -DGT4_ASSETS_ROOT=%A% -DGT4_TARGET=v200 ^
 -DGT4_EE_UNITS_DIR=%GT4_EE_UNITS_DIR% -DGT4_EE_UNITS_MANIFEST=%GT4_EE_UNITS_MANIFEST% ^
 -DGT4_PORT_DIR=%A%/private/slice200-port ^
 -DGT4_PERF_RUNTIME_SOURCE=%A%/private/slice262-frame-cost-runtime ^
 -DGT4_LICENSE_GUARD_SOURCE=%A%/private/slice376-frame-render-license-guard-runtime ^
 -DGT4_MEMCARD_RUNTIME_SOURCE=%A%/private/slice200-memcard-runtime ^
 -DGT4_IPU_RUNTIME_SOURCE=%A%/private/slice365-ipu-fast-runtime ^
 -DGT4_SPU2_RUNTIME_SOURCE=%A%/private/slice396-spu2-runtime ^
 -DGT4_VU1_GENERATED_DIR=%A%/private/slice397-vu1-gen ^
 -DGT4_VU0_GENERATED_DIR=%A%/private/slice394-v200-vu0-gen ^
 -DGT4_HW_SHADER_CPP_DIR=%GT4_HW_SHADER_CPP_DIR% ^
 -DGT4_VULKAN_INCLUDE=%GT4_VULKAN_INCLUDE% ^
 -DGT4_SDL3_ANDROID_AAR_DIR=%A%/private/tooling/sdl3-3.4.18-android/aar ^
 -DGT4_PYTHON=%A%/private/tooling-venv/Scripts/python.exe
if errorlevel 1 exit /b 1
if "%~3"=="" (cmake --build "%OUT%" -- -k 0) else (cmake --build "%OUT%" --target %~3 -- -k 0)
exit /b %errorlevel%
:usage
echo usage: configure-android.cmd ^<x86_64^|arm64-v8a^> ^<build folder^> [target]
exit /b 2
