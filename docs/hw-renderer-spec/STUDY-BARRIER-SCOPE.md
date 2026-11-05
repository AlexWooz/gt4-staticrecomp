# CoreGS 2.0 study: how the GPU is synchronised and fed (TASK-182 step 1), and a ranking of exact changes

Date: 2026-11-05. Branch `task/182-barrier-scope` (base `release/1.3.0-rc15` `02315e95`). Agent status: **not PCSX2-tainted**
(TASK-027 implementation agent; no PCSX2 sources, fork, `private/tooling/pcsx2-*` or `private/slice189-pcsx2-ref` opened; the
only outside input is the clean-room prose `ORACLE-GSDX-TONE-READBACK-FLUSH.md`, sections D2-D4 and "What CoreGS 2.0 could
do", item 1). Step 1 measures and classifies; **no rendering changed, no barrier change implemented, no timing A/B**.

Labels: **C** Confirmed (code read, or a count/hash from a run named here), **H** High confidence, **Hyp** Hypothesis,
**U** Unknown. All counts are per VBlank, means over VBlanks 4800-9600 of the press scenes (the race window; a cumulative
snapshot every 600 VBlanks, so the window is 8 snapshots). Counts do not depend on timing, so a loaded machine does not
disturb them. The only timings here are in section 6 and are indicative (the desktop was shared with an Android emulator and
builds).

## 1. Summary

- **Full (all-commands) barriers:** Hong Kong **566**, Motegi **240** per VBlank, the same at 2x, 10x and 12x (C: the totals
  match to the second decimal at every scale). By hazard (C):
  - target images (read-after-write): 224 and 123, that is **40% and 51%**; the commands are `dirty patch` (105, 29), `patch (cpu
    words)` (60, 46), `self copy` (34, 31), `fill`/`clear` (17, 15). Every one of them declares the **whole image**.
  - the single GPU CLUT buffer: 175 (**31%**) in Hong Kong, 34 (14%) in Motegi; `palette row`, `clut from target`, `clut write`.
  - GLM pages: 104 (18%) and 30 (13%).
  - the hazard at the start of a draw flush (`draws`): 46 and 41 (8% and 17%).
  - the 512-pending overflow: **0**. Readback (`Reduce`, `ReduceMany`): 2 and 1.3. Uploads (`glm upload`): 1.3 and 0.1.
- **Render passes:** 52 (Hong Kong) and 47 (Motegi) per VBlank, 7.8 and 6.6 batches per pass. **65% and 65% end because a
  `self copy` needs the pass closed** (34 and 31 per VBlank, 1.7 batches per pass); 13% end for a `clear`, 5-6% for a `fill`,
  8-10% for the submission cadence (every 64 batches), 2% for the VBlank flush, 1.5% for a readback. **No pass ends for a
  target switch (0) and none for a layout transition (0: every image stays in `VK_IMAGE_LAYOUT_GENERAL`).**
- **Fragment (in-pass) barriers with pixel interlock (this desktop):** 33 and 28 per VBlank, 89-96% "texture is another
  target". **Without it (`GT4_HW_INTERLOCK=0`, the path of most mobile GPUs): 1,103 (Hong Kong) and 593 (Motegi)**, 97% and 95%
  "a primitive may share a pixel with an earlier one". The same exact result either way (flare hash unchanged).
- **Descriptors:** 800 (Hong Kong) and 337 (Motegi) `vkUpdateDescriptorSets` calls per VBlank, 4,139 and 1,781 writes. The
  transient sets are 59% decode sets in Hong Kong. **Pool exhaustion forced 0 submissions** (C).
- **Outside draw passes (section 6):** in a 480i menu, presenting at 2x, the GPU spends 0.68 ms per VBlank of which draw passes
  are 0.10 ms; at 10x it spends 7.2 ms of which draw passes are 0.7 ms and compose/deinterlace 5.1 ms. The non-draw share is
  large on the desktop too (85-90%), and scales with the output pixel count; the absolute ms are small on an RX 9060 XT.
- **Ranked changes (section 7):** R1 draw the self copy inside the open pass (removes about two thirds of the pass ends and
  the `self copy` full barriers); R2 region-granular hazards for target images; R3 CLUT hazards by entry range; R4 barrier
  scope instead of all-commands; R5 per-image tracking of fragment barriers; R6 persistent descriptor sets; R7 GLM below the
  8 KiB page; plus the compose/deinterlace/present items of section 6.

## 2. What was added (diagnostics only)

All in `native/hw_gs/core2/`. Nothing records a command differently; everything counts or timestamps.

| Switch | What it adds |
|---|---|
| `GT4_CORE2_BARRIER_TRACE=1` | A `[core2] sync vblank=N <section> <key>: count=...` line per row at every 600-VBlank summary, cumulative: `fragment_barrier` by reason, `pass_end` by reason (with batches, commands and fragment barriers of the ended passes), `flush` (what asked for the deferred draws to be recorded), `submit` by caller (waited or not), `set_allocation` by kind, and a `totals` line (full and fragment barriers, passes, batches, draw calls, descriptor binds, pipeline, vertex-buffer, viewport, scissor and push-constant records, descriptor update calls and writes, submissions). The existing `barriers by reason` line is finer: image hazards say `raw`, `waw` or `war`, and a barrier no command labelled is named after the function that recorded it. With the switch off the old always-on counting of barrier reasons is gone (it only fed this line), so a normal run does a little less work. |
| `GT4_CORE2_GPU_CATEGORIES=1` | A `[core2] gpu categories ms/vblank draw_passes=... compose_deinterlace=... present=... uploads_copies=... compute=... readback=... start_gap=... unattributed=... (gpu_busy=...)` line. One bottom-of-pipe timestamp (no barrier) is written at each change of kind of work; a kind owns the time from its mark to the next mark (or the recording's end). Draw flushes mark `draw_passes`; `compose`, `Deinterlace`, `Fxaa` mark `compose_deinterlace`; `PresentScale` marks `present`; the transfer and upload commands mark `uploads_copies`; fills, decodes, patches, palette rows, CLUT work and tone mark `compute`; the host-read functions mark `readback`. |
| CTest `hw_core2_sync_trace` | Each switch on against off over the cache fixture and the GPU palette fixture: images and GS memory of every frame identical (exact). |

**How `gpu_busy` is measured today (C, code).** Each recording writes a top-of-pipe timestamp when it starts and a
bottom-of-pipe timestamp when it ends; `gpu_busy` is the **sum over submissions of (last minus first timestamp)**
(`wait_slot`, `stats.gpuNs`), read when the slot's fence signals, in the 600-VBlank line divided by 600. So it **counts the idle gaps
inside a submission**, and because the first timestamp is top-of-pipe, two submissions queued back to back can overlap and
count the same time twice (H). It does not include anything outside the Gpu's command buffers: the blit of the
presented image to the swapchain happens in `hw_gs_backend.cpp` on its own command buffer and is in no counter (H, from the
code; the owner's log can confirm: `present` below is the Gpu's own `PresentScale` into the output-size image, not the
swapchain blit). `gpu_draw_passes` is the sum of up to 64 draw-pass (start, end) timestamp pairs per submission.

## 3. Method

- Build: `build/t178-spec2` (Spec II, plain Release, SSE4.1; the TASK-178 configure recipe `build/t178-configure.cmd spec2`).
  - Base executable (rc15 source with the TASK-178 O3 switch off, built before this change): SHA256 `05d1a922a11abb30c7579be06dbba9a14090b6d7b9214eefe31b8549a0d0918e`.
  - Candidate (this change): SHA256 `a5c581c0f1b4c69262d5fd1a5cee249d070ca6d6ea10e00b178838305fd974c6`. Scene runs and hash gates used the candidate; the 2x baselines used the base.
- Scenes and presses: the TASK-178 press scenes (`private/task178-tools/scenes.sh`, `run_scene`): Spec II Hong Kong
  (reverse) and Motegi Road Course B-Spec, headless, unpaced, `--frames 9876`/`9860`, the card
  `spec2-replays-20261104.ps2`. Command shape: `SCALE=<s> run_scene <hk|mot> <name> <exe> GT4_CORE2_BARRIER_TRACE=1 [GT4_HW_INTERLOCK=0]
  [GT4_CORE2_PERFORMANCE=1]` with `GT4_HW_PROFILE_EVERY=600 GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1
  GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9`. Logs: `build/evidence182/press-trace-*` (ignored by git).
- Tables: `private/task182-tools/sync_table.py <log> 4800 9600` (differences of two cumulative snapshots, per VBlank).
- **Identity (C).** The flare reply hash after the run, base against candidate and against the TASK-178 references:

| Run | Replies | Hash | Reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | base run at 2x: same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | TASK-178 O3 run `-k0`: same |
| Hong Kong 12x | 6835 | `7a427a3ccaeb9d4d` | same |
| Hong Kong 2x, no interlock | 6835 | `2f4c1cc9f174d00e` | same as 2x with interlock |
| Motegi 2x | 6588 | `7bf932fbb8799965` | base run at 2x: same |
| Motegi 10x | 6588 | `9ab7085aa7c6d425` | same |
| Motegi 12x | 6588 | `6370c79dfe9d9bcc` | same |
| Motegi 2x, no interlock | 6588 | `7bf932fbb8799965` | same as 2x with interlock |
| Motegi 2x Performance mode, with and without interlock | 6588 | `a0d73fbfe507a325` | equal to each other (Performance changes the picture on purpose, so it has no normal-mode reference) |

- **CTest (C).** Full run on the candidate (`ctest -j 4`, desktop under load): 316 of 318 passed, 1 skipped
  (`hw_replace_q07_pcsx2_dumps`), 2 failed under load and **passed alone**: `vu_recomp_corpus` (the VU1 corpus differential,
  "program did not end" budget, 92 s alone) and `hw_queue_lock` (19 s). The new `hw_core2_sync_trace` (added after that run) passes,
  as do `hw_core2_gpu_clut` and `hw_core2_self_texture` in the second run. The diagnostics are behind environment switches; with
  both off the only change to a normal run is that barrier-reason bookkeeping, which only fed the trace line, no longer runs.

## 4. Full barriers (question 1)

Every FULL barrier is `ALL_COMMANDS` to `ALL_COMMANDS`, memory write to memory read and write, recorded outside a render pass.
Window 4800-9600, 2x; the same at 10x and 12x (C). The "command" is the label of the `touch` that needed it, or the function.

| Class | Hong Kong | Motegi | Commands (Hong Kong / Motegi) |
|---|---|---|---|
| image, read after write (`image raw`) | 224.3 | 123.0 | dirty patch 104.9 / 28.7; patch (cpu words) 60.2 / 45.8; self copy 33.8 / 30.6; fill 9.3 / 7.2; clear 8.1 / 8.1; glm write (target) 4.7 / 1.5; clut from target 3.3 / 1.2 |
| CLUT buffer (RAW) | 113.2 | 25.3 | palette row 62.2 / 11.6; clut from target 40.4 / 10.5; clut write 10.6 / 3.2 |
| CLUT buffer (WAR) | 61.9 | 9.0 | clut from target 50.0 / 4.3; clut write 11.9 / 4.7 |
| GLM pages (RAW) | 91.1 | 26.5 | dirty patch 36.2 / 17.2; glm write (target) 25.0 / 2.7; decode 19.5 / 4.5; clut from target 10.4 / 2.1 |
| GLM pages (WAR) | 13.0 | 3.5 | glm write (target) 11.7 / 3.5; glm upload 1.2 / 0.1 |
| the hazard at the start of a draw flush (`draws`) | 45.5 | 41.1 | the deferred draws read or write what a pending compute or transfer command wrote or read |
| image write-after-write, write-after-read | 1.9 | 0.6 | decode 1.5 / 0.6; dirty patch 0.3 / 0 |
| 512-pending overflow | 0 | 0 | |
| readback | 2.0 | 1.4 | `Reduce` 1.6 / 1.3, `ReduceMany` 0.4 / 0.1 |
| begin of a recording, new image clear (functions) | 12.9 | 9.4 | `begin_recording` 7.2 / 5.6, `clear_new_image` 5.7 / 3.8 |
| total | 565.7 | 239.8 | |

Reading:
- **The hazards are real but over-declared (H, from code):** `dirty patch` reads and writes `{image}` and `clear`/`fill` read
  and write `{frame, depth}`, always the whole image, though a patch or a fill covers a rectangle. Two patches into
  different corners of the same target are ordered. A palette row declares the CLUT buffer as a whole, so two rows with different
  entries are ordered too. This is the room for R2 and R3.
- **Not the cause (C):** the 512-pending overflow, uploads and readbacks.
- **The count is scale-independent (C):** the dependencies come from the guest's draw order, not from pixels.
  The study STUDY-FRAME-DOWNLOAD-COST quoted about 660 (George V) and 683 (Hong Kong) at 6x; here Hong Kong is 566 over
  4800-9600 at every scale (the earlier figure was a different window, or a spike; not reconciled: U).

## 5. Render passes, fragment barriers, descriptors (questions 2-4)

### 5.1 Render pass ends, 2x (C)

| Why the pass ended | Hong Kong | Motegi | Batches per pass |
|---|---|---|---|
| `self copy` (reads what a draw wrote): copy of the sampled rectangle | 33.8 | 30.6 | 1.7 / 1.9 |
| `clear` of a frame or depth (reads what a draw wrote) | 6.8 | 6.4 | 6.5 / 3.8 |
| the submission cadence (`Draw`, every 64 batches) | 5.3 | 3.9 | 35.7 / 33.8 |
| `fill` (compute fill) | 2.9 | 3.0 | 35.2 / 25.9 |
| `clut from target` | 1.0 | 1.1 | 4.3 / 5.3 |
| the VBlank flush (`Submit (renderer)`) | 1.0 | 1.0 | 3.0 |
| readback (`Reduce`, `ReduceMany`) | 0.9 | 0.6 | 2.7 / 3.5 |
| dirty patch, glm write, palette row, decode, patch | 0.7 | 0.2 | |
| target switch | 0 | 0 | |
| layout transition of a sampled image | 0 | 0 | |
| barrier | 0 | 0 | a barrier never ends a pass here |
| **total** | **52.3** | **46.7** | **7.8 / 6.6** |

The render pass has no attachments (storage-image writes, one subpass with a self-dependency; the optional colour-attachment path is
off on this device), so the passes in the table cover the deferred draw batches only; compute and transfer commands are
outside them (C). A `self copy` is a `vkCmdCopyImage` of the sampled rectangle plus border into a pooled image, which needs the pass closed.
Over the run, copies average 8,300 px (Hong Kong) and 11,800 px (Motegi) at 2x (`self_copy_mpx` over `self_copies`), small.
Whether the copied rectangles are disjoint from what the batch draws (the oracle's path 2, no copy at all): **U**, no counter yet.

### 5.2 Fragment barriers (C)

| | with interlock (2x) | no interlock (`GT4_HW_INTERLOCK=0`, 2x) |
|---|---|---|
| Hong Kong | 32.7 (another target 29.2, the frame itself 2.0, depth peek 1.5) | **1,103.2** (primitive may share a pixel **1,070.5**, another target 29.7, frame 2.0, depth peek 1.0) |
| Motegi | 27.9 (another target 26.9, depth peek 1.0) | **593.4** (primitive 565.5, another target 26.9, depth peek 1.0) |
| Motegi Performance mode | 27.7 | 569.1 (primitive 541.4) |

Without the interlock the draw calls become 1,409 (Hong Kong) and 821 (Motegi) against 410 and 306 batches: each overlapping
primitive is its own draw after a barrier. The phone's figure (about 750 fragment barriers, 75 full barriers, 20 passes per VBlank
in the Nordschleife in Performance mode) is the same order as Motegi without interlock here (569, 253, 46); the full-barrier and
pass counts are 3x and 2x the phone's, so the scenes are not the same (U: the Nordschleife was not run here; step 2 should run
the trace on the phone).

"Texture is another target" (about 27-29 per VBlank): a batch that samples a target waits for **any** earlier draw of the pass,
whether or not that draw wrote the sampled target (code: `draw.textureFromTarget && drewSinceBarrier`).

### 5.3 Descriptors, binds, submissions (C)

| Per VBlank, 2x | Hong Kong | Motegi |
|---|---|---|
| transient sets allocated: decode / draw / compute | 472 / 233 / 95 | 109 / 162 / 67 |
| `vkUpdateDescriptorSets` calls / descriptor writes | 800 / 4,139 | 337 / 1,781 |
| descriptor-set binds / pipeline binds / vertex-buffer binds | 410 / 187 / 52 | 306 / 151 / 47 |
| viewport / scissor / push-constant records | 61 / 396 / 96 | 55 / 292 / 91 |
| submissions (cadence 5.3 / 3.9, VBlank flush 1.0, waited readbacks 1.0 / 0.7) | 7.2 | 5.6 |
| submissions forced by descriptor-pool exhaustion or a ring | **0** | **0** |

The draw descriptor set is cached per recording by its four images, so every submission rebuilds the sets of the batches after
it (233 draw sets for 410 batches in Hong Kong). Nothing is pushed or updated per draw otherwise: binds are already skipped when
unchanged (IN-458).

## 6. GPU time outside draw passes (coordinator's request, from the owner's S23 logs)

**The finding to explain.** In 480i menus the phone's normal mode at 2x reports `gpu_busy` 13.7-16.8 ms and `gpu_draw_passes` 1.0-2.0 ms.

**Where the desktop stands.** A presenting run (`--window`, `GT4_OUTPUT_SIZE=3088x1440`; the window's client area came out
2858x1333, the same shape, because the work area of the 3440x1440 monitor is smaller), Spec II, the first six presses of the
Hong Kong timeline then idle in the menu, 90 s, unpaced, `GT4_CORE2_GPU_CATEGORIES=1`, the desktop's RX 9060 XT; mean per VBlank
over the last intervals (a 600-VBlank line each, stable to 0.02 ms). **Indicative timings** (the desktop was shared with other
work; one run each):

| Scale | Mode | draw passes | compose + deinterlace | present (`PresentScale`) | uploads/copies | compute | unattributed | `gpu_busy` |
|---|---|---|---|---|---|---|---|---|
| 2x | normal | 0.10 | 0.22 | 0.35 | 0.00 | 0.02 | 0.00 | 0.68 |
| 2x | Performance | 0.09 | 0.20 | 0.34 | 0.00 | 0.02 | 0.00 | 0.65 |
| 10x | normal | 0.73 | 5.06 | 1.18 | 0.00 | 0.22 | 0.00 | 7.19 |
| 10x | Performance | 0.68 | 5.10 | 1.17 | 0.00 | 0.21 | 0.00 | 7.16 |

- **The non-draw share is large on the desktop too (C for the numbers, H for the reading):** draw passes are 15% (2x) and 10%
  (10x) of `gpu_busy`. The absolute cost is small at 2x (0.58 ms) and grows with the output pixel count: from 2x to 10x the
  compose/deinterlace time is 23x for 25x the pixels. So the cost is bandwidth over full-output images, which is the kind of
  work a tile-based GPU does worst (Hyp). The phone's 12-15 ms against this desktop's 0.58 ms is a ratio of 20-26x for the same
  work at 2x; the desktop cannot say how much of it is the same passes (U), but there is no other large category here.
- **Normal and Performance mode are the same in menus (C):** Performance mode changes races only.
- **Unattributed is 0 and the start gap is 0 on the desktop (C):** with timestamps tiling the submission there is no idle inside
  a menu submission; the phone's 12-15 ms is therefore not idle gaps if its timings behave the same (Hyp; Adreno timestamps can
  differ).
- **What is not measured (U):** the blit of the output image to the swapchain (`hw_gs_backend.cpp`, own command buffer) is in no
  counter, and the headless runs (the whole TASK-178 bench) never reach compose, deinterlace or present (`presents=0`), so they
  say nothing about this share.
- **Compose/deinterlace is two things (U):** the category merges `compose`, `Deinterlace` (up to two dispatches over the
  display image plus an image copy, a full barrier after each) and `Fxaa`. Splitting it needs one more category; step 2.

Candidate changes for this section are in R8 below. None is measured.

## 7. Ranked changes (each exact, each with an A/B switch)

"Exact" here: every read-after-write and write-after-read pair on the touched region stays ordered, so the memory and images
are bit-identical (gate: the flare reply hash, the frame and state comparisons of TASK-178 9.2, and CTest). Gains are
**hypotheses**; the counts say how many things there are to remove, not what each costs. Tile-GPU gains need the phone to
confirm.

| # | Change | Narrows | Exactness | Gain, desktop | Gain, tile-based GPU | Risk | Switch |
|---|---|---|---|---|---|---|---|
| R1 | **Draw the self copy inside the open pass.** A tiny fragment (or compute-free) pipeline writes the sampled rectangle plus border into the pooled image as a storage-image write, ordered with a fragment barrier (draw to draw), instead of `vkCmdCopyImage` after closing the pass. Falls back to the copy on the colour-attachment path. | 34 / 31 of 52 / 47 pass ends; the 34 / 31 `self copy/image raw` full barriers and, Hyp, most of the `draws` ones (the batch reads the copy); one flush per copy | A copy of words; the barrier orders the writes of earlier draws to the source and the later read by the batch (RAW) and the next write of the same rectangle (WAR). Bit-identical | Hyp: 34 fewer passes, 34 fewer full barriers, 34 fewer deferred-flush boundaries per VBlank; a few tenths of a ms at 2x | Hyp: the largest: passes 52 to about 18 (47 to 16); the 20 passes of the phone's Nordschleife are probably this chain too (U) | Low-medium: new tiny pipeline and descriptor set; copy and draw must agree on the border and on scaled addressing | `GT4_CORE2_SELF_COPY_DRAW=0` |
| R2 | **Region-granular hazards for target images.** `AccessSet` records rectangles (in 64x64 output-pixel tiles) instead of a whole image; `dirty patch`, `patch (cpu words)`, `fill`, `clear`, `self copy` and the flush hazard test declare their rectangles; two accesses conflict only if they share a tile. A command that cannot name a rectangle stays whole-image. | `image raw/waw/war` 224 / 123 per VBlank (40% / 51% of full barriers), part of `draws` (46 / 41) | Conservative rectangles (scissor boxes and sampled areas padded to tile size; anything unknown is whole-image) never miss a real pair | Hyp: the largest barrier count to remove; fewer drains; a ms or more in Hong Kong spike frames (the study's hypothesis, unmeasured) | Hyp: medium-large (fewer full barriers between small patches) | Medium-high: the rectangle of every reader (texture sampling with bilinear border, mip levels, palette rows) must be right; needs an A/B on a stream that overlaps patches on purpose | `GT4_CORE2_REGION_HAZARDS=0` |
| R3 | **CLUT hazards by entry range.** The GPU CLUT buffer (512 entries) is tracked as ranges: a palette row reads the entries of its CSA and storage mode, a CLUT load or write names the entries it writes. | `clut buffer` + `war`: 175 / 34 per VBlank (31% / 14%) | The entry range of each CLUT access is exact from CSA, CPSM and the load size | Hyp: moderate in Hong Kong, small in Motegi | Hyp: moderate | Low-medium | `GT4_CORE2_CLUT_RANGES=0` |
| R4 | **Scope the remaining full barriers** from `ALL_COMMANDS` to the stages and accesses that matter (compute and transfer as source and destination, fragment shader writes of the draws as source), and to one `VkImageMemoryBarrier2`/buffer range when the hazard is on one image or the GLM/CLUT buffer. Also carry the pending-access sets over a submission boundary so `begin_recording` (7.2 / 5.6 per VBlank) needs no barrier. | all 566 / 240 (scope), the 12.9 / 9.4 function barriers | Same hazards, narrower masks. Cross-submission order still needs a barrier unless the pending sets say nothing is pending | Hyp: small by itself (a drain is a drain), it lets later work overlap | Hyp: small-moderate; ALL_COMMANDS includes vertex and binning work that nothing needs | Low | `GT4_CORE2_BARRIER_SCOPE=0` |
| R5 | **Fragment barriers by image.** For a batch sampling another target, wait only if an earlier draw of the pass wrote that target (track written image serials since the last barrier). With interlock off, the 1,070 / 565 `primitive may share a pixel` barriers stay (they are real overlaps; tightening them needs a better disjointness test or the colour-attachment path, see R10). | 29 / 27 per VBlank now; not the 565-1,070 | A later draw reading a target no earlier draw wrote cannot see a hazard | Hyp: small | Hyp: small-moderate (in-pass barriers are expensive) | Low | `GT4_CORE2_FRAGMENT_BY_IMAGE=0` |
| R6 | **Persistent descriptor sets.** Keep draw sets across submissions (the key already carries the image serials) and give decodes a per-image set or push descriptors; write a set only when its images change. | 233 / 162 draw sets and 472 / 109 decode sets per VBlank; 800 / 337 update calls, 4,139 / 1,781 writes | Pure CPU bookkeeping | Hyp: CPU only, small-medium on the GS thread in Hong Kong | Hyp: driver cost of descriptor allocation and writes can be much higher on mobile | Low | `GT4_CORE2_PERSISTENT_SETS=0` |
| R7 | **GLM hazards below the 8 KiB page** (by block run). | `glm pages` + `war` 104 / 30 per VBlank | Page to block precision, rectangles already exact | Hyp: small | Hyp: small | Medium (page bookkeeping is per page everywhere) | `GT4_CORE2_GLM_BLOCKS=0` |
| R8 | **Compose, deinterlace and present at output size** (section 6): fuse the deinterlace weave, compose and present into fewer full-output passes and skip a pass when its inputs did not change (a static menu repeats the same source). Needs the compose/deinterlace split of step 2 before it is ranked. | up to 5-6 of 7.2 ms at 10x in menus on the desktop | Fusing is exact; skipping an unchanged frame is exact for a stateless pass, not for deinterlace modes with history | Hyp: large in menus at high scale | Hyp: the 12-15 ms of the S23 menus | Medium | `GT4_CORE2_FUSE_POST=0` |
| R9 | **Raise `GT4_CORE2_SUBMIT_EVERY` on tile GPUs** (existing switch): the cadence ends 5.3 / 3.9 passes per VBlank; not exact by construction (submission boundaries never change what is drawn, TASK-178 9.2). A/B only. | 5.3 / 3.9 pass ends | Submission boundaries do not change results | none | Hyp: small | None (existing) | `GT4_CORE2_SUBMIT_EVERY=<n>` |
| R10 | **More batches through colour attachments (IN-481)** on devices without interlock, so ROP order replaces the per-primitive barriers (565-1,070 per VBlank). The attachment path is off here; which batches it takes and why is a separate study. | the no-interlock fragment barriers | ROP order is the GS order per pixel | n/a (interlock is on) | Hyp: the largest for the 593-1,103 fragment barriers | High | existing `GT4_CORE2_ROP` family |

Order of attack for step 2 (cheapest, exact, biggest first): **R1** (one new pipeline, removes two thirds of the pass ends),
then **R3** (small bookkeeping, 31% of Hong Kong's barriers), then **R2** (the biggest barrier count, the most careful), with
**R4** and **R6** as low-risk additions. Step 2 should first add two counters (the disjointness of each self-copy rectangle
against its batch's draw rectangle, and the compose/deinterlace split), run the trace on the phone, and A/B each switch on the
Avell and the S23 (not on this shared desktop).

## 8. Unknowns

- U: whether the S23's counts are Motegi-like (here without interlock: 569 fragment, 253 full, 46 passes; the phone: 750, 75, 20 in the
  Nordschleife). The trace works on any device (`GT4_CORE2_BARRIER_TRACE=1`); the phone run decides R1's share of the 20.
- U: the share of self-copy rectangles disjoint from their draw (the oracle's path 2).
- U: how much of the S23's 12-15 ms the compose/deinterlace/present passes are, and the swapchain blit (outside `gpu_busy`).
- U: the time each barrier costs; the counts here rank the things to remove, not their cost. The TASK-178 profile (section 9.4 of
  STUDY-FRAME-DOWNLOAD-COST) isolates commands with barriers and cannot give it either.
- Hyp: R1's pass reduction brings the desktop's 52 passes near the phone's 20 only if the chain is the same one.

## Step 2a: R1, the self copy drawn inside the open render pass

Date 2026-11-05, same branch and the same **not PCSX2-tainted** status as step 1 (no PCSX2 source, fork,
`private/tooling/pcsx2-*` or reference tree opened; the only outside input is the clean-room prose
`ORACLE-GSDX-TONE-READBACK-FLUSH.md`, D3). This section records the implementation of row R1, the diagnostic it added and
its gates. Timing is not measured: the desktop is shared and the gate is counts plus identity.

### What changed (C, code)

- **`core2_self_copy.vert` / `.frag`** (new): a full-screen triangle and a fragment stage that stores
  `imageLoad(source, texel)` into the destination at the same texel. Both images are the core 2 target format (r32ui GS
  words), so a word keeps its pixel, exactly what `vkCmdCopyImage` moved. Built by the pinned glslang like every core 2
  shader (no new tool).
- **`Gpu::Draw`** (`core2_gpu.cpp`): with `GT4_CORE2_SELF_COPY_DRAW=1` the spec 8.8 copy is recorded as a draw inside the
  render pass the earlier batches opened (`Impl::record_self_copy`), after a fragment barrier (earlier stores to the
  source), and the batch that samples the copy is recorded right after it in the same pass, ordered by its own
  texture-from-target fragment barrier. The copy vitals are identical to the transfer's: the same clipped native
  rectangle times the scale, at the same offsets, into the same pooled image.
- **`Impl::flush_draws`** gained `keepPassOpen`: the flush that used to end the pass at the copy now leaves it open (and
  skips the pass end), and later batches continue in it. A pass left open is closed by the next `outside_pass` /
  `end_render_pass` of whatever command follows (all of them still call it), and if a full barrier is needed while a
  pass is open, `flush_draws` ends the pass before the barrier (a barrier inside a pass is illegal).
- **The transfer stays** wherever the draw cannot apply, and each reason is a plain condition in `Gpu::Draw`: the
  colour-attachment path (IN-481: a sampled image may be an attachment of the open pass), `GT4_CORE2_LAZY_BARRIERS=0`
  (which records a full barrier per command), a rectangle outside the pass's framebuffer scissor, and the default (the
  switch unset or 0).
- **WAR across passes**: a pooled copy image is reused from pass to pass. `copyImagesUnordered` is set when a copy is
  drawn and cleared by any full barrier; a copy that has to begin a new pass (a compute or transfer command ended the
  previous one) records a full barrier first, so the next write of the pooled image is ordered against the previous
  pass's readers. Inside one pass the two fragment barriers cover RAW and WAR.
- **Diagnostics** (`GT4_CORE2_BARRIER_TRACE=1`): `[core2] sync ... self_copy <key>: count=` rows for
  `drawn in the pass, overlapping the batch`, `vkCmdCopyImage, overlapping the batch` and, from the renderer
  (`Gpu::NoteSelfSampleNoCopy`), `no copy: sampled rectangle disjoint from the drawn one` and
  `no copy: frame untouched (IN-495)`.
- **CTest `hw_core2_self_copy_draw`** (new): a synthetic stream with two copied self-sampling sprites (a half-size
  reduction that meets its draw, and a 1:1 pass) and one whose sampled rectangle is disjoint from what it draws, at 1x,
  2x, 3x and 12x, switch off against on: images and GS memory bit-identical, the 1x memory equal to the CPU reference,
  two copies drawn in the pass with the switch on and none with it off.

### The disjoint question (C)

**The oracle's D3 path 2 is already there, upstream of the copy.** `ProcessBatch` takes the copy only when the sampled
footprint meets the batch's drawn rectangle (`core2_renderer.cpp`, the spec 8.8 block, `Intersect(textureFootprint_,
t.rect)`); otherwise the batch samples the target itself. So the copy path never sees a disjoint rectangle by
construction, and the counters say exactly that: **none of the copies is disjoint** (0 in both scenes, either path), and
**the disjoint case is already handled without a copy**: 2.00 per VBlank in Hong Kong (5.4% of the 37.35 self-sampled
batches), 0 in Motegi. IN-495's "frame untouched" case is another 1.00 per VBlank (2.7% / 3.1%). **Implementing path 2
at the copy site would save nothing** (C for these two scenes, H for others); what remains of the self-copy problem is
the 92% / 97% of self-sampled batches that really do overlap, which is what R1 addresses. The new fixture forces a
disjoint sprite and gets its row (1 per replay), so the counter can see it when it happens.

### Counts (C)

Window (4800, 9600] of the press scenes, per VBlank, `private/task182-tools/sync_table.py`; one build
(`build/t178-spec2`), `GT4_CORE2_BARRIER_TRACE=1`, the switch off against on, Spec II headless, 2x.

| Per VBlank | Hong Kong off | Hong Kong on | Motegi off | Motegi on |
|---|---|---|---|---|
| render passes | 52.31 | **23.81** | 46.71 | **20.30** |
| pass ends: `draws recorded for self copy` | 33.76 | 0 | 30.56 | 0 |
| full barriers | 565.73 | 505.51 | 239.78 | 184.07 |
| fragment barriers | 32.71 | 97.51 | 27.90 | 87.96 |
| draw flushes | 52.31 | 23.09 | 46.71 | 19.57 |
| submissions | 7.22 | 7.22 | 5.58 | 5.58 |
| self-sampled batches | 37.35 | 37.35 | 32.02 | 32.02 |
| ... copied (of which drawn in the pass) | 34.35 (0) | 34.35 (**100%**) | 31.02 (0) | 31.02 (**100%**) |
| ... no copy: sampled rectangle disjoint | 2.00 | 2.00 | 0 | 0 |
| ... no copy: frame untouched (IN-495) | 1.00 | 1.00 | 1.00 | 1.00 |

Pass ends by reason with the switch on (per VBlank, Hong Kong / Motegi): the clear (`draws recorded for clear: reads
what a draw writes`) 6.81 / 6.37, the submission cadence (`draws recorded for Draw: submission`) 4.71 / 3.43, the fill
2.87 / 2.97, the VBlank flush 0.96 / 0.99, the readback 0.72 / 0.60, and the ends the open pass now pays when a compute
or transfer command closes it: `Draw` 3.20 / 2.13 (the clear and fill paths), `clear_new_image` 0.95 / 0.89,
`WriteClut` 0.86 / 0.77, `a full barrier before the draws` 0.95 / 0.78, then `UploadGlm`, `ClutFromTarget`,
`DecodeTexture`, `MaterializeTarget` and `Patch` (0.02-0.33 each).

Reading: **the pass count is 54% lower in Hong Kong and 57% lower in Motegi**; the copies all take the drawn path
(100%, no fallback in these scenes). The full barriers fall by 60 and 56 per VBlank — the copy's own `image raw` barrier
and the flush hazard that preceded it — and the fragment barriers rise by 65 and 60, the two per copy that replace them.
It is not the full 34/31: a copy still needs a pass of its own when the submission cadence, a readback or a compute
command falls between it and its batch, and the pass the copy used to share with the draws before it is now closed by
whichever command follows the chain instead of by the copy.

### Exactness gates (C)

Flare reply hashes, `build/t178-spec2` (Spec II), the study's scenes and commands, `GT4_CORE2_SELF_COPY_DRAW=1`:

| Run | Replies | Hash | Section 3 reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | same |
| Motegi 2x | 6588 | `7bf932fbb8799965` | same |
| Motegi 2x, `GT4_HW_INTERLOCK=0` | 6588 | `7bf932fbb8799965` | same |

The switch-off runs of the same build give the same hashes (Hong Kong 2x and Motegi 2x were run both ways).

CTest: the full suite on the same build (`build/t178-spec2`, run serially, `ctest --output-on-failure`): **320 tests,
319 passed, 1 skipped** (`hw_replace_q07_pcsx2_dumps`: no PCSX2 dumps on this machine), **0 failed**, 471.8 s. The new
`hw_core2_self_copy_draw` passes at 1x, 2x, 3x and 12x, as do `hw_core2_self_texture`, `hw_core2_sync_trace`,
`hw_core2_rop`, `hw_core2_whole_clears` and `hw_core2_compute_fills`. No test failed under load, so no test needed a
rerun alone.

### Doubts (U, Hyp)

- **U: the GPU cost of a copy drawn instead of transferred.** Two fragment barriers and a small draw replace a transfer
  and a full barrier; the count is right, the cost is not measured (no timing A/B on this shared desktop).
- **Hyp: the remaining passes.** The pass ends that remain are the clear, the submission cadence, the fill, the VBlank
  flush and the readback, plus the ends the open pass now pays when a compute or transfer command closes it (`Draw`,
  `clear_new_image`, `WriteClut`, `UploadGlm`, ...). Making a command wait for the pass instead of ending it would need
  the pass to survive a compute dispatch, which it cannot; recording that work before the chain starts would.
- **U: other tracks and the phone.** The disjoint share and the copy rate are measured on Hong Kong and Motegi only; the
  Nordschleife (rally, rain, night) and the S23's chain are not. `GT4_CORE2_BARRIER_TRACE=1` answers both on the device.
- **U: the colour-attachment path** (integrated GPUs) always falls back to the transfer, so R1's gain there is zero.
- **Hyp: the first copy after `Init`** compiles a pipeline inside the first frame unless the driver had it cached; the
  pipeline is created at `Init`, not at first use.

