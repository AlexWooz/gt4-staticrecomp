# CoreGS 2.0 (core 2) performance ledger

One table of every phase B optimisation step: what was tried, what it measured, and whether it stayed. The details
are in `IMPLEMENTATION-NOTES.md` (one row per IN number) and the journals of 2026-10-28 and 2026-10-29. Update this
file with every step, including the ones that are reverted.

## How to measure

- **Scene:** the headless night race (Hong Kong Reverse, B-Spec, VW Lupo, 5 AI cars, 3x). The presses are in the
  journal of 2026-10-28; local scripts `night-run.sh` and `title-run2.sh`.
  - Short race: VBlanks 6500-9100. Long race: VBlanks 9000-14000 (`FRAMES=14000 EVERY=1200 FROM=9000`).
  - Until candidate 335 some pixels varied from run to run in every build (frame 6600, the first race camera; 0-2
    pixels at frames 7200 and 9000; the box (1204-1359, 568-621) at frame 12000; far more at 12x). The cause was
    batches that sample the target they draw into (IN-463); since then two runs of one build are identical at 3x and
    12x, so a frame difference between two candidates is real.
- **Title check:** the GT Mode visit then the title (the IN-411 bug scene), compared byte for byte with candidate 287
  at VBlanks 4200, 4300 and 4400.
- **Timing lines,** every 600 VBlanks:
  - `[core2] ms/vblank`:
    - CPU stages: gif, batch, upload, translate, targets, texture, draw, readback, glm, swizzle;
    - GPU: `gpu_wait` (fence waits), `gpu_busy` (GPU time from timestamps), `gpu_draw_passes`, and the pipeline
      statistics `prims`, `fragments` and `compute`.
  - `[core2] gpu_wait ms/vblank by readback reason`.
  - The `[core2] vblank=` counters: barriers, render passes, flushes, recycled images and more.
- **Owner tests:** F7/F8 in a 1080p window at 3x on the night race; the report is in the session log (UTF-16).
- **Rule:** one change per candidate, with an A/B switch, frames checked, and CTest in both interlock modes.

## Ledger

Numbers are per VBlank on the night race at 3x unless stated. "GS thread" is the `gif` field. "Emulator" is the
wall time of the whole emulator.

| Step | Change | Measured effect | Kept? | Candidate |
|---|---|---|---|---|
| IN-406 | Present halving stops while the halved level still covers the window | Sharpness of GSdx at 3x in a 1080p window (owner test 13b) | yes | 287 |
| IN-407 | Small untextured sprites drawn by the CPU into memory | Part of B1, below | yes | 288 |
| IN-408 | CLUT loads and textures over a drawn CT32 target read on the GPU (GLM), no readback | Alone: slower, because GPU-palette textures were not cacheable | yes, with IN-409/410 | 288 |
| IN-409 | Indexed textures keep their indices; palette rows looked up at draw time | Made IN-408 a gain | yes | 288 |
| IN-410 | Submit every 96 batches | Readback waits fall, because the GPU is busy earlier | yes | 288 |
| B1 total | IN-407..410 | Readbacks 6.9-8.7 -> 0.8-1.6 ms; emulator 20.8-26.4 -> 17.0-20.6 ms | yes | 288 |
| IN-411 | GPU reads are not ageing uses; GPU-read targets stored before a drop or a row patch | Fixed the owner's title-backdrop bug; owner F8 57.0 VBlanks/s | yes | 289 |
| IN-412 | Repeated identical uploads dropped; block versions instead of page versions | 5-19 uploads dropped, decodes unchanged | **reverted** | - |
| IN-413 | Texture cache keyed by content (block tags) | Decodes 200 -> 15, but the emulator went 17-20 -> 26-30 ms (hashing about 2 MB per VBlank) | **reverted** | - |
| B2 lesson | | A decode costs about 10 us of CPU; the texture cache was not the bottleneck | | |
| measure | GLM page copies against the CPU upload swizzle | GLM copies 0.3-0.5 ms; swizzle 2.3-2.8 ms. The planned "uploads straight into the GLM on the GPU" would have removed little | | |
| IN-414 | Upload swizzle in page-row runs (in-page byte tables) | Swizzle 2.3-2.8 -> 0.9 ms; the new fixture found and fixed wrong T8H/T4HL/T4HH uploads | yes | 290 |
| IN-415 | No IN-411 store before a write that replaces whole pages under the target | Store waits 1.7-2.2 -> 0.2-0.3 ms; GS thread 16-22 -> 14-19 ms; owner F8 59.17 VBlanks/s, p99 19.6 ms | yes | 291 |
| IN-416 | Dirty rectangles exact page by page and block by block for writes in the same layout | Store waits about 0 on the long race; GS thread 9.0-15.7 -> 8.3-13.9 ms | yes | 292 |
| IN-417 | CLUT loads over several or depth targets, and textures over depth targets, via the GLM | CLUT readbacks about 1 -> 0; GS thread 8.3-13.9 -> 7.2-11.1 ms; owner F8 59.33, EE critical path 14.0 ms | yes | 293 |
| IN-418 | No allocation per batch or CLUT load; finished images reused by size | GS thread 7.2-11.1 -> 5.5-8.9 ms; owner F8 59.14 (now GPU-limited in the window) | yes | 294 |
| IN-419 | GPU timestamps; barriers only on real dependencies | Barriers halved; GPU time only 3-13% less | yes | 295 |
| IN-420 | Draw batches deferred into one render pass | Render passes 106-312 -> 4-8; GPU time 5-13% less | yes | 296 |
| IN-421 | D1 measured first: shader experiments, pipeline statistics | The whole fragment stage is 0.4-0.7 ms of GPU time; 53-65 M fragments; D1 not the next lever on the desktop | diagnostics only | - |
| IN-422 | Textures over no target decoded from a CPU page window (no GLM upload or barrier) | Barriers 254-961 -> 137-843; GPU time 3-10% less; emulator 0.1-0.3 ms less | yes | 297 |
| IN-423 | Per-VBlank spike log (`GT4_CORE2_SPIKE_MS`) | Found the race-start stutters: 40-160 new images per VBlank, each a device allocation | diagnostics | 298 |
| IN-424 | Image memory taken from 64 MiB blocks | Race-start VBlanks over 20 ms: 60 -> 2; averages unchanged; image memory +90 MB | yes | 298 |
| IN-425 | GPU cost map (`GT4_CORE2_GPU_PROFILE_AT`) | Headlight scene 5.9 ms of isolated GPU work against 7-19 ms per VBlank: gaps, not power | diagnostics | 299 |
| measure | Submit every 32 or 16 batches with two slots | Flare wait down, slot waits and spikes up (47 -> 66 -> 94) | not alone | - |
| IN-426 | Up to 8 submissions in flight, submit every 32 batches | Arcade race VBlanks over 17 ms 59 -> 15; GPU waits about a third; emulator 8.9-13.1 -> 7.6-10.5 ms | yes | 299 |
| IN-427 | 1x fix: texture from a target marked in the push word | 1x shows the 3D scene (was HUD only); 3x unchanged | yes | 300 |
| IN-428 | C1 mipmapping and LOD (all levels in one image, LOD in the pixel shader) | fidelity feature; distant asphalt less noisy; no visible cost on the night race | yes | 301 |
| IN-429 | readbacks of one transfer in one submission | camera cut readback 16.6 -> 14.4 ms | yes | 302 |
| IN-430 | dirty rectangles merged into the last pending one | camera cut dirty patches 863 -> 387, readback 13.3 ms, wall 46 -> 40 ms | yes | 302 |
| IN-431 | texture cache budget 192 MiB on integrated GPUs (768 discrete) | at 192 MiB: images 621 -> 507 MB, texture +0.16 ms | yes | 303 |
| IN-432 | present resampling (Lanczos-2 down, FSR1 EASU up, optional RCAS) | quality feature; three small compute passes at the window size | yes | 303 |
| IN-433 | forced bilinear option (forced, forced_no_sprite) | option, off by default | yes | 304 |
| IN-434 | anisotropic filtering of mipmapped draws (GT4_CORE2_ANISO) | option, off by default; GPU time unchanged within noise at 16x | yes | 304 |
| IN-435 | display reads in bands (12x dumps) | 12x: 1.6 GB of images, GPU 27 ms per VBlank | yes | 305 |
| IN-436 | scale sweep 1x-12x and the 12x cost found | GPU time per VBlank grows with the fragment count (6 M at 1x, 853 M at 12x, about 27 G fragments/s): 2.8 / 4.0 / 5.6 / 7.4 / 8.6 / 10.3 / 15.9 / 23.8 / 28.4 / 33.3 ms at 1/2/3/4/5/6/8/10/11/12x. Not bandwidth (skipping unneeded frame/depth reads: no change), not the interlock (removed: no change), not clocks (3.3 GHz, 98% busy, 85 W of 180 W). A fill-rate benchmark at 7680x5376: the uber shader 1.26 ms per full-screen pass, the same shader with the draw state folded to constants 0.41 ms, a minimal interlocked read-modify-write 0.58 ms, an RGBA8 attachment 0.21 ms (0.42 blended). The uber shader re-decides about 50 draw-state branches in every wave (`hw_process_pixel`: about 137 cycles per wave) | diagnostics only (read skipping reverted) | - |
| IN-437 | pipelines specialised per draw state (specialization constants, worker-thread compile, uber pipeline meanwhile) | Night race GPU busy per VBlank, 305 -> 306: 1x 2.8 -> 2.1-2.3, 3x 5.6 -> 4.2-4.5, 4x 7.4 -> 5.8, 5x 8.6 -> 7.2, 6x 10.3 -> 8.8, 8x 15.9 -> 11.2, 10x 23.8 -> 15.5, 11x 28.4 -> 18.3, 12x 33.3 -> 21.4 (max 42.4 -> 27.2); about 97 variants, 128 ms each on average (max 250 ms) on the worker; CPU wall within noise (a video playing during part of the sweep slowed every CPU stage alike) | yes | 306 |
| IN-438 | frame and depth words read only when the outcome depends on them; what limits 12x now | 12x GPU busy 21.3 -> 21.1 ms. Owner VRAM test 2500 -> 2800 MHz (+12%): 21.3 -> 21.1 (306), 21.1 -> 20.9 (438), so not memory bound; without interlock 20.6 (throw-away); all textures nearest 19.6 (throw-away, bound of a hardware-filter relaxation). Fill benchmark with incompressible colour: RGBA8 attachment 0.40 ms per 12x pass, the same as the specialised storage path (0.41) and the floor of a shader that may write an image (0.40): the earlier 0.21 ms was colour compression of a constant. A ROP path would not speed up this GPU | yes (exact; less memory traffic for bandwidth-limited GPUs such as the laptop's) | - |
| IN-439 | performance mode (decision 0073), first item: delayed downloads | GT4 downloads one tiny row per VBlank in a race (light flare visibility test, same buffer for 10-25 VBlanks). Answered with the previous same download's words, reduced on the GPU without a wait: 10x night race synchronous downloads 0.66 -> 0.24 per VBlank, download waits 3.5 -> 2.0 ms, wall 19.8 -> 19.2 ms (the GPU stays the limit at 10x: 16.2 ms busy). Frames at 10x differ from faithful mode by 2-54 pixels of 28.7 M (below the run-to-run variation) | yes, performance mode only (GT4_CORE2_PERFORMANCE=1 or GT4_CORE2_DELAYED_DOWNLOADS=1) | 307 |
| IN-440 | real cost of the GS memory upkeep at 12x (throw-away: no GLM writes) | 12x GPU busy 21.4 -> 20.8 ms with every GLM write (target materialisation, about 390 per race VBlank) removed: the isolated cost map (1.9 ms) overstates small commands, which overlap in a real run. Batching them would save a fraction of 0.6 ms | no (measurement only) | - |
| IN-441 | performance mode item 2: flares hidden (the flare visibility table answered as zeros, as rc7's TASK-117 does for the PCSX2 GS) | Clubman Stage Route 5 night (Arcade, B-Spec) at 10x: wall 18.1 -> 16.9 ms (max 19.9 -> 19.0), GPU 15.5 -> 15.4 ms, fragments 590 -> 587 M; 3676 tables hidden; no download left to delay. Hong Kong dusk (few lights) 3x/4x/6x/12x: within 0.1-0.6 ms. The light flares and the sun glare are gone; the rest of the frame is unchanged | yes, performance mode only (GT4_GS_HIDE_FLARES overrides) | 308 |
| IN-442 | constant fills as compute dispatches (core2_fill.comp) | Throw-away bounds at 12x: skipping the large untextured no-blend sprites 21.6 -> 18.1 ms (248 M of 853 M fragments: GT4's full-screen colour, depth and alpha clears), skipping large 1:1 copies 21.6 -> 20.4. Fill benchmark at 7680x5376: compute store 0.20 ms per pass, compute masked load+store 0.25, vkCmdClearColorImage 0.09, against 0.41-0.55 for the fragment stage. Implemented: GPU busy 3x 4.3 -> 4.1, 4x 5.4 -> 5.2, 6x 8.0 -> 7.9, 12x 21.6 -> 20.6 ms (fragments 853 -> 563 M); frames identical | yes (exact; GT4_CORE2_COMPUTE_FILLS=0 A/B) | 309 |
| IN-443 | large textured sprite passes as compute (the uber shader built as a compute shader, U/V planes fitted to the corners) | Throw-away bound at 12x: skipping the large textured sprites 20.6 -> 16.1-16.5 ms and peaks 27.8 -> 19.4 (162 M fragments). Implemented and measured: 12x 20.7 -> 20.8 ms (peak 27.7 -> 26.4), 3x 4.5 -> 4.3; frames identical once small sprites were left out (HUD glyphs scaled by odd ratios put a few U values within float rounding of a 1/16 step: 8 pixels per frame, 8/255). These passes are bound by the exact 4-texel bilinear sampling of large target textures, which costs the same in compute; the fragment-stage launch floor that IN-442 removed is not their limit | no (reverted; the lever for them is hardware filtering in performance mode) | - |
| IN-444 | hardware bilinear filter for RGBA8 / CT24 textures in performance mode (RGBA8 sampled views, unnormalised linear sampler, shader variants) | Performance mode, night race GPU busy: 12x 20.4 -> 19.9 ms (peak 27.7 -> 26.2), 6x 8.5 -> 8.6, 3x 4.4 -> 4.4. Frame differences on the car paint (the environment reflection), 1-8 levels on 0.05% of the pixels. The IN-440-era bound of 1.8 ms came mostly from the paletted scene textures, which the filter cannot take | no (reverted: gain only at 12x) | - |
| IN-445 | CPU survey: process sampler, transport line, exclusive stage times; SSE4.1 and AVX2 builds | 3x headless night race (310): process 27 ms CPU/VBlank (EE 10.2, GS worker 9.4, VU1 worker 8.1); GS worker stages add up to its thread time (front end 2.6, draw 1.7 + driver 1.6, texture 1.2, swizzle 0.8, translate 0.7, CLUT 0.6); EE transport about 2.8 ms plus the flare-read waits. Instruments cost nothing measurable (309 vs 310 same intervals). ISA, medians of 3 interleaved runs: SSE2 -> SSE4.1 wall 10.00 -> 9.62 ms (-3.8%), VU1 worker -16%, GS worker unchanged; SSE4.1 -> AVX2 9.62 -> 9.66 (no gain). Frames identical across the three builds | yes (instruments; `GT4_SSE41` ON by default; AVX2 not adopted). Fronts and task order: CORE2-CPU-SURVEY.md | 313 |
| IN-446 + IN-447 | GS stream ring (T2) and allocation-free draw path (R2), candidate 314 | 3x headless night race, 2+2 interleaved runs vs 313: GS worker CPU 7.20 -> 6.65 ms/VBlank (-7.6%), process CPU 20.6 -> 19.3 (-6.4%), EE time in the stream hooks 0.76 -> 0.47 ms, EE forwarding of VU1 output 0.95 -> 0.68 ms, wall 10.20 -> 9.63 ms (-5.6%), EE critical path 9.97 -> 9.31, frame p95 12.5 -> 11.6 ms; draw stage 1.28 -> 1.14 ms. Frames identical to 310 at VBlanks 8400, 9600, 10800 | yes | 314 |
| IN-448 | MTVU journal queues as rings of reusable slots (no deque block and no byte buffer allocated per journalled GS operation on the EE) | 2+2 interleaved runs, 315 vs 314: wall 9.50 vs 9.52 ms, EE critical path 9.10 vs 9.09, GS worker 6.70 vs 6.63: no measurable gain on this desktop (the sampler's 0.53 ms of allocator time came from the earlier busy-desktop run). Frames identical | no (reverted; the shared MTVU journal stays unchanged) | - |
| R1 (env only) | submission interval `GT4_CORE2_SUBMIT_EVERY` 32 (IN-426) vs 64 vs 128, on 315 | GS worker 6.63 / 6.27 / 6.19 ms (draw stage 1.11 / 0.84 / 0.70: about 40 us of CPU per submission), but the GPU starts later, so the flare read waits longer: GPU waits 0.28 / 0.63 / 1.18 ms, wall 9.52 / 9.61 / 9.90 ms, frame p95 11.2 / 11.3 / 12.0. Net worse | no (32 kept); next: submit from a separate thread so the GS worker does not pay the submission | - |
| IN-449 | bilinear footprints with textureGather (exact: R32_UINT sampled views of the same images, a nearest sampler, one gather instead of four image loads; core 2 shader variants, bindings 5 and 6) | 12x headless night race, 2+2 runs, `GT4_CORE2_GATHER=0` vs on: GPU busy 20.75 -> 21.29 ms (+2.6%), draw passes 14.74 -> 15.29 ms. On RDNA4 the sampler path costs more than four R32_UINT image loads (the same pattern as IN-444) | no (reverted) | - |
| IN-450 | uploads that change no byte keep the page versions (candidate 317) | 3x, 2+2 runs with `GT4_CORE2_KEEP_UNCHANGED=0` vs on: about 75 uploads per VBlank skip the version bump; GS worker 6.74 -> 6.55 ms (-2.8%), process CPU 19.36 -> 18.83, wall 9.64 -> 9.52, frame p95 11.5 -> 11.2. The stale decodes barely moved (about 220 per VBlank), so another writer invalidates them: `stale_by` counters added to find it. Frames identical | yes | 317 |
| IN-452 | PSMCT32 uploads by whole 8 x 2 columns with SSE2 (two host rows of 8 pixels are the column's four 16-byte quarters through unpacklo/hi_epi64; the column table is checked against the in-page offsets at start-up) | Micro-benchmark (fixture `--bench`, 512 x 256): CT32 0.61 -> 0.21 ns per pixel. 3x race, 2+2 runs with `GT4_CORE2_CT32_COLUMNS=0`: swizzle stage 0.75 -> 0.47 ms per VBlank; whole-thread totals within noise. Upload fixture (169 cases, both modes) and frames identical (id319noz) | yes | 319 |
| IN-451 | speculative depth rejection before texturing (exact) | First version (318/319) read the depth word through `hw_depth` itself before the interlock: frames differed (30-67k pixels at 3x; garbled banners, black patches on the car), isolated with `GT4_CORE2_EARLY_Z=0` (identical) and `GT4_CORE2_CT32_COLUMNS=0` (still different): the compiler reused the speculative value for the load inside the interlock. Fixed (320): the speculative word comes through binding 5, a second descriptor of the depth image, in core 2 shader variants. 12x, 2+2 runs: GPU busy 20.65 -> 20.27 ms (-1.8%), draw passes 14.76 -> 14.38 ms, frame p50 23.8 -> 22.4 ms (-5.8%), p95 28.1 -> 27.3 ms. Frames identical (id320) | yes | 320 |
| IN-453 | texture cache validated by block identities instead of 4 KiB page versions | About 220 decodes per 3x VBlank were "stale" textures whose pages had been written beside them (`stale_by`: uploads). v1 (320) hashed every upload (bytes and placement) into block identities: 934k hits, but the hash of 3.6 MB per VBlank cost what it saved. v2 (321) unique versions per block without content: few hits (GT4 cycles textures A, B over part of A, A again). v3 (323/324): the swizzle marks the blocks whose pixels changed (exact), only those are hashed (their 256 bytes, four lanes of xor-multiply), so any history that leaves the same bytes gives the same identity; a texture's key is its address key and its blocks' identities. 3x, 2+2 runs: 1.02 M block hits per run, decode windows 29.2 -> 2.3 GB per run, texture stage 0.84 -> 0.36 ms, draw +0.11 (recording moved there), swizzle +0.07, block hashing about 0.2 ms; GPU busy at 3x -3%. Frames identical | yes | 324 |
| IN-454 | GT4's clears (strips that cover a whole target; 3.7 ms of isolated GPU time per 12x VBlank in the S3 profile, its largest item): the frame cleared by vkCmdClearColorImage when the stored word is constant (PSMCT32, no FBMSK, alpha test, DATE or blend, depth test ALWAYS), the depth by the fill shader alone | Frames identical, 32k frame clears per run, but 12x GPU busy 20.31 -> 20.25 ms (-0.3%): the profile isolates each command behind barriers, and the fills are bound by the Z24 depth read-modify-write (the stored top byte is kept), which stays | no (reverted; the profile now prints each fill's shape) | - |
| IN-455 | the front end vertex queue as a ring over its 8-vertex array (the strip advance moves an index) | 3x, 2+2 runs vs 324: front end stage 1.80 -> 1.74 ms per VBlank, totals within noise. Front end fixtures and frames identical. Also: IN-451 alone at 6x, 2+2 runs: GPU busy 7.34 -> 7.22 ms | yes | 326 |
| IN-456 | the block of a pixel from the in-page address tables (`BlockOfPixel`) instead of `memory::pixelSlot` in the upload span, block identity and texture key paths (`pixelSlot` was 3.3% of the GS worker in the sampler) | 3x, 2+2 runs vs 326: upload stage 0.352 -> 0.329 ms per VBlank; the other stages and threads (the VU1 worker included, which the change does not touch) 1-2% higher in both pairs, machine drift. Upload fixture (182 cases: `BlockOfPixel` equals `pixelSlot` for 400 random pixels per format) and frames identical (id327) | yes | 327 |
| IN-457 | R6, CLUT loads: a CSM1 load walks a per-format table of block offsets and bytes (built once with `LocatePixel` at CBP 0) instead of `LocatePixel` per entry; the renderer marks the halfwords a load writes as at most two ranges instead of a 512-entry list | 3x, 2+2 runs vs 327: CLUT stage 0.468 -> 0.264 ms per VBlank, front end 1.75 -> 1.57 ms (the copy runs there), GS worker 6.38 -> 6.04 ms (-5.3%), wall 9.38 -> 9.17 ms (-2.3%); the GS worker now drains sooner (EE waits for it: drain 1.17 -> 0.91 ms, read 1.55 -> 1.33 ms) and so sleeps and is woken more often (30 -> 49 wake-ups). Front end fixtures (79, with a new check of every CPSM, both index widths and wrapping base pointers against `LocatePixel`), the ranges proved equal to the lists for all 192 load shapes, frames identical (id328). New line `[core2] batch flushes/vblank` (flushes by reason): in the race about 275 of the 480 batches per VBlank are flushed by CLUT loads | yes | 328 |
| IN-458 | R3: `record_draw` leaves out binds that would set what the draw pass already holds (descriptor set and dynamic offset, viewport, push words, scissor, pipeline); the vertex ring is bound once per pass and each draw starts at its batch's first vertex | 3x, 2+2 runs vs 328: draw stage 1.235 -> 1.225 ms (within noise: binds are cheap on this driver), EE waits for the GS worker 0.93 -> 0.84 (drain) and 1.36 -> 1.27 ms (read), wall within noise. Frames identical (id329). Kept for drivers with costlier calls (the laptop gate); `GT4_CORE2_SKIP_REDUNDANT=0` records every bind. Counter added: about 55 of the 275 CLUT-load flushes per VBlank keep the batch's texture (only the palette changes) | yes | 329 |
| IN-459 | T3 (first step): the VU1 worker wakes a sleeping EE for its output only past 64 KiB in the output ring, when it goes idle or stops with quiet publishes behind it, or when the ring is full; a freed input slot wakes the EE only while it waits for input space. Other publishes advance the bell without a system call | The EE slept and woke about 650 times per 3x VBlank while it waited for the VU1 worker (the flare read): now 16, with 37 wake calls on the worker. 3x, 2+2 runs vs 329: process CPU 18.37 -> 16.16 ms per VBlank (-12%), VU1 worker output 0.48 -> 0.26 ms, VU1 worker busy 3.19 -> 2.95 ms, EE wait for the VU1 worker 2.15 -> 1.93 ms, EE forwarding 0.66 -> 0.61 ms; the GS worker now gets the VU1 output in larger pieces, so the EE waits longer for it (drain 0.86 -> 1.15 ms, read 1.27 -> 1.57 ms) and wall is unchanged (9.05 -> 9.04 ms). Journal fixture: half of the 2,000 random scripts, a second 100,000-step script, the tiny-ring deadlock and the shutdown case run with quiet wakes (it caught a lost wake at shutdown in the first version: the last JobDone was quiet with the Stop record queued). Frames identical (id330). `GT4_MTVU_WAKE_KB=0` wakes at every record | yes | 330 |
| IN-460 | the GLM patch kernel (EE uploads into scaled targets) reads each native word once per 16 x 16 output tile into shared memory instead of once per output pixel (the S3 profile at 12x: "dirty patch" 1.9 ms of isolated GPU time per VBlank) | 12x, 2+2 runs vs 330: GPU busy 20.21 -> 20.21 ms, draw passes unchanged: the profile isolates each command behind barriers, and overlapped with the other work the patches cost nearly nothing. 3x frames identical | no (reverted) | 331 |
| exp. | 12x: indexed textures expanded into every decode (`GT4_CORE2_PALETTE_ROWS=0`) instead of palette rows looked up in the shader | GPU busy 20.21 -> 20.37 ms, draw passes 14.34 -> 14.44 ms, texture stage on the CPU 0.90 -> 2.92 ms: the palette rows are faster on both sides; the 12x draw passes are bound by the frame and depth traffic of 165 MB targets, not by the texel fetches | - | 331 |
| IN-461 | T3, direct GS intake (`GT4_MTVU_DIRECT=1`, hw renderer): the EE puts each VU1 job at its place in the GS stream (a 0-byte `MtvuJob` record) and the GS worker executes that job's GIF records straight from the MTVU output ring up to its JobDone; the EE no longer copies, forwards or walks the VU1 output and no longer waits for the VU1 worker before a GS read (the read waits for the GS worker, which waits for the output it reads); syncs that need the VU1 state wait on the worker's finished-job counter | 3x, 2+2 runs, journal vs direct (candidate 332): process CPU 15.92 -> 14.48 ms per VBlank (-9%), wall 8.95 -> 8.71 ms (-2.7%), EE critical path 8.67 -> 8.44 ms, frame p50 8.61 -> 8.40 ms and p95 11.04 -> 10.83 ms, EE forwarding 0.61 -> 0 ms, EE GIF hook 0.42 -> 0.33 ms, VU1 worker wake calls 36 -> 4 per VBlank; the EE's waits moved into the GS read (VU1 wait 1.91 + read 1.57 ms -> read 4.62 ms). Journal fixture: 1,000 random scripts, a 100,000-step script and the tiny-ring deadlock with a consumer thread that reads the output ring as the GS worker does. Frames identical with direct intake on and off (night race id332d/id332, Arcade menus, showroom and race start) | yes | 332 |
| IN-462 | Direct GS intake (IN-461) on by default after the owner's play test of candidate 332 (`GT4_MTVU_DIRECT=0` keeps the journal; a renderer without a GS worker in this process always uses it); frame dumps written a row at a time (a write per pixel made a 12x dump slower than the frame) | 12x headless runs with 31 dumps about 4 minutes instead of about 19; frames unchanged | yes | 333 |
| IN-463 | Spec 8.8, a texture that is the target its batch draws into: the batch samples a copy of the sampled rectangle taken before it (vkCmdCopyImage into one kept image per size, ordered after the draws that wrote the target). About 35 batches per race VBlank sample their own colour target (GT4's glow buffers, the luminance reduction in place, a full-screen 1:1 nearest pass in 32-pixel strips); on the GPU their fragments read texels that other fragments of the same batch write, and at scale even the 1:1 pass reads a neighbouring sub-pixel (the 12.4 texture coordinate is coarser than 1/12 pixel). `GT4_CORE2_SELF_COPY=0` samples the target itself (A/B) | 12x night race, two runs: 17-21 of 31 frames differed (up to 15,500 pixels, max 208) with or without full barriers (`GT4_CORE2_LAZY_BARRIERS=0`), every renderer counter equal between runs; with the copy 31 of 31 identical. 3x frames identical to 332. Cost: 51 Mpx copied per VBlank at 12x (the full-screen pass is 41), GPU busy 20.41 -> 21.69 ms (+6%), frame p50 22.5 -> 24.8 ms; at 3x one sixteenth of that. New CTest `hw_core2_self_texture` (an in-place half-size reduction equals the same sprite sampling an untouched second copy at 1x, 2x, 3x and 12x, the CPU reference at 1x, and two 12x runs) | yes | 335-337 |
| IN-464 | GLM hazards by 8 KiB page instead of the whole 4 MiB buffer: a decode, dirty patch, CLUT load from a target, target write into the GLM or EE upload names the pages it reads or writes (the renderer's conservative page rows), so a GLM access needs a barrier only after a command that touched one of its pages; plain decodes (palette modes 0 and 1) no longer count as GPU CLUT readers. New diagnostics: `GT4_CORE2_BARRIER_TRACE=1` prints the full barriers by command and hazard. `GT4_CORE2_GLM_PAGES=0` for A/B | 3x race: full barriers 579 -> 276 per VBlank (about 285 had been whole-buffer GLM hazards between unrelated pages), GPU busy 4.03 -> 3.95 ms (-2%, interleaved 2+2), CPU and wall within noise; 12x unchanged (21.7 ms: the draw passes are bandwidth bound). Frames identical to 337 (3x long race 10/10, 12x 31/31). Kept for GPUs where a full barrier costs more (tilers, the laptop's iGPU) | yes | 338-339 |
| IN-465 | The pipeline statistics query over every command buffer (prims, fragments, compute invocations of the reports) is diagnostics only: off by default, `GT4_CORE2_GPU_STATISTICS=1` turns it on; `GT4_CORE2_PASS_TIMESTAMPS=0` leaves out the draw pass timestamps (A/B; kept on for `gpu_draw_passes`) | 3x, interleaved 2+2, both off against both on: draw stage 1.27 -> 1.20 ms, readback stage 0.52 -> 0.46 ms, GPU busy 3.92 -> 3.89 ms; wall and process CPU within noise. Off by default because some drivers (mobile ones in particular) pay for statistics queries; frames unaffected (no rendering change) | yes | 341 |
| IN-466 | The per-primitive path (GPUs without the pixel interlock, and line and point batches everywhere): a primitive waits for the earlier ones of its batch only when its rectangle meets one drawn since the last wait; disjoint primitives write disjoint pixels. `GT4_CORE2_GROUP_DISJOINT=0` waits before every primitive (A/B). New diagnostic `GT4_HW_INTERLOCK=0` turns the interlock off on a device that has it | 3x night race without the interlock (the path of most mobile GPUs): GPU busy 3.9 ms with it, 49.1 ms without it, 43.7 ms without it and with IN-466 (-11%; strips of triangles mostly overlap by their boxes); frames identical with and without the interlock (3x long race 10/10; CTest 29/29 with it, 52/52 GPU tests without it) | yes | 342 |
| IN-467 | Triangle strips: of a run of non-drawing (ADC) kicks only the first (the flush decision) and the last two (the queue the next kick needs, the registers left behind) go through `Kick`; the ones between only count | GIF front end 1.59 -> 1.48 ms per VBlank (-7%, interleaved 2+2), totals within noise; `hw_front_end_fast_paths` (fast against generic path on random streams) passes; frames identical | yes | 343 |
| IN-468 | PGO and ThinLTO of the GS libraries only (`GT4_HW_PGO=generate\|use`, `GT4_HW_PGO_PROFILE`, `GT4_HW_THINLTO=ON`; `scripts/coregs2_bench/pgo-build.sh`), trained on the night race and the Arcade path at 3x, off by default | Against the same code without them, 3x, interleaved: PGO alone GS worker 5.88 -> 5.50 ms (-6.5%), frame p50 8.85 -> 8.28 ms; with ThinLTO GS worker -11%, frame p50 8.05 ms, GIF front end 1.59 -> 1.14 ms (-28%), translate 0.65 -> 0.57 ms. No code change, frames identical. Not wired into the release build yet | yes | 345, 347 |
| IN-469 | The VU1 output scan (`GifScanner`, the PATH1 invariant check on the worker) skips a PACKED tag without A+D in one step instead of walking every qword with a division | VU1 worker busy 2.93 -> 2.66 ms per VBlank (-9%, interleaved 2+2), the rest within noise; new case in `mtvu_measure_fixture` | yes | 346-347 |
| no number | Translate without the copy of three vertices per triangle | Translate stage 0.645 -> 0.635 ms: the compiler already avoided most of it. Dropped; small edits on the per-vertex path are exhausted | no | - |
| IN-470 | Texture replacement and dumping (chapter 11, Format P names, `GT4_HW_TEXTURES`, `GT4_HW_DUMP_TEXTURES`) | Owner's HD UI pack (15,423 files, 1.7 GB): 133 images matched on the Arcade path, so the hash matches GSdx packs; 3x night race with the pack against none, interleaved 2+2: GS worker 5.68 -> 6.17 ms, wall 8.74 -> 9.03 ms, frame p50 8.54 -> 8.80 ms, p95 unchanged, GPU +1%. Hash spikes in menus 33 ms -> gone (a whole-row read instead of an allocation per texel; 200 -> 3 spike lines). Without a pack frames are identical to 337 and the path is one null check | yes | 351 |
| IN-471 | GPUs without pixel interlock: primitives wait only for the ones they may share a pixel with (triangle test across the batches of a pass, a fan or strip neighbour that shares only a vertex or an edge does not wait), merged draws, specialised pipelines | 3x night race, `GT4_HW_INTERLOCK=0`: GPU busy 49.05 -> 6.85 ms (3.9 ms with the interlock), frame p50 46.3 -> 10.7 ms, p95 13.5, x_draw 22.4 -> 2.83, GS CPU 8.5 -> 7.36. 10/10 frames identical to the interlock path at 3x, 14/14 Arcade, 31/31 at 12x. With the interlock nothing changes (10/10) | GPU -86% without the interlock | 352 |
| IN-472 | Hardware raster path (D1): measured, not built for rc8 | `GT4_CORE2_D1_STATS=1`, night race at 3x: exact no-feedback draws are 20-23% of the area, of which 16-19% are untextured (mostly the compute fills of IN-44x); about 4% is left for an exact attachment path. 38-44% is blended: the ROP rounds differently from the GS, and IN-436/IN-438 measured no gain from it on this GPU. Decided with the laptop test | - | - |
| IN-473 | PGO and ThinLTO of the GS libraries in the release build | 3x, 3+3 interleaved, plain -> PGO, medians: GS worker 5.93 -> 5.14, process CPU 16.48 -> 15.72, wall 8.74 -> 8.33, frame p50 8.72 -> 7.84, p95 11.21 -> 10.67, GPU 3.90 -> 3.86. 10/10, 14/14, 31/31 frames identical | GS worker -13% | 354 |
| IN-477, IN-478 | Performance mode on the HD 620 laptop: the EE does not wait for the GS at the flare table; plain blit instead of FSR on integrated GPUs | Arcade race at 1x, headless: EE drain wait 11.4 -> 0 ms, wall 22.3 -> 19.5 ms (51 FPS), now bound by the GS worker (19.3 ms). With a window: GPU 18.07 -> 13.64 ms, wall 29.1 -> 22.95 ms. Desktop: 7.7 -> 6.6 ms, 14/14 frames identical | - | 358 |
| IN-481..484 | Light path on the HD 620 | Headless Arcade race at 1x: wall 22.3 ms (first bench) -> 18.3 ms. GSdx reference with a window, first 35 s of the race (approximate; samples counted twice in that run and halved): GSdx 1x about 54 FPS, 2x about 44; CoreGS 1x about 45, 2x about 28, contaminated by 96 s of shader compiling. GSdx itself holds no 60 FPS in this scene | - | 362 |
| IN-485..487 | Palette memo, upload memo, block lookup | GS thread on the HD 620 at 1x by day 13.70 -> 11.91 ms for the palettes, -0.45 for the uploads. Frames 10/10, 14/14 | yes | 366 |
| IN-488 | Fixing threads to cores | No gain | no | - |
| IN-489 | Shader cache between sessions | HD 620 GPU 13.7 -> 8 ms in the first sessions of a new executable | yes | 366 |
| IN-507 | Shaders compiled before Play | RX 9060 XT, George V Paris at 6x, prepared cache: 0 of 5.9 M batches on the uber pipeline | yes | 369 |
| IN-490, IN-491 | Video decoder with the same results | i5-7200U, track selection video: 265-280 of about 297 intervals at 16-17 ms, was 123-134 at 20-25 ms in part | yes | 366 |
| IN-492, IN-498 | Depth and alpha fail before the interlock | HD 620 2x by day 25.65 -> 23.9 ms for the depth test; -7% and more with both | yes | 366 |
| IN-494, IN-495 | Whole clears, no self copy when no lane changes | HD 620 2x by day 23.7 -> 22.35 ms | yes | 366 |
| IN-496 | No interlock for large disjoint batches, integrated GPUs | HD 620 2x by day 20.8 -> 20.28 ms; RX 9060 XT +4% at 12x, so off there | yes | 366 |
| IN-497 | Small effect targets at 1x, Performance Mode | HD 620 2x night 27.5 -> 25.3 ms; picture changes a little | yes, Performance Mode | 366 |
| IN-499 | CT16 frames through attachments | HD 620 2x by day 17.99 -> 17.85 ms | yes | 367 |
| IN-501, IN-502 | No road sheen and no alpha fills, Performance Mode | HD 620 2x by day 18.81 -> 17.93 and 17.88 -> 16.94 ms; the road is darker | yes, Performance Mode | 367 |
| IN-503 | The sheen leaves early without destination alpha | HD 620 2x by day 18.59 -> 17.99 ms; frames 22/22 | yes | 367 |
| dropped | Sampled texels, real depth probe, unordered interlock, depth store skip, ROP on a discrete GPU, tone palette in push constants, input attachments | Bench gains that the game did not show: -1%, -3.6%, none, none, +13%, none, none | no | - |
| owner | Test of 362 on the HD 620 laptop through the launcher (performance mode, 1x, day track) | F8, 48 s: 59.3 VBlanks/s, frame median 16.8 ms, p95 19.5, p99 21.1, max 25.3 (first test of the day: 41.5 VBlanks/s, median 24 ms) | 1x reached | 362 |
| owner | Test of 299 (Arcade night race, B-Spec, 3x) | 60 FPS, frametime nearly flat; GPU 2113 MHz, 66%; fidelity perfect. Phase B closed for the beta | - | 299 |

## What we learned

- **Measure before building.** The plan was wrong three times, and each time a measurement caught it:
  - the content-keyed texture cache (IN-413);
  - "the double copy is the cost" (it was the swizzle, IN-414);
  - D1 as the next GPU lever (IN-421).
- **Stores and readbacks:**
  - GPU waits came mostly from IN-411's safety stores, not from the flare download.
  - GT4 streams textures over the memory of its night render every frame. Whole-page writes (IN-415) and page-exact
    dirty rectangles (IN-416) removed the stores without changing a pixel.
- **CPU housekeeping:** allocation churn and device-memory churn cost the GS thread about 2 ms per VBlank (IN-418).
  Profile with the all-thread sampler (`build/prof/sampler2.cpp`, git-ignored) on a RelWithDebInfo build.
- **The GPU side:**
  - The owner sees the GPU at 45 W of 180 W: it waits more than it works.
  - The fixed GPU time is the chain of about 1000 small dependent commands per VBlank (texture decodes, GLM uploads,
    patches, CLUT loads); halving barriers and render passes gave only about 10%.
  - The fragment shader is cheap here. D1 remains required for the HD 620 beta gate, where shading is far slower.
- **GPU changes give far less than expected.** Removing half the barriers (IN-419), 97% of the render passes
  (IN-420) and more barriers (IN-422) each gave 3-13% of GPU time.
  - **Confirmed by the owner (297):** the RX 9060 XT runs at about 1650 of 3300 MHz, 47-50% use, 29 W; VRAM at about
    1350 of 2500 MHz; the same with GSdx.
  - The driver keeps the GPU at a low clock because it is idle, so GPU timestamps measure the clock more than work.
  - On this desktop the GPU is not the limit. The frame is the EE critical path (about 14 ms), of which the renderer
    owns about 2.5 ms (the EE waiting for the flare downloads).
- **Stutters are spikes, not averages.** The 600-VBlank averages hid them. Look per VBlank with the spike log. The
  race-start stutters were device memory allocations (IN-424), not readbacks.
- **Two submissions in flight were the structural limit:** the CPU waited for a GPU that ran at a low clock and was
  full of gaps (IN-426).
- **Split rings into aligned shares:** a non-power-of-two slot count broke buffer offsets. Only the frame checks
  caught it, and the bad run looked faster.
- **Timestamps:** with deferred draws, compute recorded before a pass overlaps the pass's timestamps, so
  `gpu_draw_passes` overstates drawing.

## The readback before a write (IN-508, candidate 370)

| Race on George V Paris, 6x, ms per VBlank | Off | On | Performance off | Performance on |
|---|---|---|---|---|
| GS thread, all its work (`x_total`) | 17.41 | 14.04 (-19%) | 14.95 | 12.25 (-18%) |
| Readback phase | 6.91 | 2.76 (-60%) | 4.50 | 1.78 (-60%) |
| GPU waits | 4.86 | 2.08 (-57%) | 2.44 | 0.39 (-84%) |
| GPU waits, "before write" | 3.28 | 0.00 | 2.08 | 0.00 |
| Readbacks "before write" per VBlank | 3.00 | 0.00 | 3.00 | 0.00 |
| GPU busy | 12.00 | 10.29 (-14%) | 9.39 | 8.11 (-14%) |
| EE critical path | 19.97 | 16.70 (-16%) | 16.20 | 13.21 (-18%) |
| Frame median | 20.03 | 16.54 (-17%) | 16.15 | 12.97 (-20%) |
| Frame p95 | 23.41 | 20.88 (-11%) | 20.21 | 17.11 (-15%) |
| GS thread CPU | 12.31 | 11.81 (-4%) | 13.18 | 12.64 (-4%) |
| Process CPU | 24.92 | 24.37 (-2%) | 26.83 | 26.07 (-3%) |

Headless, unpaced, Spec II, RX 9060 XT, one executable with `GT4_CORE2_DRAWN_PAGE_TEST=0` and without it, three interleaved rounds. "Off" and "On" are without Performance Mode; the other two columns are with it. The `[core2]` lines cover VBlanks 20400 to 21000, the `[hw] cost` lines 20700 to 21300. Candidate 369 in the same rounds gave `x_total` 17.37 and 14.91, the same as the switch off. Evidence: `build/evidence/in508-ab.log` (ignored by git).

Frames against candidate 369: 78 of 78 identical. The largest cost on this scene was a store for a write that touched no drawn pixel. What is left of the readback phase is "keep" and the flare download. Not yet the owner's F8.

## The frametime wave (IN-509, candidate 371)

| Owner's F8 on candidate 370, George V Paris at 6x | No Performance Mode | Performance Mode |
|---|---|---|
| Session | session-1793511403 | session-1793511564 |
| VBlanks/s | 59.20 | 59.45 |
| Frame median / p95 / p99 / max | 16.68 / 21.59 / 23.27 / 41.15 | 16.63 / 23.30 / 24.21 / 42.96 |
| Intervals under 15 ms | 1684 of 5657 | 878 of 6088 |
| Intervals of 20 ms and more | 1148 of 5657 | 704 of 6088 |

| Race on George V Paris, 6x, ms per VBlank | Off | On | Performance off | Performance on |
|---|---|---|---|---|
| Wall per VBlank, mean | 16.63 | 13.33 | 13.09 | 11.12 |
| Wall per VBlank, p95 | 21.87 | 15.13 | 17.35 | 12.75 |
| Wall per VBlank, p99 | 22.83 | 15.77 | 18.15 | 13.45 |
| Wall per VBlank, max | 23.87 | 18.10 | 20.20 | 15.05 |
| Change from one VBlank to the next | 4.38 | 0.44 | 3.87 | 0.40 |
| GS thread, even / odd VBlanks | 15.22 / 11.17 | 10.24 / 10.26 | 14.30 / 10.55 | 10.85 / 10.71 |
| GS thread, all its work (`x_total`) | 14.07 | 11.05 | 12.36 | 10.60 |
| Readback phase | 2.79 | 0.59 | 1.80 | 0.00 |
| GPU waits | 2.05 | 0.51 | 0.39 | 0.03 |
| GPU busy | 10.25 | 8.72 | 8.16 | 7.33 |
| EE critical path | 16.72 | 13.22 | 13.33 | 11.02 |

Headless, unpaced, Spec II, RX 9060 XT, one executable with `GT4_CORE2_WIDTH_GRACE_LONG=0` and without it, three interleaved rounds, VBlanks 20400 to 21300 for the per-VBlank rows. The Performance Mode columns use rounds 2 and 3: round 1 with the change on had one 261 ms VBlank (20462) in which every phase slowed by the same factor of about 8, with no readback: External interference (High confidence), not seen in the other five Performance Mode runs. Evidence: `build/evidence/in509.log` (ignored by git).

Frames against candidate 370: 117 of 117 identical. **Lesson:** averages hid this. The mean GS time was below 16.67 ms and the machine held 60 FPS, while every second VBlank went over it. Look at the VBlank-by-VBlank line (`GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=1000`) and at the change from one VBlank to the next. Not yet the owner's F8 on this build.

## Race start, vsync and showroom (IN-510 to IN-513, candidate 372)

| Window, Spec II George V Paris at 6x, vsync on, ms per VBlank | 371 normal | 372 normal | 371 Performance | 372 Performance |
|---|---|---|---|---|
| Showroom: GS thread, even / odd VBlanks | 6.1 / 9.9 | 6.0 / 6.0 | 5.7 / 9.5 | 5.7 / 5.7 |
| Showroom: GPU waits | 4.25 | 1.40 | 3.79 | 1.04 |
| Pre-race: GS thread mean / max | 8.82 / 26.7 | 8.05 / 18.3 | 8.07 / 23.5 | 7.36 / 11.4 |
| Pre-race: wall p95 | 18.4 | 17.1 | 16.8 | 16.8 |
| Rolling start and race: wall mean | 18.13 | 16.79 | 16.67 | 16.67 |
| Rolling start and race: wall p95 / p99 | 19.9 / 20.2 | 17.6 / 18.0 | 16.8 / 16.9 | 16.8 / 16.9 |
| Rolling start and race: VBlanks over 20 ms, of 1000 | 29 | 1 | 0 | 0 |
| Rolling start and race: GS thread | 13.22 | 11.18 | 11.90 | 10.17 |
| Rolling start and race: GPU waits | 3.76 | 2.44 | 1.11 | 0.18 |

Scripted runs in a real fullscreen window, paced, one run each, on the desktop's active display (a 1920 x 1080, 60 Hz virtual display, DISPLAY22, the same one as the owner's sessions). VBlanks 17800-18200 (showroom), 19000-20300 (pre-race), 20300-21300 (rolling start and race). Evidence: `build/evidence/final.log` (ignored by git).

- **Race start of the v2.00 Arcade path at 6x, headless, VBlanks 8800-8999, three interleaved rounds (IN-510 set memo against none):** VBlanks over 16.67 ms 16 to 0 in each round; p95 19.7 to 13.0 ms; worst 25.2 to 16.4 ms. In the burst itself, VBlank 8826: 37 to 12 ms, GLM writes 2,257 to 25, GPU busy 21.9 to 9.5 ms.
- **Three swapchain images (IN-511), window, the same Arcade race start, two rounds each:** GS thread 8.47 to 6.47 ms, GPU waits 6.52 to 4.18 ms, equal to no vsync (the "mailbox" request fell back to immediate on this display).
- **Showroom, headless (IN-512, one run each):** stores about 1 to 0 per VBlank, GPU waits 1.44 to 0.10 ms, GS thread 4.1 / 6.9 to 6.0 / 5.9 ms on even and odd VBlanks: the same work spread evenly, slightly more on average in a bench with no display (5.51 to 5.96 ms).
- **George V Paris, whole run to VBlank 21000, headless (IN-513 and the per-row IN-510):** decodes 413,167 to 133,392, GLM writes 716,978 to 137,362; race GS thread 11.09 to 10.66 ms and GPU busy 8.69 to 8.23 ms for IN-513 alone.

**Frames against 371:** 1x 37/37, 3x 10/10, Arcade 27/27, Spec II 49/49; 12x 30/31 (2 pixels, at most 8 of 255, IN-512). **Measured and dropped:** an exact page test for textures over targets (no gain). **Lessons:** the headless bench understates GPU waits by up to ten times, so a window run is worth its 6 minutes; before optimising a repeated cost, count how many of the repeats are distinct. Not yet the owner's F8.

## Frame pacing from the owner's reports (IN-514 to IN-519, candidate 373)

Window, fullscreen, Spec II George V Paris at 6x, 16:9, real 144 Hz monitor unless noted, vsync on, normal mode, one run each; race VBlanks 20300-24000. "Over 17.5" and "longest" are the intervals between presentations, what RivaTuner draws.

| Build | GS ms | GPU waits ms | Over 17.5 | Longest ms | Late flares |
|---|---|---|---|---|---|
| 372 | 9.42 | 3.08 | 40 | 19.6 | - |
| + second queue, fixed 12 ms limit, 8 slots (`slots8`) | 10.15 | 3.37 | 283 | 20.5 | 1,124 |
| + 16 slots (`slots16`) | 7.84 | 2.56 | 84 | 19.8 | 385 |
| + adaptive limit (`ad32`) | 7.75 | 2.67 | 19 | 18.7 | 1,099 |
| + fresh PGO, FIFO (`f0`) | 6.79 | 3.49 | 4 | 18.6 | 481 |
| + allowance 0.7 ms (`z0`, final) | 6.79 | 3.44 | 5 | 18.8 | 291 |
| Performance Mode, 372 / final | 8.49 / 7.52 | 0.90 / 0.46 | 0 / 1 | 17.5 / 17.6 | - |

**The "GS ms" column leaves out the flare read,** which runs outside the GIF stage, so it flatters normal mode: with 16 slots the wait moved into that read. All stages (`[core2] exclusive` total, race, last six reports):

| | All stages ms | Flare and other reads ms | GPU waits ms | CPU ms (all less waits) | EE at the read ms |
|---|---|---|---|---|---|
| 372 normal | 11.00 | 1.43 | 3.07 | 7.94 | 8.55 |
| 373 normal | 10.73 | 3.80 | 3.42 | 7.31 | 8.58 |
| 372 Performance | 8.70 | 0.24 | 0.92 | 7.78 | - |
| 373 Performance | 7.72 | 0.57 | 0.49 | 7.24 | - |

So 373 costs about 8% less CPU in normal mode and 7% in Performance Mode, and Performance Mode is lighter than normal mode, as expected. The flatter frametime in normal mode comes from IN-514.

The frametime log costs nothing measurable (final build without it: GS 6.77 ms, 255 late flares). The run-to-run spread here is large (GPU waits 3.4 or 4.2 ms on identical builds, bimodal, Unknown cause, possibly the GPU's clock). Treat single runs as direction.

**Virtual 60 Hz display, 16:9 (fullscreen, the owner's display while streaming), candidate 372's paths; a real monitor at 60 Hz was not tested:** FIFO showroom GS 13.77 ms, waits 10.96 ms, 132 of 400 over 17 ms; latest ready 13.81 / 11.04 / 130; immediate (no vsync, mailbox falls back) 5.93 / 0.87 / 2. **The second queue has no run on that display.**

**The same virtual display at 144 Hz (set by the owner), 16:9, candidate 373's build:** old path, 8 slots: showroom waits 0.79 ms, GS 5.60 ms, race over 17.5 ms 26, longest 18.4 ms. Old path, 16 slots: 0.01 ms, 4.62 ms, 19, 18.4 ms. Second queue, 16 slots: 0.01 ms, 4.65 ms, 9, 18.7 ms. So the showroom stall goes with the 60 Hz, not with the virtual display.

**The virtual display back at 60 Hz, candidate 373's build, same scene, normal mode.** "Sent" is what RivaTuner draws, "shown" is when the display took the frame:

| Path | Showroom waits ms | Sent over 20 ms, showroom + race | Longest sent ms | Shown over 18 ms, race | Latency ms | Not shown, session |
|---|---|---|---|---|---|---|
| Old, 8 slots | 11.42 | 1 (134 over 18) | 20.1 | 0 | 45-49 | 0 |
| Old, 16 slots, two runs | 0.47, 1.61 | 2, 4 | 25.4, 33.1 | 0, 1 | 49-51 | 0 |
| Old, 24 slots | 0.06 | 3 | 25.1 | 2 | 50-51 | 0 |
| Old, 16 slots, 2 images | 1.40 | 45 | 36.1 | 24 | 35-39 | 0 (late flares 96%) |
| **Second queue, 16 slots, two runs** | 0.01, 0.01 | 0, 0 | 18.2, 17.9 | 59, 25 | 51-55 | 1, 1 |
| Second queue, latest ready | 0.01 | 0 | 17.8 | 4 | 63 | 1 (3 lost in the race) |
| Second queue, 2 images | 0.01 | 1 | 33.4 | 19 | 44 | 11 |

The jagged sent graph of the old path did not reach the display: its shown intervals stay even. Latency near 3 refreshes at 60 Hz (about 50 ms) and at 144 Hz (13-20 ms). Default kept: second queue, 3 images, FIFO.

**21.5:9 (the owner's monitor):** without a limit, race over 20 ms 126 of 3,700; with one (`pqA`) 3; final 0, longest 18.6 ms, late flares 1,681.

**Dropped or left off:** latest ready (IN-515: no gain); 4 swapchain images (worse: race over 17 ms 633 against 182); `GT4_CORE2_SUBMIT_EVERY` 16 (GS 10.47 ms) and 64 (GS 7.35 ms, more late flares); 24 slots (nothing over 16); limit allowance 2 ms (over 17.5: 35).

**CPU sampler, 6x race (`scripts/cpu_sampler_report.py`, before the fresh PGO):** after IN-518 the GS worker's profile is flat: `GpuVertex` construction 0.33 ms, stage timers 0.13 ms, submission 0.55 ms inclusive, the driver about 1.2 ms. No single large item is left.

**v2.00 Arcade race start at 6x (window, 16:9):** presented p99 21.4 to 19.9 ms and intervals under 12 ms 16 to 9 of 1,200 with IN-519. About 1 s has GPU busy 15-18 ms, and the GS thread waits for it at the flare read (11.8 ms): the next step.

## The waits and the display queue (IN-521, IN-522, IN-523, candidate 374)

Spec II, George V Paris, normal mode, the owner's lap `george-volta2` replayed with `lap-run.sh`, VBlanks 2040-8032, 16:9, 60 Hz virtual display, one run each.

| 6x | Presentation to display, mean (p1 / p99) | Presented over 17.5 / longest | Shown over 25 | EE at the flare read | GS all stages |
|---|---|---|---|---|---|
| 373's behaviour (no queue control, EE waits) | 53.0 (37 / 55.3) | - | - | 9.5 | - |
| Queue control only, EE waits (`d0`) | 37.2 (36.6 / 37.8) | 17 / 33.1 | 2 | 9.5 | 11.54 |
| No EE wait, first queue control (`d1`, stuck at the longer level) | 41.8 (30.9 / 48.3) | 9 / 33.4 | 2 | 0.00 | 11.12 |
| **Final (`e1`)** | **39.5 (39.1 / 40.1)** | 7 / **21.0** | 1 | **0.00** | 11.38 |
| Final, Performance Mode (`p1`) | 39.8 (39.3 / 40.3) | 8 / 33.6 | 2 | 0.00 | 7.11 |

For scale: no vsync 24.9 ms on the same display, the phase walk (dropped) 35.4 ms with 18 repeats of a frame per lap. The level, 31 or about 39 ms, comes from the phase of the presentation against the flip and changes between runs.

**Frames:** the whole lap headless at 3x, EE waiting against not waiting: 18 of 20 identical. The other 2 (VBlanks 4500, 4800, at most 1,216 pixels at the right edge) differ the same way between two runs of the EE waiting: the noise the owner put down to the crowd's flashes. EE cycles equal, 39,478,966,503. 269,534 VIF packets got their words in the lap, about 47 per frame.

**12x (`d1probe`):** normal mode does not hold 60 FPS on this track: presented mean 21.3 ms, GPU busy 19.73, GS 21.00, reads 10.34. With `GT4_CORE2_ROP=1` 23.0 ms, GPU busy 21.37. Area and reading in IN-523.

**Evidence:** `build/evidence/in522`, `in522-frames`, `d1probe`.
