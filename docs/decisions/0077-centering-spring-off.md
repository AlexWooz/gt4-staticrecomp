# 0077: the game's centring spring is off by default (`[ffb] centering_spring`)

Date: 2026-11-06. TASK-192. From the owner's rc15 report: "the steering is heavy and not natural; from the moment the game
opens, the wheel keeps slowly coming back to the centre, all the time" (Logitech G27). The owner's conclusion: "I think it
has to be disabled".

## Decision

`wheel.toml` gets `[ffb] centering_spring = "off" | "game"`, and the **default is `off`**:

- `off` plans no spring at all: neither GT4's own spring effects (types `01`, `0B`, `03`, `0D`) nor the DFP's default
  spring (`autocenter`).
- `game` is the behaviour of rc15 and earlier: the game's spring plays when it sends one, and the DFP default spring plays
  while it is on.
- Nothing else changes: the racing constant force, the damper, the friction and the way GT4's own force feedback strength
  setting (Strong/Medium/Weak) scales them.
- A 1.2.x file keeps loading: the old `[ffb] centering_spring` amount or boolean is read (nonzero = `game`), and the
  "centring" spelling is accepted too.

## Why

- **Measured on the rc15 build, headless, with the virtual wheel** (`GT4_DFP=1 GT4_DFP_TRACE=1`, the census and
  `dfp-forces.csv`), through the menus and two bench drives (`licenca-b1`, v2.00; `el-capitan`, Spec II), with a steering
  sweep (`GT4_DFP_TEST_STEER=60`) because GT4's racing force depends on the wheel angle:
  - boot: `F3`, `F4` twice, `F5` (the DFP default spring goes off and stays off), `F8 03` (900 degrees);
  - menus: a high-resolution spring, type `0B`, slot 1 or 2, `21 0B 7F 7F 77 00 80`: k = 7 on both sides (7/15 = 0.47),
    clip 0x80 (0.5), centred, no dead band; stopped (`23 0B`) before a drive;
  - drive: no spring, no damper; friction on slot 1 (`0E`, coefficient 0x80, clip 0x20 = 0.125); the variable force (`08`)
    on slot 0 about 100 times a second, its level following the wheel angle (mean 0x2D left, 0x80 at the centre, 0xCA
    right).
- **So the pull is the game's own spring, played as the game asked.** Coefficients, clip and centre all decode to what the
  bytes say; the DFP default spring was already off; no friction or damper was mapped as a spring. The decoder is not the
  problem: the game wants a centring force in its menus, and on the owner's G27 that reads as an unnatural, permanent pull.
- **The alternative is not "no spring ever"**: `game` keeps it for players who want the DFP behaviour, and a wheel's own
  firmware spring (Logitech G HUB / Gaming Software) is outside this key.
- **Why the default changes for everyone**: the spring is a comfort behaviour, not a gameplay one; it does not touch the
  simulation and it cannot make a car faster or slower. Fidelity here is the owner's feel, and he asked for it disabled.

## Consequences

- rc15 and later report `centering spring off|game` at the end of the `[wheel] force feedback summary` line.
- The launcher still writes `wheel.toml` itself and does not know the key yet: `gt4l-core`'s `wheel.rs` refuses unknown
  `[ffb]` keys, so a hand-edited file that contains `centering_spring` is refused by the launcher (the game loads it and
  logs nothing). The Wheel page row ("Centring spring": Off / Game) is a follow-up task.
- Tests: `native_usb_dfp` decodes GT4's exact command bytes (the spring, its stop, the friction and the racing force);
  `native_wheel` checks that `off` plans no spring and that the racing force, damper and friction do not change.
