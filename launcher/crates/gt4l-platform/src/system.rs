//! Reads the processor features and probes Vulkan for the requirement checks (TASK-081, TASK-108). The rules that turn these
//! readings into a message are pure and tested in `gt4l_core::system_check`.
//!
//! - `read_cpu_features`: CPUID leaf 1 (SSE3, SSSE3, SSE4.1).
//! - `probe_vulkan`: loads `vulkan-1.dll` from System32 (as the game does), lists the instance extensions, creates a
//!   bare Vulkan 1.0 instance with no extensions or layers, lists the GPUs with their Vulkan version, and destroys the
//!   instance. It opens no window and allocates no GPU memory. A loaded driver stays in the launcher process until it
//!   exits. The graphics cards are listed by `gpu::probe_gpus`, which uses this probe.

use gt4l_core::system_check::{CpuFeatures, VulkanDevice, VulkanProbe};

/// The Windows user language as a locale name (`pt-BR`, `en-US`), from `GetUserDefaultLocaleName`. Empty when it cannot
/// be read or off Windows; `gt4l_core::language` then uses English.
#[cfg(windows)]
pub fn os_locale() -> String {
    use windows::Win32::Globalization::GetUserDefaultLocaleName;

    // LOCALE_NAME_MAX_LENGTH is 85 UTF-16 units; a locale name is far shorter.
    let mut buffer = [0u16; 85];
    // SAFETY: the buffer is valid for the length passed; the call writes a NUL-terminated name or fails with 0.
    let written = unsafe { GetUserDefaultLocaleName(&mut buffer) };
    if written <= 1 {
        return String::new();
    }
    String::from_utf16_lossy(&buffer[..(written as usize) - 1])
}

/// Off Windows there is no Windows user language: English.
#[cfg(not(windows))]
pub fn os_locale() -> String {
    String::new()
}

/// The processor features the rules need (CPUID leaf 1, ECX: SSE3, SSSE3, SSE4.1).
#[cfg(target_arch = "x86_64")]
pub fn read_cpu_features() -> CpuFeatures {
    use std::arch::x86_64::__cpuid;

    let bit = |value: u32, index: u32| value & (1 << index) != 0;
    // CPUID is always present on x86-64.
    #[allow(unused_unsafe)]
    let leaf1 = unsafe { __cpuid(1) };
    CpuFeatures { sse3: bit(leaf1.ecx, 0), ssse3: bit(leaf1.ecx, 9), sse4_1: bit(leaf1.ecx, 19) }
}

/// Other targets are not supported by the game; report nothing so the rules refuse.
#[cfg(not(target_arch = "x86_64"))]
pub fn read_cpu_features() -> CpuFeatures {
    CpuFeatures::default()
}

/// The processor check, which blocks setup and Play (TASK-108: setup needs no GPU, and the graphics card is chosen per
/// run by `gpu::run_settings`).
pub fn processor_problem(language: gt4l_core::language::Language) -> Option<String> {
    gt4l_core::system_check::cpu_problem_in(&read_cpu_features(), language)
}

#[cfg(windows)]
pub fn probe_vulkan() -> VulkanProbe {
    vulkan_ffi::probe()
}

#[cfg(not(windows))]
pub fn probe_vulkan() -> VulkanProbe {
    VulkanProbe::LoaderMissing
}

/// The few Vulkan 1.0 entry points the probe uses, declared by hand (no Vulkan crate in the launcher).
#[cfg(windows)]
mod vulkan_ffi {
    use std::ffi::{c_char, c_void, CStr};
    use std::ptr;

    use windows::core::{s, w};
    use windows::Win32::Foundation::FreeLibrary;
    use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_SYSTEM32};

    use super::{VulkanDevice, VulkanProbe};

    type VkInstance = *mut c_void;
    type VkPhysicalDevice = *mut c_void;
    type VkResult = i32;
    const VK_SUCCESS: VkResult = 0;
    const VK_INCOMPLETE: VkResult = 5;
    const VK_STRUCTURE_TYPE_APPLICATION_INFO: u32 = 0;
    const VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO: u32 = 1;
    const VK_API_VERSION_1_0: u32 = 1 << 22;
    const VK_MAX_EXTENSION_NAME_SIZE: usize = 256;

    #[repr(C)]
    struct VkApplicationInfo {
        s_type: u32,
        p_next: *const c_void,
        p_application_name: *const c_char,
        application_version: u32,
        p_engine_name: *const c_char,
        engine_version: u32,
        api_version: u32,
    }

    #[repr(C)]
    struct VkInstanceCreateInfo {
        s_type: u32,
        p_next: *const c_void,
        flags: u32,
        p_application_info: *const VkApplicationInfo,
        enabled_layer_count: u32,
        pp_enabled_layer_names: *const *const c_char,
        enabled_extension_count: u32,
        pp_enabled_extension_names: *const *const c_char,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct VkExtensionProperties {
        extension_name: [c_char; VK_MAX_EXTENSION_NAME_SIZE],
        spec_version: u32,
    }

    /// VkPhysicalDeviceProperties starts with these fields; the rest (limits, sparse properties) is not read. The
    /// probe hands the driver a larger zeroed buffer (`PROPERTIES_BUFFER`) and reads only this prefix.
    #[repr(C)]
    struct PropertiesPrefix {
        api_version: u32,
        driver_version: u32,
        vendor_id: u32,
        device_id: u32,
        device_type: u32,
        device_name: [c_char; 256],
    }
    /// sizeof(VkPhysicalDeviceProperties) is 824 bytes on x64; 4 KiB leaves room for any driver.
    const PROPERTIES_BUFFER: usize = 4096;

    type GetInstanceProcAddr = unsafe extern "system" fn(VkInstance, *const c_char) -> Option<unsafe extern "system" fn()>;
    type EnumerateInstanceExtensionProperties =
        unsafe extern "system" fn(*const c_char, *mut u32, *mut VkExtensionProperties) -> VkResult;
    type CreateInstance =
        unsafe extern "system" fn(*const VkInstanceCreateInfo, *const c_void, *mut VkInstance) -> VkResult;
    type DestroyInstance = unsafe extern "system" fn(VkInstance, *const c_void);
    type EnumeratePhysicalDevices = unsafe extern "system" fn(VkInstance, *mut u32, *mut VkPhysicalDevice) -> VkResult;
    type GetPhysicalDeviceProperties = unsafe extern "system" fn(VkPhysicalDevice, *mut c_void);
    type GetPhysicalDeviceProperties2 = unsafe extern "system" fn(VkPhysicalDevice, *mut c_void);

    const VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2: u32 = 1_000_059_001;
    const VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: u32 = 1_000_071_004;

    /// VkPhysicalDeviceIDProperties, chained through pNext of VkPhysicalDeviceProperties2 (Vulkan 1.1).
    #[repr(C)]
    struct IdPropertiesChain {
        s_type: u32,
        p_next: *mut c_void,
        device_uuid: [u8; 16],
        driver_uuid: [u8; 16],
        device_luid: [u8; 8],
        device_node_mask: u32,
        device_luid_valid: u32,
    }

    fn text(raw: &[c_char]) -> String {
        let bytes: Vec<u8> = raw.iter().take_while(|byte| **byte != 0).map(|byte| *byte as u8).collect();
        String::from_utf8_lossy(&bytes).into_owned()
    }

    /// The LUID of a device, when the driver reports it as valid (VkPhysicalDeviceIDProperties::deviceLUIDValid).
    unsafe fn device_luid(handle: VkPhysicalDevice, properties2_of: GetPhysicalDeviceProperties2) -> Option<u64> {
        let mut id = IdPropertiesChain {
            s_type: VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            p_next: ptr::null_mut(),
            device_uuid: [0; 16],
            driver_uuid: [0; 16],
            device_luid: [0; 8],
            device_node_mask: 0,
            device_luid_valid: 0,
        };
        // u64 storage keeps the buffer 8-byte aligned; the header is sType (u32) then pNext, as the driver reads it.
        let mut buffer = vec![0u64; PROPERTIES_BUFFER / 8];
        let bytes = buffer.as_mut_ptr().cast::<u8>();
        bytes.cast::<u32>().write(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        bytes.add(8).cast::<*mut c_void>().write(&mut id as *mut IdPropertiesChain as *mut c_void);
        properties2_of(handle, buffer.as_mut_ptr().cast());
        if id.device_luid_valid != 0 {
            Some(u64::from_le_bytes(id.device_luid))
        } else {
            None
        }
    }

    pub fn probe() -> VulkanProbe {
        // SAFETY: the library is loaded from System32 only and freed at the end; every function pointer comes from the
        // loader's own vkGetInstanceProcAddr and is called with the Vulkan 1.0 signatures declared above.
        unsafe {
            let Ok(library) = LoadLibraryExW(w!("vulkan-1.dll"), None, LOAD_LIBRARY_SEARCH_SYSTEM32) else {
                return VulkanProbe::LoaderMissing;
            };
            let result = match GetProcAddress(library, s!("vkGetInstanceProcAddr")) {
                Some(address) => {
                    let get: GetInstanceProcAddr = std::mem::transmute(address);
                    probe_with(get)
                }
                None => VulkanProbe::LoaderMissing,
            };
            let _ = FreeLibrary(library);
            result
        }
    }

    unsafe fn load<T>(get: GetInstanceProcAddr, instance: VkInstance, name: &CStr) -> Option<T> {
        let address = get(instance, name.as_ptr())?;
        Some(std::mem::transmute_copy::<unsafe extern "system" fn(), T>(&address))
    }

    unsafe fn probe_with(get: GetInstanceProcAddr) -> VulkanProbe {
        let null = ptr::null_mut();
        let (Some(enumerate_extensions), Some(create_instance)) = (
            load::<EnumerateInstanceExtensionProperties>(get, null, c"vkEnumerateInstanceExtensionProperties"),
            load::<CreateInstance>(get, null, c"vkCreateInstance"),
        ) else {
            return VulkanProbe::LoaderMissing;
        };

        let mut instance_extensions = Vec::new();
        let mut count = 0u32;
        if enumerate_extensions(ptr::null(), &mut count, ptr::null_mut()) == VK_SUCCESS && count > 0 {
            let empty = VkExtensionProperties { extension_name: [0; VK_MAX_EXTENSION_NAME_SIZE], spec_version: 0 };
            let mut list = vec![empty; count as usize];
            let code = enumerate_extensions(ptr::null(), &mut count, list.as_mut_ptr());
            if code == VK_SUCCESS || code == VK_INCOMPLETE {
                list.truncate(count as usize);
                instance_extensions = list.iter().map(|item| text(&item.extension_name)).collect();
            }
        }

        let application = VkApplicationInfo {
            s_type: VK_STRUCTURE_TYPE_APPLICATION_INFO,
            p_next: ptr::null(),
            p_application_name: c"GT4Recomp launcher check".as_ptr(),
            application_version: 0,
            p_engine_name: ptr::null(),
            engine_version: 0,
            api_version: VK_API_VERSION_1_0,
        };
        let create_info = VkInstanceCreateInfo {
            s_type: VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            p_next: ptr::null(),
            flags: 0,
            p_application_info: &application,
            enabled_layer_count: 0,
            pp_enabled_layer_names: ptr::null(),
            enabled_extension_count: 0,
            pp_enabled_extension_names: ptr::null(),
        };
        let mut instance: VkInstance = ptr::null_mut();
        let code = create_instance(&create_info, ptr::null(), &mut instance);
        if code != VK_SUCCESS || instance.is_null() {
            return VulkanProbe::InstanceFailed { code };
        }

        let mut devices = Vec::new();
        let destroy = load::<DestroyInstance>(get, instance, c"vkDestroyInstance");
        let enumerate_devices = load::<EnumeratePhysicalDevices>(get, instance, c"vkEnumeratePhysicalDevices");
        let properties_of = load::<GetPhysicalDeviceProperties>(get, instance, c"vkGetPhysicalDeviceProperties");
        // The LUID comes from vkGetPhysicalDeviceProperties2 (core in Vulkan 1.1; the KHR name is tried too). Without it
        // the devices are still listed, with no LUID.
        let properties2_of = load::<GetPhysicalDeviceProperties2>(get, instance, c"vkGetPhysicalDeviceProperties2")
            .or_else(|| load::<GetPhysicalDeviceProperties2>(get, instance, c"vkGetPhysicalDeviceProperties2KHR"));
        if let (Some(enumerate_devices), Some(properties_of)) = (enumerate_devices, properties_of) {
            let mut count = 0u32;
            if enumerate_devices(instance, &mut count, ptr::null_mut()) == VK_SUCCESS && count > 0 {
                let mut handles: Vec<VkPhysicalDevice> = vec![ptr::null_mut(); count as usize];
                let code = enumerate_devices(instance, &mut count, handles.as_mut_ptr());
                if code == VK_SUCCESS || code == VK_INCOMPLETE {
                    handles.truncate(count as usize);
                    for handle in handles {
                        // u64 storage keeps the buffer 8-byte aligned, as the driver's struct needs.
                        let mut buffer = vec![0u64; PROPERTIES_BUFFER / 8];
                        properties_of(handle, buffer.as_mut_ptr().cast());
                        let prefix = &*(buffer.as_ptr() as *const PropertiesPrefix);
                        let luid = properties2_of.and_then(|properties2| device_luid(handle, properties2));
                        devices.push(VulkanDevice {
                            name: text(&prefix.device_name),
                            api_major: (prefix.api_version >> 22) & 0x7F,
                            api_minor: (prefix.api_version >> 12) & 0x3FF,
                            vendor_id: prefix.vendor_id,
                            device_type: prefix.device_type,
                            device_id: prefix.device_id,
                            driver_version: prefix.driver_version,
                            luid,
                        });
                    }
                }
            }
        }
        if let Some(destroy) = destroy {
            destroy(instance, ptr::null());
        }
        VulkanProbe::Ready { instance_extensions, devices }
    }
}

/// The local date and time now (the launcher's log file names and lines, TASK-112).
#[cfg(windows)]
pub fn local_time() -> gt4l_core::logs::LocalTime {
    use windows::Win32::System::SystemInformation::GetLocalTime;

    // SAFETY: a plain call that fills in the local SYSTEMTIME.
    let now = unsafe { GetLocalTime() };
    gt4l_core::logs::LocalTime {
        year: u32::from(now.wYear),
        month: u32::from(now.wMonth),
        day: u32::from(now.wDay),
        hour: u32::from(now.wHour),
        minute: u32::from(now.wMinute),
        second: u32::from(now.wSecond),
    }
}

/// Off Windows the clock is read in UTC (the launcher only runs on Windows today).
#[cfg(not(windows))]
pub fn local_time() -> gt4l_core::logs::LocalTime {
    utc_time_of(std::time::SystemTime::now())
}

/// The UTC date and time of `time` (a civil calendar from the Unix day count).
pub fn utc_time_of(time: std::time::SystemTime) -> gt4l_core::logs::LocalTime {
    let seconds = time.duration_since(std::time::UNIX_EPOCH).map(|value| value.as_secs()).unwrap_or(0) as i64;
    let days = seconds.div_euclid(86_400);
    let of_day = seconds.rem_euclid(86_400);
    // Howard Hinnant's civil-from-days algorithm.
    let shifted = days + 719_468;
    let era = shifted.div_euclid(146_097);
    let day_of_era = shifted.rem_euclid(146_097);
    let year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 { month_index + 3 } else { month_index - 9 };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    gt4l_core::logs::LocalTime {
        year: year as u32,
        month: month as u32,
        day: day as u32,
        hour: (of_day / 3600) as u32,
        minute: ((of_day % 3600) / 60) as u32,
        second: (of_day % 60) as u32,
    }
}

/// The Windows version for the launcher log and the bug report, e.g. "Windows 11 (build 26100)". Reads the registry;
/// Windows 11 has build 22000 and up (its product name still says Windows 10). Empty when it cannot be read.
#[cfg(windows)]
pub fn windows_version() -> String {
    let build = registry_string(r"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "CurrentBuild").unwrap_or_default();
    let product = registry_string(r"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "ProductName").unwrap_or_default();
    let display = registry_string(r"SOFTWARE\Microsoft\Windows NT\CurrentVersion", "DisplayVersion").unwrap_or_default();
    let build_number: u32 = build.trim().parse().unwrap_or(0);
    let name = if build_number >= 22_000 { "Windows 11".to_string() } else { product.trim().to_string() };
    let mut text = if name.is_empty() { "Windows".to_string() } else { name };
    if !display.trim().is_empty() {
        text.push_str(&format!(" {}", display.trim()));
    }
    if build_number > 0 {
        text.push_str(&format!(" (build {build_number})"));
    }
    text
}

/// Off Windows there is no registry: nothing to report.
#[cfg(not(windows))]
pub fn windows_version() -> String {
    String::new()
}

/// A REG_SZ value under HKEY_LOCAL_MACHINE (64-bit view).
#[cfg(windows)]
fn registry_string(key: &str, value: &str) -> Option<String> {
    use windows::core::HSTRING;
    use windows::Win32::System::Registry::{RegGetValueW, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ};
    let (key, value) = (HSTRING::from(key), HSTRING::from(value));
    let mut buffer = vec![0u16; 512];
    let mut size = (buffer.len() * 2) as u32;
    // SAFETY: the buffer and its size in bytes live for the call; the value is read as REG_SZ only.
    let status = unsafe {
        RegGetValueW(HKEY_LOCAL_MACHINE, &key, &value, RRF_RT_REG_SZ, None, Some(buffer.as_mut_ptr() as *mut _), Some(&mut size))
    };
    if status.is_err() {
        return None;
    }
    let length = (size as usize / 2).min(buffer.len());
    Some(String::from_utf16_lossy(&buffer[..length]).trim_end_matches('\0').to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Reads this machine; the rules are tested in gt4l-core. Here only the plumbing: the bits are consistent.
    /// Reads this machine's Vulkan devices (the launcher's own probe, with the LUID read). Any probe result is accepted
    /// here; the output (`--nocapture`) shows the devices, their PCI ids and LUIDs.
    #[test]
    fn vulkan_probe_runs_on_this_machine() {
        let probe = probe_vulkan();
        if let VulkanProbe::Ready { devices, .. } = &probe {
            for device in devices {
                println!("vulkan device {} vendor {:#06x} device {:#06x} luid {:?}", device.name, device.vendor_id, device.device_id, device.luid);
            }
        }
        println!("vulkan probe: {probe:?}");
    }

    #[test]
    fn local_time_is_a_plausible_clock_reading() {
        let now = local_time();
        assert!((2024..=2100).contains(&now.year) && (1..=12).contains(&now.month) && (1..=31).contains(&now.day));
        assert!(now.hour < 24 && now.minute < 60 && now.second < 60);
        let epoch = utc_time_of(std::time::UNIX_EPOCH);
        assert_eq!((epoch.year, epoch.month, epoch.day, epoch.hour), (1970, 1, 1, 0));
        // 2026-10-23 21:03:40 UTC is 1792789420 seconds after the epoch.
        let sample = utc_time_of(std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_792_789_420));
        assert_eq!(sample.stamp(), "2026-10-23 21:03:40");
    }

    #[test]
    fn cpu_reading_is_consistent() {
        let features = read_cpu_features();
        // Every x86-64 CPU that has SSE4.1 also has SSSE3 and SSE3.
        if features.sse4_1 {
            assert!(features.ssse3 && features.sse3);
        }
    }

    /// Prints what this machine's Vulkan loader reports. Ignored by default: it loads the GPU driver.
    #[test]
    #[ignore]
    fn vulkan_probe_on_this_machine() {
        let probe = probe_vulkan();
        println!("{probe:#?}");
        println!("problem: {:?}", gt4l_core::system_check::vulkan_problem(&probe));
        println!("cpu: {:?} problem: {:?}", read_cpu_features(), gt4l_core::system_check::cpu_problem(&read_cpu_features()));
    }
}
