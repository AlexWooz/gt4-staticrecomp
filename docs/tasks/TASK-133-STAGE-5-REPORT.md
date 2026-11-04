# TASK-133 Stage 5: Local rc7 Test Kit Assembly & Verification Report (Revision 2)

Date: 2026-10-27  
Workspace: `C:\Antigravity\gt4-wt-133-rc7-ra-controls`  
Head Commit at Review Base: `db0150c`  
Branch: `task/133-rc7-ra-controls`  
Author: João Alano <57041838+moesuito@users.noreply.github.com>

---

## 1. Executive Summary & Review Adjustments

Following Codex Orchestrator review (`docs/tasks/TASK-133-STAGE-5-REVIEW.md`), this revised Stage 5 report records the exact corrected provenance, release notes, and independent package verification for the local **1.3.0-rc7 test kit**:

1. **Release Notes Corrections:**
   - **Accurate RetroAchievements Location & Flow:** The release notes accurately state that RetroAchievements account connection is managed in **Settings > Achievements** (em português: **Configurações > Conquistas**) using username and password.
   - **Credential Persistence Clarification:** Removed misleading claims regarding encryption or zero plain text credentials. Notes accurately state that login exchanges credentials with RetroAchievements `dorequest.php` for an authentication token; the username and session token are persisted in plain text TOML (`retroachievements.toml`) in the configuration directory (passwords are not saved).
   - **Controls & Physics Clarification:** Removed any implication of actual physics or game timing changes. The 7 selectable curves modify analog input mapping only, while vehicle physics and 1/60s simulation timing remain untouched.
   - **Update Expectations:** Removed claims of an automatic updater from rc6 for this unpublished local test kit; installation and testing are performed directly via the local installer or portable folder.
2. **Provenance & Toolchain Precision:**
   - Corrected candidate source paths to their verified locations on drive `O:`:
     - v200: `O:\gt4recomp-scratch\slice335-rc7-controls-build\build\gt4vulkan.exe`
     - Spec II: `O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build\gt4vulkan-spec2.exe`
   - Corrected toolchain metadata in candidate manifests (`slice335-pcsx2.json` and `slice336-spec2.json`) to reflect measured environment versions: CMake 4.3.1, Ninja 1.13.2, Clang-cl 22.1.3.
3. **Targeted CTest Precision:**
   - Corrected the names of the 5 targeted tests executed per configuration:
     `native_audio_input`, `native_controls`, `native_achievements`, `native_achievements_spec2`, `native_achievements_http`.
4. **Rebuilt Fresh Test Kit (Kit v2):**
   - Because launcher-embedded release notes (`build.rs` embeds `docs/release-notes/launcher/1.3.0.md` and `pt-BR/1.3.0.md`) were updated, fresh launcher binaries and packages were built in `O:\gt4recomp-scratch\task133-cargo-target-v2` and packaged into `O:\gt4recomp-scratch\task133-rc7-kit-v2`. The original kit in `task133-rc7-kit` was preserved intact for comparison.

---

## 2. Toolchain & Environment Provenance

Measured directly from the active build environment:
- **Compiler:** Clang-cl 22.1.3 (`C:/Program Files/LLVM/bin/clang-cl.exe`) / MSVC 17.14
- **Build Generator:** CMake 4.3.1 (`C:/Program Files/CMake/bin/cmake.exe`), Ninja 1.13.2
- **Rust Toolchain:** cargo 1.99.0 (5f94df478 2026-08-27), rustc 1.99.0 (b940084d7 2026-09-28)
- **Tauri CLI:** 2.11.0
- **NSIS:** Makensis v3.13 (`C:\Program Files (x86)\NSIS\makensis.exe`)
- **Archive Tool:** 7-Zip 25.01 x64 (`C:\Program Files\7-Zip\7z.exe`)
- **Python:** Python 3.14.2 (virtual environment `private/tooling-venv/Scripts/python.exe`)
- **Node / Playwright:** Node.js v25.3.0 with Playwright in `C:\Users\Alano\AppData\Roaming\npm\node_modules`

---

## 3. Candidate Binaries & Staging Provenance

### 3.1 Candidate Native Executables (Stage 4 Candidate Builds)
- **Gran Turismo 4 v2.00 (SCUS-97328):**
  - Source Path: `O:\gt4recomp-scratch\slice335-rc7-controls-build\build\gt4vulkan.exe`
  - Distributed As: `gt4.exe`
  - Size: 97,702,912 bytes
  - SHA-256: `17406ba4bfcd3ded71e68427c45b3528b43c1caccb3c4872fda3ded5634ae917`
  - Pinned Manifest: `docs/inputs/slice335-pcsx2.json`
- **Gran Turismo 4 Spec II (Mod 1.10):**
  - Source Path: `O:\gt4recomp-scratch\slice336-rc7-controls-spec2\build\gt4vulkan-spec2.exe`
  - Distributed As: `gt4-spec2.exe`
  - Size: 104,719,872 bytes
  - SHA-256: `67951470d1faf767c8d619bac5483bbfd30f9b164e6aaeb04e7ad20858083f1a`
  - Pinned Manifest: `docs/inputs/slice336-spec2.json`

### 3.2 Game Distribution Directory
Staged via `scripts/make_distribution.py`:
`C:\Antigravity\gt4-staticrecomp\private\slice337-dist-130-rc7` (19 files total)
- `gt4.exe` (97,702,912 bytes, `17406ba4...`)
- `gt4-spec2.exe` (104,719,872 bytes, `67951470...`)
- `SDL3.dll` (2,848,256 bytes, SDL 3.4.18, `785ae6a8...`)
- CRT Runtime: `msvcp140.dll` (557,728 bytes), `msvcp140_atomic_wait.dll` (50,304 bytes), `vcruntime140.dll` (124,544 bytes)
- `MANIFEST.json` (3,796 bytes)
- Compute Shaders (`shaders/`, 9 SPV files)
- Licenses (`licenses/`, 4 files)

### 3.3 Rebuilt Release Launcher Binaries (Kit v2)
Built with `launcher/tools/build-release.ps1 -Label rc7`:
- **Installer Edition (`GT4Recomp.exe`):**
  - Path: `O:\gt4recomp-scratch\task133-cargo-target-v2\release\GT4Recomp.exe`
  - Size: 17,949,696 bytes
  - SHA-256: `444f5befc8caee61fe26467d346d0e629ee2760c0dfd3ce7a8d786431b3319eb`
  - Build/machine path scan: 0 hits (clean).
- **Portable Edition (`GT4Recomp.exe`):**
  - Path: `O:\gt4recomp-scratch\task133-cargo-target-v2-portable\release\GT4Recomp.exe`
  - Size: 17,936,896 bytes
  - SHA-256: `5fa6060d404bd1e581e1f0bf526975be410176dcb6fa850481498f6999dc7e45`
  - Build/machine path scan: 0 hits (clean).

### 3.4 Launcher Release Notes Embedding Contract
In `launcher/crates/gt4l-tauri/build.rs`, `embed_release_notes()` reads numeric versions `(u32, u32, u32)` up to `CARGO_PKG_VERSION` (`1.3.0`).
- The en-US embedded note is `docs/release-notes/launcher/1.3.0.md`.
- The pt-BR embedded note is `docs/release-notes/launcher/pt-BR/1.3.0.md`.
- `docs/release-notes/launcher/1.3.0-rc7.md` and its `pt-BR` sibling are kept for documentation parity with full notes `docs/release-notes/1.3.0-rc7.md`. Both 1.3.0 and 1.3.0-rc7 note sets share identical content bullets.

---

## 4. Test Kit Packages & Verification Receipts

Output destination: `O:\gt4recomp-scratch\task133-rc7-kit-v2\`

| Package Asset | Size (bytes) | SHA-256 Hash |
| :--- | :--- | :--- |
| `GT4Recomp-Setup-1.3.0-rc7.exe` | 51,816,606 | `03b714a3292b27ec410f07b1d460e2b7e8209c2d01f8a12a1ca7cbf18ffba4fe` |
| `GT4Recomp-1.3.0-rc7-portable.zip` | 83,612,296 | `425ec22c07d92e0af1efbde43c58c4ef809304f12a7916c28208aca162ae741f` |

### `SHA256SUMS` (Kit v2):
```text
03b714a3292b27ec410f07b1d460e2b7e8209c2d01f8a12a1ca7cbf18ffba4fe  GT4Recomp-Setup-1.3.0-rc7.exe
425ec22c07d92e0af1efbde43c58c4ef809304f12a7916c28208aca162ae741f  GT4Recomp-1.3.0-rc7-portable.zip
```

---

## 5. Independent Archive Extraction & Member Integrity Analysis

Both v2 archives were independently unpacked with 7-Zip (`7z x`) into isolated directories under `O:\gt4recomp-scratch\task133-kit-v2-verification\`:
- Portable archive: 25 files, 2 empty directories (`user/`, `data/`).
- Setup archive: 30 files (including `$PLUGINSDIR` and `.update-staging`).

### 5.1 Member Bit-Exact Verification Against Source Artifacts
- `game/gt4.exe` (both archives) == `17406ba4bfcd3ded71e68427c45b3528b43c1caccb3c4872fda3ded5634ae917` (Exact match).
- `game/gt4-spec2.exe` (both archives) == `67951470d1faf767c8d619bac5483bbfd30f9b164e6aaeb04e7ad20858083f1a` (Exact match).
- Setup `.update-staging/GT4Recomp.exe` == `444f5befc8caee61fe26467d346d0e629ee2760c0dfd3ce7a8d786431b3319eb` (Exact match).
- Portable `GT4Recomp.exe` == `5fa6060d404bd1e581e1f0bf526975be410176dcb6fa850481498f6999dc7e45` (Exact match).
- Setup `$PLUGINSDIR/MicrosoftEdgeWebview2Setup.exe` == `f414de01c4680f0bb0694d1ea7803c6968f3a81bf6bad01b89bfc65dce14c2a2` (Exact match).
- All 9 SPV shaders, 3 CRT DLLs, 4 licenses, `SDL3.dll`, and `MANIFEST.json` matched their staged counterparts in `slice337-dist-130-rc7` bit-for-bit.

### 5.2 Deep Header & Content Inspection
Beyond extension checks, every file's byte headers were inspected:
- **ELF Magic (`\x7fELF`):** 0 occurrences.
- **ISO 9660 Volume Descriptor (`CD001` at 0x8001 / 0x8801 / 0x9001):** 0 occurrences.
- **PS2 Memory Card Format Magic (`Sony PS2 Memory Card Format`):** 0 occurrences.
- **Plain Text Password Persistence:** 0 occurrences.
Full JSON receipt generated at `O:\gt4recomp-scratch\task133-rc7-kit-v2\INSPECTION-RECEIPT.json`.

---

## 6. Automated Controls Summary

1. **Locales Parity:**
   - 604 keys in `en-US.json` and `pt-BR.json`.
   - Missing keys: 0. Mismatched placeholders: 0.
2. **Playwright UI Automation (Node.js + Headless Edge):**
   - `focus-check.html?lang=en-US`: 215/215 passed, 0 failures, 0 console errors.
   - `focus-check.html?lang=pt-BR`: 222/222 passed, 0 failures, 0 console errors.
   - `nav-test.html`: 209/209 passed, 0 failures, 0 console errors.
3. **Rust Workspace Suite:**
   - 34 test blocks, 578 tests passed, 0 failed, 5 ignored (live network tests).
4. **Targeted CTest Suite (Both Targets):**
   - **v200 Candidate (`slice335-rc7-controls-build`):** 5/5 passed.
     - `Test #10: native_audio_input`: Passed (3.43s)
     - `Test #11: native_controls`: Passed (0.11s)
     - `Test #14: native_achievements`: Passed (0.04s)
     - `Test #15: native_achievements_spec2`: Passed (0.03s)
     - `Test #16: native_achievements_http`: Passed (0.10s)
   - **Spec II Candidate (`slice336-rc7-controls-spec2`):** 5/5 passed.
     - `Test #10: native_audio_input`: Passed (3.45s)
     - `Test #11: native_controls`: Passed (0.08s)
     - `Test #14: native_achievements`: Passed (0.04s)
     - `Test #15: native_achievements_spec2`: Passed (0.03s)
     - `Test #16: native_achievements_http`: Passed (0.06s)

### Accounting of Known Baseline Failures:
The 13 known baseline failures in the full Python CTest suite from rc6 remain documented as pre-existing GS settings/live window test expectations. They are not regressions.

---

## 7. Owner In-Game Verification Checklist

Hardware feel, tactile response, and live network server unlocks cannot be verified via automated mocks and remain for owner verification:

- [ ] **Dual-Game Home Screen:**
  - Launch `GT4Recomp.exe`. Verify choice between Gran Turismo 4 (v2.00) and Spec II (1.10).
  - Verify separate saves, separate disc preparation, and independent boot for each title.
- [ ] **RetroAchievements (Gran Turismo 4 v2.00 Only):**
  - Navigate to **Settings > Achievements** (or **Configurações > Conquistas**).
  - Log in with your RetroAchievements username and password. Confirm session status displays score and username.
  - Launch Gran Turismo 4 v2.00: verify rich presence updates and achievement triggers fire on real hardware.
  - Launch Spec II: verify RetroAchievements is cleanly deactivated (unsupported for mod image).
- [ ] **Controller Response Curves:**
  - Navigate to **Settings > Controls**.
  - Review the 7 curves (Linear, Quadratic, Cubic, S-Curve, Smooth Power, Sublinear, Logarithmic) on the visualizer.
  - Test driving feel on physical gamepad (DualSense, DualShock 4, or Xbox pad) to verify comfortable steering sensitivity and progressive throttle/brake travel.
- [ ] **Saves & Settings:**
  - Verify active save rename, export, and game-specific tags.

---

## 8. Git State & Review Hand-Off

All test kit packages and inspection logs remain local under `O:\gt4recomp-scratch\task133-rc7-kit-v2\`. Only authorized documentation, release notes, and input manifests are staged for commit. Work stops here for Codex review.
