// Library rules for the Saves page (TASK-104): New save, Export of the active save, Rename and the name kept with a save.

mod common;

use gt4saves::SaveNames;
#[allow(dead_code)]
const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
#[allow(dead_code)]
const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
#[allow(dead_code)]
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
use common::*;
use gt4saves::card::{same_contents, Node};
use gt4saves::library::{clean_name, LibraryError, NAME_MAX_CHARS};
use gt4saves::{psu, Card, Library};
use std::fs;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn card_of(items: &[Node]) -> Vec<u8> {
    Card::build(items).unwrap().to_image()
}

fn game_names(bytes: &[u8]) -> Vec<String> {
    Card::from_image(bytes).unwrap().root().unwrap().iter().map(Node::name).collect()
}

/// A library whose active save is `seed`'s save, after an import and an activation.
fn library_with_active(name: &str, seed: u8) -> Library {
    let library = Library::open(&scratch(name), GT4).unwrap();
    library.import_card_image(&card_of(&[game_save(seed)])).unwrap();
    library.activate_slot("Save 01").unwrap();
    library
}

#[test]
fn new_save_parks_the_active_save_and_the_next_card_has_no_game_save() {
    let library = library_with_active("new-save", 1);
    assert_eq!(library.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));

    let parked = library.new_save().unwrap();
    assert_eq!(parked.as_deref(), Some("Save 01"));
    assert_eq!(library.active_save().unwrap(), None);
    assert_eq!(library.slots().unwrap(), ["Save 01"]);

    // Play with an empty Active builds a card with the replays only.
    let card = library.build_card(&[]).unwrap();
    assert!(!game_names(&card).contains(&GT4_GAME_SAVE.to_string()));
    let library_replay = replay(7);
    let with_replay = Card::build(&[library_replay.clone()]).unwrap().to_image();
    library.import_card_image(&with_replay).unwrap();
    let card = library.build_card(&["BASCUS-97328R0007".to_string()]).unwrap();
    assert_eq!(game_names(&card), ["BASCUS-97328R0007"]);
}

#[test]
fn new_save_with_no_active_save_parks_nothing() {
    let library = Library::open(&scratch("new-save-empty"), GT4).unwrap();
    assert_eq!(library.new_save().unwrap(), None);
    assert!(library.slots().unwrap().is_empty());
}

#[test]
fn harvest_fills_an_empty_active_save_and_parks_nothing_else() {
    let library = library_with_active("harvest-after-new", 1);
    library.new_save().unwrap();
    // The game made a new profile and wrote the card with it.
    let written = card_of(&[game_save(7)]);
    let report = library.harvest(&written).unwrap();
    assert_eq!(report.added, [GT4_GAME_SAVE.to_string()]);
    assert!(!report.replaced_active, "nothing was replaced: Active was empty");
    assert_eq!(library.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));
    assert_eq!(library.slots().unwrap(), ["Save 01"], "the parked save is still there");
    // A harvest of a card with no game save changes nothing.
    let empty = card_of(&[]);
    let report = library.harvest(&empty).unwrap();
    assert!(report.added.is_empty());
}

#[test]
fn an_import_of_a_card_with_no_gt4_item_names_what_it_found() {
    let library = Library::open(&scratch("no-gt4"), GT4).unwrap();
    let other = dir("SLUS-20000GAMEDATA", vec![file("icon.sys", pattern(964, 9))]);
    match library.import_card_image(&card_of(&[other])) {
        Err(LibraryError::NoGtSave { found }) => assert_eq!(found, ["SLUS-20000GAMEDATA"]),
        other => panic!("expected NoGtSave, got {other:?}"),
    }
    match library.import_card_image(&card_of(&[])) {
        Err(LibraryError::NoGtSave { found }) => assert!(found.is_empty()),
        other => panic!("expected NoGtSave for an empty card, got {other:?}"),
    }
}

#[test]
fn export_writes_the_active_save_as_a_psu_file() {
    let library = library_with_active("export-active", 2);
    let bytes = library.export_psu(GT4_GAME_SAVE).unwrap();
    let node = psu::read(&bytes).unwrap();
    assert_eq!(node.name(), GT4_GAME_SAVE);
    assert!(same_contents(std::slice::from_ref(&node), &[game_save(2)]));
    library.new_save().unwrap();
    assert!(matches!(library.export_psu(GT4_GAME_SAVE), Err(LibraryError::NotFound(_))));
}

#[test]
fn rename_sets_a_clean_name_and_keeps_the_folder_name() {
    let library = library_with_active("rename", 1);
    library.new_save().unwrap();
    let stored = library.rename_save("Save 01", "  My   save: #1  ").unwrap();
    assert_eq!(stored, "My save #1");
    assert_eq!(library.display_name("Save 01").as_deref(), Some("My save #1"));
    assert!(library.slot_dir("Save 01").join(GT4_GAME_SAVE).is_dir(), "the folder name is unchanged");

    let long = "x".repeat(60);
    let stored = library.rename_save("Save 01", &long).unwrap();
    assert_eq!(stored.chars().count(), NAME_MAX_CHARS);

    assert!(matches!(library.rename_save("Save 01", " :: "), Err(LibraryError::Name(_))));
    assert!(matches!(library.rename_save("Save 09", "Nope"), Err(LibraryError::NotFound(_))));
}

#[test]
fn the_active_save_can_be_renamed() {
    let library = library_with_active("rename-active", 1);
    library.rename_save(GT4_GAME_SAVE, "Race").unwrap();
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Race"));
}

#[test]
fn a_name_goes_with_its_save_when_saves_are_swapped() {
    let library = library_with_active("names-swap", 1);
    library.rename_save(GT4_GAME_SAVE, "Alpha").unwrap();
    library.new_save().unwrap(); // Alpha parks as Save 01
    assert_eq!(library.display_name("Save 01").as_deref(), Some("Alpha"));
    library.import_card_image(&card_of(&[game_save(5)])).unwrap(); // Save 02, unnamed
    library.rename_save("Save 02", "Beta").unwrap();
    library.activate_slot("Save 02").unwrap(); // Beta becomes active; the empty Active had no name
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Beta"));
    library.activate_slot("Save 01").unwrap(); // Alpha back; Beta parks as Save 02 with its name
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Alpha"));
    assert_eq!(library.display_name("Save 02").as_deref(), Some("Beta"));
}

#[test]
fn clean_name_drops_what_a_file_name_cannot_hold() {
    assert_eq!(clean_name("a\tb").unwrap(), "a b");
    assert_eq!(clean_name("<>|?*\"/\\:x").unwrap(), "x");
    assert_eq!(clean_name("Corrida Fácil").unwrap(), "Corrida Fácil");
    assert!(clean_name("  \u{7}  ").is_err());
}
