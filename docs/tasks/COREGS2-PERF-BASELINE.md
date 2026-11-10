# CoreGS 2.0 performance baseline (TASK-215, 2026-11-10)

> **Base of the current numbers (2026-11-10, evening).** The tree is the merge of `origin/release/1.3.0-rc15`
> (`f86d53d9`, the owner's live line; rc15 is 129 commits ahead of `origin/main` `59f27aa1`) into
> `task/215-coregs2-perf` (`fc1a61c5`). rc15 already carries TASK-168 (palette segments), TASK-174 (presentation off the
> GS thread on one-queue devices), TASK-178 (submit after a waited readback, off by default), TASK-179 (device profile) and
> TASK-186 (separate effect toggles), so the audits' present, CLUT-flush and slot items are partly answered there.
> **The numbers in the next section (measured on `main` 59f27aa1) are historical**; the current matrix and wait budget are
> in §1b. IN-540's A/B was re-run on the rc15 base (§1c) and holds.

## 1. Historical matrix (main 59f27aa1, plain build, before the rc15 merge)

Measured on the owner's desktop only (the policy of 2026-11-10: no laptop numbers, the Avell is not a performance
parameter). Every figure here comes from a run made for this page; older ledger numbers are quoted only where they are
labelled as history.

- Machine: Ryzen 7 5700X, Radeon RX 9060 XT, Windows. `C:` had about 5-6 GB free during the work.
- Build: `build/perf-base`, Release, clang-cl 22.1.3 (Clang 22), Ninja, **no PGO and no ThinLTO** (a plain build: the
  ledger's release builds carry both, IN-473), `GT4_HW_GS=ON`, `GT4_HW_CORE=2`, production EE library, the slice262
  frame-cost runtime capsule, the slice262 license-guard capsule, the slice397 VU1 table, the slice393 v2.00 VU0 table,
  the slice200 port and the slice153 pacing shaders. `gt4vulkan.exe` 88,624,640 bytes.
- Shader cache: `build/shader-cache-215`, warmed once. Without it every run compiles about 97 specialised pipelines
  (32.8 s of worker CPU); with it, 73 ms and a maximum of 1.1 ms per compile. Every run below uses it.
- Timers: `GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600` (the stage timers are off in a player's session).
- Headless, unpaced: the wall per VBlank is how fast the whole emulator can go. Present and compose do not run.

## 1b. Current matrix and wait budget (rc15 base `fc1a61c5`, plain build, shader cache warm)

Night race, race window VBlanks 7,800-11,400 (means of the 600-VBlank reports; one run per scale, so ±1-3%):

| ms per VBlank | 3x | 6x | 10x | 12x |
|---|---|---|---|---|
| GS worker CPU | 7.14 | 7.38 | 6.83 | 6.33 |
| process CPU | 17.40 | 18.16 | 17.33 | 16.59 |
| wall (headless) | 7.49 | 8.61 | 15.43 | 20.99 |
| EE critical path | 7.13 | 8.61 | 16.74 | 23.21 |
| frame p50 / p95 | 6.93 / 9.86 | 8.45 / 11.80 | 16.34 / 21.19 | 22.76 / 28.48 |
| GPU busy | 2.80 | 6.15 | 14.32 | 19.92 |
| GPU draw passes | 1.56 | 4.08 | 10.23 | 14.55 |

The same runs, the **wait budget** (ms/VBlank; `[core2] gpu_wait ms/vblank by readback reason`, now with the flare split):

| Wait | 3x | 6x | 10x | 12x |
|---|---|---|---|---|
| **flare table readback** (0.75 reads/VBlank) | 0.16 | 0.90 | **6.05** | **10.36** |
| readback before a write (IN-508 remainder) | 0.06 | 0.22 | 0.69 | 1.00 |
| keep (target ageing) | 0.01 | 0.02 | 0.10 | 0.16 |
| tone-hook CLUT (attributed now) | 0.00 | 0.00 | 0.00 | 0.00 |
| other / unattributed readback | 0.00 | 0.00 | 0.00 | 0.00 |
| submission slots and fences (`not_readback`) | 0.01 | 0.09 | 1.49 | 2.75 |
| GPU wait, total | 0.23 | 1.23 | 8.32 | 14.27 |
| EE VBlank back-pressure (`vsync_wait_ms`, cadence) | 2.35 | 3.14 | 9.99 | 15.86 |
| EE drain/read waits (`drain_wait_ms`) | 0.01 | 0.01 | 0.03 | 0.04 |

**Reading:** at 10x the GPU is busy 14.3 of the 16.7 ms budget (2.4 ms of headroom) and the frame is 15.4-16.7 ms: the
GPU is close to the limit but the **flare read is 6.05 ms of the 8.32 ms of GPU wait** and it is what stops the stream
(73% of the wait). At 12x the GPU is over budget (19.9) and the flare wait is 10.36 ms. At 3x/6x the flare wait is
0.16/0.90 ms and the slot waits are negligible. On this desktop the EE itself does not wait for the read (IN-522: the
reads are "without the EE waiting"); the wait is the GS worker's and it stops the VU1/GS stream behind it.

**10x is not GPU-bound yet** (14.3 busy against 16.67, 2.4 ms of headroom); it becomes GPU-bound at about 12x. Removing
the flare wait would move the 10x frame from 16.3 to about 10.3-12 ms (the GPU's own 14.3 plus the rest), i.e. it would
buy the headroom a weak machine needs to hold 60 FPS.

### IN-540 on the rc15 base (re-run after the merge)

Two executables from the same merged tree, only `core2_depth_alias.cpp` reverted for the control:

| Night race, ms/VBlank | base | IN-540 | change |
|---|---|---|---|
| 3x, batch stage (2+2) | 1.143 | 0.879 | **-0.264 (-23%)** |
| 3x, GS worker (2+2) | 7.777 | 7.410 | -0.37 |
| 3x, wall (2+2) | 8.088 | 7.756 | -0.33 |
| 6x, batch stage (1+1) | 1.091 | 0.849 | -0.24 |
| 6x, wall (1+1) | 8.674 | 8.446 | -0.23 |

Frames: 3x VBlanks 10,800 and 11,400 identical (2/2); the aliased-depth fixture 306/306; on the main base the full
identity was night 3x 9/9, 12x 4/4, Arcade 14/14 with the same code.

## Recordings

The reference scenes are driven by recordings (IN-520) so the same input can be replayed on CoreGS 2.0 and, black box,
on the GSdx reference (`gt4-gs.exe`) for the comparison. `build/scripts-215/make_recordings.py` writes them from the
press schedules of `night-run.sh` and `arcade-run.sh` (the pad state a press produces: buttons only, every pressure byte
0xFF and every axis 0x80, which is what the donor's `--press` path sets):

- `build/bench-inputs-215/night215.txt`: the night race (GT Mode, B-Spec, Hong Kong Reverse, VW Lupo, 5 AI), the ledger's
  reference scene, VBlanks 1-11,500; race from about 7,300.
- `build/bench-inputs-215/arcade215.txt`: Arcade, Clubman Stage Route 5, the menu/showroom/pre-race/race path, to 20,000
  (the runs below use 16,000).
- The license B-1 recording from the main checkout (`private/coregs2-bench/inputs/licenca-b1.txt`, v2.00, 3,514 VBlanks).

## Historical matrix table (main 59f27aa1)

Night race, race window VBlanks 7,800-11,400 (means of the 600-VBlank reports, `cpu_summary.py`). 3x: two runs (the
pair below); 6x and 12x: one run each (single runs drift 1-3%, so the A/B sections use 2+2).

| ms per VBlank | 3x | 6x | 12x | 3x Performance | 12x Performance |
|---|---|---|---|---|---|
| GS worker CPU (`gs_cpu`) | 7.47 | 7.44 | 6.67 | 7.72 | 6.81 |
| process CPU | 17.26 | 17.50 | 16.47 | 17.49 | 17.25 |
| wall (headless) | 7.71 | 8.71 | 20.75 | 7.88 | 15.85 |
| EE critical path | 7.17 | 8.69 | 22.89 | 7.49 | 17.97 |
| frame p50 / p95 | 7.02 / 9.52 | 8.50 / 11.82 | 22.53 / 28.22 | 7.20 / 10.92 | 16.39 / 26.03 |
| GPU busy | 2.78 | 6.12 | 19.68 | 2.68 | 15.53 |
| GPU waits (`gpu_wait`) | 0.23 | 1.24 | 13.75 | 0.12 | 9.03 |
| readback stage | 0.46 | 1.40 | 11.40 | - | - |

GS stages at 3x (`exclusive ms/vblank`): gif_parse 1.85, draw 1.27, batch_own 1.12, swizzle 0.82, translate 0.59,
readback 0.46, texture 0.37, clut 0.31, upload_own 0.10, targets 0.10, glm 0.10, patch 0.09, sprite_check 0.09,
written 0.13, glm_hook 0.06, flush 0.04, total 7.47. Per VBlank: 462.9 batches, 11,806 primitives, 426 CLUT loads,
130 uploads (3,570 KiB swizzled). Batches by flush reason: clut 283.1, prim 81.2, texture 59.2, test 22.5, autoflush
11.1, transfer 3.8, vblank 1.0, alpha 1.0.

**License B-1, 1x** (the first license test, VBlanks 2,400-3,500): GS worker 8.92, wall 10.01, frame p50 10.58 / p95
12.56, GPU busy 1.04, EE critical path 10.59. It is a front-end/batch scene: gif_parse 3.93, batch_own 0.89, clut 0.60.
Flushes: **clut 1,430** (1,010 of them with the same texture), prim 211, texture 183, test 14.3, no autoflush.

**Arcade, 3x.** Race (VBlanks 9,300-15,500, Clubman Stage Route 5): GS worker 7.52, wall 7.70, frame p50 6.09 / p95
9.12, GPU 2.16, EE 6.25, gif_parse 2.45, batch_own 0.70, draw 1.07. Showroom (4,200-6,400): GS worker 1.21, wall 2.29,
GPU 0.13, and the frame is the EE (p50 6.50 ms): the showroom is not a renderer cost at 3x on this machine.

## What limits each case

- **3x and 6x:** the EE and the GS worker take turns; the GPU has headroom (2.8-6.1 of 16.67 ms). At 6x the EE critical
  path (8.69) exceeds the GS worker (7.44) because the EE waits for the GS at the flare read.
- **12x:** the GPU (19.68 ms busy, 14.32 ms of draw passes) against 16.67 ms; the EE waits 13.75 ms at the read and the
  frame is 22.5 ms (about 44 FPS). 12x is fill-bound, as IN-438/IN-523 concluded; this round did not touch it.
- **License 1x:** 8.9 ms of GS worker for 27k primitives and 1,930 batches: the CLUT-load flush (1,430 per VBlank) and
  the per-primitive front end dominate, as the static audit says.

### CPU sampler (3x, VBlanks 9,000-11,000, RelWithDebInfo-style build with line tables)

GS worker 7.367 ms/VBlank. Exclusive: amdvlk64 internal 1.113 (15.1%), CRT memcpy/memmove 0.358, `ZwWaitForMultipleObjects`
0.307, ntdll internal 0.252, `FrontEnd::ShouldDraw` 0.205, `GifParser::Transfer` 0.191, `HashUploadBytes` 0.164,
`StoreRun` 0.162, `memory::pixelSlot` 0.155, `_Construct_in_place` 0.129 (allocations), GPU submit 0.126,
`BlockOfPixel` 0.101, `AppendPrimitive` 0.100, `Kick` 0.100, `RtlFreeHeap` 0.099.

Inclusive: `FlushBatch` 3.678 (49.9% of the thread), `OnBatch` 3.617, `Kick` 2.873, `WriteRegister` 2.392,
`OnTex0Write` 2.304 (31.3%), `StripVertices` 2.280, `CommitUpload` 1.040, `Gpu::Draw` 1.110, `submit` 0.821,
`CarryDepthFillIntoColour` 0.825, `DepthBlocksInColour` 0.675, `Readback` 0.509, `Translate` 0.505,
`MemoryTexture` 0.478, `wait_slot` 0.459. EE 5.125 ms; VU1 worker 3.906 ms.

## GSdx black box (same recording, same work)

`replay-gsdx.sh night215 <run> build/perf-base/gt4vulkan.exe` with `SCALE=3` and `SCALE=12`, the same memory card and the
same 11,500 VBlanks; wall time of the whole process, measured outside the game (`timed.sh`). Only the game's own lines
were read; the GS process's output and stats were not used. GSdx's GPU busy is not visible from the game side, so the
comparison is wall time over identical work (the GS process is part of it).

| Same 11,500 VBlanks, wall | CoreGS 2.0 | GSdx (`gt4-gs.exe`, pcsx2 mode) | CoreGS 2.0 |
|---|---|---|---|
| 3x | 63.9 s | 70.2 s | 9.0% faster |
| 12x | 161.7 s | 182.4 s | 11.4% faster |

The `[gs-ipc]` lines of the GSdx runs (our client's own numbers) show the transport the other renderer pays:
`bytes=4,003,189` per frame, `consumer busy=1.238 ms/frame, wait=4.263 ms/frame`, 4,998 readbacks with a mean wait of
1.641 ms. On this machine, at 3x and 12x on this scene, CoreGS 2.0 is not slower than GSdx.

## Candidates measured (details in the ledger, IN-540 to IN-543)

| Candidate | Result | Kept |
|---|---|---|
| IN-540 depth-fill alias: the block of a pixel from the address tables, no vector per block, reserved output | `batch_own` 1.129 -> 0.883 ms (-22%), GS worker -0.18 at 3x (wall 7.80 -> 7.64, 4+4), -0.33 on the license 1x; frames identical | **yes** |
| IN-541 CLUT-load scope (skip a flush whose load cannot reach the batch's palette) | 54-72 loads (night) and 53-69 (license) per VBlank skipped, but the total batch count does not move: no gain | no (`GT4_CORE2_CLUT_SCOPE=1` only) |
| IN-542 flush-reason split (rule 2 into test/scissor/xyoffset/scanmsk/dthe) and `clut_scope_avoided` | night 3x: test 19-21, scissor 0.9-1.1, xyoffset 0.0, scanmsk 4.7-7.9; license 1x: test 8.0, xyoffset 4.7-12.0 | diagnostics |
| IN-543 Performance Mode without the self-copy from 8x | no self-copies in this scene at 12x (`self_copies=0`), GPU identical | no (reverted) |

## Method and limits

- One variable per candidate, 2+2 interleaved pairs on one executable where possible (the depth-fill candidate used two
  executables built from the same tree with only that file reverted for the control).
- Identity: `frame_diff.py` over frame dumps from both executables (night 3x 9/9, night 12x 4/4, Arcade 3x 14/14, all
  identical), the aliased-depth fixture (306 cases), and the renderer's own `depth_into_colour` counters (38,711 fills /
  1,080.2 Mpx at VBlank 10,800, equal in both).
- CTest 304/304 (1 skipped, `hw_replace_q07_pcsx2_dumps`, pre-existing), Python 795 OK / 62 skipped with
  `GT4_NATIVE_BUILD` on this build.
- Noise: single runs drift 1-3%; the machine also runs other agents' processes (Process Lasso and a chat client are
  always there). The paired stage difference (`batch_own`) is the reliable number, not any single absolute value.
- Not measured here: the laptop, rain/rally/replay/split-screen, photo mode, George V Paris (needs the Spec II build),
  and the owner's windowed F8.
