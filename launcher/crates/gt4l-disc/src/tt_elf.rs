//! Tourist Trophy: the ELF from `CORE.TT` (a port of `scripts/spec2_beta_elf.py`, which `docs/TT-STUDY.md` confirmed on
//! TT's disc unchanged).
//!
//! ```text
//! CORE.TT   = [u64 LE nonce][Salsa20/20 cipher text][u32 LE CRC-32C of everything before]
//! plaintext = Salsa20/20(key "PolyphonyDigital" (16-byte, tau constants), nonce, counter 0) XOR cipher text
//! plaintext = the CORE container of v2.00: [u16 0x0101][u32 inflated size][raw deflate] (`elf::inflate_core`)
//! inflated  = two u16-length-prefixed blobs, i32 record count + u32 entry, then per record u32 address, u32 size, body
//! ```
//!
//! Layout (as `build_elf`): text at file 0x1000 / VA 0x100000, the 24-byte `.reginfo` record as its own segment right
//! after the text, the data page-aligned with an 8 MiB zero-fill extent. The result must equal the pinned derived ELF.

use crate::elf;

const SALSA_KEY: &[u8; 16] = b"PolyphonyDigital";
const R5900_FLAGS: u32 = 0x2092_4001;
const TEXT_ADDRESS: u32 = 0x0010_0000;
const MAX_ELF: usize = 16 * 1024 * 1024;

fn quarter(x: &mut [u32; 16], a: usize, b: usize, c: usize, d: usize) {
    x[b] ^= x[a].wrapping_add(x[d]).rotate_left(7);
    x[c] ^= x[b].wrapping_add(x[a]).rotate_left(9);
    x[d] ^= x[c].wrapping_add(x[b]).rotate_left(13);
    x[a] ^= x[d].wrapping_add(x[c]).rotate_left(18);
}

fn word(bytes: &[u8], index: usize) -> u32 {
    u32::from_le_bytes([bytes[4 * index], bytes[4 * index + 1], bytes[4 * index + 2], bytes[4 * index + 3]])
}

/// One 64-byte Salsa20/20 block (16-byte key: the key fills both halves, tau constants).
fn salsa20_block(nonce: &[u8; 8], counter: u64) -> [u8; 64] {
    let constants = b"expand 16-byte k";
    let ctr = counter.to_le_bytes();
    let state: [u32; 16] = [
        word(constants, 0), word(SALSA_KEY, 0), word(SALSA_KEY, 1), word(SALSA_KEY, 2),
        word(SALSA_KEY, 3), word(constants, 1), word(nonce, 0), word(nonce, 1),
        word(&ctr, 0), word(&ctr, 1), word(constants, 2), word(SALSA_KEY, 0),
        word(SALSA_KEY, 1), word(SALSA_KEY, 2), word(SALSA_KEY, 3), word(constants, 3),
    ];
    let mut x = state;
    for _ in 0..10 {
        quarter(&mut x, 0, 4, 8, 12);
        quarter(&mut x, 5, 9, 13, 1);
        quarter(&mut x, 10, 14, 2, 6);
        quarter(&mut x, 15, 3, 7, 11);
        quarter(&mut x, 0, 1, 2, 3);
        quarter(&mut x, 5, 6, 7, 4);
        quarter(&mut x, 10, 11, 8, 9);
        quarter(&mut x, 15, 12, 13, 14);
    }
    let mut out = [0u8; 64];
    for index in 0..16 {
        out[4 * index..4 * index + 4].copy_from_slice(&x[index].wrapping_add(state[index]).to_le_bytes());
    }
    out
}

/// CRC-32C (Castagnoli) as the loader computes it: MSB-first table, polynomial 0x1EDC6F41, initial and final inversion.
pub fn crc32c_msb(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (index, slot) in table.iter_mut().enumerate() {
        let mut crc = (index as u32) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 { (crc << 1) ^ 0x1EDC_6F41 } else { crc << 1 };
        }
        *slot = crc;
    }
    let mut crc = 0xFFFF_FFFFu32;
    for &byte in data {
        crc = (crc << 8) ^ table[(((crc >> 24) ^ byte as u32) & 0xFF) as usize];
    }
    crc ^ 0xFFFF_FFFF
}

/// Checks the trailer and decrypts: the CORE container (`[u16 0x0101][u32 size][raw deflate]`).
pub fn decrypt_core(core: &[u8]) -> Result<Vec<u8>, String> {
    if core.len() < 12 {
        return Err("CORE.TT is truncated".into());
    }
    let (body, trailer) = core.split_at(core.len() - 4);
    let expected = u32::from_le_bytes([trailer[0], trailer[1], trailer[2], trailer[3]]);
    let found = crc32c_msb(body);
    if found != expected {
        return Err(format!("CORE.TT CRC-32C mismatch: {found:08x} != {expected:08x}"));
    }
    let nonce: [u8; 8] = body[..8].try_into().expect("8 bytes");
    let cipher = &body[8..];
    let mut plain = Vec::with_capacity(cipher.len());
    for (block_index, chunk) in cipher.chunks(64).enumerate() {
        let stream = salsa20_block(&nonce, block_index as u64);
        plain.extend(chunk.iter().zip(stream.iter()).map(|(a, b)| a ^ b));
    }
    Ok(plain)
}

struct Record<'a> {
    address: u32,
    body: &'a [u8],
}

fn read_u16(data: &[u8], offset: usize) -> Result<u16, String> {
    data.get(offset..offset + 2).map(|b| u16::from_le_bytes([b[0], b[1]])).ok_or_else(|| "payload truncated".to_string())
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32, String> {
    data.get(offset..offset + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        .ok_or_else(|| "payload truncated".to_string())
}

fn parse_records(payload: &[u8]) -> Result<(u32, Vec<Record<'_>>), String> {
    let mut offset = 0usize;
    for _ in 0..2 {
        let length = read_u16(payload, offset)? as usize;
        offset += 2 + length;
    }
    let count = read_u32(payload, offset)? as i32;
    let entry = read_u32(payload, offset + 4)?;
    offset += 8;
    if !(1..=16).contains(&count) {
        return Err(format!("implausible record count {count}"));
    }
    let mut records = Vec::new();
    for _ in 0..count {
        let address = read_u32(payload, offset)?;
        let size = read_u32(payload, offset + 4)? as usize;
        offset += 8;
        let body = payload.get(offset..offset + size).ok_or("record extends beyond the payload")?;
        records.push(Record { address, body });
        offset += size;
    }
    if offset != payload.len() {
        return Err(format!("{} trailing bytes after the records", payload.len() - offset));
    }
    Ok((entry, records))
}

/// `CORE.TT` to the derived ELF (the pin check is the caller's).
pub fn derive(core: &[u8], data_bss_bytes: u32) -> Result<Vec<u8>, String> {
    let container = decrypt_core(core)?;
    let payload = elf::inflate_core(&container).map_err(|error| error.to_string())?;
    let (entry, records) = parse_records(&payload)?;
    let text = records.iter().find(|record| record.address == TEXT_ADDRESS).ok_or("no text record at 0x100000")?;
    let data = records.iter().max_by_key(|record| record.address).ok_or("no records")?;
    if std::ptr::eq(text, data) {
        return Err("records do not describe a text and a data image".into());
    }
    let text_end = text.address + text.body.len() as u32;
    // (type, file offset, vaddr, filesz, memsz, flags, align, body)
    let mut parts: Vec<(u32, usize, u32, u32, u32, u32, u32, &[u8])> = Vec::new();
    let mut file_offset = 0x1000usize;
    parts.push((1, file_offset, text.address, text.body.len() as u32, text.body.len() as u32, 7, 4096, text.body));
    file_offset += text.body.len();
    for record in records.iter().filter(|record| !std::ptr::eq(*record, text) && !std::ptr::eq(*record, data)) {
        if record.body.len() != 24 {
            return Err(format!("unexpected extra record of {} bytes at {:08x}", record.body.len(), record.address));
        }
        parts.push((0x7000_0000, file_offset, text_end, 24, 24, 4, 4, record.body));
        file_offset += 24;
    }
    file_offset = (file_offset + 0xFFF) & !0xFFF;
    let data_size = data.body.len() as u32;
    parts.push((1, file_offset, data.address, data_size, data_size + data_bss_bytes, 6, 4096, data.body));
    file_offset += data.body.len();
    if file_offset > MAX_ELF || 52 + 32 * parts.len() > file_offset {
        return Err("ELF size outside the supported limits".into());
    }
    let mut output = vec![0u8; file_offset];
    output[..7].copy_from_slice(b"\x7fELF\x01\x01\x01");
    let mut header = Vec::with_capacity(36);
    for value in [2u16, 8] {
        header.extend_from_slice(&value.to_le_bytes());
    }
    for value in [1u32, entry, 52, 0, R5900_FLAGS] {
        header.extend_from_slice(&value.to_le_bytes());
    }
    for value in [52u16, 32, parts.len() as u16, 0, 0, 0] {
        header.extend_from_slice(&value.to_le_bytes());
    }
    output[16..16 + header.len()].copy_from_slice(&header);
    for (index, (kind, offset, vaddr, filesz, memsz, flags, align, body)) in parts.iter().enumerate() {
        let at = 52 + index * 32;
        for (slot, value) in [*kind, *offset as u32, *vaddr, *vaddr, *filesz, *memsz, *flags, *align].iter().enumerate() {
            output[at + 4 * slot..at + 4 * slot + 4].copy_from_slice(&value.to_le_bytes());
        }
        output[*offset..*offset + body.len()].copy_from_slice(body);
    }
    Ok(output)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex16(bytes: &[u8]) -> String {
        bytes[..16].iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn salsa20_matches_the_python_reference_block() {
        // scripts/spec2_beta_elf.py salsa20_block(b"PolyphonyDigital", nonce, counter)[:16].
        assert_eq!(hex16(&salsa20_block(&[0u8; 8], 0)), "1fd3329393af822e0d2a06e02f7a8992");
        assert_eq!(hex16(&salsa20_block(&[0, 1, 2, 3, 4, 5, 6, 7], 5)), "cd5680e6a1dce86968ee589b620e6fbd");
    }

    #[test]
    fn crc32c_msb_matches_the_python_reference() {
        // scripts/spec2_beta_elf.py crc32c(b"123456789") (the loader's MSB-first table).
        assert_eq!(crc32c_msb(b"123456789"), 0x0544_0F15);
    }

    #[test]
    fn a_bad_trailer_is_refused() {
        let mut core = vec![0u8; 64];
        core[60..].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
        assert!(decrypt_core(&core).unwrap_err().contains("CRC-32C"));
    }
}
