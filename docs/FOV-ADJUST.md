# Field of view adjustment on R3 + D-pad (TASK-160)

2026-11-04. Branch `task/160-fov-adjustment` (from `task/159b-camera-pedal-scope`). Both games, v2.00 and Spec II. **On by
default** (owner, 2026-11-04): `GT4_FOV_ADJUST=0` (or `fov_adjust=0` in `game.ini` for a direct start) turns it off; unset,
empty or `1` = on; anything else refuses the start. No launcher toggle yet.

**Credits.** Idea from the "FoV Adjustment" PCSX2 patch by Duchi/Vyerq (v2.00) and Blitznoz, from Duchi/Vyerq (Spec II), in
the reference copy `private/references/pcsx2_patches-8767f43d` (thefirefox12537/pcsx2_patches at `8767f43d`; no licence, so
read only). No patch words are used: the function, its constant and the pad halfword were read from the reconstructed ELFs and
named with gt4-decomp at `3b1f2ae5b` (`private/references/gt4-decomp`).

## What the patch does (decoded, Confirmed)

Three pairs of PCSX2 conditional lines, run every frame:

- if the halfword at v2.00 `0x0064C794` (Spec II `0x00681344`) is `0xFFEB` (only R3 and D-pad up held; the halfword is active
  low), add 1 (Spec II: 2) to the byte at v2.00 `0x0037A1A4` (Spec II `0x0023E814`);
- if it is `0xFFBB` (only R3 and D-pad down), subtract 1 (Spec II: 2) from that byte;
- if it is `0xFFF9` (only L3 and R3), write the word `0x3C013F80` back there.

The word there is `lui at,0x3F80`, the high half of 1.0f; its low byte is the low byte of that high half, so the patch rewrites
a float immediate in guest code: each step up adds 1/128 above 1.0, each step down takes 1/256 below 1.0, and the byte wraps
after 127 steps up (1.9921875 to 0.5) or 128 down (0.5 to 1.9921875). Because the buttons must match exactly, nothing else may
be held (not even the accelerator).

## What the constant is

- **The function** (v2.00 `func_0037A170`, a virtual reached through two vtables, at `0x0067A63C` and `0x0067AA4C`, of
  classes gt4-decomp has not named; Spec II `0x0023E7E0`, found by its word pattern): `t = tan(fov * 0.5 * pi / 180)` with `fov` the camera's
  field of view in degrees at camera `+0xC` (`func_0057D380` is the tangent; Spec II `0x0050EAB8`), then
  `func_00378F18(camera, 1.0, 1.0, 1.0 / t)` (Spec II `0x0023D558`), which stores the three values at camera `+0x1C`, `+0x20`
  and `+0x24` (gt4-decomp `func_00378F18`: three floats into the object). Those are the projection's x and y scales and its
  focal scale.
- **The patch's 1.0** becomes k in all three: x = y = k and focal = k / t. The projection scales by k: k above 1 zooms in (a
  narrower field of view), below 1 zooms out (wider). The camera's distance is not touched (the patch's "distance view" is the
  impression of distance a zoom gives).

## Our implementation

- **Mechanism.** The constant is a guest instruction, and translated code cannot be rewritten at run time. The value is
  consumed by a direct `jal` (`0x0037A1BC` → `0x00378F18`, return `0x0037A1C4`; Spec II `0x0023E82C` → `0x0023D558`, return
  `0x0023E834`), so `scripts/camera360_units.py` (TASK-159's tool) adds one host call between that call's delay slot
  (`mov.s $f13,$f12`) and the call: `gt4::fov_adjust::before_projection_scale_call`. With k = 1 it touches nothing; otherwise
  it sets f12 = f13 = k and f14 = k / t with the guest's own divide (`ps2::fpu_div`, the translated `div.s`; f0 still holds t),
  exactly the values the patched immediate gives. The hook acts only when `ra` holds the target's return label.
- **The pad.** At every guest VBlank (`native/mtvu_runtime.cpp`, next to the TASK-079/080/117 holds) the host reads the port 0
  button halfword the game keeps (v2.00 `0x0064C794` = port 0 pad object `0x0064C728` + `0x6C`, followed by the right X, right
  Y, left X and left Y bytes; Spec II `0x00681344`, the same layout; Confirmed by RAM peeks with R3 + up held: `0xFFEB`), the
  same halfword the patch tests, so a recorded drive replays the same zoom.
- **The rule** (`native/fov_adjust.hpp`, `next_steps`): R3 + up raises k one step per VBlank, R3 + down lowers it, L3 + R3
  sets it back to 1 (L3 + R3 wins over up or down; up and down together do nothing). Unlike the patch, other buttons may be
  held at the same time (a trigger or Cross on the accelerator), since a zoom is wanted while driving.
- **Step and range.** k = 1 + n / 128 with n from -64 to +128: 0.5 to 2.0 in exact f32 steps of 1/128 (the patch's step above
  1; below 1 its step is 1/256), stopping at both ends instead of wrapping. One second of R3 + up takes k from 1.0 to about
  1.47; the full range up takes 128 VBlanks (2.1 s), as the patch's 127 steps to its wrap. The same on both games (the Spec II
  patch steps by 2).
- **Persistence.** k stays as it is across races and menus until L3 + R3 or the next start (the patch's byte does the same).

## Build

As TASK-159 (`docs/CAMERA-360.md`, "Build recipe"): `scripts/camera360_units.py <v200|spec2>` writes the camera and the
projection calls together (v2.00 `unit_009E`, Spec II `unit_004F` for this one), and the EE library is built from its output.
The host side (`native/fov_adjust_hook.cpp`) is always in the executable; an EE library from the plain units leaves it unused.

## The buttons in the game

- **Races (Confirmed, both games, reference builds without the hook).** The Arcade race (v2.00) and the owner's Tokyo R246
  drive (Spec II) replayed with R3 + up held for 100 VBlanks, R3 + down for 200 and L3 + R3 for 10 give the same race clock,
  the same state dump and the same frames as the same drives without them: in a race the game does nothing with R3, D-pad up
  or down or L3 in the default controller setup. (A first v2.00 run, made while a build was running, differed in 15 words of
  the race object's light flare list only; repeated without the build it is identical.)
- **Menus (High confidence, not measured).** D-pad up and down move the menu cursors; with R3 held they still do (the zoom then
  steps too, unseen until a race camera draws). R3 and L3 have no menu function we know of.
- **Replays (Unknown).** Not checked: a replay's own controls (camera, speed) were not driven.
- So the patch's combinations are kept; no other combination is proposed.

## Validation

Plain test builds (`gt4vulkan.exe` `1512f315...`, `gt4vulkan-spec2.exe` `2ad03124...`, CTest `native_fov_adjust`,
`native_game_target`, `native_standalone`, `native_camera_360` pass in both builds, Python `test_camera360_units.py` 7/7)
against the same trees linked with the plain units, headless, CoreGS 2.0 at 1x, `scripts/coregs2_bench/camera360-check.sh`
with `SWITCH=GT4_FOV_ADJUST`. The combination recordings are copies of TASK-159's Arcade recording (v2.00, chase view from
VBlank 10050) and of `tokyo-r246` (Spec II, chase view from about 7000) with R3 + up held for 100 VBlanks (k to 1.78), R3 +
down for 200 (k to its 0.5 floor) and L3 + R3 for 10 (back to 1): v2.00 from 10300, Spec II from 7300
(`build/cam360/button_copy.py` in the worktree).

| Check | v2.00 | Spec II |
|---|---|---|
| switch `0`, combinations | identical: race clock (1386 / 5487 racing VBlanks), 128 KiB / 208 KiB state at both dumps, every frame (41 + 11 / 37 + 10) | identical |
| on, nothing pressed | identical: clock, state at 11900, 60 frames from boot to the race | identical: clock, state at 8900, 12 frames (the owner's drive) |
| on, combinations: race clock | identical at every VBlank | identical at every VBlank |
| ... frames | the view zooms in (k 1.78), then out to 0.5 and back to 1 at the reset; 24 of 41 frames differ, only from 10325 to 10900; identical again after the reset | the same, 7325 to 7900; identical again from 8000 |
| ... simulation, zoomed in (v2.00 10450, Spec II 7450) | 0 of 16384 words differ | 9 of 36864: the words drawing writes per car (`0x6B40` + `0x4`, `0x8`, `0xF8`, `0xFC`, `0x2E0`, `0x2E4`, `0x7CC`, `0x7D0`, `0x7D8`) |
| ... simulation, after the reset (v2.00 11100, Spec II 8000) | 0 words | 10 words, the same kind (`+0xF8`... `+0x7D8`) |
| ... race object | differs: the light flare list (`0xDA0`...) while zoomed in, 97 words of a list at `0x1ACC..` after the reset | differs: 21 words while zoomed in, 512 words at `0xFA0..` after the reset |

What the zoom changes in the state is what the camera draws: the light flares it sees, the race object's lists of what was in
view, and (Spec II) the per-car words that drawing writes, which TASK-082 found when frames are left out and which the physics
never reads (`scripts/coregs2_bench/frame_skip_check.py`). The race clock stays identical at every VBlank and the rest of the
simulation identical, on v2.00 all of it; nothing points at collision, AI or sound reading the projection (no word of the
cars' dynamics outside those drawing words changes). So the race object and those drawing words are not byte-identical with
the zoom used, and that is the expected effect of drawing a different view, not of the race.

## Limits

- Port 0 only (player 1), as the patch.
- The zoom is not saved; every start begins at 1.0.
- D-pad up and down still reach the game: in the menus they also move the cursor (the zoom changes then too, with no visible
  effect until a race camera draws).
