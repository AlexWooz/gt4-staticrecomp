//! Builds a memory card image for the CoreGS 2.0 recorded drives (`scripts/coregs2_bench/record-input.sh`), the way the
//! launcher builds the card of a Play (`Library::build_card`): the active game save of one library plus the replays of
//! that library and of any other libraries given, read back before it is written.
//!
//! usage: cargo run -p gt4saves --example bench_card -- <gt4|spec2> <out.ps2> <save library> [replay library ...]
//! A library is a game's folder holding `Saves/Active` and `Replays` (`%APPDATA%\GT4Recomp\games\<game>`, or a copy).
//! Nothing in the libraries is written; an existing output file is refused.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use gt4saves::card::same_contents;
use gt4saves::{folder, Card, Node, SaveNames};

fn names_of(game: &str) -> Option<SaveNames> {
    match game {
        "gt4" => Some(SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328")),
        "spec2" => Some(SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436")),
        _ => None,
    }
}

/// The replay folders of one library, by name.
fn replays_of(library: &Path, names: &SaveNames) -> Result<Vec<PathBuf>, String> {
    let folder = library.join("Replays");
    let mut found = Vec::new();
    let entries = std::fs::read_dir(&folder).map_err(|error| format!("{}: {error}", folder.display()))?;
    for entry in entries {
        let path = entry.map_err(|error| error.to_string())?.path();
        let name = path.file_name().and_then(|name| name.to_str()).unwrap_or_default().to_string();
        if path.is_dir() && names.is_replay(&name) {
            found.push(path);
        }
    }
    found.sort();
    Ok(found)
}

fn build(game: &str, out: &Path, save_library: &Path, replay_libraries: &[PathBuf]) -> Result<(), String> {
    let names = names_of(game).ok_or_else(|| format!("unknown game '{game}': gt4 or spec2"))?;
    if out.exists() {
        return Err(format!("{} exists: choose another name", out.display()));
    }
    let save = save_library.join("Saves").join("Active").join(names.game_save);
    let mut items: Vec<Node> = vec![folder::import_item(&save).map_err(|error| format!("{}: {error}", save.display()))?];
    println!("save    {}", save.display());
    let mut seen = Vec::new();
    for library in std::iter::once(&save_library.to_path_buf()).chain(replay_libraries) {
        for replay in replays_of(library, &names)? {
            let name = replay.file_name().unwrap_or_default().to_string_lossy().to_string();
            if seen.contains(&name) {
                println!("same name already on the card, left out: {}", replay.display());
                continue;
            }
            items.push(folder::import_item(&replay).map_err(|error| format!("{}: {error}", replay.display()))?);
            println!("replay  {}", replay.display());
            seen.push(name);
        }
    }
    let card = Card::build(&items).map_err(|error| error.to_string())?;
    let bytes = card.to_image();
    let readback = Card::from_image(&bytes).and_then(|card| card.root()).map_err(|error| error.to_string())?;
    if !same_contents(&readback, &items) {
        return Err("the built card does not read back identical".into());
    }
    std::fs::write(out, &bytes).map_err(|error| format!("{}: {error}", out.display()))?;
    println!("card    {} ({} items, {} bytes)", out.display(), items.len(), bytes.len());
    Ok(())
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 3 {
        eprintln!("usage: bench_card <gt4|spec2> <out.ps2> <save library> [replay library ...]");
        return ExitCode::from(2);
    }
    let extra: Vec<PathBuf> = args[3..].iter().map(PathBuf::from).collect();
    match build(&args[0], Path::new(&args[1]), Path::new(&args[2]), &extra) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("bench_card: {message}");
            ExitCode::FAILURE
        }
    }
}
