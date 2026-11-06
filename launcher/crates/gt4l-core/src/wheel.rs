//! The racing wheel configuration, `wheel.toml` (TASK-018, TASK-181): the file the game reads for its virtual Logitech
//! Driving Force Pro (DFP). The game sees a DFP on its emulated USB port, runs its own wheel driver (900 degrees) and
//! sends its own force feedback; the host wheel feeds the DFP through this file. Owner decision (2026-11-05): the DFP
//! path only. The 1.2.x "pad bridge" (`mode = "pad"`, the wheel mapped onto the DualShock 2) is gone; the launcher
//! migrates such a file to `mode = "dfp"` and keeps its bindings, while the game refuses it.
//!
//! Credits: this file grew from the 1.2.5 launcher's wheel model (commit cbfd8b8, PR #101) by **JulioHeroZ**, with
//! **luanantony**: the TOML reader and writer, the source syntax, `set_setting`, `assign_binding` and `reset_binding`
//! and the pedal modes. TASK-181 adapts it to the DFP-only contract and drops the pad-bridge settings.
//!
//! The reader follows the game's parser (`native/wheel.cpp`, `parse_wheel_config`) rule for rule, so a file the
//! launcher accepts is a file the game accepts: the same sections, keys, ranges, sources, comments, quoting and line
//! order checks (a pedal's saturation is checked against its dead zone the moment either line is read). [`validate`]
//! applies the same rules to the model before anything is written, and the writer rounds every number to the three
//! decimals it writes, so the file on disk is exactly the model that was checked.
//!
//! [`validate`]: WheelConfig::validate

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// The file name, beside `controls.toml` (the game looks there when `GT4_WHEEL_FILE` is not set).
pub const FILE_NAME: &str = "wheel.toml";

/// The folder of named presets in the user config folder: `<name>.toml`, the same format as `wheel.toml`.
pub const PRESETS_FOLDER: &str = "wheel-presets";

/// The longest preset name the launcher writes.
pub const PRESET_NAME_MAX: usize = 48;

/// At most this many sources on one button when the launcher adds one (the game sets no limit).
pub const SOURCES_PER_BUTTON: usize = 3;

/// One `[buttons]` slot: its key in the file (the DualShock 2 name of the 1.2.x files) and the DFP control it presses.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct ButtonSlot {
    pub id: &'static str,
    /// The DFP control: `cross`, `circle`, `square`, `triangle`, `gear_up` (DFP R1, right paddle), `gear_down` (DFP L1,
    /// left paddle), `r2`, `l2`, `r3`, `l3`, `select`, `start`, `dpad_up`, `dpad_right`, `dpad_down`, `dpad_left`.
    pub dfp: &'static str,
}

/// The 16 slots in the game's order (`native/wheel.hpp` `button_slots`).
pub const BUTTON_SLOTS: [ButtonSlot; 16] = [
    ButtonSlot { id: "dpad_up", dfp: "dpad_up" },
    ButtonSlot { id: "dpad_right", dfp: "dpad_right" },
    ButtonSlot { id: "dpad_down", dfp: "dpad_down" },
    ButtonSlot { id: "dpad_left", dfp: "dpad_left" },
    ButtonSlot { id: "cross", dfp: "cross" },
    ButtonSlot { id: "circle", dfp: "circle" },
    ButtonSlot { id: "square", dfp: "square" },
    ButtonSlot { id: "triangle", dfp: "triangle" },
    ButtonSlot { id: "l1", dfp: "l2" },
    ButtonSlot { id: "r1", dfp: "r2" },
    ButtonSlot { id: "l2", dfp: "gear_down" },
    ButtonSlot { id: "r2", dfp: "gear_up" },
    ButtonSlot { id: "l3", dfp: "l3" },
    ButtonSlot { id: "r3", dfp: "r3" },
    ButtonSlot { id: "select", dfp: "select" },
    ButtonSlot { id: "start", dfp: "start" },
];

/// The analog inputs besides steering. The handbrake presses the DFP's Circle past half travel; the clutch is never sent
/// to the game (the DFP has none) and exists for the launcher's live view.
pub const PEDALS: [&str; 4] = ["throttle", "brake", "handbrake", "clutch"];

pub const PEDAL_MODES: [&str; 5] = ["inverted", "normal", "positive", "combined_negative", "combined_positive"];
pub const CURVES: [&str; 3] = ["linear", "quadratic", "cubic"];

/// The comment block at the top of every file the launcher writes: what each `[buttons]` key presses on the DFP.
pub const SLOT_COMMENT: &str = "\
# GT4Recomp racing wheel: the virtual Logitech Driving Force Pro (TASK-018, TASK-181). Written by the launcher.
# The [buttons] keys keep the DualShock 2 names of the 1.2.x files. What each one presses on the Driving Force Pro:
#   cross, circle, square, triangle -> Cross, Circle, Square, Triangle
#   r2 -> gear up (DFP R1, the right paddle)    l2 -> gear down (DFP L1, the left paddle)
#   r1 -> DFP R2                                l1 -> DFP L2
#   r3, l3, select, start -> R3, L3, Select, Start
#   dpad_up, dpad_right, dpad_down, dpad_left -> the DFP's D-pad (hat)
# [handbrake] presses Circle past half travel. [clutch] is for the launcher's live view only: the DFP has no clutch.
# rotation_degrees is the wheel's own lock-to-lock range; set the same number in the wheel's software (900 for GT4).
";

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct SteeringConfig {
    /// `axis:N`, `dev:<guid>:axis:N`, or empty (unbound).
    pub source: String,
    /// The host wheel's physical lock-to-lock range (180..1080), mapped 1:1 onto the game's 900 degrees.
    pub rotation_degrees: i32,
    pub invert: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct PedalConfig {
    pub source: String,
    pub mode: String,
    /// 0.0..0.45
    pub deadzone: f64,
    /// 0.20..1.0, and more than `deadzone + 0.05`.
    pub saturation: f64,
    pub curve: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct FfbConfig {
    pub enabled: bool,
    /// 0.0..2.0, scales the game's forces.
    pub strength: f64,
    /// 0.0..1.0, the damping the wheel always has (not scaled by `strength`).
    pub damper: f64,
    /// Reverses the game's constant force.
    pub invert: bool,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct WheelConfig {
    pub version: i32,
    /// False: no wheel (the virtual DFP is not plugged in).
    pub enabled: bool,
    /// Empty, or the 32 lowercase hex digits of the wheel's SDL GUID (the game then reads that device).
    pub device_guid: String,
    pub device_name: String,
    /// A free label: the family template or the preset the mapping came from.
    pub preset: String,
    pub steering: SteeringConfig,
    pub throttle: PedalConfig,
    pub brake: PedalConfig,
    pub handbrake: PedalConfig,
    pub clutch: PedalConfig,
    pub ffb: FfbConfig,
    /// Slot id -> sources (`btn:N`, `hat:N:dir`, `axis:N+`/`axis:N-`, each optionally `dev:<guid>:`).
    pub buttons: BTreeMap<String, Vec<String>>,
}

fn pedal(source: &str, deadzone: f64, saturation: f64) -> PedalConfig {
    PedalConfig { source: source.into(), mode: "inverted".into(), deadzone, saturation, curve: "linear".into() }
}

/// The game's default buttons (`assign_default_buttons` in `native/wheel.cpp`).
pub fn default_buttons() -> BTreeMap<String, Vec<String>> {
    let mut map = BTreeMap::new();
    for (slot, source) in [
        ("dpad_up", "hat:0:up"),
        ("dpad_right", "hat:0:right"),
        ("dpad_down", "hat:0:down"),
        ("dpad_left", "hat:0:left"),
        ("cross", "btn:0"),
        ("circle", "btn:2"),
        ("square", "btn:1"),
        ("triangle", "btn:3"),
        ("l1", "btn:7"),
        ("r1", "btn:6"),
        ("l2", "btn:5"),
        ("r2", "btn:4"),
        ("l3", "btn:10"),
        ("r3", "btn:11"),
        ("select", "btn:8"),
        ("start", "btn:9"),
    ] {
        map.insert(slot.to_string(), vec![source.to_string()]);
    }
    map
}

impl Default for WheelConfig {
    /// The game's default configuration (`default_wheel_config`): what a file that leaves keys out means.
    fn default() -> Self {
        WheelConfig {
            version: 1,
            enabled: false,
            device_guid: String::new(),
            device_name: String::new(),
            preset: "generic".into(),
            steering: SteeringConfig { source: "axis:0".into(), rotation_degrees: 900, invert: false },
            throttle: pedal("axis:1", 0.02, 0.98),
            brake: pedal("axis:2", 0.02, 0.98),
            handbrake: pedal("", 0.05, 0.95),
            clutch: pedal("axis:3", 0.05, 0.95),
            ffb: FfbConfig { enabled: true, strength: 1.0, damper: 0.15, invert: false },
            buttons: default_buttons(),
        }
    }
}

impl WheelConfig {
    /// The mapping the launcher starts a wheel from when it has no preset for it (TASK-189, owner 2026-11-06): every
    /// control unassigned, so the page never guesses which button presses what. The calibration numbers stay the game's
    /// (dead zones, saturation, curves, force feedback), the wheel off, and the source of every control empty; the player
    /// maps each one, or a shipped preset for that wheel fills them in.
    pub fn unassigned() -> WheelConfig {
        let mut config = WheelConfig::default();
        config.enabled = false;
        config.preset = "generic".into();
        config.steering.source.clear();
        for name in PEDALS {
            if let Some(pedal) = pedal_mut(&mut config, name) {
                pedal.source.clear();
            }
        }
        for sources in config.buttons.values_mut() {
            sources.clear();
        }
        config
    }

    /// True when no control is bound at all: the state a wheel starts in before the player maps it.
    pub fn is_unassigned(&self) -> bool {
        self.steering.source.is_empty()
            && PEDALS.iter().all(|name| self.pedal(name).is_some_and(|pedal| pedal.source.is_empty()))
            && self.buttons.values().all(|sources| sources.is_empty())
    }
}

// ---- sources ------------------------------------------------------------------------------------------------------

/// The kinds of source the game reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum SourceKind {
    Axis,
    AxisPositive,
    AxisNegative,
    Button,
    Hat,
}

/// A parsed source, compared the way the game compares them (the GUID lowercased).
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Source {
    /// Empty: the primary wheel.
    pub device_guid: String,
    pub kind: SourceKind,
    pub index: u8,
    /// `up`, `right`, `down` or `left` for a hat; empty otherwise.
    pub direction: &'static str,
}

impl Source {
    /// The text the game reads (`format_source_ref`).
    pub fn text(&self) -> String {
        let prefix = if self.device_guid.is_empty() { String::new() } else { format!("dev:{}:", self.device_guid) };
        let body = match self.kind {
            SourceKind::Axis => format!("axis:{}", self.index),
            SourceKind::AxisPositive => format!("axis:{}+", self.index),
            SourceKind::AxisNegative => format!("axis:{}-", self.index),
            SourceKind::Button => format!("btn:{}", self.index),
            SourceKind::Hat => format!("hat:{}:{}", self.index, self.direction),
        };
        prefix + &body
    }
}

/// Parses a source as the game does (`parse_source_ref`). `Ok(None)` is an empty source (`""` or `none`).
pub fn parse_source(text: &str) -> Result<Option<Source>, String> {
    let mut text = trim(text);
    if text.is_empty() || text == "none" {
        return Ok(None);
    }
    let mut device_guid = String::new();
    if let Some(rest) = text.strip_prefix("dev:") {
        let Some(colon) = rest.find(':') else {
            return Err("malformed device source (expected dev:<32-hex-guid>:<source>)".into());
        };
        let guid = &rest[..colon];
        if !is_guid(guid) {
            return Err(format!("invalid device GUID '{guid}' in source"));
        }
        device_guid = guid.to_ascii_lowercase();
        text = &rest[colon + 1..];
    }
    if let Some(rest) = text.strip_prefix("axis:") {
        let (kind, digits) = if let Some(digits) = rest.strip_suffix('+') {
            (SourceKind::AxisPositive, digits)
        } else if let Some(digits) = rest.strip_suffix('-') {
            (SourceKind::AxisNegative, digits)
        } else {
            (SourceKind::Axis, rest)
        };
        let index = parse_index(digits, 31, "axis")?;
        return Ok(Some(Source { device_guid, kind, index, direction: "" }));
    }
    if let Some(rest) = text.strip_prefix("btn:") {
        let index = parse_index(rest, 127, "button")?;
        return Ok(Some(Source { device_guid, kind: SourceKind::Button, index, direction: "" }));
    }
    if let Some(rest) = text.strip_prefix("hat:") {
        let Some(colon) = rest.find(':') else {
            return Err("malformed hat source (expected hat:<index>:up|right|down|left)".into());
        };
        let index = parse_index(&rest[..colon], 7, "hat")?;
        let direction = match &rest[colon + 1..] {
            "up" => "up",
            "right" => "right",
            "down" => "down",
            "left" => "left",
            other => return Err(format!("unknown hat direction '{other}'")),
        };
        return Ok(Some(Source { device_guid, kind: SourceKind::Hat, index, direction }));
    }
    Err(format!("unknown wheel source '{text}'"))
}

/// The source in the game's own spelling (`dev:` GUID lowercased, `none` as empty). An invalid source is an error.
pub fn normalize_source(text: &str) -> Result<String, String> {
    Ok(parse_source(text)?.map(|source| source.text()).unwrap_or_default())
}

fn parse_index(digits: &str, max: u8, label: &str) -> Result<u8, String> {
    let value = parse_int(digits).ok_or_else(|| format!("invalid integer for {label}"))?;
    if value < 0 || value > i64::from(max) {
        return Err(format!("{label} index out of range (0..{max})"));
    }
    Ok(value as u8)
}

/// An integer as `std::from_chars` reads it into an `int`: an optional `-`, then digits only, within 32 bits.
fn parse_int(text: &str) -> Option<i64> {
    let digits = text.strip_prefix('-').unwrap_or(text);
    if digits.is_empty() || !digits.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let value: i64 = text.parse().ok()?;
    (i64::from(i32::MIN)..=i64::from(i32::MAX)).contains(&value).then_some(value)
}

/// A number as `strtod` reads it (the whole token, finite). Hexadecimal floats, which `strtod` also takes, are refused
/// here; the launcher never writes them.
fn parse_number(text: &str) -> Option<f64> {
    let value: f64 = text.parse().ok()?;
    value.is_finite().then_some(value)
}

/// 32 hexadecimal digits.
pub fn is_guid(text: &str) -> bool {
    text.len() == 32 && text.bytes().all(|byte| byte.is_ascii_hexdigit())
}

/// The game trims spaces, tabs and carriage returns only.
fn trim(text: &str) -> &str {
    text.trim_matches(|c| c == ' ' || c == '\t' || c == '\r')
}

/// Rounds to the three decimals the writer uses, so the model is exactly the file.
pub fn round3(value: f64) -> f64 {
    (value * 1000.0).round() / 1000.0
}

/// The slot of a `[buttons]` key, aliases included (`find_button_slot`).
pub fn button_slot(key: &str) -> Option<&'static str> {
    if let Some(slot) = BUTTON_SLOTS.iter().find(|slot| slot.id == key) {
        return Some(slot.id);
    }
    match key {
        "shift_up" | "gear_up" | "paddle_right" => Some("r2"),
        "shift_down" | "gear_down" | "paddle_left" => Some("l2"),
        "look_back" | "rear_view" => Some("l1"),
        "nitro" | "boost" | "high_beam" => Some("r1"),
        "reverse" => Some("triangle"),
        "handbrake_btn" | "ebrake" => Some("circle"),
        "camera" | "view" | "change_view" => Some("select"),
        "pause" => Some("start"),
        "horn" => Some("l3"),
        _ => None,
    }
}

/// The slot that presses a DFP control (`gear_up` -> `r2`).
pub fn slot_of_dfp(dfp: &str) -> Option<&'static str> {
    BUTTON_SLOTS.iter().find(|slot| slot.dfp == dfp).map(|slot| slot.id)
}

// ---- reading --------------------------------------------------------------------------------------------------------

/// What reading a file found besides the configuration.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReadNotes {
    /// The value of `[wheel] mode` when the file has one.
    pub mode: Option<String>,
    /// Keys of the 1.2.x pad bridge the file still has (accepted and ignored).
    pub legacy_keys: Vec<String>,
}

fn strip_comment(line: &str) -> Result<&str, String> {
    let bytes = line.as_bytes();
    let mut in_quotes = false;
    for index in 0..bytes.len() {
        if bytes[index] == b'"' {
            if index > 0 && bytes[index - 1] == b'\\' {
                return Err("escape sequences are not supported in wheel strings".into());
            }
            in_quotes = !in_quotes;
        } else if bytes[index] == b'#' && !in_quotes {
            return Ok(&line[..index]);
        }
    }
    if in_quotes {
        return Err("unterminated quoted string".into());
    }
    Ok(line)
}

fn unquote(token: &str) -> Result<&str, String> {
    let token = trim(token);
    if token.len() < 2 || !token.starts_with('"') || !token.ends_with('"') {
        return Err("expected a double-quoted string".into());
    }
    let inner = &token[1..token.len() - 1];
    if inner.contains('"') {
        return Err("unexpected quote inside string".into());
    }
    Ok(inner)
}

fn parse_bool(token: &str) -> Result<bool, String> {
    match trim(token) {
        "true" => Ok(true),
        "false" => Ok(false),
        _ => Err("expected true or false".into()),
    }
}

fn parse_string_list(raw: &str) -> Result<Vec<&str>, String> {
    let raw = trim(raw);
    if raw.starts_with('"') {
        return Ok(vec![unquote(raw)?]);
    }
    if raw.len() < 2 || !raw.starts_with('[') || !raw.ends_with(']') {
        return Err("expected an array of strings or a quoted string".into());
    }
    let body = trim(&raw[1..raw.len() - 1]);
    if body.is_empty() {
        return Ok(Vec::new());
    }
    let bytes = body.as_bytes();
    let mut items = Vec::new();
    let mut position = 0;
    while position < body.len() {
        let mut in_quotes = false;
        let mut comma = body.len();
        for (index, byte) in bytes.iter().enumerate().skip(position) {
            if *byte == b'"' {
                in_quotes = !in_quotes;
            } else if *byte == b',' && !in_quotes {
                comma = index;
                break;
            }
        }
        let item = trim(&body[position..comma]);
        if item.is_empty() {
            return Err("empty element in array".into());
        }
        items.push(unquote(item)?);
        if comma == body.len() {
            break;
        }
        position = comma + 1;
        if position == body.len() {
            return Err("trailing comma in array".into());
        }
    }
    Ok(items)
}

fn pedal_mut<'a>(config: &'a mut WheelConfig, name: &str) -> Option<&'a mut PedalConfig> {
    match name {
        "throttle" => Some(&mut config.throttle),
        "brake" => Some(&mut config.brake),
        "handbrake" => Some(&mut config.handbrake),
        "clutch" => Some(&mut config.clutch),
        _ => None,
    }
}

/// One pedal line, with the game's checks at the moment the line is read.
fn read_pedal_key(pedal: &mut PedalConfig, key: &str, value: &str) -> Result<(), String> {
    match key {
        "source" => pedal.source = normalize_source(unquote(value)?)?,
        "mode" => {
            let mode = unquote(value)?;
            if !PEDAL_MODES.contains(&mode) {
                return Err(format!(
                    "unknown pedal axis mode '{mode}' (expected inverted, normal, positive, combined_negative, or combined_positive)"
                ));
            }
            pedal.mode = mode.to_string();
        }
        "deadzone" => {
            let deadzone = parse_number(trim(value)).ok_or("invalid number for deadzone")?;
            if !(0.0..=0.45).contains(&deadzone) {
                return Err("pedal deadzone must be between 0.0 and 0.45".into());
            }
            pedal.deadzone = deadzone;
            if pedal.saturation <= pedal.deadzone + 0.05 {
                return Err("pedal saturation must exceed deadzone by at least 0.05".into());
            }
        }
        "saturation" => {
            let saturation = parse_number(trim(value)).ok_or("invalid number for saturation")?;
            if !(0.20..=1.0).contains(&saturation) {
                return Err("pedal saturation must be between 0.20 and 1.0".into());
            }
            pedal.saturation = saturation;
            if pedal.saturation <= pedal.deadzone + 0.05 {
                return Err("pedal saturation must exceed deadzone by at least 0.05".into());
            }
        }
        "curve" => {
            let curve = unquote(value)?;
            if !CURVES.contains(&curve) {
                return Err(format!("unknown curve '{curve}' (expected linear, quadratic, or cubic)"));
            }
            pedal.curve = curve.to_string();
        }
        other => return Err(format!("unknown pedal key '{other}'")),
    }
    Ok(())
}

impl WheelConfig {
    /// Reads a file exactly as the game does. Errors are `line N: message`.
    pub fn from_toml(text: &str) -> Result<WheelConfig, String> {
        Self::read(text, false).map(|(config, _)| config)
    }

    /// Reads a file as the game does; `allow_pad_mode` also takes `mode = "pad"` (only to migrate such a file).
    pub fn read(text: &str, allow_pad_mode: bool) -> Result<(WheelConfig, ReadNotes), String> {
        let mut config = WheelConfig::default();
        let mut notes = ReadNotes::default();
        let mut section = String::new();
        let mut seen_version = false;
        for (index, raw_line) in text.split('\n').enumerate() {
            let line_no = index + 1;
            let at = |message: String| format!("line {line_no}: {message}");
            let line = trim(strip_comment(raw_line).map_err(at)?);
            if line.is_empty() {
                continue;
            }
            if line.starts_with('[') {
                if !line.ends_with(']') {
                    return Err(at("malformed table header".into()));
                }
                section = trim(&line[1..line.len() - 1]).to_string();
                if !matches!(
                    section.as_str(),
                    "wheel" | "steering" | "throttle" | "brake" | "handbrake" | "clutch" | "ffb" | "buttons"
                ) {
                    return Err(at(format!("unknown section '[{section}]'")));
                }
                continue;
            }
            let Some(equals) = line.find('=') else {
                return Err(at("expected key = value".into()));
            };
            let key = trim(&line[..equals]);
            let value = trim(&line[equals + 1..]);
            if key.is_empty() || value.is_empty() {
                return Err(at("expected key = value".into()));
            }
            match section.as_str() {
                "" | "wheel" => match key {
                    "version" | "schema" => {
                        let version = parse_int(value).ok_or_else(|| at("invalid integer for version".into()))?;
                        if version != 1 {
                            return Err(at("unsupported wheel config version (expected 1)".into()));
                        }
                        config.version = 1;
                        seen_version = true;
                    }
                    "enabled" => config.enabled = parse_bool(value).map_err(at)?,
                    "device_guid" => {
                        let guid = unquote(value).map_err(at)?;
                        if !guid.is_empty() && !is_guid(guid) {
                            return Err(at("device_guid must be empty or a 32-character hex GUID".into()));
                        }
                        config.device_guid = guid.to_ascii_lowercase();
                    }
                    "device_name" => config.device_name = unquote(value).map_err(at)?.to_string(),
                    "preset" => config.preset = unquote(value).map_err(at)?.to_string(),
                    "mode" => {
                        let mode = unquote(value).map_err(at)?;
                        if mode == "pad" && !allow_pad_mode {
                            return Err(at(
                                "mode \"pad\" (the 1.2.x bridge onto the DualShock 2) was removed; the wheel drives the virtual Driving Force Pro (mode = \"dfp\")"
                                    .into(),
                            ));
                        }
                        if mode != "dfp" && mode != "pad" {
                            return Err(at("mode must be \"dfp\"".into()));
                        }
                        notes.mode = Some(mode.to_string());
                    }
                    "pedal_map_mode" => {
                        unquote(value).map_err(at)?;
                        notes.legacy_keys.push("pedal_map_mode".into());
                    }
                    other => return Err(at(format!("unknown [wheel] key '{other}'"))),
                },
                "steering" => match key {
                    "source" => {
                        let source = parse_source(unquote(value).map_err(at)?).map_err(at)?;
                        if let Some(source) = &source {
                            if source.kind != SourceKind::Axis {
                                return Err(at("steering source must be an axis (e.g. \"axis:0\")".into()));
                            }
                        }
                        config.steering.source = source.map(|source| source.text()).unwrap_or_default();
                    }
                    "rotation_degrees" => {
                        let degrees =
                            parse_int(value).ok_or_else(|| at("invalid integer for rotation_degrees".into()))?;
                        if !(180..=1080).contains(&degrees) {
                            return Err(at("rotation_degrees must be between 180 and 1080".into()));
                        }
                        config.steering.rotation_degrees = degrees as i32;
                    }
                    "invert" => config.steering.invert = parse_bool(value).map_err(at)?,
                    // The pad bridge's stick shaping: the DFP gets the wheel's real angle. Ignored, value unread.
                    "effective_degrees" | "deadzone" | "linearity" | "sensitivity" => {
                        notes.legacy_keys.push(format!("steering.{key}"))
                    }
                    other => return Err(at(format!("unknown [steering] key '{other}'"))),
                },
                "throttle" | "brake" | "handbrake" | "clutch" => {
                    let pedal = pedal_mut(&mut config, &section).expect("a pedal section");
                    read_pedal_key(pedal, key, value).map_err(at)?;
                }
                "ffb" => match key {
                    "enabled" => config.ffb.enabled = parse_bool(value).map_err(at)?,
                    "strength" => {
                        let strength = parse_number(value).ok_or_else(|| at("invalid number for strength".into()))?;
                        if !(0.0..=2.0).contains(&strength) {
                            return Err(at("ffb strength must be between 0.0 and 2.0".into()));
                        }
                        config.ffb.strength = strength;
                    }
                    "damper" => {
                        let damper = parse_number(value).ok_or_else(|| at("invalid number for damper".into()))?;
                        if !(0.0..=1.0).contains(&damper) {
                            return Err(at("ffb damper must be between 0.0 and 1.0".into()));
                        }
                        config.ffb.damper = damper;
                    }
                    "invert" => config.ffb.invert = parse_bool(value).map_err(at)?,
                    // The pad bridge's rumble shaping; the game's own forces replace it. Ignored, value unread.
                    "centering_spring" | "curb_rumble" => notes.legacy_keys.push(format!("ffb.{key}")),
                    other => return Err(at(format!("unknown [ffb] key '{other}'"))),
                },
                "buttons" => {
                    let slot = button_slot(key).ok_or_else(|| at(format!("unknown button input '{key}'")))?;
                    let mut parsed: Vec<Source> = Vec::new();
                    for item in parse_string_list(value).map_err(at)? {
                        let Some(source) = parse_source(item).map_err(at)? else {
                            continue;
                        };
                        if source.kind == SourceKind::Axis {
                            return Err(at(format!("button '{key}' requires btn:, hat:, or axis:N+/N- threshold source")));
                        }
                        if parsed.contains(&source) {
                            return Err(at(format!("duplicate source '{item}' on '{key}'")));
                        }
                        parsed.push(source);
                    }
                    config.buttons.insert(slot.to_string(), parsed.iter().map(Source::text).collect());
                }
                _ => unreachable!("sections are checked when the header is read"),
            }
        }
        if !seen_version {
            return Err("line 1: missing required version = 1".into());
        }
        config.round_numbers();
        Ok((config, notes))
    }

    fn round_numbers(&mut self) {
        for name in PEDALS {
            let pedal = pedal_mut(self, name).expect("a pedal");
            pedal.deadzone = round3(pedal.deadzone);
            pedal.saturation = round3(pedal.saturation);
        }
        self.ffb.strength = round3(self.ffb.strength);
        self.ffb.damper = round3(self.ffb.damper);
    }

    /// The pedal named `name` (`throttle`, `brake`, `handbrake`, `clutch`).
    pub fn pedal(&self, name: &str) -> Option<&PedalConfig> {
        match name {
            "throttle" => Some(&self.throttle),
            "brake" => Some(&self.brake),
            "handbrake" => Some(&self.handbrake),
            "clutch" => Some(&self.clutch),
            _ => None,
        }
    }

    /// The game's rules on the model, as the written file will hold it. A config that passes reads back in the game.
    pub fn validate(&self) -> Result<(), String> {
        if self.version != 1 {
            return Err(format!("unsupported wheel config version {}", self.version));
        }
        if !self.device_guid.is_empty() && !is_guid(&self.device_guid) {
            return Err("device_guid must be empty or a 32-character hex GUID".into());
        }
        for (label, text) in [("device_name", &self.device_name), ("preset", &self.preset)] {
            if !is_plain_text(text) {
                return Err(format!("{label} may not hold quotes, backslashes or line breaks"));
            }
        }
        if !self.steering.source.is_empty() {
            let source = parse_source(&self.steering.source)?;
            if source.is_some_and(|source| source.kind != SourceKind::Axis) {
                return Err("steering source must be an axis (e.g. \"axis:0\")".into());
            }
        }
        if !(180..=1080).contains(&self.steering.rotation_degrees) {
            return Err("rotation_degrees must be between 180 and 1080".into());
        }
        for name in PEDALS {
            let pedal = self.pedal(name).expect("a pedal");
            parse_source(&pedal.source).map_err(|error| format!("{name}: {error}"))?;
            if !PEDAL_MODES.contains(&pedal.mode.as_str()) {
                return Err(format!("{name}: unknown pedal axis mode '{}'", pedal.mode));
            }
            if !CURVES.contains(&pedal.curve.as_str()) {
                return Err(format!("{name}: unknown curve '{}'", pedal.curve));
            }
            let (deadzone, saturation) = (round3(pedal.deadzone), round3(pedal.saturation));
            if !(0.0..=0.45).contains(&deadzone) {
                return Err(format!("{name}: pedal deadzone must be between 0.0 and 0.45"));
            }
            if !(0.20..=1.0).contains(&saturation) {
                return Err(format!("{name}: pedal saturation must be between 0.20 and 1.0"));
            }
            if saturation <= deadzone + 0.05 {
                return Err(format!("{name}: pedal saturation must exceed deadzone by at least 0.05"));
            }
        }
        if !(0.0..=2.0).contains(&round3(self.ffb.strength)) {
            return Err("ffb strength must be between 0.0 and 2.0".into());
        }
        if !(0.0..=1.0).contains(&round3(self.ffb.damper)) {
            return Err("ffb damper must be between 0.0 and 1.0".into());
        }
        for (slot, sources) in &self.buttons {
            if !BUTTON_SLOTS.iter().any(|known| known.id == slot) {
                return Err(format!("unknown button input '{slot}'"));
            }
            let mut seen: Vec<Source> = Vec::new();
            for text in sources {
                let Some(source) = parse_source(text).map_err(|error| format!("{slot}: {error}"))? else {
                    continue;
                };
                if source.kind == SourceKind::Axis {
                    return Err(format!("button '{slot}' requires btn:, hat:, or axis:N+/N- threshold source"));
                }
                if seen.contains(&source) {
                    return Err(format!("duplicate source '{text}' on '{slot}'"));
                }
                seen.push(source);
            }
        }
        Ok(())
    }

    /// The file text: the slot comment, then the DFP keys only (no 1.2.x key), `mode = "dfp"`, numbers with 3 decimals.
    pub fn to_toml(&self) -> String {
        let mut out = String::from(SLOT_COMMENT);
        out.push_str("[wheel]\n");
        out.push_str(&format!("version = {}\n", self.version));
        out.push_str(&format!("enabled = {}\n", self.enabled));
        out.push_str("mode = \"dfp\"\n");
        out.push_str(&format!("device_guid = \"{}\"\n", self.device_guid.to_ascii_lowercase()));
        out.push_str(&format!("device_name = \"{}\"\n", self.device_name));
        out.push_str(&format!("preset = \"{}\"\n\n", self.preset));
        out.push_str("[steering]\n");
        out.push_str(&format!("source = \"{}\"\n", normalize_source(&self.steering.source).unwrap_or_default()));
        out.push_str(&format!("rotation_degrees = {}\n", self.steering.rotation_degrees));
        out.push_str(&format!("invert = {}\n\n", self.steering.invert));
        for name in PEDALS {
            let pedal = self.pedal(name).expect("a pedal");
            out.push_str(&format!("[{name}]\n"));
            out.push_str(&format!("source = \"{}\"\n", normalize_source(&pedal.source).unwrap_or_default()));
            out.push_str(&format!("mode = \"{}\"\n", pedal.mode));
            out.push_str(&format!("deadzone = {:.3}\n", round3(pedal.deadzone)));
            out.push_str(&format!("saturation = {:.3}\n", round3(pedal.saturation)));
            out.push_str(&format!("curve = \"{}\"\n\n", pedal.curve));
        }
        out.push_str("[ffb]\n");
        out.push_str(&format!("enabled = {}\n", self.ffb.enabled));
        out.push_str(&format!("strength = {:.3}\n", round3(self.ffb.strength)));
        out.push_str(&format!("damper = {:.3}\n", round3(self.ffb.damper)));
        out.push_str(&format!("invert = {}\n\n", self.ffb.invert));
        out.push_str("[buttons]\n");
        for slot in BUTTON_SLOTS {
            let sources = self.buttons.get(slot.id).cloned().unwrap_or_default();
            let quoted: Vec<String> = sources
                .iter()
                .filter_map(|source| normalize_source(source).ok())
                .filter(|source| !source.is_empty())
                .map(|source| format!("\"{source}\""))
                .collect();
            out.push_str(&format!("{} = [{}]\n", slot.id, quoted.join(", ")));
        }
        out
    }

    /// Validates, then writes through a temporary file and a rename. A refused config leaves the file as it was.
    pub fn save_atomic(&self, path: &Path) -> Result<(), String> {
        self.validate()?;
        let text = self.to_toml();
        // The writer and the reader must agree: what is written reads back as this model.
        let back = WheelConfig::from_toml(&text).map_err(|error| format!("the written text does not read back: {error}"))?;
        if back.enabled != self.enabled || back.steering != self.steering {
            return Err("the written text does not read back as the same configuration".into());
        }
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("cannot create {}: {error}", parent.display()))?;
        }
        let temporary = path.with_extension("toml.tmp");
        fs::write(&temporary, text.as_bytes()).map_err(|error| format!("cannot write {}: {error}", temporary.display()))?;
        fs::rename(&temporary, path).map_err(|error| {
            let _ = fs::remove_file(&temporary);
            format!("cannot replace {}: {error}", path.display())
        })
    }

    /// Changes one setting. `key` is `section.field`: `wheel.enabled`, `wheel.device_guid`, `wheel.device_name`,
    /// `wheel.preset`, `steering.rotation_degrees`, `steering.invert`, `<pedal>.mode|deadzone|saturation|curve`,
    /// `ffb.enabled|strength|damper|invert`. Sources change through [`assign_binding`]. A pedal's dead zone and
    /// saturation move each other to stay 0.05 apart. The result is validated.
    ///
    /// [`assign_binding`]: WheelConfig::assign_binding
    pub fn set_setting(&mut self, key: &str, value: &serde_json::Value) -> Result<(), String> {
        let as_bool = || value.as_bool().ok_or_else(|| format!("expected true or false for {key}"));
        let as_number = || value.as_f64().filter(|number| number.is_finite()).ok_or_else(|| format!("expected a number for {key}"));
        let as_text = || value.as_str().map(str::to_string).ok_or_else(|| format!("expected text for {key}"));
        let mut next = self.clone();
        match key {
            "wheel.enabled" => next.enabled = as_bool()?,
            "wheel.device_guid" => next.device_guid = as_text()?.trim().to_ascii_lowercase(),
            "wheel.device_name" => next.device_name = plain_text(&as_text()?),
            "wheel.preset" => next.preset = plain_text(&as_text()?),
            "steering.rotation_degrees" => {
                let degrees = as_number()?;
                if degrees.fract() != 0.0 {
                    return Err("rotation_degrees must be a whole number".into());
                }
                next.steering.rotation_degrees = degrees as i32;
            }
            "steering.invert" => next.steering.invert = as_bool()?,
            "ffb.enabled" => next.ffb.enabled = as_bool()?,
            "ffb.strength" => next.ffb.strength = round3(as_number()?),
            "ffb.damper" => next.ffb.damper = round3(as_number()?),
            "ffb.invert" => next.ffb.invert = as_bool()?,
            _ => {
                let Some((section, field)) = key.split_once('.') else {
                    return Err(format!("unknown wheel setting '{key}'"));
                };
                let pedal = pedal_mut(&mut next, section).ok_or_else(|| format!("unknown wheel setting '{key}'"))?;
                match field {
                    "mode" => pedal.mode = as_text()?,
                    "curve" => pedal.curve = as_text()?,
                    "deadzone" => {
                        pedal.deadzone = round3(as_number()?);
                        if pedal.saturation <= pedal.deadzone + 0.05 {
                            pedal.saturation = round3((pedal.deadzone + 0.06).clamp(0.20, 1.0));
                        }
                    }
                    "saturation" => {
                        pedal.saturation = round3(as_number()?);
                        if pedal.saturation <= pedal.deadzone + 0.05 {
                            pedal.deadzone = round3((pedal.saturation - 0.06).clamp(0.0, 0.45));
                        }
                    }
                    _ => return Err(format!("unknown wheel setting '{key}'")),
                }
            }
        }
        next.validate()?;
        *self = next;
        Ok(())
    }

    /// Binds `source` to `target`: `steering`, a pedal, a `[buttons]` slot (aliases taken) or a DFP control name
    /// (`gear_up`). An empty source unbinds. A pedal axis takes `pedal_mode` when the capture found it. A button source
    /// replaces the slot's sources (`add`: joins them, at most [`SOURCES_PER_BUTTON`]) and leaves every other slot,
    /// so one control never presses two DFP buttons by accident. Returns the slots it was taken from.
    pub fn assign_binding(&mut self, target: &str, source: &str, pedal_mode: Option<&str>, add: bool) -> Result<Vec<String>, String> {
        let parsed = parse_source(source)?;
        let text = parsed.as_ref().map(Source::text).unwrap_or_default();
        let mut next = self.clone();
        let mut taken_from = Vec::new();
        match target {
            "steering" => {
                if parsed.as_ref().is_some_and(|source| source.kind != SourceKind::Axis) {
                    return Err("steering source must be an axis (e.g. \"axis:0\")".into());
                }
                next.steering.source = text;
            }
            "throttle" | "brake" | "handbrake" | "clutch" => {
                let pedal = pedal_mut(&mut next, target).expect("a pedal");
                pedal.source = text;
                if let Some(mode) = pedal_mode {
                    if !PEDAL_MODES.contains(&mode) {
                        return Err(format!("unknown pedal axis mode '{mode}'"));
                    }
                    pedal.mode = mode.to_string();
                }
            }
            _ => {
                let slot = button_slot(target)
                    .or_else(|| slot_of_dfp(target))
                    .ok_or_else(|| format!("unknown wheel control '{target}'"))?;
                if parsed.as_ref().is_some_and(|source| source.kind == SourceKind::Axis) {
                    return Err(format!("button '{slot}' requires btn:, hat:, or axis:N+/N- threshold source"));
                }
                if text.is_empty() {
                    next.buttons.insert(slot.to_string(), Vec::new());
                } else {
                    for (other, sources) in next.buttons.iter_mut() {
                        if other != slot && sources.iter().any(|existing| normalize_source(existing).ok().as_deref() == Some(&text)) {
                            sources.retain(|existing| normalize_source(existing).ok().as_deref() != Some(&text));
                            taken_from.push(other.clone());
                        }
                    }
                    let entry = next.buttons.entry(slot.to_string()).or_default();
                    if add {
                        if !entry.iter().any(|existing| normalize_source(existing).ok().as_deref() == Some(&text)) {
                            if entry.len() >= SOURCES_PER_BUTTON {
                                entry.remove(0);
                            }
                            entry.push(text);
                        }
                    } else {
                        *entry = vec![text];
                    }
                }
            }
        }
        next.validate()?;
        *self = next;
        Ok(taken_from)
    }

    /// Unbinds `target` (`clear`), or puts back the value of `template` (a family template, else the game's default).
    pub fn reset_binding(&mut self, target: &str, clear: bool, template: &WheelConfig) -> Result<(), String> {
        let mut next = self.clone();
        match target {
            "steering" => next.steering.source = if clear { String::new() } else { template.steering.source.clone() },
            "throttle" | "brake" | "handbrake" | "clutch" => {
                let source = if clear { String::new() } else { template.pedal(target).expect("a pedal").source.clone() };
                pedal_mut(&mut next, target).expect("a pedal").source = source;
            }
            _ => {
                let slot = button_slot(target)
                    .or_else(|| slot_of_dfp(target))
                    .ok_or_else(|| format!("unknown wheel control '{target}'"))?;
                let sources = if clear { Vec::new() } else { template.buttons.get(slot).cloned().unwrap_or_default() };
                next.buttons.insert(slot.to_string(), sources);
            }
        }
        next.validate()?;
        *self = next;
        Ok(())
    }

    /// Every axis source the steering and the pedals use (the capture of a button ignores them).
    pub fn analog_sources(&self) -> Vec<String> {
        let mut list = vec![self.steering.source.clone()];
        list.extend(PEDALS.iter().filter_map(|name| self.pedal(name)).map(|pedal| pedal.source.clone()));
        list.retain(|source| !source.is_empty());
        list
    }
}

/// True for text that can sit between the file's double quotes: no quote, backslash or control character.
fn is_plain_text(text: &str) -> bool {
    !text.chars().any(|c| c == '"' || c == '\\' || c.is_control())
}

/// `text` without the characters [`is_plain_text`] refuses, trimmed, at most 96 characters.
pub fn plain_text(text: &str) -> String {
    let kept: String = text.chars().filter(|c| *c != '"' && *c != '\\' && !c.is_control()).collect();
    kept.trim().chars().take(96).collect()
}

// ---- the file on disk: load and migration -------------------------------------------------------------------------

/// What was found where the wheel file lives.
#[derive(Clone, Debug, PartialEq)]
pub enum LoadState {
    /// No file: the unassigned mapping, the wheel off (TASK-189). Nothing is written until the player changes something.
    Missing,
    /// A DFP file read as it is.
    Loaded,
    /// A 1.2.x file (no `mode`, or the pad bridge's keys) or a `mode = "pad"` file. Its bindings are kept and it must be
    /// written again as a DFP file (the caller does it at once). The text says what was found.
    Migrated(String),
    /// The game refuses the file (the error, `line N: ...`). The unassigned mapping is used; the file is kept until a change.
    Refused(String),
    /// A version this launcher does not know (a newer launcher wrote it): kept read only.
    Newer(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Loaded {
    pub config: WheelConfig,
    pub state: LoadState,
}

/// Reads `path` for the launcher: missing, read, migrated, refused or newer (see [`LoadState`]). A file that is not there
/// (or cannot be used) starts the mapping unassigned (TASK-189: no guessed controls); nothing is written until a change.
pub fn load(path: &Path) -> Loaded {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Loaded { config: WheelConfig::unassigned(), state: LoadState::Missing };
        }
        Err(error) => {
            return Loaded { config: WheelConfig::unassigned(), state: LoadState::Refused(format!("cannot read the file: {error}")) };
        }
    };
    load_text(&text)
}

/// [`load`] on the text of a file.
pub fn load_text(text: &str) -> Loaded {
    match WheelConfig::read(text, false) {
        Ok((config, notes)) => {
            let state = if notes.mode.is_none() {
                LoadState::Migrated("a 1.2.x wheel file (no mode): rewritten for the Driving Force Pro".into())
            } else if !notes.legacy_keys.is_empty() {
                LoadState::Migrated(format!("1.2.x keys ({}) dropped", notes.legacy_keys.join(", ")))
            } else {
                LoadState::Loaded
            };
            Loaded { config, state }
        }
        Err(error) => {
            if error.contains("unsupported wheel config version") {
                return Loaded { config: WheelConfig::unassigned(), state: LoadState::Newer(error) };
            }
            match WheelConfig::read(text, true) {
                Ok((config, notes)) if notes.mode.as_deref() == Some("pad") => Loaded {
                    config,
                    state: LoadState::Migrated("mode \"pad\" (the removed DualShock 2 bridge): its bindings now drive the Driving Force Pro".into()),
                },
                _ => Loaded { config: WheelConfig::unassigned(), state: LoadState::Refused(error) },
            }
        }
    }
}

// ---- named presets --------------------------------------------------------------------------------------------------

/// A preset name the launcher can use as a file name: letters, digits, spaces and `- _ ( ) . +`, 1 to
/// [`PRESET_NAME_MAX`] characters, not only dots. Returns the trimmed name.
pub fn preset_name(name: &str) -> Result<String, String> {
    let name = name.trim();
    if name.is_empty() {
        return Err("a preset needs a name".into());
    }
    if name.chars().count() > PRESET_NAME_MAX {
        return Err(format!("a preset name has at most {PRESET_NAME_MAX} characters"));
    }
    if !name.chars().all(|c| c.is_alphanumeric() || matches!(c, ' ' | '-' | '_' | '(' | ')' | '.' | '+')) {
        return Err("a preset name may hold letters, digits, spaces and - _ ( ) . + only".into());
    }
    if name.chars().all(|c| c == '.') || name.ends_with('.') {
        return Err("a preset name may not end with a dot".into());
    }
    // Windows device names cannot be file names, with or without an extension.
    let upper = name.to_ascii_uppercase();
    let reserved = matches!(upper.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || ((upper.starts_with("COM") || upper.starts_with("LPT")) && upper.len() == 4 && upper.as_bytes()[3].is_ascii_digit());
    if reserved {
        return Err("that preset name is reserved by Windows".into());
    }
    Ok(name.to_string())
}

/// The file of preset `name` in `folder`.
pub fn preset_path(folder: &Path, name: &str) -> Result<PathBuf, String> {
    Ok(folder.join(format!("{}.toml", preset_name(name)?)))
}

/// One preset file, as the page lists it.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct PresetEntry {
    pub name: String,
    pub device_name: String,
    /// The template or preset label inside the file.
    pub preset: String,
    /// The file is not a valid wheel file (the reason); it is listed so the player can delete it.
    pub problem: Option<String>,
}

/// The presets in `folder`, by name.
pub fn list_presets(folder: &Path) -> Vec<PresetEntry> {
    let mut list: Vec<PresetEntry> = fs::read_dir(folder)
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.extension().and_then(|extension| extension.to_str()) == Some("toml"))
        .filter_map(|path| {
            let name = path.file_stem()?.to_str()?.to_string();
            preset_name(&name).ok()?;
            let loaded = load(&path);
            let problem = match &loaded.state {
                LoadState::Refused(error) | LoadState::Newer(error) => Some(error.clone()),
                _ => None,
            };
            Some(PresetEntry { name, device_name: loaded.config.device_name, preset: loaded.config.preset, problem })
        })
        .collect();
    list.sort_by_key(|entry| entry.name.to_lowercase());
    list
}

/// Writes `config` as preset `name` (the file's `preset` label becomes the name; `enabled` is not part of a preset).
pub fn save_preset(folder: &Path, name: &str, config: &WheelConfig) -> Result<String, String> {
    let name = preset_name(name)?;
    let mut preset = config.clone();
    preset.preset = name.clone();
    preset.enabled = true;
    preset.save_atomic(&folder.join(format!("{name}.toml")))?;
    Ok(name)
}

/// Reads preset `name`: a 1.2.x or `pad` preset is migrated like `wheel.toml`; a refused one is an error.
pub fn read_preset(folder: &Path, name: &str) -> Result<WheelConfig, String> {
    let loaded = load(&preset_path(folder, name)?);
    match loaded.state {
        LoadState::Missing => Err(format!("no preset named '{name}'")),
        LoadState::Refused(error) | LoadState::Newer(error) => Err(error),
        LoadState::Loaded | LoadState::Migrated(_) => Ok(loaded.config),
    }
}

/// Deletes preset `name`.
pub fn delete_preset(folder: &Path, name: &str) -> Result<(), String> {
    let path = preset_path(folder, name)?;
    fs::remove_file(&path).map_err(|error| format!("cannot delete {}: {error}", path.display()))
}

// ---- what the page draws: the live values, as the game computes them ---------------------------------------------

/// A pedal axis's travel 0..1 before calibration (`normalize_pedal_axis`).
pub fn pedal_travel(mode: &str, raw: i16) -> f64 {
    let raw = f64::from(raw);
    match mode {
        "inverted" => ((32767.0 - raw) / 65535.0).clamp(0.0, 1.0),
        "normal" => ((raw + 32768.0) / 65535.0).clamp(0.0, 1.0),
        "positive" | "combined_positive" => {
            if raw <= 0.0 {
                0.0
            } else {
                (raw / 32767.0).clamp(0.0, 1.0)
            }
        }
        "combined_negative" => {
            if raw >= 0.0 {
                0.0
            } else {
                (-raw / 32768.0).clamp(0.0, 1.0)
            }
        }
        _ => 0.0,
    }
}

/// The byte the game sends for a pedal axis (`read_pedal`): 0 inside the dead zone, else 1..255.
pub fn pedal_byte(pedal: &PedalConfig, raw: i16) -> u8 {
    let travel = pedal_travel(&pedal.mode, raw);
    let deadzone = pedal.deadzone.clamp(0.0, 0.45);
    let saturation = pedal.saturation.clamp((deadzone + 0.05).min(1.0), 1.0);
    if travel <= deadzone {
        return 0;
    }
    let normalized = ((travel - deadzone) / (saturation - deadzone)).clamp(0.0, 1.0);
    let curved = match pedal.curve.as_str() {
        "quadratic" => normalized * normalized,
        "cubic" => normalized * normalized * normalized,
        _ => normalized,
    }
    .clamp(0.0, 1.0);
    ((curved * 255.0).round() as i32).clamp(1, 255) as u8
}

/// The pedal mode from an axis at rest and pressed (the capture's polarity check).
pub fn detect_pedal_mode(rest: i16, pressed: i16) -> &'static str {
    let (rest, pressed) = (i32::from(rest), i32::from(pressed));
    if rest > 16000 && pressed < rest - 8000 {
        "inverted"
    } else if rest < -16000 && pressed > rest + 8000 {
        "normal"
    } else if rest.abs() <= 16000 && pressed < -10000 {
        "combined_negative"
    } else if rest.abs() <= 16000 && pressed > 10000 {
        "positive"
    } else if pressed < rest {
        "inverted"
    } else {
        "normal"
    }
}

/// The wheel's angle in degrees for a raw steering axis: the position (-1..1) times half the lock-to-lock range, so a
/// 900-degree wheel reads -450..450 (the angle the game's DFP gets on its own 900 degrees).
pub fn steering_degrees(steering: &SteeringConfig, raw: i16) -> f64 {
    let mut position = if raw < 0 { f64::from(raw) / 32768.0 } else { f64::from(raw) / 32767.0 };
    if steering.invert {
        position = -position;
    }
    position * f64::from(steering.rotation_degrees.clamp(180, 1080)) / 2.0
}

#[cfg(test)]
mod tests {
    use super::*;

    const GUID: &str = "030000006d0400009bc2000000000000";

    fn scratch(name: &str) -> PathBuf {
        let unique = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
        let dir = std::env::temp_dir().join(format!("gt4l-wheel-{name}-{}-{unique}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn refused(text: &str) -> String {
        WheelConfig::from_toml(text).expect_err(text)
    }

    /// A file written by the 1.2.5 launcher (cbfd8b8 `to_toml`), as it was on disk.
    const FILE_1_2_X: &str = "# GT4Recomp racing wheel configuration (TASK-018)
[wheel]
version = 1
enabled = true
device_guid = \"030000006D0400009BC2000000000000\"
device_name = \"Logitech G27 Racing Wheel USB\"
preset = \"logitech_g29\"
pedal_map_mode = \"cross_square\"

[steering]
source = \"axis:0\"
rotation_degrees = 900
effective_degrees = 540
deadzone = 0.000
linearity = \"linear\"
sensitivity = 1.000
invert = false

[throttle]
source = \"axis:2\"
mode = \"inverted\"
deadzone = 0.020
saturation = 0.980
curve = \"linear\"

[brake]
source = \"axis:3\"
mode = \"inverted\"
deadzone = 0.020
saturation = 0.980
curve = \"quadratic\"

[handbrake]
source = \"\"
mode = \"inverted\"
deadzone = 0.050
saturation = 0.950
curve = \"linear\"

[clutch]
source = \"axis:1\"
mode = \"inverted\"
deadzone = 0.050
saturation = 0.950
curve = \"linear\"

[ffb]
enabled = true
strength = 0.800
centering_spring = 0.250
damper = 0.150
curb_rumble = 1.000
invert = false

[buttons]
dpad_up = [\"hat:0:up\"]
dpad_right = [\"hat:0:right\"]
dpad_down = [\"hat:0:down\"]
dpad_left = [\"hat:0:left\"]
cross = [\"btn:0\"]
circle = [\"btn:2\"]
square = [\"btn:1\"]
triangle = [\"btn:3\"]
l1 = [\"btn:7\"]
r1 = [\"btn:6\"]
l2 = [\"btn:5\", \"btn:13\"]
r2 = [\"btn:4\", \"btn:12\"]
l3 = [\"btn:10\"]
r3 = [\"btn:11\"]
select = [\"btn:8\"]
start = [\"btn:9\"]
";

    #[test]
    fn the_default_is_the_games_default_and_off() {
        let config = WheelConfig::default();
        assert!(!config.enabled);
        assert_eq!(config.steering.rotation_degrees, 900);
        assert_eq!(config.throttle.source, "axis:1");
        assert_eq!(config.buttons["r2"], vec!["btn:4"], "gear up (DFP R1) is the right paddle, button 4 by default");
        assert!(config.validate().is_ok());
    }

    #[test]
    fn a_written_file_reads_back_as_the_same_model_and_holds_only_dfp_keys() {
        let mut config = WheelConfig::default();
        config.enabled = true;
        config.device_guid = GUID.into();
        config.device_name = "Logitech G27 Racing Wheel USB".into();
        config.preset = "logitech_legacy".into();
        config.throttle.source = format!("dev:{GUID}:axis:2");
        config.buttons.insert("r2".into(), vec!["btn:4".into(), "hat:0:right".into(), "axis:5+".into()]);
        let text = config.to_toml();
        assert_eq!(WheelConfig::from_toml(&text).unwrap(), config);
        assert!(text.starts_with("# GT4Recomp racing wheel"), "the slot comment comes first");
        assert!(text.contains("r2 -> gear up (DFP R1, the right paddle)"));
        assert!(text.contains("mode = \"dfp\"\n"));
        for legacy in ["pedal_map_mode", "effective_degrees", "linearity", "sensitivity", "centering_spring", "curb_rumble"] {
            assert!(!text.contains(legacy), "{legacy} is a 1.2.x key and is not written");
        }
        let (_, notes) = WheelConfig::read(&text, false).unwrap();
        assert_eq!(notes.mode.as_deref(), Some("dfp"));
        assert!(notes.legacy_keys.is_empty());
    }

    #[test]
    fn a_file_written_by_the_1_2_x_launcher_loads_and_is_migrated() {
        let loaded = load_text(FILE_1_2_X);
        assert!(matches!(loaded.state, LoadState::Migrated(_)), "{:?}", loaded.state);
        let config = loaded.config;
        assert!(config.enabled);
        assert_eq!(config.device_guid, GUID, "the GUID is lowercased");
        assert_eq!(config.throttle.source, "axis:2");
        assert_eq!(config.brake.curve, "quadratic");
        assert_eq!(config.ffb.strength, 0.8);
        assert_eq!(config.buttons["r2"], vec!["btn:4", "btn:12"], "the bindings are kept");
        let rewritten = config.to_toml();
        assert!(!rewritten.contains("pedal_map_mode") && !rewritten.contains("effective_degrees"));
        assert_eq!(load_text(&rewritten).state, LoadState::Loaded, "the rewritten file is a plain DFP file");
        // The game accepts the 1.2.x file as it is too (legacy keys ignored, mode defaults to dfp).
        assert!(WheelConfig::from_toml(FILE_1_2_X).is_ok());
    }

    #[test]
    fn a_pad_mode_file_is_refused_by_the_game_rules_but_migrated_with_its_bindings() {
        let text = FILE_1_2_X.replace("pedal_map_mode = \"cross_square\"", "mode = \"pad\"\npedal_map_mode = \"both\"");
        let error = refused(&text);
        assert!(error.contains("mode \"pad\""), "{error}");
        let loaded = load_text(&text);
        assert!(matches!(&loaded.state, LoadState::Migrated(why) if why.contains("pad")), "{:?}", loaded.state);
        assert_eq!(loaded.config.buttons["l2"], vec!["btn:5", "btn:13"]);
        assert!(WheelConfig::from_toml(&loaded.config.to_toml()).is_ok(), "the migrated file is a DFP file the game reads");
    }

    #[test]
    fn every_refusal_of_the_game_is_a_refusal_here() {
        let head = "[wheel]\nversion = 1\n";
        let cases: Vec<(String, &str)> = vec![
            ("[wheel]\nenabled = true\n".into(), "missing required version"),
            ("[wheel]\nversion = 2\n".into(), "unsupported wheel config version"),
            ("[wheel]\nversion = 1.0\n".into(), "invalid integer for version"),
            ("[wheel]\nversion = +1\n".into(), "invalid integer for version"),
            (format!("{head}mode = \"pad\"\n"), "mode \"pad\""),
            (format!("{head}mode = \"other\"\n"), "mode must be"),
            (format!("{head}mode = dfp\n"), "double-quoted"),
            (format!("{head}unknown = 1\n"), "unknown [wheel] key"),
            (format!("{head}enabled = yes\n"), "expected true or false"),
            (format!("{head}device_guid = \"1234\"\n"), "device_guid must be empty"),
            (format!("{head}[extra]\n"), "unknown section"),
            (format!("{head}[steering\n"), "malformed table header"),
            (format!("{head}[steering]\nrotation_degrees\n"), "expected key = value"),
            (format!("{head}[steering]\nrotation_degrees =\n"), "expected key = value"),
            (format!("{head}[steering]\nrotation_degrees = 90\n"), "between 180 and 1080"),
            (format!("{head}[steering]\nrotation_degrees = 1081\n"), "between 180 and 1080"),
            (format!("{head}[steering]\nrotation_degrees = 900.5\n"), "invalid integer"),
            (format!("{head}[steering]\nsource = \"btn:1\"\n"), "steering source must be an axis"),
            (format!("{head}[steering]\nsource = \"axis:0+\"\n"), "steering source must be an axis"),
            (format!("{head}[steering]\nsource = \"axis:32\"\n"), "axis index out of range"),
            (format!("{head}[steering]\nwobble = 1\n"), "unknown [steering] key"),
            (format!("{head}[throttle]\ndeadzone = 0.40\nsaturation = 0.42\n"), "saturation must exceed deadzone"),
            (format!("{head}[throttle]\nsaturation = 0.30\ndeadzone = 0.40\n"), "saturation must exceed deadzone"),
            (format!("{head}[throttle]\ndeadzone = 0.5\n"), "deadzone must be between"),
            (format!("{head}[throttle]\nsaturation = 0.1\n"), "saturation must be between"),
            (format!("{head}[throttle]\nsaturation = 1.01\n"), "saturation must be between"),
            (format!("{head}[throttle]\ndeadzone = nan\n"), "invalid number"),
            (format!("{head}[throttle]\ndeadzone = inf\n"), "invalid number"),
            (format!("{head}[throttle]\nmode = \"flipped\"\n"), "unknown pedal axis mode"),
            (format!("{head}[throttle]\ncurve = \"s\"\n"), "unknown curve"),
            (format!("{head}[throttle]\nsource = \"key:A\"\n"), "unknown wheel source"),
            (format!("{head}[throttle]\nsource = \"btn:128\"\n"), "button index out of range"),
            (format!("{head}[throttle]\nsource = \"hat:8:up\"\n"), "hat index out of range"),
            (format!("{head}[throttle]\nsource = \"hat:0:north\"\n"), "unknown hat direction"),
            (format!("{head}[throttle]\nsource = \"hat:0\"\n"), "malformed hat source"),
            (format!("{head}[throttle]\nsource = \"dev:abc:axis:1\"\n"), "invalid device GUID"),
            (format!("{head}[throttle]\nsource = \"dev:{GUID}\"\n"), "malformed device source"),
            (format!("{head}[throttle]\nsensitivity = 1\n"), "unknown pedal key"),
            (format!("{head}[ffb]\nstrength = 3\n"), "between 0.0 and 2.0"),
            (format!("{head}[ffb]\ndamper = -0.1\n"), "between 0.0 and 1.0"),
            (format!("{head}[ffb]\nrumble = 1\n"), "unknown [ffb] key"),
            (format!("{head}[buttons]\ncross = [\"axis:0\"]\n"), "requires btn:, hat:, or axis:N+/N-"),
            (format!("{head}[buttons]\ncross = [\"btn:1\", \"btn:1\"]\n"), "duplicate source"),
            (format!("{head}[buttons]\ncross = [\"btn:1\",]\n"), "trailing comma"),
            (format!("{head}[buttons]\ncross = [\"btn:1\", , \"btn:2\"]\n"), "empty element"),
            (format!("{head}[buttons]\ncross = btn:1\n"), "expected an array"),
            (format!("{head}[buttons]\nhonk = [\"btn:1\"]\n"), "unknown button input"),
            (format!("{head}device_name = \"G27\\\"\n"), "escape sequences"),
            (format!("{head}device_name = \"G27\n"), "unterminated quoted string"),
        ];
        for (text, expected) in cases {
            let error = refused(&text);
            assert!(error.contains(expected), "{text:?}: {error} (expected {expected})");
            assert!(error.starts_with("line "), "errors name the line: {error}");
            if expected != "mode \"pad\"" {
                assert!(matches!(load_text(&text).state, LoadState::Refused(_) | LoadState::Newer(_)), "{text:?}");
            }
        }
        assert!(matches!(load_text("[wheel]\nversion = 2\n").state, LoadState::Newer(_)), "a newer version is kept read only");
    }

    #[test]
    fn what_the_game_accepts_is_accepted_here() {
        let text = "# leading comment\nversion = 1 # keys before a section are [wheel] keys\nenabled = true\n\
                    [wheel]\nschema = 1\nmode = \"dfp\"\n[steering]\nsource = \"none\"\neffective_degrees = anything\n\
                    [ffb]\ncenturing = 1\n";
        // An unknown [ffb] key is refused even next to accepted ones.
        assert!(WheelConfig::from_toml(text).is_err());
        let ok = "version = 1\nenabled = true\r\n[steering]\nsource = \"none\"\neffective_degrees = anything\n\
                  [buttons]\ngear_up = \"btn:7\"\nhorn = [ ]\nshift_down = [\"dev:030000006D0400009BC2000000000000:btn:5\", \"\"]\n\
                  [throttle]\ndeadzone = .1\nsaturation = 1.\n[ffb]\ncurb_rumble = 3\n";
        let config = WheelConfig::from_toml(ok).unwrap();
        assert!(config.enabled);
        assert_eq!(config.steering.source, "", "none is an empty source");
        assert_eq!(config.buttons["r2"], vec!["btn:7"], "aliases name slots; a single string is a list of one");
        assert!(config.buttons["l3"].is_empty());
        assert_eq!(config.buttons["l2"], vec![format!("dev:{GUID}:btn:5")], "GUIDs are lowercased, empty items skipped");
        assert_eq!(config.throttle.deadzone, 0.1);
        assert_eq!(config.throttle.saturation, 1.0);
    }

    #[test]
    fn validate_refuses_what_the_game_would_refuse_and_save_writes_nothing_then() {
        let dir = scratch("validate");
        let path = dir.join(FILE_NAME);
        let good = WheelConfig::default();
        good.save_atomic(&path).unwrap();
        let before = fs::read_to_string(&path).unwrap();
        let mut bad = good.clone();
        bad.throttle.deadzone = 0.40;
        bad.throttle.saturation = 0.45;
        assert!(bad.validate().unwrap_err().contains("exceed deadzone"));
        assert!(bad.save_atomic(&path).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), before, "a refused save leaves the file");
        let mut variants: Vec<WheelConfig> = Vec::new();
        let mut v = good.clone();
        v.steering.rotation_degrees = 1200;
        variants.push(v);
        let mut v = good.clone();
        v.steering.source = "btn:0".into();
        variants.push(v);
        let mut v = good.clone();
        v.buttons.insert("cross".into(), vec!["axis:3".into()]);
        variants.push(v);
        let mut v = good.clone();
        v.buttons.insert("cross".into(), vec!["btn:1".into(), "btn:1".into()]);
        variants.push(v);
        let mut v = good.clone();
        v.buttons.insert("honk".into(), vec![]);
        variants.push(v);
        let mut v = good.clone();
        v.device_name = "a \"quoted\" name".into();
        variants.push(v);
        let mut v = good.clone();
        v.device_guid = "xyz".into();
        variants.push(v);
        let mut v = good.clone();
        v.ffb.strength = 2.5;
        variants.push(v);
        let mut v = good.clone();
        v.brake.mode = "pad".into();
        variants.push(v);
        for config in variants {
            assert!(config.validate().is_err(), "{config:?}");
        }
        // A number is checked as written: 0.4004 / 0.45049 would pass unrounded, but the file holds 0.400 / 0.450.
        let mut edge = good.clone();
        edge.throttle.deadzone = 0.4004;
        edge.throttle.saturation = 0.45049;
        assert!(edge.validate().is_err());
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn set_setting_keeps_the_model_valid_and_refuses_unknown_keys() {
        let mut config = WheelConfig::default();
        config.set_setting("wheel.enabled", &serde_json::json!(true)).unwrap();
        config.set_setting("steering.rotation_degrees", &serde_json::json!(1080)).unwrap();
        assert!(config.set_setting("steering.rotation_degrees", &serde_json::json!(170)).is_err());
        assert_eq!(config.steering.rotation_degrees, 1080, "a refused change keeps the old value");
        config.set_setting("throttle.deadzone", &serde_json::json!(0.45)).unwrap();
        assert!(config.throttle.saturation > config.throttle.deadzone + 0.05);
        config.set_setting("brake.saturation", &serde_json::json!(0.2)).unwrap();
        assert_eq!(config.brake.saturation, 0.2);
        assert!(config.brake.deadzone < 0.15);
        config.set_setting("ffb.strength", &serde_json::json!(0.6666)).unwrap();
        assert_eq!(config.ffb.strength, 0.667, "numbers are kept as written (3 decimals)");
        config.set_setting("wheel.device_name", &serde_json::json!("G27 \"pro\"\\")).unwrap();
        assert_eq!(config.device_name, "G27 pro");
        for key in ["wheel.pedal_map_mode", "steering.sensitivity", "ffb.centering_spring", "ffb.curb_rumble", "nothing"] {
            assert!(config.set_setting(key, &serde_json::json!(1)).is_err(), "{key} is not a DFP setting");
        }
        assert!(config.set_setting("throttle.mode", &serde_json::json!("sideways")).is_err());
        assert!(config.validate().is_ok());
    }

    #[test]
    fn bindings_replace_add_move_and_clear() {
        let mut config = WheelConfig::default();
        let taken = config.assign_binding("gear_up", "btn:2", None, false).unwrap();
        assert_eq!(config.buttons["r2"], vec!["btn:2"], "gear up is the r2 slot");
        assert_eq!(taken, vec!["circle"], "button 2 left Circle");
        assert!(config.buttons["circle"].is_empty());
        config.assign_binding("r2", "btn:12", None, true).unwrap();
        config.assign_binding("r2", "btn:13", None, true).unwrap();
        config.assign_binding("r2", "btn:14", None, true).unwrap();
        assert_eq!(config.buttons["r2"], vec!["btn:12", "btn:13", "btn:14"], "at most three sources, the oldest goes");
        config.assign_binding("throttle", &format!("dev:{}:axis:2", GUID.to_uppercase()), Some("normal"), false).unwrap();
        assert_eq!(config.throttle.source, format!("dev:{GUID}:axis:2"));
        assert_eq!(config.throttle.mode, "normal");
        assert!(config.assign_binding("steering", "btn:1", None, false).is_err());
        assert!(config.assign_binding("cross", "axis:1", None, false).is_err());
        assert!(config.assign_binding("throttle", "axis:1", Some("diagonal"), false).is_err());
        assert!(config.assign_binding("trunk", "btn:1", None, false).is_err());
        config.reset_binding("cross", true, &WheelConfig::default()).unwrap();
        assert!(config.buttons["cross"].is_empty());
        config.reset_binding("cross", false, &WheelConfig::default()).unwrap();
        assert_eq!(config.buttons["cross"], vec!["btn:0"]);
        config.reset_binding("steering", true, &WheelConfig::default()).unwrap();
        assert!(config.steering.source.is_empty());
        assert_eq!(config.analog_sources().len(), 3, "brake axis 2, clutch axis 3 and the throttle");
    }

    #[test]
    fn presets_save_list_read_and_delete() {
        let dir = scratch("presets");
        let mut config = WheelConfig::default();
        config.device_name = "Logitech G27 Racing Wheel USB".into();
        config.enabled = false;
        assert_eq!(save_preset(&dir, "  G27 (owner) ", &config).unwrap(), "G27 (owner)");
        fs::write(dir.join("broken.toml"), "[wheel]\nversion = 9\n").unwrap();
        fs::write(dir.join("notes.txt"), "not a preset").unwrap();
        let list = list_presets(&dir);
        assert_eq!(list.len(), 2);
        assert_eq!(list[0].name, "broken");
        assert!(list[0].problem.is_some());
        assert_eq!(list[1].name, "G27 (owner)");
        assert_eq!(list[1].device_name, "Logitech G27 Racing Wheel USB");
        let read = read_preset(&dir, "G27 (owner)").unwrap();
        assert_eq!(read.preset, "G27 (owner)");
        assert!(read_preset(&dir, "broken").is_err());
        assert!(read_preset(&dir, "missing").is_err());
        for bad in ["", "a/b", "..", "x.", "con:", "CON", "lpt1", &"n".repeat(PRESET_NAME_MAX + 1)] {
            assert!(preset_name(bad).is_err(), "{bad:?}");
        }
        delete_preset(&dir, "G27 (owner)").unwrap();
        assert_eq!(list_presets(&dir).len(), 1);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn a_missing_file_starts_unassigned_and_nothing_is_written() {
        let dir = scratch("missing");
        let path = dir.join(FILE_NAME);
        let loaded = load(&path);
        assert_eq!(loaded.state, LoadState::Missing);
        assert!(!loaded.config.enabled);
        assert!(loaded.config.is_unassigned(), "no preset, no guessed control (TASK-189)");
        assert!(loaded.config.steering.source.is_empty());
        assert!(loaded.config.buttons["cross"].is_empty());
        assert!(loaded.config.validate().is_ok(), "an unassigned mapping is a valid file");
        assert!(!path.exists());
        let _ = fs::remove_dir_all(dir);
    }

    /// TASK-189: the launcher's starting mapping never guesses a control; the game's own default (what a file that leaves
    /// keys out means) keeps its indices, so `default()` and `unassigned()` stay two different things.
    #[test]
    fn the_unassigned_mapping_holds_no_control_and_keeps_the_calibration() {
        let config = WheelConfig::unassigned();
        assert!(!config.enabled);
        assert_eq!(config.preset, "generic");
        assert_eq!(config.steering.rotation_degrees, 900);
        assert_eq!((config.throttle.deadzone, config.throttle.saturation), (0.02, 0.98));
        assert_eq!((config.ffb.strength, config.ffb.damper), (1.0, 0.15));
        assert!(config.is_unassigned());
        assert!(config.validate().is_ok());
        let text = config.to_toml();
        let back = WheelConfig::from_toml(&text).expect("the game reads it");
        assert!(back.is_unassigned(), "an unassigned file stays unassigned through the game's parser");
        assert!(!WheelConfig::default().is_unassigned(), "the game's default still has its own indices");
    }

    #[test]
    fn the_live_values_follow_the_game() {
        let pedal = PedalConfig { source: "axis:1".into(), mode: "inverted".into(), deadzone: 0.02, saturation: 0.98, curve: "linear".into() };
        assert_eq!(pedal_byte(&pedal, 32767), 0, "an inverted pedal at rest");
        assert_eq!(pedal_byte(&pedal, -32768), 255);
        assert!(pedal_byte(&pedal, 0) > 120 && pedal_byte(&pedal, 0) < 135);
        assert_eq!(pedal_travel("combined_negative", 100), 0.0);
        assert_eq!(pedal_travel("normal", -32768), 0.0);
        assert_eq!(detect_pedal_mode(32767, -30000), "inverted");
        assert_eq!(detect_pedal_mode(-32768, 30000), "normal");
        assert_eq!(detect_pedal_mode(0, -30000), "combined_negative");
        assert_eq!(detect_pedal_mode(0, 30000), "positive");
        let steering = SteeringConfig { source: "axis:0".into(), rotation_degrees: 900, invert: false };
        assert_eq!(steering_degrees(&steering, 32767), 450.0);
        assert_eq!(steering_degrees(&steering, -32768), -450.0);
        assert!((steering_degrees(&steering, 6554) - 90.0).abs() < 0.1, "a fifth of the travel is 90 degrees at 900");
        let inverted = SteeringConfig { invert: true, rotation_degrees: 1080, ..steering };
        assert_eq!(steering_degrees(&inverted, 32767), -540.0);
    }
}
