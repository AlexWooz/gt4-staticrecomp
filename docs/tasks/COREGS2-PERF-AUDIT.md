# CoreGS 2.0 performance audit (TASK-215b, 2026-11-10)

Read-only static audit of CoreGS 2.0 (`GT4_HW_CORE=2`, `native/hw_gs`). This page is the map for the parallel TASK-215
agent, which owns the builds, benchmarks and game runs. **No number below comes from a run made for this audit**: every
measured figure is quoted from the existing ledger, the CPU survey or the game-code study, with its source. New
opportunities are labelled with the evidence that exists (the cost) and the evidence that does not (the saving), and
each one names the A/B and the acceptance test that would prove it.

- Worktree: `C:\Antigravity\gt4-wt-215-coregs2-audit`, branch `docs/coregs2-perf-audit`, from `origin/main` `59f27aa1`.
- Code state: `native/hw_gs` as of that commit (IN-539 is the newest implementation note in the tree).
- Agent status: **not PCSX2-tainted**. No PCSX2, GSdx, `gt4recomp-gs`, `private/tooling/pcsx2-*`,
  `private/slice189-pcsx2-ref` or ARMSX2 source was opened. GSdx is a black box.
- Labels: **C** = confirmed (measured, or read directly in the code cited); **H** = high confidence; **Hyp** =
  hypothesis; **Unknown** = needs a measurement or a design decision.

## 0. The short answer

At **1x on the desktop and on the laptop**, the frame is limited by CPU work, and the largest single renderer item is
the batch/CLUT path of the GS worker: about 1,500-1,630 CLUT-load batch flushes per VBlank on George V Paris (2.0 ms of
the GS worker's 8.0 ms, 1.5 ms of it the flush), and 1,930 batches for 26,000-27,000 primitives on the first license
(C, TASK-134 section 9 and TASK-171). At **6x**, the Avell's GPU has headroom (about 14 ms busy) but the GS worker waits
4.5-5.5 ms per VBlank for the GPU at the game's one flare-table download, so CPU and GPU take turns (C, TASK-134
section 9). At **12x**, the GPU is the limit (about 20.6 ms busy, draw passes 14.7 ms) and the frame is fill-bound (C,
IN-438/IN-442/IN-451/IN-523).

The new items this audit recommends, in order of expected gain per unit of effort, are: (1) stop ending a batch on
SCISSOR and XYOFFSET changes, because CoreGS 2.0 records a per-primitive scissor and the vertices already carry
XYOFFSET; (2) remove the flare-table read from the faithful path by modulating the flare draws on the GPU from the 64x8
target (GS-READBACK-STUDY's S4, never scheduled); (3) skip a CLUT-load flush when the loaded palette halfwords cannot
reach the pending batch's used halfwords; (4) extend the Performance Mode effect-target cap beyond the current 1x and
width <= 256; (5) turn the self-copy off in Performance Mode at 12x, where it was measured at +1.3 ms. The already
planned TASK-168 (several palettes in one batch) and TASK-171 (per-primitive front end for the license) remain the
largest known items; they are not repeated here as new work.

## 1. Where the time goes

### 1.1 The frame in one picture

```
EE (game)        update + render lists + VIF1 chain + flare-table read position + VBlank
VU1 worker       VU1 programs, VIF1 unpack, PATH1 output into the output ring
GS worker        GIF front end -> batches -> translate -> texture/CLUT -> targets -> draw recording -> submit
                 -> at VBlank: front-end flush, target ageing, compose, present
GPU              decode/CLUT/patch compute, draw passes (storage images + pixel interlock), display compose
```

The GS worker is fed by the VU1 output ring (direct intake, IN-461/IN-462) and submits every 64 batches on a discrete
GPU (32 on an integrated one) with up to 16 submissions in flight (IN-426/IN-517, TASK-134's second round).

### 1.2 CPU stages

**3x night race, candidate 354 (PGO + ThinLTO), interleaved 3+3 (C, IN-473):**

| Item | ms/VBlank | Note |
|---|---|---|
| GS worker CPU | 5.14 | was 5.93 before PGO |
| Process CPU | 15.72 | was 16.48 |
| Wall (headless, unpaced) | 8.33 | was 8.74 |
| Frame p50 / p95 | 7.84 / 10.67 | was 8.72 / 11.21 |
| GPU busy | 3.86 | GPU is not the limit at 3x |

**GS worker stages, 3x night race (C for each stage at its candidate; the sum is H because the stages come from
different builds):** front end 1.14-1.48 ms (IN-467/IN-469), draw 1.14-1.20 (IN-446/IN-473), translate 0.57-0.65, texture
0.36 (IN-453), swizzle 0.47 (IN-452), CLUT 0.26 (IN-457), upload 0.33 (IN-456), driver about 1.2 ms (IN-445), batch
execution 4.0 ms of which 2.4 ms is forced by CLUT loads (IN-445's sampler, candidate 310).

**1x George V Paris (Spec II, rc14a plain, CPU sampler; C, `docs/GAME-CODE-OPTIMIZATION-STUDY.md` section 9):**

| Item | ms/VBlank | Note |
|---|---|---|
| Process CPU | 16.2 | the sampler window |
| GS worker | 8.0 | the largest thread |
| VU1 worker | 4.2 | 1.2 of it waiting |
| EE | 4.1 | 1.2 of it waiting |
| Graphics driver (GS) | 1.0 exclusive | 1.8 inclusive |
| GIF parsing / `ShouldDraw` | 0.26 / 0.26 | flat per-primitive path |
| Upload hash | 0.16 | IN-486 memo |
| One `GpuDraw` record per primitive | 0.11 | |
| Memory copies (memo replays, palettes, vertex inserts) | 0.3 total | all needed per the study |
| **CLUT path** | **2.0** | **1.5 ms of it is the batch flush the load forces** |

**First license B-1, Test Course, i7-11800H + RTX 3060 laptop (C, TASK-171):** 26,000-27,000 primitives and 1,930
batches per VBlank while driving; GS thread about 19-20 ms, VU1 worker about 15 ms, EE about 13 ms, GPU busy 7-10 ms.
The profile is flat over the per-primitive front end (`ShouldDraw`, `AppendPrimitive`, `Kick`, `StripVertices`, GIF
parsing), so this is a **batch-count and per-primitive-cost** problem, not one hot function.

**Showroom (C, IN-512/IN-538):** 46,000 primitives per frame; after IN-512/IN-538 the GS thread is 6.0 / 5.9 ms on
even/odd VBlanks and GPU waits 1.40 ms. The large backdrop upload is merged on the GPU; no store remains.

### 1.3 EE, readback and transport

- The EE no longer waits for the flare table: IN-522 posts the read in stream order and the VU1 packet that uses it
  waits on the VU1 worker. The owner's 374 table shows **EE at the flare read 0.00 ms** and 122/122 identical frames
  (C, ledger 374).
- The **GS worker still waits** at that read for the GPU to finish the work before it; the read is inside its stage
  list. On the Avell at 6x the measured wait is **4.5-5.5 ms per VBlank** (`download=5.46 (1.00)`) and the GPU is busy
  about 14.1 ms; with `GT4_CORE2_DELAYED_DOWNLOADS=1` the wait is 0, wall falls 15.9-17.1 -> 14.6-14.7 ms and frame p50
  17.1-17.9 -> 15.5-15.6 ms, at the cost of a one-frame-late answer the owner rejected for normal mode (C, TASK-134
  section 9; decision 0073).
- At 1x the same wait is about 1.3 ms (C, TASK-134 section 9). At 3x the ledger's 373 build had 3.80 ms of "flare and
  other reads" and 3.42 ms of GPU waits; the final 374 build moved the EE part to zero and left the GS-worker part (C
  for 373, H that the 374 split is the same minus the EE wait; the ledger does not print the final split).
- The **readback before a write** is no longer the largest wait: IN-508 took the 6x George V readback phase from 6.91
  to 2.76 ms, GPU waits 4.86 -> 2.08 ms and the EE critical path 19.97 -> 16.70 ms (C, ledger IN-508 table). What is
  left of the readback phase is "keep" and the flare download.
- Transport (C, CORE2-CPU-SURVEY, candidate 310): 2,589 GIF commands and 5.9 MB per VBlank; EE hooks 0.93 ms; the EE
  waits for the GS worker before a read 1.25 ms (drain) / 1.59 ms (whole read); the VU1 worker's output used to wake
  the EE about 650 times per VBlank, now 16 (IN-459).
- Race start and camera cut: 27-33 readbacks and about 42 ms once at the cut (C/H, STATUS 2026-11-01); IN-429/IN-430
  batched them and merged dirty rectangles (readback 16.6 -> 14.4 -> 13.3 ms, wall 46 -> 40 ms). Not a steady-state
  cost.

### 1.4 GPU

- **At 1x-3x on the desktop the GPU is not the limit:** 3.86-4.0 ms busy at 3x (C, IN-473). GPU changes since IN-419
  gave 3-13% each; halving barriers (IN-419), removing 97% of render passes (IN-420) and more barriers (IN-422) did not
  change the conclusion (C, "What we learned").
- **At 6x the GPU has headroom but the frame waits for it at the flare read:** about 8-14 ms busy against 16.67 ms
  (the desktop's 371 rows are 7.3-10.25 ms, IN-509; the Avell is 14.1 ms, TASK-134 section 9), with the 4.5-5.5 ms wait
  above (C/H, ledger 373/374 and TASK-134 section 9).
- **At 12x the GPU is the limit:** 20.6-21.6 ms busy, draw passes 14.7 ms, 563 M fragments after the compute fills
  (IN-442), frame p50 22-25 ms; the 30 FPS mode gives 12.52 ms of GPU per drawn frame (C, IN-438/IN-442/IN-451/IN-463/
  IN-525). The isolated cost map says the largest 12x groups are the full-screen constant blends and the frame/depth
  traffic of 165 MB targets (C, IN-523; IN-436 measured no gain from skipping unneeded reads and no clock limit).
- **The fixed GPU cost is the chain of about 1,000 small dependent commands per VBlank** (decodes, GLM uploads,
  patches, CLUT loads), not one big pass (H, "What we learned"); barriers by 8 KiB page (IN-464) and compute fills
  (IN-442) are the kept reductions.
- **The self-copy for a batch that samples its own target** costs +1.3 ms of GPU busy at 12x (+6%, frame p50 22.5 ->
  24.8 ms) and is 1/16 of that at 3x; without it 12x frames differ run to run (C, IN-463).

### 1.5 Frame pacing and presentation

- 6x George V, 16:9, 60 Hz virtual display: the 374 work (IN-521..IN-523) measured 373's behaviour at a mean of 53.0 ms
  from the game's frame to the display, queue control alone at 37.2 ms and the final build at 39.5 ms; the second queue
  removed the GS-thread waits (11.42 -> 0.01 ms in the showroom) at 2-6 ms more latency (C, ledger IN-514..IN-523). At
  144 Hz the latency is 13-20 ms (C).
- The frametime wave (every second VBlank over budget) was removed by IN-509: wall change from one VBlank to the next
  4.38 -> 0.44 ms, GS thread even/odd 15.22/11.17 -> 10.24/10.26 ms (C, ledger IN-509).
- Frame skipping and the 30 FPS mode are in place: 12x on George V goes from 76.85% game speed at 46.1 drawn frames/s
  to 100.02% at 38.5 (C, IN-524); the 30 FPS mode holds 100% at 30.0 drawn frames/s with 33.3 ms intervals (C, IN-525).
- The performance HUD costs nothing measurable (C, ledger TASK-135 section).

### 1.6 The situations the owner calls heavy

| Situation | Measured signature | Source | Label |
|---|---|---|---|
| George V Paris, 1x | GS worker 8.0 ms; CLUT 2.0 ms (1.5 flush); ~1,500-1,630 flushes/VBlank | TASK-134 section 9 | C |
| George V Paris, 6x, laptop | wall 15.9-17.1 ms; GPU 14.1 ms; 4.5-5.5 ms wait at the flare download | TASK-134 section 9 | C |
| Night race (CSR5/Hong Kong) | one 2 KiB flare read per frame; 275 of 480 batches flushed by CLUT loads; GPU light at 1x-3x | FRAME-COST, IN-457 | C |
| First license B-1 | 26-27k prims, 1,930 batches; GS 19-20 ms; VU1 15; EE 13; GPU 7-10 | TASK-171 | C |
| Showroom | 46k prims; GS 6.0/5.9 ms; waits 1.40; backdrop merged on the GPU | IN-512/IN-538 | C |
| Photo mode | 3-4 ms GPU waits + GS ~10 ms per game frame at 6x on the RTX 3060 | IN-537, TASK-162 | C |
| 12x | GPU 20.6 ms; draw passes 14.7; 563 M fragments; frame p50 22-25 | IN-438/IN-442/IN-451 | C |
| Race start | 27-33 readbacks, ~42 ms once; VBlanks over 16.67 16 -> 0 after IN-510 | STATUS 2026-11-01, IN-510 | C/H |
| Rain, rally, split screen, replays | **no measured renderer signature in the tree** | - | Unknown |

## 2. New opportunities

Each item: the code, the measured cost it addresses, what is expected (label), effort, risk, the exact A/B and test,
whether the image changes, and why the ledger does not already cover it. "Preserve the image" means the default
faithful mode stays identical; a Performance Mode item says so.

### 2.1 A. Stop ending a batch on SCISSOR and XYOFFSET changes (CoreGS 2.0 only)

**Code.** `native/hw_gs/frontend/hw_front_end.cpp:675-680` — `DecideFlush` rule 2 returns `TestOrScissor` when TEST,
SCISSOR, XYOFFSET, SCANMSK or DTHE changed. `native/hw_gs/frontend/hw_front_end.cpp:551-627` — `ShouldDraw` already
clips **each primitive's** coverage rectangle to the live SCISSOR and stores it in `PrimRecord::rect`
(`hw_front_end.cpp:639-657`). `native/hw_gs/writeback/hw_gs_writeback.cpp:539` — `TranslateBatch` then re-applies the
batch's **snapshot** scissor to every primitive. `native/hw_gs/core2/core2_gpu.cpp:3365-3373` — the CoreGS 2.0 draw
scissor is the per-draw rectangle, not the batch state; `core2_gpu.cpp:3313` — "the batch vertices already carry
XYOFFSET". No shader reads the scissor or XYOFFSET registers (checked in `native/hw_gs/shaders` and `core2_gpu.cpp`).

**What is wrong with the current rule for CoreGS 2.0.** TEST, SCANMSK and DTHE do change the recorded draw and must keep
flushing. SCISSOR and XYOFFSET only affect the **culling** of primitives, and the front end has already applied them
per primitive: the vertex coordinates include the offset, and `PrimRecord::rect` is the coverage clipped to the scissor.
Ending the batch is therefore not needed by the renderer; it only re-applies the old scissor in `TranslateBatch`
line 539, which is the reason the flush is currently correct.

**Design.** Add a front-end option (CoreGS 2.0 only) that (a) removes SCISSOR and XYOFFSET from rule 2 and (b) makes
`TranslateBatch` use `PrimRecord::rect` (with the existing IN-391/IN-397 slack) as the per-draw scissor instead of
intersecting with the batch snapshot scissor. `out.scissor` and `out.rect` for the other renderers stay as they are.
This relaxes the spec's chapter 02.5 rule 2 for CoreGS 2.0 only; record it in the implementation notes (the CPU
reference and the writeback keep the conservative rule).
The per-primitive rectangle is a valid scissor: a primitive cannot cover a pixel outside its own coverage box, and the
box is already clipped to the live scissor.

**Cost it addresses.** Batch count and the per-batch renderer path: 480 batches on the night race (IN-457), 1,930 on
the license (TASK-171), at least 1,500 CLUT flushes on George V 1x plus the other reasons (TASK-134 section 9). Every
flush costs a `Batch` translation, a `Gpu::Draw` record, a driver submission interval and the chance of a new descriptor
set/pipeline; the GS worker is 8.0 ms at 1x George V and 19-20 ms on the license (C). The split of the `test` reason
between TEST, SCISSOR, XYOFFSET, SCANMSK and DTHE is **not** in the existing line; it must be counted before the gain is
known (the line prints `test=` as one number, `core2_renderer.cpp:5280-5298`).

**Expected impact.** Hyp: if SCISSOR/XYOFFSET are a large share of the `test` flushes, the batch count falls by that
share; the GS worker saving is bounded by the batch cost share, not claimed here. On the license and George V it could
be several hundred microseconds to a few milliseconds; on the night race likely less. The measurement first is a
counter, not a build with the change: split `TestOrScissor` into `test`/`scissor`/`xyoffset` in the existing
`FlushReason` report (diagnostics only) and run the three heavy scenes once.

**Effort / risk.** Small (1-2 days including fixtures); risk low-medium: the shared `TranslateBatch` must be gated so
the writeback path and the CPU reference keep the old behavior, and the front-end option must be off for them. The
exactness argument depends on using `PrimRecord::rect` for every primitive, which the fixture must prove.

**A/B and acceptance.** `GT4_CORE2_PER_PRIM_SCISSOR=0/1` (or a `Core2FrontEndOptions` flag; no environment switch in
the release path is fine if the core2 option is on by default and the other renderers are unaffected). Tests:
- front-end fixture: a batch with a scissor change and an XYOFFSET change in the middle draws the same pixels as two
  batches with the old rule (compare against the CPU reference at 1x);
- frames identical on the night race 3x (10/10) and 12x (31/31), Arcade 14/14, George V 6x, the license replay;
- `[core2] batch flushes/vblank` falls in the `scissor`/`xyoffset` reasons and the other reasons do not move;
- CTest `hw_m3_*` and the core2 suites.

**Fidelity.** Exact in faithful mode if the test passes; no image change.

**Why the ledger does not cover it.** IN-455 (vertex queue ring), IN-458 (skip redundant binds), IN-467 (strip kicks)
and IN-457 (CLUT table) reduce per-batch/per-primitive work but none removes a flush. The CPU survey's R7 (front end)
and R9 (CLUT flush) are different fronts; TASK-171's plan is per-primitive draw checks, the strip path and GIF parsing,
then TASK-168; TASK-168 is palettes. No ledger row treats a SCISSOR or XYOFFSET **change** as a reason to end a batch;
IN-458 only skips a redundant `vkCmdSetScissor` bind inside a draw pass (a different thing). A case-insensitive search of
the ledger, CPU survey and game-code study finds no other mention.

### 2.2 B. Skip a CLUT-load flush when the loaded palette halfwords cannot reach the batch

**Code.** `native/hw_gs/frontend/hw_front_end.cpp:372-390` — when a TEX0 write loads a CLUT, the pending batch is
flushed if it is textured and indexed (`Tme` + `IsIndexedPsm`), or if the batch writes the CLUT source
(`BatchTouches(write.cbp, 4, false)`). `native/hw_gs/clut/hw_clut.cpp:186-252` — `ClutUnit::Decide/ProcessWrite` decides
whether the write loads and whether the load is redundant. `native/hw_gs/core2/core2_renderer.cpp:894-930` — the
existing **exact** `ClutHalfwordsWritten` (at most two ranges) and `ClutHalfwordsUsed` (the 16/256 halfwords a texture's
palette reads), already used for the GPU CLUT hazard key (`GpuPaletteKey`, `core2_renderer.cpp:929-939`).
`core2_renderer.cpp:726-772` — the palette row is looked up by palette content / GPU key.

**What is wrong.** The flush is unconditional for every indexed pending batch. The CLUT buffer is one 512-halfword
buffer indexed by CSA/CPSM; a load writes at most two halfword ranges. If the pending batch's texture uses a disjoint
range, the load changes no halfword that batch can read. The palette row the batch will get at translation is then
identical in its used halfwords, and the draw is exact. The 1,013-1,020 "same texture" flushes on George V are real
palette changes and would still flush; the remaining ~500-600 different-texture flushes may or may not share the range.
The TASK-134 probe that compared the **whole loaded palette** with the buffer found only 45-105 identical loads (5%) and
was dropped; this test is different: it never compares bytes, only whether the loaded halfwords intersect the used
halfwords.

**Design.** In the `flushBeforeLoad` callback, when the batch is indexed, compute
`ClutHalfwordsUsed(tex0::Psm(snapCtx.tex0), tex0::Cpsm(snapCtx.tex0), tex0::Csa(snapCtx.tex0))` and intersect it with
`ClutHalfwordsWritten(write)`. Flush only on an intersection (or when `BatchTouches` says the batch writes the source).
A 512-bit mask or a two-range check makes it cheap. Add a counter of "flush avoided" and "flush needed" first.

**Cost it addresses.** On George V 1x, 1,500-1,630 CLUT flushes per VBlank and 2.0 ms of CLUT path, 1.5 ms of it the
flush (C, TASK-134 section 9). The night race has 275 CLUT flushes of 480 batches (C, IN-457). The saving is the share
of loads that miss the pending batch's used halfwords, which is Unknown until the counter runs; Hyp 20-40% of the
different-texture flushes, i.e. a few hundred microseconds at 1x.

**Effort / risk.** Small (about 1 day plus the counter); risk low. Exactness rests on the range helper, which is already
proved against `LocatePixel` for all load shapes (IN-457 fixture) and against the GPU palette path (IN-408); the new
test must cover a load that misses, a load that overlaps one range, a load that writes only unused entries of the same
texture, a wrapping CT16 range, CSM2, and an 8-bit vs 4-bit texture. Keep B on the palette-rows path (the default,
IN-409): with `GT4_CORE2_PALETTE_ROWS=0` the texture decode key includes the unused palette entries, so the saved flush
could cost a decode instead. With palette rows the pending batch's `t.palette` may change in **unused** entries, so the
palette-row key can change and write a new 256-pixel row; that is a small extra, not a correctness problem. (Hashing
only the used entries would remove it; a follow-up.)

**A/B and acceptance.** `GT4_CORE2_CLUT_SCOPE=0/1`; counter `clut_flushes_avoided` next to
`clut_same_texture`. Frames identical on the night race 3x/12x, George V 6x, the license and the showroom; CLUT fixtures
(`hw_m3_hw_clut_copy_free`, the `hw_clut` suite, the new overlap cases); the `[core2] batch flushes/vblank` `clut=` line
falls and no other reason rises.

**Fidelity.** Exact in faithful mode; no image change.

**Why the ledger does not cover it.** IN-457/458 count `clut_same_texture` (55 of 275 on the night race) but do not use
it to skip; IN-485 is the palette data memo (identical bytes); TASK-134's third round dropped the **whole-palette**
identity probe (5%); TASK-168 is the large fix (several palettes in one batch). This is the cheap partial step that does
not conflict with TASK-168: if TASK-168 lands first, B is subsumed for the palette changes.

### 2.3 C. Remove the per-frame flare-table read from the faithful path: modulate the flare draws on the GPU

**Code and measured cost.** The read is posted by `native/hw_gs/hw_gs_stream_sink.cpp:574-589` (`post_flare_table`),
executed at `hw_gs_stream_sink.cpp:682-688` (`FlareRead`), and waits for the GPU in `Renderer::ReadFifo`
(`native/hw_gs/core2/core2_renderer.cpp:5162-5204`) through `Readback` (`core2_renderer.cpp:2643-2731`). The table is
64x8 PSMCT32, 2 KiB, once per race frame (C, GS-READBACK-STUDY section 1.1). The GS worker still pays the GPU wait:
4.5-5.5 ms per VBlank at 6x on the Avell, 1.3 ms at 1x (C, TASK-134 section 9; ledger 373's 3.80 ms of "flare and other
reads"). The one-frame-late answer removes it (wall 15.9-17.1 -> 14.6-14.7 ms) but the owner rejected the lateness in
normal mode (C, TASK-134 section 9; decision 0073). Performance Mode hides the flares instead (IN-441).

**Design (GS-READBACK-STUDY R3/S4, never scheduled in the ledger).** Nothing on the EE reads the table: only the VU1
flare program does, through a VIF REF, and the VU1 uses it to scale the flare sprites (C/H, GS-READBACK-STUDY section 2;
P0/P1 prove the reader set and the linear use). Then:
1. In the VIF1 shadow walk, recognise a REF into the registered 64x8 table and mark the VU1 job that uses it with
   `{table GS address, table texel x, y, lane}` (the existing `MtvuJob` record can carry a side marker, or a new record
   pair next to `post_mtvu_job`, `hw_gs_stream_sink.cpp:563-569`).
2. Replace the REF'd qword with 0xFF per pixel (the constant GT4 itself uses in split screen, `D_006AF5D0`), so the VU1
   emits the full-intensity flare.
3. In `Gpu::Draw` (`core2_gpu.cpp:2990`), for the marked job's draws, bind the 64x8 table image as an extra draw
   descriptor and pass the texel in a push word; the vertex shader samples it and multiplies the vertex colour/alpha
   (a new specialised variant, `core2_gpu.cpp:1017-1060`, `hw_pixel_uber.frag`). The table target is found by the normal
   target lookup (`TextureTarget`, `core2_renderer.cpp:1913`); the draw already reads it earlier in the frame, and the
   barrier tracking must order that read (a `Use` on the table image at the draw).
4. No GPU readback (the GS FIFO still needs the transfer consumed: a skipped read as in IN-441/IN-477 keeps its state
   in step and the EE bytes are never read), no GS-worker wait, no VU1 wait for the table, same frame, exact light per
   slot.

**Precondition (must run first).** P1 of GS-READBACK-STUDY: run the VU1 flare entry through the AOT/interpreter
differential harness with visibility 0..255 and prove the output changes only in RGBA/alpha, linearly. If the value also
changes size or culls, the GPU must generate vertices (indirect draw) and this design is not worth it. This is an
offline check, not a game run.

**Expected impact.** H: removes the measured 4.5-5.5 ms GS-worker wait at 6x on the Avell (the same wait the delayed
answer removes, without the lateness) and about 1.3 ms at 1x; night tracks and any race with the sun in view. The extra
shader work is one texture fetch and one multiply on the flare sprites, negligible against the wait (Hyp; measure GPU
busy). At 12x the GPU remains the limit; the wait removal helps the frame pacing, not the 20.6 ms of fill.

**Effort / risk.** 3-5 days (GS-READBACK-STUDY S4). Risk medium: the marker must cover exactly the flare primitives and
not the rest of the job; the table must be read after the sample draws that wrote it (the existing barrier/`touch`
tracking); the colour may differ by ±1 LSB from the VU1's float-and-FTOI path. A switch and the owner's visual test
decide whether ±1 is acceptable; if not, keep the exact path and use this in Performance Mode only.

**A/B and acceptance.** `GT4_CORE2_GPU_FLARE=0/1`. Night race: **0** readbacks of the 2 KiB table, `[hw] flare table`
reads=0, the GS worker's GPU wait at the read removed; flare pixels within ±1 LSB of the current exact path and every
other pixel identical (3x 10/10, 6x, 12x); the VU1/MTVU verify and the race clock/state unchanged; F8 on the Avell at
6x shows the wall/frame improvement with exact flares.

**Fidelity.** Faithful in the same frame; only flare brightness may differ by ±1. This is the only design that satisfies
the owner's "normal mode keeps the exact answer" decision without the GPU wait.

**Why the ledger does not cover it.** IN-439 (delayed downloads), IN-441 (hidden flares) and IN-522 (EE does not wait)
are workarounds that either delay or remove the effect; the ledger has no GPU-side modulation. GS-READBACK-STUDY S4
recommends it for CoreGS 2.0 "after M8/M9" and was never turned into a task; TASK-134's "left" list mentions a deferred
flare read on the GS thread and the owner rejected the lateness. This is the missing faithful-mode item.

### 2.4 D. Extend the Performance Mode effect-target cap beyond 1x and width <= 256

**Code.** `native/hw_gs/core2/core2_renderer.cpp:571-574` (`effectTargets1x_`, on in Performance Mode) and
`core2_renderer.cpp:1842-1844`: a target is created at 1x only when `width <= 256`. `native/hw_gs/core2/core2_gpu.cpp:2829-2842` — `CreateImage(nativeWidth, nativeHeight, scaled)`; `Image::scale` is already per image
(`core2_gpu.cpp:73-86`) and `pushTexScale` already carries a texture's own scale (`core2_gpu.cpp:3088`,
`hw_pixel_uber.frag`). `core2_renderer.cpp:4863-4944` — the display compose and present scale use the global scale.

**What it addresses.** GPU fill at 6x/12x on targets that are sampled but never displayed: any sampled-only target over
256 (a 512-wide glow or effect buffer, if the profile finds one) that the current `<= 256` rule leaves at the full
scale. IN-497 measured -8% (27.5 -> 25.3 ms) on the HD 620 at 2x for the small ones and "the picture changes a little"
(C for that machine); at 6x on the RTX 3060 and 12x on the RX 9060 XT the gain of the wider targets is Unknown. The 12x
GPU is 20.6 ms with 14.7 ms of draw passes (C).

**Design (measurement first).** Use the existing `GT4_CORE2_GPU_PROFILE_AT=<vblank>` profile (IN-425) at 6x and 12x to
list the targets by size, draw area and whether they are sampled/displayed/read back. Then create a sampled-only target
whose width is over 256 at a **cap scale** (for example 2 or 4) in Performance Mode, keeping the global scale for the
display frame, the depth target, the flare table, CLUT sources and anything read back or materialized. The infrastructure
already supports a per-image scale for sampling; the readback, materialize, patch and self-copy paths must use
`target.image.scale` (they mostly use the image's native coordinates; audit each). A target that later needs a readback
can be recreated at the global scale or fall back on the first read.

**Expected impact.** Hyp; bounded by the fill of the affected targets. At 6x, the GPU has headroom (14.1 ms busy) and
the frame is held by the flare wait, so D is mainly a 12x/weak-GPU item and a way to give the 6x GPU more margin for
heavier tracks. At 12x a 512-wide target drawn at 1x instead of 12x removes 143/144 of its fragments (Hyp for GT4's
actual sizes; the profile decides).

**Effort / risk.** Medium (2-3 days); risk medium: the picture changes in Performance Mode (blurred effects), and a
target that is read back/displayed/materialized must not be capped. The owner's rule (a cut goes to Performance Mode)
applies.

**A/B and acceptance.** `GT4_CORE2_EFFECT_SCALE=<cap>` (0/off = today) in Performance Mode. Measure GPU busy and frame
p50 at 6x and 12x on George V and the night race; picture checks on night, rain, rally, photo and split screen (the
chapter 14 list); faithful mode unchanged; identity gates on the non-capped targets.

**Why the ledger does not cover it.** IN-497 is the 1x/<=256 rule and its HD 620 measurement; IN-432/IN-478 are the
present resampling; the roadmap lists per-target resolution as an enhancement but no ledger row tries a cap scale for
targets over 256.

### 2.5 E. Fuse the display compose with the present downsample

**Code.** `native/hw_gs/core2/core2_renderer.cpp:4863-4944` — `ComposeGpu` computes the display at `outWidth =
size.width * scale_`, then `PresentScale` (`core2_gpu.cpp:4403-4480`) resamples to the window with Lanczos/FSR/RCAS.
At 6x the compose output is 3840x2688 (10.3 Mpx) and the window is 3440x1440 (5.0 Mpx); at 12x the compose output is
41 Mpx.

**What it addresses.** One full internal-resolution write + read per frame that the present downsample immediately
consumes. The fill benchmark's full-screen pass at 7680x5376 is 0.4-0.5 ms for a simple shader (C, IN-436/IN-438); the
compose and present passes together are at least that at 12x. The ledger does not print a separate compose GPU time
(the `compose` CPU stage is not GPU timestamps), so the exact cost is Unknown.

**Design.** A Performance Mode (or exact, if the arithmetic matches) single compute pass that reads the display
target(s) at the output pixel positions and writes the window-sized image, applying the same Lanczos/FSR/RCAS kernel;
skip the intermediate `outWidth x outHeight` image. For exactness the fused pass must reproduce the same two-step
sampling; if the kernel order changes the output, it is a Performance Mode item. The `CircuitSource`/display layout
already gives the source rectangle and scale; `hw_display_compose_scaled.comp` and `core2_present_scale.comp` are the
two shaders to merge.

**Expected impact.** Hyp: about 0.5-1 ms at 12x, less at 6x; measured with GPU busy, not claimed. It does not address
the 1x CPU or the flare wait.

**Effort / risk.** Medium (2-3 days); risk medium (shader merge, color rounding, interlaced/deinterlace path, two
circuits, aspect/crop).

**A/B and acceptance.** `GT4_CORE2_FUSED_COMPOSE=0/1`; GPU busy and frame p50 at 6x/12x on George V and the night race;
output diff (exact if the kernel matches, otherwise Performance Mode and the owner's picture check); the overlay/aspect
and deinterlace checks unchanged.

**Why the ledger does not cover it.** IN-432 added present resampling, IN-478 changed the integrated-GPU path to a plain
blit; no row measures or fuses the internal-resolution compose with the downsample.

### 2.6 F. Self-copy off in Performance Mode at 12x

**Code.** `native/hw_gs/core2/core2_renderer.cpp:4245-4264` decides the copy for a batch that samples its own target;
`core2_gpu.cpp:3052-3075` copies the sampled rectangle into a kept image before the batch; IN-463 explains why (the
glow buffers, the in-place luminance reduction and a full-screen 1:1 pass sample pixels other fragments of the same
batch write; at 12x the 1:1 pass reads a neighbouring sub-pixel).

**Measured cost.** +1.3 ms of GPU busy at 12x (+6%), frame p50 22.5 -> 24.8 ms, 51 Mpx copied per VBlank; without the
copy 17-21 of 31 12x frames differed (up to 15,500 pixels, max 208), every renderer counter equal; at 3x frames were
identical (C, IN-463). The A/B already exists: `GT4_CORE2_SELF_COPY=0`.

**Design.** In Performance Mode at high scale, set `selfCopy_` off (or only for the full-screen 1:1 pass) and accept
the run-to-run pixel differences; keep the exact path in faithful mode. A middle option: copy only when the sampled
footprint meets the drawn rectangle (the code already computes that intersection); at 12x the full-screen pass always
meets, but the glow buffers may not.

**Expected impact.** C at 12x: up to 1.3 ms of GPU busy; about 0.3 ms at 6x (Hyp, proportional to area). It does not
help 1x/6x normal mode.

**Effort / risk.** Small (1 day); risk medium: the picture changes in the glow buffers and the 12x frames are not
run-to-run deterministic without the copy; Performance Mode only, owner's visual test.

**A/B and acceptance.** Performance Mode + `GT4_CORE2_SELF_COPY=0`; GPU busy and frame p50 at 12x; a night/rain/photo
picture check; faithful mode (copy on) unchanged.

**Why the ledger does not cover it.** IN-463 kept the copy for exactness and determinism; it did not offer a
Performance Mode variant, and the A/B is already in the code.

### 2.7 G. Target-table index (measurement first)

**Code.** `core2_renderer.cpp:1883-1927` — `DrawTarget`/`TextureTarget` scan `targets_` linearly; `core2_renderer.cpp:1521` — `MemoryTexture` scans all live drawn targets for every memory texture; `OverTargetsSignature` (1686) and
`ClutTargetsOnGpu` (858) also scan. `target_slots` and `live_targets` are printed every 600 VBlanks
(`core2_renderer.cpp:4972-5001`), but no measured value is in the docs.

**What it addresses.** If a long session, photo mode or the showroom grows the table to hundreds of slots (dead slots
are reused, but the peak stays), every batch pays several scans. With 1,930 batches on the license and 100+ slots the
cost could be a few tenths of a millisecond (Unknown; no measurement). This is a **measurement-first** item: read
`target_slots` on the heavy scenes and a long session; if it is small (tens), do nothing.

**Design if large.** An open-addressing index from `basePointer` to live target indices, maintained by
`CreateTarget`/`DestroyTarget`/`Grow`, and used by `DrawTarget`/`TextureTarget`; the over-target scans keep a small
live list. Exact; no image change.

**Effort / risk.** 1-2 days if the measurement justifies it; risk low. Not in the short list until measured. Test: a
counter or sampler of the `DrawTarget`/`TextureTarget` time before and after; frames identical; no A/B behavior switch
is needed because the index changes no output. Fidelity: exact; no image change.

### 2.8 Known items that are not new (do not re-derive)

- **TASK-168, several palettes in one batch**: the largest GS-worker lever on George V 1x (2.0 ms CLUT path, 1.5 ms
  flush; 1,013-1,020 same-texture flushes; C, TASK-134 section 9). Planned, large (vertex format, shaders, CLUT upload,
  texture cache). Items A and B are cheaper partial steps and do not replace it.
- **TASK-171, per-primitive front end for the license**: 26-27k prims, 1,930 batches, GS 19-20 ms on the i7 (C). Planned
  for rc15; item A is a new candidate inside its scope.
- **A CoreGS 2.0 submission thread**: about 0.3 ms off the GS thread; presentation submits on the same queue, so every
  submission must go through one ordered queue (C, TASK-134 section 9 left list). Planned, not new.
- **Photo mode (TASK-162)**: 3-4 ms of GPU waits + GS ~10 ms per game frame at 6x; the ways are the GLM holding pages
  as the newest copy, or a one-frame-late depth download (owner's word) (C, IN-537/TASK-162). Planned.
- **The GS worker's per-record backend mutex** (`hw_gs_stream_sink.cpp:735-740`): a lock per VU1 output record; no
  measured cost; Hyp <0.1 ms. Leave it unless the sampler shows it.
- **G2 alpha/blend ranges, G7 exact additive blends, P2 blend through attachments**: small in the GPU plan and not
  measured on the owner's current machines; not worth a TASK-215 slot.

## 3. Track and situation hypotheses

| Track / situation | What differs (measured or read) | Items that target it | What is still Unknown |
|---|---|---|---|
| George V Paris, day, 1x (Spec II TT) | 1,500-1,630 CLUT-load flushes; GS 8.0 ms; CLUT 2.0 (1.5 flush) | B (cheap partial), TASK-168 (full), A (other flushes) | the `test`/`autoflush`/`capacity` split; the unaffected-load share |
| George V Paris, 6x, Avell | wall 15.9-17.1; GPU 14.1; 4.5-5.5 ms flare wait | C (removes the wait), D (GPU margin) | the final GPU wait after IN-522; the effect-target fill |
| Night race (CSR5/Hong Kong) | one flare read; 275/480 CLUT flushes; GPU light at 1x-3x | C (night), A/B (CPU) | rain/night variants; split-screen flare reads |
| First license B-1 | 26-27k prims, 1,930 batches; GS 19-20; VU1 15; EE 13; GPU 7-10 | A, TASK-171, TASK-168 | the flush-reason split on this recording; how many batches are capacity/autoflush |
| Showroom | 46k prims; GS 6.0/5.9; waits 1.40; backdrop merged | none new; watch target_slots/upload replay | heavy cars not reproduced; 2x HD 620 30 FPS is a display/rate issue |
| Photo mode | CLUT + 8x4 depth readbacks; 3-4 ms GPU waits + GS ~10 ms/game frame at 6x | TASK-162 (planned) | whether a one-frame-late depth answer is acceptable; the GLM-pages design |
| 12x | GPU 20.6; draw passes 14.7; 563 M fragments | D, F, E | which effect targets dominate; whether the self-copy differences are visible |
| Rain / rally / replay / split screen | no renderer signature in the tree | A/B/C apply if the scene is CPU/batch/flare bound; measure first | everything: prims, batches, fill, readbacks, FLARE count |
| 3D menus | 480i; not in the heavy reports | none | whether the 480i/deinterlace path costs anything |
| Race start / return to menus | 27-33 readbacks, 42 ms once; IN-510 fixed the burst | none new | whether the camera cut still shows on the owner's current build |

## 4. Already exhausted / do not repeat

These are measured or decided. Do not spend TASK-215 time re-running them without a **new design** named in section 2.

| Ledger / task | What was tried | Result | Why it stays out |
|---|---|---|---|
| IN-412 | Repeated identical uploads dropped; block versions | 5-19 uploads dropped, decodes unchanged | reverted |
| IN-413 | Content-keyed texture cache (hash every upload) | decodes 200 -> 15, emulator 17-20 -> 26-30 ms | reverted; a decode is about 10 us |
| IN-421 | D1 first measurements, shader experiments | fragment stage 0.4-0.7 ms; D1 not the next lever on the desktop | diagnostics |
| IN-436 | Skip unneeded frame/depth reads at 12x | 21.3 -> 21.1 ms; not bandwidth, not clocks | reverted / diagnostics |
| IN-438/IN-523 | Nearest-texture relaxation and the ROP/depth attachment (the exact read skipping stayed) | 12x 21.3 -> 21.1; nearest 19.6 throw-away; ROP 19.73 -> 21.37 ms; only 18% of area could reject early | ROP/depth attachment not built; the exact reads stayed |
| IN-443 | Large textured sprites as compute | 20.7 -> 20.8 ms; bound by exact bilinear | reverted |
| IN-444 | Hardware bilinear in Performance Mode | 12x 20.4 -> 19.9; 3x/6x flat; car paint differs | reverted: gain only at 12x |
| IN-449 | Bilinear footprints with textureGather | 12x 20.75 -> 21.29 ms (+2.6%) | reverted |
| IN-448 | MTVU journal slot rings | no measurable gain on the quiet desktop | reverted |
| IN-454 | Whole clears via `vkCmdClearColorImage` | 12x 20.31 -> 20.25 ms; Z24 read-modify-write remains | reverted |
| IN-460 | GLM patch kernel with shared words | 12x unchanged | reverted |
| R1 (ledger) | Submission interval 64/128 instead of 32 | GS -0.4 ms but the flare read waits longer; wall +1/+4% | 32/64 kept; TASK-134 later chose 64 on discrete, 32 integrated |
| IN-488 | Fixing threads to cores | no gain | no |
| IN-505 | Performance Mode without the colour filter | -4.4% by day, changes the race colours | measured, owner left out |
| IN-439/IN-514 | One-download-late flare answer in normal mode | removes the 6x wait but the flare is late | owner: no; Performance Mode only |
| IN-537/IN-539 | Z16 into the GLM; whole-block GPU stores | 57-58 FPS against 59.6-60.1 at 6x | measured, off by default |
| TASK-134 | CLUT same-palette probe | only 45-105 of 1,494-1,631 loads identical (5%) | dropped (B is a different test) |
| TASK-134 | VU1 `vfix` census | 70,534 at program entry/after a load, 20,124 Q, 2,298 I; the rest needed | dropped |
| TASK-134 | SPU2 envelope constants | ~0.03 ms of the 0.17 ms mixer | dropped |
| TASK-134 | Submission batches 32-256 on the Avell | 64 best; larger saves GS CPU but loses frame time | kept at 64 |
| TASK-134 | AVX2 build | wall 9.62 -> 9.66 (no gain) | not adopted |
| GSDX-PLAN | G5 real depth, G6 attachment-only targets, P3 palette expansion | G5 -3.6% real (dropped under 15%); G0 no difference; P3 -1% in game | dropped |
| ledger "dropped" | Sampled texels, real depth probe, unordered interlock, depth store skip, ROP on a discrete GPU, tone palette in push constants, input attachments | -1%, -3.6%, none, none, +13%, none, none | dropped |
| ledger | Translate without the three-vertex copy | 0.645 -> 0.635 ms; compiler already avoided it | dropped; per-vertex edits exhausted |
| IN-463 | Removing the self-copy in normal mode | 12x frames differ run to run and from the reference | normal mode keeps it; F is the Performance Mode variant |
| any | A renderer rewrite, JIT, PCSX2/GSdx/ARMSX2 code | not MIT/clean room; out of scope | never |

Also: **do not re-measure the same stage averages without a question.** The ledger's "What we learned" is explicit:
the fragment shader is cheap on the desktop, the GPU gives less than expected, stutters are spikes (per-VBlank, not
600-VBlank averages), and the frame is the EE/GS-worker critical path; look at the VBlank-by-VBlank line
(`GT4_CORE2_SPIKE_MS`) and the change from one VBlank to the next.

**The 12x ceiling is not a TASK-215 target.** The 12x draw passes are fill-bound by the 165 MB frame/depth targets (C,
IN-438/IN-523), and the GPU plan's conclusion is that big GPU gains there need CoreGS 3.0. The only 12x items in this
audit are D and F, both Performance Mode and both capped by the measured fill; do not start a 12x shader rewrite here.

## 5. Prioritized short list for TASK-215

**Prerequisite measurement (no code change).** Run the existing instruments on the three heavy scenes and read:
`[core2] batch flushes/vblank` (the `prim/test/dimx/alpha/fog/frame/zbuf/texture/clut/transfer/vblank/autoflush/
capacity/reset/sync` split), `[core2] targets ... live_targets/target_slots`, `[core2] gpu_wait ms/vblank by readback
reason`, `[hw] transport` and `[core2] exclusive`. One run each of the license recording, George V 1x and the night
race 3x. This decides whether A or B is the first code change and whether G is worth anything. It does not need a new
build.

| # | Item | Effort | Expected (label) | First acceptance test |
|---|---|---|---|---|
| 1 | **A. SCISSOR/XYOFFSET do not end a batch** (per-primitive rect as the scissor) | 1-2 d | Hyp: the largest cheap batch reduction; several hundred us to a few ms on the license/George V if those reasons dominate | front-end fixture: a mid-batch scissor/XYOFFSET change draws the same as two batches; frames identical; `test`-reason flushes fall |
| 2 | **C. GPU flare modulation** (R3/S4; P1 first) | 3-5 d | H: removes the measured 4.5-5.5 ms 6x wait and ~1.3 ms at 1x; ±1 LSB on flare pixels | night race: 0 table readbacks, flare pixels within ±1, everything else identical; Avell 6x F8 wall/frame |
| 3 | **B. CLUT-flush scope test** (used halfwords vs written halfwords) | ~1 d + counter | Hyp: a partial cut of the 1,500-1,630 flushes; bounded by the unaffected share | frames identical; `clut=` flushes fall; overlap/miss/CT16-wrap cases in the CLUT fixture |
| 4 | **D. Effect-target scale cap** in Performance Mode (>256, sampled-only) | 2-3 d | Unknown/Hyp: GPU fill at 6x/12x; bounded by the profile | GPU busy/frame at 6x/12x; night/rain/rally/photo picture checks; faithful unchanged |
| 5 | **F. Self-copy off in Performance Mode at 12x** | 1 d | C 1.3 ms GPU at 12x; ~0.3 ms at 6x Hyp | 12x GPU busy/frame; Performance Mode picture check; faithful (copy on) unchanged |

Not in the short list, deliberately: **TASK-168** and **TASK-171** (already planned and larger; A/B feed them),
**TASK-162** (photo mode), the **submission thread** (known 0.3 ms), **E** (compose fusion; unmeasured and lower rank
than D/F), **G** (target index; measurement first), and the micro items in 2.8.

**Ordering logic.** A is first because it is exact, small and can be read on the same run as the prerequisite; C is
second because its cost is measured and it is the only faithful-mode way to remove the 6x wait the owner can feel, but
it needs the P1 check and 3-5 days; B is the cheap partial of TASK-168 and can land while TASK-168 waits; D and F are
Performance Mode GPU items for 12x and weak GPUs, below the 1x/6x work.

## 6. Open questions (measurement or decision, not more code reading)

1. **Flush-reason split** on the license, George V 1x and the night race: how much of `test` is SCISSOR vs XYOFFSET vs
   TEST/SCANMSK/DTHE, and how much is `autoflush`/`capacity`? The existing line prints the reasons; no stored log in the
   tree has them. This sizes A before it is built.
2. **Unaffected CLUT-load share**: how many of the 1,500-1,630 George V flushes write no halfword the pending batch
   uses? A counter in the `flushBeforeLoad` callback answers it; B's expected gain is that share.
3. **P1 of GS-READBACK-STUDY**: does the VU1 flare entry use the table value only as a linear colour/alpha factor? Run
   the AOT/interpreter differential harness with visibility 0..255 and diff the GIF output. If it changes size or culls,
   C is not viable.
4. **The final GPU-wait split at 6x after IN-522**: the ledger's 374 table shows EE at the read 0.00 but does not print
   the GS-worker GPU waits; the Avell's 4.5-5.5 ms is from the TASK-134 build. Confirm on the current build with
   `[core2] gpu_wait ms/vblank by readback reason`.
5. **Which targets over 256 are sampled-only** and what they cost at 6x/12x: a profile run (`GT4_CORE2_GPU_PROFILE_AT`)
   with the target sizes and draw areas, before D's cap is chosen.
6. **`target_slots`** on a long session, photo mode and the showroom: is G worth anything, or is the table small?
7. **Rain, rally, replay and split screen**: no renderer signature (prims, batches, fill, readbacks, flare count) is in
   the tree. Measure one of each before assuming A/B/C apply.
8. **What the owner means by heavy**: 1x on the laptop, 6x on the laptop, or 12x on the desktop? The priorities differ
   (A/B/TASK-168 for 1x, C for 6x, D/F for 12x). This is a decision, not a measurement.
9. **Is ±1 LSB on flare pixels acceptable in normal mode** for C, or must the exact VU1 path stay and C be Performance
   Mode only? The owner's picture test decides.
10. **Does the self-copy difference at 12x** (up to 15,500 pixels of the 12x frame, run-to-run) matter visually in
    Performance Mode? F needs the owner's eye, not a counter.
11. **Can E be exact** (same two-step kernel order) or only Performance Mode? If only Performance Mode, its value is
    lower than D/F.
12. **The cost of B's range test** per flush (a 512-entry used list vs a bitmask) at 1,500 flushes/VBlank: measure the
    callback time before making it the default.

## 7. Evidence, limits and what was not done

- Read: `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`, `docs/COREGS2-ROADMAP.md`, `docs/FRAME-COST-STUDY.md`,
  `docs/GS-READBACK-STUDY.md`, `docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`, `docs/GSDX-PLAN.md` C.7,
  `docs/GAME-CODE-OPTIMIZATION-STUDY.md` sections 8-9, the current sections of `docs/STATUS.md`, the TASK-134/162/168/171
  sections of `TASKS.md`, `docs/hw-renderer-spec/CORE2-GPU-PLAN.md`, `scripts/coregs2_bench/README.md`.
- Read in the code: `native/hw_gs/core2/core2_renderer.cpp`, `core2_gpu.cpp`, `frontend/hw_front_end.cpp`,
  `frontend/hw_batch.hpp`, `writeback/hw_gs_writeback.cpp`, `writeback/hw_gs_writeback.hpp`, `clut/hw_clut.cpp`,
  `hw_gs_stream_sink.cpp`, `hw_gs_backend.cpp`, `hw_gs_options.cpp`, `hw_flare_table.hpp`, the shader list,
  `native/frame_skip.cpp`, `native/window.cpp` (the VBlank path).
- Not done, by the brief: no build, no benchmark, no game run, no code/data/pin/test change; no PCSX2/GSdx/fork/ARMSX2
  source; no owner data; no Telegram/PR/release/tag/draft.
- Limits: the stage averages come from different candidates and scenes; the exact split of the `test` flush reason, the
  unaffected CLUT share, the 6x post-IN-522 GPU wait and the target sizes are **Unknown** until the parallel agent runs
  the existing instruments. The savings in section 2 are hypotheses bounded by the measured costs; none is a promise.
- This page is the static half (TASK-215b). The measurement/implementation half (TASK-215) owns the builds and the
  numbers; if a measurement contradicts a hypothesis here, the measurement wins and this page should be corrected.
