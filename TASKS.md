# GT4Recomp Tasks

Single ordered task list. Each task has its own branch, automated checks, an
in-game test by the owner and an explicit owner approval before its PR is merged
into `main` (workflow rules in [AGENTS.md](AGENTS.md)). Evidence for finished
tasks lives in `docs/journal/` and `docs/inputs/`; background studies are linked
from [STATUS](docs/STATUS.md).

States: ✅ Done · 🧪 Waiting for owner test · 🔄 In progress · ⏭️ Next · 📋 Planned

Tasks are numbered in execution order (renumbered on 2026-10-15). Older
journals and commits use the previous numbers: 018→010, 010→011, 031→012,
014→013, 015→014, 016→015, 017→016, 020→017, 022→018, 023→019, 026→020,
013→021, 024→022, 027→023, 025→024, 028→025. Second renumbering (same day): 022→017, 021→018,
020→019, 019→020, phase 2/3 tasks 022-025→021-024; the in-game 16:9 task was
dropped (the owner's game already boots in 16:9). Earlier IDs 011, 012, 019, 029
were merged into the new 010/012; the pre-donor cleanup was done outside the
list (private repository `moesuito/gt4-staticrecomp-legacy`). Third renumbering
(2026-10-15, owner): 018→017 (license crash), 019→018 (wheels), 020→019
(launcher), 017→020 (decomp names, last in phase 1, after the first stable release).

## Phases

| Phase | Platform | Goal |
|---|---|---|
| **Phase 1** (current) | Windows PC | Original game running natively: correct speed, smooth 60 FPS, no interpreter on the normal path, PCSX2 GS renderer in a separate process (decision 0070), CoreGS 1.1 fallback |
| **Phase 2** | Android | ARM64 build with the ARMSX2 mobile GS pipeline (Vulkan) |
| **Phase 3** | iOS | ARM64 build with the ARMSX2 Metal GS pipeline |

**Renderer decision (2026-10-15, [decision 0070](docs/decisions/0070-pcsx2-gs-separate-process.md)):**
PC uses the **PCSX2 GS** in a separate GPL process, `gt4-gs.exe`, built from our fork
`moesuito/gt4recomp-gs` and downloaded by the launcher (TASK-032..044). **CoreGS 1.1** (our MIT
Vulkan compute renderer) is built in as the fallback and offline default. **CoreGS 2.0** (our MIT
hardware renderer, clean room from the TASK-026 specification; TASK-027) is planned and paused. paraLLEl-GS
is dropped from the release (0070 supersedes the renderer options of 0069). The game executable never
contains GPL code. Mobile uses the **ARMSX2 GS pipeline** (Vulkan on Android, Metal on iOS) without its JIT.
See [GS renderer options](docs/GS-RENDERER-OPTIONS.md) and [GSDX-PLAN](docs/GSDX-PLAN.md).

## Phase 1 — Windows PC

| Task | Goal | State |
|---|---|---|
| TASK‑001 | Rebuild the slice 149 baseline on this machine | ✅ Done |
| TASK‑002 | Owner-launched VU1 program recording mode | ✅ Done |
| TASK‑003 | Hardened VU1 AOT emitter with differential tests | ✅ Done |
| TASK‑004 | VU1 microcode compiled ahead of time in the game | ✅ Done |
| TASK‑005 | Asynchronous GPU submission + overlay (RTSS) fix | ✅ Done |
| TASK‑006 | Frame pacing on a single clock, rate-controlled audio | ✅ Done |
| TASK‑007 | VU1 per-program flag elision | ✅ Done |
| TASK‑008 | Full static VU1 entry coverage (all 2,048 entries) | ✅ Done |
| TASK‑009 | Quick fixes: title, fixed 16:9, fullscreen, interval counters, races in 480p | ✅ Done |
| TASK‑010 | Build pipeline: faster builds and build tuning (parallel, cache, lld-link, fp-contract off, x86-64-v3 variant, LTO, PGO) | ✅ Done (tuning measurements pending) |
| TASK‑011 | MTVU: VU1 on its own thread, on by default | ✅ Done · in `--mode main` (slice178), `GT4_MTVU=0` A/B |
| TASK‑012 | paraLLEl-GS as an optional PC renderer | ❌ Dropped (decision 0070): LGPL code not shipped. Branch `task/012-parallel-gs` kept as history; MIT seam and capture code salvaged into TASK-035 |
| TASK‑013 | VU0 microcode compiled ahead of time | ✅ Done |
| TASK‑014 | EE entry coverage from the decomp function list | ✅ Done (2026-10-21) · 1,105 new entries (+3,728 dispatch ids), slice208 `pcsx2-coverage`; headless 42/42 identical; owner play-through approved |
| TASK‑015 | LWR sign-extension fidelity check | ✅ Done (already correct; no change) |
| TASK‑016 | IOP per-module cost counters and review | ⏸️ Deferred (not needed for 1.0.0) |
| TASK‑017 | License-test crash | ✅ Done (2026-10-16) · license demo guard on both demo loaders (slice206, `--mode pcsx2`): licenses playable, demos that decrypt wrong are skipped as in PCSX2 default; root cause moved to TASK‑063 |
| TASK‑018 | Native Driving Force Pro (DFP): 900-degree steering, game-generated force feedback bridged to SDL3, wheel configuration | 📋 Planned for 1.3.1; TASK-123 research brief ready; TASK-121 host-input groundwork is partial |
| TASK‑019 | Launcher 1.0.0 (Rust core + Tauri 2 HTML frontend) and NSIS per-user installer | ✅ Launcher merged into main (2026-10-21): setup with disc copy, gt4-gs download, settings, saves, controls (SDL3), tray, updates; owner tested end to end. Still for 1.0.0: NSIS installer build and the v1.0.0 release |
| TASK‑020 | Decomp names in generated EE code (waits for more decompilation progress) | 📋 Planned |
| TASK‑025 | Save manager: memory card slots, import/export | ✅ Code done, merged into `task/019-launcher`; ships with the launcher (owner test pending) |
| TASK‑026 | CoreGS 2.0, part 1: clean-room specification of the PCSX2 hardware renderer | ✅ Done (docs/hw-renderer-spec, PR #17) |
| TASK‑027 | CoreGS 2.0, part 2: MIT implementation from the specification (3x-6x, texture packs) | 🧪 Candidate 332 on `task/027-coregs2` (2026-10-30): CPU survey and fronts (process CPU -28%, wall -15% at 3x against 313), direct GS intake (P6); specialised pipelines, performance mode started (decision 0073); 1.3.0-rc7 merged in as the base (CoreGS 2.0 is not part of 1.3.0); fine-tuning, then a PR to the EE optimisation branch |
| TASK‑028 | CoreGS 1.1: upscaling, interlaced-only deinterlace, FXAA-class AA, FSR 1.0 | ⏸️ Paused by the owner: 3x shows native-size aliasing on 3D models; stays the built-in fallback renderer (`task/028-donor-upscale`, latest slice186) |
| TASK‑029 | Controller and keyboard mapping (binding table, XInput LT/RT as L2/R2 pressure, controls file for the launcher) | ✅ Done (2026-10-21) · merged with the launcher; owner tested remapping in game |
| TASK‑030 | Night-race readback wait (CoreGS 1.1) | ⏸️ Paused: owner test 59.07 FPS, wait unchanged at 4.3 ms because the page is drawn the same frame (`task/030-night-readback`) |
| TASK‑031 | High frame rate (120-144 FPS) with game physics untouched | ❌ Dropped in this form; redone as TASK-045 on the PCSX2 GS (study docs on `task/031-high-fps`) |
| TASK‑032 | Decision 0070 docs: PCSX2 GS in a separate process, clean-room wording, renderer docs | ✅ Done (PR #24) |
| TASK‑033 | PCSX2 GS fork (`moesuito/gt4recomp-gs`): reproducible build and CI releases | ✅ Done · standalone repo, CI green, prerelease `gt4gs-v0.1.0-poc` (docs/GS-FORK.md) |
| TASK‑034 | Proof of concept: replay our `.gscap` capture through the PCSX2 GS and compare images | ✅ Done · owner approved the images on 2026-10-16; gate passed (docs/GS-PCSX2-POC.md) |
| TASK‑035 | MIT GS stream protocol, shared-memory IPC and renderer seam on main | ✅ Done · slice191: CTest 29/29, Python 321 OK, donor 42/42, stream converts to the PoC dump byte for byte (docs/GS-STREAM-PROTOCOL.md) |
| TASK‑036 | Live `gt4-gs.exe` MVP: the game drives the PCSX2 GS over IPC | ✅ Done · owner test 2026-10-16 at 3x (sharp, fullscreen OK): Nordschleife 59.98 FPS, night race with 5 AI 59.89 FPS, readback wait 0.93 ms; `--mode pcsx2` holds 480p by default |
| TASK‑037 | PCSX2 GS readbacks and the night race | ✅ Done · owner test 2026-10-16: night race 59.97 FPS, 69 late frames (was 213); accurate readbacks are the default, unsynchronized optional (drops headlight flares) |
| TASK‑038 | GS window, fullscreen, focus, input and crash handling | ✅ Done · owner test 2026-10-16: fullscreen, Alt+Tab and the click-in-picture focus fix (gt4-gs 0.3.1); crash grace frames and CoreGS 1.1 restart offer |
| TASK‑039 | GS settings table and `gt4-gs.ini` | ✅ Done (main-repo part): 51-row table drives `gt4-gs.ini`; allow-list enforced by gt4-gs 1.0.0 (slice201: ignored with a warning) |
| TASK‑040 | Texture dumping and replacement packs | ➡️ Folded into TASK-050 (mod manager, release 1.1.0) |
| TASK‑041 | GS process performance: F8 `[gs-ipc]`, tuning, owner A/B | ✅ Done · owner test 2026-10-16: night race 59.97 FPS, 53 late frames (was 69), EE critical path 11.8 of 16.7 ms (headroom 4.9 ms); GS wake batching and ordered privileged writes |
| TASK‑042 | JIT-free `gt4-gs.exe` (no recompilers, no RWX, no SW JIT); required before 1.0.0 | ✅ Done · owner test 2026-10-16 (menus, night race): gt4-gs 0.4.0 JIT-free, enforced by Arbitrary Code Guard; images identical to 0.3.2 |
| TASK‑043 | Launcher: download, pin and update the GS renderer | ✅ Done (2026-10-21) · the launcher installs gt4-gs 1.0.1 from the GitHub release at setup; owner tested |
| TASK‑075 | Input layer on SDL3 (game side in slice210: SDL3 3.4.18 gamepads mapped to the DS2 layout, controls.toml applied, GT4_INPUT=sdl default / xinput for one release) | ✅ Done (2026-10-21) · game (slice210) and launcher read pads through SDL3 3.4.18; owner tested and approved |
| TASK‑076 | Modern fixed keyboard layout (several keys per DS2 button, table in docs/CONTROLS.md `keyboard-default`) | ✅ Done (2026-10-21) · fixed keyboard layout in game and launcher, P = Start (owner choice); owner tested |
| TASK‑044 | Packaging, release notes and the release 1.0.0 plan update | ✅ Done (2026-10-17) · release 1.0.0 ships gt4-gs 1.0.1; fork release notes, README, THIRD_PARTY and RELEASE-1.0.0-PLAN updated (docs only) |
| TASK‑045 | High frame rate on the PCSX2 GS (TASK-031 redone) | 📋 Planned (after 1.0.0) |
| TASK‑046 | Launcher "Prepare shaders": compile GT4's PCSX2 GS pipeline list headless (gt4-gs --warm-selectors) and keep the cache warm | 📋 Planned (study done, 2026-10-16) |
| TASK‑047 | PCSX2 GS deinterlacing in the 480i menus (field and interlace state in the stream) | ✅ Done · owner test 2026-10-16: 480i menus clean on Automatic; root cause missing SYNCV and a one-field-early VSYNC field (protocol 1.1, gt4-gs 0.3.2) |
| TASK‑048 | Mod manager in the launcher (umbrella): texture packs first, Content Manager style | 📋 Planned for 1.1.0 (umbrella; owner decisions 2026-10-16) |
| TASK‑049 | GS settings curation for the launcher: Graphics and Advanced pages, reset to defaults, 12x, widescreen finding | ✅ Done · decisions 2026-10-16; default gt4-gs.ini unchanged; 58-row table ([settings](docs/GS-SETTINGS.md)) |
| TASK‑050 | Texture replacement contract and headless dump/replace round trip (absorbs TASK-040) | 📋 Planned for 1.1.0 |
| TASK‑051 | `gt4mods` core: manifest, pack detection, validation, import, store | 📋 Planned for 1.1.0 |
| TASK‑052 | Mods active view (hardlinks), priority, conflicts, uninstall, `gt4mods` CLI | 📋 Planned for 1.1.0 |
| TASK‑053 | GS settings and dev launcher wiring for mods (texture rows off Graphics, `[Folders] Textures`) | 📋 Planned for 1.1.0 |
| TASK‑054 | Launcher Mods page (mockup, then Tauri) | 📋 Planned for 1.1.0 |
| TASK‑055 | Mod creator tools: dump workspace, package as mod | 📋 Planned for 1.1.0 |
| TASK‑056 | Live texture reload in gt4-gs | 📋 Planned for 1.1.0 |
| TASK‑057 | Study: other mod kinds (GS presets, VOL overlay, data patches) | 📋 Planned for 1.1.0 |
| TASK‑058 | Crash-safe memory card in the real build: atomic writer, dirty retry, exit flush (GS-loss path too) | ✅ Done · owner test 2026-10-16; kill test 25/25; card flush on exit and after a GS loss (slice200; in slice202) |
| TASK‑059 | Distribution executable: installed-folder shaders and GS package, no repo paths, `--portable-check`, `scripts/make_distribution.py` | ✅ Done · owner test 2026-10-16; distribution folder smoke 42/42 and 41/41 (slice200; slice202: also from `private/slice202-dist`) |
| TASK‑060 | Mouse cursor auto-hide over the picture (fullscreen and windowed, game and gt4-gs agree) | ✅ Done · owner test 2026-10-16 (fullscreen and windowed); slice201, in slice202 |
| TASK‑061 | "Compiling shaders…" notice in gt4-gs (real compiles only) | ✅ Done · owner test 2026-10-16; gt4-gs 1.0.0 (cold cache 238 compiles shown, warm cache none) |
| TASK‑062 | Performance overlay (FPS, frame time, CPU/GPU of the game, resolutions, frame cost and headroom) | ✅ Done · owner test 2026-10-16; overlay off by default, +0.35 us/frame EE, +0.02 ms/frame GS thread |
| TASK‑063 | Bit-exact R5900 FPU `div.s`/`mul.s` (license demo phase, real-PS2 card replays and ghosts) | 📋 Planned · oracle ready (`scripts/license_demo_oracle.py`); needs hardware vectors |
| TASK‑064 | Third-party overlays (MSI Afterburner / RivaTuner) with the PCSX2 GS: Arbitrary Code Guard off by default | ✅ Done (2026-10-17) · owner test: the RivaTuner/Afterburner overlay shows in game with default settings |
| TASK‑077 | Context-aware input mapping (race vs menus, e.g. Esc = Start in a race, Triangle in menus) | 📋 Open, study done (docs/CONTEXT-INPUT-STUDY.md); not in 1.0.0 |
| TASK‑078 | Graphics settings inside GT4's own Options menu (output resolution 480p to 8K replacing the 480i/480p/1080i row, fullscreen, anti-aliasing); game fixed at 480p | 📋 Planned (after 1.0.0), study done (docs/INGAME-SETTINGS-STUDY.md) |
| TASK‑079 | Force 16:9 at boot: `GT4_FORCE_WIDESCREEN` holds the game's own Wide flag at 16:9 (slice211); launcher setting "Widescreen 16:9", default on | ✅ Done (2026-10-21) · slice211, GT4_FORCE_WIDESCREEN default on, launcher "Widescreen 16:9"; owner tested (boots 16:9 and 480p; the 100% save loads). The "Load failed!" seen on the way was the launcher card bug, fixed in TASK-019 |
| TASK‑080 | Native ultrawide and other aspect ratios (16:10, phones, 21:9, 32:9): screen-ratio detection, automatic Width/Aspect for the 3D, HUD/UI fix, renderer stretch | ✅ Done 2026-10-21 (P0, P1, P2 and the seamless-display addendum, owner approved; release 1.1.0) · P3 progressive menus open with TASK-078 |
| TASK‑081 | Release 1.0.0 hardening and installer: release game build without build paths (slice213), launcher without user paths, CPU (SSE4.1) and Vulkan checks before setup and Play, game Vulkan baseline path for older drivers (slice214), WebView2 bootstrapper in the installer, installer asset name `GT4Recomp-Setup-X.Y.Z.exe`, release notes and draft release | ✅ Done (2026-10-21) · owner tested and approved the installer; released as v1.0.0 |
| TASK‑082 | Native variable frame rate: fixed 60 Hz simulation, render at any rate (render skip below 60, extra render-only passes above 60 with the game's own render time t) | Deferred by owner (2026-10-26); preserved at task/082-render-skip15b7bf3 for CoreGS2.0; excluded from rc7 (TASK-133) |
| TASK‑083 | Faster loading and saves: instant disc reads (deeper than PCSX2 Fast CDVD) and loading/save speed-ups in our runtime and game code; gameplay untouched | 📋 Future study (owner, 2026-10-21) |
| TASK‑084 | Frame cost optimisation: cut the emulation-side EE overhead (VIF1 chain jobs, GIF fast path, VBlank journal) | ✅ Done (2026-10-22) · owner test approved, night race critical path 11.8 → 10.0 ms; release 1.2.2 (PR #82); flare readback moved to CoreGS 2.0 (TASK-087) |
| TASK‑085 | Launcher self-update: the setup starts outside the launcher's kill-on-close job | ✅ Done (2026-10-21) · PR #67; v1.1.0 republished with the fix; launchers 1.0.0 update by running the setup |
| TASK‑086 | Progressive 480p everywhere (menus and races at 480p through ELF patches) | ⏸️ Shelved (owner, 2026-10-21) · owner test broke the car select and the race load; branch `task/086-progressive-480p` kept |
| TASK‑087 | GS readbacks without stalls, in CoreGS 2.0 (GPU-driven flare visibility, no EE wait) | 📋 Planned (owner, 2026-10-21) · study done (docs/GS-READBACK-STUDY.md); CoreGS 2.0 only, gt4-gs frozen |
| TASK‑088 | Release 1.2.0: custom textures (gt4-gs texture replacements from `Documents\Gran Turismo 4\Textures`, two launcher options), 2-row bottom crop, black window from the first frame, monitor names, desktop resolution, every gt4-gs value enabled, GSdx setup choice and switch | ✅ Done (2026-10-22) · owner tested and approved; released as GT4Recomp v1.2.0 with gt4-gs 1.2.0 (fork PR #9, tag `gt4gs-v1.2.0`) |
| TASK‑089 | Respect CoreGS 1.1 selection during setup and automatic renderer maintenance (contribution by JulioHeroZ, PR #72) | ✅ Done in release 1.2.0 (merged into `task/088-custom-textures-120` with a merge commit; PR #72 closes when 1.2.0 lands on main); owner tested in 1.2.0; superseded by TASK‑096 in 1.2.3 |
| TASK‑090 | CoreGS 3.0: modern render pipeline (VU1 work on the GPU, native effects restored one by one), opt-in; faithful renderer stays the default | 📋 Planned (owner, 2026-10-22) · after CoreGS 2.0; VU1 study in progress |
| TASK‑091 | Release 1.2.1: per-machine install with data beside the program, user data in %APPDATA%, portable folder, silent and safe self-update, WebView2 checks (issue #80), Performance Mode, home screen layout | ✅ Done (2026-10-22) · owner approved; released as v1.2.1 |
| TASK‑095 | Ultrawide: undistorted 2D overlay on the pre-race menu (race lobby), as TASK-080 P2 does for the race HUD | 📋 Planned (owner, 2026-10-22) · study first; idea only |
| TASK‑097 | Self-sufficient game executable: defaults built in (GSdx, 480p, 16:9 and ultrawide, fullscreen), paths found from the exe folder, settings read from the launcher's config file; environment variables only as test overrides | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test · release 1.3.0 (moved from 1.2.4) |
| TASK‑098 | Portable build: a second launcher build shipped as a zip, user data in `user\` and large data in `data\` beside the exe, no registry or system folders, self-update by zip swap | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test · release 1.3.0 (moved from 1.2.4) |
| TASK‑099 | Launcher home: patch notes panel on the right (glassmorphism card), Markdown release notes of the installed version embedded in the build | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner visual check · release 1.3.0 (moved from 1.2.4) |
| TASK‑100 | Spec II support: SCUS-97436 as a second target (decision 0072); subtasks S1-S9 in docs/SPEC2-PLAN.md | 🟡 Owner test passed (2026-10-23): Spec II menus, Options and license tests; license demo and installer test pending · release 1.3.0 |
| TASK‑101 | Multi-game launcher: Gran Turismo 4 and Spec II side by side (game selector, Play/Install, per-game disc, card and saves, shared settings, Spec II logo) | 🟡 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 with TASK-100 |
| TASK‑102 | Launcher language: automatic en-US / pt-BR with a General > Language setting (locale files, Rust messages, release notes) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑103 | Graphics API choice for GSdx: Vulkan (recommended, default), Direct3D 12 (recommended), Direct3D 11 (older PCs), OpenGL later; hardware backends only, in Settings > Graphics (locale files, same ids kept apart from the engine Renderer) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner in-game test pending · release 1.3.0 |
| TASK‑104 | Launcher saves: crash-proof PCSX2 card import (issue #86: superblock layout for 8 to 64 MB, bounded directory walk, import under a panic guard), New save, Export of the active save, Rename (display name, 40 characters) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑105 | Integrated GPUs: 1x default and a plain GPU-lost message (issue #89) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑106 | CHD disc images: accept `.chd` at setup and decompress to the launcher's own ISO image (issue #90) | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑107 | Automatic graphics setup per GPU: automatic Graphics API, GPU generation table, step down after a lost GPU, game without Vulkan when GSdx draws, OpenGL in gt4-gs | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑108 | Launcher: automatic Graphics API (Vulkan, else Direct3D 11) and graphics card (Adapter), no Vulkan requirement; game programs shipped as gt4.exe and gt4-spec2.exe | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑109 | Launcher update channels: Stable (default) or Test builds (rc pre-releases) | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑110 | Controller tuning: stick and trigger deadzone, sensitivity, response curve, saturation, pressure, inversion, vibration, live test view | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑111 | Logs for bug reports: one-click "Export logs" zip (latest log, recent launcher and game logs, settings, system summary; user name and paths masked), offered after a crash or a lost card | 🔧 Implemented on `task/101-multi-game` (rc4, 2026-10-23), owner test pending · release 1.3.0 |
| TASK‑112 | Launcher logs: one file per session with INFO, WARN and ERROR lines, the last 20 kept, `latest.log` with the earlier errors, game session folders capped at 20, Open latest log | 🔧 Implemented on `task/101-multi-game` (rc4, 2026-10-23), owner test pending · release 1.3.0 |
| TASK‑113 | Spec II race HUD at ultrawide ratios (wider than 16:9): speed figure and odometer digits stretched and spilling past the gauge (known issue in 1.3.0) | 📋 Planned (owner, 2026-10-23) · next update |
| TASK‑115 | Game recognition and Discord: Windows Game Mode/Game Bar, AMD and NVIDIA capture (Instant Replay, ShadowPlay) and Discord Rich Presence | 📋 Planned (owner, 2026-10-24) · future (1.3.1 or later). Discord Rich Presence portion implemented on branch task/133-rc7-ra-controls (2026-10-27); Game Bar/GPU recognition remains future. |
| TASK‑116 | George V Paris below 60 FPS: one GS readback per frame makes the game wait for VU1 and GSdx; 1.3.1 = no VU1 drain when the readback returns nothing, plus unsynchronized readbacks in gt4-gs 1.2.2 | 🔧 rc6 step 1 implemented and tested headless (2026-10-25); owner test pending; step 2 stays 1.3.1 |
| TASK‑117 | Performance mode, part 2: fewer light flares at night (no halo, distance cull, light caps), our own clean implementation in GT4 v2.00, off when Performance mode is off | ⚠ rc6 ELF, library and host block built and tested (2026-10-25), NOT shipped: the switch-off identity gate failed (journal 2026-10-25) |
| TASK‑118 | Graphics settings simplification: Performance Mode becomes a real preset (blending Basic, trilinear/dithering off, preloading partial), the Advanced page loses the cosmetic rows, RC-only settings move to an RC Settings area, GS async and deinterlace become fixed | 🔧 Planned (2026-10-25, owner), 1.3.0-rc6 |
| TASK‑093 | Custom textures straight in the Textures folder: gt4-gs 1.2.1 reads the folder itself; the launcher stops creating `replacements` and moves an old `replacements` folder's contents up once gt4-gs 1.2.1 is in use | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑096 | Remove CoreGS 1.1 from the launcher (owner, 2026-10-22): GSdx only, CoreGS 2.0 shown as "Coming soon" (disabled), a stored CoreGS 1.1 choice migrates to GSdx, a GSdx failure shows a clear error instead of falling back; CoreGS 1.1 stays in the game executable, unused by the launcher. For launcher release 1.2.3 | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑092 | MTVU test switches after TASK-084: `GT4_MTVU=0` and `GT4_MTVU_VERIFY=1` in pcsx2 mode, portable doorbell ordering | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑094 | Desktop shortcut: finish-page checkbox (on by default), all-users desktop `Gran Turismo 4.lnk`, choice kept in HKLM `DesktopShortcut`, silent updates create it for older installs and restore it when chosen | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑134 | Game-code optimization study: CPU time and energy outside the GS renderer, ordered plan for the i5-7200U laptop (docs/GAME-CODE-OPTIMIZATION-STUDY.md) | 🔄 Study done and owner decisions recorded (2026-10-28) · next: P0 baselines when the owner frees the desktop |
| TASK‑135 | Performance HUD for CoreGS 2.0, in the spirit of the macOS Metal HUD: FPS, frame time, CPU and GPU of the game, memory and a live frame time graph | 🧪 Built, waits for the owner's test (branch `task/135-coregs2-performance-hud`, candidate 378, on the notebook, 2026-11-02) |
| TASK‑136 | Simple onboarding: a new install uses CoreGS 2.0 when the PC runs it, GSdx becomes optional and is downloaded only when the card needs it | 🧪 Built, waits for the owner's test (branch `task/136-simple-onboarding`, 2026-11-02) |
| 🧩 | **gt4-gs 1.0.0, the first stable renderer release** (with TASK‑060/061/062; settings allow-list enforced) | ✅ Published 2026-10-16 (`gt4gs-v1.0.0`, zip sha256 `2e878019...`) |
| 🚀 | **First stable release (1.0.0) on GitHub** (after TASK‑044, TASK‑043 and the owner's end-to-end test) | 📋 Planned |

## Phase 2 — Android

| Task | Goal | State |
|---|---|---|
| TASK‑021 | Mobile GS: ARMSX2 GS pipeline (Vulkan) behind our GS front end | 📋 Planned |
| TASK‑022 | Android app: ARM64 build, host layers, touch controls, Bluetooth controllers | 📋 Planned |

## Phase 3 — iOS

| Task | Goal | State |
|---|---|---|
| TASK‑023 | ARMSX2 Metal GS backend | 📋 Planned |
| TASK‑024 | iOS app: ARM64 build, signing, touch and controller input | 📋 Planned |

## Done

### TASK-001 — Rebuild the slice 149 baseline on this machine
Rebuilt the measured diagnostic candidate on the Ryzen 7 5700X / RX 9060 XT
machine; fixed a fixture that assumed every GPU limits workgroup counts below
`UINT32_MAX`. Commit `8b65693`.

### TASK-002 — VU1 program recording mode
`--mode vu1-record` captured 86 VU1 programs in two owner sessions. Commit `8702d90`.

### TASK-003 — Hardened VU1 AOT emitter
`tools/vu_recomp`: exact inputs, fresh outputs, identity digests, budget/resume
contract, differential tests. Commit `96c71f4`.

### TASK-004 — VU1 AOT in the game (slice 151)
Race frame 33.3 ms → 22.0 ms; 140,034 in-game verifications, 0 mismatches.
Commit `2d21abb`.

### TASK-005 — Asynchronous GPU submission (slice 152)
GS wait 9.5 ms → 0.8 ms; race at 100% speed; RTSS overlay visible on AMD.
Commit `56819b0`.

### TASK-006 — Frame pacing (slice 153)
Flat 16.7 ms frametime (99.6% of intervals in 16-17 ms). Commit `fe51478`.

### TASK-007 — VU1 flag elision (slice 154)
Showroom 44.8 → 59.1 FPS; 5-AI race at 60.0. Commit `26e211d`, merged in PR #1.

### TASK-008 — Full static VU1 entry coverage (slice 157)
All 2,048 entries of the decrypted VU1 image compiled. Owner test on an idle
CPU: night race 58.9 FPS (remaining cost is the 4.1 ms GS fence wait, for
TASK-012), rally 59.9, showroom 60.0; 0 interpreted VU1 calls in all three.

### TASK-009 — Quick fixes (slice 164)
Title "GT4Recomp", fixed 16:9, Alt+Enter/F11 borderless fullscreen, interval
counters, `late_frames` fixed. `--force-480p` holds the game's race display
option (index at `[0x622F4C]+0x38CB0+0x16B0`) at 480p: owner confirmed races
render 480p while menus stay 480i as in the original.

### TASK-010 — Build pipeline (slice 159)
Full build 3,942 → about 1,700 CPU-seconds: shared corpus library, split
generated tables, `-Wno-unused-label` on generated code. `GT4_FP_CONTRACT_OFF`
and `GT4_X86_64_V3` options exist (default off); their speed effect is Unknown
until an idle-CPU bench. ThinLTO/PGO not attempted.

### TASK-013 — VU0 microcode AOT (slice 160; main build slice 165)
All 14 VCALLMS entries compiled with exact MAC/status/clip (the EE reads them via
CFC2) and exact `run(0,pc)` hand-offs. 28,000-case differential vs the donor,
0 failures; headless frames identical. Owner test: 0 interpreted VU0 calls;
showroom 58.0, night 57.9, rally 60.0 VBlanks/s (performance neutral, VU0 cost
0.1-0.3 ms/frame). `--mode main` (slice 165) is the build of main with all of it.

### TASK-032 — Decision 0070 docs
Docs only, branch `docs/decision-0070-gs-process`: decision 0070 (PCSX2 GS in a separate process, owner answers recorded), `docs/GSDX-PLAN.md` with the go/no-go gate, TASK-032..045 in this file, CLAUDE.md clean-room wording, RELEASE-1.0.0-PLAN workstream W8, README renderer text, GS-RENDERER-OPTIONS note, journal 2026-10-15. No code, build or game launch.

## Phase 1 details

### TASK-011 — MTVU
Run VU1 programs on a worker thread (donor `start_vu_thread` exists), on by
default with an env switch to disable; exact sync points wherever the EE observes
VU1 results. Acceptance: identical headless frames, verify 0 mismatches, owner
showroom/night/race frametime equal or better.
State: ✅ Done (release 1.0.0 W1, branch `task/011-mtvu`). Plan in docs/MTVU-PLAN.md; step 0 (slice169 measurement), steps 1-4 (ring and journal, slice170 worker, slice171 per-thread F8, slice172 EE-side signals) and step 6 (`--cpu-cores N`) are in. Release W1 (slice178): MTVU is part of `--mode main`, on by default (`GT4_MTVU=0` is the A/B switch), and `--mode main` holds 480p by default. Checks: CTest 27/27, Python 281 OK (12 skipped), headless 4,200 frames: 42/42 identical to slice157 with MTVU on (three runs) and off (one run), and with the default environment. The PR into `main` is not opened yet (ready for review); the merge waits for owner approval. Owner A/B (MTVU-PLAN section 12.6) still pending. Details: BUILD.md "Slice178".

### TASK-012 — paraLLEl-GS as the PC renderer
Dropped (decision 0070): not shipped; LGPL code is not merged. Branch `task/012-parallel-gs` is kept as history; its MIT renderer seam and capture code are salvaged in TASK-035. The text below is the original plan.
One large task, delivered in verified steps on its own branch:
1. Import paraLLEl-GS (github.com/Arntzen-Software/parallel-gs, LGPL-3.0+, with
   Granite MIT) pinned by commit; build it in our CMake; bridge its Vulkan device
   to ours (Granite device shim or port).
2. Adapter from our GS front end (`write`, `write_packed`, `write_image`,
   `read_image`, privileged registers, signals, vsync) to its interface
   (`gif_transfer`, `write_register`, `map_vram_*`, `read_transfer_fifo`,
   `vsync` → scanout). Renderer selected by env (later the launcher); the donor
   renderer stays as experimental fallback.
3. GT4 validation: boot, menus, races, night, rally, showroom; image comparison
   against the donor renderer at 1x; channel-shuffle brightness effects, lens
   flares, half-pixel issues.
4. Options: super-sampling upscaling 2x-16x, deinterlacing/field options for the
   interlaced menus, page-tracked readbacks against the night-race wait.
5. Extras where paraLLEl-GS allows: post-processing (sharpening, FXAA, filters),
   dithering/texture options, texture dump/replacement feasibility.
Replaces the former donor-renderer upscaling, readback-wait and renderer-feature tasks.

### TASK-014 — EE entry coverage
Add translator entries for decomp functions without one, starting with the 15
referenced ones (docs/DECOMP-INTEGRATION.md).

State (2026-10-20, branch task/014-ee-coverage-b, Phase 1 done, not merged): static entry ledger
(scripts/ee_entry_ledger.py, docs/EE-ENTRY-LEDGER.md). Translated entries (case labels): 135,806. New
candidates: 1,122 (high 1,076, medium 39, low 6, rejected 1), 246 unit windows. Decomp names untranslated: 83
of 7,740. The 16 stage A seeds are found by the strict rules (14) and a 40-word window (2). The name table at
decomp e0b70c4d6 equals the committed one. Open: the generated folder has 135,806 labels but its log says
135,093; resolve before Phase 2 (extra-entries library A/B, then producer regeneration after owner approval).
State (2026-10-20, branch task/014-ee-coverage-b, phase 2 candidate built, not merged): 1,105 accepted seeds
(scripts/ee_entries.py). Library profile coverage: 139,534 dispatch ids (+3,728), 325 of 326 units change. Built with
the portable clang-cl 23.1.2; the 8-unit object check is identical to production. CTest 33/33, Python 583 OK,
headless 42/42 identical to slice206. New mode pcsx2-coverage (docs/inputs/slice208-pcsx2.json). Owner play-through
pending; the 135,806 vs 135,093 gap is the codegen boundary entries (EE-ENTRY-LEDGER).
State (2026-10-21): ✅ Done. The owner approved the slice208 candidate after the play-through and asked to close the task (PR into main). `--mode pcsx2` stays slice206 until the default is switched to slice208 in a follow-up (new pin labelled pcsx2, distribution and launcher test folder rebuilt).
Follow-up (2026-10-21, slice209): `--mode pcsx2` runs the slice209 pin (the slice208 coverage library plus the GT4_FULLSCREEN start, for the TASK-019 launcher test). `pcsx2-coverage` and `pcsx2-slice206` stay as history modes.

### TASK-015 — LWR fidelity
State: analysis done, no runtime change (branch task/015-lwr-fidelity). The donor helper matches PCSX2:
offset 0 sign-extends, offsets 1-3 preserve the upper 32 bits. The brief's premise does not hold in code.
The lone-LWR scan found one site (0x00549178, `lwr r5, 0(r2)`). A change would need hardware evidence and
an overlay or ps2game.lib regeneration, so it is parked. Part B (silent audio test) is done.

### TASK-016 — IOP per-module counters
Per-module instruction counters and per-library HLE call counts in F8; decide
whether the sound modules need native HLE (docs/IOP-MAP.md).

### TASK-017 — License-test crash
Pre-existing; traced to `RaceSolitaire::loadDemoInputs` (0x3EAD18) →
`AutomobileControlRecord::Manager::load` (0x346BB8) reading a bad record count.
Env-gated logging there and in the PS2ZIP loader for `replay/license/` files,
plus a CDVD read trace; one owner reproduction; fix the cause.
State (2026-10-15, branch task/017-license-crash): FPU rounding lead checked. Donor mul/div/sqrt equal an exact
chop reference; add/sub truncate the smaller operand (one-ulp, magnitude-larger). Not the crash cause by itself
(Hypothesis). The failing record-size words (0xFF80FF80 / 0x80FF80FF at offset 0x14F8, every file) look like a
decode or layout error in the triage parser, not FP results (Hypothesis, to verify with the owner trace).

State (2026-10-16, branch task/017-license-crash, slice 204): halt forensic dump built, not yet run by the owner. `GT4_HALT_DUMP=1` adds `[halt-dump]` lines at the halt (every thread's registers; for a thread whose saved pc is in the record copy loop of `Manager::load` (0x346BB8..0x346EB0), its record base and count, and whether the record window overlaps any thread stack, with the stack of thread 17 at 0x008722C0 as the suspect; the halted thread's stack top). Overlay `scripts/halt_dump_overlay.inc`; capsule `private/slice204-halt-runtime`; build `private/slice204-halt-build`; pins `docs/inputs/slice204-halt-runtime.json` and `slice204-halt-trace.json`. Owner command: `play_performance.py --assets-root C:\Antigravity\gt4-staticrecomp --mode license-halt`. The FSV read trace is on in the same run. Next: decide from the dump whether the copy overruns (Hypothesis) or the record count is wrong (Unknown).

State (2026-10-16, FPU oracle phase 1): cause Confirmed by the owner test (B1, B2, B5 crash; B3, B4 load, as predicted): the demos are decrypted by GT4's FPU-keyed cipher (func_00579E00), which needs bit-exact R5900 mul.s/div.s. `scripts/license_demo_oracle.py` checks the game CRC on all 592 demos; `tests/python/test_license_demo_oracle.py`. No model passes yet (chop/chop 0/592; every model tried 0/592). A CRC-confirmed, model-free beam search recovered exact chains for 528/592 files (52,469 unique div.s steps): the hardware quotient is always chop or chop + 1 ulp; per-step agreement chop 60.9%, nearest 82.8%, radix-2 carry-save SRT candidate 93.1% (best 94.1%). mul.s is a truncated Booth array (hardware vector 1.0 x 0x3FFFFFFF = 0x3FFFFFFE) that equals chop on this data. Next: identify the divider's digit-selection details (Hypothesis: uncorrected radix-2 SRT), then phase 2 (fpu.hpp overlay, ps2game.lib rebuild, new pins).

State (2026-10-17, done): owner approved guard v1 (slice205: B1 no longer crashes, its background movie fades and the demo does not run, the license is playable, as PCSX2 default; B3's demo plays). B5 then crashed through the second loader 0x00110230 (vtbl[57], target 0x003D5F00; session-20261016-235625-870886), which v1 did not see. Guard v2 (capsule `private/slice206-license-guard-runtime`, build slice206) keys on both loaders' call sites; `--mode pcsx2` = slice206 (`docs/inputs/slice206-pcsx2.json`), `pcsx2-slice203` and `license-guard` (slice205) kept. Consumer map: [LICENSE-DEMO-CIPHER](docs/LICENSE-DEMO-CIPHER.md) (memory-card loads check the CRC themselves). PCSX2 with EE clamping Full plays B1's demo but crashes in B3 (owner): another phase coin flip. Follow-up: TASK-063.

State (2026-10-16, round 3, slice205): overflow clamping models (from the public PCSX2 "Full clamping" workaround prose) refuted: every 0x7F7FFFFF profile gives 0 sane layouts and no model passes a CRC; CRC-confirmed chains use exponent-255 states as large normals, as the donor does. Mitigation per owner decision: license demo guard in the runtime's dispatch.cpp (capsule `private/slice205-license-guard-runtime`, ps2game.lib unchanged): when a decrypted demo fails the game CRC and is not the single-Recorder layout, its record count is set to 0xFFFFFFFF so Manager::load copies nothing; one `[license-guard]` line per demo load. Build slice205 on the slice203 recipe (gt4-gs 1.0.1), mode `license-guard`, pins `docs/inputs/slice205-license-guard*.json`. Known issue: those demos and license backgrounds do not play (as in PCSX2). Waiting for the owner license test (B1-B5).

State (2026-10-16, round 2): oracle reports layout sanity per model. The record payload is past the saturation point, so it is all right or all wrong (one key-stream phase); no FPU model gives a sane layout on all files (best 270/592, US best 50/85, every model near a coin flip), so there is no 1.0 candidate among FPU models. Divider fitting: radix-2 SRT carry-save variants plateau at 94.1% of 52,469 confirmed steps; radix-4 -2..2 ruled out; exact rule still Unknown.

### TASK-063 — Bit-exact R5900 FPU div.s/mul.s
Follow-up of TASK-017. Our `ps2/fpu.hpp` is not bit-exact with the R5900 for the cipher's mul.s/div.s chain: 0 of 592
license demos pass the game CRC, so demos in the wrong key-stream phase play no demo (guarded) and real-PS2 card
replays and ghosts fail to load (cleanly). The oracle (`scripts/license_demo_oracle.py`) and 52,469 CRC-confirmed div.s
steps are ready; the divider's digit selection is unknown (best model 94.1% per step). Needs targeted hardware div.s
vectors (or a published design description). Acceptance: 592/592 CRC passes, then an fpu.hpp overlay, a ps2game.lib
rebuild with new pins, and an F8 physics check.

### TASK-064 — Third-party overlays (MSI Afterburner / RivaTuner) with the PCSX2 GS
Goal: the RivaTuner Statistics Server on-screen display (MSI Afterburner) appears in game with the default settings.
Cause (suspected, not confirmed in game): Windows Arbitrary Code Guard (ACG), which gt4-gs enabled by default
(`[GT4GS] ProhibitDynamicCode`, missing key = true). RTSS injects a hook that needs executable memory, and ACG refuses it.
Change: ACG is OFF by default. Row `prohibit_dynamic_code` (gs-settings.toml) is on the Advanced page, default `false`,
written `always`, so gt4-gs.ini carries `ProhibitDynamicCode = false` under `[GT4GS]`; the Advanced page switch turns
ACG back on. The no-JIT guarantee does not depend on ACG: gt4-gs.exe is built JIT-free (GT4GS_ONLY) and `check-nojit.ps1`
verifies every release. Tests: `ProhibitDynamicCodeTests` and the default-ini assertions in `test_gs_settings.py` and
`test_gs_live.py`. README known issue left unchanged until the owner confirms the overlay.
Acceptance: owner sees the RTSS overlay in game with the default settings.
State: ✅ Done (2026-10-17). Owner test: the MSI Afterburner / RivaTuner overlay shows in game with the default settings, confirming Arbitrary Code Guard as the cause. Branch `task/064-overlay-compat` (single writer, from main da1eb95). Python suite
544 tests OK (38 skipped for missing private files). Merged with owner approval (PR #46).

### TASK-077 — Context-aware input mapping (idea)
Different mappings by game state (race vs menus), e.g. Esc = Start (pause) in a race and Triangle in menus. Needs a
study of how to read the game state reliably. After 1.0.0; the owner has more ideas.
State (2026-10-20): open, out of the 1.0.0 scope (owner). The read-only study is in docs/CONTEXT-INPUT-STUDY.md:
a host-side VBlank read of guest RAM (the race_display pattern) can tell race classes apart by vtable and see the pause
object; replay is still unknown. Proposed first step: a logging-only probe build (S1). Depends on TASK-029 reaching
main and on TASK-075.

### TASK-078 — Graphics settings in the game's own menu
Owner idea 2026-10-20, after 1.0.0. Remove GT4's video-output row (480i / 480p / 1080i, the "HiVision" widget on
Options → Screen), keep the game fixed at 480p, and show our settings in the native Options menu: output resolution
(480p to 8K, the renderer's internal resolution), fullscreen, anti-aliasing and maybe more. Read-only study in
docs/INGAME-SETTINGS-STUDY.md. Recommended path: host-side substitution of the option menu data when the game reads
the disc (patch recipes applied to the user's own disc, never shipped game files) plus a host bridge for the script
natives in `dispatch`; a host overlay menu is the fallback. Phases: P0 hold races at 480p for any saved value; P1 hide
the old row; P2 the "Output resolution" row and a settings file the launcher merges; later fullscreen and AA rows and
live apply in gt4-gs. Estimate: 4-5 slices for P0-P2, 6-8 plus a gt4-gs release in total. Owner decisions pending:
provenance of OpenAdhoc-derived script edits, progressive menus (ELF patch) or not.

### TASK-080 — Native ultrawide and other aspect ratios
Owner request 2026-10-21. The launcher reads the monitor resolution. 4:3 and 16:9 use the game's own modes
(TASK-079 forces 16:9). For other ratios the renderer stretches to the screen and the game's projection values
(hidden Detail Monitor Settings: Width, the display width in mm that acts as FOV, default 425; Aspect, default 0.97
for 16:9) are computed from the ratio (owner data points: 3088x1440 → Width 515, Aspect 1.20; 3440x1440 → Width 573,
Aspect 1.34). The HUD, UI and text are corrected too. The SilentWarrior112 PCSX2 patches (no licence) are a
reference only, mapped to decomp functions and redone in our own parameterised way. Progressive menus overlap
TASK-078. Study first, then a phased plan.
State (2026-10-21): study done, `docs/ULTRAWIDE-NATIVE-STUDY.md` (owner asked to keep it). Findings: the patch file
targets our ELF; the hidden-menu record (Width, Aspect...) lives in the saved option object, so the plan writes no
save data. 3D: a dispatch hook on the race camera call after `RaceBase::initMonitor` (return 0x388B38) widens the
projection by s = R / (16/9) (Hor+, Vert+ below 16:9; at 16:9 frames stay byte-identical). The owner's values
scale both Width and Aspect, which is about 23-37% too wide by the game's maths: first owner test. HUD: a hook on
`RaceDisplay::render` for the canvas (320 -/+ 240 R), plus an ELF patch table and re-translation for the fixed HUD
constants. Menus, FMV and photo stay pillarboxed 16:9. gt4-gs needs "Stretch" and a per-scene control message (a
new gt4-gs release). Plan: P0 a decision superseding the 0055 lines; P1 auto 3D aspect plus a launcher row; P1b
gt4-gs stretch; P2 HUD; P3 progressive menus (TASK-078); P4 optional true-FOV mode. After 1.0.0; seven owner
decisions listed in the study.
State (2026-10-21, P0 + P1 in progress, branch `task/080-aspect-ratios`, slice212b): owner decisions taken
(geometry-correct Hor+, Vert+ below 16:9, no extra FOV by default; menus, FMV and photo pillarboxed 16:9; order P1, P1b,
P2, P3). P0: decision 0071 supersedes the 0055 sentence on game patches, memory writes and camera projection (guard
rails: no save-data writes, 16:9 byte-identical, every hook listed with tests, the SilentWarrior112 file is a reference
only). P1: `GT4_DISPLAY_ASPECT` (auto = the window client area; 16:9, 4:3, W:H, decimal; invalid refuses the start) and
the hidden `GT4_ASPECT_FOV_EXTRA` (default 1.0) for owner test T1; race camera hook at return 0x00388B38 scales the
stack matrix (x by 16/(9R) Hor+, y by 9R/16 Vert+); race view = RaceDisplay::render (0x0039F498) in the last 30 VBlanks
minus photo/movie requests; CoreGS 1.1 fill with linear filter; protocol 1.3 CONTROL display_aspect producer (only to a
consumer with READY minor >= 3); launcher row "Aspect ratio: Auto / 16:9 / 4:3 / Custom" (Auto names the monitor,
Custom ratio slider 1.00-6.00), settings schema 4; TASK-079 hold hardened (writes only after the game-data pointer
settled). Checks: CTest 37/37, Python 612 OK, headless 16:9 and auto 42/42 identical to slice211, race frames differ
from frame 7700 at 21:9/32:9 (hook evidence in docs/BUILD.md slice 212b), gt4-gs 1.0.1 gets no kind 7, gt4-gs 1.1.0
switches to fill at frame 7670. Pins `docs/inputs/slice212b-pcsx2.json` (`--mode pcsx2`; `pcsx2-slice211` history).
Open: owner tests T1 (roundness, auto vs `GT4_ASPECT_FOV_EXTRA`), T2 (3440x1440, phone-like window), T3 (edge pop-in
at 32:9), menus pillarboxed, the 100% save still loads; then owner approval. HUD (P2) is still 16:9-stretched.
State (2026-10-21, after release 1.0.0, branch `task/080-aspect-ratios`, slice215): main db99f9e merged (480p stays
fixed; the Aspect ratio row is the only aspect control; `GT4_FORCE_WIDESCREEN=1` always, the game's own 16:9 is the base;
the GS settings-file aspect stays forced 16:9 and the per-scene fill comes only from protocol 1.3). Settings schema 4
migrates the 1.0.0 schema 3 file (drops the retired keys, adds Auto). The launcher passes `GT4_ASPECT_FOV_EXTRA`
through for T1. slice215 = the slice214 release recipe plus the slice212c capsule and the TASK-080 host code: CTest
39/39, Python 616 OK, cargo 256, headless 16:9/auto 42/42 = slice214, race 21:9/32:9 = slice212b, gt4-gs 1.0.1 no kind 7,
gt4-gs 1.1.0 fill from frame 7670. Pin `docs/inputs/slice215-pcsx2.json` (`--mode pcsx2`). Owner test installer
`private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe` (app version 1.0.0; no GitHub release). Planned as
GT4Recomp 1.1.0 (Auto default). Open: the owner tests above, then approval, PR and the 1.1.0 release.
State (2026-10-21, P2 race HUD, branch `task/080-aspect-ratios`, slice217): owner test of P1 on 1920x1080 with Custom
2.44: the race was stretched because gt4-gs 1.1.0 fills the whole window; CoreGS 1.1 already letterboxed. Protocol 1.3
fill now carries the target aspect x 1e6 in `args[1]` (0 = whole window); gt4-gs needs a release that reads it (fork
work). HUD (owner design: 3D full screen, HUD centred 16:9): runtime hook at `RaceDisplay::render` rewrites the HUD
canvas (320 -/+ 240 R; Vert+ below 16:9); the race font x scale (0x0039F554) and the mirror divisor (0x0033EF98) are
immediates behind direct calls, so they go through the ELF patch table (`scripts/aspect_elf_patch.py`) as loads from a
host-filled block at guest 0x7F00; library profile `aspect` (only `unit_008F` and `unit_00A7` change). Split screen is
left alone and stays pillarboxed 16:9. Checks: CTest 39/39, Python 627 OK; headless 16:9 menus 42/42 = slice215, 16:9
race 160/160 = slice212b; tachometer aspect at 21:9, 2.44 and 32:9 equal to 16:9 within 0.5% (P1: +31% at 21:9) and
centred like 16:9. Pins `docs/inputs/slice217-pcsx2.json` (`--mode pcsx2`; `pcsx2-slice215` history),
`slice217-license-guard-runtime.json`, `slice217-ee-libraries.json`; decision 0071 lists every hook and word. New owner
installer `private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe` and portable folder
`private/ultrawide-test-portable`. Open: owner T4 (HUD at 21:9/2.44/32:9: dials, lap times, mini-map, mirror,
countdown, results; Custom ratios with CoreGS 1.1 until a gt4-gs release reads `args[1]`), T5 (license test, split
screen if reachable), photo mode; then approval, PR and the 1.1.0 release.
State (2026-10-21, addendum: seamless display, branch `task/080-aspect-ratios`, slice218): owner design: no Aspect ratio
and no Internal resolution option. The aspect is always the chosen Resolution's shape (`GT4_DISPLAY_ASPECT=auto` with
`GT4_OUTPUT_SIZE`; 16:9 unchanged, other ratios turn P1/P2 on); Display has Monitor (connected monitors, default
primary, stable identifier `<GDI device>@<W>x<H>`) and Resolution (the monitor's real modes, default desktop);
fullscreen stays borderless on the chosen monitor; Graphics has Render scale 50-200% (step 25%) shown as "100% · 4x ·
2560x1792". Rule `gt4l_core::display::display_plan`: k = ceil(max(W/640, H/448)), multiplier = k x scale rounded ties
up, clamped 1..12 (gt4-gs takes whole `upscale_multiplier` 1..12). Settings schema 5 (from 3 or 4, backup first). Game
slice218: `GT4_OUTPUT_SIZE`, `GT4_MONITOR`, per-monitor DPI awareness. Checks: CTest 39/39, Python 629 OK, cargo 270,
nav-test 150/150, focus-check 100/100; headless 1920x1080 menus 42/42 and race 160/160 = slice217; 2560x1080 and
3440x1440 hook scaled, HUD widened, fill from VBlank 7670. Pin `docs/inputs/slice218-pcsx2.json` (`--mode pcsx2`;
`pcsx2-slice217` history). New owner installer (sha256 9b039d61...) and portable folder `private/ultrawide-test-portable`.
Open: owner tests on the 1080p remote screen and the 3440x1440 setup; then approval, PR and the 1.1.0 release.
State (2026-10-21, ✅ done, release 1.1.0): the owner tested slice218 on ultrawide and approved ("funcionou
perfeitamente"). Done: P0 (decision 0071), P1 (3D Hor+/Vert+, race view fill, protocol 1.3), P2 (race HUD centred 16:9)
and the seamless-display addendum (Monitor, Resolution, automatic internal resolution, Render scale 50-200%). Release
prep on the branch: the launcher's Settings has no Display tab any more (Graphics holds Display, Graphics and Advanced
sections; `#/settings/display` falls back to Graphics); version 1.1.0 (Cargo workspace, tauri.conf.json, NSIS default,
release notes `docs/release-notes/1.1.0.md`); the launcher's gt4-gs minimum is 1.1.0 and an older install is replaced at
launch (`gs_release::needs_install`, test with an installed 1.0.1); `version.json` is brought in line with the installed
versions at start. The game executable is unchanged (slice218, `docs/inputs/slice218-pcsx2.json`). Draft GitHub
release v1.1.0 with `GT4Recomp-Setup-1.1.0.exe`. Follow-ups: P3 progressive menus (with TASK-078) stays open; split
screen and photo mode in ultrawide are unverified; P4 (optional true-FOV mode) not started.

### TASK-081 — Release 1.0.0 hardening and installer
Release audit 2026-10-21. Branch `task/081-release-1-0-0` (from main 26f83b8). Not PCSX2-tainted.
- Game: CMake `GT4_RELEASE_DISTRIBUTION=ON` drops the build-time shader folder from the executable; shaders load only
  from `<exe folder>\shaders` (the build tree gets its own copy) and a missing folder stops with a clear message.
  slice213 = the slice211 recipe plus the switch; `--mode pcsx2` = slice213 (`docs/inputs/slice213-pcsx2.json`),
  slice211 kept as `pcsx2-slice211`. String scan: no absolute path or user name left.
- Launcher: `launcher/tools/build-release.ps1` builds with `--remap-path-prefix` (Cargo trim-paths is unstable) and
  fails on any user or build path left. The launcher already links the VC runtime statically (Tauri default); it
  imports only the Windows UCRT (`api-ms-win-crt-*`, part of Windows 10/11).
- Requirement checks before setup and Play (`gt4l_core::system_check`, pure and tested; `gt4l_platform::system`
  reads CPUID and probes `vulkan-1.dll`): SSE4.1 with SSE3/SSSE3 (owner decision 2026-10-21; the game executable is
  baseline x86-64 and the PCSX2 GS needs SSE4.1, so the game has no own check), and a GPU with Vulkan 1.2 plus
  `VK_KHR_surface` and `VK_KHR_win32_surface` (the PCSX2 GS renders with Vulkan too, so the check is always on).
- Game Vulkan baseline path (slice214, release blocker): `native/vulkan_extensions.cpp` (pure, CTest
  `native_vulkan_extensions`) enables swapchain maintenance 1 (KHR or EXT, with its instance prerequisites) only when
  the driver has it; otherwise slots wait on the GS timeline and each swapchain image has its own present semaphore.
  `GT4_VULKAN_FORCE_BASELINE=1` forces it (CTest `native_vulkan_pixels_baseline` presents to a hidden window on it).
  slice214 = slice213 plus this change; `--mode pcsx2` = slice214.
- Installer: detects the Evergreen WebView2 Runtime (HKLM WOW6432Node, HKLM, HKCU `pv`) and runs Microsoft's
  bootstrapper (embedded, Authenticode-checked) only when it is missing; default name `GT4Recomp-Setup-X.Y.Z.exe`;
  the uninstaller also removes `SDL3.dll`.
- Launcher controls: the built-in keyboard keys of the `gt4controls` crate now match the TASK-076 layout (they were
  the old Z/X table; a remap wrote them into `controls.toml`). `cargo test --workspace` green again (233 passed).
- Release: README for 1.0.0, `docs/release-notes/1.0.0.md`, draft GitHub release v1.0.0 with the installer and
  `SHA256SUMS`. The owner tests the installer end to end before the merge and the publish.
- Saves in Documents (owner request 2026-10-21): the save library and replays live in `Documents\Gran Turismo 4`
  (`Saves\Active`, `Saves\Save NN`, `Saves\.backup`, `Replays`), found with SHGetKnownFolderPath(FOLDERID_Documents)
  (OneDrive and redirection followed; data-folder fallback `<data>\Gran Turismo 4` with a logged warning;
  `GT4RECOMP_DOCUMENTS_DIR` is a dev and test override). The disc copy, prepared files, gs, config, controls, logs,
  sessions, `runtime\card.ps2` and `library\backups` stay in `%LOCALAPPDATA%\GT4Recomp`. One-time migration
  (`gt4saves::migration`): copy, size and sha256 check, rename the old `library\memcard` and `library\saves` into
  `library.migrated-<UTC stamp>`; both non-empty = no merge, conflict logged; failure = staging copy removed, old
  data untouched; marker `config\library-migration.json`. The guard refuses the player's folder for install,
  update and renderer writes. Saves, Replays and About show the folder with Open folder. The page items now carry
  `id` and `modified` (the real launcher sent neither, so activate and delete could not name an item). Uninstaller
  page: saves and replays in Documents are always kept; the AppData box stays unchecked. `cargo test --workspace`
  250 passed; nav-test 150/150, focus-check pass; scratch exe run migrated a copy of the owner's library.
- Settings feedback (owner, 2026-10-21, round 4): the game is fixed at 480p and 16:9 until the ultrawide update. The
  Input resolution, Widescreen 16:9 and GS Aspect ratio rows are removed; the launcher always sends
  `GT4_FORCE_480P=1` and `GT4_FORCE_WIDESCREEN=1` (no `GT4_RACE_DISPLAY`) and writes `AspectRatio = 16:9`. Old values
  are ignored by a load-time rule (`settings::RETIRED_SETTINGS`; settings stay schema 3, TASK-080 keeps schema 4).
  GS table column `forced` (Python and Rust writers): `aspect` 16:9 and PCSX2's `osd_fps`, `osd_gpu`,
  `osd_resolution` hidden and always written off; the other stock OSD rows hidden; allow-list unchanged. Advanced is a
  section at the bottom of Graphics (no Advanced tab; D-pad reaches it, B back, Y Reset). `cargo test --workspace`
  252 passed; Python 598 OK; nav-test 150/150; focus-check 93/93 pass. Installer rebuilt (sha256 8cf61ef4...), draft
  assets replaced.
State (2026-10-21): ✅ Done. The owner tested the installer end to end and approved it; merged into main and published as release v1.0.0.

### TASK-082 — Native variable frame rate

Owner update2026-10-26: experimental branch preserved at15b7bf3, deferred for CoreGS2.0; excluded from rc7. See TASK-133 and the remote resumption report.
Owner request 2026-10-21: 30, 45, 120, 144 or unlocked FPS with game speed unchanged, real frames from the game rather
than image interpolation (TASK-045 stays the fallback and runs its race oracle in parallel). Study:
docs/NATIVE-VARIABLE-FPS-STUDY.md. GT4's main loop (`func_001016E8`) updates every object with dt 1/60, renders every
object, then flips; render already takes a time `t` (`render_world(buf, t)`), and the race camera lerps two poses with
it. Race time counts simulation steps, and replays and ghosts are frame-indexed inputs.
Owner decisions (2026-10-21, the study's recommendations accepted):
- N1: the simulation (physics, AI, timers, the 60 Hz guest VBlank) stays fixed at 60 Hz; only rendering changes rate.
  N2 (BLACK-style variable dt) is rejected for vehicle physics and allowed only for cosmetic effects.
- First P1, render skip below 60: game speed stays 100% down to 20 FPS with an identical simulation. Then extra
  render-only passes above 60 (P2 camera-only at 120 Hz in Time Trial; kill gate: a pass over 7.3 ms), P3 cars and
  scene, P4 present pacing (cap, VRR, unlocked), P5 a combined scheduler.
- Extra frames interpolate (+1 frame) by default; latency is cut elsewhere later. Extrapolation stays an option.
- Allowed: the main-loop ELF patch with re-translation (superseding decision 0055 for this feature), a GS stream
  PRESENT record and the gt4-gs change. Menus stay at 60 Hz.
Cost today: race critical path 10-19 ms of 16.7; one extra render pass 7-15 ms, so 120/144 needs cheaper passes.

### TASK-083 — Faster loading and saves (future study)
Owner request 2026-10-21. Loading and saves run at PS2 speed today: the CDVD HLE models every read as 0.4 ms plus
0.25 ms per 2 KiB sector of guest time (about 8 MB/s, donor `iop/cdvd.cpp` `read_cycles`), and the guest clock stays
paced at 60 VBlanks per second during loading screens and saves. The owner reports that PCSX2's Fast CDVD breaks
nothing in GT4 but does not gain much, and asks for a deeper, deliberate speed-up, game code changes included.
Study first: measure where boot, race-entry and save time goes (disc wait, decompression, fixed waits in game code,
card writes, VBlank pacing). Candidate levers: instant disc reads (and a faster byte copy than `write8` per byte);
an unthrottled guest clock while a loading or save screen is active (the TASK-077 study found the loading object),
with audio muted or kept at the right pace; hooks or ELF patches that shorten fixed waits and per-frame save
chunking. Never during races or replays; physics, lap times and replays untouched. After 1.0.0.

### TASK-084 — Frame cost optimisation (Done 2026-10-22)
Owner request 2026-10-21. Study: docs/FRAME-COST-STUDY.md (slice211, headless, timed only with no builds running).
In the day race the EE thread takes 8.2 ms per frame, of which the translated game code is 2.9 ms; the rest is VIF1
job posting (about 5,200 MTVU jobs per frame, one doorbell), the journal GS front end, stream writes, IOP, run loop and
waits for the VU1 worker. The night-race lens-flare readback (2 KiB per frame) adds 3.3 ms of EE wait plus a 0.9-1.6 ms
GS round trip and serialises EE and VU1. Ranked plan: (1) defer the flare readback one frame, ordered through the
journal, after checking nothing in gameplay reads it (-4 to -5 ms night); (2) one MTVU job per VIF1 DMA chain and
wake-ups only when waiting (-1 to -2 ms); (3) no EE wait for the VU1 worker at every VBlank in pcsx2 mode (-1 to -3 ms);
(4) drop the second GIF walk and a copy (-0.4 to -0.8 ms); then compiler flags, run-loop fast paths, VU1 speed. Add the
missing F8 instrumentation first (pacing-sleep scope, fixed summary formula, per-readback records). Each step: its
own slice, headless frame identity, CTest and Python, owner F8 check. Feeds TASK-082 (a render pass must fit 7.3 ms for
120 Hz) and weaker PCs. After 1.0.0. **Done 2026-10-22** (branch `task/084-frame-cost-122`, build `slice262`, pin
`docs/inputs/slice262-pcsx2.json`; PR into main opened the same day). Owner in-game test, session
`private/manual-play/session-20261022-163408-808184`, `--mode pcsx2`, 0 late frames in every scene, picture
unchanged, approved:
- Showroom: frame median 16.689 ms, p99 19.056; critical path 6.06 ms/frame; vif jobs/frame 35.8; EE wait 0.000.
- Nordschleife Time Trial (Supra Wood One, 480p): median 16.664, p99 18.304; critical path 6.13; jobs/frame 58.6;
  EE wait 1.135 (download), readback 0.68.
- Night race, Clubman Stage Route 5, 5 AI: median 16.663, p99 18.565; critical path 9.97 (TASK-041 owner baseline
  11.8); jobs/frame 80.4; EE wait 3.052 (download), readback 1.30; 0 audio underruns.
Headless (slice262, this PC): day race 12.07 -> 10.38 ms, night 13.21 -> 12.08, showroom 9.88 -> 8.61, menu
1.54 -> 1.41; frame dumps and the GS stream session hash identical to release 1.2.0 (slice240);
`GT4_MTVU_VERIFY=1` 0 mismatches in 4,200 frames; CTest 40/40; Python 640 tests OK (42 skipped). Switches:
`GT4_MTVU_CHAIN` (one VIF1 job per DMA chain), `GT4_GIF_FASTPATH` (no second GS stream walk),
`GT4_MTVU_VBLANK_JOURNAL` (VBlank as a journal entry in pcsx2 mode), `GT4_PERF_COUNTERS`. Note: the night-race
download wait and the GS readbacks stay out of scope (TASK-041 Performance Mode and CoreGS 2.0 cover them). Build,
numbers and the owner checklist: docs/BUILD.md "Slice 260/262".

### TASK-085 — Launcher self-update starts the setup outside its job
Done 2026-10-21 (branch `task/085-launcher-self-update`). Owner report: launcher 1.0.0 offered 1.1.0, downloaded it,
closed, and nothing was installed. Cause (Confirmed): the launcher sits in a kill-on-close Job Object so the game ends
with it, and the setup program it started for the update joined that job; Windows ended the setup as soon as the
launcher exited. The log showed "setup verified; starting it" and the verified setup stayed in
`%TEMP%\GT4Recomp-update`. Fix: the job allows breakaway and the setup starts with `CREATE_BREAKAWAY_FROM_JOB`
(`gt4l_platform::process::spawn_outside_job`); when an outer job forbids that, Explorer starts it and the launcher waits
3 s. The game still ends with the launcher, and a launcher update is refused while the game runs. Test
`gt4l-platform/tests/job_breakaway.rs` runs the real job code in a child process: a helper started outside the job
survives the exit, a plain one dies. Launchers 1.0.0 keep the bug, so their users install 1.1.0 by running the setup
(release notes). Release v1.1.0 was set back to draft while fixing it and republished with the fixed launcher.

### TASK-086 — Progressive 480p everywhere
**Shelved (owner, 2026-10-21): the game stays 480i.** Step 1 was built on branch `task/086-progressive-480p` (a3873a1,
slice232, kept for a later restart; not merged). Owner test of the 1.1.1-test installer: boot menus stay 480i (known);
after the first race the menus are 480p, but the car selection shows large green/black garbage blocks over the car
picture, and the next race load hangs on its loading screen (frames still produced). Likely a VRAM layout or
render-target difference of the 480p CT16 menu mode in the car-select/garage path; not investigated further.
Original plan (owner, 2026-10-21; after the CoreGS 2.0 parity work, shares the ELF patch table with TASK-078 P5 and TASK-080
P3). Study: [docs/PROGRESSIVE-EVERYWHERE-STUDY.md](docs/PROGRESSIVE-EVERYWHERE-STUDY.md). The 480p "Try..." pop-up in
the Screen options proves the menus already render at 480p: `MOption.applyRaceDisplayMode` (`func_00436640`) requests
CRT row 6 (480p, 640x448) and `applyMenuDisplayMode` (`func_00436740`) returns to row 1 (480i field). Plan:
(1) menus and races, behind a switch: `func_00436740` requests 480p (0x436750 `24050001`→`24050002`, 0x436754
`0000302D`→`24060006`) and races always take the 480p index (0x43665C); same VRAM footprint as 480i menus; removes the
1-second black between menus and races; (2) CRT table row 2 (photo pause, after movies) and boot init
(`func_001063A8`, `func_00107788`); (3) optional: movies and photo develop (256x384 buffers, risky) and CT16 vs CT24
menus. Checks: SMODE gate, the 42-dump headless run, owner pass on menus, garage, colour window, license tests, race,
replay and split screen. Risks: two hard-coded halvings (`func_00218170`, `mRenderContextPS2__virtual_67`), 16-bit
banding in menus. For CoreGS 2.0 it moves the deinterlacers (most of M15) and field compose off the main path; a small
interlaced fallback stays. Owner decisions open: progressive by default, CT16 or CT24 menus, movies in scope.

### TASK-096 — Remove CoreGS 1.1 from the launcher

State (2026-10-22): done on branch `task/096-remove-coregs11` for launcher release 1.2.3. Not merged; waiting for the owner test and approval.
Owner decision (2026-10-22): CoreGS 1.1 is removed from the launcher. Only GSdx (the external PCSX2 GS process `gt4-gs.exe`) runs
the game. CoreGS 1.1 stays inside the game executable (no native change) and the launcher never starts it.
- Renderer: the choice lists GSdx (selected) and CoreGS 2.0 (coming soon, disabled). `launch::choose_renderer` refuses to start
  the session when GSdx is not installed or fails verification; Play does not start the game.
- Error text: "GSdx could not run the game: {reason}. Check your internet connection and try again. Details are in the log
  folder: {folder}." Reasons: "GSdx is not installed yet", and "GSdx stopped while playing" for a restart request.
- Restart: the game's restart-with-CoreGS-1.1 request (`GS_RESTART_MARKER`) ends the session with that error; nothing restarts.
- Settings schema 7 (`migrate::settings_6_to_7`): a stored `renderer = "coregs11"` becomes `pcsx2`. The CoreGS 1.1 rows
  (Internal resolution, Scaling filter, FSR sharpness, Deinterlacing, Anti-aliasing) are retired: ignored on load, dropped at the next write.
- Setup: the renderer chooser dialog is gone; setup installs GSdx. A failed download finishes setup with a note, and Play
  refuses until GSdx is installed.
- Docs: README, `docs/launcher/gs-settings.toml` (`gs_engine` lists GSdx only), `docs/launcher/FLOW-AUDIT.md`. Old plans and
  release notes keep their history.
- Supersedes TASK-089 (its CoreGS 1.1 path no longer exists).
- Checks: `cargo test --workspace` all green (launcher tests with the new migration, renderer-choice, restart and renderer-switch tests).

### TASK-093 — Custom textures straight in the Textures folder
State (2026-10-22): done on branch `task/093-textures-root`; waiting for gt4-gs 1.2.1 (not published yet) and the owner
test. Planned for release 1.2.3 with TASK-092.
- gt4-gs 1.2.1 reads the `Textures` folder itself, recursively (`Textures\replacements\...` still works). A pack goes
  straight into `<install>\data\Textures`.
- The launcher creates only `Textures` (no `replacements`). `RENDERER_MINIMUM` is 1.2.1: an installed gt4-gs 1.2.0 is
  replaced at the next launch (`gs_release::needs_install`, as before with 1.0.1 and 1.2.0).
- One-time flatten at start, after the large-data move, on the same thread (`relocation::flatten_textures`): only when
  gt4-gs 1.2.1+ is in use (`current.txt`), only when `replacements` is a real folder (junctions and links are skipped),
  each child renames up into `Textures`, a name already there stays in `replacements` (logged), `replacements` is
  removed only when empty, and the first error stops the rest until the next start. Nothing is deleted or overwritten.
  Pure logic and tests: `gt4l-core/src/textures.rs`.
- Text: `docs/launcher/gs-settings.toml` (`texture_load`, `textures_folder` notes), the Open textures folder comment,
  README bullet. The 1.2.1 release notes stay as history.

### TASK-094 — Desktop shortcut for the installed program
Built on branch `task/094-desktop-shortcut` (2026-10-22), waiting for the owner test; planned for launcher release 1.2.3.
Owner request (2026-10-22): the installer makes Start-menu shortcuts only, and players want a desktop icon.
- Interactive install and update: the finish page offers "Create a desktop shortcut", ticked by default (after "Start Gran Turismo 4"). A choice made earlier (`DesktopShortcut` = 0) unticks it.
- The shortcut `Gran Turismo 4.lnk` is on the all-users desktop and starts `$INSTDIR\GT4Recomp.exe` (working folder and icon: the same).
- The choice is kept in HKLM `DesktopShortcut` (1 created, 0 declined). Uninstall removes the shortcut; the app key goes with the uninstall.
- Silent runs (`/S`, and the launcher's `/S /UPDATE` self-update): `DesktopShortcut` missing (an install from before 1.2.3, or a fresh silent install) or 1 creates or makes again the shortcut; 0 does nothing.
Acceptance: `makensis` compiles with no new warnings; owner test of a fresh interactive install (box ticked and unticked), an update from 1.2.2 (box follows the stored choice), a silent update, and the uninstall.

### TASK-092 — MTVU test switches after TASK-084
🟡 **Done on branch (2026-10-22), waiting for release.** Branch `task/092-mtvu-test-switches`, slice264 (build recipe
in BUILD.md "Slice 264"). Owner decisions 2026-10-22: release 1.2.2 ships the owner-tested slice262 build; this fixes
the TASK-084 review findings afterwards. None is reachable from the launcher, which sets neither variable.
Result: `GT4_MTVU=0` in pcsx2 mode no longer crashes (slice262: access violation, no frames; slice264: 4,200 frames,
41 dumps, 4,200 VSYNCs at the GS); `GT4_MTVU_VERIFY=1` sends its VSYNCs (4,200 frames, 0 mismatches); the default path
is identical to slice262 (dump hashes and GS stream session hash equal in menus and night). Open before merge: the
`gt4vulkan.exe` pin tests (`test_make_distribution`, `test_portable_install`) still point at slice262's executable;
the new pins (a slice264 pin set) need an owner decision on the release path. CTest 40 of 41 pass (the 41st is
`python_native_contracts`, the same two pin tests).
- `GT4_MTVU=0` in pcsx2 mode crashes at the first VBlank (Confirmed by reading): `mtvu_bridge.cpp` sets
  `set_vsync_deferral(settings.vblank_journal)` without checking `enabled()`, `start()` returns before the journal
  exists, and `vblank_sync()` calls `r.journal->append_vblank_flush()` on a null journal.
- `GT4_MTVU_VERIFY=1` in pcsx2 mode sends no VSYNC (Confirmed by reading): `vblank_sync()` keeps the blocking drain,
  but the GS stream client still holds the field and nothing calls `post_pending_vsync`.
- Fix: one condition, `vblank_journal && enabled() && !verify`, for both the client deferral and `defer`.
- Doorbell sleeper handshake (`ring` / `wait_changed`): seq_cst on the four operations so the store-then-load
  pattern holds on ARM (Hypothesis today; x86 locked RMWs already order it). Needed before the Android phase.
- Acceptance: CTest fixtures for both switches (a VSYNC reaches the stream; no crash), the frame identity matrix and
  the GS stream session hash unchanged against slice262 with default switches, a new slice and pins.
- 1.2.2 bug found by the CoreGS 2.0 agent (2026-10-22), fixed here: donor mode (CoreGS 1.1) crashed at the first
  guest VBlank (exit 0xC0000005; slice262 and slice264 crash, slice240 runs). `mtvu_bridge.cpp` declares `Donor donor;`
  and sets `post_vsync` only in pcsx2/hw mode; the struct had no initialisers, so `vblank_sync` took the TASK-084
  deferred path and called stack garbage. Every `Donor` callback is now `= nullptr`; `native_mtvu_switches` checks a
  Donor default-initialised over 0xFF memory. slice268 (O:\gt4recomp-scratch\slice268-build, the slice264 recipe):
  CTest 40/40; headless menus with `--renderer donor` and `pcsx2` both run (exit 0). The launcher drops CoreGS 1.1 in
  1.2.3 (TASK-096); the fix stays as protection for runs without the launcher and the built-in restart path.

### TASK-101 — Multi-game launcher (Gran Turismo 4 and Spec II)
Planned (owner, 2026-10-23) for release 1.3.0, the product side of TASK-100 (its S9). Plan and owner decisions:
[docs/MULTI-GAME-LAUNCHER-PLAN.md](docs/MULTI-GAME-LAUNCHER-PLAN.md). One executable per game, one shared settings set,
per-game disc, prepared files, card, saves and replays (saves are incompatible both ways and share the PS2 folder
name), identification by ISO, the selected game is the last played, Spec II 1.10 and same-executable variants, and
the Spec II logo on the home screen when it is selected.
Progress (2026-10-23): launcher side implemented on `task/101-multi-game` (pieces 1 to 6 of the plan; the UI in the mock and
headless checks): game table, per-game folders and libraries, identification by serial and executable hash with named image
variants (1.10, PT-BR, variant, refusals), Spec II setup with the Rust port of the ELF patch (byte-identical to the script),
per-game launch, the 1.2.x to `games\gt4` migration, selector, Install, Settings > Games. Owner decision 2026-10-23: no permission from the Spec II authors is needed; credit them by name and ship none of
their files (full credit list in the release notes, About and README). Open: look on a real window, the authors'
logo, installer build, `PreviousDataDir` into the games layout.

### TASK-102 — Launcher language: automatic en-US / pt-BR with a General > Language setting
Implemented on `task/101-multi-game` (owner, 2026-10-23). The launcher follows the Windows language (`auto`): pt-BR, pt_BR
and pt (and pt-PT, by the base language) give Portuguese; anything else English. Settings > General > Language (Auto,
English, Português (Brasil)) applies at once, without a restart. The texts are data: `launcher/crates/gt4l-tauri/ui/locales/<tag>.json`
(one flat file per language, keys with `{placeholders}`, en-US as the fallback), `ui/locales/index.json` (the language list),
`ui/js/i18n.js` (`GT4.t`), and the launcher's own messages in the same files under `rust.*` keys (`gt4l-core/build.rs`
generates the table; `gt4l-core/src/messages.rs`). The short release notes are in `docs/release-notes/launcher/<version>.md`
(en-US) and `docs/release-notes/launcher/<tag>/<version>.md` (pt-BR for 1.0.0 to 1.2.3). A new language is documented in
[docs/TRANSLATING.md](docs/TRANSLATING.md). Checks: focus-check in en-US (151 of 151) and pt-BR (158 of 158, with the
locale parity, placeholder and index checks), nav-test 150 of 150, `cargo test --workspace` 450 passed. Screenshots of Home,
Settings > General and Settings > Graphics in pt-BR at 1440 x 860: `O:\gt4recomp-scratch\slice279-shots`. Not translated yet:
the PCSX2 GS rows that the table does not list in the UI keys (they show their English text), the XInput names in the
controller chips, the key names, the Release notes text of 1.3.0 in the sample (dev mock only), and the library's
`Active save` slot name (a value the code compares). Owner approved the mock in pt-BR (2026-10-23; main-agent review:
cargo 450 passed, focus-check en-US 151/151 and pt-BR 158/158, nav-test 150/150). Open: the owner test on a real
window with the 1.3.0 candidate.

### TASK-103 — Graphics API choice for GSdx (Vulkan, Direct3D 12, Direct3D 11)
Implemented on `task/101-multi-game` (owner, 2026-10-23), release 1.3.0; owner in-game test pending. Settings > Graphics
shows "Graphics API (GSdx)" (table row `graphics_api`, written as `[EmuCore/GS] Renderer` = 14, 15 or 3): Vulkan
(recommended, default), Direct3D 12 (recommended), Direct3D 11 (older PCs). The row moved from the Advanced section to the
Graphics page. The engine setting keeps its id `renderer` (GSdx or CoreGS 2.0) and its label "Renderer"; the new row
cannot be confused with it in the UI, the settings store or the table. A missing or unknown stored value, and an existing
install, use Vulkan. No OpenGL and no software renderer (OpenGL later, with a new gt4-gs build and its allow-list entry).
Research (2026-10-23, read-only): gt4-gs.exe already contains Direct3D 11, Direct3D 12 and OpenGL; the backend follows
`[EmuCore/GS] Renderer` (DX11 3, OpenGL 12, Vulkan 14, DX12 15). Checks: `cargo test --workspace` 453 passed; Python
`test_gs_*` 168 tests OK; focus-check en-US 151 of 151 and pt-BR 158 of 158, nav-test 150 of 150; screenshots of
Settings > Graphics in `O:\gt4recomp-scratch\slice280-shots` (pt-BR and en-US). Open: the owner in-game test on Direct3D 11
and Direct3D 12 against Vulkan (Nordschleife TT 480p, night race, menus, ultrawide crop and fill, texture pack, upscaling,
flares and readbacks). A backend that fails the owner test is not offered.

### TASK-104 — Launcher saves: crash-proof card import, New save, Export, Rename
Planned and implemented (owner, 2026-10-23) for release 1.3.0. GitHub issue moesuito/gt4-staticrecomp#86: importing a PCSX2
`Mcd001.ps2` closed the launcher with no message. Reproduced on the old reader with synthetic cards: a directory that points
back to an ancestor overflowed the stack (0xc00000fd), and a 4 GB length aborted on a 2 TB allocation (0xc0000409). Both are
silent in the launcher. Fix: the reader takes the card size, the allocation area and the indirect FAT list from the
superblock and accepts 8, 16, 32 and 64 MB cards (with or without spare bytes); every length is checked against the chain
and the card before allocating; directories nest at most 8 levels and each directory is visited once (a loop is
`corrupt card`); the import runs under `catch_unwind` and logs the file name and size. Messages: not a PS2 memory card,
unsupported card size, card damaged (with the reason), no Gran Turismo 4 save on this card (with what it holds).
Saves page: New save (one confirmation; the current save is kept as a new `Save NN`, the last run's card is harvested and
moved to `backups` first), Export of the active save or a slot as `.psu`, Rename (name in `name.txt` beside the save,
sanitised, 40 characters, moved with the save when saves are swapped). Tests: `gt4saves/tests/hostile_cards.rs` (10 synthetic
card cases, built in `tests/common`), `gt4saves/tests/save_management.rs` (9 library cases). Checks: `cargo test --workspace`
474 passed, 0 failed, 4 ignored; focus-check en-US 158/158 and pt-BR 165/165, nav-test 150/150. Screenshots:
`O:\gt4recomp-scratch\slice283-shots\saves-en-US.png` and `saves-pt-BR.png`. Open: the panic guard is not covered by a test
(no injected panic); the owner test with a real PCSX2 card and in-game New save; gamepad rename needs a keyboard to type.

### TASK-105 — Integrated GPUs: lighter default and GPU-lost message (#89)
Planned and implemented (owner report, 2026-10-23) for release 1.3.0. GitHub issue moesuito/gt4-staticrecomp#89, reported on
1.2.3: Intel Core i3-1005G1 with the integrated Intel UHD only, 1440 x 900 screen. The default Render scale gave k 3
(1920 x 1344 internal); GSdx lost the device ("Host GPU lost too many times, device is probably completely wedged") at 3x and
at 2x. Fix: `display::integrated_default_applies` and `display_plan_on`: when the GPU the game uses is integrated (Vulkan
device type 1, `system_check::integrated_gpu_name`; with any discrete card, or none, the default is unchanged) a Render scale
that is the untouched default (100%) plays at 1x (`INTEGRATED_DEFAULT_MULTIPLIER`, 640 x 448). A changed scale keeps its
multiplier. The stored value cannot tell an untouched 100% from a chosen one: a chosen 100% on an integrated GPU also plays at
1x. The decision is logged (`display: integrated GPU <name>: default internal 1x`); the Render scale readout follows the plan.
The device comes from the same Vulkan probe as the system check (`gt4l_platform::system::integrated_gpu_name`, read once per
launcher process). GPU lost: a crashed session whose `game.log` holds a GSdx device-lost text (`exit::GPU_LOST_MARKERS`:
"Host GPU lost", "device is probably completely wedged", `VK_ERROR_DEVICE_LOST`) shows the plain message in en-US and pt-BR
(`rust.game.gpu_lost`) in place of the generic crash text; the matched line goes to the launcher log. Saves dates use the
interface locale (`Intl.DateTimeFormat` with `GT4.i18n.intlLocale()`; pt-BR showed English months). Not changed: the default
Graphics API (Vulkan) and gt4-gs. Checks: `cargo test --workspace` (launcher) 487 passed, 0 failed, 4 ignored (new tests in
`display.rs`, `system_check.rs`, `exit.rs`, `tests/launch_contract.rs`, `model.rs`); focus-check en-US 158/158 and pt-BR
165/165, nav-test 150/150. Open: the owner test on the Intel UHD machine (1x default and the message are not yet seen in-game);
the exact device-lost line in a real `game.log` is not confirmed (the markers come from the GSdx dialog text); a multi-GPU
laptop keeps the discrete default by design.

### TASK-106 — CHD disc images (issue #90)
Planned (owner, 2026-10-23) for release 1.3.1. Issue moesuito/gt4-staticrecomp#90 asks for CHD support. The launcher
already writes its own ISO image of the player's disc at setup, so the game never reads the player's file: accept `.chd`
(and keep `.iso`) in the disc picker, decompress it to that image with a permissively licensed CHD reader (MIT or
BSD; check the licence before use; never GPL in the launcher), then run the normal identification (serial and
executable hash, both games and their variants). Show progress and free-space needs (a full image is about 3.7 GB for
Spec II). Acceptance: synthetic CHD fixtures in tests (no game data), a CD and a DVD CHD, a damaged CHD refused with a
plain message, and an owner test with a real CHD of each game.

### TASK-107 — Automatic graphics setup per GPU
Planned (owner, 2026-10-23) for release 1.3.1, after issue #89 and TASK-105. Owner design:
1. **Game without Vulkan (check first).** Done by TASK-108 (1.3.0): the game program is not checked for Vulkan
   when the chosen API is not Vulkan, and Automatic falls back to Direct3D 11. Today `gt4vulkan.exe` itself requires Vulkan 1.2 (`native/native_vulkan.cpp`,
   `system_check.rs`), so a GPU without Vulkan cannot play even when GSdx draws with Direct3D or OpenGL. Find out
   whether the game needs its own Vulkan device when `GT4_RENDERER=pcsx2` and, if not, make that path Vulkan-free and
   relax the launcher check for it. Everything below depends on this.
   First finding (2026-10-23, High confidence, from the owner's 1.2.3 game.log): in pcsx2 mode the game logs
   `[gs-ipc] GT4_RENDERER=pcsx2: donor Vulkan renderer not created`, so the game creates no Vulkan device and only
   gt4-gs renders; the Vulkan 1.2 requirement then comes from the launcher's system check (and gt4-gs on Vulkan). To
   confirm: no other Vulkan call on that path (the loader is loaded dynamically) and a run on a machine without Vulkan.
2. **Automatic Graphics API.** Vulkan when the GPU reports Vulkan 1.2 or newer with what GSdx needs; otherwise OpenGL
   on NVIDIA and Direct3D 11 on Intel and AMD (OpenGL tends to stall on non-NVIDIA GPUs; OpenGL has the same blending
   precision as Vulkan and Direct3D 12; Direct3D 11 is for weak and old hardware only). A player's own choice always
   wins; the automatic choice is shown as such.
3. **OpenGL in gt4-gs.** Allow `Renderer = 12` in a new gt4-gs release (allow-list and SHA256SUMS in the fork) and in
   `docs/launcher/gs-settings.toml`; offer OpenGL in the Graphics API list ("older PCs" group) after an owner test.
4. **GPU generation table.** A small table we maintain ourselves (no third-party database copied; check the licence of
   any source used for reference): vendor ID plus PCI device-ID ranges per GPU generation (for example Intel Gen9, Gen11,
   Xe; AMD GCN, RDNA1-3; NVIDIA Maxwell to Ada), so every variant of a family (RX 6600, 6600M, 6600 XT, 6650 XT) falls
   in the right range. Each generation gives a starting profile (internal scale, API). An unknown GPU uses the driver's
   own facts (integrated or discrete, video memory, Vulkan or Direct3D level) and else the default.
5. **Step down after a lost GPU.** When a session ends with a GPU-lost line (TASK-105), the next start uses one level
   less (lower internal scale, then Direct3D 11), told to the player, never below 1x; a successful session keeps it.
6. **Auto render scale token.** Store the untouched Render scale as `auto` instead of 100 (settings migration), so an
   automatic choice and a chosen 100% are told apart (TASK-105 limit).
Acceptance: pure unit tests for every rule (tables, fallbacks, step-down), the migration test, and owner tests on this PC
plus at least one integrated-GPU machine.

### TASK-108 — Launcher: automatic graphics API and card; game programs renamed gt4.exe and gt4-spec2.exe
Status: done in the working tree (branch `task/101-multi-game`, release 1.3.0); owner test pending. Owner decisions 2026-10-23.
Completes step 1 of TASK-107 (the game no longer needs Vulkan to start).
1. **Automatic Graphics API.** `graphics_api` gets a new first choice, **Automatic** (`auto`), and it is the default for
   new installs and for installs that never changed the row (no stored value). It is resolved at every launcher start and
   before each Play: Vulkan when the chosen graphics card reports Vulkan 1.2 or newer, otherwise Direct3D 11. The Settings
   page shows the resolved API ("Automatic (Vulkan)"). An explicit Vulkan, Direct3D 12 or Direct3D 11 is kept.
   Explicit Vulkan on a card without Vulkan 1.2 runs with Direct3D 11 for that run, with the note "No Vulkan 1.2 graphics
   card was found, so the game uses Direct3D 11. You can change this in Settings > Graphics." (pt-BR in the locale file).
   The Vulkan check is per run, so a driver update brings Vulkan back without a setting change. Setup needs no GPU;
   the processor check (SSE4.1) is unchanged.
2. **Graphics card and Adapter.** The launcher lists the cards: Vulkan device names from its Vulkan probe and DXGI
   adapter descriptions (`IDXGIFactory1::EnumAdapters1`; software adapters skipped). The **Graphics card** row follows the
   Graphics API row: "Automatic (best)" (default) and every card by name. Automatic picks a discrete card over an
   integrated one, then the most dedicated video memory. The chosen card's name goes to the game's `Adapter` key: the
   Vulkan name for Vulkan, the DXGI description for Direct3D 11 and 12. No name for the API means an empty Adapter (system
   default) with a log line. A stored card that is gone (unplugged eGPU, new PC) falls back to Automatic for that run,
   with a log line. The `Adapter` line is written under `[EmuCore/GS]` by the launcher (`gs_config_text`), because the
   table row `adapter` is still `resolved` (not settable from a value; the table and `scripts/gs_settings.py` are unchanged).
3. **One chosen card.** The Vulkan support decision (point 1) and the integrated-GPU 1x default (TASK-105) use the chosen
   card instead of "all GPUs integrated".
4. **Logs.** Every Play logs the card list, the chosen card, the API and the Adapter value.
5. **Game programs renamed at distribution.** `gt4vulkan.exe` is `gt4.exe` and `gt4vulkan-spec2.exe` is `gt4-spec2.exe`
   in the installer, the portable zip, the game folder and the launcher (`games.rs`, paths, launch, process detection,
   update and migration). The build output names are unchanged. The files are byte-identical and pinned by hash, so no
   game rebuild and no new pins. `scripts/make_distribution.py` copies them under the new names; the installer deletes
   the 1.2.x names from `game\` on install and update, and checks both sets of names for locks; the uninstaller removes
   the whole `game` folder. Process detection and the portable swap cover both old and new names.
   The developer scripts keep `gt4vulkan.exe`, the build output name.
Acceptance: pure unit tests for each rule in `gt4l-core::gpu` (laptop integrated + NVIDIA, integrated only without
Vulkan, two discrete cards, explicit choice kept, missing card, software skipped, name mismatch leaves Adapter empty), the
Vulkan fallback in both languages, `system_check` for the processor, the settings migration of the Automatic default,
the distribution tests with the new names, `cargo test --workspace`, the touched Python modules, the headless Edge focus
checks, and an owner test on this PC (NVIDIA + integrated) and on one machine without Vulkan 1.2.

### TASK-109 — Launcher update channels and version picker
**Implemented for 1.3.0** (owner, 2026-10-23; pulled forward from 1.3.1), on `task/101-multi-game`, owner test pending.
- Settings > General, below Language: **Channel** (Stable default, or Beta) and **Version** (a dropdown read from GitHub
  each time the page opens, with a refresh button).
- Stable offers stable releases. Beta offers the release candidates (`vX.Y.Z-rcN`, GitHub pre-releases) and stable releases
  together, newest by version; a draft is never offered. A candidate build with no stored choice starts on Beta.
- Moving back to Stable never downgrades automatically: the launcher waits for a stable release newer than the installed
  build.
- The version carries its release candidate part (`gt4l-core::version::ReleaseVersion`): `1.3.0-rc4 < 1.3.0-rc5 < 1.3.0 <
  1.3.1-rc1 < 1.3.1`. A build gets its label from `launcher/tools/build-release.ps1 -Label rc5` (`GT4_RELEASE_LABEL`, read
  by `gt4l-core/build.rs`); About, the home credit line, `version.json` and the update decisions use `1.3.0-rc5`. Asset
  names of a test build: `GT4Recomp-Setup-1.3.0-rc5.exe`, `GT4Recomp-1.3.0-rc5-portable.zip`.
- The picker lists, per channel, stable releases by version (Stable) or candidates by publication date with a newer stable on
  top (Beta); drafts and versions below 1.3.0-rc5 are left out (older builds cannot read the multi-game data layout).
  Installing a chosen version asks first. A target older than the installed version (SemVer, any channel) shows the strong
  downgrade warning (back up the saves, a saves folder button, an explicit "Install older version" button that is never the
  default focus); a newer target is always the plain question. Yes runs the normal update path (`install_launcher_version`:
  download, `SHA256SUMS` check, silent setup or portable zip swap); No restores the dropdown. When a channel lists only
  versions older than the running build the page says updates resume when a newer release ships.
- Both languages. Tests: version parsing and ordering, channel default, `decide` per channel, no automatic downgrade, list
  building and sorting, `is_downgrade` and the dialog variant, asset names, a fake GitHub JSON through `FolderSource`,
  `make_portable_zip` with a label; headless Edge focus checks (Yes calls the update path, No restores the selection).
- Not verified here: the real GitHub fetch (the fake JSON follows the REST format), a real install of a chosen version.

### TASK-110 — Controller tuning: stick and trigger deadzone, sensitivity and response
Planned (owner, 2026-10-23) for release 1.3.1. Players report that the game sometimes reads the stick and triggers
differently from what the controller sends. No code yet.
- A per-controller tuning section on the launcher's Controls page, applied by the game's input layer
  (`native/host_io.cpp`, SDL3) and the controls config the launcher writes. It needs a game rebuild and new pins.
- Settings (the feature list only, from what PCSX2 offers its players; no PCSX2 code is used, clean room):
  left and right stick deadzone; stick sensitivity or scale; response curve (linear or curved); outer deadzone or
  saturation; trigger deadzone and saturation (the analog throttle and brake mapped to GT4's pressure buttons and stick
  axes); button pressure modifier; stick inversion; vibration strength.
- A live test view in the launcher showing the raw value and the value the game will see, for each stick and trigger.
- Defaults reproduce today's behaviour exactly. Unit tests for every curve and deadzone rule.

### TASK-111 — Logs for bug reports
Implemented (rc4, 2026-10-23, `task/101-multi-game`) for release 1.3.0, together with TASK-112. The owner moved it from
1.3.1 into rc4 (2026-10-23). Nothing from this entry is left for 1.3.1.
- **Export logs** (About, and offered by a dialog after a crash or a lost graphics card): one zip
  `GT4Recomp-logs-YYYYMMDD-HHMMSS.zip` in a folder the player picks (Desktop by default). It holds `latest.log`, the
  last 3 launcher session logs, the last 3 game session folders' `game.log` and `gt4-gs.ini`, the settings file and
  `system.txt` (Windows version, CPU, every GPU with vendor, driver version and video memory, the chosen card and API,
  the displays, launcher version, install or portable, the installed games).
- The Windows user name and the user profile paths are masked in every text (`C:\Users\<user>`). Cards, saves,
  replays, disc images, prepared game files and programs are never put in (checked by a unit test).
- After the export the zip path is shown, with "Open the folder" and "Report a problem on GitHub" (the named link
  `report_problem`, through `open_link`).
- Both languages. Unit tests in `gt4l-core/src/log_export.rs`: the path and user masking, the forbidden entries, the
  zip contents, the newest-first selection and the system report.
- The size limits are the session scheme of TASK-112 (20 session logs, 20 game session folders), not a size cap.

### TASK-112 — Launcher logs: one file per session, last 20 kept, latest.log
Implemented (rc4, 2026-10-23, `task/101-multi-game`) for release 1.3.0, with TASK-111. Replaces the single
`launcher.log` of 1.2.x, which had no size limit.
- One file per launcher session: `logs\launcher-YYYYMMDD-HHMMSS.log` (local time; a counter when two starts share a
  second). Each line is `[time] LEVEL text`, with `INFO`, `WARN` or `ERROR`.
- `ERROR`: a game crash, a lost graphics card, a setup or import failure, a failed harvest, an update failure, a
  WebView2 problem, a settings file that could not be written. `WARN`: a soft problem (a refused disc, a GSdx
  not installed during setup, a version file not updated).
- The last 20 session logs are kept; the oldest go when a session starts. The running session's file is never removed.
- `logs\latest.log`: a header (version, install kind, Windows, date), the `ERROR` lines of the earlier kept sessions,
  oldest first and each prefixed with its file name, then the current session's lines as they are written. About >
  "Open latest log" selects it in Explorer.
- A 1.2.x `launcher.log` is renamed once to `launcher-legacy-YYYYMMDD.log` and counts as one of the kept logs.
- Game session folders (`data\sessions\session-*`) are capped at the last 20, never the running one.
- Both languages. Unit tests in `gt4l-core/src/logs.rs`: names, rotation at 20, the running file kept, the `latest.log`
  error collection, the legacy migration and the session-folder cap.

### TASK-113 — Spec II race HUD digits at ultrawide ratios
Known issue of 1.3.0 (owner test on rc4, 2026-10-23, ultrawide 3440 x 1440, GSdx 6x): in Spec II only, the race HUD's
current-speed figure and the odometer digits are stretched horizontally and spill past their place on the gauges; the
rest of the HUD (gauges, gear, G meter, labels) is right; at 16:9 and narrower it is all right, and GT4 v2.00 is fine at every ratio. Hypothesis: Spec II draws those digits
through a path the S8 HUD hook (`RaceDisplay::render` 0x1C5040, HUD matrix at this+0x70) does not cover, or with a
second matrix or sprite list the widening scales twice. Find where Spec II draws the speed and odometer digits, compare
with v2.00, and extend the Spec II hook table; headless check at 16:9 and 21:9 with frame dumps. Planned for the next
update; listed under Known issues in the 1.3.0 notes.

### TASK-114 — rc5 hotfixes (laptop test, RTX 3060 Laptop + Intel UHD 630, Windows 11 25H2)
Done in the working tree (branch `task/101-multi-game`, release 1.3.0, 2026-10-23); owner test pending. Four items:
1. **Setup identifies the disc first.** The first-run page is generic ("Install a game") until the picked image is
   identified; the stored game choice no longer names a game there. The image is identified in place (SYSTEM.CNF serial,
   executable hash, known-image table) before any copy, both when picked and again when setup starts; a refused or changed
   file copies nothing. The game installed is the one the image really is, also for an Install button of the other game.
2. **Copy bar reaches 100%.** The steps after the copy keep the finished copy's size, the copy step is drawn with a full bar
   and its final figure, without a width transition.
3. **GPU list.** Two integrated Vulkan devices (or two small-memory DXGI adapters) with the same PCI ids are one card; two
   identical discrete cards with different LUIDs stay two. The launcher log lists every raw Vulkan and DXGI entry with PCI
   ids and LUID at start, and the cards line shows source, vendor and kind.
4. **Black window.** The intro looks up its version without the `-rcN` suffix (an rc never played it before), is never
   started while the page is hidden, never on the setup page, has a 12 s hard limit, and logs start, end and skip. Cause not
   confirmed; see the 1.3.0 notes.
5. **Names and wording (rc5 polish, 2026-10-24).** Settings > Games titles the rows by the game, and Spec II as
   "Gran Turismo 4 Spec II v1.10" (the only supported version). The detail is the status, what tells the copy apart
   (`USA v2.00`; Spec II `PT-BR`, or nothing for the plain 1.10 image, `other image` for an unpinned image) and the size.
   The button "Change disc image" is "Update" (`games.update`). Setup step 1 says `recognised: <title (variant)>`; the Spec
   II variant is known only after the copy is checked, so step 1 shows the title alone until then. Step 2 is "Copying
   and checking the image", step 3 "Final check". "Spec II 1.10" became "Spec II v1.10" in the texts that name the version.
6. **Compact density (2026-10-24).** Every page is denser (body 13 px, 36 px hints and tabs, 38 px rows, 34 px buttons,
   190 px controls); the existing rules changed, no override file. Checked at 1280x720 and 1440x900 in pt-BR.
7. **Launcher icon (2026-10-24).** `launcher.ico` is the GT emblem on a white rounded square (corner 22%, emblem 80%,
   10 sizes), generated by `launcher/tools/make_brand.py` (`write_launcher_ico`). `game.ico` is unchanged.

8. **Settings open at once (rc5, 2026-10-24; Games, Saves and Replays included).** The graphics cards are probed once and kept (`gt4l-platform::gpu`:
   `cached_gpus`, `store_gpus` from the start-up probe, `refresh_gpus` once at Play); the Settings table, the saves and
   the log read the kept list. The Settings tabs open from memory (settings table, About info, launcher version list,
   the Games list, the Saves and Replays lists), prefetched once the home screen is up. A change (import, new save, rename,
   delete, activate, remove a game) updates the kept list from the command's answer; each tab reads again behind and draws again only when something changed.
   "Loading settings..." waits 300 ms and shows only with no data. A tab's rows fade in once (180 ms, 18 ms apart, the last
   starts within 150 ms; off with reduced motion). Probes per action, from the code paths (before, after): start 1, 1;
   open a Settings tab 2, 0 (1 when the first open comes before the start-up probe ends); save a setting 2, 0; Play 3, 1;
   export logs 1, 0. Checks: `cargo test --workspace` 556 passed, 0 failed, 5 ignored (6 new: the cache, and a settings
   table read that probes nothing); focus-check en-US 195/195 and pt-BR 202/202 (one new check: a loaded Saves tab shows its
   list at once), nav-test 150/150. No locale text added.
   Owner test pending; no release-notes bullet (rc polish).
9. **Dropdown values stay on one line (rc5, 2026-10-24; pt-BR owner screenshot).** The value of a dropdown is nowrap and
   cut with an ellipsis when it is too long, with the full text in its tooltip (`app.css` `.select`, `settings.js`). The
   Language row is wide, so "Automático (Português (Brasil))" fits. The opened list may wrap its names inside the panel.
   Focus check: every General and Graphics dropdown value is one line, and the Language value stays one line with its
   longest text, in both languages (en-US 198/198, pt-BR 205/205, nav-test 150/150). Screenshots at 1280x720:
   `O:\gt4recomp-scratch\slice295-shots\general-pt-BR.png` and `graphics-pt-BR.png`. Owner test pending.

### TASK-115 — Game recognition (Windows, GPU drivers, Discord) and Discord Rich Presence
Planned (owner, 2026-10-24) as a future plan (maybe 1.3.1, maybe later; the owner decides when). Goal: Windows, the AMD and NVIDIA drivers and Discord treat GT4Recomp
as a game, so Game Mode, AMD Instant Replay / NVIDIA ShadowPlay and Discord's "Playing" status work out of the box.
Facts to start from (main agent, to verify): the picture is drawn by `gt4-gs.exe` (GSdx) into a child window of
`gt4.exe`, so a driver or overlay sees the 3D work in `gt4-gs.exe` and the window in `gt4.exe`; RivaTuner already
hooks it (owner screenshots). Windows decides "game" from its own list, a fullscreen 3D swap chain, or the player's
Game Bar choice "Remember this is a game" (stored per user in `HKCU\System\GameConfigStore`); there is no
manifest flag. AMD Adrenalin and NVIDIA App detect games from their own databases and 3D activity and let players
add an executable by hand; their Instant Replay can also record the desktop.
Work:
1. Identity: version resources on `gt4.exe`, `gt4-spec2.exe` and `gt4-gs.exe` (ProductName "Gran Turismo 4" /
   "Gran Turismo 4 Spec II", CompanyName "GT4Recomp", FileDescription, icon), window title the game name.
2. Measure on the owner's machines (AMD Adrenalin, NVIDIA App on the RTX laptop, Windows Game Bar) whether each one
   detects the game today, in fullscreen and windowed, and what changes it (fullscreen mode, flip-model swap chain in
   gt4-gs, the identity above). Never write GameConfigStore or driver profiles behind the player's back; if a manual
   step is needed, a short in-launcher guide ("Add GT4Recomp to AMD Adrenalin / NVIDIA App / Game Bar").
3. Discord Rich Presence (owner: 1.3.1): the launcher (it stays alive in the tray while the game runs) talks to the
   local Discord client over its IPC pipe with a permissively licensed crate (MIT/Apache, never GPL), a GT4Recomp
   Discord application id (owner creates it), and shows "Gran Turismo 4" or "Gran Turismo 4 Spec II", elapsed time
   and the GT4Recomp logo; a Settings > General switch (on by default), off when Discord is not running, no data
   sent anywhere else. In-game details (mode, track, car) need guest-memory reads: a later step.
Acceptance: the owner sees each feature work on his machines; unit tests for the presence payload and the switch.

### TASK-117 — Performance mode, part 2: no light flares at all in Performance Mode (sun included)
**Done in rc6 as a host change; owner visual test passed, F8 pending (2026-10-25).** Two forms were tried: the derived-ELF guest patch (built, parked below: a runtime-switched guest patch cannot pass its identity gate) and the host-side zero table, which ships. The owner tested v2.00 and Spec II, night and day: flares gone, no other visual difference, and the glows no longer draw over the virtual rear-view mirror (a known GT4/GSdx artefact). Partial success until his F8 on a free machine confirms the frame times. Owner decision (2026-10-25 evening): Performance Mode removes every flare, the sun included, because weaker PCs lose frames on the Nordschleife only when driving into the sun.
History (2026-10-24): planned for 1.3.0-rc6 together with TASK-116 step 1, because both need a new GT4 v2.00 game
build (one rebuild, one set of pins).
Idea source: the owner found https://github.com/nobz226/gt-pcsx2-patches (PCSX2 pnach files for GT4 USA, CRC
77E61C8A, and GT3 JP). That repository has **no license**, so it is a reference only: we never copy its stub code or
words; we write our own guest code from our own disassembly and credit the idea in the release notes. Its README
explains the mechanism: each frame the game fills two flare queues (track lights, car lights; limit 100 each) through
one shared visibility test; each queued light draws a glow and, when close, a second larger halo.
Facts checked against our reconstructed ELF (`private/reconstructed/SCUS_973.28.elf`, 2026-10-24, Confirmed):
- 0x3B7BA8 `slti a2, a2, 0x64` (track-light queue limit 100);
- visibility test at 0x3B7C98 (prologue `addiu sp,sp,-16`), its `jr ra` at 0x3B7DCC; two callers: `jal` at 0x3B7BE4
  (track lights) and 0x3B85C0 (car lights);
- car-light flare draw call `jal 0x498198` at 0x3B8BA0; flare draw routine 0x498198, halo decision near 0x49822C;
- sun flare draw call `jal 0x4985F8` at 0x3B89E0;
- zero padding at 0x495580 and 0x495C40..0x495CFF (room for helper code).
Why it should help George V (hypothesis to measure): with Performance mode (no readbacks) the owner measured
59.8 VBlanks/s but 9% late frames, the dips to about 55 FPS in the street-lamp section. Fewer and simpler flares mean
less EE work to queue them, less VU1 work (shorter VU1 drain before the per-frame readback) and fewer GS sprites.
What Performance mode would change (fixed values, the patch author's tested set; no player tuning in rc6):
1. No halo: each flare draws its glow only (glow size unchanged).
2. Distance cull: track lights beyond 60 units and car lights beyond 40 units are not queued.
3. Light caps: at most 12 track lights and 12 car lights queued per frame (with the distance cull, so no flicker).
4. Far car-light thinning: cars beyond 50 units draw about half their light pairs, always both lights of a pair.
Left out: sun flare at half size (tiny gain, visible change) and the "GT3 chase camera" (not performance; it changes
the camera feel).
Design:
- The game executable keeps one code path for both modes. A derived v2.00 ELF (the same mechanism as the Spec II
  boot patches, `scripts/spec2_patches.py`) places our helper routines in the zero padding and redirects the four
  sites above to them. Each helper first reads one guest word, the flare-limits switch, which the host writes at boot
  from Performance mode (new env var from the launcher, for example `GT4_FLARE_LIMITS=1`); with the switch at 0 the
  helper runs exactly the original instruction, so Performance mode off renders as today.
- Direct `jal` calls are compiled as direct C++ calls (only virtual calls pass through `ps2::dispatch`), so a host
  hook cannot catch them; the redirect has to be in the ELF before translation. Confirm this in step 1.
- Game logic never changes: the flares are presentation only. Confirm that nothing the game reads back for gameplay
  (physics, AI, timing, saves, replays) depends on the flare queues or the halo (step 1, with gt4-decomp names).
- Spec II uses different code: these addresses do not apply. Spec II gets the same feature later from its own
  disassembly (a follow-up step), not in rc6.
Steps:
1. Study (read-only): map the visibility test, both queue fills, the flare draw and halo decision, and the car-lamp
   object field used for pair thinning, with gt4-decomp names; list every reader of the queues; confirm the padding is
   never written at run time; confirm whether the per-frame 2 KB readback is the flare occlusion test (shared with
   TASK-116 step 2).
2. Derived ELF: our helpers and redirects, a pinned patch table, a verify mode (like the Spec II patch tool), Python
   tests for every word.
3. Host switch: the guest word written once at boot from `GT4_FLARE_LIMITS`; CTest fixture; the launcher sends it
   when Performance mode is on (GT4 v2.00 only).
4. Identity: with the switch off, the headless frame comparison matches today's v2.00 build (Nordschleife TT and a
   night scene); with it on, no new halts or interpreter fallbacks.
5. New v2.00 game build (with TASK-116 step 1), new `docs/inputs` pins, launcher manifest.
6. Owner test: George V Paris night race and another night track, F7/F8 with Performance mode on and off; visual check
   (far lights appearing, flicker, car lights).
Acceptance: Performance mode on holds 60 FPS on George V Paris (late frames clearly below 9%, F8 report), no visible
flicker; Performance mode off is unchanged (identity checks); automated tests for the patch table and the switch.

#### rc6 plan after the step 1 studies (2026-10-24; reports in the main agent's scratchpad, key words re-checked)
Study facts (C = confirmed in the ELF or code, H = high confidence):
- Track queue fill 0x3B7B70 (count flare+0x1848, limit `slti` 0x3B7BA8), car queue fill 0x3B8530 (count +0xA34,
  limit `slti` 0x3B8568 = 0x28620064); flare object at RacePS2Base+0xDA0 (C).
- Visibility test 0x3B7C98 already takes a per-type maximum view depth in f13 (default 1000.0): the distance cull is
  only a smaller f13 (C). Only two callers, 0x3B7BE4 (track) and 0x3B85C0 (car) (C).
- Flare draw 0x498198, called only from 0x3B8BA0 and 0x3B8CE8; the halo flag s5 is set at 0x49823C (0x24150001) when
  f18 > 0, f19 > 0 and depth > 1.0; zeroing f19 before the call removes only the halo (C).
- **The per-frame 2 KB readback is the flare occlusion test** (H, matches docs/GS-READBACK-STUDY.md): each queued
  light renders a small depth-tested sample reduced to one pixel of a 64x8 table, read back to flare+0x104 and fed
  to VU1 as the glow brightness; the EE never reads it. Culling cuts the sampling, VU1 and GS work but keeps the
  readback while any light is queued.
- Free code space: zero run 0x495574..0x495FFF (2,700 bytes, unreferenced, ends at the function at 0x496000) (C/H).
- Direct `jal` is compiled as a direct C++ call; changed ELF words are translated with no seed change; new entries in
  the hole renumber `unit_00E5` and regenerate its callers plus 00A4, 00A5, 00AD, 00AE (C). The aspect derived-ELF
  tool (`aspect_elf_patch.py`) needs a "redirect + code block" row type.
rc6 scope (fixed values, no player tuning):
1. TASK-116 step 1 (no VU1 drain on the flare readback in Performance mode), native only, no new ELF:
   - the game reads HWDownloadMode from the INI named by `GT4_GS_CONFIG` (one source of truth, no launcher change);
     A/B kill switch `GT4_GS_READBACK_LOCAL=0` (added to `GS_STREAM_NAMES` in external_runtime_probe.py);
   - only the 128-qword (64x8) flare readback in mode 2 is answered locally (with the previous flare reply, see
     step 3); car icons, loading picture,
     thumbnail and photo readbacks keep today's drain and round trip;
   - the READ_FIFO record is still posted in journal order without waiting (new journal entry kind, no-wait IPC post),
     so the GS stream hash is unchanged; `sync("download")` becomes lazy (the real drain runs only for non-flare reads);
   - CTest fixtures for the client, IPC, journal order and the switch; F8 text counter for local answers.
2. TASK-117 flare limits, derived v2.00 ELF: four `jal` redirects (0x3B7BE4, 0x3B85C0 to our track/car helpers;
   0x3B8BA0, 0x3B8CE8 to our halo helper) and about 350 bytes of our own helpers at 0x495580; the queue limits of 100
   stay. Parameters in a host-written guest block next to the HUD block (0x7F10 flags, 0x7F14 track depth 60,
   0x7F18 car depth 40, 0x7F20 / 0x7F24 caps 12), written at boot and every VBlank from `GT4_FLARE_LIMITS` when
   Performance mode is on; zero means every helper jumps straight to the original routine.
   Far car-light pair thinning is left out of rc6: its left/right key needs a lamp-table dump first, and with the
   car cull at 40 units a thinning start at 50 never acts.
3. Gate P0 before enabling either: `GT4_GS_READBACK_LOG=1` logs each readback (size, destination, hash of the reply)
   in Accurate and Performance mode on George V night, a day race, menus, photo and a replay, v2.00 and Spec II.
   Owner (2026-10-24): in Performance mode the street-light glows show normally on George V, so gt4-gs does NOT
   answer with zeros there and a local zero answer would hide the flares. Local answer = the previous flare reply:
   the no-wait READ_FIFO's reply is collected when it completes and copied into the next frame's flare readback
   (one frame late; the first frame of a race waits as today). P0 shows whether the reply is constant (then the
   result is byte-identical to rc5) or varies (then one frame late, judged by the owner on screen).
4. One new v2.00 game build (both changes) and a Spec II build (step 1 only), new `docs/inputs` pins, launcher
   manifest, rc6 notes in both languages, language gate, draft upload.
Identity: Performance mode off renders as rc5 (headless frame dumps, Nordschleife TT and a night race, GS stream hash
equal, MTVU and VU1 verify clean, hole hash unchanged at the first race frame and at exit, no "no translated code").
Owner test: George V Paris night F7/F8 with Performance mode on (target: mtvu download and readback wait near 0,
60 FPS without the dips), flare look on and off.



#### Scope change (owner, 2026-10-25 evening): cut every flare, the sun included
Performance Mode no longer reduces the flares: it removes them. No track lamps, no car lamps and no sun flare (weak PCs
lose FPS on the Nordschleife only when driving into the sun). With no light queued the game skips the 2 KiB sample, so
the per-frame readback, the VU1 drain, the round trip and the one-frame lag all disappear. Perceived on the owner's rc6
test build. Implementation: redirect the two lamp-queue fills (0x3B7BE4, 0x3B85C0) and the sun sample/draw (inside
0x3B8400 and `jal 0x4985F8` at 0x3B89E0) to helpers that return "not visible" with the switch on; the flare draw sites
need nothing because no entry is ever queued. The switch-off path must stay byte-identical to rc5: the derived ELF built
for the earlier "fewer flares" variant still diverges with the switch off, so the bisect below comes first. Full spec:
ai-memory `decisions/rc6-performance-mode-cuts-flares`.
#### rc6 delivery (2026-10-25): built, not shipped
Derived v2.00 ELF `SCUS_973.28.flare.elf` (sha256 `6c156b5f…`, 6,123,004 bytes) built by
`scripts/flare_limits_elf_patch.py produce` and pinned by that script (`check`, `verify`, `record-library`): the four
`jal` redirects (0x3B7BE4 track fill and 0x3B85C0 car fill → 0x4955A0 / 0x495600; 0x3B8BA0 and 0x3B8CE8 halo →
0x495580) plus our helpers in the zero padding, each ending in `j <original routine>` so `ra` (call + 8) is kept and
a zero flags word runs the original instruction in effect. Only caller-saved registers are touched (t0-t2, $f0, $f19).
The generated code calls the helpers from units 00AD (track fill) and 00AE (car fill and both halo sites), with the
delay slot unchanged; unit_00E5 holds the three helper entries (91-93) and unit_00E6 the flare routine. The accepted
seed set is unchanged (translator entries 138,815 against 138,813 for the original: the three new entries). Eight
units differ from the slice208 library: 008E, 008F, 00A6, 00A7 (the TASK-080 rows and the id renumbering), 00AD,
00AE, 00E5, 00E6. Library pin `docs/inputs/slice299-ee-libraries.json`, profile `flare` (CMake `GT4_EE_PROFILE=flare`).
Host: `native/flare_limits.*` writes the six-word block at 0x7F10 (flags 7, track depth 60, car depth 40, caps 12/12)
at every guest VBlank when `GT4_FLARE_LIMITS=1`, a zero block otherwise; CTest `native_flare_limits`. Launcher:
`launch.rs` sends `GT4_FLARE_LIMITS=1` for GT4 v2.00 (its ELF name) with Performance Mode on, never for Spec II, and
the direct start reads `HWDownloadMode = 2` from the config `gt4-gs.ini`; `launch_contract` and the standalone
fixture cover both. Tests: `tests/python/test_flare_limits_elf_patch.py` pins every helper word, the redirects, the
block offsets against `native/flare_limits.hpp`, the old words against the pinned ELF and the produced derivative.



#### Final implementation (2026-10-25, in rc6): the host answers the flare table as zeros
In Performance Mode the gs_stream client already answers the 128-qword (64x8) flare readback from the VU1 journal.
It now answers **zeros**: the VU1 scales every lamp, every headlight and the sun by its own pixel of that table
(`docs/GS-READBACK-STUDY.md` 1.1), so an all-zero table draws no flare, halo or sun glare. The READ_FIFO is still
queued, so the journal keeps deferring `sync("download")` exactly as in TASK-116 step 1; only the bytes the guest
receives change. Switch: `GT4_GS_HIDE_FLARES` (unset = on with the local answer, `0` = the previous reply). No guest
code and no launcher wiring, so Performance Mode off stays byte-identical to rc5 by construction; the sun needs no
separate path. The one-frame occlusion lag also disappears, because no flare is drawn at all.

Measured headless (night, 12,000 frames, 1x, `HWDownloadMode=2`, `GT4_GS_READBACK_LOG=1`): with hiding, 3,763 of 3,763
tables answered as zeros (probe `nz=0..0`), no readback wait, `mtvu download` 0.000 ms and the EE critical path
unchanged (5.87-6.52 ms against 6.00-6.44 ms with the previous reply); every dump before the race is byte-identical,
and the race dumps differ only on the lights and their glows. CTest: new `pcsx2-hidden` scenario; `pcsx2-local` now
pins `GT4_GS_HIDE_FLARES=0`.

Why the guest patch was dropped: the emulated EE clock is counted in instructions (`system.cpp` `now()` =
`g_base + (g_given - budget)`; VBlank and alarms are scheduled at guest cycles), so any runtime switch that adds
instructions moves where the VBlank lands in the guest frame. The derived ELF therefore diverged from rc5 on the
first frame that drew flares even with every helper taking the original path, and no placement or entry-id fix can
remove that. The derived ELF (`scripts/flare_limits_elf_patch.py`, the `flare` profile, `native/flare_limits.*`)
stays on the branch as a documented dead end; only a guest patch removes the GS-side 2 KiB download, which belongs to
TASK-116 step 2 (async download in gt4-gs, 1.3.1).

Owner test (2026-10-25, night): **visual acceptance passed** in v2.00 and Spec II, night and day: flares gone, no
other visual difference, and the glows stopped drawing over the virtual rear-view mirror (a known artefact in GT4
and in PCSX2 GSdx). Performance felt clearly better even with the machine busy compiling; **F8 metrics pending** on
a free machine. The build he tested stays as delivered until that run, so TASK-118 presets do not mix with its
numbers. Split screen keeps the headlight glow (constant path, see the study); the sun is hidden there too.

#### In progress (2026-10-26, owner switching agents): the four laptop fixes
The owner tested rc6 on an i5-7200U / Intel HD 620 and sent the sessions (`Z:` share). Four defects, each measured:

1. **The launcher cost ~20% CPU in the background** (six full-screen 72 s CSS animations kept running after Play;
   hiding the window does not stop WebView2). F8 proof: the game EE CPU itself went from 6-12 ms to 19.61/19.16 ms
   on that run. Fix: pause them while hidden/blurred/in game (body.is-idle) and keep WebView2 GPU acceleration on.
2. **A normal close was reported as a crash**: the launcher read `game.log` in the same instant the game wrote its last
   line (three sessions: `Crashed { reason: None, exit_code: Some(0) }` with a normal `[ee] stopped ... window closed`).
   Fix: wait for the shutdown line (25 ms poll, 1.5 s cap) and treat exit 0 without one as a normal close.
3. **Vulkan loses the device on the HD 620** at the showroom (issue #89; it reports Vulkan 1.3, so the automatic API
   rule picks it). Direct3D 11 runs fine; Direct3D 12 is clearly slower there. Fix: remember `graphics_api = d3d11`
   after a Vulkan device loss and say so.
4. **Render scale jumped 1x -> 4x in one notch** on the integrated GPU (100% = 1x by issue #89, then k x percent).
   Fix: additive steps, one multiplier per 25% from the 100% plan (integrated 1/2/3/4/5x), clamped 1..12.

Code state when the session ended (all in the worktree, `cargo test -p gt4l-core` green, `cargo check -p gt4l-tauri`
green with `GT4_SDL3_ROOT` set):
- `display.rs`: additive mapping + tests; `exit.rs`: `is_settled` + relaxed `classify` + test;
- `commands.rs`: `read_settled_log`, `PlayInputs.api`, the Vulkan->D3D11 memory; locale key
  `rust.game.gpu_lost_switched`; `gs-settings.toml`: `blending` hidden; `launch.rs`: preset writes blending
  Minimum (Performance Mode on) or High (off).
- **Still missing:** the UI pause (`app.css` body.is-idle + `app.js` game-state/visibilitychange/blur), and the
  portable launcher rebuild + owner/laptop copies. Full checklist: ai-memory `procedures/task-118-launcher-fixes`.

Performance Mode preset (supersedes the earlier list): `HWDownloadMode 2` + `accurate_blending_unit Minimum` only.
Trilinear, dithering, preloading, filtering and mipmapping stay at their accurate defaults: measured on the laptop,
turning them off changed nothing outside the noise and cost picture quality. Details in ai-memory
`gotchas/weak-laptop-ee-bound` and `decisions/rc6-laptop-launcher-fixes`.

### TASK‑118 — Graphics settings simplification (owner, 2026-10-25, 1.3.0-rc6)
Owner decision, recorded directly by the implementation session (no orchestrator round trip). The Advanced page of
Settings > Graphics is too technical and Performance Mode is only a readback switch today.

What changes:
1. **Performance Mode becomes a preset.** On: `HWDownloadMode` 2 (as today), `accurate_blending_unit` Basic,
   `TriFilter` off, `dithering_ps2` off, `TexturePreloadingLevel` Partial. Off: exactly today's values (blending
   High, trilinear automatic, dithering unscaled, preloading full). Blending accuracy leaves the Advanced page.
2. **Fixed values, rows removed:** deinterlace automatic, present filter smooth, integer scaling off, interlace offset
   off, anti-blur on, offsets/overscan off, TV shader none, anisotropic off, accurate alpha test off, AA1 off, ROV off,
   shade boost and its four sliders off, disable shader cache off, texture filtering PS2, mipmapping on.
3. **GS async leaves the launcher** and is always on (it was a debugging A/B switch and is the default).
4. **RC Settings area** (only in release-candidate builds, `gt4l_core::build_kind`): Release performance report
   (`performance_report`) and Verificar código VU compilado (`check_vu_code`). Final builds do not ship them.
5. **Kept for everyone, default off:** the PCSX2 performance overlay (OSD) and the "compiling shaders" notice.
6. **Removing a row never stops writing the key**: each removed row keeps a fixed value in `gt4-gs.ini`, otherwise an
   existing install keeps a stale value.

Rules: gameplay never changes (an icon, loading picture or flare may be lost; physics, timing, AI, saves and replays
may not). Every preset key needs a headless A/B on the F8 harness before it ships, and the owner tests in game.
6. **No "Automatic" choice for API or card** (owner, 2026-10-25): the launcher resolves both on the first start and
   stores the result, so the UI shows "Vulkan" and the player's card name directly; the player can change either one,
   and a card that disappears falls back to the rule's pick for that run and updates the stored value (never silently
   back to "Automatic"). Needs a settings migration that materialises a stored "auto".
7. **Render scale becomes one slider**: 100% by default, 50%..200%, no "6x" text and no resolution text beside it. It
   snaps to the integer GSdx multipliers 1x..12x and stores the multiplier (the ini keeps integers); the percentage is
   relative to the automatic plan, which stays the 100% baseline and must keep working from the monitor resolution.
   **Hard cap at 8K**: 12x is 7680x6144, so an 8K output tops out at 100% (12x).
8. **AA1 stays disabled by default** (owner, 2026-10-25): it does not join the anti-aliasing row and does not enter the
   Performance Mode preset unless a headless A/B plus the owner's eyes show a real GT4 gain.
Acceptance: the Advanced page holds only rows a player can reason about; Performance Mode on/off is the single
switch; no lost option changes gameplay; tests for the preset layer, the fixed rows and the RC-only gate.

### TASK-116 — George V Paris performance
Recorded (owner, 2026-10-24) on 1.3.0-rc5, Spec II (Arcade unlocks every track), night race on George V Paris,
GSdx Vulkan 6x, Performance Mode off, owner PC (Ryzen 7 5700X, RX 9060 XT). The owner calls it the heaviest and least
optimised track of the game. Measured on screen (RivaTuner): 43-52 FPS in the race (60 on the track-select screen),
frametime spikes to about 23 ms; GPU only 32-42% busy at about 2.5 GHz, CPU about 33% total, so one thread is the limit,
not the GPU. Hypotheses (unconfirmed): many street lamps with lens flares cause GS readbacks that make the game wait
for the GS process each frame (the C.S. Route 5 night capture already had 2,928 readbacks), or the EE / VU1 / GS-stream
thread is saturated by the scene. Next: the owner records an F7/F8 interval on this track (Graphics > Advanced >
Performance report), the F8 report lands in that session's `game.log`; compare with Nordschleife TT and a day race.
Applies to GT4 v2.00 too if the track behaves the same there.
Diagnosis (two owner F8 intervals, 99 s each, 2026-10-24; details in ai-memory notes/george-v-performance):
- Accurate readbacks: 55.3 VBlanks/s (92% game speed, 66 audio underruns); EE critical path 17.73 ms of 16.68;
  about one 2 KB readback per frame, and before it the EE drains the VU1 worker (5.65 ms/frame) and then waits for
  gt4-gs to render and download (5.28 ms/frame): a serial chain, so the GPU idles at 30-40%.
- No readbacks (Performance mode): 59.8 VBlanks/s, 99.7% speed, 9% late frames (the dips to ~55 FPS); the game still
  asks for the readback every frame and the EE still drains the VU1 worker first (5.52 ms/frame) although gt4-gs answers
  with no data (1.23 ms).
Plan for 1.3.1 (owner, 2026-10-24):
1. No VU1 drain when the readback returns nothing: with HWDownloadMode NoReadbacks the game exe answers the readback at
   once without waiting for the VU1 worker (stream order unchanged). Native change in the game's MTVU / gs_stream client
   (our MIT code): new v2.00 and Spec II builds, new pins, v2.00 identity checks; expected about 5.5 ms/frame less on
   George V, enough to hold 60 FPS with Performance mode.
2. Unsynchronized readbacks: gt4-gs 1.2.2 accepts HWDownloadMode Unsynchronized (fork allow-list and SHA256SUMS; ship
   with the OpenGL entry of TASK-107); the readback returns the previous frame's data without waiting, so the flares stay
   and react one frame late, and the game skips the VU1 drain as in step 1. Before shipping, confirm the 2 KB readback is
   the flare visibility test (game logic must never change) and offer it as a third Readbacks choice after an owner test.


#### rc6 delivery of step 1 (2026-10-25)
Performance mode answers only the 128-qword flare readback locally with the previous reply (one frame old, never a
zero table): `sync("download")` closes the VIF1 batch and returns (`download_deferred`), the client queues the
READ_FIFO behind the journal (`EntryKind::Readback`, `Session::post_read_fifo_nowait`) so the record keeps its stream
position, and the reply is collected one frame later by exact request id. Every other readback (car icons, loading
picture, thumbnail, photo) drains on demand (`mtvu::drain_download`) and waits, as does the first flare readback of a
scene. `GT4_GS_READBACK_LOCAL=0` restores the old behaviour; `GT4_GS_READBACK_LOG=1` logs every reply (P0 probe).
Night race headless 1x: blocking drains 5,026 → 29, readback wait total 4,596.6 / 2,329.1 → 46.1 ms, 4,997 of
4,998 flare replies from the cache, record counts equal to the pre-rc6 Performance-mode run; the Accurate run still
hashes `c091716e3cb40d79`. The owner F8 decides the gain: part of the old 5.5 ms "download" was the EE's own ring
writing (now `EE gs_drain` 2.2-4.2 ms/frame), so the net saving is about 2-3.5 ms/frame in the race stretch.

### TASK-100 — Spec II support (release 1.3.0)
Planned (owner, 2026-10-22) for release 1.3.0: Spec II ships before the wheels and before CoreGS 2.0 (owner, 2026-10-23), on GSdx. Scope as decision 0072:
the SCUS-97436 Public Beta lineage image with its own target executable, not a content merge into v2.00. Identity
study: docs/SPEC2-STUDY.md. Expected to be most of the work at the start; once one Spec II build runs, keeping it
working should be much cheaper. Work to map first:
- How Spec II's plugin injector puts its code and scripts into the game at start (where the injected MIPS code and
  data come from, when it runs, what it patches). Everything the EE runs must be compiled ahead of time (no JIT), so
  the injected code has to be captured and translated like the rest of the ELF, or its effects reproduced natively.
- A second EE pipeline: `SCUS_974.36` (no `CORE.GT4`) through the donor translator into its own pinned library,
  with its own manifests, coverage runs and "no translated code" hunt.
- Our own game-side hooks are bound to v2.00 addresses and must be remapped (decomp names where gt4-decomp helps):
  the 16:9 hold (TASK-079), the aspect/race camera and HUD hooks (TASK-080), the 480p race display (GT4_FORCE_480P),
  the license demo guard, the memory card store, the MTVU/VU1 AOT (Spec II VU1 microcode image and its keys), VU0
  AOT, the IOP IRX set, and every pinned constant in docs/inputs.
- Adhoc v7 scripts and the natives Spec II registers (menus, GT mode, `projects/%s/%s`), the `GT4_PREMIUM_US2560`
  spec database, `/sound_gt` and `carsound` paths: data the game reads, so they need no translation, but every native
  the scripts call must exist in the translated executable.
- Launcher: a second game target (disc identity check, setup, saves kept apart from v2.00), chosen per disc image.
Plan (2026-10-22): [docs/SPEC2-PLAN.md](docs/SPEC2-PLAN.md), subtasks S1 patch set and injector map, S2 derived
ELF, S3 target-aware host hooks (v2.00 byte-identical), S4 second-target build, S5 VU0/VU1, S6 IOP network modules,
S7 first boot, S8 re-targeted hooks, S9 launcher and installer. Worktree `C:\Antigravity\gt4-wt-100-spec2`; the
owner runs the long parts in OpenCode. The unmodified Public Beta ISO is wanted for S1 (exact diff) and S7a.
Progress (2026-10-22, S1 done): the owner supplied the Public Beta ISO; the community `.xdelta` decodes it to exactly
the owner's Spec II ISO (byte identity), so the beta is the confirmed base. The beta's encrypted `CORE.GT4` was
recovered (Salsa20/20, key `PolyphonyDigital`, CRC-32C trailer) and rebuilt into `private/slice271-spec2-beta-elf`
(ELF `2e5621ed…`). The diff against Spec II's `SCUS_974.36` gives the complete static patch set: **one word**
(`0x001001E8`, the trampoline into the plugin) plus the `.plugin` segment at `0x0068A480` (0x2CCC bytes) over
zero-filled data; every other byte is identical. The injector is mapped by executing its init on an EE-RAM image:
35 word stores (18 text, 17 data), unconditional (no variants; identical under stack/return-value perturbation).
Report [docs/SPEC2-PATCHES.md](docs/SPEC2-PATCHES.md), pins `docs/inputs/spec2-patches.json` and
`docs/inputs/spec2-beta.json`, tools `scripts/spec2_patches.py` and `scripts/spec2_beta_elf.py`,
`tests/python/test_spec2.py`. Open: S3 build/identity, then S5/S6, S2/S4.
Progress (2026-10-22, S3 done): `native/game_target.hpp` carries every guest address of the host hooks per target
(v2.00 keeps the release values; an absent address disables the hook and logs once; `GT4_TARGET=v200|spec2`). The
capsule overlay text is unchanged. CTest 42/42 (new `native_game_target`); Python 653 OK, 42 skipped and the known
release-pin distribution failure that any rebuilt executable has. Headless identity against the 1.2.3 build (slice268)
is exact in menus, day and night, dumps and GS stream hashes, on the final code state. Open: the new executable's
release pin set (release decision), S8 fills the Spec II table.
Progress (2026-10-22, S5/S6 mapped): VU1 decrypts to the v2.00 image (`ae6b6cb3…`), the AOT table carries over; VU0
differs (0x920 vs 0xA80, 1,050 bytes) and runs on the counted interpreter fallback. IOP: 35 files pinned with policy
(`inputs/spec2-iop.json`), 6 new, inherited from the beta lineage. Open: S7b for both (VU0 profile, offline hangs).
Progress (2026-10-22, S2 done): `scripts/spec2_elf_patch.py` applies the 35 injector stores to `SCUS_974.36` from the
ISO and writes `private/slice272-spec2-elf` (`10a794df…`, original `db08421b…`), with `--revert` reproducing the
original exactly. Pins `docs/inputs/spec2-iso.json` and `docs/inputs/spec2-elf.json`; the trampoline at 0x1001E8 stays
(documented: the init also calls the beta's boot initialiser and one more game function). Tests 21 in
`tests/python/test_spec2.py`. Open: S4 (plain-ELF translation, own library and executable) and S7b.
Progress (2026-10-22, S4 translation): `scripts/spec2_ee_generate.py` runs the pinned translator on the derived ELF:
346 units, 348 files, inventory `13354c95…`; 1,285,650 instructions, 17,080 functions, 148,756 entries, 2 rejected
seeds. Pin `docs/inputs/spec2-ee-generated.json`; output stays in scratch. Open: the Spec II library and executable
(own CMake profile) and S7b.
Progress (2026-10-22, S4 build done): profile `spec2` in CMake (`-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2`, output
`gt4vulkan-spec2.exe`); library `private/slice274-spec2-ee-producer/game-build/ps2game.lib` 77,369,746 bytes
(`5e483768…`), pin `docs/inputs/spec2-ee-libraries.json`. v2.00 untouched.
Progress (2026-10-22, S7b BLOCKED before the menus): the derived image boots headless with **no "no translated code"**
(600 and 4,200 frames). Two findings fixed on the way: the plugin PT_LOAD sits inside the data PT_LOAD, so the
translator read zeros for it (the derived ELF now splits the data segment around the plugin, image still identical);
and the plugin's own pointer tables are inside its executable segment, so 12 entry seeds were added. The game then
draws nothing (8 primitives against v2.00's 2,524 at the same frame), all EE threads wait, the IOP is idle. The
original unpatched image behaves identically, so our patches are not the cause. Next: semaphore trace, the DNAS310
IOP-reboot path, and an IOP module A/B.
Progress (2026-10-23, S7b unblocked: menus and a race): the stall was the FILEIO protocol generation. The Spec II disc
reboots the IOP with `DNAS310.IMG` instead of `IOPRP300.IMG`; the donor selected its asynchronous FILEIO server (the
one that answers `devctl`, method 23, with the SIF packet `0x80000011`) only for a path containing `IOPRP`, so the
game's fileio thread waited forever on its completion semaphore. The image needs no loading: DNAS310.IMG is the same
romdir as IOPRP300.IMG, with the same 16 Sony system modules in the same order, all served as HLE (pin
`docs/inputs/spec2-dnas310.json`, section in docs/IOP-MAP.md). Fix: capsule `private/slice276-spec2-iop-runtime`
(`scripts/prepare_perf_runtime.py --spec2-iop`, `scripts/spec2_iop_overlay.inc` + `native/spec2_iop.hpp`); v2.00
keeps its capsule. Headless: 4,200 menu frames (1.8 M primitives), the main menu, Arcade Mode, track and car
selection, and a 21,000-frame race at Costa di Amalfi (169.7 M primitives, HUD, lap timer), no "no translated code".
CTest 43/43 in the Spec II build (new `native_spec2_iop`); Python 668 tests: all pass with the Spec II build (51
skipped), only the expected release-pin failure with the v2.00 build. v2.00 identity against the 1.2.3 build: menus,
day and night dump and GS stream hashes all equal. Open: S8 (Spec II hook addresses), S9 (launcher, installer,
credits); the race used a scripted accelerator press, and GT mode still needs game data on the card.
Progress (2026-10-23, S7c PASS on GSdx): the harness gained target overrides (--exe/--elf/--iso/--press/--textures/--gs-sha256; defaults are the v2.00 gate values and the menus/day/night identity still equals slice268). Spec II runs on the shipping PCSX2 GS process: menus 4,200 frames, Costa di Amalfi race 21,000 frames (12 readbacks), night race at C.S. Route 5 (2,928 readbacks), 3x upscale, and the owner's HD HUD texture pack (with [Folders] Textures + LoadTextureReplacements, needs gt4-gs 1.2.1; the repo pin is 1.0.1). The GT4 display crop is a fixed gt4-gs constant ({0,0,1,2}) and logs for both games; gt4-gs has no SCUS-97436 GameDB entry and the game still announces SCUS-97328 (recorded for S8/S9).
Progress (2026-10-23, S8 done for the main hooks): `native/game_target.hpp` kSpec2Hooks now carries the Spec II option chain (owner 0x664EDC, option +0x680, flag +0x14, record +0x24 44 bytes at width +0xC/height +0x10, presets 0x664E20/50/80/B0, published wide word 0x6655A0, race display index +0x1718), RaceBase::initMonitor 0x26BDB0 with camera return 0x26BE60, RaceDisplay::render 0x1C5040 and the HUD matrix at +0x70. Found from the v2.00 code shapes in the derived ELF and confirmed with a runtime RAM dump. New fixture native_spec2_hooks asserts every value and the write rules in synthetic RAM; native_game_target asserts the filled table. Headless GSdx race at 21:9 shows the camera matrix scaled, the first RaceDisplay::render, the HUD canvas (left -240 right 880) and the fill policy; the Spec II build now uses the slice262 license guard capsule for the aspect overlay. The 16:9 hold finds the settled owner 0x9BD690 but writes nothing because the game itself boots 16:9; the 480p hold runs in window.cpp's VBlank, so headless runs only cover it through the fixture. Not mapped (rows 0, hook logs once): display request struct (photo mode), license demo guard (feature exists, loaders unmapped), shape observer. CTest 44/44 in the Spec II build; Python 671 tests clean on the Spec II build (51 skipped) and only the expected release-pin failure on v2.00; v2.00 identity against slice268 exact (menus/day/night dumps and stream hashes).
Progress (2026-10-23, S8b license demo guard mapped): kSpec2Hooks now carries the Spec II license values: loadDemoInputs 0x2012C8 (RaceSolitaire vtable 0x6B3130 +0x510, the same virtual v2.00 has at +0x4D8), loader entries 0x10CA18/0x10CBA8 with return labels 0x10CB54/0x10CCE4 (each loader calls the license/%s helper 0x1A8BE8 and makes one vtable call), Manager::load 0x207AA0, CRC-32 table 0x731DB0, Manager add 0x1A24 and body pointer shift 0x80. The cipher is the same (0x459E90, FT at 0x667048, T8 at 0x731C98); the Spec II volume's 256 replay/license demos are the retail files (171 shared with v2.00, 170 bodies byte-identical) and CRC-confirmed plaintexts keep the single-Recorder stream at body+0x1AA4, so the layout and the CRC rule are unchanged and the code's 0x1A24 add applies to a pointer 0x80 into the body (High confidence; not observed at runtime). Guard: two table fields (license_manager_add, license_body_shift), the overlay evaluates the header body and writes n=0xFFFFFFFF at a1+manager_add, static assert add+shift == 0x1AA4; fresh capsule private/slice277-license-guard-runtime (slice262 profile) and pin docs/inputs/slice277-license-guard-runtime.json, CMake accepts slice27x; v2.00 keeps slice262. Oracle on the Spec II set with our FPU (chop/chop): 0/256 CRC passes, 114 sane layouts, 142 demos guarded (other div models 130-152). Headless license test not reachable (GT mode needs game data; the seed cards have none), owner run remains the gate. CTest 44/44 and Python 675 OK (51 skipped) on the Spec II build, including the overlay harness compiled for GT4_TARGET_SPEC2; v2.00 identity against slice268 exact in menus/day/night (dumps and stream hashes).
Progress (2026-10-23, S8c Options-menu halt fixed): the owner test halted twice opening Options at 0x3CA690 (ra 0x50D1B0). The callback is built by the Options page (lui s0,0x003D at 0x3CC8EC + addiu s0,s0,-22896 53 instructions later) and stored in the descriptor; the translator's luipair window is 3 instructions and no dataword points at it. New scripts/spec2_entry_seeds.py seeds the missing classes (wide luipair window 64, stack-frame fallthrough, executable-segment literal code entries): 1,299 seed delta, regenerated 346 units, 1,332,727 instructions, 18,597 functions, 153,474 entries (inventory 2d8c92a4.), new library private/slice278-spec2-ee-producer/game-build/ps2game.lib (80,229,124 bytes, 9a94c3ed.), pins spec2-ee-libraries/generated updated, CMake accepts only slice278 for the spec2 profile. Sweep after the reseed: no real missing entries in the luipair/fallthrough/dataword/executable classes (only plugin strings/zeros). Headless: the old exe reproduces the halt after 11,159 frames; the new exe O:\gt4recomp-scratch\spec2-s7d-build\build\gt4vulkan-spec2.exe (104,475,136 bytes, 301bbb4a.) opens Options plus every submenu (Race, Replay, Screen 16:9, Picture Quality, Sound, Arcade Mode, LAN Connection Settings, Photo Lab), Replay Theater, Save Game, Credits, GT Mode and the Arcade menu with no halt; the harness gained --card. A license demo was not reached headless (the mod's Spec II Settings dialog blocks the GT map), so the [license-guard] confirmation stays with the owner. CTest 44/44, Python 678 OK (51 skipped), v2.00 identity against slice268 unchanged.
Progress (2026-10-23, 1.3.0 integration candidate, not published): merged origin/main db5c559 and origin/task/100-spec2 9206af5 (S8c) into task/101-multi-game (63c110e, b8a6b5f); resolved TASKS/BUILD/journal conflicts keeping the implemented TASK-102/103 rows and CRLF. Version 1.3.0 in launcher/Cargo.toml, Cargo.lock workspace crates, tauri.conf.json and the installer default. Release notes written: docs/release-notes/1.3.0.md, launcher 1.3.0 en-US and pt-BR, with the Spec II credit list (owner decision: no permission needed) and "GT4Recomp ships no Spec II files". New game builds in fresh slices: v2.00 private/slice279-build/build/gt4vulkan.exe (97,377,792; 9dccb5a4.), pin slice279-pcsx2.json; Spec II private/slice280-build/build/gt4vulkan-spec2.exe (104,612,352; 174e7adf.), pin slice280-spec2.json (slice276 IOP + slice277 guard + slice278 library); gt4-gs 1.2.1 package slice281; PERF_PROFILES pcsx2 -> slice279 (slice268 kept as pcsx2-slice268), SPEC2_RELEASE -> slice280. GAME_DIR slice282-dist-130 (both exes); launcher release 69530dfa. and portable a1a0ed0d.; installer GT4Recomp-Setup-1.3.0.exe 51,373,344 / d1569b35. (7-Zip extraction: 24 files match their sources, 0 differences); portable zip 83,032,500 / bfa3a08a. with SHA256SUMS. Checks: CTest 45/45 both builds, Python 694 OK (42/56 skipped), cargo workspace clean, focus en-US 151/151 + pt-BR 158/158 + nav 150/150, Rust Spec II derivation byte-identical to the script, v2.00 identity vs slice268 exact in menus/day/night. Flaky migration test cause: a scanner/indexer can hold a freshly written file, so the rename failed transiently; the real rename now retries 5 times (20-100 ms apart) and the policy has a test. Not done: owner tests, no PR/merge/tag/release.
S9 recorded (2026-10-23, not started): Spec II ships before CoreGS 2.0 on GSdx with upscaling and texture packs; the launcher keeps ONE shared settings set and only the game executable, the disc and the save folder differ per game; texture packs use the same Textures folder and settings, no new option; the planned release notes are two files (en-US and pt-BR) per AGENTS.md. GT mode needs game data on the card: the first boot creates it (decline -> 'GT mode is not playable'); plan the fresh Spec II card in normal play and keep its save separate from v2.00's. Open: launcher disc identity/prepared layout without CORE.GT4, second exe name, chooser, the Logo (credits are decided: no permission needed, credit by name) and the GS handshake serial decision (still SCUS-97328; gt4-gs has no SCUS-97436 GameDB entry).

### TASK-099 — Patch notes panel on the launcher home screen
Planned (owner, 2026-10-22) for release 1.2.4. The home screen keeps the logo, Play and Settings on the left; a new
block on the right shows the patch notes of the installed version, rendered from Markdown, in a light, minimal
glassmorphism card (translucent background with a backdrop blur, thin border, soft shadow) that matches the current
dark theme.

Plan:
- Source: the release notes the project already writes, `docs/release-notes/<version>.md` (the same text as the
  GitHub release body). The launcher build embeds the file of its own version (`include_str!` from the version in
  `Cargo.toml`, with a build check that the file exists), so it works offline, in the installer and the portable
  build (TASK-098), and can never be missing or edited by accident. A file beside the launcher is not needed.
- When an update is available, the panel can also show the new version's notes, read from the GitHub release body
  that the update check already downloads (`releases.rs`), with the update button next to them. Optional, after
  the basic panel.
- Rendering: a small Markdown subset (headings, paragraphs, bold/italic, inline code, lists, links, tables if
  cheap) converted to HTML in Rust with an MIT crate (for example pulldown-cmark), raw HTML disabled and the output
  escaped; links open in the default browser through the existing opener, never inside the web view. A link at the
  bottom: "All release notes on GitHub".
- UI (`ui/js/pages/home.js`, `ui/css/app.css`): the right-side card scrolls on its own; on narrow windows it moves
  under the buttons or hides behind a "What's new" button; keyboard and gamepad focus can reach and scroll it; the
  WebView2 safe mode (no GPU) falls back to a plain translucent background without blur.
- Release notes style: start each file with a short "Highlights" list so the panel reads well; the long sections
  stay below.
- Tests: Rust tests for the Markdown conversion (escaping, links, no raw HTML) and the embedded version; the UI mock
  shows the panel with real notes; the owner checks the look.

### TASK-098 — Portable build (zip) beside the installer
**Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test; release 1.3.0** (moved from 1.2.4 with
TASK-097, on top of the multi-game layout of TASK-101). Planned by the owner on 2026-10-22. Two launcher builds from one source: the
installer build (as today) and a portable build shipped as a zip. The portable copy keeps everything in its own
folder, so it can be carried to another PC as one folder. No path is hardcoded: every folder is found from the
folder of `GT4Recomp.exe`.

Layout of the portable folder:
- `GT4Recomp.exe` (portable build), `SDL3.dll`, `game\` (game executable, shaders, runtimes), licences.
- `user\`: what the installer build keeps in `%APPDATA%\GT4Recomp`: `config\settings.toml`, `controls.toml`,
  `Saves`, `Replays`, `backups`. Shipped in the zip (empty).
- `data\`: as in the installer build: disc copy, prepared files, `gs\` (gt4-gs versions and caches), runtime card,
  sessions, logs, `Textures`.

Map of the work:
- Build kind: a Cargo feature (`portable`) gives a compile-time build kind; `build-release.ps1` builds both
  (`-Portable`); Settings > About shows "Portable".
- Paths (`gt4l-core/src/paths.rs`, `resolve_layout`): in the portable build `UserDirs` = `<exe dir>\user` (a new
  `UserRootSource::Portable`) and `DataDirs` = `<exe dir>\data`, with no read layers and no `%LOCALAPPDATA%` write
  fallback: a folder that cannot be written (for example a copy inside Program Files) gives a clear message to move
  the folder somewhere writable. `UserDirs::new` already builds the standard layout under one root.
- No registry and no system places in the portable build: no `InstallDir`/`PreviousDataDir` reads
  (`relocation.rs` `previous_data_for`), no 1.2.0 legacy migration from `%LOCALAPPDATA%` and Documents
  (`relocate.rs`; an optional "import saves from the installed GT4Recomp" button can come later), no
  `clean_old_install`, no Start-menu or desktop shortcuts, no uninstall entry.
- Self-update for the portable build (`updates.rs`, `releases.rs`): the launcher stream gets a second asset,
  `GT4Recomp-<version>-portable.zip`, listed in the release `SHA256SUMS`. The portable launcher downloads it, checks
  it, extracts it to `<exe dir>\.update-staging`, renames the running `GT4Recomp.exe` aside (Windows allows renaming
  a running exe), swaps the program files in (never `user\` or `data\`), restarts itself, and removes the old copy at
  the next start; a failure rolls back as the installer's swap does (TASK-091). No administrator prompt.
- gt4-gs: unchanged; it is downloaded into `data\gs` as today (portable already; check its caches stay in `data\gs`).
- Single instance: the lock is per folder, so two portable copies (or a portable and the installed one) on one PC do
  not block each other but one copy cannot start twice.
- WebView2: the portable zip cannot install it; the TASK-091 detection message (with the download link) covers it.
- Game executable (TASK-097): one game build for both. When it starts on its own it reads the settings from
  `<install>\user\config` if that folder exists, else from `%APPDATA%\GT4Recomp\config`; the installer never creates
  `user\`.
- Release: `scripts/make_distribution.py` or a new script makes the zip (with the empty `user\` and `data\`
  folders) from the same files as the installer; release notes and README explain both downloads.
- Tests: `cargo test --workspace` for both builds (`--features portable`); path resolution, no registry use, the
  portable self-update swap and rollback with an injected file system, the per-folder lock.

Result (slice 278, 2026-10-23): `cargo test --workspace` 431 passed in each build (the installer build and
`--features portable`), 0 failed. Python: `tests/python/test_make_portable_zip.py` (7 tests, fixtures only).
What was built, and the decisions behind it:
- `gt4l_core::build_kind` (`BUILD`, `BuildKind::label`): the Cargo feature `portable` of `gt4l-core`, forwarded by
  `gt4l-tauri`. Logic takes the kind as a value (`LayoutInputs::portable`, `relocation::prepare_as(portable)`), so both
  behaviours are tested in either build. Settings > About shows "Build: Portable" (`about_info.build`).
- Paths: `resolve_layout` with `portable` gives `UserDirs` = `<exe dir>\user` (`UserRootSource::Portable`) and `DataDirs` =
  `<exe dir>\data`, the per-game layout inside both, no read layers, no `%LOCALAPPDATA%` fallback: an unwritable folder
  gives "Move the GT4Recomp folder to a place you can write to" (a native message box, because no window exists yet).
  The test overrides (`GT4RECOMP_DATA_DIR`, `GT4RECOMP_USER_DIR`) still win.
- Nothing in the registry or system folders: `prepare_as(portable)` reads no `InstallDir`/`PreviousDataDir`, no Documents
  folder, runs no 1.2.0 or 1.2.x migration; `clean_old_install_as(portable)` does nothing; the launcher creates no
  shortcut and no uninstall entry (only the NSIS installer does). The WebView2 profile goes to `data\launcher-webview`
  (`appDirectoriesOverride`, Tauri 2.12) instead of `%LOCALAPPDATA%\io.github.moesuito.gt4recomp`. The only registry read
  left is the existing read-only WebView2 runtime detection (TASK-091), which the portable zip cannot replace.
- Self-update (`gt4l_core::portable_update`, injected `UpdateFs`): `GT4Recomp-<version>-portable.zip` + `SHA256SUMS` ->
  `.update-download\` (SHA-256 checked while downloading) -> extracted into `.update-staging\` (zip-slip, size and entry
  limits) -> each top-level item except `user\`, `data\` and the update's own folders is renamed into `.update-old\` and the
  staged item renamed in (the running `GT4Recomp.exe` is renamed aside, which Windows allows) -> the new exe starts with
  `--after-update` (waits for the old one to exit) and the old one exits. A failed rename undoes every step; a marker
  `.update-swapping` survives a crash and the next start restores `.update-old\`; a finished update's `.update-old\` is removed
  at the next start (retried for 20 s in the background while the old launcher exits). No administrator prompt.
- Single instance: `Local\GT4Recomp.Launcher.<key of the exe folder>` in the portable build, so two copies never block each
  other and one copy cannot start twice; the installer build keeps `Local\GT4Recomp.Launcher`. Known limit: a second start of
  the same folder brings forward the window titled "GT4Recomp", which may be another copy's window when both run.
- `scripts/make_portable_zip.py` makes the zip from the same files the installer stages (`GT4Recomp.exe`, `SDL3.dll`,
  `NOTICE.md`, `LICENSE.txt`, `game\`) plus empty `user\` and `data\` and a `PORTABLE.txt`, refuses game data in the game
  folder, is deterministic and appends the zip's line to `SHA256SUMS`. `launcher\tools\build-release.ps1 -Portable` builds
  the portable launcher into `<target dir>-portable`. No release zip was built (owner rule); the script is tested on fixtures.
- Not done on purpose: an "import saves from the installed GT4Recomp" button (later); the game executable reading its
  settings from `<install>\user\config` is part of TASK-097 below.

### TASK-097 — Self-sufficient game executable
**Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test; release 1.3.0** (moved from 1.2.4). Planned
by the owner on 2026-10-22, after the 1.2.3 hotfix. Principle: the game runs on its own with the
same defaults every time; the launcher is only an easier way to start it and to manage settings. Today a double click
on gt4vulkan.exe does not start the game: the launcher passes about 14 arguments and variables (ELF, disc, card,
window; GT4_RENDERER, GT4_GS_PROCESS(_SHA256), GT4_GS_CONFIG; GT4_FORCE_480P, GT4_FORCE_WIDESCREEN,
GT4_DISPLAY_ASPECT; GT4_FULLSCREEN, GT4_PRESENT_MODE, GT4_MONITOR, GT4_OUTPUT_SIZE, ...). Plan:
- Built-in defaults: GSdx (pcsx2 renderer), 480p race display, 16:9 hold with the automatic ultrawide correction,
  fullscreen, vsync. The variables stay as developer/test overrides; test harnesses and pins that assume today's
  defaults set them explicitly.
- Paths from the exe folder (`<install>\data`): prepared ELF, disc copy, runtime card, the gt4-gs version in use
  (`data\gs\current.txt`), checked as the launcher checks them.
- Settings: the launcher writes the player's choices (monitor, resolution, GSdx options, the gt4-gs.ini) to
  `%APPDATA%\GT4Recomp\config`; the game reads them at start, so a direct start honours them.
- Missing pieces (no prepared files, no GSdx): a clear message that says to open the launcher once.
- New game slice with the frame identity checks, launcher changes, tests.

Design (slice 278, 2026-10-23; written before the code, release 1.3.0 on branch `task/101-multi-game`):
- **Two ways to start the game.** *Explicit mode* is the way the launcher, the harnesses and the pins start it today: the
  first argument is the ELF. It runs exactly the old code path: no built-in default, no config file, no path search. This is
  what keeps v2.00 frames byte-identical and what keeps every test and pin working; variables and arguments are the test
  overrides. *Standalone mode* is a start without an ELF argument (a double click, or only `--` options such as `--frames 600`
  or `--fullscreen`): the game builds the old command line and environment itself, from the rules below.
- **Where the code lives.** A new `native/game_main.cpp` provides `main`; the pinned donor `main.cpp` (port `slice200`) is
  compiled unchanged with `main` renamed to `gt4_donor_main` (a compile definition on that one source), and `game_main`
  calls it with the argument vector it built. The pure rules (paths, defaults, config parsing, the command line) are in
  `native/standalone.cpp/.hpp` with no Windows calls, tested by the CTest fixture `native_standalone`.
- **Order of every value:** explicit argument or environment variable (wins) > the launcher's config file > the built-in
  default. An empty variable counts as unset.
- **Built-in defaults:** renderer GSdx (`GT4_RENDERER=pcsx2`, `PS2_GS=gpu`), race display 480p (`GT4_FORCE_480P=1`), 16:9 hold
  (`GT4_FORCE_WIDESCREEN=1`), automatic ultrawide (`GT4_DISPLAY_ASPECT=auto`), fullscreen (`GT4_FULLSCREEN=1`, `--window`),
  vsync (`GT4_PRESENT_MODE=fifo`), NTSC, the monitor the window opens on and its desktop size (no `GT4_MONITOR`).
- **Paths from the executable's own folder:** the game must run from `<install>\game\gt4vulkan*.exe`; the game id follows the file
  name (`gt4vulkan.exe` -> `gt4`, `gt4vulkan-spec2.exe` -> `spec2`) and the files are
  `<install>\data\games\<id>\prepared\<ELF>`, the disc copy named by the setup receipt `prepare.json` (`image.file`; its size
  and modified time must equal the receipt, the same cheap check the launcher makes at every Play, and the prepared ELF size
  must equal `elf.size_bytes`), and `...\runtime\card.ps2` (a missing card is a blank card, as today). When
  `<install>\data` has no prepared files, `%LOCALAPPDATA%\GT4Recomp` is tried too (the launcher's write fallback for a
  read-only data folder). The renderer is `<data>\gs\current.txt` -> `<data>\gs\<version>\gt4-gs.exe`, whose SHA-256 must equal the
  `sha256.txt` recorded at install (`gs_release::installed`); that recorded hash is passed as `GT4_GS_PROCESS_SHA256`, so the game's
  existing pin check does the hashing. The game takes the same exclusive lock as the launcher (`launcher.lock` beside the card),
  so a direct start and a launcher start never share a card.
- **Session:** `<data>\sessions\standalone-<unix seconds>`; it becomes the working directory, `misses.txt` goes there, and
  stdout/stderr go to `game.log` there (the console is released). The newest 20 `standalone-*` folders are kept.
- **Settings:** the launcher writes `game.ini` (key=value) and `gt4-gs.ini` (the session ini it renders today) into the user
  config folder: `<install>\user\config` when `<install>\user` exists (portable build), else `%APPDATA%\GT4Recomp\config`. It does so
  at start-up, after every settings change and before every Play. `game.ini` keys: `fullscreen`, `present_mode`, `monitor`,
  `output_size`, `async_gs`, `performance_report`, `check_vu_code`. Each key becomes the variable the launcher would have set
  (`GT4_FULLSCREEN`, `GT4_PRESENT_MODE`, `GT4_MONITOR`, `GT4_OUTPUT_SIZE`, `GT4_GS_SYNC`, `GT4_PERF`, `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY`);
  `gt4-gs.ini` becomes `GT4_GS_CONFIG`; `controls.toml` in the user folder becomes `GT4_CONTROLS_FILE` when it exists. A missing or
  damaged config file means the built-in defaults, with one line in `game.log`.
- **Missing pieces:** no prepared files, no disc copy, a changed disc copy, no GSdx or a GSdx that fails its hash, or a game not
  in an install `game\` folder: a native message box (`MessageBoxW`) says what is missing and to open GT4Recomp once, and the
  game exits with code 3. Nothing is started.
- **Launcher side:** `gt4l_core::launch::standalone_config_text` renders `game.ini` from the same settings and display target
  that build the Play environment (a test compares the two); the Play path is unchanged (explicit arguments and variables), so
  behaviour when the launcher starts the game does not change.
- **Tests:** CTest `native_standalone` (layout from the executable path, game ids, the receipt reader, defaults, precedence,
  command line, config parsing, missing-piece messages); the launcher's cargo tests; the frame identity checks of
  `scripts/frame_cost_headless.py` against the slice268 build (menus, day, night); a headless standalone run.

Result (slice 278, 2026-10-23; recipe and numbers in docs/BUILD.md "Slice 278"):
- Built from the slice268 recipe into `O:\gt4recomp-scratch\slice278-build\build`. CTest 43 of 44 pass (new
  `native_standalone`); the failing test is `python_native_contracts`, whose only error is the release-pin check
  (`test_make_distribution`), expected for any rebuilt executable. Python with `GT4_NATIVE_BUILD` = slice278: 682 tests,
  1 error (that one), 42 skipped; new `tests/python/test_standalone_start.py` (7 tests) passes.
- **Identity with the launcher's arguments (explicit mode), exact:** menus dumps `463d455c3204cb56` / stream
  `1ccb97c58a6d2945`, day `992f1ac9a6871901` / `0c79dcf5208e5e4d`, night `c57e7d03accbb3aa` / `c091716e3cb40d79`, equal to the
  slice268 run in each scene (41, 159 and 159 dumps, name and hash lists byte-identical).
- **Headless standalone start** (a scratch install, no ELF argument, `--no-window`): menus and day give the same dump lists and
  stream hashes as above, and as the harness run with the defaults passed as explicit variables.
- `cargo test --workspace`: 436 passed, 4 ignored, 0 failed in each build (installer and `--features portable`); new tests: the
  `game.ini` text against the sample the game fixture reads and against the variables Play sets, and the files written
  after a settings change (`flow_audit`).
- Decisions: the donor `main.cpp` stays untouched (pins): `game_main.cpp` provides `main`. **Standalone mode is a start with no
  ELF argument**; with an ELF argument the old path runs unchanged, so no harness needed an explicit "off" value and no
  pin or test changed. The launcher keeps passing its explicit arguments and variables at Play (behaviour unchanged) and
  writes `game.ini` / `gt4-gs.ini` for the direct start. `controls.toml` is found in the same user folder.
- Not done or not seen: a real double click (window, fullscreen, the console release and `game.log`), the native message boxes
  on screen (they are tested through `GT4_STANDALONE_MESSAGE_FILE`), the portable zip on another PC, a direct start before the
  launcher ever played (the card is then blank). Paths with characters outside the system code page are not handled (the donor
  main reads its arguments as narrow text). A direct start does not harvest the card into the saves library; the launcher
  does it at the next Play (`prepare_card`).

### TASK-095 — Ultrawide: undistorted pre-race menu overlay
Planned (owner, 2026-10-22), idea to study later. At ratios wider than 16:9 (owner screenshot at 3440x1440, Time Trial
on Trial Mountain) the pre-race menu (race lobby: car name, CV/kg/tires table, Session Record, the icon row, the
car spec line) is a 2D overlay drawn over the live 3D track. The 3D background is already correct (the TASK-080
race camera hook widens the projection), but the overlay is stretched horizontally with the screen. In the race
itself the HUD is right, because TASK-080 P2 draws it as an undistorted, centred layer (RaceDisplay canvas, ELF
constants). Idea: apply the same inverse horizontal scale to the pre-race menu overlay, centred, keeping the 3D
stretched. Study first: which code draws this overlay (it is not the race HUD path), whether the P2 canvas hook or
a per-scene display control covers it, and how the other race menus (pause, results, replay menu) behave. Same
guard rails as TASK-080 (decision 0071): 16:9 frames stay byte-identical, no save-data writes, hooks listed with
tests.

### TASK-091 — Release 1.2.1: per-machine install, data beside the program, user data in AppData
✅ **Done (2026-10-22): owner tested and approved; released as v1.2.1.** Branch `task/091-install-layout-121`. Owner decisions 2026-10-22:
- the installer installs per machine in `C:\Program Files\GT4Recomp` (administrator rights; the folder page stays);
- the large data (disc copy, prepared files, gt4-gs, caches, sessions, logs, custom textures) lives in
  `<install>\data`, found relative to `GT4Recomp.exe` (portable: the folder can be copied to another drive or PC; a
  read-only copy is read and writes go to `%LOCALAPPDATA%\GT4Recomp`); the installer gives the Users group
  (`S-1-5-32-545`) Modify on that folder only;
- settings, controls, saves and replays live per Windows user in `%APPDATA%\GT4Recomp`;
- the launcher, not the installer, moves the 1.2.0 files at its first start (checked copies, Documents folder renamed
  with a README, nothing overwritten, never while the game runs, retried after a failure) and removes leftovers of the
  old per-user install; the installer removes the old per-user program install;
- the self-update starts the elevated setup (`runas`); uninstall removes `<install>\data`, and the "also delete" box
  acts only for the current user.
- the setup of a self-update from 1.2.1 on is silent (`/S /UPDATE /D=<install folder>`, one UAC prompt) and the launcher starts again as the player; a reinstall into another folder records the old data folder (`PreviousDataDir`) and the launcher moves its large data at its first start.

Acceptance: `cargo test --workspace` green with the new path, move, cleanup, elevation and settings tests; UI checks;
installer compiled and extracted; owner test of a fresh install, the 1.2.0 update, the move, a portable copy and the
uninstall. Result so far: 330 Rust tests passed; owner test installer `private/slice250-installer/GT4Recomp-Setup-1.2.1-test.exe`. Slice 251 (review fixes): 332 passed. Slice 252: 343 passed; installer `private\slice252-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,845,242 bytes, sha256 `2FD43DCF6D18FDAE981AF0CD22D79E259E7175ED08DFF41AE3007802A1CF42FF`), not run yet: owner test of the silent update from 1.2.1 and of a reinstall into another folder (data moved from `PreviousDataDir`).
Slice 257 (GitHub issue #80, blank white launcher window): the window starts hidden and shows on `ui_ready`; a 12 s watchdog restarts the launcher in WebView2 safe mode (`--disable-gpu`, flag `webview-safe-mode`, Settings toggle "Launcher GPU acceleration"), then a native message offers the WebView2 download page or the log folder. `cargo test --workspace` 351 passed. Installer `private\slice257-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,859,313 bytes, sha256 `51998294e3ccd9b0b80b6175e5c6e3b969d89c6236f4669878dcf274c6bbb70a`), not run yet: owner test with `GT4RECOMP_TEST_UI_NEVER_READY=1`.
Slice 258 (1.2.1 final candidate): a missing WebView2 runtime is detected before any window (registry as the installer checks it, `GT4RECOMP_TEST_WEBVIEW2_MISSING=1` for tests); the update path is faster (no icacls on an update unless its probe fails, EstimatedSize without the data tree, the launcher starts before the old copies are removed; phase timings in setup.log); the launcher clean-up runs after the window exists; a second start never shows the hidden window. Slice 259: **Performance Mode** (Settings > Graphics, Off by default; On = GSdx hardware downloads NoReadbacks, replaces the Readbacks row; CoreGS 1.1 shows it disabled). Installer `private\slice259-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,859,775 bytes). Not run on this PC; owner test pending.
Details: BUILD.md slice 250, [release notes 1.2.1](docs/release-notes/1.2.1.md).

### TASK-090 — CoreGS 3.0: modern render pipeline
Planned (owner, 2026-10-22), after CoreGS 2.0. The VU1 work (the PS2's vertex shader, today AOT C++ on the MTVU
thread) moves onto the GPU and the game renders natively. It may first run the whole game with some effects missing
or approximate, then restore them one by one natively (flares through occlusion queries, no GS readbacks). Fidelity
is a measured, per-scene target, not a bit-exact gate. A faithful renderer stays the default and the reference; CoreGS
3.0 is opt-in until it beats it. Game logic, physics, timing, saves and gameplay-relevant read-backs never change.
Scope rule in AGENTS.md; roadmap in docs/COREGS2-ROADMAP.md; study `vu1-direct-gpu-study` (in progress).

### TASK-087 — GS readbacks without stalls
**Owner decision 2026-10-21: CoreGS 2.0 only.** gt4-gs (the PCSX2 GS fork) gets no more feature work; at most texture
replacement is switched on (its implementation already exists) with a launcher option (TASK-048). So the gt4-gs steps
below (S1 protocol hint, S3 asynchronous download in the fork) are dropped. The readback work lands in CoreGS 2.0:
S4 GPU-driven flare after M8/M9, with P0/P1 as its proofs and the one-shot captures kept on the GPU. Removing EE-side
waits in our runtime (S2 deferral, TASK-084) stays the general goal: optimise where possible and remove the waits.
Planned (owner, 2026-10-21). Study: [docs/GS-READBACK-STUDY.md](docs/GS-READBACK-STUDY.md). Owner report: at 8x
(5120x2160) on gt4-gs a night race drops to 40 FPS when the headlight flares show; "unsynchronized" readbacks run
smoothly but lose the flares, the loading-screen logo and the car icon beside the gap time. Findings (Confirmed): all
readbacks go through `func_004A5178` (4 call sites), queued in the pgl display list; the EE never reads the flare pixels.
The GS reduces each light's depth-tested square to one visibility pixel in a 64x8 table, the table is copied back and
pointed at by a DMA REF to VU1, which scales the flare sprite in the same frame. The other readbacks are one-shots
(`CarIconMaker::render` icons at race load, `func_00104B68` loading-screen capture, a 64x64 thumbnail) plus photo mode
(8x4 depth autofocus, tile capture). Plan: P0 per-call-site readback log and game-state hash, accurate vs zero-filled;
P1 VU1 flare program checked over visibility 0..255; S1 protocol 1.4 per-call-site hint so one-shots stay accurate;
S2 deferred flare readback (TASK-084) with a per-light remap (about -4.2 ms per night frame at 1x); S3 asynchronous
download in the gt4-gs fork (PCSX2-tainted agent; expected 60 FPS with flares at 8x); S4 GPU-driven flare in CoreGS 2.0
after M8/M9 (owner's idea: the renderer multiplies the marked flare draws by the visibility texel on the GPU; zero
readbacks per frame). Corrections: `updateFlare` does not read the result (NATIVE-VARIABLE-FPS-STUDY), and deferral
moves the flare from 0 to 1 frame late, not 1 to 2 (FRAME-COST-STUDY).

### TASK-088 — Release 1.2.0: custom textures, crop, black start
**Done 2026-10-22: owner approved ("1.2.0 pronta pra lançamento"); released as GT4Recomp v1.2.0 and gt4-gs 1.2.0.**
Owner decisions (2026-10-21). Release 1.2.0 of both the game/launcher and gt4-gs; the work is one branch per repository
(`task/088-custom-textures-120` in this repository, `gt4gs/textures-1.2.0` in `moesuito/gt4recomp-gs`).
- **Custom textures (gt4-gs 1.2.0).** PCSX2's replacement loader was already compiled into gt4-gs; the fork now reads the
  pack from `[Folders] Textures` directly (`replacements` inside it, no game-serial folder) and never dumps (load only;
  the Dump keys are refused and forced off). The launcher sets `[Folders] Textures` to `Documents\Gran Turismo 4\Textures`
  and creates `Textures\replacements` on demand. Keys: `LoadTextureReplacements` (Graphics, "Load custom textures", default on,
  always written) and `PrecacheTextureReplacements` (Advanced, "Precache custom textures", default off). An "Open textures
  folder" button sits in Graphics. Settings schema 6 (no value changes; the backup of schema 5 is kept as before).
- **Crop.** The GT4 display crop is 2 native pixels at the bottom (was 1) and 1 at the right; a log line states it.
- **White start.** The gt4-gs window had `COLOR_WINDOW` as its background and erased nothing, and the game window had no
  background brush, so both showed white until the first frame. Now black (class brush, `WM_ERASEBKGND` paints black
  in gt4-gs; `window.cpp` class brush black in the game). A new game build (slice240) carries the game change.
- **Versions.** gt4-gs 1.2.0; GT4Recomp 1.2.0 (Cargo workspace, `tauri.conf.json`, NSIS default). The launcher's minimum
  gt4-gs is 1.2.0, so an older gt4-gs is replaced at launch by the existing `needs_install` path.
- **Checks so far.** `cargo test --workspace` (launcher) all green; new tests in `launcher/crates/gt4l-core/tests/textures.rs`
  and the migration test for schema 5 to 6. Browser UI: nav-test 150 of 150; focus-check 92 of 100 on this checkout and on
  the baseline copy (the same 8 tab-highlight checks fail on both: a headless timing difference, not this change).
  Game build slice240 (`private/slice240-game-build`): see `docs/BUILD.md` slice 240.
- **Status.** Waiting for the owner's test. Nothing is tagged, released, drafted or merged; gt4-gs 1.2.0 is a local package
  until the owner approves it.

### TASK-018 — Racing wheels
High-level emulation at the PDIUSB boundary with SDL3 wheels and force feedback
(docs/MENUS-AND-WHEEL.md). Builds on the TASK-075 SDL3 input layer.
Planned for 1.3.1 (owner update, 2026-10-26; previously 1.4.0): the game must recognise the connected wheel as a Logitech Driving Force Pro (DFP)
(900-degree range and force feedback in GT4), whatever wheel SDL3 finds (Logitech, Thrustmaster, Fanatec); the
game's force-feedback commands go to SDL3 haptics. The launcher gets a new Wheel tab (detection, rotation, force
feedback strength, pedals and buttons, a live test). Study first: the GT4 USB stack (USBD, PDIUSB, LGDEV IRX,
docs/IOP-MAP.md), where to answer as a DFP, and the force-feedback command set.

Owner scope update 2026-10-26: the virtual target is Driving Force Pro (DFP), not DFGT.
Native 900-degree steering and game-generated wheel force feedback are planned for 1.3.1;
1.3.0 stays focused on Spec II and performance. TASK-123 prepares research and an implementation
plan only. The private 1.2.5 draft is held back from publication; existing input contributions remain
in main as reusable groundwork and are not proof of native wheel support.

Host-input groundwork was merged through TASK-121 and packaged in the private 1.2.5 draft.
Publication is now on hold. The guest USB/Logitech identity and native force-feedback protocol
remain future acceptance for 1.3.1; this contribution does not complete TASK-018.

### TASK-075 — Input layer on SDL3
Owner decision 2026-10-21. Replace the game's XInput-only pad code (native/host_io.cpp) with one input layer on
SDL3 (zlib licence, fine in the MIT game exe; also the Linux and Android path, TASK-074 and TASK-022). SDL3 opens
only its gamepad/joystick subsystems; the game window stays ours. Every pad SDL3 knows (Xbox/XInput, DualShock 4,
DualSense, Switch Pro, generic) maps to the DS2 layout with analog L2/R2 pressure and stick dead zones kept. The game
reads the launcher's controls.toml (the branch controls code, GT4_CONTROLS_FILE), so remapping in the launcher
applies in game. The launcher's Controls page reads pads through the same SDL3 database (Rust sdl3 crate) instead of
the WebView Gamepad API, so the launcher shows exactly what the game sees. The input layer runs in the game process
(no launcher bridge: the launcher is hidden in the tray during play, and latency and force feedback need the game
side). Racing wheels (TASK-018) build on this layer. Acceptance: Xbox and DS4/DualSense pads work in game with no
third-party tools; launcher remaps apply in game; headless frame dumps unchanged; owner test.

State: ✅ Done (2026-10-21). Game side slice210 (SDL3 3.4.18 pinned in `docs/inputs/slice210-sdl3.json`, `native/pad_mapping.*`, `GT4_INPUT=sdl` default, `xinput` kept as a fallback), launcher side on the Controls page (SDL3 through the Rust `sdl3` crate). Owner tested pads and remapping, and approved.

### TASK-076 — Modern fixed keyboard layout
Owner design 2026-10-21, hard-coded (not remappable for now), several keys per DS2 button: D-pad left/right = A/D and
arrows; D-pad up/down = arrows; D-pad up (ghost) also E; Cross (accelerate, confirm) = W and Enter; Square (brake) = S;
Circle (handbrake) = Space; Triangle (back) = Backspace and Esc; nitro = Shift; gear up / gear down = their own keys
near WASD; look back = Q; camera = C or Tab. Map each action to the DS2 button GT4 really uses (check the decomp and
the manual: nitro, gears, rear view, camera). The launcher Controls page shows the layout read-only in Keyboard view.
State: ✅ Done (2026-10-21): the hard-coded table (`native/controls.cpp`, `docs/CONTROLS.md` keyboard-default, `docs/INPUT-SDL3.md`). Nitro = R1, gears = R2/L2, rear view = L1, camera = Select (ludo.guide); Start = P (owner). Owner tested.

### TASK-079 — Force 16:9 at boot
Owner request 2026-10-20. A fresh save starts the game in 4:3. When the player picks 16:9 (Wide) in GT4's own Options and
saves, it stays 16:9. Force 16:9 at boot for everyone, the same way GT4_FORCE_480P holds 480p (TASK-009, slice164).
Environment `GT4_FORCE_WIDESCREEN`: 1 forces 16:9, 0 leaves the game's own setting, unset means 1 (the launcher passes
its setting). Any other value refuses the start. The launcher setting "Widescreen 16:9" (Graphics page, default on) sends
it. Forced in guest RAM only; the owner's card file is never written by the host.

Guest chain (decompiled source at identical addresses, gt4-decomp e0b70c4d6; Confirmed unless marked):
- owner pointer `0x622F4C` (the game-data object); option object `owner + 0x38CB0` (MOption).
- wide flag `option + 0x14` = `owner + 0x38CC4`: 0 = 4:3, 1 = 16:9. Set by `func_00437110` (the script setter
  `MOption.wide_mode`, `MOption__get_wide_mode`), read by `func_001AB4C0` (race HUD rectangle, 0.75 scale of the
  4:3 squeeze) and by the photo mode code. Saved with the option object (High confidence: the same object holds the
  garage and calendar that the save path writes).
- aspect record `option + 0x24`, 11 words, written by `func_00439108` from one of four presets in guest data:
  `0x622E90` (4:3), `0x622EC0` (wide), `0x622EF0` (4:3 NTSC), `0x622F20` (wide NTSC); index = wide + 2 * (video == 1).
  `func_004390D0` says "wide" when width / height >= 1.5 (f32) at `record + 0xC` and `+ 0x10`.
- presets are filled at start by `func_00439B30` from `0x6A6188` (4:3, 275 x 205) and `0x6A61B8` (wide, 425 x 240);
  the NTSC presets hold the same two records (checked in the pinned ELF, Confirmed).
- video system `option + 0x1388` (1 = NTSC).
- published word `0x6184F0` (D_006184F0) = `func_004390D0(option + 0x24)`, written by `MOption.apply` (`func_004365B8`).
  It is the value the renderers read: RaceDisplay, RaceDisplayUtil::wideOffset, the 0.75 scale in `func_00393790`
  and `func_00102B30`, the minimap offset.

Native (`native/race_widescreen.*`, pure, fixture `native_race_widescreen`): at each guest VBlank, when the owner chain
is valid and the wide preset is wide, it sets the flag to 1, copies the wide preset into the record when the record is
not wide (what `func_004391C8` does), and publishes `0x6184F0` from the record. A preset that is not wide is refused
with no write (logged once).

Limits: the game's menu can show 4:3 for one frame before the next VBlank forces it back (same limit as 480p); a save
made with the force on stores 16:9. The presentation of the 4:3 and 16:9 layouts of the game is otherwise unchanged.

Checks (2026-10-21, slice211): see `docs/BUILD.md` slice 211 and `docs/journal/2026-10-21.md`.
State: ✅ Done (2026-10-21). slice211 (`docs/inputs/slice211-pcsx2.json`, `--mode pcsx2`) and the launcher setting
"Widescreen 16:9" (default on). Owner tested: a blank card boots in 16:9 and 480p, and the 100% save loads. A "Load
failed!" seen during the test was the launcher card bug (save folder "." entry), fixed in TASK-019; the force was
ruled out headless (slice210 and slice211 with the force off failed the same way on the bad card).

### TASK-019 — Launcher
State (2026-10-21, TASK-075 launcher side, branch task/019-launcher): the launcher reads pads through SDL3 3.4.18 (`gt4l-platform` `sdl_pads`, the same layer the game uses), in a Rust pad service that emits `pad-status`, `pad-live` (about 60 Hz) and `pad-nav`; the Controls page and nav.js use these events, the Web Gamepad API is only in the dev mock. The Keyboard view shows the fixed modern layout (TASK-076), read only. Checks: `cargo test --workspace` green, nav-test 149 of 149, focus-check 90 of 90 (headless Edge), real exe over WebView2 with 0 ACL denials. Not done: the game side (task/075-sdl3-input) and the owner's pad test; the launcher's controls file still uses XInput-named sources (the game reads them until its SDL layer lands).
State (2026-10-21, slice209): native game reads `GT4_FULLSCREEN` (strict 0/1; the launcher default is on). Game executable slice209 (build `private/slice209-build`, sha256 a75c58fa..., 97,105,408 bytes), mode `pcsx2` (docs/inputs/slice209-pcsx2.json): the slice208 coverage library and the fullscreen start only. CTest 33 of 33, Python 589 OK (23 skipped), headless 42 of 42 frame dumps identical to slice208. The owner launcher test folder was rebuilt (`private/owner-test-launcher/game`, slice209; the slice206 folder is `game-206-old`). Owner test of the fullscreen start pending; the branch is not in a PR.
State (2026-10-20): in progress on `task/019-launcher`. Main is merged (c4c7304) and the UI branch is merged (4292f7f).
The settings table is refreshed from main (TASK-049 page/curation, TASK-064 prohibit_dynamic_code always written), and the
Rust gt4-gs.ini writer equals scripts/gs_settings.py (golden fixtures regenerated by the Python writer). Checks: cargo
test --workspace 117 passed, 0 failed, 1 ignored; Python unittest 555 OK, 38 skipped. The logo has a white-text variant
(ui/assets/logo-gt4-white.png, launcher/tools/make_white_logo.py) on the setup, home and settings pages. The home page
has a background slideshow (12 s per image, 1.5 s crossfade, slow pan, reduced-motion fallback) with placeholder images
(ui/assets/backgrounds, see its README; the owner is making new images). The version is 1.0.0 everywhere it is declared
(Cargo workspace, tauri.conf.json, installer NSIS, home label, About). UI pass 3 (2026-10-20, owner requirements: touch, controller, Play): every control reachable by touch
(48 px targets, tap help tips, slider drag, swipe between Settings tabs, scrolling lists), every page
reachable by controller (X Save on Settings, Y secondary action, right-stick scroll, footer hints per
page), and the glass Play button (idle sheen, solid white when focused). Branch task/019-launcher, not yet
in a PR. Still open: the owner test of setup, saves, controls and the new touch and pad behaviour;
greying the presets above the device limit (max_upscale); the installer test and the live update test (TASK-043).
State (2026-10-20, owner requirements, one writer): SemVer streams (GT4Recomp `vX.Y.Z`, gt4-gs `gt4gs-vX.Y.Z`, minimum
gt4-gs 1.0.1, asset `gt4-gs-X.Y.Z-windows-x64.zip`), `version.json` (installer writes it, launcher records gt4-gs),
first gt4-gs install during setup and at launch, a launch-time update check (never blocks Play; popup with Update,
Not now, Skip this version; updates never automatic), data-folder guard, schema migrations with backups (settings,
controls, version.json), tray (hidden while the game runs, back on Home when it ends, close asks while it runs), and
the Graphics table of the UI mock. Docs: `docs/RELEASES.md`, `docs/launcher/MIGRATIONS.md`. Checks: `cargo test
--workspace` green. Not done here: a real installer build, a live GitHub check and the owner's end-to-end test.
State (2026-10-20, flow audit, `docs/launcher/FLOW-AUDIT.md`): the whole flow mapped and tested end to end with the
real discs in a sandbox (45 steps: 23 PASS, 17 FIXED, 0 FAIL, 5 NEEDS-OWNER). Disc rule: SCUS-97328 whose CORE.GT4
equals the pin is accepted (v2.00 must also match the full image pin); Spec II and other discs get "This disc is not
supported yet". Setup now copies the image to `%LOCALAPPDATA%\GT4Recomp\disc\SCUS-97328.iso` (free-space check,
verify then rename, schema 2 receipt), so the picked file is needed only once; a missing copy leads back to setup.
Setup draws copy, check, prepare and gt4-gs download progress; the real gt4-gs 1.0.1 zip (one wrapping folder) now
installs. The real GT4Recomp.exe opened on Setup, Home and "Game files missing" in the sandbox. Checks: cargo test
--workspace 186 passed, 3 ignored; nav-test 55/55. Owner: file picker, Play, tray, update popup, pad, touch.
Rust + egui or GPUI: game image, renderer (paraLLEl-GS / donor fallback),
upscaling, video mode, controls; CPU feature detection for build variants.

### TASK-025 — Save manager
One standard 8 MB PS2 card image per slot under the user data folder; the
launcher passes the active slot's card to the game. Import from single-save files
(.psu first; .max/.cbs/.xps later), from card images (.ps2/.mcd, listing their
saves) and from PCSX2 folder cards; import always creates a new slot and never
overwrites one. Export, delete to Recycle Bin, backups. Optional slot summary
(day, credits) from Nenkai's PDTools save format. Atomic card writes in the
runtime. Design: `docs/launcher/LAUNCHER-PLAN.md` on branch `task/019-launcher`.

### TASK-028 — CoreGS 1.1
Our current Vulkan compute GS renderer (CoreGS 1.x, donor lineage, MIT) gains:
internal resolution scale 1x-4x (`--mode upscale --scale N`, PS2_SCALE; 3x is
the release target), a deinterlace filter active only for interlaced output
(behaviour from our clean-room description in docs/hw-renderer-spec chapter 13,
no GPL/LGPL code copied), an FXAA-class AA pass and AMD FSR 1.0 (MIT) for
viewport scaling. Branch `task/028-donor-upscale`. Part of release 1.0.0
(docs/RELEASE-1.0.0-PLAN.md).

### TASK-029 — Controller and keyboard mapping
Binding table in the runtime (XInput auto-map, LT/RT as L2/R2 pressure,
keyboard remap, deadzones/curves) read from a controls file the launcher
writes; schema in docs/CONTROLS.md. Branch `task/029-input-mapping`.

### TASK-026 — Own hardware renderer: clean-room specification
Study only (decision 0069). The specification team reads the PCSX2 hardware GS
renderer (pinned source) and public GS documentation and writes a behavioral
specification in plain English under `docs/hw-renderer-spec/` (branch
`task/026-hw-renderer-spec`): architecture and data flow, GS state to GPU draw
translation, texture cache and render-target management, upscaling (internal
resolution multiplier and its fixes), CLUT, readbacks, blending and channel
shuffles, texture dumping/replacement and pack format, GT4-specific fixes,
post-processing, validation scenes and test cases, and an implementation plan
with milestones for TASK-027. No PCSX2 code or code-like translation in the
specification. Sizes and coupling overview: `docs/GSDX-STUDY.md` on branch
`task/012-parallel-gs`.

State (2026-10-15): specification complete in `docs/hw-renderer-spec/`
(README index, chapters 00-15; chapter 15 holds milestones M0-M19 for TASK-027,
~94 implementer-days, risks and 13 open owner questions). Ready for owner review;
TASK-027 starts after approval and the answers to Q-01 to Q-03.

### TASK-027 — Own hardware renderer (CoreGS 2.0): implementation
**Next:** the owner's test of candidate 374. D1 is measured and not built (IN-523); the waits and the input lag are in 374 (IN-521, IN-522). The darker road in Performance Mode stays. Open items are listed at the top of `docs/STATUS.md`.

**State 2026-11-02, end:** 🧪 candidate 376 (not uploaded), branch `task/082-coregs2-frame-skip`: a frame rate choice, 60 or 30 in races, replaces the option of 375 (IN-525), 60 by default: 12x 100% speed at 30 frames per second, intervals 33.0-33.7 ms. Unlocked FPS paused, `docs/TASK-082-UNLOCKED-FPS-STATE.md`. Next: CPU optimisation with another agent (TASK-134).

**State 2026-11-02, later:** candidate 375 (not uploaded), branch `task/082-coregs2-frame-skip`: candidate 374 plus TASK-082, frame skipping on CoreGS 2.0, off by default (IN-524): 12x on George V Paris 100% speed at 38.5 frames per second, against 77% at 46; the frame shown is always the last one drawn; race clock equal on both games. Waiting for the owner's test, above all on the laptop.

**State 2026-11-02:** 🧪 candidate 374 (not uploaded), the base of 375: input lag at 60 Hz 53 to 31-39.5 ms, the EE's wait at the flare read 9.5 ms to 0, flares exact, frames identical to 373 (122 of 122, then 47 of 47).

**State 2026-11-01:** 🧪 candidate 373 (not uploaded), adding to 372 the frame pacing work IN-514 to IN-519 (race on George V Paris at 6x without Performance Mode: GS CPU 7.94 to 7.31 ms, frames over 17.5 ms 40 to 5 of 3,700; frames identical 154 of 154; the second presentation queue is not yet confirmed on the owner's virtual display; the owner decides on the late flares of IN-514). Candidate 372 before it: Spec II on CoreGS 2.0
(IN-506), shaders compiled before Play (IN-507), no readback for a write beside a drawn area (IN-508: George V Paris at 6x,
headless, GS thread -19%), no frametime wave from a target drawn every second VBlank (IN-509), and IN-510 to IN-513: the race start
drop, three swapchain images under vsync, the showroom wave and reused decodes (window, race without Performance Mode: VBlanks over
20 ms 29 to 1 of 1000). Frames identical at 1x, 3x and 6x; at 12x 2 pixels of one frame. The owner's F8 on 372 is pending. The owner's F8 at 6x on George V Paris: 49.6 VBlanks/s, 57.7 in
Performance Mode; the GS thread's waits for GPU readbacks are the largest cost. Open: GSdx's F8 for comparison.

**State 2026-10-31:** 🧪 candidate 367 (not uploaded) waits for the owner's test on the laptop: on the HD 620 at 2x GPU
busy 27% less by day and 23% at night against candidate 362 in normal mode, 35% and 31% in Performance Mode (IN-485 to
IN-504, `docs/hw-renderer-spec/CORE2-GPU-PLAN.md`). In the launcher as a beta in 1.3.0-rc8 (GSdx stays the
default): texture replacement (IN-470), GPUs without interlock (IN-471), PGO in the release game (IN-473), options by
renderer (IN-474). Open: the owner's test, then the upload of rc8 and the PR into
`task/134-game-code-optimization-study` on their word; fractional scales, a screen that prepares shaders, frame skipping.

**Resumed on 2026-10-21 (owner decision)** on branch `task/027-coregs2` (worktree
`C:\Antigravity\gt4-wt-027-coregs2`): `main` at release 1.0.0 (slice214) merged with
`task/027-hw-renderer` and the `task/027-cpuref` docs checkpoint. paraLLEl-GS code, pins and launcher modes
were left out (decision 0070); `GT4_RENDERER=hw` now runs through main's GS stream seam into the in-process
HWR backend (`GT4_HW_GS=ON` builds only; black output). `task/027-m4-gpuref` (e31fcc4, GPU write-back WIP)
is not merged. Implementation continues with the external agent **GPT 6.1 Sol** as the only writer of the
branch, clean room from `docs/hw-renderer-spec` (brief `AGENT-BRIEF-COREGS2.md`, progress in
`docs/hw-renderer-spec/PROGRESS.md` and ai-memory `decisions/coregs2-implementation`). No pins until a
candidate is offered to the owner. Warm-up complete (2026-10-21): shader TEXA/AEM follows 07.5 / IN-07; slice217 builds and all three GPU exactness fixtures pass. Native v2 replay is implemented (slice219: 69/69 CPU-library tests; 1,101/1,101 identical menu PPMs across legacy, converted v2 and fresh v2). VRAM diagnostic added (local221:75/75 tests, deterministic1,096-image replay; original scanout unchanged). Correct pairing resolves the IN-315 comparison offset; the separate A+D kick fix (IN-326, local222: 76/76 tests) closes IN-316: the garage defect was the dropped A+D kicks, exactly frames 811..916 change, and 11/11 aligned samples now sit inside the chapter 14.4 2D thresholds (garage 0.2051%, 45.992 dB) while 990/1,096 VRAM and 995/1,101 scanout images stay unchanged. The M4 GPU write-back path of `task/027-m4-gpuref` is then ported, built and wired into the backend (local223, IN-329): eleven fixtures pass (03.3 coverage vectors and seven GPU/CPU equivalence cases), the default renderer stays 42/42 vs slice214 and `GT4_RENDERER=hw` runs 600 headless frames with 0 faults, still without frame dumps. IN-328 fixed the CPU reference's constant-attribute truncation. M5 then wired the whole pixel pipeline into the same draw stage (blend unit over 81 modes, COLCLAMP, PABE, alpha test, DATE, FBA, FBMASK, dither; five new raster fixtures at zero mismatches); `GT4_RENDERER=hw` stays clean at about 62 ms per headless frame. M6 then started with the presentation interfaces (IN-331): a present scheduler decoupled from the VBlank (hold/re-present, newest-wins backlog, late-input hook), auxiliary frames that restore guest memory and refuse readbacks, and per-pass CPU timers; default 42/42 parity unchanged. HWR frame dumps then landed (IN-332): the 4,200-frame headless run writes the same 42-frame schedule with 0 faults and measures 42/42 frames outside the 2D gate, with two visible draw-loss defects (boot text line, garage list). The HWR replay tool then landed (IN-333): on the same records the GPU path is pixel-identical on every even frame and loses content on odd frames (boot text line, garage 10.6%), so field-parity draw loss is the open defect. The cause was found and fixed (IN-335, local slice228): the write-back stored stale depth words over a frame buffer sharing pages with the unused depth buffer. The 42-dump headless run is now 42/42 inside the 2D gate against the donor (before 0/42). The window now follows the VBlank field (13.3 offset, 13.4 deinterlace; IN-337/IN-338, local slice229). Next: owner test 1 (windowed look at 1x), then M7. History of the pause follows.

**2026-10-21, Claude (clean-room writer after GPT 6.1 Sol):** M6 output finished (odd-frame fix, field phase,
deinterlace; owner tests 1 and 2), then the 3D speed work: resident target slots, pixel-interlock batching and runs,
the GLM with GPU CLUT, GLM-owned pages and GPU uploads, CPU front-end savings, double-buffered submission and an
asynchronous presentation source (IN-340..IN-355). Every step keeps per-frame memory identical to the write-back
mode on the menu, car and race captures and the 42 menu dumps byte-identical. Headless race 300 -> 17-18.6 ms per
frame. Waiting for owner test 3 (local slice239, `build/hwr-play.ps1`). Next: GPU compose of the display, fewer
barriers around CLUT loads, the GIF front end, M8 readbacks per the readback study.

**2026-10-22, Claude:** owner tests 3-5 (60 FPS in 3D races, flares, 16:9 480p; test 5 approved a flat 60 FPS frametime on local slice247). `main` 1.2.2 merged into the branch (local slice248/249; hw output identical to slice247, default renderer 42/42 after fixing the 1.2.2 donor-mode crash in `mtvu::Donor`). M12 upscaling (2x-8x) and FXAA in local slice250 (IN-371..IN-376; memory and readbacks identical to 1x at every scale), waiting for owner test 6. Next: owner verdict vs GSdx, 480i menus at scale, then GS-thread CPU cost.

**2026-10-23, Claude:** `main` 1.2.3 merged. Owner test 6 failed (race shown at 1x on every second frame at 4x; ultrawide race squeezed into 16:9); both fixed in local slice270 (IN-377 display shadows checked per pixel against memory, IN-378 display-aspect fill in the HWR presenter) with new controls (`hw_scale_display_shadow`, `hw_smoke_window_aspect_*`, `hw_stream_sink_display_aspect`, replay `--check-presented`). Owner test 7: ultrawide confirmed, race still 1x; fixed in local slice271 (IN-379 tone carry for GT4's T8 tone curve, IN-380 texture footprint for full-screen copies; fixture `hw_scale_fullscreen_passes`; `--check-presented` counts low-detail frames). Owner test 8 passed (races at real resolution, beta minimum met in races); new owner requirement: GSdx-level performance at the same scale. Local slice272: 480i menus at scale (IN-381, fixture `hw_scale_interlaced_present`), no scaled repetition of the tone sequence (IN-382), timing split into 1x/scaled/refresh, scene sweep findings and a ranked parity plan in PROGRESS. Waiting for owner test 9.

**2026-10-23, OpenCode (parity work after the hand-over, `PARITY-PLAN.md`):** owner decisions: item 1 (fast scaled
raster) approved with a tolerance only on the presentation-only scaled image, item 2 (tone-sequence HLE at 1x)
approved as memory-exact, item 5 (skipping the 1x redraw) rejected. Item 2's first step landed (IN-383): the eight
strip batches of one 64 x 32 page run as one GLM dispatch (`hw_tone_strips.comp`), 135,399 -> 16,940 dispatches on the
480p race capture, with 118,580 of its palette loads held (GPU CLUT loads 200,467 -> 98,827); all five memory captures
byte-identical, `hw_tone_hle_exact` 0 of 2 frames different, replay CPU GS stream 9.95 -> 8.14 ms per frame; in the game the GS thread drops 13.4-15.6 -> 10.6-11.6 ms and the
render passes 341-347 -> 198-204. Item 1 (fast scaled raster) is designed but not implemented, see PARITY-PLAN. Switches
`GT4_HW_TONE_HLE` and `GT4_HW_VERIFY_SHUFFLE` (replay `--no-tone-hle`/`--tone-verify`). No PR, no merge, no pins;
owner test 10 pending.

**2026-10-23, OpenCode (parity Plan A):** the presentation-only scaled images are no longer fixed 1024 x 1024 squares:
each covers its slot's real extent (FBW * 64 wide, loaded/drawn rows), is created lazily and grows on demand with the
old content copied, and a texture copy only takes the batch's sampled footprint (IN-380). Scaled image device memory
on the 480p race capture: 184.5 -> 64.1 MB at 2x, 738.2 -> 243.1 MB at 4x, 1,660.9 -> 541.4 MB at 6x. New fixture
`hw_scale_target_extent` (growth, a second FBW); CTest 245/245 both interlock modes, five memory captures
byte-identical, scaled memory = 1x and 0 low-detail frames at 2x/4x/6x. GPU time unchanged (memory win, not a time
win). Next: the fast scaled raster (early-Z second pass) itself, then owner test 11.

**2026-10-27..30, Claude:** CORE2 (one image per target at scale), owner tests through candidate 299 (60 FPS, flat frametime, fidelity perfect), the beta list (IN-427..IN-435: 1x, mipmapping, spike cuts, memory budget, FSR1/Lanczos presentation, filters, 12x), specialised pipelines (IN-437: 12x GPU 33.3 -> 21.4 ms) and the first performance mode item (IN-439, decision 0073). 1.3.0-rc7 (`task/133-rc7-ra-controls` c21e934) merged into the branch as its base on 2026-10-30; CoreGS 2.0 stays out of 1.3.0. Details in STATUS, the journals and `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`. CPU survey of the GS pipeline and transport (IN-445, 2026-10-30): instruments (process sampler, transport line, exclusive stage times), the list of CPU fronts and a proposed task order in `docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`; SSE4.1 adopted (`GT4_SSE41` ON, -3.8% wall time), AVX2 measured with no further gain (candidate 313). CPU and transport fronts the same night (IN-446..IN-461, candidate 332): lock-free GS stream ring, allocation-free draw path, unchanged uploads keep page versions, speculative early-Z, CT32 column swizzle, texture cache by block identity, CLUT loads by table, quiet VU1 output wakes, and direct GS intake (`GT4_MTVU_DIRECT=1`, TASK-134's P6: the GS worker reads the VU1 output ring itself); against 313 at 3x process CPU -28%, wall -15%, frames identical. Afternoon of 2026-10-30 (candidates 333-337): direct intake on by default (IN-462); the 12x run-to-run differences traced to batches that sample the target they draw into and fixed with spec 8.8's copy (IN-463, CTest `hw_core2_self_texture`; also removes the 3x run-to-run pixels the ledger listed); `task/134-game-code-optimization-study` merged into the branch (documents). Then (candidates 338-347, IN-464..IN-469): GLM hazards by page, statistics query opt-in, disjoint primitives without waits, strip run skip, PGO and ThinLTO of the GS libraries (option, not yet in the release build), VU1 output scan; against 313 at 3x process CPU -37%, frame p50 -29%, frames identical to 337. Handover to TASK-134 in `docs/hw-renderer-spec/CORE2-HANDOVER-TASK134.md`.

**Paused by the owner on 2026-10-15** to focus on release 1.0.0. State on the
integration branch `task/027-hw-renderer`: CPU-verified pure libraries for GS
memory/swizzle (M2), GIF front end and CLUT (M3), exact pixel arithmetic (M5),
display/deinterlace/LOD, texture-replacement keys and pack loading, surface
cache bookkeeping with an authority fuzz (M7 core), per-draw decisions, the
shader toolchain with GPU shaders bit-exact against the CPU reference (42.5 M
blend cases), a CPU reference renderer that replays real GT4 captures (menus
mostly match the donor; garage frame and a 2-line field offset open as
IN-315/IN-316), the M1 comparison tools, and the game hookup (GT4_RENDERER=hw,
black output). WIP branches: task/027-m4-gpuref (GPU write-back path) and
task/027-cpuref. Interpretations awaiting confirmation:
`docs/hw-renderer-spec/IMPLEMENTATION-NOTES.md` on the integration branch.
MIT implementation written only from the TASK-026 specification, behind our GS
front end next to paraLLEl-GS and the donor renderer. Implementers never open
PCSX2 GS sources. Milestones and acceptance come from the specification.

Roadmap beyond parity (owner, 2026-10-21): docs/COREGS2-ROADMAP.md. It covers the separate UI layer, a fractional
render scale, per-target resolution, precompiled pipelines, readback removal, AF and SMAA, FSR 2/3, DLSS and XeSS with
camera motion vectors and jitter, frame generation with TASK-082, effects gated by game state (AO before the UI),
HDR10, texture packs and mobile backends. Every enhancement is opt-in; the faithful image stays the default.

### First stable release
After TASK-019: a tagged GitHub Release of this repository with the NSIS
installer (prebuilt game executable and launcher; the user supplies their own
disc; official branding with a trademark notice; decision 0069), under the MIT license
([decision 0067](docs/decisions/0067-mit-license-and-release-model.md)). TASK-020
is deliberately after it, to give the gt4-decomp project more time to progress
before its names are imported.

### TASK-020 — Decomp names in generated code
Label comments in generated units with identical object code.

**Level 1 delivered (2026-10-18, PR #41, not the whole task).** Read-only name table from gt4-decomp `0b54ed5c`:
`docs/data/ee-names.csv` (11,481 rows: address, name, kind, origin, quality), built by `scripts/ee_symbols.py`; see
[docs/EE-NAMES.md](docs/EE-NAMES.md). No generated EE file, pin or runtime code changed. Still open: labelling `unit_XXXX.cpp`
needs the new producer contract described under "Level 1" in [docs/DECOMP-INTEGRATION.md](docs/DECOMP-INTEGRATION.md).

Common rules for TASK-032..045: agents that open PCSX2 sources or the fork are PCSX2-tainted and never
work on TASK-027. MIT files in this repository never contain PCSX2 code. The fork's own rules: one writer at
a time, commits as `moesuito`, every release built by CI from a tag.

### TASK-032 — Decision 0070 docs
Repo: main, branch `docs/decision-0070-gs-process`. Haiku, 1 session.
Goal: add `docs/decisions/0070-pcsx2-gs-separate-process.md`, `docs/GSDX-PLAN.md`, the CLAUDE.md
clean-room paragraph and renderer lines, GS-RENDERER-OPTIONS update, these TASKS rows, STATUS and journal.
Acceptance: owner answers to GSDX-PLAN section G recorded; links valid; Python suite unchanged.
Parallel: none needed; first.

### TASK-033 — Fork, reproducible build, CI releases
Repo: standalone public repository `moesuito/gt4recomp-gs` (not a GitHub fork: the account already has a fork in
the PCSX2 network, ARMSX2, and GitHub allows one per network). Full PCSX2 history at tag `v2.8.2`
(`fd9d310c`); branch `gt4gs/main` is the default. Haiku, Opus if the dependency build stalls; 2-3 sessions.

**Result (2026-10-15, 3 CI runs, 1 fix).** Details and run URLs in [docs/GS-FORK.md](docs/GS-FORK.md).
- Fork: `gt4gs/main` at `70a0aa66f`; `gt4gs/` docs, trimmed dependency script (Qt removed), packaging script,
  `gt4gs-windows.yml`; upstream workflows set to `workflow_dispatch` only (no file deleted).
- Release `gt4gs-v0.1.0-poc` (prerelease, tag on `3fd7faba8`): zip 18,202,463 bytes, source archive 24,954,987
  bytes, `SHA256SUMS`. Checksums verified after download.
- Deviation: MSVC (VS 18, MSVC 14.51, CMake 4.4.4, Ninja 1.13.2) instead of clang-cl; upstream's CI matrix
  also builds CMake with MSVC.
- Not done: GSRunner smoke with `-renderer null` (no dump in the repo, and game data must not be committed);
  bit-identical local rebuild; build provenance attestation. The first run failed on the VS locator and is kept
  in the run history.
Goal: CMake + Ninja + clang-cl build of the unmodified `pcsx2-gsrunner` (`-DENABLE_QT_UI=OFF
-DENABLE_GSRUNNER=ON`, Vulkan/D3D11/D3D12), dependency script trimmed of Qt and cached, `gt4gs/BUILD.md`,
NOTICE, third-party license list, a release workflow that publishes `gt4-gs-<ver>-win64.zip`, its SHA-256,
a source archive and build provenance.
Acceptance: CI green from a clean runner; release zip contains COPYING.GPLv3, NOTICE, licenses; GSRunner
smoke on CI with `-renderer null` on a tiny dump; local rebuild from the tag on the owner's machine produces
the same file list (bit-identical is a goal, recorded either way).
Risks: dependency build time and breakage; runner disk limits.
Parallel: with TASK-035.

### TASK-034 — Proof of concept: our capture through the PCSX2 GS
Repos: fork branch `gt4gs/gscap2gs` (GPL converter) and main branch `task/034-gs-replay-compare` (MIT
compare script). Haiku, Opus for the GS freeze state; 2-3 sessions.
Goal: `gscap2gs` converts `private/slice175-capture/menu-0-1100.gscap` to a `.gs` dump: header with serial
`SCUS-97328`, freeze state of a freshly reset GS, an 8 KiB privileged block maintained from 'P' records and
emitted as Registers before each VSync, 'G' path 1/2/3 to Path1New/Path2/Path3, 'R' to ReadFIFO2, 'V' to
VSync with alternating field. Stock GSRunner replays it (`-dumpdir`, `-renderer vulkan`, `-upscale 1` and
`4`, `-perf`).
Acceptance: 1,101 frames replayed, no errors; frame count and VSync count match the capture; a MIT script
compares frames 100..1100 with the donor frames in `private/slice175-capture/run-menu/frames` (pct_bad > 8,
PSNR, as in `scripts/compare_frame_dumps.py`) and writes a JSON report and contact sheet; the 4x replay
shows upscaled output; GS-thread ms/frame from `-perf` recorded; GameDB fixes for SCUS-97328 reported as
active in the log. Owner review of side-by-side images.
Risks: freeze-state format (only in the fork); field parity; the donor is not bit-identical to PCSX2, so
the gate is "recognisably the same frame", not equality.
Parallel: with TASK-035 (different repos).

### TASK-035 — MIT GS stream protocol, IPC and renderer seam
Repo: main, branch `task/035-gs-stream`. Opus, 4-6 sessions.
Goal: `native/gs_stream/gs_stream_protocol.h` (plain C, versioned records GIF, PRIV, VSYNC, READ_FIFO,
RESET, CONTROL, reserved STATE and AUX_FRAME; back-channel READY, STATS, ERROR, SNAPSHOT_DONE, EXITING;
control block; written from GS hardware semantics and our `.gscap`, not from PCSX2 code); cross-process SPSC
ring with named events and peer-handle waits; `GT4_RENDERER` dispatch on main with values `donor` (default)
and `pcsx2`, salvaging only the MIT seam from task/012; `GT4_GS_CAPTURE` (generalized `GT4_PGS_CAPTURE`,
capture v2 header); a MIT consumer `gs_stream_sink` (records to `.gscap`, or discards) used as the test peer;
the MTVU journal drain writes to the ring; signals applied on the EE for all paths.
Acceptance: CTest ring fixtures (wrap, split, full ring, peer death, spurious wake, 10^6 random records
byte-identical); protocol round trip; headless 4,200-frame run with `GT4_RENDERER=pcsx2` against the sink:
the sink's capture is byte-identical to the in-process `GT4_GS_CAPTURE` of a `donor` run with the same
schedule; donor frames still 42/42 identical to slice157 with the default renderer; Python suite green;
launcher pins updated.
Risks: ordering between MTVU worker output and EE work; readback blocking with the worker.
Parallel: with TASK-033/034.
State (2026-10-15, slice191, branch `task/035-gs-stream`): built; owner review pending, no PR.
- Protocol `native/gs_stream/gs_stream_protocol.h` (C++20 header, fixed-width structs) and the normative
  [GS-STREAM-PROTOCOL](docs/GS-STREAM-PROTOCOL.md); transport `gs_stream_ipc` (mapping, rings, events with
  waiter flags, peer-handle waits, Job Object spawn); `.gscap` v2 files; game client `gs_stream_client` spliced by
  `prepare_external_vulkan.py --gs-stream-hooks` and `prepare_perf_runtime.py --gs-stream`; consumer
  `tools/gs_stream_record` (recorder or null consumer; readbacks answered with zeros); F8 `[gs-ipc]`.
- CTest 29/29 (`native_gs_stream_ipc`, `native_gs_stream_client` new); Python 321 OK, 12 skipped.
- Headless 4,200 frames: donor 42/42 identical (default and with `GT4_GS_CAPTURE`); `GT4_RENDERER=pcsx2` completes;
  donor capture == pcsx2 in-process capture == recorder file (plus CONTROL shutdown); 619 KB and 390 records per
  frame, 0 readbacks. Converted frames 0-1100 equal the TASK-034 PoC dump byte for byte; GSRunner PNGs 1,100/1,100
  identical to the PoC (300 and 700 included). EE cost in menus: `gs_ipc_write` about 0.08 ms/frame, VSYNC post
  0.004 ms/frame, no back-pressure.
- Not verified: readbacks over IPC in the game (none in the schedule; covered by fixtures only), race scenes,
  window mode (`--mode pcsx2-stream` shows a black window by design), weak CPUs.

### TASK-036 — Live MVP
Repos: fork branch `gt4gs/live`, main branch `task/036-gs-live`. Opus, 4-6 sessions.
Goal: fork adds `pcsx2-gt4gs/` frontend (from GSRunner) producing `gt4-gs.exe`, `GSStreamReplayer` (an R5900
CPU provider reading the ring or a `.gscap` with `--replay-gscap`), handshake, child window in the game
window, VMManager hooks; the protocol header vendored with its hash checked. Main: spawn with Job Object,
handshake, pin check of `gt4-gs.exe`, `play_performance.py --mode pcsx2-gs` with a new pinned manifest,
headless frame dumps via `CONTROL snapshot`.
Acceptance: headless run: live frames equal the offline `--replay-gscap` frames of the same session capture;
boot to menus, Time Trial, race at 1x and 3x; GS crash (killed) ends the game cleanly with a message and a
flushed card; protocol-version mismatch refused; owner plays the reference scene.
Risks: VMManager boot without BIOS in stream mode; first-run shader compile stutter.
Parallel: 037/038/039 start once the MVP boots.
State (2026-10-16, slice193): built; owner windowed test pending, no PR.
- Fork `gt4gs/live` (commits `0b51d4c3c`, `acf4f986e`; tag `gt4gs-v0.2.0-live`): `pcsx2/GSStreamReplayer`, frontend
  `pcsx2-gt4gs` (`gt4-gs.exe`: `--ipc/--parent/--config`, `--replay-gscap`, GSRunner dump mode), child window,
  vendored protocol files checked by SHA-256. Launcher mode is `--mode pcsx2` (not `pcsx2-gs`), with `--gs-scale N`.
- Main: GS process SHA-256 pin (`GT4_GS_PROCESS_SHA256`), `GT4_GS_CONFIG`, `WS_CLIPCHILDREN`, crash message box;
  minimal `gt4-gs.ini` from the launcher; slice193 pins; `tools/gs_live_check`.
- Headless frame dumps come from `gt4-gs.exe` itself (`[GT4GS] DumpDir`), so the game port is unchanged; CONTROL
  snapshot is implemented in `gt4-gs.exe` and used by the window check.
- Checks: CTest 29/29, Python 335 OK; donor 42/42; live 41/41 PNGs = offline replay, 300/700 = PoC; GS kill halts
  the game; hidden-window attach/resize/snapshot/shutdown. The PoC vertical-scale question is the aspect ratio
  (Auto 4:3 gives 480 rows). Not verified: windowed image, races, 480i, readbacks, GPU time.

### TASK-037 — Readbacks and night race
Repos: both, branches `task/037-038-gs-window` (main, shared with TASK-038), `gt4gs/window`. Opus, 2-3 sessions.
Goal: READ_FIFO round trip through `MTGS::InitAndReadFIFO`; `HWDownloadMode` and readback spin options
exposed; F8 counts and wait time; Unsynchronized mode served without draining the ring (if the owner allows).
Acceptance: CTest/fixture with a scripted stream (draw, TRXDIR=1, read) compared with the donor bytes;
headless frames unchanged; owner night race at 60 FPS with `gs_ipc_readback_wait` reported; lens flare and
night lighting correct versus CoreGS 1.1.
Risks: GPU sync cost at high upscale.
Parallel: with 038/039.
State (2026-10-16, slice194): built; owner night-race test pending, no PR.
- Cause of the TASK-036 night-race late frames (213/7,467): one readback per frame (1.07 ms each) removes the GS
  buffering, so GS-thread spikes (pipeline compiles at race start) land in the game frame; the shader cache was in
  `Documents\PCSX2`; the MTVU `gs_privileged` wait (3.4 ms/frame) leaves about 3.7 ms of headroom.
- `GT4_GS_READBACK=accurate|unsynchronized` (`--gs-readback`), F8 readback histograms, `gt4-gs.exe` readback and
  acquire timing, own cache/data folders, deferred swap chain acquire (A/B `GT4_GS_EAGER_ACQUIRE=1`).
- Checks: readback fixture exact in accurate mode (drawn sprite and uploaded bytes); headless frames unchanged;
  cold vs warm cache replay (max GS frame 84 ms -> 6 ms).
- Owner night race (slice194): accurate 59.97 FPS, 69 late frames (was 213), readback mean 0.78 ms; unsynchronized
  60.00 FPS, 1 late frame, but headlight flares missing. Owner chose accurate as the default; unsynchronized stays
  an option ("faster, may drop effects").

### TASK-038 — Window, fullscreen, focus, input, lifecycle
Repos: both, branches `task/037-038-gs-window` (main, shared with TASK-037), `gt4gs/window`. Opus, 3-4 sessions.
Goal: child window hardening (own pump thread, `HTTRANSPARENT`, `MA_NOACTIVATE`), Alt+Enter/F11 borderless,
resize, minimize, DPI, multi-monitor, focus loss; GS hotkeys forwarded (screenshot, OSD toggle); fallback
option 2 (GS-owned window, keyboard forwarding) behind a switch.
Acceptance: Python/CTest for the message contract; a scripted soak (resize and fullscreen toggles, 1,000
cycles) with no hang; a GS-thread stall test does not freeze our window beyond the stall; RTSS overlay
visible; owner checklist (fullscreen, alt-tab, minimize, controller and keyboard).
Risks: attached input queues; DWM latency in child windows.
Parallel: with 037/039.
State (2026-10-16, slice194): built; owner checklist pending, no PR.
- Input queues detached after SetParent; click on the picture brings the game forward; minimized (0x0) resizes
  ignored; WM_CLOSE in pcsx2 mode halts before the window is destroyed; GS death: 180 grace VSYNCs (card flush),
  restart offer with CoreGS 1.1 (launcher relaunches with `GT4_RENDERER=donor`); handshake timeout message.
- Checks: stall fixture (3 s GS window stall, parent answers within 17 ms), soak 3,000 resizes, aspect check (same
  rectangles as TASK-009), CTest handshake-timeout / game-killed / grace scenarios.
- Not done: own pump thread for the child (the main thread only pumps), GS hotkeys, fallback option 2, DPI and
  multi-monitor checks, RTSS.
- slice195 (2026-10-16): owner found that a click in the picture deactivated the game window (detached queues make
  the GS thread the foreground thread). gt4-gs 0.3.1 gives the foreground back after every click; the game keeps the
  keyboard focus on activation. Visible click fixture passes (0.3.0 fails). Owner click/focus check pending.

### TASK-039 — Settings table and `gt4-gs.ini`
Follow-up (2026-10-16, `fix/duplicate-gs-json-settings`): reject duplicate JSON setting ids consistently
with CLI assignments. Module validation: 58 tests pass; native/gameplay validation not performed.
Repos: main `task/039-gs-settings`, fork `gt4gs/config`. Haiku, 2-3 sessions.
Goal: `docs/launcher/gs-settings.toml` (GSDX-PLAN D.1), generator to `gt4-gs.ini` (dev script now, launcher
later), allow-list validation in `gt4-gs.exe`, folders under the user data directory, `CONTROL
reload_config`.
Acceptance: Python tests (table schema, generated INI golden file, refusal of unknown keys and out-of-range
values); `gt4-gs.exe` reports effective settings in READY; owner checks 1x/3x/6x, FXAA, CAS, dithering,
deinterlace in menus.
Parallel: with 037/038.
State: ✅ Done (main-repo part); fork allow-list enforcement pending (TASK-037/038 agent). All rows resolved. Branch `task/039-gs-settings` (2026-10-16). Details: [GS-SETTINGS](docs/GS-SETTINGS.md).

### TASK-040 — Texture dumping and replacement (folded into TASK-050, release 1.1.0)
Repos: fork `gt4gs/textures`, main docs. Haiku, 1-2 sessions.
Goal: folder layout `...\gs\textures\SCUS-97328\{replacements,dumps}` and `.zip` packs; dump, load, async,
precache options; user documentation.
Acceptance: scripted test: dump from a replayed capture, replace one texture with a solid colour, replay,
detect it in the frame; owner tries a community GT4 pack if available.
Parallel: after 039.

### TASK-041 — Performance
Repos: both, branches `task/041-gs-perf`, `gt4gs/perf`. Haiku for counters, Opus for tuning; 3-4 sessions.
Goal: F8 `[gs-ipc]` section (game: write, back-pressure, vsync post, readback wait, bytes, records; GS:
thread busy, GPU ms, draws, uploads, readbacks, present wait, ring occupancy); tuning: frames in flight,
ring size, direct GS-thread feed; `--cpu-cores 2` check.
Acceptance: `perf_probe_fixture` covers the new categories; owner A/B in showroom, night race, rally,
reference scene: CoreGS 1.1 vs PCSX2 GS at 1x and 3x, 60 FPS, flat frametime, no measurements while builds run.
Parallel: after 037.
State (2026-10-16, slice198, branch `task/041-gs-perf`; fork unchanged, `gt4gs-v0.3.2`): built and checked headless;
owner night-race A/B pending, no PR.
- Headless race schedule added (Arcade, El Capitan, 5 AI, 16,000 frames): readbacks with the night-race pattern.
- Night-race `gs_privileged` wait (3.48 ms/frame): 8 privileged writes per frame = 5 at the VBlank (PMODE, SMODE2,
  DISPFB2, DISPLAY2, BGCOLOR) + CSR and BUSDIR x2 per readback; the wait sits before the CSR write (the EE waits for
  the frame's VU1 work right before the flare readback). `GT4_MTVU_PRIV_ASYNC` (default 1) queues privileged writes
  in the MTVU journal: stream bytes identical (menus captures, race stream hash with 588 queued writes), frames
  identical; the gs_privileged wait becomes 0 but moves to the download sync of the readback (no net gain expected
  in the night race).
- Readback tail: long waits are pipeline compiles (cold cache: 12 of 294 at 8 ms or more, max 58 ms; warm: none, max
  2.7 ms); a GS-side spin does not help (fork unchanged); fix is TASK-046.
- Stream: 322 GS wake-ups per frame -> 11-14 with `GT4_GS_WAKE_BYTES` (default 256 KiB); `gs_ipc_write` about -30%
  per MB; one copy less per worker GIF record in the MTVU journal.
- F8 `[gs-ipc]`: privileged writes and MTVU waits by register, GS wake-ups, GS CPU thread per interval, one-line EE
  critical path summary against the 16.68 ms budget.
- Menus, same build: EE CPU donor 7.6 vs pcsx2 7.2 ms/frame. CTest 29/29, Python 416 OK, donor 42/42, pcsx2 41/41.
  Details: [GS-LIVE](docs/GS-LIVE.md) section 9.

### TASK-042 — JIT-free `gt4-gs.exe`
Repos: both, branches `gt4gs/nojit` (fork, from `gt4gs/main` `660017bb1`) and `task/042-gs-nojit` (main, from `a1a06a4`).
Opus, 1 session.
Goal: `GT4GS_ONLY` build without EE/IOP/VU recompilers, without RWX code mapping, with the C draw-scanline
path or CPU sprite/CLUT render refused.
Acceptance: binary scan finds no recompiler symbols; a VirtualAlloc/VirtualProtect audit shows no executable
private memory; PoC and live frames unchanged; GSRunner tests still pass.
Parallel: with 037-041 (fork-only).
State (2026-10-16, slice197): built and published (`gt4gs-v0.4.0`, zip sha256 `5c92ad00...`); owner windowed check
pending, no PR.
- Fork option `GT4GS_ONLY`: no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software JIT (C rasterizer for CPU
  sprite/CLUT render and the software renderer; no setting refused), no code area, executable protections refused;
  Windows Arbitrary Code Guard on at start (works with the AMD Vulkan driver); executable-memory audit in the log.
- Static check `tools/gs_nojit_check/check-nojit.ps1` (fork `gt4gs/check-nojit.ps1`, run by the release script): 0.4.0
  passes, 0.3.2 shows 22 markers; `tests/python/test_gs_nojit.py` repeats it on the pinned package.
- Offline menus replays (slice191 and slice196 streams, 1x and 3x): frames byte-identical to 0.3.2; also with CPU
  sprite and CLUT render on. Live headless pcsx2 1x 41/41 = slice196; donor 42/42. GS thread time unchanged within
  noise. `--mode pcsx2` = slice197 (same game runtime as slice196), `--mode pcsx2-slice196` kept.
  Details [GS-LIVE](docs/GS-LIVE.md) section 9.

### TASK-043 — Launcher integration
Repo: main, branch `task/019-launcher` (when the launcher resumes). Haiku, 2-3 sessions.
State (2026-10-20): in progress on `task/019-launcher`. Code: `launcher/crates/gt4l-core/src/{releases,update,version,versions,guard,migrate,gs_release}.rs`
and `launcher/crates/gt4l-tauri/src/{updates,tray}.rs`. Stable releases only (drafts and prereleases ignored); the
renderer minimum is gt4-gs 1.0.1; the install is verified against `SHA256SUMS` and staged as before. Checks: fixtures
for both repositories (no network), SemVer, update decision, version.json, migrations, data-path guard, tray state machine.
Still open: the owner's live test against GitHub, and a real NSIS build (the installer was edited, not built here).
Goal: renderer manifest (version, URL, size, SHA-256, protocol version, upstream commit) in the launcher;
download with progress and resume, verify, unpack to the user data folder, update to newer pinned versions,
offline fallback to CoreGS 1.1; settings pages generated from the table; license/source links shown.
Acceptance: Rust tests (hash mismatch refused, partial download resumed, wrong protocol refused, fallback);
owner end-to-end install.

### TASK-044 — Packaging and release notes
Repos: both. Haiku, 1-2 sessions.
Goal: fork release `gt4-gs v1.0.0` (notes: upstream commit, our patch list, license, source); main: README
renderer section, `native/THIRD_PARTY.md`, RELEASE-1.0.0-PLAN renderer table and W8, known issues.
Acceptance: release assets complete; links valid; owner approval.
Note (2026-10-16): `gt4gs-v1.0.0` is published (stable) and pinned in slice202. Known issue for the release notes: gt4-gs 1.0.0 writes under `Documents\PCSX2`: `VMManager::Internal::LoadStartupSettings()` runs before `[GT4GS] DataRoot` is applied, so it rewrites `Documents\PCSX2\logs\emulog.txt` every session (Confirmed: before/after snapshot of the folder) and creates the PCSX2 user folders there (bios, memcards, logs, ...) on a PC that has none (High confidence, `EmuFolders::EnsureFoldersExist`). The log line "MemoryCards Directory: ...Documents\PCSX2\memcards" is that early pass; the session itself uses DataRoot. Fix on fork branch `gt4gs/datadir` (commit `b30870fe8`, version 1.0.1, NOT released): the call moves after the stream folders; checked: 0 changes under `Documents\PCSX2`, emulog in DataRoot\logs.
Decided (2026-10-17): release 1.0.0 ships gt4-gs 1.0.1 (see Result below).
Note (2026-10-16, TASK-044): gt4-gs 1.0.1 is published (stable, tag `gt4gs-v1.0.1`, fork PR #7 merged) and pinned in
slice203 (`--mode pcsx2`). The `Documents\PCSX2` known issue is fixed in 1.0.1: no changes in the `Documents\PCSX2` snapshot for a
live run or an offline replay (slice203, TASK-044). The clean-profile check was not run (see BUILD.md slice 203).
Result (2026-10-17, docs only; no build, no game run): **decision: release 1.0.0 ships gt4-gs 1.0.1** (`gt4gs-v1.0.1`,
pinned in slice203 and slice206, decision 0070). The gt4-gs 1.0.0 release stays as history. The fork release notes of
`gt4gs-v1.0.1` were edited (upstream base PCSX2 v2.8.2, commit `fd9d310cc`; patch list; GPL-3.0 and source; SHA256SUMS);
assets unchanged. README renderer section, known issues and legal notes updated; `native/THIRD_PARTY.md` rewritten;
RELEASE-1.0.0-PLAN renderer table and W8 updated. Release order: this task, then the launcher and installer (TASK-019/043),
then the owner's end-to-end test, then the v1.0.0 tag and GitHub Release. Mods (TASK-048, 050-057) are release 1.1.0 and
appear only in this list.

### TASK-045 — High frame rate on the PCSX2 GS
Repos: both. Opus, 10-14 sessions, after release 1.0.0.
Plan: (1) D1: VRR and present-mode tests, no interpolation (1 session). (2) Oracle on the PCSX2 GS: the MIT
`tools/gs_interp` builds interpolated and extrapolated frames as streams; replay them with `gt4-gs.exe
--replay-gscap` and compare with real frames (race capture needed; kill if matched primitives < 95% or
race PSNR < 32 dB) (2-3 sessions). (3) In engine: the game holds frame N+1, generates the in-between stream
(MIT, game side) and sends it as `AUX_FRAME` before N+1; the GS renders and presents it without guest-visible
side effects (readbacks and signals stripped; targets overwritten by N+1); present scheduler for 120/144 Hz;
+1 frame latency measured (6-10 sessions).
Risks: texture-cache pollution by auxiliary frames, page alternation (HIGH-FPS 9.3), HUD.

### TASK-058 — Crash-safe memory card in the real build
Repo: worktree `C:\Antigravity\gt4-wt-058-harden`, branch `task/058-059-release-hardening` (from main `3b5334d`). Release 1.0.0 hardening, game side. One writer.
Goal: the runtime writes the card through the atomic writer of TASK-025 Part A (temporary file, flush, rename), keeps a failed write dirty and retries, and flushes a pending save on exit, including the pcsx2-mode GS-process loss path after its grace VSYNCs.
Result (slice200, docs/BUILD.md): TASK-025 Part A was on `task/025-save-manager`, not on main, and is imported; memcard capsule `private/slice200-memcard-runtime` (derived `iop/sio2.cpp`, pinned by `runtime.json`); CMake `GT4_MEMCARD_RUNTIME_SOURCE`; `flush_cards_now()` after `emulate()`. CTest 29/29 (`native_memcard_store`), Python kill test (25 kills of a copy: old or new image, never torn). The headless menus schedule never saves: the card of the donor and pcsx2 runs equals the seed, so the byte comparison of a saved card is not available headless; the in-game save check is the owner's test.

### TASK-059 — Distribution executable
Repo: same worktree and branch as TASK-058. Haiku, 1 session.
Goal: an install folder that runs with no repository path: `<exe folder>\shaders` when present (else the build-time folder), the gt4-gs package at `<exe folder>\gs\gt4-gs.exe` or `GT4_GS_PROCESS`, no build path embedded except the development fallback, a `--portable-check` self-test, and `scripts/make_distribution.py` for the folder (no gt4-gs package, no game data; GPL kept separate, decision 0070).
Result (slice200, docs/DISTRIBUTION.md): the shader change of `task/019-launcher` ported (its printf string repaired); `native/install_paths.*`; port `private/slice200-port` with `--portable-check`; `make_distribution.py` wrote `private/slice200-dist` (16 files, 93 MB); the exe holds one build path (the build-time shader fallback). Smoke from that folder, headless: donor 42/42 frames identical to slice157; pcsx2 41/41 identical to slice199 (gt4-gs 0.4.0 via `GT4_GS_PROCESS`, ACG on). Tests: `test_portable_install.py`, `test_make_distribution.py`.
Open: the release build should drop the build-time shader fallback; `--mode main` is not rebuilt (its build is not shared); the owner's disc is not copied into the worktree.

### TASK-047 — PCSX2 GS 480i deinterlace
Repos: both, branches `task/047-gs-deinterlace` (main, from `6806f78`) and `gt4gs/deinterlace` (fork). Opus, 1 session.
Goal: the 480i menus look like real PCSX2 with deinterlace Automatic (owner report 2026-10-16: soft and flickering
in gt4-gs; races at 480p fine).
Acceptance: root cause with evidence; headless before/after menu frames with a comb and flicker metric; progressive
output unchanged; CTest, Python, donor 42/42; published `gt4gs-v0.3.2` pinned; owner check of the menus.
State (2026-10-16, slice196): built; owner check pending, no PR.
- Cause (Confirmed): the stream never carried SYNCV (the BIOS writes it; the donor's SetGsCrt HLE does not), so
  PCSX2's GS treated the 480i output as not interlaced and showed each half-height field line-doubled, bouncing one
  line per frame. With SYNCV alone the fields were woven in the wrong order: gt4-gs used the previous VSYNC's field,
  but the donor flips FIELD before GT4's VBLANK_S handler (`0x004AB430`) reads it, one field earlier than hardware.
- Fix: SetGsCrt posts SYNCV for NTSC/PAL; protocol 1.1 defines VSYNC `field` as the display field (same values);
  gt4-gs 0.3.2 presents with the record's own field. `--mode pcsx2` = slice196, `--mode pcsx2-slice195` kept.
- Static Garage menu, frames 1000-1199: slice195 comb 0.50, flicker 3.23; SYNCV with the old field comb 1.00
  (combing); slice196 comb 0.57, flicker 0. Progressive control identical. Samples:
  `private/slice196-deinterlace-samples`; details [GS-LIVE](docs/GS-LIVE.md) section 8.

### TASK-048 — Mod manager in the launcher (umbrella)
Owner, 2026-10-21: gt4-gs is frozen apart from this; texture replacement is the one feature still to switch on there
(the PCSX2 GS implementation exists; enable it and expose it in the launcher).
Plan: [MOD-MANAGER-PLAN](docs/launcher/MOD-MANAGER-PLAN.md).
Goal: a Mods page where users install, enable, order and remove mods; texture packs for the PCSX2 GS first.
Texture replacement is not a graphics setting: the launcher writes the GS keys from the enabled mods.
Children: TASK-050..057. Legal: users bring their own packs; we never host or download packs.
State: plan written 2026-10-16; owner answered all ten questions the same day (see below); release 1.1.0.

### TASK-050 — Texture replacement contract and headless round trip
Repos: main, branch `task/050-texture-contract` (from `main`); fork `gt4gs/textures` only if a summary log line is
needed. Haiku, 1-2 sessions. PCSX2-tainted agent (never TASK-027).
Goal: confirm the PCSX2 v2.8.2 replacement contract on our stream and correct GSDX-PLAN D.3 (no zip packs;
duplicates resolved by enumeration order; PNG only 8-bit RGB/RGBA; `-mipN` files ignored; `[Folders] Textures`
relative to `DataRoot`). Measure map scan time, RAM (with and without precache) and VRAM on a synthetic
10,000-file pack.
Acceptance:
- Python test (skipped without the private capture): replay twice with dumping on, identical dump name sets;
  solid-magenta replacement of a texture used in frame K appears in frame K (`Async = false`) and within 2 vsyncs
  with `Async = true`; one-digit-changed name, `-mip1` name and empty view leave frames byte-identical to the
  baseline; duplicate-key rule recorded.
- Name-parser fixture built from real dump names (all parse; round trip name → key → name).
- Measurements recorded in GS-LIVE or a new `docs/GS-TEXTURES.md`; D.3 corrected; `gs-settings.toml` texture rows
  get their sources.
- Optional: name-set overlap ≥ 95% against a PCSX2 v2.8.2 dump of the same menu scene; owner tries a community
  pack if available.
Risks: hash-relevant settings (`HWMipmap`, `TriFilter`, `UserHacks_EstimateTextureRegion`) differ from a pack
author's; the capture may not contain the texture chosen (pick from the dump set itself).

### TASK-051 — `gt4mods` core: manifest, detection, validation, import, store
Repo: main, branch `task/051-mods-core` cut from `task/019-launcher` (the launcher workspace lives there); new
UI-free crate `launcher/crates/gt4mods`. Haiku, 3 sessions; Opus reviews the validation code (plan 5.2).
Goal: `gt4mod.toml` schema 1 and `files.lock.json`; import from zip or folder with or without a manifest;
detection of raw PCSX2 layouts (`SCUS-97328/replacements`, bare `replacements`, name-majority folders); texture
key parser (all five name forms); PNG/DDS validation; staging and atomic commit into `mods\store\<id>\`;
registry `mods.toml` written with temp + `ReplaceFileW`; operation journal and recovery at start.
Acceptance: Rust tests with synthetic zips: zip-slip (`..`, absolute, drive, ADS, device names, case
collisions), zip bomb (ratio, total, entries, lying headers), symlink and encrypted entries refused; PNG 16-bit,
paletted and interlaced detected; DDS block-size and truncated data detected; within-pack duplicate rule; kill
between steps leaves a recoverable state; update of an installed id keeps enabled state and order.
Risks: very large packs (streaming hash and extraction, progress and cancel); non-ASCII and long paths.

### TASK-052 — Active view, priority, conflicts, uninstall, CLI
Repo: main, branch `task/052-mods-view` (after TASK-051, same base). Haiku, 2 sessions.
Goal: build `mods\active\textures\SCUS-97328\replacements\` with one hardlink per winning texture key (top of
the list wins), copy fallback when hardlinks fail; view digest; atomic swap (`active.new` → `active`); conflict
report per mod; enable/disable/move without reinstall; uninstall to the Recycle Bin with the quota check; a
`gt4mods` CLI (`list`, `import`, `enable`, `disable`, `move`, `remove`, `build-view`, `report`) for tests and
the dev launcher.
Acceptance: Rust tests: each key exactly once in the view; priority changes switch the winner; identical-content
collisions not reported; view rebuilt only when the digest changes; refused while the game lock is held;
tampered view detected and rebuilt; copy fallback on a non-NTFS temp volume (or simulated failure); Recycle Bin
path covered by a fake platform layer. Manual: 10,000-file view build time recorded.
Risks: hardlink limits (1,023 links per file, irrelevant here); antivirus scanning many new links.

### TASK-053 — GS settings and dev launcher wiring for mods
Repo: main, branch `task/053-mods-gs-wiring` (from `main`). Haiku, 1 session.
Goal: in `docs/launcher/gs-settings.toml` move `texture_load`, `texture_load_async`, `texture_precache` off the
Graphics page (`page = "Mods"`, `visibility = "hidden"`, `written = "launcher"`), move the dump rows to a creator
group, add `textures_folder` (`[Folders] Textures`, `path`, launcher, absolute); `scripts/gs_settings.py`
accepts the new page; `external_runtime_probe.py` / `play_performance.py --textures-root <dir>` writes the
folder and `LoadTextureReplacements = true` when the folder has files; `result.json` records the folder digest.
Acceptance: Python tests: golden INI unchanged without the option; with the option, the three keys and the
folder appear; relative paths refused; UTF-8 path with non-ASCII characters round trips; allow-list regenerated;
pins regenerated where the probe changes. Owner: one in-game run with a synthetic or community pack.
Risks: gt4-gs pin and allow-list drift (TASK-037/038 enforcement).

### TASK-054 — Launcher Mods page
Repo: launcher branch after TASK-019 resumes (Tauri 2). Haiku, 2-3 sessions.
Goal: `mockup-mods.html` (states: empty, list, details, import progress, conflict, remove confirm, wrong
renderer) for owner approval, then the Tauri page and commands (`mods_list`, `mods_import`, `mods_set_enabled`,
`mods_move`, `mods_remove`, `mods_details`, progress and cancel events); drag and drop of zips; Inbox folder
import at start; controller navigation (D-pad focus, Cross toggle, Triangle details, L1/R1 priority, Square
remove, Start install help, Circle back); the view is rebuilt before Play and its digest written to `launch.json`.
Acceptance: owner approves the mockup; Rust command tests; UI test of keyboard and XInput focus order; owner
installs, reorders and removes a pack end to end.
Risks: WebView file drop and CSP; long imports must not block the UI thread.

### TASK-055 — Mod creator tools
Repos: launcher branch (UI and packaging), main docs. Haiku, 1-2 sessions.
Goal: collapsed "Creator" section: "Dump textures" for the next Play (folder `mods\creator\textures`, dump keys
written by the launcher, FMV off), open dumps and work folders, "Package as mod" (zip with `gt4mod.toml`,
validated by the TASK-051 importer), "Clear dumps" (Recycle Bin); `docs/MODDING.md` for pack authors (names,
formats, alpha scale, BC7 with mips advice, hash-relevant settings).
Acceptance: a dump session produces PNGs in the creator folder only; a packaged work folder re-imports cleanly;
docs reviewed by the owner.
Risks: dumping cost in races (documented banner).

### TASK-056 — Live texture reload in gt4-gs (after 1.0.0)
Repos: fork `gt4gs/texture-reload`, main. Haiku, 1-2 sessions.
Goal: a control message (`reload_textures`) that calls the replacement map reload on the GS thread, and a game
window key that sends it, so creators see edits without restarting.
Acceptance: headless test: replacement added during a replay appears after the message; no leak across reloads.
Risks: protocol version bump; reload stalls (sync worker thread wait).

### TASK-057 — Study: other mod kinds (after 1.0.0)
Repo: main docs only. Opus, 1 session.
Goal: decide on `gs-preset` mods (bundles of `gs-settings.toml` rows), a read-only `GT4.VOL` overlay for data mods
(cars, tracks, menus; Spec II idea), and RAM data patches; code patches stay out (AOT EE).
Acceptance: a decision record with risks and effort; no code.

Release: 1.1.0. Owner decisions (2026-10-16)

1. The mod manager is **not in release 1.0.0**; it is planned for **1.1.0**. It is tracked in TASKS.md only
   (not in the README task table). The launcher's Mods tab stays hidden in 1.0.0.
2. Priority: the mod higher in the list wins a texture conflict.
3. TASK-040 is folded into TASK-050.
4. The "precache all textures" toggle is hidden (off by default; RAM risk).
5. PAL-version packs are allowed with a warning.
6. PNGs in an unsupported format: the launcher shows a warning and asks whether to convert them to 8-bit.
7. Third-party pack catalogue: not now, but planned later (we never host packs ourselves).
8. Limits: 20 GiB per pack; zip and loose-folder import.
9. Creator texture dumps are taken with mods off, from a creator option in the Advanced settings.
10. A pack too big for the Recycle Bin: ask clearly, then delete permanently only after on-screen confirmation.

### TASK-060 — Cursor auto-hide
Repos: both, branches `task/060-062-osd` (main, from `3b5334d`) and `gt4gs/osd` (fork, from `0e40edc92`). Opus, 1 session
(with 061 and 062).
Goal: hide the mouse cursor after about 3 s without movement, in fullscreen and in windowed mode only over the picture;
show it on any movement; never touch it elsewhere; the game window and the gt4-gs child window (another process) agree.
`GT4_CURSOR_AUTOHIDE=0` switch (default on) and an Advanced-page row.
Acceptance: CTest of the rule; a visible tiny fixture with the real child (move, hold, assert visibility); owner check.
State: ✅ done, owner test 2026-10-16 (fullscreen and windowed); merged with main as slice202.
- `native/cursor_autohide.*` (pure rule, CTest `native_cursor_autohide`), used by `window.cpp`; protocol 1.2 CONTROL
  cursor carries the policy to gt4-gs 1.0.0, whose window thread applies the same rule and answers `WM_SETCURSOR`.
- `tools/gs_live_check --cursor-check` (visible, about 1.7 s) passes; details [GS-LIVE](docs/GS-LIVE.md) section 11.1.

### TASK-061 — "Compiling shaders…" notice
Repos: fork (`gt4gs/osd`) and main (settings row). Same session.
Goal: plain text "Compiling shaders…" bottom-left while the PCSX2 GS really compiles shaders or pipelines (not cache
loads), gone when compiling stops; reuse PCSX2's compile signal; on by default with an Advanced-page row.
State: ✅ done, owner test 2026-10-16 (gt4-gs 1.0.0). `GSShaderCompileIndicator` plus a real-compile timestamp;
pipelines from the driver cache are excluded with `VK_EXT_pipeline_creation_feedback`; shown until 0.5 s after the last
compile. Cold cache 238 compiles (notice drawn), warm cache 0 compiles and 138 cache hits (no notice). [GS-LIVE](docs/GS-LIVE.md) 11.2.

### TASK-062 — Performance overlay
Repos: both. Same session.
Goal: PCSX2-OSD-like overlay, top-right, each item switchable: FPS, frame time, CPU and GPU usage of the game only (game
process plus gt4-gs), output and internal resolution, per-part ms per frame and the headroom against 16.67 ms (the F8
summary, live, about twice per second); off by default; negligible overhead (measured).
State: ✅ done, owner test 2026-10-16. Game numbers in protocol 1.2 OSD_STATS records (sent only when
READY asks for them); CPU via GetProcessTimes of both processes, GPU via PDH "GPU Engine" counters of both pids; drawn
with the PCSX2 ImGui OSD. Overhead: 0.35 us/frame on the EE thread, 12-15 us/frame of ImGui text and +0.02 ms/frame of GS
thread time. Rows `perf_overlay` (master) and one per item. [GS-LIVE](docs/GS-LIVE.md) 11.3.

Settings and stable renderer (same branch): internal resolution presets 480p (1x), 720p (2x), 1080p (3x), 1440p (4x),
4K (6x), 8K (12x) only (default 1080p; launcher rules for the monitor height and the GPU limit documented in
[GS-SETTINGS](docs/GS-SETTINGS.md)); `--gs-resolution`; gt4-gs 1.0.0 ignores INI keys and values outside the allow-list
with a warning; **`gt4gs-v1.0.0` published as the first stable release** and pinned by slice201.

### TASK-049 - GS settings curation for the launcher
Done (2026-10-16). Owner decisions applied to `docs/launcher/gs-settings.toml` (58 rows, `page`, `order`, `control`,
`gt4-defaults` profile): Graphics page = renderer choice (PCSX2 GS or CoreGS 1.1), internal resolution slider 1 to 12,
aspect, FXAA, CAS mode and sharpness; Advanced page = 36 rows pre-configured to the GT4 ideal; texture rows on `mods`.
Findings (PCSX2 v2.8.2, [GS-SETTINGS](docs/GS-SETTINGS.md)): FSR 1.0 does not exist in v2.8.2 (CAS only);
12x needs no `ExtendedUpscalingMultipliers` (the device limit is `max texture size / 1280`); READY does not carry the
device limit yet; widescreen patches are pnach EE patches and are not exposed. `reset_to_defaults()` reproduces the
default gt4-gs.ini byte for byte. Default gt4-gs.ini unchanged. The `mipmap` key is now `hw_mipmap`.

## Phase 2 — Android details

### TASK-021 — Mobile GS (ARMSX2 pipeline, Vulkan)
Bring the ARMSX2 GS pipeline (PCSX2 hardware renderer with ARMSX2's Android
changes: GPU driver profiles, tile-friendly pass scheduler, framebuffer-fetch
policy) behind our GS front end. ARMSX2's ARM64 JIT is not used (no JIT rule).
Owner's ARMSX2 checkout: `C:/Antigravity/armsx2` (upstream `ARMSX2/ARMSX2`).

### TASK-022 — Android app
ARM64 build of the AOT EE/VU code and runtime (fp-contract off), Android host
layers (window, audio, input, storage, lifecycle), on-screen controls that hide
when a Bluetooth controller connects; target Galaxy S23 Ultra.

## Phase 3 — iOS details

### TASK-023 — Metal GS (ARMSX2)
ARMSX2's Metal backend for the same GS pipeline.

### TASK-024 — iOS app
ARM64 build, iOS host layers, signing/installation on iPhone 15 Pro Max and
17 Pro Max (built on a Mac with Xcode).

## Ideas (not scheduled)

LAN-over-internet and an online lobby launcher (docs/ONLINE-NOTES.md), in-game
settings menus.

**Two-player split screen (open, not numbered; owner, 2026-10-24).** Make GT4's own two-player split-screen mode
(Arcade "2P Battle", `RaceSplitBattle`) work properly with two controllers. Not a priority; map it before any work.
What we know today:
- Input: the host feeds only player one. `native/host_io.cpp` polls one XInput slot (the first connected) and SDL3
  opens one gamepad (`gt4::pad::first_pad`); the guest's second pad port gets nothing. Needs a second guest pad
  (port 2) fed from a second controller, plus launcher Controls support (player 1 / player 2 assignment, a keyboard
  layout for player 2 or keyboard + pad).
- Rendering: split screen has its own path (`RacePS2Base::render_world_split` 0x0034028C, `func_0033EAB8` second
  view; docs/GS-READBACK-STUDY.md, docs/NATIVE-RENDERER-STUDY.md). In split screen the headlight flares use a constant
  (only the sun uses the GS readback), so its readback cost is lower.
- Ultrawide: split screen stays pillarboxed 16:9 today (TASK-080 scene policy); a widened split view is a later
  question.
- Owner checks wanted: both players drive, both HUDs right, performance at 60 FPS on both renderers, Spec II too.

**Release plan (owner, 2026-10-22):** big features take a minor version, intermediate updates and hotfixes the
patch number: 1.2.3 hotfix, 1.2.4 (TASK-097, 098, 099), 1.3.0 Spec II (TASK-100, on GSdx), 1.3.1 native DFP wheels (TASK-018; owner update 2026-10-26);
2.0.0 is CoreGS 2.0 (phase 2).

**Spec II is a second target** (owner, 2026-10-22): support means the SCUS-97436
Public Beta lineage ISO with its own target executable, not a content merge into
v2.00. Recorded, not scheduled: [decision 0072](docs/decisions/0072-spec2-second-target.md).
The asset-merge variant is closed by the 2026-10-22 file study (same audio and
art already in v2.00; menus are Adhoc v7 against v5).

A decompilation-based source port (native renderer, changing the game's physics
timing, remaster) is out of scope for this repository and would be a
separate project, possibly a fork. Presenting more than 60 FPS with the game's
fixed 1/60 s physics untouched is in scope (TASK-031, owner 2026-10-15). Ultrawide and other aspect ratios
are in scope (TASK-080, owner 2026-10-21).

### TASK-031 — High frame rate (physics untouched)
Dropped in this form (decision 0070): redone as TASK-045 on the PCSX2 GS after 1.0.0. Study docs stay on branch `task/031-high-fps`.
Goal: present 120-144 FPS while the game's physics keeps its fixed 1/60 s step
(`RaceBase__updateRace` uses a constant dt; read-only study, 2026-10-15).
Approaches: (A) GS primitive interpolation between two guest frames, with real
rasterised in-between frames and no optical flow or frame generation. It costs
about half a guest frame of latency, and an extrapolation variant is measured
alongside. (B/C) A native render rate: re-run the game's render path (EE scene
submission and VU1) with interpolated car and camera transforms, via decomp
hooks. Step 1: an offline oracle that builds frame N from captured frames N-1
and N+1 (and an extrapolated frame from N-2 and N-1) and compares it with the
real frame N; kill if fewer than 95% of primitives match or race PSNR is below
32 dB. In parallel, a read-only study of the native render-rate path: main
loop, update/render split, transforms and render cost. Input-latency trims
(submit ring depth, present mode) are measured with the prototype.

### TASK-034 — PCSX2 GS proof of concept (offline replay)
Branch `task/034-gs-replay-compare`. Goal: prove that the PCSX2 GS (decision 0070, separate GPL process) renders
GT4 correctly from our GS stream, offline. Done: `tools/gscap_to_gsdump/` converts the menu capture to a `.gs`
dump (23 unit tests); stock `pcsx2-gsrunner` built from PCSX2 `685637d` replays it at 1x and 3x without errors
(1,100 frames each); the GameDB fixes for SCUS-97328 apply from the serial. Frames 300 and 700 match the reference
exactly; frames 900 and 1100 do not, and the PCSX2 output is 480 rows against the references' 448.
Recommendation: GO for TASK-033 and TASK-035; conditional GO for TASK-036 (open: the vertical scale, the 900/1100
differences, a freeze state). Details and numbers: [docs/GS-PCSX2-POC.md](docs/GS-PCSX2-POC.md). Clean room:
PCSX2-tainted work, not touching CoreGS 2.0. Owner review: the side-by-side images and frames 900 and 1100.


### TASK-089 — Independent CoreGS launcher path

**Done (2026-10-21) in release 1.2.0.** Contribution by JulioHeroZ (PR #72, branch `task/089-coregs-independent-launcher`, based on main `6422988`), merged into `task/088-custom-textures-120` with a merge commit so their commits and authorship are kept. The launcher review of the diff found no problem. Added on the 1.2.0 branch: the setup dialog chooses GSdx (recommended) or CoreGS 1.1 (built in, no download) before setup starts; choosing GSdx in Settings installs it at once when it is missing (inline progress, "GSdx installed successfully", and a failure keeps CoreGS 1.1 selected); every user-facing "PCSX2 GS" string now says GSdx (settings keys and file names unchanged).

Original branch description:

Branch `task/089-coregs-independent-launcher`, based on main `6422988` (2026-10-21).
A saved `renderer = "coregs11"` choice skips the external renderer installer during setup,
its automatic missing/outdated-package repair at startup and its background release-list request.
The automatic launcher update check still runs. The explicit Check for updates and renderer
update actions continue to include the optional package. Play prepares a built-in renderer session
without reading or pinning an unused external executable.

Acceptance: synthetic setup completes without invoking the injected external installer, preserves the
choice across a launcher restart and prepares `GT4_RENDERER=donor` without an external process;
injected update requests query only launcher releases for CoreGS, while default PCSX2 startup and
explicit manual checks retain both streams. Full Rust launcher workspace passed on Windows (Tauri: 53 passed, one real-disc flow ignored).
Maintainer launcher acceptance remains pending.
Owner test: select CoreGS 1.1, restart the launcher with no gt4-gs package, launch a game and confirm
there is no renderer download; switch back to PCSX2 and confirm its existing setup/update path.
This changes package management, not rendering fidelity, performance or CoreGS 2.0 readiness.



### TASK-119 — RetroAchievements release 1.2.4

Owner decision 2026-10-25: integrate JulioHeroZ's PR #96 on the 1.2.3 code line, preserve authorship,
fix the reviewed account refresh/logout race, HTTP shutdown delay and missing test coverage, and prepare
a focused 1.2.4 release before bringing the feature to 1.3.0. Worktree `C:\Antigravity\gt4-wt-119-ra-124`,
branch `task/119-retroachievements-124`. The original PR commit `d6c9ea0` is preserved by merge.

State: completed, owner approved and published as stable 1.2.4 on 2026-10-25. PR #96 and #105 merged.
CTest 43/43, Python 641 tests/42 skipped, final Rust 379 passed/4 ignored; UI focus 114/114,
navigation 150/150; final launcher rebuild and all 24 installer/source checks passed.
Owner authorized final publication; real-account and installer execution remain untested.
Final asset GT4Recomp-Setup-1.2.4.exe, tag v1.2.4, Latest.
User-approved exception: the legacy launcher remains English-only in 1.2.4, with en-US and pt-BR notes.
Installer-only legacy packaging; the separate portable edition and multi-game launcher remain in 1.3.0.
Native inputs are reused unchanged from slice268; new output reservations are slice1190 (build/pin),
slice1191 (distribution), slice1192 (Cargo target). No owner saves or installation paths may be changed.
Acceptance: CTest, Python with the exact new build/pin, launcher workspace and UI checks; no credential
restoration after logout; bounded cancellation of a slow loopback HTTP request; installer/source identity;
owner real-account login, unlock, overlay and save test. Later 1.3.0 integration must keep RA disabled for
Spec II until its identity and achievement conditions are independently validated.

### TASK-120 — Integrate PR #100: DualSense trigger range and response curves

Owner request 2026-10-25: accept external PR #100 before PR #101, resolve integration conflicts,
and validate each contribution separately. Contributor: luanantony (Luan Antony); original commits
`b58024245253d548f3a8306e3eadc680bc547bd6` and `a36486d333c8fa23b89e1bddaa2404979b53dd87`.
Worktree `C:\Antigravity\gt4-wt-120-dualsense-integration`, branch `task/120-dualsense-integration`,
based on stable 1.2.4 main `90af154`.

State: merged through integration PR #106 at `8b73fa0` on 2026-10-25; original PR #100 is merged.
CTest 43/43, Python 641 tests/42 skipped, Rust 381 passed/4 ignored, UI focus 114/114 and
navigation 154/154 passed. Immutable slice1200 preserves slice1190 inputs and RA hashes.
No hardware gameplay or release performed. The incoming task label TASK-104 was incorrect: that number belongs to save import
on the 1.3.0 line. This contribution is tracked as TASK-120 and relates to planned controller tuning
TASK-110; it does not mark that broader future task complete.

Scope: preserve the contributed extended curves (s_curve, smooth_power, sublinear, logarithmic).
The proposed signed-axis modes and device-name heuristics are removed after review: SDL3 Gamepad
triggers already use 0..32767 for DualSense and Xbox alike. The unsupported raw-joystick mode was
never in a released build; native and Rust reject it consistently. Existing controls remain compatible.
Preserve the 1.2.4 RetroAchievements account, HTTP-cancellation and UI response guards.

Acceptance: independently reviewed SDL3 normalization and mode semantics, shared Rust/native parser and
writer compatibility, default and custom controller mapping regression fixtures, native CTest, Python
against the new build, Rust workspace and Controls UI navigation checks. Actual DualSense USB/Bluetooth
travel, reconnect and in-game pressure remain hardware checks; synthetic fixtures alone cannot prove them.
Record observed results before marking acceptance complete. Integration only: no release assets, labels,
installed application, owner card/save or active game session is changed. PR #101 begins only afterward.
See `docs/PR-100-101-INTEGRATION.md` for the sequential mapping and acceptance limits.

TASK-120 validation result (2026-10-25): CTest 43/43; Python 641 tests, 42 skipped; Rust 381 passed, 4 ignored; UI focus 114/114 and navigation 154/154, no JavaScript errors. Owner authorized sequential integration. Physical controller gameplay remains unvalidated; no release created.


### TASK-121 — Integrate PR #101 and #109: configurable racing wheels

Owner approved sequential integration on 2026-10-25 and assigned these contributions to stable
1.2.5 on 2026-10-26, followed by a port to 1.3.0-rc7 preserving frame skipping. PR #100 is already
merged through #106 at main8b73fa0. PR #101 by JulioHeroZ now ends at
`6da047cf5ca9829224ade13dd84e9c797129bdf8`; PR #109 by luanantony contributes conflict-resolution
commit `99f8dc48f6f6fc9ecfc0421bf0e75758203ea6bf`. Both histories are preserved by integration.

State: complete and merged on 2026-10-26 through PR #111 at main cbfd8b8.
Original PR #101 (latest head) and PR #109 are also marked merged.
Three original conflicts are resolved. Independent native and Rust/UI static reviews approved
routing, opt-in defaults, unique physical identity, full haptic stop, capture and UI/backend contracts.
Fresh slice1210 build/pin: CTest 44/44, Python 641 tests (42 skipped), Rust workspace 391 passed
(4 ignored), actual UI focus 114/114 and navigation 169/169. Actual UI setting commands were
recorded and replayed through real Rust command bodies. No physical haptics or gameplay claimed.

This implements configurable host input through existing gamepad actions, contributing to TASK-018.
Native guest wheel USB identity/protocol, physics-derived torque, direct gear selection and manual
clutch remain outside this implementation; damper/inversion fields are reserved and inactive.
Existing RetroAchievements protections and TASK-120 controller curves/standardized SDL triggers stay.
See docs/WHEEL.md and docs/PR-100-101-INTEGRATION.md. No new release is built or published here;
1.2.4 remains Latest. The later rc7 port must keep RA disabled for Spec II and pass both-game,
frame-skipping, input and language gates without replacing published rc6 assets.

### TASK-122 — Release 1.2.5 private draft (reserved on release branch)

Packaging is recorded on task/122-release-125 at 70aa363, without a main source merge. Draft v1.2.5
remains private; owner update 2026-10-26 puts publication on hold in favor of the native DFP plan.
This reservation prevents reusing TASK-122 while its release branch is open.

### TASK-123 — Native DFP wheel research and implementation plan

State 2026-10-26: research brief ready for an owner-run OpenCode agent; research not started.
Parent TASK-018, planned release 1.3.1. The virtual guest target is Logitech Driving Force Pro
(DFP), 900-degree steering, original game-generated force feedback translated to SDL3.
The launcher/host SDL3 configuration from TASK-121 is reusable groundwork, not the guest protocol.
Branch task/123-dfp-wheel-study; worktree C:\Antigravity\gt4-wt-123-dfp-wheel-study.
Brief: docs/tasks/TASK-123-DFP-WHEEL-RESEARCH.md. Study both USA v2.00 and Spec II independently.
Deliver an evidence-backed device/protocol map, HLE-versus-USB boundary analysis, reuse audit,
FFB effect coverage and staged implementation/test plan. No implementation, build, game launch,
physical force test, release or integration into active rc7 work is authorized by this research.
The owner starts the economical research agent in OpenCode; Codex reviews its deliverables.

## Open issue intake (owner, 2026-10-26)

Registration and priority triage only: no implementation plan or work has started.
Urgent defects are candidates for a later 1.3.0 rc before final, subject to owner selection.
Checked main, the modern launcher and rc6/rc7 task lists, and Claude's ai-memory notes.
Complete issue coverage: [issue triage](docs/ISSUE-TRIAGE.md).

Existing tasks reused; do not allocate replacements:
- #86 -> TASK-104 on the 1.3.0 line: the new chain-entry-7937 import failure needs follow-up.
- #89 -> TASK-105 on the 1.3.0 line: existing integrated-GPU/startup work, reporter validation pending.
- #98 -> TASK-025: deleted save returns; related to card persistence TASK-058.
- #88 -> TASK-018 and research TASK-123: native DFP/FFB remains planned for 1.3.1.
- #97 and #112 -> existing performance backlog TASK-041, related TASK-082/105/116/117/118
  and the CPU-parity TASK-119 on task/116-117-flare-perf. Retain both hardware reports.
- #95 -> TASK-053 for reuse of an existing texture directory, TASK-128 for optional external ISO.
- #92 -> TASK-080/TASK-079 for 4:3, TASK-130 for split-screen display.
- #87 -> existing TASK-057 mod-scope disposition and the owner's not-planned cheat policy.
  This is not approval or scheduling of a cheat feature.

TASK-104/105 exist in frozen rc6 commit 58322229c0bd63a0652b0550c5ad2154bd907039.
The CPU-parity TASK-119 is recorded in commit 9226927 on task/116-117-flare-perf, distinct
from main's RetroAchievements TASK-119 and task/101's player-one-controller TASK-119.
No existing task is renumbered, copied as a new task or silently dropped here.

### TASK-124 — Game cannot start (issue #107)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/107
Report: cannot start on both 1.2.4 and 1.3.0; screenshot and log bundle supplied. Cause unknown.
Priority: high, candidate for an upcoming 1.3.0 rc before final.
Related: TASK-105 and TASK-129 only if evidence establishes a shared cause; no duplicate confirmed.

### TASK-125 — GT Auto service audio keeps looping (issue #104)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/104
Report: oil-change/car-wash bubble audio persists after the service has finished.
Priority: normal backlog, release unassigned.
Related: distinct from missing-output-device startup behavior in TASK-129; no shared cause established.

### TASK-126 — Home key unavailable to ReShade (issue #102)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/102
Report: Home/Pos1 does not open the ReShade options while the game is active.
Priority: normal backlog, release unassigned.
Related: existing window/input tasks TASK-038 and TASK-076. No compatibility fix is claimed yet.

### TASK-127 — Flashing window followed by crashes (issue #99)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/99
Report: repeated flashing and random crashes in 1.2.2, 1.2.3 and rc5, for both games; logs attached.
Priority: high, candidate for an upcoming 1.3.0 rc before final.
Related: TASK-105 and TASK-124 remain separate until evidence confirms the same cause.

### TASK-128 — Optional use of an existing disc image (issue #95)

State: request recorded, not planned or approved for implementation (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/95
Request: reuse the original ISO without storing a second full copy. Claude had recorded the idea
without a task number; it remains an option under consideration, with no release assignment.
Related: the texture-directory portion of the same issue belongs to existing TASK-053.

### TASK-129 — Keep running without an audio output device (issue #93)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/93
Existing diagnosis: no Windows audio output causes the game to close. Desired behavior: run silently.
Priority: high, candidate for an upcoming 1.3.0 rc before final. Claude recorded the fix without an ID.
Deduplication: closed issue #94 is already a duplicate of #93. Issue #107 joins only if confirmed.

### TASK-130 — Split-screen display quality follow-up (issue #92)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/92
Report: split-screen resolution/quality concern; the reporter supplied the requested screenshot.
Priority: backlog, owner to select release priority. The old split-screen concern had no task number.
Related: 4:3 stays with TASK-080/TASK-079. Broader two-controller support remains the existing future
idea; this display intake does not silently schedule it.


### TASK-131 — Restore main to the released 1.2.4 program baseline

State: completed source restoration, owner-directed (2026-10-26).
Branch: task/131-restore-124. Restores program files directly from v1.2.4 / ad46df6;
preserves documentation and historical input pins. Git blob identity against that tag is
the validation; no build, hardware test, release or owner-data modification is performed.

Owner shelves the existing private 1.2.5 wheel draft. Public Latest stays 1.2.4. A future
1.2.5 will reintroduce only reviewed controller improvements from PR #100/#106 (8b73fa0),
with fresh packaging and acceptance; do not reintroduce the current wheel bridge.
This supersedes TASK-120/121 entries describing those changes as active on main and the
TASK-122 draft as a publication candidate. All contribution commits/authorship remain in
history (wheel integration cbfd8b8); native DFP remains TASK-018 with research TASK-123.

Contributor reply: https://github.com/moesuito/gt4-staticrecomp/pull/101#issuecomment-6422692723
TASK-018 and the TASK-123 brief were verified on main before linking. Requested native DFP
recognition (900 degrees) and original wheel FFB through SDL3, not pad-state/rumble adaptation.
No native wheel backend is claimed to exist, no new feature implementation dispatched.


### TASK-132 — Publish controller-only 1.2.5 from the reviewed PR #100 integration

Owner request 2026-10-26: replace the private wheel-containing 1.2.5 draft with a final
controller-only 1.2.5, after review and checks. Branch task/132-release-125-controls starts
from main749af82 and reapplies exact program blobs from reviewed integration8b73fa0
(PR #100 via #106), before wheel integration. No Git history is rewritten.

State: done, published stable Latest v1.2.5 (2026-10-26), PR #125 / merge5d7a669. Only launcher,
workspace lockfile, Tauri metadata and installer version change beyond that reviewed code.
Native game reuses immutable slice1200-pcsx2.json; fresh distribution slice1320-controls125-dist.
No wheel implementation or wheel UI is included; native DFP remains TASK-018 / research123.
RetroAchievements1.2.4 is retained. Legacy English UI exception remains owner-authorized;
matching en-US/pt-BR short notes precede the launcher build. Installer-only legacy release.

Observed checks: CTest43/43, Python641 tests/24 skipped, Rust381 passed/4 ignored, UI focus114/114
and navigation154/154 with no script errors. Official launcher build/path scan and installer
extraction/hash comparison passed. Publication follows the owner-authorized final release step.
No new physical controller, account-unlock or installer execution validation is claimed.
This supersedes the held TASK-122 wheel release candidate; keep its historical branch intact.

TASK-132 publication receipt: old private wheel draft deleted; new controller-only draft asset
sizes/SHA256 digests verified before owner-authorized final publication. Release v1.2.5 is
public, not a pre-release, Latest; tag points at5d7a669bdecf5e60c0782ca449f48aacac3c7a32.
https://github.com/moesuito/gt4-staticrecomp/releases/tag/v1.2.5
Installer SHA256556e7725d11146f1f79613814fbf961e1299000bd56df7c8831a324213003a74.
No new wheel support, Spec II, frame skipping or CoreGS changes are included.
### TASK‑119 — CPU: reach PCSX2 usage at 1x, then go below it (owner, 2026-10-26)
Owner decision (pinned in ai-memory `decisions/next-objective-cpu-parity-with-pcsx2`). On the weak laptop (i5-7200U,
HD 620, 1x, Direct3D 11, blending Minimum, MTVU on, no speed hacks) the same scene (Time Trial Nordschleife, Pitwork Z)
runs at **~50% CPU on PCSX2 with 60 FPS and the flares on**, against **70-80% and dips** on this port. GPU is the same
20-30%. A static recompiler must not use more CPU than the JIT: the gap is our overhead.

Order: **first match PCSX2** (about 50% on that laptop, 60 FPS, no dips, correctness unchanged), then go beyond it.
Where to look, in order: (1) per-instruction `c.budget -= N` / `preempt(c)` / `ld32`-`set32` in the generated code;
(2) the GS IPC, ~1,670 records per frame to a separate process plus journal ordering and frames-in-flight waits;
(3) FPU/MMI semantic wrappers per operation; (4) kernel/syscall and idle handling.
Acceptance: F8 on the laptop with the launcher idle, same scene and settings, EE CPU and EE critical path at or below
PCSX2's, 60 FPS without dips, identity gates still green. Baseline and measurement plan: ai-memory
`notes/pcsx2-baseline-weak-laptop`. Another agent is already working on the EE CPU side; coordinate before touching
the same files.


### TASK-133 — Integrate RetroAchievements and controller-only 1.2.5 into rc7

State: mapped/prepared, not implemented or dispatched (owner decision, 2026-10-26).
New rc7 scope: published rc6 + stable1.2.4 RetroAchievements + stable1.2.5 controller improvements.
OriginalGT4 v2.00 achievements only; Spec II stays disabled at both launcher and native boundaries.
All7 controller curves apply to both targets. No wheel bridge, native DFP or frame skipping.

Frozen base: v1.3.0-rc6 /58322229c0bd63a0652b0550c5ad2154bd907039.
References: v1.2.4/ad46df6; reviewed controller integration8b73fa0; finalv1.2.5/5d7a669.
Branch task/133-rc7-ra-controls, worktree C:/Antigravity/gt4-wt-133-rc7-ra-controls,
created directly from published rc6. Brief: docs/tasks/TASK-133-RC7-RA-CONTROLS-INTEGRATION.md.
Do not merge legacy main or the frame-skip branch wholesale into this candidate.

Preserve TASK082 on task/082-render-skip at15b7bf3971f4046faabc2d0cbd08768ae4883374,
PR113 OPEN/DRAFT, unmerged, report docs/TASK-082-RESUMPTION-REPORT.md. Resume later with
CoreGS2.0, without a version assignment. The former test082 kit is not the new rc7 candidate.

Acceptance: target isolation, account/logout/HTTP cancellation regressions, control persistence/
preview consistency, full native/Python/Rust and en-US/pt-BR checks, both target builds/pins,
installer+portable verification. New builds go to draft; owner gameplay/review before merge or
publication. No code, builds, running agents or public rc7 authorized by this mapping step.

### TASK-134 — Game-code optimization study (research and plan)

State: study done; owner decisions recorded (2026-10-28); P0 waits for the owner's signal that the desktop is free. Branch `task/134-game-code-optimization-study`, worktree
`C:/Antigravity/gt4-wt-134-game-code-study`, from main 795cdc0. Research and documents only: no code, build, benchmark,
profiler or game run (a CoreGS 2.0 session was measuring on the desktop). Agent status: not PCSX2-tainted.
Follows the owner's CPU-parity objective (PCSX2 ~50% vs GT4Recomp 70-80% on the i5-7200U), recorded as "TASK-119" in
commit 9226927 on `task/116-117-flare-perf` and in ai-memory `decisions/next-objective-cpu-parity-with-pcsx2`.

Deliverable: [docs/GAME-CODE-OPTIMIZATION-STUDY.md](docs/GAME-CODE-OPTIMIZATION-STUDY.md). Evidence: 13 desktop F8
blocks and the owner's laptop logs (6 race F8 intervals, ~200 periodic reports), copied to the worktree's ignored
`private/laptop-i5-7200u-f8/` with hashes.

Result: on the laptop race the four busy threads use ~35 ms of CPU per 16.7 ms frame (EE 11.7, VU1 worker 9.3,
gt4-gs CPU thread 3.4, gt4-gs GS thread ~11); half or more of the EE critical path (12.2 ms) is emulation plumbing, the
rest translated game code; no large spinning wait exists. Ordered plan P0 (measure) .. P8 (IOP AOT), with gains, effort,
risk and identity gates per step; GSdx-bridge costs are left to the CoreGS 2.0 intake (section 3.6).
Owner decisions (2026-10-28, study section 7): measure SSE4.1 first, no AVX2 executable unless a large gain; always
the easiest and cheapest step first, built and tested right away, the translator (now P8) last; thread priority and
core placement yes; the WPR profile and the first baselines run when the owner says the current CoreGS 2.0 build is
done; v2.00 is already on the laptop; no more work on the GSdx bridge, the game-side optimisations ship with CoreGS 2.0.
Test method: one fixed headless Arcade night race on the desktop (Clubman Stage Route 5, Ford Ka, 5 AI, plus its
replay for the varied cameras; B-Spec exists only in GT Mode, which the owner ruled out); laptop runs by the owner with F7/F8 on builds the agent copies to the laptop's
portable folder over the network, logs collected the same way.
2026-10-29: branch synced with 1.3.0-rc7 (`task/133-rc7-ra-controls` c21e934, program files identical); the
RetroAchievements per-frame hook added to the study (section 3.8, P1g). Next: P0 (measurement) and P1 (host quick wins) on this branch once the desktop is free; each later step gets its own
task or branch as it starts.

### TASK-135 — Performance HUD for CoreGS 2.0
Repos: this one. Clean room: a TASK-027 agent, not PCSX2-tainted.
Goal (owner, 2026-11-02): CoreGS 2.0 has no in-game overlay today. The overlay of TASK-062 is drawn inside `gt4-gs.exe` with the
PCSX2 OSD, so it exists only under GSdx and its code cannot be ported: this one is our own, MIT, in the game executable. It
shows what a player understands, CPU and GPU instead of EE and GS: graphics card, output resolution, render scale and refresh
rate; FPS with its minimum and maximum; frame time; CPU and GPU use of the game; memory; a live frame time graph. The owner's
reference is the look of the macOS Metal HUD (screenshots only).
Acceptance: off by default, launcher rows under CoreGS 2.0, overhead measured, the identity gates unchanged with it off, owner
test. Updating the GSdx overlay to match is separate work in the fork, for a PCSX2-tainted agent.
State (2026-11-02): candidate 378, off by default; 26.6 us per presentation, no measurable cost on the graphics thread (IN-526, ledger). Waits for the owner's test.

### TASK-136 — Simple onboarding, GSdx optional
Decision 0074. Owner, 2026-11-02: no GSdx download on a first install when the PC runs CoreGS 2.0; automatic, nothing to choose. Acceptance: a new install on a capable PC ends on CoreGS 2.0 with no download; a PC that cannot run it ends on GSdx, downloaded, and is told why; a player on GSdx or with a stored choice sees no change; owner test.
