# TASK-133: Baseline and Provenance Report (Stage 1 Completion)

Date: 2026-10-26  
Branch: `task/133-rc7-ra-controls`  
Worktree: `C:/Antigravity/gt4-wt-133-rc7-ra-controls`  
HEAD Commit: `e7546fc` (`docs(review): flag native rc6 artifact mismatch before task133 integration`)  
Program Baseline: `v1.3.0-rc6` (`58322229c0bd63a0652b0550c5ad2154bd907039`)  

---

## 1. Source, Pin, Toolchain, and Build References

### Provenance and Artifact Hashes
The program baseline of this task is the published release `v1.3.0-rc6` (`5832222`). All program files in the worktree equal the published tag byte-for-byte.

The table below reconciles all tested and published binaries with their claimed pins:

| Artifact Path | Purpose | Measured SHA256 | Pin Reference | Status |
| :--- | :--- | :--- | :--- | :--- |
| `C:/Antigravity/gt4-staticrecomp/private/slice306-dist-130-rc6b/gt4.exe` | Published rc6 v2.00 distribution binary | `6682777e1a6c141d3f32aa54265d609a4926a55f0c19c45d84c51a2e39970945` | `docs/inputs/slice303-pcsx2.json` | **VERIFIED** (Exact match) |
| `C:/Antigravity/gt4-staticrecomp/private/slice306-dist-130-rc6b/gt4-spec2.exe` | Published rc6 Spec II distribution binary | `e841615236ec9107e0d0580d877ebc5ffa3afae9644aed1196f12a58f89df173` | `docs/inputs/slice304-spec2.json` | **VERIFIED** (Exact match) |
| `O:/gt4recomp-scratch/slice301-rc6-spec2/build/gt4vulkan-spec2.exe` | Spec II scratch build output | `e841615236ec9107e0d0580d877ebc5ffa3afae9644aed1196f12a58f89df173` | `docs/inputs/slice304-spec2.json` | **VERIFIED** (Exact match) |
| `O:/gt4recomp-scratch/slice300-rc6-v200/build/gt4vulkan.exe` | Mutable v2.00 scratch build output tested in baseline | `3e15166ade27309b90f62be91477dcae532b2ffbde2eceb1b0591055f29e1503` | `docs/inputs/slice303-pcsx2.json` (`6682777e...`) | **MISMATCH** (Documented below) |

### Native Build Provenance and Worktree Distinction
- **Fixture Build Directory:** `O:/gt4recomp-scratch/slice300-rc6-v200/build` was configured from `CMAKE_HOME_DIRECTORY = C:/Antigravity/gt4-wt-116-117-perf` on 2026-10-25. That source worktree was clean on branch `task/116-117-flare-perf` (`5f0d3d6`), whose tracked files outside `docs/` and `TASKS.md` are identical to published rc6 (`5832222`).
- **Build Drift Root Cause:** `.ninja_log` reveals that `O:/gt4recomp-scratch/slice300-rc6-v200/build` was an active, mutable development directory where `gs_stream_client.cpp` and `gt4vulkan.exe` were recompiled several times on the evening of 2026-10-25 (timestamps 81466178... through 81467771...). This resulted in a subsequent binary (`3e15166a...`) differing from the final immutable distribution artifact delivered to `slice306-dist-130-rc6b/gt4.exe` (`6682777e...`).
- **CTest Environment Path:** Because CMake configured the build in `C:/Antigravity/gt4-wt-116-117-perf`, CTest's Test #33 (`python_native_contracts`) executes against test files located in `C:/Antigravity/gt4-wt-116-117-perf/tests/python`.
- **Published Runtime Reference:** `C:/Antigravity/gt4-staticrecomp/private/slice306-dist-130-rc6b/gt4.exe` is the immutable, verified rc6 game binary matching pin `slice303`. The scratch binary in `slice300` is documented as a rebuilt scratch artifact, not the published release binary.

### Toolchains (Actual Measured Versions)
- **C/C++ Compiler:** Clang-cl version `22.1.3` (`C:/Program Files/LLVM/bin/clang-cl.exe`, LLVM git `e9846648fd6183ee6d8cbdb4502213fcf902a211`, Target: `x86_64-pc-windows-msvc`)
- **Build System:** Ninja `1.13.2`
- **CMake:** `4.3.1` (both active binary and recorded in `CMakeCache.txt`)
- **Rust:** `cargo 1.99.0 (5f94df478 2026-08-27)`
- **Python:** `Python 3.14.2` (`C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe`)
- **SDL3:** Version `3.4.18` located under `C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18`

---

## 2. Test Execution Commands, Evidence, and Classification

### A. Rust Launcher Workspace Suite
- **Command:**
  ```powershell
  $env:GT4_SDL3_ROOT = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18"
  cargo test --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml --workspace --locked --offline
  ```
- **Overall Result:** **559 passed, 0 failed, 5 ignored**.

#### Complete Test Target Breakdown (All 28 Test Runs)

| Crate | Test Target / Binary | Passed | Failed | Ignored | Total |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **gt4controls** | `unittests src\lib.rs` | 16 | 0 | 0 | 16 |
| | `tests\docs_match.rs` | 4 | 0 | 0 | 4 |
| | *Doc-tests gt4controls* | 0 | 0 | 0 | 0 |
| **gt4l-core** | `unittests src\lib.rs` | 213 | 0 | 0 | 213 |
| | `tests\config_migration.rs` | 16 | 0 | 0 | 16 |
| | `tests\enabled_values.rs` | 3 | 0 | 0 | 3 |
| | `tests\gs_golden.rs` | 5 | 0 | 0 | 5 |
| | `tests\gs_release.rs` | 11 | 0 | 0 | 11 |
| | `tests\launch_contract.rs` | 21 | 0 | 0 | 21 |
| | `tests\multi_game.rs` | 5 | 0 | 0 | 5 |
| | `tests\release_lists.rs` | 5 | 0 | 0 | 5 |
| | `tests\relocate.rs` | 20 | 0 | 0 | 20 |
| | `tests\settings_config.rs` | 9 | 0 | 0 | 9 |
| | `tests\textures.rs` | 7 | 0 | 0 | 7 |
| | *Doc-tests gt4l_core* | 0 | 0 | 0 | 0 |
| **gt4l-disc** | `unittests src\lib.rs` | 6 | 0 | 0 | 6 |
| | `tests\elf_rebuild.rs` | 3 | 0 | 0 | 3 |
| | `tests\real_disc.rs` | 1 | 0 | 2 | 3 |
| | `tests\setup_fake_iso.rs` | 12 | 0 | 0 | 12 |
| | `tests\spec2_identify.rs` | 8 | 0 | 0 | 8 |
| | *Doc-tests gt4l_disc* | 0 | 0 | 0 | 0 |
| **gt4l-platform** | `unittests src\lib.rs` | 47 | 0 | 2 | 49 |
| | `tests\job_breakaway.rs` | 4 | 0 | 0 | 4 |
| | *Doc-tests gt4l_platform* | 0 | 0 | 0 | 0 |
| **gt4l-tauri** | `unittests src\main.rs` (`GT4Recomp`) | 88 | 0 | 1 | 89 |
| **gt4saves** | `unittests src\lib.rs` | 6 | 0 | 0 | 6 |
| | `tests\card_owner.rs` | 6 | 0 | 0 | 6 |
| | `tests\hostile_cards.rs` | 10 | 0 | 0 | 10 |
| | `tests\library.rs` | 12 | 0 | 0 | 12 |
| | `tests\migration.rs` | 7 | 0 | 0 | 7 |
| | `tests\per_game.rs` | 5 | 0 | 0 | 5 |
| | `tests\save_management.rs` | 9 | 0 | 0 | 9 |
| | *Doc-tests gt4saves* | 0 | 0 | 0 | 0 |
| **TOTALS** | **All 28 Test Blocks** | **559** | **0** | **5** | **564** |

- **Unit Tests Subtotal:** 376 passed, 0 failed, 3 ignored.
- **Integration Tests Subtotal:** 183 passed, 0 failed, 2 ignored.
- **Log Reference:**
  - Path: `C:/Antigravity/gt4-staticrecomp/private/task133-orchestrator-review-20261026/cargo-baseline.log`
  - SHA256: `0c3b22ededd926b4e2c463212a1aedff43d41eae594952e8115bc47a0aaaee61`

---

### B. CTest Native Suite
- **Command:**
  ```powershell
  ctest --test-dir O:/gt4recomp-scratch/slice300-rc6-v200/build --output-on-failure
  ```
- **Result:** **46 of 47 passed (98%), 1 failed** (Test #33 `python_native_contracts`).
  - Total real execution time: 226.64 s.
  - All 46 native and recompilation C++ test targets passed:
    - Native Vulkan (#1-#5, #7-#9)
    - FPU chop (#6)
    - Audio, controls, and pad mapping (#10-#12)
    - Pacing, quickfix, and display (#13-#18)
    - Shape observation & performance accounting (#19-#21)
    - MTVU runtime (#22-#27)
    - GS Stream & IPC (#28-#30)
    - Shape capture (#31-#32)
    - VU AOT & Differential (#34-#40, including 98s corpus test)
    - System & target hooks (#41-#47: memcard, license guard, standalone, game target, spec2 iop, spec2 hooks, flare limits)
  - Test #33 `python_native_contracts` failed because it executes the Python test suite, whose pre-existing failures are detailed below.
- **Log Reference:**
  - Path: `private/task133-baseline-logs/ctest-baseline.log`
  - SHA256: `095e3a03e723ef73db2fe11c8496a946af35fde206e9cad241fed5910488fe7f`

---

### C. Full Python Test Suite
- **Command:**
  ```powershell
  $env:GT4_NATIVE_BUILD = "O:/gt4recomp-scratch/slice300-rc6-v200/build"
  C:\Antigravity\gt4-staticrecomp\private\tooling-venv\Scripts\python.exe -m unittest discover -s C:\Antigravity\gt4-wt-133-rc7-ra-controls\tests\python
  ```
- **Result:** Ran 710 tests in 48.981 s. **655 passed, 13 failed, 42 skipped**.
- **Log Reference:**
  - Path: `private/task133-baseline-logs/python-baseline.log`
  - SHA256: `ab638172071273f1be430b8c423103306493dd3c540dd43b447dda4e29393d59`

#### Detailed Classification of the 13 Failures
These failures are **pre-existing technical debt** on the rc6 baseline caused by the TASK-118 graphics settings simplification, independently verified by Codex:
1. `test_gs_live.ConfigTests.test_defaults_are_vulkan_3x_high_blending_automatic_deinterlace_16_9_vsync`:
   - Fails because `LoadTextureReplacementsAsync = true` was added to `gt4-gs.ini` by default in rc6 (TASK-118), whereas the legacy assertion expects it omitted.
2. `tests/python/test_gs_settings.py` (12 failures):
   - `DefaultTests.test_cache_and_headless_sections_are_unchanged`
   - `DefaultTests.test_default_file_bytes_match_the_main_writer`
   - `DefaultTests.test_default_ini_is_main_default`
   - `DefaultTests.test_probe_default_is_unchanged`
   - `DefaultTests.test_run_text_with_data_root_and_eager_switch`
   - `IniTests.test_user_settings_replace_defaults_and_add_when_set_rows`
   - `OfferedTests.test_launcher_offers_only_settable_options`
   - `OsdRowTests.test_overlay_rows_are_advanced_toggles_off_by_default`
   - `PageLayoutTests.test_advanced_page_holds_the_rest_of_the_user_options` (expects 40 rows in `advanced`; simplified table only has 5 rows)
   - `ProhibitDynamicCodeTests.test_row_is_an_advanced_always_written_switch_defaulting_to_false` (row moved to `hidden` in rc6)
   - `ResetToDefaultsTests.test_reset_gives_the_exact_default_ini`
   - `ResetToDefaultsTests.test_when_set_row_equal_to_its_default_is_not_written`
- **Orchestrator Independent Verification:** Rerun by Codex at review time without code changes (`python-settings-baseline.log` and `python-live-baseline.log`), confirming identical failure counts.
- **Other Modules:** 0 failures across memory card, display aspect, VU0/VU1 image, license guard, distribution, and Spec II tests.

---

## 3. Corrected Proposed Integration Contract

### A. Persisted Controller Curves
1. **Canonical Curve Identifiers:**
   The exact persisted string identifiers are:
   - `linear`
   - `quadratic`
   - `cubic`
   - `s_curve`
   - `smooth_power`
   - `sublinear`
   - `logarithmic`
   Ad-hoc identifiers (such as `exponential` or `aggressive`) are strictly prohibited. Existing saved `controls.toml` configurations are preserved with full backward compatibility.
2. **Scope of Application:**
   All 7 curves apply symmetrically to both game targets (`gt4.exe` and `gt4-spec2.exe`).
3. **Trigger Standardization:**
   SDL Gamepad analog triggers use standardized `0..32767` integer scaling for all mapped gamepads without device-specific name heuristics.
4. **Boundary:**
   Strictly controller-only. No wheel, DFP, or force-feedback code enters this integration.

### B. Target Identification and Isolation (GT4 v2.00 vs Spec II)
1. **Target Architecture Contract:**
   Target identity is governed by the existing native target architecture:
   - `gt4::target::active().id`
   - Evaluated as `gt4::target::Id::v200` versus `gt4::target::Id::spec2`.
   - Selected at compile time via CMake `GT4_TARGET` (default `v200`, or `spec2`).
2. **Native Guard:**
   Inside the native achievements runtime (`native/achievements.cpp`), initialization explicitly checks:
   ```cpp
   if (::gt4::target::active().id != ::gt4::target::Id::v200) {
     // Achievements strictly disabled for Spec II: no memory evaluation,
     // no network connections, no overlay rendering.
     return;
   }
   ```
   Environment variables and executable filenames are not treated as authoritative native security boundaries.
3. **Spec II Negative Testing:**
   Tests verifying that Spec II disables achievements must evaluate the inactive path without defining `GT4_TARGET_FIXTURE_V200` (which would override the target profile to v2.00).
4. **Launcher Boundary:**
   In `launcher/crates/gt4l-tauri/src/commands.rs` (`prepare_play`), when `games::SPEC2` is selected:
   - All `GT4_RA_*` environment variables are scrubbed and forcibly set to disabled.
   - The launcher UI achievements tab/page displays a clear notice that RetroAchievements is unsupported for Spec II.

### C. RetroAchievements Lifecycle and Hygiene
1. **Integration Status:**
   RetroAchievements functionality is an **expected outcome of Stage 2 integration**, not a feature present in the unintegrated rc6 baseline.
2. **Configuration and Storage:**
   - Account credentials (`retroachievements.toml`) are resolved through `UserDirs` and `BuildKind`, maintaining strict isolation between Installed (`%APPDATA%/GT4Recomp`) and Portable (`user/`) installations.
   - Cache data is placed in the designated user/data cache directory.
3. **Cancellation and Concurrency Guards:**
   - Asynchronous HTTP requests via WinHTTP are gracefully cancelled during runtime shutdown.
   - Account revision numbers guard against race conditions between background logins/refreshes and logouts.
4. **Secrets Hygiene:**
   Passwords, web tokens, and raw credentials must never be written to logs, diagnostic export archives, console output, or child-process command-line arguments.

### D. Localization Parity
- All new player-facing UI text, settings rows, and native overlay messages are translated into both `en-US` and `pt-BR`.
- Key and `{placeholder}` parity between `en-US.json` and `pt-BR.json` is strictly maintained.

---

## 4. Stage 1 Final Status

- Baseline checks completed and reconciled across Rust (559/559 across 28 test blocks), Native CTest (46/47 passing, 1 known python contract failure), and Python (655/710 passing, 13 recorded TASK-118 drift failures).
- Build and artifact provenance resolved: published rc6 distribution binaries (`slice306-dist-130-rc6b`) verified against pins `slice303` and `slice304`; build-directory drift in mutable `slice300` documented with measured hashes and `.ninja_log` evidence.
- Measured toolchain versions recorded accurately.
- Proposed architecture and target contract fully specified.
- No runtime, launcher, or pin files modified.
- **Stage 1 is complete. Standing by for orchestrator review before Stage 2 release.**
