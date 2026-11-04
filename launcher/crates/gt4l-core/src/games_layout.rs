//! The move from the 1.2.x folders to the multi-game folders (TASK-101, release 1.3.0, owner 2026-10-23).
//!
//! 1.2.x kept Gran Turismo 4's files flat: `<data>\disc`, `<data>\prepared`, `<data>\runtime` and, in the user data
//! folder, `Saves`, `Replays`, `backups`. From 1.3.0 every game has its own folders (`games\<id>`), so a second game
//! never mixes with the first. The existing files become Gran Turismo 4's (`games\gt4`).
//!
//! The rules are those of the 1.2.1 move (`crate::relocate`): same volume is a rename, another volume is a verified copy
//! (size and SHA-256, the pinned disc hash through the caller's check) and only then the removal of the source; a
//! failed unit keeps its old place in use and runs again at the next start; nothing is overwritten and nothing is
//! deleted before it is verified; a marker is written when no unit failed. Neither move runs while the game runs.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::games::GT4;
use crate::paths::{DataDirs, UserDirs};
use crate::relocate::{self, FsOps, LargeMove, LargeOutcome, LargeProgress, UnitResult};

/// Moves Gran Turismo 4's flat large data (`disc`, `prepared` together, then `runtime`) into `games\gt4`.
/// `check_game_files` runs on the game files in their new place (see `LargeMove::check_game_files`). Never panics; every
/// problem is in the outcome.
pub fn migrate_data(
    data: &DataDirs,
    game_running: bool,
    stamp: &str,
    check_game_files: &dyn Fn(&Path, &BTreeMap<PathBuf, String>) -> Result<(), String>,
    ops: &FsOps<'_>,
    progress: &mut dyn FnMut(&LargeProgress),
) -> LargeOutcome {
    let marker = data.games_layout_marker();
    if relocate::marker_status(&marker).as_deref() == Some("migrated") {
        return LargeOutcome::AlreadyMoved;
    }
    let flat = data.flat_gt4_root().to_path_buf();
    let destination = data.for_game(&GT4).game_root();
    let anything = ["disc", "prepared", "runtime"].iter().any(|name| relocate::has_files(&flat.join(name)));
    if !anything {
        return LargeOutcome::Ran(Vec::new());
    }
    if game_running {
        return LargeOutcome::Deferred("Gran Turismo 4 is running".to_string());
    }
    let request = LargeMove {
        local: &flat,
        data_root: &destination,
        textures_sources: Vec::new(),
        old_copy_folder: "from-1.2.x-layout",
        marker: marker.clone(),
        game_running,
        stamp,
        check_game_files,
    };
    let mut units = vec![("game files".to_string(), relocate::move_game_files(&request, ops, progress))];
    let runtime_source = flat.join("runtime");
    let runtime = if !relocate::has_files(&runtime_source) {
        UnitResult::Absent
    } else if relocate::has_files(&destination.join("runtime")) {
        UnitResult::Kept(format!(
            "{} already has a runtime card; the older one in {} is not used and may be deleted",
            destination.display(),
            runtime_source.display()
        ))
    } else {
        let no_check = |_: &Path, _: &BTreeMap<PathBuf, String>| Ok(());
        relocate::move_tree(&runtime_source, &destination.join("runtime"), ops, &mut |_| {}, &no_check)
    };
    units.push(("runtime".to_string(), runtime));
    let outcome = LargeOutcome::Ran(units);
    if !outcome.failed() {
        let value = serde_json::json!({
            "schema": relocate::MARKER_SCHEMA,
            "status": "migrated",
            "at_utc": relocate::stamp_json(stamp),
            "from": flat.display().to_string(),
            "to": destination.display().to_string(),
            "units": outcome.log_lines(),
        });
        let _ = relocate::write_marker(&marker, &value);
    }
    outcome
}

/// Whether Gran Turismo 4's flat 1.2.x game folders (`disc`, `prepared`, `runtime`) still hold files that have not moved.
pub fn flat_data_pending(data: &DataDirs) -> bool {
    if relocate::marker_status(&data.games_layout_marker()).as_deref() == Some("migrated") {
        return false;
    }
    let flat = data.flat_gt4_root();
    ["disc", "prepared", "runtime"].iter().any(|name| relocate::has_files(&flat.join(name)))
}

/// What the move of Gran Turismo 4's library did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UserLayoutOutcome {
    /// No flat library on this PC (a fresh install, or already moved).
    NothingToMove,
    /// The game runs; the flat folders stay in use this time.
    Deferred(String),
    /// Moved to `games\gt4`. `kept` lists folders left where they are because the new place already had files.
    Moved { folders: Vec<String> },
    /// Nothing is lost; the flat folders stay in use and the move runs again at the next start.
    Failed(String),
}

impl UserLayoutOutcome {
    /// Whether Gran Turismo 4's library is in `games\gt4` after this outcome.
    pub fn uses_new_place(&self) -> bool {
        matches!(self, UserLayoutOutcome::NothingToMove | UserLayoutOutcome::Moved { .. })
    }

    pub fn log_lines(&self) -> Vec<String> {
        match self {
            UserLayoutOutcome::NothingToMove => vec!["library: no 1.2.x library to move".to_string()],
            UserLayoutOutcome::Deferred(reason) => vec![format!("library: not moved now ({reason}); the 1.2.x folders are used")],
            UserLayoutOutcome::Moved { folders } => vec![format!("library: moved {} to games\\gt4", folders.join(", "))],
            UserLayoutOutcome::Failed(error) => {
                vec![format!("library: WARNING not moved, the 1.2.x folders stay in use and the move runs again at the next start: {error}")]
            }
        }
    }
}

/// Moves Gran Turismo 4's flat library (`Saves`, `Replays`, `backups` in the library folder) into `games\gt4`. A
/// folder whose new place already holds files is a failure (both are kept, the flat one stays in use): the launcher never
/// merges or overwrites saves.
pub fn migrate_user_library(user: &UserDirs, game_running: bool, ops: &FsOps<'_>) -> UserLayoutOutcome {
    let flat_user = {
        let mut flat = user.clone();
        flat.set_flat_gt4(true);
        flat
    };
    let mut new_user = user.clone();
    new_user.set_flat_gt4(false);
    let new_places = [new_user.saves(), new_user.replays(), new_user.card_backups()];
    let pairs: Vec<(String, PathBuf, PathBuf)> = flat_user
        .flat_gt4_places()
        .into_iter()
        .zip(new_places)
        .filter(|((_, source), _)| relocate::has_files(source))
        .map(|((name, source), destination)| (name.to_string(), source, destination))
        .collect();
    if pairs.is_empty() {
        return UserLayoutOutcome::NothingToMove;
    }
    if game_running {
        return UserLayoutOutcome::Deferred("Gran Turismo 4 is running".to_string());
    }
    if let Some((name, _, destination)) = pairs.iter().find(|(_, _, destination)| relocate::has_files(destination)) {
        return UserLayoutOutcome::Failed(format!("{} of games\\gt4 already holds files ({}); nothing was moved", name, destination.display()));
    }
    let no_check = |_: &Path, _: &BTreeMap<PathBuf, String>| Ok(());
    let mut moved: Vec<(String, PathBuf, PathBuf)> = Vec::new();
    for (name, source, destination) in &pairs {
        match relocate::move_tree(source, destination, ops, &mut |_| {}, &no_check) {
            UnitResult::Moved { .. } => moved.push((name.clone(), source.clone(), destination.clone())),
            UnitResult::Failed(error) | UnitResult::Kept(error) => {
                // Put the folders already moved back, so one library is never split across two layouts.
                for (_, back_to, from) in moved.iter().rev() {
                    let _ = (ops.rename)(from, back_to);
                }
                return UserLayoutOutcome::Failed(error);
            }
            UnitResult::Absent => {}
        }
    }
    UserLayoutOutcome::Moved { folders: moved.into_iter().map(|(name, _, _)| name).collect() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::paths::UserRootSource;
    use std::fs;
    use std::io;

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-games-layout-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn accept(_: &Path, _: &BTreeMap<PathBuf, String>) -> Result<(), String> {
        Ok(())
    }

    fn flat_data(root: &Path) {
        write(&root.join("disc").join("SCUS-97328.iso"), "image");
        write(&root.join("prepared").join("SCUS_973.28"), "elf");
        write(&root.join("prepared").join("prepare.json"), "{}");
        write(&root.join("runtime").join("card.ps2"), "card");
        write(&root.join("gs").join("current.txt"), "1.2.3");
    }

    #[test]
    fn same_volume_renames_the_flat_game_files_into_games_gt4() {
        let base = scratch("data-rename");
        flat_data(&base);
        let data = DataDirs::new(&base);
        let outcome = migrate_data(&data, false, "20261023-000000", &accept, &FsOps::real(), &mut |_| {});
        assert!(!outcome.failed());
        assert_eq!(fs::read_to_string(base.join("games/gt4/disc/SCUS-97328.iso")).unwrap(), "image");
        assert_eq!(fs::read_to_string(base.join("games/gt4/prepared/SCUS_973.28")).unwrap(), "elf");
        assert_eq!(fs::read_to_string(base.join("games/gt4/runtime/card.ps2")).unwrap(), "card");
        assert!(!base.join("disc").exists() && !base.join("prepared").exists() && !base.join("runtime").exists());
        assert!(base.join("gs").join("current.txt").is_file(), "the shared renderer stays where it is");
        assert_eq!(relocate::marker_status(&data.games_layout_marker()).as_deref(), Some("migrated"));
        assert_eq!(migrate_data(&data, false, "x", &accept, &FsOps::real(), &mut |_| {}), LargeOutcome::AlreadyMoved);
        assert_eq!(data.prepared(), base.join("games").join("gt4").join("prepared"));
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn another_volume_copies_checks_and_only_then_removes_the_source() {
        let base = scratch("data-copy");
        flat_data(&base);
        let data = DataDirs::new(&base);
        let other_volume = |from: &Path, to: &Path| -> io::Result<()> {
            if from.starts_with(&base) && to.starts_with(&base) && from.file_name() != Some("prepared.tmp".as_ref()) {
                // Every rename between the flat folder and games\gt4 reports another volume; staging renames work.
                if !to.to_string_lossy().contains(".migrating") && !from.to_string_lossy().contains(".migrating") {
                    return Err(io::Error::from_raw_os_error(relocate::ERROR_NOT_SAME_DEVICE));
                }
            }
            fs::rename(from, to)
        };
        let ops = FsOps { rename: &other_volume, open: FsOps::real().open };
        let checked = std::cell::Cell::new(false);
        let check = |folder: &Path, hashes: &BTreeMap<PathBuf, String>| -> Result<(), String> {
            checked.set(true);
            assert!(!hashes.is_empty(), "a copy reports the SHA-256 of every file");
            assert!(folder.join("disc").join("SCUS-97328.iso").is_file(), "the check sees the copy before it is placed");
            assert!(base.join("disc").join("SCUS-97328.iso").is_file(), "the source is still there during the check");
            Ok(())
        };
        let outcome = migrate_data(&data, false, "s", &check, &ops, &mut |_| {});
        assert!(!outcome.failed(), "{outcome:?}");
        assert!(checked.get());
        assert_eq!(fs::read_to_string(base.join("games/gt4/disc/SCUS-97328.iso")).unwrap(), "image");
        assert!(!base.join("disc").exists(), "removed after the checked copy");
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_failed_check_leaves_the_source_and_resumes_at_the_next_start() {
        let base = scratch("data-resume");
        flat_data(&base);
        let data = DataDirs::new(&base);
        let refuse = |_: &Path, _: &BTreeMap<PathBuf, String>| Err("the pinned disc hash differs".to_string());
        let outcome = migrate_data(&data, false, "s", &refuse, &FsOps::real(), &mut |_| {});
        assert!(outcome.failed());
        assert!(base.join("disc").join("SCUS-97328.iso").is_file(), "nothing was deleted");
        assert!(base.join("prepared").join("prepare.json").is_file());
        assert!(!data.games_layout_marker().exists(), "no marker, so the move runs again");
        assert_eq!(data.prepared(), base.join("prepared"), "the flat place stays in use");
        let again = migrate_data(&data, false, "s", &accept, &FsOps::real(), &mut |_| {});
        assert!(!again.failed());
        assert!(base.join("games/gt4/prepared/prepare.json").is_file());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn nothing_moves_while_the_game_runs_or_when_there_is_nothing() {
        let base = scratch("data-idle");
        let data = DataDirs::new(&base);
        assert_eq!(migrate_data(&data, false, "s", &accept, &FsOps::real(), &mut |_| {}), LargeOutcome::Ran(Vec::new()));
        assert!(!data.games_layout_marker().exists(), "a fresh install writes no marker");
        flat_data(&base);
        assert!(matches!(migrate_data(&data, true, "s", &accept, &FsOps::real(), &mut |_| {}), LargeOutcome::Deferred(_)));
        assert!(base.join("disc").is_dir());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn an_existing_games_gt4_is_never_overwritten() {
        let base = scratch("data-kept");
        flat_data(&base);
        write(&base.join("games/gt4/disc/SCUS-97328.iso"), "newer");
        write(&base.join("games/gt4/prepared/prepare.json"), "{}");
        let data = DataDirs::new(&base);
        let outcome = migrate_data(&data, false, "s", &accept, &FsOps::real(), &mut |_| {});
        assert!(matches!(outcome.units()[0].1, UnitResult::Kept(_)));
        assert_eq!(fs::read_to_string(base.join("games/gt4/disc/SCUS-97328.iso")).unwrap(), "newer");
        assert!(base.join("disc").join("SCUS-97328.iso").is_file(), "the older copy is kept");
        assert_eq!(data.disc_folder(), base.join("games").join("gt4").join("disc"), "the new place wins");
        let _ = fs::remove_dir_all(&base);
    }

    fn user_with_flat_library(base: &Path) -> UserDirs {
        write(&base.join("Saves/Active/BASCUS-97328GAMEDATA/file"), "save");
        write(&base.join("Saves/Save 01/BASCUS-97328GAMEDATA/file"), "slot");
        write(&base.join("Replays/replay-1/file"), "replay");
        write(&base.join("backups/card-1.ps2"), "card");
        write(&base.join("config/settings.toml"), "x");
        let mut user = UserDirs::new(base, UserRootSource::Override);
        user.set_flat_gt4(true);
        user
    }

    #[test]
    fn the_flat_library_becomes_the_gt4_library_and_the_settings_stay() {
        let base = scratch("user-move");
        let user = user_with_flat_library(&base);
        assert_eq!(user.saves(), base.join("Saves"), "flat until the move");
        let outcome = migrate_user_library(&user, false, &FsOps::real());
        assert_eq!(outcome, UserLayoutOutcome::Moved { folders: vec!["Saves".into(), "Replays".into(), "backups".into()] });
        let mut moved = user.clone();
        moved.set_flat_gt4(false);
        assert_eq!(moved.saves(), base.join("games").join("gt4").join("Saves"));
        assert_eq!(fs::read_to_string(moved.saves().join("Active/BASCUS-97328GAMEDATA/file")).unwrap(), "save");
        assert_eq!(fs::read_to_string(moved.saves().join("Save 01/BASCUS-97328GAMEDATA/file")).unwrap(), "slot");
        assert_eq!(fs::read_to_string(moved.replays().join("replay-1/file")).unwrap(), "replay");
        assert_eq!(fs::read_to_string(moved.card_backups().join("card-1.ps2")).unwrap(), "card");
        assert!(!base.join("Saves").exists() && !base.join("Replays").exists() && !base.join("backups").exists());
        assert!(base.join("config/settings.toml").is_file());
        assert!(!moved.for_game(&crate::games::SPEC2).saves().exists(), "Spec II starts with no library");
        assert_eq!(migrate_user_library(&user, false, &FsOps::real()), UserLayoutOutcome::NothingToMove);
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_library_move_that_fails_part_way_is_undone_and_the_flat_library_stays() {
        let base = scratch("user-undo");
        let user = user_with_flat_library(&base);
        let replays = base.join("Replays");
        let failing = |from: &Path, to: &Path| -> io::Result<()> {
            if from == replays { Err(io::Error::other("locked")) } else { fs::rename(from, to) }
        };
        let ops = FsOps { rename: &failing, open: FsOps::real().open };
        let outcome = migrate_user_library(&user, false, &ops);
        assert!(matches!(outcome, UserLayoutOutcome::Failed(_)), "{outcome:?}");
        assert!(!outcome.uses_new_place());
        assert!(base.join("Saves/Active/BASCUS-97328GAMEDATA/file").is_file(), "Saves moved back");
        assert!(base.join("Replays/replay-1/file").is_file());
        assert!(!base.join("games/gt4/Saves/Active").exists());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_library_with_files_in_both_places_is_not_merged() {
        let base = scratch("user-both");
        let user = user_with_flat_library(&base);
        write(&base.join("games/gt4/Saves/Active/x"), "other");
        let outcome = migrate_user_library(&user, false, &FsOps::real());
        assert!(matches!(outcome, UserLayoutOutcome::Failed(_)));
        assert!(base.join("Saves/Active/BASCUS-97328GAMEDATA/file").is_file());
        assert!(matches!(migrate_user_library(&user, true, &FsOps::real()), UserLayoutOutcome::Deferred(_)));
        let _ = fs::remove_dir_all(&base);
    }
}
