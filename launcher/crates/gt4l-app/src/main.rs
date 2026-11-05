//! GT4Recomp launcher: first-run setup, home, settings and the controls page.
//! The UI runs on the Slint event loop; long jobs (hashing, rebuilding, watching the game) run on
//! worker threads and report back through `slint::invoke_from_event_loop`.

mod game;
mod pad;
mod process;
mod recycle;
mod rows;
mod saves;

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use gt4controls::{self as controls_file, Controls, Curve, TriggerTuning, CONTROLS_FILE_NAME};
use gt4l_core::config::{self, ConfigFile};
use gt4l_core::exit::{self, Outcome};
use gt4l_core::paths::{self, DataDirs, GAME_EXECUTABLE};
use gt4l_core::LAUNCHER_VERSION;
use gt4l_disc::setup::{self as disc_setup, SetupError, SetupRequest, Stage};
use gt4l_disc::Pins;
use gt4saves::library::Library;
use slint::{ModelRc, SharedString, VecModel};

slint::include_modules!();

/// Order of the 16 Controls-page hotspots; the index matches the Slint `captions` array.
const CONTROL_INPUTS: [&str; 16] = [
    "dpad_up", "dpad_right", "dpad_down", "dpad_left", "cross", "circle", "square", "triangle", "l1", "r1",
    "l2", "r2", "l3", "r3", "select", "start",
];

/// Shared state. Worker threads hold clones of the `Arc`, so every field must be `Send`.
struct App {
    dirs: DataDirs,
    pins: Pins,
    config: ConfigFile,
    controls: Controls,
    controls_selected: Option<usize>,
    capturing: bool,
    controls_status: String,
    program_dir: PathBuf,
    game_child: Option<Arc<Mutex<Option<std::process::Child>>>>,
    setup_cancel: Arc<AtomicBool>,
    settings_page: i32,
    page_rows: Vec<rows::RowData>,
    library_root: PathBuf,
    library: Library,
    /// Controller state when the current capture started; a control already held does not bind.
    pad_baseline: Option<pad::PadSample>,
    /// Result of the last import, delete or activate, shown on the Saves and Replays pages.
    saves_status: String,
}

type Shared = Arc<Mutex<App>>;

fn main() -> Result<(), slint::PlatformError> {
    if !process::claim_single_instance() {
        return Ok(());
    }
    let dirs = DataDirs::from_environment().expect("LOCALAPPDATA is not set and GT4RECOMP_DATA_DIR is not given");
    let _ = fs::create_dir_all(dirs.root());
    log(&dirs, &format!("GT4Recomp {LAUNCHER_VERSION} starting"));
    if let Err(message) = process::join_kill_on_close_job() {
        log(&dirs, &format!("the game would not end with the launcher: {message}"));
    }

    let (config, warnings) = config::load(&dirs.config_file());
    for warning in &warnings {
        log(&dirs, &format!("settings: {warning}"));
    }
    let controls = match controls_file::load(&dirs.root().join(CONTROLS_FILE_NAME)) {
        Ok(Some(controls)) => controls,
        Ok(None) => Controls::default(),
        Err(message) => {
            log(&dirs, &format!("controls: {message}; built-in bindings used"));
            Controls::default()
        }
    };
    let (library_root, library) =
        saves::open(dirs.root()).expect("the save library folder can be created under the data folder");
    let pins = Pins::embedded().expect("embedded pins are valid (build error otherwise)");
    let program_dir = std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."));

    let disc_ready = config.disc_path.as_ref().is_some_and(|disc| disc.exists())
        && disc_setup::verify_prepared(&dirs.prepared(), &pins).is_ok();
    let home_message = match (&config.disc_path, disc_ready) {
        (Some(_), false) => "Game files are not ready. Prepare them again from setup.".to_string(),
        _ => String::new(),
    };
    let app: Shared = Arc::new(Mutex::new(App {
        dirs,
        pins,
        config,
        controls,
        controls_selected: None,
        capturing: false,
        controls_status: String::new(),
        program_dir,
        game_child: None,
        setup_cancel: Arc::new(AtomicBool::new(false)),
        settings_page: 0,
        page_rows: Vec::new(),
        library_root,
        library,
        pad_baseline: None,
        saves_status: String::new(),
    }));

    let ui = AppWindow::new()?;
    ui.set_version_text(format!("Version {LAUNCHER_VERSION}").into());
    ui.set_about_text(rows::about_text(LAUNCHER_VERSION).into());
    ui.set_screen(if disc_ready { 1 } else { 0 });
    ui.set_home_message(home_message.into());
    ui.set_setup_steps(steps_model(&[
        ("Disc", "", 0),
        ("Checking disc", "", 0),
        ("Preparing game files", "about a minute", 0),
        ("Memory card", "Created on first save", 0),
    ]));
    refresh_settings(&ui, &app);
    refresh_controls(&ui, &app);
    refresh_saves(&ui, &app);

    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_import_saves(move || {
            let Some(ui) = weak.upgrade() else { return };
            let picked = rfd::FileDialog::new()
                .set_title("Choose a save or memory card file")
                .add_filter("Save or card", &["psu", "ps2", "mcd", "bin", "card"])
                .pick_file();
            if let Some(path) = picked {
                let status = {
                    let state = lock(&app);
                    match saves::import_path(&state.library, &path) {
                        Ok(report) => import_summary(&report),
                        Err(error) => format!("Not imported: {error}"),
                    }
                };
                set_saves_status(&app, &status);
                refresh_saves(&ui, &app);
            }
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_activate_save(move |name| {
            let Some(ui) = weak.upgrade() else { return };
            let status = match saves::activate(&lock(&app).library, &name) {
                Ok(()) => "The chosen save will be used the next time you play.".to_string(),
                Err(error) => format!("Not changed: {error}"),
            };
            set_saves_status(&app, &status);
            refresh_saves(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_delete_item(move |name| {
            let Some(ui) = weak.upgrade() else { return };
            let status = {
                let state = lock(&app);
                match find_item(&state.library, &name) {
                    Some(item) => match saves::delete(&state.library_root, &item) {
                        Ok(()) => format!("{} moved to the Recycle Bin.", item.label),
                        Err(error) => error,
                    },
                    None => "That item is not in the library any more.".to_string(),
                }
            };
            set_saves_status(&app, &status);
            refresh_saves(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_choose_disc(move || {
            let Some(ui) = weak.upgrade() else { return };
            let picked = rfd::FileDialog::new()
                .set_title("Choose your Gran Turismo 4 disc image")
                .add_filter("Disc image", &["iso", "bin", "chd", "cso", "zso"])
                .pick_file();
            if let Some(path) = picked {
                check_disc_choice(&ui, &app, &path);
            }
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_start_setup(move || {
            if let Some(ui) = weak.upgrade() {
                start_setup(&ui, &app);
            }
        });
    }
    {
        let app = app.clone();
        ui.on_cancel_setup(move || {
            lock(&app).setup_cancel.store(true, Ordering::Relaxed);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_play(move || {
            if let Some(ui) = weak.upgrade() {
                play(&ui, &app);
            }
        });
    }
    {
        let app = app.clone();
        ui.on_stop_game(move || {
            let child = lock(&app).game_child.clone();
            if let Some(child) = child {
                if let Ok(mut guard) = child.lock() {
                    if let Some(process) = guard.as_mut() {
                        let _ = process.kill();
                    }
                }
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_open_settings(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_screen(2);
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_close_settings(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_screen(1);
            }
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_select_settings_page(move |index| {
            if let Some(ui) = weak.upgrade() {
                lock(&app).settings_page = index;
                ui.set_settings_page(index);
                refresh_settings(&ui, &app);
            }
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_setting_choice(move |index, label| {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                if let Some(row) = state.page_rows.get(index as usize).cloned() {
                    if let Some(value) = rows::value_for_label(row.id, &label) {
                        let _ = state.config.settings.set(row.id, value);
                    }
                    save_config(&state);
                }
            }
            refresh_settings(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_setting_toggled(move |index, on| {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                if let Some(row) = state.page_rows.get(index as usize).cloned() {
                    let _ = state.config.settings.set(row.id, if on { "on" } else { "off" });
                    save_config(&state);
                }
            }
            refresh_settings(&ui, &app);
        });
    }
    {
        let app = app.clone();
        ui.on_open_log_folder(move || {
            let logs = lock(&app).dirs.logs();
            let _ = fs::create_dir_all(&logs);
            let _ = std::process::Command::new("explorer").arg(logs).spawn();
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_dismiss_message(move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_home_message(SharedString::new());
            }
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_controls_select(move |index| {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                state.controls_selected = usize::try_from(index).ok().filter(|index| *index < CONTROL_INPUTS.len());
                state.capturing = false;
                state.pad_baseline = None;
                state.controls_status.clear();
            }
            refresh_controls(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_controls_start_capture(move || {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                state.capturing = state.controls_selected.is_some();
                state.pad_baseline = if state.capturing { pad::read_first_connected() } else { None };
            }
            refresh_controls(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_controls_cancel_capture(move || {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                state.capturing = false;
                state.pad_baseline = None;
            }
            refresh_controls(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_controls_capture_key(move |text| {
            let Some(ui) = weak.upgrade() else { return };
            capture_key(&app, &text);
            refresh_controls(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_controls_reset(move || {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                state.controls = Controls::default();
                state.capturing = false;
                state.controls_status = "Controls reset to the built-in bindings.".into();
                save_controls(&state);
            }
            refresh_controls(&ui, &app);
        });
    }
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        ui.on_trigger_changed(move |trigger, deadzone, curve| {
            let Some(ui) = weak.upgrade() else { return };
            {
                let mut state = lock(&app);
                let input = if trigger == 0 { "l2" } else { "r2" };
                let curve = match curve {
                    1 => Curve::Quadratic,
                    2 => Curve::Cubic,
                    3 => Curve::SCurve,
                    4 => Curve::SmoothPower,
                    5 => Curve::Sublinear,
                    6 => Curve::Logarithmic,
                    _ => Curve::Linear,
                };
                match state.controls.set_trigger_tuning(input, TriggerTuning { deadzone, curve }) {
                    Ok(()) => save_controls(&state),
                    Err(error) => state.controls_status = error.to_string(),
                }
            }
            refresh_controls(&ui, &app);
        });
    }

    // Polls the controller while a capture waits for a button, trigger or stick.
    let pad_timer = slint::Timer::default();
    {
        let (weak, app) = (ui.as_weak(), app.clone());
        pad_timer.start(slint::TimerMode::Repeated, Duration::from_millis(30), move || {
            if let Some(ui) = weak.upgrade() {
                poll_pad(&ui, &app);
            }
        });
    }

    #[cfg(feature = "visual-check")]
    apply_visual_check(&ui, &app);

    ui.run()
}

/// Development hook for the visual pass (`--features visual-check` only): opens a page directly.
/// `GT4RECOMP_VISUAL_SCREEN` is 0 setup, 1 home, 2 settings; `GT4RECOMP_VISUAL_PAGE` is the settings page.
#[cfg(feature = "visual-check")]
fn apply_visual_check(ui: &AppWindow, app: &Shared) {
    if let Some(screen) = std::env::var("GT4RECOMP_VISUAL_SCREEN").ok().and_then(|value| value.parse().ok()) {
        ui.set_screen(screen);
    }
    if let Some(page) = std::env::var("GT4RECOMP_VISUAL_PAGE").ok().and_then(|value| value.parse::<i32>().ok()) {
        lock(app).settings_page = page;
        ui.set_settings_page(page);
        refresh_settings(ui, app);
    }
}

fn lock(app: &Shared) -> MutexGuard<'_, App> {
    match app.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    }
}

fn log(dirs: &DataDirs, message: &str) {
    let seconds = SystemTime::now().duration_since(UNIX_EPOCH).map(|value| value.as_secs()).unwrap_or(0);
    let _ = fs::create_dir_all(dirs.logs());
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(dirs.logs().join("launcher.log")) {
        let _ = writeln!(file, "[{seconds}] {message}");
    }
}

fn save_config(state: &App) {
    if let Err(error) = config::save(&state.dirs.config_file(), &state.config) {
        log(&state.dirs, &format!("settings not saved: {error}"));
    }
}

fn save_controls(state: &App) {
    let path = state.dirs.root().join(CONTROLS_FILE_NAME);
    if let Err(error) = controls_file::save(&path, &state.controls) {
        log(&state.dirs, &format!("controls not saved: {error}"));
    }
}

fn steps_model(steps: &[(&str, &str, i32)]) -> ModelRc<SetupStep> {
    let items: Vec<SetupStep> = steps
        .iter()
        .map(|(label, status, state)| SetupStep {
            label: (*label).into(),
            status: (*status).into(),
            state: *state,
        })
        .collect();
    ModelRc::new(VecModel::from(items))
}

fn set_saves_status(app: &Shared, status: &str) {
    lock(app).saves_status = status.to_string();
}

/// Short sentence about what an import did, for the Saves and Replays pages.
fn import_summary(report: &gt4saves::library::Report) -> String {
    let mut parts = Vec::new();
    if !report.added.is_empty() {
        parts.push(format!("Added {}.", report.added.join(", ")));
    }
    if !report.unchanged.is_empty() {
        parts.push("Already in the library.".to_string());
    }
    if !report.skipped.is_empty() {
        parts.push(format!("Skipped: {}.", report.skipped.join("; ")));
    }
    if parts.is_empty() {
        parts.push("Nothing to import.".to_string());
    }
    parts.join(" ")
}

/// Finds a library item by its name, among game saves first.
fn find_item(library: &Library, name: &str) -> Option<saves::Item> {
    let all = saves::saves(library).ok()?.into_iter().chain(saves::replays(library).ok()?);
    all.into_iter().find(|item| item.name == name)
}

fn save_rows(items: &[saves::Item]) -> ModelRc<SaveRow> {
    let rows: Vec<SaveRow> = items
        .iter()
        .map(|item| SaveRow { name: item.name.clone().into(), label: item.label.clone().into(), active: item.active })
        .collect();
    ModelRc::new(VecModel::from(rows))
}

fn refresh_saves(ui: &AppWindow, app: &Shared) {
    let state = lock(app);
    let saves_list = saves::saves(&state.library).unwrap_or_default();
    let replay_list = saves::replays(&state.library).unwrap_or_default();
    let status = state.saves_status.clone();
    drop(state);
    ui.set_save_rows(save_rows(&saves_list));
    ui.set_replay_rows(save_rows(&replay_list));
    ui.set_library_message(status.into());
}

fn refresh_settings(ui: &AppWindow, app: &Shared) {
    let mut state = lock(app);
    let page_rows = match rows::page_for_index(state.settings_page) {
        Some(page) => rows::rows_for_page(&state.config.settings, page),
        None => Vec::new(),
    };
    let model: Vec<SettingRow> = page_rows
        .iter()
        .map(|row| SettingRow {
            id: row.id.into(),
            group_header: row.group_header.clone().into(),
            title: row.title.into(),
            info: row.info.into(),
            is_choice: row.is_choice,
            choices: ModelRc::new(VecModel::from(
                row.choices.iter().map(|label| SharedString::from(label.as_str())).collect::<Vec<_>>(),
            )),
            choice_index: row.choice_index,
            toggle_on: row.toggle_on,
            enabled: row.enabled,
        })
        .collect();
    state.page_rows = page_rows;
    drop(state);
    ui.set_rows(ModelRc::new(VecModel::from(model)));
}

/// Short label of the first source, shown inside a hotspot.
fn short_label(sources: &[String]) -> String {
    let Some(first) = sources.first() else {
        return "-".into();
    };
    let name = first.split_once(':').map_or(first.as_str(), |(_, name)| name);
    match name {
        "LThumb" => "L3".into(),
        "RThumb" => "R3".into(),
        "Back" => "Sel".into(),
        "DpadUp" => "Up".into(),
        "DpadDown" => "Down".into(),
        "DpadLeft" => "Left".into(),
        "DpadRight" => "Right".into(),
        "Backspace" => "Bksp".into(),
        other => other.into(),
    }
}

fn describe(sources: &[String]) -> String {
    if sources.is_empty() {
        return "Not bound".into();
    }
    sources
        .iter()
        .map(|source| {
            let base = source.split(" | ").next().unwrap_or(source);
            match base.split_once(':') {
                Some(("xinput", name)) => format!("Controller: {name}"),
                Some(("key", name)) => format!("Keyboard: {name}"),
                _ => base.to_string(),
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn refresh_controls(ui: &AppWindow, app: &Shared) {
    let state = lock(app);
    let captions: Vec<SharedString> = CONTROL_INPUTS
        .iter()
        .map(|input| SharedString::from(short_label(state.controls.sources(input).unwrap_or(&[]))))
        .collect();
    let detail = match state.controls_selected {
        Some(index) => {
            let input = CONTROL_INPUTS[index];
            format!("{}\n{}", pretty_name(input), describe(state.controls.sources(input).unwrap_or(&[])))
        }
        None => "Click a button on the controller to change it.".to_string(),
    };
    let l2 = state.controls.trigger_tuning("l2");
    let r2 = state.controls.trigger_tuning("r2");
    let capturing = state.capturing;
    let status = state.controls_status.clone();
    let selected = state.controls_selected.map_or(-1, |index| index as i32);
    drop(state);

    ui.set_controls_captions(ModelRc::new(VecModel::from(captions)));
    ui.set_controls_selected(selected);
    ui.set_controls_detail(detail.into());
    ui.set_controls_capturing(capturing);
    ui.set_controls_status(status.into());
    ui.set_l2_deadzone(l2.deadzone);
    ui.set_l2_curve(curve_index(l2.curve));
    ui.set_l2_deadzone_text(format!("{:.0}%", l2.deadzone * 100.0).into());
    ui.set_r2_deadzone(r2.deadzone);
    ui.set_r2_curve(curve_index(r2.curve));
    ui.set_r2_deadzone_text(format!("{:.0}%", r2.deadzone * 100.0).into());
}

fn curve_index(curve: Curve) -> i32 {
    match curve {
        Curve::Linear => 0,
        Curve::Quadratic => 1,
        Curve::Cubic => 2,
        Curve::SCurve => 3,
        Curve::SmoothPower => 4,
        Curve::Sublinear => 5,
        Curve::Logarithmic => 6,
    }
}

fn pretty_name(input: &str) -> &'static str {
    match input {
        "dpad_up" => "D-pad up",
        "dpad_right" => "D-pad right",
        "dpad_down" => "D-pad down",
        "dpad_left" => "D-pad left",
        "cross" => "Cross",
        "circle" => "Circle",
        "square" => "Square",
        "triangle" => "Triangle",
        "l1" => "L1",
        "r1" => "R1",
        "l2" => "L2 (trigger, pressure)",
        "r2" => "R2 (trigger, pressure)",
        "l3" => "L3 (left stick click)",
        "r3" => "R3 (right stick click)",
        "select" => "Select",
        _ => "Start",
    }
}

/// Maps the key text sent by the UI to a key name the controls file accepts.
fn key_name(text: &str) -> Option<String> {
    let mut characters = text.chars();
    if let (Some(character), None) = (characters.next(), characters.next()) {
        if character.is_ascii_alphanumeric() {
            return Some(character.to_ascii_uppercase().to_string());
        }
    }
    match text {
        "Enter" | "Backspace" | "Tab" | "Space" | "Up" | "Down" | "Left" | "Right" => Some(text.to_string()),
        _ => None,
    }
}

fn capture_key(app: &Shared, text: &str) {
    let mut state = lock(app);
    if !state.capturing {
        return;
    }
    state.capturing = false;
    state.pad_baseline = None;
    let Some(key) = key_name(text) else {
        state.controls_status = "That key cannot be used. Try a letter, a number, an arrow or Enter.".into();
        return;
    };
    bind(&mut state, "key:", &format!("key:{key}"), &key);
}

/// Checks the controls for a newly pressed controller control while a capture waits for one.
fn poll_pad(ui: &AppWindow, app: &Shared) {
    let captured = {
        let state = lock(app);
        if !state.capturing || state.controls_selected.is_none() {
            return;
        }
        let Some(baseline) = state.pad_baseline else { return };
        let Some(now) = pad::read_first_connected() else { return };
        pad::captured_source(&baseline, &now, pad::Target::Button)
    };
    let Some(source) = captured else { return };
    {
        let mut state = lock(app);
        state.capturing = false;
        state.pad_baseline = None;
        let label = source.trim_start_matches("xinput:").to_string();
        bind(&mut state, "xinput:", &source, &label);
    }
    refresh_controls(ui, app);
}

/// Replaces the sources of one kind (`key:` or `xinput:`) on the selected input and keeps the others.
fn bind(state: &mut App, prefix: &str, source: &str, label: &str) {
    let Some(index) = state.controls_selected else { return };
    let input = CONTROL_INPUTS[index];
    let mut sources: Vec<String> = state
        .controls
        .sources(input)
        .unwrap_or(&[])
        .iter()
        .filter(|existing| !existing.starts_with(prefix))
        .cloned()
        .collect();
    sources.push(source.to_string());
    match state.controls.set_sources(input, sources) {
        Ok(()) => {
            state.controls_status = format!("{} is now bound to {label}.", pretty_name(input));
            save_controls(state);
        }
        Err(error) => state.controls_status = error.to_string(),
    }
}

fn check_disc_choice(ui: &AppWindow, app: &Shared, path: &Path) {
    let pins = lock(app).pins.clone();
    match disc_setup::precheck(path, &pins) {
        Ok(summary) => {
            {
                let mut state = lock(app);
                state.config.disc_path = Some(path.to_path_buf());
                save_config(&state);
            }
            ui.set_disc_name(format!("{}  ·  v{}  ·  {}", summary.serial, summary.version, path.display()).into());
            ui.set_setup_has_disc(true);
            ui.set_setup_message(SharedString::new());
            ui.set_setup_steps(steps_model(&[
                ("Disc", &format!("{} · v{}", summary.serial, summary.version), 2),
                ("Checking disc", "", 0),
                ("Preparing game files", "about a minute", 0),
                ("Memory card", "Created on first save", 0),
            ]));
        }
        Err(error) => {
            ui.set_setup_has_disc(false);
            ui.set_disc_name(SharedString::new());
            ui.set_setup_message(error.user_message().into());
            log(&lock(app).dirs, &format!("disc rejected: {error}"));
        }
    }
}

fn start_setup(ui: &AppWindow, app: &Shared) {
    let (disc, pins, dirs, cancel) = {
        let state = lock(app);
        let Some(disc) = state.config.disc_path.clone() else {
            ui.set_setup_message("Choose your disc image first.".into());
            return;
        };
        (disc, state.pins.clone(), state.dirs.clone(), state.setup_cancel.clone())
    };
    cancel.store(false, Ordering::Relaxed);
    ui.set_setup_busy(true);
    ui.set_setup_message(SharedString::new());
    let weak = ui.as_weak();
    let app = app.clone();
    std::thread::spawn(move || {
        let prepared = dirs.prepared();
        let staging = dirs.prepared_staging();
        let mut last_percent = -1;
        let progress_weak = weak.clone();
        let result = disc_setup::run(
            &SetupRequest {
                disc: &disc,
                prepared: &prepared,
                staging: &staging,
                pins: &pins,
                launcher_version: LAUNCHER_VERSION,
                cancel: &cancel,
            },
            &mut |stage, fraction| {
                let percent = (fraction * 100.0) as i32;
                if stage == Stage::Checking && percent == last_percent {
                    return;
                }
                last_percent = percent;
                let weak = progress_weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(ui) = weak.upgrade() else { return };
                    let checking = if stage == Stage::Checking { format!("{percent}%") } else { "Done".into() };
                    let checking_state = if stage == Stage::Checking { 1 } else { 2 };
                    let preparing_state = if stage == Stage::Preparing { 1 } else { 0 };
                    ui.set_setup_steps(steps_model(&[
                        ("Disc", "SCUS-97328", 2),
                        ("Checking disc", &checking, checking_state),
                        ("Preparing game files", "about a minute", preparing_state),
                        ("Memory card", "Created on first save", 0),
                    ]));
                });
            },
        );
        let outcome = match result {
            Ok(receipt) => {
                let state = lock(&app);
                log(&dirs, &format!("setup complete: {} elf {}", receipt.disc.serial, receipt.elf.sha256));
                drop(state);
                Ok(())
            }
            Err(error) => {
                log(&dirs, &format!("setup stopped: {error}"));
                Err(error)
            }
        };
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            ui.set_setup_busy(false);
            match outcome {
                Ok(()) => {
                    ui.set_setup_steps(steps_model(&[
                        ("Disc", "SCUS-97328", 2),
                        ("Checking disc", "Done", 2),
                        ("Preparing game files", "Done", 2),
                        ("Memory card", "Ready", 2),
                    ]));
                    ui.set_setup_message(SharedString::new());
                    ui.set_home_message(SharedString::new());
                    ui.set_screen(1);
                }
                Err(error) => {
                    ui.set_setup_message(setup_error_text(&error).into());
                    ui.set_setup_steps(steps_model(&[
                        ("Disc", "", 0),
                        ("Checking disc", "", 0),
                        ("Preparing game files", "Not finished", 3),
                        ("Memory card", "", 0),
                    ]));
                }
            }
        });
    });
}

fn setup_error_text(error: &SetupError) -> String {
    error.user_message()
}

fn play(ui: &AppWindow, app: &Shared) {
    let (disc, pins, dirs, program_dir, dev_dir, settings) = {
        let state = lock(app);
        if state.game_child.is_some() {
            return;
        }
        (
            state.config.disc_path.clone(),
            state.pins.clone(),
            state.dirs.clone(),
            state.program_dir.clone(),
            state.config.dev_game_dir.clone(),
            state.config.settings.clone(),
        )
    };
    let Some(disc) = disc.filter(|path| disc_present(path, &pins)) else {
        set_home_error(ui, app, "Disc image not found or changed. Choose it again in setup.");
        ui.set_screen(0);
        return;
    };
    if let Err(message) = disc_setup::verify_prepared(&dirs.prepared(), &pins) {
        set_home_error(ui, app, &format!("Game files are not ready ({message}). Prepare them again in setup."));
        ui.set_screen(0);
        return;
    }
    let game_dir = paths::game_directory(&program_dir, dev_dir.as_deref());
    let executable = game_dir.join(GAME_EXECUTABLE);
    if !executable.exists() {
        set_home_error(ui, app, "The game files are missing. Reinstall GT4Recomp.");
        return;
    }
    let controls_path = dirs.root().join(CONTROLS_FILE_NAME);
    let mut extra: Vec<(String, String)> = Vec::new();
    if controls_path.exists() {
        extra.push(("GT4_CONTROLS_FILE".into(), controls_path.display().to_string()));
    }
    // TASK-181: the racing wheel file beside the controls file (the game's virtual Driving Force Pro), when it exists.
    let wheel_path = controls_path.with_file_name(paths::WHEEL_FILE);
    if wheel_path.exists() {
        extra.push(("GT4_WHEEL_FILE".into(), wheel_path.display().to_string()));
    }
    let card = dirs.runtime_card();
    let prepared = saves::prepare_card(&lock(app).library, &card);
    if let Err(message) = prepared {
        set_home_error(ui, app, &message);
        return;
    }
    let running = match game::start(
        &dirs,
        &executable,
        &dirs.prepared().join(disc_setup::ELF_FILE),
        &disc,
        &settings,
        &extra,
    ) {
        Ok(running) => running,
        Err(message) => {
            set_home_error(ui, app, &message);
            return;
        }
    };
    lock(app).game_child = Some(running.child.clone());
    ui.set_game_running(true);
    ui.set_home_message(SharedString::new());
    log(&dirs, &format!("game started, session {}", running.session_dir.display()));

    let weak = ui.as_weak();
    let app = app.clone();
    std::thread::spawn(move || {
        let outcome = game::wait_for_exit(&running);
        match saves::harvest_card(&lock(&app).library, &card) {
            Ok(report) => log(&lock(&app).dirs, &format!("card harvested: {report:?}")),
            Err(error) => log(&lock(&app).dirs, &format!("card not harvested: {error}")),
        }
        let log_path = running.log_path.clone();
        drop(running);
        {
            let mut state = lock(&app);
            state.game_child = None;
            log(&state.dirs, &format!("game ended: {outcome:?} (log {})", log_path.display()));
        }
        let message = match &outcome {
            Outcome::NormalClose => String::new(),
            Outcome::Crashed { .. } => exit::user_message(&outcome),
        };
        let refresh_app = app.clone();
        let _ = slint::invoke_from_event_loop(move || {
            let Some(ui) = weak.upgrade() else { return };
            ui.set_game_running(false);
            refresh_saves(&ui, &refresh_app);
            if !message.is_empty() {
                ui.set_home_message(message.into());
            }
        });
    });
}

fn set_home_error(ui: &AppWindow, _app: &Shared, message: &str) {
    ui.set_home_message(message.into());
}

/// Cheap check before Play: the file exists and has the pinned size (the full hash ran at setup).
fn disc_present(path: &Path, pins: &Pins) -> bool {
    fs::metadata(path).map(|meta| meta.len() == pins.disc.size_bytes).unwrap_or(false)
}
