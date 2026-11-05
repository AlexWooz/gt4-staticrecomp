# Android emulator: build, install and run (TASK-180, Android plan stage A2)

Status: first part of TASK-180 (2026-11-04). How to build `libgt4.so` with the NDK, package the game activity, and run
it in the Android Studio emulator on the desktop (Ryzen 7 5700X, RX 9060 XT). Plans: [ANDROID-PORT-PLAN](ANDROID-PORT-PLAN.md),
[LINUX-PORT-PLAN](LINUX-PORT-PLAN.md) (the platform seam, stage L0). Nothing here is a release path: no store, no
signing beyond the debug key, the player's files pushed with adb.

## What was downloaded (owner approval of 2026-11-04) and why

| Item | Exact name and source | Size | Why |
|---|---|---|---|
| Emulator system image | `system-images;android-36;google_apis;x86_64` revision 7, Google's SDK repository, installed with `sdkmanager --install` | 4.3 GB unpacked | Android 16 (API 36) on x86-64: the emulator runs it with WHPX at native speed; it matches the installed `platforms;android-36`, and Google APIs (no Play Store) keeps root over adb for logs. The SDK licence was already accepted on this machine; no prompt appeared. |
| SDL3 3.4.18 for Android | `SDL3-devel-3.4.18-android.zip` from the libsdl-org GitHub release `release-3.4.18` (sha256 `e09e4d6593335d089ba124375e8b8abd0fd75173dffcd2c17012b45af121d4e9`, equal to GitHub's digest; git hash `829a65d769d935c4852f8159e964312c0957260a`, the same as our Windows pin `docs/inputs/slice210-sdl3.json`) | 16.7 MB | The official AAR: `libSDL3.so` for every ABI (16 KB aligned), the `SDLActivity` Java classes, the headers. Unpacked under `private/tooling/sdl3-3.4.18-android/` (`pkg/` the AAR, `aar/` its contents); never committed. |
| Gradle | `gradle-9.8.2-bin.zip` (sha256 `21bd5da2de49b642904077f38c0efdc7778e92b3ed0889392dca61b97e389607`, from services.gradle.org, served by the gradle/gradle-distributions GitHub release) | 152 MB (164 MB unpacked) | Builds the APK; Android Studio 2026.2.2's JBR is Java 25, which needs Gradle 9.1 or later. Used once to write the wrapper (`android/gradlew`); the wrapper then keeps its own copy in `%USERPROFILE%\.gradle`. |
| Android Gradle Plugin | `com.android.application` 9.5.0 (latest stable on Google's Maven on 2026-11-04) with its dependencies, from Google's Maven and Maven Central, fetched by Gradle | a few hundred MB in `%USERPROFILE%\.gradle` | Packages the APK. `buildToolsVersion "36.1.0"` keeps it on the installed build tools (no extra SDK download). |

Already installed before (checked): platforms android-34, 36, 36.1; build-tools 34.0.0, 36.1.0, 37.0.0; NDK
26.1.10909125; CMake 3.22.1; platform-tools; emulator 37.2.13. **The Windows Hypervisor Platform is already on**
(`emulator-check accel`: "WHPX(10.0.26300) is installed and usable"; Hyper-V enabled): nothing for the owner to enable.

## The AVD

Created with `avdmanager create avd -n GT4Recomp_API36_x86_64 -k "system-images;android-36;google_apis;x86_64" -d pixel_7`
with `ANDROID_AVD_HOME=F:\GT4Recomp-android\avd` (owner rule: only the few large image files on the HDD; builds and the
Gradle home stay on the SSD). `config.ini` changes:

| Setting | Value | Why |
|---|---|---|
| `hw.gpu.enabled`, `hw.gpu.mode` | `yes`, `host` | Vulkan through gfxstream on the host GPU (the RX 9060 XT); `swiftshader_indirect` would be CPU Vulkan, far too slow |
| `hw.ramSize` | 8192 | the game process, ~400 MiB of CoreGS host rings and the system; the host has 64 GB |
| `hw.cpu.ncore` | 8 | EE, VU1 worker, GS worker, audio and the system |
| `hw.initialOrientation` | `landscape` | the game activity is landscape-only |
| `hw.keyboard`, `hw.dPad` | `yes` | the host keyboard reaches the game as keys (arrows, Z/X/A/S, Enter) |
| `disk.dataPartition.size` | 12G | the 5.3 GB disc image, the ELF, the card and the app |
| `hw.sdCard` | `no` | `/sdcard` is the emulated storage on the data partition; no separate image |
| snapshots | off (cold boot, no save) | a snapshot would write 8 GB of RAM to the HDD at each exit |
| `showDeviceFrame` | `no` | |

Start it (no system setting changes; the variables are per command):

```
set ANDROID_AVD_HOME=F:\GT4Recomp-android\avd
%LOCALAPPDATA%\Android\Sdk\emulator\emulator.exe -avd GT4Recomp_API36_x86_64 -gpu host -feature Vulkan -no-snapshot -no-boot-anim
```

## Build

1. Windows tree first (its CoreGS 2.0 shaders are reused as embedded C++; the SPIR-V is the same on every platform):
   any configured build with `GT4_HW_GS=ON`, for example `build\t180-v200`.
2. `libgt4.so` for the emulator (x86_64) or a phone (arm64-v8a), with the NDK:
   ```
   set GT4_EE_UNITS_DIR=C:/Antigravity/gt4-wt-180-android/build/android-inputs/v200-units-rc14b
   set GT4_HW_SHADER_CPP_DIR=C:/Antigravity/gt4-wt-180-android/build/t180-v200/native/hw_gs/shaders
   android\native\configure-android.cmd x86_64 build\android-x86_64
   ```
   Inputs are the pinned capsules of the rc14 v2.00 recipe (checked against `docs/inputs` at configure) and a copy of
   the translated v2.00 units (rc14b, the hooked units checked against
   `private/slice408-ee-v200/v200-units-rc14b.camera360-units.json`). Flags: API 29, `-ffp-contract=off`,
   `-fno-strict-aliasing` (as clang-cl), `-fsigned-char` (as Windows), SSE4.1 on x86_64, `-z max-page-size=16384`.
3. Stage and package:
   ```
   android\stage-native-libs.cmd x86_64 build\android-x86_64
   set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
   set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
   cd android && gradlew.bat assembleDebug
   ```
   The APK is `android\app\build\outputs\apk\debug\app-debug.apk`. `gradle/wrapper/gradle-wrapper.jar` is not committed
   (a third-party binary): on a fresh checkout run `gradle wrapper` once in `android\` with any Gradle 9.x (the
   unpacked 9.8.2 distribution works) before `gradlew`.

The app's name and icon are the PC game's (owner, 2026-11-05): "Gran Turismo 4" and the art of
`launcher/assets/brand/launcher.ico`, redrawn at the Android sizes from its source (`logo-gt4.png`) by
`android/tools/make_icons.py` (Pillow from the tooling venv): legacy `mipmap-*/ic_launcher.png` and an adaptive icon
(white background, emblem foreground, also the themed icon).

## Install and first run (adb)

The game reads its files from the app-specific external folder; adb can write there without root.

```
set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
%ADB% install -r android\app\build\outputs\apk\debug\app-debug.apk
%ADB% shell mkdir -p /sdcard/Android/data/io.github.moesuito.gt4recomp/files
%ADB% push "C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 (USA) (v2.00).iso" /sdcard/Android/data/io.github.moesuito.gt4recomp/files/disc.iso
%ADB% push C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf /sdcard/Android/data/io.github.moesuito.gt4recomp/files/SCUS_973.28.elf
rem optional: a memory card (a copy, never the owner's own card file), and settings
%ADB% push <a copy of a card.ps2> /sdcard/Android/data/io.github.moesuito.gt4recomp/files/card.ps2
%ADB% push gt4.env /sdcard/Android/data/io.github.moesuito.gt4recomp/files/gt4.env
%ADB% shell am start -n io.github.moesuito.gt4recomp/.GameActivity
%ADB% logcat -s GT4Recomp SDL SDL/APP
%ADB% pull /sdcard/Android/data/io.github.moesuito.gt4recomp/files/game.log
%ADB% shell am force-stop io.github.moesuito.gt4recomp
```

`gt4.env` holds `NAME=VALUE` lines set before the game starts (the launcher's environment contract): for example
`GT4_HW_SCALE=1`, `GT4_PERF=1`, `GT4_HW_DUMP_DIR=/sdcard/Android/data/io.github.moesuito.gt4recomp/files/frames`.
`args.txt` adds command-line arguments, one per line (for example `--frames` and `3000` for a bounded run). Defaults:
CoreGS 2.0 (`GT4_RENDERER=hw`, `GT4_HW_CORE=2`), scale 1, 480p races, 16:9, FIFO.
