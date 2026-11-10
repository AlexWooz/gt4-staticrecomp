//! The Wheel page's commands (TASK-181): `wheel.toml` (the game's virtual Logitech Driving Force Pro), its presets, the
//! capture of a control, the force test and what the last game session said about the wheel.
//!
//! Based on the nine wheel commands of the 1.2.5 launcher (cbfd8b8, PR #101) by JulioHeroZ, with luanantony:
//! `get_wheel_config`, `set_wheel_setting`, `apply_wheel_preset`, `assign_wheel_binding`, `reset_wheel_binding`,
//! `reset_wheel_config`, `capture_wheel_binding`, `cancel_wheel_capture`, `test_wheel_ffb`. TASK-181 adapts them to the
//! DFP-only contract and adds `wheel_watch`, `get_wheel_session_check`, `list_wheel_presets`, `save_wheel_preset` and
//! `delete_wheel_preset`. Every change writes `wheel.toml` at once, validated with the game's rules first: a refused
//! change keeps the file and the model as they were and returns the reason.
//!
//! The view every change returns (`get_wheel_config`): `{config, slots, families, pedal_modes, curves, file, status,
//! presets, game_running, taken_from}` (see `ui/dev/README.md`).

use std::fs;
use std::path::PathBuf;
use std::time::SystemTime;

use gt4l_core::messages;
use gt4l_core::wheel::{self, LoadState, WheelConfig, BUTTON_SLOTS, CURVES, PEDALS, PEDAL_MODES};
use gt4l_core::wheel_capture::{Request, Want};
use gt4l_core::wheel_identity;
use gt4l_core::wheel_session;
use serde_json::{json, Value};
use tauri::State;

use crate::pad_service::{self, WheelCaptureAsk};
use crate::state::{self, lock, AppState, Shared};
use crate::wheel_service;

fn file_json(state: &AppState) -> Value {
    let (kind, detail) = match &state.wheel_load {
        LoadState::Missing => ("missing", String::new()),
        LoadState::Loaded => ("loaded", String::new()),
        LoadState::Migrated(why) => ("migrated", why.clone()),
        LoadState::Refused(error) => ("refused", error.clone()),
        LoadState::Newer(error) => ("newer", error.clone()),
    };
    json!({ "state": kind, "detail": detail, "exists": state.wheel_path().exists(), "locked": state.wheel_locked })
}

/// The page's whole view. `status` is the pad service's last `wheel-status` payload.
pub fn view(state: &AppState, status: Value, game_running: bool) -> Value {
    json!({
        "config": state.wheel,
        "slots": BUTTON_SLOTS,
        "families": wheel_identity::FAMILIES,
        "pedal_modes": PEDAL_MODES,
        "curves": CURVES,
        "file": file_json(state),
        "status": status,
        "presets": wheel::list_presets(&state.wheel_presets_dir()),
        "game_running": game_running || state.game_child.is_some(),
        "taken_from": [],
        "applied_preset": state.wheel_applied_preset,
    })
}

/// Applies `change` to a copy, writes `wheel.toml` (validated first), and only then keeps the copy.
fn change_wheel<T>(state: &mut AppState, change: impl FnOnce(&mut WheelConfig) -> Result<T, String>) -> Result<T, String> {
    let language = state::language_of(state);
    if state.wheel_locked {
        return Err(messages::text("wheel.newer_file", language).to_string());
    }
    let path = state.wheel_path();
    let existed = path.exists();
    let mut next = state.wheel.clone();
    let result = change(&mut next)?;
    next.save_atomic(&path)
        .map_err(|error| messages::render("wheel.could_not_save", language, &[("error", &error)]))?;
    state.wheel = next;
    state.wheel_load = LoadState::Loaded;
    if !existed {
        // A direct start of the game finds the new file through game.ini.
        state.write_standalone_files(&state.config.settings);
        state::log(&state.dirs, &format!("wheel: {} created", path.display()));
    }
    Ok(result)
}

fn status_of(pads: &State<'_, pad_service::Handle>) -> (Value, bool) {
    (pads.inner().wheel_status(), pads.inner().game_running())
}

/// The device the page talks about: the one the game drives, else the first wheel, else the first device (the page's own
/// `mainDevice` in `js/wheel-model.js`).
pub fn main_device(status: &Value) -> Option<&Value> {
    let devices = status.get("devices")?.as_array()?;
    let primary = status.get("primary").and_then(Value::as_i64);
    devices
        .iter()
        .find(|device| primary.is_some() && device.get("id").and_then(Value::as_i64) == primary)
        .or_else(|| devices.iter().find(|device| device.get("role").and_then(Value::as_str) != Some("auxiliary")))
        .or_else(|| devices.first())
}

/// TASK-189: the shipped preset of the wheel the page shows, as a whole mapping, when the launcher has one for it.
pub fn builtin_for(status: &Value) -> Option<(&'static wheel_identity::BuiltinPreset, WheelConfig)> {
    let device = main_device(status)?;
    let guid = device.get("guid")?.as_str()?;
    let preset = wheel_identity::builtin_preset(guid)?;
    let mut config = wheel_identity::builtin_preset_config(preset, guid)?;
    config.device_name = wheel::plain_text(device.get("name").and_then(Value::as_str).unwrap_or_default());
    Some((preset, config))
}

/// TASK-189: applies the shipped preset of the wheel the page shows, when the launcher has no mapping of its own (no file
/// yet, and the file was not refused or written by a newer launcher). Returns the preset's name. A write failure is logged
/// and does not stop the page: the player can still map the wheel by hand.
fn auto_apply_builtin(state: &mut AppState, status: &Value) -> Option<String> {
    if !matches!(state.wheel_load, LoadState::Missing) || state.wheel_path().exists() || state.wheel_locked {
        return None;
    }
    let (preset, config) = builtin_for(status)?;
    let device_name = config.device_name.clone();
    let guid = config.device_guid.clone();
    if let Err(error) = change_wheel(state, |current| {
        *current = config.clone();
        Ok(())
    }) {
        state::log_error(&state.dirs, &format!("wheel: preset {} could not be written ({error})", preset.name));
        return None;
    }
    state.wheel_applied_preset = Some(preset.name.to_string());
    state::log(
        &state.dirs,
        &format!("wheel: preset {} applied for {} ({guid})", preset.name, if device_name.is_empty() { "the wheel" } else { &device_name }),
    );
    Some(preset.name.to_string())
}

/// TASK-189: Reset puts the wheel back to its shipped preset (a G27 keeps its own mapping), or to nothing when the launcher
/// has none for it. `change_wheel` already wrote the empty mapping; this writes the preset over it.
fn reset_to_builtin(state: &mut AppState, status: &Value) {
    if state.wheel_locked {
        return;
    }
    let Some((preset, config)) = builtin_for(status) else {
        return;
    };
    let device_name = config.device_name.clone();
    let guid = config.device_guid.clone();
    match change_wheel(state, |current| {
        *current = config.clone();
        Ok(())
    }) {
        Ok(()) => {
            state.wheel_applied_preset = Some(preset.name.to_string());
            state::log(
                &state.dirs,
                &format!("wheel: reset gives preset {} for {} ({guid})", preset.name, if device_name.is_empty() { "the wheel" } else { &device_name }),
            );
        }
        Err(error) => state::log_error(&state.dirs, &format!("wheel: preset {} could not be written after the reset ({error})", preset.name)),
    }
}

/// `get_wheel_config()`: the view. A wheel the launcher ships a preset for starts from it (TASK-189).
#[tauri::command]
pub fn get_wheel_config(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    let mut state = lock(shared.inner());
    auto_apply_builtin(&mut state, &status);
    Ok(view(&state, status, running))
}

pub fn get_wheel_config_of(shared: &Shared) -> Value {
    get_wheel_config_with(shared, json!({ "available": true, "devices": [] }), false)
}

/// The view for a known wheel status (the tests and the page's own reads).
pub fn get_wheel_config_with(shared: &Shared, status: Value, running: bool) -> Value {
    let mut state = lock(shared);
    auto_apply_builtin(&mut state, &status);
    view(&state, status, running)
}

/// `set_wheel_setting({key, value})`: one setting (`wheel.enabled`, `steering.rotation_degrees`, `throttle.deadzone`,
/// `ffb.strength`...; see `WheelConfig::set_setting`). Returns the view.
#[tauri::command]
pub fn set_wheel_setting(key: String, value: Value, shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    set_wheel_setting_of(key, value, shared.inner(), status, running)
}

pub fn set_wheel_setting_of(key: String, value: Value, shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    change_wheel(&mut state, |config| config.set_setting(&key, &value))?;
    state.wheel_applied_preset = None;
    Ok(view(&state, status, running))
}

/// The name of the device with `guid` in a `wheel-status` payload.
fn device_name(status: &Value, guid: &str) -> Option<String> {
    status["devices"].as_array()?.iter().find(|device| device["guid"].as_str().is_some_and(|known| known.eq_ignore_ascii_case(guid)))?["name"]
        .as_str()
        .map(str::to_string)
}

/// `apply_wheel_preset({name, saved, guid})`: a family template (`logitech_legacy`...) or, with `saved`, a named preset.
/// `guid` makes that device the wheel the game drives (a template); a preset keeps its own device. The wheel stays on or
/// off as it was. Returns the view.
#[tauri::command]
pub fn apply_wheel_preset(
    name: String,
    saved: Option<bool>,
    guid: Option<String>,
    shared: State<'_, Shared>,
    pads: State<'_, pad_service::Handle>,
) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    apply_wheel_preset_of(name, saved.unwrap_or(false), guid, shared.inner(), status, running)
}

pub fn apply_wheel_preset_of(name: String, saved: bool, guid: Option<String>, shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    let mut next = if saved {
        wheel::read_preset(&state.wheel_presets_dir(), &name)?
    } else {
        wheel_identity::template(&name).ok_or_else(|| messages::render("wheel.unknown_template", language, &[("name", &name)]))?
    };
    if !saved {
        if let Some(guid) = guid.filter(|guid| wheel::is_guid(guid)) {
            next.device_name = wheel::plain_text(&device_name(&status, &guid).unwrap_or_default());
            next.device_guid = guid.to_ascii_lowercase();
        }
    }
    change_wheel(&mut state, |config| {
        next.enabled = config.enabled;
        *config = next;
        Ok(())
    })?;
    state.wheel_applied_preset = None;
    state::log(&state.dirs, &format!("wheel: {} {name} applied", if saved { "preset" } else { "template" }));
    Ok(view(&state, status, running))
}

/// `assign_wheel_binding({target, source, mode, add})`: binds a captured source to `steering`, a pedal or a `[buttons]`
/// slot (`r2` is gear up, the DFP's R1). `mode` is the pedal mode the capture found; `add` keeps the slot's other
/// sources. Returns the view, with `taken_from` (the slots that lost the source).
#[tauri::command]
pub fn assign_wheel_binding(
    target: String,
    source: String,
    mode: Option<String>,
    add: Option<bool>,
    shared: State<'_, Shared>,
    pads: State<'_, pad_service::Handle>,
) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    assign_wheel_binding_of(target, source, mode, add.unwrap_or(false), shared.inner(), status, running)
}

pub fn assign_wheel_binding_of(target: String, source: String, mode: Option<String>, add: bool, shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    let taken = change_wheel(&mut state, |config| config.assign_binding(&target, &source, mode.as_deref(), add))?;
    state.wheel_applied_preset = None;
    let mut answer = view(&state, status, running);
    answer["taken_from"] = json!(taken);
    Ok(answer)
}

/// `reset_wheel_binding({target, clear})`: unbinds one control (`clear`), or puts back its template value (the family
/// template the mapping came from, else the game's default). Returns the view.
#[tauri::command]
pub fn reset_wheel_binding(target: String, clear: bool, shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    reset_wheel_binding_of(target, clear, shared.inner(), status, running)
}

pub fn reset_wheel_binding_of(target: String, clear: bool, shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    let template = reset_template(&state.wheel);
    change_wheel(&mut state, |config| config.reset_binding(&target, clear, &template))?;
    state.wheel_applied_preset = None;
    Ok(view(&state, status, running))
}

/// What a control goes back to when the player resets it: the shipped preset the mapping came from (a G27 keeps the G27's
/// own binding), else the family's template, else nothing at all (TASK-189: never a guessed control).
pub fn reset_template(config: &WheelConfig) -> WheelConfig {
    if let Some(preset) = wheel_identity::builtin_preset_by_name(&config.preset) {
        if let Some(found) = wheel_identity::builtin_preset_config(preset, &config.device_guid) {
            return found;
        }
    }
    wheel_identity::template(&config.preset).unwrap_or_else(WheelConfig::unassigned)
}

/// `reset_wheel_config()`: every control unassigned, then the shipped preset again for the wheel connected now (a G27
/// starts from its own mapping). The virtual wheel in the game follows Play (TASK-189). Returns the view.
#[tauri::command]
pub fn reset_wheel_config(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    reset_wheel_config_of(shared.inner(), status, running)
}

pub fn reset_wheel_config_of(shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    change_wheel(&mut state, |config| {
        *config = WheelConfig::unassigned();
        Ok(())
    })?;
    state.wheel_applied_preset = None;
    state::log(&state.dirs, "wheel: every control unassigned");
    reset_to_builtin(&mut state, &status);
    Ok(view(&state, status, running))
}

/// The capture request for `target`: steering (`phase` `right` waits for `axis`, the axis found by `left`), a pedal (the
/// steering axis never counts) or a button (the steering and pedal axes never count).
pub fn capture_request(config: &WheelConfig, target: &str, phase: Option<&str>, axis: Option<&str>) -> Result<Request, String> {
    let (want, ignore) = match target {
        "steering" => {
            let only = if phase == Some("right") { axis.map(str::to_string) } else { None };
            (Want::Steering { only }, Vec::new())
        }
        _ if PEDALS.contains(&target) => (Want::Pedal, vec![config.steering.source.clone()]),
        _ => {
            wheel::button_slot(target).ok_or_else(|| format!("unknown wheel control '{target}'"))?;
            (Want::Button, config.analog_sources())
        }
    };
    Ok(Request { want, primary: None, ignore: ignore.into_iter().filter(|source| !source.is_empty()).collect() })
}

/// `capture_wheel_binding({target, phase, axis})`: waits for the player to move a control on the wheel or an auxiliary
/// device (SDL3, in the pad service) and resolves with `{target, source, mode, rest, value, device_name, guid}`. It binds
/// nothing: the page calls `assign_wheel_binding`. A control held when the wait starts does not count;
/// `cancel_wheel_capture` ends the wait with an error. Refused while the game runs.
#[tauri::command]
pub async fn capture_wheel_binding(
    target: String,
    phase: Option<String>,
    axis: Option<String>,
    shared: State<'_, Shared>,
    pads: State<'_, pad_service::Handle>,
) -> Result<Value, String> {
    let request = {
        let state = lock(shared.inner());
        if state.game_child.is_some() {
            return Err(messages::text("wheel.game_running", state::language_of(&state)).to_string());
        }
        capture_request(&state.wheel, &target, phase.as_deref(), axis.as_deref())?
    };
    let answer = pads.inner().start_wheel_capture(WheelCaptureAsk { request });
    let outcome = tauri::async_runtime::spawn_blocking(move || answer.recv()).await;
    match outcome {
        Ok(Ok(Ok(found))) => Ok(json!({
            "target": target,
            "source": found.captured.source,
            "mode": found.captured.mode,
            "rest": found.captured.rest,
            "value": found.captured.value,
            "device_name": found.device_name,
            "guid": found.captured.guid,
        })),
        Ok(Ok(Err(message))) => Err(message),
        Ok(Err(_)) => Err(pad_service::CAPTURE_CANCELLED.to_string()),
        Err(error) => Err(error.to_string()),
    }
}

/// `cancel_wheel_capture()`: ends a running capture without binding anything.
#[tauri::command]
pub fn cancel_wheel_capture(pads: State<'_, pad_service::Handle>) -> Result<(), String> {
    pads.inner().cancel_wheel_capture();
    Ok(())
}

/// `wheel_watch({on})`: the Wheel page is shown (`true`, renewed every two seconds: the live view and its device
/// handles) or gone (`false`: every handle closes). Returns the last `wheel-status` payload.
#[tauri::command]
pub fn wheel_watch(on: bool, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    pads.inner().wheel_watch(on);
    Ok(pads.inner().wheel_status())
}

/// TASK-189 (owner, 2026-11-06): what Play does to `wheel.toml` before the game starts. There is no on/off switch on the
/// page any more:
/// 1. a wheel the launcher ships a preset for, with no mapping of its own yet, gets that preset;
/// 2. the virtual Logitech Driving Force Pro is plugged in when a wheel is connected, and left out when none is.
///
/// The stored `enabled` value of a 1.3.0-rc15 file is not a setting any more: it is the result of step 2, so the old key's
/// value is dropped and replaced here (logged). When SDL3 has not listed the devices there is nothing to decide on, and
/// the stored value is kept so a Play started right at boot does not lose the wheel.
pub fn prepare_for_play(state: &mut AppState, status: &Value) {
    if state.wheel_locked {
        state::log(&state.dirs, "wheel: the wheel file is from a newer launcher: it is kept as it is");
        return;
    }
    auto_apply_builtin(state, status);
    let Some(connected) = wheel_service::wheel_connected(status) else {
        if state.wheel_path().exists() {
            state::log(&state.dirs, "wheel: the device list was not ready: the stored on/off is kept for this Start");
        }
        return;
    };
    if state.wheel.enabled == connected {
        return;
    }
    if !state.wheel_path().exists() && !connected {
        return; // no file, no wheel: nothing to write
    }
    let name = main_device(status)
        .and_then(|device| device.get("name"))
        .and_then(Value::as_str)
        .filter(|name| !name.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| state.wheel.device_name.clone());
    let note = if connected {
        format!("{} connected", if name.is_empty() { "a wheel" } else { &name })
    } else {
        "no wheel connected".to_string()
    };
    match change_wheel(state, |config| {
        config.enabled = connected;
        Ok(())
    }) {
        Ok(()) => state::log(
            &state.dirs,
            &format!("wheel: virtual Driving Force Pro {} for this Start ({note})", if connected { "on" } else { "off" }),
        ),
        Err(error) => state::log_error(&state.dirs, &format!("wheel: the virtual Driving Force Pro could not be switched {} ({error})", if connected { "on" } else { "off" })),
    }
}

/// `test_wheel_ffb()`: the force test on the device the game drives: a short constant pull one way, then the other, at
/// the configured strength (a low base level); one haptic handle, opened for the test and closed right after; never SDL
/// rumble. Refused while the game runs. Resolves with `{device_name, has_ffb, features, played, error}`.
#[tauri::command]
pub async fn test_wheel_ffb(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (strength, invert) = {
        let state = lock(shared.inner());
        if state.game_child.is_some() || pads.inner().game_running() {
            return Err(messages::text("wheel.game_running", state::language_of(&state)).to_string());
        }
        (state.wheel.ffb.strength, state.wheel.ffb.invert)
    };
    let answer = pads.inner().test_force(strength, invert);
    let outcome = tauri::async_runtime::spawn_blocking(move || answer.recv()).await;
    match outcome {
        Ok(Ok(Ok((device_name, test)))) => Ok(json!({
            "device_name": device_name,
            "has_ffb": test.has_ffb,
            "features": {
                "constant": test.features.constant,
                "spring": test.features.spring,
                "damper": test.features.damper,
                "friction": test.features.friction,
                "gain": test.features.gain,
                "autocenter": test.features.autocenter,
            },
            "played": test.played,
            "error": test.error,
        })),
        Ok(Ok(Err(message))) => Err(message),
        Ok(Err(_)) => Err(pad_service::CAPTURE_CANCELLED.to_string()),
        Err(error) => Err(error.to_string()),
    }
}

/// The newest game session folder (`session-*`) and its last change.
fn newest_session(sessions: &std::path::Path) -> Option<(PathBuf, SystemTime)> {
    fs::read_dir(sessions)
        .ok()?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.is_dir() && path.file_name().and_then(|name| name.to_str()).is_some_and(|name| name.starts_with("session-")))
        .filter_map(|path| {
            let changed = fs::metadata(path.join("game.log")).and_then(|metadata| metadata.modified()).ok()?;
            Some((path, changed))
        })
        .max_by_key(|(_, changed)| *changed)
}

/// `get_wheel_session_check()`: the wheel lines of the newest game session's log (`gt4l_core::wheel_session`):
/// `{found, session, modified, ...check}`; `received` is `yes`, `no` or `unknown`.
#[tauri::command]
pub fn get_wheel_session_check(shared: State<'_, Shared>) -> Result<Value, String> {
    Ok(get_wheel_session_check_of(shared.inner()))
}

pub fn get_wheel_session_check_of(shared: &Shared) -> Value {
    let sessions = lock(shared).dirs.sessions();
    let Some((folder, changed)) = newest_session(&sessions) else {
        return json!({ "found": false, "received": "unknown", "reason": "no_session" });
    };
    let text = fs::read_to_string(folder.join("game.log")).unwrap_or_default();
    let mut answer = serde_json::to_value(wheel_session::check(&text)).unwrap_or_else(|_| json!({}));
    answer["found"] = json!(true);
    answer["session"] = json!(folder.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default());
    answer["modified"] = json!(gt4saves::iso8601_utc(changed));
    answer
}

/// `list_wheel_presets()`: `[{name, device_name, preset, problem}]`.
#[tauri::command]
pub fn list_wheel_presets(shared: State<'_, Shared>) -> Result<Value, String> {
    Ok(json!(wheel::list_presets(&lock(shared.inner()).wheel_presets_dir())))
}

/// `save_wheel_preset({name})`: the current mapping as preset `name` (`config\wheel-presets\<name>.toml`, the
/// `wheel.toml` format, so a preset can be shared). Returns the list.
#[tauri::command]
pub fn save_wheel_preset(name: String, shared: State<'_, Shared>) -> Result<Value, String> {
    save_wheel_preset_of(name, shared.inner())
}

pub fn save_wheel_preset_of(name: String, shared: &Shared) -> Result<Value, String> {
    let state = lock(shared);
    let folder = state.wheel_presets_dir();
    let saved = wheel::save_preset(&folder, &name, &state.wheel)?;
    state::log(&state.dirs, &format!("wheel: preset {saved} saved"));
    Ok(json!(wheel::list_presets(&folder)))
}

/// `delete_wheel_preset({name})`: deletes one preset (the page asks first). Returns the list.
#[tauri::command]
pub fn delete_wheel_preset(name: String, shared: State<'_, Shared>) -> Result<Value, String> {
    let state = lock(shared.inner());
    let folder = state.wheel_presets_dir();
    wheel::delete_preset(&folder, &name)?;
    state::log(&state.dirs, &format!("wheel: preset {name} deleted"));
    Ok(json!(wheel::list_presets(&folder)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_capture_request_ignores_the_axes_that_belong_to_another_control() {
        let config = WheelConfig::default();
        let steering = capture_request(&config, "steering", Some("left"), None).unwrap();
        assert_eq!(steering.want, Want::Steering { only: None });
        let right = capture_request(&config, "steering", Some("right"), Some("axis:0")).unwrap();
        assert_eq!(right.want, Want::Steering { only: Some("axis:0".into()) });
        let throttle = capture_request(&config, "throttle", None, None).unwrap();
        assert_eq!((throttle.want, throttle.ignore), (Want::Pedal, vec!["axis:0".to_string()]));
        let cross = capture_request(&config, "cross", None, None).unwrap();
        assert_eq!(cross.want, Want::Button);
        assert_eq!(cross.ignore, vec!["axis:0", "axis:1", "axis:2", "axis:3"]);
        assert!(capture_request(&config, "trunk", None, None).is_err());
    }

    #[test]
    fn the_device_name_comes_from_the_status() {
        let status = json!({ "devices": [{ "guid": "030000006d0400009bc2000000000000", "name": "G27" }] });
        assert_eq!(device_name(&status, "030000006D0400009BC2000000000000").as_deref(), Some("G27"));
        assert_eq!(device_name(&status, "0300000000000000000000000000ffff"), None);
    }

    /// TASK-189: the page talks about the wheel the game drives, then the first wheel, then the first device.
    #[test]
    fn the_main_device_is_the_one_the_game_drives() {
        let status = json!({
            "primary": 9,
            "devices": [
                { "id": 5, "role": "other_wheel", "name": "second wheel" },
                { "id": 9, "role": "wheel", "name": "the wheel" },
                { "id": 12, "role": "auxiliary", "name": "pedals" }
            ]
        });
        assert_eq!(main_device(&status).unwrap()["name"], json!("the wheel"));
        let no_primary = json!({ "primary": null, "devices": [{ "id": 12, "role": "auxiliary" }, { "id": 5, "role": "other_wheel" }] });
        assert_eq!(main_device(&no_primary).unwrap()["id"], json!(5), "a wheel before an auxiliary device");
        assert!(main_device(&json!({ "devices": [] })).is_none());
        assert!(main_device(&json!({})).is_none());
    }

    /// TASK-189: a control goes back to the shipped preset it came from, else to its family's template, else to nothing.
    #[test]
    fn a_control_goes_back_to_the_mapping_it_came_from() {
        let g27 = wheel_identity::builtin_preset_config(
            wheel_identity::builtin_preset("030000006d0400009bc2000000000000").unwrap(),
            "030000006d0400009bc2000000000000",
        )
        .unwrap();
        let back = reset_template(&g27);
        assert_eq!(back.buttons["cross"], vec!["btn:17"], "the G27's own Cross, not the game's guess");
        assert_eq!(back.throttle.source, "axis:1");
        let ps = WheelConfig { preset: "logitech_ps".into(), ..WheelConfig::unassigned() };
        assert_eq!(reset_template(&ps).buttons["cross"], vec!["btn:0"]);
        let unknown = WheelConfig { preset: "generic".into(), ..WheelConfig::unassigned() };
        assert!(reset_template(&unknown).is_unassigned(), "an unknown wheel goes back to nothing");
        let nothing = WheelConfig { preset: "not a preset".into(), ..WheelConfig::unassigned() };
        assert!(reset_template(&nothing).is_unassigned());
    }

    /// TASK-189: Play's rule, without a state: the shipped preset only when the launcher has no file of its own.
    #[test]
    fn the_shipped_preset_applies_only_without_a_mapping_of_the_players_own() {
        let preset = wheel_identity::builtin_preset("030000006d0400009bc2000000000000").unwrap();
        let config = wheel_identity::builtin_preset_config(preset, "030000006d0400009bc2000000000000").unwrap();
        assert_eq!(config.preset, "Logitech G27");
        assert!(config.validate().is_ok());
        assert!(wheel_identity::builtin_preset("0300000000000000000000000000ffff").is_none());
    }
}
