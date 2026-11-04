//! The SDL3 gamepad reader. Only the gamepad subsystem is started (no video, no window: the Tauri window and its
//! event loop are not touched). Owned by one thread (the launcher's pad service), which calls `pump` and `read`
//! each tick. SDL is not `Send`, so the reader is created on that thread.

use std::collections::{BTreeMap, BTreeSet};
use std::ffi::CStr;
use std::path::Path;

use sdl3::event::Event;
use sdl3::gamepad::{Axis, Button, Gamepad};
use sdl3::joystick::JoystickId;
use sdl3::{GamepadSubsystem, JoystickSubsystem};
use sdl3::{EventPump, Sdl};

use crate::pad::RawPad;

/// One connected pad as SDL names it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PadInfo {
    /// SDL's joystick instance id (the order "controller 1 of N" uses).
    pub id: u32,
    /// The name SDL reports, e.g. "Xbox 360 Controller" or "DualSense Wireless Controller".
    pub name: String,
    /// SDL's gamepad type string, e.g. "xbox360", "ps5", "switch_pro" (empty when unknown).
    pub kind: String,
    /// SDL's player index, when the platform assigns one.
    pub player_index: Option<u16>,
    /// TASK-148: the GUID SDL3 gives the model (32 hex digits) and the serial number when the pad reports one: what the
    /// `controller` setting stores (`pad::PadChoice`).
    pub guid: String,
    pub serial: String,
    /// False for a joystick SDL3 has no gamepad mapping for: neither the launcher nor the game can read it as a gamepad.
    pub mapped: bool,
}

pub struct SdlPads {
    // Kept alive for the subsystem and the event pump; dropping it quits SDL.
    _sdl: Sdl,
    subsystem: GamepadSubsystem,
    joysticks: JoystickSubsystem,
    events: EventPump,
    open: BTreeMap<u32, Gamepad>,
}

impl SdlPads {
    /// Starts SDL's gamepad subsystem and opens the pads that are connected now.
    pub fn start() -> Result<SdlPads, String> {
        let sdl = sdl3::init().map_err(|error| format!("SDL3 did not start: {error}"))?;
        let subsystem = sdl.gamepad().map_err(|error| format!("SDL3 gamepads did not start: {error}"))?;
        let joysticks = sdl.joystick().map_err(|error| format!("SDL3 joysticks did not start: {error}"))?;
        let events = sdl.event_pump().map_err(|error| format!("SDL3 events did not start: {error}"))?;
        let mut pads = SdlPads { _sdl: sdl, subsystem, joysticks, events, open: BTreeMap::new() };
        pads.rescan();
        Ok(pads)
    }

    /// Reads SDL's events (hot plug) and refreshes the pad states. Call once per tick while the launcher is active.
    pub fn pump(&mut self) {
        let mut changed = false;
        for event in self.events.poll_iter() {
            if matches!(
                event,
                Event::GamepadAdded { .. } | Event::GamepadRemoved { .. } | Event::JoyDeviceAdded { .. } | Event::JoyDeviceRemoved { .. }
            ) {
                changed = true;
            }
        }
        if changed {
            self.rescan();
        }
        self.subsystem.update();
    }

    /// Opens the pads that appeared and closes the ones that went away.
    fn rescan(&mut self) {
        let ids = self.subsystem.gamepads().unwrap_or_default();
        let live: BTreeSet<u32> = ids.iter().map(|id| u32::from(*id)).collect();
        self.open.retain(|id, _| live.contains(id));
        for id in ids {
            let raw = u32::from(id);
            if !self.open.contains_key(&raw) {
                if let Ok(pad) = self.subsystem.open(id) {
                    self.open.insert(raw, pad);
                }
            }
        }
    }

    /// The connected pads in SDL's order.
    pub fn connected(&self) -> Vec<PadInfo> {
        self.open
            .iter()
            .map(|(id, pad)| PadInfo {
                id: *id,
                name: pad.name().filter(|name| !name.trim().is_empty()).unwrap_or_else(|| "Controller".to_string()),
                kind: pad.r#type().string(),
                player_index: pad.player_index(),
                guid: self.subsystem.guid_for_id(JoystickId::from(*id)).string(),
                serial: pad.serial_number().unwrap_or_default(),
                mapped: true,
            })
            .collect()
    }

    /// TASK-148: the joysticks SDL3 sees but has no gamepad mapping for (an old or unusual controller). They can be
    /// named, not read; a mapping for them in the SDL mappings file (`load_mappings`) turns them into gamepads.
    pub fn unrecognized(&self) -> Vec<PadInfo> {
        self.joysticks
            .joysticks()
            .unwrap_or_default()
            .into_iter()
            .filter(|id| !self.subsystem.is_gamepad(*id))
            .map(|id| {
                let raw = u32::from(id);
                // SAFETY: SDL returns a string it owns (or null) for a connected joystick id; it is copied at once.
                let name = unsafe {
                    let text = sdl3::sys::joystick::SDL_GetJoystickNameForID(sdl3::sys::joystick::SDL_JoystickID(raw));
                    if text.is_null() { String::new() } else { CStr::from_ptr(text).to_string_lossy().into_owned() }
                };
                PadInfo {
                    id: raw,
                    name: if name.trim().is_empty() { "Controller".to_string() } else { name },
                    kind: String::new(),
                    player_index: None,
                    // SDL_GetGamepadGUIDForID gives the joystick GUID for any connected joystick id.
                    guid: self.subsystem.guid_for_id(id).string(),
                    serial: String::new(),
                    mapped: false,
                }
            })
            .collect()
    }

    /// TASK-148: adds the gamepad mappings of an SDL mappings file (`gamecontrollerdb.txt` format) and opens the pads
    /// they make readable. Returns how many mappings were added.
    pub fn load_mappings(&mut self, path: &Path) -> Result<i32, String> {
        let added = self.subsystem.load_mappings(path).map_err(|error| format!("{}: {error}", path.display()))?;
        self.rescan();
        Ok(added)
    }

    /// The raw reading of one pad, or `None` when it is not connected.
    pub fn read(&self, id: u32) -> Option<RawPad> {
        let pad = self.open.get(&id)?;
        let button = |which: Button| pad.button(which);
        let axis = |which: Axis| pad.axis(which);
        Some(RawPad {
            south: button(Button::South),
            east: button(Button::East),
            west: button(Button::West),
            north: button(Button::North),
            left_shoulder: button(Button::LeftShoulder),
            right_shoulder: button(Button::RightShoulder),
            back: button(Button::Back),
            start: button(Button::Start),
            left_stick: button(Button::LeftStick),
            right_stick: button(Button::RightStick),
            dpad_up: button(Button::DPadUp),
            dpad_down: button(Button::DPadDown),
            dpad_left: button(Button::DPadLeft),
            dpad_right: button(Button::DPadRight),
            left_trigger: axis(Axis::TriggerLeft),
            right_trigger: axis(Axis::TriggerRight),
            left_x: axis(Axis::LeftX),
            left_y: axis(Axis::LeftY),
            right_x: axis(Axis::RightX),
            right_y: axis(Axis::RightY),
        })
    }
}
