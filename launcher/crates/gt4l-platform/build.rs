//! Build script: links SDL3 3.4.18 (the version the game ships) dynamically.
//!
//! `GT4_SDL3_ROOT` is the folder that holds `devel/SDL3-3.4.18/lib/x64/SDL3.lib` and `runtime/SDL3.dll`
//! (the official SDL3-devel-3.4.18-VC and SDL3-3.4.18-win32-x64 packages). When it is not set, the folder
//! `private/tooling/sdl3-3.4.18` of the main checkout is used. Git worktrees have no `private/`, so they must
//! set the variable. The DLL is copied next to the built binaries (`target/<profile>` and `deps`) so tests and
//! the launcher start; the launcher's installer ships it beside GT4Recomp.exe.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=GT4_SDL3_ROOT");
    println!("cargo:rerun-if-env-changed=GT4_SDL3_ANDROID_ROOT");
    // TASK-180: the Android app links the libSDL3.so of the official SDL3 3.4.18 Android package (the one the game
    // loads; the APK ships it). `GT4_SDL3_ANDROID_ROOT` is that package's folder (private/tooling/sdl3-3.4.18-android).
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("android") {
        let root = PathBuf::from(env::var("GT4_SDL3_ANDROID_ROOT").expect("GT4_SDL3_ANDROID_ROOT: the SDL3 3.4.18 Android package folder"));
        let abi = match env::var("CARGO_CFG_TARGET_ARCH").as_deref() {
            Ok("aarch64") => "arm64-v8a",
            Ok("x86_64") => "x86_64",
            other => panic!("no SDL3 Android library for {other:?}"),
        };
        let libs = root.join(format!("aar/prefab/modules/SDL3-shared/libs/android.{abi}"));
        assert!(libs.join("libSDL3.so").exists(), "libSDL3.so not found under {}", libs.display());
        println!("cargo:rustc-link-search=native={}", libs.display());
        return;
    }
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        return;
    }
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("cargo sets CARGO_MANIFEST_DIR"));
    let root = match env::var("GT4_SDL3_ROOT") {
        Ok(value) => PathBuf::from(value),
        Err(_) => manifest.join("../../../../private/tooling/sdl3-3.4.18"),
    };
    let import_library = root.join("devel/SDL3-3.4.18/lib/x64/SDL3.lib");
    let dll = root.join("runtime/SDL3.dll");
    if !import_library.exists() || !dll.exists() {
        panic!(
            "SDL3 3.4.18 not found under {}: set GT4_SDL3_ROOT to the folder that holds devel/ and runtime/",
            root.display()
        );
    }
    println!("cargo:rustc-link-search=native={}", parent_of(&import_library).display());
    println!("cargo:rerun-if-changed={}", dll.display());

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("cargo sets OUT_DIR"));
    // OUT_DIR is target/<profile>/build/<package>-<hash>/out
    if let Some(profile_dir) = out_dir.ancestors().nth(3) {
        for destination in [profile_dir.to_path_buf(), profile_dir.join("deps")] {
            if fs::create_dir_all(&destination).is_ok() {
                let _ = fs::copy(&dll, destination.join("SDL3.dll"));
            }
        }
    }
}

fn parent_of(path: &Path) -> &Path {
    path.parent().unwrap_or(path)
}
