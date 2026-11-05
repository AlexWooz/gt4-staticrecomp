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

TASK-178 (section 9) answers the first four rows on the desktop; the last still needs the owner's F8 runs.

## 9. TASK-178 results

Date: 2026-11-04/05. Branch `task/178-submit-after-wait` (base `release/1.3.0-rc15` `8255e507`). Agent status: **not
PCSX2-tainted** (TASK-027 implementation agent; no PCSX2 sources, fork or `private/tooling/pcsx2-*` opened). Machine: the
desktop (Ryzen 7 5700X, RX 9060 XT), Spec II, headless, unpaced, plain Release builds (`build/t178-spec2`, `build/t178-v200`).
The desktop was shared with two other agents' benches (TASK-168b, TASK-179) during this work, so every time below is
**indicative**; counts, hashes and identity results are exact. Logs: `build/evidence/` of the TASK-178 worktree.

### 9.1 What was built

- **The rule (O1).** `GT4_CORE2_SUBMIT_AFTER_WAIT=<K>`: after any waited submission in `Gpu` (`submit(true)`: `Reduce`,
  `ReduceMany`, `ReadClut`, and the diagnostic `ReadDisplay`, `ReadImage`, `ProfileReport`), the first mid-frame submission
  comes after K draw batches, then the cadence is `submitEvery` again. Any other submission (ring full, VBlank flush) ends
  the K window. `0` = off, which is today's behaviour; **the default stays 0** until the Avell A/B. The log line
  `[core2] submissions: <slots> slots, every <n> batches, after a wait <K> batches` shows the setting in every run.
- **Diagnostics** (no behaviour change), in the 600-VBlank lines:
  - `[core2] downloads/vblank=... with_readback=... targets/readback_download=... px/readback_download=...
    wait_ms/readback_download=... | drains/vblank=... gpu_idle_ms/drain=... (timestamps ...) cpu_gap_ms/drain=...
    after_wait_submits/vblank=... (K=..., every ...)`. The GPU idle is measured with the slot timestamps: from the end of
    the drained work's last command buffer to the start of the next submission's first. The CPU gap is the end of the fence
    wait to the next `vkQueueSubmit`.
  - `[hw] flare replies vsync=... replies=... hash=... | i_cpu_ms/read=... i_wall_ms/read=...`: a running FNV-1a hash
    over every 128-qword flare reply in stream order, and I_cpu: the GS worker's busy time from the end of the IN-522
    `FlareRead` to the start of the first VU1 job given that read's words (the IN-522 `TablePatch`, matched by job id
    through a 16-entry ring the VU1 transport fills before the job enters the GS stream).
- **CTest control** (`hw_core2_gpu_clut`): K=1 against off on two captures (stream over a GPU-read render, GPU-read ageing):
  images and memory equal, the rule ran (7 and 71 submissions by it, 0 off).

### 9.2 Identity (C)

- **Flare reply hash sequence**, 16 lines per run (the cumulative hash every 600 VBlanks), identical for K = 0, 8, 16 and 32:
  Hong Kong 6835 replies, final hash `3f22899e9703be24`; Motegi 6588 replies, `817268cdcf807c3b`. The A/B runs (K = 0 and
  16, Hong Kong, Motegi and George V) repeat them: see 9.3.
- **Desktop frame and state gates** against the rc14 PGO references (`gates6.sh`, copy in `private/task178-tools/`), the
  candidate run with `GT4_CORE2_SUBMIT_AFTER_WAIT=16`: `wrong=0`.
  - Spec II George V, El Capitan, Tokyo R246 at 1x: race clock identical, simulation state identical (212,992 bytes),
    frames 24/35/35 identical.
  - v2.00 night race 3x: state identical (131,072 bytes), clock identical, 10 frames identical; Arcade path 3x: 14 frames
    identical.
  - Spec II George V lap 6x: 12 frames identical.
  - VU1 and VU0 against the interpreter (Spec II 1x and v2.00 night 1x): `verify_mismatches=0`; MTVU check: 8032 frames,
    0 mismatches.
- **CTest** (full, `-j 4`, desktop under the other agents' load): Spec II build 0 failures of 301; v2.00 build 1 failure of 301:
  `hw_core2_rop` failed once ("batches through colour attachments: 0") and passed twice alone (H: its pipelines compile
  asynchronously and were not ready under load; the rule is off by default and touches no ROP code). The only skip in both
  is `hw_replace_q07_pcsx2_dumps`. An earlier full run had `hw_cpu_sampler` fail once under load and pass alone.

### 9.3 Indicative desktop A/B, K = 0 against K = 16

Method: `private/task178-tools/ab.sh`, order A B B A A B (A = 0, B = 16), each round Hong Kong (reverse), Motegi Road
Course and the George V replay, all Spec II at 6x, headless, `GT4_HW_PROFILE_EVERY=600`; press timelines from TASK-168's
Avell notes. Windows: Hong Kong 4476-9876, Motegi 4460-9860, George 3000-8032. Table: median [min-max] of the run means
(`private/task178-tools/ab_table.py`). The runner waited up to 90 s for no other game process, then ran anyway and sampled
every 3 s: **15 of the 18 runs had another agent's game running** (2-33 samples), and all nine K = 0 runs did. That
load, rather than K, may explain part of the wall differences.

| Scene | Metric (ms) | K = 0 | K = 16 | Change |
|---|---|---|---|---|
| Hong Kong | wall per VBlank | 13.10 [13.03-13.28] | 10.68 [10.59-14.94] | -2.42 |
| | GS worker CPU | 9.19 [8.58-9.64] | 8.65 [8.61-9.19] | -0.54 |
| | read wait (download fence) | 2.94 [2.05-4.09] | 1.70 [1.70-4.99] | -1.24 |
| | GPU busy | 8.23 [7.69-8.90] | 7.44 [7.33-10.04] | -0.79 |
| | GPU idle after a drain, per drain | 2.25 [2.19-2.53] | 1.29 [1.18-2.28] | -0.96 |
| | CPU gap per drain | 1.50 [1.32-2.08] | 1.02 [0.93-1.03] | -0.48 |
| | I_cpu per read | 0.75 [0.73-0.78] | 0.71 [0.70-0.77] | -0.04 |
| Motegi | wall per VBlank | 8.44 [7.60-16.86] | 7.54 [7.48-7.85] | -0.90 |
| | GS worker CPU | 6.31 [6.10-7.27] | 6.42 [6.35-6.69] | +0.12 |
| | read wait | 1.80 [0.95-2.41] | 0.95 [0.89-0.98] | -0.85 |
| | GPU busy | 5.66 [4.94-6.22] | 4.80 [4.78-5.10] | -0.87 |
| | GPU idle per drain | 1.97 [1.23-10.00] | 1.07 [0.89-1.10] | -0.91 |
| | CPU gap per drain | 1.00 [0.88-8.08] | 0.69 [0.68-0.73] | -0.31 |
| | I_cpu per read | 0.50 [0.48-0.64] | 0.48 [0.47-0.51] | -0.03 |
| George V | wall per VBlank | 15.32 [14.39-15.65] | 14.17 [13.36-16.87] | -1.15 |
| | GS worker CPU | 13.06 [12.62-13.12] | 12.38 [12.12-14.53] | -0.68 |
| | read wait | 2.43 [1.19-2.52] | 1.52 [1.20-1.61] | -0.91 |
| | GPU busy | 7.74 [7.17-8.00] | 8.52 [6.99-9.91] | +0.78 |
| | GPU idle per drain | 3.40 [3.08-3.41] | 1.65 [1.58-1.97] | -1.75 |
| | CPU gap per drain | 2.67 [2.65-2.68] | 1.26 [1.23-1.53] | -1.41 |
| | I_cpu per read | 0.33 [0.32-0.33] | 0.31 [0.30-0.36] | -0.02 |

Per VBlank: drains 0.99 (Hong Kong), 0.69 (Motegi), 1.07 (George V), the same with both settings; the rule sent 0.94,
0.65 and 1.07 extra submissions per VBlank, one per drain that had draws after it. The flare reply hash sequences are
identical over all six runs of each scene (Hong Kong `3f22899e9703be24` after 6835 replies, Motegi `817268cdcf807c3b` after
6588, George V `1e108f7eb7013b1c` after 5746).

Reading (C for the counters, H for the wall):
- **The mechanism works as the study predicted.** The GPU idle after a drain falls by 0.9-1.8 ms per drain in every run,
  and the CPU gap with it. The download's fence wait falls by 0.85-1.24 ms per VBlank: the next drain finds less queued
  work, because the post-read work of the frame before reached the GPU earlier.
- **The wall falls in the medians (-0.9 to -2.4 ms)**, but the K = 0 runs all ran beside another agent's bench and the
  least disturbed pair at Motegi (r5, K = 0, 10 samples: 7.60; r6, K = 16, clean: 7.54) differs by only 0.07 ms. The
  K-only sweep before the A/B (one run each, others' games absent at the start and end of each run) gave Motegi 10.27 ms at
  K = 0 against 7.08 at K = 8, and Hong Kong 9.57 against 10.51 (that K = 8 run took 109 s against 79 s, H: disturbed).
  The desktop cannot separate the wall gain from the other agents' load; the Avell A/B decides.
- **No sign of a cost**: the GS worker CPU is equal or lower with K = 16 (the extra submission per drain costs less than
  the study's 0.05-0.2 ms estimate on this CPU, or is hidden by the noise), and I_cpu is unchanged.

### 9.4 The GPU profiles (`GT4_CORE2_GPU_PROFILE_AT`, 6x, desktop; C numbers, H reading)

Each command is isolated by barriers and timed on its own, so the totals are the commands' own GPU cost, not the frame's
GPU time. The profiled VBlank's commands start at the end of VBlank V and stop at the end of V + 1, so they include the end
of one game frame and the start of the next. Another agent's game was running at the time (GPU contention possible).

**Motegi, VBlank 5100** (`press-prof-mot6-5100`). The 6x dump of VBlank 5101 shows the race looking toward the sun: the
washed-out, hazy sky of GT4's sun glare and the following cars' headlights (C, frame); the sun disc itself is above the
frame (H). The download that frame reads back the table (4 px). 513 commands, 4.24 ms:

| Kind | GPU ms | Share |
|---|---|---|
| opaque world geometry (indexed textures, depth test and write), 119 batches | 0.62 | 15% |
| compute fills (frame and depth clears), 13 | 0.49 | 12% |
| full-screen passes over targets (glare/post: blend 0211, copy, alpha write; 10.3 Mpx each) | 0.89 | 21% |
| full-screen untextured overlays (blend 1222 x2, 0101 x2, a 31.9 Mpx depth pass) | 0.51 | 12% |
| the light-sampling chain's small draws (64x192 scratch and 64x32 table, at 6x) | 0.24 | 6% |
| CPU-word patches and dirty patches | 0.24 | 6% |

After the table's last write (H: where the read waits): 0.81 ms of GPU work, of which the full-screen copy 0.27, the 30
additive flare sprites 0.16 (tiny, about 5 us each), a fill 0.16, the sun-glare overlay (untextured, blend 1222) 0.11 and two
untextured full-screen 0101 passes 0.08. So the flares themselves are cheap; what follows the read is a handful of
full-screen passes at 10-23 Mpx each.

**Hong Kong, VBlank 7000** (`press-prof-hk6-7000`; dusk, a street lamp in view). 1193 commands, 8.49 ms:

| Kind | GPU ms | Share |
|---|---|---|
| dirty patches (GLM rows into targets after EE texture uploads), 323 | 1.55 | 18% |
| full-screen depth-tested passes sampling a target, 4 (41 Mpx) | 0.84 | 10% |
| opaque world geometry, 71 batches (46 Mpx) | 0.61 | 7% |
| compute fills, 14 | 0.49 | 6% |
| indexed blend 0101 with depth test (34 batches, 27 Mpx) | 0.44 | 5% |
| GLM writes of targets, 118 | 0.42 | 5% |
| other full-screen target passes (blend 0211, 0101, copy, alpha write) | 1.13 | 13% |

After the read: 2.81 ms, led by the four full-screen depth-tested target passes (0.84), a 0101 target pass (0.30), the copy
(0.26), two untextured 0122 overlays (0.21) and 101 dirty patches (0.21).

**Ranking for O5 (H).** The GPU cost at 6x is the full-screen post and glare passes (Motegi about 33%, Hong Kong about
28% of the isolated time) and, in Hong Kong, the 323 dirty patches (18%). The light-sampling chain is about 6% and the flare
sprites under 4%. The cheapest O5 targets are therefore the full-screen passes after the read and Hong Kong's dirty patches,
not the flares.

### 9.5 The two traces

**`GT4_CORE2_READBACK_TRACE`, Hong Kong 6x, VBlanks 4476-9876** (`press-trace-hk6-rb`, C). 4,591 download readbacks
(0.85 per VBlank), 1.24 targets each:

| Targets reduced together | Count | Mean native px |
|---|---|---|
| the table only (64x32 CT32 target, `bw=1`) | 3,635 | 4.8 |
| a 128x128 Z24 target (`bw=2`) + the table | 431 | 16,391 |
| a 640x224 CT24 target (`bw=10`) + the table | 180 | 108,800 |
| CT24 640x224 + Z24 128x128 + the table | 104 | 46,281 |
| up to four CT24 640x224 targets + the table | 56 | 89,000-266,000 |

- **The extra targets are other-view targets over the table's blocks:** 640x224 CT24 targets (499 reductions, 78,500 px
  on average, often the whole 143,360 px) and a 128x128 Z24 target (636, always its whole 16,384 px). The table moves in
  memory from frame to frame (BP 13472, 13504, 12864, 13888, 14048, ...) and lands inside the memory of old 640x224 render
  targets. Per VBlank that is 7,260 px of CT24 and 1,930 px of Z24 written back to memory for 4 px of table: about 99.9% of
  the download's pixels. This is exactly O3's case (limit an other-view target to the read's blocks), and it also leaves
  those targets undrawn, which costs later reads.
- **Motegi** (1x run with dumps, `press-trace-mot1-dumps`): 0.64 download readbacks per VBlank, 1.04 targets each; the
  table alone in 88% of them. The readbacks come in stretches that match the frames: the sun-glare views (5040-5220,
  5520-5640, 6240) and front views with the cars' headlights toward the camera read the table back every VBlank; rear views
  with no light toward the camera (4740-4980, 6600-6780, 9000-9240) read none. This confirms the study's hypothesis (sun or
  lamp sampled) for Motegi (C for the correlation, H for the cause).

**`GT4_CORE2_STORE_TRACE`, Motegi Performance mode, VBlanks 8340-8700** (`press-trace-mot6p-store`, C). The
every-second-VBlank store of section 6 is, from VBlank 8422 to 8446 and in similar runs elsewhere:
`store write bp=15936 bw=1 psm=00 rect=0,0,15,15 target bp=15936 bw=1 size=64x64` followed by
`readback before write ... psm=49 size=64x64 area=0,0,63,63`. GT4 uploads a 16x16 CT32 image (and on other VBlanks a 16x10
PSMT4 image, `bw=2`) into the memory of a drawn 64x64 **Z24** target at BP 15936, and that target is read back whole first.
Why the exact merges decline it (code, C):
- IN-512 (bit patch) and IN-538 (block patch) take only targets of the 32-bit colour class; this target is Z24 (depth
  class), so both refuse;
- IN-539 (store on the GPU) accepts depth targets (multi-target on) and the CT32 write fills whole blocks, but it is off by
  default (`GT4_CORE2_WHOLE_BLOCK_WRITES=1`, measured as a loss on George V at 2x in rc13); the PSMT4 16x10 write is not on
  block cells, so IN-539 could not take that one anyway.
- Desktop cost: 0.25 ms of store wait per VBlank over 8400-9000 (0.28 stores per VBlank); the study's Avell figure is 2.2-3.7
  ms per store. Exact fix candidates (H): extend IN-512/IN-538 to the depth-32 class (the merge is by 256-byte block and both
  layouts hold one 32-bit word per pixel, but the Z32 block order differs, so the bit positions must come from the Z layout),
  or enable IN-539 only when the target under the write is a depth target.

### 9.6 Numbers at K = 0 (desktop, means over the race window; C)

Hong Kong and Motegi from the K sweep's K = 0 runs (no other game at their start and end), George V from the A/B's K = 0
runs (median).

| Scene | Downloads with a readback /VBlank | Targets per readback | Pixels per readback | Drains /VBlank | GPU idle per drain | CPU gap per drain | I_cpu per read |
|---|---|---|---|---|---|---|---|
| Hong Kong 6x | 0.86 | 1.23 | 10,540 | 0.99 | 1.36 ms | 1.18 ms | 0.66 ms |
| Motegi 6x | 0.63 | 1.04 | 518 | 0.69 | 3.48 ms | 3.02 ms | 0.49 ms |
| George V 6x | 1.00 | 1.01 | 69 | 1.07 | 3.40 ms | 2.67 ms | 0.33 ms |

- The GPU idle after a drain is mostly the CPU gap: the GS worker recording up to 64 batches before the next submission.
  At Motegi the GS worker also waits for the EE there (headless, the EE-bound scene), so part of that idle is not the
  cadence's (H); two intervals at the end of the run averaged 5 and 19 ms per drain.
- I_cpu is 0.3-1.4 ms per read by interval on the desktop (Hong Kong 0.66, Motegi 0.49 on average), and its wall time equals
  it: the GS worker is busy all the way from the read to the flare job. Below the study's 1 ms bar for O4 on this CPU;
  the Avell's slower CPU decides.

### 9.7 Recommendation

- **K = 16 on a discrete GPU**, as the study suggested: identity holds at 8, 16 and 32, the gates pass at 16, and on the
  desktop it halves the GPU idle after a drain in all three scenes with no CPU cost seen. 8 brought the idle a little
  lower (Motegi 0.78 ms per drain in the K = 8 sweep run, 1.07 at 16 in the A/B), but each K submission then carries less
  work; that and an integrated GPU's K are for the Avell and the notebook to show.
- **Keep the default at 0 until the Avell A/B** (study section 7's rule: the wall at least 0.3 ms/VBlank lower on two scenes
  with no loss on the third). The kit is ready in the worktree's `build/t178-avell-kit/` (`prepare-kit.sh` already run:
  exe, ELF, cards, recording, `SHA256SUMS`; `run-ab.cmd 16` runs A B B A A B over the three scenes; `README.txt`); it was not
  run. If the Avell confirms, the default becomes 16 where `submitEvery` is 64 (discrete GPUs), one line in `Gpu::Init`.
- **Next, by these results:**
  - O3 (limit other-view targets to the read's blocks) for Hong Kong: 99.9% of its download pixels are other-view CT24 and
    Z24 targets (9.5);
  - the Motegi store: extend IN-512/IN-538 to the depth-32 class, or IN-539 for depth targets only (9.5);
  - O5 on the full-screen passes after the read and Hong Kong's dirty patches (9.4); the flares themselves are cheap;
  - O4 waits for the Avell's I_cpu: 0.3-0.75 ms per read on the desktop, under the 1 ms bar;
  - O2 (spin on the last fence) remains the next cheap A/B.
