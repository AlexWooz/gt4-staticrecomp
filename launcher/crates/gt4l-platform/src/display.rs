//! Reads the connected monitors for the Display page (TASK-080 addendum): the GDI device name, the friendly name, the
//! primary flag, the desktop mode and the modes each monitor supports. The rules (identifiers, sorting, resolving a
//! stored choice, the render plan) are pure and tested in `gt4l_core::display`.
//!
//! - `EnumDisplayMonitors` + `GetMonitorInfoW` (MONITORINFOEXW): device name and primary flag;
//! - `EnumDisplaySettingsW` on the device: the desktop mode (`ENUM_CURRENT_SETTINGS`) and every mode (index 0, 1, ...),
//!   reduced to unique width x height pairs by `gt4l_core::display::tidy_modes`;
//! - `QueryDisplayConfig` + `DisplayConfigGetDeviceInfo`: the monitor's friendly name (its EDID name, e.g.
//!   "DELL U3419W"), matched to the GDI device through the path's source name. Without it, `EnumDisplayDevicesW`
//!   gives the driver's monitor string; an empty name is shown as the device ("DISPLAY1").
//!
//! The modes are physical pixels whatever the desktop scaling; the launcher is per-monitor DPI aware (WebView2),
//! and the game declares itself per-monitor DPI aware before it opens its window.

use gt4l_core::display::Monitor;

/// The connected monitors in system order (empty when none can be read).
#[cfg(windows)]
pub fn monitors() -> Vec<Monitor> {
    windows_impl::monitors()
}

#[cfg(not(windows))]
pub fn monitors() -> Vec<Monitor> {
    Vec::new()
}

/// The refresh rate (Hz) and the scale factor (percent, from the effective DPI) of each monitor, by GDI device name, for the
/// system report. Either is `None` when the system does not say.
#[cfg(windows)]
pub fn details() -> Vec<(String, Option<u32>, Option<u32>)> {
    windows_impl::details()
}

#[cfg(not(windows))]
pub fn details() -> Vec<(String, Option<u32>, Option<u32>)> {
    Vec::new()
}

/// The effective DPI as a scale factor in percent (96 DPI is 100%), rounded.
pub fn scale_percent_of(dpi: u32) -> Option<u32> {
    (dpi > 0).then(|| (dpi * 100 + 48) / 96)
}

#[cfg(test)]
mod scale_tests {
    use super::scale_percent_of;

    #[test]
    fn dpi_becomes_a_scale_percent() {
        assert_eq!(scale_percent_of(96), Some(100));
        assert_eq!(scale_percent_of(120), Some(125));
        assert_eq!(scale_percent_of(144), Some(150));
        assert_eq!(scale_percent_of(192), Some(200));
        assert_eq!(scale_percent_of(0), None);
    }
}

#[cfg(windows)]
mod windows_impl {
    use std::collections::HashMap;

    use gt4l_core::display::{tidy_modes, Monitor, Size};
    use windows::core::{BOOL, PCWSTR};
    use windows::Win32::Devices::Display::{
        DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes, QueryDisplayConfig, DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
        DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, DISPLAYCONFIG_MODE_INFO, DISPLAYCONFIG_PATH_INFO,
        DISPLAYCONFIG_SOURCE_DEVICE_NAME, DISPLAYCONFIG_TARGET_DEVICE_NAME, QDC_ONLY_ACTIVE_PATHS,
    };
    use windows::Win32::Foundation::{ERROR_SUCCESS, LPARAM, RECT};
    use windows::Win32::UI::HiDpi::{GetDpiForMonitor, MDT_EFFECTIVE_DPI};
    use windows::Win32::Graphics::Gdi::{
        EnumDisplayDevicesW, EnumDisplayMonitors, EnumDisplaySettingsW, GetMonitorInfoW, DEVMODEW, DISPLAY_DEVICEW,
        ENUM_CURRENT_SETTINGS, ENUM_DISPLAY_SETTINGS_MODE, HDC, HMONITOR, MONITORINFO, MONITORINFOEXW,
    };

    /// `MONITORINFOF_PRIMARY`.
    const PRIMARY_FLAG: u32 = 1;
    /// A safety stop for drivers that never end the mode list.
    const MAX_MODES: u32 = 4096;

    fn wide_to_string(wide: &[u16]) -> String {
        let end = wide.iter().position(|&unit| unit == 0).unwrap_or(wide.len());
        String::from_utf16_lossy(&wide[..end])
    }

    fn wide_null(text: &str) -> Vec<u16> {
        text.encode_utf16().chain(std::iter::once(0)).collect()
    }

    unsafe extern "system" fn collect(monitor: HMONITOR, _hdc: HDC, _rect: *mut RECT, data: LPARAM) -> BOOL {
        // SAFETY: `data` is the address of the Vec passed by `monitors` below, alive for the whole enumeration.
        let handles = unsafe { &mut *(data.0 as *mut Vec<HMONITOR>) };
        handles.push(monitor);
        BOOL(1)
    }

    fn mode(device: &[u16], index: ENUM_DISPLAY_SETTINGS_MODE) -> Option<Size> {
        let mut mode = DEVMODEW { dmSize: std::mem::size_of::<DEVMODEW>() as u16, ..Default::default() };
        // SAFETY: `device` is null-terminated; `mode` is a valid DEVMODEW with dmSize set.
        let ok = unsafe { EnumDisplaySettingsW(PCWSTR(device.as_ptr()), index, &mut mode) };
        ok.as_bool().then(|| Size::new(mode.dmPelsWidth, mode.dmPelsHeight))
    }

    /// Friendly names by GDI device name, from the active display paths.
    fn friendly_names() -> HashMap<String, String> {
        let mut names = HashMap::new();
        let (mut path_count, mut mode_count) = (0u32, 0u32);
        // SAFETY: plain out-parameters.
        if unsafe { GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut path_count, &mut mode_count) } != ERROR_SUCCESS {
            return names;
        }
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); path_count as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); mode_count as usize];
        // SAFETY: the buffers hold the counts passed in.
        let status = unsafe {
            QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS,
                &mut path_count,
                paths.as_mut_ptr(),
                &mut mode_count,
                modes.as_mut_ptr(),
                None,
            )
        };
        if status != ERROR_SUCCESS {
            return names;
        }
        for path in paths.iter().take(path_count as usize) {
            let mut source = DISPLAYCONFIG_SOURCE_DEVICE_NAME::default();
            source.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = std::mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            let mut target = DISPLAYCONFIG_TARGET_DEVICE_NAME::default();
            target.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = std::mem::size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32;
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            // SAFETY: each packet starts with its header, sized for its own struct.
            let source_ok = unsafe { DisplayConfigGetDeviceInfo(&mut source.header) } == 0;
            let target_ok = unsafe { DisplayConfigGetDeviceInfo(&mut target.header) } == 0;
            if source_ok && target_ok {
                let device = wide_to_string(&source.viewGdiDeviceName);
                let name = wide_to_string(&target.monitorFriendlyDeviceName);
                if !device.is_empty() && !name.trim().is_empty() {
                    names.entry(device).or_insert(name);
                }
            }
        }
        names
    }

    /// The driver's monitor string ("Generic PnP Monitor") when DisplayConfig has no friendly name.
    fn driver_name(device: &[u16]) -> String {
        let mut info = DISPLAY_DEVICEW { cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32, ..Default::default() };
        // SAFETY: `device` is null-terminated; `info` has cb set.
        let ok = unsafe { EnumDisplayDevicesW(PCWSTR(device.as_ptr()), 0, &mut info, 0) };
        if ok.as_bool() {
            wide_to_string(&info.DeviceString)
        } else {
            String::new()
        }
    }

    pub fn details() -> Vec<(String, Option<u32>, Option<u32>)> {
        let mut handles: Vec<HMONITOR> = Vec::new();
        // SAFETY: the callback only pushes into `handles`, which outlives the call.
        let enumerated = unsafe {
            EnumDisplayMonitors(None, None, Some(collect), LPARAM(&mut handles as *mut Vec<HMONITOR> as isize))
        };
        if !enumerated.as_bool() {
            return Vec::new();
        }
        let mut details = Vec::new();
        for handle in handles {
            let mut info = MONITORINFOEXW::default();
            info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
            // SAFETY: MONITORINFOEXW starts with MONITORINFO and cbSize says which one it is.
            if !unsafe { GetMonitorInfoW(handle, &mut info as *mut MONITORINFOEXW as *mut MONITORINFO) }.as_bool() {
                continue;
            }
            let device = wide_to_string(&info.szDevice);
            let device_wide = wide_null(&device);
            let mut current = DEVMODEW { dmSize: std::mem::size_of::<DEVMODEW>() as u16, ..Default::default() };
            // SAFETY: `device_wide` is null-terminated; `current` is a valid DEVMODEW with dmSize set.
            let read = unsafe { EnumDisplaySettingsW(PCWSTR(device_wide.as_ptr()), ENUM_CURRENT_SETTINGS, &mut current) };
            // 0 and 1 mean "the hardware default" in dmDisplayFrequency, not a rate.
            let hertz = read.as_bool().then_some(current.dmDisplayFrequency).filter(|hertz| *hertz > 1);
            let (mut dpi_x, mut dpi_y) = (0u32, 0u32);
            // SAFETY: plain out-parameters on a monitor handle the enumeration just gave.
            let scale = unsafe { GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &mut dpi_x, &mut dpi_y) }
                .ok()
                .and_then(|()| super::scale_percent_of(dpi_x));
            details.push((device, hertz, scale));
        }
        details
    }

    pub fn monitors() -> Vec<Monitor> {
        let mut handles: Vec<HMONITOR> = Vec::new();
        // SAFETY: the callback only pushes into `handles`, which outlives the call.
        let enumerated = unsafe {
            EnumDisplayMonitors(None, None, Some(collect), LPARAM(&mut handles as *mut Vec<HMONITOR> as isize))
        };
        if !enumerated.as_bool() {
            return Vec::new();
        }
        let names = friendly_names();
        let mut monitors = Vec::new();
        for handle in handles {
            let mut info = MONITORINFOEXW::default();
            info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
            // SAFETY: MONITORINFOEXW starts with MONITORINFO and cbSize says which one it is.
            if !unsafe { GetMonitorInfoW(handle, &mut info as *mut MONITORINFOEXW as *mut MONITORINFO) }.as_bool() {
                continue;
            }
            let device = wide_to_string(&info.szDevice);
            let device_wide = wide_null(&device);
            let rect = info.monitorInfo.rcMonitor;
            let desktop = mode(&device_wide, ENUM_CURRENT_SETTINGS).unwrap_or_else(|| {
                Size::new((rect.right - rect.left).max(0) as u32, (rect.bottom - rect.top).max(0) as u32)
            });
            let mut raw = Vec::new();
            for index in 0..MAX_MODES {
                match mode(&device_wide, ENUM_DISPLAY_SETTINGS_MODE(index)) {
                    Some(size) => raw.push(size),
                    None => break,
                }
            }
            let name = names.get(&device).cloned().unwrap_or_else(|| driver_name(&device_wide));
            monitors.push(Monitor {
                device,
                name,
                primary: info.monitorInfo.dwFlags & PRIMARY_FLAG != 0,
                desktop,
                modes: tidy_modes(&raw, desktop),
            });
        }
        monitors
    }
}
