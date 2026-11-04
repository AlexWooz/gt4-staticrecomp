//! The Gran Turismo 4 Spec II 1.10 identities, read from the same `docs/inputs` files the Python tools use (embedded at
//! compile time, so there is one source). TASK-100 owns `spec2-iso.json`, `spec2-elf.json`, `spec2-beta.json` and
//! `spec2-patches.json`; `spec2-variants.json` (TASK-101) names the known disc images.

use std::collections::BTreeMap;

use serde::Deserialize;

use crate::pins::Identity;

const ISO_JSON: &str = include_str!("../../../../docs/inputs/spec2-iso.json");
const ELF_JSON: &str = include_str!("../../../../docs/inputs/spec2-elf.json");
const BETA_JSON: &str = include_str!("../../../../docs/inputs/spec2-beta.json");
const VARIANTS_JSON: &str = include_str!("../../../../docs/inputs/spec2-variants.json");
/// The 35 injector words, the same file `scripts/spec2_elf_patch.py` reads.
const PATCHES_JSON: &str = include_str!("../../../../docs/inputs/spec2-patches.json");

/// One word the Spec II plugin injector writes into guest RAM at boot, applied ahead of translation
/// (`spec2_elf::apply_patches`). `old` is checked first.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Spec2Patch {
    pub address: u32,
    pub size: u32,
    pub old: u64,
    pub new: u64,
}

/// A disc image of Spec II whose executable is the pinned one.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KnownImage {
    pub name: String,
    pub identity: Identity,
}

/// Everything setup needs to know about Gran Turismo 4 Spec II 1.10 (TASK-100, TASK-101).
#[derive(Clone, Debug)]
pub struct Spec2Pins {
    pub serial: String,
    /// `/SCUS_974.36;1`.
    pub boot_path: String,
    /// `SYSTEM.CNF` `VER` of the disc (the Public Beta's `1.00`; Spec II keeps it).
    pub system_version: String,
    /// The game program as it is on the disc. A disc is Spec II when its SCUS_974.36 has this identity.
    pub executable: Identity,
    /// The program after the injector words and the data-segment split: what the translator reads.
    pub derived: Identity,
    /// Known full images, by name. Another image with the pinned executable is a "Spec II 1.10 variant".
    pub known_images: Vec<KnownImage>,
    /// SHA-256 of the unmodified GT4 Online Public Beta's boot file (same serial, a loader), to name it in a refusal.
    pub beta_boot_sha256: String,
    pub patches: Vec<Spec2Patch>,
}

#[derive(Deserialize)]
struct IdentityJson {
    size_bytes: u64,
    sha256: String,
}

#[derive(Deserialize)]
struct SystemJson {
    boot_path: String,
    serial: String,
    version: String,
}

#[derive(Deserialize)]
struct IsoJson {
    files: BTreeMap<String, IdentityJson>,
    system: SystemJson,
}

#[derive(Deserialize)]
struct ElfJson {
    original: IdentityJson,
    derived: IdentityJson,
    patches_applied: usize,
}

#[derive(Deserialize)]
struct BetaJson {
    iso: BetaIsoJson,
}

#[derive(Deserialize)]
struct BetaIsoJson {
    boot_file_sha256: String,
}

#[derive(Deserialize)]
struct VariantsJson {
    executable: IdentityJson,
    images: Vec<NamedImageJson>,
}

#[derive(Deserialize)]
struct NamedImageJson {
    name: String,
    size_bytes: u64,
    sha256: String,
}

#[derive(Deserialize)]
struct PatchesJson {
    patches: Vec<PatchJson>,
}

#[derive(Deserialize)]
struct PatchJson {
    addr: u32,
    size: u32,
    old: i64,
    new: i64,
    /// Entries of another source than the injector are not applied (the script's rule; default `injector`).
    source: Option<String>,
}

impl Spec2Pins {
    /// The pins of Spec II 1.10 from the embedded `docs/inputs` files.
    pub fn embedded() -> Result<Spec2Pins, String> {
        let iso: IsoJson = serde_json::from_str(ISO_JSON).map_err(|error| format!("spec2-iso.json: {error}"))?;
        let elf: ElfJson = serde_json::from_str(ELF_JSON).map_err(|error| format!("spec2-elf.json: {error}"))?;
        let beta: BetaJson = serde_json::from_str(BETA_JSON).map_err(|error| format!("spec2-beta.json: {error}"))?;
        let variants: VariantsJson =
            serde_json::from_str(VARIANTS_JSON).map_err(|error| format!("spec2-variants.json: {error}"))?;
        let patch_file: PatchesJson =
            serde_json::from_str(PATCHES_JSON).map_err(|error| format!("spec2-patches.json: {error}"))?;
        let executable = iso.files.get("/SCUS_974.36;1").ok_or("spec2-iso.json has no SCUS_974.36 entry")?;
        if executable.sha256 != elf.original.sha256
            || executable.sha256 != variants.executable.sha256
            || executable.size_bytes != variants.executable.size_bytes
        {
            return Err("the Spec II executable pins (spec2-iso, spec2-elf, spec2-variants) disagree".to_string());
        }
        let patches: Vec<Spec2Patch> = patch_file
            .patches
            .iter()
            .filter(|patch| patch.source.as_deref().unwrap_or("injector") == "injector")
            .map(|patch| {
                let mask = if patch.size >= 8 { u64::MAX } else { (1u64 << (8 * patch.size)) - 1 };
                Spec2Patch { address: patch.addr, size: patch.size, old: patch.old as u64 & mask, new: patch.new as u64 & mask }
            })
            .collect();
        if patches.len() != elf.patches_applied {
            return Err(format!(
                "spec2-patches.json has {} injector words, spec2-elf.json says {}",
                patches.len(),
                elf.patches_applied
            ));
        }
        Ok(Spec2Pins {
            serial: iso.system.serial,
            boot_path: iso.system.boot_path,
            system_version: iso.system.version,
            executable: Identity { size_bytes: executable.size_bytes, sha256: executable.sha256.clone() },
            derived: Identity { size_bytes: elf.derived.size_bytes, sha256: elf.derived.sha256 },
            known_images: variants
                .images
                .into_iter()
                .map(|image| KnownImage {
                    name: image.name,
                    identity: Identity { size_bytes: image.size_bytes, sha256: image.sha256 },
                })
                .collect(),
            beta_boot_sha256: beta.iso.boot_file_sha256,
            patches,
        })
    }

    /// The name of a known full image, if `identity` is one.
    pub fn known_image(&self, identity: &Identity) -> Option<&str> {
        self.known_images.iter().find(|image| image.identity == *identity).map(|image| image.name.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_embedded_pins_agree_with_each_other() {
        let pins = Spec2Pins::embedded().unwrap();
        assert_eq!(pins.serial, "SCUS-97436");
        assert_eq!(pins.boot_path, "/SCUS_974.36;1");
        assert_eq!(pins.executable.size_bytes, 6_520_944);
        assert_eq!(pins.derived.sha256, "36f60b1a989b343e98ca55b1f07f051aa83a3fa1a4fcb4acb409adb148afc640");
        assert_eq!(pins.patches.len(), 35);
        assert_eq!(pins.known_images.len(), 3);
        let ten = pins.known_image(&Identity {
            size_bytes: 3_683_418_112,
            sha256: "6a3484b9672c10a92154ecee6faa5bbcfd4ed8cd538d8283e429c3fe6e1704b2".into(),
        });
        assert_eq!(ten, Some("Spec II 1.10"));
        let pt_br = pins.known_image(&Identity {
            size_bytes: 3_695_083_520,
            sha256: "fe8b51cf3008620260ff8945ac75d9b6fed4ab06813fa91ae5c9866529bf3352".into(),
        });
        assert_eq!(pt_br, Some("Spec II 1.10 (PT-BR)"));
        // TASK-167: the PT-BR image with Gran Turismo PSP cars (only GT4.VOL differs).
        let cpsp = pins.known_image(&Identity {
            size_bytes: 4_202_430_464,
            sha256: "96557472b44068e6926b51cc657ff997696c0314db11ec2ab3b8ab147f5954ed".into(),
        });
        assert_eq!(cpsp, Some("Spec II 1.10 (PT-BR + CPSP)"));
        assert_eq!(pins.known_image(&Identity { size_bytes: 1, sha256: "00".into() }), None);
    }
}
