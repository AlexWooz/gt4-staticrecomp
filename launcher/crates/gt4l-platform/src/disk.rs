//! Free space of a volume, and the path of a disc image as the game can open it.

use std::path::{Path, PathBuf};

/// The path the game can open a file by. The game takes its arguments in the system's ANSI code page: accented names such as
/// `C:\\Users\\João` fit it on a Western Windows and go as they are (owner, 2026-11-02). A name outside it would reach the game as
/// `???`, so it goes by its short (8.3) name. `None`: it does not fit and Windows keeps no short name, so the game cannot be given this file.
pub fn game_readable_path(path: &Path) -> Option<PathBuf> {
    if fits_ansi_code_page(path) {
        return Some(path.to_path_buf());
    }
    short_path_of(path).filter(|short| fits_ansi_code_page(short))
}

/// Whether every character of the path survives the system's ANSI code page unchanged.
#[cfg(windows)]
fn fits_ansi_code_page(path: &Path) -> bool {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::{BOOL, PCSTR};
    use windows::Win32::Globalization::{GetACP, WideCharToMultiByte, CP_ACP, CP_UTF8, WC_NO_BEST_FIT_CHARS};
    let wide: Vec<u16> = path.as_os_str().encode_wide().collect();
    if wide.is_empty() || wide.iter().all(|unit| *unit < 128) {
        return true;
    }
    // SAFETY: no pointers are involved in GetACP.
    if unsafe { GetACP() } == CP_UTF8 {
        return true;
    }
    let mut narrow = vec![0u8; wide.len() * 4];
    let mut lost = BOOL(0);
    // SAFETY: both slices are valid for the call, and `lost` is a local the call writes to.
    let written = unsafe { WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, &wide, Some(&mut narrow), PCSTR::null(), Some(&mut lost)) };
    written > 0 && !lost.as_bool()
}

#[cfg(not(windows))]
fn fits_ansi_code_page(path: &Path) -> bool {
    path.to_string_lossy().is_ascii()
}

#[cfg(windows)]
fn short_path_of(path: &Path) -> Option<PathBuf> {
    use std::ffi::OsString;
    use std::os::windows::ffi::{OsStrExt, OsStringExt};
    use windows::core::PCWSTR;
    use windows::Win32::Storage::FileSystem::GetShortPathNameW;
    let wide: Vec<u16> = path.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
    // SAFETY: `wide` is a NUL-terminated UTF-16 path that outlives both calls; the buffer is as long as the call is told.
    let needed = unsafe { GetShortPathNameW(PCWSTR(wide.as_ptr()), None) } as usize;
    if needed == 0 {
        return None;
    }
    let mut buffer = vec![0u16; needed];
    let written = unsafe { GetShortPathNameW(PCWSTR(wide.as_ptr()), Some(&mut buffer)) } as usize;
    (written > 0 && written < needed).then(|| PathBuf::from(OsString::from_wide(&buffer[..written])))
}

#[cfg(not(windows))]
fn short_path_of(_path: &Path) -> Option<PathBuf> {
    None
}

/// Free bytes available to the user on the volume that holds `folder` (or its nearest existing parent).
/// `None` when it cannot be read; setup then copies and reports a full disk as an error if it happens.
pub fn free_space(folder: &Path) -> Option<u64> {
    let existing = folder.ancestors().find(|candidate| candidate.exists())?;
    free_space_of(existing)
}

#[cfg(windows)]
fn free_space_of(existing: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::PCWSTR;
    use windows::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
    let wide: Vec<u16> = existing.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
    let mut available = 0u64;
    // SAFETY: `wide` is a NUL-terminated UTF-16 path that outlives the call; the out pointer is a local u64.
    unsafe { GetDiskFreeSpaceExW(PCWSTR(wide.as_ptr()), Some(&mut available), None, None) }.ok()?;
    Some(available)
}

#[cfg(not(windows))]
fn free_space_of(_existing: &Path) -> Option<u64> {
    None
}

#[cfg(test)]
mod tests {
    #[test]
    fn a_plain_path_is_left_as_it_is_and_another_becomes_plain_or_none() {
        let plain = std::path::Path::new("D:\\ISOs\\gt4.iso");
        assert_eq!(super::game_readable_path(plain).as_deref(), Some(plain), "a plain path is not even looked up");
        // On a Western Windows accents fit the code page, and the file need not have a short name.
        #[cfg(windows)]
        if unsafe { windows::Win32::Globalization::GetACP() } == 1252 {
            let accented = std::path::Path::new("C:\\Users\\João\\Imagens de Coração\\gt4 ção.iso");
            assert_eq!(super::game_readable_path(accented).as_deref(), Some(accented), "accents go as they are");
            assert!(super::game_readable_path(std::path::Path::new("C:\\no-such-folder\\ゲーム.iso")).is_none(), "outside the code page, with no short name");
        }
        let folder = std::env::temp_dir().join(format!("gt4l-ゲーム-ção-{}", std::process::id()));
        std::fs::create_dir_all(&folder).unwrap();
        let file = folder.join("スペック.iso");
        std::fs::write(&file, b"x").unwrap();
        // A volume without short names gives None; with them, a path of plain characters that names the same file.
        if let Some(short) = super::game_readable_path(&file) {
            assert!(short.to_string_lossy().is_ascii(), "{short:?}");
            assert_eq!(std::fs::read(&short).unwrap(), b"x");
        }
        let _ = std::fs::remove_dir_all(&folder);
    }

    use super::free_space;

    #[test]
    fn the_temp_folder_has_some_free_space_and_missing_children_use_their_parent() {
        let temp = std::env::temp_dir();
        let direct = free_space(&temp);
        if cfg!(windows) {
            assert!(direct.is_some_and(|bytes| bytes > 0));
            assert!(free_space(&temp.join("does-not-exist").join("disc")).is_some());
        }
    }
}
