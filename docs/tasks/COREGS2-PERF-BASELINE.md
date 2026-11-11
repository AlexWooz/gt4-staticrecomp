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


### License B-1 on the rc15 base (the CPU-bound scene the streamer reports)

VBlanks 2,400-3,500, 1x, one baseline run plus two with rc15's GT4_CORE2_PALETTE_SEGMENTS=1 (TASK-168, off by default):

| ms/VBlank | rc15 base | segments on, run 1 | run 2 |
|---|---|---|---|
| GS worker | 8.25 | 8.48 | 8.21 |
| wall | 9.55 | 8.99 | 8.71 |
| EE critical path | 10.18 | 8.26 | 8.22 |
| frame p50 / p95 | 10.26 / 12.58 | 8.13 / - | 7.81 / - |
| gif_parse | 3.80 | - | - |
| category batch | 0.61 | 0.61 | 0.59 |
| GPU busy | 1.05 | - | - |

The scene is the CLUT/primitive one: **1,430 CLUT-load flushes per VBlank** (1,010 with the same texture), 211 primitive
and 170 texture flushes, GPU busy 1.05 of 16.7, 128-130 uploads and 3.5 MB swizzled per VBlank. clut_kept and
segmented_batches stay 0 and 	one_refused is 1.67 M: the palette-segment path is refused while a tone run is active,
so the two faster runs are **not** explained by segments merging batches (Hypothesis: another effect of the switch or
machine drift; the GS worker is unchanged, the EE critical path falls). Needs its own A/B before any claim.

### Scene matrix and what is missing locally

| Scene the streamer reports | Local status |
|---|---|
| License B-1 (first license oval) | measured (above); recording private/coregs2-bench/inputs/licenca-b1.txt |
| Night race (Hong Kong Reverse) | measured (matrix above); night215 recording |
| Clubman Stage Route 5 (Arcade, day) | measured (Arcade 3x race in the main section) |
| **Opera Paris** (night city) | **missing**: needs an Arcade press schedule selecting it (a new recording, 2-3 runs to validate) |
| **SS Route 5** | **missing**: same |
| **Tsukuba Wet** (wet) | **missing**: same, and the wet surface may need an unlocked car |
| **George Paris Rally** | **missing**: needs a GT Mode card with the rally event unlocked (no such card in the tree) |
| Showroom / menus | measured (Arcade 3x showroom) |

### The streamer's GTX 1660 logs (historical, another machine, black box)

rc14, Spec II, 3x (1920x1344), accurate, 15,423-file texture pack. Steady race state: GPU busy mean 6.82 ms (p99 14.78),
GPU wait mean 0.65 (p99 5.21), 7.7 submits/VBlank, **128 uploads and 2.38 MB swizzled and 1,356 CLUT loads per VBlank**.
Spikes (1,362, 1,258 race-like): wall mean 30.4, p99 89, max 127.8 ms; GPU wait mean 17.3, p99 75.4; **wall~wait
correlation 0.856**; **created>0 in 96.4%**, decodes > 20 in 89.3%, 194 keep / 303 store / 199 download; unattributed
readbacks p95 25. These are hypotheses for this tree, not local numbers: the steady state is not raster-bound on a
GTX 1660; the stutters are readback / image-creation / texture-churn spikes. The next local checks are (1) a counter for
images created while recording and a spike line beside it (the created>0 question), (2) the texture-replacement path
on/off at 3x, (3) every readback reason attributed (done on rc15: 0 unattributed).


## S4 gate: what the VU1 job actually takes from the flare table

Measured with `GT4_HW_FLARE_PATCH_TRACE` and `GT4_FLARE_TABLE_DUMP` on the night race 3x:

- The IN-522 patch record for a flare job is **16 bytes at table offset 0**, repeated at **four payload offsets** of the
  job's VIF packet (11,720 / 12,752 / 14,760 / 15,792 in the sample frames). So each job copies the **first four words**
  of the 64x8 table, four times.
- Those four words are **not** one value replicated: over 1,400 reads the tuple is
  `(ff0000fd, ff0000fd, ff0000fd, ff0000fd)` 90 times, `(ff0000fd, 11111111, 11111111, 11111111)` 67 times, and the four
  words differ in **1,129 of 1,400** reads (RGBA `R = visibility`, `A = 0xff` in the quiet case).
- Therefore a **single factor per job is not justified**: the four words are four lights' visibilities packed in one
  16-byte window, and which sprite reads which word is decided by the VU1 program (or by the unpack lane at each of the
  four payload positions). S4 needs that mapping before it can multiply each flare draw by the right word; applying word
  0 to the whole job would be wrong whenever the four differ (81% of the reads).

**Exact gate to finish S4 (1-2 d):** record the VU1 program that consumes the job (`PS2_VU_RECORD` on the night race, or
a per-entry call histogram) and read its generated code to find (a) which VU1 memory word each of the four payload copies
fills and (b) which sprite reads which of the four values; then pass that index to the draw variant. The rest is ready:
the patch points are known, the table target lookup and barrier ordering exist, and the multiply is a single scalar on
the flare's colour or alpha (P1). Until then the CPU read stays (faithful, same frame).

## `GT4_CORE2_PALETTE_SEGMENTS` on the license: no gain (correction)

Five 1x runs of license B-1, VBlanks 2,400-3,500 (the first measurement's 9.55 ms was machine drift, not the switch):

| | off (3 runs) | on (2 runs) |
|---|---|---|
| wall | 8.675, 8.705, 9.550 (the outlier) | 8.705, 8.990 |
| frame p50 | 7.93, 7.97, 10.26 | 7.81, 8.13 |
| GS worker | 8.08, 8.11, 8.25 | 8.21, 8.48 |
| `clut_kept` | 0 | 0 |
| `segmented_batches` | 0 | 0 |
| `tone_refused` | 1.67 M | 1.67 M |

The segment path is **refused while a tone run is active**, so nothing merges; the on/off difference is run-to-run drift.
Keep the switch **off** (rc15's default) and treat the license's 1,430 CLUT flushes as still open: TASK-168's segments
need to work under the tone hook to help this scene.

## `python_native_contracts`: one pre-existing rc15 test gap

`tests/python` with `GT4_NATIVE_BUILD=build/perf-base`: 804 tests, **one failure**,
`test_gs_live.ConfigTests.test_bad_values_are_refused` — it expects `GT4_GS_SCALE=1.5` to be refused, while rc15 accepts
fractional scales (1.25x/1.5x/1.75x/2.5x). The branch touches no test or launcher file (`git diff
origin/release/1.3.0-rc15..HEAD -- tests scripts docs/launcher` is empty), so this is rc15's stale test, not the merge.
Reported here for its own fix; no test was weakened.

## Scene schedules still to create

Opera Paris, SS Route 5 and Tsukuba Wet need a recording (`build/bench-inputs-215/<name>.txt` + `.setup`) built from an
Arcade press schedule on `private/slice110-menu-pilot/name-control4/card.ps2` (City Courses / wet track menu positions),
each validated in 2-3 runs. George Paris Rally needs a GT Mode card with the rally event unlocked (no such card here).


## S4 mapping: the four words are per sprite (not per vertex)

`GT4_FLARE_TABLE_WORD=<0..3> GT4_FLARE_TABLE_WORD_VALUE=0` (the other three words 0xFF) on the night race 3x, VBlank
9,600, against the all-0xFF frame:

| Forced word | Pixels that change | Box | Max |
|---|---|---|---|
| 0 | 117 | x 934..960, y 239..262 | 224 |
| 1 | 117 | x 908..936, y 240..269 | 192 |
| 2 | 0 | - | 0 |
| 3 | 0 | - | 0 |

So the 16-byte window is **four lights**, and words 0 and 1 drive **two distinct flare sprites** (the two headlights of
the car ahead); words 2 and 3 are spare in this frame. The factor is therefore **per sprite** (not per vertex), and the
draw variant needs the **light index of each sprite**. The open mapping question before coding: whether the VU1 emits
exactly one sprite per used light, in light order, within the job (then draw order = word index); the four payload
positions (11,720 / 12,752 / 14,760 / 15,792) are consistent with one REF per light. Next step: count and order the
job's flare draws (a marker + a per-job batch counter) and compare the forced-word frames with a per-draw-index
modulation; if a light emits several sprites, the index must come from the VU1 program.

## Tone-strip drop on license B-1: implemented, **no gain** (off by default)

`GT4_CORE2_TONE_STRIP_DROP=1` keeps a pending tone-run copy-back strip pending across a CLUT load instead of flushing it
(the renderer drops it whole). Measured on license B-1 1x, VBlanks 2,400-3,500, 4+4 interleaved:

| | on (4 runs) | off (4 runs) |
|---|---|---|
| CLUT flushes/VBlank | **170** | 1,430 |
| `tone_strip_kept`/VBlank | 1,260 | 0 |
| GS worker | 8.07 (8.04/8.22/8.08/7.96) | 8.19 (8.28/8.08/8.30/8.11) |
| wall | 8.91 (9.17/9.23/8.57/8.68) | 8.80 (8.97/8.71/8.83/8.68) |
| frame p50 | 8.90 (9.49/9.85/7.61/7.78) | 7.94 (8.21/7.95/7.65/7.94) |

The 1,260 flushes disappear but the time does not: the merged strip batch is still handed over once (and dropped there),
so the front-end cost moves rather than disappears, and the frame is the same within noise (slightly worse on the two
early pairs). Kept in the code behind the switch (default **off**) for TASK-171/168 to build on; no gain claimed.


## S4 mapping, second measurement: one sprite per used light

The forced-word frames (night race 3x, VBlank 9,600) give two **disjoint, single-sprite** boxes: word 0 -> x 934..960
y 239..262 (117 px), word 1 -> x 908..936 y 240..269 (117 px), words 2/3 -> nothing. Because each word changes exactly one
contiguous sprite and the two sets do not overlap, **each used light emits one sprite**; the four-word window is four
lights with two in use. That answers the "one sprite per light" half of the mapping without the VU1 program.

**What is still missing for S4 (the exact gate):** the **draw order inside the job** - which of the job's flare draws is
light 0 and which is light 1. The four REF payload positions (11,720 / 12,752 / 14,760 / 15,792) suggest one REF per light
in order, but it is unmeasured. Next step (0.5 d): mark the flare job at the sink (the patch already identifies the
16-byte window at table offset 0), carry a one-bit marker to the renderer on the `MtvuJob` path, and print the job's
batches in draw order with their draw rects; the rects can then be matched to the forced-word boxes (word 0 = the
x934..960 sprite, word 1 = x908..936). With that order known, the variant is: REF qword 0xFF, the job's draw index selects
table word 0..3, the draw samples the 64x8 target and multiplies the flare's colour or alpha (P1), with the target lookup
and barrier ordering already in place. No per-vertex design is needed.


## Cooled 10x reference check and the flare draw-order measurement

**Cooled re-run (10+ minutes idle, same commands):** GPU busy **15.78** (p50 14.56 / p99 21.48), wall mean **17.04**
(p50 15.82 / p99 23.45), flare **6.85**, unchanged from the warm fresh run (15.79 / 17.05 / 6.84) and still ~10 % above the
earlier stock (14.32 / 15.43 / 6.05). Cooling changed nothing, so this is a **persistent GPU state**, not transient heat:
either the Afterburner restore did not fully apply (core/PL) or the card sits in a different boost/driver state. Treat it
as "stock but limited/warm": the flare A/B must be **interleaved in one session** (stock/flare/stock/flare), and absolute
numbers carry that label. The earlier stock point stays historical.

**Draw-order measurement (`GT4_CORE2_FLARE_TRACE`, marker in `hw_flare_job.hpp`, jobs flagged in
`post_table_patches`):** the marked job is **not** a small per-light job. It carries **6 batches, 10 / 60 / 14 / 60 / 42 /
62 primitives** (rects spanning the screen, e.g. 0,0..639,447 and 354,113..579,414), i.e. the whole VIF1 chain for the
frame contains the table REF; the two flare sprites are 2 of ~258 primitives in it. The recorded S4 design assumed one
job per light, so **a per-job or per-draw-index factor is not well defined**.

**The exact gate (next, ~1 d):** mark at the **VU1 program invocation** level, not the job. In `walk_vif_batch` the worker
can see the MSCAL (0x14) that follows each flare REF; it must post a `FlareLight{index}` record into the output ring
before that invocation's PATH1 records, and the renderer tags the batches after it (until the next marker) with that light
index. Then the variant is as planned (REF qword 0xFF, table word per light, the draw samples the 64x8 target and
multiplies the flare's colour or alpha). No per-vertex design is needed, and the forced-word measurement already bounds the
picture gate to the two sprites (117 px each).


## Flare REF structure: one 1-qword packet per REF, no MSCAL in it (2026-11-10)

`GT4_HW_FLARE_PATCH_TRACE=1` now prints the packet a flare REF lands in and the MSCALs in it. The night race 3x gives, for
one frame's chain:

```
[mtvu] flare patch job=1008968 packet=8   qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=18  qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=36  qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=46  qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=86  qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=96  qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=114 qwc=1 at=0 mscals=0
[mtvu] flare patch job=1008968 packet=124 qwc=1 at=0 mscals=0
```

So **each flare REF is its own one-qword VIF packet**, and that packet carries **no MSCAL**; the program that consumes it
runs from a later packet. There are **eight** REF packets per frame, not four, so the four-table-word window is copied by
more than one REF per light (two REFs per used light, or separate colour/occlusion uses).

**Design that follows (per-invocation marker, still to implement):** in the worker's packet walk, remember the REF packet's
order (0..7) and, when the next MSCAL packet arrives, post a `FlareLight{refIndex}` record into the output ring before
replaying that packet; the sink sets the current light index from the record, and the renderer tags the batches that
follow until the next marker. The **REF order to table word mapping is not 1:1** (8 REFs, 4 words) and must be measured:
one run with `GT4_FLARE_TABLE_WORD=i` per word and the marker trace shows which REF index drives which word's sprite, or
vice versa.

**Status:** the structure is separable (good news), but the record + shader variant are not implemented this round; the
CPU read and the 6.85 / 11.25 ms wait stand. No 0-readback claim.


## TASK-171: license front-end split and the first candidate (2026-11-10)

**Measurement (no code change).** CPU sampler on license B-1, 1x, VBlanks 2,000-3,400 (line-table build); GS worker
8.371 ms/VBlank sampled (sampler costs ~5%). Exclusive, per VBlank: `ShouldDraw` **0.445**, `Kick` **0.360**,
`GifParser::Transfer` **0.266**, `ConsumeRegList` 0.157, `AppendPrimitive` **0.126**, `PixelRect::Empty` **0.114**,
`EmitBatch` 0.096, `StripVertices` 0.085, `MakeVertex` 0.077, `TakeSnapshot` 0.079, `std::function` destruction 0.079,
`std::vector<Vertex>::emplace_back` 0.081, `std::array<Vertex,8>::operator[]` 0.080, `BuildCandidate` 0.054,
`VertexQueue::push_back` 0.046. The front end is about 1.3 ms of the 8.37; the GIF parser 0.42; the rest is batches,
CLUT, uploads and draw recording. Per primitive (~26.5k/VBlank): `ShouldDraw` ~16.8 ns, `Kick` ~13.6 ns,
`AppendPrimitive` ~4.8 ns. It scales with **primitives**, not packets.

**Candidate 1 (kept, `GT4_CORE2_SCISSOR_VIEW=0` for A/B):** `ShouldDraw` decoded the SCISSOR register's four bit ranges
and its 12.4 cull bounds on **every primitive**. They depend only on the register, so a two-entry (per context) view caches
the decoded rectangle and cull bounds, recomputed on a register change. Exact: same ranges, same arithmetic. License B-1
1x, 2+2 interleaved: `gif_parse` 3.898 -> 4.010 ms (-2.8%), GS worker 8.138 -> 8.275 (-1.7%), wall 8.665 -> 8.755 (-1.0%);
frames identical (license 4/4 at VBlanks 2400-3600, night 3x 2/2 at 10800/11400); CTest 319/320 (the one failure is the
pre-existing rc15 `test_gs_live`).

**Next candidates, in order:** inline `PixelRect::Empty` (0.114 ms, out-of-line call in every cull), the `Kick` state
decode (~13.6 ns/prim), `AppendPrimitive`'s vertex copies (0.126 + 0.081 emplace + 0.063 memcpy), and the GIF parser's
per-qword walk (0.42 ms). Anything touching vertex-queue ownership or the batch record format is a design decision and is
not started.


## TASK-171 stage split (license B-1 1x and night 3x) and the remaining lever

`[core2] ms/vblank` (inclusive) and `exclusive`, same runs as the candidate, GS worker 7.52 / 6.60 ms:

| Stage | license 1x (exclusive) | night 3x (exclusive) | license count/VBlank |
|---|---|---|---|
| **gif_parse** | **3.28** (44 %) | **1.73** | 26.5k prims, ~1,553 CLUT loads |
| batch_own | 0.64 | 0.79 | 1,930 batches |
| draw (recording) | 0.93 | 1.32 | |
| clut | 0.48 | 0.16 | 1,553 loads |
| swizzle | 0.49 | 0.80 | 1,298 KiB / 3,597 KiB |
| translate | 0.45 | 0.53 | |
| cpu_sprites | 0.37 | 0.00 | |
| texture | 0.19 | 0.28 | |
| upload_own / written | 0.12 / 0.14 | 0.09 / 0.10 | 135 uploads |
| readback / targets / patch / glm | 0.08 / 0.04 / 0.03 / 0.03 | 0.30 / 0.11 / 0.05 / 0.05 | |
| sprite_check / flush / hook | 0.10 / 0.06 / 0.07 | 0.10 / 0.05 / 0.01 | |
| GPU busy | 1.28 | 2.26 | |

**The largest stage is the GIF front end (gif_parse 3.28 ms, 44 %)**, and it scales with primitives (26.5k), not packets.
The sampler's inclusive split on the license: `Kick` **3.62** (43 %), of which `StripVertices` **2.48**, `DecodeRegisterXyz`
**1.20**; `FlushBatch` **3.14** (`EmitBatch` 3.12, `OnBatch` 2.76, `Gpu::Draw` 0.96); `WriteRegister` **2.35**, of which
`OnTex0Write` **2.26** / `ProcessWrite` 1.82 / the CLUT-load **flush lambda 1.59**; `ConsumeRegList` **2.06**;
`ImageQwords` 0.75. Exclusive per-site: ShouldDraw 0.445, Kick 0.360, parser 0.266, ConsumeRegList 0.157,
AppendPrimitive 0.126, PixelRect::Empty 0.114, EmitBatch 0.096, StripVertices 0.085, MakeVertex 0.077.

**Why no new micro-candidate this round:** `PixelRect::Empty` is already an inline header method (the 0.114 is call
*volume*, not an out-of-line call); `EffectivePrim` is ~6 instructions; `AppendPrimitive`'s copies are memcpy-bound
(0.14 ms together); the parser walk is 0.266. None of them is above the ~0.1 ms noise of a 2+2 on this scene, so nothing
was landed as a "win".

**The remaining lever is a design decision (stop point):** the per-vertex path (`Kick` 3.62 with `StripVertices` 2.48 and
`DecodeRegisterXyz` 1.20) and the **CLUT-load batch flush** (1.59 ms inside `OnTex0Write`, 1,553 loads/VBlank). Two options,
each 2-3 days and each changing the front-end contract:
1. **Batched strip decode into the batch's vertex array**: have `StripVertices` write vertices directly into the pending
   batch (reserve + decode in a tight loop) instead of one `Kick` + `MakeVertex` + `AppendPrimitive` per vertex. Bound:
   the per-vertex overhead minus the unavoidable decode (Hyp: 0.5-1.0 ms of the license's 8.4).
2. **TASK-168 palette segments under the tone hook**: the 1,553 CLUT loads force 1,430 flushes (the tone-strip drop removed
   the flushes but not the cost, because the merged batch is still handed over once; the segment path is refused while
   `toneActive_`). Bound: up to the 1.59 ms flush cost if segments are allowed under tone (4+4 measured segments-off vs on
   showed no gain so far, so this needs the tone-refusal removed and re-measured).
Neither is a micro-hoist; both need the front-end/writeback contract reviewed.

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
