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
