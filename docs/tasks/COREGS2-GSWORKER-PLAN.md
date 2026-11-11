# CoreGS 2.0 GS worker: aggressive CPU plan (TASK-215, 2026-11-11)

Role: single writer for TASK-215 on `task/215-coregs2-perf`. The GS worker is the measured bottleneck in every scene
(89-98 % of the frame, **busy, not waiting**: VU1 wait 0.00-0.23 ms, GPU wait 0.05-0.35, drain/read 0.00, VU1 AOT
`interpreted_calls=0`). The GPU has slack (busy 1.3-3.7 ms of 16.7) and the VU1 is not the limiter. This round attacks
the worker itself. Every candidate is behind a switch, measured with a 2+2 interleaved A/B, and kept only if it reaches
0.5 ms on the license or Opera with identity (frames byte-identical, same-frame flare, no game logic/clock/digest
change). Local machine only; untainted (no PCSX2/GSdx/fork/ARMSX2 source; GSdx black box); no GPL.

## 1. The stage split (exclusive ms/VBlank, `[core2] exclusive` lines of the existing runs)

| scene | gif_parse | batch_own | clut | translate | draw | swizzle | texture | readback | cpu_sprites | GS worker |
|---|---|---|---|---|---|---|---|---|---|---|
| license B-1 1x | **3.25-3.86** | 0.55-0.63 | 0.20-0.47 | 0.41-0.45 | 0.86-0.94 | 0.51-0.61 | 0.19-0.21 | 0.08-0.10 | 0.35-0.55 | 7.4-7.7 |
| Opera 3x | 2.29 | 0.76 | 0.26 | 0.39 | 0.79 | 0.75 | 0.19 | 0.51 | 0.00 | 6.44 |
| night 3x | 1.75 | 0.80 | 0.16 | 0.55 | 1.33 | 0.82 | 0.29 | 0.30 | 0.00 | 6.70 |
| reference 3x | 4.76 | 1.36 | 0.59 | 1.12 | 2.35 | 0.78 | 0.39 | 0.79 | 0.00 | 13.00 |

Two halves: **front end** (gif_parse + batch_own + clut: 2.7-5.3 ms) and **render** (translate + targets + texture +
draw + swizzle + upload + cpusprites + sprite_check + hook + flush + patch + readback: 2.6-4.4 ms). A perfect two-thread
overlap is `max(front, render) + tail`, i.e. **1.7-3.5 ms** of the 6.4-13.0 ms worker (Hypothesis, bounded by the smaller
half). That is the only lever aimed at the bottleneck itself.

## 2. Why the split is not a plain "parse here, draw there"

The front end and the renderer share three mutable things, and the stream mutates them *inside* parsing:

- **GS memory**: 126-161 uploads/VBlank (1.2-3.6 MiB) and image transfers; translate and the texture decode read it.
- **The CLUT unit**: **1,553 loads/VBlank on the license** (`clut=0.50 (1553)`), one every ~2.8 µs of parse; translate
  and the tone hook decode palettes from it.
- **Targets/GPU**: readbacks (`clut_readbacks=0` on the license but `clut_gpu_loads=37.6/VBlank`), patches and the GPU
  command buffer, which is recorded by one thread into one command buffer.

A split that drains the consumer at every mutation would drain ~1,700 times per VBlank: at ~0.5-1.5 µs per round trip
that is 0.9-2.5 ms of pure hand-off, i.e. the whole gain on the license. So the prototype is designed around *not*
draining at the two hot mutations:

1. **The CLUT is snapshotted in stream order.** A CLUT load becomes a queue item (the write, its flags and the 512
   halfwords after the load); the consumer applies it to its own `ClutUnit` copy and translates each batch with that
   copy. Order is preserved, so the palette a batch sees is the one the stream gave it. Cost: one 1 KiB copy per load.
2. **`beforeLocalRead` (the readback before a load) is only entered when a drawn target can overlap the load's span.**
   The consumer ORs the block spans of every drawn target and of every batch it draws into a shared "may hold drawn
   pixels" bitmap at block granularity; the producer ORs the spans of the batches it emits. A load whose blocks are all
   clear cannot read a drawn target, so it skips the callback **and the drain**. On the license 1,515 of 1,553 loads
   are such loads (`clut_readbacks=0`, `clut_gpu_loads=37.6/VBlank`); the ~38 that remain drain and ask exactly.
3. **Uploads drain** (126-161/VBlank, cheap: the consumer is faster than the producer) and the display/VBlank/EE entries
   drain. No other entry drains.

The consumer owns every GPU call, every target, the memory readbacks and the CPU sprite path; the producer owns the
front end, GS memory writes and the batch emission. `GT4_CORE2_GS_PIPELINE=1` selects it, **default off**; the
switch-off path is byte-exact (same code, same order, `OnBatch` calls `ProcessBatch` directly).

## 2b. What the read-only GSdx study changes (prose only, no code)

`docs/gsdx/GS-THREAD-CPU-COST.md` (branch `docs/gsdx-pipeline-study`, read-only, separate agent) is the **existence
proof** for this split: GSdx's Vulkan path already runs a front thread (GIF parse, vertex kick, flush decision, present)
and a **back** thread (texture cache, validation, command recording) fed by a ring of **self-contained records with an
environment snapshot and pooled vertex/index buffers**; on a device where downloads read live memory, the split is
**refused** and the pixels are the same. That confirms three rules of our prototype: the record must be self-contained
(the `Batch` already is: state snapshot + vertices + prims), buffers are pooled and handed across the ring, and
backpressure is the only coupling. It also confirms where we must differ:

- **They have no CPU translate** (raw 12.4 words, the shader converts). We keep `TranslateBatch` (0.39-1.12 ms) for now:
  removing it means a vertex-format change with the CPU reference as oracle, which is not a first prototype.
- **They have no per-primitive and no per-draw record** (the index buffer is the record); we write 26.5k `PrimRecord`s and
  9,197 `GpuDraw`s per VBlank. Adoptable later (5.4 in their page); the per-primitive `rect` is an exactness feature
  (scissor), so it needs a fixture.
- **Their kick is one fused SIMD pass**; ours is `Kick` -> `MakeVertex` -> `ShouldDraw` -> `AppendPrimitive` per vertex.
  Their bound (0.5-1.0 Hyp) is lower than our own measured attempt (TASK-171 option 1, below the bar) because that attempt
  only batched the queue push and kept the per-vertex cull/candidate; a fused pass must fold the cull and drop the
  per-primitive record to reach it. Not this prototype.
- **Their flush is cheap and deferred** (push a buffer, draw later); ours draws synchronously in `OnBatch`. **Our split is
  precisely what makes our flush cheap**: the front thread only enqueues.
- **One command buffer per frame, submitted at present**, extra submits only for a readback/spill/allocation. Our desktop
  cadence was already swept (32/64/128, do not retry).
- Their per-draw **state vector with dirty flags** and grown buffers: **we already have both** (IN-458 draw sets, IN-360/B4
  capacity reuse) and the study says do not chase the GPU side: our renderer is 9.0 %/11.4 % faster at 3x/12x on the same
  recording, and our draw recorder is 0.94 ms for 9,197 draws (~102 ns each). So Phase 3b (draw recording) is **dropped**
  from this round, and the "2.85 ms per draw" figure is corrected: 2.85 ms per VBlank across ~500 real draw batches.

**Net change to the design:** none in the split boundary; it is strengthened by the refusal rule (drain where a download
reads live memory, i.e. our readbacks, instead of refusing the whole pipeline), and it adds one ordering rule we already
had (present/merge stay on the front thread and drain first) and one record rule (self-contained state snapshot, which the
`Batch` is). Phase 3b is dropped, and the two remaining GSdx-shaped candidates (fused kick, index-at-kick) are recorded
with their bounds and gates but not attempted in this round.

## 3. Candidates

| # | candidate | expected | effort | risk | identity / test |
|---|---|---|---|---|---|
| P1 | two-stage pipeline: front-end thread + bounded SPSC item ring + render thread (CLUT snapshots, drawn-block shadow, drains only on uploads/VBlank/EE) | **1.7-3.5 ms** (H) | 3-5 d | high (state ownership, readback order) | license 1x + Opera 3x frames byte-identical 4/4; then night 3x/6x, four Spec II scenes, Arcade; `game_identity.sh` state dump + clock; MT anxiety: 0 interpreted, same digests |
| P2 | third worker / GPU compute for texture decode + swizzle + CLUT (0.19-0.39 + 0.51-0.82 + 0.16-0.59) | 1.0-1.8 ms (H) | 2-4 d | medium (memory read order) | same as P1; the swizzle bytes and the decoded texels compared with the CPU path |
| P3a | `gif_parse` hot loop: branchless vertex decode, decode tables, fewer per-primitive state checks (no cull/state change) | 0.3-0.8 ms (H) | 1-2 d | low | front-end fixtures (`hw_m3_tests`), frames identical |
| ~~P3b~~ | ~~draw recording~~ | **dropped** | - | - | the GSdx study measured our recorder at ~102 ns/draw and our renderer 9-11 % faster; not a lever |
| P3c | batch/translate/CLUT pools: reuse the vectors and the 1 KiB palette copies (no alloc/free per batch) | 0.2-0.4 ms (H) | 0.5-1 d | low | frames identical; allocation counters |
| P3d | swizzle (`FastHostToLocal`) wider stores / SIMD | 0.2-0.4 ms (H) | 0.5-1 d | low | `GT4_CORE2_UPLOAD_CHECK=1` equal on a run; fixture |
| P3e | GSdx-shaped, **recorded only**: fused kick (cull folded in, index-at-kick) and no CPU translate | 0.5-1.0 / 0.2-0.4 ms (Hyp) | 2-3 d each | high (vertex/record contract) | differential kernel fixture + frames identical |
| P4 | combine, full matrix, re-measure, report | - | 0.5 d | - | the matrix above |

Order: P1 (the only lever at the bottleneck) → P2 if P1 works → P3a/P3c/P3d in measured order → P4. P3e is recorded,
not attempted this round.

## 4. Do not retry (measured, in the ledger)

VU1 AOT/coverage (closed, 0 interpreted); flare readback (closed, no address/LQ; path 2 has no exact same-frame form);
palette segments under the tone hook (IN-548, **+0.61 ms**); per-record vertex conversion (IN-552, ~0); per-batch flush
merge (IN-554, ~0.1 ms); thread pinning (IN-488); IN-541 CLUT scope, IN-542, IN-505, IN-537/539; the submission sweep
(32/64/128); GSDX-PLAN G5/G6/P3; any renderer rewrite, JIT or PCSX2 code. The batched strip decode (TASK-171 option 1)
was measured and reverted: do not re-propose it without a new mechanism.

## 5. Method

`build/perf-base` (v2.00 target) and `build/spec2-215` (Spec II) at `3c0be6f9`; headless, shader cache warm, plain
release; 2+2 interleaved (`ab-pair.sh`), intervals of 600 VBlanks read from the last `[core2]` lines; identity with
`frame_diff.py` on the dumped frames plus `game_identity.sh` (state dump byte for byte, race clock, dynamics); scene
tags from `common.sh`. Gate: keep only a measured **≥ 0.5 ms** on license 1x or Opera 3x with 4/4 identical frames and
no state/clock difference; anything inconsistent (e.g. one pair up, one down beyond noise) is reverted.

## 6. Phase 1 result: the two-stage prototype measured **5.4x slower** (reverted, 2026-11-11)

Implemented behind `GT4_CORE2_GS_PIPELINE=1` (default off) as designed above: the front end on the GS worker, a 128-item
SPSC ring of self-contained `Batch` + CLUT items, the consumer translating and drawing them, in-order CLUT snapshots, the
drawn-block readback shadow, and drains only at uploads, VBlank and the EE entries. The switch-off path stayed exact (same
build, switch off: GS stages 8.82-9.90 ms, wall 10.18-10.23, `gpu batches` 613k, the old split). The switch-on path **ran
and rendered**, but measured:

| license B-1 1x, last 600 VBlanks | switch off | pipeline on | delta |
|---|---|---|---|
| wall ms/VBlank | 10.18-10.23 | **39.1-44.3** | +29 to +34 |
| GS stage total (exclusive) | 8.82-9.90 | **47.7-53.1** | +39 to +43 |
| `gif_parse` (the producer) | 3.61-4.92 | **37.2-41.7** | +33 to +37 |
| `batch_own` (the consumer) | 1.14-1.17 | **4.11-4.62** | +3.0 to +3.5 |
| `targets` (the consumer) | 0.04-0.05 | **2.22-2.52** | +2.2 to +2.5 |
| `draw` / `translate` / `texture` / `clut` | 1.14 / 0.45 / 0.24 / 0.44 | 1.21 / 0.41 / 0.25 / 0.51 | equal |
| **drains, drain wait** | - | **1,967-2,051 per VBlank, 18.7-21.0 ms** | - |
| items produced | - | 6,745-10,127 per VBlank | - |

Two independent, measured causes, both structural:

1. **The synchronous readback-before-CLUT-load is the hand-off bill.** The renderer must reduce a drawn target into GS
   memory before a palette load reads it; with two threads that is a producer stall. Measured: ~2,000 drains/VBlank and
   **18.7-21.0 ms/VBlank of drain wait**, more than the whole serial frame. The shadow cannot be tightened enough: the
   license's palettes live under the frame buffer, whose blocks are drawn every VBlank, and there are ~8,300 CLUT-buffer
   rewrites per VBlank (`SetClutLoaded`) against 1,553 `flushBeforeLoad` loads, each rewrite one queue item.
2. **The renderer's memory/version bookkeeping assumes one thread.** With the producer ahead of the consumer, the
   consumer's untimed per-batch work doubles (`batch_own` 1.14 -> 4.11) and its target work grows ~50x (`targets`
   0.04 -> 2.22): `ApplyDirty` patches the rectangles the producer's uploads marked dirty, and the accumulated dirty
   rectangles plus the block-identity and `writeSequence_` traffic inflate when the writer and the reader are separated by
   a queue. The translate/draw/texture/CLUT stages themselves did **not** inflate, so the split is not the cost - the
   coupling is.

**Verdict: reverted** (39 ms below the bar; the round's rule requires the revert). The prototype does prove the model runs
and renders, and it prices the missing piece: the split needs **memory-version ownership** (GS memory, CLUT and dirty
pages versioned per record or per segment, so a readback is a queued task with a completion ticket instead of a producer
stall) and **one owner for the cache/version state**. That is the 3-5 day design round the option was priced at. For the
next attempt, measure `drains/VBlank`, the drain wait and the consumer's `batch_own`/`targets` inflation first: they are
the three numbers that decide the design.

## 7. Pivot round: the contained candidates, each measured or stopped at its contract

The owner's pivot after IN-556: the GSdx study's contained wins, gate relaxed to **>= 0.2 ms** per candidate on the
license or Opera (2+2 interleaved, judged on wall/GS worker) with a **combined total >= 0.5 ms**. All numbers below are
license B-1 1x, last 600 VBlanks, on `build/perf-base` built from `a3a26e39` plus this round, 2+2 interleaved where an
A/B exists.

### The front-end primitive bill (new diagnostics, kept)

`[core2] front-end prims/vblank emitted=... skipped=... (culled=...) vertices=... culled_share_of_drawing=...` and the
`PrimsCulled` counter (added last, so no existing counter changes value). Per VBlank on the license, three intervals:

| interval | vertices | prims emitted | prims skipped | of those culled | culled share of drawing |
|---|---|---|---|---|---|
| 1 | 109,530 | 15,698 | 82,574 | 24,516 | 61.0 % |
| 2 | 131,524 | 16,923 | 101,244 | 29,082 | 63.2 % |
| 3 | 84,094 | 17,039 | 55,635 | 11,883 | 41.1 % |

So **41-63 % of the drawing primitives are rejected by the cull**, and ~80 % of all primitives are non-drawing strip
kicks (the ADC markers). The bill is what any kick/record candidate has to attack; the diagnostics cost nothing
(two neutral runs: wall 9.92 / 9.27, stage total 8.45 / 7.89, inside the earlier range).

### 1. Fused kick + cull folded + index-at-kick

- **The cull folded into a strip run: implemented, measured, reverted.** A triangle-strip run's band (`StripVertices`'
  vertices plus the queue's, in 12.4 units) tested against `ShouldDraw`'s own cull rectangle, shared through a
  `ScissorViewFor` helper so the constants are the same; a clear band let every drawing kick of the run count as culled
  without a candidate build, a `ShouldDraw` call or an append (`GT4_CORE2_BAND_CULL`, with a `PrimsBandCulled` counter).
  Result: **83.8 primitives skipped per VBlank of the 11,883 culled (0.7 %)**, i.e. ~0.003 ms - below the bar. The culls
  are per-primitive (coverage box, degenerate triangles), not whole off-screen strips: GT4's strips are long and nearly
  always partly visible. The code was reverted; the counters stayed.
- **Batching alone** was measured before the round (TASK-171 option 1, `GT4_CORE2_BATCH_STRIP`): **-0.15 ms**, inconsistent
  between pairs, reverted. Not repeated: its lesson stands (the queue round-trip is not where `gif_parse` lives).
- **Index-at-kick (the record removal): stopped at the contract.** `Batch::prims` (`PrimRecord{type, first, count, rect}`)
  is read by `writeback::TranslateBatch` (draw grouping and the per-draw scissor), `cpuref::DrawBatch` (the CPU reference,
  the oracle), `PossibleShuffle`/`PossibleShuffleSegment`, the `PaletteSegment*` helpers, `RewriteDepthClear`,
  `ClassifyOverlap`/`BuildSpriteDrawList`/`ComputeVertexTrace`, and `hw_m3_tests` asserts on it. Replacing it with an index
  buffer built at kick time is the full per-record design (IN-552 priced the vertex-conversion half at ~0 and the whole
  design at **0.15-0.3 ms**), which is exactly the contract point the round said to stop at. `GpuDraw` (9,197/VBlank) is
  the other half of the same contract. **No code.**
- **Related exact micro-win, measured and reverted as sub-bar:** `IntersectRects`/`UnionRects`/`RectsOverlap` moved inline
  into `hw_batch.hpp` (they were cross-TU calls made per drawing primitive: ~41k/VBlank). 2+2 interleaved:
  **GS stage total 7.84/7.80 -> 7.68/7.68 (-0.13, both pairs)**, `gif_parse` 3.31/3.30 -> 3.24/3.21, `translate`
  0.45 -> 0.41; **wall unchanged** (10.44/10.34 -> 10.40/10.34), because this window of the license is EE-bound. -0.13 is
  real and consistent but below the 0.2 gate as a standalone candidate, so it was reverted; it is the first item to
  bundle if a future round needs the last 0.1 ms.

### 2. GPU compute swizzle

**Stopped at the data-ownership contract.** The CPU swizzle (`FastHostToLocal` -> `StoreRun`, 0.59-0.65 ms) writes the
swizzled texels into the CPU GS-memory shadow, and that shadow is authoritative for the CPU reference, the CLUT loads,
the upload memo's diff (IN-450/IN-453 block identities) and `ReadFifo`. A compute pass can only write GPU memory (the GLM);
making the GLM authoritative for uploads is the "GLM owns GS memory" design (the same class of change as the pipeline's
memory-version ownership), and copying the result back to the CPU shadow costs what the swizzle costs: IN-548 measured a
1 KiB palette copy at **+0.61 ms for ~900 copies** (~0.68 us per KiB, ~1.5 GB/s effective for scattered runs), and the
license moves 1.2-1.3 MiB per VBlank. No contained version reaches 0.2 ms.

### 3. Texture worker only

**Stopped at the GPU-recording contract.** The texture path is not CPU-only: `MemoryTexture` calls `Gpu::DecodeTexture`
/`DecodeTextureGpuClut`/`PaletteRowFromGpuClut` (command recording into the one command buffer) and `CreateImage`/
`UploadImage`, and `fromGlm` can call `Readback` (submit + wait). A worker therefore needs a second command-buffer recorder
and a submission serializer - the exact thread-safety problem that made IN-556 fail, and its CPU part (texture 0.22-0.25 +
swizzle 0.59-0.65) is only a copy-vs-compute trade on top. Not attempted: it is not contained, and the round's rule is not
to repeat the pipeline's coupling.

### 4. No CPU translate

**Stopped at the per-record contract.** `writeback::Translate` converts every `Vertex` (int32 x4/y4, float q/s/t) into
`GpuVertex` (int16 x/y, resolved UV), and `GpuVertex`/`BatchTranslation` are read by the draw path, the CPU reference, the
self-copy/fill/prim-id diagnostics and the fixtures. Leaving the raw 12.4 words for the shader means a new vertex format
and a shader-side conversion, with the CPU reference as the oracle - the full per-record design (IN-552: 0.15-0.3 ms,
2-3 days) - so it is out of a contained round.

### Verdict of the pivot round

Nothing reached the 0.2 ms gate: the one candidate that could be built quickly (the run cull) fired on 0.7 % of the culls
(~0.003 ms), the one exact micro-win measured (-0.13) is sub-bar, and the other three stop at data-ownership,
GPU-recording or per-record contracts. **Kept: the diagnostics only** (`PrimsCulled` + the front-end prim line), which
price every future kick/record candidate for free. The next contained attempt should bundle the rect inlining with a
second exact micro-win (the queue-window candidate build, `pixelSlot`/`BlockOfPixel` inlining) to clear 0.5 together,
or take the per-record design as its own round.

## 8. Bundle round: two exact front-end items kept (-0.33 license, -0.23 Opera), `BlockOfPixel` reverted

Owner's instruction after IN-557: bundle the exact micro-wins, add the parser candidate the prim bill points at, and
measure on **Opera 3x** (GS-bound: 6.44 of 6.60) as well as the license (EE-bound). Gate: **combined GS stage >= 0.5 ms on
Opera 3x**, no regression on license/night, frames byte-identical; if the bundle does not clear 0.5, revert the items that
do not add up and report the exact total.

| item | what it does | license 1x (2+2, GS stage total) | Opera 3x (2 pairs, window 20,400-21,600) | kept |
|---|---|---|---|---|
| **Candidate window** | `FrontEnd::Candidate` names the queue's own vertices (3 pointers) instead of copying up to 96 bytes per drawing primitive (~41k/VBlank); the queue is not advanced until after the cull and the append, so the pointers stay valid | **gif_parse 3.31/3.45 -> 3.09/3.26 (-0.21)** | **gif_parse 3.41/3.41 -> 3.23/3.18 (-0.21)** | **yes** |
| **Rect inlining** | `IntersectRects`/`UnionRects`/`RectsOverlap` in `hw_batch.hpp` (they were per-primitive cross-TU calls) | translate 0.46/0.47 -> 0.42/0.43 | **translate 0.70/0.69 -> 0.60/0.60 (-0.095)** | **yes** |
| `BlockOfPixel` inline | the block-of-pixel table read inlined into the renderer's block loops | (in the first bundle) | **draw 1.43/1.43 -> 1.51/1.60/1.66, swizzle +0.1: the total went +0.09** | **no, reverted** |
| `pixelSlot` inline | not contained: it needs the memory module's runtime in-page tables and `psmInfo` in the header | - | - | no (report) |
| **Bundle (the two kept items)** | | **GS stages 8.01/8.04 -> 7.52/7.87 (-0.49/-0.17, mean -0.33)** | **8.96-9.00 -> 8.75-8.76 (-0.21/-0.24)** | **kept (below the 0.5 target)** |

- **`BlockOfPixel` did not add up** and is the round's one revert: with it the Opera total was 9.13 against a base median
  of 9.04 (four pairs: 8.79/8.93/9.34/9.72 against 9.02/9.03/9.05/9.41), with `draw` 1.44 -> 1.56 and `swizzle` 0.97 -> 1.04,
  while `gif_parse` still improved. Inlining that ~30-line table walk into `Translate`'s `WriteSpan` path bloats the
  translation/draw code enough to cost more than it saves on this scene. Removed; the two front-end items alone give the
  numbers above, with `draw`/`swizzle` back to base (1.45/1.46 against 1.43; 0.94/0.97 against 0.94/0.96).
- **The 0.5 target was not reached** (-0.33 license, -0.23 Opera). Both items are exact and consistent on both scenes and
  above the 0.2 per-candidate bar as a unit; the report states the measured total rather than claiming the gate.
- **A bug found and fixed in the Candidate change:** `AppendPrimitive` dereferenced all three pointers while a point names
  one and a line or sprite two. It crashed the first bundle build (exit 139); the fix copies only `count` vertices, which
  is what the old code effectively did.
- **Identity (frames byte-identical, `frame_diff.py`):** license B-1 1x 2/2, night race 3x 5/5, Opera 3x 3/3. CTest 319/320
  (the pre-existing rc15 `python_native_contracts`) and Python 827 with that same failure; the front-end fixtures
  (`m3_hw_front_end_*`) pass, which is the semantic gate for the Candidate change.
- **Machine noise, for the record:** the license window is EE-bound (wall 8.99-10.48 across runs of the *same* binary,
  while the GS stage stays 7.5-8.1), and Opera's interval totals swing 8.96-9.41 at 20,400-21,600 between runs of the same
  binary. The A/B is therefore judged on the GS stage total with interleaved pairs, never on a single stage or on wall.

## 9. ADC markers: the exact fast skip already exists (measured, no new code)

The bill now splits the vertices (`VerticesDecoded`, added last in the enum): license 1x, **84,094 vertices/VBlank of which
51,282 are decoded (61 %) and 32,812 (39 %) are counted without decoding**. The skip is IN-467's run rule in
`StripVertices`: of a run of non-drawing strip kicks only the first goes through `Kick` (the flush decision), the middle
ones are counted (they can only pair with other non-drawing vertices, so no primitive they touch can draw) and the last two
stay for the next kick; `Kick`'s IN-366 fast path then does no candidate build, no cull and no append for a marker. What
remains per decoded vertex is `MakeVertex` (needed: the vertex is part of a later triangle) and the queue push, and per
drawing kick the cull and the append. There is no further exact skip to add without the fused-kernel/record design, so
this item is **measured and closed** (the do-not-retry list already carries the batched strip decode, TASK-171 option 1:
-0.15, reverted).

## 11. Per-record contract (index-at-kick + no CPU translate): priced by measurement, stopped

Owner's next round after the bundle: the per-record contract as one change, 2-3 days, gated at **>= 0.2 ms GS stage** on
license 1x or Opera 3x, the CPU reference as oracle, `GT4_CORE2_RECORDS=1` off by default until the gate.

### The two halves, priced before building them

**No CPU translate (shader converts the raw words): measured at 0.02 ms - dead.** The contract's CPU side is exactly what
`TranslateBatch` does per primitive: copy the primitive's `Vertex`es into temporaries, convert each with `ToGpuVertex`,
insert the converted records, and build the `GpuDraw`. The exact, contained slice of that (convert straight into the
output slots, no `Vertex` temporaries, no temporary array, the de-index as a 4-byte lane copy) was built and measured on
license B-1 1x, 2+2 interleaved against the kept bundle: **`translate` 0.41/0.41 -> 0.39/0.39 (-0.02)**, with every other
stage inside its noise band (GS total 7.44/7.64 vs 7.94/7.69 - the drift dominates). So the compiler already collapses the
copies and the conversion: **1.2 ns per primitive of the 23 ns the per-primitive body costs.** Moving the conversion into
the vertex shader would remove ~0.02 ms, not the 0.2-0.4 the GSdx study hypothesised for its own code shape, and it would
need the vertex format, the pipeline input state, the shader and the CPU fill/disjoint paths changed (the fill path reads
the *converted* list for its geometry). Reverted as sub-bar.

**Index-at-kick (no per-primitive `PrimRecord`): 0.05-0.07 ms, dead.** What the record costs is the `PrimRecord` write and
read: 20 bytes written per primitive (17,039/VBlank) plus the `UnionRects` that also maintains `batch_.drawRect` (which the
front end needs for autoflush and the depth-clear rewrite, so it stays). The `record.rect` itself is derivable from the
primitive's vertices (the coverage box is a pure function of them, as `ShouldDraw` shows), so it could be dropped - that is
the whole saving: ~16 bytes and 1-2 ns per primitive, **~0.05 ms**, plus ~0.02 for the `prims.push_back`. The *rest* of the
per-primitive work is `TranslateBatch`'s body (23 ns/primitive): the two `ToClip`/`Intersect` pairs for the 1x and the
scaled scissor (IN-391/IN-397 exactness), the `GpuDraw` fields, the union and the counters. **A record cannot remove that**:
every primitive needs its own scissor because its coverage differs from its neighbours', so a draw per primitive is the
exact shape.

### What a real merge would take (the only remaining form of the idea)

To stop paying per-primitive scissors you must draw a *run* as one draw: one scissor per run (the union of its primitives'
coverage, which is exactly the coverage box of the run's vertex span, as shown above), one `GpuDraw` with a strip topology.
That is a **renderer** contract, not a vertex/record one:

- `GpuTopology` has Points/Lines/Triangles only; a strip needs a new topology (or index expansion) in
  `core2_gpu.cpp`'s pipeline selection and `record_draw`.
- The CPU paths that read the per-draw geometry (`CarryDepthFillIntoColour`, the constant-fill/compute-fill path, the
  disjoint-group interlock, `PossibleShuffle`) assume the converted list and the per-draw rect; they would read the run's
  span and rect instead.
- The target bookkeeping (`frameTarget->drawn`/`valid`, `lastDraw`) would move from per-primitive rects to the run's
  union, which is *wider* where a run has culled primitives (the cull drops primitives inside a run), so the tracked area,
  the ageing and the readback decisions change - an exactness question of its own, not a mechanical change.
- Bound: the per-primitive `TranslateBatch` body (23 ns x 17k = 0.39 ms) minus the run-level cost (a few hundred runs) plus
  the wider-rect bookkeeping risk. It is the only path to 0.2-0.4 ms here, and it is its own 2-3 day round with the identity
  matrix, not a slice of this one.

### Verdict

The contract as specified (index-at-kick + no CPU translate) has **~0.07 ms of addressable headroom, measured**, below the
0.2 ms gate, and the remaining per-primitive cost is inherent to per-primitive scissors. Stopped at the contract point with
the measurement rather than building the 2-3 day change; the only route left is the run-as-one-draw renderer contract
described above. Nothing kept this round (the in-place conversion slice was reverted, -0.02).
