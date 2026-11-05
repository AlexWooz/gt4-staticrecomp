# Study: GT4's tone sequence strips as one GPU pass?

Written 2026-11-05 for CoreGS 2.0 (core 2), clean room (decision 0070): from this repository's front end, CLUT unit and
core 2 sources, headless runs of our own builds, and the GSdx oracle's prose report
(`ORACLE-GSDX-TONE-READBACK-FLUSH.md`, section A, on release/1.3.0-rc15). Labels: **C** Confirmed (measured or read in
our code), **H** High confidence, **Hyp** Hypothesis, **U** Unknown.

## Short answer

- **The GPU side is already one pass (C).** Core 2's `ToneHook` (CORE2-SPEC 7.2) answers the run's first shuffle draw
  with ONE compute dispatch (`Gpu::Tone`, `core2_tone.comp`) over the whole frame target at its scale, and drops every
  later draw of the run, the copy-back strips included. Today's frames are that pass's output by definition, so "draw
  the strips as one pass" is already done and cannot be made more exact than it is. GSdx does the same (oracle A3).
- **The cost is CPU (C):** the front end still parses, batches and flushes the run's 1,540 draws and performs its 1,261
  CLUT loads before `ToneHook` drops them: about 2.0-2.3 ms of the GS thread per VBlank on the desktop at 1x (about
  17% of it). The target is to drop them earlier and cheaper, with every side effect kept.

## 1. What a tone run is (George V lap, Spec II, VBlank 6000, one-VBlank GS trace)

**Prologue** (before the run, every VBlank, C):
- an upload of a 16 x 16 CT24 image to block 14240 (page 445): the 256-entry ramp CLUT (the tone curve; its alpha bytes
  are whatever the memory held);
- an upload of an 8 x 32 T8 image to block 14244: the index texture of the copy-back strips;
- FRAME_1 = the temporary page (FBP 444, block 14208, FBW 10, CT32, FBMSK 0xFF000000), FRAME_2 = the frame
  (FBP 210, block 6720, FBW 10, CT24, FBMSK 0xFF000000), ALPHA_1 = Cs + Cd (FIX 128), depth test ALWAYS without writes,
  XYOFFSET_2 placing context 2 on the frame.

**Per 64 x 32 page of the 640 x 448 frame** (140 pages, all the same shape, C):
1. TEX0_1: the frame page read as T8 (TBP0 6720 + 32 k, TBW 2, 128 x 64 texels), CBP 14240 (the ramp), CLD 5: a CLUT
   load (the strips of the previous page replaced the buffer).
2. Three extraction batches (context 1, 16 sprites of 8 x 16 each, covering the page): R, then G and B added
   (RGBAQ 0x80 in one channel, CLAMP_1 region repeat selecting the channel's bytes): the temporary page receives
   (ramp[R], ramp[G], ramp[B]) per pixel, in the temporary page's own layout.
3. Eight copy-back strips (context 2, one 8 x 32 sprite each): TEX0_2 TBP0 14244, CBP = the temporary page's quadrant q
   (14208 + {0, 4, 16, 20, 8, 12, 24, 28}), CSM1, CT32, CLD 5, so each strip loads a CLUT from 16 x 16 pixels of the
   temporary page and draws it back, through the index texture, into the frame page (CT24, RGB only).

**Totals per run:** 1,540 batches (420 extraction + 1,120 strips), 7,840 sprites (15,687 vertex kicks), 1,264 CLUT
load requests of which 1,261 real loads (1,120 strips + 141 ramp), 1,262 TEX0 writes, 282 CLAMP_1 writes. One run per
VBlank in the race (`tone_runs` +600 per 600 VBlanks, skipped draws +1,540 per VBlank), 72% of the VBlank's 2,149
batches and 33% of its 46,934 vertex kicks.

**What it computes:** a per-channel LUT of the frame (R' = ramp[R].r, G' = ramp[G].g, B' = ramp[B].b, alpha kept),
i.e. a colour-grading tone curve. **Between frames** only the ramp's content changes (brightness, fades). **Between
pages and strips** only the texture base (frame page), the strip CBP quadrant and the destination XY change.

**Scenes:** every Spec II race view measured; none in the v2.00 night race (`tone_runs=0` over 12,000 VBlanks). v2.00
day races have it (IN-383's 140 pages per frame) (H).

## 2. Can it be one exact GPU pass?

Yes, and it is (C): `core2_tone.comp` reads each target pixel at scale and writes the three looked-up channels with the
alpha byte kept; FBMSK 0xFF000000 and the CT24 target are honoured by construction (RGB only, the target's own image).
GPU cost of that dispatch, isolated (`GT4_CORE2_GPU_PROFILE_AT`, RX 9060 XT): 0.13 ms at 6x; at 1x it was not among the
profiled VBlank's commands (U, expected about 1/36 of that). Nothing to gain on the GPU.

Not exact to a real GS (C, oracle A4, unchanged here): the dropped draws never write the temporary page or the frame's
GS memory, and the strips' CLUT loads read a temporary page the extraction never wrote. Identity is measured against
today's core 2 (rc14 PGO references), which already behaves this way.

## 3. Where the CPU goes inside a run (desktop, 1x, `GT4_HW_PROFILE_EVERY=600`, temporary counters, not committed)

| Part of the run (ms per VBlank) | VBlanks 4200-7800 |
|---|---|
| GS thread inside a run (TSC inside `GifTransfer` while a run is active) | 1.97-2.29 |
| front end (GIF parse, register writes, kicks, batch emission) | 1.39-1.69 |
| CLUT stage (the `SetBeforeLocalRead` walk of the targets, `OnClutLoaded`) | 0.52-0.56 |
| batch and hook stages (`OnBatch` up to `ToneHook`'s drop) | 0.07 |

The sampler profile (whole GS thread) adds the cost per item: each real CLUT load walks the targets three times in
`SetBeforeLocalRead` (`ClutSourceTarget`, `ClutTargetsOnGpu`, the readback loop: about 0.55 ms per VBlank over all
loads), `ClutUnit::Copy` about 0.29 ms (1 KiB compare and 1 KiB copy per load), and every emitted batch runs the
chapter 02.4 ST rounding loop over its vertices before `OnBatch`. Translation never runs for a dropped batch (C).

## 4. What can be dropped earlier, exactly

`ToneHook` drops a batch when a run is going on, the batch has the run's FBMSK and `PossibleShuffle(batch)` holds
(TME, T8, sprite class, its vertex box and first sprite). That needs the batch's vertices, so the decision cannot be
made at a batch's first primitive: a later primitive could grow the box past 64 and make the batch a drawn one. The
kick work (parse, `ShouldDraw`, `AppendPrimitive`) therefore stays (exact only with buffering, which costs what it
saves). What can go, with every side effect kept:

1. **Drop on arrival** (front end + core 2): when the front end emits a batch, it asks the sink first, after the
   chapter 02.6 discard and before the work that only a drawn batch needs (ST rounding per segment, the vertex trace,
   the hand-over of the batch object). Core 2 answers with `ToneHook`'s own skip test and applies the skip's side
   effects itself (`counters.batches`, the last scene frame, `toneDrawsSkipped`). The front end keeps its own: the CLUT
   staleness of the batch's frame rows (`NoteBatchWritesToClut`), the flush reason and counts, `BatchesEmitted`, and the
   batch storage reuse.
2. **One walk per CLUT load**: `SetBeforeLocalRead` first looks for any drawn target over the span; with none (the
   usual case, all strip and ramp loads) neither the GPU CLUT paths nor a readback can apply, and it returns after one
   walk instead of three.
3. Not done: holding the strips' CLUT loads until one is observed (IN-383's old-core idea). Only the last load of a run
   is observable, but the CLUT unit's redundancy, staleness and memo state and core 2's GPU CLUT mirror would all have
   to replay exactly; worth it only if 1 and 2 leave too much.

Expected saving: up to about 0.4 ms per VBlank from 1 (Hyp: 1,540 batches at a few hundred ns of loops and hand-over
each) and about 0.3 ms from 2 (Hyp: two walks of about 43 target slots for 1,600-2,500 loads), of the 2.0-2.3 ms.
Renderer batches per VBlank stay the same (2,288 on this lap): they are counted as before; only fewer of them reach
`OnBatch`.

## 5. Risks and checks

- **Ordering:** nothing is deferred; every decision happens at the same point of the stream as today.
- **Readbacks during a run:** unchanged (no draw of the run touches a target; readbacks follow their own flushes).
- **Spec II vs v2.00:** the drop test is `ToneHook`'s own, so it holds wherever the hook does (v2.00 day races, Spec II);
  the night race has no run.
- **Fallback:** a batch the test refuses takes `OnBatch` as before; `GT4_CORE2_TONE_EARLY_DROP=0` turns the early drop
  off (A/B).
- **Counters:** the renderer's batch count and `toneDrawsSkipped` keep their meaning; a new counter
  (`tone_dropped_on_arrival`) tells how many batches took the early path.

## 6. Done (2026-11-05)

Items 4.1 and 4.2 of the plan below are implemented on `task/168b-segments-fix`:
- `BatchSink::DropOnArrival` (front end): asked for every emitted batch after the 02.6 discard; on yes the front end
  keeps `NoteBatchWritesToClut`, the flush reason and counts and `BatchesEmitted`, and recycles the batch storage,
  without the 02.4 rounding, the vertex trace or `OnBatch`. Core 2 answers with `ToneHook`'s skip test and its side
  effects (`counters.batches`, the last scene frame, `toneDrawsSkipped`, and the new `toneDropsOnArrival`, printed as
  `tone_early`). `GT4_CORE2_TONE_EARLY_DROP=0` turns it off.
- `SetBeforeLocalRead` returns after one walk when no drawn target meets the load's span.
- Every skipped draw but a run's first now takes the early path: George V lap `tone_skipped=8659420 tone_early=8653797`
  (5,623 runs), El Capitan 11,922,680 / 11,914,938, Tokyo R246 12,153,680 / 12,145,788, v2.00 Arcade 1,319,780 /
  1,318,923.
- Identity (gates6 against rc14 PGO, Confirmed): George V, El Capitan and Tokyo R246 at 1x (race clock, simulation state,
  state dump, 24/35/35 frames), v2.00 night race 3x (state, clock, 10 frames), v2.00 Arcade 3x (14 frames), George V 6x
  (12 frames), VU1/VU0 verify (0 mismatches, 0 interpreted calls) and MTVU verify (8,032 frames, 0 mismatches) all
  identical. Fixtures: `hw_core2_tone_early_drop` (A/B at 1x and 2x: images and GS memory equal, the same tone runs and
  skipped draws, 3 early drops per frame), `m3_hw_front_end_drop_on_arrival` (a sink dropping every batch leaves the
  same flush counts, CLUT loads performed and skipped, and CLUT buffer as one receiving them), the palette and tone
  fixtures, and the full CTest.

## 7. Plan, cheapest first

1. The single-walk CLUT callback (exact by inspection; gates: George V, El Capitan and Tokyo R246 1x identity, v2.00
   night 3x, v2.00 Arcade 3x, George V 6x frames, VU and MTVU checks (`gates6`); fixtures `hw_core2_gpu_clut`, the
   palette fixtures).
2. Drop on arrival with its A/B switch (the same gates; fixtures: core 2 A/B of a tone run with copy-back strips,
   images, GS memory and tone counters equal with the switch on and off; a front-end test where a sink that drops every
   batch leaves the same flush counts and CLUT loads as one that receives them).
3. Measure (desktop now, Avell later). Only if the gain is small: the held CLUT loads of item 4.3, with a CLUT-unit test
   comparing the whole unit state after a held and an immediate sequence.
4. Out of scope here: the EE's generation of the 7,840 sprites per run (game code, TASK-134).
