//! Disc identification and the Spec II setup (TASK-101): which game a disc is, the variant rules, and the setup on small
//! synthetic images. The tests against the owner's real images run only when those files are present (they are
//! git-ignored game data and are only read).

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use gt4l_disc::elf::sha256_hex;
use gt4l_disc::identify::{identify, DiscGame};
use gt4l_disc::setup::{self, Installed, SetupError, Stage, Unsupported};
use gt4l_disc::spec2::{self, Spec2Request};
use gt4l_disc::spec2_elf;
use gt4l_disc::spec2_pins::{KnownImage, Spec2Patch};
use gt4l_disc::testing::{build_iso, pins_for, synthetic_game, system_cnf};
use gt4l_disc::{Identity, Pins, Spec2Pins};

const PLUGIN_VA: u32 = 0x68A480;

/// A small ELF32 with a text segment, a data segment that covers the plugin address and the plugin segment.
fn synthetic_spec2_executable(seed: u8) -> Vec<u8> {
    let headers: [[u32; 8]; 3] = [
        [1, 0x1000, 0x100000, 0x100000, 0x100, 0x100, 5, 0x1000],
        [1, 0x1100, 0x680000, 0x680000, 0x20000, 0x30000, 6, 0x1000],
        [1, 0x21100, PLUGIN_VA, PLUGIN_VA, 0x40, 0x40, 7, 0x10],
    ];
    let mut image = vec![seed; 0x21140];
    image[..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
    image[4] = 1;
    image[5] = 1;
    image[28..32].copy_from_slice(&52u32.to_le_bytes());
    image[42..44].copy_from_slice(&32u16.to_le_bytes());
    image[44..46].copy_from_slice(&3u16.to_le_bytes());
    for (index, header) in headers.iter().enumerate() {
        for (field, value) in header.iter().enumerate() {
            let at = 52 + index * 32 + field * 4;
            image[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
    }
    image[0x1000..0x1004].copy_from_slice(&0x1111_2222u32.to_le_bytes());
    image
}

fn spec2_image(executable: &[u8], boot_name: &str, volume: &str, filler: u8) -> Vec<u8> {
    build_iso(
        volume,
        &[
            ("SYSTEM.CNF;1", system_cnf(boot_name, "1.00", "NTSC")),
            ("SCUS_974.36;1", executable.to_vec()),
            ("EXTRA.DAT;1", vec![filler; 5000]),
        ],
    )
}

fn synthetic_pins(executable: &[u8], known: &[(&str, &[u8])]) -> Spec2Pins {
    let patches = vec![Spec2Patch { address: 0x100000, size: 4, old: 0x1111_2222, new: 0xAABB_CCDD }];
    let derived = spec2_elf::derive(executable, &patches).unwrap();
    Spec2Pins {
        serial: "SCUS-97436".into(),
        boot_path: "/SCUS_974.36;1".into(),
        system_version: "1.00".into(),
        executable: Identity { size_bytes: executable.len() as u64, sha256: sha256_hex(executable) },
        derived: Identity { size_bytes: derived.len() as u64, sha256: sha256_hex(&derived) },
        known_images: known
            .iter()
            .map(|(name, image)| KnownImage {
                name: name.to_string(),
                identity: Identity { size_bytes: image.len() as u64, sha256: sha256_hex(image) },
            })
            .collect(),
        beta_boot_sha256: sha256_hex(b"the beta loader"),
        patches,
    }
}

fn scratch(name: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!("gt4l-spec2-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn gt4_pins_and_image() -> (Pins, Vec<u8>) {
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    (pins_for(&image, &game, "SCUS-97328", "2.00"), image)
}

#[test]
fn a_disc_is_recognised_as_the_game_it_is() {
    let dir = scratch("identify");
    let executable = synthetic_spec2_executable(0);
    let image = spec2_image(&executable, "SCUS_974.36", "GRANTURISMO4", 1);
    let pins = synthetic_pins(&executable, &[("Spec II 1.10", &image)]);
    let (gt4_pins, gt4_image) = gt4_pins_and_image();

    let spec_path = dir.join("spec.iso");
    fs::write(&spec_path, &image).unwrap();
    let found = identify(&spec_path, &gt4_pins, &pins).unwrap();
    assert_eq!(found.game, DiscGame::Spec2);
    assert_eq!(found.summary.serial, "SCUS-97436");
    assert_eq!(found.summary.version, "1.10");

    let gt4_path = dir.join("gt4.iso");
    fs::write(&gt4_path, &gt4_image).unwrap();
    let found = identify(&gt4_path, &gt4_pins, &pins).unwrap();
    assert_eq!(found.game, DiscGame::Gt4);
    assert!(found.summary.qualified_version);

    // Another game is refused with what it is.
    let other = dir.join("other.iso");
    fs::write(&other, build_iso("OTHER", &[("SYSTEM.CNF;1", system_cnf("SCUS_971.00", "1.00", "NTSC"))])).unwrap();
    let error = identify(&other, &gt4_pins, &pins).unwrap_err();
    assert!(matches!(&error, SetupError::Unsupported(Unsupported::OtherGame { serial }) if serial == "SCUS-97100"), "{error:?}");
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn a_scus_97436_with_another_executable_is_refused_and_the_beta_is_named() {
    let dir = scratch("refuse");
    let executable = synthetic_spec2_executable(0);
    let pins = synthetic_pins(&executable, &[]);
    let (gt4_pins, _) = gt4_pins_and_image();

    // The unmodified Public Beta: the same serial and boot name, a different (loader) executable.
    let beta = dir.join("beta.iso");
    fs::write(&beta, spec2_image(b"the beta loader", "SCUS_974.36", "GRANTURISMO4", 1)).unwrap();
    let error = identify(&beta, &gt4_pins, &pins).unwrap_err();
    assert!(matches!(error, SetupError::Unsupported(Unsupported::Spec2Executable { beta: true })), "{error:?}");
    assert!(error.user_message().contains("unmodified GT4 Online Public Beta"), "{}", error.user_message());
    assert!(error.is_disc_refusal());

    // Any other SCUS-97436 program.
    let other = dir.join("other.iso");
    fs::write(&other, spec2_image(&synthetic_spec2_executable(9), "SCUS_974.36", "GRANTURISMO4", 1)).unwrap();
    let error = identify(&other, &gt4_pins, &pins).unwrap_err();
    assert!(matches!(error, SetupError::Unsupported(Unsupported::Spec2Executable { beta: false })), "{error:?}");
    assert!(error.user_message().contains("different game program"));
    let _ = fs::remove_dir_all(&dir);
}

struct SetupDirs {
    root: PathBuf,
}

impl SetupDirs {
    fn disc(&self) -> PathBuf {
        self.root.join("games/spec2/disc")
    }
    fn prepared(&self) -> PathBuf {
        self.root.join("games/spec2/prepared")
    }
    fn staging(&self) -> PathBuf {
        self.root.join("games/spec2/prepared.tmp")
    }
}

fn run_spec2(dirs: &SetupDirs, disc: &Path, pins: &Spec2Pins) -> Result<setup::Receipt, SetupError> {
    let cancel = AtomicBool::new(false);
    let prepared = dirs.prepared();
    let staging = dirs.staging();
    let request = Spec2Request {
        disc,
        prepared: &prepared,
        staging: &staging,
        pins,
        launcher_version: "test",
        cancel: &cancel,
    };
    let mut last = None;
    let result = spec2::run(&request, &mut |stage, fraction| last = Some((stage, fraction)));
    if result.is_ok() {
        assert_eq!(last, Some((Stage::Preparing, 1.0)));
    }
    result
}

#[test]
fn spec2_setup_copies_the_image_and_prepares_the_derived_elf_with_no_core_step() {
    let root = scratch("setup");
    let dirs = SetupDirs { root: root.clone() };
    let executable = synthetic_spec2_executable(0);
    let image = spec2_image(&executable, "SCUS_974.36", "GRANTURISMO4", 1);
    let pins = synthetic_pins(&executable, &[("Spec II 1.10", &image)]);
    let disc = root.join("picked.iso");
    fs::write(&disc, &image).unwrap();

    let receipt = run_spec2(&dirs, &disc, &pins).expect("a known Spec II image is set up");
    assert_eq!(receipt.game, "spec2");
    assert_eq!(receipt.image_name, "Spec II 1.10");
    assert!(receipt.qualified_dump);
    assert_eq!(receipt.core, Default::default(), "no CORE step");
    assert_eq!(receipt.elf.sha256, pins.derived.sha256);
    assert_eq!(receipt.original.as_ref().unwrap().sha256, pins.executable.sha256);
    assert!(dirs.prepared().join("SCUS_974.36").is_file());
    assert!(!dirs.prepared().join("CORE.GT4").exists());
    assert!(!dirs.disc().exists(), "the image is linked, not copied");
    assert_eq!(fs::read(&disc).unwrap(), image, "and only read");
    assert!(!dirs.staging().exists());
    match spec2::check_installed(&dirs.prepared(), &dirs.disc(), &pins) {
        Installed::Ready { image, serial, .. } => {
            assert_eq!(image, disc, "the game reads the image where it is");
            assert_eq!(serial, "SCUS-97436");
        }
        other => panic!("{other:?}"),
    }
    // The derived ELF is what spec2_elf::derive gives.
    let derived = fs::read(dirs.prepared().join("SCUS_974.36")).unwrap();
    assert_eq!(derived, spec2_elf::derive(&executable, &pins.patches).unwrap());
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn an_unknown_image_with_the_known_executable_is_accepted_as_a_variant() {
    let root = scratch("variant");
    let dirs = SetupDirs { root: root.clone() };
    let executable = synthetic_spec2_executable(0);
    let known = spec2_image(&executable, "SCUS_974.36", "GRANTURISMO4", 1);
    let pins = synthetic_pins(&executable, &[("Spec II 1.10", &known)]);
    // A translation: the same executable, other language data.
    let variant = spec2_image(&executable, "SCUS_974.36", "GRANTURISMO4", 2);
    assert_ne!(variant, known);
    let disc = root.join("variant.iso");
    fs::write(&disc, &variant).unwrap();
    let receipt = run_spec2(&dirs, &disc, &pins).expect("accepted");
    assert_eq!(receipt.image_name, spec2::VARIANT_NAME);
    assert!(!receipt.qualified_dump, "not a known image, so it is logged as a variant");
    assert!(dirs.prepared().join("SCUS_974.36").is_file());
    // A second known name is found by its own hash.
    let pins = synthetic_pins(&executable, &[("Spec II 1.10", &known), ("Spec II 1.10 (PT-BR)", &variant)]);
    assert_eq!(spec2::image_name(&pins, &Identity { size_bytes: variant.len() as u64, sha256: sha256_hex(&variant) }), "Spec II 1.10 (PT-BR)");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn a_refused_disc_and_a_cancelled_setup_leave_nothing_behind() {
    let root = scratch("clean");
    let dirs = SetupDirs { root: root.clone() };
    let executable = synthetic_spec2_executable(0);
    let pins = synthetic_pins(&executable, &[]);
    let beta = root.join("beta.iso");
    fs::write(&beta, spec2_image(b"the beta loader", "SCUS_974.36", "GRANTURISMO4", 1)).unwrap();
    assert!(run_spec2(&dirs, &beta, &pins).is_err());
    assert!(!dirs.prepared().exists() && !dirs.staging().exists() && !dirs.disc().join("SCUS-97436.iso").exists());

    let good = root.join("good.iso");
    fs::write(&good, spec2_image(&executable, "SCUS_974.36", "GRANTURISMO4", 1)).unwrap();
    let cancel = AtomicBool::new(true);
    let prepared = dirs.prepared();
    let staging = dirs.staging();
    let request = Spec2Request {
        disc: &good,
        prepared: &prepared,
        staging: &staging,
        pins: &pins,
        launcher_version: "test",
        cancel: &cancel,
    };
    assert!(matches!(spec2::run(&request, &mut |_, _| {}), Err(SetupError::Cancelled)));
    assert!(!dirs.prepared().exists() && !dirs.staging().exists());
    assert!(!dirs.disc().join("SCUS-97436.iso.tmp").exists());
    let _ = fs::remove_dir_all(&root);
}

// The owner's real images (read only) -----------------------------------------------------------------------------

fn owner_file(variable: &str, name: &str) -> Option<PathBuf> {
    let path = std::env::var_os(variable)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Antigravity\gt4-staticrecomp").join(name));
    path.is_file().then_some(path)
}

fn read_boot_file(iso: &Path) -> Vec<u8> {
    let mut image = gt4l_disc::iso::Iso::open(iso).unwrap();
    let entry = image.find("SCUS_974.36;1").unwrap();
    image.read(&entry, 16 * 1024 * 1024).unwrap()
}

#[test]
fn real_spec2_discs_are_identified_by_their_executable() {
    let pins = Spec2Pins::embedded().unwrap();
    let (gt4_pins, _) = gt4_pins_and_image();
    let real_gt4 = Pins::embedded().unwrap();
    let mut ran = false;
    for (variable, name) in [
        ("GT4_SPEC2_ISO", "Gran Turismo 4 Spec II v1.10.iso"),
        ("GT4_SPEC2_PTBR_ISO", "Gran Turismo 4 Spec II v1.10 (PTBR).iso"),
    ] {
        let Some(path) = owner_file(variable, name) else {
            eprintln!("skipped: {name} is not here");
            continue;
        };
        ran = true;
        let found = identify(&path, &real_gt4, &pins).unwrap_or_else(|error| panic!("{name}: {error}"));
        assert_eq!(found.game, DiscGame::Spec2, "{name}");
        assert_eq!(found.summary.serial, "SCUS-97436");
        if std::env::var_os("GT4_FULL_HASH").is_some() {
            let cancel = AtomicBool::new(false);
            let identity = setup::hash_image(&path, &cancel, &mut |_, _| {}).unwrap();
            let shown = spec2::image_name(&pins, &identity);
            assert!(shown.starts_with("Spec II 1.10"), "{name}: {shown}");
            assert_ne!(shown, spec2::VARIANT_NAME, "{name} is a known image");
        }
    }
    let _ = (gt4_pins, ran);
}

#[test]
fn the_unmodified_public_beta_is_refused() {
    let Some(path) = owner_file("GT4_BETA_ISO", "Gran Turismo 4 (USA) (Beta).iso") else {
        eprintln!("skipped: the Public Beta image is not here");
        return;
    };
    let pins = Spec2Pins::embedded().unwrap();
    let error = identify(&path, &Pins::embedded().unwrap(), &pins).unwrap_err();
    assert!(matches!(error, SetupError::Unsupported(Unsupported::Spec2Executable { beta: true })), "{error:?}");
}

#[test]
fn the_rust_derived_elf_is_byte_identical_to_the_python_script_output() {
    let Some(iso) = owner_file("GT4_SPEC2_ISO", "Gran Turismo 4 Spec II v1.10.iso") else {
        eprintln!("skipped: the Spec II image is not here");
        return;
    };
    let pins = Spec2Pins::embedded().unwrap();
    let original = read_boot_file(&iso);
    assert_eq!(sha256_hex(&original), pins.executable.sha256);
    let derived = spec2_elf::derive(&original, &pins.patches).unwrap();
    assert_eq!(derived.len() as u64, pins.derived.size_bytes);
    assert_eq!(sha256_hex(&derived), pins.derived.sha256);
    assert_eq!(pins.derived.sha256, "36f60b1a989b343e98ca55b1f07f051aa83a3fa1a4fcb4acb409adb148afc640");
    // Against the script's own file, when it is there.
    let script_output = PathBuf::from(r"C:\Antigravity\gt4-staticrecomp\private\slice272-spec2-elf\SCUS_974.36");
    if script_output.is_file() {
        assert_eq!(derived, fs::read(script_output).unwrap(), "byte for byte");
    } else {
        eprintln!("the script's output file is not here; the pinned hash was compared");
    }
    // An unexpected old word is refused exactly like the script.
    let mut wrong = pins.patches.clone();
    wrong[0].old ^= 1;
    let error = spec2_elf::apply_patches(&original, &wrong).unwrap_err();
    let (address, actual, expected) = (wrong[0].address, pins.patches[0].old, wrong[0].old);
    assert_eq!(error, format!("apply: word at {address:08x} is {actual:08x}, expected {expected:08x}"));
}
