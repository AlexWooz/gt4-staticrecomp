# CoreGS 2.0 measurement: stutter, scenes and the acceptance bar (TASK-215e)

Status: tooling and method. This document defines what the TASK-215 performance work measures and how a result is
accepted; it contains no new performance numbers of its own. The tool is `scripts/coregs2_bench/cpu_summary.py` and the
run scripts beside it; the usage details are in `scripts/coregs2_bench/README.md`.

Evidence labels: **Confirmed** (printed by the code or reproduced by a test), **High confidence** (a report from a
measurement run), **Hypothesis** (a proposal to be measured), **Unknown**.

## Why averages are not enough

The owner's complaint is stutter, not the average frame rate. The streamer's rc14 log (GTX 1660 class) that commissioned
this task shows the shape of it: a steady race GPU busy of 6.82 ms, but 1,258 race spikes with a mean GPU wait of
17.3 ms, a p99 of 75 ms, and a 0.856 correlation between frame wall time and GPU wait (**High confidence**: reported
from that log; the log itself is not in this repository). A mean of the interval hides both the spikes and their cause.
Everything below exists to make a spike visible and attributable before and after an optimisation.

## Sources: what each line can and cannot see

| Source | Cadence and switch | Resolution | Use |
|---|---|---|---|
| `[core2]` report group (`wall ms/vblank`, `ms/vblank`, `exclusive ms/vblank`, `gpu_wait ... by readback reason`, `vblank=` counters) | every 600 VBlanks, always | 600-VBlank means | CPU stages, GPU busy, fence waits by reason, submissions, uploads, CLUT loads, swizzle, prims, batches. A spike is spread over the 10 s mean and disappears from it |
| `[core2] spike vblank=...` | one line per VBlank over `GT4_CORE2_SPIKE_MS` (default 20 ms, `0` = off), at most `GT4_CORE2_SPIKE_LINES` per 600 VBlanks (default 40) | single VBlank, thresholded and capped | the frame-level breakdown of a spike: stages, readbacks by kind, images/allocations, submits, prims |
| `[hw] cost` / `[hw] transport` | every `GT4_HW_PROFILE_EVERY` VBlanks | interval means | GS thread CPU, process CPU per core, transport traffic; the per-interval CSV matches them by vsync |
| `[hw] presentation` | every profile report | interval totals per presentation | requests, shown, not_shown, failed, left_out; the GS and presenter thread time per presentation |
| `[perf]` / `[frame]` | periodic (5 s) and F8 | per report, no VBlank tag | EE frame wall, `critical path`, the per-report median/p95/p99/max of frame times, thread CPU |
| `GT4_HW_FRAMETIME_LOG=<csv>` | every presentation of a window run | one row per presentation | `present_ms` (queue present, what RivaTuner shows) and `shown_ms` (display), the real stutter and dropped frames |

Two consequences:

- A **complete per-VBlank trace** is a run with `GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000` (`lap-run.sh`
  passes `0.01` with a cap of 9000, enough for its window). Only then are the per-VBlank percentiles of the summary
  meaningful. With the defaults the trace is censored and `cpu_summary.py` says `no (censored)`; its count and maximum
  are still valid, its percentiles are not computed.
- A **displayed** stutter figure needs a window run with `GT4_HW_FRAMETIME_LOG`. A headless run is unpaced: its wall and
  spike times are compute cost, which tells whether a build *can* hold 60 FPS, not whether the display dropped a frame.

## Metric definitions

All metric names below are the ones `cpu_summary.py` prints and writes.

- **Window.** Guest VBlanks `SUMMARY_LO..SUMMARY_HI` (`--lo/--hi`). Default 7800-11400 (night race). The scene tag names
  the scene the window belongs to; the window and the tag are recorded together.
- **Interval.** One `[core2]` report group, normally 600 VBlanks (10 s at 60 Hz). Its figures are means over its
  VBlanks. Equal intervals make the share of intervals over budget equal to the share of frames whose interval mean is
  over budget.
- **p50 / p95 / p99.** Nearest rank: the sample at `ceil(percent / 100 * n)`, the rule `native/perf_probe.cpp` uses for
  the p95/p99 it prints. p50 is the median (the mean of the two middle samples for an even count).
- **1% low.** `1000 / p99`, in FPS, as TASK-215e defines it. It is not the mean of the worst 1% of frames; that needs
  per-frame data, and the `per frame` section of the summary is where per-frame sources are read.
- **max.** The largest sample.
- **frames > 16.7 ms** and **frames > 20 ms.** Samples strictly above the 60 Hz budget (16.7 ms) and the 50 Hz line
  (20 ms), as a count and as a share. On interval samples they count intervals; on `present_ms`/`shown_ms` they count
  frame intervals. Strictly greater, so a sample of exactly 16.7 ms is not over.
- **stutter ratio.** The share of samples over 16.7 ms. On intervals: share of 10 s windows over budget. On
  `present_ms`/`shown_ms`: share of frames over budget.
- **worst interval.** The interval with the largest wall mean; the summary lists its VBlank, wall, GS worker, process
  CPU, GPU busy, GPU wait, submissions, prims and batches so the cause is visible without opening the log.

## Acceptance bar (proposal, to be confirmed by the owner)

A scene passes when **all** of these hold inside its window, for the same build, scene tag and window as the reference
run:

1. **60 FPS**: `wall` p99 <= 16.7 ms (intervals), and `present_ms` p99 <= 16.7 ms when the run has a frametime log.
2. **1% low >= 60 FPS**: `1000 / p99 >= 60` from the same samples.
3. **No interval over budget**: count over 16.7 ms = 0 and stutter ratio = 0.000 in the race window.
4. **No hard stutter**: count over 20 ms = 0; the maximum is informative, not a bar of its own, once counts 1-3 hold.
5. **Headroom proposal**: at 3x on the desktop, `wall` p95 <= 15.0 ms (10% under the budget) so run-to-run noise does
   not flip the result. To be set from the first owner baseline, not assumed.
6. **No effect cut and no identity change**: the frame/state gates of the optimisation step stay green; a performance
   step never buys time by dropping a game-visible effect or a readback.

The bar is the same on every proxy. A proxy that misses it is the next optimisation target, not a reason to lower the
bar; 10x/12x runs are diagnostic (GPU stress) and are reported (p99, spikes) without a 60 FPS bar.

## Scene and resolution matrix

Scenes are named by the scene tag and run by the existing scripts. The window is part of the scene; write it in the run
notes and pass it with `--lo/--hi` (or `SUMMARY_LO`/`SUMMARY_HI`).

| Scene (tag) | Window (VBlanks) | 1x / 2x (mobile baseline) | 3x / 6x (PC) | 10x / 12x (GPU stress) |
|---|---|---|---|---|
| Night race, Hong Kong Reverse (`night`) | 7800-11400 | planned (Phase 4) | **in use** (3x reference, 6x heavy) | diagnostic |
| Arcade Clubman Stage Route 5 (`ssr5-night`) | race start 8700-9200 | planned | planned | diagnostic |
| License B-1 (`license-b1`) | from the TASK-215 run notes | in use (TASK-215 1x) | - | - |
| Opera Paris (`opera-paris`) | from that scene's run notes | planned | planned | diagnostic |
| Tsukuba Wet (`tsukuba-wet`) | from that scene's run notes | planned | planned | diagnostic |
| George V Paris Rally, Spec II (`george-paris-rally`) | 20400 to about 21300 | planned | in use (6x window run, `lap-run.sh`) | planned |
| Spec II night (`spec2-night`) | race starts near 20000 | planned | planned | diagnostic |
| Spec II day, Costa di Amalfi (`spec2-day`) | race starts near 21000 | planned | planned | diagnostic |
| Spec II George V (`spec2-george`) | race starts near 20300 | planned | planned | planned |

Rules for the matrix:

- One scene per run and one tag per scene; never compare two runs whose tags differ.
- 1x/2x are the mobile target (Phase 4) and the reference for menus and the license; a 1x result is not a PC result.
- 3x is the PC reference; 6x is the heavy PC case; 10x/12x exist to expose GPU-bound behaviour and are not shipping
  targets.
- Every cell needs the same window, the same scene tag and the same report cadence (`GT4_MTVU_REPORT_EVERY=600`,
  `GT4_HW_PROFILE_EVERY=600`).

## Weak-GPU and slow-CPU proxies

The owner runs controlled baselines; the point is to find the low-end behaviour without a low-end machine of each kind.

| Proxy | What it represents | How to run it | What to record |
|---|---|---|---|
| GTX 1660 class | the streamer's card and the low-end discrete PC | the streamer's own log (no local run needed) | GPU busy p99, spike count, `gpu_wait` by reason |
| RTX 3060 laptop | a common mid-range laptop | on the laptop, same scene/window/scale | clamps, GPU busy p99, p99 wall |
| Underclocked RX 9060 XT | the desktop pushed into the GPU-bound regime | owner's underclock (core and memory), same scene/window | the actual core/memory clocks, not just the offset |
| Slow CPU at 3.4 GHz | a desktop CPU below the reference | owner's clock/PBO limit, 3x or 6x | effective clocks, SMT on/off, process CPU per core |

Measurement hygiene: one variable per candidate, 2+2 interleaved runs, nothing else running, shader cache warm, the same
driver, and the exact build hash plus the scene tag and window in the notes. A baseline taken while another build runs
is not a baseline.

## Commands

```bash
# Night race, full per-VBlank trace, one CSV for the summary (the perf agent's usual case)
GT4_BENCH_SCENE=night GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 \
  night-run.sh a1 build/gt4vulkan.exe GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000
python scripts/coregs2_bench/cpu_summary.py a1 --csv build/evidence/a1.csv

# Two candidates, interleaved 2+2, one CSV with the scene and run in every row
GT4_BENCH_SCENE=night series.sh a1:build/a/gt4vulkan.exe b1:build/b/gt4vulkan.exe \
  a2:build/a/gt4vulkan.exe b2:build/b/gt4vulkan.exe
python scripts/coregs2_bench/cpu_summary.py a1 a2 b1 b2 --csv build/evidence/night-ab.csv

# A window run: presented/shown frame intervals from frametime.csv
GT4_BENCH_SCENE=george-paris-rally lap-run.sh g1
python scripts/coregs2_bench/cpu_summary.py g1 --lo 20400 --hi 21300 --csv build/evidence/g1.csv

# Another renderer (GSdx or CoreGS 1.1): the same summary, switch reasons are empty
GT4_BENCH_SCENE=night replay-gsdx.sh george-volta2 gsdx-a build/gt4vulkan.exe
python scripts/coregs2_bench/cpu_summary.py gsdx-a
```

The old averages table still prints first and `--table-only` prints only it, so an existing comparison script keeps
working.

## What the renderer still has to print

These are the gaps `cpu_summary.py` must not fill by guessing. Each one is a candidate log line for the TASK-215 work.

1. **A per-VBlank trace without a threshold or a cap.** Today the trace is the `[core2] spike` line, censored by
   `GT4_CORE2_SPIKE_MS` (default 20 ms) and `GT4_CORE2_SPIKE_LINES` (default 40 per 600 VBlanks). Until a plain line
   exists, full traces need `0.01` and a high cap; a run that forgets them cannot give per-VBlank percentiles.
2. **A VBlank tag on the EE and presentation lines.** `[frame] ... critical path=` and `[hw] presentation ...` have no
   VBlank. The CSV attributes them through the cumulative `N VBlank samples` of `[perf]`, which is only approximate at
   a window boundary; a tag would make it exact.
3. **One wait-reason set on both lines.** The rc15 renderer prints `download`, `before write`, `keep`, `clut`, `other`
   and the derived `not_readback`. The flare read has no reason of its own on this line (it falls inside `download`),
   and the tone CLUT has none. The tool accepts any reason the log prints (extra CSV columns), but a cross-build
   comparison needs the same set everywhere.
4. **GPU wait by reason per VBlank.** The reasons are per 600 VBlanks; the spike line has the total `gpu_wait` only.
5. **Dropped presentations per VBlank.** `[hw] presentation` counts `not_shown`/`left_out` per interval, not per VBlank.
6. **A frame-time source for headless runs.** `GT4_HW_FRAMETIME_LOG` needs a presenting run. Until headless runs can
   print per-VBlank wall and GS time uncensored (gap 1), a headless result is a capacity result, not a display result.

## Limits

- Interval figures cannot localise a spike inside the 600 VBlanks; only the spike trace or the frametime log can.
- The 1% low is defined as `1000/p99`; the true worst-1% mean is a different number and is not printed by the renderer.
- The spike trace's `wall` is the time between two VBlank boundaries on the GS/renderer side, not a display interval.
- `[core2]` and `[hw]` report cadences are independent; the CSV only has cost/transport cells when their vsync matches
  an interval (set both report every values to 600, as `series.sh` does).
- Every figure here is only comparable inside the same scene tag, window, scale and build hash.
