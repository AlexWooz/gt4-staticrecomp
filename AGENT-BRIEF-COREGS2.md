# Agent brief: CoreGS 2.0 (TASK-027) on `task/027-coregs2`

Written 2026-10-21 for the external implementation agent (OpenAI GPT 6.1 Sol). It is self-contained: read it
fully, then `AGENTS.md`, then the specification. Repository text is en-US; talk to the owner in Brazilian
Portuguese, in plain lay language.

## 0. Your status, your folders, your session

- **You are a TASK-027 clean-room agent.** You implement from the specification only (section 2).
- **Your session opens in the main checkout `C:\Antigravity\gt4-staticrecomp`.** That is only where the ai-memory
  MCP finds the project. **Do all your work in the worktree `C:\Antigravity\gt4-wt-027-coregs2`** (branch
  `task/027-coregs2`). Never edit files in the main checkout: other agents work on `main` and in other worktrees
  at the same time.
- **You are the only writer of `task/027-coregs2`.** Nobody else commits to it while you hold it.
- **Builds** run from the worktree source with `-DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp`; build output
  goes to new `C:\Antigravity\gt4-staticrecomp\private\sliceNNN-*` folders (section 6). The worktree has no
  `private/` of its own; never create a junction or symlink to the main checkout's `private/`.

### Session start (every session)

1. Follow the ai-memory section of `AGENTS.md`: workspace `default`, project `kaezzey-gt4-staticrecomp` (pass both
   on every call if your client does not forward a session id).
   - `memory_recent`, then `memory_query` for "CoreGS 2.0", "TASK-027", "HWR".
   - Read in full: `concepts/current-state`, `_rules/project-rules`, and `decisions/coregs2-implementation` (your
     progress page, section 6) plus any other CoreGS page the query returns.
   - **Memory is prose history, never a design source.** Pages about gt4-gs, the PCSX2 GS or its patches are
     background only. The specification is the only design source. `AGENTS.md` and the repository documents win
     over memory.
2. Read `docs/hw-renderer-spec/PROGRESS.md` on the branch (your running log) and this brief.
3. `git -C C:\Antigravity\gt4-wt-027-coregs2 status --short` and `git log --oneline -5`.

## 1. Goal

**CoreGS 2.0** (code name HWR) is GT4Recomp's own **MIT hardware GS renderer on Vulkan, built into the game
executable**: one process, no GPL code. GT4Recomp is a static recompilation of Gran Turismo 4 NTSC-U
(SCUS-97328) for Windows x86-64 + Vulkan; the guest code (EE, VU, IOP) is compiled ahead of time, there is no JIT.

- Today the release (1.0.0, `main`, slice214) renders with **gt4-gs.exe**, the PCSX2 GS in a separate GPL process
  fed over shared memory (decision 0070), or with **CoreGS 1.1**, the built-in donor compute renderer (1x only).
- CoreGS 2.0 **replaces gt4-gs.exe on Windows** and is the **base for Android and iOS** (iOS forbids extra
  processes and GPL code in the app).
- Targets, in order:
  1. **1x pixel parity** with the donor renderer (CoreGS 1.1) on GT4 scenes, by the chapter 14 gates;
  2. **upscaling 2x-8x** (chapter 06);
  3. **texture replacement** (chapter 11: own format first, PCSX2-pack compatibility later);
  4. **60 FPS at 100% game speed** with smooth frame times (chapter 15 M17).

## 2. Clean-room rules (decision 0070, GSDX-PLAN E.3, spec README)

**Never open, search, list or copy from** (also not through search results, diffs or logs):

| Forbidden | Where |
|---|---|
| PCSX2 sources, PCSX2 shaders, GSdx-derived code | `C:\Antigravity\gt4-staticrecomp\private\tooling\pcsx2-*` (every folder with that prefix) |
| PCSX2 reference dumps | `C:\Antigravity\gt4-staticrecomp\private\slice189-pcsx2-ref` |
| Our PCSX2 GS fork (gt4-gs.exe source) | `C:\Antigravity\gt4recomp-gs`, github `moesuito/gt4recomp-gs` |
| ARMSX2 (GPL) | `C:\Antigravity\armsx2` |
| gt4-gs debug dumps and logs | any gt4-gs dump/log folder, e.g. `private\gt4gs-*-work`, `Documents\PCSX2`, `gs\` logs |
| PCSX2-derived studies | `docs/GSDX-STUDY.md`, `docs/POSTFX-TEXTURES-STUDY.md`, `docs/PARALLEL-GS-PLAN.md` (deliberately left off this branch; they exist on old branches) |
| Any other GPL GS renderer | paraLLEl-GS is LGPL and dropped (0070): do not reintroduce it |

The main checkout sits next to these folders: never `dir`/`ls`/`rg` the whole `private\` tree or `C:\Antigravity`
without excluding them.

**Allowed inputs:**
- `docs/hw-renderer-spec/` (TASK-026 specification): **the only design source.** Where it is silent or
  ambiguous, record the question in `IMPLEMENTATION-NOTES.md` and ask the owner (who relays it to the
  specification team). Never fill a gap from memory of other emulators.
- Our MIT code and prose docs in this repository; the MIT donor runtime
  `C:\Antigravity\gt4-staticrecomp\private\references\ps2-web-recomp` (Copyright 2026 Samir Nuri; its swizzle
  tables may be reused with attribution); the gt4-decomp clone
  `C:\Antigravity\gt4-staticrecomp\private\slice151-research\gt4-decomp`; public hardware documentation (GS
  User's Manual knowledge, ps2tek, ps2sdk headers); the Vulkan and GLSL specifications; the permissive libraries
  listed in chapter 12.
- GT4 GS captures recorded by our own game client (section 3.6).
- **gt4-gs.exe only as a black box:** you may run it and compare its **final frame images** with ours. Never its
  dumps, logs, settings internals or source.

**Every HWR source file** carries the MIT header and the line "Written from the TASK-026 specification
(docs/hw-renderer-spec)". Commit messages name the chapter sections they implement. Names in code are our own.

**Other `AGENTS.md` rules that apply to you:** no JIT ever; our code is MIT; the game executable never contains or
links GPL code; game data (ISO, BIOS, ELF, cards, captures, frames, binaries) stays under `private/` and is never
staged; never edit an existing `docs/inputs/*.json` pin or an existing `private/slice*` folder; never touch the
owner's cards (`private/manual-play/card.ps2` and backups) — headless runs use a **copy** of a seed card; **never
launch the game windowed** unless the owner asks you to in the session; never launch while an owner session runs;
human-readable code, no host UB for guest semantics; label claims Confirmed / High confidence / Hypothesis /
Unknown. Commits:

```
git -c user.email=57041838+moesuito@users.noreply.github.com -c user.name="João Alano" commit -m "..."
```

Never rewrite history. Push the branch; open no PR and merge nothing without explicit owner approval.

## 3. Where things are

### 3.1 Specification (`docs/hw-renderer-spec/`)

- `README.md` (index, labels, clean-room rules), chapters `00`..`14`:
  00 overview and data-authority rule, 01 GS hardware model, 02 front end and batching, 03 draw translation,
  04 texture cache, 05 render targets and depth, 06 upscaling, 07 CLUT, 08 blending/DATE/alpha test,
  09 shuffles and GT4 effects, 10 readbacks and sync, 11 texture replacement, 12 integration, 13 output and
  post-fx, 14 GT4 specifics and validation (scenes S-01..S-11, oracle, metrics, gates, vectors).
- `15-implementation-plan.md`: milestones **M0..M19** with acceptance checks (15.2), dependency graph (15.3),
  risks (15.4), open owner questions Q-01.. (15.5).
- `IMPLEMENTATION-NOTES.md`: our interpretations awaiting confirmation (IN-xxx). Add new ones there; never
  silently decide a Hypothesis.

### 3.2 Code under `native/hw_gs/`

| Folder / file | What | Milestone |
|---|---|---|
| `hw_gs_backend.{hpp,cpp}`, `hw_gs_vulkan_loader.*`, `hw_gs_dispatch.*` | Backend interface of chapter 12.3 (Vulkan-free header), device, feature report, swapchain, black 16:9 present. GS data path is still a stub (gif bytes counted, FIFO reads zero-filled, `scanout` returns false). | M0 |
| `hw_gs_options.*`, `hw_options_fixture.cpp` | `GT4_HW_*` options of chapter 12.8 | M0 |
| `hw_gs_stream_sink.cpp` | **New on this branch**: the game hookup (3.4) | M0 |
| `hw_gs_smoke.cpp`, `hw_stream_sink_fixture.cpp`, `hw_present_rect_fixture.cpp` | backend and hookup fixtures | M0 |
| `memory/` | swizzle/unswizzle all PSMs, SLM, transfers, reinterpretation maps | M2 |
| `frontend/` | GIF parser, register file, vertex queue, batches, flush rules, CLUT unit | M3 |
| `clut/` | CLUT load/decode | M3 |
| `pixel/` | exact pixel arithmetic (TFX, fog, alpha test, all blend modes, COLCLAMP, dither, FBMASK, DATE) | M5 |
| `cpuref/` | **CPU reference renderer** composing memory+clut+frontend+pixel+display; `hw_cpuref_replay` tool | M4-M6 (CPU) |
| `display/` | PCRTC rectangles, merge, interlace/deinterlace, present rectangle, shadeboost/scanlines | M6, M15 |
| `cache/` | surface-cache bookkeeping with an authority fuzz | M7 core |
| `decide/` | per-draw decisions (blend classes, alpha test methods, colour wrap, ...) | M5-M11 |
| `lod/` | mipmap/LOD reference | M13 |
| `replace/` | texture-replacement keys (XXH3), names, pack loading (stb_image PNG) | M16, M18 core |
| `shaders/` | GLSL compiled by the pinned glslangValidator 16.2.0 (Vulkan SDK 1.4.341.1); `hw_pixel_gpu_fixture` proves the GPU pixel shaders **bit-exact** with the CPU reference | M4 toolchain |

All pure libraries also build standalone (`cmake -S native/hw_gs/<lib> -B <dir>`); in the game tree they are added
by `native/hw_gs/CMakeLists.txt` (cpuref brings memory, clut, frontend, pixel, display).

### 3.3 Tools and Python tests

- `native/hw_gs/cpuref/hw_cpuref_replay.cpp`: `hw_cpuref_replay <capture> <out-dir>` replays a capture through
  the CPU reference and writes `frame_NNNNN.ppm` per frame.
- `scripts/hw_capture.py` (reader/validator of the old `GT4_PGS_CAPTURE` record format, see 3.6),
  `scripts/hw_compare.py` (pct_bad, max_delta, PSNR per frame, chapter 14.4 gates, JSON report),
  `tools/hw_replay/hw_replay.py` (backend-neutral replay driver; Python recording backend only).
- Python tests: `tests/python/test_hw_capture.py`, `tests/python/test_hw_replay_compare.py`.

### 3.4 GS front end and integration points (chapter 12, `docs/GS-STREAM-PROTOCOL.md`)

`main` has one GS seam, made for gt4-gs.exe (TASK-035): the derived donor `gs.cpp` (pinned port
`private/slice200-port`, made by `scripts/prepare_external_vulkan.py --gs-stream-hooks`) and the runtime capsule
`private/slice191-gs-stream-runtime` (`gif.cpp`, `hw.cpp`) call the hooks of
`native/gs_stream/gs_stream_client.hpp` on the EE thread: GIF chunks at the render point (after the MTVU journal
drain), privileged writes in stream order, VSYNC with the field bit, GS/GIF resets, SetGsCrt, local-to-host FIFO
reads, window attach/resize, finish. With `GT4_RENDERER=pcsx2` they become stream records for gt4-gs.exe.

**On this branch, with `GT4_HW_GS=ON`,** the client also accepts `GT4_RENDERER=hw` and calls the same hooks on the
in-process sink `native/gs_stream/gs_stream_sink.hpp`, implemented by `native/hw_gs/hw_gs_stream_sink.cpp` (start
backend, `gif_transfer`, `write_privileged`, `read_transfer_fifo`, VSYNC = flush + present (window) or
`next_frame` (headless), resets, window). The donor GS front end and Vulkan renderer are not created; no GS process
starts. `GT4_GS_CAPTURE` is refused in hw mode for now. The old M0b gs.cpp splice (`hw_gs_frontend.inc`,
`renderer_dispatch.inc`) was dropped with paraLLEl-GS; **the port and capsules stay the pinned slice214 ones**, so
the default renderer is byte-identical. Consequences you must handle:
- The donor `display()`/`copy_frame()` return false in a non-donor mode, so **headless `--frames-dir` dumps are
  empty with `GT4_RENDERER=hw`**. M6 needs HWR frame dumps (`GT4_HW_DUMP_SOURCE=vram|scanout`, chapter 12.9): write
  them from the sink/backend (e.g. at VSYNC), not through the donor.
- The game executable already links the Vulkan loader; `gt4_hw_backend` loads entry points at run time through
  `hw_gs_vulkan_loader`.
- `docs/GS-STREAM-PROTOCOL.md` defines the stream semantics (record order, VSYNC field rule 6.1, readbacks,
  `.gscap` v2 file). Use it as the contract of the hooks.
- The donor GS front end (`private/references/ps2-web-recomp/runtime/src/gs/`) is the 1x oracle (MIT).

### 3.5 Build and test commands (exact for this worktree)

VS x64 environment: `cmd /c` with
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64`.
Vulkan SDK: `VULKAN_SDK=C:\VulkanSDK\1.4.341.1`. Build scripts for slice216 are in
`C:\Antigravity\gt4-staticrecomp\private\slice216-scripts\` (`build-game-216.bat`, `ctest-216.bat`,
`run-headless-216.sh`); copy them to a new slice folder for each candidate.

Configure (the slice214 recipe plus `GT4_HW_GS=ON` and a C compiler for `replace/`):

```
cmake -S C:\Antigravity\gt4-wt-027-coregs2 -B C:\Antigravity\gt4-staticrecomp\private\sliceNNN-coregs2-build\build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DGT4_EE_PROFILE=coverage ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
  -DGT4_SDL3_ROOT=C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18 ^
  -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice191-gs-stream-runtime ^
  -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port ^
  -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders ^
  -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus ^
  -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen ^
  -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 ^
  -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin ^
  -DGT4_MEMCARD_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice200-memcard-runtime ^
  -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice206-license-guard-runtime ^
  -DGT4_RELEASE_DISTRIBUTION=ON -DGT4_HW_GS=ON
cmake --build <build dir> --parallel 4
```

- CTest (all, serial): `ctest --test-dir <build dir> --output-on-failure`. One test: `-R hw_stream_sink`.
  HWR groups: `hw_*` (backend, options, smoke, sink), `cpuref_*`, `pixel_*`, `display_*`, `memory_*`, `m3_*`,
  cache/decide/lod tests, `hw_replace_*`, `hw_gpu_*` (shader bit-exactness).
- Python: `set GT4_NATIVE_BUILD=<build dir>` then
  `C:\Antigravity\gt4-staticrecomp\private\tooling-venv\Scripts\python.exe -m unittest discover -s tests/python`
  from the worktree root.
- Expected today: everything passes except the two known failures listed in section 4.
- Headless game run (4,200 frames, 12 presses, frame dump every 100, copy of the widescreen seed card
  `private/slice113-video/widescreen-seed/card.ps2`): `bash run-headless-216.sh <name> <card> unset [hw]` after
  pointing `EXE` and the smoke folder at your slice. `SDL3.dll` must sit beside `gt4vulkan.exe` (the build copies
  it; if it is missing, copy it from `private/tooling/sdl3-3.4.18`). Compare donor dumps with `private/slice214-scripts/sha-h214-default.txt`.
- CPU reference replay of a capture:
  `<build dir>\native\hw_gs\cpuref\hw_cpuref_replay.exe <capture> <out-dir>`, then
  `python scripts/hw_compare.py <donor-frames> <out-dir>`.

### 3.6 Captures (game data, never staged)

- `C:\Antigravity\gt4-staticrecomp\private\slice175-capture\menu-0-1100.gscap` (518 MB, frames 0-1100 of the boot
  and menus, **old `GT4_PGS_CAPTURE` record format**: `'G'` GIF, `'P'` privileged, `'R'` FIFO read, `'V'` frame
  end; see `scripts/hw_capture.py`). Earlier results next to it (`cpuref-menu`, `compare-menu.json`).
- `C:\Antigravity\gt4-staticrecomp\private\slice173-synthetic\synthetic_menu.gscap` (synthetic, old format).
- **New captures** come from `main`'s client: `GT4_GS_CAPTURE=<file>` (optionally `GT4_GS_CAPTURE_FRAMES=a:b`) with
  the donor renderer writes a **`.gscap` version 2** file (`docs/GS-STREAM-PROTOCOL.md`, header magic `GSSTREAM`).
  The CPU reference does not read v2 yet (section 5, step 1).

## 4. State per milestone (2026-10-21)

| Milestone | State |
|---|---|
| M0 scaffolding | **Done** for the backend, options, device and black present; **hookup redone on this branch** through the GS stream seam (`hw_stream_sink` CTest). Not done: `result.json` option logging through the launcher (no launcher mode on this branch by design), the in-game boot check with `GT4_RENDERER=hw` (black screen expected) needs an owner session. |
| M1 tools | Python reader/compare/driver done; native replay is `hw_cpuref_replay` (old format only). |
| M2 memory | Done (CTest `memory_*`). |
| M3 front end, CLUT | Done (CTest `m3_*`). |
| M4 reference I | CPU reference done and replays real captures; **GPU write-back path WIP, not built, not tested**: branch `task/027-m4-gpuref`, tip `e31fcc4` (adds `native/hw_gs/writeback/`, shader updates, backend routing, and an edit of the dropped `hw_gs_frontend.inc` that must move into `hw_gs_stream_sink.cpp`). Not merged here. |
| M5 reference II | Exact arithmetic done on CPU (`pixel_*`) and on GPU (`hw_gpu_*` bit-exact). Not wired into a renderer yet. |
| M6 output | Display library done (`display_*`); open: **IN-315** (two-row vertical offset on menu frames 800-1100, field phase rule not derived) and **IN-316** (frame 900 garage texture wrong over 64x64 tiles; see IN-320/321/322 for the negative results). No HWR frame dumps in game (3.4). |
| M7 cache | Bookkeeping core done (`cache` tests, authority fuzz); no GPU targets. |
| M9-M11, M13 | Decision and LOD references done as pure libraries; not wired. |
| M16/M18 | Replacement keys and pack loading core done; Q-07 PCSX2-dump check pending (test skips). |
| M8, M12, M14, M15, M17, M19 | Not started. |

**In game today:** `GT4_RENDERER=hw` starts the HWR backend, which receives every GIF chunk, privileged write and
VSYNC and presents **black** (window) or counts frames (headless). Nothing is drawn yet.

**Checks at handover (slice216, `docs/BUILD.md` slice 216):** default renderer 42/42 headless dumps identical to
slice214; `GT4_RENDERER=hw` runs 4,200 headless frames (2.5 GB of GIF data and 20,987 privileged writes reach the
backend, 0 faults, no dumps). CTest 179: 176 pass, `hw_replace_q07_pcsx2_dumps` skips (Q-07), and two fail:
- `hw_gpu_pixel_directed`: 8 of 1,048,576 TEXA cases. The CPU pixel reference took the IN-07 AEM rule (A bit
  first; commit 2434e1d) after the GPU shader `hw_pixel_core.glsl` was verified, and the shader was not updated.
- `python_native_contracts`: `test_make_distribution` refuses this executable because it is not the pinned slice214
  release build. Expected on an unpinned development build; leave the check as it is.

## 5. Next steps, in order

0. **Warm-up:** bring the IN-07 AEM rule into `hw_pixel_core.glsl` so `hw_gpu_pixel_directed` passes again
   (0 mismatches in every sweep), one commit naming chapter 07/IN-07.
1. **First concrete task: native `.gscap` v2 replay for the CPU reference.** Extend `cpuref_replay` (or add a
   reader beside it) so `hw_cpuref_replay` accepts `.gscap` v2 files written by `GT4_GS_CAPTURE` (keep the old
   format working); CTest with a small synthetic v2 file; then record a fresh donor capture of the menu scene and
   the S-0x scenes the spec lists (headless, seed-card copy) and re-run the menu comparison. Acceptance: the v2
   replay of the same frames produces the same PPMs as the old-format replay of `menu-0-1100.gscap` where both
   cover the same frames (or a recorded reason where they cannot).
2. **Close IN-315 and IN-316** on the CPU reference (spec 13.3/13.4 field phase; 04/02.7 texture uploads): record
   the evidence in `IMPLEMENTATION-NOTES.md`; ask the spec team where the spec is silent.
3. **M4 GPU write-back:** rebase the ideas of `task/027-m4-gpuref` (`e31fcc4`) onto this branch (cherry-pick, then
   move its front-end edit into `hw_gs_stream_sink.cpp`), build, add `writeback/tests` to CTest. Acceptance from
   15.2 M4: `hw_flat_provoking`, chapter 03.3 coverage vectors, draw-rectangle table, TFX/fog vectors through real
   shaders; S-01/S-02 replays vs donor recorded in the journal.
4. **M5:** SHADER blending (81 modes) with copy-based Cd reads, PABE, COLCLAMP, FBA, FBMASK, DATE FULL, alpha test
   methods; `hw_blend_vectors`, `hw_atest_fold`, V-01-8, V-01-9, V-08-1; S-01/S-02 meet the 2D thresholds.
5. **M6 first playable (owner test 1):** HWR frame dumps (`vram`, `scanout`) from the sink, 42 dumps from the
   headless schedule with `GT4_RENDERER=hw`, `hw_pcrtc_rects`, V-01-12; then ask the owner to look at menus and one
   race at 1x (slow is expected).
6. **M7 surface cache** (highest risk; cache mode must equal write-back mode on S-01..S-04), **M8** readbacks and
   F8 block (in-process readbacks need no round trip, section 7), **M9** owner test 2 (60 FPS at 1x), then
   M10-M19 in the order and with the acceptance of chapter 15.

## 6. Workflow

- **One writer:** you, on `task/027-coregs2`. Commit small, push often (`git push origin task/027-coregs2`).
- **One variable per candidate:** each candidate build gets new `private/sliceNNN-*` folders (next free number;
  `slice216` is this merge's check build). Never overwrite an existing slice folder.
- **Pins:** add `docs/inputs/sliceNNN-*.json` pins and a launcher mode **only when a candidate is offered to the
  owner**; do not change `--mode pcsx2` or any existing pin.
- **Tests:** every new behaviour gets a CTest fixture or a Python test; spec test vectors become CTest cases; a
  Hypothesis needs its verification test before it is relied on. Never weaken a check to make it pass.
- **Default path stays identical:** before each push that touches shared code, rerun CTest, Python and the
  headless default-renderer run (42/42 dumps equal to slice214).
- **Documentation is part of the task:** `TASKS.md` (TASK-027 row and section; **CRLF blob**: stage with
  `git -c core.autocrlf=false add TASKS.md` and check there is no `\r\r`), `docs/STATUS.md`, the daily journal
  `docs/journal/YYYY-MM-DD.md`, `docs/BUILD.md` for each slice, `IMPLEMENTATION-NOTES.md`. `README.md` (also CRLF)
  is the short public summary: touch it only for a headline result or known issue.
- **Progress, two places:**
  - `docs/hw-renderer-spec/PROGRESS.md` on the branch (milestone table, last slice, open questions, next step);
  - ai-memory page **`decisions/coregs2-implementation`** (pinned; ADR shape: context, decision, state, next step).
    A write replaces the whole page, so **read it before every rewrite**. Never store secrets or game data there.
    Hand unfinished work over with `memory_handoff_begin` when you stop mid-task.
- **Owner:** report in Brazilian Portuguese, plain language, short. Ask only for decisions (spec questions,
  Q-xx answers) and gameplay tests (windowed runs). No code review requests.

## 7. Performance context (`docs/FRAME-COST-STUDY.md`)

- The EE thread is not slow because of game code: in a headless day race at 1x, game code is about 2.9 ms of an
  8.2 ms frame; the rest is plumbing (VIF1 posting 1.2, MTVU journal/GS front end 0.5, **GS stream writes 0.8**,
  IOP 0.5, run loop 0.8, waits for the VU1 worker 1.4).
- Night races issue one 2,048-byte readback per frame (the flare visibility buffer, 64x8 RGBA): 3.3 ms of MTVU
  `download` drain plus **0.9 ms GS round trip at 1x (2.2-2.8 ms at 3x)** with gt4-gs.exe.
- **In process, CoreGS 2.0 removes the stream writes and the IPC round trip**: a readback becomes a direct read of
  our own GPU result (chapter 10); the MTVU drain stays and is addressed by the study's ranked plan, not by you.
- Budget: 16.7 ms per frame at 60 FPS, with the GPU work overlapping the EE. Measure with F7/F8 only when no build
  runs in parallel.

## 8. Known GT4 effects to watch (chapters 09 and 14)

- **Night flare readback**: LOCAL->HOST of a 64x8 RGBA rectangle every night frame; must be exact (chapter 10).
- **Shuffles**: texture and channel shuffles (8/16-bit reinterpretation, chapter 09.1-09.3, 07.7 GPU CLUT from
  targets) used by Polyphony's brightness and fade patterns (09.4); verify with `GT4_HW_VERIFY_SHUFFLE`.
- **Page alternation / double buffers**: front/back buffers at different FBP, double-half and striped double
  clears (05.8, 09), display lookup of the right target (05.10).
- **480i fields**: the game boots and keeps menus interlaced (480i, field mode); races run 480p by default in this
  build line. Field phase from the VSYNC field bit (GS-STREAM-PROTOCOL 6.1); IN-315 is exactly this problem.
- Reference scene for the owner: Time Trial, Nordschleife, Toyota Supra Wood One, 480p.

## 9. Branch facts

- `task/027-coregs2` = `origin/main` (db99f9e, release 1.0.0, slice214) + `origin/task/027-hw-renderer` (80c87a2)
  + the docs checkpoint `origin/task/027-cpuref` (03bdd0d). paraLLEl-GS code, its pins (slice163-175) and launcher
  modes were not taken (decision 0070). `task/027-m4-gpuref` (e31fcc4) is **not merged** (section 5, step 3).
- Check build: `private/slice216-coregs2-build` (see `docs/BUILD.md`, "Slice216").
