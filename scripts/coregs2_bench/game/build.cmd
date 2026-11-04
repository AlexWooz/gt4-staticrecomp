@echo off
rem usage: build.cmd <build dir under the worktree's build\> [targets]
setlocal
for %%I in ("%~dp0..\..\..") do set "WT=%%~fI"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "DIR=%~1"
shift
if "%~1"=="" (
  cmake --build "%WT%\build\%DIR%" --target gt4vulkan
) else (
  cmake --build "%WT%\build\%DIR%" --target %1 %2 %3 %4
)
exit /b %errorlevel%
