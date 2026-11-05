@echo off
rem TASK-180 test3: configure and build libgt4spec2.so (the Spec II game library) with the NDK, arm64 only.
rem The inputs are the Spec II ones the Windows build uses (docs/BUILD.md, the t134 spec2 recipe): the rc14b Spec II
rem translated units and their hook manifest, the Spec II IOP runtime capsule, the Spec II VU0 image, and the shared VU1
rem and port directories. Same flags as libgt4.so: API 29, -ffp-contract=off, 16 KB page alignment (CMakeLists.txt).
rem usage: configure-android-spec2.cmd <build folder>
setlocal
set "OUT=%~1"
if "%OUT%"=="" goto usage
if "%GT4_ANDROID_NDK%"=="" set "GT4_ANDROID_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125"
if "%GT4_ASSETS_ROOT%"=="" set "GT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp"
if "%GT4_VULKAN_INCLUDE%"=="" set "GT4_VULKAN_INCLUDE=C:/VulkanSDK/1.4.341.1/Include"
set "A=%GT4_ASSETS_ROOT%"
set "HERE=%~dp0"
cmake -S "%HERE%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_TOOLCHAIN_FILE="%GT4_ANDROID_NDK%\build\cmake\android.toolchain.cmake" ^
 -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static ^
 "-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG" "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" ^
 -DGT4_ASSETS_ROOT=%A% -DGT4_TARGET=spec2 ^
 -DGT4_EE_UNITS_DIR=%HERE%../../build/android-inputs/spec2-units-rc14b ^
 -DGT4_EE_UNITS_MANIFEST=%A%/private/slice408-ee-spec2/spec2-units-rc14b.camera360-units.json ^
 -DGT4_PORT_DIR=%A%/private/slice200-port ^
 -DGT4_PERF_RUNTIME_SOURCE=%A%/private/slice276-spec2-iop-runtime ^
 -DGT4_LICENSE_GUARD_SOURCE=%A%/private/slice376-frame-render-license-guard-runtime ^
 -DGT4_MEMCARD_RUNTIME_SOURCE=%A%/private/slice200-memcard-runtime ^
 -DGT4_IPU_RUNTIME_SOURCE=%A%/private/slice365-ipu-fast-runtime ^
 -DGT4_SPU2_RUNTIME_SOURCE=%A%/private/slice396-spu2-runtime ^
 -DGT4_VU1_GENERATED_DIR=%A%/private/slice397-vu1-gen ^
 -DGT4_VU0_GENERATED_DIR=%A%/private/slice394-spec2-vu0-gen ^
 -DGT4_HW_SHADER_CPP_DIR=C:/Antigravity/gt4-wt-180-android/build/t180-v200/native/hw_gs/shaders ^
 -DGT4_VULKAN_INCLUDE=%GT4_VULKAN_INCLUDE% ^
 -DGT4_SDL3_ANDROID_AAR_DIR=%A%/private/tooling/sdl3-3.4.18-android/aar ^
 -DGT4_PYTHON=%A%/private/tooling-venv/Scripts/python.exe
if errorlevel 1 exit /b 1
cmake --build "%OUT%" -- -k 0
exit /b %errorlevel%
:usage
echo usage: configure-android-spec2.cmd ^<build folder^>
exit /b 2
