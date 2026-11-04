//! GT4Recomp setup core: identity checks of the disc image, `SYSTEM.CNF`, full SHA-256 with progress,
//! `CORE.GT4` extraction, the ELF rebuild (a port of `scripts/ee_producer.py`) and the prepared folder.
//! No UI here; the launcher runs [`setup::run`] on a worker thread and shows its progress.

pub mod cnf;
pub mod elf;
pub mod identify;
pub mod iso;
pub mod pins;
pub mod setup;
pub mod spec2;
pub mod spec2_elf;
pub mod spec2_pins;
pub mod testing;

pub use identify::{DiscGame, Identified};
pub use pins::{Identity, Pins};
pub use spec2_pins::Spec2Pins;
pub use setup::{SetupError, SetupRequest, Stage};
