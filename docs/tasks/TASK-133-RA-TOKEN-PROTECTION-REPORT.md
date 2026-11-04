# TASK-133: RetroAchievements Credential Protection Report

Date: 2026-10-27.  
Branch: `task/133-rc7-ra-controls`  
Worktree: `C:/Antigravity/gt4-wt-133-rc7-ra-controls`  
Starting Base: `adfd5132b775d354d4a5bb33152fb23fb542b85e`  
Review 1 Base: `c3c9959`  
Review 2 Base: `25c5070`  
Review 3 Base: `5177830`  
Author: João Alano <57041838+moesuito@users.noreply.github.com>

---

## 1. Executive Summary & Architecture Overview

This work implements secure protection at rest for RetroAchievements authentication tokens on Windows across both installed and portable launcher layouts, addressing all requirements of `TASK-133-RA-TOKEN-PROTECTION.md` and resolving the findings in `TASK-133-RA-TOKEN-PROTECTION-REVIEW.md`, `TASK-133-RA-TOKEN-PROTECTION-REVIEW-2.md`, and `TASK-133-RA-TOKEN-PROTECTION-REVIEW-3.md`.

### Core Design Principles
1. **Windows DPAPI Standard:** Uses Win32 Data Protection API (`CryptProtectData` and `CryptUnprotectData` via `crypt32.dll` and `kernel32.dll`) bound strictly to the current Windows user (`dwFlags = CRYPTPROTECT_UI_FORBIDDEN`, no machine-wide scope `CRYPTPROTECT_LOCAL_MACHINE`). No custom cryptography or bundled symmetric keys.
2. **Platform-Neutral Abstraction:** Introduced a clean `SecretProtector` trait and `SecretError` enum in `launcher/crates/gt4l-core/src/secret.rs`. Non-Windows platforms return explicit `UnsupportedPlatform` errors without silently persisting plaintext or falling back to fake implementations.
3. **Deterministic Testability & Seams:** Implemented `FakeProtector` for unit/concurrency tests and `AtomicFileOps` trait (`StdAtomicFileOps`, `MockAtomicFileOps`) for deterministic filesystem failure injection.
4. **Zero Plaintext at Rest & Serde Serialization Guard:** In `RaConfig`, `token` is annotated with `#[serde(default, skip_serializing)]`, ensuring that the TOML serializer can never write plaintext tokens to disk under any condition. `token_protected` stores the versioned envelope `dpapi:v1:<hex>`.
5. **Exclusive Temporary File Ownership:** Files are created exclusively (`OpenOptions::create_new(true)`) with up to 10 retry attempts on collision. `TempFileGuard` is constructed and armed strictly *after* exclusive creation succeeds on that attempt. Pre-existing files (e.g. from PID reuse or prior crashes) are never truncated and never deleted on failure.
6. **Command Boundary Safety:** Launcher commands (`ra_status_at_with`, `ra_set_options_with`, `ra_set_options_with_custom`, `ra_login_at_with`, `ra_logout_with`) provide injectable seams for deterministic integration testing of migration/write failures, option updates, replacement logins, intentional logouts, and competing/stale network responses.
7. **Strict Spec II Isolation:** Gran Turismo 4 Spec II mod playthroughs remain completely isolated: RA launch environment is omitted, `GT4_RA_ENABLED=0` is explicitly set, and no credentials or envelopes are forwarded.

---

## 2. Review 3 Findings & Implemented Corrections

### Finding 1: Exclusive Temporary-File Ownership & Armed Guard
- **Problem:** Temp names were derived from process ID and an atomic counter, but opened with `File::create` with the `TempFileGuard` constructed before the open call. A pre-existing file with that candidate name (e.g. from PID reuse or prior crash) could be truncated, or deleted on open/write failure. PID plus counter alone does not establish exclusive ownership across process lifecycles.
- **Fix:**
  - Redefined `AtomicFileOps` with explicit stages: `open_exclusive(temp_path) -> Result<File>`, `write_and_flush(file, temp_path, content) -> Result<()>`, and `rename_replace(temp_path, target) -> Result<()>`.
  - `StdAtomicFileOps::open_exclusive` opens via `OpenOptions::new().write(true).create_new(true).open(temp_path)`. If `ErrorKind::AlreadyExists` occurs, it retries up to 10 times with the next atomic counter value.
  - `TempFileGuard::arm(&tmp)` is instantiated strictly **after** `open_exclusive` returns `Ok(file)`. If creation fails or collisions occur, no guard is ever armed for that pre-existing candidate path, guaranteeing pre-existing files are never deleted on failure.
  - `write_and_flush` takes ownership of `File`, writes, flushes (`sync_all()`), and explicitly drops the handle before returning, preventing Windows sharing violation errors during subsequent rename or cleanup.
- **Evidence:** Tested in `retroachievements_tests::interrupted_and_failed_atomic_writes_are_safe`:
  - Verified pre-existing sentinel file `.retroachievements.toml.tmp-{pid}-{counter}` collides on first attempt, retries succeed, and sentinel file is preserved byte-for-byte.
  - Multi-collision mock retry (3 collisions then success) and 10-attempt exhaustion handling.
  - Pre-open failure preserves sentinel file and unrelated files untouched.
  - Partial write failure (15 bytes) sets `partial_write_reached = true`, cleans up only the newly created partial file, and preserves sentinel file.
  - Flush failure and replace failure clean up only the owned temporary file.

### Finding 2: Corrected Write-Failure Seam in Scenario 8C
- **Problem:** In `flow_audit.rs` Scenario 8C, `fail_protector` and `fail_during_write` were both active. Protection failed before the partial write seam was reached, meaning the test did not verify the filesystem write failure.
- **Fix:** Used a functioning protector (`working_protector = FakeProtector::default()`) so secret protection succeeds and `MockAtomicFileOps` is actually reached. Added `partial_write_reached: Arc<AtomicBool>` to `MockAtomicFileOps`.
- **Evidence:** Verified in `flow_audit.rs` Scenario 8C:
  - `write_fail_res.is_err()` is true with write failure message (`cannot write` / `injected partial write failure`).
  - `partial_write_ops.partial_write_reached.load(Ordering::SeqCst)` is asserted `true`.
  - Legacy `retroachievements.toml` on disk remains 100% byte-for-byte intact.
  - No temporary files remain in the config folder.

### Finding 3: Real Competing Launcher Action in Scenario 8D
- **Problem:** Scenario 8D previously simulated a race by manually bumping `state.ra_revision = state.ra_revision.wrapping_add(1)` rather than executing a real competing command at the launcher command boundary.
- **Fix:** Replaced manual revision bump with an actual command call: `commands::ra_logout_with(&shared, &working_protector)`. While the TCP test server held an in-flight login request (`commands::ra_login_at_with` for `StaleCandidate`) at `SyncGate`, the competing logout command was executed at the boundary.
- **Evidence:** Verified in `flow_audit.rs` Scenario 8D:
  - Competing `ra_logout_with` succeeded, signed out immediately, cleared credentials, and persisted empty configuration to disk.
  - Upon gate release, the late `StaleCandidate` response completed and was discarded because `state.ra_revision != revision`.
  - Late response did not apply `StaleCandidate` and did not resurrect the old legacy plaintext token `tok_do_not_erase_me`.
  - Scenario 8E verified subsequent replacement login (`commands::ra_login_at_with`) for `FreshUser` cleanly replaced the config with a protected envelope.

### Finding 4: Diagnostic Exporter Filter Scheme & Collection Safety
- **Problem:** Exporter `is_forbidden` checked `name == "retroachievements.toml" || name.starts_with("retroachievements.toml.")`, but the temporary naming scheme starts with a leading dot: `.{filename}.tmp-...`.
- **Fix:**
  - Updated `is_forbidden` in `launcher/crates/gt4l-core/src/log_export.rs`:
    `let trimmed = name.strip_prefix('.').unwrap_or(name);`
    `if trimmed == "retroachievements.toml" || trimmed.starts_with("retroachievements.toml.") { return true; }`
  - Added unit regression test cases in `cards_saves_replays_discs_prepared_files_and_programs_are_forbidden` for `.retroachievements.toml.tmp-1234-1-1`, `.RETROACHIEVEMENTS.TOML.TMP-9999-5`, and `config/.retroachievements.toml.tmp-555-1`.
  - Inspected production collection paths in `launcher/crates/gt4l-tauri/src/log_bundle.rs`: confirmed that `collect()` uses an explicit allowlist (`logs/latest.log`, newest session logs, session `game.log`/`gt4-gs.ini`, `settings.toml`, `system.txt`). It never traverses `config/` or collects `retroachievements.toml` or any temporary files. The `is_forbidden` check is a defense-in-depth gate at the `write_zip` boundary.

---

## 3. Threat Model Boundaries & Security Disclosures

1. **What Windows DPAPI Protects:**
   - Protects the stored RetroAchievements authentication token at rest against offline extraction (e.g. stolen laptop, offline disk imaging, backup extraction by unauthorized parties).
   - Protects against other user accounts on the same Windows computer reading or using the stored credentials.
2. **What Windows DPAPI Does Not Protect (Real Limitations):**
   - **Same-User Execution Context:** Does not protect against malware, scripts, or untrusted code running within the *same* Windows user session (as that process runs with the user's logon security token and can call `CryptUnprotectData`).
   - **Process Memory Inspection:** Does not protect against local memory scraping or debuggers inspecting the launcher or game process while running.
   - **Process Environment Handoff:** Does not alter how the token is handed off to `gt4vulkan.exe` via process environment variables (`GT4_RA_TOKEN`) during game launch.
3. **Concurrency & Locking Limits:**
   - Unique temporary file paths prevent file-system collisions and accidental deletion of other processes' in-progress writes.
   - **However**, separate running launcher instances modifying the same configuration file are serialized at the file-system replacement boundary under **last-write-wins** semantics. Application-level file locking is not implemented.
4. **Normal Errors vs Sudden Termination / Power Loss:**
   - **Normal Runtime Errors:** An armed `TempFileGuard` executes upon unwind or early error return, immediately deleting the unpersisted temporary file from disk.
   - **Sudden Process Crash / SIGKILL / Power Loss:** In an abrupt abnormal termination or power loss, Rust `Drop` guards do not execute. The temporary file may remain as an orphan on disk.
   - **Orphan File Safety:** Orphaned temporary files contain only encrypted ciphertext envelopes (`token_protected`) and never plaintext secrets (`token` is `#[serde(skip_serializing)]`). On the next startup, `RaConfig::load` loads the valid target file and ignores orphaned `.tmp` files.
5. **Storage & Power-Loss Durability Limits:**
   - Atomic replacement (`create_new` -> `write` -> `sync_all` -> `rename`) prevents partial writes on standard NTFS/ext4 under normal conditions.
   - No unevidenced power-loss durability guarantees are made for underlying hardware or journaling edge cases.
   - Does not perform physical cryptographic erasure of underlying storage blocks (SSD wear-leveling / TRIM); previous historical backups created by the OS or user prior to rc7 may retain plaintext tokens.
6. **Downgrade Behavior:**
   - If a user downgrades their installation from rc7 to 1.2.4 or 1.2.5, older versions do not recognize the `token_protected` envelope. The user will simply need to enter their credentials again. The launcher never downgrades ciphertext back to plaintext.

---

## 4. Automated Verification Results

All tests were executed serially from the workspace root `C:/Antigravity/gt4-wt-133-rc7-ra-controls`.

### 4.1 RetroAchievements Unit & Integration Test Suite (`gt4l-core`)
- **Command:**
  ```powershell
  cargo test --test retroachievements_tests -p gt4l-core --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Result:** **21 passed, 0 failed, 0 ignored** (finished in 0.14s)
  - `diagnostic_export_forbids_retroachievements_file` ... ok
  - `diagnostic_export_exclusions_comprehensive` ... ok
  - `malformed_non_ascii_and_truncated_ciphertext_never_panics` ... ok
  - `launch_environment_includes_language_tag` ... ok
  - `debug_and_serialization_paths_never_leak_plaintext_token` ... ok
  - `fake_protector_encrypt_decrypt_and_injected_failures` ... ok
  - `real_windows_dpapi_roundtrip_tampered_blob_and_empty_handling` ... ok
  - `retroachievements_cache_explicitly_resolves_under_gt4_in_modern_layout` ... ok
  - `hostile_inherited_retroachievements_environment_is_scrubbed` ... ok
  - `failed_migration_leaves_original_plaintext_file_untouched` ... ok
  - `hostile_inherited_retroachievements_environment_with_case_variations_is_scrubbed_and_redacted` ... ok
  - `switching_games_preserves_shared_retroachievements_file` ... ok
  - `legacy_plaintext_migration_from_124_125_config_and_clean_disk_replacement` ... ok
  - `migration_failure_prevents_option_updates_from_destroying_legacy_token` ... ok
  - `unprotectable_token_preserves_ciphertext_and_non_secret_settings` ... ok
  - `retroachievements_config_125_compatibility_and_malformed_handling` ... ok
  - `real_windows_dpapi_end_to_end_migration_and_persistence` ... ok
  - `two_portable_profiles_and_installed_profile_independence` ... ok
  - `retroachievements_config_roundtrips_in_installed_and_portable_layouts` ... ok
  - `interrupted_and_failed_atomic_writes_are_safe` ... ok
  - `fake_server_http_login_refresh_and_failure_scenarios` ... ok

### 4.2 Log Export Unit Tests (`gt4l-core`)
- **Command:**
  ```powershell
  cargo test -p gt4l-core log_export --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Result:** **7 passed, 0 failed, 0 ignored**
  - `log_export::tests::cards_saves_replays_discs_prepared_files_and_programs_are_forbidden` ... ok
  - `log_export::tests::masking_applies_to_every_entry` ... ok
  - `log_export::tests::the_bundle_is_named_by_local_time` ... ok
  - `log_export::tests::the_newest_items_are_chosen_newest_first` ... ok
  - `log_export::tests::the_system_report_names_every_fact_and_no_user_path` ... ok
  - `log_export::tests::the_user_name_and_the_profile_paths_are_masked` ... ok
  - `log_export::tests::the_zip_holds_exactly_the_entries_and_no_forbidden_file` ... ok

### 4.3 Flow Audit & Gated Network Race Tests (`gt4l-tauri`)
- **Command:**
  ```powershell
  $env:GT4_SDL3_ROOT = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18"
  cargo test -p gt4l-tauri retroachievements_network_race_tests_with_real_tcp_server --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Result:** **1 passed, 0 failed, 0 ignored** (finished in 0.19s)
  - `flow_audit::retroachievements_network_race_tests_with_real_tcp_server` ... ok

### 4.4 Full Rust Workspace Test Suite Reconciliation
- **Command:**
  ```powershell
  $env:GT4_SDL3_ROOT = "C:/Antigravity/gt4-staticrecomp/private/tooling/sdl3-3.4.18"
  cargo test --workspace --target-dir O:\gt4recomp-scratch\task133-cargo-target-v2 --manifest-path C:\Antigravity\gt4-wt-133-rc7-ra-controls\launcher\Cargo.toml
  ```
- **Durable Log:** `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`
- **Reconciliation of Workspace Totals:**
  - Total test result blocks: **34** (29 executable test suites + 5 doc-tests)
  - Total tests passed: **589**
  - Total tests failed: **0**
  - Total tests ignored: **5** (2 in `real_disc.rs`, 2 in `gt4l_platform`, 1 in `gt4l-tauri`)
- **Complete Suite Breakdown:**
  1. `gt4controls` (lib): 18 passed, 0 failed, 0 ignored
  2. `gt4controls` (docs_match): 4 passed, 0 failed, 0 ignored
  3. `gt4l_core` (lib): 215 passed, 0 failed, 0 ignored
  4. `gt4l_core` (config_migration): 16 passed, 0 failed, 0 ignored
  5. `gt4l_core` (enabled_values): 3 passed, 0 failed, 0 ignored
  6. `gt4l_core` (gs_golden): 5 passed, 0 failed, 0 ignored
  7. `gt4l_core` (gs_release): 11 passed, 0 failed, 0 ignored
  8. `gt4l_core` (launch_contract): 21 passed, 0 failed, 0 ignored
  9. `gt4l_core` (multi_game): 5 passed, 0 failed, 0 ignored
  10. `gt4l_core` (release_lists): 5 passed, 0 failed, 0 ignored
  11. `gt4l_core` (relocate): 20 passed, 0 failed, 0 ignored
  12. `gt4l_core` (retroachievements_tests): 21 passed, 0 failed, 0 ignored
  13. `gt4l_core` (settings_config): 9 passed, 0 failed, 0 ignored
  14. `gt4l_core` (textures): 7 passed, 0 failed, 0 ignored
  15. `gt4l_disc` (lib): 6 passed, 0 failed, 0 ignored
  16. `gt4l_disc` (elf_rebuild): 3 passed, 0 failed, 0 ignored
  17. `gt4l_disc` (real_disc): 1 passed, 0 failed, 2 ignored
  18. `gt4l_disc` (setup_fake_iso): 12 passed, 0 failed, 0 ignored
  19. `gt4l_disc` (spec2_identify): 8 passed, 0 failed, 0 ignored
  20. `gt4l_platform` (lib): 48 passed, 0 failed, 2 ignored
  21. `gt4l_platform` (job_breakaway): 4 passed, 0 failed, 0 ignored
  22. `gt4l-tauri` / `GT4Recomp` (lib/bin, including `flow_audit` steps 1-8): 92 passed, 0 failed, 1 ignored
  23. `gt4saves` (lib): 6 passed, 0 failed, 0 ignored
  24. `gt4saves` (card_owner): 6 passed, 0 failed, 0 ignored
  25. `gt4saves` (hostile_cards): 10 passed, 0 failed, 0 ignored
  26. `gt4saves` (library): 12 passed, 0 failed, 0 ignored
  27. `gt4saves` (migration): 7 passed, 0 failed, 0 ignored
  28. `gt4saves` (per_game): 5 passed, 0 failed, 0 ignored
  29. `gt4saves` (save_management): 9 passed, 0 failed, 0 ignored
  30-34. Doc-tests (`gt4controls`, `gt4l_core`, `gt4l_disc`, `gt4l_platform`, `gt4saves`): 0 passed each

### 4.5 Playwright UI Focus and Spatial Navigation Checks
- **Command:**
  ```powershell
  $env:NODE_PATH = "C:\Users\Alano\AppData\Roaming\npm\node_modules"
  node C:\Antigravity\gt4-wt-133-rc7-ra-controls\docs\tasks\evidence\stage4-ui\run_ui_checks.js
  ```
- **Durable Log:** `docs/tasks/evidence/ra-token-logs/ui-checks.log`
- **Results:**
  - Focus Check (`focus-check.html?lang=en-US`): **215 of 215 passed**, 0 JS errors (`body[data-focus-check]="pass"`).
  - Focus Check (`focus-check.html?lang=pt-BR`): **222 of 222 passed**, 0 JS errors (`body[data-focus-check]="pass"`).
  - Spatial Navigation (`nav-test.html`): **209 of 209 passed**, 0 failures, 0 JS errors.

### 4.6 Native CTest Provenance & Baseline Status
- **Candidate Slices:** `slice335` (v2.00) and `slice336` (Spec II) built during Stage 4.
- **Status:** Native binaries were not modified in this launcher token protection task. Existing durable logs remain documented in `docs/tasks/evidence/stage4-logs/`:
  - `ctest-full-v200.log`: 49/50 passed; 1 baseline failure in `python_native_contracts` (710 tests, 13 known GS settings failures, 42 skipped).
  - `ctest-full-spec2.log`: 49/50 passed; 1 baseline failure in `python_native_contracts` (710 tests, 13 known GS settings failures, 55 skipped).
  - `curve-parity.log`: 8,960 / 8,960 bit-accurate matches (100.0000%).
- Unaffected native binaries are not falsely presented as newly executed.

---

## 5. Scope & Artifact Disposition

- **Worktree:** Strictly on `C:/Antigravity/gt4-wt-133-rc7-ra-controls`.
- **Branch:** `task/133-rc7-ra-controls`.
- **Files Modified for Review 3:**
  - `launcher/crates/gt4l-core/src/log_export.rs`
  - `launcher/crates/gt4l-core/src/retroachievements.rs`
  - `launcher/crates/gt4l-core/tests/retroachievements_tests.rs`
  - `launcher/crates/gt4l-tauri/src/flow_audit.rs`
  - `docs/tasks/evidence/ra-token-logs/cargo-test-workspace.log`
  - `docs/tasks/evidence/ra-token-logs/ui-checks.log`
  - `docs/tasks/TASK-133-RA-TOKEN-PROTECTION-REPORT.md`
- **Zero Real Credentials or Live Server Calls:** All tests use synthetic data, `FakeProtector`, local TCP server loops, or offline DPAPI round-trips.
- **Zero Game/Installer Execution:** Neither the installer nor the game executable was launched.
- Ready for independent review by Codex before proceeding to Discord Rich Presence.
