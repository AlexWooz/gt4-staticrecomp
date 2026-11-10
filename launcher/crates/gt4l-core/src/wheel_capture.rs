//! Finding the control the player moves (TASK-181): the Wheel page's "bind" and set-up wizard. Pure: the launcher's SDL3
//! service passes the raw readings of the wheel and its auxiliary devices every tick, and [`Capture::step`] answers once
//! a control moved.
//!
//! Based on cbfd8b8's `detect_wheel_capture` (JulioHeroZ, with luanantony): a baseline taken when the capture starts,
//! axis motion for steering and pedals (with the pedal's polarity from its rest and pressed values), a new button, hat
//! direction or axis threshold for a button, and `dev:<guid>:` sources for a device that is not the wheel. Held
//! controls never count: a button or hat held at the start counts only after it was released.

use crate::wheel::{self, SourceKind};

/// How far an axis must move from where it rested to count for steering or a pedal (about a third of its travel).
pub const AXIS_MOTION: i32 = 11000;
/// How far an axis must move to count as a button, and the game's threshold it must pass (`axis:N+` above +16384).
pub const AXIS_BUTTON_MOTION: i32 = 18000;
pub const AXIS_BUTTON_THRESHOLD: i32 = 16384;

/// One device's raw state (SDL3 joystick values).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Reading {
    /// SDL's instance id.
    pub id: u32,
    /// The SDL GUID, 32 lowercase hex digits.
    pub guid: String,
    pub axes: Vec<i16>,
    pub buttons: Vec<bool>,
    /// SDL hat bits: 1 up, 2 right, 4 down, 8 left.
    pub hats: Vec<u8>,
}

/// What the capture waits for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Want {
    /// An axis that turns (the steering); `only`: that axis, as a source (`axis:0`, `dev:<guid>:axis:2`), for the
    /// wizard's "fully right" after "fully left".
    Steering { only: Option<String> },
    /// An axis (with its polarity) or a button (a digital pedal or handbrake).
    Pedal,
    /// A button, a hat direction, or an axis past the game's threshold.
    Button,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Request {
    pub want: Want,
    /// The device a plain source means (the wheel the game drives); `None` when there is none.
    pub primary: Option<u32>,
    /// Sources that never count (the steering and pedal axes while a button is captured; the steering while a pedal is).
    pub ignore: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Captured {
    pub device: u32,
    pub guid: String,
    /// The source in the game's spelling: plain for the primary device, else `dev:<guid>:`.
    pub source: String,
    /// The axis where it rested and where it is now (axis captures).
    pub rest: Option<i16>,
    pub value: Option<i16>,
    /// The pedal mode found from `rest` and `value` (pedal axis captures).
    pub mode: Option<&'static str>,
}

pub struct Capture {
    request: Request,
    baseline: Vec<Reading>,
}

const DIRECTIONS: [(u8, &str); 4] = [(1, "up"), (2, "right"), (4, "down"), (8, "left")];

impl Capture {
    pub fn new(request: Request) -> Capture {
        Capture { request, baseline: Vec::new() }
    }

    /// The `(device, axis)` of an axis source on these readings: a plain source is on the primary device.
    fn axis_of(&self, text: &str, readings: &[Reading]) -> Option<(u32, u8)> {
        let source = wheel::parse_source(text).ok()??;
        if !matches!(source.kind, SourceKind::Axis | SourceKind::AxisPositive | SourceKind::AxisNegative) {
            return None;
        }
        let device = if source.device_guid.is_empty() {
            self.request.primary
        } else {
            readings.iter().find(|reading| reading.guid.eq_ignore_ascii_case(&source.device_guid)).map(|reading| reading.id)
        };
        device.map(|device| (device, source.index))
    }

    /// The `(device, axis)` pairs of the ignored sources on these readings.
    fn ignored_axes(&self, readings: &[Reading]) -> Vec<(u32, u8)> {
        self.request.ignore.iter().filter_map(|text| self.axis_of(text, readings)).collect()
    }

    /// The prefix of a source on `reading`: none for the primary device, `dev:<guid>:` for another one with a unique
    /// GUID. `None`: the device cannot be named (two devices share its GUID).
    fn prefix(&self, reading: &Reading, readings: &[Reading]) -> Option<String> {
        if Some(reading.id) == self.request.primary {
            return Some(String::new());
        }
        let same = readings.iter().filter(|other| other.guid.eq_ignore_ascii_case(&reading.guid)).count();
        (wheel::is_guid(&reading.guid) && same == 1).then(|| format!("dev:{}:", reading.guid.to_ascii_lowercase()))
    }

    /// One tick: the readings now. Returns the control once one moved.
    pub fn step(&mut self, readings: &[Reading]) -> Option<Captured> {
        // New devices join the baseline as they are now; a device that left is dropped.
        self.baseline.retain(|base| readings.iter().any(|reading| reading.id == base.id));
        for reading in readings {
            if !self.baseline.iter().any(|base| base.id == reading.id) {
                self.baseline.push(reading.clone());
            }
        }
        // A button or hat held at the start counts once it was released.
        for base in self.baseline.iter_mut() {
            let Some(now) = readings.iter().find(|reading| reading.id == base.id) else { continue };
            for (index, pressed) in base.buttons.iter_mut().enumerate() {
                if *pressed && !now.buttons.get(index).copied().unwrap_or(false) {
                    *pressed = false;
                }
            }
            for (index, bits) in base.hats.iter_mut().enumerate() {
                if now.hats.get(index).copied().unwrap_or(0) == 0 {
                    *bits = 0;
                }
            }
        }
        let ignored = self.ignored_axes(readings);
        match self.request.want.clone() {
            Want::Steering { only: None } => self.moved_axis(readings, &ignored, None, false),
            Want::Steering { only: Some(text) } => {
                let axis = self.axis_of(&text, readings)?;
                self.moved_axis(readings, &[], Some(axis), false)
            }
            Want::Pedal => self.moved_axis(readings, &ignored, None, true).or_else(|| self.new_button(readings, false)),
            Want::Button => self.new_button(readings, true).or_else(|| self.axis_button(readings, &ignored)),
        }
    }

    /// The axis that moved most, at least [`AXIS_MOTION`] from its rest.
    fn moved_axis(&self, readings: &[Reading], ignored: &[(u32, u8)], only: Option<(u32, u8)>, pedal: bool) -> Option<Captured> {
        let mut best: Option<(i32, &Reading, usize, i16, i16)> = None;
        for reading in readings {
            let Some(base) = self.baseline.iter().find(|base| base.id == reading.id) else { continue };
            for (index, value) in reading.axes.iter().enumerate() {
                let axis = index as u8;
                if index > 31 || ignored.contains(&(reading.id, axis)) {
                    continue;
                }
                if only.is_some_and(|wanted| wanted != (reading.id, axis)) {
                    continue;
                }
                let rest = base.axes.get(index).copied().unwrap_or(0);
                let delta = (i32::from(*value) - i32::from(rest)).abs();
                if delta >= AXIS_MOTION && best.is_none_or(|(most, ..)| delta > most) {
                    best = Some((delta, reading, index, rest, *value));
                }
            }
        }
        let (_, reading, index, rest, value) = best?;
        let prefix = self.prefix(reading, readings)?;
        Some(Captured {
            device: reading.id,
            guid: reading.guid.clone(),
            source: format!("{prefix}axis:{index}"),
            rest: Some(rest),
            value: Some(value),
            mode: pedal.then(|| wheel::detect_pedal_mode(rest, value)),
        })
    }

    /// A button pressed since the start (and, for a button capture, a hat direction).
    fn new_button(&self, readings: &[Reading], hats: bool) -> Option<Captured> {
        for reading in readings {
            let Some(base) = self.baseline.iter().find(|base| base.id == reading.id) else { continue };
            for (index, pressed) in reading.buttons.iter().enumerate() {
                if index <= 127 && *pressed && !base.buttons.get(index).copied().unwrap_or(false) {
                    let prefix = self.prefix(reading, readings)?;
                    return Some(Captured {
                        device: reading.id,
                        guid: reading.guid.clone(),
                        source: format!("{prefix}btn:{index}"),
                        rest: None,
                        value: None,
                        mode: None,
                    });
                }
            }
            if !hats {
                continue;
            }
            for (index, bits) in reading.hats.iter().enumerate() {
                let before = base.hats.get(index).copied().unwrap_or(0);
                if index > 7 || *bits == 0 || *bits == before {
                    continue;
                }
                // A diagonal names the direction that is new; the first one in up, right, down, left order.
                let fresh = *bits & !before;
                let Some((_, direction)) = DIRECTIONS.iter().find(|(bit, _)| fresh & bit != 0) else { continue };
                let prefix = self.prefix(reading, readings)?;
                return Some(Captured {
                    device: reading.id,
                    guid: reading.guid.clone(),
                    source: format!("{prefix}hat:{index}:{direction}"),
                    rest: None,
                    value: None,
                    mode: None,
                });
            }
        }
        None
    }

    /// An axis pushed past the game's threshold (an analog paddle or a shifter lever): `axis:N+` or `axis:N-`.
    fn axis_button(&self, readings: &[Reading], ignored: &[(u32, u8)]) -> Option<Captured> {
        for reading in readings {
            let Some(base) = self.baseline.iter().find(|base| base.id == reading.id) else { continue };
            for (index, value) in reading.axes.iter().enumerate() {
                if index > 31 || ignored.contains(&(reading.id, index as u8)) {
                    continue;
                }
                let rest = base.axes.get(index).copied().unwrap_or(0);
                let value32 = i32::from(*value);
                if (value32 - i32::from(rest)).abs() < AXIS_BUTTON_MOTION || value32.abs() <= AXIS_BUTTON_THRESHOLD {
                    continue;
                }
                let prefix = self.prefix(reading, readings)?;
                let sign = if value32 > 0 { "+" } else { "-" };
                return Some(Captured {
                    device: reading.id,
                    guid: reading.guid.clone(),
                    source: format!("{prefix}axis:{index}{sign}"),
                    rest: Some(rest),
                    value: Some(*value),
                    mode: None,
                });
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const WHEEL: &str = "030000006d0400009bc2000000000000";
    const PEDALS: &str = "03000000b70e00003b18000000000000";

    fn g27() -> Reading {
        Reading { id: 1, guid: WHEEL.into(), axes: vec![0, 32767, 32767, 32767], buttons: vec![false; 24], hats: vec![0] }
    }

    fn pedals() -> Reading {
        Reading { id: 2, guid: PEDALS.into(), axes: vec![-32768, -32768, -32768], buttons: vec![], hats: vec![] }
    }

    fn capture(want: Want, ignore: &[&str]) -> Capture {
        Capture::new(Request { want, primary: Some(1), ignore: ignore.iter().map(|text| text.to_string()).collect() })
    }

    #[test]
    fn steering_finds_the_axis_that_turns_and_keeps_the_rest_and_value() {
        let mut steering = capture(Want::Steering { only: None }, &[]);
        assert!(steering.step(&[g27(), pedals()]).is_none(), "the first reading is the baseline");
        let mut left = g27();
        left.axes[0] = -30000;
        left.axes[1] = 30000; // a pedal touched a little moves less
        let found = steering.step(&[left, pedals()]).unwrap();
        assert_eq!((found.source.as_str(), found.rest, found.value), ("axis:0", Some(0), Some(-30000)));
        // "Fully right" waits for that axis only.
        let mut right = capture(Want::Steering { only: Some("axis:0".into()) }, &[]);
        right.step(&[g27(), pedals()]);
        let mut other = g27();
        other.axes[2] = -32768;
        assert!(right.step(&[other.clone(), pedals()]).is_none(), "another axis does not count");
        other.axes[0] = 31000;
        assert_eq!(right.step(&[other, pedals()]).unwrap().value, Some(31000));
    }

    #[test]
    fn a_pedal_gets_its_polarity_and_the_steering_axis_never_counts() {
        let mut throttle = capture(Want::Pedal, &["axis:0"]);
        throttle.step(&[g27(), pedals()]);
        let mut turned = g27();
        turned.axes[0] = 32000;
        assert!(throttle.step(&[turned.clone(), pedals()]).is_none(), "turning the wheel is not a pedal");
        turned.axes[2] = -20000;
        let found = throttle.step(&[turned, pedals()]).unwrap();
        assert_eq!((found.source.as_str(), found.mode), ("axis:2", Some("inverted")));
        // Pedals on their own device (rest -32768): `dev:` source, normal mode.
        let mut brake = capture(Want::Pedal, &["axis:0"]);
        brake.step(&[g27(), pedals()]);
        let mut pressed = pedals();
        pressed.axes[1] = 10000;
        let found = brake.step(&[g27(), pressed]).unwrap();
        assert_eq!(found.source, format!("dev:{PEDALS}:axis:1"));
        assert_eq!(found.mode, Some("normal"));
    }

    #[test]
    fn a_button_capture_takes_new_buttons_hats_and_axis_thresholds_but_not_held_ones() {
        let mut held = g27();
        held.buttons[3] = true;
        let mut cross = capture(Want::Button, &["axis:0", "axis:1", "axis:2", "axis:3"]);
        cross.step(&[held.clone(), pedals()]);
        assert!(cross.step(&[held.clone(), pedals()]).is_none(), "a button held at the start does not count");
        let mut pedal_down = held.clone();
        pedal_down.axes[2] = -32768;
        assert!(cross.step(&[pedal_down, pedals()]).is_none(), "a bound pedal is not a button");
        let released = g27();
        assert!(cross.step(&[released.clone(), pedals()]).is_none());
        assert_eq!(cross.step(&[held.clone(), pedals()]).unwrap().source, "btn:3", "pressed again after a release, it counts");

        let mut hat = capture(Want::Button, &[]);
        hat.step(&[g27(), pedals()]);
        let mut diagonal = g27();
        diagonal.hats[0] = 1 | 2;
        assert_eq!(hat.step(&[diagonal, pedals()]).unwrap().source, "hat:0:up");

        let mut paddle = capture(Want::Button, &["axis:0"]);
        paddle.step(&[g27(), pedals()]);
        let mut lever = pedals();
        lever.axes[2] = 32767;
        assert_eq!(paddle.step(&[g27(), lever]).unwrap().source, format!("dev:{PEDALS}:axis:2+"));
    }

    #[test]
    fn a_device_that_cannot_be_named_is_skipped_and_new_devices_join_the_baseline() {
        let twin_a = Reading { id: 5, guid: PEDALS.into(), axes: vec![0], buttons: vec![false], hats: vec![] };
        let twin_b = Reading { id: 6, ..twin_a.clone() };
        let mut button = capture(Want::Button, &[]);
        button.step(&[g27(), twin_a.clone(), twin_b.clone()]);
        let mut pressed = twin_a.clone();
        pressed.buttons[0] = true;
        assert!(button.step(&[g27(), pressed, twin_b]).is_none(), "two devices share the GUID: no dev: source names one");
        let mut late = capture(Want::Button, &[]);
        late.step(&[g27()]);
        let mut joined = pedals();
        joined.axes[0] = 32767;
        assert!(late.step(&[g27(), joined.clone()]).is_none(), "a device that appears joins the baseline as it is");
        let mut digital = g27();
        digital.buttons[7] = true;
        assert_eq!(late.step(&[digital, joined]).unwrap().source, "btn:7");
    }

    #[test]
    fn without_a_primary_wheel_every_source_names_its_device() {
        let mut capture = Capture::new(Request { want: Want::Button, primary: None, ignore: vec![] });
        capture.step(&[g27()]);
        let mut pressed = g27();
        pressed.buttons[0] = true;
        assert_eq!(capture.step(&[pressed]).unwrap().source, format!("dev:{WHEEL}:btn:0"));
    }
}
