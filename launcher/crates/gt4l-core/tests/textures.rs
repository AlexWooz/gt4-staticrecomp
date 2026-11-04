//! Custom textures (TASK-088, gt4-gs 1.2.0; TASK-091 moved the folder to `<install>\data\Textures`): the Textures folder, the folder the launcher
//! makes on demand (TASK-093: no replacements subfolder), and the two Graphics rows (Load custom textures on by default, Precache off by default).

use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

use gt4l_core::gs_table;
use gt4l_core::paths::{ensure_texture_folders, DataDirs};

fn scratch(name: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!("gt4l-textures-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

#[test]
fn the_textures_folder_is_inside_the_data_folder_with_no_serial_folder() {
    let install = scratch("root");
    let dirs = DataDirs::new(install.join("data"));
    assert_eq!(dirs.textures(), install.join("data").join("Textures"));
    assert!(!dirs.textures().exists(), "resolving creates nothing");
}

#[test]
fn a_textures_folder_not_moved_yet_stays_in_use() {
    let base = scratch("in-place");
    let mut dirs = DataDirs::new(base.join("data"));
    let documents = base.join("Documents").join("Gran Turismo 4").join("Textures");
    dirs.set_textures_in_place(Some(documents.clone()));
    assert_eq!(dirs.textures(), documents);
}

#[test]
fn the_launcher_makes_the_textures_folder_on_demand_and_no_replacements_folder() {
    let textures = scratch("ensure").join("Gran Turismo 4").join("Textures");
    ensure_texture_folders(&textures).unwrap();
    assert!(textures.is_dir());
    assert!(!textures.join("replacements").exists(), "gt4-gs 1.2.1 reads Textures itself");
    // Idempotent: a second call keeps what is there.
    fs::write(textures.join("pack.txt"), b"kept").unwrap();
    ensure_texture_folders(&textures).unwrap();
    assert_eq!(fs::read(textures.join("pack.txt")).unwrap(), b"kept");
}

#[test]
fn load_custom_textures_is_on_by_default_and_always_written() {
    let row = gs_table::row("texture_load").unwrap();
    assert_eq!((row.page.as_str(), row.default.as_str(), row.written.as_str(), row.status.as_str()), ("basic", "true", "always", "ok"));
    assert!(gs_table::offered().iter().any(|offered| offered.id == "texture_load"));
    let text = gs_table::render_ini(&BTreeMap::new(), &BTreeMap::new()).unwrap();
    assert!(text.contains("LoadTextureReplacements = true\n"), "{text}");
    let off = BTreeMap::from([("texture_load".to_string(), "false".to_string())]);
    let text = gs_table::render_ini(&off, &BTreeMap::new()).unwrap();
    assert!(text.contains("LoadTextureReplacements = false\n"), "{text}");
}

#[test]
fn precache_is_hidden_off_by_default_and_written_only_when_on() {
    // TASK-118 (owner, 2026-10-26): the row left the page; the key is still written when a stored value turns it on.
    let row = gs_table::row("texture_precache").unwrap();
    assert_eq!((row.page.as_str(), row.default.as_str(), row.written.as_str()), ("hidden", "false", "when-set"));
    assert!(!gs_table::offered().iter().any(|offered| offered.id == "texture_precache"));
    let text = gs_table::render_ini(&BTreeMap::new(), &BTreeMap::new()).unwrap();
    assert!(!text.contains("PrecacheTextureReplacements"), "{text}");
    let on = BTreeMap::from([("texture_precache".to_string(), "true".to_string())]);
    let text = gs_table::render_ini(&on, &BTreeMap::new()).unwrap();
    assert!(text.contains("PrecacheTextureReplacements = true\n"), "{text}");
}

#[test]
fn texture_dumping_is_not_offered() {
    for id in ["texture_dump", "texture_dump_mipmaps", "texture_dump_fmv"] {
        assert!(!gs_table::offered().iter().any(|offered| offered.id == id), "{id} is not offered");
    }
}

#[test]
fn the_textures_folder_is_written_only_when_the_launcher_supplies_it() {
    let launcher = BTreeMap::from([("textures_folder".to_string(), "C:/Users/x/Documents/Gran Turismo 4/Textures".to_string())]);
    let text = gs_table::render_ini(&BTreeMap::new(), &launcher).unwrap();
    assert!(text.contains("[Folders]\nTextures = C:/Users/x/Documents/Gran Turismo 4/Textures\n"), "{text}");
}

// TASK-157: a textures folder the player chose, read in place.

use gt4l_core::textures::{check_textures_path, resolve_location, TextureLocation, TexturesPathError};

fn snapshot(folder: &std::path::Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(folder).unwrap().map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned()).collect();
    names.sort();
    names
}

#[test]
fn a_chosen_folder_wins_over_the_default_and_over_a_folder_not_moved_yet() {
    let base = scratch("choice-wins");
    let data = base.join("data");
    let mut dirs = DataDirs::new(&data);
    dirs.set_textures_in_place(Some(base.join("Documents").join("Textures")));
    let player = base.join("pcsx2").join("textures");
    fs::create_dir_all(&player).unwrap();
    let (location, note) = resolve_location(&dirs.textures(), Some(&player), dirs.root());
    assert_eq!(location.folder, player);
    assert!(location.custom);
    assert_eq!(note, None);
    let (default, _) = resolve_location(&dirs.textures(), None, dirs.root());
    assert!(!default.custom);
    assert_eq!(default.folder, dirs.textures());
}

#[test]
fn a_missing_folder_falls_back_to_the_default_with_a_log_line() {
    let base = scratch("choice-missing");
    let dirs = DataDirs::new(base.join("data"));
    let gone = base.join("unplugged").join("textures");
    let (location, note) = resolve_location(&dirs.textures(), Some(&gone), dirs.root());
    assert_eq!(location.folder, dirs.textures());
    assert!(!location.custom);
    assert_eq!(note, Some(format!("textures: {} not found; using {}", gone.display(), dirs.textures().display())));
}

#[test]
fn a_chosen_folder_is_checked_before_it_is_used() {
    let base = scratch("choice-check");
    let data = base.join("data");
    fs::create_dir_all(data.join("gs").join("1.2.1")).unwrap();
    let file = base.join("pack.zip");
    fs::write(&file, b"x").unwrap();
    let player = base.join("textures");
    fs::create_dir_all(&player).unwrap();

    assert_eq!(check_textures_path(std::path::Path::new("textures"), &data), Err(TexturesPathError::NotAbsolute));
    assert_eq!(check_textures_path(&base.join("nope"), &data), Err(TexturesPathError::NotFound));
    assert_eq!(check_textures_path(&file, &data), Err(TexturesPathError::NotAFolder));
    assert_eq!(check_textures_path(&data, &data), Err(TexturesPathError::DataRoot));
    assert_eq!(check_textures_path(&data.join("gs"), &data), Err(TexturesPathError::InsideRenderer));
    assert_eq!(check_textures_path(&data.join("gs").join("1.2.1"), &data), Err(TexturesPathError::InsideRenderer));
    // A roundabout spelling of the data folder is still the data folder.
    assert_eq!(check_textures_path(&data.join("gs").join(".."), &data), Err(TexturesPathError::DataRoot));
    // A good folder is accepted; a trailing separator is dropped.
    assert_eq!(check_textures_path(&player, &data), Ok(player.clone()));
    let with_separator = std::path::PathBuf::from(format!("{}{}", player.display(), std::path::MAIN_SEPARATOR));
    assert_eq!(check_textures_path(&with_separator, &data), Ok(player.clone()));
    // The default Textures folder of the data folder is allowed (it is what the default is).
    fs::create_dir_all(data.join("Textures")).unwrap();
    assert!(check_textures_path(&data.join("Textures"), &data).is_ok());
    // Every error has a launcher text.
    for error in [TexturesPathError::NotAbsolute, TexturesPathError::NotFound, TexturesPathError::NotAFolder, TexturesPathError::DataRoot, TexturesPathError::InsideRenderer] {
        assert!(gt4l_core::messages::text(error.message_id(), gt4l_core::language::Language::fallback()) != error.message_id());
    }
}

#[test]
fn an_invalid_choice_that_exists_also_falls_back_and_says_why() {
    let base = scratch("choice-invalid");
    let data = base.join("data");
    fs::create_dir_all(data.join("gs")).unwrap();
    let default = data.join("Textures");
    let (location, note) = resolve_location(&default, Some(&data.join("gs")), &data);
    assert_eq!(location.folder, default);
    assert!(note.unwrap().contains("cannot be used (textures.invalid_renderer)"));
}

#[test]
fn resolving_and_checking_a_chosen_folder_writes_nothing_into_it() {
    let base = scratch("choice-readonly");
    let data = base.join("data");
    let player = base.join("pcsx2").join("textures");
    fs::create_dir_all(player.join("replacements")).unwrap();
    fs::write(player.join("replacements").join("a.png"), b"png").unwrap();
    fs::write(player.join("b.png"), b"png").unwrap();
    let before = (snapshot(&player), snapshot(&player.join("replacements")), snapshot(player.parent().unwrap()));
    let (location, _) = resolve_location(&data.join("Textures"), Some(&player), &data);
    assert!(location.custom, "Play skips ensure_texture_folders and the flatten for a custom folder");
    let after = (snapshot(&player), snapshot(&player.join("replacements")), snapshot(player.parent().unwrap()));
    assert_eq!(before, after, "a nested replacements folder stays where it is and nothing is created beside the folder");
}

#[test]
fn the_shader_cache_is_beside_the_default_folder_and_in_the_data_folder_for_a_custom_one() {
    let base = scratch("choice-cache");
    let data = base.join("data");
    let default = TextureLocation::default_folder(&data.join("Textures"));
    assert_eq!(default.shader_cache, Some(data.join("shader-cache")));
    let player = base.join("pcsx2").join("textures");
    let custom = TextureLocation::custom_folder(&player, &data);
    assert_eq!(custom.shader_cache, Some(data.join("shader-cache")));
    assert!(!custom.shader_cache.unwrap().starts_with(player.parent().unwrap()));
}
