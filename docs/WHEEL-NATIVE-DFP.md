# Native Driving Force Pro: virtual USB wheel (contributor implementation for TASK-018)

Status: **working on one setup, contributor-tested** (2026-11-05): MOZA R5 base with CS V2 rim and pedals on the base,
Turtle Beach VelocityOne Multi-Shift, GT4 USA v2.00, CoreGS 2.0, Windows 11. Spec II is not tested. This follows option A
of [the plan](WHEEL-DFP-PLAN.md) (an OHCI model under the game's own USB stack), chosen because the static evidence
below showed the game's modules already load and only the controller was missing.

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
| `scripts/prepare_usb_runtime.py` | USB capsule: derives the donor's `iop/hw.cpp` so the OHCI registers reach the model; CMake `GT4_USB_RUNTIME_SOURCE` checks its hash. Without a wheel in DFP mode the donor stub answers as before. |
| `native/wheel.{hpp,cpp}` | The SDL3 joystick layer from the 1.2.x work (device identity, auxiliary devices, capture sources, calibration). New `mode = "dfp"`: fills the DFP report and plays the game's forces; persistent effects (constant, spring, damper, friction) are created once and only updated. |
| `native/usb_dfp_fixture.cpp`, `tests/python/test_usb_runtime.py` | Enumeration through the controller model on fake guest memory, interrupt IN/OUT, report bits, command decoding (including GT4's variable force); the capsule derivation. CTest `native_usb_dfp`, `native_wheel`. |

## Configuration

`wheel.toml` (the launcher has no wheel page yet): `GT4_WHEEL_FILE`, else beside `GT4_CONTROLS_FILE`, else beside the game
executable. `[wheel] mode = "dfp"` enables the virtual wheel. Steering source and `rotation_degrees` (the host wheel's
physical range) map 1:1 onto the range the game sets. `[ffb] strength`, `invert`, `damper` (a damping floor applied by the
wheel's firmware). Button slots keep the DualShock names; in DFP mode the shift slots (`l2`/`r2`) become the DFP's gear
paddles and the D-pad slots its hat. `GT4_DFP=1` plugs the virtual wheel in without a host wheel (headless tests);
`GT4_DFP_TRACE=1` logs every command, a command census and `dfp-forces.csv`.

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
- Spec II, other wheels and the pad-mode bridge are not retested here. Save states do not include the USB model.
