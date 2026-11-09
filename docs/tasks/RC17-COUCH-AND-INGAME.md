# rc17: couch launch from Steam and the first in-game options

Owner request, 2026-11-09, from a Discord conversation in `#general-en` (photo in the Telegram inbox,
`photo-20261109-112045-1244.jpg`):

- **moxxx** wants to sit on the couch, add GT4Recomp to Steam, launch it from there with a controller, and close the
  game with the controller to return to Steam. His two asks:
  1. a way to **skip the launcher** and boot straight into the game from Steam (for example `--skip-launcher`);
  2. a built-in controller shortcut to exit the game, or a documented Alt+F4-through-Steam mapping.
- **YocYoc** notes the launcher can be added as a non-Steam game and controlled with a gamepad.
- **badrockjones** notes Steam Big Picture can exit any game (Xbox/PS button, the running game, "Exit Game").
- **Owner:** add a launcher start flag to skip the launcher, with a game flag (`retail` and `spec2`) because the
  selection is skipped. For rc17, start exploring GT4's own menus: a **Quit Game** in the main menu, and graphics and
  control options inside the game. The direction is to move as many launcher functions as possible into the game.

## rc17 tasks

| Task | Scope | Build |
|---|---|---|
| **TASK-211** | `--skip-launcher` + `--game=retail|spec2`; hidden launch, same Play path, wait for the game, exit; Steam/Big Picture docs | rc17 |
| **TASK-212** | GT4 main-menu **Quit Game** and/or a controller exit shortcut; study the menu and host exit first | rc17 study, first item if the hook is safe |
| **TASK-213** | In-game settings parity (graphics, controls, audio) as an umbrella; study the menu and overlay options first | rc17 study, later rows |

The split-screen tasks (TASK-209 two controllers, TASK-210 split-screen validation) are registered on
`task/205-variant-coverage` (`459bdfda`); merge their TASKS entry into the rc17 line before the release branch.

## TASK-211: the couch launch

The launcher is the Steam process. It must stay alive for the whole game session so Steam knows the game is running
and returns when it ends. The direct path therefore:

1. reads `--skip-launcher` and `--game=retail|spec2` before the window is shown;
2. resolves the game (`retail` -> `gt4`, `spec2` -> `spec2`) and writes the same selected game the UI would;
3. runs the ordinary Play path (checks, disc, card, texture folder, shader prepare, `game::start`) with no window;
4. stays hidden while the game runs; if the game is already running, refuses and logs;
5. exits when the game process ends, so Steam returns to Big Picture.

If the game is not ready (no disc, no install, a check failure), the normal launcher window opens with the error, so
the player can fix it. A missing `--game` falls back to the last played game and logs it; an unknown value is an error.

Steam setup to document:

- Add `GT4Recomp.exe` as a non-Steam game.
- Launch options: `--skip-launcher --game=retail` for GT4 v2.00, `--skip-launcher --game=spec2` for Spec II.
- Exit: the in-game Quit Game (TASK-212) is the primary path; Big Picture's "Exit Game" remains the fallback; the
  controller shortcut, if implemented, is the third.
- The launcher still owns the disc, card, shader prepare and update checks; do not point Steam directly at
  `gt4.exe`/`gt4-spec2.exe`.

## TASK-212: Quit Game in GT4's menu

The game has no quit-to-desktop. The study must map:

- the main-menu data and the selection handler (GT4 decomp/ELF references);
- the host exit path (the window/process close, the card flush, the session log);
- whether a new menu row can be added safely, or whether a controller shortcut with a confirmation overlay is the
  safer first step.

The quit must flush the card and the session log before the process ends, and must not change gameplay, progression
or the game's own save flow. A controller-only exit is the acceptance: the player can put the pad down, not touch the
keyboard.

## TASK-213: launcher functions in the game

The umbrella starts with a read-only study of GT4's Options menu and the existing overlay hooks. Two routes:

- **Extend the game's Options menu** with rows that write the launcher's config (renderer, resolution/scale, effects,
  controls). The game then reads the same config the launcher writes, with the same restart rules.
- **A native overlay** over the game menu (the performance overlay/F8 style) when patching the game's menu is not
  safe. This is a host UI, not a game-logic change.

Start with a small, safe set; do not change physics, AI, timing, progression or saves. The long-term direction is
the maximum of launcher functions in the game, but each row needs its own write-through and restart rule.

## Study result (2026-11-09)

The full study is in [GT4-MENU-QUIT-SETTINGS-STUDY.md](GT4-MENU-QUIT-SETTINGS-STUDY.md). The decisions:

- The GT4 main menu is data + script (`arcade` project, `TopRoot`), not a fixed ELF table. A real **Quit Game**
  row is feasible but needs the **GT4.VOL overlay reader** and a v5 script/widget edit (TASK-078 P1/P2); it is
  not an rc17 change.
- **TASK-212 rc17** is host-only: a controller combo (L3+R3+Start) with a confirmation drawn by the achievements
  overlay, closing through the normal `WM_CLOSE` path (card/audio/F8 flush, `window closed`). Also classify the
  guest `LoadExecPS2`/`Exit` as a normal quit, not a crash.
- **TASK-213 rc17** is overlay-first: Fullscreen (live) and then Renderer (next start) through a
  `game-changes.toml` feedback file the launcher merges into `settings.toml`. The native GT4 page waits for the
  VOL reader and the v5 script round trip.
- Spec II's extra menus are **data/script replacements inside GT4.VOL** (Adhoc v7), not executable code; the
  plugin supplies behaviour only. They are not reusable for v2.00 (Adhoc v5); the approach is the reference.

## Acceptance and risks

- TASK-211: from Steam, the game starts without the launcher window; the game closes and Steam returns; both
  `retail` and `spec2` work; a missing disc falls back to the launcher error, not a silent exit.
- TASK-212: the controller can quit from the game; the card is flushed and the session log closed; no game logic
  change.
- TASK-213: the first in-game row writes the same config as the launcher and survives a restart.
- Risks: the launcher's single-instance lock and tray; WebView2 is required even for the hidden path; the game
  process parent/child relation; Steam's process detection; menu hook safety; the game's font/layout; save flushing;
  PGO/ThinLTO hashes unaffected by the launcher-only part.

## References

- `docs/launcher/LAUNCHER-PLAN.md` section 15.
- `launcher/crates/gt4l-tauri/src/main.rs`, `commands.rs` (`play`, `prepare_shaders`), `state.rs`.
- `launcher/crates/gt4l-core/src/games.rs` (`gt4`/`spec2`, executables).
- `launcher/crates/gt4l-platform/src/game.rs` (`start`, `prepare_shaders`).
- GT4 decomp references in `private/slice151-research/gt4-decomp` and `private/references/gt4-decomp` (read-only).
- The Discord screenshot: `C:\Users\Alano\.agents\skills\telegram-notify\inbox\photo-20261109-112045-1244.jpg`.
