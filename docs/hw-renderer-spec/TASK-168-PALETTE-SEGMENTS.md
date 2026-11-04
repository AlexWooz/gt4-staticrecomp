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
- **Split**: core 2 hands a segmented batch to `OnBatch` segment by segment (`PaletteSegmentBatch`, what the flush
  would have emitted) when a consumer takes one palette per batch: texture replacement or dumping is active, or the
  batch is a T8 sprite batch that the tone hook could take (a 32-bit target at its texture base, or a tone run going on).
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
`[core2] palette segments` line gives the session's segmented batches, segments and splits.

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
  from a GPU palette that must still flush, and a T8 batch over a 32-bit target split for the tone hook.

## Known limits

- In a first frame where a target is created by the first of several small batches over memory the CPU wrote (a CPU
  sprite, IN-407) and then grown by the next ones, the grown part does not take that memory (`Grow` copies the image;
  only `CreateTarget` seeds from recent writes). The flushes then gave a result off the CPU reference, while the merged
  batch creates the target for the union at once and matches it. This is the existing growth behaviour, not a
  segment rule; the fixture starts each frame with a GPU draw over the whole frame to keep the A/B comparison on the
  segments.
