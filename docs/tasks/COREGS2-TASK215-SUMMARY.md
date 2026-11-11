# CoreGS 2.0 performance round: TASK-215 / TASK-171 summary (2026-11-11)

One page for the owner. Labels: **C** confirmed by a measurement in this round, **H** hypothesis with the experiment
named, **U** unknown. Every number is the local desktop (Ryzen 7 5700X, RX 9060 XT), headless, shader cache warm, plain
release build; the GPU was **true stock** (10x GPU busy 14.267 against the historical 14.32), the CPU stock unless a line
says ~3.4 GHz (the CPU-down proxy). Branch `task/215-coregs2-perf`. Details: `COREGS2-PERF-BASELINE.md`,
`COREGS2-PERF-LEDGER.md` (IN-540..IN-555), `COREGS2-FLARE-REF-MAPPING.md`, `COREGS2-FLARE-VU1-INDEX-STUDY.md`,
`COREGS2-PER-RECORD-DESIGN.md`, `COREGS2-PER-BATCH-DESIGN.md`.

## 1. What was kept

| change | measurement | identity | commit |
|---|---|---|---|
| **IN-540** aliased depth fill (`DepthBlocksInColour` takes the block from the address tables) | batch stage **1.129 -> 0.883 ms**/VBlank at night 3x (-22 %), GS 7.51 -> 7.33, wall 7.80 -> 7.64; license 1x GS 9.03 -> 8.71 | night 3x 9/9, 12x 4/4, Arcade 14/14 frames identical; aliased-depth counters equal | pre-round |
| **IN-547** scissor view (`GT4_CORE2_SCISSOR_VIEW`, default on) | license B-1 1x, 2+2: `gif_parse` **4.010 -> 3.898 ms (-2.8 %)**, GS worker **8.275 -> 8.138 (-1.7 %)** | license 4/4, night 3x 2/2 frames identical | `41405c37` |
| **measurement tooling** (TASK-215e) | p50/p95/p99, 1 % low, budget over 16.7/20 ms, worst intervals, histogram, per-VBlank CSV, scene tags; **23/23** `test_cpu_summary` | no native behaviour | TASK-215e |
| **diagnostics, all off by default** | `GT4_CORE2_SCISSOR_VIEW`, `GT4_CORE2_TONE_STRIP_DROP`, `GT4_CORE2_FLARE_TRACE[_DRAWS]`, `GT4_FLARE_REF_HIDE`, `GT4_FLARE_TABLE_WORD/_FORCE`, `GT4_HW_FLARE_PATCH_TRACE`, `GT4_CORE2_SPIKE_*`, `GT4_CORE2_PERF_FLARES` | none changes a pixel unless set | several |

## 2. What was reverted or closed

| item | result | reason | ledger |
|---|---|---|---|
| Palette segments under the tone hook | **+0.61 ms** (GS 8.795 against 8.183; `gif_parse` +0.56) | exact (frames identical) but a segment carries a 1 KiB CLUT copy: reverted | IN-548 |
| Per-record vertex conversion | **~0** (an extra conversion pass costs nothing measurable; the whole inlined `Translate` body is 0.135 ms) | `ToGpuVertex` is a pure function of `Vertex`, so moving it adds a store; replaced by the corrected design bound 0.15-0.3 ms (H) | IN-552 |
| Per-batch flush merge | 61 % of batch pairs share the signature, **980.9 CLUT flushes mergeable** on the license - but each merged batch is ~1 primitive at ~0.1 µs -> **~0.1 ms** | below the 0.2 ms bar; the ~500 real draw batches are not mergeable by signature | IN-554 |
| Flare readback, path 2 (cheaper EE read) | **no exact same-frame form** | the table is a GPU occlusion result; avoiding the wait means zeros or a one-frame-late answer (decision 0073, rejected) | IN-549..551 context |
| Flare readback, path 1 (GPU per-light multiply) | **closed** | the four REF sites carry no address (one unique tag pattern, identical data, no VIF command in front) and are the game's per-frame visibility data; no stable LQ or site to emit a per-light index from | IN-553, IN-555 |

**Do not retry:** palette segments under fresh copies of the CLUT buffer; moving the vertex conversion; a per-batch merge
of the CLUT-load flushes; the flare readback through a seekable table window (there is no window). Retrying any of them
needs a new mechanism, not a new measurement.

## 3. Scene matrix at 3x (true-stock GPU; interval statistics over the stated window)

| scene | window | wall (p50 / p99) | GS worker | GPU busy | EE critical | split |
|---|---|---|---|---|---|---|
| license B-1 1x | 2,400-3,500 | 8.885 / 9.190 | 8.475 | 0.935 | 8.288 | CPU/GS-bound |
| night race 3x | 7,800-11,400 | 7.100 / 9.800 | 6.850 | 2.260 | 7.055 | CPU/GS-bound |
| Opera Paris 3x | 20,000-21,600 | 8.930 / 9.600 | 8.620 | 1.640 | 4.943 | CPU/GS-bound |
| S.S. Route 5 3x | 20,000-21,600 | 7.780 / 10.230 | 7.420 | 2.330 | 4.825 | CPU/GS-bound |
| Tsukuba Wet 3x | 20,000-21,600 | 8.570 / 9.720 | 8.200 | 2.360 | 4.979 | CPU/GS-bound |
| George V rally 3x | 20,000-21,600 | 9.680 / 14.420 | 9.350 | 3.230 | 5.207 | CPU/GS-bound, heaviest |
| George V reference (Ford GT replay) | **20,400-21,300** | 11.975 / 13.950 | 11.680 | 3.425 | 13.526 | in-race, rally load |

- **C** the four streamer scenes are CPU/GS-worker bound at 3x: GPU busy **1.6-3.2 ms of 16.7**, so their frame time is the
  GS worker/EE, not the GPU.
- **C** the George V reference window 20,400-21,300 is **in-race** (1,028 batches/VBlank against 597 before it, race start
  near 20,300); its 13.5 ms EE critical is the rally's own load.
- **C** CPU-down proxy (~3.4 GHz, boost off): 3x wall 9.91 (+32.7 %), license 11.95 (+35.3 %), 6x 10.52 (+19.4 %); the
  3x/license scenes scale with the clock, so they are CPU-bound. Exact clock: Unknown (estimated ~3.4 GHz from the 480 ms
  benchmark against 683-691 ms boost-off).
- 10x/12x stress points (true stock): wall 15.533 / 21.269, GPU busy 14.267 / 19.980, flare wait 5.767 / 9.853 - 10x is
  not GPU-bound, 12x is.

## 4. The remaining lever

- **C** the license spends **2.99 ms/VBlank** after the front end: `draw` 0.93 + `batch_own` 0.64 + `clut` 0.48 +
  `swizzle` 0.49 + `translate` 0.45, over 1,930 batches. The ~500 **real draw batches** cost ~5.7 µs each (≈2.85 ms); the
  1,430 CLUT-load flush batches cost ~0.1 µs each and are **not** the cost (IN-554).
- **H** a **draw-level** reduction (fewer/larger draws per batch, or cheaper per-draw state) is the only remaining CPU
  lever: 2-3 days, uncertain, and it needs a new design question (what makes a draw cheap: the draw-set key, the palette
  row, the state uniform, the submission bookkeeping). It is not a batch merge and not a record format.
- **C** the flare wait stays at **0.15 / 0.95 ms** at 3x / 6x and 5.77 / 9.85 ms at 10x / 12x; the readback is closed as a
  target, so those numbers are the price of exact flare occlusion.

## 5. Options for the owner

| option | evidence | cost |
|---|---|---|
| (a) draw-level study | the only CPU lever left, 2.85 ms on the license; needs a design question first, not code | 2-3 days, uncertain |
| (b) test kit of the kept changes (IN-540 + scissor hoist + tooling) | both kept changes are exact and small (-22 % batch stage, -1.7 % GS on the license); a weak-machine/streamer test is the only way to confirm they help the reported cases | < 0.5 d to cut a kit |
| (c) focus 6x GPU | 6x is the first GPU-sensitive point (GPU busy 6.15 ms, flare wait 0.95 ms) and the only scene where the flare wait is near the bar | 2-4 days, unknown |
| (d) stop and integrate | every lever measured this round is closed with numbers; the kept changes are small but exact | 0 |

**Recommended: (b) then (d).** The kept changes are exact and already in the tree; a kit lets the streamer's weak machine
confirm them without another design round, and (a) should only start if the owner wants another 2-3 days on an uncertain
lever - the numbers say the remaining CPU cost is real (2.85 ms) but its cheap fixes are exhausted.

## 6. State and limits

- Branch `task/215-coregs2-perf`, HEAD `2ea07a7e`; commits this round: `41405c37`, `76f31922`, `ff97bd5c`, `354d6212`,
  `8326eef7`, `fd6c5de6`, `2cead55e`, `83862dce`, `a04071d4`, `7e6d9a85`, `b0524246`, `400e9f37`, `df2b3b26`, `2ea07a7e`
  (append-only, each with the `Assisted-By` trailer).
- Tests: **CTest 319/320**, the one failure the pre-existing rc15 `python_native_contracts` (`test_gs_live` accepts
  `GT4_GS_SCALE=1.5`); Python **827 with that one failure**; `test_cpu_summary` **23/23**.
- Disk: C: kept above ~2 GB (2.37 GB free at the close); only this worktree's reproducible dumps and its prof build were
  deleted, never owner data or the main checkout's `private/`.
- No PR, tag, release or Telegram; no game logic, save or readback behaviour changed beyond the diagnostics listed above.
- Limits: all numbers are one machine and one session; the GPU state is true stock but the card is a single sample (U); the
  interval p99 values come from 2-7 intervals of 600 VBlanks, so they are coarse (C).
