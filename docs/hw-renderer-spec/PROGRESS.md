# CoreGS 2.0 (HWR, TASK-027) progress log

Running log of the TASK-027 implementation on `task/027-coregs2`. The implementer updates it with every pushed
step; the same state goes to the ai-memory page `decisions/coregs2-implementation`. Clean room: written from
`docs/hw-renderer-spec` only. Brief: [AGENT-BRIEF-COREGS2.md](../../AGENT-BRIEF-COREGS2.md).

## Milestones

| M | State | Evidence |
|---|---|---|
| M0 scaffolding | Done; game hookup moved to the GS stream seam on 2026-10-21 | CTest `hw_options`, `hw_present_rect`, `hw_smoke_*`, `hw_stream_sink` |
| M1 tools | Native replay supports legacy and GSSTREAM v2; real menu parity confirmed | slice219: 69/69 CPU-library tests; 1,101/1,101 identical PPMs in legacy, converted v2 and fresh donor v2 replays |
| M2 memory | Done | CTest `memory_*` |
| M3 front end, CLUT | Done; A+D vertex kicks fixed (IN-326) | CTest `m3_*`, `m3_hw_gif_ad_xyz` |
| M4 reference I | CPU reference done; the GPU write-back path is ported from `task/027-m4-gpuref`, built, wired into the backend and green in 11 fixtures (IN-329). The S-01/S-02 parity replay is still missing (no HWR replay tool) | CTest `cpuref_*`, `hw_flat_provoking`, `hw_sprite_corners`, `hw_gpu_coverage_vectors`, `hw_gpu_equivalence_*` |
| M5 reference II | Arithmetic done on CPU and GPU (bit-exact) and wired into the raster path: blend unit (81 modes), COLCLAMP, PABE, alpha test (8 methods x 4 AFAIL), DATE, FBA, FBMASK, DIMX dither | CTest `pixel_*`, `hw_gpu_*`, `hw_gpu_raster_*` |
| M6 output | Done except owner test 1: display library, presentation interfaces (IN-331) with `end_frame`/`present_slot`, HWR frame dumps (IN-332; `vram` = reference image, `scanout` = presentation image, IN-338), replay tool (IN-333/IN-334), odd-frame draw loss fixed (IN-335), field offset and deinterlace in the presentation (IN-338) with the adaptive rule corrected (IN-337). Open: IN-336 (triangle precision, inside the gate) | local slice229: `hw_presentation_field_phase`, `display_hw_deint_adaptive`; 42/42 `vram` dumps inside the 2D gate and byte-identical to slice228; presentation replay of the menu capture bob-free on static screens |
| M7 cache | In progress: resident target slots with LRU (IN-340/IN-344), decoded-texture cache (IN-341), deferred submission (IN-342), textures from targets (IN-346), CPU small sprites (IN-348), GPU CLUT (IN-349), GLM-owned pages instead of write-backs for evictions, overlaps and uploads (IN-350/IN-351); gated by `hw_cache_equals_writeback` and per-frame memory hashes of the menu, car and race captures (identical to the write-back mode). Open: target lists per chapter 05, display from targets, preload, dirty lists, clears, GPU moves | local slice237: race replay 1 write-back per frame (display) instead of 12; headless race 65 -> 38 ms per vsync |
| M12 upscaling | Owner test 6 failed (half the race frames shown at 1x; ultrawide squeezed); both fixed in local slice270 (IN-377 display shadows, IN-378 display-aspect fill); owner test 7 still 1x (GT4 tone curve and full-screen copies at 1x), fixed in local slice271 (IN-379, IN-380); owner test 8 passed (races); 480i menus at scale in local slice272 (IN-381), in owner test 9. 2x-8x through a presentation-only scaled copy of every target (IN-371..IN-375), FXAA (IN-376, M15 part); 480i menus still 1x | `hw_scale_exact`, `hw_scale_display_shadow`, `hw_scale_fullscreen_passes`, `hw_scale_interlaced_present`, `hw_postfx_fxaa`, `hw_smoke_window_scaled`, `hw_smoke_window_aspect_*`, `hw_present_rect`, scale-3 block of `hw_cache_equals_writeback`; replay `--check-presented`; memory hashes identical at 6x |
| M8, M14, M15 (rest), M17, M19 | M8 flare readback done earlier (IN-357..IN-359); others not started | |

| M9 batching | In progress: one draw per chunk of triangles with an ordered pixel interlock (IN-345); runs of chunks in one render pass with texture preparation hoisted into a texel atlas (IN-352). Open: CLUT loads and GLM operations still cost two barriers each; double-buffered submission (CPU and GPU overlap) | `hw_cache_equals_writeback` and the GPU fixtures in both interlock modes; race barriers 9,500 -> 3,230 per frame |
| M10 GLM | Started: GLM, decode, swizzle, unswizzle, CLUT, merge and upload kernels (IN-347, IN-349..IN-351) | `hw_glm_decode`, `hw_glm_swizzle`, `hw_glm_unswizzle`, `hw_glm_clut`, `hw_glm_upload_merge`, `hw_glm_host_upload` |
| M11, M13, M16, M18 | Pure references only (lod, replace) | lod, `hw_replace_*` CTest |

## 2026-10-24 reference comparison (IN-385): dither per output pixel, bilinear 1x uploads

The TASK-026 specification team compared the reference renderer with CoreGS 2.0 (prose only, no reference code; see
`Q-GSDX-COMPARISON.md`) and three presentation-only changes came out of it:

- **Dither indexed per output pixel** (the reference's default at scale): our cell covered s x s output pixels, which
  on a 480i field is the owner's 6 x 12 lattice on flat backgrounds. `GT4_HW_DITHER=fine` is now the default;
  `native` restores the old cell. It only reaches CT16 targets, so the menus (CT32) were unchanged: on the 480p race
  capture the modes differ on 38% of pixels by at most 8 levels and the presented mixed-block share rises
  0.786 -> 0.915, with memory byte-identical on all 121 frames.
- **`GT4_HW_SCALED_REFRESH=linear`** for bilinear 1x uploads into a scaled target (the reference's default; ours is
  nearest). The default stays nearest because the 06.7 gate prefers the exact block (28.76 dB nearest against 23.55 dB
  bilinear on the synthetic capture); it is an owner choice.
- **`GT4_HW_SCALED_FILTER=linear`** now also covers textures sampled from a scaled source; it moves 4% of a 6x menu
  frame, so it is not the menu grid either.
- Confirmed by the comparison: our 06.3 half-pixel geometry already matches the reference formula for formula, and the
  reference's slow path is decided by feedback only (DATE, PABE, AFAIL != KEEP, COLCLAMP 0, partial FBMSK), which
  widens what Part B may keep on the fast path (wrap modes, texture formats).

Gates: CTest 245/245 in both interlock modes, race480p memory identical, `--check-presented` 0 low-detail frames at 6x.

## 2026-10-23 scaled images sized to their slot (IN-384, parity plan Part A)
Part A of `PARITY-PLAN.md`, the memory half of the GSdx-parity work: every scaled presentation image now holds only
its slot's real extent instead of a fixed 1024 x 1024 native square.

- **Before:** 11 fixed squares at scale s (6 colour slots, 3 depth slots, the scaled texel image, 2 display shadows)
  plus the 1x copies: 184.5 MB at 2x, 738.2 MB at 4x and 1,660.9 MB at 6x on the 480p race capture, i.e. 151 MB per
  640 x 448 target at 6x.
- **Now:** width = the target's FBW * 64 native pixels, height = the rows the slot loaded or drew (rounded to a page),
  created lazily with a 64 x 32 minimum per slot and grown on demand (old content copied, the GPU waited on both
  sides). Uploads, GLM slot loads, draw chunks and the scaled texture copy grow their images first, so no pixel is
  ever clipped; a texture taken from a slot copies only the batch's sampled footprint (IN-380), which is what keeps
  the race frame from growing to its 1024 x 512 texture view.
- **Device memory on the 480p race capture:** 64.1 MB at 2x, 243.1 MB at 4x, 541.4 MB at 6x (65-67% less);
  `scaled_draws_clipped=0`.
- **Gates:** CTest 245/245 with and without the interlock (new `hw_scale_target_extent`); the five memory captures
  byte-identical (menu 1,101 frames, race 121, car 121, night 121 with `fifo_hash=3a2fe617a174e7e7`, race480p 121);
  scaled memory equal to 1x and 0 low-detail presented frames at 2x/4x/6x; the 06.3/IN-377/IN-379 fixtures unchanged.
- **Reporting:** `stats.scaledImageBytes`, printed as `scaled_image_mb` on the `[hw] cost` line and on the replay's
  `scale=` line with `scaled_image_grows` and `scaled_draws_clipped`.
- **GPU busy on the 480p race replay** (`--gpu-timing`, same capture and recipe, this build against candidate 273):
  - 1x 5.25 (was 5.35), 2x 6.42 (5.99), 4x 9.58 (8.68), 6x 14.77 (16.92) ms.
  - At 6x the scaled pass is 8.04 instead of 9.18 ms; the 1x pass 2.75 (3.24), refresh 0.70 (0.82).
  - Only 6x is outside the run-to-run spread of this machine; the item is mainly a memory win.
- **CPU per frame (replay, same recipe):** 1x gif 8.82 / batch 4.70 / execute 3.41 ms; 6x gif 17.86 / batch 12.84 /
  execute 11.33 ms. Unchanged by this item (the CPU does not see the images).

## 2026-10-23 tone-sequence strip executor (IN-383, parity plan item 2)

Item 2 of `PARITY-PLAN.md`, first step (the strip executor), implemented and gated:

- `hw_tone_strips.comp` writes the eight strip batches of one 64 x 32 frame-buffer page in one dispatch from the GLM
  (index texel -> 07.4 CSM1 palette entry of the strip's quadrant -> CT24 RGB, alpha kept); the page becomes
  GLM-owned like a parked target. Matching is a strict template with the palette base and each strip CBP checked, so
  anything that is not GT4's sequence keeps the normal path, and a page that stops decoding keeps its prefix.
  A collected strip's palette load is held (only the last of the eight can be observed) and recorded before anything
  else reads the CLUT.
- Race 480p replay: 16,940 page dispatches replacing 135,399 strip batches and 118,580 held (101,640 dropped)
  strip palette loads, so GPU CLUT loads 200,467 -> 98,827; 9.95 -> 8.14 ms GS stream, 5.54 -> 4.28 ms batch,
  1.83 -> 0.97 ms translate, 3.52 -> 2.89 ms execute, 1.12 -> 0.75 ms CLUT per frame; GLM swizzles 33,938 -> 18,272.
- Gates: memory byte-identical on all five captures (menu 1,101 frames, race 121, car 121, night 121 with
  `fifo_hash=3a2fe617a174e7e7`, race480p 121); `hw_tone_hle_exact` 0/2 frames differ, 0 pixels off the CPU form of
  the rule, 0 alpha bytes changed, and the refused third page exact; CTest with and without the interlock.
- Switches: `GT4_HW_TONE_HLE=0` / replay `--no-tone-hle`; `GT4_HW_VERIFY_SHUFFLE=1` / replay `--tone-verify`.
- In the game (headless 480p race, same build with the HLE off and on): GS thread 13.4-15.6 -> 10.6-11.6 ms per
  frame, process 32.7-37.3 -> 30.9-33.5 core-ms, render passes 341-347 -> 198-204, barriers 1,591-1,645 ->
  1,437-1,474. GPU busy in the replay (`--gpu-timing`): 7.19 -> 5.35 ms at 1x, 19.01 -> 16.92 ms at 6x, so the
  scaled pass (9.2 ms at 6x) is now the largest single cost and item 1 is the next step.

## Log

- 2026-10-21: branch `task/027-coregs2` from `main` (db99f9e) merging `task/027-hw-renderer` (80c87a2) and the
  cpuref docs checkpoint (03bdd0d). paraLLEl-GS and its pins dropped (decision 0070). `GT4_RENDERER=hw` now rides
  `main`'s GS stream seam through `native/gs_stream/gs_stream_sink.hpp`. Check build slice216 (see
  `docs/BUILD.md`). Handed to the external implementation agent.

## Open questions

- IN-322 (local-to-local DIR mirroring) and IN-320 (composed scanout phase) are open; IN-321 is a negative-result
  record for the garage defect that IN-326 turned out to explain. IN-315 was a frame-pairing error for the VRAM
  comparison; actual scanout phase still needs M6 qualification. IN-316 and IN-326 are resolved (slice222). See
  `IMPLEMENTATION-NOTES.md`.

## Known failures at handover (slice216)

- `hw_gpu_pixel_directed`: resolved on 2026-10-21 in slice217; the 07.5 / IN-07 A-bit priority rule now matches the CPU reference (all three GPU exactness fixtures pass).
- `python_native_contracts` / `test_make_distribution`: refuses the unpinned development executable (expected).

## Next step

**M6 first playable**: wire the present scheduler into the sink for the windowed path, add the HWR frame dumps
(GT4_HW_DUMP_SOURCE=vram|scanout) so the 42-dump schedule can run with GT4_RENDERER=hw, then record the
S-01/S-02 parity (the HWR replay tool) and ask the owner for the first look at 1x. The interface work decided
in IN-331 is done; what remains is the dump/present plumbing, not new pixel behaviour. Then M5 (blending in the raster path: ABE is currently
drawn opaque, IN-500) and M6 (HWR frame dumps from the sink, first owner look at 1x). The front-end equivalent of the old `.inc` copy is not needed: the sink no longer uses donor memory for output. S-03..S-11 still need owner gameplay captures.

## 2026-10-21 replay checkpoint (slice219)

- Confirmed: IN-07 shader warm-up committed; slice217 full native CTest: 177 passed, 1 skipped (178 selected; Python run separately). Python: 627 tests, 44 skipped, one pre-existing distribution-pin error. Default donor headless: 42/42 = slice214, copied card unchanged.
- Confirmed: slice219 standalone CPU reference and composed libraries: 69/69 tests pass, including four new v2 fixtures. Real legacy, envelope-converted v2 and newly recorded donor v2 each replay all 1,101 frames to identical PPMs, with no rejected GIF, skipped texture or FIFO fault.
- Outputs are private/ and build/ **inside this worktree**, honoring the owner's no-writes-to-main instruction. Main assets and old slices remain read-only; no launcher mode/pin changes. Evidence and hashes: BUILD slice219 and the journal.
- Diagnostic only: after aligning capture N to donor N+1, 10/11 sampled menu frames have pct_bad <= 0.038%; donor frame900 still has 29.5487%, PSNR 8.7427 dB. These compare composed CPU output to donor VRAM output, so are not chapter14 qualification (IN-325). IN-315/IN-316 remain open; no field-offset rule was added.

## 2026-10-21 VRAM diagnostic checkpoint (local slice221)

- Confirmed:75/75 CPU-library CTests and2 CLI process tests pass. Python629 tests with44 skips and only the
  pre-existing unpublished-game-executable distribution-pin error.
- CLI defaults to `vram`; `--dump-source scanout` preserves the composed output. `frames.jsonl` records every
  capture-native ID and the selected raw registers, geometry, validity and line-doubling rule.
- Real capture: two raw replays produce1,096 identical PPMs; capture frames0..4 have no enabled circuit. All
  1,101 explicit scanout PPMs remain identical to slice219. No frame IDs or pixels were shifted by the tool.
- Aligned VRAM comparison:10/11 sampled donor frames100..1100 meet pct_bad<=0.5% and PSNR>=40dB; garage900
  remains29.5487%,8.7427dB. Report-only at this milestone, not full scene qualification.
- IN-315 historical offset is explained by wrong frame pairing for these samples (vertical-pairing.json),
  not by a missing VRAM field offset. IN-316 is now a confirmed remaining rendering/memory discrepancy.

## 2026-10-21 A+D XYZ kicks checkpoint (slice222, IN-326 closes IN-316)

- Confirmed: `m3_hw_gif_ad_xyz` (new) and 75 existing CPU/memory/front-end/pixel/display tests pass, 76/76, in a
  fresh standalone `build/slice222-cpuref`. PACKED descriptor 0xE now kicks XYZF2/XYZ2/XYZF3/XYZ3 in the
  canonical register layout (01.1, 02.1, 02.3); bits 72..127, bit 111 ADC included, are ignored for A+D.
- Confirmed cause of IN-316: the missing A+D kicks were the garage defect. Replaying the same donor v2 menu
  capture (`menu-v2.gscap`, SHA-7abb51cf...) through the fixed reference changes exactly frames 811..916 and
  nothing else: 990/1,096 VRAM images and 995/1,101 scanout PPMs are byte-identical to slice221.
- Confirmed: aligned donor sample comparison (capture N = donor N+1) now has 11/11 frames inside the chapter
  14.4 2D thresholds; the garage sample donor900/capture899 went from pct_bad 29.5487% / PSNR 8.7427 dB to
  0.2051% / 45.992 dB. Report only: scene qualification remains M8, and the donor 2D gate is gating from M8.
- Confirmed: the game executable is untouched (`gt4vulkan.exe` 97,291,264 bytes, SHA-3fa40ea1..., the slice217
  binary). The front end is linked only into the CPU reference, not into `gt4_hw_stream_sink`, so the default
  renderer path and the recorded 42/42 headless parity are unchanged by construction.
- Slice217 full native CTest after the change: 188 passed, Q-07 skipped, only the expected pre-existing
  `python_native_contracts` distribution-pin failure. Python: 629 tests, 44 skips, same single expected error.
- Evidence: `private/slice222-in326-evidence/` (vram, scanout, aligned-candidate, comparison JSON, logs).

## 2026-10-21 M4 write-back port checkpoint (slice223, IN-328/IN-329)

- Ported `task/027-m4-gpuref` (e31fcc4) onto this branch: `writeback/` (Vulkan-free drawer interface, `gt4_hwd_*` device table, batch translation, chunked execute with SLM read/write-back, Renderer), the uber vertex/fragment shaders and the backend routing. The dropped `.inc` edit has no equivalent need here: the sink owns presentation and never reads donor memory.
- Confirmed: the WIP did not compile; fixes were include/API drift, a mangled struct fragment at writeback.cpp:228 that closed the namespace twice, a `ClutUnit` constructor argument, a test predicate that compared `word >> 16` with 255 instead of the masked blue channel, and the loader's instance set (surface and swapchain entry points). New `load_headless_entry_points` serves windowless devices.
- Confirmed: 11 fixtures green — `hw_flat_provoking`, `hw_sprite_corners`, `hw_gpu_coverage_vectors` (chapter 03.3 vectors, sub-pixel sprite edges), `hw_gpu_equivalence_{flat,texture,region,bilinear,tfx_fog,depth_alpha,scissor}` (0 mismatching pixels over 1089-1681 compared pixels each) and the report-only `hw_gpu_gouraud_report`.
- Confirmed IN-328: the CPU reference truncated a constant fog of 90 to 89 (double barycentric weights that do not sum to 1), which was the 16-pixel TFX/fog difference. Constant attributes now interpolate exactly; new fixture `cpuref_constant_attributes` (528 covered pixels, exact (80,55,58)).
- Confirmed: full native CTest 200 pass + Q-07 skip + the known `python_native_contracts` distribution-pin failure (202 selected). Default renderer headless 4,200 frames: 42/42 dumps equal slice214, card copy unchanged. Python 629 tests, 44 skips, same single expected error.
- Confirmed: `GT4_RENDERER=hw` now runs the write-back renderer in process: 600 headless frames, 205,791,616 GIF bytes, 2,987 privileged writes, 0 faults, 33 s (about 55 ms per guest frame; one draw per primitive with a barrier at 1x). Nothing is dumped yet, so this is a stability result, not a picture.
- Evidence: `private/slice222-in326-evidence/hw-run/` and `.../default-parity/`. Fresh full build: `build/slice223-coregs2`. No pin, launcher mode, PR or merge.

## 2026-10-21 M5 raster path checkpoint (blend unit through the draw stage)

- Confirmed: the raster (fragment-shader) path now runs the whole pixel pipeline of chapter 08. The translation decodes the ALPHA register, PABE and COLCLAMP exactly as the CPU reference does and passes PRIM.ABE; `hw_process_pixel` was already the bit-exact core, so the only missing piece was the state.
- Confirmed: five new fixtures compare GPU against the CPU reference with zero mismatching pixels: `hw_gpu_raster_blend` (81 modes x COLCLAMP both ways, 13,122 pixels), `hw_gpu_raster_pabe` (both alpha MSB cases), `hw_gpu_raster_atest` (8 methods x 4 AFAIL, 4,896 pixels), `hw_gpu_raster_date` (DATM both values over the written result) and `hw_gpu_raster_frame` (FBA, FBMSK on CT24, DIMX dither on CT16).
- Confirmed: full native CTest 205 pass + Q-07 skip + the known distribution-pin failure (207 selected). Default renderer headless 4,200 frames: 42/42 dumps equal slice214, card copy unchanged.
- Confirmed: `GT4_RENDERER=hw` headless 600 frames with blending enabled: 0 faults, 37.0 s (about 62 ms per guest frame; blending costs about 7% over the previous 33.2 s). No frame dump yet.
- Open: DATE in CT24 writes nothing (read as High confidence in 08.7 and mirrored by both sides); the S-01/S-02 parity replay still needs the HWR replay tool.

## 2026-10-21 M6 presentation interface checkpoint (IN-331)

- Audit against docs/COREGS2-ROADMAP.md: M5 does not conflict. The draw path holds no per-frame target (writes back per chunk) and reads the destination at draw time, so holding a frame or running an auxiliary pass cannot corrupt blending. Two real gaps: presentation composed inside the VBlank and owned no image; and write-back always landed in guest memory.
- Decided and implemented as interfaces: `present::Scheduler` (stamp per frame end, `presentNew` / `presentHeld` / `none`, newest-wins backlog, late-input hook), a reused compose buffer plus the kept shown image, `BeginAuxiliaryFrame`/`EndAuxiliaryFrame` (4 MiB snapshot and restore, readbacks refused, guest display history untouched) and per-pass CPU timers with session totals (`executeGpuUs` reserved at 0).
- Parity: default policy presents every VBlank, so the windowed path and the donor renderer are unchanged; default headless 42/42 dumps equal slice214 with the card copy unchanged.
- Confirmed: 212 CTests, 210 pass, Q-07 skip and only the known `python_native_contracts` distribution pin failure. Five new fixtures cover the scheduler (new/held/dropped, hold off, 2x rate + hook calls), the auxiliary-frame contract (guest SLM hash identical, readback refused, nested begin refused) and the timers (compose measured, draw timers 0 without a device, GPU field reserved).
- Confirmed: headless HWR 600 frames, 0 faults, 33.7 s; per-frame CPU average translate 16.1 ms and execute 34.8 ms, which is the synchronous per-primitive submit that M7/M9 must attack.
- Not done yet: windowed scheduler wiring (tick per host present slot), HWR frame dumps and the S-01/S-02 replay. Deferred deliberately: parity first.

## 2026-10-21 M6 dumps checkpoint (IN-332)

- Implemented `GT4_HW_DUMP_DIR` / `GT4_HW_DUMP_EVERY` in the sink: P6 PPM per interval from `Backend::scanout`, plus a counter line (batches, primitives, skipped textures, empty batches, textures, texels, GPU draws, written pixels, de-indexed, sprites).
- Confirmed: 4,200 headless frames with `GT4_RENDERER=hw`, 0 faults, 42 dumps, card copy unchanged; per frame CPU averages translate 26.6 ms and execute 53.8 ms in this (menu-heavy) schedule.
- Measured against the donor dumps of the same schedule: 42/42 outside the 14.4 2D gate. Boot frame 100 is 1.53% bad (the first text line is missing); from the garage screen on, 42-49% with alternating row bands and content kept from the previous screen, i.e. some primitives never reach VRAM.
- The CPU reference draws the same scenes inside the gate, so the defect is in the GPU draw path, not in the front end or the pixel rules. Next: the HWR replay tool (capture -> GPU renderer -> PPM) to iterate on the garage frame without launching the game, then the draw-order/write-back diagnosis.

## 2026-10-21 HWR replay tool checkpoint (IN-333)

- Added `hw_writeback_replay`: one capture stream -> the GPU write-back renderer -> PPM per frame, using a renderer-neutral `ReplayTarget` shared with the CPU reference reader. 1,101 frames of the real menu capture replay with 133,637 primitives, 0 skipped textures and 0 rejected GIF packets (same counters as the CPU run).
- Confirmed by direct A/B on identical input: every EVEN frame is pixel-identical between the CPU reference and the GPU path; every ODD frame differs by exactly 4,394 pixels, which is the first boot text line the GPU path does not keep. Later frames diverge further (garage 10.6%).
- Diagnosis: not textures (0 refused), not the front end (same batches/primitives), not composition (even frames match). It is field-parity dependent draw loss in the GPU path, the same thing the owner saw in the window as 'one frame right, one striped'.
- Also fixed a misleading counter: `black_presents` now counts only presents without a composed image (it was a leftover from the M0 stub that counted every present).
- Next: root-cause the odd-frame loss in the GPU path with this deterministic replay (candidate: the per-field SCANMSK / clipping interaction in the per-primitive scissor), then re-run the 42-dump schedule.

## 2026-10-21 memory-level diagnosis checkpoint (IN-334)

- Added `--slm-frame N` to both replay tools: they dump the same 4 MiB shadow local memory at the same frame number, so the GPU path and the CPU reference can be compared in memory.
- Confirmed at frame 101 (odd): the SLMs differ in 6,591 bytes, all in the framebuffer pages that hold the boot text (64 KiB pages 3-4). Even frames still match byte for byte.
- Conclusion: the odd-field text loss is a WRITE problem (the primitive does not land in VRAM), not a display/composition problem, and it is tied to the field parity of the input. Ruled out this round: texture refusal (0), the front end (same counters), composition (even frames identical) and the per-primitive scissor (disabling it made the garage worse: 48.8% against 10.6%).
- Next: instrument the draw of the text region on an odd frame (which batch, which rect, which scissor, whether the pixel rules reject it) using the SLM dump as the oracle.

## Handover 2026-10-21 (local227) - continue in Claude

Branch `task/027-coregs2`, worktree `C:\Antigravity\gt4-wt-027-coregs2`, last commit **04daf64**, tree
clean, nothing merged. Read before working: `AGENTS.md`, `AGENT-BRIEF-COREGS2.md`, this file,
`IMPLEMENTATION-NOTES.md` (IN-331..IN-334), and the ai-memory page `decisions/coregs2-implementation`.

### Where the work stands

- M0-M3 done. M4 GPU write-back ported and wired (IN-329/IN-328). M5 pixel pipeline in the raster path
  (IN-330). M6 presentation interfaces (IN-331), HWR frame dumps (IN-332), HWR replay tool (IN-333) and the
  shadow-memory comparison (IN-334) are done.
- Open defect, the reason we are not at parity: on identical recorded input the GPU path is
  **pixel-identical on every even frame** and loses content on **odd** frames. In memory (frame 101) the two
  4 MiB shadow memories differ in 6,591 bytes, all inside the framebuffer pages holding the boot text
  (64 KiB pages 3-4). The garage frame 900 diverges 10.6%. So it is a VRAM **write** problem tied to the
  input's field parity.
- Ruled out, each with a test: texture refusal (0 skipped), the front end (identical counters), composition
  (even frames byte-identical), per-primitive scissor (disabling it made the garage worse: 48.8% vs 10.6%).

### Exact commands

```text
# build the candidate tree (incremental; new candidates get a new build/sliceNNN-* recipe)
cmd /c build\scripts-222\build-game-225.bat

# GPU path replay (add --slm-frame N to dump the 4 MiB shadow memory at that frame)
build\slice217-coregs2\build\hw_writeback_replay.exe private\slice217-coregs2-evidence\menu-v2.gscap <out-gpu> [--slm-frame N]

# CPU reference on the same records (ground truth for this comparison)
build\slice217-coregs2\build\hw_cpuref_replay.exe private\slice217-coregs2-evidence\menu-v2.gscap <out-cpu> --dump-source scanout [--slm-frame N]

# compare (frames, pixels, memory) - committed helper, no local state needed
private\tooling-venv\Scripts\python.exe scripts\hw_frame_tools.py pct <a> <b> 100 101 900
private\tooling-venv\Scripts\python.exe scripts\hw_frame_tools.py slm <a.bin> <b.bin>
private\tooling-venv\Scripts\python.exe scripts\hw_frame_tools.py diff <a.ppm> <b.ppm> <out.png>
private\tooling-venv\Scripts\python.exe scripts\hw_frame_tools.py png <in.ppm> <out.png>
```

Launching the game: `build\hwr-play.ps1` (windowed HWR, card copy, dumps, session log). It refuses to run
while a game is open, and never touches the owner's card.

### Next step (in order)

1. Instrument the draw list for the boot-text rows on an odd frame in both paths (`writeback::Translate`
   and `cpuref::DrawBatch`): primitive type, rect, scissor and the pixel-rule outcome, for frames 100-102,
   then diff the two lists. The SLM dump is the oracle.
2. Fix the divergence found; re-run the replay until all 1,101 frames match in image AND memory.
3. Re-run the 42-dump schedule (`private/slice226-hwr-dumps/run-hw-dumps.py` recipe in the build dir) and
   compare against the donor dumps; target: inside the chapter 14.4 gates.
4. Then the windowed scheduler wiring (IN-331 follow-up), and only then ask the owner for a windowed look.

### Known costs and limits

- Headless HWR costs ~26 ms translate + ~54 ms execute per menu frame (windowed: 33 + 67 ms), because it
  issues one draw per primitive with a barrier. That is why the game runs at ~10 FPS, and the 3D car view
  is slower still. M7 (surface cache) and M9 (batching) must remove it; do not treat it as an M6 bug.
- Lines still use Vulkan rasterisation, not Bresenham; DATE on CT24 writes nothing (08.7, High).
- No pin, no launcher mode, no PR, no merge. The main checkout stays read-only; PCSX2 and the gt4-gs fork
  remain forbidden (clean room, decision 0070).

### Evidence on disk (private, never staged)

- `private/slice227-replay-gpu/{cpu,gpu,cpu-slm,gpu-slm}` and `inspect/` - the A/B that isolates the defect.
- `private/slice226-hwr-dumps/` - 42-dump schedule from the game and the comparison JSON.
- `private/hwr-play/` - the owner's windowed session (dumps + `session.log` with the counters).

## 2026-10-21 M6 odd-frame defect fixed (local slice228, IN-335) - Claude

- Confirmed cause: `ExecuteBatch` wrote back the whole rectangle of BOTH frame and depth words after every chunk,
  even with ZMSK = 1. GT4's 2D passes keep ZBUF at page 0 with Z writes masked while the display double buffer
  alternates FBP 0 / FBP 140, so on every other frame the unused depth buffer shares pages with the buffer being
  drawn; Z32 and CT32 swizzle a page differently and the stale depth words landed on scattered drawn pixels (the
  6,591 bytes of IN-334). Fix: write back only the words a chunk changed, frame word then depth word per pixel.
- The coordinator's lead (odd fields draw with a half-line Y offset, `func_004A2A10` / scratchpad 0x2C0) was
  tested as a Hypothesis and is not the cause: after the fix frame 101 is identical in image and memory, and the
  remaining differences (IN-336) are spread evenly over odd and even frames.
- Same commit aligns the GPU renderer with the CPU reference: `ReadFifo` flushes the open batch, consumes
  LOCAL->HOST data sequentially, counts overruns and sets TRXDIR = 3 when drained; CSR.RESET resets the GS;
  `ResetGs`/`GifReset` clear the pending read and display state like the CPU reference.
- New controls (each fails on the old code where stated): `hw_gpu_writeback_aliased_depth` (1,274/1,176
  mismatching pixels before the fix), `hw_replay_parity_double_buffer` (synthetic double-buffered capture, CPU vs
  GPU image and 4 MiB memory hash per frame: 6/6 frames failed before the fix), `hw_readback_pending_draw`.
  `hw_smoke_window` expectation updated to the IN-333 counter meaning (black_presents=0, composed background).
- Replay tools: `--slm-hashes <file>` (per-frame FNV-1a of the 4 MiB memory, both tools) and
  `hw_writeback_replay --trace-frame N --trace-rect x0,y0,x1,y1` (per-batch state dump).
- Confirmed on `menu-v2.gscap` (1,101 frames): 678 frames byte-identical GPU vs CPU, 672 with identical memory;
  the other 423 frames differ by small amounts on textured/Gouraud triangles (max pct_bad 0.154%, max delta 18),
  IN-336, inside the 2D gate. Before: odd frames lost content (garage 10.6%).
- Confirmed in game, headless 4,200 frames `GT4_RENDERER=hw`: 0 faults, 42 dumps, **42/42 inside the chapter 14.4
  2D gate vs the donor** (frames 100-700 identical, max pct_bad 0.131% at frame 900, min PSNR 47.6 dB); before
  the fix 0/42. Per-frame CPU averages: translate 27.4 ms, execute 61.9 ms (one draw per primitive; M7/M9).
- Confirmed: fresh full build `build/slice228-coregs2` (215 CTests: 212 pass, Q-07 skipped, the expected
  `python_native_contracts` distribution-pin error and `hw_smoke_window`, whose stale expectation is fixed in this
  commit); default renderer headless 42/42 = slice214, card copy unchanged. Evidence:
  `private/slice228-replay/`, `private/slice228-evidence/`.

### Next step

1. Owner test 1 (spec 15.2 M6): windowed look at menus and one race at 1x with `GT4_RENDERER=hw` (slow is
   expected, about 10 FPS). The request goes to the owner in the handback report.
2. IN-336 question to the spec team (oracle for triangle attribute interpolation at 1x).
3. M7 surface cache, then M8 readbacks, M9 batching (the 27 + 62 ms per frame).

## 2026-10-21 M6 output stage: field phase and deinterlace (local slice229, IN-337/IN-338) - Claude

- The renderer composes two images (12.9): `Reference` (the donor-matching merge; dump source `vram`, the 14.4
  gates) and `Presentation` (13.3 step 3 field offset + 13.4 deinterlacer; the window and dump source
  `scanout`). The VSYNC field reaches the renderer; the 13.2 layout is computed once per VBlank; 13.7/V-01-12
  counters print in the sink's `[hw] output:` line.
- IN-337: the adaptive deinterlacer treated every sharp static line as motion (IN-39 compared the bob average
  with the previous output), so the static boot text bobbed one line per VBlank (headless scanout dumps: 2.04%
  of pixels changed between consecutive frames; 1.23% with `off`). Now literal 13.4: compare current vs
  previous same-parity field on the line above, the line itself and below; still lines take the previous
  field's line. Presentation replay of the menu capture: static screens 0.000% between consecutive frames; fades
  change only by their real content; one edge row (row 0 on the odd field) still alternates, as 13.4 passes edge
  rows through.
- `Backend::present` is now `end_frame(phase)` + `present_slot()` (IN-331 follow-up): the host-slot entry point
  for TASK-082/TASK-045 exists, still called once per VBlank on the EE thread (parity unchanged).
- Confirmed: incremental rebuild of `build/slice228-coregs2` (local slice229): 178 HWR/CPU-library CTests pass
  (Q-07 skipped); the 4,200-frame headless `vram` dumps are byte-identical to slice228 (42/42 inside the 2D
  gate). `hw_writeback_replay --compose presentation` added.
- Not done: the headless game scanout run with the corrected adaptive rule (another agent's slice230 headless runs
  were active; the launch guard refused). The replay covers the same frames.

### Next step

1. Owner test 1 (windowed, 1x, slow): menus and one race.
2. M7 surface cache (8 d in the plan; highest risk).

## 2026-10-21 Owner test 1 and the menu field phase (local slice230, IN-339) - Claude

- Owner test 1 (coordinator relay): 55 s windowed, Home menu reached, "the delivery looks right in principle",
  very slow as expected; defect: menus looked combed / not deinterlaced. `frames=0` in the session line.
- Cause (Confirmed): the field-to-row mapping. GT4 draws its CSR.FIELD = 1 frames half a line higher; with the
  IN-35 reading every woven line landed two rows from its partner. Fixed: row field = !CSR.FIELD (IN-339).
  Garage menu combing 5.09 -> 2.04 (reference 2.73); static screens stable; headless game scanout 1,200 frames
  confirms. `frames` now counts windowed VBlanks too.
- Controls: `hw_presentation_field_phase` updated to the IN-339 phase; new capture-gated
  `tests/python/test_hw_presentation_capture.py` (passes; fails with the old phase).
- `hw_writeback_replay --deinterlace <mode>[_bff]` added for these studies.
- Next: owner look at the menus again (same command), then M7.

## 2026-10-21 M7 first step: resident targets and decoded-texture cache (local slice231, IN-340/IN-341) - Claude

- `GT4_HW_SYNC_MODE=cache` keeps one colour and one depth target resident on the GPU between batches; coherence
  by conservative block spans (reads write back drawn ranges, writes drop loaded ranges, aliasing batches fall back
  to the write-back path). Decoded textures are cached and invalidated by per-page write sequence numbers.
- Gate: `hw_cache_equals_writeback` (synthetic capture: depth targets, target switches, feedback texture, upload into
  a drawn target, local->local move, local->host read, CLUT load from drawn memory, colour/depth aliasing; identical
  images and memory hashes per frame in both modes). It caught a real defect while being written (the fallback
  upload overwrote the resident depth target). Menu capture: memory hashes per frame equal the slice228 write-back
  run on all 1,101 frames in both modes.
- Deferred submission (IN-342): chunks are recorded into one command buffer and submitted at readbacks, full rings
  and each VBlank (no wait), instead of one synchronous submit per batch.
- Measured on the menu capture replay (per frame, CPU): write-back mode translate 12.7 / execute 20.9 ms; cache
  mode 15.3 / 5.5 ms (before this step: 15.9 / 33.2 ms). Headless game, 4,200 frames with `GT4_RENDERER=hw` (cache
  is the default): 416 s -> 152 s, 0 faults, 42/42 dumps byte-identical to slice228, card copy unchanged.
- Tests: 179 HWR/CPU-library CTests pass (Q-07 skip); Python 631 tests, 46 skips, only the expected
  distribution-pin error; `test_hw_presentation_capture.py` passes.
- Next M7 steps: display compose read from the resident target (no per-VBlank write-back), two colour slots for the
  double buffer, then target lists per chapter 05; M9: one draw per primitive with barriers is the next cost.

## 2026-10-21 memory addressing performance (local slice232, IN-343) - Claude

- Same addressing, faster: table lookups for the PSM description and the in-page layout, 4-byte slot accesses,
  row-wise `readRect`/`writeRect` used by target loads, write-backs, texture decode and compose.
  `memory_hw_rect_matches_pixels` proves equality with the per-pixel functions on every PSM.
- Headless `GT4_RENDERER=hw`, 4,200 frames: 416 s (slice228) -> 152 s (slice231) -> **76 s**, translate 6.5 ms and
  execute 3.2 ms per frame, 42/42 dumps byte-identical to slice228. Menu capture memory hashes unchanged.
- Tests: 180 HWR/CPU-library CTests pass (Q-07 skip); default renderer 42/42 = slice214, card copy unchanged.
- Next: an owner look at the speed and the menus (`build\hwr-play.ps1`), then the remaining M7 items (target lists,
  display from targets, preload, dirty lists, clears, GPU moves) and M9 batching.

## 2026-10-21 owner test 2 and the 3D cost (local slice234, IN-344..IN-346) - Claude

- Owner test 2 (coordinator relay): 2D menus run in real time and look clean; GT Mode with light 3D is somewhat slow;
  the License Center (3D car) collapses.
- Profiled headless (`GT4_HW_PROFILE_EVERY`, new): the 3D car scene drew 30k primitives per frame as 30k draws with
  barriers and spent most of its time in ~100 target write-backs and ~90 reloads per frame (a single resident colour
  target); the race 1,100 batches, 700 write-backs and 700 reloads per frame, and 143 batches per frame on the
  write-back path because adjacent colour and depth buffers looked aliased.
- Fixed: several target slots with LRU eviction (IN-344), page-exact block spans (IN-344), one draw per chunk of
  triangles with an ordered pixel interlock (IN-345), textures copied on the GPU from resident targets (IN-346).
  Every step keeps memory hashes identical to the write-back mode on the menu, car and race captures.
- Headless in-game (`run-game.py race`, 10,500 frames): car menu 255 -> 100 ms per frame, race 300 -> 113 ms.
  2D menus 14 ms. 42/42 menu dumps byte-identical to slice228; 42/42 race-schedule dumps inside the 14.4 3D gate vs
  the donor (worst pct_bad 0.96%, PSNR 36.0 dB); default renderer 42/42 = slice214.
- Remaining race costs per frame: CPU texture decode ~25 ms; write-backs ~33 ms, of which about 70 per frame are CLUT
  loads from palettes the game draws on the GPU (07.7 GPU CLUT) and about 70 texture reads of frame-buffer pages
  through other formats (09 shuffles / M10 GLM).

## 2026-10-21 GLM, GPU CLUT and GPU-resident memory (local slice235..slice237, IN-347..IN-351) - Claude

- Profiling the race (write-back reason counters, a slot trace): about 12 CPU write-backs and 11 reloads per frame,
  because GT4 reuses the same memory under other widths and formats (BP 2240 depth 640 and 64 wide, BP 6720 frame
  buffer and a 128-wide target inside it, BP 12224 colour and depth), plus about 70 CLUT loads per frame from a
  palette target and several megabytes of texture uploads per frame.
- Built (all exact): the GLM with decode and swizzle kernels (IN-347), small untextured sprites on the CPU (IN-348),
  the GPU CLUT (IN-349), GLM-owned pages so evicted and overlapped targets are parked on the GPU and loaded back from
  the GLM, uploads into them merged on the GPU, swizzles skipped when nothing changed (IN-350), and EE uploads
  written into the GLM by a kernel (IN-351).
- Gates: per-frame memory hashes identical to the write-back mode on the menu (1,101 frames, = slice228), car (121)
  and race (121) captures; 223/224 CTests (the one failure is `python_native_contracts`: the rebuilt
  `gt4vulkan.exe` differs from its pin, no pin changes on this branch); the hw tests also pass with
  `GT4_HW_TEST_NO_INTERLOCK=1`; headless menu 42/42 dumps byte-identical to slice228; race schedule 42/42 identical
  to slice235 and inside the 3D gate vs the donor; default renderer 42/42 = slice214.
- Race replay per frame (no hashing): CPU write-backs 12 -> 1 (the display compose), uploads 27.5 -> 0.25 ms, GS
  stream 45 -> 17 ms. Headless in-game race: 65 -> 38 ms per vsync; of the 32 ms GS stream about 14 ms is waiting for
  the GPU and about 11 ms is the GIF front end.
- Measured: skipping all barriers (wrong output, experiment only) cuts the GPU wait at the display from 15.2 to
  3.4 ms per race frame. The 7,100 barriers per frame (one per chunk, two per GLM decode, CLUT load and upload) are
  the GPU cost; the next M9 step is several batches per render pass with texture preparation hoisted per run.
- Spec question for the TASK-026 team (IN-350): 04.9 keeps targets authoritative and the GLM a pure function of
  SLM + targets. Parking moves a target's content into the GLM, which then owns those pages until a CPU user needs
  them. Is that acceptable as the M7/M10 coherence model, or should the parked content stay in a target image?

## 2026-10-21 runs and CPU front-end savings (local slice238, IN-352/IN-353) - Claude

- Runs (IN-352): interlocked chunks share one render pass; texture preparation goes into a texel atlas before it.
  Race replay barriers 6,900 -> 3,230 per frame, display wait 15.6 -> 8.7 ms.
- CPU (IN-353, found with a local sampling profiler): the CPU CLUT unit no longer copies entries while the GPU CLUT is
  authoritative, its write tracking has a range early-out, palettes and the 02.9 vertex trace are computed only when
  needed. Race replay GS stream 17.3 -> 11.5 ms per frame.
- Gates: memory hashes identical on the three captures; 224/224 non-Python CTests in both interlock modes (new
  `m3_hw_clut_copy_free`); menu 42/42 dumps byte-identical to slice228; race schedule identical to slice235 and inside
  the 3D gate; default renderer 42/42 = slice214.
- Headless in-game race: 35 -> 25-26 ms per vsync (65 ms at the start of the day's 3D work). About 7.6 ms per frame is
  still the CPU waiting for the previous frame's GPU work (one command buffer); next: double-buffered submission so
  the CPU records frame N+1 while the GPU draws frame N.

## 2026-10-21 double-buffered submission and asynchronous presentation (local slice239, IN-354/IN-355) - Claude

- IN-354: two sets of command buffer, fence and rings; the CPU records frame N+1 while the GPU draws frame N.
  Headless race 25-26 -> 17-18.6 ms per vsync (about 55-58 FPS without a window; GS stream about 11-12 ms of it).
- IN-355: with a window, the displayed memory is downloaded asynchronously inside each frame's submission and the
  next VBlank composes it (one frame of presentation latency), so the CPU never waits for the frame it just
  submitted; the present no longer waits for the whole device. Race replay with presentation composes 31 -> 22 ms
  per frame. Not run windowed here (rules); the owner's test is the check, with `-SyncPresent` as a fallback.
- Gates: memory hashes identical on the three captures; 224/224 non-Python CTests in both interlock modes;
  async-presentation fixture; menu 42/42 dumps byte-identical to slice228; race schedule identical to slice235 and
  inside the 3D gate; default renderer 42/42 = slice214.
- Ready for owner test 3: License Center 3D car, GT Mode garage and a Time Trial race
  (`& "C:\Antigravity\gt4-wt-027-coregs2\build\hwr-play.ps1"`).

## 2026-10-22 owner test 3, disk, aspect (local slice240, IN-356) - Claude

- Owner test 3: License Center 3D car locked 60 FPS; Nordschleife Time Trial 39-42 FPS, lower facing the sun
  (lens-flare readbacks: 7 transfer write-backs per frame, each a submit and wait; GIF front end 13.4 ms on heavy
  frames). The window showed 16:9 squeezed into 4:3.
- Disk: about 10.7 GB of local intermediates deleted (slice229/230 replay and evidence dirs, slice232/234/237/238
  evidence, PPM folders of replay outputs, race profile frame dumps, build dirs slice224/225). Kept: the
  slice228 build and its gate references (slice228 menu dumps and replay hashes, slice233 captures and donor race,
  slice235 race schedule, slice239 evidence), every dir this file names.
- IN-356: presentation into the 16:9 rectangle of 13.6; `hw_smoke_window` checks it. Ultrawide / TASK-080 fill:
  later, in the CoreGS 2.0 ultrawide and UI-layer work (hook: the present rectangle in `record_frame`).
- Next: M8 readbacks starting with the flare (P0 per-call-site logging), then the GIF front end and the CPU compose.

## 2026-10-22 M8 flare readback (local slice240, IN-357..IN-359) - Claude

- Night schedule added to the local run script (FRAME-COST-STUDY night race, flare read every frame from VBlank 9,000);
  night capture `private/slice240-capture/night-10000.gscap` (121 frames, 121 FIFO reads, donor-recorded) with
  write-back reference hashes `night-10000-wb.txt`.
- P0 found the cost: every FIFO read wrote back every resident target (7 per frame, each a submission and wait). Now
  only the transfer's source is made coherent, through one batched GLM download (IN-357); the recording is
  submitted every 64 chunks so the GPU keeps up (IN-358); GPU timestamps per category (IN-359).
- Headless night race in game: 20-28 -> 11-19 ms per vsync (heaviest 500-frame interval 18.9 ms; most below 16.7).
  Flare replies byte-identical in every mode; memory hashes identical on all four captures; 224/224 non-Python
  CTests in both interlock modes; menu 42/42 dumps byte-identical to slice228; race schedule identical to slice235
  and inside the 3D gate; default renderer 42/42 = slice214.
- GPU busy about 8.2-9.4 ms per night frame. The remaining cost of the heaviest frames is CPU: the GIF front end
  (about 7-8 ms of the GS stream on the heaviest interval) and translate/execute (about 4.5 ms).
- Next: the GIF front end (step 3), then the CPU compose; the GPU-driven flare (R3) remains a later, producer-side step.
- IN-360 (CPU front end, part 2): headless without dumps, night race 10.4-17.2 ms per vsync (race start with the full
  field is the 17.2 interval), day race 15.2-16.7 ms. Gates as above (local slice241), 225/225 non-Python CTests.
  Next: the presentation compose (3-4 ms of CPU per windowed frame).

## 2026-10-22 480p test setup and compose (local slice242, IN-361) - Claude

- Owner request: tests boot like the shipped launcher, 16:9 and 480p races (menus stay 480i). `GT4_FORCE_480P=1`
  is the launcher's mechanism; its hold was windowed-only and `hwr-play.ps1` did not pass it, so owner test 3 raced
  at 480i. Now `hwr-play.ps1` passes `GT4_FORCE_WIDESCREEN=1` and `GT4_FORCE_480P=1` and prints the `[display]`
  hold line and `[hw] video: vblanks_480i/vblanks_480p`; the hold also runs headless. Performance runs from now on
  use the 480p race (`run-game.py ... race ... GT4_FORCE_480P=1 GT4_FORCE_WIDESCREEN=1`, capture
  `private/slice242-capture/race480p-10000.gscap` with write-back hashes); the gate references stay 480i.
- 480p race headless: 19.6-21.5 ms per vsync (480i: 15.2-16.7). Replay: about 1,930 batches, 1,660 CLUT loads and
  1,860 GLM decodes per frame; GPU busy about 11.5 ms; GS stream 17.6 ms (front end about 11 ms).
- Compose 3.3 -> 2.5 ms (IN-361). Next: the 480p race front end and GPU CLUT/decode costs.
- IN-362 fewer Vulkan calls per run and dispatch; IN-363 the GS stream runs on its own worker thread (same calls in
  the same order; FIFO reads and window calls drain it first). Headless 480p race (the launcher's mode):
  19.6-21.5 -> 14.4-14.8 ms per vsync. Night race and gates: see BUILD local slice243.

## 2026-10-22 owner test 4, GPU compose, plans (local slice244, IN-364/IN-365) - Claude

- Owner test 4 (night race 480p, 4 min 43 s, windowed): 60 FPS on the overlay, flares correct, no readback drops,
  16:9 and 480p confirmed (`vblanks_480p=13738`); frametime spikes of about 18 ms because the GS worker took about
  16.8 ms per frame (gif 12-13 ms + compose 2.55 ms + present 0.48 ms).
- IN-364: progressive presentation frames are composed on the GPU from the GLM and copied into the swapchain on the
  GPU (no CPU compose, no download, no latency); interlaced menus keep the CPU path. IN-365: fewer per-chunk copies.
  Mid-frame submission interval re-measured in game (64 / 128 / 256 chunks): 64 stays best (submissions are not the
  cost). Expected windowed worker on the heaviest night frames: about 13-13.5 ms (headless 480p night 10-13 ms).
- Next CPU target: the GPU CLUT loads (about 3 ms per 480p race frame, two barriers each).

### Step 2 assessment: GPU-driven flare (study R3 / S4)

Not reachable inside the renderer alone. The flare table reaches VU1 through a DMA REF into EE memory; the renderer
only sees the GIF stream that VU1 produces afterwards. R3 needs, on the producer (game runtime) side: (1) recognising
the flare readback (`func_00497EF8` / `func_004A5178` at `0x00497F30`) and the REF tags into its buffer in the VIF1
path, (2) feeding VU1 the full-visibility constant (`D_006AF5D0`) instead of the read bytes, (3) bracketing the flare
sprites' GIF output with markers carrying (table texel, lane), and (4) the P1 check that the VU1 flare program uses the
value only as a linear colour/alpha factor. The donor runtime (DMA, VIF1, MTVU) is frozen and changes go through overlay
capsules (`scripts/prepare_perf_runtime.py`), so this is a separate runtime task (proposed: TASK-087 step S4 with a
runtime-overlay writer). With IN-357/IN-358 the flare readback costs about 0.3-0.9 ms per night frame and stays exact
and same-frame, so R3 is now an optimisation and a prerequisite for side-effect-free auxiliary frames (roadmap 2), not
a frame-rate blocker.

### Step 3 plan: upscaling (M12, chapter 06), 1x parity first

The current design keeps resident targets as exact 1x GS words (`R32_UINT` storage images, exact pixel arithmetic in
the fragment shader), and the GLM, parking, unswizzle loads, GPU CLUT and display compose all work on 1x words.
Upscaling therefore has to be added as a second representation, not a change of the 1x one:

1. **Scale option and ceiling (06.1):** `GT4_HW_SCALE` 1..8 (already parsed), s_max from the device, the launcher's
   display plan k = ceil(max(W/640, H/448)) x render scale mapped onto it. s = 1 must leave every gate unchanged.
2. **Scaled slot images:** each resident slot gets an s x s image next to its 1x words (or the slot image becomes
   s x and the 1x words are derived by the top-left sub-sample of 06.8/10). Draws rasterise at s x with the 03.3
   position mapping and the 06.3 half-pixel modes (`native` default for GT4); points/lines at native thickness; dither
   indexed by native pixel (06.5).
3. **Coherence at scale:** write-backs, GLM swizzles and readbacks take the top-left sub-sample (06.8), so the 1x memory
   model, the gates and the readbacks stay exact. Parking keeps the scaled image alive (a scaled "shadow" keyed by view)
   so GT4's buffer reuse does not collapse the frame to 1x; a reload from the GLM upsamples only when no scaled shadow
   exists (counted).
4. **Textures from targets at scale (06.8):** target textures sample the scaled image with normalised coordinates;
   index reinterpretation (T8 from CT32) reduces to 1x first; hazard copies at scale with a 1-pixel border.
5. **Display at scale:** the GPU compose (IN-364) samples the scaled circuit images; the present scales to the window.
6. **Box downsample (06.7) and native post-effects (06.6, default off).**
7. **Acceptance (06.9, 14.4):** every 1x gate unchanged at s = 1; box-downsampled 2x/3x/4x frames vs 1x with the PSNR
   thresholds recorded; owner look at 3x/4x (no seams, bloom aligned, no edge halos).

Order: 1 -> 2 (colour/depth at scale, write-back by sub-sample) -> 5 (see it) -> 3 -> 4 -> 6 -> 7. Then step 4
(anti-aliasing: FXAA as a present pass first, SMAA from the roadmap).


## 2026-10-22 S1 strip decoder and cost metrics (local slice245, IN-366/IN-367) - Claude

- IN-366 (study S1): the GIF parser decodes a whole PACKED STQ+RGBA+XYZF2 strip run in one loop and hands the vertices
  to the front end in one call; the front end keeps its vertex queue in a fixed array, re-evaluates its flush rules
  only after a register change, and skips the setup of kicks that draw nothing. Generic path kept as fallback
  (`FrontEndOptions::fastPaths`). Gates: memory hashes on all five captures identical (night `fifo_hash=3a2fe617a174e7e7`,
  121 reads), menu dumps 42/42, race calibration identical, default parity 42/42, 227/227 non-Python CTests (with and
  without interlock); new fixture `hw_front_end_fast_paths` (40 random streams with ADC vertices, repeated A+D writes,
  material, offset and scissor changes and random splits; fast and slow worlds must give the same log and counters;
  83,673 fast-path vertices). Headless 480p race 10.8-12.9 ms per frame, 480p night race 8.1-12.0 ms.
- IN-367: a `[hw] cost` line next to `[hw] profile` (same interval): CPU ms of the thread that runs the GS stream,
  CPU core-ms of the whole process, render passes, barriers and MB uploaded/read back, per frame.
  `GT4_HW_GPU_TIMING=1` roughly doubles the GS thread cost (timestamp queries): never measure CPU with it on.

### Efficiency table (headless, launcher video mode, slice245, no GPU timing)

| Scene | GS thread CPU ms/frame | Process CPU core-ms/frame | Render passes | Barriers | Upload MB | Readback MB | GPU busy ms |
|---|---|---|---|---|---|---|---|
| Menu (480i) | 0.5 | 1.9 | 1 | 220 | 0.66 | 0 | 0.5 |
| 480p race (Nordschleife) | 10.7-11.1 | 27.7-29.0 | 330-345 | 5,180-5,270 | 5.5 | 0.002 | 11.7-12.1 |
| 480p night race (Route 5) | 12.4-15.4 | 28.8-38.5 | 380-430 | 5,220-5,730 | 4.0-4.7 | 0.008 | 11.9-13.0 |

Budget (owner): GS worker <= 6 ms and renderer CPU <= 8 core-ms per 480p race frame. The GS worker is about 2x over.
The process column includes the EE and VU1 emulation threads (about 17-23 core-ms), so the renderer share is the GS
thread plus the driver's own threads. Car view not yet measured (no headless car schedule in this build). GPU busy is
taken from a separate `GT4_HW_GPU_TIMING=1` run. Spinning: none in the renderer by default (`GT4_HW_READBACK_SPIN` is an off-by-default
diagnostic). Largest remaining costs: about 5,200 barriers and 330-430 render passes per frame, the GPU CLUT loads
(about 1,660 per frame, two barriers each, about 3 ms of GS thread CPU) and batch translate/execute (about 4.8 ms).

Mobile note: the fast path relies on fragment shader interlock (ordered pixel access). Mobile GPUs mostly lack it, so
phase 2 will need another fast path (for example a tile-local path). PC focus now; nothing mobile-specific is built.

## 2026-10-22 fewer barriers, decode reuse (local slice246, IN-368/IN-369) - Claude

- IN-368: repeated full barriers with nothing in between are skipped (480p race: about 5,200 -> 4,050 barriers per
  frame). IN-369: a run reuses an identical GLM texture decode (15% of decodes in the 480p race; about 0.8 ms less GPU
  time per frame). GS thread CPU unchanged at about 10-11 ms per 480p race frame: recording barriers is cheap; the CPU
  cost of the GPU CLUT loads is elsewhere (next: defer CLUT loads and apply several in one dispatch).
- Gates: hashes identical on all five captures (night `fifo_hash=3a2fe617a174e7e7`), menu dumps 42/42, race frames
  42/42 identical, default parity 42/42, 227/227 non-Python CTests with and without interlock.

## 2026-10-22 deferred CLUT loads (local slice247, IN-370) - Claude

- IN-370: GPU CLUT loads are kept pending until the GLM or the CLUT buffer is touched; an indexed texture whose
  palette the last load wrote reads it straight from the GLM. 480p race replay: CLUT dispatches 1,657 -> 300 per
  frame, barriers about 4,090 -> 1,630 per frame, GPU busy 10.7 -> 6.9 ms per frame, GS stream 11.6 -> 9.6 ms.
- Headless with the launcher's video mode (`[hw] cost`): 480p race GS thread 10.3-11.2 ms per frame (unchanged within
  run-to-run noise in game, although the replay drops 2 ms), 480p night race 6.2-10.3 ms (was 12.4-15.4 ms).
- Gates: hashes identical on all five captures (night `fifo_hash=3a2fe617a174e7e7`), menu dumps 42/42, race frames
  42/42 identical, default parity 42/42, 227/227 non-Python CTests with and without interlock.

### Efficiency table update (slice247, headless, launcher video mode)

| Scene | GS thread CPU ms/frame | Process CPU core-ms/frame | Render passes | Barriers | Upload MB | Readback MB | GPU busy ms |
|---|---|---|---|---|---|---|---|
| 480p race (Nordschleife) | 10.3-11.2 | 26.6-30.0 | 330-350 | 1,520-1,630 | 5.5 | 0.002 | 6.9 (replay) |
| 480p night race (Route 5) | 6.2-10.3 | 17.1-28.3 | 230-430 | 1,100-1,940 | 3.2-4.7 | 0.008 | not measured |

Still over the owner's 6 ms GS-worker budget in the race. Largest remaining CPU parts (replay): batch translate
1.8 ms, execute 3.3 ms, front end about 2.4 ms, and 330-430 render passes per frame.

## 2026-10-22 owner test 5 and the merge of main 1.2.2 (local slice248/249) - Claude

- Owner test 5 (slice247) approved: flat 60 FPS in the Nordschleife Time Trial, a night race and Opera Paris, 0 dropped
  frames, picture as GSdx at 1x. Measured windowed: GS thread 8.9-11.3 ms CPU per frame (gif 8.7-10.0, translate
  1.6-1.8, execute 3.1-3.5), process 21-27 core-ms, 1,600-1,800 barriers and 315-340 render passes per frame, GPU
  nearly idle. GSdx's own CPU thread: about 1.7 ms per night frame.
- `main` 1.2.2 merged. TASK-084's VBlank journal now holds the VSYNC for the hw sink as well and posts it after the
  frame's VU1 output (`hw_stream_sink_deferred_vsync`). Identity: hw memory hashes on the five captures, menu dumps
  42/42 and race frames 42/42 identical to slice247's references; CTest 230/230 non-Python with and without
  interlock; default renderer 42/42 = slice214 after fixing the 1.2.2 donor-mode crash (indeterminate
  `mtvu::Donor::post_vsync`; main needs the same fix).
- Local builds: slice248 = merged source on the slice247 recipe (coverage, slice191 capsule, slice206 guard);
  slice249 = the release 1.2.2 recipe (aspect, slice262 capsule and guard). Build trees on
  `O:\gt4recomp-scratch\coregs2-slice248-merge` and `coregs2-slice249-r122`; local gate helper `build/gates2/gates.sh`.
- Next: M12 upscaling (step 3 plan above), then anti-aliasing.

## 2026-10-22 M12 upscaling and FXAA (local slice250, IN-371..IN-376) - Claude

- Design (IN-371): the 1x words stay the only authority. With `GT4_HW_SCALE` s > 1 every target slot gets a second
  image at s x s sub-pixels per native pixel (native extent 1024); every draw is rasterised a second time into it
  (06.3 native mode, scissor moved by floor(s/2) sub-pixels), every load of 1x words refreshes it. Memory, GLM,
  readbacks (night flare `fifo_hash=3a2fe617a174e7e7`), `vram` dumps and every 1x gate are identical at 2x, 4x and 6x.
- Presentation (IN-372): a scaled GPU compose reads the displayed buffer's scaled image (06.3 edge band handled);
  the displayed buffer is evicted last at scale (IN-373); textures copied from targets are sampled at scale (IN-374,
  GT4's CT24 -> CT16 frame conversion); the present reduces large frames by 2x2 box steps before the final blit
  (IN-375). Menus (480i, CPU presentation) stay 1x for now.
- FXAA (IN-376, 13.5): `GT4_HW_POSTFX=fxaa`, own edge-only implementation, on GPU-composed frames.
- 06.9 metric on the 480p race capture (box downsample vs 1x): 40.0 dB (2x), 40.4 (4x), 40.2 (6x).
- New controls: `hw_scale_exact` (exact 06.3 expectation for flat sprites and a 1:1 target copy at 2x-4x, both
  interlock modes), the scale-3 block of `hw_cache_equals_writeback` (memory equals 1x; box PSNR >= 25 dB on that
  noise-pattern capture), `hw_postfx_fxaa`, `hw_smoke_window_scaled`, the `upscaled` dump source option.
- Cost (headless, not windowed; noisy): 480p race at 6x GS thread 10.7-11.8 ms CPU per frame (unchanged from 1x
  within noise), about 15-16 ms per vsync; replay GPU busy (with timestamps) 6.4 ms at 1x, 12.8 at 4x, 14.8 at 6x,
  17.8 at 8x: the scaled pass is the new GPU cost; 6x is near the 16.7 ms budget.
- Ready for owner test 6: `build\hwr-play-scale.ps1 -Scale N [-AA]` (frozen copy `O:\gt4recomp-scratch\coregs2-candidate-250`).
- Next: owner verdict at 2x-6x vs GSdx; scaled presentation for the 480i menus; textures decoded from the GLM at scale
  where they are targets (mirror); scaled-pass GPU cost; GS-thread CPU (gif front end).

## 2026-10-23 owner test 6 fixes: presented frames at scale, ultrawide fill (local slice270, IN-377/IN-378) - Claude

- `main` 1.2.3 merged (1ca15bc; conflicts: `mtvu_runtime.hpp` comment, journal 2026-10-22).
- Owner test 6 (slice250, 4x + FXAA, 3440 x 1440) failed: the race looked 1x while the GPU did the 4x work, and the
  ultrawide race stayed in a 16:9 pillarbox.
- IN-377, upscaling (Confirmed on the 480p race capture): GT4 shows each of its two CT16 frame buffers the VBlank
  after it finished it, and in between it uses the first 1,024 blocks of that buffer as the Z32 buffer of a small
  256-wide target. The depth slot load dropped the shown buffer's colour slot, so every second presented frame was
  composed from the GLM at 1x (61 of 121 on the capture; the game log had 447 of 1,005 composes "from GLM"). Fix:
  display shadows (scaled image plus 1x words of a recently shown buffer, saved when its slot is dropped); the scaled
  compose uses a shadow sub-pixel only where the GLM still holds the saved word, else 1x. Replay `--check-presented`
  now checks the presented frame itself: 1x-sourced frames 61 -> 1 of 121 at 2x/4x/6x (the one left is the capture's
  first frame, drawn before the capture started); memory hashes equal to 1x at 2x/4x/6x.
- IN-378, ultrawide: the TASK-080 fill request went to a GS process that does not exist in hw mode. The in-process
  presenter now takes it (`InProcessSink::display_aspect`) and fills the largest centred rectangle of the race's display
  ratio, the same rectangle as the default renderer; menus stay 16:9.
- New controls: `hw_scale_display_shadow` (fails without the fix), `hw_smoke_window_aspect_{169,ultrawide,219,329,219_in_169}`,
  `hw_stream_sink_display_aspect`, ratio vectors in `hw_present_rect`; gate helper runs `--check-presented` at 2x/4x/6x.
- GPU busy per frame, 480p race replay, timestamps on, no presentation compose: 1x 6.5 ms, 2x 9.5, 4x 13.3, 6x 15.6-16.3.
- Gates (local slice270, release recipe: aspect, slice262 capsule and guard): CTest 241/241 with and without interlock;
  memory hashes identical on all five captures (night `fifo_hash=3a2fe617a174e7e7`); menu dumps 42/42 = slice228; race
  frames 42/42 = slice235; default renderer 42/42 = slice214; `--check-presented` 1 of 121 at 2x/4x/6x.
- Candidate for owner test 7: `O:\gt4recomp-scratch\coregs2-candidate-270` (`gt4vulkan.exe` sha256
  `b878160b537ba0df9dc515ab11203a1921f379f9af2f58ccc451b606b481e827`), `build\hwr-play-scale.ps1 -Scale 2|4|6 [-AA]`.

## 2026-10-23 owner test 7 fixes: the presented race at scale (local slice271, IN-379/IN-380) - Claude

- Owner test 7 (candidate 270, 2x and 6x+AA) failed: the race still looked 1x (stair-steps on the 640 grid, soft HUD
  text) while the GPU did the scaled work; the ultrawide fill was correct. `--check-presented` had passed because a
  1x picture with a scaled HUD on top still has some mixed s x s blocks.
- Causes (Confirmed with the replay batch trace and dumps of the scaled images): (1) GT4's per-frame RGB tone curve
  (09.4 variant A: T8 reads of the CT32 frame into a temporary page, T8-textured write-back as CT24) runs at 1x through
  the GLM and left the frame's scaled image as the 1x result; (2) the full-screen passes (the CT16 display conversion
  and an earlier feedback pass) sample the 640 x 448 frame through a 1,024 x 512 REGION_CLAMP texture, and textures
  from targets required the whole power-of-two extent loaded, so they always read the GLM at 1x.
- Fixes: IN-379 tone carry (save the frame's scaled image at the sequence start; at its end make the CT32 view
  resident again and rebuild its scaled image through the per-channel table its 1x words imply, per-pixel checked,
  else 1x); IN-380 textures from targets need only the batch's sampled footprint loaded.
- Check: `--check-presented` now also counts low-detail frames (mixed-block share below 20%): candidate 270 about 6%
  on every frame; slice271 69% (2x), 77% (4x), 79% (6x); 1 low-detail frame of 121 (the capture's first). New fixture
  `hw_scale_fullscreen_passes` (copy and tone cases, 2x and 4x, both interlock modes); each case fails with its fix
  disabled.
- Gates (local slice271, built in the slice270 tree): CTest 242/242 with and without interlock; memory hashes identical
  on all five captures (night `fifo_hash=3a2fe617a174e7e7`); menu dumps 42/42 = slice228; race frames 42/42 =
  slice235; scaled memory equal to 1x at 2x/4x/6x; default renderer 42/42 = slice214.
- Candidate for owner test 8: `O:\gt4recomp-scratch\coregs2-candidate-271` (`gt4vulkan.exe` sha256
  `8f1c4a66bb274ee37ba0c3ddc01aeee3a725361e904737b0a7f162870218db29`), `build\hwr-play-scale.ps1 -Scale 1|2|4|6 [-AA]`.

## 2026-10-23 owner test 8 passed; 480i menus at scale, scene sweep, cost and parity plan (local slice272) - Claude

- Owner test 8 (candidate 271, 6x + AA): race at real resolution, approved; the beta minimum (upscaling and AA) is met
  in races. New owner requirement: CoreGS 2.0 must reach at least GSdx's performance at the same scale (owner reading
  at 6x: gt4-gs 37% GPU load, 1,357 MHz, ~41 W; candidate 271 ~105-120 W at ~3,300 MHz).
- IN-381, 480i menus at scale: GPU compose of interlaced frames at scale with the field offset and a GPU 13.4
  deinterlacer in native rows (`hw_deinterlace_scaled.comp`). Fixture `hw_scale_interlaced_present` (exact against
  the CPU 1x deinterlacer for auto/weave/bob/blend at 2x and 4x; sub-pixel detail from a drawn sprite). Menu capture
  `--check-presented` (detail now measured against each frame's own 1x edges): candidate 271 behaviour
  (`--no-gpu-compose`) 922 low-detail frames of 922 with edges; slice272 323 at 2x and 163 at 6x. The remaining
  low-detail menu frames are the intro logo movie (frames ~300-650; Hypothesis: IPU video uploaded as 1x pixels, 1x by
  nature) and menu fades (~30 frames per transition; Hypothesis: 09.4 variant B alpha stashes, not carried yet).
- Scene sweep (headless): 480p race (El Capitan, launcher settings) and the night schedule at 2x through the game,
  plus the five captures through replay. Findings:
  - the old 480i captures (`race-9000`, `car-6600`, `night-10000`) present at 1x: their display reads 448 rows of a
    224-row field buffer, so no scaled source covers the read rectangle (480i races only; the launcher forces 480p);
  - car-select and track-select menus in the live sweep: low detail, combing on the rotating 3D car (Hypothesis: the
    same 224-row field-buffer layout or variant B fades);
  - not reachable headless (owner test 9): rain, rally/dirt, replays, photo mode, split screen, in-car mirror view.
- IN-382 (safe win): no scaled draws or refreshes while GT4's tone sequence runs (its scaled result is rebuilt by the
  carry): presented frames byte-identical, memory unchanged, scaled passes 348 -> 61 per race frame.
- Timing categories split (12.8): `pass` (1x draws), `scaled`, `refresh`, plus decode, clut, swizzle, upload and
  `download` (readbacks and the presentation compute).
- Gates (local slice272 in the slice270 tree): CTest 243/243 with and without interlock; memory hashes identical on all
  five captures (night `fifo_hash=3a2fe617a174e7e7`); menu dumps 42/42 = slice228; race 42/42 = slice235; default
  42/42 = slice214; race `--check-presented` 0 low-detail frames at 2x/4x/6x, memory equal to 1x; menu memory equal to
  1x at 2x and 6x.
- Candidate for owner test 9: `O:\gt4recomp-scratch\coregs2-candidate-272` (`gt4vulkan.exe` sha256
  `6a0506e174944bcac7a830d422d90fb6615518f90c249f540534aba142145c4d`).

### Clean measurement (2026-10-23, no game, GS process or build running; checked before and after every run)

GPU ms per frame, replay with timestamps (timestamps add a little), before -> after IN-382:

| Capture | Scale | Busy | 1x pass | Scaled pass | Refresh | Decode + CLUT + swizzle + upload | Presentation compute |
|---|---|---|---|---|---|---|---|
| race 480p | 1x | 6.6 -> 7.1 | 3.0 -> 3.4 | - | - | 3.6 -> 3.7 | 0.0 |
| race 480p | 2x | 10.0 -> 8.7 | 3.3 | 2.8 -> 1.5 | 0.14 -> 0.12 | 3.6 -> 3.5 | 0.26 |
| race 480p | 4x | 14.0 -> 11.9 | 3.6 -> 3.5 | 5.8 -> 4.2 | 0.43 -> 0.35 | 3.7 -> 3.5 | 0.44 |
| race 480p | 6x | 18.3 -> 17.1 | 3.6 -> 3.7 | 9.9 -> 8.5 | 0.78 -> 0.72 | 3.5 -> 3.5 | 0.59 -> 0.67 |
| night (old 480i) | 6x | 17.1 -> 16.4 | 3.5 | 9.6 -> 9.0 | 0.81 -> 0.83 | 2.8 | 0.3 |

CPU per frame in the game (headless, launcher settings, last 600-VBlank interval of the race), after IN-382: GS thread
10.6-11.3 ms at every scale (before 12.0-13.5; run-to-run noise about +-1 ms), process 27-28 core-ms (before 29-34);
render passes per frame 344 (1x) and 403 at scale (before 690); barriers about 1,640 (race) and 1,990 (night).

Where it stands: 6x is at 17.1 ms GPU busy against the 16.7 ms frame budget (over it with timestamps, borderline
without); 2x and 4x are inside. The GS thread is at about 11 ms against its 6 ms budget.

### Parity plan with GSdx (ranked by expected gain)

1. Fast scaled raster: the scaled pass is half of the 6x frame (8.5 ms). It runs the exact pixel pipeline with fragment
   interlock and storage-image read-modify-write for every one of the s x s samples. Draw the scaled copy into normal
   attachments with fixed-function blending and depth where the GS state maps to them, keeping the interlocked path for
   the rest. Expected: scaled pass 3-4x cheaper (8.5 -> about 2-3 ms at 6x). Rule: the 1x authority is untouched; it
   relaxes only the bit-exact 8-bit arithmetic of the presentation-only scaled image (CoreGS 2.0 keeps its 1x gates;
   the 06.3 scaled fixtures would get a tolerance). Owner decision needed.
2. Tone-sequence HLE at 1x (spec 09.4 policy 2): one exact compute pass instead of about 1,540 batches and 280 runs per
   frame. Expected: GS thread about 11 -> 4-6 ms (the sequence is about 80% of the frame's batches), 1x pass, decode,
   CLUT and barriers down by most of their tone share (about 2-3 ms GPU). Must stay memory-exact (temporary page, CLUT
   state), verified with `GT4_HW_VERIFY_SHUFFLE` and the memory gates. No rule relaxed.
3. Fewer render passes and barriers: with 2 done only about 40 runs per frame remain; merge consecutive runs (larger
   texel atlas ring) for about 0.5-1 ms.
4. Cheaper refreshes (0.7 ms at 6x): refresh only what a presentation or scaled texture reads.
5. Skipping the 1x redraw where nothing reads the words would save the 1x pass (3.6 ms) but relaxes the 1x-authority
   rule (readbacks would come from downsampled scaled images). Not recommended.
