//! Tourist Trophy (USA, SCUS-97502): identification and setup (experimental third game; `docs/TT-STUDY.md`).
//!
//! Identification is by the **code**, like Spec II: the serial, `CORE.TT` (the game, encrypted ELF records) and the loader,
//! `SYSTEM.CNF` and IOP code pinned in `docs/inputs/tt-iso.json`. The pinned full image is named; another image with the same
//! code is accepted as a "Tourist Trophy (USA) variant" and logged. Any other SCUS-97502 code is refused: it would need its
//! own translation and hook table.
//!
//! Setup checks the image where it is (decision 0075; see `setup`) and writes the ELF derived from `CORE.TT`
//! (`tt_elf::derive`) to `games\tt\prepared`.

use std::fs;
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::cnf;
use crate::elf;
use crate::iso::Iso;
use crate::pins::Identity;
use crate::setup::{
    self, DiscReceipt, DiscSummary, ImageReceipt, Receipt, ReceiptIdentity, SetupError, Stage, Unsupported, RECEIPT_FILE,
    RECEIPT_SCHEMA,
};
use crate::spec2;
use crate::tt_elf;
use crate::tt_pins::TtPins;

/// The version text of this game in the UI and the receipt.
pub const VERSION: &str = "1.00";
/// What an image with the pinned code and an unknown hash is called.
pub const VARIANT_NAME: &str = "Tourist Trophy (USA) variant";
const CORE_PATH: &str = "/CORE.TT;1";
const MAX_CORE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_CODE_FILE_BYTES: u64 = 4 * 1024 * 1024;

/// The fast identity check of a Tourist Trophy disc (no full hash): the code files are read and compared with the pins.
pub fn precheck(disc: &Path, pins: &TtPins) -> Result<DiscSummary, SetupError> {
    let (iso, config) = spec2::open_and_read_system(disc)?;
    precheck_opened(iso, config, pins)
}

pub(crate) fn precheck_opened(mut iso: Iso, config: cnf::SystemConfig, pins: &TtPins) -> Result<DiscSummary, SetupError> {
    let volume_identifier = iso.volume_identifier().to_string();
    let length_bytes = iso.length_bytes();
    if config.serial != pins.serial {
        return Err(SetupError::Unsupported(Unsupported::OtherGame { serial: config.serial }));
    }
    if length_bytes < iso.volume_space_bytes() {
        return Err(SetupError::IncompleteOrModified);
    }
    if config.boot_path != pins.boot_path {
        return Err(SetupError::Unsupported(Unsupported::TtCode));
    }
    let matches = |iso: &mut Iso, path: &str, identity: &Identity, limit: u64| {
        iso.find_path(path)
            .ok()
            .filter(|entry| entry.size_bytes == identity.size_bytes)
            .and_then(|entry| iso.read(&entry, limit).ok())
            .is_some_and(|bytes| elf::sha256_hex(&bytes) == identity.sha256)
    };
    if !matches(&mut iso, CORE_PATH, &pins.core, MAX_CORE_BYTES) {
        return Err(SetupError::Unsupported(Unsupported::TtCode));
    }
    for file in &pins.code_files {
        if !matches(&mut iso, &file.path, &file.identity, MAX_CODE_FILE_BYTES) {
            return Err(SetupError::Unsupported(Unsupported::TtCode));
        }
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

/// Inputs of one Tourist Trophy setup run (the folders are the `games\tt` ones).
pub struct TtRequest<'a> {
    pub disc: &'a Path,
    pub prepared: &'a Path,
    pub staging: &'a Path,
    pub install_dir: &'a Path,
    pub pins: &'a TtPins,
    pub launcher_version: &'a str,
    pub cancel: &'a AtomicBool,
}

/// The name an image of Tourist Trophy is shown and logged under: the pinned name, or the variant text.
pub fn image_name(pins: &TtPins, identity: &Identity) -> String {
    pins.known_image(identity).map(str::to_string).unwrap_or_else(|| VARIANT_NAME.to_string())
}

/// Runs the whole Tourist Trophy setup. On success the prepared folder holds the receipt and the derived ELF, and the
/// image is where it was. Cancel or failure removes the staging folder.
pub fn run(request: &TtRequest<'_>, progress: &mut dyn FnMut(Stage, f32)) -> Result<Receipt, SetupError> {
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
    request: &TtRequest<'_>,
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
    let (core, derived) = extract_and_derive(request.disc, pins)?;
    progress(Stage::Preparing, 0.5);
    if request.staging.exists() {
        fs::remove_dir_all(request.staging)?;
    }
    fs::create_dir_all(request.staging)?;
    setup::write_synced(&request.staging.join(&pins.derived_file), &derived)?;

    let metadata = fs::metadata(request.disc)?;
    let receipt = Receipt {
        schema_version: RECEIPT_SCHEMA,
        launcher_version: request.launcher_version.to_string(),
        payload: "bundled".to_string(),
        game: "tt".to_string(),
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
        core: ReceiptIdentity { size_bytes: core.len() as u64, sha256: elf::sha256_hex(&core) },
        original: None,
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

/// Reads `CORE.TT` from the image, checks it against the pin, derives the ELF and checks that too.
pub fn extract_and_derive(image: &Path, pins: &TtPins) -> Result<(Vec<u8>, Vec<u8>), SetupError> {
    let mut iso = Iso::open(image).map_err(|error| SetupError::Io(error.to_string()))?;
    let entry = iso.find_path(CORE_PATH).map_err(|error| SetupError::Internal(error.to_string()))?;
    let core = iso.read(&entry, MAX_CORE_BYTES).map_err(|error| SetupError::Internal(error.to_string()))?;
    if (Identity { size_bytes: core.len() as u64, sha256: elf::sha256_hex(&core) }) != pins.core {
        return Err(SetupError::Internal("CORE.TT differs from the pinned identity".into()));
    }
    let derived = tt_elf::derive(&core, pins.data_bss_bytes).map_err(SetupError::Internal)?;
    if (Identity { size_bytes: derived.len() as u64, sha256: elf::sha256_hex(&derived) }) != pins.derived {
        return Err(SetupError::Internal("the derived ELF differs from the pinned identity".into()));
    }
    Ok((core, derived))
}

/// Fast check: the prepared ELF exists with the pinned size.
pub fn verify_prepared(prepared: &Path, pins: &TtPins) -> Result<(), String> {
    let file = &pins.derived_file;
    let size = fs::metadata(prepared.join(file)).map_err(|_| format!("{file} is missing"))?.len();
    if size != pins.derived.size_bytes {
        return Err(format!("{file} has size {size}, expected {}", pins.derived.size_bytes));
    }
    Ok(())
}

/// The cheap check of every Tourist Trophy launch (receipt, prepared size, image size and time; no hash).
pub fn check_installed(prepared: &Path, disc_folder: &Path, install_dir: &Path, pins: &TtPins) -> setup::Installed {
    setup::check_installed_with(prepared, disc_folder, install_dir, &|folder| verify_prepared(folder, pins))
}
