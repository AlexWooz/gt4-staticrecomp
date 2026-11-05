//! Builds the log bundle of TASK-111 (rc4): the newest logs and game sessions, the settings file and the system report,
//! written as one zip. The rules (names, masking, what may go in, the zip writer) are in `gt4l_core::log_export`.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use gt4l_core::games;
use gt4l_core::log_export::{self, Entry, GpuFact, SystemFacts, KEPT_FOR_EXPORT};
use gt4l_core::logs;

use crate::state::AppState;

/// The file time of `path`, or the epoch when it cannot be read.
fn changed(path: &Path) -> SystemTime {
    fs::metadata(path).and_then(|metadata| metadata.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}

/// Writes the bundle into `folder` and returns its path. `user_name` is the Windows user name to mask.
pub fn export(state: &AppState, folder: &Path, user_name: &str) -> Result<PathBuf, String> {
    let now = gt4l_platform::system::local_time();
    let path = folder.join(log_export::zip_name(&now));
    let entries = collect(state)?;
    let masked = log_export::masked(&entries, user_name);
    log_export::write_zip(&path, &masked).map_err(|error| format!("{}: {error}", path.display()))?;
    Ok(path)
}

/// The entries of the bundle before masking.
pub(crate) fn collect(state: &AppState) -> Result<Vec<Entry>, String> {
    let dirs = &state.dirs;
    let logs_dir = dirs.logs();
    let mut entries = Vec::new();

    if let Ok(text) = fs::read_to_string(crate::session_log::latest_path(dirs)) {
        entries.push(Entry { path: logs::LATEST_NAME.to_string(), text });
    }

    // TASK-170: the last shader prepare step (gt4.exe --prepare-shaders writes its stderr there): where a compile stopped.
    if let Ok(text) = fs::read_to_string(logs_dir.join("shaders.log")) {
        entries.push(Entry { path: "logs/shaders.log".to_string(), text });
    }

    // The newest launcher session logs (the legacy 1.2.x file counts as one of them).
    let session_logs = logs::session_logs(&logs_dir).unwrap_or_default();
    let found: Vec<(SystemTime, PathBuf)> = session_logs.into_iter().map(|path| (changed(&path), path)).collect();
    for path in log_export::newest(found, KEPT_FOR_EXPORT) {
        let name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
        if let Ok(text) = fs::read_to_string(&path) {
            entries.push(Entry { path: format!("launcher/{name}"), text });
        }
    }

    // The newest game sessions: their game log and the GSdx settings the run wrote.
    let sessions = dirs.sessions();
    let mut folders: Vec<(SystemTime, PathBuf)> = fs::read_dir(&sessions)
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.is_dir())
        .filter(|path| path.file_name().and_then(|name| name.to_str()).is_some_and(|name| name.starts_with("session-")))
        .map(|path| (changed(&path), path))
        .collect();
    folders.sort_by(|a, b| b.0.cmp(&a.0));
    for folder in log_export::newest(folders, KEPT_FOR_EXPORT) {
        let name = folder.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
        for file in ["game.log", "gt4-gs.ini"] {
            if let Ok(text) = fs::read_to_string(folder.join(file)) {
                entries.push(Entry { path: format!("sessions/{name}/{file}"), text });
            }
        }
    }

    // The settings file (the player's choices; no card, save or path of a game).
    if let Ok(text) = fs::read_to_string(state.user_dirs.config_file()) {
        entries.push(Entry { path: "settings.toml".to_string(), text });
    }

    // TASK-180: how the game's process ended (Android's records), and the newest crash texts (tombstones, ANR traces).
    let exits = dirs.logs().join("game-exits");
    if let Ok(text) = fs::read_to_string(exits.join("game-exits.log")) {
        entries.push(Entry { path: "game-exits/game-exits.log".to_string(), text });
    }
    let traces: Vec<(SystemTime, PathBuf)> = fs::read_dir(&exits)
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "txt"))
        .map(|path| (changed(&path), path))
        .collect();
    for path in log_export::newest(traces, KEPT_FOR_EXPORT) {
        let name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
        if let Ok(text) = fs::read_to_string(&path) {
            entries.push(Entry { path: format!("game-exits/{name}"), text });
        }
    }

    entries.push(Entry { path: "system.txt".to_string(), text: log_export::system_text(&system_facts(state)) });
    Ok(entries)
}

/// The phone's facts (TASK-180); none on Windows.
#[cfg(target_os = "android")]
fn device_lines() -> Vec<String> {
    crate::android::device_lines()
}

#[cfg(not(target_os = "android"))]
fn device_lines() -> Vec<String> {
    Vec::new()
}

/// The facts of `system.txt`, read now.
fn system_facts(state: &AppState) -> SystemFacts {
    let language = crate::state::language_of(state);
    let (_, plan, cards) = gt4l_platform::gpu::run_settings(&state.config.settings, language);
    let installed = state.installed_games();
    let monitors = gt4l_platform::display::monitors()
        .into_iter()
        .map(|monitor| {
            let name = if monitor.name.is_empty() { monitor.device.clone() } else { monitor.name.clone() };
            (name, monitor.desktop.width, monitor.desktop.height, monitor.primary)
        })
        .collect();
    SystemFacts {
        windows: gt4l_platform::system::windows_version(),
        cpu: std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_default(),
        gpus: cards
            .iter()
            .map(|card| GpuFact {
                name: card.name().unwrap_or_default().to_string(),
                vendor_id: card.vendor_id,
                driver_version: card.driver_version,
                video_memory: card.dedicated_video_memory,
            })
            .collect(),
        chosen: (plan.card.as_ref().and_then(|card| card.name()).unwrap_or_default().to_string(), plan.api.clone()),
        displays: monitors,
        launcher_version: gt4l_core::LAUNCHER_VERSION.to_string(),
        install: if cfg!(target_os = "android") { "Android app".to_string() } else { gt4l_core::build_kind::BUILD.label().to_string() },
        games: games::GAMES
            .iter()
            .map(|game| (game.display_name.to_string(), game.serial.to_string(), installed.contains(game)))
            .collect(),
        selected_game: state.selected.display_name.to_string(),
        graphics: vec![
            ("graphics_api".to_string(), state.config.settings.get("graphics_api").to_string()),
            ("graphics_card".to_string(), state.config.settings.get("graphics_card").to_string()),
        ],
        device: device_lines(),
    }
}
