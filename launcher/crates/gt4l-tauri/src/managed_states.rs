// SPDX-License-Identifier: MIT
// TASK-180 test3 (owner's phone): every command handler asks Tauri for its state (`State<'_, Shared>`,
// `State<'_, pad_service::Handle>`) and a missing one stops the command at once with "state not managed for fields
// 'pads' on command 'capture_binding'". The two app builders (the desktop one in lib.rs and the Android one in
// android.rs) are separate, so a state could be managed on one platform and not the other — which is exactly what
// happened to the pad state on Android. Both builders now call [`check`] after their setup, the result goes to the
// launcher log, and a test (flow_audit.rs) keeps the list below in step with the commands' signatures.

/// The state types the command handlers ask for, named as Rust shows them.
pub const STATES: &[&str] = &["Shared", "pad_service::Handle", "Arc<Guard>"];

/// The states that are missing in a running app (empty when every one is managed).
pub fn missing(app: &tauri::AppHandle) -> Vec<&'static str> {
    use tauri::Manager;
    let mut missing = Vec::new();
    if app.try_state::<crate::state::Shared>().is_none() {
        missing.push("Shared");
    }
    if app.try_state::<crate::pad_service::Handle>().is_none() {
        missing.push("pad_service::Handle");
    }
    if app.try_state::<std::sync::Arc<crate::webview_guard::Guard>>().is_none() {
        missing.push("Arc<Guard>");
    }
    missing
}

/// Logs the state check for this start-up: one line per missing state (an error), else one OK line.
pub fn check(app: &tauri::AppHandle) {
    use tauri::Manager;
    let dirs = app.try_state::<crate::state::Shared>().map(|shared| crate::state::lock(shared.inner()).dirs.clone());
    let missing = missing(app);
    let line = if missing.is_empty() {
        format!("start-up: command state managed ({})", STATES.join(", "))
    } else {
        format!("start-up: command state NOT MANAGED: {}", missing.join(", "))
    };
    match dirs {
        Some(dirs) if missing.is_empty() => crate::state::log(&dirs, &line),
        Some(dirs) => crate::state::log_error(&dirs, &line),
        None => eprintln!("{line}"),
    }
}

#[cfg(test)]
mod tests {
    /// Every `State<'_, T>` a command asks for must be in [`super::STATES`]: the check can only report what it knows.
    #[test]
    fn the_list_covers_the_states_the_commands_ask_for() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let mut found: Vec<String> = Vec::new();
        for entry in std::fs::read_dir(&root).expect("the launcher's sources") {
            let path = entry.expect("a source file").path();
            if path.extension().and_then(|extension| extension.to_str()) != Some("rs")
                || path.file_name().and_then(|name| name.to_str()) == Some("managed_states.rs")
            {
                // This file's own doc comments name the pattern, not a command.
                continue;
            }
            let text = std::fs::read_to_string(&path).expect("a readable source file");
            let mut rest = text.as_str();
            while let Some(start) = rest.find("State<'_, ") {
                rest = &rest[start + "State<'_, ".len()..];
                if let Some(end) = rest.find('>') {
                    found.push(rest[..end].trim().to_string());
                }
            }
        }
        assert!(!found.is_empty(), "no State<'_, T> found: the scan is broken");
        for state in found {
            assert!(super::STATES.contains(&state.as_str()), "a command asks for state {state}, which the start-up check does not know");
        }
    }

    /// Both app builders run the check: a state managed on Windows only is what broke the Android pad commands.
    #[test]
    fn both_builders_check_the_states() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        for file in ["lib.rs", "android.rs"] {
            let text = std::fs::read_to_string(root.join(file)).expect("a builder");
            assert!(text.contains("managed_states::check("), "{file} does not check the command states at start-up");
        }
    }
}
