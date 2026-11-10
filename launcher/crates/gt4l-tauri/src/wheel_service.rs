//! The Wheel page's side of the pad service (TASK-181): the payloads of the `wheel-status` and `wheel-live` events, the
//! device the game drives, and the facts `system.txt` lists. Pure: `pad_service` owns SDL and calls these.
//!
//! Events:
//! - `wheel-status` `{available, error, game_running, watching, primary, configured, configured_connected, devices}`:
//!   on every change. `devices` lists the wheels and the auxiliary joysticks (pedals, shifters); gamepads belong to the
//!   Controls page. A device is `{id, guid, name, vid_pid, sdl_type, is_gamepad, role, axes, buttons, hats, haptic,
//!   features, family, family_name, model, matched_by, software, direct_drive, force_feedback, notes}`; the counts and
//!   `haptic` are `null` until the page's live view opened the device, `features` until a force test ran.
//! - `wheel-live` `{devices: [{id, guid, axes, pressed, hats}]}`: about 60 times a second while the Wheel page is shown
//!   (`wheel_watch`), the game is not running and a device is open. `pressed` holds the indices of the buttons down.

use std::collections::BTreeMap;

use gt4l_core::log_export::WheelFact;
use gt4l_core::wheel_capture::Reading;
use gt4l_core::wheel_identity;
use gt4l_platform::sdl_wheels::{Counts, Features, JoystickEntry};
use serde_json::{json, Value};

/// The device the game drives (`select_primary_device` in `native/wheel.cpp`): the one with the configured GUID when
/// exactly one has it; with no GUID configured, the first wheel. `None` when that device is not there.
pub fn primary(entries: &[JoystickEntry], configured_guid: &str) -> Option<u32> {
    if !configured_guid.is_empty() {
        let mut matching = entries.iter().filter(|entry| entry.guid.eq_ignore_ascii_case(configured_guid));
        let first = matching.next()?;
        return matching.next().is_none().then_some(first.id);
    }
    entries.iter().find(|entry| entry.is_wheel()).map(|entry| entry.id)
}

/// The devices the Wheel page lists.
pub fn page_devices(entries: &[JoystickEntry]) -> Vec<&JoystickEntry> {
    entries.iter().filter(|entry| entry.is_wheel_page_device()).collect()
}

/// TASK-189: whether a `wheel-status` payload lists a wheel, which is exactly when Play plugs the virtual Driving Force
/// Pro into the game. `None` when the status cannot say (SDL3 did not start or has not listed anything yet): the caller
/// then keeps what it had instead of switching the wheel off on a guess. Auxiliary devices (a pedal set or a shifter on
/// its own USB cable) are never the wheel.
pub fn wheel_connected(status: &Value) -> Option<bool> {
    if !status.get("available").and_then(Value::as_bool)? {
        return None;
    }
    let devices = status.get("devices")?.as_array()?;
    Some(devices.iter().any(|device| {
        matches!(device.get("role").and_then(Value::as_str), Some("wheel") | Some("other_wheel"))
    }))
}

fn features_json(features: &Features) -> Value {
    json!({
        "constant": features.constant,
        "spring": features.spring,
        "damper": features.damper,
        "friction": features.friction,
        "gain": features.gain,
        "autocenter": features.autocenter,
    })
}

/// `046D:C29B`, or empty when the GUID holds no IDs.
pub fn vid_pid_text(guid: &str) -> String {
    wheel_identity::vid_pid(guid).map(|(vendor, product)| format!("{vendor:04X}:{product:04X}")).unwrap_or_default()
}

/// One device of the `wheel-status` payload.
pub fn device_json(entry: &JoystickEntry, counts: Option<&Counts>, features: Option<&Features>, primary: Option<u32>) -> Value {
    let identity = wheel_identity::identify(&entry.guid, &entry.name, entry.sdl_wheel);
    let family = identity.family;
    json!({
        "id": entry.id,
        "guid": entry.guid,
        "name": entry.name,
        "vid_pid": vid_pid_text(&entry.guid),
        "sdl_type": entry.sdl_type(),
        "is_gamepad": entry.is_gamepad,
        "role": if Some(entry.id) == primary { "wheel" } else if entry.is_wheel() { "other_wheel" } else { "auxiliary" },
        "axes": counts.map(|counts| counts.axes),
        "buttons": counts.map(|counts| counts.buttons),
        "hats": counts.map(|counts| counts.hats),
        "haptic": counts.map(|counts| counts.haptic),
        "features": features.map(features_json),
        "family": family.map(|family| family.id),
        "family_name": family.map(|family| family.name),
        "model": identity.model,
        "matched_by": identity.matched_by,
        "software": family.map(|family| family.software).unwrap_or(""),
        "direct_drive": family.is_some_and(|family| family.direct_drive),
        "force_feedback": family.is_none_or(|family| family.force_feedback),
        "notes": identity.notes,
    })
}

/// What the status is built from.
pub struct StatusInputs<'a> {    pub available: bool,
    pub error: &'a str,
    pub entries: &'a [JoystickEntry],
    pub counts: &'a BTreeMap<u32, Counts>,
    /// Force test results this launcher session, by GUID.
    pub features: &'a BTreeMap<String, Features>,
    pub configured_guid: &'a str,
    pub game_running: bool,
    pub watching: bool,
}

/// The `wheel-status` payload.
pub fn status_json(inputs: &StatusInputs<'_>) -> Value {
    let primary = primary(inputs.entries, inputs.configured_guid);
    let devices: Vec<Value> = page_devices(inputs.entries)
        .into_iter()
        .map(|entry| device_json(entry, inputs.counts.get(&entry.id), inputs.features.get(&entry.guid), primary))
        .collect();
    json!({
        "available": inputs.available,
        "error": inputs.error,
        "game_running": inputs.game_running,
        "watching": inputs.watching,
        "primary": primary,
        "configured": inputs.configured_guid,
        "configured_connected": !inputs.configured_guid.is_empty()
            && inputs.entries.iter().any(|entry| entry.guid.eq_ignore_ascii_case(inputs.configured_guid)),
        "devices": devices,
    })
}

/// The `wheel-live` payload.
pub fn live_json(readings: &[Reading]) -> Value {
    let devices: Vec<Value> = readings
        .iter()
        .map(|reading| {
            let pressed: Vec<usize> = reading.buttons.iter().enumerate().filter(|(_, down)| **down).map(|(index, _)| index).collect();
            json!({ "id": reading.id, "guid": reading.guid, "axes": reading.axes, "pressed": pressed, "hats": reading.hats })
        })
        .collect();
    json!({ "devices": devices })
}

/// The `system.txt` facts of the connected devices (TASK-181): names, IDs, SDL type, role and what is known of their
/// force feedback. Never a serial number or a path.
pub fn facts(entries: &[JoystickEntry], counts: &BTreeMap<u32, Counts>, features: &BTreeMap<String, Features>, configured_guid: &str) -> Vec<WheelFact> {
    let primary = primary(entries, configured_guid);
    page_devices(entries)
        .into_iter()
        .map(|entry| WheelFact {
            name: entry.name.clone(),
            vid_pid: vid_pid_text(&entry.guid),
            sdl_type: entry.sdl_type().to_string(),
            role: if Some(entry.id) == primary { "wheel".into() } else { "auxiliary".into() },
            haptic: match (features.get(&entry.guid), counts.get(&entry.id)) {
                (Some(found), _) => found.text(),
                (None, Some(counts)) => (if counts.haptic { "yes (effects not read)" } else { "no" }).to_string(),
                (None, None) => String::new(),
            },
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const G27: &str = "030000006d0400009bc2000000000000";
    const PEDALS: &str = "03000000b70e00003b18000000000000";

    fn entry(id: u32, name: &str, guid: &str, sdl_wheel: bool, is_gamepad: bool) -> JoystickEntry {
        JoystickEntry { id, guid: guid.into(), name: name.into(), sdl_wheel, is_gamepad }
    }

    fn setup() -> Vec<JoystickEntry> {
        vec![
            entry(3, "Xbox Wireless Controller", "030000005e040000130b000000000000", false, true),
            entry(5, "Logitech G27 Racing Wheel USB", G27, true, false),
            entry(7, "ClubSport Pedals", PEDALS, false, false),
        ]
    }

    #[test]
    fn the_primary_is_the_configured_guid_else_the_first_wheel() {
        let entries = setup();
        assert_eq!(primary(&entries, ""), Some(5));
        assert_eq!(primary(&entries, PEDALS), Some(7), "a configured device is driven even when it is not a wheel");
        assert_eq!(primary(&entries, "0300000000000000000000000000ffff"), None, "the configured wheel is not connected");
        let mut twins = entries.clone();
        twins.push(entry(9, "Logitech G27 Racing Wheel USB", G27, true, false));
        assert_eq!(primary(&twins, G27), None, "two devices share the GUID: the game drives none");
    }

    #[test]
    fn the_status_lists_wheels_and_auxiliaries_with_their_identity() {
        let entries = setup();
        let mut counts = BTreeMap::new();
        counts.insert(5, Counts { axes: 6, buttons: 23, hats: 1, haptic: true });
        let mut features = BTreeMap::new();
        features.insert(G27.to_string(), Features { constant: true, spring: true, damper: true, friction: true, gain: true, autocenter: true });
        let status = status_json(&StatusInputs {
            available: true,
            error: "",
            entries: &entries,
            counts: &counts,
            features: &features,
            configured_guid: "",
            game_running: false,
            watching: true,
        });
        let devices = status["devices"].as_array().unwrap();
        assert_eq!(devices.len(), 2, "the gamepad belongs to the Controls page");
        assert_eq!(status["primary"], json!(5));
        let wheel = &devices[0];
        assert_eq!(wheel["vid_pid"], json!("046D:C29B"));
        assert_eq!(wheel["family"], json!("logitech_legacy"));
        assert_eq!(wheel["model"], json!("G27"));
        assert_eq!(wheel["role"], json!("wheel"));
        assert_eq!(wheel["axes"], json!(6));
        assert_eq!(wheel["features"]["autocenter"], json!(true));
        assert_eq!(wheel["software"], json!("Logitech G HUB or Logitech Profiler"));
        assert_eq!(devices[1]["role"], json!("auxiliary"));
        assert_eq!(devices[1]["axes"], Value::Null, "not opened: the counts are unknown");
        let facts = facts(&entries, &counts, &features, "");
        assert_eq!(facts.len(), 2);
        assert_eq!(facts[0].haptic, "constant spring damper friction gain autocenter");
        assert_eq!(facts[0].role, "wheel");
        assert_eq!(facts[1].haptic, "");
    }

    #[test]
    fn the_live_payload_lists_the_pressed_buttons() {
        let reading = Reading { id: 5, guid: G27.into(), axes: vec![-16384, 32767], buttons: vec![false, true, false, true], hats: vec![2] };
        let live = live_json(&[reading]);
        assert_eq!(live["devices"][0]["pressed"], json!([1, 3]));
        assert_eq!(live["devices"][0]["axes"], json!([-16384, 32767]));
        assert_eq!(live["devices"][0]["hats"], json!([2]));
    }

    /// TASK-189: Play plugs the virtual Driving Force Pro in only when the status lists a wheel, and says it does not
    /// know when SDL3 could not list the devices.
    #[test]
    fn a_wheel_is_connected_only_when_the_status_lists_one() {
        let wheel = json!({ "available": true, "primary": 1, "devices": [{ "id": 1, "role": "wheel" }] });
        assert_eq!(wheel_connected(&wheel), Some(true));
        let other = json!({ "available": true, "primary": null, "devices": [{ "id": 1, "role": "other_wheel" }] });
        assert_eq!(wheel_connected(&other), Some(true), "a second wheel is still a wheel");
        let pedals = json!({ "available": true, "primary": null, "devices": [{ "id": 2, "role": "auxiliary" }] });
        assert_eq!(wheel_connected(&pedals), Some(false), "a pedal set on its own USB cable is not a wheel");
        assert_eq!(wheel_connected(&json!({ "available": true, "devices": [] })), Some(false));
        assert_eq!(wheel_connected(&json!({ "available": false, "error": "starting", "devices": [] })), None);
        assert_eq!(wheel_connected(&json!({})), None);
        assert_eq!(wheel_connected(&Value::Null), None);
    }
}
