# TASK-095: 2D overlays over the race view at ratios other than 16:9

Date: 2026-11-04. Branch `task/095-ultrawide-menu-canvas` (from `task/134-game-code-optimization-study` at `bb4c7ed7`). Re-does
PR #177 by Luan Antony, which added the canvas maths (`menu_canvas`, `MenuCanvasHook`, `on_menu_overlay_matrix`) but no call
site: nothing in the game reached it, and these screens keep no canvas matrix in memory for a hook to rewrite. This page
records where the game sets the canvas of these overlays and the hooks that now reach it. Guard rails of decision 0071: 16:9
byte-identical, no save-data writes, every hook listed with tests.

Labels: **Confirmed** (read in the translated units or the ELF, or measured in a headless run), **High confidence**,
**Hypothesis**, **Unknown**. Names come from gt4-decomp at `3b1f2ae5b` (`private/references/gt4-decomp`, names only, no code
copied); unnamed functions are given by address.

## 1. The problem

TASK-080 fills the screen with the race view: the game's 640-pixel frame is stretched to the screen's ratio R, the race camera is
widened (Hor+) and the race HUD is drawn into a centred 16:9 box (P2). Every other 2D layer drawn over the race in that frame is
stretched with it by s = R / (16/9): the owner's screenshot at 3440x1440 shows the pre-race screen of a Time Trial (car name,
power and weight table, session record, icon row) 1.31 times too wide. The pause menu, the pit menu and the results are drawn
over the race the same way.

## 2. Where the canvas is set (Confirmed: v2.00 slice217 units, Spec II slice405 units, headless runs)

Each of these screens sets its 2D canvas right before it draws, through the ortho function (glOrtho-like: f12 left, f13 right,
f14 bottom, f15 top, f16 near, f17 far; it builds the 4x4 matrix on its own stack and multiplies it into the current matrix):

| | v2.00 | Spec II (matched by code shape) |
|---|---|---|
| ortho function | `0x004A5A90` (`unit_00E9` entry 94) | `0x005A2600` (`unit_0128` entry 135) |
| menu context render: the pre-race screens (the event's entry list and icon row; the Time Trial lobby with its session record) | `mUpdateContextPS2` render `0x00214FB0` (a virtual): `jal` at `0x002151A0`, return `0x002151A8`; 0..640 x 480..0, near 0, far 100 | `0x0036D278`: `jal` at `0x0036D4D0`, return `0x0036D4D8`; width and height from the words `0x006661D0`/`0x006661D4` (640 and 480 in the runs), near 0, far 100 |
| strobe overlay renderer: the pause menu, the pit menu, the results, photo mode's panel, the race monitor's movie | `func_003EC0B0`: `jal` at `0x003EC1F8`, return `0x003EC200`; 0..640 x 480..0, far 1.0 | `0x001FDCC8`: `jal` at `0x001FDE10`, return `0x001FDE18`; the same canvas |
| the renderer's callers | `RacePause::render` (`0x003C39C0`), `PhotoPause::update` (`0x003C4F2C`), the race monitor `func_003DA150` (`0x003DA294`), `PitMenu::callFunction` (`0x003DD6C8`), `ResultArcade::render` (`0x003DFCB8`) | `0x001E048C`, `0x001F2FD4` (the race monitor), `0x00288658`, `0x0028A510`, `0x0028E128` |
| text style parser (the strobe text's size) | `0x00490290`: stores 1.0 into the text context's x and y scale (`0x004902C8`, `0x004902CC`; context + 0x24, + 0x28), then the style's percentages (`0x0049038C` y, `0x00490390` x) | `0x0033AF90`: the same (`0x0033AFC8`, `0x0033AFCC`; + 0x3C, + 0x40; `0x0033B08C`, `0x0033B090`) |

All the ortho calls are direct `jal`s with immediate (or global) arguments: no dispatch point lies between the arguments and the
call, and the matrix lives only on the callee's stack. A dispatch hook cannot reach them; the canvas has to change in the
argument registers.

The strobe overlays' text does not follow the canvas (Confirmed, frames and a scratch trace build): the strobe player maps each
text field's box through the canvas, but sizes the glyphs in frame pixels by the text context's x and y scale, which the style
parser stores for every text (traced: 0.8 and 0.97 in the pause menu, the context at `*0x00624980`). With only the canvas widened
the pause buttons were narrowed and their text was not ("Screen Settings" overflowed its button and was clipped by it). The menu
context's text follows its canvas (frames).

Not touched, on purpose:

- the race monitor (`RaceMonitor`, drawn from `RaceBasic::render_panel`; v2.00 `func_003DA150`, Spec II `0x001F2E98`): after its
  strobe movie it sets a canvas of its own (0..853.333 when the game is set to 16:9) and draws items with the race font, whose
  x scale it sets itself (0.75 in wide mode); narrowing those needs one more constant per target (like the P2 patch table), so
  the race monitor stays exactly as the game draws it, its movie included (the hook checks the renderer's caller, which the
  renderer keeps at sp + 0x60). Not seen in a run; a follow-up if the owner wants it.
- the other callers of the ortho function draw into off-screen buffers (the race meters `RaceMTR*::render_buffer`,
  `CarIconMaker::render`, the mini-map and lap-time panels), set a centred full-frame canvas (`GameObjectPS2::internal_render`,
  -320..320 x -224..224: full-screen layers that must keep covering the whole screen) or the photo mode frame
  (`PhotoPause::update`'s own orthos; photo mode is pillarboxed on v2.00).
- `SimplePause::render` (`0x003C3B08`) draws text through the style parser outside the strobe renderer; not seen in a run
  (Unknown which mode uses it).
- The race HUD keeps its own P2 hook (`RaceDisplay::render`, matrix at this + 0x70).

## 3. The hooks

- `scripts/camera360_units.py` (with the camera calls of TASK-159) adds:
  - `::gt4::display_aspect::before_overlay_ortho_call(c);` between the delay slot and the call at the two ortho calls of each
    target (v2.00 `unit_0045`, `unit_00BB`; Spec II `unit_009B`, `unit_003F`);
  - `::gt4::display_aspect::after_overlay_draw(c);` before the strobe renderer's first call after its draw (v2.00 `jal 0x004A5400`
    at `0x003EC240`, return `0x003EC248`; Spec II `jal 0x005A1E08` at `0x001FDE58`, return `0x001FDE60`);
  - the four scale stores of the text style parser, each found in its own labelled block exactly once, through
    `overlay_text_scale_x` / `_y` (v2.00 `unit_00E4`, Spec II `unit_008E`).
  Removing the insertions gives the pinned units back byte for byte.
- `native/display_aspect_hook.cpp` checks ra against the target's return labels (`native/game_target.hpp`
  `menu_ortho_call_return`, `overlay_ortho_call_return`, `overlay_draw_end_return`) and leaves the strobe renderer alone when
  the race monitor called it (`overlay_caller_slot`, `monitor_overlay_call_return`).
- `on_overlay_ortho()` follows what is on screen: while the presentation fills the screen at R (`presented_fill`, the state the
  presenter uses), the canvas is `overlay_canvas()`: for R > 16:9 the x extent times s around its centre (21:9: 0..640 becomes
  -100..740, 32:9: -320..960); for R < 16:9 the y extent times (16/9) / R around its centre (4:3: 480..0 becomes 560..-80). The
  overlay keeps its 16:9 shape, centred, like the HUD. Otherwise (the 16:9 pillarbox of menus, FMVs and v2.00 photo mode, and
  always at 16:9) the game's own arguments stay. Only the game's own canvas (left 0, top 0, bottom 480, right 640) is changed;
  anything else is logged once per call site and left alone.
- When it widened the strobe renderer's canvas, it opens the text window: until `after_overlay_draw`, the parser's stores get
  the value times the inverse factor on the same axis (21:9: x times 16/21), so the glyphs are narrowed with their boxes.
  Outside the window the functions return the value they are given.
- Writes: four FPU argument registers at the ortho calls, and the value the game itself stores at the parser's four stores
  while the window is open. The host writes nothing in guest RAM; no save data.

## 4. Validation (2026-11-04, plain test builds, headless)

Builds: the same tree linked with the units of the task/134 base (camera calls only: the reference) and with this script's units
(the candidate); v2.00 from `private/slice217-ee-aspect-producer/generated`, Spec II from `private/slice405-spec2-aspect-producer/
generated` (TASK-166). `scripts/coregs2_bench/overlay-aspect-check.sh`, frames at 1x. Recordings: v2.00 Arcade single race
(Clubman Stage Route 5, the presses of `arcade-run.sh`, Start at VBlank 10700: `build/cam360/inputs/t095-pause-v200` in the
worktree; the pre-race screen is on from 8041 to 8735, the pause from 10702); Spec II the owner's `george-pausa` (George V
Paris Time Trial: the lobby from 2037, the pause at 3500..3600; replayed to VBlank 3700).

| Check | v2.00 | Spec II |
|---|---|---|
| 16:9 (the replay default): candidate against reference | race clock identical at all 11997 VBlanks (2186 racing), state at 10900 identical (128 KiB), 41 of 41 frames identical | clock identical at all 3698 VBlanks (1542 racing), state at 3550 identical (208 KiB), 37 of 37 frames identical |
| 21:9 and 32:9: the race | clock and state identical to the reference at each ratio | the same |
| 21:9 and 32:9: frames that differ | only the overlay frames: the pre-race screen (8100..8700) and the pause (10800..12000), 20 of 41 at each ratio | only the lobby (2050..2150) and the pause (3550, 3600), 5 of 37 |
| overlay geometry (`overlay_geometry.py`, inside the centred 16:9 box) | 20 of 20 frames: the candidate is the reference narrowed by s (mean error 12..18 at 21:9 against 66..76 unnarrowed; 17..26 against 56..80 at 32:9), centred on the frame's middle | 5 of 5 (5..28 against 36..86) |
| pause button and its text, frame pixels (the CONTINUE button) | 16:9: button 155, text 91; 21:9: 119 and 70 (155/1.3125 = 118, 91/1.3125 = 69); 32:9: 78 and 46; text/button 0.587 / 0.588 / 0.590; with the canvas alone (before the parser stores) the text stayed 91 and overflowed | the same look on the pictures (Config. de Tela inside its button) |

Pictures (worktree `build/cam360/evidence/t095/`, each frame stretched to the presented ratio): the candidate's pre-race screen,
lobby and pause at 21:9 and 32:9 have the 16:9 reference's shape, centred, with the wider race around them; the reference's are
stretched. The overlays follow the presentation: in these runs it filled the screen from the first `RaceDisplay::render`
(v2.00 8041, Spec II 2037) to the end, pause included.

Known look: a full-screen layer of these pages is narrowed with them. The Spec II lobby fades out to the race (frame 2150): the
dim covers the centred 16:9 box and the sides show the race until it starts; the bars at the top and bottom of the pre-race
screens end at the box's edges, like the HUD's.

Not reached in a run (High confidence from the call graph, not checked): the results (`ResultArcade::render` calls the strobe
renderer), the pit menu, the race monitor's exclusion (the renderer's saved caller), Spec II photo mode (the scene policy does not
see photo mode on Spec II, so its panel is widened with the filled picture). Replay playback controls are race HUD
(`RaceDisplay::put_replay_mode_display`), covered by P2 since TASK-080.

CTest `native_display_aspect` (`test_overlay_canvas`), `native_game_target`, `native_camera_360`, `native_race_display`,
`native_race_widescreen`, `native_license_guard`, `native_controls`, `native_standalone` pass in both builds; Python
`test_camera360_units.py` 11, `test_display_aspect.py` 22, `test_aspect_elf_patch.py` 9, `test_spec2_aspect_patch.py` 5.
