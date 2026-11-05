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

/// `get_wheel_config()`: the view.
#[tauri::command]
pub fn get_wheel_config(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    Ok(view(&lock(shared.inner()), status, running))
}

pub fn get_wheel_config_of(shared: &Shared) -> Value {
    view(&lock(shared), json!({ "available": true, "devices": [] }), false)
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
    let template = wheel_identity::template(&state.wheel.preset).unwrap_or_default();
    change_wheel(&mut state, |config| config.reset_binding(&target, clear, &template))?;
    Ok(view(&state, status, running))
}

/// `reset_wheel_config()`: everything back to the game's defaults, the wheel off. Returns the view.
#[tauri::command]
pub fn reset_wheel_config(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    let (status, running) = status_of(&pads);
    reset_wheel_config_of(shared.inner(), status, running)
}

pub fn reset_wheel_config_of(shared: &Shared, status: Value, running: bool) -> Result<Value, String> {
    let mut state = lock(shared);
    change_wheel(&mut state, |config| {
        *config = WheelConfig::default();
        Ok(())
    })?;
    state::log(&state.dirs, "wheel: reset to the defaults (off)");
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
}
