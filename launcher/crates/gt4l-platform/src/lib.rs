//! Windows and process code shared by the launcher UI: gamepad input (SDL3), deletes to the
//! Recycle Bin, the Documents known folder, the connected monitors and their modes, one launcher at a time, the kill-on-close job, and starting and watching the game.
//! Nothing here knows about pages, windows or the web view.

pub mod dialog;
pub mod disk;
pub mod display;
pub mod game;
pub mod gpu;
pub mod install_record;
pub mod known_folder;
pub mod legacy_install;
pub mod pad;
pub mod process;
pub mod recycle;
pub mod sdl_pads;
pub mod system;
pub mod webview2;
