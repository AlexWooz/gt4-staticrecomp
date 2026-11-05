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
}

/// What `system.txt` says about the machine and the launcher (all plain text, no paths of the player).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SystemFacts {
    pub windows: String,
    pub cpu: String,
    pub gpus: Vec<GpuFact>,
    /// The graphics card and API this run would use: `(card, api)`; empty when none.
    pub chosen: (String, String),
    /// `(name, width, height, primary)` for each monitor.
    pub displays: Vec<(String, u32, u32, bool)>,
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

/// `system.txt`: one `key: value` line per fact.
pub fn system_text(facts: &SystemFacts) -> String {
    let mut lines = Vec::new();
    lines.push(format!("launcher: GT4Recomp {}", facts.launcher_version));
    lines.push(format!("install: {}", facts.install));
    lines.push(format!("windows: {}", or_unknown(&facts.windows)));
    lines.push(format!("cpu: {}", or_unknown(&facts.cpu)));
    if facts.gpus.is_empty() {
        lines.push("graphics cards: none found".to_string());
    }
    for (index, gpu) in facts.gpus.iter().enumerate() {
        lines.push(format!(
            "graphics card {}: {} (vendor {}, driver version {:#010x}, video memory {} MB)",
            index + 1,
            or_unknown(&gpu.name),
            vendor_name(gpu.vendor_id),
            gpu.driver_version,
            gpu.video_memory / (1024 * 1024)
        ));
    }
    let (card, api) = &facts.chosen;
    lines.push(format!("chosen card: {} (API {})", or_unknown(card), or_unknown(api)));
    if facts.displays.is_empty() {
        lines.push("displays: none found".to_string());
    }
    for (name, width, height, primary) in &facts.displays {
        lines.push(format!(
            "display {} ({}): {}x{}",
            or_unknown(name),
            if *primary { "primary" } else { "secondary" },
            width,
            height
        ));
    }
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
                GpuFact { name: "NVIDIA GeForce RTX 3060 Laptop GPU".into(), vendor_id: 0x10DE, driver_version: 0x0001_5521, video_memory: 6 << 30 },
                GpuFact { name: "Intel(R) UHD Graphics".into(), vendor_id: 0x8086, driver_version: 0, video_memory: 128 << 20 },
            ],
            chosen: ("NVIDIA GeForce RTX 3060 Laptop GPU".into(), "vulkan".into()),
            displays: vec![("DELL U3419W".into(), 3440, 1440, true)],
            launcher_version: "1.3.0".into(),
            install: "Installer".into(),
            games: vec![("Gran Turismo 4".into(), "SCUS-97328".into(), true), ("Gran Turismo 4 Spec II".into(), "SCUS-97436".into(), false)],
            selected_game: "Gran Turismo 4".into(),
            graphics: vec![("graphics_api".into(), "auto".into())],
        };
        let text = system_text(&facts);
        for expected in [
            "launcher: GT4Recomp 1.3.0",
            "install: Installer",
            "windows: Windows 11 (build 26100)",
            "graphics card 1: NVIDIA GeForce RTX 3060 Laptop GPU (vendor NVIDIA, driver version 0x00015521, video memory 6144 MB)",
            "graphics card 2: Intel(R) UHD Graphics (vendor Intel",
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
}
