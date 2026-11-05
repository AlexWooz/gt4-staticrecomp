//! TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): the launcher as the Android app's main activity.
//!
//! The same page and the same Rust core as on Windows. What Windows does with files, dialogs and processes, Android does
//! through a small Kotlin plugin of the app (`GameBridgePlugin`, gen/android): the Storage Access Framework for the disc
//! image (a persistable permission, never a copy) and for save, replay and log import or export, the browser for links,
//! and Play, which starts the game activity (SDL3, in its own `:game` process) with the launch contract: the same
//! arguments and environment the Windows launcher gives `gt4vulkan.exe`.
//!
//! Platform rules (CoreGS 2.0 only, no GSdx, update check without installer) are in `gt4l_core::settings` and the page
//! model, keyed by `gt4l_core::platform`, not here.

use std::fs::File;
use std::os::fd::FromRawFd;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Instant;

use serde::{Deserialize, Serialize};
use tauri::plugin::{PluginHandle, TauriPlugin};
use tauri::{Manager, Wry};

use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_disc::Pins;

use crate::state::{self, AppState, Shared};
use crate::webview_guard::{Guard, PageReady};

/// The Kotlin side, registered when the app starts.
static BRIDGE: OnceLock<PluginHandle<Wry>> = OnceLock::new();

fn bridge() -> Result<&'static PluginHandle<Wry>, String> {
    BRIDGE.get().ok_or_else(|| "the Android bridge is not ready".to_string())
}

fn bridge_plugin() -> TauriPlugin<Wry> {
    tauri::plugin::Builder::new("gamebridge")
        .setup(|_app, api| {
            let handle = api.register_android_plugin("io.github.moesuito.gt4recomp", "GameBridgePlugin")?;
            let _ = BRIDGE.set(handle);
            Ok(())
        })
        .build()
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PickArgs<'a> {
    mime_types: &'a [&'a str],
    /// Keep a persistable read permission (the disc image, read again at every Play).
    persist: bool,
    /// Copy the picked document into this file (imports read a plain file).
    copy_to: Option<String>,
}

/// The document the player picked.
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct Picked {
    pub uri: Option<String>,
    pub name: Option<String>,
}

#[derive(Serialize)]
struct UriArgs<'a> {
    uri: &'a str,
}

#[derive(Deserialize)]
struct Descriptor {
    fd: i32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SaveArgs<'a> {
    source: &'a str,
    name: &'a str,
    mime_type: &'a str,
}

#[derive(Deserialize)]
struct Saved {
    saved: bool,
}

#[derive(Serialize)]
struct GameArgs<'a> {
    arguments: &'a [String],
    environment: Vec<String>,
}

#[derive(Serialize)]
struct UrlArgs<'a> {
    url: &'a str,
}

#[derive(Deserialize)]
struct Done {}

#[derive(Deserialize, Default)]
struct DeviceInfo {
    #[serde(default)]
    lines: Vec<String>,
}

#[derive(Serialize)]
struct ExitsArgs<'a> {
    since: i64,
    folder: &'a str,
}

/// One end of the game's process, as Android recorded it (ApplicationExitInfo).
#[derive(Deserialize, Debug)]
#[serde(rename_all = "camelCase")]
struct GameExit {
    time: i64,
    pid: i32,
    #[serde(default)]
    process: String,
    reason: i32,
    reason_name: String,
    status: i32,
    importance: i32,
    #[serde(default)]
    description: String,
    #[serde(default)]
    pss_kb: i64,
    #[serde(default)]
    rss_kb: i64,
    #[serde(default)]
    trace_file: Option<String>,
    #[serde(default)]
    trace_error: Option<String>,
}

#[derive(Deserialize, Default)]
struct GameExits {
    #[serde(default)]
    exits: Vec<GameExit>,
}

// Logcat (liblog): the launcher's lines under GT4Recomp-launcher, as the game's under GT4Recomp-game.
#[link(name = "log")]
extern "C" {
    fn __android_log_write(priority: std::ffi::c_int, tag: *const std::ffi::c_char, text: *const std::ffi::c_char) -> std::ffi::c_int;
}

/// The phone's screen as the launcher's display model sees a monitor (TASK-180 test2, owner: 100% is the screen's
/// native output, as a 1440p monitor on the PC). MainActivity puts the current display mode in the environment before
/// the launcher starts (`GT4_ANDROID_DISPLAY=3088x1440@120`, landscape), so no call to the Java side is needed here.
pub fn screen() -> Option<(gt4l_core::display::Size, u32)> {
    let text = std::env::var("GT4_ANDROID_DISPLAY").ok()?;
    let (size, refresh) = text.split_once('@').unwrap_or((text.as_str(), "0"));
    let (width, height) = size.split_once('x')?;
    let (width, height): (u32, u32) = (width.trim().parse().ok()?, height.trim().parse().ok()?);
    let refresh = refresh.trim().parse::<f32>().map(|hertz| hertz.round() as u32).unwrap_or(0);
    (width > 0 && height > 0).then(|| (gt4l_core::display::Size::new(width.max(height), width.min(height)), refresh))
}

/// The monitors of the display model: the phone's screen, its current mode as the desktop and only mode.
pub fn monitors() -> Vec<gt4l_core::display::Monitor> {
    match screen() {
        Some((size, _)) => vec![gt4l_core::display::Monitor {
            device: "android-screen".to_string(),
            name: "Phone screen".to_string(),
            primary: true,
            desktop: size,
            modes: vec![size],
        }],
        None => Vec::new(),
    }
}

/// Writes one launcher log line to logcat (tag `GT4Recomp-launcher`).
pub fn logcat(level: gt4l_core::logs::Level, message: &str) {
    let priority = match level {
        gt4l_core::logs::Level::Info => 4,
        gt4l_core::logs::Level::Warn => 5,
        gt4l_core::logs::Level::Error => 6,
    };
    let text = std::ffi::CString::new(message.replace('\0', " ")).unwrap_or_default();
    // SAFETY: both strings are NUL-terminated and live through the call.
    unsafe {
        __android_log_write(priority, c"GT4Recomp-launcher".as_ptr(), text.as_ptr());
    }
}

/// The phone's facts for system.txt (model, Android version, SoC, memory, display, Vulkan features).
pub fn device_lines() -> Vec<String> {
    match bridge().and_then(|bridge| {
        bridge.run_mobile_plugin::<DeviceInfo>("deviceInfo", ()).map_err(|error| error.to_string())
    }) {
        Ok(info) => info.lines,
        Err(message) => vec![format!("device facts unavailable: {message}")],
    }
}

/// The folder of the game's exit records: `logs/game-exits.log`, the raw tombstones and their text.
fn exits_folder(dirs: &DataDirs) -> PathBuf {
    dirs.logs().join("game-exits")
}

/// Records the ends of the game's process that Android reported since the last call: one line each in the launcher log
/// and in `logs/game-exits/game-exits.log`, and a native crash's tombstone as text next to its raw file. The time of the
/// newest one is kept in `logs/game-exits/seen`, so each end is recorded once. Returns how many were new.
pub fn record_game_exits(dirs: &DataDirs) -> usize {
    let folder = exits_folder(dirs);
    if let Err(error) = std::fs::create_dir_all(&folder) {
        state::log_warn(dirs, &format!("game exits: {}: {error}", folder.display()));
        return 0;
    }
    let seen_path = folder.join("seen");
    let since: i64 = std::fs::read_to_string(&seen_path).ok().and_then(|text| text.trim().parse().ok()).unwrap_or(0);
    let folder_text = folder.display().to_string();
    let exits = match bridge().and_then(|bridge| {
        bridge
            .run_mobile_plugin::<GameExits>("gameExits", ExitsArgs { since, folder: &folder_text })
            .map_err(|error| error.to_string())
    }) {
        Ok(found) => found.exits,
        Err(message) => {
            state::log_warn(dirs, &format!("game exits could not be read: {message}"));
            return 0;
        }
    };
    let mut newest = since;
    for exit in &exits {
        newest = newest.max(exit.time);
        let when = gt4l_platform::system::utc_time_of(std::time::UNIX_EPOCH + std::time::Duration::from_millis(exit.time.max(0) as u64));
        let mut line = format!(
            "{} process {} ended {} UTC: {} (reason {}, status {}, importance {}), memory pss {} MB rss {} MB",
            if exit.process.ends_with(":game") { "game" } else { "launcher" },
            exit.pid,
            when.stamp(),
            exit.reason_name,
            exit.reason,
            exit.status,
            exit.importance,
            exit.pss_kb / 1024,
            exit.rss_kb / 1024
        );
        if !exit.description.is_empty() {
            line.push_str(&format!("; {}", exit.description));
        }
        if let Some(trace) = &exit.trace_file {
            line.push_str(&format!("; trace {trace}"));
            if trace.ends_with(".pb") {
                match std::fs::read(trace).map(|bytes| gt4l_core::tombstone::to_text(&bytes, 400)) {
                    Ok(Some(text)) => {
                        let text_path = PathBuf::from(trace.trim_end_matches(".pb").to_string() + ".txt");
                        if let Err(error) = std::fs::write(&text_path, text) {
                            line.push_str(&format!(" (text not written: {error})"));
                        }
                    }
                    Ok(None) => line.push_str(" (not a readable tombstone)"),
                    Err(error) => line.push_str(&format!(" (unreadable: {error})")),
                }
            }
        }
        if let Some(error) = &exit.trace_error {
            line.push_str(&format!("; trace unreadable: {error}"));
        }
        // A failure: a crash (Java 4, native 5), not responding (6), an initialization failure (7), low memory (3),
        // excessive resource use (9), or the game's own exit with a status other than 0 (android_main.cpp ends the process
        // with the game's result). The rest (the activity closed, the player stopped the app) is a normal end.
        const EXIT_SELF: i32 = 1;
        let failed = matches!(exit.reason, 3..=7 | 9) || (exit.reason == EXIT_SELF && exit.status != 0);
        if failed {
            state::log_error(dirs, &line);
        } else {
            state::log(dirs, &line);
        }
        let mut file = std::fs::OpenOptions::new().create(true).append(true).open(folder.join("game-exits.log"));
        if let Ok(file) = file.as_mut() {
            use std::io::Write;
            let _ = writeln!(file, "{line}");
        }
    }
    if newest != since {
        let _ = std::fs::write(&seen_path, newest.to_string());
    }
    exits.len()
}

/// Lets the player pick a document. `None` when cancelled.
pub fn pick_document(mime_types: &[&str], persist: bool, copy_to: Option<&Path>) -> Result<Option<Picked>, String> {
    let picked: Picked = bridge()?
        .run_mobile_plugin(
            "pickDocument",
            PickArgs { mime_types, persist, copy_to: copy_to.map(|path| path.display().to_string()) },
        )
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("document picker failed: {error}"));
            format!("document picker: {error}")
        })?;
    logcat(
        gt4l_core::logs::Level::Info,
        &format!(
            "document picker: {} ({}), persistable permission {}",
            picked.uri.as_deref().unwrap_or("cancelled"),
            picked.name.as_deref().unwrap_or("-"),
            persist
        ),
    );
    Ok(picked.uri.is_some().then_some(picked))
}

/// Opens a document for reading: the descriptor the system hands the app for a permission it holds.
pub fn open_document(uri: &str) -> std::io::Result<File> {
    let descriptor: Descriptor = bridge()
        .map_err(std::io::Error::other)?
        .run_mobile_plugin("openFd", UriArgs { uri })
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("cannot open {uri}: {error}"));
            std::io::Error::new(std::io::ErrorKind::PermissionDenied, error.to_string())
        })?;
    if descriptor.fd < 0 {
        return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("{uri} cannot be opened")));
    }
    // SAFETY: the Kotlin side detached this descriptor from its ParcelFileDescriptor for us; it is ours to own and close.
    Ok(unsafe { File::from_raw_fd(descriptor.fd) })
}

/// Lets the player save `source` as a new document. False when cancelled.
pub fn save_document(source: &Path, name: &str, mime_type: &str) -> Result<bool, String> {
    let saved: Saved = bridge()?
        .run_mobile_plugin("saveDocument", SaveArgs { source: &source.display().to_string(), name, mime_type })
        .map_err(|error| format!("save as: {error}"))?;
    Ok(saved.saved)
}

/// Starts the game activity (`:game` process) with the launch contract.
pub fn start_game(arguments: &[String], environment: &[(String, String)]) -> Result<(), String> {
    let environment = environment.iter().map(|(name, value)| format!("{name}={value}")).collect();
    let _: Done = bridge()?
        .run_mobile_plugin("startGame", GameArgs { arguments, environment })
        .map_err(|error| format!("game start: {error}"))?;
    Ok(())
}

/// Opens a web page in the browser (project links, the download page of a newer version).
pub fn open_url(url: &str) -> Result<(), String> {
    let _: Done = bridge()?.run_mobile_plugin("openUrl", UrlArgs { url }).map_err(|error| format!("{url}: {error}"))?;
    Ok(())
}

/// A file in the app's cache, for imports and exports that pass through a plain file.
pub fn cache_file(app: &tauri::AppHandle, name: &str) -> Result<PathBuf, String> {
    let folder = app.path().app_cache_dir().map_err(|error| error.to_string())?.join("transfer");
    std::fs::create_dir_all(&folder).map_err(|error| format!("{}: {error}", folder.display()))?;
    let safe: String = name.chars().map(|c| if c.is_alphanumeric() || ".-_ ".contains(c) { c } else { '_' }).collect();
    Ok(folder.join(if safe.is_empty() { "file".to_string() } else { safe }))
}

/// The app: the same commands as the desktop launcher (minus the tray), the bridge, and the data in app storage.
pub fn run() {
    let started = Instant::now();
    gt4l_disc::source::set_document_opener(open_document);
    let result = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(bridge_plugin())
        .setup(move |app| {
            // App-specific storage: removed with the app, never shared with other apps. Large data (the prepared game
            // files, sessions, logs) under data\, the player's things (settings, controls, the save library) under user\.
            let root = app.path().app_data_dir()?;
            let dirs = DataDirs::new(root.join("data"));
            std::fs::create_dir_all(dirs.root())?;
            let header = vec![
                format!("GT4Recomp {} (Android)", gt4l_core::LAUNCHER_VERSION),
                format!("Date: {}", gt4l_core::logs::LocalTime::stamp(&gt4l_platform::system::local_time())),
            ];
            if let Err(message) = crate::session_log::start(&dirs, &header) {
                eprintln!("GT4Recomp cannot open its log: {message}");
            }
            let user = UserDirs::new(root.join("user"), UserRootSource::AppData);
            let pins = Pins::embedded()?;
            let app_state = AppState::load_with(dirs.clone(), pins, user.clone(), None, root.join("install"))?;
            state::log(&dirs, &format!("GT4Recomp {} (Android) starting, data in {}", gt4l_core::LAUNCHER_VERSION, root.display()));
            // The phone's facts and the game's last ends, read off the main thread (the bridge answers through it).
            let start_dirs = dirs.clone();
            std::thread::spawn(move || {
                for line in device_lines() {
                    state::log(&start_dirs, &line);
                }
                record_game_exits(&start_dirs);
            });
            let shared: Shared = Arc::new(Mutex::new(app_state));
            app.manage(shared);
            app.manage(Arc::new(Guard { page: PageReady::default(), dirs, user_root: user.root().to_path_buf(), started }));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            crate::commands::app_state,
            crate::commands::get_settings_table,
            crate::commands::set_setting,
            crate::commands::reset_gs_settings_to_defaults,
            crate::renderer_switch::gsdx_download_needed,
            crate::renderer_switch::download_gsdx,
            crate::renderer_switch::install_gsdx_archive,
            crate::renderer_switch::coregs20_offer,
            crate::renderer_switch::use_coregs20,
            crate::setup::setup_choose_disc,
            crate::setup::setup_start,
            crate::setup::setup_cancel,
            crate::commands::prepare_shaders,
            crate::commands::cancel_shader_prepare,
            crate::commands::play,
            crate::commands::first_start_hint,
            crate::commands::stop_game,
            crate::commands::saves_list,
            crate::commands::saves_import,
            crate::commands::saves_activate,
            crate::commands::saves_delete,
            crate::commands::saves_new,
            crate::commands::saves_export,
            crate::commands::saves_rename,
            crate::commands::replays_list,
            crate::commands::replays_import,
            crate::commands::replays_delete,
            crate::webview_guard::ui_ready,
            crate::webview_guard::ui_log,
            crate::commands::get_controls,
            crate::commands::pad_status,
            crate::commands::capture_binding,
            crate::commands::cancel_capture,
            crate::commands::reset_binding,
            crate::commands::assign_binding,
            crate::commands::reset_all_bindings,
            crate::wheel_page::get_wheel_config,
            crate::wheel_page::set_wheel_setting,
            crate::wheel_page::apply_wheel_preset,
            crate::wheel_page::assign_wheel_binding,
            crate::wheel_page::reset_wheel_binding,
            crate::wheel_page::reset_wheel_config,
            crate::wheel_page::capture_wheel_binding,
            crate::wheel_page::cancel_wheel_capture,
            crate::wheel_page::test_wheel_ffb,
            crate::wheel_page::wheel_watch,
            crate::wheel_page::get_wheel_session_check,
            crate::wheel_page::list_wheel_presets,
            crate::wheel_page::save_wheel_preset,
            crate::wheel_page::delete_wheel_preset,
            crate::commands::about_info,
            crate::commands::release_notes,
            crate::commands::open_link,
            crate::commands::open_log_folder,
            crate::commands::open_latest_log,
            crate::commands::export_logs,
            crate::commands::open_export_folder,
            crate::commands::open_game_files_folder,
            crate::commands::open_player_folder,
            crate::commands::open_saves_folder,
            crate::commands::open_replays_folder,
            crate::commands::open_textures_folder,
            crate::commands::textures_state,
            crate::commands::choose_textures_folder,
            crate::commands::reset_textures_folder,
            crate::commands::games_list,
            crate::commands::select_game,
            crate::commands::games_remove,
            crate::commands::ra_status,
            crate::commands::ra_login,
            crate::commands::ra_logout,
            crate::commands::ra_set_options,
            crate::updates::check_updates,
            crate::updates::list_launcher_versions,
            crate::updates::install_launcher_version,
            crate::updates::apply_update,
            crate::updates::skip_update,
        ])
        .run(tauri::generate_context!());
    if let Err(error) = result {
        eprintln!("GT4Recomp could not start: {error}");
    }
}

/// Play on Android: the same checks and launch plan as `commands::prepare_play` and `game::start` on Windows, with CoreGS
/// 2.0 (the only renderer here) and no monitor list (the game sizes itself from its window: the aspect correction of
/// TASK-079/080 follows the screen as GT4_DISPLAY_ASPECT=auto does on the PC). The disc stays a document: the game
/// activity opens it itself with the app's permission.
pub fn play(shared: &Shared) -> Result<(), String> {
    use gt4l_core::launch::{self, LaunchInputs};
    use gt4l_core::messages;
    use gt4l_disc::setup::Installed;

    // The previous run's end first: a crash is in the launcher log before the next start.
    let data_dirs = state::lock(shared).dirs.clone();
    record_game_exits(&data_dirs);
    let state = state::lock(shared);
    let language = state::language_of(&state);
    let disc = match state.installed() {
        Installed::Ready { image, .. } => image,
        Installed::NotSetUp => return Err(messages::text("game.set_up_first", language).into()),
        _ => return Err(messages::render("setup.game_files_missing_reason", language, &[("reason", "")])),
    };
    let game = state.selected;
    let dirs = state.game_dirs(game);
    let settings = state.config.settings.clone();
    let elf = launch::game_elf(game, &dirs.prepared());
    let card = dirs.runtime_card();
    if let Some(parent) = card.parent() {
        std::fs::create_dir_all(parent).map_err(|error| format!("runtime folder: {error}"))?;
    }
    crate::saves::prepare_card(state.library_of(game), &card, language)
        .inspect_err(|message| state::log_error(&dirs, &format!("play: the memory card could not be prepared: {message}")))?;
    state::log(&dirs, &format!("play: card ready at {}", card.display()));
    let seconds = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    // Named as on Windows (session-<seconds>), so the log bundle takes its game.log.
    let session_dir = dirs.session(&format!("session-{seconds}"));
    std::fs::create_dir_all(&session_dir).map_err(|error| format!("session folder: {error}"))?;
    let (textures, _) = state.texture_location();
    let inputs = LaunchInputs { game_executable: Path::new("libgt4.so"), elf: &elf, disc: &disc, card: &card, session_dir: &session_dir };
    // TASK-180 test2: the output is the phone's screen (100% = its native multiplier, as a monitor on the PC). The
    // decision is logged, so a scale seen in the game can be traced to the setting it came from.
    let monitors = monitors();
    let display = gt4l_core::display::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
    let scale = launch::session_plan(&settings, display.as_ref(), false);
    state::log(
        &dirs,
        &format!(
            "play: render scale {}% gives {}x ({}x{} internal) for the screen {} ({})",
            settings.get("render_scale"),
            scale.multiplier,
            scale.internal_width,
            scale.internal_height,
            display.as_ref().map(|target| target.output.text()).unwrap_or_else(|| "unknown, 1920x1080 assumed".to_string()),
            screen().map(|(_, hertz)| format!("{hertz} Hz")).unwrap_or_else(|| "refresh unknown".to_string()),
        ),
    );
    let mut plan = launch::plan_coregs20_launch(&inputs, &settings, display.as_ref(), false, &textures, &[]);
    let controls_path = state.user_dirs.controls_file();
    if controls_path.exists() {
        plan.environment.push(("GT4_CONTROLS_FILE".into(), controls_path.display().to_string()));
    }
    if let Some(choice) = launch::controller_environment(&settings) {
        plan.environment.push(("GT4_PAD".into(), choice));
    }
    let mappings = state.user_dirs.config_dir().join(launch::GAMEPAD_MAPPINGS_FILE);
    if mappings.exists() {
        plan.environment.push(("SDL_GAMECONTROLLERCONFIG_FILE".into(), mappings.display().to_string()));
    }
    // RetroAchievements needs the desktop HTTP and overlay layers, not built for Android yet.
    plan.environment.push(("GT4_RA_ENABLED".into(), "0".into()));
    plan.environment.push(("GT4_ANDROID_LOG".into(), plan.log_path.display().to_string()));
    plan.environment.push(("GT4_ANDROID_CWD".into(), plan.working_directory.display().to_string()));
    state::log(&dirs, &format!("game start (Android): session {}", session_dir.display()));
    state::log(&dirs, &format!("game start: arguments {}", plan.arguments.join(" ")));
    for (name, value) in &plan.environment {
        state::log(&dirs, &format!("game start: {name}={value}"));
    }
    drop(state);
    start_game(&plan.arguments, &plan.environment)
}
