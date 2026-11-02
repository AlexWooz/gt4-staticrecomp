# CoreGS 2.0: performance parity plan (TASK-027)

Hand-over document for the next implementer of TASK-027. Written on 2026-10-23 by the previous writer (Claude) when
the owner moved the work to another agent. Read it together with `AGENTS.md`, `docs/hw-renderer-spec/PROGRESS.md`,
`docs/hw-renderer-spec/IMPLEMENTATION-NOTES.md` (IN-371 to IN-382) and the ai-memory page
`decisions/coregs2-implementation` (workspace `default`, project `kaezzey-gt4-staticrecomp`).

## 1. Goal and owner decisions

- **Goal (owner, 2026-10-23):** CoreGS 2.0 must reach at least GSdx's performance at the same internal scale. This
  is a requirement for the beta and for the launcher.
  - Owner reading at 6x on the Fuji Test Course start: gt4-gs at 37% GPU load, 1,357 MHz, about 41 W.
  - CoreGS 2.0 candidate 271 at 6x with AA: about 105-120 W at about 3,300 MHz.
- **Item 1, fast scaled raster: approved.** It may relax only the bit-exactness of the presentation-only scaled
  image, with an explicit tolerance. The 1x words, the GLM, memory, readbacks, and every 1x and memory gate stay
  exact and unchanged.
- **Item 2, tone-sequence HLE at 1x: approved.** It must be memory-exact.
- **Items 3 and 4:** do them if time allows.
- **Item 5, skipping the 1x redraw: rejected.**
- Already met (owner test 8): races render at real resolution at 6x with FXAA; the beta minimum (upscaling and AA)
  is met in races.

## 2. Current state

- **Branch:** `task/027-coregs2`, last green commit before the parity work **22f6dd7** (pushed): item 2 done.
  - Item 2, strip executor **done** (IN-383): `hw_tone_strips.comp` writes the eight strip batches of one 64 x 32 page
    in one dispatch (135,399 -> 16,940 on the 480p race capture), the strips' palette loads are held (GPU CLUT loads
    200,467 -> 98,827), all five memory captures are byte-identical and `hw_tone_hle_exact` passes.
  - Item 2's second step (the three temp batches per page) is **not** done; the plan's warning stands (pixel-centre
    rules and v steps are not worth guessing).
  - **Part A of item 1 (IN-384) is done**: scaled images sized to their slot, lazy and grown on demand. Scaled image
    device memory on the 480p race capture: 64.1 MB at 2x, 243.1 MB at 4x, 541.4 MB at 6x (was 184.5 / 738.2 /
    1,660.9). Gates unchanged (see IN-384).
  - The fast scaled raster itself (the early-Z second pass) is **not** implemented yet.
  - The build tree `O:\gt4recomp-scratch\coregs2-slice270-present\build` matches the branch HEAD; rebuild before use.
- **Candidate 274:** `O:\gt4recomp-scratch\coregs2-candidate-274`.
  - `gt4vulkan.exe` sha256 `0f02bc8bfe1ddeabad05cd9d8675120cb0d77f5bcc29798dcc55fb8f2286435d` (97,969,664 bytes; rebuilt with the scaled-texture origin fix).
  - `build\hwr-play-scale.ps1` defaults to it (`-Candidate`, run folder `private\hwr-play-274`).
  - Contents: IN-384 (scaled images sized to their slot) on top of candidate 273's IN-371..IN-383.
- **Candidate 273 (owner test 10, superseded):** `O:\gt4recomp-scratch\coregs2-candidate-273`, sha256
  `7c410e535e3a33b50cc71e411d44a9c98c188c1f51dee1a31a73eb0e306d28e0`.
- **Gates after IN-384 (slice274):**
  - CTest 245/245 with and without interlock.
  - Memory hashes identical on all five captures (night `fifo_hash=3a2fe617a174e7e7`).
  - Menu dumps 42/42 = slice228; race 42/42 = slice235; default renderer 42/42 = slice214.
  - Race `--check-presented`: 0 low-detail frames at 2x/4x/6x.
  - Scaled memory equal to 1x.

### Measurement (clean machine, 2026-10-23)

GPU ms per frame on the 480p race replay (`private/slice242-capture/race480p-10000.gscap`, `--gpu-timing`; the
timestamps add a little), with candidate 272's code:

| Scale | Busy | 1x pass | Scaled pass | Refresh | Decode | CLUT | Swizzle | Upload | Presentation compute |
|---|---|---|---|---|---|---|---|---|---|
| 1x | 7.1 | 3.4 | - | - | 1.65 | 0.53 | 1.03 | 0.49 | 0.01 |
| 2x | 8.7 | 3.3 | 1.5 | 0.12 | 1.54 | 0.53 | 0.98 | 0.49 | 0.26 |
| 4x | 11.9 | 3.5 | 4.2 | 0.35 | 1.53 | 0.52 | 0.96 | 0.49 | 0.43 |
| 6x | 17.1 | 3.7 | 8.5 | 0.72 | 1.52 | 0.57 | 0.89 | 0.50 | 0.67 |

CPU in the game (headless, launcher settings, last 600-VBlank interval):

| Measure | Value |
|---|---|
| GS thread | 10.6-11.3 ms per frame at every scale (budget 6 ms) |
| Process | 27-28 core-ms |
| Render passes per frame | 344 at 1x, 403 at scale |
| Barriers per frame | about 1,640 (race), 1,990 (night) |

Raw results: `O:\gt4recomp-scratch\coregs2-measure\{before,after-in382}\results.json`.

### Where the costs are

- **Scaled pass (the largest GPU cost: 8.5 ms at 6x, half the frame).** It runs the exact uber pixel shader
  (`native/hw_gs/shaders/hw_pixel_uber.frag` + `hw_pixel_core.glsl`) for each of the s x s samples.
  - Colour and depth are read-modify-written in R32_UINT storage images (raw GS words), under fragment-shader
    interlock.
  - Textures are fetched with `imageLoad`; bilinear means 4 loads.
  - Experiment (not committed): dropping the interlock saved only 0.5-1 ms of the 8 ms. The cost is the per-sample
    shading and the storage read-modify-write, not the ordering. Only real attachments (fixed-function blend,
    early depth, compression) will cut it.
- **1x work: 3.4 ms pass + 1.5 decode + 0.5 CLUT + 1.0 swizzle + 0.5 upload, about 7 ms, mostly GT4's tone
  sequence.**
  - The tone sequence is spec 09.4 variant A; IN-379 describes it.
  - About 1,540 of the 1,928 batches per race frame and about 280 of the ~320 runs belong to it.
- **GS-thread CPU (about 11 ms):** mostly the per-batch front end, translate and execute of the same tone
  sequence.

### What was started on items 1 and 2 (nothing committed)

- **Item 1:** only the interlock experiment above.
- **Item 2:** the full analysis of the tone sequence, recorded in section 4.

## 3. Item 1: fast scaled raster (approved, presentation-only tolerance)

**Design**

- Give every scaled colour slot image a second view: create `scaledFrames[i]` with
  `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and `COLOR_ATTACHMENT` usage, with an `R8G8B8A8_UNORM` view next to the
  `R32_UINT` storage view.
  - For CT32/CT24 the packed word layout (R in the low byte) is exactly RGBA8 in memory, so both views see the same
    pixels.
  - CT16/CT16S targets keep the exact path.
- Give every scaled depth slot a `D32_SFLOAT` twin attachment, with a per-slot flag saying which representation is
  newer.
  - Converting r32ui -> D32 is a full-screen draw writing `gl_FragDepth = word / 2^32`.
  - Converting D32 -> r32ui is a compute pass that reads the depth as a sampled image.
  - Convert lazily, only when the other path needs that slot.
  - `refresh_scaled` for depth writes the r32ui image, so it marks it newer.
- **Fast pipeline:** a new simple fragment shader (colour from vertex/texture/fog with TFX, alpha test only as
  discard when AFAIL = KEEP) plus fixed-function blend and depth.
  - Use a hardware sampler on an RGBA8 view of the texel atlas, so bilinear is free.
  - Cache pipelines by (blend equation, depth test/write, colour write mask, topology).
- **Classification per draw** (CPU, in `hw_gs_writeback_gpu.cpp` where `ScaledDraw` entries are built):
  - Fast when all of these hold:
    - the frame format is CT32/CT24;
    - the blend maps to fixed function: Cs/Cd/0 with factors As, Ad (CT32), FIX as a constant, the common modes
      (Cs-Cd)*As+Cd, (Cs-0)*As+Cd, (Cs-0)*FIX+Cd, opaque;
    - no DATE, no PABE;
    - FBMSK is byte-aligned (use the colour write mask), or 0;
    - COLCLAMP = 1;
    - the depth format is Z24/Z32 or depth is unused;
    - AFAIL is KEEP, or alpha test is off.
  - Exact otherwise: ATE with FB_ONLY/ZB_ONLY/RGB_ONLY, DATE, PABE, wrap colour, Z16, FBMSK not byte-aligned,
    CT16 targets.
  - Dither: ignore it on the fast path (it is inside the tolerance).
- Keep `record_scaled_draws` grouping. Draws of one run that switch between fast and exact need a pass break and a
  representation sync.

**Files and functions**

- `native/hw_gs/writeback/hw_gs_writeback_gpu.cpp`:
  - `make_image`, plus a mutable variant for the scaled colour images;
  - `create_scaled_resources` (descriptor sets, framebuffers, new render pass);
  - `record_scaled_draws` (path split);
  - the two places that build `ScaledDraw` (run path near `kTimeScaled` in `flush_run`, chunk path in `Execute`);
  - `refresh_scaled`.
- `native/hw_gs/writeback/hw_gs_writeback_gpu.hpp`: stats `scaledDrawsFast` / `scaledDrawsExact`.
- New shaders in `native/hw_gs/shaders/` (register them in `CMakeLists.txt` and `hw_shader_blobs.hpp`).
- `native/hw_gs/hw_gs_stream_sink.cpp` and `writeback/hw_writeback_replay.cpp`: print the fast/exact split.

**Read first:** spec chapters 06 (upscaling, 06.3 native mode), 08 (blending), 03.4/03.5 (texturing); IN-371,
IN-372, IN-374, IN-376.

**A/B switch:** add `GT4_HW_SCALED_RASTER=fast|exact` (default fast after acceptance) and a replay flag
`--scaled-exact`.

**Tolerance rule.** Record it as a new IN note that names this relaxation and the owner approval of 2026-10-23.
Against the exact scaled path, on the same capture and scale:

- max per-channel error <= 8 on at least 99.9% of the presented pixels;
- presented-frame PSNR >= 40 dB;
- `--check-presented` still 0 low-detail frames on the race;
- the 06.3 fixtures (`hw_scale_exact`, `hw_scale_display_shadow`, `hw_scale_fullscreen_passes`,
  `hw_scale_interlaced_present`) keep running on the exact path, and get a fast-path variant with that tolerance;
- memory hashes and every 1x gate stay byte-identical. The fast path never touches 1x images.

**Fixtures to add:** `hw_scale_fast_raster` (alpha blend, additive, depth-tested overlapping triangles, a texture
with bilinear, at 2x and 4x: compare to the exact scaled path within the tolerance, memory identical), plus a
classification test that an exact-only state (DATE, PABE, ATE FB_ONLY) takes the exact path.

**Expected gain:** scaled pass 8.5 -> about 2-3 ms at 6x, about 4.2 -> 1-1.5 at 4x.

**Implementation order (for the next writer, 2026-10-23, after Part A).** Each step ends in a committable, gated state:

1. **Colour view only.** Add `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and `COLOR_ATTACHMENT` usage to the scaled colour
   images and keep an `R8G8B8A8_UNORM` view (`Image::rgbaView`); reuse it in `update_scaled_descriptors` when the image
   grows (recreate the view with the new image). Add a second render pass (one colour attachment, `LOAD`/`STORE`,
   `DONT_CARE` for depth) plus one framebuffer per colour slot, recreated on growth. Take the fast path only for draws
   with **no depth test and no depth write** (ZTE 0, ZMSK 1) and a mappable blend: that alone proves the attachment,
   the pipeline cache, the classification and the A/B switch, and covers GT4's HUD, the sky and the full-screen passes.
2. **Fast fragment shader.** `hw_scale_fast.frag` includes `hw_pixel_core.glsl`, computes the same source colour as
   `hw_pixel_uber.frag` (texture function, fog; skip dither inside the tolerance), applies the alpha test as
   `discard` (only AFAIL = KEEP is eligible) and writes `gl_FragColor`; fixed-function blend with the mapped factors,
   `pBlendConstants` from FIX (only FIX <= 0x80, i.e. <= 1.0). Colour write mask from a byte-aligned FBMSK.
3. **Depth twin.** Add a lazily created `D32_SFLOAT` image per depth slot sized like the slot, a second render pass
   (colour RGBA8 + depth D32) and 18 framebuffers. Conversion r32ui -> D32 is a full-screen triangle writing
   `gl_FragDepth = word / 2^32` (`hw_scale_depth_words.frag`), no colour attachment. Track a per-slot "twin valid"
   rectangle: empty after `refresh_scaled(depth)` (a load), after every exact scaled draw that tested or wrote depth,
   and after the tone carry; convert `union(dirty, chunk rect)` before the first fast draw that needs it. **Require
   Z32 for depth writes on the fast path** (a Z24 write merges with the upper byte of the stored word, which the fast
   path would have to read back); Z24 keeps the exact path.
4. **Mirror.** The fast fragment shader writes the depth word it computed into the r32ui storage image (binding 2,
   write only, no read, no interlock), so an exact draw after a fast one still compares the exact words. Add
   `stats.scaledDrawsFast/Exact` and print the share.
5. **Tolerance.** A new IN note names the owner approval (2026-10-23) and the measured limits: max per-channel error
   <= 8 on >= 99.9% of the presented pixels, presented PSNR >= 40 dB against the exact path, `--check-presented` still
   0 low-detail frames on the race, `hw_scale_exact` / `hw_scale_display_shadow` / `hw_scale_fullscreen_passes` /
   `hw_scale_interlaced_present` unchanged on the exact path, plus a fast-path variant of them within the tolerance.
   Fixture `hw_scale_fast_raster`: alpha blend, additive, depth-tested overlapping triangles and one bilinear texture
   at 2x and 4x, fast vs exact; and a classification control that DATE, PABE, ATE FB_ONLY, wrap colour, Z24 writes,
   CT16 targets and a non-byte-aligned FBMSK take the exact path.
6. **Switch.** `GT4_HW_SCALED_RASTER=fast|exact` (default `exact` until the tolerance is measured, then `fast`) and the
   replay flag `--scaled-exact`.

**Measure:** `--gpu-timing` replays at 1/2/4/6x (the `scaled` category), plus the game's `[hw] cost` and `[hw] gpu`
lines. Compare with gt4-gs only as a black box: power and GPU load, final frames only.

### Findings for the implementer (2026-10-23, before the work started)

> **Part A implemented (IN-384).** The 1024 x 1024 squares are gone: a slot image covers FBW * 64 x its loaded rows,
> grows on demand and is copied on growth, so the numbers below about "mutable-format view costs nothing but a D32 twin
> adds 150 MB per slot" are now much smaller (a 640 x 448 target is 41 MB at 6x, and its D32 twin would be 41 MB).
> The second render pass, the fast shader and the depth twin are still to be built.
>
> **What Part A changed for item 1 (read before starting it):**
> - A scaled image can be *replaced* at any time (`ensure_scaled_image`) after the GPU was waited on. Everything that
>   names its view must be rewritten there (`update_scaled_descriptors`): the scaled graphics sets of BOTH frame sets,
>   the upsample sets, the tone sets and the scaled compose sets. A set left on a destroyed view loses the device.
>   The item 1 framebuffers (colour RGBA8 view + D32 twin) have the same problem and must be recreated on growth.
> - A colour slot's scaled image is no longer 1024 x 1024: its extent is `gpu::ScaledExtent(slot)` (native pixels) and
>   a viewer must ask `ScaledCovers(slot, x1, y1)` before reading it. The compose already does.
> - The existing scaled pass uses the storage images with the *same absolute sub-pixel coordinates* the compose reads
>   (`x * s + floor(s/2)`), so the fast pass must render into the RGBA8 view of that same image with a viewport of
>   `extent * s` and the same 06.3 offset; nothing is rebased.
> - A texture taken from a slot now copies only its sampled footprint (`GpuChunk::texCopyWidth/Height`), which is
>   enough evidence that a fast pass sampling the atlas can stay footprint-sized.

- **The scaled images are `kGpuScaledExtent * scale` squares**, i.e. 6144 x 6144 at 6x: about 151 MB each, and the
  drawer already allocates 6 colour + 3 depth + `scaledTexels` + 2 shadows (about 1.6 GB at 6x). The mutable-format
  RGBA8 view therefore costs nothing extra, but a D32 twin per depth slot adds about 150 MB per slot at 6x: create it
  lazily and only for the slots the fast path uses.
- **The scaled pass has no attachments today**: `scaledFramebuffer` is created with `attachmentCount = 0` and the
  uber shader reads and writes the scaled images as storage images (bindings 2 and 3 of the slot descriptor set), so
  the fast path needs a *second* render pass (1 colour attachment + 1 depth attachment) and one framebuffer per
  (colour slot, depth slot) pair, 18 of them. `setLayout` can stay the layout of the fast fragment shader (uniform
  buffer, atlas sampler), so the fast pipeline needs its own small descriptor set layout.
- **Do not convert D32 back to r32ui.** The depth words are the only exact thing in the scaled image, and
  `uint -> float -> uint` loses mantissa bits. Instead let the fast fragment shader write the incoming depth word into
  the r32ui storage image (binding 2, write only, no read, no interlock) while the D32 attachment does the test and the
  early-Z rejection: the two stay in step because the fragment sequence per pixel is the same. The only conversion left
  is `r32ui -> D32` after `refresh_scaled(depth)` (a slot load) or an *exact* scaled depth draw, over a rectangle the
  caller knows (`slot.loaded` / the draw rect), tracked with a per-slot "D32 current" rectangle. A whole-slot
  conversion at 6x is about 19 M pixels; over the loaded rect of a race frame it is 0.3 M.
- **Cost model:** the scaled pass is 8.5 ms for about 10.3 M pixels (640 x 448 at 6x) or less at 1.2 Gfrag/s, so the
  win must come from early-Z (most of those fragments are hidden) and from the simpler shader, not from the blend unit
  alone: the interlock removal alone measured 0.5-1 ms.
- Alpha test can only be `discard` (AFAIL = KEEP); DATE and PABE must take the exact path, and a texture sampled with
  REGION_REPEAT/REGION_CLAMP or the 4-bit fix stays exact (a hardware sampler cannot express those).
- The classification has to know the *palette*: an indexed texture decoded through the GPU CLUT (IN-349/IN-370) is
  already expanded into the atlas, so the fast sampler can use it as long as the atlas holds RGBA8 texels.

## 4. Item 2: tone-sequence HLE at 1x (approved, memory-exact)

### The sequence (Confirmed on the race capture; replay `--trace-frame 10050` with all vertices)

Per CT32 page k (140 pages of the 640 x 448 frame at BP 6720, FBW 10) there are 11 batches.

**Three temp batches**, all with:

- FRAME: BP 13600, FBW 10, CT32, FBMSK 0xFF000000 (a 64 x 32 temporary page);
- TEX0: TBP0 = 6720 + 32k, TBW 2, T8, TW 7, TH 6, TCC 1, TFX modulate, CBP 13632 (the ramp CLUT), CPSM CT32, CSM 0,
  CSA 0, CLD 5;
- TEX1 nearest, TEST 0x31002, ZBUF masked;
- 16 sprites of 8 x 16 px (8 across, 2 rows), FST UV.

| Channel | Vertex colour | ABE | ALPHA | CLAMP | Sprite j UV (texels) |
|---|---|---|---|---|---|
| R | (128,0,0,0) | off | - | REPEAT | u = 16*(j/2) .. +8, v = j%2 .. +64 |
| B | (0,0,128,0) | on | 0x8000000068 (Cs + Cd, FIX 0x80) | REPEAT | u + 8 |
| G | (0,128,0,0) | on | 0x8000000068 | 0x3f73 (WMS region repeat, MINU 1015) | u + 4, v + 2 |

**Eight strip batches** i = 0..7, each one sprite of 8 x 32 px:

- FRAME: BP 6720, FBW 10, CT24, FBMSK 0xFF000000, rect x = page_x + 8i;
- TEX0: TBP0 13636, TBW 1, T8, TW 3, TH 5, TFX decal, CPSM CT32, CSM 0, CLD 5, with CBP = 13600 + {0, 4, 16, 20, 8,
  12, 24, 28}[i] (the 16 x 16 quadrants of the temporary page used as a CLUT);
- UV (0,0)-(8,32), one texel per pixel.

**Net effect:**

- Each frame pixel gets RGB' = (ramp[R].r, ramp[G].g, ramp[B].b), alpha unchanged (spec 09.4 variant A).
- The temporary page ends holding the last page's intermediate.
- The CLUT ends as loaded by the last strip.

### Design (exact by construction)

1. **Strip executor.** This removes 8 of the 11 batches per page; it is the safe first step.
   - A strip is a decal T8 sprite with one texel per pixel, so its exact effect is
     `dest(x, y).rgb = CLUT_i[index(x % 8, y)]`.
   - The index comes from the T8 texture at 13636; `CLUT_i` is the temporary page quadrant at CBP_i (CSM1 layout,
     as in `hw_glm_clut.comp`).
   - Defer the eight strips of page k and run one compute dispatch for them:
     - the dispatch reads the temporary page and the index texture from the GLM, and writes page k as CT24 with
       `glm_write`, which leaves alpha untouched;
     - flush it before the next temp R batch (which overwrites the temporary page) and before any other event
       (non-matching batch, upload, local-to-local move, readback, VBlank, compose, GS reset, auxiliary frame,
       VRAM clear);
     - before the dispatch, `CoherentForWrite(page span, nullptr, true)`; after it, mark the pages GLM-owned and
       call `MarkWritten`.
   - Leave the strips' CLUT loads to the normal path at first (exact). Later, suppress them and replay only the last
     one before any consumer.
2. **Temp batches.** Keep them on the normal path at first (exact). The generic sampling positions (pixel-centre
   rules, v steps of 4 texels selecting lanes) are not worth guessing.
   - If you HLE them later, use a deferral log: record each batch and each CLUT load in order (copies of `Batch`
     and `clut::Tex0Write`).
   - When a page step completes and matches the template, commit it with the kernel, keeping the last step's three
     temp batches to replay at the flush (they produce the exact final temporary page).
   - When the pattern breaks mid-step, replay the log in order through the normal path (exact).
3. **Matching.** Match on raw `Batch` state before `Translate`:
   - registers as listed above;
   - exact vertex arrays generated from the template;
   - DTHE off, FBA 0, PABE 0, FGE off.
   - Any difference means the normal path.
4. **Interaction with IN-379/IN-382.**
   - The HLE must run at 1x as well: today `TrackToneRun` returns early at scale 1, so decouple the HLE from the
     scale.
   - Flush the HLE inside `FinishToneRun` before the carry, while the scaled work is still paused.

**Files and functions**

- `native/hw_gs/writeback/hw_gs_writeback.cpp`:
  - `Renderer::Impl::OnBatch` (hook before `Translate`);
  - `TrackToneRun` / `FinishToneRun`;
  - `OnClutLoaded` (split into record and load);
  - `InvalidateDestination`, `CommitUpload`, `LocalToLocalMove`, `BeginLocalToHost`, `Renderer::Flush`,
    `ComposeFrameGpu`, `ResetGs`, `BeginAuxiliaryFrame`, `ClearVram` (flush points).
- A new GPU entry in `hw_gs_writeback_gpu.cpp/.hpp` and a new shader using `hw_glm_common.glsl`
  (`glm_read`, `glm_write`, `glm_slot_rows`).

**Read first:** spec 09 (09.1 reinterpretation maps, 09.3 channel shuffles, 09.4 GT4 patterns: policy 2 allows this
HLE), 07 (CLUT, CLD modes), 04.9 (GLM); IN-347..IN-351, IN-370, IN-379, IN-382.

**A/B switch:**

- `GT4_HW_TONE_HLE=0|1` and replay `--no-tone-hle`.
- `GT4_HW_VERIFY_SHUFFLE=1` (the option already exists in `hw_gs_options`): execute both the HLE and the batches,
  and compare the written pages (download, or a GPU compare) and the final CLUT; count mismatches.

**Fixtures to add:**

- `hw_tone_hle_exact`: a synthetic capture with two pages of the template, run with and without the HLE. Compare
  per-frame memory hashes and the CLUT buffer; it must fail if one strip quadrant offset is changed.
- Run the memory gates with the HLE on and off.

**Expected gain:**

- Strips alone: about 1,120 of the 1,540 tone batches per frame and most of the 280 runs. GS thread about 11 ->
  6-7 ms; GPU 1x pass, CLUT and barriers about -1.5 to -2 ms.
- With the temp batches as well: GS thread about 4-5 ms.

**Measure:** the game `[hw] cost` lines (`gs_thread_cpu_ms`, `render_passes`, `barriers`) and the replay
`--gpu-timing` (`pass`, `decode`, `clut`).

## 5. Item 3: fewer render passes and barriers

- **Design:** after item 2, about 40 runs per frame remain.
  - Runs end because texture preparation writes the shared texel atlas (IN-352) and CLUT loads need barriers.
  - Use a ring of atlas regions, so consecutive runs that prepare different textures can stay in one render pass.
  - Use `full_barrier` elision (IN-368) more aggressively.
- **Files:** `hw_gs_writeback_gpu.cpp`: `flush_run`, the atlas allocation, `full_barrier*`.
- **Gain:** about 0.5-1 ms GPU and fewer CPU submissions.
- **Measure:** the `render_passes` and `barriers` counters plus `--gpu-timing`.

## 6. Item 4: cheaper refreshes

- **Design:** `refresh_scaled` (0.72 ms at 6x) refreshes every loaded rectangle.
  - Skip refreshing scaled images that no presentation, scaled texture copy or scaled draw will read before the
    next full overwrite. For example, a slot whose next use is a full-rectangle opaque draw, and depth slots with
    no scaled depth test before the next clear.
  - The fast path of item 1 can also refresh with a blit-like draw.
- **Gain:** about 0.3-0.5 ms at 6x.
- **Measure:** the `refresh` category.

## 7. Build, gates, timing

- **Build** (VS x64 env inside the script; the release recipe):

  ```
  build\slice248-coregs2\build.bat <build dir> aspect slice262-frame-cost-runtime slice262-license-guard-runtime
  ```

  - The tree is `O:\gt4recomp-scratch\coregs2-slice270-present\build`. Use a new folder for a fresh tree.
  - `build/` is git-ignored but present in the worktree.
- **Gates:**

  ```
  sh build/gates2/gates.sh <tag> '<build dir>' [noctest nohash nodefault nopresented]
  ```

  It runs:
  - CTest in both interlock modes;
  - memory hashes on five captures (menu, race, car, night, race480p);
  - `--check-presented` at 2/4/6x;
  - menu dumps vs slice228, race frames vs slice235, default renderer vs slice214.

  It refuses to launch the game while any game session runs, so retry later.
- **`--check-presented` (replay):**
  - It counts presented frames from 1x and low-detail frames: mixed s x s blocks below 0.25 x the frame's 1x edge
    pixels.
  - `--no-gpu-compose` is the A/B for 480i frames; `GT4_REPLAY_DETAIL_TRACE=1` prints it per frame.
- **Timing rule:** never measure while a game, the GS process or a build runs. Check
  `Get-Process gt4vulkan,gt4vulkan-spec2,gt4-gs,cmake,ninja,clang-cl,cargo,rustc,hw_writeback_replay` before and
  after each run, and repeat any run that overlapped.
  - Another agent (OpenCode, Spec II) builds and runs headless games on this machine for long stretches.
  - A reusable waiter is in the previous writer's scratchpad. Rewrite it: before and after each run, poll
    `Get-Process` and redo the run if anything appeared.
- **Gotchas:**
  - `--slm-hashes` WRITES the file.
  - Bash heredocs and `sed` eat backslashes and turn `\n` into newlines: write Python edit scripts with a file
    tool, or edit directly.
  - `near` is a Windows macro.
  - `ctest -R "a|b"` breaks through `ctest.bat` (cmd pipe): call `"C:/Program Files/CMake/bin/ctest.exe"`
    directly.
  - numpy and PIL are only in `C:\Python314`, not in the tooling venv.
  - Never rebuild a tree while a gate or a measurement uses it.

## 8. Clean room and working rules

- Never open:
  - PCSX2 sources, the `gt4recomp-gs` fork, ARMSX2;
  - `private/tooling/pcsx2-*`, `private/slice189-pcsx2-ref`, `O:\gt4recomp-scratch\slice265-*`;
  - gt4-gs dumps or logs.
  `docs/hw-renderer-spec` is the only design source; gt4-gs.exe final frames, power and GPU load are allowed as a
  black box.
- Never touch the owner's cards (`private/manual-play/card.ps2`) or the main checkout `C:\Antigravity\gt4-staticrecomp`.
- Headless only: the owner runs the visible tests.
- Single writer on `task/027-coregs2` (worktree `C:\Antigravity\gt4-wt-027-coregs2`).
- Commit as `git -c user.email=57041838+moesuito@users.noreply.github.com -c user.name="João Alano" commit`, push the
  branch. No PR, no merge into main, no pins, no launcher mode.
- Update PROGRESS, IMPLEMENTATION-NOTES (a new IN note per change), STATUS, TASKS, the journal and ai-memory with
  every step.

## 9. Owner test 11 (deliverable)

- **Status (2026-10-23, after IN-384):** Part A is in, items 2 is in, the fast scaled raster (the rest of item 1) is
  **not** implemented; section 3 has the implementation order.
- **Deliver:**
  - a new frozen candidate in a new folder `O:\gt4recomp-scratch\coregs2-candidate-NNN`, with its sha256;
  - `build\hwr-play-scale.ps1` pointed at it (`$Candidate` default and the run folder);
  - the measurement table after items 1 and 2 at 1/2/4/6x (GPU busy per category, GS thread, process, render passes,
    barriers) and the scaled image memory at 2/4/6x;
  - the remaining gap to gt4-gs.
- **Owner commands:**

  ```
  & "C:\Antigravity\gt4-wt-027-coregs2\build\hwr-play-scale.ps1" -Scale 1
  & "C:\Antigravity\gt4-wt-027-coregs2\build\hwr-play-scale.ps1" -Scale 6 -AA
  ```

  The candidate folder must hold `SDL3.dll` next to `gt4vulkan.exe` (the executable links SDL3 dynamically); the
  script copies it from the build tree when it is missing. Owner test 10 never ran for this reason.

  Compare with gt4-gs (GSdx) at 6x in the launcher on the same scene (Fuji Test Course or Nordschleife TT, 480p):
  GPU load, clock and power in RivaTuner, and the picture.

## 10. Open items from the scene sweep

- **Headless reproduction of the owner's GT Mode screens (2026-10-24).** The game accepts a scripted button sequence
  (`--press <vblank>:<button>:<duration>`), which is how the race schedules are driven. With a card that has GT4 save
  data (`private/slice113-video/widescreen-seed/card.ps2`, a copy, never the owner's card):

  ```
  gt4vulkan.exe <elf> --ntsc --disc <iso> --frames 9600 --memcard <copy> ^
    --press 700:start:30 --press 1200:start:30 --press 5000:triangle:20 --press 6000:triangle:20
  ```

  reaches the **GT Mode map with the "Do you really want to quit Gran Turismo Mode?" dialog** (frame 9600), i.e. the
  owner's artifact screens, deterministically and headless. With `GT4_HW_DUMP_SOURCE=upscaled` and `=vram` the same
  VBlank gives the 6x and the 1x image of the same frame, which is what the artifact measurements below compare.
  - **Measured on that frame:** the thin vertical line the owner reports **exists in the 1x image** (native column 481,
    contrast 15.2 levels against a 5.0 median) and so does the horizontal one (native row 409, 46.4) - they are 1-pixel
    features of the map content magnified 6x, not a scaled-pass artifact. The 3-level checkerboard on flat areas is
    **not** in the 1x image and is still ours to find (next: dump the slot's scaled image, the shadow copy and the
    composed frame of one VBlank; `ReadImageDebug` already reads a slot's scaled image, the shadow needs a case).

- **Owner test 11 comparison against GSdx (2026-10-24):** the owner sent side-by-side shots (GSdx 6x against ours) of
  the GT Mode map and of an "Info" dialog. Ours shows a fine grid over the flat background, a vertical line, and a
  ghosted second copy of the dialog text; GSdx is clean. Measured on a headless 6x menu dump (Hue's Home, 3840 x 2688):
  - The background gradient steps every **6 pixels horizontally** (the 1x texture magnified) and every **2 native rows
    vertically** (a 480i field has 224 rows, so woven rows come in pairs), a 6 x 12 cell grid. That is the 1x source
    content at 6x, not a fallback.
  - `GT4_HW_SCALED_FILTER=linear` (new, default `ps2`) forces bilinear on 1x-sampled textures in the scaled pass. It
    changes the picture only slightly (mean absolute difference 0.44) and does **not** remove the grid, so the grid is
    not the nearest texture filter: it is magnified 1x content plus the 480i row pairs. GSdx's own upscaling filter is
    what smooths it there.
  - The dialog ghost and the vertical line are still unexplained; the dialog is a 480i menu, so the IN-381 field offset
    (6,216 of 14,009 frames carried one in the owner's run) and the deinterlace history are the next suspects.
  - `build\hwr-play-scale.ps1 -Linear` exposes the new filter for an A/B by the owner. He tried it: **no visible change**
    (as measured).
  - **Sporadic 1x frames (owner, second run):** 184 of 22,518 VBlanks (0.8%) had no GPU compose (the first run: 64 of
    5,395), i.e. a single frame flashed at 1x. Instrumented: `Counters::gpuComposeSkips[4]` records the reason (0
    `GpuComposeApplies` declined, 1 zero output size, 2 an interlaced frame larger than the deinterlace images, 3 the
    compose or its flush failed) and the sink prints `[hw] compose_skips ...` when any is non-zero. A 4,200-frame
    headless menu run had none, so the next owner run is what identifies it.

- **Owner test 11, 6x on the Nordschleife (2026-10-24, candidate 274 `4e54ad04`):** the upscale reaches the screen
  (confirmed after the deinterlace-set fix) and the frame is stable at 60 FPS, but two **presentation-only artifacts**
  appear, both exactly **one native pixel wide** in the 6x image:
  - a thin vertical line in the sky at x = 2676 of 3840 (column break 20.0 against a 2.9 median in the dump
    `private/hwr-play-274/dumps-x6/frame_04200.ppm`);
  - a horizontal banding with a **6-row period**: the row-to-row difference of phase 5 is 4.48 against 2.3-2.6 for the
    other five phases.
  The dumps are genuinely 6x (21.7% of 6x6 blocks uniform), so this is not a fallback to 1x for a whole frame: it is
  a one-native-pixel error inside the scaled image. Candidate sources, in order of likelihood: the IN-379 tone carry
  and the IN-377 display shadow, which fall back to the 1x word per sub-pixel where their per-pixel check fails (a
  column or row where the check fails shows one native pixel of 1x content), and the IN-381 field offset for the 892
  frames that carried one. Next step: count the per-frame fallback sub-pixels of each source in the compose (a counter
  and a per-row/column histogram) and fix the one that matches the measured rows/columns. Memory, 1x words and
  readbacks are unaffected.
- **First bisection (2026-10-24, replay `race480p` at 6x, `GT4_HW_TONE_CARRY=0`, dumps in
  `O:\gt4recomp-scratch\coregs2-evidence\in384-carry\`):**
  - The 6-row banding **does** come from the tone carry: with it on the row-to-row differences per phase are
    [3.12, 3.09, 3.31, 3.13, 3.19, **4.81**] (phase 5 is 1.5x the others, the same shape as the owner's frame);
    with it off the frame collapses to 1x (phases 0.27-0.30) and phase 5 is the block boundary (9.41), i.e. there is
    no detail left to band.
  - The **vertical line is not from the carry**: a strong column break at x ~ 2700 appears with the carry on and off
    (71.4 and 73.5 against a 2.4 median). It is a real edge of the 1x picture that the compose maps to one native
    column, so the owner's "line that does not exist" is a 1x-granular feature of the source frame, not a scaled-image
    fallback. Next: reproduce it with a frame dump of the owner's scene and compare the same column against the 1x
    `vram` dump of the same frame.
  - Switch: `GT4_HW_TONE_CARRY=0` (diagnostic only; the race then shows at 1x, so it is a bisection tool).
  - **Correction (same day):** the row-phase metric above also rises inside a block wherever the scaled image has real
    sub-pixel detail (only 18-24% of 6x6 blocks are uniform), so it does not prove the carry causes the banding. A
    per-native-pixel version of the carry's decision was written and measured: the phases did not move
    ([3.11, 3.09, 3.29, 3.14, 3.21, 4.76] against [3.12, 3.09, 3.31, 3.13, 3.19, 4.81]), so it was reverted rather than
    kept as an unverified behaviour change. **The artifacts are still undiagnosed**: the next step is the per-source
    fallback counter in the compose (how many sub-pixels of each circuit came from the 1x words, with a per-row and
    per-column histogram), which is the only measurement that can separate a real band from ordinary detail.
- **Second owner report (2026-10-24, candidate 274, 6x):** 1x is exact and 0 faults in every run; the artifacts are
  only at scale. Five scenes, each pointing at a different mechanism, so the counter must separate them:
  - **GT Mode map (480i menu):** thin white *dashed* lines along the map's road edges that do not exist in the game,
    plus a slightly different texture. The map is a texture decoded from the GLM, i.e. a 1x layer inside the 6x frame
    (IN-374 limitation): the dashes are the 1x pixel grid showing through a smooth gradient.
  - **Nordschleife race:** the 3D is sharp and correct, but the car's rear window and hood show a rectangular
    lattice ("cortes que vazam"), and one full-screen region shows a **checkerboard of ~6-pixel cells** (a 1x layer
    blown up) — the same 1x-layer mechanism, this time on a texture taken from the GLM instead of a slot.
  - **Race with the tone curve:** the subtle "linha certa, linha preta" banding, still unexplained (the carry's
    per-block decision did not change the measured phases).
  So the artifacts come from at least two sources: **1x layers** (textures decoded from the GLM, which the scaled pass
  samples at 1x) and **per-sub-pixel fallbacks** (tone carry, display shadow). The counter below must report both, per
  circuit, with a per-row/column histogram, and each scene above is a test case for the fix.
- **Third measurement (2026-10-24, after the five scenes):**
  - **The 06.3 half-pixel convention is not the bug.** On the same replay frames the shipped native shift (shift = s)
    gives a box PSNR against the 1x presentation of 27.4-28.0 dB, while `--half-pixel off` (shift = 1, plain 6x
    tiling) gives 21.7-22.0 dB. The convention stays; a replay flag `--half-pixel native|normal|off` now exists for
    future comparisons.
  - **The scaled texture copy ignored the atlas origin** (`hw_pixel_uber.frag`, `fetch_texel_scaled`, added
    `tex_origin * s` while `Execute` places the copy at (0, 0) of `scaledTexels`). Every texture after the first in a
    run sampled the wrong region: fixed by dropping the origin term (the image holds exactly the sampled footprint
    since IN-384). CTest 245/245 in both interlock modes, box PSNR unchanged (27.97 dB on the same frame).
  - **The lattice on flat backgrounds is magnified dither**, not a fallback: at 6x each 1x pixel is a 6x6 block, so
    the game's per-pixel dither (one level between neighbours) becomes a visible grid on flat areas. GSdx magnifies
    the same dither. Recorded as expected behaviour, not a defect.
  - **Menus (480i) still show alternating-row detail** (even-row difference 0.06-0.16 against 1.5-2.9 on odd rows):
    that is what a 480i source looks like woven at scale (IN-381), and the map's "dashed" lines are thin field rows of
    the source. Not a regression of IN-384.

- **480i races** (without the forced 480p): presented at 1x. The display reads 448 rows of a 224-row field buffer,
  so no scaled image covers the read rectangle. Seen on the old captures `race-9000`, `car-6600` and `night-10000`.
- **Car-select and track-select menus:** low detail and combing on the rotating 3D car (cause not confirmed).
- **Menu fades** (about 30 frames per transition) stay 1x. Hypothesis: spec 09.4 variant B alpha stashes, not
  carried at scale.
- **The intro logo movie** stays 1x. Hypothesis: IPU video uploaded as 1x pixels, so 1x by nature.
- **Scenes only the owner can check:** rain, rally/dirt, replays, photo mode, split screen, the in-car mirror view;
  also menus at scale and owner test 9 on candidate 272.

- **The three-image split (IN-387, 2026-10-25).** `GT4_HW_DISPLAY_DUMP=<folder>` now writes, for the first compose of a
  run, every displayed circuit's source (the slot's scaled image or the shadow), the composed frame, the deinterlace's
  merge image (before the algorithm), the circuit's 1x words and every colour slot's scaled image and 1x words;
  `GT4_HW_DRAW_TRACE=<file>` with `GT4_HW_DRAW_TRACE_RECT=x0,y0,x1,y1` writes one line per batch that touches a probe
  rectangle with its target, texture path (1x atlas, GLM decode, scaled copy), texture slot, footprint, filter and size.
  Measured on the GT Mode map + Info dialog at 6x:
  - the display circuit is bp=0 bw=10 **PSM CT24**, read 0,0,639,223, served by a **resident slot** (not a shadow, not the
    GLM); the display buffer itself is written by ten **untextured** draws plus transfers;
  - the map background is a full-screen quad into a CT32 target with a **1024x1024 texture from the GLM at FST, nearest**;
    the menu layer above it is a quad into a CT24 target whose texture is a **scaled copy of a resident slot**;
  - the flat dialog grey is 138.33 in the 1x words and 137.3-137.7 in the slot's scaled image, in a cell of exactly one
    native pixel: the amplitude is **one 8-bit level**, i.e. a sampling artifact (Q-GSDX-COMPARISON 12.4), created by the
    6x pass and not present in the 1x content;
  - `GT4_HW_SCALED_FILTER=linear` changes the CT24 layer target (21.8% of its pixels, mean 0.27 levels) and
    `GT4_HW_SCALED_REFRESH=linear` changes nothing at all; the two slots the display reads and the composed frame are
    **byte-identical** in both cases. The displayed content therefore arrives through the scaled texture copy, which is a
    plain nearest image copy, and no sampler switch can reach it;
  - the present blit is already bilinear (after the IN-375 2x2 box chain), so the present filter is not the difference.
  The comparison team's section 14 answers what the reference does here: it samples the **scaled surface of a live
  target** (as we do), preloads a recreated target with a **bilinear** blit but patches a surviving target's dirty
  rectangle with **nearest**, and with FST it samples level 0 point at 6x exactly as at 1x. So the remaining difference is
  the **preload filter** (IN-385 kept nearest for the 06.7 box-PSNR gate), not the sampler and not the present stage.
  Open: the IN-387 dumps are from candidate 274, where the owner has not yet judged the IN-386 deinterlace, so the
  residual one-level cell may not be visible at all in a window; the next step is his verdict before changing a default.

- **The owner's thin white lines, located (2026-10-25, IN-387 follow-up).** Reproduced headless at 6x on the GT Mode map
  (frame 6600) and compared with the 1x presentation of the same VBlank (same settings, `GT4_FORCE_WIDESCREEN=1`):
  - the display circuit's scaled image carries a **three-output-row band** (exactly `scale / 2`) at a bright value (211
    against 186) at the bottom of one native pixel, and the same pixel's 1x word is 186 everywhere: the 1x presentation
    has no line at all there;
  - the band is full width except where the opaque dialog covers it, and it is present with the deinterlace **off** too,
    so it is raster coverage, not the deinterlacer and not the compose (the compose is a 1:1 read of the slot image, and
    the slot image carries the band while the 1x words do not);
  - the band's height and position are exactly the `h = scale / 2` half-pixel band of 06.3/IN-372: the sub-pixels
    `[N*s, N*s + h)` of native pixel N belong to native pixel N-1, so a quad edge that lands mid-native-pixel covers the
    **bottom half of one native pixel** at scale, while the 1x sample grid (one sample at the pixel centre) sees the pixel
    as uncovered.
  So the lines are **sub-native-pixel coverage at the edges of bright quads**: a faithful consequence of rasterising at
  6x with sub-pixel coverage, where the 1x raster quantises coverage per native pixel. The reference does not show them
  because its GT4 per-game entry selects the **native-alignment** half-pixel mode (Q-GSDX-COMPARISON 11.1: "the
  native-alignment half-pixel mode" is one of the four GT4 database items), i.e. its scaled coverage is quantised to the
  native sample grid. Next step: an A/B switch that snaps the scaled pass's vertex positions to the native pixel grid
  (the reference's mode), then re-measure the band and the 06.3 box PSNR before making it the default.

- **The sliver is not the half-pixel mode (measured 2026-10-25).** Three vertex mappings were compared on the same
  VBlank against the 1x presentation: the shipped 06.3 native mode (`shift = 0.5 * s`), `GT4_HW_HALF_PIXEL=off`
  (`shift = 0.5` output pixels, i.e. block-aligned edges) and the IN-388 snap (`floor(gpu / s) * s`). Box PSNR against
  the 1x image: 24.52 / 24.54 / 23.06 dB; the bright sliver row stays at 7-9 levels in **all three** (it moves by one row
  with the mapping). So the sliver is not a geometry/half-pixel artifact. It is a **point-sampled thin texture
  feature**: the map layer is a 1024 x 256 (and a 1024 x 1024) texture on a 640 x 224 screen, so one texel is 0.625
  native pixels; a one-texel bright line in that texture is aliased away by the 1x sample grid and resolved by the 6x
  one, which is the "broken white line" at scale. The reference hides it with its sampler and, when the texture is
  minified, with mipmaps (Q-GSDX-COMPARISON 14 question 3: mipmapping is active with MXL > 0 and a mipmap-range MMIN).
  Our `GT4_HW_MIPMAP` option is parsed but not implemented, so every texture samples level 0; and the displayed layer
  reaches the frame through nearest scaled texture copies, which no sampler switch reaches (IN-387). Next step:
  implement the minification filter (mip level selection, or at least bilinear minification for the sampled footprint)
  and make the scaled texture copy filter-aware, then re-measure the sliver row and the map's white lines.
