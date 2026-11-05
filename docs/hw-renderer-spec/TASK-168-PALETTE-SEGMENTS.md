# TASK-168: several palettes per batch (palette segments)

Written 2026-11-04 for CoreGS 2.0 (core 2), clean room (decision 0070): from this repository's front end, CLUT unit and
core 2 sources only.

## Why

In the player's view (HUD and rear-view mirror) GT4 sends 1,700-2,500 batches per VBlank, and the GS thread time
follows the batch count. The largest flush reason there is a CLUT load (chapter 07.3 step 1): about 1,340-1,730 per
VBlank, about 1,000 of them under a pending batch that keeps its texture and only changes the palette.

## What changed

- **Front end** (`frontend/hw_front_end.cpp`, `hw_batch.hpp`): when a CLUT load would end the pending batch only
  because the batch draws an indexed texture, the front end asks `PaletteSegmentAllowed`. When it answers yes, the
  batch goes on and the primitives after the load form a new **palette segment** (`Batch::paletteSegments`): the
  segment's first primitive and vertex, the CLUT generation and a copy of the 512-halfword CLUT buffer the load
  produced. The first segment is the buffer before the first kept load. A batch without segments has one palette, the
  CLUT unit's buffer, as before. A load right after another one (no primitive between) replaces the palette of the
  segment the first one opened; a segment with no primitive at the end of a batch is dropped.
- **Consumers** read the segment's copy for the segment's primitives:
  - the writeback translation (`TranslateBatchHooked`): `palette` is segment 0's, `segmentPalettes` holds every
    segment's 256 entries and `drawSegments` the segment of each draw; a texture path with one palette (the CPU
    decode, the GPU CLUT expansion of IN-349) reports `paletteSegmentsUnsupported` instead of drawing;
  - the CPU reference (`cpuref::DrawBatch`): the palette is decoded again at each segment's first primitive;
  - the vertex trace (chapter 02.9): the indexed alpha range over all the batch's palettes;
  - chapter 02.4 ST rounding: decided per segment (constant Z over the segment's vertices), as the batch a flush
    would have ended there;
  - core 2: `PaletteRow` per segment (keyed by content as for any CPU palette); the tone hook and the texture
    replacement key read the batch's own palette (`DecodeSegmentPalette`, `PaletteSegmentClut`).
- **Route** (TASK-168b, `RoutePaletteSegments`): texture replacement or dumping takes one palette per batch, so core 2
  draws a segmented batch segment by segment (`PaletteSegmentBatch`, what the flush would have emitted) while it is
  active. For GT4's tone sequence (7.2), core 2 follows `ToneHook` segment by segment, as it would have taken each
  segment's batch, when the batch is a T8 sprite batch that the hook could take (a 32-bit target at its texture base, or
  a tone run going on): the leading segments the hook would skip (a run going on, the run's FBMSK, `PossibleShuffle`
  true for that segment's primitives) are skipped without a batch each; when no segment has the shuffle shape, the hook
  would end the run and leave every segment to the normal path, so the batch is drawn whole without the hook; otherwise
  the remaining segments get a batch each (the hook may start a run there, which reads that segment's palette).
- **GPU** (`core2_gpu.cpp`, `hw_vertex.vert`, `hw_pixel_uber.frag`): `GpuVertex::paletteRow` (bytes 30-31, unused
  before; the vertex stays 32 bytes) is bound as `R16_UINT` at location 7 in core 2's pipelines and passed flat to the
  pixel stage. 0 means the batch's row (`DrawBatch::paletteRow`, the state block's `tex_origin_y`); n means row n - 1.
  Only the vertices of segments 1 and later carry a row, so a batch with one palette draws exactly as before. Core 2
  builds its own vertex variant (`hw_vertex_core2`, `HWR_PALETTE_ROW=1`) and its four pixel variants with the same
  define; the old core's shaders and vertex input are unchanged.
- **Rows and hazards**: every row a batch names is registered with the IN-419/IN-420 hazard tracking
  (`DrawBatch::segmentPaletteRows`), so a later rewrite of a row waits for the deferred draws that read it. While a
  batch takes its rows, the rows already given to its earlier segments are pinned: the round-robin ring skips them, so
  a later segment never takes the row an earlier one of the same batch got (at most 256 segments of 1024 rows).
- **Capacity**: with segments, the vertex limit of chapter 02.5 counts from the last segment's start, where the flush
  would have started a batch: the primitives before that segment are emitted when the limit is reached
  (`FlushSegmentsBeforeLast`, no transfer commit and no TEXFLUSH change, as no flush happened there before) and only a
  full segment ends with a capacity flush.

## The switch

`GT4_CORE2_PALETTE_SEGMENTS`, on by default in core 2 (it needs the palette rows of IN-409, so
`GT4_CORE2_PALETTE_ROWS=0` turns it off too). `=0` does not set the front-end hook, so the front end runs the chapter
07.3 rule exactly as before. Logged at start as `[core2] palette segments (TASK-168) on|off`. Counters: the
`[core2] batch flushes/vblank` line adds `clut_kept`, `segmented_batches` and `segments_per_segmented_batch`; the
`[core2] palette segments` line gives the session's batches drawn with several segments (`batches`, `segments`), the
segmented batches drawn segment by segment (`splits`), and (TASK-168b) the segments the tone hook skipped without a
batch of their own (`tone_skipped`) and the segmented T8 sprite batches drawn whole without the hook (`whole_no_tone`).

## Why the pixels stay the same

A segmented batch must be equal to the batches the chapter 07.3 flushes would have made, primitive for primitive:

1. **The palette.** Each primitive's palette is the CLUT buffer at the time it was kicked: the old path drew each
   flushed batch with the CLUT unit's buffer, which could not change under a pending indexed batch without a flush;
   the segment keeps exactly that buffer.
2. **The rest of the state.** The segment is kept only when the live TEX0 equals the batch's snapshot under the base
   and indexed masks (TBP0 to TFX, CPSM, CSA) in the batch's context. The fields left out (CBP, CSM, CLD) only steer
   CLUT loads; every other register goes through the chapter 02.5 rules for the next primitive as before.
3. **The memory the load reads.** The batch's frame rows (whatever FBMSK says) and its depth rows, when it tests or
   writes depth, do not meet the palette's CSM1 page (`cbp & ~31`, 64 blocks, the range `SetBeforeLocalRead` reports);
   CSM2 loads always flush. So the earlier primitives cannot change what the load reads, and in core 2 the hook also
   asks that no drawn target holds that page (no readback, no GPU CLUT load).
4. **No feedback.** The batch's texture (level 0 and its mip levels) does not meet its frame or depth rows, so no
   primitive samples what an earlier one of the batch drew.
5. **The batch boundaries.** No auto-flush (chapter 02.6) can split the batch (its register conditions forbid it);
   the capacity limit counts per segment (above); ST rounding is decided per segment. Uploads, moves and readbacks keep
   their overlap flushes (they test the whole batch, which may only flush earlier than before, never later).
6. **CPU palettes only (core 2).** The hook refuses when the batch's palette uses a halfword a GPU load wrote
   (`GpuPaletteKey`): the GPU CLUT is one state, and a later load would overwrite it. A load under a batch then reads
   memory and writes every halfword the same CPSM and CSA read, so every segment's palette is a CPU one whose content
   is its copy.
7. **The GPU draw.** A segmented batch has one pixel state; only the palette row differs per primitive, given flat per
   vertex. Primitive order inside the batch is kept as for any batch (interlock, ROP or the IN-466 waits).

## The cases that still flush

The load ends the batch, as before, when: the texture changes (TBP0, TBW, PSM, TW, TH, TCC, TFX, CPSM or CSA), the load
is a TEX0 write of the other context, CSM2, the batch's frame or depth rows meet the palette's page, the texture meets
the batch's frame or depth rows, an auto-flush could split the batch, the batch holds 256 segments, a drawn target
holds the palette's page (core 2), the batch's palette was loaded on the GPU (core 2), texture replacement or dumping is
active (core 2), or `GT4_CORE2_PALETTE_SEGMENTS=0`. The old core and the CPU reference never set the hook.

## Tests

- `m3_hw_palette_segments`: segments serialised and compared with the batches of the flushes (all but the flush reason
  and the CBP, CSM and CLD fields of the TEX0 snapshot), the palettes decoded from the copies, ST rounding per segment,
  each case that still flushes, the 256-segment limit and the capacity rule.
- `m3_hw_clut_snapshot`: a segment's copy is its load's palette whatever the palette memory holds later.
- `m3_hw_front_end_fast_paths`: random strips, sprites and palette loads with the hook on give byte-identical
  batches (segments included) on the IN-366 fast paths and the generic path.
- `hw_core2_palette_segments`: core 2 A/B at 1x and 2x against the CPU reference: four palettes per run of loads, the
  row ring (1100 palettes a frame, five batches of up to 256 segments, more palettes than rows), a row an earlier
  segment of the same batch names (the ring's next row; without the pinning that frame differs in 1178 words), loads
  from a GPU palette that must still flush, and a T8 batch over a 32-bit target split for the tone hook; TASK-168b: a
  tone run's copy-back strips (a run started by one batch, then one batch of three 8 x 32 segments that the hook skips:
  `tone_skipped` 3 per frame, no split, the same tone runs and skipped draws as with the switch off), and 12 x 12 T8
  sprites over a 32-bit target (not of the shuffle shape: drawn whole, `whole_no_tone` 1 per frame).

## TASK-168b: what the segments were in GT4, and what changed

Written 2026-11-04 from headless runs of the George V Paris replay lap (Spec II, `george-volta2`, 1x, 480p, 16:9) on
the desktop; counts are exact, times are indicative (a shared machine).

**The measurement that started it** (Avell, RTX 3060 laptop, e655a630, segments on vs `GT4_CORE2_PALETTE_SEGMENTS=0`):
CLUT-load flushes fell from 1601 to 645 per VBlank (957 loads kept, 166 segmented batches), but the GS thread took 13%
more at 1x and 10% more at 6x, almost all in `gif_parse` (+34%), and the session line read
`batches=0 segments=0 splits=742980`: every segmented batch was split back into its segments, so the GPU got exactly
the old batches and the front end paid for nothing.

**Diagnosis** (Confirmed, temporary counters in the split path, e655a630 + counters, run `diag1`):

- Which condition split: `toneActive_` in 742980 of 742980 splits; `ShuffleSource` alone in 0.
- What the batches are: GT4's tone sequence (CORE2-SPEC 7.2, IN-383), its copy-back strips. Every split batch is one
  64 x 32 strip of eight 8 x 32 T8 sprites (TEX0 TBP0 14244, TBW 1, TW 3, TH 5; CLUT at CBP 14208, CT32, CSA 0, CSM1)
  drawn into the CT24 frame at FBP 210 (FBW 10, FBMSK 0xFF000000), with a CLUT load before each sprite: one segment per
  sprite, 8 per batch (840 distinct strip rectangles and register sets over the run).
- What the tone hook did with them: `PossibleShuffle` was true for 100% of the segments' batches, and `ToneHook` skipped
  100% of them (a run going on, the run's FBMSK): 5943840 segment batches built over the run, none reached the normal
  path, and none read its palette. The run's tone runs (5623) and skipped draws (8659420) are the same in the TASK-168
  build and after this change.
- Where the time went (CPU sampler, `GT4_CPU_SAMPLER`, VBlanks 3000-8000, on vs off, GS thread): core 2's segment hook
  0.61 ms per VBlank, of which 0.56 ms in `GpuPaletteKey` (it built the list of the 512 halfwords a T8 CT32 palette reads
  and looked each up, about 950 times per VBlank, while no halfword had been written on the GPU); the split's batch copies
  0.22 ms in vertex pushes plus 0.15 ms in the heap; and the 1 KiB copies of each kept load (three per load).
- Motegi and Hong Kong at 6x (B-Spec): about 35 kept loads per VBlank, `segments_per_segmented_batch=1.00` and
  `batches=0 splits=0`: the segment a load opened got no primitive before the batch ended (dropped), so nothing changed.
  The track-map A/B (52 Spec II tracks, desktop, A-Spec, 1x): the same picture, about 99.97% of the segmented batches
  split back (`splits` about 910000 per run); that those are the same copy-back strips is a hypothesis (not traced
  there).

**What changed**

1. **The route** (`RoutePaletteSegments`, above): the copy-back strips are no longer split. The segments the tone hook
   would skip are skipped at once (`tone_skipped`), with the same counters as the hook (`toneDrawsSkipped`, the batch
   count, the last scene frame), so the run's state and every count stay as with the switch off. A segmented T8 sprite
   batch that no segment of could take the hook is drawn whole (`whole_no_tone`); only the others are still split.
2. **`GpuPaletteKey` answers 0 at once while no CLUT halfword was written on the GPU**: core 2 counts the nonzero
   `clutSource_` entries (`SetClutSource`, `ClearClutSource`, every write of that array), and a load from memory with no
   GPU halfword anywhere writes nothing. This helps every caller (the palette row of every indexed batch, the tone
   hook's CLUT sync), with segments on or off.
3. **The kept load's walk of the targets is done once**: the segment hook proves that no drawn target holds the load's
   CSM1 page, and the front end calls `SetBeforeLocalRead` for that same load right after it, so that callback skips
   its own walk (`clutSpanClear_`, valid for that one load).
4. **The front end copies a palette once per segment**: the buffer before the first kept load is copied only for the
   first one of a batch, and segments are built in place (`emplace_back`) instead of a local 1 KiB copy pushed back.
   Copying only the halfwords CPSM and CSA read was not done: every kept load in these scenes is a T8 CT32 palette with
   CSA 0, which reads all 512 halfwords.

## Known limits

- In a first frame where a target is created by the first of several small batches over memory the CPU wrote (a CPU
  sprite, IN-407) and then grown by the next ones, the grown part does not take that memory (`Grow` copies the image;
  only `CreateTarget` seeds from recent writes). The flushes then gave a result off the CPU reference, while the merged
  batch creates the target for the union at once and matches it. This is the existing growth behaviour, not a
  segment rule; the fixture starts each frame with a GPU draw over the whole frame to keep the A/B comparison on the
  segments.
