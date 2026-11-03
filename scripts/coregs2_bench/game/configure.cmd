@echo off
rem usage: configure.cmd <build dir under the worktree's build\> <v200|spec2> [extra cmake args]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
for %%I in ("%~dp0..\..\..") do set "WT=%%~fI"
set "A=C:/Antigravity/gt4-staticrecomp"
set "DIR=%~1"
set "TARGET=%~2"
set "EXTRA=%T134_ARGS%"
rem VU1GEN and VU0GEN override the VU tables (VU1 slice397: rc10, a product with VF0.w is a copy; VU0 slice394: the second
rem rc9 round; the first rc9 had slice157 and slice390/slice160).
if "%VU1GEN%"=="" set "VU1GEN=%A%/private/slice397-vu1-gen"
set "VU0GEN_GIVEN=%VU0GEN%"
if /i "%TARGET%"=="spec2" (
  set "PROFILE=spec2"
  set "RUNTIME=%A%/private/slice276-spec2-iop-runtime"
  set "VU0GEN=%A%/private/slice394-spec2-vu0-gen"
  set "VU0IMG=%A%/private/slice390-spec2-vu0-image/vu0_image.bin"
) else (
  set "PROFILE=aspect"
  set "RUNTIME=%A%/private/slice262-frame-cost-runtime"
  set "VU0GEN=%A%/private/slice394-v200-vu0-gen"
  set "VU0IMG=%A%/private/slice160-vu0-image/vu0_image.bin"
)
if not "%VU0GEN_GIVEN%"=="" set "VU0GEN=%VU0GEN_GIVEN%"
cmake -S "%WT%" -B "%WT%\build\%DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
 "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7 -gline-tables-only" "-DCMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Z7 -gline-tables-only" ^
 "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%%_PDB%%" ^
 -DGT4_ASSETS_ROOT=%A% -DGT4_EE_PROFILE=%PROFILE% -DGT4_TARGET=%TARGET% ^
 -DGT4_HW_GS=ON -DGT4_HW_VULKAN_SDK=C:/VulkanSDK/1.4.341.1 -DGT4_HW_GLSLANG=C:/VulkanSDK/1.4.341.1/Bin/glslangValidator.exe ^
 -DGT4_IPU_RUNTIME_SOURCE=%A%/private/slice365-ipu-fast-runtime ^
 -DGT4_LICENSE_GUARD_SOURCE=%A%/private/slice376-frame-render-license-guard-runtime ^
 -DGT4_MEMCARD_RUNTIME_SOURCE=%A%/private/slice200-memcard-runtime ^
 -DGT4_PERF_RUNTIME_SOURCE=%RUNTIME% ^
 -DGT4_SPU2_RUNTIME_SOURCE=%A%/private/slice396-spu2-runtime ^
 -DGT4_PORT_DIR=%A%/private/slice200-port ^
 -DGT4_PYTHON=%A%/private/tooling-venv/Scripts/python.exe ^
 -DGT4_RELEASE_DISTRIBUTION=ON ^
 -DGT4_SDL3_ROOT=%A%/private/tooling/sdl3-3.4.18 ^
 -DGT4_SHADER_DIR=%A%/private/slice153-pacing-shaders ^
 -DGT4_VU0_GENERATED_DIR=%VU0GEN% ^
 -DGT4_VU0_IMAGE_FILE=%VU0IMG% ^
 -DGT4_VU1_GENERATED_DIR=%VU1GEN% ^
 -DGT4_VU_CORPUS_DIR=%A%/private/slice155-vu1-full-corpus %EXTRA%
exit /b %errorlevel%
