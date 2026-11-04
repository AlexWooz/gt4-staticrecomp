//! Tray behaviour (Windows). On Play the launcher hides its window to the system tray; when the game process ends
//! (normally or by a crash) the window comes back on the Home page with any message.
//!
//! The rules are a pure state machine ([`apply`]) so they are tested without a window. [`handle`] runs the actions
//! on the Tauri window and the tray menu.
//!
//! - game started -> hidden;
//! - game ended -> shown, Home, with the message;
//! - close button while the game runs -> ask "The game is running. Close the launcher anyway?" (Close ends the
//!   launcher and, through the kill-on-close job, the game); otherwise the close button ends the launcher, it does
//!   not go to the tray;
//! - tray "Quit" is disabled while the game runs (and ignored if sent anyway).

use gt4l_core::messages;
use std::sync::Mutex;

use serde_json::json;
use tauri::menu::MenuItem;
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{App, AppHandle, Emitter, Manager, Runtime};

use crate::state::log;

/// Whether the launcher window is visible to the player.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Window {
    Shown,
    Hidden,
}

/// The state the rules depend on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct State {
    pub game_running: bool,
    pub window: Window,
}

impl Default for State {
    fn default() -> Self {
        State { game_running: false, window: Window::Shown }
    }
}

/// What happened.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    GameStarted,
    /// The game process ended. `message` is empty after a normal close.
    GameEnded { message: String },
    /// The window close button (or Alt+F4).
    CloseRequested,
    /// The player confirmed "Close anyway" in the dialog.
    ConfirmClose,
    /// "Show launcher" in the tray menu, or a click on the tray icon.
    ShowRequested,
    /// "Quit" in the tray menu.
    QuitRequested,
}

/// What the launcher does about it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Action {
    Hide,
    /// Show and focus the window. `home` selects the Home page; a non-empty `message` is shown there.
    Show { home: bool, message: String },
    /// Show the confirmation "The game is running. Close the launcher anyway?".
    AskClose,
    /// End the launcher.
    Exit,
    Ignore,
}

/// Applies one event. Updates `state` and returns the action to run.
pub fn apply(state: &mut State, event: Event) -> Action {
    match event {
        Event::GameStarted => {
            state.game_running = true;
            state.window = Window::Hidden;
            Action::Hide
        }
        Event::GameEnded { message } => {
            state.game_running = false;
            state.window = Window::Shown;
            Action::Show { home: true, message }
        }
        Event::CloseRequested => {
            if state.game_running {
                Action::AskClose
            } else {
                Action::Exit
            }
        }
        Event::ConfirmClose => Action::Exit,
        Event::ShowRequested => {
            state.window = Window::Shown;
            Action::Show { home: false, message: String::new() }
        }
        Event::QuitRequested => {
            if state.game_running {
                Action::Ignore
            } else {
                Action::Exit
            }
        }
    }
}

/// The "Quit" item is enabled only while no game runs.
pub fn quit_enabled(state: &State) -> bool {
    !state.game_running
}

/// The tray control kept in the Tauri state: the rules, and the menu item whose enabled state follows them.
pub struct Control<R: Runtime> {
    pub state: Mutex<State>,
    pub quit_item: MenuItem<R>,
}

/// `confirm_close()`: the player chose "Close anyway" in the confirmation dialog.
#[tauri::command]
pub fn confirm_close(app: AppHandle) {
    handle(&app, Event::ConfirmClose);
}

/// Runs one event on the launcher window and the tray. Safe to call from any thread.
pub fn handle<R: Runtime>(app: &AppHandle<R>, event: Event) {
    let Some(control) = app.try_state::<Control<R>>() else {
        return;
    };
    let (action, enabled) = {
        let mut state = match control.state.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        let action = apply(&mut state, event);
        (action, quit_enabled(&state))
    };
    let _ = control.quit_item.set_enabled(enabled);
    // The gamepad service reads the pad only while the window is shown (a hidden launcher leaves the game its input).
    if let Some(pads) = app.try_state::<crate::pad_service::Handle>() {
        match &action {
            Action::Hide => pads.set_active(false),
            Action::Show { .. } => pads.set_active(true),
            _ => {}
        }
    }
    let window = app.get_webview_window("main");
    match action {
        Action::Hide => {
            if let Some(window) = window {
                let _ = window.hide();
            }
        }
        Action::Show { home, message } => {
            if let Some(window) = window {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
            }
            if home {
                let _ = app.emit("game-state", json!({ "running": false, "message": message }));
            }
        }
        Action::AskClose => {
            if let Some(window) = window {
                let _ = window.show();
                let _ = window.set_focus();
            }
            let _ = app.emit("close-confirm", json!({ "message": gt4l_core::messages::text("tray.close_confirm", crate::state::current_language()) }));
        }
        Action::Exit => app.exit(0),
        Action::Ignore => {}
    }
}

/// Builds the tray icon and its menu, and manages the [`Control`]. Called once from the setup hook.
pub fn install(app: &mut App) -> tauri::Result<()> {
    let show = MenuItem::with_id(app, "show", gt4l_core::messages::text("tray.show", crate::state::current_language()), true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
    let menu = tauri::menu::Menu::with_items(app, &[&show, &quit])?;
    let mut builder = TrayIconBuilder::with_id("gt4recomp-tray")
        .tooltip("GT4Recomp")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(|app, event| match event.id.as_ref() {
            "show" => handle(app, Event::ShowRequested),
            "quit" => handle(app, Event::QuitRequested),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                handle(tray.app_handle(), Event::ShowRequested);
            }
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    builder.build(app)?;
    app.manage(Control { state: Mutex::new(State::default()), quit_item: quit });
    log_state(app.handle(), "tray ready");
    Ok(())
}

fn log_state<R: Runtime>(app: &AppHandle<R>, message: &str) {
    if let Some(dirs) = app.try_state::<crate::state::Shared>().map(|shared| crate::state::lock(shared.inner()).dirs.clone()) {
        log(&dirs, message);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn play_hides_the_window_and_the_game_running_disables_quit() {
        let mut state = State::default();
        assert_eq!(apply(&mut state, Event::GameStarted), Action::Hide);
        assert!(state.game_running);
        assert_eq!(state.window, Window::Hidden);
        assert!(!quit_enabled(&state));
    }

    #[test]
    fn the_game_ending_shows_home_with_the_message() {
        let mut state = State { game_running: true, window: Window::Hidden };
        let action = apply(&mut state, Event::GameEnded { message: "stopped unexpectedly".into() });
        assert_eq!(action, Action::Show { home: true, message: "stopped unexpectedly".into() });
        assert!(!state.game_running);
        assert_eq!(state.window, Window::Shown);
        assert!(quit_enabled(&state));
    }

    #[test]
    fn a_normal_close_ends_with_no_message_and_is_shown_on_home() {
        let mut state = State { game_running: true, window: Window::Hidden };
        assert_eq!(apply(&mut state, Event::GameEnded { message: String::new() }), Action::Show { home: true, message: String::new() });
    }

    #[test]
    fn closing_while_the_game_runs_asks_first() {
        let mut state = State { game_running: true, window: Window::Shown };
        assert_eq!(apply(&mut state, Event::CloseRequested), Action::AskClose);
        assert!(state.game_running, "asking changes nothing");
        assert_eq!(apply(&mut state, Event::ConfirmClose), Action::Exit);
    }

    #[test]
    fn closing_while_idle_ends_the_launcher_and_never_goes_to_the_tray() {
        let mut state = State::default();
        assert_eq!(apply(&mut state, Event::CloseRequested), Action::Exit);
    }

    #[test]
    fn quit_is_ignored_while_the_game_runs_and_works_when_idle() {
        let mut state = State { game_running: true, window: Window::Hidden };
        assert_eq!(apply(&mut state, Event::QuitRequested), Action::Ignore);
        let mut idle = State::default();
        assert_eq!(apply(&mut idle, Event::QuitRequested), Action::Exit);
    }

    #[test]
    fn show_brings_back_a_hidden_window_without_changing_the_page() {
        let mut state = State { game_running: true, window: Window::Hidden };
        assert_eq!(apply(&mut state, Event::ShowRequested), Action::Show { home: false, message: String::new() });
        assert_eq!(state.window, Window::Shown);
        assert!(state.game_running);
    }

    #[test]
    fn a_full_session_goes_hidden_then_shown_then_idle() {
        let mut state = State::default();
        apply(&mut state, Event::GameStarted);
        assert_eq!(state.window, Window::Hidden);
        apply(&mut state, Event::GameEnded { message: String::new() });
        assert_eq!(state.window, Window::Shown);
        assert_eq!(apply(&mut state, Event::CloseRequested), Action::Exit);
    }
}
