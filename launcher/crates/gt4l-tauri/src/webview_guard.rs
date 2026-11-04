//! WebView2 guard (TASK-091, release 1.2.1, GitHub issue #80: a blank white launcher window).
//!
//! The launcher's window starts hidden and the page calls `ui_ready` once its first screen has drawn; then the window
//! is shown. A watchdog waits up to [`READY_TIMEOUT`] for that call:
//!
//! - ready in time: nothing more happens;
//! - not ready, safe mode off: the flag file [`SAFE_MODE_FILE`] is written in the user folder and a new launcher starts
//!   with [`RESTART_ARGUMENT`] outside the kill-on-close job; this process exits;
//! - not ready, safe mode on: the window is hidden, a native message box (no WebView2) offers the WebView2 download
//!   page or the log folder, and the launcher exits.
//!
//! Safe mode adds `--disable-gpu` to `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` before the web view is created. It stays
//! on until the player turns "Launcher GPU acceleration" On in Settings > Graphics > Advanced (the flag is deleted).
//! A WebView2 runtime that is missing or fails to start gets the native message in [`Problem::Missing`].
//!
//! The decisions, the flag file and the argument and environment rules are pure functions over folders and values,
//! so they are tested without a window. The window actions are in [`start_watchdog`], [`ui_ready`] and
//! [`show_failure`].

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use gt4l_core::paths::DataDirs;
use gt4l_platform::dialog::{self, Choice};
use gt4l_platform::process::spawn_outside_job;
use serde_json::{json, Value};
use tauri::{AppHandle, Manager};

use crate::state::{log, log_error, log_warn};

/// The flag file in the user folder (`%APPDATA%\GT4Recomp`) while safe mode is on.
pub const SAFE_MODE_FILE: &str = "webview-safe-mode";
/// The argument of the restarted launcher (the old process is still exiting).
pub const RESTART_ARGUMENT: &str = "--webview-restart";
/// How long the page has to call `ui_ready`.
pub const READY_TIMEOUT: Duration = Duration::from_secs(12);
/// How long a restarted launcher keeps trying to claim the single-instance mutex, every [`RESTART_CLAIM_STEP`].
pub const RESTART_CLAIM_TIMEOUT: Duration = Duration::from_secs(10);
pub const RESTART_CLAIM_STEP: Duration = Duration::from_millis(250);
/// The WebView2 environment variable read by the runtime at start.
pub const WEBVIEW_ARGUMENTS_VAR: &str = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
/// The switch that turns GPU acceleration off in WebView2.
pub const DISABLE_GPU: &str = "--disable-gpu";
/// Test only: when this variable is `1`, `ui_ready` is ignored, so the watchdog and the native message can be seen.
pub const TEST_NEVER_READY_VAR: &str = "GT4RECOMP_TEST_UI_NEVER_READY";
/// The WebView2 Runtime download page (Microsoft's evergreen bootstrapper).
pub const WEBVIEW2_DOWNLOAD_URL: &str = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
/// The row id of the Settings toggle (Graphics > Advanced). It is not a game setting: see [`gpu_row`].
pub const GPU_ROW_ID: &str = "launcher_gpu";

/// What the watchdog does after the wait.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    Show,
    RestartInSafeMode,
    ShowNativeErrorAndExit,
}

/// The rule of the watchdog. `ready_in_time`: `ui_ready` arrived within [`READY_TIMEOUT`].
pub fn decide(safe_mode_on: bool, ready_in_time: bool) -> Decision {
    if ready_in_time {
        Decision::Show
    } else if safe_mode_on {
        Decision::ShowNativeErrorAndExit
    } else {
        Decision::RestartInSafeMode
    }
}

/// Whether the launcher was started as the restart of a safe-mode run.
pub fn restart_requested<I, S>(arguments: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    arguments.into_iter().any(|argument| argument.as_ref() == RESTART_ARGUMENT)
}

/// The value of `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` with `--disable-gpu` added. An existing value is kept (its
/// switches stay in front); the switch is not added twice.
pub fn with_gpu_disabled(existing: Option<&str>) -> String {
    with_switch(existing, DISABLE_GPU)
}

/// `existing` with `switch` added once, after the switches already there.
fn with_switch(existing: Option<&str>, switch: &str) -> String {
    let existing = existing.unwrap_or("").trim();
    if existing.split_whitespace().any(|present| present == switch) {
        return existing.to_string();
    }
    if existing.is_empty() {
        switch.to_string()
    } else {
        format!("{existing} {switch}")
    }
}


/// The flag file of safe mode in `user_root`.
pub fn safe_mode_flag(user_root: &Path) -> PathBuf {
    user_root.join(SAFE_MODE_FILE)
}

/// Whether safe mode is on (the flag file exists).
pub fn safe_mode_on(user_root: &Path) -> bool {
    safe_mode_flag(user_root).is_file()
}

/// Writes the flag: the date and the reason, one per line. `now_seconds` is the Unix time.
pub fn write_safe_mode(user_root: &Path, reason: &str, now_seconds: u64) -> std::io::Result<()> {
    fs::create_dir_all(user_root)?;
    let reason = reason.replace(['\r', '\n'], " ");
    fs::write(safe_mode_flag(user_root), format!("date: {}\nreason: {reason}\n", date_text(now_seconds)))
}

/// Deletes the flag. A missing flag is not an error.
pub fn clear_safe_mode(user_root: &Path) -> std::io::Result<()> {
    match fs::remove_file(safe_mode_flag(user_root)) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
    }
}

/// The text of the flag file, if it exists (the date and the reason). Read by the tests only.
#[cfg(test)]
pub fn read_safe_mode_note(user_root: &Path) -> Option<String> {
    fs::read_to_string(safe_mode_flag(user_root)).ok()
}

/// `YYYY-MM-DD` (UTC) of a Unix time in seconds.
pub fn date_text(seconds: u64) -> String {
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let days = (seconds / 86_400) as i64 + 719_468;
    let era = days.div_euclid(146_097);
    let day_of_era = days - era * 146_097;
    let year_of_era = (day_of_era - day_of_era / 1_460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 { month_index + 3 } else { month_index - 9 };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02}")
}

/// The Settings row of the launcher (Graphics > Advanced). Shown as Off while the flag exists. It is appended to the
/// game settings table by `commands::get_settings_table`.
pub fn gpu_row(safe_mode_on: bool) -> Value {
    json!({
        "id": GPU_ROW_ID,
        "label": "Launcher GPU acceleration",
        "help": "Off keeps the launcher window without GPU acceleration, after it failed to draw in WebView2. On takes effect at the next start of the launcher.",
        "page": "basic",
        "group": "general",
        "control": "toggle",
        "values": Value::Null,
        "default": true,
        "current": !safe_mode_on,
        "disabled": false,
    })
}

/// Applies the setting of the row: On deletes the flag, Off writes it. Takes effect at the next start.
pub fn set_gpu_acceleration(user_root: &Path, on: bool, now_seconds: u64) -> Result<(), String> {
    if on {
        clear_safe_mode(user_root).map_err(|error| format!("could not turn GPU acceleration on: {error}"))
    } else {
        write_safe_mode(user_root, "turned off in Settings (Launcher GPU acceleration)", now_seconds)
            .map_err(|error| format!("could not turn GPU acceleration off: {error}"))
    }
}

/// At startup, before the web view exists: when safe mode is on, `--disable-gpu` goes into the WebView2 arguments.
/// Returns whether safe mode is on.
pub fn apply_safe_mode_environment(user_root: &Path) -> bool {
    if !safe_mode_on(user_root) {
        return false;
    }
    let existing = std::env::var(WEBVIEW_ARGUMENTS_VAR).ok();
    std::env::set_var(WEBVIEW_ARGUMENTS_VAR, with_gpu_disabled(existing.as_deref()));
    true
}

/// Test only (see [`TEST_NEVER_READY_VAR`]).
pub fn test_never_ready() -> bool {
    std::env::var(TEST_NEVER_READY_VAR).map(|value| value == "1").unwrap_or(false)
}

/// Test only: when this variable is `1`, the start-up check reports WebView2 as missing, so the native message can be
/// seen on a PC that has the runtime. Not for players.
pub const TEST_WEBVIEW2_MISSING_VAR: &str = "GT4RECOMP_TEST_WEBVIEW2_MISSING";
/// A fixed-version WebView2 runtime folder; when it is set, the registry check is skipped (see `gt4l_platform::webview2`).
pub const WEBVIEW2_FIXED_FOLDER_VAR: &str = "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER";

/// Test only (see [`TEST_WEBVIEW2_MISSING_VAR`]).
pub fn test_webview2_missing() -> bool {
    std::env::var(TEST_WEBVIEW2_MISSING_VAR).map(|value| value == "1").unwrap_or(false)
}

/// The start-up check of the WebView2 runtime (TASK-091, issue #80). Runs before the Tauri builder, so a missing
/// runtime is reported at once, not after the watchdog's wait. Returns `true` when the runtime is missing; the caller
/// shows [`show_failure`] with [`Problem::Missing`] and exits.
pub fn check_webview2_at_start(dirs: &DataDirs) -> bool {
    let fixed = gt4l_platform::webview2::fixed_runtime_folder_set(std::env::var(WEBVIEW2_FIXED_FOLDER_VAR).ok().as_deref());
    let check = gt4l_platform::webview2::check_installed(fixed, test_webview2_missing());
    match &check.version {
        Some(version) => log(dirs, &format!("WebView2 runtime found in the registry: {version}")),
        None => log_warn(dirs, "WebView2 runtime not found in the registry"),
    }
    if check.missing {
        log_error(dirs, "WebView2 is missing: the native message is shown before any window");
    }
    check.missing
}

/// Set by `ui_ready` once the first screen has drawn.
#[derive(Default)]
pub struct PageReady {
    ready: AtomicBool,
}

impl PageReady {
    /// Marks the page ready. Returns `true` for the first call only.
    pub fn mark(&self) -> bool {
        !self.ready.swap(true, Ordering::SeqCst)
    }

    pub fn is_ready(&self) -> bool {
        self.ready.load(Ordering::SeqCst)
    }
}

/// Waits until the page is ready or `timeout` passes. Returns whether it became ready.
pub fn wait_for_ready(page: &PageReady, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    loop {
        if page.is_ready() {
            return true;
        }
        let now = Instant::now();
        if now >= deadline {
            return false;
        }
        std::thread::sleep((deadline - now).min(Duration::from_millis(50)));
    }
}

/// The state the window and the watchdog share. Managed by Tauri.
pub struct Guard {
    pub page: PageReady,
    pub dirs: DataDirs,
    pub user_root: PathBuf,
    /// When the process started (for the "ui ready after N ms" log line, TASK-091 1.2.1).
    pub started: std::time::Instant,
}

/// Why the web view could not show the launcher.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Problem {
    /// The page did not draw (in safe mode too).
    NotDrawn,
    /// The WebView2 runtime is missing or failed to start.
    Missing,
}

/// The page's first screen has drawn: the window is shown and focused. Idempotent.
#[tauri::command]
pub fn ui_ready(app: AppHandle) -> Result<(), String> {
    let guard = app.state::<Arc<Guard>>().inner().clone();
    if test_never_ready() {
        log(&guard.dirs, "test: ui_ready ignored (GT4RECOMP_TEST_UI_NEVER_READY=1)");
        return Ok(());
    }
    if guard.page.mark() {
        log(&guard.dirs, &format!("the launcher page drew its first screen; ui ready after {} ms", guard.started.elapsed().as_millis()));
    }
    show_main_window(&app);
    Ok(())
}

/// The page writes a short line to the launcher log (the 1.3.0 intro: start, end, skipped). Capped at 200 characters.
#[tauri::command]
pub fn ui_log(app: AppHandle, message: String) -> Result<(), String> {
    let guard = app.state::<Arc<Guard>>().inner().clone();
    let line: String = message.chars().filter(|c| !c.is_control()).take(200).collect();
    log(&guard.dirs, &format!("page: {line}"));
    Ok(())
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn hide_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.hide();
    }
}

/// Starts the watchdog thread that waits for `ui_ready` (see the module comment).
pub fn start_watchdog(app: AppHandle, guard: Arc<Guard>) {
    std::thread::spawn(move || {
        let ready = wait_for_ready(&guard.page, READY_TIMEOUT);
        let safe = safe_mode_on(&guard.user_root);
        match decide(safe, ready) {
            Decision::Show => {}
            Decision::RestartInSafeMode => restart_in_safe_mode(&app, &guard),
            Decision::ShowNativeErrorAndExit => {
                log_error(&guard.dirs, "the launcher page did not draw in safe mode either");
                hide_main_window(&app);
                show_failure(&guard.dirs, Problem::NotDrawn);
                app.exit(0);
            }
        }
    });
}

fn restart_in_safe_mode(app: &AppHandle, guard: &Guard) {
    let now = unix_seconds();
    if let Err(error) = write_safe_mode(&guard.user_root, "the window did not draw within 12 s (WebView2)", now) {
        // Without the flag a restart would wait again and again, so the player gets the message instead.
        log_error(&guard.dirs, &format!("the page did not draw; the safe mode flag could not be written: {error}"));
        hide_main_window(app);
        show_failure(&guard.dirs, Problem::NotDrawn);
        app.exit(0);
        return;
    }
    log_warn(&guard.dirs, "the page did not draw in 12 s: restarting with WebView2 GPU acceleration off (safe mode)");
    let started = std::env::current_exe().and_then(|exe| spawn_outside_job(Command::new(exe).arg(RESTART_ARGUMENT)));
    match started {
        Ok(_) => app.exit(0),
        Err(error) => {
            log_error(&guard.dirs, &format!("the safe mode restart could not start: {error}"));
            hide_main_window(app);
            show_failure(&guard.dirs, Problem::NotDrawn);
            app.exit(0);
        }
    }
}

/// The native message for a web view that failed. Yes opens the WebView2 download page, No opens the log folder,
/// Cancel only closes it. The message box does not depend on WebView2.
pub fn show_failure(dirs: &DataDirs, problem: Problem) {
    let logs = dirs.logs();
    let logs_text = logs.display();
    // The native message comes before any window and before the settings are read in full: it follows the Windows
    // language (`auto`).
    let language = gt4l_core::language::from_os_locale(&gt4l_platform::system::os_locale());
    let id = match problem {
        Problem::NotDrawn => "dialog.not_drawn",
        Problem::Missing => "dialog.missing",
    };
    let text = gt4l_core::messages::render(
        id,
        language,
        &[("url", WEBVIEW2_DOWNLOAD_URL), ("logs", &logs_text.to_string())],
    );
    log(dirs, &format!("native message: {problem:?}"));
    match dialog::error_box_yes_no_cancel("GT4Recomp", &text) {
        Choice::Yes => {
            let _ = Command::new("explorer.exe").arg(WEBVIEW2_DOWNLOAD_URL).spawn();
        }
        Choice::No => {
            let _ = fs::create_dir_all(&logs);
            let _ = Command::new("explorer.exe").arg(&logs).spawn();
        }
        Choice::Cancel => {}
    }
}

/// Seconds since the Unix epoch (0 when the clock is before it).
pub fn unix_seconds() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|value| value.as_secs()).unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A fresh folder under the system temp folder (the tests make no other use of it).
    fn scratch_folder(name: &str) -> PathBuf {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let folder = std::env::temp_dir().join(format!("gt4-webview-guard-{name}-{}-{nanos}", std::process::id()));
        fs::create_dir_all(&folder).unwrap();
        folder
    }

    #[test]
    fn the_watchdog_decisions() {
        assert_eq!(decide(false, true), Decision::Show);
        assert_eq!(decide(true, true), Decision::Show, "a ready page is shown in safe mode too");
        assert_eq!(decide(false, false), Decision::RestartInSafeMode);
        assert_eq!(decide(true, false), Decision::ShowNativeErrorAndExit, "no second restart in safe mode");
    }

    #[test]
    fn the_restart_argument_is_recognised_exactly() {
        assert!(restart_requested(["--webview-restart"]));
        assert!(restart_requested(vec!["--other".to_string(), "--webview-restart".to_string()]));
        assert!(!restart_requested(Vec::<String>::new()));
        assert!(!restart_requested(["--webview-restartx"]));
        assert!(!restart_requested(["--WEBVIEW-RESTART"]));
    }

    #[test]
    fn the_gpu_switch_is_appended_once() {
        assert_eq!(with_gpu_disabled(None), "--disable-gpu");
        assert_eq!(with_gpu_disabled(Some("")), "--disable-gpu");
        assert_eq!(with_gpu_disabled(Some("   ")), "--disable-gpu");
        assert_eq!(with_gpu_disabled(Some("--remote-debugging-port=9222")), "--remote-debugging-port=9222 --disable-gpu");
        assert_eq!(with_gpu_disabled(Some("--disable-gpu")), "--disable-gpu");
        assert_eq!(with_gpu_disabled(Some("--a --disable-gpu --b")), "--a --disable-gpu --b");
        assert_eq!(with_gpu_disabled(Some("--disable-gpu-compositing")), "--disable-gpu-compositing --disable-gpu");
    }

    #[test]
    fn the_flag_file_round_trip() {
        let folder = scratch_folder("flag");
        assert!(!safe_mode_on(&folder));
        assert_eq!(read_safe_mode_note(&folder), None);
        write_safe_mode(&folder, "first\nline", 951_782_400).unwrap();
        assert!(safe_mode_on(&folder));
        let note = read_safe_mode_note(&folder).unwrap();
        assert!(note.contains("date: 2000-02-29"), "{note}");
        assert!(note.contains("reason: first line"), "{note}");
        clear_safe_mode(&folder).unwrap();
        assert!(!safe_mode_on(&folder));
        clear_safe_mode(&folder).unwrap();
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn the_settings_row_follows_the_flag_and_writes_it() {
        let folder = scratch_folder("row");
        assert_eq!(gpu_row(false)["current"], json!(true));
        assert_eq!(gpu_row(true)["current"], json!(false), "shown as Off while the flag exists");
        set_gpu_acceleration(&folder, false, 0).unwrap();
        assert!(safe_mode_on(&folder), "Off writes the flag");
        set_gpu_acceleration(&folder, true, 0).unwrap();
        assert!(!safe_mode_on(&folder), "On deletes the flag");
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn dates_are_utc_calendar_dates() {
        assert_eq!(date_text(0), "1970-01-01");
        assert_eq!(date_text(951_782_400), "2000-02-29");
        assert_eq!(date_text(951_868_799), "2000-02-29");
        assert_eq!(date_text(951_868_800), "2000-03-01");
        assert_eq!(date_text(1_767_225_600), "2026-01-01");
    }

    #[test]
    fn the_page_ready_wait_returns_on_mark_and_times_out() {
        let page = PageReady::default();
        assert!(!wait_for_ready(&page, Duration::from_millis(60)));
        assert!(page.mark(), "the first mark is the first ready");
        assert!(!page.mark(), "ui_ready is idempotent");
        assert!(wait_for_ready(&page, Duration::from_millis(60)));
    }
}
