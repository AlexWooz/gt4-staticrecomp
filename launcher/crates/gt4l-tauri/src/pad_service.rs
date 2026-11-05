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
//! - `wheel-status` and `wheel-live` (TASK-181): the Wheel page's devices and their raw state (`wheel_service`). The
//!   joystick handles of the Wheel page are open only while the page asks for them (`wheel_watch`, renewed every two
//!   seconds); a haptic handle only during the force test.
//!
//! The thread does no SDL work while the window is hidden for a game (`Handle::set_active(false)`, called from the
//! tray). TASK-181: before the game starts, Play calls `Handle::release_for_game`, which closes every SDL handle (SDL
//! itself shuts down) and waits for it, so the game owns the wheel and its force feedback; SDL starts again when the
//! window is shown, without any racing wheel while the game still runs. A rebind capture is answered by the same loop,
//! so the page never reads the pad itself.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gt4l_core::wheel_capture::{Capture as WheelCaptureState, Captured, Request as WheelCaptureRequest};
use gt4l_platform::pad::{self, PadChoice, PadIdentity, PadSample};
use gt4l_platform::sdl_pads::{PadInfo, SdlPads};
use gt4l_platform::sdl_wheels::{self, Counts, Features, ForceTest, JoystickEntry, WheelReader};
use serde_json::{json, Map, Value};
use tauri::{AppHandle, Emitter, Manager};

use crate::pad_nav::Navigator;
use crate::state::{self, Shared};
use crate::wheel_service;

/// About 60 readings a second.
const TICK: Duration = Duration::from_millis(16);
/// How long the stored controller may be missing before the pad being read replaces it. SDL3 reports some pads a moment
/// after it starts (and a wireless pad can drop out briefly): the controller chosen last time keeps its place until then.
const CHOICE_SETTLE: Duration = Duration::from_secs(2);
/// The error a capture returns when it is cancelled (the page looks for "cancel" in it).
pub const CAPTURE_CANCELLED: &str = "capture cancelled";
/// How long a `wheel_watch(true)` keeps the Wheel page's devices open (the page renews it every two seconds).
const WHEEL_WATCH: Duration = Duration::from_secs(5);
/// How long Play waits for the service to close its SDL handles before the game starts.
const RELEASE_WAIT: Duration = Duration::from_millis(1500);
/// How often the device list is read again without a hot-plug event.
const WHEEL_LIST_REFRESH: Duration = Duration::from_millis(1000);

/// What a wheel capture waits for, from the page (`capture_wheel_binding`).
pub struct WheelCaptureAsk {
    pub request: WheelCaptureRequest,
}

/// A finished wheel capture: the control, and the name of the device it came from.
pub struct WheelCaptureAnswer {
    pub captured: Captured,
    pub device_name: String,
}

enum Command {
    /// Waits for the next newly pressed control of the active pad; the reply carries its source name.
    Capture(Sender<Result<String, String>>),
    Cancel,
    /// TASK-181: closes every SDL handle (SDL shuts down) and answers once done: the game is about to start.
    Release(Sender<()>),
    /// TASK-181: the Wheel page shows (`true`, renewed by the page) or left (`false`).
    WheelWatch(bool),
    WheelCapture(WheelCaptureAsk, Sender<Result<WheelCaptureAnswer, String>>),
    WheelCancel,
    /// The force test on the device the game drives: strength 0..2 and `[ffb] invert`.
    WheelTest { strength: f64, invert: bool, reply: Sender<Result<(String, ForceTest), String>> },
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
    /// TASK-181: the last `wheel-status` payload.
    wheel_status: Arc<Mutex<Value>>,
    /// TASK-181: the game runs: no wheel handle, no force test.
    game_running: Arc<AtomicBool>,
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

    /// TASK-181: the last `wheel-status` payload.
    pub fn wheel_status(&self) -> Value {
        match self.wheel_status.lock() {
            Ok(value) => value.clone(),
            Err(poisoned) => poisoned.into_inner().clone(),
        }
    }

    /// TASK-181: called by Play before the game starts. Marks the game as running and waits (at most `RELEASE_WAIT`)
    /// until the service closed every SDL joystick, gamepad and haptic handle. Returns false when it did not answer.
    pub fn release_for_game(&self) -> bool {
        self.game_running.store(true, Ordering::Relaxed);
        let (reply, answer) = channel();
        if self.commands.send(Command::Release(reply)).is_err() {
            return true; // no service thread: nothing is open
        }
        answer.recv_timeout(RELEASE_WAIT).is_ok()
    }

    /// TASK-181: the game ended; the Wheel page may open the wheel again.
    pub fn game_ended(&self) {
        self.game_running.store(false, Ordering::Relaxed);
    }

    pub fn game_running(&self) -> bool {
        self.game_running.load(Ordering::Relaxed)
    }

    /// TASK-181: the Wheel page is shown (`true`, renewed every two seconds) or gone (`false`).
    pub fn wheel_watch(&self, on: bool) {
        let _ = self.commands.send(Command::WheelWatch(on));
    }

    /// TASK-181: starts a wheel capture; the receiver gets the control, or `Err` when cancelled or impossible.
    pub fn start_wheel_capture(&self, ask: WheelCaptureAsk) -> Receiver<Result<WheelCaptureAnswer, String>> {
        let (reply, receiver) = channel();
        let _ = self.commands.send(Command::WheelCapture(ask, reply));
        receiver
    }

    pub fn cancel_wheel_capture(&self) {
        let _ = self.commands.send(Command::WheelCancel);
    }

    /// TASK-181: plays the force test; the receiver gets the device name and what the test found.
    pub fn test_force(&self, strength: f64, invert: bool) -> Receiver<Result<(String, ForceTest), String>> {
        let (reply, receiver) = channel();
        let _ = self.commands.send(Command::WheelTest { strength, invert, reply });
        receiver
    }
}

/// Starts the service thread and returns its handle. Manage the handle in Tauri's state.
pub fn spawn(app: AppHandle) -> Handle {
    let (commands, receiver) = channel();
    let handle = Handle {
        commands,
        status: Arc::new(Mutex::new(status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, "starting"))),
        active: Arc::new(AtomicBool::new(true)),
        wheel_status: Arc::new(Mutex::new(json!({ "available": false, "error": "starting", "devices": [] }))),
        game_running: Arc::new(AtomicBool::new(false)),
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

/// TASK-181: the `wheel.toml` GUID of the device the game drives (empty: the first wheel), read each time.
fn configured_wheel(app: &AppHandle) -> String {
    let shared = app.state::<Shared>();
    let state = state::lock(shared.inner());
    state.wheel.device_guid.clone()
}

/// TASK-181: the Wheel page's side of the service: the device list, the open joysticks, a capture, the force results.
#[derive(Default)]
struct Wheels {
    reader: WheelReader,
    entries: Vec<JoystickEntry>,
    counts: BTreeMap<u32, Counts>,
    /// Force test results of this launcher session, by GUID.
    features: BTreeMap<String, Features>,
    watch_until: Option<Instant>,
    listed_at: Option<Instant>,
    capture: Option<PendingWheelCapture>,
    last_status: Value,
}

struct PendingWheelCapture {
    state: WheelCaptureState,
    reply: Sender<Result<WheelCaptureAnswer, String>>,
}

impl Wheels {
    fn watching(&self) -> bool {
        self.watch_until.is_some_and(|until| Instant::now() < until)
    }

    /// Closes every joystick handle and ends a capture with `reason`.
    fn close(&mut self, reason: &str) {
        self.reader.close_all();
        if let Some(pending) = self.capture.take() {
            let _ = pending.reply.send(Err(reason.to_string()));
        }
    }

    fn status(&self, configured: &str, game_running: bool) -> Value {
        wheel_service::status_json(&wheel_service::StatusInputs {
            available: true,
            error: "",
            entries: &self.entries,
            counts: &self.counts,
            features: &self.features,
            configured_guid: configured,
            game_running,
            watching: self.watching(),
        })
    }

    /// Sends `wheel-status` when it changed, and keeps the `system.txt` facts in the state.
    fn publish(&mut self, app: &AppHandle, handle: &Handle, configured: &str, game_running: bool) {
        let status = self.status(configured, game_running);
        if status == self.last_status {
            return;
        }
        self.last_status = status.clone();
        if let Ok(mut slot) = handle.wheel_status.lock() {
            *slot = status.clone();
        }
        let facts = wheel_service::facts(&self.entries, &self.counts, &self.features, configured);
        {
            let shared = app.state::<Shared>();
            state::lock(shared.inner()).wheel_facts = facts;
        }
        let _ = app.emit("wheel-status", status);
    }

    /// One tick with SDL running: the list, the open handles, the live event and the capture.
    fn tick(&mut self, app: &AppHandle, handle: &Handle, pads: &SdlPads, devices_changed: bool, game_running: bool) {
        if devices_changed || self.listed_at.is_none_or(|at| at.elapsed() >= WHEEL_LIST_REFRESH) {
            self.entries = sdl_wheels::joysticks(pads);
            self.listed_at = Some(Instant::now());
            let entries = &self.entries;
            self.counts.retain(|id, _| entries.iter().any(|entry| entry.id == *id));
        }
        let configured = configured_wheel(app);
        if game_running || !self.watching() {
            if !self.reader.is_empty() || self.capture.is_some() {
                self.close(if game_running { "the game is running" } else { CAPTURE_CANCELLED });
            }
        } else {
            let wanted: Vec<u32> = wheel_service::page_devices(&self.entries).iter().map(|entry| entry.id).collect();
            self.reader.sync(pads.joystick_subsystem(), &wanted);
            for id in &wanted {
                if let Some(counts) = self.reader.counts(*id) {
                    self.counts.insert(*id, counts);
                }
            }
            let readings = self.reader.read(&self.entries);
            let _ = app.emit("wheel-live", wheel_service::live_json(&readings));
            let captured = self.capture.as_mut().and_then(|pending| pending.state.step(&readings));
            if let Some(captured) = captured {
                if let Some(pending) = self.capture.take() {
                    let device_name =
                        self.entries.iter().find(|entry| entry.id == captured.device).map(|entry| entry.name.clone()).unwrap_or_default();
                    let _ = pending.reply.send(Ok(WheelCaptureAnswer { captured, device_name }));
                }
            }
        }
        self.publish(app, handle, &configured, game_running);
    }

    /// Starts a capture: the device the game drives now is the one plain sources mean.
    fn start_capture(&mut self, app: &AppHandle, pads: &SdlPads, mut ask: WheelCaptureAsk, reply: Sender<Result<WheelCaptureAnswer, String>>) {
        if let Some(previous) = self.capture.take() {
            let _ = previous.reply.send(Err(CAPTURE_CANCELLED.to_string()));
        }
        self.entries = sdl_wheels::joysticks(pads);
        self.listed_at = Some(Instant::now());
        ask.request.primary = wheel_service::primary(&self.entries, &configured_wheel(app));
        self.watch_until = Some(Instant::now() + WHEEL_WATCH);
        self.capture = Some(PendingWheelCapture { state: WheelCaptureState::new(ask.request), reply });
    }

    /// The force test on the device the game drives. The joystick is the open one, or opened for the test only.
    fn test(&mut self, app: &AppHandle, pads: &SdlPads, strength: f64, invert: bool) -> Result<(String, ForceTest), String> {
        let language = state::current_language();
        let no_wheel = || gt4l_core::messages::text("wheel.no_wheel", language).to_string();
        self.entries = sdl_wheels::joysticks(pads);
        self.listed_at = Some(Instant::now());
        let id = wheel_service::primary(&self.entries, &configured_wheel(app)).ok_or_else(no_wheel)?;
        let entry = self.entries.iter().find(|entry| entry.id == id).cloned().ok_or_else(no_wheel)?;
        // The haptic subsystem lists the force feedback devices when it starts; it stops when `haptics` is dropped.
        let haptics = pads.haptic()?;
        let mut temporary = WheelReader::default();
        if !self.reader.is_open(id) {
            temporary.sync(pads.joystick_subsystem(), &[id]);
        }
        let reader = if self.reader.is_open(id) { &self.reader } else { &temporary };
        let result = reader.joystick(id).map(|joystick| sdl_wheels::test_force(joystick, sdl_wheels::pull_level(strength), invert));
        temporary.close_all();
        drop(haptics);
        let test = result.ok_or_else(no_wheel)?;
        if test.has_ffb {
            self.features.insert(entry.guid.clone(), test.features);
        }
        Ok((entry.name, test))
    }
}

/// Starts SDL and loads the player's gamepad mappings. `skip_wheels`: the game runs, no racing wheel is opened.
fn start_sdl(app: &AppHandle, skip_wheels: bool) -> Result<SdlPads, String> {
    let mut pads = SdlPads::start()?;
    pads.set_skip_wheels(skip_wheels);
    // TASK-148: the player's own SDL gamepad mappings (gamecontrollerdb.txt in the config folder), when there are any.
    let mappings = {
        let shared = app.state::<Shared>();
        let state = state::lock(shared.inner());
        state.user_dirs.config_dir().join(gt4l_core::launch::GAMEPAD_MAPPINGS_FILE)
    };
    if mappings.exists() {
        match pads.load_mappings(&mappings) {
            Ok(added) => log(app, &format!("controller input: {added} gamepad mappings from {}", mappings.display())),
            Err(message) => log(app, &format!("controller input: gamepad mappings not loaded: {message}")),
        }
    }
    Ok(pads)
}

fn run(app: AppHandle, commands: Receiver<Command>, handle: Handle) {
    let mut sdl: Option<SdlPads> = match start_sdl(&app, false) {
        Ok(pads) => {
            log(&app, "controller input: SDL3 gamepad subsystem started");
            Some(pads)
        }
        Err(message) => {
            log(&app, &format!("controller input unavailable: {message}"));
            publish(Some(&app), &handle, status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, &message));
            let wheel_status = json!({ "available": false, "error": message, "devices": [] });
            if let Ok(mut slot) = handle.wheel_status.lock() {
                *slot = wheel_status.clone();
            }
            let _ = app.emit("wheel-status", wheel_status);
            // Keep answering: a capture fails with the reason instead of waiting for a pad that cannot come.
            while let Ok(command) = commands.recv() {
                match command {
                    Command::Capture(reply) => {
                        let _ = reply.send(Err(message.clone()));
                    }
                    Command::WheelCapture(_, reply) => {
                        let _ = reply.send(Err(message.clone()));
                    }
                    Command::WheelTest { reply, .. } => {
                        let _ = reply.send(Err(message.clone()));
                    }
                    Command::Release(reply) => {
                        let _ = reply.send(());
                    }
                    Command::Cancel | Command::WheelCancel | Command::WheelWatch(_) => {}
                }
            }
            return;
        }
    };

    let mut navigator = Navigator::default();
    let mut capture: Option<PendingCapture> = None;
    let mut wheels = Wheels::default();
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
                Command::Release(reply) => {
                    // TASK-181: the game starts. Every handle goes (the wheel's, the gamepads', any haptic one): SDL shuts
                    // down when `SdlPads` is dropped.
                    wheels.close("the game is running");
                    if let Some(pending) = capture.take() {
                        let _ = pending.reply.send(Err("controller input is paused while the game runs".to_string()));
                    }
                    if sdl.take().is_some() {
                        log(&app, "controller input: SDL3 closed for the game (the game owns the wheel and its force feedback)");
                    }
                    navigator = Navigator::default();
                    let _ = reply.send(());
                }
                Command::WheelWatch(on) => {
                    wheels.watch_until = on.then(|| Instant::now() + WHEEL_WATCH);
                }
                Command::WheelCapture(ask, reply) => match (&sdl, handle.game_running()) {
                    (_, true) => {
                        let _ = reply.send(Err(gt4l_core::messages::text("wheel.game_running", state::current_language()).to_string()));
                    }
                    (None, false) => {
                        let _ = reply.send(Err("SDL3 is not running".to_string()));
                    }
                    (Some(pads), false) => wheels.start_capture(&app, pads, ask, reply),
                },
                Command::WheelCancel => {
                    if let Some(pending) = wheels.capture.take() {
                        let _ = pending.reply.send(Err(CAPTURE_CANCELLED.to_string()));
                    }
                }
                Command::WheelTest { strength, invert, reply } => {
                    let answer = match (&sdl, handle.game_running()) {
                        (_, true) => Err(gt4l_core::messages::text("wheel.game_running", state::current_language()).to_string()),
                        (None, false) => Err("SDL3 is not running".to_string()),
                        (Some(pads), false) => wheels.test(&app, pads, strength, invert),
                    };
                    match &answer {
                        Ok((name, test)) => log(
                            &app,
                            &format!(
                                "wheel: force test on {name}: haptics {}, effects {}, played {}{}",
                                if test.has_ffb { "yes" } else { "no" },
                                test.features.text(),
                                if test.played { "yes" } else { "no" },
                                if test.error.is_empty() { String::new() } else { format!(" ({})", test.error) }
                            ),
                        ),
                        Err(message) => log(&app, &format!("wheel: force test not run: {message}")),
                    }
                    let _ = reply.send(answer);
                }
            }
        }

        let game_running = handle.game_running();
        if !handle.active.load(Ordering::Relaxed) {
            if was_active {
                was_active = false;
                navigator = Navigator::default();
                unsettled_since = None;
                if let Some(pending) = capture.take() {
                    let _ = pending.reply.send(Err("controller input is paused while the game runs".to_string()));
                }
            }
            // A window hidden for the game holds nothing of SDL.
            if game_running {
                wheels.close("the game is running");
                if sdl.take().is_some() {
                    log(&app, "controller input: SDL3 closed while the game runs");
                }
            }
            continue;
        }
        was_active = true;

        if sdl.is_none() {
            // SDL starts again when the window is shown: after the game, or while it runs (then without any wheel).
            match start_sdl(&app, game_running) {
                Ok(pads) => {
                    log(
                        &app,
                        if game_running {
                            "controller input: SDL3 started again (no racing wheel while the game runs)"
                        } else {
                            "controller input: SDL3 started again"
                        },
                    );
                    wheels.listed_at = None;
                    sdl = Some(pads);
                }
                Err(message) => {
                    publish(Some(&app), &handle, status_json(false, &[], &[], None, pad::AUTO_CONTROLLER, &message));
                    continue;
                }
            }
        }
        let Some(pads) = sdl.as_mut() else {
            continue;
        };
        pads.set_skip_wheels(game_running);
        let devices_changed = pads.pump();
        wheels.tick(&app, &handle, pads, devices_changed, game_running);
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
