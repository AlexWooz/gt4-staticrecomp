//! Minimal ISO 9660 reader for the root directory only. GT4's files (`SYSTEM.CNF`, `CORE.GT4`, the
//! boot executable) all sit in the root, so nothing more is needed. The reader never writes.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

pub const SECTOR_BYTES: u64 = 2048;
const PRIMARY_DESCRIPTOR_SECTOR: u64 = 16;
const MAX_DIRECTORY_BYTES: u64 = 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileEntry {
    /// Name as stored on disc, including the version suffix, for example `SYSTEM.CNF;1`.
    pub name: String,
    pub extent_sector: u64,
    pub size_bytes: u64,
}

#[derive(Debug)]
pub enum IsoError {
    /// No `CD001` primary volume descriptor at sector 16, or an unsupported block size.
    NotIso(String),
    Truncated(String),
    FileNotFound(String),
    TooLarge { name: String, size_bytes: u64, limit: u64 },
    Io(io::Error),
}

impl std::fmt::Display for IsoError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            IsoError::NotIso(detail) => write!(formatter, "not a plain ISO 9660 image: {detail}"),
            IsoError::Truncated(detail) => write!(formatter, "image is truncated: {detail}"),
            IsoError::FileNotFound(name) => write!(formatter, "{name} is not on the disc"),
            IsoError::TooLarge { name, size_bytes, limit } => {
                write!(formatter, "{name} is {size_bytes} bytes, above the {limit}-byte limit")
            }
            IsoError::Io(error) => write!(formatter, "{error}"),
        }
    }
}

impl std::error::Error for IsoError {}

impl From<io::Error> for IsoError {
    fn from(error: io::Error) -> Self {
        IsoError::Io(error)
    }
}

pub struct Iso {
    file: File,
    length_bytes: u64,
    volume_space_bytes: u64,
    volume_identifier: String,
    root: FileEntry,
}

impl Iso {
    pub fn open(path: &Path) -> Result<Iso, IsoError> {
        let mut file = File::open(path)?;
        let length_bytes = file.metadata()?.len();
        let descriptor = read_at(&mut file, PRIMARY_DESCRIPTOR_SECTOR * SECTOR_BYTES, SECTOR_BYTES as usize)
            .map_err(|_| IsoError::Truncated("no primary volume descriptor".into()))?;
        if descriptor[0] != 1 || &descriptor[1..6] != b"CD001" {
            return Err(IsoError::NotIso("sector 16 has no CD001 primary volume descriptor".into()));
        }
        let block_size = u16::from_le_bytes([descriptor[128], descriptor[129]]);
        if block_size as u64 != SECTOR_BYTES {
            return Err(IsoError::NotIso(format!("logical block size {block_size}, expected 2048")));
        }
        let volume_identifier = String::from_utf8_lossy(&descriptor[40..72]).trim().to_string();
        let volume_sectors = u32::from_le_bytes([descriptor[80], descriptor[81], descriptor[82], descriptor[83]]);
        let volume_space_bytes = volume_sectors as u64 * SECTOR_BYTES;
        let root = parse_record(&descriptor[156..190])
            .ok_or_else(|| IsoError::NotIso("root directory record is malformed".into()))?;
        Ok(Iso { file, length_bytes, volume_space_bytes, volume_identifier, root: FileEntry { name: ".".into(), ..root } })
    }

    pub fn length_bytes(&self) -> u64 {
        self.length_bytes
    }

    /// The volume space size of the primary descriptor, in bytes. On a dual-layer DVD it covers the first layer
    /// only, so it is a lower bound of the image size, never an exact size.
    pub fn volume_space_bytes(&self) -> u64 {
        self.volume_space_bytes
    }

    /// The volume identifier from the primary descriptor (used to recognise other discs).
    pub fn volume_identifier(&self) -> &str {
        &self.volume_identifier
    }

    /// Finds a root-level file. Names compare case-insensitively and must include `;1`.
    pub fn find(&mut self, name: &str) -> Result<FileEntry, IsoError> {
        let root = self.root.clone();
        self.find_in(&root, name)
    }

    /// Finds a file by its path from the root, directories separated by `/` (`IRX/CDVDMAN.IRX;1`). Directory names have no
    /// version suffix; the file name must include `;1`.
    pub fn find_path(&mut self, path: &str) -> Result<FileEntry, IsoError> {
        let mut directory = self.root.clone();
        let mut parts = path.trim_start_matches('/').split('/').peekable();
        while let Some(part) = parts.next() {
            let entry = self.find_in(&directory, part)?;
            if parts.peek().is_none() {
                return Ok(entry);
            }
            directory = entry;
        }
        Err(IsoError::FileNotFound(path.to_string()))
    }

    /// Finds `name` in one directory (the records of `directory`'s extent).
    fn find_in(&mut self, directory: &FileEntry, name: &str) -> Result<FileEntry, IsoError> {
        let directory_bytes = directory.size_bytes.min(MAX_DIRECTORY_BYTES);
        let directory = read_at(&mut self.file, directory.extent_sector * SECTOR_BYTES, directory_bytes as usize)
            .map_err(|_| IsoError::Truncated(format!("directory {}", directory.name)))?;
        let mut offset = 0usize;
        while offset < directory.len() {
            let length = directory[offset] as usize;
            if length == 0 {
                // Records never cross a sector boundary; zero padding skips to the next sector.
                offset = (offset / SECTOR_BYTES as usize + 1) * SECTOR_BYTES as usize;
                continue;
            }
            if offset + length > directory.len() {
                break;
            }
            if let Some(entry) = parse_record(&directory[offset..offset + length]) {
                if entry.name.eq_ignore_ascii_case(name) {
                    return Ok(entry);
                }
            }
            offset += length;
        }
        Err(IsoError::FileNotFound(name.to_string()))
    }

    /// Reads a whole file, refusing anything larger than `limit_bytes`.
    pub fn read(&mut self, entry: &FileEntry, limit_bytes: u64) -> Result<Vec<u8>, IsoError> {
        if entry.size_bytes > limit_bytes {
            return Err(IsoError::TooLarge {
                name: entry.name.clone(),
                size_bytes: entry.size_bytes,
                limit: limit_bytes,
            });
        }
        read_at(&mut self.file, entry.extent_sector * SECTOR_BYTES, entry.size_bytes as usize)
            .map_err(|_| IsoError::Truncated(format!("{} data", entry.name)))
    }
}

fn read_at(file: &mut File, offset: u64, length: usize) -> io::Result<Vec<u8>> {
    file.seek(SeekFrom::Start(offset))?;
    let mut buffer = vec![0u8; length];
    file.read_exact(&mut buffer)?;
    Ok(buffer)
}

/// Parses one directory record (ECMA-119 section 9.1). Returns `None` for a malformed record.
fn parse_record(record: &[u8]) -> Option<FileEntry> {
    if record.len() < 34 {
        return None;
    }
    let name_length = record[32] as usize;
    if record.len() < 33 + name_length {
        return None;
    }
    let extent_sector = u32::from_le_bytes(record[2..6].try_into().ok()?) as u64;
    let size_bytes = u32::from_le_bytes(record[10..14].try_into().ok()?) as u64;
    let raw_name = &record[33..33 + name_length];
    let name = match raw_name {
        [0] => ".".to_string(),
        [1] => "..".to_string(),
        _ => String::from_utf8_lossy(raw_name).into_owned(),
    };
    Some(FileEntry { name, extent_sector, size_bytes })
}
