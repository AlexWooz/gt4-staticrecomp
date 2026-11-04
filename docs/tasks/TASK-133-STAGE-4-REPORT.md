# TASK-133 Stage 4 Report: Controller Improvements Integration & Parity Verification

**Date:** 2026-10-27  
**Workspace:** `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
**Branch:** `task/133-rc7-ra-controls`  
**Base Commit:** `83bcd1e` (Stage 3 Accepted by Codex)  
**Review Checkpoint:** Addressing Codex Stage 4 Review (`TASK-133-STAGE-4-REVIEW.md`)  
**Status:** Stage 4 Parity Verification Complete — Paused for Codex Review  

---

## 1. Executive Summary & Review Scope Disposition

This report documents the completion of **Stage 4** of TASK-133 in strict compliance with `docs/tasks/TASK-133-STAGE-4-CONTROLLERS.md`, `docs/tasks/TASK-133-STAGE-4-REVIEW.md`, and `AGENTS.md`.

### Disposition of Stage 4 Scope & Review Findings
1. **Controller-Only Semantics from 1.2.5:** Integrated the reviewed controller improvements from stable 1.2.5 (`v1.2.5` `5d7a669`, reviewed controller merge `8b73fa0` from PR #100 via #106).
2. **Seven Canonical Response Curves:** Preserved all seven canonical curve identifiers (`linear`, `quadratic`, `cubic`, `s_curve`, `smooth_power`, `sublinear`, `logarithmic`), including accepted aliases, defaults, and old-configuration backwards compatibility.
3. **Role Architecture Clarification (per Review Item 1):**
   - **Native C++ Runtime (`native/controls.cpp`):** Implements `read_trigger` and `read_axis` evaluating shaping math (`shape()`, `is_default_shape()`, `deadzone_raw()`) to produce guest PS2 pressure bytes (`0..255`) and stick bytes (`0..255`, neutral 128).
   - **Rust Crates (`gt4controls`, `gt4l-tauri`, `gt4l-platform`):** Parse, validate, and serialize `Curve` selections, maintain TOML round-tripping, preserve backwards compatibility, and normalize SDL3 Gamepad trigger inputs (`clamped >> 7`), but do **not** evaluate curve shaping formulas.
   - **Web UI Model (`controls-model.js`):** Implements `triggerOutput` in JavaScript to mirror C++ shaping math in real-time on the launcher interface, returning normalized `0..1` floats whose quantized bytes `Math.round(val * 255)` match C++ `read_trigger` bit-for-bit.
4. **Demonstrable Parity Verification Across Full Space:**
   - Added `controls_fixture.exe --dump-vectors` dumping the exact C++ output bytes across 7 curves, 5 deadzones (0.0 boundary, 30/255 default, 0.25, 0.5, 0.75), and all 256 raw bytes (`0..255`).
   - Ran `verify_curve_parity.js`: verified **8,960 of 8,960 points** (100.0000%) exact bit-accurate matches between native C++ and web JS, with **0 discrepancies**.
   - Updated `nav-test.js` to assert shared fixture vectors for all 7 curves across 3 deadzones (0.0, 30/255, 0.5).
5. **Analog Stick Signed Endpoints & Neutral in Native Code:**
   - Implemented `check_stick_curves_and_signed_endpoints()` in `native/controls_fixture.cpp`.
   - Verified that all 7 curves maintain neutral (`raw = 0 -> 128`), deadzone boundary (`[-6553, 6553] -> 128`), signed extremes (`raw = 32767 -> 255` non-inverted / `0` inverted; `raw = -32768 -> 0` non-inverted / `255` inverted), and strict monotonicity across all 65,536 values.
6. **Correct Attribution of Baseline Failures (per Review Item 1 & 3):**
   - Corrected Section 6 to document the exact 13 failing test names in `test_gs_live.py` (1) and `test_gs_settings.py` (12), removing unsupported claims of missing slice executables or aspect-profile checks.
7. **Clean Room Isolation:**
   - Zero wheel/DFP/FFB bridge code (`wheel.cpp`, `wheel.rs`, `sdl_wheels.rs`, `wheel.toml` omitted).
   - Zero frame skipping from TASK-082 (unmerged PR #113 kept untouched).
   - Zero CoreGS work or GPL/PCSX2 source inspection.
   - Zero version bumps, installer compiling, portable zip generation, or release publications.

---

## 2. File-by-File Applicability & Provenance

Comparing the 24 files touched in reviewed controller commit `8b73fa0` against the current rc6 base identified 17 applicable files, with the remaining files excluded or adapted:

| File | Status in Stage 4 | Semantic Mapping & Changes |
| --- | --- | --- |
| `native/controls.hpp` | Ported | Expanded `enum class Curve` with `s_curve`, `smooth_power`, `sublinear`, `logarithmic`. |
| `native/controls.cpp` | Ported | Implemented math shaping for all 4 curves; added alias parsing (`scurve`, `smooth`, `root`, `log`) and updated error message. |
| `native/controls_fixture.cpp` | Ported / Extended | Added unit tests for 7 curves (neutral=0, full=255, monotonicity, deadzones 0, 30/255, 0.5); added stick signed endpoints (-32768, 32767) and neutral (128) across 7 curves; added `--dump-vectors` for reproducible parity verification. |
| `native/host_io.hpp` | Ported | Declared `read_sdl_gamepad(SDL_Gamepad *gamepad)` for shared polling and virtual-device testing. |
| `native/host_io.cpp` | Ported | Extracted `read_sdl_gamepad` with standardized trigger normalization (`gt4::pad::trigger_byte`). |
| `native/host_io_fixture.cpp` | Ported | Added `check_standardized_sdl_triggers()` testing virtual SDL gamepad (PS5 and Xbox IDs) through rest, 1/4, 1/2, 3/4, and full travel. |
| `launcher/crates/gt4controls/src/lib.rs` | Ported | Added `Curve` variants, string conversion, alias parsing, error validation, round-trip serialization, and old-config compatibility tests (stores/serializes selections; does not evaluate formulas). |
| `launcher/crates/gt4l-platform/src/pad.rs` | Ported | Replaced trigger division with bitshift `(clamped >> 7) as u8` to match native; added virtual trigger travel test. |
| `launcher/crates/gt4l-tauri/src/model.rs` | Ported | Updated `trigger_row` dropdown choices; added `curve_from_name` parser; updated `TriggerDto`; added unit test. |
| `launcher/crates/gt4l-tauri/ui/js/controls-model.js` | Ported | Updated `triggerOutput` to calculate `s_curve`, `smooth_power`, `sublinear`, `logarithmic` matching native formulas for web preview. |
| `launcher/crates/gt4l-tauri/ui/dev/mock-data.js` | Ported | Added the 4 new curves to analog dropdown mock definitions. |
| `launcher/crates/gt4l-tauri/ui/dev/nav-test.js` | Ported / Extended | Added shared fixture vector assertions across all 7 curves for 3 deadzones (0.0, 30/255, 0.5) testing rest, mid, and full travel. |
| `launcher/crates/gt4l-tauri/ui/locales/en-US.json` | Added | Added `choice.curve.s_curve`, `choice.curve.smooth_power`, `choice.curve.sublinear`, `choice.curve.logarithmic`. |
| `launcher/crates/gt4l-tauri/ui/locales/pt-BR.json` | Added | Added localized Brazilian Portuguese equivalents with matching keys. |
| `docs/CONTROLS.md` | Ported | Documented the 7 canonical curves and explained standardized SDL3 Gamepad trigger behavior. |
| `launcher/crates/gt4l-app/src/main.rs` | Ported | Updated legacy Slint curve mapping and `curve_index` for exhaustive match integrity. |
| `launcher/crates/gt4l-app/ui/controller.slint` | Ported | Added the 4 curves to Slint ComboBox model and path preview command. |
| `docs/inputs/slice1200-pcsx2.json` | Excluded | Release pin for old stable 1.2.5 branch; inapplicable to rc7 candidate. |
| `scripts/external_runtime_probe.py` | Excluded | Pinned probe profiles belong to historical slices; no pin updates solely to hide mismatches. |
| `tests/python/test_gs_*.py` | Excluded | Historical probe pin tests testing `slice1200-pcsx2.json`; rc6 baseline preserved. |

### Disposition of Slint `gt4l-app`
- **Current Build Usage:** In `launcher/Cargo.toml`, workspace members include only `gt4l-core`, `gt4l-disc`, `gt4saves`, `gt4controls`, `gt4l-platform`, and `gt4l-tauri`. The legacy Slint crate `gt4l-app` is **not** a member of the active workspace and is not built during release or test workflows.
- **Disposition:** The changes in `gt4l-app/src/main.rs` and `gt4l-app/ui/controller.slint` were applied to ensure code integrity and avoid match exhaustion errors if the crate is ever compiled standalone. It remains legacy/inactive.

---

## 3. Mathematical Formula Parity & Verification Evidence

### 3.1 Architectural Execution of Response Curves
- **C++ Native Runtime (`native/controls.cpp`):**
  - Evaluates curve shaping in `read_trigger` for digital button pressures (`0..255`) and `read_axis` for analog stick bytes (`0..255`, neutral 128).
  - Uses integer arithmetic for `linear` with `scale=1.0` to preserve slice 157 behavior bit-for-bit: `pressure = (raw - deadzone) * 255 / (255 - deadzone)`.
  - For non-linear curves, evaluates normalized fraction \(n = \frac{raw - deadzone}{255 - deadzone}\), shapes via `shape(n, curve)`, applies `scale`, and quantizes: `floor(shaped * 255)`.
- **Rust Crates (`gt4controls`, `gt4l-tauri`, `gt4l-platform`):**
  - **No formula evaluation.** Stores, parses, validates, and serializes the `Curve` selection and handles old-config aliases (`scurve`, `smooth`, `root`, `log`). Normalizes SDL3 physical trigger axes via `(clamped >> 7) as u8`.
- **Web UI Model (`controls-model.js`):**
  - Evaluates `triggerOutput(raw, deadzone, curve)` in JavaScript to drive the launcher's controller visualization SVG.
  - Returns `floor(shaped * 255) / 255`, yielding normalized `0..1` float values whose quantized byte representation `Math.round(val * 255)` is bit-for-bit identical to C++ `read_trigger`.

### 3.2 Mathematical Definitions
| Canonical Identifier | Formula \(f(n)\) for \(n \in [0, 1]\) | Native C++ (`native/controls.cpp`) | Web UI (`controls-model.js`) | Quantized Byte at Half Travel (\(dz=0, v=128\)) |
| --- | --- | --- | --- | --- |
| `linear` | \(n\) | `value` | `n` | `128` (0.5020) |
| `quadratic` | \(n^2\) | `value * value` | `n * n` | `64` (0.2510) |
| `cubic` | \(n^3\) | `value * value * value` | `n * n * n` | `32` (0.1255) |
| `s_curve` | \(n^2(3 - 2n)\) | `value * value * (3.0 - 2.0 * value)` | `n * n * (3 - 2 * n)` | `128` (0.5020) |
| `smooth_power` | \(n^{1.5} = n\sqrt{n}\) | `value * std::sqrt(value)` | `n * Math.sqrt(n)` | `90` (0.3529) |
| `sublinear` | \(n^{0.5} = \sqrt{n}\) | `std::sqrt(value)` | `Math.sqrt(n)` | `180` (0.7059) |
| `logarithmic` | \(\frac{\log_{10}(1 + 9n)}{\log_{10}(10)}\) | `std::log(1.0 + 9.0 * value) / std::log(10.0)` | `Math.log(1 + 9 * n) / Math.log(10)` | `189` (0.7412) |

### 3.3 End-to-End Native vs Web Parity Sweep
- **Tool:** `docs/tasks/evidence/stage4-ui/verify_curve_parity.js`
- **Input:** Ground truth vector dump exported directly by `controls_fixture.exe --dump-vectors` (`slice335`).
- **Scope:** 7 curves \(\times\) 5 deadzones (\(0.0\), \(30/255 \approx 0.1176\), \(0.25\), \(0.5\), \(0.75\)) \(\times\) 256 raw bytes (\(0..255\)).
- **Total Evaluated Points:** **8,960**
- **Bit-Accurate Byte Matches:** **8,960 (100.0000%)**
- **Discrepancies / Rounding Differences:** **0**
- **Durable Log:** `docs/tasks/evidence/stage4-logs/curve-parity.log`

### 3.4 Analog Stick Signed Endpoints & Neutral Verification
Implemented in `native/controls_fixture.cpp` (`check_stick_curves_and_signed_endpoints()`):
- **Neutral:** `read_axis(binding, 0)` evaluates to exactly `128` (PS2 stick center) across all 7 curves, both non-inverted and inverted.
- **Deadzone Boundary:** With `deadzone=0.2` (raw threshold 6553), all raw values in \([-6553, 6553]\) evaluate to exactly `128`. First step past deadzone steps away from 128.
- **Signed Endpoints:**
  - `raw = 32767` (full positive): non-inverted evaluates to `255`; inverted evaluates to `0`.
  - `raw = -32768` (full negative): non-inverted evaluates to `0`; inverted evaluates to `255`.
- **Monotonicity:** All 65,536 raw values from \(-32768\) to \(+32767\) were verified strictly monotonic across all 7 curves:
  - Non-inverted: `current >= prior` for all steps.
  - Inverted: `current <= prior` for all steps.

---

## 4. Fresh Native Build Provenance & Binary Hashes

Both game targets were configured and built from source in fresh slice directories on drive O: using Clang-cl (LLVM 22.1.3), Ninja, and MSVC 17.14 build tools.

### 4.1 Configure Recipes
#### v200 Candidate (`O:\gt4recomp-scratch\slice335-rc7-controls-build`)
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice335-rc7-controls-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DGT4_TARGET=v200 -DGT4_EE_PROFILE=aspect -DGT4_RELEASE_DISTRIBUTION=ON ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
  -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
  -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-frame-cost-runtime ^
  -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port ^
  -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen ^
  -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 ^
  -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin ^
  -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus ^
  -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice262-license-guard-runtime ^
  -DGT4_MEMCARD_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice200-memcard-runtime ^
  -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
cmake --build O:\gt4recomp-scratch\slice335-rc7-controls-build\build
```

#### Spec II Candidate (`O:\gt4recomp-scratch\slice336-rc7-controls-spec2`)
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" ^
  -DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2 -DGT4_RELEASE_DISTRIBUTION=ON ^
  -DGT4_ASSETS_ROOT=C:/Antigravity/gt4-staticrecomp ^
  -DGT4_PYTHON=C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe ^
  -DGT4_PERF_RUNTIME_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice276-spec2-iop-runtime ^
  -DGT4_PORT_DIR=C:/Antigravity/gt4-staticrecomp/private/slice200-port ^
  -DGT4_VU1_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen ^
  -DGT4_VU0_GENERATED_DIR=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3 ^
  -DGT4_VU0_IMAGE_FILE=C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin ^
  -DGT4_VU_CORPUS_DIR=C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus ^
  -DGT4_LICENSE_GUARD_SOURCE=C:/Antigravity/gt4-staticrecomp/private/slice277-license-guard-runtime ^
  -DGT4_SHADER_DIR=C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders
cmake --build O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build
```

### 4.2 Exact Artifact Hashes & Sizes
All binary artifacts were verified using SHA256:

| Slice Target | Binary File | Size (Bytes) | SHA256 Hash | Notes |
| --- | --- | --- | --- | --- |
| `slice335` (v200) | `gt4vulkan.exe` | 97,702,912 | `17406BA4BFCD3DED71E68427C45B3528B43C1CACCB3C4872FDA3DED5634AE917` | Verified unchanged |
| `slice335` (v200) | `controls_fixture.exe` | 179,200 | `BAE5BBF558C55D255654C79838CF8651BDAEEDA27A085E98E5DEA50767D0B325` | Updated with stick tests & vector dump |
| `slice335` (v200) | `host_io_fixture.exe` | 125,440 | `4F45914B34532011A03F0F3F02E3E93FC51D15156AD433D0018845DB962FFE99` | Unchanged |
| `slice335` (v200) | `achievements_fixture.exe` | 402,432 | `0C64875BDBD869F41C8ED224F66E5A37FEA2A3DA8A97F6CC9E31BC18D495FBFD` | Unchanged |
| `slice335` (v200) | `achievements_spec2_fixture.exe` | 104,448 | `2CD3C68927B26F7A22001E0BBD1BC257D51B8EC81782504F3400A62FC1EDDE16` | Unchanged |
| `slice335` (v200) | `achievements_http_fixture.exe` | 40,960 | `8D248B8B915E781FEEF88753404F8D8F89916C684C9FE39B132BF81766282ADF` | Unchanged |
| `slice336` (Spec II) | `gt4vulkan-spec2.exe` | 104,719,872 | `67951470D1FAF767C8D619BAC5483BBFD30F9B164E6AAEB04E7AD20858083F1A` | Verified unchanged |
| `slice336` (Spec II) | `controls_fixture.exe` | 179,200 | `B58929969C42B9F7C406CA4DA2CCD6274121F07D573D6AAD8B56E56F93D5A07C` | Updated with stick tests & vector dump |
| `slice336` (Spec II) | `host_io_fixture.exe` | 125,440 | `0CC9D314ABCA1C1B284E4A1D8EF3C2D0C406729781015A6B851B64D7A42B0820` | Unchanged |
| `slice336` (Spec II) | `achievements_fixture.exe` | 402,432 | `D48582C00A791EEE5152E1EBEB18B34BD44F98FB3F48449616B475F14FCEDF82` | Unchanged |
| `slice336` (Spec II) | `achievements_spec2_fixture.exe` | 104,448 | `FE9F83F705FB83E1B91A04201AF3BEE3235D1D12DFDE2499738021147AAC4003` | Unchanged |
| `slice336` (Spec II) | `achievements_http_fixture.exe` | 40,960 | `869CFF6E058912D21BE497BEF1B697D6600A91DF83301747B65E40560402FF74` | Unchanged |

*Note: Slices `slice333-rc7-ra-build` and `slice334-rc7-ra-spec2` remain intact.*

---

## 5. Comprehensive Test Execution Evidence

All test suites were executed serially.

### 5.1 Rust Workspace Test Suite
- **Command:** `cargo test --workspace --manifest-path launcher/Cargo.toml` (with `CARGO_TARGET_DIR=O:\gt4recomp-scratch\task133-cargo-target` and `GT4_SDL3_ROOT=C:\Antigravity\gt4-staticrecomp\private\tooling\sdl3-3.4.18`)
- **Durable Log:** `docs/tasks/evidence/stage4-logs/cargo-test-workspace.log`
- **Result:** **34 test blocks, 578 passed, 0 failed, 5 ignored** (reconciled across all crates and integration suites).
- **Preserved Behavior:** All channel-coordinated TCP race tests (`retroachievements_network_race_tests_with_real_tcp_server`) passed without race or deadlock.

### 5.2 UI Focus and Navigation Checks
- **Command:** `node docs/tasks/evidence/stage4-ui/run_ui_checks.js` (using Playwright Edge engine)
- **Durable Log:** `docs/tasks/evidence/stage4-logs/ui-checks.log`
- **Results:**
  - `dev/focus-check.html?lang=en-US`: **215 of 215 passed**, 0 JS errors.
  - `dev/focus-check.html?lang=pt-BR`: **222 of 222 passed**, 0 JS errors.
  - `dev/nav-test.html`: **209 of 209 passed** (+55 checks: verified shared fixture vectors across all 7 curves for 3 representative deadzones 0.0, 30/255, and 0.5 at rest, mid, and full travel), 0 failures, 0 JS errors.

### 5.3 Native vs Web Curve Parity Sweep
- **Command:** `node docs/tasks/evidence/stage4-ui/verify_curve_parity.js`
- **Durable Log:** `docs/tasks/evidence/stage4-logs/curve-parity.log`
- **Result:** **8,960 / 8,960 exact bit-accurate matches (100.0000%)**, 0 discrepancies.

### 5.4 Native CTest Suites
#### Targeted Controls, Audio Input & Achievements Tests
- `slice335` (v200):
  - `Test #10: native_audio_input`: **Passed** (3.46s) — verified virtual PS5 and Xbox triggers through rest, 1/4, 1/2, 3/4, and full travel.
  - `Test #11: native_controls`: **Passed** (0.12s) — verified default table, legacy slice 157 parity, all 7 trigger curves across deadzones 0.0, 30/255, 0.5, and stick signed endpoints/neutral/monotonicity.
  - `Test #14: native_achievements`: **Passed** (0.04s)
  - `Test #15: native_achievements_spec2`: **Passed** (0.03s)
  - `Test #16: native_achievements_http`: **Passed** (0.06s)
- `slice336` (Spec II):
  - `Test #10: native_audio_input`: **Passed** (3.41s)
  - `Test #11: native_controls`: **Passed** (0.08s)
  - `Test #14: native_achievements`: **Passed** (0.04s)
  - `Test #15: native_achievements_spec2`: **Passed** (0.03s)
  - `Test #16: native_achievements_http`: **Passed** (0.06s)

#### Full CTest Suites (50 Tests Each)
- **v200 Candidate (`slice335`):**
  - **Durable Log:** `docs/tasks/evidence/stage4-logs/ctest-full-v200.log`
  - **Result:** **49/50 passed (98%)**, 1 failed.
  - Failing test: `Test #36: python_native_contracts` (Ran 710 tests, 13 failures, 42 skipped).
- **Spec II Candidate (`slice336`):**
  - **Durable Log:** `docs/tasks/evidence/stage4-logs/ctest-full-spec2.log`
  - **Result:** **49/50 passed (98%)**, 1 failed.
  - Failing test: `Test #36: python_native_contracts` (Ran 710 tests, 13 failures, 55 skipped).

### 5.5 Targeted Python Tests
- `tests/python/test_controls_probe.py`: **6 passed in 0.026s (OK)**.
- `tests/python/test_pacing_launcher.py`: **14 passed, 1 skipped in 0.029s (OK)**.

---

## 6. Accounting of Known Baseline Failures

The single failing CTest test (#36 `python_native_contracts`) in both `slice335` and `slice336` was audited against the Stage 3 baseline. Exactly 13 tests failed out of 710, identically in both targets, consisting entirely of `test_gs_live` and `test_gs_settings`:

### 1 Failure in `test_gs_live.py`:
1. `test_gs_live.ConfigTests.test_defaults_are_vulkan_3x_high_blending_automatic_deinterlace_16_9_vsync`

### 12 Failures in `test_gs_settings.py`:
2. `DefaultTests.test_cache_and_headless_sections_are_unchanged`
3. `DefaultTests.test_default_file_bytes_match_the_main_writer`
4. `DefaultTests.test_default_ini_is_main_default`
5. `DefaultTests.test_probe_default_is_unchanged`
6. `DefaultTests.test_run_text_with_data_root_and_eager_switch`
7. `IniTests.test_user_settings_replace_defaults_and_add_when_set_rows`
8. `OfferedTests.test_launcher_offers_only_settable_options`
9. `OsdRowTests.test_overlay_rows_are_advanced_toggles_off_by_default`
10. `PageLayoutTests.test_advanced_page_holds_the_rest_of_the_user_options`
11. `ProhibitDynamicCodeTests.test_row_is_an_advanced_always_written_switch_defaulting_to_false`
12. `ResetToDefaultsTests.test_reset_gives_the_exact_default_ini`
13. `ResetToDefaultsTests.test_when_set_row_equal_to_its_default_is_not_written`

### Cause & Scope:
These 13 failures represent the documented divergence between current rc6 INI settings/options definitions and the older static expectation fixtures in `test_gs_live.py` and `test_gs_settings.py`. They are unrelated to input controls, RetroAchievements, or compiler runtime code. No test assertions were weakened, and no pins were artificially updated to hide mismatches.

---

## 7. Limitations & Strict Boundaries

1. **Synthetic Device Testing Only:** All controller and trigger evaluations were conducted using mathematical models, shared fixture vectors, and synthetic SDL virtual joysticks (`SDL_AttachVirtualJoystick`). No physical DualSense, DualShock, or Xbox controllers were connected.
2. **No Subjective Gameplay Claims:** These tests prove mathematical parity, trigger threshold consistency, serialization round-tripping, and input mapping correctness. They do **not** establish subjective steering feel, car responsiveness, or in-game handling fidelity.
3. **Strict Stage Boundaries Maintained:**
   - No version numbers were bumped.
   - NSIS installer was not compiled.
   - Portable zip package was not assembled.
   - No pull requests, draft releases, or public releases were published.
   - Work is paused for Codex Orchestrator review.
