# Avell retest suite (TASK-215, prepared 2026-11-11): the kept changes and the GPU-bound negatives

**Purpose.** The desktop (Ryzen 7 5700X, RX 9060 XT) is CPU/GS-bound with the GPU idle at 3x, so it cannot judge the GPU side
or a laptop's thermal and present behaviour. This suite re-checks, on the owner's Avell (i7 + RTX 3060 Laptop, 3440x1440
144 Hz), the changes this branch kept and the GPU-side items the ledger reverted, with the **same recordings, windows and
tags** as the local runs so the numbers are comparable.

**The agent never runs on the Avell and never connects to it.** The owner runs the suite; this page is the kit: the matrix,
the exact commands, what to record and the pass criteria. No build or run of this suite was made anywhere but the desktop.

## 1. Build and inputs

| item | value |
|---|---|
| branch / build | `task/215-coregs2-perf`, the build under test (plain release, clang-cl, shader cache warm) |
| recordings | the same ones the desktop uses: `licenca-b1` (v2.00, 1x), `night215` (v2.00, 3x/6x), and the Spec II Arcade drives `opera`, `ssr5`, `tsukuba-wet`, `george-rally` (3x) |
| windows | license 2,400-3,500; night 7,800-11,400; Opera/SSR5/Tsukuba/George rally 20,000-21,600 (all VBlanks, as in `docs/tasks/COREGS2-TASK215-SUMMARY.md`) |
| report cadence | `GT4_MTVU_REPORT_EVERY=600 GT4_HW_PROFILE_EVERY=600`; per-VBlank CSV through `cpu_summary.py --csv` |
| hygiene | one variable per run, 2+2 interleaved for every A/B, nothing else running, the same driver, the build hash and the exact clocks in the notes, serial (no build during a run) |

## 2. The GPU cap levels (the 3060's thermal/power envelope)

Run the **kept** build at each level on `night 3x`, `night 6x`, `Opera 3x` and `license 1x`; `GT4_CORE2_PERF_*` effects stay
at their defaults (faithful). Record the level as the tool reports it (power limit and clock), never as the intended one.

| level | setting | what it answers |
|---|---|---|
| L0 stock | MSI Afterburner profile reset (core 0, PL 100 %) | the reference |
| L1 power-capped | PL 70 % (no core offset) | a warm laptop in a 45-65 W envelope |
| L2 clock-capped | core -300 MHz, PL 100 % | the same thermal state without the power clamp |
| L3 both | core -500 MHz, PL 70 %, VRAM stock | the low end; the GPU-bound regime at 3x/6x |

For each level and scene: wall p50/p95/p99, 1 % low, the budget over 16.7/20 ms, GPU busy p99, the flare readback wait, and
the per-VBlank CSV. The GPU-bound sign is **GPU busy p99 within 5 % of the frame** plus a wall that follows the cap (the
desktop's GPU-down proxy moved only 10x, not 3x; the 3060 is expected to bind earlier, which is the point).

## 3. The present matrix at 3440x1440 (144 Hz vs 60 Hz)

The default is a 60 Hz guest VBlank; presenting on a 144 Hz panel must not change pacing or the game's clock.

| case | env | record |
|---|---|---|
| P0 60 Hz panel | desktop refresh 60 Hz, `GT4_PRESENT_MODE=fifo` | presented intervals (the `frametime.csv`), wall p99, the game clock at a fixed VBlank |
| P1 144 Hz + FIFO | desktop refresh 144 Hz, `GT4_PRESENT_MODE=fifo` | as P0; the frame interval must stay ~16.7 ms (60 VBlank-driven), not 6.94 |
| P2 144 Hz + mailbox | 144 Hz, `GT4_PRESENT_MODE=mailbox` | as P0; no tearing-driven pacing change |
| P3 144 Hz + immediate | 144 Hz, `GT4_PRESENT_MODE=immediate` | as P0, plus the tear line position (a photo, not a number) |

Pass: **the game's VBlank cadence and the race clock are identical in all four** (the identity gates below), and the
presented interval distribution in P1-P3 shows no more than one frame of extra latency against P0 (the p99 of the present
interval, not its mean).

## 4. The scenes on the i7

The i7 is the streamer-like CPU: the four Spec II scenes plus the license are the streamer's own cases, so the CPU side
matters there as much as the GPU. Run **L0** and **L3** for all five, and add `night 6x` as the heavy PC case.

| scene | drive | scale | window |
|---|---|---|---|
| license B-1 | `licenca-b1` | 1x | 2,400-3,500 |
| Opera Paris | `opera` | 3x | 20,000-21,600 |
| S.S. Route 5 | `ssr5` | 3x | 20,000-21,600 |
| Tsukuba Wet | `tsukuba-wet` | 3x | 20,000-21,600 |
| George V rally | `george-rally` | 3x | 20,000-21,600 |
| night race | `night215` | 6x | 7,800-11,400 |

## 5. What is being re-checked

**Kept on this branch (must not regress; the GPU-sensitive ones are marked *):**

| item | switch / shape | desktop result |
|---|---|---|
| IN-540 aliased depth fill * | `DepthBlocksInColour` blocks from the address tables | batch stage -22 % at night 3x |
| IN-547 scissor view | `GT4_CORE2_SCISSOR_VIEW` (default on) | license GS -1.7 % |
| IN-558 bundle | `Candidate` window + rect inlining (exact) | license GS -0.33, Opera GS -0.23 |
| diagnostics | `GT4_CORE2_SPIKE_*`, per-VBlank CSV, `[core2] draw ring` / `front-end prims` lines | inert |

**GPU-bound negatives the ledger recorded (must stay negative: a resurrected one is a regression):** IN-443 (large textured
sprite passes as compute), IN-444 (hardware bilinear in Performance Mode), IN-449 (bilinear via `textureGather`), IN-454
(clears as strips), IN-460 (GLM patch kernel), IN-496 (no interlock for large disjoint batches on integrated GPUs), IN-501/502
(no road sheen / no alpha fills in Performance Mode), IN-503 (the sheen leaving early), IN-541 (CLUT-load scope), IN-543
(no self-copy from 8x), IN-548 (palette segments under the tone hook), IN-553/555 (flare readback), IN-556 (two-stage
pipeline), IN-557 (band cull), IN-559 (per-record). Each row's ledger entry names its scene and window; run the ones the
owner wants re-checked at L0 and L3 with their switch on and off, and compare against the ledger's number.

## 6. Identity on the Avell

For every scene above, one frame-dump pair (`EVERY=600`, two frames per window) between the build and the reference
(`main`'s build or the previous release), compared with `frame_diff.py`: **frames byte-identical**. This is the gate the
suite exists to protect: the GPU-side items must not change a pixel even where they are faster.

## 7. Results table (to fill in by hand)

| level | scene | wall p50 / p99 (ms) | 1 % low | GPU busy p99 | flare wait | CSV |
|---|---|---|---|---|---|---|
| L0 | license 1x | | | | | `avell-L0-license.csv` |
| L0 | night 3x | | | | | `avell-L0-night3.csv` |
| L0 | night 6x | | | | | `avell-L0-night6.csv` |
| L0 | Opera 3x | | | | | `avell-L0-opera.csv` |
| L0 | (the other three Spec II scenes) | | | | | |
| L1..L3 | as L0 | | | | | |

Present matrix (P0-P3): presented interval p50/p99, the game clock at the window's last VBlank, and one screenshot per case.

## 8. Commands (the same shapes the desktop uses; run by the owner on the Avell)

```bat
:: one scene, one level, the per-VBlank CSV and the spike CSV
set GT4_BENCH_SCENE=night
set GT4_MTVU_REPORT_EVERY=600
set GT4_HW_PROFILE_EVERY=600
set GT4_CORE2_SPIKE_MS=0.01
set GT4_CORE2_SPIKE_LINES=100000
night-run.sh avell-L0-night3 C:\gt4\gt4vulkan.exe
python scripts\coregs2_bench\cpu_summary.py avell-L0-night3 --csv avell-L0-night3.csv

:: present matrix: one run per mode at 144 Hz, then the same at 60 Hz
set GT4_PRESENT_MODE=mailbox
night-run.sh avell-P2-144 C:\gt4\gt4vulkan.exe
```

Notes for the owner: set the panel refresh by hand before each P run and write it in the notes; do not change the
Afterburner profile during a run; keep the shader cache folder warm between runs (`GT4_HW_SHADER_CACHE`), and give each
level its own folder so a CSV is never mixed.
