//! The move from the 1.2.0 layout to the 1.2.1 layout (TASK-091, owner 2026-10-22). The launcher does it at its
//! start, running as the player, never the installer. See [`crate::paths`] for the two layouts.
//!
//! **User data** ([`migrate_user_data`]): settings, controls and card backups from `%LOCALAPPDATA%\GT4Recomp`, and
//! the save library (`Saves`, `Replays`) from `Documents\Gran Turismo 4` (and from the 1.2.0 data-folder fallback
//! `%LOCALAPPDATA%\GT4Recomp\Gran Turismo 4`), go to `%APPDATA%\GT4Recomp`.
//! - Every file is copied to a staging folder in the destination and checked byte for byte (size and SHA-256)
//!   against its source before anything is placed. A failure removes the staging copy and nothing else, so the old
//!   places stay in use for this start and the move runs again at the next one.
//! - Old files are never deleted. After the marker is written, the old player folder is renamed to
//!   `Gran Turismo 4 (moved to AppData)` with a `README.txt` that says where the files went.
//! - A destination item that already exists is never overwritten: identical content counts as done (a resumed
//!   run); different content keeps both (a save goes to the next free `Save NN` slot, anything else under
//!   `Kept from 1.2.0`), and the log says so.
//!
//! **Large data** ([`migrate_large_data`]): the disc copy and prepared files (one unit, they belong together),
//! gt4-gs, the runtime card, sessions and logs move from `%LOCALAPPDATA%\GT4Recomp` to `<install>\data`; custom
//! textures move from `Documents\Gran Turismo 4\Textures` to `<install>\data\Textures`.
//! - Same volume: one rename per unit. Another volume (`ERROR_NOT_SAME_DEVICE`): copy into a staging folder with
//!   progress, keep the modified times (the setup receipt records the image's), check size and SHA-256 of every
//!   file, let the caller check the game files (the pinned disc hash), rename into place, then delete the source.
//! - A unit whose destination already has files is left where it is (logged); nothing is overwritten.
//! - A failed unit keeps its old place in use and is tried again at the next start; the marker is written only when
//!   no unit failed.
//!
//! Neither move runs while the game runs.
use crate::language::Language;
use crate::messages;


use std::collections::BTreeMap;
use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::paths::{LegacyLocations, UserDirs, MOVED_USER_FOLDER_NAME, TEXTURES_FOLDER_NAME};

/// Folder of the active save in `Saves` (`gt4saves::library::ACTIVE_FOLDER`).
const ACTIVE_FOLDER: &str = "Active";

/// `Save 01`, `Save 02`, ... (`gt4saves::library::slot_name`).
fn slot_name(number: u32) -> String {
    format!("Save {number:02}")
}

/// Whether a folder name is an inactive save slot (`Save NN`).
fn is_slot_name(name: &str) -> bool {
    name.strip_prefix("Save ").is_some_and(|number| !number.is_empty() && number.chars().all(|c| c.is_ascii_digit()))
}

/// `ERROR_NOT_SAME_DEVICE`: a rename across volumes. The move then copies.
pub const ERROR_NOT_SAME_DEVICE: i32 = 17;

/// Schema of both marker files.
pub const MARKER_SCHEMA: u32 = 1;

/// Folder of the user data root that receives items whose place was already taken.
pub const KEPT_FOLDER_NAME: &str = "Kept from 1.2.0";

/// Name of the note left in a renamed player folder.
pub const README_NAME: &str = "README.txt";

/// The file operations a move uses, so tests can fail them: a rename that reports another volume, or a read that
/// fails like an offline OneDrive placeholder, a locked file or a full disk.
pub struct FsOps<'a> {
    pub rename: &'a dyn Fn(&Path, &Path) -> io::Result<()>,
    pub open: &'a dyn Fn(&Path) -> io::Result<Box<dyn Read>>,
}

/// Whether a rename error is the transient kind a scanner, the indexer or Explorer can cause on a freshly written
/// file (Windows sharing/lock violation, access denied while a scan holds the file). Retried a few times; a real
/// failure is still reported to the caller, which keeps the old place in use.
fn transient_rename_error(error: &io::Error) -> bool {
    matches!(error.raw_os_error(), Some(5) | Some(32) | Some(33))
}

fn real_rename(from: &Path, to: &Path) -> io::Result<()> {
    let mut last = None;
    for attempt in 0..5u32 {
        match fs::rename(from, to) {
            Ok(()) => return Ok(()),
            Err(error) if transient_rename_error(&error) => {
                last = Some(error);
                std::thread::sleep(std::time::Duration::from_millis(20 * u64::from(attempt + 1)));
            }
            Err(error) => return Err(error),
        }
    }
    Err(last.expect("a retryable rename error"))
}

fn real_open(path: &Path) -> io::Result<Box<dyn Read>> {
    Ok(Box::new(fs::File::open(path)?))
}

impl FsOps<'static> {
    /// The operations of the launcher.
    pub fn real() -> FsOps<'static> {
        FsOps { rename: &real_rename, open: &real_open }
    }
}

// Helpers -----------------------------------------------------------------------------------------------------------

/// Every file under `path` (recursively), sorted; a file is its own list; a missing path has none.
fn files_under(path: &Path) -> io::Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    if path.is_file() {
        files.push(path.to_path_buf());
        return Ok(files);
    }
    if !path.is_dir() {
        return Ok(files);
    }
    let mut stack = vec![path.to_path_buf()];
    while let Some(current) = stack.pop() {
        for entry in fs::read_dir(&current)? {
            let entry = entry?;
            let file_type = entry.file_type()?;
            if file_type.is_dir() {
                stack.push(entry.path());
            } else {
                files.push(entry.path());
            }
        }
    }
    files.sort();
    Ok(files)
}

/// Whether `path` is a file or a folder that holds at least one file. Unreadable counts as holding files (so
/// nothing is ever placed over it).
pub(crate) fn has_files(path: &Path) -> bool {
    match files_under(path) {
        Ok(files) => !files.is_empty(),
        Err(_) => path.exists(),
    }
}

/// Removes a folder tree that holds no file (the old data folder after its move). Fails, removing nothing more, when
/// a file exists. A missing folder is done.
pub fn remove_empty_folder(folder: &Path) -> io::Result<()> {
    remove_empty_tree(folder)
}

/// Removes a folder tree that holds no file. Fails, removing nothing more, when a file exists.
fn remove_empty_tree(folder: &Path) -> io::Result<()> {
    if !folder.exists() {
        return Ok(());
    }
    if folder.is_file() {
        return Err(io::Error::other(format!("{} is a file", folder.display())));
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

/// Size and SHA-256 (lower-case hex) of a file read through std.
fn digest(path: &Path) -> io::Result<(u64, String)> {
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 20];
    let mut size = 0u64;
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        size += read as u64;
    }
    Ok((size, hex(&hasher.finalize())))
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// Relative path -> (size, SHA-256) of every file under `path`.
fn tree_digest(path: &Path) -> io::Result<BTreeMap<PathBuf, (u64, String)>> {
    let mut map = BTreeMap::new();
    for file in files_under(path)? {
        // A single file is keyed by an empty path, so two files compare by content whatever their names.
        let relative = file.strip_prefix(path).unwrap_or(&file).to_path_buf();
        map.insert(relative, digest(&file)?);
    }
    Ok(map)
}

/// Total size of the files under `path`.
fn tree_size(path: &Path) -> u64 {
    files_under(path).unwrap_or_default().iter().filter_map(|file| fs::metadata(file).ok()).map(|meta| meta.len()).sum()
}

/// `path`, or `path 2`, `path 3`, ... : the first name that does not exist yet.
fn unique_name(path: &Path) -> PathBuf {
    if !path.exists() {
        return path.to_path_buf();
    }
    let name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    (2..).map(|number| path.with_file_name(format!("{name} {number}"))).find(|candidate| !candidate.exists()).expect("a free name")
}

/// The result of a verified copy: bytes copied and the SHA-256 of every file, by path relative to the copy.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CopyReport {
    pub files: usize,
    pub bytes: u64,
    pub sha256: BTreeMap<PathBuf, String>,
}

/// Copies `source` (a file or a folder) to `target` and checks every copy against its source: size and SHA-256 of
/// what was read equal size and SHA-256 of what is on disk afterwards. Modified times are kept. `progress` gets the
/// bytes copied so far. The first problem stops the copy; the caller removes `target`.
fn copy_verified(source: &Path, target: &Path, ops: &FsOps<'_>, progress: &mut dyn FnMut(u64)) -> Result<CopyReport, String> {
    let files = files_under(source).map_err(|error| format!("{}: {error}", source.display()))?;
    let mut report = CopyReport::default();
    let mut buffer = vec![0u8; 1 << 20];
    for file in files {
        let relative = if source.is_file() {
            PathBuf::new()
        } else {
            file.strip_prefix(source).expect("file under its folder").to_path_buf()
        };
        let destination = if relative.as_os_str().is_empty() { target.to_path_buf() } else { target.join(&relative) };
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
        }
        let mut reader = (ops.open)(&file).map_err(|error| format!("read {}: {error}", file.display()))?;
        let mut writer = fs::File::create(&destination).map_err(|error| format!("{}: {error}", destination.display()))?;
        let mut hasher = Sha256::new();
        let mut size = 0u64;
        loop {
            let read = reader.read(&mut buffer).map_err(|error| format!("read {}: {error}", file.display()))?;
            if read == 0 {
                break;
            }
            hasher.update(&buffer[..read]);
            writer.write_all(&buffer[..read]).map_err(|error| format!("write {}: {error}", destination.display()))?;
            size += read as u64;
            progress(report.bytes + size);
        }
        writer.sync_all().map_err(|error| format!("write {}: {error}", destination.display()))?;
        drop(writer);
        let source_hash = hex(&hasher.finalize());
        if let Ok(modified) = fs::metadata(&file).and_then(|meta| meta.modified()) {
            let applied = fs::OpenOptions::new().write(true).open(&destination).and_then(|handle| handle.set_modified(modified));
            applied.map_err(|error| format!("{}: modified time not kept: {error}", destination.display()))?;
        }
        let (copy_size, copy_hash) = digest(&destination).map_err(|error| format!("{}: {error}", destination.display()))?;
        if copy_size != size || copy_hash != source_hash {
            return Err(format!("{} does not match its source after the copy (size or sha256)", destination.display()));
        }
        report.files += 1;
        report.bytes += size;
        report.sha256.insert(relative, source_hash);
    }
    Ok(report)
}

pub(crate) fn stamp_json(stamp: &str) -> serde_json::Value {
    serde_json::Value::String(stamp.to_string())
}

pub(crate) fn write_marker(marker: &Path, value: &serde_json::Value) -> Result<(), String> {
    if let Some(parent) = marker.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    let temporary = marker.with_extension("json.tmp");
    fs::write(&temporary, serde_json::to_string_pretty(value).unwrap_or_default()).map_err(|error| format!("{}: {error}", temporary.display()))?;
    fs::rename(&temporary, marker).map_err(|error| format!("{}: {error}", marker.display()))
}

fn read_marker(marker: &Path) -> Option<serde_json::Value> {
    serde_json::from_str(&fs::read_to_string(marker).ok()?).ok()
}

/// The status a marker records (`migrated`), if the marker exists and is readable.
pub fn marker_status(marker: &Path) -> Option<String> {
    read_marker(marker)?.get("status")?.as_str().map(str::to_string)
}

// User data -------------------------------------------------------------------------------------------------------

/// What a user-data item is, for the keep-both rule.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ItemKind {
    /// `Saves\Active` or `Saves\Save NN`: a taken place sends it to the next free slot.
    SaveSlot,
    /// Anything else: a taken place sends it to `Kept from 1.2.0`.
    Other,
}

#[derive(Clone, Debug)]
struct Item {
    source: PathBuf,
    /// Place under the user data root.
    target: PathBuf,
    kind: ItemKind,
}

/// The inputs of the user-data move.
pub struct UserMove<'a> {
    pub legacy: &'a LegacyLocations,
    /// The user data in its 1.2.1 place (`%APPDATA%\GT4Recomp`).
    pub user: &'a UserDirs,
    /// The large data folder (named in the README note for the textures).
    pub data_root: &'a Path,
    pub game_running: bool,
    /// UTC time stamp for the marker.
    pub stamp: &'a str,
}

/// What the user-data move did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UserOutcome {
    /// No 1.2.0 user data on this PC (a fresh install, or a new PC).
    NothingToMove,
    /// The marker says the move happened before. `notes` reports a pending folder rename tried again.
    AlreadyMoved { notes: Vec<String> },
    /// The game runs; the old places are used this time.
    Deferred(String),
    /// Copied, checked and placed. `kept_both` lists items whose place was taken; `notes` the folder renames.
    Moved { files: usize, bytes: u64, kept_both: Vec<String>, notes: Vec<String> },
    /// Nothing was placed (or a resumed run will finish it); the old places are used this time.
    Failed(String),
}

impl UserOutcome {
    /// Whether the launcher uses `%APPDATA%\GT4Recomp` after this outcome (otherwise the 1.2.0 places).
    pub fn uses_new_place(&self) -> bool {
        matches!(self, UserOutcome::NothingToMove | UserOutcome::AlreadyMoved { .. } | UserOutcome::Moved { .. })
    }

    /// Lines for the launcher log.
    pub fn log_lines(&self, user: &UserDirs) -> Vec<String> {
        let root = user.root().display();
        match self {
            UserOutcome::NothingToMove => vec![format!("user data: nothing from 1.2.0 to move; using {root}")],
            UserOutcome::AlreadyMoved { notes } => {
                let mut lines = vec![format!("user data: moved before; using {root}")];
                lines.extend(notes.iter().map(|note| format!("user data: {note}")));
                lines
            }
            UserOutcome::Deferred(reason) => vec![format!("user data: not moved now ({reason}); the 1.2.0 places are used")],
            UserOutcome::Moved { files, bytes, kept_both, notes } => {
                let mut lines = vec![format!("user data: moved {files} files ({bytes} bytes, size and sha256 checked) to {root}")];
                lines.extend(kept_both.iter().map(|line| format!("user data: kept both: {line}")));
                lines.extend(notes.iter().map(|note| format!("user data: {note}")));
                lines
            }
            UserOutcome::Failed(error) => vec![format!(
                "user data: WARNING the move to {root} failed and was undone; the 1.2.0 places are used and the move runs again at the next start: {error}"
            )],
        }
    }

    /// A sentence for the launcher window, when there is something to say (English).
    pub fn notice(&self, user: &UserDirs) -> Option<String> {
        self.notice_in(user, Language::fallback())
    }

    /// [`UserOutcome::notice`] in `language`.
    pub fn notice_in(&self, user: &UserDirs, language: Language) -> Option<String> {
        let root = user.root().display().to_string();
        match self {
            UserOutcome::Moved { kept_both, .. } if kept_both.is_empty() => {
                Some(messages::render("move.done", language, &[("root", &root)]))
            }
            UserOutcome::Moved { kept_both, .. } => Some(messages::render(
                "move.done_kept",
                language,
                &[("root", &root), ("count", &kept_both.len().to_string())],
            )),
            UserOutcome::Deferred(_) => Some(messages::text("move.close_game", language).to_string()),
            UserOutcome::Failed(_) => Some(messages::text("move.saves_later", language).to_string()),
            _ => None,
        }
    }
}

/// The user-data items of the 1.2.0 places, each with its place under the user data root. Items with no file are
/// left out (the library creates empty `Active` and `.backup` folders).
fn user_items(legacy: &LegacyLocations) -> Result<Vec<Item>, String> {
    let mut items = Vec::new();
    let entries = |folder: &Path| -> Result<Vec<PathBuf>, String> {
        if !folder.is_dir() {
            return Ok(Vec::new());
        }
        let mut list: Vec<PathBuf> = fs::read_dir(folder)
            .map_err(|error| format!("{}: {error}", folder.display()))?
            .filter_map(|entry| entry.ok().map(|entry| entry.path()))
            .collect();
        list.sort();
        Ok(list)
    };
    let name_of = |path: &Path| path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    if let Some(local) = &legacy.local {
        for entry in entries(&local.join("config"))? {
            items.push(Item { target: Path::new("config").join(name_of(&entry)), source: entry, kind: ItemKind::Other });
        }
        for entry in entries(local)? {
            let name = name_of(&entry);
            if entry.is_file() && name.to_ascii_lowercase().starts_with("controls.toml") {
                items.push(Item { target: PathBuf::from(&name), source: entry, kind: ItemKind::Other });
            }
        }
        for entry in entries(&local.join("library").join("backups"))? {
            items.push(Item { target: Path::new("backups").join(name_of(&entry)), source: entry, kind: ItemKind::Other });
        }
    }
    for player in [legacy.documents_player.clone(), legacy.local_player()].into_iter().flatten() {
        for entry in entries(&player.join("Saves"))? {
            let name = name_of(&entry);
            let kind = if name == ACTIVE_FOLDER || is_slot_name(&name) { ItemKind::SaveSlot } else { ItemKind::Other };
            items.push(Item { target: Path::new("Saves").join(&name), source: entry, kind });
        }
        for entry in entries(&player.join("Replays"))? {
            items.push(Item { target: Path::new("Replays").join(name_of(&entry)), source: entry, kind: ItemKind::Other });
        }
    }
    items.retain(|item| has_files(&item.source));
    Ok(items)
}

/// The player folders that hold items of the move (they are renamed afterwards).
fn player_folders_with_items(legacy: &LegacyLocations, items: &[Item]) -> Vec<PathBuf> {
    [legacy.documents_player.clone(), legacy.local_player()]
        .into_iter()
        .flatten()
        .filter(|player| items.iter().any(|item| item.source.starts_with(player)))
        .collect()
}

/// The next `Save NN` that is free in `saves`.
fn next_free_slot(saves: &Path) -> PathBuf {
    let used: Vec<u32> = fs::read_dir(saves)
        .map(|entries| {
            entries
                .filter_map(|entry| entry.ok())
                .filter_map(|entry| entry.file_name().to_string_lossy().strip_prefix("Save ").and_then(|number| number.parse().ok()))
                .collect()
        })
        .unwrap_or_default();
    let mut number = used.iter().copied().max().unwrap_or(0) + 1;
    while saves.join(slot_name(number)).exists() {
        number += 1;
    }
    saves.join(slot_name(number))
}

/// The note left in a renamed player folder.
pub fn readme_text(user: &UserDirs, data_root: &Path) -> String {
    format!(
        "GT4Recomp 1.2.1 moved the files of this folder.\r\n\
         \r\n\
         - Saves and replays: {}\r\n\
         - Custom textures: {}\r\n\
         \r\n\
         The launcher now uses those folders. This folder is the old copy, kept for safety: nothing in it was deleted.\r\n\
         Once your saves show in the launcher (Saves page), you may delete this folder.\r\n",
        user.root().display(),
        data_root.join(TEXTURES_FOLDER_NAME).display()
    )
}

/// Renames a moved player folder to `Gran Turismo 4 (moved to AppData)` and writes the note in it.
fn park_player_folder(player: &Path, user: &UserDirs, data_root: &Path) -> Result<PathBuf, String> {
    let parked = unique_name(&player.with_file_name(MOVED_USER_FOLDER_NAME));
    fs::rename(player, &parked).map_err(|error| format!("{} was not renamed: {error}", player.display()))?;
    fs::write(parked.join(README_NAME), readme_text(user, data_root)).map_err(|error| format!("{}: {error}", parked.join(README_NAME).display()))?;
    Ok(parked)
}

/// Retries the folder renames a marker lists as pending; returns notes and the folders still pending.
fn retry_parking(pending: Vec<PathBuf>, user: &UserDirs, data_root: &Path) -> (Vec<String>, Vec<PathBuf>) {
    let mut notes = Vec::new();
    let mut still = Vec::new();
    for player in pending {
        if !player.exists() {
            continue;
        }
        match park_player_folder(&player, user, data_root) {
            Ok(parked) => notes.push(format!("old folder {} renamed to {}", player.display(), parked.display())),
            Err(error) => {
                notes.push(format!("{error}; it is no longer used and is renamed at a later start"));
                still.push(player);
            }
        }
    }
    (notes, still)
}

fn pending_list(value: &serde_json::Value) -> Vec<PathBuf> {
    value
        .get("rename_pending")
        .and_then(|list| list.as_array())
        .map(|list| list.iter().filter_map(|item| item.as_str()).map(PathBuf::from).collect())
        .unwrap_or_default()
}

/// Moves the 1.2.0 user data to the 1.2.1 place. Never panics; every problem is in the outcome.
pub fn migrate_user_data(request: &UserMove<'_>, ops: &FsOps<'_>) -> UserOutcome {
    let user = request.user;
    let marker = user.relocation_marker();
    if let Some(mut value) = read_marker(&marker).filter(|value| value.get("status").and_then(|s| s.as_str()) == Some("migrated")) {
        let pending = pending_list(&value);
        if pending.is_empty() {
            return UserOutcome::AlreadyMoved { notes: Vec::new() };
        }
        let (notes, still) = retry_parking(pending, user, request.data_root);
        value["rename_pending"] = still.iter().map(|path| path.display().to_string()).collect::<Vec<_>>().into();
        let _ = write_marker(&marker, &value);
        return UserOutcome::AlreadyMoved { notes };
    }
    let items = match user_items(request.legacy) {
        Ok(items) => items,
        Err(error) => return UserOutcome::Failed(format!("the 1.2.0 folders cannot be read: {error}")),
    };
    if items.is_empty() {
        return UserOutcome::NothingToMove;
    }
    if request.game_running {
        return UserOutcome::Deferred("Gran Turismo 4 is running".to_string());
    }
    let staging = user.root().join(".migrating-1.2.1");
    let _ = fs::remove_dir_all(&staging);
    if let Err(error) = fs::create_dir_all(&staging) {
        return UserOutcome::Failed(format!("{}: {error}", staging.display()));
    }
    // 1. Copy every item into the staging folder and check it. Nothing is placed before all of it passed.
    let mut files = 0usize;
    let mut bytes = 0u64;
    for (index, item) in items.iter().enumerate() {
        match copy_verified(&item.source, &staging.join(index.to_string()), ops, &mut |_| {}) {
            Ok(report) => {
                files += report.files;
                bytes += report.bytes;
            }
            Err(error) => {
                let _ = fs::remove_dir_all(&staging);
                return UserOutcome::Failed(error);
            }
        }
    }
    // 2. Place each item: free places first, then the items whose place is taken (never overwritten), so a kept
    //    save takes the next slot after every slot the move brings.
    let mut kept_both = Vec::new();
    let mut taken = Vec::new();
    // Every destination this run placed, with its staging path. A failed placement puts them back (`undo_placements`).
    let mut placements: Vec<(PathBuf, PathBuf)> = Vec::new();
    for (index, item) in items.iter().enumerate() {
        let staged = staging.join(index.to_string());
        let destination = user.root().join(&item.target);
        if has_files(&destination) {
            if matches!((tree_digest(&staged), tree_digest(&destination)), (Ok(left), Ok(right)) if left == right) {
                continue; // placed by an earlier run that stopped before its marker; this run did not place it
            }
            taken.push(index);
            continue;
        }
        let placed = remove_empty_tree(&destination)
            .and_then(|_| destination.parent().map(fs::create_dir_all).unwrap_or(Ok(())))
            .and_then(|_| (ops.rename)(&staged, &destination));
        if let Err(error) = placed {
            undo_placements(&placements, &staging, ops);
            return UserOutcome::Failed(format!("{}: {error}", destination.display()));
        }
        placements.push((destination, staged));
    }
    for index in taken {
        let item = &items[index];
        let destination = user.root().join(&item.target);
        let alternative = match item.kind {
            // The 1.2.1 move writes the flat 1.2.x places; `games_layout` moves them into `games/gt4` afterwards.
            ItemKind::SaveSlot => next_free_slot(&user.root().join("Saves")),
            ItemKind::Other => unique_name(&user.root().join(KEPT_FOLDER_NAME).join(&item.target)),
        };
        let staged = staging.join(index.to_string());
        let placed = alternative
            .parent()
            .map(fs::create_dir_all)
            .unwrap_or(Ok(()))
            .and_then(|_| (ops.rename)(&staged, &alternative));
        if let Err(error) = placed {
            undo_placements(&placements, &staging, ops);
            return UserOutcome::Failed(format!("{}: {error}", alternative.display()));
        }
        placements.push((alternative.clone(), staged));
        kept_both.push(format!(
            "{} already existed; the 1.2.0 copy from {} is in {}",
            destination.display(),
            item.source.display(),
            alternative.display()
        ));
    }
    let _ = fs::remove_dir_all(&staging);
    // 3. The marker, then the old player folders are renamed (never deleted).
    let players = player_folders_with_items(request.legacy, &items);
    let mut value = serde_json::json!({
        "schema": MARKER_SCHEMA,
        "status": "migrated",
        "at_utc": stamp_json(request.stamp),
        "files": files,
        "bytes": bytes,
        "kept_both": kept_both,
        "sources": items.iter().map(|item| item.source.display().to_string()).collect::<Vec<_>>(),
        "rename_pending": players.iter().map(|path| path.display().to_string()).collect::<Vec<_>>(),
    });
    if let Err(error) = write_marker(&marker, &value) {
        // Without a marker the next run finds every item in place (identical) and writes it then.
        return UserOutcome::Moved { files, bytes, kept_both, notes: vec![format!("marker not written: {error}")] };
    }
    let (notes, still) = retry_parking(players, user, request.data_root);
    value["rename_pending"] = still.iter().map(|path| path.display().to_string()).collect::<Vec<_>>().into();
    let _ = write_marker(&marker, &value);
    UserOutcome::Moved { files, bytes, kept_both, notes }
}

/// Puts back what this run placed after a failed placement, so the new user folder holds none of its items. The
/// sources were never touched and the staged copies are verified, so a destination that cannot be renamed back is
/// removed. The staging folder is removed by the caller.
fn undo_placements(placements: &[(PathBuf, PathBuf)], staging: &Path, ops: &FsOps<'_>) {
    for (destination, staged) in placements.iter().rev() {
        if (ops.rename)(destination, staged).is_ok() {
            continue;
        }
        let _ = if destination.is_dir() { fs::remove_dir_all(destination) } else { fs::remove_file(destination) };
    }
    let _ = fs::remove_dir_all(staging);
}

// Large data ------------------------------------------------------------------------------------------------------

/// Progress of the large-data move, for the launcher window.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LargeProgress {
    /// The unit being moved (`game files`, `gt4-gs`, `textures`, ...).
    pub item: String,
    pub bytes_done: u64,
    pub bytes_total: u64,
}

/// What happened to one unit.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum UnitResult {
    /// The 1.2.0 folder holds nothing of it.
    Absent,
    /// Moved to `destination` (by a rename, or by a checked copy across volumes). `notes` lists source leftovers.
    Moved { destination: PathBuf, renamed: bool, bytes: u64, notes: Vec<String> },
    /// Left where it is: the destination already has it. The text says where the old copy is.
    Kept(String),
    /// Not moved; the old place stays in use and the move runs again at the next start.
    Failed(String),
}

/// The inputs of the large-data move.
pub struct LargeMove<'a> {
    /// The folder the large data moves from, in the layout of 1.2.0 (`%LOCALAPPDATA%\GT4Recomp`) or of an earlier
    /// 1.2.1 install's data folder (`PreviousDataDir`): disc, prepared, gs, runtime, sessions, logs, Textures.
    pub local: &'a Path,
    /// The 1.2.1 data folder (`<install>\data`).
    pub data_root: &'a Path,
    /// Where custom textures may be: under the source folder (`Textures`) or, for 1.2.0, in `Documents`.
    pub textures_sources: Vec<PathBuf>,
    /// The folder under `logs` that keeps a source's logs, sessions and runtime card when the new folder has its own
    /// (`from-1.2.0`, or `from-previous-install`).
    pub old_copy_folder: &'a str,
    pub marker: PathBuf,
    pub game_running: bool,
    pub stamp: &'a str,
    /// Checks game files placed under a folder (`prepared` and `disc` side by side), given the SHA-256 of the files
    /// copied (empty after a rename). The launcher checks the setup receipt and the pinned disc hash.
    pub check_game_files: &'a dyn Fn(&Path, &BTreeMap<PathBuf, String>) -> Result<(), String>,
}

/// What the large-data move did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LargeOutcome {
    /// The marker says the move happened before.
    AlreadyMoved,
    /// The game runs; nothing moved.
    Deferred(String),
    /// One result per unit, in order.
    Ran(Vec<(String, UnitResult)>),
}

impl LargeOutcome {
    pub fn units(&self) -> &[(String, UnitResult)] {
        match self {
            LargeOutcome::Ran(units) => units,
            _ => &[],
        }
    }

    pub fn failed(&self) -> bool {
        self.units().iter().any(|(_, result)| matches!(result, UnitResult::Failed(_)))
    }

    /// Whether anything moved.
    pub fn moved(&self) -> bool {
        self.units().iter().any(|(_, result)| matches!(result, UnitResult::Moved { .. }))
    }

    /// Lines for the launcher log.
    pub fn log_lines(&self) -> Vec<String> {
        match self {
            LargeOutcome::AlreadyMoved => vec!["large data: moved before".to_string()],
            LargeOutcome::Deferred(reason) => vec![format!("large data: not moved now ({reason})")],
            LargeOutcome::Ran(units) => units
                .iter()
                .filter(|(_, result)| *result != UnitResult::Absent)
                .map(|(name, result)| match result {
                    UnitResult::Absent => unreachable!(),
                    UnitResult::Moved { destination, renamed, bytes, notes } => {
                        let how = if *renamed { "renamed".to_string() } else { format!("copied {bytes} bytes (size and sha256 checked)") };
                        let mut line = format!("large data: {name} moved to {} ({how})", destination.display());
                        for note in notes {
                            line.push_str(&format!("; {note}"));
                        }
                        line
                    }
                    UnitResult::Kept(reason) => format!("large data: {name} left in place: {reason}"),
                    UnitResult::Failed(error) => format!("large data: WARNING {name} not moved, the old place stays in use: {error}"),
                })
                .collect(),
        }
    }
}

/// Moves `source` to `destination`: one rename on the same volume; across volumes a checked copy into a staging
/// folder beside `destination`, a rename into place and the removal of `source`. `check` runs on the copy before it
/// is placed.
pub(crate) fn move_tree(
    source: &Path,
    destination: &Path,
    ops: &FsOps<'_>,
    progress: &mut dyn FnMut(u64),
    check: &dyn Fn(&Path, &BTreeMap<PathBuf, String>) -> Result<(), String>,
) -> UnitResult {
    if let Err(error) = remove_empty_tree(destination) {
        return UnitResult::Failed(format!("{}: {error}", destination.display()));
    }
    if let Some(parent) = destination.parent() {
        if let Err(error) = fs::create_dir_all(parent) {
            return UnitResult::Failed(format!("{}: {error}", parent.display()));
        }
    }
    match (ops.rename)(source, destination) {
        Ok(()) => {
            if let Err(error) = check(destination, &BTreeMap::new()) {
                let _ = (ops.rename)(destination, source);
                return UnitResult::Failed(error);
            }
            return UnitResult::Moved { destination: destination.to_path_buf(), renamed: true, bytes: 0, notes: Vec::new() };
        }
        Err(error) if error.raw_os_error() == Some(ERROR_NOT_SAME_DEVICE) => {}
        Err(error) => return UnitResult::Failed(format!("{} -> {}: {error}", source.display(), destination.display())),
    }
    let name = destination.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    let staging = destination.with_file_name(format!(".{name}.migrating"));
    let _ = fs::remove_dir_all(&staging);
    let _ = fs::remove_file(&staging);
    let report = match copy_verified(source, &staging, ops, progress) {
        Ok(report) => report,
        Err(error) => {
            let _ = fs::remove_dir_all(&staging);
            return UnitResult::Failed(error);
        }
    };
    if let Err(error) = check(&staging, &report.sha256) {
        let _ = fs::remove_dir_all(&staging);
        return UnitResult::Failed(error);
    }
    if let Err(error) = fs::rename(&staging, destination) {
        let _ = fs::remove_dir_all(&staging);
        return UnitResult::Failed(format!("{}: {error}", destination.display()));
    }
    let mut notes = Vec::new();
    if let Err(error) = fs::remove_dir_all(source) {
        notes.push(format!("the old copy {} was not fully removed: {error}", source.display()));
    }
    UnitResult::Moved { destination: destination.to_path_buf(), renamed: false, bytes: report.bytes, notes }
}

/// Moves the game files: `disc` and `prepared` together, so the receipt and the image it names never split.
pub(crate) fn move_game_files(request: &LargeMove<'_>, ops: &FsOps<'_>, progress: &mut dyn FnMut(&LargeProgress)) -> UnitResult {
    let names = ["disc", "prepared"];
    let sources: Vec<PathBuf> = names.iter().map(|name| request.local.join(name)).collect();
    if !sources.iter().any(|source| has_files(source)) {
        return UnitResult::Absent;
    }
    if names.iter().any(|name| has_files(&request.data_root.join(name))) {
        return UnitResult::Kept(format!(
            "{} already has game files; the older copy in {} is not used and may be deleted",
            request.data_root.display(),
            request.local.display()
        ));
    }
    for name in names {
        if let Err(error) = remove_empty_tree(&request.data_root.join(name)) {
            return UnitResult::Failed(format!("{}: {error}", request.data_root.join(name).display()));
        }
    }
    if let Err(error) = fs::create_dir_all(request.data_root) {
        return UnitResult::Failed(format!("{}: {error}", request.data_root.display()));
    }
    // Same volume: two renames, undone together.
    let mut renamed: Vec<usize> = Vec::new();
    let mut cross_volume = false;
    for (index, source) in sources.iter().enumerate() {
        if !source.exists() {
            continue;
        }
        match (ops.rename)(source, &request.data_root.join(names[index])) {
            Ok(()) => renamed.push(index),
            Err(error) => {
                for done in renamed.iter().rev() {
                    let _ = (ops.rename)(&request.data_root.join(names[*done]), &sources[*done]);
                }
                if error.raw_os_error() == Some(ERROR_NOT_SAME_DEVICE) && renamed.is_empty() {
                    cross_volume = true;
                    break;
                }
                return UnitResult::Failed(format!("{} -> {}: {error}", source.display(), request.data_root.display()));
            }
        }
    }
    if !cross_volume {
        if let Err(error) = (request.check_game_files)(request.data_root, &BTreeMap::new()) {
            for done in renamed.iter().rev() {
                let _ = (ops.rename)(&request.data_root.join(names[*done]), &sources[*done]);
            }
            return UnitResult::Failed(error);
        }
        return UnitResult::Moved { destination: request.data_root.to_path_buf(), renamed: true, bytes: 0, notes: Vec::new() };
    }
    // Another volume: one staging folder with both, checked, then renamed into place.
    let staging = request.data_root.join(".game-files.migrating");
    let _ = fs::remove_dir_all(&staging);
    let total: u64 = sources.iter().map(|source| tree_size(source)).sum();
    let mut done_before = 0u64;
    let mut hashes = BTreeMap::new();
    for (index, source) in sources.iter().enumerate() {
        if !source.exists() {
            continue;
        }
        let mut report_progress = |bytes: u64| {
            progress(&LargeProgress { item: "game files".into(), bytes_done: done_before + bytes, bytes_total: total })
        };
        match copy_verified(source, &staging.join(names[index]), ops, &mut report_progress) {
            Ok(report) => {
                done_before += report.bytes;
                for (relative, sha) in report.sha256 {
                    hashes.insert(Path::new(names[index]).join(relative), sha);
                }
            }
            Err(error) => {
                let _ = fs::remove_dir_all(&staging);
                return UnitResult::Failed(error);
            }
        }
    }
    if let Err(error) = (request.check_game_files)(&staging, &hashes) {
        let _ = fs::remove_dir_all(&staging);
        return UnitResult::Failed(error);
    }
    let mut placed: Vec<&str> = Vec::new();
    for name in names {
        if !staging.join(name).exists() {
            continue;
        }
        if let Err(error) = fs::rename(staging.join(name), request.data_root.join(name)) {
            for done in placed {
                let _ = fs::rename(request.data_root.join(done), staging.join(done));
            }
            let _ = fs::remove_dir_all(&staging);
            return UnitResult::Failed(format!("{}: {error}", request.data_root.join(name).display()));
        }
        placed.push(name);
    }
    let _ = fs::remove_dir_all(&staging);
    let mut notes = Vec::new();
    for source in &sources {
        if source.exists() {
            if let Err(error) = fs::remove_dir_all(source) {
                notes.push(format!("the old copy {} was not fully removed: {error}", source.display()));
            }
        }
    }
    UnitResult::Moved { destination: request.data_root.to_path_buf(), renamed: false, bytes: done_before, notes }
}

/// Moves the 1.2.0 large data into the 1.2.1 data folder. Never panics; every problem is in the outcome.
pub fn migrate_large_data(request: &LargeMove<'_>, ops: &FsOps<'_>, progress: &mut dyn FnMut(&LargeProgress)) -> LargeOutcome {
    if marker_status(&request.marker).as_deref() == Some("migrated") {
        return LargeOutcome::AlreadyMoved;
    }
    let anything = ["disc", "prepared", "gs", "runtime", "sessions", "logs"].iter().any(|name| has_files(&request.local.join(name)))
        || request.textures_sources.iter().any(|source| has_files(source));
    if !anything {
        return LargeOutcome::Ran(Vec::new());
    }
    if request.game_running {
        return LargeOutcome::Deferred("Gran Turismo 4 is running".to_string());
    }
    let no_check = |_: &Path, _: &BTreeMap<PathBuf, String>| Ok(());
    let mut units = Vec::new();

    units.push(("game files".to_string(), move_game_files(request, ops, progress)));

    let simple = |name: &str, label: &str, overflow: bool, progress: &mut dyn FnMut(&LargeProgress)| -> UnitResult {
        let source = request.local.join(name);
        if !has_files(&source) {
            return UnitResult::Absent;
        }
        let mut destination = request.data_root.join(name);
        if has_files(&destination) {
            if !overflow {
                return UnitResult::Kept(format!(
                    "{} already has it; the older copy in {} is not used and may be deleted",
                    destination.display(),
                    source.display()
                ));
            }
            // Logs, sessions and the runtime card of 1.2.0 are kept for reference beside the new ones.
            destination = request.data_root.join("logs").join(request.old_copy_folder).join(name);
            if has_files(&destination) {
                return UnitResult::Kept(format!("{} already has it", destination.display()));
            }
        }
        let total = tree_size(&source);
        let label = label.to_string();
        move_tree(&source, &destination, ops, &mut |bytes| progress(&LargeProgress { item: label.clone(), bytes_done: bytes, bytes_total: total }), &no_check)
    };
    units.push(("gt4-gs".to_string(), simple("gs", "gt4-gs", false, progress)));
    for name in ["runtime", "sessions", "logs"] {
        units.push((name.to_string(), simple(name, name, true, progress)));
    }

    let textures = match request.textures_sources.iter().find(|source| has_files(source)) {
        None => UnitResult::Absent,
        Some(source) => {
            let destination = request.data_root.join(TEXTURES_FOLDER_NAME);
            if has_files(&destination) {
                UnitResult::Kept(format!("{} already has textures; {} is not used", destination.display(), source.display()))
            } else {
                let total = tree_size(source);
                move_tree(source, &destination, ops, &mut |bytes| progress(&LargeProgress { item: "textures".into(), bytes_done: bytes, bytes_total: total }), &no_check)
            }
        }
    };
    units.push(("textures".to_string(), textures));

    let outcome = LargeOutcome::Ran(units);
    if !outcome.failed() {
        let value = serde_json::json!({
            "schema": MARKER_SCHEMA,
            "status": "migrated",
            "at_utc": stamp_json(request.stamp),
            "from": request.local.display().to_string(),
            "units": outcome.log_lines(),
        });
        let _ = write_marker(&request.marker, &value);
    }
    outcome
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unique_names_count_up() {
        let base = std::env::temp_dir().join(format!("gt4l-relocate-unique-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(base.join("Gran Turismo 4 (moved to AppData)")).unwrap();
        assert_eq!(unique_name(&base.join("free")), base.join("free"));
        assert_eq!(
            unique_name(&base.join("Gran Turismo 4 (moved to AppData)")),
            base.join("Gran Turismo 4 (moved to AppData) 2")
        );
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn the_next_free_slot_follows_the_highest_one() {
        let base = std::env::temp_dir().join(format!("gt4l-relocate-slot-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(base.join("Save 01")).unwrap();
        fs::create_dir_all(base.join("Save 04")).unwrap();
        fs::create_dir_all(base.join("Active")).unwrap();
        assert_eq!(next_free_slot(&base), base.join("Save 05"));
        assert_eq!(next_free_slot(&base.join("missing")), base.join("missing").join("Save 01"));
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn only_scanner_and_lock_errors_are_retried() {
        // Windows access denied, sharing violation and lock violation: a scanner or the indexer holding the file.
        for code in [5, 32, 33] {
            assert!(transient_rename_error(&io::Error::from_raw_os_error(code)), "code {code}");
        }
        for error in [io::ErrorKind::NotFound, io::ErrorKind::AlreadyExists, io::ErrorKind::Other] {
            assert!(!transient_rename_error(&io::Error::from(error)), "{error:?}");
        }
    }
}
