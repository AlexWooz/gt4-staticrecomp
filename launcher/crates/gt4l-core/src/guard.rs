//! The data-path guard: installs and updates must never write to the user's data (see `docs/RELEASES.md` and the
//! owner rule "user data is sacred"). Every write the install and update code makes under the data folder is
//! checked here first. A refused path is an error, never a silent skip.
//!
//! The guard is lexical: `..` is resolved before the check, and names are compared case-insensitively
//! (Windows). A path outside the data folder is not user data for this guard; the program folder is replaced by
//! updates on purpose. The one exception is the player's folder `Documents\Gran Turismo 4` (TASK-081): it is
//! refused wherever it is, both as registered at start ([`protect_user_library`]) and by name (a `Saves` or
//! `Replays` folder inside a `Gran Turismo 4` folder). Only the save library writes there.

use std::path::{Component, Path};
use std::sync::Mutex;

use crate::paths::USER_FOLDER_NAME;

/// Folders of the player's `Gran Turismo 4` folder, refused wherever that folder is (lower case).
pub const USER_LIBRARY_FOLDERS: &[&str] = &["saves", "replays"];

/// Folders under the data folder that hold memory cards, save slots, replays, the card the game reads, or the
/// copy of the player's disc image (`disc`, made by setup; the game reads it at run time).
pub const PROTECTED_FOLDERS: &[&str] = &["saves", "cards", "library", "replays", "runtime", "disc"];

/// File extensions of memory card images and save files, refused anywhere under the data folder.
pub const PROTECTED_EXTENSIONS: &[&str] = &["ps2", "psu", "mcd", "card", "sav"];

/// The components of a path, with `..` resolved, names lower-cased.
fn normalized(path: &Path) -> Vec<String> {
    let mut parts: Vec<String> = Vec::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                parts.pop();
            }
            other => parts.push(other.as_os_str().to_string_lossy().to_lowercase()),
        }
    }
    parts
}

/// The player's folders registered by the launcher at start (`Documents\Gran Turismo 4`, or its fallback).
static USER_LIBRARY_ROOTS: Mutex<Vec<Vec<String>>> = Mutex::new(Vec::new());

/// Protects the player's folder (`Documents\Gran Turismo 4`, wherever Documents is redirected) for the rest of the
/// process: [`check_write`] refuses every path inside it. Only the save library code writes there, and it does not
/// go through this guard.
pub fn protect_user_library(root: &Path) {
    let normalized_root = normalized(root);
    let mut roots = USER_LIBRARY_ROOTS.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if !roots.contains(&normalized_root) {
        roots.push(normalized_root);
    }
}

/// `Err` when `target` is inside the player's folder: a registered root, or (registered or not) a `Saves` or
/// `Replays` folder directly inside a folder named `Gran Turismo 4`, or that folder itself.
pub fn check_user_library(target: &Path) -> Result<(), String> {
    let path = normalized(target);
    let roots = USER_LIBRARY_ROOTS.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let registered = roots.iter().any(|root| !root.is_empty() && path.starts_with(root));
    drop(roots);
    let folder_name = USER_FOLDER_NAME.to_lowercase();
    let lexical = path.iter().enumerate().any(|(index, part)| {
        *part == folder_name
            && match path.get(index + 1) {
                None => true,
                Some(next) => USER_LIBRARY_FOLDERS.contains(&next.as_str()),
            }
    });
    if registered || lexical {
        return Err(format!("refused: {} is in the player's Gran Turismo 4 folder (saves and replays)", target.display()));
    }
    Ok(())
}

/// `Ok` when `target` may be written by an install or update. An error names the refused path.
pub fn check_write(data_root: &Path, target: &Path) -> Result<(), String> {
    check_user_library(target)?;
    let root = normalized(data_root);
    let path = normalized(target);
    if !path.starts_with(&root) {
        return Ok(());
    }
    let inside = &path[root.len()..];
    if inside.iter().any(|part| PROTECTED_FOLDERS.contains(&part.as_str())) {
        return Err(format!("refused: {} is user data (saves, cards, replays or the disc copy)", target.display()));
    }
    if let Some(last) = inside.last() {
        if let Some((_, extension)) = last.rsplit_once('.') {
            if PROTECTED_EXTENSIONS.contains(&extension) {
                return Err(format!("refused: {} is a memory card or save file", target.display()));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{check_user_library, check_write, protect_user_library};
    use std::path::Path;

    const DATA: &str = "C:/Users/someone/AppData/Local/GT4Recomp";

    #[test]
    fn renderer_and_settings_paths_are_allowed() {
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/1.0.1"))).is_ok());
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/current.txt"))).is_ok());
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/config/settings.toml"))).is_ok());
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/prepared/CORE.GT4"))).is_ok());
    }

    #[test]
    fn card_save_and_replay_folders_are_refused_at_any_depth() {
        for path in ["saves/slot1.psu", "cards/card.ps2", "library/Active save", "replays/run.bin", "runtime/card.ps2", "disc/SCUS-97328.iso", "disc"] {
            let target = format!("{DATA}/{path}");
            assert!(check_write(Path::new(DATA), Path::new(&target)).is_err(), "{target} must be refused");
        }
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/saves/x.txt"))).is_err());
    }

    #[test]
    fn memory_card_and_save_extensions_are_refused_anywhere_under_the_data_folder() {
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/copy.PS2"))).is_err());
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/sessions/misc/x.psu"))).is_err());
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/1.0.1/gt4-gs.exe"))).is_ok());
    }

    #[test]
    fn parent_segments_are_resolved_before_the_check() {
        let escaping = format!("{DATA}/gs/../saves/slot1.psu");
        assert!(check_write(Path::new(DATA), Path::new(&escaping)).is_err());
        let back_inside = format!("{DATA}/saves/../gs/1.0.1");
        assert!(check_write(Path::new(DATA), Path::new(&back_inside)).is_ok());
    }

    #[test]
    fn names_are_compared_case_insensitively() {
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/SAVES/x"))).is_err());
    }

    #[test]
    fn paths_outside_the_data_folder_are_not_user_data_for_this_guard() {
        assert!(check_write(Path::new(DATA), Path::new("C:/Program Files/GT4Recomp/version.json")).is_ok());
    }

    #[test]
    fn the_documents_gran_turismo_4_folder_is_refused_wherever_documents_is() {
        for path in [
            "C:/Users/someone/Documents/Gran Turismo 4",
            "C:/Users/someone/Documents/Gran Turismo 4/Saves/Active/BASCUS-97328GAMEDATA/garage",
            "C:/Users/someone/OneDrive/Documents/Gran Turismo 4/Replays/BASCUS-97328R0001",
            "D:/Redirected/Docs/gran turismo 4/SAVES/Save 01",
            "D:/Redirected/Docs/Gran Turismo 4/Saves/../Replays/x",
        ] {
            assert!(check_write(Path::new(DATA), Path::new(path)).is_err(), "{path} must be refused");
            assert!(check_user_library(Path::new(path)).is_err(), "{path} must be refused");
        }
        // The data-folder fallback of the player's folder is protected the same way.
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/Gran Turismo 4/Saves/x"))).is_err());
        // `..` that leaves the player's folder is resolved first.
        assert!(check_user_library(Path::new("D:/Docs/Gran Turismo 4/Saves/../../GT4Recomp/gs")).is_ok());
    }

    #[test]
    fn a_registered_player_folder_is_refused_at_any_depth() {
        let root = std::env::temp_dir().join(format!("gt4l-guard-{}", std::process::id())).join("Docs-Override").join("GT4 Player");
        protect_user_library(&root);
        protect_user_library(&root); // registering twice is harmless
        assert!(check_write(Path::new(DATA), &root).is_err());
        assert!(check_write(Path::new(DATA), &root.join("anything").join("file.txt")).is_err());
        assert!(check_write(Path::new(DATA), &root.with_file_name("GT4 Player sibling")).is_ok());
    }

    #[test]
    fn the_renderer_and_program_folders_stay_writable() {
        assert!(check_write(Path::new(DATA), Path::new(&format!("{DATA}/gs/1.0.1/gt4-gs.exe"))).is_ok());
        assert!(check_write(Path::new(DATA), Path::new("C:/Games/Gran Turismo 4 Launcher/version.json")).is_ok());
        // A program folder named after the game is not the player's folder unless Saves or Replays follow.
        assert!(check_write(Path::new(DATA), Path::new("D:/Games/Gran Turismo 4/game/gt4.exe")).is_ok());
    }
}
