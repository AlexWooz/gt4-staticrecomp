//! Tourist Trophy (SCUS-97502): the embedded pins, and the real disc against them. The disc test is ignored by default (it
//! reads the contributor's image). Run with:
//!   set GT4_TT_ISO=<path to Tourist Trophy (USA).iso>
//!   cargo test -p gt4l-disc --test tt_disc -- --ignored --nocapture
//! The image is only read; nothing is written next to it.

use std::path::PathBuf;

use gt4l_disc::elf::sha256_hex;
use gt4l_disc::identify::{identify, DiscGame};
use gt4l_disc::{tt, Pins, Spec2Pins, TtPins};

#[test]
fn tt_pins_are_self_consistent() {
    let pins = TtPins::embedded().unwrap();
    assert_eq!(pins.serial, "SCUS-97502");
    assert_eq!(pins.derived.sha256.len(), 64);
    assert_eq!(pins.core.sha256.len(), 64);
    assert!(pins.code_files.len() >= 4, "loader, SYSTEM.CNF, IOPRP300.IMG and the IRX modules");
}

#[test]
#[ignore = "needs the contributor's Tourist Trophy image; see the module comment"]
fn real_tt_disc_is_identified_and_derives_the_pinned_elf() {
    let path = PathBuf::from(std::env::var("GT4_TT_ISO").expect("set GT4_TT_ISO to the ISO path"));
    let identified = identify(&path, &Pins::embedded().unwrap(), &Spec2Pins::embedded().unwrap()).expect("identify");
    assert_eq!(identified.game, DiscGame::Tt);
    assert_eq!(identified.summary.serial, "SCUS-97502");
    let pins = TtPins::embedded().unwrap();
    let (core, derived) = tt::extract_and_derive(&path, &pins).expect("CORE.TT derives the pinned ELF");
    assert_eq!(sha256_hex(&core), pins.core.sha256);
    assert_eq!(sha256_hex(&derived), pins.derived.sha256);
    println!("TT: CORE.TT {} bytes -> ELF {} bytes, sha256 {}", core.len(), derived.len(), sha256_hex(&derived));
}
