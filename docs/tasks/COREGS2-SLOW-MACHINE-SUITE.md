# Slow-machine baseline suite (TASK-215, prepared 2026-11-10; **not started**)

The owner will underclock the GPU (Afterburner, no reboot) and later fix the CPU at 3.4 GHz (BIOS, reboot). Run this
**after the go-ahead**, serially, with no build and no other heavy program during the runs. Everything is headless.

- Exe: `build/perf-base/gt4vulkan.exe`, sha256 `A2A9CD8AFDF2BCA46B4B31A6E906EE941B32658255DB139E5833BDCC6A3FBFDE`
  (rc15 base `fc1a61c5` + TASK-215 changes; tooling merge `e8a2aead` changes no executable). Plain Release, no PGO,
  shader cache `build/shader-cache-215` warm.
- Machine: AMD Ryzen 7 5700X (8C/16T, 3401 MHz base), AMD Radeon RX 9060 XT, driver 32.0.32015.7029 (17/10/2026),
  Windows. **Clocks and temperatures are not in any log this tree prints: record them by hand (Afterburner/GPU-Z/adrenalin)
  or state Unknown.** Fill the table at the end per configuration.
- The new tool prints the old averages table first, then the interval statistics (p50/p95/p99, 1% low = 1000/p99, counts
  over 16.7 and 20 ms, stutter ratio), the worst intervals, a histogram, and the per-VBlank spike trace; `--csv` writes the
  rows with the scene tag, `--csv` also writes a `-spikes.csv` beside it.
- Full per-VBlank traces need `GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000`; without them the tool says the
  trace is censored. `GT4_BENCH_SCENE` tags every row; the run scripts also write `scene.txt`.
- The stock reference was re-run with the new trace after the tooling merge (see the last section), because the earlier
  rc15 runs had no per-VBlank trace. No fresh stock run is needed again unless the clocks change.

## Run matrix and order

One script drives one configuration; each line is one run. Keep exactly this order (stock → GPU-down → CPU-down → both),
and do not interleave configurations across a clock change. Two runs per point when the go-ahead allows it (2+2 is for
A/B; for the clock suite one run per point is the baseline, the second only if the first is within 3% of another point).

| # | Configuration | Scene | Scale | Command tag |
|---|---|---|---|---|
| 1 | stock | night race | 3x | `stock-night-3x` (**done**) |
| 2 | stock | night race | 6x | `stock-night-6x` (**done**) |
| 3 | stock | license B-1 | 1x | `stock-license-1x` (**done**) |
| 4 | GPU-down | night race | 3x | `gpudown-night-3x` |
| 5 | GPU-down | night race | 6x | `gpudown-night-6x` |
| 6 | GPU-down | license B-1 | 1x | `gpudown-license-1x` |
| 7 | CPU-down (3.4 GHz) | night race | 3x | `cpudown-night-3x` |
| 8 | CPU-down (3.4 GHz) | night race | 6x | `cpudown-night-6x` |
| 9 | CPU-down (3.4 GHz) | license B-1 | 1x | `cpudown-license-1x` |
| 10 | both | night race | 3x | `both-night-3x` |
| 11 | both | license B-1 | 1x | `both-license-1x` |

The suite script (`build/scripts-215/stock-suite.sh`) does 1-3; the same three `replay-run.sh` lines with the new run
tags do the rest:

```bash
# GPU-down (CPU stock)
GT4_BENCH_INPUTS=/c/Antigravity/gt4-wt-215-coregs2-perf/build/bench-inputs-215 \
  SCALE=3 GT4_BENCH_SCENE=night bash scripts/coregs2_bench/replay-run.sh night215 gpudown-night-3x <exe> \
  GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600 GT4_CORE2_SPIKE_MS=0.01 GT4_CORE2_SPIKE_LINES=100000 \
  GT4_HW_SHADER_CACHE=$(cygpath -w build/shader-cache-215)
GT4_BENCH_INPUTS=/c/Antigravity/gt4-wt-215-coregs2-perf/build/bench-inputs-215 \
  SCALE=6 GT4_BENCH_SCENE=night bash scripts/coregs2_bench/replay-run.sh night215 gpudown-night-6x <exe> <same flags>
GT4_BENCH_INPUTS=/c/Antigravity/gt4-staticrecomp/private/coregs2-bench/inputs \
  SCALE=1 GT4_BENCH_SCENE=license-b1 bash scripts/coregs2_bench/replay-run.sh licenca-b1 gpudown-license-1x <exe> <same flags>
# CPU-down and both: the same three with the run tags cpudown-* and both-*, 3x/6x/1x as in the matrix
```

Summaries (one CSV per point, plus the spikes CSV the tool writes beside it):

```bash
python scripts/coregs2_bench/cpu_summary.py replay-night215-gpudown-night-3x --lo 7800 --hi 11400 \
  --csv build/evidence/gpudown-night-3x.csv
python scripts/coregs2_bench/cpu_summary.py replay-night215-gpudown-night-6x --lo 7800 --hi 11400 \
  --csv build/evidence/gpudown-night-6x.csv
python scripts/coregs2_bench/cpu_summary.py replay-licenca-b1-gpudown-license-1x --lo 2400 --hi 3500 \
  --csv build/evidence/gpudown-license-1x.csv
# ... one per run, with the same windows; the license window is 2400-3500
```

A presenting run (window) is not part of this suite: it needs the owner's display. If one is wanted for a configuration,
use `WINDOW=1` with the same recording and `GT4_HW_FRAMETIME_LOG=<csv>` (the tool reads `present_ms`/`shown_ms` from it).

## Stock reference with the new trace (already run, 2026-11-10, before any underclock)

| Run | wall p50 / p99 | 1% low | >16.7 / >20 | per-VBlank p99 (complete trace) | CSV |
|---|---|---|---|---|---|
| night 3x | 6.97 / 9.76 ms | 102.5 FPS | 0 / 0 | 11.10 ms | `build/evidence/stock-night-3x.csv` |
| night 6x | 8.54 / 10.57 ms | 94.6 FPS | 0 / 0 | 10.57 ms | `build/evidence/stock-night-6x.csv` |
| license 1x | 8.83 / 9.26 ms | 108.0 FPS | 0 / 0 | 9.26 ms | `build/evidence/stock-license-1x.csv` |

Spike CSVs: `build/evidence/stock-*-spikes.csv` (3,601 per-VBlank rows for the night runs, 1,200 for the license).
Averages table (same window): night 3x GS worker 6.90, process 15.91, GPU busy 2.26, GPU wait 0.11 ms/VBlank; night 6x
7.15 / 17.47 / 6.15 / 1.28; license 1x 8.23 / 21.33 / 0.94 / 0.03.

## Clock record (fill per configuration; Unknown until the owner reads them)

| Configuration | CPU effective (MHz) | GPU core (MHz) | VRAM (MHz) | Temps | Date/session |
|---|---|---|---|---|---|
| stock | 3401 base, 4.4-4.6 boost (PBO), Unknown live | Unknown | Unknown | Unknown | 2026-11-10 |
| GPU-down | stock | set by owner | set by owner | Unknown | - |
| CPU-down | 3400 fixed | stock | stock | Unknown | - |
| both | 3400 fixed | set | set | Unknown | - |

The tree prints no clock or temperature line; any number here must come from a monitor the owner runs, or stay Unknown.
