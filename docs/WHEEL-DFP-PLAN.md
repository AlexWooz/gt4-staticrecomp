# Native Driving Force Pro wheels: plan (TASK-018, research TASK-123)

Status: **plan only** (owner, 2026-11-04). Not part of 1.3.0. Nothing here is implemented. Owner hardware for acceptance:
Logitech G27. Evidence base: [DFP-PCSX2-FACTS](research/DFP-PCSX2-FACTS.md) (secondary, PCSX2-derived prose),
[TASK-123 brief](tasks/TASK-123-DFP-WHEEL-RESEARCH.md), [WHEEL.md](WHEEL.md) (the withdrawn 1.2.x pad bridge),
[MENUS-AND-WHEEL.md](MENUS-AND-WHEEL.md), [IOP-MAP.md](IOP-MAP.md), [INPUT-SDL3.md](INPUT-SDL3.md), PR #151 (external study).

## Goal

The game sees a **virtual Logitech Driving Force Pro** (DFP: 900-degree steering, the game's own force feedback), whatever
wheel the host has. Two flows:

1. Host wheel, pedals and buttons -> SDL3 -> a normalized wheel state -> the DFP input contract GT4 reads.
2. GT4's own force-feedback commands -> a translator -> SDL3 haptics on the chosen host wheel.

Ordinary gamepad and keyboard play stay as they are. The native wheel path never goes through DualShock 2 pad axes (that
was the 1.2.x bridge, withdrawn in 1.2.5).

## Fixed rules

- **Game logic unchanged.** The game reads its own wheel input and runs its own physics; we only supply a device. No
  writes into game state to emulate clutch, H-shifter or assists (PR #151 proposes some; they are out).
- **No GPL in the game executable.** PCSX2 and Linux `hid-lg4ff` are GPL. A PCSX2-tainted reader produced the prose facts
  page; the person who writes our device model works from that prose, the game's own modules, public vendor and USB/HID
  documents and real-hardware captures, never from PCSX2 or Linux driver sources. The implementer does not read the
  facts' cited source lines either. This mirrors the CoreGS 2.0 spec/implementation split.
- **PCSX2-tainted people never work on TASK-027 or `native/hw_gs/`** (AGENTS.md). Keep the wheel researcher and the
  CoreGS implementer separate.
- **No JIT.** The game's IRX modules keep running on our IOP interpreter (or are replaced by HLE we write).
- Forces must stop deterministically on pause, focus loss, game exit, crash, device removal and launcher stop.

## What we know

### The guest stack (Confirmed unless marked)

| Layer | Module | Notes |
|---|---|---|
| USB host controller | OHCI registers | The donor runtime only **stubs** them (`runtime/src/iop/hw.cpp:190-208`: a register array, `HcRevision=0x110`). No transfers, no ports, no devices. |
| USB driver | `USBD.IRX` from the disc | v2.00 "Version 1.4.0" (`PsIIusbd 3010`, 34,993 B); Spec II "Version 1.6.0" (`PsIIusbd 3100`, 35,009 B). Interpreted today. |
| Polyphony USB class stack | `PDIUSB.IRX` | Hub, keyboard, mouse, serial (`pdiusb_iopsvr`, `ukbd`, `ums`, `userial`). Byte-identical in both games. |
| Logitech wheel driver | `LGDEV.IRX` | "Version 1.11.036 (PDI branch), Rooz special", module `LgDev_tb_rb_Driver`, library `LGTGliblgdev`, imports `usbd`, options `thpri=`, `devs=`, `effs=`: a **device-and-effects API**. Same version in both games (different build dates). |
| EE client | `liblgdev 1.11.036` in the game ELF | Strings at v2.00 vaddr 0x6C8D40 onwards: `PlayStation2::GT4GamePort`, `USBGamePort`, `PS2GT4 slot1/slot2`, `USB port1/port2`, `DISCONNECT`, and `COUGAR`, `JAGUAR`, `CHEETAH` (Hypothesis: device class names LGDEV reports). |
| Game options | `steering_forcefeedback_1p/2p`, `wheel_angle`, menu `WHEEL FEEDBACK` | Not yet traced to code. |

All 23 game IRX modules are interpreted (`register_hle_module` has no caller); only Sony system libraries are native HLE.
The decomp names no wheel or USB functions; `RaceInput::getAutomobileControl` (0x00426D50), `RaceInput::fetch`
(0x00426F00) and the replay recorder `AutomobileControlRecord::Recorder::steer_update` (0x00345C38) are where the game's
control values land (v2.00).

### The device contract (from PCSX2, secondary; to be confirmed against GT4)

Vendor 0x046D, product **0xC298**, bcdDevice 0x1106 (a 0x1102 variant exists); one HID interface, EP1 interrupt IN and
EP2 interrupt OUT, 8-byte packets every 10 ms. Input: 14-bit steering (centre 0x1FFF), 14 buttons (R1/L1 = shift up/down),
a hat, inverted 8-bit throttle and brake, a 0x11 marker. Output: 8-byte force commands on 4 slots (constant, spring,
damper, friction, auto-centre, periodic shapes) and 0xF8 extended commands (DFP mode, range 200 / 900 degrees). PCSX2
implements neither the range commands nor several effects, and swaps the root hub's ports because GT4 cares. Details:
[DFP-PCSX2-FACTS](research/DFP-PCSX2-FACTS.md).

### What can be reused from the withdrawn 1.2.x bridge (commit `cbfd8b8a`, PRs #101/#109/#111)

| Part | Disposition |
|---|---|
| SDL3 joystick enumeration with GUID identity, auxiliary pedals/shifters, ownership (a wheel-owned device leaves the gamepad path) | Reuse, adapted |
| Capture, calibration, pedal polarity modes, `wheel.toml` schema, launcher Wheel page and i18n | Reuse, adapted (new fields: range, force gain, effect toggles) |
| Stop-everything lifecycle on focus loss/shutdown/disconnect | Reuse |
| Mapping wheel input onto DS2 pad axes; rumble-derived feedback plus a fixed centring spring | Replace |

### The host side (general knowledge, to be confirmed on the G27)

SDL3 sees a G27 as a joystick (steering, three pedal axes, paddles and wheel buttons, a hat, the H-shifter as buttons).
Force feedback goes through SDL haptics (DirectInput on Windows): constant, spring, damper, friction, periodic,
auto-centre. **SDL cannot set the wheel's rotation range**; on Windows that is the Logitech driver/G HUB setting, on Linux
a sysfs attribute of the kernel driver. Pedals may be combined on one axis depending on the driver setting.

## The design decision to make first: where the virtual device lives

| Option | What runs | For | Against |
|---|---|---|---|
| A. OHCI model + virtual USB DFP | USBD, PDIUSB, LGDEV interpreted unchanged; we model the OHCI controller (endpoint and transfer descriptor lists, HCCA frame counter, root-hub ports, interrupts) and a DFP behind it | Most faithful; the game's own drivers see real USB traffic; the OUT packets are the documented DFP commands | Largest; OHCI timing against our IOP interpreter; the port-swap quirk; the public OHCI spec must be followed closely |
| **B. HLE of USBD (recommended to evaluate first)** | PDIUSB and LGDEV interpreted; our USBD answers their calls with a virtual bus holding one DFP | No OHCI timing model; LGDEV still produces the genuine 8-byte DFP commands, which we decode with known semantics; the same seam serves any USB device later | Two USBD versions (1.4.0 v2.00, 1.6.0 Spec II) with export tables to match; LGDEV/PDIUSB callback and thread expectations must be honoured |
| C. HLE of LGDEV's RPC | Our code replaces the Logitech driver's service; the EE calls arrive as effect-level requests | Effects map directly to SDL haptics | LGDEV's API is proprietary and undocumented (reverse-engineered from the IRX and `liblgdev`); higher risk of subtle misbehaviour; loses the documented command stream |

Recommendation: decide in stage W0 with evidence, leaning to **B**. Fall back to A if LGDEV or PDIUSB touch OHCI registers
or timing directly. Use C only if B proves impossible. HLE of a disc module needs the unused `register_hle_module` path (or
an equivalent load-time interception), for each game target separately.

## Stages and gates

Each stage is one or a few short agent sessions with tests. A gate is a stop for review before the next stage.

### W0. Evidence (static; no hardware, no forces)

- Disassemble `USBD.IRX` (both versions) and `LGDEV.IRX` exports/imports; find how LGDEV registers with USBD, what it
  asks for (descriptors, product IDs, the device table behind COUGAR/JAGUAR/CHEETAH), and its thread/callback model.
- Trace the EE `liblgdev` calls from `GT4GamePort`/`USBGamePort` to the options and to `RaceInput`.
- Boot log with no device: which OHCI registers the modules touch and whether anything waits or times out.
- Do it for v2.00 and Spec II separately (the Spec II EE call sites are unmapped today; its ELF is
  `private/slice272-spec2-elf/SCUS_974.36`).
- Output: `docs/research/DFP-WHEEL-STUDY.md` with tables A-G of the TASK-123 brief. **Gate W0: boundary chosen (A/B/C) with
  call-site evidence; DFP identification rule known; owner approves the boundary.**

### W1. Virtual bus and recognition

- Implement the chosen boundary with a virtual DFP reporting a neutral wheel. No host wheel yet.
- Prove recognition: LGDEV reports a device; the game's wheel options and `WHEEL FEEDBACK` behave as with a wheel; the
  900-degree mode is requested (or the identity alone enables it: established here).
- Negative controls: no virtual device -> the game behaves exactly as today; the pad still drives; recorded drives
  (`scripts/coregs2_bench`) keep identical race clock and state with the device absent.
- **Gate W1: recognized on both targets; no change without a wheel.**

### W2. Input contract

- Host SDL3 wheel -> normalized state -> 8-byte DFP report: 14-bit steering, inverted pedals, buttons, hat, paddles.
- Steering scale follows the range the game sets (200 or 900 degrees): full report span = the guest range; the host
  wheel's physical range must match (instruct the player, or set it on Logitech hardware in W5).
- Calibration and pedal modes from the 1.2.x page; launcher live view of the *encoded* report, not just host axes.
- Fixtures: report encoding for every bit; neutral values; clamping; hat diagonals.
- Extend the input recorder (`GT4_INPUT_RECORD`) with wheel report lines so wheel drives can be replayed and compared
  like pad drives.
- **Gate W2: owner drives a Time Trial on the G27; full lock is 450 degrees each side at 900; pedals use the whole travel;
  no dead zone added by us.**

### W3. Force feedback

- First **log** every OUT packet GT4 sends (test builds only) across scenes: menus, garage, race start, kerbs, gravel,
  walls, pause, replay, split screen, both targets. This replaces PCSX2's guesses with GT4's real command set.
- Translator: a per-slot state machine (download, play, stop, default spring) -> persistent SDL haptic effects (constant,
  spring, damper, friction) and auto-centre; update only on change; never restart running effects needlessly.
- Unsupported effects are logged and dropped, never turned into rumble.
- Stops: pause, focus loss, game exit, crash handler, device removal, guest reset; a watchdog stops forces when the game
  stops sending updates. Force gain and invert in the launcher.
- Automated test without hardware: a fake haptic sink replays a captured OUT log and checks the effect calls and the stop
  order.
- **Gate W3: owner feel test on the G27 (centring in menus, cornering load, kerbs), and a forced-quit test with no stuck
  force.**

### W4. Launcher and play

- Controls > Wheel page reused and adapted: device choice (same GUID/serial identity as the controller choice of rc12),
  calibration, live encoded view, range hint, force gain, effect toggles; `wheel.toml` migration; both languages; direct
  start through `game.ini`.
- Coexistence: wheel for player 1, pad for player 2 in split screen (the game's `_1p/_2p` options and `USB port1/port2`).
- Frame skipping and the 30 FPS mode: forces follow the game's commands, not presented frames; check timing with
  frames left out.

### W5. Optional: Logitech pass-through

- For Logitech wheels that speak the classic protocol, send the game's raw DFP commands straight to the wheel through
  SDL's HID access, including the range command, for exact force feel. Needs W3's command log and a per-model allow
  list. On Windows the Logitech driver may own the device: research item.

### W6. Release

- In a 1.3.x or 1.4.0 minor release (owner decides), Windows first. Linux follows the Linux port (SDL3 haptics map to
  evdev there). Android: USB-OTG wheels are possible in principle (SDL3 on Android reads joysticks; force feedback on
  Android is not exposed by the platform for wheels): treat as unsupported until a study says otherwise.

## Subtle risks

1. **Wrong boundary.** LGDEV or PDIUSB may depend on OHCI behaviour (frame counter, interrupts) even if they talk through
   USBD. W0's boot log decides; do not skip it.
2. **USBD version split.** An HLE of USBD must serve 1.4.0 (v2.00) and 1.6.0 (Spec II) export tables exactly.
3. **Port mapping.** GT4 cares which root-hub port the wheel is on (PCSX2's swap). Establish from the game, not by copying.
4. **Identity details.** Product ID alone may not be enough; bcdDevice, string descriptors or the 0x11 marker byte may be
   checked. W1 tests each.
5. **Range semantics.** Whether GT4 sends the 900-degree command, and what a real DFP does after it (rescale, re-centre).
   Steering precision must be preserved from SDL (16-bit) to the 14-bit report without a pad-style conversion.
6. **Stuck forces.** A crash or a killed process can leave a spring or constant force active on some drivers. Stop on
   every exit path, and test a forced kill.
7. **Effect restarts.** Restarting SDL effects every update causes jolts or drop-outs on many wheels; update in place.
8. **Thread and timing.** IOP-side polling is 10 ms; SDL haptic calls should run on the host input thread, fed through a
   queue in guest order. Never call SDL haptics from the IOP interpreter thread inside a guest callback.
9. **Host range and pedal modes** are set outside the game on Windows (Logitech software). The launcher must detect and
   explain, not silently assume.
10. **Determinism tooling.** Wheel drives must be recordable and replayable, or every later CPU/renderer change risks
    unnoticed wheel regressions.
11. **Spec II differences.** Same LGDEV, different USBD and EE code: everything is checked per target.
12. **Licensing hygiene.** Keep the facts page as the only bridge from PCSX2; review new code for structural copying.

## Owner tests (G27)

W1 recognition in the game's menus; W2 steering lock, pedal travel and buttons; W3 force feel and a forced-quit stop test;
W4 split screen with a pad for player 2; both targets each time.

## Not in scope

Clutch, H-pattern gears and LEDs (not in the DFP contract), game-side assists or torque changes, wheels on Android.
