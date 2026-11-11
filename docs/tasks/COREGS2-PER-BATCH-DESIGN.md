# CoreGS 2.0: per-batch removal - design note (prepared 2026-11-11, **not started**)

Context: the 3x CPU levers measured so far are closed - palette segments under the tone hook cost **+0.61 ms** (reverted,
IN-548), the per-record vertex conversion is **~0** (stopped, IN-552). The license B-1 1x split still shows **1,930
batches** per VBlank costing `draw` 0.93 + `batch_own` 0.64 + `clut` 0.48 + `swizzle` 0.49 + `translate` 0.45 = 2.99 ms,
and 1,430 of those batches are CLUT-load flushes whose *flush* is worth only ~0.1 us each. The lever that remains is
**removing batches**, not repacking vertices.

## What "one batch fewer" is worth, measured

- Each CLUT-load flush costs ~0.1 us (the `GT4_CORE2_TONE_STRIP_DROP` run: 1,260 flushes = 0.12 ms).
- The expensive part is the **per-batch start**: `TakeSnapshot`, the batch `state` copy, the translation reset, the draw-set
  key, the palette row lookup and the submission bookkeeping - the ~500 real draw batches cost ~6 us each
  (2.99 ms / 500), and 1,430 flush batches cost ~0.1 us each.
- So merging N flush batches into their neighbours only pays if the *merged* batch does not grow the expensive path: the
  bound is the 1,430 x 0.1 us = **0.14 ms** unless the merge also removes real per-batch work.

## The two candidate shapes

1. **Second-level batching of the CLUT-load flushes** (what TASK-168 tried via palette segments, rejected for the 1 KiB
   palette copy): a batch that would be flushed only because its palette changed could instead be *appended* to the next
   batch's queue when the two share state, texture and target. Needs a cheap palette representation (a row index into the
   renderer's palette rows, or a CLUT generation counter) instead of a 512-halfword copy.
2. **One draw record per primitive inside a batch** (the "record" idea, but removing the *batch* boundary rather than the
   vertex pack): the front end emits a single state-keyed batch per (texture, palette row, target) run instead of flushing
   on every CLUT load, and the renderer draws the run as one record with a palette-row byte per vertex (the `GpuVertex`
   paletteRow field already exists for TASK-168 segments).

## Bound and price

- Bound: **0.3-0.6 ms** of the license's 8.1 ms (Hypothesis; the 0.14 ms of flush cost plus part of the 0.64 ms
  `batch_own` and the per-batch `draw`/`translate` starts).
- Price: 2-3 days, and it touches the batch record and the CPU reference (the same contract points that stopped the
  per-record step).
- First step: measure how many of the 1,430 CLUT-load flushes share texture/target/state with their neighbour (a counter,
  no behaviour change) - that number bounds the whole lever and costs half a day.

**Status:** prepared only, not started. The owner decides after the flare work.
