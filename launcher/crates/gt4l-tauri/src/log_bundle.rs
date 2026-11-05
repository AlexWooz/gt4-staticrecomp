//! Builds the log bundle of TASK-111 (rc4): the newest logs and game sessions, the settings file and the system report,
//! written as one zip. The rules (names, masking, what may go in, the zip writer) are in `gt4l_core::log_export`.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use gt4l_core::games;
use gt4l_core::log_export::{self, DisplayExtra, Entry, GpuFact, SystemFacts, KEPT_FOR_EXPORT};
use gt4l_core::logs;
use gt4l_core::shader_prepare;

use crate::state::AppState;

/// The file time of `path`, or the epoch when it cannot be read.
fn changed(path: &Path) -> SystemTime {
    fs::metadata(path).and_then(|metadata| metadata.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}

/// Writes the bundle into `folder` and returns its path. `user_name` is the Windows user name to mask.
pub fn export(state: &AppState, folder: &Path, user_name: &str, controllers: &[String]) -> Result<PathBuf, String> {
    let now = gt4l_platform::system::local_time();
    let path = folder.join(log_export::zip_name(&now));
    let entries = collect(state, controllers)?;
    let masked = log_export::masked(&entries, user_name);
    log_export::write_zip(&path, &masked).map_err(|error| format!("{}: {error}", path.display()))?;
    Ok(path)
}

/// The entries of the bundle before masking.
pub(crate) fn collect(state: &AppState, controllers: &[String]) -> Result<Vec<Entry>, String> {
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

    // TASK-170b: the shader guard files of the shader cache (variants quarantined, variants in flight when a run stopped),
    // as a readable listing. Nothing else from the cache folder goes in.
    if let Some(text) = shader_guard_listing(state.texture_location().0.shader_cache.as_deref()) {
        entries.push(Entry { path: "logs/shader-guard.txt".to_string(), text });
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
    // TASK-180: Android carries the newest 10 (each F8 report is in its session's game.log); Windows 3.
    for folder in log_export::newest(folders, gt4l_core::Platform::current().game_sessions_exported()) {
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
    // TASK-181: the racing wheel file (the mapping and force settings of the virtual Driving Force Pro).
    if let Ok(text) = fs::read_to_string(state.wheel_path()) {
        entries.push(Entry { path: "wheel.toml".to_string(), text });
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

    entries.push(Entry { path: "system.txt".to_string(), text: log_export::system_text(&system_facts(state, controllers)) + &wheel_report(state) });
    Ok(entries)
}

/// The readable listing of `quarantine.bin`, `suspects.bin` and `compiling.bin` in the shader cache `folder`, if either exists.
pub(crate) fn shader_guard_listing(folder: Option<&Path>) -> Option<String> {
    let folder = folder?;
    let quarantine = fs::read(folder.join(shader_prepare::QUARANTINE_FILE)).ok();
    let compiling = fs::read(folder.join(shader_prepare::COMPILING_FILE)).ok();
    let suspects = fs::read(folder.join(shader_prepare::SUSPECTS_FILE)).ok();
    shader_prepare::guard_listing(quarantine.as_deref(), compiling.as_deref(), suspects.as_deref())
}

/// The names of the connected pads in a `pad_status` payload (`pads[].name`), without anything that identifies a unit.
pub fn controller_names(status: &serde_json::Value) -> Vec<String> {
    status["pads"]
        .as_array()
        .map(|pads| pads.iter().filter_map(|pad| pad["name"].as_str()).map(str::trim).filter(|name| !name.is_empty()).map(str::to_string).collect())
        .unwrap_or_default()
}

/// TASK-181: the wheel lines of `system.txt` (the connected wheels the pad service saw, and whether the virtual Driving
/// Force Pro is on), appended after the other facts.
fn wheel_report(state: &AppState) -> String {
    log_export::wheel_lines(&state.wheel_facts, state.wheel.enabled)
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
fn system_facts(state: &AppState, controllers: &[String]) -> SystemFacts {
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
    let details = gt4l_platform::display::details();
    let display_extras = gt4l_platform::display::monitors()
        .iter()
        .map(|monitor| {
            let found = details.iter().find(|(device, _, _)| *device == monitor.device);
            DisplayExtra { refresh_hz: found.and_then(|(_, hertz, _)| *hertz), scale_percent: found.and_then(|(_, _, scale)| *scale) }
        })
        .collect();
    let mut machine = gt4l_platform::machine::read(state.dirs.root());
    machine.controllers = controllers.to_vec();
    // TASK-180: on Android the Vulkan devices come straight from the system loader (the Windows card list keeps only
    // what the Windows game can open), and the graphics settings of the PC do not apply.
    #[cfg(target_os = "android")]
    let (cards, plan, graphics_settings) = {
        let _ = (cards, plan);
        let devices = match gt4l_platform::system::probe_vulkan() {
            gt4l_core::system_check::VulkanProbe::Ready { devices, .. } => devices,
            _ => Vec::new(),
        };
        let chosen = devices.first().map(|device| device.name.clone()).unwrap_or_default();
        (devices, (chosen, "vulkan".to_string()), Vec::new())
    };
    #[cfg(not(target_os = "android"))]
    let graphics_settings = vec![
        ("graphics_api".to_string(), state.config.settings.get("graphics_api").to_string()),
        ("graphics_card".to_string(), state.config.settings.get("graphics_card").to_string()),
    ];
    SystemFacts {
        windows: gt4l_platform::system::windows_version(),
        cpu: std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_default(),
        machine,
        display_extras,
        #[cfg(not(target_os = "android"))]
        gpus: cards
            .iter()
            .map(|card| GpuFact {
                name: card.name().unwrap_or_default().to_string(),
                vendor_id: card.vendor_id,
                driver_version: card.driver_version,
                video_memory: card.dedicated_video_memory,
                graphics_queues: card.graphics_queues,
                api: card.vulkan_name.as_ref().map(|_| (card.vulkan_api.0, card.vulkan_api.1, card.driver.api_patch)),
                driver: card.driver.clone(),
            })
            .collect(),
        #[cfg(target_os = "android")]
        gpus: cards
            .iter()
            .map(|device| GpuFact {
                name: format!(
                    "{} (Vulkan {}.{}, device 0x{:X}, type {}, driver {}.{}.{})",
                    device.name,
                    device.api_major,
                    device.api_minor,
                    device.device_id,
                    device.device_type,
                    device.driver_version >> 22,
                    (device.driver_version >> 12) & 0x3FF,
                    device.driver_version & 0xFFF
                ),
                vendor_id: device.vendor_id,
                driver_version: device.driver_version,
                video_memory: 0,
                ..GpuFact::default()
            })
            .collect(),
        #[cfg(not(target_os = "android"))]
        chosen: (plan.card.as_ref().and_then(|card| card.name()).unwrap_or_default().to_string(), plan.api.clone()),
        #[cfg(target_os = "android")]
        chosen: plan,
        displays: monitors,
        launcher_version: gt4l_core::LAUNCHER_VERSION.to_string(),
        install: if cfg!(target_os = "android") { "Android app".to_string() } else { gt4l_core::build_kind::BUILD.label().to_string() },
        games: games::GAMES
            .iter()
            .map(|game| (game.display_name.to_string(), game.serial.to_string(), installed.contains(game)))
            .collect(),
        selected_game: state.selected.display_name.to_string(),
        graphics: graphics_settings,
        device: device_lines(),
    }
}
