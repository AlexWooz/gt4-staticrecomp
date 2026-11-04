//! Lists the graphics cards and resolves the settings of one run (release 1.3.0, TASK-108). The rules are pure and
//! tested in `gt4l_core::gpu`; this module reads the machine: the Vulkan probe (`system::probe_vulkan`) and, on Windows,
//! the DXGI adapters (`IDXGIFactory1::EnumAdapters1`).

use std::cell::Cell;
use std::sync::Mutex;

use gt4l_core::gpu::{self, DxgiGpu, GpuDevice, GraphicsPlan};
use gt4l_core::language::Language;
use gt4l_core::settings::Settings;
use gt4l_core::system_check::{self, VulkanProbe};

/// The graphics cards of this machine, as the launcher knows them now. A Vulkan device counts only when the game's
/// instance extensions are offered (`system_check::missing_instance_extensions`).
pub fn probe_gpus() -> Vec<GpuDevice> {
    probe_gpus_with_inputs().0
}

thread_local! {
    /// How many real probes ran on this thread. A test hook: a caller that must not probe checks that it stays put.
    static PROBES_ON_THREAD: Cell<usize> = const { Cell::new(0) };
}

/// The number of real GPU probes this thread has run (see [`PROBES_ON_THREAD`]).
pub fn probe_count() -> usize {
    PROBES_ON_THREAD.with(Cell::get)
}

/// The last probed card list, kept for the whole process (TASK-114 rc5). Probing creates a Vulkan instance and
/// enumerates DXGI, which is slow, so the Settings table, the saves and the log read this copy. Play refreshes it.
pub struct GpuCache {
    cards: Mutex<Option<Vec<GpuDevice>>>,
}

impl GpuCache {
    pub const fn new() -> Self {
        Self { cards: Mutex::new(None) }
    }

    /// The stored cards; `probe` runs only while nothing is stored yet.
    pub fn get_or_probe(&self, probe: impl FnOnce() -> Vec<GpuDevice>) -> Vec<GpuDevice> {
        let mut stored = self.lock();
        if stored.is_none() {
            *stored = Some(probe());
        }
        stored.clone().unwrap_or_default()
    }

    /// Runs `probe` and stores its list in place of the old one.
    pub fn refresh(&self, probe: impl FnOnce() -> Vec<GpuDevice>) -> Vec<GpuDevice> {
        let fresh = probe();
        *self.lock() = Some(fresh.clone());
        fresh
    }

    /// Stores a list that was already probed (the start-up probe), without probing again.
    pub fn store(&self, cards: Vec<GpuDevice>) {
        *self.lock() = Some(cards);
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Option<Vec<GpuDevice>>> {
        // A poisoned lock still holds a whole list: the cache is only ever replaced whole, so reading it is safe.
        self.cards.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

static GPU_CACHE: GpuCache = GpuCache::new();

/// The cards the launcher knows: probed on the first use, then kept.
pub fn cached_gpus() -> Vec<GpuDevice> {
    GPU_CACHE.get_or_probe(probe_gpus)
}

/// Probes again and keeps the new list. Play calls this once, so a run sees the hardware of this moment (an eGPU
/// plugged in after start, a driver update).
pub fn refresh_gpus() -> Vec<GpuDevice> {
    GPU_CACHE.refresh(probe_gpus)
}

/// Keeps a list that the start-up probe already made.
pub fn store_gpus(cards: Vec<GpuDevice>) {
    GPU_CACHE.store(cards);
}

/// [`probe_gpus`] and one log line for every raw entry (source API, PCI ids, LUID) the merge started from, so an owner log
/// shows why the list has the cards it has (rc5 laptop: one card too many).
pub fn probe_gpus_with_inputs() -> (Vec<GpuDevice>, Vec<String>) {
    PROBES_ON_THREAD.with(|count| count.set(count.get() + 1));
    let (vulkan, dxgi) = gpu_inputs();
    let mut lines: Vec<String> = vulkan.iter().map(gpu::describe_vulkan).collect();
    lines.extend(dxgi.iter().map(gpu::describe_dxgi));
    (gpu::merge(&vulkan, &dxgi), lines)
}

fn gpu_inputs() -> (Vec<gpu::VulkanGpu>, Vec<DxgiGpu>) {
    let vulkan = match super::system::probe_vulkan() {
        VulkanProbe::Ready { instance_extensions, devices }
            if system_check::missing_instance_extensions(&instance_extensions).is_empty() =>
        {
            devices
                .iter()
                .filter(|device| system_check::is_gpu_driver(device))
                .map(|device| gpu::VulkanGpu {
                    name: device.name.clone(),
                    api: (device.api_major, device.api_minor),
                    integrated: device.device_type == system_check::DEVICE_TYPE_INTEGRATED,
                    vendor_id: device.vendor_id,
                    device_id: device.device_id,
                    driver_version: device.driver_version,
                    luid: device.luid,
                })
                .collect()
        }
        _ => Vec::new(),
    };
    (vulkan, dxgi_adapters())
}

/// The hardware and software DXGI adapters (Windows). Software ones are flagged, and `gpu::merge` drops them.
#[cfg(windows)]
fn dxgi_adapters() -> Vec<DxgiGpu> {
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1, DXGI_ADAPTER_FLAG_SOFTWARE};

    let mut adapters = Vec::new();
    // SAFETY: plain DXGI calls. The factory and each adapter are COM objects that the windows crate releases on drop.
    unsafe {
        let Ok(factory) = CreateDXGIFactory1::<IDXGIFactory1>() else {
            return adapters;
        };
        for index in 0u32.. {
            let Ok(adapter) = factory.EnumAdapters1(index) else {
                break;
            };
            let Ok(desc) = adapter.GetDesc1() else {
                continue;
            };
            let length = desc.Description.iter().position(|&unit| unit == 0).unwrap_or(desc.Description.len());
            // The LUID is the adapter's own id on this machine: a 64-bit value from its two halves.
            let luid = (u64::from(desc.AdapterLuid.HighPart as u32) << 32) | u64::from(desc.AdapterLuid.LowPart);
            adapters.push(DxgiGpu {
                name: String::from_utf16_lossy(&desc.Description[..length]),
                dedicated_video_memory: desc.DedicatedVideoMemory as u64,
                software: (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32) != 0,
                vendor_id: desc.VendorId,
                device_id: desc.DeviceId,
                luid: Some(luid),
            });
        }
    }
    adapters
}

#[cfg(not(windows))]
fn dxgi_adapters() -> Vec<DxgiGpu> {
    Vec::new()
}

/// The name of the card a run would use when that card is integrated, else `None` (the 1x integrated default, TASK-105).
/// Reads the cached cards; see [`integrated_gpu_name_of`] for the rule over a known list.
pub fn integrated_gpu_name(stored_card: &str) -> Option<String> {
    integrated_gpu_name_of(stored_card, &cached_gpus())
}

/// [`integrated_gpu_name`] over a card list that is already known. Probes nothing.
pub fn integrated_gpu_name_of(stored_card: &str, cards: &[GpuDevice]) -> Option<String> {
    gpu::choose_card(stored_card, cards)
        .map(|index| &cards[index])
        .filter(|card| card.integrated)
        .and_then(|card| card.name().map(str::to_string))
}

/// The settings of one run (TASK-108) over the cached cards. See [`run_settings_with_cards`].
pub fn run_settings(settings: &Settings, language: Language) -> (Settings, GraphicsPlan, Vec<GpuDevice>) {
    run_settings_with_cards(settings, language, cached_gpus())
}

/// The settings of one run (TASK-108): the stored values with the API, the card and the Adapter resolved. Returns the
/// settings, the plan (for the log and the player's note) and the card list it used. Probes nothing.
pub fn run_settings_with_cards(settings: &Settings, language: Language, cards: Vec<GpuDevice>) -> (Settings, GraphicsPlan, Vec<GpuDevice>) {
    let plan = gpu::plan(&settings.gs_value("graphics_api"), settings.get("graphics_card"), &cards, language);
    let mut run = settings.clone();
    run.set_run_gs("graphics_api", Some(&plan.api));
    run.set_run_gs("adapter", (!plan.adapter.is_empty()).then_some(plan.adapter.as_str()));
    let card_name = plan.card.as_ref().and_then(GpuDevice::name).unwrap_or(gpu::AUTOMATIC).to_string();
    // A card name the settings cannot hold (a control character, or over 256 bytes) is left out of the run: the run
    // keeps the stored value, which only matters to the Settings page; the plan's API and Adapter are already set.
    let _ = run.set("graphics_card", &card_name);
    (run, plan, cards)
}

/// The card names for the log, in the order the machine lists them.
pub fn card_list(cards: &[GpuDevice]) -> String {
    if cards.is_empty() {
        return "none".to_string();
    }
    cards
        .iter()
        .map(|card| {
            let sources = match (card.vulkan_name.is_some(), card.dxgi_name.is_some()) {
                (true, true) => "vulkan+dxgi",
                (true, false) => "vulkan",
                (false, true) => "dxgi",
                (false, false) => "none",
            };
            format!(
                "{} ({sources}, vendor {:04x}, {})",
                card.name().unwrap_or("unnamed"),
                card.vendor_id,
                if card.integrated { "integrated" } else { "discrete" }
            )
        })
        .collect::<Vec<_>>()
        .join("; ")
}

#[cfg(test)]
mod cache_tests {
    use super::*;
    use std::cell::Cell;

    fn card(name: &str, integrated: bool) -> GpuDevice {
        GpuDevice {
            vulkan_name: Some(name.to_string()),
            vulkan_api: (1, 3),
            dxgi_name: None,
            dedicated_video_memory: 0,
            integrated,
            vendor_id: 0,
            driver_version: 0,
        }
    }

    #[test]
    fn cache_returns_the_stored_list_without_probing() {
        let cache = GpuCache::new();
        let probes = Cell::new(0);
        let first = cache.get_or_probe(|| {
            probes.set(probes.get() + 1);
            vec![card("Probed once", false)]
        });
        let second = cache.get_or_probe(|| {
            probes.set(probes.get() + 1);
            vec![card("Must not be used", false)]
        });
        assert_eq!(probes.get(), 1, "the second read must come from the cache");
        assert_eq!(first, second);
        assert_eq!(second[0].name(), Some("Probed once"));
    }

    #[test]
    fn refresh_replaces_the_stored_list() {
        let cache = GpuCache::new();
        cache.get_or_probe(|| vec![card("Before", false)]);
        let refreshed = cache.refresh(|| vec![card("After", true)]);
        assert_eq!(refreshed[0].name(), Some("After"));
        let later = cache.get_or_probe(|| panic!("the refreshed list is stored, nothing probes"));
        assert_eq!(later[0].name(), Some("After"));
    }

    #[test]
    fn store_keeps_a_startup_list_without_probing() {
        let cache = GpuCache::new();
        cache.store(vec![card("Startup", false)]);
        let read = cache.get_or_probe(|| panic!("a stored list must not be probed again"));
        assert_eq!(read[0].name(), Some("Startup"));
    }

    #[test]
    fn integrated_name_over_a_known_list_probes_nothing() {
        let cards = vec![card("Discrete", false), card("Integrated", true)];
        let before = probe_count();
        assert_eq!(integrated_gpu_name_of("Integrated", &cards).as_deref(), Some("Integrated"));
        assert_eq!(integrated_gpu_name_of("Discrete", &cards), None);
        assert_eq!(probe_count(), before);
    }

    #[test]
    fn run_settings_over_known_cards_probes_nothing() {
        let settings = Settings::default();
        let before = probe_count();
        let (_, _, used) = run_settings_with_cards(&settings, Language::fallback(), vec![card("Only", false)]);
        assert_eq!(used.len(), 1);
        assert_eq!(probe_count(), before, "a run over a known list must not probe");
    }
}

#[cfg(test)]
mod probe_on_this_machine {
    /// Prints the raw Vulkan devices and DXGI adapters with their PCI ids and LUIDs, then the merged cards. Ignored by
    /// default: it loads the GPU drivers. Run with `cargo test -p gt4l-platform -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn gpu_inputs_on_this_machine() {
        if let gt4l_core::system_check::VulkanProbe::Ready { devices, .. } = super::super::system::probe_vulkan() {
            for device in devices {
                println!("vulkan {} vendor {:#06x} device {:#06x} type {} luid {:?}", device.name, device.vendor_id,
                         device.device_id, device.device_type, device.luid.map(|value| format!("{value:#018x}")));
            }
        }
        for adapter in super::dxgi_adapters() {
            println!("dxgi {} vendor {:#06x} device {:#06x} software {} luid {:?}", adapter.name, adapter.vendor_id,
                     adapter.device_id, adapter.software, adapter.luid.map(|value| format!("{value:#018x}")));
        }
        println!("cards: {}", super::card_list(&super::probe_gpus()));
    }
}
