//! Gran Turismo 4 Spec II 1.10: identification and setup (TASK-101, `docs/MULTI-GAME-LAUNCHER-PLAN.md`).
//!
//! Identification is by the **executable**, not by the whole image: SCUS-97436 plus the pinned SHA-256 of the disc's
//! `SCUS_974.36` (the program our derived ELF and plugin map depend on). The known full images are named
//! (`spec2-variants.json`); an image with the pinned executable and an unknown hash is accepted as a "Spec II 1.10 variant"
//! and logged (translations such as the Brazilian Portuguese one only replace language data). Any other SCUS-97436
//! program, including the unmodified GT4 Online Public Beta, is refused: it would need its own patch map.
//!
//! Setup checks the image where it is (decision 0075; see `setup`) and writes the derived ELF (`spec2_elf::derive`) to
//! `games\spec2\prepared`. There is no CORE step: Spec II is one plain ELF.

use std::fs;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::cnf;
use crate::elf;
use crate::iso::{Iso, IsoError};
use crate::pins::Identity;
use crate::setup::{
    self, DiscReceipt, DiscSummary, ImageReceipt, Receipt, ReceiptIdentity, SetupError, Stage, Unsupported, MAX_SYSTEM_CNF_BYTES,
    RECEIPT_FILE, RECEIPT_SCHEMA,
};
use crate::spec2_elf;
use crate::spec2_pins::Spec2Pins;

/// The prepared ELF (the donor derives the boot name from the file name).
pub const ELF_FILE: &str = "SCUS_974.36";
/// The version text of this game in the UI and the receipt.
pub const VERSION: &str = "1.10";
/// What an image with the pinned executable and an unknown hash is called.
pub const VARIANT_NAME: &str = "Spec II 1.10 variant";
const BOOT_FILE: &str = "SCUS_974.36;1";
const MAX_EXECUTABLE_BYTES: u64 = 16 * 1024 * 1024;

/// Opens the image and reads `SYSTEM.CNF`, with the same refusals as the Gran Turismo 4 check (file, ISO, PS2 disc).
pub(crate) fn open_and_read_system(disc: &Path) -> Result<(Iso, cnf::SystemConfig), SetupError> {
    let extension = disc.extension().and_then(|value| value.to_str()).map(str::to_string);
    match extension.as_deref() {
        Some(value) if value.eq_ignore_ascii_case("iso") => {}
        other => return Err(SetupError::NotPlainIso { extension: other.map(str::to_string) }),
    }
    if !disc.is_file() || fs::File::open(disc).is_err() {
        return Err(SetupError::NotFound);
    }
    let mut iso = Iso::open(disc).map_err(|error| match error {
        IsoError::Io(error) => SetupError::Io(error.to_string()),
        other => SetupError::NotIso(other.to_string()),
    })?;
    let system_entry = iso.find("SYSTEM.CNF;1").map_err(|_| SetupError::NotPlayStation2)?;
    let system_bytes = iso.read(&system_entry, MAX_SYSTEM_CNF_BYTES).map_err(|_| SetupError::IncompleteOrModified)?;
    let config = cnf::parse(&system_bytes).map_err(SetupError::NotIso)?;
    Ok((iso, config))
}

/// The fast identity check of a Spec II disc (no full hash): the boot file is read and compared with the pin.
pub fn precheck(disc: &Path, pins: &Spec2Pins) -> Result<DiscSummary, SetupError> {
    let (iso, config) = open_and_read_system(disc)?;
    precheck_opened(iso, config, pins)
}

pub(crate) fn precheck_opened(mut iso: Iso, config: cnf::SystemConfig, pins: &Spec2Pins) -> Result<DiscSummary, SetupError> {
    let volume_identifier = iso.volume_identifier().to_string();
    let length_bytes = iso.length_bytes();
    if config.serial != pins.serial {
        return Err(SetupError::Unsupported(Unsupported::OtherGame { serial: config.serial }));
    }
    if length_bytes < iso.volume_space_bytes() {
        return Err(SetupError::IncompleteOrModified);
    }
    let program = if config.boot_path == pins.boot_path {
        iso.find(BOOT_FILE).ok().and_then(|entry| iso.read(&entry, MAX_EXECUTABLE_BYTES).ok())
    } else {
        None
    };
    let Some(program) = program else {
        return Err(SetupError::Unsupported(Unsupported::Spec2Executable { beta: false }));
    };
    let sha256 = elf::sha256_hex(&program);
    if sha256 != pins.executable.sha256 || program.len() as u64 != pins.executable.size_bytes {
        return Err(SetupError::Unsupported(Unsupported::Spec2Executable { beta: sha256 == pins.beta_boot_sha256 }));
    }
    Ok(DiscSummary {
        serial: config.serial,
        version: VERSION.to_string(),
        video_mode: config.video_mode,
        volume_identifier,
        size_bytes: length_bytes,
        qualified_version: false,
    })
}

/// Inputs of one Spec II setup run (the folders are the `games\spec2` ones).
pub struct Spec2Request<'a> {
    pub disc: &'a Path,
    pub prepared: &'a Path,
    pub staging: &'a Path,
    pub install_dir: &'a Path,
    pub pins: &'a Spec2Pins,
    pub launcher_version: &'a str,
    pub cancel: &'a AtomicBool,
}

/// The name an image of Spec II is shown and logged under: a known name, or the variant text.
pub fn image_name(pins: &Spec2Pins, identity: &Identity) -> String {
    pins.known_image(identity).map(str::to_string).unwrap_or_else(|| VARIANT_NAME.to_string())
}

/// Runs the whole Spec II setup. On success the prepared folder holds the receipt and the derived ELF, and the image is where
/// it was. Cancel or failure removes the staging folder.
pub fn run(request: &Spec2Request<'_>, progress: &mut dyn FnMut(Stage, f32)) -> Result<Receipt, SetupError> {
    let summary = precheck(request.disc, request.pins)?;
    progress(Stage::Copying, 0.0);
    let result = setup::hash_image_as(request.disc, Stage::Copying, request.cancel, progress)
        .and_then(|identity| finish(request, &summary, &identity, progress));
    if result.is_err() {
        let _ = fs::remove_dir_all(request.staging);
    }
    result
}

fn finish(
    request: &Spec2Request<'_>,
    summary: &DiscSummary,
    identity: &Identity,
    progress: &mut dyn FnMut(Stage, f32),
) -> Result<Receipt, SetupError> {
    let pins = request.pins;
    progress(Stage::Checking, 0.0);
    if identity.size_bytes != summary.size_bytes {
        return Err(SetupError::IncompleteOrModified);
    }
    let name = image_name(pins, identity);
    progress(Stage::Checking, 1.0);
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }

    progress(Stage::Preparing, 0.0);
    let (original, derived) = extract_and_derive(request.disc, pins)?;
    progress(Stage::Preparing, 0.5);
    if request.staging.exists() {
        fs::remove_dir_all(request.staging)?;
    }
    fs::create_dir_all(request.staging)?;
    setup::write_synced(&request.staging.join(ELF_FILE), &derived)?;

    let metadata = fs::metadata(request.disc)?;
    let receipt = Receipt {
        schema_version: RECEIPT_SCHEMA,
        launcher_version: request.launcher_version.to_string(),
        payload: "bundled".to_string(),
        game: "spec2".to_string(),
        disc: DiscReceipt {
            path: request.disc.display().to_string(),
            size_bytes: identity.size_bytes,
            sha256: identity.sha256.clone(),
            serial: summary.serial.clone(),
            version: summary.version.clone(),
            volume_identifier: summary.volume_identifier.clone(),
        },
        image: Some(ImageReceipt {
            file: setup::image_file_name(request.disc),
            linked_path: Some(setup::linked_path(request.disc, request.install_dir)?),
            size_bytes: metadata.len(),
            modified_unix_nanos: setup::modified_nanos(&metadata),
            sha256: identity.sha256.clone(),
        }),
        qualified_dump: pins.known_image(identity).is_some(),
        image_name: name,
        core: ReceiptIdentity::default(),
        original: Some(ReceiptIdentity { size_bytes: original.len() as u64, sha256: elf::sha256_hex(&original) }),
        elf: ReceiptIdentity { size_bytes: derived.len() as u64, sha256: elf::sha256_hex(&derived) },
        prepared_unix_seconds: setup::unix_seconds(),
    };
    let receipt_bytes = serde_json::to_vec_pretty(&receipt).map_err(|error| SetupError::Internal(error.to_string()))?;
    setup::write_synced(&request.staging.join(RECEIPT_FILE), &receipt_bytes)?;
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }
    setup::swap_into_place(request.staging, request.prepared)?;
    progress(Stage::Preparing, 1.0);
    Ok(receipt)
}

/// Reads `SCUS_974.36` from the image, checks it against the pin, derives the ELF and checks that too.
fn extract_and_derive(image: &Path, pins: &Spec2Pins) -> Result<(Vec<u8>, Vec<u8>), SetupError> {
    let mut iso = Iso::open(image).map_err(|error| SetupError::Io(error.to_string()))?;
    let entry = iso.find(BOOT_FILE).map_err(|error| SetupError::Internal(error.to_string()))?;
    let original = iso.read(&entry, MAX_EXECUTABLE_BYTES).map_err(|error| SetupError::Internal(error.to_string()))?;
    let found = Identity { size_bytes: original.len() as u64, sha256: elf::sha256_hex(&original) };
    if found != pins.executable {
        return Err(SetupError::Internal("SCUS_974.36 differs from the pinned identity".into()));
    }
    let derived = spec2_elf::derive(&original, &pins.patches).map_err(SetupError::Internal)?;
    let derived_identity = Identity { size_bytes: derived.len() as u64, sha256: elf::sha256_hex(&derived) };
    if derived_identity != pins.derived {
        return Err(SetupError::Internal("the derived ELF differs from the pinned identity".into()));
    }
    Ok((original, derived))
}

/// Fast check: the prepared ELF exists with the pinned size.
pub fn verify_prepared(prepared: &Path, pins: &Spec2Pins) -> Result<(), String> {
    let size = fs::metadata(prepared.join(ELF_FILE)).map_err(|_| format!("{ELF_FILE} is missing"))?.len();
    if size != pins.derived.size_bytes {
        return Err(format!("{ELF_FILE} has size {size}, expected {}", pins.derived.size_bytes));
    }
    Ok(())
}

/// The cheap check of every Spec II launch (receipt, prepared size, image size and time; no hash).
pub fn check_installed(prepared: &Path, disc_folder: &Path, install_dir: &Path, pins: &Spec2Pins) -> setup::Installed {
    setup::check_installed_with(prepared, disc_folder, install_dir, &|folder| verify_prepared(folder, pins))
}
