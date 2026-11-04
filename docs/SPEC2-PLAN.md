# Spec II support — implementation plan (TASK-100, release 1.4.0)

Owner request 2026-10-22. Scope: [decision 0072](decisions/0072-spec2-second-target.md) — Gran Turismo 4 Spec II
v1.10 (SCUS-97436, GT4 Online Public Beta lineage) as a **second target** with its own executable, not a content
merge into v2.00. Background measurements: [SPEC2-STUDY](SPEC2-STUDY.md). Branch `task/100-spec2`.

The work is split into subtasks S1..S9. Each one is a commit series with its own checks and an entry in this file's
"Progress" section. Subtasks marked *independent* can be done in any order; the others follow the arrows.

## Inputs

| Input | Where | Status |
|---|---|---|
| Spec II v1.10 ISO | `C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 Spec II v1.10.iso` (owner's, ignored) | available; SHA-256 `6a3484b9672c10a92154ecee6faa5bbcfd4ed8cd538d8283e429c3fe6e1704b2`, 3,683,418,112 bytes |
| GT4 Online Public Beta ISO (unmodified SCUS-97436) | `C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 (USA) (Beta).iso` | available (owner, 2026-10-22); 2,654,109,696 bytes, SHA-256 `1f49b9d8077801188860b62c3efdbc631ba8af7df979373811ce8c60dcc48ebe` |
| Spec II 1.10 community `.xdelta` | `C:\Antigravity\gt4-staticrecomp\Gran Turismo 4 Spec II (1.10 Full).xdelta` | cross-check done: decoding it with the beta ISO produces exactly the Spec II ISO hash above, so the beta is the confirmed base. Not used for anything else |
| v2.00 retail ISO and pins | as today | available |

Why the unmodified Public Beta helps: (1) a byte diff of `SCUS_974.36` against the beta ELF gives the exact static
patch set of Spec II; (2) the beta is a cleaner first target (S7a): it proves the second-target pipeline without the
plugin injector. Both are now unblocked: the beta's game image is rebuilt from its encrypted `CORE.GT4` (S1) and the
static patch set is measured. The beta ISO is also the base the S7a boot will use.

Note: the beta is *not* "Spec II minus the injector" at the file level. Its `SCUS_974.36` is a small loader and the
game lives in `CORE.GT4`; Spec II ships the game itself as one plain ELF. Rebuilding the beta image therefore needs
the loader's container (Salsa20/20 + CRC-32C + deflate), which `scripts/spec2_beta_elf.py` reproduces.

## Subtasks

### S1 — Patch set and plugin injector map (study; first)
Goal: the complete list of every change Spec II makes to game code, static (in the ELF file) and dynamic (written at
boot by the injector), with the plugin's own code and data, so S2 can build a derived ELF.
- Disassemble the `.plugin_note` payload (RWX segment at VA `0x68A480`, 0x2CCC bytes) and the call at `0x1001E8`.
  Map its patch tables: target addresses, replaced words, `jal`/`j` redirections into the payload, prologue rewrites,
  data writes (physics words), and any variant chosen by arguments or settings (each variant listed explicitly).
- With the beta ISO: byte diff `SCUS_974.36` (Spec II) against the beta ELF, section by section.
- Optional cross-check: a guest RAM snapshot taken after boot in PCSX2 (allowed for this task; it is not the GS clean
  room of TASK-027) compared with the ELF image, to catch any boot-time write the static map missed.
- Output: `docs/SPEC2-PATCHES.md` (every patch: address, old word, new word, purpose if known, source: static /
  injector / both, confidence label) and a machine-readable table `docs/inputs/spec2-patches.json`.

### S2 — Derived ELF (after S1)
Goal: a build-time ELF with every patch applied, as the existing `aspect` EE profile does for v2.00
(`scripts/aspect_elf_patch.py` is the precedent).
- `scripts/spec2_elf_patch.py`: reads `SCUS_974.36` from the ISO (plain ELF, no `CORE.GT4`/Salsa step), applies
  `spec2-patches.json`, keeps the plugin segment as translatable code, and writes `private/sliceNNN-spec2-elf/`.
- The injector call at `0x1001E8` is neutralised in the derived ELF only when S1 shows that its whole effect is
  covered by the applied patches; otherwise its runtime writes are reproduced natively. Either way, documented.
- Pins: `docs/inputs/spec2-iso.json` (ISO identity), `docs/inputs/spec2-elf.json` (original and derived ELF hashes).
- Tests: Python fixtures for the patcher (every patch applied once, the inverse restores the input, refuses an
  unknown ISO).

### S3 — Target-aware host hooks (*independent*; can start now, on v2.00 only)
Goal: no v2.00 address is written or read in another target. Today the VBlank hooks write fixed v2.00 addresses:
16:9 hold (TASK-079, `native/race_widescreen.*`), display aspect / race camera / HUD canvas (TASK-080,
`native/display_aspect.*`, `scripts/display_aspect_overlay.inc`, `scripts/aspect_elf_patch.py`), race display 480p
(`native/race_display.*`, `0x009A86C0`), license demo guard (`native/license_guard.*`,
`scripts/prepare_license_guard.py`), and any other fixed guest address in `native/` and the overlay capsules.
- Introduce one per-target table (`native/game_target.hpp`: target id, and for each hook its guest addresses or
  "not available"). v2.00 fills it with today's values; a target without an entry disables that hook and logs once.
- v2.00 must stay byte-identical: headless frame identity (menus, day race, night race) and GS stream hash against
  the current release build (slice268), CTest, Python suite.
- This is the only subtask that touches the shipped v2.00 code path; it lands on its own commit series and is
  reviewed before anything else merges.

### S4 — Second-target build (after S2; S3 for running)
Goal: `SCUS_974.36` (derived) through the donor translator into its own pinned EE library and its own
`gt4vulkan` executable.
- CMake option for the target (for example `-DGT4_TARGET=spec2`), separate build and library dirs, separate
  manifests in `docs/inputs/` (never edit an existing pin). The generated units use one global symbol set
  (`gen::unit_XXXX`, `gen::kUnits`), so the two targets are two executables (decision 0072).
- `scripts/ee_producer.py` input for a plain ELF; entry ledger with the generic sources only (`fallthrough`,
  `dataword`, `luipair`); retail decomp names and the seeded entry list do not apply.
- The plugin segment overlays a zero-filled part of the data image: decide and document the translator policy
  (translate it as code from the derived ELF).

### S5 — VU0 and VU1 (*independent* once the ELF is readable)
- VU1: confirm the 0x3FF0-byte blob at file offset `0x59C110` is byte-identical to v2.00 (decrypted SHA-256 prefix
  `ae6b6cb3b4406ff3`); the content-addressed VU1 AOT table then applies unchanged. Add a fixture.
- VU0: the upload is 0x920 bytes (v2.00: 0xA80), 1,050 of 2,336 bytes differ. Generate a Spec II VU0 AOT table with
  the existing tools (`tools/vu_recomp`, the slice160 VU0 flow), or use the counted interpreter fallback first.

### S6 — IOP modules (*independent*)
- The ISO adds network modules (`INET`, `NETCNF`, `DEV9`, `SMAP`, `RTINETB`, `INETCTL`, `PDINETCNF`, `PPP`, `PPPOE`,
  `LGAUD`, `DNAS310.IMG`, `NET/`). Map them in `docs/IOP-MAP.md` style; decide per module: HLE stub that reports "no
  network device", interpreted as today, or not loaded. Goal: offline play without hangs.

### S7 — First boot (after S2, S3, S4, S5, S6)
- S7a (optional, with the beta ISO): boot the unmodified Public Beta headless to its first menus.
- S7b: boot Spec II headless; hunt every "no translated code" halt (coverage runs, ledger), then menus, then a race.
  Frame dumps reviewed by eye; no window launched by agents.

### S7c — Spec II on GSdx (after S7b; owner 2026-10-23)
Goal: the Spec II target on the **shipping renderer** — the external PCSX2 GS process (`gt4-gs.exe`,
`GT4_RENDERER=pcsx2`), not the built-in donor renderer used for debugging. Spec II ships before CoreGS 2.0 on GSdx,
with upscaling and texture packs.
- Run `gt4vulkan-spec2.exe` headless through the same path as the v2.00 pcsx2 mode (`scripts/frame_cost_headless.py
  --renderer pcsx2`, with `GT4_GS_PROCESS`, `GT4_GS_PROCESS_SHA256` and `GT4_GS_CONFIG`; frame dumps come from the GS
  process, `DumpEvery`).
- Checks: boot, menus and an arcade race; the GT4 crop (2 rows at the bottom, 1 column at the right) compared with
  v2.00; the black start; GS readbacks (flares, a night race if reachable); stream health (no protocol errors or
  back-pressure stalls); 1x and 3x upscale; a GT4 texture pack (`[Folders] Textures`, recursive in gt4-gs 1.2.1).
- Do not change the gt4-gs fork. If a gt4-gs behaviour is keyed to v2.00, report it.

### S8 — Re-target the host hooks for Spec II (after S7c)
- Find the Spec II equivalents of the 16:9 hold, 480p race display, aspect/HUD hooks and the license demo guard, and
  fill the S3 table for the target. Each hook with its fixture and a headless check on GSdx.

### S9 - Product (after S7, before release 1.4.0)
- Launcher: `gt4l-disc` accepts SCUS-97436 by its pinned identity (today `Unsupported::SpecII`), the prepared layout
  without `CORE.GT4`, a second game executable in the installer, a separate save folder, the second disc copy, a game
  chooser when both discs are set up.
- Credit the Spec II authors and ask them for permission before any public release (decision 0072).

#### S9 recorded decisions and open questions (owner, 2026-10-23; recorded, not started)

- **Order:** Spec II ships **before CoreGS 2.0**, on GSdx (the external PCSX2 GS process, `GT4_RENDERER=pcsx2`), with
  upscaling and texture packs. S7c proved that path headless (menus, day and night races, 1x/3x, the owner's pack).
- **One shared settings set** in the launcher for both games; only the **game executable, the disc and the save folder**
  differ per game. No second settings schema, no per-game Graphics page.
- **Texture packs:** the same `Textures` folder and the same settings for both games, no new folder or option
  (owner-confirmed; S7c loaded the owner's `!_HD HUD & UI` pack on Spec II).
- **16:9 and 480p holds** are implemented for Spec II (S8) so both games behave the same; Spec II may already select
  480p by itself, which the windowed hold leaves alone (index 1/2 are never overwritten).
- **GT mode needs game data on the card.** The seed cards used for headless work have none, so GT mode only shows the
  creation flow. S9 must plan how a fresh Spec II card is created in normal play: the first boot offers "create game
  data" (cancel leads to "GT mode is not playable unless game data is created"), then the name entry and the GT mode
  save. The launcher must not require an existing GT4 save; the second game needs its own memory-card folder so a
  v2.00 save and a Spec II save never share the same file name.
- **Release rules** (AGENTS.md, 2026-10-23): every release writes both notes files before the build
  (`docs/release-notes/<version>.md` and `docs/release-notes/launcher/<version>.md`, plus the pt-BR launcher note at
  `docs/release-notes/launcher/pt-BR/<version>.md`), bumps the version in the launcher/installer files, pins the new
  game build in new `docs/inputs` files and is tested by the owner before anything is public.
- **Open:** the launcher's disc identity and prepared layout for a disc without `CORE.GT4`; the second executable's
  name (today `gt4vulkan-spec2.exe`); the save folder layout; the game chooser; the Spec II authors' credit and
  permission; the GS handshake serial decision (S7c: the game still announces `SCUS-97328`, and gt4-gs has no
  `SCUS-97436` GameDB entry, so keeping `SCUS-97328` is what applies the GT4 GS profile).

## Rules for every subtask
- AGENTS.md applies: one variable per build, fresh `private/sliceNNN-*` dirs, new pins only, CTest and Python
  fixtures for new behaviour, Confirmed / High confidence / Hypothesis / Unknown labels, STATUS and the journal kept
  current, TASKS.md updated.
- Never distribute or commit game data (ISOs, ELFs, extracted files, RAM dumps).
- No JIT: injected code is translated ahead of time like the rest of the ELF.
- C: is short of space: big build trees and run outputs go to `O:\gt4recomp-scratch\spec2-*`.

## Progress
(Each subtask appends: date, what was done, checks, commits, open points.)

### S1 — patch set and plugin injector map: DONE (2026-10-22)

Report: [SPEC2-PATCHES](SPEC2-PATCHES.md). Pins: [spec2-patches](inputs/spec2-patches.json),
[spec2-beta](inputs/spec2-beta.json). Tools: `scripts/spec2_patches.py`, `scripts/spec2_beta_elf.py`. Tests:
`tests/python/test_spec2.py` (14 tests).

- The beta is the confirmed base of Spec II 1.10: the community `.xdelta` decodes to the owner's Spec II ISO hash.
- The beta's encrypted `CORE.GT4` is reproduced (Salsa20/20, key `PolyphonyDigital`, CRC-32C trailer, then the retail
  v2.00 container) and rebuilt into `private/slice271-spec2-beta-elf/SCUS_974.36.elf` (`2e5621ed…`, entry 0x100008).
- Static patch set: **one word** (`0x001001E8`, `jal 0x005E3AA8` -> `jal 0x0068A768`) plus the `.plugin` segment at
  `0x0068A480` (0x2CCC bytes, RWX) over 0x2CCC zero bytes of the beta data image. Every other word is identical.
- Injector map: the init at `0x0068A480` executes 356 instructions, calls `0x005E3AA8` (the function it replaced) and
  `0x004E2940`, and makes 35 unconditional word stores (18 text, 17 data). No variants: same table under stack and
  external-return perturbation; no settings or arguments are read.
- Open points: the `0x006605F4` flag word has no reader in a linear text scan (Unknown); the two 128-byte container
  header blobs are opaque (Unknown); purpose labels marked Hypothesis are documentation only.
### S3 — target-aware host hooks: DONE (2026-10-22)

Code: `native/game_target.hpp` (table), `native/game_target_fixture.cpp` and `native/game_target_spec2_fixture.cpp`
(CTest `native_game_target`), `GT4_TARGET=v200|spec2` in `native/CMakeLists.txt`. The four hook modules
(`race_widescreen.*`, `race_display.*`, `display_aspect.*`, `license_guard.*`) and the shape observer read their guest
addresses from the table; a target without an address disables the hook and logs once, and the overlay capsules keep
their text unchanged (`kCameraCallReturn`/`kRaceDisplayRender` become `0xFFFFFFFF` when absent). BUILD.md has the
slice recipe and the full list of what changed.

Checks (candidate `gt4vulkan.exe` 97,240,064 bytes, sha256 `ca1eff8d90b6df03…`; release capsules slice262, EE profile
`aspect`, `GT4_TARGET=v200`, build `O:\gt4recomp-scratch\spec2-s3-build`):

- CTest 42 of 42 in the first build (the final build's 41 non-Python tests also pass; `native_game_target` is new).
- Python suite with `GT4_NATIVE_BUILD` = the candidate: 653 OK, 42 skipped, **1 known failure**:
  `test_make_distribution` compares `gt4vulkan.exe` against the release slice268 pin, which any rebuilt executable
  fails (same size, different hash). A new pin set is a release decision (same situation as slice264).
- Headless frame identity against `private/slice268-build` (the 1.2.3 release build), 1x gt4-gs, `GT4_GS_STREAM_HASH=1`,
  same seed cards and schedules:

  | Scene | Dumps | Candidate | slice268 | Stream hash | Candidate | slice268 |
  |---|---|---|---|---|---|---|
  | menus | 41 | `463d455c3204cb56` | `463d455c3204cb56` | | `1ccb97c58a6d2945` | `1ccb97c58a6d2945` |
  | day | 159 | `992f1ac9a6871901` | `992f1ac9a6871901` | | `0c79dcf5208e5e4d` | `0c79dcf5208e5e4d` |
  | night | 159 | `c57e7d03aaccb3aa` | `c57e7d03aaccb3aa` | | `c091716e3cb40d79` | `c091716e3cb40d79` |

  All six comparisons are equal, on the final code state (the run was repeated after the last two edits).
- Open: the release pin set for the new executable (release decision); S8 fills the Spec II table.

### S4 — second-target build: library and executable done (2026-10-22)

- **Library** [done]: 346 units compiled with clang-cl 23.1.2 into
  `private/slice274-spec2-ee-producer/game-build/ps2game.lib` (77,369,746 bytes, sha256 `5e483768…`), pinned in
  [inputs/spec2-ee-libraries.json](inputs/spec2-ee-libraries.json) as profile `spec2`. 12 plugin entry seeds were added
  after the first boot (see S7b): the plugin's pointer tables live inside its executable segment, which the translator
  does not scan for datawords. Translation pin: [inputs/spec2-ee-generated.json](inputs/spec2-ee-generated.json)
  (346 units, inventory `43b02351…`, 1,285,650 instructions, 17,080 functions, 148,756 entries, 2 rejected seeds).
- **Executable** [done]: `-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2` selects the library and names the output
  `gt4vulkan-spec2.exe`, so v2.00 is untouched. The perf runtime capsule now accepts the `spec2` profile (it is
  target-independent code); the license guard capsule stays out until S8. Built at
  `O:\gt4recomp-scratch\spec2-s4-build\build`.
- **Pending**: the entry ledger / "no translated code" hunt on a real boot (S7b, blocked) and the later S8 hooks.

### S7b — first headless boot (2026-10-22; **resolved 2026-10-23, see the entry above**)

What was fixed to get this far:

1. **The translator read the plugin as zeros.** The Spec II file stores the plugin as its own RWX PT_LOAD at
   `0x0068A480` *inside* the address range of the data PT_LOAD (the beta's data image is zero there). The runtime loads
   segments in order, so the plugin wins; the donor translator instead reads the *first* segment containing an address
   (`recomp/elf.cpp`, `Image::word`), so every plugin entry compiled to an empty stub and the boot halted at
   `0x0068D14C`. Fix: `scripts/spec2_elf_patch.py` now splits the data PT_LOAD around the plugin (data-low, plugin,
   data-high), which keeps the loaded image byte-identical (0 differing words) and makes every lookup unambiguous.
   New derived ELF `36f60b1a…`.
2. **Plugin entry points from its own tables.** With the segment fixed, the boot halted at `0x0068C7C4`, an internal
   plugin function reached through the plugin's data table at `0x68D068`. Those tables are inside the executable
   segment, so the translator's dataword source does not see them. 12 addresses (the five `jal` hook targets, the
   thunk, `0x68C7C4`, `0x68C6E8`) are now explicit entry seeds.

After both fixes the derived image boots headless with **no "no translated code" halt**: 600 and 4,200 frame runs
complete on the frame limit, with the EE kernel initialised, the syscall table installed, `SetGsCrt` done and the IOP
running 26 threads.

**Blocker**: the game draws nothing after that — 8 primitives, one GIF tag per frame, no path-3 traffic (v2.00 at the
same frame: 2,524 primitives, 923 path-2 and 912 path-3 tags). All EE threads end up waiting (the main thread on a
semaphore after the sound-module init chain at `0x5E5ED0`), and the IOP is idle. Two controls:
- the **original, unpatched** `SCUS_974.36` behaves identically (8 primitives, 26 IOP threads), so the stall is not
  caused by our 35 patches, the injector or the derived image;
- the game requests the IOP reboot with `rom0:UDNL cdrom0:\DNAS310.IMG;1` (the beta's protected runtime) instead of
  v2.00's `IOPRP300.IMG`; the runtime only logs that request (it serves the system libraries as HLE, so this is not a
  module-load failure by itself, but the DNAS path is untested).

Next steps for S7b (recorded, not done): trace which semaphore the main thread waits on and who should signal it;
decide whether the DNAS reboot needs a runtime overlay (a capsule, never the frozen donor); as an S6 experiment, boot
a scratch ISO copy without one of the new modules (LGAUD/PPP/PPPOE) to see whether the stall is module-driven.

### S7b — first headless boot: BLOCKER CLEARED, menus and a race run headless (2026-10-23)

The 2026-10-22 blocker (nothing drawn, every EE thread waiting, IOP idle) is fixed. Root cause [Confirmed by the
client code and the traces]: the Spec II disc reboots the IOP with `rom0:UDNL cdrom0:\DNAS310.IMG;1`, and the donor
runtime selected its **asynchronous FILEIO server** (0x80000001, the one that answers `devctl`, method 23, with the
SIF packet `0x80000011`) only when the reboot path contained `IOPRP`. Spec II fell back to the synchronous server:
the `devctl` on `dev9x` returned -1 and sent no completion packet, and the game thread that waits on the per-call
FILEIO semaphore never woke (the dump showed the waiter). The image itself needs no module loading: DNAS310.IMG is
the same romdir as IOPRP300.IMG with the same 16 Sony system modules in the same order, all served as HLE
(`docs/IOP-MAP.md`, pin `inputs/spec2-dnas310.json`).

What landed:

- **Runtime capsule** `private/slice276-spec2-iop-runtime` (`scripts/prepare_perf_runtime.py --mtvu --gs-stream
  --frame-cost --vu0-aot --spec2-iop`, pin `inputs/slice276-spec2-iop-runtime.json`): the slice262 profile plus a
  derived `sif.cpp` (`scripts/spec2_iop_overlay.inc`) that calls `native/spec2_iop.hpp`
  (`async_fileio_boot_image`): IOPRP* or DNAS310.IMG selects the asynchronous FILEIO server, and the line logs the
  LOADFILE version read from the image. v2.00 never asks for DNAS310.IMG; its build path and behaviour are unchanged.
- **CMake**: `GT4_FRAME_CAPSULE_RE` (slice260-279) for the frame-cost profile; a `slice27x` capsule also carries
  `sif.cpp`; a Spec II build compiles a v2.00-pinned copy of the three hook modules for the v2.00 hook fixtures
  (`GT4_TARGET_FIXTURE_V200`, `gt4_quickfix_v200_fixture`), so the v2.00 checks still run in a Spec II build.
- **CTest** `native_spec2_iop` (`native/spec2_iop_fixture.cpp`) pins the reboot-image rule.

Boot evidence (derived ELF `36f60b1a…`, Spec II ISO, build `O:\gt4recomp-scratch\spec2-s7d-build`, donor renderer 1x,
run directories `O:\gt4recomp-scratch\spec2-s7d-runs\*`):

| Run | Frames | Result |
|---|---|---|
| `menus` | 4,200 | 1,805,640 primitives, all 23+ IRX modules loaded, the boot devctl answered by the async path |
| `menus-frames` | 1,500 | language menu at 600, game-data dialog at 1,500 (frames reviewed by eye) |
| `menus-main` | 4,000 | "Welcome to Gran Turismo 4 Spec II", name entry, intro movie, Ford GT showroom |
| `menus-skip` | 9,000 | main menu: "GRAN TURISMO 4 spec-II", Arcade Mode highlighted |
| `arcade-3` | 21,000 | Arcade → Single Race → Costa di Amalfi → Ford RS200 '84 → **race running** (HUD, lap timer, position, minimap; 169,687,609 primitives) |

No "no translated code" in any run; the IOP runs up to 68 threads with the network modules loaded and their device
opens failing cleanly. Checks: CTest 43 of 43 in the Spec II build; Python suite 668 tests with the Spec II build
(0 failures, 51 skipped) and with the v2.00 build (1 expected failure: the release distribution pin, unchanged).
**v2.00 identity re-run against the 1.2.3 build (`private/slice268-build`) with `GT4_GS_STREAM_HASH=1`:** menus
`463d455c…`/`1ccb97c5…`, day `992f1ac9…`/`0c79dcf5…`, night `c57e7d03…`/`c091716e…` — all six equal.

Candidates this round (not release-pinned; the pin set is a release decision): Spec II `gt4vulkan-spec2.exe`
102,215,168 bytes, sha256 `87298c7b9e78…` (`O:\gt4recomp-scratch\spec2-s7d-build`); v2.00 control `gt4vulkan.exe`
97,240,064 bytes, sha256 `44d39102ea46…` (`O:\gt4recomp-scratch\spec2-s7e-v200-build`).

Open: the race ran with a scripted accelerator press (no driving test); the UI/HUD are the 4:3 layout until S8
retargets the hooks; S6's "offline play without hangs" is now met on the boot/race path but not yet swept across
menus (GT mode needs game data, which the seed card does not have).

### S7c — Spec II on GSdx: PASS (2026-10-23)

Runs (donor: none; all with `GT4_RENDERER=pcsx2` through `scripts/frame_cost_headless.py`, Spec II derived ELF
`36f60b1a…` + Spec II ISO, seed card, `--keep-dumps`; run directories `O:\gt4recomp-scratch\spec2-s7c-*`):

| Run | GS process | Frames | Result |
|---|---|---|---|
| `spec2-s7c-menus-1x` | pinned `slice203-gt4gs-release-pkg` (1.0.1, `bda98d49…`) | 4,200 | 41 dumps 640x480, 2,585,149 records; language/menus; 0 readbacks |
| `spec2-s7c-race-1x` | pinned 1.0.1 | 21,000 | 37,495,084 records (46.9 GB), arcade race at Costa di Amalfi; 12 readbacks served (0 zero-filled) |
| `spec2-s7c-night-1x` | pinned 1.0.1 | 22,000 | C.S. Route 5 (night, Clubman): 2,928 readbacks served, mean 4.6 ms, max 483 ms; race HUD at position 6 |
| `spec2-s7c-menus-3x` | pinned 1.0.1 | 4,200 | dumps 1920x1440, same record count, clean |
| `spec2-s7c-race-121` / `spec2-s7c-v200-race-121` | installed gt4-gs 1.2.1 (`887f5bf9…`) | 21,000 / 16,000 | Spec II Costa di Amalfi race vs v2.00 day race on the shipping version: identical frame geometry |
| `spec2-s7c-tex-control` / `-tex-pack2` | installed gt4-gs 1.2.1 (`887f5bf9…`) | 4,200 each | owner's `!_HD HUD & UI` pack via `[Folders] Textures` + `LoadTextureReplacements`: log shows the pack folder, SCEA splash 13,374 changed pixels, language screen ~257,000 |

Checks:

- **Boot, menus, race** [Confirmed]: GS process posts READY (`PCSX2 GS Vulkan x1`, protocol 1.2), exits 0; the boot
  splash ("Sony Computer Entertainment America Presents" on black), language menu, main menu, Arcade Mode, track and
  car selection, and the race all render through gt4-gs. **Black start** is the SCEA splash on black (frame 100).
- **GT4 crop** [Confirmed]: the crop is a **fixed constant in gt4-gs** (`GT4_DISPLAY_CROP = {0,0,1,2}`,
  `pcsx2-gt4gs/Main.cpp`; the `[EmuCore/GS] Crop*` keys are refused), applied to every live stream regardless of the
  game, and both runs log `gt4-gs: display crop left 0 top 0 right 1 bottom 2 (native pixels)`. Measured at 1x:
  1.2.1 crops and scales (content to row 479 of 480, rightmost column duplicating its neighbour in flat areas),
  identically for v2.00 and Spec II (`live_frame18500` vs v2.00 `live_frame12500`); the pinned 1.0.1 leaves 2 black
  rows (its bottom crop was 1 row, changed in gt4-gs 1.2.0). **Reported for S8/S9:** the game announces the v2.00
  serial **SCUS-97328** (`native/gs_stream/gs_stream_client.cpp`, `kGameSerial`) even for Spec II, so gt4-gs selects
  the GT4 GameDB entry; gt4-gs has no SCUS-97436 entry (only a memory-card filter). The crop itself does not depend on
  the serial.
- **GS readbacks** [Confirmed]: 12 in the day race, 2,928 in the night race (tunnel/lights), 0 zero-filled; readback
  waits are reported by the game (`readback 152 ms total` day) and the GS buckets show one 483 ms outlier at night.
  No protocol or stream errors; no back-pressure stalls (space waits 0, frames-in-flight waits only).
- **1x and 3x** [Confirmed]: 640x480 and 1920x1440 dumps, same record count, clean text.
- **Texture pack** [Confirmed with the owner's pack]: needs gt4-gs ≥ 1.2.1 and the launcher's two keys
  (`[Folders] Textures`, `[EmuCore/GS] LoadTextureReplacements`); the repo's pinned package is 1.0.1, so the texture
  check used the installed 1.2.1 and recorded its SHA-256. Without a pack the same 1.2.1 run is deterministic against
  itself; only pack-replaced art changes (the pack's bootup art also shows the "SECRET MENU" cheat sheet).
- Harness: `scripts/frame_cost_headless.py` gained `--exe`, `--elf`, `--iso`, `--press`, `--textures`, `--gs-sha256`
  (defaults unchanged, so the v2.00 release gates keep their behaviour); tests in `test_frame_cost.py`.

Open: the GT4 serial constant decision (above); the pinned 1.0.1 GS package cannot load texture packs, so a release
that ships packs must pin 1.2.1+ (launcher already requires 1.2.1).

### S8 — Re-target the host hooks for Spec II: DONE for the 16:9/480p holds, the camera and the HUD (2026-10-23)

The Spec II image is a different build of the same game: the structures keep their shapes and every address moved.
Each value was located in the derived ELF from the v2.00 code shapes, then confirmed against a **runtime RAM dump**
of the Spec II menus (`O:\gt4recomp-scratch\spec2-s8-spec2-ram\ram.bin`, `PS2_DUMP`) and a v2.00 dump for comparison:

| Spec II | v2.00 | How |
|---|---|---|
| owner word `0x664EDC` | `0x622F4C` | read by the translated `initMonitor` (`lui 0x0066; lw 0x4EDC`) and the RAM dump (owner `0x9BD690`) |
| option `owner + 0x680` | `owner + 0x38CB0` | same instruction |
| wide flag `option + 0x14`, record `option + 0x24` | same offsets | `initMonitor` and the wide setter `0x3124E4` (`lw a1,0x14; addiu a0,+0x24`) |
| record width/height `+0xC/+0x10` (44 bytes) | same | RAM dump: wide 425.0x240.0, 4:3 275.0x205.0 |
| presets `0x664E20/50/80/B0` | `0x622E90/C0/F0/20` | the preset copier `0x315368` selects them by (wide, NTSC); RAM dump shows the four records |
| published wide word `0x6655A0` | `0x6184F0` | `MOption::apply` `0x31257C` stores the test result; `RaceDisplay::render` reads it |
| race display index `option + 0x1718` (0/1/2) | `+0x16B0` | `initMonitor` compares it with 2; setter `0x312630` with 1 |
| `RaceBase::initMonitor` `0x26BDB0`, camera return `0x26BE60` | `0x388A80` / `0x388B38` | found by the 1.8/1.35 fovy constant pair and the virtual call with `a1 = sp` |
| `RaceDisplay::render` `0x1C5040`, HUD matrix `this + 0x70` | `0x39F498` / `0x70` | found by the five repeated setup calls (`100,15,16,13,14`) and the `addiu a0,sN,0x70` + matrix-load call |

Not mapped yet, and disabled in the table (each logs one line and does nothing):

- the **display request struct** (v2.00 `0x6184D0`): the scene policy knows render activity but not photo mode, so a
  photo-mode frame could be filled for one request;
- the **license demo guard**: mapped in S8b below (the table fields are filled and the guard is on);
- the **shape observer** (diagnostic only).

Checks:

- Fixture `native_spec2_hooks` (`native/spec2_hooks_fixture.cpp`, always compiled with `GT4_TARGET_SPEC2`): asserts
  every table value, the preset stride/owner layout, and exercises the Spec II write rules in synthetic 32 MiB RAM:
  the wide hold copies `0x664E50` into `option+0x24`, sets `option+0x14` and `0x6655A0`; the 480p hold writes 1 on the
  normal index and leaves 1080i alone. `native_game_target` now asserts the filled table.
- **Headless on GSdx** (Spec II build `O:\gt4recomp-scratch\spec2-s7d-build`, now with the `slice262` license guard
  capsule that carries the aspect overlay; derived ELF + Spec II ISO, 21,000-frame Costa di Amalfi race):
  - 16:9 hold: `[display] game-data pointer settled on 0x009BD690 at vblank 612` (the same owner as the RAM dump) and
    the wide preset validates; no write because the game itself boots 16:9 (the record is already 425x240 and the
    published word is 1). The write path is covered by the fixture.
  - 480p hold: **not reachable headless** — the hold runs in `window.cpp`'s VBlank (`on_vblank`), which exists only in
    a windowed run; the Spec II rule is covered by `native_spec2_hooks`, and the game's index was 0 at the end of the
    race, so a windowed run would force it.
  - camera aspect at `GT4_DISPLAY_ASPECT=21:9`: `[aspect] race camera hook 1 ... matrix 0x01FFF800 scaled
    (x 0.761905, y 1.000000)` (and hook 2), `[aspect] HUD canvas of RaceDisplay 0x01E4CEA0 ... left -240 right 880`,
    `[aspect] presentation ... fill 21:9` — the camera, HUD and scene-policy hooks all key on the Spec II addresses.
  - Frames reviewed at 21:9 vs 16:9: the HUD is centred in its 16:9 box and the scene FOV widens.
- v2.00 unchanged: `native_game_target` still asserts the release table, the v2.00 hook fixtures still run against it,
  and the menus/day/night identity against slice268 is re-run after the change.

### S8b - License demo guard for Spec II: DONE, owner license run open (2026-10-23)

Why: every `license_*` field in `kSpec2Hooks` was 0, so the TASK-017 guard (which stops the crash when our FPU
decrypts a license demo in the wrong key-stream phase) was off for Spec II. The demo feature is present in the image
(`license/%s`, `replay`, `DemoDirectory`), so it very likely crashes the same way.

Method: the v2.00 code shapes in the derived Spec II ELF, then the demo files themselves (retail, beta and Spec II
discs) as ground truth. No runtime license demo has been observed yet (see below), so the pointer shift is the one
inferred value.

| Spec II | v2.00 | Evidence (label) |
|---|---|---|
| `RaceSolitaire::loadDemoInputs` `0x002012C8` (vtbl `+0x510`) | `0x003EAD18` (vtbl `+0x4D8`) | Confirmed: RaceSolitaire vtable base `0x6B3130`; the same virtual that v2.00 base `0x680140` points at `+0x4D8`; the function calls `AutomobileControlRecord::Manager::load` |
| loader A `0x0010CA18`, return `0x0010CB54` | `0x001100D0` / `0x001101E4` | Confirmed: entry prologue, calls the `license/%s` helper `0x001A8BE8`, one `jalr` through vtable `+0x510`, return label is its delay-slot return |
| loader B `0x0010CBA8`, return `0x0010CCE4` | `0x00110230` / `0x00110344` | Confirmed: same helper, one `jalr` through vtable `+0x1C8` (index 57, unchanged) |
| `Manager::load` `0x00207AA0` | `0x00346BB8` | Confirmed: `lw s3,0(s1)` count, `bne`, `addiu s1,s1,4`, `sw zero,4(s0)`, `addiu a0,s0,8` and the same tail shape |
| Manager stream add `0x1A24` | `0x1AA4` | Confirmed in `0x002012C8` / `0x002012F0` (`addiu a1,a1,0x1A24`; `this+0xF318`, v2.00 `this+0xE560`) |
| demo body pointer shift `0x80` | `0` | **High confidence**: `0x1A24 + 0x80 = 0x1AA4`, and the decrypted bodies on all three discs carry the Manager stream at body `+0x1AA4`; the pointer itself was not observed at runtime |
| game CRC-32 table `0x00731DB0` | `0x00655358` | Confirmed: all 256 words equal the standard reflected table |

Cipher and format check (step 2 of S8b):

- The cipher is the same: `0x00459E90` (v2.00 `0x00579E00`) reached through `0x002E3CE8` (v2.00 `0x0044D278`);
  the FT table at `0x00667048` matches `FT[i] = 0x3F800000 + (2i+1)*0x4010 + (i>>5)` for all 256 entries and the T8
  bytes at `0x00731C98` are the bit reversal, exactly the v2.00 rule.
- The demos are the retail files: the Spec II volume catalogues 26,891 files and 256 `replay/license/*` demos (RoFS
  3.1 TOC at `0x115B800`; leaf pages carry an odd entry count, so the pinned reader's parity check was relaxed in
  scratch only). 171 names are shared with v2.00 and **170 bodies are byte-identical**; the beta disc has the same
  256 plus 381 in `replay/license.old`, also retail format.
- A CRC-confirmed beam search on Spec II demos (`cnl0a0001`, `cnl0b0001`) returns the hardware plaintext
  (`u32 body size`, `"GT4M"`, `"RaceLicense"`) with the single-Recorder Manager stream at body `+0x1AA4` and the
  Recorder size ending exactly at the body end, so the **layout and the CRC rule are unchanged**. The code's `0x1A24`
  add is therefore applied to a pointer the loader hands the virtual `0x80` into the body (`body + 0x80 + 0x1A24 =
  body + 0x1AA4`).

Guard change: `HookAddresses` gained `license_manager_add` and `license_body_shift`. `license_guard.hpp` keeps the
layout constant `kManagerOffset = 0x1AA4` and adds `manager_address(a1) = a1 + license_manager_add`; the overlay
evaluates the CRC and the layout at the header's own body start (`buffer + offset`) and writes `n = 0xFFFFFFFF` at the
Manager address the virtual itself will read. A static assert keeps `license_manager_add + license_body_shift ==
kManagerOffset` on every qualified target. The overlay text changed, so it was re-derived into a **fresh capsule**
`private/slice277-license-guard-runtime` (the slice262 profile: aspect + frame counters), pinned in
[inputs/slice277-license-guard-runtime.json](inputs/slice277-license-guard-runtime.json); `native/CMakeLists.txt`
accepts a slice27x guard capsule and the Spec II build now uses it (v2.00 keeps its pinned slice262 capsule).

How many demos the guard protects (the S8b number): with our FPU (`chop/chop`), **0 of the 256 Spec II demos pass the
game CRC; 114 still decrypt to the single-Recorder layout, so the guard writes for 142 demos.** The other division
models move the layout count between 104 and 126 (guard 130-152):
`chop/nearest` 123 sane/133 guarded, `chop/srt2-cs-t1` 104/152, `chop/srt2-cs-t3` 115/141, `chop/srt2-la-t3` 126/130.

Headless license test: **not reachable** - the license menu is GT mode and the seed cards carry no game data; the
S7b/S7c harness reaches Arcade menus and a race only. The guard's evidence is therefore the code mapping, the demo
files and the synthetic tests below; the owner run on a card with game data remains the gate. Its log lines print
`demo 0x<a1>`, `body 0x<header body start>` and `manager 0x<write>` and would confirm the 0x80 pointer shift directly.

Checks: CTest 44/44 in the Spec II build (`native_license_guard` v2.00 rule, `native_game_target`, the extended
`native_spec2_hooks`: Spec II values, both loader return labels key the guard and other labels do not, the shift/add
maths and the guard rule); Python 675 OK (51 skipped) with the Spec II build, including the overlay harness compiled
twice (v2.00 and `GT4_TARGET_SPEC2` with a shifted pointer). v2.00 identity against slice268 after the header change
(1x gt4-gs, `GT4_GS_STREAM_HASH=1`, same seed cards and schedules; candidate build
`O:\gt4recomp-scratch\spec2-s7e-v200-build`, runs `O:\gt4recomp-scratch\spec2-s8b-identity`):

| Scene | Dumps | Candidate | slice268 | Stream hash | Candidate | slice268 |
|---|---|---|---|---|---|---|
| menus | 41 | `463d455c3204cb56` | `463d455c3204cb56` | | `1ccb97c58a6d2945` | `1ccb97c58a6d2945` |
| day | 159 | `992f1ac9a6871901` | `992f1ac9a6871901` | | `0c79dcf5208e5e4d` | `0c79dcf5208e5e4d` |
| night | 159 | `c57e7d03aaccb3aa` | `c57e7d03aaccb3aa` | | `c091716e3cb40d79` | `c091716e3cb40d79` |

All six comparisons are equal. Candidates this round (not release-pinned): Spec II `gt4vulkan-spec2.exe` 102,229,504
bytes, sha256 `7efa63eceb26.` (`O:\gt4recomp-scratch\spec2-s7d-build`); v2.00 control `gt4vulkan.exe` 97,240,064
bytes, sha256 `3b2f3ac19eb7.` (`O:\gt4recomp-scratch\spec2-s7e-v200-build`; the release-pin distribution test keeps
its expected failure).

### S8c - Spec II owner test 1: the Options-menu halt and the entry-seed sweep (2026-10-23)

The owner's first Spec II test (`O:\gt4recomp-scratch\spec2-owner-play`, GSdx 1.2.1, the owner's card with game data)
halted twice on opening the Options menu: `missing code at 0x003CA690 (ra 0x0050D1B0)` after 2,687 and 1,188 frames
(runs `run-20261023-070428` and `-070522`). A third run closed normally after 4,322 frames. License tests opened and
played normally; no `[license-guard]` line appeared, so no demo played yet.

**Root cause (Confirmed).** The call at `0x0050D1A8` is `jalr $v1` with `v1 = lw 16(s1)`: a callback stored in the
Options-menu descriptor object. The callback pointer is built by the Options page code itself:
`lui s0,0x003D` at `0x003CC8EC` followed by `addiu s0,s0,-22896` at `0x003CC9C0` -> `0x003CA690`, 53 instructions
apart, then stored into the descriptor. The pinned translator (`eerecomp.exe`) discovers entries from `fallthrough`,
`dataword` and `luipair`; its `luipair` window is **3 instructions**, so this address never became an entry. It is not
a dataword anywhere in the image (0 literal pointers to `0x003CA690`), and the direct call graph cannot see it because
the caller goes through the object field. The same class covers every menu descriptor / vtable initialiser that sets
many registers before its `addiu`; `dataword` was already complete (0 missing).

**Fix (root, generic).** New `scripts/spec2_entry_seeds.py` runs the read-only ledger scans on the pinned derived ELF
and writes the translator's `--entries` file, adding the classes the built-in discovery misses:

- `luipair` with a **64-instruction window** and register-clobber detection (the Options callback is one of them);
- `fallthrough` stack-frame openers after an unconditional jump (1,233 entries the translator had not reached);
- literal text pointers inside **executable** segments (the plugin's own tables; only stack-frame or direct-jump
  targets count, so plugin strings such as `unkB`/`over`/`argv` are not seeded).

The seed file is the delta against the previous generated units: **1,299 seeds** (98 luipair + 1,233 fallthrough;
0 plugin new), sha256 `041c1864...`, kept in `private/slice278-spec2-ee-producer/entry-seeds.txt`. Seeding only the
delta is deterministic because the translator's own discovery is deterministic for the same ELF and translator.

**Regeneration.** Fresh output `O:\gt4recomp-scratch\spec2-s8c-generated-clean` (346 units, inventory `2d8c92a4...`,
93,030,717 bytes): **1,332,727 instructions** (was 1,285,650), **18,597 functions** (was 17,080), **153,474 entries**
(was 148,756), 33 rejected seeds (data-shaped seeds that the generator now filters). New library
`private/slice278-spec2-ee-producer/game-build/ps2game.lib`, 80,229,124 bytes, sha256 `9a94c3ed...`; pins updated in
[inputs/spec2-ee-libraries.json](inputs/spec2-ee-libraries.json) and
[inputs/spec2-ee-generated.json](inputs/spec2-ee-generated.json). `native/CMakeLists.txt` now accepts only the slice278
library for the spec2 profile; the old slice274 library stays on disk as history.

**Sweep (after reseed).** 154,193 translated entries; the wide-luipair candidates are 4,726 with **0 real missing**
(the 29 remaining are plugin strings/zeros: ASCII data, not code), fallthrough 10,745 with 0 missing, dataword 12,319
with 0 missing, executable literals complete. For comparison, v2.00's 326 units translate 139,534 entries over 3,894
wide-luipair / 9,435 fallthrough / 10,672 dataword candidates (and its own narrow seed list leaves 7+0+3 data-shaped
candidates). Spec II has more entries because of the plugin and the added menus.

**Headless before/after (same schedule, seed card so it is data-independent).** Boot to the main menu, three Downs
(Arcade -> GT Mode -> Replay Theater -> Options) and Cross:

| Build | Result |
|---|---|
| old `gt4vulkan-spec2.exe` (owner copy, `7efa63ec...`) | halt `missing code at 0x003CA690 (ra 0x0050D1B0)` after 11,159 frames (reproduction) |
| new `gt4vulkan-spec2.exe` (`301bbb4a...`) | 16,000 frames, no halt; Options screen: General (Race, Replay, Screen, Picture Quality, Sound), Arcade Mode (Arcade, 2-Player Battle, LAN Settings, Course Registration), Photo Mode (Photo Lab) |

Menu sweep on the new build (headless, no halt in any run; every screen checked on a frame dump): Options -> Race
Settings, Replay Settings, Screen Settings (16:9), Picture Quality Settings, Sound Settings, Arcade Mode Settings,
**LAN Connection Settings** (the network menu), **Photo Lab Settings**; main menu -> **Replay Theater** ("No data
found."), Save Game (create-data dialog), **Credits** (01/16), GT Mode (create-data dialog without data), and the
Arcade Mode selection (Single Race / Time Trial / 2-Player Battle).

**License demo (S8b follow-up):** not reached headless in this round. With a **copy** of the owner's card
(`O:\gt4recomp-scratch\spec2-s8c-owner-card.ps2`, 8,650,752 bytes, sha256 `41bd9d57...`; the owner's card was never
touched) GT Mode loads (10,000 credits, 4 APR) and opens the mod's "Spec II Settings" dialog over the GT map, but the
current press schedule did not dismiss it and reach License Center/test/demo. The owner run remains the gate for the
`[license-guard]` confirmation; the harness now has a `--card` option for a card copy.

**Checks:** CTest 44/44 in the Spec II build (the Python suite is inside it), Python 678 OK (51 skipped); new tests
`EntrySeedTests` (seed format, the Options target in the widened luipair source, plugin strings rejected) and
`CardOverrideTests` (`--card`); v2.00 identity against slice268 unchanged (table below). New executable for the owner:
`O:\gt4recomp-scratch\spec2-s7d-build\build\gt4vulkan-spec2.exe`, 104,475,136 bytes, sha256 `301bbb4a...`.

| Scene | Dumps | Candidate | slice268 | Stream hash | Candidate | slice268 |
|---|---|---|---|---|---|---|
| menus | 41 | `463d455c3204cb56` | `463d455c3204cb56` | | `1ccb97c58a6d2945` | `1ccb97c58a6d2945` |
| day | 159 | `992f1ac9a6871901` | `992f1ac9a6871901` | | `0c79dcf5208e5e4d` | `0c79dcf5208e5e4d` |
| night | 159 | `c57e7d03aaccb3aa` | `c57e7d03aaccb3aa` | | `c091716e3cb40d79` | `c091716e3cb40d79` |

All six comparisons are equal (v2.00 build unchanged since S8b).

### S5 - VU0 and VU1: mapping done (2026-10-22)

Tool: `scripts/spec2_vu.py`. Pin: [inputs/spec2-vu.json](inputs/spec2-vu.json). Tests: `VuTests` in
`tests/python/test_spec2.py`.

- **VU1 carries over, confirmed by content**: the 0x3FF0-byte blob at file `0x59C110` decrypts (v2.00 keystream) to
  SHA-256 `ae6b6cb3b4406ff3…`, byte-identical to the v2.00 image. The content-addressed VU1 AOT table applies
  unchanged; nothing to regenerate.
- **VU0 does not**: the upload is 0x920 bytes at VA `0x731180` (v2.00: 0xA80 at `0x6D3280`), 1,050 differing bytes in
  the 2,336-byte overlap, and the v2.00 upload is 352 bytes longer. Decision: the v2.00 generated programs cannot match
  (key plus identity digest, `scripts/vu0_aot_overlay.inc`), so the exact interpreter runs as the counted fallback for
  the first boot; generate a Spec II table after S7b only if the profile shows VU0 cost.
- Open: a Spec II VU0 table still needs its VCALLMS entry list from a running boot (S7b).

### S6 — IOP modules: map done, offline acceptance open (2026-10-22)

Pin: [inputs/spec2-iop.json](inputs/spec2-iop.json) (35 files with sizes and hashes). Documentation: the new "Spec II"
section of [IOP-MAP](IOP-MAP.md). Tests: `IopTests`.

- 6 new module files (`LGAUD`, `PPP`, `PPPOE`, `DNAS_MC`, `DNAS_NET`, `NTG_ISCK`), inherited from the Public Beta
  lineage; 28 module names appear in the Spec II text against v2.00's 23 loaded modules.
- Policy: everything the offline path needs stays interpreted; the network group is interpreted with no device model
  (device open must fail cleanly) and DNAS stays unloaded. Fallback order if a hang appears: per-module instruction
  counter, HLE stub reporting "no network device", refusal to load.
- Open: "offline play without hangs" needs S7b. **[2026-10-23]** The boot now loads all 28 requested modules
  (including PPP/PPPOE/LGAUD-path ones) and reaches a race without a hang; GT mode still needs game data on the card,
  so its menu sweep is open.

### S2 — derived ELF: DONE (2026-10-22)

- `scripts/spec2_elf_patch.py`: reads `SCUS_974.36` from the ISO (identity pinned in `inputs/spec2-iso.json`), applies
  every `source: injector` entry of `inputs/spec2-patches.json` after checking the old word, and writes the result to a
  fresh directory. `--revert` removes them again (control run: it reproduces the original hash exactly).
- Derived image: `private/slice272-spec2-elf/SCUS_974.36`, 6,520,944 bytes, sha256
  `36f60b1a989b343e98ca55b1f07f051aa83a3fa1a4fcb4acb409adb148afc640`; original `db08421b…`; pin
  [inputs/spec2-elf.json](inputs/spec2-elf.json). The data PT_LOAD is split around the plugin segment (S7b finding);
  the loaded image is byte-identical to the un-split form (0 differing words, checked).
- **The trampoline at 0x001001E8 is kept**, documented: the plugin's init calls the function it replaced
  (`0x005E3AA8`, the beta's own boot initialiser) and one further game function (`0x004E2940`) whose side effects are
  not in the store table. Reproducing only the stores and dropping the call would change behaviour; keeping the call
  makes the runtime stores redundant (they write the same words again) and harmless, because translated code never
  re-reads them. The plugin segment stays translatable code, so its five `jal` hooks and its handlers keep working.
- Tests: `DerivedElfTests` (apply/revert round trip, refusal on a mismatched old word, pin consistency, every patch
  address inside the text/data image).

### S4 — second-target build (done, see below)
### S7 — first boot (blocked, see S7b below)
