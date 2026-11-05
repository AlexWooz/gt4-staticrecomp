# Android emulator: build, install and run (TASK-180, Android plan stage A2)

Status: TASK-180 (2026-11-04/05). The first APK (launcher first, SAF disc pick, Play) runs in the emulator: the game reaches its menus and saves with CoreGS 2.0 at 1x, on CPU Vulkan
(Lavapipe) for now. How to run the app in the Android Studio emulator on the desktop (Ryzen 7 5700X, RX 9060 XT); the
APK build is in `android/README.md`. Plans: [ANDROID-PORT-PLAN](ANDROID-PORT-PLAN.md),
[LINUX-PORT-PLAN](LINUX-PORT-PLAN.md) (the platform seam, stage L0). Nothing here is a release path: no store, and the
APKs are test builds.

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
| `hw.gpu.enabled`, `hw.gpu.mode` | `yes`, `host` | meant for Vulkan through gfxstream on the RX 9060 XT; that mode crashes here (below), so the runs pass `-gpu lavapipe` |
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
%LOCALAPPDATA%\Android\Sdk\emulator\emulator.exe -avd GT4Recomp_API36_x86_64 -gpu lavapipe -no-window -no-snapshot -no-boot-anim -crash-report-mode never
```

**Host GPU mode does not work on this desktop yet (2026-11-05).** `-gpu host` (Vulkan through gfxstream on the RX 9060 XT,
which the emulator detects and selects) crashes the emulator during boot inside AMD's OpenGL driver (`atio6axx.dll`,
seen in the emulator's minidump with cdb); the host side of the emulator still uses OpenGL for its composition. The same
happens without a window (`-no-window`); with `-feature -HostComposition` the guest hangs at boot; `-feature ForceANGLE`
falls back to SwiftShader. `-crash-report-mode never` keeps the emulator from asking to send a crash report.
- `-gpu swiftshader` boots, but SwiftShader lacks `dualSrcBlend`, so CoreGS 2.0 refuses the device (clear message).
- `-gpu lavapipe` (Mesa's CPU Vulkan for the guest, SwiftShader for the system's GLES) works: **the game boots, plays
  the opening movie and reaches the menus (GT Mode home) at 1x with CoreGS 2.0**, in landscape. Speed is software-render
  speed: about 36 VBlanks/s in the opening movie and 11 in the GT Mode home on the 5700X (the game's own pacing clock;
  the F8 probe's TSC-calibrated clock reads about twice too fast inside the emulator).
- Still to try for host-GPU speed: a newer or older AMD driver, or an emulator release whose host composition uses
  Vulkan; a real phone (Adreno/Mali) is the target anyway.

Screens and the game log of that run: `build/evidence/t180-emulator/` in the TASK-180 worktree (not committed).

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
3. Stage and package: `android/README.md` (the launcher's Gradle project, `cargo tauri android build`). For the emulator,
   `--target x86_64` with an x86_64 `libgt4.so`, or the arm64 APK itself: this Android 16 image runs arm64 apps through
   its ARM translation (`ro.dalvik.vm.native.bridge=libndk_translation.so`), slower but enough for the app's flows.

The app's name and icon are the PC game's (owner, 2026-11-05): "Gran Turismo 4" and the art of
`launcher/assets/brand/launcher.ico`, redrawn at the Android sizes from its source (`logo-gt4.png`) by
`android/tools/make_icons.py` (Pillow from the tooling venv): legacy `mipmap-*/ic_launcher.png` and an adaptive icon
(white background, emblem foreground, also the themed icon).

## Install and first run (the player's way, no adb push)

Since the first APK (2026-11-05) the app starts like on a phone: the launcher opens, the player picks the disc image
with the system's file picker, the launcher sets the game up, and Play starts it. adb is only the installer and the
log reader here:

```
set ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
rem the disc image where a player keeps it (any folder the picker shows; Downloads here)
%ADB% push "C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 (USA) (v2.00).iso" "/sdcard/Download/Gran Turismo 4 (USA) (v2.00).iso"
%ADB% install -r <the APK>
%ADB% shell am start -n io.github.moesuito.gt4recomp/.MainActivity
%ADB% logcat -s GT4Recomp-launcher GT4Recomp-game
```

Then on the screen: Choose disc image, the menu, Downloads, the ISO; setup checks it in place (about 90 s for the
5.3 GB image under the arm64 translation); Finish; Play. An APK signed with another key (the old debug skeleton) must be
uninstalled first; its leftover `Android/data/<package>` files may need `adb root` and `rm -rf` to free the space.
From Git Bash set `MSYS_NO_PATHCONV=1`, or `/sdcard/...` paths are rewritten. Keys: the emulator keyboard reaches the
game (arrows, Enter = Cross); `adb shell input keyevent KEYCODE_ENTER` works too. Screens:
`adb exec-out screencap -p > shot.png`.

Result of the first APK on this emulator (arm64 build, translated): launcher in landscape with the PC look; the picker
returned the document with a persistable permission; setup identified and hashed the image through the descriptor
(pins matched, "Gran Turismo 4 (USA v2.00)"); Play started the game process with the launch contract; the game booted,
created its game data on the card ("Save successful") at 1x with CoreGS 2.0 on Lavapipe. A first build crashed in the
launcher's Vulkan probe under the translation (a Vulkan 1.1 call on a 1.0 instance, now not made off Windows): the crash
was recorded with its tombstone at the next start, as designed.
