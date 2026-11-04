//! TASK-043 tests: the renderer download against a fake release folder (no network).
//!
//! The fake server is `FolderSource`: `releases.json` in GitHub's list format plus the asset files, served by
//! the URL prefix https://github.com/moesuito/gt4recomp-gs/releases/download/<tag>/<name>.

use std::fs;
use std::io::{Cursor, Write};
use std::path::{Path, PathBuf};

use gt4l_core::gs_release::{self, FolderSource, Outcome, Version};
use gt4l_core::releases::{self as releases, Stream};

const BASE: &str = "https://github.com/moesuito/gt4recomp-gs/releases/download";

/// A fresh folder under the system temp directory, unique per test.
fn scratch(name: &str) -> PathBuf {
    let directory = std::env::temp_dir().join(format!("gt4l-gs-test-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&directory);
    fs::create_dir_all(&directory).unwrap();
    directory
}

/// A zip package holding `gt4-gs.exe` with the given bytes.
fn package(executable: &[u8]) -> Vec<u8> {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer.start_file("gt4-gs.exe", zip::write::SimpleFileOptions::default()).unwrap();
    writer.write_all(executable).unwrap();
    writer.finish().unwrap().into_inner()
}

/// Adds one release to the fake server: its tag folder with the package and SHA256SUMS.
/// `sums_exe` is the hash written for gt4-gs.exe; `package_bytes` is what the server serves for the package.
struct FakeRelease<'a> {
    tag: &'a str,
    prerelease: bool,
    executable: &'a [u8],
    serve: &'a [u8],
}

fn write_release(root: &Path, release: &FakeRelease<'_>) -> String {
    let version = release.tag.trim_start_matches("gt4gs-v");
    let package_name = format!("gt4-gs-{version}-windows-x64.zip");
    let folder = root.join(release.tag);
    fs::create_dir_all(&folder).unwrap();
    let package_bytes = package(release.executable);
    fs::write(folder.join(&package_name), release.serve).unwrap();
    let sums = format!(
        "{}  {}\n{}  gt4-gs.exe\n",
        gs_release::sha256_hex(&package_bytes),
        package_name,
        gs_release::sha256_hex(release.executable),
    );
    fs::write(folder.join("SHA256SUMS"), sums).unwrap();
    format!(
        r#"{{"tag_name":"{}","prerelease":{},"draft":false,"assets":[{{"name":"{}","browser_download_url":"{}/{}/{}"}},{{"name":"SHA256SUMS","browser_download_url":"{}/{}/SHA256SUMS"}}]}}"#,
        release.tag, release.prerelease, package_name, BASE, release.tag, package_name, BASE, release.tag,
    )
}

fn write_list(root: &Path, entries: &[String]) {
    fs::write(root.join("releases.json"), format!("[{}]", entries.join(","))).unwrap();
}

fn source(root: &Path) -> FolderSource {
    FolderSource { root: root.to_path_buf(), repository: gs_release::RELEASE_REPOSITORY.to_string() }
}

#[test]
fn stable_release_is_verified_and_installed_with_its_hash_recorded() {
    let server = scratch("install-server");
    let install = scratch("install-root");
    let exe = b"fake gt4-gs 0.3.2".to_vec();
    let entry = write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &exe, serve: &package(&exe) });
    write_list(&server, &[entry]);

    let outcome = gs_release::install_latest(&source(&server), &install).expect("install");
    let Outcome::Installed(installed) = outcome else { panic!("expected a fresh install") };
    assert_eq!(installed.version, Version(1, 2, 1));
    assert_eq!(installed.executable, install.join("1.2.1").join("gt4-gs.exe"));
    assert_eq!(installed.executable_sha256, gs_release::sha256_hex(&exe));
    assert_eq!(fs::read(&installed.executable).unwrap(), exe);
    assert_eq!(gs_release::installed(&install), Some(installed.clone()));
    assert_eq!(fs::read_to_string(install.join("current.txt")).unwrap().trim(), "1.2.1");
    // TASK-155: gt4-gs keeps its settings beside it (no Documents lookup, which failed inside OneDrive).
    assert!(install.join("1.2.1").join(gs_release::PORTABLE_MARKER).is_file());

    // Asking again with the same release is an up-to-date result, not a second install.
    assert!(matches!(gs_release::install_latest(&source(&server), &install), Ok(Outcome::UpToDate(_))));
}

#[test]
fn prereleases_and_too_old_versions_are_ignored() {
    let server = scratch("prerelease-server");
    let install = scratch("prerelease-root");
    let stable = b"stable 0.3.2".to_vec();
    let newer_prerelease = b"prerelease 0.4.0".to_vec();
    let old = b"old 0.3.1".to_vec();
    let entries = vec![
        write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: true, executable: &newer_prerelease, serve: &package(&newer_prerelease) }),
        write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &stable, serve: &package(&stable) }),
        write_release(&server, &FakeRelease { tag: "gt4gs-v1.0.1", prerelease: false, executable: &old, serve: &package(&old) }),
    ];
    write_list(&server, &entries);

    let releases = gs_release::parse_releases(&fs::read_to_string(server.join("releases.json")).unwrap(), Stream::Renderer).unwrap();
    assert_eq!(releases::select_latest(&releases, gs_release::MINIMUM_VERSION).unwrap().tag, "gt4gs-v1.2.1", "1.2.1 is a prerelease");

    let Outcome::Installed(installed) = gs_release::install_latest(&source(&server), &install).unwrap() else {
        panic!("expected install")
    };
    assert_eq!(installed.version, Version(1, 2, 1));

    // Only a prerelease and an old release on the server: nothing to install, the error names the minimum.
    let only_old = scratch("only-old-server");
    write_list(&only_old, &[write_release(&only_old, &FakeRelease { tag: "gt4gs-v1.0.1", prerelease: false, executable: &old, serve: &package(&old) })]);
    let error = gs_release::install_latest(&source(&only_old), &install).unwrap_err();
    assert!(error.contains("1.2.1"), "{error}");
}

#[test]
fn upgrade_keeps_the_previous_version_and_removes_older_ones() {
    let server = scratch("upgrade-server");
    let install = scratch("upgrade-root");
    let first = b"exe 0.3.2".to_vec();
    let second = b"exe 0.3.3".to_vec();
    let third = b"exe 0.3.4".to_vec();

    let entry = |tag: &str, bytes: &[u8]| write_release(&server, &FakeRelease { tag, prerelease: false, executable: bytes, serve: &package(bytes) });
    write_list(&server, &[entry("gt4gs-v1.2.1", &first)]);
    gs_release::install_latest(&source(&server), &install).unwrap();

    write_list(&server, &[entry("gt4gs-v1.2.1", &first), entry("gt4gs-v1.2.2", &second)]);
    let Outcome::Installed(upgraded) = gs_release::install_latest(&source(&server), &install).unwrap() else {
        panic!("expected upgrade")
    };
    assert_eq!(upgraded.version, Version(1, 2, 2));
    assert!(install.join("1.2.1").is_dir(), "the previous version stays until the next upgrade");
    assert!(install.join("1.2.2").is_dir());

    write_list(&server, &[entry("gt4gs-v1.2.3", &third)]);
    gs_release::install_latest(&source(&server), &install).unwrap();
    assert!(install.join("1.2.3").is_dir());
    assert!(install.join("1.2.2").is_dir(), "the new previous version is kept");
    assert!(!install.join("1.2.1").exists(), "two versions back is removed");
}

#[test]
fn corrupt_download_is_refused_and_the_installed_version_is_kept() {
    let server = scratch("corrupt-server");
    let install = scratch("corrupt-root");
    let good = b"exe good".to_vec();
    write_list(&server, &[write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &good, serve: &package(&good) })]);
    gs_release::install_latest(&source(&server), &install).unwrap();

    // The next release is served with a damaged package: its sum no longer matches.
    let bad = b"exe bad".to_vec();
    let mut damaged = package(&bad);
    let last = damaged.len() - 1;
    damaged[last] ^= 0xFF;
    let next = write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.2", prerelease: false, executable: &bad, serve: &damaged });
    // The package was written from `damaged`, but the sums list the original package bytes.
    let original_sum = gs_release::sha256_hex(&package(&bad));
    let sums = format!("{original_sum}  gt4-gs-1.2.2-windows-x64.zip\n");
    fs::write(server.join("gt4gs-v1.2.2").join("SHA256SUMS"), sums).unwrap();
    write_list(&server, &[next]);

    let error = gs_release::install_latest(&source(&server), &install).unwrap_err();
    assert!(error.contains("SHA-256"), "{error}");
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 2, 1), "the old version still works");
    assert!(!install.join("1.2.2").exists());
    assert!(!install.join(".staging-0.3.3").exists(), "the staging folder is removed");
}

#[test]
fn package_without_the_program_is_refused_and_nothing_changes() {
    let server = scratch("noexe-server");
    let install = scratch("noexe-root");
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer.start_file("readme.txt", zip::write::SimpleFileOptions::default()).unwrap();
    writer.write_all(b"no program here").unwrap();
    let archive = writer.finish().unwrap().into_inner();
    write_list(&server, &[write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: b"", serve: &archive })]);
    assert!(gs_release::install_latest(&source(&server), &install).is_err());
    assert!(gs_release::installed(&install).is_none());
    assert!(!install.join("1.2.1").exists());
}

#[test]
fn a_damaged_installed_program_fails_the_check_so_the_launcher_falls_back() {
    let server = scratch("damaged-installed-server");
    let install = scratch("damaged-installed-root");
    let exe = b"exe installed".to_vec();
    write_list(&server, &[write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &exe, serve: &package(&exe) })]);
    gs_release::install_latest(&source(&server), &install).unwrap();
    fs::write(install.join("1.2.1").join("gt4-gs.exe"), b"changed").unwrap();
    assert!(gs_release::installed(&install).is_none());
}

#[test]
fn a_release_url_outside_the_fake_server_is_refused() {
    let server = scratch("url-server");
    let error = gs_release::ReleaseSource::download(&source(&server), "https://example.invalid/x").unwrap_err();
    assert!(error.contains("not a release URL"), "{error}");
}

#[test]
fn the_install_reports_its_steps_and_the_package_bytes() {
    use gt4l_core::gs_release::Step;
    let server = scratch("steps-server");
    let install = scratch("steps-root");
    let exe = b"fake gt4-gs for progress".to_vec();
    let served = package(&exe);
    let entry = write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &exe, serve: &served });
    write_list(&server, &[entry]);
    let mut steps = Vec::new();
    gs_release::install_latest_reporting(&source(&server), &install, &mut |step| steps.push(step)).expect("install");
    assert_eq!(steps.first(), Some(&Step::Listing));
    assert!(steps.contains(&Step::Downloading));
    let size = served.len() as u64;
    assert!(steps.contains(&Step::Transfer { done: size, total: Some(size) }), "{steps:?}");
    assert_eq!(steps.last(), Some(&Step::Installing));
}

/// The real gt4-gs packages (1.0.1 checked 2026-10-20) wrap their files in one folder, `gt4-gs-<version>-windows-x64/`,
/// with `resources/` beside the program. The install removes that folder so `gt4-gs.exe` is at the version root.
#[test]
fn a_package_wrapped_in_one_folder_installs_like_the_real_release() {
    let server = scratch("wrapped-server");
    let install = scratch("wrapped-root");
    let exe = b"fake gt4-gs inside a folder".to_vec();
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let options = zip::write::SimpleFileOptions::default();
    writer.add_directory("gt4-gs-1.2.1-windows-x64/", options).unwrap();
    writer.start_file("gt4-gs-1.2.1-windows-x64/gt4-gs.exe", options).unwrap();
    writer.write_all(&exe).unwrap();
    writer.add_directory("gt4-gs-1.2.1-windows-x64/resources/shaders/", options).unwrap();
    writer.start_file("gt4-gs-1.2.1-windows-x64/resources/GameIndex.yaml", options).unwrap();
    writer.write_all(b"index").unwrap();
    let wrapped = writer.finish().unwrap().into_inner();

    let folder = server.join("gt4gs-v1.2.1");
    fs::create_dir_all(&folder).unwrap();
    fs::write(folder.join("gt4-gs-1.2.1-windows-x64.zip"), &wrapped).unwrap();
    // Like the real SHA256SUMS: a line for the package only, none for gt4-gs.exe.
    fs::write(folder.join("SHA256SUMS"), format!("{}  gt4-gs-1.2.1-windows-x64.zip\n", gs_release::sha256_hex(&wrapped))).unwrap();
    let entry = format!(
        r#"{{"tag_name":"gt4gs-v1.2.1","prerelease":false,"draft":false,"assets":[{{"name":"gt4-gs-1.2.1-windows-x64.zip","browser_download_url":"{BASE}/gt4gs-v1.2.1/gt4-gs-1.2.1-windows-x64.zip"}},{{"name":"SHA256SUMS","browser_download_url":"{BASE}/gt4gs-v1.2.1/SHA256SUMS"}}]}}"#
    );
    write_list(&server, &[entry]);

    let Outcome::Installed(installed) = gs_release::install_latest(&source(&server), &install).expect("install") else {
        panic!("expected a fresh install");
    };
    assert_eq!(installed.executable, install.join("1.2.1").join("gt4-gs.exe"));
    assert_eq!(fs::read(&installed.executable).unwrap(), exe);
    assert!(install.join("1.2.1").join("resources").join("GameIndex.yaml").exists());
    assert!(!install.join("1.2.1").join("gt4-gs-1.2.1-windows-x64").exists());
}

/// Release 1.2.1 raised the minimum to gt4-gs 1.2.1 (the ultrawide fill needs the protocol 1.3 consumer). A launcher that
/// finds gt4-gs 1.0.1 installed (as left by release 1.0.0) must replace it at launch: `needs_install` is true, and the
/// install path of `check_on_launch` puts 1.2.1 in place and points `current.txt` at it.
#[test]
fn an_installed_1_0_1_is_below_the_minimum_and_is_replaced_by_1_2_0() {
    let server = scratch("below-minimum-server");
    let install = scratch("below-minimum-root");

    // The state release 1.0.0 leaves behind: gt4-gs 1.0.1 installed and checked.
    let old = b"gt4-gs 1.0.1 as installed by launcher 1.0.0".to_vec();
    fs::create_dir_all(install.join("1.0.1")).unwrap();
    fs::write(install.join("1.0.1").join("gt4-gs.exe"), &old).unwrap();
    fs::write(install.join("1.0.1").join("sha256.txt"), format!("{}\n", gs_release::sha256_hex(&old))).unwrap();
    fs::write(install.join("current.txt"), "1.0.1\n").unwrap();
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 0, 1), "1.0.1 passes the check");
    assert!(gs_release::needs_install(&install), "1.0.1 is below the 1.2.1 minimum");

    // The server as it is today: 1.2.1 is the newest stable release, 1.0.1 and 1.0.0 are older.
    let new = b"gt4-gs 1.2.1 with the protocol 1.3 consumer".to_vec();
    let entry = |tag: &str, bytes: &[u8]| write_release(&server, &FakeRelease { tag, prerelease: false, executable: bytes, serve: &package(bytes) });
    write_list(&server, &[entry("gt4gs-v1.2.1", &new), entry("gt4gs-v1.0.1", &old), entry("gt4gs-v1.0.0", b"1.0.0")]);

    let Outcome::Installed(installed) = gs_release::install_latest(&source(&server), &install).unwrap() else {
        panic!("expected 1.2.1 to be installed over 1.0.1")
    };
    assert_eq!(installed.version, Version(1, 2, 1));
    assert_eq!(fs::read(&installed.executable).unwrap(), new);
    assert_eq!(fs::read_to_string(install.join("current.txt")).unwrap().trim(), "1.2.1");
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 2, 1));
    assert!(!gs_release::needs_install(&install), "1.2.1 meets the minimum");
    assert!(install.join("1.0.1").is_dir(), "the previous version stays until the next upgrade");

    // Nothing installed at all also needs an install; a damaged 1.2.1 does too.
    assert!(gs_release::needs_install(&scratch("below-minimum-empty")));
    fs::write(install.join("1.2.1").join("gt4-gs.exe"), b"changed").unwrap();
    assert!(gs_release::needs_install(&install));
}

/// If the update to 1.2.1 cannot be made (offline, or no release at or above the minimum), the installed 1.0.1 stays
/// in place and keeps working; the next launch tries again.
#[test]
fn a_failed_update_from_1_0_1_keeps_the_old_version() {
    let server = scratch("below-minimum-offline-server");
    let install = scratch("below-minimum-offline-root");
    let old = b"gt4-gs 1.0.1 kept".to_vec();
    fs::create_dir_all(install.join("1.0.1")).unwrap();
    fs::write(install.join("1.0.1").join("gt4-gs.exe"), &old).unwrap();
    fs::write(install.join("1.0.1").join("sha256.txt"), format!("{}\n", gs_release::sha256_hex(&old))).unwrap();
    fs::write(install.join("current.txt"), "1.0.1\n").unwrap();
    write_list(&server, &[write_release(&server, &FakeRelease { tag: "gt4gs-v1.0.1", prerelease: false, executable: &old, serve: &package(&old) })]);

    let error = gs_release::install_latest(&source(&server), &install).unwrap_err();
    assert!(error.contains("1.2.1"), "{error}");
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 0, 1), "the old version still works");
    assert!(gs_release::needs_install(&install), "the next launch tries again");
}

#[test]
fn the_portable_marker_is_added_once_and_an_existing_one_is_kept() {
    let folder = scratch("portable-marker");
    fs::create_dir_all(&folder).unwrap();
    gs_release::ensure_portable_marker(&folder).unwrap();
    let marker = folder.join(gs_release::PORTABLE_MARKER);
    assert_eq!(fs::read(&marker).unwrap(), b"", "an install made before TASK-155 gets an empty marker");
    fs::write(&marker, b"kept").unwrap();
    gs_release::ensure_portable_marker(&folder).unwrap();
    assert_eq!(fs::read(&marker).unwrap(), b"kept", "an existing marker is left as it is");
    let _ = fs::remove_dir_all(&folder);
}

// TASK-158: installing a package the player downloaded by hand.

/// A server with one release 1.2.1 and a download folder holding `saved` under `file_name`.
fn archive_fixture(name: &str, file_name: &str, saved: &[u8]) -> (PathBuf, PathBuf, PathBuf) {
    let server = scratch(&format!("{name}-server"));
    let install = scratch(&format!("{name}-root"));
    let downloads = scratch(&format!("{name}-downloads"));
    let good = b"exe good".to_vec();
    write_list(&server, &[write_release(&server, &FakeRelease { tag: "gt4gs-v1.2.1", prerelease: false, executable: &good, serve: &package(&good) })]);
    let path = downloads.join(file_name);
    fs::write(&path, saved).unwrap();
    (server, install, path)
}

#[test]
fn a_good_archive_installs_like_the_download_and_current_points_at_it() {
    let good = b"exe good".to_vec();
    let (server, install, path) = archive_fixture("archive-good", "gt4-gs-1.2.1-windows-x64.zip", &package(&good));
    let mut steps = Vec::new();
    let outcome = gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |step| steps.push(step)).unwrap();
    let Outcome::Installed(installed) = outcome else { panic!("installed") };
    assert_eq!(installed.version, Version(1, 2, 1));
    assert_eq!(installed.executable_sha256, gs_release::sha256_hex(&good));
    assert_eq!(fs::read_to_string(install.join("current.txt")).unwrap().trim(), "1.2.1");
    assert!(install.join("1.2.1").join(gs_release::PORTABLE_MARKER).is_file());
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 2, 1));
    assert!(steps.contains(&gs_release::Step::Installing));
}

#[test]
fn a_tampered_archive_is_refused_and_the_previous_install_is_untouched() {
    let good = b"exe good".to_vec();
    let (server, install, path) = archive_fixture("archive-tampered", "gt4-gs-1.2.1-windows-x64.zip", &package(&good));
    gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |_| {}).unwrap();
    let before = fs::read(install.join("1.2.1").join("gt4-gs.exe")).unwrap();
    let mut tampered = package(b"exe evil");
    let last = tampered.len() - 1;
    tampered[last] ^= 0xFF;
    fs::write(&path, &tampered).unwrap();
    let problem = gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |_| {}).unwrap_err();
    assert!(matches!(problem, gs_release::ArchiveProblem::HashMismatch { .. }), "{problem:?}");
    assert_eq!(fs::read(install.join("1.2.1").join("gt4-gs.exe")).unwrap(), before);
    assert_eq!(gs_release::installed(&install).unwrap().version, Version(1, 2, 1));
    assert!(!install.join(".staging-1.2.1").exists());
}

#[test]
fn a_tampered_archive_on_a_fresh_pc_installs_nothing() {
    let (server, install, path) = archive_fixture("archive-fresh", "gt4-gs-1.2.1-windows-x64.zip", b"not the package");
    let problem = gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |_| {}).unwrap_err();
    assert!(problem.message(gt4l_core::language::Language::fallback()).contains("checksum"));
    assert!(gs_release::installed(&install).is_none());
    assert!(!install.join("1.2.1").exists());
}

#[test]
fn only_the_exact_package_file_name_is_accepted() {
    let good = b"exe good".to_vec();
    for wrong in [
        "gt4-gs.zip",
        "gt4-gs-1.2.1-windows-x64 (1).zip",
        "GT4-GS-1.2.1-windows-x64.zip",
        "gt4-gs-1.2-windows-x64.zip",
        "gt4-gs-01.2.1-windows-x64.zip",
        "gt4-gs-1.2.1-windows-x64.zip.zip",
    ] {
        let (server, install, path) = archive_fixture("archive-name", wrong, &package(&good));
        let problem = gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |_| {}).unwrap_err();
        assert!(matches!(problem, gs_release::ArchiveProblem::WrongName { .. }), "{wrong}: {problem:?}");
        assert!(gs_release::installed(&install).is_none(), "{wrong}");
    }
    assert_eq!(gs_release::parse_package_name("gt4-gs-1.2.1-windows-x64.zip"), Some(Version(1, 2, 1)));
    assert_eq!(gs_release::parse_package_name("gt4-gs-v1.2.1-windows-x64.zip"), None);
}

#[test]
fn a_version_below_the_minimum_is_refused() {
    let good = b"exe good".to_vec();
    let (server, install, path) = archive_fixture("archive-old", "gt4-gs-1.0.1-windows-x64.zip", &package(&good));
    let problem = gs_release::install_archive_with_release(&source(&server), &path, &install, &mut |_| {}).unwrap_err();
    assert!(matches!(problem, gs_release::ArchiveProblem::TooOld { version: Version(1, 0, 1) }), "{problem:?}");
    // Even with a matching sums line, the pure function refuses it.
    let sums = gs_release::parse_checksums(&format!("{}  gt4-gs-1.0.1-windows-x64.zip\n", gs_release::sha256_hex(&package(&good))));
    assert!(gs_release::install_from_archive(&path, &sums, &install).is_err());
    assert!(gs_release::installed(&install).is_none());
}

#[test]
fn an_oversized_file_is_refused_before_it_is_read() {
    let install = scratch("archive-big-root");
    let downloads = scratch("archive-big-downloads");
    let path = downloads.join("gt4-gs-1.2.1-windows-x64.zip");
    let file = fs::File::create(&path).unwrap();
    file.set_len(gs_release::MAX_PACKAGE_BYTES + 1).unwrap(); // sparse where supported: the check is on the length
    drop(file);
    let sums = gs_release::parse_checksums(&format!("{}  gt4-gs-1.2.1-windows-x64.zip\n", "a".repeat(64)));
    let problem = gs_release::install_from_archive_checked(&path, &sums, &install).unwrap_err();
    assert!(matches!(problem, gs_release::ArchiveProblem::TooLarge { .. }), "{problem:?}");
    assert!(!install.join("1.2.1").exists());
    let _ = fs::remove_file(&path);
}

#[test]
fn an_archive_without_the_program_is_refused_even_with_a_matching_sum() {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    writer.start_file("readme.txt", zip::write::SimpleFileOptions::default()).unwrap();
    writer.write_all(b"no program here").unwrap();
    let archive = writer.finish().unwrap().into_inner();
    let install = scratch("archive-noexe-root");
    let downloads = scratch("archive-noexe-downloads");
    let path = downloads.join("gt4-gs-1.2.1-windows-x64.zip");
    fs::write(&path, &archive).unwrap();
    let sums = gs_release::parse_checksums(&format!("{}  gt4-gs-1.2.1-windows-x64.zip\n", gs_release::sha256_hex(&archive)));
    let error = gs_release::install_from_archive(&path, &sums, &install).unwrap_err();
    assert!(error.contains("gt4-gs.exe"), "{error}");
    assert!(gs_release::installed(&install).is_none());
    assert!(!install.join("1.2.1").exists());
    assert!(!install.join(".staging-1.2.1").exists());
}

#[test]
fn an_archive_without_a_sums_line_is_never_installed() {
    let good = b"exe good".to_vec();
    let archive = package(&good);
    let install = scratch("archive-noline-root");
    let downloads = scratch("archive-noline-downloads");
    let path = downloads.join("gt4-gs-1.2.1-windows-x64.zip");
    fs::write(&path, &archive).unwrap();
    let empty = gs_release::parse_checksums("");
    let other = gs_release::parse_checksums(&format!("{}  gt4-gs-1.2.2-windows-x64.zip\n", gs_release::sha256_hex(&archive)));
    for sums in [&empty, &other] {
        let problem = gs_release::install_from_archive_checked(&path, sums, &install).unwrap_err();
        assert!(matches!(problem, gs_release::ArchiveProblem::NoEntry { .. }), "{problem:?}");
    }
    // The shared installer refuses it as well.
    assert!(gs_release::install_package(&archive, Version(1, 2, 1), &empty, &install, &mut |_| {}).is_err());
    assert!(gs_release::installed(&install).is_none());
}

#[test]
fn without_the_release_sums_the_archive_is_refused_with_a_clear_reason() {
    let good = b"exe good".to_vec();
    let downloads = scratch("archive-offline-downloads");
    let install = scratch("archive-offline-root");
    let path = downloads.join("gt4-gs-1.2.1-windows-x64.zip");
    fs::write(&path, package(&good)).unwrap();
    // A server with no release list at all (offline), and one whose release lacks SHA256SUMS.
    let empty_server = scratch("archive-offline-server");
    let problem = gs_release::install_archive_with_release(&source(&empty_server), &path, &install, &mut |_| {}).unwrap_err();
    assert!(matches!(problem, gs_release::ArchiveProblem::SumsUnavailable { .. }), "{problem:?}");
    let other_server = scratch("archive-nosums-server");
    write_list(&other_server, &[r#"{"tag_name":"gt4gs-v1.2.1","prerelease":false,"draft":false,"assets":[]}"#.to_string()]);
    let problem = gs_release::install_archive_with_release(&source(&other_server), &path, &install, &mut |_| {}).unwrap_err();
    assert!(matches!(problem, gs_release::ArchiveProblem::SumsUnavailable { .. }), "{problem:?}");
    assert!(problem.message(gt4l_core::language::Language::fallback()).contains("not installed"));
    assert!(gs_release::installed(&install).is_none());
}
