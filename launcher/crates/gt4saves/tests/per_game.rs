// One save library per game (TASK-101): the two games write the same PS2 folder name, so nothing may cross.

use gt4saves::SaveNames;
#[allow(dead_code)]
const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
#[allow(dead_code)]
const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
#[allow(dead_code)]
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4saves::{Card, Library, LibraryPaths};
use std::fs;
use std::path::PathBuf;

const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn game_save(seed: u8) -> Node {
    let file = |name: &str, len: usize| {
        let entry = DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap();
        Node::file(entry, (0..len).map(|i| (i as u8).wrapping_add(seed)).collect())
    };
    let entry = DirEntry::new(GT4_GAME_SAVE, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
    Node::dir(entry, vec![file("icon.sys", 964), file("garage", 20_000)])
}

fn card_with(seed: u8) -> Vec<u8> {
    Card::build(&[game_save(seed)]).unwrap().to_image()
}

#[test]
fn the_libraries_of_two_games_are_different_folders() {
    let root = scratch("per-game-paths");
    let gt4 = LibraryPaths::for_game(&root, "gt4");
    let spec2 = LibraryPaths::for_game(&root, "spec2");
    assert_eq!(gt4.saves, root.join("games/gt4/Saves"));
    assert_eq!(spec2.replays, root.join("games/spec2/Replays"));
    assert_eq!(spec2.backups, root.join("games/spec2/backups"));
    assert_ne!(gt4, spec2);
}

#[test]
fn an_import_or_harvest_of_one_game_never_reaches_the_other() {
    let root = scratch("per-game-never-mix");
    let gt4 = Library::open_paths(&LibraryPaths::for_game(&root, "gt4"), GT4).unwrap();
    let spec2 = Library::open_paths(&LibraryPaths::for_game(&root, "spec2"), SPEC2).unwrap();

    // Import into Gran Turismo 4: Spec II sees nothing.
    let report = gt4.import_card_image(&card_with(1)).unwrap();
    assert_eq!(report.added.len(), 1);
    gt4.activate_slot("Save 01").unwrap();
    assert_eq!(gt4.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));
    assert_eq!(spec2.active_save().unwrap(), None);
    assert!(spec2.slots().unwrap().is_empty());

    // The runtime card of each game is built from its own library only.
    let gt4_card = Card::from_image(&gt4.build_card(&[]).unwrap()).unwrap();
    let spec2_card = Card::from_image(&spec2.build_card(&[]).unwrap()).unwrap();
    assert!(gt4saves::card::same_contents(&gt4_card.root().unwrap(), &[game_save(1)]));
    assert!(spec2_card.root().unwrap().is_empty(), "a Spec II card starts without a Gran Turismo 4 save");

    // The game wrote a save on the Spec II card: it is harvested into the Spec II library only.
    // A Gran Turismo 4 save on a Spec II card is refused into the Spec II library (it stays on the card in backups).
    let refused = spec2.harvest(&card_with(2)).unwrap();
    assert_eq!(refused.skipped, vec![GT4_GAME_SAVE.to_string()]);
    assert_eq!(spec2.active_save().unwrap(), None);
    spec2.harvest(&Card::build(&[spec2_save(2)]).unwrap().to_image()).unwrap();
    assert_eq!(spec2.active_save().unwrap().as_deref(), Some("BASCUS-97436GAMEDATA"));
    let still_gt4 = Card::from_image(&gt4.build_card(&[]).unwrap()).unwrap();
    assert!(gt4saves::card::same_contents(&still_gt4.root().unwrap(), &[game_save(1)]), "Gran Turismo 4's save did not change");
    let spec2_now = Card::from_image(&spec2.build_card(&[]).unwrap()).unwrap();
    assert!(gt4saves::card::same_contents(&spec2_now.root().unwrap(), &[spec2_save(2)]));
}

/// A Spec II game save: its own folder name, the same kind of contents.
fn spec2_save(seed: u8) -> Node {
    let file = |name: &str, len: usize| {
        let entry = DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap();
        Node::file(entry, (0..len).map(|i| (i as u8).wrapping_add(seed)).collect())
    };
    let entry = DirEntry::new("BASCUS-97436GAMEDATA", MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
    Node::dir(entry, vec![file("icon.sys", 964), file("garage", 20_000)])
}

#[test]
fn a_spec2_save_is_kept_by_the_spec2_library_and_refused_by_the_gt4_one() {
    let root = scratch("per-game-spec2-save");
    let gt4 = Library::open_paths(&LibraryPaths::for_game(&root, "gt4"), GT4).unwrap();
    let spec2 = Library::open_paths(&LibraryPaths::for_game(&root, "spec2"), SPEC2).unwrap();
    let image = Card::build(&[spec2_save(3)]).unwrap().to_image();

    // Before the fix this save was dropped as "not a Gran Turismo 4 save"; now it is kept by its own game.
    let report = spec2.import_card_image(&image).unwrap();
    assert_eq!(report.added, vec!["Save 01".to_string()]);
    assert!(report.skipped.is_empty());
    assert_eq!(spec2.slots().unwrap(), vec!["Save 01".to_string()]);
    assert!(gt4.import_card_image(&image).is_err(), "a Gran Turismo 4 library refuses a Spec II-only card");
    assert!(gt4.slots().unwrap().is_empty());

    // Harvest and build use the Spec II names: the Spec II card carries the save back.
    spec2.activate_slot("Save 01").unwrap();
    let card = Card::from_image(&spec2.build_card(&[]).unwrap()).unwrap();
    assert!(gt4saves::card::same_contents(&card.root().unwrap(), &[spec2_save(3)]));
    let harvested = spec2.harvest(&Card::build(&[spec2_save(4)]).unwrap().to_image()).unwrap();
    assert_eq!(harvested.added, vec!["BASCUS-97436GAMEDATA".to_string()]);
    assert_eq!(spec2.active_save().unwrap().as_deref(), Some("BASCUS-97436GAMEDATA"));
}

#[test]
fn a_mixed_card_is_split_by_game_and_nothing_is_dropped_silently() {
    let root = scratch("per-game-mixed");
    let gt4 = Library::open_paths(&LibraryPaths::for_game(&root, "gt4"), GT4).unwrap();
    let spec2 = Library::open_paths(&LibraryPaths::for_game(&root, "spec2"), SPEC2).unwrap();
    let image = Card::build(&[game_save(1), spec2_save(2)]).unwrap().to_image();

    // Each library takes its own items; the other game's item is reported as skipped, never lost.
    let gt4_report = gt4.import_items(&gt4saves::library::items_of_card_image(&image).unwrap()).unwrap();
    assert_eq!(gt4_report.added, vec!["Save 01".to_string()]);
    assert_eq!(gt4_report.skipped, vec!["BASCUS-97436GAMEDATA".to_string()]);
    let spec2_report = spec2.import_items(&gt4saves::library::items_of_card_image(&image).unwrap()).unwrap();
    assert_eq!(spec2_report.added, vec!["Save 01".to_string()]);
    assert_eq!(spec2_report.skipped, vec!["BASCUS-97328GAMEDATA".to_string()]);
}

#[test]
fn the_card_names_of_the_two_games_do_not_overlap() {
    let gt4_names = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
    let spec2_names = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
    assert!(!gt4_names.owns("BASCUS-97436GAMEDATA"));
    assert!(!spec2_names.owns("BASCUS-97328R0001"));
}
