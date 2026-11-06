//! Moving a file or a folder to another place that may be on a different volume (TASK-188, issue #208).
//!
//! The launcher keeps the large data (`<install>\data`: the disc copy, the prepared files, the gt4-gs versions and the
//! runtime card) beside the executable and the save library (`Saves`, `Replays`, the card backups) in
//! `%APPDATA%\GT4Recomp` (see [`crate::paths`]). Those two places are on the same drive in a normal install, but not in
//! every supported layout: a player with the game on `F:` and Windows on `C:` has the runtime card on `F:` and the save
//! library on `C:`. [`std::fs::rename`] cannot move between volumes: it is a `MoveFileEx` without a copy on Windows and
//! fails with `ERROR_NOT_SAME_DEVICE` (os error 17), a `rename(2)` with `EXDEV` (os error 18) on Unix.
//!
//! [`move_path`] keeps the atomic rename (and its replace-the-target behaviour) whenever the two paths are on one
//! volume, so the guarantees the save library relies on stay: a card written through a temporary file appears whole or
//! not at all. Across volumes it copies, checks every file (size and SHA-256), places the copy, and only then removes the
//! source. A move that cannot be checked leaves the source where it was, so nothing is ever lost.
//!
//! Tests on one volume give [`MoveOps`] a rename that reports another volume (`from_raw_os_error(17)`), which runs the
//! copy path exactly as a player's two drives do.

use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

/// `ERROR_NOT_SAME_DEVICE`: a rename between two volumes (Windows).
pub const ERROR_NOT_SAME_DEVICE: i32 = 17;
/// `EXDEV`: a rename between two file systems (Unix).
pub const EXDEV: i32 = 18;

/// Whether a rename failed only because the two places are on different volumes, so a copy is allowed.
pub fn is_cross_device(error: &io::Error) -> bool {
    matches!(error.raw_os_error(), Some(code) if code == ERROR_NOT_SAME_DEVICE || code == EXDEV)
}

/// The file operations a move uses, so a test can make the rename report another volume or a copy come out wrong.
pub struct MoveOps<'a> {
    pub rename: &'a dyn Fn(&Path, &Path) -> io::Result<()>,
    /// Copies one file; returns the bytes written. The move checks the result itself.
    pub copy_file: &'a dyn Fn(&Path, &Path) -> io::Result<u64>,
}

impl MoveOps<'static> {
    /// The operations of the launcher.
    pub fn real() -> MoveOps<'static> {
        MoveOps { rename: &real_rename, copy_file: &real_copy_file }
    }
}

/// A rename that retries the transient failures a scanner, the indexer or Explorer can cause on a freshly written file
/// (sharing/lock violation, access denied while a scan holds it). A real failure is reported to the caller.
fn real_rename(from: &Path, to: &Path) -> io::Result<()> {
    let mut last = None;
    for attempt in 0..5u32 {
        match fs::rename(from, to) {
            Ok(()) => return Ok(()),
            Err(error) if matches!(error.raw_os_error(), Some(5) | Some(32) | Some(33)) => {
                last = Some(error);
                std::thread::sleep(std::time::Duration::from_millis(20 * u64::from(attempt + 1)));
            }
            Err(error) => return Err(error),
        }
    }
    Err(last.expect("a retryable rename error"))
}

fn real_copy_file(from: &Path, to: &Path) -> io::Result<u64> {
    fs::copy(from, to)
}

/// Moves `from` (a file or a folder) to `to`. See the module documentation.
pub fn move_path(from: &Path, to: &Path) -> io::Result<()> {
    move_path_with(from, to, &MoveOps::real())
}

/// [`move_path`] with the file operations given, so a test can report another volume or a bad copy.
pub fn move_path_with(from: &Path, to: &Path, ops: &MoveOps<'_>) -> io::Result<()> {
    if !from.exists() && fs::symlink_metadata(from).is_err() {
        return Err(io::Error::new(io::ErrorKind::NotFound, format!("{} is not there", from.display())));
    }
    // The destination's folder exists first, so a missing folder is not mistaken for a failure of the move itself.
    if let Some(parent) = to.parent().filter(|parent| !parent.as_os_str().is_empty()) {
        fs::create_dir_all(parent)?;
    }
    // One volume: the rename, which replaces the target and leaves no half-written file.
    match (ops.rename)(from, to) {
        Ok(()) => return Ok(()),
        Err(error) if is_cross_device(&error) => {}
        Err(error) => return Err(error),
    }
    // Two volumes: copy, check every file, place the copy, and only then remove the source.
    copy_then_remove(from, to, ops)
}

/// The cross-volume half of [`move_path`]: a checked copy is placed and the source removed after it. A failure removes
/// only the half-made copy; the source stays where it was.
fn copy_then_remove(from: &Path, to: &Path, ops: &MoveOps<'_>) -> io::Result<()> {
    let metadata = fs::symlink_metadata(from)?;
    if metadata.is_dir() {
        move_folder_across(from, to, ops)?;
    } else if metadata.is_file() {
        move_file_across(from, to, ops)?;
    } else {
        return Err(io::Error::other(format!("{} is neither a file nor a folder", from.display())));
    }
    remove_source(from)
}

/// A file: copied to a temporary name beside the destination, checked, renamed into place, then the source removed. The
/// final rename is on the destination's own volume, so the destination is replaced whole (as a same-volume rename does).
fn move_file_across(from: &Path, to: &Path, ops: &MoveOps<'_>) -> io::Result<()> {
    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = temporary_beside(to);
    remove_any(&temporary);
    let checked = (|| -> io::Result<()> {
        (ops.copy_file)(from, &temporary)?;
        check_copy(from, &temporary)?;
        keep_modified_time(from, &temporary);
        (ops.rename)(&temporary, to)
    })();
    match checked {
        Ok(()) => Ok(()),
        Err(error) => {
            remove_any(&temporary);
            Err(error)
        }
    }
}

/// A folder: copied into a staging folder beside the destination, every file checked, renamed into place, then the
/// source removed. Empty folders are kept, so the move is faithful; an entry that is neither a file nor a folder (a
/// link, say) is refused rather than silently lost.
fn move_folder_across(from: &Path, to: &Path, ops: &MoveOps<'_>) -> io::Result<()> {
    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent)?;
    }
    let staging = temporary_beside(to);
    remove_any(&staging);
    let checked = (|| -> io::Result<()> {
        copy_tree(from, &staging, ops)?;
        check_tree(from, &staging)?;
        (ops.rename)(&staging, to)
    })();
    match checked {
        Ok(()) => Ok(()),
        Err(error) => {
            remove_any(&staging);
            Err(error)
        }
    }
}

/// `<destination>.<pid>.moving` in the destination's folder: a name no other run picks, on the destination's volume.
fn temporary_beside(destination: &Path) -> PathBuf {
    let name = destination.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_else(|| "moved".into());
    destination.with_file_name(format!(".{name}.{}.moving", std::process::id()))
}

fn copy_tree(from: &Path, to: &Path, ops: &MoveOps<'_>) -> io::Result<()> {
    fs::create_dir_all(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let path = entry.path();
        let target = to.join(entry.file_name());
        let file_type = entry.file_type()?;
        if file_type.is_dir() {
            copy_tree(&path, &target, ops)?;
        } else if file_type.is_file() {
            (ops.copy_file)(&path, &target)?;
            keep_modified_time(&path, &target);
        } else {
            return Err(io::Error::other(format!("{} is a link or a special file, which cannot be moved", path.display())));
        }
    }
    Ok(())
}

/// Size and SHA-256 (lower-case hex) of a file.
fn digest(path: &Path) -> io::Result<(u64, String)> {
    let mut file = fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0u8; 1 << 16];
    let mut size = 0u64;
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
        size += read as u64;
    }
    Ok((size, hasher.finalize().iter().map(|byte| format!("{byte:02x}")).collect()))
}

/// Fails when the copy is not the source: another size, or the same size with different bytes.
fn check_copy(source: &Path, copy: &Path) -> io::Result<()> {
    let (source_size, source_hash) = digest(source)?;
    let (copy_size, copy_hash) = digest(copy)?;
    if source_size != copy_size || source_hash != copy_hash {
        return Err(io::Error::other(format!(
            "{} does not match its source after the copy ({} bytes, sha256 {source_hash} against {} bytes, sha256 {copy_hash})",
            copy.display(),
            copy_size,
            source_size
        )));
    }
    Ok(())
}

/// Fails when the copy holds a different tree from the source: a missing or extra file, or a file whose bytes differ.
fn check_tree(source: &Path, copy: &Path) -> io::Result<()> {
    let source_files = files_under(source)?;
    let copy_files = files_under(copy)?;
    if source_files != copy_files {
        return Err(io::Error::other(format!(
            "{} does not hold the same files as {} after the copy",
            copy.display(),
            source.display()
        )));
    }
    for relative in source_files {
        check_copy(&source.join(&relative), &copy.join(&relative))?;
    }
    Ok(())
}

/// Every file under `folder`, as paths relative to it, sorted. Empty folders are not listed (the copy makes them).
fn files_under(folder: &Path) -> io::Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    let mut stack = vec![folder.to_path_buf()];
    while let Some(current) = stack.pop() {
        for entry in fs::read_dir(&current)? {
            let entry = entry?;
            let path = entry.path();
            if entry.file_type()?.is_dir() {
                stack.push(path);
            } else {
                files.push(path.strip_prefix(folder).unwrap_or(&path).to_path_buf());
            }
        }
    }
    files.sort();
    Ok(files)
}

/// Keeps the source's last-change time on the copy, so a moved card or save keeps the date the game gave it. A file
/// system that cannot set it is not a reason to fail a move that is already checked.
fn keep_modified_time(source: &Path, copy: &Path) {
    let Ok(modified) = fs::metadata(source).and_then(|metadata| metadata.modified()) else { return };
    let _ = fs::OpenOptions::new().write(true).open(copy).and_then(|handle| handle.set_modified(modified));
}

/// Removes the source of a finished move: the folder tree for a folder, the file otherwise.
fn remove_source(from: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(from)?;
    if metadata.is_dir() {
        fs::remove_dir_all(from)
    } else {
        fs::remove_file(from)
    }
}

/// Removes a leftover staging copy, if there is one; a name that cannot be removed is left for the next run.
fn remove_any(path: &Path) {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.is_dir() => {
            let _ = fs::remove_dir_all(path);
        }
        Ok(_) => {
            let _ = fs::remove_file(path);
        }
        Err(_) => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-moves-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    /// A rename that reports another volume for the source, the way a card on `F:` moved into a library on `C:` does, and
    /// works for the staging copy, which is made beside the destination (on its own volume).
    fn other_volume(from: &Path, to: &Path) -> io::Result<()> {
        let staging = from.file_name().is_some_and(|name| name.to_string_lossy().contains(".moving"));
        if staging {
            fs::rename(from, to)
        } else {
            Err(io::Error::from_raw_os_error(ERROR_NOT_SAME_DEVICE))
        }
    }

    fn copy_file(from: &Path, to: &Path) -> io::Result<u64> {
        fs::copy(from, to)
    }

    fn same_volume(from: &Path, to: &Path) -> io::Result<()> {
        fs::rename(from, to)
    }

    fn cross_device_ops<'a>(copy: &'a dyn Fn(&Path, &Path) -> io::Result<u64>) -> MoveOps<'a> {
        MoveOps { rename: &other_volume, copy_file: copy }
    }

    #[test]
    fn a_cross_device_error_is_recognised_and_other_errors_are_not() {
        assert!(is_cross_device(&io::Error::from_raw_os_error(ERROR_NOT_SAME_DEVICE)));
        assert!(is_cross_device(&io::Error::from_raw_os_error(EXDEV)));
        assert!(!is_cross_device(&io::Error::from_raw_os_error(2)), "a missing file is not a volume");
        assert!(!is_cross_device(&io::Error::other("no os code")));
    }

    #[test]
    fn a_move_on_one_volume_renames_and_never_copies() {
        let base = scratch("same-volume");
        let from = base.join("card.ps2");
        fs::write(&from, b"a card").unwrap();
        let to = base.join("library").join("card.ps2");
        fs::create_dir_all(to.parent().unwrap()).unwrap();
        let copy = |_: &Path, _: &Path| -> io::Result<u64> { panic!("a same-volume move must not copy") };
        move_path_with(&from, &to, &MoveOps { rename: &same_volume, copy_file: &copy }).unwrap();
        assert_eq!(fs::read(&to).unwrap(), b"a card");
        assert!(!from.exists());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_file_across_volumes_is_copied_checked_and_the_source_removed() {
        let base = scratch("file");
        let from = base.join("install").join("runtime").join("card.ps2");
        fs::create_dir_all(from.parent().unwrap()).unwrap();
        fs::write(&from, vec![7u8; 40_000]).unwrap();
        let to = base.join("appdata").join("backups").join("20261106-120000-before-delete.ps2");

        move_path_with(&from, &to, &cross_device_ops(&copy_file)).unwrap();

        assert_eq!(fs::read(&to).unwrap(), vec![7u8; 40_000], "every byte arrived");
        assert!(!from.exists(), "the source is gone, so the card cannot be harvested again");
        let leftovers: Vec<String> = fs::read_dir(to.parent().unwrap())
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .filter(|name| name != "20261106-120000-before-delete.ps2")
            .collect();
        assert!(leftovers.is_empty(), "no staging file is left: {leftovers:?}");
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_folder_across_volumes_keeps_every_file_and_empty_folder() {
        let base = scratch("folder");
        let from = base.join("documents").join("Saves").join("Active");
        fs::create_dir_all(from.join("empty-because-the-game-made-it")).unwrap();
        fs::create_dir_all(from.join("BASCUS-97328GAMEDATA").join("deep")).unwrap();
        fs::write(from.join("BASCUS-97328GAMEDATA").join("garage"), vec![3u8; 5_000]).unwrap();
        fs::write(from.join("BASCUS-97328GAMEDATA").join("deep").join("icon.sys"), b"icon").unwrap();
        let to = base.join("install").join("data").join("Saves").join("Active");

        move_path_with(&from, &to, &cross_device_ops(&copy_file)).unwrap();

        assert_eq!(fs::read(to.join("BASCUS-97328GAMEDATA").join("garage")).unwrap(), vec![3u8; 5_000]);
        assert_eq!(fs::read(to.join("BASCUS-97328GAMEDATA").join("deep").join("icon.sys")).unwrap(), b"icon");
        assert!(to.join("empty-because-the-game-made-it").is_dir(), "an empty folder is kept");
        assert!(!from.exists(), "the source folder is gone");
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_copy_that_does_not_match_its_source_leaves_the_source_and_removes_the_copy() {
        let base = scratch("bad-copy");
        let from = base.join("install").join("card.ps2");
        fs::create_dir_all(from.parent().unwrap()).unwrap();
        fs::write(&from, vec![9u8; 2_000]).unwrap();
        let to = base.join("appdata").join("card-backup.ps2");
        // A copy that stops short, as a full disk would.
        let truncated = |from: &Path, to: &Path| -> io::Result<u64> {
            let bytes = fs::read(from)?;
            fs::write(to, &bytes[..bytes.len() / 2])?;
            Ok((bytes.len() / 2) as u64)
        };

        let error = move_path_with(&from, &to, &cross_device_ops(&truncated)).unwrap_err();

        assert!(error.to_string().contains("does not match its source"), "{error}");
        assert_eq!(fs::read(&from).unwrap(), vec![9u8; 2_000], "the source keeps its bytes");
        assert!(!to.exists(), "no half card is placed");
        let names: Vec<String> =
            fs::read_dir(to.parent().unwrap()).unwrap().map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned()).collect();
        assert!(names.is_empty(), "the staging copy is removed: {names:?}");
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_file_moved_across_volumes_replaces_the_one_already_there() {
        let base = scratch("replace");
        let from = base.join("install").join("card.ps2");
        fs::create_dir_all(from.parent().unwrap()).unwrap();
        fs::write(&from, b"new card").unwrap();
        let to = base.join("appdata").join("card.ps2");
        fs::create_dir_all(to.parent().unwrap()).unwrap();
        fs::write(&to, b"old card").unwrap();

        move_path_with(&from, &to, &cross_device_ops(&copy_file)).unwrap();

        assert_eq!(fs::read(&to).unwrap(), b"new card");
        assert!(!from.exists());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_file_across_volumes_is_placed_into_a_folder_that_does_not_exist_yet() {
        let base = scratch("new-folder");
        let from = base.join("install").join("card.ps2");
        fs::create_dir_all(from.parent().unwrap()).unwrap();
        fs::write(&from, b"a card").unwrap();
        let to = base.join("appdata").join("games").join("gt4").join("backups").join("card.ps2");

        move_path_with(&from, &to, &cross_device_ops(&copy_file)).unwrap();

        assert_eq!(fs::read(&to).unwrap(), b"a card", "the destination folders are made first");
        assert!(!from.exists());
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn a_missing_source_is_reported_and_nothing_is_written() {
        let base = scratch("missing");
        let error = move_path(&base.join("not-there.ps2"), &base.join("target.ps2")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert!(!base.join("target.ps2").exists());
        let _ = fs::remove_dir_all(&base);
    }
}
