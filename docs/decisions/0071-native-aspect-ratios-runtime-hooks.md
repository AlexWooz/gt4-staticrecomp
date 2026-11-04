# 0071 — Native aspect ratios through runtime hooks (supersedes part of 0055)

Date: 2026-10-21. Status: Accepted (owner, 2026-10-21: "proceed with the plan"; scope change in AGENTS.md, PR #52).
Supersedes, in [0055](0055-native-widescreen-video.md), only the sentence "No translated game patch, guessed
memory write or forced camera projection." The rest of 0055 stays as written. Design:
[ULTRAWIDE-NATIVE-STUDY](../ULTRAWIDE-NATIVE-STUDY.md) (TASK-080).

**Context.** 0055 relied on the game's own Screen setting (4:3 or 16:9) and refused any host change to the game's
camera. The owner now wants other display ratios (16:10, phones near 19.5:9, 21:9, 32:9) in this repository, without
replacing the GS renderer. The study found where the race camera gets its aspect: `RaceBase::initMonitor`
(0x00388A80) builds the monitor matrix on its stack and hands it to the camera through a virtual call that our
runtime dispatches (return address 0x00388B38). TASK-079 (slice211) already writes guest RAM at VBlank (the game's own
Wide setting), so the "guessed memory write" line no longer described the code either.

**Decision.**
- Native aspect ratios are implemented by the host: a hook on the dispatched camera call scales the projection
  matrix (Hor+ at R >= 16:9, Vert+ below; geometry-correct, an optional hidden factor `GT4_ASPECT_FOV_EXTRA` for the
  owner's comparison), and the renderers stretch the race view to the screen while menus, FMVs and photo mode stay
  pillarboxed 16:9. Later phases (P2 HUD, P3 progressive menus) may add an ELF patch table re-translated into a new
  game library; each such patch falls under the same guard rails.
- **Guard rails (all phases):**
  1. **No save-data writes.** No hook writes the option object, the monitor record or anything else the game saves.
     The camera hook writes only the 64-byte matrix copy on the guest stack. (TASK-079's hold of the game's own Wide
     setting is the one existing exception, kept and hardened: it writes only after the game-data pointer settled.)
  2. **16:9 stays byte-identical.** At R = 16:9 with factor 1.0 no hook writes anything and the presentation does not
     change; headless frame dumps must equal the previous slice.
  3. **Every hook and patched word is listed, with tests.** Each hook names its guest address and what it reads and
     writes (table below); each future ELF patch lists address, old word and new word, and CTest checks the old
     words against the pinned ELF.
  4. **SilentWarrior112's patch file has no licence.** It is a reference for facts only (addresses and what they
     do); its text, tables and values are never copied, and nothing is applied by pnach.
- A setting that the player cannot see must not change the game: every switch has a documented default that
  reproduces today's behaviour (`GT4_DISPLAY_ASPECT=16:9`, `GT4_ASPECT_FOV_EXTRA=1.0`).

**Hooks and words (P1, slice212b).**

| Item | Where | Reads | Writes | Test |
|---|---|---|---|---|
| Race camera aspect hook | dispatch, call with return address 0x00388B38 (jalr 0x00388B30 in `RaceBase::initMonitor`; also reached from photo pause `func_003C49C8`) | a1 = sp (the 4x4 matrix), the window client size | the 16 stack words at a1, only when the scale is not 1 | `native_display_aspect`, `tests/python/test_display_aspect.py` (overlay harness) |
| Race-view marker | dispatch of `RaceDisplay::render` 0x0039F498 (race HUD; only reached through vtables) | nothing (pc only) | nothing | `native_display_aspect` (scene policy), overlay harness |
| Photo/movie signal | guest VBlank | display request struct 0x006184D0 (+0x24 pending, +0x2C mode, +0x30 row; row 2 = photo and movies) | nothing | `native_display_aspect` (scene policy) |
| TASK-079 Wide hold (existing) | guest VBlank | owner pointer 0x00622F4C (settled), option + 0x14/+0x24/+0x1388, presets 0x622E90..0x622F20 | option + 0x14, option + 0x24 (11 words), 0x006184F0 | `native_race_widescreen` |

**Hooks and words (P2, slice217).** Owner design: the 3D fills the screen and the race HUD stays the game's 16:9
layout, undistorted and centred ("HUD centred 16:9"). The game draws the HUD into the same 640-pixel frame after the
3D, so the stretch of the frame to R is undone for the HUD only. Runtime hook where a dispatch point exists; an ELF
patch word where the constant is an immediate behind direct calls only (the translator compiles `lui`/`addiu`
immediates into C++ constants, and after the dispatched entry of `RaceDisplay::render` every call is a direct `jal`).

| Item | Where | Reads | Writes | Test |
|---|---|---|---|---|
| HUD canvas hook | dispatch of `RaceDisplay::render` 0x0039F498 (also `RaceLicenseDisplay`, vtable 0x680D00), a0 = this | the 16 words of the HUD ortho matrix at this + 0x70 (`RaceDisplay::setProjectionMatrix` 0x0039BB38 layout), the ratio of the last camera hook | m[0], m[5], m[12], m[13] of that matrix, only when it is the game's full-screen 16:9 canvas (left -106.667, right 746.667, top 480, bottom 0) or a canvas this hook wrote: R >= 16:9 left/right = 320 -/+ 240 R; R < 16:9 top/bottom = 240 +/- 240 (16/9)/R. At 16:9 the game's own words are restored exactly (nothing is written while R stays 16:9). Split-screen canvases (`RaceSplitDisplayBase::initialize`) and the 4:3 game canvas are never written | `native_display_aspect` (`test_hud_canvas`), overlay harness (`test_render_hook_*`) |
| HUD parameter block | guest VBlank (host) | nothing | 0x00007F00..0x00007F07 in guest kernel RAM that neither the game nor our HLE kernel uses: +0x00 f32 race font x scale, +0x04 s32 mirror divisor. 16:9 (and before the first camera hook): 0x3F400000 (0.75) and 640, the original constants; other R: (4/3)/R and round(640 R/(16/9)) for R > 16:9 (640 below). This is the one write made at 16:9: it is needed because the patched code loads these words, and it reproduces the original values exactly | `native_display_aspect` (`test_hud_parameters`), `tests/python/test_aspect_elf_patch.py` |
| ELF word 0x0039F554 | `RaceDisplay::render`: race HUD font x scale in wide mode (stored by `func_0044E390` into the race font object `D_00623838` + 0x18) | | old 0x3C013F40 `lui at,0x3F40` (0.75) -> new 0x8C017F00 `lw at,0x7F00(zero)` | `tests/python/test_aspect_elf_patch.py` (old word in the pinned ELF, table, header) |
| ELF word 0x0033EF98 | rear-view mirror placement after the `jal` at 0x0033EF70 (RacePS2Base render path): the mirror is 260 x width / divisor frame pixels wide, centred | | old 0x24080280 `addiu t0,zero,640` -> new 0x8C087F04 `lw t0,0x7F04(zero)` | same |

The ELF words live in `scripts/aspect_elf_patch.py` (the only copy of the table besides this page). The re-translated
library is profile `aspect` (`docs/inputs/slice217-ee-libraries.json`): the slice208 coverage recipe (accepted seed set,
translator, clang-cl 23.1.2) on the patched derivative of the pinned ELF; the translator's analysis is unchanged (138,813
entries) and only `unit_008F` and `unit_00A7` differ from slice208, each by the one patched line.

**Spec II (TASK-166, 2026-11-04).** The Spec II port of the hooks (TASK-100 S8) had the canvas hook but not these two words,
so at a ratio wider than 16:9 the race-font glyphs (the digital speedometer and odometer of the gauges) and the mirror kept
their 16:9 width in frame pixels and were stretched with the frame. The same two instructions, patched to the same words
and the same block (`scripts/spec2_aspect_patch.py`, `tests/python/test_spec2_aspect_patch.py`):

| ELF word (Spec II) | v2.00 twin | Old -> new |
|---|---|---|
| 0x001C519C, `RaceDisplay::render` 0x001C5040: the race font x scale in wide mode, multiplied by the font's own x scale at this + 0xB0 and set on the race font 0x00665048 through 0x0031EB78 | 0x0039F554 | 0x3C013F40 `lui at,0x3F40` -> 0x8C017F00 `lw at,0x7F00(zero)` |
| 0x001EB65C, the mirror placement after the `jal` at 0x001EB634 (the same sequence as v2.00's 0x0033EF78..0x0033EF98) | 0x0033EF98 | 0x24080280 `addiu t0,zero,640` -> 0x8C087F04 `lw t0,0x7F04(zero)` |

Translated with the pinned translator and the pinned S8c entry seeds (`private/slice405-spec2-aspect-producer`), the units
are byte-identical to the pinned Spec II translation (`docs/inputs/spec2-ee-generated.json`) except `unit_0031` and
`unit_003A`, one line each. The host already fills the block for both targets at every guest VBlank, with the original
constants at 16:9.

**2D overlays over the race view (TASK-095, 2026-11-04; `docs/ULTRAWIDE-OVERLAYS.md`).** The pre-race screens (the menu
context) and the pause menu, pit menu and results (strobe movies, one renderer) are drawn over the race; each sets its
0..640 x 480..0 canvas through a direct call of the ortho function, and the strobe text is sized by the text style parser.
Re-does PR #177 (Luan Antony), whose canvas maths had no call site. The race monitor is left as the game draws it.

| Item | Where | Reads | Writes | Test |
|---|---|---|---|---|
| Overlay canvas hook | translated-call hook (`scripts/camera360_units.py`) between the delay slot and `jal <ortho>`: v2.00 returns 0x002151A8 (menu context, `mUpdateContextPS2` render 0x00214FB0) and 0x003EC200 (strobe renderer `func_003EC0B0`), ortho 0x004A5A90; Spec II 0x0036D4D8 and 0x001FDE18, ortho 0x005A2600 | f12..f15 (left, right, bottom, top), ra, the renderer's caller at sp + 0x60 (the race monitor's call, v2.00 return 0x003DA294 / Spec II 0x001F2FD4, is left alone), the presented state and ratio | f12..f15 only, only while the presentation fills the screen at R != 16:9 and only for the game's canvas (left 0, top 0, bottom 480, right 640): the x extent times R / (16/9) around its centre (R > 16:9) or the y extent times (16/9) / R (R < 16:9) | `native_display_aspect` (`test_overlay_canvas`), `native_game_target`, `tests/python/test_camera360_units.py`, `scripts/coregs2_bench/overlay-aspect-check.sh` |
| Strobe text window | opened by the hook above when it widened the strobe renderer's canvas; closed by a translated-call hook before the renderer's first call after its draw (v2.00 return 0x003EC248, Spec II 0x001FDE60) | nothing | nothing (host state) | same |
| Text style parser stores | translated statements (`scripts/camera360_units.py`): the parser's four stores of the text x and y scale (v2.00 0x004902C8, 0x004902CC, 0x0049038C, 0x00490390 into context + 0x24/+0x28; Spec II 0x0033AFC8, 0x0033AFCC, 0x0033B08C, 0x0033B090 into + 0x3C/+0x40) go through `overlay_text_scale_x` / `_y` | the stored value | the game's own store of that value, times the inverse canvas factor on the same axis while the window is open, unchanged otherwise | same |

Not changed in P2 (and why): the ±96 edge offsets (`RaceDisplayUtil::wideOffset`, they keep the HUD a centred 16:9
block, the owner's default); the race monitor bar width 854 (`func_003D8518`, centred in the canvas); the menu/PGL text
cave and the loading-screen orthos (menus, loading screens, FMVs and photo mode stay pillarboxed 16:9);
`func_00107328` and `func_00393790` (result and menu paths, pillarboxed); the HUD safe-zone w and height. Split screen
keeps its own canvases (left alone) and a `RaceDisplay::render` with a split-screen canvas is not counted as the race
view, so split screen stays pillarboxed 16:9 (its 3D under the camera hook is not verified: owner test).

**Presentation of a custom ratio (P2).** A ratio other than the window's is a centred band of that ratio (letterbox when
wider, pillarbox when narrower), never stretched to the window: CoreGS 1.1 `presentation_rect_ratio` (unchanged since
P1); for gt4-gs, protocol 1.3 `display_aspect` fill now carries the target aspect x 1e6 in `args[1]` (0 = the whole
window). gt4-gs 1.1.0 ignores `args[1]` and fills the whole window, which is what the owner saw on 2026-10-21
(Custom 2.44 on 1920x1080, race stretched to 16:9); a gt4-gs release that reads it is needed (fork work).

**Addendum (owner, 2026-10-21): seamless display, Auto only (slice218).** The owner removed every aspect and
internal-resolution choice from the launcher:

- **No Aspect ratio option.** The aspect is always the output size's shape: the launcher sends
  `GT4_DISPLAY_ASPECT=auto` with `GT4_OUTPUT_SIZE=WxH`, and the game takes R = W / H (snapped to 16:9 within 0.5%).
  At 16:9 nothing changes (guard rail 2 still holds: a 1920x1080 output is byte-identical to slice217); at any other
  ratio the P1/P2 path above turns on by itself. Custom ratios are gone, so the output aspect always equals the window
  in fullscreen on a monitor whose desktop has the same shape; the protocol 1.3 `args[1]` fill target stays in the
  protocol and is sent as before, but no gt4-gs change is requested. CoreGS 1.1 is being retired: it gets no new
  feature (it keeps its own internal resolution and the auto aspect it already had).
- **Display page: Monitor and Resolution.** Monitor lists the connected monitors (EnumDisplayMonitors, GetMonitorInfo,
  friendly names from DisplayConfig), default the primary one, stored as `<GDI device>@<W>x<H>`; Resolution lists the
  modes that monitor reports (EnumDisplaySettings, unique sizes, at least 640x480, largest first), default its desktop
  mode. The game gets `GT4_MONITOR` (device name) and `GT4_OUTPUT_SIZE`; invalid values refuse the start like
  `GT4_FULLSCREEN`, and a well-formed monitor that is no longer connected falls back to the primary monitor with a log
  line. Fullscreen is always borderless on the chosen monitor (there never was an exclusive mode; the Fullscreen toggle
  and Alt+Enter/F11 stay, owner decisions of TASK-009/038 and settings schema 2). Windowed, the client area is the
  output size (shrunk with its shape to the work area), centred on the monitor. The game declares per-monitor DPI
  awareness (v2) before its window exists, so sizes are physical pixels on scaled desktops.
- **Internal resolution is automatic.** k = ceil(max(W / 640, H / 448)) (640 x 448 is GT4's frame; wide screens stretch
  its width), so the render is never below the output on either axis; the **Render scale** slider (50% to 200% in steps
  of 25%, default 100%) multiplies it: the multiplier is k x scale rounded to a whole number (ties up) and clamped to
  1..12, what gt4-gs accepts (`upscale_multiplier` 1 to 12, the allow-list; gt4-gs itself lowers a value above the
  GPU's limit, max texture size / 1280). The rule is the pure `gt4l_core::display::display_plan` (mobile-ready).
  A resolution below the desktop mode renders at that plan and the picture is scaled to fill the monitor by the
  presenter (one scale step from the internal frame to the screen; the swapchain stays the window's size).
- The `upscale` row of `docs/launcher/gs-settings.toml` is now `page = hidden` and written by the launcher from the plan;
  its choices (1 to 12) and the gt4-gs allow-list are unchanged.

**Consequences.** The P1 build changes only runtime code (a new license guard capsule `slice212c`, used by the slice212b game build, native host code,
pins); the game library is unchanged. A save made with any ratio is a plain save. PCSX2 GS presentation per scene
needs protocol 1.3 (`CONTROL display_aspect`) in gt4-gs; a 1.2 consumer keeps the 16:9 picture. P2/P3 ELF patches
need their own slices and pins.
