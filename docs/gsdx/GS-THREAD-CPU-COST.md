# The GS thread's CPU cost: how GSdx/ARMSX2 spends it, and where our GS worker spends more

Companion to [`GSdx-PIPELINE-STUDY.md`](GSdx-PIPELINE-STUDY.md). That page answers "what is synchronous, what waits, what the
GPU does". This one answers "where does the GS thread's CPU go, why is it (or is it not) lighter than our GS worker, and what
can an MIT renderer adopt from it". Same rules: concepts and architecture only, no GPL code or constants copied, their file
paths only as evidence, and every claim labelled **C** (confirmed by the code cited or measured with its source), **H**, **Hyp**
or **Unknown**. No build, benchmark or game run was made for this page.

Written 2026-11-11, against `origin/main` `59f27aa1` (our code), the vendored ARMSX2 tree
`third_party/armsx2/pcsx2/GS`, and our own TASK-215 measurements on `origin/task/215-coregs2-perf`
(`docs/tasks/COREGS2-PIPELINE-SPLIT.md`, `docs/tasks/COREGS2-TASK215-SUMMARY.md`,
`docs/tasks/COREGS2-PER-RECORD-DESIGN.md`, `docs/tasks/COREGS2-PERF-BASELINE.md`).

## 0. The answer in one page

1. **On Vulkan their "GS thread" is two threads.** MTGS parses GIF and kicks vertices; a second GS **back** thread runs the
   draw path (texture cache, validation, command recording) from a ring of self-contained records. Present and merge stay on
   MTGS. (C: `GS/GSBackQueue.h`, `GS/Renderers/Common/GSBackThreadPolicy.h`, `GS/GS.cpp`.) On a device that cannot have the
   split (non-Vulkan, or downloads that read live memory) it is one thread again, and the pixels are the same (C, same file).
2. **Their per-vertex work is one fused, SIMD pass.** A packed tag whose layout they recognise is walked as a whole, the
   vertices are written in device format, the indices are written in the same pass, and the cull runs in the same pass with
   packed band outcodes. Nothing per vertex allocates, and no rectangle or record is built. (C: `GS/GSVertexKickKernel.h`,
   `GS/GSVertexKick.h`, the per-layout handlers in `GS/GSState.h`; their differential ctest is the exactness gate.)
3. **They have no CPU vertex translate stage.** The vertex buffer holds the guest's own 12.4 fixed-point words; the vertex
   shader converts. (C: `GS/Renderers/Common/GSVertex.h`, the TFX vertex shader in `Renderers/Vulkan/VKShaderCache.cpp` and
   the `shaders/` sources.) We convert every vertex to floats on the CPU in `TranslateBatch` (0.45 ms on the license, C).
4. **One command buffer per frame, submitted at present.** There is no per-N-batch submission. Extra submits happen only for
   a readback (submit + wait), a stream-buffer spill, a failed allocation, and the armed "readback kick". Per-draw state is a
   state vector with dirty flags: a draw whose state word is unchanged re-applies **nothing** (C:
   `GSDeviceVK::ApplyTFXState`, one load and a branch).
5. **But no measurement says their GS thread is lighter on the same CPU.** On the desktop, on the same recording and the same
   work, CoreGS 2.0 is 9.0% faster at 3x and 11.4% faster at 12x than the `gt4-gs.exe` reference (C, TASK-215 baseline), and
   the black-box run shows the game itself stalling 4.263 ms/frame waiting for the GS process to consume its stream
   (C for the number; H that the process boundary is part of its cause). Their per-draw validation (`Draw()` + `DrawPrims`)
   is heavier than ours by a wide margin in code terms (C: `GSRendererHW.cpp`), and their scene work per draw is the same.
6. **So the weak-device difference is not "their GS thread is lighter".** The measured, mechanical cause is the GPU side:
   our S23 Ultra run is 11.5 ms of GPU at both 1x and 2x with ~750 fragment barriers, 75 full barriers and 20 render passes
   per VBlank, per-pass/per-barrier overhead on a tiler (C, owner's log bundle in the private `TASKS.md` TASK-180), while
   their draws are attachment draws with one pass per run. Two CPU-side causes remain plausible and are worth attacking:
   the **two-thread split** (our own TASK-215 prices the overlap at 1.5-3 ms, H) and the **fused batch kick** (our baseline
   prices a batched strip decode at 0.5-1.0 ms on the license, Hyp; C that the current path is per-vertex).

## 1. Their GS thread's CPU, stage by stage

### 1.1 Parse and vertex kick (front)

- Four GIF paths (`GSState.h`: `GIFPath m_path[4]`), each with its own packet state (tag, descriptor index, remaining items,
  register list). Tags are dispatched by format (PACKED, REGLIST, IMAGE) and, for PACKED, by a **layout** key: a table of
  handlers, one per (layout, prim class, ADC) combination, that walks a whole tag's data in one call and kicks a **run** of
  vertices (`m_fpGIFPackedRegHandlersLayoutC`, `GSState.h`; the fused handlers and the kernel in
  `GSVertexKickKernel.h`). (C.)
- The kernel is **two-pass**: pass one decodes each vertex and stores side data (position extremes, a small metadata word);
  pass two writes vertices and u16 indices together, with `head/tail/next/itail` as locals, and folds the min/max trace into
  an accumulator as it emits. (C: `GSVertexKickKernel.h`'s header comment.)
- The **cull** is a scalar-outcode reformulation of their reference cull: packed band bounds instead of a bounding box build,
  fed by a per-buffer "kick mirror" that is refreshed when the scissor changes. (C: `GSVertexKick.h`.) A differential test
  replays a tag qword by qword and compares it with the fused handler (C: `tests/ctest/core/GS/gs_kick_kernel_tests.cpp`,
  named in the source).
- Register writes go through a 256-entry handler table (`m_fpGIFRegHandlers`) plus dedicated handlers for the AFL/A+D forms;
  the handlers that end a draw call `FlushBuffers`/`Flush` with a reason. (C: `GSState.h`, `GSState.cpp`.)

### 1.2 Flush decision and draw buffering

- Up to **3 draw buffers** (`MAX_DRAW_BUFFERS = 3`), each with its own vertex array, index array and an environment snapshot
  (the whole drawing environment, not just the six registers a draw reads) (C: `GSState.h`, `GSBackQueue.h`). A state change
  **pushes** a new buffer, carrying over the one or two vertices a strip, fan or line still needs, and the draws themselves
  happen later, when `FlushBuffers` walks the used buffers and calls the draw tail for each. (C: `GSState::PushBuffer`,
  `GSState::FlushBuffers`.)
- The effect is that a flush is **cheap and deferred**: it snapshots state and swaps a buffer, instead of immediately paying
  the whole draw. On the back-thread split the same call hands a pooled node to the ring and the *other* thread pays it.
  (C: `GSState::FlushPrim`, `AcquireDrawNode`.)
- Flush reasons are the GS state machine's: context change, CLUT change (only when the pending buffered draw is texture
  mapped **and** indexed, and only for the buffers that use it), the various upload-dirty reasons, transfers, autoflush,
  vertex-count overflow. (C: `GSState.cpp`, `GSFlushReason`, `ApplyTEX0`.) That is the same shape as our
  `DecideFlush`/`FlushReason`, and the CLUT condition is the same restriction our rule uses
  (`native/hw_gs/frontend/hw_front_end.cpp:376-390`).

### 1.3 The draw tail (back)

For each flushed buffer:

1. `GSVertexTrace::FindMinMax` over the referenced vertices: a SIMD kernel chosen by a function-pointer table indexed by
   prim class, IIP, TME, FST and colour mode; it produces min/max position, colour, fog, UV/Q and the constant masks, plus
   the filter/LOD decision and the alpha range. (C: `Renderers/Common/GSVertexTrace.cpp` and `GSVertexTraceFMM.cpp`.)
2. `GSRendererHW::Draw()`: the per-draw validation and routing. In order: a bad-frame guard, a degenerate-draw guard, channel
   shuffle detection and skipping, clear detection (several forms), CLUT-draw detection (CPU or GPU), texture shuffle
   detection, HLE hacks (`GSHwHack`), target lookup (`GSTextureCache::LookupTarget`), source lookup
   (`GSTextureCache::LookupSource`), and then `DrawPrims`. (C: `Renderers/HW/GSRendererHW.cpp`, `Draw()` from line 3427.)
3. `DrawPrims`: builds `m_conf` (a `GSHWDrawConfig` on the stack) from the draw state: blending emulation level, depth
   emulation, DATE/stencil, colclip, FBMSK handling, texture functions, dithering, ROV/feedback-loop choice, specialisation
   words, and the pipeline selector that keys the cached pipeline map (`PipelineSelector`, `m_tfx_pipelines`). (C: same
   file; `m_conf` is filled across tens of sites and handed to the device at one `RenderHW` call.)
4. `SetupIA`: topology from the prim class, the vertex and index pointers and counts (the index buffer was already built at
   kick time), a few exact adjustments for points/lines/sprites, and the VS expansion choice. (C: `GSRendererHW::SetupIA`,
   line 6067.)
5. `GSDevice::RenderHW(config)`: either hands the draw to the pass scheduler (deferrable draws are held and coalesced per
   target run) or calls the backend's `DoRenderHW` (C: `Renderers/Common/GSDevice.cpp:1273`, `GSPassScheduler.h`).

### 1.4 The device recording (per draw)

- **State vector with dirty flags**: `ApplyTFXState` returns immediately when `m_dirty_flags == 0`, i.e. when this draw wants
  exactly what the last one left bound; otherwise it writes only the dirty pieces (uniform block, textures, pipeline,
  viewport, scissor, blend constants, line width) (C: `Renderers/Vulkan/GSDeviceVK.cpp`, `ApplyTFXState` and the
  `DIRTY_FLAG_*` sets).
- **Stream buffers**: the vertex and index data are copied once per draw into persistent host-visible rings
  (`IASetVertexBuffer` / `UploadIndices`, C). Their comment records that on one mobile part the *memory type* of that ring,
  not the copy instruction, was what made "800 KiB of vertices cost a third of the GS thread", and that switching to a
  cached coherent type fixed it (C: their comment in `IASetVertexBuffer`). Our rings already choose `HOST_CACHED` on
  integrated GPUs (`native/hw_gs/core2/core2_gpu.cpp:2509`).
- **Descriptors**: push descriptors where the driver allows, otherwise per-frame descriptor sets from a pool, updated only
  when a texture binding changes (C: `GSDeviceVK.h` `UsePushDescriptors`, `PSSetShaderResource`).
- **One `vkCmdDrawIndexed` per draw**, with the vertex/index offsets taken from the stream rings (C:
  `DrawIndexedPrimitive`).

### 1.5 Command buffers and submission

- **One command buffer per frame** (plus an optional init buffer): `MoveToNextCommandBuffer` activates
  `command_buffers[1]` for the frame; present is appended to that buffer and the whole thing is submitted at `EndPresent`
  (C: `GSDeviceVK::ActivateCommandBuffer`, `EndPresent`, `SubmitCommandBuffer`). 3 buffers in the ring, 2 submissions in
  flight.
- **Extra submits** (not a cadence): a readback (`ExecuteCommandBufferForReadback` submits **and waits**), a stream-buffer
  spill or an allocation failure (`ExecuteCommandBufferAndRestartRenderPass`), and the armed readback **kick** (3-frame
  window after a readback, render-pass boundary, next buffer verifiably free) (C: `GSDeviceVK.cpp`).
- The consequence for CPU: no per-batch submission cost, no fence per batch, no ring-share bookkeeping per submit. The whole
  frame's recording is one linear pass into one buffer. Our design pays a submission every 32/64 batches, with a fence reset,
  a queue submit and a slot rotation each time (`native/hw_gs/core2/core2_gpu.cpp:2188` `submit`, `:3574` the cadence).

### 1.6 What the frame costs on the device outside the draws

Merge (display composition), the presenter, texture uploads, the hash cache, the readback, and the deferred-draw emission.
None of it is per-draw. (C.)

## 2. What they do not do per draw or per batch (and we do)

| Per-draw / per-batch work | GSdx/ARMSX2 | Ours | Evidence (theirs / ours) |
|---|---|---|---|
| CPU conversion of vertices to the device's float format | none: raw 12.4 words, the vertex shader converts | every vertex, in `TranslateBatch` | `GSVertex.h` + the VS / `writeback/hw_gs_writeback.cpp` `TranslateBatch` (`core2_renderer.cpp:2643` caller chain) |
| Per-primitive record (type, vertex span, coverage rectangle) | none: the index buffer *is* the record, built at kick | one `PrimRecord` with a rect per primitive (26.5k/VBlank on the license) | `GSVertexKickKernel.h` / `frontend/hw_batch.hpp` `PrimRecord`, `hw_front_end.cpp` `AppendPrimitive` |
| Per-draw command record (`GpuDraw`: topology, span, two scissors) | none: `m_conf` is a stack struct, one draw per flush | one `GpuDraw` per draw, 9,197/VBlank on the license, 40 bytes each | `SetupIA` / `writeback/hw_gs_writeback_gpu.hpp` `GpuDraw` |
| Draw grouping on the CPU | one draw per flushed buffer | primitives grouped into draws per batch (`drawSegments`, draw-set key) | `FlushPrim` / `TranslateBatch` |
| Per-vertex trace | SIMD `FindMinMax` over the same buffer, per draw | `ComputeVertexTrace` per batch (scalar loop over the batch) | `GSVertexTraceFMM.cpp` / `frontend/hw_batch.cpp` |
| Palette upload / lookup per batch | palette texture bound per draw, updated only when the CLUT changed | palette row computed and uploaded per distinct palette row, per batch | `GSTextureCache::Palette` / `core2_renderer.cpp:726-772` (IN-409) |
| State re-application between draws | none when the state word is unchanged (dirty flags) | binds skipped when unchanged (IN-458); push word written per draw | `ApplyTFXState` / `core2_gpu.cpp` `record_draw` |
| Submission / fence per N batches | none (one submit per frame) | one per 32/64 batches, slots 2-24 | `EndPresent` / `core2_gpu.cpp:211`, `:3574` |
| Per-batch heap growth | grown-and-reused buffers; a pool node per draw | capacity reused (IN-360, B4); no steady-state allocation | `GrowVertexBuffer`, `AcquireDrawNode` / `hw_front_end.cpp` emit, `ResetTranslation` |

Two things on that list are already handled on our side and must not be "fixed" again: allocation churn (IN-418/IN-360/B4)
and redundant binds (IN-458). Two are explicitly *small* in our profile: the draw recording is 0.94 ms for 9,197 draws
(about 102 ns per draw, C) and `Gpu::Draw` 0.96 ms inclusive on the license (C, baseline sampler), so "the per-draw recorder"
is not where our CPU goes.

## 3. Readbacks and present: the CPU cost, in one paragraph each

- **Synchronous readback (default).** The GS thread records a copy into a staging buffer and **submits and waits**
  (`ExecuteCommandBufferForReadback`); then it maps and unswizzles the bytes into GS memory, possibly after a format-convert
  pass. The CPU work is small (a copy of the rectangle); the cost is the **stall** on the fence for the work recorded before
  it, and it is why the readback-kick exists. (C: `GSTextureVK.cpp`, `GSDeviceVK.cpp`.)
- **Asynchronous readback.** The EE thread serves itself from a CPU shadow under a mutex (a few microseconds); the GS thread
  only queues the copy and never waits; the VSync retire does the unswizzle and the shadow write, and a stale snapshot is
  dropped by page generation. Per-frame CPU: one queue entry and one publish per completed download. (C:
  `GSTextureCache.cpp`, `GSState.cpp`.)
- **Present.** One acquire, a render pass with a single full-screen draw, one submit, one present, then a pre-acquire. No
  readback, no CPU image work (C: `GSDeviceVK::DoBeginPresent`, `EndPresent`). Our present is heavier but more decoupled
  (compose + resample passes, a second queue, a pacer thread); the CPU difference is small and already measured (IN-514..523).

## 4. Where our GS worker spends more, with our numbers

Our stage split (exclusive ms/VBlank, TASK-215 pipeline-split page unless marked):

| Scene | gif_parse | batch_own | draw | clut | swizzle | translate | texture | readback | GS total |
|---|---|---|---|---|---|---|---|---|---|
| license B-1 1x | **3.36** | 0.63 | 0.94 | 0.48 | 0.48 | 0.45 | 0.19 | 0.08 | 7.67 |
| night 3x | 1.75 | 0.80 | 1.33 | 0.16 | 0.82 | 0.55 | 0.29 | 0.30 | 6.70 |
| Opera 3x | 2.29 | 0.76 | 0.79 | 0.26 | 0.75 | 0.39 | 0.19 | 0.51 | 6.44 |
| George V rally 3x | 3.42 | 1.00 | 1.35 | 0.46 | 0.76 | 0.64 | 0.29 | 0.60 | 9.22 |

Counts on the license (C, `COREGS2-PER-RECORD-DESIGN.md` and the TASK-171 split): **1,930 batches, 9,197 draws, 26.5k
primitives, 1,553 CLUT loads, 145.6 guest uploads (3.6 MB swizzled), ~50 texture decodes per VBlank**. The ~500 real draw
batches cost ~5.7 us each, and the post-front-end total is **2.85 ms/VBlank** (`draw` 0.93 + `batch_own` 0.64 + `clut` 0.48
+ `swizzle` 0.49 + `translate` 0.45); the 1,430 CLUT-load flush batches cost about 0.1 us each and are not the cost (IN-554).
The figure sometimes quoted as "2.85 ms per draw" is therefore 2.85 ms **per VBlank** across the ~500 real draw batches, not
per draw: the 9,197 draws in that VBlank average 310 ns each of GS-worker time in those stages.

The sampler's inclusive/exclusive split on the same scene (C, baseline):

- inclusive: `Kick` 3.62 (of which `StripVertices` 2.48, `DecodeRegisterXyz` 1.20), `FlushBatch` 3.14 (`EmitBatch` 3.12,
  `OnBatch` 2.76, `Gpu::Draw` 0.96), `WriteRegister` 2.35 (`OnTex0Write` 2.26, `ProcessWrite` 1.82, the CLUT-load flush
  lambda 1.59), `ConsumeRegList` 2.06, `ImageQwords` 0.75;
- exclusive: `ShouldDraw` 0.445, `Kick` 0.360, the parser walk 0.266, `ConsumeRegList` 0.157, `AppendPrimitive` 0.126,
  `PixelRect::Empty` 0.114, `EmitBatch` 0.096, `StripVertices` 0.085, `MakeVertex` 0.077, plus 1.113 ms inside the Vulkan
  driver (15.1%), 0.358 of memcpy, 0.307 waiting on fences (`wait_slot`).

Read with the table above, that says:

1. **The GIF front end is the single largest item: 1.75-4.76 ms, 26-44% of the GS worker.** Within it, the per-vertex path
   (`StripVertices` + `DecodeRegisterXyz` + `MakeVertex` + `AppendPrimitive`) and the CLUT-load path (`OnTex0Write`, with
   the 1,430 flushes it forces) dominate. That is exactly the part GSdx fused into a batch kernel and moved to a second
   thread. (C for our costs, C for their structure.)
2. **The translate stage (0.39-1.12) exists only because we convert vertices on the CPU.** They do not have it. (C.)
3. **The per-primitive record and the per-draw record are our extra writes**: 26.5k `PrimRecord`s and 9,197 `GpuDraw`s per
   VBlank on the license, where they keep an index buffer and a stack config. For us their cost sits in `gif_parse` (the
   per-primitive rect and cull), `batch_own` and `draw` (the records and the grouping) (C that the writes exist; Hyp their
   size).
4. **The draw recorder itself is not a problem** (0.94 ms for 9,197 draws, C): our per-draw command recording is already in
   the same order of magnitude as a state-cached bind set.
5. **Our driver time (1.113 ms exclusive) and fence wait (0.307) look comparable to theirs** (H: their driver work per draw has
   the same shape, but we have no measurement of it). Not a lever.

## 5. Adoptable concepts, ranked (each with a bound and a gate)

All of these are prose/concept only. The bounds are ours, from the measured stages; the ranking is by expected gain per unit
of risk.

### 5.1 Split the front end from the draw side (their back thread is the existence proof)

- **Concept.** Front thread: GIF parse, vertex kick, flush decision, and a bounded ring of self-contained draw records (state
  snapshot + vertex/index spans). Back thread: texture cache, draw validation, command recording. Present and merge stay on
  the front thread, which drains the ring first. Records carry their own state snapshot so the consumer needs no live state
  machine; buffers are pooled and handed across the ring; the ring's backpressure is the only coupling.
- **Our path.** `FrontEnd` and `Renderer::OnBatch` both run on the GS worker (`native/hw_gs/hw_gs_stream_sink.cpp` `run`,
  `core2/core2_renderer.cpp:3918`). The sink already has a ring for the EE->GS direction; this would be a second ring inside
  the GS worker.
- **Bound.** TASK-215 already priced it: the smaller half is `gif_parse` 1.75-4.76, the rest 4.1-8.2, overlap **1.5-3 ms**
  (H), 3-5 days, license as the prototype. Their design is evidence that the record shape and the buffer pool work in
  practice, and it shows the refusal rules that keep the split correct (non-Vulkan, live-memory downloads).
- **Gate.** Faithful; frames byte-identical (the existing identity gates); the record path behind a switch with the
  single-thread path as the reference.

### 5.2 Batch the strip decode and kick (their fused kernel)

- **Concept.** Walk a recognised packed layout as a run: decode each vertex into a side table, cull with packed band
  outcodes, then write vertices and indices in one second pass, folding the trace min/max as you emit. No per-vertex call,
  no per-vertex record, no per-vertex rectangle.
- **Our path.** `GifParser::Transfer` already has a one-loop fast path for GT4's STQ/RGBA/XYZF2 strips
  (`frontend/hw_gif_parser.cpp`, IN-366), but it hands each vertex to `Kick` -> `MakeVertex` -> `ShouldDraw` ->
  `AppendPrimitive`. The baseline's own option 1 is exactly this: write the batch's vertex array directly, reserve once,
  decode in a tight loop.
- **Bound.** **0.5-1.0 ms** on the license (Hyp; C that the current path is per-vertex), the largest CPU item we can address
  without a new thread. Start with the scalar batched loop (no SIMD) and measure; SIMD only if the scalar version pays.
- **Gate.** Faithful: a differential fixture that replays a tag vertex by vertex and compares it with the batched decode
  (their ctest does this for their kernel; ours would be the same idea), plus frames identical.

### 5.3 Remove the CPU translate stage (their shader-side conversion)

- **Concept.** Keep the guest's 12.4 words in the vertex buffer and let the vertex shader convert position, fog/Z unpacking
  and the UV/Q form; the CPU only computes what the draw *decision* needs (the trace, the cull, the palette row).
- **Our path.** `TranslateBatch` (`writeback/hw_gs_writeback.cpp`) converts every vertex to `GpuVertex` floats.
- **Bound.** 0.2-0.4 ms of the `translate` stage per the per-record design note (Hyp); the full stage is 0.39-1.12 (C). It
  also shrinks the per-frame vertex write from 32 bytes to the guest's own words if we keep the raw form (Unknown whether
  that is a win, since our `GpuVertex` is already compact and carries the resolved UV).
- **Gate.** Faithful; the CPU reference keeps the translated path as the oracle.

### 5.4 Replace per-primitive records with an index buffer plus a per-batch trace

- **Concept.** The index buffer built at kick is the primitive record. The per-primitive rectangle exists only so each draw
  can carry its own scissor; when the scissor does not change, it is not needed. Their draw rect comes from the trace, and
  their per-primitive scissor work is zero.
- **Our path.** `PrimRecord{type, first, count, rect}` per primitive (`frontend/hw_batch.hpp`), and `GpuDraw` carries two
  scissors per draw (`writeback/hw_gs_writeback_gpu.hpp`).
- **Bound.** Part of `batch_own` 0.63-1.36 and of the 9,197 `GpuDraw` writes (Hyp; the draw recorder is 0.94 ms, so the
  ceiling is small on that side and larger on the record writes). Risky: per-primitive scissors are an exactness feature
  (IN-542 measured scissor flushes at 0.9-1.1 per VBlank on the night race, so the rule could compute the rect only while a
  scissor is live; XYOFFSET, which our vertices already carry, was 4.7-12.0 on the license).
- **Gate.** Faithful; front-end fixture comparing a batch with and without a mid-batch scissor change.

### 5.5 Keep (do not redo)

- State-vector caching (dirty flags) and the draw-set cache: we have both (`core2_gpu.cpp:3275` `drawSets`, IN-458).
- Grown-and-reused buffers: we have them (IN-360, B4).
- HOST_CACHED rings on integrated: we have it (`core2_gpu.cpp:2509`); on Android it is **unverified** (Unknown U4).
- Hash/identity caching: ours (IN-453 block identities) is ahead of their content hash (IN-413 measured hashing as too
  expensive for us).

### 5.6 The mobile-specific one: verify the stream memory type and the submit cadence on the S23

- Their comment says the memory type of the stream ring, not the copy, decided whether 800 KiB of vertices cost a third of
  the GS thread on one mobile part (C). Ours picks `HOST_CACHED` on integrated GPUs but the Android path has never been
  measured for it (Unknown).
- Their submits sleep in the Adreno driver and the governor lowers the clock (C, their comment); our own S23 measurement
  agrees that fewer submits helped (`GT4_CORE2_SUBMIT_EVERY=128`: +11.6% VBlanks/s, 32% fewer passes on the showroom, C,
  private `TASKS.md` TASK-182). Our **desktop** cadence (64 discrete / 32 integrated) is the wrong default for a phone, and
  the release branch's K rule is the right shape; main does not have it.

## 6. What this page does not claim

- **It does not claim their GS thread is lighter on the same CPU.** No measurement of their GS-thread CPU on our stream
  exists (Unknown U1). The desktop comparison points the other way: our whole renderer is 9.0%/11.4% faster at 3x/12x, and
  the black-box run's producer stall (4.263 ms/frame, C) says their pipeline did not keep up with the same stream on the
  same machine (H that the process boundary contributes).
- **It does not claim the two-thread split explains the S23 result.** The private Android build defaults to OpenGL, where
  the split is refused; if the owner's session used that default, the measured GSdx had no back thread at all (C for the
  default, Unknown for the session; U1 in the pipeline study).
- **It does not claim a CPU saving without a gate.** Every bound above is either measured (C, quoted) or labelled Hyp, and
  each carries the frame-identity gate that our pipeline already uses.

## 7. Unknowns

- **U1.** Their GS thread's CPU time on our recordings, in ms/VBlank. Would settle the premise. The ways to get it: a
  `gt4-gs.exe` replay read through PCSX2's own GS statistics, or a wall-clock A/B of their GS process on the same stream with
  the back thread on and off. Our `[gs-ipc]` lines measure only our client side.
- **U2.** The real cost of their `Draw()`/`DrawPrims` validation per draw on GT4's stream (C that the code is large; no
  measurement). If it is 2-4 us per draw, they pay 1-2 ms/frame on the license, i.e. as much as our whole post-front-end
  path.
- **U3.** How much of our `gif_parse` a batched kick removes in practice (0.5-1.0 Hyp from the baseline; the first scalar
  prototype answers it in a day).
- **U4.** Whether our Android rings actually land in a cached coherent memory type, and what the copy costs there.
- **U5.** Whether their `FindMinMax` SIMD trace is materially cheaper than our scalar `ComputeVertexTrace` per batch (ours is
  inside `batch_own` 0.63-1.36; their kernel is SIMD but over the same vertices).
- **U6.** The cost of their pass scheduler's flush checks per draw (a `References`/`ConflictsWithQueued` scan); it is per
  draw and bounded, but unmeasured by us.

## 8. Evidence

Their side: `GS/GSState.{h,cpp}` (GIF paths, handlers, draw buffering, flush, `FlushPrim`, `ApplyTEX0`, CLUT records),
`GS/GSBackQueue.h` (records, buffers, the draw node), `GS/GSVertexKick.h`, `GS/GSVertexKickKernel.h`,
`GS/Renderers/Common/GSVertexTrace.{h,cpp}`, `GSVertexTraceFMM.cpp`, `GSVertex.h`, `GS/Renderers/HW/GSRendererHW.{h,cpp}`
(`Draw`, `DrawPrims`, `SetupIA`), `GS/Renderers/Common/GSDevice.{h,cpp}` (`RenderHW`), `GSPassScheduler.h`,
`GS/Renderers/Vulkan/GSDeviceVK.{h,cpp}` (`ActivateCommandBuffer`, `IASetVertexBuffer`, `UploadIndices`, `ApplyTFXState`,
`DrawIndexedPrimitive`, `DoBeginPresent`, `EndPresent`), `GS/Renderers/Vulkan/VKStreamBuffer.cpp`, `GS/GSClut.{h,cpp}`,
`GS/Renderers/HW/GSTextureCache.{h,cpp}` (`LookupSource`, `Palette`, `ReadbackAll`, the async queue),
`GS/GSJobQueue.h`, `GS/GSPng.h`.

Ours: `native/hw_gs/frontend/hw_gif_parser.cpp`, `frontend/hw_front_end.cpp`, `frontend/hw_batch.{hpp,cpp}`,
`writeback/hw_gs_writeback.cpp` (`TranslateBatch`), `writeback/hw_gs_writeback_gpu.hpp` (`GpuVertex`, `GpuDraw`),
`core2/core2_renderer.cpp` (`OnBatch`, `ResetTranslation`, `ProcessBatch`, `Readback`), `core2/core2_gpu.cpp` (`Slot`,
`submit`, `begin_recording`, `drawSets`, `record_draw`, ring memory types), `hw_gs_stream_sink.cpp`.

Numbers: `docs/tasks/COREGS2-PIPELINE-SPLIT.md` (scene matrix, waits), `docs/tasks/COREGS2-TASK215-SUMMARY.md` (2.85 ms,
500 real draw batches, options), `docs/tasks/COREGS2-PER-RECORD-DESIGN.md` (counts, per-stage bounds, risks),
`docs/tasks/COREGS2-PERF-BASELINE.md` (TASK-171 stage split, sampler, uploads/decodes), `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`
(IN-360, IN-366, IN-409, IN-413, IN-418, IN-452/453, IN-455, IN-457/458, IN-467, IN-485, IN-540..543, IN-554),
private `TASKS.md` TASK-180/TASK-182 (S23 numbers and the submit cadence).

Not done: no build, benchmark, game run, code/pin/test/data change, no PR, no release, no Telegram, no owner data.
