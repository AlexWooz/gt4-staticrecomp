# Current Status

## TASK-215 H-shifter, direct gear and clutch mod study, deep follow-up (2026-11-11, branch `task/215-hshifter-deep`, read-only)

Owner directive: modify the game, not just remap buttons; add a real clutch pedal path and apply gears directly,
with R, N, 1..6 as gate positions. The real drivetrain is now mapped. The live gear is `Automobile + 0x624` (0 = R,
1..count forward; no neutral value, neutral is clutch open), the clutch state is `Automobile + 0x626` (0 open,
1 engaged, 2/3 slipping) and the engagement float is `Automobile + 0x618`; all are fields of the drivetrain block
at `Automobile + 0x104`. The HUD reads the same bytes from `RaceDisplay::update` (`0x0039DBB8`) through the player
car chain `RaceDisplay + 4 -> organization + 0x60 -> Cars + 8 -> item + 0x18`. The first study's `func_0037AD40`
gear path was the onboard camera (`CameraSys::CameraOnBoard`, vtable `0x0067A5D8`), and its HUD caller was
`RaceDisplay` slots 10/11/13/14, not gear/clutch; both are corrected. GT4 does have an automatic clutch state
machine and a torque gate on the gearbox force path, so a mod can cut torque and drive the states; continuous slip
torque would need a deeper force patch. Direct gear application is feasible by writing the gear byte with the
clutch open and clearing the shift step/delay, or by a one-instruction patch table that injects the host clutch
state, target gear and HUD R/N character through the existing `0x7F00` host block (TASK-080 pattern; candidate
words in the study, not built). R and N both use gear 0 with the clutch open/closed; the HUD gear-0 character can be
host-selected (`R`/`N`) with one patch. Replays store the control packet, not the gear/clutch, so the mod must be
disabled in replay, ghost and autodemo; saves are unaffected if car data is never written. Owner decision 0073
(2026-11-11) makes gameplay mods in scope and shipped in the same bundle, opt-in and off by default; the base game
and the faithful modes keep exact physics, timing, AI, saves and gameplay read-backs. The implementation can use a
separate derivative ELF/library profile inside that bundle. Details:
[H-SHIFTER-CLUTCH-MOD-STUDY](H-SHIFTER-CLUTCH-MOD-STUDY.md); the faithful button-emulation front stays in
[H-SHIFTER-CLUTCH-STUDY](H-SHIFTER-CLUTCH-STUDY.md). PoC estimate 4-6 weeks, medium risk; next step is a read-only
live probe (player index, gearbox type, VBlank ordering, N glyph) and the owner's scope decision.

## TASK-215 H-shifter and clutch study (2026-11-11, branch `task/104-hshifter-study`, read-only)

Study only; no game code, build, pin, save or card changed. GT4's input actions are sequential shift up/down and
reverse gear; the ELF action table has no clutch action and no H-gate action, and the supported DFP wheel report
(8 bytes: 14-bit steering, 14 buttons, hat, throttle, brake) has no clutch or gate channel. The HUD does have
`setGear`, `setClutch`, `setAutomaticTransmission` and `setShiftTimingLamp`, and `Automobile::isFullManualTransmission`
(0x003481C8) reads `Automobile + 0x546 == 6`; a gear-selection path around `func_0037AD40` uses a 12-entry table
at `0x006211E8` and a gear field at `+0x180` (player ownership and the physics gear field are Unknown). Recommended
front 1: button emulation (pad R2/L2 on main, DFP R1/L1 on the rc15 wheel work) with a pure shift sequencer and
the game's own timing; direct gear writes are a mod with high physics/replay risk. Front 2: no clutch input exists;
a real clutch would be a drivetrain physics mod with Unknown feasibility, so keep the pedal display-only. Details:
[H-SHIFTER-CLUTCH-STUDY](H-SHIFTER-CLUTCH-STUDY.md), journal 2026-11-11. Numbered TASK-215 because the 1.3.0 line already uses TASK-104 for
the launcher save import; the number needs confirmation before scheduling.

## Release 1.2.3 published (2026-10-22, hotfix: TASK-092, 093, 094, 096)

Game slice268 (the 1.2.2 game plus the CoreGS 1.1 crash fix and the MTVU test-switch fixes); launcher 1.2.3: GSdx only
(CoreGS 1.1 removed from the launcher, CoreGS 2.0 shown as coming soon), textures straight in `data\Textures` with
gt4-gs 1.2.1 (published earlier the same day), desktop shortcut from the installer. Installer
`GT4Recomp-Setup-1.2.3.exe` (sha256 `475defe07926f6e41e8f8c91fd4bb3565a17d6aac92f5c8fc857d0a143530a0b`). Owner tested.
Notes: [1.2.3](release-notes/1.2.3.md). Next: 1.2.4 (TASK-097, 098, 099), 1.3.0 wheels, 1.4.0 Spec II.

## TASK-096 on branch (2026-10-22, for launcher 1.2.3, waiting for the owner test)

Branch `task/096-remove-coregs11`: the launcher no longer starts CoreGS 1.1. GSdx is the only renderer; a missing or
unverified GSdx stops Play with a message that names the log folder, and a GSdx restart request does the same. A stored
CoreGS 1.1 choice becomes GSdx in settings schema 7. CoreGS 1.1 stays in the game executable, unused by the launcher.
Not merged.

## TASK-093 on branch (2026-10-22, waiting for gt4-gs 1.2.1 and the owner test)

Branch `task/093-textures-root`: custom textures go straight in `data\Textures`; the launcher no longer creates
`replacements`, `RENDERER_MINIMUM` is gt4-gs 1.2.1, and an old `replacements` folder is flattened into `Textures` at
start once gt4-gs 1.2.1 is in use (`gt4l-core/src/textures.rs`). Workspace tests and the Python gs suite pass. Not
merged; planned for release 1.2.3 with TASK-092.

## MTVU test switches fixed (2026-10-22, TASK-092, branch `task/092-mtvu-test-switches`, waiting for release)

Slice264 (not the player path): `GT4_MTVU=0` and `GT4_MTVU_VERIFY=1` in pcsx2 mode work again. One condition,
`vblank_journal_active()` (journal on, worker on, verify off), decides the GS client VSYNC deferral and the VBlank
drain. The doorbell handshake uses seq_cst. Default path identical to slice262 (dump hashes and GS stream hash equal in
menus and night). Open: two `gt4vulkan.exe` pin tests still point at slice262's executable; a slice264 pin set needs an
owner decision. Details: BUILD.md "Slice 264".

## Release 1.2.2 published (2026-10-22, TASK-084, PR #82)

Launcher 1.2.1 at version 1.2.2 with the TASK-084 game build slice262 (owner tested and approved). Installer
`GT4Recomp-Setup-1.2.2.exe` (28,862,899 bytes, sha256 `6ecab016197fa2869261f9d3fd4ca099cb974b576ff05e1dc7b284847628a35f`),
gt4-gs unchanged (1.2.0). Release notes: [1.2.2](release-notes/1.2.2.md). Follow-up: TASK-092 (MTVU test switches).

## Frame cost, runtime side (2026-10-22, TASK-084, DONE, branch `task/084-frame-cost-122`, owner approved)

The study's missing F8 instrumentation and items 2-4 are implemented; item 1 (deferred flare readback) stays out
by owner decision (readback work belongs to CoreGS 2.0). New runtime switches, all on by default:
`GT4_MTVU_CHAIN` (one MTVU job per VIF1 DMA chain), `GT4_GIF_FASTPATH` (no second GS stream walk),
`GT4_MTVU_VBLANK_JOURNAL` (VBlank as a journal entry in pcsx2 mode). Release-candidate build `slice262`
(pin `docs/inputs/slice262-pcsx2.json`; mode `pcsx2` points at it for the owner test). Checks: CTest 40/40; the
Python suite with the new `tests/python/test_frame_cost.py`; menus 41/41, day race 15/15 and night race 15/15 frame
dumps and the GS stream session hash identical to release 1.2.0 (slice240); `GT4_MTVU_VERIFY=1` with 0
mismatches in 4,200 frames. **Owner in-game test 2026-10-22 approved** (session
`session-20261022-163408-808184`, 0 late frames in every scene, picture unchanged): showroom critical path
6.06 ms/frame with 35.8 VIF jobs/frame, Nordschleife TT 480p 6.13 with 58.6, night race 9.97 with 80.4 (the
TASK-041 owner baseline was 11.8). The night-race download wait and the GS readbacks stay out of scope
(TASK-041 Performance Mode and CoreGS 2.0). Numbers, build recipe and the owner F7/F8 checklist:
[BUILD slice 260/262](BUILD.md). PR into main opened; the reviewer merges main (launcher 1.2.1) and builds
release 1.2.2.

## Release 1.2.1 prepared (2026-10-22, TASK-091, branch `task/091-install-layout-121`, waiting for owner test)

Owner decisions: per-machine install in `C:\Program Files\GT4Recomp` (administrator rights, any folder allowed); the
large data in `<install>\data` (found relative to `GT4Recomp.exe`, so the folder is portable; Users get Modify on that
folder only); settings, controls, saves and replays per Windows user in `%APPDATA%\GT4Recomp`. The launcher moves the
1.2.0 files at its first start (user data copied and checked, the Documents folder renamed with a README; game files
renamed or copied, checked and then removed) and removes the old per-user program install; the self-update starts the
elevated setup through the UAC prompt. Game build and gt4-gs unchanged (slice240, gt4-gs 1.2.0). Owner test installer
`private/slice250-installer/GT4Recomp-Setup-1.2.1-test.exe` (28,838,764 bytes, sha256
`333d72bcbc05066f020244b32c1c0a1e890da635451f757586498c1bf7316fb6`), not run on this PC; nothing published. Details:
[BUILD slice 250](BUILD.md), [TASK-091](../TASKS.md), [release notes 1.2.1](release-notes/1.2.1.md), journal 2026-10-22.

## Release 1.2.0 prepared (2026-10-21, TASK-088, branch `task/088-custom-textures-120`, waiting for owner test)

Owner decisions: custom textures in gt4-gs (load only; folder `Documents\Gran Turismo 4\Textures`, Graphics option
"Load custom textures" on by default, Advanced "Precache custom textures" off, "Open textures folder"), the bottom crop
of 2 native rows, and a black window from the first frame. Versions: GT4Recomp 1.2.0 and gt4-gs 1.2.0 (the launcher needs
gt4-gs 1.2.0 at least). Settings schema 6 (backup kept). Game build `slice240` (`docs/inputs/slice240-pcsx2.json`).
Local artifacts, nothing published: gt4-gs package `private/slice240-gt4gs-120/gt4-gs-1.2.0-windows-x64.zip`
(branch `gt4gs/textures-1.2.0`, commit `c9dc54e93`); owner installer `private/slice240-installer/GT4Recomp-Setup-1.2.0-test.exe`
(28,765,729 bytes, sha256 `0edcfefe3a143c180bdc1255357780d414aac9cd1e60bf65fc6b583648187ec7`); helper
`private/slice240-gt4gs-120/install-gt4gs-1.2.0-test.ps1` for the local gt4-gs. Details: [BUILD slice 240](BUILD.md),
[TASK-088](../TASKS.md), journal 2026-10-21. Owner test pending: white start, crop, the texture folder with a real pack.
## CoreGS independent launcher candidate (2026-10-21, TASK-089)

Branch `task/089-coregs-independent-launcher`, baseline main `6422988`. Selecting CoreGS 1.1 now
skips the optional PCSX2 GS installer during setup, automatic package repair at startup and background
renderer release queries. Launcher updates and explicit manual renderer updates remain available.
Play also avoids inspecting an unused external renderer executable. Synthetic setup/launch and injected
release-request controls pass: the full Rust launcher workspace passes on Windows, including 53 Tauri tests
(one real-disc flow ignored). Maintainer launcher acceptance remains pending. Branch audit and limits:
[renderer independence](reverse-engineering/renderer-independence-2026-10-21.md).
No renderer implementation, game executable, input pins or saves changed. CoreGS 2.0 is not enabled by this change.
Contribution: [PR #72](https://github.com/moesuito/gt4-staticrecomp/pull/72), from the
`JulioHeroZ/gt4-staticrecomp` fork; implementation commit `aa51f2a`. Awaiting maintainer acceptance.


## Launcher self-update fixed (2026-10-21, TASK-085, branch `task/085-launcher-self-update`)

Owner report: launcher 1.0.0 offered 1.1.0, closed after the download and installed nothing. The setup joined the
launcher's kill-on-close job and died with it; now it leaves the job (`CREATE_BREAKAWAY_FROM_JOB`, Explorer fallback).
Release v1.1.0 went back to draft (0 installer downloads) and was rebuilt: launcher GT4Recomp.exe 16,010,752 bytes,
sha256 `884f4e9b4a17e54ead031bca744534ca9f50e8b94d988e0ff92b83f897979c77`; installer
`private/release-110b-out/GT4Recomp-Setup-1.1.0.exe` 28,761,196 bytes, sha256
`6ad624307370998142bdfcacbd330f6cbe6a17e70e35c8ab5a2d44db8097da0d` (same game files, slice218). Launchers 1.0.0 cannot
self-update; the release notes tell those users to run the setup.

## Release 1.1.0 prepared (2026-10-21, branch `task/080-aspect-ratios`, draft release, not merged)

TASK-080 is done: the owner tested slice218 on ultrawide and approved ("funcionou perfeitamente"). Release prep on the
branch: version 1.1.0 (Cargo workspace, `tauri.conf.json`, NSIS default; release notes
[1.1.0](release-notes/1.1.0.md)); the launcher's Settings has no Display tab any more (Graphics holds the Display,
Graphics and Advanced sections; `#/settings/display` falls back to Graphics); the launcher's gt4-gs minimum is 1.1.0 and
an older install is replaced at launch (`gs_release::needs_install`); `version.json` is brought in line with the
installed versions at start. Game executable unchanged (slice218, pin `docs/inputs/slice218-pcsx2.json`). Installer
`private/release-110-out/GT4Recomp-Setup-1.1.0.exe` (sha256 `2ca0f80f8eb28023ffabe2446a30ffcdb6a139a0d96ac101fd6bc56fa7dd19de`) on a draft GitHub release v1.1.0; the coordinator
merges and publishes. Open follow-ups: P3 progressive menus (with TASK-078); split screen and photo mode in ultrawide
unverified. README: ultrawide headline, no emojis.

## TASK-080 ultrawide (2026-10-21, branch `task/080-aspect-ratios`, not merged)

Rebuilt on the 1.0.0 release recipe after merging main db99f9e: slice215 = `--mode pcsx2` on the branch (slice214 plus
native display aspect ratios: `GT4_DISPLAY_ASPECT`, auto by default; race camera Hor+/Vert+ hook; race view filled,
menus 16:9; protocol 1.3 for gt4-gs 1.1.0; launcher Aspect ratio row, settings schema 4 migrating the 1.0.0 file).
16:9 frames byte-identical to slice214. Owner test installer `private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe`
(app version 1.0.0, no GitHub release). Waiting for the owner tests and approval; planned as GT4Recomp 1.1.0. Details:
[BUILD](BUILD.md) slice 215 (and 212b), decision [0071](decisions/0071-native-aspect-ratios-runtime-hooks.md).

P2 (slice217, `--mode pcsx2` on the branch): the race HUD stays the game's 16:9 layout, undistorted and centred, at any
ratio (HUD canvas hook at `RaceDisplay::render`; race font x scale and mirror divisor through the ELF patch table and the
re-translated library profile `aspect`); split screen stays 16:9. Protocol 1.3 fill carries the target aspect so a custom
ratio is letterboxed (CoreGS 1.1 already was; gt4-gs needs a release that reads it). 16:9 byte-identical (menus 42/42,
race 160/160). New owner installer and the portable folder `private/ultrawide-test-portable`. Details: [BUILD](BUILD.md)
slice 217.

Addendum (slice218, `--mode pcsx2` on the branch, owner design 2026-10-21): seamless display. No Aspect ratio and no
Internal resolution rows: the aspect is always the chosen Resolution's shape (16:9 unchanged; other ratios turn P1/P2
on), Display has Monitor and Resolution (real monitors and modes), Graphics has Render scale 50-200% with the resulting
internal resolution (k = ceil(max(W/640, H/448)), whole multipliers 1..12). Game: `GT4_OUTPUT_SIZE`, `GT4_MONITOR`,
per-monitor DPI awareness. Settings schema 5. 16:9 byte-identical (menus 42/42, race 160/160). New owner installer and
portable folder. Details: [BUILD](BUILD.md) slice 218, decision 0071 addendum.

## Current state (2026-10-21)

**TASK-081 release 1.0.0 hardening (branch `task/081-release-1-0-0`, waiting for the owner's installer test):**
- Game: slice213 = the slice211 recipe with `-DGT4_RELEASE_DISTRIBUTION=ON` (no build-time shader folder in the
  executable; shaders only from `<exe folder>\shaders`). slice214 = slice213 plus the Vulkan baseline path (older
  drivers: swapchain maintenance 1 only when present, `GT4_VULKAN_FORCE_BASELINE=1`). `--mode pcsx2` = slice214,
  `pcsx2-slice213` and `pcsx2-slice211` kept. String scan
  clean; headless 42/42 frames identical to slice211; CTest and Python green ([BUILD](BUILD.md) slice 213).
- Launcher: release build through `launcher/tools/build-release.ps1` (`--remap-path-prefix`; no user or build path in
  `GT4Recomp.exe`); processor (SSE4.1, owner decision) and Vulkan 1.2 checks before setup and Play; the controls crate's built-in
  keyboard keys now match the TASK-076 layout (they were the old Z/X table, which a remap would have written to
  `controls.toml`).
- Installer: WebView2 detection plus Microsoft's Evergreen bootstrapper, name `GT4Recomp-Setup-1.0.0.exe`, the
  uninstaller removes `SDL3.dll`. Built in `private/release-100-out` with `SHA256SUMS`; draft GitHub release v1.0.0
  (not published). Release notes: [1.0.0](release-notes/1.0.0.md); steps: [RELEASES](RELEASES.md).
- Saves in Documents (owner request): saves and replays live in `Documents\Gran Turismo 4` (`Saves`, `Replays`; the
  Documents known folder, data-folder fallback); everything else, including the live `runtime\card.ps2`, stays in
  `%LOCALAPPDATA%\GT4Recomp`. An older AppData library is copied, checked (size, sha256) and parked as
  `library.migrated-<t>` at the first start; the guard protects the player's folder; the uninstaller never touches it.
  See [FLOW-AUDIT section 8](launcher/FLOW-AUDIT.md) and [MIGRATIONS](launcher/MIGRATIONS.md).
- Settings feedback (owner, 2026-10-21): 480p and 16:9 are fixed (the Input resolution, Widescreen 16:9 and GS
  Aspect ratio rows are gone; the launcher always sends `GT4_FORCE_480P=1` and `GT4_FORCE_WIDESCREEN=1` and writes
  `AspectRatio = 16:9`); PCSX2's own OSD rows are hidden and always written off (GS table column `forced`); Advanced
  is a section at the bottom of Graphics, not a tab. Old values are ignored by a load-time rule (settings stay
  schema 3). See [GS-SETTINGS](GS-SETTINGS.md) and [MIGRATIONS](launcher/MIGRATIONS.md).
- Next: the owner installs from the draft and tests end to end (ideally also an NVIDIA or Intel PC and a fresh Windows
  user), approves, then the merge and the publish.

## Earlier state (2026-10-17)

**TASK-044 (packaging, docs only, branch `task/044-packaging`):** release 1.0.0 ships gt4-gs 1.0.1 (`gt4gs-v1.0.1`, the
renderer already pinned for `--mode pcsx2`). The fork's release notes were edited (upstream base PCSX2 v2.8.2, commit
`fd9d310cc`; patch list; GPL-3.0 and source; SHA256SUMS). README (renderers, known issues, license and legal),
[THIRD_PARTY](../native/THIRD_PARTY.md) and [RELEASE-1.0.0-PLAN](RELEASE-1.0.0-PLAN.md) are updated. No build, no game run.
Release order: the launcher and installer (TASK-019, TASK-043), the owner's end-to-end test, then the v1.0.0 tag.

**slice206 (TASK-017 done, branch `task/017-license-crash`, main `ed123ed` merged in):** `--mode pcsx2` = slice206, the
slice203 game recipe (gt4-gs 1.0.1) plus the license demo guard v2 (capsule `private/slice206-license-guard-runtime`,
`ps2game.lib` unchanged). License demos that decrypt wrong (our FPU is not bit-exact, so 0 of 592 pass the game CRC; 52
of the 85 US demos land in the wrong phase) get record count 0xFFFFFFFF at both demo loaders (0x001100D0 and 0x00110230),
so the licenses are playable and those demos do not play, as in PCSX2 default. Memory-card loads check the CRC
themselves; replays and ghosts from a real PS2 card fail to load cleanly. Consumer map and remaining risk:
[LICENSE-DEMO-CIPHER](LICENSE-DEMO-CIPHER.md). Halt dump and FSV trace are built in, off unless GT4_HALT_DUMP /
GT4_FSV_TRACE. New `--play-card PATH` (a separate private test card, never `private/manual-play/card.ps2`). CTest
33/33 (native_memcard_store and native_license_guard now registered), Python suite with GT4_NATIVE_BUILD = slice206 all
passing. `pcsx2-slice203` and `license-guard` (slice205, guard v1) kept. Follow-up TASK-063: bit-exact div.s/mul.s.

Earlier (2026-10-16):

**slice203 (TASK-044, branch `task/044-gs-101`):** the slice202 game executable (same hash) with the stable gt4-gs 1.0.1
(tag `gt4gs-v1.0.1`, fork PR #7 merged as `d90c0f111`), which no longer writes under `Documents\PCSX2`. `--mode pcsx2` = slice203;
`pcsx2-slice202` kept. Checks: Python 472 run (18 skipped), CTest 29/30 (`python_native_contracts` uses the stale slice202 build path),
donor 42/42 = slice202, pcsx2 4200 frames 41/41 = slice202 (1x), no-JIT check of the 1.0.1 package passed, `Documents\PCSX2`
unchanged. [BUILD](BUILD.md) slice 203.

TASK-039 tooling follow-up (external contribution by Gustavo Canepa, PR #36, `fix/duplicate-gs-json-settings`): duplicate JSON setting ids now
raise the same error as duplicate CLI assignments. The 58 GS settings module tests pass, including
unchanged default INI output; no native build or gameplay validation was performed for this change.

**slice202 (main `b955235` merged into `task/060-062-osd`):** one candidate with the owner-approved TASK-058/059 (crash-safe
memory card, installed-folder paths, `--portable-check`, `make_distribution.py`) and TASK-060/061/062 (cursor, shader
notice, overlay) on the stable gt4-gs 1.0.0. CTest 30/30, Python 469 OK, donor 42/42, pcsx2 41/41 = slice201, the same
from the distribution folder, `--portable-check` ok. `--mode pcsx2` was slice202 until slice203 (gt4-gs 1.0.1). [BUILD](BUILD.md) slice202.

**slice201 (TASK-060/061/062, branch `task/060-062-osd`, fork `gt4gs/osd`, `gt4gs-v1.0.0` stable):** the mouse cursor
hides over the picture after 3 s without movement in fullscreen and windowed mode (game window and gt4-gs child agree
through protocol 1.2 CONTROL cursor; `GT4_CURSOR_AUTOHIDE=0` keeps it); gt4-gs shows "Compiling shaders…" bottom-left
while it really compiles (pipeline cache hits excluded); an optional top-right performance overlay (FPS, frame time,
CPU and GPU usage of the game and gt4-gs, resolutions, EE/VU1/GS write/readback/GS thread ms and the headroom) fed by
protocol 1.2 OSD_STATS (+0.35 us/frame on the EE thread, +0.02 ms/frame GS thread). Internal resolution presets 480p to
8K (default 1080p); gt4-gs 1.0.0 ignores INI keys outside the allow-list with a warning and is the **first stable
gt4-gs release** (zip sha256 `2e878019...`). CTest 30/30, Python 455 OK, donor 42/42, pcsx2 1x 41/41 = slice199,
no-JIT check passes. `--mode pcsx2` = slice201, `--mode pcsx2-slice199` kept. Owner windowed test pending.
[GS-LIVE](GS-LIVE.md) section 11.
**slice200 (TASK-058 crash-safe memory card, TASK-059 installed folder; branch `task/058-059-release-hardening` from main `3b5334d`):** `--mode pcsx2` = slice200 (`docs/inputs/slice200-pcsx2.json`, game build `private/slice200-build`, 95,680,000 bytes). The runtime writes the card atomically through the TASK-025 writer (memcard capsule, dirty retry, flush on exit and after the GS-loss grace VSYNCs). The executable finds `shaders\` and `gs\gt4-gs.exe` next to itself, has `--portable-check`, and `scripts/make_distribution.py` assembles the folder (`private/slice200-dist`, 16 files, no GS package, no game data; `docs/DISTRIBUTION.md`). Checks: CTest 29/29; Python kill test, portable-check and distribution tests; donor 42/42 identical to slice157; pcsx2 41/41 identical to slice199, both run from the distribution folder. The headless schedule never saves, so the card check is the fixture and kill test; the in-game save is the owner's test. `--mode main` is not rebuilt. No PR opened.

**slice199 (TASK-041 on the JIT-free gt4-gs 0.4.0, branch `task/041-gs-perf`, fork `gt4gs/nojit`, `gt4gs-v0.4.0`):** the slice198 game build (same file and hash) with the JIT-free gt4-gs 0.4.0 package of slice197. `--mode pcsx2` = slice199; `--mode pcsx2-slice198` and `--mode pcsx2-slice197` stay for A/B. Owner test of slice198 (gt4-gs 0.3.2) on 2026-10-16, night race with 5 AI: 59.97 FPS, 53 late frames (69 before), EE waits for the VU1 worker 2.99 ms (3.48 before), readback 0.69 ms, EE critical path 11.81 of 16.68 ms (71%), headroom 4.87 ms, no frametime spikes once the shader cache was warm. CTest 29/29, Python 438 OK, 20 skipped; donor 42/42 identical to slice157; pcsx2 1x 41/41 identical to slice198; no-JIT static check passes on the package. [GS-LIVE](GS-LIVE.md) sections 9 and 10. Owner night-race check on 0.4.0 pending.

**slice198 (TASK-041, branch `task/041-gs-perf`, fork unchanged, `gt4gs-v0.3.2`):** performance of the PCSX2 GS path.
A headless race schedule (El Capitan, 5 AI) reproduces the night-race privileged-write pattern: the 3.48 ms/frame
`gs_privileged` wait is the EE waiting for the frame's VU1 work at the CSR write right before the flare readback.
`GT4_MTVU_PRIV_ASYNC=1` (default) queues privileged writes in the MTVU journal: byte-identical stream and frames, the
gs_privileged wait is 0 but moves to the readback's download sync (no net gain expected in the night race). Long
readbacks are pipeline compiles (cold cache max 58 ms, warm 2.7 ms; TASK-046). Wake batching (`GT4_GS_WAKE_BYTES`)
cuts GS wake-ups from 322 to about 12 per frame and `gs_ipc_write` by about 30% per MB. F8 `[gs-ipc]` gains
per-register lines and an EE critical path summary. CTest 29/29, Python 416 OK, donor 42/42, pcsx2 41/41.
Owner test 2026-10-16 (see slice199 above). `--mode pcsx2` was slice198 until slice199. [GS-LIVE](GS-LIVE.md) section 9.

**slice197 (TASK-042, branch `task/042-gs-nojit`, fork `gt4gs/nojit`, `gt4gs-v0.4.0`):** JIT-free `gt4-gs.exe`. Build
option `GT4GS_ONLY`: no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software JIT (C rasterizer), no executable
memory; Windows Arbitrary Code Guard on at start (works with the AMD Vulkan driver). Static check passes (0.3.2: 22
markers); replayed menu frames byte-identical to 0.3.2 at 1x and 3x; GS thread time unchanged within noise. CTest
29/29, Python 414 OK, donor 42/42, live pcsx2 41/41 = slice196. `--mode pcsx2` was slice197 until slice199 (same game runtime). Owner
windowed check pending. [GS-LIVE](GS-LIVE.md) section 10.


**slice196 (TASK-047, branch `task/047-gs-deinterlace`, fork `gt4gs/deinterlace`, `gt4gs-v0.3.2`):** 480i menus in the
PCSX2 GS mode. Cause: the stream had no SYNCV (PCSX2 never deinterlaced; fields line-doubled and bouncing) and,
once SYNCV is sent, gt4-gs 0.3.1 used the previous VSYNC's field (wrong order, combing). Fix: SetGsCrt posts SYNCV;
protocol 1.1 VSYNC field = display field; gt4-gs 0.3.2 uses it. Garage menu: comb 0.50 -> 0.57 with flicker 3.23 ->
0; progressive unchanged. CTest 29/29, Python 410 OK, donor 42/42. `--mode pcsx2` = slice196. Owner menu check
pending. [GS-LIVE](GS-LIVE.md) section 8.

**slice195 (TASK-038 click fix):** owner night race with slice194: accurate 59.97 FPS, 69 late frames (was 213),
unsynchronized 60.00 FPS but headlight flares missing; accurate stays the default. A click on the picture deactivated
the game window; gt4-gs 0.3.1 gives the foreground back (visible click fixture passes). `--mode pcsx2` = slice195.

**TASK-037/038 (slice194, branch `task/037-038-gs-window`, fork `gt4gs/window`, tag `gt4gs-v0.3.0`):** night-race late
frames explained (one readback per frame removes the GS buffering, so shader/pipeline compiles and GS-thread spikes
land in the game frame; the shader cache was in `Documents\PCSX2`, not in our folder). Readback modes
`--gs-readback accurate|unsynchronized`, F8 readback histograms, our own cache/data folders, deferred swap chain
acquire, window hardening (detached input queues, minimize, close while the GS window hangs) and crash handling
(card flush grace, restart offer). CTest 29/29, Python 350 OK, donor 42/42, pcsx2 41/41. Owner windowed test pending.
[GS-LIVE](GS-LIVE.md) section 6.

**TASK-036 (slice193, branch `task/036-gs-live`, fork `gt4gs/live`, 2026-10-16):** the live PCSX2 GS process
`gt4-gs.exe` runs the game's GS stream (`--mode pcsx2`, `--gs-scale N`). Headless: CTest 29/29, Python 335 OK,
donor frames 42/42, live frames 41/41 identical to the offline `--replay-gscap` frames and frames 300/700 identical
to the TASK-034 PoC; a killed GS process halts the game cleanly. Owner windowed test at 1x and 3x pending.
[GS-LIVE](GS-LIVE.md).

## Earlier state (2026-10-15)

**Current renderer plan (2026-10-15, [decision 0070](decisions/0070-pcsx2-gs-separate-process.md)):** TASK-032 (decision docs) is done in its PR; TASK-034 (PCSX2 GS proof of concept) is in progress, with an owner go/no-go gate before TASK-035/036. paraLLEl-GS (TASK-012) is dropped; CoreGS 1.1 stays the built-in fallback. Plan: [GSDX-PLAN](GSDX-PLAN.md).

**TASK-035 (slice191, branch `task/035-gs-stream`, 2026-10-15):** the MIT GS stream protocol ([spec](GS-STREAM-PROTOCOL.md)), shared-memory transport and the `GT4_RENDERER=pcsx2` seam are built and checked headless (CTest 29/29, Python 321 OK; donor frames 42/42 identical; the stream is byte-identical in donor mode, pcsx2 mode and in the recorder process; the converted stream equals the TASK-034 PoC dump and replays to identical GSRunner frames). The consumer is a test recorder, so `--mode pcsx2-stream` shows no image until TASK-036. Owner review pending.

**Latest (merged to main):** TASK-008 slice 157 (all 2,048 VU1 entries, owner: night
58.9, rally 59.9, showroom 60.0 VBlanks/s, 0 interpreted VU1 calls), TASK-010 build
pipeline (slice 159) and TASK-009 slice 164 `--mode quickfix3 --force-480p` (title,
16:9, fullscreen, races held at 480p; owner approved). In flight: TASK-012 paraLLEl-GS
(step 5 headless, textures missing), TASK-013 VU0 AOT (owner test), TASK-014 stage A.
TASK-011 MTVU (branch `task/011-mtvu`, release 1.0.0 W1): slice 178 `--mode main` runs the VU1 worker
(slice 172 runtime, `GT4_MTVU` default on, `GT4_MTVU=0` is the A/B switch) and holds 480p by default.
Headless 42/42 identical with MTVU on (three runs), off (one run) and default. PR ready for review; owner A/B and approval pending.

**Earlier:** slice 154 `--mode vu1-flags` (executable `800da980`) adds VU1 flag
elision: showroom 59.14 VBlanks/s (was 44.79), 5-AI race 60.00 at 480p. Next:
full static VU1 entry coverage: slice 155 `--mode vu1-full` (426 entries) is
built and checked headless, waiting for the owner test.

2026-10-15 — Slice 154, O1 follow-up (candidate, not owner-measured): `--mode vu1-flags` = slice 153 pacing with a VU1 table that computes MAC and clip only where a reader or the exit observes them (exit state exact); VU1 status is not kept, since the image never reads it and the EE cannot read VU1 flags. Local evidence: corpus differential 0 failures over 100 programs, fixture differential 0 failures, 42 identical headless frames versus slice 153, verify smoke with 0 mismatches. Read [journal 2026-10-15](journal/2026-10-15.md) and [BUILD](BUILD.md).

Integration branch `main`. Machine: Ryzen 7
5700X, RX 9060 XT. Plan: [OPTIMIZATION-PLAN](OPTIMIZATION-PLAN.md). Journal:
[2026-10-15](journal/2026-10-15.md).

**Playable candidate:** slice 153 `--mode pacing` (executable `c8d31fdf`):
VU1 AOT (102 entries) + asynchronous GS submission + single-clock pacing with
rate-controlled audio. A/B switches: `GT4_PACING=audio`, `GT4_GS_SYNC=1`,
`--vu1-interpret-only`, `--vu1-verify N`, `GT4_PRESENT_MODE`.

**Confirmed (owner measurements):**

| Scene | VBlanks/s | Speed | Notes |
|---|---|---|---|
| Time Trial, Nordschleife, Supra, 480p | 60.00 | 100% | median 16.665 ms, 99.6% of intervals 16-17 ms |
| Arcade race, Nordschleife, 5 AI | 57.98 | 96.6% | VU1 8.4 ms |
| Replay | 59.10 | 98.5% | |
| Night race | 46.15 | 76.9% | uncompiled VU1 entries (3.7 ms interpreted) + 4.1 ms GS wait (readback suspected) |
| Rally, Grand Canyon | 53.85 | 89.8% | uncompiled VU1 entries, VU1 11.3 ms |
| Car showroom | 44.79 | 74.6% | VU1 16.2 ms, 46k primitives/frame |

Starting point the same day was about 30 FPS and 51% speed (VU1 interpreted
17 ms, synchronous GS wait 10.8 ms). VU1 AOT verified on real game state
(140,034 in-game comparisons, 0 mismatches). RTSS overlay works on AMD since the
swapchain requests color-attachment usage.

**Known issues:** license tests crash in every configuration (pre-existing;
traced to `RaceSolitaire::loadDemoInputs` → `AutomobileControlRecord::Manager::load`
reading a bad record count); night/rally VU1 entry coverage; showroom VU1 load;
night GS readback wait; counters `late_frames`/`audio_underruns` look
session-cumulative.

**In progress / next:** VU1 per-program flag elimination (slice 154, subagent);
then full static VU1 entry coverage, MTVU (VU1 thread, default on), window title
"GT4Recomp" and fixed 16:9 (remove F6/4:3), license-crash instrumentation, LDL/LDR
semantics check, GS readback counting/staging, Vulkan (GS) internal scale 2-3x
experiment, then O3 (VU0 AOT, EE entry coverage, IOP review, build tuning).

**Research done (docs):** [VU1 microcode source](VU1-MICROCODE-SOURCE.md),
[VU0 map](VU0-MICROCODE-MAP.md), [IOP map](IOP-MAP.md),
[decomp identity](DECOMP-IDENTITY.md), [decomp integration](DECOMP-INTEGRATION.md),
[GS async design](GS-ASYNC-DESIGN.md), [PCSX2 lessons](PCSX2-LESSONS.md),
[hardware renderer plan](HW-RENDERER-PLAN.md), [Spec II study](SPEC2-STUDY.md),
[menus and wheel](MENUS-AND-WHEEL.md), [online notes](ONLINE-NOTES.md).

## History (newest first)

2026-10-15 — Slice 178, release 1.0.0 W1 (candidate, not owner-measured): `--mode main` is the build of main with the
MTVU worker runtime (slice 172 capsule, byte-identical; `GT4_MTVU` default 1, 0 is the single-thread path) and holds the race
display at 480p by default (hidden opt-out `GT4_FORCE_480P=0`). Build `private/slice178-main-build` (pin `docs/inputs/slice178-main.json`),
CTest 27/27, Python 281 OK (12 skipped), headless 4,200 frames 42/42 identical to slice157 in four runs (on x3, off x1) plus one
default-environment run. `--mode mtvu` keeps slice 172. Details: BUILD.md "Slice178", MTVU-PLAN section 12.

2026-10-15 — Slice 153, O4 frame pacing (candidate, not owner-measured): `--mode pacing` (VU1 AOT plus asynchronous GS, as slice 152) with one 60 Hz wall-clock guest VBlank grid, non-blocking audio (16 x 256-frame buffers, 60 ms target, +-0.5 % resample control), and `GT4_PACING=audio` for the old blocking clock. FIFO stays default; `GT4_PRESENT_MODE` for experiments. Local checks: pacing and paced-device fixtures, CTest 18/18, Python 217 OK, 42 identical headless frames versus slice 152. Owner measurement pending. Read [journal 2026-10-15](journal/2026-10-15.md) and [BUILD](BUILD.md).

2026-10-14 — Slice 152, O2 asynchronous GS submission (candidate, not owner-measured):
`--mode gs-async` = VU1 AOT plus a three-slot submission ring on one timeline semaphore,
presentation merged into the slot submission, at most two presents in flight, cache
eviction guarded by completion, and `GT4_GS_SYNC=1` for synchronous A/B. Confirmed headless:
4200-frame dumps identical sync vs async; stress VRAM byte-identical; CTest 17/17; Python 203
OK. Synchronization validation is clean with overlay layers disabled; with the installed
overlay layers it reports present-after-write (pre-existing class). Step 0: swapchain now
requests COLOR_ATTACHMENT (overlay layers previously hit render-pass VUIDs). Owner gameplay,
overlay check and frame timing pending. Read [GS async design](GS-ASYNC-DESIGN.md#implementation-status-slice-152).

2026-10-14 — Slice 151, owner replanning: [Optimization Plan](OPTIMIZATION-PLAN.md)
and [decision 0065](decisions/0065-aot-all-guest-code-no-jit.md) set the order of work:
VU1 AOT first (donor recording + `vurecomp` flow never activated for GT4; all VU1
calls hit the interpreter), then asynchronous GS submission, then VU0-micro/IOP AOT
and EE build tuning. Read-only source review; no build, run or code change.
Branch `codex/optimization-plan-151` from slice 150.

2026-10-14 — Slice 150, owner split report identifies VU1 interpreted wall cost.
Playable base remains slice114; slice143 includes slice136's keyboard correction.
Active track: `codex/hybrid-no-jit-port-plan-141`; slice143 based on5c7ab92.
`main` retains slice140/native-scene work; this candidate stays on the hybrid track.
Slice 118 candidates remain separate and owner gameplay qualification is pending.

## Active priority on this branch

**Confirmed (slice143):** portable timing core is connected to a separately pinned
production-EE player. Measures exclusive/inclusive wall categories and owner-thread
CPU time, clips active scopes and guest counters at VBlank boundaries, preserves
keyboard correction, excludes shape capture and inherited guest tuning settings.
F7 starts a continuous manual interval, F8 reports, F9 pauses/resumes diagnostics.
Candidate native CTest7/7; separate macro-off observation controls9/9,
Python171/171 zero skips, warning-free builds. Launcher retains card/backup/lock flow.
**Confirmed (slice144):** owner F8 report saved completely:970/970 samples across
40.323s,24.06 VBlanks/s; no invalid/reset marker. Exclusive wall sum reconciles
within printed rounding. VU interpretation25.646ms/sample is61.69% of the interval;
GS fence wait9.392ms/sample is completion latency, not GPU execution time.
Owner identifies that interval as Toyota Prius, external camera, Nordschleife;
year/trim, exact position and GT4 video setting are not supplied.
**Unknown:** diagnostic overhead and exact interval scene composition,
distinct scene frame rate and GPU timestamp time. No performance improvement
claim. VU is the leading observed wall category; qualify overhead and the matching
scene before choosing an optimization. No runtime changes in this verification.
**Confirmed (slice145):** fresh detailed and frame-only profiles share production
EE/runtime/port/shaders and all guest settings. Frame-only compiles subsystem
scopes and host operation counter hooks out; common boundary CPU/wall clocks and
guest progress counters remain. Native CTest7/7 each, separate full9/9 with
Python173/173 zero skips. Object-symbol controls confirm no Probe references in
five frame-only hot-path objects. Owner closed143 normally and completed the145
detailed trial recorded below. Matched control intervals remain pending.
Next: owner matched detailed/control/detailed intervals, then VU0-micro/VU1 timing
separation and one content-identified offline AOT experiment. No speedup claim.
**Confirmed (slice146):**145 detailed manual interval covers962 samples/40.732s,
23.62 VBlanks/s, median42.065ms; VU26.246ms (61.99%), GS fence9.392ms,
owner-thread CPU32.533ms/76.8%. SecondF8 covers only2.704s, recorded separately.
Independent review and arithmetic controls pass; detailed session closed normally,
card hashes unchanged. Owner confirmed closure;145 frame-only now open through
protected launcher. Its manual interval/overhead result remain pending.
Owner screenshot is a single logical-CPU/overlay snapshot: no fixed CPU at100%
does not rule out serial work with waits/migration; neither migration nor thermal
limiting is measured. See [detailed evidence](inputs/slice146-owner-detailed-results.json).
**Confirmed (slice147):**145 frame-only report covers1175 samples/56.199s,
20.91VBlanks/s, median46.311ms,p9983.614ms, threadCPU37.274ms/77.9%.
Complete five-line report, normal closure, unchanged card and independent review
pass. It is slower than detailed in this one sample; different durations/scenes/
machine state prevent causal overhead attribution. Protected145 detailed repeat
now open, owner controls F7/F8. No optimization or thermal-cause conclusion.
See [control evidence](inputs/slice147-owner-control-results.json).
**Confirmed (slice148):** detailed repeat1191 samples/53.161s, median43.779ms,
p9956.683ms, threadCPU35.094ms/78.6%, VU28.155ms. Normal close/card unchanged;
exclusive partition and independent log checks pass. Initial sequence medians
42.065/46.311/43.779ms: control slower than both detailed runs. Causal overhead
remains Unknown under varying scenes/durations/machine state; no more repeats of
this initial sequence requested now. Next is a fresh VU0-micro/VU1-interpreter
scope split, keeping149+ candidate separate from frozen143/145. No optimization.
See [sequence evidence](inputs/slice148-owner-aba-results.json).
**Confirmed (slice149):** fresh pinned source capsule replaces aggregate run_fast
timing with disjoint VU0-micro/VU1-interpreter categories. Old aggregate stays zero;
derived rollup is separate from the16-category partition. Core controls prove70us
aggregate equals20+50us split after active cut/nested waits. Native7/7, separate
full9/9 with Python175 zero skips. Initial category rejection was caught/corrected
by fixtures/review. Frozen143/145 unchanged; no algorithm/clock optimization.
Next owner F7/F8 interval identifies the slower interpreter unit. VU0 macro in EE
and generated VU1 AOT are outside this scope. [Candidate](inputs/slice149-vu-unit-attribution.json).
Independent final artifact gate passed; protected149 player initialized native
Vulkan/audio with verified backup, then owner completed and closed the150 session.
**Confirmed (slice150):** completed149 interval1139 samples/49.161s,
median42.711ms. VU1 interpreted26.987ms is62.53% of interval wall; VU0 micro
prints0.000ms (rounded, not proof of no calls). VU0 macro remains in EE; generated
VU1 AOT excluded. GS completion wait9.367ms is next largest category. All16 terms
sum within rounding; normal close/card unchanged. [Evidence](inputs/slice150-owner-vu-split-results.json).
Owner additionally reports menu pacing sometimes43FPS then60FPS in the same
opening. Screenshot/description recorded only; cause and transition timing Unknown.
Next: bounded VU1 content-key/call inventory before selecting one offline-AOT
program. No menu audit/fix or runtime optimization in150; no new game launch.
See [pair protocol](PERFORMANCE-ATTRIBUTION.md#matched-overhead-pair-slice145)
and [VU AOT feasibility](VU-AOT-FEASIBILITY.md).
See [attribution](PERFORMANCE-ATTRIBUTION.md#playable-diagnostic-candidate-slice143)
and [candidate pins](inputs/slice143-performance-candidate.json).
Protected candidate launch confirmed native Vulkan/16:9/48kHz initialization and
per-session card backup; owner controls the open game. Manual report and immutable
observed prefix preserved privately; see [report evidence](inputs/slice144-owner-performance-report.json).
Slice142 reconciled OpenCode's old executable/receipts and reproduced accounting
defects; its reported494-interval selection remains unreproduced.

Owner changed direction: optimize the hybrid base for correct game speed and
stable60FPS on Windows, then add GS internal scaling/dithering and portability.
Mobile targets: S23 Ultra/Snapdragon8 Gen2 and iPhone15 Pro Max; Windows first.
Guest JIT is excluded. AOT EE and selectively compiled VU may coexist with
qualified instruction interpreters/device models. No zero-emulation claim.
Selective PCSX2 GS implementation reuse is authorized; GPL-compatible derivative
planning and attribution are required before imports. No code/license change yet.

Independent Luna source review confirms Windows host/COFF pins, Win32 WSI and
mandatory presentation-extension dependencies. Donor has offline VU compilation
and scale-aware1..3 GS code, not qualified in our current1x adapter. Costs and
ARM64/mobile60FPS remain Unknown. No profiling/build/game action in this slice.
See [Hybrid Port Plan](HYBRID-PORT-PLAN.md), [decision0064](decisions/0064-hybrid-no-jit-windows-first.md)
and [next H0/H1 package](tasks/HYBRID-H0-H1.md).
Read-only source hashes/review limits are published in
[slice141 evidence](inputs/slice141-hybrid-port-plan.json).

The game-scene replacement research below is preserved and deferred on this
track. Its next richer shape capture is not the default next task here. Existing
OpenCode experiments stay separate; review their measured evidence before reuse.

## Authorized P0/P1 Progress

- **Confirmed (slice 140):** exact pinned offline/capture comparison and independent
  Luna parsing align18 of22 menu Ford Ka shapes at candidate MDLS base0x01748740,
  accounting for97 calls. Other53 menu and all60 race-LOD candidate bases match
  one distinct address each. Four menu shapes are unobserved;12 other live addresses
  are outside the candidate menu extent. Ten authored matching controls pass.
- **High confidence:** partial loaded menu-asset layout correspondence.
  **Unknown:** resource identity/lifetime, descriptor relocation/payloads, car-part
  roles, camera/transforms and geometry. No runtime/build/session/performance change.
  Next: bounded prefix/descriptor observation; see [crosswalk](LIVE-SHAPE-CROSSWALK.md)
  and [evidence](inputs/slice140-shape-crosswalk.json).

- **Confirmed (slice 139):** owner capture exports valid schema1,180767 bytes,
  256 completed records, all before/after validity masks0x3f. Started/completed257,
  dropped1 reconcile with automatic record-cap stop; no orphan/abort/rejected/
  ambiguous/incomplete sample counters. Independent bounded JSON review matches.
- **Owner context:** screenshot shows Ford Ka '01 in Arcade selection and
  `capture saved`. **Unknown:** which of30 distinct raw shape arguments belong
  to that car, geometry and submission ownership. No timestamps/frame IDs exist;
  capture speed/duration is not measured. Original preserved and frozen private
  copy hash-matched. Game remains owner-controlled. See
  [capture verification](inputs/slice139-live-capture.json).

- **Confirmed (slice 138):** owner reported capture began before the car appeared
  and explicitly requested close/reopen. Delivered normal close; exit0/session
  completion confirmed, previous capture preserved. Qualified136 reopened with
  the same save, fresh backup/session and capture off. This authorization covers
  this restart only; owner still controls scene navigation and F8. Target scene
  remains unqualified. See [retry evidence](inputs/slice138-owner-capture-retry.json).

- **Confirmed (slice 137):** owner requested opening the corrected build. Old134
  session closed normally with exit0; qualified136 runtime launched with existing
  save and a fresh8650752-byte backup. Native window/audio initialization logged.
  Owner controls keyboard and F8; no agent scene navigation or performance probe.
  Menu/steering/capture identity confirmation remains pending. See
  [corrected launch evidence](inputs/slice137-corrected-owner-launch.json).

- **Confirmed (slice 136):** owner separately authorized the input fix blocking
  car selection. Arrows previously emitted both a digital D-pad bit and a full
  left-stick axis; keyboard merge now emits only buttons/pressures. XInput axes,
  rumble and all other button mappings remain unchanged. No arbitrary debounce.
- **Confirmed (slice 136):** fresh CTest7/7 in13.03s and Python164/164, zero skips.
  Native controls cover all16 keyboard bits, four directions, hold/release/chord
  and preservation of non-neutral XInput axes. The same new fixture rejects the
  old implementation specifically for non-neutral keyboard-derived axes.
- **High confidence:** duplicate digital/analog channels explain the reported
  menu double movement. **Unknown:** owner keyboard/menu confirmation and digital
  D-pad steering in races. The strict capture launcher now selects the frozen136
  candidate; physical input/game navigation remains owner-controlled.
  See [keyboard evidence](inputs/slice136-keyboard-input.json).

The owner authorized P0/P1 while OpenCode continues its separate performance
branch. Work uses an isolated checkout; no OpenCode edits, session or saves changed.

- **Confirmed (slice 131):** all 328 reviewed two-probe sources copied into a
  fresh producer and all 327 AMD64 objects rebuilt. Exactly 326 normalized object
  payloads match baseline; only D5 changes and matches its independent slice 129
  unit compilation. Compiler recipe differs only in generated-source directory.
- **Confirmed (slice 131):** producer CTest 1/1, Python 155/155 with zero skips,
  using frozen slice 130 native fixtures. This does not repeat their native build.
- **Unknown:** retail runtime linkage, live shape identity/reachability and capture
  overhead. The ordinary playable target and launcher remain unchanged.
  See [full library evidence](inputs/slice131-observation-library.json).
- **Confirmed (slice 132):** fresh `observation` native profile links the pinned
  full probe library and reviewed runtime overlays directly into its game target.
  Ordinary production selection is unchanged; the runtime archive stays original.
  Capture starts disabled and empty; activation/export remain unimplemented.
- **Confirmed (slice 132):** CTest 5/5 and Python 155/155, zero skips. Actual game
  map/graph checks cover retained EE hooks, boot, adapter and D5. Synthetic guest
  outputs still match their unoverlaid control. Invalid profile/missing overlay,
  extra header and post-configure source edit are rejected before compilation.
- **Unknown:** this candidate's boot/gameplay, live shape association and overhead.
  No game session launched. See [runtime evidence](inputs/slice132-observation-runtime.json).
- **Confirmed (slice 133):** fixed metadata-only JSON preserves every raw argument
  lane, FPR/SP bits, original owner identities, counters and before/after validity.
  It requires a stopped observer and exclusively creates a new file. Actual native
  controls cover 0/1/256 records, no overwrite, invalid reason and missing parent.
- **Confirmed (slice 133):** fresh CTest 6/6, Python 156/156, zero skips. Exporter
  remains fixture-only; no window hook, game activation or owner capture added.
- **Unknown:** injected write/flush/cleanup/close failures and crash atomicity;
  partial cleanup may fail. The host path is caller-trusted; no public path UI.
  Next: owner capture latch at the EE vblank boundary and fresh private launcher
  output, then owner-led scene evidence. See [export evidence](inputs/slice133-capture-export.json).
- **Confirmed (slice 134):** observation-only F8 latches a start/stop request;
  the EE-owned vblank host boundary services it. One capture per launch, default
  off; manual, capacity/ambiguity stop and shutdown preserve/export fixed records.
  Failed export retains records for F8 retry or one best-effort shutdown attempt.
- **Confirmed (slice 134):** launcher pins the exact manual manifest/runtime,
  clears inherited capture output, uses a new private session path and preserves
  the existing card/backup/locks. CTest 7/7 in11.72s, Python164/164, zero skips.
  Link evidence retains the control/export code; baseline graph excludes it.
- **Unknown:** physical F8, actual scene reachability/identity and runtime cost.
  Synchronous bounded export may pause a frame; no gameplay/performance probe was
  used for build qualification. Owner-led capture is next. See
  [manual capture evidence](inputs/slice134-manual-capture.json).
- **Confirmed (slice 135):** qualified134 candidate launched after checking no
  existing game process. Native window/Vulkan, Windows audio and guest IOP boot
  services are present in the startup log. Existing owner save selected; session
  backup8650752 bytes created. Capture starts off; no capture file at snapshot.
- **Unknown:** menu/race scene, physical F8 and real shape identity. No agent GUI
  navigation/input or performance measurement. The owner controls the open game.
  Startup proof is frozen separately from changing live session logs:
  [launch evidence](inputs/slice135-owner-capture-launch.json).

- **Confirmed:** fresh CORE-to-ELF reconstruction matches the pinned native ELF;
  independent verified ISO extraction produces the same CORE/ELF.
- **Confirmed:** the donor translator rebuilt with Clang 23.1.2 generates all
  328 source files identically to the frozen producer. No old CMake cache is used.
- **Confirmed:** the fresh library's 327 AMD64 object payloads match the baseline
  after normalizing only their COFF timestamps; archive paths explain the size delta.
- **Confirmed (slice 117):** producer CTest 2/2 passes; Python discovery 55 tests,
  52 passing and 3 native integration skips in the isolated checkout. These are
  not candidate runtime or gameplay qualification.
- **Confirmed:** seven model/camera candidates map to real generated registry
  entries and prefix hashes; camera helpers and model VM/callbacks have state effects.
- **Confirmed:** baseline callback starts `0x0044EA58`/`0x0044EAC8` lack generated
  bodies/entries. An offline supplier-only seed recovers both, adding 45 instructions
  and nine entries. Supplier-only/both source matches; all 18 changed units compile.
  Existing bodies/callers match under independently reviewed dispatch-ID normalization.
  Production seeds/library are unchanged. The donor EE dispatcher halts on missing code without a module
  replacement; it has no EE interpreter fallback.
- **Unknown:** live reachability of these omissions and any performance relationship.
  Generation equality is not complete-code coverage or PS2 fidelity proof.
- **Confirmed (slice 118):** complete recovered library compiled in a fresh cache;
  all 327 normalized AMD64 object members checked: exactly 18 reviewed differences,
  309 unchanged. Baseline remains the qualified rebuilt slice 116 library.
- **Confirmed (slice 118):** both isolated native runtime profiles link and pass
  CTest 3/3 and Python 59/59 with zero skips and zero observed compile warnings.
  All nine shaders match production. The actual candidate executables are used
  by native integration fixtures; no manual game session was launched during build qualification.
- **Confirmed (slice 118):** bounded research located the actual GT4.VOL header/
  offset table; parent independently verified its metadata. Page decoding, named
  car/asset/live-instance identification were Unknown at that checkpoint. No car payload was extracted.
- **Confirmed (slice 119):** six pinned research repositories cloned; Luna decoded
  all 407 ToC pages with bounded raw-DEFLATE-before-XOR processing. Parent replay
  independently matches decoded hash, 18,562 records, 210 directories, directory
  reachability and 18,352 file extent bounds. Car/SpecDB paths are now observed.
  Packed version matches GT4FS's 0x00030001 constant.
- **Unknown:** production reader/lookup qualification, named-car crosswalk,
  model/texture payloads and live instance/camera effects. No asset payload was
  read and no renderer or performance change was made in slice 119.
- **Confirmed (slice 120):** public bounded volume reader reproduces slice 119
  catalogue hashes/counts. Five selected SpecDB files extracted after whole-ISO
  verification on the same handle; strict PS2ZIP framing, sizes and hashes pass.
- **Confirmed (slice 120):** offline `ka_01` / car code 59 resolves to Ford Ka '01
  in USA GT4_US2560. Name/Grade/ShortName use localized string 84; NarrationID
  is raw integer 435. Worker and independent parent code-list Huffman replay
  match both selected rows. SpecDB decoding remains a private research probe.
- **Confirmed (slice 120):** 22 new public controls pass; existing baseline
  candidate CTest 3/3 and Python 81/81, zero skips. No native code/build change.
- **Unknown:** which menu/LOD file corresponds to this car; exact `ka_01`
  basenames and MODEL_INFO pair are absent. Eleven Ford-prefixed leaves remain
  candidates, not identified assets. No model/texture payload read or live hook added.
  B-tree key routing and comprehensive format/fidelity qualification remain open.
- **Confirmed (slice 121):** CAR_VARIATION row 59 selects VARIATION row 444/table
  42; all 11 physical variations use SDB model index 1598, `fdeu0006`. Independent
  Huffman replay matches all 12 selected row hashes. Exact garage/LOD leaves,
  not guessed Ford prefixes, are identified and extracted with verified pins.
- **Confirmed (slice 121):** both CAR4 main MDLS bases are 0xC00; garage/race
  have 4 models each and 22/113 shapes. Independent header checks match. Signed
  -1 VM words are opaque; five zero-size Tex1 slots are reported header-only.
  Geometry, texture records/pixels, relocation and live argument chain remain Unknown.
- **Confirmed (slice 121):** 17 public header controls; CTest 3/3, Python 98/98
  with zero skips. No native rebuild, live hook or runtime behavior change.
- **Confirmed (slice 122):** selected garage/racing main models contain 154/312
  bounded shape descriptors, 14/18 material records and 49/123 PGLU texture
  records plus 5/15 transfer records. Packet spans and fixed arrays are bounded;
  shape +4 is opaque, transfer storage extent and material semantics Unknown.
- **Confirmed (slice 122):** 11 new public controls; CTest 3/3, Python 109/109,
  zero skips. No VIF/vertex/pixel decoding or runtime change. Independent record
  review is recorded in [Model Records](MODEL-RECORDS.md).
- **Confirmed, conditional (slice 123):** under explicit initial CL/WL=(1,1),
  all 466 selected packet spans frame to their exact QWC end. No STCYCL appears,
  so live incoming cycle remains Unknown. No VIF/VU execution, GIF meaning,
  channels, topology, vertices or renderer substitution is qualified.
- **Confirmed (slice 123):** 11 public framing controls, CTest 3/3, Python
  120/120, zero skips. Fill cycles reduce source consumption; NUM0 expansion
  is explicit. Packed/reserved UNPACK and zero-cycle fields reject as unqualified.
- **Confirmed (slice 124, static only):** pinned ELF corroborates shape argument+4
  loaded to F12 and helper `0x0049E548`'s conditional matrix/VU and 144-byte buffer
  effects. Submission reads descriptor addresses directly; serialized relocation
  and selected live object identity remain unqualified.
- **Confirmed (slice 124, static only):** three local packet builders emit (0,4)
  then (4,4) cycle words. Channel submission, previous/live cycle and selected
  Ford Ka relation remain Unknown. Channel-role formulas remain reference leads.
- **Confirmed:** documentation-only slice, source/ELF pins and bounded independent
  review pass. Existing CTest 3/3 and Python 120/120 are inherited from slice 123;
  no new build/test or runtime change claimed.
- **Confirmed, conditional (slice 125):** 2,320 ordinary UNPACK commands supply
  157,441 raw scalar lanes under explicit cycle/signedness assumptions. Integer
  decoding preserves 32-bit patterns and opaque alignment; no channels, floats,
  geometry, VU results or live behavior inferred.
- **Confirmed (slice 125):** 11 new literal controls; CTest 3/3, Python 131/131,
  zero skips. Exact selected pins and frozen framing reproduced; no native rebuild.
- **Confirmed, conditional (slice 126):** source predicates match a position
  candidate in 466/466 packets but float-normal candidates in only 6/312 racing
  packets; 23 garage UV candidates have three components and would be skipped.
  Independent derived-data audits and complete qualified-header check agree.
- **Unknown:** actual channel/normal/UV/topology semantics. No converter adopted;
  predicate coverage is not game geometry proof. Research-only slice inherits
  CTest 3/3 and Python 131/131 from125; no new runtime/build/test claim.

- **Confirmed (slice 127, source only):** existing donor probes can observe pre-call
  0x00455A5C and registered continuation 0x00455A64 without translator changes.
  Direct dispatch bypass, depth spill/resume, stale pre-PC and reused thread IDs
  require original-input tokens and lifecycle invalidation, not a post-only hook.
- **High confidence proposal:** default-off fixed metadata observer, bounded RAM
  reads and no duplicate game evaluation. New generation/profile and lifecycle
  adapter still required. No live hook, build or gameplay qualification this slice;
  CTest 3/3/Python 131/131 inherited from125. See [Observation](SCENE-OBSERVATION.md).

- **Confirmed (slice128):** default-off fixed observation core and safe direct
  RAM/scratchpad sampler pass actual native synthetic controls. Persistent
  original inputs, exact continuation/SP, owner isolation, incarnation/epoch,
  handler skips and deterministic storage/ambiguity failure are exercised.
- **Confirmed:** fresh baseline-profile Windows/Clang build, CTest4/4 and
  Python132/132 with zero skips. Core is fixture-only, not linked into playable
  runtime. Generated probes, lifecycle adapter and live evidence remain pending.

- **Confirmed (slice129):** separate fresh --probes generation changes only D5:
  exactly two27-byte statements at intended pre/return frontiers. All327 other
  files, including registry/header, match frozen116 byte-for-byte. Independent
  removal of the two lines restores D5 exactly; baseline producer gates unchanged.
- **Confirmed:** changed D5 compiles with qualified Clang/VS ABI. Producer CTest1/1
  and Python141/141, zero skips, reuse128 native fixtures. No full probed library,
  lifecycle/runtime adapter, new game executable or live capture yet.

- **Confirmed (slice130):** optional fixture-only adapter observes actual runtime
  probes, preserves original shape inputs across a synthetic handler dispatched
  exactly once, and invalidates tokens through real kernel lifecycle/load paths.
  Three pinned source overlays invert exactly to donor bytes after removing hooks.
- **Confirmed:** fresh Clang build, CTest5/5 and Python147/147, zero skips.
  Unoverlaid/observed controls emit identical46,036-byte defined-field transcripts.
  Link-map/COFF gates select reviewed direct EE bindings; game link excludes hooks.
  Extra headers and post-configure source edits reject before altered compilation.
- **Unknown:** retail call reachability, full probed-library/game linkage, geometry
  and live scheduling/overhead. Owner session unchanged; no performance probe or
  drawing replacement. See [Observation](SCENE-OBSERVATION.md).

See [EE Producer](EE-PRODUCER.md), [Render Contracts](RENDER-CONTRACTS.md) and
[Callback Coverage](EE-CALLBACK-COVERAGE.md), with [slice 116](inputs/slice116-ee-producer-contracts.json)
and [slice 117 evidence](inputs/slice117-ee-entry-coverage.json), for build/test
results and remaining gates. [EE Candidates](EE-CANDIDATES.md) and
[Car Asset Research](CAR-ASSET-RESEARCH.md) cover slice 118. P0/P1 remain **in
progress**: [Nenkai References](NENKAI-REFERENCES.md) and
[slice 119 metadata](inputs/slice119-nenkai-assets.json) record subsequent catalogue
progress. [Volume Reader](VOLUME-READER.md) and [slice 120 metadata](inputs/slice120-car-identity.json)
record the selected extraction and offline identity; candidate gameplay, remaining entry coverage and one real car's
data/conventions are pending. Production library, default launcher and runtime
have not been replaced. Candidate checks do not qualify game boot or racing.
See [Model Asset Probe](MODEL-ASSET-PROBE.md) and
[slice 121 metadata](inputs/slice121-model-file-map.json) for the now-confirmed
offline file mapping and exact header-only parsing boundary. P1 remains open.
[Model Records](MODEL-RECORDS.md) and
[slice 122 metadata](inputs/slice122-shape-records.json) extend this to fixed
record and declared packet-span inventory; renderable geometry remains pending.
[VIF Framing](VIF-FRAMING.md) and [slice 123 metadata](inputs/slice123-vif-framing.json)
record conditional framing, corrected source assumptions and remaining live-state gates.
[Static VIF State Contract](VIF-STATE-CONTRACT.md) and
[slice 124 metadata](inputs/slice124-vif-state-contract.json) record submission
effects, buffered cycle words and the boundary before raw lane/geometry decoding.
[Raw Channel Lanes](RAW-CHANNEL-LANES.md) and
[slice 125 metadata](inputs/slice125-raw-channel-lanes.json) extend this to
controlled raw integer inputs, with channel/topology/geometry gates still open.
[Channel Selection Audit](CHANNEL-SELECTION-AUDIT.md) and
[slice 126 metadata](inputs/slice126-channel-selection.json) record reference
coverage gaps before any semantic converter or native car renderer is adopted.
The owner's active OpenCode baseline session was preserved during build qualification.
On the owner's subsequent explicit request, it closed normally and the slice 118
baseline candidate launched at 13:16 local time. Its window responded and module
loading progressed; owner gameplay confirmation remains pending. The existing
card, normal session locks and per-session backup were used. This startup
observation does not qualify racing or performance. Recovered remains unlaunched.

## Active Path

`native/` + MIT ps2-web-recomp runtime frozen at
1b4808793a12c45125262fd2d5f50a9ec9f7c020. EE translated AOT to x86-64,
IOP/VU interpreted, and PS2 hardware/services modeled in software. Vulkan compute,
Win32, WinMM, and XInput. Does not use a browser, WebAssembly, or embedded PCSX2.

Earlier pre-donor work was removed from this repository and lives in the private
`gt4-staticrecomp-legacy` repository. Private data, saves, and frozen producers remain local.

## Confirmed Results and Limits

- Boot/menus, car/track selection, and genuine Nürburgring racing gameplay; limited 9,000-frame automated controls.
- In-game 16:9 camera/HUD and default 16:9 window framing; 4:3 optional.
- Owner manually tested 480i/480p/1080i during gameplay, all in 16:9; 480p subjectively
  most stable. Human functional confirmation without independent proof of scan fields.
- 48 kHz stereo PCM delivered to Windows; buffer underrun/continuity issues remain open.
- XInput/analog/rumble implementation covered by test fixtures; physical controller hardware untested.
- Persistent memory card, profile/settings saved by the game, per-session backup, and mutual exclusion lock.

Severe performance and frametime degradation recorded: 19 FPS in-race, 11 FPS in 3D car showroom,
18 FPS / 72.5 ms in 1080i screenshot; overlay metrics are not independently calibrated.
Keyboard arrow keys produced double-movements per keystroke per owner report; other keys
remain suspect. Slice136 removes the confirmed dual digital/analog keyboard mapping;
live menu/steering confirmation remains pending. The prior record-only performance restriction is now
superseded for an owner-delegated OpenCode investigation; Codex prepares context only.
The owner separately authorized this keyboard correction. Gameplay is owner-controlled: no Computer Use,
GUI automation, simulated input or scripted navigation. Manual play window and memory card are untouched
by this cleanup. CPU/GPU fidelity, CFC2(28), temporal deinterlacing, 100% realtime rate,
prolonged stability, completed full races, and broad save scenarios remain open qualification items.

## Build and Verification

The root build now only covers the current Clang/C++20 pipeline, generated by
`native/build.cmd` into `private/slice114-native-base/`. Runtime 113 and nine original SPIR-V
shaders remain preserved. The private AOT library is a prerequisite,
validated by hash; the playable build does not yet consume the separate P0 producer.
Clean build with zero warnings; 3/3 CTest passing in 11.13s, 32 Python tests passing without skips (final rerun in 5.812s).
A 120-frame headless smoke test exits cleanly with memory card duplication,
without navigating or launching a new race. All 9 SPIR-V shaders match slice 113; frozen pins preserved.
The original 54 CTest / 117 Python tests are historical: tests for the removed engine reside
on the archive branch; the active test suite covers the surviving codebase.

## Native Port Direction

Historical scene-renderer direction below is deferred on the slice 141 hybrid
branch. Use [Hybrid Port Plan](HYBRID-PORT-PLAN.md) for the active next task.

The owner requested a comprehensive native-renderer/native-port and multi-model plan:
[Native Port Plan](NATIVE-PORT-PLAN.md), [Agent Workflow](AGENT-WORKFLOW.md).
Initial target selected by owner: Windows, entry-level dedicated GPU, internal
1920x1080 at 60 FPS. Exact hardware/preset and achieved throughput remain Unknown.
Owner clarified that deferred rendering and a graphical remaster are outside the
current roadmap. Native graphics, progressive output, configurable internal
resolution, ultrawide and independent simulation/presentation timing remain goals.

The owner authorized reproducible EE generation and bounded model/camera research
in slice 116; slice 117 qualifies one callback recovery mechanism offline. The
planned following stage is one live showroom car drawn through a game-level
Vulkan renderer. Expand to racing, progressive/internal resolution and temporal
separation; retire all graphics paths, then remaining execution/device dependencies
through separate qualified service/core migrations. AOT alone does not mean zero
emulation. No Windows JIT is active today; the proposed port does not introduce one.

The new plan supersedes the phase order of [ROADMAP.md](ROADMAP.md), whose timing
analysis remains useful. Eliminating IOP/VU models is not a prerequisite for
FPS independence; 144 distinct scenes require qualified visual state/hooks.
Plans include economical worker routing, independent review, serial integration
and owner-operated gameplay gates. No workers, provider setup or recurring jobs
were dispatched outside the read-only planning review.

This revision is documentation only: no renderer/code generation implementation,
profiling, build, game run or new qualification result. Existing OpenCode changes
and the owner's session/card are preserved. Input, audio, save and stability work
continues to require its own authorized package and acceptance controls.

Technical reference: [Architecture](ARCHITECTURE.md), [Build](BUILD.md),
[Slice 114 Report](reverse-engineering/slice114-native-base-cleanup.md).
Prior functional evidence: reports 110–113, corresponding manifests, and
`inputs/slice113-owner-results.json`. Slice 114 does not reclassify these results
as complete fidelity proofs or official benchmarks.

README updated for public presentation: project vision, confirmed features,
experimental boundaries, and future goals. Editorial change only; does not alter
runtime, requirements, priorities, or validation results.

## OpenCode Performance Handoff

The owner requested an investigation prompt, not debugging by Codex.
[Task prompt](OPENCODE-PERFORMANCE-HANDOFF.md) provides verified paths, build and
launcher constraints, hypotheses to test, fresh producer/manifest requirements,
manual feedback iterations and save protection. Initial source baseline is
cbccf80; executing agent must recheck HEAD. No profiling, build or game run occurred
while preparing this handoff. Maintain documentation and new comments in en-US.

**Round 2 (same day):** launcher `49d25309...` and owner installer `private\slice243-installer\GT4Recomp-Setup-1.2.0-test.exe`
(28,771,288 bytes, sha256 `92666e14386d92fa5098f69a0aab4ae452c1e8978e3329abfd606ed606f0e585`). Adds: monitor names
without resolution, Resolution default "desktop" (follows the desktop at each start), every gt4-gs value enabled, the
renderer choice in setup and the GSdx switch in Settings, the GSdx naming, and PR #72 (TASK-089) merged.
