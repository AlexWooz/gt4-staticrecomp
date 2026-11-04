//! The save library (TASK-025; Documents layout TASK-081, owner 2026-10-21):
//!
//! ```text
//! <saves>/Active/<game save>      the active game save (the one the next Play uses)
//! <saves>/Save NN/<game save>     inactive game saves, one slot per folder (ids only grow)
//! <saves>/.backup/                copies of the active save before a harvest replaces it (the newest 10)
//! <replays>/<replay>              every replay the library keeps
//! <backups>/                      card images written before they are replaced (the newest 10, and the newest 5
//!                                 taken before a New save)
//! ```
//!
//! The launcher puts `<saves>` and `<replays>` in `Documents\Gran Turismo 4\Saves` and `...\Replays`
//! (see `gt4l_core::paths::UserDirs`) and `<backups>` in the AppData data folder. [`Library::open`] keeps all
//! three under one root (tests and tools).
//!
//! Rules: an import never overwrites (it adds a slot or skips an identical item); a harvest
//! never deletes a save or a replay (only backup copies beyond the limits of [`Library::prune_backups`]); the card built
//! for Play holds exactly one game save and the chosen replays.
//! Every item is written to a staging folder and renamed into place (`folder::export_item`).

use crate::card::{self, Card, CardError, Node};
use crate::names::SaveNames;
use crate::folder::{self, FolderError};
use crate::psu;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub enum LibraryError {
    Io(io::Error),
    Card(CardError),
    Folder(FolderError),
    /// The named item does not exist in the library.
    NotFound(String),
    /// The item is not something this library stores (for example another game's save).
    NotGtSave(String),
    /// A card with no Gran Turismo 4 save or replay on it; `found` lists the names it holds.
    NoGtSave { found: Vec<String> },
    /// A display name that cannot be used (empty after cleaning).
    Name(String),
}

impl From<io::Error> for LibraryError {
    fn from(error: io::Error) -> Self {
        LibraryError::Io(error)
    }
}

impl From<CardError> for LibraryError {
    fn from(error: CardError) -> Self {
        LibraryError::Card(error)
    }
}

impl From<FolderError> for LibraryError {
    fn from(error: FolderError) -> Self {
        LibraryError::Folder(error)
    }
}

impl fmt::Display for LibraryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LibraryError::Io(error) => write!(f, "{error}"),
            LibraryError::Card(error) => write!(f, "{error}"),
            LibraryError::Folder(error) => write!(f, "{error}"),
            LibraryError::NotFound(name) => write!(f, "not in the library: {name}"),
            LibraryError::NotGtSave(name) => write!(f, "not a Gran Turismo 4 save: {name}"),
            LibraryError::NoGtSave { found } => {
                write!(f, "no Gran Turismo 4 save on this card (found: {})", found.join(", "))
            }
            LibraryError::Name(reason) => write!(f, "{reason}"),
        }
    }
}

impl std::error::Error for LibraryError {}

/// What an import or harvest did, for the Saves and Replays pages.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Report {
    /// Slots created ("Save NN") or replays added, by name.
    pub added: Vec<String>,
    /// Items already present with the same contents (nothing written).
    pub unchanged: Vec<String>,
    /// Items refused, with the reason.
    pub skipped: Vec<String>,
    /// The active game save was replaced; the old copy went to `<saves>/.backup`.
    pub replaced_active: bool,
}

/// `Save 01`, `Save 02`, ...
pub fn slot_name(number: u32) -> String {
    format!("Save {number:02}")
}

/// Folder of the active game save inside the saves folder.
pub const ACTIVE_FOLDER: &str = "Active";
/// Folder of the copies of the active save made before a harvest replaces it, inside the saves folder.
pub const ACTIVE_BACKUP_FOLDER: &str = ".backup";
/// Prefix of an inactive slot folder (`Save 01`, `Save 02`, ...).
pub const SLOT_PREFIX: &str = "Save ";
/// The display name of a save, kept next to the save's game folder (`<slot>/name.txt`, or `Active/name.txt`).
/// The folder names do not change when a save is renamed.
pub const NAME_FILE: &str = "name.txt";
/// Longest display name, in characters.
pub const NAME_MAX_CHARS: usize = 40;

/// A display name made safe to store and show: control characters and the characters Windows refuses in file
/// names are dropped, runs of spaces become one space, the ends are trimmed and the text is cut to
/// [`NAME_MAX_CHARS`]. An empty result is an error.
pub fn clean_name(text: &str) -> Result<String, LibraryError> {
    let kept: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .filter(|c| !matches!(c, '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*'))
        .collect();
    let spaced = kept.split_whitespace().collect::<Vec<_>>().join(" ");
    let cut: String = spaced.chars().take(NAME_MAX_CHARS).collect();
    let cut = cut.trim_end().to_string();
    if cut.is_empty() {
        return Err(LibraryError::Name("the name is empty".to_string()));
    }
    Ok(cut)
}

/// Moves `<from>/name.txt` to `<to>/name.txt` (replacing one there). Nothing happens when `from` has no name.
fn move_name_file(from: &Path, to: &Path) -> Result<(), LibraryError> {
    let source = from.join(NAME_FILE);
    if !source.is_file() {
        return Ok(());
    }
    let target = to.join(NAME_FILE);
    if target.exists() {
        fs::remove_file(&target)?;
    }
    fs::rename(&source, &target)?;
    Ok(())
}

/// Where the three parts of the library live.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LibraryPaths {
    /// Game saves: `Active`, `Save NN` and `.backup`.
    pub saves: PathBuf,
    /// Replays, one folder each.
    pub replays: PathBuf,
    /// Card images kept before the runtime card is replaced (not player-facing).
    pub backups: PathBuf,
}

impl LibraryPaths {
    /// All three under one folder: `<root>/Saves`, `<root>/Replays`, `<root>/backups`.
    pub fn under(root: &Path) -> LibraryPaths {
        LibraryPaths { saves: root.join("Saves"), replays: root.join("Replays"), backups: root.join("backups") }
    }

    /// The library of one game (TASK-101): `<root>/games/<game id>/{Saves,Replays,backups}`. Two games never share a
    /// folder, so a save of one can never be harvested into, or built onto the card of, the other (each game has its
    /// own item names, see [`SaveNames`]).
    pub fn for_game(root: &Path, game_id: &str) -> LibraryPaths {
        LibraryPaths::under(&root.join("games").join(game_id))
    }
}

pub struct Library {
    paths: LibraryPaths,
    names: SaveNames,
}

impl Library {
    /// Opens a library kept under one folder (see [`LibraryPaths::under`]) for the game `names` describes.
    pub fn open(root: &Path, names: SaveNames) -> Result<Library, LibraryError> {
        Library::open_paths(&LibraryPaths::under(root), names)
    }

    /// Opens (and creates, if needed) a library at `paths`.
    pub fn open_paths(paths: &LibraryPaths, names: SaveNames) -> Result<Library, LibraryError> {
        let folders = [
            paths.saves.join(ACTIVE_FOLDER),
            paths.saves.join(ACTIVE_BACKUP_FOLDER),
            paths.replays.clone(),
            paths.backups.clone(),
        ];
        for folder in folders {
            fs::create_dir_all(folder)?;
        }
        Ok(Library { paths: paths.clone(), names })
    }

    pub fn paths(&self) -> &LibraryPaths {
        &self.paths
    }

    /// The item names of the game this library belongs to.
    pub fn names(&self) -> SaveNames {
        self.names
    }

    /// Folder that holds the active game save.
    pub fn active_dir(&self) -> PathBuf {
        self.paths.saves.join(ACTIVE_FOLDER)
    }

    /// Folder of the copies of the active save made before a harvest replaced it.
    pub fn active_backup_dir(&self) -> PathBuf {
        self.paths.saves.join(ACTIVE_BACKUP_FOLDER)
    }

    /// Folder that holds every replay.
    pub fn replays_dir(&self) -> PathBuf {
        self.paths.replays.clone()
    }

    fn saves_dir(&self) -> PathBuf {
        self.paths.saves.clone()
    }

    /// Folder of an inactive slot (`Save NN`).
    pub fn slot_dir(&self, slot: &str) -> PathBuf {
        self.saves_dir().join(slot)
    }

    /// Name of the active game save, if any.
    pub fn active_save(&self) -> Result<Option<String>, LibraryError> {
        let game = self.active_dir().join(self.names.game_save);
        Ok(if game.is_dir() { Some(self.names.game_save.to_string()) } else { None })
    }

    /// `Save NN` folders that hold a game save, sorted by slot number.
    pub fn slots(&self) -> Result<Vec<String>, LibraryError> {
        let mut slots: Vec<String> = fs::read_dir(self.saves_dir())?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().join(self.names.game_save).is_dir())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|name| name.starts_with(SLOT_PREFIX))
            .collect();
        slots.sort();
        Ok(slots)
    }

    /// Every replay the library keeps, in name order.
    pub fn replays(&self) -> Result<Vec<String>, LibraryError> {
        let mut names: Vec<String> = fs::read_dir(self.replays_dir())?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| self.names.is_replay(n))
            .collect();
        names.sort();
        Ok(names)
    }

    /// True when the active save or a slot already holds these exact contents.
    fn holds_identical_game_save(&self, node: &Node) -> Result<bool, LibraryError> {
        let mut candidates = vec![self.active_dir().join(self.names.game_save)];
        for slot in self.slots()? {
            candidates.push(self.saves_dir().join(slot).join(self.names.game_save));
        }
        for path in candidates.into_iter().filter(|p| p.is_dir()) {
            let existing = folder::import_item(&path)?;
            if card::same_contents(std::slice::from_ref(&existing), std::slice::from_ref(node)) {
                return Ok(true);
            }
        }
        Ok(false)
    }

    /// First unused replay name `BASCUS-97328Rnnnn` in the replays folder (letter R, four digits). Used only when two
    /// different replays share a name; Spec II's own names are seven characters, and whether Spec II lists a replay named
    /// this way is not verified.
    fn next_free_replay_name(&self) -> Result<String, LibraryError> {
        for number in 1..=9999 {
            let name = self.names.full_replay_name(number);
            if !self.replays_dir().join(&name).exists() {
                return Ok(name);
            }
        }
        Err(LibraryError::Card(CardError::Full))
    }

    fn next_slot_name(&self) -> Result<String, LibraryError> {
        let mut highest = 0;
        for entry in fs::read_dir(self.saves_dir())? {
            let name = entry?.file_name().to_string_lossy().into_owned();
            if let Some(number) = name.strip_prefix(SLOT_PREFIX).and_then(|n| n.parse::<u32>().ok()) {
                highest = highest.max(number);
            }
        }
        Ok(slot_name(highest + 1))
    }

    /// Adds `node` (a game save or replay) to the library. Returns the name it was stored under.
    fn add_item(&self, node: &Node, report: &mut Report) -> Result<(), LibraryError> {
        let name = node.name();
        if self.names.is_game_save(&name) {
            if self.holds_identical_game_save(node)? {
                report.unchanged.push(name);
                return Ok(());
            }
            // Every other game save becomes a new slot; the active save never changes on import.
            let slot = self.next_slot_name()?;
            let slot_dir = self.saves_dir().join(&slot);
            fs::create_dir_all(&slot_dir)?;
            folder::export_item(&slot_dir, node)?;
            report.added.push(slot);
        } else if self.names.is_replay(&name) {
            let existing = self.replays_dir().join(&name);
            if existing.is_dir() && card::same_contents(std::slice::from_ref(&folder::import_item(&existing)?), std::slice::from_ref(node)) {
                report.unchanged.push(name);
            } else if existing.is_dir() {
                // Same name, other contents: keep both, under a fresh name.
                let renamed = self.next_free_replay_name()?;
                let mut copy = node.clone();
                copy.entry.set_name(&renamed)?;
                folder::export_item(&self.replays_dir(), &copy)?;
                report.added.push(renamed);
            } else {
                folder::export_item(&self.replays_dir(), node)?;
                report.added.push(name);
            }
        } else {
            report.skipped.push(name);
        }
        Ok(())
    }

    /// Imports the items this library owns (its game save and replays) from `items`. Items of other games, and items no
    /// game knows, are not stored: they come back in `Report::skipped` by name. When none of the items is this game's,
    /// the import fails with [`LibraryError::NoGtSave`].
    pub fn import_items(&self, items: &[Node]) -> Result<Report, LibraryError> {
        if !items.iter().any(|item| self.names.owns(&item.name())) {
            return Err(LibraryError::NoGtSave { found: items.iter().map(Node::name).collect() });
        }
        let mut report = Report::default();
        for item in items {
            self.add_item(item, &mut report)?;
        }
        Ok(report)
    }

    /// Imports every item of a card image (`.ps2`, `.mcd`, `.bin`, with or without spare bytes).
    pub fn import_card_image(&self, bytes: &[u8]) -> Result<Report, LibraryError> {
        self.import_items(&items_of_card_image(bytes)?)
    }

    /// Imports a `.psu` save file.
    pub fn import_psu(&self, bytes: &[u8]) -> Result<Report, LibraryError> {
        self.import_items(&[psu::read(bytes)?])
    }

    /// Imports a PCSX2 folder memory card: each save sub-folder is a raw directory.
    pub fn import_folder_card(&self, folder_card: &Path) -> Result<Report, LibraryError> {
        self.import_items(&items_of_folder_card(folder_card)?)
    }

    /// The folder a save's name file lives in: the active save's folder for the game save, a slot's for `Save NN`.
    fn save_folder(&self, name: &str) -> Option<PathBuf> {
        if name == self.names.game_save {
            Some(self.active_dir())
        } else if name.starts_with(SLOT_PREFIX) {
            Some(self.slot_dir(name))
        } else {
            None
        }
    }

    /// The display name a player gave a save, if any. `name` is the library name (`Save NN` or the game save name).
    pub fn display_name(&self, name: &str) -> Option<String> {
        let file = self.save_folder(name)?.join(NAME_FILE);
        let text = fs::read_to_string(file).ok()?;
        let cleaned = text.trim();
        if cleaned.is_empty() {
            None
        } else {
            Some(cleaned.to_string())
        }
    }

    /// Gives a save (`Save NN` or the active save) a display name. The folder names do not change. Returns the name
    /// as stored, after [`clean_name`].
    pub fn rename_save(&self, name: &str, new_name: &str) -> Result<String, LibraryError> {
        let cleaned = clean_name(new_name)?;
        let folder_path = self.save_folder(name).ok_or_else(|| LibraryError::NotFound(name.to_string()))?;
        if !folder_path.join(self.names.game_save).is_dir() {
            return Err(LibraryError::NotFound(name.to_string()));
        }
        let file = folder_path.join(NAME_FILE);
        let temporary = folder_path.join("name.tmp");
        fs::write(&temporary, cleaned.as_bytes())?;
        if file.exists() {
            fs::remove_file(&file)?;
        }
        fs::rename(&temporary, &file)?;
        Ok(cleaned)
    }

    /// Starts a new save: the active save (if any) is parked in a new `Save NN` slot and `Active` is left empty.
    /// The next Play builds a card without a game save, so the game creates a new profile. Returns the slot the
    /// old save went to, or `None` when there was no active save.
    pub fn new_save(&self) -> Result<Option<String>, LibraryError> {
        let target = self.active_dir().join(self.names.game_save);
        if !target.is_dir() {
            // Nothing to park; a name left behind by a removed save must not stick to the next one.
            self.clear_active_name()?;
            return Ok(None);
        }
        let parked = self.next_slot_name()?;
        let parked_dir = self.saves_dir().join(&parked);
        fs::create_dir_all(&parked_dir)?;
        fs::rename(&target, parked_dir.join(self.names.game_save))?;
        move_name_file(&self.active_dir(), &parked_dir)?;
        Ok(Some(parked))
    }

    /// Folder of the active game save (`Active/<game save>`); it may not exist.
    pub fn active_save_dir(&self) -> PathBuf {
        self.active_dir().join(self.names.game_save)
    }

    /// Removes the display name of the active save, so a name left behind by a removed save does not stick to the next one.
    /// The launcher calls it after moving the active save to the Recycle Bin. Nothing to do when there is no name.
    pub fn clear_active_name(&self) -> Result<(), LibraryError> {
        let stale = self.active_dir().join(NAME_FILE);
        if stale.is_file() {
            fs::remove_file(stale)?;
        }
        Ok(())
    }

    /// `.psu` bytes of a library item (`Active` for the active save, a slot name such as "Save 02", or a replay name).
    pub fn export_psu(&self, name: &str) -> Result<Vec<u8>, LibraryError> {
        let folder_path = if name == self.names.game_save {
            self.active_dir().join(self.names.game_save)
        } else if name.starts_with(SLOT_PREFIX) {
            self.slot_dir(name).join(self.names.game_save)
        } else {
            self.replays_dir().join(name)
        };
        if !folder_path.is_dir() {
            return Err(LibraryError::NotFound(name.to_string()));
        }
        let item = folder::import_item(&folder_path)?;
        Ok(psu::write(&item)?)
    }

    /// Makes `slot` the active game save. The current active save (if any) gets a new slot.
    pub fn activate_slot(&self, slot: &str) -> Result<(), LibraryError> {
        let source = self.saves_dir().join(slot).join(self.names.game_save);
        if !source.is_dir() {
            return Err(LibraryError::NotFound(slot.to_string()));
        }
        let target = self.active_dir().join(self.names.game_save);
        if target.is_dir() {
            let parked = self.next_slot_name()?;
            let parked_dir = self.saves_dir().join(&parked);
            fs::create_dir_all(&parked_dir)?;
            fs::rename(&target, parked_dir.join(self.names.game_save))?;
            move_name_file(&self.active_dir(), &parked_dir)?;
        }
        fs::rename(&source, &target)?;
        // The display name goes with its save; the slot folder is then empty.
        move_name_file(&self.saves_dir().join(slot), &self.active_dir())?;
        fs::remove_dir(self.saves_dir().join(slot))?;
        Ok(())
    }

    /// Builds the card image for the next Play: the active game save plus `replays`.
    /// The image is verified by reading it back before it is returned.
    pub fn build_card(&self, replays: &[String]) -> Result<Vec<u8>, LibraryError> {
        let mut items = Vec::new();
        if self.active_save()?.is_some() {
            items.push(folder::import_item(&self.active_dir().join(self.names.game_save))?);
        }
        for name in replays {
            let path = self.replays_dir().join(name);
            if !path.is_dir() {
                return Err(LibraryError::NotFound(name.clone()));
            }
            items.push(folder::import_item(&path)?);
        }
        let card = Card::build(&items)?;
        let bytes = card.to_image();
        let readback = Card::from_image(&bytes)?.root()?;
        if !card::same_contents(&readback, &items) {
            return Err(LibraryError::Card(CardError::Corrupt("built card does not read back identical".into())));
        }
        Ok(bytes)
    }

    /// Copies the card image written by a Play back into the library, without deleting
    /// anything: a changed active save replaces the old one (the old copy goes to
    /// `<saves>/.backup`); new replays are added.
    pub fn harvest(&self, card_bytes: &[u8]) -> Result<Report, LibraryError> {
        let card = Card::from_image(card_bytes)?;
        let mut report = Report::default();
        for item in card.root()? {
            let name = item.name();
            if self.names.is_game_save(&name) {
                let current = self.active_dir().join(&name);
                if current.is_dir() {
                    let before = folder::import_item(&current)?;
                    if card::same_contents(std::slice::from_ref(&before), std::slice::from_ref(&item)) {
                        report.unchanged.push(name);
                        continue;
                    }
                    let backup = self.active_backup_dir();
                    let stamp = crate::timestamp();
                    let copy_dir = backup.join(format!("{name}-{stamp}"));
                    fs::create_dir_all(&copy_dir)?;
                    folder::export_item(&copy_dir, &before)?;
                    // The oldest copies beyond KEPT_ACTIVE_COPIES go; a copy that cannot be removed now is tried again later.
                    let _ = self.prune_backups();
                    report.replaced_active = true;
                }
                folder::export_item(&self.active_dir(), &item)?;
                report.added.push(name);
            } else if self.names.is_replay(&name) {
                self.add_item(&item, &mut report)?;
            } else {
                report.skipped.push(name);
            }
        }
        Ok(report)
    }

    /// Writes a card image to `path`: the previous file (if any) is copied to backups/ first,
    /// and the new image is written to a temporary file and renamed into place.
    pub fn write_card_image(&self, path: &Path, bytes: &[u8]) -> Result<(), LibraryError> {
        if path.is_file() {
            fs::create_dir_all(&self.paths.backups)?;
            let backup = self.paths.backups.join(format!("{}-{}", crate::timestamp(), file_name(path)));
            fs::copy(path, backup)?;
        }
        let temporary = path.with_extension("tmp");
        fs::write(&temporary, bytes)?;
        fs::rename(&temporary, path)?;
        // An old backup that cannot be removed (open in another program, say) must not stop the game from starting: it is
        // tried again at the next Play.
        let _ = self.prune_backups();
        Ok(())
    }

    /// Keeps the newest backups of each kind and deletes the older ones (owner, 2026-11-03: every Play added a card image of
    /// about 8 MB to `<backups>` and nothing removed them, 74 images in two weeks on the owner's PC). The kinds, by name:
    /// card images `<timestamp>-<card file>.ps2` ([`KEPT_CARD_BACKUPS`]), images taken before a New save or before the active save is deleted
    /// `<timestamp>-before-new-save.ps2`, `<timestamp>-before-delete.ps2` ([`KEPT_NEW_SAVE_BACKUPS`]), and copies of the active save
    /// `<saves>/.backup/<game save>-<timestamp>` ([`KEPT_ACTIVE_COPIES`]). A name of any other form is never touched.
    /// Returns how many were deleted.
    pub fn prune_backups(&self) -> Result<usize, LibraryError> {
        let mut deleted = 0;
        let backups = &self.paths.backups;
        if backups.is_dir() {
            deleted += keep_newest(backups, KEPT_CARD_BACKUPS, |name| is_card_backup(name), |path| fs::remove_file(path))?;
            deleted += keep_newest(backups, KEPT_NEW_SAVE_BACKUPS, |name| is_new_save_backup(name), |path| fs::remove_file(path))?;
        }
        let copies = self.active_backup_dir();
        if copies.is_dir() {
            let game_save = self.names.game_save;
            deleted += keep_newest(&copies, KEPT_ACTIVE_COPIES, |name| is_active_copy(name, game_save), |path| fs::remove_dir_all(path))?;
        }
        Ok(deleted)
    }
}

/// Card images kept in `<backups>` (about 8 MB each).
pub const KEPT_CARD_BACKUPS: usize = 10;
/// Card images taken before a New save, kept in `<backups>`.
pub const KEPT_NEW_SAVE_BACKUPS: usize = 5;
/// Copies of the active save kept in `<saves>/.backup`.
pub const KEPT_ACTIVE_COPIES: usize = 10;
/// The end of the name of a card image taken before a New save (the launcher's `new_save`).
pub const NEW_SAVE_BACKUP_SUFFIX: &str = "-before-new-save.ps2";
/// The end of the name of a card image taken before the active save is deleted (kept with the New save images).
pub const DELETE_BACKUP_SUFFIX: &str = "-before-delete.ps2";

/// True for `YYYYMMDD-HHMMSS`, the form of [`crate::timestamp`].
fn is_timestamp(text: &str) -> bool {
    let bytes = text.as_bytes();
    bytes.len() == 15 && bytes.iter().enumerate().all(|(index, byte)| if index == 8 { *byte == b'-' } else { byte.is_ascii_digit() })
}

/// `<timestamp>-<card file>.ps2`, not taken before a New save.
fn is_card_backup(name: &str) -> bool {
    name.get(..15).is_some_and(is_timestamp)
        && name.get(15..16) == Some("-")
        && name.len() > 20
        && name.ends_with(".ps2")
        && !name.ends_with(NEW_SAVE_BACKUP_SUFFIX)
        && !name.ends_with(DELETE_BACKUP_SUFFIX)
}

/// `<timestamp>-before-new-save.ps2` or `<timestamp>-before-delete.ps2`.
fn is_new_save_backup(name: &str) -> bool {
    name.strip_suffix(NEW_SAVE_BACKUP_SUFFIX).or_else(|| name.strip_suffix(DELETE_BACKUP_SUFFIX)).is_some_and(is_timestamp)
}

/// `<game save>-<timestamp>`.
fn is_active_copy(name: &str, game_save: &str) -> bool {
    name.strip_prefix(game_save).and_then(|rest| rest.strip_prefix('-')).is_some_and(is_timestamp)
}

/// Deletes all but the `keep` newest entries of `dir` whose names `kind` accepts, and returns how many it deleted. Every kind
/// has a fixed part and a timestamp in the same place, so name order is age order.
fn keep_newest(
    dir: &Path,
    keep: usize,
    kind: impl Fn(&str) -> bool,
    remove: impl Fn(&Path) -> io::Result<()>,
) -> Result<usize, LibraryError> {
    let mut names: Vec<String> = fs::read_dir(dir)?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.file_name().to_string_lossy().into_owned())
        .filter(|name| kind(name))
        .collect();
    if names.len() <= keep {
        return Ok(0);
    }
    names.sort();
    let excess = names.len() - keep;
    for name in &names[..excess] {
        remove(&dir.join(name))?;
    }
    Ok(excess)
}

/// The items of a card image (`.ps2`, `.mcd`, `.bin`, with or without spare bytes), whichever game they belong to.
pub fn items_of_card_image(bytes: &[u8]) -> Result<Vec<Node>, LibraryError> {
    Ok(Card::from_image(bytes)?.root()?)
}

/// The item of a `.psu` save file.
pub fn items_of_psu(bytes: &[u8]) -> Result<Vec<Node>, LibraryError> {
    Ok(vec![psu::read(bytes)?])
}

/// The items of a PCSX2 folder memory card: each save sub-folder is a raw directory.
pub fn items_of_folder_card(folder_card: &Path) -> Result<Vec<Node>, LibraryError> {
    let mut dirs: Vec<PathBuf> = fs::read_dir(folder_card)?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    dirs.sort();
    dirs.iter().map(|dir| Ok(folder::import_raw_directory(dir)?)).collect()
}

fn file_name(path: &Path) -> String {
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "card".into())
}
