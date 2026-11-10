# CoreGS 2.0 headless benches

The tools behind `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`. They run the game without a person, with scripted pad
presses, on two scenes. Git bash on Windows; Python with numpy and Pillow for the two `.py` tools.

Needs (not in git, see `common.sh`): the main checkout's `private/reconstructed/SCUS_973.28.elf` and disc image, the
cards `private/coregs2-bench/night-card.ps2` and `private/slice110-menu-pilot/name-control4/card.ps2` (copied per run,
never written). Close the game first; never run while an owner session is running. No perf measurement while a build
runs.

| Tool | Use |
|---|---|
| `night-run.sh <name> <exe> [ENV=VAL ...]` | Night race (GT Mode, B-Spec): the reference scene. `SCALE`, `EVERY`, `FROM`, `FRAMES`; scene tag `GT4_BENCH_SCENE` (default `night`) |
| `arcade-run.sh <name> <exe> [ENV=VAL ...]` | Arcade path: menus, showroom, pre-race, race start. Scene tag default `ssr5-night` |
| `series.sh <tag:exe[:ENV=VAL,...]> ...` | Interleaved night runs with the 600-VBlank reports on |
| `cpu_summary.py <tag> ...` | Averages (unchanged: `gs_cpu`, `process_cpu`, `wall`, `ee_critical_path`, `frame_p50/p95`, `gpu_busy`, the `x_*` GS stages) plus the stutter sections: p50/p95/p99, 1% low, max, frames over 16.7/20 ms, worst intervals, histogram, optional per-VBlank CSV. See "Stutter measurement" |
| `frame_diff.py <run A> <run B> [--delete-identical]` | Identity check of the dumped frames |
| `pgo-build.sh <build dir> <out dir>` | PGO and ThinLTO of the GS libraries from a fresh profile (IN-468) |

## Method

- One variable per candidate; keep an A/B switch where practical.
- A/B: interleave at least 2+2 runs (`a1 b1 a2 b2`) and compare averages; single runs drift 1-3%.
- Identity, before keeping any step:
  - `EVERY=600 FROM=6600 FRAMES=12100 SCALE=3 night-run.sh` (10 frames);
  - `EVERY=100 FROM=8000 FRAMES=11000 SCALE=12 night-run.sh` (31 frames, 124 MB each);
  - `EVERY=700 FROM=700 SCALE=3 arcade-run.sh` (14 frames).
- Since candidate 335 two runs of one build are identical at 3x and 12x, so any difference between candidates is real.
- Compare steps on a plain build (no PGO), then take the PGO build as the last step of a candidate.

## Stutter measurement (TASK-215e)

The owner feels stutter, not averages, so `cpu_summary.py` reports both and tags the scene:

```bash
GT4_BENCH_SCENE=night GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 \
  night-run.sh a1 build/gt4vulkan.exe GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000
python scripts/coregs2_bench/cpu_summary.py a1 --csv build/evidence/a1.csv
```

The first table is the old averages table, unchanged. The sections after it are `stutter`, `statistics` (p50/p95/p99,
1% low, max, counts over 16.7/20 ms), `worst` intervals, a `histogram`, `per frame` figures when the run folder has a
`frametime.csv`, and `spikes` (the `[core2] spike` lines). `--table-only` prints the old output alone; `--stats all`
adds every metric of the old table to the statistics (its budget counts only make sense for the millisecond metrics).

### Where each number comes from

| Source | What it is | What it can see |
|---|---|---|
| `[core2]` report group, every 600 VBlanks | means of one interval: wall, `gpu_busy`, `gpu_wait` and its reasons, stages, counters | 10 s means: an interval over 16.7 ms cannot hold 60 FPS, but a spike inside it disappears |
| `[core2] spike vblank=...` (`GT4_CORE2_SPIKE_MS`, default 20, 0 = off; `GT4_CORE2_SPIKE_LINES`, default 40 per 600) | one line per VBlank whose GS thread or wall time passes the threshold, with its stages, readbacks, submits and prims | single frames, but only those over the threshold and only up to the line cap |
| `GT4_HW_FRAMETIME_LOG=<csv>` (a window run; `replay-run.sh WINDOW=1` writes `frametime.csv`) | one row per presentation: `id,vblank,present_ms,call_ms,shown_ms` | the presented and displayed frame intervals and the frames the display never got |
| `[hw] cost` and `[hw] transport`, every `GT4_HW_PROFILE_EVERY` | GS thread CPU, process CPU, transport | no frame resolution; the CSV matches them to the interval by their vsync |

A **complete per-VBlank trace** is `GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000` (`lap-run.sh` passes `0.01`
with a cap of 9000, enough for its window). When the spike lines cover at least 98% of the window, the summary computes
per-VBlank p99 and 1% low from
them; below that the spikes section says `no (censored)` and only the count and the maximum are meaningful. Never read a
percentile from a censored trace.

### Definitions

- **Interval**: one report group, normally 600 VBlanks (10 s at 60 Hz). `vblank_start`/`vblank_end` in the CSV are its
  guest VBlank range. Equal intervals mean a share of intervals over budget equals the share of frames whose interval
  mean is over budget.
- **p50/p95/p99**: nearest rank, the sample at `ceil(percent / 100 * n)`; the same rule as the p95/p99 that
  `native/perf_probe.cpp` prints.
- **1% low**: `1000 / p99` in FPS, as the TASK-215e brief defines it (the mean of the worst 1% of frames needs the
  per-frame sources).
- **max**: the largest sample.
- **frames over 16.7 ms / 20 ms**: samples strictly above 60 Hz (16.7 ms) and 50 Hz (20 ms), as a count and a share of
  the samples. The interval sections count intervals; the `per frame` section counts frame intervals.
- **stutter ratio**: the share of samples over 16.7 ms. For intervals it is the share of 10 s windows over budget; for
  `present_ms`/`shown_ms` it is the share of frames over budget.
- **window**: guest VBlanks `SUMMARY_LO..SUMMARY_HI` (default the night race, 7800-11400) or `--lo/--hi`. The scene tag
  says what the window is; the two are recorded together.

### CSV

`--csv <path>` (or `GT4_BENCH_CSV`) writes one row per `[core2]` report interval, all runs in the order given, with the
scene tag in every row. Columns, in order:

`run, scene, vblank_start, vblank_end, vblanks, wall_ms, gs_worker_ms, gs_exclusive_ms, process_cpu_ms,
ee_critical_path_ms, gpu_busy_ms, gpu_draw_passes_ms, gpu_wait_ms, gpu_wait_not_readback_ms`, then
`gpu_wait_<reason>_ms` and `readbacks_<reason>_per_vblank` for `download`, `before_write`, `keep`, `clut`, `other` (and
any reason a newer renderer adds, for example `flare` or `tone_clut`), then `submits, submits_per_vblank,
uploads_per_vblank, clut_loads_per_vblank, swizzle_kib_per_vblank, prims_per_vblank, fragments_per_vblank, batches,
batches_per_vblank, draws, draws_per_vblank, readbacks, readbacks_per_vblank`.

- `submits`, `batches`, `draws` and `readbacks` are the deltas of the cumulative `[core2] vblank=` counters of that
  interval; the `_per_vblank` figures are the means the renderer prints (or the delta divided by the interval length).
- `ee_critical_path_ms` is the mean of the `[frame] critical path=` reports whose cumulative `N VBlank samples` end
  inside the interval (the `[frame]` line itself has no VBlank; see the gaps below).
- Empty cells are fields the log did not print (a run without `GT4_HW_PROFILE_EVERY=600`, a renderer without `[core2]`
  lines, an interval without an EE report). A run without `[core2]` blocks (GSdx, CoreGS 1.1) gets rows from its
  `[hw] cost` lines, with the core2 cells empty.
- When the log has `[core2] spike` lines, `<name>-spikes.csv` is written beside the CSV: one row per spike VBlank with
  all of its fields (`wall_ms`, `gs_ms`, stages, `readbacks` by kind, `gpu_busy_ms`, `prims`, ...).

### Scene tags

`GT4_BENCH_SCENE=<name>` or `--scene <name>` names the scene; the default is `night`. `night-run.sh`, `arcade-run.sh`,
`spec2-run.sh` and the replay scripts also write `scene.txt` into the run folder, and `cpu_summary.py` reads it, so an
old run is never guessed from its VBlank range. Names used in the matrix: `night` (night race, Hong Kong Reverse),
`ssr5-night` (Arcade Clubman Stage Route 5), `license-b1`, `opera-paris`, `tsukuba-wet`, `george-paris-rally`,
`spec2-night`, `spec2-day`, `spec2-george`. The acceptance bar, the scene/resolution matrix and the weak-GPU proxies are
in [`docs/tasks/COREGS2-MEASUREMENT.md`](../../docs/tasks/COREGS2-MEASUREMENT.md).

### What the renderer does not print yet

These limit what the tool can report without inventing data; they are the log lines to add for the TASK-215 matrix.

- **A per-VBlank trace without a threshold or a cap.** The spike line is censored by `GT4_CORE2_SPIKE_MS` and by
  `GT4_CORE2_SPIKE_LINES` (40 per 600 VBlanks by default), so a run with the defaults cannot give per-VBlank
  percentiles. Until a plain per-VBlank line exists, a complete trace needs `0.01` and a high line cap.
- **A VBlank tag on the EE and presentation lines.** `[frame] ... critical path=` (EE thread) and
  `[hw] presentation ... shown/not_shown/left_out` carry no VBlank; the CSV attributes them through the `N VBlank
  samples` of `[perf]`, which is the only printed bridge.
- **The `flare` and tone-CLUT wait reasons on rc15.** The renderer of the rc15 line prints `download`, `before write`,
  `keep`, `clut` and `other` only, plus the derived `not_readback`; the flare read is inside `download` and the tone CLUT
  has no reason of its own. The tool takes any reason the log prints, so the newer lines appear as extra CSV columns
  automatically, but a comparison of the reasons across builds needs the same reason set on both.
- **GPU wait by reason per VBlank.** The reasons are printed per 600-VBlank interval; a spike line has the total
  `gpu_wait` only.
- **A dropped-present count per VBlank.** `[hw] presentation` has `not_shown`/`left_out` per interval, not per VBlank.
- **Frame times without a window.** `GT4_HW_FRAMETIME_LOG` needs a presenting run; a headless unpaced run has no
  display intervals, and its `wall`/spike figures are compute time (a run at 7 ms wall still holds 60 FPS, one at 20 ms
  cannot), not displayed stutter.

## CPU profile of every thread

```bash
GT4_CPU_SAMPLER='C:\path\sampler.bin' GT4_CPU_SAMPLER_FROM=9000 GT4_CPU_SAMPLER_TO=11000   # passed as ENV=VAL to night-run.sh
python scripts/cpu_sampler_report.py sampler.bin --exe <exe> --top 45 --threads 4 --out report.txt
```

`--focus REGEX` lists caller -> function -> callee; `--blame REGEX` puts library time (driver, allocator) on its nearest
caller. The build needs line tables (`/Z7 -gline-tables-only`, `/DEBUG`). Reading it:

- a sample takes the cycles since the previous visit, so time in `ZwWaitForAlertByThreadId` is mostly work done before the thread slept;
- `VCRUNTIME140.dll!__NLG_Return2` is memcpy (nearest export), not exceptions.

## Recording a drive and replaying it (IN-520)

`record-input.sh <name> [gt4|spec2] [card]` (or `record.cmd ...` from PowerShell) opens the game (Spec II by default) from a copy of a memory card and records every pad change by guest VBlank, into `private/coregs2-bench/inputs/<name>.txt` in the main checkout (never overwritten), with `<name>.setup` beside it: the game, the card and its sha256, which the replays read (and refuse a changed card). `card` names a file in `private/coregs2-bench/cards/` (without `.ps2`); without it, Spec II starts from the Arcade card without game data. Build a card from a save library (`%APPDATA%\GT4Recomp\games\<game>` or a copy) the way a Play does, with replays from other libraries added: `cargo run -p gt4saves --example bench_card -- <gt4|spec2> <out.ps2> <save library> [replay library ...]`. It runs the slice 401 package (rc12 test) unless `EXE` is set. F9 ends the recording and closes the game. Keep the game focused: without focus the keyboard reads as released, and that is recorded too.

`replay-run.sh <name> <run> <exe> [ENV=VAL ...]` replays it from a fresh copy of the same card up to its last VBlank, into `$GT4_BENCH_OUT/replay-<name>-<run>/`. `WINDOW=1` opens a window and writes `frametime.csv`; `PACED_FROM=<VBlank>` skips the waiting before that VBlank (use the VBlank where the measured stretch starts). Frame dumps slow a window run: leave `EVERY` unset when measuring.

Pass `GT4_HW_FLARE_DEADLINE_MS=0` when comparing frames from window runs: a late flare answer depends on real time.

`KEEP="A-B C-D"` with `EVERY=1` keeps only those windows of frames on disk while a replay dumps every frame (`keep_frames.py`), on either renderer. `frame_compare.py <our frames> <GSdx frames> <first> <last> <out> [--sheet A-B]` pairs our `frame_N` with GSdx's `live_frame(N-1)`, writes `compare.csv` and lists the frames that jump on one renderer only (TASK-139 found its bugs this way: the trace of VBlank N shows on the frame dumped at N + 2, the double buffer).

`replay-gsdx.sh <name> <run> <exe> [ENV=VAL ...]` replays it headless on the reference renderer (`EVERY`, `SCALE`, `GS_EXE`), with the final frames in `gs-frames/`. Clean room: its `log.txt` also holds the GS process's output, so read only the game's lines. On the Ford GT lap both renderers stay in sync to the end, and the reference's `live_frameN` is one frame ahead of our `frame_N`. A Time Trial is the safe recording: with AI cars the rival list has differed between the two (IN-506).
`GT4_BENCH_INPUTS=<folder>` replays recordings from another folder than the main checkout's `private/coregs2-bench/inputs` (synthetic recordings kept in a worktree's `build/`, each with its `.setup`).

`camera360-check.sh <recording> <reference exe> <candidate exe> <dump VBlank>` (TASK-159, `docs/CAMERA-360.md`) replays a drive on a build without the 360-degree chase camera and on one with it, switch off (`0`) and on (unset, the default): the race clock of every VBlank, the simulation state and race object at the dump VBlank, and frames (`FROM`, `EVERY`, `SCALE`, `DYNAMICS=0x24000` for Spec II). Off must equal the reference in everything; on must keep the race clock and lists the state words and frames the camera changes.

`overlay-aspect-check.sh <recording> <reference exe> <candidate exe> <dump VBlank>` (TASK-095, `docs/ULTRAWIDE-OVERLAYS.md`) replays a drive on a build without the 2D overlay canvas hooks and on one with them, at 16:9 (the replay default) and at each ratio of `RATIOS` (default `21:9 32:9`): the race clock of every VBlank, the state at the dump VBlank, and frames (`FROM`, `EVERY`, `SCALE`, `DYNAMICS`). At 16:9 everything must be identical; at the other ratios the race must be identical and `overlay_geometry.py` checks that each frame that differs is the reference's overlay narrowed by R / (16/9) around the centre.

## Frame skipping (TASK-082)

`frame-skip-suite.sh <Spec II exe>` and `frame-skip-suite-v200.sh <v2.00 exe>`: headless. The simulation over the whole drive against nothing left out (`frame_skip_check.py`), then every frame of a stretch (`frame_sheet.py`): `sheet-<pattern>/sheet.png`, `frames.csv`, `summary.txt`. Exit code 1 on a wrong result.

`lap-run.sh <run> GT4_FRAME_SKIP=1 GT4_FRAME_SKIP_LOG=<csv>`, then `frame_skip_speed.py <csv> <frametime.csv>`: game speed and frames drawn in real time. `GT4_FRAME_SKIP_FORCE=1` logs with nothing left out; `2` and `3` keep 1 frame in 2 or 3.
