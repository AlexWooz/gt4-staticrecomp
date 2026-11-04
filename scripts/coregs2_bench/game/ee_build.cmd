@echo off
setlocal
for %%I in ("%~dp0..\..\..") do set "WT=%%~fI"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
cmake --build "%WT%\build\%~1" --target ps2game
exit /b %errorlevel%
