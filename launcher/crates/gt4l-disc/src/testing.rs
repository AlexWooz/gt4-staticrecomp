//! Helpers for tests: a synthetic ISO 9660 image and a synthetic CORE with a matching pinned layout.
//! Nothing here contains game data. The real disc is checked by an ignored test that needs the
//! owner's image (see `tests/real_disc.rs`).

use std::io::Write;

use flate2::write::DeflateEncoder;
use flate2::Compression;

use crate::elf::{self, CoreRecord, NativePolicy, ProgramHeader};
use crate::iso::SECTOR_BYTES;
use crate::pins::{Identity, Pins};

const ROOT_DIRECTORY_SECTOR: u32 = 18;

/// Builds a plain ISO image with the given files: root-level names, or one directory level (`IRX/CDVDMAN.IRX;1`). Each
/// file starts on its own sector; each directory takes one sector.
pub fn build_iso(volume_identifier: &str, files: &[(&str, Vec<u8>)]) -> Vec<u8> {
    // Directories in first-seen order, one sector each after the root's.
    let mut directories: Vec<&str> = Vec::new();
    for (name, _) in files {
        if let Some((directory, _)) = name.split_once('/') {
            if !directories.contains(&directory) {
                directories.push(directory);
            }
        }
    }
    let directory_sector = |index: usize| ROOT_DIRECTORY_SECTOR + 1 + index as u32;
    let mut records = Vec::new();
    records.extend(directory_record(&[0], ROOT_DIRECTORY_SECTOR, SECTOR_BYTES as u32, true));
    records.extend(directory_record(&[1], ROOT_DIRECTORY_SECTOR, SECTOR_BYTES as u32, true));
    let mut sub_records: Vec<Vec<u8>> = directories
        .iter()
        .enumerate()
        .map(|(index, _)| {
            let mut own = directory_record(&[0], directory_sector(index), SECTOR_BYTES as u32, true);
            own.extend(directory_record(&[1], ROOT_DIRECTORY_SECTOR, SECTOR_BYTES as u32, true));
            own
        })
        .collect();
    let mut data_sector = ROOT_DIRECTORY_SECTOR + 1 + directories.len() as u32;
    let mut placed = Vec::new();
    for (name, bytes) in files {
        match name.split_once('/') {
            Some((directory, file)) => {
                let index = directories.iter().position(|known| *known == directory).unwrap();
                sub_records[index].extend(directory_record(file.as_bytes(), data_sector, bytes.len() as u32, false));
            }
            None => records.extend(directory_record(name.as_bytes(), data_sector, bytes.len() as u32, false)),
        }
        placed.push((data_sector, bytes));
        data_sector += sectors_for(bytes.len() as u64) as u32;
    }
    for (index, directory) in directories.iter().enumerate() {
        records.extend(directory_record(directory.as_bytes(), directory_sector(index), SECTOR_BYTES as u32, true));
    }
    assert!(records.len() <= SECTOR_BYTES as usize, "test ISO root directory is too large");
    assert!(sub_records.iter().all(|own| own.len() <= SECTOR_BYTES as usize), "test ISO directory is too large");

    let total_sectors = data_sector as usize;
    let mut image = vec![0u8; total_sectors * SECTOR_BYTES as usize];

    // Primary volume descriptor at sector 16.
    let pvd = &mut image[16 * 2048..17 * 2048];
    pvd[0] = 1;
    pvd[1..6].copy_from_slice(b"CD001");
    pvd[6] = 1;
    let volume = format!("{volume_identifier:<32}");
    pvd[40..72].copy_from_slice(&volume.as_bytes()[..32]);
    pvd[80..84].copy_from_slice(&(total_sectors as u32).to_le_bytes());
    pvd[84..88].copy_from_slice(&(total_sectors as u32).to_be_bytes());
    pvd[120..122].copy_from_slice(&1u16.to_le_bytes());
    pvd[128..130].copy_from_slice(&2048u16.to_le_bytes());
    pvd[130..132].copy_from_slice(&2048u16.to_be_bytes());
    let root = directory_record(&[0], ROOT_DIRECTORY_SECTOR, SECTOR_BYTES as u32, true);
    pvd[156..156 + root.len()].copy_from_slice(&root);

    // Volume descriptor set terminator at sector 17.
    image[17 * 2048] = 255;
    image[17 * 2048 + 1..17 * 2048 + 6].copy_from_slice(b"CD001");
    image[17 * 2048 + 6] = 1;

    // Root directory at sector 18, then one sector per directory.
    image[18 * 2048..18 * 2048 + records.len()].copy_from_slice(&records);
    for (index, own) in sub_records.iter().enumerate() {
        let start = directory_sector(index) as usize * 2048;
        image[start..start + own.len()].copy_from_slice(own);
    }

    for (sector, bytes) in placed {
        let start = sector as usize * 2048;
        image[start..start + bytes.len()].copy_from_slice(bytes);
    }
    image
}

fn sectors_for(bytes: u64) -> u64 {
    bytes.div_ceil(SECTOR_BYTES).max(1)
}

/// One ECMA-119 directory record with an even total length.
fn directory_record(name: &[u8], extent: u32, size: u32, is_directory: bool) -> Vec<u8> {
    let mut length = 33 + name.len();
    if length % 2 == 1 {
        length += 1;
    }
    let mut record = vec![0u8; length];
    record[0] = length as u8;
    record[2..6].copy_from_slice(&extent.to_le_bytes());
    record[6..10].copy_from_slice(&extent.to_be_bytes());
    record[10..14].copy_from_slice(&size.to_le_bytes());
    record[14..18].copy_from_slice(&size.to_be_bytes());
    record[25] = if is_directory { 2 } else { 0 };
    record[28..30].copy_from_slice(&1u16.to_le_bytes());
    record[30..32].copy_from_slice(&1u16.to_be_bytes());
    record[32] = name.len() as u8;
    record[33..33 + name.len()].copy_from_slice(name);
    record
}

/// `SYSTEM.CNF` text for a boot executable name such as `SCUS_973.28`.
pub fn system_cnf(boot_name: &str, version: &str, video_mode: &str) -> Vec<u8> {
    format!("BOOT2 = cdrom0:\\{boot_name};1\r\nVER = {version}\r\nVMODE = {video_mode}\r\n").into_bytes()
}

/// A synthetic game: CORE bytes plus the policy and final ELF that the rebuild must produce.
pub struct SyntheticGame {
    pub core: Vec<u8>,
    pub policy: NativePolicy,
    pub elf: Vec<u8>,
}

/// Builds a CORE with one loadable record of `body` at `address`, and the matching policy.
pub fn synthetic_game(address: u32, entry: u32, body: &[u8]) -> SyntheticGame {
    let body_offset = 20u32;
    let mut payload = Vec::new();
    payload.extend_from_slice(&0u16.to_le_bytes());
    payload.extend_from_slice(&0u16.to_le_bytes());
    payload.extend_from_slice(&1i32.to_le_bytes());
    payload.extend_from_slice(&entry.to_le_bytes());
    payload.extend_from_slice(&address.to_le_bytes());
    payload.extend_from_slice(&(body.len() as u32).to_le_bytes());
    payload.extend_from_slice(body);

    let mut encoder = DeflateEncoder::new(Vec::new(), Compression::best());
    encoder.write_all(&payload).unwrap();
    let mut core = Vec::new();
    core.extend_from_slice(&0u16.to_le_bytes());
    core.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    core.extend_from_slice(&encoder.finish().unwrap());

    let header = ProgramHeader {
        kind: 1,
        file_offset: 4096,
        virtual_address: address,
        physical_address: address,
        file_size: body.len() as u32,
        memory_size: body.len() as u32,
        flags: 7,
        alignment: 4096,
    };
    let mut policy = NativePolicy {
        entry_address: entry,
        native_elf_size: 4096 + body.len() as u64,
        native_elf_sha256: String::new(),
        records: vec![CoreRecord {
            core_target_address: address,
            size_bytes: body.len() as u32,
            inflated_payload_offset: body_offset,
            payload_sha256: elf::sha256_hex(body),
            reference_program_header: header,
        }],
    };
    let parts = elf::split_payload(&payload, &policy).expect("synthetic payload matches its own policy");
    let elf_image = elf::assemble(&parts, &policy).expect("synthetic layout is valid");
    policy.native_elf_sha256 = elf::sha256_hex(&elf_image);
    SyntheticGame { core, policy, elf: elf_image }
}

/// Pins for a synthetic disc. `image` is the full ISO bytes.
pub fn pins_for(image: &[u8], game: &SyntheticGame, serial: &str, version: &str) -> Pins {
    Pins {
        disc: Identity { size_bytes: image.len() as u64, sha256: elf::sha256_hex(image) },
        core: Identity { size_bytes: game.core.len() as u64, sha256: elf::sha256_hex(&game.core) },
        elf: Identity { size_bytes: game.elf.len() as u64, sha256: elf::sha256_hex(&game.elf) },
        boot_path: "/SCUS_973.28;1".into(),
        serial: serial.into(),
        version: version.into(),
        video_mode: "NTSC".into(),
        policy: game.policy.clone(),
    }
}
