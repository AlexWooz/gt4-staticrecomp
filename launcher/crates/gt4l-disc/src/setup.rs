//! "Preparing game files": verify the disc where it is, extract `CORE.GT4`, rebuild the ELF and write the prepared folder. The
//! image is never copied (owner, 2026-11-02, decision 0075): the receipt records where it is, and the game reads it there, so a
//! player can keep one image for this and for an emulator. An image that moved or changed asks to be chosen again.
//!
//! Installs from before that keep working: a receipt with no `linked_path` names the launcher's own copy in `<data>\disc\`.
//!
//! Steps (the UI shows them as rows):
//! 1. Identity (`precheck`, fast): plain ISO, `SYSTEM.CNF` serial and boot path, and `CORE.GT4` against its pin.
//!    The game code decides: any disc whose `CORE.GT4` is byte-identical to the pinned one is accepted, whatever
//!    its `VER` (docs/launcher/FLOW-AUDIT.md, "Disc rule"). A copy that claims the qualified version (`2.00`) must
//!    also have the pinned size and full SHA-256, so a damaged or patched v2.00 dump is still refused.
//! 2. Full hash of the image, in place (`Stage::Copying`, the row the copy used to have).
//! 3. Check (`Stage::Checking`): the qualified version must match the disc pin; any version must keep its size.
//! 4. Extract `CORE.GT4` from the image and verify it; rebuild the ELF and verify it (`Stage::Preparing`).
//! 5. Commit: the staging folder (CORE, ELF, `prepare.json`) replaces `prepared`. The receipt is the commit point: a later launch
//!    trusts it (size and modified time of the image).
//! Cancel or failure removes the staging folder, so nothing is left half-written. The player's image is only ever read.
//! The memory card needs no step: a missing card is a blank card created by the game on first save.

use std::fs::{self, File};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

use gt4l_core::language::Language;
use gt4l_core::messages;

use crate::cnf;
use crate::elf;
use crate::iso::{Iso, IsoError};
use crate::pins::{Identity, Pins};

/// Name of the receipt written into the prepared folder.
pub const RECEIPT_FILE: &str = "prepare.json";
/// File name the rebuilt ELF must have (the donor derives the boot name from it).
pub const ELF_FILE: &str = "SCUS_973.28";
pub const CORE_FILE: &str = "CORE.GT4";
/// Schema 2 (2026-10-20) adds the copied image and `qualified_dump`. Schema 1 receipts have no copied image, so
/// they never pass [`check_installed`] and the player is asked for the disc once (docs/launcher/MIGRATIONS.md).
/// Schema 3 (2026-11-02) adds `image.linked_path`. Launchers before it read it as a copy that is missing, and ask for the disc.
pub const RECEIPT_SCHEMA: u32 = 3;
/// Folder under the data folder that holds the copied disc image of an install from before decision 0075.
pub const DISC_FOLDER: &str = "disc";
/// Message shown for every disc that is a PS2 game but not the supported one.
pub const UNSUPPORTED_MESSAGE: &str =
    "This disc is not supported yet. GT4Recomp supports Gran Turismo 4 (USA, SCUS-97328) and Gran Turismo 4 Spec II v1.10 (SCUS-97436).";
const HASH_CHUNK_BYTES: usize = 4 * 1024 * 1024;
pub(crate) const MAX_SYSTEM_CNF_BYTES: u64 = 4096;
const MAX_CORE_BYTES: u64 = 16 * 1024 * 1024;

/// The progress phases the UI shows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Copying,
    Checking,
    Preparing,
}

/// What kind of unsupported disc it is: one extra sentence after [`UNSUPPORTED_MESSAGE`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Unsupported {
    /// SCUS-97436 whose executable is not the pinned Spec II 1.10 one. `beta` is true for the unmodified GT4 Online
    /// Public Beta (same serial, a different executable: it would need its own patch map and translation).
    Spec2Executable { beta: bool },
    /// A PAL or Asian release (SCES, SLES, SCPS, SLPS, ...).
    OtherRegion { serial: String },
    /// SCUS-97328 whose game code (`CORE.GT4`) differs from the supported one.
    OtherRevision { version: String },
    /// Any other PS2 game.
    OtherGame { serial: String },
}

/// Why setup stopped. `user_message` is the one or two plain sentences the UI shows.
#[derive(Debug)]
pub enum SetupError {
    NotPlainIso { extension: Option<String> },
    /// The chosen file is not there (moved, deleted, a disconnected drive) or cannot be opened.
    NotFound,
    NotIso(String),
    NotPlayStation2,
    Unsupported(Unsupported),
    IncompleteOrModified,
    DiffersFromKnownGood,
    Cancelled,
    /// Internal inconsistency after the disc matched: reported as a bug.
    Internal(String),
    Io(String),
}

impl SetupError {
    /// The plain sentences the UI shows, in English (logs and the tests of this crate).
    pub fn user_message(&self) -> String {
        self.user_message_in(Language::fallback())
    }

    /// The plain sentences the UI shows, in `language` (the texts are in `gt4l_core::messages`).
    pub fn user_message_in(&self, language: Language) -> String {
        match self {
            SetupError::NotPlainIso { extension: Some(extension) } if extension.eq_ignore_ascii_case("chd") => {
                messages::text("setup.compressed", language).into()
            }
            SetupError::NotPlainIso { extension: Some(extension) } => {
                messages::render("setup.not_plain_iso_extension", language, &[("extension", extension)])
            }
            SetupError::NotPlainIso { extension: None } => messages::text("setup.not_plain_iso", language).into(),
            SetupError::NotFound => messages::text("setup.not_found", language).into(),
            SetupError::NotIso(_) => messages::text("setup.not_iso", language).into(),
            SetupError::NotPlayStation2 => messages::text("setup.not_playstation2", language).into(),
            SetupError::Unsupported(kind) => {
                let detail = match kind {
                    Unsupported::Spec2Executable { beta: true } => messages::text("setup.public_beta", language).into(),
                    Unsupported::Spec2Executable { beta: false } => {
                        messages::text("setup.spec2_other_program", language).into()
                    }
                    Unsupported::OtherRegion { serial } => {
                        messages::render("setup.other_region", language, &[("serial", serial)])
                    }
                    Unsupported::OtherRevision { version } => {
                        messages::render("setup.gt4_other_code", language, &[("version", version)])
                    }
                    Unsupported::OtherGame { serial } => messages::render("setup.other_game", language, &[("serial", serial)]),
                };
                format!("{} {detail}", messages::text("setup.not_supported", language))
            }
            SetupError::IncompleteOrModified => messages::text("setup.incomplete", language).into(),
            SetupError::DiffersFromKnownGood => messages::text("setup.damaged_dump", language).into(),
            SetupError::Cancelled => messages::text("setup.cancelled", language).into(),
            SetupError::Internal(_) | SetupError::Io(_) => messages::text("setup.prepare_failed", language).into(),
        }
    }

    /// True for refusals of the chosen file (the page says "Not supported"), false for failures of a run.
    pub fn is_disc_refusal(&self) -> bool {
        matches!(
            self,
            SetupError::NotPlainIso { .. }
                | SetupError::NotFound
                | SetupError::NotIso(_)
                | SetupError::NotPlayStation2
                | SetupError::Unsupported(_)
                | SetupError::IncompleteOrModified
                | SetupError::DiffersFromKnownGood
        )
    }
}

impl std::fmt::Display for SetupError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SetupError::NotIso(detail) | SetupError::Internal(detail) | SetupError::Io(detail) => {
                write!(formatter, "{}: {detail}", self.user_message())
            }
            other => write!(formatter, "{}", other.user_message()),
        }
    }
}

impl std::error::Error for SetupError {}

impl From<std::io::Error> for SetupError {
    fn from(error: std::io::Error) -> Self {
        SetupError::Io(error.to_string())
    }
}

/// Written to `prepare.json`. It records what was verified, for support and for later launches.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Receipt {
    pub schema_version: u32,
    pub launcher_version: String,
    pub payload: String,
    /// The game this setup prepared: `gt4` or `spec2` (schema 2 receipts of 1.2.x have none and are `gt4`).
    #[serde(default = "default_game")]
    pub game: String,
    pub disc: DiscReceipt,
    /// The image the game reads: where it is (schema 3), or a copy under `<data>\disc\` (schema 2).
    #[serde(default)]
    pub image: Option<ImageReceipt>,
    /// True when the full image is byte-identical to the qualified v2.00 dump (schema 2).
    #[serde(default)]
    pub qualified_dump: bool,
    /// The name of the known disc image (`Spec II 1.10`, `Spec II 1.10 (PT-BR)`), or the text of an unknown image with the
    /// known executable (`Spec II 1.10 variant`). Empty for Gran Turismo 4.
    #[serde(default)]
    pub image_name: String,
    /// `CORE.GT4` (Gran Turismo 4). Empty for a game with no CORE (Spec II: one plain ELF).
    #[serde(default)]
    pub core: ReceiptIdentity,
    /// The executable as it is on the disc, before the launcher's own preparation (Spec II only).
    #[serde(default)]
    pub original: Option<ReceiptIdentity>,
    pub elf: ReceiptIdentity,
    pub prepared_unix_seconds: u64,
}

fn default_game() -> String {
    "gt4".to_string()
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ReceiptIdentity {
    pub size_bytes: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct DiscReceipt {
    /// The file the player picked.
    pub path: String,
    pub size_bytes: u64,
    pub sha256: String,
    pub serial: String,
    pub version: String,
    pub volume_identifier: String,
}

/// The image: the cheap check of every later launch compares size and modified time.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImageReceipt {
    /// The image's file name (inside `<data>\disc\` when `linked_path` is absent).
    pub file: String,
    /// Where the player keeps the image. Absent: an install from before decision 0075, with its copy in `<data>\disc\`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub linked_path: Option<String>,
    pub size_bytes: u64,
    pub modified_unix_nanos: u64,
    pub sha256: String,
}

/// What a successful identity check learned about the disc.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscSummary {
    pub serial: String,
    pub version: String,
    pub video_mode: String,
    pub volume_identifier: String,
    pub size_bytes: u64,
    /// `VER` equals the qualified version: the full image must then match the disc pin.
    pub qualified_version: bool,
}

/// Inputs of one setup run.
pub struct SetupRequest<'a> {
    /// The file the player picked: it stays where it is.
    pub disc: &'a Path,
    /// `%LOCALAPPDATA%\GT4Recomp\prepared`.
    pub prepared: &'a Path,
    /// `%LOCALAPPDATA%\GT4Recomp\prepared.tmp`.
    pub staging: &'a Path,
    pub pins: &'a Pins,
    pub launcher_version: &'a str,
    pub cancel: &'a AtomicBool,
}

/// The path a receipt records for the image: absolute, so it means the same whatever folder the launcher starts from.
pub(crate) fn linked_path(disc: &Path) -> Result<String, SetupError> {
    Ok(std::path::absolute(disc)?.display().to_string())
}

/// The file name a receipt records for the image.
pub(crate) fn image_file_name(disc: &Path) -> String {
    disc.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default()
}

/// Fast identity checks (no full hash): extension, ISO structure, `SYSTEM.CNF`, `CORE.GT4`, size.
pub fn precheck(disc: &Path, pins: &Pins) -> Result<DiscSummary, SetupError> {
    let extension = disc.extension().and_then(|value| value.to_str()).map(str::to_string);
    match extension.as_deref() {
        Some(value) if value.eq_ignore_ascii_case("iso") => {}
        other => return Err(SetupError::NotPlainIso { extension: other.map(str::to_string) }),
    }
    if !disc.is_file() || File::open(disc).is_err() {
        return Err(SetupError::NotFound);
    }
    let mut iso = Iso::open(disc).map_err(|error| match error {
        IsoError::Io(error) => SetupError::Io(error.to_string()),
        other => SetupError::NotIso(other.to_string()),
    })?;
    let system_entry = iso.find("SYSTEM.CNF;1").map_err(|_| SetupError::NotPlayStation2)?;
    let system_bytes = iso
        .read(&system_entry, MAX_SYSTEM_CNF_BYTES)
        .map_err(|_| SetupError::IncompleteOrModified)?;
    let config = cnf::parse(&system_bytes).map_err(SetupError::NotIso)?;
    let volume_identifier = iso.volume_identifier().to_string();
    let length_bytes = iso.length_bytes();

    if config.serial != pins.serial || config.boot_path != pins.boot_path {
        return Err(SetupError::Unsupported(classify_other(&config.serial, &volume_identifier)));
    }
    let qualified_version = config.version == pins.version;
    if length_bytes < iso.volume_space_bytes() {
        return Err(SetupError::IncompleteOrModified);
    }
    let core_entry = iso.find("CORE.GT4;1").map_err(|_| SetupError::IncompleteOrModified)?;
    let core = iso.read(&core_entry, MAX_CORE_BYTES).map_err(|_| SetupError::IncompleteOrModified)?;
    let core_identity = Identity { size_bytes: core.len() as u64, sha256: elf::sha256_hex(&core) };
    if core_identity != pins.core {
        return Err(if qualified_version {
            SetupError::DiffersFromKnownGood
        } else {
            SetupError::Unsupported(Unsupported::OtherRevision { version: config.version })
        });
    }
    if qualified_version && length_bytes != pins.disc.size_bytes {
        return Err(SetupError::IncompleteOrModified);
    }
    Ok(DiscSummary {
        serial: config.serial,
        version: config.version,
        video_mode: config.video_mode,
        volume_identifier,
        size_bytes: length_bytes,
        qualified_version,
    })
}

fn classify_other(serial: &str, volume_identifier: &str) -> Unsupported {
    let _ = volume_identifier;
    const OTHER_REGIONS: &[&str] =
        &["SCES", "SLES", "SCED", "SLED", "SCPS", "SLPS", "SCAJ", "SLAJ", "SCKA", "SLKA", "PAPX", "PBPX"];
    if OTHER_REGIONS.iter().any(|prefix| serial.starts_with(prefix)) {
        return Unsupported::OtherRegion { serial: serial.to_string() };
    }
    Unsupported::OtherGame { serial: serial.to_string() }
}

pub(crate) fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// Full SHA-256 of a file, reporting the fraction done and honouring cancellation.
pub fn hash_image(
    path: &Path,
    cancel: &AtomicBool,
    progress: &mut dyn FnMut(Stage, f32),
) -> Result<Identity, SetupError> {
    hash_image_as(path, Stage::Checking, cancel, progress)
}

/// [`hash_image`], reporting as `stage`.
pub(crate) fn hash_image_as(
    path: &Path,
    stage: Stage,
    cancel: &AtomicBool,
    progress: &mut dyn FnMut(Stage, f32),
) -> Result<Identity, SetupError> {
    let mut file = File::open(path)?;
    let total = file.metadata()?.len().max(1);
    let mut digest = Sha256::new();
    let mut buffer = vec![0u8; HASH_CHUNK_BYTES];
    let mut done = 0u64;
    loop {
        if cancel.load(Ordering::Relaxed) {
            return Err(SetupError::Cancelled);
        }
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        digest.update(&buffer[..read]);
        done += read as u64;
        progress(stage, done as f32 / total as f32);
    }
    Ok(Identity { size_bytes: done, sha256: hex(&digest.finalize()) })
}

/// Runs the whole setup. On success the prepared folder holds the receipt, CORE and ELF, and the image is where it was.
pub fn run(request: &SetupRequest<'_>, progress: &mut dyn FnMut(Stage, f32)) -> Result<Receipt, SetupError> {
    let summary = precheck(request.disc, request.pins)?;
    progress(Stage::Copying, 0.0);
    let result = hash_image_as(request.disc, Stage::Copying, request.cancel, progress)
        .and_then(|identity| finish(request, &summary, &identity, progress));
    if result.is_err() {
        let _ = fs::remove_dir_all(request.staging);
    }
    result
}

fn finish(
    request: &SetupRequest<'_>,
    summary: &DiscSummary,
    identity: &Identity,
    progress: &mut dyn FnMut(Stage, f32),
) -> Result<Receipt, SetupError> {
    let pins = request.pins;
    progress(Stage::Checking, 0.0);
    let qualified_dump = identity.sha256 == pins.disc.sha256 && identity.size_bytes == pins.disc.size_bytes;
    if summary.qualified_version && !qualified_dump {
        return Err(SetupError::DiffersFromKnownGood);
    }
    if identity.size_bytes != summary.size_bytes {
        return Err(SetupError::IncompleteOrModified);
    }
    progress(Stage::Checking, 1.0);
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }

    progress(Stage::Preparing, 0.0);
    let (core_bytes, elf_bytes) = extract_and_rebuild(request.disc, pins)?;
    progress(Stage::Preparing, 0.5);
    write_staging(request, &core_bytes, &elf_bytes)?;

    let metadata = fs::metadata(request.disc)?;
    let receipt = Receipt {
        schema_version: RECEIPT_SCHEMA,
        launcher_version: request.launcher_version.to_string(),
        payload: "bundled".to_string(),
        game: default_game(),
        disc: DiscReceipt {
            path: request.disc.display().to_string(),
            size_bytes: identity.size_bytes,
            sha256: identity.sha256.clone(),
            serial: summary.serial.clone(),
            version: summary.version.clone(),
            volume_identifier: summary.volume_identifier.clone(),
        },
        image: Some(ImageReceipt {
            file: image_file_name(request.disc),
            linked_path: Some(linked_path(request.disc)?),
            size_bytes: metadata.len(),
            modified_unix_nanos: modified_nanos(&metadata),
            sha256: identity.sha256.clone(),
        }),
        qualified_dump,
        image_name: String::new(),
        core: ReceiptIdentity { size_bytes: core_bytes.len() as u64, sha256: elf::sha256_hex(&core_bytes) },
        original: None,
        elf: ReceiptIdentity { size_bytes: elf_bytes.len() as u64, sha256: elf::sha256_hex(&elf_bytes) },
        prepared_unix_seconds: unix_seconds(),
    };
    let receipt_bytes = serde_json::to_vec_pretty(&receipt).map_err(|error| SetupError::Internal(error.to_string()))?;
    write_synced(&request.staging.join(RECEIPT_FILE), &receipt_bytes)?;
    if request.cancel.load(Ordering::Relaxed) {
        return Err(SetupError::Cancelled);
    }
    swap_into_place(request.staging, request.prepared)?;
    progress(Stage::Preparing, 1.0);
    Ok(receipt)
}

pub(crate) fn modified_nanos(metadata: &fs::Metadata) -> u64 {
    metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_nanos() as u64)
        .unwrap_or(0)
}

fn extract_and_rebuild(image: &Path, pins: &Pins) -> Result<(Vec<u8>, Vec<u8>), SetupError> {
    let mut iso = Iso::open(image).map_err(|error| SetupError::Io(error.to_string()))?;
    let entry = iso
        .find("CORE.GT4;1")
        .map_err(|error| SetupError::Internal(error.to_string()))?;
    let core = iso
        .read(&entry, MAX_CORE_BYTES)
        .map_err(|error| SetupError::Internal(error.to_string()))?;
    let core_identity = Identity { size_bytes: core.len() as u64, sha256: elf::sha256_hex(&core) };
    if core_identity != pins.core {
        return Err(SetupError::Internal("CORE.GT4 differs from the pinned identity".into()));
    }
    let image = elf::reconstruct(&core, &pins.policy).map_err(|error| SetupError::Internal(error.to_string()))?;
    let elf_identity = Identity { size_bytes: image.len() as u64, sha256: elf::sha256_hex(&image) };
    if elf_identity != pins.elf {
        return Err(SetupError::Internal("rebuilt ELF differs from the pinned identity".into()));
    }
    Ok((core, image))
}

fn write_staging(request: &SetupRequest<'_>, core: &[u8], image: &[u8]) -> Result<(), SetupError> {
    if request.staging.exists() {
        fs::remove_dir_all(request.staging)?;
    }
    fs::create_dir_all(request.staging)?;
    write_synced(&request.staging.join(CORE_FILE), core)?;
    write_synced(&request.staging.join(ELF_FILE), image)?;
    Ok(())
}

pub(crate) fn write_synced(path: &Path, bytes: &[u8]) -> Result<(), SetupError> {
    let mut file = File::create(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}

/// Moves the staging folder to `prepared`. The old folder is removed only after the new one is in place.
pub(crate) fn swap_into_place(staging: &Path, prepared: &Path) -> Result<(), SetupError> {
    let old: Option<PathBuf> = if prepared.exists() {
        let parked = prepared.with_file_name(format!("prepared.old-{}", unix_seconds()));
        fs::rename(prepared, &parked)?;
        Some(parked)
    } else {
        None
    };
    fs::rename(staging, prepared)?;
    if let Some(old) = old {
        let _ = fs::remove_dir_all(old);
    }
    Ok(())
}

pub(crate) fn unix_seconds() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|duration| duration.as_secs()).unwrap_or(0)
}

/// Fast check: the prepared files exist with the pinned sizes.
pub fn verify_prepared(prepared: &Path, pins: &Pins) -> Result<(), String> {
    let elf_path = prepared.join(ELF_FILE);
    let core_path = prepared.join(CORE_FILE);
    let elf_size = fs::metadata(&elf_path).map_err(|_| format!("{ELF_FILE} is missing"))?.len();
    if elf_size != pins.elf.size_bytes {
        return Err(format!("{ELF_FILE} has size {elf_size}, expected {}", pins.elf.size_bytes));
    }
    let core_size = fs::metadata(&core_path).map_err(|_| format!("{CORE_FILE} is missing"))?.len();
    if core_size != pins.core.size_bytes {
        return Err(format!("{CORE_FILE} has size {core_size}, expected {}", pins.core.size_bytes));
    }
    Ok(())
}

/// Reads `prepare.json`. `None` when it is missing or unreadable.
pub fn read_receipt(prepared: &Path) -> Option<Receipt> {
    let bytes = fs::read(prepared.join(RECEIPT_FILE)).ok()?;
    serde_json::from_slice(&bytes).ok()
}

/// What a later launch knows without hashing anything.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Installed {
    /// Nothing was ever set up.
    NotSetUp,
    /// Set up once, but the files are gone or changed: the player must choose the disc again.
    Missing { reason: String },
    /// Set up once, but the image is not at this path any more, or it changed: choosing it again links it.
    LinkBroken { image: PathBuf },
    /// Ready: the image the game starts with.
    Ready { image: PathBuf, serial: String, version: String },
}

/// The cheap check of every launch: the receipt, the prepared sizes, and the image's size and modified time as recorded
/// at setup. No hash runs here.
pub fn check_installed(prepared: &Path, disc_folder: &Path, pins: &Pins) -> Installed {
    check_installed_with(prepared, disc_folder, &|folder| verify_prepared(folder, pins))
}

/// [`check_installed`] with the game's own check of the prepared files (sizes of its ELF and, for Gran Turismo 4, CORE).
pub fn check_installed_with(prepared: &Path, disc_folder: &Path, verify: &dyn Fn(&Path) -> Result<(), String>) -> Installed {
    if !prepared.join(RECEIPT_FILE).exists() {
        if prepared.exists() || disc_folder.exists() {
            return Installed::Missing { reason: "the game files are incomplete".into() };
        }
        return Installed::NotSetUp;
    }
    let Some(receipt) = read_receipt(prepared) else {
        return Installed::Missing { reason: "the setup record is damaged".into() };
    };
    let Some(image) = receipt.image.as_ref().filter(|_| receipt.schema_version >= 2) else {
        return Installed::Missing { reason: "this version keeps its own copy of the disc image".into() };
    };
    if let Err(reason) = verify(prepared) {
        return Installed::Missing { reason };
    }
    if let Some(linked) = &image.linked_path {
        let path = PathBuf::from(linked);
        return match fs::metadata(&path) {
            Ok(metadata) if metadata.len() == image.size_bytes && modified_nanos(&metadata) == image.modified_unix_nanos => {
                Installed::Ready { image: path, serial: receipt.disc.serial, version: receipt.disc.version }
            }
            _ => Installed::LinkBroken { image: path },
        };
    }
    let path = disc_folder.join(&image.file);
    let Ok(metadata) = fs::metadata(&path) else {
        return Installed::Missing { reason: format!("{} is missing", image.file) };
    };
    if metadata.len() != image.size_bytes || modified_nanos(&metadata) != image.modified_unix_nanos {
        return Installed::Missing { reason: format!("{} has changed", image.file) };
    }
    Installed::Ready { image: path, serial: receipt.disc.serial, version: receipt.disc.version }
}
