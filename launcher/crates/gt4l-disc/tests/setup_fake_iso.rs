//! Setup against small synthetic ISO images: good disc, the disc rule (game code decides), wrong serial and
//! region, Spec II, damaged dump, truncated copy, wrong file type, cancellation, low space, a hash mismatch after
//! the copy, the staging/rename rules and the cheap check of later launches.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use gt4l_disc::setup::{self, Installed, SetupError, SetupRequest, Stage, Unsupported, UNSUPPORTED_MESSAGE};
use gt4l_disc::testing::{build_iso, pins_for, synthetic_game, system_cnf, SyntheticGame};
use gt4l_disc::Pins;

const BODY: &[u8] = b"synthetic guest code, not game data, 0123456789abcdef";
const SPEC_TRAILER: usize = 3000;

struct Fixture {
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Fixture {
        let root = std::env::temp_dir().join(format!("gt4l-setup-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        Fixture { root }
    }

    fn path(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    fn disc_folder(&self) -> PathBuf {
        self.path("disc")
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn game() -> SyntheticGame {
    synthetic_game(0x0010_0000, 0x0010_0008, BODY)
}

/// A disc image for the synthetic game with the given `SYSTEM.CNF` boot name, version and volume id.
fn image_with_trailer(game: &SyntheticGame, boot_name: &str, version: &str, volume: &str, trailer: u8) -> Vec<u8> {
    build_iso(
        volume,
        &[
            ("SYSTEM.CNF;1", system_cnf(boot_name, version, "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![trailer; SPEC_TRAILER]),
        ],
    )
}

fn image_for(game: &SyntheticGame, boot_name: &str, version: &str, volume: &str) -> Vec<u8> {
    image_with_trailer(game, boot_name, version, volume, 0xA5)
}

/// Pins that describe the good image, plus the image itself.
fn good_pins_and_image(volume: &str) -> (Pins, Vec<u8>) {
    let game = game();
    let image = image_for(&game, "SCUS_973.28", "2.00", volume);
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    (pins, image)
}

fn run_setup(fixture: &Fixture, disc: &Path, pins: &Pins, cancel: &AtomicBool) -> Result<setup::Receipt, SetupError> {
    let prepared = fixture.path("prepared");
    let staging = fixture.path("prepared.tmp");
    let install_dir = fixture.path("launcher");
    let request = SetupRequest {
        disc,
        prepared: &prepared,
        staging: &staging,
        install_dir: &install_dir,
        pins,
        launcher_version: "test",
        cancel,
    };
    let mut seen: Vec<(Stage, f32)> = Vec::new();
    let result = setup::run(&request, &mut |stage, fraction| seen.push((stage, fraction)));
    if result.is_ok() {
        assert!(seen.iter().any(|(stage, fraction)| *stage == Stage::Copying && *fraction == 1.0));
        assert!(seen.iter().any(|(stage, _)| *stage == Stage::Checking));
        assert_eq!(seen.last().copied(), Some((Stage::Preparing, 1.0)));
    }
    result
}

/// Nothing half-written: no staging folder, no prepared folder, no copy of the image.
fn assert_nothing_written(fixture: &Fixture) {
    assert!(!fixture.path("prepared.tmp").exists(), "staging folder left behind");
    assert!(!fixture.path("prepared").exists(), "prepared folder written");
    assert!(!fixture.disc_folder().exists(), "setup never writes a copy of the image");
}

#[test]
fn good_disc_is_linked_where_it_is_and_nothing_is_copied() {
    let fixture = Fixture::new("good");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let modified = fs::metadata(&disc).unwrap().modified().unwrap();
    let cancel = AtomicBool::new(false);
    let receipt = run_setup(&fixture, &disc, &pins, &cancel).expect("setup succeeds");
    assert_eq!(receipt.schema_version, 3);
    assert_eq!(receipt.disc.serial, "SCUS-97328");
    assert_eq!(receipt.disc.volume_identifier, "GT4 USA");
    assert!(receipt.qualified_dump);
    assert_eq!(receipt.image.as_ref().unwrap().linked_path.as_deref(), Some(disc.to_str().unwrap()));
    let prepared = fixture.path("prepared");
    assert_eq!(fs::read(prepared.join("SCUS_973.28")).unwrap(), game().elf);
    assert!(prepared.join("CORE.GT4").exists());
    assert!(!fixture.path("prepared.tmp").exists(), "staging folder is renamed away");
    assert!(!fixture.disc_folder().exists(), "no copy of the image");
    assert_eq!(fs::read(&disc).unwrap(), image, "the player's image is only read");
    assert_eq!(fs::metadata(&disc).unwrap().modified().unwrap(), modified);
    let receipt_text = fs::read_to_string(prepared.join("prepare.json")).unwrap();
    assert!(receipt_text.contains("\"payload\": \"bundled\""));
    assert!(receipt_text.contains("\"modified_unix_nanos\""));
    match setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins) {
        Installed::Ready { image, serial, version } => {
            assert_eq!(image, disc, "the game reads the image where it is");
            assert_eq!(serial, "SCUS-97328");
            assert_eq!(version, "2.00");
        }
        other => panic!("expected Ready, got {other:?}"),
    }
}

#[test]
fn an_image_that_moved_or_changed_asks_to_be_chosen_again_and_linking_it_is_all_it_takes() {
    let fixture = Fixture::new("moved");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    let prepared = fixture.path("prepared");

    let moved = fixture.path("elsewhere.iso");
    fs::rename(&disc, &moved).unwrap();
    assert_eq!(setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins), Installed::LinkBroken { image: disc.clone() });

    // Another file at the same path is not the image that was checked.
    fs::write(&disc, &image[..image.len() - 2048]).unwrap();
    assert_eq!(setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins), Installed::LinkBroken { image: disc.clone() });
    fs::remove_file(&disc).unwrap();

    run_setup(&fixture, &moved, &pins, &cancel).expect("the image is linked again");
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Ready { image, .. } if image == moved
    ));
}

#[test]
fn an_image_inside_the_launcher_folder_is_saved_relative_and_moves_with_the_folder() {
    let fixture = Fixture::new("portable");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let install = fixture.path("launcher");
    fs::create_dir_all(install.join("ISOs")).unwrap();
    let disc = install.join("ISOs").join("game.iso");
    fs::write(&disc, &image).unwrap();
    let prepared = install.join("data").join("prepared");
    let staging = install.join("data").join("prepared.tmp");
    fs::create_dir_all(install.join("data")).unwrap();
    let cancel = AtomicBool::new(false);
    let request = SetupRequest { disc: &disc, prepared: &prepared, staging: &staging, install_dir: &install, pins: &pins, launcher_version: "test", cancel: &cancel };
    let receipt = setup::run(&request, &mut |_, _| {}).expect("setup succeeds");
    assert_eq!(receipt.image.unwrap().linked_path.as_deref(), Some(Path::new("ISOs").join("game.iso").to_str().unwrap()), "relative to the folder");

    // The whole folder goes to another place, or another drive letter.
    let carried = fixture.path("on-the-usb-stick");
    fs::rename(&install, &carried).unwrap();
    let prepared = carried.join("data").join("prepared");
    match setup::check_installed(&prepared, &carried.join("data").join("disc"), &carried, &pins) {
        Installed::Ready { image, .. } => assert_eq!(image, carried.join("ISOs").join("game.iso")),
        other => panic!("the folder moved with its image: {other:?}"),
    }
}

#[test]
fn an_image_outside_the_launcher_folder_stays_absolute_when_the_folder_moves() {
    let fixture = Fixture::new("outside");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    let receipt = run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    assert!(Path::new(receipt.image.unwrap().linked_path.as_deref().unwrap()).is_absolute());
    match setup::check_installed(&fixture.path("prepared"), &fixture.disc_folder(), &fixture.path("another-place"), &pins) {
        Installed::Ready { image, .. } => assert_eq!(image, disc),
        other => panic!("{other:?}"),
    }
}

#[test]
fn a_file_system_that_rounds_the_time_is_not_a_changed_image() {
    let fixture = Fixture::new("rounded");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    let prepared = fixture.path("prepared");
    let recorded = fs::metadata(&disc).unwrap().modified().unwrap();
    let file = fs::OpenOptions::new().write(true).open(&disc).unwrap();
    file.set_modified(recorded + std::time::Duration::from_millis(1500)).unwrap();
    assert!(matches!(setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins), Installed::Ready { .. }), "a rounded time");
    file.set_modified(recorded + std::time::Duration::from_secs(10)).unwrap();
    assert!(matches!(setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins), Installed::LinkBroken { .. }), "a real change");
}

/// Turns the prepared folder into what a launcher from before decision 0075 left: the image copied, no link in the receipt.
fn make_it_an_old_install(fixture: &Fixture, disc: &Path, name: &str) {
    fs::create_dir_all(fixture.disc_folder()).unwrap();
    fs::rename(disc, fixture.disc_folder().join(name)).unwrap();
    let receipt_path = fixture.path("prepared").join("prepare.json");
    let mut receipt: serde_json::Value = serde_json::from_slice(&fs::read(&receipt_path).unwrap()).unwrap();
    receipt["schema_version"] = 2.into();
    receipt["image"]["file"] = name.into();
    receipt["image"].as_object_mut().unwrap().remove("linked_path");
    fs::write(&receipt_path, serde_json::to_vec(&receipt).unwrap()).unwrap();
}

#[test]
fn an_install_from_before_the_link_keeps_working_with_its_copy() {
    let fixture = Fixture::new("old-install");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    make_it_an_old_install(&fixture, &disc, "SCUS-97328.iso");
    match setup::check_installed(&fixture.path("prepared"), &fixture.disc_folder(), &fixture.path("launcher"), &pins) {
        Installed::Ready { image, .. } => assert_eq!(image, fixture.disc_folder().join("SCUS-97328.iso")),
        other => panic!("an old install must stay installed: {other:?}"),
    }
}

#[test]
fn a_second_run_replaces_the_prepared_folder_and_the_image() {
    let fixture = Fixture::new("rerun");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    fs::write(fixture.path("prepared").join("stale.txt"), b"old").unwrap();
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    assert!(!fixture.path("prepared").join("stale.txt").exists());
    let leftovers: Vec<_> = fs::read_dir(&fixture.root)
        .unwrap()
        .filter_map(|entry| entry.ok().map(|entry| entry.file_name().to_string_lossy().into_owned()))
        .filter(|name| name.starts_with("prepared.old-"))
        .collect();
    assert!(leftovers.is_empty(), "old folder removed: {leftovers:?}");
    assert!(matches!(
        setup::check_installed(&fixture.path("prepared"), &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Ready { .. }
    ));
}

#[test]
fn other_games_regions_and_spec_ii_get_the_not_supported_yet_message() {
    let fixture = Fixture::new("identity");
    let cancel = AtomicBool::new(false);
    let game = game();
    let disc = fixture.path("other.iso");

    // Another US serial: the disc's SYSTEM.CNF names SCUS_971.00.
    let image = image_for(&game, "SCUS_971.00", "2.00", "GT4 USA");
    fs::write(&disc, &image).unwrap();
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let error = run_setup(&fixture, &disc, &pins, &cancel).unwrap_err();
    assert!(matches!(&error, SetupError::Unsupported(Unsupported::OtherGame { serial }) if serial == "SCUS-97100"));
    assert_eq!(error.user_message(), format!("{UNSUPPORTED_MESSAGE} This disc is SCUS-97100."));
    assert!(error.is_disc_refusal());

    // A PAL boot path.
    let image = image_for(&game, "SCES_500.01", "2.00", "GT4 EUR");
    fs::write(&disc, &image).unwrap();
    let error = run_setup(&fixture, &disc, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::Unsupported(Unsupported::OtherRegion { .. })), "{error:?}");
    assert!(error.user_message().starts_with(UNSUPPORTED_MESSAGE));

    // A Spec II serial given to the Gran Turismo 4 setup: another game of the launcher, which `identify` routes to the
    // Spec II setup (tests/identify.rs). The v2.00 setup itself refuses it by serial.
    let spec = fixture.path("spec.iso");
    let image = image_for(&game, "SCUS_974.36", "1.00", "GRANTURISMO4");
    fs::write(&spec, &image).unwrap();
    let error = run_setup(&fixture, &spec, &pins, &cancel).unwrap_err();
    assert!(matches!(&error, SetupError::Unsupported(Unsupported::OtherGame { serial }) if serial == "SCUS-97436"), "{error:?}");
    assert_eq!(error.user_message(), format!("{UNSUPPORTED_MESSAGE} This disc is SCUS-97436."));
    assert_nothing_written(&fixture);
}

#[test]
fn another_revision_with_the_same_game_code_is_accepted() {
    let fixture = Fixture::new("revision-same-code");
    let cancel = AtomicBool::new(false);
    let game = game();
    let (pins, _) = good_pins_and_image("GT4 USA");
    // VER 1.01 and different data elsewhere on the disc, but CORE.GT4 byte-identical.
    let image = image_with_trailer(&game, "SCUS_973.28", "1.01", "GT4 USA", 0x5A);
    let disc = fixture.path("v101.iso");
    fs::write(&disc, &image).unwrap();
    let receipt = run_setup(&fixture, &disc, &pins, &cancel).expect("same game code is accepted");
    assert_eq!(receipt.disc.version, "1.01");
    assert!(!receipt.qualified_dump, "not the qualified v2.00 dump");
    assert_eq!(receipt.elf.sha256, pins.elf.sha256);
}

#[test]
fn another_revision_with_different_game_code_is_refused() {
    let fixture = Fixture::new("revision-other-code");
    let cancel = AtomicBool::new(false);
    let (pins, _) = good_pins_and_image("GT4 USA");
    let other_game = synthetic_game(0x0010_0000, 0x0010_0008, b"other synthetic code, not game data, 0123456789abcd");
    let image = image_for(&other_game, "SCUS_973.28", "1.00", "GT4 USA");
    let disc = fixture.path("v100.iso");
    fs::write(&disc, &image).unwrap();
    let error = run_setup(&fixture, &disc, &pins, &cancel).unwrap_err();
    match &error {
        SetupError::Unsupported(Unsupported::OtherRevision { version }) => assert_eq!(version, "1.00"),
        other => panic!("expected OtherRevision, got {other:?}"),
    }
    assert!(error.user_message().starts_with(UNSUPPORTED_MESSAGE));
    assert_nothing_written(&fixture);
}

#[test]
fn truncated_copy_is_incomplete_and_a_changed_core_is_a_different_dump() {
    let fixture = Fixture::new("dump");
    let cancel = AtomicBool::new(false);
    let (pins, image) = good_pins_and_image("GT4 USA");

    let truncated = fixture.path("truncated.iso");
    fs::write(&truncated, &image[..image.len() - 2048]).unwrap();
    let error = run_setup(&fixture, &truncated, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::IncompleteOrModified), "{error:?}");
    assert_eq!(error.user_message(), "This copy is incomplete or modified. Please re-dump your disc.");

    // Flip one byte inside the CORE file's data (same size, different hash): refused before the full hash.
    let core_offset = find_bytes(&image, &game().core).expect("CORE data present in image");
    let mut changed = image.clone();
    changed[core_offset + 100] ^= 0xFF;
    let damaged = fixture.path("damaged.iso");
    fs::write(&damaged, &changed).unwrap();
    let error = run_setup(&fixture, &damaged, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DiffersFromKnownGood), "{error:?}");
    assert_nothing_written(&fixture);
}

#[test]
fn a_hash_mismatch_leaves_nothing_behind() {
    let fixture = Fixture::new("mismatch");
    let cancel = AtomicBool::new(false);
    let (pins, image) = good_pins_and_image("GT4 USA");
    // Same size, same CORE, one changed byte in another file: only the full hash can see it.
    let trailer_offset = image.len() - 10;
    let mut changed = image.clone();
    changed[trailer_offset] ^= 0xFF;
    let disc = fixture.path("patched.iso");
    fs::write(&disc, &changed).unwrap();
    let error = run_setup(&fixture, &disc, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::DiffersFromKnownGood), "{error:?}");
    assert_nothing_written(&fixture);
}

fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack.windows(needle.len()).position(|window| window == needle)
}

#[test]
fn wrong_file_types_are_refused_before_hashing() {
    let fixture = Fixture::new("types");
    let cancel = AtomicBool::new(false);
    let (pins, image) = good_pins_and_image("GT4 USA");

    let bin = fixture.path("game.bin");
    fs::write(&bin, &image).unwrap();
    let error = run_setup(&fixture, &bin, &pins, &cancel).unwrap_err();
    assert_eq!(
        error.user_message(),
        "Please use a plain .iso file. A .bin file is not supported. Convert it to a plain .iso first."
    );

    let chd = fixture.path("game.chd");
    fs::write(&chd, &image).unwrap();
    let error = run_setup(&fixture, &chd, &pins, &cancel).unwrap_err();
    assert_eq!(error.user_message(), "This image is compressed (.chd). Please convert it to a plain .iso.");

    let zeros = fixture.path("zeros.iso");
    fs::write(&zeros, vec![0u8; 20 * 2048]).unwrap();
    let error = run_setup(&fixture, &zeros, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::NotIso(_)), "{error:?}");

    let missing = fixture.path("moved-away.iso");
    let error = run_setup(&fixture, &missing, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::NotFound), "{error:?}");
    assert!(error.is_disc_refusal());
    assert_nothing_written(&fixture);
}

#[test]
fn cancel_leaves_nothing_half_written() {
    let fixture = Fixture::new("cancel");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(true);
    let error = run_setup(&fixture, &disc, &pins, &cancel).unwrap_err();
    assert!(matches!(error, SetupError::Cancelled));
    assert_nothing_written(&fixture);
}

#[test]
fn later_launches_see_a_missing_or_changed_image_and_old_receipts() {
    let fixture = Fixture::new("later");
    let (pins, image) = good_pins_and_image("GT4 USA");
    let prepared = fixture.path("prepared");
    assert_eq!(setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins), Installed::NotSetUp);

    let disc = fixture.path("game.iso");
    fs::write(&disc, &image).unwrap();
    let cancel = AtomicBool::new(false);
    run_setup(&fixture, &disc, &pins, &cancel).unwrap();
    make_it_an_old_install(&fixture, &disc, "SCUS-97328.iso");
    let imported = fixture.disc_folder().join("SCUS-97328.iso");

    // An install from before the link. The player cleaned the data folder: the copy is gone.
    let kept = fs::read(&imported).unwrap();
    fs::remove_file(&imported).unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Missing { reason } if reason.contains("missing")
    ));

    // A different file of another size in its place.
    fs::write(&imported, &kept[..kept.len() - 2048]).unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Missing { reason } if reason.contains("changed")
    ));

    // A schema 1 receipt (before the copy existed) asks for the disc once.
    let receipt_path = prepared.join("prepare.json");
    let mut receipt: serde_json::Value = serde_json::from_slice(&fs::read(&receipt_path).unwrap()).unwrap();
    receipt["schema_version"] = 1.into();
    receipt.as_object_mut().unwrap().remove("image");
    fs::write(&receipt_path, serde_json::to_vec(&receipt).unwrap()).unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Missing { .. }
    ));

    // A damaged receipt.
    fs::write(&receipt_path, b"{ not json").unwrap();
    assert!(matches!(
        setup::check_installed(&prepared, &fixture.disc_folder(), &fixture.path("launcher"), &pins),
        Installed::Missing { reason } if reason.contains("damaged")
    ));
}
