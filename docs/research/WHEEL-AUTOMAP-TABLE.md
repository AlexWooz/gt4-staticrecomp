# Racing wheel auto-map table for the DFP bridge

Status: research input for the launcher Wheel page (TASK-018, PR #205). 2026-11-05. en-US.

## Provenance and limits

- Method: web research (read-only) plus the PR #205 worktree files `docs/WHEEL-NATIVE-DFP.md`, `docs/WHEEL-DFP-PLAN.md`,
  `docs/research/DFP-PCSX2-FACTS.md`, `native/wheel.hpp`, `native/wheel.cpp`.
- No PCSX2 or other emulator source was read. No Linux kernel driver *source* was read. Linux driver READMEs and
  compatibility tables were used only as a list of USB IDs and model names (facts, not code).
- **Honest summary of the evidence:** USB IDs and SDL's wheel classification are well sourced. **Exact button indices,
  axis indices and axis polarity per wheel are mostly not published anywhere I could reach**, and the sandbox has no
  hardware. Where an index is not known the table says "manual mapping needed" (or "capture wizard") and does not
  guess. Every unknown index should be filled by one real-hardware capture per wheel (see "Unknowns").
- Confidence labels: **Confirmed** (SDL source, vendor doc, or primary source read in this task), **High**, **Hypothesis**,
  **Unknown**.

Sources used (S-numbers are cited below):

- S1: SDL `src/joystick/SDL_joystick.c` (`initial_wheel_devices`, `SDL_IsJoystickWheel`, `SDL_GetJoystickGUIDType`),
  https://github.com/libsdl-org/SDL/blob/main/src/joystick/SDL_joystick.c (zlib).
- S2: SDL `src/joystick/usb_ids.h` (vendor IDs): https://github.com/libsdl-org/SDL/blob/main/src/joystick/usb_ids.h
- S3: JacKeTUs Linux steering-wheel compatibility table (IDs and model names only):
  https://github.com/JacKeTUs/linux-steering-wheels
- S4: berarma new-lg4ff README (Logitech models and IDs): https://github.com/berarma/new-lg4ff
- S5: hid-fanatecff README (Fanatec IDs, PC mode): https://github.com/gotzl/hid-fanatecff
- S6: hid-tmff2 README (Thrustmaster support notes): https://github.com/Kimplul/hid-tmff2
- S7: universal-pidff README (Moza, Cammus, PXN, others): https://github.com/JacKeTUs/universal-pidff
- S8: mescon Logitech trueforce wiki, Button Reference (G923 PS and RS50 / G PRO indices):
  https://github.com/mescon/logitech-trueforce-linux-driver/wiki/Button-Reference
- S9: simagic-ff README (shared Simagic ID): https://github.com/JacKeTUs/simagic-ff ; SDL commit adding Simagic:
  https://discourse.libsdl.org/t/sdl-detect-simagic-wheel-bases-as-wheels-8198-1d495/45866
- S10: GTPlanet forum posts (G27 console labels), https://www.gtplanet.net/forum/threads/setting-up-g27-for-gt5p.123266/
- S11: this repo, PR #205 worktree files listed above.
- S12: driverscloud / drivermax listings (Thrustmaster Windows hardware IDs), low quality, cited only where noted.

## The target: what the DFP contract needs (from S11)

- Steering 14-bit, centre 0x1FFF. GT4 itself sets 900 degrees (command `F8 03`), so the **host wheel should be at
  its full physical range (900 degrees, or the wheel's maximum) and `rotation_degrees` in `wheel.toml` must equal it**.
- Throttle and brake are **separate** axes (the DFP report is inverted, 255 = released). The DFP has **no clutch and no
  H-shifter**. Face buttons Cross, Circle, Square, Triangle; R1, L1, R2, L2, R3, L3; Start, Select; an 8-way hat; two
  gear paddles (the PR's `wheel.toml` calls the paddle slots `r2` = shift up and `l2` = shift down in DFP mode; the
  report layout lists the paddles at the R1/L1 bits. The launcher author should confirm which slot name is the paddle
  in `native/wheel.cpp` before writing templates; this document uses "paddle up / paddle down" for that reason).
- Existing code facts that matter for the table (S11, `native/wheel.cpp`):
  - Pedal polarity modes: `inverted` (+32767 at rest), `normal` (-32768 at rest), `positive`, `combined_negative`,
    `combined_positive`. Sources are `axis:N`, `axis:N+`, `axis:N-`, `btn:N`, `hat:H:dir`, optionally `dev:<guid>:...`.
  - Presets today: `generic`/`logitech_g29` (steer axis 0, throttle axis 1, brake axis 2, clutch axis 3, all
    `inverted`), `hori`, `logitech_dfgt`, `thrustmaster` (throttle axis 2, brake axis 1), `fanatec_moza`
    (polarity `normal`), `combined_pedals`.
  - **Bug found in `is_known_wheel_vendor`:** it lists `0x1209` as "Moza". MOZA's vendor ID is **0x346E** (S1
    `MAKE_VIDPID(0x346e, ...)`); 0x1209 is the generic OpenFFBoard ID in SDL's list. MOZA wheels are only found
    today through SDL's wheel flag or the name list. Also missing from the vendor list: Simagic (0x0483, 0x3670),
    Turtle Beach (0x10F5), PXN (0x11FF, 0x36E6). Candidate detection should not rely on that function alone (see
    "How the launcher should use this").
  - Name list in `is_candidate_wheel_device` has no "moza", "fanatec", "csl", "velocity", "pxn", "simagic", "cammus",
    "t818", "tmx", "tx ", "ts-xw".

## SDL3 on Windows: the cross-cutting facts

- **Wheel flag (Confirmed, S1):** SDL marks a device as `SDL_JOYSTICK_TYPE_WHEEL` if its VID:PID is in `initial_wheel_devices`
  (71 entries in current main, listed below per wheel), or the VID:PID is added through hint
  `SDL_HINT_JOYSTICK_WHEEL_DEVICES`, or (XInput devices) the XInput subtype is `0x02` (wheel). A wheel type does **not**
  imply it is a gamepad; wheels are normally plain joysticks (no gamepad mapping), so axis indices are the raw
  HID/DirectInput order, not SDL gamepad names.
- **Axis order (High):** for DirectInput/HID joysticks SDL numbers axes in the order the device reports them (usually
  X, Y, Z, Rx, Ry, Rz, then sliders). Pedals are commonly Y/Z/Rz and rest at +32767 (the `inverted` mode). This is the
  convention the repo's `generic` preset assumes; it is **not verified per wheel here**.
- **Xbox-edition wheels and XInput (Hypothesis):** wheels that present as XInput (Xbox-licensed G920, G923 Xbox, T248X,
  Xbox-side Hori) may reach SDL as a gamepad-like device with steering on the left stick and the pedals on the two
  triggers, and XInput has no force feedback. To get haptics the launcher may need the DirectInput/HIDAPI path
  (SDL hints such as `SDL_HINT_XINPUT_ENABLED=0`, `SDL_HINT_JOYSTICK_RAWINPUT`). **Test with hardware before claiming FFB.**
- **FFB (Confirmed from the PR, S11):** the bridge uses one SDL haptic handle with persistent constant, spring, damper
  and friction effects; periodic/ramp are not played. Using SDL rumble and haptics together crashed MOZA R5 in `pid.dll`; a
  device without autocentre (R5) must not get autocentre calls.
- **900-degree range (High, general):** SDL haptics cannot set the rotation range. On Windows it is set by the vendor
  driver (Logitech G HUB / Profiler, Thrustmaster Control Panel, Fanatec Control Panel / FanaLab, MOZA Pit House,
  Simagic SimPro Manager, Cammus and PXN apps). The launcher can only tell the user the number to set and put the same
  number in `rotation_degrees`.

## Summary table

"Pedals" = how throttle/brake/clutch arrive. "Auto-map" = yes (template with confident indices), partial (axes/pedals
and physical labels known, button indices need a short capture), no (manual only). Index columns are deliberately
absent where unknown.

| Wheel | VID:PID | SDL view | Pedals | Auto-map | FFB notes |
|---|---|---|---|---|---|
| Logitech Driving Force / Formula Force EX (and compat mode of most Logitech wheels) | 046D:C294 | wheel list ("generic wheel", S1) | separate throttle/brake (Hypothesis); 10-bit steering | partial | needs Logitech driver for FFB (High); 200 deg only |
| Logitech Driving Force Pro | 046D:C298 | wheel list (S1) | separate Y/Rz (Confirmed from PR facts for the emulated layout; real device High) | **yes** (it is the target layout) | native 900 deg; constant/spring/damper (High) |
| Logitech Driving Force GT | 046D:C29A | wheel list (S1) | separate; no clutch | partial | needs LGS/Profiler for 900; FFB High |
| Logitech G25 | 046D:C299 | wheel list (S1) | separate throttle/brake/clutch | partial | Logitech driver needed on Windows (High) |
| Logitech G27 | 046D:C29B native (C294 / C298 in compat modes) | wheel list (S1) | separate throttle, brake, clutch (axes High, indices Hypothesis) | partial (owner's wheel; capture once) | LGS/Profiler needed; 900 deg native (High) |
| Logitech G29 (PS3 mode) | 046D:C24F (PS4 mode C260) | wheel list (S1) | separate; clutch; paddles | partial to yes | G HUB; 900 deg; FFB constant/spring/damper (High) |
| Logitech G920 | 046D:C261 initial, C262 active | wheel list (S1); XInput risk | separate on DInput (Hypothesis) | partial | HID++; G HUB; XInput path has no FFB (Hypothesis) |
| Logitech G923 PS/PC | 046D:C266 (PC mode), C267 (PS4 mode) | wheel list (S1) | separate | **yes for buttons (S8)**, pedal axes Hypothesis | G HUB; TrueForce is extra; classic FFB High |
| Logitech G923 Xbox/PC | 046D:C26D and C26E (S1 lists both) | wheel list; XInput risk | separate on DInput (Hypothesis) | partial | as G920 |
| Logitech G PRO (PS/PC) | 046D:C268 (PC mode), C269 (PS mode) | wheel list (S1) | separate | partial (S8 covers RS50/G PRO buttons) | G HUB; direct drive; see notes |
| Logitech G PRO (Xbox/PC) | 046D:C272 (PC mode) | wheel list (S1) | separate | partial (S8) | as above |
| Thrustmaster T150 | 044F:B677 | wheel list (S1) | separate | partial | Thrustmaster driver needed (High); T150 has firmware PS3/PS4 modes |
| Thrustmaster TMX | 044F:B67F | wheel list (S1) | separate | partial | Thrustmaster driver (High) |
| Thrustmaster T248 | 044F:B696 | wheel list (S1) | separate | partial | driver; screen not used |
| Thrustmaster T300RS (also T-GT II, shares ID) | 044F:B66E normal, B66F advanced, B66D PS4 | wheel list (S1) | separate (brake/throttle/clutch) | partial | driver; T-GT II reports as T300 (S6) |
| Thrustmaster TX | 044F:B664 initial, B669 active | wheel list (S1) | separate | partial | driver; initial mode needs the driver's init |
| Thrustmaster T-GT (original) | Unknown (S12 hints B685 for a Ferrari rim, not the base) | probably T300 family | Unknown | no | Unknown |
| Thrustmaster T818 | Unknown | Unknown (not in SDL list per S1) | Unknown | no | driver; direct drive; Unknown |
| Fanatec CSL Elite (PC) | 0EB7:0E03 (PS4 console mode 0EB7:0005) | wheel list (S1) | pedals via base or separate device (Hypothesis) | partial | Fanatec driver, base in PC mode (S5) |
| Fanatec CSL DD / GT DD Pro | 0EB7:0020 | wheel list (S1) | pedals via base (Hypothesis) | partial | PC mode, Fanatec driver (S5) |
| Fanatec ClubSport V2 / V2.5 | 0EB7:0001 / 0EB7:0004 | wheel list (S1) | as above | partial | as above |
| Fanatec Podium DD1 / DD2 | 0EB7:0006 / 0EB7:0007 | wheel list (S1) | as above | partial | as above |
| MOZA R3 / R5 / R9 / R12 / R16 / R21 | 346E:0005 / 0004 / 0002 / 0006 / 0000 / 0000 | wheel list (S1) | pedals plug into base, appear as axes on the base joystick (the PR contributor's R5 setup, S11) | partial | Pit House; R5 has no SDL autocentre (S11) |
| Simagic Alpha / Alpha Mini / Alpha Ultimate / M10 | 0483:0522 (one shared ID) | wheel list (S1, S9) | separate pedals device usually | partial | SimPro Manager; PID FFB |
| Simagic EVO Sport / EVO / EVO Pro | 3670:0500 / 0501 / 0502 (S3) | **not in S1 list** (Unknown) | Unknown | no | SimPro Manager |
| Turtle Beach VelocityOne Race | 10F5:7077 (S3) | **not in S1 list** | Unknown | no (contributor reports it works; see below) | Control Center |
| PXN V10 | 11FF:3245 (S3) | not in S1 list | Unknown | no | PXN app, PID FFB |
| PXN V9 | Unknown | not in S1 list | Unknown | no | Unknown |
| PXN VD6 | 36E6:400F (S1) | wheel list | Unknown | no | Unknown |
| Hori Racing Wheel Apex | Unknown (Hori VID is 0F0D, S2) | not in S1 list | probably triggers, no clutch (Hypothesis) | no | no FFB (High, product has none) |
| Hori Racing Wheel Overdrive | Unknown | Unknown | XInput triggers (Hypothesis, repo `hori` preset assumes it) | no | no FFB (High) |
| Cammus C5 / C12 | 3416:0301 / 3416:0302 (S1) | wheel list | Unknown | no | Cammus software; PID FFB (S7) |

## Wheel families

Each section: identities, SDL view, a proposed DFP mapping, FFB, sources. The mapping column uses the wheel's printed
labels. "[capture]" means the launcher template names the control and the wizard asks the user to press it once;
the index is not published. `wheel.toml` syntax from the repo: `axis:N`, `btn:N`, `hat:0:up`.

### Logitech DF / DFP / DFGT / G25 / G27 (the legacy family)

**Identity (Confirmed, S1/S3):** Driving Force/generic C294, MOMO Force C295, DFP C298, G25 C299, DFGT C29A, G27 C29B.
S3 lists G25 under C29B; SDL (S1) lists G25 as C299. Use SDL's value and treat C29B as "G27 (and G25 on some firmware)".

**Compatibility modes (High, general knowledge; not primary-sourced in this task):** a G25/G27/DFGT can emulate a
Driving Force Pro (and G27 can emulate DF-EX/DFP/G25) and, on a PC without Logitech software, often enumerates in the
compatibility PID (C294, sometimes C298) with a *reduced* feature set: fewer buttons, a combined pedal axis, no
clutch, 200 or 900 degree range depending on mode. The launcher should read the PID and warn "your Logitech wheel is
in compatibility mode; open Logitech Profiler/G HUB and set native mode". A wheel in **C298 mode is already a DFP**; the
launcher can use the DFP layout verbatim (the real DFP report: X steer, separate Y and Rz pedals, 12 buttons plus hat,
S11 facts).

**SDL view (Confirmed list membership S1; axes High/Hypothesis):** all are in SDL's wheel list. Native G25/G27:
X = steering, Y = clutch, Z = throttle, Rz = brake, a hat for the shifter D-pad (Hypothesis from convention; the repo's
`generic` preset assumes throttle axis 1, brake axis 2, clutch axis 3, which would be wrong for this family
unless SDL reorders axes. **Do not ship the generic preset for G27; capture the three pedals.**). G27 has 24 buttons.
Polarity: rest = +32767, press = -32768, so `inverted` (High, Logitech pedals are conventional).

**Physical labels (Confirmed S10, Wikipedia):** two steel paddles; on the wheel face four red buttons, a D-pad, and
extra round buttons labelled with PlayStation symbols on the face (Cross, Square, Circle, Triangle); on the wheel top
the red row; on the shifter, 8 buttons plus a D-pad and the H-pattern gears. Console users on PS3 report the paddles as
R1/L1, the face buttons by symbol and the four red buttons as L3, Select, Start, R3 from left to right (S10, forum
posts; consoles, not DirectInput indices).

**Proposed mapping to the DFP (Hypothesis on the numbering, High on the intent):**

| DFP control | G27 control | Index |
|---|---|---|
| Steering | wheel | axis X (axis 0) [capture to confirm] |
| Accelerator / Brake | pedals | [capture]; polarity `inverted` |
| Cross / Circle / Square / Triangle | the four PlayStation-symbol buttons on the wheel face | [capture] |
| Paddle up / down | right / left paddle | [capture] |
| R2 / L2 | the two extra buttons beside the paddles if present, else shifter black buttons | [capture] |
| R3 / L3 | red buttons on the right / left | [capture] |
| Start / Select | the two middle red buttons (per S10, left to right: L3, Select, Start, R3) | [capture] |
| D-pad | shifter D-pad (hat 0) | `hat:0:up/right/down/left` (High) |
| Clutch / H-gears | unused by the DFP | none |

**FFB (High):** on Windows the DirectInput FFB of G25/G27 only works with Logitech's driver (LGS or Profiler) installed;
the wheel's rotation is 900 degrees in native mode and must be set in Profiler/LGS (not by SDL). Constant, spring, damper
and friction effects are supported (High); periodic effects are not needed. Sources: S1, S4 (supported-model list), S10.
**Unknown:** whether SDL3 DirectInput haptics on the G27 handle persistent infinite spring/damper updates without
glitches; test first (the FFB test below).

### Logitech G29 / G920 / G923 / G PRO

**Identities (Confirmed, S1):** G29 PS3 mode C24F, PS4 mode C260. G920 initial C261, active C262. G923 PS4/PC: PC mode
C266, PS4 mode C267. G923 Xbox: C26D and C26E (SDL lists both; mode meaning Unknown). G PRO PS: C268 PC mode, C269
PS4/PS5 mode. G PRO Xbox: C272 PC mode. Linux note (S4): G920 and G923 Xbox use HID++ rather than the older
protocol; irrelevant to SDL on Windows but a hint that these are two protocol generations.

**SDL view:** all in SDL's wheel list. Buttons for G923 PS mode (S8, indices published for Windows DirectInput order):
0 X (Cross), 1 Square, 2 Circle, 3 Triangle, 4 right paddle, 5 left paddle, 6 R2, 7 L2, 8 Share (Select), 9 Options
(Start), 10 R3, 11 L3, 19 plus, 20 minus, 21 dial CW, 22 dial CCW, 23 dial push, 24 PS. D-pad is a hat. G PRO / RS50
(Xbox-labelled, S8): 0 A, 1 X, 2 B, 3 Y, 4 right paddle, 5 left paddle, 6 RT, 7 LT, 8 View, 9 Menu, 10 RSB, 11 LSB,
21-23 right encoder, 24-26 left encoder, 27 logo, 28 GL, 29 GR. The G29 in PS3 mode very likely uses the same PS
layout as the G923 (Hypothesis, same family and same labels). Axes: steering X; throttle, brake, clutch are separate
(Hypothesis; S8 does not list them). Rest values +32767 inverted (Hypothesis).

**Proposed mapping (G923/G29 PS layout, Confirmed for buttons by S8, Hypothesis for G29):**

| DFP control | G29/G923 PS control | Index |
|---|---|---|
| Steering | wheel | axis 0 (High) |
| Accelerator / Brake | pedals | axes to capture; `inverted` |
| Cross / Circle / Square / Triangle | X / Circle / Square / Triangle | btn 0 / 2 / 1 / 3 |
| Paddle up / down | right / left paddle | btn 4 / btn 5 |
| R2 / L2 | R2 / L2 | btn 6 / btn 7 |
| Select / Start | Share / Options | btn 8 / btn 9 |
| R3 / L3 | R3 / L3 | btn 10 / btn 11 |
| D-pad | hat 0 | `hat:0:*` |
| R1 / L1 (no distinct buttons) | plus / minus (btn 19 / 20) or encoder if the game needs them | btn 19 / 20 |

For G920 / G923 Xbox the label set is Xbox: A, B, X, Y, LB, RB, View, Menu; map A=Cross, B=Circle, X=Square, Y=Triangle,
paddles = paddle up/down, LB/RB = L1/R1 (the RB/LB are the wheel's bumper buttons). Index order on those wheels is
Unknown (the G PRO order in S8 is the best available clue, Hypothesis for G920/G923 Xbox).

**FFB (High):** G HUB sets rotation (900 on G29/G920/G923; G PRO 1080 max, set 900 for GT4). Constant, spring, damper
and friction are standard on these wheels. G HUB must be installed for the DirectInput FFB driver on Windows (High).
The "Xbox" wheels may arrive through XInput without haptics (Hypothesis; see cross-cutting section).
Pedals: three pedals; G29 clutch is a separate axis; none used by the DFP.

**Sources:** S1, S3, S4, S8.

### Thrustmaster (T150, TMX, T248, T300RS, TX, T-GT / T-GT II, T818)

**Identities (Confirmed, S1/S3):** T150 B677, TMX B67F, T248 B696, T300RS B66E (normal) / B66F (advanced, with F1 rim in
PS3 mode) / B66D (PS4), TX B664 (initial) / B669 (active), TS-XW B691 initial / B692 active. T-GT II shares the T300
ID (S6), so it appears as a T300RS; this also means the launcher cannot tell them apart and the same template works.
Original T-GT and T818: **Unknown** (a driverscloud listing ties B685 to a Ferrari F1 rim, which is not the base;
B689 is the TS-PC Racer, not T-GT; all of this is S12-quality). The T818 is not in SDL's wheel list as of main (S1).

**Windows driver requirement (High):** the Thrustmaster Control Panel (driver) is needed on Windows for FFB and for the
wheel to leave its "initial" mode; the TX and TS-XW have an `initial` PID until the driver switches them to the `active`
PID (S1 comments, S6 hid-tminit note). The launcher should show "install/open the Thrustmaster driver" if the PID is an
initial one.

**SDL view / axes (Hypothesis, only the repo preset gives a clue):** the repo's `thrustmaster` preset says "brake axis 1,
throttle axis 2, paddles buttons 0/1, cross 5, square 3, circle 4, triangle 2". That is the presets author's
observation for T150/T248/T300RS/TX and is **Hypothesis** until captured. The T300RS has 2 paddles, 4-8 face buttons,
a D-pad, MODE and PS/SHARE/OPTIONS buttons on the PS-licensed rim (retail listing, not index-level).

**Proposed mapping:** steering = axis 0 (Hypothesis); pedals = [capture]; the four symbol-labelled face buttons ->
Cross/Circle/Square/Triangle ([capture]); right/left paddle -> paddle up/down ([capture]); the shoulder/trigger buttons on the
rim -> R2/L2 and R1/L1 if present; Share/Options -> Select/Start ([capture]); D-pad hat 0. TX/T300RS pedals are
usually a 2- or 3-pedal set; the clutch axis is ignored. **Do not accept the `thrustmaster` preset's indices as
verified.**

**FFB (High):** driver-based DirectInput FFB, constant/spring/damper/friction; rotation range set in the Control Panel
(up to 1080); set 900. T300RS/T-GT II firmware older than v31 has known problems on Linux (S6, not relevant to Windows
but shows the version sensitivity).

**Sources:** S1, S3, S6, S11, S12.

### Fanatec (CSL Elite, CSL DD, GT DD Pro, ClubSport, Podium)

**Identities (Confirmed, S1/S3/S5):** ClubSport base V2 0EB7:0001; V2.5 0EB7:0004; CSL Elite base (PC) 0EB7:0E03;
CSL Elite base PS4 console mode 0EB7:0005; Podium DD1 0EB7:0006; Podium DD2 0EB7:0007; CSR/Forza 0EB7:0011;
**CSL DD, GT DD Pro and ClubSport DD share 0EB7:0020** (the rim is what differs; the base ID does not identify it).
Porsche rim 0EB7:0197 (several models). Pedals can be a separate USB device (CSL Elite Pedals 0EB7:6204; LC 6205, LC V2
6206; ClubSport V3 183B, S5).

**Mode quirk (Confirmed, S5):** the CSL Elite base must be in **PC mode** (red LED); the PS4 PID 0005 is console mode
and its input layout is different (Hypothesis). When the launcher sees 0005, tell the user to put the base into PC
mode and not attempt auto-map.

**SDL view / axes (Hypothesis):** one joystick for the base (steering X); the pedals either as extra axes on the base
(pedals wired into the base) or as a separate joystick (forum post, S-search result). Throttle/brake/clutch are
separate axes. Buttons depend on the **rim** fitted, not on the base; the rim (CSL Universal Hub, GT, McLaren, Formula, WRC,
Porsche...) changes the count (rim ID is reported on the wire per S5 but not in the HID VID:PID), so rim-specific labels
cannot be derived from VID:PID. **Auto-map is partial: steering and pedals can be templated, buttons need capture.**

**Proposed mapping:** steering axis 0; pedals [capture] (direct-drive/load-cell pedals often rest at -32768, i.e.
`normal`, which is what the repo's `fanatec_moza` preset assumes, Hypothesis); the four face buttons -> Cross/Circle/
Square/Triangle ([capture]); paddles -> paddle up/down; D-pad hat; Start/Select from the rim's start/select-like buttons.

**FFB (High):** Fanatec driver required on Windows; the rotation range is set in the Fanatec control panel/FanaLab
(set 900); "FFB strength" in the control panel scales force; DD bases are strong and should start with the
launcher's `[ffb] strength` low (see hints). Constant/spring/damper/friction/periodic are supported (High).

**Sources:** S1, S3, S5.

### MOZA (R3, R5, R9, R12, R16, R21)

**Identities (Confirmed, S1/S3):** R16/R21 346E:0000, R9 346E:0002, R5 346E:0004, R3 346E:0005, R12 346E:0006.
R16 and R21 share an ID.

**SDL view:** in SDL's wheel list. Devices define many buttons (S7 note: >80 buttons, more than Linux's limit, so the
rim's own count is high). The PR contributor tested **R5 + CS V2 rim + pedals on the base** (S11) and confirms haptics
work with one haptic handle; Moza pedals plugged into the base appear as axes of the base joystick (S11: "pedals on the
base"; axis numbers not recorded there). Rim variants (CS V2, GS V2, ES, FSR, KS) change buttons.

**Proposed mapping:** steering axis 0; pedals [capture]; face buttons [capture]; paddles [capture] (CS/GS rims have two
paddles); D-pad via the rim's hat if present. **Auto-map: partial.**

**FFB (High / Confirmed from PR):** Pit House must be running (High, direct-drive base needs its software); set the
"game steering angle" to 900 degrees inside Pit House; the R5 has no SDL autocentre (Confirmed, S11). Effects:
constant/spring/damper/friction work (S11 shows the bridge using them on the R5). High-speed straight-line wobble
is the known limit in S11, not wheel-specific.

**Sources:** S1, S3, S7, S11.

### Simagic (Alpha, Alpha Mini, Alpha Ultimate, M10, EVO)

**Identities:** Alpha/Alpha Mini/Alpha Ultimate/M10 all share 0483:0522 (Confirmed, S9, SDL). EVO Sport 3670:0500, EVO
3670:0501, EVO Pro 3670:0502 (S3, a compatibility table). SDL's wheel list does not contain the 3670 IDs (S1). The
simagic-ff note says the base is detected in "Normal mode" (S9). Pedals are usually a separate Simagic pedal device.

**SDL view:** 0483:0522 in the wheel list; the 3670 family Unknown. Axes, buttons: Unknown. **Auto-map: no (manual).**

**FFB:** Simagic SimPro Manager (rotation angle and force scaling); PID force feedback (High). Unknown quirks.

### Turtle Beach VelocityOne Race

**Identity:** 10F5:7077 (S3, a Linux compatibility table; S2 confirms 0x10F5 is Turtle Beach). Not in SDL's wheel list
(S1); SDL will see it as a joystick, which the launcher must still accept by VID:PID. The PR contributor tested a
**VelocityOne Multi-Shift** (the shifter accessory) with a MOZA wheel (S11), so the Multi-Shift shifter is known to be
readable as an auxiliary device. Everything else: Unknown. **Auto-map: no (manual).** FFB: PID-compatible direct drive
(S3 status "Platinum" on Linux); Windows requires Turtle Beach Control Center (High).

### PXN (V9, V10, VD6, V12)

**Identities:** V10 11FF:3245, V12 11FF:1212, V12 Lite 11FF:1112 / 11FF:1211 (S3); VD6 36E6:400F (S1, in SDL's wheel
list); V9 Unknown. **SDL view and mapping: Unknown.** The older PXN V900 and similar DragonRise-based wheels use
0079:1864 or 11FF:0511 and are in SDL's list (S1) with a limited, non-FFB-grade feature set. **Auto-map: no.**

### Hori Racing Wheel Apex / Overdrive

**Identity:** Hori vendor ID 0F0D (S2); product IDs Unknown. Neither is in SDL's wheel list (S1). The repo `hori`
preset treats them as XInput-style wheels (A/B/X/Y, LB/RB paddles, RT throttle axis 5, LT brake axis 2, `normal`
polarity, 270 degrees, no FFB). That preset's indices are the repo author's assumption, **Hypothesis**. Proposed mapping
(from that preset): A=Cross, B=Circle, X=Square, Y=Triangle, LB/RB = paddle down/up, Back=Select, Start=Start, hat =
D-pad, L3/R3 from the stick-click buttons. The Apex is a non-FFB 270-degree wheel (High), so the launcher should
disable FFB and warn about the 270-degree range (GT4 expects 900 in DFP mode; use the analog pad bridge instead,
`mode = "pad"`).

### Cammus (C5, C12)

**Identities (Confirmed, S1):** C5 3416:0301, C12 3416:0302. In SDL's wheel list. Axes/buttons: Unknown. Cammus software
sets the range. Standard PID FFB (S7). **Auto-map: no.**

## How the launcher should use this

1. **Match by VID:PID first** against a table built from this document (the PIDs marked Confirmed). SDL gives both the
   VID/PID (from the device GUID, already parsed by `extract_guid_vid_pid` in `wheel.cpp`) and the wheel flag.
   Treat a VID:PID in SDL's `initial_wheel_devices` as a wheel even if the name does not match, and extend the
   vendor/name tables with MOZA 346E (replace the wrong 1209 use), Simagic (0483:0522, 3670), Turtle Beach 10F5, PXN
   (11FF, 36E6), plus names "moza", "fanatec"/"csl"/"clubsport"/"podium", "simagic", "velocity", "pxn", "t818", "tmx".
2. **Fall back to name patterns** (case-insensitive substrings of the SDL device name), then to **SDL wheel type**,
   then offer the manual mapping.
3. **Per family, ship a template**, not per-model hard-coded indices: the template lists the controls and, only where
   Confirmed/High, the default index (G923/G29-PS buttons from S8, hat D-pad, steering = axis 0).
   The wizard asks the user to press each control that has no confident index (pedals, paddles, face buttons)
   in the order of the DFP's controls. For the owner's G27 the template is "legacy Logitech", which asks for pedals and
   the four face buttons and paddles once, then stores them keyed by `device_guid`.
4. **Pedal polarity:** run a 2-second rest/press capture; do not trust presets. Choose among the repo's five
   `PedalAxisMode`s from the rest value (+32767 -> `inverted`; -32768 -> `normal`; 0 -> `combined_*`).
5. **Modes:** detect compatibility/initial PIDs (Logitech C294/C298 on a G27/G25/DFGT; TX B664, TS-XW B691; G920 C261;
   Fanatec 0005 console mode) and show a plain-language hint rather than mapping.
6. **Rotation:** show "set the wheel's range to 900 degrees in <vendor software>" and write `rotation_degrees` to the
   same number; offer 270 degrees only for non-FFB budget wheels (Hori).
7. **FFB test:** on the Wheel page, a "Test force" button that plays, in order: a short centring spring, a constant force left then
   right for 1 s each at low gain, a damper, then releases; report which effects the SDL haptic handle supports
   (`SDL_GetHapticFeatures`). Warn when constant or spring is missing, when the device is XInput (no haptics), or when
   the base is a direct drive (default `[ffb] strength` to a low value and ramp up with a slider).
   Never call autocentre when the feature is absent (R5), and never mix `SDL_RumbleJoystick` with haptics on one device (S11).
8. **Save** with `device_guid` and the family template name so a different wheel does not inherit the mapping.

## Unknowns (to resolve with hardware, one capture per wheel)

- Windows/SDL3 axis indices and polarities for every wheel except what the repo's presets assume. In particular G27
  (Y = clutch? Z = throttle? Rz = brake?), G29, G920/G923, Thrustmaster family, Fanatec, MOZA.
- Button indices for G25/G27, DFGT, Thrustmaster, Fanatec rims, MOZA rims, Simagic, PXN, Turtle Beach, Cammus,
  Hori, G920/G923 Xbox. Only G923 PS (S8) and G PRO / RS50 (S8) are published.
- The G923 Xbox PIDs C26D versus C26E (which is which, which mode).
- Original T-GT and T818 product IDs; whether SDL lists them; Simagic EVO IDs in SDL; Turtle Beach and PXN V9/V10/V12
  and Hori Apex/Overdrive product IDs in SDL (not in the 71-entry list in S1).
- Whether SDL3 on Windows presents Xbox-edition wheels via XInput (no FFB) or DirectInput, and which hint fixes it.
- Which of the DFP's slots (`r2`/`l2` in `wheel.toml`, or R1/L1 bits in the report) are the gear paddles in the PR as built.
- G27 shifter extras (reverse gear button is outside the main bit field in SDL per a 2025 discourse thread referenced in
  the research search; its index is not known here).
- Whether Logitech's compatibility-mode PID is C294 or C298 after a power cycle without LGS (varies by wheel; High
  from general knowledge, not primary-sourced).
- Force strength scaling and recommended default `[ffb] strength` per wheel (depends on torque: G27 about 2 Nm,
  R5 about 5.5 Nm, DD bases much more). Nothing published to cite; the FFB test slider should handle it.
