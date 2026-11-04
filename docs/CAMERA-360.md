# 360-degree chase camera on the right stick (TASK-159)

2026-11-04. Branch `task/159-360-chase-camera`. Both games, v2.00 and Spec II. **On by default** (owner, 2026-11-04; it ships in
rc13): `GT4_CAMERA_360=0` (or `camera_360=0` in `game.ini` for a direct start) turns it off. The launcher has no toggle yet (a
later step).

**Credits.** Idea and call site from the 360 Degree Chase Camera patch by Vyerq/unko_ (v2.00) and Nenkai's port from
Duchi/Vyerq (Spec II). The community patches were read only to understand what they do; no patch words are used or committed.
Our implementation is a host function in C++ (`native/camera_360.*`) reached from the translated game code
(`scripts/camera360_units.py`), with its own angle rule.

## What the reference patches do (decoded, Confirmed)

Both are PCSX2 patches for the same idea. Decoded word by word (R5900):

- **The call site.** One `jal` of the game is pointed at a small routine placed in free RAM at `0x000F1100`:
  - v2.00: the word at `0x003456D0`, originally `jal 0x0036CC40` (Confirmed in the reconstructed ELF), with
    `lwc1 $f13,0($s0)` in its delay slot;
  - Spec II: the word at `0x00206488`, originally `jal 0x00230F00` (Confirmed in the derived ELF), with `mov.s $f12,$f3` in its
    delay slot.
- **The routine.** It loads one byte (`lui t7` + `lbu`): v2.00 `0x0086F99E`, Spec II `0x0089B0DE`; subtracts 127; moves it to
  `f30` and converts it to a float; loads the f32 `0x3CC90000` (0.0245361328, pi/128 rounded down) into `f31` and multiplies;
  then jumps (`j`) to the original callee with `add.s $f13,$f31,$f13` in the jump's delay slot. So the callee receives
  `f13 + (byte - 127) * 0.0245` and returns to the original return address. The routine uses `t7`, `f30` and `f31` as scratch.
- **"Disable right analog throttle/brake"** (supplied with the Spec II lines). Two 16-bit writes of `0x1000` to the upper half of
  the words at `0x004387CC` and `0x004387F4`. **These are v2.00 addresses** (Confirmed): in v2.00 both words are
  `beql a3,zero,0x00438860` inside `func_004386F0`, and the write turns each into an unconditional `b 0x00438860`. In the Spec II
  derived ELF the same addresses hold `lui a3,0x0066` of an unrelated initialiser, which the writes would corrupt. The Spec II
  equivalents are the `beql` at `0x0031494C` and `0x00314974` (the same function at `0x00314870`).

## What the game does there

- **The chase camera spring** (v2.00 `func_00345658`, gt4-decomp names; Spec II `0x002063D8`, Public Beta lineage, with an
  extra mode switch before the call). Each frame it reads the camera's yaw state `s->x` (`s` = `*(this+4) + 0xCAB8 + 48 * index`
  on v2.00, `+ 0x20900` on Spec II) and the target yaw, and computes
  `force = angle_difference(target, s->x) * k - s->v * damping`, then integrates `s->v` and `s->x`. The angle difference
  (v2.00 `func_0036CC40`, Spec II `0x00230F00`) wraps `a - b` into `[-pi, pi)`; the target is in `f12`, the current yaw in
  `f13`. (Confirmed: decomp source and both ELFs.)
- Adding an angle `d` to `f13` for that call makes the spring settle at `target - d`: the camera swings round the car,
  damped by the game's own spring, and swings back when `d` returns to 0. Only the camera's own state moves.
- **The pad byte.** The game opens pad port 0 with its DMA buffer at v2.00 `0x0086F980` (`func_00551810`, ports `0x100` bytes
  apart) and Spec II `0x0089B0C0` (the same code shape at `0x0051BF18`). A headless run of the rc12 test build with a recording
  that holds the sticks at `23 45 67 89` (right X, right Y, left X, left Y) and `PS2_PEEK=86F980,80` read, at the end:
  buttons `FF FF` (none pressed, active low) at `+0x1C`, then `23 45 67 89` at `+0x1E..+0x21`. So `0x0086F99E` is the right stick
  X byte the game reads (Confirmed, v2.00). The same run on the Spec II rc12 test build with `PS2_PEEK=89B0C0,40` read the same
  bytes at `0x0089B0DC..0x0089B0E1`, so `0x0089B0DE` is Spec II's right stick X (Confirmed).
- **The analog channel handler** (v2.00 `func_004386F0`, Spec II `0x00314870`) takes a channel's value and its assigned type:
  type 3 stores the accelerator at `+8` (flag 2), type 4 the brake at `+10` (flag 4), each through the scale
  `func_004386C8` (Spec II `0x00314848`: `n < 0 ? 0 : min((n + 4) >> 3, 4096)`) and only when the scaled value is not 0. In the
  default controller setup the right stick Y is one of those channels (Confirmed, v2.00 Arcade race, rc12 test build: with
  the accelerator released at VBlank 10300, the car coasted to 56 mph at 10600 and 42 at 10800; with the right stick pushed up
  from 10300 it reached 71 mph at 10600, and pushed down from 10600 it stood at 0 mph with the brake lights on at 10800).

## Our implementation

**Why a translated call hook.** The call is a direct `jal`. The translator (donor `recomp/codegen.cpp`, `call_to`) turns a direct
`jal` into a direct C++ call into the callee's unit (`unit_009B(c, 40u, depth + 1u)`), so the runtime's `dispatch` never sees
it (the TASK-080 aspect hooks work only because their calls are virtual, through `jalr`). A VBlank write cannot reach it either,
and a code cave in guest RAM cannot run (no translated code there: the game halts on "no translated code"). The options were:

- (a) add a host call to the translated unit at the call site, between the delay slot and the call;
- (b) add one at the callee's entry, filtered by return address: same unit change, but the callee has four callers in v2.00;
- (c) an ELF patch with our own MIPS in padding and a re-translation (TASK-117's flare helpers): a new producer and entry list,
  and the angle rule would be guest code.

(a) is the smallest: `scripts/camera360_units.py` copies the target's translated units and changes one line per call site plus
one declaration per unit, and proves that removing them gives the pinned unit back byte for byte. Entry ids, labels and every
other unit stay as they are, so no re-translation and no new entry list. The EE library is then built from those units as
TASK-134 builds it.

**The calls.**

| Target | Unit | Call (`jal`) | Return label | Host function |
|---|---|---|---|---|
| v2.00 | `unit_0091` | `0x003456D0` → `0x0036CC40` | `0x003456D8` | `before_chase_yaw_call` |
| v2.00 | `unit_00CE` | `0x004387C0` → `0x004386C8` (case 3) | `0x004387C8` | `before_analog_pedal_call` |
| v2.00 | `unit_00CE` | `0x004387E8` → `0x004386C8` (case 4) | `0x004387F0` | `before_analog_pedal_call` |
| Spec II | `unit_0041` | `0x00206488` → `0x00230F00` | `0x00206490` | `before_chase_yaw_call` |
| Spec II | `unit_0085` | `0x00314940` → `0x00314848` (case 3) | `0x00314948` | `before_analog_pedal_call` |
| Spec II | `unit_0085` | `0x00314968` → `0x00314848` (case 4) | `0x00314970` | `before_analog_pedal_call` |

The return labels and the stick byte are in `native/game_target.hpp` (`camera_*` fields); each host function returns at once
when the switch is off, and otherwise acts only when `ra` holds one of its own return labels (a unit of another target can
never feed it).

**The angle.** `offset = pi * clamp((stick - 128) / 127, -1, 1)` as an f32, from a table built at compile time, added to `f13`
with the guest FPU add (`ps2::fp::add`, the add the translated `add.s` uses). Unlike the patch (centre 127, so a resting stick
at 128 turns the camera by 0.0245 rad, 1.4 degrees, and the right end stops at 3.1407), rest is exactly no change (`f13` is not
touched at all), and both ends are a half turn. Direction as the patch: a higher byte adds a positive angle.

**Throttle and brake.** While the camera is on, `before_analog_pedal_call` sets the channel value `a0` to 0 just before the
scale call of the accelerator and brake cases; the scale of 0 is 0, and the handler's own `beql a3,zero` then stores nothing:
the outcome of the "Disable right analog throttle/brake" patch (which forces that branch), for both targets, only while the
camera is on (so by default; `GT4_CAMERA_360=0` gives the right stick Y back to the pedals). The pressure buttons still
accelerate and brake, and steering, the menus and photo mode never pass
through these two cases. A host-side alternative (holding the guest right stick Y at rest) was not taken: it would also
take the stick from the menus and photo mode, and it would change what recordings hold.

**The switch.** `GT4_CAMERA_360`: unset, empty or `1` = on (the default, owner 2026-11-04), `0` = off, anything else refuses
the start (like `GT4_FORCE_WIDESCREEN`). Read at start in both start paths (`native/game_main.cpp`), which log
`[camera360] on: ...` or `[camera360] off`; `game.ini` key `camera_360` (`0`/`1`, absent = `1`) becomes the variable in a
direct start (`native/standalone.cpp`). A target without the addresses keeps the camera off and logs one line. With the stick
at rest the camera changes nothing (checked below), and none of the owner's recorded drives moves the right stick, so the
benches and identity gates of later builds are unaffected by the default.

## Build recipe (plain test build)

From the worktree, in Git Bash (the TASK-134 scripts; VS x64 environment inside them):

```
python scripts/camera360_units.py v200 C:/Antigravity/gt4-staticrecomp/private/slice217-ee-aspect-producer/generated build/cam360/v200-units
cmd //c scripts\coregs2_bench\game\ee_configure.cmd cam360-ee-v200 <build/cam360/v200-units, absolute> ""
cmd //c scripts\coregs2_bench\game\ee_build.cmd cam360-ee-v200
T134_ARGS="-DGT4_EE_LIBRARY=<ps2game.lib> -DGT4_EE_LIBRARY_SHA256=<its hash> -DGT4_FAST_HEADERS=ON -DGT4_SSE41=ON" \
  cmd //c scripts\coregs2_bench\game\configure.cmd cam360-v200 v200
cmd //c scripts\coregs2_bench\game\build.cmd cam360-v200 all
```

Spec II: the same with `spec2`, the Spec II units (`docs/inputs/spec2-ee-generated.json`) and `configure.cmd ... spec2`. A
release build adds PGO and ThinLTO as `scripts/coregs2_bench/pgo-all.sh` does, with `EE_GENERATED` set to the hooked units.
The profile of the changed units then changes slightly (one call more per site), which PGO handles like any source change.

## Validation

**Fixtures.** CTest `native_camera_360` (switch values, the stick table: 0 at rest, odd, monotonic, half turns at both ends; the
yaw argument untouched at rest; the pedal return labels; the v2.00 table), `native_game_target` (the new fields of both tables),
`native_standalone` (`camera_360` in `game.ini`). Python `tests/python/test_camera360_units.py` (insertion point, exact
reversal, refusals, the call lines against the table and against `native/game_target.hpp`, and the pinned units when present).
Full CTest of the v2.00 build: 294 of 296 pass, one skipped (`hw_replace_q07_pcsx2_dumps`), and `python_native_contracts`
fails only in 13 GS settings tests (`test_gs_settings`, `test_gs_live`) whose files this branch does not touch; the Spec II
build's 50 `native_` tests: 49 pass, the same Python failure.

**Drives.** No v2.00 race recording had the chase camera, so `scripts/coregs2_bench/arcade-run.sh`'s presses (Arcade, Clubman
Stage Route 5, Ford Ka, 6 cars) were written as a recording, plus Select at VBlank 10050 once the rolling start runs (the race
starts in the bumper view; Select gives the chase view). The stick copy moves the right stick X: full left 10300-10500, rest,
full right 10700-10900, rest, a quarter left 11100-11250, rest, a sweep from 0 to 255 in steps of 16 every 8 VBlanks from
11400, rest from 11600; the race clock runs from about VBlank 9814. Recordings and their `.setup` (the Arcade card) are in the
worktree's `build/cam360/inputs`, made by `build/cam360/make_arcade_recordings.py`; `GT4_BENCH_INPUTS` points the bench
scripts there. Reference: the same tree and flags linked with the library of the plain slice217 units; candidate: with the
hooked units. CoreGS 2.0 at 1x, headless, `camera360-check.sh`. The rows below were measured with the switch given
explicitly (`1` on, unset off, before the default changed); the default-on build was checked again afterwards (end of this
section).

| Check (v2.00, 2026-11-04) | Result |
|---|---|
| Switch off, stick recording, against the reference | race clock equal at all 2186 racing VBlanks; state dump (64 KiB simulation + 64 KiB race object) byte-identical at VBlank 10450 and at 11900; 121 + 61 frames identical |
| Switch on, stick at rest (plain recording), against the reference | everything identical: clock, the 128 KiB state dump at 11900, 120 frames from boot to the race (every 100 VBlanks) |
| Switch on, stick recording, against the reference | race clock equal at every VBlank (11997, 2186 racing) |
| ... state at VBlank 10450 (stick full left) | simulation: 30 of 16384 words differ, all camera-owned: the six cars' camera spring states (yaw, speed, force at `0xCAB8 + 48 * i`) and two camera-relative angles per car (`0x6D98 + 0x10A0 * i` and `+0x294`, off by about pi); race object: 690 words from `0xDA0` (the light flare queue, what the camera sees) |
| ... state at VBlank 11900 (stick released at 11600) | simulation: 0 words differ; race object: 21 presentation words (`0x279C..0x27E0`, `0xE240..0xE268`) |
| ... frames (every 25 VBlanks from 9000) | 73 identical, 48 differ: none before the first move (10300); the view turns to the front at full left and full right, to the side at a quarter turn, sweeps with the stick, and is identical again 175 VBlanks after each release (10675, 11025, from 11725) |
| Right stick Y moved (up 10300-10600, down 10600-10900, accelerator released at 10300) | without the hook: the car accelerates and brakes (2462 simulation words differ from coasting, 14 of 16 frames); camera on: identical to coasting (0 simulation and 0 race object words, 16 of 16 frames) |

**Default-on builds (owner decision, same day).** Rebuilt with the new switch rule (`gt4vulkan.exe` `80884fb1...`,
`gt4vulkan-spec2.exe` `ec4727dc...`, plain test builds) and checked again with `camera360-check.sh` (off = `GT4_CAMERA_360=0`,
on = unset): v2.00 stick drive, off identical to the reference (clock, 128 KiB state at 10450, 61 frames), on unset gives the
same 30 camera words and the same 24 changed frames as the explicit switch; v2.00 stick at rest, on unset identical to the
reference (clock, state at 11900, 120 frames); Spec II stick copy, off identical (clock, 208 KiB state at 7420, 35 frames), on
unset the same 21 words; Spec II original drive, on unset identical to the reference (clock, state at 8900, 12 frames). The
logs show `[camera360] off` for `0` and `[camera360] on: ...` unset.

The camera's own state lives in the same block as the cars (`*(this+4)` of the camera is the dynamics object), so a dump taken
while the stick is off rest cannot be identical; the race clock at every VBlank and the whole block being identical again after
the release say the physics never saw the camera (a changed word would spread through the cars within seconds).

## Spec II

The same mechanism and rules; only the addresses differ (table above; `kSpec2Hooks` in `native/game_target.hpp`). The units are
the Spec II translation pinned by `docs/inputs/spec2-ee-generated.json`, regenerated for this check with
`scripts/spec2_ee_generate.py` (the derived ELF and the slice278 entry seeds) into the worktree's `private/ee-spec2/generated`:
inventory `2d8c92a4...`, equal to the pin.

- **The analog handler** is the same function with ten cases (`0x00314870`); the accelerator and brake cases call the same scale
  (`0x00314848`) at `0x00314940` and `0x00314968`, followed by `beql a3,zero` at `0x0031494C` and `0x00314974`.
- **Right stick Y** is the analog throttle and brake here too (Confirmed): the owner's Tokyo Route 246 Time Trial
  (`tokyo-r246`) with the right stick Y pushed up at 8000-8150 and down at 8700-8850, on the rc12 test build and on the
  reference, differs from the original drive in 473 simulation words at VBlank 8900 and in 3 frames from 8800.
- **Drive.** `tokyo-r246` changes views; the chase view runs from about VBlank 7000 to 10400 (the race clock from 2482). The
  stick copy (`build/cam360/stick_copy.py`, every other field of every state kept) holds the right stick X full left 7300-7450,
  full right 7600-7750, at a quarter right (`0xC0`) 7900-8050 and sweeps from 0 to 255 from 8300 to 8436.

| Check (Spec II, 2026-11-04) | Result |
|---|---|
| Switch off, stick copy, against the reference | race clock equal at all racing VBlanks (6087 to 8700, 7919 to 10959); 208 KiB state dump byte-identical at 7420 and 9500; 69 + 40 frames identical |
| Switch on, the original drive (stick at rest) | clock (8998 VBlanks), state dump at 8900 and 12 frames identical to the reference |
| Switch on, stick copy | race clock equal at every VBlank |
| ... state at 7420 (stick full left) | simulation: 21 of 36864 words: the camera spring states (`0x20900` and `0x20930`: yaw, speed, force) and words the drawing writes per car (car `0x6B40`: `+0x4`, `+0x8`, `+0xF8`, `+0xFC`, `+0x1EC`, `+0x1F0`, `+0x2E0`, `+0x2E4`, `+0x4FC`, `+0x7CC`, `+0x7D0`, `+0x7D8`, `+0x7E4`; the second car `0xB080`: `+0x4FC`, `+0x7E4`); race object: 18 words |
| ... state at 9500 (stick at rest since 8436) | simulation: 14 words, the same kind (the second camera state `0x20930` and drawing-written car words); race object: 0 |
| ... frames (every 25 VBlanks from 7000) | 27 identical, 42 differ, from 7325 to 8525 only: the view swings to the front at both ends, to the side at a quarter turn, follows the sweep, and is identical again from 8600 |
| Right stick Y moved, camera on | identical to the original drive (0 simulation and 0 race object words at 8900, 12 of 12 frames) |

The drawing-written car words are the ones TASK-082 found when frames are left out (`scripts/coregs2_bench/frame_skip_check.py`
lists `0x4`, `0x8`, `0xF8`, `0xFC`, `0x1EC`, `0x1F0`, `0x2E0`, `0x2E4` and `0x7CC..0x7D8` from `0x6B40`): what is drawn sets them, and
the physics never reads them. Here too the race clock is identical at every VBlank and the rest of the 144 KiB simulation is
identical a thousand VBlanks after the stick returned.

## Limits

- Pad port 0 only: in split screen both chase cameras follow player 1's stick (the patch does the same).
- The angle follows the stick position, not a speed: the stick held at one side holds the camera there; released, the game's
  spring brings it back. Full left and full right are the same view (from the front).
- With the camera on (the default), the right stick Y no longer accelerates or brakes in races, whatever the controller setup
  assigns to it (cases 3 and 4 of the handler, as the patch). A player who drives with the right stick Y turns the camera off
  (`GT4_CAMERA_360=0`, `camera_360=0`; a launcher toggle comes later).
- A recording replays the camera only on a build with the hook and the same switch: the camera is presentation, so the race
  is the same either way.
