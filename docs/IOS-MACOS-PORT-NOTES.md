# iOS and macOS port: notes for a later experimental port

Status: idea recorded on 2026-11-05 at the owner's request; not scheduled. The current phases are Phase 3 (native Linux)
and Phase 4 (Android). Android optimisation comes first. These notes are written so another agent can start the port
without this session's context.

## Goal (owner)
- An experimental iOS build for a friend's iPhone 17 Pro Max. The friend has a MacBook Air M1 to build on.
- Personal use only, installed directly. The friend already uses SideStore, StikDebug and LiveContainer and plays ARMSX2
  on iOS. An unsigned or ad-hoc `.ipa` is enough; no App Store.
- macOS on Apple Silicon comes as a side effect of the same work.

## Apple GPUs and why they suit CoreGS 2.0
- **Architecture.** Apple GPUs (M1 and later, A14 and later, including the A19 Pro in the iPhone 17 Pro Max) are
  tile-based deferred renderers with unified memory, like Android's Adreno and Mali. They are not like desktop
  immediate-mode GPUs. (High confidence.)
- **The key difference from the Android target.** Metal offers in-tile reads of the attachment in rasterization order:
  programmable blending (framebuffer fetch) and raster order groups.
  - These give CoreGS its exact, shader-side GS blending WITHOUT per-primitive barriers. That is the "interlock-like"
    path CoreGS uses on desktop GPUs with fragment shader interlock.
  - (High confidence on Metal's features; Unknown on how MoltenVK exposes them: check for
    `VK_EXT_fragment_shader_interlock` and `VK_EXT_rasterization_order_attachment_access` on MoltenVK.)
- **Contrast with the owner's S23.**
  - Its Adreno 740 driver (Vulkan 1.3, driver 0x802a4001, checked on 2026-11-05) has NONE of: rasterization-order
    attachment access, dynamic-rendering local read, fragment shader interlock, shader tile image.
  - So on Android, CoreGS takes its colour-attachment path, with about 750 fragment barriers per VBlank. The full list is
    in `private/android-test3/s23-vulkan-extensions.txt`; it is private and not in git.
  - Expectation: Apple devices should run CoreGS proportionally better than Android. (Hypothesis.)
- **ARMSX2 on iOS** uses Metal directly. MoltenVK is used only on macOS. Source:
  `docs/hw-renderer-spec/ORACLE-ARMSX2-MOBILE-GPU.md`, clean-room prose.

## What transfers between Android and Apple, and what does not
- **Transfers** (generic tile-GPU and unified-memory work, mostly done or planned for Android):
  - fewer render passes;
  - correct load and store ops (DONT_CARE or CLEAR where possible);
  - fewer full-image copies;
  - presenting straight into the swapchain image with pre-rotation;
  - unified-memory placement (`docs/hw-renderer-spec/STUDY-UNIFIED-MEMORY.md`);
  - asynchronous readbacks (`STUDY-ASYNC-READBACK.md`);
  - the barrier and deinterlace work (`STUDY-BARRIER-SCOPE.md`, TASK-182).
- **Does not transfer:** the in-tile ordered blending path is Apple-only, plus some Mali. Android without the extension
  needs tighter overlap tests (TASK-184) instead.
- **Consequence:** optimise Android on the Android phone itself, using bench mode. The friend's M1 becomes the lab for the
  Apple-specific parts once the Apple port exists.

## Port outline (to plan properly later)
1. **Platform layer.** Reuse the Linux/POSIX work (Phase 3, L0 platform seam: `native/platform/*`) and the Android lessons:
   - SDL3 window and input;
   - audio;
   - `frame_clock_posix`;
   - file access through the iOS document picker or the Files app instead of Android SAF;
   - a separate game process is NOT possible on iOS, so the game must run in-process with a clean restart strategy.
     This is a design question (Unknown).
2. **Build.**
   - The EE units, VU1 AOT and IOP are C++: compile them for arm64-apple-ios and arm64-apple-macos with clang.
   - Keep `-ffp-contract=off`.
   - No JIT anywhere: CoreGS and the recompiler are ahead of time, which matters on iOS, where JIT is not allowed for
     normal apps.
3. **Renderer.**
   - Start with MoltenVK (Vulkan over Metal).
   - Detect the interlock or raster-order capability and use CoreGS's exact interlock path.
   - A native Metal backend can come later if MoltenVK limits it.
4. **Launcher.** Tauri supports iOS and macOS. Reuse the Android work: no auto-update, the disc link, the settings, and
   CoreGS only.
5. **Limits.**
   - per-app memory limits on iOS: test on the real device, and ask about the increased-memory entitlement;
   - thermal throttling on the iPhone in long sessions;
   - the iOS simulator does NOT show real performance: measure on the device.

## Measurement
- Use the same bench mode as Android: `GT4_BENCH_CARD` + `GT4_INPUT_REPLAY` through a debug.env file, with the recorded
  drives in `private/coregs2-bench/inputs/`.
- Compare the race (licence B1 or a Nordschleife lap), the menus (480i) and the showroom at 1x, 2x and 3x, with the
  `GT4_CORE2_GPU_CATEGORIES` and `GT4_CORE2_BARRIER_TRACE` lines.
