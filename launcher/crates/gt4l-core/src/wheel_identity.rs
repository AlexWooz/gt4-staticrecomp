//! Which wheel is connected, and the mapping to start from (TASK-181). Pure: the launcher's SDL3 service passes the
//! device's SDL GUID, name and SDL type; nothing here opens a device.
//!
//! The order of the evidence: the USB vendor and product ID (from the SDL GUID, bytes 4-5 and 8-9 little-endian, as
//! cbfd8b8's `parse_guid_vid_pid` by JulioHeroZ did), then the device name, then SDL's wheel type. The table is
//! `docs/research/WHEEL-AUTOMAP-TABLE.md` (2026-11-05): only its Confirmed IDs are listed, MOZA is 0x346E (never the
//! generic 0x1209), and the templates are per family. A template pre-fills only what that table marks Confirmed or
//! High: steering on axis 0, the D-pad on hat 0, and the PlayStation button indices of the G923 (and the G29, the same
//! labels). Everything else (pedals, paddles, face buttons) is asked by the launcher's set-up wizard.

use serde::Serialize;

use crate::wheel::WheelConfig;

/// The USB vendor and product ID inside an SDL GUID: bytes 4-5 and 8-9, little-endian (hex digits 8..12 and 16..20).
pub fn vid_pid(guid: &str) -> Option<(u16, u16)> {
    if !crate::wheel::is_guid(guid) {
        return None;
    }
    let byte = |at: usize| u8::from_str_radix(&guid[at..at + 2], 16).ok();
    let vendor = u16::from(byte(8)?) | (u16::from(byte(10)?) << 8);
    let product = u16::from(byte(16)?) | (u16::from(byte(18)?) << 8);
    Some((vendor, product))
}

/// A family of wheels with one template and one vendor program.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct Family {
    /// `logitech_legacy`, `logitech_ps`, `logitech_xbox`, `logitech_pro`, `thrustmaster`, `fanatec`, `moza`, `simagic`,
    /// `turtle_beach`, `pxn`, `cammus`, `hori`, `generic`.
    pub id: &'static str,
    /// The family as the page names it (product names, not translated).
    pub name: &'static str,
    /// The vendor program where the rotation range and the force are set (a product name); empty: the wheel's own.
    pub software: &'static str,
    /// A direct-drive base: the force template starts lower.
    pub direct_drive: bool,
    /// The family has force feedback at all (HORI wheels have none).
    pub force_feedback: bool,
}

pub const FAMILIES: [Family; 13] = [
    Family { id: "logitech_legacy", name: "Logitech Driving Force / G25 / G27", software: "Logitech G HUB or Logitech Profiler", direct_drive: false, force_feedback: true },
    Family { id: "logitech_ps", name: "Logitech G29 / G923 (PlayStation)", software: "Logitech G HUB", direct_drive: false, force_feedback: true },
    Family { id: "logitech_xbox", name: "Logitech G920 / G923 (Xbox)", software: "Logitech G HUB", direct_drive: false, force_feedback: true },
    Family { id: "logitech_pro", name: "Logitech G PRO", software: "Logitech G HUB", direct_drive: true, force_feedback: true },
    Family { id: "thrustmaster", name: "Thrustmaster", software: "Thrustmaster Control Panel", direct_drive: false, force_feedback: true },
    Family { id: "fanatec", name: "Fanatec", software: "Fanatec Control Panel (FanaLab)", direct_drive: true, force_feedback: true },
    Family { id: "moza", name: "MOZA", software: "MOZA Pit House", direct_drive: true, force_feedback: true },
    Family { id: "simagic", name: "Simagic", software: "Simagic SimPro Manager", direct_drive: true, force_feedback: true },
    Family { id: "turtle_beach", name: "Turtle Beach VelocityOne", software: "Turtle Beach Control Center", direct_drive: true, force_feedback: true },
    Family { id: "pxn", name: "PXN", software: "PXN app", direct_drive: false, force_feedback: true },
    Family { id: "cammus", name: "Cammus", software: "Cammus app", direct_drive: true, force_feedback: true },
    Family { id: "hori", name: "HORI", software: "", direct_drive: false, force_feedback: false },
    Family { id: "generic", name: "Racing wheel", software: "", direct_drive: false, force_feedback: true },
];

pub fn family(id: &str) -> Option<&'static Family> {
    FAMILIES.iter().find(|family| family.id == id)
}

/// A mapping the launcher ships for a wheel it knows by its USB IDs (TASK-189, owner 2026-11-06). The mechanism is
/// generic: another wheel gets an entry here and a file under `assets/wheel-presets`, and its preset is applied by itself
/// when the wheel is detected and the player has no mapping of their own.
#[derive(Clone, Copy, Debug)]
pub struct BuiltinPreset {
    pub vendor: u16,
    pub product: u16,
    /// The name the page shows and `wheel.toml` keeps in its `preset` field.
    pub name: &'static str,
    /// The mapping, in the `wheel.toml` format (`include_str!` of the shipped file).
    pub text: &'static str,
}

/// The presets the launcher ships. Only IDs whose mapping was mapped and tested on the real wheel belong here.
pub const BUILTIN_PRESETS: [BuiltinPreset; 1] = [BuiltinPreset {
    vendor: 0x046D,
    product: 0xC29B,
    name: "Logitech G27",
    text: include_str!("../assets/wheel-presets/logitech-g27.toml"),
}];

/// The shipped preset of the wheel behind `guid`, when the launcher has one.
pub fn builtin_preset(guid: &str) -> Option<&'static BuiltinPreset> {
    let (vendor, product) = vid_pid(guid)?;
    BUILTIN_PRESETS.iter().find(|preset| preset.vendor == vendor && preset.product == product)
}

/// The shipped preset with that name (as `wheel.toml` keeps it in `preset`).
pub fn builtin_preset_by_name(name: &str) -> Option<&'static BuiltinPreset> {
    BUILTIN_PRESETS.iter().find(|preset| preset.name == name)
}

/// The mapping of a shipped preset, ready to use: read with the game's own parser, named, and pointing at `guid` when it
/// is known. `None` when the shipped file does not parse (a build error, never patched over at run time).
pub fn builtin_preset_config(preset: &BuiltinPreset, guid: &str) -> Option<WheelConfig> {
    let mut config = WheelConfig::from_toml(preset.text).ok()?;
    config.preset = preset.name.to_string();
    config.enabled = false;
    if crate::wheel::is_guid(guid) {
        config.device_guid = guid.to_ascii_lowercase();
    }
    Some(config)
}

/// A plain hint about the wheel's mode, shown on the page (the page has the texts, keyed by these ids).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Note {
    /// A Logitech wheel in a compatibility PID (C294 Driving Force/EX, C298 Driving Force Pro): a G25, G27 or Driving
    /// Force GT there has fewer buttons and no clutch. Open Logitech G HUB or Profiler and set native mode.
    LogitechCompatibility,
    /// A Thrustmaster base in its initial PID (TX B664, TS-XW B691): the Thrustmaster drivers switch it to its normal mode.
    ThrustmasterInitial,
    /// A G920 in its initial PID (C261): Logitech G HUB switches it to its normal mode.
    LogitechG920Initial,
    /// A Fanatec CSL Elite base in PS4 console mode (0005): put the base in PC mode.
    FanatecConsoleMode,
    /// The family has no force feedback.
    NoForceFeedback,
}

struct Model {
    vendor: u16,
    product: u16,
    family: &'static str,
    model: &'static str,
    note: Option<Note>,
}

const fn model(vendor: u16, product: u16, family: &'static str, model: &'static str) -> Model {
    Model { vendor, product, family, model, note: None }
}

const fn noted(vendor: u16, product: u16, family: &'static str, model: &'static str, note: Note) -> Model {
    Model { vendor, product, family, model, note: Some(note) }
}

/// The Confirmed IDs of the research table (SDL's wheel list and the Linux compatibility tables).
const MODELS: &[Model] = &[
    noted(0x046D, 0xC294, "logitech_legacy", "Driving Force / Formula Force EX (or a wheel in compatibility mode)", Note::LogitechCompatibility),
    model(0x046D, 0xC295, "logitech_legacy", "MOMO Force"),
    noted(0x046D, 0xC298, "logitech_legacy", "Driving Force Pro (or a wheel in compatibility mode)", Note::LogitechCompatibility),
    model(0x046D, 0xC299, "logitech_legacy", "G25"),
    model(0x046D, 0xC29A, "logitech_legacy", "Driving Force GT"),
    model(0x046D, 0xC29B, "logitech_legacy", "G27"),
    model(0x046D, 0xC24F, "logitech_ps", "G29 (PS3 mode)"),
    model(0x046D, 0xC260, "logitech_ps", "G29 (PS4 mode)"),
    model(0x046D, 0xC266, "logitech_ps", "G923 (PC mode)"),
    model(0x046D, 0xC267, "logitech_ps", "G923 (PS4 mode)"),
    noted(0x046D, 0xC261, "logitech_xbox", "G920 (initial mode)", Note::LogitechG920Initial),
    model(0x046D, 0xC262, "logitech_xbox", "G920"),
    model(0x046D, 0xC26D, "logitech_xbox", "G923 (Xbox)"),
    model(0x046D, 0xC26E, "logitech_xbox", "G923 (Xbox)"),
    model(0x046D, 0xC268, "logitech_pro", "G PRO (PC mode)"),
    model(0x046D, 0xC269, "logitech_pro", "G PRO (PlayStation mode)"),
    model(0x046D, 0xC272, "logitech_pro", "G PRO (Xbox, PC mode)"),
    model(0x044F, 0xB677, "thrustmaster", "T150"),
    model(0x044F, 0xB67F, "thrustmaster", "TMX"),
    model(0x044F, 0xB696, "thrustmaster", "T248"),
    model(0x044F, 0xB66E, "thrustmaster", "T300RS / T-GT II"),
    model(0x044F, 0xB66F, "thrustmaster", "T300RS (advanced mode)"),
    model(0x044F, 0xB66D, "thrustmaster", "T300RS (PS4 mode)"),
    noted(0x044F, 0xB664, "thrustmaster", "TX (initial mode)", Note::ThrustmasterInitial),
    model(0x044F, 0xB669, "thrustmaster", "TX"),
    noted(0x044F, 0xB691, "thrustmaster", "TS-XW (initial mode)", Note::ThrustmasterInitial),
    model(0x044F, 0xB692, "thrustmaster", "TS-XW"),
    model(0x0EB7, 0x0001, "fanatec", "ClubSport V2"),
    model(0x0EB7, 0x0004, "fanatec", "ClubSport V2.5"),
    model(0x0EB7, 0x0E03, "fanatec", "CSL Elite"),
    noted(0x0EB7, 0x0005, "fanatec", "CSL Elite (console mode)", Note::FanatecConsoleMode),
    model(0x0EB7, 0x0006, "fanatec", "Podium DD1"),
    model(0x0EB7, 0x0007, "fanatec", "Podium DD2"),
    model(0x0EB7, 0x0011, "fanatec", "CSR Elite"),
    model(0x0EB7, 0x0020, "fanatec", "CSL DD / GT DD Pro / ClubSport DD"),
    model(0x346E, 0x0000, "moza", "R16 / R21"),
    model(0x346E, 0x0002, "moza", "R9"),
    model(0x346E, 0x0004, "moza", "R5"),
    model(0x346E, 0x0005, "moza", "R3"),
    model(0x346E, 0x0006, "moza", "R12"),
    model(0x0483, 0x0522, "simagic", "Alpha / Alpha Mini / Alpha Ultimate / M10"),
    model(0x3670, 0x0500, "simagic", "EVO Sport"),
    model(0x3670, 0x0501, "simagic", "EVO"),
    model(0x3670, 0x0502, "simagic", "EVO Pro"),
    model(0x10F5, 0x7077, "turtle_beach", "VelocityOne Race"),
    model(0x11FF, 0x3245, "pxn", "V10"),
    model(0x11FF, 0x1212, "pxn", "V12"),
    model(0x11FF, 0x1112, "pxn", "V12 Lite"),
    model(0x11FF, 0x1211, "pxn", "V12 Lite"),
    model(0x36E6, 0x400F, "pxn", "VD6"),
    model(0x3416, 0x0301, "cammus", "C5"),
    model(0x3416, 0x0302, "cammus", "C12"),
];

/// True for a VID:PID of the table (the game's `is_known_wheel_vid_pid` lists the same IDs).
pub fn is_known_wheel(vendor: u16, product: u16) -> bool {
    MODELS.iter().any(|model| model.vendor == vendor && model.product == product)
}

/// Auxiliary devices (pedals, shifters, handbrakes) are never the wheel, whatever else they say.
fn is_auxiliary_name(lower: &str) -> bool {
    ["pedal", "shifter", "shift", "handbrake"].iter().any(|word| lower.contains(word))
}

/// The game's rule for "a wheel" (`is_candidate_wheel_device`): not an auxiliary by name, and SDL's wheel type, a known
/// VID:PID or a wheel name. Without a configured GUID the game drives the first such device.
pub fn is_candidate(sdl_wheel: bool, name: &str, guid: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    if is_auxiliary_name(&lower) {
        return false;
    }
    if sdl_wheel {
        return true;
    }
    if let Some((vendor, product)) = vid_pid(guid) {
        if is_known_wheel(vendor, product) {
            return true;
        }
    }
    const WHEEL_NAMES: [&str; 25] = [
        "wheel", "driving force", "g29", "g920", "g923", "g27", "g25", "t150", "t248", "t300", "t500", "t-gt", "t818",
        "tmx", "apex", "overdrive", "racing base", "wheelbase", "moza", "fanatec", "clubsport", "podium", "simagic",
        "cammus", "velocityone race",
    ];
    WHEEL_NAMES.iter().any(|word| lower.contains(word))
}

/// What the launcher knows about one device.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Identity {
    /// `vid_pid`, `name`, `sdl_type` or `none`: what told the family.
    pub matched_by: &'static str,
    pub family: Option<&'static Family>,
    /// The model of the ID table, when the VID:PID is in it.
    pub model: Option<&'static str>,
    pub notes: Vec<Note>,
}

/// Name patterns for a VID:PID the table does not have (another firmware, a renamed device).
fn family_by_name(lower: &str) -> Option<&'static str> {
    let has = |words: &[&str]| words.iter().any(|word| lower.contains(word));
    if has(&["g923", "g29 "]) || lower.ends_with("g29") || lower.contains("g29 driving force") {
        return Some(if has(&["xbox"]) { "logitech_xbox" } else { "logitech_ps" });
    }
    if has(&["g920"]) {
        return Some("logitech_xbox");
    }
    if has(&["g pro racing", "logitech g pro"]) {
        return Some("logitech_pro");
    }
    if has(&["g27", "g25", "driving force", "dfgt", "momo"]) {
        return Some("logitech_legacy");
    }
    if has(&["thrustmaster", "t150", "t248", "t300", "t500", "tmx", "t-gt", "t818", "ts-xw", "ts-pc"]) {
        return Some("thrustmaster");
    }
    if has(&["fanatec", "csl dd", "csl elite", "clubsport", "podium", "gt dd"]) {
        return Some("fanatec");
    }
    if has(&["moza"]) {
        return Some("moza");
    }
    if has(&["simagic"]) {
        return Some("simagic");
    }
    if has(&["velocityone", "velocity one", "turtle beach"]) {
        return Some("turtle_beach");
    }
    if has(&["pxn"]) {
        return Some("pxn");
    }
    if has(&["cammus"]) {
        return Some("cammus");
    }
    if has(&["hori", "apex", "overdrive"]) {
        return Some("hori");
    }
    None
}

/// Identifies a device: VID:PID first, then the name, then SDL's wheel type.
pub fn identify(guid: &str, name: &str, sdl_wheel: bool) -> Identity {
    let lower = name.to_ascii_lowercase();
    if let Some((vendor, product)) = vid_pid(guid) {
        if let Some(found) = MODELS.iter().find(|model| model.vendor == vendor && model.product == product) {
            let family = family(found.family);
            let mut notes: Vec<Note> = found.note.into_iter().collect();
            if family.is_some_and(|family| !family.force_feedback) {
                notes.push(Note::NoForceFeedback);
            }
            return Identity { matched_by: "vid_pid", family, model: Some(found.model), notes };
        }
        // HORI has no product IDs in the table: its vendor ID and a wheel name or type are enough.
        if vendor == 0x0F0D && (sdl_wheel || lower.contains("wheel") || lower.contains("apex") || lower.contains("overdrive")) {
            return Identity { matched_by: "vid_pid", family: family("hori"), model: None, notes: vec![Note::NoForceFeedback] };
        }
    }
    if !is_auxiliary_name(&lower) {
        if let Some(id) = family_by_name(&lower) {
            let family = family(id);
            let notes = if family.is_some_and(|family| !family.force_feedback) { vec![Note::NoForceFeedback] } else { Vec::new() };
            return Identity { matched_by: "name", family, model: None, notes };
        }
        if sdl_wheel {
            return Identity { matched_by: "sdl_type", family: family("generic"), model: None, notes: Vec::new() };
        }
    }
    Identity { matched_by: "none", family: None, model: None, notes: Vec::new() }
}

/// The mapping a family starts from. Steering on axis 0 and the D-pad on hat 0 for every family (High); the
/// PlayStation button indices for the G29/G923 family (Confirmed for the G923 by S8 of the research table). Pedals and
/// the other buttons are left empty for the wizard. A direct-drive base starts with less force; HORI with none.
///
/// The `generic` family is a wheel the launcher does not know: it starts unassigned (TASK-189, owner 2026-11-06), so the
/// page never guesses which button or axis is which. The player maps it with the set-up wizard.
pub fn template(family_id: &str) -> Option<WheelConfig> {
    let family = family(family_id)?;
    if family.id == "generic" {
        return Some(WheelConfig::unassigned());
    }
    let mut config = WheelConfig::default();
    config.preset = family.id.to_string();
    config.steering.source = "axis:0".into();
    config.steering.rotation_degrees = 900;
    config.steering.invert = false;
    for pedal in [&mut config.throttle, &mut config.brake, &mut config.clutch, &mut config.handbrake] {
        pedal.source.clear();
        pedal.mode = "inverted".into();
        pedal.curve = "linear".into();
    }
    for sources in config.buttons.values_mut() {
        sources.clear();
    }
    for (slot, direction) in [("dpad_up", "up"), ("dpad_right", "right"), ("dpad_down", "down"), ("dpad_left", "left")] {
        config.buttons.insert(slot.into(), vec![format!("hat:0:{direction}")]);
    }
    if family.id == "logitech_ps" {
        // G923 PlayStation layout (S8): 0 Cross, 1 Square, 2 Circle, 3 Triangle, 4 right paddle, 5 left paddle, 6 R2,
        // 7 L2, 8 Share, 9 Options, 10 R3, 11 L3. The paddles shift (DFP R1/L1); the wheel's R2/L2 are the DFP's.
        for (slot, button) in [
            ("cross", 0),
            ("square", 1),
            ("circle", 2),
            ("triangle", 3),
            ("r2", 4),
            ("l2", 5),
            ("r1", 6),
            ("l1", 7),
            ("select", 8),
            ("start", 9),
            ("r3", 10),
            ("l3", 11),
        ] {
            config.buttons.insert(slot.into(), vec![format!("btn:{button}")]);
        }
    }
    config.ffb.enabled = family.force_feedback;
    config.ffb.strength = if family.direct_drive { 0.6 } else { 1.0 };
    config.ffb.damper = 0.15;
    config.ffb.invert = false;
    Some(config)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The SDL GUID a G27 gets on Windows (bus 0x0003, VID 046D, PID C29B).
    const G27: &str = "030000006d0400009bc2000000000000";

    fn guid(vendor: u16, product: u16) -> String {
        format!(
            "03000000{:02x}{:02x}0000{:02x}{:02x}000000000000",
            vendor & 0xFF,
            vendor >> 8,
            product & 0xFF,
            product >> 8
        )
    }

    #[test]
    fn the_vid_and_pid_come_from_bytes_4_5_and_8_9_little_endian() {
        assert_eq!(vid_pid(G27), Some((0x046D, 0xC29B)));
        assert_eq!(vid_pid(&G27.to_uppercase()), Some((0x046D, 0xC29B)));
        assert_eq!(guid(0x346E, 0x0004), "030000006e3400000400000000000000");
        for bad in ["", "1234", "zz0000006d0400009bc2000000000000", "030000006d0400009bc200000000000"] {
            assert_eq!(vid_pid(bad), None, "{bad}");
        }
    }

    #[test]
    fn every_family_of_the_research_table_is_found_by_its_ids() {
        for (vendor, product, family_id, model) in [
            (0x046D, 0xC29B, "logitech_legacy", "G27"),
            (0x046D, 0xC299, "logitech_legacy", "G25"),
            (0x046D, 0xC24F, "logitech_ps", "G29 (PS3 mode)"),
            (0x046D, 0xC266, "logitech_ps", "G923 (PC mode)"),
            (0x046D, 0xC262, "logitech_xbox", "G920"),
            (0x044F, 0xB66E, "thrustmaster", "T300RS / T-GT II"),
            (0x0EB7, 0x0020, "fanatec", "CSL DD / GT DD Pro / ClubSport DD"),
            (0x346E, 0x0004, "moza", "R5"),
            (0x0483, 0x0522, "simagic", "Alpha / Alpha Mini / Alpha Ultimate / M10"),
            (0x3670, 0x0501, "simagic", "EVO"),
            (0x10F5, 0x7077, "turtle_beach", "VelocityOne Race"),
            (0x11FF, 0x3245, "pxn", "V10"),
            (0x36E6, 0x400F, "pxn", "VD6"),
            (0x3416, 0x0302, "cammus", "C12"),
        ] {
            let identity = identify(&guid(vendor, product), "USB device", false);
            assert_eq!(identity.matched_by, "vid_pid");
            assert_eq!(identity.family.map(|family| family.id), Some(family_id), "{vendor:04x}:{product:04x}");
            assert_eq!(identity.model, Some(model));
            assert!(is_candidate(false, "USB device", &guid(vendor, product)));
        }
    }

    #[test]
    fn moza_is_0x346e_and_never_the_generic_0x1209() {
        assert!(identify(&guid(0x1209, 0x0004), "OpenFFBoard", false).family.is_none());
        assert!(!is_candidate(false, "OpenFFBoard", &guid(0x1209, 0x0004)));
        assert_eq!(identify(&guid(0x346E, 0x0000), "Gudsen MOZA R16", false).model, Some("R16 / R21"));
    }

    #[test]
    fn compatibility_and_initial_modes_get_their_hint() {
        assert_eq!(identify(&guid(0x046D, 0xC294), "Logitech Driving Force EX", true).notes, vec![Note::LogitechCompatibility]);
        assert_eq!(identify(&guid(0x046D, 0xC298), "Logitech Driving Force Pro", true).notes, vec![Note::LogitechCompatibility]);
        assert_eq!(identify(&guid(0x044F, 0xB664), "Thrustmaster TX", true).notes, vec![Note::ThrustmasterInitial]);
        assert_eq!(identify(&guid(0x044F, 0xB691), "Thrustmaster TS-XW", true).notes, vec![Note::ThrustmasterInitial]);
        assert_eq!(identify(&guid(0x046D, 0xC261), "G920", true).notes, vec![Note::LogitechG920Initial]);
        assert_eq!(identify(&guid(0x0EB7, 0x0005), "FANATEC CSL Elite", true).notes, vec![Note::FanatecConsoleMode]);
        assert!(identify(G27, "G27 Racing Wheel", true).notes.is_empty());
    }

    #[test]
    fn an_unknown_id_falls_back_to_the_name_then_to_the_sdl_type() {
        let unknown = guid(0x1234, 0x5678);
        assert_eq!(identify(&unknown, "Logitech G27 Racing Wheel", false).family.unwrap().id, "logitech_legacy");
        assert_eq!(identify(&unknown, "Thrustmaster T818", false).family.unwrap().id, "thrustmaster");
        assert_eq!(identify(&unknown, "Logitech G923 Racing Wheel for Xbox", false).family.unwrap().id, "logitech_xbox");
        let hori = identify(&guid(0x0F0D, 0x0123), "HORI Racing Wheel Apex", false);
        assert_eq!((hori.family.unwrap().id, hori.matched_by), ("hori", "vid_pid"));
        assert_eq!(hori.notes, vec![Note::NoForceFeedback]);
        let typed = identify(&unknown, "Some Wheel Base", true);
        assert_eq!((typed.matched_by, typed.family.unwrap().id), ("sdl_type", "generic"));
        let nothing = identify(&unknown, "USB Gamepad", false);
        assert_eq!((nothing.matched_by, nothing.family), ("none", None));
        assert_eq!(identify(&unknown, "Thrustmaster T3PA Pedals", true).family, None, "pedals are never the wheel");
    }

    #[test]
    fn auxiliary_devices_and_gamepads_are_not_wheels_but_known_wheels_are_without_a_name() {
        assert!(!is_candidate(true, "Fanatec ClubSport Pedals V3", &guid(0x0EB7, 0x183B)));
        assert!(!is_candidate(false, "VelocityOne Multi-Shift", &guid(0x10F5, 0x7008)));
        assert!(!is_candidate(false, "Xbox Wireless Controller", &guid(0x045E, 0x0B13)));
        assert!(is_candidate(false, "", G27));
        assert!(is_candidate(false, "HORI Racing Wheel Apex", &guid(0x0F0D, 0x0001)));
    }

    #[test]
    fn templates_pre_fill_only_confirmed_indices_and_are_valid() {
        for family in FAMILIES {
            let config = template(family.id).unwrap();
            assert!(config.validate().is_ok(), "{}", family.id);
            if family.id == "generic" {
                assert!(config.is_unassigned(), "an unknown wheel starts unassigned (TASK-189)");
                assert_eq!(config.preset, "generic");
                continue;
            }
            assert_eq!(config.steering.source, "axis:0");
            assert_eq!(config.buttons["dpad_up"], vec!["hat:0:up"]);
            assert!(config.throttle.source.is_empty() && config.brake.source.is_empty(), "pedals are captured");
            assert_eq!(config.preset, family.id);
        }
        let legacy = template("logitech_legacy").unwrap();
        assert!(legacy.buttons["cross"].is_empty(), "the G27's face buttons are asked by the wizard");
        let ps = template("logitech_ps").unwrap();
        assert_eq!(ps.buttons["cross"], vec!["btn:0"]);
        assert_eq!(ps.buttons["r2"], vec!["btn:4"], "the right paddle shifts up (DFP R1)");
        assert_eq!(ps.buttons["r1"], vec!["btn:6"], "the wheel's R2 is the DFP's R2");
        assert_eq!(ps.buttons["l3"], vec!["btn:11"]);
        assert!(template("moza").unwrap().ffb.strength < 1.0, "direct drive starts lower");
        assert!(!template("hori").unwrap().ffb.enabled);
        assert!(template("nothing").is_none());
    }

    /// TASK-189: the launcher ships the owner's own G27 mapping, found by its USB IDs and valid for the game.
    #[test]
    fn the_g27_ships_a_preset_the_launcher_applies_by_itself() {
        let preset = builtin_preset(G27).expect("the G27 has a preset");
        assert_eq!((preset.vendor, preset.product, preset.name), (0x046D, 0xC29B, "Logitech G27"));
        assert!(builtin_preset("030000006d0400009bc20000ffffffff").is_some(), "the USB IDs decide, not the instance bytes");
        let other = guid(0x046D, 0xC29A);
        assert!(builtin_preset(&other).is_none(), "a Driving Force GT has no preset yet");
        assert!(builtin_preset("nonsense").is_none());

        let config = builtin_preset_config(preset, G27).expect("the shipped file parses");
        assert!(config.validate().is_ok(), "the game accepts the preset");
        assert!(!config.is_unassigned());
        assert_eq!(config.preset, "Logitech G27");
        assert_eq!(config.device_guid, G27);
        assert!(!config.enabled, "the preset never decides the wheel is on: Play does");
        assert_eq!(config.steering.source, "axis:0");
        assert_eq!((config.throttle.source.as_str(), config.brake.source.as_str()), ("axis:1", "axis:2"));
        assert_eq!(config.clutch.source, "axis:4");
        assert_eq!(config.buttons["cross"], vec!["btn:17"]);
        assert_eq!(config.buttons["square"], vec!["btn:16"]);
        assert_eq!(config.buttons["triangle"], vec!["btn:15"]);
        assert_eq!(config.buttons["circle"], vec!["btn:18"]);
        assert_eq!(config.buttons["r2"], vec!["btn:4"], "gear up: the right paddle");
        assert_eq!(config.buttons["l2"], vec!["btn:5"], "gear down: the left paddle");
        assert_eq!(config.buttons["r1"], vec!["btn:19"]);
        assert_eq!(config.buttons["l1"], vec!["btn:20"]);
        assert_eq!(config.buttons["start"], vec!["btn:6"]);
        assert_eq!(config.buttons["select"], vec!["btn:7"]);
        assert_eq!(config.buttons["r3"], vec!["btn:21"]);
        assert_eq!(config.buttons["l3"], vec!["btn:22"]);
        assert_eq!(config.buttons["dpad_left"], vec!["hat:0:left"]);
        assert_eq!((config.ffb.strength, config.ffb.damper), (1.0, 0.15));
        assert_eq!(builtin_preset_by_name("Logitech G27").map(|one| one.product), Some(0xC29B));
        assert!(builtin_preset_by_name("G27").is_none());
    }
}
