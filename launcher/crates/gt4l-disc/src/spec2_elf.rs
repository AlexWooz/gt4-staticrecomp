//! The Spec II derived game ELF: a port of `scripts/spec2_elf_patch.py` (TASK-100 S2, TASK-101).
//!
//! The plugin injector writes 35 words into guest RAM at boot; statically recompiled code never executes those stores,
//! so the words must be in the image the translator reads. `apply_patches` applies every injector entry (the old word is
//! checked first, so an unexpected image is refused) and `split_data_around_plugin` rewrites the program headers so that no
//! segment overlaps the `.plugin` PT_LOAD (the donor translator reads the first segment that contains an address).
//! Spec II is one plain ELF on the disc: no `CORE.GT4`, no decryption.
//!
//! The output must be byte-identical to the Python script's: `derive` on the pinned executable gives the pinned derived
//! identity (`spec2-elf.json`), and a test compares the Rust output with the script's file when the owner's image is present.

use crate::spec2_pins::Spec2Patch;

/// Plugin segment address (constant of `scripts/spec2_elf_patch.py`).
pub const PLUGIN_VA: u32 = 0x68A480;

/// A little-endian ELF32 program header: type, offset, vaddr, paddr, filesz, memsz, flags, align.
type Header = [u32; 8];

const PT_LOAD: u32 = 1;

fn u32_at(data: &[u8], at: usize) -> Result<u32, String> {
    data.get(at..at + 4)
        .map(|bytes| u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
        .ok_or_else(|| "the ELF header is truncated".to_string())
}

fn u16_at(data: &[u8], at: usize) -> Result<u16, String> {
    data.get(at..at + 2)
        .map(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]))
        .ok_or_else(|| "the ELF header is truncated".to_string())
}

/// The program header table: (`e_phoff`, `e_phentsize`, the headers).
fn read_headers(data: &[u8]) -> Result<(usize, usize, Vec<Header>), String> {
    if data.len() < 52 || data[..4] != [0x7f, b'E', b'L', b'F'] || data[4] != 1 || data[5] != 1 {
        return Err("not a little-endian ELF32 image".to_string());
    }
    let phoff = u32_at(data, 28)? as usize;
    let phentsize = u16_at(data, 42)? as usize;
    let phnum = u16_at(data, 44)? as usize;
    if phentsize < 32 {
        return Err("the program header entries are too small".to_string());
    }
    let mut headers = Vec::with_capacity(phnum);
    for index in 0..phnum {
        let base = phoff + index * phentsize;
        let mut header = [0u32; 8];
        for (field, slot) in header.iter_mut().enumerate() {
            *slot = u32_at(data, base + field * 4)?;
        }
        headers.push(header);
    }
    Ok((phoff, phentsize, headers))
}

/// The file offset of `address..address + size` inside a file-backed PT_LOAD.
fn file_offset(headers: &[Header], address: u32, size: u32) -> Result<usize, String> {
    for header in headers.iter().filter(|header| header[0] == PT_LOAD) {
        let (offset, vaddr, filesz) = (header[1] as u64, header[2] as u64, header[4] as u64);
        if vaddr <= address as u64 && address as u64 + size as u64 <= vaddr + filesz {
            return Ok((offset + address as u64 - vaddr) as usize);
        }
    }
    Err(format!("address {address:08x} is not file-backed"))
}

/// Applies every patch. Each old word must be there exactly, as in the script.
pub fn apply_patches(data: &[u8], patches: &[Spec2Patch]) -> Result<Vec<u8>, String> {
    let (_, _, headers) = read_headers(data)?;
    let mut out = data.to_vec();
    for patch in patches {
        if patch.size == 0 || patch.size > 8 {
            return Err(format!("patch at {:08x} has size {}", patch.address, patch.size));
        }
        let at = file_offset(&headers, patch.address, patch.size)?;
        let end = at + patch.size as usize;
        let window = out.get(at..end).ok_or_else(|| format!("address {:08x} is past the end of the file", patch.address))?;
        let mut current = 0u64;
        for (index, byte) in window.iter().enumerate() {
            current |= (*byte as u64) << (8 * index);
        }
        if current != patch.old {
            return Err(format!("apply: word at {:08x} is {:08x}, expected {:08x}", patch.address, current, patch.old));
        }
        for index in 0..patch.size as usize {
            out[at + index] = (patch.new >> (8 * index)) as u8;
        }
    }
    Ok(out)
}

/// Rewrites the program headers so no segment overlaps the `.plugin` PT_LOAD (see the module comment).
pub fn split_data_around_plugin(data: &[u8]) -> Result<Vec<u8>, String> {
    let (phoff, phentsize, headers) = read_headers(data)?;
    let plugin_index = headers
        .iter()
        .position(|header| header[0] == PT_LOAD && header[2] == PLUGIN_VA)
        .ok_or("no PT_LOAD at the plugin address")?;
    let data_index = headers
        .iter()
        .position(|header| {
            header[0] == PT_LOAD
                && header[2] != PLUGIN_VA
                && header[2] < PLUGIN_VA
                && (PLUGIN_VA as u64) < header[2] as u64 + header[5] as u64
        })
        .ok_or("no data PT_LOAD covers the plugin address")?;
    let plugin = headers[plugin_index];
    let covering = headers[data_index];
    if covering[5] < plugin[5] {
        return Err("the covering segment is smaller than the plugin".to_string());
    }
    let low_size = PLUGIN_VA - covering[2];
    let high_address = PLUGIN_VA.checked_add(plugin[4]).ok_or("the plugin segment ends past the address space")?;
    let into_covering = high_address - covering[2];
    let high_offset = covering[1] + into_covering;
    let high_size =
        covering[4].checked_sub(into_covering).ok_or("the plugin ends past the covering segment's file data")?;
    let low: Header = [1, covering[1], covering[2], covering[3], low_size, low_size, covering[6], covering[7]];
    let bss = covering[5] - covering[4];
    let high: Header =
        [1, high_offset, high_address, covering[3] + into_covering, high_size, high_size + bss, covering[6], covering[7]];
    let mut rewritten: Vec<Header> = Vec::new();
    for (index, header) in headers.iter().enumerate() {
        if index == data_index {
            rewritten.extend([low, plugin, high]);
        } else if index == plugin_index {
            continue;
        } else {
            rewritten.push(*header);
        }
    }
    if 52 + 32 * rewritten.len() > 0x1000 {
        return Err("program header table no longer fits before the text".to_string());
    }
    let mut out = data.to_vec();
    let write = |out: &mut Vec<u8>, index: usize, header: &Header| -> Result<(), String> {
        let base = phoff + index * phentsize;
        for (field, value) in header.iter().enumerate() {
            let slot = out
                .get_mut(base + field * 4..base + field * 4 + 4)
                .ok_or_else(|| "the program header table is past the end of the file".to_string())?;
            slot.copy_from_slice(&value.to_le_bytes());
        }
        Ok(())
    };
    for (index, header) in rewritten.iter().enumerate() {
        write(&mut out, index, header)?;
    }
    out[44..46].copy_from_slice(&(rewritten.len() as u16).to_le_bytes());
    // The old table had more entries; zero the leftovers so the file is deterministic.
    for index in rewritten.len()..headers.len() {
        write(&mut out, index, &[0; 8])?;
    }
    Ok(out)
}

/// The Spec II derived ELF: the injector words applied, then the data segment split around the plugin.
pub fn derive(original: &[u8], patches: &[Spec2Patch]) -> Result<Vec<u8>, String> {
    split_data_around_plugin(&apply_patches(original, patches)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A small ELF32 with a text segment, a data segment that covers the plugin address and the plugin segment.
    fn synthetic() -> Vec<u8> {
        let headers: [Header; 3] = [
            [1, 0x1000, 0x100000, 0x100000, 0x100, 0x100, 5, 0x1000],
            [1, 0x1100, 0x680000, 0x680000, 0x20000, 0x30000, 6, 0x1000],
            [1, 0x21100, PLUGIN_VA, PLUGIN_VA, 0x40, 0x40, 7, 0x10],
        ];
        let mut image = vec![0u8; 0x21140];
        image[..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
        image[4] = 1;
        image[5] = 1;
        image[28..32].copy_from_slice(&52u32.to_le_bytes());
        image[42..44].copy_from_slice(&32u16.to_le_bytes());
        image[44..46].copy_from_slice(&3u16.to_le_bytes());
        for (index, header) in headers.iter().enumerate() {
            for (field, value) in header.iter().enumerate() {
                let at = 52 + index * 32 + field * 4;
                image[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
        }
        image[0x1000..0x1004].copy_from_slice(&0x1111_2222u32.to_le_bytes());
        image
    }

    fn patch(address: u32, old: u64, new: u64) -> Spec2Patch {
        Spec2Patch { address, size: 4, old, new }
    }

    #[test]
    fn a_patch_replaces_the_expected_word_and_refuses_any_other_old_word() {
        let image = synthetic();
        let patched = apply_patches(&image, &[patch(0x100000, 0x1111_2222, 0xAABB_CCDD)]).unwrap();
        assert_eq!(&patched[0x1000..0x1004], &0xAABB_CCDDu32.to_le_bytes());
        let error = apply_patches(&image, &[patch(0x100000, 0x1234_5678, 1)]).unwrap_err();
        assert_eq!(error, "apply: word at 00100000 is 11112222, expected 12345678", "the script's message");
        assert!(apply_patches(&image, &[patch(0x500000, 0, 1)]).unwrap_err().contains("not file-backed"));
        assert!(apply_patches(&[0u8; 80], &[]).is_err());
    }

    #[test]
    fn the_data_segment_is_split_around_the_plugin() {
        let image = synthetic();
        let split = split_data_around_plugin(&image).unwrap();
        let (_, _, headers) = read_headers(&split).unwrap();
        assert_eq!(headers.len(), 4);
        assert_eq!(headers[1], [1, 0x1100, 0x680000, 0x680000, PLUGIN_VA - 0x680000, PLUGIN_VA - 0x680000, 6, 0x1000]);
        assert_eq!(headers[2][2], PLUGIN_VA, "the plugin keeps its place");
        let high_address = PLUGIN_VA + 0x40;
        let consumed = high_address - 0x680000;
        assert_eq!(headers[3][1], 0x1100 + consumed);
        assert_eq!(headers[3][2], high_address);
        assert_eq!(headers[3][4], 0x20000 - consumed);
        assert_eq!(headers[3][5], 0x20000 - consumed + 0x10000, "the bss follows the high part");
        assert_eq!(&split[44..46], &4u16.to_le_bytes());
        let loads: Vec<(u64, u64)> =
            headers.iter().filter(|h| h[0] == 1).map(|h| (h[2] as u64, h[2] as u64 + h[5] as u64)).collect();
        for (index, left) in loads.iter().enumerate() {
            for right in &loads[index + 1..] {
                assert!(left.1 <= right.0 || right.1 <= left.0, "{left:?} overlaps {right:?}");
            }
        }
    }

    #[test]
    fn an_image_without_the_plugin_is_refused() {
        let mut image = synthetic();
        let plugin_type_at = 52 + 2 * 32;
        image[plugin_type_at..plugin_type_at + 4].copy_from_slice(&0u32.to_le_bytes());
        assert_eq!(split_data_around_plugin(&image).unwrap_err(), "no PT_LOAD at the plugin address");
    }
}
