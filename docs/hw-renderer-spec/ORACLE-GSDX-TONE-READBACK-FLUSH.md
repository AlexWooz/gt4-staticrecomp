# Oracle report: GSdx on GT4's tone strips, the flare readback and its flush rules

Written by a PCSX2-tainted oracle agent, prose only, decision 0070.

Date: 2026-11-05. Audience: the CoreGS 2.0 team (TASK-027). This report has no code and no pseudo-code. It names no
functions, variables or comments from the reference sources; the only names it uses are GS register and field names.

**Sources read** (read-only; nothing built or run):
- **Reference tree:** upstream PCSX2 at `aa7ab43` (2026-10-08), `private/tooling/pcsx2-slice101-source`: GS state and
  flush logic, CLUT unit, hardware renderer, texture cache, per-game hook table, Vulkan device and texture code, Vulkan
  pixel shader, configuration defaults, game database entries for SCUS-97328 and SCUS-97436.
- **Fork that builds gt4-gs.exe:** `C:\Antigravity\gt4recomp-gs`, upstream base `fd9d310` (2026-08-31). The parts that
  matter here are identical to the reference tree (C, compared): TEX0/CLUT flush, channel-shuffle detection, the GT4
  hook, the readback path and the download wait. The fork's own changes touch transport, presentation, settings and
  the readback service only.
- **CoreGS side:** TASK-168-PALETTE-SEGMENTS (168b worktree), STUDY-FRAME-DOWNLOAD-COST, CORE2-SPEC 7.2,
  CORE2-GPU-PLAN, Q-GSDX-COMPARISON; rc15 `core2_renderer.cpp`, `core2_gpu.cpp`, `hw_front_end.cpp`, `hw_clut.cpp`.
  STUDY-TONE-STRIPS-ONE-PASS.md does not exist yet (C).

**Labels:** **C** Confirmed (read in source), **H** High confidence, **Hyp** Hypothesis, **U** Unknown.

## 0. How GSdx runs GT4

- **GT4's database entry** (C; v2.00 and the Online Public Beta share it): half-pixel mode "native" (mode 4),
  auto-flush for sprites only, the Polyphony skip-draw hook (A3), and blending High as a recommendation only. It does
  not enable multi-batch draw buffering, GPU palette conversion, palettes read from GPU targets, CPU sprite or CLUT
  rendering, or effects at native scale.
- **gt4-gs as our launcher writes it** (C, docs/GS-SETTINGS.md): blending High, texture preloading Full, readback mode
  accurate in normal mode and "no GPU readbacks" in Performance Mode.
- **Reference defaults** (C): GPU palette conversion off; CPU spin and GPU spin during readbacks both off.

## A. GT4's tone copy-back strips

### A1. Does GSdx flush on every CLUT load? Yes, on every real load.

- **The load test (C).** A TEX0 write whose CLD asks for a load is compared with the last load: same CBP, CPSM, CSM,
  CSA (and TEXCLUT for CSM2), same palette size, and no write to the palette's blocks since. Uploads and local moves
  over those blocks, and a pending batch whose frame covers them with the CLUT's bit depth, mark the CLUT stale. A
  redundant load is dropped completely: no flush, no copy. CoreGS's 07.2 redundancy filter is the same rule (C,
  `hw_clut.cpp`).
- **A real load (C)** flushes the pending batch when it is textured with an indexed PSM (or made the CLUT stale), then
  copies the entries (1 KiB for T8 over CT32) from local memory into the CLUT buffer at the TEX0 write. The expansion
  to 32-bit colours waits until a drawn batch needs it, keyed by CBP, CPSM, CSA and TEXA.
- **No render-target check during a load** (C): the hardware renderer always takes the palette from local memory (a
  GPU-target mode exists, off for GT4). A fidelity shortcut, not a model for CoreGS.
- **Same flush count as CoreGS** (H): same rule, same stream, so GSdx flushes once per strip sprite too, about 1,000
  times per VBlank in the player's view. (TASK-168 lists CBP 14208 for the strips, IN-383 one CBP per quadrant;
  either way the loads pass both filters, since CoreGS measures them as real.)

### A2. Palettes on the GPU, indexed per draw? One palette per draw, never per primitive.

- **One palette per GPU draw** (C): no per-primitive palette index, nothing like palette segments.
- **Texture decoded from local memory** (GT4's default, C): the indexed texture is expanded to RGBA on the CPU once per
  pair (texture content, palette content) and kept in a cross-frame cache keyed by TEX0, TEXA and the palette bytes.
  "Same texture, another palette" is a flush followed, after the first frame, by a cache hit: no decode, no upload.
- **Render target read as T8** (channel shuffle or palette conversion of a target, C): the target is sampled directly
  and the palette is a separate 256 x 1 texture, kept in a content-keyed map and uploaded once. A palette change binds
  another small texture (one descriptor push).

### A3. A special case? Yes: the per-game hook, already mirrored by CORE2-SPEC 7.2.

- **Where (C).** The hook runs on every flushed draw, as the first step of the hardware draw: before target lookup,
  texture lookup, palette read, vertex work or any GPU command.
- **Detection (C).** TME, PSM T8, sprite class. Rejected when the batch has at most two vertices and a box of at most
  8 x 8. Accepted when the box is at most 64 x 64 and the first sprite is exactly 8 wide or 2 tall (or a region wrap
  mode is set), or for a page-aligned strip at most 64 wide with TBW = 2 x FBW and the same test. Each copy-back strip
  (one 8 x 32 sprite per CLUT-ended batch) passes.
- **Run (C).** The first accepted draw whose TEX0 base resolves to an existing colour target starts a run and remembers
  its FBMSK (no target: the hook steps aside, normal path). Every later accepted draw with that FBMSK is dropped at
  once, whatever its CBP, TEX0 base, frame base or VBlank. The first draw that fails detection ends the run and is drawn
  normally; another FBMSK starts a new run.
- **What it draws instead, FBMSK not 0x00FFFFFF (C).** One quad over the source target's whole native rectangle, at that
  target's scale, reading the same target in place (one pixel per pixel, framebuffer fetch or one feedback barrier):
  R' = entry[R].red, G' = entry[G].green, B' = entry[B].blue of the palette captured at the run's first draw, RGB
  written, alpha kept. That palette is the CLUT buffer as the first draw's own load left it (the flush precedes the
  next load).
- **FBMSK 0x00FFFFFF variant (C):** three CT32 targets at block 0 and at one and two times an offset from half the
  display height (FBW 10), each receiving one channel through the palette into alpha. CoreGS refuses it (counted); fine.
- **The measured strips** (TBP0 14244, CT24 frame at FBP 210, FBMSK 0xFF000000) (H): inside a run started by an earlier
  shuffle draw with the same FBMSK, so GSdx drops each one at the hook, as ToneHook does (100% in TASK-168b).
- **GSdx's cost per dropped strip** (C list; Hyp size, well under a microsecond): a state swap, the vertex trace over two
  vertices, the hook's shape test, and the 1 KiB CLUT copy at the next load. No batch object, lookup, palette
  expansion or GPU command.
- **Where CoreGS differs** (C core2 code; Hyp effect). GPU level: identical (ToneHook is core2's first batch decision
  after the depth-clear rewrite). CPU level: CoreGS builds a full batch per CLUT-ended sprite before ToneHook drops it
  (register snapshot, vertex and primitive vectors, flush accounting, two timer scopes, the handler call). GSdx's
  pending batch is only its vertex buffer plus live registers. Most likely reason GSdx absorbs the 1,000 extra flushes
  per VBlank more cheaply.

### A4. Exact? No, on two counts.

- **GS memory (C):** no dropped draw writes local memory or a target; the temporary pages and the CT24 page at FBP 210
  never receive the GS's bytes, and a later texture read or readback of them sees older content.
- **The picture** (Hyp; U for GT4's real palettes): it equals the real sequence only if each channel's palette agrees
  with the first palette in that channel and the three-channel accumulation never saturates. GSdx checks neither. The
  lookup itself is exact for 8-bit values (C). CORE2-SPEC 7.2 already treats the hook as GSdx-equivalent, not GS-exact;
  the 1x memory-exact strip executor (IN-383) is a separate path (H).

## B. The per-frame flare readback (64 x 8 PSMCT32, local to host)

### B1. All GPU work, or only the region's dependencies? All of it, from a cold start.

- **The wait (C).** Overlapping pending draws are flushed; the read records a small copy, submits the current command
  buffer and sleeps on its fence. A fence covers all earlier work on the queue: a full drain.
- **No mid-frame submissions (C).** The Vulkan device submits only at present, at such reads, and when a stream buffer
  fills (32 MiB vertex, 16 MiB index, 8 MiB per uniform stream).
- **Consequence** (C mechanism, H effect): at the flare read the GPU starts this frame's work only then (unless the last
  frame's is still running), so the wait is the frame's whole GPU time up to the read, with no CPU/GPU overlap. After it,
  nothing is submitted until present: the GPU idles while the CPU records the rest (the bubble of CoreGS's O1; GSdx has
  no fix).
- **So** (H): CoreGS's submission every 64 batches is structurally ahead here. GSdx's lead in Hong Kong at 6x cannot come
  from the read mechanism; it comes from less GPU work before and after the read.

### B2. Which data, from which image?

- **Search (C):** overlapping targets, exact-base matches first, then loose overlaps, oldest to newest; it stops once an
  exact-base target covers the end of the read.
- **Rectangle per target (C):** the read rectangle in the target's layout (page-rounded when formats differ) ∩ the area
  drawn since that target's last read. A second read with no draw in between takes the whole drawn-since area.
- **Exact skips, no GPU work (C):** nothing drawn since the last read; every page read covered by a CPU upload newer than
  the draws; an upload of exactly the same rectangle since the last draw.
- **Scale (C):** from the upscaled target itself, no 1x shadow. At 1x a direct copy into a staging buffer; above 1x one
  nearest-sampled draw into a temporary of exactly the read size (one sample per native pixel, at its centre), then the
  copy.
- **Write-back (C):** only the channels the target holds as valid (a 24-bit target never overwrites alpha). Then the
  drawn-since area is cleared, or trimmed when the read covers a whole row or column band.

### B3. What the download modes change (C)

| Mode | Behaviour | Exact? |
|---|---|---|
| Accurate (default; our normal mode) | as in B1 and B2 | yes |
| Accurate, whole area | always the whole drawn-since area | yes |
| No GPU readbacks (our Performance Mode) | no download; local memory as it stands (uploads and older reads, never GPU-drawn pixels) | no |
| Unsynchronized | local memory without waiting for the GS thread (the fork: on the replayer's thread); stale, racy | no |
| Disabled | zeros | no |

Two optional latency settings, both off by default and in gt4-gs (C): spin the CPU on the fence instead of sleeping;
after a read, keep the GPU busy on a low-priority spin queue for 30 frames so its clocks do not drop between drains.

### B4. Cheap and still exact?

- Nothing beyond B2 (C). CoreGS already reduces only drawn ∩ the read rectangle, samples once per native pixel and skips
  when no drawn target lies over the table (STUDY-FRAME-DOWNLOAD-COST sections 2-4).
- The real difference is the GPU backlog ahead of the read, especially how cheaply GSdx runs the self-sampling flare
  chain just before it (D3) (H).

## C. Flush and batch-merging rules

### C1. Lazy, register-difference flushes: the same as CoreGS chapter 02.5

- **Dirty bits (C):** each register write recomputes a bit against the pending batch's state through a relevance mask;
  writing back the batch's value clears it. Checked at the next vertex kick (a non-drawing kick only if the primitive
  class or XYOFFSET changed).
- **Flush triggers (C):** PRIM class or any non-type PRIM bit (CTXT included: a context switch flushes); TEST, SCISSOR,
  XYOFFSET, SCANMSK, DTHE; DIMX while dithering; ALPHA/PABE with ABE; FOGCOL with FGE; FRAME, COLCLAMP, FBA if colour
  can be written; ZBUF if Z can be written; with TME, TEX0 (TBP0..TFX, plus CPSM/CSA for indexed PSM), TEX1, CLAMP,
  TEXA; MIPTBP1/2 with MXL > 0.
- **Never (C):** CBP, CSM, CLD (they only steer loads). TEXFLUSH only sets a flag.
- **Versus CoreGS (C):** the same rules as CORE2 02.5 and 02.6; no difference found that would change the flush count.

### C2. CLUT loads, texture changes, context switches (C)

CLUT loads as in A1; a TEX0 change under the mask flushes with TME; context switches flush through PRIM. No batch ever
spans textures or palettes (no atlas, no multi-texture or multi-palette draw).

### C3. Auto-flush, sprites only for GT4 (C)

- **Split condition:** before a new sprite, when the texture base (any reachable mip level) is the frame or Z base, the
  channels overlap, the draw is not a recognised shuffle, the sprite's texel rectangle meets what the batch drew, and
  either the sprite moves to another texture page than the previous primitive's last texel or TEXFLUSH is set (then
  at once). Same as CoreGS 02.6.
- **Cost:** each piece takes the whole draw path; on the GPU it stays cheap only because of D3.

### C4. Merging GSdx does not do for GT4

- **Draw buffering (C):** an optional mode keeps up to three pending batches so interleaved depth-tested blended
  triangles can rejoin their batch. Off by default, absent from GT4's entry, heuristic. Sprite merging is off too.
- **Consequence (H):** GSdx's flush count in the player's view is about CoreGS's renderer batches (about 2,300 per
  VBlank), its GPU draws about CoreGS's GPU batches (about 750). GSdx does not keep the draw count low; it keeps two
  costs low: a dropped draw, and the GPU cost of each real draw.

### C5. Draws dropped before any GPU work (C)

Z test NEVER; colour and depth both fully masked; recognised clears (target clears, load-op clears, local-memory
clears); continued split clears; the black point drawn at a reset. Which of these CoreGS drops too: U.

## D. Other GSdx cost savers relevant to CoreGS's hot spots

- **D1. Specialised pipelines, no uber shader (C).** The pixel shader is compiled per state key (about a hundred bits:
  texture format and palette mode, channel fetch, wrap modes, alpha test, blend emulation, dithering, fog, depth
  feedback, ...), and a pipeline per key plus blend state, depth state, topology and render pass, compiled on first use
  and cached on disk. A draw runs only the arithmetic its state needs. The fixed-function side (attachments, hardware
  blend and depth, sampler fetches) is already in Q-GSDX-COMPARISON and CORE2-GPU-PLAN.
- **D2. Per-draw state churn (C).** Uniforms go into a stream buffer only when they differ from the last draw's (byte
  compare); one persistent set holds the uniform buffers, rebound only when a dynamic offset changes; textures and the
  palette are push descriptors, pushed only for the slots that changed; the pipeline is rebound only on change. Nothing
  is allocated or written into a descriptor set per draw. CoreGS today (C) skips redundant binds but allocates
  transient sets from a per-submission pool and writes them per compute dispatch; running out of sets forces a submit.
- **D3. Dependencies and barriers (C).** No global all-commands barrier between draws. A draw that reads the target it
  writes takes one of three paths:
  1. **Same pixel 1:1:** reads its own pixel through framebuffer fetch or an input attachment after one image barrier
     inside the render pass (one per group of non-overlapping primitives when primitives overlap).
  2. **Sampled rectangle disjoint from the drawn one:** samples the target directly (feedback-loop layout) after one
     image barrier; no copy.
  3. **Overlap:** copies only the sampled rectangle plus a one-texel border, at scale, into a pooled temporary.

  The barriers are scoped to the image and the stage pair (colour output to fragment shader; depth: fragment tests to
  fragment shader), framebuffer-local, inside the open pass. Sampling another target costs one layout transition of
  that image (which can end the pass). CoreGS: about 660 (George) and 683 (Hong Kong) full barriers per VBlank
  (STUDY-FRAME-DOWNLOAD-COST), all-commands to all-commands memory barriers; core2 already uses fragment barriers in a
  pass for draws with a texture from a target (C).
- **D4. Render passes and reuse (C).** One pass per run of draws to the same colour and depth pair; a clear becomes the
  load op; a pass ends only on a target switch, a copy, or a layout transition of a sampled image. Temporaries come
  from a pool keyed by usage, size, format and levels, reused across frames.

## What CoreGS 2.0 could do, behaviour-level (ranked by expected gain)

1. **Scope GPU dependencies the way D3 does.** For Hong Kong at 6x (GPU-bound, 21-27 ms in spike frames) and the backlog
   in front of the flare drain.
   - Use the narrowest barrier that covers each real hazard (that image or buffer range, that stage pair, by region in a
     pass where possible) instead of all-commands barriers.
   - Give self-sampling sprites (auto-flushed flare chain, box filter by halving, glare) D3's three paths.
   - First check which barrier reasons dominate: image, CLUT buffer, GLM pages, or overflow (the 512 pending limit).
   - **Exact:** every read-after-write and write-after-read pair on the touched region stays ordered; only unrelated
     work overlaps.
   - **Gain:** Hyp, several ms in frames with about 680 full barriers; U until `GT4_CORE2_BARRIER_TRACE` and
     `GT4_CORE2_GPU_PROFILE_AT` run on a Hong Kong spike VBlank and a Motegi sun VBlank. Also shortens the flare drain
     (study O5).
2. **Drop tone-run strips before a batch exists (case 1).** For GS-thread CPU and the scenes bound by it (George V at
   6x: 19.9 ms worker without the wait).
   - While a run is active, keep for the pending batch only ToneHook's inputs (primitive class, TME, PSM, first sprite
     size, vertex box, wrap modes, TBW and FBW, FBMSK). When it ends, apply the same detection; if ToneHook would skip
     it, discard it without snapshot, vertex conversion, translation or timers. Decode no strip palette beyond the CLUT
     buffer, as GSdx.
   - **Exact:** the same inputs as ToneHook's decision; a batch failing the light test goes the normal way; every side
     effect of the skipped flush stays (flush-reason and batch counters, last-scene frame record, transfer commits at
     the CLUT load, the load itself, auto-flush and TEXFLUSH state, the TASK-168b batch filter, tone counters).
   - **Gain:** Hyp, 0.5-2 ms per VBlank in the player's view (about 1,000 strips at 0.5-2 µs). Measure first with
     `GT4_CPU_SAMPLER`, splitting `gif_parse` and `batch_own` for strip batches.
3. **Specialise the hottest pixel pipelines further (D1):** opaque textured batches, decal sprites, palette sprites.
   **Exact** when each variant comes from the same source with constants folded. **Gain:** U, for fragment-bound frames
   at 6x. Pre-build the known variant set so no compile happens in play.
4. **Per-batch state churn (D2):** uniform writes only on change; one persistent set with dynamic offsets plus push
   descriptors for textures and palette rows; no transient set per compute dispatch. **Exact** (CPU bookkeeping only).
   **Gain:** Hyp, small to medium, CPU; also removes the submits forced when sets run out.
5. **Flare read: GSdx offers nothing better.** Keep the study's O1 (restart the GPU right after a drain) and O2 (bounded
   spin on the last fence), both exact. One new A/B for laptops only: after a drain, keep the GPU clocks up briefly
   (GSdx's optional GPU spin). Exact, costs power; Hyp, useful where the GPU drops clocks between drains.
6. **Not worth copying:** palette segments (GSdx has none, and the strips' GPU work is already gone in both renderers);
   multi-batch draw buffering (heuristic, off for GT4); non-accurate readback modes (not exact); GPU palette conversion
   (off for GT4); the alpha-only tone variant's fixed memory layout.

**Limits.** No measurements were made: every per-strip and per-barrier cost above is a hypothesis to measure. I opened
no GSdx log or dump, and gt4-gs.exe was not run.
