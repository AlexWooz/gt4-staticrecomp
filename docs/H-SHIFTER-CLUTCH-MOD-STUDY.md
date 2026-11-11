# H-shifter, direct gear and clutch mod: deep study (TASK-215 follow-up)

Date: 2026-11-11. Author: research agent. Read-only on source: no game code, build, pin, save or card was
changed. Worktree `O:/Antigavity/gt4-wt-215-hshifter-deep`, branch `task/215-hshifter-deep`, based on the study
branch `task/104-hshifter-study` at `5c2373bd`.

Owner directive (2026-11-11, Telegram, two messages): modify the game, do not just remap buttons. Improve or add the
clutch system, add an analog clutch pedal input from the wheel, and make the gears apply directly, without emulating
the game's upshift/downshift sequence. Later addition: reverse is a gear position in the H-gate, the gate sequence is
R, N, 1, 2, 3, 4, 5, 6, and downshifting past 1 goes through N and then R; the game's reverse rules (speed lock, gear
lock, rev limits) must still be respected and the HUD must show R/N correctly.

This document deepens [H-SHIFTER-CLUTCH-STUDY](H-SHIFTER-CLUTCH-STUDY.md). It corrects two conclusions of that study
(the 0x0037AD40 gear path was the onboard camera, and the HUD setters were not called from
`RaceSinglePlayer::updatePlay`), maps the real drivetrain, and specifies an opt-in mod and its proof-of-concept plan.

Labels: **Confirmed** (read in the ELF, the decomp at identical addresses, our code, or measured), **High
confidence** (strong code or document evidence, not yet tested in game), **Hypothesis** (plausible, untested),
**Unknown** (not established).

Inputs used (read-only):

- The reconstructed ELF `private/reconstructed/SCUS_973.28.elf`, sha256
  `10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c` (same hash as the first study).
  Mapping used for every address in this document: guest address minus `0x000FF000` is the file offset
  (the first LOAD segment is file 0x1000 at vaddr 0x100000; the data segment is file 0x518A80 at vaddr 0x617A80,
  so both use the same subtraction). The first study's example `0x6A5EA0 -> 0x5A6EA0` was 0x1000 low; the correct
  file offset is `0x5A7EA0`. That does not change its string-table conclusion.
- Decomp `private/slice151-research/gt4-decomp` at commit `e0b70c4d6` (names, types and addresses).
- The first study, [H-SHIFTER-CLUTCH-STUDY.md](H-SHIFTER-CLUTCH-STUDY.md), and the input/wheel documents it cites.
- Disassembly: the ELF was decoded with Capstone (MIPS64 little-endian mode, which decodes the R5900 instructions
  used here except a few EE-only encodings), plus a direct scan of every `lui`/`addiu`/`ori` pair and every
  load/store displacement. The decoded instructions and the decomp agree at every address quoted below.
- No PCSX2 source, no `gt4recomp-gs` fork and no `private/tooling/pcsx2-*` was opened. This study is not
  PCSX2-tainted.

---

## Summary

1. **The real drivetrain is found.** The player car is an `Automobile` object. Its live gear is the byte at
   `Automobile + 0x624`, its clutch state is the byte at `Automobile + 0x626`, and its clutch engagement float is at
   `Automobile + 0x618`. These are fields of the drivetrain sub-object at `Automobile + 0x104` (gear is
   `+0x520` inside it, clutch state `+0x522`, engagement `+0x514`). The HUD reads the same two bytes. **Confirmed**
   (code shape and both consumers), **High confidence** for the field names.
2. **The first study's gear path was the onboard camera.** Functions `func_0037AD40`, `func_0037ACD0`,
   `func_0037AD10`, `func_0037BC20`, `func_0037BC68` and the `+0x180` field belong to
   `CameraSys::CameraOnBoard`: vtable `0x0067A5D8`, typeinfo function `0x005F5B70`, RTTI name string
   `0x0069FAC8` = `Q29CameraSys13CameraOnBoard`. The 12-entry table at `0x006211E8` and the values 20/25 are camera
   state, not gears. **Confirmed** (RTTI and vtable).
3. **The game's gear numbering is 0 = R, 1..count = forward; there is no neutral gear value.** Neutral is the
   clutch open, not a gear. The HUD renders gear 0 as `R` and 1..6 as digits; the source code has no `N` glyph in
   this display. The clutch flag only dims the gear character. **Confirmed** for 0 = R and the missing N value;
   **High confidence** for the dimming. A one-instruction patch can make the gear-0 character host-selected
   (`R` or `N`), so the mod can show R/N correctly while both use gear 0 with the clutch open.
4. **Direct gear application is feasible.** Write the gear byte directly and open the clutch first; the game's
   engine force path reads the clutch state and cuts torque. The game's own selector can be suppressed by clearing
   the shift request and delay, or overridden with a one-instruction patch. The gear applies in one physics tick,
   with no upshift/downshift sequence. **High confidence**; the remaining dependency is which selector path the
   reference car uses (gearbox type), settled by a live probe.
5. **GT4 does model a clutch, but it is an automatic clutch with no player input.** The drivetrain state machine has
   states 0 (open/free rev), 1 (engaged), 2/3 (slipping during a shift) and a continuous engagement float. The
   engine force is gated by the state, and rpm is synchronised through the 2/3 states. A mod can drive the states
   and cut torque; true continuous slip torque needs a deeper force patch. **Confirmed** for the state machine and
   torque gate; **High confidence** for the analog limitation.
6. **There is no clutch channel in the guest input.** The action table has no clutch action and the 0x20-byte
   `_AutomobileControl` packet has no clutch field. The pedal must come from the host (SDL3 or the wheel HID
   device) into a mod layer; the guest receives it through the existing host parameter block at guest `0x7F00`
   (TASK-080 pattern) or through VBlank guest-RAM writes. **Confirmed** for the action table and packet;
   **High confidence** for the two host routes.
7. **Replays and saves.** The replay/ghost recorder stores the control packet, not the gear or clutch. A mod lap is
   not replayable faithfully; the mod must be disabled in replay, ghost and autodemo. Race state is not saved, but
   the mod must never write car data or the transmission type. **Confirmed** for the recorder; **High confidence**
   for saves.
8. **Boundary.** Owner decision 0073 (2026-11-11): gameplay mods are in scope in this repository and ship in the
   same bundle, opt-in and off by default; the base game and the faithful modes keep exact physics, timing, AI,
   saves and gameplay read-backs, and with every mod off the runtime behaves exactly as the reference. The mod is
   explicit, labelled, documented with its replay/save effects, and never enabled silently. The button-emulation
   H-shifter front from the first study remains the faithful feature; the direct-gear/clutch mod is the owner's
   opt-in gameplay modification. The implementation can use a derivative ELF/library profile (the TASK-080
   pattern) shipped in that bundle.

---

## 1. Corrections to the first study

### 1.1 The 0x0037AD40 path is `CameraSys::CameraOnBoard`

The first study called this group "the best static candidate for the gear-selection path". It is the onboard
camera:

- The functions are virtual methods of the class whose vtable is `0x0067A5D8`. That table's typeinfo entry is
  `0x005F5B70`, a lazy typeinfo function whose name pointer is `0x0069FAC8`; the string there is
  `Q29CameraSys13CameraOnBoard`. **Confirmed** (ELF data and typeinfo function).
- `func_0037AD40` is slot 38 of that vtable (entry `0x0067A710`, function word at `0x0067A714`); `func_0037A458` is
  slot 9 (entry `0x0067A628`). **Confirmed** (vtable dump).
- `func_0037A458` selects camera transforms by the type returned from vtable slot 23 (`0x37BB20`, which returns
  `+0x180`); the values 0, 1, 20 and 25 are camera types. `func_0037B580` and `func_0037B918` build camera
  matrices and FOV from the Automobile and the car geometry; they are not drivetrain code. **Confirmed** (code
  behavior).
- `func_0037AD40` reads the Automobile only to choose an onboard camera behaviour and uses bytes of the control
  packet as view/look inputs (the exact field names are **Unknown**). The 12-entry table at `0x006211E8` is a
  camera mode/value ring; the `+0x180` field is the current camera mode, not a gear. **Confirmed** for the camera
  ownership; the exact camera table semantics are outside this task.
- The static bytes at `0x006211E8` are not a gear table: with the correct mapping the words are
  `0, 1, 6, 0x42820000 (65.0f), 0x41F00000 (30.0f), 0x40800000 (4.0f), ...`. **Confirmed** (ELF dump). Whether
  the camera code fills the table at startup is **Unknown** and irrelevant to the gearbox.

Consequence: the first study's section 1.4 and its "High confidence" on the gear path are superseded by section 2
of this document. Nothing in the first study's input-path, action-table or wheel conclusions changes.

### 1.2 The HUD setters are called from `RaceDisplay::update`

The first study guessed that `RaceSinglePlayer::updatePlay` (`0x003D5FF8`) updates the panel gear and clutch. It
does not. Its virtual calls on the display object are `RaceDisplay` slots 10, 11, 13 and 14: `setTargetCarNumber`,
`setViewMode`, `setDiveReplayMode` and `setDisplayEnableInReplay` (vtable `0x0067E278`; call offsets
`0x58/0x60/0x70/0x78`). **Confirmed** (disassembly and decomp + RaceDisplay vtable names).

The real HUD update is `RaceDisplay::update` (`0x0039DBB8`):

- At `0x0039DFC8` it loads the organization from `RaceDisplay + 4`, then the car list at `organization + 0x60`
  (`count` at `+0`, item array at `+8`), indexes it with `RaceDisplay + 0x2E` (the player/target car number set by
  `RaceDisplay::setTargetCarNumber`, `0x0039CDF0`) and loads the Automobile from item `+0x18`. **Confirmed**
  (disassembly; matches `RaceBase::initializeDynamicsConductor`).
- At `0x0039E130` it reads `Automobile + 0x624` and calls the panel's `setGear` slot (panel vtable entry at
  `panel_vptr + 0x98`, slot 18). At `0x0039E17C` it calls `func_00343BE0(Automobile)` and passes the result to the
  panel's `setClutch` slot (`+0xA0`, slot 19). **Confirmed** (disassembly).
- `RaceDisplay + 0x14` holds the panel pointer; the panel's own vptr is at `panel + 0x14` (the panel constructors
  store it there). **Confirmed** (`RaceOnboardPanel::structor_1` and the call sequence).

### 1.3 The HUD gear field candidates are copies, not the source

`RaceSimplePanel::setGear` writes `panel + 0x178` and `RaceOnboardPanel::setGear` writes `panel + 0xA74`; these are
the display copies inside `RaceShiftPositionDisplay` (offset `+0x54` of that display). The source value is
`Automobile + 0x624`. **Confirmed**.

---

## 2. Transmission and clutch map

### 2.1 Object layout

The `Automobile` object has a pointer to the engine/gearbox parameter block at `+0x10`, and a large drivetrain
sub-object at `+0x104`. The decomp names the sub-object `Sub`; this document calls it the **drivetrain block**.
Every field below is an Automobile-relative address (the drivetrain-relative offset is shown for cross-checking).

| Automobile offset | Drivetrain offset | Type | Meaning | Evidence | Label |
| --- | --- | --- | --- | --- | --- |
| `+0x010` | - | `Engine *` | engine/gearbox parameter block pointer | `func_00350E28`, `func_0036D818`, `func_00363868` | Confirmed |
| `+0x546` | `+0x442` | `u8` | transmission/control type; `6` = full manual | `Automobile__isFullManualTransmission` `0x003481C8`; selector `func_0036C8E8` | Confirmed |
| `+0x566` | `+0x462` | `s8` | reverse request flag; set when the car rolls backward with no pedals | `func_00348428`; `func_0036C160` | High confidence |
| `+0x567` | `+0x463` | `s8` | shift step (+1 up, -1 down), copied from control packet `+0x02` | `func_003481D8`; control update `0x00349418` at `0x00349C90` | Confirmed |
| `+0x56A` | `+0x466` | `s8` | control mode (1 = player manual path, 4 = special; other values unknown) | `func_00362E00`, `func_0036C8E8`, `func_003535B0` | High confidence |
| `+0x56B` | `+0x467` | `u8` | driver shift-animation state; nonzero forces the manual selector | `func_003452F0/003453A0/00345400`, `func_0036C8E8` | High confidence |
| `+0x5BE` | `+0x4BA` | `u16` | shift/handbrake-path flag; nonzero blocks the shift-request copy | `func_003481D8`, `func_00351EB0`, `func_0036C8E8` | High confidence |
| `+0x5C0` | `+0x4BC` | `u8` | inhibit flag (pit/start/other); nonzero disables the shift assist | `func_003481D8`, `func_00351EB0` | Hypothesis |
| `+0x5EC` | `+0x4E8` | `f32` | engine/clutch inertia term A | `func_003561D0`, `func_00356BB8` | High confidence |
| `+0x5F0` | `+0x4EC` | `f32` | engine/clutch inertia term B | same | High confidence |
| `+0x5F4` | `+0x4F0` | `f32` | torque/ratio helper | `func_003554A0` | High confidence |
| `+0x5FE` | `+0x4FA` | `s16` | engine rpm (integer) | HUD/`func_00363868`; `func_0036D818` | Confirmed |
| `+0x600` | `+0x4FC` | `f32` | engine rpm (float) | `func_003519B8`, `func_00363868` | Confirmed |
| `+0x610` | `+0x50C` | `f32` | rpm window low | `func_003519B8`, `func_00350FB8` | Confirmed |
| `+0x614` | `+0x510` | `f32` | rpm window high | same | Confirmed |
| `+0x618` | `+0x514` | `f32` | clutch engagement (0 = open, 1 = engaged; intermediate possible) | `func_003481D8`, `func_00350FB8`, `func_00351178`, `func_003519B8` | High confidence |
| `+0x61C` | `+0x518` | `f32` | rpm ratio applied when engaged (usually 1.0) | `func_003519B8` | Confirmed |
| `+0x624` | `+0x520` | `u8` | **current gear** (0 = R, 1..count forward) | HUD read `0x0039E130`; selector write `func_0036BEB8` | Confirmed |
| `+0x625` | `+0x521` | `u8` | gear sub-state (`s->b521`; set at init and by the CVT path) | `func_00363560`, `func_003554xx` | Hypothesis |
| `+0x626` | `+0x522` | `u8` | **clutch/shift state**: 0 open, 1 engaged, 2/3 slipping | HUD `func_00343BE0`; `func_003519B8`, `func_0036D818`, `func_00350FB8`, `func_00351178` | Confirmed for values 0/1; High for 2/3 |
| `+0x627` | `+0x523` | `u8` | drivetrain flag set when the gear type is 4..6 or 9 | `func_00363560` | Hypothesis |
| `+0x628` | `+0x524` | `u8[2]` | per-axle drivetrain flags | `func_00363560` | Hypothesis |
| `+0x62A` | `+0x526` | `s8` | shift-delay counter (decrements; nonzero blocks a new shift) | `func_0036C160`, `func_0036C690` | Confirmed |
| `+0x640` | `+0x53C` | `f32` | drivetrain speed (signed) used by the gear logic | `func_0036C160`, `func_00351EB0` | Confirmed |

Engine/gearbox block (via `Automobile + 0x10`):

| Engine offset | Meaning | Evidence | Label |
| --- | --- | --- | --- |
| `+0x032` | drivetrain type byte; `4` blocks writing gear 0 | `func_0036BEB8` | High confidence |
| `+0x200` | maximum rpm (u16) | `func_003519B8` (clamp `maxRpm + 10`) | Confirmed |
| `+0x202` | minimum rpm (u16) | `func_003519B8` | Confirmed |
| `+0x234` | gear/gearbox type byte (0..10 switch) | `func_00350DA0`, `func_0036D818`, `func_00363868` | Confirmed |
| `+0x235` | gear "n" byte | `func_0036C160` state | High confidence |
| `+0x236` | gear count (forward gears) | `func_0036C160`, `func_0036BF68` | Confirmed |
| `+0x246` | gearbox model byte; 1/2/3 select different clutch/force paths | `func_003F1F78/1F88/1F98`, `func_003535B0`, `func_003511A0` | Confirmed for the predicates |
| `+0x274 + gear*4` | gear ratio float, indexed by the current gear (index 0 = R; `func_00350E28` applies a type-2 adjustment) | `func_00350E28` at `0x00350E78`; `func_00363868` | Confirmed |
| `+0x29C` | ratio/scale area used by `func_00363868`; the exact split is not needed for the mod | `func_00363868` | High confidence |

**Important unification:** `Automobile + 0x546` is not a standalone byte; it is the drivetrain block's `+0x442`
(`0x104 + 0x442 = 0x546`). `Automobile::isFullManualTransmission` therefore reads the drivetrain control type.
**Confirmed**.

### 2.2 Player car pointer chain

The HUD code gives a stable chain, confirmed instruction by instruction:

```
RaceDisplay (the display object, RaceDisplay vtable slot 1 = update 0x0039DBB8)
  + 0x04 -> RaceOrganization                      (RaceDisplay::setOrganization writes +0x04)
  + 0x2E -> player/target car number (u8)         (RaceDisplay::setTargetCarNumber writes +0x2E)
RaceOrganization + 0x60 -> Cars { count at +0, item array at +8 }
Cars.items[index] -> CarRecord
CarRecord + 0x18 -> Automobile *
```

Evidence: `RaceDisplay::update` `0x0039DFC8..0x0039E0xx`; `RaceDisplay::setOrganization`; `RaceDisplay::setTargetCarNumber`
`0x0039CDF0`; `RaceBase::initializeDynamicsConductor` (`organization + 0x60`, item `+0x18`). **Confirmed** for
the code shape and the offsets; the class names are from the decomp's RTTI/headers.

What is **Unknown** is how a native mod finds the live `RaceDisplay` object without a guest hook. Two candidate
methods:

1. Scan guest RAM for a vtable-like pointer `V` where `[V + 0x10] == 0x0039DBB8` (RaceDisplay vtable symbol + 8 +
   slot 1 * 8) and `[V + 4]` is a typeinfo function in the `0x005Fxxxx` range; then validate
   `V + 0x04 -> organization -> list -> car`. This mirrors the aspect/widescreen pointer discovery style.
   **Hypothesis** until tested.
2. Find a global that points at the current race/display. `D_006201C8` is set by the battle constructor, but it is
   not confirmed as the generic current race. **Unknown**.

A read-only live probe (section 8) settles the discovery method and the actual player index value. The index is
not assumed to be 0: it is whatever `RaceDisplay + 0x2E` holds, which is also what the HUD uses.

### 2.3 Gear numbering: 0 = R, 1..count forward, no neutral value

- The shift-position display maps the gear byte `0` to the character `R` (`0x52`) and `1..6` to `0x31..0x36`.
  **Confirmed** (`RaceShiftPositionDisplay::render_main` `0x003A6DE0`, char selection at `0x003A6F50..0x003A6F68`).
- At drivetrain init the game writes gear `1` (`func_0036BEB8(self, 1)` in `func_00363560`), so 1 is first gear.
  **Confirmed**.
- The selector treats low/negative speed with gear 0 as reverse and shifts 0 -> 1 when moving forward
  (`func_0036C160`, `func_00351EB0`). **Confirmed**.
- No code writes a neutral gear value. Neutral is the clutch open: drivetrain state `+0x626 == 0` and/or
  engagement `+0x618 == 0`. **Confirmed** for the absence of a neutral value; **High confidence** for the
  neutral-as-open-clutch interpretation.
- The first study's 12-entry table values 20/25 are camera types, not N/R markers. **Confirmed** (section 1.1).

HUD characters: gear 0 -> `R`; forward gear with the automatic-transmission display bit clear -> digit; with the
bit set -> `C`. There is no `N` branch in the display code. The clutch flag (`setClutch`, bit 24 of the display
word) halves the character brightness when clear: `RaceShiftPositionDisplay::render_main` checks `+0x57` and scales
the color. **High confidence**. So the game's own neutral indication is a dimmed gear character, not an `N`.

**Mod consequence:** the mod can use gear 0 for both the N and R gate positions because gear 0 with the clutch open
has no torque; the character shown is selected by a tiny HUD patch (section 5.4). This satisfies the owner's
"R and N must show correctly" without inventing a gear value the physics does not have.

### 2.4 Per-tick order

The physics runs in the EE recompiled code. The relevant order within a frame is:

1. **Control update** `func_00349418`, reached through `func_00349F48`. It reads the `_AutomobileControl` packet:
   steering, throttle/brake, the shift step at packet `+0x02`, and writes the drivetrain flags. It runs before the
   per-car dynamics.
2. **Per-car dynamics loop** `DynamicsConductor` at `0x0034DB10` (GT HD name `processCollision_`; the role here is
   the per-car update loop). For each car it calls the drivetrain tick.
3. **Drivetrain tick** `func_003535B0`:
   - `func_003511A0` selects the clutch state machine: gearbox type 3 -> `func_00351178` (state from engagement),
     otherwise `func_00350FB8` (automatic clutch); `Eng + 0x246 == 4` forces state 1.
   - `func_003518E8`, `func_00352408`, `func_00351FA8` (rigid-body force integration), `func_0036D230`,
     `func_0036D2F8`, `func_0036D818` (engine force), `func_0036E4B8`.
   - Depending on the gearbox predicates, `func_00351EB0` or `func_00351DC8` applies the selected gear.
   - `func_00350ED0`, `func_00352848`, `func_00352C90` finish the step.
4. **HUD update** `RaceDisplay::update` (`0x0039DBB8`) later in the frame, which reads `Automobile + 0x624` and
   `+0x626` and calls the panel setters.

**Confirmed** for the call chain and the order in the code; the exact frame position relative to the guest VBlank
interrupt is **Unknown** and is a target of the live probe.

### 2.5 Gear selection and the shift request

- The control packet `+0x02` is a signed shift step. The control update copies it to `Automobile + 0x567` unless
  `Automobile + 0x5BE` is nonzero (`0x00349C84..0x00349C90`). **Confirmed** for the consumer; the producer is the
  game's platform input driver and is not in the matched decomp (**Unknown** in detail, but it is the shift
  up/down action).
- `func_00351DC8` calls `func_0036C8E8`. The selector chooses a program from `Automobile + 0x546` (drivetrain
  `+0x442`), unless `+0x56A == 1` or `+0x56B != 0`, in which case it uses the manual program (`mode 0`). The jump
  table at `0x0069F840` (11 entries) selects:
  - mode 0 -> `func_0036C160(self, 0)` (manual logic);
  - mode 1 -> `func_0036C160(self, 1)` (automatic logic);
  - mode 2 -> `func_0036C690`;
  - modes 3, 4, 6 -> `func_0036C780`;
  - mode 5 -> `func_0036C850`.
  **Confirmed** (disassembly `0x0036C8E8..0x0036C9B0`).
- `func_0036C160` is the heart of the shift logic:
  - shift delay `+0x62A` counts down; nonzero returns -1 (no shift);
  - `+0x566 != 0` forces reverse (gear 0) or returns -1;
  - speed thresholds 0.2778 m/s (0x1.1C71C6p-2) gate R <-> 1;
  - manual (`automatic == 0`): reads `+0x567` and returns `gear + 1` or `gear - 1`, subject to the rpm limits and
    wheel grip checks;
  - automatic (`automatic == 1`): picks a gear from `func_0036BF68` (speed-based) plus a bias;
  - rpm limits: `func_0036BEE0` (high) and `func_0036BF28` (low) read the engine rpm table; the engine's own
    clamp is `maxRpm + 10`.
  **Confirmed** (decomp).
- `func_00351DC8` applies the target:
  - if the target is -1, it returns;
  - if the clutch state `+0x626` is nonzero, it sets state 2 (target 0 or current gear 0) or 3 (other gearboxes),
    calls `func_0035A5F8` for the driver animation, then calls `func_0036BEB8(self, target)`;
  - `func_0036BEB8` writes `+0x624 = target` (except type `Eng+0x32 == 4` with target 0).
  **Confirmed**.
- The shift *sequence* the owner wants to avoid is therefore the selector's step logic and delay, not the gear
  write itself. The write is a single byte write.

### 2.6 The clutch model

GT4 has an automatic clutch, not a player clutch. The model:

- **State** `Automobile + 0x626` (drivetrain `+0x522`):
  - `0` = open/neutral: `func_003519B8` runs the free-rev branch (engine rpm integrates from torque, clamped to
    min/max); `func_0036D818` skips the engine force block. **Confirmed**.
  - `1` = engaged: `func_003519B8` locks the engine rpm to the wheel-derived rpm; `func_0036D818` treats the
    engine torque as the clutch torque. **Confirmed**.
  - `2`/`3` = slipping/engaging/disengaging: `func_003519B8` computes an rpm window (`+0x610/+0x614`) and
    transitions to 1 when the rpm crosses it (and the engagement is 1); `func_00350FB8` manages the transitions
    from speed, brake and rpm. **High confidence** for 2/3; the exact 2-vs-3 distinction is **Unknown**.
- **Engagement** `Automobile + 0x618` (drivetrain `+0x514`): 0 = open, 1 = engaged; intermediate values are
  produced (`func_003481D8` sets `1 - throttle` in one launch mode). The physics branches on `== 0` and `== 1`;
  the continuous value is mostly a target, not a torque multiplier. **High confidence**.
- **Automatic clutch state machine** `func_00350FB8` (`0x00350FB8`):
  - state 0: if engagement is nonzero, starts slipping (state 2) at high speed with brake, or when the engine rpm
    exceeds a threshold; otherwise opens the clutch;
  - state 1: if engagement becomes 0, returns to state 0; otherwise tracks the target rpm;
  - states 2/3: computes the rpm window and the brake pulse. **Confirmed** (decomp).
- **Type 3 gearbox** `func_00351178`: state = engagement == 0 ? 0 : 1. This is the simplest model and the easiest
  for a mod to drive. **Confirmed**.
- **Torque gate** `func_0036D818` (`0x0036D818`):
  - `lbu v0,0x522(s1); beqz v0, skip engine force` at `0x0036D870`;
  - type 3 has a second read at `0x0036D9AC`;
  - the later state checks at `0x0036DA74` set the clutch torque `f23` (engaged = engine torque, slipping = a
    fraction, open = 0) and the rpm update uses `f21 - f23`. **Confirmed**.
- **Slip factor** `func_003561D0` (`0x003561D0`) computes a 0..1 clutch slip from throttle/brake, rpm and the
  inertia terms `+0x5EC/+0x5F0`; `func_00356BB8` uses it for engine braking/force. `func_00369510` returns zero
  force when engagement `+0x618 == 0`. **Confirmed**.
- The HUD passes `Automobile + 0x626 == 1` to `setClutch` (`func_00343BE0`), which dims/brightens the gear
  character. **Confirmed**.

**Bottom line:** the game has the state and the torque cut; an external pedal can drive the state machine. The
model does not expose a continuous clutch torque to an input, so an analog pedal can feel like a staged
open/slip/engage clutch, not like a continuous friction clutch, unless the force path is patched further.

---

## 3. Direct gear application

### 3.1 What "direct" means here

The owner wants the gear to apply immediately when the gate moves, without the game's upshift/downshift sequence.
The game's sequence is the selector's step logic (`func_0036C160` returns `gear +/- 1`), the shift delay
(`+0x62A`), the clutch slip states, and the driver animation. The physical gear is a single byte
(`Automobile + 0x624`); writing it directly changes the ratio used by the next physics tick.

### 3.2 Fields and ordering

A safe direct change, per physics tick, is:

1. **Open the clutch first.** Write `Automobile + 0x626 = 0` and `Automobile + 0x618 = 0` (and, if the game's own
   control update would overwrite them, use the reader patches in section 5.3). This makes the engine free-rev and
   removes the clutch torque.
2. **Clear the game's shift request path.** Write `Automobile + 0x567 = 0`, `+0x566 = 0` and `+0x62A = 0` so the
   selector returns -1 and does not overwrite the gear. (For gearbox types that use `func_00351EB0`, the selector
   does not read `+0x567`; see section 3.4.)
3. **Write the gear.** `Automobile + 0x624 = gate_gear` (0 for R/N, 1..gear_count for forward). The gear count is
   `Engine + 0x236`.
4. **Engage on release.** When the pedal is released and the gate is stable, let the clutch state go through 3
   (sync) to 1 (engaged). For type 3, 0 -> 1 is enough. The game's rpm window and clamp do the synchronisation.

Reverse/neutral ordering is discussed in section 3.5.

### 3.3 The two delivery routes

**Route A, VBlank guest-RAM writes (probe first).** The host writes the fields above at every guest VBlank, the
same place where `widescreen_hold_at_guest_vblank` writes guest RAM (`native/mtvu_runtime.cpp`
`vblank_sync()`). This needs no guest code change and is the fastest PoC. The risk is ordering: the guest control
update later in the frame can overwrite the clutch fields, and the selector can overwrite the gear for gearbox
types that do not read `+0x567`. The live probe measures the ordering. **Hypothesis** until measured.

**Route B, derivative ELF with a one-instruction patch table (recommended for the mod).** The project already
patches guest code this way for the aspect HUD (TASK-080): a derivative of the reconstructed ELF replaces one
instruction with a load from the host parameter block at guest `0x7F00`, and a separate library profile is built
from it. The same mechanism fits this mod because the host needs to inject only a few scalar values. Candidate rows
(verified against the pinned ELF; the new words are the one-instruction equivalents):

| Address | Old word | New word | Purpose |
| --- | --- | --- | --- |
| `0x003519E8` | `0x92220522` (`lbu v0,0x522(s1)`) | `0x90027F10` (`lbu v0,0x7F10(zero)`) | `func_003519B8` reads the host clutch state (rpm/mode path) |
| `0x0036D870` | `0x92220522` (`lbu v0,0x522(s1)`) | `0x90027F10` (`lbu v0,0x7F10(zero)`) | `func_0036D818` engine-force gate reads the host clutch state |
| `0x0036D9AC` | `0x92220522` (`lbu v0,0x522(s1)`) | `0x90027F10` (`lbu v0,0x7F10(zero)`) | same gate on the gearbox-type-3 branch (only if the car is type 3) |
| `0x00351DE4` | `0x0040882D` (`move s1,v0`) | `0x8C117F14` (`lw s1,0x7F14(zero)`) | `func_00351DC8` uses the host target gear instead of the selector result (type 2/default path) |
| `0x003A6F54` | `0x24060052` (`addiu a2,zero,0x52`) | `0x90067F16` (`lbu a2,0x7F16(zero)`) | HUD gear-0 character becomes host-selected: `R` (`0x52`) or `N` (`0x4E`) |
| `0x00343BE0` | `0x90820626` (`lbu v0,0x626(a0)`) | `0x90027F12` (`lbu v0,0x7F12(zero)`) | optional: HUD clutch dimming follows the pedal instead of the state byte |

Host parameter block (guest `0x7F00`, the TASK-080 block; offsets `0x00` and `0x04` are already used by the
aspect profile, this mod uses `0x10..0x17`):

| Block word | Type | Meaning |
| --- | --- | --- |
| `0x7F10` | `u8` | clutch state seen by the physics (0/1/2/3) |
| `0x7F12` | `u8` | optional HUD clutch value (1 = bright/engaged) |
| `0x7F14` | `s32` | target gear (0 = R/N, 1..count) |
| `0x7F16` | `u8` | HUD character for gear 0 (`0x52` = R, `0x4E` = N) |

The table is a **candidate**: it has not been built or tested. The old words are verified in the pinned ELF; each
new word is a one-instruction load from the block. The derivative ELF, producer and library manifest must be new
slice directories with their own pins, exactly like the aspect profile. The base ELF and the faithful build are
not touched. **High confidence** for the mechanism; **Hypothesis** for the final word choices until the mod build
runs.

The `0x00351DE4` row covers the `func_00351DC8` selector path. Gearbox types that use `func_00351EB0` (the
speed-based selector) do not go through it; for those, either a small code-cave stub at the `func_00351EB0` call
site (`0x00351F74`) or a VBlank gear write is needed. The reference car's gearbox type is read by the live probe
(`Engine + 0x246` and `Engine + 0x234`). **High confidence** for the dependency; **Unknown** for the reference
car's type until measured.

### 3.4 Failure modes

- **Gear changed with the clutch engaged.** The next tick uses the new ratio with the old engine rpm; the clutch
  torque or the rpm sync can spike, lock a wheel or jolt the car. Always open the clutch first.
- **Selector overwrite.** If `+0x567`/`+0x566`/`+0x62A` are not cleared, the game can shift the gear back on the
  next tick (or refuse, depending on the delays). The HUD can then flicker between two gears.
- **N at speed with the selector active.** Gear 0 with positive speed makes `func_0036C160` return 1 even with no
  shift request; a VBlank-only N is overwritten to 1 above 0.2778 m/s. The target-gear patch (or a selector
  bypass) is required to hold N while the car is moving. R is only used at standstill, so it does not hit this.
- **Gear above the car's count.** `Engine + 0x274 + gear*4` reads an out-of-range ratio; the rpm and force become
  garbage. Clamp to `Engine + 0x236`.
- **Reverse at speed.** Gear 0 is the reverse ratio. Engaging it forward is a sign reversal; enforce `|speed| <
  0.2778 m/s` for R.
- **Automatic transmission.** The auto selector (`func_0036C160` with `automatic == 1`, or a car whose control type
  is not 6) recomputes a target every tick and fights the direct write. Detect MT (`Automobile + 0x546 == 6` and
  the selector path) and refuse on AT cars.
- **Wrong frame point.** A VBlank write can be overwritten by the control update before the drivetrain tick; the
  symptom is a gear or clutch that changes and immediately reverts. Use the reader patches or measure the order.
- **Engine stall / over-rev.** The game clamps rpm at `maxRpm + 10` and `minRpm`, but a direct downshift at high
  speed can still produce a large rpm error for one tick. Reject changes that would exceed the limit, or hold the
  clutch open until the player's speed is plausible for the target gear.
- **Stuck slip state.** Forcing state 2/3 every tick prevents the transition to 1; mirror the game's real state
  back to the block when the pedal is released.
- **HUD mismatch.** The HUD reads `+0x624/+0x626`; writing a different field (for example the panel copy) moves the
  car but not the display, or the reverse. Write the Automobile fields.

### 3.5 Reverse, neutral and the gate sequence

The owner's gate sequence is `R, N, 1, 2, 3, 4, 5, 6`, and downshifting past 1 must go through N and then R. The
game has no neutral gear value, so the mod maps both N and R to gear 0:

| Gate | Gear field `+0x624` | Clutch state `+0x626` | HUD char `0x7F16` | Rules |
| --- | --- | --- | --- | --- |
| R | 0 | 1 only when stopped, otherwise 0 | `R` (`0x52`) | only when `abs(speed) < 0.2778 m/s`; reverse lock respected |
| N | 0 | 0 (open) | `N` (`0x4E`) | always allowed; engine free-revs; car coasts |
| 1..6 | 1..count | 0 while the gate moves or the pedal is pressed; 3 -> 1 on release | not used (digit path) | clamp to `Engine + 0x236`; rev limit respected |

This gives correct HUD R/N through the `0x003A6F54` patch, because gear 0 takes the `R` branch and the patched
load supplies `N` or `R`. The gear-0 reverse ratio is harmless in N because the clutch is open; on N -> R at
standstill the clutch can engage. On N -> 1 the mod writes gear 1 while the clutch is open, then engages. The gate
debounce (about 50 ms) prevents a flicker between N and R while the lever crosses the gate. **High confidence** for
the mapping; the visual N glyph is tested in the owner session.

The gear-0 N position must be held by the target-gear patch when the car is moving: the manual selector returns 1
for gear 0 above 0.2778 m/s even with no shift request. The VBlank-only probe can test N at standstill, but the
final mod uses the patch (or a selector bypass) for N at speed.

The game's own reverse rules remain in charge where they apply: the selector's 0.2778 m/s threshold, the
`maxRpm + 10` clamp, and the reverse lock logic in `func_0036C160`. The mod adds the same speed gate before writing
R, so a direct write cannot bypass it.

---

## 4. Clutch input and physics

### 4.1 Does GT4 have a clutch the mod can drive?

Yes, as an automatic clutch model (section 2.6). The player has no clutch action and no clutch channel; the game
computes the state from throttle, brake, speed and rpm. The mod can:

- drive the **state** (`+0x626`) between 0 and 1/2/3, which cuts or applies torque;
- drive the **engagement** (`+0x618`) between 0 and 1, which the rpm sync and some force gates read;
- rely on the game's rpm window, clamp and slip logic for the engagement transient.

That is a real clutch in the game's physics, not a button mapping. **High confidence**.

### 4.2 What an analog pedal can and cannot do

- **Can:** open the clutch instantly (torque cut, engine free-rev), command a slipping engagement, and release into
  the game's sync. It can also gate the gear change (the owner must press the pedal to change gear, or the mod
  auto-cuts for a short time as an assist).
- **Cannot, without a deeper patch:** continuously vary the clutch torque with pedal travel. The physics path
  checks the state as 0/1 and the engagement as 0/1; the intermediate float is not multiplied into the engine force
  in `func_0036D818`. A continuous feel would require a code-cave patch that scales the clutch torque `f23` by
  `(1 - pedal)` (or scales the engine force before it reaches the wheels). That is a later, higher-risk step.
  **High confidence** for the limitation; **Hypothesis** for the force-scale patch.
- **Launch feel:** with the state-based model, a standing start is clutch-drop or a pulsed slip, not a smooth
  friction launch. The owner should test this before the final design; if a smooth launch is required, the
  force-scale patch becomes necessary.

### 4.3 Input plumbing from SDL3 to the guest

Current path (main): `native/host_io.cpp` reads SDL3 (or XInput) and builds a `GamepadReading`; `pad_mapping`
produces a `PadState`; `window.cpp` calls `ps2::set_pad_buttons/axis/pressure`; the donor SIO2 HLE stores it; the
game polls SIO2. There is **no clutch channel** in that path. **Confirmed** (code and the first study).

The mod needs a separate input source, not a pad mapping:

1. **Read the pedal before mapping.** In `host_io.cpp` (SDL3 gamepad) and in the wheel HID path
   (`native/wheel.cpp`, `native/usb_dfp.cpp` on the rc15 wheel branch), expose the clutch axis as a `0..1` value.
   The launcher already has a `[clutch]` live-view axis in the wheel work; reuse that source. The G27's exact HID
   report is **Hypothesis** until the owner's device is captured; SDL3 axis enumeration makes a first PoC possible
   without knowing the report.
2. **Feed the mod layer, not the guest pad.** The mod layer (`native/hshifter_mod.*`) receives
   `{gate, clutch, now_ms}` and owns the gate map, debounce and clutch state machine.
3. **Reach the guest.** Either write the Automobile fields at VBlank (Route A) or write the `0x7F00` block and use
   the derivative ELF patch table (Route B). No new SIO2 device, no packet extension and no new guest input action
   is needed. **High confidence**.

The `_AutomobileControl` packet is 0x20 bytes; its fields are `+0x00` flags, `+0x01`, `+0x02` shift step, `+0x03`
data count, `+0x04` steering, `+0x06`, `+0x07`, `+0x08/+0x0A/+0x0C/+0x0E` analog values, `+0x10..+0x1F` digital
data (`AutomobileControl::clearPacket`, `RaceInput::getAutomobileControl`). There is no spare clutch field that
the game reads; adding one would require patching the guest input consumer anyway. **Confirmed** for the layout;
the exact names of the analog fields beyond steering/step are **Unknown**.

### 4.4 Replays and saves

- The replay/ghost recorder stores the 0x20-byte control packet (`AutomobileControlRecord::Recorder::write/read`).
  It records the shift step, not the gear or the clutch. A mod lap cannot be replayed faithfully. **Confirmed**.
- The mod must be **disabled during replay, ghost and autodemo**. Otherwise the target-gear patch would override
  the recorded shifts and the clutch patch would override the replay's state. The exact replay-mode flag is
  **Unknown**; the mod should fail safe (no writes) when the race mode is not a live player session, and the live
  probe should log the flag.
- Saves: the live race state is not part of the garage/options save. The mod must not write car data, the
  transmission type (`+0x546`) or the engine block. With that rule, saves are unaffected. **High confidence**;
  verify with a save/load in the owner session.
- A mod lap in Time Trial would have a ghost/replay that does not contain the mod inputs; the mod should either
  refuse to record a best lap or mark the session as modded in its own UI. That is an owner decision.

---

## 5. Mod design and proof-of-concept plan

### 5.1 Boundary and project rule

- **Owner decision 0073 (2026-11-11) governs this work.** Gameplay mods are in scope in this repository and ship in
  the same bundle. They are opt-in and off by default. The base game and the faithful modes (GSdx, CoreGS 2.0,
  CoreGS 3.0 without mods) keep exact physics, timing, AI, saves and gameplay-relevant read-backs; with every mod
  off the runtime behaves exactly as the reference. Each mod states what it changes and its replay/save
  consequences; no mod is enabled silently.
- The implementation can be a **derivative ELF/library profile** (the TASK-080 pattern) shipped in that bundle:
  a new producer, a new patched ELF, a new `docs/inputs` library pin and a new slice directory, selected by an
  explicit mod switch. The base ELF, the base library and the base pins are not edited and the faithful modes are
  unchanged. If the patch table cannot be made off-state equivalent, the mod becomes a separate executable profile,
  still in the bundle and still off by default.
- The mod is off unless the owner enables it (launcher setting or environment variable). With the mod off, the
  base behavior is bit-identical to the current build. The acceptance includes a frame-dump comparison with the
  mod off.
- The button-emulation H-shifter from the first study remains the faithful feature. The mod is the owner's
  gameplay modification, not a replacement for the faithful path.

### 5.2 Module boundary

Suggested native modules (names are proposals):

- `native/hshifter_mod.{hpp,cpp}`: pure logic. Gate enum (`R, N, 1..6`), debounce, target gear, clutch state
  machine, reverse lock, gear-clamp and rev-limit checks. No SDL, no Windows, no guest access. Unit-testable.
- `native/hshifter_guest.{hpp,cpp}`: guest access. Finds the player `Automobile` via the `RaceDisplay` chain (or a
  validated scan), reads the fields, writes the `0x7F00` block and/or the Automobile fields at the chosen point.
  Every pointer and range is validated against the 32 MiB guest RAM, in the style of `native/race_widescreen.cpp`.
  Fails closed (no writes) when the chain or the mode is unknown.
- `native/hshifter_input.{hpp,cpp}` or an extension of `host_io`/`wheel`: exposes the pedal axis and the gate
  buttons from SDL3/HID. The launcher binding UI can reuse the wheel capture.
- `scripts/hshifter_elf_patch.py`: the patch table and verification, modeled on `scripts/aspect_elf_patch.py`
  (check old words, produce the derivative ELF, record the library inventory). New slice directories and new pins;
  never edit an existing manifest.
- Tests: `tests/python/test_hshifter_mod.py` (pure logic and patch table), a headless scripted run that reads back
  the fields, and a frame-dump equivalence test for the mod-off build.

### 5.3 Guest write/call points

- **Clutch state and engagement:** the `0x7F10` block byte is read by the patched `func_003519B8` and
  `func_0036D818` loads. The host mirrors the game's real `+0x626` when the pedal is released and writes 0 while
  the pedal is pressed; on release it writes 3 (sync) and then 1 when the game's real state becomes 1. For type 3,
  0 -> 1 is enough. `+0x618` can be written at VBlank for the force gates that read it, or left to the game if the
  state path is sufficient.
- **Gear:** the `0x7F14` block word is read by the patched `func_00351DC8` load (type 2/default). For type 1/3,
  a code-cave stub at the `func_00351EB0` call or a VBlank write is the fallback. The host writes the gate gear
  (`0` for R/N, `1..count` for forward) and always a valid value; there is no "no change" sentinel.
- **Shift-request suppression:** in the VBlank route, write `+0x567 = 0`, `+0x566 = 0`, `+0x62A = 0`. In the patch
  route, the target-gear patch bypasses the selector result; the VBlank clears are still useful for the type 1/3
  fallback.
- **HUD:** the `0x7F16` block byte is read by the patched `RaceShiftPositionDisplay::render_main` delay slot when
  the gear is 0; the host writes `R` (`0x52`) or `N` (`0x4E`). The optional `0x7F12` byte drives the HUD clutch
  dimming from the pedal.
- **Ordering:** open the clutch, write the gear, then engage. The patch route makes the order deterministic inside
  the tick; the VBlank route relies on the frame order measured by the probe.

### 5.4 HUD

- Gear and clutch: the game's own panel setters run from `RaceDisplay::update`; writing `+0x624/+0x626` makes the
  HUD follow automatically. **Confirmed**.
- R/N: the `0x003A6F54` patch makes the gear-0 character host-selected, so R and N both display correctly while
  the physics uses gear 0 with the clutch open/closed. **High confidence**; the font must render `N` (ASCII
  0x4E), which is a visual check.
- Clutch pedal bar: the game's indicator is binary (bright/dim). An analog bar needs a native overlay; that is
  optional and outside the first PoC.

### 5.5 Acceptance checks

1. **Direct gear.** With the clutch pressed, moving the gate from 2 to 4 changes `Automobile + 0x624` from 2 to 4
   within one physics tick (<= 16.7 ms); the HUD shows 4; the shift step `+0x567` and delay `+0x62A` stay 0; no
   intermediate 3 is applied.
2. **Clutch torque cut.** In 4th at about 50 km/h, pressing the pedal with the throttle closed makes the engine rpm
   fall to the free-rev/idle range and the car decelerate without engine braking; with the throttle open in N the
   rpm rises but the speed does not. The F8 probe or a speed/rpm log shows the difference against a clutch-engaged
   run.
3. **Engagement.** Releasing the pedal moves `+0x626` 0 -> 3 -> 1, the rpm syncs to the wheel speed, there is no
   spike above `maxRpm + 10`, and no wheel locks.
4. **R.** At standstill, R engages and the car reverses; HUD shows R. At about 10 km/h, R is refused (log) and the
   clutch stays open. The game's reverse lock is not bypassed.
5. **N and the gate sequence.** The full sequence R, N, 1, 2, 3, 4, 5, 6 works with the G27; 1 -> N -> R passes
   through N; N coasts with the engine free-revving; HUD shows N and R.
6. **AT car.** The mod detects the automatic and refuses; no gear writes, no fight with the auto selector.
7. **Replay.** A base lap recorded with the mod off replays identically with the mod on (mod disabled); a mod lap
   is not presented as a faithful replay.
8. **Saves.** Save and load after a mod session leaves the garage, options and car data unchanged.
9. **Headless.** A scripted gate/pedal sequence produces the expected field transitions; the mod-off run's frame
   dumps equal the base build's.

### 5.6 Owner's G27 test

Reference scene: Time Trial, Nordschleife, Toyota Supra Wood One, in-game video 480p. Test:

- each gate 1..6 from the current gear, standing and moving;
- the R, N, 1 sequence at standstill and the 1 -> N -> R sequence;
- a launch with the pedal held and released (state-based feel, and whether a smoother launch is required);
- a clutch-pressed upshift and downshift at speed, including a skip (2 -> 5, 5 -> 2) with the pedal;
- an AT car (refusal);
- a replay/ghost (mod disabled);
- save/load and a card backup check;
- force feedback, paddles and the existing wheel features unchanged.

### 5.7 Effort and risk

| Item | Estimate | Confidence |
| --- | --- | --- |
| This deep study | done | - |
| Live read-only probe (fields, index, type, ordering, N glyph) | 2-3 days | High |
| Pure gate/clutch state machine + unit tests | 3-5 days | High |
| HUD R/N patch + host block | 3-5 days | High |
| Direct gear patch (type 2/default) | about 1 week | Medium |
| Type 1/3 selector stub | 3-5 days | Medium |
| Host input (G27 gate/axis) and launcher binding | 1-2 weeks | Medium |
| Clutch state machine tuning and owner test | 1-2 weeks | Medium |
| Continuous torque patch (optional) | 1-2 weeks | Low |
| **PoC total** | **4-6 weeks** | **Medium** |

Risks: invalid physics states if the ordering is wrong (jolt, over-rev, stall); the state-based clutch may not
feel analog enough; replays cannot be faithful and the mod must disable itself; the reference car's gearbox type
decides which selector patch applies; the HUD `N` glyph is a visual unknown; the mod build and its pins are new
release artifacts and need the owner's approval. The base game and the faithful build are not changed.

---

## 6. Evidence table

| Claim | Evidence | Label |
| --- | --- | --- |
| `func_0037AD40` et al. are `CameraSys::CameraOnBoard`, not the gearbox | vtable `0x0067A5D8`, typeinfo `0x005F5B70`, name string `0x0069FAC8` = `Q29CameraSys13CameraOnBoard`; slot 38 = `0x0037AD40`, slot 9 = `0x0037A458` | Confirmed |
| The first study's HUD setter caller was `RaceDisplay` slots 10/11/13/14, not gear/clutch | `RaceSinglePlayer::updatePlay` `0x003D5FF8`; RaceDisplay vtable `0x0067E278`; decomp names | Confirmed |
| HUD gear = `Automobile + 0x624`; HUD clutch = `Automobile + 0x626 == 1` | `RaceDisplay::update` `0x0039DBB8`, reads at `0x0039E130` and `0x0039E17C`; `func_00343BE0` | Confirmed |
| Player car chain | `RaceDisplay + 4 -> organization + 0x60 -> Cars + 8 -> item + 0x18 -> Automobile`; index `RaceDisplay + 0x2E` | Confirmed (code shape) |
| Gear field = `Automobile + 0x624` = drivetrain `+0x520`; clutch state = `+0x626` = drivetrain `+0x522`; engagement = `+0x618` = drivetrain `+0x514` | `func_00351DC8`, `func_003519B8`, `func_0036D818`, `func_00350FB8`, `func_0036BEB8`; HUD | Confirmed |
| Gear numbering 0 = R, 1..count forward; no neutral value | HUD `R` branch at `0x003A6F50`; init gear 1 in `func_00363560`; selector `func_0036C160`/`func_00351EB0` | Confirmed |
| Reverse/neutral are gear 0 + clutch state; HUD N glyph absent, clutch dims the character | `RaceShiftPositionDisplay::render_main`; `setClutch` bit 24 | Confirmed / High |
| Shift step comes from control packet `+0x02` into `Automobile + 0x567` | `func_00349418` at `0x00349C84..0x00349C90`; `func_003481D8` | Confirmed |
| Manual/auto selector and target gear | `func_0036C8E8`, jump table `0x0069F840`, `func_0036C160`, `func_00351DC8`, `func_0036BEB8` | Confirmed |
| Clutch state machine and torque gate | `func_00350FB8`, `func_00351178`, `func_003519B8`, `func_0036D818` (gate at `0x0036D870`) | Confirmed |
| Engine rpm clamp `maxRpm + 10`, min rpm | `func_003519B8`; `Engine + 0x200/+0x202` | Confirmed |
| Gear ratios at `Engine + 0x274 + gear*4`; count at `Engine + 0x236`; type at `Engine + 0x246` | `func_00350E28` at `0x00350E78`; `func_0036C160`; `func_003F1F78/1F88/1F98` | Confirmed |
| No clutch action in the guest action table; no clutch channel in the 0x20-byte packet | action strings at `0x006A5E30..`; `AutomobileControl::clearPacket`; `RaceInput::getAutomobileControl` | Confirmed |
| Replay recorder stores the control packet, not gear/clutch | `AutomobileControlRecord::Recorder::write/read` | Confirmed |
| VBlank guest-RAM write and `0x7F00` host block precedents | `native/mtvu_runtime.cpp` `vblank_sync`; `native/race_widescreen.cpp`; `scripts/aspect_elf_patch.py` | Confirmed |
| Patch candidate words | old words read from the pinned ELF at `0x003519E8`, `0x0036D870`, `0x0036D9AC`, `0x00351DE4`, `0x003A6F54`, `0x00343BE0`; each new word is a one-instruction load from `0x7F00` | High confidence (not built) |
| Analog clutch torque is state-based, not continuous | `func_0036D818` branches on state; `func_00369510` gates on engagement | High confidence |

---

## 7. Open questions and targeted probes

1. **Reference car type and count.** Launch the reference scene and read `Automobile + 0x10 -> Engine + 0x234`,
   `+0x236`, `+0x246` at VBlank. This decides whether the `func_00351DC8` patch or the `func_00351EB0` stub is
   needed.
2. **Player index value.** Read `RaceDisplay + 0x2E` and validate the organization chain in Time Trial, a race,
   split screen and a replay. The HUD already uses this value; the mod should use the same.
3. **RaceDisplay discovery.** Test the vtable-scan method (slot 1 = `0x0039DBB8`) and a possible global pointer.
   This is the only Unknown in the pointer chain.
4. **VBlank ordering.** Log `+0x624`, `+0x626`, `+0x618`, `+0x567`, `+0x62A` at VBlank and again after the
   drivetrain tick (or at the HUD update). Measure whether a VBlank write survives to the tick. If not, use the
   reader patches.
5. **N glyph.** Apply the `0x003A6F54` patch in a test build, write `0x4E`, and check the font. If the font does not
   render `N`, use an overlay or a different glyph.
6. **Analog feel.** Test the state-based clutch with the owner; if a smooth launch is required, prototype the
   engine-force scale by `(1 - pedal)` in a code cave and measure.
7. **Replay flag.** Log the `RaceInput` run mode and the replay/ghost paths to find a reliable mod-off condition.
8. **G27 report.** Capture the HID report for the gates and the clutch axis; the SDL3 axis path is a first PoC, the
   native HID path is the final one.
9. **AT behavior.** Test an AT car; confirm the detection (`+0x546`, selector path) and that the mod writes nothing.
10. **Off-state equivalence.** With the patch table and the host block mirroring the original values, compare frame
    dumps against the base build. The mod build must be a separate profile; the base build must stay identical.

---

## 8. Files and references

- Owner decision 0073, `docs/decisions/0073-mod-support-in-bundle.md` (branch `docs/0073-mod-policy`, commit
  `495eecc1`, 2026-11-11; not in this worktree's base commit): gameplay mods are in scope, opt-in and off by
  default, shipped in the same bundle; the faithful modes keep exact physics, timing, AI, saves and read-backs.
- First study: [H-SHIFTER-CLUTCH-STUDY.md](H-SHIFTER-CLUTCH-STUDY.md) (input path, action table, DFP report,
  button-emulation front; sections 1.4/1.5 and 2.2 are superseded by this document).
- Decomp (read-only): `src/race/unit_00347A00/` (`func_003481D8`, `func_00348368`, `func_00348428`,
  `func_003484D0`, `func_00348588`, `Automobile__isFullManualTransmission`); `src/race/unit_0034E9A0/`
  (`func_00350FB8`, `func_00351178`, `func_003519B8`, `func_00351DC8`, `func_00351EB0`, `func_003535B0`,
  `func_00363868`, `func_003554A0`, `func_00355F80`, `func_003561D0`, `func_00356BB8`); `src/race/unit_00364818/`
  (`func_0036BEB8`, `func_0036C160`, `func_0036C690`, `func_0036C8E8`, `func_0036C9B8`); `src/race/unit_00343220/`
  (`func_00343BE0`, `func_003452F0`, `func_00345400`); `src/race/RaceDisplay/` (`RaceDisplay__update`,
  `setTargetCarNumber`, `setOrganization`); `src/race/RaceShiftPositionDisplay/` (`render_main`, `update`,
  `structor_1`); `src/race/RaceSimplePanel/`, `src/race/RaceOnboardPanel/` (setters and constructors).
- Guest hooks: `native/mtvu_runtime.cpp` `vblank_sync()`; `native/race_widescreen.{hpp,cpp}`;
  `scripts/aspect_elf_patch.py`; `native/display_aspect.hpp` (`kHudParameterBlock = 0x7F00`).
- Wheel/input: `native/host_io.cpp`, `native/pad_mapping.*`, `native/window.cpp`, `native/wheel.cpp`,
  `native/usb_dfp.cpp` (rc15 wheel branch), `docs/WHEEL.md`, `docs/WHEEL-NATIVE-DFP.md`, `docs/INPUT-SDL3.md`.
- ELF: `private/reconstructed/SCUS_973.28.elf`, sha256 `10f82e22...c7c935c`; file offset = guest - `0x000FF000`.

---

## 9. Confidence summary

- **Confirmed:** the camera correction; the HUD gear/clutch source; the player car pointer chain; the gear field
  and numbering; the clutch state/engagement fields; the shift step path; the manual/auto selector; the engine
  force gate; the rpm clamp; the absence of a clutch input; the replay recorder; the VBlank/parameter-block
  precedents.
- **High confidence:** drivetrain field names; the clutch state machine semantics for 2/3; neutral = clutch open;
  the HUD dimming as the clutch indication; the patch mechanism; the type-dependent selector path; the state-based
  analog limitation.
- **Hypothesis:** the RaceDisplay vtable-scan discovery; the exact VBlank ordering; the G27 HID report; the
  force-scale patch for a continuous clutch; the reference car's gearbox type.
- **Unknown:** the player index value until measured; the N glyph rendering; the exact replay-mode flag; the
  detailed producer of the control packet's shift byte (only its consumer is confirmed); the camera table's
  runtime initialisation (irrelevant here).
