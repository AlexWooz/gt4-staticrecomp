//! The disc image as setup reads it (TASK-180). On the desktop it is a file path. On Android the player picks the image
//! through the Storage Access Framework and the app keeps a persistable permission to a document (`content://...`): an
//! app may read such a file only through the descriptor the system hands it, so the app registers a document opener
//! (its Java side, see the launcher's android.rs) and every read of the image goes through [`open`]. The image is never
//! copied (owner rule).

use std::fs::{File, Metadata};
use std::io;
use std::path::Path;

/// Whether `path` names an Android document (a content URI kept as the image's path) rather than a file.
pub fn is_document(path: &Path) -> bool {
    path.to_str().is_some_and(|text| text.starts_with("content://"))
}

/// The name to show for an image: its file name, or for a document the last part of its URI with the percent escapes
/// decoded (`.../document/raw%3A%2Fstorage%2F...%2FGT4.iso` shows as `GT4.iso`).
pub fn display_name(path: &Path) -> String {
    if !is_document(path) {
        return path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    }
    let text = path.to_str().unwrap_or_default();
    let last = text.rsplit('/').next().unwrap_or(text);
    let bytes = last.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        let hex = |byte: u8| (byte as char).to_digit(16);
        match (bytes[index], bytes.get(index + 1).copied().and_then(hex), bytes.get(index + 2).copied().and_then(hex)) {
            (b'%', Some(high), Some(low)) => {
                decoded.push((high * 16 + low) as u8);
                index += 3;
            }
            (byte, _, _) => {
                decoded.push(byte);
                index += 1;
            }
        }
    }
    let decoded = String::from_utf8_lossy(&decoded).into_owned();
    decoded.rsplit(['/', ':']).next().unwrap_or(&decoded).to_string()
}

#[cfg(target_os = "android")]
static DOCUMENT_OPENER: std::sync::OnceLock<fn(&str) -> io::Result<File>> = std::sync::OnceLock::new();

/// Android: the function that opens a document for reading (a file descriptor from the app's ContentResolver).
#[cfg(target_os = "android")]
pub fn set_document_opener(opener: fn(&str) -> io::Result<File>) {
    let _ = DOCUMENT_OPENER.set(opener);
}

/// Opens the image for reading.
pub fn open(path: &Path) -> io::Result<File> {
    #[cfg(target_os = "android")]
    if is_document(path) {
        let opener = DOCUMENT_OPENER
            .get()
            .ok_or_else(|| io::Error::new(io::ErrorKind::Unsupported, "no document opener registered"))?;
        return opener(path.to_str().unwrap_or_default());
    }
    File::open(path)
}

/// The image's metadata (size, modification time).
pub fn metadata(path: &Path) -> io::Result<Metadata> {
    if is_document(path) {
        return open(path)?.metadata();
    }
    std::fs::metadata(path)
}

/// True when the image is a regular file (or document) that can be opened for reading.
pub fn is_readable_file(path: &Path) -> bool {
    if is_document(path) {
        return open(path).is_ok_and(|file| file.metadata().is_ok_and(|metadata| metadata.is_file()));
    }
    path.is_file() && File::open(path).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_content_uris_are_documents() {
        assert!(is_document(Path::new("content://com.android.externalstorage.documents/document/primary%3AGT4.iso")));
        assert!(!is_document(Path::new("C:\\Games\\GT4.iso")));
        assert!(!is_document(Path::new("/storage/emulated/0/Download/GT4.iso")));
    }

    #[test]
    fn a_document_shows_its_decoded_file_name() {
        let document = "content://com.android.providers.downloads.documents/document/raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2FGran%20Turismo%204%20(USA)%20(v2.00).iso";
        assert_eq!(display_name(Path::new(document)), "Gran Turismo 4 (USA) (v2.00).iso");
        assert_eq!(display_name(Path::new("content://com.android.providers.downloads.documents/document/msf%3A31")), "31");
        assert_eq!(display_name(Path::new("C:/Games/GT4.iso")), "GT4.iso");
    }

    #[cfg(not(target_os = "android"))]
    #[test]
    fn a_document_cannot_be_opened_without_an_opener() {
        assert!(open(Path::new("content://example/doc")).is_err());
        assert!(!is_readable_file(Path::new("content://example/doc")));
    }
}
