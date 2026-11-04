# CoreGS 2.0 CPU survey (TASK-027, 2026-10-30)

The owner asked (2026-10-30) for a survey of every CPU-side cost of CoreGS 2.0 before any of it is attacked, then one
task per front with automated tests. Scope (decision `coregs2-cpu-boundary-with-task134` in ai-memory): the GS
graphics pipeline and the transport to it, from the point where GS data leaves the game side (MTVU output ring, GIF
packets, image transfers, privileged writes, downloads) to the GPU. Game code (EE, IOP/SPU2, VIF up to the MTVU ring,
VU1 programs, build-level speed apart from SSE4.1) stays with TASK-134.

Status: survey done (2026-10-30); the tasks at the end wait for the owner's go-ahead. SSE4.1 is adopted (B1); AVX2
is not (B2). Candidate 313 (`d15a3872...`) = 309 + these instruments + SSE4.1, no visual change.

## Instruments (new in this survey)

- **CPU sampler** (`native/hw_gs/hw_cpu_sampler.cpp`, `GT4_CPU_SAMPLER=<file>`, CTest `hw_cpu_sampler`): a
  statistical profiler of the whole game process. Every millisecond it visits every thread that used CPU since its
  last visit, suspends it only to copy its registers and the top 64 KiB of its stack, resumes it, then walks the copy
  with the x64 unwind tables. `GT4_CPU_SAMPLER_FROM/TO` choose the VBlank window. `scripts/cpu_sampler_report.py`
  (Python test `test_cpu_sampler_report.py`) symbolises the stacks with `llvm-symbolizer` (inlined functions
  included) and prints, per thread, CPU ms per VBlank and the exclusive and inclusive cost of each function. The build
  needs line tables (`/Z7 -gline-tables-only`, link `/DEBUG /OPT:REF /OPT:ICF`), which do not change the generated
  code. Perturbation: about 20 us per visit of a busy thread (2% at one visit per millisecond).
- **Transport line** (`[hw] transport`, with `GT4_HW_PROFILE_EVERY=N`, CTest `hw_stream_sink_transport`): the cost
  of carrying the GS stream from the EE thread to the GS worker: GIF commands and bytes per VBlank, EE time inside
  the hooks and in the byte copy, EE waits (VBlank back-pressure, the drain before a LOCAL->HOST read), pushes that
  wake the worker, and the worker's busy time per command kind, idle time and wake-ups.
- **Exclusive stage times** (`[core2] exclusive ms/vblank`, every 600 VBlanks): each timed stage of the renderer minus
  the stages nested in it, so they add up; `gif_parse` is the GIF front end itself. The stage timers now read the TSC
  (a few ns) instead of `steady_clock`. New stages: `vblank` (Renderer::Flush) and `fifo` (Renderer::ReadFifo).
- **SSE4.1 build switch** (`GT4_SSE41`, owner rule `isa-sse41-mandatory`): `/clang:-msse4.1` on every target of the
  tree; AVX2 stays the A/B switch `GT4_X86_64_V3`.

## Measurements

**Scene and method.** Headless night race (Hong Kong Reverse, B-Spec, 3x, `GT4_HW_CORE=2`, faithful mode), averages
of the 600-VBlank intervals ending at VBlanks 7800-11400 (the race). Headless runs are not paced, so the wall time
per VBlank is how fast the whole emulator can go; present and compose are not run (window runs below). Candidate 310
(`7c04b9b2...`, SSE2, the instruments above, line tables) is compared with 309 on the same intervals: same wall time
and GS thread CPU (for example VBlank 7800: 17.6 vs 17.7 ms; 8400: 13.0 vs 12.9 ms), so the instruments cost nothing
measurable. The sampler itself adds about 5% wall time and its own thread (about 5 ms of CPU per VBlank); profiles
come from the sampled run, all other numbers from plain runs.

### Threads (ms of CPU per VBlank, 310 SSE2)

| Thread | CPU | Role |
|---|---|---|
| EE (game thread) | 10.2 | translated game code, kernel and DMA, IOP and SPU2, VIF, the MTVU journal (it forwards the VU1 worker's GS output) and the GS stream hooks |
| GS worker | 9.4 | CoreGS 2.0: GIF front end, batches, texture cache, CLUT, uploads, Vulkan recording and submission |
| VU1 worker (MTVU) | 8.1 | VU1 programs, VIF1 unpack, PATH1 output into the output ring |
| pipeline compiler | 0.6 | IN-437 specialised pipelines (background) |
| whole process | 27.2 | headless wall 13.2 ms per VBlank, so about two cores busy |

### The transport (`[hw] transport`, per VBlank)

| | 310 SSE2 |
|---|---|
| GIF commands / bytes queued by the EE | 2,589 / 5.9 MB (PATH1 2.0 MB from the VU1 worker, PATH3 3.7 MB of image data) |
| EE time in the hooks (copy) | 0.93 ms (0.49 ms) |
| LOCAL->HOST reads (all the flare visibility table) | 1.0 per VBlank |
| EE wait for the GS worker before the read (drain) / whole read | 1.25 ms / 1.59 ms |
| EE wait for the VU1 worker before the read (`mtvu_wait_download`) | 3.17 ms |
| EE forwarding of VU1 output to the GS (`gs_drain`) | 1.11 ms |
| VBlank back-pressure (2 VBlanks queued) | 0 |
| pushes that woke the worker / worker wake-ups | 167 / 50 |
| GS worker busy (GIF commands) / idle | 8.9 ms / 4.0 ms |

From the sampler on the EE thread: the MTVU journal copies every EE-side GIF transfer into a new heap block before it
reaches the hooks (`EeJournal::append_ee_gif` 0.83 ms, 0.53 ms of it in the allocator), so PATH2/PATH3 bytes are
copied twice; the EE spins in `WaitOnAddress` about 2.2 ms per VBlank waiting for VU1 output (the doorbell), and the
VU1 worker spends 0.74 ms alerting the EE (about 4,900 output doorbell rings per VBlank).

### The GS worker (`[core2] exclusive ms/vblank`, 310 SSE2)

| Stage (exclusive) | ms/VBlank | What the sampler shows inside |
|---|---|---|
| gif_parse (GIF front end) | 2.58 | per vertex: `FrontEnd::Kick`, `ShouldDraw`, `VertexQueue::erase_first` (0.15), `AppendPrimitive`, `DecodeRegisterXyz`, `ConsumeRegList` |
| draw (recording) | 1.68 | `record_draw`, `touch` / `AccessSet::add` (barrier tracking, with heap allocations), descriptor sets; plus the driver below |
| texture (texture cache) | 1.19 | `MemoryTexture` lookups and `DecodeTexture` dispatches (about 300 decodes per VBlank) |
| swizzle (image uploads into local memory) | 0.78 | `StoreRun`, `HostToLocalPageAligned<32>` for 3.6 MB per VBlank |
| translate | 0.73 | `writeback::Translate` |
| clut | 0.59 | 360-475 CLUT loads per VBlank: `ClutUnit::Copy`, `LocatePixel`, `OnClutLoaded` |
| readback | 0.50 | including the GPU fence waits (0.13-0.5 ms) |
| batch_own | 0.38 | |
| glm_hook, glm, patch, written, sprite_check, targets, flush, upload_own | 0.73 together | |
| **all stages** | **9.23** | matches the thread's CPU time (9.38) |

Cross-cutting, from the sampler (ms/VBlank of the GS worker):

- **Batch flushes:** 4.0 ms of batch execution, of which 2.4 ms is flushed by CLUT loads (`OnTex0Write`, the
  "before local read" check) and 1.5 ms by `Kick` (state changes); 465 batches and 12,000 primitives per VBlank.
- **Driver time (amdvlk64, vulkan-1, the kernel submit):** about 1.6 ms: `submit` 0.39 (13-15 submissions per
  VBlank), `record_draw` 0.34, `DecodeTexture` 0.28, `wait_slot` 0.13, `begin_recording` 0.10, `full_barrier` 0.09.
- **Heap allocations:** about 0.3 ms (`AccessSet::add` 0.14, `Gpu::Draw` 0.09, `MemoryTexture` 0.06).
- **Waiting for work:** the condition-variable wait shows 1.6 ms of samples; part of it is the sampler's bias (a
  thread that worked then went to sleep is sampled asleep), the rest is the wake-up and spin cost of about 50
  wake-ups per VBlank.

## Fronts

Each front: what costs, the measured cost (310, 3x headless race, per VBlank), what could remove it, a bound, the
exactness gate, and the effort. "EE" costs lengthen the game thread's critical path, which is what limits the weak
laptop (i5-7200U, EE-bound); "GS" costs are the GS worker's, which matter when the EE waits for it (T1) and as total
CPU on a 4-thread laptop.

### Transport (from the game side to the GS worker)

| ID | Front | Measured | Lever | Bound | Gate | Effort |
|---|---|---|---|---|---|---|
| T1 | The faithful LOCAL->HOST read (the flare table, once per VBlank) waits for the VU1 worker, then for the GS worker, one after the other | EE 3.2 ms (VU1) + 1.6 ms (GS drain and read) | overlap both waits (T3), a faster GS backlog (R fronts), the flare batches submitted early; performance mode already answers it locally | up to about 2 ms of EE wait | frames, the stream hash, readback bytes identical | comes with T3 and the R fronts |
| T2 | Every GIF chunk is copied into its own vector and queued under a mutex with a notify | EE 0.93 ms (0.49 copy), 2,589 commands, 167 wake pushes | one SPSC byte ring with a doorbell that rings only when the worker sleeps; no vector per chunk | EE about 0.5 ms; GS worker the vector recycling and lock | stream order unchanged: frames and `hw_stream_sink*` tests identical | small |
| T3 | The EE forwards the VU1 worker's GS output record by record (`gs_drain`) and journals its own GIF transfers with a heap copy; the VU1 worker alerts the EE about 4,900 times per VBlank | EE 1.1 ms (`gs_drain`) + 0.8 ms (`append_ee_gif`) + about 2 ms spinning on the doorbell; VU1 worker 0.74 ms of alerts | TASK-134's P6, handed to TASK-027: the GS worker reads PATH1 records straight from the MTVU output ring (zero copy), the EE inserts only ordering markers for its own GS work and never waits for VU1 output outside real syncs; ring sizes for a 3 MB L3; `GT4_GS_STREAM_HASH` identity kept | EE about 2-4 ms, VU1 worker about 0.7 ms | stream hash, MTVU verify, frames | large (touches the MTVU journal shared with TASK-134; documented for it) |

### GS worker (the renderer)

| ID | Front | Measured | Lever | Bound | Gate | Effort |
|---|---|---|---|---|---|---|
| R1 | Vulkan submissions and per-submit work | 13-15 submits per VBlank (IN-426 submits every 32 batches on purpose, so the GPU starts earlier and late waits drop): `submit` 0.39 ms + `wait_slot` 0.13 + `begin_recording` 0.10 of driver time | a longer or time-based interval (64 batches, or by recorded work), weighed against IN-426's GPU waits | about 0.3 ms | frames; GPU waits and frametime peaks not worse | small |
| R2 | Heap allocations in the draw path | about 0.3 ms (`AccessSet::add`, `Gpu::Draw`, `MemoryTexture`) | reuse the containers (reserve once, clear per batch) | about 0.3 ms | identical output by construction | small |
| R3 | Draw recording and barrier tracking | draw 1.68 ms exclusive, `record_draw` driver 0.34 ms | fewer state binds per draw, descriptor sets reused or pushed, cheaper `touch` bookkeeping | about 0.5-0.8 ms | frames, CTest A/B | medium |
| R4 | Image uploads swizzled into local memory on the CPU | swizzle 0.78 ms for 3.6 MB | SIMD swizzle (SSE4.1 shuffles) of whole blocks; or upload linear and swizzle on the GPU | about 0.4-0.6 ms | `hw_core2_upload` fixture, frames | medium |
| R5 | Texture cache lookups and decodes | texture 1.19 ms exclusive, `DecodeTexture` driver 0.28 ms, about 300 decodes per VBlank | cheaper key and hash path, decodes batched into fewer dispatches | about 0.4-0.6 ms | frames, texture counters | medium |
| R6 | CLUT loads | clut 0.59 ms + copy and decode about 0.5 ms, 360-475 loads per VBlank | skip reloads with identical source bytes, table-driven `LocatePixel`, SIMD copy | about 0.5 ms | CLUT fixtures, frames | medium |
| R7 | GIF front end per vertex | gif_parse 2.58 ms | strip fast path without per-register dispatch, a ring vertex queue (no `erase_first`), cheaper `ShouldDraw` / autoflush decisions | about 0.8-1.2 ms | front end fixtures (m3_*), frames, stream replays | medium |
| R8 | Vertex translation | 0.73 ms | SIMD translate of whole primitives | about 0.3 ms | frames | medium |
| R9 | Batches split by CLUT loads | 2.4 ms of the 4.0 ms of batch execution is flushed by the CLUT "before local read" check; 465 batches per VBlank | flush only when the pending batch really writes the CLUT's source blocks (counter first: how many of these flushes were needed) | fewer batches, so less of R3 | frames (exactness critical) | medium, needs care |
| R10 | Wake-up cost of the worker | about 50 wake-ups per VBlank | comes with T2's doorbell | small | - | with T2 |

### Build

Candidates 310 (SSE2), 311 (SSE4.1) and 312 (AVX2, `GT4_X86_64_V3`, which also turns floating-point contraction
off) differ only in the compiler's target (the pinned EE game library is the same SSE2 library in all three). Nine
runs interleaved after background programs were closed (the earlier runs, with a chat client busy, were up to 15%
slower and are not used); medians of three, ms per VBlank:

| | SSE2 (310) | SSE4.1 (311) | AVX2 (312) |
|---|---|---|---|
| wall (whole emulator, headless) | 10.00 | 9.62 (-3.8%) | 9.66 (+0.5% vs SSE4.1) |
| EE critical path | 9.53 | 9.22 (-3.3%) | 9.39 |
| VU1 worker busy | 3.52 | 2.97 (-16%) | 2.86 (-3.5% vs SSE4.1) |
| EE wait for the VU1 worker before the read | 1.98 | 1.45 | 1.34 |
| EE wait for the GS worker at the read | 1.42 | 1.71 | 1.88 |
| GS worker CPU | 6.90 | 6.93 | 7.02 |
| process CPU | 20.16 | 19.59 (-2.8%) | 19.38 |

| ID | Front | Decision |
|---|---|---|
| B1 | SSE4.1 (owner rule) | **adopted, `GT4_SSE41` ON by default.** The gain is the VU1 worker (VU1 `vfix` uses `pminsd`) and with it the EE's wait before the read; the GS worker gains nothing, because its hot code is not vectorised by the compiler: the R fronts need hand-written SSE4.1 (swizzle, translate). Frames: see the identity check below. |
| B2 | AVX2 | **not worth a second executable:** wall time equal to SSE4.1 within noise; only the VU1 worker gains a further 3.5%. |

Frame identity: the dumps at VBlanks 8400, 9600 and 10800 of the night race are byte for byte equal across 310, 311
and 312 (and between two 310 runs). CTest on the SSE4.1 tree: 266/267 in both interlock modes; the one failure is
`python_native_contracts`, the 13 `test_gs_settings` / `test_gs_live` cases about the GS settings file that came with
the rc7 merge (files this work does not touch). The line-table build embeds only the PDB's file name
(`/PDBALTPATH:%_PDB%`), so `test_portable_install` (no build-machine path in the executable) passes.

Note on T1: with the VU1 worker faster, the EE now waits longer for the GS worker at the read (1.42 -> 1.71 ms): the GS
backlog is what the read waits for, so GS worker speed (the R fronts) shortens the EE's critical path in faithful
mode.

### Measurement noise

Whole-game headless runs of the same build vary by up to 15% on this desktop (310 SSE2 twice: 13.2 and 11.4 ms per
VBlank; background programs such as a chat client and Process Lasso were active), while the work is identical (2,589
GIF commands per VBlank in every run). Gains of a few percent per front cannot be told apart this way. Hence M0 below:
a CORE2 replay bench that feeds a recorded GS stream to the renderer on one thread, with no EE, VU1 or pacing, so each
R front is measured as GS worker CPU per VBlank of the same stream.

## Proposed tasks (cheapest first)

Each task: one front, an A/B switch where the behaviour could differ, automated tests (CTest fixture or replay, frame
comparison against the previous candidate), the transport and stage lines before/after, and the ledger row.

| Order | Task | Fronts | Expected (3x race, per VBlank) |
|---|---|---|---|
| M0 | CORE2 replay bench. `hw_writeback_replay` with `GT4_HW_CORE=2` already replays `private/slice240-capture/night-10000.gscap` (121 VBlanks of the night race, 8 s) through CORE2, but not as the game runs it: 7-8 readbacks per VBlank (the game: 1) and the targets recreated every VBlank (the reference compose path). Make it replay as the GS worker does (GPU presentation compose, no reference readbacks, repeated passes over the capture), print the exclusive stage times and thread CPU, and record a fresh capture in hw mode (`GT4_GS_CAPTURE` is refused in hw mode today) | instrument | noise-free A/B for every R task |
| 1 | Containers reused in the draw path; the hw sink's per-chunk vector and mutex replaced by an SPSC byte ring with a doorbell | R2, T2, R10 | GS about 0.3 ms, EE about 0.5 ms |
| 2 | Submission interval weighed against IN-426's GPU waits | R1 | GS about 0.3 ms |
| 3 | SIMD block swizzle for image uploads | R4 | GS about 0.4-0.6 ms |
| 4 | CLUT loads: skip identical reloads, faster copy and decode | R6 | GS about 0.5 ms |
| 5 | Texture cache lookup and batched decodes | R5 | GS about 0.4-0.6 ms |
| 6 | GIF front end vertex path | R7 | GS about 0.8-1.2 ms |
| 7 | Vertex translation in SIMD | R8 | GS about 0.3 ms |
| 8 | Draw recording and barrier bookkeeping | R3 | GS about 0.5-0.8 ms |
| 9 | Batches no longer split by CLUT loads that do not touch them | R9 | fewer batches; exactness critical |
| 10 | Zero-copy intake from the MTVU output ring, GIF front end off the EE thread (TASK-134's P6), with the read waits overlapped | T3, T1 | EE about 2-4 ms, VU1 worker about 0.7 ms |

Together the R tasks bound the GS worker at roughly 4-5 of its 9.4 ms per VBlank at 3x; T2 and T3 bound the EE's
GS-related time (about 4 ms plus the read waits) at roughly 1 ms.

## Results (night of 2026-10-30, owner asleep)

Every step: one switch for A/B, 2+2 interleaved headless runs (3x or 12x night race), the frame dumps at VBlanks
8400/9600/10800 compared with candidate 310 (byte for byte), CORE2 CTests.

| Step | Front | Result | Kept |
|---|---|---|---|
| IN-446 | T2: GS stream through a lock-free SPSC ring with conditional doorbells | with IN-447: GS worker 7.20 -> 6.65 ms, EE hooks 0.76 -> 0.47 ms, wall -5.6% | yes |
| IN-447 | R2: barrier access sets as a flat generation-stamped table | draw stage 1.28 -> 1.14 ms | yes |
| IN-448 | MTVU journal queues as reusable slot rings (EE allocations) | no measurable gain on the quiet desktop | no |
| R1 | submission interval 64 / 128 instead of 32 | GS -0.4 ms but the flare read waits longer for the GPU: wall +1% / +4% | no (32 kept) |
| IN-449 | bilinear footprints with textureGather (exact) | +2.6% GPU time at 12x (RDNA4's sampler path is slower than four image loads) | no |
| IN-450 | uploads that change no byte keep the page versions | GS worker -2.8% | yes |
| IN-451 | speculative depth rejection before texturing (exact after a fix: a second descriptor for the speculative read) | 12x GPU 20.65 -> 20.27 ms, frame p50 -5.8% | yes |
| IN-452 | R4: PSMCT32 uploads by whole 8 x 2 columns (SSE2) | 0.61 -> 0.21 ns per pixel; swizzle stage 0.75 -> 0.47 ms | yes |
| IN-453 | R5: texture cache by block identity (the hash of each changed block) | texture stage 0.84 -> 0.36 ms, decode windows 12x less data, GPU -3% at 3x | yes |
| IN-455 | R7: the front end vertex queue as a ring (no copy of the remaining vertices per strip vertex) | front end stage 1.80 -> 1.74 ms; totals within noise; front end fixtures (79 tests) and frames identical | yes |
| IN-456 | R4/R5: the block of a pixel from the address tables instead of `pixelSlot` | upload stage 0.352 -> 0.329 ms; totals within drift | yes |
| IN-457 | R6: CSM1 CLUT loads by table, the renderer's CLUT halfwords as ranges | CLUT stage 0.47 -> 0.26 ms, GS worker -5.3%, wall -2.3% | yes |
| IN-458 | R3: binds equal to the draw pass's held state left out, the vertex ring bound once per pass | draw stage within noise, EE waits for the GS worker -0.09 ms each | yes |
| IN-459 | T3: the VU1 worker wakes the sleeping EE only past 64 KiB of output, when idle or stopping, or with a full ring | EE wake-ups 650 -> 16 per VBlank, process CPU -12%, VU1 worker -7.6%, wall unchanged (the GS worker is fed in larger pieces) | yes |
| IN-460 | GPU: GLM patch kernel with the native words shared per tile | no change at 12x (20.21 ms) | no |
| IN-461 | T3: direct GS intake, the GS worker reads the VU1 output ring itself (`GT4_MTVU_DIRECT=1`) | process CPU -9%, wall -2.7%, EE critical path -2.6%; frames identical | yes |
| IN-462 to IN-469 | Afternoon of 2026-10-30, see the ledger: direct intake by default, self-sampling copies (exactness), GLM hazards by page, statistics query opt-in, strip run skip, PGO and ThinLTO of the GS libraries, VU1 output scan | see `CORE2-HANDOVER-TASK134.md` for the totals | yes |
| IN-454 | whole-image constant fills: the frame by vkCmdClearColorImage, the depth still by the fill shader (Z24 keeps the stored top byte) | exact, but 12x GPU 20.31 -> 20.25 ms (-0.3%): the fills are bound by the depth read-modify-write | no |

**Candidate 324 against 313** (interleaved headless night race): 3x, 2+2 runs: GS worker 7.19 -> 6.41 ms per VBlank
(-11%), process CPU 20.2 -> 18.9 ms, EE stream hooks 0.74 -> 0.47 ms, EE forwarding of VU1 output 0.93 -> 0.65 ms,
EE critical path 9.69 -> 8.96 ms, frame p50 9.73 -> 8.93 ms, p95 11.86 -> 10.89 ms, GPU busy 4.16 -> 3.90 ms. 12x, one
run each: GPU busy 20.77 -> 20.25 ms, frame p50 24.0 -> 22.4 ms, p95 28.2 -> 27.2 ms. 6x, one run each: GPU busy
7.63 -> 7.59 ms; early-Z alone at 6x, 2+2 runs: GPU busy 7.34 -> 7.22 ms (-1.6%), draw passes 4.86 -> 4.73 ms.

Found on the way: GT4's stale texture decodes came from 4 KiB page versions (blocks beside a texture written); the
largest GPU item at 12x is GT4's clears (strips covering whole targets, 3.7 ms of isolated GPU time per VBlank); the
exactness check caught a compiler load merge across the pixel interlock (ai-memory gotcha).

**Candidate 332 (direct GS intake on) against 313** (interleaved headless night race): 3x, 2+2 runs: process CPU
20.21 -> 14.54 ms per VBlank (-28%), GS worker 7.28 -> 5.77 ms (-21%), wall 10.28 -> 8.70 ms (-15%), EE critical
path 9.98 -> 8.47 ms (-15%), frame p50 10.05 -> 8.38 ms (-17%), p95 12.33 -> 11.02 ms (-11%), GPU busy 4.29 -> 4.01
ms, VU1 worker 3.05 -> 2.88 ms. 12x, one run each: process CPU 19.85 -> 15.24 ms (-23%), GPU busy 20.78 -> 20.26 ms,
frame p50 23.97 -> 22.76 ms, p95 28.17 -> 27.62 ms.

Found later the same night: two runs of the same executable (candidate 330) at 12x differ in one frame of the night
race (VBlank 9600: one column of 26 output pixels at x = 4215, the edge of a dark object against the road, black in
one run and grey 16-24 in the other); every 3x comparison of the night was identical. A column at the edge of an
object that is 0 in one run points at pixels read from a pooled image that nothing wrote (12x recycles images more
often); to be traced in its own task.

Still open from the list above: T3 (zero-copy MTVU intake, P6), R3 draw recording, R6 CLUT, R7 front end vertex path,
R8 translate, R9 batches split by CLUT loads, a submission thread, M0 replay bench.
