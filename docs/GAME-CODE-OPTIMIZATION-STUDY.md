# Game-code optimization study: CPU time and energy outside the GS renderer (TASK-134)

2026-10-28. Research and planning only: no build, no benchmark, no profiler, no game run. Branch
`task/134-game-code-optimization-study`. Agent status: **not PCSX2-tainted** (no PCSX2 source, no `gt4recomp-gs`
fork, no `private/tooling/pcsx2-*`, no `private/slice189-pcsx2-ref` was opened; PCSX2 is named below only as
black-box behaviour or as the owner's measured baseline). Nothing under `native/hw_gs/`, the `task/027-*` branches
or the CoreGS 2.0 worktree was read or touched.

**Code baseline (2026-10-29, owner request):** this branch is synced with the newest build, 1.3.0-rc7
(`task/133-rc7-ra-controls` at `c21e934`): every program file is byte-identical to it. The study read the runtime at tag
`v1.3.0-rc6`; rc7 is rc6 plus RetroAchievements and controller changes, and the only per-frame difference is the
RetroAchievements hook (section 3.8). Every other finding applies to rc7 unchanged.

Labels: **Confirmed** (measured in a log or read in code), **High confidence**, **Hypothesis**, **Unknown**.
Times are ms per guest VBlank (one 60 Hz frame, budget 16.68 ms) unless stated.

Target: the owner's laptop, Intel Core i5-7200U (2 cores / 4 threads, Kaby Lake, 15 W package shared with the
HD 620 iGPU, 3 MB L3), at full speed and 60 FPS at 1x. Measurement machine: this desktop (Ryzen 7 5700X,
RX 9060 XT). Owner baseline on the laptop, same scene and settings: **PCSX2 ~50% CPU at a stable 60 FPS; GT4Recomp
1.3.0-rc6 ~70-80% CPU with dips** (ai-memory `notes/pcsx2-baseline-weak-laptop`,
`decisions/next-objective-cpu-parity-with-pcsx2`, recorded on `task/116-117-flare-perf` as the CPU-parity
"TASK-119", commit `9226927`).

## 0. Summary

1. **The laptop is busy, not spinning.** In the laptop race the four working threads use about 35 ms of CPU per
   16.7 ms frame: EE 11.7, VU1 worker 9.3, gt4-gs CPU thread 3.4, gt4-gs GS thread ~11 (Confirmed, owner logs). That
   is ~2.1 busy logical CPUs out of 4, which with Windows' turbo scaling and the driver threads matches the owner's
   70-80%. No large spin-wait exists in our code; the only steady spin is the last 0.5 ms of the pacing wait
   (section 3.1).
2. **Half or more of the EE critical path is plumbing.** Of 12.2 ms on the laptop: translated game code 4.9
   (`ee_dispatch`), interrupt handlers and their delivery 1.4, GS stream copy to gt4-gs 1.4, IOP + sound mixer 1.3,
   VIF1 posting 1.2, host VBlank work 0.5, MTVU journal 0.4, events 0.3 (section 2.2). Syscalls, the kernel's
   thread switches and guest idle cost almost nothing (0.03-0.06 ms; section 3.2).
3. **The owner's four hypotheses, checked against the code:** per-instruction accounting is **not** the problem
   (one subtraction per basic block); the GS IPC **is** a large cost (1.4 ms on the EE plus a 3.4 ms thread) but it
   disappears with an in-process CoreGS 2.0 if that intake is zero-copy; FPU/MMI wrappers are exact software
   emulation whose share is Unknown until a profile; kernel/syscall cost is small, but the interrupt-handler path
   (`kernel_service`, partly game code) is 1.4 ms. There is **no guest busy-wait to skip**: every game wait (frame,
   DMA, SIF RPC) already blocks and is fast-forwarded by the runtime (section 3.2).
4. **Generated EE code** keeps all guest state in memory behind a reference that may alias guest RAM, splits code
   into 16 KiB-window functions with a switch at every entry, and calls through those switches from 95,305 sites
   (section 3.3): the largest long-term lever for `ee_dispatch` and the 3D-menu dips.
5. **Free ISA win:** the game executable is built for SSE2 although the launcher already requires SSE4.1; the VU1
   worker's clamp (`vfix`) needs one SSE4.1 instruction instead of four (section 3.5).
6. **Ordered plan (section 4), cheapest first (owner rule):** P0 measure (sub-scopes, guest-state hash, one CPU
   profile) -> P1 host quick wins (SSE4.1, doorbells, copies, probe clock, pacing tail, thread placement) -> P2 kernel
   delivery -> P3 IOP/SPU2 -> P4 build-level speed (one compiler, ThinLTO, PGO) -> P5 VIF path -> P6 zero-copy CoreGS
   2.0 intake (ships with CoreGS 2.0) -> P7 VU1 program quality -> P8 EE translator (noalias state, per-function
   output; the most complex, last) -> P9 IOP AOT only if needed. Expected (Hypothesis): laptop race critical path
   12.2 -> 9-10 ms with GSdx, about 8 ms with CoreGS 2.0, about 6.5-7 ms after P7-P8, with total CPU in or below the
   PCSX2 class.
7. **Owner decisions of 2026-10-28** are in section 7: SSE4.1 only (no AVX2 executable unless a large
   measured gain), cheapest first, thread placement yes, profiler and first baselines once the desktop is free of the
   CoreGS 2.0 build, laptop tests manual on both games, no more work on the GSdx bridge.

## 1. Sources and method

| Source | What it gives | Where |
|---|---|---|
| Laptop F8 and 5 s periodic reports, rc6 portable, Spec II, GSdx D3D11/D3D12, 1x, 1366x768 | 6 race F8 intervals (48-63 s each) and ~200 periodic intervals | copied 2026-10-28 from the owner's network share to `private/laptop-i5-7200u-f8/` in this worktree (git-ignored; `SHA256SUMS.txt` beside them; the logs hold the laptop's user path, so they are never committed) |
| Desktop F8, slice262 (release 1.2.2 recipe), v2.00, GSdx | showroom, Time Trial Nordschleife (day), night race | `private/manual-play/session-20261022-163408-808184/probe.log` lines 13736, 18619, 27952 |
| Desktop F8, rc6 portable, 6x at 3440x1440 | v2.00 night race and Spec II George V, Accurate and Performance mode | `C:/Users/Alano/Desktop/GT4Recomp-1.3.0-rc6-portable/data/sessions/session-17929565*/game.log` |
| Desktop F8, slice19x-21x (2026-10-16) | older pcsx2-mode races (5,000 MTVU jobs per frame, before TASK-084) | `private/manual-play/session-20261016-*` |
| Headless study (TASK-084) | menus, showroom, day and night race per stage, MTVU on/off | `docs/FRAME-COST-STUDY.md` |
| Code | runtime capsule `private/slice262-frame-cost-runtime` (the one rc6 links), donor headers and translator `private/references/ps2-web-recomp` (`1b48087`), generated EE source `private/slice217-ee-aspect-producer/generated` (the rc6 v2.00 library, pin `docs/inputs/slice303-pcsx2.json` at tag `v1.3.0-rc6`), `native/` at `v1.3.0-rc6` | read only |

The extraction scripts (a small F8 parser and two table printers) stayed in the agent's scratchpad; every number below
was read from the logs listed in the row, not estimated. Laptop and desktop scenes are **not matched**: the laptop
runs Spec II on Time Trial Nordschleife, the desktop F8 runs v2.00. Ratios between them are indicative only.

What the F8 categories mean (Confirmed in code, `docs/FRAME-COST-STUDY.md` section 1.2 and
`private/slice262-frame-cost-runtime/system.cpp:375-397`):

- `ee_dispatch`: the translated game code **plus** everything it calls without its own scope (syscalls, thread
  switches, MMIO slow paths, DMA starts, PATH3 GIF, MTVU journal operations).
- `kernel_service`: `kernel::service(c)` once per run-loop iteration (interrupt delivery and handlers).
- `iop_run`: the IOP interpreter, run from `sync_iop()` whenever guest time advanced.
- `vif_gif`: `vif::transfer()` on the EE, the shadow VIF1 parse plus the copy into the MTVU batch.
- `gs_ipc_write`, `gs_drain` and the MTVU and readback waits are stopwatches **outside** the exclusive partition: the
  critical path is the sum of the exclusive categories plus these (checked on the F8 rows below: the residual is
  0.2-1.1 ms).
- The release build keeps every scope (`GT4_PERF_PROBE` on, `GT4_PERF_FRAME_ONLY` off): each scope reads
  `steady_clock` twice (`native/perf_probe.hpp:147-163`, `native/perf_probe.cpp:28-31`).

## 2. Measured cost map

### 2.1 The laptop race, thread by thread (Confirmed)

Best laptop interval: rc6, Spec II, Time Trial Nordschleife, Performance Mode preset (`HWDownloadMode 2`, blending
Minimum), Direct3D 11, 1x. `session-1792983550` F8 at line 6308: 61.2 s, 59.89 VBlanks/s, 27 late frames.

| Thread | ms per frame | Share of one logical CPU | Source |
|---|---:|---:|---|
| EE thread (game, IOP, VIF, GS stream producer, input, audio push) | 11.68 CPU, 12.21 critical path | 70% | F8 `measured_thread_cpu`, `[frame] critical path` |
| VU1 worker (MTVU, VU1 AOT programs + PATH1 output) | 9.29 busy (vu1_program 8.83) | 56% | F8 `[mtvu] worker` |
| gt4-gs CPU thread (reads our stream, feeds GSdx) | 3.39 (3.5-3.9 in its own log) | 20% | F8 `[gs-ipc] GS process CPU thread`, `GSStreamReplayer` lines |
| gt4-gs GS thread (GSdx hardware renderer + D3D11 driver calls) | 10.6-11.8 | 64-71% | `GSStreamReplayer` lines at vsync 2000-6000 |
| GPU (HD 620) | 2.9-9.5 | - | same lines |
| **Sum of the four CPU threads** | **~35** | **~2.1 logical CPUs** | |

2.1 busy logical CPUs out of 4 is 53% before Windows scales "% Processor Utility" by the turbo clock
(3.1 GHz / 2.5 GHz base = 1.24, so about 66%); the D3D11 driver's own threads, the Windows audio threads and the launcher add
the rest of the owner's 70-80% (High confidence; the per-thread times are Confirmed, the Task Manager scaling is
documented Windows behaviour). On 2 cores, four busy threads means two pairs of hyper-threads share a core most of
the frame, so **every millisecond saved on any thread also speeds up the EE critical path** (High confidence).

The other laptop F8 intervals (same scene, one setting changed each) move the same way:

| Interval | VBlanks/s | crit. path | EE CPU | ee_dispatch | kernel_service | iop_run | vif_gif | gs_ipc_write | gs_drain | host_vblank | run_due_events | vu0_aot | waits | VU1 worker | gt4-gs CPU thr. | syscalls | thread sw. | stream MB | stream records | source |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Laptop, Accurate, blend High | 48.97 | 18.87 | 13.61 | 4.21 | 1.55 | 1.09 | 1.24 | 1.37 | 0.47 | 0.46 | 0.19 | 0.10 | 7.37 | 10.82 | 3.21 | 146 | 45 | 4.89 | 1616 | `session-1792983301` L3533 |
| Laptop, Perf., blend High | 58.84 | 13.79 | 12.58 | 5.69 | 1.34 | 1.35 | 1.17 | 1.48 | 0.41 | 0.48 | 0.36 | 0.15 | 0.56 | 9.92 | 5.76 | 145 | 45 | 4.86 | 1600 | `session-1792983420` L6506 |
| **Laptop, Perf., blend Min (preset)** | 59.89 | **12.21** | 11.68 | 4.87 | 1.42 | 1.27 | 1.19 | 1.38 | 0.38 | 0.46 | 0.33 | 0.11 | 0.04 | 9.29 | 3.39 | 144 | 45 | 4.86 | 1593 | `session-1792983550` L6308 |
| Laptop, Perf., Min + "fast" GS keys | 59.70 | 13.29 | 12.29 | 5.16 | 1.61 | 1.31 | 1.26 | 1.49 | 0.40 | 0.62 | 0.36 | 0.13 | 0.17 | 9.87 | 4.16 | 144 | 45 | 4.89 | 1606 | `session-1792983709` L6571 |
| Laptop, Perf., Min, D3D12 | 56.19 | 17.34 | 15.20 | 6.36 | 2.25 | 1.54 | 1.50 | 2.05 | 0.50 | 0.72 | 0.42 | 0.17 | 0.93 | 12.22 | 8.85 | 146 | 45 | 4.92 | 1636 | `session-1792985790` L9350 |
| Laptop, Perf., Min, PCSX2 OSD on | 58.65 | 15.29 | 14.09 | 5.96 | 1.67 | 1.44 | 1.28 | 1.77 | 0.43 | 0.63 | 0.38 | 0.16 | 0.72 | 11.12 | 8.15 | 144 | 45 | 4.91 | 1608 | `session-1792985983` L5764 |

`waits` = MTVU waits + readback wait + GS back-pressure. The same guest work (4.86-4.92 MB and ~1,600 stream records per
frame in every row) costs 12.2 ms of EE critical path with the lightest renderer settings and 15.3-17.3 ms when the
renderer process works harder (D3D12: gt4-gs CPU thread 8.9 ms; OSD on: 8.2 ms). The EE-side categories grow
together by 20-40% (`kernel_service` 1.42 -> 2.25, `ee_dispatch` 4.87 -> 6.36): this is **core sharing, not more
game work** (Confirmed by the identical stream size and syscall counts; mechanism High confidence).

### 2.2 The laptop EE critical path, split (Confirmed)

`session-1792983550` L6308, 12.21 ms:

| Part | ms | Share | What it is |
|---|---:|---:|---|
| `ee_dispatch` | 4.87 | 40% | translated game code and the unscoped work it calls |
| `kernel_service` | 1.42 | 12% | interrupt, DMA and alarm handlers: **guest code** run through `run_guest` -> `dispatch` inside this scope, plus the HLE delivery around it (section 3.2) |
| `gs_ipc_write` | 1.38 | 11% | copying ~4.9 MB of GS stream per frame into the shared ring of gt4-gs |
| `iop_run` | 1.27 | 10% | IOP interpreter (23 game IRX modules) |
| `vif_gif` | 1.19 | 10% | VIF1 shadow parse on the EE and the copy of every packet into the MTVU batch |
| `host_vblank` | 0.46 | 4% | message pump, keyboard, SDL pad, audio pump, present post (section 3.1) |
| `gs_drain` | 0.38 | 3% | MTVU journal front end: worker GIF output forwarded to the stream |
| `run_due_events` | 0.33 | 3% | event queue scan |
| `vu0_aot` + audio + unattributed | 0.18 | 1% | VU0 micro AOT and small items |
| residual | 0.69 | 6% | not covered by a scope (vsync post, scope overhead, scheduling) |

**Half or more of the EE critical path is emulation plumbing, not game code:** 5.9-7.3 of 12.2 ms, depending on how
much of `kernel_service` is guest handler code (its split is open measurement M10). The same picture holds on the
desktop (`ee_dispatch` + `kernel_service` is 20-55% of the critical path in the F8 rows below, the low end in
Accurate mode where readback waits inflate the path), which confirms the
TASK-084 study's headless result (game code 2.9 of 8.2 ms) with owner data.

### 2.3 Desktop per scene (Confirmed)

| Interval | VBlanks/s | crit. path | EE CPU | ee_dispatch | kernel_service | iop_run | vif_gif | gs_ipc_write | gs_drain | host_vblank | run_due_events | vu0_aot | waits | VU1 worker | gt4-gs CPU thr. | syscalls | thread sw. | stream MB | stream records | source |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| v2.00 showroom, Accurate | 60.00 | 6.06 | 5.75 | 1.99 | 0.53 | 0.23 | 1.00 | 0.66 | 0.30 | 0.34 | 0.15 | 0.40 | 0.00 | 6.78 | 0.26 | 758 | 41 | 5.38 | 3167 | slice262 L13736 |
| v2.00 TT Nordschleife day, Accurate | 60.00 | 6.13 | 4.69 | 1.50 | 0.50 | 0.43 | 0.40 | 0.57 | 0.15 | 0.37 | 0.08 | 0.07 | 1.82 | 2.94 | 1.17 | 129 | 44 | 4.81 | 1692 | slice262 L18619 |
| v2.00 night race, Accurate | 60.01 | 9.96 | 8.15 | 2.31 | 0.60 | 0.44 | 0.63 | 0.58 | 0.23 | 0.35 | 0.04 | 0.13 | 4.35 | 3.89 | 1.66 | 129 | 38 | 4.64 | 2603 | slice262 L27952 |
| rc6 v2.00 night, Accurate, 6x | 59.79 | 10.49 | 7.62 | 2.56 | 0.62 | 0.45 | 0.56 | 0.53 | 0.21 | 0.38 | 0.04 | 0.14 | 4.74 | n/a | 2.12 | 129 | 39 | 4.25 | 2328 | rc6 `session-1792956312` L980 |
| rc6 v2.00 night, Perf., 6x | 59.97 | 5.91 | 5.69 | 2.63 | 0.63 | 0.46 | 0.54 | 0.52 | 0.20 | 0.37 | 0.16 | 0.14 | 0.01 | n/a | 0.69 | 129 | 39 | 4.23 | 2375 | rc6 `session-1792956734` L5700 |
| rc6 Spec II George V, Accurate, 6x | 53.84 | 18.36 | 11.20 | 2.87 | 0.81 | 0.45 | 0.90 | 0.86 | 0.40 | 0.36 | 0.05 | 0.04 | 11.12 | n/a | 5.88 | 144 | 40 | 6.72 | 4217 | rc6 `session-1792956506` L927 |
| rc6 Spec II George V, Perf., 6x | 59.97 | 7.87 | 7.66 | 3.20 | 0.84 | 0.48 | 0.92 | 0.86 | 0.39 | 0.38 | 0.24 | 0.04 | 0.00 | n/a | 1.67 | 144 | 40 | 6.88 | 4312 | rc6 `session-1792956881` L6305 |

`n/a`: the rc6 v2.00 build does not publish worker times (it prints `busy=0.000`); the Spec II laptop build does.
gt4-gs GS thread on the desktop: 2.9-5.2 ms per 3D frame (slice262 log, `GSStreamReplayer` lines 2949-26745), against
10.4-15.7 ms on the laptop.

Per scene (Confirmed):

- **Menus (2D):** EE critical path 1.4 ms headless on the desktop (`docs/FRAME-COST-STUDY.md` table 2.1) and 1.7-2.6 ms
  on the laptop (periodic reports at lines 283-340 of every session); nothing to gain for full speed.
- **3D menus and loading:** laptop periodic intervals with 1,200-2,500 stream records show `ee_dispatch` 6-8 ms,
  `vif_gif` 1.0-2.4 ms and 30-120 late frames per 5 s (e.g. `session-1792983301` lines 360-438). These are the dips
  the owner sees outside races. Scene labels for these periodic intervals are Unknown.
- **Showroom:** the heaviest VU1 scene (worker 6.8 ms on the desktop) and the most syscalls (758 per frame: 0x2F
  GetThreadId 444.8, 0x29 ChangeThreadPriority 258.4), but they cost **0.058 ms in total** (Confirmed: `syscalls/frame=757.6
  in 0.058 ms`).
- **Day race:** EE 4.7 ms, worker 2.9 ms on the desktop; laptop 11.7 and 9.3.
- **Night race (flares):** Accurate adds 4-5 ms of waits (MTVU download drain + readback); rc6 Performance Mode
  removes them (5.91 ms, TASK-116 step 1). Spec II George V is the heaviest race (6.9 MB, 4,300 records per frame).
- **Replays:** no F8 interval exists (Unknown; open measurement M5).

### 2.4 Laptop against desktop

No matched pair exists (Spec II on the laptop, v2.00 on the desktop). Indicative ratios for the race categories:
`iop_run` 2.6-3.0x, `kernel_service` 1.7-2.8x, `ee_dispatch` 1.5-3.2x, `gs_ipc_write` 1.6-2.7x, VU1 worker
2.4-3.2x, gt4-gs GS thread 2-4x, `host_vblank` only 1.25x. A Kaby Lake at 3.1 GHz has about 55-60% of a Zen 3
core's single-thread speed (High confidence), which explains 1.7-1.8x; the rest (Hypothesis, to measure in M2):
hyper-thread sharing with four busy threads, a 3 MB L3 against the stream's 4.9 MB per frame that is written or read
at least three times (worker output ring, gt4-gs ring, gt4-gs reader; section 3.6), DRAM shared with the iGPU, and the 15 W limit lowering the clock when CPU and iGPU are both busy
(owner: 3.1 -> 2.6-2.7 GHz at 2x).

## 3. Findings per area

### 3.1 CPU burned without useful work: sleeping and spinning waits

Paths below are `native/` at tag `v1.3.0-rc6` unless stated.

| Wait | Kind | Cost per frame | Evidence |
|---|---|---|---|
| Pacing sleep (`release_frame` -> `FrameClock::wait_until`) | high-resolution waitable timer, then a **busy spin for the last 0.5 ms** that calls `QueryPerformanceCounter` in a tight loop with no `_mm_pause` | up to 0.5 ms of one logical CPU (3%) every frame, whatever the load | `frame_clock.cpp:15-16,68-78` (Confirmed); the log line `timer=high-resolution` is in every laptop session |
| MTVU doorbells (`Doorbell::wait_changed`) | `std::atomic::wait` (WaitOnAddress); sleeps, no user spin in our code | the waits are rare: EE 3-7 sleeps per frame, worker idle 5-12 ms per frame asleep | `mtvu_ring.hpp:81-94` (Confirmed); F8 `[mtvu] doorbells` (Confirmed). Whether the C++ library spins inside `wait` before sleeping is Unknown |
| MTVU doorbell **rings** | two locked read-modify-writes per ring (`rings_` statistics counter and `value_`), on every push **and** every pop, on a cache line both threads write | 6,208 worker-to-EE rings per frame in the showroom, ~3,000 in the laptop race (two per GIF record); EE-to-worker 135-350 | `mtvu_ring.hpp:72-77,156,179,198,212` (Confirmed); cost per ring Unknown (Hypothesis 20-100 ns contended, so 0.1-0.6 ms per frame across both threads) |
| GS stream IPC waits (ring space, frames in flight, readback reply) | spin 30 µs with `_mm_pause` and a clock read per turn, then `WaitForMultipleObjects` | back-pressure 0.03 ms in the laptop preset interval, 0.9 ms with D3D12; the 30 µs spin is paid at most once per wait | `gs_stream/gs_stream_ipc.cpp:313-345`, `gs_stream_ipc.hpp:205` (Confirmed) |
| gt4-gs consumer wake-ups | same IPC code on the other side: up to 30 µs spin per wake, 5-13 wakes per frame | at most ~0.4 ms per frame of the gt4-gs CPU thread | F8 `GS process woken from sleep per frame` (Confirmed); spin bound from the code (High confidence) |
| Audio | no thread of its own: WinMM `waveOut` with an event callback, pushed from the SPU2 tick inside `iop::run` and pumped once per VBlank; never waits in paced mode | `audio_push` 0.01-0.05 ms | `host_io.cpp:263-300,422-472` (Confirmed) |
| EE idle | the EE thread sleeps 4.6-10.8 ms of each desktop frame in the pacing wait; when no guest thread is ready, `idle()` jumps straight to the next event, so `guest_idle` is 0.001-0.005 ms | none | `system.cpp:150-163` in the capsule (Confirmed); F8 `guest_idle` |
| VU1 worker idle | asleep in `receive()` (doorbell wait) | none | `mtvu_runtime.cpp:223-245` (Confirmed) |

**Conclusion:** the runtime has no large spinning wait. The only steady spin is the 0.5 ms pacing tail (about 3% of
a logical CPU, roughly 0.1-0.2 W on the laptop: Hypothesis from a 4-6 W busy-core estimate), plus the doorbell
cache-line traffic. **The owner's ~25 extra CPU points are busy work, not spinning** (High confidence: the four
thread totals in section 2.1 already add up to the observed load). The pacing sleep itself is a true sleep and is
where the power headroom comes from.

`host_vblank` (0.34-0.72 ms) is mostly fixed host work at every VBlank: the Win32 message pump, cursor update, one
`GetAsyncKeyState` per keyboard binding (about 20 calls), `SDL_UpdateGamepads` and the pad read, the audio pump and the
present/VSYNC post (`window.cpp:290-360`, Confirmed). Pad rescans run every 2 s and rumble is sent only on change
(`host_io.cpp:102-260`, Confirmed), so no obvious waste; the split is Unknown until it has sub-scopes. One outlier:
the laptop's `session-1792983673` periodic intervals show `host_vblank` 7.0 and 2.0 ms while the launcher was
animating in the background (ai-memory `gotchas/launcher-background-cpu`, fixed in TASK-118).

### 3.2 Run loop, kernel service and guest idle

Read in the capsule `system.cpp` and the donor `runtime/src/kernel.cpp` (Confirmed unless noted).

- **Run loop** (`system.cpp:375-397`): each iteration runs `run_due_events()`, then `kernel::service(c)`, then either
  `idle(c)` or `dispatch(c, 0, 0)`, each under its own scope. The translated code returns to the loop when its
  budget (EE slices of up to 20,000 cycles, `system.cpp:45,85-86`) runs out at a backward branch, on a thread switch
  (`c.pc |= kUnwind`) and after interrupts.
- **`kernel::service`** (`kernel.cpp:417-431`) runs pending alarms, then up to 16 pending INTC/DMAC causes, then
  reschedules. Each handler runs through `run_guest`: save the whole thread context (~690 bytes), set up arguments
  and a kernel stack, then loop on `dispatch(c, 0, 0)` until the handler returns, then restore (`kernel.cpp:172-199`).
  So **the guest's own interrupt, DMA and alarm handlers execute inside the `kernel_service` scope**: it is game code
  plus the delivery overhead, not kernel overhead alone. Small costs on every interrupt: the handler list is
  copied into a new `std::vector` per delivery (`kernel.cpp:205,217`, a heap allocation each time), alarms are
  erased from the front of a vector and looked up in a `std::map` (`kernel.cpp:258-268`), `g_guest_calls.push_back`
  per handler.
- **Scheduler:** threads and semaphores live in `std::map` containers; `best_ready()` scans every thread on each
  reschedule (`kernel.cpp:76-80,118-125`); a switch copies two contexts (`kernel.cpp:127-145`). With 38-53 thread
  switches per frame this is microseconds, not milliseconds (High confidence).
- **Syscalls are cheap:** 129-1,187 per frame measured, 0.03-0.06 ms in total (`[frame] syscalls/frame=N in M ms`
  in every F8). 0x2F GetThreadId and 0x29 ChangeThreadPriority dominate in menus and the showroom (444.8 and 258.4 per
  frame); in races 0x2F is 66 per frame. A fit over four desktop intervals gives about 25 ns per GetThreadId and
  80 ns per ChangeThreadPriority (Hypothesis), and most of the 25 ns is the two clock reads of the always-on
  `SyscallScope` (`slice262-license-guard-runtime/kernel.cpp:572-573`, `native/perf_probe.cpp:67-98`).
- **Who calls them (Confirmed in gt4-decomp and the trace):** a recursive mutex of the game's stream/manager library
  (`src/libstream/unit_005676B0`): lock `func_00576418`, unlock `func_00576640`, wrappers `func_00576788` (lock, 299
  call sites) and `func_005767C0` (unlock, 315); the variants `func_005769F0`/`func_00576A28` (34 sites each) also
  raise the calling thread from priority 24 to 1 and back. Every lock and unlock asks GetThreadId. The title-screen
  trace `private/slice214-smoke/h214-baseline/stderr.log` holds 4,920,060 GetThreadId and 1,255,474
  ChangeThreadPriority calls in 4,200 frames (1,171 and 299 per frame), the priorities always 24 -> 1 -> 24 on the same
  thread. The uncontended path never blocks.
- **Exact short-circuits for them** (small gain, 0.03-0.05 ms desktop): GetThreadId answered inline (it only returns
  the current thread id; keep the budget charge and the counter); ChangeThreadPriority raising the running thread is
  a no-op for scheduling when no reschedule is pending (it is already the best thread); the restore must still look for
  a better ready thread, and a same-priority call on a *ready* thread still moves it in the queue in the donor model
  (`kernel.cpp:632-640`). The donor model is the reference; the real PS2 kernel's requeue rule is Unknown.
- **Guest idle costs nothing measurable:** when no guest thread is ready, `idle()` advances guest time to the next
  event (`system.cpp:150-163`); F8 `guest_idle` is 0.001-0.005 ms per frame on both machines while the EE is idle
  for 4-11 ms of every frame. The game waits mostly by blocking on semaphores (0x44 WaitSema 14.9 per frame) and
  sleeping threads, which the HLE kernel turns into this fast-forward. DMA and VIF transfers complete instantly in
  this runtime, so polling loops on `D_STAT`, `CHCR` or `VIF1_STAT` exit at their first test (High confidence).
- **The game's waits, one by one** (Confirmed in gt4-decomp unless noted):
  - Frame wait: main loop `func_001016E8` -> flip `func_00107878` -> `func_004A21F0` does `while (old == lock->value)
    SleepThread()` on a scratchpad counter that the VBLANK-start interrupt handler (0x4AB430) bumps. A **block**, not a
    spin: it ends in `idle()`.
  - DMA wait for channels 1/2: `func_004A0F68` queues the thread and calls SleepThread until the DMAC handler (0x4AB6D8)
    wakes it. The DMA itself already finished at start (`dmac.cpp:84-91,230-263` in the capsule), but the interrupt is
    delivered at the next unwind, so each wait is one sleep/wake pair (Hypothesis: part of the 38-53 switches per frame).
  - Flip check `func_004A20C8`: three polling loops with 0x1000000-iteration timeouts on `VIF1_STAT`, `VPU_STAT` and
    `GIF_STAT`; in this runtime those bits never read as busy, so the loops run zero iterations.
  - SIF RPC: CreateSema -> SifSetDma -> WaitSema -> DeleteSema about 2.4 times per frame on the title schedule; the
    reply wakes the thread through iSignalSema and the wait ends in `idle()`.
  - Movie playback polls IPU and DMA channels 3/4 (`func_0054C8F0` and neighbours): movies only. Polls of `INTC_STAT`
    or GS `CSR` elsewhere: Unknown (the register-access scan was coarse).
  - **Conclusion:** there is no guest busy-wait to skip. Unlike a JIT emulator, this runtime already turns every guest
    wait into a host fast-forward, so "idle-loop skipping" has nothing left to win here.
- **What must not change:** the number of run-loop iterations and the 20,000-cycle slice decide when the IOP is synced
  and therefore when SIF replies and DMA interrupts are delivered. Fewer or larger slices, fibers per guest thread or a
  different `kMaxSlice` would move guest-visible timing. **Only cheaper iterations are exact** (`system.cpp:45,69-88,
  337-349`; High confidence).
- Two accounting details that change how F8 reads: the 0.5 ms pacing spin runs on the EE thread, so it is inside
  `measured_thread_cpu` (about 9% of the 5.75 ms showroom EE CPU); and the VIF batch copy in `flush_vif_chain`
  (`mtvu_runtime.cpp:476-488` at rc6, called from the capsule's `dmac.cpp:79,86`) has no scope of its own, so its 0.5-2.5 MB per frame lands
  in whichever category is open (usually `ee_dispatch`).

### 3.3 Quality of the generated EE C++

Read in `private/slice217-ee-aspect-producer/generated` (the rc6 v2.00 library) and the donor translator
`private/references/ps2-web-recomp/recomp/codegen.cpp` (824 lines). Confirmed unless noted.

1. **One host function per 16 KiB guest window.** `unit_0040` is 5,227 lines with 344 entries; `void unit_XXXX(Cpu &c,
   std::uint32_t entry, std::uint32_t depth)` starts with `switch (entry)` over every label that can be entered from
   outside (`unit_0040.cpp:138`; `codegen.cpp:193`). Every guest `jal` becomes a host call into another giant function
   that dispatches again through its jump table (95,305 call sites in the library: `if (depth < kMaxDepth)
   unit_0130(c, 61u, depth + 1u); ... if (c.pc != 0x0020001Cu) return;`, `codegen.cpp:277-279`). The return is checked
   by comparing `c.pc` after the call. Function returns are real host returns, so the return predictor works; the
   indirect jump at the top of each unit is shared by all its callers (Hypothesis: a frequent mispredict).
2. **All guest state lives in memory behind `Cpu &c`.** GPRs are 128-bit unions `c.r[N]` (`include/ps2/cpu.hpp:18-58`).
   `g_ram` is a global byte array (`include/ps2/memory.hpp:14`) and `c` is an unqualified reference, so clang must
   assume a guest store `st32(addr, v)` can overwrite `c`: values cannot stay in host registers across guest stores
   (168,402 store sites) or calls (95,305 sites) (High confidence from the C++ aliasing rules; no disassembly was taken:
   open measurement M12). A `__restrict` (noalias) `c` would lift most of it, but runtime code reaches the CPU
   state through the global `g_cpu` too (`request_iop_sync` lowers `g_cpu.budget`, `system.cpp:347-349`), so slow
   paths must take `c` explicitly first, or the change is unsafe.
3. **Memory helpers are inline and cheap.** `ld32` = `a < 32 MiB` compare, then a `memcpy` load; scratchpad and MMIO
   go to out-of-line `slow_*` functions (`memory.hpp:28-93`). 226,393 load and 168,402 store sites. No TLB, no page
   table: this is not where the overhead is.
4. **Cycle accounting is per block, not per instruction.** `c.budget -= N` once per basic block (298,321 sites, a
   memory read-modify-write each, because of item 2); `preempt(c)` only on backward branches when the budget is
   negative (8,906 sites, `codegen.cpp:267`). The owner hypothesis "per-instruction accounting" is therefore **not
   confirmed**: the cost is one subtraction per block plus the memory traffic of item 2.
5. **FPU is exact software emulation.** `fp::add` aligns and normalises mantissas in integer code with several
   branches; `fp::mul` multiplies mantissas in 64-bit and repacks (`include/ps2/fpu.hpp:24-67`). It implements the
   donor's model of the PS2 FPU (denormals flushed, no infinity or NaN, truncation, a single guard bit in add and sub),
   so it cannot be swapped for `addss`/`mulss` without a proof that the results are identical. Static sites: 8,447 `fp::mul`, 4,884 `fp::add`, 3,466
   `fp::sub`, 8,452 `fp::key` comparisons, 1,551 divides. Dynamic counts are Unknown (no profile).
6. **Indirect jumps** inside a unit use the entry table and `goto dispatch_entry`; across units they call `dispatch()`,
   which finds the unit with a **32-bit division by a runtime unit size** (`runtime/src/dispatch.cpp:70-74`) on every
   call (Confirmed in code; count per frame Unknown).
7. **Syscalls and kernel HLE are cheap.** 129-1,187 syscalls per frame cost 0.03-0.06 ms in total (F8 and periodic
   `syscalls/frame ... in N ms`). The owner hypothesis "kernel/syscall" is confirmed only for `kernel_service` (section 3.2), not for
   syscalls.

Hottest guest functions: **Unknown** — no sampling profile exists. How to get them later (M3): a Windows Performance
Recorder CPU profile of a headless day and night race (`wpr -start CPU`, elevated, desktop only), resolved with the
library's PDB to source lines, each `L_XXXXXXXX:` label mapped to its gt4-decomp function name through
`docs/EE-NAMES.md`; with item 1 the profile resolves only to units unless line information is used.

### 3.4 Remaining interpreters

- **IOP (Confirmed design, cost split Unknown).** A plain fetch/decode/switch interpreter with no block cache;
  loads and stores call out-of-line `read32/write32`; each instruction calls `kernel::switch_pending()` in another
  translation unit (`runtime/src/iop/cpu.cpp:55-207`, `hw.cpp:40`); HLE imports go through a `std::function`
  (`cpu.cpp:193-200`). Slices are at most 512 instructions and clipped to the next event (`iop/kernel.cpp:34,592-616`);
  events sit in a `std::priority_queue` that copies a `std::function` per pop. The EE syncs the IOP from every
  `preempt` (EE slices of up to 20,000 cycles, so at least ~246 syncs per busy frame) and after SIF syscalls with a
  64-cycle EE slice (`system.cpp:45,337-349` in the capsule).
- **The SPU2 mixer is inside `iop_run`** (Confirmed in code, magnitude Unknown): 48 voices (2 cores x 24) with ADPCM
  decode, envelope and interpolation run as an IOP event, 25 ticks of 32 samples per frame
  (`runtime/src/iop/spu2.cpp:20-24,235-269`). Its cost grows with the active voices, which fits `iop_run` rising from
  0.13-0.18 ms in menus to 0.4-0.5 ms in races on the desktop and 1.1-1.5 ms in laptop races. `PS2_SPU2_COUNT=1`
  already prints the active voices (free measurement, not done).
- **What to do:** measure first (M4: per-module IOP instruction counts, TASK-016 "deferred"; SPU2 voices and time).
  Interpreter hygiene in an overlay (inline the RAM fast path and the switch check, no `std::function` copies) is
  1-2 days; full IRX AOT is weeks for an estimated 0.2-0.3 ms on the desktop (`docs/FRAME-COST-STUDY.md` item 10).
  If the SPU2 mix dominates, the right tool is a vectorised mixer with identical integer results, not IOP AOT.
- **VU0 micro: already AOT** (TASK-013; all 14 `VCALLMS` entries compiled, `interpreted_calls=0` in every F8).
  `vu0_aot` costs 0.04-0.40 ms. Nothing left to compile; only a per-call overhead trim.
- **VU1: already AOT** on its own thread; the worker is not on the EE critical path but is the second-largest CPU
  consumer on the laptop (9.3 ms). Program quality is the open lever (`docs/FRAME-COST-STUDY.md` item 8).

### 3.5 Build flags and baseline ISA

- Generated EE library: clang-cl 23.1.2 `/O2 /Ob2 /EHsc -std:c++20`, no `-march`, no LTO or PGO, pinned by
  `scripts/ee_producer.py:261-262` (Confirmed in the slice217 `build.ninja`). Runtime and `native/`: clang-cl 22.1.3,
  same flags plus `/W4`, linked with `lld-link`, no LTO (Confirmed in the slice268 `build.ninja`).
- **The game executable is built for baseline x86-64 (SSE2 only), while the launcher already refuses any CPU without
  SSE4.1** (`launcher/crates/gt4l-core/src/system_check.rs:6-8`, `docs/BUILD.md:1757-1759`, Confirmed). Compiling
  the runtime, the VU1/VU0 AOT code and the EE library with `-msse4.1` (exactly the launcher's minimum: SSSE3 +
  SSE4.1, not `x86-64-v2`, which would also require SSE4.2 and POPCNT that a 45 nm Core 2 lacks) costs no
  compatibility. SSE4.1 adds 32-bit signed min/max, blends and zero/sign-extending loads that the VU float clamping,
  VIF unpack and MMI code can use; it adds no FMA, so results stay bit-identical (High confidence). Gain Unknown.
- The clearest case is the VU1 worker. Every VU1 AOT vector result and every shadowed register load goes through
  `vfix`, whose core is `__builtin_elementwise_min` on four signed 32-bit lanes
  (`runtime/include/ps2/vu_ops.hpp:136-150`; emitted as `vfix(vload(...))`, `vmuln`, `vmaddn` in
  `private/slice157-vu1-all-gen/vu1_generated_*.cpp`). SSE2 has no signed 32-bit lane minimum, so clang builds it from
  a compare and three logic operations; SSE4.1 has one instruction (`pminsd`) (Confirmed ISA fact; the worker-time
  gain is a Hypothesis, plausibly 5-15% of `vu1_program`, which is 8.8 ms per frame on the laptop).
- `GT4_X86_64_V3` (AVX2, FMA, with `-ffp-contract=off`) exists for the runtime only (TASK-010) and was never
  measured on an idle machine (`docs/BUILD.md:700-760`). The i5-7200U has AVX2; a second executable chosen by CPUID
  in the launcher would keep the SSE4.1 floor; owner decision of 2026-10-28: no second executable unless M9 shows a
  large gain (section 7).
- ThinLTO and PGO were never tried (TASK-010). They need one compiler version for the library and the executable.
  PGO profiles from the headless day/night/menu schedules are deterministic and need no owner session.

### 3.6 GSdx-process costs that CoreGS 2.0 removes (do not optimise twice)

The GS stream path today: the VU1 worker writes GIF output into the MTVU output ring; the EE forwards each record
straight from that ring into gt4-gs's 32 MiB shared ring (`gs_ipc_write`, one copy, `mtvu_journal.cpp:248-275`,
`gs_stream_client.cpp:302-316,841-852`, `gs_stream_ipc.cpp:84-122`); PATH2/PATH3 data is copied from guest memory;
the gt4-gs CPU thread reads the ring and feeds GSdx. About 4.9 MB per frame crosses this path in the laptop race,
far more than the laptop's 3 MB L3.

| Cost | Laptop race (ms per frame) | Gone with an in-process CoreGS 2.0? |
|---|---:|---|
| `gs_ipc_write` on the EE (ring copy, record header, padding, release store) | 1.38 | yes, if CoreGS consumes the records in place (see below) |
| gt4-gs CPU thread | 3.4 (8.9 with D3D12) | yes |
| IPC wake-ups (`SetEvent`, about 12 per frame), VSYNC post, frames-in-flight handshake, readback round trips | small, plus back-pressure 0.03-0.9 | yes |
| gt4-gs GS thread (GSdx and the D3D11 driver) | 10.6-11.8 | **replaced** by CoreGS 2.0's own cost, not saved |
| `gs_drain` (MTVU journal front end), PATH2/3 GIF scan and signal pre-apply, worker->EE doorbells | 0.38 + part of `ee_dispatch` | **no**, game side |
| `vif_gif` (VIF1 shadow parse and the copy into the MTVU input ring) | 1.19 | **no**, game side |

Requirements for the game-side intake that CoreGS 2.0 will consume (recorded here so neither side optimises the
GSdx path twice; the CoreGS 2.0 internals are TASK-027's business and were not read):

1. **Zero-copy intake:** the renderer reads worker GIF output in place from the MTVU output ring (borrow, then
   release), and EE PATH2/PATH3 data from a small staging ring, in journal order. No intermediate stream copy.
2. **Its GIF front end runs on its own thread, never on the EE** (the CoreGS 1.1 lesson: its front end on the EE cost
   3.5 ms per race frame, `docs/FRAME-COST-STUDY.md` table 2.1).
3. **Rings sized for a 3 MB L3** where possible, with back-pressure instead of a 32 MiB buffer.
4. **The same identity hooks:** a stream hash equal to `GT4_GS_STREAM_HASH` of the GSdx path, so that the game side is
   proven unchanged when the renderer changes.

Until CoreGS 2.0 replaces GSdx on the laptop, the GSdx path is frozen (ai-memory `decisions/gt4gs-frozen`): no work
on `gs_stream_ipc.cpp` or gt4-gs beyond bug fixes is planned here.

### 3.7 MTVU and VIF data path, and the always-on probe

- VIF1 packets are copied three times before the worker unpacks them: into `vif_batch` (`mtvu_runtime.cpp`
  `post_vif_packet`), into the MTVU input ring (`post_job`), and out of the ring into a `std::vector` on the worker
  (`SpscByteRing::try_pop` -> `payload.assign`, `mtvu_ring.hpp:162-184`; `worker_main`, `mtvu_runtime.cpp:223-245`).
  0.5-2.5 MB per frame each time (Confirmed sizes from F8 `bytes/job` x `jobs/frame`). The worker could read the
  batch in place with `try_peek` as the EE already does for output (`mtvu_ring.hpp:188-213`), and the EE could build
  the batch directly in the ring. The guest buffer must still be copied once, because DMA completes instantly in our
  model and the game may reuse the buffer.
- The shadow VIF1 parse on the EE already skips the unpack itself (`vif.cpp:132-137` in the capsule, "dry" mode), so
  `vif_gif` is the command walk, the batch copy and PATH2 signal scans, one perf scope per packet
  (2,600-8,800 packets per frame).
- **The release build pays for the detailed probe.** Every `Scope` reads `steady_clock` on entry and exit, also on the
  worker (`perf_probe.hpp:147-163`); per frame that is roughly 2 x (VIF packets + GS stream records, each wrapped in
  a `gs_ipc_write` scope at `gs_stream_client.cpp:846` + run-loop iterations x 3 + IOP syncs) on the EE, so 8,000-20,000
  clock reads, and 2 x (VU1 program calls, ~1,700) on the worker. Hypothesis: 0.1-0.5 ms per frame on the laptop across
  both threads. The slice145 matched-overhead pair was inconclusive (`docs/PERFORMANCE-ATTRIBUTION.md` slice148).
  Options: per-chain instead of per-packet VIF scopes, and a cheaper clock (one `rdtsc` per scope) while keeping F8.
- `std::map<std::string, std::uint64_t> syncs` is updated by name on every MTVU sync and deferral
  (`mtvu_runtime.cpp:141,445,604-616,699`), a string build and tree lookup per call (small, Confirmed in code).

### 3.8 New in rc7: RetroAchievements on the EE thread

Read at `c21e934` (Confirmed in code; costs Unknown, nothing was run):

- `on_vblank()` now calls `gt4::achievements::on_vblank` on the EE thread, inside the `host_vblank` scope, after the
  audio pump and before the present (`native/window.cpp` near line 365). When the player is not signed in to
  RetroAchievements, or the game is Spec II, it returns after one atomic load (`native/achievements.cpp:182-184`).
- Signed in on v2.00, it runs `rc_client_do_frame` every VBlank, which evaluates every active achievement condition
  against guest RAM, then `overlay_.tick` (`achievements.cpp:190-198`). Network requests run on a separate worker
  thread (`achievements.cpp:155`), so HTTP never blocks the EE.
- The overlay is a layered window drawn with GDI+ over the **whole client area**, then `UpdateLayeredWindow` with
  per-pixel alpha (`native/achievements_overlay.cpp:342-388,391-606`). It redraws on every VBlank while a toast animates
  (an unlock, a progress notice) and whenever its content is dirty. At 1366x768 that is a software-rendered 4 MB ARGB
  frame plus its upload to the desktop compositor every frame of the toast, on the EE critical path (Hypothesis:
  several ms per frame on the laptop, so a visible hitch at every unlock; at 3440x1440 about 20 MB per frame).
- Plan impact: P0 gives `achievements` its own scope (M13, M14); P1 gains item (g): draw the toast only in its own
  rectangle (a small layered window sized to the toast) and render it off the EE thread, keeping the
  `rc_client_do_frame` call where it is (it must read guest RAM at the VBlank). Both are host-only and invisible to
  the game.

## 4. Ordered plan

Every step is one variable with an A/B switch or a separate pinned build, as AGENTS.md requires. Gains are
**Hypotheses** in laptop ms per frame (EE critical path unless a thread is named), derived from the measured category
sizes of section 2; the desktop runs of section 5 decide them. "Identity" lists how the step proves the game is
unchanged. Effort is agent days, not counting builds.

| Step | What | Laptop gain (Hypothesis) | Effort | Risk | Identity proof |
|---|---|---|---|---|---|
| **P0** | Measurement first: sub-scopes for `kernel_service` (delivery vs guest handlers), `host_vblank`, `iop_run` (interpreter / SPU2 mix / HLE); counters for run-loop iterations, IOP syncs, `dispatch()` calls and scope entries; a per-VBlank guest-state hash (EE RAM, IOP RAM, SPU2 RAM, CPU state) behind a switch; `--cpu-mask` for 2-core/4-thread runs; one WPR CPU profile on the desktop (M3) | none (diagnostics) | 2-3 | low | stream hash and frame dumps equal with the new counters on and off |
| **P1** | Host quick wins, each behind its own switch: (a) build runtime, native and VU AOT code with `-msse4.1` (plus one `x86-64-v3` experiment build for M9, never shipped); (b) doorbell diet: statistics counters per thread instead of a locked add on the shared line, cache-line padding of both bells, power-of-two ring masks; (c) worker reads VIF batches in place (`try_peek`) instead of copying into a `std::vector`; (d) probe: one VIF scope per DMA chain, `SyscallScope` timing only inside F7..F8 intervals (counts kept), a cheaper clock; (e) pacing tail spin 0.5 ms -> about 0.15 ms with `_mm_pause` (measure timer lateness first); (f) MMCSS "Games" for the EE thread, EE and VU1 threads steered to different physical cores; (g) RetroAchievements toast drawn only in its own small window and off the EE thread (section 3.8) | EE 0.3-0.8; VU1 worker 0.5-1.5 (mostly 1a); ~0.1-0.3 W | 3-5 | low | stream hash, frame dumps, `GT4_MTVU_VERIFY=1` 0 mismatches, CTest/Python, laptop F8 |
| **P2** | Kernel delivery and run loop hygiene in a runtime overlay (section 3.2): no `std::vector` copy of the handler list per interrupt (copy only when the list changed), alarms in a small sorted array, flat thread and semaphore tables, the event queue without `std::function` calls per event, GetThreadId answered inline and the ChangeThreadPriority raise-self fast path; then whatever the P0 split of `kernel_service` shows outside the guest handlers. Never fewer iterations or larger slices: only cheaper ones | 0.1-0.4 (the guest handler code itself speeds up with P4 and P8) | 1-2 | medium | guest-state hash every 500 VBlanks, stream hash, syscall trace equal on a short schedule |
| **P3** | IOP and SPU2, chosen by the P0 split: interpreter hygiene overlay (inline RAM fast path and the switch check, no `std::function` copy per event, no `std::map` scan per switch); if the mix dominates, a vectorised SPU2 mixer with identical integer output | 0.3-0.8 of the 1.27 ms | 2-4 | medium (audio timing, SIF) | audio PCM hash of the headless schedules, guest-state hash, card bytes after a save |
| **P4** | Build-level speed for the translated code (`ee_dispatch` 4.9 ms in races, 6-8 ms in 3D menus), no change to the generated C++: (a) one compiler version for library and executable, then ThinLTO, then PGO from the headless schedules; (b) EE library rebuilt with `-msse4.1`; (c) `dispatch()` division -> shift | 5-15% of EE CPU (Hypothesis) | 3-5 | low | guest-state hash every VBlank on all schedules, stream hash, frame dumps; new library pin |
| **P5** | VIF/MTVU data path: EE builds VIF batches directly in the input ring (one copy fewer); fewer worker->EE records per job where the stream hash allows | EE 0.2-0.5; worker 0.2-0.4 | 2-3 | medium (record boundaries; check how `GT4_GS_STREAM_HASH` frames records before merging any) | stream hash, MTVU verify, frame dumps |
| **P6** | CoreGS 2.0 intake (with TASK-027 integration, game side only; shipped together with CoreGS 2.0): the requirements of section 3.6 | EE 1.4 + gt4-gs CPU thread 3.4 (8.9 on D3D12) no longer exist; GS thread cost becomes CoreGS 2.0's | part of TASK-027 integration | owned by TASK-027 | same stream hash as the GSdx path on the headless schedules |
| **P7** | VU1 worker program quality (worker 9.3 ms, the second-largest thread): after P1a, fewer `vfix` where the operand is provably already fixed, register shadows kept across blocks, PATH1 output batching (`docs/FRAME-COST-STUDY.md` item 8) | worker 1-3 | 5-10 | medium | VU1 corpus differential (existing), `--vu1-verify N`, frame dumps |
| **P8** | Translator, **the most complex step, last** (owner, 2026-10-28): (a) `Cpu & __restrict c` once every slow path and callback takes `c` explicitly instead of `g_cpu`; (b) one host function per guest function (gt4-decomp boundaries, as TASK-014 used for entries) with direct calls, keeping the 16 KiB window switch only for indirect entries; (c) FPU fast paths only where M3 shows `fp::` hot and an exhaustive or proven equivalence exists | 20-40% of translated code: 1-2 ms in races, 1.5-3 ms in 3D menus | (a) 3-5; (b) 10-15 | medium-high | new producer contract; guest-state hash every VBlank on all schedules (old and new library must agree bit for bit), stream hash, frame dumps, replay and save identity, owner play-through |
| **P9** | IOP IRX ahead-of-time compilation | 0.2-0.5, only if P3 shows the interpreter dominates | weeks | medium | as P3 |

Expected outcome (Hypothesis, to be replaced by measurements): after P0-P5 on GSdx, the laptop race critical path goes
from 12.2 ms to about 9-10 ms and the four busy threads from about 35 to 28-31 ms per frame; with P6 and a CoreGS 2.0
GS thread no heavier than GSdx's, about 8 ms and 24-27 ms. P7 and P8 are the steps that take it into and below the
PCSX2 class (~50% of the laptop): about 6.5-7 ms and 20-23 ms.

Why this order (owner rule, 2026-10-28: **always the easiest and cheapest step first, built and tested right away;
the most complex work last**): P0 is cheap and turns the Unknowns into numbers. P1 is the cheapest risk-free set and
helps every thread on a 2-core machine. P2 and P3 are small runtime overlays. P4 speeds up the translated code
without touching it. P5 trims copies. P6 is done with CoreGS 2.0 and ships with it (the GSdx bridge gets no more
work). P7 works on the VU1 emitter, which already has differential tests. P8 changes the EE translator and needs a new
producer contract: the largest lever for the game code, and the most expensive, so it comes last, guided by P0's
profile. P9 only if P3's numbers ask for it.


## 5. Measurement protocol (to run later; nothing here was run)

No step of this protocol may run while another build, benchmark or CoreGS 2.0 measurement uses the desktop (owner
rule; `docs/FRAME-COST-STUDY.md` section 1.1 has the checker that waits for `ninja`, `clang-cl`, `lld-link`, `cargo`,
`rustc`, `msbuild`, `makensis`).

**Desktop, per candidate (agent, headless, about 1 hour per variant):**

1. Build the candidate in a fresh `private/sliceNNN-*` directory with its own pin; the baseline is the rc7 build
   (pins `docs/inputs/slice335-pcsx2.json` for v2.00 and `slice336-spec2.json` for Spec II; same EE library
   `private/slice217-ee-aspect-producer` as rc6, so section 3.3 holds), measured with RetroAchievements signed out
   unless the run measures section 3.8.
2. **Main scene (owner, 2026-10-28): one fixed night race in Arcade Mode**, same car and same track every run; Arcade
   goes straight to the race and is predictable, while GT Mode needs a car bought on the card and many menu steps. The
   existing Arcade night schedule fits: Arcade > Single Race > City Courses > **Clubman Stage Route 5 (night)**, Ford
   Ka, 5 AI, accelerator held by the script, so every run drives the same race (`scripts/frame_cost_headless.py` scene
   `night`, 16,000 VBlanks, presses in `docs/FRAME-COST-STUDY.md` section 1.1, pinned seed card
   `private/slice110-menu-pilot/name-control4`). Night adds the flare readbacks.
   - The owner asked for B-Spec because the game's AI then drives and its changing cameras show the heavy lighting
     moments. B-Spec exists only in GT Mode (High confidence; the first headless run checks the Arcade menus). In Arcade
     the same camera variety comes from the **replay of that race** (TV-style cameras), so the schedule is extended to
     finish the race and play its replay, measured as a second window. Building that extension is part of P0.
   - The CoreGS 2.0 work's GT Mode B-Spec night scene (FF Challenge race 3, Hong Kong Reverse, VW Lupo; ai-memory
     `concepts/coregs2-state`) is **not** used as the main scene, by owner decision; it stays available for a
     cross-check with CoreGS 2.0 numbers.
   - Secondary schedules from `scripts/frame_cost_headless.py` (rc6): menus (4,200 VBlanks), showroom (7,000), day
     race El Capitan (16,000).
   - Order A, B, A, B (two runs per variant), surfaceless, 1x and the owner's 3x, `GT4_GS_STREAM_HASH=1`,
     `GT4_MTVU_REPORT_EVERY=500`.
3. Weak-CPU shape: run the day and night race again with the process limited to **two physical cores and their SMT
   siblings** (logical CPUs 0-3 on this Ryzen). `scripts/cpu_affinity.py --cpu-cores 4` picks four physical cores, so a
   `--cpu-mask` option is needed first (small script change, part of step P0).
4. Report per stage, ms per VBlank, from the 500-VBlank reports of the scene window (the study's index ranges): critical
   path, EE CPU, every exclusive category, `gs_ipc_write`, `gs_drain`, MTVU waits, worker busy, gt4-gs CPU and GS
   threads, stream bytes and records, syscalls, thread switches, VIF packets and jobs; median of the two runs; flag any
   difference under the A/A spread (about 2-3%, `docs/FRAME-COST-STUDY.md` section 2.1).
5. Identity gates (every candidate): session stream hash equal to the baseline on every schedule; frame dumps equal
   (`dumps.sha256`); `GT4_MTVU_VERIFY=1` 0 mismatches on the day race; CTest and the Python suite on the candidate
   build. For runtime or translator changes also the guest-state gate of step P0 (EE RAM, IOP RAM, SPU2 RAM and
   CPU-state hash every 500 VBlanks) and the audio PCM hash.
6. M3 (once, P0; owner approved on 2026-10-28, to run when the owner says the CoreGS 2.0 build is done and the desktop
   is free): a Windows Performance Recorder CPU sampling profile (`wpr -start CPU -filemode`, elevated) of the headless
   Arcade night race (and its replay) and the day race on the baseline, resolved with the PDBs; top 50 functions,
   the translated ones mapped to gt4-decomp names.

**Laptop, per milestone (owner runs it by hand; no agent runs on the laptop):**

0. The agent builds and checks each candidate headless on the desktop first, then copies only the program files into
   the laptop's portable folder over the network (`\\10.1.1.151\GT4Recomp`, mapped as `Z:` here) when the laptop is
   on, keeping the old files in `Z:\_program-backups\<stamp>` and hashes in a receipt (as on 2026-10-26). The
   laptop's save is a test save (owner, 2026-10-28). After the owner's run the agent copies `Z:\data\sessions\*\game.log`
   back into the task worktree's ignored `private/`.
1. Launcher closed or idle (TASK-118 fix), power adapter connected, same Windows power mode each time; no overlay.
2. Both games are installed there (owner, 2026-10-28): v2.00 and Spec II. The same Arcade night race (Clubman Stage
   Route 5, Ford Ka) and its replay, and Time Trial Nordschleife with the Performance Mode preset at 1x, Direct3D 11 (the existing laptop
   reference). F7 when the race runs, F8 after about 60 s. Then one 3D menu stretch (car select or showroom) with
   F7/F8, and, when available, a replay (open measurements M5, M6).
**Matched desktop/laptop pair (M1/M2, once):** run the laptop scenes (Spec II and v2.00, TT Nordschleife, Performance
preset, 1x, D3D11, and the Arcade night race) on the desktop too, with the same car, so the per-category ratio between the machines is measured instead of
inferred (section 2.4).

## 6. Open measurements (only a run can answer them)

| Id | Question | How |
|---|---|---|
| M1/M2 | Real laptop/desktop ratio per category on the same scene | matched pair above |
| M3 | Hottest translated functions; share of FPU emulation, dispatch, `fp::` helpers | WPR profile (protocol step 6) |
| M4 | Split of `iop_run`: interpreter vs SPU2 mix vs HLE; IOP syncs per frame | P0 sub-scopes, `PS2_SPU2_COUNT=1` |
| M5 | Night race and replay on the laptop (no F8 exists) | owner F8 |
| M6 | 3D menu dips on the laptop: which screens, which category | owner F7/F8 in a menu |
| M7 | Cost of the always-on probe (clock reads per frame) | P0 counter of scope entries, then an A/B with a cheap clock |
| M8 | Cost of a doorbell ring under contention on 2 cores | P1 A/B with per-thread statistics |
| M9 | Whether `-msse4.1` or `x86-64-v3` changes anything measurable on the VU1 worker | P1/P4 A/B, worker busy |
| M10 | Split of `kernel_service` (delivery overhead vs guest handler code) | P0 sub-scope |
| M11 | Split of `host_vblank` (message pump, keyboard, SDL, audio pump, present) | P0 sub-scopes |
| M12 | Whether clang really reloads guest registers after every guest store in the generated units | disassembly of two hot units (`llvm-objdump` of the pinned library), P0 |
| M13 | Cost of `rc_client_do_frame` per VBlank with a real achievement set (signed in, v2.00) | P0 `achievements` scope, F8 with RetroAchievements on and off |
| M14 | EE time per frame while an achievement toast animates, at 1366x768 and at the owner's 3440x1440 | same scope, an unlock or a test toast in a headless-like run |

## 7. Owner decisions (2026-10-28)

| # | Question | Owner's answer | What it means for the plan |
|---|---|---|---|
| 1 | Second executable for AVX2 CPUs? | Measure first. If SSE2 -> SSE4.1 already gives enough, that may be all; AVX2 may be validated, but no second build or executable for now, only if it gives a large gain. | P1a and P4b build with `-msse4.1` (the launcher's existing floor; SSE4.2 adds nothing this code uses and would drop the 45 nm Core 2 the launcher accepts). `x86-64-v3` is only an A/B experiment (M9); a second executable is reopened only on a large measured gain. |
| 2 | New EE library (translator changes)? | Valid, but the most complex work goes last; always the easiest and cheapest step first, tested right away. | The translator work moved to P8; P4 keeps only build-level changes (one compiler, ThinLTO, PGO, SSE4.1 library). |
| 3 | Thread priority and core placement? | Valid. | P1f goes ahead behind a switch, measured on the laptop. |
| 4 | Windows Performance Recorder profile? | Yes, but not now: the owner says when the current CoreGS 2.0 build is done; then the first baselines and the profile run, to have metrics of today's hardware path before CoreGS 2.0 continues. | P0 and M3 wait for the owner's signal; nothing runs on the desktop before it. |
| 5 | v2.00 on the laptop? | Already installed there, not benchmarked yet. | Laptop runs cover both games (protocol above). |
| 6 | CoreGS 2.0 intake and the GSdx bridge? | No effort on optimising GSdx; CoreGS 2.0 has progressed a lot, and these optimisations ship together with CoreGS 2.0. | No work on `gs_stream_ipc`/gt4-gs; P6 is part of the CoreGS 2.0 integration and the game-side steps target the 2.0.0 release. |

Test method (owner, 2026-10-28): headless runs on one fixed race in Arcade Mode (same car, same track, at night; the
owner prefers B-Spec, which Arcade lacks, so the race's replay supplies the varied cameras) on the desktop; the laptop
only with a finished build, tested by hand by the owner with F7/F8, logs collected over the network.

## 8. Results of the rc9 work (2026-11-02)

The owner freed the desktop on 2026-11-02 with CoreGS 2.0 merged into this branch. The owner also changed the baseline: **the recorded lap on George V Paris, Spec II** (`scripts/coregs2_bench/replay-run.sh george-volta2`, a Ford GT in time trial, the heaviest track), replacing the Arcade night race of section 5; the v2.00 night race with 5 AI cars (`night-run.sh`) is the second parameter. Tools: `scripts/coregs2_bench/game/ab.sh` (interleaved A/B with report lines), `quiet_cpu.ps1` (whole process with no report on), `game_identity.sh` (physics and frames), `laptop/lap.ps1` (the same lap on the notebook over SSH).

### What the profile said (Confirmed, sampler on rc8-based builds, ms per VBlank, 1x)

| Thread | Desktop | Notebook | Largest items |
|---|---|---|---|
| GS worker | 7.2 | 17.5 | driver 1.4 / 2.7, flat otherwise |
| VU1 worker | 3.3 + waits | 12.6 | `vfix` and the operations around it, VIF unpack 13%, clock 11% before the change |
| EE | 2.5 + waits | 10.5 | translated code 21-30%, exact FPU 20-25%, SPU2 5%, clock 6% before the change |

Answers to open measurements: M3 is this profile. M7 (the probe) was large: 11% of the worker and 6% of the EE. M9 was answered by TASK-027. M10: `kernel_service` is guest handler code; the delivery overhead shows at 0.02 ms, so **P2 is not worth doing**. M12 is unanswered: a noalias `Cpu &` is P8, not attempted.

### What was kept

Every step leaves the race clock, the simulation state and the frames identical to rc8.

| Step | Where | Effect (desktop unless noted) |
|---|---|---|
| Scopes read the TSC, not `steady_clock` (`native/perf_clock.*`) | P1d | EE thread about -10% with the report on |
| No clock read, scope stack, stage timer or pass timestamp without a report | P1d | process -2.6% for the scopes and timers; 2-5% of the frame for the pass timestamps |
| Doorbell statistics without a locked add; `receive()` keeps its buffer | P1b, P1c | small, inside the A/A spread |
| `vfix` integer compare (all 2^32 values) and `mac_of_result` without branches | P7, no regeneration | VU1 program 3.50 -> 3.23 ms, -8% |
| EE library rebuilt with this executable's compiler and SSE4.1 | P4a, P4b | no change by itself, physics identical |
| **Spec II VU0 programs compiled** (it had none: 3.1 million interpreted calls in a lap) | not in the plan | found by the profile; 0 interpreted calls |
| **PGO and ThinLTO of the whole game** (`scripts/coregs2_bench/pgo-all.sh`) | P4a | `ee_dispatch` -28%, VIF path -38%, VU1 -9% |
| `fp::add`, `fp::mul` without unpredictable branches | not in the plan | -1% on the notebook, no change on the desktop; kept because exact and free |
| **VU0 macro operations over four lanes** (`native/fp4.hpp`) | not in the plan | `ee_dispatch` a further -16% before PGO |

Result, rc8 against rc9 (slice382, slice390), interleaved, in a player's session (no report): i5-7200U **33.87 -> 27.64 ms of process CPU per VBlank (-18%)**, wall 15.77 -> 14.48; Ryzen 7 5700X **12.59 -> 10.14 (-19%)**, wall 5.88 -> 5.43. The study's Hypothesis of 28-31 ms for the threads on the notebook is met at 27.6 in a scene heavier than its reference, on CoreGS 2.0.

### What was tried and dropped

- `__restrict` on the VU registers in the generated VU1 code: -2.5% of the VU1 program, not worth a promise the runtime's global accesses could break.
- Fixing threads to cores: already measured by TASK-027 (IN-488), no gain.

### Left for later, cheapest first

- VU0 AOT lanes through `fp4` (Spec II 0.5 ms of the notebook's EE, emitter change plus the differential tests).
- The worker reading VIF batches in place (P1c, about 2% of the worker), the EE building them in the ring (P5).
- Masked VIF unpack without the per-element loop; the SPU2 mixer (0.75 ms on the notebook, exact output required).
- `Cpu & __restrict` and one host function per guest function (P8): the largest remaining lever on the translated code, and the riskiest for the physics. A wrong assumption in a slow path changes timing, which the identity gates would catch only on the scenes they run.
- The pacing tail spin (P1e) and the RetroAchievements toast (P1g): need window runs, not touched. The owner was using the desktop remotely.
- The GS worker is now the largest thread on both machines.

### Found on the way

- **A rendering race, on rc8 as well:** on the lap at VBlank 3750 a block of about 16 x 7 pixels of a track sign is sometimes dark for one frame, 2 runs of 12 on rc8 and 2 of 12 on rc9, also at 4500 in other runs. The simulation state stays identical. This is probably what ai-memory `procedures/coregs2-replay-bench` calls differences at picture edges; it is not crowd flashes (Confirmed by looking at the frame; cause Unknown).
- **A bug in the VU emitter, caught by the differential test:** an E-bit instruction that is also a branch must leave TPC at the branch target. Only Spec II's VU0 has it (0x3B0, entries 0x258 and 0x268). Fixed; the v2.00 VU0 table comes out byte-identical. The guest never reads TPC there, so no game behaviour depended on it.
- A build folder reused with a new profile fails to link under ThinLTO (`ProfileSummary` conflict): `pgo-all.sh` now starts clean.
