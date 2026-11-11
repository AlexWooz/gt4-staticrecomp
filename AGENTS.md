# AGENTS.md

This file guides every AI coding agent (Claude Code, Codex, OpenCode and others) working in this repository.
`CLAUDE.md` only points here.

## Local workflow: `private/OWNER.md` (if it exists)

Each person working on this repository may keep their own local agent instructions in `private/OWNER.md`: memory tools,
notifications, machines and how they like the work organised. `private/` is git-ignored, so the file is never committed;
contributors who want their own workflow write their own. **If `private/OWNER.md` exists, read it at the start of every
session and follow it** together with this file (in a git worktree, which has no `private/` folder, read it from the main
checkout). This file holds the project's rules; `OWNER.md` adds the local ones.

## Read first

The ordered task list is `TASKS.md`. Current state is at the top of
`docs/STATUS.md`; technical background for the optimization work is in
`docs/OPTIMIZATION-PLAN.md`; daily progress goes to
`docs/journal/YYYY-MM-DD.md`. `README.md`, `docs/ARCHITECTURE.md` and
`docs/BUILD.md` give the public overview, architecture and build recipes per
slice. Older slice-by-slice history lives in STATUS (History section) and the
journals. Work from before the ps2-web-recomp base (the project's earlier own
recompiler) lives only in the private repository `moesuito/gt4-staticrecomp-legacy`;
do not reintroduce it here.

## Mission and direction (owner, 2026-10-15)

GT4Recomp is a static recompilation of Gran Turismo 4 NTSC-U (SCUS-97328, disc
labeled v2.00; its CORE.GT4 is byte-identical to the gt4-decomp target) for
Windows x86-64 + Vulkan first, then native Linux and Android (phases below). **No JIT ever**: guest code
(EE, VU, IOP) is compiled ahead of time per target; interpreters may remain only
as explicit, counted fallbacks. GPU driver shader compilation is allowed. Never
claim zero emulation or full PS2 fidelity.

- **Phase 1 (done, release 1.0.0; 1.x updates):** fast, stable recompiled port: 60 FPS
  at 100% game speed, smooth frametime, no interpreter on the normal path.
- **Phase 2 (current):** CoreGS 2.0 (TASK-027); when it is ready, release 2.0.0 with
  CoreGS 2.0 as the default renderer and GSdx as the fallback.
- **Out of scope here:** a decompilation-based source port, changes to the game's
  physics timing and remaster features will be a separate project, possibly a fork.
  This repository's goal is the original game running natively, optimized and as
  faithful as possible.
- **Game patches: improvements that leave gameplay and progression alone are in scope**
  (owner, 2026-11-04). The original game stays, with built-in options on top.
  - **In scope.** Examples:
    - the 360-degree chase camera on the right stick (TASK-159);
    - real-time FoV adjustment (TASK-160);
    - ultrawide and other screen ratios (TASK-079, TASK-080);
    - control comfort such as the full trigger range (TASK-161).
  - **Out of scope:** anything that changes how the game plays or what it rewards. That covers physics, AI, race timing,
    car data, progression and saves.
  - **How a patch is made.** Community PCSX2 patches and the owner's own ideas are rebuilt as our own hooks in the translated
    code (decisions/game-patches-scope in ai-memory). Third-party patch code is a reference only, never copied.
  - **What each patch needs.** A switch, CTest fixtures and identity evidence:
    - with the switch off, the game is identical to the reference;
    - with it on, the race clock is identical and the simulation differs only by the patch's own state;
    - a control patch changes only how input becomes the car's controls.
  - The owner decides each patch's default.
- **Modern render pipeline is in scope as an option** (owner, 2026-10-22): the render
  pipeline is modernised in stages. CoreGS 2.0 optimises as far as possible inside the
  VU1 + GS model. **CoreGS 3.0** then moves the VU1 work onto the GPU and renders
  natively: it may first run the whole game with some effects missing or approximate,
  and restores them one by one in a native way (for example flares through occlusion
  queries instead of GS readbacks). Rules:
  - **Faithful by default:** a faithful renderer (GSdx today, CoreGS 2.0 later) stays the
    default and the reference; the modern pipeline is opt-in until it beats it in owner
    tests.
  - **Fidelity is a measured target there, not a bit-exact gate:** per-scene visual
    checks (night, rain, rally, replays, photo, split screen) and a list of effects to
    restore. CoreGS 2.0 keeps its 1x identity gates where practical and relaxes one only
    explicitly, for a named optimisation.
  - **Game logic never changes:** physics, timing, AI, saves, and anything the EE or IOP
    reads back that affects gameplay stay exact (decisions/coregs3-modern-pipeline in
    ai-memory).
- **Aspect ratios are in scope** (owner, 2026-10-21; TASK-079, TASK-080): 16:9 forced
  at boot, and native support for other screen ratios (16:10, phones, 21:9, 32:9):
  the renderer stretches to the screen, the game's own projection values (Width and
  Aspect) and the HUD/UI layout are corrected from the screen ratio, parameterised
  rather than fixed per ratio. Third-party PCSX2 patches are references only, never
  copied.
  Presenting more than 60 FPS while the game's fixed 1/60 s physics stays
  untouched is in scope (TASK-045 on the PCSX2 GS, after 1.0.0; TASK-031 study;
  owner, 2026-10-15).
- **Renderer focus now:** TASK-032..044 (PCSX2 GS in a separate process, decision
  0070; CoreGS 1.1 built in as the fallback).
- **Phases (owner, 2026-10-22):** Phase 1 = Windows PC, 1.x (done); Phase 2 = CoreGS 2.0
  and release 2.0.0 (current); Phase 3 = native Linux; Phase 4 = Android (only when the
  owner asks; the focus is PC until then). Phases 3 and 4 start after 2.0.0.
- **Graphics:** PC renderers = the **PCSX2 GS** in a separate GPL process
  (`gt4-gs.exe`, built from our fork `moesuito/gt4recomp-gs`, downloaded by the
  launcher) plus **CoreGS 1.1** built in (MIT, fallback and offline default), with
  **CoreGS 2.0** in development (our own MIT hardware GS renderer, TASK-026 clean-room
  spec, TASK-027 implementation; 3x-6x and texture packs; it enters the launcher only
  when at least equivalent to GSdx, a beta needs upscaling and anti-aliasing), then
  **CoreGS 3.0** (TASK-090, the modern pipeline above). Linux and Android use
  **CoreGS 2.0** (the ARMSX2 GS pipeline is no longer the mobile plan). See
  `docs/GS-RENDERER-OPTIONS.md` and `docs/GSDX-PLAN.md`.
- **New installs (owner, 2026-11-02; decision 0074):** setup checks the PC and uses CoreGS 2.0 when it runs it, with no download;
  GSdx is optional, downloaded when chosen or when the card cannot run CoreGS 2.0. A player already on GSdx stays on it.
- **Disc images (owner, 2026-11-02; decision 0075):** the launcher links the image where the player keeps it and never copies it; an install from before keeps its copy.
- **No GPL code in the release executable** (it contains game code): the game
  executable never contains or links GPL code; the PCSX2 GS lives only in the
  separate `gt4-gs.exe` (decision 0070, which limits 0066, 0067 and 0069). PCSX2 is
  a reference only. TASK-027 implementation agents must never open PCSX2 sources
  or the fork (clean room). Never import PCSX2 JITs.
- **Clean room, PCSX2-tainted agents (decision 0070, GSDX-PLAN E.3).** Agents that
  read PCSX2 sources, the `gt4recomp-gs` fork, `private/tooling/pcsx2-*` or
  `private/slice189-pcsx2-ref` are *PCSX2-tainted*. Tainted agents never work on
  TASK-027, never edit `native/hw_gs/` or `task/027-*` branches, and never answer
  TASK-027 questions except through the TASK-026 specification team in prose.
  TASK-027 agents never open those sources or the fork; they may use `gt4-gs.exe`
  final frame images only, as a black-box reference (never its debug dumps or
  logs). The task brief states the agent's status.
- **Distribution (decision 0069):** the installer ships the prebuilt game
  executable; the user supplies their own disc. Our code is MIT (`LICENSE`).
- The launcher (TASK-019) and racing wheels (TASK-018) are planned tasks;
  LAN/online launcher and in-game settings menus are recorded ideas.
- **Spec II is a second target** (owner, 2026-10-22;
  `docs/decisions/0072-spec2-second-target.md`): the SCUS-97436 Public Beta
  lineage ISO with its own target executable, not a content merge into v2.00.
  Recorded, not scheduled; Phase 2 stays the priority.

Working fork: github.com/moesuito/gt4-staticrecomp. Upstream
github.com/Kaezzey/gt4-staticrecomp is read-only.

## Workflow

- Speak Brazilian Portuguese with the owner, in plain lay language. Keep all
  repository docs, reports, prompts and code comments in en-US.
- The main agent orchestrates, reviews and tests; implementation and research go
  to subagents (Haiku by default; escalate to Opus or do it directly when a task
  stalls). Research runs in parallel and read-only; **exactly one repository
  writer at a time**. Review every subagent claim: rerun CTest/Python and check
  pins before committing.
- One variable per candidate build; keep A/B switches (`GT4_PACING=audio`,
  `GT4_GS_SYNC=1`, `--vu1-interpret-only`, `--vu1-verify N`). Attack whatever the
  F8 report shows as the largest cost.
- **Clean build caches after every task** (owner, 2026-11-11): after a task is merged or a
  worktree is finished, delete its compiled build directories (CMake caches, objects,
  binaries) in every worktree and never let them accumulate. Keep the pinned artifacts the
  gates need (the v5 EE library, the reference executable), the docs and the `evidence-*`
  files; check free space before a build round.
- Work is organized in `TASKS.md` (TASK-001, TASK-002, ...). Keep it current:
  states, branch, acceptance, and new tasks appended in order.
- **Task lifecycle:** each task gets its own branch from `main`
  (`task/NNN-short-name`) → implementation by the single writer → automated
  checks (CTest, Python, headless frame comparison, verify modes) → in-game test
  by the owner → **explicit owner approval** → PR into `main` and merge
  (`gh pr create` / `gh pr merge --merge`) → next task on a new branch. Never
  merge a task the owner has not approved. Commits and pushes are authorized;
  never rewrite history.
- Update STATUS and the journal as part of each task.
- **Every PR updates `TASKS.md`** in the PR itself to mark the task or tasks it
  completes (state, result, date), so `main` never lags behind what was merged.
- **`README.md` is a short public summary** (owner, 2026-10-17): header badges,
  where we are, performance, known issues, roadmap phases, credits and legal. It
  lists no tasks. A PR touches it only when one of those changes (a badge, a
  headline result, a known issue).
- Ask the owner only for decisions and gameplay tests, not code review.

## Releases and release notes (owner, 2026-10-23)

- **Version numbers:**
  - a big feature gets a minor version (1.x.0);
  - intermediate updates and hotfixes get the patch number (1.x.y);
  - the plan is in ai-memory `decisions/release-plan-1x`.
- **Every release has two notes files, written before the build** (the launcher does not build without its own):
  - **Full notes:** `docs/release-notes/<version>.md`.
    - The GitHub release body: complete and technical.
    - Covers install or update steps, every change, fixes, known issues, requirements and credits.
  - **Launcher notes:** `docs/release-notes/launcher/<version>.md`, shown in the launcher's "What's new" card (the
    launcher embeds every launcher note up to its own version).
    - Short and plain, for a player who is not technical; readable in a few seconds.
    - Format: a `# GT4Recomp <version>` title, then one bold sentence that sums up the release.
    - Then a few bullets under `## New`, `## Fixes`, `## Performance` or `## Good to know`, in that spirit:
      new features first, then fixes.
    - No internal names (task numbers, slices, files, environment variables, renderer internals) and no long
      explanations.
    - Give a measured number whenever there is one (for example "about 15% less processor work per frame"), taken
      from a real measurement and never invented.
    - About 5-10 bullets at most.
    - **Two languages** (owner, 2026-10-23): the en-US note above, plus the same note in Brazilian Portuguese in
      `docs/release-notes/launcher/pt-BR/<version>.md`. Headings there: Novidades, Correções, Desempenho, Bom saber,
      Agradecimentos. The launcher shows the note in its interface language and falls back to en-US when the pt-BR
      file is missing; write both for every release.
- **Release steps:**
  - Bump the version in `launcher/Cargo.toml`, `Cargo.lock` (workspace crates only), `tauri.conf.json` and the
    installer default.
  - **PGO only for published game builds** (owner, 2026-11-03):
    - profile-guided optimization (PGO, with ThinLTO) adds about 30 minutes for both games, so only a build meant to
      be published gets it: an rc (uploaded as a draft, a pre-release on the owner's word) or a final release;
    - a build made only to test a change (internal, on the owner's desktop or laptop) is a plain Release build; tell
      the owner it is plain, since it is a little heavier on the processor.
  - Pin new game builds in new `docs/inputs` files.
  - Build the launcher with `launcher/tools/build-release.ps1` and the installer with makensis.
  - Check the installer contents against their sources.
  - **Language check before every release, rc included** (owner, 2026-10-23). en-US and pt-BR are the two default
    languages and both must be right:
    - the locale files `launcher/crates/gt4l-tauri/ui/locales/en-US.json` and `pt-BR.json` have the same keys and the
      same `{placeholders}`;
    - no pt-BR value is left in English, except names and terms kept on purpose (Saves, Replays, Start, Select);
    - the focus checks pass in both languages (`dev/focus-check.html?lang=en-US` and `?lang=pt-BR`);
    - both launcher notes exist for the version, with the same bullets.
  - Then the release channels below decide what is published and when.

### Release channels (owner, 2026-10-23)

- **Stable channel:** final releases, tag `vX.Y.Z`, marked Latest on GitHub. Launchers on Stable (the default) only
  see these.
- **Beta channel:** release candidates, GitHub **pre-releases** tagged `vX.Y.Z-rcN` (rc1, rc2, …). The build carries
  its label (`build-release.ps1 -Label rcN`, makensis `/DLABEL=rcN`, `make_portable_zip.py --version X.Y.Z-rcN`), so
  the launcher reports `X.Y.Z-rcN` and the assets are named `GT4Recomp-Setup-X.Y.Z-rcN.exe` and
  `GT4Recomp-X.Y.Z-rcN-portable.zip`. Never reuse the final tag for an rc.
- **The launcher** (Settings > General, from 1.3.0-rc5) offers Stable or Beta and a version list read from GitHub:
  Beta lists rcs by date (plus a newer stable on top), Stable lists finals by SemVer. Drafts are never listed or
  offered (the public API does not even return them), and versions older than the picker minimum are hidden. Going to
  an older version shows a strong downgrade warning; a newer version, from either channel, is always plain.
- **Every new build goes up as a GitHub draft** (owner, 2026-10-23): after the automated checks and the language check,
  upload it as a **draft** release (`gh release create <tag> --draft`, with the label, the assets and the full notes
  plus a test-build warning). A draft is private: neither the public nor any launcher sees it, and the owner can
  download it on other PCs.
  - **Replacing a draft:** a small fix keeps the same rc number. Rebuild with the same label and replace the draft's
    assets (`gh release upload <tag> <files> --clobber`, and update `SHA256SUMS`), or delete and recreate the draft.
    Only a more substantial change gets a new rc number. Never replace the assets of a published pre-release or
    release.
- **The agent never publishes on its own:**
  - **Release candidate:** it stays a draft until the owner says, in words, that it can become a pre-release; then
    `gh release edit <tag> --draft=false --prerelease --latest=false`.
  - **Final release:** only when the owner says, in words, that a given rc is stable and should be rebuilt as final.
    Then rebuild without a label, publish `vX.Y.Z` as Latest with the full notes as the body, the installer, the
    portable zip and `SHA256SUMS`, open and merge the PR (owner approval), update the README badge and download name,
    and STATUS.
  - **Withdrawn rcs:** an rc replaced by a newer one is turned into a **draft** (`gh release edit <tag> --draft=true`)
    when the owner asks, so nobody downloads it and no launcher sees it.

## Testing the game

- While the owner is awake, the owner plays and reports: F7 starts an interval,
  F8 writes the report to `private/manual-play/session-*/probe.log` (one interval
  per scene).
- When left unattended, the agent may launch candidates and drive the game via
  Computer Use. Keys: arrows = D-pad, Z = Cross/accelerate, A = Square/brake,
  S = Triangle, X = Circle, Enter = Start.
- Reference scene: Time Trial, Nordschleife, Toyota Supra Wood One, in-game video
  480p (the game boots 480i; menus stay interlaced).
- Preserve the owner's card (`private/manual-play/card.ps2`), backups and locks;
  never launch while an owner session is running; close the game normally.

## Commands

All native work runs from the repo root in a VS x64 environment (`cmd /c` with
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64`;
see `native/build.cmd`). Toolchain: clang-cl (C++20), Ninja, CMake 3.24+. Run
builds and tests serially.

- Configure a candidate build (current recipe = slice 153; every new candidate
  uses fresh `private/sliceNNN-*` dirs and new `docs/inputs/*.json` pins):
  ```
  cmake -S . -B private\slice153-pacing-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
    -DGT4_EE_PROFILE=production -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
    -DGT4_PYTHON=private/tooling-venv/Scripts/python.exe ^
    -DGT4_PERF_RUNTIME_SOURCE=private/slice151-vu1-aot-runtime ^
    -DGT4_VU1_GENERATED_DIR=private/slice151-vu1-gen-smoke ^
    -DGT4_PORT_DIR=private/slice153-pacing-port -DGT4_SHADER_DIR=private/slice153-pacing-shaders
  ```
  CMake refuses port/shader/runtime combinations that do not match the pins.
- Rebuild an existing dir: `cmake --build private\slice153-pacing-build\build`
- Baseline (slice 114) build: `cmd /c native\build.cmd`
- Tests: `ctest --test-dir <build dir> --output-on-failure`; one test:
  `ctest --test-dir <build dir> -R native_pacing --output-on-failure`
- Python suite (must point at the build under test, otherwise it checks the stale
  slice 114 binaries and two tests fail):
  `GT4_NATIVE_BUILD=<build dir> private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python`;
  one module: `... -m unittest discover -s tests/python -p test_pacing_launcher.py`
- Launch: `private\tooling-venv\Scripts\python.exe scripts\play_performance.py --assets-root C:\Antigravity\gt4-staticrecomp --mode pacing`
  (modes map to pinned manifests: `vu1-aot`, `gs-async`, `pacing`, `vu1-record`;
  options `--vu1-verify N`, `--vu1-interpret-only`; env `GT4_PACING=audio`,
  `GT4_GS_SYNC=1`, `GT4_PRESENT_MODE=fifo|mailbox|immediate`).
- VU1 emitter: `gt4_vu_recomp <corpus dir> <fresh out dir>` (built from
  `tools/vu_recomp`; refuses non-empty output dirs).

## Architecture (big picture)

- **EE pipeline:** ISO → `CORE.GT4` → reconstructed ELF
  (`private/reconstructed/SCUS_973.28.elf`) → donor translator (`tools/ee_producer`,
  `scripts/ee_producer.py`) → 326 C++ units (`unit_XXXX.cpp`, one per 16 KiB
  address window, `switch(entry)` to labels; VU0 macro ops inline) → pinned private
  `ps2game.lib`. A call to an address without a translated entry halts ("no
  translated code"); there is no EE interpreter. The build therefore needs the
  pinned private library and is not an end-to-end clean-checkout build.
- **Donor runtime:** MIT ps2-web-recomp at `1b4808793a12c45125262fd2d5f50a9ec9f7c020`
  (`private/references/ps2-web-recomp`), frozen and hash-checked. Behavior changes
  go through overlay capsules generated by `scripts/prepare_perf_runtime.py`
  (e.g. `--vu1-aot` applies `scripts/vu1_aot_overlay.inc` to `vu_micro.cpp`) and
  selected by `GT4_PERF_RUNTIME_SOURCE`. IOP: system libraries are native HLE, the
  23 game IRX modules are interpreted (`docs/IOP-MAP.md`); VU0 micro is
  interpreted (`docs/VU0-MICROCODE-MAP.md`).
- **Native adapter** (`native/`): the donor GS backend `gpu.cpp` is spliced with
  `native/gpu_*.inc` by `scripts/prepare_external_vulkan.py` into a port dir;
  `native_vulkan.cpp` (Vulkan context, 3-slot async submit ring, present),
  `window.cpp` (Win32, VBlank hook, F-keys), `host_io.cpp` (WinMM audio, XInput,
  keyboard), `pacing.cpp`/`frame_clock.cpp` (single-clock pacing, audio rate
  control), `perf_probe.*`/`perf_accounting.hpp` (F7/F8 categories). Shaders are
  the donor WGSL compiled to SPIR-V by `tools/wgsl_spirv` (naga).
- **VU1 AOT:** the whole VU1 microcode image is XOR-encrypted in the ELF (file
  offset 0x525DD0, `docs/VU1-MICROCODE-SOURCE.md`); `tools/vu_recomp` emits C++ per
  (key, entry) with identity digests, a budget/resume contract
  (`vu_recomp_contract.hpp`) and differential tests against the donor interpreter.
  Unseen entries fall back to the interpreter and are counted in F8.
- **Launchers and pins:** `scripts/play_performance.py` → `play_external_runtime.py`
  → `external_runtime_probe.py`, which verifies every file against
  `docs/inputs/*.json`, scrubs inherited `PS2_*`/`GT4_*` variables, backs up and
  locks the card, and records the run in `result.json`.
- **Research references** (private clones): gt4-decomp with names/types at
  identical addresses (`private/slice151-research/gt4-decomp`), Nenkai repos,
  PCSX2 (`private/tooling/pcsx2-slice101-source`).

## Code and evidence

All code and tooling must be human readable: descriptive names, straightforward
flow, explicit guest addresses and fixed-width state, defined wrapping, no host
undefined behavior standing in for guest semantics. New behavior needs automated
controls in CTest and Python fixtures. Never silently accept unsupported
instructions or services; never weaken acceptance checks to make tests pass.
Label claims Confirmed / High confidence / Hypothesis / Unknown and record hashes,
tool versions, addresses/offsets, method and limits.

**The game window's responsiveness timer stays** (owner, 2026-11-03; TASK-145, decision 0076). It lives in
`native/window_heartbeat.hpp` (from the 1.3.0 line).
- Why: without it, Windows marks the window "not responding" after 5 seconds without input (a `PeekMessage` on an empty
  queue does not count). In borderless fullscreen on NVIDIA RTX PCs the desktop then flashes every 6 seconds, and after
  Alt+Tab the game can come back as a white window.
- Never remove it, slow it past one second, or filter the game's message loop. Do it only after the laptop test in
  decision 0076 passes on an RTX PC.

## Data, pins and documentation

- ISO/BIOS/CORE/ELF, translated code, cards, captures, outputs and binaries stay
  ignored under `private/`, `generated/`, `build/` or ignored ISO paths. Never stage.
- Verify inputs against `docs/inputs/`; verification never updates pins.
- Git worktrees for parallel tasks must not link `private/` (no junctions or
  symlinks to the main checkout's `private/`, which holds the owner's cards and
  saves): pass `-DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp` or copy the
  few files CMake needs. Before removing a worktree, check `dir /AL` and remove
  any junction with plain `rmdir <link>` first; never delete recursively through it. Never
  overwrite an existing `private/slice*` directory or edit an existing manifest
  to make a hash match; create a new slice.
- After a rebuild the executable hash changes; the launcher manifest pin must be
  regenerated (Python pin tests enforce it).
- This machine (Ryzen 7 5700X, RX 9060 XT) has an AMD driver reporting
  `maxComputeWorkGroupCount[0] = UINT32_MAX`; fixtures must derive limits from the device.
- Documentation is part of completion: update STATUS when state changes, append
  the daily journal, add numbered decisions (`docs/decisions/`) as useful, keep
  README progress and known issues current. Before ending: `git status --short`,
  commit and push.
