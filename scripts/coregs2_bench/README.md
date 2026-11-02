# CoreGS 2.0 headless benches

The tools behind `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`. They run the game without a person, with scripted pad
presses, on two scenes. Git bash on Windows; Python with numpy and Pillow for the two `.py` tools.

Needs (not in git, see `common.sh`): the main checkout's `private/reconstructed/SCUS_973.28.elf` and disc image, the
cards `private/coregs2-bench/night-card.ps2` and `private/slice110-menu-pilot/name-control4/card.ps2` (copied per run,
never written). Close the game first; never run while an owner session is running. No perf measurement while a build
runs.

| Tool | Use |
|---|---|
| `night-run.sh <name> <exe> [ENV=VAL ...]` | Night race (GT Mode, B-Spec): the reference scene. `SCALE`, `EVERY`, `FROM`, `FRAMES` |
| `arcade-run.sh <name> <exe> [ENV=VAL ...]` | Arcade path: menus, showroom, pre-race, race start |
| `series.sh <tag:exe[:ENV=VAL,...]> ...` | Interleaved night runs with the 600-VBlank reports on |
| `cpu_summary.py <tag> ...` | Race-window averages per run: `gs_cpu`, `process_cpu`, `wall`, `ee_critical_path`, `frame_p50/p95`, `gpu_busy`, the `x_*` GS stages |
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

`record-input.sh <name>` opens Spec II from the bench's card without game data and records every pad change by guest VBlank, into `private/coregs2-bench/inputs/<name>.txt` in the main checkout (never overwritten). F9 ends the recording and closes the game. Keep the game focused: without focus the keyboard reads as released, and that is recorded too.

`replay-run.sh <name> <run> <exe> [ENV=VAL ...]` replays it from a fresh copy of the same card up to its last VBlank, into `$GT4_BENCH_OUT/replay-<name>-<run>/`. `WINDOW=1` opens a window and writes `frametime.csv`; `PACED_FROM=<VBlank>` skips the waiting before that VBlank (use the VBlank where the measured stretch starts). Frame dumps slow a window run: leave `EVERY` unset when measuring.

Pass `GT4_HW_FLARE_DEADLINE_MS=0` when comparing frames from window runs: a late flare answer depends on real time.
