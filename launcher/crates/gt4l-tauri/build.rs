//! Build script: generates the permissions of this app's own commands (see `capabilities/`), so the
//! page can call only the commands listed in `commands.list`. Nothing else in Tauri is enabled for the page.
//! It also embeds the launcher's short release notes (TASK-099): every `docs/release-notes/launcher/<version>.md` up to
//! this launcher's version, newest first, into `$OUT_DIR/release_notes.rs`. This version's own short notes must exist,
//! or the build stops. (The full, technical notes beside them in `docs/release-notes/` are for GitHub.)

use std::path::{Path, PathBuf};

/// Reads `commands.list`: one command name per line, `#` starts a comment line.
fn command_names() -> &'static [&'static str] {
    let list: &'static str = include_str!("commands.list");
    let names: Vec<&'static str> = list
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .collect();
    // The build script runs once per build; the names live until the process exits.
    Vec::leak(names)
}

/// `1.2.3` -> (1, 2, 3); anything else is not a release notes file.
fn parse_version(text: &str) -> Option<(u32, u32, u32)> {
    let mut parts = text.split('.');
    let version = (parts.next()?.parse().ok()?, parts.next()?.parse().ok()?, parts.next()?.parse().ok()?);
    if parts.next().is_some() {
        return None;
    }
    Some(version)
}

/// A language tag folder name: `pt-BR`, `en-US` (letters, digits and hyphens). Anything else is not a language folder.
fn is_language_tag(text: &str) -> bool {
    !text.is_empty() && text.chars().all(|character| character.is_ascii_alphanumeric() || character == '-')
}

/// Embeds the short release notes (TASK-099) and their translations (TASK-102):
/// - `docs/release-notes/launcher/<version>.md` is the en-US text (the fallback for every language);
/// - `docs/release-notes/launcher/<tag>/<version>.md` is the text in another language (for example `pt-BR`). Any tag
///   folder is embedded, so a new language needs no Rust change; a version without a translation shows en-US.
/// The result is `RELEASE_NOTES: &[(tag, version, markdown)]` in `$OUT_DIR/release_notes.rs`. The version of this build
/// must have its en-US note at the top folder, or the build stops.
fn embed_release_notes() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let folder = manifest.join("../../../docs/release-notes/launcher");
    println!("cargo:rerun-if-changed={}", folder.display());
    let current_text = std::env::var("CARGO_PKG_VERSION").expect("CARGO_PKG_VERSION");
    let current = parse_version(&current_text).expect("the launcher version is x.y.z");
    // (tag, version, path); the top folder is the en-US note.
    let mut notes: Vec<(String, (u32, u32, u32), String, PathBuf)> = Vec::new();
    for entry in std::fs::read_dir(&folder).expect("docs/release-notes/launcher is readable") {
        let path = entry.expect("a release notes entry").path();
        if path.is_dir() {
            let Some(tag) = path.file_name().and_then(|name| name.to_str()).map(str::to_string) else { continue };
            if !is_language_tag(&tag) {
                continue;
            }
            println!("cargo:rerun-if-changed={}", path.display());
            for translated in std::fs::read_dir(&path).expect("a language folder is readable") {
                let file = translated.expect("a translated release notes entry").path();
                if let Some(version) = note_version(&file, current) {
                    println!("cargo:rerun-if-changed={}", file.display());
                    notes.push((tag.clone(), version, file.file_stem().unwrap().to_str().unwrap().to_string(), file.canonicalize().expect("a release notes path")));
                }
            }
        } else if let Some(version) = note_version(&path, current) {
            println!("cargo:rerun-if-changed={}", path.display());
            notes.push(("en-US".to_string(), version, path.file_stem().unwrap().to_str().unwrap().to_string(), path.canonicalize().expect("a release notes path")));
        }
    }
    assert!(
        notes.iter().any(|note| note.0 == "en-US" && note.1 == current),
        "docs/release-notes/launcher/{current_text}.md is missing: every launcher version ships its own en-US short release notes"
    );
    notes.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    let mut source = String::from("/// Release notes embedded by build.rs: (language tag, version, Markdown), newest version first.\n");
    source.push_str("pub const RELEASE_NOTES: &[(&str, &str, &str)] = &[\n");
    for (tag, _, version, path) in &notes {
        source.push_str(&format!("    ({tag:?}, {version:?}, include_str!({:?})),\n", path.display().to_string()));
    }
    source.push_str("];\n");
    let out = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));
    std::fs::write(Path::new(&out).join("release_notes.rs"), source).expect("write release_notes.rs");
}

/// The version of a release notes file (`<version>.md`) that is not newer than this build, or `None`.
fn note_version(path: &Path, current: (u32, u32, u32)) -> Option<(u32, u32, u32)> {
    if path.extension().and_then(|ext| ext.to_str()) != Some("md") {
        return None;
    }
    let version = parse_version(path.file_stem()?.to_str()?)?;
    (version <= current).then_some(version)
}

fn main() {
    embed_release_notes();
    println!("cargo:rerun-if-changed=commands.list");
    println!("cargo:rerun-if-changed=capabilities");
    let manifest = tauri_build::AppManifest::new().commands(command_names());
    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(manifest))
        .expect("Tauri build script failed");
}
