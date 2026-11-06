//! The Saves and Replays pages and the card around each Play, on top of the `gt4saves` library.
//!
//! The library lives in the user data folder `%APPDATA%\GT4Recomp` (`Saves`, `Replays`; TASK-091, owner 2026-10-22;
//! 1.0.0 to 1.2.0 used `Documents\Gran Turismo 4`). Before a start the launcher writes the card the game reads
//! (`runtime\card.ps2`, in the large data folder) from the library; after the game exits it harvests that card back
//! into the library. So the game never writes into the library while it runs. Imports never overwrite;
//! deletes go to the Recycle Bin (see `recycle`).

use std::fs;
use std::path::Path;

use gt4l_core::games::{self, Game};
use gt4l_core::guard;
use gt4l_core::language::Language;
use gt4l_core::messages;
use gt4l_core::moves;
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4saves::card::Node;
use gt4saves::folder::FolderError;
use gt4saves::library::{items_of_card_image, items_of_folder_card, items_of_psu, Library, LibraryError, LibraryPaths, Report, SLOT_PREFIX};
use gt4saves::migration::{self, LegacySources, Outcome, Targets};
use gt4saves::{CardError, SaveNames};

use gt4l_platform::recycle;
use serde::Serialize;

/// The item names of a game (its game save folder and its replay prefix), from the game table.
pub fn names_of(game: &Game) -> SaveNames {
    SaveNames::new(game.save_folder, game.replay_prefix)
}

/// The opened library and what the launcher log and the Saves page should say about it.
pub struct OpenedLibrary {
    pub library: Library,
    /// The player's folder in use (the Documents one, or the data-folder fallback).
    pub user: UserDirs,
    /// Lines for the launcher log (folder choice, the one-time move).
    pub log: Vec<String>,
    /// A sentence for the Saves page when the move to Documents did not happen as planned.
    pub notice: Option<String>,
}

/// Moves a library of launchers before 1.0.0 (`library\memcard` and friends in `pre_1_0_root`, the old
/// `%LOCALAPPDATA%\GT4Recomp`) into the library once (see `gt4saves::migration`), then opens the library. When the
/// library folder cannot be used, the data-folder fallback is used, so Play never fails on it.
pub fn open(dirs: &DataDirs, user: &UserDirs, game: &Game, pre_1_0_root: Option<&Path>) -> Result<OpenedLibrary, String> {
    let names = names_of(game);
    let mut log = Vec::new();
    if let UserRootSource::Fallback(reason) = user.source() {
        log.push(format!("save library: WARNING {reason}; using {}", user.root().display()));
    }
    match open_at(user, names, pre_1_0_root, &mut log) {
        Ok((library, notice)) => Ok(OpenedLibrary { library, user: user.clone(), log, notice }),
        Err(error) if !matches!(user.source(), UserRootSource::Fallback(_)) => {
            let fallback = user.with_library_at(&dirs.fallback_user_root(), UserRootSource::Fallback(error.clone()));
            log.push(format!("save library: WARNING {} cannot be used ({error}); using {}", user.root().display(), fallback.root().display()));
            let (library, notice) = open_at(&fallback, names, pre_1_0_root, &mut log)?;
            let notice = notice.or_else(|| Some(format!("Saves are kept in {} because {} could not be used.", fallback.root().display(), user.root().display())));
            Ok(OpenedLibrary { library, user: fallback, log, notice })
        }
        Err(error) => Err(error),
    }
}

fn open_at(user: &UserDirs, names: SaveNames, pre_1_0_root: Option<&Path>, log: &mut Vec<String>) -> Result<(Library, Option<String>), String> {
    guard::protect_user_library(user.root());
    let mut notice = None;
    if let Some(old_root) = pre_1_0_root {
        let targets = Targets { saves: user.saves(), replays: user.replays(), names };
        let outcome = migration::migrate(
            &LegacySources::of_data_root(old_root),
            &targets,
            old_root,
            &user.library_migration_marker(),
            &gt4saves::timestamp(),
            &migration::copy_file,
            &moves::move_path,
        );
        log.push(outcome.describe(&targets));
        let old_library = old_root.join("library");
        notice = match &outcome {
            Outcome::Conflict { .. } => Some(format!(
                "An older save library is still in {}. It was not merged; the saves shown here are in {}.",
                old_library.display(),
                user.root().display()
            )),
            Outcome::Failed(_) => Some(format!(
                "Your saves could not be moved to {}. They are safe in {}; the launcher tries again at the next start.",
                user.root().display(),
                old_library.display()
            )),
            _ => None,
        };
    }
    let paths = LibraryPaths { saves: user.saves(), replays: user.replays(), backups: user.card_backups() };
    let library = Library::open_paths(&paths, names).map_err(|error| format!("save library: {error}"))?;
    Ok((library, notice))
}

/// One row of the Saves or Replays page.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    /// Library name: `Save NN` for an inactive slot, the game's save name, or a replay name.
    pub name: String,
    pub label: String,
    /// The save the next start uses.
    pub active: bool,
    /// Last change of the item's folder, ISO 8601 UTC (empty when unknown).
    pub modified: String,
    /// The player gave the save a name (`label` is that name; the library name stays `Save NN`).
    pub renamed: bool,
    /// The game the item belongs to (`gt4` or `spec2`). Empty for replays, which the page lists per game.
    pub game: String,
}

/// The page's item shape (`ui/js/pages/library.js`): `id` is what the page sends back to `saves_activate`,
/// `saves_delete`, `saves_export`, `saves_rename` and `replays_delete`: the library name, prefixed with the game id
/// (`gt4:Save 02`) for a save. `slot` is the library name alone; `name` is the text shown.
impl Serialize for Item {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeStruct;
        let id = if self.game.is_empty() { self.name.clone() } else { format!("{}:{}", self.game, self.name) };
        let mut row = serializer.serialize_struct("Item", 7)?;
        row.serialize_field("id", &id)?;
        row.serialize_field("slot", &self.name)?;
        row.serialize_field("name", &self.label)?;
        row.serialize_field("active", &self.active)?;
        row.serialize_field("modified", &self.modified)?;
        row.serialize_field("renamed", &self.renamed)?;
        row.serialize_field("game", &self.game)?;
        row.end()
    }
}

/// ISO 8601 time of the last change of `folder`, or an empty text.
fn modified_of(folder: &Path) -> String {
    fs::metadata(folder).and_then(|metadata| metadata.modified()).map(gt4saves::iso8601_utc).unwrap_or_default()
}

/// The active save first, then the inactive slots in slot order.
pub fn saves(library: &Library) -> Result<Vec<Item>, String> {
    let mut items = Vec::new();
    if library.active_save().map_err(|error| error.to_string())?.is_some() {
        let game_save = library.names().game_save;
        let named = library.display_name(game_save);
        items.push(Item {
            name: game_save.to_string(),
            renamed: named.is_some(), game: String::new(),
            label: named.unwrap_or_else(|| "Active save".to_string()),
            active: true,
            modified: modified_of(&library.active_dir().join(game_save)),
        });
    }
    for slot in library.slots().map_err(|error| error.to_string())? {
        let modified = modified_of(&library.slot_dir(&slot).join(library.names().game_save));
        let named = library.display_name(&slot);
        items.push(Item {
            renamed: named.is_some(), game: String::new(),
            label: named.unwrap_or_else(|| slot.clone()),
            name: slot,
            active: false,
            modified,
        });
    }
    Ok(items)
}

/// Every replay the library keeps, in name order.
pub fn replays(library: &Library) -> Result<Vec<Item>, String> {
    Ok(library
        .replays()
        .map_err(|error| error.to_string())?
        .into_iter()
        .map(|name| {
            let modified = modified_of(&library.replays_dir().join(&name));
            Item { label: name.clone(), name, active: false, modified, renamed: false, game: String::new() }
        })
        .collect())
}

/// What an import did: the report of each game that took items, and the names no game of the launcher knows.
#[derive(Debug, Default)]
pub struct ImportOutcome {
    pub per_game: Vec<(&'static Game, Report)>,
    pub unknown: Vec<String>,
}

/// Imports a file the user picked: a `.psu` save, a card image (`.ps2`, `.mcd`, `.bin`) or a folder card. Each item goes
/// to the library of the game its name belongs to (TASK-101 item 1), whatever game the page shows. The error text is the
/// page's own (see [`import_error_text`]).
pub fn import_path(libraries: &[(&'static Game, &Library)], path: &Path, language: Language) -> Result<ImportOutcome, String> {
    let items = read_items(path, language)?;
    let mut outcome = ImportOutcome::default();
    for &(game, library) in libraries {
        let names = names_of(game);
        let mine: Vec<Node> = items.iter().filter(|item| names.owns(&item.name())).cloned().collect();
        if mine.is_empty() {
            continue;
        }
        let report = library.import_items(&mine).map_err(|error| import_error_text(&error, language))?;
        outcome.per_game.push((game, report));
    }
    if outcome.per_game.is_empty() {
        let found = items.iter().map(Node::name).collect();
        return Err(import_error_text(&LibraryError::NoGtSave { found }, language));
    }
    outcome.unknown = items
        .iter()
        .map(Node::name)
        .filter(|name| !games::GAMES.iter().any(|game| names_of(game).owns(name)))
        .collect();
    Ok(outcome)
}

/// The items of a picked file, whatever game they belong to.
fn read_items(path: &Path, language: Language) -> Result<Vec<Node>, String> {
    let result = if path.is_dir() {
        items_of_folder_card(path)
    } else {
        let extension = path.extension().map(|ext| ext.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
        let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
        match extension.as_str() {
            "psu" => items_of_psu(&bytes),
            "ps2" | "mcd" | "bin" | "card" => items_of_card_image(&bytes),
            other => return Err(messages::render("saves.not_save_file", language, &[("extension", other)])),
        }
    };
    result.map_err(|error| import_error_text(&error, language))
}

/// What the page says about a failed import: the card is not a PS2 card, its size is not supported, it is damaged (with
/// the reason), or it holds no Gran Turismo 4 save (with what it does hold).
pub fn import_error_text(error: &LibraryError, language: Language) -> String {
    match error {
        LibraryError::Card(CardError::BadSize(_)) | LibraryError::Card(CardError::NotACard) => {
            messages::text("saves.card_not_ps2", language).to_string()
        }
        LibraryError::Card(CardError::Unsupported(_)) => messages::text("saves.card_unsupported", language).to_string(),
        LibraryError::Card(CardError::Corrupt(reason)) => {
            messages::render("saves.card_damaged", language, &[("reason", reason)])
        }
        LibraryError::NoGtSave { found } => {
            messages::render("saves.no_gt4_save", language, &[("found", &describe_found(found, language))])
        }
        other => other.to_string(),
    }
}

/// "2 saves of other games, 1 other item": the names a card holds, counted by kind. A game save is a folder whose name
/// ends in `GAMEDATA`.
fn describe_found(found: &[String], language: Language) -> String {
    if found.is_empty() {
        return messages::text("saves.found_nothing", language).to_string();
    }
    let saves = found.iter().filter(|name| name.ends_with("GAMEDATA")).count();
    let others = found.len() - saves;
    let mut parts = Vec::new();
    if saves > 0 {
        parts.push(messages::render("saves.found_other_saves", language, &[("count", &saves.to_string())]));
    }
    if others > 0 {
        parts.push(messages::render("saves.found_other_items", language, &[("count", &others.to_string())]));
    }
    parts.join(", ")
}

/// Starts a new save. What the last run left on the card is harvested first, so its progress is kept; the card is then
/// moved to `backups`, so the next Play does not harvest the old save back into the empty Active folder. Returns the slot
/// the old save went to.
pub fn new_save(library: &Library, runtime_card: &Path) -> Result<Option<String>, String> {
    new_save_with(library, runtime_card, &moves::MoveOps::real())
}

/// [`new_save`] with the file operations given, so a test can make the move between the card and the backups report
/// another volume.
fn new_save_with(library: &Library, runtime_card: &Path, file_ops: &moves::MoveOps<'_>) -> Result<Option<String>, String> {
    harvest_card(library, runtime_card)?;
    park_runtime_card(library, runtime_card, gt4saves::library::NEW_SAVE_BACKUP_SUFFIX, file_ops)?;
    library.new_save().map_err(|error| error.to_string())
}

/// Moves the card the game reads into `<backups>\<timestamp><suffix>`, so the next Play builds a fresh card instead of
/// harvesting the save that was just deleted or kept aside. The card and the backups are on different drives in some
/// installs (the card lives in the `<install>\data` folder, the backups in `%APPDATA%`; issue #208), so this goes through
/// [`moves::move_path`]: a plain rename would fail there and leave the stale card in place, which brings a deleted save
/// back at the next Play.
fn park_runtime_card(
    library: &Library,
    runtime_card: &Path,
    suffix: &str,
    file_ops: &moves::MoveOps<'_>,
) -> Result<(), String> {
    if !runtime_card.is_file() {
        return Ok(());
    }
    let backups = library.paths().backups.clone();
    fs::create_dir_all(&backups).map_err(|error| format!("{}: {error}", backups.display()))?;
    let target = backups.join(format!("{}{suffix}", gt4saves::timestamp()));
    moves::move_path_with(runtime_card, &target, file_ops).map_err(|error| format!("{}: {error}", runtime_card.display()))?;
    Ok(())
}

/// The `.psu` bytes of a save, and the file name the save dialog suggests (`<name>.psu`).
pub fn export(library: &Library, name: &str) -> Result<(Vec<u8>, String), String> {
    let bytes = library.export_psu(name).map_err(|error| error.to_string())?;
    let base = library.display_name(name).unwrap_or_else(|| {
        if name == library.names().game_save {
            "Active save".to_string()
        } else {
            name.to_string()
        }
    });
    Ok((bytes, format!("{base}.psu")))
}

/// Gives a save a display name. Returns the name as stored.
pub fn rename(library: &Library, name: &str, new_name: &str, language: Language) -> Result<String, String> {
    library.rename_save(name, new_name).map_err(|error| match error {
        LibraryError::Name(_) => messages::text("saves.name_empty", language).to_string(),
        other => other.to_string(),
    })
}

/// Makes a slot the active save. The save that was active moves to a new slot.
pub fn activate(library: &Library, slot: &str) -> Result<(), String> {
    library.activate_slot(slot).map_err(|error| error.to_string())
}

/// TASK-164 (issue #160): whether the next start meets the game's first-start questions (create the game data, then format
/// the card, where Cancel is selected first). True when the library has no active save and the card the game left behind,
/// which the next start harvests first (see [`prepare_card`]), holds none either. A library that cannot be read gives false:
/// Play reports that problem itself.
pub fn first_start_expected(library: &Library, runtime_card: &Path) -> bool {
    match library.active_save() {
        Ok(Some(_)) | Err(_) => return false,
        Ok(None) => {}
    }
    let names = library.names();
    let on_card = fs::read(runtime_card)
        .ok()
        .and_then(|bytes| items_of_card_image(&bytes).ok())
        .is_some_and(|items| items.iter().any(|item| names.is_game_save(&item.name())));
    !on_card
}

/// Writes the card for the next start: the active save and every replay, read back before it is
/// written. A card the game left behind is harvested first, so nothing it wrote is lost; the old file
/// is also kept under `backups` by `write_card_image`.
pub fn prepare_card(library: &Library, runtime_card: &Path, language: Language) -> Result<(), String> {
    if runtime_card.is_file() {
        if let Ok(bytes) = fs::read(runtime_card) {
            // An unreadable card is not an error here: its bytes are backed up when it is replaced.
            let _ = library.harvest(&bytes);
        }
    }
    let replay_names: Vec<String> = library.replays().map_err(|error| match error {
        // The listing of the Replays folder failed, so that folder is the one to name.
        LibraryError::Io(io_error) => file_unreadable_text(library, &library.replays_dir(), &io_error, language),
        other => card_build_problem(library, &other, language),
    })?;
    let image = library.build_card(&replay_names).map_err(|error| card_build_problem(library, &error, language))?;
    if let Some(parent) = runtime_card.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("runtime folder: {error}"))?;
    }
    library.write_card_image(runtime_card, &image).map_err(|error| error.to_string())
}

/// What Play says when the card for the next start could not be built. A card that is full says so; a file of a save or a replay
/// that cannot be read, or a replay folder that went missing, is named (issue #160: the text was only "(os error 2)", so the
/// player could not tell what to restore); anything else keeps its reason.
fn card_build_problem(library: &Library, error: &LibraryError, language: Language) -> String {
    match error {
        LibraryError::Card(CardError::Full) => messages::text("saves.memory_full", language).to_string(),
        LibraryError::Folder(FolderError::Unreadable { path, error }) => file_unreadable_text(library, path, error, language),
        LibraryError::NotFound(item) => messages::render("saves.card_item_missing", language, &[("item", item)]),
        other => messages::render("saves.card_not_built", language, &[("reason", &other.to_string())]),
    }
}

/// "This file could not be read: Saves\Active\...\_item.hex (reason)", with the path from the library's own folder.
fn file_unreadable_text(library: &Library, path: &Path, error: &std::io::Error, language: Language) -> String {
    messages::render(
        "saves.card_file_unreadable",
        language,
        &[("file", &library_relative_path(library, path)), ("reason", &error.to_string())],
    )
}

/// A file of the library as the player reads it: relative to the folder that holds `Saves` and `Replays` (the game's data
/// folder), for example `Replays\Replay 05\_item.hex`, so the text holds no user name. A file outside both folders shows its
/// name only.
fn library_relative_path(library: &Library, path: &Path) -> String {
    let paths = library.paths();
    for base in [&paths.saves, &paths.replays] {
        if !path.starts_with(base) {
            continue;
        }
        // `Saves` and `Replays` stay in the text; what is above them (the user's folders) does not.
        if let Some(relative) = base.parent().and_then(|parent| path.strip_prefix(parent).ok()) {
            return relative.display().to_string();
        }
    }
    path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_else(|| path.display().to_string())
}

/// Copies the card the game wrote back into the library. A missing card means nothing was saved.
pub fn harvest_card(library: &Library, runtime_card: &Path) -> Result<Report, String> {
    if !runtime_card.is_file() {
        return Ok(Report::default());
    }
    let bytes = fs::read(runtime_card).map_err(|error| format!("{}: {error}", runtime_card.display()))?;
    library.harvest(&bytes).map_err(|error| error.to_string())
}

/// Short sentence about what an import did, for the Saves and Replays pages. An item of another game than the page shows
/// is said to have gone to that game; items no game knows are named as skipped.
pub fn import_summary(outcome: &ImportOutcome, page: &Game, language: Language) -> String {
    let mut parts = Vec::new();
    for (game, report) in &outcome.per_game {
        let mut text = report_text(report, language);
        if game.id != page.id {
            text = format!("{} {}", text, messages::render("saves.went_to_game", language, &[("game", game.display_name)]));
        }
        if outcome.per_game.len() > 1 {
            text = format!("{}: {text}", game.display_name);
        }
        parts.push(text);
    }
    if !outcome.unknown.is_empty() {
        parts.push(messages::render("saves.skipped", language, &[("items", &outcome.unknown.join(", "))]));
    }
    parts.join(" ")
}

/// The sentence for one game's report.
fn report_text(report: &Report, language: Language) -> String {
    let mut parts = Vec::new();
    if !report.added.is_empty() {
        parts.push(messages::render("saves.added", language, &[("items", &report.added.join(", "))]));
    }
    if !report.unchanged.is_empty() {
        parts.push(messages::text("saves.already_in_library", language).to_string());
    }
    if !report.skipped.is_empty() {
        parts.push(messages::render("saves.skipped", language, &[("items", &report.skipped.join(", "))]));
    }
    if parts.is_empty() {
        parts.push(messages::text("saves.nothing_to_import", language).to_string());
    }
    parts.join(" ")
}

/// Sends a library item to the Recycle Bin. Deleting the active save also parks the runtime card: what the last run left on
/// it is harvested first (so unharvested progress is not lost), then the card moves to `backups` as
/// `<timestamp>-before-delete.ps2`. Without that, the next Play would harvest the deleted save back from the stale card,
/// which is the whole of issue #208 on an install whose card and library are on different drives. The caller refuses this
/// while the game runs.
pub fn delete(library: &Library, item: &Item, runtime_card: &Path) -> Result<(), String> {
    delete_with(library, item, runtime_card, &recycle::move_to_recycle_bin, &moves::MoveOps::real())
}

/// [`delete`] with the removal of the item's folder and the file operations given (the Recycle Bin and real moves for the
/// launcher, a plain removal and a rename that can report another volume in tests).
fn delete_with(
    library: &Library,
    item: &Item,
    runtime_card: &Path,
    remove: &dyn Fn(&Path) -> Result<(), String>,
    file_ops: &moves::MoveOps<'_>,
) -> Result<(), String> {
    if item.active {
        return delete_active(library, runtime_card, remove, file_ops);
    }
    let path = if item.name.starts_with(SLOT_PREFIX) {
        library.slot_dir(&item.name)
    } else {
        library.replays_dir().join(&item.name)
    };
    if !path.is_dir() {
        return Err(format!("{} is not in the library any more", item.label));
    }
    remove(&path)
}

/// Deletes the active save: harvest the runtime card, park it in `backups`, move the save to the Recycle Bin and clear its
/// display name. The next Play builds a card without a game save, so the game starts a new profile.
fn delete_active(
    library: &Library,
    runtime_card: &Path,
    remove: &dyn Fn(&Path) -> Result<(), String>,
    file_ops: &moves::MoveOps<'_>,
) -> Result<(), String> {
    harvest_card(library, runtime_card)?;
    let path = library.active_save_dir();
    if !path.is_dir() {
        return Err("The active save is not in the library any more".to_string());
    }
    park_runtime_card(library, runtime_card, gt4saves::library::DELETE_BACKUP_SUFFIX, file_ops)?;
    remove(&path)?;
    library.clear_active_name().map_err(|error| error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
    use gt4saves::Card;
    use gt4l_core::games::GT4;

    fn import_path(library: &Library, path: &Path, language: Language) -> Result<Report, String> {
        let outcome = super::import_path(&[(&GT4, library)], path, language)?;
        Ok(outcome.per_game.into_iter().next().map(|(_, report)| report).unwrap_or_default())
    }
    use std::path::PathBuf;

    /// The library of a scratch data folder, with a scratch stand-in for Documents (never the real one).
    fn open_scratch(data: &Path) -> (UserDirs, Library) {
        let dirs = DataDirs::new(data);
        let user = UserDirs::new(data.join("Documents").join("Gran Turismo 4"), UserRootSource::Override);
        let opened = open(&dirs, &user, &GT4, Some(data)).unwrap();
        (opened.user, opened.library)
    }

    const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
    const REPLAY: &str = "BASCUS-97328R0001";

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-saves-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn file(name: &str, data: Vec<u8>) -> Node {
        Node::file(DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap(), data)
    }

    fn directory(name: &str, children: Vec<Node>) -> Node {
        Node::dir(DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap(), children)
    }

    /// A synthetic save: a few files with `seed` in their contents, so two saves differ.
    fn game_save(seed: u8) -> Node {
        let pattern = |len: usize| (0..len).map(|i| (i as u8).wrapping_mul(7).wrapping_add(seed)).collect();
        directory(GT4.save_folder, vec![file("icon.sys", pattern(964)), file("garage", pattern(20_000))])
    }

    fn replay() -> Node {
        directory(REPLAY, vec![file("icon.sys", vec![1; 964]), file("replay.rep", vec![9; 5_000])])
    }

    /// A card image holding the given items, as the game would leave it.
    fn card_with(items: &[Node]) -> Vec<u8> {
        Card::build(items).unwrap().to_image()
    }

    fn names(items: &[Item]) -> Vec<String> {
        items.iter().map(|item| item.name.clone()).collect()
    }

    #[test]
    fn a_game_card_is_harvested_and_the_next_card_is_built_from_the_library() {
        let data = scratch("harvest");
        let (user, library) = open_scratch(&data);
        let runtime_card = data.join("runtime").join("card.ps2");
        fs::create_dir_all(runtime_card.parent().unwrap()).unwrap();
        fs::write(&runtime_card, card_with(&[game_save(1), replay()])).unwrap();

        let report = harvest_card(&library, &runtime_card).unwrap();
        assert!(report.added.contains(&GT4.save_folder.to_string()));
        assert_eq!(names(&saves(&library).unwrap()), vec![GT4.save_folder.to_string()]);
        assert_eq!(names(&replays(&library).unwrap()), vec![REPLAY.to_string()]);

        fs::remove_file(&runtime_card).unwrap();
        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let built = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let built_names: Vec<String> = built.iter().map(|node| node.name()).collect();
        assert_eq!(built_names, vec![GT4.save_folder.to_string(), REPLAY.to_string()]);
        assert!(user.saves().join("Active").join(GT4.save_folder).is_dir(), "the save is in Documents\\Gran Turismo 4\\Saves");
        assert!(user.replays().join(REPLAY).is_dir());
        assert!(!data.join("library").join("memcard").exists(), "nothing is written to the old AppData library");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn an_import_never_overwrites_and_identical_saves_are_not_repeated() {
        let data = scratch("import");
        let (_, library) = open_scratch(&data);
        let first = data.join("first.ps2");
        fs::write(&first, card_with(&[game_save(1)])).unwrap();
        let second = data.join("second.ps2");
        fs::write(&second, card_with(&[game_save(2)])).unwrap();

        assert_eq!(import_path(&library, &first, Language::fallback()).unwrap().added, vec!["Save 01".to_string()]);
        // The same save again is recognised and nothing new is written.
        let again = import_path(&library, &first, Language::fallback()).unwrap();
        assert!(again.added.is_empty());
        assert_eq!(again.unchanged, vec![GT4.save_folder.to_string()]);
        // A different save becomes a new slot; the active save is untouched.
        assert_eq!(import_path(&library, &second, Language::fallback()).unwrap().added, vec!["Save 02".to_string()]);
        assert_eq!(names(&saves(&library).unwrap()), vec!["Save 01".to_string(), "Save 02".to_string()]);
        assert!(saves(&library).unwrap().iter().all(|item| !item.active));
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn activating_a_slot_parks_the_previous_save() {
        let data = scratch("activate");
        let (_, library) = open_scratch(&data);
        let runtime_card = data.join("runtime").join("card.ps2");
        let source = data.join("source.ps2");
        fs::write(&source, card_with(&[game_save(3)])).unwrap();
        import_path(&library, &source, Language::fallback()).unwrap();
        // Make the imported save active: the library had no active save, so the slot becomes it.
        activate(&library, "Save 01").unwrap();
        let before = saves(&library).unwrap();
        assert_eq!(names(&before), vec![GT4.save_folder.to_string()]);
        assert!(before[0].active && before[0].label == "Active save");

        let other = data.join("other.ps2");
        fs::write(&other, card_with(&[game_save(4)])).unwrap();
        import_path(&library, &other, Language::fallback()).unwrap();
        activate(&library, "Save 01").unwrap();
        let after = names(&saves(&library).unwrap());
        assert_eq!(after, vec![GT4.save_folder.to_string(), "Save 02".to_string()]);
        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let _ = fs::remove_dir_all(&data);
    }

    /// A library with the active save and one replay, harvested from a card the way a Play leaves it.
    fn library_with_a_save_and_a_replay(name: &str) -> (PathBuf, Library) {
        let data = scratch(name);
        let (_, library) = open_scratch(&data);
        let card = data.join("harvest.ps2");
        fs::write(&card, card_with(&[game_save(1), replay()])).unwrap();
        harvest_card(&library, &card).unwrap();
        (data, library)
    }

    #[test]
    fn a_file_missing_from_a_replay_is_named_relative_to_the_library_not_the_user_folder() {
        // Issue #160: "the memory card could not be built: ... (os error 2)" named nothing.
        let (data, library) = library_with_a_save_and_a_replay("missing-replay-file");
        let runtime_card = data.join("runtime").join("card.ps2");
        fs::remove_file(library.replays_dir().join(REPLAY).join("replay.rep")).unwrap();

        let relative = Path::new("Replays").join(REPLAY).join("replay.rep").display().to_string();
        let english = prepare_card(&library, &runtime_card, Language::fallback()).unwrap_err();
        assert!(
            english.starts_with(&format!("The memory card could not be built because this file could not be read: {relative} (")),
            "{english}"
        );
        assert!(english.ends_with("Put the file back, or remove the save or replay it belongs to on the Saves or Replays page, then press Play again."), "{english}");
        assert!(!english.contains(&data.display().to_string()), "no folder above the library, so no user name: {english}");
        assert!(!runtime_card.exists(), "no card was written");

        let portuguese = prepare_card(&library, &runtime_card, Language::from_tag("pt-BR").expect("pt-BR is listed")).unwrap_err();
        assert!(
            portuguese.starts_with(&format!("Não foi possível montar o cartão de memória porque este arquivo não pôde ser lido: {relative} (")),
            "{portuguese}"
        );
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn a_file_missing_from_the_active_save_is_named_under_saves() {
        let (data, library) = library_with_a_save_and_a_replay("missing-save-file");
        fs::remove_file(library.active_dir().join(GT4.save_folder).join("garage")).unwrap();
        let relative = Path::new("Saves").join("Active").join(GT4.save_folder).join("garage").display().to_string();
        let message = prepare_card(&library, &data.join("runtime").join("card.ps2"), Language::fallback()).unwrap_err();
        assert!(message.contains(&format!("this file could not be read: {relative} (")), "{message}");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn a_missing_replays_folder_names_the_folder() {
        let (data, library) = library_with_a_save_and_a_replay("missing-replays-folder");
        fs::remove_dir_all(library.replays_dir()).unwrap();
        let message = prepare_card(&library, &data.join("runtime").join("card.ps2"), Language::fallback()).unwrap_err();
        assert!(message.contains("this file could not be read: Replays ("), "{message}");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn the_other_card_problems_keep_their_texts() {
        let (data, library) = library_with_a_save_and_a_replay("card-problems");
        let english = Language::fallback();
        assert_eq!(
            card_build_problem(&library, &LibraryError::Card(CardError::Full), english),
            "The memory card is full. Delete some replays on the Replays page."
        );
        assert_eq!(
            card_build_problem(&library, &LibraryError::NotFound("BASCUS-97328R0009".into()), english),
            "The memory card could not be built because the folder of BASCUS-97328R0009 is missing from your replays. Remove it on the Replays page, then press Play again."
        );
        assert_eq!(
            card_build_problem(&library, &LibraryError::Card(CardError::Corrupt("bad page".into())), english),
            "The memory card could not be built: corrupt card: bad page"
        );
        // A path outside the two library folders shows its file name only.
        assert_eq!(library_relative_path(&library, Path::new("elsewhere").join("deep").join("file.bin").as_path()), "file.bin");
        let _ = fs::remove_dir_all(&data);
    }

    fn active_item() -> Item {
        Item { name: GT4.save_folder.into(), label: "Active save".into(), active: true, modified: String::new(), renamed: false, game: String::new() }
    }

    // The tests remove the folder instead of sending it to the Recycle Bin.
    fn remove_folder(path: &Path) -> Result<(), String> {
        fs::remove_dir_all(path).map_err(|error| error.to_string())
    }

    /// The moves of a one-volume machine.
    fn moves_here() -> moves::MoveOps<'static> {
        moves::MoveOps::real()
    }

    fn copy_file(from: &Path, to: &Path) -> std::io::Result<u64> {
        fs::copy(from, to)
    }

    /// A rename that reports another volume when the two places are on either side of `install`, as Windows does for an
    /// install on `F:` whose save library is on `C:` (issue #208). A staging copy beside its destination still renames: it
    /// is on the destination's own volume.
    fn cross_drive_rename(install: &Path, from: &Path, to: &Path) -> std::io::Result<()> {
        let staging = from.file_name().is_some_and(|name| name.to_string_lossy().contains(".moving"));
        if !staging && from.starts_with(install) != to.starts_with(install) {
            return Err(std::io::Error::from_raw_os_error(moves::ERROR_NOT_SAME_DEVICE));
        }
        fs::rename(from, to)
    }

    /// A card file left by the game, and the folders of an install on another drive.
    fn other_drive_card(data: &Path, items: &[Node]) -> PathBuf {
        let install = data.join("install-data").join("games").join("gt4").join("runtime");
        fs::create_dir_all(&install).unwrap();
        let card = install.join("card.ps2");
        fs::write(&card, card_with(items)).unwrap();
        card
    }

    /// The parked card in `backups` whose name ends with `suffix` (there is one in each test).
    fn parked_cards(library: &Library, suffix: &str) -> Vec<PathBuf> {
        fs::read_dir(library.paths().backups.clone())
            .unwrap()
            .map(|entry| entry.unwrap().path())
            .filter(|path| path.file_name().is_some_and(|name| name.to_string_lossy().ends_with(suffix)))
            .collect()
    }

    #[test]
    fn deleting_the_active_save_clears_it() {
        let data = scratch("delete");
        let (_, library) = open_scratch(&data);
        let card = data.join("card.ps2");
        fs::write(&card, card_with(&[game_save(5), replay()])).unwrap();
        harvest_card(&library, &card).unwrap();
        library.rename_save(GT4.save_folder, "My career").unwrap();
        assert!(library.active_dir().join(gt4saves::library::NAME_FILE).is_file());
        delete_with(&library, &active_item(), &data.join("no-runtime-card.ps2"), &remove_folder, &moves_here()).unwrap();
        assert!(!library.active_dir().join(GT4.save_folder).exists(), "the save left the library");
        assert!(!library.active_dir().join(gt4saves::library::NAME_FILE).exists(), "its display name is gone");
        assert!(!names(&saves(&library).unwrap()).contains(&GT4.save_folder.to_string()));
        assert_eq!(replays(&library).unwrap().len(), 1, "replays are untouched");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn deleting_the_active_save_parks_the_runtime_card_and_the_next_play_starts_clean() {
        let data = scratch("delete-card");
        let (_, library) = open_scratch(&data);
        let runtime_card = data.join("runtime").join("card.ps2");
        fs::create_dir_all(runtime_card.parent().unwrap()).unwrap();
        fs::write(&runtime_card, card_with(&[game_save(5), replay()])).unwrap();
        harvest_card(&library, &runtime_card).unwrap();
        delete_with(&library, &active_item(), &runtime_card, &remove_folder, &moves_here()).unwrap();
        assert!(!runtime_card.exists(), "the stale runtime card is moved away");
        let parked: Vec<String> = fs::read_dir(&library.paths().backups)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .filter(|name| name.ends_with("-before-delete.ps2"))
            .collect();
        assert_eq!(parked.len(), 1, "{parked:?}");

        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let rebuilt = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let root: Vec<String> = rebuilt.iter().map(|node| node.name()).collect();
        assert!(!root.iter().any(|name| name == GT4.save_folder), "no game save on the next card: {root:?}");
        assert!(root.iter().any(|name| name == REPLAY), "the replay is still there: {root:?}");
        assert!(!library.active_dir().join(GT4.save_folder).exists(), "and Play did not bring the save back");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn deleting_the_active_save_parks_a_card_from_another_drive_and_the_next_play_starts_clean() {
        // Issue #208: the game and its data folder on one drive, `%APPDATA%` (the library and the card backups) on
        // another. The park was a plain rename, which Windows refuses across volumes, so the save was never deleted.
        let data = scratch("delete-other-drive");
        let (_, library) = open_scratch(&data);
        let install = data.join("install-data");
        let card_before = card_with(&[game_save(5), replay()]);
        let runtime_card = other_drive_card(&data, &[game_save(5), replay()]);
        harvest_card(&library, &runtime_card).unwrap();
        library.rename_save(GT4.save_folder, "My career").unwrap();

        let rename = |from: &Path, to: &Path| cross_drive_rename(&install, from, to);
        let file_ops = moves::MoveOps { rename: &rename, copy_file: &copy_file };
        delete_with(&library, &active_item(), &runtime_card, &remove_folder, &file_ops).unwrap();

        assert!(!runtime_card.exists(), "the card on the other drive is moved away, so nothing brings the save back");
        assert!(!library.active_dir().join(GT4.save_folder).exists(), "the save left the library");
        assert!(!library.active_dir().join(gt4saves::library::NAME_FILE).exists(), "its display name is gone");
        let parked = parked_cards(&library, gt4saves::library::DELETE_BACKUP_SUFFIX);
        assert_eq!(parked.len(), 1, "{parked:?}");
        assert_eq!(fs::read(&parked[0]).unwrap(), card_before, "the card is kept whole, byte for byte");

        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let rebuilt = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let root: Vec<String> = rebuilt.iter().map(|node| node.name()).collect();
        assert!(!root.iter().any(|name| name == GT4.save_folder), "no game save on the next card: {root:?}");
        assert!(!library.active_dir().join(GT4.save_folder).exists(), "and Play did not bring the save back");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn a_new_save_from_an_install_on_another_drive_parks_the_card_and_keeps_the_old_save() {
        let data = scratch("new-save-other-drive");
        let (_, library) = open_scratch(&data);
        let install = data.join("install-data");
        let runtime_card = other_drive_card(&data, &[game_save(6), replay()]);

        let rename = |from: &Path, to: &Path| cross_drive_rename(&install, from, to);
        let file_ops = moves::MoveOps { rename: &rename, copy_file: &copy_file };
        let parked = new_save_with(&library, &runtime_card, &file_ops).unwrap();

        assert_eq!(parked, Some("Save 01".to_string()), "the old save went to a slot");
        assert!(!runtime_card.exists(), "the card on the other drive is moved away");
        assert_eq!(parked_cards(&library, gt4saves::library::NEW_SAVE_BACKUP_SUFFIX).len(), 1);
        assert!(library.slot_dir("Save 01").join(GT4.save_folder).is_dir());
        assert!(!library.active_dir().join(GT4.save_folder).exists(), "Active is empty, so the game starts a new profile");

        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let rebuilt = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let root: Vec<String> = rebuilt.iter().map(|node| node.name()).collect();
        assert!(!root.iter().any(|name| name == GT4.save_folder), "the new card carries no save: {root:?}");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn a_card_on_another_drive_is_harvested_into_the_library_and_written_back_there() {
        let data = scratch("harvest-other-drive");
        let (_, library) = open_scratch(&data);
        let runtime_card = other_drive_card(&data, &[game_save(8), replay()]);

        let report = harvest_card(&library, &runtime_card).unwrap();
        assert!(report.added.contains(&GT4.save_folder.to_string()));
        assert!(library.active_dir().join(GT4.save_folder).is_dir(), "the save is in the library, on its own drive");
        assert!(library.replays_dir().join(REPLAY).is_dir());

        prepare_card(&library, &runtime_card, Language::fallback()).unwrap();
        let rebuilt = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let root: Vec<String> = rebuilt.iter().map(|node| node.name()).collect();
        assert_eq!(root, vec![GT4.save_folder.to_string(), REPLAY.to_string()], "the card keeps the save and the replay");
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn the_first_start_hint_shows_only_while_no_save_exists_in_the_library_or_on_the_last_card() {
        let data = scratch("first-start");
        let (_, library) = open_scratch(&data);
        let runtime_card = data.join("runtime").join("card.ps2");
        assert!(first_start_expected(&library, &runtime_card), "a new library, no card yet");

        fs::create_dir_all(runtime_card.parent().unwrap()).unwrap();
        fs::write(&runtime_card, card_with(&[replay()])).unwrap();
        assert!(first_start_expected(&library, &runtime_card), "a card with only a replay");

        fs::write(&runtime_card, b"not a card").unwrap();
        assert!(first_start_expected(&library, &runtime_card), "an unreadable card holds no save");

        fs::write(&runtime_card, card_with(&[game_save(3)])).unwrap();
        assert!(!first_start_expected(&library, &runtime_card), "the game made its data on the last start");

        harvest_card(&library, &runtime_card).unwrap();
        fs::remove_file(&runtime_card).unwrap();
        assert!(!first_start_expected(&library, &runtime_card), "the library has an active save");
        let _ = fs::remove_dir_all(&data);
    }
}

#[cfg(test)]
mod migration_tests {
    use super::*;
    use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
    use gt4saves::Card;
    use gt4l_core::games::GT4;

    use std::path::PathBuf;

    const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-saves-move-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn old_save(data: &Path) -> Node {
        let file = Node::file(DirEntry::new("garage", MODE_FILE_DEFAULT, STAMP, STAMP).unwrap(), vec![7; 4_000]);
        let save = Node::dir(DirEntry::new(GT4.save_folder, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap(), vec![file]);
        let memcard = data.join("library").join("memcard");
        fs::create_dir_all(&memcard).unwrap();
        gt4saves::folder::export_item(&memcard, &save).unwrap();
        save
    }

    #[test]
    fn the_first_start_moves_the_old_library_to_the_players_folder() {
        let data = scratch("first-start");
        let save = old_save(&data);
        let dirs = DataDirs::new(&data);
        let user = UserDirs::new(data.join("Docs").join("Gran Turismo 4"), UserRootSource::Override);
        let opened = open(&dirs, &user, &GT4, Some(&data)).unwrap();
        assert!(opened.notice.is_none());
        assert!(opened.log.iter().any(|line| line.contains("moved 3 files")), "{:?}", opened.log);
        assert_eq!(saves(&opened.library).unwrap()[0].label, "Active save");
        // Play builds the card from the moved save.
        let card = dirs.runtime_card();
        prepare_card(&opened.library, &card, Language::fallback()).unwrap();
        let built = Card::from_image(&fs::read(&card).unwrap()).unwrap().root().unwrap();
        assert!(gt4saves::card::same_contents(&built, std::slice::from_ref(&save)));
        let parked: Vec<_> = fs::read_dir(&data).unwrap().filter_map(|e| e.ok()).filter(|e| e.file_name().to_string_lossy().starts_with("library.migrated-")).collect();
        assert_eq!(parked.len(), 1, "the old folders are parked, not deleted");
        // The guard now refuses the player's folder for install and update writes.
        assert!(gt4l_core::guard::check_write(&data, &user.saves().join("x")).is_err());
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn an_unusable_players_folder_falls_back_to_the_data_folder() {
        let data = scratch("fallback");
        old_save(&data);
        let dirs = DataDirs::new(&data);
        // A file where the folder should be: the library cannot be made there.
        let blocked = data.join("blocked-docs");
        fs::write(&blocked, b"not a folder").unwrap();
        let user = UserDirs::new(blocked.join("Gran Turismo 4"), UserRootSource::AppData);
        let opened = open(&dirs, &user, &GT4, Some(&data)).unwrap();
        assert_eq!(opened.user.root(), data.join("Gran Turismo 4"));
        assert!(opened.notice.is_some());
        assert_eq!(saves(&opened.library).unwrap().len(), 1, "the old save was moved to the fallback folder");
        let _ = fs::remove_dir_all(&data);
    }
}

#[cfg(test)]
mod page_shape_tests {
    use super::Item;

    #[test]
    fn an_item_has_the_fields_the_page_reads() {
        let item = Item { name: "Save 02".into(), label: "Save 02".into(), active: false, modified: "2026-10-21T12:00:00Z".into(), renamed: false, game: String::new() };
        let json = serde_json::to_value(&item).unwrap();
        assert_eq!(
            json,
            serde_json::json!({ "id": "Save 02", "slot": "Save 02", "name": "Save 02", "active": false, "modified": "2026-10-21T12:00:00Z", "renamed": false, "game": "" })
        );
        // A save of a game carries the game in its id, so the page's actions reach that game's library.
        let spec2 = Item { game: "spec2".into(), ..item };
        let json = serde_json::to_value(&spec2).unwrap();
        assert_eq!(json["id"], "spec2:Save 02");
        assert_eq!(json["slot"], "Save 02");
        assert_eq!(json["game"], "spec2");
    }
}
