//! The launcher's command and event contract. Five places must agree, and these tests fail on any mismatch:
//!
//! - `commands.list`: the one list of commands (build.rs reads it for the `allow-*` permissions);
//! - the `tauri::generate_handler![...]` call of lib.rs (the commands Rust serves; android.rs serves the same minus the tray);
//! - `capabilities/default.json` (the `allow-<name>` permissions the page is granted; a missing one is denied at
//!   run time with "not allowed by ACL", which the dev mock never reports);
//! - the commands the page calls (`call("name")` and the list/import/activate/remove tables in ui/js);
//! - the events the page listens to (`listen("name")`), which Rust must emit.

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fs;
    use std::path::{Path, PathBuf};

    use serde_json::Value;

    /// The one list of commands the page may call. build.rs reads the same file.
    const COMMANDS_LIST: &str = include_str!("../commands.list");
    const BUILD_RS: &str = include_str!("../build.rs");
    const LIB_RS: &str = include_str!("lib.rs");
    const ANDROID_RS: &str = include_str!("android.rs");
    const CAPABILITY: &str = include_str!("../capabilities/default.json");

    /// Names from commands.list (blank lines and `#` comments skipped).
    fn listed_commands() -> BTreeSet<String> {
        COMMANDS_LIST
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
            .map(str::to_string)
            .collect()
    }

    /// The names in the `generate_handler!` call of lib.rs (the desktop launcher), without module prefixes.
    fn registered_commands() -> BTreeSet<String> {
        handler_names(LIB_RS)
    }

    /// The names in the first `generate_handler!` call of a source file, without module prefixes.
    fn handler_names(source: &str) -> BTreeSet<String> {
        let start = source.find("generate_handler![").expect("handler list") + "generate_handler![".len();
        let end = start + source[start..].find(']').expect("end of handler list");
        source[start..end]
            .split(',')
            .map(str::trim)
            .filter(|name| !name.is_empty())
            .map(|name| name.rsplit("::").next().unwrap().to_string())
            .collect()
    }

    /// The `allow-<name>` permissions of capabilities/default.json, as command names (dashes back to underscores).
    fn allowed_by_capability() -> BTreeSet<String> {
        let capability: Value = serde_json::from_str(CAPABILITY).expect("capabilities/default.json is JSON");
        capability["permissions"]
            .as_array()
            .expect("permissions list")
            .iter()
            .filter_map(Value::as_str)
            .filter_map(|permission| permission.strip_prefix("allow-"))
            .map(|name| name.replace('-', "_"))
            .collect()
    }

    /// The text of every file under `folder` with the given extension.
    fn files_under(folder: &Path, extension: &str) -> Vec<String> {
        let mut texts = Vec::new();
        let mut stack = vec![folder.to_path_buf()];
        while let Some(current) = stack.pop() {
            for entry in fs::read_dir(&current).expect("readable folder") {
                let path: PathBuf = entry.unwrap().path();
                if path.is_dir() {
                    stack.push(path);
                } else if path.extension().and_then(|value| value.to_str()) == Some(extension) {
                    texts.push(fs::read_to_string(&path).unwrap());
                }
            }
        }
        texts
    }

    fn manifest_folder(relative: &str) -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join(relative)
    }

    /// The names that follow each marker (`call("`, `list: "`, ...) in the page scripts. A name is the run of
    /// lowercase letters, digits, `_` and `-` directly before the closing quote.
    fn quoted_after(texts: &[String], markers: &[&str]) -> BTreeSet<String> {
        let mut names = BTreeSet::new();
        for text in texts {
            for marker in markers {
                for piece in text.split(marker).skip(1) {
                    let name: String = piece
                        .chars()
                        .take_while(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == '_' || *c == '-')
                        .collect();
                    if !name.is_empty() && piece[name.len()..].starts_with('"') {
                        names.insert(name);
                    }
                }
            }
        }
        names
    }

    fn page_commands() -> BTreeSet<String> {
        let scripts = files_under(&manifest_folder("ui/js"), "js");
        quoted_after(&scripts, &["call(\"", "list: \"", "importCmd: \"", "activate: \"", "remove: \""])
    }

    fn page_events() -> BTreeSet<String> {
        let scripts = files_under(&manifest_folder("ui/js"), "js");
        quoted_after(&scripts, &["listen(\""])
    }

    /// Rust files that can emit events: everything in src/ except the tests (contract.rs, flow_audit.rs).
    fn rust_sources() -> Vec<String> {
        let mut texts = Vec::new();
        for entry in fs::read_dir(manifest_folder("src")).expect("src") {
            let path = entry.unwrap().path();
            let name = path.file_name().unwrap().to_string_lossy().to_string();
            if path.extension().and_then(|value| value.to_str()) == Some("rs")
                && name != "contract.rs"
                && name != "flow_audit.rs"
            {
                texts.push(fs::read_to_string(&path).unwrap());
            }
        }
        texts
    }

    /// Fails with the names that only one side has, so the message says what to fix.
    fn assert_same(what: &str, expected_label: &str, expected: &BTreeSet<String>, actual_label: &str, actual: &BTreeSet<String>) {
        let only_expected: Vec<&String> = expected.difference(actual).collect();
        let only_actual: Vec<&String> = actual.difference(expected).collect();
        assert!(
            only_expected.is_empty() && only_actual.is_empty(),
            "{what}: only in {expected_label}: {only_expected:?}; only in {actual_label}: {only_actual:?}"
        );
    }

    #[test]
    fn build_script_takes_its_permissions_from_the_command_list() {
        assert!(
            BUILD_RS.contains("include_str!(\"commands.list\")"),
            "build.rs must read commands.list, not keep its own copy of the names"
        );
    }

    #[test]
    fn the_command_list_has_no_duplicates() {
        let names: Vec<&str> = COMMANDS_LIST
            .lines()
            .map(str::trim)
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
            .collect();
        assert_eq!(names.len(), listed_commands().len(), "a command is listed twice in commands.list");
    }

    #[test]
    fn registered_commands_match_the_command_list() {
        assert_same(
            "generate_handler! in lib.rs",
            "lib.rs",
            &listed_commands(),
            "commands.list",
            &registered_commands(),
        );
    }

    /// TASK-180: the Android app serves the same commands as the desktop launcher, except the tray's close confirmation.
    #[test]
    fn android_serves_the_desktop_commands_minus_the_tray() {
        let mut expected = registered_commands();
        assert!(expected.remove("confirm_close"), "the desktop list has the tray's confirm_close");
        assert_eq!(handler_names(ANDROID_RS), expected, "android.rs generate_handler! differs from lib.rs minus confirm_close");
    }

    #[test]
    fn capability_allows_exactly_the_listed_commands() {
        assert_same(
            "allow-* permissions in capabilities/default.json",
            "commands.list",
            &listed_commands(),
            "capabilities/default.json",
            &allowed_by_capability(),
        );
    }

    #[test]
    fn the_page_calls_exactly_the_listed_commands() {
        assert_same("commands the page calls (ui/js)", "commands.list", &listed_commands(), "ui/js", &page_commands());
    }

    #[test]
    fn every_event_the_page_listens_to_is_emitted_by_rust() {
        let sources = rust_sources();
        let missing: Vec<String> = page_events()
            .into_iter()
            .filter(|event| {
                let quoted = format!("\"{event}\"");
                !sources.iter().any(|text| {
                    text.lines().any(|line| (line.contains("emit(") || line.contains("notify(")) && line.contains(&quoted))
                })
            })
            .collect();
        assert!(missing.is_empty(), "the page listens to events that no Rust emit() or notify() sends: {missing:?}");
    }
}
