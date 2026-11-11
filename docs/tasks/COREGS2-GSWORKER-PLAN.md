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
