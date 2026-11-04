//! Which game a disc image is (TASK-101). The launcher recognises the game from the picked file and installs that game.
//!
//! - SCUS-97328: Gran Turismo 4 v2.00, by the pinned `CORE.GT4` and, for a disc that claims v2.00, the pinned image
//!   (`setup::precheck`).
//! - SCUS-97436: Gran Turismo 4 Spec II, by the pinned executable (`spec2::precheck`).
//! - Anything else is refused with a message that names what was found.

use std::path::Path;

use crate::pins::Pins;
use crate::spec2_pins::Spec2Pins;
use crate::setup::{self, DiscSummary, SetupError};
use crate::spec2;

/// The games the disc crate can set up. The ids are those of `gt4l_core::games`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiscGame {
    Gt4,
    Spec2,
}

impl DiscGame {
    /// `gt4` or `spec2`.
    pub fn id(self) -> &'static str {
        match self {
            DiscGame::Gt4 => "gt4",
            DiscGame::Spec2 => "spec2",
        }
    }

    pub fn from_id(id: &str) -> Option<DiscGame> {
        match id {
            "gt4" => Some(DiscGame::Gt4),
            "spec2" => Some(DiscGame::Spec2),
            _ => None,
        }
    }
}

/// A disc that passed the fast check.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identified {
    pub game: DiscGame,
    pub summary: DiscSummary,
}

/// Identifies the picked file. Fast: no full hash.
pub fn identify(disc: &Path, gt4: &Pins, spec2_pins: &Spec2Pins) -> Result<Identified, SetupError> {
    let (iso, config) = spec2::open_and_read_system(disc)?;
    if config.serial == spec2_pins.serial {
        return spec2::precheck_opened(iso, config, spec2_pins).map(|summary| Identified { game: DiscGame::Spec2, summary });
    }
    drop(iso);
    setup::precheck(disc, gt4).map(|summary| Identified { game: DiscGame::Gt4, summary })
}
