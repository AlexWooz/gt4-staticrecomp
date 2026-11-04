//! The Saves and Replays pages and the card around each Play, on top of the `gt4saves` library.
//!
//! The library lives in `<data folder>\library`. Before a start the launcher writes the card the game
//! reads (`runtime\card.ps2`) from the library; after the game exits it harvests that card back into
//! the library. Imports never overwrite; deletes go to the Recycle Bin (see `recycle`).

use std::fs;
use std::path::{Path, PathBuf};

use gt4saves::SaveNames;

/// The Gran Turismo 4 item names (the command-line tool works on the retail game only).
const GAME_SAVE_NAME: &str = "BASCUS-97328GAMEDATA";
const GT4_NAMES: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
use gt4saves::library::{Library, Report};

use crate::recycle;

/// Folder name of the library under the data folder.
pub const LIBRARY_FOLDER: &str = "library";

/// One row of the Saves or Replays page.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    /// Library name: `Save NN` for an inactive slot, the game's save name, or a replay name.
    pub name: String,
    pub label: String,
    /// The save the next start uses.
    pub active: bool,
}

/// Opens (and creates, if needed) the library under `data_root`.
pub fn open(data_root: &Path) -> Result<(PathBuf, Library), String> {
    let root = data_root.join(LIBRARY_FOLDER);
    let library = Library::open(&root, GT4_NAMES).map_err(|error| format!("save library: {error}"))?;
    Ok((root, library))
}

/// The active save first, then the inactive slots in slot order.
pub fn saves(library: &Library) -> Result<Vec<Item>, String> {
    let mut items = Vec::new();
    if library.active_save().map_err(|error| error.to_string())?.is_some() {
        items.push(Item { name: GAME_SAVE_NAME.to_string(), label: "Active save".to_string(), active: true });
    }
    for slot in library.slots().map_err(|error| error.to_string())? {
        items.push(Item { label: slot.clone(), name: slot, active: false });
    }
    Ok(items)
}

/// Every replay the library keeps, in name order.
pub fn replays(library: &Library) -> Result<Vec<Item>, String> {
    Ok(library
        .replays()
        .map_err(|error| error.to_string())?
        .into_iter()
        .map(|name| Item { label: name.clone(), name, active: false })
        .collect())
}

/// Imports a file the user picked: a `.psu` save, a card image (`.ps2`, `.mcd`, `.bin`) or a folder card.
pub fn import_path(library: &Library, path: &Path) -> Result<Report, String> {
    let result = if path.is_dir() {
        library.import_folder_card(path)
    } else {
        let extension = path.extension().map(|ext| ext.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
        let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
        match extension.as_str() {
            "psu" => library.import_psu(&bytes),
            "ps2" | "mcd" | "bin" | "card" => library.import_card_image(&bytes),
            other => return Err(format!("'.{other}' is not a save or card file")),
        }
    };
    result.map_err(|error| error.to_string())
}

/// Makes a slot the active save. The save that was active moves to a new slot.
pub fn activate(library: &Library, slot: &str) -> Result<(), String> {
    library.activate_slot(slot).map_err(|error| error.to_string())
}

/// Writes the card for the next start: the active save and every replay, read back before it is
/// written. A card the game left behind is harvested first, so nothing it wrote is lost; the old file
/// is also kept under `backups` by `write_card_image`.
pub fn prepare_card(library: &Library, runtime_card: &Path) -> Result<(), String> {
    if runtime_card.is_file() {
        if let Ok(bytes) = fs::read(runtime_card) {
            // An unreadable card is not an error here: its bytes are backed up when it is replaced.
            let _ = library.harvest(&bytes);
        }
    }
    let replay_names: Vec<String> = library.replays().map_err(|error| error.to_string())?;
    let image = library.build_card(&replay_names).map_err(|error| match error {
        gt4saves::library::LibraryError::Card(gt4saves::CardError::Full) => {
            "The memory card is full. Delete some replays on the Replays page.".to_string()
        }
        other => format!("the memory card could not be built: {other}"),
    })?;
    if let Some(parent) = runtime_card.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("runtime folder: {error}"))?;
    }
    library.write_card_image(runtime_card, &image).map_err(|error| error.to_string())
}

/// Copies the card the game wrote back into the library. A missing card means nothing was saved.
pub fn harvest_card(library: &Library, runtime_card: &Path) -> Result<Report, String> {
    if !runtime_card.is_file() {
        return Ok(Report::default());
    }
    let bytes = fs::read(runtime_card).map_err(|error| format!("{}: {error}", runtime_card.display()))?;
    library.harvest(&bytes).map_err(|error| error.to_string())
}

/// Sends a library item to the Recycle Bin. Only inactive slots and replays can be deleted here.
pub fn delete(root: &Path, item: &Item) -> Result<(), String> {
    if item.active {
        return Err("The active save cannot be deleted. Activate another save first.".to_string());
    }
    let path = if item.name.starts_with("Save ") {
        root.join("saves").join(&item.name)
    } else {
        root.join("memcard").join(&item.name)
    };
    if !path.is_dir() {
        return Err(format!("{} is not in the library any more", item.label));
    }
    recycle::move_to_recycle_bin(&path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
    use gt4saves::Card;

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
        directory(GAME_SAVE_NAME, vec![file("icon.sys", pattern(964)), file("garage", pattern(20_000))])
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
        let (root, library) = open(&data).unwrap();
        let runtime_card = data.join("runtime").join("card.ps2");
        fs::create_dir_all(runtime_card.parent().unwrap()).unwrap();
        fs::write(&runtime_card, card_with(&[game_save(1), replay()])).unwrap();

        let report = harvest_card(&library, &runtime_card).unwrap();
        assert!(report.added.contains(&GAME_SAVE_NAME.to_string()));
        assert_eq!(names(&saves(&library).unwrap()), vec![GAME_SAVE_NAME.to_string()]);
        assert_eq!(names(&replays(&library).unwrap()), vec![REPLAY.to_string()]);

        fs::remove_file(&runtime_card).unwrap();
        prepare_card(&library, &runtime_card).unwrap();
        let built = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap();
        let built_names: Vec<String> = built.iter().map(|node| node.name()).collect();
        assert_eq!(built_names, vec![GAME_SAVE_NAME.to_string(), REPLAY.to_string()]);
        assert!(root.join("memcard").join(GAME_SAVE_NAME).is_dir());
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn an_import_never_overwrites_and_identical_saves_are_not_repeated() {
        let data = scratch("import");
        let (_, library) = open(&data).unwrap();
        let first = data.join("first.ps2");
        fs::write(&first, card_with(&[game_save(1)])).unwrap();
        let second = data.join("second.ps2");
        fs::write(&second, card_with(&[game_save(2)])).unwrap();

        assert_eq!(import_path(&library, &first).unwrap().added, vec!["Save 01".to_string()]);
        // The same save again is recognised and nothing new is written.
        let again = import_path(&library, &first).unwrap();
        assert!(again.added.is_empty());
        assert_eq!(again.unchanged, vec![GAME_SAVE_NAME.to_string()]);
        // A different save becomes a new slot; the active save is untouched.
        assert_eq!(import_path(&library, &second).unwrap().added, vec!["Save 02".to_string()]);
        assert_eq!(names(&saves(&library).unwrap()), vec!["Save 01".to_string(), "Save 02".to_string()]);
        assert!(saves(&library).unwrap().iter().all(|item| !item.active));
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn activating_a_slot_parks_the_previous_save() {
        let data = scratch("activate");
        let (_, library) = open(&data).unwrap();
        let runtime_card = data.join("runtime").join("card.ps2");
        let source = data.join("source.ps2");
        fs::write(&source, card_with(&[game_save(3)])).unwrap();
        import_path(&library, &source).unwrap();
        // Make the imported save active: the library had no active save, so the slot becomes it.
        activate(&library, "Save 01").unwrap();
        let before = saves(&library).unwrap();
        assert_eq!(before, vec![Item { name: GAME_SAVE_NAME.into(), label: "Active save".into(), active: true }]);

        let other = data.join("other.ps2");
        fs::write(&other, card_with(&[game_save(4)])).unwrap();
        import_path(&library, &other).unwrap();
        activate(&library, "Save 01").unwrap();
        let after = names(&saves(&library).unwrap());
        assert_eq!(after, vec![GAME_SAVE_NAME.to_string(), "Save 02".to_string()]);
        prepare_card(&library, &runtime_card).unwrap();
        let _ = fs::remove_dir_all(&data);
    }

    #[test]
    fn the_active_save_cannot_be_deleted() {
        let data = scratch("delete");
        let (root, library) = open(&data).unwrap();
        let card = data.join("card.ps2");
        fs::write(&card, card_with(&[game_save(5), replay()])).unwrap();
        harvest_card(&library, &card).unwrap();
        let active = Item { name: GAME_SAVE_NAME.into(), label: "Active save".into(), active: true };
        assert!(delete(&root, &active).is_err());
        assert!(root.join("memcard").join(GAME_SAVE_NAME).is_dir(), "refused delete leaves the save");
        let _ = fs::remove_dir_all(&data);
    }
}
