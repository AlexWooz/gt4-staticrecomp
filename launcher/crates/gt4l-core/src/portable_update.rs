//! Self-update of the portable build by swapping program files (TASK-098, release 1.3.0).
//!
//! The portable launcher keeps its program files (`GT4Recomp.exe`, `SDL3.dll`, `game\`, the licence texts) in its own
//! folder, next to the player's `user\` and `data\`. An update is a zip, `GT4Recomp-<version>-portable.zip`, listed in the
//! release `SHA256SUMS`. The steps, all inside the portable folder (no administrator prompt, no system folder):
//!
//! 1. the launcher downloads the zip into `.update-download\` and checks its SHA-256 (`crate::releases`);
//! 2. [`install_package`] extracts it into `.update-staging\` ([`UpdateFs::extract_zip`]; every entry stays inside the
//!    folder, `GT4Recomp.exe` must be there);
//! 3. [`swap_in`] renames each live top-level item into `.update-old\` (Windows allows renaming a running executable) and
//!    renames the staged item into its place; `user\` and `data\` are never touched. A failed rename rolls every item
//!    back, as the installer's swap does (TASK-091), and the staging folder is removed;
//! 4. the launcher starts the new `GT4Recomp.exe` with [`RESTART_ARGUMENT`] and exits;
//! 5. at the next start [`finish_previous_update`] removes `.update-old\` (the old executable cannot go while it runs).
//!
//! A marker file, `.update-swapping`, exists while a swap is in progress; if it is found at start (a crash, power loss),
//! [`finish_previous_update`] puts the previous files back first. The file system is injected ([`UpdateFs`]) so the swap
//! and the rollback are tested with failures at every step.

use std::io;
use std::path::{Path, PathBuf};

use crate::releases::{self, Asset, Release, Stream, CHECKSUM_ASSET};
#[cfg(test)]
use crate::version::Version;

/// Where the verified zip is downloaded.
pub const DOWNLOAD_FOLDER: &str = ".update-download";
/// Where the zip is extracted before the swap.
pub const STAGING_FOLDER: &str = ".update-staging";
/// Where the replaced program files wait until the next start.
pub const OLD_FOLDER: &str = ".update-old";
/// Exists while a swap is in progress.
pub const SWAPPING_MARKER: &str = ".update-swapping";
/// The argument of the launcher started after an update: it waits for the old launcher to exit (single instance).
pub const RESTART_ARGUMENT: &str = "--after-update";
/// The launcher executable inside the zip and the folder.
pub const LAUNCHER_FILE: &str = "GT4Recomp.exe";
/// Top-level names the swap never touches: the player's folders.
pub const KEPT_ITEMS: [&str; 2] = ["user", "data"];

/// Largest total size of an extracted package (the program is well under 100 MB).
pub const MAX_EXTRACTED_BYTES: u64 = 2 * 1024 * 1024 * 1024;
/// Most entries a package may hold.
pub const MAX_ENTRIES: usize = 20_000;

/// The file operations of an update. [`RealFs`] for the launcher; a fake in tests.
pub trait UpdateFs {
    fn exists(&self, path: &Path) -> bool;
    /// The names (not paths) of the entries directly inside `dir`.
    fn list(&self, dir: &Path) -> io::Result<Vec<String>>;
    fn rename(&self, from: &Path, to: &Path) -> io::Result<()>;
    fn create_dir_all(&self, path: &Path) -> io::Result<()>;
    /// Removes a file or a folder with everything in it. A missing path is not an error.
    fn remove_all(&self, path: &Path) -> io::Result<()>;
    fn write_file(&self, path: &Path, bytes: &[u8]) -> io::Result<()>;
    /// Extracts `zip` into `dest` (created). Every entry must stay inside `dest`.
    fn extract_zip(&self, zip: &Path, dest: &Path) -> Result<(), String>;
}

/// The real file system.
pub struct RealFs;

impl UpdateFs for RealFs {
    fn exists(&self, path: &Path) -> bool {
        path.symlink_metadata().is_ok()
    }

    fn list(&self, dir: &Path) -> io::Result<Vec<String>> {
        let mut names = Vec::new();
        for entry in std::fs::read_dir(dir)? {
            names.push(entry?.file_name().to_string_lossy().into_owned());
        }
        names.sort();
        Ok(names)
    }

    fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
        std::fs::rename(from, to)
    }

    fn create_dir_all(&self, path: &Path) -> io::Result<()> {
        std::fs::create_dir_all(path)
    }

    fn remove_all(&self, path: &Path) -> io::Result<()> {
        match path.symlink_metadata() {
            Ok(meta) if meta.is_dir() => std::fs::remove_dir_all(path),
            Ok(_) => std::fs::remove_file(path),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
        }
    }

    fn write_file(&self, path: &Path, bytes: &[u8]) -> io::Result<()> {
        std::fs::write(path, bytes)
    }

    fn extract_zip(&self, zip_path: &Path, dest: &Path) -> Result<(), String> {
        extract_zip_safely(zip_path, dest)
    }
}

/// Extracts a zip into `dest` with the limits above. An entry whose name would leave `dest` (`..`, a drive, an absolute
/// path) fails the whole extraction.
pub fn extract_zip_safely(zip_path: &Path, dest: &Path) -> Result<(), String> {
    let file = std::fs::File::open(zip_path).map_err(|error| format!("{}: {error}", zip_path.display()))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|error| format!("the update package is not a zip: {error}"))?;
    if archive.len() > MAX_ENTRIES {
        return Err(format!("the update package has {} entries (limit {MAX_ENTRIES})", archive.len()));
    }
    std::fs::create_dir_all(dest).map_err(|error| format!("{}: {error}", dest.display()))?;
    let mut total = 0u64;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|error| format!("update package entry {index}: {error}"))?;
        let relative = entry
            .enclosed_name()
            .ok_or_else(|| format!("the update package has an unsafe entry name '{}'", entry.name()))?;
        let target = dest.join(&relative);
        if entry.is_dir() {
            std::fs::create_dir_all(&target).map_err(|error| format!("{}: {error}", target.display()))?;
            continue;
        }
        total = total.saturating_add(entry.size());
        if total > MAX_EXTRACTED_BYTES {
            return Err("the update package is larger than expected".to_string());
        }
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
        }
        let mut output = std::fs::File::create(&target).map_err(|error| format!("{}: {error}", target.display()))?;
        io::copy(&mut entry, &mut output).map_err(|error| format!("{}: {error}", target.display()))?;
    }
    Ok(())
}

/// One step of a swap, kept so a failure can undo them in reverse order.
#[derive(Clone, Debug, PartialEq, Eq)]
enum Step {
    /// The live item was moved to `.update-old`.
    MovedAside(String),
    /// The staged item was moved to its live place.
    Placed(String),
}

/// What a finished swap did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SwapReport {
    /// The top-level items that were replaced or added.
    pub items: Vec<String>,
}

fn is_kept(name: &str) -> bool {
    KEPT_ITEMS.iter().any(|kept| kept.eq_ignore_ascii_case(name))
}

/// Internal folders of the update itself are never program files.
fn is_update_internal(name: &str) -> bool {
    [DOWNLOAD_FOLDER, STAGING_FOLDER, OLD_FOLDER, SWAPPING_MARKER].iter().any(|internal| internal.eq_ignore_ascii_case(name))
}

/// The top-level items of a staged package that the swap installs, sorted: everything but `user\`, `data\` and the
/// update's own folders.
pub fn items_to_swap(fs: &dyn UpdateFs, staging: &Path) -> Result<Vec<String>, String> {
    let mut items: Vec<String> = fs
        .list(staging)
        .map_err(|error| format!("cannot read the staged update {}: {error}", staging.display()))?
        .into_iter()
        .filter(|name| !is_kept(name) && !is_update_internal(name))
        .collect();
    items.sort();
    Ok(items)
}

/// Swaps the staged program files into `exe_dir`. On any failure every step is undone (the previous files are back, the
/// staged ones are back in `staging`) and the error says what failed and whether the rollback worked.
pub fn swap_in(fs: &dyn UpdateFs, exe_dir: &Path, staging: &Path) -> Result<SwapReport, String> {
    let items = items_to_swap(fs, staging)?;
    if !items.iter().any(|name| name.eq_ignore_ascii_case(LAUNCHER_FILE)) {
        return Err(format!("the update package has no {LAUNCHER_FILE}; nothing was changed"));
    }
    let old = exe_dir.join(OLD_FOLDER);
    let marker = exe_dir.join(SWAPPING_MARKER);
    // A previous update that was not cleaned up (the launcher did not restart) is removed first.
    fs.remove_all(&old).map_err(|error| format!("cannot clear {}: {error}", old.display()))?;
    fs.create_dir_all(&old).map_err(|error| format!("cannot create {}: {error}", old.display()))?;
    fs.write_file(&marker, b"swapping\n").map_err(|error| format!("cannot write {}: {error}", marker.display()))?;

    let mut done: Vec<Step> = Vec::new();
    let mut failure: Option<String> = None;
    for name in &items {
        let live = exe_dir.join(name);
        let staged = staging.join(name);
        let aside = old.join(name);
        if fs.exists(&live) {
            match fs.rename(&live, &aside) {
                Ok(()) => done.push(Step::MovedAside(name.clone())),
                Err(error) => {
                    failure = Some(format!("{name} could not be moved aside ({error})"));
                    break;
                }
            }
        }
        match fs.rename(&staged, &live) {
            Ok(()) => done.push(Step::Placed(name.clone())),
            Err(error) => {
                failure = Some(format!("{name} could not be put in place ({error})"));
                break;
            }
        }
    }
    if let Some(reason) = failure {
        return match roll_back(fs, exe_dir, staging, &done) {
            Ok(()) => {
                let _ = fs.remove_all(&marker);
                let _ = fs.remove_all(&old);
                Err(format!("The update was not installed: {reason}. The previous version was restored."))
            }
            // The marker stays: the next start restores `.update-old` first.
            Err(rollback_error) => Err(format!(
                "The update was not installed: {reason}. Restoring the previous version failed too ({rollback_error}); \
                 it is restored at the next start."
            )),
        };
    }
    fs.remove_all(&marker).map_err(|error| format!("cannot remove {}: {error}", marker.display()))?;
    Ok(SwapReport { items })
}

/// Undoes `done` in reverse. Keeps going after an error and returns the first one.
fn roll_back(fs: &dyn UpdateFs, exe_dir: &Path, staging: &Path, done: &[Step]) -> Result<(), String> {
    let old = exe_dir.join(OLD_FOLDER);
    let mut first_error: Option<String> = None;
    for step in done.iter().rev() {
        let result = match step {
            Step::Placed(name) => fs.rename(&exe_dir.join(name), &staging.join(name)),
            Step::MovedAside(name) => fs.rename(&old.join(name), &exe_dir.join(name)),
        };
        if let Err(error) = result {
            first_error.get_or_insert(format!("{step:?}: {error}"));
        }
    }
    match first_error {
        None => Ok(()),
        Some(error) => Err(error),
    }
}

/// The whole install after the zip is downloaded and verified: stage, then swap. The staging folder is removed in every
/// case (the zip stays for the caller to remove).
pub fn install_package(fs: &dyn UpdateFs, exe_dir: &Path, zip: &Path) -> Result<SwapReport, String> {
    let staging = exe_dir.join(STAGING_FOLDER);
    fs.remove_all(&staging).map_err(|error| format!("cannot clear {}: {error}", staging.display()))?;
    if let Err(message) = fs.extract_zip(zip, &staging) {
        let _ = fs.remove_all(&staging);
        return Err(format!("The update package could not be unpacked: {message}"));
    }
    let result = swap_in(fs, exe_dir, &staging);
    let _ = fs.remove_all(&staging);
    result
}

/// What [`finish_previous_update`] did, as lines for the log.
pub type CleanupLog = Vec<String>;

/// At start: finishes or undoes the previous update. A `.update-swapping` marker means a swap was interrupted: every item
/// in `.update-old` goes back to its place (replacing the half-installed new one). Then `.update-old`, `.update-staging`
/// and `.update-download` are removed. Items that cannot be removed yet (the old launcher is still exiting) are left and
/// reported; the next start tries again.
pub fn finish_previous_update(fs: &dyn UpdateFs, exe_dir: &Path) -> CleanupLog {
    let mut log = CleanupLog::new();
    let old = exe_dir.join(OLD_FOLDER);
    let marker = exe_dir.join(SWAPPING_MARKER);
    if fs.exists(&marker) {
        log.push("an update was interrupted: restoring the previous program files".to_string());
        let mut restored_all = true;
        match fs.list(&old) {
            Ok(names) => {
                for name in names {
                    let live = exe_dir.join(&name);
                    let result = fs.remove_all(&live).and_then(|()| fs.rename(&old.join(&name), &live));
                    match result {
                        Ok(()) => log.push(format!("restored {name}")),
                        Err(error) => {
                            restored_all = false;
                            log.push(format!("could not restore {name}: {error}"));
                        }
                    }
                }
            }
            Err(error) => {
                restored_all = false;
                log.push(format!("cannot read {}: {error}", old.display()));
            }
        }
        if !restored_all {
            // The marker and `.update-old` stay for the next start.
            return log;
        }
        let _ = fs.remove_all(&marker);
    }
    for folder in [OLD_FOLDER, STAGING_FOLDER, DOWNLOAD_FOLDER] {
        let path = exe_dir.join(folder);
        if !fs.exists(&path) {
            continue;
        }
        match fs.remove_all(&path) {
            Ok(()) => log.push(format!("removed {folder}")),
            Err(error) => log.push(format!("{folder} could not be removed yet ({error}); it is tried again at the next start")),
        }
    }
    log
}

/// Whether the cleanup left something that a later try can remove.
pub fn cleanup_pending(fs: &dyn UpdateFs, exe_dir: &Path) -> bool {
    [OLD_FOLDER, STAGING_FOLDER, DOWNLOAD_FOLDER, SWAPPING_MARKER].iter().any(|name| fs.exists(&exe_dir.join(name)))
}

/// The release assets a portable update needs: the zip and the `SHA256SUMS` list. Errors name what is missing.
pub fn release_assets(release: &Release, version: impl Into<crate::version::ReleaseVersion>) -> Result<(&Asset, &Asset), String> {
    let package = Stream::Launcher.package_name_for(version, true);
    let zip = release.asset(&package).ok_or_else(|| format!("release {} has no {package}", release.tag))?;
    let sums = release
        .asset(CHECKSUM_ASSET)
        .ok_or_else(|| format!("release {} has no {CHECKSUM_ASSET}", release.tag))?;
    Ok((zip, sums))
}

/// Checks the downloaded zip against the release `SHA256SUMS` (`actual_sha256` was computed while it downloaded), then
/// installs it. A zip that does not match changes nothing.
pub fn verify_and_install(
    fs: &dyn UpdateFs,
    exe_dir: &Path,
    zip: &Path,
    package: &str,
    actual_sha256: &str,
    sums_text: &str,
) -> Result<SwapReport, String> {
    releases::check_sum(package, actual_sha256, sums_text)?;
    install_package(fs, exe_dir, zip)
}

/// The path of the downloaded zip.
pub fn download_path(exe_dir: &Path, package: &str) -> PathBuf {
    exe_dir.join(DOWNLOAD_FOLDER).join(package)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::BTreeMap;

    /// An in-memory file system. Paths are compared with `/` separators. `fail_rename_number` makes that
    /// rename call (counted from 1) fail, and `fail_all_renames_from` makes every call from that one on fail.
    #[derive(Default)]
    struct FakeFs {
        /// path -> is_dir
        nodes: RefCell<BTreeMap<String, bool>>,
        renames: RefCell<u32>,
        fail_rename_number: RefCell<Option<u32>>,
        fail_all_renames_from: RefCell<Option<u32>>,
        /// What an extraction creates: relative paths, a trailing `/` marks a folder.
        package: RefCell<Vec<String>>,
        extract_error: RefCell<Option<String>>,
    }

    fn key(path: &Path) -> String {
        path.to_string_lossy().replace('\\', "/").trim_end_matches('/').to_string()
    }

    impl FakeFs {
        fn add_parents(&self, path: &Path) {
            let mut nodes = self.nodes.borrow_mut();
            let mut current = key(path);
            while let Some(index) = current.rfind('/') {
                current.truncate(index);
                if current.is_empty() {
                    break;
                }
                nodes.entry(current.clone()).or_insert(true);
            }
        }

        fn add_file(&self, path: &Path) {
            self.nodes.borrow_mut().insert(key(path), false);
            self.add_parents(path);
        }

        fn add_dir(&self, path: &Path) {
            self.nodes.borrow_mut().insert(key(path), true);
            self.add_parents(path);
        }

        fn has(&self, path: &Path) -> bool {
            self.nodes.borrow().contains_key(&key(path))
        }

        fn snapshot_under(&self, path: &Path) -> Vec<String> {
            let prefix = key(path);
            self.nodes.borrow().keys().filter(|name| **name == prefix || name.starts_with(&format!("{prefix}/"))).cloned().collect()
        }
    }

    impl UpdateFs for FakeFs {
        fn exists(&self, path: &Path) -> bool {
            self.has(path)
        }

        fn list(&self, dir: &Path) -> io::Result<Vec<String>> {
            if !self.has(dir) {
                return Err(io::Error::new(io::ErrorKind::NotFound, "missing folder"));
            }
            let prefix = format!("{}/", key(dir));
            let names: Vec<String> = self
                .nodes
                .borrow()
                .keys()
                .filter_map(|name| name.strip_prefix(&prefix))
                .filter(|rest| !rest.contains('/'))
                .map(str::to_string)
                .collect();
            Ok(names)
        }

        fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
            let number = {
                let mut renames = self.renames.borrow_mut();
                *renames += 1;
                *renames
            };
            if *self.fail_rename_number.borrow() == Some(number) || self.fail_all_renames_from.borrow().is_some_and(|first| number >= first) {
                return Err(io::Error::new(io::ErrorKind::PermissionDenied, "injected rename failure"));
            }
            if !self.has(from) {
                return Err(io::Error::new(io::ErrorKind::NotFound, "rename source missing"));
            }
            if self.has(to) {
                return Err(io::Error::new(io::ErrorKind::AlreadyExists, "rename target exists"));
            }
            let source = key(from);
            let target = key(to);
            let moved: Vec<(String, bool)> = self
                .nodes
                .borrow()
                .iter()
                .filter(|(name, _)| **name == source || name.starts_with(&format!("{source}/")))
                .map(|(name, is_dir)| (name.clone(), *is_dir))
                .collect();
            {
                let mut nodes = self.nodes.borrow_mut();
                for (name, _) in &moved {
                    nodes.remove(name);
                }
                for (name, is_dir) in moved {
                    nodes.insert(format!("{target}{}", &name[source.len()..]), is_dir);
                }
            }
            self.add_parents(to);
            Ok(())
        }

        fn create_dir_all(&self, path: &Path) -> io::Result<()> {
            self.add_dir(path);
            Ok(())
        }

        fn remove_all(&self, path: &Path) -> io::Result<()> {
            let prefix = key(path);
            self.nodes.borrow_mut().retain(|name, _| !(*name == prefix || name.starts_with(&format!("{prefix}/"))));
            Ok(())
        }

        fn write_file(&self, path: &Path, _bytes: &[u8]) -> io::Result<()> {
            self.add_file(path);
            Ok(())
        }

        fn extract_zip(&self, _zip: &Path, dest: &Path) -> Result<(), String> {
            if let Some(error) = self.extract_error.borrow().clone() {
                return Err(error);
            }
            self.add_dir(dest);
            for entry in self.package.borrow().iter() {
                if let Some(folder) = entry.strip_suffix('/') {
                    self.add_dir(&dest.join(folder));
                } else {
                    self.add_file(&dest.join(entry));
                }
            }
            Ok(())
        }
    }

    fn folder() -> PathBuf {
        PathBuf::from("E:/Portable/GT4Recomp")
    }

    /// A portable folder as 1.3.0 left it: program files, `game\`, and the player's folders.
    fn installed_folder(fs: &FakeFs) {
        let root = folder();
        for file in [
            "GT4Recomp.exe",
            "SDL3.dll",
            "LICENSE.txt",
            "version.json",
            "game/gt4.exe",
            "game/shaders/raster.spv",
            "user/config/settings.toml",
            "data/games/gt4/disc/game.iso",
        ] {
            fs.add_file(&root.join(file));
        }
    }

    fn new_package(fs: &FakeFs) {
        *fs.package.borrow_mut() = ["GT4Recomp.exe", "SDL3.dll", "NOTICE.md", "game/gt4.exe", "game/gt4-spec2.exe", "user/", "data/"]
            .iter()
            .map(|name| name.to_string())
            .collect();
    }

    #[test]
    fn the_swap_replaces_program_files_and_never_touches_user_or_data() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        new_package(&fs);
        let user_before = fs.snapshot_under(&folder().join("user"));
        let data_before = fs.snapshot_under(&folder().join("data"));
        let report = install_package(&fs, &folder(), Path::new("package.zip")).unwrap();
        assert_eq!(report.items, ["GT4Recomp.exe", "NOTICE.md", "SDL3.dll", "game"]);
        let root = folder();
        assert!(fs.has(&root.join("NOTICE.md")), "a new file arrives");
        assert!(fs.has(&root.join("game/gt4-spec2.exe")), "the new game folder is in place");
        assert!(!fs.has(&root.join("game/shaders/raster.spv")), "the old game folder is gone with its files");
        assert!(fs.has(&root.join(".update-old/GT4Recomp.exe")), "the old launcher waits for the next start");
        assert!(fs.has(&root.join(".update-old/game/shaders/raster.spv")));
        assert!(!fs.has(&root.join(SWAPPING_MARKER)), "the marker goes when the swap is complete");
        assert!(!fs.has(&root.join(STAGING_FOLDER)), "the staging folder is removed");
        assert_eq!(fs.snapshot_under(&root.join("user")), user_before, "user is untouched");
        assert_eq!(fs.snapshot_under(&root.join("data")), data_before, "data is untouched");
        assert!(fs.has(&root.join("version.json")), "files outside the package stay");
    }

    #[test]
    fn the_next_start_removes_the_old_copy() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        new_package(&fs);
        install_package(&fs, &folder(), Path::new("package.zip")).unwrap();
        assert!(cleanup_pending(&fs, &folder()));
        let log = finish_previous_update(&fs, &folder());
        assert!(log.iter().any(|line| line.contains(OLD_FOLDER)), "{log:?}");
        assert!(!cleanup_pending(&fs, &folder()));
        assert!(fs.has(&folder().join("GT4Recomp.exe")), "the new launcher stays");
        assert!(fs.has(&folder().join("user/config/settings.toml")));
    }

    #[test]
    fn a_failed_rename_at_any_step_rolls_everything_back() {
        // Four items; a replaced item needs two renames (aside, place) and the new one needs one.
        let probe = FakeFs::default();
        installed_folder(&probe);
        new_package(&probe);
        install_package(&probe, &folder(), Path::new("package.zip")).unwrap();
        let total = *probe.renames.borrow();
        assert_eq!(total, 7);
        for failing in 1..=total {
            let fs = FakeFs::default();
            installed_folder(&fs);
            new_package(&fs);
            let before = fs.snapshot_under(&folder());
            *fs.fail_rename_number.borrow_mut() = Some(failing);
            let error = install_package(&fs, &folder(), Path::new("package.zip")).unwrap_err();
            assert!(error.contains("previous version was restored"), "step {failing}: {error}");
            assert_eq!(fs.snapshot_under(&folder()), before, "step {failing}: the folder is exactly as it was");
        }
    }

    #[test]
    fn a_failed_rollback_keeps_the_marker_and_the_next_start_restores() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        new_package(&fs);
        // The seventh rename (the new game folder into place) fails and then every rename of the rollback fails as well.
        *fs.fail_all_renames_from.borrow_mut() = Some(7);
        let error = install_package(&fs, &folder(), Path::new("package.zip")).unwrap_err();
        assert!(error.contains("Restoring the previous version failed"), "{error}");
        assert!(fs.has(&folder().join(SWAPPING_MARKER)), "the marker stays");
        // The next start (renames work again) puts the old files back and clears the leftovers.
        *fs.fail_all_renames_from.borrow_mut() = None;
        let log = finish_previous_update(&fs, &folder());
        assert!(log.iter().any(|line| line.contains("interrupted")), "{log:?}");
        assert!(!fs.has(&folder().join(SWAPPING_MARKER)));
        assert!(!fs.has(&folder().join(OLD_FOLDER)));
        for file in ["GT4Recomp.exe", "SDL3.dll", "LICENSE.txt", "game/gt4.exe", "game/shaders/raster.spv", "user/config/settings.toml"] {
            assert!(fs.has(&folder().join(file)), "{file} is back");
        }
        assert!(!fs.has(&folder().join("game/gt4-spec2.exe")), "the half-installed new game folder is gone");
    }

    #[test]
    fn a_package_without_the_launcher_changes_nothing() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        *fs.package.borrow_mut() = vec!["game/gt4.exe".to_string()];
        let before = fs.snapshot_under(&folder());
        let error = install_package(&fs, &folder(), Path::new("package.zip")).unwrap_err();
        assert!(error.contains("no GT4Recomp.exe"), "{error}");
        assert_eq!(fs.snapshot_under(&folder()), before);
    }

    #[test]
    fn an_unpack_failure_leaves_the_folder_as_it_was() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        *fs.extract_error.borrow_mut() = Some("disk full".to_string());
        let before = fs.snapshot_under(&folder());
        let error = install_package(&fs, &folder(), Path::new("package.zip")).unwrap_err();
        assert!(error.contains("disk full"), "{error}");
        assert_eq!(fs.snapshot_under(&folder()), before, "no staging folder is left");
    }

    #[test]
    fn a_leftover_old_folder_that_cannot_go_yet_is_reported_and_kept() {
        struct LockedOldExe(FakeFs);
        impl UpdateFs for LockedOldExe {
            fn exists(&self, path: &Path) -> bool {
                self.0.exists(path)
            }
            fn list(&self, dir: &Path) -> io::Result<Vec<String>> {
                self.0.list(dir)
            }
            fn rename(&self, from: &Path, to: &Path) -> io::Result<()> {
                self.0.rename(from, to)
            }
            fn create_dir_all(&self, path: &Path) -> io::Result<()> {
                self.0.create_dir_all(path)
            }
            fn remove_all(&self, path: &Path) -> io::Result<()> {
                if key(path).ends_with(OLD_FOLDER) {
                    return Err(io::Error::new(io::ErrorKind::PermissionDenied, "the old launcher is still running"));
                }
                self.0.remove_all(path)
            }
            fn write_file(&self, path: &Path, bytes: &[u8]) -> io::Result<()> {
                self.0.write_file(path, bytes)
            }
            fn extract_zip(&self, zip: &Path, dest: &Path) -> Result<(), String> {
                self.0.extract_zip(zip, dest)
            }
        }
        let fs = LockedOldExe(FakeFs::default());
        installed_folder(&fs.0);
        fs.0.add_file(&folder().join(OLD_FOLDER).join("GT4Recomp.exe"));
        let log = finish_previous_update(&fs, &folder());
        assert!(log.iter().any(|line| line.contains("tried again at the next start")), "{log:?}");
        assert!(cleanup_pending(&fs, &folder()));
    }

    #[test]
    fn a_zip_that_does_not_match_its_checksum_changes_nothing() {
        let fs = FakeFs::default();
        installed_folder(&fs);
        new_package(&fs);
        let before = fs.snapshot_under(&folder());
        let name = "GT4Recomp-1.3.0-portable.zip";
        let sums = format!("{}  {name}\n", "a".repeat(64));
        let error = verify_and_install(&fs, &folder(), Path::new(name), name, &"b".repeat(64), &sums).unwrap_err();
        assert!(error.contains("does not match its SHA-256"), "{error}");
        assert_eq!(fs.snapshot_under(&folder()), before, "nothing was extracted or swapped");
        let missing = verify_and_install(&fs, &folder(), Path::new(name), name, &"a".repeat(64), "").unwrap_err();
        assert!(missing.contains("has no line"), "{missing}");
        let report = verify_and_install(&fs, &folder(), Path::new(name), name, &"a".repeat(64), &sums).unwrap();
        assert!(report.items.contains(&"GT4Recomp.exe".to_string()));
    }

    #[test]
    fn the_release_must_carry_the_portable_zip_and_the_checksum_list() {
        let json = r#"[{"tag_name":"v1.3.0","prerelease":false,"draft":false,"body":"","assets":[
            {"name":"GT4Recomp-Setup-1.3.0.exe","browser_download_url":"https://example.invalid/setup"},
            {"name":"GT4Recomp-1.3.0-portable.zip","browser_download_url":"https://example.invalid/zip"},
            {"name":"SHA256SUMS","browser_download_url":"https://example.invalid/sums"}]},
            {"tag_name":"v1.2.9","prerelease":false,"draft":false,"body":"","assets":[
            {"name":"GT4Recomp-Setup-1.2.9.exe","browser_download_url":"https://example.invalid/old"},
            {"name":"SHA256SUMS","browser_download_url":"https://example.invalid/oldsums"}]}]"#;
        let list = releases::parse_releases(json, Stream::Launcher).unwrap();
        let (zip, sums) = release_assets(&list[0], Version(1, 3, 0)).unwrap();
        assert_eq!(zip.name, "GT4Recomp-1.3.0-portable.zip");
        assert_eq!(sums.name, "SHA256SUMS");
        let error = release_assets(&list[1], Version(1, 2, 9)).unwrap_err();
        assert!(error.contains("GT4Recomp-1.2.9-portable.zip"), "{error}");
    }

    #[test]
    fn the_download_path_is_inside_the_portable_folder() {
        assert_eq!(
            download_path(&folder(), "GT4Recomp-1.3.0-portable.zip"),
            folder().join(".update-download").join("GT4Recomp-1.3.0-portable.zip")
        );
    }

    // ---- the real file system and a real zip ----

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-portable-update-{}-{name}", std::process::id()));
        let _ = std::fs::remove_dir_all(&path);
        std::fs::create_dir_all(&path).unwrap();
        path
    }

    fn write_zip(path: &Path, entries: &[(&str, &[u8])]) {
        use std::io::Write;
        let mut writer = zip::ZipWriter::new(std::fs::File::create(path).unwrap());
        let options = zip::write::SimpleFileOptions::default();
        for (name, bytes) in entries {
            if name.ends_with('/') {
                writer.add_directory(*name, options).unwrap();
            } else {
                writer.start_file(*name, options).unwrap();
                writer.write_all(bytes).unwrap();
            }
        }
        writer.finish().unwrap();
    }

    #[test]
    fn a_real_zip_is_unpacked_and_swapped_on_the_real_file_system() {
        let base = scratch("real");
        let exe_dir = base.join("GT4Recomp");
        std::fs::create_dir_all(exe_dir.join("game")).unwrap();
        std::fs::create_dir_all(exe_dir.join("user")).unwrap();
        std::fs::create_dir_all(exe_dir.join("data")).unwrap();
        std::fs::write(exe_dir.join("GT4Recomp.exe"), b"old launcher").unwrap();
        std::fs::write(exe_dir.join("game").join("gt4.exe"), b"old game").unwrap();
        std::fs::write(exe_dir.join("user").join("settings.toml"), b"mine").unwrap();
        std::fs::write(exe_dir.join("data").join("disc.iso"), b"disc").unwrap();
        let zip = base.join("GT4Recomp-1.3.0-portable.zip");
        write_zip(
            &zip,
            &[
                ("GT4Recomp.exe", b"new launcher"),
                ("game/gt4.exe", b"new game"),
                ("game/shaders/raster.spv", b"spv"),
                ("user/", b""),
                ("data/", b""),
            ],
        );
        let report = install_package(&RealFs, &exe_dir, &zip).unwrap();
        assert_eq!(report.items, ["GT4Recomp.exe", "game"]);
        assert_eq!(std::fs::read(exe_dir.join("GT4Recomp.exe")).unwrap(), b"new launcher");
        assert_eq!(std::fs::read(exe_dir.join("game").join("shaders").join("raster.spv")).unwrap(), b"spv");
        assert_eq!(std::fs::read(exe_dir.join(OLD_FOLDER).join("GT4Recomp.exe")).unwrap(), b"old launcher");
        assert_eq!(std::fs::read(exe_dir.join("user").join("settings.toml")).unwrap(), b"mine");
        assert_eq!(std::fs::read(exe_dir.join("data").join("disc.iso")).unwrap(), b"disc");
        assert!(!exe_dir.join(STAGING_FOLDER).exists());
        let log = finish_previous_update(&RealFs, &exe_dir);
        assert!(!exe_dir.join(OLD_FOLDER).exists(), "{log:?}");
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn a_zip_entry_that_leaves_the_folder_is_refused() {
        let base = scratch("zip-slip");
        let zip = base.join("bad.zip");
        write_zip(&zip, &[("GT4Recomp.exe", b"x"), ("../outside.txt", b"escaped")]);
        let error = extract_zip_safely(&zip, &base.join("staging")).unwrap_err();
        assert!(error.contains("unsafe entry name"), "{error}");
        assert!(!base.join("outside.txt").exists());
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn a_file_that_is_not_a_zip_is_refused() {
        let base = scratch("not-zip");
        std::fs::write(base.join("x.zip"), b"not a zip").unwrap();
        assert!(extract_zip_safely(&base.join("x.zip"), &base.join("staging")).unwrap_err().contains("not a zip"));
        let _ = std::fs::remove_dir_all(&base);
    }
}
