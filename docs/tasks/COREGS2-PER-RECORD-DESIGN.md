# CoreGS 2.0: per-record design note (prepared 2026-11-10, **not implemented**)

Owner decision pending: spend 2-3 days on the per-record front end, or leave it and go to the flare (CoreGS 3.0) work
instead. Nothing in this note is in the tree; the measurements below come from TASK-215 and TASK-171.

## 1. What the front end produces today

`FrontEnd::Kick` / `StripVertices` decode every GIF vertex into a `Vertex` (12.4 fixed point x4/y4, z, RGBA, fog, float
q/s/t, 10.4 u/v), cull it (`ShouldDraw`), and `AppendPrimitive` **copies** it into `Batch::vertices` (a `std::vector`)
together with a `PrimRecord{type, first, count, rect}`. `EmitBatch` moves that `Batch` to the renderer sink
(`Renderer::OnBatch(Batch&&)`).

A `Batch` (`native/hw_gs/frontend/hw_batch.hpp`) carries: the register snapshot (`BatchState`: environment, context,
effective PRIM, CLUT generation), the class, the vertex and primitive vectors, `drawRect`, the chapter 02.9 `VertexTrace`,
the flush reason, and the TASK-168 palette segments (a 1 KiB CLUT copy each).

## 2. The three consumers of `Batch` (the contract)

1. **Core 2 GPU path.** `ProcessBatch` → `TranslateBatch(batch, local, clut, t, counters)` fills a `BatchTranslation`
   (its own `std::vector<gpu::GpuVertex>`, `std::vector<gpu::GpuDraw>`, palette rows, texture state) → `DrawBatch`
   records the GPU commands. `TranslateBatch` re-reads every vertex, resolves tex coords, CLUT/colour conversion and the
   draw grouping (`drawSegments` for palette segments), and calls the texture hooks (`TextureFromTarget`, `TextureFromGlm`).
2. **CPU reference** (`cpuref/cpuref_draw.cpp`). Draws a `Batch` into shadow memory; takes segment 0's palette for an
   indexed batch (`DecodeSegmentPalette(batch, 0)`) and walks the segments per primitive. It is the fidelity reference.
3. **Fixtures.** 34 `hw_core2_*` tests and 22 `m3_hw_*` tests build `Batch`es (by hand or through the front end) and
   compare GPU against reference pixels; `hw_cpuref_replay` replays recordings through both paths.

## 3. What the record form would change

- `Batch` gains a **record form**: per primitive (or per draw run) a fixed-size record holding what `TranslateBatch`
  computes today from scratch - palette row, texel rectangle, clip/test state, the draw's vertex span and its draw-set
  key. The front end fills it while the register snapshot is already in hand, so the renderer skips the per-vertex
  re-read and the per-batch vector rebuild.
- `Batch::vertices` / `prims` **stay** as the reference form: CPU reference, fixtures, the segment path and every
  fallback keep using them. The record path is behind `GT4_CORE2_RECORDS=1` (`0` = today's behaviour, byte for byte).
- The `BatchTranslation` vectors become a renderer-owned arena that is reset per batch instead of refilled per batch.

## 4. Bound and its basis (license B-1 1x, exclusive ms/VBlank)

| stage | ms | removable by a record |
|---|---|---|
| `draw` (command recording) | 0.93 | partly: the per-draw assembly, not the GPU recording |
| `batch_own` | 0.64 | partly: per-batch vectors, trace and bookkeeping |
| `translate` | 0.45 | mostly: the per-vertex conversion |
| `clut` | 0.48 | no: palette rows and loads are required work |
| `swizzle` | 0.49 | no: guest uploads |
| **total** | **2.99** | |

Counts in that window: **1,930 batches, 9,197 draws, 26.5k primitives** per VBlank → 1.55 us per batch, 113 ns per
primitive. The removable part is the copy plus the conversion: `AppendPrimitive` 0.126 + `emplace_back` 0.081 + memcpy
0.063 = **0.27**, `translate` 0.45, and part of `draw` 0.93. **Bound: 0.5-1.0 ms** (Hypothesis; 6-12 % of the license's
8.2-8.5 ms GS worker). The measurement precision of a 2+2 on this scene is about +/-0.1 ms, so anything under 0.2 ms is
not a win.

## 5. Risks

1. **Draw grouping is batch-wide.** `TranslateBatch` groups vertices into draws from batch-wide facts (texture
   footprint, `VertexTrace`, the draw-set key). A per-primitive record must carry the same key or the group boundary
   moves and pixels change.
2. **Batch-wide aggregates** stay needed: `AutoflushWanted`, culling, `drawRect`, `trace`, `RewriteDepthClear`, the
   tone routing (`RoutePaletteSegments`, per-segment geometry) and the CPU sprite path all read the whole batch.
3. **56 renderer fixtures plus the CPU reference** consume `Batch`; the record path must be pixel-identical or the
   fixtures must keep running the reference path (they do, by building `Batch`es).
4. **Lifetime.** `Batch&&` moves into the renderer and `hw_cpuref_replay` reads batches after the fact; a record arena
   must outlive the batch or be copied, which eats the win.
5. **Size.** A record large enough to hold every draw's state can exceed the `Vertex` + `PrimRecord` + translation
   vectors it replaces. The win only exists if the record is smaller than what it removes.

## 6. Identity gates (a keep needs all of them)

- License B-1 1x, 2+2 interleaved: wall, GS worker, p99; **frames 4/4 byte-identical**.
- Night 3x and 6x, Arcade 3x, showroom: frames identical.
- CTest **319/320** (only the pre-existing rc15 `python_native_contracts`), with `hw_core2_palette_segments`,
  `hw_core2_tone_early_drop`, `hw_core2_texture_replacement`, the 22 `m3_hw_*` and `hw_cpuref_replay` green.
- `GT4_CORE2_RECORDS=0` reproduces today's output byte for byte (the reference path stays reachable and tested).

## 7. Smallest first step (if approved) - **stopped and corrected, see section 8**

**Only the vertex conversion, one switch, no grouping change.** `FrontEnd::AppendPrimitive` writes the
`gpu::GpuVertex` values into a renderer-provided arena, and `TranslateBatch` skips its per-vertex conversion when the
batch carries them (`GT4_CORE2_RECORDS=1`). Culling, `drawRect`/`trace`, draw grouping, tone routing, segments and the
CPU reference are untouched; the reference path stays the default until the gates pass.

- Expected: most of `translate` (0.45) - Hypothesis 0.2-0.4 ms.
- Then, only if that lands exactly: the draw assembly inside `draw` (0.93) is the second step and carries risk 1.
- Cost: 2-3 days for both steps; the first step alone is about one day.

**Decision for the owner:** 0.5-1.0 ms of CPU on the license (Hypothesis) against 2-3 invasive days, versus the flare
work on the GPU path. The clean reference below prices the flare side: 10x GPU busy 14.27 / flare wait 5.77, 12x
19.98 / 9.85 ms.

## 8. Correction (2026-11-11): the vertex conversion is **not** the prize - measured

The section 7 step was priced **before** implementing it, with a work-injection diagnostic on license B-1 1x
(`GT4_CORE2_GV_DOUBLE=1`: one extra `ToGpuVertex` pass per emitted GPU vertex, result kept), 2+2 interleaved:

| | normal | one extra conversion pass |
|---|---|---|
| `translate` (ordinary 32-byte arena store) | 0.460 / 0.460 | **0.460 / 0.450** |
| `translate` (forced volatile store) | 0.540 / 0.535 | 0.770 / 0.755 |
| GS worker | 8.305 / 8.400 | 8.435 / 8.270 |
| wall | 8.730 / 9.165 | 9.655 / 8.830 |

The pack plus an ordinary store is **below the noise floor**; the +0.22 ms of the volatile variant is the forced uncached
writes, not the pack. This agrees with the CPU sampler: the whole inlined body of `writeback::Translate` is
**0.135 ms/VBlank** (0.405 inclusive), so whatever the conversion costs lives inside 0.135 - under the 0.2 ms bar.

Consequences:

- "Move the conversion into the front end" cannot win: it re-adds the same pack **plus** a 32-byte store, and the removal
  saves at most a fraction of 0.135 ms.
- **Sections 4 and 7 are corrected**: the per-record lever is not the vertex conversion. What is left is `draw` 0.93 (GPU
  command recording, which a record does not remove), `batch_own` 0.64, `clut` 0.48, `swizzle` 0.49; a record could touch
  part of `batch_own` and the draw assembly. Corrected bound **0.15-0.3 ms** (Hypothesis) - at or under the bar.
  **Recommendation: do not spend the 2-3 days; treat the lever as closed.**
- **Exact contract point where the step stops:** `Batch::vertices` is `std::vector<Vertex>`, and its int32 `x4`/`y4`,
  float `q`/`s`/`t` and `u`/`v` are read by the CPU reference, `TextureFootprint`, `PossibleShuffle*` and the front end's
  cull/trace. `gpu::GpuVertex` is the GPU's 32-byte record with `x`/`y` truncated to the low 16 bits. Replacing `Vertex`
  by the record is the full design of section 3, not an isolated step - so the step was stopped here, not implemented.

## 9. What is *not* in this design

- No change to the batch record for the palette segments (the 1 KiB per-segment copy stays; it is the reason TASK-171
  option 2 was reverted, and changing it is a separate decision with its own gates).
- No change to the CLUT-load flush rule, the tone hook, `DropOnArrival`, or the writeback/GS-memory contract.
- No GPU-side change: the draws, shaders and present path are unchanged.
