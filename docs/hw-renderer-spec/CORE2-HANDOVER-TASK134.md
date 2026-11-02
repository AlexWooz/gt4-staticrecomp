# Handover: CoreGS 2.0 CPU work to the game-code optimization agent (TASK-134)

Written 2026-10-30 by the CoreGS 2.0 agent (TASK-027), at the owner's request, for the agent that continues the CPU work
in `docs/GAME-CODE-OPTIMIZATION-STUDY.md`. Read this first, then that study. Details of every step:
`CORE2-PERF-LEDGER.md` (numbers, kept and dropped), `IMPLEMENTATION-NOTES.md` (how each IN-nnn works),
`CORE2-CPU-SURVEY.md` (the CPU fronts of the GS side), `docs/journal/2026-10-30.md`.

Agent status: this work is clean room (decision 0070). `native/hw_gs/` and `task/027-*` must stay free of PCSX2-tainted
agents; the TASK-134 agent is not tainted as long as it keeps away from PCSX2 sources and the fork.

## Video (2026-10-31, already done here, do not redo)

The IPU decoder runs on the EE thread. IN-490 and IN-491 (`scripts/prepare_ipu_fast.py`, `native/ipu_fast.hpp`, capsule
`GT4_IPU_RUNTIME_SOURCE`) cut it by 55% with the donor's exact results: desktop, track selection, EE 6.03 to 4.11 ms. What is
left during video is yours: on that desktop profile the game's own code leads, `gen::unit_011A` at 18% of the EE thread (guest
window 0x468000-0x46BFFF, what it does not identified yet). `docs/journal/2026-10-31.md` has the laptop numbers.

## Where things are

- **Branch:** `task/027-coregs2`. It already contains `task/134-game-code-optimization-study` (merged on 2026-10-30,
  documents only). The PR back into `task/134-game-code-optimization-study` waits for the owner's approval; never merge
  without it.
- **Newest build (2026-10-30, evening):** candidate 354, the game of 1.3.0-rc8: 347 + texture replacement (IN-470), GPUs without interlock (IN-471), launcher entry (IN-474), PGO retrained (IN-473). None of them changes the CPU figures below by more than noise (GS worker 5.14 ms at 3x).
- **Candidate of these figures:** 347 (`build/candidates/coregs2-candidate-347` in the `gt4-wt-027-coregs2` worktree), built from
  commit 68af02f with PGO and ThinLTO of the GS libraries. Play script: `build/hwr-play-scale.ps1 -Scale 3`.
- **State:** frames identical to 337 at 3x and 12x; CTest 267/268 (the known `python_native_contracts` GS settings
  failure from the rc7 merge), 137/137 `hw_` tests without the interlock. The owner played 332 and liked it; 347 is not
  play tested yet.

## Results so far

Headless night race, RX 9060 XT + Ryzen 7 5700X, ms per VBlank, race window. 3x is interleaved 2+2 in one session, so
313 reads a little higher than in older notes; 12x is one run each. 313 is the start of the CPU work; 332 is what the
owner played; 347 is current.

| 3x | 313 | 332 | 347 | 347 against 313 |
|---|---|---|---|---|
| Process CPU | 22.0 | 14.5 | 13.8 | -37% |
| GS worker CPU | 7.76 | 5.84 | 5.08 | -35% |
| VU1 worker busy | 3.29 | 2.90 | 2.54 | -23% |
| Wall | 11.1 | 8.69 | 8.26 | -26% |
| EE critical path | 10.7 | 8.49 | 7.85 | -27% |
| Frame p50 | 10.9 | 8.38 | 7.75 | -29% |
| Frame p95 | 13.7 | 11.1 | 10.6 | -23% |
| GPU busy | 4.28 | 3.85 | 3.81 | -11% |

At 12x, 332 -> 347: GS worker 5.88 -> 4.98 ms, process CPU 15.0 -> 13.5 ms, GPU busy 21.3 -> 21.7 ms (the IN-463 copies),
frame p50 24.4 -> 24.8 ms. 12x is GPU bound, so CPU gains do not shorten its frame.

## What already happened on your plan

| Your item | State |
|---|---|
| P0 measure | Not done, except the sampler and bench tools below, which you can reuse. M9 is answered: SSE2 -> SSE4.1 wall -3.8%, VU1 worker -16%; AVX2 no further gain |
| P1a SSE4.1 | Done: `GT4_SSE41` is ON by default in CMake (candidate 313) |
| P1b doorbell diet | Not done as you defined it (per-thread counters, padding, masks). Separately, IN-459 made the worker's wakes of the EE quiet: it rings loud only past `GT4_MTVU_WAKE_KB` KiB of output, before it sleeps or stops with quiet publishes pending, or on a full ring; 650 -> 16 EE wake-ups per VBlank |
| P1c to P1g | Not touched |
| P2, P3 | Not touched |
| P4 ThinLTO, PGO | Done **only for the GS libraries** (IN-468, option in `native/hw_gs/CMakeLists.txt`). Runtime, native adapter, VU AOT and the EE library are yours; see "PGO" below |
| P5 VIF path | Not touched, except the output scan of IN-469 (`GifScanner`, VU1 worker -9%) |
| P6 CoreGS 2.0 intake | Done (IN-446, IN-459, IN-461, IN-462): the EE no longer copies, forwards or walks the VU1 output; on by default for the hw renderer |
| P7, P8, P9 | Not touched |

## Game-side files this branch changed (merge with care)

- `native/mtvu_ring.hpp`: `Doorbell::ring_quiet`, `notifies` in the stats, `SpscByteRing::try_push(header, payload, wake)`,
  `set_producer_waiting` / `producer_waiting`.
- `native/mtvu_journal.hpp/.cpp`: `WorkerEnd(wake_output_bytes)`, quiet publishes; `GsSink::direct_job`,
  `EeJournal::set_direct`; `send_chunks` does not pump output in direct mode.
- `native/mtvu_runtime.hpp/.cpp`: donor callbacks `direct_gs_available` and `post_gs_job`, `Runtime::direct`,
  `completed_job`, `wait_for_jobs`; `sync()` waits on `completed_job` except for `download` and `vblank_pending`;
  `drain_download` is journal-only in direct mode; `GT4_MTVU_DIRECT` defaults to on.
- `native/mtvu_bridge.cpp`, `native/gs_stream/gs_stream_client.*`, `gs_stream_sink.hpp`: the seam for direct intake.
- `native/mtvu_measure.*`: `GifScanner` skips PACKED tags without A+D.
- `native/mtvu_journal_fixture.cpp`: quiet-wake and direct variants, about 35-45 s of the 120 s CTest timeout.

Rules that came out of this, which your work must keep:

- PATH1 output never carries SIGNAL, FINISH or LABEL (the worker checks it; 0 in GT4). In direct mode such an IRQ would
  not be applied at all, so a new source of them must go through the journal.
- Verify mode (`--vu1-verify`) keeps the journal: its hash covers the GS stream on the EE.
- With direct intake the GS worker is the output ring's only reader. Anything else that reads the ring breaks it.

## What limits a frame now (RX 9060 XT + 5700X, 3x night race)

The EE reaches the flare read (a LOCAL->HOST download, once per VBlank) and waits for the GS worker to finish everything
before it: `read_fifo_ms` about 4 ms of `[hw] transport`. The GS worker is the slowest stage, so every millisecond off it
shortened the frame so far. If your work speeds up the EE or the VU1 worker, expect the frame to stay bound by that wait
until the GS worker is done first: read `ee_critical_path` and `read_fifo_ms` together. Performance mode
(`GT4_CORE2_PERFORMANCE=1`, decision 0073) answers that read locally and removes the wait.

On a 4-thread laptop the total CPU matters as well as this path. Laptop numbers for CoreGS 2.0 do not exist yet.

## Profile of the threads that are yours

Candidate 337, 3x, VBlanks 9000-11000 (`GT4_CPU_SAMPLER`), ms per VBlank. A flat profile on the GS side; yours has clear
items:

- **VU1 worker (3.9 ms of CPU):** `vuops::vfix` 0.34 (your P7), `RtlQueryPerformanceCounter` 0.23 (`gt4::perf` scopes on
  the worker, your M7), `vif::unpack` 0.15, `vugen::mac_of_result` 0.12, `std::bit_cast` 0.09, `vuops::fout` 0.08.
- **EE (4.1 ms of CPU):** `StreamRing::try_push` memcpy 0.25 (GIF data into the GS ring, mostly 3.6 MB of texture uploads
  per VBlank; `copy_ms` in the transport line), `gen::unit_00E7/EA/E9/D5/E8` 0.16-0.07 each, `vif::unpack` 0.155,
  `vu::maddbc` 0.10 (VU0), `spu2::render_voice` 0.10, `spu2::Envelope::step` 0.06.
- Reading the report: see `scripts/coregs2_bench/README.md` (time in `ZwWaitForAlertByThreadId` is mostly work before the
  sleep; `__NLG_Return2` is memcpy, not exceptions).

## How to measure

`scripts/coregs2_bench/README.md` has the scenes, the method and the identity gates. In short:

```bash
SCALE=3 bash scripts/coregs2_bench/series.sh a1:<exe A> b1:<exe B> a2:<exe A> b2:<exe B>
python scripts/coregs2_bench/cpu_summary.py a1 a2 b1 b2
SCALE=3 EVERY=600 FROM=6600 FRAMES=12100 bash scripts/coregs2_bench/night-run.sh idb <exe B>
python scripts/coregs2_bench/frame_diff.py night-ida night-idb
```

- Since candidate 335 two runs of one build are identical at 3x and 12x (IN-463). The "run-to-run pixels" of older notes
  are gone: a frame difference between candidates is real, and it is a good gate for game-side changes too.
- The scenes run the hw renderer headless; the study's M3 baselines can use the Arcade scene and its replay from there.
- No measurement while anything else is building. The owner may be playing: check first.

## PGO (IN-468)

`scripts/coregs2_bench/pgo-build.sh <build dir> <out dir>` trains on the night race and the Arcade path and rebuilds.
Gain on the GS worker: about -11% with ThinLTO, GIF front end -28%, frames identical. Facts for extending it:

- Options: `GT4_HW_PGO=generate|use`, `GT4_HW_PGO_PROFILE`, `GT4_HW_THINLTO`. They apply to `GT4_HW_PGO_TARGETS` in
  `native/hw_gs/CMakeLists.txt`; add your libraries there or give them their own list.
- `generate` builds only `gt4vulkan` (fixtures do not link the profile runtime). Reset the build dir with
  `-DGT4_HW_PGO= -DGT4_HW_THINLTO=OFF` for fixtures and A/B work.
- Compare steps on a plain build; take the PGO build last, with a fresh profile.
- **Not wired into the release yet:** `launcher/tools/build-release.ps1` does not pass these options. The profile is kept
  outside git in `private/coregs2-pgo/` (it holds only hashes and counts of MIT code). Whether release builds train on
  the spot or use a stored profile is open: ask the owner.

## Switches (all A/B; the defaults are the shipped behaviour)

| Switch | Default | Effect when changed |
|---|---|---|
| `GT4_MTVU_DIRECT=0` | on | The EE journal forwards the VU1 output |
| `GT4_MTVU_WAKE_KB=<n>` | 64 | Output between worker wake-ups of the EE; 0 wakes for every record |
| `GT4_HW_GS_THREAD=0` | on | No GS worker (direct intake then stays off) |
| `GT4_CORE2_SELF_COPY=0` | on | Samples the target itself (IN-463, frames vary by run) |
| `GT4_CORE2_GLM_PAGES=0` | on | GLM hazards on the whole buffer (IN-464) |
| `GT4_CORE2_LAZY_BARRIERS=0` | on | A barrier around every command |
| `GT4_CORE2_GROUP_DISJOINT=0` | on | Per-primitive path waits before every primitive (IN-466) |
| `GT4_CORE2_GPU_STATISTICS=1` | off | Pipeline statistics: prims, fragments, compute in the reports (IN-465) |
| `GT4_CORE2_PASS_TIMESTAMPS=0` | on | No `gpu_draw_passes` |
| `GT4_CORE2_BARRIER_TRACE=1` | off | Full barriers by command and hazard in the report |
| `GT4_CORE2_SELF_TRACE=<vblank>` | off | Lists that VBlank's self-sampled batches |
| `GT4_HW_INTERLOCK=0` | on | The path of GPUs without the pixel interlock (GPU about 11x slower) |
| `GT4_CPU_SAMPLER=<file>` | off | Whole-process sampler (`_FROM`, `_TO`, `_PERIOD_US`) |
| `GT4_HW_PROFILE_EVERY`, `GT4_MTVU_REPORT_EVERY` | off | The 600-VBlank report lines |

## Open on the GS side (not yours, listed so nobody repeats them)

- Batches: 443 per VBlank, 278 flushed by CLUT loads, about 52 of those with the same texture (R9b: a palette per
  primitive). Merging across textures fights the specialised pipelines (IN-437), so it trades GPU for CPU at high scales.
- Vulkan recording and submission are about 0.9 ms of the GS worker; a submission thread was considered and not tried.
- The per-vertex and per-primitive code is at its floor for small edits: a copy removed from translate gave 0.01 ms.
- The 12x full-screen copy of IN-463 costs up to +1.3 ms GPU; spec 8.8's feedback rule for 1:1 passes would drop it but
  changes the sub-pixels the 12.4 coordinate rounds to a neighbour. An owner decision, since it changes frames.
- Mobile: without the interlock the race needs about 44 ms of GPU at 3x. The owner set mobile aside for now.
- Latent, unreachable today: in `Gpu::Draw` the uniform ring could submit after the vertex ring reserved, leaving the
  vertex offset in the previous slot's share (2 MiB of uniforms per slot, a submission every 32 batches).

## The laptop on candidate 382 (owner, 2026-11-02)

The owner's F7/F8 on the i5-7200U / HD 620, Spec II, in a window; each is their own stretch:

| Scale, mode | Choice | VBlanks/s | Critical path | VU1 program | Thread CPU |
|---|---|---|---|---|---|
| 1x, Performance | 60 | 60.00 | 9.40 ms | 7.07 ms | 8.88 ms |
| 1x, normal | 60 | 59.89 | 11.69 ms | 8.03 ms | 10.36 ms |
| 2x, Performance | 60 | 49.22 | 20.28 ms | 12.42 ms | 15.68 ms |
| 2x, Performance | 30 in races | 60.00 | 6.99 ms | 4.23 ms | 6.62 ms |

The VU1 program does the same work at 1x and 2x, so its 12.42 ms suggests the processor slows when the graphics part is busy (Hypothesis, shared power and memory; not measured apart). Sessions: `\\100.78.47.2\GT4Recomp\data\sessions\session-1793647975`, `-1793648174`, `-1793648322`, `-1793648442`. The owner's rc9 target is this CPU work.

## Gotchas that cost time

- **Line endings are mixed by file.** `sed -i` in git bash and naive scripts convert CRLF to LF. After an edit compare
  `git diff --stat` with `git diff --stat --ignore-cr-at-eol`.
- **A bash heredoc can turn `\n` inside C++ strings into a real newline.** Use the editor tool for those lines.
- `build/` in the worktree and `private/` are ignored. Never stage game data, cards or binaries.
- Exclusive stage times (`x_*`) show small gains that the totals hide: 1-3% is run-to-run drift on the totals.
