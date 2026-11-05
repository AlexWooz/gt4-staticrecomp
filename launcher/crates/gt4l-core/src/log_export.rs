//! The log bundle for a bug report (TASK-111, release 1.3.0; owner decision 2026-10-23, moved into rc4).
//!
//! One zip, `GT4Recomp-logs-YYYYMMDD-HHMMSS.zip`, written to a folder the player picks. It holds only text:
//! - `latest.log` (see [`crate::logs`]);
//! - the newest [`KEPT_FOR_EXPORT`] launcher session logs;
//! - `logs/shaders.log`, the last shader prepare step with a header per attempt (TASK-170: it names where a compile stopped);
//! - `logs/shader-guard.txt`, a readable listing of the shader cache's `quarantine.bin` and `compiling.bin` (TASK-170b:
//!   header fields and each key as hex words; no other file of the cache folder);
//! - the newest [`KEPT_FOR_EXPORT`] game session folders' `game.log` and `gt4-gs.ini`;
//! - the settings file;
//! - `system.txt` (see [`system_text`]).
//!
//! Nothing else is ever put in: no card, save, replay, disc image, prepared game file or program file ([`is_forbidden`]
//! is the rule the tests check). The player's Windows user name and the profile folder paths are masked in every text
//! ([`mask_text`]). The launcher (`gt4l-tauri`) reads the files and calls these functions.

use std::fs::File;
use std::io::{self, Write};
use std::path::Path;
use std::time::SystemTime;

use crate::logs::LocalTime;
use crate::system_check::{decode_driver_version, windows_driver_text, DriverFacts};

/// The newest session logs and game sessions the bundle holds.
pub const KEPT_FOR_EXPORT: usize = 3;

/// The file name of a bundle: `GT4Recomp-logs-20261023-210340.zip`.
pub fn zip_name(time: &LocalTime) -> String {
    format!("GT4Recomp-logs-{}.zip", time.file_stamp())
}

/// True for a file or folder that must never go into a bundle: cards, saves, replays, disc images, prepared game files
/// and program files.
pub fn is_forbidden(path: &str) -> bool {
    let lower = path.to_ascii_lowercase().replace('\\', "/");
    let name = lower.rsplit('/').next().unwrap_or("");
    let trimmed = name.strip_prefix('.').unwrap_or(name);
    if trimmed == "retroachievements.toml" || trimmed.starts_with("retroachievements.toml.") {
        return true;
    }
    const EXTENSIONS: &[&str] = &[
        ".ps2", ".mcd", ".mc2", ".psu", ".bin", ".iso", ".img", ".elf", ".exe", ".dll", ".gt4", ".zip", ".7z", ".sav",
    ];
    if EXTENSIONS.iter().any(|extension| name.ends_with(extension)) {
        return true;
    }
    if name.starts_with("card") {
        return true;
    }
    // Game data and replays are named by the game's serial (`BASCUS-97328GAMEDATA`, `BASCUS-97328R0001`).
    lower.split('/').any(|part| {
        matches!(part, "saves" | "replays" | "backups" | "disc" | "prepared" | "prepared.tmp" | "runtime" | "memcard")
            || part.starts_with("save ")
            || part.contains("gamedata")
            || part.starts_with("bascus")
            || part.starts_with("scus")
            || part.starts_with("slus")
            || part.starts_with("sces")
            || part.starts_with("slps")
    })
}

/// The `count` newest of `found` (`(last change, item)`), newest first.
pub fn newest<T>(mut found: Vec<(SystemTime, T)>, count: usize) -> Vec<T> {
    found.sort_by(|a, b| b.0.cmp(&a.0));
    found.into_iter().take(count).map(|(_, item)| item).collect()
}

/// Replaces the Windows user name and the user profile paths in `text`: `C:\Users\<name>` (and `C:/Users/<name>`) becomes
/// `C:\Users\<user>`; any other occurrence of the name (ASCII case-insensitive) becomes `<user>`. A name shorter than three
/// characters is left alone, so an ordinary word is never changed.
pub fn mask_text(text: &str, user_name: &str) -> String {
    let user = user_name.trim();
    if user.len() < 3 {
        return text.to_string();
    }
    // The profile folder name is joined at run time: the release build refuses an executable that holds the literal
    // `\Users\` (launcher/tools/build-release.ps1 scans it for build paths).
    let mut profile = text.to_string();
    for separator in ["\\", "/"] {
        let users = ["C:", "Users"].join(separator);
        let found = format!("{users}{separator}{user}");
        let masked = format!("{users}{separator}<user>");
        profile = replace_ascii_insensitive(&profile, &found, &masked);
    }
    replace_ascii_insensitive(&profile, user, "<user>")
}

/// `text` with every ASCII case-insensitive match of `needle` replaced by `replacement`.
fn replace_ascii_insensitive(text: &str, needle: &str, replacement: &str) -> String {
    let lower_text = text.to_ascii_lowercase();
    let lower_needle = needle.to_ascii_lowercase();
    let mut out = String::with_capacity(text.len());
    let mut start = 0;
    while let Some(found) = lower_text[start..].find(&lower_needle) {
        let at = start + found;
        out.push_str(&text[start..at]);
        out.push_str(replacement);
        start = at + needle.len();
    }
    out.push_str(&text[start..]);
    out
}

/// One file of the bundle: its path inside the zip and its text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub path: String,
    pub text: String,
}

/// The entries with the user name masked in every text.
pub fn masked(entries: &[Entry], user_name: &str) -> Vec<Entry> {
    entries.iter().map(|entry| Entry { path: entry.path.clone(), text: mask_text(&entry.text, user_name) }).collect()
}

/// Writes the zip. Entries that are [`is_forbidden`] are refused with an error, not written.
pub fn write_zip(path: &Path, entries: &[Entry]) -> io::Result<()> {
    if let Some(bad) = entries.iter().find(|entry| is_forbidden(&entry.path)) {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("{} may not go into a log bundle", bad.path)));
    }
    let file = File::create(path)?;
    let mut writer = zip::ZipWriter::new(file);
    let options = zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
    for entry in entries {
        writer.start_file(entry.path.as_str(), options).map_err(|error| io::Error::other(error.to_string()))?;
        writer.write_all(entry.text.as_bytes())?;
    }
    writer.finish().map_err(|error| io::Error::other(error.to_string()))?;
    Ok(())
}

/// One graphics card as the system report lists it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GpuFact {
    pub name: String,
    /// The PCI vendor id (0 when unknown); the report shows the vendor's name.
    pub vendor_id: u32,
    /// The Vulkan driver version, raw.
    pub driver_version: u32,
    /// Dedicated video memory in bytes (DXGI).
    pub video_memory: u64,
    /// The queue count of the graphics family the game would use (`None` when unknown).
    pub graphics_queues: Option<u32>,
    /// The Vulkan apiVersion `(major, minor, patch)`; `None` when the card has no Vulkan driver here.
    pub api: Option<(u32, u32, u32)>,
    /// The driver as Vulkan describes it, and the Windows driver version of DXGI.
    pub driver: DriverFacts,
}

/// The refresh rate and the scale factor of one monitor, in the order of `SystemFacts::displays`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DisplayExtra {
    pub refresh_hz: Option<u32>,
    pub scale_percent: Option<u32>,
}

/// Wine (and so Proton, the Steam Deck's way): `ntdll.dll` exports `wine_get_version`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WineFacts {
    pub version: String,
    /// The host system as `wine_get_host_version` says it ("Linux 6.5.0"); empty when it does not.
    pub host: String,
}

/// The drive that holds the game data folder: the drive letter only, never a path.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StorageFact {
    pub drive: String,
    pub free: u64,
    pub total: u64,
}

/// `GetSystemPowerStatus`, reduced to what a bug report needs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PowerFact {
    /// `Some(true)` on AC power, `Some(false)` on battery, `None` when unknown.
    pub on_ac: Option<bool>,
    /// A battery is present.
    pub has_battery: bool,
    /// Battery charge, 0 to 100; `None` when unknown.
    pub battery_percent: Option<u8>,
}

/// The machine facts that are not about graphics. Every part is read in a way that cannot fail the bundle: what could not
/// be read stays `None` or empty and the report says "unknown".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MachineFacts {
    /// The processor's brand string ("AMD Ryzen 7 5700X 8-Core Processor"), already cleaned by [`clean_cpu_brand`];
    /// empty when it could not be read.
    pub cpu_model: String,
    pub cpu_cores: Option<u32>,
    pub cpu_threads: Option<u32>,
    pub sse4_1: Option<bool>,
    pub avx2: Option<bool>,
    pub ram_total: Option<u64>,
    pub ram_available: Option<u64>,
    /// The Windows edition ("Professional", "Core"); empty when unknown.
    pub os_edition: String,
    /// The processor architecture ("AMD64", "ARM64"); empty when unknown.
    pub os_architecture: String,
    pub wine: Option<WineFacts>,
    pub storage: Option<StorageFact>,
    pub power: Option<PowerFact>,
    /// The connected pads by name (no serial numbers).
    pub controllers: Vec<String>,
}

/// What `system.txt` says about the machine and the launcher (all plain text, no paths of the player).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SystemFacts {
    pub windows: String,
    pub cpu: String,
    pub machine: MachineFacts,
    pub gpus: Vec<GpuFact>,
    /// The graphics card and API this run would use: `(card, api)`; empty when none.
    pub chosen: (String, String),
    /// `(name, width, height, primary)` for each monitor.
    pub displays: Vec<(String, u32, u32, bool)>,
    /// Refresh rate and scale per monitor, by position in `displays` (missing entries are unknown).
    pub display_extras: Vec<DisplayExtra>,
    pub launcher_version: String,
    /// `Installer` or `Portable`.
    pub install: String,
    /// The installed games: `(name, serial, installed)`.
    pub games: Vec<(String, String, bool)>,
    /// The game the page shows.
    pub selected_game: String,
    /// Settings that decide the graphics (name, value).
    pub graphics: Vec<(String, String)>,
}

/// The vendor's name for a PCI vendor id.
pub fn vendor_name(vendor_id: u32) -> String {
    match vendor_id {
        0x10DE => "NVIDIA".to_string(),
        0x1002 => "AMD".to_string(),
        0x8086 => "Intel".to_string(),
        0x1414 => "Microsoft".to_string(),
        0 => "unknown".to_string(),
        other => format!("0x{other:04X}"),
    }
}

/// The CPUID brand string (48 bytes from leaves 0x80000002-4, or a registry value) as one clean line: cut at the first NUL,
/// control characters dropped, runs of spaces made one, trimmed. Old processors pad the string with spaces on the left.
pub fn clean_cpu_brand(raw: &[u8]) -> String {
    let end = raw.iter().position(|byte| *byte == 0).unwrap_or(raw.len());
    let text: String = String::from_utf8_lossy(&raw[..end]).chars().filter(|c| !c.is_control()).collect();
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// A size in GB (2^30 bytes) with one decimal.
pub fn gigabytes(bytes: u64) -> String {
    format!("{:.1}", bytes as f64 / (1u64 << 30) as f64)
}

/// The drive letter of a path as `C:`, or `None` for a path with no drive (a network share, a relative path). Nothing else
/// of the path is kept, so no user name can reach the report.
pub fn drive_of(path: &str) -> Option<String> {
    let mut chars = path.chars();
    let (letter, colon) = (chars.next()?, chars.next()?);
    if letter.is_ascii_alphabetic() && colon == ':' {
        Some(format!("{}:", letter.to_ascii_uppercase()))
    } else {
        None
    }
}

/// The number of physical cores in the buffer `GetLogicalProcessorInformationEx(RelationProcessorCore)` fills: records of
/// `{relationship: u32, size: u32, ...}`, one per core. `None` for a buffer that does not parse (a size of zero, or a record
/// that runs past the end).
pub fn count_core_records(buffer: &[u8]) -> Option<u32> {
    const RELATION_PROCESSOR_CORE: u32 = 0;
    let mut offset = 0usize;
    let mut cores = 0u32;
    while offset < buffer.len() {
        let header = buffer.get(offset..offset + 8)?;
        let relationship = u32::from_le_bytes(header[0..4].try_into().ok()?);
        let size = u32::from_le_bytes(header[4..8].try_into().ok()?) as usize;
        if size < 8 || offset + size > buffer.len() {
            return None;
        }
        if relationship == RELATION_PROCESSOR_CORE {
            cores += 1;
        }
        offset += size;
    }
    Some(cores)
}

fn yes_no(value: Option<bool>) -> &'static str {
    match value {
        Some(true) => "yes",
        Some(false) => "no",
        None => "unknown",
    }
}

fn number_or_unknown(value: Option<u32>) -> String {
    value.map_or("unknown".to_string(), |number| number.to_string())
}

/// The lines about the processor and memory.
fn machine_lines(machine: &MachineFacts) -> Vec<String> {
    vec![
        format!("cpu model: {}", or_unknown(&machine.cpu_model)),
        format!(
            "cpu cores: {} physical, {} threads",
            number_or_unknown(machine.cpu_cores),
            number_or_unknown(machine.cpu_threads)
        ),
        format!("cpu features: SSE4.1 {}, AVX2 {}", yes_no(machine.sse4_1), yes_no(machine.avx2)),
        format!(
            "memory: total {} GB, available {} GB",
            machine.ram_total.map_or("unknown".to_string(), gigabytes),
            machine.ram_available.map_or("unknown".to_string(), gigabytes)
        ),
    ]
}

/// The lines about the operating system: the edition and architecture (the `windows:` line keeps the version and build) and
/// whether this is Wine or Proton.
fn os_lines(machine: &MachineFacts) -> Vec<String> {
    let mut lines = vec![format!(
        "windows edition: {}, architecture {}",
        or_unknown(&machine.os_edition),
        or_unknown(&machine.os_architecture)
    )];
    lines.push(match &machine.wine {
        Some(wine) if wine.host.trim().is_empty() => format!("wine: Wine {}", or_unknown(&wine.version)),
        Some(wine) => format!("wine: Wine {} (host {})", or_unknown(&wine.version), wine.host.trim()),
        None => "wine: not detected".to_string(),
    });
    lines
}

/// One line about the Vulkan side of a card: apiVersion, the driver as it names itself, the decoded version and the Windows
/// driver version. The raw value stays on the `graphics card` line.
fn vulkan_driver_line(index: usize, gpu: &GpuFact) -> String {
    let api = gpu.api.map_or("unknown".to_string(), |(major, minor, patch)| format!("{major}.{minor}.{patch}"));
    let name = match (gpu.driver.name.trim(), gpu.driver.id) {
        ("", None) => "unknown".to_string(),
        ("", Some(id)) => format!("id {id}"),
        (name, None) => name.to_string(),
        (name, Some(id)) => format!("{name} (id {id})"),
    };
    let decoded = decode_driver_version(gpu.vendor_id, gpu.driver.id, gpu.driver_version).unwrap_or_else(|| "unknown".to_string());
    let info = if gpu.driver.info.trim().is_empty() { "unknown" } else { gpu.driver.info.trim() };
    format!(
        "graphics card {} vulkan: api {}, driver {}, driver info {}, driver version {}, windows driver {}",
        index + 1,
        api,
        name,
        info,
        decoded,
        gpu.driver.windows_driver.map_or("unknown".to_string(), windows_driver_text)
    )
}

/// The lines about storage, power and controllers.
fn environment_lines(machine: &MachineFacts) -> Vec<String> {
    let mut lines = Vec::new();
    lines.push(match &machine.storage {
        Some(storage) => format!(
            "storage: drive {}, free {} GB of {} GB",
            or_unknown(&storage.drive),
            gigabytes(storage.free),
            gigabytes(storage.total)
        ),
        None => "storage: unknown".to_string(),
    });
    lines.push(match &machine.power {
        Some(power) => {
            let source = match power.on_ac {
                Some(true) => "AC power",
                Some(false) => "battery",
                None => "unknown source",
            };
            let battery = match (power.has_battery, power.battery_percent) {
                (false, _) => "no battery".to_string(),
                (true, Some(percent)) => format!("battery {percent}%"),
                (true, None) => "battery charge unknown".to_string(),
            };
            format!("power: {source}, {battery}")
        }
        None => "power: unknown".to_string(),
    });
    if machine.controllers.is_empty() {
        lines.push("controllers: none connected".to_string());
    }
    for (index, name) in machine.controllers.iter().enumerate() {
        lines.push(format!("controller {}: {}", index + 1, or_unknown(name)));
    }
    lines
}

/// `system.txt`: one `key: value` line per fact.
pub fn system_text(facts: &SystemFacts) -> String {
    let mut lines = Vec::new();
    lines.push(format!("launcher: GT4Recomp {}", facts.launcher_version));
    lines.push(format!("install: {}", facts.install));
    lines.push(format!("windows: {}", or_unknown(&facts.windows)));
    lines.push(format!("cpu: {}", or_unknown(&facts.cpu)));
    lines.extend(os_lines(&facts.machine));
    lines.extend(machine_lines(&facts.machine));
    if facts.gpus.is_empty() {
        lines.push("graphics cards: none found".to_string());
    }
    for (index, gpu) in facts.gpus.iter().enumerate() {
        lines.push(format!(
            "graphics card {}: {} (vendor {}, driver version {:#010x}, video memory {} MB, graphics queues {})",
            index + 1,
            or_unknown(&gpu.name),
            vendor_name(gpu.vendor_id),
            gpu.driver_version,
            gpu.video_memory / (1024 * 1024),
            gpu.graphics_queues.map_or("unknown".to_string(), |queues| queues.to_string())
        ));
        lines.push(vulkan_driver_line(index, gpu));
    }
    let (card, api) = &facts.chosen;
    lines.push(format!("chosen card: {} (API {})", or_unknown(card), or_unknown(api)));
    if facts.displays.is_empty() {
        lines.push("displays: none found".to_string());
    }
    for (index, (name, width, height, primary)) in facts.displays.iter().enumerate() {
        let extra = facts.display_extras.get(index).copied().unwrap_or_default();
        lines.push(format!(
            "display {} ({}): {}x{}, refresh {}, scale {}",
            or_unknown(name),
            if *primary { "primary" } else { "secondary" },
            width,
            height,
            extra.refresh_hz.map_or("unknown".to_string(), |hz| format!("{hz} Hz")),
            extra.scale_percent.map_or("unknown".to_string(), |percent| format!("{percent}%"))
        ));
    }
    lines.extend(environment_lines(&facts.machine));
    lines.push(format!("selected game: {}", or_unknown(&facts.selected_game)));
    for (name, serial, installed) in &facts.games {
        lines.push(format!(
            "game {name} ({serial}): {}",
            if *installed { "installed" } else { "not installed" }
        ));
    }
    for (name, value) in &facts.graphics {
        lines.push(format!("setting {name}: {value}"));
    }
    let mut text = lines.join("\n");
    text.push('\n');
    text
}

fn or_unknown(text: &str) -> &str {
    if text.trim().is_empty() {
        "unknown"
    } else {
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    use std::time::{Duration, UNIX_EPOCH};

    const TIME: LocalTime = LocalTime { year: 2026, month: 10, day: 23, hour: 21, minute: 3, second: 40 };

    #[test]
    fn the_bundle_is_named_by_local_time() {
        assert_eq!(zip_name(&TIME), "GT4Recomp-logs-20261023-210340.zip");
    }

    #[test]
    fn the_user_name_and_the_profile_paths_are_masked() {
        let text = "found C:\\Users\\Alano\\AppData\\Roaming\\GT4Recomp and c:/users/alano/Desktop; user Alano here";
        assert_eq!(
            mask_text(text, "Alano"),
            "found C:\\Users\\<user>\\AppData\\Roaming\\GT4Recomp and C:/Users/<user>/Desktop; user <user> here"
        );
        // Other users and ordinary words stay.
        assert_eq!(mask_text("C:\\Users\\Other\\x", "Alano"), "C:\\Users\\Other\\x");
        assert_eq!(mask_text("Alanocorp", "Al"), "Alanocorp", "a very short name is left alone");
        assert_eq!(mask_text("nothing here", "Alano"), "nothing here");
    }

    #[test]
    fn masking_applies_to_every_entry() {
        let entries = vec![Entry { path: "latest.log".into(), text: "C:\\Users\\Alano\\x ALANO".into() }];
        assert_eq!(masked(&entries, "Alano")[0].text, "C:\\Users\\<user>\\x <user>");
    }

    #[test]
    fn cards_saves_replays_discs_prepared_files_and_programs_are_forbidden() {
        for path in [
            "Saves/Active/BASCUS-97328GAMEDATA/garage",
            "BASCUS-97328GAMEDATA/icon.sys",
            "card.ps2",
            "runtime/card.ps2",
            "disc/SCUS-97328.iso",
            "prepared/SCUS_973.28",
            "gt4.exe",
            "gt4-gs.dll",
            "Replays/BASCUS-97328R0001/data",
            "backups/card.mcd",
            "sessions/session-1/gt4-gs.ps2",
            "retroachievements.toml",
            ".retroachievements.toml.tmp-1234-1-1",
            ".RETROACHIEVEMENTS.TOML.TMP-9999-5",
            "config/.retroachievements.toml.tmp-555-1",
        ] {
            assert!(is_forbidden(path), "{path} must be refused");
        }
        for path in ["latest.log", "launcher-20261023-210340.log", "sessions/session-1/game.log", "settings.toml", "system.txt"] {
            assert!(!is_forbidden(path), "{path} is allowed");
        }
    }

    #[test]
    fn the_newest_items_are_chosen_newest_first() {
        let base = UNIX_EPOCH + Duration::from_secs(1_000_000);
        let found = vec![
            (base + Duration::from_secs(10), "b"),
            (base + Duration::from_secs(50), "e"),
            (base + Duration::from_secs(30), "d"),
            (base, "a"),
            (base + Duration::from_secs(20), "c"),
        ];
        assert_eq!(newest(found, KEPT_FOR_EXPORT), vec!["e", "d", "c"]);
    }

    #[test]
    fn the_zip_holds_exactly_the_entries_and_no_forbidden_file() {
        let root = std::env::temp_dir().join(format!("gt4l-log-export-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&root);
        let path = root.join("bundle.zip");
        let entries = masked(
            &[
                Entry { path: "latest.log".into(), text: "[1] INFO C:\\Users\\Alano\\x".into() },
                Entry { path: "sessions/session-a/game.log".into(), text: "game".into() },
                Entry { path: "system.txt".into(), text: system_text(&SystemFacts::default()) },
            ],
            "Alano",
        );
        write_zip(&path, &entries).unwrap();
        let mut archive = zip::ZipArchive::new(File::open(&path).unwrap()).unwrap();
        let mut names = Vec::new();
        for index in 0..archive.len() {
            let mut file = archive.by_index(index).unwrap();
            names.push(file.name().to_string());
            let mut text = String::new();
            file.read_to_string(&mut text).unwrap();
            assert!(!text.contains("Alano"), "the user name is masked in {}", file.name());
        }
        names.sort();
        assert_eq!(names, vec!["latest.log", "sessions/session-a/game.log", "system.txt"]);
        assert!(names.iter().all(|name| !is_forbidden(name)));
        // A forbidden entry is refused before anything is written.
        let refused = write_zip(&root.join("refused.zip"), &[Entry { path: "Saves/Active/x".into(), text: String::new() }]);
        assert!(refused.is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn the_system_report_names_every_fact_and_no_user_path() {
        let facts = SystemFacts {
            windows: "Windows 11 (build 26100)".into(),
            cpu: "AMD64 Family 25 Model 33".into(),
            gpus: vec![
                GpuFact { name: "NVIDIA GeForce RTX 3060 Laptop GPU".into(), vendor_id: 0x10DE, driver_version: 0x0001_5521, video_memory: 6 << 30, graphics_queues: Some(16), ..GpuFact::default() },
                GpuFact { name: "Intel(R) UHD Graphics".into(), vendor_id: 0x8086, driver_version: 0, video_memory: 128 << 20, graphics_queues: None, ..GpuFact::default() },
            ],
            chosen: ("NVIDIA GeForce RTX 3060 Laptop GPU".into(), "vulkan".into()),
            displays: vec![("DELL U3419W".into(), 3440, 1440, true)],
            launcher_version: "1.3.0".into(),
            install: "Installer".into(),
            games: vec![("Gran Turismo 4".into(), "SCUS-97328".into(), true), ("Gran Turismo 4 Spec II".into(), "SCUS-97436".into(), false)],
            selected_game: "Gran Turismo 4".into(),
            graphics: vec![("graphics_api".into(), "auto".into())],
            ..SystemFacts::default()
        };
        let text = system_text(&facts);
        for expected in [
            "launcher: GT4Recomp 1.3.0",
            "install: Installer",
            "windows: Windows 11 (build 26100)",
            "graphics card 1: NVIDIA GeForce RTX 3060 Laptop GPU (vendor NVIDIA, driver version 0x00015521, video memory 6144 MB, graphics queues 16)",
            "graphics card 2: Intel(R) UHD Graphics (vendor Intel",
            "graphics queues unknown)",
            "chosen card: NVIDIA GeForce RTX 3060 Laptop GPU (API vulkan)",
            "display DELL U3419W (primary): 3440x1440",
            "game Gran Turismo 4 Spec II (SCUS-97436): not installed",
            "setting graphics_api: auto",
        ] {
            assert!(text.contains(expected), "missing {expected:?} in\n{text}");
        }
        assert!(!text.contains("C:\\Users"), "no profile path in the report");
        assert_eq!(vendor_name(0x1234), "0x1234");
    }

    fn full_facts() -> SystemFacts {
        SystemFacts {
            windows: "Windows 11 24H2 (build 26100)".into(),
            cpu: "AMD64 Family 25 Model 33".into(),
            machine: MachineFacts {
                cpu_model: "AMD Ryzen 7 5700X 8-Core Processor".into(),
                cpu_cores: Some(8),
                cpu_threads: Some(16),
                sse4_1: Some(true),
                avx2: Some(true),
                ram_total: Some(32 << 30),
                ram_available: Some((12 << 30) + (1 << 29)),
                os_edition: "Professional".into(),
                os_architecture: "AMD64".into(),
                wine: None,
                storage: Some(StorageFact { drive: "D:".into(), free: 120 << 30, total: 931 << 30 }),
                power: Some(PowerFact { on_ac: Some(false), has_battery: true, battery_percent: Some(55) }),
                controllers: vec!["Xbox Wireless Controller".into(), "DualSense Wireless Controller".into()],
            },
            gpus: vec![GpuFact {
                name: "NVIDIA GeForce RTX 4060".into(),
                vendor_id: 0x10DE,
                driver_version: (581 << 22) | (15 << 14),
                video_memory: 8 << 30,
                graphics_queues: Some(16),
                api: Some((1, 4, 312)),
                driver: DriverFacts {
                    api_patch: 312,
                    id: Some(4),
                    name: "NVIDIA".into(),
                    info: "581.15".into(),
                    windows_driver: Some((32u64 << 48) | (15u64 << 16) | 8115),
                },
            }],
            displays: vec![("DELL U3419W".into(), 3440, 1440, true), ("Second".into(), 1920, 1080, false)],
            display_extras: vec![DisplayExtra { refresh_hz: Some(144), scale_percent: Some(125) }],
            ..SystemFacts::default()
        }
    }

    #[test]
    fn the_report_has_one_plain_line_for_each_new_fact() {
        let text = system_text(&full_facts());
        for expected in [
            "cpu cores: 8 physical, 16 threads",
            "cpu features: SSE4.1 yes, AVX2 yes",
            "memory: total 32.0 GB, available 12.5 GB",
            "windows edition: Professional, architecture AMD64",
            "wine: not detected",
            "graphics card 1 vulkan: api 1.4.312, driver NVIDIA (id 4), driver info 581.15, driver version 581.15.0.0, windows driver 32.0.15.8115",
            "display DELL U3419W (primary): 3440x1440, refresh 144 Hz, scale 125%",
            "display Second (secondary): 1920x1080, refresh unknown, scale unknown",
            "storage: drive D:, free 120.0 GB of 931.0 GB",
            "power: battery, battery 55%",
            "controller 1: Xbox Wireless Controller",
            "controller 2: DualSense Wireless Controller",
        ] {
            assert!(text.lines().any(|line| line == expected), "missing {expected:?} in\n{text}");
        }
        // The raw driver value stays on the card line.
        assert!(text.contains("driver version 0x"), "{text}");
        assert!(text.lines().all(|line| line.contains(": ")), "one `key: value` fact per line:\n{text}");
    }

    #[test]
    fn every_unread_fact_says_unknown_and_the_report_still_builds() {
        let text = system_text(&SystemFacts {
            gpus: vec![GpuFact { name: "X".into(), ..GpuFact::default() }],
            displays: vec![("D".into(), 800, 600, true)],
            ..SystemFacts::default()
        });
        for expected in [
            "cpu model: unknown",
            "cpu cores: unknown physical, unknown threads",
            "cpu features: SSE4.1 unknown, AVX2 unknown",
            "memory: total unknown GB, available unknown GB",
            "windows edition: unknown, architecture unknown",
            "graphics card 1 vulkan: api unknown, driver unknown, driver info unknown, driver version unknown, windows driver unknown",
            "display D (primary): 800x600, refresh unknown, scale unknown",
            "storage: unknown",
            "power: unknown",
            "controllers: none connected",
        ] {
            assert!(text.lines().any(|line| line == expected), "missing {expected:?} in\n{text}");
        }
    }

    #[test]
    fn a_wine_report_names_the_version_and_the_host() {
        let mut facts = full_facts();
        facts.machine.wine = Some(WineFacts { version: "9.0".into(), host: "Linux 6.5.0-valve".into() });
        assert!(system_text(&facts).lines().any(|line| line == "wine: Wine 9.0 (host Linux 6.5.0-valve)"));
        facts.machine.wine = Some(WineFacts { version: "10.0".into(), host: String::new() });
        assert!(system_text(&facts).lines().any(|line| line == "wine: Wine 10.0"));
        // Proton on a Steam Deck: RADV with the single graphics queue, Mesa's standard driver version.
        facts.gpus[0] = GpuFact {
            name: "AMD Custom GPU 0405 (RADV VANGOGH)".into(),
            vendor_id: 0x1002,
            driver_version: (24 << 22) | 5,
            graphics_queues: Some(1),
            api: Some((1, 3, 274)),
            driver: DriverFacts { id: Some(3), name: "radv".into(), info: "Mesa 24.0.5".into(), ..DriverFacts::default() },
            ..GpuFact::default()
        };
        let text = system_text(&facts);
        assert!(text.contains("graphics card 1 vulkan: api 1.3.274, driver radv (id 3), driver info Mesa 24.0.5, driver version 24.0.5, windows driver unknown"), "{text}");
        assert!(text.contains("graphics queues 1)"), "{text}");
    }

    #[test]
    fn the_cpu_brand_is_cut_at_the_nul_and_trimmed_and_the_line_says_unknown_without_it() {
        let mut raw = b"  AMD Ryzen 7 5700X  8-Core Processor            ".to_vec();
        raw.push(0);
        raw.extend_from_slice(b"garbage after the end");
        assert_eq!(clean_cpu_brand(&raw), "AMD Ryzen 7 5700X 8-Core Processor");
        assert_eq!(clean_cpu_brand(b"Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz\r\n"), "Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz");
        assert_eq!(clean_cpu_brand(b""), "");
        assert_eq!(clean_cpu_brand(&[0, b'x']), "");
        let mut facts = full_facts();
        facts.machine.cpu_model = clean_cpu_brand(&raw);
        let text = system_text(&facts);
        assert!(text.lines().any(|line| line == "cpu model: AMD Ryzen 7 5700X 8-Core Processor"), "{text}");
        assert!(text.lines().any(|line| line == "cpu: AMD64 Family 25 Model 33"), "the cpu line stays");
        facts.machine.cpu_model = String::new();
        assert!(system_text(&facts).lines().any(|line| line == "cpu model: unknown"));
    }

    #[test]
    fn plain_drives_sizes_and_core_records() {
        assert_eq!(drive_of("D:\\Games\\GT4Recomp").as_deref(), Some("D:"));
        assert_eq!(drive_of("c:/users/alano/x").as_deref(), Some("C:"));
        assert_eq!(drive_of("\\\\server\\share\\x"), None, "a network share has no drive letter");
        assert_eq!(drive_of("relative/path"), None);
        assert_eq!(drive_of(""), None);
        assert_eq!(gigabytes(0), "0.0");
        assert_eq!(gigabytes((15 << 30) + (1 << 29)), "15.5");
        // Two core records (relationship 0) and one cache record (relationship 2), each 12 bytes here.
        let record = |relationship: u32| {
            let mut bytes = relationship.to_le_bytes().to_vec();
            bytes.extend_from_slice(&12u32.to_le_bytes());
            bytes.extend_from_slice(&[0u8; 4]);
            bytes
        };
        let buffer: Vec<u8> = [record(0), record(2), record(0)].concat();
        assert_eq!(count_core_records(&buffer), Some(2));
        assert_eq!(count_core_records(&[]), Some(0));
        assert_eq!(count_core_records(&buffer[..buffer.len() - 3]), None, "a record cut short");
        assert_eq!(count_core_records(&[0, 0, 0, 0, 0, 0, 0, 0]), None, "a size of zero");
    }

    #[test]
    fn no_new_line_can_carry_a_user_name_or_a_path() {
        let text = system_text(&full_facts());
        assert!(!text.contains('\\') && !text.contains("Users"), "{text}");
    }
}
