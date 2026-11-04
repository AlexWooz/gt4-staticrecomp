//! Release-list fixtures for both repositories (no network). `gt4gs-list.json` uses the real tag and asset names of
//! `moesuito/gt4recomp-gs` (checked with `gh release view gt4gs-v1.0.1 --json assets` and, for 1.1.0, on 2026-10-21). `gt4recomp-list.json` has
//! the shape GT4Recomp will publish; it is a fixture, not a copy of a real release.

use gt4l_core::releases::{self, Stream};
use gt4l_core::update::{self, Offer, UpdateChannel};
use gt4l_core::Version;

fn fixture(name: &str) -> String {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/releases").join(name);
    std::fs::read_to_string(path).unwrap()
}

#[test]
fn renderer_list_uses_the_real_asset_names_and_skips_drafts_and_prereleases() {
    let releases = releases::parse_releases(&fixture("gt4gs-list.json"), Stream::Renderer).unwrap();
    // The list is the real 1.1.0 release (2026-10-21). The launcher minimum is 1.2.1 (TASK-088), so the minimum is
    // passed here: the test checks the asset names and the draft and prerelease rules of the real list.
    let chosen = releases::select_latest(&releases, Version(1, 1, 0)).unwrap();
    assert_eq!(chosen.tag, "gt4gs-v1.1.0");
    assert_eq!(chosen.version, Some(Version(1, 1, 0)));
    let package = Stream::Renderer.package_name(Version(1, 1, 0));
    assert_eq!(package, "gt4-gs-1.1.0-windows-x64.zip");
    assert!(chosen.asset(&package).is_some(), "the package is listed under the real name");
    assert!(chosen.asset(releases::CHECKSUM_ASSET).is_some());
    assert!(chosen.notes.contains("**Notes**"), "the body is kept as the release notes");
}

#[test]
fn renderer_list_refuses_a_launcher_tag_and_an_old_stable() {
    let releases = releases::parse_releases(&fixture("gt4gs-list.json"), Stream::Renderer).unwrap();
    let launcher_tag = releases.iter().find(|release| release.tag == "v1.0.0").unwrap();
    assert_eq!(launcher_tag.version, None, "a `v` tag is not a renderer version");
    for tag in ["gt4gs-v1.0.0", "gt4gs-v1.0.1"] {
        let old = releases.iter().find(|release| release.tag == tag).unwrap();
        assert!(old.is_stable());
        assert!(old.version.unwrap() < Stream::Renderer.minimum(), "{tag} is below the 1.2.1 minimum");
    }
}

#[test]
fn launcher_list_offers_the_first_stable_release_and_never_the_draft() {
    let releases = releases::parse_releases(&fixture("gt4recomp-list.json"), Stream::Launcher).unwrap();
    let offer: Offer = update::decide(Stream::Launcher, UpdateChannel::Stable, None, &releases, None).unwrap();
    assert_eq!(offer.latest, Version(1, 0, 0).into(), "1.1.0 is a draft, 1.0.1-rc1 a prerelease");
    assert_eq!(offer.package, "GT4Recomp-Setup-1.0.0.exe");
    assert!(offer.notes.contains("First stable"));
}

#[test]
fn the_renderer_update_decision_uses_the_real_list() {
    let releases = releases::parse_releases(&fixture("gt4gs-list.json"), Stream::Renderer).unwrap();
    // Until gt4-gs 1.2.1 is published, the real list (newest stable 1.1.0) is below the minimum: nothing is offered.
    assert!(update::decide(Stream::Renderer, UpdateChannel::Stable, Some(Version(1, 1, 0).into()), &releases, None).is_none());
    assert!(update::decide(Stream::Renderer, UpdateChannel::Stable, Some(Version(1, 0, 1).into()), &releases, None).is_none());
    // Once 1.2.1 is listed (a fixture here, the real list has no 1.2.1 yet), an installed 1.0.1 is offered it.
    let mut listed = releases.clone();
    listed.push(releases::parse_releases(r#"[{"tag_name":"gt4gs-v1.2.1","prerelease":false,"draft":false,"assets":[]}]"#, Stream::Renderer).unwrap().remove(0));
    let offer = update::decide(Stream::Renderer, UpdateChannel::Stable, Some(Version(1, 0, 1).into()), &listed, None).unwrap();
    assert_eq!(offer.latest, Version(1, 2, 1).into());
    assert!(offer.is_update());
}

/// A fake GitHub answer read through the folder source: the Beta channel offers a candidate, the Stable channel does not,
/// and the picker list and the download of the chosen candidate work from the same JSON (TASK-109).
#[test]
fn a_fake_github_list_drives_the_channels_the_picker_and_the_candidate_download() {
    use gt4l_core::releases::{FolderSource, ReleaseSource};
    use gt4l_core::version_list;
    use gt4l_core::ReleaseVersion;

    let root = std::env::temp_dir().join(format!("gt4l-channel-source-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(root.join("v1.3.0-rc6")).unwrap();
    std::fs::write(root.join("v1.3.0-rc6").join("GT4Recomp-Setup-1.3.0-rc6.exe"), b"setup").unwrap();
    let base = "https://github.com/moesuito/gt4-staticrecomp/releases/download";
    let json = format!(
        r#"[
          {{"tag_name":"v1.3.0-rc6","prerelease":true,"draft":false,"published_at":"2026-10-25T09:00:00Z","body":"Six.",
            "assets":[{{"name":"GT4Recomp-Setup-1.3.0-rc6.exe","browser_download_url":"{base}/v1.3.0-rc6/GT4Recomp-Setup-1.3.0-rc6.exe"}}]}},
          {{"tag_name":"v1.3.0-rc5","prerelease":true,"draft":false,"published_at":"2026-10-23T09:00:00Z","assets":[]}},
          {{"tag_name":"v1.2.3","prerelease":false,"draft":false,"published_at":"2026-10-01T09:00:00Z","assets":[]}}
        ]"#
    );
    std::fs::write(root.join("releases.json"), json).unwrap();
    let source = FolderSource { root: root.clone(), repository: "moesuito/gt4-staticrecomp".into() };
    let releases = releases::parse_releases(&source.releases_json().unwrap(), Stream::Launcher).unwrap();
    let installed = ReleaseVersion::parse("1.3.0-rc5");

    let beta = update::decide(Stream::Launcher, UpdateChannel::Beta, installed, &releases, None).unwrap();
    assert_eq!(beta.latest.text(), "1.3.0-rc6");
    assert!(update::decide(Stream::Launcher, UpdateChannel::Stable, installed, &releases, None).is_none());

    let list = version_list::build_list(&releases, UpdateChannel::Beta, installed);
    assert_eq!(list.iter().map(|entry| entry.tag.as_str()).collect::<Vec<_>>(), ["v1.3.0-rc6", "v1.3.0-rc5"]);

    let (release, version) = version_list::find_installable(&releases, "v1.3.0-rc6").unwrap();
    let package = Stream::Launcher.package_name(version);
    assert_eq!(package, "GT4Recomp-Setup-1.3.0-rc6.exe");
    let asset = release.asset(&package).unwrap();
    assert_eq!(source.download(&asset.url).unwrap(), b"setup");
    let _ = std::fs::remove_dir_all(&root);
}
