// Round trips and library rules for TASK-025. The owner-card tests read a copy of the owner's
// card (never the original) and skip when the copy is missing.

use gt4saves::SaveNames;
#[allow(dead_code)]
const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
#[allow(dead_code)]
const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
#[allow(dead_code)]
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
use gt4saves::card::{Node, DirEntry, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4saves::library::slot_name;
use gt4saves::{folder, psu, Card, CardError, Library};
use std::fs;
use std::path::PathBuf;

const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
const REPLAY: &str = "BASCUS-97328R0001";

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
}

fn file(name: &str, data: Vec<u8>) -> Node {
    let entry = DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap();
    Node::file(entry, data)
}

fn directory(name: &str, children: Vec<Node>) -> Node {
    let entry = DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
    Node::dir(entry, children)
}

fn game_save(garage_seed: u8) -> Node {
    directory(
        GT4_GAME_SAVE,
        vec![
            file("icon.sys", pattern(964, 1)),
            file("icon0.ico", pattern(9796, 2)),
            file("garage", pattern(1_280_064, garage_seed)),
        ],
    )
}

fn replay() -> Node {
    directory(REPLAY, vec![file("icon.sys", pattern(964, 3)), file("replay.rep", pattern(400_001, 4))])
}

fn owner_card() -> Option<Vec<u8>> {
    let path = std::env::var("GT4SAVES_TEST_CARD")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("C:/Antigravity/gt4-staticrecomp/private/slice180-saves-owner-copy/card-copy.ps2"));
    fs::read(path).ok()
}

#[test]
fn folder_card_round_trips_through_a_built_card() {
    let lib = Library::open(&scratch("folder-card"), GT4).unwrap();
    let card_bytes = Card::build(&[game_save(7), replay()]).unwrap().to_image();
    let report = lib.import_card_image(&card_bytes).unwrap();
    assert_eq!(report.added, vec![slot_name(1), REPLAY.to_string()]);
    lib.activate_slot("Save 01").unwrap();
    assert_eq!(lib.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));

    let rebuilt = lib.build_card(&[REPLAY.to_string()]).unwrap();
    let back = Card::from_image(&rebuilt).unwrap();
    assert!(back.ecc_ok());
    assert!(gt4saves::card::same_contents(
        &back.root().unwrap(),
        &[game_save(7), replay()]
    ));
}

#[test]
fn psu_round_trip_keeps_every_file() {
    let save = game_save(9);
    let bytes = psu::write(&save).unwrap();
    assert_eq!(bytes.len() % 512, 0, "psu is a whole number of 512-byte entries");
    let back = psu::read(&bytes).unwrap();
    assert!(gt4saves::card::same_contents(std::slice::from_ref(&back), std::slice::from_ref(&save)));
}

#[test]
fn psu_import_into_a_second_library_and_export_again() {
    let first = Library::open(&scratch("psu-first"), GT4).unwrap();
    let psu_bytes = psu::write(&game_save(11)).unwrap();
    assert_eq!(first.import_psu(&psu_bytes).unwrap().added, vec![slot_name(1)]);
    let exported = first.export_psu("Save 01").unwrap();
    assert_eq!(exported, psu_bytes, "export of an imported save reproduces the psu bytes");
}

#[test]
fn harvest_replaces_a_changed_save_and_never_deletes() {
    let lib = Library::open(&scratch("harvest"), GT4).unwrap();
    let before = Card::build(&[game_save(20), replay()]).unwrap().to_image();
    lib.import_card_image(&before).unwrap();
    lib.activate_slot("Save 01").unwrap();
    lib.harvest(&before).unwrap();

    // The game wrote a new garage file; the replay was deleted in game (absent from the card).
    let after = Card::build(&[game_save(21)]).unwrap().to_image();
    let report = lib.harvest(&after).unwrap();
    assert!(report.replaced_active);
    let kept = folder::import_item(&lib.active_dir().join(GT4_GAME_SAVE)).unwrap();
    assert!(gt4saves::card::same_contents(std::slice::from_ref(&kept), std::slice::from_ref(&game_save(21))));
    let backup_dirs: Vec<_> = fs::read_dir(lib.active_backup_dir()).unwrap().collect();
    assert_eq!(backup_dirs.len(), 1, "the previous save was copied to .backup");
    assert!(lib.replays_dir().join(REPLAY).is_dir(), "a replay missing from the card is kept, not deleted");
}

// Owner, 2026-11-03: the backups had no limit (74 card images, about 600 MB, in two weeks). Only the newest of each kind
// stay, and names of any other form are left alone.
#[test]
fn backups_keep_only_the_newest_of_each_kind() {
    use gt4saves::library::{KEPT_ACTIVE_COPIES, KEPT_CARD_BACKUPS, KEPT_NEW_SAVE_BACKUPS};
    let root = scratch("prune");
    let lib = Library::open(&root, GT4).unwrap();
    let backups = lib.paths().backups.clone();
    fs::create_dir_all(&backups).unwrap();
    let stamp = |minute: usize| format!("20261103-12{minute:02}00");
    for minute in 0..14 {
        fs::write(backups.join(format!("{}-card.ps2", stamp(minute))), b"card").unwrap();
    }
    for minute in 20..27 {
        fs::write(backups.join(format!("{}-before-new-save.ps2", stamp(minute))), b"card").unwrap();
    }
    for other in ["notes.txt", "2026-card.ps2", "my card.ps2"] {
        fs::write(backups.join(other), b"user").unwrap();
    }
    let copies = lib.active_backup_dir();
    for minute in 30..43 {
        fs::create_dir_all(copies.join(format!("{GT4_GAME_SAVE}-{}", stamp(minute)))).unwrap();
    }
    fs::create_dir_all(copies.join("kept by hand")).unwrap();

    let deleted = lib.prune_backups().unwrap();
    assert_eq!(deleted, (14 - KEPT_CARD_BACKUPS) + (7 - KEPT_NEW_SAVE_BACKUPS) + (13 - KEPT_ACTIVE_COPIES));
    let names = |dir: &PathBuf| -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        names
    };
    let left = names(&backups);
    assert!(!left.contains(&format!("{}-card.ps2", stamp(3))), "the oldest card images go");
    assert!(left.contains(&format!("{}-card.ps2", stamp(4))) && left.contains(&format!("{}-card.ps2", stamp(13))));
    assert!(!left.contains(&format!("{}-before-new-save.ps2", stamp(21))));
    assert!(left.contains(&format!("{}-before-new-save.ps2", stamp(22))));
    for other in ["notes.txt", "2026-card.ps2", "my card.ps2"] {
        assert!(left.contains(&other.to_string()), "{other} is not a backup and stays");
    }
    let copies_left = names(&copies);
    assert_eq!(copies_left.len(), KEPT_ACTIVE_COPIES + 1);
    assert!(copies_left.contains(&"kept by hand".to_string()));
    assert!(!copies_left.contains(&format!("{GT4_GAME_SAVE}-{}", stamp(32))));
    assert_eq!(lib.prune_backups().unwrap(), 0, "a second pass deletes nothing");
}

#[test]
fn writing_a_card_image_prunes_old_backups() {
    use gt4saves::library::KEPT_CARD_BACKUPS;
    let root = scratch("prune-on-write");
    let lib = Library::open(&root, GT4).unwrap();
    let backups = lib.paths().backups.clone();
    fs::create_dir_all(&backups).unwrap();
    for minute in 0..KEPT_CARD_BACKUPS + 3 {
        fs::write(backups.join(format!("20200101-00{minute:02}00-card.ps2")), b"old").unwrap();
    }
    let card = root.join("card.ps2");
    fs::write(&card, b"previous").unwrap();
    lib.write_card_image(&card, b"next").unwrap();
    let count = fs::read_dir(&backups).unwrap().count();
    assert_eq!(count, KEPT_CARD_BACKUPS, "the new backup is kept and the oldest go");
    assert_eq!(fs::read(&card).unwrap(), b"next");
}

#[test]
fn import_dedupes_identical_saves_and_skips_other_games() {
    let lib = Library::open(&scratch("dedupe"), GT4).unwrap();
    let foreign = directory("SLPS-00000GAMEDATA", vec![file("x.bin", pattern(10, 5))]);
    let card = Card::build(&[game_save(30), foreign]).unwrap().to_image();
    let first = lib.import_card_image(&card).unwrap();
    assert_eq!(first.added, vec![slot_name(1)]);
    assert_eq!(first.skipped.len(), 1, "the other game is refused: {:?}", first.skipped);
    let second = lib.import_card_image(&card).unwrap();
    assert!(second.added.is_empty(), "import never adds a duplicate slot");
    assert_eq!(lib.slots().unwrap(), vec![slot_name(1)]);
}

#[test]
fn owner_card_copy_lists_rebuilds_and_matches_byte_for_byte() {
    let Some(bytes) = owner_card() else {
        eprintln!("owner card copy not present; skipping");
        return;
    };
    let lib = Library::open(&scratch("owner"), GT4).unwrap();
    let report = lib.import_card_image(&bytes).unwrap();
    assert_eq!(report.added, vec![slot_name(1)]);
    assert_eq!(lib.slots().unwrap(), vec![slot_name(1)]);
    // Importing the same card again adds nothing.
    assert!(lib.import_card_image(&bytes).unwrap().added.is_empty());

    lib.activate_slot("Save 01").unwrap();
    let rebuilt = lib.build_card(&[]).unwrap();
    let original = Card::from_image(&bytes).unwrap().root().unwrap();
    let copy = Card::from_image(&rebuilt).unwrap().root().unwrap();
    assert!(gt4saves::card::same_contents(&copy, &original));
    // Each file of the game save is identical byte for byte.
    let game_original = original.iter().find(|n| n.name() == GT4_GAME_SAVE).unwrap();
    let game_copy = copy.iter().find(|n| n.name() == GT4_GAME_SAVE).unwrap();
    for file_original in &game_original.children {
        let file_copy = game_copy.children.iter().find(|f| f.name() == file_original.name()).unwrap();
        assert_eq!(file_copy.data, file_original.data, "{} differs", file_original.name());
    }
}

#[test]
fn write_card_image_keeps_the_previous_card_as_a_backup() {
    let lib = Library::open(&scratch("backups"), GT4).unwrap();
    let target = scratch("backups-target").join("card.ps2");
    fs::write(&target, b"previous").unwrap();
    lib.write_card_image(&target, &Card::blank().to_image()).unwrap();
    assert!(Card::from_image(&fs::read(&target).unwrap()).is_ok());
    let backups: Vec<_> = fs::read_dir(&lib.paths().backups).unwrap().collect();
    assert_eq!(backups.len(), 1);
}

#[test]
fn full_card_is_refused_and_bad_images_are_rejected() {
    let big: Vec<Node> = (0..6).map(|i| directory(&format!("BASCUS-97328R{:04}", i + 1), vec![file("replay.rep", pattern(1_500_000, i))])).collect();
    assert_eq!(Card::build(&big).unwrap_err(), CardError::Full);
    assert_eq!(Card::from_image(&[0u8; 100]).unwrap_err(), CardError::BadSize(100));
    assert_eq!(Card::from_image(&vec![0u8; 8_388_608]).unwrap_err(), CardError::NotACard);
}

#[test]
fn folder_export_and_import_keep_nested_directories() {
    let root = scratch("nested");
    let item = directory(REPLAY, vec![file("a.bin", pattern(3000, 6)), directory("sub", vec![file("b.bin", pattern(5, 7))])]);
    let exported = folder::export_item(&root, &item).unwrap();
    assert!(exported.join("sub").join("b.bin").is_file());
    let back = folder::import_item(&exported).unwrap();
    assert!(gt4saves::card::same_contents(std::slice::from_ref(&back), std::slice::from_ref(&item)));
}

#[test]
fn slot_names_and_timestamps() {
    assert_eq!(slot_name(1), "Save 01");
    assert_eq!(slot_name(12), "Save 12");
    let stamp = gt4saves::timestamp();
    assert_eq!(stamp.len(), 15);
    assert_eq!(stamp.as_bytes()[8], b'-');
}

#[test]
fn owner_card_data_only_dump_round_trips() {
    let Some(bytes) = owner_card() else {
        return;
    };
    let card = Card::from_image(&bytes).unwrap();
    let raw = card.to_image_without_ecc();
    let reloaded = Card::from_image(&raw).unwrap();
    assert_eq!(reloaded.to_image_without_ecc(), raw);
    assert!(gt4saves::card::same_contents(&reloaded.root().unwrap(), &card.root().unwrap()));
}

#[test]
fn library_refuses_unknown_names() {
    let lib = Library::open(&scratch("unknown"), GT4).unwrap();
    assert!(matches!(lib.export_psu("Save 09"), Err(gt4saves::LibraryError::NotFound(_))));
    assert!(matches!(lib.activate_slot("Save 09"), Err(gt4saves::LibraryError::NotFound(_))));
}
