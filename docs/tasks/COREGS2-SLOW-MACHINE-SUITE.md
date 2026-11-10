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


## GPU-down results (2026-11-10, owner: core -500 MHz, Power Limit 70%, VRAM stock, driver 32.0.32015.7029)

One run per point, exe `A2A9CD8A...` (unchanged), headless, full per-VBlank trace. Effective GPU core/VRAM clocks and
temperatures are **Unknown**: the tree prints none and no monitor was read at run time.

| Point | wall p50 / p99 (ms) | 1% low | >16.7 / >20 | GPU busy (mean / p99) | GPU wait | flare wait | before-write / keep | EE critical | GS worker | CSV |
|---|---|---|---|---|---|---|---|---|---|---|
| stock night 3x | 6.97 / 9.76 | 102.5 | 0 / 0 | 2.79 / 4.36 | 0.22 | 0.15 | 0.06 / 0.004 | 6.98 | 7.22 | `stock-night-3x.csv` |
| **GPU-down night 3x** | **6.91 / 9.78** | **102.2** | **0 / 0** | **2.80 / 4.36** | **0.23** | **0.16** | **0.06 / 0.006** | **7.09** | **7.10** | `gpudown-night-3x.csv` |
| stock night 6x | 8.54 / 10.57 | 94.6 | 0 / 0 | 6.15 / 8.57 | 1.28 | 0.95 | 0.22 / 0.02 | 8.85 | 7.15 | `stock-night-6x.csv` |
| **GPU-down night 6x** | **8.40 / 11.21** | **89.2** | **0 / 0** | **6.62 / 9.40** | **1.67** | **1.25** | **0.25 / 0.03** | **9.20** | **7.09** | `gpudown-night-6x.csv` |
| stock license 1x | 8.83 / 9.26 | 108.0 | 0 / 0 | 0.94 / 1.28 | 0.03 | 0.00 | 0.00 / 0.00 | 7.91 | 8.23 | `stock-license-1x.csv` |
| **GPU-down license 1x** | **8.81 / 9.13** | **109.5** | **0 / 0** | **0.94 / 1.28** | **0.03** | **0.00** | **0.00 / 0.00** | **8.07** | **8.26** | `gpudown-license-1x.csv` |

Per-VBlank p99 (complete trace): 3x stock 11.10 -> GPU-down 10.80 ms; 6x 10.57 -> 10.57 (interval p99 11.21); license
9.26 -> 9.13. Spike CSVs beside each (`gpudown-*-spikes.csv`).

**What the configuration exposed:**
- **3x: not GPU-bound, and the underclock did not bite.** GPU busy 2.80 ms of 16.7 and the wait reasons are identical to
  stock within 0.01 ms. The frame is the GS worker/EE (~7.1 ms). Hypothesis: at this load the card already ran below the
  -500 MHz offset (the offset applies to the boost curve) and the 70% power limit did not bind, so the *effective* clock was
  the same; the effective clock itself is Unknown.
- **6x: still not GPU-bound, but the first clock-sensitive point.** GPU busy +7.6% (6.15 -> 6.62, max 9.40), flare wait
  0.95 -> 1.25 ms, before-write +0.03, EE critical +0.35, wall +0.18 ms (p99 +0.64). ~10 ms of GPU headroom remains.
- **License 1x: unchanged** (wall 8.81 against 8.83, GPU 0.94): CPU/GS-worker-bound, as expected.
- **Limit:** **VRAM clock stayed stock** (the underclock was not possible on this card/driver), so a
  memory-bandwidth-bound cost cannot show in this configuration; the interpretation is bounded to GPU core clock and power
  limit, and those may not have bound at 3x at all. A CPU-down point (3.4 GHz, BIOS) is the next configuration.


## GPU-down 10x stress point (2026-11-10, same configuration: core -500 MHz, PL 70%, VRAM stock)

Added because the 3x/6x points were CPU-bound even with the underclock. One run, exe `A2A9CD8A...`, full per-VBlank trace.

| Point | wall p50 / p99 (intervals) | 1 % low | >16.7 / >20 | GPU busy mean (p50 / p99) | GPU wait | flare wait | before-write / keep | EE critical | GS worker |
|---|---|---|---|---|---|---|---|---|---|
| stock 10x (mean table, no trace) | 15.43 mean | - | - | 14.32 mean | 8.32 | 6.05 | 0.69 / 0.10 | 16.74 | 6.83 |
| **GPU-down 10x** | **15.71 / 23.19** | **43.1** | **3 / 2 of 7** | **15.78 (14.55 / 21.47)** | **10.07** | **7.31** | **0.78 / 0.12** | **18.78** | **6.57** |

Per-VBlank trace (3,601 rows, complete): wall mean 17.60, p99 40.00, max 117.6 ms (VBlank 9763), GS max 51.9. CSV
`build/evidence/gpudown-night-10x.csv` (interval rows) and `gpudown-night-10x-spikes.csv` (per VBlank).

**The underclock binds at 10x.** GPU busy 14.32 -> 15.78 ms (+10.2 %, p99 21.47), the frame mean 15.43 -> 16.90 ms, 3 of 7
600-VBlank intervals over 16.7 ms and 2 over 20 (stock had none in its mean table), and the flare wait grows 6.05 -> 7.31 ms
because the GPU takes longer to reach the read. 10x here is the GPU-bound proxy the 3x/6x points are not: the GPU is at
the 16.7 ms edge (busy 15.78) and the frame is over it. **12x was not run** (the rule: 10x already over budget).
Comparison gap to record: the stock 10x run predates the trace tooling, so its per-VBlank p99 and interval p99 are Unknown;
only its means are comparable.

Limit unchanged: **VRAM stayed stock**, so bandwidth-bound costs do not show; this is a core-clock + power-limit result.


## CPU-down results (2026-11-10, no-reboot method; GPU still core -500 MHz / PL 70 % / VRAM stock)

Configuration: `PERFBOOSTMODE=0` (boost disabled, AC) and `PROCTHROTTLEMAX=100` (AC) on the "Desempenho Máximo" scheme.
Clock proxy (fixed single-thread integer benchmark, same binary): the owner's reference is **480 / 478 / 480 / 485 ms**,
taken on a **cool** machine after the plan was restored (the earlier 568-572 ms baseline was taken warm, right after the
performance runs, and is not the reference). Boost-off measured **683-691 ms**, i.e. **+42 % time, about -30 % effective
clock**, estimated **~3.4 GHz** from a 4.8-5.0 GHz boost - the BIOS target, not the earlier ~3.8 GHz guess. The exact clock
stays **Unknown** (the benchmark is the proxy; no tree line, HWiNFO is GUI). **Warm/cool caveat:** the single-thread
benchmark moves with temperature, so only same-conditions pairs are comparable, and the game runs below were taken with the
machine already loaded. GPU clocks unchanged at the time of the runs; at 3x/6x/license that GPU configuration had no
measurable effect (see above), but the 6x GPU busy is the GPU-down figure and its comparison is stated against GPU-down,
not stock.

One run per point, exe `A2A9CD8A...`, full per-VBlank trace. Restored after the runs: `PERFBOOSTMODE=2` (Aggressive),
`PROCTHROTTLEMAX=100`, `SCHEME_CURRENT` re-activated; both AC values verified by `powercfg /query` (0x00000002 and
0x00000064), and the owner's benchmark reads **480 ms** again on the cool machine after the restore.

| Point | wall mean / p50 / p99 (intervals) | 1 % low | >16.7 / >20 | per-VBlank mean / p99 / max | GPU busy | flare wait | before-write / keep | GS worker | EE critical | process |
|---|---|---|---|---|---|---|---|---|---|---|
| stock night 3x | 7.47 / 6.97 / 9.76 | 102.5 | 0 / 0 | - | 2.79 | 0.15 | 0.06 / 0.004 | 7.22 | 6.98 | 15.91 |
| **CPU-down night 3x** | **9.91 / 9.37 / 13.08** | **76.5** | **0 / 0** | **9.39 / 14.10 / 17.9** | **2.80** | **0.11** | **0.05 / 0.004** | **9.44** | **9.47** | **22.89** |
| GPU-down night 6x | 8.81 / 8.40 / 11.21 | 89.2 | 0 / 0 | - | 6.62 | 1.25 | 0.25 / 0.03 | 7.09 | 9.20 | 16.46 |
| **CPU-down night 6x** | **10.52 / 9.51 / 13.06** | **76.6** | **0 / 0** | **10.10 / 17.20 / 47.8** | **6.66** | **0.80** | **0.22 / 0.02** | **9.12** | **10.23** | **22.82** |
| stock license 1x | 8.83 / 8.83 / 9.26 | 108.0 | 0 / 0 | - | 0.94 | 0.00 | 0.00 / 0.00 | 8.23 | 7.91 | 21.33 |
| **CPU-down license 1x** | **11.95 / 11.95 / 12.25** | **81.6** | **0 / 0** | **10.58 / 16.10 / 24.5** | **0.94** | **0.00** | **0.00 / 0.01** | **11.03** | **11.56** | **29.18** |

CSVs: `build/evidence/cpudown-night-3x.csv`, `cpudown-night-6x.csv`, `cpudown-license-1x.csv`, each with a `-spikes.csv`
(3,601 / 3,601 / 1,101 per-VBlank rows, trace complete).

**What the CPU-down exposed:** the CPU-bound scenes scale with the CPU, consistent with a **4.8-5.0 -> ~3.4 GHz** drop
(about -30 % clock, +42 % single-thread time): 3x wall mean +32.7 % (7.47 -> 9.91), GS worker +30.8 %, process +43.9 %;
license wall +35.3 % (8.83 -> 11.95), GS worker +34.0 %; 6x against the *same GPU configuration* (GPU-down 8.81) wall
+19.4 % with the GPU unchanged (6.66 against 6.62), so the 6x change is CPU too. GPU busy and every wait reason stay flat
at 3x/license, which is the signature of a CPU-only clock change. **60 FPS at 3x/6x/license still holds in the interval
means (0 over 16.7)** but the single-VBlank tail reaches the budget: per-VBlank p99 14.10 (3x), **17.20 (6x)** and
16.10 ms (license), with 47.8 ms (VBlank 9125) and 24.5 ms (VBlank 2412) worst spikes plus 17.9 ms (VBlank 8983) - the
first configuration in this suite where single frames cross 16.7 ms. Margins left: 3x 6.8 ms, 6x 6.2 ms, license 4.7 ms
(interval means). The two big spikes (VBlank 9125, 47.8 ms; VBlank 2412, 24.5 ms) should be read from the spike CSVs after
the flare/TASK-171 work.

**Limits:** the effective clock is an estimate (~3.4 GHz, Unknown exactly; benchmark proxy, warm/cool sensitive); the GPU
was still under clocked, so 6x is compared against GPU-down; VRAM stock throughout; one run per point.

## Clock record (fill per configuration; Unknown until the owner reads them)

| Configuration | CPU effective (MHz) | GPU core (MHz) | VRAM (MHz) | Temps | Date/session |
|---|---|---|---|---|---|
| stock | 3401 base; boost 4.8-5.0 GHz per the 480 ms benchmark (was 568 ms warm), live value Unknown | Unknown | Unknown | Unknown | 2026-11-10 |
| GPU-down | stock | core -500 MHz, PL 70 % (effective Unknown) | stock | Unknown | - |
| CPU-down | boost off, max state 100 %: **~3.4 GHz estimated** from 683-691 ms against 480 ms (~-30 % clock); exact Unknown | stock (underclock applied, no effect at these points) | stock | Unknown | - |
| both | ~3.4 GHz estimated | core -500 MHz, PL 70 % | stock | Unknown | - |

The tree prints no clock or temperature line; any number here must come from a monitor the owner runs, or stay Unknown.
The single-thread benchmark is temperature-sensitive: 568-572 ms warm after the perf runs, 480 ms cool after the restore.
