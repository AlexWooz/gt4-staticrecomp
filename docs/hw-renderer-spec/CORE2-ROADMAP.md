# CoreGS 2.0 (CORE2): roadmap after owner test 12 (2026-10-28)

Written by the TASK-027 implementer (clean room). It replaces the open-items part of `PARITY-PLAN.md` for core 2;
`PARITY-PLAN.md` stays the record of the old core.

## 1. Where we are

- **Owner test 12 (candidate 285, 1920 x 1080 window, 3x, 16:9): passed for picture faults.** "Zero bugs visuais
  agora nesse sentido": the ring notches, horizon stripes, jagged edges and random shadows are gone (IN-403).
- **Owner findings in the same test:**
  - stutter and frame-time spikes at the start of a race;
  - drops below 60 FPS in the middle of a race;
  - the GPU nearly idle (about 40 W at 3x);
  - the picture is clearly upscaled but not "crisp": no anti-aliasing;
  - 480i menus are not deinterlaced in core 2.

## 2. Measured state

Headless El Capitan race (`race-run.sh`, VBlanks 9600-10200, 3x) and the 20 s race capture. Stage timers come from
the `[core2] ms/vblank` line. Labels as in AGENTS.md.

### CPU (Confirmed)

The GS thread costs more than one frame, so the GPU waits on it. The whole emulator takes 21.6 ms per VBlank and
the GS work 20.2 ms, against 16.7 ms for 60 FPS.

| Stage | ms per VBlank | Notes |
|---|---|---|
| Front end (`batch`: GIF parse, vertex queue, `ShouldDraw`, batching) | 9.1 | ~2.8 MB of GIF per frame, about 19 ns per qword |
| Translate (vertices, pixel state, scissor) | 2.9 | ~425 draw batches per frame |
| Texture lookup and decode | 2.5 | ~240 GPU decodes and ~108 cache hits per frame |
| EE uploads | 2.5 | ~170 uploads per frame |
| Target lookup, creation, dirty rectangles | 1.9 | ~3 targets created and ~3 replaced per frame |
| Draw recording | 1.2 | |
| CLUT loads | 0.9 | ~2.8 per frame |
| Readbacks (synchronous) | 0.6 | |

- **Loading and menu scenes:**
  - In the interval with CLUT readbacks (VBlanks 7200-7800), CLUT loads take 7.6 ms and readbacks 7.55 ms.
  - Each readback submits and waits, so the CPU and the GPU stop overlapping.
- **The old core** took 15-16 ms for the whole emulator on the same race.

### Night race (owner run, candidate 287, Confirmed)

The owner's 3x session log (`private/hwr-play-274/session-x3-20261028-175612.log`, B-Spec night City course, the
first replay-style cameras with several cars facing the camera). The owner's overlay shows 35 FPS, GPU at 51% and
34 W. Per VBlank, from the 600-VBlank timing lines and the counter deltas over VBlanks 29400-30600:

| Measure | Value |
|---|---|
| Whole emulator (wall) | 22.7-23.6 ms (16.7-17.9 ms in the day scenes of the same session) |
| GS thread, GIF to submit (wall) | 16.1-18.2 ms, of which only 8.7-9.6 ms is CPU time: the rest is waiting |
| Readbacks (wall, inside the GIF time) | 7.7-7.9 ms |
| CLUT loads (wall, they include their readbacks) | 5.3-5.9 ms (0.2-1.2 ms in the day scenes) |
| Batching (front end, translate, targets, textures, draw recording) | 7.4-9.3 ms |
| Texture lookup and decode | 2.5-3.9 ms: 236 GPU decodes and 97 cache hits |
| Targets | 1.8-1.9 ms: 5.4 created and 5.4 replaced |
| Readbacks | 3.7: 2.9 CLUT loads over a drawn target, 0.03 indexed textures over a target, about 0.7 others (EE downloads) |
| Pixels read back | about 79,000 (about 21,000 per readback: the target's whole drawn area, not the CLUT's 16 x 16) |
| Submits, each followed by a full wait | 5.7 |
| Live image memory | 177 MB at the start of the session, 1,027 MB at the end: it grows, nothing bounds it |

The reading:
- **The night cost is not the game's own readbacks.**
  - The EE asks for a download about 0.2-0.7 times per VBlank (`[mtvu] sync download` 6,121 in 31,180 VSYNCs).
  - About 2.9 readbacks per VBlank come from **our** CLUT path: GT4 draws palettes into a target and then loads a CLUT
    from that target. Core 2 copies the target's whole drawn area to the CPU, so the CPU waits for the GPU each time.
  - Hypothesis, to confirm with `GT4_CORE2_READBACK_TRACE` on the night scene: this is the light and flare effect,
    which on the PS2 stays inside the GS. That would explain why the cost grows with the number of cars facing the
    camera.
- **The GPU is idle because the CPU waits on it about six times per frame,** and then it waits for the CPU. Neither
  side runs ahead.
- **The rest is CPU work:** batching (about 8 ms), texture decodes that the cache should have hit, and the target
  churn.

### GPU (Confirmed in code, cost Hypothesis)

- **One uber pipeline for every draw:** storage images with an ordered pixel interlock, and no hardware blending or
  depth test.
  - Cheap at 3x today, because the GPU waits on the CPU.
  - At 6x-12x, the interlock and per-pixel read-modify-write grow with the pixel count.
  - Mobile GPUs often lack the interlock. Measured 2026-10-30 (IN-466, `GT4_HW_INTERLOCK=0` on the RX 9060 XT): the
    3x night race needs 43.7 ms of GPU per VBlank without it against 3.9 ms with it (frames identical), so on such
    GPUs CoreGS 2.0 needs another fast path for the common draws (the hardware raster path D1, or CoreGS 3.0).
- **Every image has its own `vkAllocateMemory`:** each target and each pooled texture, zero-cleared at creation.
  - A race start creates dozens of targets and textures in a few frames.
  - That burst is the prime suspect for the start-of-race spikes (Hypothesis: the timing per frame is not
    measured yet).

### Picture (Confirmed in code)

- **Deinterlace:** not wired in core 2 (`core2_renderer.cpp`, "no deinterlace yet"). The adaptive output-row rule
  (IN-386, IN-396) exists in the shared display code and the old core's GPU kernel.
- **FXAA:** off in core 2 (`out.fxaa = false`). The kernel exists in the old core (IN-376).
- **Mipmapping:** not implemented. `GT4_HW_MIPMAP` is parsed but every texture samples level 0.
  - The reference samples mip levels when the game sets MXL > 0 (Q-GSDX-COMPARISON 14).
  - Distant textures therefore alias and shimmer.
- **Sharpness check against gt4-gs at 3x (Hypothesis):**
  - Measured on the race capture: our frames hold 2.4x more spectral power above the native Nyquist than gt4-gs's
    (0.0365 against 0.0153, mean of six frames).
  - The frames are not exactly matched, and gt4-gs's dump is resampled to 480 rows.
  - The reading: the softness the owner sees is not missing resolution. It is aliasing (no mipmaps, no AA) and the
    present resampling.
  - **Correction (owner test 13, IN-406, Confirmed):** these dumps are taken before the present. The softness came
    from the present itself: the halving rule of IN-390 reduced the 3x frame to 960 x 672 and stretched it back to
    1920 x 1080. Fixed in candidate 287. Aliasing (mipmaps, AA) is still open for the shimmer.
- **Present:** box halvings while the halved level still covers the window, then one bilinear reduction (IN-406). At
  3x in a 1080p window, it reduces 1344 rows to 1080 and keeps 1920 columns 1:1. There is no supersampling
  horizontally, so edges stay aliased.

### Quality options

- **The launcher's GSdx rows that players use** (`docs/launcher/gs-settings.toml`):
  - basic page: FXAA, CAS mode and sharpness;
  - advanced page: filter, trilinear, anisotropic, mipmap, dithering, TV shader, shade boost, integer scaling,
    linear present.
- **Core 2 has none of them yet.**

## 3. Test method

- **A fixed scene: the owner's saved race replay.**
  - The owner saves a race replay on a card; the agent works on a copy.
  - The same frames every run, with several B-Spec-style cameras.
- **Three uses of the replay:**
  - Headless game runs drive the menus to the replay with `--press`, then dump frames.
  - A `.gscap` capture of the replay gives the GS-only cost through `hw_writeback_replay`, with no EE noise.
  - gt4-gs replays the same capture for reference frames at the same scale (final images only).
- **Per-frame timing:** each frame's GS time and its stage split, plus max and p99, so a spike can be attributed to a
  stage and a frame. The game's F7/F8 report gives the same split in owner runs.
- **Picture metrics:** per frame on matched frames:
  - edge energy;
  - power above the native Nyquist;
  - a shimmer measure (frame-to-frame difference on static camera shots).

## 4. Plan

One writer at a time. Every step is one candidate, with CTest and the race480p memory gate, and an owner test at the
end of each phase.

### Phase A: quick picture fixes (done, IN-405, candidate 286)

- **A1. Deinterlace in core 2.**
  - The FFMD field offset is already applied. Run the adaptive output-row deinterlace (IN-386, IN-396) on the
    composed image, reusing the shared kernel and its history images.
  - Acceptance: the GT Mode ticker and the Info dialog are clean at 3x, as with the old core (IN-396 evidence).
- **A2. FXAA in core 2.** Port the IN-376 pass to the core 2 present, behind the same option.

### Phase B: deep performance pass (owner, 2026-10-28)

Revised after the night measurement above.

The ledger of every phase B step (kept and reverted, with numbers) is `CORE2-PERF-LEDGER.md`.

**Owner direction (2026-10-28): 16.7 ms is not the goal, it is the ceiling.**
- Cut the load as far as possible, the waits first.
- An unlocked frame rate is planned later, mostly through frame interpolation, so headroom matters.
- So does the mobile port.
- **The beta gate is the owner's laptop:** an Intel Core i5-7200U (2 cores, 4 threads) with Intel HD 620 (shared
  memory, about 34 GB/s), running at 2x (720p) at full speed. When that works, CoreGS 2.0 ships as a beta.
- This desktop (Ryzen 7 5700X, RX 9060 XT, PCIe 4.0) only hides the cost, so the code must be efficient from the
  start, not tuned later.

**Budgets that follow** (Hypothesis: they assume the laptop is about 1.7x slower per thread and has half the cores;
they get checked on the laptop):
- **This desktop, night B-Spec scene at 3x:**
  - GS thread at or under 3 ms CPU per VBlank;
  - zero full-queue waits in a frame with no EE download;
  - at most one wait per EE download, on that copy alone;
  - whole emulator at or under 10 ms.
- **The laptop at 2x:** the whole frame inside 16.7 ms, with the GS thread under 6 ms of one core.
- **Memory:** a fixed budget for images (shared memory on the laptop).
- **GPU:** the uber shader with the pixel interlock is the heavy path on a small GPU.
  - Before the beta, the hardware raster path (D1) must carry the common draws.
  - Whether the HD 620's Windows driver exposes the fragment-shader interlock is Unknown: check `vulkaninfo` on the
    laptop.

Order inside the phase: the waits first (B1), then the CPU work (B2-B5), every step measured on the night scene
against the budgets above.

The game's logic and every value the EE reads stay exact. The changes are inside the renderer unless a measurement
proves otherwise.

- **B0. Scene and timeline (first):**
  - **Test scene:** a headless night B-Spec City race from a copy of the owner's card (the replay is not available).
    The menus are driven with `--press`, and the agent finds the path itself.
  - **GS-only capture:** a 20-second `.gscap` of the same cameras, recorded through the gt4-gs path. It is replayed
    with `hw_writeback_replay` for GS-only, frame-exact timings, and through gt4-gs for reference frames.
  - **Per-frame timeline:** a nested stage tree with no double counting. Each readback is labelled with its reason
    (CLUT, EE download, texture over a target), its pixels, its CPU drain time and its GPU wait time. GPU timestamps
    per submit. Max and p99 per stage. The F8 report adds the EE, VU1 and MTVU split.
- **B1. Readbacks without the CPU** (night: 7.7 ms). **Done in candidate 288 (IN-407..IN-410)**: readback 6.9-8.7
  -> 0.8-1.6 ms per VBlank, the whole night race 20.8-26.4 -> 17.0-20.6 ms (headless, 3x). What remains is the EE's own
  flare download (about once per VBlank) and the GPU work before it.
  - **CLUT from a drawn target on the GPU.** A GPU pass writes the target's pixels into the GPU copy of GS memory
    (the GLM that textures are already decoded from). The CLUT load then reads it there (`hw_glm_clut.comp`), so the
    CPU no longer waits.
  - **The CPU copy of those blocks is marked stale.** It is read back only when a CPU reader needs it: an EE
    download, or a CPU-side texture key. The data is the same bit for bit; only the round trip goes.
  - **Indexed textures over a target:** the same path.
  - **The EE downloads that remain:**
    - read only the requested rectangle, never the target's whole drawn area;
    - copy into a persistently mapped buffer and wait on that copy's timeline value;
    - submit the frame's work in steady chunks, so that at a download the GPU has little left to do.
- **B2. Texture cache** (2.5-3.9 ms). **Measured 2026-10-28: not the bottleneck.**
  - A content-keyed cache cut decodes from 200 to 15 per VBlank but cost more CPU than it saved (journal; IN-412/413
    reverted).
  - Measured next (IN-414): the GLM page copies cost only 0.3-0.5 ms per VBlank. The CPU swizzle of EE uploads cost
    2.3-2.8 ms; page-row runs cut it to 0.9 ms, so the GPU upload path is not needed now.
  - IN-415 then removed most IN-411 store waits (1.7-2.2 -> 0.2-0.34 ms per night VBlank): GT4's streamed texture
    uploads replace whole pages under its night render, so the stored pixels were overwritten anyway.
  - Owner test of 291: 59.2 VBlanks/s on the night race (p99 19.6 ms). IN-416 then made dirty rectangles exact
    page by page and block by block for writes in the same layout, so the partial-write stores went too (store waits
    about 0 ms on a long night race).
  - IN-417 moved the CLUT loads over several or depth targets to the GPU (CLUT readbacks about 1 -> 0 per VBlank;
    GS thread 7.2-11.1 ms on the long night race).
  - Owner test of 293: 59.3 VBlanks/s, EE critical path 14.0 ms. B4 started with IN-418 (no allocation per batch,
    images reused): GS thread 5.5-8.9 ms per VBlank on the long night race; the CPU now outruns the GPU at some
    cameras.
  - GPU timestamps (IN-419): 6.6-9.1 ms of GPU time per night VBlank at 3x but 4.3-7.3 ms at 1x, a fixed cost per
    command (the owner saw the GPU at 45 W of 180 W). Barriers only where needed halved them; GPU time 3-13% less.
  - IN-420 deferred the draws into one render pass (4-8 per VBlank instead of 106-312; GPU time 5-13% less) and
    split the GPU time: drawing 3.8-4.5 ms at 3x (1.3-1.5 ms at 1x), compute and transfer a fixed 1.4-3.8 ms.
  - D1 measured first (IN-421): the whole fragment stage is only 0.4-0.7 ms of GPU time at 3x (empty-shader
    experiment); 53-65 M fragments per VBlank. The GPU time is the chain of small dependent commands.
  - IN-422 decodes textures over no target from a CPU page window (barriers down again, GPU time 3-10% less).
  - The owner's overlay (297) confirmed the GPU idles at a low clock (about 1650 of 3300 MHz, 29 W), as with GSdx: the
    GPU is not the limit on the desktop. The frame is the EE critical path (about 14 ms); the renderer owns about
    2.5 ms of it (the EE waiting for flare downloads). CPU/EE work goes to its own task (TASK-134).
  - Next for the renderer:
    - the flare-download wait;
    - the HD 620 laptop measurement, then D1 if that GPU is the limit there;
    - patches from a page window too, if the GPU side stays on top;
    - D1 stays a beta requirement (the HD 620 shades far slower), after the chain;
    - the flare download (0.4-1.2 readbacks, 0.7-2.1 ms per VBlank) if it stays on top;
    - translate and decode recording (B4);
    - the GS thread split in two (B5).
  - Original item, kept for the record:
  - Find out why 236 textures are decoded per VBlank against 97 hits: page versions bumped by uploads or dirty
    patches that do not touch the texture, or palette changes.
  - Key the cache to the content that really changed.
- **B3. Targets and image memory** (1.9 ms, race-start spikes):
  - reuse targets of the same size instead of 5.4 creations and 5.4 replacements per VBlank;
  - sub-allocate images from large device-memory blocks and clear them with a GPU command;
  - add a memory budget: live image memory grew from 177 MB to 1 GB in one session;
  - confirm that nothing compiles at run time.
- **B4. Front end and translate** (batching about 8 ms):
  - a tight PACKED/REGLIST loop for the common register sets (ST, RGBAQ, XYZ2, XYZF2);
  - the translated pixel state cached per register version;
  - no allocation or virtual call per vertex;
  - the bounding box and `ShouldDraw` computed once per primitive from integers.
- **B5. A second GS worker** (only if B1-B4 leave the GS thread over budget): parse and translate on one thread,
  record and submit on another, in stream order.
- **B6. Spec II as the stress scene** (owner proposal, after B1):
  - Spec II runs on this renderer since candidate 368 (IN-506).
  - George V Paris (the heaviest track; it does not reach full speed in GSdx at 6x) then becomes the 6x target.
  - The v2.00 game stays the fidelity reference.

**Outside phase B:**
- **VU1 on the GPU belongs to CoreGS 3.0 (TASK-090), not here.**
  - VU1 already runs on its own thread (MTVU), and the night cost sits in the GS thread.
  - In 3.0, the VU1 output would feed the GPU directly and skip the CPU's GIF parsing.
  - B0's F8 split decides whether VU1 matters before then.
- **Flares through occlusion queries** are also CoreGS 3.0. In 2.0, B1 keeps them exact and removes the wait.
- **EE single-thread work in the game code** is a separate task (owner, later).

### Phase C: picture quality

- **C1. Mipmapping and LOD** (fidelity, not an enhancement): MIPTBP1/2, the K/L LOD rule, MMIN/MMAG, MXL. Decode
  levels from GS memory like level 0, with trilinear when the guest asks for it.
  - Expected: less shimmer on the track and trees, the map's thin white lines gone (IN-388 analysis).
- **C2. Present resampling:** a sharper downscale (Catmull-Rom or Lanczos-2) for internal sizes above the window,
  bilinear only for small factors.
- **C3. Sharpening:** contrast-adaptive sharpening as an option, like the launcher's CAS row. Write it from AMD's
  published MIT FidelityFX CAS description or source, never from a GPL copy.
- **C4. Supersampling as the real AA:** once phases B and D make 4x-6x cheap, recommend an internal scale above the
  window. At 1080p, 6x is a 2x2 supersample.
- **C5. Texture filtering options:** forced bilinear and anisotropic filtering (owner choices, off by default).

### Phase D: GPU fast path (6x-12x, power parity, mobile)

- **D1. Hardware raster path:**
  - draws whose state needs no feedback render to real attachments, with fixed-function blending and depth;
  - the feedback rules are DATE, PABE, AFAIL other than KEEP, COLCLAMP 0 and partial FBMSK (Q-GSDX-COMPARISON);
  - the colour image gets an RGBA8 view of the same memory (mutable format, r32ui and rgba8 are compatible);
  - depth gets a D32F attachment, converted where the game reads Z as a texture or readback.
  - The interlocked uber shader stays for the rest.
- **D2. Measure against gt4-gs on the same capture and scale.** Per-frame GPU time, and the owner's power reading
  live.

### Phase E: completeness before the beta

- Core 2 at 1x (today only the HUD shows); 480i races without the forced 480p.
- Spec step 3:
  - T8 from a target (3.5) and the 16-from-32 shuffle (3.4);
  - depth as a texture (2.7, 3.6);
  - GPU local-to-local moves (4.3);
  - the alpha-only tone variant (7.2).
- Fractional scales and 12x (owner requirement): the launcher's percentage scale maps to a fractional internal scale.
- GSdx-compatible texture replacement (hash and naming in prose from the comparison team).
- A memory budget for targets and textures.
- Launcher entry as a beta renderer (needs upscaling and AA), the GSdx fallback kept.
- Later: the frame-skip study (TASK-082 branch) for CoreGS.

## Beta closing plan (2026-10-29, proposal to the owner)

The owner tested candidate 299: fidelity perfect, 60 FPS, frametime nearly flat. Phase B stops here; the rest of the
frametime variation waits for the EE/VU0/VU1 optimisation. After the beta: the 1.3.0-rc7 branch comes in (CoreGS 2.0
lands on it), the PR goes to the code optimisation branch, and the launcher entry is added there.

**Beta (must):**
1. **Core 2 at 1x:** done (IN-427, candidate 300).
2. **C1 mipmapping and LOD:** done (IN-428, candidate 301).
3. **Phase E spec step 3 audit:** T8 from a target, the 16-from-32 shuffle, depth as a texture, GPU local-to-local
   moves, the alpha-only tone variant. Find which paths GT4 still takes through CPU fallbacks or readbacks; fix the
   wrong ones, count the slow ones.
4. **The remaining spike sources:** camera cut reduced (IN-429, IN-430; 46 -> 40 ms once per race start, the rest
   is the game's own download waiting for real GPU work). Step 3 audit: in the race only the game's download remains.
   - the cut to the race camera (about 35 readbacks in one VBlank);
   - textures over 16-bit targets (a readback each);
   - the IN-411 stores in the owner's menus.
5. **A memory budget** (done, IN-431: 192 MiB of texture cache on integrated GPUs) for target and texture images: about 630 MB at 3x now, and the HD 620 laptop shares system
   memory.
6. **C3 FSR1 sharpening** (done, IN-432: RCAS, plus EASU for upscaling) (owner, 2026-10-29: FSR1 replaces CAS). Written from AMD's MIT FidelityFX FSR1 (EASU and
   RCAS), never a GPL copy.
7. **C2 present resampling** (done, IN-432: Lanczos-2 to the window rectangle): a sharper downscale when the internal size is above the window (supersampling).
8. **The laptop gate:**
   - the i5-7200U / HD 620 at 2x at full speed (owner F7/F8 over the laptop share);
   - D1 (hardware raster path) moves into the beta only if that GPU is the limit there.

**Beta status (2026-10-29 night, candidate 305):** items 1-7 done (IN-427..IN-434), 12x checked (IN-435). Item 8,
the laptop gate, waits for the owner. Decided to leave out of the beta (autonomous decision on the owner's "if it
makes sense"): fractional scales (the integer scale is in every image size, patch, reduction and shader: a large
refactor with a high risk of subtle faults, and the window resampling of IN-432 already gives an exact output size),
GSdx-compatible texture replacement (needs the hash and naming from the TASK-026 specification team), D1 (only if the
laptop's GPU is the limit). Frame skipping stays out (owner).

**After the beta, for release 2.0.0** (owner requirements of 2026-10-27; the owner decides if any moves into the
beta):
- fractional scales and 12x (the scale is an integer everywhere today: a large refactor);
- GSdx-compatible texture replacement and dumping (hash and naming from the comparison team, in prose);
- C5 forced bilinear and anisotropic filtering (done in the beta: IN-433, IN-434);
- D1 on the desktop for 6x-12x and power parity;
- the frame-skip study.

## 5. Order and owner tests

| Order | Step | Owner test |
|---|---|---|
| 1 | A1 deinterlace, A2 FXAA | 13: menus (480i) and FXAA on/off |
| 2 | B0-B6 deep performance pass | 14: night B-Spec City race (cars facing the camera), race start, GPU power |
| 3 | C1-C3 picture | 15: shimmer, sharpness, CAS |
| 4 | D1-D2 GPU fast path | 16: 6x and above, power against GSdx |
| 5 | Phase E | beta candidate: full speed at 2x on the i5-7200U / HD 620 laptop |

The owner cannot save a replay now (2026-10-28). Step 2 uses the night B-Spec scene and its `.gscap` capture
instead (B0).
