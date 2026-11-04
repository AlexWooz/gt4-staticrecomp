//! One-time move of the save library from the AppData data folder to `Documents\Gran Turismo 4` (TASK-081, owner
//! 2026-10-21). The move copies, never moves: the old folders are parked, never deleted.
//!
//! Old layout (launchers before 1.0.0, under `%LOCALAPPDATA%\GT4Recomp`):
//!
//! ```text
//! library/memcard/<game save>     the active save        -> Saves/Active/<game save>
//! library/memcard/<replay>        every replay           -> Replays/<replay>
//! library/memcard/.backup/...     harvest copies         -> Saves/.backup/...
//! library/memcard/<other>         anything else          -> Saves/Active/<other>
//! library/saves/<slot>/...        inactive slots         -> Saves/<slot>/...
//! library/replays/<x>, replays/<x> (any replay folder)   -> Replays/<x>
//! library/backups/                card images            stays in AppData (not migrated)
//! ```
//!
//! Rules:
//! - runs only when the old folders hold at least one file and the new `Saves` and `Replays` hold none;
//! - both hold files: nothing is merged or overwritten, the new library stays active, the conflict is reported;
//! - every file is copied to a staging folder beside the target, then checked by size and SHA-256 against its
//!   source; any failure removes the staging copy (only the copy) and leaves the old data untouched;
//! - after the copy is renamed into place, the old folders are renamed into `library.migrated-<timestamp>` in the
//!   data folder. Nothing old is ever deleted;
//! - the result is recorded in a small JSON marker (`config\library-migration.json`); a `migrated` marker stops
//!   later runs.

use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::library::{ACTIVE_BACKUP_FOLDER, ACTIVE_FOLDER};
use crate::names::SaveNames;

/// Schema of the marker file.
pub const MARKER_SCHEMA: u32 = 1;

/// The old library folders, each with the name it gets inside `library.migrated-<timestamp>`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LegacySources {
    /// `library\memcard`: the active save, its `.backup` copies and every replay.
    pub memcard: PathBuf,
    /// `library\saves`: inactive slots.
    pub saves: PathBuf,
    /// Any folder that holds only replays, with its parked name.
    pub replays: Vec<(PathBuf, String)>,
}

impl LegacySources {
    /// The old folders of a data folder: `library\memcard`, `library\saves`, `library\replays` and `replays`.
    pub fn of_data_root(data_root: &Path) -> LegacySources {
        let library = data_root.join("library");
        LegacySources {
            memcard: library.join("memcard"),
            saves: library.join("saves"),
            replays: vec![
                (library.join("replays"), "replays".to_string()),
                (data_root.join("replays"), "replays-data-folder".to_string()),
            ],
        }
    }

    /// Every old folder with its parked name.
    fn folders(&self) -> Vec<(PathBuf, String)> {
        let mut folders = vec![(self.memcard.clone(), "memcard".to_string()), (self.saves.clone(), "saves".to_string())];
        folders.extend(self.replays.iter().cloned());
        folders
    }
}

/// Where the library goes: the `Saves` and `Replays` folders of the new location.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Targets {
    pub saves: PathBuf,
    pub replays: PathBuf,
    /// The names of the game the old library belongs to (Gran Turismo 4 before 1.0.0).
    pub names: SaveNames,
}

/// What a run did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// The old folders hold no file (fresh install, or already moved).
    NothingToMigrate,
    /// The marker says a migration already ran.
    AlreadyMigrated,
    /// Copied and verified; the old folders were parked in `parked`. `park_problems` lists old folders that could
    /// not be renamed (they keep their data; the next run reports a conflict instead of copying again).
    Migrated { files: usize, bytes: u64, parked: PathBuf, park_problems: Vec<String> },
    /// The old and the new location both hold files. Nothing was copied, merged or renamed.
    Conflict { old_files: usize, new_files: usize },
    /// The copy or its check failed. The staging copy was removed; the old folders are untouched.
    Failed(String),
}

impl Outcome {
    /// Status word written to the marker.
    pub fn status(&self) -> &'static str {
        match self {
            Outcome::NothingToMigrate => "nothing-to-migrate",
            Outcome::AlreadyMigrated => "migrated",
            Outcome::Migrated { .. } => "migrated",
            Outcome::Conflict { .. } => "conflict",
            Outcome::Failed(_) => "failed",
        }
    }

    /// One line for the launcher log.
    pub fn describe(&self, targets: &Targets) -> String {
        match self {
            Outcome::NothingToMigrate => "save library: nothing to move from the old AppData library".to_string(),
            Outcome::AlreadyMigrated => "save library: already moved to the Documents library".to_string(),
            Outcome::Migrated { files, bytes, parked, park_problems } => {
                let mut line = format!(
                    "save library: moved {files} files ({bytes} bytes, size and sha256 checked) to {} and {}; old folders kept in {}",
                    targets.saves.display(),
                    targets.replays.display(),
                    parked.display()
                );
                for problem in park_problems {
                    line.push_str(&format!("; {problem}"));
                }
                line
            }
            Outcome::Conflict { old_files, new_files } => format!(
                "save library: WARNING both the old AppData library ({old_files} files) and {} / {} ({new_files} files) hold saves; \
                 nothing was merged or overwritten, the Documents library is used and the old folders are left as they are",
                targets.saves.display(),
                targets.replays.display()
            ),
            Outcome::Failed(error) => format!(
                "save library: WARNING the move to Documents failed and was undone; the old library is untouched: {error}"
            ),
        }
    }
}

/// Copies one file and returns the bytes copied. Tests pass a failing copier.
pub type Copier<'a> = &'a dyn Fn(&Path, &Path) -> io::Result<u64>;

/// The copier used by the launcher.
pub fn copy_file(from: &Path, to: &Path) -> io::Result<u64> {
    fs::copy(from, to)
}

/// Every file under `folder` (recursively), sorted. A missing folder has none.
fn files_under(folder: &Path) -> io::Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    if !folder.is_dir() {
        return Ok(files);
    }
    let mut stack = vec![folder.to_path_buf()];
    while let Some(current) = stack.pop() {
        for entry in fs::read_dir(&current)? {
            let path = entry?.path();
            if path.is_dir() {
                stack.push(path);
            } else {
                files.push(path);
            }
        }
    }
    files.sort();
    Ok(files)
}

/// Number of files under the folders.
fn count_files(folders: &[PathBuf]) -> io::Result<usize> {
    let mut count = 0;
    for folder in folders {
        count += files_under(folder)?.len();
    }
    Ok(count)
}

/// The (source file, destination file) pairs, destinations under the staging folders.
fn plan(sources: &LegacySources, names: SaveNames, saves_stage: &Path, replays_stage: &Path) -> Result<Vec<(PathBuf, PathBuf)>, String> {
    fn add_tree(pairs: &mut Vec<(PathBuf, PathBuf)>, from: &Path, to: &Path) -> Result<(), String> {
        for file in files_under(from).map_err(|error| format!("{}: {error}", from.display()))? {
            let relative = file.strip_prefix(from).expect("file under its folder");
            pairs.push((file.clone(), to.join(relative)));
        }
        Ok(())
    }
    let mut pairs: Vec<(PathBuf, PathBuf)> = Vec::new();
    if sources.memcard.is_dir() {
        let entries = fs::read_dir(&sources.memcard).map_err(|error| format!("{}: {error}", sources.memcard.display()))?;
        let mut entries: Vec<PathBuf> = entries.filter_map(|entry| entry.ok()).map(|entry| entry.path()).collect();
        entries.sort();
        for entry in entries {
            let name = entry.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
            let destination = if name == ACTIVE_BACKUP_FOLDER {
                saves_stage.join(ACTIVE_BACKUP_FOLDER)
            } else if names.is_replay(&name) {
                replays_stage.join(&name)
            } else {
                // The game save, and anything else the old folder card held, goes to Active.
                saves_stage.join(ACTIVE_FOLDER).join(&name)
            };
            if entry.is_dir() {
                add_tree(&mut pairs, &entry, &destination)?;
            } else {
                pairs.push((entry.clone(), destination));
            }
        }
    }
    add_tree(&mut pairs, &sources.saves, saves_stage)?;
    for (folder, _) in &sources.replays {
        add_tree(&mut pairs, folder, replays_stage)?;
    }
    let mut destinations: Vec<&PathBuf> = pairs.iter().map(|(_, to)| to).collect();
    destinations.sort();
    if let Some(pair) = destinations.windows(2).find(|pair| pair[0] == pair[1]) {
        return Err(format!("two old files would land on {}; nothing was moved", pair[0].display()));
    }
    Ok(pairs)
}

/// SHA-256 and size of a file.
fn digest(path: &Path) -> io::Result<([u8; 32], u64)> {
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 16];
    let mut size = 0u64;
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        size += read as u64;
    }
    Ok((hasher.finalize().into(), size))
}

/// Removes a folder tree that holds no file (only empty folders). Fails, removing nothing more, when a file exists.
fn remove_empty_tree(folder: &Path) -> io::Result<()> {
    if !folder.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(folder)? {
        let path = entry?.path();
        if path.is_dir() {
            remove_empty_tree(&path)?;
        } else {
            return Err(io::Error::other(format!("{} is not empty", folder.display())));
        }
    }
    fs::remove_dir(folder)
}

/// Staging folder beside `target`: `.<name>.migrating-<stamp>`.
fn staging_for(target: &Path, stamp: &str) -> PathBuf {
    let name = target.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_else(|| "library".into());
    target.with_file_name(format!(".{name}.migrating-{stamp}"))
}

/// Reads the marker's status, if the marker exists and is readable.
pub fn marker_status(marker: &Path) -> Option<String> {
    let text = fs::read_to_string(marker).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    value.get("status")?.as_str().map(str::to_string)
}

/// Writes the marker through a temporary file and a rename.
fn write_marker(marker: &Path, outcome: &Outcome, sources: &LegacySources, targets: &Targets, stamp: &str) -> io::Result<()> {
    let mut value = serde_json::json!({
        "schema": MARKER_SCHEMA,
        "status": outcome.status(),
        "at_utc": stamp,
        "from": [sources.memcard.display().to_string(), sources.saves.display().to_string()],
        "to": { "saves": targets.saves.display().to_string(), "replays": targets.replays.display().to_string() },
    });
    match outcome {
        Outcome::Migrated { files, bytes, parked, park_problems } => {
            value["files"] = (*files).into();
            value["bytes"] = (*bytes).into();
            value["parked"] = parked.display().to_string().into();
            value["park_problems"] = park_problems.clone().into();
        }
        Outcome::Conflict { old_files, new_files } => {
            value["old_files"] = (*old_files).into();
            value["new_files"] = (*new_files).into();
        }
        Outcome::Failed(error) => value["error"] = error.clone().into(),
        Outcome::NothingToMigrate | Outcome::AlreadyMigrated => {}
    }
    if let Some(parent) = marker.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = marker.with_extension("json.tmp");
    fs::write(&temporary, serde_json::to_string_pretty(&value).unwrap_or_default())?;
    fs::rename(&temporary, marker)
}

/// Runs the migration once. `data_root` is the AppData data folder (the parked folder goes there), `marker` the
/// JSON marker, `stamp` the UTC timestamp used in folder names. Never panics; every problem is in the outcome.
pub fn migrate(sources: &LegacySources, targets: &Targets, data_root: &Path, marker: &Path, stamp: &str, copier: Copier<'_>) -> Outcome {
    if marker_status(marker).as_deref() == Some("migrated") {
        return Outcome::AlreadyMigrated;
    }
    let old_folders: Vec<PathBuf> = sources.folders().into_iter().map(|(folder, _)| folder).collect();
    let old_files = match count_files(&old_folders) {
        Ok(count) => count,
        Err(error) => return record(marker, Outcome::Failed(format!("old library unreadable: {error}")), sources, targets, stamp),
    };
    if old_files == 0 {
        return Outcome::NothingToMigrate;
    }
    let new_files = match count_files(&[targets.saves.clone(), targets.replays.clone()]) {
        Ok(count) => count,
        Err(error) => return record(marker, Outcome::Failed(format!("new library unreadable: {error}")), sources, targets, stamp),
    };
    if new_files > 0 {
        return record(marker, Outcome::Conflict { old_files, new_files }, sources, targets, stamp);
    }
    let outcome = match copy_and_swap(sources, targets, data_root, stamp, copier) {
        Ok(outcome) => outcome,
        Err(error) => Outcome::Failed(error),
    };
    record(marker, outcome, sources, targets, stamp)
}

fn record(marker: &Path, outcome: Outcome, sources: &LegacySources, targets: &Targets, stamp: &str) -> Outcome {
    if let Err(error) = write_marker(marker, &outcome, sources, targets, stamp) {
        if let Outcome::Migrated { files, bytes, parked, mut park_problems } = outcome {
            park_problems.push(format!("marker not written: {error}"));
            return Outcome::Migrated { files, bytes, parked, park_problems };
        }
    }
    outcome
}

fn copy_and_swap(sources: &LegacySources, targets: &Targets, data_root: &Path, stamp: &str, copier: Copier<'_>) -> Result<Outcome, String> {
    let saves_stage = staging_for(&targets.saves, stamp);
    let replays_stage = staging_for(&targets.replays, stamp);
    let rollback = || {
        // Only the staging copies made by this run are removed; the old folders are never touched here.
        let _ = fs::remove_dir_all(&saves_stage);
        let _ = fs::remove_dir_all(&replays_stage);
    };
    let pairs = plan(sources, targets.names, &saves_stage, &replays_stage)?;
    let mut bytes = 0u64;
    let copy_all = || -> Result<u64, String> {
        fs::create_dir_all(&saves_stage).map_err(|error| format!("{}: {error}", saves_stage.display()))?;
        fs::create_dir_all(&replays_stage).map_err(|error| format!("{}: {error}", replays_stage.display()))?;
        let mut total = 0u64;
        for (from, to) in &pairs {
            if let Some(parent) = to.parent() {
                fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
            }
            copier(from, to).map_err(|error| format!("copy {} -> {}: {error}", from.display(), to.display()))?;
            let (source_hash, source_size) = digest(from).map_err(|error| format!("{}: {error}", from.display()))?;
            let (copy_hash, copy_size) = digest(to).map_err(|error| format!("{}: {error}", to.display()))?;
            if source_size != copy_size || source_hash != copy_hash {
                return Err(format!("{} does not match its source after the copy (size or sha256)", to.display()));
            }
            total += source_size;
        }
        Ok(total)
    };
    match copy_all() {
        Ok(total) => bytes += total,
        Err(error) => {
            rollback();
            return Err(error);
        }
    }
    // Swap in: the targets hold no file (checked before), so only empty folders are removed.
    for target in [&targets.saves, &targets.replays] {
        if let Err(error) = remove_empty_tree(target) {
            rollback();
            return Err(format!("{}: {error}", target.display()));
        }
    }
    if let Err(error) = fs::rename(&saves_stage, &targets.saves) {
        rollback();
        return Err(format!("{}: {error}", targets.saves.display()));
    }
    if let Err(error) = fs::rename(&replays_stage, &targets.replays) {
        // Undo the first rename so the run leaves nothing half done.
        let _ = fs::rename(&targets.saves, &saves_stage);
        rollback();
        return Err(format!("{}: {error}", targets.replays.display()));
    }
    // Park the old folders: renamed, never deleted.
    let parked = data_root.join(format!("library.migrated-{stamp}"));
    let mut park_problems = Vec::new();
    if let Err(error) = fs::create_dir_all(&parked) {
        park_problems.push(format!("{}: {error}", parked.display()));
    } else {
        for (folder, name) in sources.folders() {
            if folder.exists() {
                if let Err(error) = fs::rename(&folder, parked.join(&name)) {
                    park_problems.push(format!("{} was not renamed ({error}); it keeps its data", folder.display()));
                }
            }
        }
    }
    Ok(Outcome::Migrated { files: pairs.len(), bytes, parked, park_problems })
}
