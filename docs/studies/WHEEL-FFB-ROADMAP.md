> **Study by Luan Antony**, from pull request #151 (issue #88), imported on 2026-11-04 at the owner's word.
> Task numbers inside are the author's own numbering (TASK-018 / TASK-123); in our TASKS.md this study belongs to TASK-018 and TASK-123.
> It is a study, not a decision: anything it proposes is checked again before it is built.

# Racing Wheel, Multi-Device & Force Feedback Architecture Roadmap (Issue #88 / TASK-018 / TASK-123)

## 1. Executive Summary

Upstream Issue **#88** (*"Wheel support and FFB"*, requested by `@benmatthews202-cyber` on `moesuito/gt4-staticrecomp`) requested:
> *"Implement Wheel support and FFB.. ability to add multiple USB devices such as separate pedals using a controls menu, Shifter (H-pattern shifting) and handbrake."*

The repository owner João Alano confirmed:
> *"Planned for 1.4.0: wheel support with force feedback, plus a controls page for separate pedals, shifter and handbrake. No date yet."*

This roadmap establishes the authoritative technical architecture for **TASK-018** (Racing Wheel Support) and **TASK-123** (Native DFP Protocol Research) to implement full sim-racing peripheral support in GT4Recomp Release 1.4.0.

---

## 2. Guest Identity: Why Native Driving Force Pro (DFP) Emulation is Mandatory

### 2.1 The Limitations of Gamepad / DualShock 2 Emulation
Previous proof-of-concept integrations (such as historical PR #101/#109) bridged wheel steering and pedals into standard DualShock 2 gamepad axes (`cross_square` or `rstick_y` pedal modes). While functional for basic input, gamepad bridging has severe flaws:
1. **Low Precision:** DualShock 2 analog axes are restricted to 8-bit resolution ($-128 \dots +127$, only 255 discrete steps across the full steering range).
2. **Artificial Steering Dampening:** In `CORE.GT4`, gamepad inputs pass through an internal steering assist pipeline:
   - Dynamic steering speed limits that reduce maximum steering angle at high speeds to prevent spinouts on gamepads.
   - Non-linear speed-sensitive input smoothing.
   - Artificial centering assist that fights real physical wheel rotation.
3. **Lack of Physics-Driven FFB:** DualShock 2 only receives 2-motor vibration signals (small high-frequency motor and large low-frequency weight motor). This produces vague rumble rather than directional tire-aligning torque or vehicle slip feedback.

### 2.2 The Native DFP Advantage
When Gran Turismo 4 detects an authentic **Logitech Driving Force Pro (DFP)** on the virtual PS2 USB bus:
- **14-Bit Steering Resolution:** Unlocks 16,384 discrete steps of steering resolution across $900^\circ$ of rotation (0.055 degrees per tick).
- **Zero Gamepad Filtering:** GT4 disables all speed-sensitive steering angle clamps and artificial smoothing, transmitting $1:1$ linear steering rack kinematics directly to the vehicle physics model.
- **Genuine Force Feedback Calculation:** GT4's physics engine calculates true mechanical tire forces:
  - **Self-Aligning Torque (SAT):** Tire contact patch pneumatic trail and caster torque resisting turn-in.
  - **Understeer Scrub:** Loss of steering resistance when front tires exceed maximum slip angle.
  - **Surface Textures:** High-frequency rumble from road rumble strips, gravel, grass, and curbing.
  - **Weight Transfer:** Dynamic steering weight reduction during braking dive and load increases under heavy compression.

---

## 3. Multi-USB Device Architecture (Composite Sim-Rig Integration)

Modern sim-racing setups almost universally use **separate, independent USB devices** plugged into the PC:
- **Wheelbase:** Direct Drive (Fanatec Podium/CSL DD, Moza R5/R9/R12/R16, Simucube 2, Asetek) or belt/gear-driven bases (Thrustmaster T300/T248, Logitech G29/G920/G923).
- **Pedals:** Independent USB load-cell pedals (Heusinkveld Sprint/Ultimate, Fanatec ClubSport V3, Moza CRP, Thrustmaster T-LCM).
- **Shifter:** Dedicated USB H-pattern or sequential shifter (Thrustmaster TH8A/TH8S, Fanatec ClubSport Shifter, Moza HGP).
- **Handbrake:** Dedicated USB analog or digital hall-effect handbrake.

### 3.1 Composite Device Layer (`gt4l-core::controls` & `native/host_io`)
The host input subsystem utilizes SDL3 Joystick and Gamepad APIs to aggregate disparate physical USB controllers into a single unified virtual DFP device state:

```
[Physical Peripherals]
  ├── USB Wheelbase (GUID A)    ──► Steering Axis, D-Pad, Face Buttons
  ├── USB Pedals (GUID B)       ──► Throttle (Axis 1), Brake (Axis 2), Clutch (Axis 3)
  ├── USB Shifter (GUID C)      ──► H-Pattern Gates / Sequential Switches
  └── USB Handbrake (GUID D)    ──► Analog Handbrake (Axis 0)
                 │
                 ▼
     [Host Composite Layer]
     • GUID-qualified bindings (e.g. `dev:0300...:axis:1`)
     • Per-axis calibration (deadzone, saturation, polarity, curves)
     • Input normalization (-32768..32767 -> DFP report)
                 │
                 ▼
     [Virtual DFP USB Device]
     • 14-bit steering angle (0..16383)
     • 8-bit throttle (0..255)
     • 8-bit brake (0..255)
     • Digital buttons & D-pad
                 │
                 ▼
[GT4 Engine / SIF USB Transport (PDIUSB / LGDEV)]
```

### 3.2 GUID-Qualified Persistent Bindings (`wheel.toml`)
To prevent device enumeration index shifts across Windows reboots, every binding is tagged with the hardware vendor/product GUID:
```toml
version = 2
[wheel]
enabled = true
preset = "custom_composite"
primary_wheel_guid = "030000006d0400004fc2000000000000" # Wheelbase GUID
primary_wheel_name = "Logitech G29 Driving Force Racing Wheel"

[steering]
source = "dev:030000006d0400004fc2000000000000:axis:0"
rotation_degrees = 900
effective_degrees = 900
deadzone = 0.00
linearity = "linear"

[throttle]
source = "dev:03000000790000000600000000000000:axis:1" # Separate USB Pedals
mode = "inverted"
deadzone = 0.02
saturation = 0.98

[brake]
source = "dev:03000000790000000600000000000000:axis:2"
mode = "inverted"
deadzone = 0.03
saturation = 0.95

[handbrake]
source = "dev:03000000b40400001201000000000000:axis:0" # Standalone USB Handbrake
deadzone = 0.05
saturation = 0.90

[shifter]
mode = "h_pattern" # "sequential" or "h_pattern"
gear_1 = "dev:030000004f0400000800000000000000:btn:0"
gear_2 = "dev:030000004f0400000800000000000000:btn:1"
gear_3 = "dev:030000004f0400000800000000000000:btn:2"
gear_4 = "dev:030000004f0400000800000000000000:btn:3"
gear_5 = "dev:030000004f0400000800000000000000:btn:4"
gear_6 = "dev:030000004f0400000800000000000000:btn:5"
reverse = "dev:030000004f0400000800000000000000:btn:6"
```

---

## 4. Shifter (H-Pattern) & Handbrake Technical Integration

### 4.1 Gran Turismo 4 Native Transmission Constraints
- In Gran Turismo 4, the original Logitech DFP hardware featured a 2-way sequential shift stick (+/-) and steering wheel paddle buttons.
- GT4's core transmission logic responds natively to sequential shift commands:
  - Shift Up: `DFP_BTN_SHIFT_UP` (Paddle Right / Stick Forward)
  - Shift Down: `DFP_BTN_SHIFT_DOWN` (Paddle Left / Stick Backward)

### 4.2 H-Pattern Support Architecture
To support physical H-Pattern gated shifters (1st, 2nd, 3rd, 4th, 5th, 6th, Reverse):
1. **Sequential Micro-Step Emulation (Universal / No-Mod Path):**
   - When a gate is engaged (e.g. player shifts from 2nd to 3rd gear), the host input manager monitors the current in-game gear index via an EE memory hook (`RaceState__currentGear`).
   - The engine automatically issues the required sequential shift pulse(s) to align the in-game transmission with the physical gate.
   - Clutched shifts: If the player has a clutch pedal mapped, gear engagement only occurs when the clutch pedal exceeds its disengagement threshold ($> 60\%$), simulating realistic missed shifts.
2. **Direct Transmission Injection Hook (Native Recomp Path):**
   - Using the static recompiler's symbolic addresses, hook the transmission state update in `CORE.GT4` (`CarTransmission::requestGear(int gear)`), directly writing the target gear index when the physical H-pattern gate closes.

### 4.3 Analog & Digital Handbrake Integration
- **Digital Handbrake:** Maps directly to `DFP_BTN_CIRCLE` (GT4 default emergency brake button).
- **Analog Handbrake:**
  - Reads analog voltage / load cell pressure ($0.0 \dots 1.0$).
  - When pressure exceeds threshold ($> 5\%$), triggers handbrake engagement.
  - In native mode, writes progressive handbrake torque to the rear brake pressure register in vehicle physics.

---

## 5. Force Feedback Pipeline & Protocol Translation

### 5.1 Logitech DFP Protocol Telemetry
GT4 communicates with the DFP via the PS2 USB controller (`PDIUSB.IRX` / `LGDEV.IRX`). The game issues standard Logitech Force Feedback command packets:
1. **Constant Force (`0x01`):** Directional steering torque vector representing cornering tire lateral grip and steering rack load (signed 16-bit or 8-bit magnitude, $-10000 \dots +10000$).
2. **Spring / Auto-Center Effect (`0x02`):** Speed-dependent caster return force centering the wheel based on vehicle velocity.
3. **Damper Effect (`0x03`):** Dynamic damping simulating tire friction against asphalt and steering fluid resistance.
4. **Surface / Periodic Rumble (`0x04`):** Sinusoidal / sawtooth vibrations for curbing, gravel, and engine vibration.

### 5.2 Translation to Modern PC Wheels via SDL3 Haptics
The native runtime translates incoming DFP force packets into **SDL3 Haptic / FFB subsystem** structures:
- `Constant Force` $\longrightarrow$ `SDL_HapticConstant`
- `Spring Effect` $\longrightarrow$ `SDL_HapticSpring`
- `Damper Effect` $\longrightarrow$ `SDL_HapticDamper`
- `Periodic Rumble` $\longrightarrow$ `SDL_HapticPeriodic`

### 5.3 Critical FFB Safety Lifecycle Controls
To ensure player safety and protect high-torque Direct Drive wheelbases (up to 25 Nm of torque):
- **Focus Loss Silence:** If the GT4Recomp window loses focus (Alt+Tab), all active FFB effects and motor torques are instantaneously commanded to zero.
- **Race Pause / Menu Transition:** When entering in-game menus, replay mode, or pausing, high-gain forces are immediately suppressed, leaving only a soft centering spring active.
- **Torque Limiter & Master Gain:** The launcher provides a master FFB strength slider ($0\% \dots 100\%$) and a high-frequency torque filter to prevent clipping and aggressive oscillation.

---

## 6. Launcher UI Specification ("Controls > Racing Wheel")

The Tauri launcher UI will feature a dedicated **Racing Wheel & Pedals** settings tab:
1. **Device Assignment Dropdowns:**
   - Wheelbase Device
   - Pedals Device (supports "Combined with Wheelbase" or "Independent USB Device")
   - Shifter Device (None, Sequential, or H-Pattern)
   - Handbrake Device (None, Digital Button, or Analog Axis)
2. **Interactive Calibration Visualizers:**
   - Real-time steering wheel angle graphic with center deadzone and $900^\circ$ rotation limiter.
   - Real-time bar graphs for Throttle, Brake, Clutch, and Handbrake displaying raw vs. calibrated values.
   - Polarity inversion checkboxes (`Invert Throttle`, `Invert Brake`).
3. **Force Feedback Settings:**
   - Master FFB Gain slider ($0\% \dots 100\%$).
   - Steering Damper strength slider ($0\% \dots 100\%$).
   - Center Spring strength in menus ($0\% \dots 100\%$).
   - **Safe "Test Pulse" button:** Sends a gentle, 200 ms low-torque rumble to verify motor communication without sudden violent movement.

---

## 7. Phased Implementation Roadmap for Release 1.4.0

| Phase | Milestone | Deliverables |
|---|---|---|
| **Phase 1: Protocol Research (TASK-123)** | Guest DFP Protocol & Hook Mapping | Reverse-engineer `PDIUSB.IRX` and `LGDEV.IRX` descriptors; document exact USB HID endpoints and report structures. |
| **Phase 2: Composite Input Abstraction** | Multi-Device Host Aggregator | Implement SDL3 multi-device polling, GUID device resolution, axis calibration, and H-pattern shifter decoding. |
| **Phase 3: Native Wheel Driver Injection** | Virtual DFP USB Transport | Inject virtual DFP reports into the static recompilation SIF/IOP layer, enabling native $900^\circ$ steering and zero-dampening physics. |
| **Phase 4: FFB Protocol Bridge** | Real Force Feedback Pipeline | Translate DFP force packets into SDL3 Haptic structures; implement safety mute on focus loss and race end. |
| **Phase 5: Launcher UI & Calibration** | Controls > Racing Wheel Frontend | Build HTML/Tauri calibration UI with live telemetry bars, profile presets (Logitech, Thrustmaster, Fanatec, Moza), and FFB tuning controls. |
| **Phase 6: Owner Hardware Verification** | Physical Acceptance Testing | Physical testing on real hardware rigs (G29, T300, Fanatec CSL DD) for steering feel, pedal response, and stability. |
