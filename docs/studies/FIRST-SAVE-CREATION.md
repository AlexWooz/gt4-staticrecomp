> **Study by Luan Antony**, from pull request #167 (issue #160), imported on 2026-11-04 at the owner's word.
> Task numbers inside are the author's own numbering (TASK-146); in our TASKS.md this study belongs to TASK-164.
> It is a study, not a decision: anything it proposes is checked again before it is built.

# TASK-146: Memory Card Save Creation and First-Boot Halt Diagnosis (Issue #160)

## 1. Executive Summary & Context

- **Issue:** [#160 — Problem with save game - Memory card](https://github.com/moesuito/gt4-staticrecomp/issues/160)
- **Reporter:** *rinegan33*
- **Reported Symptom:** The game halts on the initial memory card boot screen:
  `No Gran Turismo 4 data found on MEMORY CARD slot 1. Do you want to create game data? [Yes] [No]`
  with the reporter stating: *"I can't start the game I need help with this problem."*
- **Scope & Purpose:** Diagnose the root causes preventing first-time and keyboard/controller players from progressing past the initial save data creation prompt, analyze the SIO2 memory card initialization lifecycle (`runtime\card.ps2`, `gt4saves`, `memcard_store.cpp`), evaluate input binding friction on first boot, and specify an architectural remediation path to prevent player lockouts.

---

## 2. Gran Turismo 4 Boot & Memory Card Protocol

### 2.1 Boot Detection Sequence
During initial boot of Gran Turismo 4 (SCUS-97328), the EE engine executes its initialization sequence:
1. Queries PS2 `mcman` / `sio2` for Memory Card Slot 1 (`mc0:`).
2. Inspects the root directory of Slot 1 for the existence of the game save directory:
   `BASCUS-97328GAMEDATA`
3. **If present:** Reads `icon.sys`, `savedata`, and profile flags; proceeds straight to the main menu (Arcade Mode / Gran Turismo Mode), as documented in `slice110-external-menu-pilot.md` (`card-reread5` / `menu-repeat6`).
4. **If absent (fresh install or blank card):** The game halts execution of the main sequence and displays a modal dialog:
   ```text
   No Gran Turismo 4 data found on MEMORY CARD slot 1.
   Do you want to create game data?
      Yes    No
   ```

### 2.2 In-Game Interactive Wizard Progression
Historical analysis from the slice 110 menu pilot (`docs/reverse-engineering/slice110-external-menu-pilot.md`) provides precise empirical evidence of the memory card creation wizard:
- **Frame 1600 (`name-control2`):** Game prompts to create save data. **`Start` (Enter) does not dismiss or confirm this dialog.** The dialog exclusively responds to `Cross` to select "Yes", `Circle` to select "No", and D-Pad Left/Right.
- **Frame 1800 (`name-control3`):** Game reports that 1,488 KB of free memory card space is required, requiring confirmation with `Cross`.
- **Frame 2200 (`name-control3`):** Game prompts to format or write initial card data. **Crucially, `Cancel` is selected by default on this screen.** A player who simply mashes confirm without pressing D-Pad Right cancels creation and loops back.
- **Frames 2400–3300 (`name-control4`):** Multiple sequential screens display formatting progress, autosave notices, verification results, and memory card protection rules, each demanding a distinct `Cross` press.

---

## 3. Root Cause Analysis

Three distinct factors contribute to players getting stuck on this screen:

### Factor A: Input Binding Friction & Keyboard Expectations (Primary Cause)
1. **PC Convention vs. PS2 Binding:**
   - PC players naturally press `Enter`, `Space`, `E`, `Esc`, or mouse buttons when presented with a "Yes / No" modal prompt.
   - In GT4Recomp's default keyboard map:
     - `Z` = Cross (Confirm / Accelerate)
     - `X` = Circle (Cancel / Reverse)
     - `A` = Square (Brake)
     - `S` = Triangle
     - `Enter` = Start
     - `Arrows` = D-Pad
   - When a player presses `Enter`, the game receives `Start`. Gran Turismo 4's memory card creation dialog completely ignores `Start`.
   - When a player presses `A` (expecting standard "A to confirm" on Xbox layouts), the game receives `Square`, which is also ignored.
   - When a player presses `Space`, nothing happens (unbound).
2. **Perceived System Freeze:**
   - Because none of the typical PC confirmation keys trigger any visual feedback or sound effect, players conclude the emulator/recompilation is completely frozen or stuck on the memory card screen.

### Factor B: Blank Card Generation by `gt4saves` (Secondary Cause)
1. In `launcher/crates/gt4l-tauri/src/saves.rs` (`prepare_card`):
   ```rust
   let image = library.build_card(&replay_names)...
   ```
2. When the player launches the game for the first time, `library.active_save()` is `None`.
3. In `launcher/crates/gt4saves/src/library.rs`:
   ```rust
   let mut items = Vec::new();
   if self.active_save()?.is_some() {
       items.push(folder::import_item(&self.active_dir().join(GAME_SAVE_NAME))?);
   }
   // ...
   let card = Card::build(&items)?;
   ```
4. `Card::build(&[])` produces a valid, formatted 8 MB memory card image containing a root directory with only `.` and `..`, but **no `BASCUS-97328GAMEDATA` directory**.
5. Consequently, 100% of first-time players are forced into the 5-step in-game creation wizard, exposing them directly to the input confusion described in Factor A.

### Factor C: Card Locking, Antivirus, and Write Failures (Tertiary Cause)
1. **Exclusive Lock Collisions (`native/window.cpp`):**
   - Before graphics initialization, `open_window` opens `runtime\card.ps2.lock` via `CreateFileA` without sharing flags (`GENERIC_READ | GENERIC_WRITE, 0`).
   - If an orphaned `gt4vulkan.exe` process is running in the background, or if antivirus software scans the `.lock` file, `GetLastError()` returns `32` (`ERROR_SHARING_VIOLATION`), throwing `Cannot exclusively lock game memory card: 32` and aborting startup.
2. **Atomic Write & Temp File Collisions (`native/memcard_store.cpp`):**
   - The memory card writer outputs to `<path>.tmp` and calls `std::filesystem::rename` (which invokes `MoveFileExW(REPLACE_EXISTING)` on Windows).
   - If the player's `%LOCALAPPDATA%` or install directory has restricted permissions or cloud synchronization (e.g., OneDrive locking `.tmp` files), the rename fails, leaving the card unwritable and causing in-game save creation to fail silently or prompt repeatedly.

---

## 4. Remediation Architecture & Design

### Phase 1: First-Boot Input Guidance (UI & Onboarding)
- **Launcher First-Boot Tip:** In the launcher main screen or setup modal, display an explicit banner for keyboard users:
  > *"First time playing on keyboard? Press **Z** to Confirm (Cross), **X** to Cancel (Circle), **Arrow Keys** for Navigation, and **Enter** for Start. Controllers can be configured in Settings > Controls."*
- **Splash / Window Title Reminder:** On first boot (when no active save is found), log or overlay the confirmation controls to prevent players from assuming the game has halted.

### Phase 2: Save Auto-Initialization in Launcher (`gt4saves`)
- Rather than shipping an empty card image `Card::build(&[])` when `active_save()` is absent:
  - Provide a pre-seeded minimal valid `BASCUS-97328GAMEDATA` directory structure (containing standard `icon.sys`, placeholder icon, and fresh game data template).
  - Alternatively, provide an automated "Initialize Save Data" button in the Launcher's **Saves** tab that provisions the default save slot before launching the game.
  - When the game boots with a pre-seeded save, it loads immediately into the Main Menu, skipping the memory card wizard entirely.

### Phase 3: Robust Lock Recovery & Error Diagnostics
- **Orphan Lock Cleanup:** Before failing on `CreateFileA` with `ERROR_SHARING_VIOLATION`, probe whether the process holding the lock is still active. If it is an orphaned lock from an unclean termination, offer a recovery path.
- **Actionable UI Errors:** If `prepare_card` or SIO2 atomic write fails due to permissions or disk space, surface a clear, translated error dialog in the launcher instead of silent failures.

---

## 5. Verification & Acceptance Plan

1. **Automated Unit Testing:**
   - Verify `Card::build(&[])` continues to construct valid raw images without regressions.
   - Verify `Library::open_paths` and `Library::build_card` handle missing active saves gracefully.
   - Validate that `gt4l-core` launch contract tests pass without regression.
2. **Empirical Input Verification:**
   - Documented comparison against slice 110 keypress timings (`name-control4` vs `card-reread5`).
