//! TASK-213 Phase 1: the in-game settings feedback (`game-changes.toml`).
//!
//! The launcher is the only writer of `settings.toml`. A change the player makes in GT4's own Options page is written by
//! the game to a separate file in the same config folder:
//!
//! ```toml
//! version = 1
//!
//! [changes]
//! fullscreen = true
//! ```
//!
//! The game writes it atomically (temporary file + rename) as soon as the value changes, so a crash before the game
//! closes does not lose it. The launcher merges it on game exit and again at the next start, validates every id and
//! value with the same rules the Settings page uses ([`settings::Settings::set_by_user`] and
//! [`settings::Settings::set_gs`]), logs what it refused, and deletes the file. A malformed or future-version file is
//! ignored and deleted, so a bad file can never keep the game from starting.
//!
//! The value forms are the launcher's stored forms (`on`/`off` for toggles, the stored text for choices), not the
//! environment forms. A TOML boolean is accepted for a toggle only; anything else must be a string.

use std::path::{Path, PathBuf};

use crate::settings::{self, Kind, Settings};

/// File name inside the launcher's config folder (the game receives the full path as `GT4_SETTINGS_FEEDBACK`).
pub const FILE_NAME: &str = "game-changes.toml";

/// The variable that tells the game where to write the feedback file.
pub const VARIABLE: &str = "GT4_SETTINGS_FEEDBACK";

/// The only format version this launcher understands.
pub const VERSION: i64 = 1;

/// The result of one feedback read. `warnings` are logged by the caller; nothing here is fatal.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Merge {
    /// The file existed (whether or not anything in it was accepted).
    pub existed: bool,
    /// The accepted changes, as the stored id and value.
    pub changes: Vec<(String, String)>,
    /// Lines for the launcher log: refused ids, refused values, an unreadable or unknown-version file.
    pub warnings: Vec<String>,
}

impl Merge {
    pub fn is_empty(&self) -> bool {
        self.changes.is_empty()
    }
}

/// The feedback file of a config folder (`game-changes.toml` beside `settings.toml`).
pub fn path(config_dir: &Path) -> PathBuf {
    config_dir.join(FILE_NAME)
}

fn toggle_text(value: &toml::Value) -> Option<&'static str> {
    match value {
        toml::Value::Boolean(true) => Some("on"),
        toml::Value::Boolean(false) => Some("off"),
        _ => None,
    }
}

/// Applies the text of a feedback file to `settings`. Returns the accepted changes and the refused ids/values; an
/// error means the whole file was ignored (syntax, missing or newer version).
pub fn apply_text(text: &str, settings: &mut Settings) -> Result<Merge, String> {
    let table: toml::Table = text
        .parse()
        .map_err(|error: toml::de::Error| format!("{FILE_NAME}: {error}"))?;
    match table.get("version").and_then(|value| value.as_integer()) {
        Some(VERSION) => {}
        Some(other) => return Err(format!("{FILE_NAME}: version {other} is not supported; ignored")),
        None => return Err(format!("{FILE_NAME}: no version; ignored")),
    }
    let mut merge = Merge { existed: true, ..Merge::default() };
    let Some(changes) = table.get("changes").and_then(|value| value.as_table()) else {
        return Ok(merge);
    };
    for (id, value) in changes {
        if let Some(definition) = settings::definition(id) {
            let text = match value {
                toml::Value::String(text) => text.clone(),
                other => match toggle_text(other) {
                    Some(text) if definition.kind == Kind::Toggle => text.to_string(),
                    _ => {
                        merge.warnings.push(format!(
                            "{FILE_NAME}: '{id}' cannot take the value {other}; ignored"
                        ));
                        continue;
                    }
                },
            };
            match settings.set_by_user(id, &text) {
                Ok(()) => merge.changes.push((definition.id.to_string(), settings.get(id).to_string())),
                Err(error) => merge.warnings.push(format!("{FILE_NAME}: {error}; ignored")),
            }
            continue;
        }
        if crate::gs_table::row(id).is_some() {
            match value.as_str() {
                None => merge.warnings.push(format!("{FILE_NAME}: GS setting '{id}' needs text; ignored")),
                Some(text) => match settings.set_gs(id, text) {
                    Ok(()) => merge.changes.push((id.clone(), settings.gs_value(id))),
                    Err(error) => merge.warnings.push(format!("{FILE_NAME}: {error}; ignored")),
                },
            }
            continue;
        }
        merge.warnings.push(format!("{FILE_NAME}: unknown setting '{id}' ignored"));
    }
    Ok(merge)
}

/// Reads, applies and deletes one feedback file. A missing file is not an event (returns an empty merge). A file that
/// cannot be read or parsed is reported and deleted too, so it is never merged twice.
pub fn merge_file(path: &Path, settings: &mut Settings) -> Merge {
    let text = match std::fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Merge::default(),
        Err(error) => {
            return Merge {
                existed: true,
                warnings: vec![format!("{}: {error}", path.display())],
                ..Merge::default()
            }
        }
    };
    let mut merge = match apply_text(&text, settings) {
        Ok(mut merge) => {
            merge.existed = true;
            merge
        }
        Err(error) => Merge { existed: true, warnings: vec![error], ..Merge::default() },
    };
    if let Err(error) = std::fs::remove_file(path) {
        if error.kind() != std::io::ErrorKind::NotFound {
            merge.warnings.push(format!("{}: could not delete the merged file: {error}", path.display()));
        }
    }
    merge
}

#[cfg(test)]
mod tests {
    use super::*;

    fn defaults() -> Settings {
        Settings::default()
    }

    fn apply(text: &str) -> (Merge, Settings) {
        let mut settings = defaults();
        let merge = apply_text(text, &mut settings).expect("the file is valid");
        (merge, settings)
    }

    fn work_file(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("gt4l-game-changes-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("work dir");
        dir.join(name)
    }

    #[test]
    fn a_valid_fullscreen_toggle_is_stored_and_marked_as_chosen() {
        let (merge, settings) = apply("version = 1\n\n[changes]\nfullscreen = true\n");
        assert_eq!(settings.get("fullscreen"), "on");
        assert!(settings.was_chosen("fullscreen"));
        assert_eq!(merge.changes, vec![("fullscreen".to_string(), "on".to_string())]);
        assert!(merge.warnings.is_empty());

        let (_, settings) = apply("version = 1\n\n[changes]\nfullscreen = false\n");
        assert_eq!(settings.get("fullscreen"), "off");
    }

    #[test]
    fn a_toggle_can_also_arrive_as_text() {
        let (merge, settings) = apply("version = 1\n\n[changes]\nfullscreen = \"on\"\n");
        assert_eq!(settings.get("fullscreen"), "on");
        assert_eq!(merge.changes.len(), 1);
    }

    #[test]
    fn an_invalid_value_is_refused_and_reported() {
        let (merge, settings) = apply("version = 1\n\n[changes]\nfullscreen = \"banana\"\n");
        assert_eq!(settings.get("fullscreen"), "on", "the stored value is unchanged");
        assert!(!settings.was_chosen("fullscreen"));
        assert!(merge.changes.is_empty());
        assert_eq!(merge.warnings.len(), 1);
        assert!(merge.warnings[0].contains("banana"));
    }

    #[test]
    fn a_boolean_is_refused_for_a_choice_row() {
        let (merge, settings) = apply("version = 1\n\n[changes]\nvsync = true\n");
        assert!(merge.changes.is_empty());
        assert!(merge.warnings[0].contains("vsync"));
        assert_eq!(settings.get("vsync"), "on", "the default is unchanged");
    }

    #[test]
    fn an_unknown_id_is_reported_and_dropped() {
        let (merge, settings) = apply("version = 1\n\n[changes]\nfrobnicate = true\n");
        assert!(merge.changes.is_empty());
        assert!(merge.warnings[0].contains("frobnicate"));
        assert!(!settings.was_chosen("frobnicate"));
    }

    #[test]
    fn a_gs_row_goes_through_the_gs_table() {
        let (merge, settings) = apply("version = 1\n\n[changes]\neffect_flares = \"false\"\n");
        assert_eq!(merge.changes, vec![("effect_flares".to_string(), "false".to_string())]);
        assert_eq!(settings.gs_value("effect_flares"), "false");
        assert!(merge.warnings.is_empty());
    }

    #[test]
    fn a_launcher_derived_gs_row_is_refused() {
        let (merge, _) = apply("version = 1\n\n[changes]\nupscale = \"3\"\n");
        assert!(merge.changes.is_empty());
        assert!(merge.warnings[0].contains("upscale"));
    }

    #[test]
    fn a_missing_version_ignores_the_whole_file() {
        let mut settings = defaults();
        let error = apply_text("[changes]\nfullscreen = true\n", &mut settings).unwrap_err();
        assert!(error.contains("no version"));
        assert_eq!(settings.get("fullscreen"), "on", "the default is unchanged");
        assert!(!settings.was_chosen("fullscreen"));
    }

    #[test]
    fn a_newer_version_ignores_the_whole_file() {
        let mut settings = defaults();
        let error = apply_text("version = 2\n\n[changes]\nfullscreen = true\n", &mut settings).unwrap_err();
        assert!(error.contains("version 2"));
        assert!(!settings.was_chosen("fullscreen"));
    }

    #[test]
    fn a_malformed_file_is_reported() {
        let mut settings = defaults();
        let error = apply_text("this is not toml = = =", &mut settings).unwrap_err();
        assert!(error.contains(FILE_NAME));
    }

    #[test]
    fn a_missing_file_merges_nothing() {
        let path = work_file("missing.toml");
        let _ = std::fs::remove_file(&path);
        let mut settings = defaults();
        let merge = merge_file(&path, &mut settings);
        assert!(!merge.existed);
        assert!(merge.changes.is_empty());
        assert!(merge.warnings.is_empty());
    }

    #[test]
    fn a_crash_left_file_is_merged_then_deleted() {
        let path = work_file("crash-left.toml");
        std::fs::write(&path, "version = 1\n\n[changes]\nfullscreen = true\n").unwrap();
        let mut settings = defaults();
        let merge = merge_file(&path, &mut settings);
        assert!(merge.existed);
        assert_eq!(settings.get("fullscreen"), "on");
        assert_eq!(merge.changes.len(), 1);
        assert!(!path.exists(), "the file is deleted after the merge");
        // A second merge is a no-op: nothing is applied twice.
        let second = merge_file(&path, &mut settings);
        assert!(!second.existed && second.changes.is_empty());
    }

    #[test]
    fn a_malformed_crash_left_file_is_deleted_too() {
        let path = work_file("broken.toml");
        std::fs::write(&path, "version = 1\n\n[changes\n").unwrap();
        let mut settings = defaults();
        let merge = merge_file(&path, &mut settings);
        assert!(merge.existed && !merge.warnings.is_empty());
        assert!(!path.exists());
    }

    #[test]
    fn resolution_and_monitor_are_validated_and_stored() {
        // The game writes TOML basic strings; the launcher stores the same text the Settings page does.
        let (merge, settings) = apply(
            "version = 1\n\n[changes]\nresolution = \"1920x1080\"\nmonitor = \"\\\\\\\\.\\\\DISPLAY1@1920x1080\"\n",
        );
        assert_eq!(settings.get("resolution"), "1920x1080");
        assert_eq!(settings.get("monitor"), r"\\.\DISPLAY1@1920x1080");
        assert!(settings.was_chosen("resolution"));
        assert!(settings.was_chosen("monitor"));
        assert_eq!(merge.changes.len(), 2);
        assert!(merge.warnings.is_empty());
    }

    #[test]
    fn renderer_and_render_scale_are_stored() {
        // The Graphics submenu writes the launcher's stored forms: renderer pcsx2/coregs20 and the scale percentage.
        let (merge, settings) =
            apply("version = 1\n\n[changes]\nrenderer = \"coregs20\"\nrender_scale = \"125\"\n");
        assert_eq!(settings.get("renderer"), "coregs20");
        assert_eq!(settings.get("render_scale"), "125");
        assert!(settings.was_chosen("renderer"));
        assert!(settings.was_chosen("render_scale"));
        assert_eq!(merge.changes.len(), 2);
        assert!(merge.warnings.is_empty());
    }

    #[test]
    fn bad_resolution_and_monitor_values_are_refused() {
        let (merge, settings) = apply(
            "version = 1\n\n[changes]\nresolution = \"abc\"\nmonitor = \"no-device-here\"\n",
        );
        assert!(merge.changes.is_empty());
        assert_eq!(merge.warnings.len(), 2);
        assert_eq!(settings.get("resolution"), crate::display::DESKTOP, "the default is unchanged");
        assert_eq!(settings.get("monitor"), crate::display::PRIMARY);
        assert!(!settings.was_chosen("resolution"));
        assert!(!settings.was_chosen("monitor"));
    }

    #[test]
    fn the_feedback_path_is_beside_the_settings_file() {
        let path = path(Path::new("C:/config"));
        assert_eq!(path, Path::new("C:/config").join(FILE_NAME));
    }
}
