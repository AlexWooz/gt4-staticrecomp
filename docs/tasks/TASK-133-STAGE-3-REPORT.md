# TASK-133 Stage 3 Report: Modern Launcher RetroAchievements Integration & Language Bridge

**Date:** 2026-10-27  
**Workspace:** `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
**Branch:** `task/133-rc7-ra-controls`  
**Review Base:** `9ef35d1`  
**Reviewer:** Codex Orchestrator  
**Status:** Stage 3 Review 3 Adjustments Complete — Paused for Codex Review  

---

## 1. Executive Summary & Review Scope Disposition

This report documents the resolution of all findings from `docs/tasks/TASK-133-STAGE-3-REVIEW-3.md` following the completion and verification of **Stage 3** of TASK-133. All work remains strictly within the boundaries of `docs/tasks/TASK-133-STAGE-3-LAUNCHER-RA.md` and the preceding bounded cleanup protocol `docs/tasks/TASK-133-BUILD-CLEANUP.md`.

### Disposition of Review 3 Required Adjustments

1. **Deterministic Channel/Barrier Coordinated Race Tests:** Replaced timing-only sleeps in `launcher/crates/gt4l-tauri/src/flow_audit.rs` (`retroachievements_network_race_tests_with_real_tcp_server`) with a `SyncGate` channel coordination mechanism (`Sender<()>` and `Receiver<()>`). The fake server explicitly signals request arrival and pauses response dispatch until the test thread completes the concurrent mutation (logout, newer login, or target switch). Bounded timeouts (3s) ensure the fake server cannot hang tests under any failure condition.
2. **Exercising Real Network Paths for Refresh Scenarios:** Both refresh concurrency scenarios (refresh vs. logout and refresh vs. target switch) now exercise the real `commands::ra_status_at` network path over TCP against the fake HTTP server, eliminating previous reliance on the closure-based `ra_status_with` mock. Both the returned `RaStatusView` and persisted configuration (`retroachievements.toml`) are asserted.
3. **Network Disconnect and Error Response Reporting:** Accurately characterized `ra_status_at` error behavior: because `login_with_token` and `fetch_game_overview` calls are wrapped in `.ok()`, network drops (e.g., `/abort`) do not return an `Err` to the caller; instead, `ra_status_at` returns `Ok(RaStatusView)` gracefully falling back to local cached state (`game: None`). Verified that persisted disk credentials and score remain intact.
4. **Distinction of Service Error vs. HTTP 401:** Implemented distinct test cases for service-level errors (HTTP 200 with JSON `{"Success": false, "Error": "Invalid user or password."}`) and genuine HTTP 401 Unauthorized status (`HTTP/1.1 401 Unauthorized`). Verified that both return `Err` without corrupting persisted credentials on disk.
5. **Reconciliation of Cargo Workspace Totals:** Reconciled all 34 `test result` blocks in `docs/tasks/evidence/stage3-logs/cargo-test-workspace.log`: exactly 574 tests passed, 0 failed, 5 ignored across the workspace crates.
6. **Preservation of Uncertainty on CTest Baseline:** Preserved explicit uncertainty regarding transient timeouts (`native_mtvu_ring`, `python_native_contracts`) and audio input failure in full CTest runs, avoiding unsubstantiated causality claims.
7. **Strict Stage Boundaries Maintained:**
   - No controller curves, stick deadzones, or wheel bridge modifications.
   - No packaging, NSIS installer compilation, or portable zip creation.
   - No release drafting, tags, publication, or PR creation.
   - Paused for Codex Orchestrator review.

---

## 2. Bounded Disk Cleanup Reconciliation

The bounded cleanup protocol defined in `docs/tasks/TASK-133-BUILD-CLEANUP.md` was executed and verified against its receipt:
- **Receipt Path:** `O:\gt4recomp-scratch\task133-cargo-target-receipt.json`
- **Source Relocated:** `launcher\target` -> `O:\gt4recomp-scratch\task133-cargo-target`
- **Verified Files:** 7,400 files
- **Total Relocated Bytes:** 3,833,790,678 bytes (~3.57 GiB / 3.83 GB)
- **Hash Integrity:** 100% SHA256 match for every individual file prior to deletion of `launcher\target`.
- **Drive C: Free Space Before:** 12,360,527,872 bytes (~11.51 GiB)
- **Drive C: Free Space After:** 20,778,311,680 bytes (~19.35 GiB)
- **Net Drive C: Free Space Delta:** +8,417,783,808 bytes (~7.84 GiB)
- **Attribution & Measurement Boundary:** The relocation of `launcher\target` accounts directly for 3,833,790,678 bytes. The remaining ~4.58 GB increase during that time interval occurred concurrently on the host system; because it was not tracked by the relocation receipt, its specific cause is unmeasured and declared unknown.
- **Subsequent Cargo Invocations:** All build and test commands run with `CARGO_TARGET_DIR=O:\gt4recomp-scratch\task133-cargo-target`.

---

## 3. Native Configure Recipes, Provenance & Binary Hashes

### 3.1 Reconstructed Configure Recipes from CMakeCache.txt

The fresh build directories on drive O: (`slice333-rc7-ra-build` and `slice334-rc7-ra-spec2`) were inspected directly via their respective `CMakeCache.txt` files. The original interactive command lines were not preserved in a dedicated shell script; the exact variables recorded in cache are as follows:

#### v200 Candidate (`O:\gt4recomp-scratch\slice333-rc7-ra-build\build\CMakeCache.txt`)
- `CMAKE_BUILD_TYPE`: `Release`
- `CMAKE_CXX_COMPILER`: `C:/Program Files/LLVM/bin/clang-cl.exe`
- `GT4_TARGET`: `v200`
- `GT4_EE_PROFILE`: `aspect`
- `GT4_ASSETS_ROOT`: `C:/Antigravity/gt4-staticrecomp`
- `GT4_BRANDING_DIR`: `C:/Antigravity/gt4-wt-133-rc7-ra-controls/launcher/assets/brand`
- `GT4_LICENSE_GUARD_SOURCE`: `C:/Antigravity/gt4-staticrecomp/private/slice262-license-guard-runtime`
- `GT4_MEMCARD_RUNTIME_SOURCE`: `C:/Antigravity/gt4-staticrecomp/private/slice200-memcard-runtime`
- `GT4_PERF_RUNTIME_SOURCE`: `C:/Antigravity/gt4-staticrecomp/private/slice262-frame-cost-runtime`
- `GT4_PORT_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice200-port`
- `GT4_PYTHON`: `C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe`
- `GT4_RELEASE_DISTRIBUTION`: `ON`
- `GT4_SDL3_ROOT`: `C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18`
- `GT4_SHADER_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders`
- `GT4_VU0_GENERATED_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3`
- `GT4_VU0_IMAGE_FILE`: `C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin`
- `GT4_VU1_GENERATED_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen`
- `GT4_VU_CORPUS_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus`

Reconstructed command:
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice333-rc7-ra-build\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
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
cmake --build O:\gt4recomp-scratch\slice333-rc7-ra-build\build
```

#### Spec II Candidate (`O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build\CMakeCache.txt`)
- `CMAKE_BUILD_TYPE`: `Release`
- `CMAKE_CXX_COMPILER`: `C:/Program Files/LLVM/bin/clang-cl.exe`
- `GT4_TARGET`: `spec2`
- `GT4_EE_PROFILE`: `spec2`
- `GT4_ASSETS_ROOT`: `C:/Antigravity/gt4-staticrecomp`
- `GT4_BRANDING_DIR`: `C:/Antigravity/gt4-wt-133-rc7-ra-controls/launcher/assets/brand`
- `GT4_LICENSE_GUARD_SOURCE`: `C:/Antigravity/gt4-staticrecomp/private/slice277-license-guard-runtime`
- `GT4_PERF_RUNTIME_SOURCE`: `C:/Antigravity/gt4-staticrecomp/private/slice276-spec2-iop-runtime`
- `GT4_PORT_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice200-port`
- `GT4_PYTHON`: `C:/Antigravity/gt4-staticrecomp/private/tooling-venv/Scripts/python.exe`
- `GT4_RELEASE_DISTRIBUTION`: `ON`
- `GT4_SDL3_ROOT`: `C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18`
- `GT4_SHADER_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice153-pacing-shaders`
- `GT4_VU0_GENERATED_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-gen-r3`
- `GT4_VU0_IMAGE_FILE`: `C:/Antigravity/gt4-staticrecomp/private/slice160-vu0-image/vu0_image.bin`
- `GT4_VU1_GENERATED_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice157-vu1-all-gen`
- `GT4_VU_CORPUS_DIR`: `C:/Antigravity/gt4-staticrecomp/private/slice155-vu1-full-corpus`

Reconstructed command:
```cmd
cmake -S . -B O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
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
cmake --build O:\gt4recomp-scratch\slice334-rc7-ra-spec2\build
```

### 3.2 Binary Artifact Hashes and Sizes

| Artifact | Build Dir | Size (Bytes) | SHA256 Hash |
| :--- | :--- | :--- | :--- |
| `gt4vulkan.exe` | `slice333-rc7-ra-build` | 97,702,400 | `56cdc30485e9c37d16c31bae8470c4207c29976906087db8666ed1c8ba6ff324` |
| `achievements_fixture.exe` | `slice333-rc7-ra-build` | 402,432 | `bf4341a833b908b358ca2df0b2d98e9dff14104b0001123b6b99c66e44073c80` |
| `achievements_spec2_fixture.exe` | `slice333-rc7-ra-build` | 104,448 | `e7a6af90972a21f6719fe59fff35c08dd75511988cf85d18998b77234bdd3569` |
| `achievements_http_fixture.exe` | `slice333-rc7-ra-build` | 40,960 | `a43c0d7bf9b630a54f3522583646b830782612575c1350edc1b97e52056cf03b` |
| `gt4vulkan-spec2.exe` | `slice334-rc7-ra-spec2` | 104,718,848 | `8497cb6ae7fa9ba93a6459c2b79f9b3f4d9ae8ee92415950a8492b2b6652b9d1` |
| `achievements_fixture.exe` | `slice334-rc7-ra-spec2` | 402,432 | `8b5f98e031b9793b132c2ed1b66dd43476415b44f1a395f6905ad1699fb3c451` |
| `achievements_spec2_fixture.exe` | `slice334-rc7-ra-spec2` | 104,448 | `9f827473177a9bb03bb44ff56a1a06aff49b1c73ea1f5e54a389c88da80c5b9c` |
| `achievements_http_fixture.exe` | `slice334-rc7-ra-spec2` | 40,960 | `ffa7f6f9da242bff44681fb2a35e97d91a3f102d4777d5f3d183bd8e8faed722` |

---

## 4. Launch Plan Composition, Safe Environment & Realistic Redaction Audit

### 4.1 Case-Insensitive Windows Environment Scrubbing
On Windows, process environment variables are looked up case-insensitively, allowing inherited or hostile variables like `gt4_ra_token`, `Gt4_Ra_Enabled`, or `GT4_RA_TOKEN` to leak past strict prefix checks. In `launcher/crates/gt4l-core/src/launch.rs`:
- Every environment variable key is normalized to uppercase (`upper = k.to_ascii_uppercase()`).
- All keys matching `GT4_RA_*` or `PS2_RA_*` are stripped unless explicitly forwarded by the launcher plan.
- The effective `LaunchPlan` sets `GT4_RA_ENABLED=1`, `GT4_RA_HARDCORE=1|0`, `GT4_RA_ENCORE=1|0`, `GT4_RA_USERNAME=<user>`, `GT4_RA_TOKEN=<tok>` strictly when signed in and enabled.

### 4.2 Realistic Redaction Audit
An audit was conducted on diagnostic and debug output paths:
- **Environment Scrubbing:** `LaunchPlan::Debug` and the newly introduced `LaunchPlan::safe_environment()` filter sensitive keys: any key whose name ends with `_TOKEN` or `_PASSWORD`, or equals `GT4_RA_TOKEN` / `GT4_RA_PASSWORD`, has its value replaced with `"[REDACTED]"`.
- **Command-line Arguments:** Process `arguments` (such as `--data-root` and target flags) are printed without modification as they contain filesystem paths and flags, not secret credentials. The earlier claim that arguments were masked was incorrect and has been corrected.
- **Diagnostic Logging in Tests:** In `launcher/crates/gt4l-tauri/src/flow_audit.rs` (line 494), an existing diagnostic note previously printed `plan.environment` directly. This was updated to `plan.safe_environment()`, ensuring that debug notes in flow audit tests never print raw credential tokens.
- **Unit Verification:** Automated tests in `retroachievements_tests.rs` assert that `format!("{plan:?}")` renders `[REDACTED]` for `GT4_RA_TOKEN` and never contains raw credential strings.

---

## 5. Tracked UI Preview Script, Exact Selectors & Focus Parity

### 5.1 Tracked Preview Script
The automated Playwright capture script is tracked in the repository at:
`docs/tasks/evidence/stage3-ui/capture_previews.js`

Invocations use headless Microsoft Edge against a local HTTP server serving `launcher/crates/gt4l-tauri/ui`:
```cmd
$env:NODE_PATH="C:\Antigravity\dashboard-modpack-mine\dashboard\node_modules"
node docs/tasks/evidence/stage3-ui/capture_previews.js
```

### 5.2 Exact DOM Selectors and Parity
The script asserts the actual DOM structure defined in `mock-data.js`:
- **Username:** `DemoDriver` (asserted via `.ra-page`).
- **Progress List Rows:** Exactly 3 rows (`.ra-ach-row.count() === 3` for GT4).
- **Spec II Unsupported Banner:** `.ra-unsupported-banner` is present with text `"RetroAchievements is not available for Gran Turismo 4 Spec II"` (and Portuguese equivalent), and row count is strictly 0.
- **Console & Page Errors:** 0 JavaScript errors logged across all test cases.

Generated previews in `docs/tasks/evidence/stage3-ui/`:
1. `achievements-en-us-gt4-signed-in.png` (66,764 bytes)
2. `achievements-en-us-gt4-signed-out.png` (43,178 bytes)
3. `achievements-en-us-gt4.png` (66,764 bytes)
4. `achievements-en-us-spec2.png` (58,463 bytes)
5. `achievements-pt-br-gt4-signed-in.png` (69,714 bytes)
6. `achievements-pt-br-gt4-signed-out.png` (44,438 bytes)
7. `achievements-pt-br-gt4.png` (69,714 bytes)
8. `achievements-pt-br-spec2.png` (61,303 bytes)

### 5.3 Focus Checks and Spatial Navigation
Automated execution of `dev/focus-check.html` and `dev/nav-test.html` is tracked via `docs/tasks/evidence/stage3-ui/run_ui_checks.js`, with output recorded in `docs/tasks/evidence/stage3-logs/ui-checks.log`:
- **English Focus Check (`focus-check.html?lang=en-US`):** `215 of 215 passed` (`body[data-focus-check]="pass"`), 0 JS errors.
- **Portuguese Focus Check (`focus-check.html?lang=pt-BR`):** `222 of 222 passed` (`body[data-focus-check]="pass"`), 0 JS errors.
- **Spatial Navigation Test (`nav-test.html`):** `150 of 150 passed`, 0 failures, 0 JS errors.

### 5.4 General Settings Row Order Provenance
The focus check strictly verifies the 4-row order on General settings:
```javascript
check("General lists Language, Channel, Version and GPU acceleration in that order",
  order.join(",") === "language,update_channel,launcher_version,launcher_gpu",
  order.join(","));
```
**Provenance:** Commit `40b90e81e547cdbf789d93922e580d4e0c7e751a` (`TASK-118: RC section last, GPU row under Version, plain API names, background textures always on`, 2026-10-26) introduced the launcher GPU acceleration toggle (`launcher_gpu`) directly under Version in `settings.js`. The focus check assertion in `focus-check.js` was brought into strict alignment with this DOM layout in Stage 3.

---

## 6. Real Network Race Tests, Account Isolation & Test Precision

### 6.1 Multi-Threaded Real TCP Test Server with Bounded Channel Gates
A multi-threaded TCP/HTTP test server was implemented in `launcher/crates/gt4l-tauri/src/flow_audit.rs` (`retroachievements_network_race_tests_with_real_tcp_server`). Accepted streams have `stream.set_nonblocking(false)` and `stream.set_read_timeout(Some(Duration::from_secs(3)))` configured to prevent nonblocking inheritance race conditions on Windows. Coordination between the fake server and test thread is handled via `SyncGate` bounded channels (`Sender<()>` and `Receiver<()>`) rather than sleeps:

1. **Delayed Login vs. Logout Race:**
   - Client calls `commands::ra_login_at(&url1, "UserA", "pass_wait", &shared1)`.
   - Fake server receives `"UserA"`, signals `arrived_tx.send(())`, and pauses on `release_rx.recv_timeout(Duration::from_secs(3))`.
   - Test thread confirms request arrival deterministically, executes `commands::ra_logout_of(&shared)`, and verifies disk config is immediately signed out.
   - Test thread releases the gate. The late login response completes, sees `ra_revision` incremented by logout, and discards the session.
   - Asserted: returned view reports `!signed_in`; persisted disk config remains signed out (`cfg.username == ""`).

2. **Delayed Login vs. Newer Login Race (UserOld vs. UserNew):**
   - Client calls `commands::ra_login_at(&url2, "UserOld", "pass_wait", &shared2)`.
   - Fake server confirms arrival of `"UserOld"` via channel gate and holds the response.
   - Client calls `commands::ra_login_at(&base_url, "UserNew", "pass_fast", &shared)`. Fake server immediately returns `UserNew` session.
   - Test thread asserts `UserNew` is signed in and written to disk.
   - Test thread releases `UserOld`. When `UserOld` finishes late, its result is discarded due to `ra_revision` mismatch.
   - Asserted: returned view for `UserOld` does not apply `UserOld`; persisted disk config retains `UserNew` (`tok_UserNew`).

3. **Delayed Refresh vs. Logout Race (Real `ra_status_at` TCP Path):**
   - Initial state: disk signed in as `RefreshUser` with token `tok_refresh_123` and score 1000.
   - Client calls real `commands::ra_status_at(&url3, &shared3)` over TCP.
   - Fake server receives POST `r=login2` with `RefreshUser`, signals arrival via channel gate, and holds the response.
   - Test thread confirms arrival, executes `commands::ra_logout_of(&shared)`.
   - Test thread releases the gate. The refreshed score (9999) response completes late and is discarded.
   - Asserted: returned view reports `!signed_in`; persisted disk config remains signed out.

4. **Delayed Refresh vs. Selected Game Switch Race (Real `ra_status_at` TCP Path):**
   - Initial state: disk signed in as `TargetUser`, game target set to `gt4`.
   - Client calls real `commands::ra_status_at(&url4, &shared4)` over TCP.
   - Fake server confirms arrival of `TargetUser` request and holds the response.
   - Test thread executes `commands::select_game_of("spec2".to_string(), &shared)`. Target switches to `spec2`, incrementing `ra_revision`.
   - Test thread releases the gate. The GT4 refresh completes late.
   - Asserted: returned view reports `selected_game = "spec2"`, `supported = false`, and `game = None` (stale GT4 overview discarded). Persisted credentials (`TargetUser`, `tok_target_123`) remain intact. Target restored to `gt4`.

5. **Interrupted Network Connection (`/abort`):**
   - Initial state: disk signed in as `AbortUser` with token `tok_abort_123` and score 777.
   - Client calls `commands::ra_status_at(&format!("{}/abort", base_url), &shared)`. Fake server drops the TCP connection immediately.
   - Because `ra_status_at` catches network failure via `.ok()`, it returns `Ok(RaStatusView)` falling back to local cached state with `game: None`.
   - Asserted: call succeeds with view reflecting cached score 777; persisted disk config remains intact.

6. **Failed Login: Service Error vs. Genuine HTTP 401:**
   - Initial state: disk signed in as `StableUser` with score 777.
   - **6A (Service JSON Error):** Client calls `ra_login_at` with `"badpass_service_fail"`. Server returns HTTP 200 with `{"Success": false, "Error": "Invalid user or password."}`. Returns `Err("Invalid user or password.")`. Disk state remains `StableUser` (777).
   - **6B (HTTP 401 Unauthorized):** Client calls `ra_login_at` with `"badpass_http_401"`. Server returns `HTTP/1.1 401 Unauthorized` with empty body. In `gt4l-core`, `.http_status_as_error(false)` delivers the non-JSON payload, which fails JSON deserialization, returning `Err("Unexpected response from RetroAchievements.")`. Disk state remains `StableUser` (777).

### 6.2 State-Versioning vs. Network Cancellation
- **Mechanism:** Concurrency control is achieved strictly through **state-versioning / revision fencing**. Each mutating action (login, logout, game target switch) increments `state.ra_revision`. When a background request completes, its snapshot revision and selected game are compared against current state. If `ra_revision` or `selected.id` differs, the result is discarded.
- **Limitation:** The launcher uses blocking `ureq` HTTP calls inside `tauri::async_runtime::spawn_blocking`. Requests in flight are **not cancelled** at the network socket layer; they run to completion or socket timeout in the background thread pool, but their output is fenced from modifying launcher state.

### 6.3 CTest Full Suite & Baseline Precision
Full CTest runs were executed against both fresh builds:
- **`ctest-full-v200.log`:** 48/50 passed.
  - Test #10: `native_audio_input` failed (4.70s).
  - Test #36: `python_native_contracts` timed out at 120.05s.
- **`ctest-full-spec2.log`:** 48/50 passed.
  - Test #26: `native_mtvu_ring` timed out at 120.28s.
  - Test #36: `python_native_contracts` timed out at 121.05s.
- **Isolated Re-runs & Uncertainty:** Both `native_audio_input` and `native_mtvu_ring` pass when executed in isolation. However, per AGENTS.md guidelines, passing in isolation does not definitively prove test load or system scheduling as the sole cause. The results are reported as observed, with uncertainty acknowledged.

### 6.4 Direct Python Test Suite
Direct execution using `private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python` (recorded in `docs/tasks/evidence/stage3-logs/python-spec2-full.log` and `python-v200-full.log`):
- Total tests: 710
- Passed: 642
- Skipped: 55
- Failed: 13
- **Failure Identification:** All 13 failures are identical across v200 and Spec II candidates and match the established baseline debt in `docs/tasks/TASK-133-BASELINE-REPORT.md`:
  1. `test_gs_live.ConfigTests.test_defaults_are_vulkan_3x_high_blending_automatic_deinterlace_16_9_vsync` (first failure, caused by `LoadTextureReplacementsAsync = true` added in TASK-118).
  2. `test_gs_settings.DefaultTests.test_cache_and_headless_sections_are_unchanged`
  3. `test_gs_settings.DefaultTests.test_default_file_bytes_match_the_main_writer`
  4. `test_gs_settings.DefaultTests.test_default_ini_is_main_default`
  5. `test_gs_settings.DefaultTests.test_probe_default_is_unchanged`
  6. `test_gs_settings.DefaultTests.test_run_text_with_data_root_and_eager_switch`
  7. `test_gs_settings.IniTests.test_user_settings_replace_defaults_and_add_when_set_rows`
  8. `test_gs_settings.OfferedTests.test_launcher_offers_only_settable_options`
  9. `test_gs_settings.OsdRowTests.test_overlay_rows_are_advanced_toggles_off_by_default`
  10. `test_gs_settings.PageLayoutTests.test_advanced_page_holds_the_rest_of_the_user_options`
  11. `test_gs_settings.ProhibitDynamicCodeTests.test_row_is_an_advanced_always_written_switch_defaulting_to_false`
  12. `test_gs_settings.ResetToDefaultsTests.test_reset_gives_the_exact_default_ini`
  13. `test_gs_settings.ResetToDefaultsTests.test_when_set_row_equal_to_its_default_is_not_written`

### 6.5 Cargo Test Workspace Summary
- Command: `python scratch/run_cargo.py test --workspace` (with `CARGO_TARGET_DIR=O:\gt4recomp-scratch\task133-cargo-target`)
- Log: `docs/tasks/evidence/stage3-logs/cargo-test-workspace.log`
- Reconciled Total: Exactly 34 `test result` blocks totaling **574 tests passed, 0 failed, 5 ignored** across the workspace crates (`gt4l-core`, `gt4l-tauri`, `gt4saves`, `gt4controls`, `gt4l-disc`, `gt4l-platform`).

---

## 7. Limitations & Explicit Stop for Review

- **Proven Scope:**
  - Complete RetroAchievements configuration serialization and persistence in installed and portable modes.
  - Case-insensitive Windows environment variable scrubbing and token redaction (`safe_environment`) in launch plans.
  - Deterministic concurrency race handling across login/logout, newer login, refresh, and target switch using channel-gated fake HTTP server verification.
  - Graceful fallback to cached state on interrupted network connections without state corruption.
  - Strict UI layout, spatial navigation, and bilingual localization (en-US / pt-BR) verified with 0 JavaScript errors.
- **Untested / Out of Scope for Stage 3:**
  - Automated tests do not prove full game execution, gameplay progress, or real RetroAchievements server interaction with live accounts.
  - Controller curve mappings, deadzone tuning, and wheel bridge features are not part of Stage 3.
  - Packaging, NSIS installer compilation, and release artifact drafting remain untouched.
- **Review Stop:** All work on Stage 3 is paused pending Codex Orchestrator review.
