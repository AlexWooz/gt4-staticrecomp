# CoreGS 2.0 study: the cost of the per-frame GS download, and exact ways to make it cheaper

Date: 2026-11-04. Read-only study for TASK-027 (CoreGS 2.0); no code changed, nothing run. Agent status: **not
PCSX2-tainted** (no PCSX2 sources, no `gt4recomp-gs` fork, no `private/tooling/pcsx2-*`, no `private/slice189-pcsx2-ref`).

Sources:
- **Code** at `release/1.3.0-rc15` (`f8a63bfc`): `core2_renderer.cpp` (`ReadFifo`, `Readback`, `AnswerDelayedDownload`,
  `StoreDrawnBeforeWrite`), `core2_gpu.cpp` (`Reduce*`, `submit`, `wait_all`), `core2_reduce.comp`, and
  `hw_gs_stream_sink.cpp` (GS worker, `FlareRead`, IN-522).
- **The gt4-decomp:** `RacePS2Base__render_world`, `func_0033EAB8`.
- **Docs:** GS-READBACK-STUDY, GAME-CODE-OPTIMIZATION-STUDY §9, IMPLEMENTATION-NOTES (IN-408/410/439/441/477/497/501/514/517/522),
  TASKS TASK-134.
- **Logs, read-only:** Avell `gt4-wt-134-game-code-study/build/evidence-avell/t168ab/*/log.txt` (Spec II, 6x, headless,
  unpaced), the owner's paced session `Y:\data\sessions\session-1793819856\game.log`, and the photo-mode trace
  `build/evidence/replay-photo-mode-rbt/log.txt`.

Labels: **C** Confirmed (code, data or log read), **H** High confidence, **Hyp** Hypothesis, **U** Unknown. Unless stated
otherwise, numbers are ms per VBlank, averaged over VBlanks 4800-9600 of the `-A-r1` run.

## 1. Summary

1. **What is downloaded:** once per race frame, GT4 reads its light-flare visibility table, 64x8 PSMCT32 (2 KiB). Only
   a few drawn pixels of it come from the GPU. No other download is frequent in a race (C).
2. **Who reads it:** the EE never does. The VU1 flare program reads it in the same frame (C).
3. **The cost is not the copy:** 512 nearest-sample reads, no 6x resolve (C). The GS worker submits, then waits for the
   **whole GPU queue** to drain (C). That wait is the GPU backlog at that point of the frame. It grows with the scale
   because the scene draws queued before the read are fragment-bound (C/H).
4. **Ceiling:** removing the wait entirely gains at most about 4 ms in Hong Kong and 2 ms at Motegi (wall minus
   max(GS CPU, GPU); H). Past that, Hong Kong at 6x on the RTX 3060 laptop is GPU-bound: 15.2 ms GPU busy (C).
5. **The owner's slowdowns:** in the owner's paced session, spikes with a readback carry 18.4 ms of GPU work on average
   (C). The GPU itself is over budget there, so only less GPU work fixes them (H).
6. **The dependency is real:** the table needs the depth of the whole scene before it, and the flares that use it come
   right after (C, decomp order). Exact work can only cut the scheduling latency around the wait, overlap the little CPU
   work between the read and the flares, make the GPU work cheaper, or drop the round trip with a bit-exact GPU-side
   flare.
7. **First step (section 7):** restart the GPU immediately after the drain, and add counters for the open unknowns.
   Exact, small, measurable.

## 2. What GT4 downloads each frame (question 1)

**The read (C).** A LOCAL->HOST transfer of a 64 x 8 PSMCT32 rectangle at (0,0) of the flare result buffer: 2,048 B,
128 qwords. Evidence, from Performance-mode Motegi and the sink:
- the IN-441 rule (`Ct32`, 64 x 8) matched exactly 600 per 600 VBlanks (`hidden_flare_tables`);
- `delayed_downloads=0`, and `download_readbacks` rose by only 11 in 5,400 race VBlanks;
- the sink counts 1.00 `read_fifos` per VBlank, all through IN-522 (`reads without the EE waiting`).

**Content (C, GS-READBACK-STUDY §2).** One pixel per sampled light (course lamps, headlights, the sun; up to 512), each
the visible fraction x strength. Per light the GS draws a depth-tested square into the frame buffer's alpha, copies the
patch into a 64x64 scratch buffer, box-filters it down to 1x1 by halving, and writes result pixel (n % 64, n / 64).

**What comes from the GPU.**
- Only `drawn` ∩ the rectangle (C, `Readback`). The night trace showed one row of 2-24 pixels of a 64 x 32 target (C,
  IN-439). The rest of the 2 KiB comes from GS local memory.
- In Hong Kong about 1.25 targets are reduced per download readback (1.0-1.5 by interval); at Motegi about 1.05 (C,
  counters).
- A target in another view that overlaps the table's blocks is reduced over its **whole drawn area**. Only same-view
  targets are limited to the rectangle (C, code). Which target that is in Hong Kong: U.

**Not every read waits for the GPU.**
- A readback happens only when a drawn target lies over the table (C, code). Downloads with a readback per VBlank, by
  600-VBlank interval (C): Motegi 0.39-0.97 (mean 0.63), Hong Kong 0.60-1.00 (mean 0.86), George V night 1.00.
- Per download that waits, that is about 3.3 ms at Motegi and 5.5 ms in Hong Kong.
- Hyp: in a day race the drawn table, and so the drain, comes with the sun or a lamp on screen. This would match the
  owner's "slow when the sun flare is on screen".

**Where in the frame (C decomp order; H that the stream follows it).** Late, at the end of the world pass
`func_0033EAB8`: after the sky, course, cars and other world draws, the flare begin `func_003B80E8` and the light samples
(`func_003985A0`, methods 0x3F0/0x3F8), comes the read `func_003B8848`. Then come method 0x3E8, `func_0033E828` and
`func_0033F1C8`, the flare and glare draws `func_0033F2E8`, the post passes and the display copy.

**Who reads the bytes.** The VU1 flare program, through a VIF REF tag (C). In normal mode the EE does not wait (IN-522,
C); the VU1 worker waits at the first flare packet. The **GS worker** is the thread blocked on the GPU (C): its
`FlareRead` time (`other_ms`) equals the readback time (Hong Kong 4.75 against 5.04, Motegi 2.05 against 2.17).

**Photo mode** reads something else (C, trace): 8x4 Z24 for autofocus. That read also reduced an overlapping
1024 x 192 colour area every second VBlank.

## 3. Why it costs several ms (question 2)

**Mechanism (C, code).**
1. `ReadFifo` flushes the open batch.
2. `Readback` takes the overlapping targets, oldest first.
3. `Gpu::Reduce` or `ReduceMany` records, in order:
   - `flush_draws` and the end of the render pass;
   - a `full_barrier`;
   - one `core2_reduce.comp` dispatch: one invocation per native pixel, nearest sample at the native centre;
   - another `full_barrier`;
   - `submit(true)` → `wait_all`: a wait for **every** in-flight slot, up to 16 on a discrete GPU (IN-517).
4. `writeRect` writes the words into memory.

**What each part costs.** The copy (≤2 KiB), the barriers and the CPU conversion are negligible: Hong Kong's readback
time minus its fence wait is about 0.1 ms (C). The rest is the drain: the CPU waits until the GPU has run all work
queued before the read. The GPU then idles until the next submission, 64 batches later (IN-410) or at the VBlank flush.

**What scales with the factor (C numbers, H reading).** Not the reduction: the backlog of fragment-bound draws ahead of
it. George V lap, t168ab B runs:

| Scale | Draw passes | GPU busy | Wait per download |
|---|---|---|---|
| 1x | 1.6-1.9 | 5.1-6.2 | 1.4-1.8 |
| 6x | 7.2-7.6 | 11.8-12.8 | 1.9-2.4 |

TASK-134 §9, on an earlier build: 1.3 ms at 1x against 4.5-5.5 ms at 6x.

Hyp, why a floor remains at 1x: latency. The light-sampling chain right before the read is a run of tiny dependent
passes behind full barriers (George: 660 full barriers and 120 render passes per VBlank, C). The GPU runs it at its
latency, and its tail is submitted only at the read.

**Per scene (Avell, 6x, Spec II, headless, C):**

| Scene | GS worker without the wait | Flare read (wait) | GPU busy | Worker total | Bound with no wait |
|---|---|---|---|---|---|
| Hong Kong, normal | 14.5 | 4.75 | 15.2 | 19.3 | 15.2 (GPU): -4.1 |
| Motegi, normal | 9.1 | 2.05 | 8.75 | 11.2 | 9.1 (GS CPU): -2.0 |
| Motegi, Performance | 10.5 | 0.03 | 7.6 | 10.5 | - |
| George V, normal (B run, VBlanks 3000-8032) | 19.9 | 2.3 | 12.8 | 22.2 | 19.9 (GS CPU): -2.3 |

- **A measured analogue** (TASK-134 §9, George V at 6x): IN-439 alone, with no wait at the read, took the wall from
  15.9-17.1 to 14.6-14.7 ms. The run was then GPU-bound at 14.1 (C).
- **Why the wait differs by scene:** it is large where the GPU's work for the scene approaches or exceeds the CPU's
  (Hong Kong) and small where the GS CPU dominates (George V). H, from the model "wait = GPU lag at the read".

**Spikes (C counts, H reading).**
- **Motegi:** in normal mode, 20 (A) and 17 (B) spike VBlanks retired over 20 ms of GPU work (40 ms on average, with a
  23.6 ms wait). In Performance mode (flares hidden, effects at 1x) there are 10 and 9 such VBlanks, about 30 ms each.
- **Owner's paced session:** 299 of 446 spike lines have a readback of at least 2 ms. They average a readback of 8.9 ms
  and 18.4 ms of GPU work.
- **Caveat 1:** a spike line's `gpu_busy` is GPU time *retired* in that VBlank. A drain retires all queued work, so it
  can cover more than one frame.
- **Caveat 2:** headless runs are unpaced. About 37% of Motegi VBlanks need no readback, and between such reads the GS
  worker runs up to 16 submissions ahead; the next drain then pays for all of it. Paced play drains less, but the
  owner's 18.4 ms still marks GPU-heavy frames.
- **Hyp:** the heavy part is the sun glare and flares at 6x, drawn right after the read and started late on the GPU.
  U until a GPU profile of such a VBlank exists.

## 4. Options, cheapest first (question 3)

"Exact" means that the 2 KiB reaching guest memory, and every frame, are identical to today's. The EE never reads them;
the VU1's input is what matters.

| # | Option | Identity | Expected gain | Effort | Risk |
|---|---|---|---|---|---|
| O1 | **Restart the GPU right after a drain** | exact (C) | 0.3-1.2 ms/VBlank with a readback at 6x (Hyp) | ~1 day | low |
| O2 | Spin on the last fence, wait once | exact (C) | 0.05-0.3 ms per drain (Hyp) | hours | low-medium |
| O3 | Limit other-view targets to the read's blocks | exact for the bytes (H) | CPU only, ≤0.2 ms in Hong Kong (Hyp) | ~1 day | medium |
| O4 | **Asynchronous read on the GS worker** | exact (H) | the CPU time between read and flares, U, likely ≤1 ms | 3-5 days | medium-high |
| O5 | **Cheaper GPU work around the read** | exact by requirement | the only lever for GPU-bound frames; U until profiled | varies | varies |
| O6 | Bit-exact GPU-side flare (GS-READBACK-STUDY R3) | exact only if bit-exact | the whole turn: 1.3-2.4 measured on George V, ~4 in Hong Kong | weeks | high |

**O1. Restart the GPU right after a drain.** After a waited submission, submit once after the next K batches (8-16),
then go back to every 64. Today the flare and glare draws after the read reach the GPU only 64 batches later, or at
the VBlank flush.
- **Exact:** submission boundaries never change output; frames and state were identical at 16 and 256 batches per
  submission (C, TASK-134 §9).
- **Evidence for a gain:** 64 -> 32 everywhere kept the 6x lap's wall at 17.26 while the GS CPU rose 0.8 ms (C), so
  about 0.8 ms of wait went away and was paid back in per-submission CPU (roughly 0.05-0.2 ms per extra submission,
  Hyp, from the counts at 64 and 128). One extra submission per drain should keep most of the gain (H).

**O2. Spin on the last fence, wait once.** A bounded `vkGetFenceStatus` spin on the last-submitted fence, then retire the
other slots without blocking: less wake-up latency and no 16 sequential `vkWaitForFences` calls. `GT4_HW_READBACK_SPIN`
is parsed but core 2 does not use it (C). Cap the spin: the 4-core laptops share cores with the EE and VU1.

**O3. Limit other-view targets to the read's blocks.** Reduce another-view target only over the rows that cover the
read's blocks. It saves large `writeRect`s and the texture invalidations they cause (the photo-mode read stored
1024 x 192 for 8x4, C), not the wait. `drawn` is one rectangle, so the rest of that target is read again later.

**O4. Asynchronous read on the GS worker.** At `FlareRead`, record the reduction (like `ReduceLater`), submit without
waiting and go on; the IN-522 `TablePatch` on the VU1 worker waits for a GPU-written sequence word (or a timeline
value), then copies the words.
- **Exact (H):** the bytes outside `drawn` are taken from memory at post time and those inside come from the GPU, which
  is today's result (memory as of the read, overwritten by the target as of the read). The cases
  `AnswerDelayedDownload` declines stay synchronous, and the target keeps `drawn`, so it stays authoritative.
- **Gain:** the GS worker's CPU between the read and the first flare VU1 job (the rest of `func_0033EAB8`, plus
  `func_0033E828` and `func_0033F1C8`): U, probably small. That work also reaches the GPU without a bubble.
- **Risk:** thread and fence ownership (no `vkResetFences` on a fence another thread waits on), and an IN-522-style
  deadlock review.

**O5. Cheaper GPU work around the read.** Hong Kong at 6x is GPU-bound (15.2 ms, 683 full barriers per VBlank), and the
owner's spike frames carry 18.4 ms of GPU work. Profile first: `GT4_CORE2_GPU_PROFILE_AT` on a Motegi sun VBlank and a
Hong Kong VBlank, and `GT4_CORE2_BARRIER_TRACE=1` around the flare chain. Likely targets (Hyp): the glare and flare
sprites at 6x, and the serial sampling chain (image-scoped barriers instead of full ones).

**O6. Bit-exact GPU-side flare.** The VU1 output that depends on the table would be applied on the GPU, so the table
never leaves it. Hard on two counts: a zero answer makes the game draw no flare at all (IN-441), so the value gates
emission, not only colour (indirect draws); and exactness needs PS2-float-exact arithmetic in the shader
(GS-READBACK-STUDY P1/R3; the ±1 LSB variant is not allowed in normal mode).

**Rejected (no gain or not exact).**
- **Download only the region read, or at 1x from a shadow:** already the case. Only the drawn pixels inside 64x8 are
  reduced, one texel per native pixel, with no resolve (C). Gain 0.
- **Render the sampling chain at 1x** (as IN-497 does in Performance mode): changes today's 6x bytes, so it is the
  owner's call. It is small and keeps the wait.
- **A fence over only the read's dependencies:** the table depends on the whole scene's depth, on one in-order queue.
  Gain about 0 (H).
- **Cache when unchanged:** a frame with no drawn table already skips the readback (C). Otherwise the change is known
  only after the GPU has run. Gain 0.
- **One frame late (IN-439) or zeros (IN-441):** not exact. The owner keeps the normal mode exact (2026-11-04).

## 5. What Performance mode does (question 4) (C, code)

`GT4_CORE2_PERFORMANCE=1` turns on:
- **IN-441:** the 64x8 CT32 read is answered as zeros, so the VU1 draws no flare or glare. IN-477 answers the EE at once
  and the GS worker posts a `SkippedRead`, so nothing waits for the GPU. This is what removes the readback (Motegi
  2.21 -> 0.13, A/B medians).
- **IN-439** delayed downloads (nothing left for them in a race, `delayed_downloads=0`), **IN-497** effect targets up to
  256 wide at 1x, **IN-501** no road sheen, and no present resample on integrated GPUs.

Each of these changes what the game reads or what is drawn, so none moves into the normal mode exactly. The exact part,
the EE not waiting, is already there as IN-522. The exact substitutes are O1-O6.

## 6. The spikes that are not readbacks (question 5) (C numbers, H reading)

**Motegi, Performance mode, VBlanks 8358-8546** (505-580 batches per VBlank, against a 307 median): the GS runs 19-23 ms.
Means over the 67 spike lines: `upload` 4.5, `draw` 3.0, `translate` 1.5, `clut` 1.3, `texture` 1.2, and about 7.7 not
itemised (`gif_parse` + `batch_own` = 5.2 ms in the 8400-9000 interval).
- **Inside `upload`, every second VBlank:** a **store-before-write readback** (IN-411, "store 1") of 2.2-3.7 ms, which is
  a full drain (gpu_wait 1.7-3.2).
  - The GPU keeps up here (about 9 ms per VBlank), so that drain lands directly on the GS critical path.
  - Normal mode has it too (VBlanks 8511 and 8529).
- **Without the store,** `upload` is still 4-6 ms for the same ~170 uploads and 1.9 MB (C). Hyp: the rest is the
  IN-512/538 merges, since `written` reaches 0.79 and the bit patches grow in that interval.

**Owner's paced session, race spikes without a readback.**
- GS 19-25 ms, of which upload 1.4-2.2, texture 1.1-2.2, translate 1.0-1.8, draw 2.0-3.8 and clut 1.1-2.1.
- About 10-14 ms is not itemised. In average intervals `gif_parse` is 2.4-3.6 and `batch_own` 1.1-1.6. Whether these
  grow with the geometry or the thread is being preempted: U.
- VBlank 12304 shows clut 8.1 and gpu_wait 6.3 with no readback. Hyp: `ReadClut`, which is also a full drain.

**Cheapest next steps.**
1. `GT4_CORE2_STORE_TRACE=1` over Motegi 8400-8640, to see why IN-512/538/539 decline that store, then extend the GPU
   store to it. Exact; about -1 to -1.7 ms per VBlank over the stretch (H).
2. A `GT4_CPU_SAMPLER` profile of the same stretch, to split `gif_parse`/`batch_own` growth from preemption.

## 7. Recommended first step (one task, measurable)

**Task:** "CoreGS 2.0: restart the GPU after a waited readback, plus flare-read counters" (O1).

**Behaviour (exact).** After any `submit(true)` in `Gpu` (Reduce, ReduceMany, ReadClut), the next mid-frame submission
comes once after `GT4_CORE2_SUBMIT_AFTER_WAIT` batches, then the cadence is `submitEvery` again. Default 16 on a discrete
GPU, 0 = off for A/B. One variable per candidate (K = 8, 16, 32); a frame-start variant only after this one is measured.

**Diagnostics (no behaviour change, in the 600-VBlank lines):**
- GPU idle after a drain: from the end of the fence wait to the next submission's start timestamp;
- I_cpu: the GS worker's busy time from the end of `FlareRead` to the first VU1 job carrying a `TablePatch` (sizes O4);
- targets and pixels per download;
- an FNV hash of each flare reply.

**Gates.** The reply hash sequence is identical with the switch on and off; the desktop frame and state gates are equal
(three recordings, v2.00 night and Arcade, 1x and 6x, VU and MTVU verify); CTest and Python pass.

**Measurement.** On the Avell: Hong Kong, Motegi and the George V lap at 6x, headless, order A B B A A B, the switch
checked in each log. Report the wall, the GS worker, the read wait, GPU busy, the idle and I_cpu. Keep the change if the
wall falls by at least 0.3 ms/VBlank on two scenes with no loss on the third.

**Same task, no code.** `GT4_CORE2_GPU_PROFILE_AT` on a Motegi VBlank with the sun confirmed by a frame dump and on a
Hong Kong VBlank, to rank O5's passes; `GT4_CORE2_STORE_TRACE` for section 6.

**Then:** O2 next, as a cheap A/B; O4 only if I_cpu is at least 1 ms; O5 for whatever tops the GPU profile.

## 8. Unknowns

| Unknown | How to resolve it |
|---|---|
| Which extra target Hong Kong reduces with the table | `GT4_CORE2_READBACK_TRACE` |
| Why 63% (Motegi) and 86% (Hong Kong) of reads need a readback (Hyp: sun or lamp sampled) | the trace plus frame dumps |
| The GPU cost of the glare, the flares and the sampling chain at 6x | GPU profile |
| I_cpu, the CPU work between the read and the flares | the section 7 counter |
| How much headless drains overstate paced play | owner F8, same scene, O1 on and off |
