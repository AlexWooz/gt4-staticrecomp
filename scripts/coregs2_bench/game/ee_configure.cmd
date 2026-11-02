@echo off
rem usage: ee_configure.cmd <build dir under build\> <generated dir> <extra flags in one quoted argument>
setlocal
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
for %%I in ("%~dp0..\..\..") do set "WT=%%~fI"
set "A=C:/Antigravity/gt4-staticrecomp"
cmake -S "%WT%\tools\ee_producer" -B "%WT%\build\%~1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /clang:-msse4.1 /Z7 -gline-tables-only %~3" -DGT4_DONOR=%A%/private/references/ps2-web-recomp -DGT4_PYTHON=%A%/private/tooling-venv/Scripts/python.exe "-DGT4_GENERATED=%~2" -DGT4_BUILD_TRANSLATOR=OFF -DGT4_FAST_HEADERS=ON
exit /b %errorlevel%
