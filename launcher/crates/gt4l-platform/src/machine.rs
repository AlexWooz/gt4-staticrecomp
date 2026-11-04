//! The machine facts of the system report (`system.txt`): processor, memory, operating system, Wine, storage and power.
//! Every read is made so it cannot fail the log bundle: what cannot be read stays `None` or empty and the report says
//! "unknown". The rules that turn numbers into text are pure and tested in `gt4l_core::log_export`.
//!
//! Privacy: no computer name, user name, serial number or MAC address is read, and the storage fact keeps the drive
//! letter only, never a path.

use std::path::Path;

use gt4l_core::log_export::MachineFacts;

/// The facts, read now. `data_folder` is the game data folder: only the drive that holds it is reported.
pub fn read(data_folder: &Path) -> MachineFacts {
    let mut machine = MachineFacts::default();
    #[cfg(target_arch = "x86_64")]
    {
        machine.sse4_1 = Some(std::arch::is_x86_feature_detected!("sse4.1"));
        machine.avx2 = Some(std::arch::is_x86_feature_detected!("avx2"));
    }
    machine.cpu_model = cpu_model();
    machine.cpu_threads = threads();
    #[cfg(windows)]
    {
        windows_impl::fill(&mut machine, data_folder);
    }
    #[cfg(not(windows))]
    {
        let _ = data_folder;
    }
    machine
}

/// The brand string from CPUID leaves 0x80000002-4 (the registry's `ProcessorNameString` when the leaves are missing).
fn cpu_model() -> String {
    #[cfg(target_arch = "x86_64")]
    {
        use std::arch::x86_64::__cpuid;
        // SAFETY: CPUID is always present on x86-64; leaves above the maximum are checked first.
        let brand = unsafe {
            if __cpuid(0x8000_0000).eax >= 0x8000_0004 {
                let mut bytes = Vec::with_capacity(48);
                for leaf in 0x8000_0002u32..=0x8000_0004 {
                    let registers = __cpuid(leaf);
                    for value in [registers.eax, registers.ebx, registers.ecx, registers.edx] {
                        bytes.extend_from_slice(&value.to_le_bytes());
                    }
                }
                gt4l_core::log_export::clean_cpu_brand(&bytes)
            } else {
                String::new()
            }
        };
        if !brand.is_empty() {
            return brand;
        }
    }
    #[cfg(windows)]
    {
        if let Some(name) = crate::system::registry_string(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString") {
            return gt4l_core::log_export::clean_cpu_brand(name.as_bytes());
        }
    }
    String::new()
}

fn threads() -> Option<u32> {
    #[cfg(windows)]
    {
        if let Some(count) = windows_impl::active_processors() {
            return Some(count);
        }
    }
    std::thread::available_parallelism().ok().map(|count| count.get() as u32)
}

#[cfg(windows)]
mod windows_impl {
    use std::ffi::{c_char, CStr};
    use std::path::Path;

    use gt4l_core::log_export::{self, MachineFacts, PowerFact, StorageFact, WineFacts};
    use windows::core::{s, w, HSTRING, PCWSTR};
    use windows::Win32::Storage::FileSystem::GetDiskFreeSpaceExW;
    use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress};
    use windows::Win32::System::Threading::GetActiveProcessorCount;
    use windows::Win32::System::Power::{GetSystemPowerStatus, SYSTEM_POWER_STATUS};
    use windows::Win32::System::SystemInformation::{
        GetLogicalProcessorInformationEx, GlobalMemoryStatusEx, RelationProcessorCore, MEMORYSTATUSEX,
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
    };

    /// `ALL_PROCESSOR_GROUPS`.
    const ALL_PROCESSOR_GROUPS: u16 = 0xFFFF;

    pub fn active_processors() -> Option<u32> {
        // SAFETY: a plain query with no pointers.
        let count = unsafe { GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) };
        (count > 0).then_some(count)
    }

    pub fn fill(machine: &mut MachineFacts, data_folder: &Path) {
        machine.cpu_cores = physical_cores();
        let (total, available) = memory();
        machine.ram_total = total;
        machine.ram_available = available;
        let version = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
        machine.os_edition = crate::system::registry_string(version, "EditionID").unwrap_or_default();
        machine.os_architecture = std::env::var("PROCESSOR_ARCHITEW6432")
            .or_else(|_| std::env::var("PROCESSOR_ARCHITECTURE"))
            .unwrap_or_default();
        machine.wine = wine();
        machine.storage = storage(data_folder);
        machine.power = power();
    }

    /// The physical cores: one record of `RelationProcessorCore` per core.
    fn physical_cores() -> Option<u32> {
        let mut length = 0u32;
        // SAFETY: the first call only asks for the buffer size (it fails with ERROR_INSUFFICIENT_BUFFER on purpose).
        let _ = unsafe { GetLogicalProcessorInformationEx(RelationProcessorCore, None, &mut length) };
        if length == 0 || length > 1 << 20 {
            return None;
        }
        // u64 storage keeps the buffer aligned for the records the system writes.
        let mut buffer = vec![0u64; (length as usize).div_ceil(8)];
        // SAFETY: the buffer holds at least `length` bytes.
        let read = unsafe {
            GetLogicalProcessorInformationEx(
                RelationProcessorCore,
                Some(buffer.as_mut_ptr().cast::<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>()),
                &mut length,
            )
        };
        read.ok()?;
        // SAFETY: `length` bytes of the u64 buffer were written; viewing them as bytes is always valid.
        let bytes = unsafe { std::slice::from_raw_parts(buffer.as_ptr().cast::<u8>(), (length as usize).min(buffer.len() * 8)) };
        log_export::count_core_records(bytes).filter(|cores| *cores > 0)
    }

    fn memory() -> (Option<u64>, Option<u64>) {
        let mut status = MEMORYSTATUSEX { dwLength: std::mem::size_of::<MEMORYSTATUSEX>() as u32, ..Default::default() };
        // SAFETY: `status` has dwLength set, as the call requires.
        match unsafe { GlobalMemoryStatusEx(&mut status) } {
            Ok(()) => (Some(status.ullTotalPhys), Some(status.ullAvailPhys)),
            Err(_) => (None, None),
        }
    }

    /// Wine's `ntdll.dll` exports `wine_get_version` and `wine_get_host_version`; Windows has neither.
    fn wine() -> Option<WineFacts> {
        // SAFETY: ntdll is always loaded; the exports, when present, have these signatures in Wine's ntdll.
        unsafe {
            let ntdll = GetModuleHandleW(w!("ntdll.dll")).ok()?;
            let version = GetProcAddress(ntdll, s!("wine_get_version"))?;
            let version: unsafe extern "C" fn() -> *const c_char = std::mem::transmute(version);
            let text = |pointer: *const c_char| {
                if pointer.is_null() {
                    String::new()
                } else {
                    CStr::from_ptr(pointer).to_string_lossy().into_owned()
                }
            };
            let version = text(version());
            let mut host = String::new();
            if let Some(host_version) = GetProcAddress(ntdll, s!("wine_get_host_version")) {
                let host_version: unsafe extern "C" fn(*mut *const c_char, *mut *const c_char) = std::mem::transmute(host_version);
                let (mut system, mut release) = (std::ptr::null(), std::ptr::null());
                host_version(&mut system, &mut release);
                host = format!("{} {}", text(system), text(release)).trim().to_string();
            }
            Some(WineFacts { version, host })
        }
    }

    /// Free and total space of the drive that holds `folder`. Only the drive letter is kept.
    fn storage(folder: &Path) -> Option<StorageFact> {
        let text = folder.display().to_string();
        let plain = text.strip_prefix(r"\\?\").unwrap_or(&text);
        let drive = log_export::drive_of(plain)?;
        let root = HSTRING::from(format!("{drive}\\"));
        let (mut free, mut total) = (0u64, 0u64);
        // SAFETY: the root is null-terminated by HSTRING; the out-parameters live for the call.
        unsafe { GetDiskFreeSpaceExW(PCWSTR(root.as_ptr()), Some(&mut free), Some(&mut total), None) }.ok()?;
        Some(StorageFact { drive, free, total })
    }

    fn power() -> Option<PowerFact> {
        let mut status = SYSTEM_POWER_STATUS::default();
        // SAFETY: a plain out-parameter.
        unsafe { GetSystemPowerStatus(&mut status) }.ok()?;
        const NO_BATTERY: u8 = 128;
        const UNKNOWN: u8 = 255;
        Some(PowerFact {
            on_ac: match status.ACLineStatus {
                0 => Some(false),
                1 => Some(true),
                _ => None,
            },
            has_battery: status.BatteryFlag != UNKNOWN && status.BatteryFlag & NO_BATTERY == 0,
            battery_percent: (status.BatteryLifePercent <= 100).then_some(status.BatteryLifePercent),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Reads this machine. The values are the machine's own, so only the shape is checked; `--nocapture` prints them.
    #[test]
    fn reading_this_machine_gives_plausible_facts_and_never_panics() {
        let machine = read(Path::new("C:\\"));
        println!("{machine:#?}");
        assert!(machine.cpu_threads.is_some_and(|threads| threads >= 1));
        assert!(machine.cpu_model.is_empty() || machine.cpu_model == machine.cpu_model.trim());
        if let (Some(cores), Some(threads)) = (machine.cpu_cores, machine.cpu_threads) {
            assert!(cores >= 1 && cores <= threads, "{cores} cores, {threads} threads");
        }
        if let (Some(total), Some(available)) = (machine.ram_total, machine.ram_available) {
            assert!(available <= total);
        }
        assert!(machine.os_edition.chars().all(|c| !c.is_control()));
    }

    #[test]
    fn a_path_with_no_drive_gives_no_storage_fact() {
        let machine = read(Path::new("relative/folder"));
        assert!(machine.storage.is_none());
    }
}
