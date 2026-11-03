//! Folder form of a card item (save or replay) as stored in the user data folder.
//!
//! An exported item is a folder named like its PS2 directory. It holds:
//! * `_item.hex`: the item's own 512-byte directory entry, hex encoded (exact round trip);
//! * `_children.txt`: one line per child: `file<TAB>name<TAB>entry-hex` or `dir<TAB>...`;
//! * the file contents as plain files and sub-folders for sub-directories.
//!
//! Folders written by PCSX2 (no `_item.hex`) are read by `import_raw_directory`: every
//! file is a child with a default entry, and `_pcsx2*` metadata files are ignored.

use crate::card::{validate_name, CardError, DirEntry, Node, MODE_FILE_DEFAULT};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

const ITEM_FILE: &str = "_item.hex";
const CHILDREN_FILE: &str = "_children.txt";

#[derive(Debug)]
pub enum FolderError {
    Io(io::Error),
    Card(CardError),
    /// A folder does not have the expected layout.
    Layout(String),
    /// A file of an item could not be read (issue #160: a bare "os error 2" named nothing). `path` is the file.
    Unreadable { path: PathBuf, error: io::Error },
}

impl From<io::Error> for FolderError {
    fn from(error: io::Error) -> Self {
        FolderError::Io(error)
    }
}

impl From<CardError> for FolderError {
    fn from(error: CardError) -> Self {
        FolderError::Card(error)
    }
}

impl std::fmt::Display for FolderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FolderError::Io(error) => write!(f, "{error}"),
            FolderError::Card(error) => write!(f, "{error}"),
            FolderError::Layout(what) => write!(f, "folder layout: {what}"),
            FolderError::Unreadable { path, error } => write!(f, "{}: {error}", path.display()),
        }
    }
}

impl std::error::Error for FolderError {}

pub fn to_hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &byte in bytes {
        out.push(DIGITS[(byte >> 4) as usize] as char);
        out.push(DIGITS[(byte & 15) as usize] as char);
    }
    out
}

pub fn from_hex(text: &str) -> Result<Vec<u8>, FolderError> {
    if text.len() % 2 != 0 {
        return Err(FolderError::Layout("odd-length hex".into()));
    }
    let digit = |c: u8| -> Result<u8, FolderError> {
        match c {
            b'0'..=b'9' => Ok(c - b'0'),
            b'a'..=b'f' => Ok(c - b'a' + 10),
            b'A'..=b'F' => Ok(c - b'A' + 10),
            _ => Err(FolderError::Layout("bad hex digit".into())),
        }
    };
    text.as_bytes().chunks(2).map(|pair| Ok(digit(pair[0])? << 4 | digit(pair[1])?)).collect()
}

/// Writes `item` as a folder `parent/<item name>`. The folder is built beside the target
/// and renamed into place; an existing folder of that name is moved aside, never removed.
pub fn export_item(parent: &Path, item: &Node) -> Result<PathBuf, FolderError> {
    let target = parent.join(item.name());
    let staging = parent.join(format!(".staging-{}", item.name()));
    if staging.exists() {
        fs::remove_dir_all(&staging)?; // leftover of an interrupted export of this item
    }
    fs::create_dir_all(&staging)?;
    write_contents(&staging, item)?;
    if target.exists() {
        let aside_dir = parent.join(".replaced");
        fs::create_dir_all(&aside_dir)?;
        fs::rename(&target, aside_dir.join(format!("{}-{}", item.name(), crate::timestamp())))?;
    }
    fs::rename(&staging, &target)?;
    Ok(target)
}

fn write_contents(folder: &Path, item: &Node) -> Result<(), FolderError> {
    fs::write(folder.join(ITEM_FILE), to_hex(item.entry.raw()))?;
    let mut children = String::new();
    for child in &item.children {
        let kind = if child.entry.is_dir() { "dir" } else { "file" };
        children.push_str(&format!("{kind}\t{}\t{}\n", child.name(), to_hex(child.entry.raw())));
        let path = folder.join(child.name());
        if child.entry.is_dir() {
            fs::create_dir_all(&path)?;
            write_contents(&path, child)?;
        } else {
            fs::write(&path, &child.data)?;
        }
    }
    fs::write(folder.join(CHILDREN_FILE), children)?;
    Ok(())
}

/// `fs::read_to_string` whose error names the file.
fn read_text(path: &Path) -> Result<String, FolderError> {
    fs::read_to_string(path).map_err(|error| FolderError::Unreadable { path: path.to_path_buf(), error })
}

/// `fs::read` whose error names the file.
fn read_bytes(path: &Path) -> Result<Vec<u8>, FolderError> {
    fs::read(path).map_err(|error| FolderError::Unreadable { path: path.to_path_buf(), error })
}

/// Reads a folder written by `export_item`. A file that cannot be read is reported with its path
/// ([`FolderError::Unreadable`]).
pub fn import_item(folder: &Path) -> Result<Node, FolderError> {
    let item_hex = read_text(&folder.join(ITEM_FILE))?;
    let entry = DirEntry::from_raw(&from_hex(item_hex.trim())?)?;
    let listing = read_text(&folder.join(CHILDREN_FILE))?;
    let mut children = Vec::new();
    for line in listing.lines().filter(|l| !l.is_empty()) {
        let mut fields = line.split('\t');
        let kind = fields.next().unwrap_or("");
        let name = fields.next().ok_or_else(|| FolderError::Layout("child line without a name".into()))?;
        let raw = fields.next().ok_or_else(|| FolderError::Layout("child line without an entry".into()))?;
        validate_name(name)?;
        let child_entry = DirEntry::from_raw(&from_hex(raw)?)?;
        let path = folder.join(name);
        let child = match kind {
            "dir" => import_item(&path)?,
            "file" => Node::file(child_entry, read_bytes(&path)?),
            other => return Err(FolderError::Layout(format!("unknown child kind {other:?}"))),
        };
        children.push(child);
    }
    Ok(Node { entry, children, data: Vec::new() })
}

/// Reads a folder with no metadata (a PCSX2 folder card save): every file is a child.
pub fn import_raw_directory(folder: &Path) -> Result<Node, FolderError> {
    let name = folder
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| FolderError::Layout("folder name is not text".into()))?
        .to_string();
    let mut children = Vec::new();
    let mut entries: Vec<_> = fs::read_dir(folder)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let file_name = entry.file_name().to_string_lossy().into_owned();
        if file_name.starts_with("_pcsx2") || file_name.starts_with('.') {
            continue;
        }
        let path = entry.path();
        if path.is_dir() {
            children.push(import_raw_directory(&path)?);
        } else {
            let data = fs::read(&path)?;
            let dirent = DirEntry::new(&file_name, MODE_FILE_DEFAULT, [0; 8], [0; 8])?;
            children.push(Node::file(dirent, data));
        }
    }
    let dirent = DirEntry::new(&name, crate::card::MODE_DIR_DEFAULT, [0; 8], [0; 8])?;
    Ok(Node { entry: dirent, children, data: Vec::new() })
}
