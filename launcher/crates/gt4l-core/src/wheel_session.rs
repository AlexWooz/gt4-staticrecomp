//! What the last game session says about the wheel (TASK-181): the `[wheel]` and `[dfp]` lines the game writes to its
//! log (`sessions\session-*\game.log`), read back for the Wheel page's "force feedback received" line.
//!
//! The lines (the game's stderr):
//! - `[wheel] wheel.toml found (enabled=true, preset=<p>)`, `[wheel] no wheel.toml (...)`,
//!   `[wheel] wheel.toml refused, the game runs without the wheel: <error>`;
//! - `[wheel] device: <name> (vvvv:pppp, SDL type wheel|other, N axes, N buttons, N hats)`;
//! - `[wheel] force feedback device: constant=yes|no spring=yes|no damper=yes|no friction=yes|no gain=yes|no
//!   autocenter=yes|no`, or `[wheel] the wheel has no force feedback through SDL haptics: <reason>`;
//! - `[dfp] the game's USB driver configured the virtual Driving Force Pro (configuration 1)`;
//! - at the end: `[wheel] force feedback summary: game force commands <n>, effect updates sent <n>, effects started <n>,
//!   haptic device open|none, effects supported|none, game forces played <n>, reached the wheel yes|no`.
//!
//! A game that crashed writes no summary; then the other lines decide (`ended_early`).

use serde::Serialize;

/// One `[wheel] device:` line.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct DeviceLine {
    pub name: String,
    pub vid_pid: String,
    pub sdl_type: String,
    pub axes: Option<u32>,
    pub buttons: Option<u32>,
    pub hats: Option<u32>,
}

/// The effects of the `force feedback device:` line.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct FfbFeatures {
    pub constant: bool,
    pub spring: bool,
    pub damper: bool,
    pub friction: bool,
    pub gain: bool,
    pub autocenter: bool,
}

/// The numbers of the `force feedback summary:` line.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct Summary {
    pub game_force_commands: u64,
    pub effect_updates_sent: u64,
    pub effects_started: u64,
    pub haptic_device_open: bool,
    pub effects_supported: bool,
    pub game_forces_played: u64,
    pub reached_the_wheel: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct SessionCheck {
    /// `found`, `none` or `refused`; empty when the log has no `wheel.toml` line (an older game build).
    pub wheel_file: String,
    pub enabled: Option<bool>,
    pub preset: String,
    /// The game's reason when it refused the file.
    pub refused: String,
    pub devices: Vec<DeviceLine>,
    pub ffb_device: Option<FfbFeatures>,
    /// The game's reason when the wheel had no force feedback through SDL haptics.
    pub no_ffb: String,
    /// The game's USB driver configured the virtual DFP.
    pub dfp_configured: bool,
    pub summary: Option<Summary>,
    /// `yes`, `no` or `unknown`: did the game's forces reach the wheel?
    pub received: String,
    /// Why: `summary`, `ended_early`, `no_haptics`, `no_wheel_file`, `refused`, `wheel_off`, `no_wheel_lines`.
    pub reason: String,
}

fn after<'a>(line: &'a str, marker: &str) -> Option<&'a str> {
    line.find(marker).map(|at| &line[at + marker.len()..])
}

/// `key=yes` in the features line.
fn flag(text: &str, key: &str) -> bool {
    text.split_whitespace().any(|part| part == format!("{key}=yes"))
}

/// The number before ` <label>` in `text` (`6 axes`).
fn count_before(text: &str, label: &str) -> Option<u32> {
    let at = text.find(&format!(" {label}"))?;
    text[..at].rsplit(|c: char| !c.is_ascii_digit()).next()?.parse().ok()
}

/// The number after `<label> ` in `text` (`game force commands 120`).
fn number_after(text: &str, label: &str) -> Option<u64> {
    let rest = after(text, &format!("{label} "))?;
    rest.split(|c: char| !c.is_ascii_digit()).next()?.parse().ok()
}

fn device_line(rest: &str) -> DeviceLine {
    // "<name> (vvvv:pppp, SDL type wheel, 6 axes, 23 buttons, 1 hats)"; the name may hold parentheses itself.
    let Some(open) = rest.rfind(" (") else {
        return DeviceLine { name: rest.trim().to_string(), ..DeviceLine::default() };
    };
    let name = rest[..open].trim().to_string();
    let inside = rest[open + 2..].trim_end().trim_end_matches(')');
    let parts: Vec<&str> = inside.split(", ").collect();
    DeviceLine {
        name,
        vid_pid: parts.first().map(|part| part.to_string()).unwrap_or_default(),
        sdl_type: inside.split("SDL type ").nth(1).and_then(|tail| tail.split(',').next()).unwrap_or("").trim().to_string(),
        axes: count_before(inside, "axes"),
        buttons: count_before(inside, "buttons"),
        hats: count_before(inside, "hats"),
    }
}

/// Reads the wheel lines of one game log.
pub fn check(log: &str) -> SessionCheck {
    let mut found = SessionCheck::default();
    let mut any_line = false;
    for line in log.lines() {
        let line = line.trim_end();
        if let Some(rest) = after(line, "[wheel] ") {
            any_line = true;
            if let Some(reason) = rest.strip_prefix("wheel.toml refused, the game runs without the wheel: ") {
                found.wheel_file = "refused".into();
                found.refused = reason.to_string();
            } else if let Some(tail) = rest.strip_prefix("wheel.toml found (") {
                found.wheel_file = "found".into();
                found.enabled = Some(tail.contains("enabled=true"));
                found.preset = after(tail, "preset=").map(|preset| preset.trim_end_matches(')').to_string()).unwrap_or_default();
            } else if rest.starts_with("no wheel.toml") {
                found.wheel_file = "none".into();
                found.enabled = Some(false);
            } else if let Some(device) = rest.strip_prefix("device: ") {
                found.devices.push(device_line(device));
            } else if let Some(features) = rest.strip_prefix("force feedback device: ") {
                found.ffb_device = Some(FfbFeatures {
                    constant: flag(features, "constant"),
                    spring: flag(features, "spring"),
                    damper: flag(features, "damper"),
                    friction: flag(features, "friction"),
                    gain: flag(features, "gain"),
                    autocenter: flag(features, "autocenter"),
                });
            } else if let Some(reason) = rest.strip_prefix("the wheel has no force feedback through SDL haptics: ") {
                found.no_ffb = reason.to_string();
            } else if let Some(summary) = rest.strip_prefix("force feedback summary: ") {
                found.summary = Some(Summary {
                    game_force_commands: number_after(summary, "game force commands").unwrap_or(0),
                    effect_updates_sent: number_after(summary, "effect updates sent").unwrap_or(0),
                    effects_started: number_after(summary, "effects started").unwrap_or(0),
                    haptic_device_open: summary.contains("haptic device open"),
                    effects_supported: summary.contains("effects supported"),
                    game_forces_played: number_after(summary, "game forces played").unwrap_or(0),
                    reached_the_wheel: summary.contains("reached the wheel yes"),
                });
            }
        } else if line.contains("[dfp] the game's USB driver configured the virtual Driving Force Pro") {
            any_line = true;
            found.dfp_configured = true;
        }
    }
    let (received, reason) = if let Some(summary) = &found.summary {
        (if summary.reached_the_wheel { "yes" } else { "no" }, "summary")
    } else if found.wheel_file == "refused" {
        ("no", "refused")
    } else if found.wheel_file == "none" {
        ("no", "no_wheel_file")
    } else if found.enabled == Some(false) {
        ("no", "wheel_off")
    } else if !found.no_ffb.is_empty() {
        ("no", "no_haptics")
    } else if found.dfp_configured || found.ffb_device.is_some() {
        ("unknown", "ended_early")
    } else if any_line {
        ("unknown", "ended_early")
    } else {
        ("unknown", "no_wheel_lines")
    };
    found.received = received.into();
    found.reason = reason.into();
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    const FULL: &str = "\
[ee] booting
[wheel] wheel.toml found (enabled=true, preset=logitech_legacy)
[wheel] device: Logitech G27 Racing Wheel USB (046d:c29b, SDL type wheel, 6 axes, 23 buttons, 1 hats)
[wheel] device: Pedals (Rev. B) (0eb7:183b, SDL type other, 3 axes, 0 buttons, 0 hats)
[wheel] force feedback device: constant=yes spring=yes damper=yes friction=yes gain=yes autocenter=yes
[dfp] the game's USB driver configured the virtual Driving Force Pro (configuration 1)
[wheel] force feedback summary: game force commands 5120, effect updates sent 4800, effects started 4, haptic device open, effects supported, game forces played 4790, reached the wheel yes
[ee] stopped after 1000 frames
";

    #[test]
    fn a_full_session_says_yes_with_every_detail() {
        let found = check(FULL);
        assert_eq!(found.wheel_file, "found");
        assert_eq!(found.enabled, Some(true));
        assert_eq!(found.preset, "logitech_legacy");
        assert_eq!(found.devices.len(), 2);
        assert_eq!(found.devices[0].name, "Logitech G27 Racing Wheel USB");
        assert_eq!(found.devices[0].vid_pid, "046d:c29b");
        assert_eq!(found.devices[0].sdl_type, "wheel");
        assert_eq!((found.devices[0].axes, found.devices[0].buttons, found.devices[0].hats), (Some(6), Some(23), Some(1)));
        assert_eq!(found.devices[1].name, "Pedals (Rev. B)", "a name with parentheses keeps them");
        assert!(found.ffb_device.as_ref().unwrap().constant && found.ffb_device.as_ref().unwrap().autocenter);
        assert!(found.dfp_configured);
        let summary = found.summary.unwrap();
        assert_eq!(summary.game_force_commands, 5120);
        assert_eq!(summary.game_forces_played, 4790);
        assert!(summary.haptic_device_open && summary.effects_supported && summary.reached_the_wheel);
        assert_eq!((found.received.as_str(), found.reason.as_str()), ("yes", "summary"));
    }

    #[test]
    fn a_summary_that_did_not_reach_the_wheel_says_no() {
        let text = FULL.replace("reached the wheel yes", "reached the wheel no").replace("haptic device open", "haptic device none");
        let found = check(&text);
        assert_eq!((found.received.as_str(), found.reason.as_str()), ("no", "summary"));
        assert!(!found.summary.unwrap().haptic_device_open);
    }

    #[test]
    fn a_crash_before_the_summary_is_unknown() {
        let text: String = FULL.lines().filter(|line| !line.contains("summary")).map(|line| format!("{line}\n")).collect();
        let found = check(&text);
        assert_eq!((found.received.as_str(), found.reason.as_str()), ("unknown", "ended_early"));
    }

    #[test]
    fn no_file_a_refused_file_off_and_no_haptics_say_no() {
        let none = check("[wheel] no wheel.toml (GT4_WHEEL_FILE unset)\n");
        assert_eq!((none.received.as_str(), none.reason.as_str()), ("no", "no_wheel_file"));
        let refused = check("[wheel] wheel.toml refused, the game runs without the wheel: wheel.toml:3: mode must be \"dfp\"\n");
        assert_eq!((refused.received.as_str(), refused.reason.as_str()), ("no", "refused"));
        assert!(refused.refused.contains("mode must be"));
        let off = check("[wheel] wheel.toml found (enabled=false, preset=generic)\n");
        assert_eq!((off.received.as_str(), off.reason.as_str()), ("no", "wheel_off"));
        let no_haptics = check(
            "[wheel] wheel.toml found (enabled=true, preset=generic)\n[wheel] the wheel has no force feedback through SDL haptics: Haptic: There are 0 haptic devices available\n",
        );
        assert_eq!((no_haptics.received.as_str(), no_haptics.reason.as_str()), ("no", "no_haptics"));
        assert!(no_haptics.no_ffb.contains("0 haptic devices"));
    }

    #[test]
    fn a_log_without_wheel_lines_is_unknown() {
        let found = check("[ee] booting\n[ee] stopped\n");
        assert_eq!((found.received.as_str(), found.reason.as_str()), ("unknown", "no_wheel_lines"));
        assert!(found.wheel_file.is_empty());
    }
}
