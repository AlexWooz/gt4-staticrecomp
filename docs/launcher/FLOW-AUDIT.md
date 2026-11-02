# Launcher flow audit (TASK-019)

2026-10-20, branch `task/019-launcher`. Scope: the whole launcher flow before the installer build, end to end,
with the owner's real discs (read-only) and the real gt4-gs release in a sandbox data folder. The game itself
(`gt4vulkan.exe`) was never started: Play was checked up to its launch plan.

Agent status: not PCSX2-tainted (no PCSX2 sources or fork were opened; the gt4-gs release zip was listed, not run).

## 1. Flow map

```
GT4Recomp.exe start
  single instance? (no: bring the first window forward, exit)
  data folder = GT4RECOMP_DATA_DIR or %LOCALAPPDATA%\GT4Recomp
  AppState::load: settings.toml (migrations), controls.toml, save library, pins
  background thread check_on_launch: version.json; gt4-gs retry install (only when set up and selected); update check
  page boots -> app_state
      |
      +-- setup.state != done --> #/setup
      |      needed ......... "Choose disc image"
      |      wrong_disc ..... reason + "Choose another file"
      |      failed ......... reason + "Try again" / "Choose another file"
      |      files_missing .. "Game files missing" + "Choose disc image"
      |      checking ....... copying -> checking -> preparing -> renderer when selected (bars, bytes, Cancel)
      |      done ........... "Ready" + Finish (opens Home; note when GSdx could not be downloaded, TASK-096)
      |
      +-- setup.state == done --> #/home (slideshow, logo, Play, gear)
             Play -> prepare_play -> game::start -> tray hides the window
             game ends -> card harvested -> window back on Home with any message
             Settings: Graphics (Display, Graphics, Advanced sections), Controls, Saves, Replays, About
```

### Screens, commands and events

| Screen | Commands it calls | Events it hears |
|---|---|---|
| Boot (`app.js`) | `app_state` | `update-available`, `close-confirm`, `update-finished`, `renderer-progress` (toasts outside setup), `game-state` |
| Setup | `app_state` (polled every 400 ms), `setup_choose_disc`, `setup_start`, `setup_cancel` (Finish runs no command) | (polls; `setup-progress`, `setup-finished` and `renderer-progress` are also emitted) |
| Home | `app_state` (game_running), `about_info` (version label), `play`, `stop_game` | `game-state` |
| Settings: Graphics (Display, Graphics, Advanced sections) | `get_settings_table`, `set_setting`, `reset_gs_settings_to_defaults` | |
| Settings: Controls | `get_controls`, `capture_binding`, `cancel_capture`, `reset_binding`, `set_setting` (triggers) | `gamepad` (optional) |
| Settings: Saves, Replays | `saves_list`, `saves_import`, `saves_activate`, `saves_delete`, `replays_list`, `replays_import`, `replays_delete` | |
| Settings: About | `about_info`, `check_updates`, `open_game_files_folder`, `open_log_folder` | |
| Update popup | `apply_update`, `skip_update` | `update-available`, `renderer-progress`, `update-finished` |
| Close confirmation | `confirm_close` | `close-confirm` |

`src/contract.rs` now checks both directions: every name a page calls is registered in `main.rs`, and every
registered command is called by a page (27 commands).

### Files written (all under the data folder unless noted)

| File | Written by | When |
|---|---|---|
| `disc\SCUS-97328.iso` | setup of a launcher before decision 0075 | once; an install from then reads this copy. Setup now writes none and records where the image is |
| `prepared\CORE.GT4`, `prepared\SCUS_973.28`, `prepared\prepare.json` (`prepared.tmp` while staging) | setup | once; the receipt is the commit point |
| `gs\<version>\...`, `gs\current.txt` (`gs\.staging-<v>` while unpacking) | setup, launch retry, update | first install and updates |
| `version.json` | launcher | next to `GT4Recomp.exe` (or the data folder when that is read-only) |
| `config\settings.toml` (+ `.bak-schema<N>-<t>`) | settings, setup (`disc_path`, skipped updates) | on change |
| `controls.toml` | controls | on change |
| `Documents\Gran Turismo 4\Saves\...`, `...\Replays\...` (the player's folder, not the data folder) | saves | import, activate, delete, game end (harvest), first start (migration) |
| `runtime\card.ps2`, `library\backups\` | saves | Play (built from the library), game end |
| `config\library-migration.json`, `library.migrated-<t>\` | saves | first start after an older launcher (TASK-081) |
| `sessions\session-<t>\` (`game.log`, `misses.txt`, `gt4-gs.ini`) | Play | per session, 20 kept |
| `logs\launcher.log` | everything | appended |

Protected by the data-path guard (installs and updates refuse to write there): `saves`, `cards`, `library`,
`replays`, `runtime` and now `disc`, plus card and save extensions, and (TASK-081) the player's folder
`Documents\Gran Turismo 4` wherever Documents is (registered at start, and by name: `Saves` or `Replays` inside a
`Gran Turismo 4` folder). The uninstaller never touches the player's folder; it removes the AppData data folder
only when the player ticks its unchecked "also delete" box.

## 2. Disc rule (what is accepted)

**Rule:** the game code decides. A disc is accepted when

1. it is a plain 2048-byte-sector `.iso` that can be opened;
2. its `SYSTEM.CNF` boots `cdrom0:\SCUS_973.28;1` (serial SCUS-97328);
3. its `CORE.GT4` is byte-identical to the pinned one (size 2,020,861, SHA-256 `85d26aa8...2ff9`), so the rebuilt ELF
   is exactly the code the game executable was compiled from (the rebuild is verified again against the ELF pin,
   SHA-256 `10f82e22...935c`);
4. the image is not shorter than its own volume size;
5. **if** it claims the qualified version (`VER = 2.00`), its size and full SHA-256 must also equal the v2.00 disc
   pin (5,314,478,080 bytes, `67b6c007...824f`). A v2.00 image with any other byte is a damaged or patched dump and is
   refused.

Another revision whose `VER` is not 2.00 but whose `CORE.GT4` is identical passes; its receipt records
`qualified_dump: false`. A SCUS-97328 disc with different game code is refused as "not supported yet".

**What the check compares (before this audit):** the extension, `SYSTEM.CNF` serial and boot path, `VER == 2.00`,
the exact disc size, then the whole-image SHA-256, then CORE and ELF. Any other revision was refused by `VER`.

**Why it is safe:** the executable is compiled from the EE code that `CORE.GT4` holds; the launcher rebuilds the ELF
from the disc's own CORE and checks it against the pin, so a passing disc runs exactly the code we ship. The rest of
the disc (GT4.VOL, IRX modules, IOPRP) is data the same code shipped with on that revision; the IRX modules are
interpreted, not compiled.

**Evidence:**

- Confirmed: the owner's `Gran Turismo 4 (USA) (v2.00).iso` passes (`SYSTEM.CNF` `VER = 2.00`, volume `GRANTURISMO4`,
  CORE SHA-256 equal to the pin, full image hash equal to the pin; real run below).
- Confirmed: gt4-decomp labels its target "SCUS-97328 v1.01" and publishes the same `CORE.GT4` SHA-1
  `30031137ce49...37d2` as ours (`docs/DECOMP-IDENTITY.md`). So a v1.01 disc with identical game code exists.
  High confidence that such a disc passes this rule; Unknown until a real v1.01 image is tried (none is on this
  machine).
- Confirmed: `Gran Turismo 4 Spec II v1.10.iso` boots `SCUS_974.36` (GT4 Online's serial), `VER = 1.00`, volume
  `GRANTURISMO4`, and has no `CORE.GT4`. It is refused with "This disc is not supported yet. GT4Recomp supports Gran
  Turismo 4 (USA, SCUS-97328). This looks like Gran Turismo 4 Spec II or GT4 Online (SCUS-97436)." (The old check
  looked for "SPEC" in the volume name, which Spec II does not have, so it said "This is not Gran Turismo 4 USA".)
- Note: the v2.00 image is dual layer; its primary volume descriptor covers layer 0 only (2,697,330,688 bytes). The
  volume-size check is therefore a lower bound, and the exact size check applies to the qualified version only.

Messages (all refusals start with the same sentence when the disc is a PS2 game but not ours):

| Case | Message |
|---|---|
| Spec II / GT4 Online | This disc is not supported yet. GT4Recomp supports Gran Turismo 4 (USA, SCUS-97328). This looks like Gran Turismo 4 Spec II or GT4 Online (SCUS-97436). |
| PAL / Asian | ... This disc is SCES-50001, another region. |
| SCUS-97328 with other game code | ... This SCUS-97328 disc (version X) has different game code. |
| Another game | ... This disc is SCUS-97100. |
| Not an ISO | This file is not a plain 2048-byte-sector .iso image. |
| .bin / .chd | Please use a plain .iso file ... / This image is compressed (.chd) ... |
| Missing file | That file could not be opened. Check that it is still there and try again. |
| Truncated | This copy is incomplete or modified. Please re-dump your disc. |
| v2.00 with a changed byte | This dump differs from the known good v2.00 disc (damaged or patched). Please re-dump your disc. |
| Low space | Not enough free space to copy the game files: X GB needed, Y GB free. Free up space and try again. |

## 3. The copy of the disc image (owner, 2026-10-20)

The game reads the disc at run time (`--disc <iso>`; GT4.VOL streaming), so setup now copies the image into
`<data>\disc\SCUS-97328.iso`. The copy is always an independent file (never a hard link). Free space is checked
first (image size plus 512 MiB); the copy goes to `SCUS-97328.iso.tmp` and is hashed while it is written (that pass is
the full check); on a match it is renamed into place, then the prepared folder is swapped in with a schema 2 receipt
that records the copy's size, modified time and SHA-256. Cancel or any failure removes the temporary copy and the
staging folder. Every later launch compares only size and modified time (no hash): the second launch took 1-2 ms.
If the copy is missing or changed, the launcher opens Setup with "Game files missing. Choose your disc image again."
and never starts setup by itself. About shows the folder with an "Open folder" button.

## 4. Checklist

PASS = worked as specified when tested. FIXED = a bug found in this audit, fixed and tested. FAIL = still broken.
NEEDS-OWNER = needs a person (real pad, touch screen, Play, native file picker).

| # | Step | Result | Evidence |
|---|---|---|---|
| 1 | First launch opens Setup, not Home | PASS | real exe with an empty sandbox: `audit-tauri-first-launch-setup.png`; `synthetic_flow_end_to_end` |
| 2 | Setup cannot be skipped (no Home until done) | PASS | `app_state` says `done` only when the receipt checks; Home is routed only on `done` |
| 3 | Setup page offers "Choose disc image" | FIXED | before: the page called `setup_start` at once with no disc chosen (error toast) and had no button to choose a disc in the first state; the backend never reported `wrong_disc` |
| 4 | Not an ISO refused with a clear message | PASS | real run: "This file is not a plain 2048-byte-sector .iso image." |
| 5 | Missing file refused with a clear message | FIXED | before: "Preparing the game files failed ... report this with the log"; now "That file could not be opened ..." |
| 6 | Truncated copy refused | PASS | real 64 MiB truncation: "This copy is incomplete or modified." |
| 7 | Spec II refused with "not supported yet" | FIXED | before: "This is not Gran Turismo 4 USA (SCUS-97328). The disc says SCUS-97436." (volume name has no "SPEC") |
| 8 | v2.00 accepted | PASS | real run, `qualified_dump: true` |
| 9 | Disc rule: game code decides (other revision with identical CORE passes; other code refused) | FIXED | before: any `VER` other than 2.00 refused. Tests `another_revision_with_the_same_game_code_is_accepted`, `..._different_game_code_is_refused` |
| 10 | Damaged v2.00 refused (CORE byte; other byte after the copy) | PASS | `truncated_copy_..._changed_core_...`, `a_hash_mismatch_after_the_copy_leaves_nothing_behind` |
| 11 | Image copied into the data folder, independent file | FIXED (new) | real copy 5,314,478,080 bytes in about 35-75 s; `the_copy_is_an_independent_file_not_a_link` |
| 12 | Free space checked before the copy | FIXED (new) | `low_free_space_is_refused_before_copying`; `gt4l-platform::disk::free_space` (GetDiskFreeSpaceExW) |
| 13 | Cancel leaves nothing half-written | PASS | `cancel_leaves_nothing_half_written`; flow test cancel |
| 14 | Setup progress drawn (copy bytes, percent) | FIXED | before: `app_state.progress` was always 0, the page never listened to `setup-progress`, and the `.bar` was an inline span with no height (never visible). Screenshots `audit-setup-copying.png` |
| 15 | Renderer download drawn on the setup page | FIXED | known gap: now a setup step with listing / downloading (bytes) / installing; byte progress via `ReleaseSource::download_reporting`. `audit-setup-renderer.png` |
| 16 | Real gt4-gs 1.0.1 installs | FIXED | before: "the package has no gt4-gs.exe" (the real zip wraps everything in `gt4-gs-1.0.1-windows-x64/`). Now the wrapping folder is removed on unpack; test `a_package_wrapped_in_one_folder_installs_like_the_real_release`. Real run: `gs\current.txt` = 1.0.1, `gs\1.0.1\gt4-gs.exe` + DLLs + `resources\` |
| 17 | SHA256SUMS verified | PASS | the package line is checked (the real SHA256SUMS has no `gt4-gs.exe` line; the exe hash is recorded at install) |
| 18 | version.json records gt4-gs | PASS | `{"schema":1,"gt4recomp":"1.0.0","gt4gs":"1.0.1","settings_schema":1}` next to the exe |
| 19 | Renderer install at launch did not race setup | FIXED | before: the launch thread installed gt4-gs before setup, unseen, and `setup_start` started a second install in parallel; now setup owns the first install and launch only retries after an offline setup |
| 20 | Offline renderer: setup still finishes, GSdx note; Play refuses until GSdx is installed (TASK-096) | PASS | flow test `offline_renderer`; `audit-setup-done-offline-960x600.png` |
| 21 | Setup ends on "Ready" with Play | PASS | real run `state: done` |
| 22 | Setup screen not replaced by Home before the renderer step ends | FIXED | before: `app_state` said `done` as soon as the prepared folder existed, while the renderer still downloaded |
| 23 | Later launch opens Home directly, no re-verification | PASS | real exe second launch: `audit-tauri-second-launch-home.png`; `app_state` done in 1-2 ms |
| 24 | Picked disc moved or deleted after setup: still Home | FIXED (new) | flow test deletes the picked file; Play uses `disc\SCUS-97328.iso` |
| 25 | Copy missing later: clear way back | FIXED (new) | real exe: `audit-tauri-files-missing.png`; Play refuses with "Game files missing" and Home routes to Setup |
| 26 | Old schema 1 receipt | PASS | treated as missing game files, one copy; `docs/launcher/MIGRATIONS.md` |
| 27 | Settings table, set, read back, reset | PASS | real run: 35 rows, fxaa round trip, reset |
| 28 | Unknown setting refused | PASS | flow test |
| 29 | Saves: import a test card, no duplicate on re-import | PASS | real run: 0 -> 1 ("Added Save 01."), second import keeps 1 |
| 30 | Replays list | PASS | real run: 0 items |
| 31 | Controls list and reset | PASS | real run: 16 inputs, cross reset |
| 32 | About: versions, game files folder and "Open folder" | FIXED (new) | `audit-settings-about.png`; the stale "Disc image" path row was removed |
| 33 | check_updates | PASS | real run: 0 offers, no errors, gt4-gs 1.0.1 installed (GT4Recomp has no release yet) |
| 34 | Play command line and environment | PASS | plan: `<data>\prepared\SCUS_973.28 --ntsc --disc <data>\disc\SCUS-97328.iso --frames 0 --memcard <data>\runtime\card.ps2 --misses <session>\misses.txt --window`; `GT4_RENDERER=pcsx2`, `GT4_GS_PROCESS`, `GT4_GS_PROCESS_SHA256`, `GT4_GS_CONFIG`, inherited `GT4_*` removed |
| 35 | UI command contract (both directions) | FIXED | new test; it found `open_game_files_folder` unused until About called it |
| 36 | nav-test | PASS | 55 of 55 in headless Edge |
| 37 | CSP: no inline style or script | PASS | progress widths use CSSOM (`el.style.width`), allowed by `style-src 'self'` |
| 38 | 48 px targets | FIXED | the Home "Stop" button was 40 px high; now 48 px |
| 39 | Setup fits 960 x 600 without scrolling to its buttons | FIXED | a 960 x 600 rule hides the duplicate wordmark and tightens rows; problem states show only the Disc row |
| 40 | Real Tauri exe builds and closes normally | PASS | `cargo tauri build --no-bundle`; WM_CLOSE, exit code 0, three starts |
| 41 | Tray hide on Play, restore on exit | NEEDS-OWNER | state machine tests pass (8); live check needs Play |
| 42 | Update popup live (Update / Not now / Skip) | NEEDS-OWNER | mock screenshot only; no newer release exists |
| 43 | Native file picker | NEEDS-OWNER | the harness injects the path into the same function |
| 44 | Real pad (A select, B back), L2/R2, touch | NEEDS-OWNER | |
| 45 | Play (the game itself) | NEEDS-OWNER | never started here |

Counts: PASS 23, FIXED 17 (5 of them new behaviour asked for during the audit), FAIL 0, NEEDS-OWNER 5.

## 5. How it was tested

- `cargo test --workspace` (186 passed, 3 ignored).
- `gt4l-tauri/src/flow_audit.rs::synthetic_flow_end_to_end` (every run): the command bodies in the page's order with a
  synthetic disc: refusals, accept, setup with recorded events, second launch, settings, saves, controls, about, the
  Play plan, the copy deleted, an offline renderer, cancel.
- `real_flow_end_to_end` (ignored; run once with the real discs, a real 5.3 GB copy and the real gt4-gs download in
  `%TEMP%\...\scratchpad\launcher-audit\data-real`). The command lines are in the module comment.
- `gt4l-disc/tests/real_disc.rs::real_discs_pass_or_fail_the_precheck` (ignored): v2.00 and Spec II.
- The real launcher: `cargo tauri build --no-bundle`, started three times with `GT4RECOMP_DATA_DIR` set to a sandbox
  folder (empty: Setup; after setup: Home; copy renamed away: Game files missing), window captured with PrintWindow,
  closed with WM_CLOSE.
- The dev mock in headless Edge with `--force-prefers-reduced-motion` for every setup state and the settings pages.

Screenshots (`docs/launcher/shots/`): `audit-tauri-first-launch-setup.png`, `audit-tauri-second-launch-home.png`,
`audit-tauri-files-missing.png`, `audit-setup-needed.png`, `audit-setup-not-supported.png`, `audit-setup-copying.png`,
`audit-setup-copying-960x600.png`, `audit-setup-preparing.png`, `audit-setup-renderer.png`,
`audit-setup-failed-space.png`, `audit-setup-files-missing.png`, `audit-setup-done-offline-960x600.png`,
`audit-setup-needed-844x390.png`, `audit-home.png`, `audit-settings-graphics.png`, `audit-settings-about.png`,
`audit-settings-saves.png`, `audit-settings-controls.png`.

## 6. Limits and open items

- No real v1.01 (or other revision) image was available, so rule 3's acceptance of another revision is tested with
  synthetic images only.
- The full hash of the copy is the hash of the bytes written; the copy is not read back a second time.
- At 844 x 390 (phone landscape) the setup steps scroll; the A hint and the scroll reach the button.
- The installer was not built (NSIS is not installed); its uninstall text now names the disc copy.
- The native file picker, tray, update popup, pad, touch and Play need the owner.

## 7. Owner test findings (2026-10-21) and the fixes

The owner ran the real `GT4Recomp.exe` (setup, Play and the return to Home worked). Five findings, all fixed on
`task/019-launcher`. The 2026-10-20 audit above did not catch them, for the reason in 7.6.

### 7.1 Settings did not load: "Command get_settings_table not allowed by ACL" (critical, Confirmed)

- Root cause: Tauri 2 allows an app command only when its `allow-<name>` permission is in `capabilities/default.json`,
  and the permissions come from the `COMMANDS` list in `build.rs`. Both still had the names of an older UI
  (`settings_rows`, `settings_set`, `controls_list`, ...). Every command the current UI calls was denied in the real app.
- Mismatch (old list vs the registered and called names):
  - in the old list only (removed): `settings_rows`, `settings_set`, `controls_list`, `controls_capture_start`,
    `controls_capture_cancel`, `controls_bind_key`, `controls_reset`, `controls_set_trigger`;
  - registered and called, missing from the old list (denied): `get_settings_table`, `set_setting`,
    `reset_gs_settings_to_defaults`, `get_controls`, `capture_binding`, `cancel_capture`, `reset_binding`,
    `open_game_files_folder`, `check_updates`, `apply_update`, `skip_update`, `confirm_close`.
- Fix: one source of truth, `launcher/crates/gt4l-tauri/commands.list` (27 names). `build.rs` reads it for the
  permissions; `capabilities/default.json` lists exactly the 27 `allow-*` entries.
- Test: `src/contract.rs` (6 tests) fails on any mismatch between `commands.list`, `generate_handler!` in `main.rs`,
  the `allow-*` permissions, the commands `ui/js` calls, and the events `ui/js` listens to (each must be emitted by Rust
  through `emit(` or `notify(`). The `gamepad` event that `nav.js` listened to was never emitted by Rust, so that
  listener was removed. A negative check (a fake name added to `commands.list`) failed the three set tests as intended.
- Real-app check (2026-10-21): `GT4Recomp.exe` built with `cargo tauri build --no-bundle`, started with
  `GT4RECOMP_DATA_DIR` set to a scratch folder and WebView2 remote debugging on port 9223. The probe script called 15
  commands from the real webview: 0 denied by ACL (`app_state`, `get_settings_table`, `set_setting`,
  `reset_gs_settings_to_defaults`, `get_controls`, `cancel_capture`, `reset_binding`, `about_info`, `saves_list`,
  `saves_activate`, `saves_delete`, `replays_list`, `replays_delete`, `check_updates`, `skip_update`). The probes used
  values the app refuses, or read-only calls. Not called on purpose: `play`, `stop_game`, `setup_*` (would start the
  copy), `capture_binding`, `confirm_close`, `apply_update`, `open_game_files_folder`, `open_log_folder`,
  `saves_import`, `replays_import`. Their permissions come from the same generated list, which the test checks.

### 7.2 Setup offered only Play (Bug 2)

- The setup page's done state now shows **Finish**, which goes to Home (`#/home`). It is the primary button: focused on
  the pad, A activates it, and touch and mouse click it. It starts no game. A "done offline" setup (GSdx missing; Play refuses, TASK-096)
  shows the same Finish. A later launch with setup done still opens Home directly (`app.js`).
- Check: the dev mock with `?setup=done` renders one primary `Finish` button, focused.

### 7.3 The game started in a window (Confirmed in code)

- Cause: `gt4l-core/src/launch.rs` always passes `--window` (the native window needs it). `GT4_FULLSCREEN` was sent
  (1 for "on"), but the native code did not read it: `open_window(true)` threw "Fullscreen is not qualified in this
  Vulkan pilot", and nothing set fullscreen at start.
- Native change (`native/window.cpp`, `native/window_mode.{hpp,cpp}`): `GT4_FULLSCREEN` is parsed strictly (unset, empty
  or `0` = windowed; `1` = fullscreen; any other value stops the start with a message). Fullscreen at start uses the same
  borderless path as Alt+Enter and F11 (`set_fullscreen`). Alt+Enter and F11 are unchanged. The donor `--fullscreen`
  argument reaches the same path.
- Launcher: the default stays **on** (Display, "Fullscreen"). The game and the PCSX2 GS child both get `GT4_FULLSCREEN`
  (`game_environment`, `pcsx2_environment`). Test: `launch_contract.rs`,
  `both_window_modes_pass_window_and_send_gt4_fullscreen_to_the_game_and_the_gs_child`.
- Migration (schema 1 to 2, see `docs/launcher/MIGRATIONS.md`): a schema 1 file with `fullscreen = "off"` that the user
  never chose becomes "on", after a backup. The `[choices]` table records the settings the user set on the Settings page.
  Tests: `migrate.rs` and `tests/config_migration.rs`.
- **Owner's data.** `%LOCALAPPDATA%\GT4Recomp\config\settings.toml` already says `fullscreen = "on"` (read only; not
  changed). The migration adds only the schema number and a backup for that file. The windowed start came from the
  native code and `--window`, not from the saved value.
- **Not built here.** The native change is not compiled in this session: the VS developer environment and clang-cl
  exception handling failed in this sandbox. The fixture `quickfix_fixture` has the new test
  (`test_start_mode_environment`). The game executable must be built and checked on the owner's machine before the
  fullscreen start is confirmed in the built game (Unknown until then).

### 7.4 Settings focus: two levels, console-style highlights (owner request)

- Opening Settings focuses the tab list on the current tab. The content previews the highlighted tab (up and down).
  A, or D-pad right, enters the tab: the first row, or the row last used in that tab (remembered for the session).
  In the content: up and down move between rows; left and right change a slider or dropdown; A activates; **B returns
  to the tab list on the same tab**. B on the tab list goes Home. LB and RB change tabs and keep the level.
- Logic: `ui/js/focus-levels.js` (pure). `ui/dev/nav-test.js` has 22 new cases; `nav-test.html` gives 77 of 77 PASS in
  headless Edge. The page: `ui/js/pages/settings.js` (a tab switch re-renders only the content, so focus does not reset;
  the hash follows the shown tab). `nav.js`: `GT4.focusScope` narrows navigation to the level, `GT4.hooks.horizontal`
  takes D-pad right on a tab, and `GT4.inputMode()` tells a pad A from a mouse click.
- Visuals (`css/app.css`): the focus ring is a soft glow instead of a 2 px outline; a sidebar pill glides between tabs
  (160 ms, instant with reduced motion); a focused row gets a lighter band, a thin accent edge and a brighter label; the
  tab shown while focus is in the content keeps a quiet accent edge. The same ring applies on every page (Home, dialogs,
  Saves, Replays, Controls).
- Footers: tab list A Enter, B Back, LB RB Tabs; content A Select, B Tabs, then the tab's X Save and Y.
- Screenshots (headless Edge, `--force-prefers-reduced-motion`, dev mock): `docs/launcher/shots/pad-sidebar-focus.png`,
  `pad-content-row-focus.png`, `pad-slider-row-focus.png`, `pad-dropdown-open.png`. Dev-only `?focus=` values: side,
  content, slider, dropdown.
- **Not verified.** The pad itself (no controller in this sandbox). The owner checks the controller items in
  `ui/dev/MANUAL-CHECKLIST.md`.

### 7.5 Other changes

- `ui/js/nav.js`: the unused `gamepad` Rust-event listener was removed (see 7.1).
- `commands.list` is the only list of commands; a new command is added there first.

### 7.6 Why the 2026-10-20 audit missed 7.1

- The audit drove the command bodies (`flow_audit.rs`, plain Rust functions) and the dev mock in headless Edge. Neither
  goes through Tauri's IPC, so the ACL never ran. The mock answers every command name, including the old ones, and the
  contract test compared `generate_handler!` with the UI's names only, not with the permissions the real app enforces.
  The new test checks the permissions too, and 7.1 was checked in the real webview.

### 7.7 Owner test of the real exe, round 2 (2026-10-21): focus, values, Save

Four findings from the owner's test (Settings with the real `GT4Recomp.exe`). Checked with the dev mock in headless
Edge (`ui/dev/focus-check.html`) on the HEAD code and on the fix. The pad itself is not tested here.

1. **The sidebar auto-enters Controls, Saves and Replays (Confirmed).** Cause: those pages called `focus()` after they
   drew their content (Controls: L1; Saves and Replays: a row or Import). The settings shell moves to the content level
   on any focus inside the rows, so drawing the preview entered the tab. HEAD run: focus `null` and "in rows" after
   ArrowDown reached Controls, Saves, Replays and About.
2. **Saves and Replays select "Import ..." on entry (Confirmed).** Two causes. The page focused Import when the list was
   empty, and the default entry (`focusRow`) takes the first focusable element of the tab, which is the Import button
   (it sits above the list). HEAD run: entering Saves focused "Import save".
3. **A changed value does not show on screen (Confirmed).** The dropdown's `choose()` wrote the label that was shown
   before the change, so Internal resolution kept "1080p (3x)" after 4K was picked. `set_setting` returns nothing, and
   the table was re-read only when another row's `visible_when` depended on the id. A row's `disabled` state also comes
   from other settings (`settings.is_active`), so the table must be read after every write. HEAD run: "1080p (3x)" after
   picking 4K; the table did hold 4K.
4. **Save removed (owner request).** Every change applies at once, so the X = Save action, the "Saved" toast and the
   Save hints are gone from Graphics, Display and Advanced. X stays Import (Saves, Replays) and Rebind (Controls).

Fixes (`launcher/crates/gt4l-tauri/ui`):

- `js/pages/settings.js`: a successful write updates the row model (`GT4.settingsModel.applyChange`), then the table is
  read again and the tab is drawn from it. Focus returns to the same row only while focus is still in the tab's rows.
  Only the newest read is drawn (`readGate`). The dropdown's `choose()` shows the chosen option's label. The tab's
  focus-in listener ignores focus from rendering the preview (`FL.onContentFocus(state, previewing)`). Pages may set an
  entry element (`host.entry`); a tab without one keeps the old default.
- `js/pages/library.js`: the list does not focus anything when drawn. A enters on the first row, or on Import when the
  list is empty (`libraryEntry`).
- `js/pages/controls.js`: no focus on load. A enters on L1. Trigger rows use the same newest-read rule.
- `js/settings-model.js` (new, pure): `findRow`, `rowVisible`, `applyChange`, `optionLabel`, `libraryEntry`, `readGate`.
- `js/focus-levels.js`: `onContentFocus`.
- Save removed: the X hint and `GT4.hooks.x` in the settings shell, Advanced's X hint, `SAVABLE_TABS`, the "Saved" toast.
- `dev/mock-data.js`: `set_setting` returns nothing, as the Rust command does.
- Docs: `dev/README.md` (navigation table without Save, `set_setting` row, file list), `dev/MANUAL-CHECKLIST.md`.

Tests:

- `dev/nav-test.html`: 95 of 95 PASS (was 77). New cases: a preview render does not move focus; entering Saves or
  Replays focuses the first row, or Import when empty; entering Controls is L1; the row model holds a dropdown, toggle
  and slider value after a change; a row with `visible_when` shows at once and hides again; the newest read wins.
- `dev/focus-check.html` (new, dev only): 59 of 59 PASS in headless Edge with arrow keys and clicks on the mock. It
  covers the sidebar highlight over every tab (down and up), each route opening with focus on its tab, entering and
  leaving every tab with B, the Saves and Replays entry, the absence of Save in the footers, and a 4K choice, a toggle
  and a slider showing their new values with the table holding the same values. On the HEAD code the same page gives
  41 of 59, with the symptoms of findings 1 to 3.
- `cargo test --workspace`: 196 passed, 0 failed, 3 ignored.
- Not covered: `visible_when` refresh through the mock (the mock has no such row); the pure tests cover the rule.

Screenshots (headless Edge, `--force-prefers-reduced-motion`, 1280 x 720, dev mock):

- `docs/launcher/shots/pad-sidebar-on-saves.png`: highlight on Saves, focus still in the tab list.
- `docs/launcher/shots/pad-saves-entered.png`: after A, the first save is focused.
- `docs/launcher/shots/graphics-4k-updated.png`: regenerated 2026-10-21 (TASK-080 addendum): Render scale at 200%, the readout shows the resulting internal resolution (there is no Internal resolution row any more).
- `docs/launcher/shots/graphics-sections.png`, `display-monitor-resolution.png`, `graphics-advanced-section.png`: regenerated 2026-10-21 (release 1.1.0): the Graphics tab with its Display, Graphics and Advanced sections; there is no Display tab.

Build: `cargo tauri build --no-bundle` (no Cargo.toml change). Owner test copy replaced:
`private/owner-test-launcher/GT4Recomp.exe`, SHA-256 `53a0ad0b36eeedcae865efe78c278bd867d3278e013f17a2df3b74c9c86450bd`
(was 1E122321...). Nothing else in that folder or in `%LOCALAPPDATA%\GT4Recomp` was touched.

Not verified: the real webview run with a controller (the owner checks the pad items in `MANUAL-CHECKLIST.md`), and the
real `set_setting` round trip (the mock answers at once).

## 8. Saves and replays in Documents (TASK-081, owner 2026-10-21)

- **Where:** `Documents\Gran Turismo 4\Saves` (`Active\BASCUS-97328GAMEDATA`, `Save NN\...`, `.backup\`) and
  `...\Replays\<replay>`. Documents comes from `SHGetKnownFolderPath(FOLDERID_Documents)`
  (`gt4l_platform::known_folder`), so OneDrive and redirection to another drive are followed; the path is never built
  from the user name. When Windows reports no usable Documents folder, or the library cannot be created there, the
  launcher uses `<data>\Gran Turismo 4` and logs a warning; Play never fails on it. `GT4RECOMP_DOCUMENTS_DIR` replaces
  Documents for development and tests only.
- **What stays in AppData:** the disc copy, prepared files, `gs\`, config and controls, logs, sessions, the live
  `runtime\card.ps2` (the game writes only this file during play, so OneDrive never syncs a card mid-write) and
  `library\backups` (card images before each replacement).
- **Migration:** see [MIGRATIONS](MIGRATIONS.md#the-save-library-moves-to-documents-2026-10-21).
- **Pages:** Saves and Replays show their folder with "Open folder" (`open_saves_folder`, `open_replays_folder`);
  About shows "Saves and replays" with "Open folder" (`open_player_folder`). Items now carry `id` (the library name the
  page sends back) and `modified`; before, the real launcher sent `name`/`label` only, so activate and delete from
  the page had no id (found while wiring the folder row; the dev mock had hidden it).
- **Tests:** `gt4saves/tests/migration.rs` (fresh, migrated and parked, empty new folders, both non-empty, copy
  failure rolled back, a corrupted copy refused, harvest and prepare against Documents), `gt4l-core` `paths` (known
  folder injected, override, fallback) and `guard` (the player's folder refused anywhere, registered roots, program
  folders still writable), `gt4l-tauri` `saves` (first start moves and builds the card; unusable folder falls back;
  item shape). Real exe run against a scratch `GT4RECOMP_DATA_DIR` holding a copy of the owner's old library and a
  scratch `GT4RECOMP_DOCUMENTS_DIR`: 6 files (1,533,775 bytes) moved and checked, old folders in
  `library.migrated-<t>`, a second start logs "already moved". Play was not pressed.


## TASK-089 addendum — Explicit CoreGS choice (2026-10-21, candidate; superseded by TASK-096 in 1.2.3: no CoreGS 1.1 fallback)

A saved CoreGS 1.1 selection makes the external PCSX2 GS package optional throughout setup and
startup: setup emits no renderer download progress and finishes normally; startup performs no package
repair or external renderer release-list request. It still checks launcher releases. The player's
explicit Check for updates and apply-update actions keep the existing optional renderer update path.
Selecting PCSX2 retains its existing default setup, repair and release checks. Play does not inspect an
external renderer executable when CoreGS is selected.

New controls in `flow_audit::coregs_setup_and_play_need_no_external_renderer_package` use a synthetic
ISO and an installer that panics if invoked, persist the choice, reload state and inspect the built-in
launch plan. `updates::tests` inject release fetchers to assert the requested streams, preserve launcher
offers and retain default PCSX2 repair/manual-check behavior. The full Rust workspace passed on Windows,
including 53 Tauri tests (one real-disc flow ignored). Maintainer launcher acceptance remains pending;
this addendum does not qualify renderer quality or a new game executable.
