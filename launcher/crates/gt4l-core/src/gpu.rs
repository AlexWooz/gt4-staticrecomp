//! Which graphics card renders the game and which Graphics API it uses (release 1.3.0, TASK-108).
//!
//! The launcher lists the graphics cards itself: the Vulkan device names from its Vulkan probe and the DXGI adapter
//! descriptions (`gt4l-platform::gpu` reads both). This module holds only the rules, as pure functions over plain
//! data, so every case is tested with fake devices:
//!
//! - **Automatic card.** A discrete card over an integrated one; among equals the most dedicated video memory.
//!   Software and CPU adapters are never candidates.
//! - **Automatic API.** Vulkan when the chosen card reports Vulkan 1.2 or newer, else Direct3D 11 (a card without
//!   Vulkan almost never has Direct3D 12, and Direct3D 11 is the fastest of the rest).
//! - **Explicit choices win.** A stored API or card is used as stored. Explicit Vulkan on a card without Vulkan 1.2
//!   runs with Direct3D 11 for that run, with a note for the player. A stored card that is no longer present falls
//!   back to Automatic for that run, with a log line.
//! - **Adapter.** The game's `Adapter` key gets the chosen card's name exactly as the active API reports it: the
//!   Vulkan name for Vulkan, the DXGI description for Direct3D. No name for that API means an empty Adapter (the
//!   system default), with a log line.

use crate::language::Language;
use crate::messages;

/// The stored value of the Automatic choices (`graphics_api` and `graphics_card`).
pub const AUTOMATIC: &str = "auto";
/// The `graphics_api` values of the three APIs the game offers.
pub const VULKAN_API: &str = "vulkan";
pub const DIRECT3D_12_API: &str = "d3d12";
pub const DIRECT3D_11_API: &str = "d3d11";
/// The API a run uses when Vulkan is needed but the chosen card has no Vulkan 1.2.
pub const FALLBACK_API: &str = DIRECT3D_11_API;
/// The lowest Vulkan version the game accepts on its card (`system_check::MINIMUM_VULKAN`).
pub const MINIMUM_VULKAN: (u32, u32) = (1, 2);
/// DXGI only (no Vulkan device type to ask): less dedicated memory than this is shared with the system, so the card
/// counts as integrated.
const SHARED_MEMORY_LIMIT: u64 = 512 * 1024 * 1024;

/// A GPU with its own Vulkan driver, as the Vulkan probe reports it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VulkanGpu {
    pub name: String,
    pub api: (u32, u32),
    pub integrated: bool,
    /// PCI vendor and device ids (0 when unknown).
    pub vendor_id: u32,
    pub device_id: u32,
    /// The adapter LUID when the driver reports one (`VkPhysicalDeviceIDProperties::deviceLUIDValid`).
    pub luid: Option<u64>,
    /// The driver's own version number, raw (0 when unknown).
    pub driver_version: u32,
}

/// A hardware adapter as DXGI reports it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DxgiGpu {
    pub name: String,
    pub dedicated_video_memory: u64,
    /// `DXGI_ADAPTER_FLAG_SOFTWARE` (the Direct3D software rasterizer).
    pub software: bool,
    /// `DXGI_ADAPTER_DESC1::VendorId` and `DeviceId`, and the `AdapterLuid` (always reported by DXGI).
    pub vendor_id: u32,
    pub device_id: u32,
    pub luid: Option<u64>,
}

/// One graphics card: the Vulkan device and the DXGI adapter that are the same hardware (matched by name).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GpuDevice {
    pub vulkan_name: Option<String>,
    pub vulkan_api: (u32, u32),
    pub dxgi_name: Option<String>,
    pub dedicated_video_memory: u64,
    pub integrated: bool,
    /// PCI vendor id (0 when unknown), for the log bundle's system report.
    pub vendor_id: u32,
    /// The Vulkan driver's version number, raw (0 when the card has no Vulkan driver here).
    pub driver_version: u32,
}

impl GpuDevice {
    /// The name shown to the player and stored as `graphics_card`: the Vulkan name, else the DXGI description.
    pub fn name(&self) -> Option<&str> {
        self.vulkan_name.as_deref().or(self.dxgi_name.as_deref())
    }

    /// True when the card reports Vulkan 1.2 or newer (with its own Vulkan driver).
    pub fn has_vulkan(&self) -> bool {
        self.vulkan_name.is_some() && self.vulkan_api >= MINIMUM_VULKAN
    }
}

/// Software and CPU adapters, by name (the Vulkan CPU type and the DXGI software flag are checked by the platform).
pub fn is_software_name(name: &str) -> bool {
    let lower = name.to_lowercase();
    ["microsoft basic render driver", "microsoft direct3d12", "llvmpipe", "swiftshader"]
        .iter()
        .any(|software| lower.contains(software))
}

/// The alphanumeric, lower-case form of a name, so the Vulkan name and the DXGI description of one card match even
/// when the spacing or the "(R)" marks differ.
fn key(name: &str) -> String {
    name.chars().filter(|c| c.is_alphanumeric()).flat_map(char::to_lowercase).collect()
}

/// Joins the Vulkan devices and the DXGI adapters into one entry per physical card (TASK-101 item 3, rc3 owner log:
/// five entries for two cards). Rules:
/// - an exact duplicate within one API (the same device listed twice, as hybrid laptops can) is dropped;
/// - a Vulkan device and a DXGI adapter are the same card when their LUIDs are equal (both report one), else when their
///   PCI vendor and device ids are equal, else when their names match. Each DXGI adapter joins at most one Vulkan device,
///   so two identical cards with different LUIDs stay two;
/// - software adapters are dropped, and a card known only to DXGI is integrated when its dedicated memory is small.
pub fn merge(vulkan: &[VulkanGpu], dxgi: &[DxgiGpu]) -> Vec<GpuDevice> {
    let vulkan: Vec<&VulkanGpu> = without_repeats(
        vulkan.iter().filter(|gpu| !is_software_name(&gpu.name)).collect(),
        |a, b| {
            (a.name == b.name && a.vendor_id == b.vendor_id && a.device_id == b.device_id && a.luid == b.luid)
                || (a.integrated && b.integrated && same_ids_or_name(a.vendor_id, a.device_id, &a.name, b.vendor_id, b.device_id, &b.name))
        },
    );
    let dxgi: Vec<&DxgiGpu> = without_repeats(
        dxgi.iter().filter(|adapter| !adapter.software && !is_software_name(&adapter.name)).collect(),
        |a, b| {
            (a.name == b.name && a.vendor_id == b.vendor_id && a.device_id == b.device_id && a.luid == b.luid)
                || (a.dedicated_video_memory < SHARED_MEMORY_LIMIT
                    && b.dedicated_video_memory < SHARED_MEMORY_LIMIT
                    && same_ids_or_name(a.vendor_id, a.device_id, &a.name, b.vendor_id, b.device_id, &b.name))
        },
    );
    let mut matched = vec![false; dxgi.len()];
    // Pass 1: the strict rule (LUIDs when both have one) for every Vulkan device, so two identical cards each find their
    // own adapter first. Pass 2: a device still alone takes a left-over adapter with the same PCI ids (or name), since a
    // hybrid laptop's Vulkan LUID can differ from every LUID DXGI lists (rc4 owner log).
    let mut partners: Vec<Option<usize>> = vec![None; vulkan.len()];
    for (slot, gpu) in vulkan.iter().enumerate() {
        let mine = Identity { name: &gpu.name, vendor_id: gpu.vendor_id, device_id: gpu.device_id, luid: gpu.luid };
        partners[slot] = dxgi.iter().enumerate().position(|(index, adapter)| {
            !matched[index]
                && same_card(&mine, &Identity { name: &adapter.name, vendor_id: adapter.vendor_id, device_id: adapter.device_id, luid: adapter.luid })
        });
        if let Some(index) = partners[slot] {
            matched[index] = true;
        }
    }
    for (slot, gpu) in vulkan.iter().enumerate() {
        if partners[slot].is_some() {
            continue;
        }
        let mine = Identity { name: &gpu.name, vendor_id: gpu.vendor_id, device_id: gpu.device_id, luid: None };
        partners[slot] = dxgi.iter().enumerate().position(|(index, adapter)| {
            !matched[index]
                && same_card(&mine, &Identity { name: &adapter.name, vendor_id: adapter.vendor_id, device_id: adapter.device_id, luid: None })
        });
        if let Some(index) = partners[slot] {
            matched[index] = true;
        }
    }
    let mut cards = Vec::new();
    for (slot, gpu) in vulkan.iter().enumerate() {
        let (dxgi_name, memory) = match partners[slot] {
            Some(index) => (Some(dxgi[index].name.clone()), dxgi[index].dedicated_video_memory),
            None => (None, 0),
        };
        cards.push(GpuDevice {
            vulkan_name: Some(gpu.name.clone()),
            vulkan_api: gpu.api,
            dxgi_name,
            dedicated_video_memory: memory,
            integrated: gpu.integrated,
            vendor_id: gpu.vendor_id,
            driver_version: gpu.driver_version,
        });
    }
    // DXGI can list one physical card several times with different LUIDs (rc4: this machine's single RX 9060 XT shows up
    // as three adapters; the owner's hybrid laptop lists each card twice). A DXGI adapter left over after the LUID
    // matching is therefore a repeat when a Vulkan device or an earlier left-over adapter has the same PCI ids; only an
    // adapter no other entry explains (a card without Vulkan) becomes a card of its own.
    let mut kept_ids: Vec<(u32, u32)> =
        vulkan.iter().filter(|gpu| gpu.vendor_id != 0).map(|gpu| (gpu.vendor_id, gpu.device_id)).collect();
    let mut kept_names: Vec<String> = vulkan.iter().map(|gpu| key(&gpu.name)).collect();
    for (index, adapter) in dxgi.iter().enumerate() {
        if matched[index] {
            continue;
        }
        // A small-memory (integrated) adapter with the name of a kept card is a repeat even when its ids differ: a PC
        // never has two integrated GPUs of one name.
        let integrated_repeat = adapter.dedicated_video_memory < SHARED_MEMORY_LIMIT && kept_names.contains(&key(&adapter.name));
        let repeat = integrated_repeat
            || if adapter.vendor_id != 0 {
                kept_ids.contains(&(adapter.vendor_id, adapter.device_id))
            } else {
                kept_names.contains(&key(&adapter.name))
            };
        if repeat {
            matched[index] = true;
            continue;
        }
        kept_ids.push((adapter.vendor_id, adapter.device_id));
        kept_names.push(key(&adapter.name));
    }
    for (index, adapter) in dxgi.iter().enumerate() {
        if !matched[index] {
            cards.push(GpuDevice {
                vulkan_name: None,
                vulkan_api: (0, 0),
                dxgi_name: Some(adapter.name.clone()),
                dedicated_video_memory: adapter.dedicated_video_memory,
                integrated: adapter.dedicated_video_memory < SHARED_MEMORY_LIMIT,
                vendor_id: adapter.vendor_id,
                driver_version: 0,
            });
        }
    }
    cards
}

/// Equal PCI ids (both known), or, when neither has ids, equal names.
fn same_ids_or_name(vendor_a: u32, device_a: u32, name_a: &str, vendor_b: u32, device_b: u32, name_b: &str) -> bool {
    if vendor_a != 0 && vendor_b != 0 {
        vendor_a == vendor_b && device_a == device_b
    } else {
        vendor_a == 0 && vendor_b == 0 && key(name_a) == key(name_b)
    }
}

/// One line of the log for a Vulkan device as the probe reported it (before the merge).
pub fn describe_vulkan(gpu: &VulkanGpu) -> String {
    format!(
        "graphics input: vulkan {} pci {:04x}:{:04x} {} api {}.{} luid {}",
        gpu.name,
        gpu.vendor_id,
        gpu.device_id,
        if gpu.integrated { "integrated" } else { "other" },
        gpu.api.0,
        gpu.api.1,
        gpu.luid.map_or("-".to_string(), |luid| format!("{luid:#018x}"))
    )
}

/// One line of the log for a DXGI adapter as DXGI reported it (before the merge).
pub fn describe_dxgi(adapter: &DxgiGpu) -> String {
    format!(
        "graphics input: dxgi {} pci {:04x}:{:04x} memory {} MB{} luid {}",
        adapter.name,
        adapter.vendor_id,
        adapter.device_id,
        adapter.dedicated_video_memory / (1024 * 1024),
        if adapter.software { " software" } else { "" },
        adapter.luid.map_or("-".to_string(), |luid| format!("{luid:#018x}"))
    )
}

/// What tells one device entry (a Vulkan device or a DXGI adapter) from another: its name, PCI ids and LUID.
struct Identity<'a> {
    name: &'a str,
    vendor_id: u32,
    device_id: u32,
    luid: Option<u64>,
}

/// True when two entries are the same physical card. LUIDs decide when both have one; else the PCI ids (when both are
/// known); else the names.
fn same_card(a: &Identity, b: &Identity) -> bool {
    match (a.luid, b.luid) {
        (Some(left), Some(right)) => left == right,
        _ if a.vendor_id != 0 && b.vendor_id != 0 => a.vendor_id == b.vendor_id && a.device_id == b.device_id,
        _ => key(a.name) == key(b.name),
    }
}

/// The entries with the first of every group of exact duplicates (`same`), in their order.
fn without_repeats<T>(items: Vec<&T>, same: impl Fn(&T, &T) -> bool) -> Vec<&T> {
    let mut kept: Vec<&T> = Vec::new();
    for item in items {
        if !kept.iter().any(|known| same(known, item)) {
            kept.push(item);
        }
    }
    kept
}

/// The index of the Automatic card: a discrete card before an integrated one, then the most dedicated memory. The
/// first card wins a tie. `None` when no card has a name.
pub fn automatic_index(cards: &[GpuDevice]) -> Option<usize> {
    let mut best: Option<usize> = None;
    for (index, card) in cards.iter().enumerate() {
        if card.name().is_none() {
            continue;
        }
        let better = match best {
            None => true,
            Some(current) => {
                let known = &cards[current];
                (!card.integrated, card.dedicated_video_memory) > (!known.integrated, known.dedicated_video_memory)
            }
        };
        if better {
            best = Some(index);
        }
    }
    best
}

/// The index of the card with exactly this name (`GpuDevice::name`), or `None` when it is not present.
pub fn find_index(cards: &[GpuDevice], name: &str) -> Option<usize> {
    cards.iter().position(|card| card.name() == Some(name))
}

/// The card a run uses: the stored one when present, else (stored Automatic or a missing card) the Automatic card.
pub fn choose_card(stored_card: &str, cards: &[GpuDevice]) -> Option<usize> {
    let stored = stored_card.trim();
    if stored.is_empty() || stored == AUTOMATIC {
        return automatic_index(cards);
    }
    find_index(cards, stored).or_else(|| automatic_index(cards))
}

/// The label of an API in the log and the Automatic choice (product names, not translated).
pub fn api_name(api: &str) -> &'static str {
    match api {
        DIRECT3D_12_API => "Direct3D 12",
        DIRECT3D_11_API => "Direct3D 11",
        _ => "Vulkan",
    }
}

/// The API and card of one run, with the Adapter value and what to log.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GraphicsPlan {
    /// The card the run uses; `None` when the machine has none.
    pub card: Option<GpuDevice>,
    /// `vulkan`, `d3d12` or `d3d11`.
    pub api: String,
    /// The `Adapter` value to write; empty means the system default.
    pub adapter: String,
    /// The note for the player when Vulkan was chosen but the card has no Vulkan 1.2; `None` otherwise.
    pub notice: Option<String>,
    /// Lines for the launcher log, in the order they happened.
    pub log: Vec<String>,
}

/// Plans one run from the stored `graphics_api` and `graphics_card` values and the cards the machine has.
pub fn plan(stored_api: &str, stored_card: &str, cards: &[GpuDevice], language: Language) -> GraphicsPlan {
    let mut log = Vec::new();
    let stored = stored_card.trim();
    if !stored.is_empty() && stored != AUTOMATIC && find_index(cards, stored).is_none() {
        log.push(format!("graphics card {stored} is not present; Automatic for this run"));
    }
    let card = choose_card(stored_card, cards).map(|index| cards[index].clone());
    let card_name = card.as_ref().and_then(GpuDevice::name).unwrap_or("none").to_string();
    let vulkan_ok = card.as_ref().is_some_and(GpuDevice::has_vulkan);
    let mut notice = None;
    let api = match stored_api.trim() {
        "" | AUTOMATIC => {
            if vulkan_ok {
                VULKAN_API.to_string()
            } else {
                FALLBACK_API.to_string()
            }
        }
        VULKAN_API if !vulkan_ok => {
            log.push(format!("graphics: {card_name} has no Vulkan 1.2; this run uses Direct3D 11"));
            notice = Some(messages::text("system.vulkan_fallback", language).to_string());
            FALLBACK_API.to_string()
        }
        other => other.to_string(),
    };
    let adapter = match &card {
        None => String::new(),
        Some(card) => {
            let name = if api == VULKAN_API { card.vulkan_name.clone() } else { card.dxgi_name.clone() };
            match name {
                Some(name) => name,
                None => {
                    log.push(format!(
                        "graphics: {card_name} has no name for {}; Adapter left empty (system default)",
                        api_name(&api)
                    ));
                    String::new()
                }
            }
        }
    };
    GraphicsPlan { card, api, adapter, notice, log }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vulkan(name: &str, api: (u32, u32), integrated: bool) -> VulkanGpu {
        VulkanGpu { name: name.into(), api, integrated, vendor_id: 0, device_id: 0, luid: None, driver_version: 0 }
    }

    fn dxgi(name: &str, memory: u64) -> DxgiGpu {
        DxgiGpu { name: name.into(), dedicated_video_memory: memory, software: false, vendor_id: 0, device_id: 0, luid: None }
    }

    /// A card with its PCI ids and LUID, as both APIs report it.
    fn card_vulkan(name: &str, vendor: u32, device: u32, luid: u64) -> VulkanGpu {
        VulkanGpu { name: name.into(), api: (1, 3), integrated: false, vendor_id: vendor, device_id: device, luid: Some(luid), driver_version: 0 }
    }

    fn card_dxgi(name: &str, vendor: u32, device: u32, luid: u64, memory: u64) -> DxgiGpu {
        DxgiGpu { name: name.into(), dedicated_video_memory: memory, software: false, vendor_id: vendor, device_id: device, luid: Some(luid) }
    }

    const GIB: u64 = 1024 * 1024 * 1024;

    /// A laptop: Intel integrated graphics and an NVIDIA card, both with Vulkan 1.3 and DXGI names.
    fn laptop() -> Vec<GpuDevice> {
        merge(
            &[vulkan("Intel(R) UHD Graphics", (1, 3), true), vulkan("NVIDIA GeForce RTX 4060", (1, 3), false)],
            &[dxgi("Intel(R) UHD Graphics", 128 * 1024 * 1024), dxgi("NVIDIA GeForce RTX 4060", 8 * GIB)],
        )
    }

    fn english() -> Language {
        Language::fallback()
    }

    #[test]
    fn a_laptop_with_integrated_and_nvidia_picks_the_nvidia_card_on_vulkan() {
        let cards = laptop();
        let run = plan("auto", "auto", &cards, english());
        assert_eq!(run.card.as_ref().and_then(GpuDevice::name), Some("NVIDIA GeForce RTX 4060"));
        assert_eq!(run.api, "vulkan");
        assert_eq!(run.adapter, "NVIDIA GeForce RTX 4060");
        assert!(run.notice.is_none());
    }

    #[test]
    fn an_integrated_only_machine_without_vulkan_runs_direct3d_11_on_its_integrated_card() {
        let cards = merge(&[], &[dxgi("AMD Radeon(TM) Graphics", 512 * 1024 * 1024 - 1)]);
        let run = plan("auto", "auto", &cards, english());
        assert_eq!(run.api, "d3d11");
        assert_eq!(run.adapter, "AMD Radeon(TM) Graphics");
        assert!(run.card.as_ref().unwrap().integrated, "the 1x integrated default applies to the chosen card");
        assert!(run.notice.is_none(), "Automatic falls back without a note");
    }

    #[test]
    fn two_discrete_cards_pick_the_one_with_more_video_memory() {
        let cards = merge(
            &[vulkan("Radeon RX 6600", (1, 3), false), vulkan("Radeon RX 9060 XT", (1, 4), false)],
            &[dxgi("Radeon RX 6600", 8 * GIB), dxgi("Radeon RX 9060 XT", 16 * GIB)],
        );
        assert_eq!(plan("auto", "auto", &cards, english()).adapter, "Radeon RX 9060 XT");
        let cards = merge(
            &[vulkan("Radeon A", (1, 3), false), vulkan("Radeon B", (1, 3), false)],
            &[dxgi("Radeon A", 4 * GIB), dxgi("Radeon B", 12 * GIB)],
        );
        assert_eq!(automatic_index(&cards).map(|index| cards[index].name()), Some(Some("Radeon B")));
    }

    #[test]
    fn an_explicit_card_and_api_are_kept() {
        let cards = laptop();
        let run = plan("d3d12", "Intel(R) UHD Graphics", &cards, english());
        assert_eq!(run.card.as_ref().and_then(GpuDevice::name), Some("Intel(R) UHD Graphics"));
        assert_eq!(run.api, "d3d12");
        assert_eq!(run.adapter, "Intel(R) UHD Graphics", "the DXGI description of the chosen card");
        assert!(run.log.is_empty());
    }

    #[test]
    fn a_stored_card_that_is_gone_falls_back_to_automatic_with_a_log_line() {
        let cards = laptop();
        let run = plan("auto", "NVIDIA eGPU (unplugged)", &cards, english());
        assert_eq!(run.card.as_ref().and_then(GpuDevice::name), Some("NVIDIA GeForce RTX 4060"));
        assert!(run.log.iter().any(|line| line.contains("NVIDIA eGPU (unplugged) is not present")), "{:?}", run.log);
    }

    #[test]
    fn software_adapters_are_never_candidates() {
        let cards = merge(
            &[vulkan("llvmpipe (LLVM 19.1)", (1, 3), true)],
            &[
                DxgiGpu { name: "Microsoft Basic Render Driver".into(), dedicated_video_memory: 0, software: true, vendor_id: 0x1414, device_id: 0x8c, luid: None },
                dxgi("Intel UHD", 128 * 1024 * 1024),
            ],
        );
        assert_eq!(cards.len(), 1, "the software rasterizer and the basic driver are dropped");
        assert_eq!(cards[0].name(), Some("Intel UHD"));
        assert_eq!(automatic_index(&[]), None);
        assert_eq!(plan("auto", "auto", &[], english()).adapter, "", "no card means the system default");
    }

    #[test]
    fn a_name_mismatch_between_vulkan_and_dxgi_leaves_the_adapter_empty_for_direct3d() {
        // Vulkan calls the card one thing, DXGI another: they are two cards, and the Vulkan-only one has no DXGI name.
        let cards = merge(&[vulkan("Intel(R) Graphics (Arrow Lake)", (1, 3), true)], &[dxgi("Intel Arc Graphics", 0)]);
        assert_eq!(cards.len(), 2);
        let run = plan("d3d11", "Intel(R) Graphics (Arrow Lake)", &cards, english());
        assert_eq!(run.adapter, "", "no DXGI name for this card");
        assert!(run.log.iter().any(|line| line.contains("has no name for Direct3D 11")), "{:?}", run.log);
        // The same card on Vulkan keeps its Vulkan name.
        assert_eq!(plan("vulkan", "Intel(R) Graphics (Arrow Lake)", &cards, english()).adapter, "Intel(R) Graphics (Arrow Lake)");
    }

    #[test]
    fn explicit_vulkan_on_a_card_without_vulkan_12_runs_direct3d_11_with_the_note() {
        let cards = merge(&[vulkan("Intel HD 4000", (1, 1), true)], &[dxgi("Intel HD 4000", 128 * 1024 * 1024)]);
        let run = plan("vulkan", "auto", &cards, english());
        assert_eq!(run.api, "d3d11");
        assert_eq!(run.adapter, "Intel HD 4000");
        assert_eq!(
            run.notice.as_deref(),
            Some("No Vulkan 1.2 graphics card was found, so the game uses Direct3D 11. You can change this in Settings > Graphics.")
        );
        let portuguese = Language::from_tag("pt-BR").expect("pt-BR is a language");
        assert_eq!(
            plan("vulkan", "auto", &cards, portuguese).notice.as_deref(),
            Some("Nenhuma placa de vídeo com Vulkan 1.2 foi encontrada, então o jogo usa Direct3D 11. Você pode mudar isso em Configurações > Gráficos.")
        );
    }

    #[test]
    fn explicit_vulkan_with_no_card_at_all_runs_direct3d_11_without_a_card() {
        let run = plan("vulkan", "auto", &[], english());
        assert_eq!(run.api, "d3d11");
        assert_eq!(run.adapter, "");
        assert!(run.notice.is_some());
    }

    #[test]
    fn the_five_entries_of_the_rc3_log_are_two_cards() {
        // rc3 laptop (RTX 3060 Laptop + Intel UHD): the log listed NVIDIA, Intel, Intel, NVIDIA, Intel. Vulkan repeats
        // a device and DXGI repeats one too; the LUIDs tell the repeats apart from the other card.
        let nvidia_luid = 0x0000_1234_0000_5678;
        let intel_luid = 0x0000_1234_0000_9abc;
        let cards = merge(
            &[
                card_vulkan("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2520, nvidia_luid),
                card_vulkan("Intel(R) UHD Graphics", 0x8086, 0x9bc8, intel_luid),
                card_vulkan("Intel(R) UHD Graphics", 0x8086, 0x9bc8, intel_luid),
                card_vulkan("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2520, nvidia_luid),
            ],
            &[
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc8, intel_luid, 128 * 1024 * 1024),
                card_dxgi("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2520, nvidia_luid, 6 * GIB),
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc8, intel_luid, 128 * 1024 * 1024),
            ],
        );
        let names: Vec<&str> = cards.iter().map(|card| card.name().unwrap()).collect();
        assert_eq!(names, vec!["NVIDIA GeForce RTX 3060 Laptop GPU", "Intel(R) UHD Graphics"]);
        // Each card keeps both API names.
        assert_eq!(cards[0].dxgi_name.as_deref(), Some("NVIDIA GeForce RTX 3060 Laptop GPU"));
        assert_eq!(cards[0].dedicated_video_memory, 6 * GIB);
        assert_eq!(cards[1].dxgi_name.as_deref(), Some("Intel(R) UHD Graphics"));
        // And the automatic choice is the same card as before.
        assert_eq!(automatic_index(&cards), Some(0));
    }

    #[test]
    fn two_identical_cards_with_different_luids_stay_two() {
        let cards = merge(
            &[card_vulkan("NVIDIA GeForce RTX 4090", 0x10de, 0x2684, 1), card_vulkan("NVIDIA GeForce RTX 4090", 0x10de, 0x2684, 2)],
            &[card_dxgi("NVIDIA GeForce RTX 4090", 0x10de, 0x2684, 1, 24 * GIB), card_dxgi("NVIDIA GeForce RTX 4090", 0x10de, 0x2684, 2, 24 * GIB)],
        );
        assert_eq!(cards.len(), 2, "two cards with two LUIDs");
        assert!(cards.iter().all(|card| card.vulkan_name.is_some() && card.dxgi_name.is_some()), "each keeps both names");
    }

    #[test]
    fn dxgi_repeats_of_one_card_with_other_luids_are_one_card() {
        // rc4, the main agent's desktop: one RX 9060 XT; Vulkan reports LUID 0x137b4, DXGI lists the card three times
        // (LUIDs 0x137b4, 0x268fc, 0x25746) plus the software adapter.
        let cards = merge(
            &[card_vulkan("AMD Radeon RX 9060 XT", 0x1002, 0x7590, 0x137b4)],
            &[
                card_dxgi("AMD Radeon RX 9060 XT", 0x1002, 0x7590, 0x137b4, 16 * GIB),
                card_dxgi("AMD Radeon RX 9060 XT", 0x1002, 0x7590, 0x268fc, 16 * GIB),
                card_dxgi("AMD Radeon RX 9060 XT", 0x1002, 0x7590, 0x25746, 16 * GIB),
            ],
        );
        assert_eq!(cards.len(), 1, "{cards:?}");
        assert_eq!(cards[0].dxgi_name.as_deref(), Some("AMD Radeon RX 9060 XT"));
    }

    #[test]
    fn a_hybrid_laptop_whose_vulkan_luids_match_no_dxgi_luid_gives_two_cards() {
        // rc4, the owner's laptop log: four entries for two cards; no Vulkan LUID matched a DXGI LUID.
        let cards = merge(
            &[
                card_vulkan("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2560, 0x1_0000_0001),
                card_vulkan("Intel(R) UHD Graphics", 0x8086, 0x9bc4, 0x1_0000_0002),
            ],
            &[
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc4, 0x2_0000_0001, 128 * 1024 * 1024),
                card_dxgi("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2560, 0x2_0000_0002, 6 * GIB),
            ],
        );
        assert_eq!(cards.len(), 2, "{cards:?}");
        assert!(cards.iter().all(|card| card.vulkan_name.is_some() && card.dxgi_name.is_some()), "each keeps both names");
        assert_eq!(cards[0].dedicated_video_memory, 6 * GIB, "the NVIDIA card got its own DXGI adapter");
    }

    #[test]
    fn a_card_without_vulkan_listed_twice_by_dxgi_is_one_card() {
        let cards = merge(
            &[],
            &[
                card_dxgi("Intel(R) HD Graphics 4600", 0x8086, 0x0412, 1, 128 * 1024 * 1024),
                card_dxgi("Intel(R) HD Graphics 4600", 0x8086, 0x0412, 2, 128 * 1024 * 1024),
            ],
        );
        assert_eq!(cards.len(), 1, "{cards:?}");
    }

    #[test]
    fn identical_entries_without_luids_are_one_card() {
        // Without a LUID two identical cards cannot be told apart, so exact repeats count as one card. DXGI always
        // reports a LUID, so this only applies to Vulkan-only data.
        let cards = merge(
            &[vulkan("Radeon RX 7800 XT", (1, 3), false), vulkan("Radeon RX 7800 XT", (1, 3), false)],
            &[
                DxgiGpu { vendor_id: 0x1002, device_id: 0x747e, ..dxgi("Radeon RX 7800 XT", 16 * GIB) },
                DxgiGpu { vendor_id: 0x1002, device_id: 0x747e, ..dxgi("Radeon RX 7800 XT", 16 * GIB) },
            ],
        );
        assert_eq!(cards.len(), 1);
        assert!(cards[0].dxgi_name.is_some());
    }

    #[test]
    fn a_dxgi_adapter_joins_at_most_one_vulkan_device() {
        // Two different cards that only share a name and ids (no LUID): each DXGI adapter joins one Vulkan device.
        let cards = merge(
            &[
                VulkanGpu { vendor_id: 0x1002, device_id: 0x747e, ..vulkan("Radeon RX 7800 XT", (1, 3), false) },
                VulkanGpu { vendor_id: 0x1002, device_id: 0x7480, ..vulkan("Radeon RX 7800 XT", (1, 3), false) },
            ],
            &[DxgiGpu { vendor_id: 0x1002, device_id: 0x747e, ..dxgi("Radeon RX 7800 XT", 16 * GIB) }],
        );
        assert_eq!(cards.len(), 2);
        assert_eq!(cards.iter().filter(|card| card.dxgi_name.is_some()).count(), 1);
    }

    #[test]
    fn the_names_of_one_card_match_across_vulkan_and_dxgi() {
        let cards = merge(&[vulkan("NVIDIA GeForce RTX 4060", (1, 3), false)], &[dxgi("NVIDIA GeForce RTX 4060 ", 8 * GIB)]);
        assert_eq!(cards.len(), 1, "a DXGI description with a trailing space is the same card");
        assert_eq!(cards[0].dedicated_video_memory, 8 * GIB);
        assert_eq!(cards[0].dxgi_name.as_deref(), Some("NVIDIA GeForce RTX 4060 "));
    }

    fn integrated_vulkan(luid: u64) -> VulkanGpu {
        VulkanGpu { integrated: true, ..card_vulkan("Intel(R) UHD Graphics", 0x8086, 0x9bc4, luid) }
    }

    #[test]
    fn vulkan_listing_the_integrated_intel_twice_with_different_luids_gives_two_cards_in_total() {
        // rc5 laptop (RTX 3060 Laptop + Intel UHD 630): three entries for two GPUs.
        let cards = merge(
            &[card_vulkan("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2520, 0x100), integrated_vulkan(0x200), integrated_vulkan(0x300)],
            &[
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc4, 0x200, 128 * 1024 * 1024),
                card_dxgi("NVIDIA GeForce RTX 3060 Laptop GPU", 0x10de, 0x2520, 0x100, 6 * GIB),
            ],
        );
        assert_eq!(cards.len(), 2, "{cards:?}");
    }

    #[test]
    fn two_identical_integrated_entries_with_different_names_of_ids_only_merge_by_ids() {
        let cards = merge(&[integrated_vulkan(1), VulkanGpu { name: "Intel UHD".into(), ..integrated_vulkan(2) }], &[]);
        assert_eq!(cards.len(), 1);
    }

    #[test]
    fn dxgi_listing_a_small_memory_adapter_twice_with_other_ids_is_one_card() {
        let cards = merge(
            &[integrated_vulkan(1)],
            &[
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc4, 7, 128 * 1024 * 1024),
                card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x3e9b, 8, 128 * 1024 * 1024),
            ],
        );
        assert_eq!(cards.len(), 1, "{cards:?}");
    }

    #[test]
    fn two_identical_discrete_cards_with_different_luids_stay_two_with_equal_ids() {
        let cards = merge(
            &[
                card_vulkan("Radeon RX 7800 XT", 0x1002, 0x747e, 1),
                card_vulkan("Radeon RX 7800 XT", 0x1002, 0x747e, 2),
            ],
            &[
                card_dxgi("Radeon RX 7800 XT", 0x1002, 0x747e, 1, 16 * GIB),
                card_dxgi("Radeon RX 7800 XT", 0x1002, 0x747e, 2, 16 * GIB),
            ],
        );
        assert_eq!(cards.len(), 2, "{cards:?}");
    }

    #[test]
    fn the_input_lines_name_the_api_ids_and_luid() {
        let line = describe_vulkan(&integrated_vulkan(0x200));
        assert!(line.contains("vulkan") && line.contains("8086:9bc4") && line.contains("integrated") && line.contains("0x0000000000000200"), "{line}");
        let line = describe_dxgi(&card_dxgi("Intel(R) UHD Graphics", 0x8086, 0x9bc4, 0x200, 128 * 1024 * 1024));
        assert!(line.contains("dxgi") && line.contains("memory 128 MB"), "{line}");
    }
}
