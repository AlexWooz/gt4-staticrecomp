//! TASK-211: the couch launch (`--skip-launcher`). The launcher runs its normal start-up without a window, starts the
//! game through the same Play path as the button and ends when the game process ends, so Steam (or any other front end
//! that started it) gets the process back. A game that cannot start brings the launcher window with the usual message.
//!
//! One thread, started from the setup hook: it waits for the start-up file moves (the wait Play and setup have), resolves
//! the game (the command line, else the game played last), selects it as the page would (`commands::select_game_of`) and
//! calls `commands::start_play` with [`commands::PlayMode::Direct`]. Nothing here starts a game by itself.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use gt4l_core::messages;
use tauri::{AppHandle, Manager};

use crate::launch_flags::{self, GameSource, LaunchArguments};
use crate::state::{self, lock, Shared};
use crate::{commands, webview_guard};

/// How often the thread looks at the start-up work it waits for.
const STEP: Duration = Duration::from_millis(50);
/// How long a direct launch waits for the 1.2.0 large-data move before it shows the launcher instead (the move of a
/// real install takes seconds; a stuck one must not leave an invisible launcher).
const STARTUP_WAIT: Duration = Duration::from_secs(300);
/// How often the wait writes a line to the log.
const STARTUP_REPORT: Duration = Duration::from_secs(10);

/// Starts the hidden Play of `--skip-launcher`. `startup_moves_done` is set by the launch thread once the 1.2.0 large
/// data has moved and the files a direct start reads are written (Play and setup wait for the same point). Called once,
/// from the setup hook. The thread returns after the game started (its watcher then ends the launcher) or after the
/// launcher window was given the reason the game cannot start.
pub fn start(app: AppHandle, arguments: LaunchArguments, startup_moves_done: Arc<AtomicBool>) {
    std::thread::spawn(move || run(&app, &arguments, &startup_moves_done));
}

fn run(app: &AppHandle, arguments: &LaunchArguments, startup_moves_done: &AtomicBool) {
    let shared = app.state::<Shared>().inner().clone();
    let dirs = lock(&shared).dirs.clone();
    // Never for a normal start (main calls this only for `--skip-launcher`), but keep the thread harmless then.
    let Some(requested) = arguments.direct_game() else {
        return;
    };
    // A `--game` that is not a game is a typo in the front end's launch options: the window opens with the reason. The
    // couch player has no console, so a silent exit would look like the launcher ignoring the game.
    if let Err(problem) = &requested {
        let message = problem.message(state::language_of(&lock(&shared)));
        state::log_error(&dirs, &format!("direct launch: {message}"));
        webview_guard::show_after_failed_launch(app, &message);
        return;
    }
    // The 1.2.0 large data moves in the background at every start; Play and setup wait for it, and so does this (a
    // direct launch that read the folders while they move would fail or start from a half-moved install).
    if !wait_for_startup_moves(&shared, startup_moves_done) {
        let message = messages::text("setup.migrating", state::language_of(&lock(&shared))).to_string();
        state::log_error(&dirs, "direct launch: the start-up file moves are still running; the launcher window is shown");
        webview_guard::show_after_failed_launch(app, &message);
        return;
    }
    // Read the stored choice and the start selection after the moves: both are final now.
    let (stored, fallback) = {
        let state = lock(&shared);
        (state.config.selected_game.clone(), state.selected)
    };
    let (game, source) = match launch_flags::resolve_game(arguments, stored.as_deref(), fallback) {
        Ok(resolved) => resolved,
        Err(problem) => {
            let message = problem.message(state::language_of(&lock(&shared)));
            state::log_error(&dirs, &format!("direct launch: {message}"));
            webview_guard::show_after_failed_launch(app, &message);
            return;
        }
    };
    let reason = match source {
        GameSource::CommandLine => format!("{} named it", launch_flags::GAME),
        GameSource::LastPlayed => "the game played last, no --game was given".to_string(),
        GameSource::StartSelection => "the launcher's start selection, no --game was given and no game was played yet".to_string(),
    };
    state::log(&dirs, &format!("direct launch: {} ({}); {reason}", game.id, game.display_name));
    // A game that is already running is refused, and nothing is shown: the player is in a game already.
    if lock(&shared).game_child.is_some() {
        state::log_error(&dirs, "direct launch refused: a game is already running");
        app.exit(0);
        return;
    }
    // The selection the home page would make, so the page, Saves and Play all name the game that was asked for.
    if let Err(message) = commands::select_game_of(game.id.to_string(), &shared) {
        state::log_error(&dirs, &format!("direct launch: {message}"));
        webview_guard::show_after_failed_launch(app, &message);
        return;
    }
    // The Play button first prepares the CoreGS 2.0 shader cache in a window with a progress bar of its own; a direct
    // launch has no window to show it, so the game compiles what it needs while it runs (TASK-204 records it too).
    if gt4l_core::launch::uses_coregs20(&lock(&shared).config.settings) {
        state::log(&dirs, "direct launch: the shader prepare before Play is skipped (its progress needs the window); the game compiles what it needs");
    }
    // The Play button's own path (the checks, disc, card, textures, pads and `game::start`). Its watcher ends the
    // launcher when the game ends (`PlayMode::Direct`), so this thread is done as soon as the game process runs.
    if let Err(message) = commands::start_play(app, &shared, &commands::program_directory(), commands::PlayMode::Direct) {
        state::log_error(&dirs, &format!("direct launch: {message}"));
        webview_guard::show_after_failed_launch(app, &message);
    }
}

/// Waits until the start-up file moves have finished. Logs the migration every [`STARTUP_REPORT`] and gives up after
/// [`STARTUP_WAIT`]; `false` means the launcher window should be shown instead of starting the game.
fn wait_for_startup_moves(shared: &Shared, done: &AtomicBool) -> bool {
    let began = Instant::now();
    let mut next_report = STARTUP_REPORT;
    while !done.load(Ordering::SeqCst) {
        let waited = began.elapsed();
        if waited >= STARTUP_WAIT {
            return false;
        }
        if waited >= next_report {
            let (dirs, item, bytes_done, bytes_total) = {
                let state = lock(shared);
                (state.dirs.clone(), state.migration.item.clone(), state.migration.bytes_done, state.migration.bytes_total)
            };
            state::log(
                &dirs,
                &format!("direct launch: still moving the game files ({item}, {bytes_done} of {bytes_total} bytes)"),
            );
            next_report += STARTUP_REPORT;
        }
        std::thread::sleep(STEP);
    }
    let waited = began.elapsed();
    if waited >= Duration::from_millis(500) {
        let dirs = lock(shared).dirs.clone();
        state::log(&dirs, &format!("direct launch: the start-up file moves finished after {} ms", waited.as_millis()));
    }
    true
}
