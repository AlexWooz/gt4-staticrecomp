# CoreGS 2.0 license B-1 tone/CLUT audit (TASK-215f, 2026-11-10)

Read-only follow-up to `docs/tasks/COREGS2-SCENE-GENERALITY-AUDIT.md` (TASK-215d). The TASK-215 perf agent measured
rc15's `GT4_CORE2_PALETTE_SEGMENTS=1` (TASK-168, off by default) on license B-1 (Test Course): the earlier 9.55 ->
8.99/8.71 was an outlier; 3 off runs 8.675 / 8.705 / 9.550 against 2 on runs 8.705 / 8.990, with `clut_kept=0`,
`segmented_batches=0` and `tone_refused=1.67M` in both. The segment path is refused while a tone run is active. The
license stays CPU-bound: GS worker **8.08-8.25 ms**, GPU **1.05**, **1,430 CLUT flushes/VBlank (1,010 same texture)**,
`gif_parse` **3.80 ms**. The questions: why the tone hook refuses the segments; can the 1,430 be cut without segments;
what is the smallest exact design; is the tone CLUT readback on the license path; and what should the perf agent do
first.

- Worktree `C:\Antigravity\gt4-wt-215-coregs2-audit`, branch `docs/coregs2-perf-audit` at `ff5c3d98` (docs).
- **The TASK-168 segment path is not in this docs branch.** It lives in the TASK-215/rc15 worktree
  `C:\Antigravity\gt4-wt-215-coregs2-perf` (`d18542f2`), which was read **read-only** for the trace; no file outside the
  audit worktree was changed. The line numbers below are from that worktree; the docs branch's `native/hw_gs` is the
  ff5c3d98 version without `PALETTE_SEGMENTS`.
- No builds, benchmarks, game runs, code/data/pin/test changes; no PCSX2/GSdx/ARMSX2 source; GSdx is a black box.
- Labels: **C** = confirmed (measured or read in the cited code); **H** = high confidence; **Hyp** = hypothesis;
  **Unknown** = needs a measurement or a decision.

## 0. The short answer

The tone refusal is **not a visibility or ordering impossibility**. `ToneHook` can already handle a segmented batch
correctly: `OnBatch` splits a batch with more than one palette segment into one batch per segment
(`core2_renderer.cpp:4170-4190`), and each split batch's tone uses **its own segment's palette**
(`DecodeSegmentPalette(batch, 0, ...)`, `core2_renderer.cpp:3530-3531`). The refusal is a **measured performance veto**
added by TASK-168b (`PaletteSegmentBatchAllowed`, `core2_renderer.cpp:815-825`): when a tone run is active and the
pending batch is a **T8 sprite** (`ToneShape`, `870-873`), the batch is a **copy-back strip** that `ToneHook` skips
whole anyway (`3505-3512`); keeping the CLUT load as a segment and then splitting it cost more than the flush it
replaced (desktop A/B of the George V lap: +4% GS thread with the strips merged and skipped at once, +3% when the hook
refused after the front end's memory checks, equal to segments off with this filter; the TASK-168b comment at
`core2_renderer.cpp:815-820`). On license B-1 the **1,010 same-texture flushes are those T8 copy-back strips**: the
segment path never forms (`clut_kept=0`, `segmented_batches=0`), so the `GT4_CORE2_PALETTE_SEGMENTS=1` A/B measured
nothing. The `tone_refused=1.67M` in both runs is **likely** the **tone early-drop count** (`DropOnArrival`,
`core2_renderer.cpp:4158-4167`), which is active in both because it is not gated by the segment switch; the segment
filter's own `paletteSegmentToneRefusals` is only installed with segments on (`995-999`), so the label needs
confirmation (open question 3). Either way, the T8 strips are dropped/skipped in both runs, and the segment filter refuses
them when on.

The license's flushes are a **batch boundary caused by the CLUT load**, not a texture-cache event: the front end's
`OnTex0Write` flushes the pending indexed batch because a batch has one palette (`hw_front_end.cpp:390-418`; the
`clutFlushSameTexture_` at `415-417` counts the palette-only changes). The 1,010 same-texture flushes are palette
changes on the same T8 texture; the 211 `prim` and 170 `texture` flushes are the other state changes; `gif_parse 3.80`
is the per-primitive front end, not a flush reason.

The **smallest exact design for the license's 1,010** is not the segments and not the unchanged-palette merge: it is a
**front-end tone-strip drop**. The tone hook already skips those T8 copy-back strips (`ToneHook`, `3505-3512`;
`DropOnArrival`, `4158-4167`); dropping them **before** the front-end `FlushBatch` on the CLUT load avoids the batch
boundary, the 1,010 `clut` flushes and the batch work, while the **first** batch of the run still reaches the renderer
for the tone pass and every pixel stays identical. The expected gain is **Hyp** about 1 ms of the GS worker (the
TASK-134 CLUT path was 2.0 ms for ~1,500 flushes on George V), not enough alone for the RTX 3060's 19-20 ms; it is the
first license-specific step next to TASK-171's parse/front end. The CLUT-flush scope (TASK-215b B) is the right exact
merge for the **non-tone** CLUT loads whose used halfwords do not change, but it cannot cut the 1,010 (their palettes
do change). The tone CLUT readback (`ReadClut`) is **not on the license path**: the license's palettes are CPU loads
(`GpuPaletteKey == 0`), so `SyncClutToCpu` returns before `ReadClut` (`1257-1276`); the `clut` wait is zero on the
license. The GPU-CLUT tone shader is a night/George V item, not the license.

## 1. Why the tone hook refuses the palette segments

### 1.1 The segment path in rc15 (read in `gt4-wt-215-coregs2-perf`)

- **Option:** `paletteSegments_ = paletteRows_ && GT4_CORE2_PALETTE_SEGMENTS != 0`
  (`core2_renderer.cpp:743-752`), off by default; the comment records "TASK-168b found no GT4 scene that gains".
- **Front end:** `FrontEnd::PaletteSegmentAllowed` (`hw_front_end.cpp:461-512`) allows a CLUT load that would end the
  pending batch to start a **new palette segment** instead when:
  - the pending batch is textured, indexed, same context, and the TEX0 base/indexed fields do not change (`471-482`);
  - the load is CSM1 (`483-485`);
  - the renderer's **batch filter** does not veto (`486-489`);
  - the batch's frame/depth rows do not meet the palette source page (`490-497`) and its texture does not meet its
    frame/depth (`498-500`);
  - no auto-flush can split it (`501-506`), the segment count is bounded (`507-510`), and the renderer's
    `PaletteSegmentAllowed` agrees (`511`).
- **Renderer's per-load answer:** `PaletteSegmentAllowed` (`core2_renderer.cpp:827-846`) requires no GPU-written
  palette halfword (`GpuPaletteKey == 0`), no drawn target over the CSM1 source page, no replacement loader, and
  `paletteSegments_`; it marks the CSM1 span so the front end can skip its own target walk (`848-855`).
- **Kept load:** the front end copies the CLUT before the load (`hw_front_end.cpp:408-412`) and records a
  `PaletteSegment` at the current primitive/vertex (`430-439`, `444-459`); `clutLoadsKept_`/`clut_kept` counts it.
- **Consumers:** `OnBatch` (`core2_renderer.cpp:4170-4190`) routes a batch with >1 segment through
  `RoutePaletteSegments` (`874-890`): `Whole` (no tone shape), `WholeNoTone` (no segment has the shuffle shape; the
  tone run is ended), or `Split` (one batch per segment, each with its own palette copy). `PaletteSegmentBatch`
  (`hw_batch.cpp:93-120`) creates the per-segment batch; `ToneHook` decodes **segment 0** of that batch
  (`core2_renderer.cpp:3530-3531`), which is the segment's palette. The GPU orders each batch's segment palette rows
  (`core2_gpu.cpp:3785-3809`), and the reference renderer draws each segment's primitives with its palette
  (`cpuref/cpuref_draw.cpp:533-534`, `653-664`). So a segmented batch **can** be tone-hooked correctly: split per
  segment, first segment starts/continues the run, later segments are skipped with their own palette available.

### 1.2 The refusal (TASK-168b)

- **Batch filter:** `FrontEnd::PaletteSegmentAllowed` calls the renderer's `paletteSegmentBatchFilter_` **before** the
  memory checks (`hw_front_end.cpp:486-489`); the renderer installs it only when `paletteSegments_`
  (`core2_renderer.cpp:995-999`):
  ```
  bool PaletteSegmentBatchAllowed(const Batch &pending) {
      if (!toneActive_ || !ToneShape(pending)) return true;
      stats.paletteSegmentToneRefusals++;
      return false;
  }
  ```
  (`core2_renderer.cpp:821-825`). `ToneShape` is a textured T8 sprite (`870-873`); `toneActive_` is the current tone
  run. The comment (`815-820`) states the reason: the T8 copy-back strips (about 950 loads/VBlank in the player's
  view) are **skipped whole** by `ToneHook`; a segment saves nothing downstream, while a kept load costs the front end
  more than the flush it replaces.
- **The tone hook's skip:** `ToneHook` (`3503-3562`) returns true (skips the batch) for a T8 sprite with the shuffle
  shape; the first batch of a run applies the tone pass to the target with `DecodeSegmentPalette(batch,0,...)`
  (`3514-3544`), then `toneActive_ = true`, `toneFbmsk_ = fbmsk` (`3557-3561`); later T8 batches with the same FBMSK
  return true at `3510-3512` without drawing. The strips are not drawn; the target is the tone result.
- **The early drop:** TASK-168b's `DropOnArrival` (`4150-4168`) skips a batch **before** `OnBatch` when
  `toneEarlyDrop_ && toneActive_ && PossibleShuffle(batch)` and `paletteSegments.size() <= 1`; that is the T8 strips in
  both the segment-on and segment-off runs (`stats.toneDropsOnArrival`). This is the **likely** reason
  `tone_refused=1.67M` is the same in both: the early-drop count is active in both, while the segment filter's own
  `paletteSegmentToneRefusals` is only installed with segments on (`995-999`). If the perf agent's `tone_refused` is the
  print line's `paletteSegmentToneRefusals` (`5289-5295`), an off run should read 0; confirm the exact counter. Either
  way, the strips are dropped/skipped in both, and the segment filter refuses them when on. The batch-flush line adds
  `clut_kept`/`segmented_batches` (`5619-5622`).
- **The CLUT unit:** `ClutUnit::ProcessWrite` (`hw_clut.cpp:231-252`) decides load vs redundant and calls `Copy`
  (`331-466`), which fills the 512 halfwords and the data memo; `hw_front_end.cpp:472` requires `clut_.CopiesData()` for a
  segment, the segment takes `clut_.Halfwords()` (`hw_front_end.cpp:410`, `436`, `450`, `458`), and the renderer's
  `PaletteSegmentAllowed` requires `GpuPaletteKey == 0`/no GPU-written halfword (`core2_renderer.cpp:827-846`). The
  segment palette is therefore a CPU copy, not a GPU target read: no `ReadClut`, no extra barrier.

### 1.3 Is the refusal fundamental?

**No; it is a measured heuristic.** The correctness pieces are in place: `Split` per segment, the segment palette copy,
`DecodeSegmentPalette`, the palette-row ordering. If a segment were kept under a tone run and the batch were **not
split**, the tone hook would use segment 0's palette for the whole batch and skip the later strips, which would be
wrong for later segments; the `Split` route avoids that. What breaks without the split is the **one-palette-per-batch**
assumption of `ToneHook` (`batch.state.ctx.tex0`, `DecodeSegmentPalette(batch,0)`), not a GPU barrier: the segment
palettes are CPU copies (`PaletteSegment::clut`, `hw_batch.hpp:80-105`), the segment rows are ordered before the draw
(`core2_gpu.cpp:3785-3809`), and the renderer's `PaletteSegmentAllowed` already requires that no drawn target meets the
CSM1 source page and no GPU-written halfword is used (`827-846`), so there is no readback/barrier to add. The
**measured** reason the filter exists is cost: on the George V lap, merging the strips and skipping them at once was
**+4% GS thread**, and letting the hook refuse after the front-end memory checks was **+3%**; equal to segments off with
the filter (`815-820`). On the license the strips are the 1,010 same-texture loads; the filter refuses them before the
segment is formed. The refusal is therefore **correct and intentional**, but it is **not** a fundamental limit: the
`Split` route can handle the tone, and a **front-end drop** (section 3) removes the flush without keeping the batch.

**The `tone_refused` label.** The perf agent's `tone_refused=1.67M in both` is **likely** the **early-drop** count
(`toneDropsOnArrival`, `4158-4167`) for the T8 strips, which is active in both runs; the segment-specific refusal
`paletteSegmentToneRefusals` is only installed with segments on (`995-999`, `821-825`). If the value is the print line's
`paletteSegmentToneRefusals`, the off run should read 0; confirm the exact counter. The audit should not read the 1.67M
as "the segment path refused 1.67M loads in the off run"; the off run has no segment filter. The on run's
`paletteSegmentToneRefusals` is the relevant refusal count; the `clut_kept=0`/`segmented_batches=0` confirm no load was
kept. The segment A/B measured **nothing** on the license: the path never executed.

## 2. Can the license's 1,430 CLUT flushes be cut without segments?

### 2.1 What the 1,430 are

| Flush reason | Count | Mechanism | What it is |
|---|---|---|---|
| `clut` | 1,430 | `hw_front_end.cpp:390-418`; `FlushReason::ClutLoad` | The pending indexed batch ends because a CLUT load changes its palette (the chapter 07.3 one-palette-per-batch rule) |
| ... of which same texture | 1,010 | `clutFlushSameTexture_`, `415-417` | The T8 copy-back strips: same TEX0 base/indexed fields, only the palette changes |
| `prim` | 211 | `DecideFlush` rule 1, `hw_front_end.cpp` | Primitive class or attributes changed |
| `texture` | 170 | `DecideFlush` rule 8 | TEX0/TEX1/CLAMP/TEXA changed (texture state) |
| others (`test`, `autoflush`, `capacity`, ...) | remainder to ~1,930 batches | `DecideFlush`/`AutoflushWanted` | Test/scissor/scanmsk, overlap splits, vertex capacity |
| `gif_parse` | 3.80 ms | `[core2] exclusive` | The per-primitive front end (parse, `ShouldDraw`, `AppendPrimitive`, `Kick`, strips), not a flush reason |

The 1,010 same-texture loads are **not** a texture-cache or CLUT-decode event: the texture does not change; the load
writes the palette buffer, and the front end ends the batch because the next primitives would see a different palette.
The `clut` flush is a **batch boundary**. The 211 `prim` and 170 `texture` are the other state boundaries; the
`gif_parse` is the front end's per-primitive work (26-27k prims, 1,930 batches; TASK-171).

### 2.2 Which mechanism can merge without violating the tone ordering

- **Same texture, same palette (no actual change):** the CLUT load is redundant; `ClutUnit::ProcessWrite` already skips
  it (`hw_clut.cpp:241-245`), and the IN-541 probe found only 5% identical whole palettes. No gain here; the 1,010 are
  real palette changes.
- **Same texture, different palette, batch kept:** two designs:
  1. **Palette segments** (TASK-168): keep the batch, copy the palette, draw per-segment palette rows. Under a tone run
     the copy-back strips are **skipped** by `ToneHook`; the `Split` route is correct but measured **+3-4% GS thread**
     on George V vs the flush (TASK-168b); on the license the filter refuses before the segment forms. Not the license
     win.
  2. **CLUT-flush scope** (TASK-215b B): keep the batch if the loaded halfwords do **not** intersect the pending
     batch's **used** halfwords (`ClutHalfwordsWritten`/`ClutHalfwordsUsed`, `core2_renderer.cpp:894-930` in the docs
     branch); the load still happens, the batch's used palette entries are unchanged, and the next batch uses the new
     palette. This is exact and cheap, but it **cannot cut the 1,010**: their palettes do change in the used entries.
     It can cut the **other** CLUT loads whose used entries are untouched (the `1,430 - 1,010` remainder), and it is
     independent of segments/tone.
- **Same texture, palette changed, batch dropped:** the **tone-strip drop** (section 3): the T8 copy-back strips are
  skipped by the tone hook; drop them **before** the front-end `FlushBatch` on the CLUT load. This removes the 1,010
  batch boundaries without keeping the batch, without segments, and without a palette row. This is the license's
  mechanism.
- **Different texture (170):** a batch has one texture; merging would need per-primitive texture binding
  (bindless/descriptor indexing) and a front-end batch key change; large, not the first.
- **Primitive class/attributes (211):** a batch has one PRIM state; merging would need per-primitive state; the
  SCISSOR/XYOFFSET batching (TASK-215b A) covers only the culling-only state, not class/attributes; the 211 stay until
  TASK-171/per-primitive work.
- **The 1,010 are the tone strips**: the batch is going to be **skipped** by `ToneHook` (`3505-3512`) or
  `DropOnArrival` (`4158-4167`); the front-end flush and the batch work are pure overhead. The segment path tries to
  keep the batch; the front-end drop avoids it entirely.

## 3. The smallest exact design

### 3.1 The front-end tone-strip drop (recommended for the license)

**What:** before `FlushBatch(FlushReason::ClutLoad)` in `FrontEnd::OnTex0Write`, ask the renderer whether the pending
batch is a **tone copy-back strip**: a T8 sprite batch (`ToneShape`) while a tone run is active (`toneActive_`) with
the run's FBMSK (`toneFbmsk_`) and `PossibleShuffle` (the same predicate as `ToneHook`/`DropOnArrival`,
`core2_renderer.cpp:3505-3512`, `4158-4161`). If yes, **discard the pending batch** (clear it, do not call
`FlushBatch`, do not count a `clut` flush) and let the CLUT load proceed. The **first** batch of the run has
`toneActive_ == false` and is **not** dropped; it reaches the renderer, applies the tone pass, and sets the run state
(`3514-3561`). The strips after it are dropped. When the run ends (FBMSK changes or a non-T8 batch), the front end stops
dropping and the next batch is normal.

**Why exact:** the dropped batches are exactly the batches `ToneHook` returns true for without drawing
(`3505-3512`) or `DropOnArrival` discards (`4158-4167`); the tone pass is unchanged because the first batch still
reaches the renderer; no pixel is drawn by a dropped batch; the palette loads still happen (the CLUT unit buffer and
`beforeLocalRead` are unchanged), so the next batch sees the correct palette. The renderer's predicate is authoritative
(`toneActive_`, `toneFbmsk_`, `PossibleShuffle`, `ShuffleSource`); the front end only asks.

**What it saves (Hyp):** the 1,009 `clut` flushes after the first become 0; the **first** batch of the run keeps its
flush (it must reach the renderer for the tone pass), so the `clut` 1,430 -> about 421 and the same-texture 1,010 -> 1;
the batch count drops by ~1,009 (from ~1,930); the front-end `FlushBatch`/`DropOnArrival`/batch counters and the 1,009
`Batch` objects/`ApplyStRounding`/`NoteBatchWritesToClut` go. TASK-134 measured the George V CLUT path at 2.0 ms for
~1,500 flushes (1.5 ms of it the flush), i.e. about 1.3 us per flush; 1,009 would be about **1.0-1.3 ms** of GS worker,
plus the front-end batch work (Unknown exact share). It does **not** remove the `gif_parse` (the GIF parser still reads
the strips; the `AppendPrimitive`/`Kick`/`ShouldDraw` after the drop can be skipped only if the front end drops before the
append, which the same hook can do at the first kick of a strip) or the CLUT load itself (the `ClutUnit::Copy`).
Expected **Hyp 1-2 ms** GS worker on the 9060 XT (8.08-8.25 -> ~6.3-7.2 if the parse part remains); the RTX 3060's
19-20 would fall less than 16.7, so this is not enough alone.

**The invariant:** a batch the tone hook would skip is never drawn; dropping it early is a no-op for pixels; the
tone run's first batch and the run state are untouched. The **barrier/visibility** requirement is the same as the tone
hook's: the target's dirty/pending rects are applied on the first batch (`ApplyDirty`, `3532`); the dropped strips write
nothing, so no new barrier; the segment palette rows/GPU CLUT are not involved (the strips are not drawn). The
**tone's palette** is CPU (`DecodeSegmentPalette(batch,0)`, `3531`); the license's palettes are CPU loads, so no
`ReadClut`.

**A/B and fixture:**
- `GT4_CORE2_TONE_STRIP_DROP=0/1` (default 0 until measured); the A/B on license B-1, 1x, 3 off/3 on interleaved;
  `[core2] batch flushes/vblank` `clut=`/`clut_same_texture`/`clut_kept`/`segmented_batches`; the `[core2] palette
  segments ... tone_refused`; the `[core2] exclusive` `gif_parse`/`batch`/`clut`/`draw`; the GS worker/wall/p99.
- Frames identical on license B-1 (the tone/colour-filter frames), the George V lap, the night race; the tone/colour-
  filter sheet; the `hw_core2_tone` fixture extended with a T8 strip sequence after the run's first batch (the strips
  dropped, the tone pass equal, the following non-strip batch drawn); the `hw_m3` front-end fixture for the hook's
  decision order.
- The **owner's visual test** on the colour filter (the strips are not visible; the tone result unchanged); the
  `GT4_CORE2_TONE_TRACE` to confirm the run/first batch.

**Risk:** the front-end predicate must match `ToneHook` exactly (the run's FBMSK, `PossibleShuffle`, `ShuffleSource`,
the `toneEarlyDrop_` A/B); the first batch/run state; a batch that changes texture/class while the run is active; the
tone's `ApplyDirty`/target; the CLUT load's `beforeLocalRead`; a replacement loader; the `paletteSegments` interaction
(the front-end drop should be independent of `PALETTE_SEGMENTS`; it is a new hook). The fallback is the current flush.

### 3.2 The alternatives (and why they are not the license first)

- **Keep the batch across a CLUT load when the used palette rows are unchanged** (the CLUT-flush scope, TASK-215b B):
  exact, cheap, but the 1,010 have changed palettes; it cuts the other CLUT loads. The batch is translated after the
  load; the used entries are unchanged; the palette row for the used entries is the same; the unused entries can change
  (a new row key maybe, no pixel difference). **The IN-541 negative was about skipping the load (whole-palette
  identity), not merging the batch**; the scope merge still performs the load and keeps the batch. This is the right
  exact design for **non-tone** loads, not the 1,010.
- **Palette segments under the tone** (TASK-168 with the filter off / the `Split` route): correct, but measured +3-4%
  on George V vs the flush; the license's strips are exactly the case the filter refuses; do not re-enable without a
  new reason. If the front-end drop is not accepted, the `Split` route is the exact fallback, but the expected gain is
  negative on the strips.
- **Merge the 170 texture flushes:** per-primitive texture binding; large, no.
- **Merge the 211 prim flushes:** per-primitive state; large; the A (SCISSOR/XYOFFSET) only the culling state.

## 4. The tone-hook CLUT readback (`ReadClut`) on the license

- **Code:** `SyncClutToCpu` (`core2_renderer.cpp:1257-1276`) returns **immediately** when
  `GpuPaletteKey(psm,cpsm,csa) == 0`; only a palette with a GPU-written halfword reaches `FlushClutMirror` and
  `Gpu::ReadClut` (`core2_gpu.cpp:3994-4008`, `submit(true)`). TASK-215 (IN-546) attributed the wait to the `clut`
  reason (`reasonWaits_`, `core2_renderer.cpp:601`; the attribution at `1263-1272`).
- **The license:** the license's CLUT loads are from GS memory (the T8 copy-back strips; `clutSource_` 0, no GPU
  palette writes); `GpuPaletteKey == 0`; `SyncClutToCpu` returns true; **no `ReadClut`, no `clut` wait**. The license's
  `clut` cost is the **flush/load**, not the readback. The tone pass uses the CPU palette (`DecodeSegmentPalette`,
  `3531`); no GPU CLUT read.
- **The night/George V:** when a palette's used halfwords were written by a GPU CLUT load (`GpuPaletteKey != 0`), the
  tone hook reads the 512 halfwords back (`ReadClut`) before `Gpu::Tone`. That is the `clut` wait reason; the
  TASK-215c audit proposed a tone shader that reads the GPU CLUT storage buffer (`s.clut`, bound in
  `core2_gpu.cpp:3631-3651`) and decodes the palette there, instead of the CPU `palette[256]`; the
  `core2_tone.comp` would use the `hw_glm_decode.comp` `gpu_palette_entry` expansion; the gate is
  `GpuPaletteKey != 0` / `clutSource_` and the `clut_reads` counter.
- **The license gate:** no `ReadClut` to remove; the tone shader change would be a **night/George V** item, exact and
  useful there, but not the license's 8.08-8.25 ms. The license's `clut` reason should be ~0; if the perf agent sees
  a nonzero `clut` wait on the license, the palette is being GPU-written (check `GPU_CLUT`/`clutSource_`), not the
  typical license.

## 5. Prioritized short list for the perf agent

The license is **CPU-bound** (GS 8.08-8.25, GPU 1.05, `gif_parse` 3.80, 1,430 CLUT/1,010 same); the tone segments are
the wrong lever. The order:

| # | Item | Expected (label) | Effort / risk | Exact proof |
|---|---|---|---|---|
| 1 | **Front-end tone-strip drop** (section 3.1): drop the T8 copy-back strips before the CLUT-load flush; the first batch of the run still applies the tone | **Hyp 1-2 ms** GS worker (the 1,009 flushes + batch work); not enough alone for the RTX 3060's 19-20 | 1-2 d; medium (predicate must match `ToneHook`) | `GT4_CORE2_TONE_STRIP_DROP=0/1`; license `clut` 1,430 -> ~421, `clut_same_texture` 1,010 -> 1; frames identical; the tone sheet; `hw_core2_tone` strip case |
| 2 | **TASK-171 per-primitive front end** (parse/append/strip path): the `gif_parse` 3.80, 26-27k prims, 1,930 batches | **H**: the largest license CPU item; already planned (rc15? TASK-171) | planned; measure | `[core2] gif_parse`/`batch_own`; frames; the license |
| 3 | **CLUT-flush scope** (TASK-215b B) for the **non-tone** CLUT loads: keep a batch when the loaded halfwords miss its used halfwords; the load still runs | **Hyp** a partial of the `1,430 - 1,010`; exact | ~1 d + counter; low | `GT4_CORE2_CLUT_SCOPE=0/1`; `clut` falls for unaffected loads; frames; CLUT fixtures |
| 4 | **SCISSOR/XYOFFSET batching** (TASK-215b A) for the 1,930 batches / `prim`/`test`/autoflush | **Hyp** smaller; exact; not the 1,010 | 1-2 d; low-medium | the front-end fixture; frames; the `test`/scissor/xyoffset counters |
| 5 | **Tone CLUT read in the shade** (TASK-215c) for the **night/George V** GPU CLUT (`ReadClut` -> `clut` wait) | **H** for the GPU-CLUT scenes; **not the license** | 1-2 d; low-medium | `clut_reads` 0, `clut` wait 0, colour-filter frames; night/George V |
| 6 | **Keep `GT4_CORE2_PALETTE_SEGMENTS` off**; do not retry the segment under the tone without the front-end drop | the TASK-168b +3-4% (George V); the license no-gain | none | the existing `palette segments` line; the 3/2-run A/B; the outlier 9.55 |
| 7 | **Texture/prim flushes (170/211)** and the per-primitive texture/state path | **Unknown**; a larger design | design decision | the flush line; the per-primitive texture/state study |
| 8 | **Owner decision** on the front-end tone-strip drop: a game-effect-specific exact skip (the strips are skipped by the tone today); the picture/tone test; the license's 1,010; the alternative is to leave the flushes | - | owner | the owner's visual test; the license F8 |

The **smallest exact design** is #1; the **largest planned** is #2 (TASK-171); #3/#4 are the cheap exact batch/CLUT
steps; #5 is the night/George V readback, not the license; #6 is a do-not-repeat; #7 needs a design decision; #8 is the
owner's call on a game-specific exact skip.

## 6. Open questions

1. The front-end tone-strip predicate: can the renderer expose `ToneHook`'s decision (`toneActive_`, `toneFbmsk_`,
   `PossibleShuffle`, `ShuffleSource`) at the front end's `OnTex0Write`/first kick without changing the tone pass? The
   `PaletteSegmentBatchFilter` is the existing precedent (`995-999`, `hw_front_end.cpp:486-489`); the new hook is a
   timing/ownership decision.
2. The exact share of the 1,010 that is the **flush** vs the **CLUT load** vs the **batch work**: the TASK-134 2.0 ms/
   1,500 is George V; the license's 1,430/1,010; the `[core2] exclusive` `clut`/`batch` on the license; the front-end
   drop's expected 1-2 ms is a Hyp.
3. The `tone_refused` counter: is 1.67M `toneDropsOnArrival` (both) or `paletteSegmentToneRefusals` (on only)? The
   print/line labels; the off run cannot have the segment filter; confirm which counter the perf agent read.
4. The license's `clut` wait reason: is it 0 (CPU palettes), or is some palette GPU-written on the license? The
   `clut_reads`/`GpuPaletteKey` on the license.
5. The `GT4_CORE2_TONE_STRIP_DROP` interaction with `toneEarlyDrop_` (`GT4_CORE2_TONE_EARLY_DROP=0`), the tone
   `ApplyDirty`, `GT4_CORE2_PALETTE_SEGMENTS`, and the replacement loader.
6. The front-end drop's effect on `gif_parse`: can it skip the `AppendPrimitive`/`Kick`/`ShouldDraw` after the parse,
   or only the `FlushBatch`? The TASK-171 interaction.
7. The frames/tone identity on the colour filter: the strips are skipped today; the front-end drop must be pixel-
   identical; the owner's sheet.
8. The RTX 3060 laptop's license p99 and the 1,010 flush share; the 9060 XT 8.08-8.25; the 19-20; the p99 missing.

## 7. Evidence, limits and what was not done

- Read: the rc15/TASK-168 code in `gt4-wt-215-coregs2-perf` (`d18542f2`): `core2_renderer.cpp` (the segment option,
  `PaletteSegmentBatchAllowed`, `PaletteSegmentAllowed`, `ToneHook`, `DropOnArrival`, `OnBatch`, `RoutePaletteSegments`,
  `SyncClutToCpu`, the print lines), `core2_renderer.hpp` (the counters), `hw_front_end.cpp` (`OnTex0Write`,
  `PaletteSegmentAllowed`, `BeginPaletteSegment`), `hw_front_end.hpp`, `hw_batch.cpp/hpp` (the segment helpers),
  `core2_gpu.cpp` (the segment rows/order), `cpuref_draw.cpp` (the reference), `hw_clut.hpp/cpp`; the docs branch's
  `core2_renderer.cpp` for the pre-segment CLUT/flush code; the TASK-215d/215c audits; the ledger/IN-507/517/508/168/168b.
- No build, benchmark, game run, code/data/pin/test change; no PCSX2/GSdx/ARMSX2 source; no owner data; no
  Telegram/PR/release.
- Limits: the `tone_refused=1.67M` interpretation (early drop vs segment refusal) needs the perf agent's exact counter;
  the front-end drop's gain is a **Hyp** (the 1-2 ms is scaled from the George V CLUT path, not measured on the
  license); the license's per-VBlank `clut`/`batch`/`gif_parse` split is not in the docs tree; the RTX 3060's 19-20 is
  TASK-171; the segment path is rc15 code read in the perf worktree, not the docs branch.
