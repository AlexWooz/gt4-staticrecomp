//! Gamepad mapping and capture rules, pure and tested without a controller. The pad is read through SDL3
//! (`sdl_pads`), the same layer and controller database the game uses.
//!
//! Each SDL control becomes a source of the controls file (`gt4controls`): SDL's SOUTH is `xinput:A` and so on.
//! The file keeps its names, so a pad that SDL maps (Xbox, DualShock 4, DualSense, Switch Pro, generic) reaches
//! the DS2 buttons through the same bindings as before. The mapping, in one place:
//!
//! | SDL                        | controls file source              | default DS2 button |
//! | -------------------------- | --------------------------------- | ------------------ |
//! | SOUTH                      | `xinput:A`                        | Cross              |
//! | EAST                       | `xinput:B`                        | Circle             |
//! | WEST                       | `xinput:X`                        | Square             |
//! | NORTH                      | `xinput:Y`                        | Triangle           |
//! | LEFT / RIGHT SHOULDER      | `xinput:LB` / `xinput:RB`         | L1 / R1            |
//! | LEFT / RIGHT TRIGGER       | `xinput:LT` / `xinput:RT`         | L2 / R2 (pressure) |
//! | LEFT / RIGHT STICK (click) | `xinput:LThumb` / `xinput:RThumb` | L3 / R3            |
//! | BACK / START               | `xinput:Back` / `xinput:Start`    | Select / Start     |
//! | D-pad                      | `xinput:DpadUp` and so on         | D-pad              |
//! | stick axes                 | `xinput:LX` `LY` `RX` `RY`        | sticks             |

use std::collections::BTreeMap;

/// Pressure at or above which a source counts as pressed (half travel, as the old capture did).
pub const PRESS: u8 = 128;

/// One SDL reading of a pad, the parts the launcher uses. Triggers are SDL's raw 0..=32767; sticks are raw
/// -32768..=32767 with y positive down (SDL's convention).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RawPad {
    pub south: bool,
    pub east: bool,
    pub west: bool,
    pub north: bool,
    pub left_shoulder: bool,
    pub right_shoulder: bool,
    pub back: bool,
    pub start: bool,
    pub left_stick: bool,
    pub right_stick: bool,
    pub dpad_up: bool,
    pub dpad_down: bool,
    pub dpad_left: bool,
    pub dpad_right: bool,
    pub left_trigger: i16,
    pub right_trigger: i16,
    pub left_x: i16,
    pub left_y: i16,
    pub right_x: i16,
    pub right_y: i16,
}

/// The mapped pad: the pressure of each controls-file source (digital 0 or 255, triggers 0..=255) and the raw
/// stick axes (same units as `RawPad`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PadSample {
    pub pressure: BTreeMap<&'static str, u8>,
    pub left_x: i16,
    pub left_y: i16,
    pub right_x: i16,
    pub right_y: i16,
}

/// The order in which a capture looks for a newly pressed control: buttons first, then the triggers.
const CAPTURE_ORDER: &[&str] = &[
    "xinput:A",
    "xinput:B",
    "xinput:X",
    "xinput:Y",
    "xinput:LB",
    "xinput:RB",
    "xinput:Back",
    "xinput:Start",
    "xinput:LThumb",
    "xinput:RThumb",
    "xinput:DpadUp",
    "xinput:DpadDown",
    "xinput:DpadLeft",
    "xinput:DpadRight",
    "xinput:LT",
    "xinput:RT",
];

/// Maps one SDL reading to controls-file sources (the table in the module comment).
pub fn sample(raw: &RawPad) -> PadSample {
    let digital = |on: bool| if on { 255u8 } else { 0u8 };
    let trigger = |value: i16| -> u8 {
        let clamped = i32::from(value).clamp(0, 32_767);
        // Match the native SDL adapter: standardized trigger units reduced to one pressure byte.
        (clamped >> 7) as u8
    };
    let pairs: [(&'static str, u8); 16] = [
        ("xinput:A", digital(raw.south)),
        ("xinput:B", digital(raw.east)),
        ("xinput:X", digital(raw.west)),
        ("xinput:Y", digital(raw.north)),
        ("xinput:LB", digital(raw.left_shoulder)),
        ("xinput:RB", digital(raw.right_shoulder)),
        ("xinput:Back", digital(raw.back)),
        ("xinput:Start", digital(raw.start)),
        ("xinput:LThumb", digital(raw.left_stick)),
        ("xinput:RThumb", digital(raw.right_stick)),
        ("xinput:DpadUp", digital(raw.dpad_up)),
        ("xinput:DpadDown", digital(raw.dpad_down)),
        ("xinput:DpadLeft", digital(raw.dpad_left)),
        ("xinput:DpadRight", digital(raw.dpad_right)),
        ("xinput:LT", trigger(raw.left_trigger)),
        ("xinput:RT", trigger(raw.right_trigger)),
    ];
    PadSample {
        pressure: pairs.into_iter().collect(),
        left_x: raw.left_x,
        left_y: raw.left_y,
        right_x: raw.right_x,
        right_y: raw.right_y,
    }
}

/// The source of the first control that is pressed now and was not pressed in `baseline` (a button held when
/// the capture started does not count). `None` while nothing new is pressed.
pub fn captured_source(baseline: &PadSample, now: &PadSample) -> Option<String> {
    CAPTURE_ORDER.iter().find_map(|name| {
        let pressed_now = now.pressure.get(name).copied().unwrap_or(0) >= PRESS;
        let pressed_before = baseline.pressure.get(name).copied().unwrap_or(0) >= PRESS;
        (pressed_now && !pressed_before).then(|| (*name).to_string())
    })
}

/// "Controller 1 of N": the pad the game reads is the first connected one in SDL's order (the lowest joystick
/// instance id). `ids` may come in any order. Returns the chosen id, or `None` with no pad.
pub fn active_pad(ids: &[u32]) -> Option<u32> {
    ids.iter().copied().min()
}

// TASK-148: the choice itself lives in gt4l-core (the `controller` setting validates it there).
pub use gt4l_core::controller::{is_controller_setting, PadChoice, AUTO_CONTROLLER};

/// One connected pad as the choice sees it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PadIdentity<'a> {
    pub id: u32,
    pub guid: &'a str,
    pub serial: &'a str,
}

/// TASK-148: the pad to read. The chosen one when it is connected: the same GUID and serial, or else the first pad of
/// the same model (the same unit is not there, another of its kind is); without a choice, or with the chosen model
/// unplugged, controller 1 (`active_pad`). The game applies the same rule.
pub fn chosen_pad(pads: &[PadIdentity<'_>], choice: Option<&PadChoice>) -> Option<u32> {
    if let Some(choice) = choice {
        let same_model = |pad: &&PadIdentity<'_>| pad.guid.eq_ignore_ascii_case(&choice.guid);
        if !choice.serial.is_empty() {
            if let Some(pad) = pads.iter().filter(same_model).filter(|pad| pad.serial == choice.serial).min_by_key(|pad| pad.id) {
                return Some(pad.id);
            }
        }
        if let Some(pad) = pads.iter().filter(same_model).min_by_key(|pad| pad.id) {
            return Some(pad.id);
        }
    }
    let ids: Vec<u32> = pads.iter().map(|pad| pad.id).collect();
    active_pad(&ids)
}

#[cfg(test)]
mod tests {
    use super::*;

    const XBOX: &str = "030000005e0400008e02000014010000";
    const DUALSENSE: &str = "050000004c050000e60c000000810000";

    #[test]
    fn the_chosen_pad_wins_over_controller_one_and_falls_back_to_it() {
        let pads = [
            PadIdentity { id: 3, guid: XBOX, serial: "" },
            PadIdentity { id: 7, guid: DUALSENSE, serial: "a0:ab:51:00:00:01" },
            PadIdentity { id: 9, guid: DUALSENSE, serial: "a0:ab:51:00:00:02" },
        ];
        assert_eq!(chosen_pad(&pads, None), Some(3), "no choice: controller 1, the lowest id");
        let second = PadChoice::parse(&format!("{DUALSENSE}/a0:ab:51:00:00:02")).unwrap();
        assert_eq!(chosen_pad(&pads, Some(&second)), Some(9), "the same unit by its serial");
        let gone_unit = PadChoice::parse(&format!("{DUALSENSE}/a0:ab:51:00:00:03")).unwrap();
        assert_eq!(chosen_pad(&pads, Some(&gone_unit)), Some(7), "another pad of the chosen model");
        let model = PadChoice::parse(DUALSENSE).unwrap();
        assert_eq!(chosen_pad(&pads, Some(&model)), Some(7));
        let unplugged = PadChoice::parse("03000000d620000011a7000000000000").unwrap();
        assert_eq!(chosen_pad(&pads, Some(&unplugged)), Some(3), "the chosen model is unplugged: controller 1");
        assert_eq!(chosen_pad(&[], Some(&model)), None);
    }

    fn raw() -> RawPad {
        RawPad::default()
    }

    #[test]
    fn south_east_west_north_are_cross_circle_square_triangle_sources() {
        let mut pad = raw();
        pad.south = true;
        let sampled = sample(&pad);
        assert_eq!(sampled.pressure["xinput:A"], 255, "SOUTH is the Cross source");
        assert_eq!(sampled.pressure["xinput:B"], 0);
        pad = raw();
        pad.east = true;
        assert_eq!(sample(&pad).pressure["xinput:B"], 255, "EAST is the Circle source");
        pad = raw();
        pad.west = true;
        assert_eq!(sample(&pad).pressure["xinput:X"], 255, "WEST is the Square source");
        pad = raw();
        pad.north = true;
        assert_eq!(sample(&pad).pressure["xinput:Y"], 255, "NORTH is the Triangle source");
    }

    #[test]
    fn shoulders_sticks_menu_and_dpad_map_to_their_sources() {
        let mut pad = raw();
        pad.left_shoulder = true;
        pad.right_shoulder = true;
        pad.back = true;
        pad.start = true;
        pad.left_stick = true;
        pad.right_stick = true;
        pad.dpad_left = true;
        let sampled = sample(&pad);
        for name in ["xinput:LB", "xinput:RB", "xinput:Back", "xinput:Start", "xinput:LThumb", "xinput:RThumb", "xinput:DpadLeft"] {
            assert_eq!(sampled.pressure[name], 255, "{name}");
        }
        assert_eq!(sampled.pressure["xinput:DpadRight"], 0);
    }

    #[test]
    fn triggers_scale_to_0_255_and_keep_pressure() {
        let mut pad = raw();
        pad.left_trigger = 32_767;
        pad.right_trigger = 16_384;
        let sampled = sample(&pad);
        assert_eq!(sampled.pressure["xinput:LT"], 255);
        assert_eq!(sampled.pressure["xinput:RT"], 128, "half of SDL's travel is about half of 255");
        pad.left_trigger = -5;
        assert_eq!(sample(&pad).pressure["xinput:LT"], 0, "negative raw values are clamped");
    }

    #[test]
    fn all_standardized_gamepads_keep_the_same_trigger_range_and_capture_threshold() {
        // SDL supplies this range for Xbox, DualShock and DualSense alike, independently of HID ranges.
        let baseline = sample(&raw());
        for (value, byte) in [(0, 0), (8192, 64), (16384, 128), (24576, 192), (32767, 255)] {
            let mut pad = raw();
            pad.left_trigger = value;
            let now = sample(&pad);
            assert_eq!(now.pressure["xinput:LT"], byte);
            assert_eq!(now.pressure["xinput:RT"], 0);
            assert_eq!(captured_source(&baseline, &now).is_some(), byte >= PRESS);
        }
        assert_eq!(captured_source(&baseline, &sample(&raw())), None, "released triggers never capture");
        for value in -32768..0 {
            let mut pad = raw();
            pad.left_trigger = value;
            assert_eq!(sample(&pad).pressure["xinput:LT"], 0, "invalid negative values clamp to rest");
        }
    }

    #[test]
    fn sticks_keep_their_raw_axes() {
        let mut pad = raw();
        pad.left_x = -32_768;
        pad.right_y = 20_000;
        let sampled = sample(&pad);
        assert_eq!((sampled.left_x, sampled.right_y), (-32_768, 20_000));
    }

    #[test]
    fn a_newly_pressed_button_is_captured_by_its_source_name() {
        let baseline = sample(&raw());
        let mut pad = raw();
        pad.south = true;
        assert_eq!(captured_source(&baseline, &sample(&pad)), Some("xinput:A".to_string()));
        let mut pad = raw();
        pad.north = true;
        assert_eq!(captured_source(&baseline, &sample(&pad)), Some("xinput:Y".to_string()));
    }

    #[test]
    fn a_button_held_before_the_capture_does_not_bind() {
        let mut held = raw();
        held.east = true;
        let baseline = sample(&held);
        assert_eq!(captured_source(&baseline, &sample(&held)), None);
    }

    #[test]
    fn triggers_bind_at_half_travel_and_not_before() {
        let baseline = sample(&raw());
        let mut pad = raw();
        pad.left_trigger = 16_000; // about 124 of 255, below the press level
        assert_eq!(captured_source(&baseline, &sample(&pad)), None);
        pad.left_trigger = 16_500; // about 128
        assert_eq!(captured_source(&baseline, &sample(&pad)), Some("xinput:LT".to_string()));
        let mut right = raw();
        right.right_trigger = 32_767;
        assert_eq!(captured_source(&baseline, &sample(&right)), Some("xinput:RT".to_string()));
    }

    #[test]
    fn buttons_are_preferred_over_triggers_when_both_are_new() {
        let baseline = sample(&raw());
        let mut pad = raw();
        pad.left_trigger = 32_767;
        pad.dpad_up = true;
        assert_eq!(captured_source(&baseline, &sample(&pad)), Some("xinput:DpadUp".to_string()));
    }

    #[test]
    fn controller_one_of_n_is_the_lowest_id() {
        assert_eq!(active_pad(&[]), None);
        assert_eq!(active_pad(&[7]), Some(7));
        assert_eq!(active_pad(&[9, 4, 12]), Some(4));
    }
}
