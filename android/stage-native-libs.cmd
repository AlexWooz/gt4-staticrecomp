@echo off
rem TASK-180: stage the native libraries and the SDL3 AAR for Gradle (nothing staged is committed).
rem usage: stage-native-libs.cmd <ABI> <android/native build folder>
rem   copies <build>\libgt4.so, the SDL3 3.4.18 libSDL3.so of that ABI and the NDK's libc++_shared.so into
rem   app\src\main\jniLibs\<ABI>\, and SDL3-3.4.18.aar into app\libs\.
setlocal
set "ABI=%~1"
set "BUILD=%~2"
if "%ABI%"=="" goto usage
if "%BUILD%"=="" goto usage
if "%GT4_ANDROID_NDK%"=="" set "GT4_ANDROID_NDK=%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125"
if "%GT4_ASSETS_ROOT%"=="" set "GT4_ASSETS_ROOT=C:\Antigravity\gt4-staticrecomp"
set "SDL=%GT4_ASSETS_ROOT%\private\tooling\sdl3-3.4.18-android"
if /i "%ABI%"=="x86_64" (set "TRIPLE=x86_64-linux-android") else (set "TRIPLE=aarch64-linux-android")
set "JNI=%~dp0app\src\main\jniLibs\%ABI%"
if not exist "%JNI%" mkdir "%JNI%"
if not exist "%~dp0app\libs" mkdir "%~dp0app\libs"
copy /y "%BUILD%\libgt4.so" "%JNI%\libgt4.so" >nul || exit /b 1
copy /y "%SDL%\aar\prefab\modules\SDL3-shared\libs\android.%ABI%\libSDL3.so" "%JNI%\libSDL3.so" >nul || exit /b 1
copy /y "%GT4_ANDROID_NDK%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\%TRIPLE%\libc++_shared.so" "%JNI%\libc++_shared.so" >nul || exit /b 1
copy /y "%SDL%\pkg\SDL3-3.4.18.aar" "%~dp0app\libs\SDL3-3.4.18.aar" >nul || exit /b 1
echo staged %ABI%: libgt4.so libSDL3.so libc++_shared.so and SDL3-3.4.18.aar
exit /b 0
:usage
echo usage: stage-native-libs.cmd ^<x86_64^|arm64-v8a^> ^<android/native build folder^>
exit /b 2
