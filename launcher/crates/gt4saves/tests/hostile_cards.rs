// Reader tests on synthetic cards (TASK-104): every case gives the right result or a clear error, never a panic, a stack
// overflow or an allocation abort. The cards come from tests/common (no real game data).

mod common;

use common::*;
use gt4saves::card::{same_contents, CardError, Node, MAGIC};
use gt4saves::{Card, Library, SaveNames};

const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
use std::fs;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn names(nodes: &[Node]) -> Vec<String> {
    nodes.iter().map(Node::name).collect()
}

fn expect_corrupt<T: std::fmt::Debug>(result: Result<T, CardError>) {
    match result {
        Err(CardError::Corrupt(_)) => {}
        other => panic!("expected a corrupt-card error, got {other:?}"),
    }
}

#[test]
fn a_card_with_saves_of_several_games_lists_each_one() {
    let items = vec![
        game_save(1),
        replay(1),
        dir("SLUS-20000GAMEDATA", vec![file("icon.sys", pattern(964, 9))]),
        dir("SCUS-97436GAMEDATA", vec![file("icon.sys", pattern(964, 8))]),
    ];
    let image = synth(8192, 41, 8135, &items);
    let card = Card::from_image(&image).unwrap();
    assert_eq!(
        names(&card.root().unwrap()),
        ["BASCUS-97328GAMEDATA", "BASCUS-97328R0001", "SLUS-20000GAMEDATA", "SCUS-97436GAMEDATA"]
    );
    // Import keeps the Gran Turismo 4 items and names the other games' saves as skipped.
    let library = Library::open(&scratch("several-games"), GT4).unwrap();
    let report = library.import_card_image(&image).unwrap();
    assert_eq!(report.added, vec!["Save 01".to_string(), "BASCUS-97328R0001".to_string()]);
    assert_eq!(report.skipped.len(), 2, "{:?}", report.skipped);
}

#[test]
fn a_directory_that_spans_many_clusters_reads_every_entry() {
    let files: Vec<Node> = (0..60).map(|i| file(&format!("file{i:02}"), pattern(i * 37 % 3000, i as u8))).collect();
    let items = vec![dir("BASCUS-97328GAMEDATA", files.clone())];
    let image = synth(8192, 41, 8135, &items);
    let card = Card::from_image(&image).unwrap();
    let root = card.root().unwrap();
    assert!(card.chain(root[0].entry.cluster()).unwrap().len() > 2, "the directory spans several clusters");
    assert_eq!(root[0].children.len(), 60);
    for (got, want) in root[0].children.iter().zip(&files) {
        assert_eq!(got.name(), want.name());
        assert!(got.data == want.data, "data of {}", want.name());
    }
}

#[test]
fn sixteen_and_sixty_four_megabyte_cards_read_back_in_every_form() {
    let items = vec![game_save(3), replay(2)];
    // (clusters, alloc_offset, alloc_end): the allocation area ends 57 clusters before the card's end, as on 8 MB.
    for (clusters, alloc_offset, alloc_end) in [(16384u32, 80u32, 16327u32), (65536, 300, 65479)] {
        let image = synth(clusters, alloc_offset, alloc_end, &items);
        let card = Card::from_image(&image).unwrap_or_else(|e| panic!("{clusters} clusters: {e}"));
        assert_eq!(card.layout().clusters, clusters);
        assert!(same_contents(&card.root().unwrap(), &items), "{clusters} clusters, data-only form");

        let with_spare = card.to_image();
        assert_eq!(with_spare.len(), clusters as usize * 2 * 528);
        let spare_form = Card::from_image(&with_spare).unwrap();
        assert!(spare_form.ecc_ok());
        assert!(same_contents(&spare_form.root().unwrap(), &items), "{clusters} clusters, spare form");

        let data_only = card.to_image_without_ecc();
        assert_eq!(data_only.len(), clusters as usize * 2 * 512);
        let again = Card::from_image(&data_only).unwrap();
        assert!(same_contents(&again.root().unwrap(), &items), "{clusters} clusters, data-only dump");
    }
}

#[test]
fn a_superblock_with_another_alloc_offset_is_followed() {
    let items = vec![game_save(4), replay(3)];
    let image = synth(8192, 60, 8135, &items);
    let card = Card::from_image(&image).unwrap();
    assert_eq!(card.layout().alloc_offset, 60);
    assert!(same_contents(&card.root().unwrap(), &items));
}

#[test]
fn a_directory_that_points_back_to_an_ancestor_is_corrupt_not_a_crash() {
    let items = vec![dir("BASCUS-97328GAMEDATA", vec![file("icon.sys", pattern(964, 1))])];
    let base = synth(8192, 41, 8135, &items);
    let mut image = base.clone();
    // The game directory is root entry 2 (after "." and ".."): its cluster becomes the root, its count 4.
    let at = root_byte(&base, 2 * 512);
    put_u32(&mut image, at + 0x10, 0);
    put_u32(&mut image, at + 4, 4);
    expect_corrupt(Card::from_image(&image).unwrap().root());
}

#[test]
fn directories_nested_past_the_depth_limit_are_refused() {
    fn nest(levels: usize) -> Vec<Node> {
        let mut node = file("leaf", pattern(10, 1));
        for level in 0..levels {
            node = dir(&format!("D{level}"), vec![node]);
        }
        vec![node]
    }
    // Eight levels of directories are read; a ninth is refused.
    let eight = Card::from_image(&synth(8192, 41, 8135, &nest(8))).unwrap();
    assert!(eight.root().is_ok());
    let nine = Card::from_image(&synth(8192, 41, 8135, &nest(9))).unwrap();
    match nine.root() {
        Err(CardError::Corrupt(what)) => assert!(what.contains("nested"), "{what}"),
        other => panic!("expected a depth error, got {other:?}"),
    }
}

#[test]
fn a_huge_or_too_long_length_is_refused_before_anything_is_allocated() {
    let items = vec![file("F", pattern(10, 1))]; // root entry 2
    let base = synth(8192, 41, 8135, &items);
    // A file length of 4 GB.
    let mut image = base.clone();
    put_u32(&mut image, root_byte(&base, 2 * 512) + 4, u32::MAX);
    expect_corrupt(Card::from_image(&image).unwrap().root());
    // A file length longer than its one-cluster chain.
    let mut image = base.clone();
    put_u32(&mut image, root_byte(&base, 2 * 512) + 4, 5000);
    expect_corrupt(Card::from_image(&image).unwrap().root());
    // The root's own count of 4 billion entries.
    let mut image = base.clone();
    put_u32(&mut image, root_byte(&base, 0) + 4, u32::MAX);
    expect_corrupt(Card::from_image(&image).unwrap().root());
}

#[test]
fn a_fat_chain_that_loops_is_corrupt() {
    let base = synth(8192, 41, 8135, &[file("F", pattern(3000, 2))]);
    let card = Card::from_image(&base).unwrap();
    let root = card.root().unwrap();
    let first = root[0].entry.cluster();
    let chain = card.chain(first).unwrap();
    assert_eq!(chain.len(), 3);
    let mut image = base.clone();
    // The last cluster of the chain now points back to the first.
    put_u32(&mut image, fat_entry_byte(chain[2]), 0x8000_0000 | chain[0]);
    expect_corrupt(Card::from_image(&image).unwrap().root());
}

#[test]
fn an_unformatted_card_is_not_a_card() {
    for length in [8_650_752usize, 8_388_608] {
        match Card::from_image(&vec![0xFFu8; length]) {
            Err(CardError::NotACard) => {}
            other => panic!("{length} bytes of 0xFF: {other:?}"),
        }
    }
}

#[test]
fn wrong_sizes_and_damaged_superblocks_get_clear_errors() {
    assert_eq!(Card::from_image(&vec![0u8; 1000]).unwrap_err(), CardError::BadSize(1000));

    let four_mb = synth(4096, 41, 4039, &[]);
    assert_eq!(four_mb.len(), 4_194_304);
    assert!(matches!(Card::from_image(&four_mb), Err(CardError::Unsupported(_))));

    let base = synth(8192, 41, 8135, &[game_save(5)]);
    let mut odd = base.clone();
    odd.push(0);
    assert!(matches!(Card::from_image(&odd), Err(CardError::Unsupported(_))));

    let mut other_clusters = base.clone();
    put_u32(&mut other_clusters, 0x30, 16384);
    expect_corrupt(Card::from_image(&other_clusters));

    let mut bad_page = base.clone();
    bad_page[0x28..0x2A].copy_from_slice(&1024u16.to_le_bytes());
    assert!(matches!(Card::from_image(&bad_page), Err(CardError::Unsupported(_))));

    // `alloc_end` counts clusters from `alloc_offset`: 9000 is capped by the room on the card (8192 - 41).
    let mut alloc_end_large = base.clone();
    put_u32(&mut alloc_end_large, 0x38, 9000);
    assert!(Card::from_image(&alloc_end_large).is_ok());

    let mut alloc_end_zero = base.clone();
    put_u32(&mut alloc_end_zero, 0x38, 0);
    expect_corrupt(Card::from_image(&alloc_end_zero));

    let mut alloc_order = base.clone();
    put_u32(&mut alloc_order, 0x34, 9000);
    expect_corrupt(Card::from_image(&alloc_order));

    let mut ifc_outside = base.clone();
    put_u32(&mut ifc_outside, 0x50, 99_999);
    expect_corrupt(Card::from_image(&ifc_outside));

    let mut no_magic = base.clone();
    no_magic[..MAGIC.len()].fill(0);
    assert_eq!(Card::from_image(&no_magic).unwrap_err(), CardError::NotACard);
}

/// Moves the only file of a one-file card to a one-cluster chain at `relative` and returns the image.
fn file_moved_to(clusters: u32, alloc_offset: u32, alloc_end: u32, relative: u32, data: &[u8]) -> Vec<u8> {
    let base = synth(clusters, alloc_offset, alloc_end, &[file("F", data.to_vec())]);
    let mut image = base.clone();
    // The file is root entry 2 (after "." and ".."); point it at `relative` and end the chain there.
    let at = root_byte(&base, 2 * 512);
    put_u32(&mut image, at + 0x10, relative);
    put_u32(&mut image, fat_entry_byte(relative), 0xFFFF_FFFF);
    let physical = (alloc_offset + relative) as usize * CLUSTER;
    image[physical..physical + data.len()].copy_from_slice(data);
    image
}

#[test]
fn a_chain_entry_past_the_old_absolute_bound_but_inside_the_relative_area_is_read() {
    let data = pattern(500, 7);
    let image = file_moved_to(8192, 41, 8135, 8100, &data);
    let root = Card::from_image(&image).unwrap().root().unwrap();
    assert_eq!(root[0].data, data);
}

#[test]
fn relative_cluster_7937_reads_on_a_card_with_alloc_end_7978() {
    let data = pattern(500, 9);
    let image = file_moved_to(8192, 41, 7978, 7937, &data);
    let card = Card::from_image(&image).unwrap();
    assert_eq!(card.layout().alloc_end, 7978);
    assert_eq!(card.root().unwrap()[0].data, data);
}

#[test]
fn a_chain_entry_equal_to_the_usable_clusters_is_still_corrupt() {
    for (alloc_end, usable) in [(8135u32, 8135u32), (7978, 7978)] {
        let image = file_moved_to(8192, 41, alloc_end, 100, &pattern(10, 1));
        let mut broken = image.clone();
        let base = synth(8192, 41, alloc_end, &[file("F", pattern(10, 1))]);
        put_u32(&mut broken, root_byte(&base, 2 * 512) + 0x10, usable);
        match Card::from_image(&broken).unwrap().root() {
            Err(CardError::Corrupt(what)) => assert!(what.contains("outside the allocation area"), "{what}"),
            other => panic!("expected a chain error, got {other:?}"),
        }
    }
}
