//! The start-up steps of the 1.2.1 layout (TASK-091, owner 2026-10-22), in order:
//!
//! 1. [`prepare`]: resolve the folders (`<exe dir>\data`, `%APPDATA%\GT4Recomp`), then move the 1.2.0 user data
//!    (settings, controls, card backups, saves, replays) before the window opens. It is small; until it has
//!    happened (the game runs, or a copy failed), the 1.2.0 places stay in use.
//! 2. [`clean_old_install`]: remove the per-user install of 1.0.0 to 1.2.0 that this account can still see.
//! 3. [`move_large_data`], on a background thread: move the 1.2.0 large data (disc copy, prepared files, gt4-gs,
//!    runtime card, sessions, logs, custom textures) into `<exe dir>\data`, with progress for the page. Play and
//!    setup wait for it.
//!
//! The rules of both moves are in `gt4l_core::relocate`.

use gt4l_core::messages;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

use gt4l_core::games_layout::{self, UserLayoutOutcome};
use gt4l_core::paths::{self, DataDirs, Layout, LayoutInputs, LegacyLocations, UserDirs, TEXTURES_FOLDER_NAME};
use gt4l_core::relocate::{self, FsOps, LargeMove, LargeOutcome, UnitResult, UserMove};
use gt4l_core::textures::{self as texture_folder, FlattenOps};
use gt4l_disc::setup::{self as disc_setup, Installed};
use gt4l_disc::Pins;

use crate::state::{self, lock, Shared};

/// What the start needs from step 1.
pub struct Startup {
    pub layout: Layout,
    /// The user data in use (the 1.2.1 place, or the 1.2.0 places until the move happened).
    pub user: UserDirs,
    /// Lines for the launcher log, written once the data folder is known to be usable.
    pub log: Vec<String>,
    /// A sentence for the page, and whether it reports a problem.
    pub notice: Option<(String, bool)>,
}

/// Whether the game or its renderer process runs: nothing moves then.
fn game_running() -> bool {
    gt4l_platform::process::process_running(gt4l_platform::process::GAME_PROCESS_NAMES)
}

/// The 1.2.0 places of the user data, for a start where the move has not happened.
pub fn legacy_user_dirs(layout: &Layout, reason: String) -> UserDirs {
    let legacy = &layout.legacy;
    let Some(local) = legacy.local.as_ref() else {
        return layout.user_dirs();
    };
    let player = legacy
        .documents_player
        .clone()
        .filter(|player| player.is_dir())
        .or_else(|| legacy.local_player())
        .unwrap_or_else(|| local.join(paths::USER_FOLDER_NAME));
    UserDirs::legacy(local, &player, reason)
}

/// `PreviousDataDir` applies only when this launcher runs from the registered install folder.
fn previous_data_for(
    exe_dir: &Path,
    read_registered: &dyn Fn() -> Option<String>,
    read_previous: &dyn Fn() -> Option<PathBuf>,
) -> Option<PathBuf> {
    let registered = read_registered()?;
    let key = |folder: &str| folder.trim().trim_end_matches(['\\', '/']).to_ascii_lowercase();
    if key(&exe_dir.to_string_lossy()) != key(&registered) {
        return None;
    }
    read_previous()
}

/// Step 1. `Err` only when no data or user folder can be named at all (or, in the portable build, cannot be written).
pub fn prepare(exe_dir: &Path) -> Result<Startup, String> {
    prepare_as(
        exe_dir,
        gt4l_core::build_kind::BUILD.is_portable(),
        &gt4l_platform::install_record::registered_install_folder,
        &gt4l_platform::install_record::previous_data_dir,
    )
}

/// [`prepare`] for a given build kind, with the two registry reads injected (the portable build never calls them).
pub fn prepare_as(
    exe_dir: &Path,
    portable: bool,
    read_registered: &dyn Fn() -> Option<String>,
    read_previous: &dyn Fn() -> Option<PathBuf>,
) -> Result<Startup, String> {
    // The portable build (TASK-098) reads no registry value and no system folder: no Documents lookup, no recorded
    // install, no 1.2.0 places.
    let mut inputs = LayoutInputs::from_environment(exe_dir, if portable { None } else { gt4l_platform::known_folder::documents() });
    inputs.portable = portable;
    if !portable {
        // The data folder of an earlier install in another folder, as the setup recorded it (TASK-091). Only the
        // registered install takes it: a portable copy started on the same PC must never move the installed data.
        inputs.previous_data = previous_data_for(exe_dir, read_registered, read_previous);
    }
    let layout = paths::resolve_layout(&inputs, &paths::probe_writable)?;
    let mut log = layout.notices.clone();
    log.push(format!(
        "folders: data {} (read also: {}), user {}",
        layout.data.root().display(),
        layout.data.read_layers().iter().map(|layer| layer.display().to_string()).collect::<Vec<_>>().join(", "),
        layout.user_root.display()
    ));
    let new_user = layout.user_dirs();
    if portable {
        // A portable folder starts in the 1.3.0 layout: there is nothing to move from 1.2.0 or 1.2.x, so none of the
        // migrations below run.
        log.push("portable build: no 1.2.0 migration, no registry".to_string());
        return Ok(Startup { layout, user: new_user, log, notice: None });
    }
    let outcome = relocate::migrate_user_data(
        &UserMove {
            legacy: &layout.legacy,
            user: &new_user,
            data_root: layout.data.root(),
            game_running: game_running(),
            stamp: &gt4saves::timestamp(),
        },
        &FsOps::real(),
    );
    log.extend(outcome.log_lines(&new_user));
    let notice = outcome.notice(&new_user).map(|text| (text, !outcome.uses_new_place()));
    let mut user = if outcome.uses_new_place() {
        new_user
    } else {
        legacy_user_dirs(&layout, format!("{outcome:?}"))
    };
    // TASK-101: Gran Turismo 4's library moves into `games\gt4` (the 1.3.0 layout). It is small and the move is a rename,
    // so it is done here, before the window. Until it has happened the flat 1.2.x folders stay in use.
    if outcome.uses_new_place() {
        let moved = games_layout::migrate_user_library(&user, game_running(), &FsOps::real());
        log.extend(moved.log_lines());
        user.set_flat_gt4(!moved.uses_new_place());
        if let UserLayoutOutcome::Failed(_) = moved {
            log.push("library: the saves are used where they are".to_string());
        }
    }
    Ok(Startup { layout, user, log, notice })
}

/// Step 2: the per-user install of 1.0.0 to 1.2.0 (program files, Start-menu shortcuts, `HKCU` entries). Skipped
/// under the development overrides, so a test run never touches a real install.
pub fn clean_old_install(exe_dir: &Path) -> Vec<String> {
    clean_old_install_as(gt4l_core::build_kind::BUILD.is_portable(), exe_dir)
}

/// [`clean_old_install`] for a given build kind. The portable build never touches the registry, Start menu or program
/// folders of an install.
pub fn clean_old_install_as(portable: bool, exe_dir: &Path) -> Vec<String> {
    if portable {
        return Vec::new();
    }
    if std::env::var_os(paths::DATA_DIR_OVERRIDE_VAR).is_some() || std::env::var_os(paths::USER_DIR_OVERRIDE_VAR).is_some() {
        return Vec::new();
    }
    #[cfg(windows)]
    {
        use gt4l_platform::legacy_install::{clean_old_per_user_install, Places, WindowsUserRegistry};
        let local = std::env::var_os("LOCALAPPDATA").map(PathBuf::from);
        let programs = gt4l_platform::known_folder::programs();
        let places = Places { exe_dir, local_app_data: local.as_deref(), start_menu_programs: programs.as_deref() };
        clean_old_per_user_install(&WindowsUserRegistry, &places).log_lines()
    }
    #[cfg(not(windows))]
    {
        let _ = exe_dir;
        Vec::new()
    }
}

/// The check of game files placed under `folder` (`prepared` and `disc` side by side): when the 1.2.0 files were
/// ready, the moved ones must pass the setup check, and a copied image must have the receipt's SHA-256 (and the
/// pinned one for a qualified dump).
pub fn check_moved_game_files(folder: &Path, hashes: &BTreeMap<PathBuf, String>, pins: &Pins, source_ready: bool) -> Result<(), String> {
    let prepared = folder.join("prepared");
    match disc_setup::check_installed(&prepared, &folder.join("disc"), folder, pins) {
        Installed::Ready { image, .. } => {
            if hashes.is_empty() {
                return Ok(());
            }
            let receipt = disc_setup::read_receipt(&prepared).ok_or("the moved setup receipt cannot be read")?;
            let recorded = receipt.image.as_ref().map(|image| image.sha256.clone()).unwrap_or_default();
            let name = image.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
            let copied = hashes.get(&Path::new("disc").join(&name)).cloned().unwrap_or_default();
            if copied.is_empty() || !copied.eq_ignore_ascii_case(&recorded) {
                return Err(format!("the copied disc image {name} does not match its setup receipt (sha256)"));
            }
            if receipt.qualified_dump && !copied.eq_ignore_ascii_case(&pins.disc.sha256) {
                return Err(format!("the copied disc image {name} does not match the pinned disc (sha256)"));
            }
            Ok(())
        }
        other if source_ready => Err(format!("the moved game files do not pass the setup check: {other:?}")),
        _ => Ok(()),
    }
}

/// Resets a moved folder to the permissions of the data folder (a rename keeps the profile folder's permissions,
/// which other Windows users of the PC cannot read). Best effort; the result is logged.
fn inherit_permissions(dirs_root: &Path, moved: &Path) -> Option<String> {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        if !moved.exists() || paths::same_path(moved, dirs_root) {
            return None;
        }
        let status = Command::new("icacls.exe")
            .arg(moved)
            .args(["/reset", "/T", "/C", "/Q"])
            .creation_flags(CREATE_NO_WINDOW)
            .status();
        match status {
            Ok(status) if status.success() => None,
            Ok(status) => Some(format!("permissions of {} not reset (icacls {status})", moved.display())),
            Err(error) => Some(format!("permissions of {} not reset: {error}", moved.display())),
        }
    }
    #[cfg(not(windows))]
    {
        let _ = (dirs_root, moved, Command::new("true"));
        None
    }
}

/// One folder the large data moves from, with what its move needs.
struct LargeSource {
    /// The folder holding the 1.2.0 layout (disc, prepared, gs, runtime, sessions, logs).
    local: PathBuf,
    /// Where its custom textures may be.
    textures: Vec<PathBuf>,
    /// The marker of this move (one per source folder).
    marker: PathBuf,
    /// The folder under `logs` for kept logs, sessions and runtime card (see `LargeMove::old_copy_folder`).
    old_copy_folder: &'static str,
    /// The data folder of an earlier install: it is removed once its move has emptied it.
    is_previous_install: bool,
}

/// The folders the large data moves from, in order: the 1.2.0 folder, then the data folder of an earlier install in
/// another folder (`PreviousDataDir`, TASK-091). A folder that is the data folder itself, or is missing, is left out.
fn large_sources(legacy: &LegacyLocations, dirs: &DataDirs) -> Vec<LargeSource> {
    let mut sources = Vec::new();
    if let Some(local) = legacy.local.clone() {
        // The data folder could not be written, so the 1.2.0 folder is the write folder itself: nothing moves.
        if !paths::same_path(&local, dirs.root()) {
            let textures = [legacy.documents_player.clone(), legacy.documents_moved()]
                .into_iter()
                .flatten()
                .map(|player| player.join(TEXTURES_FOLDER_NAME))
                .collect();
            sources.push(LargeSource {
                local,
                textures,
                marker: dirs.relocation_marker(),
                old_copy_folder: "from-1.2.0",
                is_previous_install: false,
            });
        }
    }
    if let Some(previous) = legacy.previous_data.clone() {
        if previous.is_dir() && !paths::same_path(&previous, dirs.root()) {
            sources.push(LargeSource {
                textures: vec![previous.join(TEXTURES_FOLDER_NAME)],
                marker: dirs.previous_data_marker(&previous),
                local: previous,
                old_copy_folder: "from-previous-install",
                is_previous_install: true,
            });
        }
    }
    sources
}

/// Step 3, on a background thread. Moves each large-data source (`large_sources`) with its own marker. Updates
/// `migration` in the state for the page; leaves Play and setup blocked while it runs.
pub fn move_large_data(shared: &Shared, legacy: &LegacyLocations) {
    let (dirs, pins) = {
        let state = lock(shared);
        (state.dirs.clone(), state.pins.clone())
    };
    let sources = large_sources(legacy, &dirs);
    // The 1.2.0 sources move first (into the flat 1.2.x names), then the flat names move into `games\gt4` (TASK-101).
    if sources.is_empty() && !games_layout::flat_data_pending(&dirs) {
        lock(shared).dirs.set_flat_gt4(false);
        return;
    }
    lock(shared).migration.running = true;
    let mut notices: Vec<String> = Vec::new();
    let mut problem = false;
    for source in &sources {
        let (notice, failed) = move_source(shared, &dirs, &pins, source);
        if !notice.is_empty() {
            notices.push(notice);
        }
        problem |= failed;
    }
    let (layout_notice, layout_failed) = move_games_layout(shared, &dirs, &pins);
    if !layout_notice.is_empty() {
        notices.push(layout_notice);
    }
    problem |= layout_failed;
    let mut state = lock(shared);
    state.migration = crate::state::MigrationRun {
        running: false,
        notice: if notices.is_empty() { state.migration.notice.clone() } else { notices.join(" ") },
        problem: problem || state.migration.problem,
        ..Default::default()
    };
}

/// The move of the flat 1.2.x game folders into `games\gt4` (TASK-101, `gt4l_core::games_layout`). Returns the page sentence
/// (empty when nothing needs saying) and whether it reports a problem. After a move that did not fail, the flat folders are
/// no longer read.
fn move_games_layout(shared: &Shared, dirs: &DataDirs, pins: &Pins) -> (String, bool) {
    let flat = dirs.flat_gt4_root().to_path_buf();
    let source_ready = matches!(
        disc_setup::check_installed(&flat.join("prepared"), &flat.join("disc"), &flat, pins),
        Installed::Ready { .. }
    );
    let check = |folder: &Path, hashes: &BTreeMap<PathBuf, String>| check_moved_game_files(folder, hashes, pins, source_ready);
    let mut last_permille = u64::MAX;
    let outcome = games_layout::migrate_data(dirs, game_running(), &gt4saves::timestamp(), &check, &FsOps::real(), &mut |progress| {
        let permille = if progress.bytes_total == 0 { 0 } else { progress.bytes_done * 1000 / progress.bytes_total };
        if permille == last_permille {
            return;
        }
        last_permille = permille;
        let mut state = lock(shared);
        state.migration.item = progress.item.clone();
        state.migration.bytes_done = progress.bytes_done;
        state.migration.bytes_total = progress.bytes_total;
    });
    for line in outcome.log_lines() {
        state::log(dirs, &line);
    }
    match &outcome {
        LargeOutcome::Deferred(_) => (messages::text("move.organise_close", state::current_language()).to_string(), true),
        _ if outcome.failed() => (
            messages::text("move.organise_later", state::current_language()).to_string(),
            true,
        ),
        _ => {
            lock(shared).dirs.set_flat_gt4(false);
            (String::new(), false)
        }
    }
}

/// Moves one source. Returns the page sentence (empty when nothing happened) and whether it reports a problem.
fn move_source(shared: &Shared, dirs: &DataDirs, pins: &Pins, source: &LargeSource) -> (String, bool) {
    let local = &source.local;
    let source_ready = matches!(
        disc_setup::check_installed(&local.join("prepared"), &local.join("disc"), &local, pins),
        Installed::Ready { .. }
    );
    let check = |folder: &Path, hashes: &BTreeMap<PathBuf, String>| check_moved_game_files(folder, hashes, pins, source_ready);
    let request = LargeMove {
        local,
        data_root: dirs.root(),
        textures_sources: source.textures.clone(),
        marker: source.marker.clone(),
        game_running: game_running(),
        stamp: &gt4saves::timestamp(),
        check_game_files: &check,
        old_copy_folder: source.old_copy_folder,
    };
    let mut last_permille = u64::MAX;
    let outcome = relocate::migrate_large_data(&request, &FsOps::real(), &mut |progress| {
        let permille = if progress.bytes_total == 0 { 0 } else { progress.bytes_done * 1000 / progress.bytes_total };
        if permille == last_permille {
            return;
        }
        last_permille = permille;
        let mut state = lock(shared);
        state.migration.item = progress.item.clone();
        state.migration.bytes_done = progress.bytes_done;
        state.migration.bytes_total = progress.bytes_total;
    });
    for line in outcome.log_lines() {
        state::log(dirs, &line);
    }
    for (name, result) in outcome.units() {
        if let UnitResult::Moved { destination, renamed: true, .. } = result {
            let moved: Vec<PathBuf> = if name == "game files" {
                vec![destination.join("disc"), destination.join("prepared")]
            } else {
                vec![destination.clone()]
            };
            for folder in moved {
                if let Some(problem) = inherit_permissions(dirs.root(), &folder) {
                    state::log(dirs, &problem);
                }
            }
        }
    }
    let textures_failed = outcome.units().iter().any(|(name, result)| name == "textures" && matches!(result, UnitResult::Failed(_)));
    if textures_failed {
        // The textures stay in use where they are until the next start moves them.
        let in_place = source.textures.iter().find(|folder| folder.is_dir()).cloned();
        lock(shared).dirs.set_textures_in_place(in_place);
    }
    // The old data folder of an earlier install goes once it is empty (the launcher cannot remove Program Files;
    // the marker keeps the move from running again either way).
    if source.is_previous_install && !outcome.failed() {
        match relocate::remove_empty_folder(local) {
            Ok(()) => {}
            Err(error) => state::log_warn(dirs, &format!("the old data folder {} is kept: {error}", local.display())),
        }
    }
    let (notice, problem) = large_notice(&outcome, local, dirs.root());
    (notice, problem)
}

/// TASK-093: flattens an old `Textures\replacements` into `Textures` once gt4-gs 1.2.1 or later is in use (the rules
/// are in `gt4l_core::textures`). Runs on the same thread after [`move_large_data`], so the move is never in progress;
/// it waits for the next start while the game runs. Only the launcher's own data folder is touched.
pub fn flatten_textures(shared: &Shared) {
    let dirs = lock(shared).dirs.clone();
    let folder = dirs.textures();
    if !folder.starts_with(dirs.root()) {
        state::log(&dirs, "textures: not flattened (the folder is still in Documents until its move succeeds)");
        return;
    }
    if game_running() {
        state::log(&dirs, "textures: not flattened now (Gran Turismo 4 is running); the next start tries again");
        return;
    }
    let outcome = texture_folder::flatten_replacements(&folder, &dirs.renderer_root(), dirs.root(), &FlattenOps::real());
    for line in outcome.log_lines() {
        state::log(&dirs, &line);
    }
}

/// The sentence for the page after the large-data move (empty when nothing happened).
pub fn large_notice(outcome: &LargeOutcome, local: &Path, data_root: &Path) -> (String, bool) {
    match outcome {
        LargeOutcome::Deferred(_) => (messages::text("move.game_close", state::current_language()).to_string(), true),
        LargeOutcome::AlreadyMoved => (String::new(), false),
        LargeOutcome::Ran(units) => {
            if outcome.failed() {
                return (
                    messages::text("move.game_later", state::current_language()).to_string(),
                    true,
                );
            }
            let kept_game_files = units.iter().any(|(name, result)| name == "game files" && matches!(result, UnitResult::Kept(_)));
            if kept_game_files {
                return (
                    messages::render("move.old_copy", state::current_language(), &[("path", &local.display().to_string())]),
                    false,
                );
            }
            if outcome.moved() {
                return (messages::render("move.game_moved", state::current_language(), &[("path", &data_root.display().to_string())]), false);
            }
            (String::new(), false)
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn only_the_registered_install_takes_the_previous_data_folder() {
        let previous = || Some(PathBuf::from(r"C:\Program Files\Old\data"));
        let registered = || Some(r"C:\Program Files\GT4Recomp\".to_string());
        assert_eq!(
            super::previous_data_for(Path::new(r"c:\program files\gt4recomp"), &registered, &previous),
            previous()
        );
        assert_eq!(super::previous_data_for(Path::new(r"E:\Portable\GT4Recomp"), &registered, &previous), None);
        assert_eq!(super::previous_data_for(Path::new(r"C:\Program Files\GT4Recomp"), &|| None, &previous), None);
    }

    use super::*;
    use gt4l_core::paths::{DataDirs, UserRootSource};

    fn portable_scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-portable-start-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn the_portable_start_reads_no_registry_and_moves_nothing() {
        let exe_dir = portable_scratch("start").join("GT4Recomp");
        let no_registry = || -> Option<String> { panic!("the portable build must not read the registry") };
        let no_previous = || -> Option<PathBuf> { panic!("the portable build must not read PreviousDataDir") };
        let startup = prepare_as(&exe_dir, true, &no_registry, &no_previous).unwrap();
        assert_eq!(startup.layout.data.root(), exe_dir.join("data"));
        assert_eq!(startup.user.root(), exe_dir.join("user"));
        assert_eq!(startup.layout.user_source, UserRootSource::Portable);
        assert_eq!(startup.layout.legacy, LegacyLocations::default(), "no 1.2.0 places and no previous install");
        assert!(startup.layout.data.read_layers().is_empty());
        assert!(startup.notice.is_none());
        assert!(exe_dir.join("data").is_dir() && exe_dir.join("user").is_dir(), "the two folders are the whole footprint");
        let siblings: Vec<_> = std::fs::read_dir(&exe_dir).unwrap().map(|entry| entry.unwrap().file_name()).collect();
        assert_eq!(siblings.len(), 2, "nothing else was created: {siblings:?}");
        // A second start finds the same folders.
        let again = prepare_as(&exe_dir, true, &no_registry, &no_previous).unwrap();
        assert_eq!(again.layout.data.root(), startup.layout.data.root());
        let _ = std::fs::remove_dir_all(exe_dir.parent().unwrap());
    }

    #[test]
    fn a_portable_folder_that_cannot_be_written_asks_to_move_it() {
        let base = portable_scratch("readonly");
        // A file where the data folder should be: it can never be created.
        let exe_dir = base.join("GT4Recomp");
        std::fs::create_dir_all(&exe_dir).unwrap();
        std::fs::write(exe_dir.join("data"), b"not a folder").unwrap();
        let error = match prepare_as(&exe_dir, true, &|| None, &|| None) {
            Ok(_) => panic!("a data folder that cannot be created must stop the start"),
            Err(error) => error,
        };
        assert!(error.contains("Move the GT4Recomp folder"), "{error}");
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn the_portable_build_never_cleans_an_old_install() {
        assert!(clean_old_install_as(true, Path::new(r"E:\Portable\GT4Recomp")).is_empty());
    }

    fn layout(base: &Path, documents: bool) -> Layout {
        let local = base.join("Local").join("GT4Recomp");
        let player = base.join("Documents").join(paths::USER_FOLDER_NAME);
        if documents {
            std::fs::create_dir_all(&player).unwrap();
        }
        Layout {
            data: DataDirs::new(base.join("data")),
            user_root: base.join("Roaming").join("GT4Recomp"),
            user_source: UserRootSource::AppData,
            legacy: LegacyLocations { local: Some(local), documents_player: Some(player), previous_data: None },
            notices: Vec::new(),
        }
    }

    #[test]
    fn until_the_move_the_1_2_0_places_are_used() {
        let base = std::env::temp_dir().join(format!("gt4l-relocation-legacy-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let with_documents = legacy_user_dirs(&layout(&base, true), "deferred".into());
        assert_eq!(with_documents.saves(), base.join("Documents").join(paths::USER_FOLDER_NAME).join("Saves"));
        assert_eq!(with_documents.config_file(), base.join("Local").join("GT4Recomp").join("config").join("settings.toml"));
        let _ = std::fs::remove_dir_all(&base);
        let without = legacy_user_dirs(&layout(&base, false), "deferred".into());
        assert_eq!(without.saves(), base.join("Local").join("GT4Recomp").join(paths::USER_FOLDER_NAME).join("Saves"));
    }

    #[test]
    fn the_notice_names_what_the_player_should_know() {
        let local = Path::new("L:/GT4Recomp");
        let data = Path::new("P:/GT4Recomp/data");
        let moved = LargeOutcome::Ran(vec![(
            "game files".into(),
            UnitResult::Moved { destination: data.to_path_buf(), renamed: true, bytes: 0, notes: Vec::new() },
        )]);
        assert!(large_notice(&moved, local, data).0.contains("moved to"));
        let failed = LargeOutcome::Ran(vec![("gt4-gs".into(), UnitResult::Failed("locked".into()))]);
        assert!(large_notice(&failed, local, data).1);
        let kept = LargeOutcome::Ran(vec![("game files".into(), UnitResult::Kept("x".into()))]);
        assert!(large_notice(&kept, local, data).0.contains("older copy"));
        assert_eq!(large_notice(&LargeOutcome::Ran(Vec::new()), local, data).0, "");
    }
}
