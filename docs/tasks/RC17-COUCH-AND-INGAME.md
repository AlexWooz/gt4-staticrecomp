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
3. runs the ordinary Play path (checks, disc, card, texture folder, `game::start`) with no window;
4. stays hidden while the game runs; if the game is already running, refuses and logs;
5. exits when the game process ends, so Steam returns to Big Picture.

If the game is not ready (no disc, no install, a check failure), the normal launcher window opens with the error, so
the player can fix it. A missing `--game` falls back to the last played game and logs it; an unknown value is an error.

Implementation (2026-11-09, branch `task/211-rc17-launcher-flags`):

- `launcher/crates/gt4l-tauri/src/launch_flags.rs`: the parser (`--skip-launcher`, `--game=<id>` and `--game <id>`,
  `retail`/`gt4`/`spec2`, case-insensitive values), the game resolution (command line, else the game played last, else
  the launcher's start selection), the safe-mode restart arguments and the two messages.
- `launcher/crates/gt4l-tauri/src/direct_launch.rs`: the hidden launch thread. It waits for the 1.2.0 large-data move,
  selects the game as the page would (`commands::select_game_of`) and calls the shared Play path; a failure shows the
  launcher window with the message; the game watcher exits the launcher (`app.exit(0)`) when the game ends.
- `commands::start_play` is the Play path both the button and the direct launch use (`commands::play` is now a thin
  wrapper), with `PlayMode::Windowed` (tray, page) or `PlayMode::Direct` (hidden, exit with the game).
- `webview_guard`: the window stays hidden under `--skip-launcher` (`Guard::hidden_launch`), a failed direct launch
  reveals it once the page is ready (`show_after_failed_launch`), and a safe-mode restart repeats the flags.
- The shader prepare before Play (CoreGS 2.0 only) is skipped in a direct launch: its progress needs the window, and the
  game compiles what it needs while it runs (TASK-204). The log records the decision.
- Tests: parser and game-resolution unit tests (`launch_flags`), the hidden-window flag (`webview_guard`), and a flow
  test that resolves `--game=retail`/`--game=spec2` against a synthetic install and runs the same Play checks headlessly
  (`flow_audit::a_direct_launch_selects_its_game_and_runs_the_play_checks_headlessly`).
- The Steam setup text is in `docs/launcher/LAUNCHER-PLAN.md` section 16.

Steam setup (player steps, also in `docs/launcher/LAUNCHER-PLAN.md` section 16.4):

1. Steam > Games > **Add a Non-Steam Game** > Browse > `GT4Recomp.exe`.
2. Properties > **Launch Options**: `--skip-launcher --game=retail` (GT4 v2.00) or `--skip-launcher --game=spec2`
   (Spec II). Both games can have their own entry.
3. Start from Big Picture: the game starts with no launcher window, and Steam returns to the game list when the game
   closes.
4. Exit: the in-game Quit Game (TASK-212) is the primary path; Big Picture's "Exit Game" remains the fallback (it ends
   the launcher, and the launcher's kill-on-close job ends the game); the controller shortcut, if implemented, is the
   third.
5. Never point Steam at `gt4.exe`/`gt4-spec2.exe`: the launcher still owns the disc image, the card, the renderer, the
   shader prepare and the update checks.

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
