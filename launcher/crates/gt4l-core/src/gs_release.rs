//! TASK-043: download, verify and install the PCSX2 GS renderer (`gt4-gs.exe`) from GitHub Releases.
//!
//! The launcher only downloads a package and starts `gt4-gs.exe` in its own process. No GPL code is in the
//! launcher (decision 0070). Flow of [`install_latest`]:
//! 1. read the release list (`ReleaseSource::releases_json`); skip drafts and prereleases;
//! 2. pick the highest stable version that is at least [`MINIMUM_VERSION`];
//! 3. download `SHA256SUMS` and the package `gt4-gs-<version>-windows-x64.zip`; the package must match its sum;
//! 4. unpack into a staging folder; `gt4-gs.exe` must exist and must match its line in `SHA256SUMS`;
//! 5. rename the staging folder to `<gs root>\<version>\`, then point `current.txt` at it.
//!
//! The previous version stays in place until the new one has passed step 4 and step 5. Any failure leaves the
//! installed version as it was; the caller then reports the GSdx error and does not start the game (see
//! `choose_renderer` in `launch.rs`, TASK-096). Every path written here passes [`crate::guard::check_write`] first.

use std::collections::BTreeMap;
use std::fs;
use std::io::{Cursor, Read, Write};
use std::path::{Path, PathBuf};

use crate::guard;
use crate::releases::{self, Stream};
pub use crate::releases::{Asset, FolderSource, GithubSource, Release, ReleaseSource, parse_releases};
pub use crate::version::Version;

/// GitHub repository that publishes the renderer.
pub const RELEASE_REPOSITORY: &str = releases::RENDERER_REPOSITORY;
/// Lowest stable version the launcher accepts.
pub const MINIMUM_VERSION: Version = releases::RENDERER_MINIMUM;
/// Name of the checksum list attached to each release.
pub const CHECKSUM_ASSET: &str = releases::CHECKSUM_ASSET;
/// The renderer program inside the package.
pub const EXECUTABLE_NAME: &str = "gt4-gs.exe";
/// Name of the state file in the renderer root: the version in use.
pub const CURRENT_FILE: &str = "current.txt";
/// TASK-155 (issue #131; re-done from Luan Antony's PR #136): an empty `portable.ini` beside gt4-gs.exe makes gt4-gs keep its
/// own settings beside it instead of looking up the Documents folder first; a Documents folder moved into OneDrive stopped
/// it at start ("Failed to initialize config") before it read `--config`. Written at install and before each GSdx start.
pub const PORTABLE_MARKER: &str = "portable.ini";

/// Creates [`PORTABLE_MARKER`] in `folder` when it is missing; an existing file is left as it is.
pub fn ensure_portable_marker(folder: &Path) -> std::io::Result<()> {
    let marker = folder.join(PORTABLE_MARKER);
    if marker.is_file() {
        return Ok(());
    }
    fs::write(marker, b"")
}
/// Largest package the launcher accepts (bytes). Protects memory on a bad or hostile response.
pub const MAX_PACKAGE_BYTES: u64 = 256 * 1024 * 1024;

/// The package file name of a version.
pub fn package_name(version: Version) -> String {
    Stream::Renderer.package_name(version)
}

/// Parses `SHA256SUMS` text (`<hex>  <name>` or `<hex> *<name>` per line) into name -> lowercase hex.
pub fn parse_checksums(text: &str) -> BTreeMap<String, String> {
    let mut sums = BTreeMap::new();
    for line in text.lines() {
        let line = line.trim_end();
        let Some((hex, rest)) = line.split_once(' ') else { continue };
        let name = rest.trim_start_matches([' ', '*']);
        if hex.len() == 64 && hex.chars().all(|c| c.is_ascii_hexdigit()) && !name.is_empty() {
            sums.insert(name.to_string(), hex.to_ascii_lowercase());
        }
    }
    sums
}

/// Lowercase hex SHA-256 of bytes.
pub fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(bytes);
    digest.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// What an installed renderer looks like to the launcher.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Installed {
    pub version: Version,
    pub executable: PathBuf,
    /// SHA-256 of `gt4-gs.exe`, passed to the game as `GT4_GS_PROCESS_SHA256`.
    pub executable_sha256: String,
}

/// Result of a successful install or update.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// The version was installed (or reinstalled after a damaged folder).
    Installed(Installed),
    /// The newest stable version is already in use and passes the check.
    UpToDate(Installed),
}

impl Outcome {
    pub fn installed(&self) -> &Installed {
        match self {
            Outcome::Installed(installed) | Outcome::UpToDate(installed) => installed,
        }
    }
}

/// Steps reported to the caller while an install runs (the setup page and the update popup show them).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// Reading the release list.
    Listing,
    /// Downloading `SHA256SUMS` and the package.
    Downloading,
    /// Bytes of the package received so far, and its size when the server sent it.
    Transfer { done: u64, total: Option<u64> },
    /// Checking sums and unpacking into the staging folder.
    Installing,
}

/// The folder of one version under the renderer root.
pub fn version_directory(root: &Path, version: Version) -> PathBuf {
    root.join(version.text())
}

/// True when the renderer must be installed at launch: nothing usable is installed, or the installed version is below
/// [`MINIMUM_VERSION`] (for example gt4-gs 1.0.1 under launcher 1.1.0). [`install_latest`] then installs the newest
/// stable release, which is at least the minimum. Until that succeeds, the older version keeps working (the game only
/// sends protocol 1.3 messages to a 1.3 consumer).
pub fn needs_install(root: &Path) -> bool {
    match installed(root) {
        Some(current) => current.version < MINIMUM_VERSION,
        None => true,
    }
}

/// The installed version named by `current.txt`, if its folder passes the check. Any problem gives `None`.
pub fn installed(root: &Path) -> Option<Installed> {
    let text = fs::read_to_string(root.join(CURRENT_FILE)).ok()?;
    let version = Version::parse(text.trim())?;
    check_installed(&version_directory(root, version), version).ok()
}

/// The check of an installed folder: the program exists and its hash is the one recorded at install time.
fn check_installed(directory: &Path, version: Version) -> Result<Installed, String> {
    let executable = directory.join(EXECUTABLE_NAME);
    let bytes = fs::read(&executable).map_err(|error| format!("{}: {error}", executable.display()))?;
    let recorded = fs::read_to_string(directory.join("sha256.txt"))
        .map_err(|error| format!("{}: {error}", directory.display()))?;
    let actual = sha256_hex(&bytes);
    if recorded.trim() != actual {
        return Err(format!("{EXECUTABLE_NAME} does not match its recorded hash"));
    }
    Ok(Installed { version, executable, executable_sha256: actual })
}

/// Downloads, verifies and installs the newest stable release (see the module comment).
pub fn install_latest(source: &dyn ReleaseSource, root: &Path) -> Result<Outcome, String> {
    install_latest_reporting(source, root, &mut |_| {})
}

/// [`install_latest`] that reports each [`Step`].
pub fn install_latest_reporting(
    source: &dyn ReleaseSource,
    root: &Path,
    progress: &mut dyn FnMut(Step),
) -> Result<Outcome, String> {
    progress(Step::Listing);
    let releases = parse_releases(&source.releases_json()?, Stream::Renderer)?;
    let release = releases::select_latest(&releases, MINIMUM_VERSION).ok_or_else(|| {
        format!("no stable release of {RELEASE_REPOSITORY} at or above {}", MINIMUM_VERSION.text())
    })?;
    let version = release.version.expect("select_latest only returns versions");
    let asset = |name: &str| {
        release
            .asset(name)
            .cloned()
            .ok_or_else(|| format!("release {} has no {name}", release.tag))
    };

    if let Some(current) = installed(root) {
        if current.version == version {
            return Ok(Outcome::UpToDate(current));
        }
    }

    progress(Step::Downloading);
    let sums_text = source.download(&asset(CHECKSUM_ASSET)?.url)?;
    let sums = parse_checksums(&String::from_utf8_lossy(&sums_text));
    let package = package_name(version);
    let expected = sums.get(&package).ok_or_else(|| format!("{CHECKSUM_ASSET} has no line for {package}"))?;
    let package_bytes = source.download_reporting(&asset(&package)?.url, &mut |done, total| {
        progress(Step::Transfer { done, total })
    })?;
    if package_bytes.len() as u64 > MAX_PACKAGE_BYTES {
        return Err(format!("{package} is larger than the limit"));
    }
    if sha256_hex(&package_bytes) != *expected {
        return Err(format!("{package} does not match its SHA-256 (corrupt or changed download)"));
    }

    progress(Step::Installing);
    fs::create_dir_all(root).map_err(|error| format!("{}: {error}", root.display()))?;
    let staging = root.join(format!(".staging-{}", version.text()));
    guard::check_write(root, &staging)?;
    let _ = fs::remove_dir_all(&staging);
    let result = stage_and_publish(&package_bytes, &staging, root, version, &sums);
    if result.is_err() {
        let _ = fs::remove_dir_all(&staging);
    }
    result
}

fn stage_and_publish(
    package: &[u8],
    staging: &Path,
    root: &Path,
    version: Version,
    sums: &BTreeMap<String, String>,
) -> Result<Outcome, String> {
    unpack_zip(package, staging, root)?;
    let executable = staging.join(EXECUTABLE_NAME);
    let bytes = fs::read(&executable).map_err(|_| format!("the package has no {EXECUTABLE_NAME}"))?;
    let actual = sha256_hex(&bytes);
    if let Some(expected) = sums.get(EXECUTABLE_NAME) {
        if *expected != actual {
            return Err(format!("{EXECUTABLE_NAME} does not match {CHECKSUM_ASSET}"));
        }
    }
    fs::write(staging.join("sha256.txt"), format!("{actual}\n")).map_err(|error| error.to_string())?;
    ensure_portable_marker(staging).map_err(|error| format!("{PORTABLE_MARKER}: {error}"))?;

    let target = version_directory(root, version);
    guard::check_write(root, &target)?;
    if target.exists() {
        fs::remove_dir_all(&target).map_err(|error| format!("{}: {error}", target.display()))?;
    }
    fs::rename(staging, &target).map_err(|error| format!("{}: {error}", target.display()))?;
    let installed = check_installed(&target, version)?;

    let previous = installed_version(root);
    write_atomic(root, &root.join(CURRENT_FILE), &format!("{}\n", version.text()))?;
    remove_other_versions(root, &[Some(version), previous])?;
    Ok(Outcome::Installed(installed))
}

/// The version named by `current.txt`, whether or not its folder passes the check.
fn installed_version(root: &Path) -> Option<Version> {
    let text = fs::read_to_string(root.join(CURRENT_FILE)).ok()?;
    Version::parse(text.trim())
}

/// Removes version folders other than the ones kept (the new version and the previous one).
fn remove_other_versions(root: &Path, keep: &[Option<Version>]) -> Result<(), String> {
    let entries = fs::read_dir(root).map_err(|error| error.to_string())?;
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        let Some(version) = Version::parse(&name) else { continue };
        if !keep.contains(&Some(version)) && entry.path().is_dir() {
            guard::check_write(root, &entry.path())?;
            let _ = fs::remove_dir_all(entry.path());
        }
    }
    Ok(())
}

/// Writes `text` to `path` through a temporary file and a rename. `guard_root` is the folder the guard
/// checks against (see [`crate::guard`]).
pub fn write_atomic(guard_root: &Path, path: &Path, text: &str) -> Result<(), String> {
    guard::check_write(guard_root, path)?;
    let temporary = path.with_extension("tmp");
    {
        let mut file = fs::File::create(&temporary).map_err(|error| error.to_string())?;
        file.write_all(text.as_bytes()).map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())?;
    }
    fs::rename(&temporary, path).map_err(|error| format!("{}: {error}", path.display()))
}

/// The one top-level folder every entry of the package sits in, when the program is inside it and not at the root.
/// The real gt4-gs 1.0.1 package wraps everything in `gt4-gs-1.0.1-windows-x64/` (checked 2026-10-20); the folder is
/// removed on unpack so `gt4-gs.exe` lands at the version folder's root either way.
fn wrapping_folder(names: &[PathBuf]) -> Option<PathBuf> {
    if names.iter().any(|name| name == Path::new(EXECUTABLE_NAME)) {
        return None;
    }
    let first = names.first()?.components().next()?.as_os_str().to_owned();
    let folder = PathBuf::from(first);
    let all_inside = names.iter().all(|name| name.starts_with(&folder));
    (all_inside && names.iter().any(|name| *name == folder.join(EXECUTABLE_NAME))).then_some(folder)
}

/// Unpacks a zip into `destination`. Entries that would leave the folder are refused. A single wrapping folder
/// around the program is removed (see [`wrapping_folder`]).
fn unpack_zip(bytes: &[u8], destination: &Path, guard_root: &Path) -> Result<(), String> {
    let mut archive = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|error| format!("package is not a zip: {error}"))?;
    fs::create_dir_all(destination).map_err(|error| error.to_string())?;
    let mut names = Vec::new();
    for index in 0..archive.len() {
        let entry = archive.by_index(index).map_err(|error| error.to_string())?;
        names.push(entry.enclosed_name().ok_or_else(|| format!("unsafe path in package: {}", entry.name()))?);
    }
    let strip = wrapping_folder(&names);
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|error| error.to_string())?;
        let relative = entry.enclosed_name().ok_or_else(|| format!("unsafe path in package: {}", entry.name()))?;
        let relative = match &strip {
            Some(folder) => relative.strip_prefix(folder).map(Path::to_path_buf).unwrap_or(relative),
            None => relative,
        };
        if relative.as_os_str().is_empty() {
            continue;
        }
        let target = destination.join(relative);
        guard::check_write(guard_root, &target)?;
        if entry.is_dir() {
            fs::create_dir_all(&target).map_err(|error| error.to_string())?;
            continue;
        }
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let mut data = Vec::new();
        entry.by_ref().take(MAX_PACKAGE_BYTES).read_to_end(&mut data).map_err(|error| error.to_string())?;
        fs::write(&target, data).map_err(|error| error.to_string())?;
    }
    Ok(())
}

/// The renderer root: `%LOCALAPPDATA%\GT4Recomp\gs` (the caller passes the local data folder).
pub fn renderer_root(local_data: &Path) -> PathBuf {
    local_data.join("GT4Recomp").join("gs")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checksum_lines_parse_both_common_forms() {
        let text = format!("{}  a.zip\n{} *b.exe\nnot a line\n", "A".repeat(64), "b".repeat(64));
        let sums = parse_checksums(&text);
        assert_eq!(sums.get("a.zip").map(String::as_str), Some("a".repeat(64).as_str()));
        assert_eq!(sums.get("b.exe").map(String::as_str), Some("b".repeat(64).as_str()));
        assert_eq!(sums.len(), 2);
    }

    #[test]
    fn sha256_matches_the_known_empty_digest() {
        assert_eq!(sha256_hex(b""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    #[test]
    fn the_minimum_is_1_2_1_with_the_real_asset_name() {
        assert_eq!(MINIMUM_VERSION, Version(1, 2, 1));
        assert_eq!(package_name(Version(1, 2, 0)), "gt4-gs-1.2.0-windows-x64.zip");
    }
}
