# Android port: plan (TASK-022, CoreGS 2.0 only)

Status: **plan only** (owner, 2026-11-04). Phase 4 of the roadmap; the owner's long-term goal. Builds on the
[Linux plan](LINUX-PORT-PLAN.md) foundation (platform seam, determinism, Linux arm64 rehearsal). CoreGS 2.0 is the only
renderer; GSdx is Windows-only. Sources: four read-only audits of 2026-11-04 (game code, CoreGS 2.0, launcher, previous
plans) and the decision "efficiency first, mobile later" (ai-memory `decisions/efficiency-first-mobile`). Claims about
Android GPUs, drivers and store rules are general knowledge (Hypothesis) until measured on devices.

## Goal

Gran Turismo 4 (v2.00 and Spec II) running natively on arm64 Android phones and handhelds: the same game logic, CoreGS
2.0 on Vulkan, 60 FPS at 1x (2x on strong devices) with Performance Mode, a controller first, the player's own disc
image. No JIT is needed, which is a real advantage on Android (no W^X or code-download conflicts).

## What already works in our favour

- No Windows API and no x86 intrinsics in the game code; the VU output uses portable clang vector extensions.
- The EE FPU is integer soft-float; VU results go through integer flush/clamp helpers, so flush-to-zero modes do not change
  them; float-to-int conversions are masked against x86/arm64 saturation differences.
- Guest memory is a plain static array accessed through `memcpy` (unaligned access is safe on arm64); no page tricks, no
  `VirtualProtect`, no guard pages; the MTVU ring was already reviewed for weak ordering (TASK-092).
- Input is already SDL3, which has a mature Android backend (`SDLActivity`, controllers, sensors, audio).
- CoreGS 2.0 has a path without pixel interlock (IN-466/471) and a colour-attachment path (IN-481) for weaker GPUs.

## The hard parts, ranked

### 1. CoreGS 2.0 on tile-based mobile GPUs (the largest risk)

CoreGS 2.0 does the whole pixel stage in the fragment shader as a read-modify-write of `R32_UINT` storage images, inside a
render pass with no attachments. Mobile GPUs:
- **lack fragment shader interlock** (Mali, Adreno, PowerVR as far as known): the barrier-based ordering path becomes the
  default. It was tuned on desktop (6.85 ms vs 3.9 ms per VBlank at 3x on an RX 9060 XT) and never measured on a tiler,
  where in-pass framebuffer barriers without `BY_REGION` may flush tiles;
- get no tile-memory benefit from storage-image read-modify-write (all pixel traffic goes to memory; mutable formats also
  prevent the framebuffer compression Mali and Adreno use);
- pay for readbacks with fence waits on the EE's critical path (the night flare table), and for 284-1026 full barriers
  per VBlank and per-batch descriptor updates (about 443 batches per VBlank).

What it needs, in order:
1. Start-up capability probe and **per-vendor profiles** (Adreno, Mali, Xclipse, PowerVR) instead of today's
   `INTEGRATED_GPU` heuristic, which on Android would switch on defaults tuned on an Intel HD 620 (an immediate-mode GPU).
2. Measure the no-interlock path on a real Adreno and a real Mali with the recorded drives before any redesign.
3. `BY_REGION` dependencies where only same-pixel order matters; fewer full barriers.
4. The roadmap's **D1 path** (common draws through real colour/depth attachments, hardware blending where the GS blend is
   expressible; `COREGS2-ROADMAP.md:279`) as the main mobile workstream; storage-image fallback only for the GS cases
   hardware cannot express. The decision "no mobile-only paths yet" holds until the owner starts this phase; the probe
   and measurements come first.
5. Performance Mode on by default on phones (it already hides the flare readback); scale 1x default.

### 2. Vulkan correctness gaps that are bugs everywhere (fix on Windows now; not mobile-only)

- Features used but never enabled: `shaderStorageImageArrayDynamicIndexing` (`hw_deinterlace_scaled.comp:30,62`),
  `largePoints` (`hw_vertex.vert:67`, a POINT_LIST pipeline), and lines without the wide-line feature. Strict mobile
  drivers may misdraw or crash. Query, enable, or fall back (quad expansion from `hw_gs/decide`).
- Required formats that core 2 does not use (`D32_SFLOAT` depth attachment, blendable `R16G16B16A16_UNORM`) can refuse a
  device for nothing (`hw_gs_backend.cpp:78-89`): drop them.
- Vulkan 1.2 is required (`hw_gs_backend.cpp:583`); much of the Android installed base reports 1.1. Accept 1.1 with
  `VK_KHR_timeline_semaphore` (and what else 1.2 core is used for) if the device floor needs it.
- The swapchain uses `preTransform = currentTransform` but never rotates the content (`hw_gs_backend.cpp:895`): Android
  surfaces are often portrait-native, so the picture would be rotated or distorted. Use identity with a rotated blit, or
  handle the transform.
- Memory: about 400 MiB of mapped host buffers (256 MiB staging, 64 vertex, 16 uniform, 64 readback) before any image,
  sized by constants. Size the rings from the heaps and `VK_EXT_memory_budget`; Android's low-memory killer acts fast.
- Uploads require `HOST_COHERENT` memory and never flush; correct, but on Adreno/Mali the cached type may be non-coherent,
  so readbacks get slow: choose memory types per vendor.
- Interpolation exactness: depth from `floor(gl_FragCoord.z * 2^32)` and colour from `floor(interpolant + 0.001)`
  (`hw_pixel_uber.frag:582,587`) without `precise`/`invariant` in the vertex shader; mobile precision and FMA can move
  one-pixel ties. Golden frames per GPU family decide.

### 3. Determinism (game logic identical to Windows)

Shared with Linux: `-ffp-contract=off` everywhere (arm64 always has FMA; `vu_ops.hpp:304` fuses otherwise), the libm
calls in EFU/IPU paths (bionic differs from UCRT in the last bit), the IOP `strtol` `long` clamp, ThreadSanitizer and arm64
hardware runs for memory ordering, and the cross-platform identity test on the recorded drives. Proven first on **Linux
arm64** (stage L1b), where tools are easier than on a phone.

### 4. App structure: no second executable on Android

An Android app cannot start the game as a separate program. Design:
- **Launcher**: the existing HTML/JS UI through Tauri 2 Android (its own focus engine, `nav.js`, already drives the UI
  from keys and pads), with Rust core crates cross-compiled for `aarch64-linux-android`. Tauri mobile is less mature than
  desktop; file picking with persistent permissions needs a small Kotlin plugin. Fallback: a native Kotlin launcher calling
  the same Rust core through JNI.
- **Game**: a second Activity in **its own process** (`android:process=":game"`), hosting SDL3's `SDLActivity`, loading
  `libgt4.so` or `libgt4spec2.so`. A separate process keeps the game's global state fresh for each Play and lets the
  launcher survive a game crash.
- **Launch contract**: the environment-variable and `game.ini` contract is kept: the game Activity receives the values as
  Intent extras and sets them in its own process before the game starts. Logs go to app files.
- **Exit and crash reporting**: no exit codes. A result file written by the game on normal exit, `ApplicationExitInfo`
  and tombstones for crashes, and the existing log-based classification.

### 5. The disc image and storage

- The player picks the ISO through the Storage Access Framework; the app keeps a persistable permission and reads the
  file through a file descriptor (`ContentResolver.openFileDescriptor` + `pread`). Today both the launcher (`gt4l-disc`
  opens a `Path`) and the game (donor `disc.cpp:68` uses `fopen` on a path) need an fd-based reader. No copy (4+ GB).
- First hash of a 4 GB image over SAF is slow: hash once, then trust size and modification time (`DocumentsContract`
  queries) as the launcher already does with paths.
- FAT32 SD cards cannot hold a file of 4 GiB or more: explain, do not fail obscurely.
- Cards, saves, replays, settings and the shader cache live in app-specific storage; import/export through SAF.
- **Durability**: Android kills background apps without warning. The memory card must reach storage when the game saves
  (fsync the file and the directory: `memcard_store.cpp:65` renames without a directory sync today).

### 6. Lifecycle

`onPause`: stop the VBlank pump, mute and pause audio, stop haptics; `surfaceDestroyed`: drop the swapchain;
`onResume`/`surfaceCreated`: recreate it and continue. A process killed in the background loses unsaved progress (no
save states in this project): say so in the UI. Multi-window, foldables (resize, aspect changes mid-game), and the
display cutout (an option to keep the HUD clear of it, using TASK-080's parameterised HUD layout).

### 7. Performance, heat and threads

- Threads: EE (with an explicit large stack: today it runs on the Windows main thread's 1 MB; arm64 frames differ),
  VU1/MTVU worker, GS worker, audio, presentation. `hardware_concurrency()` counts little cores: size pools from big cores.
- Android Dynamic Performance Framework hint sessions for the EE and GS threads; sustained-performance mode; thermal
  status to step down (scale, Performance Mode, the 30 FPS mode).
- `fp4.hpp` needs a NEON form (the scalar fallback is bit-correct but costs about a fifth of EE time in races).
- The IOP and VU0 run on interpreters; profile them on a big core early (stage A2).
- Shader preparation: about 544 specialised pipelines of a very large shader on first run; driver caches are not
  portable and are invalidated by driver updates. A "preparing graphics" screen with background compilation and a
  persistent cache; watch for driver watchdogs on long compiles.

### 8. Audio, display and input

- Audio: SDL3 audio (AAudio). Bluetooth headsets add 150-250 ms: the pace stays VBlank-based; consider an A/V offset.
- Display: 90/120/144 Hz panels: present at 60 with the platform frame-rate API or Swappy/`VK_GOOGLE_display_timing`;
  the game pace never follows the display rate.
- Controllers: SDL3 on Android (Bluetooth and USB); the controller choice of rc12 carries over. Touch controls are a new
  feature (an on-screen DualShock 2 with analog throttle/brake, and optionally tilt steering through SDL3 sensors): owner
  decision on scope. Racing wheels over USB-OTG: out of scope (no force-feedback API for wheels on Android).

### 9. Toolchain and distribution

- NDK clang (LLVM family like our clang-cl 22; the FP flags, not the version, carry determinism), CMake + Ninja, `arm64-v8a`
  only, **16 KB page alignment** for every native library (`-Wl,-z,max-page-size=16384`: the game, Rust, SDL3), current
  `targetSdk`.
- Two game libraries of roughly the Windows executables' size (94-102 MB each before compression), plus the launcher.
- First channel: **APK on GitHub** (sideload; updates through `PackageInstaller` with the user's consent; `SHA256SUMS`).
  Play Store later, if ever: no runtime code downloads (our renderer is built in, fine), the size limits (Play Feature
  Delivery per game if needed), and the store's stance on apps that need a game disc image (a policy risk to assess).
- Discord: none on Android. RetroAchievements: token in the Android Keystore.

## Stages

| Stage | Content | Gate |
|---|---|---|
| A0 | Linux L0-L1b done: platform seam, determinism foundation, Linux arm64 identity, NEON `fp4` | arm64 identity on the recorded drives |
| A1 | Fix the Vulkan correctness gaps on Windows (missing feature enables, vestigial formats, rotation-safe present, budget-sized rings, per-vendor profile structure) | Windows identity and frames unchanged |
| A2 | Android skeleton: NDK build of `libgt4.so`, SDL3 game Activity in its own process, Android surface, boot to the title screen at 1x with CoreGS 2.0; IOP/VU0 interpreter and EE profile on a big core | boots on an Adreno and a Mali device |
| A3 | Storage: SAF disc picker, fd-based reader in launcher and game, app storage for cards and caches, durable card writes | Spec II and v2.00 reach a race from the player's ISO |
| A4 | Correctness on mobile GPUs: golden frames of the recorded drives against Windows, per vendor; fix driver issues behind per-vendor switches | picture parity on Adreno and Mali |
| A5 | Performance: measure the no-interlock path, BY_REGION and barrier reduction, then the D1 attachment path; Performance Mode default; ADPF and thermal steps | 60 FPS at 1x in races on the target device class, stable over 20 minutes |
| A6 | Lifecycle, audio, input, display (pause/resume, Bluetooth audio, 120 Hz panels, cutouts, controller choice, optional touch) | owner checklist |
| A7 | Launcher on Android (Tauri or native), shader preparation screen, settings subset for phones | focus checks with a controller |
| A8 | Packaging: signed APK, 16 KB alignment check, update flow, release assets | sideload install and update on two devices |
| A9 | Owner test and a closed beta | owner approval |

## Stage A2 progress (TASK-180, from 2026-11-04)

Started on the Android Studio emulator (x86_64, API 36) before any phone, with the owner's approval. Procedure, the
download list, the AVD and the adb commands: [ANDROID-EMULATOR](ANDROID-EMULATOR.md).
- **Seam:** the Linux plan's L0 layer, done for Android first (see LINUX-PORT-PLAN, stage L0); Windows unchanged.
- **Build:** `android/native/CMakeLists.txt` builds the whole game as `libgt4.so` with NDK 26.1 (API 29, CMake + Ninja
  outside Gradle): the translated v2.00 units (rc14b), the pinned donor runtime and capsules (checked against
  `docs/inputs`), VU1/VU0 AOT, CoreGS 2.0, SDL3 3.4.18 from the official AAR. Flags pinned for determinism and ABI parity
  with clang-cl: `-ffp-contract=off`, `-fno-strict-aliasing`, `-fsigned-char`; 16 KB page alignment for every ABI.
- **App:** `android/` holds a minimal Gradle project (AGP 9.5.0, Gradle 9.8.2): one `GameActivity` extending
  `SDLActivity` in its own process (`:game`), landscape, Vulkan, controller; the native libraries are staged into it.
- **arm64-v8a probe (2026-11-05):** every native library (runtime and capsules, VU1/VU0 AOT, CoreGS 2.0, the GS seam,
  host I/O), the entry sources and a sample of translated units (with `table.cpp` and the hooked units) compile for
  `aarch64-linux-android29` with no change: `fp4.hpp` takes its scalar form (bit-identical, slower; NEON is still to
  write), `core2_upload.cpp` its new portable column store, the perf clock the generic timer. The full arm64 link was
  not run (disk); the x86_64 link has no undefined symbols. Build command: `android/README.md`.
- **First boot (2026-11-05):** on the emulator with Lavapipe (CPU Vulkan) the game boots, plays the opening movie
  (IPU video), takes Start from the emulator keyboard and reaches the GT Mode home at 1x with CoreGS 2.0, in landscape,
  with SDL3 audio (AAudio). Host-GPU emulation crashes inside AMD's OpenGL driver on this desktop (details in
  ANDROID-EMULATOR); SwiftShader lacks `dualSrcBlend`, which CoreGS 2.0 requires.
- **Learned so far:** the donor runtime and the translated units need no source change for Android x86_64; CoreGS 2.0
  needed only the surface/loader seam and POSIX forms of three diagnostics; one LP64 fix in the pinned runtime capsule
  (configure-time copy, see LINUX-PORT-PLAN); the SDL3 AAR ships its libraries as prefab modules only, so they are staged
  into `jniLibs` by hand. Orientation: SDLActivity overrides the manifest's orientation from `SDL_HINT_ORIENTATIONS`
  (a resizable window without it gets "any orientation"); this Android 16 image reports the window's own size as
  `currentExtent` with `currentTransform` = rotate 90 in landscape, so the identity pre-transform with the extent as
  reported is right (the picture was turned with `preTransform = currentTransform`, and squashed when the extent was
  swapped). That confirms TASK-179's rotation-safe present; the Android-only branch in `hw_gs_backend.cpp` gives way to it.
  The EE thread is SDL's game thread, with a 1 MB stack (as the Windows main thread); enough for the boot.

## Subtle risks (Android), ranked

1. The no-interlock CoreGS path is slow or misordered on tilers; D1 may be required before playable speed.
2. Desktop-tuned "integrated GPU" defaults switching on by accident on every phone.
3. Strict drivers misdrawing because of features used without being enabled.
4. Landscape picture rotated or squashed by the unrotated `preTransform`.
5. FP contraction or bionic libm changing game results silently (identity test is the guard).
6. Weak memory ordering exposing a missing acquire/release in the GS or VU hand-offs.
7. EE thread stack overflow on arm64 in very large translated functions.
8. Low-memory kills from 400 MiB of fixed rings plus upscaled images on 6-8 GB phones.
9. Progress lost when Android kills the backgrounded game before the card reaches storage.
10. Long first-run pipeline compiles hitting driver watchdogs; caches lost after driver updates.
11. SAF quirks: cloud providers without seekable fds, revoked permissions, FAT32 limits.
12. Thermal throttling turning a 60 FPS race into 45 after ten minutes.
13. 16 KB page devices refusing to load a library built with 4 KB alignment.
14. Store policy for disc-image-based games.

## What can be done now on Windows without breaking the "PC first" rule

Correctness and portability work that helps the PC too: `-ffp-contract=off` by default, the missing Vulkan feature
enables, dropping vestigial format requirements, budget-sized rings, the directory fsync for cards, the IOP `strtol` clamp,
the platform seam (L0), keeping new code free of Win32 and x86-only constructs. No mobile-only renderer paths until the
owner starts Phase 4.

## Owner decisions needed

Target device floor (for example Snapdragon 8 Gen 1 / Dimensity 9000 / Exynos 2200 class and newer), whether Spec II and
v2.00 ship together, touch controls scope, Tauri or native launcher, GitHub APK only or Play Store too, and which test
devices (at least one Adreno and one Mali).
