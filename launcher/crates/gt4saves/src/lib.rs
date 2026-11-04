//! gt4saves: PS2 memory card images, GT4 save folders and card import/export (TASK-025).
//!
//! * [`ecc`]: the 12 ECC bytes of each 512-byte page (fitted to MCMAN-written cards).
//! * [`card`]: read and build 8 MB card images (superblock, FAT, directories, files).
//! * [`psu`]: single-save `.psu` files.
//! * [`folder`]: the folder form of an item (save or replay) in the user data folder.
//! * [`library`]: Saves/, Replays/ and backups/ with import, export, activate, build and harvest.
//! * [`migration`]: the one-time copy of the old AppData library to `Documents\Gran Turismo 4`.

pub mod card;
pub mod ecc;
pub mod folder;
pub mod library;
pub mod migration;
pub mod names;
pub mod psu;

pub use card::{Card, CardError, DirEntry, Node};
pub use library::{Library, LibraryError, LibraryPaths, Report};
pub use names::SaveNames;

/// Current UTC time as (year, month, day, hour, minute, second).
fn utc_now() -> (i64, i64, i64, i64, i64, i64) {
    utc_parts(std::time::SystemTime::now())
}

/// A time as UTC (year, month, day, hour, minute, second). Times before 1970 count as 1970.
fn utc_parts(time: std::time::SystemTime) -> (i64, i64, i64, i64, i64, i64) {
    let seconds = time.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let days = seconds.div_euclid(86_400);
    let secs_of_day = seconds.rem_euclid(86_400);
    // Civil date from a day count (Howard Hinnant's algorithm).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day, secs_of_day / 3600, secs_of_day / 60 % 60, secs_of_day % 60)
}

/// ISO 8601 UTC text of a time (`2026-10-21T12:00:00Z`), for the Saves and Replays pages.
pub fn iso8601_utc(time: std::time::SystemTime) -> String {
    let (year, month, day, hour, minute, second) = utc_parts(time);
    format!("{year:04}-{month:02}-{day:02}T{hour:02}:{minute:02}:{second:02}Z")
}

/// UTC timestamp `YYYYMMDD-HHMMSS` for backup and replaced-item names.
pub fn timestamp() -> String {
    let (year, month, day, hour, minute, second) = utc_now();
    format!("{year:04}{month:02}{day:02}-{hour:02}{minute:02}{second:02}")
}

/// Current time in the card's 8-byte timestamp form: unused, second, minute, hour, day,
/// month, year (u16 little endian).
pub fn card_time_now() -> [u8; 8] {
    let (year, month, day, hour, minute, second) = utc_now();
    let year = (year as u16).to_le_bytes();
    [0, second as u8, minute as u8, hour as u8, day as u8, month as u8, year[0], year[1]]
}