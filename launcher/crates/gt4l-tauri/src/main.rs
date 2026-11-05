#![cfg_attr(all(not(debug_assertions), windows), windows_subsystem = "windows")]

//! `GT4Recomp.exe`: the desktop launcher. Everything lives in the library (src/lib.rs), which is also the Android app.

fn main() {
    gt4l_tauri_lib::run();
}
