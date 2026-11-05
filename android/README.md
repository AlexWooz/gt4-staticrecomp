# GT4Recomp for Android: how the APK is built

TASK-180 (Android plan stage A2). The game for Android is one native library, `libgt4.so`, inside a small Gradle app
whose only activity is SDL3's `SDLActivity`. Procedure for the emulator, the downloads and the adb commands:
[docs/ANDROID-EMULATOR.md](../docs/ANDROID-EMULATOR.md). Nothing game-derived is committed: the translated units, the
capsules, the generated VU code, the built libraries and the APK stay under `build/` or `private/`.

## Toolchain (exact versions)

| Piece | Version | Where |
|---|---|---|
| NDK | 26.1.10909125 | `%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125` |
| CMake, Ninja | CMake 3.22+ (4.3.1 used), Ninja 1.13 | PATH |
| Android platform, build-tools | android-36, build-tools 36.1.0 | SDK |
| JDK | Android Studio's JBR (Java 25) | `C:\Program Files\Android\Android Studio\jbr` |
| Gradle, Android Gradle Plugin | 9.8.2 (wrapper, sha256 pinned in `gradle-wrapper.properties`), AGP 9.5.0 | `gradlew` |
| SDL3 | 3.4.18, `SDL3-devel-3.4.18-android.zip` (sha256 `e09e4d6593335d089ba124375e8b8abd0fd75173dffcd2c17012b45af121d4e9`, git `829a65d7`); its `SDL3-3.4.18.aar` sha256 `bea5cbd6ec56a247196d70c3626860e0d6f36163e562e20a33041bee2fb339f8` | `private/tooling/sdl3-3.4.18-android/` |
| Windows build (for the CoreGS 2.0 shaders) | any `GT4_HW_GS=ON` build of the same commit | e.g. `build/t180-v200` |

## Build (x86_64 emulator; `arm64-v8a` for phones the same way)

From the repository root, in a normal `cmd` (the NDK brings its own clang):

```
rem 1. The translated v2.00 units (rc14b, hooked) and the CoreGS 2.0 shaders as C++ from the Windows build.
set GT4_EE_UNITS_DIR=C:/Antigravity/gt4-wt-180-android/build/android-inputs/v200-units-rc14b
set GT4_HW_SHADER_CPP_DIR=C:/Antigravity/gt4-wt-180-android/build/t180-v200/native/hw_gs/shaders
rem 2. libgt4.so (configure checks every capsule against docs/inputs and prints the units' combined SHA-256).
android\native\configure-android.cmd x86_64 build\android-x86_64
rem 3. Stage libgt4.so, libSDL3.so, libc++_shared.so and the SDL3 AAR into the Gradle project.
android\stage-native-libs.cmd x86_64 build\android-x86_64
rem 4. The APK (debug-signed with the machine's Android debug key).
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
cd android
gradlew.bat assembleDebug
```

Output: `android\app\build\outputs\apk\debug\app-debug.apk`. On a fresh checkout create the wrapper jar once first
(`gradle wrapper` in `android\` with Gradle 9.x; the jar is not committed).

Inputs of the first boot build (2026-11-05, commit `d82dbae5`): units combined SHA-256
`a60221e7fe5e9af41f1bc1ae9646271b5812d4c6c2583e515213691844359f51` (327 files; list in
`build/android-x86_64/ee-units.sha256`), runtime capsule `slice262-frame-cost-runtime` (its `vu_micro.cpp` compiled from
the LP64 copy, sha256 `7385efa2...a4c267`), guard `slice376`, memcard `slice200`, IPU `slice365`, SPU2 `slice396`, VU1
`slice397-vu1-gen`, VU0 `slice394-v200-vu0-gen`, port `slice200-port`. Results: `libgt4.so` sha256
`e18c100d5807a1a5bbdb06344e98f5f81e18fe265c06f0dc826cb8cdb0b3cc67`, APK `73c88435268151b6626a82014de80ca00e04e4262710dd097cfeddd710c92b84`
(x86_64 only). Builds are not bit-reproducible across machines yet (paths and the debug key are in the outputs).

## Before an APK goes up as a GitHub draft

- arm64-v8a: configure and stage `arm64-v8a` too (both ABIs end up in one APK); the arm64 libraries compile today.
- A release signing key (not the machine's debug key), kept out of git; `assembleRelease` with it.
- `versionCode`/`versionName` in `app/build.gradle` from the release plan; launcher notes as for the PC releases.
- The player's files: the first stage reads them from the app's own folder, pushed with adb; a file picker (Storage Access
  Framework) is stage A3.
