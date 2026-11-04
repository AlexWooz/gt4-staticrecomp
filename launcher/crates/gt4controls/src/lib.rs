//! Controls file for the game (`GT4_CONTROLS_FILE`), schema 1, as specified in W3's `docs/CONTROLS.md`
//! (task/029-input-mapping). The launcher writes the file and validates it before Play; the game's C++
//! reader applies the full rules. Changes here must follow that document.
//!
//! The mirrored tables below follow `docs/CONTROLS.md`. `tests/docs_match.rs` fails when they drift from
//! the document or from `docs/controls/*.toml`.

use std::collections::BTreeMap;
use std::fmt;
use std::path::Path;

pub const SCHEMA_VERSION: u32 = 1;
pub const CONTROLS_FILE_NAME: &str = "controls.toml";

/// Digital inputs (pressure-sensitive ones take analog sources when available).
pub const BUTTON_INPUTS: &[&str] = &[
    "dpad_up", "dpad_right", "dpad_down", "dpad_left", "cross", "circle", "square", "triangle", "l1", "r1",
    "l2", "r2", "l3", "r3", "select", "start",
];
/// Stick axes.
pub const STICK_INPUTS: &[&str] = &["rstick_x", "rstick_y", "lstick_x", "lstick_y"];

const XINPUT_BUTTONS: &[&str] = &[
    "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LThumb", "RThumb", "DpadUp", "DpadDown", "DpadLeft",
    "DpadRight",
];
const XINPUT_TRIGGERS: &[&str] = &["LT", "RT"];
const XINPUT_AXES: &[&str] = &["LX", "LY", "RX", "RY"];
const RESERVED_KEYS: &[&str] = &["F7", "F8", "F9", "F11"];

/// Built-in mapping: each input and its default sources (the game's built-in map).
pub fn default_bindings() -> BTreeMap<String, Vec<String>> {
    let entries: &[(&str, &[&str])] = &[
        // TASK-076 keyboard layout, the same table as native/controls.cpp and docs/controls/default.toml
        // (tests/docs_match.rs checks both). The launcher writes every input to controls.toml, so a stale table here
        // would replace the game's keyboard layout after the first remap.
        ("dpad_up", &["xinput:DpadUp", "key:Up", "key:E"]),
        ("dpad_right", &["xinput:DpadRight", "key:D", "key:Right"]),
        ("dpad_down", &["xinput:DpadDown", "key:Down"]),
        ("dpad_left", &["xinput:DpadLeft", "key:A", "key:Left"]),
        ("cross", &["xinput:A", "key:W", "key:Enter"]),
        ("circle", &["xinput:B", "key:Space"]),
        ("square", &["xinput:X", "key:S"]),
        ("triangle", &["xinput:Y", "key:Backspace", "key:Esc"]),
        ("l1", &["xinput:LB", "key:Q"]),
        ("r1", &["xinput:RB", "key:Shift"]),
        ("l2", &["xinput:LT", "key:F"]),
        ("r2", &["xinput:RT", "key:R"]),
        ("l3", &["xinput:LThumb"]),
        ("r3", &["xinput:RThumb"]),
        ("select", &["xinput:Back", "key:C", "key:Tab"]),
        ("start", &["xinput:Start", "key:P"]),
        ("rstick_x", &["xinput:RX"]),
        ("rstick_y", &["xinput:RY | invert=true"]),
        ("lstick_x", &["xinput:LX"]),
        ("lstick_y", &["xinput:LY | invert=true"]),
    ];
    entries
        .iter()
        .map(|(name, sources)| (name.to_string(), sources.iter().map(|source| source.to_string()).collect()))
        .collect()
}

#[derive(Clone, Debug, PartialEq)]
pub struct Controls {
    bindings: BTreeMap<String, Vec<String>>,
}

impl Default for Controls {
    fn default() -> Self {
        Controls { bindings: default_bindings() }
    }
}

/// A refusal. `line` is 1-based, or 0 when the problem is not tied to one line.
#[derive(Debug, PartialEq, Eq)]
pub struct ControlsError {
    pub line: usize,
    pub message: String,
}

impl fmt::Display for ControlsError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.line == 0 {
            write!(formatter, "{}", self.message)
        } else {
            write!(formatter, "line {}: {}", self.line, self.message)
        }
    }
}

impl std::error::Error for ControlsError {}

fn error(line: usize, message: impl Into<String>) -> ControlsError {
    ControlsError { line, message: message.into() }
}

/// Curve of the trigger pressure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Curve {
    Linear,
    Quadratic,
    Cubic,
    SCurve,
    SmoothPower,
    Sublinear,
    Logarithmic,
}

impl Curve {
    pub fn as_str(self) -> &'static str {
        match self {
            Curve::Linear => "linear",
            Curve::Quadratic => "quadratic",
            Curve::Cubic => "cubic",
            Curve::SCurve => "s_curve",
            Curve::SmoothPower => "smooth_power",
            Curve::Sublinear => "sublinear",
            Curve::Logarithmic => "logarithmic",
        }
    }
}

/// Analog trigger tuning used by the Controls page (stored as options of the trigger source).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TriggerTuning {
    pub deadzone: f32,
    pub curve: Curve,
}

/// Default trigger deadzone: 30 of 255 raw units.
pub const TRIGGER_DEFAULT_DEADZONE: f32 = 30.0 / 255.0;
/// Default left stick deadzone: XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE (7849) of 32767 (native/controls.cpp).
pub const LEFT_STICK_DEFAULT_DEADZONE: f32 = 7849.0 / 32767.0;
/// Default right stick deadzone: XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE (8689) of 32767 (native/controls.cpp).
pub const RIGHT_STICK_DEFAULT_DEADZONE: f32 = 8689.0 / 32767.0;
/// Largest deadzone the launcher writes for a trigger or a stick (the file accepts values below 1).
pub const MAX_DEADZONE: f32 = 0.95;

/// One of the two PS2 analog sticks, as the Controls page tunes it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stick {
    Left,
    Right,
}

impl Stick {
    /// The two axis inputs of the stick in the controls file.
    pub fn inputs(self) -> [&'static str; 2] {
        match self {
            Stick::Left => ["lstick_x", "lstick_y"],
            Stick::Right => ["rstick_x", "rstick_y"],
        }
    }
}

/// What `Controls::assign` changed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Assignment {
    /// The other input that held the source and was swapped (or lost it), if any.
    pub swapped_with: Option<String>,
    /// True when the input already had exactly this source: nothing changed.
    pub unchanged: bool,
}

/// `xinput:A | deadzone=0.1` -> `xinput:A`.
pub fn source_base(source: &str) -> &str {
    split_options(source).0
}

/// `xinput` or `key`: the family of a source (the part before the first `:`), or the whole base without one.
pub fn source_family(source: &str) -> &str {
    let base = source_base(source);
    base.split_once(':').map_or(base, |(family, _)| family)
}

fn is_trigger_source(source: &str) -> bool {
    matches!(source_base(source), "xinput:LT" | "xinput:RT")
}

fn is_axis_source(source: &str) -> bool {
    matches!(source_base(source), "xinput:LX" | "xinput:LY" | "xinput:RX" | "xinput:RY")
}

/// The source with option `key` set to `value` (replaced when present, appended otherwise). Other options keep
/// their order.
fn with_option(source: &str, key: &str, value: &str) -> String {
    let (base, options) = split_options(source);
    let prefix = format!("{key}=");
    let mut kept: Vec<String> = options
        .iter()
        .filter(|option| !option.starts_with(&prefix))
        .map(|option| option.to_string())
        .collect();
    kept.push(format!("{key}={value}"));
    format!("{base} | {}", kept.join(" "))
}

/// XInput sources first, then keys, each in their current order (the drawing shows the first one of each family).
fn ordered(sources: Vec<String>) -> Vec<String> {
    let (pad, other): (Vec<String>, Vec<String>) =
        sources.into_iter().partition(|source| source_family(source) == "xinput");
    pad.into_iter().chain(other).collect()
}

impl Controls {
    pub fn bindings(&self) -> &BTreeMap<String, Vec<String>> {
        &self.bindings
    }

    pub fn sources(&self, input: &str) -> Option<&[String]> {
        self.bindings.get(input).map(|sources| sources.as_slice())
    }

    /// Replaces the sources of one input after checking each one. An empty list unbinds the input.
    pub fn set_sources(&mut self, input: &str, sources: Vec<String>) -> Result<(), ControlsError> {
        let kind = input_kind(input).ok_or_else(|| error(0, format!("unknown input '{input}'")))?;
        check_list(0, input, kind, &sources)?;
        self.bindings.insert(input.to_string(), sources);
        Ok(())
    }

    /// Trigger tuning of an input, read from its analog trigger source (`xinput:LT` or `xinput:RT`).
    pub fn trigger_tuning(&self, input: &str) -> TriggerTuning {
        let mut tuning = TriggerTuning { deadzone: TRIGGER_DEFAULT_DEADZONE, curve: Curve::Linear };
        let source = self.sources(input).and_then(|sources| {
            sources.iter().find(|source| source.starts_with("xinput:LT") || source.starts_with("xinput:RT"))
        });
        if let Some(source) = source {
            let (_, options) = split_options(source);
            for option in options {
                if let Some(value) = option.strip_prefix("deadzone=") {
                    if let Ok(parsed) = value.parse::<f32>() {
                        tuning.deadzone = parsed;
                    }
                }
                match option {
                    "curve=quadratic" => tuning.curve = Curve::Quadratic,
                    "curve=cubic" => tuning.curve = Curve::Cubic,
                    "curve=s_curve" | "curve=scurve" | "curve=s-curve" => tuning.curve = Curve::SCurve,
                    "curve=smooth_power" | "curve=smooth" => tuning.curve = Curve::SmoothPower,
                    "curve=sublinear" | "curve=root" => tuning.curve = Curve::Sublinear,
                    "curve=logarithmic" | "curve=log" => tuning.curve = Curve::Logarithmic,
                    _ => {}
                }
            }
        }
        tuning
    }

    /// Writes the tuning into the analog trigger source of `input`. The deadzone is clamped to [0, 0.95]. A `range`
    /// option (TASK-147: the pressure byte at full travel) is kept.
    pub fn set_trigger_tuning(&mut self, input: &str, tuning: TriggerTuning) -> Result<(), ControlsError> {
        let current: Vec<String> = self.sources(input).map(|sources| sources.to_vec()).unwrap_or_default();
        let deadzone = tuning.deadzone.clamp(0.0, 0.95);
        let options = format!("deadzone={deadzone:.3} curve={}", tuning.curve.as_str());
        let updated: Vec<String> = current
            .into_iter()
            .map(|source| {
                if source.starts_with("xinput:LT") || source.starts_with("xinput:RT") {
                    let (base, existing) = split_options(&source);
                    let range = existing.iter().find(|option| option.starts_with("range=")).map(|option| format!(" {option}"));
                    format!("{base} | {options}{}", range.unwrap_or_default())
                } else {
                    source
                }
            })
            .collect();
        self.set_sources(input, updated)
    }

    /// Deadzone of a stick, read from its first XInput axis source. Without a `deadzone` option, the default of the
    /// physical stick that source reads (LX/LY left, RX/RY right) applies, as in native/controls.cpp.
    pub fn stick_deadzone(&self, stick: Stick) -> f32 {
        let source = stick.inputs().iter().find_map(|input| {
            self.sources(input).and_then(|sources| sources.iter().find(|source| is_axis_source(source)).cloned())
        });
        let Some(source) = source else {
            return match stick {
                Stick::Left => LEFT_STICK_DEFAULT_DEADZONE,
                Stick::Right => RIGHT_STICK_DEFAULT_DEADZONE,
            };
        };
        let (base, options) = split_options(&source);
        for option in options {
            if let Some(value) = option.strip_prefix("deadzone=") {
                if let Ok(parsed) = value.parse::<f32>() {
                    return parsed;
                }
            }
        }
        if base.starts_with("xinput:L") {
            LEFT_STICK_DEFAULT_DEADZONE
        } else {
            RIGHT_STICK_DEFAULT_DEADZONE
        }
    }

    /// Writes `deadzone` (clamped to [0, MAX_DEADZONE]) on every XInput axis source of both axes of the stick.
    /// Other options (`invert`, `curve`, `scale`) are kept.
    pub fn set_stick_deadzone(&mut self, stick: Stick, deadzone: f32) -> Result<(), ControlsError> {
        let value = format!("{:.3}", deadzone.clamp(0.0, MAX_DEADZONE));
        let mut next = self.clone();
        for input in stick.inputs() {
            let updated: Vec<String> = self
                .sources(input)
                .unwrap_or(&[])
                .iter()
                .map(|source| if is_axis_source(source) { with_option(source, "deadzone", &value) } else { source.clone() })
                .collect();
            next.set_sources(input, updated)?;
        }
        *self = next;
        Ok(())
    }

    /// The button input other than `except` whose sources include `source` (compared without options).
    pub fn holder_of(&self, source: &str, except: &str) -> Option<String> {
        let base = source_base(source);
        BUTTON_INPUTS
            .iter()
            .filter(|input| **input != except)
            .find(|input| {
                self.sources(input)
                    .is_some_and(|sources| sources.iter().any(|existing| source_base(existing) == base))
            })
            .map(|input| input.to_string())
    }

    /// Binds `source` to the button input `input`, in place of the input's sources of the same family (`xinput:` or
    /// `key:`); sources of the other family stay. A trigger that replaces a trigger keeps its options (deadzone,
    /// curve).
    ///
    /// When another button input already has the source, `swap` gives that input this input's previous source of the
    /// same family (or removes the source from it when there was none). Without `swap` such a call is refused, so a
    /// press never drives two PS2 buttons by accident. Nothing changes when any check fails.
    pub fn assign(&mut self, input: &str, source: &str, swap: bool) -> Result<Assignment, ControlsError> {
        if input_kind(input) != Some(InputKind::Button) {
            return Err(error(0, format!("'{input}' is not a button input")));
        }
        let base = source_base(source).to_string();
        validate_source(0, InputKind::Button, &base)?;
        let family = source_family(&base).to_string();
        let current: Vec<String> = self.sources(input).unwrap_or(&[]).to_vec();
        if current.iter().any(|existing| source_base(existing) == base) {
            return Ok(Assignment { swapped_with: None, unchanged: true });
        }
        let previous: Option<String> = current.iter().find(|existing| source_family(existing) == family).cloned();
        let new_source = match &previous {
            Some(old) if is_trigger_source(old) && is_trigger_source(&base) => {
                let (_, options) = split_options(old);
                if options.is_empty() {
                    base.clone()
                } else {
                    format!("{base} | {}", options.join(" "))
                }
            }
            _ => base.clone(),
        };
        let holder = self.holder_of(&base, input);
        let mut next = self.clone();
        if let Some(holder) = &holder {
            if !swap {
                return Err(error(0, format!("'{base}' is already bound to {holder}")));
            }
            let mut replaced: Vec<String> = Vec::new();
            for existing in self.sources(holder).unwrap_or(&[]) {
                let incoming = if source_base(existing) == base { previous.clone() } else { Some(existing.clone()) };
                if let Some(incoming) = incoming {
                    if !replaced.iter().any(|kept| source_base(kept) == source_base(&incoming)) {
                        replaced.push(incoming);
                    }
                }
            }
            next.set_sources(holder, ordered(replaced))?;
        }
        let mut sources: Vec<String> =
            current.into_iter().filter(|existing| source_family(existing) != family).collect();
        sources.push(new_source);
        next.set_sources(input, ordered(sources))?;
        *self = next;
        Ok(Assignment { swapped_with: holder, unchanged: false })
    }

    /// Every input back to the built-in mapping, including the trigger and stick options.
    pub fn reset_all(&mut self) {
        self.bindings = default_bindings();
    }

    /// True when `input` has exactly its built-in sources.
    pub fn is_default(&self, input: &str) -> bool {
        default_bindings().get(input).map(|sources| sources.as_slice()) == self.sources(input)
    }

    /// The file text (schema 1). Every input is written, so the file is complete on its own.
    pub fn render(&self) -> String {
        let mut text = String::from("# GT4Recomp controls. Read by the game when it starts.\n");
        text.push_str(&format!("schema = {SCHEMA_VERSION}\n\n[bindings]\n"));
        for input in BUTTON_INPUTS.iter().chain(STICK_INPUTS.iter()) {
            let sources = self.bindings.get(*input).cloned().unwrap_or_default();
            let quoted: Vec<String> = sources.iter().map(|source| format!("\"{source}\"")).collect();
            text.push_str(&format!("{input} = [{}]\n", quoted.join(", ")));
        }
        text
    }

    /// Parses and checks a file. Refusals name the line, as the game does. Inputs not listed keep
    /// their defaults.
    pub fn parse(text: &str) -> Result<Controls, ControlsError> {
        let mut schema_seen = false;
        let mut in_bindings = false;
        let mut bindings: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for (index, raw) in text.lines().enumerate() {
            let line_number = index + 1;
            let line = strip_comment(raw).trim();
            if line.is_empty() {
                continue;
            }
            if let Some(table) = line.strip_prefix('[') {
                let name = table
                    .strip_suffix(']')
                    .ok_or_else(|| error(line_number, "malformed table header"))?;
                if name != "bindings" {
                    return Err(error(line_number, format!("unknown table '{name}'")));
                }
                in_bindings = true;
                continue;
            }
            let (key, value) = line
                .split_once('=')
                .ok_or_else(|| error(line_number, "expected 'key = value'"))?;
            let key = key.trim();
            let value = value.trim();
            if !in_bindings {
                if key != "schema" {
                    return Err(error(line_number, format!("unknown key '{key}' outside [bindings]")));
                }
                if value != "1" {
                    return Err(error(line_number, "schema must be exactly 1"));
                }
                schema_seen = true;
                continue;
            }
            let kind = input_kind(key).ok_or_else(|| error(line_number, format!("unknown input '{key}'")))?;
            if bindings.contains_key(key) {
                return Err(error(line_number, format!("input '{key}' appears twice")));
            }
            let sources = parse_sources(line_number, value)?;
            check_list(line_number, key, kind, &sources)?;
            bindings.insert(key.to_string(), sources);
        }
        if !schema_seen {
            return Err(error(0, "schema = 1 is required"));
        }
        let mut merged = default_bindings();
        merged.extend(bindings);
        Ok(Controls { bindings: merged })
    }
}

/// Reads and checks an existing file. A missing file is not an error: the built-in bindings apply.
pub fn load(path: &Path) -> Result<Option<Controls>, String> {
    match std::fs::read_to_string(path) {
        Ok(text) => Controls::parse(&text)
            .map(Some)
            .map_err(|error| format!("{}: {error}", path.display())),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(format!("{}: {error}", path.display())),
    }
}

/// Writes the file atomically (temporary file, then rename).
pub fn save(path: &Path, controls: &Controls) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("toml.tmp");
    std::fs::write(&temporary, controls.render())?;
    std::fs::rename(&temporary, path)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum InputKind {
    Button,
    Stick,
}

fn input_kind(input: &str) -> Option<InputKind> {
    if BUTTON_INPUTS.contains(&input) {
        Some(InputKind::Button)
    } else if STICK_INPUTS.contains(&input) {
        Some(InputKind::Stick)
    } else {
        None
    }
}

fn check_list(line: usize, input: &str, kind: InputKind, sources: &[String]) -> Result<(), ControlsError> {
    let mut seen: Vec<&str> = Vec::new();
    for source in sources {
        validate_source(line, kind, source)?;
        if seen.contains(&source.as_str()) {
            return Err(error(line, format!("duplicate source '{source}' on {input}")));
        }
        seen.push(source);
    }
    Ok(())
}

fn strip_comment(line: &str) -> &str {
    let mut quoted = false;
    for (index, character) in line.char_indices() {
        match character {
            '"' => quoted = !quoted,
            '#' if !quoted => return &line[..index],
            _ => {}
        }
    }
    line
}

/// `["a", "b"]` or a single `"a"` -> the quoted strings. Strings cannot contain `"`.
fn parse_sources(line: usize, value: &str) -> Result<Vec<String>, ControlsError> {
    let value = value.trim();
    if let Some(inner) = value.strip_prefix('[') {
        let inner = inner
            .strip_suffix(']')
            .ok_or_else(|| error(line, "list is not closed with ']'"))?;
        if inner.trim().is_empty() {
            return Ok(Vec::new());
        }
        inner.split(',').map(|item| parse_quoted(line, item.trim())).collect()
    } else {
        Ok(vec![parse_quoted(line, value)?])
    }
}

fn parse_quoted(line: usize, text: &str) -> Result<String, ControlsError> {
    let inner = text
        .strip_prefix('"')
        .and_then(|rest| rest.strip_suffix('"'))
        .ok_or_else(|| error(line, format!("source must be in double quotes: {text}")))?;
    if inner.contains('"') {
        return Err(error(line, "strings cannot contain '\"'"));
    }
    Ok(inner.to_string())
}

fn split_options(source: &str) -> (&str, Vec<&str>) {
    match source.split_once('|') {
        Some((base, options)) => (base.trim(), options.split_whitespace().collect()),
        None => (source.trim(), Vec::new()),
    }
}

fn validate_source(line: usize, kind: InputKind, source: &str) -> Result<(), ControlsError> {
    let (base, options) = split_options(source);
    let (family, name) = base
        .split_once(':')
        .ok_or_else(|| error(line, format!("source '{base}' needs a prefix 'xinput:' or 'key:'")))?;
    let mut analog = false;
    match family {
        "xinput" => {
            analog = XINPUT_TRIGGERS.contains(&name) || XINPUT_AXES.contains(&name);
            if !(XINPUT_BUTTONS.contains(&name) || analog) {
                return Err(error(line, format!("unknown XInput source '{name}'")));
            }
            if kind == InputKind::Stick && !XINPUT_AXES.contains(&name) {
                return Err(error(line, format!("'{name}' cannot drive a stick")));
            }
            if kind == InputKind::Button && XINPUT_AXES.contains(&name) {
                return Err(error(line, format!("stick axis '{name}' cannot drive a button")));
            }
        }
        "key" => {
            if kind == InputKind::Stick {
                return Err(error(line, "keyboard keys cannot drive a stick"));
            }
            if RESERVED_KEYS.contains(&name) {
                return Err(error(line, format!("key '{name}' is reserved by the launcher")));
            }
            if !is_key_name(name) {
                return Err(error(line, format!("unknown key '{name}'")));
            }
        }
        other => return Err(error(line, format!("unknown source family '{other}'"))),
    }
    for option in options {
        validate_option(line, kind, analog, option)?;
    }
    Ok(())
}

fn validate_option(line: usize, kind: InputKind, analog: bool, option: &str) -> Result<(), ControlsError> {
    let (key, value) = option
        .split_once('=')
        .ok_or_else(|| error(line, format!("option '{option}' needs a value")))?;
    match key {
        "deadzone" | "scale" => {
            if !analog {
                return Err(error(line, format!("{key} applies to triggers and sticks only")));
            }
            let parsed: f32 = value.parse().map_err(|_| error(line, format!("{key} must be a number")))?;
            if key == "deadzone" && !(0.0..1.0).contains(&parsed) {
                return Err(error(line, "deadzone must be at least 0 and below 1"));
            }
            if key == "scale" && !(0.0..=2.0).contains(&parsed) {
                return Err(error(line, "scale must be between 0 and 2"));
            }
        }
        "curve" => {
            if !analog {
                return Err(error(line, "curve applies to triggers and sticks only"));
            }
            if !["linear", "quadratic", "cubic", "s_curve", "scurve", "s-curve", "smooth_power", "smooth", "sublinear", "root", "logarithmic", "log"].contains(&value) {
                return Err(error(line, "curve must be linear, quadratic, cubic, s_curve, smooth_power, sublinear or logarithmic"));
            }
        }
        // TASK-147: the pressure byte a trigger gives at full travel (the game defaults R2 to 45 and L2 to 84).
        "range" => {
            if !(analog && kind == InputKind::Button) {
                return Err(error(line, "range applies to triggers only"));
            }
            let parsed: u32 = value.parse().map_err(|_| error(line, "range must be a whole number from 1 to 255"))?;
            if !(1..=255).contains(&parsed) {
                return Err(error(line, "range must be a whole number from 1 to 255"));
            }
        }
        "invert" => {
            if kind != InputKind::Stick {
                return Err(error(line, "invert applies to sticks only"));
            }
            if !["true", "false"].contains(&value) {
                return Err(error(line, "invert must be true or false"));
            }
        }
        other => return Err(error(line, format!("unknown option '{other}'"))),
    }
    Ok(())
}

fn is_key_name(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    if lower.len() == 1 && lower.as_bytes()[0].is_ascii_alphanumeric() {
        return true;
    }
    if let Some(number) = lower.strip_prefix('f') {
        if let Ok(value) = number.parse::<u32>() {
            return (1..=12).contains(&value);
        }
    }
    if let Some(number) = lower.strip_prefix("num") {
        return number.len() == 1 && number.as_bytes()[0].is_ascii_digit();
    }
    matches!(
        lower.as_str(),
        "enter" | "backspace" | "tab" | "space" | "shift" | "ctrl" | "alt" | "esc" | "up" | "down" | "left"
            | "right" | "insert" | "delete" | "home" | "end" | "pageup" | "pagedown"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_render_and_parse_back_to_themselves() {
        let controls = Controls::default();
        assert_eq!(Controls::parse(&controls.render()).unwrap(), controls);
    }

    #[test]
    fn the_schema_line_is_required_and_must_be_one() {
        assert!(Controls::parse("[bindings]\ncross = []\n").is_err());
        assert!(Controls::parse("schema = 2\n").is_err());
    }

    #[test]
    fn refusals_name_the_line() {
        let error = Controls::parse("schema = 1\n[bindings]\ncross = [\"xinput:Q\"]\n").unwrap_err();
        assert_eq!(error.line, 3);
        assert!(error.message.contains("unknown XInput source 'Q'"));
    }

    #[test]
    fn sticks_and_buttons_accept_only_their_sources() {
        assert!(Controls::parse("schema = 1\n[bindings]\nrstick_x = [\"key:Z\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\ncross = [\"xinput:LX\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\ncross = [\"key:F9\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\nrstick_y = [\"xinput:RY | invert=true\"]\n").is_ok());
        assert!(Controls::parse("schema = 1\n[bindings]\ncross = [\"xinput:A | invert=true\"]\n").is_err());
    }

    #[test]
    fn options_are_range_checked() {
        assert!(Controls::parse("schema = 1\n[bindings]\nl2 = [\"xinput:LT | deadzone=1.5\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=cubic\"]\n").is_ok());
        assert!(Controls::parse("schema = 1\n[bindings]\nl2 = [\"xinput:LT | curve=sharp\"]\n").is_err());
        // TASK-147: the pressure byte at full travel, triggers only, 1..=255.
        assert!(Controls::parse("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=45\"]\n").is_ok());
        assert!(Controls::parse("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=0\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=256\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=4.5\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\nlstick_x = [\"xinput:LX | range=45\"]\n").is_err());
    }

    #[test]
    fn duplicates_and_unknown_tables_are_refused() {
        assert!(Controls::parse("schema = 1\n[bindings]\ncross = [\"key:Z\", \"key:Z\"]\n").is_err());
        assert!(Controls::parse("schema = 1\n[bindings]\ncross = []\ncross = []\n").is_err());
        assert!(Controls::parse("schema = 1\n[other]\n").is_err());
    }

    #[test]
    fn unlisted_inputs_keep_their_defaults_and_an_empty_list_unbinds() {
        let controls = Controls::parse("schema = 1\n[bindings]\ncross = []\n").unwrap();
        assert_eq!(controls.sources("cross"), Some(&[][..]));
        assert_eq!(
            controls.sources("circle").unwrap(),
            &["xinput:B".to_string(), "key:Space".to_string()][..]
        );
    }

    #[test]
    fn a_trigger_range_survives_a_tuning_change() {
        let mut controls = Controls::parse("schema = 1\n[bindings]\nr2 = [\"xinput:RT | range=90\"]\n").unwrap();
        controls.set_trigger_tuning("r2", TriggerTuning { deadzone: 0.05, curve: Curve::Linear }).unwrap();
        assert!(controls.render().contains("r2 = [\"xinput:RT | deadzone=0.050 curve=linear range=90\"]"));
    }

    #[test]
    fn trigger_tuning_round_trips_through_the_source_options() {
        let mut controls = Controls::default();
        controls
            .set_trigger_tuning("l2", TriggerTuning { deadzone: 0.05, curve: Curve::Quadratic })
            .unwrap();
        let tuning = controls.trigger_tuning("l2");
        assert!((tuning.deadzone - 0.05).abs() < 0.001);
        assert_eq!(tuning.curve, Curve::Quadratic);
        let text = controls.render();
        assert!(text.contains("l2 = [\"xinput:LT | deadzone=0.050 curve=quadratic\", \"key:F\"]"));
        assert!(Controls::parse(&text).is_ok());
    }

    #[test]
    fn stick_deadzones_default_to_the_xinput_constants_and_round_trip() {
        let mut controls = Controls::default();
        assert!((controls.stick_deadzone(Stick::Left) - 7849.0 / 32767.0).abs() < 1e-6);
        assert!((controls.stick_deadzone(Stick::Right) - 8689.0 / 32767.0).abs() < 1e-6);
        controls.set_stick_deadzone(Stick::Left, 0.1).unwrap();
        assert!((controls.stick_deadzone(Stick::Left) - 0.1).abs() < 1e-6);
        let text = controls.render();
        assert!(text.contains("lstick_y = [\"xinput:LY | invert=true deadzone=0.100\"]"), "{text}");
        assert!(text.contains("lstick_x = [\"xinput:LX | deadzone=0.100\"]"), "{text}");
        assert!(Controls::parse(&text).is_ok());
        controls.set_stick_deadzone(Stick::Right, 3.0).unwrap();
        assert!((controls.stick_deadzone(Stick::Right) - MAX_DEADZONE).abs() < 1e-6, "clamped below 1");
    }

    #[test]
    fn assign_replaces_only_the_same_family_and_swaps() {
        let mut controls = Controls::default();
        let result = controls.assign("cross", "xinput:RB", true).unwrap();
        assert_eq!(result.swapped_with.as_deref(), Some("r1"));
        assert_eq!(
            controls.sources("cross").unwrap(),
            &["xinput:RB".to_string(), "key:W".to_string(), "key:Enter".to_string()][..]
        );
        assert_eq!(controls.sources("r1").unwrap(), &["xinput:A".to_string(), "key:Shift".to_string()][..]);
    }

    #[test]
    fn assign_refuses_a_taken_source_without_swap_and_changes_nothing() {
        let mut controls = Controls::default();
        assert!(controls.assign("cross", "key:Q", false).is_err());
        assert_eq!(controls, Controls::default());
        let free = controls.assign("cross", "key:M", false).unwrap();
        assert_eq!(free.swapped_with, None);
        assert_eq!(controls.sources("cross").unwrap(), &["xinput:A".to_string(), "key:M".to_string()][..]);
    }

    #[test]
    fn assign_moves_a_source_when_the_input_had_none_of_that_family() {
        let mut controls = Controls::default();
        controls.set_sources("cross", vec!["xinput:A".into()]).unwrap();
        controls.assign("cross", "key:Q", true).unwrap();
        assert_eq!(controls.sources("l1").unwrap(), &["xinput:LB".to_string()][..], "L1 lost Q");
        assert_eq!(controls.sources("cross").unwrap(), &["xinput:A".to_string(), "key:Q".to_string()][..]);
    }

    #[test]
    fn assign_keeps_trigger_options_and_reports_no_change() {
        let mut controls = Controls::default();
        controls.set_trigger_tuning("l2", TriggerTuning { deadzone: 0.05, curve: Curve::Cubic }).unwrap();
        controls.assign("l2", "xinput:RT", true).unwrap();
        assert_eq!(controls.sources("l2").unwrap()[0], "xinput:RT | deadzone=0.050 curve=cubic");
        assert_eq!(controls.sources("r2").unwrap()[0], "xinput:LT | deadzone=0.050 curve=cubic");
        let again = controls.assign("l2", "xinput:RT", false).unwrap();
        assert!(again.unchanged);
        assert!(Controls::parse(&controls.render()).is_ok());
    }

    #[test]
    fn assign_refuses_reserved_keys_sticks_and_unknown_inputs() {
        let mut controls = Controls::default();
        assert!(controls.assign("cross", "key:F9", true).is_err());
        assert!(controls.assign("cross", "xinput:LX", true).is_err());
        assert!(controls.assign("lstick_x", "xinput:A", true).is_err());
        assert!(controls.assign("nope", "xinput:A", true).is_err());
        assert_eq!(controls, Controls::default());
    }

    #[test]
    fn new_curves_round_trip_and_unsupported_raw_modes_are_rejected() {
        for curve in [Curve::SCurve, Curve::SmoothPower, Curve::Sublinear, Curve::Logarithmic] {
            let mut controls = Controls::default();
            controls.set_trigger_tuning("r2", TriggerTuning { deadzone: 0.1, curve }).unwrap();
            let parsed = Controls::parse(&controls.render()).unwrap();
            assert_eq!(parsed.trigger_tuning("r2").curve, curve);
        }
        for (input, source) in [("l2", "xinput:LT"), ("lstick_x", "xinput:LX")] {
            let text = format!("schema = 1\n[bindings]\n{input} = [\"{source} | mode=dualsense\"]\n");
            assert!(Controls::parse(&text).unwrap_err().to_string().contains("unknown option 'mode'"));
        }
    }

    #[test]
    fn all_seven_curves_and_aliases_support_old_configs_and_monotonic_travel() {
        // Old configs without curve option default to Linear.
        let old_text = "schema = 1\n[bindings]\nl2 = [\"xinput:LT | deadzone=0.100\"]\n";
        let old_controls = Controls::parse(old_text).unwrap();
        assert_eq!(old_controls.trigger_tuning("l2").curve, Curve::Linear);

        // All 7 canonical curves and their accepted aliases.
        let cases = [
            ("linear", Curve::Linear),
            ("quadratic", Curve::Quadratic),
            ("cubic", Curve::Cubic),
            ("s_curve", Curve::SCurve),
            ("scurve", Curve::SCurve),
            ("s-curve", Curve::SCurve),
            ("smooth_power", Curve::SmoothPower),
            ("smooth", Curve::SmoothPower),
            ("sublinear", Curve::Sublinear),
            ("root", Curve::Sublinear),
            ("logarithmic", Curve::Logarithmic),
            ("log", Curve::Logarithmic),
        ];
        for (alias, expected_curve) in cases {
            let text = format!("schema = 1\n[bindings]\nr2 = [\"xinput:RT | curve={alias}\"]\n");
            let parsed = Controls::parse(&text).unwrap();
            assert_eq!(parsed.trigger_tuning("r2").curve, expected_curve);
        }
    }

    #[test]
    fn reset_all_and_is_default() {
        let mut controls = Controls::default();
        controls.assign("cross", "xinput:B", true).unwrap();
        assert!(!controls.is_default("cross") && !controls.is_default("circle"));
        assert!(controls.is_default("square"));
        controls.reset_all();
        assert_eq!(controls, Controls::default());
    }

    #[test]
    fn default_trigger_tuning_matches_the_game() {
        let tuning = Controls::default().trigger_tuning("r2");
        assert!((tuning.deadzone - 30.0 / 255.0).abs() < 1e-6);
        assert_eq!(tuning.curve, Curve::Linear);
    }
}
