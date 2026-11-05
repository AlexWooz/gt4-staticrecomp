# Native Driving Force Pro: virtual USB wheel (TASK-018, TASK-181)

Status: **the only wheel path** (owner, 2026-11-05). The device model and the game side are the contributor work of
**WaimateWihongi** (TASK-018, PR #205), working on one setup (MOZA R5 base with CS V2 rim and pedals on the base, Turtle
Beach VelocityOne Multi-Shift, GT4 USA v2.00, CoreGS 2.0, Windows 11). TASK-181 removed the 1.2.x bridge onto the DualShock 2
pad, made the force feedback cheaper, pinned the USB capsule for both games and added the launcher's Wheel page, whose base
is the 1.2.x wheel UI of **JulioHeroZ** (with **luanantony**, PR #101). Spec II and the owner's Logitech G27 are not tested
on hardware yet. This follows option A of [the plan](WHEEL-DFP-PLAN.md) (an OHCI model under the game's own USB stack),
chosen because the static evidence below showed the game's modules already load and only the controller was missing.

## What it does

GT4 sees a **Logitech Driving Force Pro** on its USB port and runs its own Logitech driver unchanged. The host wheel
(any SDL3 joystick) feeds the DFP's input report, and the game's own force feedback commands are decoded and played on the
host wheel through SDL3 haptics. Steering is native 14-bit at the range the game sets (GT4 switches the DFP to 900 degrees
itself); nothing goes through the DualShock 2 axes.

## Evidence (Confirmed on v2.00, 2026-11-05)

- `USBD.IRX` (USB Driver 1.4.0), `PDIUSB.IRX` and `LGDEV.IRX` (LgDev 1.11.036 "Rooz special") load and start under the IOP
  interpreter; `USBD.IRX` registers IOP interrupt line 22 (handler 0x00060CC0). With the donor's register stub no device is
  ever found.
- With the controller model and a virtual DFP, the game's driver issues SET_ADDRESS, GET_DESCRIPTOR (device 8 and 18 bytes,
  configuration 4 and 41 bytes) and SET_CONFIGURATION 1, then polls interrupt IN (about 100 reports per second).
- **Port:** the driver configures and polls the wheel on either root hub port, but the game uses its input only from
  **port 2**. Headless check: a virtual Cross press answers the first-start memory card dialog on port 2 and is ignored on
  port 1 (frame dumps identical to a run without input). `GT4_DFP_PORT=1|2`, default 2.
- **Commands GT4 sends** (census of one lap): `F8 06` once (meaning unknown), `F3`, `F4`/`F5` (default spring on/off),
  `F8 03` (900-degree range, twice), `21 0B 7F 7F 77 00 80` and `23 0B ...` (a high-resolution spring, the menu centring),
  `21 0E 80 FF 20 ...` (friction on slot 1) and, while racing, about 100 per second of **`11 08 <L1> 80 00 00 00`**: a
  *variable* force (type 0x08) on slot 0 whose level L1 (0x80 = none) is the game's aligning force. `13 08 ...` stops it.
  The constant force type (0x00) was never seen. The variable force with zero step parameters is played as a constant force.

## Files

| File | Role |
|---|---|
| `native/usb_ohci.{hpp,cpp}` | OHCI 1.0a register set, two-port root hub (reset, connect and change bits), 1 ms frames: periodic (interrupt) and control/bulk lists, general TDs, data toggle, done queue write-back, interrupts. Written from the public OpenHCI specification. |
| `native/usb_dfp.{hpp,cpp}` | The virtual DFP: device, configuration, HID and string descriptors (written fresh), the 8-byte input report, the force command decoder, and `DfpBridge` (host <-> guest state). |
| `scripts/prepare_usb_runtime.py` | USB capsule: derives the donor's `iop/hw.cpp` (pinned SHA-256 `b8b56b21...c836`) so the OHCI registers reach the model; CMake `GT4_USB_RUNTIME_SOURCE` checks its hash. Without the virtual wheel the donor stub answers. Pinned capsule `private/slice409-usb-runtime` (`docs/inputs/slice409-usb-runtime.json`); recipe in `docs/BUILD.md`. |
| `native/wheel.{hpp,cpp}` | `wheel.toml`, the SDL3 devices (identity, auxiliary devices, capture sources, pedal calibration), the DFP input for a device snapshot (`map_dfp_input`), and `WheelInput` (every VBlank; the SDL haptic sink). |
| `native/dfp_haptics.{hpp,cpp}` | The game's forces on the wheel: the effect plan (`plan_dfp_effects`), the effect lifecycle (`DfpHapticPlayer`) and the one owner of the haptic device (`DfpHapticThread`). |
| `native/usb_dfp_fixture.cpp`, `native/wheel_fixture.cpp`, `tests/python/test_usb_runtime.py` | CTest `native_usb_dfp` (enumeration on fake guest memory, interrupt IN/OUT, report bits, command decoding) and `native_wheel` (the file, pedals, identity, steering on the game's range, button bits, the effect order on a fake haptic sink, the haptics thread, an SDL3 virtual wheel); the capsule on a synthetic donor and its pin. |
| launcher `gt4l-core/src/wheel*.rs`, `gt4l-platform/src/sdl_wheels.rs`, `gt4l-tauri/src/wheel_*.rs`, `ui/js/pages/wheel.js`, `ui/js/wheel-*.js` | The Wheel page (TASK-181): detection and auto-map, the capture wizard, calibration, force settings, the force test and the last session's force check. |

## Configuration

The launcher's **Wheel** page writes `wheel.toml` beside `controls.toml` and passes `GT4_WHEEL_FILE` at Play; `game.ini`
carries `wheel_file=<path>` for a direct start of the game executable. Without the variable the game looks beside
`GT4_CONTROLS_FILE`, then beside its executable. **A file the parser refuses never stops the game**: it logs
`[wheel] wheel.toml refused, the game runs without the wheel: <reason>` and runs without the wheel.

- `[wheel] enabled = true` plugs the virtual DFP in. `mode = "dfp"` is the default and the only mode; `mode = "pad"` (the
  removed 1.2.x bridge) is refused. Keys of the bridge are read and ignored, so 1.2.x files load: `pedal_map_mode`, steering
  `effective_degrees`, `deadzone`, `linearity`, `sensitivity`, `[ffb] centering_spring`, `curb_rumble`. The launcher
  migrates such files to DFP keys.
- `[steering] source` and `rotation_degrees`: the host wheel's physical range, which maps 1:1 onto the range the game sets
  (GT4 sets 900 degrees). It must equal the range set in the wheel's software (G HUB, Profiler, Pit House...). `invert`.
- `[throttle]`, `[brake]`: source, polarity mode, dead zone, saturation, curve. `[handbrake]` presses Circle past half
  travel. `[clutch]` is only for the launcher's live view: the DFP has no clutch.
- `[ffb] enabled`, `strength` (0..2, scales the game's forces), `damper` (0..1, a damping floor the wheel always has, not
  scaled by strength), `invert` (reverses the game's constant force).
- `[buttons]` keeps the DualShock 2 slot names of the 1.2.x files, so those files keep their meaning:

  | Slot | DFP control |
  |---|---|
  | `cross`, `square`, `circle`, `triangle` | the same buttons (bits 0, 1, 2, 3) |
  | `r2` | gear up, right paddle: the DFP's R1 (bit 4) |
  | `l2` | gear down, left paddle: the DFP's L1 (bit 5) |
  | `r1`, `l1` | the DFP's R2, L2 (bits 6, 7) |
  | `select`, `start`, `r3`, `l3` | bits 8, 9, 10, 11 |
  | `dpad_up`, `dpad_right`, `dpad_down`, `dpad_left` | the hat (0 up, clockwise, 8 released) |

Switches: `GT4_DFP=1` plugs the virtual wheel in without a host wheel (headless tests); `GT4_DFP_TRACE=1` logs every
command, a command census and `dfp-forces.csv`; `GT4_DFP_PORT=1|2` (default 2); `GT4_WHEEL_HAPTIC_THREAD=0` applies the
forces on the emulation thread instead of the haptics thread (A/B).

## Force feedback (TASK-181)

- **One owner:** `DfpHapticThread` opens the haptic device from the wheel's joystick, creates the four persistent effects
  (constant, spring, damper, friction, where supported) and is the only code that touches the device. The emulation thread
  only stores the latest force set (every game force command) and wakes it, so a slow wheel driver never stalls the game; a
  force set the thread has not reached yet is replaced by the newer one.
- **Only what changed:** an effect is sent only when its parameters change. While racing that is the constant force alone
  (at most one call per game command), instead of four `SDL_UpdateHapticEffect` calls per command (about 400 a second).
- **Stop once:** focus loss stops the effects once (`stop_all`, gain 0), however often VBlank or the window ask; focus
  regained sends what changed and starts them again.
- **Exit:** the effects stop and the device closes before the joysticks close (the haptic handle uses the wheel's
  DirectInput device), also on `std::exit` (an exit hook). On a crash the process ends and Windows closes the device handle;
  that the wheel's driver then ends the force is a Hypothesis to check on each wheel.
- **Log:** at exit `[wheel] force feedback summary: game force commands N, effect updates sent N, effects started N, haptic
  device open|none, effects supported|none, game forces played N, reached the wheel yes|no`. The launcher's Wheel page reads
  it from the last session log ("force feedback received").
- **The launcher** closes all its SDL handles (the wheel's joystick and any haptic handle) before the game starts and opens
  a haptic handle only for its "Test force" button (a short constant pull each way), never while the game runs.

## Fixes found on the way

- The withdrawn bridge sent rumble through both SDL haptic rumble and `SDL_RumbleJoystick`; on a DirectInput wheel that is two
  effect owners on one device, and the MOZA R5 crashed the game in `pid.dll` (access violation). All forces now use one SDL
  haptic handle, and features the device lacks (the R5 has no autocentre) are never called.
- Forces are applied the moment the game sends them (the IOP runs on the emulation thread), not at the next VBlank.

## Known limits

- **High-speed wobble** (as on the original DFP): on fast straights GT4's force near the centre is almost bang-bang and lags
  the wheel by about 40 ms, which makes a strong, low-friction direct-drive base oscillate at about 2 Hz. A contributor-side
  adaptive filter was tried and works, but is left out of this change for a separate review.
- Periodic, ramp and square effects are decoded but not played (GT4 was not seen sending them). `F8 06` is accepted unread.
- Spec II and other wheels are not tested on hardware. Save states do not include the USB model.
- With `enabled = true` the virtual DFP is plugged in even while the host wheel is unplugged (it reports a centred
  wheel and nothing pressed); the pad keeps working.
