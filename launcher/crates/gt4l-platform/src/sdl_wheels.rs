//! The SDL3 side of the Wheel page (TASK-181): the joysticks the game could drive (the wheel and its auxiliary pedals,
//! shifters and handbrakes), their raw state for the live view and the capture, and the force test.
//!
//! Based on cbfd8b8's `sdl_wheels.rs` and its additions to `sdl_pads.rs` (JulioHeroZ, with luanantony): the device list,
//! the raw reading and the force test. TASK-181 keeps the handles to the minimum: the list is read without opening a
//! device (`SDL_Get*ForID`); joystick handles are open only while the Wheel page shows its live view ([`WheelReader`]);
//! a haptic handle is open only during the force test ([`test_force`]), which plays one constant effect and never SDL
//! rumble (two effect owners on one DirectInput device crashed a MOZA R5). The launcher's pad service drops all of it
//! before the game starts: the game needs the wheel's force feedback for itself.
//!
//! Owned by the launcher's pad thread (SDL is not `Send`), like `sdl_pads`.

use std::collections::BTreeMap;
use std::ffi::CStr;
use std::time::Duration;

use sdl3::joystick::{Joystick, JoystickId};
use sdl3::sys::haptic as sys_haptic;
use sdl3::sys::joystick as sys_joystick;
use sdl3::JoystickSubsystem;

use gt4l_core::wheel_capture::Reading;
use gt4l_core::wheel_identity;

use crate::sdl_pads::SdlPads;

/// One joystick SDL sees, read without opening it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct JoystickEntry {
    pub id: u32,
    /// 32 lowercase hex digits.
    pub guid: String,
    pub name: String,
    /// SDL's type is "wheel".
    pub sdl_wheel: bool,
    /// SDL has a gamepad mapping for it (a controller the Controls page reads).
    pub is_gamepad: bool,
}

impl JoystickEntry {
    /// `wheel`, `gamepad` or `other`, as the game's log names the SDL type.
    pub fn sdl_type(&self) -> &'static str {
        if self.sdl_wheel {
            "wheel"
        } else if self.is_gamepad {
            "gamepad"
        } else {
            "other"
        }
    }

    /// The game's rule for the wheel it drives (`wheel_identity::is_candidate`).
    pub fn is_wheel(&self) -> bool {
        wheel_identity::is_candidate(self.sdl_wheel, &self.name, &self.guid)
    }

    /// A device the Wheel page shows: a wheel, or a joystick that is not a game controller (pedals, a shifter, a
    /// handbrake). Gamepads belong to the Controls page.
    pub fn is_wheel_page_device(&self) -> bool {
        self.is_wheel() || !self.is_gamepad
    }
}

/// The joysticks SDL sees now, in SDL's order, without opening any of them.
pub fn joysticks(pads: &SdlPads) -> Vec<JoystickEntry> {
    let subsystem = pads.joystick_subsystem();
    subsystem
        .joysticks()
        .unwrap_or_default()
        .into_iter()
        .map(|id| {
            let raw = u32::from(id);
            let sdl_id = sys_joystick::SDL_JoystickID(raw);
            // SAFETY: plain queries on a connected instance id; SDL owns the returned name and it is copied at once.
            let (name, sdl_wheel, is_gamepad) = unsafe {
                let text = sys_joystick::SDL_GetJoystickNameForID(sdl_id);
                let name = if text.is_null() { String::new() } else { CStr::from_ptr(text).to_string_lossy().into_owned() };
                let kind = sys_joystick::SDL_GetJoystickTypeForID(sdl_id);
                (name, kind == sys_joystick::SDL_JOYSTICK_TYPE_WHEEL, sdl3::sys::gamepad::SDL_IsGamepad(sdl_id))
            };
            JoystickEntry {
                id: raw,
                guid: pads.guid_of(id).to_ascii_lowercase(),
                name: if name.trim().is_empty() { "USB device".to_string() } else { name },
                sdl_wheel,
                is_gamepad,
            }
        })
        .collect()
}

/// What an open joystick says about itself.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub axes: u32,
    pub buttons: u32,
    pub hats: u32,
    /// SDL says the device has force feedback (haptics); found without opening a haptic handle.
    pub haptic: bool,
}

/// Joystick handles for the live view and the capture. Open only while the Wheel page shows; dropping it (or
/// [`WheelReader::close_all`]) closes every handle.
#[derive(Default)]
pub struct WheelReader {
    open: BTreeMap<u32, Joystick>,
}

impl WheelReader {
    /// Opens the joysticks of `wanted` that are not open yet and closes the others.
    pub fn sync(&mut self, subsystem: &JoystickSubsystem, wanted: &[u32]) {
        self.open.retain(|id, _| wanted.contains(id));
        for id in wanted {
            if !self.open.contains_key(id) {
                if let Ok(joystick) = subsystem.open(JoystickId::from(*id)) {
                    self.open.insert(*id, joystick);
                }
            }
        }
    }

    pub fn close_all(&mut self) {
        self.open.clear();
    }

    /// The open joystick `id` (the force test uses it).
    pub fn joystick(&self, id: u32) -> Option<&Joystick> {
        self.open.get(&id)
    }

    pub fn is_open(&self, id: u32) -> bool {
        self.open.contains_key(&id)
    }

    pub fn is_empty(&self) -> bool {
        self.open.is_empty()
    }

    pub fn counts(&self, id: u32) -> Option<Counts> {
        let joystick = self.open.get(&id)?;
        // SAFETY: the joystick is open for as long as `self.open` holds it.
        let haptic = unsafe { sys_haptic::SDL_IsJoystickHaptic(joystick.raw()) };
        Some(Counts { axes: joystick.num_axes(), buttons: joystick.num_buttons(), hats: joystick.num_hats(), haptic })
    }

    /// The raw state of every open joystick (`guid` from `entries`).
    pub fn read(&self, entries: &[JoystickEntry]) -> Vec<Reading> {
        self.open
            .iter()
            .map(|(id, joystick)| Reading {
                id: *id,
                guid: entries.iter().find(|entry| entry.id == *id).map(|entry| entry.guid.clone()).unwrap_or_default(),
                axes: (0..joystick.num_axes().min(32)).map(|axis| joystick.axis(axis).unwrap_or(0)).collect(),
                buttons: (0..joystick.num_buttons().min(128)).map(|button| joystick.button(button).unwrap_or(false)).collect(),
                hats: (0..joystick.num_hats().min(8)).map(|hat| joystick.hat(hat).map(|state| state.to_raw()).unwrap_or(0)).collect(),
            })
            .collect()
    }
}

/// The SDL3 haptic feature bits (`SDL_HAPTIC_*`, `SDL_haptic.h`). sdl3-sys types them as 16 bits, which drops GAIN and
/// AUTOCENTER, so they are written out here.
pub mod feature {
    pub const CONSTANT: u32 = 1 << 0;
    pub const SPRING: u32 = 1 << 7;
    pub const DAMPER: u32 = 1 << 8;
    pub const FRICTION: u32 = 1 << 10;
    pub const GAIN: u32 = 1 << 16;
    pub const AUTOCENTER: u32 = 1 << 17;
}

/// The effects a device supports, as the game's log lists them.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Features {
    pub constant: bool,
    pub spring: bool,
    pub damper: bool,
    pub friction: bool,
    pub gain: bool,
    pub autocenter: bool,
}

impl Features {
    pub fn from_bits(bits: u32) -> Features {
        Features {
            constant: bits & feature::CONSTANT != 0,
            spring: bits & feature::SPRING != 0,
            damper: bits & feature::DAMPER != 0,
            friction: bits & feature::FRICTION != 0,
            gain: bits & feature::GAIN != 0,
            autocenter: bits & feature::AUTOCENTER != 0,
        }
    }

    /// `constant spring damper` (the supported effects, space-separated), or `none`.
    pub fn text(&self) -> String {
        let names: Vec<&str> = [
            (self.constant, "constant"),
            (self.spring, "spring"),
            (self.damper, "damper"),
            (self.friction, "friction"),
            (self.gain, "gain"),
            (self.autocenter, "autocenter"),
        ]
        .iter()
        .filter(|(on, _)| *on)
        .map(|(_, name)| *name)
        .collect();
        if names.is_empty() {
            "none".to_string()
        } else {
            names.join(" ")
        }
    }
}

/// What the force test did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ForceTest {
    /// The device opened as a haptic device.
    pub has_ffb: bool,
    pub features: Features,
    /// The constant pulls were played.
    pub played: bool,
    /// SDL's reason when the device has no force feedback or an effect failed.
    pub error: String,
}

/// How long each pull lasts, and the pause after it.
pub const PULL: Duration = Duration::from_millis(400);
const SETTLE: Duration = Duration::from_millis(80);

/// The constant level of a test pull: a low base (a quarter of full force) times the configured strength (0..2).
pub fn pull_level(strength: f64) -> i16 {
    (0.25 * strength.clamp(0.0, 2.0) * 32767.0).round() as i16
}

fn sdl_error() -> String {
    // SAFETY: SDL returns a string it owns (never null); it is copied at once.
    unsafe {
        let text = sdl3::sys::error::SDL_GetError();
        if text.is_null() { String::new() } else { CStr::from_ptr(text).to_string_lossy().into_owned() }
    }
}

/// Plays the force test on `joystick`: one haptic handle, opened here and closed before returning; a constant pull one
/// way, then the other (each [`PULL`] long) at `level` (see [`pull_level`]); `invert` reverses the sign as the game does
/// with `[ffb] invert`. The haptic subsystem must be started (`SdlPads::haptic`). Gain and autocentre are left to SDL's
/// open (SDL sets the gain and turns autocentre off only when the device has them).
///
/// The sign of the first pull follows the game's effect (`SDL_HAPTIC_CARTESIAN`, `dir[0] = 1`, a negative level);
/// which way that turns a given wheel is not verified on hardware (Hypothesis), so the page says "one way, then the
/// other".
pub fn test_force(joystick: &Joystick, level: i16, invert: bool) -> ForceTest {
    // SAFETY: the joystick is open; the haptic handle and the effect are created, used and destroyed in this block.
    unsafe {
        let haptic = sys_haptic::SDL_OpenHapticFromJoystick(joystick.raw());
        if haptic.is_null() {
            return ForceTest { has_ffb: false, features: Features::default(), played: false, error: sdl_error() };
        }
        let bits = sys_haptic::SDL_GetHapticFeatures(haptic);
        let features = Features::from_bits(bits);
        let mut result = ForceTest { has_ffb: true, features, played: false, error: String::new() };
        if features.constant {
            // A game that ended may have left the device gain at 0 (the game stops its forces that way).
            if features.gain {
                sys_haptic::SDL_SetHapticGain(haptic, 100);
            }
            let first = if invert { level } else { level.saturating_neg() };
            let mut effect = sys_haptic::SDL_HapticEffect::default();
            effect.constant = sys_haptic::SDL_HapticConstant {
                r#type: sys_haptic::SDL_HAPTIC_CONSTANT,
                direction: sys_haptic::SDL_HapticDirection { r#type: sys_haptic::SDL_HAPTIC_CARTESIAN, dir: [1, 0, 0] },
                length: PULL.as_millis() as u32,
                delay: 0,
                button: 0,
                interval: 0,
                level: first,
                attack_length: 40,
                attack_level: 0,
                fade_length: 60,
                fade_level: 0,
            };
            let id = sys_haptic::SDL_CreateHapticEffect(haptic, &effect);
            if id.0 < 0 {
                result.error = sdl_error();
            } else {
                let mut played = sys_haptic::SDL_RunHapticEffect(haptic, id, 1);
                std::thread::sleep(PULL + SETTLE);
                effect.constant.level = first.saturating_neg();
                played = played && sys_haptic::SDL_UpdateHapticEffect(haptic, id, &effect);
                played = played && sys_haptic::SDL_RunHapticEffect(haptic, id, 1);
                std::thread::sleep(PULL + SETTLE);
                sys_haptic::SDL_StopHapticEffect(haptic, id);
                sys_haptic::SDL_DestroyHapticEffect(haptic, id);
                result.played = played;
                if !played {
                    result.error = sdl_error();
                }
            }
        }
        sys_haptic::SDL_CloseHaptic(haptic);
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_feature_bits_are_sdl3s_including_gain_and_autocentre() {
        let all = Features::from_bits(feature::CONSTANT | feature::SPRING | feature::DAMPER | feature::FRICTION | feature::GAIN | feature::AUTOCENTER);
        assert_eq!(all.text(), "constant spring damper friction gain autocenter");
        let r5 = Features::from_bits(feature::CONSTANT | feature::SPRING | feature::DAMPER | feature::FRICTION | feature::GAIN);
        assert!(!r5.autocenter, "the MOZA R5 has no autocentre");
        assert_eq!(Features::from_bits(0).text(), "none");
        assert_eq!(feature::GAIN, 65536, "SDL_HAPTIC_GAIN is 1 << 16");
    }

    #[test]
    fn the_test_pull_is_a_quarter_of_full_force_times_the_strength() {
        assert_eq!(pull_level(1.0), 8192);
        assert_eq!(pull_level(2.0), 16384);
        assert_eq!(pull_level(0.0), 0);
        assert_eq!(pull_level(9.0), 16384, "the strength is clamped to 2");
    }

    #[test]
    fn the_wheel_page_lists_wheels_and_auxiliaries_but_not_gamepads() {
        let entry = |name: &str, guid: &str, sdl_wheel: bool, is_gamepad: bool| JoystickEntry {
            id: 1,
            guid: guid.into(),
            name: name.into(),
            sdl_wheel,
            is_gamepad,
        };
        let g27 = entry("Logitech G27 Racing Wheel USB", "030000006d0400009bc2000000000000", true, false);
        assert!(g27.is_wheel() && g27.is_wheel_page_device());
        assert_eq!(g27.sdl_type(), "wheel");
        let pedals = entry("Pedals", "03000000b70e00003b18000000000000", false, false);
        assert!(!pedals.is_wheel() && pedals.is_wheel_page_device());
        let pad = entry("Xbox Wireless Controller", "030000005e040000130b000000000000", false, true);
        assert!(!pad.is_wheel_page_device());
        assert_eq!(pad.sdl_type(), "gamepad");
        let hori = entry("HORI Racing Wheel Apex", "030000000d0f00000000000000000000", false, true);
        assert!(hori.is_wheel_page_device(), "an XInput wheel is a gamepad and a wheel");
    }
}
