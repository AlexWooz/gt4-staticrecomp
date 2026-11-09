//! Decides whether a game session ended normally, from its exit code and the `[ee] stopped` line.
//!
//! Mirrors `external_runtime_probe.summarize_exit` and the manual-session rule in `run_probe`: a normal close is exit
//! code 0 with one of the deliberate reasons below. `window closed` is the window's close; `quit game` is the native
//! Quit Game row in a headless run; `Exit` and `program asked to run another executable` are the guest's own
//! `Exit`/`LoadExecPS2` (GT4's `MSystem::Reboot`), which the recomp cannot honour in-process, so the runtime stops and
//! the session is a deliberate quit, not a crash.
use crate::language::Language;
use crate::messages;


const STOP_PREFIX: &str = "[ee] stopped after ";
const CYCLES_MARKER: &str = " cycles: ";
/// Reasons that mean the player or the game asked to end the session (TASK-212).
const NORMAL_REASONS: [&str; 4] = ["window closed", "quit game", "Exit", "program asked to run another executable"];

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// The player closed the window or the game's Quit Game/Exit path stopped the session (exit code 0).
    NormalClose,
    /// Anything else. `reason` is the text of the last `[ee] stopped` line, when there was one.
    Crashed { reason: Option<String>, exit_code: Option<i32> },
}

/// Classifies a finished session from its full log text and exit code (`None` = killed by us).
pub fn classify(log_text: &str, exit_code: Option<i32>) -> Outcome {
    let reason = log_text
        .lines()
        .filter_map(stop_reason)
        .last()
        .map(str::to_string);
    // Exit code 0 with no shutdown line at all: the log was read before the game flushed it (owner, 2026-10-26: three
    // normal closes were reported as crashes). The caller retries the read, so a real crash, which exits non-zero or
    // prints a reason, is still a crash.
    let normal = reason.as_deref().map_or(true, |text| NORMAL_REASONS.contains(&text));
    if exit_code == Some(0) && normal {
        Outcome::NormalClose
    } else {
        Outcome::Crashed { reason, exit_code }
    }
}

/// True when the log holds the game's final shutdown line (`[ee] stopped after ...`). The launcher waits for it before
/// classifying, so a normal close is never read mid-write.
pub fn is_settled(log_text: &str) -> bool {
    log_text.lines().any(|line| stop_reason(line).is_some())
}

/// `[ee] stopped after N frames, C cycles: <reason>` -> `<reason>`.
fn stop_reason(line: &str) -> Option<&str> {
    let rest = line.trim().strip_prefix(STOP_PREFIX)?;
    let index = rest.find(CYCLES_MARKER)?;
    Some(rest[index + CYCLES_MARKER.len()..].trim())
}

/// Text that GSdx writes to a session's `game.log` when the graphics card stops responding (issue #89). The first
/// two are the GSdx assertion and its message ("Host GPU lost too many times, device is probably completely wedged");
/// the last is the Vulkan result name. Matched case-insensitively. A bare "device lost" is not a marker: other devices
/// (audio) can report it too.
pub const GPU_LOST_MARKERS: [&str; 3] = ["host gpu lost", "device is probably completely wedged", "vk_error_device_lost"];

/// The first line of a session log that says the graphics card was lost, or `None`. The line stays in the launcher log
/// as the detail behind the plain message.
pub fn gpu_lost_line(log_text: &str) -> Option<&str> {
    log_text.lines().find(|line| {
        let lower = line.to_lowercase();
        GPU_LOST_MARKERS.iter().any(|marker| lower.contains(marker))
    })
}

/// The plain message for a lost graphics card, in `language` (issue #89). Shown in place of the generic crash text.
pub fn gpu_lost_message_in(language: Language) -> String {
    messages::text("game.gpu_lost", language).to_string()
}

/// `STATUS_DLL_NOT_FOUND`: the Windows loader could not find a DLL the game program imports, so the process ended before
/// it ran any code and `game.log` stayed empty (issues #107 and #99). Shown by tools as -1073741515 or 3221225781.
pub const STATUS_DLL_NOT_FOUND: u32 = 0xC000_0135;

/// `STATUS_APPLICATION_HANG`: the code Windows gives an application it closed because it stopped answering.
pub const STATUS_APPLICATION_HANG: u32 = 0xCFFF_FFFF;

/// The high bit of a 32-bit status: Windows error statuses (NTSTATUS) have it set, and a signed exit code shows them negative.
const STATUS_ERROR_BIT: u32 = 0x8000_0000;

/// The exit code as Windows defines it: the same 32 bits read as unsigned (defined two's-complement wrapping).
fn status_of(exit_code: i32) -> u32 {
    exit_code as u32
}

/// An exit code for a message: the decimal number, and for an NTSTATUS-like value (high bit set) also the hex form that
/// Windows tools and web searches use, for example `-1073741819 (0xC0000005)`.
pub fn exit_code_text(exit_code: i32) -> String {
    let status = status_of(exit_code);
    if status & STATUS_ERROR_BIT != 0 {
        format!("{exit_code} (0x{status:08X})")
    } else {
        exit_code.to_string()
    }
}

/// One plain sentence for the crash dialog, in English. Technical detail stays in the log.
pub fn user_message(outcome: &Outcome) -> String {
    user_message_in(outcome, Language::fallback())
}

/// [`user_message`] in `language`. Two Windows statuses get their own plain sentence (a missing file, a hung window); any
/// other exit code is named in the generic sentence.
pub fn user_message_in(outcome: &Outcome, language: Language) -> String {
    match outcome {
        Outcome::NormalClose => String::new(),
        Outcome::Crashed { reason: Some(reason), .. } => {
            messages::render("game.stopped_unexpectedly_reason", language, &[("reason", reason)])
        }
        Outcome::Crashed { reason: None, exit_code: Some(code) } => match status_of(*code) {
            STATUS_DLL_NOT_FOUND => messages::render("game.missing_file", language, &[("code", &format!("0x{STATUS_DLL_NOT_FOUND:08X}"))]),
            STATUS_APPLICATION_HANG => {
                messages::render("game.not_responding", language, &[("code", &format!("0x{STATUS_APPLICATION_HANG:08X}"))])
            }
            _ => messages::render("game.stopped_unexpectedly_code", language, &[("code", &exit_code_text(*code))]),
        },
        Outcome::Crashed { reason: None, exit_code: None } => messages::text("game.stopped_unexpectedly", language).to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_closed_with_exit_zero_is_normal() {
        let log = "boot\n[ee] stopped after 1200 frames, 9000 cycles: window closed\n";
        assert_eq!(classify(log, Some(0)), Outcome::NormalClose);
    }

    #[test]
    fn the_native_quit_row_and_guest_exit_paths_are_normal_quits() {
        // TASK-212: the native Quit Game row closes the window (window closed) or, headless, halts with `quit game`.
        // The guest's own Exit/LoadExecPS2 (GT4 `MSystem::Reboot`) cannot restart the recomp, so it is a deliberate
        // quit too, not a crash.
        for reason in ["quit game", "Exit", "program asked to run another executable"] {
            let log = format!("boot\n[ee] stopped after 900 frames, 3000 cycles: {reason}\n");
            assert_eq!(classify(&log, Some(0)), Outcome::NormalClose, "{reason}");
        }
        let log = "[ee] stopped after 900 frames, 3000 cycles: Exit\n";
        assert!(matches!(classify(log, Some(1)), Outcome::Crashed { .. }), "a non-zero exit stays a crash");
    }

    #[test]
    fn exit_zero_without_a_shutdown_line_is_normal_not_a_crash() {
        // Owner (2026-10-26): the launcher read the log before the game wrote its last line, so a normal close was
        // reported as Crashed { reason: None, exit_code: Some(0) } three times.
        assert_eq!(classify("boot\n[perf] last line not flushed yet\n", Some(0)), Outcome::NormalClose);
        assert!(!is_settled("boot\n[audio] paced: ...\n"));
        assert!(is_settled("boot\n[ee] stopped after 120 frames, 9000 cycles: window closed\n"));
        // A killed process (no exit code) is still a crash, and so is a non-zero exit.
        assert_eq!(classify("boot\n", None), Outcome::Crashed { reason: None, exit_code: None });
        assert!(matches!(classify("boot\n", Some(1)), Outcome::Crashed { exit_code: Some(1), .. }));
    }

    #[test]
    fn last_stop_line_wins() {
        let log = "[ee] stopped after 5 frames, 1 cycles: window closed\n\
                   [ee] stopped after 9 frames, 2 cycles: no translated code at 0x00123456\n";
        assert_eq!(
            classify(log, Some(1)),
            Outcome::Crashed { reason: Some("no translated code at 0x00123456".into()), exit_code: Some(1) }
        );
    }

    #[test]
    fn window_closed_with_nonzero_exit_is_a_crash() {
        let log = "[ee] stopped after 5 frames, 1 cycles: window closed\n";
        assert!(matches!(classify(log, Some(3)), Outcome::Crashed { exit_code: Some(3), .. }));
    }

    #[test]
    fn missing_stop_line_and_killed_process_is_a_crash_without_reason() {
        assert_eq!(classify("", None), Outcome::Crashed { reason: None, exit_code: None });
    }

    #[test]
    fn user_message_names_the_reason_in_one_sentence() {
        let outcome = Outcome::Crashed { reason: Some("no translated code".into()), exit_code: Some(1) };
        assert!(user_message(&outcome).starts_with("Gran Turismo 4 stopped unexpectedly (no translated code)"));
    }

    fn pt_br() -> Language {
        Language::from_tag("pt-BR").expect("pt-BR is listed")
    }

    fn crashed_with(code: i32) -> Outcome {
        Outcome::Crashed { reason: None, exit_code: Some(code) }
    }

    #[test]
    fn a_missing_dll_is_a_plain_message_about_a_file_missing_from_the_game_folder() {
        // Issue #107: exit code -1073741515 (0xC0000135), an empty game.log. The same 32 bits read as unsigned are 3221225781.
        assert_eq!(STATUS_DLL_NOT_FOUND, 3_221_225_781);
        let english = user_message_in(&crashed_with(-1_073_741_515), Language::fallback());
        assert!(english.starts_with("Gran Turismo 4 could not start because a file it needs is missing"), "{english}");
        assert!(english.contains("0xC0000135"), "{english}");
        assert!(english.contains("Reinstall GT4Recomp") && english.contains("antivirus"), "{english}");
        assert!(!english.contains("Redistributable") && !english.contains("Visual C++"), "the missing file is not known: {english}");
        let portuguese = user_message_in(&crashed_with(-1_073_741_515), pt_br());
        assert!(portuguese.starts_with("O Gran Turismo 4 não conseguiu iniciar porque falta um arquivo"), "{portuguese}");
        assert!(portuguese.contains("0xC0000135") && portuguese.contains("antivírus"), "{portuguese}");
    }

    #[test]
    fn a_window_windows_closed_as_not_responding_asks_for_the_logs() {
        // Issue #99: exit code -805306369 (0xCFFFFFFF).
        assert_eq!(STATUS_APPLICATION_HANG, 3_489_660_927);
        let english = user_message_in(&crashed_with(-805_306_369), Language::fallback());
        assert!(english.starts_with("Gran Turismo 4 stopped responding and Windows closed it"), "{english}");
        assert!(english.contains("0xCFFFFFFF") && english.contains("Export logs"), "{english}");
        let portuguese = user_message_in(&crashed_with(-805_306_369), pt_br());
        assert!(portuguese.starts_with("O Gran Turismo 4 parou de responder e o Windows o fechou"), "{portuguese}");
        assert!(portuguese.contains("0xCFFFFFFF") && portuguese.contains("Exportar logs"), "{portuguese}");
    }

    #[test]
    fn another_windows_status_is_named_in_hex_and_in_decimal() {
        // 0xC0000005 is an access violation: not explained, but named the way Windows tools name it.
        assert_eq!(exit_code_text(-1_073_741_819), "-1073741819 (0xC0000005)");
        let english = user_message_in(&crashed_with(-1_073_741_819), Language::fallback());
        assert_eq!(english, "Gran Turismo 4 stopped unexpectedly (exit code -1073741819 (0xC0000005)). The session folder has the details.");
        let portuguese = user_message_in(&crashed_with(-1_073_741_819), pt_br());
        assert!(portuguese.contains("código de saída -1073741819 (0xC0000005)"), "{portuguese}");
    }

    #[test]
    fn an_ordinary_exit_code_stays_a_plain_number() {
        assert_eq!(exit_code_text(1), "1");
        assert_eq!(exit_code_text(0), "0");
        assert_eq!(exit_code_text(i32::MAX), "2147483647");
        assert_eq!(exit_code_text(i32::MIN), "-2147483648 (0x80000000)", "the high bit alone is a status");
        let english = user_message_in(&crashed_with(3), Language::fallback());
        assert_eq!(english, "Gran Turismo 4 stopped unexpectedly (exit code 3). The session folder has the details.");
    }

    #[test]
    fn a_reason_from_the_game_log_wins_over_the_exit_code() {
        // The game wrote its own stop line, so that reason is the detail; the status codes above are for the silent ends.
        let outcome = Outcome::Crashed { reason: Some("no translated code".into()), exit_code: Some(-1_073_741_515) };
        assert!(user_message(&outcome).starts_with("Gran Turismo 4 stopped unexpectedly (no translated code)"));
    }

    #[test]
    fn the_gsdx_device_lost_text_is_found_in_the_session_log() {
        let log = "[gs] init renderer Vulkan
Assertion failed in function GSRenderer::BeginPresentFrame: Host GPU lost too many times, device is probably completely wedged
[ee] stopped after 120 frames, 9000 cycles: GS process exited";
        assert_eq!(
            gpu_lost_line(log),
            Some("Assertion failed in function GSRenderer::BeginPresentFrame: Host GPU lost too many times, device is probably completely wedged")
        );
        assert!(gpu_lost_line("vkQueueSubmit returned VK_ERROR_DEVICE_LOST").is_some());
        assert!(gpu_lost_line("HOST GPU LOST").is_some(), "matched case-insensitively");
    }

    #[test]
    fn ordinary_crashes_and_other_device_messages_are_not_a_lost_card() {
        assert_eq!(gpu_lost_line(""), None);
        assert_eq!(gpu_lost_line("[ee] stopped after 5 frames, 80 cycles: no translated code at 0x00123456"), None);
        assert_eq!(gpu_lost_line("audio device lost, switching to the default output"), None);
    }

    #[test]
    fn the_gpu_lost_message_is_plain_and_translated() {
        let english = gpu_lost_message_in(Language::fallback());
        assert_eq!(
            english,
            "Your graphics card stopped responding. Try a lower resolution (Graphics > Render scale) or switch Graphics API to Direct3D 11."
        );
        let portuguese = gpu_lost_message_in(Language::from_tag("pt-BR").expect("pt-BR is listed"));
        assert!(portuguese.starts_with("Sua placa de vídeo parou de responder."), "{portuguese}");
        assert!(portuguese.contains("Direct3D 11"), "{portuguese}");
    }
}
