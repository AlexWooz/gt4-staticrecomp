# Oracle report: ARMSX2's GS on mobile GPUs, and what CoreGS 2.0 could take from it

Written by a PCSX2-tainted oracle agent, prose only, decision 0070.

Date: 2026-11-05. Audience: the CoreGS 2.0 team and the owner. This report has no code and no pseudo-code. It names
no functions, classes, files, variables or setting keys from the ARMSX2 or PCSX2 sources, and it copies no comments.
The only names it uses are GS register and field names, Khronos/Apple API names and product names.

**Source.** `C:\Antigravity\armsx2`. I added nothing: the `upstream` remote (github.com/ARMSX2/ARMSX2) was already
configured, so I only fetched it. The clone is now on a detached checkout of upstream master `1049db37fb` (2026-11-04).
Nothing was pushed, `gh repo fork` was not run, and our repositories were not touched. One unshipped branch is noted
in 1.13.

**Our side.** Read: STUDY-BARRIER-SCOPE (worktree 178), STUDY-ASYNC-READBACK, STUDY-FRAME-DOWNLOAD-COST,
CORE2-GPU-PLAN, CORE2-SPEC, and the brief's S23 numbers. STUDY-UNIFIED-MEMORY: only its numbers as quoted in
STUDY-ASYNC-READBACK.

**Labels:** **C** Confirmed (read in source or in a commit message that reports a measurement), **H** High confidence,
**Hyp** Hypothesis, **U** Unknown. Numbers quoted from ARMSX2 commit messages are their measurements, not ours.

## 0. The short answer

1. **ARMSX2's GS is the GSdx design plus tiler-aware policy layers** (C):
   - real colour and depth attachments in tile memory, hardware blending and depth test for most draws;
   - a per-device choice of how a draw reads the pixel it writes;
   - a driver-bug database keyed on driver, version and model;
   - a mobile game-database overlay;
   - a fork-specific readback mode.
   
   There is no exact, GS-memory-authoritative path in what ships. An experimental one exists on a branch (1.13).
2. **On the owner's S23 with the stock Qualcomm driver**, ARMSX2 reads the destination by copying (C, 1.2):
   - every draw that must read the pixel it writes ends the render pass, copies the drawn area of the target and
     starts a new pass;
   - nothing is ordered per primitive and nothing waits per primitive.
   
   With GT4 at Basic blending (the Android default) few draws need that read (H).
3. **GT4 runs on mobile with an asynchronous flare readback** (C). The mobile overlay sets, for every retail GT4 serial
   (SCUS-97328 and SCUS-97436 included), a fork-only download mode:
   - the EE is answered at once from a CPU shadow of GS memory;
   - the real GPU download lands one or more frames later.
   
   That is the one-frame-late flare our decision 0073 rejected for normal mode. Their comment guesses it is an
   auto-exposure measurement; our studies know it is the flare visibility table.
4. **CoreGS's phone profile points at fixed per-barrier cost, not fill** (H).
   - The numbers: 11.5 ms of GPU at both 1x and 2x, 750 fragment barriers, 75 full barriers and 20 passes per VBlank,
     no interlock.
   - On tilers, ARMSX2's own measurements price an in-pass barrier like a pipeline drain (about 0.1 ms each on a
     PowerVR, and "about what a per-draw copy does" on Turnip) and a render pass at 27-32 µs whatever its area.
   - The single largest exact lever is therefore to stop paying per-primitive barriers on the phone (section 4,
     item 1).
5. **Exact, cheap and ARMSX2-proven:** one present pass straight into the swapchain image, with pre-rotation (4, item 2).
6. **ARMSX2-proven but not exact (Performance Mode only, or an owner decision):**
   - hardware blending and depth for the draws the fixed-function units can express;
   - presenting the field picture directly in field mode at an integer scale of 2 or more;
   - a previous-frame flare table instead of zeros.

## 1. Where ARMSX2's GS differs from current GSdx, for tile-based GPUs

### 1.1 Device and driver model (C)

- **Two independent identities, resolved at device creation:**
  - the GPU: Adreno, Mali, PowerVR, Xclipse or Apple, with a model table per family;
  - the driver: Qualcomm, Arm, Imagination, Mesa Turnip, PanVK, ANGLE, or ARMSX2's own builds.
- **A driver-bug table** maps (API, vendor, driver, model range, version range) to named defects and named
  workarounds. Behaviour is chosen per driver, not per vendor.
- **Custom drivers are first-class.** ARMSX2 distributes its own Turnip builds and a Mali driver ("malisx2"). The builds
  carry a tag in the driver-info string; the tag unlocks behaviours measured only on those builds.
- **The owner's S23:** U which driver it runs. The stock Qualcomm driver is the default (the custom-driver setting is
  empty by default).

### 1.2 Reading the destination without interlock (C, the core of the fork)

**Three ways to serve a draw that reads the target it writes, fixed per device at creation:**

| Way | What happens | Cost and ordering |
|---|---|---|
| (a) Copy | End the render pass, copy the draw's area (plus the sampled area for an offset read) into a temporary, start a new pass, sample the temporary | Always correct for draws as a whole; wrong for primitives that overlap inside one draw (each sees the pre-draw copy) |
| (b) In-pass with the backend's own barriers | One barrier per draw, or one per group of non-overlapping primitives | |
| (c) In-pass, ordered by the hardware or driver, no barrier | In-tile read under rasterization-order attachment access (`VK_EXT/ARM_rasterization_order_attachment_access`, colour and depth flags on the subpass and the pipeline), GLES `GL_ARM_shader_framebuffer_fetch`, Metal programmable blending; or a declared attachment feedback loop on a driver build known to order it | No barrier |

**Who gets what:**
- **Qualcomm proprietary driver (any Adreno): copy.** The database marks its subpass self-read and its feedback-loop
  layout as broken, so texture barriers are off by default. A user override restores the in-tile path. The original
  evidence is text lost on Turnip with a texture pack (Adreno 650); the Qualcomm row predates it and its own evidence
  is not recorded.
- **Adreno 8xx on the Qualcomm driver:** the in-tile read is also denied above Basic blending (stale reads).
- **Turnip on Adreno 740:** the declared feedback loop with ARMSX2's own per-draw barriers kept. Per their notes
  (C, text):
  - on the stock driver the copy path renders some titles wrong there;
  - declaring the loop makes Turnip render that pass untiled.
- **ARMSX2's generation-2 Turnip on the 730 and up:** the declared loop with no barriers, the driver ordering it.
  - It costs a cache clean and an idle wait per reading draw.
  - The 722 keeps its barriers: run-to-run differences without them.
- **Mali:** the in-tile ordered read (rasterization-order access), except MediaTek parts (but one) and the G57, which
  return stale colour.
- **PowerVR on Imagination's driver:** an ordered single-sample input-attachment read without the extension.
- **Apple (Metal):** programmable blending, ordered, in tile, same pass, no copy.
- **Xclipse:** no working in-tile read.

**Keeping the pass open (C).** A pass that a reading draw opened in its feedback configuration stays open for following
non-reading draws, instead of being ended twice around each reader. This is limited to the devices where it was
measured (Mali fetch, Adreno declared loop, Apple barrier path).

**Where reads cost per draw, blending is lowered (C):**
- An opt-in per-title cap lowers blending accuracy on the copy path and on costly barrier paths.
- An optimisation that voluntarily added a destination read (to run a destination-alpha blend in hardware) is disabled
  where the read costs a pass break: one title went from 14 to 610 reading draws per frame with it.

**Adreno pitfalls they hit (C):**
- A feedback read of a target whose pass load op is CLEAR returns garbage: the clear is committed first.
- With load op DONT_CARE the read sees undefined tile memory: the load op is forced to LOAD.

### 1.3 Barrier scope (C)

- **Scope and count.** Barriers are image-scoped feedback barriers inside the pass: one per reading draw, or one per
  non-overlapping primitive group when the draw's primitives overlap and the read matters.
- **The ordered paths drop them**, except for two cases:
  - a draw that samples its target elsewhere than its own pixel keeps one barrier (on the in-tile path that read gets a
    copy instead, since a framebuffer-local barrier does not order other pixels);
  - a depth read through a texture keeps its barrier.
- **Their cost model for tilers:** a barrier is a cache flush plus a pipeline drain, priced on their devices as above.
- **External dependencies.** Each render pass now declares a real dependency on earlier work. The implicit one orders
  nothing, and two consecutive passes on one image with no layout change raced: the second pass's LOAD read before the
  first pass's store.

### 1.4 Render pass structure and load/store operations (C)

- **Load and store ops.**
  - The load op follows the target's state: cleared becomes CLEAR, invalidated becomes DONT_CARE, otherwise LOAD.
  - Colour and depth are always stored; stencil is never stored. Their depth target has a stencil aspect only for
    DATE; Adreno drops stencil where it hangs (Turnip below Mesa 26.2).
- **Restarts.** A restarted pass always loads what it drew. A DONT_CARE kept from the first opening had made a tiler
  store undefined tile memory over earlier work.
- **Coalescing.** An optional per-title scheduler holds back plain non-reading draws and emits them grouped per target:
  up to 4 runs, one pass per run. Any draw that could observe another run flushes. It is off for GT4.
- **Render area.** Sizing the pass to the draws' area was built and switched off everywhere. On an Apple M2 and an
  SD865 the area did not matter; the pass count did, at 27-32 µs per pass.
- **Mali:** depth and stencil load ops are kept equal (a Mali corruption).

### 1.5 Texture uploads and conversions (C; CPU-side mostly)

- **Hash cache:**
  - keys built from per-256-byte-block hashes kept current by dirty bits;
  - 800 to 1,600 entries under a 128 MB cap;
  - a miss uploads the expansion the hash pass already made.
- **Upload ring:** grows to 128 MiB instead of waiting for the GPU.
- **Stream rings:** per-driver choice of write-combined or cached memory (cached won only on Turnip and Adreno 610).
- **GPU palette decode:** a compute decode of paletted windows from a GPU copy of local memory was tried and reverted.
- **Defaults:** GPU palette conversion is off; there is no ASTC or BC transcoding of game textures (ASTC only for
  replacement packs on Metal).

### 1.6 Readbacks (C)

- **The asynchronous mode** (fork-only, added after the five upstream modes) works in three steps:
  1. The EE's local-to-host read is answered at once from a CPU shadow of GS memory guarded by a mutex.
  2. The GS thread records the real download into a staging texture and retires it at a later VSync. Up to a fixed
     number can be in flight; when full, a new one is dropped rather than waited for.
  3. The shadow is updated when the copy lands.
  
  It is not exact: data is one or more frames late. The overlay enables it only for GT4 and one other title. Their
  verification was a replay on an M2: blocking waits 6 to 2, frames byte-identical in the replay. They state that a
  replay cannot show the game's reaction, and that speed on Mali and Adreno was not measured.
- **Mid-frame submission "kick"**, exact: for frames near a recent synchronous readback, recorded work is submitted
  every 8 unsubmitted render passes, so a readback's fence wait finds the GPU already done. For one title on an SD865:
  about 8 ms per frame of stall before this. This is the same idea as our K rule, O1 in STUDY-FRAME-DOWNLOAD-COST.
- **Qualcomm-specific:** slow image-to-buffer copies and slow cached readback memory are recorded as defects; their
  workarounds are wired only for Mali today.

### 1.7 Deinterlace and post chain (C)

- **Field mode presented directly.** With deinterlace on Automatic, field mode (FFMD = 1, GT4's 640 x 224 menu fields)
  and an integer scale of 2 or more for every enabled circuit, the deinterlace pass is skipped. The merged field is
  presented with only the half-line offset, since each field render already holds as many device rows as the display.
  At 1x and at fractional scales the weave stays.
- **Other post changes:**
  - a comb-adaptive pass for movies in frame mode (opt-in, off);
  - the history-based deinterlacer is avoided on the Mali-G57 (stale history banks);
  - sharpening and shader chains are sized to the presentation size;
  - one native pixel is trimmed off each edge at 2x and above (on by default) to hide edge garbage.
- **Skipped frames.** A frame identical to the last is not presented (on by default). The blank frames before the first
  output are not presented either.

### 1.8 Present and pre-rotation (C)

- **One render pass on the acquired swapchain image:**
  - load op CLEAR (the black bars come from the clear, free on a tiler);
  - one draw of the output texture with the chosen filter;
  - the overlay; then present.
  
  There is no intermediate copy and no second queue.
- **Pre-rotation.** The swapchain takes the surface's current rotation as its pre-transform, with axes swapped for
  90/270, and the rotation is folded into the present draw. On a rotated panel without it, Android composed the game
  layer on the GPU: 16-17% of an Adreno handheld's GPU. A setting turns it off for one device that showed the image
  turned.
- **Format.** An 8-bit RGBA/BGRA sRGB-colour-space format is preferred: one compositor listed a 16-bit float format
  first, which doubled present bytes and could not be scanned out.
- **Present mode.** FIFO_RELAXED is preferred with vsync, mailbox or immediate without.
- **Surface size.** No fixed smaller surface (no setFixedSize, no ADPF hints): the swapchain is the panel size.

### 1.9 Pipeline cache and shader compilation (C)

- **Storage.** Shaders, pipeline keys and the Vulkan pipeline cache live in one in-RAM pack per game. A low-priority
  thread writes them about every 30 s or once a minute; nothing is written on the GS thread.
- **Precompile.** The keys used in the game's last 4 sessions are built on worker threads at boot: up to 2,048 pipelines
  (768 under 6 GB of RAM).
- **Misses** still compile synchronously on the GS thread; compiles over 20 ms are logged.
- **Specialisation.** Shaders are specialised per state key, as in GSdx; Metal uses function constants.

### 1.10 Threading (C)

- **The GS front end** runs on its own GS thread, as upstream.
- **An optional "back thread"** splits GIF parsing and draw building from texture cache, draw recording and present.
  It is Vulkan only, refused with unsynchronised downloads, off by default, and placed on a free big core.
- **Affinity.** Emulator threads are confined to the performance cluster by default; MTVU is on.
- **Auto-tune** is off by default. It can change scale, blending, download mode, the back thread, the IOP thread and a
  dynamic EE cycle rate.

### 1.11 Vendor paths, extensions and driver workarounds (C)

- **Adreno, Qualcomm driver:**
  - primitive restart and provoking vertex are broken; a reversed depth range;
  - colour write mask ignored with depth test on Adreno 5xx and older drivers (emulated);
  - dynamic rendering and imageless framebuffers broken on old driver versions;
  - push descriptors trusted (Qualcomm and Turnip only).
- **Turnip:**
  - the blend constant is sometimes applied as zero, history-dependent (the constant is sent through the second
    fragment output instead);
  - a D32S8 early/late-Z hang before Mesa 26.2;
  - the feedback-loop declaration is made per draw with the dynamic-state extension
    (`VK_EXT_attachment_feedback_loop_dynamic_state`), not by pipeline flag, which cost up to 2.8x;
  - after each pipeline bind the per-draw value must be set again on Mesa.
- **Mali:**
  - push descriptors crash (Arm driver), descriptor sets are used instead;
  - no dual-source blending, so dual-source draws are software-blended;
  - gl_FragDepth disables early-ZS, so a depth floor that cannot change anything is skipped;
  - the r44p1 driver has a broken self-read.
- **PowerVR:** the feedback-loop layout costs about 0.1 ms per barrier, so an input attachment is used instead.
- **Apple:**
  - a tiler profile that must not inherit Adreno or Mali workarounds;
  - memoryless textures;
  - programmable blending;
  - raster order groups exist but are used only when fetch is absent.
- **Exact blend-unit tricks** that remove target reads:
  - setting or clearing alpha bit 7 under FBMSK 0x7FFFFFFF with the colour logic op (OR, or AND-inverted) instead of a
    read: bit-exact for any overlap, at most two runs per draw;
  - giving a draw whose blend was dropped as the identity the previous draw's identical blend state, to avoid two
    pipeline switches: bit-exact.
- **An approximate trick:** a shadow-volume alpha counter done by the blend unit, off by one level at alpha 128.

### 1.12 OpenGL ES path (C)

- **Adreno defaults to Vulkan** (GL is the weaker driver there). Mali Valhall v10 and Bifrost default to GL.
- **Framebuffer fetch.** The ARM extension is preferred on Mali and PowerVR, because it orders overlapping primitives.
  The EXT extension is used elsewhere but is not trusted to order overlap, so overlapping draws are split per primitive
  group.
- **No barrier extension on a tiler:** one copy per draw, never per primitive group. Per primitive meant hundreds of tile
  resolves per draw: well under 1 fps on Mali.
- **Bandwidth.** Depth and stencil are invalidated before the present pass and at swap.
- **Depth read.** The ARM depth-stencil fetch is used where offered, since sampling depth is not coherent on GLES.

### 1.13 Not shipped: a tiler-native exact renderer (C, branch only)

- **What it is.** A branch (443 commits beyond master, last commit 2026-09-01) holds a "Tile" hardware renderer:
  - a GS-memory interval model at page granularity, with a software-renderer floor and a byte-identity gate against it;
  - an opaque native draw path.
- **How it compares.** It is the closest thing in ARMSX2 to CoreGS's memory-exact design. U how far it got. Master only
  mentions it in comments.

## 2. Fractional resolution scales

**How they are implemented (C).**
- The scale has always been a float in this code line, as in GSdx. ARMSX2's settings now offer quarter steps up to 3x,
  then 3.5x. Quarter steps were chosen so that 512 x 448 times the scale lands on whole pixels.
- **Ownership rule:** native pixel n owns device pixels [ceil(n·s), ceil((n+1)·s)). Target sizes are ceil(native·s). At
  1.5x native pixels alternate between owning 1 and 2 device columns.
- **Vertices** scale by s in floating point. GT4's half-pixel mode ("native", mode 4) offsets by half a native pixel,
  which at 1.5x is 0.75 of a device pixel. The first device row and column of a target can stay undrawn, so a fully
  overwritten target is cleared rather than invalidated in that mode.

**Pitfalls they found and fixed (C; the "upscaling audit"):**
- **Truncation.** Allocations and copies that truncated the scale: a 1.5x target allocated 1x wide and then given a
  1.5x copy. Move and copy offsets must ceil exactly like extents, or blocks land a device pixel off.
- **Box filters.** Downsampling a scaled target to 1x needs an integer factor. At fractional scales they fall back to a
  bilinear stretch, which is approximate, for the rescale to 1x and for 8-bit sources built from scaled targets.
- **Dither and scan mask.** Both must index by the native pixel that owns a device pixel, using the target's scale (not
  the texture's). Uneven ownership made a visible pattern; they rotate the 4x4 matrix's phase so wide cells take the
  quietest entries. That choice is cosmetic, not GS-exact.
- **Culling.** The per-primitive cull grid is exact only at 1, 2, 4 and 8x; fractional scales get none, so more
  zero-coverage primitives survive.
- **Points** are snapped to the pixel the GS lights, round to nearest, per point.
- **Deinterlace.** Field-direct presentation (1.7) needs an integer scale of 2 or more, so 2.5x keeps the weave in
  field-mode menus. That is one plausible part of the owner's 2.5x drop (Hyp).
- **Readback.** At fractional scale the nearest sample per native pixel comes from uneven blocks (H).

**For CoreGS (H).** CORE2-GPU-PLAN G3 counts 48 places that assume an integer scale (reductions, patches, carries).
Fractional scales are a large job:
- **What can stay exact:** the native rows and words.
- **What is approximate:** every downsample or reduction that assumes s x s blocks (the exactness of the 1x authority is
  untouched).

## 3. Defaults and speed hacks behind ARMSX2's GT4 speed (C unless marked)

| Item | ARMSX2 on Android/iOS | Exact? |
|---|---|---|
| Flare readback | Asynchronous mode for all retail GT4 serials (mobile overlay) | No: one or more frames late |
| Blending accuracy | Basic by default (GT4's entry only recommends High; gt4-gs on PC runs High) | No: hardware blend rounding, fewer destination reads |
| GT4 hooks | Same as PCSX2: auto-flush for sprites, half-pixel mode 4, the channel-shuffle replacement of the tone sequence | Same approximations as GSdx |
| Stock Adreno driver | Copy per reading draw | Correct per draw, not per overlapping primitive |
| Menus at integer ≥2x | Field picture presented without deinterlace | Picture change |
| Edges | One native pixel trimmed per edge at ≥2x | Picture change |
| Present | Duplicate frames skipped | Exact |
| Threads | Big-core affinity, MTVU on | Exact |
| Off by default | Frame generation (LSFG), spatial upscalers (FSR1, Snapdragon SGSR, MetalFX), RAISR texture upscaling, manual frameskip, EE cycle-rate changes | Each approximate when on |

**Fairness note for the owner (H).** The like-for-like comparison is ARMSX2 against CoreGS Performance Mode at the same
scale and scene:
- ARMSX2's default GT4 is neither exact nor at gt4-gs's blending level;
- CoreGS normal mode also pays the exact flare drain.

## 4. What CoreGS 2.0 could adopt, ranked (biggest likely GPU win first; easiest first among equals)

The gains are for the S23 at 1x-2x and are all Hyp; none was measured.

### 1. No per-primitive barriers on the phone: an ordered in-tile read of the destination

**Exact.**

- **What to do:** where the driver offers rasterization-order attachment access (`VK_EXT_` or `VK_ARM_rasterization_order_attachment_access`):
  - keep CoreGS's exact words in colour attachments (R32_UINT for colour, a second R32_UINT attachment for packed
    depth);
  - read them with subpassLoad in a subpass and pipeline flagged for rasterization order;
  - write the computed word.
  
  The ordering per sample is the API's guarantee, so overlapping primitives see each other with no barrier, and all
  traffic stays in tile memory.
- **Exactness:** the same arithmetic as the interlock path. Gate it with the existing frame and flare hashes on the
  phone.
- **Risks:**
  - U whether the S23's Qualcomm driver advertises the extension;
  - ARMSX2 marks that driver's in-pass self-read as broken (evidence unrecorded) and denies the read on Adreno 8xx above
    Basic blending, so the gates decide.
- **Gain:** the largest. Most of the time behind 750 barriers per VBlank, plus storage-image DRAM traffic, which also
  means less heat.
- **Fallbacks, all exact:**
  - put batches that read nothing through plain attachments with no barrier;
  - replace per-primitive barriers by one per group of non-overlapping primitives (GSdx's draw-list grouping);
  - take ARMSX2's copy-per-batch only for batches whose primitives cannot overlap.

### 2. One present pass into the swapchain image

**Exact.**

- **What to do:** clear load op, one draw doing scale and pre-rotation, the overlay, and nothing else. Drop the extra
  copy, the separate black clear and the second-queue copy, and prefer an 8-bit sRGB swapchain format.
- **Gain:** about 110 MB per frame of traffic (our figure) becomes about 18 MB for a 3088 x 1440 RGBA8 image. Hyp:
  1-3 ms of GPU, plus power. Pre-rotation also removes any GPU composition by SurfaceFlinger (16-17% on one ARMSX2
  device). U whether the S23's display hardware already rotates.
- **Effort:** easy.

### 3. Performance Mode: hardware blending and hardware depth for the batches the fixed-function units can express

**Approximation.** This is ARMSX2-at-Basic.

- **What it gives:**
  - removes destination reads (and their barriers or copies) for most blended batches;
  - lets early-Z and LRZ reject fragments, where today the depth test runs in the shader.
- **Gain:** large where item 1 is not available. Ranked third only because it is not exact.
- **Exactness:** low-bit rounding and Z quantisation differ from the GS.

### 4. Menus: present the field picture directly in field mode at an integer scale of 2 or more

**Picture change; owner decision.** This is ARMSX2's rule.

- **What it removes:** the weave and history passes in GT4's 480i menus. CoreGS's exact fusions (step 2b/2c) already cut
  42% on the desktop.
- **Picture:** sharper, with a half-line bob; not identical to today's adaptive output.

### 5. Flare in Performance Mode: the previous frame's table instead of zeros

**Approximation.** This is ARMSX2's asynchronous mode.

- **Cost:** the same as today's Performance Mode (no wait).
- **Picture:** flares stay visible, one frame late, instead of disappearing.
- **Normal mode:** keeps the exact one-slot deferred read of STUDY-ASYNC-READBACK.

### 6. Pipeline precompile per game

**Exact.**

- **What to do:** keep the keys of the last sessions, build them on worker threads at boot, and write caches off the
  render thread.
- **Gain:** stutter, not average fps.

### 7. Descriptor churn

**Exact.**

- **What to do:** use push descriptors on the Qualcomm driver and Turnip (ARMSX2 trusts them there; never on Arm's Mali
  driver) and persistent uniform sets (our R6).
- **Gain:** CPU, small to medium.

### 8. Fractional scales (1.25-1.75 and 2.25-2.75)

**Exact for the native words; approximate for every s x s reduction.**

- **Why:** they let the owner pick the scale the phone can hold.
- **Effort:** large (section 2).

### Not worth taking

- render-area narrowing: measured useless on tilers;
- the per-title pass coalescer: our pass count is already about 20;
- ARMSX2's custom drivers: not ours to ship;
- spatial upscalers and frame generation: possible later as Performance Mode options, not structural.

## 5. iOS: what to plan for

- **ARMSX2 uses Metal natively on iOS** (C): Metal is the default renderer there. MoltenVK is used only on macOS, and
  Honeykrisp on Asahi Linux.
- **Metal's native exact path** (C for ARMSX2's use; H for CoreGS's fit): programmable blending reads colour attachments
  in tile, in rasterization order, inside the same pass. It is the clean equivalent of item 1, ARMSX2 relies on it with
  no barriers and no copies, and CoreGS's integer words would sit in uint colour attachments (H).
- **Other Metal features** (C exists; Hyp useful):
  - raster order groups (A11 and later) are the interlock analogue for read-write textures, but off tile (more DRAM);
  - memoryless attachments suit scratch targets;
  - tile shading and imageblocks are unused by ARMSX2 and could hold a whole GS tile's work.
- **MoltenVK:** U whether it exposes rasterization-order attachment access or fragment-shader interlock on current
  versions. Measure before choosing it over a Metal backend.
- **Memory** (H, Apple platform rules): iOS kills apps over a per-app memory limit well below physical RAM.
  - ARMSX2 sizes its replacement cache from the available-memory query (C).
  - An increased-memory-limit entitlement exists for apps.
  - Plan explicit budgets for scaled targets at 3x and above.
- **JIT:** ARMSX2's iOS build depends on JIT entitlements (C). CoreGS's no-JIT design is an advantage on iOS (H).
- **Thermals:** as on Android. Tile-resident attachments, rather than storage images, are the main power lever (H).

**Limits.** Nothing was run. Every gain above is a hypothesis to measure on the S23 with CoreGS's own gates. Which
driver the owner's ARMSX2 used is unknown, and so is whether the Qualcomm driver on the S23 offers rasterization-order
access.
