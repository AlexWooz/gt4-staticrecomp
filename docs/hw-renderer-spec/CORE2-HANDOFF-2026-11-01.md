# CoreGS 2.0 handoff, 2026-11-01

For the next agent on TASK-027. Written because the previous session's goal mode kept restarting it; there was no technical problem. If you were started with the prompt that points here, you have no goal condition to satisfy: work on what the owner asks and stop when it is done.

## Rules that decide your work

Read `AGENTS.md` first; it is canonical. The ones that matter most here:

- **Language:** Brazilian Portuguese with the owner, in plain lay language. Repository text, comments and commits in en-US.
- **Clean room:** you are a TASK-027 agent and not PCSX2-tainted. Never open PCSX2 sources, the `moesuito/gt4recomp-gs` fork, `private/tooling/pcsx2-*` or `private/slice189-pcsx2-ref`. From `gt4-gs.exe` you may use final frame images only, never its dumps or logs. The previous session read only the game process's report lines of GSdx sessions (`[perf]`, `[frame]`, `[pacing]`, `[mtvu]`, `[gs-ipc]`).
- **Performance Mode rule (owner):** nothing that affects gameplay may be cut. A cut or lowered graphics effect goes in Performance Mode only; a gain that cuts no effect goes in normal mode.
- **The renderer defines the graphics options** (owner): do not add a CoreGS option with no GSdx counterpart in the launcher.
- **No upload without the owner's word**, not even a draft. The GitHub draft `v1.3.0-rc8` still holds candidate 354. Before any upload, compare the installer contents with their sources.
- **Never merge without the owner's approval.** Never rewrite history. Commits and pushes on the branch are authorized, under moesuito:
  `git -c user.email=57041838+moesuito@users.noreply.github.com -c "user.name=João Alano" commit`
- **Never stage** game data, cards, binaries, ISO, ELF, captures or logs.
- **Before launching the game**, check that no game process is running: the owner tests on this PC. No performance measurement while a build or other runs are active.
- **Telegram:** the owner asked to stop the notices for now; send none until they ask again.
- **Credentials:** use only the owner's passwordless SSH to the laptop (`ssh -o BatchMode=yes Jamily@100.78.47.2`). Never log in with a password. The laptop was off at the end of this session.

## Where things are

- **Branch and worktree:** `task/027-coregs2` in `C:\Antigravity\gt4-wt-027-coregs2`, pushed, clean. Not merged anywhere; PR #142 is closed. A new PR into `task/134-game-code-optimization-study` waits for the owner.
- **Assets:** the main checkout `C:\Antigravity\gt4-staticrecomp` holds `private/` and the discs. Its own working tree has uncommitted files that belong to others: leave them.
- **Latest build, candidate 369 (`1.3.0-rc8`):**
  - pins `docs/inputs/slice369-pcsx2.json` (`1aa79cf0...`) and `docs/inputs/slice369-spec2.json` (`d3380f68...`);
  - game files `private/slice369-rc8-game`, distribution `private/slice369-dist-130-rc8`, output `private/release-130-rc8g-out` (installer `59dd9a42...`, zip `372f141d...`);
  - installed in the owner's `C:\Program Files\GT4Recomp` through the installer's update mode. Their saves, replays and renderer data were unchanged.
- **The laptop** (i5-7200U, HD 620) still has candidate 367: no Spec II on CoreGS 2.0 and no compile window.
- **State pages:** `docs/STATUS.md` (top), `docs/journal/2026-11-01.md`, `docs/hw-renderer-spec/IMPLEMENTATION-NOTES.md` (IN-506, IN-507), `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`, ai-memory `concepts/coregs2-rc8-state`.

## Done in the last session

- **IN-506:** Spec II runs on CoreGS 2.0 (the launcher and `native/standalone.cpp` gates are gone; Spec II's program is built with the renderer).
- **IN-507:** under CoreGS 2.0, Play runs `<game program> --prepare-shaders` and shows "Compiling shaders" only when something must compile. List `native/hw_gs/core2/core2_known_variants.inc` (544 keys, `scripts/coregs2_bench/known_variants.py`). The owner's first Play compiled 300 variants through the real launcher; with the prepared cache, George V Paris at 6x had 0 uber batches of 5.9 million.
- **Checks on 369:** v2.00 frames against 367 10/10 and 14/14; `ctest` 147 passed, one skipped; launcher `cargo test --workspace` 599; focus pages en-US 211/223, pt-BR 218/230, the same 12 known failures.

## The owner's measurements (George V Paris, 6x, Spec II, RX 9060 XT)

| ms per frame | GSdx | GSdx Performance | CoreGS 2.0 | CoreGS 2.0 Performance |
|---|---|---|---|---|
| VBlanks/s | 57.50 | 60.00 | 49.60 | 57.73 |
| Frame median / p99 / max | 16.99 / 21.86 / 25.86 | 16.65 / 19.64 / 21.43 | 19.90 / 28.92 / 31.26 | 16.83 / 27.74 / 33.62 |
| EE critical path | 16.84 | 7.58 | 20.13 | 14.76 |
| GS thread | 7.17 busy | 2.92 busy | 17.14 with waits | 16.00 with waits |

The GS thread figures are different measures, and the stretches of track differ (GSdx 98 s, CoreGS about 50 s). CoreGS 2.0 is limited by its GS thread, not by the GPU (GPU busy 12.2 and 10.4 ms). Readbacks are its largest cost: 7.7 and 7.2 ms, of which 5.5 and 5.0 ms wait for the GPU. Sessions: `C:\Program Files\GT4Recomp\data\sessions\session-1793503160`, `-1793503281` (CoreGS 2.0), `-1793505683`, `-1793505864` (GSdx).

## Next step: the readback before a write on George V Paris (done: IN-508, candidate 370; the owner's F8 is pending)

Kept as written for the record. The results are in `docs/STATUS.md` and the perf ledger. "Latest build" above is now candidate 372, which also removes the frametime wave the owner found on 370 (IN-509), the race start drop and the showroom wave (IN-510 to IN-513). The page-aligned test proposed below was not enough: small textures at a base inside page 382 needed block cells too.

No code was changed for this. `scripts/coregs2_bench/readback_trace.py` summarises a trace run:

```bash
export GT4_BENCH_OUT=$PWD/build/evidence/rb
EVERY=100000 FROM=999999 SCALE=6 FRAMES=21400 bash scripts/coregs2_bench/spec2-run.sh george trace build/spec2-core2-build/gt4vulkan-spec2.exe GT4_CORE2_PERFORMANCE=1 GT4_CORE2_STORE_TRACE=1 GT4_CORE2_READBACK_TRACE=1 GT4_HW_PROFILE_EVERY=300
python scripts/coregs2_bench/readback_trace.py build/evidence/rb/spec2-george-trace/log.txt 19800 21300
```

**Found (Confirmed from the trace and the page arithmetic):** about once per VBlank a streamed texture upload, CT32 at BP 12224, BW 1, 64 x 1184 or 64 x 1440, triggers a store of the Z32 target at BP 11232, BW 4, 256 x 256, drawn 0,0,191,255.

- That target's drawn pixels sit in pages 351 to 381: page 351, 8 rows of 4 pages, columns 0 to 2, so the last is 351 + 7 x 4 + 2.
- The upload starts at page 382 (12224 / 32), which is column 3 of the target's last row: never drawn.
- So the write touches no drawn pixel. The store runs because `SpanOfRect` counts whole page rows, and `PageExact` refuses because CT32 and Z32 are different families.
- The store reads back three targets per VBlank, each up to 65,536 pixels at 1x: BP 11232, BP 11200 and BP 12224, all BW 4.

**Proposed fix (Hypothesis, untested):** an exact page test: when both bases are page-aligned, compare the pages holding the target's drawn rectangle with the pages the write touches, allowing for each format's page size and for wrapping at 512 pages. No page in common means:

1. no store in `StoreDrawnBeforeWrite` (`native/hw_gs/core2/core2_renderer.cpp`);
2. no dirty rows in `AddUploadDirty` for the drawn part. This one is required: 4.1 marks whole page rows of that target dirty, and a later patch would read stale memory over the drawn pixels if the store is gone. Handle the undrawn page correctly: it must still count as written memory (see the `valid` and `dirty` rules and the 4.1 deletion rule in `OnMemoryWritten`).

It cuts no effect, so it belongs in normal mode. Keep an A/B switch (`GT4_CORE2_...=0`) and add a CTest case in `hw_writeback_tests.cpp`.

**Measure and gate:** identity at 3x and 12x and Arcade against candidate 369 (`scripts/coregs2_bench/README.md`, section Method), the same on Spec II with `spec2-run.sh`, then the owner's F8 on George V Paris at 6x. Remaining readbacks to look at afterwards: "keep" (1.2 ms of waiting, four 640 x 448 targets), and the EE's wait for the GS thread, 9.1 ms in `run_due_events` under Performance Mode (Hypothesis: the EE waits for the GS thread to catch up; `worker_to_ee` waits are 281 per frame against 0 under GSdx).

George V Paris headless is an Arcade rally against one car with no B-Spec icon, so the car hits a wall after about 17 s of race. Measure VBlanks 19500 to 21300, or find steering presses for a longer stretch.

## Candidate 373 (frame pacing, IN-514 to IN-519)

The state of `main` of this work is in STATUS and the perf ledger. What the next agent needs:

- **Which display a run used:** `[hw] presentation: ... last_rect`. The owner's game stream gives a 1920 x 1080, 60 Hz virtual display; without a client, the real 3440 x 1440, 144 Hz monitor at 21.5:9, 15% more GS work. Pass `GT4_DISPLAY_ASPECT=16:9` to compare runs.
- **The first thing to read after an owner test:** `frametime.csv` in the session folder, through `scripts/coregs2_bench/frametime_plot.py out.png name=<csv>`: the shown line is what reached the display.
- **The stall goes with 60 Hz, not with the virtual display** (the same display at 144 Hz: 0.79 ms against 10.96 ms). It may reach any player on a real 60 Hz monitor, which was not tested. `Win32_VideoController.CurrentRefreshRate` gives the rate of a run. The second queue removes it at 60 Hz (table in the ledger).
- **Open at 60 Hz:** about 50 ms from presentation to display, against 13-20 ms at 144 Hz. Two images save about 10 ms and lose frames; latest ready is worse.
- **Next step:** the GS thread still waits for the GPU at the flare read (2-3 ms per VBlank, 12 ms on heavy GPU stretches). Issue the reduction with `ReduceLater` and let the EE wait on its fence with the IN-514 limit.
- **Closed by candidate 374:** IN-514's late flares are gone (IN-522).
- **Candidate 374:** IN-521 (display queue), IN-522 (the EE does not wait for the flare table; the VU1 worker does), IN-523 (D1 measured, not built). The GS thread still waits for the GPU at the read (4.3 ms per VBlank at 6x, 10 ms at 12x), which no longer holds the EE. Next GPU target at 12x: the constant-colour blended layers, 30.7% of a race's area. The open items are at the top of STATUS.

## Open items and owner decisions

- **GSdx stuttering is not fixed.** A proposed session card "GSdx: compile shaders before Play in gt4-gs" (task_b82479c6) exists; it needs a PCSX2-tainted agent, never you.
- **The launcher note says CoreGS 2.0 uses "about half the processor of GSdx".** The 6x data neither confirms nor refutes it, since it does not measure the whole process. Check before publishing. The same note set lives in `docs/release-notes/1.3.0-rc8.md` and `docs/release-notes/launcher/` (en-US and pt-BR, both `1.3.0-rc8.md` and `1.3.0.md`: a labelled build embeds `1.3.0.md`).
- **Owner's decisions pending:** the darker road and the colour filter cut (IN-505) in Performance Mode; hardware depth (D1); rc8 upload; PR into task/134.
- **Not measured:** the compile time on the HD 620 (laptop off), and candidate 369 on the laptop. When the laptop is back, update it from the portable zip over the plain program files, never touching `user\` or `data\`, and check those are unchanged. The launcher's cache there is `data\shader-cache`, not `user\shader-cache`.

## Build recipes

Run from the worktree. Configure outside a VS shell; build inside one (`native/build.cmd` shows `VsDevCmd.bat`). From Git Bash, set `MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'` when passing `/O2` to CMake, or the flag becomes a path.

- **v2.00:** `cmake --build build/core2-build --target gt4vulkan` (already configured, PGO use and ThinLTO).
- **Spec II:** `cmake --build build/spec2-core2-build --target gt4vulkan`. It was configured with `-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2`, capsules `slice276-spec2-iop-runtime`, `slice277-license-guard-runtime`, `slice365-ipu-fast-runtime`, `slice200-memcard-runtime`, the same PGO profile `build/pgo/gs.profdata`, `-DGT4_SSE41=ON`, and the rest as `build/core2-build/CMakeCache.txt`.
- **A release candidate:**
  1. copy both executables to a new `private/sliceNNN-rc8-game` and write new pins from the previous ones (new `runtime` hashes; never edit an old pin);
  2. `scripts/make_distribution.py <new dist dir> --assets-root <main checkout> --exe ... --runtime-manifest ... --second-exe ... --second-runtime-manifest ...`;
  3. with `CARGO_TARGET_DIR=build/cargo-release`, `launcher/tools/build-release.ps1 -Sdl3Root <main checkout>\private\tooling\sdl3-3.4.18 -Label rc8`, then again with `-Portable`;
  4. `makensis /V2 /DVERSION=1.3.0 /DLABEL=rc8 /DLAUNCHER_EXE=... /DGAME_DIR=... /DSDL3_DLL=... /DWEBVIEW2_BOOTSTRAPPER=<main checkout>\private\release-100-webview2\MicrosoftEdgeWebview2Setup.exe /DOUTFILE=... installer\gt4recomp.nsi`;
  5. `scripts/make_portable_zip.py <out> --version 1.3.0-rc8 --launcher-exe ... --sdl3-dll ... --game-dir ... --sums-file <out>\SHA256SUMS`.
- **Installing on this PC:** with the game closed, close the launcher and run the installer as `/S /UPDATE /D=C:\Program Files\GT4Recomp` elevated (`Start-Process -Verb RunAs`); the owner accepts the Windows prompt. Compare the saves and replays before and after, and the installed hashes with the build.
- **Tests:**
  - `ctest --test-dir build/core2-build -R "^hw_|native_ipu_fast|standalone" -j 4 --timeout 300`;
  - in `launcher`, with `GT4_SDL3_ROOT` set: `cargo test --workspace`. `flow_audit::a_1_2_x_install_moves...` fails while any game process runs, so run it with none;
  - focus pages: serve `launcher/crates/gt4l-tauri/ui` over HTTP and open `dev/focus-check.html?lang=en-US` and `?lang=pt-BR`. The 12 known failures are the 11 tab-highlight timing checks and one focus check.

**Traps:**

- A shader change means cold runs; read `compile_ms`.
- The driver's own cache hides first-time compile cost on this PC.
- Measuring on the laptop's set 19 runs Performance Mode by default (`rodar.ps1`).
