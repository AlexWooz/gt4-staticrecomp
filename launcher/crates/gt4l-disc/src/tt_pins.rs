//! The Tourist Trophy (USA, SCUS-97502) identities, read from `docs/inputs/tt-iso.json` (embedded at compile time, so the
//! Python study and the launcher share one source; `docs/TT-STUDY.md`).

use std::collections::BTreeMap;

use serde::Deserialize;

use crate::pins::Identity;

const ISO_JSON: &str = include_str!("../../../../docs/inputs/tt-iso.json");

/// One code file of Tourist Trophy besides `CORE.TT`, by its path on the disc (`/IRX/MCMAN.IRX;1`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TtCodeFile {
    pub path: String,
    pub identity: Identity,
}

/// Everything setup needs to know about Tourist Trophy.
#[derive(Clone, Debug)]
pub struct TtPins {
    pub serial: String,
    /// `/SCUS_975.02;1`, the loader that decrypts `CORE.TT` on the console.
    pub boot_path: String,
    /// The pinned full image and its name; another image with the same code is a "Tourist Trophy (USA) variant".
    pub image: Identity,
    pub image_name: String,
    /// The game code: `CORE.TT` (encrypted ELF records).
    pub core: Identity,
    /// The loader, `SYSTEM.CNF` and the IOP code (`IOPRP300.IMG`, `IRX/*`): all must match.
    pub code_files: Vec<TtCodeFile>,
    /// The ELF the launcher derives from `CORE.TT` (what the translator read).
    pub derived: Identity,
    pub derived_file: String,
    pub entry: u32,
    pub data_bss_bytes: u32,
}

#[derive(Deserialize)]
struct IdentityJson {
    size_bytes: u64,
    sha256: String,
}

#[derive(Deserialize)]
struct DiscJson {
    size_bytes: u64,
    sha256: String,
    name: String,
}

#[derive(Deserialize)]
struct SystemJson {
    boot_path: String,
    serial: String,
}

#[derive(Deserialize)]
struct DerivedJson {
    file: String,
    size_bytes: u64,
    sha256: String,
    entry: String,
    data_bss_bytes: u32,
}

#[derive(Deserialize)]
struct IsoJson {
    disc: DiscJson,
    system: SystemJson,
    files: BTreeMap<String, IdentityJson>,
    derived_elf: DerivedJson,
}

const CORE_PATH: &str = "/CORE.TT;1";

impl TtPins {
    /// The embedded pins (an error only if the repository file is malformed; the tests read it).
    pub fn embedded() -> Result<TtPins, String> {
        let iso: IsoJson = serde_json::from_str(ISO_JSON).map_err(|error| format!("docs/inputs/tt-iso.json: {error}"))?;
        let identity = |value: &IdentityJson| Identity { size_bytes: value.size_bytes, sha256: value.sha256.clone() };
        let core = iso.files.get(CORE_PATH).map(identity).ok_or("tt-iso.json does not pin CORE.TT")?;
        let code_files = iso
            .files
            .iter()
            .filter(|(path, _)| path.as_str() != CORE_PATH)
            .map(|(path, value)| TtCodeFile { path: path.clone(), identity: identity(value) })
            .collect();
        let entry = u32::from_str_radix(iso.derived_elf.entry.trim_start_matches("0x"), 16)
            .map_err(|error| format!("tt-iso.json entry: {error}"))?;
        Ok(TtPins {
            serial: iso.system.serial,
            boot_path: iso.system.boot_path,
            image: Identity { size_bytes: iso.disc.size_bytes, sha256: iso.disc.sha256 },
            image_name: iso.disc.name,
            core,
            code_files,
            derived: Identity { size_bytes: iso.derived_elf.size_bytes, sha256: iso.derived_elf.sha256 },
            derived_file: iso.derived_elf.file,
            entry,
            data_bss_bytes: iso.derived_elf.data_bss_bytes,
        })
    }

    /// The name of a full image: the pinned one, or `None` for a variant with the same code.
    pub fn known_image(&self, identity: &Identity) -> Option<&str> {
        (identity == &self.image).then_some(self.image_name.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_embedded_pins_name_the_tt_disc_and_its_code() {
        let pins = TtPins::embedded().unwrap();
        assert_eq!(pins.serial, "SCUS-97502");
        assert_eq!(pins.boot_path, "/SCUS_975.02;1");
        assert_eq!(pins.core.size_bytes, 1_849_178);
        assert_eq!(pins.derived_file, "SCUS_975.02");
        assert_eq!(pins.entry, 0x0010_0008);
        assert_eq!(pins.data_bss_bytes, 0x0080_0000);
        assert!(pins.code_files.iter().any(|file| file.path == "/IOPRP300.IMG;1"));
        assert!(pins.code_files.iter().any(|file| file.path == "/IRX/MCMAN.IRX;1"));
        assert!(pins.code_files.iter().all(|file| file.path != "/CORE.TT;1"));
    }
}
