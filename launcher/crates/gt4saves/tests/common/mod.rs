// Synthetic PS2 memory cards for the reader tests. The cards are written here, in the test code, from a layout
// chosen by the test (clusters, alloc_offset, alloc_end). Nothing is copied from a real card.
//
// Data-only images (512-byte pages, no spare bytes) are built, because a cluster is then 1024 contiguous bytes and a
// patch is one offset. `Card::from_image` computes the spare bytes.

#![allow(dead_code)]

use gt4saves::card::{DirEntry, Node, MAGIC, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
use gt4saves::Card;

pub const STAMP: [u8; 8] = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
pub const CLUSTER: usize = 1024;
/// Physical cluster that lists the FAT clusters in the synthetic cards (as on the launcher's own cards).
const IFC: u32 = 8;
const FREE: u32 = 0x7FFF_FFFF;
const END: u32 = 0xFFFF_FFFF;
const IN_USE: u32 = 0x8000_0000;

pub fn file(name: &str, data: Vec<u8>) -> Node {
    Node::file(DirEntry::new(name, MODE_FILE_DEFAULT, STAMP, STAMP).unwrap(), data)
}

pub fn dir(name: &str, children: Vec<Node>) -> Node {
    Node::dir(DirEntry::new(name, MODE_DIR_DEFAULT, STAMP, STAMP).unwrap(), children)
}

pub fn pattern(len: usize, seed: u8) -> Vec<u8> {
    (0..len).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
}

/// A GT4 save with a small garage file.
pub fn game_save(seed: u8) -> Node {
    dir(
        "BASCUS-97328GAMEDATA",
        vec![file("icon.sys", pattern(964, seed)), file("garage", pattern(20_000, seed.wrapping_add(1)))],
    )
}

pub fn replay(number: u32) -> Node {
    dir(&format!("BASCUS-97328R{number:04}"), vec![file("replay.rep", pattern(3000, number as u8))])
}

/// A data-only image of a card with `clusters` clusters. The allocation area starts at physical cluster `alloc_offset`
/// and holds `alloc_end` relative clusters (capped by the room on the card). The FAT lives at physical clusters 9 and up,
/// so `alloc_offset` must lie after it.
pub fn synth(clusters: u32, alloc_offset: u32, alloc_end: u32, items: &[Node]) -> Vec<u8> {
    let usable = alloc_end.min(clusters - alloc_offset);
    let fat_clusters = usable.div_ceil(256);
    assert!(fat_clusters <= 256 && 9 + fat_clusters <= alloc_offset, "layout does not fit the synthetic card");
    let mut image = vec![0xFFu8; clusters as usize * CLUSTER];

    // Superblock (page 0).
    let superblock = &mut image[..512];
    superblock[..MAGIC.len()].copy_from_slice(MAGIC);
    superblock[0x28..0x2A].copy_from_slice(&512u16.to_le_bytes());
    superblock[0x2A..0x2C].copy_from_slice(&2u16.to_le_bytes());
    superblock[0x2C..0x2E].copy_from_slice(&16u16.to_le_bytes());
    superblock[0x30..0x34].copy_from_slice(&clusters.to_le_bytes());
    superblock[0x34..0x38].copy_from_slice(&alloc_offset.to_le_bytes());
    superblock[0x38..0x3C].copy_from_slice(&alloc_end.to_le_bytes());
    superblock[0x3C..0x40].copy_from_slice(&0u32.to_le_bytes());
    for slot in 0..32usize {
        let value = if slot == 0 { IFC } else { END };
        superblock[0x50 + slot * 4..0x54 + slot * 4].copy_from_slice(&value.to_le_bytes());
    }

    // Indirect list: entry j names FAT cluster 9 + j.
    let mut indirect = vec![0xFFu8; CLUSTER];
    for j in 0..fat_clusters as usize {
        indirect[j * 4..j * 4 + 4].copy_from_slice(&(9 + j as u32).to_le_bytes());
    }
    let ifc_start = IFC as usize * CLUSTER;
    image[ifc_start..ifc_start + CLUSTER].copy_from_slice(&indirect);

    let mut writer = Writer { image, fat: vec![FREE; usable as usize], next: 1, alloc_offset };
    // The root's own chain starts at relative cluster 0.
    writer.fat[0] = IN_USE;
    let root_children = writer.write_children(items, 0);
    let count = root_children.len() + 2;
    let blocks = (count * 512).div_ceil(CLUSTER);
    let mut root_clusters = vec![0u32];
    for _ in 1..blocks {
        root_clusters.push(writer.allocate());
    }
    let mut dot = DirEntry::new(".", MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
    dot.set_length(count as u32);
    dot.set_cluster(0);
    let mut dotdot = DirEntry::new("..", 0xA426, STAMP, STAMP).unwrap();
    dotdot.set_cluster(0);
    let mut raw = Vec::new();
    raw.extend_from_slice(dot.raw());
    raw.extend_from_slice(dotdot.raw());
    for entry in &root_children {
        raw.extend_from_slice(entry.raw());
    }
    raw.resize(blocks * CLUSTER, 0);
    writer.write_chain(&root_clusters, &raw);

    // FAT clusters.
    for (relative, value) in writer.fat.clone().iter().enumerate() {
        let physical = (9 + relative / 256) * CLUSTER + (relative % 256) * 4;
        writer.image[physical..physical + 4].copy_from_slice(&value.to_le_bytes());
    }
    writer.image
}

struct Writer {
    image: Vec<u8>,
    fat: Vec<u32>,
    next: u32,
    alloc_offset: u32,
}

impl Writer {
    fn allocate(&mut self) -> u32 {
        let relative = self.next;
        assert!((relative as usize) < self.fat.len(), "synthetic card is full");
        self.next += 1;
        relative
    }

    fn write_chain(&mut self, clusters: &[u32], data: &[u8]) {
        for (i, &relative) in clusters.iter().enumerate() {
            self.fat[relative as usize] = match clusters.get(i + 1) {
                Some(&next) => IN_USE | next,
                None => END,
            };
            let mut block = vec![0u8; CLUSTER];
            let start = i * CLUSTER;
            if start < data.len() {
                let end = (start + CLUSTER).min(data.len());
                block[..end - start].copy_from_slice(&data[start..end]);
            }
            let physical = (self.alloc_offset + relative) as usize * CLUSTER;
            self.image[physical..physical + CLUSTER].copy_from_slice(&block);
        }
    }

    fn write_children(&mut self, items: &[Node], parent: u32) -> Vec<DirEntry> {
        let mut entries = Vec::new();
        for (position, item) in items.iter().enumerate() {
            let mut entry = item.entry.clone();
            if item.entry.is_dir() {
                let count = item.children.len() + 2;
                let blocks = (count * 512).div_ceil(CLUSTER);
                let clusters: Vec<u32> = (0..blocks).map(|_| self.allocate()).collect();
                let children = self.write_children(&item.children, clusters[0]);
                let mut dot = DirEntry::new(".", MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
                dot.set_cluster(0);
                dot.set_dir_entry(position as u32 + 2);
                let mut dotdot = DirEntry::new("..", MODE_DIR_DEFAULT, STAMP, STAMP).unwrap();
                dotdot.set_cluster(parent);
                let mut raw = Vec::new();
                raw.extend_from_slice(dot.raw());
                raw.extend_from_slice(dotdot.raw());
                for child in &children {
                    raw.extend_from_slice(child.raw());
                }
                raw.resize(blocks * CLUSTER, 0);
                self.write_chain(&clusters, &raw);
                entry.set_cluster(clusters[0]);
                entry.set_length(count as u32);
            } else {
                let blocks = item.data.len().div_ceil(CLUSTER);
                let clusters: Vec<u32> = (0..blocks).map(|_| self.allocate()).collect();
                self.write_chain(&clusters, &item.data);
                entry.set_cluster(clusters.first().copied().unwrap_or(END));
                entry.set_length(item.data.len() as u32);
            }
            entries.push(entry);
        }
        entries
    }
}

/// Byte offset in an unpatched data-only image of byte `offset` of the root directory's data (its chain as the card
/// reads it now).
pub fn root_byte(image: &[u8], offset: usize) -> usize {
    let card = Card::from_image(image).expect("unpatched image opens");
    let chain = card.chain(0).unwrap();
    let alloc = card.layout().alloc_offset as usize;
    (alloc + chain[offset / CLUSTER] as usize) * CLUSTER + offset % CLUSTER
}

/// Byte offset in a data-only image of the FAT entry of relative cluster `relative` (8 MB-style FAT placement).
pub fn fat_entry_byte(relative: u32) -> usize {
    (9 + relative as usize / 256) * CLUSTER + (relative as usize % 256) * 4
}

pub fn put_u32(image: &mut [u8], at: usize, value: u32) {
    image[at..at + 4].copy_from_slice(&value.to_le_bytes());
}
