//! The launcher's gamepad service. One thread owns the SDL3 reader (`gt4l_platform::sdl_pads`, the layer the game
//! uses) and turns it into Tauri events for the page:
//!
//! - `pad-status` `{available, connected, name, kind, player_index, count, error, pads, unrecognized, choice, chosen}`: on
//!   every change. `count` is the number of pads, and the named pad is the one the game reads: the controller chosen on the
//!   Controls page when it is connected, else "controller 1 of N" (`pad::chosen_pad`, TASK-148). `pads` lists the readable
//!   pads with the `controller` setting value of each (`key`), `unrecognized` the joysticks SDL3 has no mapping for,
//!   `choice` is the setting and `chosen` says whether the chosen controller is the one being read.
//! - The `controller` setting always names a connected pad (owner, 2026-11-04: no "Automatic" entry). When the stored
//!   controller is not the one being read (nothing stored yet, or that pad is unplugged) for `CHOICE_SETTLE`, the pad
//!   being read is stored in its place (`choice_to_store`). So at every launcher start the pads are mapped again: the
//!   controller chosen last time stays selected when it is connected, and otherwise the first one connected is.
//! - `pad-live` `{sources, sticks}`: about 60 times a second while a pad is connected and the window is shown.
//!   `sources` holds the pressure (0..1) of each controls-file source (`xinput:A`, `xinput:LT`...); the page maps
//!   them to the DS2 buttons through the controls file. `sticks.left|right` are -1..1, y positive down.
//! - `pad-nav` `{name}`: a press for nav.js (`a b x y lb rb start`, and `up down left right` with repeat).
//!
//! The thread does no SDL work while the window is hidden for a game (`Handle::set_active(false)`, called from the
//! tray). A rebind capture is answered by the same loop, so the page never reads the pad itself.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gt4l_platform::pad::{self, PadChoice, PadIdentity, PadSample};
use gt4l_platform::sdl_pads::{PadInfo, SdlPads};
use serde_json::{json, Map, Value};
use tauri::{AppHandle, Emitter, Manager};

use crate::pad_nav::Navigator;
use crate::state::{self, Shared};

/// About 60 readings a second.
const TICK: Duration = Duration::from_millis(16);
/// How long the stored controller may be missing before the pad being read replaces it. SDL3 reports some pads a moment
/// after it starts (and a wireless pad can drop out briefly): the controller chosen last time keeps its place until then.
const CHOICE_SETTLE: Duration = Duration::from_secs(2);
/// The error a capture returns when it is cancelled (the page looks for "cancel" in it).
pub const CAPTURE_CANCELLED: &str = "capture cancelled";

enum Command {
    /// Waits for the next newly pressed control of the active pad; the reply carries its source name.
    Capture(Sender<Result<String, String>>),
    Cancel,
}

struct PendingCapture {
    reply: Sender<Result<String, String>>,
    /// The pad as it was when the capture started (set on the first reading), so held controls do not count.
    baseline: Option<PadSample>,
}

/// The page-facing side of the service, kept in Tauri's state.
#[derive(Clone)]
pub struct Handle {
    commands: Sender<Command>,
    status: Arc<Mutex<Value>>,
    active: Arc<AtomicBool>,
}

impl Handle {
    /// Starts a capture. The receiver gets `Ok(source)` for the control, or `Err` when cancelled or paused.
    pub fn start_capture(&self) -> Receiver<Result<String, String>> {
        let (reply, receiver) = channel();
        if self.commands.send(Command::Capture(reply)).is_err() {
            // The service thread is gone: the dropped sender makes the receiver fail at once.
        }
        receiver
    }

    pub fn cancel_capture(&self) {
        let _ = self.commands.send(Command::Cancel);
    }

    /// False while the window is hidden for a game: the service stops reading the pad.
    pub fn set_active(&self, active: bool) {
        self.active.store(active, Ordering::Relaxed);
    }

    /// The last `pad-status` payload (for a page that opens after the event).
    pub fn status(&self) -> Value {
        match self.status.lock() {
            Ok(value) => value.clone(),
            Err(poisoned) => poisoned.into_inner().clone(),
        }
    }
}

/// Starts the service thread and returns its handle. Manage the handle in Tauri's state.
pub fn spawn(app: AppHandle) -> Handle {
    let (commands, receiver) = channel();
    let handle = Handle {
        commands,
        status: Arc::new(Mutex::new(status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, "starting"))),
        active: Arc::new(AtomicBool::new(true)),
    };
    let worker = handle.clone();
    let spawned = std::thread::Builder::new()
        .name("gt4-pad".to_string())
        .spawn(move || run(app, receiver, worker));
    if spawned.is_err() {
        // No thread: captures fail at once (the dropped receiver) and the status says so.
        publish(None, &handle, status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, "the gamepad thread could not start"));
    }
    handle
}

/// The `pad-status` payload. `available` is false when SDL did not start. `choice` is the `controller` setting.
pub fn status_json(
    available: bool,
    pads: &[PadInfo],
    unrecognized: &[PadInfo],
    active: Option<&PadInfo>,
    choice: &str,
    error: &str,
) -> Value {
    let key = |pad: &PadInfo| PadChoice::key(&pad.guid, &pad.serial);
    let chosen = match (PadChoice::parse(choice), active) {
        (Some(wanted), Some(pad)) => pad.guid.eq_ignore_ascii_case(&wanted.guid),
        _ => false,
    };
    let active_id = active.map(|pad| pad.id);
    json!({
        "available": available,
        "connected": active.is_some(),
        "name": active.map(|pad| pad.name.clone()).unwrap_or_default(),
        "kind": active.map(|pad| pad.kind.clone()).unwrap_or_default(),
        "player_index": active.and_then(|pad| pad.player_index),
        "count": pads.len(),
        "error": error,
        "pads": pads
            .iter()
            .map(|pad| json!({ "name": pad.name, "kind": pad.kind, "key": key(pad), "active": active_id == Some(pad.id) }))
            .collect::<Vec<_>>(),
        "unrecognized": unrecognized.iter().map(|pad| json!({ "name": pad.name, "guid": pad.guid })).collect::<Vec<_>>(),
        "choice": choice,
        "chosen": chosen,
    })
}

/// TASK-148: the `controller` setting, read from the launcher's state each tick (a change on the Controls page takes
/// effect at once).
fn controller_setting(app: &AppHandle) -> String {
    let shared = app.state::<Shared>();
    let state = state::lock(shared.inner());
    state.config.settings.get("controller").to_string()
}

/// The `controller` value to store in place of `stored`: the key of the pad being read when it differs (nothing stored
/// yet, or the stored pad is not connected), `None` when `stored` already names it or no pad is connected.
pub fn choice_to_store(stored: &str, active: Option<&PadInfo>) -> Option<String> {
    let active = active?;
    let key = PadChoice::key(&active.guid, &active.serial);
    (key != stored).then_some(key)
}

/// Stores `key` as the `controller` setting (the pad being read, `choice_to_store`) and saves the settings.
fn store_controller(app: &AppHandle, key: &str, name: &str) {
    let shared = app.state::<Shared>();
    let mut state = state::lock(shared.inner());
    let previous = state.config.settings.get("controller").to_string();
    if let Err(error) = state.config.settings.set_by_user("controller", key) {
        state::log(&state.dirs, &format!("controller input: {name} not stored as the controller: {error}"));
        return;
    }
    state.save_config();
    state::log(&state.dirs, &format!("controller input: {name} selected ({key}); the stored controller was {previous}"));
}

/// The `pad-live` payload: the mapped sources and the sticks of one reading.
pub fn live_json(sample: &PadSample) -> Value {
    let sources: Map<String, Value> = sample
        .pressure
        .iter()
        .map(|(name, pressure)| ((*name).to_string(), json!(f64::from(*pressure) / 255.0)))
        .collect();
    json!({
        "sources": sources,
        "sticks": {
            "left": { "x": axis(sample.left_x), "y": axis(sample.left_y) },
            "right": { "x": axis(sample.right_x), "y": axis(sample.right_y) },
        },
    })
}

fn axis(raw: i16) -> f64 {
    (f64::from(raw) / 32_767.0).clamp(-1.0, 1.0)
}

fn publish(app: Option<&AppHandle>, handle: &Handle, status: Value) {
    if let Ok(mut slot) = handle.status.lock() {
        *slot = status.clone();
    }
    if let Some(app) = app {
        let _ = app.emit("pad-status", status);
    }
}

fn log(app: &AppHandle, message: &str) {
    let dirs = {
        let shared = app.state::<Shared>();
        let state = state::lock(shared.inner());
        state.dirs.clone()
    };
    state::log(&dirs, message);
}

fn run(app: AppHandle, commands: Receiver<Command>, handle: Handle) {
    let mut pads = match SdlPads::start() {
        Ok(pads) => pads,
        Err(message) => {
            log(&app, &format!("controller input unavailable: {message}"));
            publish(Some(&app), &handle, status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, &message));
            // Keep answering: a capture fails with the reason instead of waiting for a pad that cannot come.
            while let Ok(command) = commands.recv() {
                if let Command::Capture(reply) = command {
                    let _ = reply.send(Err(message.clone()));
                }
            }
            return;
        }
    };
    log(&app, "controller input: SDL3 gamepad subsystem started");
    // TASK-148: the player's own SDL gamepad mappings (gamecontrollerdb.txt in the config folder), when there are any.
    let mappings = {
        let shared = app.state::<Shared>();
        let state = state::lock(shared.inner());
        state.user_dirs.config_dir().join(gt4l_core::launch::GAMEPAD_MAPPINGS_FILE)
    };
    if mappings.exists() {
        match pads.load_mappings(&mappings) {
            Ok(added) => log(&app, &format!("controller input: {added} gamepad mappings from {}", mappings.display())),
            Err(message) => log(&app, &format!("controller input: gamepad mappings not loaded: {message}")),
        }
    }

    let mut navigator = Navigator::default();
    let mut capture: Option<PendingCapture> = None;
    let mut last_status = Value::Null;
    let mut was_active = true;
    // Since when the stored controller has not been the pad being read (`CHOICE_SETTLE`).
    let mut unsettled_since: Option<Instant> = None;

    loop {
        std::thread::sleep(TICK);
        while let Ok(command) = commands.try_recv() {
            match command {
                Command::Capture(reply) => {
                    if let Some(previous) = capture.take() {
                        let _ = previous.reply.send(Err(CAPTURE_CANCELLED.to_string()));
                    }
                    capture = Some(PendingCapture { reply, baseline: None });
                }
                Command::Cancel => {
                    if let Some(pending) = capture.take() {
                        let _ = pending.reply.send(Err(CAPTURE_CANCELLED.to_string()));
                    }
                }
            }
        }

        if !handle.active.load(Ordering::Relaxed) {
            if was_active {
                was_active = false;
                navigator = Navigator::default();
                unsettled_since = None;
                if let Some(pending) = capture.take() {
                    let _ = pending.reply.send(Err("controller input is paused while the game runs".to_string()));
                }
            }
            continue;
        }
        was_active = true;

        pads.pump();
        let infos = pads.connected();
        let unrecognized = pads.unrecognized();
        let choice_text = controller_setting(&app);
        let choice = PadChoice::parse(&choice_text);
        let identities: Vec<PadIdentity<'_>> =
            infos.iter().map(|pad| PadIdentity { id: pad.id, guid: &pad.guid, serial: &pad.serial }).collect();
        let active_id = pad::chosen_pad(&identities, choice.as_ref());
        let active_info = active_id.and_then(|id| infos.iter().find(|pad| pad.id == id));
        match choice_to_store(&choice_text, active_info) {
            Some(key) => {
                let since = *unsettled_since.get_or_insert_with(Instant::now);
                if since.elapsed() >= CHOICE_SETTLE {
                    let name = active_info.map(|pad| pad.name.as_str()).unwrap_or_default();
                    store_controller(&app, &key, name);
                    unsettled_since = None;
                }
            }
            None => unsettled_since = None,
        }
        let status = status_json(true, &infos, &unrecognized, active_info, &choice_text, "");
        if status != last_status {
            publish(Some(&app), &handle, status.clone());
            last_status = status;
        }

        let Some(id) = active_id else {
            navigator = Navigator::default();
            continue;
        };
        let Some(raw) = pads.read(id) else {
            continue;
        };
        let sample = pad::sample(&raw);
        let _ = app.emit("pad-live", live_json(&sample));
        for name in navigator.update(&sample, Instant::now()) {
            let _ = app.emit("pad-nav", json!({ "name": name }));
        }

        let found = match capture.as_mut() {
            None => None,
            Some(pending) => match &pending.baseline {
                None => {
                    pending.baseline = Some(sample.clone());
                    None
                }
                Some(baseline) => pad::captured_source(baseline, &sample),
            },
        };
        if let Some(source) = found {
            if let Some(pending) = capture.take() {
                let _ = pending.reply.send(Ok(source));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gt4l_platform::pad::{sample, RawPad};

    #[test]
    fn live_sources_are_the_controls_file_names_with_pressure_0_to_1() {
        let mut raw = RawPad::default();
        raw.south = true;
        raw.left_trigger = 32_767;
        let live = live_json(&sample(&raw));
        assert_eq!(live["sources"]["xinput:A"], json!(1.0));
        assert_eq!(live["sources"]["xinput:B"], json!(0.0));
        assert_eq!(live["sources"]["xinput:LT"], json!(1.0));
    }

    #[test]
    fn sticks_are_in_minus_one_to_one_with_y_down() {
        let mut raw = RawPad::default();
        raw.left_y = i16::MIN;
        raw.right_x = i16::MAX;
        let live = live_json(&sample(&raw));
        assert_eq!(live["sticks"]["left"]["y"], json!(-1.0), "SDL y is positive down: the most negative raw value is -1");
        assert!((live["sticks"]["right"]["x"].as_f64().unwrap() - 1.0).abs() < 1e-9);
    }

    fn info(id: u32, name: &str, guid: &str, serial: &str) -> PadInfo {
        PadInfo {
            id,
            name: name.into(),
            kind: "ps5".into(),
            player_index: Some(0),
            guid: guid.into(),
            serial: serial.into(),
            mapped: true,
        }
    }

    const DUALSENSE: &str = "050000004c050000e60c000000810000";
    const XBOX: &str = "030000005e0400008e02000014010000";

    #[test]
    fn status_names_the_active_pad_and_counts_all_of_them() {
        let pads = [info(3, "DualSense Wireless Controller", DUALSENSE, "a0:ab"), info(5, "Xbox 360 Controller", XBOX, "")];
        let status = status_json(true, &pads, &[], Some(&pads[0]), "auto", "");
        assert_eq!(status["connected"], json!(true));
        assert_eq!(status["name"], json!("DualSense Wireless Controller"));
        assert_eq!(status["kind"], json!("ps5"));
        assert_eq!(status["count"], json!(2));
        assert_eq!(status["pads"][0]["key"], json!(format!("{DUALSENSE}/a0:ab")), "the setting value of each pad");
        assert_eq!(status["pads"][1]["key"], json!(XBOX));
        assert_eq!(status["pads"][0]["active"], json!(true));
        assert_eq!(status["chosen"], json!(false), "nothing stored yet: controller 1 is read");
        let none = status_json(true, &[], &[], None, "auto", "");
        assert_eq!(none["connected"], json!(false));
        assert_eq!(none["name"], json!(""));
    }

    #[test]
    fn status_says_whether_the_chosen_controller_is_read() {
        let pads = [info(3, "DualSense Wireless Controller", DUALSENSE, ""), info(5, "Xbox 360 Controller", XBOX, "")];
        let chosen = status_json(true, &pads, &[], Some(&pads[1]), XBOX, "");
        assert_eq!(chosen["chosen"], json!(true));
        assert_eq!(chosen["choice"], json!(XBOX));
        let gone = status_json(true, &pads[..1], &[], Some(&pads[0]), XBOX, "");
        assert_eq!(gone["chosen"], json!(false), "the chosen pad is unplugged: controller 1 is read");
        let mut old = info(8, "Old USB Joystick", "03000000aaaa0000bbbb000000000000", "");
        old.mapped = false;
        let with_unknown = status_json(true, &pads, &[old], Some(&pads[0]), "auto", "");
        assert_eq!(with_unknown["unrecognized"][0]["name"], json!("Old USB Joystick"));
    }

    #[test]
    fn the_pad_being_read_replaces_a_stored_controller_that_is_not_connected() {
        let pads = [info(3, "DualSense Wireless Controller", DUALSENSE, "a0:ab"), info(5, "Xbox 360 Controller", XBOX, "")];
        let dualsense = format!("{DUALSENSE}/a0:ab");
        assert_eq!(choice_to_store("auto", Some(&pads[0])), Some(dualsense.clone()), "a fresh install stores controller 1");
        assert_eq!(choice_to_store(XBOX, Some(&pads[0])), Some(dualsense.clone()), "the Xbox pad is unplugged");
        assert_eq!(choice_to_store(&dualsense, Some(&pads[0])), None, "the stored pad is the one read");
        assert_eq!(choice_to_store(XBOX, Some(&pads[1])), None);
        assert_eq!(choice_to_store(XBOX, None), None, "no pad: the stored controller is kept");
        let other_unit = format!("{DUALSENSE}/ff:ff");
        assert_eq!(choice_to_store(&other_unit, Some(&pads[0])), Some(dualsense), "another DualSense is read: it is stored");
    }

    #[test]
    fn an_unavailable_status_carries_the_reason() {
        let status = status_json(false, &[], &[], None, "auto", "SDL3 did not start: no device");
        assert_eq!(status["available"], json!(false));
        assert_eq!(status["error"], json!("SDL3 did not start: no device"));
    }
}
