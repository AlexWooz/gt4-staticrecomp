# H-shifter and clutch in GT4: feasibility study (TASK-104)

Date: 2026-11-11. Author: research agent. Read-only study: no game code, build, pin, save or card was changed.
Scope: owner request of 2026-11-11 (Telegram). Two fronts: (1) map each physical H-shifter position (1-6, R, and
N) to the game's gear, and (2) find out whether GT4 has a clutch concept that an analog clutch pedal could drive.
This is a study for TASK-104; it does not implement anything.

Labels: **Confirmed** (read in the ELF, the decomp at identical addresses, our code, or measured), **High
confidence** (strong code or document evidence, not yet tested in game), **Hypothesis** (plausible, untested),
**Unknown** (not established).

Inputs used (read-only):

- This worktree, branch `task/104-hshifter-study`, based on `main` commit `85718831` (2026-10-24).
- Reconstructed ELF `private/reconstructed/SCUS_973.28.elf`, sha256
  `10f82e2231a51404b95682ed3ea81171100a1af2fefdeed3391943016c7c935c` (the study's `docs/ULTRAWIDE-NATIVE-STUDY.md`
  records the same hash). Its text segment matches the pinned disc and the decomp at the same addresses
  (`docs/DECOMP-IDENTITY.md`).
- Decomp `private/slice151-research/gt4-decomp` at commit `e0b70c4d6` (names, types and addresses; the reference
  `docs/DECOMP-INTEGRATION.md` describes the address identity).
- Current main input path: `native/host_io.cpp`, `native/pad_mapping.{hpp,cpp}`, `native/controls.{hpp,cpp}`,
  `native/window.cpp`, and the donor runtime under
  `private/references/ps2-web-recomp/runtime/src/` (pinned MIT donor).
- Wheel work on the 1.3.0-rc15 line, not merged into `main` at `85718831`: branch `task/121-wheel-integration`
  (`docs/WHEEL.md`), branch `task/123-dfp-wheel-study` (`docs/tasks/TASK-123-DFP-WHEEL-RESEARCH.md`), branch
  `task/192-ffb-centering` (`docs/WHEEL-NATIVE-DFP.md`, `native/usb_dfp.cpp`, `native/wheel.cpp`), and branch
  `release/1.3.0-rc15`. These are cited as the planned native wheel path, not as main.
- No PCSX2 source, `gt4recomp-gs` fork or `private/tooling/pcsx2-*` was opened. This study is not PCSX2-tainted.

---

## Summary

1. **GT4 already has a sequential gearbox.** The game's input actions are shift up, shift down and reverse gear;
   there is no clutch action and no H-pattern/gate action in the action table. The supported wheel (Logitech
   Driving Force Pro, DFP) has a sequential shifter and no clutch channel. **Confirmed** for the action table and
   the DFP report; **High confidence** that no supported GT4 wheel exposes an H-gate or clutch to the game.
2. **The HUD has a clutch indicator**, and the panel classes have `setClutch(bool)`, `setGear(int)`,
   `setAutomaticTransmission(bool)` and `setShiftTimingLamp(bool)`. The clutch flag is a real game concept at the
   display layer. Its source, and whether the drivetrain physics models clutch torque, are **Unknown**: the
   decompiled code around the panels does not contain the physics model, and the decomp's matched sources have no
   clutch symbol outside those setters.
3. **The recommended H-shifter approach is button emulation**: translate the physical gate into the game's own
   shift-up/shift-down commands, one step at a time, with the game's own timing and rules. This is a small native
   input feature and does not touch guest memory. It is the approach that matches the owner's "it is not hard"
   expectation.
4. **Direct gear writes are a mod, not an input feature.** They require the player car pointer and the gear state
   field (candidates exist, ownership is not confirmed), they bypass the game's shift delay, gear lock and rev
   limiter, and they are not replayed from controller input. They should be a later, gated investigation, not the
   first implementation.
5. **A real clutch pedal is not an input mapping problem.** The game has no clutch input and the DFP protocol has
   no clutch channel. A clutch that disengages torque and completes the shift on release is a drivetrain physics
   mod, with unknown feasibility from the current decompiled coverage. The clutch pedal can be display-only (as
   the rc15 wheel work already does) or a host-side assist; neither is a real clutch.

---

## 1. What the game already has

### 1.1 The input path, host to guest

Current main (`GT4_INPUT=sdl` default, `xinput` fallback):

1. `native/host_io.cpp` reads the SDL3 gamepad (or XInput) and builds a `gt4::pad::GamepadReading`.
2. `native/pad_mapping.cpp` maps it through the controls table to a `gt4::pad::PadState` (16 button bits,
   4 axes, 16 pressures). `docs/INPUT-SDL3.md` and `docs/CONTROLS.md` describe the mapping.
3. `native/window.cpp` `apply_pad()` calls `ps2::set_pad_buttons`, `ps2::set_pad_axis` and
   `ps2::set_pad_pressure`.
4. The donor runtime `private/references/ps2-web-recomp/runtime/src/system.cpp` forwards to
   `ps2::iop::sio2::set_buttons` (and the axis/pressure setters). `runtime/src/iop/sio2.cpp` stores the state in
   `g_pads` / `g_feel`; the game polls the emulated SIO2 registers and builds its own pad state. **Confirmed**
   (code).
5. There is no host write to guest RAM in this path. The only input reaching the game is the SIO2 device state.

Guest side (decomp at identical addresses):

- `RaceBase::getInput` (0x00389628) returns the `RaceInput` singleton at guest `0x00622B60`. **Confirmed** from
  `src/race/RaceBase/RaceBase__getInput.cpp`, which returns `&D_00622B60`.
- `RaceInput::getButtons` (0x00426A00) returns the 32-bit word at `RaceInput + 0x188`, i.e. guest
  `0x00622CE8`. **Confirmed** from `src/race/unit_00426990/RaceInput__getButtons.cpp`.
- `RaceInput::getButtonDown` (0x00426A08) returns the rising-edge buttons (new presses only), and
  `RaceInput::getExtendButtonDown` (0x00426B60) returns the second button word. **Confirmed** from the decomp and
  `include/gt4/RaceInput.h`.
- `RaceInput::getAutomobileControl` (0x00426D50) produces the 0x20-byte `_AutomobileControl` packet used by the
  control/recording path. **Confirmed** from `src/race/unit_00426990/RaceInput__getAutomobileControl.cpp`.
- The packet layout is visible in `AutomobileControl__clearPacket` (0x003557E8) and the replay recorder
  (`AutomobileControlRecord__Recorder__write` / `__read`): `+0x00` and `+0x01` bytes, `+0x02` signed byte,
  `+0x03` count, `+0x04` signed 16-bit steering, `+0x07` byte, `+0x08`, `+0x0A`, `+0x0C`, `+0x0E` signed 16-bit
  values, `+0x10..+0x1F` the digital data bytes. **Confirmed** for the layout; the semantic names of the fields
  beyond steering/count are **Unknown** (the decomp does not name them).
- The recorder stores the packet for replays, so **button presses are recorded and replayed**. A gear change made
  by pressing the game's own shift button is therefore reproducible in a replay; a direct gear write would not be
  in the recorded data. **Confirmed** (recorder code), and a key argument for button emulation.

Read-only probe available for a proof of concept: read `0x00622B60 + 0x188` at VBlank and compare it with the
button the host pressed. This proves the game's own input layer sees the press without writing anything.
**Confirmed** by the addresses above.

### 1.2 The game's gear actions and the supported wheels

The ELF string table at guest `0x6A5EA0` (file offset `0x5A6EA0`) contains the input action names. The sequence
is, in order: `ANALOG_SHIFTUP`, `ANALOG_ACCEL`, `ANALOG_BRAKE`, `ANALOG_SIDEBRAKE`, `ANALOG_REVERSEGEAR`,
`BUTTON_LSTEER`, `BUTTON_RSTEER`, `BUTTON_SHIFTDOWN`, `BUTTON_SHIFTUP`, `BUTTON_ACCEL`, `BUTTON_BRAKE`,
`BUTTON_SIDEBRAKE`, `BUTTON_REVERSEGEAR`, `BUTTON_SPECIAL`, `BUTTON_PLAYVIEW`, `BUTTON_BACKVIEW`,
`BUTTON_FRONTVIEW`, `BUTTON_REARVIEW`, ... **Confirmed** (ASCII strings in the reconstructed ELF).

There is **no input action name containing "clutch"** and **no action name for an H-gate, H-shifter or
shifter position**. The only `CLUTCH` string in the ELF is the car equipment part name at guest `0x6A6B40`
(`INTERCOOLER`, `MUFFLER`, `CLUTCH`, `FLYWHEEL`, `PROPELLERSHAFT`, `LSD`, ...). **Confirmed** for the string
table; a runtime-constructed action with a different name is **Unknown**.

The game supports the Logitech Driving Force Pro (DFP) class of wheels. The rc15 native wheel work (TASK-181 /
TASK-192, branch `task/192-ffb-centering`, not in main at `85718831`) presents an 8-byte DFP input report:
14-bit steering, 14 buttons, a 4-bit hat, a throttle byte and a brake byte. `native/usb_dfp.cpp` encodes exactly
these fields; there is **no clutch axis and no H-gate field**. **Confirmed** from the branch source.
`docs/WHEEL-NATIVE-DFP.md` states the same: the DFP has no clutch, and `[clutch]` is only for the launcher's live
view.

`docs/WHEEL.md` (TASK-121 branch) states: "The clutch is display-only: GT4's existing gamepad input has no
manual-clutch signal. H-pattern hardware can bind existing gamepad actions; direct gear selection is not added."
**Confirmed** as a document statement by the wheel integration work.

In the rc15 virtual DFP, the default button map binds `r2` to the DFP's R1 (bit 4) and `l2` to the DFP's L1
(bit 5), described as gear up / gear down. **Confirmed** from `native/wheel.cpp` on `task/192-ffb-centering`:
`// right paddle: gear up (DFP R1)`, `// left paddle: gear down (DFP L1)`. The contributor's working setup used
these bits; the owner's G27 has not been tested on hardware yet. **High confidence**.

**Consequence:** the game's own gear interface is sequential. Any H-shifter front must either (a) translate a gate
position into a sequence of the game's sequential shift commands, or (b) bypass the input layer and write the
gear state. There is no third "H-gate input" path in the game or in the supported wheel protocol. **Confirmed**
for the action table and the DFP report.

`docs/MENUS-AND-WHEEL.md` line 38 says the H-shifter is "probably not used by GT4 (anecdotal)". This study
confirms the substance for the game's own input: there is no H-gate action and the DFP report has no gate field.
The line does not mean an H-shifter cannot be used at all: it can drive the game through button emulation
(section 2), which is the recommended front.

### 1.3 MT/AT state

- The `Automobile` object has a transmission type byte at `+0x546`. `Automobile::isFullManualTransmission`
  (0x003481C8) returns true exactly when that byte is `6`. **Confirmed** from
  `src/race/unit_00347A00/Automobile__isFullManualTransmission.cpp`.
- `func_0036C9B8` (0x0036C9B8) returns true for values `1, 3, 4, 5, 6` and false for `0, 2`. **Confirmed** from
  the decomp. This suggests several transmission categories; only `6` is "full manual". The meaning of the
  other values is **Unknown**.
- `RaceBase::initializeDynamicsConductor` (0x00388790) walks the race organization's car list and stores
  `manual = Automobile::isFullManualTransmission(car) != 0` into the controller at `+0x1D4`. The direct call to
  0x003481C8 is at 0x00388844 (static JAL scan of the ELF). **Confirmed** from
  `src/race/RaceBase/RaceBase__initializeDynamicsConductor.cpp` and the call-site scan.
- The options objects have `automatic_gear` and `license_transmission_MT` fields, with script getters/setters
  (`MOption__get_automatic_gear`, `MOption__get_license_transmission_MT`). **Confirmed** from the decomp.
  `MCarGarage__setGearAuto` exists in the garage code. **Confirmed**.
- An AT car ignores manual shift commands. The H-shifter therefore only works in an MT car; the game's own MT/AT
  setting is the gate. Forcing MT would be a separate mod with save implications. **High confidence** (game
  behavior inferred from the MT flag and the gear actions; the exact rejection rule is **Unknown**).

### 1.4 Gear state fields: what is known and what is not

The best static candidate for the gear-selection path is the group of functions around `func_0037AD40`
(0x0037AD40), in the unnamed physics cluster:

- `func_0037AD40(arg0, arg1)` reads the Automobile's MT flag from `*(*(arg0 + 0x18C) + 0x18)` and calls
  `Automobile::isFullManualTransmission` at 0x0037AD70. **Confirmed** from
  `src/race/unit_00378778/func_0037AD40.cpp` and the call-site scan.
- It reads a signed byte at `arg1 + 0x19` (inside the 0x20-byte control-packet shape, in its digital data bytes)
  and, when `arg0 + 0x26A` is nonzero, calls
  `func_0037BC20(arg0, func_0037AD10(func_0037ACD0(*(s32 *)(arg0 + 0x180)), that_byte))`. **Confirmed** from the
  decomp. The natural reading is a shift request of `+1`/`-1`; the byte is used as a general step, so a larger
  value is possible (**Hypothesis** for the `+1`/`-1` meaning).
- `func_0037ACD0` (0x0037ACD0) searches a 12-entry table at guest `0x006211E8` for a value and returns its index.
  `func_0037AD10` (0x0037AD10) adds a step and wraps modulo 12. `func_0037BC20` (0x0037BC20) writes the new value
  to `arg0 + 0x180` and calls `func_0037BC68` (0x0037BC68), which steps through a list of gear records, has
  special indices `0x14` and `0x19`, and packs flags into `arg0 + 0x268`. **Confirmed** for the code; the
  semantics of the record list and the special indices are **Hypothesis** (gear/neutral/reverse ring).
- This is a gear-selection path with a manual-transmission check, a step of +/-1 through a table, and a
  "current gear" field at `arg0 + 0x180`. **High confidence** that it is part of the gearbox logic. **Unknown**
  whether `arg0` is the player's car/controller, an AI opponent's controller, or a shared driver object; the code
  references an Automobile through `arg0 + 0x18C`, so `arg0` itself is likely a controller/driver wrapper, not the
  Automobile. **Unknown** whether `+0x180` is the actual physics gear or a commanded gear index.
- The 12-entry table at `0x006211E8` is **not initialized as a gear table in the ELF file**: the bytes at that
  address are float-like constants (`0x3CAE70C1`, `0x3F800000`, `0xC1D437C7`, ...). The table must be filled at
  runtime, or the address is reused by another subsystem. **Unknown**; a proof of concept must read it at runtime.
- The HUD panel gear field is another candidate: `RaceSimplePanel__setGear` writes an `s8` at `panel + 0x178`
  (0x003A83C0) and `RaceOnboardPanel__setGear` writes `panel + 0xA74` (0x003A94B0). **Confirmed** from the decomp.
  The caller is not in the matched decomp (it is a virtual call), and the source of the value is **Unknown**.
  `RaceSinglePlayer__updatePlay` (0x003D5FF8) updates the panel from several object getters, but the exact gear
  getter is not resolved.

A stable pointer chain to the car list is available for a later direct-write investigation:

- `RaceBase + 0x6C` is the `RaceOrganization` pointer (from `RaceBase__updateRace`, which reads `self + 0x6C`).
  **Confirmed** from the decomp.
- `RaceOrganization + 0x60` points to a list structure: count at `+0`, items at `+8` (from
  `RaceOrganization__inputInitialize`). **Confirmed**.
- In `RaceBase::initializeDynamicsConductor`, each list entry is a `CarRecord` whose car pointer is at
  `+0x18`. **Confirmed** from the decomp for that function.
- The player's index in the list is **Unknown**; index 0 is the natural guess and must be verified in game.
- A runtime discovery technique: scan guest RAM for the 32-bit value `0x00682A20`, the `RaceSinglePlayer`
  vtable pointer (`include/gt4/RaceSinglePlayer.h`; the object's vptr sits at object `+0x64`), validate the
  `+0x6C` / `+0x60` chain, then read the car pointer. This is a **Hypothesis** until tested; it is how a
  direct-write proof of concept could find the object without a code hook.

### 1.5 The clutch concept

The display layer definitely has a clutch concept:

- `RacePanel`, `RaceSimplePanel` and `RaceOnboardPanel` have `setClutch(bool)` at vtable slot 19:
  `RacePanel__setClutch` 0x003A74A8, `RaceSimplePanel__setClutch` 0x003A83C8,
  `RaceOnboardPanel__setClutch` 0x003A94B8. **Confirmed** from `config/gthd_names.txt` ("high") and the headers.
- `RaceOnboardPanel__setClutch` writes bit 24 of the display word at `panel + 0xA74` (the same word family as
  `setAutomaticTransmission`, which writes bits 16-23). **Confirmed** from the decomp.
- The panels also have `setGear(int)`, `setAutomaticTransmission(bool)`, `setShiftTimingLamp(bool)` and
  `setSuggestedGear(int)`. **Confirmed**.
- The callers are virtual and not in the matched decomp. The value passed to `setClutch` is **Unknown**; an
  automatic clutch that is disengaged during a shift is the natural reading (**Hypothesis**).
- The car equipment list has a `CLUTCH` part (string at `0x6A6B40`), so the car data has clutch upgrade
  parameters. **Confirmed** for the part name; the runtime physics use is **Unknown**.
- The decomp's matched sources contain **no clutch physics symbol, no clutch input field and no clutch torque
  function**. **Confirmed** for the matched decomp; the unmatched physics clusters may contain one
  (**Unknown**). The decomp's own `knowledge/architecture.md` says "the numeric suspension/tyre code has no RTTI
  and lives in unnamed clusters", and `docs/DECOMP-IDENTITY.md` says the decomp does not cover most physics math.
  **Confirmed**.

**Conclusion:** GT4 has a clutch *indicator* and possibly an automatic clutch inside the drivetrain model, but no
clutch *input*. An analog clutch pedal cannot drive the game through any existing input action or supported wheel
channel. **Confirmed** for the input action table and the DFP report; **Unknown** for the internal physics state
and for any unsupported device the game might still read.

---

## 2. Front 1: H-shifter gear linking

### 2.1 Approach A: button emulation (recommended)

Translate the physical gate into the game's own sequential commands:

- Read the H-shifter position from SDL3 (a separate joystick, or buttons/hats on the wheel's HID device; the
  exact report is device-dependent and **Hypothesis** until the owner's G27 is inspected).
- Keep a `target_gear` (the gate) and an `assumed_gear` (the last gear the game was commanded to, or a readback).
- When the target differs from the assumed gear, emit one shift-up or shift-down pulse per step, with a fixed
  gap between pulses. For example, 1 -> 4 is three up pulses; 4 -> 1 is three down pulses.
- Feed the pulses into the game exactly as a real sequential shifter would:
  - **DFP path (rc15 wheel work):** OR the pulse into the virtual DFP's R1 (bit 4, up) or L1 (bit 5, down) for
    that frame. The game's LGDEV driver and its own gear logic do the rest.
  - **Pad path (main):** OR the pulse into `PadState.buttons` on R2 (bit 9, up) or L2 (bit 8, down), with
    pressure 255, before `apply_pad()`. This is exactly a pad press; the game's own controller assignment and
    gear logic do the rest. The default pad assignment (R2/L2) is **Hypothesis** from the public guide and the
    TASK-076 table; the owner test settles it.
- The game's own rules apply unchanged: MT/AT, shift delay, rev limiter, downshift protection, reverse lockout,
  and the replay recorder. **Confirmed** for the replay recorder; the other rules are the game's, not ours.
- **Neutral:** the H-shifter N gate is the hard part. The game's action table has shift up/down and reverse, not
  neutral. Whether neutral can be reached by downshifting from 1st, or whether the game keeps N as a gear in
  the ring, is **Unknown**. The proof of concept must test: from 1st at standstill, one downshift; from 1st at
  speed, one downshift; the HUD gear. If the game does not accept N, the first version can treat N as "no
  change" or as a clutch-like neutral only when stopped.
- **Reverse:** map the R gate to the game's reverse action (`BUTTON_REVERSEGEAR`), not to a sequence of
  downshifts through 1st. The exact in-game behavior (button versus hold, speed gate) is **Unknown**; the proof
  of concept must test it at standstill. The game's reverse lockout must stay in charge.
- **Gear skips:** the game has no "select 4th" input, so skips are multiple pulses. The game may refuse a
  downshift that would over-rev; the sequencer must not assume every pulse succeeded.
- **Open loop versus closed loop:** the first version can be open loop (assume each pulse succeeds and stop when
  the pulse count is reached). If the owner sees desync (for example a refused downshift, then the next gate
  moves two gears), add a **read-only** gear probe: read the game's gear from guest RAM (candidate fields in
  section 1.4) or from the HUD panel, and correct the assumed gear. Reading is much safer than writing and
  can be validated against the visible HUD. **Recommended staged plan.**
- **AT cars:** the game ignores shift commands in AT. The H-shifter should log "car is automatic" and do
  nothing, or the owner uses an MT car. Forcing MT is a separate mod (write the car's transmission type); not
  part of this front. **High confidence.**

Why this is the right first approach:

- No guest memory writes, no physics state changes, no save changes, no replay changes.
- The game's own shift logic, timing, rev limiter and reverse rules remain the reference.
- It works on both input paths already planned: the pad path on main and the DFP path on the rc15 wheel work.
- It is small: a pure sequencer plus a source map, both testable without the game.

### 2.2 Approach B: direct gear write (mod)

Write the gear state in guest memory instead of pressing buttons:

- Find the player car/controller (section 1.4 pointer chain or a RAM scan for the `RaceSinglePlayer` vtable).
- Find the gear field. Candidate: the controller object's `+0x180` used by `func_0037AD40` /
  `func_0037BC20` / `func_0037BC68`, with the 12-entry table at `0x006211E8`. **High confidence** that this is
  a gear index; **Unknown** whether it is the player's physics gear.
- Write it at guest VBlank from `native/mtvu_runtime.cpp` `vblank_sync()`, the same place where
  `gt4::display::widescreen_hold_at_guest_vblank(ps2::g_ram, 32u << 20)` already writes guest RAM (TASK-079).
  **Confirmed** that the hook and the RAM pointer exist; the gear module does not.
- Validate against the HUD and the physics (engine sound, RPM, acceleration) before claiming success.

Risks (all higher than button emulation):

- **Physics state:** the gear is coupled to engine RPM, wheel speed, the automatic clutch (if any) and the
  torque model. Writing the index without the matching state can over-rev, stall, or make the car jump.
- **Gear lock and rev limiter:** the game's own checks are bypassed. The mod must reproduce them or accept
  invalid states.
- **AT/MT:** writing a gear on an AT car fights the automatic logic. Forcing the transmission type changes the
  car's identity and may be saved.
- **Reverse and neutral:** the special indices `0x14`/`0x19` in `func_0037BC68` and the reverse rules are not
  understood. A direct write can put the car in an invalid gear.
- **Missed-shift logic:** the game may have a shift timer/flag (`+0x26A`, `+0x268`, `+0x274` are candidates).
  Writing the gear while a shift is in progress may corrupt it.
- **Replays and ghosts:** the replay recorder stores the control packet, not the gear. A direct gear write is
  not recorded; replaying the same inputs will not reproduce the gear changes. **Confirmed** from the recorder
  code. This alone makes direct writes unsuitable for a faithful feature.
- **Saves:** the live race state (including the current gear) is not part of the normal car save (**High
  confidence**; the save path stores garage and option data), but a forced MT/AT or a corrupted car object can
  leak into a save. The game's own save path is not touched by button emulation.
- **AI and other cars:** the same class may be used by opponents; a wrong pointer write can shift an AI car.
- **HUD mismatch:** the HUD reads its own gear field; a direct write to a different field may move the car but
  not the dashboard, or the reverse.

Direct writes are a **mod** and would need the project's mod policy (the owner's "not-planned cheat policy" is
recorded in TASKS.md). They are not recommended as the first H-shifter implementation.

| | Button emulation | Direct gear write |
| --- | --- | --- |
| Path | host input -> RaceInput -> the game's own gear logic | guest RAM -> drivetrain physics |
| Shift delay, rev limiter, gear lock, reverse | the game's own rules | bypassed; the mod must reproduce them |
| AT/MT | the game decides | the mod fights the automatic logic |
| Replay/ghost | recorded as controller input and replayed | not recorded; replay desyncs |
| Saves | untouched | possible leak through a forced MT/AT or a corrupted object |
| Risk | low | high |
| Effort | about one week including tests and the owner session | 2-3 weeks, with uncertain results |

### 2.3 Input path to hook

| Path | Where | What to inject | Risk |
| --- | --- | --- | --- |
| Pad (main) | `native/host_io.cpp` `ControllerInput::poll` / `native/pad_mapping.cpp` `map_reading` -> `PadState` | R2/L2 bit + pressure 255 for the pulse frame | None to physics; it is a pad press |
| DFP (rc15 wheel work) | `native/wheel.cpp` `WheelInput` / `map_dfp_input` -> `DfpInput.buttons` | DFP R1/L1 bits 4/5 for the pulse frame | None to physics; it is a paddle press |
| Direct write (mod) | `native/mtvu_runtime.cpp` `vblank_sync()` -> `ps2::g_ram` | gear field in the player car/controller | High (section 2.2) |

The first two are **input** changes; the third is a **game state** change. They should not be mixed in one
change.

### 2.4 Proof-of-concept plan (minimal, with the existing layers)

1. **Pure sequencer** in `native/shifter.{hpp,cpp}` (no SDL, no Windows):
   - `ShifterReading` (gate, neutral, reverse, timestamp).
   - `ShiftSequencer` with `target_gear`, `assumed_gear`, `queued_steps`, `next_pulse_ms`, `pulse_frames`.
   - `update(reading, now_ms) -> {upshift, downshift}`.
   - Unit tests: 1->2, 2->5, 5->2, 1->N, N->1, N->R, R->N, gate bounce, a refused step (no readback), AT
     ignore, and a long gap.
2. **Source map** for the owner's G27 H-shifter: one SDL source per gate (button, hat direction, or axis
   threshold). The rc15 launcher's capture UI can produce the sources; for the PoC a small TOML section is
   enough. The exact G27 report is **Hypothesis** until captured.
3. **Pad-path PoC (main):** in `ControllerInput::poll`, after `map_reading`, OR the pulse into
   `PadState.buttons` (R2/L2) and set the pressure. Add `GT4_SHIFTER=1` and a source map; default off.
4. **DFP-path PoC (rc15 branch):** in `WheelInput::poll` / `map_dfp_input`, OR the pulse into
   `DfpInput.buttons` (R1/L1). Add `GT4_SHIFTER=1`; default off.
5. **Headless check:** with `GT4_DFP=1`, run a scripted gate sequence (`GT4_SHIFTER_TEST=1,2,3,4,5,6,R,N`), log
   the host pulse frames and the game's button word at `0x00622CE8`. The pulse count and the game's button
   edges must match. If a gear readback is available, log it too.
6. **Fidelity check:** compare a frame-dump run driven by the H-shifter sequence with a run driven by the same
   number of paddle/button presses. The dumps must be identical if the H-shifter is truly button emulation.
   This is the strongest automated acceptance available without the owner.
7. **Timing measurement:** measure the time from a pulse to the visible/HUD gear change, and the smallest pulse
   that the game accepts (1, 2, 3 frames). Use the result to set `pulse_frames` and the gap. This is where the
   game's shift delay is learned; do not hard-code a guess.
8. **Owner test:** G27 H-shifter, each gate, skips, reverse, neutral, an AT car, an MT car, a replay, and the
   clutch pedal (display-only). The owner's reference scene is Time Trial, Nordschleife, Toyota Supra Wood One,
   480p.

### 2.5 Acceptance checks

- Each physical gate selects the matching gear in an MT car, and the dashboard/gear readout matches.
- Gear skips work in both directions (1->4, 4->1) within a bounded time (the game's own shift delay).
- N and R behave safely at standstill and while moving; the game's lockouts are not bypassed.
- An AT car is not shifted by the H-shifter (clear log), or the owner's chosen behavior is documented.
- A lap recorded with the H-shifter replays with the same gear changes (button path).
- No physics or timing side effects: the fixed 1/60 s step is unchanged, no interpreter is added, F8/frame
  pacing is unchanged, no guest code or save data is written.
- The owner's G27: force feedback is unchanged; the clutch pedal is display-only; paddles still work.
- Headless: the H-shifter sequence and the equivalent paddle sequence produce identical frame dumps; the
  neutral run is identical to the current build.

### 2.6 Effort estimate

| Item | Estimate | Confidence |
| --- | --- | --- |
| This study | 1 day | Done |
| Pure sequencer + unit tests | 1-2 days | High |
| Pad-path PoC + config | 1-2 days | High |
| DFP-path PoC (on the rc15 branch) | 2-3 days | High |
| Headless fidelity/timing checks | 1-2 days | High |
| Owner test and fixes | 1-2 days | Medium |
| Read-only gear probe (if open loop desyncs) | 2-4 days | Medium |
| Direct gear write (mod, not recommended) | 2-3 weeks | Low |

The button-emulation front is a small feature: about one week of work including tests and the owner session, if
the DFP path is already available. On main, the pad path is enough for a first proof of concept; the DFP path
needs the rc15 wheel branch.

---

## 3. Front 2: clutch

### 3.1 What the game has

- **No clutch input action** in the ELF action table. **Confirmed** for the string table.
- **No clutch channel** in the DFP input report (8 bytes: steering, buttons, hat, throttle, brake).
  **Confirmed** from the rc15 virtual DFP.
- **A clutch HUD indicator**: `setClutch(bool)` on all three panel classes, with a bitfield write.
  **Confirmed**.
- **A `CLUTCH` car part** in the equipment list, so the car data has clutch upgrade parameters.
  **Confirmed** for the part name; the physics use is **Unknown**.
- **No clutch physics symbol** in the matched decomp. **Confirmed** for the matched sources; the unmatched
  drivetrain cluster may contain one (**Unknown**).
- **A possible automatic clutch** during shifts: the HUD flag and the `setAutomaticTransmission` /
  `setShiftTimingLamp` family suggest the game displays a clutch/AT/shift state. The source of the flag and the
  physics behavior are **Unknown**. **Hypothesis:** the game has an automatic clutch that disengages during a
  shift, which is why the HUD can show it, but the player cannot control it.

### 3.2 Can an analog clutch pedal drive it?

No, not through an existing input:

- The game's input action table has no clutch action. **Confirmed**.
- The DFP protocol has no clutch. **Confirmed**.
- The game's own controller settings cannot assign a clutch, because there is no action to assign. **High
  confidence.**
- The rc15 wheel work reads a host clutch axis only for the launcher's live view; it does not send it to the
  game. **Confirmed** from `docs/WHEEL-NATIVE-DFP.md` and `native/wheel.cpp`.

Therefore a clutch pedal can be:

1. **Display-only** (the current rc15 behavior): the launcher shows the pedal, the game ignores it. Safe, no
   fidelity, but honest.
2. **A host-side assist:** when the clutch is pressed, the host cuts throttle or forces a shift pulse/neutral.
   This is not a clutch; it changes the player's inputs and may fight the game's own logic. Not recommended for
   a faithful renderer.
3. **A drivetrain physics mod:** the only way to get a real clutch. It needs the clutch torque/engagement state,
   the engine torque, the gearbox input shaft speed and the shift model. None of these are in the matched decomp.
   It is a research project on the unmatched physics clusters, with high risk to physics, timing, replays and
   saves. **Not recommended now.**

### 3.3 What a clutch mod would need

- Find the drivetrain state: engine RPM, gearbox input speed, clutch engagement, clutch torque capacity, the
  shift timer. The decomp's unnamed clusters around `func_0037AD40` / `func_0037BC68` are the first place to
  look; the 0x26A/0x268/0x274 fields and the 12-entry gear table are candidates. **Hypothesis.**
- Find the torque path: where the engine torque is multiplied by the gear ratio and applied to the wheels.
  **Unknown.**
- Add a host-driven clutch engagement override at VBlank or per frame. This is a physics change, not an input
  change. It must run at the game's 1/60 s step and must not alter the fixed-step timing. **High risk.**
- Handle shifts: disengage on clutch press, complete the gear change on release, respect the game's shift
  delay, rev limiter and reverse lockout. **Unknown** whether the game's own shift model can be reused.
- Replays and saves: the replay recorder stores the control packet; a clutch axis is not in the packet. The mod
  would need a new recorded field or would desync replays. **Confirmed** for the current recorder.

### 3.4 Effort and risk

- Finding the clutch state in the unmatched physics: **Unknown**, likely 1-3 weeks of RE with no guarantee.
- Implementing a stable clutch override: **Unknown**, 2-5 weeks including physics tuning and owner tests.
- Risks: engine stall/over-rev, shift shock, AI cars, replays, saves, frame timing, and a permanent divergence
  from the original physics. The project's fidelity rules (AGENTS.md) require game logic and physics to stay
  exact; a clutch mod is a gameplay change and needs an explicit owner decision and a separate mod scope.
- **Recommendation:** do not include the clutch in the first H-shifter feature. Keep the pedal display-only.
  Revisit only if the owner wants a physics mod and accepts the fidelity cost.

---

## 4. Recommendation

1. **Implement Front 1 with button emulation first.** Start on the pad path (main) because it needs no wheel HLE:
   the sequencer ORs R2/L2 into `PadState`; the game's own gear logic does the rest. Then port the same sequencer
   to the DFP path on the rc15 wheel branch (R1/L1 bits). Keep `GT4_SHIFTER` off by default until the owner
   approves it.
2. **Use open loop initially, with the game's button word at `0x00622CE8` as a read-only check.** Add a gear
   readback only if the owner reports desync. Reading the game's gear is a separate, low-risk investigation.
3. **Do not implement direct gear writes in the first version.** Treat them as a mod with the risks in section
   2.2; they would also break replay fidelity.
4. **Do not implement a clutch.** The game has no clutch input and no clutch channel; a real clutch is a physics
   mod with unknown feasibility. Keep the pedal display-only, as the rc15 wheel work already does.
5. **Owner decisions needed:**
   - Is the H-shifter for the pad path, the DFP path, or both? (Both share the sequencer.)
   - What should the N gate do if the game has no neutral action? (No change when moving; neutral when stopped;
     or a future mod.)
   - What should an AT car do? (Ignore with a log; or a forced-MT mod, not recommended now.)
   - Is the clutch pedal acceptable as display-only for this release?
   - Confirm the task number: the 1.3.0 line already uses TASK-104 for the launcher save import. This study was
     requested as TASK-104 on 2026-11-11; the final number should be confirmed before scheduling.

---

## 5. Evidence table

| Claim | Evidence | Label |
| --- | --- | --- |
| The game's gear actions are shift up/down and reverse; no clutch/H-gate action | ELF strings at guest `0x6A5EA0`; only `CLUTCH` string is the car part at `0x6A6B40` | Confirmed |
| The RaceInput singleton is at `0x00622B60`; the pad word is at `+0x188` (`0x00622CE8`) | `RaceBase__getInput` (0x00389628) returns `&D_00622B60`; `RaceInput__getButtons` (0x00426A00) returns `*(s32 *)(arg0 + 0x188)` | Confirmed |
| The control packet is 0x20 bytes and is recorded for replays | `AutomobileControl__clearPacket` (0x003557E8); `AutomobileControlRecord__Recorder__write/read` (0x00346138 / 0x00345D70) | Confirmed |
| The DFP report has steering, 14 buttons, hat, throttle, brake; no clutch/gate | `native/usb_dfp.cpp` `encode_report` on `task/192-ffb-centering` | Confirmed (branch source) |
| The rc15 wheel work maps the host gear up/down to DFP R1/L1, and the game accepts them as shifts in the contributor's setup | `native/wheel.cpp` default button map, `task/192-ffb-centering`; `docs/WHEEL-NATIVE-DFP.md` | High confidence |
| The car's transmission type is `+0x546`; full manual is value 6 | `Automobile__isFullManualTransmission` (0x003481C8); call sites 0x00388844 and 0x0037AD70 | Confirmed |
| The HUD has gear, clutch and AT setters | `config/gthd_names.txt`; `include/gt4/RacePanel.h`, `RaceOnboardPanel.h`, `RaceSimplePanel.h` | Confirmed |
| A gear-selection path uses a 12-entry table at `0x006211E8` and a gear field at `+0x180` | `func_0037ACD0`, `func_0037AD10`, `func_0037AD40`, `func_0037BC20`, `func_0037BC68` | High confidence for the code; Unknown for ownership |
| The car list is at `RaceOrganization + 0x60`; entries expose the car at `+0x18` | `RaceBase__initializeDynamicsConductor` (0x00388790); `RaceOrganization__inputInitialize` | Confirmed for the code; player index Unknown |
| The game has a VBlank guest-RAM write precedent | `native/mtvu_runtime.cpp` `vblank_sync()` calls `gt4::display::widescreen_hold_at_guest_vblank(ps2::g_ram, 32 MiB)` | Confirmed |
| The game has a clutch HUD concept but no clutch input/physics symbol in the matched decomp | `setClutch` setters; no `clutch` outside them in the matched sources; `knowledge/architecture.md` says the physics is in unnamed clusters | Confirmed for the matched code; physics Unknown |
| The rc15 wheel work says direct gear selection and manual clutch are outside its implementation | `docs/WHEEL.md` on `task/123-dfp-wheel-study`; TASKS.md on `task/192-ffb-centering` | Confirmed |

---

## 6. Open questions

1. Does the game accept neutral from the shift-down button at standstill? At speed?
2. How is reverse engaged in GT4: a dedicated button, a hold, or a gear below 1st? What is the speed gate?
3. What is the game's shift delay, and what is the smallest reliable button pulse (1, 2, 3 frames)?
4. Does the game's drivetrain model a clutch torque at all, or only a display flag?
5. Where is the player's actual physics gear (the controller `+0x180` candidate is not confirmed)?
6. Does the G27 H-shifter appear as buttons, a hat or an axis on Windows/SDL3, and does the clutch appear as an
   axis on the pedal device?
7. Does the game's in-game controller assignment for shift up/down match the default R2/L2 on a pad, and is it
   per-save?
8. Does a gear change made by button emulation record and replay exactly as a paddle press? (Expected yes;
   confirm with a recorded lap.)
9. Does an AT car ignore the shift pulses cleanly, or does it produce an event/log that must be suppressed?
10. Does the player's car index in the race organization list stay 0 in every mode (Time Trial, race, split
    screen, replay, B-Spec)? (Only relevant to the direct-write investigation.)

---

## 7. Files and references

- Current input layer: `native/host_io.cpp`, `native/pad_mapping.{hpp,cpp}`, `native/controls.{hpp,cpp}`,
  `native/window.cpp`; `docs/INPUT-SDL3.md`, `docs/CONTROLS.md`, `docs/MENUS-AND-WHEEL.md` (line 38 says the
  H-shifter is "probably not used by GT4 (anecdotal)"; this study confirms the substance from the action table and
  the DFP report, and adds the button-emulation path that makes the hardware usable as a sequential source).
- Wheel work (not main at `85718831`): `docs/WHEEL.md`, `docs/WHEEL-DFP-PLAN.md`,
  `docs/WHEEL-NATIVE-DFP.md`, `native/usb_dfp.{hpp,cpp}`, `native/wheel.{hpp,cpp}`,
  `docs/tasks/TASK-123-DFP-WHEEL-RESEARCH.md`.
- Decomp: `src/race/unit_00347A00/Automobile__isFullManualTransmission.cpp`,
  `src/race/RaceBase/RaceBase__initializeDynamicsConductor.cpp`,
  `src/race/RaceBase/RaceBase__getInput.cpp`,
  `src/race/unit_00426990/RaceInput__getButtons.cpp`,
  `src/race/unit_00426990/RaceInput__getButtonDown.c`,
  `src/race/unit_00426990/RaceInput__getAutomobileControl.cpp`,
  `src/race/unit_0034E9A0/AutomobileControl__clearPacket.cpp`,
  `src/race/unit_00343220/AutomobileControlRecord__Recorder__write.cpp`,
  `src/race/unit_00343220/AutomobileControlRecord__Recorder__read.cpp`,
  `src/race/unit_00378778/func_0037ACD0.cpp`, `func_0037AD10.cpp`, `func_0037AD40.cpp`,
  `func_0037BC20.cpp`, `func_0037BC68.cpp`,
  `src/race/unit_003D5FC0/RaceSinglePlayer__updatePlay.cpp`,
  `include/gt4/RaceInput.h`, `include/gt4/RacePanel.h`, `include/gt4/RaceOnboardPanel.h`,
  `include/gt4/RaceSimplePanel.h`, `include/gt4/RaceSinglePlayer.h`, `config/gthd_names.txt`.
- ELF: `private/reconstructed/SCUS_973.28.elf`, sha256 `10f82e22...c7c935c`. String table addresses in section
  1.2; static JAL scan for the direct `Automobile__isFullManualTransmission` calls.
- Guest VBlank hook: `native/mtvu_runtime.cpp` `vblank_sync()`; `native/race_widescreen.{hpp,cpp}` as the
  precedent for a guest-RAM hold.

---

## 8. Confidence summary

- **Confirmed:** sequential gear actions, no clutch/H-gate action, DFP report shape, MT flag address/meaning,
  HUD setters, replay recorder stores control packets, input path, VBlank RAM hook.
- **High confidence:** DFP R1/L1 are shift up/down; the gear-selection path around `func_0037AD40` uses a
  12-entry table and a `+0x180` gear field; button emulation is the faithful approach.
- **Hypothesis:** the game has an automatic clutch during shifts; the controller `+0x180` is the player's gear;
  the player car is index 0 in the race organization list; the G27 report shape.
- **Unknown:** the actual player physics gear field, neutral/reverse rules, shift delay, clutch physics, replay
  behavior under direct writes, and whether the owner wants a mod that changes physics.

