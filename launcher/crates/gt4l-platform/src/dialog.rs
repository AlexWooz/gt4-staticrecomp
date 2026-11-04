//! A native Win32 message box (TASK-091, WebView2 guard). It is used when the launcher's web view cannot draw, so it
//! must not depend on the web view: it is plain `MessageBoxW`, with no page and no WebView2.

/// The button the player pressed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Choice {
    Yes,
    No,
    Cancel,
}

/// `MB_YESNOCANCEL | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST`.
#[cfg(windows)]
const STYLE: u32 = 0x0000_0003 | 0x0000_0010 | 0x0001_0000 | 0x0004_0000;
/// `IDYES`, `IDNO` and `IDCANCEL` as `MessageBoxW` returns them.
#[cfg(windows)]
const ID_YES: i32 = 6;
#[cfg(windows)]
const ID_NO: i32 = 7;

/// Shows an error box with Yes, No and Cancel, and blocks until a button is pressed. The window closed by its
/// title bar counts as Cancel.
#[cfg(windows)]
pub fn error_box_yes_no_cancel(title: &str, text: &str) -> Choice {
    use windows::core::PCWSTR;
    use windows::Win32::UI::WindowsAndMessaging::{MessageBoxW, MESSAGEBOX_STYLE};
    let title_wide: Vec<u16> = title.encode_utf16().chain(std::iter::once(0)).collect();
    let text_wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: both buffers are NUL-terminated and live for the call; no owner window is given.
    let pressed = unsafe {
        MessageBoxW(None, PCWSTR(text_wide.as_ptr()), PCWSTR(title_wide.as_ptr()), MESSAGEBOX_STYLE(STYLE))
    };
    match pressed.0 {
        ID_YES => Choice::Yes,
        ID_NO => Choice::No,
        _ => Choice::Cancel,
    }
}

/// Without Windows there is no native box here; the caller just stops.
#[cfg(not(windows))]
pub fn error_box_yes_no_cancel(_title: &str, _text: &str) -> Choice {
    Choice::Cancel
}

/// Shows an error box with one OK button and blocks until it is pressed (a portable folder that cannot be written, before
/// any window exists).
#[cfg(windows)]
pub fn error_box_ok(title: &str, text: &str) {
    use windows::core::PCWSTR;
    use windows::Win32::UI::WindowsAndMessaging::{MessageBoxW, MESSAGEBOX_STYLE};
    // MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST
    const STYLE_OK: u32 = 0x0000_0000 | 0x0000_0010 | 0x0001_0000 | 0x0004_0000;
    let title_wide: Vec<u16> = title.encode_utf16().chain(std::iter::once(0)).collect();
    let text_wide: Vec<u16> = text.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: both buffers are NUL-terminated and live for the call; no owner window is given.
    unsafe {
        MessageBoxW(None, PCWSTR(text_wide.as_ptr()), PCWSTR(title_wide.as_ptr()), MESSAGEBOX_STYLE(STYLE_OK));
    }
}

/// Without Windows there is no native box here.
#[cfg(not(windows))]
pub fn error_box_ok(_title: &str, _text: &str) {}
