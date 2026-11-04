//! Development only (feature `visual-check`): fills an empty save library with synthetic saves so
//! the Saves and Replays pages can be captured. Enabled by `GT4RECOMP_VISUAL_SEED=1`. The saves are
//! synthetic test data, not game data.

use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4l_core::games::{self, Game, GT4};
use gt4saves::Library;
use gt4saves::Card;

use crate::saves;
use crate::state::AppState;

const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
const REPLAY: &str = "BASCUS-97328R0001";

fn file(name: &str, data: Vec<u8>) -> Node {
    Node::file(DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).expect("valid entry"), data)
}

fn directory(name: &str, children: Vec<Node>) -> Node {
    Node::dir(DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).expect("valid entry"), children)
}

/// A synthetic game save: a few files whose contents depend on `seed`, so two saves differ.
fn game_save(seed: u8) -> Node {
    let pattern = |len: usize| (0..len).map(|i| (i as u8).wrapping_mul(7).wrapping_add(seed)).collect();
    directory(GT4.save_folder, vec![file("icon.sys", pattern(964)), file("garage", pattern(20_000))])
}

fn replay() -> Node {
    directory(REPLAY, vec![file("icon.sys", vec![1; 964]), file("replay.rep", vec![9; 5_000])])
}

/// Imports three synthetic cards: two saves (the first made active) and one replay.
pub fn seed(state: &AppState) {
    if std::env::var("GT4RECOMP_VISUAL_SEED").as_deref() != Ok("1") {
        return;
    }
    if saves::saves(state.library()).map(|items| !items.is_empty()).unwrap_or(true) {
        return;
    }
    let folder = state.dirs.root().join("visual-seed");
    let _ = std::fs::create_dir_all(&folder);
    let cards = [
        ("save-a.ps2", vec![game_save(1)]),
        ("save-b.ps2", vec![game_save(2)]),
        ("replay.ps2", vec![game_save(3), replay()]),
    ];
    for (name, items) in cards {
        let image = Card::build(&items).expect("synthetic card builds").to_image();
        let path = folder.join(name);
        if std::fs::write(&path, image).is_ok() {
            let libraries: Vec<(&'static Game, &Library)> = games::GAMES.iter().map(|game| (*game, state.library_of(game))).collect();
            let _ = saves::import_path(&libraries, &path, crate::state::language_of(&state));
        }
    }
    let _ = saves::activate(state.library(), "Save 01");
}
