// The one-time move of the old AppData library to Documents\Gran Turismo 4 (TASK-081). Every test runs in its own
// scratch folder under the Cargo target dir; nothing here reads or writes a real user folder.

use gt4saves::SaveNames;
#[allow(dead_code)]
const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
#[allow(dead_code)]
const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
#[allow(dead_code)]
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4saves::library::{Library, LibraryPaths};
use gt4saves::migration::{self, copy_file, marker_status, LegacySources, Outcome, Targets};
use gt4saves::{folder, Card};
use std::cell::Cell;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
const REPLAY: &str = "BASCUS-97328R0001";
const RUN_STAMP: &str = "20261021-120000";

struct Fixture {
    data: PathBuf,
    documents_root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let base = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("migration-{name}"));
        let _ = fs::remove_dir_all(&base);
        let data = base.join("AppData").join("GT4Recomp");
        let documents_root = base.join("Documents").join("Gran Turismo 4");
        fs::create_dir_all(&data).unwrap();
        Fixture { data, documents_root }
    }

    fn sources(&self) -> LegacySources {
        LegacySources::of_data_root(&self.data)
    }

    fn targets(&self) -> Targets {
        Targets {
            saves: self.documents_root.join("Saves"),
            replays: self.documents_root.join("Replays"),
            names: GT4,
        }
    }

    fn marker(&self) -> PathBuf {
        self.data.join("config").join("library-migration.json")
    }

    fn run(&self, copier: migration::Copier<'_>) -> Outcome {
        migration::migrate(&self.sources(), &self.targets(), &self.data, &self.marker(), RUN_STAMP, copier)
    }

    fn library_paths(&self) -> LibraryPaths {
        LibraryPaths {
            saves: self.documents_root.join("Saves"),
            replays: self.documents_root.join("Replays"),
            backups: self.data.join("library").join("backups"),
        }
    }

    /// The old layout as launchers before 1.0.0 left it: an active save with a .backup copy, a replay, a slot and
    /// a card-image backup.
    fn old_library(&self) {
        let memcard = self.data.join("library").join("memcard");
        fs::create_dir_all(memcard.join(".backup")).unwrap();
        folder::export_item(&memcard, &game_save(1)).unwrap();
        folder::export_item(&memcard.join(".backup"), &game_save(0)).unwrap();
        folder::export_item(&memcard, &replay()).unwrap();
        let slot = self.data.join("library").join("saves").join("Save 01");
        fs::create_dir_all(&slot).unwrap();
        folder::export_item(&slot, &game_save(2)).unwrap();
        let backups = self.data.join("library").join("backups");
        fs::create_dir_all(&backups).unwrap();
        fs::write(backups.join("20261020-000000-card.ps2"), b"old card image").unwrap();
    }
}

fn file(name: &str, data: Vec<u8>) -> Node {
    Node::file(DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap(), data)
}

fn directory(name: &str, children: Vec<Node>) -> Node {
    Node::dir(DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap(), children)
}

fn game_save(seed: u8) -> Node {
    let pattern = |len: usize| (0..len).map(|i| (i as u8).wrapping_mul(13).wrapping_add(seed)).collect();
    directory(GT4_GAME_SAVE, vec![file("icon.sys", pattern(964)), file("garage", pattern(50_000))])
}

fn replay() -> Node {
    directory(REPLAY, vec![file("icon.sys", vec![1; 964]), file("replay.rep", vec![9; 5_000])])
}

/// Every file under `folder` with its bytes, paths relative to `folder`.
fn snapshot(folder: &Path) -> Vec<(PathBuf, Vec<u8>)> {
    let mut files = Vec::new();
    let mut stack = vec![folder.to_path_buf()];
    while let Some(current) = stack.pop() {
        let Ok(entries) = fs::read_dir(&current) else { continue };
        for entry in entries {
            let path = entry.unwrap().path();
            if path.is_dir() {
                stack.push(path);
            } else {
                files.push((path.strip_prefix(folder).unwrap().to_path_buf(), fs::read(&path).unwrap()));
            }
        }
    }
    files.sort();
    files
}

#[test]
fn a_fresh_install_has_nothing_to_migrate_and_writes_nothing() {
    let fixture = Fixture::new("fresh");
    assert_eq!(fixture.run(&copy_file), Outcome::NothingToMigrate);
    assert!(!fixture.documents_root.exists(), "no Documents folder is made by the migration itself");
    assert!(!fixture.marker().exists());
}

#[test]
fn the_old_library_is_copied_verified_and_parked() {
    let fixture = Fixture::new("migrate");
    fixture.old_library();
    let memcard_before = snapshot(&fixture.data.join("library").join("memcard"));
    let saves_before = snapshot(&fixture.data.join("library").join("saves"));

    let outcome = fixture.run(&copy_file);
    let Outcome::Migrated { files, parked, park_problems, .. } = &outcome else { panic!("{outcome:?}") };
    assert!(park_problems.is_empty(), "{park_problems:?}");
    assert_eq!(*files, memcard_before.len() + saves_before.len());
    assert_eq!(parked, &fixture.data.join(format!("library.migrated-{RUN_STAMP}")));

    // The old folders were renamed with every byte, never deleted.
    assert_eq!(snapshot(&parked.join("memcard")), memcard_before);
    assert_eq!(snapshot(&parked.join("saves")), saves_before);
    assert!(!fixture.data.join("library").join("memcard").exists());
    // The card-image backups stay in AppData.
    assert!(fixture.data.join("library").join("backups").join("20261020-000000-card.ps2").is_file());

    // The new library reads the moved items.
    let library = Library::open_paths(&fixture.library_paths(), GT4).unwrap();
    assert_eq!(library.active_save().unwrap(), Some(GT4_GAME_SAVE.to_string()));
    assert_eq!(library.slots().unwrap(), vec!["Save 01".to_string()]);
    assert_eq!(library.replays().unwrap(), vec![REPLAY.to_string()]);
    let active = folder::import_item(&library.active_dir().join(GT4_GAME_SAVE)).unwrap();
    assert!(gt4saves::card::same_contents(std::slice::from_ref(&active), std::slice::from_ref(&game_save(1))));
    assert!(library.active_backup_dir().join(GT4_GAME_SAVE).is_dir(), "harvest copies moved to Saves\\.backup");

    // Recorded once; a second run does nothing.
    assert_eq!(marker_status(&fixture.marker()).as_deref(), Some("migrated"));
    assert_eq!(fixture.run(&copy_file), Outcome::AlreadyMigrated);
    // No staging folder is left behind.
    let leftovers: Vec<_> = fs::read_dir(&fixture.documents_root)
        .unwrap()
        .filter_map(|entry| entry.ok())
        .filter(|entry| entry.file_name().to_string_lossy().contains("migrating"))
        .collect();
    assert!(leftovers.is_empty());
}

#[test]
fn empty_new_folders_made_by_an_earlier_start_do_not_block_the_move() {
    let fixture = Fixture::new("empty-new");
    fixture.old_library();
    Library::open_paths(&fixture.library_paths(), GT4).unwrap(); // creates empty Saves\Active, Saves\.backup, Replays
    assert!(matches!(fixture.run(&copy_file), Outcome::Migrated { .. }));
}

#[test]
fn both_libraries_with_saves_are_never_merged() {
    let fixture = Fixture::new("conflict");
    fixture.old_library();
    let library = Library::open_paths(&fixture.library_paths(), GT4).unwrap();
    folder::export_item(&library.active_dir(), &game_save(9)).unwrap();
    let new_before = snapshot(&fixture.documents_root);
    let old_before = snapshot(&fixture.data.join("library"));

    let outcome = fixture.run(&copy_file);
    assert!(matches!(outcome, Outcome::Conflict { .. }), "{outcome:?}");
    assert_eq!(snapshot(&fixture.documents_root), new_before, "the Documents library is untouched");
    assert_eq!(snapshot(&fixture.data.join("library")), old_before, "the old library is untouched");
    assert!(!fixture.data.join(format!("library.migrated-{RUN_STAMP}")).exists());
    assert_eq!(marker_status(&fixture.marker()).as_deref(), Some("conflict"));
}

#[test]
fn a_failed_copy_is_rolled_back_and_the_old_library_is_untouched() {
    let fixture = Fixture::new("copy-fails");
    fixture.old_library();
    let old_before = snapshot(&fixture.data.join("library"));
    let copies = Cell::new(0);
    let failing = |from: &Path, to: &Path| -> io::Result<u64> {
        copies.set(copies.get() + 1);
        if copies.get() == 3 {
            return Err(io::Error::other("disk full (test)"));
        }
        fs::copy(from, to)
    };

    let outcome = fixture.run(&failing);
    let Outcome::Failed(message) = &outcome else { panic!("{outcome:?}") };
    assert!(message.contains("disk full"), "{message}");
    assert_eq!(snapshot(&fixture.data.join("library")), old_before, "the old library is untouched");
    assert!(snapshot(&fixture.documents_root).is_empty(), "the partial copy was removed");
    assert!(!fixture.data.join(format!("library.migrated-{RUN_STAMP}")).exists());
    assert_eq!(marker_status(&fixture.marker()).as_deref(), Some("failed"));

    // The next start tries again and succeeds.
    assert!(matches!(fixture.run(&copy_file), Outcome::Migrated { .. }));
}

#[test]
fn a_copy_that_differs_from_its_source_is_refused() {
    let fixture = Fixture::new("copy-differs");
    fixture.old_library();
    let old_before = snapshot(&fixture.data.join("library"));
    let corrupting = |from: &Path, to: &Path| -> io::Result<u64> {
        let mut bytes = fs::read(from)?;
        if let Some(first) = bytes.first_mut() {
            *first ^= 0xFF;
        }
        fs::write(to, &bytes)?;
        Ok(bytes.len() as u64)
    };
    let outcome = fixture.run(&corrupting);
    let Outcome::Failed(message) = &outcome else { panic!("{outcome:?}") };
    assert!(message.contains("sha256"), "{message}");
    assert_eq!(snapshot(&fixture.data.join("library")), old_before);
    assert!(snapshot(&fixture.documents_root).is_empty());
}

#[test]
fn a_card_harvested_into_the_documents_library_builds_the_next_card() {
    let fixture = Fixture::new("harvest-prepare");
    fixture.old_library();
    assert!(matches!(fixture.run(&copy_file), Outcome::Migrated { .. }));
    let library = Library::open_paths(&fixture.library_paths(), GT4).unwrap();

    // Prepare: the card for Play holds the moved active save and the replay.
    let runtime_card = fixture.data.join("runtime").join("card.ps2");
    fs::create_dir_all(runtime_card.parent().unwrap()).unwrap();
    let image = library.build_card(&library.replays().unwrap()).unwrap();
    library.write_card_image(&runtime_card, &image).unwrap();
    let names: Vec<String> = Card::from_image(&fs::read(&runtime_card).unwrap()).unwrap().root().unwrap().iter().map(Node::name).collect();
    assert_eq!(names, vec![GT4_GAME_SAVE.to_string(), REPLAY.to_string()]);

    // Harvest: the game saved new progress; it lands in Documents, the old copy in Saves\.backup.
    let played = Card::build(&[game_save(5), replay()]).unwrap().to_image();
    library.write_card_image(&runtime_card, &played).unwrap();
    let report = library.harvest(&fs::read(&runtime_card).unwrap()).unwrap();
    assert!(report.replaced_active);
    let active = folder::import_item(&fixture.documents_root.join("Saves").join("Active").join(GT4_GAME_SAVE)).unwrap();
    assert!(gt4saves::card::same_contents(std::slice::from_ref(&active), std::slice::from_ref(&game_save(5))));
    // The runtime card's previous image went to the AppData backups, never to Documents.
    assert!(fs::read_dir(fixture.data.join("library").join("backups")).unwrap().count() >= 2);
    assert!(!fixture.documents_root.join("backups").exists());
}
