# Native Linux port: plan (TASK-074, CoreGS 2.0 only)

Status: **plan only** (owner, 2026-11-04). Supersedes `docs/linux/PLAN.md` on branch `linux/main` (2026-10-19), which
assumed CoreGS 1.1 and a Linux build of gt4-gs. Now: **CoreGS 2.0 is the only renderer on Linux and Android; GSdx stays a
Windows-only fallback.** Phase 3 of the roadmap (after 2.0.0); the [Android plan](ANDROID-PORT-PLAN.md) builds on the
foundation below. Sources: four read-only audits of 2026-11-04 (game code, CoreGS 2.0, launcher, previous plans).

## Why Linux first, and why it matters for Android

- The game code is already portable: the translated EE units (346 files, 1.78 M lines), the VU1/VU0 AOT output (130 files)
  and the donor runtime (ps2-web-recomp, which has `__linux__` futex and `fseeko` paths) contain no Windows API and no
  x86 intrinsics. The Windows-only layer is the native adapter and the launcher's platform crate.
- Every foundation step Android needs (a platform seam, SDL3 window/audio, a Linux-style build, a new pin family,
  cross-platform identity tests) is done once here. **Linux arm64** (headless) is the cheapest place to prove the arm64
  questions (floating point, memory ordering) before any Android work.
- Players already run the Windows build under Proton/SteamOS (issues #129 audio crackle, #141); a native build removes
  Wine's audio and window layers.

## Rules

- Windows behaviour stays byte-identical at every step (CTest, Python, headless frame dumps, recorded-drive identity).
- **Game logic identical across platforms**: the race clock and the full simulation state on the recorded drives
  (`scripts/coregs2_bench`: `george-volta2`, `el-capitan`, `tokyo-r246`, the v2.00 night race) must match the Windows
  build byte for byte. This is the port's main acceptance, not "it boots".
- clang everywhere (the VU AOT output uses clang vector extensions and `__builtin_shufflevector`); no GCC builds.
- No JIT; MIT game binaries; SDL3 (zlib) is fine.

## The Windows-only surface (inventory, Confirmed by grep)

| Area | Today | Linux replacement |
|---|---|---|
| Window, messages, keys, DPI, fullscreen, cursor (`window.cpp`, `window_mode.*`, `cursor_autohide.*`, `window_heartbeat.hpp`) | Win32 pump on the game thread, `GetAsyncKeyState` | SDL3 window and events; the heartbeat (decision 0076) is a Windows-only fix and stays in the Win32 backend |
| Audio (`host_io.cpp`) | WinMM `waveOut`, polled push model with rate control | SDL3 audio stream (PipeWire/Pulse); keep the push and rate-control logic |
| Clock and pacing (`frame_clock.*`) | `QueryPerformanceCounter`, waitable timers, `timeBeginPeriod` | `clock_gettime(CLOCK_MONOTONIC)`, `clock_nanosleep` |
| Vulkan surface and loader (`hw_gs_backend.cpp:1702`, `native_vulkan.cpp:116,258`, `hw_gs_vulkan_loader.hpp`, `gpu_tail.inc:77`) | `vkCreateWin32SurfaceKHR`, `LoadLibraryExW("vulkan-1.dll")` | `SDL_Vulkan_CreateSurface` (Wayland and X11), `libvulkan.so.1` |
| Process start (`game_main.cpp`) | BCrypt SHA-256, `MessageBoxW`, `_putenv_s`, `GetModuleFileNameW`, `FreeConsole` | portable SHA-256, SDL message box, `setenv`, `/proc/self/exe` |
| Env and paths | `_wgetenv` (`core2_gpu.cpp:1075`, `core2_renderer.cpp:3145,3165`), `wchar_t`/`L""` paths in 19+ files, `MAX_PATH`, backslashes | one UTF-8 `getenv` wrapper and `std::filesystem` paths |
| Diagnostics (`hw_cpu_sampler.cpp`, `hw_hud_system.cpp`, `perf_clock.*`, `__rdtsc` in `hw_gs_stream_sink.cpp`) | SEH, `Rip/Rsp`, PDH, psapi, `__rdtsc`, `__cpuid` | `/proc` stats, `clock_gettime`; the sampler becomes optional |
| Achievements (`achievements*.cpp`) | WinHTTP, GDI+ overlay | move network calls to a portable HTTP layer (or the launcher), draw the overlay through CoreGS's HUD |
| GSdx stream (`gs_stream_ipc.cpp`, `gs_stream_client.cpp`) | shared memory, events, Job Object | **not built** on Linux |
| Build (`CMakeLists.txt:4-8`, `tools/ee_producer/CMakeLists.txt:4-9`, `/W4`, `/clang:` flags, `.rc`, Windows SDL3 and Vulkan SDK libs) | refuses non-WIN32/non-clang-cl | a Linux branch with clang++; same warnings and flags in GNU form |
| Pins (`docs/inputs/*.json`, `native/CMakeLists.txt:89-170`, `scripts/ee_producer.py:21`) | MSVC-ABI `ps2game.lib` paths and hashes | a new Linux pin family (below) |

Already portable: SDL3 gamepads, `memcard_store.cpp` (has a POSIX branch), pacing logic, the MTVU ring, pad mapping,
input recording, frame skip, race hooks, display aspect, the SPU2 envelope.

## Determinism foundation (shared with Android; do it first)

1. **Pin `-ffp-contract=off`** for every target and for the EE library build. Today it is opt-in (`GT4_FP_CONTRACT_OFF`,
   default off) and harmless only because SSE4.1 has no FMA. `vu_ops.hpp:304` (`x*x + y*y + z*z`, 143 VU1 sites) fuses on
   any FMA target (arm64 always; x86 with `-march` FMA). Make it the default on Windows too: no change there.
2. **libm calls in VU paths**: `vu_ops.hpp:312-320` (`std::sin`, `std::atan`, `std::atan2`, `std::exp`) and the IPU
   `std::cos` table (`ipu.cpp:91`) can differ in the last bit between UCRT, glibc and bionic. First count whether those EFU
   indices ever run in races (VU1 AOT only emits index 0x73, sqrt/reciprocal, correctly rounded). If they run, replace them
   with our own fixed implementation used on every platform (a game-identity gate on Windows decides whether Windows may
   change).
3. **Host `long` in the IOP `strtol` HLE** (`iop/libs.cpp:819-824`): clamp to 32 bits explicitly so LP64 and LLP64 agree.
4. **Memory ordering**: run the threaded paths (donor `queue.cpp:89-138`, `hw_gs_stream_sink.cpp`'s relaxed atomics, the
   MTVU `table_dest`/`completed_job` hand-off) under ThreadSanitizer on Linux x86, then on arm64 hardware. x86's strong
   ordering hides mistakes that arm64 shows.
5. **Cross-platform identity test**: a script that replays the recorded drives on the new platform and compares the race
   clock and state dumps with the Windows reference (`game_identity.sh` already does this between two Windows builds).

## Stages

### L0. Platform seam on Windows (no behaviour change)

A small `native/platform/` layer: window and events, keyboard, audio device, monotonic clock and sleep, paths and env,
message box, process info. The Win32 code moves behind it unchanged. Remove the unconditional SSE2 in
`core2_upload.cpp:7,178-230` (add the scalar form, keep SSE2 on x86). **Gate: Windows byte-identical** (CTest, Python,
frame dumps, recorded-drive identity).

### L1. Linux build, headless

- CMake accepts Linux + clang++; the EE translator (`tools/ee_producer`) builds on Linux; check that it emits byte-identical
  units from the same ELF on both hosts (container order, path spellings).
- `libps2game.a` from the units; VU tables; glslang pinned for Linux (the shader CMake hard-codes the Windows SDK path);
  SDL3 and Vulkan from the system or a pinned build.
- **New pin family**: pin the translator inputs and outputs (unit source hashes, shader SPIR-V hashes) across platforms,
  and the Linux binaries separately. Verification never updates pins (AGENTS.md).
- Headless runs on Linux (lavapipe for correctness in WSL2 or a VM; RADV/NVIDIA on real hardware).
- **Gate: the cross-platform identity test passes on Linux x86-64; CoreGS 2.0 frames match Windows on the recorded drives
  (same driver family where possible; otherwise within a small, explained tolerance).**

### L1b. Linux arm64, headless (the Android rehearsal)

- Build for aarch64 Linux (an ARM cloud VM, a Raspberry Pi 5, or a Snapdragon X laptop with Linux); lavapipe is enough for
  identity runs. `fp4.hpp` falls back to its scalar form here (bit-identical, about a fifth of EE time slower in races):
  write a NEON form with the same bit-identity fixture.
- **Gate: identity on arm64; TSAN clean; the NEON `fp4` passes its fixture.**

### L2. SDL3 backend: playable

- SDL3 window, Vulkan surface (Wayland first, X11 too), borderless fullscreen (Wayland has no exclusive mode), HiDPI, cursor.
- Keyboard through SDL3 events (the fixed layout of TASK-076); pads already on SDL3; the controls file and the controller
  choice (TASK-148) unchanged.
- Audio through an SDL3 audio stream with the existing rate control; check the pace with PipeWire's quantum (256-1024
  frames) and the 60 ms target; the pacing clock stays single (audio or present) as on Windows.
- Present modes: FIFO by default; mailbox where available; frame pacing measured with the F8 probe.
- **Gate: owner plays on real Linux (desktop dual-boot or Steam Deck); F8 reports at 100% speed.**

### L3. Game-side parity

F7/F8 probe on Linux clocks; HUD system stats from `/proc`; achievements on a portable HTTP layer; memory card durability
(fsync the directory after the rename, `memcard_store.cpp:65`, a fix Windows does not need but harmless); recorded-drive
tools (`scripts/coregs2_bench`) runnable on Linux.

### L4. Launcher on Linux

From the launcher audit:
- **Process model**: the separate game process stays. Replace the Windows stubs that silently "succeed" (single instance
  always true, `process_running` never matches, kill-on-close missing): lock file, `PR_SET_PDEATHSIG` or a process group,
  `/proc` scans. Map signals (SIGSEGV, SIGABRT, SIGKILL) in `exit::classify` (a signal-killed game has no exit code today
  and reads as "killed by us").
- **Paths**: XDG (`$XDG_DATA_HOME`, `$XDG_CONFIG_HOME`) instead of `APPDATA`/`LOCALAPPDATA` (the launcher refuses to start
  without them today); read-only install folders (AppImage, Flatpak).
- **Hardware lists**: Vulkan probe via `dlopen("libvulkan.so.1")`; monitors and modes through SDL3 (the `GT4_MONITOR`
  Windows device name needs a portable identity).
- **WebView**: webkit2gtk 4.1 quirks (blank windows with DMA-BUF on NVIDIA/Wayland: `WEBKIT_DISABLE_DMABUF_RENDERER`), no
  acrylic backdrop (fallback style), a Linux safe mode in place of the WebView2 guard; `explorer` calls become `xdg-open`
  (some are unconditional today).
- **Secrets**: RetroAchievements token through libsecret; without a keyring, refuse to store (as today) and explain.
- **Discord**: Unix socket `$XDG_RUNTIME_DIR/discord-ipc-N` (plus Flatpak/Snap locations); the frame protocol is reused.
- **GSdx**: hidden on Linux.
- **Gate: launcher focus checks and nav-test on Linux; first-run, setup, Play, saves, controls on the owner's machine.**

### L5. Packaging and updates

AppImage first (self-update by replacing the file, verified by `SHA256SUMS`), Flatpak later (updates through Flatpak; needs
Discord and device permissions), a tarball for advanced users. Release asset names per platform
(`GT4Recomp-<v>-linux-x86_64.AppImage`). Steam Deck: gamescope session, gamepad-first launcher (the existing focus engine),
Proton not needed.

## Subtle risks (Linux)

1. FP contraction or a non-clang compiler changing VU float results (pin flags; identity test).
2. libm last-bit differences in EFU and IPU paths.
3. Pacing with PipeWire quanta and Wayland FIFO; 60 Hz game pace on 120/144 Hz displays must stay VBlank-driven, never
   present-driven.
4. Signal exits misread as normal, and the launcher's stubbed process functions silently succeeding.
5. Case-sensitive host paths (`shaders/`, `controls.toml`, `gs\...` leftovers, backslashes in code).
6. Memory card durability without a directory fsync on ext4/btrfs.
7. Translator output not byte-stable across hosts (breaks pins and identity).
8. webkit2gtk rendering quirks hiding the launcher on some GPUs.

## Owner decisions needed

Target distributions (Steam Deck/SteamOS, Ubuntu LTS, Fedora/Bazzite); AppImage or Flatpak first; whether Linux arm64 is a
shipped target or only the Android rehearsal; the test hardware (dual-boot desktop, Steam Deck).
