# Trigger sensitivity: throttle and brake precision (TASK-161 study)

2026-11-04. Read-only study of both games' code (reconstructed v2.00 ELF, derived Spec II ELF), names from gt4-decomp at
`3b1f2ae5b`, and of our host input path. Labels: Confirmed (read in the code or measured), High confidence, Hypothesis,
Unknown. The reference is the "Full Trigger Sensitivity" PCSX2 patch by Silent & Aero_ in
`private/references/pcsx2_patches-8767f43d` (no licence: read only; no patch words used).

## 1. What the game does with the pressure byte

**Where (Confirmed).** The race input's DualShock handler (v2.00 `func_0043BBC0`, called from the device switch of
`func_0043CF90` for the DualShock types) has three parts: the analog channels (sticks; TASK-159b), then, for the pressure-capable
pad type (the high nibble of the device word is 5), a loop over the 12 pressure buttons: `p = func_0055EF50(record, channel)`
with the channel from the table at `0x006A64E0` (`05 04 06 07 08 09 0A 0B 0C 0D 0E 0F`), and when `p` is not 0, a switch on
the function the controller setup assigns to that button (`sp + 13 + slot`): the jump table at `0x006A64F0` sends function 4
to `0x0043BEE8` (throttle), 5 to `0x0043BF48` (brake), 6 to `0x0043BFA8` and 7 to `0x0043C008`, every other function to the
loop's end. Functions 6 and 7 store at +14 (flag 0x10) and +12 (flag 8); the patch calls them e-brake and reverse
(Hypothesis for the names).

**Each case (Confirmed, v2.00):**

```
x   = min(p << 1, 256)            ; sll v1,v1,1 then slti/movz
out = min(100 * x / d, 256) << 4  ; 100x by shifts, div by d, slti/movn, sll 4
```

| Case | v2.00 code | d | stored at | 100% from p = | game levels (0..100%) |
|---|---|---|---|---|---|
| throttle | `0x0043BEE8` (`sll` at `0x0043BEF0`, `addiu a3,zero,35` at `0x0043BEFC`) | 35 | +8, flag 2 | 45 | 46 |
| brake | `0x0043BF48` (`0x0043BF50`, `0x0043BF5C`) | 65 | +10, flag 4 | 84 | 85 |
| e-brake (Hypothesis) | `0x0043BFA8` (`0x0043BFB0`, `0x0043BFBC`) | 20 | +14, flag 0x10 | 26 | 27 |
| reverse (Hypothesis) | `0x0043C008` (`0x0043C010`, `0x0043C01C`) | 35 | +12, flag 8 | 45 | 46 |

No dead zone or curve: any `p` above 0 counts, linearly, and the output saturates at `p = 45` (throttle) or `84` (brake). A
digital-only press (pressure mode off) gives `p = 0` here; the button's digital bit is handled by the separate button mapping
(`func_004389F8`, table `0x006A64D0`) (High confidence).

**Spec II (Confirmed).** The same loop and the same four computations at `0x0031910C..0x003192EC`, compiled differently: an
if-chain instead of the jump table, and the divisors kept in registers set before the loop: `s1 = 35` (`0x00319114`, used by
the throttle at `0x003191C8` and by the reverse case), `s6 = 65` (`0x0031911C`, brake), `s5 = 20` (`0x00319120`, hand brake).
The doubling `sll` is at `0x00319194` (throttle), `0x003191F0` (brake), `0x00319248`, `0x0031929C`.

**The patch.** v2.00: the four `sll` become `nop` and the four divisors 98, so `out = min(100p / 98, 256)`: 100% from p = 251,
about 251 levels. But its reverse line writes the halfword at `0x0043C018`, which is `slti v0,v1,256` (the divisor is at
`0x0043C01C`), so on v2.00 its reverse case clamps at 98 instead of 256 and keeps the divisor 35 (Confirmed: a slip in the
patch). Spec II: the throttle and brake `sll` become `nop` and `s1`, `s6` become 98; the reverse case shares `s1`, so it gets
98 with its doubling kept (100% from p = 125).

## 2. Our host side

**Path (Confirmed).** `native/window.cpp` polls the pad through `native/pad_mapping.cpp` (SDL3, or XInput with `GT4_INPUT`),
which reads each binding of `native/controls.cpp`; `apply_pad` writes the buttons, the four stick bytes and the 16 pressure bytes
into the SIO2 model (`ps2::set_pad_pressure`). A trigger binding gives `pressure = round(f * full)` over its travel `f` past the
dead zone (linear by default), with `full` = 45 on R2 and 84 on L2 by default (TASK-147) or the binding's `range`; the button's
bit is set above the dead zone. A digital source (a face button, a key) gives pressure 255.

**Is precision lost there? (Confirmed: no.)** The trigger is read as a byte (0..255; SDL3's 0..32767 reduced to it) and
spread over the game's own useful range 0..45 / 0..84 with rounding to the nearest step: every game level from 0% to 100% is
reachable, in order, across the travel. No curve by default, no digital mapping, the right buttons (R2 throttle, L2 brake when
the in-game setup puts them there). The host cannot add levels: the game has 46 and 85 distinct outputs whatever byte it gets.
The coarse feel comes from the game's own scale (about 2.2% of throttle per step), made for the DualShock 2's pressure buttons.

**What the defaults assume.** The physical triggers are bound to the guest R2 and L2 buttons (Confirmed,
`native/controls.cpp`). The game's default setup accelerates with Cross (Confirmed: the Arcade drive holds Cross and the car
accelerates) and brakes with Square (High confidence); a face button or a key sends pressure 255, which is 100% on either
scale. The 45/84 defaults matter when the in-game setup assigns R2 to the throttle and L2 to the brake (the usual trigger
setup); with the brake on R2, `range=84` on R2 (and `range=45` on L2) restores the end of travel as 100%.

## 3. For which setups the change matters

- Pressure buttons driven by analog triggers (R2/L2, or any button bound to a trigger): from 46/85 to about 255 levels.
- A real DualShock 2's pressure buttons (Cross/Square): the same formula, so a full press reaches 100% only at the end of a
  pressure range that is now 251 bytes wide instead of 45 (a lighter press gives less throttle than before). Keyboard and
  face buttons send 255: unchanged (100%).
- Sticks (the analog channel path, `func_004386F0`, scale `(n + 4) >> 3`, 0..4096): already full range; not touched.
- Wheels (`func_0043C260`): their own path; not touched.

## 4. Conclusion

The precision loss is in the game, not in our host: the host already gives every level the game has. More precision needs
the game-side change, and the host's trigger ranges must follow it (0..255 for both when the change is on), which TASK-161
implements behind `GT4_TRIGGER_FULL_RANGE`, off by default until the owner's test.

## 5. Our implementation (TASK-161)

- **Game side.** `scripts/camera360_units.py` replaces two statements per pedal case in the translated units (no call there,
  so not a call hook): the doubling `set32(c.r[3], c.r[3].w[0] << 1);` becomes `pedal_pressure(v)`, and the divide's divisor
  operand becomes `pedal_divisor(d)` (`native/trigger_range.hpp`). With the switch off they return `2v` and `d`, exactly the
  original statements; on, `v` and 99: `min(100p / 99, 256)`, 100% from `p = 254` (the patch: 98, from 251), so all 255
  pressure steps up to 254 are different levels. Throttle and brake only, both games; the e-brake and reverse cases are not
  touched (the owner asked for the throttle and the brake; the reference patch's reverse line is a slip on v2.00 and changes
  reverse through the shared register on Spec II). On Spec II the divide is replaced rather than `s1`'s load, so the reverse
  case keeps its 35. Sites: v2.00 `unit_00CE` (`sll` `0x0043BEF0`/`0x0043BF50` in the case blocks, `div` `0x0043BF2C`/
  `0x0043BF8C`); Spec II `unit_0086` (`0x00319194`/`0x003191F0`, `0x003191C8`/`0x00319224`). Each statement is found in its
  labelled block exactly once, and removing the replacements gives the pinned unit back byte for byte.
- **One switch.** `GT4_TRIGGER_FULL_RANGE` (`game.ini` `trigger_full_range`): unset, empty or `0` off (the default until the
  owner's test), `1` on, anything else refuses the start. The game reads it once at start (`native/game_main.cpp`) and sets
  both the pedal scale and the controls' trigger scale (`gt4::controls::set_pressure_scale`), so the launcher and a direct
  start cannot disagree.
- **Host ranges** (`native/controls.cpp`). Each trigger binding has two full-travel values: `range` (default 45 on R2, 84 on
  L2, 255 elsewhere) for the game's own scale and `range_full` (default 255) for the full one; each applies under its own
  scale only. So a `range=45` saved for TASK-147 never caps the throttle when the full range is on, and a full-scale choice
  never leaks into the game's own scale. Every combination (defaults, either option, both; R2, L2 and another button; both
  scales) is in CTest `native_controls`.
- **Launcher.** A "Full trigger range (experimental)" toggle on the Controls page above L2 and R2 (setting
  `trigger_full_range`, General/Controls, default off), passed as `GT4_TRIGGER_FULL_RANGE` at Play and written to `game.ini`;
  the trigger meters show the percentage of the game's 100% and the pressure over that 100% (`28/45`, `158/255`);
  `range_full` is accepted and kept by the controls parser. Texts in en-US and pt-BR.
- **Diagnostic.** `GT4_TRIGGER_TRACE=1` logs each change of a pedal's level (`[trigger] throttle pressure p level L/256`) from the
  pressure the case read and its divisor.

## 6. Validation (2026-11-04, plain test builds)

Recordings: copies of TASK-159's Arcade drive (v2.00) and of the owner's `tokyo-r246` (Spec II) where Cross alone is held with
its pressure rising from 1 to 255 over 510 VBlanks, then Square alone the same way (`build/cam360/pressure_sweep.py`; v2.00 from
VBlank 10300, Spec II from 7300). Reference: the same trees linked with the plain units.

| Check | v2.00 | Spec II |
|---|---|---|
| switch off (default) against the reference | race clock identical (1586 racing VBlanks), simulation identical, 24 of 24 frames; the race object differed in 1 word of the light flare list, a run made while a build ran (the same timing-sensitive words as in TASK-159b) | everything identical: clock (5787 racing VBlanks), 208 KiB state, 24 frames |
| levels the car receives, off (trace) | throttle 45 distinct levels over the sweep, 100% first at p = 45; brake 84, 100% at p = 84 | the same |
| levels the car receives, on (trace) | throttle 254 distinct levels (p = 1..253 each its own, then 100%), 100% first at p = 254; brake the same | the same |
| on: race clock | identical at every VBlank | identical at every VBlank |
| on: the drive | differs, as it must (a lighter throttle at the same pressure: 2331 simulation words at 11350) | differs (401 words at 8350) |

CTest `native_trigger_range`, `native_controls`, `native_pad_mapping`, `native_standalone`, `native_game_target`,
`native_camera_360`, `native_fov_adjust` pass in both builds; Python `test_camera360_units.py` 10/10. Launcher: Rust tests
660 passed, 0 failed; `dev/nav-test.html` 217 of 217; `dev/focus-check.html` en-US 283 of 283 and pt-BR 290 of 290, with
five new checks (the toggle starts off, the L2 meter reads over 84, on it reads over 255, back off, focus returns to the
row); the two locale files have the same 690 keys and placeholders.
