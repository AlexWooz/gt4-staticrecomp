# Driving Force Pro: protocol facts read from PCSX2 (secondary source, prose only)

Status: research input for [the native DFP wheel plan](../WHEEL-DFP-PLAN.md) (TASK-018, TASK-123). 2026-11-04.

## Provenance and rules

- Read by a **PCSX2-tainted** research agent, on the owner's request (2026-11-04), from the pinned tree
  `private/tooling/pcsx2-slice101-source`, git `aa7ab4306e269075784c7ac3eb4b45e6e6c53445`. Files: `pcsx2/USB/usb-pad/usb-pad.h`,
  `usb-pad.cpp`, `usb-pad-ff.cpp`, `usb-pad-sdl-ff.cpp/.h`, `usb-pad/lg/lg_ff.cpp/.h`, `pcsx2/USB/USB.cpp`. No GS code was read.
- PCSX2 is GPL. This page holds **facts and behaviour in prose and tables only**: device identity, byte layouts, command
  meanings. No PCSX2 code, structure or naming is reproduced, and none may be. Whoever implements our device model works
  from this prose, the game itself and public vendor documents, never from PCSX2 or Linux driver sources.
- PCSX2 is a **secondary** source: an emulator's choices, some of them marked as guesses in its own comments. Every fact
  below must be confirmed against GT4 (the game's own modules and the packets it sends) or real hardware before it is
  relied on. Line references are to the revision above.

## Identity PCSX2 presents

| Item | Value | Where |
|---|---|---|
| Vendor | 0x046D (Logitech) | usb-pad.h:357-368 |
| Product IDs | Driving Force 0xC294, **Driving Force Pro 0xC298**, DFGT 0xC29A, GT Force 0xC293, G25 0xC299, G27 0xC29B | usb-pad.h:357-368 |
| DFP revisions | bcdDevice 0x1106 (default DFP type) and 0x1102 (a variant whose pedal bytes differ; PCSX2 calls it buggy hardware) | usb-pad.h:404-436 |
| Device descriptor | USB 1.1, class 0, EP0 max packet 8, one configuration; product string index 1 ("Logitech Driving Force Pro"), manufacturer index 3 ("Logitech"), no serial | usb-pad.h:421-436, usb-pad.cpp:15-35 |
| Configuration | 41 bytes, one HID interface (class 3, subclass 0, protocol 0), self-powered, 80 mA | usb-pad.h:877-921 |
| Endpoints | EP1 interrupt IN, 8-byte packets, 10 ms; EP2 interrupt OUT, 8-byte packets, 10 ms | usb-pad.h:877-921 |
| HID report descriptor | X 14 bits (0..16383), 14 buttons, 4-bit hat (0..7 plus null), a constant byte, Y 8 bits, Rz 8 bits, a constant byte; a vendor output report of 7 bytes | usb-pad.h:590-640 |

PCSX2 does not say why GT4 enables 900 degrees. Hypothesis: the product ID (0xC298) and report layout make GT4 treat the
device as a DFP, and the game then sends the range command itself.

## Input report (interrupt IN, 8 bytes, little-endian 64-bit word)

| Bits | Content |
|---|---|
| 0-13 | Steering, 14 bits; centre 0x1FFF; lower = left, higher = right, up to 16382 |
| 14-27 | 14 button bits: 0 cross, 1 square, 2 circle, 3 triangle, 4 R1 (shift up), 5 L1 (shift down), 6 R2, 7 L2, 8 select, 9 start, 10 R3, 11 L3; 12-13 unused by PCSX2 |
| 28-31 | Hat: 0 up, then clockwise in 45-degree steps to 7 up-left; 8 released |
| 32-39 | Zero |
| 40-47 | Throttle, inverted: 255 released, 0 fully pressed |
| 48-55 | Brake, inverted the same way |
| 56-63 | Constant 0x11 (PCSX2's comment guesses it "enables wheel and pedals") |

- Pedals are separate axes in this layout. Gear paddles are the R1/L1 bits. No clutch and no H-shifter exist in the DFP
  contract.
- The 11.02 variant packs throttle and brake as 6-bit fields in the upper word with fixed marker bits; PCSX2 gives no
  source for that pattern.
- PCSX2 answers every IN token with a full report (no NAK when unchanged), and has no mode switch: the chosen device type
  fixes the format. The plain Driving Force reports 10-bit steering.

## Force feedback (interrupt OUT, 8 bytes)

Byte 0: low nibble = command, high nibble = slot mask (4 slots). Byte 1: effect type (or extended code). Bytes 2-6: five
parameters. Byte 7: unused. Source: usb-pad-ff.cpp:86-294, usb-pad.h:128-157.

| Command | Meaning (PCSX2 handling) |
|---|---|
| 0x0 download | stores the effect type per slot |
| 0x1 download and play | stores and applies the effect |
| 0x2 play | defined, not handled |
| 0x3 stop | stops, per slot, the effect type last stored there |
| 0x4 / 0x5 | default spring on / off (off with all slots: constant force to centre) |
| 0x8 normal mode, 0x9 LED, 0xB raw mode, 0xE set default spring, 0xF set dead band | logged, not handled |
| byte 0 = 0xF8 | extended: 0x01 DFP mode, 0x02 range 200 degrees, 0x03 range 900 degrees, 0x09 change mode, 0x0A revert identity, 0x10/0x11 G25 mode, 0x12 RPM LEDs, 0x81 wheel range; **none implemented** in PCSX2 |

Effect types: 0x00 constant, 0x01 spring, 0x02 damper, 0x03 auto-centre, 0x04-0x07 periodic shapes, 0x08 variable,
0x09 ramp, 0x0A square, 0x0B high-res spring, 0x0C high-res damper, 0x0D high-res auto-centre, 0x0E friction.

- **Constant:** one force byte per slot, 128 = zero, below = one direction, above = the other; with all four slots the
  centred bytes are summed and clamped. PCSX2 maps the byte to a signed 16-bit level.
- **Spring / high-res spring:** two dead-band bytes, coefficient nibbles k1/k2, sign nibbles, a clip (saturation) byte;
  high-res uses 11-bit dead bands. Coefficient table for k = 0..7: 1/16, 1/8, 3/16, 1/4, 3/8, 2/4, 3/4, 1 (high-res: k/15).
- **Damper / high-res damper:** coefficient and sign per side, clip; centre and dead band 0.
- **Friction:** two coefficients, clip, sign nibble (PCSX2 labels its conversion a guess).
- **Auto-centre:** strength from k1 and clip (PCSX2 labels the conversion a guess); the high-res auto-centre is not handled.
- **Unhandled in PCSX2:** periodic shapes, ramp, square, high-res auto-centre, the range and mode commands.

## Host mapping in PCSX2 (behaviour only)

- Host force feedback through SDL3 haptics only. Four persistent effects are created at open when the device supports
  them (constant, spring, damper, friction), all on the steering axis with infinite duration, started once and then only
  updated; auto-centre through SDL's auto-centre call.
- PCSX2's comment notes that GT4 uses one indefinite spring to centre the wheel in menus and one long constant force in
  turns, and that restarting effects breaks many wheels.
- No special handling of focus loss or pause in these files; a guest reset clears slot state without stopping host
  effects (a stuck force is possible there).

## USB plumbing worth knowing

- The emulated root hub swaps its two ports relative to the user's port numbers; PCSX2's comment says GT4 does not turn
  the wheel when this is wrong (USB.cpp:55-61). Any model of ours must establish GT4's port expectation from the game.
- SET_REPORT and SET_IDLE are accepted and ignored. IN on endpoints other than 1 stalls. Re-enumeration (mode switching)
  is not emulated.

## What PCSX2 cannot tell us

Whether GT4 sends the 900-degree command or relies on identity alone; the exact packets GT4 sends in each scene; whether
it checks bcdDevice 11.02 versus 11.06; how a real DFP scales steering after a range change; the meaning of byte 7 and of
the unused parameter bytes; the sign and gain on a real wheel. These belong to stage W0 and W3 of the plan.
