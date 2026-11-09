# TASK-019 — Launcher, installer and branding: plan

Status: planning study, 2026-10-15; implementation status in section 14 (branch `task/019-launcher`). Inputs read: CLAUDE.md,
TASKS.md, README.md, docs/BUILD.md, docs/ARCHITECTURE.md, docs/EE-PRODUCER.md,
docs/VOLUME-READER.md, native/THIRD_PARTY.md (main and the TASK-012 worktree),
docs/PARALLEL-GS-PLAN.md (TASK-012 worktree), scripts/play_performance.py,
scripts/play_external_runtime.py, scripts/external_runtime_probe.py, scripts/gt4_volume.py,
Jogar-GT4.cmd, native/window.cpp, native/CMakeLists.txt, donor `runtime/src/iop/{sio2,io,cdvd}.cpp`,
build logs (`.ninja_log`) of the production EE library and of slice 166.

Labels: **Confirmed** (read in code/data or measured), **High confidence**, **Hypothesis**,
**Unknown**. Nothing here is legal advice.

## 0. Owner decisions applied (2026-10-15)

1. **Distribution = option A.** The NSIS installer ships prebuilt executables that contain the
   statically translated game code. The owner accepts the risk. Option B (build the
   game-derived code on the user's PC from their ISO, with a bundled toolchain and an unbranded
   launcher) is kept as the documented fallback if Sony objects. The launcher is designed around
   a `GamePayload` / prepare-step abstraction (section 7.3) so switching to B is a contained change.
2. **Branding = official.** The launcher and the executable icons use the official Gran Turismo 4
   cover art and the game's logo/icon style. Every official asset lives in one folder
   (`launcher/assets/brand/`) and is listed in one manifest (`brand.toml`); a de-branded build swaps the
   folder for `launcher/assets/brand-neutral/` (section 8).
3. **Owner design feedback (round 2).** Clean and minimalist, borrowing the Gran Turismo identity
   (GT4 plus the GT Sport/GT7 look) without copying GT4 screens; far less information everywhere;
   Home = art, logo, Play and a way to Settings only; setup step renamed "Preparing game files";
   memory card created automatically (no choice in setup); Graphics = renderer, resolution (race
   display mode), super-sampling, high-resolution output, deinterlacing and the paraLLEl-GS options;
   Advanced = Asynchronous GS (on), renderer hacks, diagnostics; no frame-pacing option; no variable
   names in the UI. Applied in sections 3, 6 and 12 and in the three mockups.
4. **Owner feedback (round 3).** "Resolution" renamed "Input resolution"; no helper text under any
   option row, each option title gets an "i" with a hover card instead; High-resolution output is
   visible but locked below 4× super-sampling; public official GT4 images and logos are bundled in
   the launcher with a trademark/IP notice (section 8); a Saves page with a save manager
   (section 13, separate task TASK-025).
6. **Owner additions (rounds 4–6):** generic slot names with date only; a Replays page; the PCSX2-style
   folder memory card. After studying the runtime (raw SIO2-level card emulation), the chosen model is
   a folder card in user data that the launcher turns into a card image around Play (section 13.2).
5. **Owner decisions (2026-10-15):** MIT license for our own code (LICENSE added on main by the
   coordinator; third-party parts keep their licenses, paraLLEl-GS LGPL in its DLL); installers are
   GitHub Releases of `moesuito/gt4-staticrecomp` and the launcher source lives in the same
   repository; the Windows display name is "Gran Turismo 4".
7. **Owner feedback on the installed 1.0.0 launcher (2026-10-21, TASK-081).** The game is fixed at 480p and 16:9
   (until the ultrawide update): the "Input resolution" (section 6, Image) and "Widescreen 16:9" rows and the GS
   "Aspect ratio" row are removed; the launcher always sends `GT4_FORCE_480P=1` and `GT4_FORCE_WIDESCREEN=1` and
   writes `AspectRatio = 16:9` (no `GT4_RACE_DISPLAY`). Advanced is a section at the bottom of the Graphics page, not
   a tab. PCSX2's own OSD rows (Show FPS, GPU name, resolution) are hidden and always written off; only GT4Recomp's
   performance overlay rows remain. Old stored values are ignored at load (docs/launcher/MIGRATIONS.md).
8. **Owner design, seamless display (2026-10-21, TASK-080 addendum, decision 0071).** No aspect and no internal
   resolution choice: Display has **Monitor** (the connected monitors, default the primary one) and **Resolution** (that
   monitor's modes, default its desktop mode) next to Fullscreen (always borderless on the chosen monitor) and VSync;
   Graphics has **Render scale** (50% to 200%, the resulting internal resolution shown beside it). The launcher sends
   `GT4_MONITOR`, `GT4_OUTPUT_SIZE` and `GT4_DISPLAY_ASPECT=auto`, and writes `upscale_multiplier` from
   `gt4l_core::display::display_plan`. Settings schema 5 (docs/launcher/MIGRATIONS.md).

Consequences to record when TASK-019 starts (documentation changes, not done here):
`TASKS.md` "First stable release" currently says *"no game assets or binaries derived from the
game"*; it must be rewritten to match decision 1, and a numbered decision (`docs/decisions/0067-…`)
should record decisions 1 and 2 with the risk acceptance. Release binaries still never enter git
(CLAUDE.md data rules stay valid): they are built from `private/` and uploaded as release assets.

## 1. Recommendation in one paragraph

A **Rust + Slint** launcher (`GT4Recomp.exe`) installed per-user by an **NSIS** installer into
`%LOCALAPPDATA%\Programs\GT4Recomp` (no admin). First run: the user picks their ISO; the launcher
pre-checks it in under a second (size + `SYSTEM.CNF`), hashes it against the existing pin with a
progress bar, extracts `CORE.GT4`, rebuilds the pinned ELF (both hash-checked), optionally extracts
the 64 in-game photo JPEGs for the home background, creates the memory-card location and writes a
receipt. Later runs open straight to the home screen: full-bleed art, the logo, a **PLAY** button
and a Settings icon, nothing else. Settings has three short pages (Graphics, Display, Advanced) in a
GT7-style option list. The race display mode (Native 480i / 480p / 1080i) is a setting with 480p as
the default, held by the game itself. The launcher starts the game executable with validated
settings passed as environment variables on the child process only, supervises it, backs up the card,
and turns a crash into a readable report. paraLLEl-GS moves into its own DLL to satisfy LGPL-3.0
relinking cleanly.

## 2. How the game is launched today (Confirmed, from the scripts)

`Jogar-GT4.cmd` → `play_external_runtime.py` (or `play_performance.py --mode main`) →
`external_runtime_probe.run_probe`, which:

- verifies the runtime exe and dependencies against `docs/inputs/<slice>.json`;
- verifies `private/fingerprint-check/CORE.GT4` (pin `85d26aa8…`, 2,020,861 bytes),
  `private/reconstructed/SCUS_973.28.elf` (pin `10f82e22…`, 6,123,004 bytes, from
  `usa-v2.00-native.json`) and **the whole ISO** (`67b6c007…`, 5,314,478,080 bytes) on every launch;
- copies the ELF into a fresh session directory as `SCUS_973.28` (argv0 must equal the disc boot
  name; donor `system.cpp:353` builds `cdrom0:\SCUS_973.28;1` from the file name);
- copies a pinned seed card on first play, otherwise uses `private/manual-play/card.ps2`, and keeps
  `card-before.ps2`; a byte lock (`session.lock`) prevents two sessions;
- scrubs inherited `PS2_*`, `GT4_PERF_*`, `GT4_VU1_*`, `GT4_VU0_*`, `GT4_ASPECT`…, forwards and
  records `GT4_PACING`, `GT4_PRESENT_MODE`, `GT4_GS_SYNC`, `GT4_FORCE_480P`;
- runs `gt4vulkan.exe <session>\SCUS_973.28 --ntsc --disc <iso> --frames 0 --memcard <card>
  --misses <session>\misses.txt --window`, cwd = session dir, stdout+stderr → `probe.log`;
- decides success by the last `[ee] stopped after N frames, C cycles: <reason>` line
  (`window closed` = normal exit) and writes `result.json`.

The launcher reproduces exactly this contract natively, minus the developer-only modes.

### Release blockers found in the native code (Confirmed)

| Finding | Where | Needed change |
|---|---|---|
| Shader directory is an absolute path baked at compile time | `native/CMakeLists.txt:336` `GT4_VULKAN_SHADER_DIR="${SHADERS}"`, used by `gpu_init.inc:7` | Resolve shaders relative to the exe (`<exe dir>\shaders`) in release builds; keep the baked path for dev builds. (Slice 165 already broke on this.) |
| No window/exe icon, no version resource | `window.cpp:241-247` (no `hIcon`), no `.rc` in CMake | `.rc` with icon + VERSIONINFO; `WNDCLASSEXW.hIcon/hIconSm` from resource 1 |
| Start-in-fullscreen throws | `window.cpp:239` `open_window(true)` → "not qualified" | `GT4_FULLSCREEN=1` that applies the existing Alt+Enter borderless path after creation |
| 480p is opt-in and binary | `GT4_FORCE_480P` unset = off (`window.cpp:291`); the hold writes index 1 only when the index reads 0 | Generalize to `GT4_RACE_DISPLAY=480i\|480p\|1080i` (default `480p` when unset): at each guest VBlank hold the race display index (`[0x622F4C]+0x38CB0+0x16B0`) at 0, 1 or 2. Menus stay as the game draws them. Keep `GT4_FORCE_480P` as an alias for one release. Index 2 = 1080i is **High confidence** (BUILD.md: "a user choosing 1080i keeps it"); confirm in game. |
| CRT is `/MD` | worktree `native/THIRD_PARTY.md` notes | Ship `vcruntime140*.dll`/`msvcp140.dll` app-local (Microsoft "Distributable Code") or build `/MT`; decide once |
| Crash evidence is only the log | — | Unhandled-exception filter writing a minidump (`MiniDumpWriteDump`) into the session folder |

## 3. First-run flow

### 3.1 What is needed from the ISO, and when (Confirmed unless marked)

| Item | Setup (once) | Run time |
|---|---|---|
| Disc identity (size + SHA-256 = `usa-v2.00.json`) | Full hash once, with progress | Cheap recheck: path, size, last-write time, NTFS file ID; full rehash only if any changed |
| `CORE.GT4` → `%LOCALAPPDATA%\GT4Recomp\prepared\CORE.GT4` | Extract (ISO9660), verify pin | Not read by the exe (only the ELF is) |
| Reconstructed ELF → `prepared\SCUS_973.28` | Raw-deflate CORE, rebuild layout per `usa-v2.00-native.json`, verify pin | Passed as argv[1] |
| `GT4.VOL`, `IRX/`, `IOPRP300.IMG`, everything else | — | Read **from the ISO** by the exe via `--disc` (streamed; no extraction) |
| VU1/VU0 microcode images | **Not needed under A** (the game uploads microcode from the ELF; compiled programs are matched by identity digest) | — |
| In-game photos `GT4.VOL: photo/000.JPG … 063.JPG` (64 JPEGs, 1280x960, ~10.9 MB) | Extracted to `art\` as part of "Preparing game files" (no separate step; Confirmed: plain JFIF inside PS2ZIP entries) | Read by the launcher only (Home background) |

**PS2 BIOS: not required — Confirmed.** The donor runtime has no BIOS loader: `rom0:` modules are
HLE (`iop/cdvd.cpp:169`), `ROMVER` is synthesized (`iop/io.cpp:57`), the launcher passes no BIOS
argument and no BIOS file is pinned. (Not shown in the UI; it belongs in the README/FAQ.)

### 3.2 Identity check and friendly rejection

1. **Pre-check (< 1 s).** Reject at once if the extension/format is not a 2048-byte-sector ISO
   (`.bin/.cue`, `.chd`, `.cso/.zso`, `.gz`, `.7z`: "Please convert to a plain .iso"). Read the
   ISO9660 primary volume descriptor and `SYSTEM.CNF` (`BOOT2`, `VER`, `VMODE`).
   - `BOOT2` not `cdrom0:\SCUS_973.28;1` → "This is not Gran Turismo 4 USA (SCUS-97328)". If the
     serial is a European one (`SCES…`): "PAL versions are not supported"; if the volume/serial
     identify *Spec II*: "Gran Turismo 4 Spec II is not supported (yet)"; other serials: show what
     was found.
   - `VER` ≠ `2.00` → "This is GT4 USA version X; only the v2.00 disc is supported."
   - Size ≠ 5,314,478,080 → "This copy is incomplete or modified."
   (Owner's `Gran Turismo 4 Spec II v1.10.iso` is a ready negative fixture.)
2. **Full hash** (SHA-256 with SHA-NI/AVX2 via the `sha2` crate's asm backend). Speed is disk-bound:
   NVMe a few seconds, SATA SSD ~10-20 s, HDD ~45-90 s (**Hypothesis**, measure). Shown as a thin
   progress bar with a percentage ("Checking disc"). Mismatch with correct serial/version → "This dump differs from
   the known good v2.00 disc (damaged or patched). Please re-dump your disc."
3. **Preparing game files (shown as "about a minute"):** extract `CORE.GT4`, rebuild the game's ELF
   from it (a fixed-layout reconstruction: nothing is compiled and the executable is not touched),
   extract the disc photos, verify the CORE and ELF pins. Any mismatch here is an internal error (the
   disc hash already matched) and is reported as a bug.
4. **Memory card**, created automatically (section 3.3). 5. **Receipt**
   `prepared\prepare.json` (launcher version, payload kind, disc path/size/file-ID/mtime/hash, CORE and
   ELF hashes, timestamps).

Never copy the ISO by default; offer "Move/copy my ISO into GT4Recomp's folder" only as an option.
If the ISO later moves, the home screen shows "Disc image not found — Locate…" instead of PLAY.

### 3.3 Memory card (Confirmed from donor `sio2.cpp:173-179, 322-333`)

A missing card file is presented to the game as a **new, unformatted** 8,650,752-byte card and is
written on save; GT4 then offers to format it. So the launcher does not need any seed card (the pinned
seeds stay private evidence). Setup creates nothing and asks nothing. The first Play gets a freshly built blank card image. Once
the game creates its save, the save is harvested into the folder card as "Save 01" (section 13.2). Storage, backups and the Saves page are the
save manager's (section 13). **Import** of an existing PCSX2 8 MB `.ps2`
card (same size and raw page+ECC layout, **High confidence**; validate size and the
`Sony PS2 Memory Card Format` superblock magic) is part of the save manager (section 13), never part of setup. Hold the card lock (same byte-lock design as `session.lock`) while the game runs.

**Hypothesis to check during TASK-019:** the owner's game boots in 16:9 because the seed card has the
game's wide option set; a fresh card may show a 4:3 image in the fixed 16:9 window until the player
enables 16:9 in GT4's options. Either show a one-time tip or add a hold similar to the 480p hold.

### 3.4 Progress UI, errors, repair

- One list of four rows: **Disc** (name, region, version), **Checking disc** (progress bar),
  **Preparing game files** ("about a minute"), **Memory card**. Each row shows only a tick, a
  percentage or "Not supported". Cancel is always available and leaves no partial output (write to
  `prepared.tmp\`, atomic rename at the end).
- Errors are one or two plain sentences ("what happened / what to do"); technical detail goes to the
  log only.
- **Repair** (Settings › About): re-verify installed binaries against the release manifest
  (hash list shipped with the installer, < 1 s for ~110 MB); re-run setup from the ISO; never touches
  memory cards. Under A a damaged binary means "reinstall" (link opens the release page).
- **Re-run setup** = "Change disc image…" (same flow, keeps saves).
- Pre-flight before PLAY: Vulkan 1.2 device with `VK_EXT_swapchain_maintenance1` (BUILD.md
  requirement) checked through `ash`; failure message names the GPU and suggests a driver update.

## 4. Distribution model

### 4.1 What the shipped exe contains (Confirmed)

`gt4vulkan.exe` (slice 166: 95,529,472 bytes) statically links `ps2game.lib` (326 EE units, 86 MB of
generated C++ derived from the game's MIPS code) and the generated VU1 (2,048 entries) and VU0 (14
entries) programs, derived from the game's encrypted microcode. Under A this binary is distributed.

### 4.2 Comparison

| | **A — prebuilt (chosen)** | **B — build on the user's PC (fallback)** | **C — hybrid: encrypted payload** |
|---|---|---|---|
| What ships | Launcher + game exe (with translated code) + pgs DLL | Launcher + runtime host exe (no game code) + translator + VU recompiler + clang-cl/lld-link | As A, but the game-code DLL is encrypted with a key derived from the user's `CORE.GT4`/ELF hash |
| Installer size | ~35-45 MB compressed (**Hypothesis**; exe ~96 MB, compresses well) | ~80-100 MB compressed (toolchain ~190 MB unpacked: `clang-cl.exe` 104.7 MB + `lld-link.exe` 71.4 MB + a header subset; Confirmed file sizes of LLVM on this machine) | As A |
| First-run time | ISO hash + extract: ~0.5-2 min | Above + translate + compile: game-derived compile is ~2,000 CPU-s (EE units 811 CPU-s, Confirmed from `reference-acceleration-study/runtime-build/.ninja_log`; VU1 parts 1,172 CPU-s, Confirmed from slice 166 `.ninja_log`) → ~4-6 min on 16 threads, ~10-20 min on 4 threads (**Hypothesis**) | As A + decrypt (seconds) |
| Machine needs | None beyond the game | 8 GB RAM recommended (jobs capped by free RAM), ~3 GB temporary disk, all cores busy for minutes | As A |
| Failure modes | Binary rejected by AV/SmartScreen; GPU driver | AV quarantining freshly linked binaries (high risk for unsigned new files), out-of-disk/RAM, laptop thermal throttling, non-ASCII user paths, toolchain/CRT mismatch; result is still verifiable (generated-source inventory hash `7a1a0909…` and pinned toolchain → reproducible objects) | As A |
| Legal posture (not legal advice) | Distributes code derived from the game: copyright-takedown exposure; using "Gran Turismo" marks adds trademark exposure | Distributes only original/MIT/LGPL code; game-derived code exists only on the user's PC — the conservative model | Still distributes a derivative work, just encrypted; obfuscation is not a legal shield (**Hypothesis**) but makes "needs your own disc" technically true |
| Engineering cost | Lowest; current qualified build | High: game code must become a **freestanding DLL** (no CRT/STL; imports runtime symbols from the host exe) because MSVC/UCRT `.lib` files are not redistributable; `ee_producer`/VU steps ported or Python embedded; performance of the DLL boundary must be measured | Medium: same DLL split as B, no compile on the user side |

**Comparable projects** (High confidence unless marked; not legal advice): *Zelda64Recomp*
(N64Recomp) and *Sonic Unleashed Recompiled* (XenonRecomp) publish prebuilt executables that contain
the recompiled game code and ask the user for their own ROM/game files at first run for assets —
the same UX and the same model as A. No takedown of either is known to me as of my knowledge cutoff
(**Hypothesis** that this still holds). The *Super Mario 64 PC port* era saw DMCA takedowns of GitHub
repositories distributing builds (High confidence). Sony's posture toward recompilation projects is
**Unknown**.

**Risk reducers for A:** the owner decided to publish installers as GitHub Releases of the source
repository itself (accepted risk: a DMCA notice against the binaries could affect the whole
repository; keep a private mirror of the repo and its history as a precaution); never ship ISO-derived data files (ELF, CORE, photos) — those are
always produced on the user's PC; keep B's DLL split prototyped (milestone M10) so the fallback is real.

**GPL interaction (important):** under A the distributed exe combines our code with code we cannot
license (the game's). LGPL-3.0 (paraLLEl-GS) permits this with relinking (section 4.3). **GPL-3.0
code inside the game exe does not** — e.g. importing PCSX2 GS code (decision 0066) or the ARMSX2
pipeline planned for Android (TASK-021) into a binary that also contains translated game code would
make that binary undistributable under GPL terms (**High confidence**). Under A, GPL components must
stay out of the game binary (or Phase 2 uses B-like delivery). This needs an owner decision before
TASK-021.

### 4.3 Recommendation and its consequences

A as decided, with three structural choices that keep B one step away:

1. Launcher logic is written against a `GamePayload` trait (section 7.3). A = `BundledPayload`
   (verifies hashes of shipped files); B = `BuildFromIsoPayload` (runs translator + compiler). The UI
   only sees "prepare steps" with progress, so B adds steps, not screens.
2. paraLLEl-GS in a DLL now (needed for LGPL anyway).
3. Fallback branding: B ships with `launcher/assets/brand-neutral/` and name "GT4Recomp" only.

Installer: ~40 MB, first run ~1 min. If B is ever needed, installer grows to ~100 MB and first run
to 5-20 min with a "Building the game for your PC…" step.

## 5. Licenses in a binary release

| Component | License | Obligation in a binary release |
|---|---|---|
| paraLLEl-GS `cc6184af…` | LGPL-3.0-or-later | LGPL §4: prominent notice that the library is used and is LGPL; ship GPL-3.0 and LGPL-3.0 texts; show its copyright notice where we show notices; allow relinking: **ship it as a shared library (`gt4_parallel_gs.dll`) — LGPL §4(d)(1)** — the user can drop in a rebuilt DLL with the same C ABI. Provide the exact corresponding source (pinned commit tarball + our patch files + build script) next to the release (GPL §6 via LGPL). Static linking would instead require shipping all other object files (including game-derived ones) for relinking — avoid. |
| Granite, volk (MIT) | MIT | Keep copyright + license text (they move into the DLL with paraLLEl-GS and our thin backend) |
| Vulkan-Headers | Apache-2.0 OR MIT | Notice |
| ps2-web-recomp donor `1b480879…` | MIT | Copyright + license text (runtime, translator, VU recompiler derivative) |
| naga (shader build tool, not shipped) | MIT OR Apache-2.0 | None for the release unless shipped |
| Rust crates of the launcher | mostly MIT/Apache-2.0 | Generated with `cargo about` into `licenses\launcher-crates.html` |
| Slint | GPLv3 / Royalty-free 2.0 / commercial | Royalty-free license for desktop apps requires attribution ("Made with Slint" in About) — or GPLv3 if the launcher is GPL. Choose with the project license question. |
| MSVC runtime DLLs (if `/MD`) | Microsoft Distributable Code | App-local copy allowed; include no headers/libs |
| NSIS | zlib/libpng-style | No notice required in the installer output |
| **GT4Recomp itself** | **MIT** (owner decision 2026-10-15; LICENSE added on main) | MIT notice in the installer and the About › Licenses screen; MIT is compatible with shipping the LGPL DLL and with the game-derived code under option A |

DLL ABI: a small `extern "C"` API (init with our `VkDevice`/queues, `gif_transfer`,
`write_register`, privileged register state, `read_transfer_fifo`, `map_vram_*`, `vsync`,
`set_super_sampling`, `set_hacks`, signal callbacks). Our glue (`parallel_gs_backend.cpp`) moves inside
the DLL. Cost: one indirect call per GIF transfer (negligible; **Hypothesis**, measure in F8).

**Launcher "Licenses" screen:** About → Open-source licenses: list of components (name, version/commit,
license, link), full texts rendered from `licenses\*.txt`, the LGPL relink note with a link to the
source package, and the trademark/ownership notice ("Gran Turismo and the Gran Turismo 4 artwork are
trademarks/copyright of Sony Interactive Entertainment / Polyphony Digital; GT4Recomp is an
unofficial fan project, not affiliated or endorsed").

## 6. Settings model

There are three short pages. The UI shows no variable names. Each control maps to exactly one variable (the A/B rule), all of them listed in one Rust table. Settings save as soon as they change and take effect at the next Play.

### 6.1 Graphics

| Group | UI control | Values (default **bold**) | Variable / paraLLEl-GS field |
|---|---|---|---|
| Image | Renderer (dropdown) | **paraLLEl-GS**, Legacy | `GT4_RENDERER=parallel\|donor` (Confirmed in the TASK-012 worktree) |
| Image | **Input resolution** (dropdown): race display mode. "i" card: "480p is recommended: it is progressive. Native (480i) and 1080i are interlaced, which looks softer and flickers. Menus are not affected." | Native (480i), **480p**, 1080i | `GT4_RACE_DISPLAY=480i\|480p\|1080i` (new; generalizes `GT4_FORCE_480P`, see section 2) |
| Image | Super-sampling (segmented) | Off, 2×, 4×, 8×, 16×. The default is chosen after the TASK-012 measurements; the mockup shows 4×. | `GT4_PGS_SSAA=1\|2\|4\|8\|16` → `SuperSampling::X1..X16` (Confirmed enum, `gs_renderer.hpp:232`) |
| Image | High-resolution output (toggle; always visible, dimmed and not toggleable unless super-sampling is 4×, 8× or 16×; choosing ≥4× unlocks it and restores the user's last value) | **On** at ≥4× | `GT4_PGS_HIGHRES` → `VSyncInfo::high_resolution_scanout`, which also needs `force_progressive` (Confirmed, `gs_renderer.cpp:4294-4310`) |
| Image | Super-sampled textures (toggle) | **Off** | `GT4_PGS_SS_TEXTURES` → `set_super_sampling_rate(…, super_sampled_textures)` |
| Interlacing | Deinterlacing (dropdown) | **Progressive**, Weave (motion-adaptive), Off | Progressive = `force_progressive`. Weave = paraLLEl-GS's built-in deinterlacer: the `weave` program, plus a motion-adaptive pass over the last 4 fields labelled "fastmad" in the source (Confirmed, `gs_renderer.cpp:5106-5190`). Off = `skip_deinterlace` (raw fields, shown as they are) |
| Interlacing | Reduce field blending (toggle) | **On** | `anti_blur` (the paraLLEl-GS header calls it "usually a good idea" with progressive or SSAA) |

**No helper text under any row** (all pages). Every option title has a small "i" in a circle; hover
(or keyboard/controller focus) opens a small paper-coloured card with one or two sentences. The
texts live in the settings table next to each option so Android can show them on tap.

### 6.2 Display

| Group | UI control | Values (default **bold**) | Variable / field |
|---|---|---|---|
| Window | Fullscreen (toggle) | **On** | `GT4_FULLSCREEN` (new) |
| Window | VSync (dropdown) | **On** (fifo), Fast (mailbox), Off (immediate). Modes the surface lacks are hidden, based on a Vulkan surface query. | `GT4_PRESENT_MODE` (Confirmed) |
| Picture | Show overscan | **Off** | `VSyncInfo::overscan` |
| Picture | Use the game's screen offsets | **Off** | `crtc_offsets` |
| Picture | Match the game's horizontal resolution | **On** (to be confirmed by measuring on GT4) | `adapt_to_internal_horizontal_resolution` |
| Picture | Direct scanout | **Off** | `raw_circuit_scanout` |

### 6.3 Advanced (kept short)

| Group | UI control | Default | Variable / field |
|---|---|---|---|
| Performance | Asynchronous GS | **On** | `GT4_GS_SYNC`: unset or 0 = on (Confirmed). Turning it off writes `GT4_GS_SYNC=1`. Under paraLLEl-GS this switch must map to that renderer's own submission path; how is **Unknown** until TASK-012 step 2 settles it. |
| Renderer hacks ("may cause glitches") | Disable mipmaps · Back-buffer promotion · Blend demotion · Unsynced readbacks | all **Off** | `GT4_PGS_HACKS=` comma list → `Hacks::{disable_mipmaps, backbuffer_promotion, allow_blend_demote, unsynced_readbacks}` (Confirmed struct, `gs_interface.hpp:211-237`) |
| Diagnostics | Performance report (on/off) | **Off** | `GT4_PERF=1` |
| Diagnostics | Check compiled VU code | **Off** (choices: Off / every 100th / every 1000th call) | `GT4_VU1_VERIFY`, `GT4_VU0_VERIFY` |

These are deliberately left out of the UI:
- **Frame pacing:** always paced. The launcher never sets `GT4_PACING`; the audio-clock A/B stays a developer switch in the Python launcher.
- **Developer and debug switches:** `PS2_VU_INTERPRET`, `PS2_SCALE`, paraLLEl-GS `ordered_super_sampling` (a debug aid) and `DebugMode`.
- **Controls remapping:** comes later; racing wheels are TASK-018.
- **CPU build variant:** chosen automatically from CPUID. Only the baseline x86-64 build ships until TASK-010 shows that x86-64-v3 helps.

### 6.4 How the launcher passes settings

- **Environment variables on the game process only.** Nothing changes in the system or user environment. The launcher starts from a scrubbed environment (it drops every inherited `GT4_*`/`PS2_*`, as `external_runtime_probe` does) and then sets the renderer plus any value that differs from its default.
- **One Rust table is the single source of truth.** Each entry lists: setting id → variable → allowed values → default → page → "requires" rules. Examples of rules: high-resolution output needs ≥4× and progressive; the two verify modes exclude each other. Values are validated on load and again before launch; an invalid value resets to its default.
- **Storage:** `%LOCALAPPDATA%\GT4Recomp\config\settings.toml`, with a `schema_version` and migrations.
- **Launch record:** each launch writes `launch.json` (arguments, variables, versions, hashes) into the session folder.
- **Later (optional):** the exe reads the same TOML through `--config`, so in-game settings menus and the Android build can share it.
- **Native work this implies:** `GT4_RACE_DISPLAY`, `GT4_FULLSCREEN` and the `GT4_PGS_*` names. TASK-012 owns the paraLLEl-GS ones. Each must refuse unknown values, as the existing variables do.

## 7. Launcher architecture

### 7.1 UI toolkit choice

| Toolkit | Look & motion | Maturity on Windows | Portability (Android/iOS later) | Verdict |
|---|---|---|---|---|
| **Slint** | Declarative `.slint` markup, built-in property animations, states/transitions, custom fonts, image effects (blur/drop shadow, rounded clipping), live preview for design iteration | Stable (1.x), native Windows with Skia or FemtoVG renderer, high-DPI | Android supported; iOS supported in recent releases (**High confidence**); embedded too | **Chosen** |
| egui | Immediate mode; polishable but animations/typography need hand-work; looks like a tool unless heavily customized | Very stable | Android possible, iOS weak | Good for debug panels, not for the polished front |
| iced | Elm architecture, good theming; animation support still limited (**High confidence**) | Stable-ish, APIs churn between releases | Mobile not first-class | Second choice |
| GPUI (Zed) | Beautiful, GPU-native | Windows support is recent, API unstable, few docs outside Zed | No mobile | Too risky now |

Slint's licensing (Royalty-free 2.0 with attribution, or GPLv3) fits either project-license answer.

**Superseded (owner, 2026-10-15): the frontend moves to Tauri 2 with the HTML mockups; see section 15.**
The Rust crates stay; only `gt4l-app` (Slint) is replaced.

### 7.2 Crate layout (`launcher/` Cargo workspace inside the repo)

```
launcher/
  Cargo.toml                # workspace, pinned Cargo.lock
  crates/
    gt4l-core/              # no UI: paths, settings schema, config TOML, logging, card manager,
                            #   release manifest verification, single-instance, launch/supervise
    gt4l-disc/              # ISO9660 reader, SYSTEM.CNF parse, SHA-256 with progress,
                            #   CORE.GT4 raw-deflate + ELF rebuild (port of ee_producer prepare),
                            #   GT4.VOL catalogue reader (port of gt4_volume.py subset) for photo/*.JPG
    gt4l-payload/           # trait GamePayload + BundledPayload (A); BuildFromIsoPayload stub (B)
    gt4l-mcfs/              # TASK-025: PS2 card filesystem (ECC), save importers/exporters
    gt4l-gt4save/           # TASK-025: GT4 save decryption (PDTools port), read-only summary
    gt4l-app/               # Slint UI (ui/*.slint), app state machine, theme, brand loading
      build.rs              # embed-resource: icon + VERSIONINFO from $GT4_BRANDING_DIR
    gt4l-cli/               # headless: `gt4l-cli prepare --iso X --data-dir Y`, `verify`, `launch --dry-run`
  tests/                    # synthetic ISO fixtures, parity tests vs Python (private, skip if absent)
installer/
  gt4recomp.nsi, pages/*.nsh, art/ (bitmaps generated from launcher/assets/brand/)
launcher/assets/
  brand/  brand.toml + official assets (bundled)      brand-neutral/  brand.toml + original assets
```

Key crates: `slint`, `sha2` (asm), `flate2` (raw deflate, `miniz_oxide` or `zlib-rs` backend),
`serde` + `toml`, `tracing` + `tracing-appender` (rolling `launcher.log`), `windows` (CreateProcessW,
Job Objects, named mutex, `SHGetKnownFolderPath`, file IDs), `rfd` (native file dialog), `ash`
(Vulkan pre-flight), `raw-cpuid`, `image` (JPEG decode/resize to cached 1920x1080 WebP/PNG),
`fs4` (file locks), `embed-resource`.

### 7.3 Payload abstraction (the A→B switch)

```rust
trait GamePayload {
    fn describe(&self) -> PayloadInfo;                  // kind, version, expected files
    fn prepare_steps(&self, ctx: &PrepareCtx) -> Vec<Step>; // UI renders these generically
    fn verify(&self, ctx: &PrepareCtx) -> Result<Verified, PayloadError>; // fast check per launch
    fn game_executable(&self, ctx: &PrepareCtx) -> PathBuf;
}
```

Common steps (both kinds): `PrecheckDisc`, `HashDisc`, `ExtractCore`, `RebuildElf`, `PrepareCard`,
`ExtractArt`, `WriteReceipt`. A adds `VerifyBundledBinaries`. B adds `Translate` (donor translator →
generated sources, inventory hash check), `ExtractVuImages`, `RecompileVu`, `CompileGameDll`
(clang-cl, job count from cores and free RAM), `LinkGameDll` (lld-link, imports from the host exe),
`VerifyGameDll`. Selection is a build-time feature (`payload-bundled` / `payload-build`), so the B
launcher contains no reference to the official branding or bundled payload.

### 7.4 Paths

| What | Where |
|---|---|
| Program (installer-owned, replaced on update) | `%LOCALAPPDATA%\Programs\GT4Recomp\` → `GT4Recomp.exe`, `game\gt4recomp-game.exe`, `game\gt4_parallel_gs.dll`, `game\shaders\*.spv`, `branding\`, `licenses\`, `release-manifest.json`, `uninstall.exe` |
| User data (survives updates/uninstall unless asked) | `%LOCALAPPDATA%\GT4Recomp\` → `config\settings.toml`, `prepared\` (CORE, ELF, receipt), `disc\`, `gs\`, `runtime\card.ps2` (section 13), `library\backups\`, `logs\launcher.log`, `sessions\session-YYYYMMDD-HHMMSS\` (keep last 20). **Saves and replays (TASK-081, owner 2026-10-21):** `Documents\Gran Turismo 4\Saves\` (`Active\` = the active save, `Save NN\`, `.backup\`) and `\Replays\`, found through the Documents known folder and always kept by the uninstaller (see FLOW-AUDIT section 8) |

`%LOCALAPPDATA%` (not Roaming) avoids syncing 8 MB cards and 6 MB ELFs through roaming profiles/
OneDrive. A "Portable mode" (a `portable.txt` next to the exe redirects data to `.\data`) is cheap to
add and handy for testing.

### 7.5 Launch, supervision, crash report

- Single instance: named mutex `Local\GT4Recomp.Launcher`; a second start activates the first window.
  Game lock: the runtime's exclusive `<card>.lock` plus a launcher byte lock so two games never share a card.
- Launch: verify payload (fast), cheap disc recheck, back up card, create session folder, write
  `launch.json`, `CreateProcessW(game\gt4recomp-game.exe, "prepared\SCUS_973.28 --ntsc --disc <iso>
  --frames 0 --memcard runtime\card.ps2 (built from the folder card) --misses <session>\misses.txt --window")`, cwd = session,
  stdout/stderr to `<session>\game.log`, process in a Job Object (so "Stop game" works and children die
  with it). The ELF is passed in place (`prepared\SCUS_973.28`, the name argv0 needs) instead of
  copying it per session.
- While the game runs, Play becomes "Running · Stop" (nothing else) and the launcher can
  minimize to the taskbar; option "Close launcher when the game starts" (off by default, because the
  launcher reports crashes).
- Exit classification: exit code 0 and `[ee] stopped … window closed` = normal. Anything else = crash
  dialog: what happened in one sentence (reason from the `[ee] stopped` line, e.g. "no translated code
  at 0x…"), no log text on screen, buttons **Open session folder**, **Copy report** (zip of `game.log`,
  `launch.json`, `misses.txt`, minidump if present; card and ISO excluded; user name in paths
  replaced by `%USER%`), **Play again**. Nothing is uploaded automatically.

### 7.6 Updates (later)

Opt-in "Check for updates" (GitHub Releases API of the releases host, at most daily). When a newer
version exists the launcher shows the notes and asks before downloading the installer to a temp
folder; it then runs it silently (`/S`) into the same per-user folder and restarts. Data is untouched.

## 8. Branding and assets

### 8.1 Decision: official assets bundled in the launcher

The owner decided (round 3) to download public official GT4 images and logos and bundle them in the launcher. Every official file lives in one folder, `launcher/assets/brand/`, which the Slint build embeds. The game exe's `.rc` takes its icon from the same folder. A manifest, `launcher/assets/brand/brand.toml`, lists each file with its source URL, retrieval date, SHA-256 and pixel size.

A de-branded build (fallback if Sony objects) is one switch: the cargo feature `brand-neutral`, plus `GT4_BRANDING_DIR` for the game's `.rc`, point at `launcher/assets/brand-neutral/` instead. That folder holds original art and the text name "GT4Recomp". A CI check fails if any hash from the official manifest appears in a neutral package.

The Windows display name is **"Gran Turismo 4"** everywhere the OS shows it:
- the Start Menu shortcut;
- the window titles of the launcher and the game;
- `FileDescription` and `ProductName` in both exes' VERSIONINFO, which Task Manager shows;
- the installer title and the "Apps & features" `DisplayName`.

Internal ids stay `GT4Recomp`: the install folder, the registry key, the AppUserModelIDs `GT4Recomp.Launcher` and `GT4Recomp.Game`, the data folder and the repository. Keeping them stable means a later rename never orphans user data.

### 8.2 Official images to source (listed only; nothing has been downloaded)

| Asset | Used in | Resolution needed | Candidate public sources (verify size/quality before download) |
|---|---|---|---|
| `cover-front.png`: GT4 NTSC-U front box art | Setup left column (300x425 shown); About; installer welcome bitmap (cropped); blurred backdrop before setup | ≥ 900x1275 (3x of the 300x425 slot), ideally ~1200x1700, no scan glare, no plastic-case border | 1. Giant Bomb image gallery for Gran Turismo 4: lists a "High Res Cropped NTSC PS2 Cover" (seen in search results). 2. The Cover Project (thecoverproject.net, full-wrap scans; front crop needed). 3. MobyGames cover-art page for GT4 (PS2, NA release). 4. LaunchBox Games Database (front box image). 5. libretro-thumbnails `Sony_-_PlayStation_2/Named_Boxarts/Gran Turismo 4 (USA).png` (GitHub; usually ~512 px, too small for 2x/3x). The Wikipedia infobox image is a low-resolution fair-use file: not suitable. |
| `logo-gt4.png` (or `.svg`): GT4 wordmark, transparent | Home bottom-left above Play (≈360x96 slot); Setup header; Settings nav; installer header | Vector preferred; else PNG ≥ 1600 px wide with alpha | 1. Vector copies on logo sites (VectorSeek "Gran Turismo 4 Logo Vector", iconape "gran turismo 4 logo" SVG). Their terms say "not for commercial use / identification only", which fits a free fan project but should be recorded. 2. The Gran Turismo fandom wiki file pages. 3. Clean trace from the high-resolution cover (last resort). |
| `emblem-gt.svg`: the GT emblem (the GT4-era "GT" mark) | Source for `launcher.ico` and `game.ico`, plus the small title-bar icon | Vector, or PNG ≥ 1024x1024 with alpha | 1. Cropped from the GT4 logo vector above (the emblem is part of the GT4 logo). 2. Brandfetch's gran-turismo.com assets (modern GT emblem, SVG); use only if the GT4-era mark is unavailable, since its style differs. |
| `launcher.ico`, `game.ico` | Taskbar, Task Manager, Start Menu, Alt+Tab, window title | Multi-size ICO: 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 (256 PNG-compressed) | Generated from `emblem-gt.svg` by `scripts/make_icons.py` (Pillow + resvg). Game = emblem on a dark tile; launcher = same emblem with a small play-chevron badge, so the two taskbar buttons differ. |
| Home background | Home | 1920x1080 | **Not bundled**: comes from the user's disc at setup (`GT4.VOL photo/*.JPG`, Confirmed). Before setup completes, a blurred `cover-front.png` is shown. |
| NSIS bitmaps | Installer welcome/finish (164x314, @2x 328x628) and header (150x57, @2x 300x114) | — | Generated from `cover-front.png` and `logo-gt4.png` at build time |

### 8.3 Trademark / IP notice

The same text goes in three places: the repository (`NOTICE.md`, linked from README), the About page, and the installer's first page.

> **Gran Turismo 4** — GT4Recomp is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment Inc. or Polyphony Digital Inc. "Gran Turismo", "GT", the Gran Turismo 4 logo, the GT emblem and the Gran Turismo 4 cover artwork are trademarks or copyrighted works of Sony Interactive Entertainment Inc. and/or Polyphony Digital Inc., used here only to identify the game. No game data is included in the source repository; you need your own copy of Gran Turismo 4 (USA, v2.00) to play. If you are a rights holder and want something removed, please open an issue at github.com/moesuito/gt4-staticrecomp.

`NOTICE.md` also lists each bundled official asset with its source and date, taken from `brand.toml`.

### 8.4 Embedding in the game exe (CMake)

`native/gt4recomp.rc.in` → `configure_file` with:
- `IDI_APP ICON "@GT4_BRANDING_DIR@/game.ico"`;
- a `VS_VERSION_INFO` block: `FileDescription` and `ProductName` "Gran Turismo 4" (de-branded: "GT4Recomp"), the version numbers, and `LegalCopyright` carrying the short unofficial-project notice.

Then `enable_language(RC)` (llvm-rc with clang-cl).

`window.cpp` registers a `WNDCLASSEXW` with `hIcon`/`hIconSm` loaded from `IDI_APP` and sets the window title "Gran Turismo 4". Each process sets an explicit AppUserModelID, and the Start Menu shortcut carries the same ID so pinning works. The launcher gets its icon and version resource through `embed-resource` in `build.rs`.

## 9. Installer (NSIS)

- `RequestExecutionLevel user`; `InstallDir "$LOCALAPPDATA\Programs\GT4Recomp"`; `SetShellVarContext
  current`; registry only under `HKCU\Software\GT4Recomp` and
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\GT4Recomp` (DisplayName, DisplayIcon,
  DisplayVersion, Publisher, EstimatedSize, UninstallString, QuietUninstallString, NoModify/NoRepair).
- Installer title, Start Menu shortcut and "Apps & features" `DisplayName` = **"Gran Turismo 4"**
  (registry key and folder stay `GT4Recomp`). Published as GitHub Releases of
  `moesuito/gt4-staticrecomp`.
- Pages (MUI2, bitmaps generated from `launcher/assets/brand/`): Welcome (cover art + "you need your own GT4 USA v2.00
  disc image") → Notice/License (unofficial project, third-party licenses) → Folder (default shown,
  changeable) → Install → Finish ("Start Gran Turismo 4" checked; "Create desktop shortcut" checkbox).
  The beautiful onboarding happens in the launcher; the installer stays short and clean.
- Shortcuts: Start Menu `Gran Turismo 4.lnk` (launcher icon, AUMID set via `WinShell` plugin), optional
  desktop shortcut. No auto-start, no file associations.
- Silent mode (`/S`) for updates; upgrade in place detects a running launcher/game and asks to close.
- Uninstaller: removes the program folder and shortcuts; a custom page asks
  "Also delete my saves, settings and prepared files?" (**unchecked by default**); when checked, a
  second line lists the memory-card path. Never deletes the user's ISO.
- Packaging script (`installer/build_release.py`): copies the pinned release files from a qualified
  slice build, writes `release-manifest.json` (path, size, SHA-256), bundles licenses and the LGPL
  source package link, runs `makensis`, hashes the installer.
- **Code signing / SmartScreen:** an unsigned installer gets the "Windows protected your PC" warning
  until it builds reputation per file hash. Options: (1) unsigned v1 with a README note; (2) OV
  certificate (~USD 200-400/year, hardware token/cloud HSM; reputation still builds gradually; EV no
  longer bypasses SmartScreen — High confidence); (3) Azure Trusted Signing (low monthly cost;
  eligibility for individuals outside some countries **Unknown**). Free OSS signing services
  (e.g. SignPath Foundation) would very likely refuse a binary that contains game-derived code
  (**High confidence**). Unsigned freshly built binaries also raise AV false-positive risk.

## 10. Implementation milestones (TASK-019 branch `task/019-launcher`)

| # | Milestone | Acceptance checks | Effort (agent-days) |
|---|---|---|---|
| M0 | Decision record + TASKS/README release wording; branding folders with neutral placeholders | Decision file merged; neutral package builds | 0.5 |
| M1 | Native release prerequisites: exe-relative shader dir (release config), `.rc` icon + VERSIONINFO, window `hIcon`, `GT4_FULLSCREEN`, `GT4_RACE_DISPLAY` (default 480p; 480i/1080i holds), minidump filter, decide `/MD`+app-local CRT vs `/MT` | CTest + Python suite green; new fixture: exe started from a copied folder finds shaders; headless 4,200-frame smoke identical to slice 166 (480p hold only changes race display index); owner check of icon in taskbar/Task Manager | 2-3 |
| M2 | paraLLEl-GS as `gt4_parallel_gs.dll` with C ABI (after TASK-012 step 2 lands) | Fixture suite passes through the DLL; frames identical to the static build; F8 shows no GS cost change beyond noise; relink test: rebuild DLL from the source package, drop in, game runs | 2-3 |
| M3 | `gt4l-disc`: ISO9660 + SYSTEM.CNF + hash + CORE/ELF rebuild + VOL photo reader | Synthetic ISO fixtures (good, wrong serial, PAL-like, Spec II-like, truncated, wrong VER, bin/cue); private parity tests: CORE/ELF bytes equal the pinned files; Spec II ISO rejected with the Spec II message; 64 photos extracted with hashes recorded | 3 |
| M4 | `gt4l-core` + `gt4l-payload` (Bundled): paths, settings table + TOML, card manager (new/import/backup/lock), launch/supervise, exit classification, crash report zip, single instance | Unit tests for every validation rule mirrored from `play_performance.py`; dry-run launch prints the exact argv/env; scrubbed env test; crash classification tests from recorded logs; second instance focuses the first | 3 |
| M5 | Slint UI with the tokens and components of section 12: setup list, home, settings (Graphics/Display/Advanced/About), running state, crash dialog, licenses; keyboard/gamepad navigation (XInput: D-pad + A to PLAY) | Screens match the mockups at 1280x720 and scale at 125/150%; reduced-motion honored; whole flow usable by keyboard and controller; owner design review | 6-8 |
| M6 | Branding: source the official images listed in 8.2 (owner approves each), `brand.toml` with sources/hashes, icon generation, `NOTICE.md` + About notice, neutral swap test | Neutral package contains no official asset hash; official build shows art/icons everywhere listed in 8.2; notice present in repo, installer and About | 1-2 |
| M7 | NSIS installer/uninstaller + release packaging + license bundle | Install/uninstall as a standard (non-admin) user in Windows Sandbox; non-ASCII user name; OneDrive-redirected Desktop; upgrade over previous version keeps data; uninstall keeps saves by default | 2-3 |
| M8 | End-to-end QA + owner test | Clean machine: install → select ISO → play → save → exit → relaunch skips setup → crash path (forced) produces a report; owner plays the reference scene from the launcher | 2 |
| M9 | Release candidate: CHANGELOG, known issues, GitHub Release on `moesuito/gt4-staticrecomp` | Owner approval → tag | 0.5 |
| — | **Dependency: TASK-025 save manager** (section 13): slot storage and the Saves page; the launcher's v1 can fall back to a single slot if it slips | see 13.6 | 14.5-17.5 (cut 12-14.5) |
| M10 (insurance, optional) | Option B spike: freestanding game-code DLL + bundled clang-cl/lld-link build on a test VM | Game DLL built on a clean VM from the ISO; frames identical; F8 shows the DLL boundary cost; first-run time measured on 4 and 16 threads | 5-8 |

Total for A: about **22-28 agent-days** (M0-M9), plus owner review/testing time. M2 depends on
TASK-012 progress; M1-M5 can start in parallel with it (single-writer rule: one branch writer at a time).

## 11. Open questions for the owner

1. **Code signing:** ship v1 unsigned (SmartScreen warning until reputation builds) or pay for a certificate (section 9)?

Decided on 2026-10-15 (recorded so nobody re-asks):
- **Project license:** MIT for our own code. Third-party parts keep their licenses; paraLLEl-GS stays LGPL inside its DLL.
- **Hosting:** installers are published as GitHub Releases of `moesuito/gt4-staticrecomp`, and the launcher source lives in the same repository.
- **Display name:** "Gran Turismo 4".
- **Artwork:** public official images, bundled (section 8).

## 12. Visual design system (round 2: clean, minimalist, Gran Turismo identity)

The mockups sit next to this file. All three are 1280x720, use inline CSS and quiet original placeholder art:
- `mockup-setup.html`: checking / wrong disc / done
- `mockup-home.html`: ready / running
- `mockup-settings.html`: Graphics / Display / Advanced

The "Official asset slots" toggle outlines every official or disc-derived asset in magenta, with its file and source. Rendered screenshots are in `../shots/*.png`.

### 12.1 What the Gran Turismo identity is (research)

- **Official web brand (Confirmed: I read the computed styles on gran-turismo.com on 2026-10-15):**
  - Fonts: `"Helvetica Neue", HelveticaNeue-Medium, HelveticaNeue-Light, Roboto` and `"Roboto Condensed"` (loaded from Google Fonts), plus one heading face, `bebas-neue-pro`.
  - Backgrounds: `#000000`, `#13161B`, `#0A0C14`, and translucent `rgba(67,67,67,.85)`.
  - Text: `#FFFFFF`, `#DEDEDE`.
  - Light surfaces: `#E7E7E7`, `#F8F8F8`, with dark text `#171717` on them.
  - The GT7 online manual uses the same font stack.
- **GT4 in-game (Confirmed string):** the European-language font file on the disc, `GT4.VOL font/europe.kf`, contains the name **`ShinGo-M`** (Morisawa A-OTF Shin Go Medium). The other GT4 font files name `DFHeiBold-B5` (DynaFont, Traditional Chinese) and custom bitmap sets (`FC_*`, `DC_*`, `GT4meter`). So GT4's Latin UI text is set in Shin Go Medium (**High confidence**).
- **GT7 / GT Sport in-game UI typeface: Unknown.** No public source names it. The logo is custom lettering, not a font.
- **Look**, from GT4, GT Sport and GT7 menu screenshots:
  - full-bleed car photography;
  - quiet panels: near-black or charcoal in GT7 options and GT Sport, pale silver or paper in GT4 menus and the GT7 Café;
  - thin hairline separators;
  - small uppercase labels with wide letter spacing, and large light-weight titles;
  - flat line icons and one restrained accent colour;
  - almost no glow or decoration.

  We borrow this identity, not the layout of any particular GT4 screen.

### 12.2 Tokens

| Token | Value | Use |
|---|---|---|
| `ink` | `#0A0C14` | Window background, nav column (from gran-turismo.com) |
| `carbon` | `#13161B` | Page background (from gran-turismo.com) |
| `graphite` | `#1E2229` | Controls, inactive toggles |
| `hair` | `rgba(255,255,255,.10)` | Row separators, outlines |
| `white` / `silver` | `#FFFFFF` / `#DEDEDE` | Primary / secondary text (from gran-turismo.com) |
| `dim` / `faint` | `#9AA0A8` / `#5C626B` | Helper text / disabled |
| `paper` / `paper-ink` | `#F8F8F8` / `#171717` | Primary button, selected segment, toggle "on" (light surface from gran-turismo.com) |
| `accent` | `#2C6BED` | Keyboard and controller focus ring only (**Hypothesis**: a GT-style blue; the owner can pick another) |
| `ok` / `error` | `#2FA866` / `#D64545` | Ticks, "Not supported" |

**Type scale** (size/line height in px):
- Titles: 34/40 Light (setup), 30/36 Light (settings); the neutral-build logo text is 44 Light.
- Body 14/20 Regular; helper text 12/16.
- Labels: Roboto Condensed Medium at 11–13, uppercase, letter spacing 0.12–0.24 em.

**Spacing:** 8 px grid (8, 16, 24, 32, 40, 48, 72, 96). Corner radius 3 px. Rows are 48 px high (56 px in setup).

**Effects:** no shadow except on the cover art, no gradients on controls, no glow anywhere.

### 12.3 Fonts

| Role | Official / reference | Redistributable? | We ship |
|---|---|---|---|
| UI text | Helvetica Neue (gran-turismo.com); Shin Go Medium (GT4 in-game) | No (commercial licenses from Linotype/Monotype and Morisawa) | **Roboto**: the official site's own fallback. SIL Open Font License 1.1 in current Google Fonts releases; older releases were Apache-2.0 (**High confidence**) |
| Labels / numbers | Roboto Condensed (gran-turismo.com) | Yes | **Roboto Condensed** (same license as Roboto) |
| Display heading (optional) | Bebas Neue Pro (gran-turismo.com) | No (commercial) | Not used. If ever needed: Bebas Neue (OFL) |
| Japanese/CJK (later: Android, other locales) | Shin Go / DynaFont | No | Noto Sans JP / Noto Sans CJK (OFL) |
| Logo | Custom GT lettering | Official asset | Only as `logo-gt4.png` in `launcher/assets/brand/` |

The fonts are embedded in the Slint build, so nothing depends on fonts installed on the machine. This matters on Android.

### 12.4 Components (Slint, portable)

| Component | Description |
|---|---|
| `TitleBar` | Icon slot, app name, window buttons |
| `PrimaryButton` | Paper fill, ink text, uppercase condensed label, 52 px high; hover darkens; focus = 2 px accent ring |
| `SecondaryButton` | Hairline outline |
| `IconButton` | 40 px; used for the settings gear |
| `OptionRow` | Label plus an optional one-line helper on the left, exactly one control on the right, hairline below |
| `GroupHeader` | Small uppercase label |
| `Dropdown` | — |
| `Segmented` | Selected segment = paper fill |
| `Toggle` | On = paper track with an ink knob |
| `StepRow` | Number, label, short detail, and a tick, percentage or status |
| `ProgressLine` | 2 px bar |
| `NavList` | Text items; a 2 px marker on the left shows the current page |
| `ArtBackground` | Cross-fading image with a dark shade along the bottom |

Components make no platform calls. The Windows shell (title bar, file dialog, process launch) stays outside them, so Android can reuse the same `.slint` files with a different shell.

### 12.5 Screens and motion

- **Setup:** cover art on the left. On the right: logo, "Set up your game", "This only happens once.", four step rows and one primary button. For a wrong disc: one red status, a two-line explanation and "Choose another file".
- **Home:** full-bleed disc photo, logo at bottom-left, Play under it, a Settings gear at top-right. While the game runs, Play becomes "Running · Stop". Nothing else: no cards, no news, no hints, no footer.
- **Settings:** a nav column (Back, logo, Graphics, Display, Saves, Replays, Advanced, About, version). Option rows carry no helper text; an "i" next to each title opens a hover card (one state shown open in the mockup). High-resolution output is dimmed and locked below 4× (both states rendered). Each page has a title, the single line "Changes apply the next time you play." and its option rows. About holds licenses, disc and game files, repair and log access.
- **Motion:**
  - background cross-fade of 1 s every 15 s;
  - button hover 150 ms, toggle 150 ms;
  - page-change fade 200 ms;
  - the progress bar's width eases.

  There is no Ken Burns, sheen, breathing effect or staggered entrance. Reduced-motion turns all of it off.
- **Input:** keyboard and XInput. D-pad moves focus, A activates, B goes back, Start = Play.

## 13. Save manager (separate task; the launcher depends on it)

Proposed as **TASK-025: Save manager** (appended after the phase 2/3 numbers, per the TASKS.md rule).
- **Dependency:** the TASK-019 Saves and Replays pages and the folder-card launch need its core. The launcher can ship v1 with one slot only if TASK-025 slips.
- **Effort:** about 14.5–17.5 agent-days for v1 (table in 13.6).

### 13.1 How saves are stored today (study)

All findings come from read-only inspection of `private/manual-play/card.ps2` and two pinned seed cards, using a throwaway parser in the scratchpad, plus the donor `runtime/src/iop/sio2.cpp`.

| Finding | Evidence | Label |
|---|---|---|
| The card is a **raw PS2 memory card image with ECC**: 8,650,752 bytes = 16,384 pages × 528 bytes (512 data + 16 spare/ECC). | File size; donor `sio2.cpp:47` `kPageBytes = 528, kPages = 0x4000` | Confirmed |
| Superblock `"Sony PS2 Memory Card Format 1.2.0.0"`, page 512, 2 pages per cluster, 16 pages per erase block, 8,192 clusters, `alloc_offset` 41, `alloc_end` 8,135, root directory at cluster 0, indirect FAT cluster 8, card type 2, flags 0x2B. | Parsed superblock (owner card and both seeds identical) | Confirmed |
| This is the same file format as PCSX2's 8 MB file memory cards (`.ps2`). `.mcd` images from other tools use the same raw layout. Some dumps omit the ECC bytes (8,388,608 bytes); those need ECC added on import. | Format match; PCSX2 behaviour from knowledge | High confidence |
| The runtime has **one card, slot 1 only**. SIO2 port 2 goes to `memory_card()`; no slot 2 exists. | `sio2.cpp:269-270` | Confirmed |
| The runtime opens it via `--memcard <path>`. It reads up to 8,650,752 bytes into memory; a missing file becomes an **unformatted** card (all 0xFF); a shorter file is padded with 0xFF. | `sio2.cpp:173-179`, `host/main.cpp:51` | Confirmed |
| Writes rewrite the **whole file in place** (`ofstream` truncate) once 50 VBlanks pass without new card writes. This is not atomic: a crash or power loss mid-write can corrupt the card. | `sio2.cpp:322-333` | Confirmed |
| ECC bytes on our cards are written by GT4's own MCMAN module (the runtime stores the spare bytes as sent; 13.2), so they should be valid for PCSX2. Not yet verified by recomputing them. | 13.2 study | High confidence |
| **Contents:** one root entry, directory `BASCUS-97328GAMEDATA`, holding `garage` 1,280,064 B, `BASCUS-97328GAMEDATA` 237,760 B, `icon0.ico` 9,796 B and `icon.sys` 964 B (title "ＧＲＡＮ　ＴＵＲＩＳＭＯ　４ / Ｇａｍｅ　ｄａｔａ"). Total 1,528,584 B. Free space 6,635 KiB. | Directory walk | Confirmed |
| **Only one GT4 game save fits per card in practice.** The directory name is fixed (`BASCUS-97328GAMEDATA`), so a second save would overwrite the first, even though about 5 × 1.5 MB would physically fit. | Fixed name seen on 3 cards; GT4 behaviour | High confidence |
| GT4 also writes replay and photo saves to other directories. The disc has save icons for them: `GT4.VOL icon/GT4_Rep_*.ico`, `GT4_photo_*.ico`, `GT4_film_*.ico`. Their exact directory names and sizes are not yet known. | Disc catalogue | Confirmed (icons); Unknown (names) |
| Backups today: the Python launcher copies `card-before.ps2` into each session folder (34 sessions exist). There is no rotation and no restore UI. | `external_runtime_probe.py`; folder listing | Confirmed |
| **PCSX2 folder memory cards**: a host directory with one sub-folder per save plus PCSX2 metadata files (`_pcsx2_superblock`, `_pcsx2_meta_directory`). Our runtime cannot mount them, and emulating them would mean re-implementing PCSX2's GPL FolderMemoryCard (not wanted). The launcher can still **import** from one by reading the save sub-folder as a raw directory. | PCSX2 behaviour from knowledge | High confidence |
| **Single-save formats:** `.psu` (uLaunchELF/EMS: uncompressed, 512-byte header per entry); `.max` (Action Replay MAX: `Ps2PowerSave` header, LZARI-compressed); `.cbs` (CodeBreaker: `CFU` header, RC4-style encryption + zlib); `.xps`/`.sps` (X-Port/SharkPort: `SharkPortSave` header, uncompressed); a raw directory (folder containing `icon.sys` and the files). We implement all of them from format facts. mymcplus is GPL-3.0, so its code is not copied into MIT code. | Public format documentation | High confidence |
| **Readable progress:** Nenkai PDTools (MIT, commit `ffe0bb2`, `PDTools.SaveFile.GT4.GT4Save.Load(directory)`) decrypts GT4 saves. GT4SaveEditor shows the profile name, credits (`GarageFile.Money`) and the calendar day from them. Porting that decryption to Rust could give "Day N · Cr X" per slot. **Not in v1** (owner, round 4): kept as a later optional step, after our own reverse-engineering review. Caveat (from its README): some garage cars do not decrypt exactly because of PS2 float behaviour; that does not affect name, money or day. | Private clones `private/slice151-research/nenkai/{PDTools,GT4SaveEditor}` | High confidence (feasible); deferred, not v1 |

### 13.2 Storage model (round 6): a folder memory card in user data, materialized to a card image around Play

#### How our runtime emulates the memory card (study)

| Finding | Evidence | Label |
|---|---|---|
| **The card is emulated at the raw SIO2 / flash-page level, not at the file level.** `memory_card()` implements the PS2 card's wire commands. `0x21/0x22/0x23` set the page address; `0x42` writes raw bytes, including the 16 spare (ECC) bytes the IOP sends; `0x43` reads raw bytes with an XOR checksum; `0x82` erases a 16-page block to 0xFF; `0x26` reports the card geometry (page size 0x200, 16 pages per block, 0x4000 pages = 8 MB); `0x27/0x28` handle the terminator and status. | Donor `runtime/src/iop/sio2.cpp:182-258` | Confirmed |
| **The PS2 filesystem (superblock, FAT, directories, `icon.sys`, ECC) is maintained by GT4's own IOP modules** `MCMAN`/`MCSERV`/`SIO2MAN`/`SIO2D`, loaded from `cdrom0:\IRX\` and **interpreted** on the IOP (`register_hle_module` has no caller). There is no HLE of `mcman`/`mcserv`/`libmc` RPC calls (open/read/write/getdir/mkdir/delete) anywhere in the donor or in `native/`. | `docs/IOP-MAP.md` (slice 151 study); grep of `runtime/src`, `runtime/include`, `native/` for mcman/mcserv/sceMc finds nothing | Confirmed |
| Consequence: ECC bytes and the directory/FAT layout on our cards are written by the real Sony `MCMAN` code, so cards produced by our runtime should be valid for PCSX2 and real hardware. | Previous row | High confidence |
| The backing store is one 8,650,752-byte vector loaded from `--memcard` at IOP reset; a missing file becomes a blank (0xFF) card. | `sio2.cpp:47-48`, `173-179` | Confirmed |
| `flush_cards()` runs once per VBlank (`system.cpp:132`). It rewrites the whole file in place (`ofstream` truncate) once 50 VBlanks (~0.83 s) pass with no card write. It is **not atomic**, and the donor has **no flush on exit**: closing the window less than ~0.8 s after a save could lose that save. | `sio2.cpp:322-333`, `system.cpp:127-133`; no other caller found | Confirmed (code); data-loss window is Hypothesis until tested |
| Our native host only adds an exclusive `<memcard>.lock` file. | `native/window.cpp:317-324` | Confirmed |
| The game always sees an **8 MB card** (geometry fixed by the `0x26` reply). About 6.6 MB is free next to one game save, so the number of replays visible **in-game at once** is limited whatever the host-side storage model. Presenting a larger card is untested with GT4's MCMAN. | `sio2.cpp:216-221`; card walk in 13.1 | Confirmed (8 MB); Unknown (larger cards) |

#### Options compared

| | (a1) Folder card **in the runtime**, at the raw-page layer | (a2) Folder card via **HLE of MCMAN/MCSERV** (file-level RPC) | **(b) Folder in user data + one raw card image built by the launcher** | (c) One raw card per save slot (previous plan) |
|---|---|---|---|---|
| How | On load, synthesize a full 8 MB FAT image (superblock, FAT, directories, ECC) from host folders. On flush, parse the FAT back and write changed files and directories to the host. Same idea as PCSX2's FolderMemoryCard, designed from behaviour only (no GPL code). | Replace GT4's disc IRX `MCMAN`/`MCSERV` with native handlers for every `sceMc*` RPC (Open, Read, Write, Seek, GetDir, Mkdir, Delete, Format, GetInfo, ChDir, SetFileInfo, …) that map onto host files. | The user-data folder holds every PS2 directory as a separate item. Before Play the launcher writes a fresh card image (the active game save + the chosen replays and playlists); after exit it reads the card back and updates the folder. The runtime is unchanged. | Each slot is its own 8 MB card file. |
| Runtime change | Yes: `sio2.cpp` backing store | Yes, large: new IOP HLE module and a stop on loading the disc IRX | **None** (only the recommended atomic-flush and exit-flush fixes) | None |
| Effort | 10–15 agent-days | 15–25 agent-days | **3–4 on top of `gt4l-mcfs`** | Already planned |
| Fidelity risk | High: mid-write FAT states, free-space accounting, directory timestamps, `icon.sys` and mode bits, erase-block semantics, write-back ordering | High: must match the exact RPC semantics and timings GT4 expects; leaves the real Sony module path | **Low**: the game still talks to real MCMAN on a real-format card | Low |
| Crash safety | Two representations kept live during play; hard to make atomic | Per-file writes; partial saves possible | Card image is the only live state during play. The folder changes only after exit, through temp dirs and renames. The previous card image is kept as backup. | Card image per slot |
| PCSX2 import/export | Folder layout similar to PCSX2's | Same | Import `.ps2`/`.mcd`/folder cards/single saves; export `.ps2`, `.psu`, or a folder of save directories | `.ps2` per slot |
| Android reuse | C++ runtime code (portable) | C++ (portable) | **Rust crate, UI-independent** (portable) | Same |
| Owner's folder model | Yes | Yes | **Yes, from the user's point of view** (the folder is the truth) | No |

#### Recommendation: (b)

It gives the owner the folder-card model he uses in PCSX2: every save and replay is a separate item in a folder, all replays kept, one game save active. It adds no emulation risk, because GT4's real `MCMAN` keeps writing a real card. It also keeps one simple crash-safety story. (a1)/(a2) can stay a later option if live folder sync during play is ever needed.

Two small runtime fixes go with it, as one overlay capsule with a CTest fixture: an **atomic flush** (temp file + `ReplaceFileW`) and a **final flush on exit**.

#### Layout

```
%LOCALAPPDATA%\GT4Recomp\
  memcard\                         # the folder card: what the game sees next Play
    BASCUS-97328GAMEDATA\          #   the active game save (exactly one, or none)
      BASCUS-97328GAMEDATA  garage  icon.sys  icon0.ico
    BASCUS-97328R0001\ …           #   every replay (all kept here, unlimited)
    BASCUS-97328LSTRPLAY\ …        #   playlists etc. (game state, kept as-is)
    _meta.toml                     #   per-item: modes, created/modified timestamps, attributes, content hash, on_card flag
  saves\
    Save 02\BASCUS-97328GAMEDATA\ …   # inactive game saves, one folder per slot (ids only grow)
    Save 03\…
  runtime\card.ps2                 # the 8 MB image built for the current/last Play (+ card.ps2.prev backup)
```

Directory names inside `memcard\` and `saves\` are the real PS2 names, so a user can copy a save folder to or from a PCSX2 folder card by hand. The PS2 timestamps and modes are kept in `_meta.toml` because NTFS cannot store them exactly.

#### Play cycle

1. **Harvest first** (at launcher start, then after every game exit): read `runtime\card.ps2` with `gt4l-mcfs`.
   - Every directory on the card that differs from the folder (by content hash) is written into `memcard\` (temp dir + rename).
   - New replays become new items; the game save replaces the active one, after a copy of the old one goes to `memcard\.backup\`.
   - Nothing in the folder is deleted because it is missing from the card. A replay deleted in-game stays in the folder, flagged `on_card = false`.
2. **Switch active save** (Saves page radio): move `memcard\BASCUS-97328GAMEDATA` into its own `saves\Save NN\` and move the chosen slot's directory into `memcard\`. Same-volume renames, logged, never while the game runs.
3. **Build** (on Play): format a fresh card image (superblock, FAT, ECC) and write the active game save, the playlists, then the replays flagged `on_card`. If they do not fit, Play is blocked with one line asking to uncheck replays. The image is written to `card.ps2.tmp`, verified by reading it back, and renamed into place; the previous image is kept as `card.ps2.prev`.
4. **Play** with `--memcard runtime\card.ps2`. The runtime's exclusive `.lock` prevents a second writer.

Never lose a save or replay:
- harvest always runs before any build;
- the folder is changed only through renames;
- each previous card image and each replaced game save are kept as backups;
- user deletes go to the Recycle Bin.

### 13.2b Replay directories (study, round 5)

Method: static reading of the pinned reconstructed ELF (read-only), using a throwaway string/xref scanner and a tiny MIPS disassembler in the scratchpad (vaddr = file offset + 0xFF000). Class names from gt4-decomp. The owner's cards hold no replay yet.

| Finding | Evidence | Label |
|---|---|---|
| GT4's card layer `GT4MC` has distinct file kinds: `FileGT4GameData`, `FileGT4Replay` (`…Full`, `…Best`, `…Demo`), `FileGT4ReplayList`, `FileGT4PhotoData` (`…Film`, `…Picture`), `FileGT4PlayList`, `FileGT4SlideShowList`, `FileGT4PatchData`. Menu modes include `MODE_SAVE_FULL_REPLAY`, `MODE_SAVE_BESTLAP_REPLAY`, `MODE_LOAD_*_REPLAY`, `MODE_SAVE_PHOTO_FILM/PICTURE`. | Type strings, ELF 0x595f90–0x5962a8, 0x58de48–0x592390 | Confirmed |
| Game data directory = product code + `"GAMEDATA"`: `FileGT4GameData::setupFilename` at 0x1cda48 builds `BASCUS-97328` then appends the string at 0x694e20. | Disassembly | Confirmed |
| Replay type letters: full `'R'`, best lap `'B'` (virtuals at 0x1d0358 / 0x1d03e0). Icons `GT4_Rep_to_race.ico`, `GT4_Rep_to_license.ico`, `GT4_Rep_tt1.ico`; titles "FileReplayFull" / "FileReplayBest" / "FileReplayLicense". | Disassembly; pointer table 0x618d28 | Confirmed |
| Replay directory names follow `BASCUS-97328` + type letter + 4 digits (e.g. `BASCUS-97328R0001`). This mirrors the photo names, whose `F%04d` (film) and `P%04d` (picture) format calls are Confirmed at 0x1d06d8 / 0x1d0b6c. | Analogy | Hypothesis (confirm with one owner replay save) |
| Playlists use the fixed suffixes `LSTRPLAY` and `LSTSLIDE`. Every replay/photo directory has `icon.sys`, `icon0.ico` and `thumbnail`. | Strings 0x6951c8/0x6951f8, 0x595e80, 0x596318 | High confidence |
| The in-game load menus list the card's root directory (`"/%s/*"`, `GT4MC::Device::getdir`) and recognise GT4 entries by name. So any correctly named directory on the slot-1 card appears in game. | Strings, class structure | High confidence |
| GT4 also knows the GT3 and GT4 Prologue save names (`BASCUS-97102GAMEDATA`, `BESCES-50294GAMEDATA`, `BISCPS-15009GAMEDATA`, …), probably for old-game bonuses. They are ignored by the launcher. | Strings 0x596450–0x5964d8 | Confirmed (strings) |
| Replay size, and so how many replays fit next to the 1.5 MB save in about 6.6 MB free (roughly 4–20 at 0.3–1.5 MB each). | Not measured | Unknown / Hypothesis |

### 13.3 Operations

**Saves page (game saves only)**

- **List:** at least 10 rows always visible, used slots first, then empty rows; scrolls with no upper limit.
  - A filled row shows a radio, the number, "Save NN" (plus a "Next play" tag when active), the saved date and time (the PS2 directory `modified` timestamp), and two icon buttons: Export (arrow out of a tray) and Delete (trash).
  - The icons are always visible on filled rows, dimmed to 40% at rest; each goes to full brightness on hover or focus, and Delete turns red.
  - Empty rows show "Empty" and no icons.
- **Select (radio):** switches the active game save (13.2, step 2). Only one is active; the active save is the one inside `memcard\`.
- **Import save:** one picker accepts `.ps2 .mcd .bin` card images, PCSX2 folder cards (a folder), `.psu .max .cbs .xps .sps`, or a raw save folder.
  - A single save is imported directly.
  - A card image or folder card is opened read-only and its entries are listed. GT4 game data can be ticked; replays are greyed with "Import it from Replays"; other games' saves are greyed with "Not a Gran Turismo 4 save".
  - Each ticked game save **becomes a new slot** (`saves\Save NN\`). Import never overwrites, and the active save does not change.
- **Export:** a single save (`.psu`), or a PS2 card image (`.ps2`) built with that save plus all on-card replays.
- **Delete:** confirm, then the folder goes to the Recycle Bin. Deleting the active save leaves no active save: the next Play starts with no game data, and GT4 creates a new one.
- **First install:** "No saves yet / Your first save appears here after you save in the game." The Import button stays in the header. After the first Play, harvest finds `BASCUS-97328GAMEDATA` on the card, and slot 01 appears as "Save 01", active, with its date.
- **Owner migration (dev builds only):** a one-time import of `private/manual-play/card.ps2`.

**Replays page (replays only; filtered by directory type `R`/`B`)**

- **Same list:** "Replay NN", date and time, the same dimmed export/trash icons, 10 visible rows plus scrolling, and an "Import replay" button.
- **Checkbox instead of radio:** "On card" means the replay is written onto the game's 8 MB card for the next Play. All replays stay in `memcard\` regardless. New replays harvested after a session are checked automatically. One small line shows the card space used by checked replays.
- **Import replay:** same picker. Replays are listed and ticked; game data is greyed with "Import it from Saves". Import always adds new items and deduplicates by content hash.
- **Export** `.psu`; **Delete** confirm, then the Recycle Bin. A replay removed in-game stays in the folder, unchecked.
- **First install:** "No replays yet / Replays you save in the game appear here."

### 13.4 Crates

- `gt4l-mcfs`: PS2 card filesystem reader and writer with ECC; the card-image builder and harvester; importers for psu/max/cbs/xps/sps, folder cards and raw directories; psu export.
- `gt4l-memcard`: the folder model (`memcard\`, `saves\`, `_meta.toml`), the switch/build/harvest cycle, and the backups.
- `gt4l-gt4save` (later, not v1): port of the PDTools GT4 decryption, for a read-only summary (day, credits).

All three are UI-free, so Android can reuse them.

Fixtures:
- synthetic cards from our own writer;
- round-trip tests (folder → card → folder must be byte- and timestamp-identical);
- crash tests: kill between any two steps, then the next harvest recovers with no loss;
- private parity tests on the owner's card (read-only).

### 13.5 Mockups

`mockup-settings.html` states:
- **Saves**: list, first run, after the first save, import dialog;
- **Replays**: list with "On card" checkboxes, empty, import dialog.

Renders in `shots\`:
- `settings-saves.png`
- `settings-saves-first-run.png`
- `settings-saves-after-first-save.png`
- `settings-saves-import.png`
- `settings-replays.png`
- `settings-replays-empty.png`
- `settings-replays-import.png`

Dates and items shown are sample values.

### 13.6 Effort (TASK-025)

| Part | Agent-days |
|---|---|
| `gt4l-mcfs` reader/writer + ECC + card builder/harvester + fixtures | 4–5 |
| Folder model, switch/build/harvest cycle, backups, crash tests | 3–4 |
| Importers: card images (with/without ECC), raw dir, PCSX2 folder card, `.psu` | 2 |
| Importers: `.max` (LZARI), `.cbs` (RC4 + zlib), `.xps`/`.sps` | 2–3 |
| Confirm replay directory names and sizes from one owner replay save | 0.5 |
| Runtime overlay: atomic flush + flush on exit + CTest fixture | 1 |
| Saves + Replays pages and import dialogs (Slint, shared components) | 2 |
| **v1 total** | **≈ 14.5–17.5** |
| v1 cut without `.max`/`.cbs`/`.xps` | ≈ 12–14.5 |
| Later: decrypted day/credits summary | +3–4 |
| Not recommended: runtime folder card (a1) / MCMAN HLE (a2) | 10–15 / 15–25 |

## 14. Implementation status (branch `task/019-launcher`, 2026-10-15)

Not yet owner-approved; not merged. Labels as above. Paused by the owner on 2026-10-15; the Slint UI
below is superseded by section 15 (Tauri 2 with the HTML design).

Done (Confirmed by tests or by the screenshots in `docs/launcher/shots/`):
- Branch merges: TASK-029 controls table (`docs/CONTROLS.md`), TASK-025 `gt4saves` and `memcard_store`,
  origin/main (MTVU, slice 178). The launcher workspace has all five crates.
- Saves and Replays pages use `gt4saves::library` (`launcher/crates/gt4l-app/src/saves.rs`): list, import
  (never overwrites), make active, delete to the Recycle Bin (`recycle.rs`, SHFileOperationW with
  FOF_ALLOWUNDO), card prepare before each start (a card left by the game is harvested first), harvest
  after the game exits. The library lives in the data folder under `library`. Tests build synthetic cards.
- Controls: "Press a control" captures XInput buttons and triggers (`pad.rs`, XInputGetState, baseline
  taken at capture start) and keys. Stick capture is implemented and tested but has no Controls-page row.
- `gt4controls` follows `docs/CONTROLS.md`: the mirrored tables stay, and `tests/docs_match.rs` fails when
  they or `docs/controls/*.toml` drift from the document.
- Settings show only options the game reads. CoreGS 1.1 rows: Internal resolution (`PS2_SCALE` 1-3),
  Scaling filter (`GT4_PRESENT_FILTER` automatic|nearest|linear|fsr; automatic sends nothing),
  FSR sharpness (`GT4_FSR_SHARPNESS`, only with FSR, default 0.2), Deinterlacing (`GT4_DEINTERLACE`,
  default auto), Anti-aliasing (`GT4_AA`, default off). Names and values come from task/028
  `docs/BUILD.md` slice 182 and `native/gpu_init.inc`; they exist only on that branch until it merges.
  paraLLEl-GS: super-sampling and high-resolution output only. CoreGS 2.0: one greyed entry. Display
  shows fullscreen and VSync only; the unwired overscan and offset rows were removed.
- Release blockers: the shader folder is `<game folder>\shaders` when present, else the build-time path
  (`native/native_vulkan.cpp`, not compiled here: no private assets or Vulkan headers in this worktree);
  the launcher joins a kill-on-close Job Object (`process.rs`), so the game ends with it; a second launcher
  brings the first window forward (window title `GT4Recomp`, not the game's title).
- Cover: `launcher/assets/brand/source/cover-front.jpeg` is the owner's 1000x1500 image; `make_brand.py`
  derives `cover-front.png` from it. The cover is not drawn in the UI yet.

Open:
- Stick rebinding has no Controls-page row (the page lists the 16 buttons only).
- Replays: every replay goes on the card at each start; the "On card" choice from the mockup is not built.
  A full card stops the start with a message.
- Save export and the `.max`/`.cbs`/`.xps` importers are not built.
- The Recycle Bin call and the delete path were not run on a real item (tests cover the refusal rules only).
- Home and the other pages need prepared game files; the screenshots were taken with the
  `visual-check` cargo feature (`GT4RECOMP_VISUAL_SCREEN`, `GT4RECOMP_VISUAL_PAGE`), which is not in release builds.
- The AMD Radeon metrics overlay (`amdow.exe`) is drawn into some captures (setup, home).
- The native shader change needs a candidate build and the launcher manifest pin; not built here.
- The Setup page still shows the "Cover art" placeholder.

## 15. Frontend decision: Tauri 2 with the approved HTML design (owner, 2026-10-15)

State: **paused**, documentation only. Nothing is built for this section until the owner resumes the launcher,
which is planned as the last release 1.0.0 workstream.

Why: the Slint screens (section 14, `docs/launcher/shots/`) drifted from the design the owner approved over
rounds 2-6. That design is already HTML: `mockup-setup.html`, `mockup-home.html` and
`mockup-settings.html` (Graphics, Display, Advanced, Saves, Replays, About), with the section 12 tokens.
Tauri 2 renders HTML in the system WebView2, so the approved pages become the real UI instead of being
redrawn.

What stays (Rust, unchanged, with their tests):
- `gt4l-core`: settings table, config TOML, launch line and environment, exit classification, process lock,
  Job Object, single instance.
- `gt4l-disc`: ISO check, CORE.GT4 extraction, ELF rebuild, setup pipeline.
- `gt4saves`: card library, import, harvest, build card.
- `gt4controls`: the binding schema of `docs/CONTROLS.md`.
- XInput capture (`pad.rs`) and Recycle Bin deletes (`recycle.rs`), moved out of `gt4l-app` into a
  UI-free crate.

What changes:
- `gt4l-app` (Slint) is replaced by a Tauri 2 app crate, `gt4l-tauri`. Its frontend is the mockup HTML,
  split into one page per screen, with no framework beyond plain HTML, CSS and JS unless the owner asks.
- The UI calls Rust through Tauri commands only: setup progress and cancel, settings get and set from the
  settings table, play, saves and replays operations, controls capture.
- Settings rows are generated from the `gt4l-core` settings table. The page never hard-codes an option,
  so only real options appear (the owner rule).
- Tauri security: a strict CSP, no remote content, all assets bundled, and capabilities limited to the
  commands above. There is no shell plugin and no arbitrary file system access from the page.
- WebView2: Windows 11 ships it. For older systems the NSIS installer uses Tauri's
  `embedBootstrapper` or `downloadBootstrapper` mode; that choice is left open for the owner.
  The installer stays per-user NSIS (Tauri's NSIS bundler, or the existing `installer/gt4recomp.nsi`).
- Licences: Tauri and WebView2 are MIT/Apache-2.0 and Microsoft-redistributable. The Slint attribution
  goes away with Slint.

Settings to add to the HTML design when it resumes:
- FSR sharpness as a 0-100 % slider, default 50 %, shown only when the scaling filter is FSR. It maps to
  `GT4_FSR_SHARPNESS` 0-100 (task/028 slice 184).
- Controls page: an original vector DualShock 2 drawing, with the PS2 names and the current binding on
  each control. The Slint generic pad is not the design.

Effort estimate (Hypothesis): port the three mockups into Tauri pages and wire the commands in about 2-3
agent sessions. The Rust core needs no rewrite.

### 15.1 Status (2026-10-16, branch `task/019-launcher`, Haiku)

Resumed for TASK-043 (decision 0070). Done in this branch:
- Renderers: PCSX2 GS (default when installed), CoreGS 1.1 (upscale, FSR, FXAA and deinterlace rows hidden),
  CoreGS 2.0 ("coming soon"). paraLLEl-GS and its `--hires` row are removed.
- Settings: `docs/launcher/gs-settings.toml` is compiled into `gt4l-core` (`gs_table`); `page` and `control`
  columns are read with the fallbacks (advanced, dropdown); `page = mods` rows are never offered in 1.0.0 and
  get their defaults only. `gt4-gs.ini` is written by the same rules as `scripts/gs_settings.py`; golden test
  `gs_golden.rs` compares bytes (CRLF on Windows, as the Python text write).
- Reset: `reset_gs_settings_to_defaults`.
- Download (TASK-043): `gt4l-core::gs_release`, stable releases of `moesuito/gt4recomp-gs`, minimum 0.3.2,
  `SHA256SUMS` check, `%LOCALAPPDATA%\GT4Recomp\gs\<version>\`, previous version kept, fallback to CoreGS 1.1.
  Started from `setup_start`.
- Launch: `GT4_RENDERER=pcsx2`, `GT4_GS_PROCESS` and `GT4_GS_PROCESS_SHA256`, `GT4_GS_CONFIG` (per session),
  `GT4_FORCE_480P=1` by default; readback accurate by default (INI `HWDownloadMode`). On the restart marker
  (`[gs-ipc] restart requested: renderer donor (CoreGS 1.1)`) the game starts again with CoreGS 1.1.
- WebView2: `downloadBootstrapper` (owner choice); NSIS per-user (`installMode: currentUser`).
- Workspace: `gt4l-tauri` is a workspace member. Tests: `cargo test --workspace` green.

Open: the UI merge (`task/019-launcher-ui`, not edited here); `ui/dev/` is still inside `frontendDist` (Tauri
has no exclude; needs a staging copy step); gamepad event; setup progress events (`app_state` reports progress 0);
launch environment golden against the Python probe (the INI is golden, the env is not yet); live GitHub download;
the PCSX2 GS update offer on startup; `gt4l-app` (the old Slint crate, not in the workspace) still calls the old
`game::start` signature.

### 15.2 Command list (contract with `task/019-launcher-ui`, `ui/dev/README.md`)

Registered in `src/main.rs`; the test `src/contract.rs` fails when this list and the registration differ.

- App and setup: `app_state`, `setup_start`, `setup_choose_disc`, `setup_cancel`, `play`, `stop_game`
- Settings: `get_settings_table` -> `{max_upscale, rows}`, `set_setting({id, value})`, `reset_gs_settings_to_defaults`
- Controls: `get_controls` -> `{controls, triggers}`, `capture_binding({control})`, `cancel_capture`, `reset_binding({control})`
- Saves: `saves_list`, `saves_import`, `saves_activate({id})`, `saves_delete({id})`
- Replays: `replays_list`, `replays_import`, `replays_delete({id})`
- About: `about_info`, `open_log_folder`

## 16. Steam and Big Picture: launch straight into a game (TASK-211, rc17)

Owner request, 2026-11-09: a player wants the couch setup, adds GT4Recomp to Steam, starts it from Big Picture with a
controller and comes back to Steam when the game closes, without the launcher window. See
`docs/tasks/RC17-COUCH-AND-INGAME.md` (TASK-211); the in-game Quit Game and the in-game settings are TASK-212 and
TASK-213.

### 16.1 The command line

`GT4Recomp.exe` reads two arguments before any window exists (`launcher/crates/gt4l-tauri/src/launch_flags.rs`):

- `--skip-launcher`: the launcher runs its normal start-up with the window hidden. It writes the same settings, the same
  logs and the same "game played last" state the page would, starts the game through the **same Play path** as the Play
  button (`commands::start_play`, shared by both callers), stays alive while the game runs and **ends when the game
  process ends**, so the front end gets the process back.
- `--game=retail|spec2`: the game to start (`--game retail` works too; `gt4` is the alias of `retail`, and case does not
  matter). `--game` without `--skip-launcher` changes nothing: the window opens as always.
  - Without `--game`, a direct launch uses the game played last (`[game] selected` in `settings.toml`, the same value the
    home screen shows) and says so in the log. Before any game was played it uses the launcher's start selection.
  - A value that names no game, or `--game` with no value, is an error the launcher window shows
    ("`{value}` is not a game of this launcher: use --game=retail or --game=spec2."), so a typo in the launch options is
    never a silent exit.

The flags are exact (`--skip-launcher`, not `--skip-launcher=1`). A safe-mode WebView2 restart (TASK-091) repeats them, so
a page that has to draw again with GPU acceleration off still starts the game it was asked for.

### 16.2 The hidden launch

- The window is created but stays hidden: `ui_ready` marks the page ready without showing it. The page is still needed
  (WebView2 is required, as for a normal start).
- The direct launch waits for the 1.2.0 large-data move (Play and setup wait for the same point) and logs it if it takes
  time; after 5 minutes it shows the launcher window with the "moving game files" message instead of starting.
- It then resolves the game, selects it exactly as the home page does (`commands::select_game_of`), and calls the Play
  path. The tray icon exists for the session, as it does during a normal Play (it is the launcher's own icon; the player
  does not have to touch it).
- While the game runs the launcher is a hidden process in the tray. Steam (or any front end) therefore sees the game
  running for the whole session.
- When the game ends: the session log and the card harvest finish, then the launcher calls `app.exit(0)`. It never leaves
  a hidden launcher behind.
- The shader prepare the Play button runs before Play (CoreGS 2.0 only) is skipped: it has its own progress window, and
  the game compiles what it needs while it runs (TASK-204 records the variants too). The log says so.
- The update check still runs in the background; an update offer waits for the next normal start of the launcher (the
  page is hidden, so nothing is shown).

### 16.3 Errors

- The game is not ready (no disc, no install, a check fails, the writer or renderer is missing): the launcher window
  opens with the usual message (the same `game-state` path a game that ends with a message uses), and the player can fix
  it and press Play. The launcher then stays open after that Play, as it does today.
- A game is already running: the launch is refused and logged; the process ends without showing anything (the player is
  in a game already).
- WebView2 missing: the native message of TASK-091, before any window, as for a normal start.
- A second launcher (the single-instance lock) still wins: starting a game from Steam while the launcher is already open
  does nothing (the running launcher is brought forward only when its window is visible). This is the existing rule, not
  a TASK-211 change.

### 16.4 Steam setup (to document for players)

1. Steam > Games > **Add a Non-Steam Game** > Browse > `GT4Recomp.exe` (the launcher, never `gt4.exe` or
   `gt4-spec2.exe`: the launcher owns the disc image, the memory card, the renderer, the shader prepare and the update
   checks).
2. Right-click the entry > Properties > **Launch Options**:
   - Gran Turismo 4 v2.00: `--skip-launcher --game=retail`
   - Gran Turismo 4 Spec II: `--skip-launcher --game=spec2`
3. Start it from Big Picture. The game starts without the launcher window; Steam shows the game as running until it
   closes and then returns to the game list.
4. Exit the game:
   - the in-game **Quit Game** (TASK-212) is the primary path once it lands;
   - Big Picture's **Exit Game** (Xbox/PS button > the running game > Exit Game) is the fallback: it ends the launcher,
     and the launcher's kill-on-close job then ends the game (the same rule the launcher has today);
   - a controller shortcut, if TASK-212 adds one, is the third path.
5. A second copy of the entry can point at the other game id, so both games appear in the library.
