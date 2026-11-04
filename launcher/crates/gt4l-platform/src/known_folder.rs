//! The Documents and Start-menu Programs folders as Windows reports them (SHGetKnownFolderPath with FOLDERID_Documents), so OneDrive backup
//! and folder redirection to another drive are followed. The path is never built from the user name.

use std::path::PathBuf;

/// The current user's Desktop folder (FOLDERID_Desktop), the default place for a log bundle. `None` when Windows cannot
/// report it.
#[cfg(windows)]
pub fn desktop() -> Option<PathBuf> {
    known_path(&windows::Win32::UI::Shell::FOLDERID_Desktop)
}

#[cfg(not(windows))]
pub fn desktop() -> Option<PathBuf> {
    None
}

/// The current user's Documents folder, or `None` when Windows cannot report it.
#[cfg(windows)]
pub fn documents() -> Option<PathBuf> {
    use windows::Win32::System::Com::CoTaskMemFree;
    use windows::Win32::UI::Shell::{FOLDERID_Documents, SHGetKnownFolderPath, KF_FLAG_DEFAULT};

    // SAFETY: FOLDERID_Documents is a valid known-folder id; the returned buffer is freed with CoTaskMemFree, as the
    // API requires, after it is copied.
    unsafe {
        let wide = SHGetKnownFolderPath(&FOLDERID_Documents, KF_FLAG_DEFAULT, None).ok()?;
        let text = wide.to_string();
        CoTaskMemFree(Some(wide.0 as *const _));
        let path = PathBuf::from(text.ok()?);
        if path.as_os_str().is_empty() {
            None
        } else {
            Some(path)
        }
    }
}

#[cfg(not(windows))]
pub fn documents() -> Option<PathBuf> {
    None
}

/// The path Windows reports for a known folder id (SHGetKnownFolderPath).
#[cfg(windows)]
fn known_path(id: &windows::core::GUID) -> Option<PathBuf> {
    use windows::Win32::System::Com::CoTaskMemFree;
    use windows::Win32::UI::Shell::{SHGetKnownFolderPath, KF_FLAG_DEFAULT};

    // SAFETY: the id is a known-folder id; the buffer Windows returns is freed with CoTaskMemFree after it is copied.
    unsafe {
        let wide = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, None).ok()?;
        let text = wide.to_string();
        CoTaskMemFree(Some(wide.0 as *const _));
        let path = PathBuf::from(text.ok()?);
        if path.as_os_str().is_empty() {
            None
        } else {
            Some(path)
        }
    }
}

/// The current user's Start-menu Programs folder (FOLDERID_Programs), where the per-user installer of 1.0.0 to
/// 1.2.0 put its shortcuts. `None` when Windows cannot report it.
#[cfg(windows)]
pub fn programs() -> Option<PathBuf> {
    use windows::Win32::System::Com::CoTaskMemFree;
    use windows::Win32::UI::Shell::{FOLDERID_Programs, SHGetKnownFolderPath, KF_FLAG_DEFAULT};

    // SAFETY: as in `documents`.
    unsafe {
        let wide = SHGetKnownFolderPath(&FOLDERID_Programs, KF_FLAG_DEFAULT, None).ok()?;
        let text = wide.to_string();
        CoTaskMemFree(Some(wide.0 as *const _));
        let path = PathBuf::from(text.ok()?);
        if path.as_os_str().is_empty() {
            None
        } else {
            Some(path)
        }
    }
}

#[cfg(not(windows))]
pub fn programs() -> Option<PathBuf> {
    None
}

#[cfg(test)]
mod tests {
    #[test]
    #[cfg(windows)]
    fn windows_reports_an_absolute_documents_folder() {
        // Read only: the folder is resolved, nothing is created or written.
        let documents = super::documents().expect("the test user has a Documents folder");
        assert!(documents.is_absolute(), "{}", documents.display());
    }
}
