# Android performance plan (research, 2026-11-05)

Scope: make `libgt4.so` fast on arm64 phones; first target Galaxy S23 Ultra (Snapdragon 8 Gen 2: Cortex-X3 + A715/A710 + A510,
Adreno 740, Android 14+), goal 60 FPS in races at 2x, 3x if possible. Read-only research; no code was changed.
Labels: **Confirmed** = read in the cited official page; **High** = standard, well documented, not re-read here;
**Hypothesis** = plausible, must be measured on the device. "Bit-identical" = cannot change game results by itself.

Repo facts used (worktree `gt4-wt-180-android`, branch `task/180-android-emulator`):
- `android/native/CMakeLists.txt` already pins `-ffp-contract=off -fno-strict-aliasing -fsigned-char -gline-tables-only`,
  `-Wl,-z,max-page-size=16384`; `configure-android.cmd` builds Release with `-O2`, `ANDROID_STL=c++_shared`, API 29, NDK 26.1.
  No `-march/-mtune`, no LTO, no visibility flags on Android (the Windows build uses `/clang:-flto=thin`).
- `native/fp4.hpp`: SSE4.1 path plus a scalar `fp::add/sub/mul` fallback (what Android runs today). The SSE code is pure
  integer (flush, 24x24 multiply, leading-bit search, per-lane shift); its only float ops are two exact int-to-float
  conversions used to find an exponent.
- `native/hw_gs/core2/core2_gpu.cpp:661`: the ROP image is created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (R32_UINT +
  RGBA8 view) when `ropOn`. `hw_gs_backend.cpp:640` already lists `VK_EXT_rasterization_order_attachment_access`
  as optional; pipeline cache is persisted (`core2_gpu.cpp:1122-1253`).
- `native/thread_placement.hpp` is a Windows-only bench switch (`GT4_THREAD_CPUS`); `core2_gpu.cpp:2342` sizes a pool from
  `hardware_concurrency()` (counts little cores).

## 1. Compiler and linker (arm64)

1.1 **Baseline ISA is Armv8.0.** The NDK `arm64-v8a` ABI is "Armv8.0 only"; NEON is always there; PAC/BTI are optional
    ([NDK ABIs](https://developer.android.com/ndk/guides/abis)). Confirmed.
1.2 **One library, tuned, not two.** Recommended order: (a) build baseline `-march=armv8-a -mtune=cortex-x3`
    (scheduling model of the big core; `-mtune` never changes semantics); (b) A/B `-march=armv8.2-a` (adds LSE atomics, which
    shorten the seq_cst handshakes in `mtvu_ring.hpp`; every Snapdragon 855+/Exynos 9820+ has it). If (b) wins, guard it with
    a start-up `getauxval(AT_HWCAP)` check (`HWCAP_ATOMICS`) that shows "device not supported" instead of SIGILL.
    Hypothesis (gain small; code is integer and memory bound). Bit-identical.
    Avoid `-mcpu=cortex-x3` as a whole (turns on Armv9 + SVE2 codegen; the X3 vector length is 128 bit, so SVE2 gives no
    width over NEON, and a build tied to X3 breaks on A7xx-less phones). High/Hypothesis.
1.3 **Shipping both a generic and an S23-class build**: only if 1.2 shows more than ~5%. Options, cheapest first:
    two `.so` files chosen with `getauxval` before `dlopen` (cost: another ~95-100 MB per target in the APK, so probably no);
    per-translation-unit runtime dispatch for the few hot kernels (NEON `fp4` needs only base NEON, so it needs no dispatch);
    clang function multiversioning on AArch64 (`target_clones` / `target_version`) depends on NDK clang version and bionic
    ifunc support: Hypothesis, verify on NDK r28+ before relying on it.
1.4 **Upgrade the NDK.** NDK r26.1 (clang 17) needs manual 16 KB flags; r28+ is 16 KB aligned by default, and Google Play
    requires 16 KB support for targetSdk 35+ updates from 2027-02-01
    ([page sizes](https://developer.android.com/guide/practices/page-sizes)). Confirmed. Newer clang also gives better
    AArch64 codegen (Hypothesis). Re-run the identity drives after any compiler change.
1.5 **16 KB pages: add `-Wl,-z,common-page-size=16384`** next to `max-page-size`, and check RELRO with
    `llvm-readelf -Wl libgt4.so | grep 'LOAD\|RELRO'` (the Android doc says NDK r27 and lower may leave RELRO end
    unaligned and crash on 16 KB devices). Confirmed. S23 itself is a 4 KB kernel; this is for the A8 gate.
1.6 **LTO / ThinLTO.** Windows already links with ThinLTO. Add `-flto=thin` + `-Wl,--thinlto-cache-dir` on Android, plus
    `-fvisibility=hidden -fvisibility-inlines-hidden` (export only the SDL/JNI entry points), `-fno-semantic-interposition`,
    `-Wl,--gc-sections -Wl,--icf=safe`, and `-fno-plt` (fewer PLT stubs for libc/libm calls). Sources: general clang/lld
    behaviour ([clang UsersManual](https://clang.llvm.org/docs/UsersManual.html)); gain High-uncertain for the 326
    generated units (code size/icache is the likely cost). Bit-identical as long as `-ffp-contract=off` stays and no
    fast-math flag is added; re-run the identity drives because inlining exposes any latent UB.
1.7 **-O2 vs -O3.** Keep `-O2` (Windows uses /O2, so A/B stays comparable); try `-O3` only as one variable per build; huge
    switch-based units usually lose to icache pressure. Hypothesis. Never `-Ofast`, `-ffast-math`, `-funsafe-math-optimizations`,
    `-ffinite-math-only` (they change float results). Add `-Wfloat-conversion`-style review, not new flags.
1.8 **PGO.** Windows rc builds already use PGO, so the Android profile must be separate (arm64-specific paths, `fp4` NEON).
    Workflow ([NDK PGO guide](https://developer.android.com/ndk/guides/pgo)), Confirmed:
    - build with `-fprofile-generate` (compiler and linker), controlled by a CMake variable;
    - on Android `atexit` never runs: the app must call `__llvm_profile_set_filename("<app files dir>/default-%m.profraw")`
      then `__llvm_profile_write_file()` itself (hook to a debug key or to onPause after a recorded drive);
    - `adb pull` the `.profraw` (path must be writable by the app; the guide gives a no-root recipe), merge with the NDK's own
      `llvm-profdata merge`, rebuild with `-fprofile-use=x.profdata` in compiler and linker; use the same NDK release for
      clang and llvm-profdata. Typical gain 10-20% on interpreter-like code (Hypothesis). Bit-identical.
    Collect it on the S23 (instrumented code is 2-3x slower, so use a short recorded drive such as the George V lap).
1.9 Optional: `-mbranch-protection=standard` (PAC/BTI, about 1% size, only active on Armv9 phones)
    ([NDK ABIs](https://developer.android.com/ndk/guides/abis)): security, not speed; skip until the Play stage.

## 2. NEON for `fp4` (bit-exact)

2.1 **Use ACLE intrinsics (`<arm_neon.h>`), not clang vector extensions.** The SSE file is already written as integer
    intrinsics on `__m128i`; a 1:1 `uint32x4_t` port keeps the structure reviewable and each op maps to a defined
    instruction. Vector extensions leave instruction choice to the compiler and add nothing here. High.
2.2 **Mapping (all integer, no float needed)**: `_mm_blendv_epi8` -> `vbslq_u32`; flush mask -> `vceqq_u32(vandq_u32(x,0x7F800000),0)`;
    24x24 products -> `vmull_u32(vget_low_u32 ..)` and `vmull_high_u32` (four 64-bit products, no even/odd shuffle);
    the `power_of_two`/`mullo` trick for variable shifts -> `vshlq_u32(x, vnegq_s32(n))` (per-lane signed shift, exact); the
    leading-bit search, which the SSE path does through `cvtepi32_ps`, -> `vclzq_u32` (exact and simpler);
    `_mm_min_epi32` -> `vminq_s32`; compares -> `vcgtq_s32`. High (instruction semantics are in the Arm ACLE/Neon reference;
    the fixture must prove it).
2.3 **FPCR/denormals.** Because the whole kernel is integer, FPCR cannot change its results; that is the safest design and the
    reason not to "optimise" it with `vmulq_f32`/`vaddq_f32`. On AArch64 (unlike AArch32) Advanced SIMD float ops obey FPCR.FZ
    and FPCR.DN ([FPCR reference](https://developer.arm.com/documentation/ddi0595/latest/AArch64-Registers/FPCR--Floating-point-Control-Register));
    Android's initial FZ/DN are not documented in what I found, so read FPCR once at start-up on each game thread (`mrs`) and log it;
    set FZ=0 DN=0 explicitly if any host-float code remains in EE/IOP paths (matches x86 MXCSR default). Hypothesis on defaults.
    Note FEAT_AFP (Armv8.7) changes FZ meaning when FPCR.AH=1; the S23 is Armv9.0 and does not use AH by default. Hypothesis.
2.4 **No FMA.** `-ffp-contract=off` stops compiler fusion, but explicit `vfmaq_*`/`vfmsq_*` always fuse, and `vmlaq_f32`
    may lower to fmul+fadd or fma depending on clang. Rule: no `vfma*`, `vmla*`, `vmls*` anywhere; code review plus a CI
    grep. With integer NEON this never appears. High.
2.5 **Proof.** Reuse `native/vu_fast_ops_fixture.cpp`: compare NEON with scalar over exhaustive exponents x random mantissas
    x all flush/zero/overflow edges, run it on an arm64 device (not only emulator x86), and add the fixture to the device
    smoke test. Expected effect: the scalar form is about a fifth of EE time in races; NEON should cut that by 3-4x
    (Hypothesis, measure). Bit-identical if the fixture passes.
2.6 Also review the other x86-only fast paths in `native/` (grep `__SSE`, `_mm_`): any hot one has the same scalar penalty.

## 3. Threads, power, pacing

3.1 **Do not hard-pin.** The platform scheduler and ADPF own placement; the Android thermal page lists "worker-thread
    affinity for big and small cores" only as an engine tunable ([thermal](https://developer.android.com/games/optimize/adpf/thermal)).
    Use affinity only as a measured A/B (`sched_setaffinity` on the EE thread to the X3 and the GS/VU1 threads to the
    A715/A710 cluster) and always fall back to no pinning. Sizing pools from `hardware_concurrency()` counts the A510s:
    count cores by `cpu_capacity`/max frequency or by the affinity mask. Hypothesis (sysfs access to apps varies by Android version).
3.2 **ADPF performance hint sessions** ([ADPF](https://developer.android.com/games/optimize/adpf),
    [NDK reference](https://developer.android.com/ndk/reference/group/a-performance-hint)): create one session per frame
    pipeline stage (EE; GS worker; VU1/MTVU) with that thread's tid and a target of 16.67 ms; call
    `APerformanceHint_reportActualWorkDuration` each VBlank with the real busy time (exclude waits and the pacing sleep).
    The framework raises or lowers CPU frequency and picks the core to hold that target. `setThreads` and the newer
    session-creation call have higher API levels (the older create call is marked deprecated in the NDK page): gate with
    `__builtin_available` / `dlsym`, our floor is API 29. Android 15 adds `setPreferPowerEfficiency` for non-critical threads
    (audio, pipeline compile workers). Confirmed (API shape); gains Hypothesis. Bit-identical.
3.3 **Thermal** ([thermal](https://developer.android.com/games/optimize/adpf/thermal)), Confirmed: NDK API from Android 12;
    poll `AThermal_getThermalHeadroom(30)` no more than once per 10 s from ONE thread (else NaN); first value NaN/0 means
    unsupported; ~0.85 = light, 0.95 = moderate, >1.0 = severe heuristics; do not trust status alone on some devices. Use
    it to step down in this order: Performance Mode, internal scale 3x->2x->1x, then the 30 FPS mode (the project already has it).
3.4 **Sustained performance mode** (`Window.setSustainedPerformanceMode`, needs device support check): it caps clocks to a
    sustainable level, which may be below what 2x needs; test as an A/B, not a default. High on API; Hypothesis on S23 effect.
3.5 **Game Mode / GameManager** ([Game Mode API](https://developer.android.com/games/gamemode/gamemode-api)), Confirmed:
    declare `android:appCategory="game"` (or `isGame`), add the `game_mode_config` meta-data to opt into performance and
    battery modes, read `getGameMode()` on every resume (can be null on some devices after Android 15), test with
    `adb shell cmd game mode performance <pkg>`. Samsung's Game Booster may also apply interventions: measure both.
3.6 **Frame pacing.** The game pace stays VBlank/audio based (plan section 8). Present at 60 on a 120 Hz panel with
    `ANativeWindow_setFrameRate`/`Surface.setFrameRate` (API 30; High) so the panel runs 60 Hz or an exact 2x repeat; Swappy
    ([overview](https://developer.android.com/games/sdk/frame-pacing)) is the alternative: initialise with
    `swappyAutoSwapInterval(false)`, `swappyAutoPipelineMode(false)`, `swappySwapIntervalNS(1e9/60)` (Google's recommended
    start), uses `VK_GOOGLE_display_timing` and fences to avoid queue stuffing. Swappy wraps `vkQueuePresentKHR` and wants to
    own the present call, which conflicts with our single-clock pacing, so try `setFrameRate` first. Confirmed (API); fit is a Hypothesis.

## 4. Memory

4.1 **Allocator.** Scudo is the default native allocator since Android 11 ([scudo](https://source.android.com/devices/tech/debug/scudo)).
    Knobs: `__scudo_default_options`, `mallopt`; quarantine (256 KB default 64-bit) and `release_to_os_interval_ms`. Confirmed.
    The translated code works on one flat guest RAM block, so scudo should be off the hot path; do not tune it before
    simpleperf shows `malloc/free` above ~1% (Hypothesis). Allocate guest RAM, rings and the GS arenas once with `mmap`.
4.2 **Large pages.** No supported way for apps to get huge pages on stock Android (Hypothesis); skip. 4 KB vs 16 KB: avoid
    `PAGE_SIZE` constants, use `sysconf(_SC_PAGESIZE)` ([page sizes](https://developer.android.com/guide/practices/page-sizes)).
4.3 **Rings (about 400 MiB: 256 staging, 64 vertex, 16 uniform, 64 readback, plus images).** Adreno is UMA: host-visible
    heaps are the same DRAM, so a "staging then copy" ring doubles traffic; prefer `HOST_VISIBLE|DEVICE_LOCAL` memory and
    write straight into the final buffer (Hypothesis, check the memory types the device reports). Size from
    `VK_EXT_memory_budget` and `VkPhysicalDeviceMemoryProperties`; target <= ~150-200 MiB of rings on 8 GB phones. Lazily
    allocated memory only for attachments that never leave the pass
    ([Qualcomm best practices](https://docs.qualcomm.com/bundle/publicresource/80-78185-2/topics/mobile_best_practices.md)).
4.4 **Low-memory killer.** Implement `onTrimMemory` (drop caches first, never rings in use), keep a foreground service-free
    game process lean, and watch `dumpsys meminfo <pkg>` (PSS, graphics) plus `adb shell dumpsys activity processes`
    for kills. Free guest-visible memory first (texture replacement caches). High.

## 5. Vulkan on Adreno (Qualcomm, Arm, Khronos)

5.1 **Tile model.** Adreno renders in GMEM tiles with a binning pass, or in direct mode (FlexRender chooses); clear or
    `DONT_CARE` loads avoid GMEM loads, avoid stores of unused targets, merge passes, and use subpasses for chains
    ([Qualcomm best practices](https://docs.qualcomm.com/bundle/publicresource/80-78185-2/topics/mobile_best_practices.md);
    [Khronos TBR guide](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)). Confirmed.
    Our CoreGS pass has no attachments, so the tile hardware gets no benefit and the render pass behaves like compute-style
    traffic to DRAM (Hypothesis). This is the point of the D1 path.
5.2 **Mutable format kills compression.** Qualcomm: avoid `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, it often breaks UBWC; avoid it
    on all devices before Adreno 750. Adreno 740 is in that group. Confirmed. Our ROP image sets it (`core2_gpu.cpp:661`)
    for the R32_UINT/RGBA8 reinterpret. Cheapest tests: (a) on Android do not use the mutable-format reinterpret
    (separate image or a copy at the end of the frame), (b) measure DRAM bytes in AGI/Snapdragon Profiler before and after.
    Storage-image writes generally cannot be UBWC-compressed at all (Hypothesis), so this may matter less than for attachments.
5.3 **Barriers.** Use `VK_DEPENDENCY_BY_REGION_BIT` where only same-pixel order matters; avoid `ALL_COMMANDS`/`MEMORY_READ|WRITE`
    and BOTTOM_OF_PIPE->TOP_OF_PIPE barriers; barriers and Z clears also block concurrent binning (Qualcomm). Confirmed.
    The renderer's 284-1026 full barriers per VBlank are the first thing to cut (plan A5).
5.4 **Storage image RMW vs attachments.** The two tile-friendly ways to do same-pixel read-modify-write are
    `VK_EXT_rasterization_order_attachment_access` / framebuffer fetch on attachments and input attachments in subpasses
    ([Arm fetch article](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/framebuffer-fetch-in-vulkan);
    Qualcomm subpass rules in the page above). Both need attachments, not storage images: matches D1. The repo already asks
    for the extension; check at runtime whether Adreno 740's driver exposes it. Confirmed (mechanism); S23 support Hypothesis.
5.5 **Fragment shader interlock on Adreno.** I found no public confirmation that Adreno exposes
    `VK_EXT_fragment_shader_interlock` (searches returned only the spec and AMD threads). Treat as unavailable until
    `vulkaninfo` on the S23 says otherwise (the loader probe at `hw_gs_vulkan_loader.cpp:140` already reports it).
    Hypothesis; also check gpuinfo.org for "Adreno 740" and the driver version.
5.6 **Shaders.** Qualcomm: keep each shader inside the instruction cache, minimise GPR use (spills kill occupancy), split
    long shaders, prefer specialization constants/uber shaders over runtime branches, avoid `discard` and shader depth writes
    (they disable Early-Z/LRZ), use mediump where valid. The "very large shader" x 544 specialisations is exactly the risk.
    Confirmed (advice); effect on CoreGS Hypothesis. Use the Adreno Offline Compiler to read instruction/GPR counts.
5.7 **Pipelines.** Create at init, persist `VkPipelineCache` (done), pass `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT`
    in shipping builds when using pipeline libraries (Qualcomm). Confirmed. Compile in 2-3 background threads on the mid
    cores with `setPreferPowerEfficiency`; cache is invalid after a driver update (compare `pipelineCacheUUID` + driverVersion,
    already in the header check at `core2_gpu.cpp:1195`). `VK_EXT_graphics_pipeline_library`: Hypothesis that Adreno 740 has it.
5.8 **Descriptors/constants.** Qualcomm: UBO per shader under ~7.3 KB, prefer UBOs over push constants, use
    `COMBINED_IMAGE_SAMPLER` (separate samplers reportedly cost 2-5% fill on Adreno bindless). Confirmed (advice). The ~443
    batches per VBlank descriptor updates: try push descriptors or one big descriptor set with dynamic offsets; Hypothesis.
5.9 **Timeline semaphores** are core in Vulkan 1.2; API 29 only guarantees 1.1, so feature-detect and keep fences as fallback.
    High. Replace fence waits on the EE critical path (flare readback) only when the profiler shows them.
5.10 **Pre-rotation.** The S23 panel is portrait-native and the game is landscape, so the surface is always rotated 90 degrees.
    Without pre-rotation the compositor may preempt GPU work
    ([Android blog](https://android-developers.googleblog.com/2020/02/handling-device-orientation-efficiently.html),
    [Khronos sample](https://docs.vulkan.org/samples/latest/samples/performance/surface_rotation/README.html)). Set the swapchain
    `preTransform` to `currentTransform` and bake the rotation into the final present/FSR pass (free, it is already a
    full-screen pass). Confirmed; bit-identical (presentation only). Handle `VK_SUBOPTIMAL_KHR` and recreate on change.
5.11 **Known quirks.** Public lists are thin. Qualcomm's advice is to read `driverVersion` (10-bit major, 10-bit minor, 12-bit
    patch) and gate workarounds by it; example given: workaround only for Adreno driver 676 patch < 17
    ([best practices](https://docs.qualcomm.com/bundle/publicresource/80-78185-2/topics/mobile_best_practices.md)). Confirmed.
    Log vendor, device, driverVersion and the feature/extension set at start-up; use the Vulkan Adreno Layer
    (reports suboptimal API use through logcat in non-shipping builds) in A4/A5. Gap: no public bug list found.

## 6. Measurement on device

Setup (once): USB debugging, `adb shell getprop ro.product.model`, `adb shell dumpsys SurfaceFlinger | grep GLES` (driver
string/version), `adb shell cmd game mode` (current mode), thermal baseline `adb shell dumpsys thermalservice`. Make the build
`profileable` (`<profileable android:shell="true"/>`) or debuggable; keep an unstripped `libgt4.so` (we already have
line tables). Charge above 80%, screen on, airplane mode, same ambient, cool phone for runs; run 5 min warm-up before A/B.
6.1 **Our own F7/F8 first**: per-thread busy ms (EE, VU1, GS worker, present), pipeline compile time on first run, readback
    waits. This is the cheapest and already comparable to Windows.
6.2 **simpleperf** ([guide](https://developer.android.com/ndk/guides/simpleperf)), Confirmed: `app_profiler.py -p <pkg> -r "-g --duration 30"`
    (or `simpleperf record -g --app <pkg> -f 1000 --duration 30`), then `report --sort tid,comm`, `--sort dso`,
    `--dsos libgt4.so --sort symbol`, and `report_html.py` for flame graphs. Sort by thread to see EE vs GS; check `fp::add/mul`
    share before and after NEON; check `malloc`, `memcpy`, `__aarch64_ldadd*`/atomics (LL/SC vs LSE). For counters:
    `simpleperf stat --per-thread -e cpu-cycles,instructions,branch-misses,cache-misses` (events available on X3: Hypothesis).
6.3 **Perfetto** ([Android quickstart](https://perfetto.dev/docs/quickstart/android-tracing)), Confirmed: record
    `adb shell perfetto -o /data/misc/perfetto-traces/gt4.pftrace -t 30s sched freq idle gfx view am wm`, pull it, open in
    the Perfetto UI. Look at: which core each thread runs on and its frequency, runnable-but-not-running gaps (scheduling),
    frame timeline (late present, jank, SurfaceFlinger), CPU idle states. Add `atrace` slices (`ATrace_beginSection`)
    around EE frame, VU1 batch, GS submit, present. GPU and thermal counters need a full trace config
    ("Record new trace" in the UI, GPU frequency/memory and thermal sources): Hypothesis on exact names per device.
6.4 **GPU**: Android GPU Inspector for Adreno (system profile: GPU busy %, frequency, bandwidth, counters; frame profile for
    the most expensive render passes) ([AGI](https://developer.android.com/agi)); the AGI page now recommends Android Performance
    Analyzer (public beta). Snapdragon Profiler (Qualcomm) for the "Rendering Stages" metric (direct vs binning, subpass
    merge) and shader stats (Vulkan shader stats need driver minor >= 636). Confirmed (tools); S23 Ultra Samsung driver counter
    support: Hypothesis, test AGI first, fall back to Snapdragon Profiler.
6.5 **Sustained run**: 20 minutes of the George V lap loop / night race, log every 10 s `AThermal_getThermalHeadroom(30)`, CPU/GPU
    frequency (Perfetto), battery temperature (`dumpsys battery`), and our frametime CSV. Gate: no frame above 20 ms in the last
    10 minutes (plan A5 gate).
6.6 **What to measure first, in order**: (1) frame time per thread at 1x, 2x, 3x; (2) EE profile (simpleperf) and `fp4` share;
    (3) thread placement and frequency (Perfetto); (4) GPU busy %, DRAM bytes, direct-vs-binning mode, barrier count;
    (5) pipeline compile and first-race hitches; (6) memory (PSS, kills) and 20-min thermal curve.

## 7. Ranked plan for GT4Recomp on the S23 (cheapest and biggest first)

| # | Do | Expected effect | Identity with Windows |
|---|----|-----------------|-----------------------|
| 1 | Baseline measurement on the S23: F8 + simpleperf by thread + Perfetto placement (section 6) | decides everything below | n/a |
| 2 | NEON `fp4` with integer intrinsics (2.1-2.5), fixture on device | removes up to ~1/5 of EE time (scalar today); biggest single CPU win | Bit-identical if the fixture passes (needs care: fixture is mandatory) |
| 3 | Link/flags pass: add `common-page-size=16384`, `-fvisibility=hidden`, `--gc-sections`, `--icf=safe`, ThinLTO, `-mtune=cortex-x3`; keep `-O2` and `-ffp-contract=off` | few % to ~10% (Hypothesis) | Identical (needs care: re-run identity drives after LTO) |
| 4 | ADPF hint sessions (EE, GS, VU1) + thermal headroom poll + Game Mode declaration | steadier clocks, fewer dips, sustained 20 min | Bit-identical (timing only) |
| 5 | Pre-rotation in the final present pass; `setFrameRate(60)` on 120 Hz | removes compositor rotation cost and double-present jank | Bit-identical |
| 6 | Android PGO profile from a recorded drive on the S23 (1.8) | 10-20% CPU (Hypothesis) | Identical; needs care: separate profile, same NDK |
| 7 | GPU quick wins: drop `MUTABLE_FORMAT` on Android, cut full barriers/`BY_REGION`, UMA host-visible rings, trim ring sizes to the budget | bandwidth and CPU submit cost; size depends on 6.4 | Frames must match golden images (needs care) |
| 8 | Pipeline strategy: persisted cache, background compile on mid cores, specialization over runtime branches, split the large shader if GPR/icache bound | removes first-race hitches; shader occupancy | Must match golden frames (needs care) |
| 9 | D1 attachment path (rasterization-order access / subpasses) for common draws | the large GPU win on a tiler; main route to 2x/3x | Faithful-by-default rules apply: golden-frame gate per scene (needs care) |
| 10 | `-march=armv8.2-a` (LSE) A/B, then `-O3` A/B, then optional second library or dispatch | small, only if measured | Identical |
| 11 | Hard core pinning / sustained performance mode | only if Perfetto shows bad placement | Bit-identical (timing only) |

Open questions to settle on the phone: does Adreno 740 expose fragment shader interlock and rasterization-order attachment
access; what Android sets FPCR to on game threads; whether Samsung Game Booster overrides Game Mode; whether
`VK_EXT_graphics_pipeline_library` is present; cost of the mutable-format ROP image in DRAM bytes.

## Sources (all fetched 2026-11-05 unless noted)
- NDK: [ABIs](https://developer.android.com/ndk/guides/abis), [PGO](https://developer.android.com/ndk/guides/pgo), [simpleperf](https://developer.android.com/ndk/guides/simpleperf), [16 KB pages](https://developer.android.com/guide/practices/page-sizes)
- ADPF: [overview](https://developer.android.com/games/optimize/adpf), [thermal](https://developer.android.com/games/optimize/adpf/thermal), [PerformanceHint NDK ref](https://developer.android.com/ndk/reference/group/a-performance-hint), [Game Mode](https://developer.android.com/games/gamemode/gamemode-api) (search snippet)
- Pacing: [Frame Pacing / Swappy](https://developer.android.com/games/sdk/frame-pacing)
- GPU: [Qualcomm Adreno mobile best practices](https://docs.qualcomm.com/bundle/publicresource/80-78185-2/topics/mobile_best_practices.md), [Khronos TBR guide](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html), [Arm framebuffer fetch](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/framebuffer-fetch-in-vulkan), [surface rotation sample](https://docs.vulkan.org/samples/latest/samples/performance/surface_rotation/README.html), [Android orientation blog](https://android-developers.googleblog.com/2020/02/handling-device-orientation-efficiently.html)
- Tools: [AGI](https://developer.android.com/agi), [Perfetto Android quickstart](https://perfetto.dev/docs/quickstart/android-tracing)
- Memory: [scudo](https://source.android.com/devices/tech/debug/scudo)
- Arm FPCR: [FPCR register reference](https://developer.arm.com/documentation/ddi0595/latest/AArch64-Registers/FPCR--Floating-point-Control-Register) (via search; not opened in full)
- Not reached: Qualcomm's older developer.qualcomm.com pages (redirect to a login/landing page), Adreno interlock support data, any public Adreno driver bug list.
